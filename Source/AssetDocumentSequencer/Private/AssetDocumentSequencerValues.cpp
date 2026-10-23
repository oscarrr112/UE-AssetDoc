// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Values of the Sequencer document (design §3.3, §7.1): the Editor typed value codec with the movie-scene adapters
// (frame numbers in display frames, binding IDs as binding paths, time-warp play rates, events), sparse objects and
// the field filters shared by sections, tracks, custom bindings and spawnable templates.

#include "AssetDocumentSequencerPrimitives.h"

#include "AssetDocumentEditorPropertyValueCodec.h"
#include "Channels/MovieSceneChannel.h"
#include "Channels/MovieSceneEvent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "MovieSceneDynamicBinding.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieSceneSection.h"
#include "MovieSceneSequenceID.h"
#include "MovieSceneSignedObject.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Variants/MovieSceneTimeWarpGetter.h"
#include "Variants/MovieSceneTimeWarpVariant.h"
#include "Variants/MovieSceneTimeWarpVariantPayloads.h"

namespace UE::AssetDocument::Graph::Sequencer
{
namespace SequencerValuesLocal
{
enum class EAdapted : uint8
{
	None,
	Frame,
	BindingId,
	TimeWarp,
	Event,
	DynamicBinding,
};

EAdapted AdaptedStruct(const UScriptStruct* Struct)
{
	if (Struct == nullptr)
	{
		return EAdapted::None;
	}
	if (Struct == TBaseStructure<FFrameNumber>::Get())
	{
		return EAdapted::Frame;
	}
	if (Struct == FMovieSceneObjectBindingID::StaticStruct())
	{
		return EAdapted::BindingId;
	}
	if (Struct == FMovieSceneTimeWarpVariant::StaticStruct())
	{
		return EAdapted::TimeWarp;
	}
	if (Struct == FMovieSceneEvent::StaticStruct())
	{
		return EAdapted::Event;
	}
	if (Struct == FMovieSceneDynamicBinding::StaticStruct())
	{
		return EAdapted::DynamicBinding;
	}
	return EAdapted::None;
}

bool IsCoreStruct(const UScriptStruct& Struct)
{
	const UPackage* Package = Struct.GetOutermost();
	return Package != nullptr && Package->GetFName() == TEXT("/Script/CoreUObject");
}

// The struct holds an adapted value at some depth (it is then encoded member by member).
bool ContainsAdapted(const UScriptStruct* Struct, int32 Depth = 0)
{
	if (Struct == nullptr || Depth > 8)
	{
		return false;
	}
	if (AdaptedStruct(Struct) != EAdapted::None)
	{
		return true;
	}
	static TMap<const UScriptStruct*, bool> Cache;
	if (const bool* Found = Cache.Find(Struct))
	{
		return *Found;
	}
	bool bContains = false;
	for (TFieldIterator<FProperty> It(Struct); It && !bContains; ++It)
	{
		const FProperty* Property = *It;
		if (IsInstancedObjectProperty(*Property))
		{
			bContains = true;
			break;
		}
		if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
		{
			Property = Array->Inner;
		}
		if (const FStructProperty* Inner = CastField<FStructProperty>(Property))
		{
			bContains = ContainsAdapted(Inner->Struct, Depth + 1);
		}
	}
	Cache.Add(Struct, bContains);
	return bContains;
}

FValue Unsupported(const FProperty& Property, const void* ValuePtr, FEncodeContext& Context, const FString& Why)
{
	FString Text;
	Property.ExportTextItem_Direct(Text, ValuePtr, nullptr, nullptr, PPF_None);
	Context.Warnings.Add(TEXT("Sequencer.UnsupportedValue"), FString::Printf(TEXT("%s (%s) cannot be represented in a document (%s); "
		"it is shown as export text and kept as it is."), *Property.GetName(), *Property.GetCPPType(), *Why), Context.Path);
	FObjectBuilder Object;
	Object.AddString(TEXT("Unsupported"), Text);
	return Object.Build();
}

FValue EncodeCodec(const FProperty& Property, const void* ValuePtr, FEncodeContext& Context)
{
	FValue Value;
	FString Code;
	if (UE::AssetDocument::EditorPropertyValueCodec::Encode(const_cast<FProperty&>(Property), ValuePtr, Value, Code))
	{
		return Value;
	}
	return Unsupported(Property, ValuePtr, Context, Code);
}

// The frame-rate payload's packed 24-bit numerator / denominator (its constructors are not exported).
FFrameRate TimeWarpFrameRate(const FMovieSceneTimeWarpVariant& Variant)
{
	static const UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/MovieScene.MovieSceneTimeWarpFrameRate"));
	const FProperty* Numerator = Struct != nullptr ? Struct->FindPropertyByName(TEXT("FrameRateNumerator")) : nullptr;
	const FProperty* Denominator = Struct != nullptr ? Struct->FindPropertyByName(TEXT("FrameRateDenominator")) : nullptr;
	if (Numerator == nullptr || Denominator == nullptr)
	{
		return FFrameRate();
	}
	const FMovieSceneTimeWarpFrameRate Payload = Variant.AsFrameRate();
	auto Unpack = [&Payload](const FProperty* Property)
	{
		int32 Value = 0;
		FMemory::Memcpy(&Value, Property->ContainerPtrToValuePtr<uint8>(&Payload), 3);
		return ((Value & 0x00800000) << 8) | (Value & ~0x00800000);
	};
	return FFrameRate(Unpack(Numerator), Unpack(Denominator));
}

FValue EncodeTimeWarp(const FProperty& Property, const FMovieSceneTimeWarpVariant& Variant, FEncodeContext& Context)
{
	FObjectBuilder Object;
	switch (Variant.GetType())
	{
	case EMovieSceneTimeWarpType::FixedPlayRate:
		Object.Add(TEXT("PlayRate"), MakeNumber(Variant.AsFixedPlayRate()));
		break;
	case EMovieSceneTimeWarpType::FixedTime:
		Object.Add(TEXT("FixedFrame"), MakeFrames(Variant.AsFixedTime().FrameNumber.Value, Context.Time));
		break;
	case EMovieSceneTimeWarpType::FrameRate:
		Object.Add(TEXT("FrameRate"), MakeRate(TimeWarpFrameRate(Variant)));
		break;
	case EMovieSceneTimeWarpType::Loop:
		Object.Add(TEXT("Loop"), MakeFrames(Variant.AsLoop().Duration.Value, Context.Time));
		break;
	case EMovieSceneTimeWarpType::Clamp:
		Object.Add(TEXT("Clamp"), MakeFrames(Variant.AsClamp().Max.Value, Context.Time));
		break;
	case EMovieSceneTimeWarpType::LoopFloat:
		Object.Add(TEXT("LoopFloat"), MakeNumber(ShortestFloat(Variant.AsLoopFloat().Duration)));
		break;
	case EMovieSceneTimeWarpType::ClampFloat:
		Object.Add(TEXT("ClampFloat"), MakeNumber(ShortestFloat(Variant.AsClampFloat().Max)));
		break;
	case EMovieSceneTimeWarpType::Custom:
	{
		FEncodeContext Inner = Context;
		Inner.Path = Context.Path + TEXT("/Custom");
		Object.Add(TEXT("Custom"), EncodeSubobject(Variant.AsCustom(), Inner));
		break;
	}
	default:
	{
		FString Text;
		Property.ExportTextItem_Direct(Text, &Variant, nullptr, nullptr, PPF_None);
		Object.AddString(TEXT("Unsupported"), Text);
		break;
	}
	}
	return Object.Build();
}

FEncodeContext Child(FEncodeContext& Context, const FString& Key)
{
	FEncodeContext Inner = Context;
	Inner.Path = Context.Path + TEXT("/") + Escape(Key);
	return Inner;
}

int32 ResolveParentIndexOf(const FMovieSceneObjectBindingID& Id)
{
	static const FIntProperty* Property = FindFProperty<FIntProperty>(FMovieSceneObjectBindingID::StaticStruct(), TEXT("ResolveParentIndex"));
	return Property != nullptr ? Property->GetPropertyValue_InContainer(&Id) : 0;
}
} // namespace

bool IsInstancedObjectProperty(const FProperty& Property)
{
	const FObjectProperty* Object = CastField<FObjectProperty>(&Property);
	return Object != nullptr && Property.ArrayDim == 1 && Object->PropertyClass != nullptr && !Property.IsA<FClassProperty>()
		&& Property.HasAnyPropertyFlags(CPF_InstancedReference | CPF_PersistentInstance);
}

bool IsSubobjectField(const FProperty& Property)
{
	// UMovieSceneSignedObject::Signature is a change stamp regenerated on every edit and load, not content.
	return IsStoredField(Property) && !IsChannelField(Property) && Property.GetOwnerClass() != UMovieSceneSignedObject::StaticClass();
}

FValue EncodeSubobject(const UObject* Object, FEncodeContext& Context)
{
	if (Object == nullptr)
	{
		return MakeNull();
	}
	FObjectBuilder Result;
	Result.AddString(TEXT("Class"), Object->GetClass()->GetPathName());
	FEncodeContext Inner = Context;
	Inner.Path = Context.Path + TEXT("/Properties");
	FValue Fields = EncodeSparseObject(*Object->GetClass(), Object, Object->GetClass()->GetDefaultObject(), &IsSubobjectField, Inner);
	if (!Fields.ObjectKeys.IsEmpty())
	{
		Result.Add(TEXT("Properties"), MoveTemp(Fields));
	}
	return Result.Build();
}

bool ContainsAdaptedValue(const UScriptStruct* Struct)
{
	using namespace SequencerValuesLocal;
	return ContainsAdapted(Struct);
}

void FWarnings::Add(const TCHAR* Code, const FString& Message, const FString& Path)
{
	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("warning"));
	Record.AddString(TEXT("code"), Code);
	Record.AddString(TEXT("message"), Message);
	Record.AddString(TEXT("path"), Path);
	Records.Add(Record.Build());
}

FString FWarnings::EncoderPathOf(const FString& DocumentPath) const
{
	for (const TPair<FString, FString>& Alias : DefaultsAliases)
	{
		if (DocumentPath == Alias.Key || DocumentPath.StartsWith(Alias.Key + TEXT("/")))
		{
			return Alias.Value + DocumentPath.RightChop(Alias.Key.Len());
		}
	}
	return DocumentPath;
}

void FWarnings::AddDefault(const FString& Path, const FValue& Value)
{
	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("default"));
	Record.AddString(TEXT("path"), Path);
	Record.Add(TEXT("value"), Value);
	Records.Add(Record.Build());
}

bool IsStoredField(const FProperty& Property)
{
	return !Property.HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_SkipSerialization)
		&& CastField<FDelegateProperty>(&Property) == nullptr && CastField<FMulticastDelegateProperty>(&Property) == nullptr;
}

bool HoldsChannel(const FProperty& Property)
{
	static TMap<const UScriptStruct*, bool> Cache;
	TFunction<bool(const FProperty&, int32)> Holds = [&Holds](const FProperty& Field, int32 Depth) -> bool
	{
		if (Depth > 8)
		{
			return false;
		}
		if (const FArrayProperty* Array = CastField<FArrayProperty>(&Field))
		{
			return Holds(*Array->Inner, Depth + 1);
		}
		if (const FMapProperty* Map = CastField<FMapProperty>(&Field))
		{
			return Holds(*Map->ValueProp, Depth + 1);
		}
		const FStructProperty* Struct = CastField<FStructProperty>(&Field);
		if (Struct == nullptr || Struct->Struct == nullptr)
		{
			return false;
		}
		if (Struct->Struct->IsChildOf(FMovieSceneChannel::StaticStruct()))
		{
			return true;
		}
		if (const bool* Found = Cache.Find(Struct->Struct))
		{
			return *Found;
		}
		bool bHolds = false;
		for (TFieldIterator<FProperty> It(Struct->Struct); It && !bHolds; ++It)
		{
			bHolds = Holds(**It, Depth + 1);
		}
		Cache.Add(Struct->Struct, bHolds);
		return bHolds;
	};
	return Holds(Property, 0);
}

bool RefersToSections(const FProperty& Property)
{
	const FProperty* Field = &Property;
	if (const FArrayProperty* Array = CastField<FArrayProperty>(Field))
	{
		Field = Array->Inner;
	}
	const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Field);
	return Object != nullptr && Object->PropertyClass != nullptr && Object->PropertyClass->IsChildOf(UMovieSceneSection::StaticClass());
}

FValue EncodeBindingId(const FMovieSceneObjectBindingID& Id, FEncodeContext& Context)
{
	using namespace SequencerValuesLocal;
	if (!Id.IsValid())
	{
		return MakeNull();
	}
	const int32 ParentIndex = ResolveParentIndexOf(Id);
	const bool bRootSpace = Id.GetRelativeSequenceID() == MovieSceneSequenceID::Root;
	if (bRootSpace && (ParentIndex == 0 || ParentIndex == -1))
	{
		FObjectBuilder Object;
		const FBindingInfo* Binding = Context.Model.FindBinding(Id.GetGuid());
		Object.AddString(TEXT("Binding"), Binding != nullptr ? Binding->Path : TEXT("#") + Id.GetGuid().ToString(EGuidFormats::Digits));
		if (ParentIndex == -1)
		{
			// The pre-5.1 form (a binding of the root sequence, "fixed"); kept so the value round-trips.
			Object.AddBool(TEXT("Fixed"), true);
		}
		return Object.Build();
	}
	if (ParentIndex == 0)
	{
		FValue Remote;
		if (DescribeRemoteBinding(Context.Model, Id.GetRelativeSequenceID(), Id.GetGuid(), Remote))
		{
			return Remote;
		}
	}
	Context.Warnings.Add(TEXT("Sequencer.UnsupportedRemoteBinding"), TEXT("A binding reference that no sub / shot section of this sequence "
		"reaches (a parent sequence's binding, or a sub-sequence that is missing or nested deeper than 4) is shown raw ({Guid, SequenceID, "
		"ParentIndex}) and kept as it is."), Context.Path);
	FObjectBuilder Object;
	Object.AddString(TEXT("Guid"), Id.GetGuid().ToString(EGuidFormats::Digits));
	Object.AddInt(TEXT("SequenceID"), static_cast<int64>(Id.GetRelativeSequenceID().GetInternalValue()));
	Object.AddInt(TEXT("ParentIndex"), ParentIndex);
	return Object.Build();
}

FString EndpointName(const UObject* Endpoint)
{
	if (const UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Endpoint))
	{
		return Event->CustomFunctionName.ToString();
	}
	if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Endpoint))
	{
		const UEdGraph* Graph = Entry->GetGraph();
		return Graph != nullptr ? Graph->GetName() : Entry->FunctionReference.GetMemberName().ToString();
	}
	return Endpoint != nullptr ? Endpoint->GetName() : FString();
}

const UObject* LiveEndpoint(const TWeakObjectPtr<UObject>& Endpoint)
{
	const UEdGraphNode* Node = Cast<UEdGraphNode>(Endpoint.Get());
	const UEdGraph* Graph = Node != nullptr ? Node->GetGraph() : nullptr;
	if (Graph == nullptr || Graph->GetTypedOuter<UBlueprint>() == nullptr)
	{
		return nullptr;
	}
	return Graph->Nodes.ContainsByPredicate([Node](const TObjectPtr<UEdGraphNode>& Item) { return Item.Get() == Node; }) ? Node : nullptr;
}

FValue EncodeEvent(const FMovieSceneEvent& Event, FEncodeContext& Context)
{
	FObjectBuilder Object;
#if WITH_EDITORONLY_DATA
	// The bound pin and payload are inputs of the endpoint: without one they call nothing (a decoded key needs its
	// Endpoint for them).
	const UObject* Endpoint = LiveEndpoint(Event.WeakEndpoint);
	if (Endpoint == nullptr)
	{
		return Object.Build();
	}
	Object.AddString(TEXT("Endpoint"), EndpointName(Endpoint));
	if (!Event.BoundObjectPinName.IsNone())
	{
		Object.Add(TEXT("BoundObjectPin"), MakeName(Event.BoundObjectPinName));
	}
	if (!Event.PayloadVariables.IsEmpty())
	{
		TArray<FName> Pins;
		Event.PayloadVariables.GetKeys(Pins);
		Pins.Sort(FNameLexicalLess());
		FObjectBuilder Payload;
		for (const FName& Pin : Pins)
		{
			const FMovieSceneEventPayloadVariable& Variable = Event.PayloadVariables[Pin];
			Payload.AddString(Pin.ToString(), Variable.ObjectValue.IsValid() ? Variable.ObjectValue.ToString() : Variable.Value);
		}
		Object.Add(TEXT("Payload"), Payload.Build());
	}
#endif
	return Object.Build();
}

FValue EncodeValue(const FProperty& Property, const void* ValuePtr, FEncodeContext& Context)
{
	using namespace SequencerValuesLocal;
	if (IsInstancedObjectProperty(Property))
	{
		return EncodeSubobject(CastFieldChecked<FObjectProperty>(&Property)->GetObjectPropertyValue(ValuePtr), Context);
	}
	if (const FStructProperty* StructProperty = CastField<FStructProperty>(&Property); StructProperty != nullptr && Property.ArrayDim == 1)
	{
		const UScriptStruct* Struct = StructProperty->Struct;
		switch (AdaptedStruct(Struct))
		{
		case EAdapted::Frame:
			return MakeFrames(static_cast<const FFrameNumber*>(ValuePtr)->Value, Context.Time);
		case EAdapted::BindingId:
			return EncodeBindingId(*static_cast<const FMovieSceneObjectBindingID*>(ValuePtr), Context);
		case EAdapted::Event:
			return EncodeEvent(*static_cast<const FMovieSceneEvent*>(ValuePtr), Context);
		case EAdapted::TimeWarp:
			return EncodeTimeWarp(Property, *static_cast<const FMovieSceneTimeWarpVariant*>(ValuePtr), Context);
		case EAdapted::DynamicBinding:
			return EncodeDynamicBinding(*static_cast<const FMovieSceneDynamicBinding*>(ValuePtr), Context);
		case EAdapted::None:
			break;
		}
		if (Struct != nullptr && !IsCoreStruct(*Struct) && ContainsAdapted(Struct))
		{
			FObjectBuilder Object;
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				if (!IsStoredField(**It) || IsChannelField(**It))
				{
					continue;
				}
				FEncodeContext Inner = Child(Context, It->GetName());
				Object.Add(It->GetName(), EncodeValue(**It, It->ContainerPtrToValuePtr<void>(ValuePtr), Inner));
			}
			return Object.Build();
		}
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		const FStructProperty* Inner = CastField<FStructProperty>(Array->Inner);
		if (Inner != nullptr && ContainsAdapted(Inner->Struct))
		{
			FScriptArrayHelper Helper(Array, ValuePtr);
			TArray<FValue> Elements;
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				FEncodeContext Element = Child(Context, FString::FromInt(Index));
				Elements.Add(EncodeValue(*Array->Inner, Helper.GetRawPtr(Index), Element));
			}
			return MakeArray(MoveTemp(Elements));
		}
	}
	return EncodeCodec(Property, ValuePtr, Context);
}

namespace SequencerValuesLocal
{
// The value at a relative document path ("/a/0/b", escaped segments) inside Value; null when it has none.
const FValue* FindAt(const FValue& Value, const FString& Relative)
{
	TArray<FString> Segments;
	Relative.ParseIntoArray(Segments, TEXT("/"), false);
	const FValue* At = &Value;
	for (int32 Index = 1; Index < Segments.Num() && At != nullptr; ++Index)
	{
		const FString Segment = Segments[Index].Replace(TEXT("~1"), TEXT("/")).Replace(TEXT("~0"), TEXT("~"));
		const FValue* Next = nullptr;
		if (At->Kind == EAssetDocumentRuntimeValueKind::Object)
		{
			const int32 Key = At->ObjectKeys.IndexOfByKey(Segment);
			Next = Key != INDEX_NONE ? &At->Elements[Key] : nullptr;
		}
		else if (At->Kind == EAssetDocumentRuntimeValueKind::Array && !Segment.IsEmpty() && Segment.IsNumeric())
		{
			const int32 Element = FCString::Atoi(*Segment);
			Next = At->Elements.IsValidIndex(Element) ? &At->Elements[Element] : nullptr;
		}
		At = Next;
	}
	return At;
}

// A field EncodeSparseObject leaves out (at its default): the default value at each requested path at or under it.
void AddDefaultsAt(const FProperty& Property, const void* ValuePtr, const FEncodeContext& Inner)
{
	TArray<TPair<FString, FString>> Wanted;  // document path, encoder path
	for (const FString& Path : Inner.Warnings.DefaultsAt)
	{
		const FString Encoded = Inner.Warnings.EncoderPathOf(Path);
		if (Encoded == Inner.Path || Encoded.StartsWith(Inner.Path + TEXT("/")))
		{
			Wanted.Emplace(Path, Encoded);
		}
	}
	if (Wanted.IsEmpty())
	{
		return;
	}
	FWarnings Ignored;  // the default value's own warnings are not the read's
	FEncodeContext Quiet{Inner.Model, Inner.Time, Ignored, Inner.Path};
	const FValue Default = EncodeValue(Property, ValuePtr, Quiet);
	for (const TPair<FString, FString>& Path : Wanted)
	{
		if (const FValue* At = FindAt(Default, Path.Value.RightChop(Inner.Path.Len())))
		{
			Inner.Warnings.AddDefault(Path.Key, *At);
		}
	}
}
} // namespace

FValue EncodeSparseObject(const UStruct& Struct, const void* Value, const void* Defaults, TFunctionRef<bool(const FProperty&)> Include,
	FEncodeContext& Context)
{
	using namespace SequencerValuesLocal;
	FObjectBuilder Object;
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		const FProperty& Property = **It;
		if (!IsStoredField(Property) || !Include(Property))
		{
			continue;
		}
		const void* ValuePtr = Property.ContainerPtrToValuePtr<void>(Value);
		const void* DefaultPtr = Defaults != nullptr ? Property.ContainerPtrToValuePtr<void>(Defaults) : nullptr;
		if (DefaultPtr != nullptr && Property.Identical(ValuePtr, DefaultPtr, PPF_DeepComparison | PPF_DeepCompareInstances))
		{
			if (!Context.Warnings.DefaultsAt.IsEmpty())
			{
				AddDefaultsAt(Property, ValuePtr, Child(Context, Property.GetName()));
			}
			continue;
		}
		FEncodeContext Inner = Child(Context, Property.GetName());
		const FStructProperty* StructProperty = CastField<FStructProperty>(&Property);
		if (DefaultPtr != nullptr && StructProperty != nullptr && StructProperty->Struct != nullptr && Property.ArrayDim == 1
			&& !IsCoreStruct(*StructProperty->Struct) && AdaptedStruct(StructProperty->Struct) == EAdapted::None)
		{
			// Engine structs member by member: only the members that differ.
			FValue Members = EncodeSparseObject(*StructProperty->Struct, ValuePtr, DefaultPtr, [](const FProperty& Member) { return !IsChannelField(Member); },
				Inner);
			if (!Members.ObjectKeys.IsEmpty())
			{
				Object.Add(Property.GetName(), MoveTemp(Members));
			}
			continue;
		}
		Object.Add(Property.GetName(), EncodeValue(Property, ValuePtr, Inner));
	}
	return Object.Build();
}
} // namespace UE::AssetDocument::Graph::Sequencer
