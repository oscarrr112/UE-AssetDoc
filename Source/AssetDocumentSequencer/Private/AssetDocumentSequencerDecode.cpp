// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Values written by seq.edit.v1 (design §3.3): the inverse of AssetDocumentSequencerValues.cpp. Frame numbers are display
// frames of the context's time base (rounded to the nearest tick), binding IDs are binding paths ({"Binding": path},
// "Fixed" for a fixed ID, the raw {Guid, SequenceID, ParentIndex} form of a reference into another sequence), time-warp
// play rates {"PlayRate"}, engine structs member by member, everything else through the Editor typed value codec. A
// value read as {"Unsupported": <export text>} is kept as it is when the text is unchanged and imported from the text
// otherwise. An authored value equal to what the read gives for the current value is not written (so re-applying an
// extract writes nothing and instanced values the codec cannot rebuild stay as they are).

#include "AssetDocumentSequencerPrimitives.h"

#include "AssetDocumentEditorPropertyValueCodec.h"
#include "Channels/MovieSceneChannel.h"
#include "Channels/MovieSceneEvent.h"
#include "Misc/EngineVersionComparison.h"
#include "MovieSceneDynamicBinding.h"
#include "MovieSceneSection.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieSceneSequenceID.h"
#include "UObject/UnrealType.h"
#include "Variants/MovieSceneTimeWarpGetter.h"
#include "Variants/MovieSceneTimeWarpVariant.h"
#include "Variants/MovieSceneTimeWarpVariantPayloads.h"

namespace UE::AssetDocument::Graph::Sequencer
{
namespace SequencerDecodeLocal
{
FString Join(const FString& Path, const FString& Key)
{
	return Path.IsEmpty() ? Escape(Key) : Path + TEXT("/") + Escape(Key);
}

bool IsNumber(const FValue& Value)
{
	return Value.Kind == EAssetDocumentRuntimeValueKind::Integer || Value.Kind == EAssetDocumentRuntimeValueKind::Number;
}

double NumberOf(const FValue& Value)
{
	return Value.Kind == EAssetDocumentRuntimeValueKind::Integer ? static_cast<double>(Value.IntegerValue) : Value.NumberValue;
}

// {"Unsupported": text} (the read's form of a value the codec cannot represent).
const FValue* UnsupportedText(const FValue& Value)
{
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object || Value.ObjectKeys.Num() != 1 || Value.ObjectKeys[0] != TEXT("Unsupported"))
	{
		return nullptr;
	}
	return Value.Elements[0].Kind == EAssetDocumentRuntimeValueKind::String ? &Value.Elements[0] : nullptr;
}

bool DecodeUnsupported(const FProperty& Property, const FString& Text, void* ValuePtr, FDecodeContext& Context)
{
	FString Current;
	Property.ExportTextItem_Direct(Current, ValuePtr, nullptr, nullptr, PPF_None);
	if (Current == Text)
	{
		return true;
	}
	if (Property.ImportText_Direct(*Text, ValuePtr, Context.Owner, PPF_None) == nullptr)
	{
		return Context.Reader.Fail(TEXT("Sequencer.UnsupportedValue"), FString::Printf(TEXT("'%s' (%s) is kept as export text in documents and the "
			"text does not import."), *Property.GetName(), *Property.GetCPPType()), Context.Path);
	}
	return true;
}

bool DecodeCodec(const FProperty& Property, const FValue& Value, void* ValuePtr, FDecodeContext& Context)
{
	FString Code;
	if (!UE::AssetDocument::EditorPropertyValueCodec::Decode(const_cast<FProperty&>(Property), Value, ValuePtr, Code, Context.Owner))
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("The value of '%s' does not fit %s (%s)."), *Property.GetName(),
			*Property.GetCPPType(), *Code), Context.Path);
	}
	return true;
}

bool IsCoreStruct(const UScriptStruct& Struct)
{
	const UPackage* Package = Struct.GetOutermost();
	return Package != nullptr && Package->GetFName() == TEXT("/Script/CoreUObject");
}

FDecodeContext Child(const FDecodeContext& Context, const FString& Key)
{
	FDecodeContext Inner = Context;
	Inner.Path = Join(Context.Path, Key);
	return Inner;
}

bool IsInstanced(const FProperty& Property)
{
	return Property.HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference | CPF_PersistentInstance);
}

// Decoded from the current value rather than the default one: instanced objects (edited in place when the class is
// unchanged) and time warps (a custom time warp's object likewise).
bool StartsFromCurrent(const FProperty& Property)
{
	const FStructProperty* Struct = CastField<FStructProperty>(&Property);
	return IsInstanced(Property) || (Struct != nullptr && Struct->Struct == FMovieSceneTimeWarpVariant::StaticStruct());
}

// The default value of an instanced field is empty (null object, empty container): an omitted field takes it.
bool IsEmptyDefault(const FProperty& Property, const void* DefaultPtr)
{
	if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property))
	{
		return Property.ArrayDim == 1 && Object->GetObjectPropertyValue(DefaultPtr) == nullptr;
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		return FScriptArrayHelper(Array, DefaultPtr).Num() == 0;
	}
	return false;
}

void MarkProxyChanged(FDecodeContext& Context)
{
	if (Context.ProxyChanged != nullptr)
	{
		*Context.ProxyChanged = true;
	}
}

UClass* FindSubobjectClass(const FString& Name)
{
	if (Name.Contains(TEXT(".")))
	{
		return LoadObject<UClass>(nullptr, *Name, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	return FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst);
}

// An instanced object onto Slot: null clears it; an object of the slot's class is edited in place; another class is a
// new object owned by Context.Owner (as the Details panel's class picker creates it; time warps are initialized as the
// editor initializes them).
bool DecodeSubobject(const UClass& Base, const FValue& Value, UObject*& Slot, FDecodeContext& Context)
{
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		if (Slot != nullptr)
		{
			MarkProxyChanged(Context);
		}
		Slot = nullptr;
		return true;
	}
	const FValue* ClassName = Value.Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(Value, TEXT("Class")) : nullptr;
	if (ClassName == nullptr || ClassName->Kind != EAssetDocumentRuntimeValueKind::String)
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("An instanced %s is {\"Class\": <class path>, \"Properties\"?: "
			"{...}} or null."), *Base.GetName()), Context.Path);
	}
	for (const FString& Key : Value.ObjectKeys)
	{
		if (Key != TEXT("Class") && Key != TEXT("Properties"))
		{
			return Context.Reader.Fail(TEXT("Graph.UnknownField"), FString::Printf(TEXT("An instanced object takes Class and Properties; '%s' is not "
				"one of them."), *Key), Join(Context.Path, Key));
		}
	}
	UClass* Class = FindSubobjectClass(ClassName->StringValue);
	if (Class == nullptr || !Class->IsChildOf(&Base) || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		TArray<FString> Names;
		for (const UClass* Candidate : ConcreteSubclasses(Base))
		{
			Names.Add(Candidate->GetPathName());
		}
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("'%s' is not a %s class. Classes: %s."), *ClassName->StringValue,
			*Base.GetName(), Names.IsEmpty() ? TEXT("(none loaded)") : *FString::Join(Names, TEXT(", "))), Join(Context.Path, TEXT("Class")));
	}
	UObject* Target = Slot != nullptr && Slot->GetClass() == Class ? Slot : nullptr;
	if (Target == nullptr)
	{
		if (Context.Owner == nullptr)
		{
			return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("An instanced object needs an owner here."), Context.Path);
		}
		Target = NewObject<UObject>(Context.Owner, Class, NAME_None, RF_Transactional);
		if (UMovieSceneTimeWarpGetter* Getter = Cast<UMovieSceneTimeWarpGetter>(Target))
		{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
			Getter->InitializeDefaults(Cast<UMovieSceneSection>(Context.Owner));
#else
			Getter->InitializeDefaults();
#endif
		}
	}
	else
	{
		Target->Modify();
	}
	const FValue Empty = MakeEmptyObject();
	const FValue* Properties = FindField(Value, TEXT("Properties"));
	FDecodeContext Inner = Context;
	Inner.Owner = Target;
	Inner.Path = Join(Context.Path, TEXT("Properties"));
	if (!DecodeSparseObject(*Class, Properties != nullptr ? *Properties : Empty, Target, Class->GetDefaultObject(), &IsSubobjectField, Inner))
	{
		return false;
	}
	if (Target != Slot)
	{
		MarkProxyChanged(Context);
		Slot = Target;
	}
	return true;
}

bool FrameRateOf(const FValue& Value, FFrameRate& Out)
{
	const FValue* Numerator = Value.Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(Value, TEXT("Numerator")) : nullptr;
	const FValue* Denominator = Value.Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(Value, TEXT("Denominator")) : nullptr;
	if (Numerator == nullptr || Denominator == nullptr || Numerator->Kind != EAssetDocumentRuntimeValueKind::Integer
		|| Denominator->Kind != EAssetDocumentRuntimeValueKind::Integer || Value.ObjectKeys.Num() != 2)
	{
		return false;
	}
	// The payload packs each into 24 bits.
	if (Numerator->IntegerValue <= 0 || Numerator->IntegerValue >= 0x800000 || Denominator->IntegerValue <= 0 || Denominator->IntegerValue >= 0x800000)
	{
		return false;
	}
	Out = FFrameRate(static_cast<int32>(Numerator->IntegerValue), static_cast<int32>(Denominator->IntegerValue));
	return true;
}

bool DecodeTimeWarp(const FValue& Value, FMovieSceneTimeWarpVariant& Variant, FDecodeContext& Context)
{
	static const TCHAR* Forms = TEXT("A time warp is one of {\"PlayRate\": x}, {\"FixedFrame\": frames}, {\"FrameRate\": {Numerator, Denominator}}, "
		"{\"Loop\": frames}, {\"Clamp\": frames}, {\"LoopFloat\": x}, {\"ClampFloat\": x}, {\"Custom\": {\"Class\": <time warp getter class>, "
		"\"Properties\"?: {...}}} (a custom time warp's curve is a channel of the section).");
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object || Value.ObjectKeys.Num() != 1)
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), Forms, Context.Path);
	}
	const FString& Key = Value.ObjectKeys[0];
	const FValue& Field = Value.Elements[0];
	FDecodeContext Inner = Child(Context, Key);
	UObject* Before = Variant.GetType() == EMovieSceneTimeWarpType::Custom ? Variant.AsCustom() : nullptr;
	if (Key == TEXT("PlayRate") || Key == TEXT("LoopFloat") || Key == TEXT("ClampFloat"))
	{
		if (!IsNumber(Field))
		{
			return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("%s is a number."), *Key), Inner.Path);
		}
		if (Key == TEXT("PlayRate"))
		{
			Variant.Set(NumberOf(Field));
		}
		else if (Key == TEXT("LoopFloat"))
		{
			FMovieSceneTimeWarpLoopFloat Payload;
			Payload.Duration = static_cast<float>(NumberOf(Field));
			Variant.Set(Payload);
		}
		else
		{
			FMovieSceneTimeWarpClampFloat Payload;
			Payload.Max = static_cast<float>(NumberOf(Field));
			Variant.Set(Payload);
		}
	}
	else if (Key == TEXT("FixedFrame") || Key == TEXT("Loop") || Key == TEXT("Clamp"))
	{
		int32 Ticks = 0;
		if (!DecodeFrames(Field, Inner, Ticks))
		{
			return false;
		}
		if (Key == TEXT("FixedFrame"))
		{
			FMovieSceneTimeWarpFixedFrame Payload;
			Payload.FrameNumber = FFrameNumber(Ticks);
			Variant.Set(Payload);
		}
		else if (Key == TEXT("Loop"))
		{
			FMovieSceneTimeWarpLoop Payload;
			Payload.Duration = FFrameNumber(Ticks);
			Variant.Set(Payload);
		}
		else
		{
			FMovieSceneTimeWarpClamp Payload;
			Payload.Max = FFrameNumber(Ticks);
			Variant.Set(Payload);
		}
	}
	else if (Key == TEXT("FrameRate"))
	{
		FFrameRate Rate;
		static const UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/MovieScene.MovieSceneTimeWarpFrameRate"));
		const FProperty* Numerator = Struct != nullptr ? Struct->FindPropertyByName(TEXT("FrameRateNumerator")) : nullptr;
		const FProperty* Denominator = Struct != nullptr ? Struct->FindPropertyByName(TEXT("FrameRateDenominator")) : nullptr;
		if (!FrameRateOf(Field, Rate) || Numerator == nullptr || Denominator == nullptr)
		{
			return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("FrameRate is {Numerator, Denominator}, positive integers below 8388608."),
				Inner.Path);
		}
		// The payload's constructors are not exported: its packed fields are written through reflection.
		uint8* Memory = static_cast<uint8*>(FMemory::Malloc(FMath::Max(Struct->GetStructureSize(), 1), Struct->GetMinAlignment()));
		Struct->InitializeStruct(Memory);
		FMemory::Memcpy(Numerator->ContainerPtrToValuePtr<uint8>(Memory), &Rate.Numerator, 3);
		FMemory::Memcpy(Denominator->ContainerPtrToValuePtr<uint8>(Memory), &Rate.Denominator, 3);
		Variant.Set(*reinterpret_cast<const FMovieSceneTimeWarpFrameRate*>(Memory));
		Struct->DestroyStruct(Memory);
		FMemory::Free(Memory);
	}
	else if (Key == TEXT("Custom"))
	{
		UObject* Getter = Before;
		if (Field.Kind == EAssetDocumentRuntimeValueKind::Null)
		{
			return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("Custom is a time warp getter object (a fixed time warp is {\"PlayRate\": x})."),
				Inner.Path);
		}
		if (!DecodeSubobject(*UMovieSceneTimeWarpGetter::StaticClass(), Field, Getter, Inner))
		{
			return false;
		}
		Variant.Set(CastChecked<UMovieSceneTimeWarpGetter>(Getter));
	}
	else
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), Forms, Context.Path);
	}
	UObject* After = Variant.GetType() == EMovieSceneTimeWarpType::Custom ? Variant.AsCustom() : nullptr;
	if (Before != After)
	{
		MarkProxyChanged(Context);
	}
	return true;
}

// The read's encoding of the current value equals the authored one.
bool SameAsCurrent(const FProperty& Property, const FValue& Authored, const void* ValuePtr, const FDecodeContext& Context)
{
	FWarnings Ignored;
	FEncodeContext Encode{Context.Model, Context.Time, Ignored, Context.Path};
	return SameValue(EncodeValue(Property, ValuePtr, Encode), Authored);
}
} // namespace

int64 TicksOf(double Frames, const FTimeBase& Time)
{
	const double Ticks = Frames * Time.Tick.AsDecimal() / Time.Display.AsDecimal();
	return static_cast<int64>(FMath::RoundToDouble(Ticks));
}

bool SameValue(const FValue& A, const FValue& B)
{
	using namespace SequencerDecodeLocal;
	if (IsNumber(A) && IsNumber(B))
	{
		if (A.Kind == EAssetDocumentRuntimeValueKind::Integer && B.Kind == EAssetDocumentRuntimeValueKind::Integer)
		{
			return A.IntegerValue == B.IntegerValue;
		}
		const double X = NumberOf(A);
		const double Y = NumberOf(B);
		return X == Y || static_cast<float>(X) == static_cast<float>(Y);
	}
	if (A.Kind != B.Kind)
	{
		return false;
	}
	switch (A.Kind)
	{
	case EAssetDocumentRuntimeValueKind::Null:
		return true;
	case EAssetDocumentRuntimeValueKind::Boolean:
		return A.BooleanValue == B.BooleanValue;
	case EAssetDocumentRuntimeValueKind::String:
	case EAssetDocumentRuntimeValueKind::Bytes:
		return A.StringValue == B.StringValue;
	case EAssetDocumentRuntimeValueKind::Array:
		if (A.Elements.Num() != B.Elements.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Elements.Num(); ++Index)
		{
			if (!SameValue(A.Elements[Index], B.Elements[Index]))
			{
				return false;
			}
		}
		return true;
	case EAssetDocumentRuntimeValueKind::Object:
		if (A.ObjectKeys.Num() != B.ObjectKeys.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.ObjectKeys.Num(); ++Index)
		{
			const FValue* Other = FindField(B, A.ObjectKeys[Index]);
			if (Other == nullptr || !SameValue(A.Elements[Index], *Other))
			{
				return false;
			}
		}
		return true;
	default:
		return false;
	}
}

bool DecodeFrames(const FValue& Value, FDecodeContext& Context, int32& OutTicks)
{
	using namespace SequencerDecodeLocal;
	if (!IsNumber(Value))
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("A time is a number of display frames."), Context.Path);
	}
	const int64 Ticks = TicksOf(NumberOf(Value), Context.Time);
	if (Ticks < MIN_int32 || Ticks > MAX_int32)
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("%g frames is out of range at this tick resolution."),
			NumberOf(Value)), Context.Path);
	}
	OutTicks = static_cast<int32>(Ticks);
	return true;
}

bool ResolveBindingPath(const FSequenceModel& Model, const FString& Path, FGuid& OutGuid)
{
	for (const TPair<FGuid, FBindingInfo>& Entry : Model.Bindings)
	{
		if (Entry.Value.Path == Path)
		{
			OutGuid = Entry.Key;
			return true;
		}
	}
	// A "#<Id>" segment names the binding by its GUID wherever it is.
	FString Last = Path;
	int32 Slash = INDEX_NONE;
	if (Path.FindLastChar(TEXT('/'), Slash))
	{
		Last = Path.Mid(Slash + 1);
	}
	FGuid Guid;
	if (Last.StartsWith(TEXT("#")) && FGuid::Parse(Last.Mid(1), Guid) && Model.Bindings.Contains(Guid))
	{
		OutGuid = Guid;
		return true;
	}
	return false;
}

bool DecodeBindingId(const FValue& Value, FMovieSceneObjectBindingID& Out, FDecodeContext& Context)
{
	using namespace SequencerDecodeLocal;
	if (Value.Kind == EAssetDocumentRuntimeValueKind::Null)
	{
		Out = FMovieSceneObjectBindingID();
		return true;
	}
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("A binding reference is {\"Binding\": \"<binding path>\"} or null."), Context.Path);
	}
	if (const FValue* Raw = FindField(Value, TEXT("Guid")))
	{
		// The raw form of a reference into another sequence (kept as read).
		const FValue* Sequence = FindField(Value, TEXT("SequenceID"));
		const FValue* Parent = FindField(Value, TEXT("ParentIndex"));
		FGuid Guid;
		if (Raw->Kind != EAssetDocumentRuntimeValueKind::String || !FGuid::Parse(Raw->StringValue, Guid) || Sequence == nullptr || !IsNumber(*Sequence)
			|| Parent == nullptr || !IsNumber(*Parent))
		{
			return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("A raw binding reference is {Guid, SequenceID, ParentIndex}."), Context.Path);
		}
		const FMovieSceneSequenceID SequenceID(static_cast<uint32>(NumberOf(*Sequence)));
		const int32 ParentIndex = static_cast<int32>(NumberOf(*Parent));
		if (ParentIndex < 0)
		{
			Out = UE::MovieScene::FFixedObjectBindingID(Guid, SequenceID);
		}
		else
		{
			Out = UE::MovieScene::FRelativeObjectBindingID(Guid, SequenceID, ParentIndex);
		}
		return true;
	}
	const FValue* Path = FindField(Value, TEXT("Binding"));
	if (Path == nullptr || Path->Kind != EAssetDocumentRuntimeValueKind::String)
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("A binding reference is {\"Binding\": \"<binding path>\"}."), Context.Path);
	}
	const bool bRemote = FindField(Value, TEXT("Sequence")) != nullptr;
	for (const FString& Key : Value.ObjectKeys)
	{
		if (Key != TEXT("Binding") && Key != (bRemote ? TEXT("Sequence") : TEXT("Fixed")))
		{
			return Context.Reader.Fail(TEXT("Graph.UnknownField"), FString::Printf(TEXT("A binding reference takes Binding and Fixed, or Binding and "
				"Sequence (a binding of a sub-sequence); '%s' is not one of them."), *Key), Join(Context.Path, Key));
		}
	}
	if (bRemote)
	{
		return DecodeRemoteBinding(Value, Out, Context);
	}
	FGuid Guid;
	if (!ResolveBindingPath(Context.Model, Path->StringValue, Guid))
	{
		return Context.Reader.Fail(TEXT("Sequencer.UnknownBinding"), FString::Printf(TEXT("The sequence has no binding '%s' (binding paths are names from "
			"the root joined by '/', or #<Id>)."), *Path->StringValue), Join(Context.Path, TEXT("Binding")));
	}
	const FValue* Fixed = FindField(Value, TEXT("Fixed"));
	if (Fixed != nullptr && Fixed->Kind == EAssetDocumentRuntimeValueKind::Boolean && Fixed->BooleanValue)
	{
		Out = UE::MovieScene::FFixedObjectBindingID(Guid, MovieSceneSequenceID::Root);
	}
	else
	{
		Out = UE::MovieScene::FRelativeObjectBindingID(Guid);
	}
	return true;
}

bool IsMemberwiseStruct(const FProperty& Property)
{
	using namespace SequencerDecodeLocal;
	const FStructProperty* Struct = CastField<FStructProperty>(&Property);
	return Struct != nullptr && Struct->Struct != nullptr && Property.ArrayDim == 1 && !IsCoreStruct(*Struct->Struct)
		&& Struct->Struct != TBaseStructure<FFrameNumber>::Get() && Struct->Struct != FMovieSceneObjectBindingID::StaticStruct()
		&& Struct->Struct != FMovieSceneTimeWarpVariant::StaticStruct() && Struct->Struct != FMovieSceneEvent::StaticStruct()
		&& Struct->Struct != FMovieSceneDynamicBinding::StaticStruct() && !Struct->Struct->IsChildOf(FMovieSceneChannel::StaticStruct());
}

bool IsChannelField(const FProperty& Property)
{
	const FProperty* Field = &Property;
	if (const FArrayProperty* Array = CastField<FArrayProperty>(Field))
	{
		Field = Array->Inner;
	}
	else if (const FMapProperty* Map = CastField<FMapProperty>(Field))
	{
		Field = Map->ValueProp;
	}
	const FStructProperty* Struct = CastField<FStructProperty>(Field);
	if (Struct != nullptr && Struct->Struct != nullptr && Struct->Struct->IsChildOf(FMovieSceneChannel::StaticStruct()))
	{
		return true;
	}
	// A container of structs holding channels (parameter curves, DMX patch channels) or a fixed array of such structs:
	// read through the channel proxy. A single struct holding channels is read member by member without them.
	return (Field != &Property || Property.ArrayDim > 1) && HoldsChannel(Property);
}

bool DecodeValue(const FProperty& Property, const FValue& Value, void* ValuePtr, FDecodeContext& Context)
{
	using namespace SequencerDecodeLocal;
	if (const FValue* Text = UnsupportedText(Value))
	{
		return DecodeUnsupported(Property, Text->StringValue, ValuePtr, Context);
	}
	if (IsInstancedObjectProperty(Property))
	{
		const FObjectProperty& Object = *CastFieldChecked<FObjectProperty>(&Property);
		UObject* Slot = Object.GetObjectPropertyValue(ValuePtr);
		if (!DecodeSubobject(*Object.PropertyClass, Value, Slot, Context))
		{
			return false;
		}
		Object.SetObjectPropertyValue(ValuePtr, Slot);
		return true;
	}
	if (const FStructProperty* StructProperty = CastField<FStructProperty>(&Property); StructProperty != nullptr && Property.ArrayDim == 1)
	{
		const UScriptStruct* Struct = StructProperty->Struct;
		if (Struct == TBaseStructure<FFrameNumber>::Get())
		{
			int32 Ticks = 0;
			if (!DecodeFrames(Value, Context, Ticks))
			{
				return false;
			}
			static_cast<FFrameNumber*>(ValuePtr)->Value = Ticks;
			return true;
		}
		if (Struct == FMovieSceneObjectBindingID::StaticStruct())
		{
			return DecodeBindingId(Value, *static_cast<FMovieSceneObjectBindingID*>(ValuePtr), Context);
		}
		if (Struct == FMovieSceneTimeWarpVariant::StaticStruct())
		{
			return DecodeTimeWarp(Value, *static_cast<FMovieSceneTimeWarpVariant*>(ValuePtr), Context);
		}
		if (Struct == FMovieSceneEvent::StaticStruct())
		{
			return DecodeEvent(Value, *static_cast<FMovieSceneEvent*>(ValuePtr), Context);
		}
		if (Struct == FMovieSceneDynamicBinding::StaticStruct())
		{
			return DecodeDynamicBinding(Value, *static_cast<FMovieSceneDynamicBinding*>(ValuePtr), Context);
		}
		if (Struct != nullptr && !IsCoreStruct(*Struct) && ContainsAdaptedValue(Struct))
		{
			if (Value.Kind != EAssetDocumentRuntimeValueKind::Object)
			{
				return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("'%s' is an object of %s fields."), *Property.GetName(),
					*Struct->GetName()), Context.Path);
			}
			for (int32 Index = 0; Index < Value.ObjectKeys.Num(); ++Index)
			{
				const FProperty* Member = Struct->FindPropertyByName(FName(*Value.ObjectKeys[Index]));
				if (Member == nullptr || !IsStoredField(*Member) || IsChannelField(*Member))
				{
					return Context.Reader.Fail(TEXT("Graph.UnknownField"), FString::Printf(TEXT("%s has no writable field '%s'."), *Struct->GetName(),
						*Value.ObjectKeys[Index]), Join(Context.Path, Value.ObjectKeys[Index]));
				}
				FDecodeContext Inner = Child(Context, Value.ObjectKeys[Index]);
				if (!DecodeValue(*Member, Value.Elements[Index], Member->ContainerPtrToValuePtr<void>(ValuePtr), Inner))
				{
					return false;
				}
			}
			return true;
		}
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		const FStructProperty* Inner = CastField<FStructProperty>(Array->Inner);
		if (Inner != nullptr && ContainsAdaptedValue(Inner->Struct))
		{
			if (Value.Kind != EAssetDocumentRuntimeValueKind::Array)
			{
				return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("'%s' is an array."), *Property.GetName()), Context.Path);
			}
			FScriptArrayHelper Helper(Array, ValuePtr);
			Helper.EmptyAndAddValues(Value.Elements.Num());
			for (int32 Index = 0; Index < Value.Elements.Num(); ++Index)
			{
				FDecodeContext Element = Child(Context, FString::FromInt(Index));
				if (!DecodeValue(*Array->Inner, Value.Elements[Index], Helper.GetRawPtr(Index), Element))
				{
					return false;
				}
			}
			return true;
		}
	}
	return DecodeCodec(Property, Value, ValuePtr, Context);
}

bool DecodeSparseObject(const UStruct& Struct, const FValue& Object, void* Memory, const void* Defaults, TFunctionRef<bool(const FProperty&)> Include,
	FDecodeContext& Context)
{
	using namespace SequencerDecodeLocal;
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("An object of %s fields is expected."), *Struct.GetName()),
			Context.Path);
	}
	TMap<FName, const FValue*> Listed;
	for (int32 Index = 0; Index < Object.ObjectKeys.Num(); ++Index)
	{
		const FString& Name = Object.ObjectKeys[Index];
		const FProperty* Property = Struct.FindPropertyByName(FName(*Name));
		if (Property == nullptr || !IsStoredField(*Property) || !Include(*Property))
		{
			TArray<FString> Editable;
			for (TFieldIterator<FProperty> It(&Struct); It; ++It)
			{
				if (IsStoredField(**It) && Include(**It))
				{
					Editable.Add(It->GetName());
				}
			}
			const int32 Shown = FMath::Min(Editable.Num(), 40);
			FString List = FString::Join(TArrayView<const FString>(Editable.GetData(), Shown), TEXT(", "));
			if (Editable.Num() > Shown)
			{
				List += TEXT(", ...");
			}
			return Context.Reader.Fail(TEXT("Sequencer.FieldNotEditable"), FString::Printf(TEXT("%s has no writable field '%s' here%s. Fields: %s."),
				*Struct.GetName(), *Name, Property != nullptr ? TEXT(" (it is a document key of its own, derived, or not stored)") : TEXT(""), *List),
				Join(Context.Path, Name));
		}
		Listed.Add(Property->GetFName(), &Object.Elements[Index]);
	}
	const FValue Empty = MakeEmptyObject();
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		const FProperty& Property = **It;
		if (!IsStoredField(Property) || !Include(Property))
		{
			continue;
		}
		void* ValuePtr = Property.ContainerPtrToValuePtr<void>(Memory);
		const void* DefaultPtr = Defaults != nullptr ? Property.ContainerPtrToValuePtr<void>(Defaults) : nullptr;
		const FValue* const* Found = Listed.Find(Property.GetFName());
		const FValue* Authored = Found != nullptr ? *Found : nullptr;
		FDecodeContext Inner = Child(Context, Property.GetName());
		if (Authored != nullptr && UnsupportedText(*Authored) != nullptr)
		{
			// Kept as it is (or imported from its export text when that changed).
			if (!DecodeUnsupported(Property, UnsupportedText(*Authored)->StringValue, ValuePtr, Inner))
			{
				return false;
			}
			continue;
		}
		if (DefaultPtr != nullptr && IsMemberwiseStruct(Property))
		{
			// Engine structs member by member (members holding channels are written by channel name, never reset here).
			const UScriptStruct& Members = *CastFieldChecked<FStructProperty>(&Property)->Struct;
			if (!DecodeSparseObject(Members, Authored != nullptr ? *Authored : Empty, ValuePtr, DefaultPtr,
				[](const FProperty& Member) { return !IsChannelField(Member); }, Inner))
			{
				return false;
			}
			continue;
		}
		if (Authored == nullptr)
		{
			// Omitted: the default value (instanced subobjects of the defaults are never shared: an instanced field takes
			// an empty default (no object, no elements) and otherwise stays as it is).
			if (DefaultPtr != nullptr && (!IsInstanced(Property) || IsEmptyDefault(Property, DefaultPtr))
				&& !Property.Identical(ValuePtr, DefaultPtr, PPF_None))
			{
				if (IsInstanced(Property))
				{
					MarkProxyChanged(Inner);
				}
				Property.CopyCompleteValue(ValuePtr, DefaultPtr);
			}
			continue;
		}
		if (SameAsCurrent(Property, *Authored, ValuePtr, Inner))
		{
			continue;
		}
		// From the default value, the authored value decoded onto it (omitted struct members keep their default); instanced
		// objects and time warps from the current value (an object of the same class is edited in place).
		FPropertyValueBuffer Value(Property);
		Property.CopyCompleteValue(Value.Memory, DefaultPtr != nullptr && !StartsFromCurrent(Property) ? DefaultPtr : ValuePtr);
		if (!DecodeValue(Property, *Authored, Value.Memory, Inner))
		{
			return false;
		}
		if (!Property.Identical(ValuePtr, Value.Memory, PPF_None))
		{
			Property.CopyCompleteValue(ValuePtr, Value.Memory);
		}
	}
	return true;
}

FPropertyValueBuffer::FPropertyValueBuffer(const FProperty& InProperty)
	: Property(InProperty)
{
	Memory = FMemory::Malloc(FMath::Max(Property.GetSize(), 1), Property.GetMinAlignment());
	Property.InitializeValue(Memory);
}

FPropertyValueBuffer::~FPropertyValueBuffer()
{
	Property.DestroyValue(Memory);
	FMemory::Free(Memory);
}
} // namespace UE::AssetDocument::Graph::Sequencer
