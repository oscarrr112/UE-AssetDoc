// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Channels of a section (design §3.3): read through the section's channel proxy (the channels Sequencer's tree shows,
// named by FMovieSceneChannelMetaData), keys through the channel struct's reflected key arrays (the KeyTimes /
// KeyValues properties every channel declares, the same reflection Sequencer's key struct generator uses). Float and
// double channels get the Sequencer key-menu interpolation tokens and their stored tangents; other channels give
// their key values through the Editor typed value codec with the movie-scene adapters. Writes (seq.edit.v1) go through
// the same names: curve keys with the channel's Set + AutoSetTangents (the key menu's interpolation tokens mapped as
// AddKeyToChannel maps them), other keys through the reflected arrays with the decoder of AssetDocumentSequencerDecode.cpp.

#include "AssetDocumentSequencerPrimitives.h"

#include "Channels/MovieSceneChannel.h"
#include "Channels/MovieSceneChannelEditorData.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneEvent.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MovieSceneSection.h"
#include "Algo/BinarySearch.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::Sequencer
{
namespace SequencerChannelsLocal
{
constexpr int32 ObjectKeyLimit = 64;     // decision §8.1: columnar form above 64 keys
constexpr int32 ColumnChunk = 400;       // keys per "columns" record (value budget)

struct FChannelStruct
{
	const UScriptStruct* Struct = nullptr;
	const FArrayProperty* Times = nullptr;
	const FArrayProperty* Values = nullptr;
	const FProperty* Default = nullptr;
	const FBoolProperty* HasDefault = nullptr;
	const FProperty* PreInfinity = nullptr;
	const FProperty* PostInfinity = nullptr;
	bool bCurve = false;  // float / double (and their subtypes, e.g. time-warp curves): FMovieSceneFloatValue / FMovieSceneDoubleValue keys
	bool bDouble = false; // a double channel (or a subtype)
};

const FChannelStruct* FindChannelStruct(FName TypeName)
{
	static TMap<FName, FChannelStruct> Cache;
	if (const FChannelStruct* Found = Cache.Find(TypeName))
	{
		return Found->Struct != nullptr ? Found : nullptr;
	}
	FChannelStruct Info;
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		if (It->GetFName() == TypeName && It->IsChildOf(FMovieSceneChannel::StaticStruct()))
		{
			Info.Struct = *It;
			break;
		}
	}
	if (Info.Struct != nullptr)
	{
		for (TFieldIterator<FProperty> It(Info.Struct); It; ++It)
		{
			const FArrayProperty* Array = CastField<FArrayProperty>(*It);
#if WITH_EDITORONLY_DATA
			if (Array != nullptr && It->HasMetaData(TEXT("KeyTimes")))
			{
				Info.Times = Array;
			}
			else if (Array != nullptr && It->HasMetaData(TEXT("KeyValues")))
			{
				Info.Values = Array;
			}
#endif
		}
		Info.Default = Info.Struct->FindPropertyByName(TEXT("DefaultValue"));
		Info.HasDefault = CastField<FBoolProperty>(Info.Struct->FindPropertyByName(TEXT("bHasDefaultValue")));
		Info.PreInfinity = Info.Struct->FindPropertyByName(TEXT("PreInfinityExtrap"));
		Info.PostInfinity = Info.Struct->FindPropertyByName(TEXT("PostInfinityExtrap"));
		Info.bDouble = Info.Struct->IsChildOf(FMovieSceneDoubleChannel::StaticStruct());
		Info.bCurve = Info.bDouble || Info.Struct->IsChildOf(FMovieSceneFloatChannel::StaticStruct());
		const FStructProperty* TimesInner = Info.Times != nullptr ? CastField<FStructProperty>(Info.Times->Inner) : nullptr;
		if (TimesInner == nullptr || TimesInner->Struct != TBaseStructure<FFrameNumber>::Get())
		{
			Info.Times = nullptr;
		}
	}
	Cache.Add(TypeName, Info);
	return Info.Struct != nullptr ? Cache.Find(TypeName) : nullptr;
}

FString ExtrapolationToken(const FProperty* Property, const void* Channel)
{
	const FByteProperty* Byte = CastField<FByteProperty>(Property);
	if (Byte == nullptr)
	{
		return FString();
	}
	const uint8 Value = Byte->GetPropertyValue_InContainer(Channel);
	if (Value == RCCE_Constant)
	{
		return FString();
	}
	FString Name = StaticEnum<ERichCurveExtrapolation>()->GetNameStringByValue(Value);
	Name.RemoveFromStart(TEXT("RCCE_"));
	return Name;
}

FString InterpToken(ERichCurveInterpMode Interp, ERichCurveTangentMode Tangent)
{
	switch (Interp)
	{
	case RCIM_Constant:
		return TEXT("Constant");
	case RCIM_Linear:
		return TEXT("Linear");
	case RCIM_Cubic:
		switch (Tangent)
		{
		case RCTM_SmartAuto:
			return TEXT("SmartAuto");
		case RCTM_Auto:
			return TEXT("Auto");
		case RCTM_User:
			return TEXT("User");
		case RCTM_Break:
			return TEXT("Break");
		default:
			return TEXT("Cubic");
		}
	default:
		return TEXT("None");
	}
}

FString WeightModeToken(ERichCurveTangentWeightMode Mode)
{
	switch (Mode)
	{
	case RCTWM_WeightedArrive:
		return TEXT("Arrive");
	case RCTWM_WeightedLeave:
		return TEXT("Leave");
	case RCTWM_WeightedBoth:
		return TEXT("Both");
	default:
		return FString();
	}
}

// One curve key's parts (float or double channel).
struct FCurveKey
{
	double Value = 0.0;
	FString Interp;
	FMovieSceneTangentData Tangent;
	bool bExplicitTangents = false;
	bool bWeighted = false;
};

template <typename FValueType>
bool HasExplicitTangents(const FValueType& Key)
{
	return Key.InterpMode == RCIM_Cubic && (Key.TangentMode == RCTM_User || Key.TangentMode == RCTM_Break);
}

bool SameTangent(const FMovieSceneTangentData& A, const FMovieSceneTangentData& B)
{
	auto Near = [](float X, float Y) { return FMath::Abs(X - Y) <= 1.e-5f * FMath::Max(FMath::Abs(X), FMath::Abs(Y)) + 1.e-12f; };
	return Near(A.ArriveTangent, B.ArriveTangent) && Near(A.LeaveTangent, B.LeaveTangent) && A.TangentWeightMode == B.TangentWeightMode
		&& (A.TangentWeightMode == RCTWM_WeightedNone || (Near(A.ArriveTangentWeight, B.ArriveTangentWeight) && Near(A.LeaveTangentWeight, B.LeaveTangentWeight)));
}

// The write's tangent rule (WriteCurve): User / Break keys and keys with stated tangents keep theirs; the others start
// without stored tangents and AutoSetTangents computes them (as for keys the editor adds); stated tangents survive it.
template <typename FChannelType>
void ComputeTangents(FChannelType& Channel, const TArray<bool>& Stated, const TArray<FMovieSceneTangentData>& Tangents)
{
	auto Values = Channel.GetData().GetValues();
	for (int32 Index = 0; Index < Values.Num(); ++Index)
	{
		if (Stated[Index])
		{
			Values[Index].Tangent = Tangents[Index];
		}
		else if (!HasExplicitTangents(Values[Index]))
		{
			Values[Index].Tangent = FMovieSceneTangentData();
		}
	}
	Channel.AutoSetTangents();
	for (int32 Index = 0; Index < Values.Num(); ++Index)
	{
		if (Stated[Index])
		{
			Values[Index].Tangent = Tangents[Index];
		}
	}
}

// The keys (other than User / Break) whose stored tangents the write rule would not reproduce: saved by an older
// Sequencer, or left by a key-menu change (a key made Constant keeps the tangent that still shapes the segment before
// it). They are read with their tangents, so the document reproduces the stored curve exactly.
template <typename FChannelType>
TArray<bool> StatedTangents(const FChannelType& Channel)
{
	const auto Stored = Channel.GetData().GetValues();
	TArray<FMovieSceneTangentData> Tangents;
	for (const auto& Key : Stored)
	{
		Tangents.Add(Key.Tangent);
	}
	TArray<bool> Stated;
	Stated.Init(false, Stored.Num());
	for (int32 Pass = 0; Pass <= Stored.Num(); ++Pass)
	{
		FChannelType Copy = Channel;
		ComputeTangents(Copy, Stated, Tangents);
		const auto Computed = Copy.GetData().GetValues();
		bool bAdded = false;
		for (int32 Index = 0; Index < Stored.Num(); ++Index)
		{
			if (!Stated[Index] && !HasExplicitTangents(Stored[Index]) && !SameTangent(Computed[Index].Tangent, Stored[Index].Tangent))
			{
				Stated[Index] = true;
				bAdded = true;
			}
		}
		if (!bAdded)
		{
			break;
		}
	}
	return Stated;
}

TArray<bool> StatedTangents(const FChannelStruct& Info, const void* Channel)
{
	if (Info.bDouble)
	{
		return StatedTangents(*static_cast<const FMovieSceneDoubleChannel*>(Channel));
	}
	return StatedTangents(*static_cast<const FMovieSceneFloatChannel*>(Channel));
}

template <typename FValueType>
FCurveKey CurveKey(const FValueType& Key, bool bDouble, bool bStated)
{
	FCurveKey Out;
	Out.Value = bDouble ? static_cast<double>(Key.Value) : ShortestFloat(static_cast<double>(Key.Value));
	Out.Interp = InterpToken(Key.InterpMode.GetValue(), Key.TangentMode.GetValue());
	Out.Tangent = Key.Tangent;
	Out.bExplicitTangents = HasExplicitTangents(Key) || bStated;
	Out.bWeighted = Out.bExplicitTangents && Key.Tangent.TangentWeightMode != RCTWM_WeightedNone;
	return Out;
}

FCurveKey ReadCurveKey(const FChannelStruct& Info, const void* ValuePtr, bool bStated)
{
	if (Info.bDouble)
	{
		return CurveKey(*static_cast<const FMovieSceneDoubleValue*>(ValuePtr), true, bStated);
	}
	return CurveKey(*static_cast<const FMovieSceneFloatValue*>(ValuePtr), false, bStated);
}

FValue CurveKeyObject(const FCurveKey& Key, FValue Frame)
{
	FObjectBuilder Object;
	Object.Add(TEXT("Frame"), MoveTemp(Frame));
	Object.Add(TEXT("Value"), MakeNumber(Key.Value));
	if (Key.Interp != TEXT("SmartAuto"))
	{
		Object.AddString(TEXT("Interp"), Key.Interp);
	}
	if (Key.bExplicitTangents)
	{
		Object.Add(TEXT("Arrive"), MakeNumber(ShortestFloat(Key.Tangent.ArriveTangent)));
		Object.Add(TEXT("Leave"), MakeNumber(ShortestFloat(Key.Tangent.LeaveTangent)));
	}
	if (Key.bWeighted)
	{
		Object.Add(TEXT("ArriveWeight"), MakeNumber(ShortestFloat(Key.Tangent.ArriveTangentWeight)));
		Object.Add(TEXT("LeaveWeight"), MakeNumber(ShortestFloat(Key.Tangent.LeaveTangentWeight)));
		Object.AddString(TEXT("WeightMode"), WeightModeToken(Key.Tangent.TangentWeightMode.GetValue()));
	}
	return Object.Build();
}

FString ChannelName(const FMovieSceneChannelEntry& Entry, int32 Index)
{
#if WITH_EDITOR
	const TArrayView<const FMovieSceneChannelMetaData> MetaData = Entry.GetMetaData();
	if (MetaData.IsValidIndex(Index) && !MetaData[Index].Name.IsNone())
	{
		return MetaData[Index].Name.ToString();
	}
#endif
	return TEXT("Value");
}

FValue ParameterPathValue(TConstArrayView<int32> Parameter)
{
	TArray<FValue> Indices;
	for (const int32 Index : Parameter)
	{
		Indices.Add(MakeInt(Index));
	}
	return MakeArray(MoveTemp(Indices));
}

void ReadKeys(const FChannelStruct& Info, const void* Channel, const FString& Name, TConstArrayView<int32> Parameter, FEncodeContext& Context,
	TArray<FValue>& OutRecords)
{
	FScriptArrayHelper Times(Info.Times, Info.Times->ContainerPtrToValuePtr<void>(Channel));
	FScriptArrayHelper Values(Info.Values, Info.Values->ContainerPtrToValuePtr<void>(Channel));
	const int32 Num = FMath::Min(Times.Num(), Values.Num());
	if (Num == 0)
	{
		return;
	}
	auto FrameAt = [&Times, &Context](int32 Index)
	{
		return MakeFrames(reinterpret_cast<const FFrameNumber*>(Times.GetRawPtr(Index))->Value, Context.Time);
	};
	const FString ChannelPath = Context.Path + TEXT("/Channels/") + Escape(Name);
	TArray<bool> Stated;
	if (Info.bCurve)
	{
		Stated = StatedTangents(Info, Channel);
	}
	if (Num <= ObjectKeyLimit)
	{
		TArray<FValue> Keys;
		for (int32 Index = 0; Index < Num; ++Index)
		{
			if (Info.bCurve)
			{
				Keys.Add(CurveKeyObject(ReadCurveKey(Info, Values.GetRawPtr(Index), Stated.IsValidIndex(Index) && Stated[Index]), FrameAt(Index)));
				continue;
			}
			FEncodeContext KeyContext{Context.Model, Context.Time, Context.Warnings, FString::Printf(TEXT("%s/Keys/%d/Value"), *ChannelPath, Index)};
			FObjectBuilder Key;
			Key.Add(TEXT("Frame"), FrameAt(Index));
			const FStructProperty* Inner = CastField<FStructProperty>(Info.Values->Inner);
			if (Inner != nullptr && Inner->Struct == FMovieSceneEvent::StaticStruct())
			{
				KeyContext.Path = FString::Printf(TEXT("%s/Keys/%d/Event"), *ChannelPath, Index);
				Key.Add(TEXT("Event"), EncodeEvent(*reinterpret_cast<const FMovieSceneEvent*>(Values.GetRawPtr(Index)), KeyContext));
			}
			else
			{
				Key.Add(TEXT("Value"), EncodeValue(*Info.Values->Inner, Values.GetRawPtr(Index), KeyContext));
			}
			Keys.Add(Key.Build());
		}
		FObjectBuilder Record;
		Record.AddString(TEXT("kind"), TEXT("keys"));
		Record.AddString(TEXT("channel"), Name);
		if (!Parameter.IsEmpty())
		{
			Record.Add(TEXT("parameter"), ParameterPathValue(Parameter));
		}
		Record.Add(TEXT("keys"), MakeArray(MoveTemp(Keys)));
		OutRecords.Add(Record.Build());
		return;
	}

	// Columnar form (decision §8.1), chunked; Python joins the chunks and collapses uniform columns.
	TArray<FCurveKey> Curve;
	bool bAnyTangents = false;
	bool bAnyWeights = false;
	if (Info.bCurve)
	{
		for (int32 Index = 0; Index < Num; ++Index)
		{
			Curve.Add(ReadCurveKey(Info, Values.GetRawPtr(Index), Stated.IsValidIndex(Index) && Stated[Index]));
			bAnyTangents |= Curve.Last().bExplicitTangents;
			bAnyWeights |= Curve.Last().bWeighted;
		}
	}
	for (int32 Start = 0; Start < Num; Start += ColumnChunk)
	{
		const int32 End = FMath::Min(Start + ColumnChunk, Num);
		TArray<FValue> Frames;
		TArray<FValue> Column;
		TArray<FValue> Interp;
		TArray<FValue> Arrive;
		TArray<FValue> Leave;
		TArray<FValue> ArriveWeight;
		TArray<FValue> LeaveWeight;
		TArray<FValue> WeightMode;
		for (int32 Index = Start; Index < End; ++Index)
		{
			Frames.Add(FrameAt(Index));
			if (!Info.bCurve)
			{
				FEncodeContext KeyContext{Context.Model, Context.Time, Context.Warnings, FString::Printf(TEXT("%s/Values/%d"), *ChannelPath, Index)};
				const FStructProperty* Inner = CastField<FStructProperty>(Info.Values->Inner);
				Column.Add(Inner != nullptr && Inner->Struct == FMovieSceneEvent::StaticStruct()
					? EncodeEvent(*reinterpret_cast<const FMovieSceneEvent*>(Values.GetRawPtr(Index)), KeyContext)
					: EncodeValue(*Info.Values->Inner, Values.GetRawPtr(Index), KeyContext));
				continue;
			}
			const FCurveKey& Key = Curve[Index];
			Column.Add(MakeNumber(Key.Value));
			Interp.Add(MakeString(Key.Interp));
			if (bAnyTangents)
			{
				Arrive.Add(Key.bExplicitTangents ? MakeNumber(ShortestFloat(Key.Tangent.ArriveTangent)) : MakeNull());
				Leave.Add(Key.bExplicitTangents ? MakeNumber(ShortestFloat(Key.Tangent.LeaveTangent)) : MakeNull());
			}
			if (bAnyWeights)
			{
				ArriveWeight.Add(Key.bWeighted ? MakeNumber(ShortestFloat(Key.Tangent.ArriveTangentWeight)) : MakeNull());
				LeaveWeight.Add(Key.bWeighted ? MakeNumber(ShortestFloat(Key.Tangent.LeaveTangentWeight)) : MakeNull());
				WeightMode.Add(Key.bWeighted ? MakeString(WeightModeToken(Key.Tangent.TangentWeightMode.GetValue())) : MakeNull());
			}
		}
		FObjectBuilder Record;
		Record.AddString(TEXT("kind"), TEXT("columns"));
		Record.AddString(TEXT("channel"), Name);
		if (!Parameter.IsEmpty())
		{
			Record.Add(TEXT("parameter"), ParameterPathValue(Parameter));
		}
		Record.Add(TEXT("frames"), MakeArray(MoveTemp(Frames)));
		Record.Add(TEXT("values"), MakeArray(MoveTemp(Column)));
		if (Info.bCurve)
		{
			Record.Add(TEXT("interp"), MakeArray(MoveTemp(Interp)));
		}
		if (bAnyTangents)
		{
			Record.Add(TEXT("arrive"), MakeArray(MoveTemp(Arrive)));
			Record.Add(TEXT("leave"), MakeArray(MoveTemp(Leave)));
		}
		if (bAnyWeights)
		{
			Record.Add(TEXT("arrive_weight"), MakeArray(MoveTemp(ArriveWeight)));
			Record.Add(TEXT("leave_weight"), MakeArray(MoveTemp(LeaveWeight)));
			Record.Add(TEXT("weight_mode"), MakeArray(MoveTemp(WeightMode)));
		}
		OutRecords.Add(Record.Build());
	}
}

// The channels of the section's proxy by document name; the channels of parameter sets are written in Parameters and
// skipped unless bWithParameters.
template <typename FVisit>
void ForEachChannel(const UMovieSceneSection& Section, FVisit Visit, bool bWithParameters = false)
{
	const TSet<const void*> Skipped = bWithParameters ? TSet<const void*>() : ParameterChannelAddresses(Section);
	const FMovieSceneChannelProxy& Proxy = Section.GetChannelProxy();
	TSet<FString> Names;
	for (const FMovieSceneChannelEntry& Entry : Proxy.GetAllEntries())
	{
		const FChannelStruct* Info = FindChannelStruct(Entry.GetChannelTypeName());
		const TArrayView<FMovieSceneChannel* const> Channels = Entry.GetChannels();
		for (int32 Index = 0; Index < Channels.Num(); ++Index)
		{
			if (Skipped.Contains(static_cast<const void*>(Channels[Index])))
			{
				continue;
			}
			const FString Base = ChannelName(Entry, Index);
			FString Name = Base;
			for (int32 Suffix = 2; Names.Contains(Name); ++Suffix)
			{
				Name = FString::Printf(TEXT("%s#%d"), *Base, Suffix);
			}
			Names.Add(Name);
			Visit(Name, Name != Base, Entry.GetChannelTypeName(), Info, static_cast<const void*>(Channels[Index]));
		}
	}
}
} // namespace

// ---- writes --------------------------------------------------------------------------------------------------------

namespace SequencerChannelsLocal
{
struct FChannelRef
{
	FString Name;
	FName TypeName;
	const FChannelStruct* Info = nullptr;
	FMovieSceneChannel* Channel = nullptr;

	bool IsSupported() const { return Info != nullptr && Info->Times != nullptr && Info->Values != nullptr && Channel != nullptr; }
};

TArray<FChannelRef> CollectChannels(const UMovieSceneSection& Section)
{
	TArray<FChannelRef> Refs;
	ForEachChannel(Section, [&Refs](const FString& Name, bool, FName TypeName, const FChannelStruct* Info, const void* Channel)
	{
		Refs.Add({Name, TypeName, Info, static_cast<FMovieSceneChannel*>(const_cast<void*>(Channel))});
	});
	return Refs;
}

bool InterpModes(const FString& Token, ERichCurveInterpMode& OutInterp, ERichCurveTangentMode& OutTangent)
{
	struct FMode
	{
		const TCHAR* Token;
		ERichCurveInterpMode Interp;
		ERichCurveTangentMode Tangent;
	};
	static const FMode Modes[] = {
		{TEXT("SmartAuto"), RCIM_Cubic, RCTM_SmartAuto}, {TEXT("Auto"), RCIM_Cubic, RCTM_Auto}, {TEXT("User"), RCIM_Cubic, RCTM_User},
		{TEXT("Break"), RCIM_Cubic, RCTM_Break}, {TEXT("Linear"), RCIM_Linear, RCTM_Auto}, {TEXT("Constant"), RCIM_Constant, RCTM_Auto},
		{TEXT("Cubic"), RCIM_Cubic, RCTM_None}, {TEXT("None"), RCIM_None, RCTM_Auto}};
	for (const FMode& Mode : Modes)
	{
		if (Token == Mode.Token)
		{
			OutInterp = Mode.Interp;
			OutTangent = Mode.Tangent;
			return true;
		}
	}
	return false;
}

bool WeightMode(const FString& Token, ERichCurveTangentWeightMode& Out)
{
	static const TPair<const TCHAR*, ERichCurveTangentWeightMode> Modes[] = {
		{TEXT("Arrive"), RCTWM_WeightedArrive}, {TEXT("Leave"), RCTWM_WeightedLeave}, {TEXT("Both"), RCTWM_WeightedBoth},
		{TEXT("None"), RCTWM_WeightedNone}, {TEXT(""), RCTWM_WeightedNone}};
	for (const auto& Mode : Modes)
	{
		if (Token == Mode.Key)
		{
			Out = Mode.Value;
			return true;
		}
	}
	return false;
}

bool IsNumber(const FValue& Value)
{
	return Value.Kind == EAssetDocumentRuntimeValueKind::Integer || Value.Kind == EAssetDocumentRuntimeValueKind::Number;
}

double NumberOf(const FValue& Value)
{
	return Value.Kind == EAssetDocumentRuntimeValueKind::Integer ? static_cast<double>(Value.IntegerValue) : Value.NumberValue;
}

// One authored key, from a key object or a row of the columns.
struct FKeyIn
{
	int32 Ticks = 0;
	const FValue* Value = nullptr;  // curve: number; other channels: the key value (event channels: the Event object)
	FString Interp = TEXT("SmartAuto");
	const FValue* Arrive = nullptr;
	const FValue* Leave = nullptr;
	const FValue* ArriveWeight = nullptr;
	const FValue* LeaveWeight = nullptr;
	FString WeightMode;
	FString Path;                   // relative to the channel
};

const FValue* Column(const FValue& Channel, const TCHAR* Key, int32 Index)
{
	const FValue* Array = FindField(Channel, Key);
	if (Array == nullptr || Array->Kind != EAssetDocumentRuntimeValueKind::Array || !Array->Elements.IsValidIndex(Index))
	{
		return nullptr;
	}
	const FValue& Element = Array->Elements[Index];
	return Element.Kind == EAssetDocumentRuntimeValueKind::Null ? nullptr : &Element;
}

bool GatherKeys(const FValue& Channel, bool bEventChannel, FDecodeContext& Context, TArray<FKeyIn>& Out)
{
	auto Frame = [&Context](const FValue& Value, const FString& Path, int32& OutTicks)
	{
		FDecodeContext At = Context;
		At.Path = Context.Path + TEXT("/") + Path;
		return DecodeFrames(Value, At, OutTicks);
	};
	auto Fail = [&Context](const FString& Path, const FString& Message)
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidKey"), Message, Context.Path + TEXT("/") + Path);
	};
	if (const FValue* Keys = FindField(Channel, TEXT("Keys")))
	{
		if (Keys->Kind != EAssetDocumentRuntimeValueKind::Array)
		{
			return Fail(TEXT("Keys"), TEXT("Keys is an array of key objects."));
		}
		for (int32 Index = 0; Index < Keys->Elements.Num(); ++Index)
		{
			const FValue& Key = Keys->Elements[Index];
			const FString Path = FString::Printf(TEXT("Keys/%d"), Index);
			const FValue* Time = Key.Kind == EAssetDocumentRuntimeValueKind::Object ? FindField(Key, TEXT("Frame")) : nullptr;
			if (Time == nullptr)
			{
				return Fail(Path, TEXT("A key is an object with Frame."));
			}
			static const TCHAR* Allowed[] = {TEXT("Frame"), TEXT("Value"), TEXT("Event"), TEXT("Interp"), TEXT("Arrive"), TEXT("Leave"),
				TEXT("ArriveWeight"), TEXT("LeaveWeight"), TEXT("WeightMode")};
			for (const FString& Field : Key.ObjectKeys)
			{
				bool bKnown = false;
				for (const TCHAR* Name : Allowed)
				{
					bKnown |= Field == Name;
				}
				if (!bKnown)
				{
					return Context.Reader.Fail(TEXT("Graph.UnknownField"), FString::Printf(TEXT("A key takes Frame, Value (Event for event keys), Interp, "
						"Arrive, Leave, ArriveWeight, LeaveWeight, WeightMode; '%s' is not one of them."), *Field), Context.Path + TEXT("/") + Path + TEXT("/")
						+ Escape(Field));
				}
			}
			FKeyIn In;
			In.Path = Path + (bEventChannel ? TEXT("/Event") : TEXT("/Value"));
			if (!Frame(*Time, Path + TEXT("/Frame"), In.Ticks))
			{
				return false;
			}
			In.Value = FindField(Key, bEventChannel ? TEXT("Event") : TEXT("Value"));
			if (In.Value == nullptr)
			{
				return Fail(Path, bEventChannel ? TEXT("An event key has Event ({Endpoint, BoundObjectPin?, Payload?}, or {} for an event that calls nothing).") : TEXT("A key has Value."));
			}
			if (const FValue* Interp = FindField(Key, TEXT("Interp")))
			{
				In.Interp = Interp->StringValue;
			}
			In.Arrive = FindField(Key, TEXT("Arrive"));
			In.Leave = FindField(Key, TEXT("Leave"));
			In.ArriveWeight = FindField(Key, TEXT("ArriveWeight"));
			In.LeaveWeight = FindField(Key, TEXT("LeaveWeight"));
			if (const FValue* Mode = FindField(Key, TEXT("WeightMode")))
			{
				In.WeightMode = Mode->StringValue;
			}
			Out.Add(MoveTemp(In));
		}
		return true;
	}
	const FValue* Frames = FindField(Channel, TEXT("Frames"));
	if (Frames == nullptr)
	{
		return true;
	}
	const FValue* Values = FindField(Channel, TEXT("Values"));
	if (Frames->Kind != EAssetDocumentRuntimeValueKind::Array || Values == nullptr || Values->Kind != EAssetDocumentRuntimeValueKind::Array
		|| Values->Elements.Num() != Frames->Elements.Num())
	{
		return Fail(TEXT("Frames"), TEXT("The columnar form is Frames and Values of the same length (plus Interp, Arrive, Leave, ... per key)."));
	}
	const FValue* Interp = FindField(Channel, TEXT("Interp"));
	for (int32 Index = 0; Index < Frames->Elements.Num(); ++Index)
	{
		FKeyIn In;
		In.Path = FString::Printf(TEXT("Values/%d"), Index);
		if (!Frame(Frames->Elements[Index], FString::Printf(TEXT("Frames/%d"), Index), In.Ticks))
		{
			return false;
		}
		In.Value = &Values->Elements[Index];
		if (Interp != nullptr && Interp->Kind == EAssetDocumentRuntimeValueKind::String)
		{
			In.Interp = Interp->StringValue;
		}
		else if (const FValue* Token = Column(Channel, TEXT("Interp"), Index))
		{
			In.Interp = Token->StringValue;
		}
		In.Arrive = Column(Channel, TEXT("Arrive"), Index);
		In.Leave = Column(Channel, TEXT("Leave"), Index);
		In.ArriveWeight = Column(Channel, TEXT("ArriveWeight"), Index);
		In.LeaveWeight = Column(Channel, TEXT("LeaveWeight"), Index);
		if (const FValue* Mode = Column(Channel, TEXT("WeightMode"), Index))
		{
			In.WeightMode = Mode->StringValue;
		}
		Out.Add(MoveTemp(In));
	}
	return true;
}

// Tangents (ComputeTangents): User / Break keys and keys that state Arrive / Leave keep them; the others are computed by
// AutoSetTangents. An append (the next chunk of a dense channel) keeps the keys already written, computes the tangents
// of the whole channel again and keeps the stated tangents of the keys the op lists in Stated (the earlier chunks'
// keys with Arrive / Leave), so every chunk's stated tangents survive.
template <typename FChannelType, typename FKeyValueType>
bool WriteCurve(FChannelType& Channel, const TArray<FKeyIn>& Keys, bool bAppend, TConstArrayView<int32> StatedBefore, FDecodeContext& Context)
{
	TArray<FFrameNumber> Times;
	TArray<FKeyValueType> Values;
	TArray<bool> Stated;
	if (bAppend)
	{
		Times.Append(Channel.GetData().GetTimes().GetData(), Channel.GetData().GetTimes().Num());
		Values.Append(Channel.GetData().GetValues().GetData(), Channel.GetData().GetValues().Num());
		Stated.Init(false, Times.Num());
		for (const int32 Index : StatedBefore)
		{
			if (!Stated.IsValidIndex(Index))
			{
				return Context.Reader.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("stated: key %d is not in the channel (it has %d keys)."),
					Index, Stated.Num()), TEXT("stated"));
			}
			Stated[Index] = true;
		}
	}
	else
	{
		Channel.Reset();
	}
	for (const FKeyIn& Key : Keys)
	{
		const FString Path = Context.Path + TEXT("/") + Key.Path;
		if (!IsNumber(*Key.Value))
		{
			return Context.Reader.Fail(TEXT("Sequencer.InvalidKey"), TEXT("A curve key's Value is a number."), Path);
		}
		FKeyValueType Value;
		Value.Value = static_cast<decltype(Value.Value)>(NumberOf(*Key.Value));
		ERichCurveInterpMode Interp = RCIM_Cubic;
		ERichCurveTangentMode Tangent = RCTM_SmartAuto;
		if (!InterpModes(Key.Interp, Interp, Tangent))
		{
			return Context.Reader.Fail(TEXT("Sequencer.InvalidKey"), FString::Printf(TEXT("'%s' is not a key interpolation (SmartAuto, Auto, User, Break, "
				"Linear, Constant)."), *Key.Interp), Path);
		}
		Value.InterpMode = Interp;
		Value.TangentMode = Tangent;
		const bool bStated = !(Tangent == RCTM_User || Tangent == RCTM_Break) && (Key.Arrive != nullptr || Key.Leave != nullptr);
		if (Tangent == RCTM_User || Tangent == RCTM_Break || bStated)
		{
			Value.Tangent.ArriveTangent = Key.Arrive != nullptr && IsNumber(*Key.Arrive) ? static_cast<float>(NumberOf(*Key.Arrive)) : 0.f;
			Value.Tangent.LeaveTangent = Key.Leave != nullptr && IsNumber(*Key.Leave) ? static_cast<float>(NumberOf(*Key.Leave)) : 0.f;
			ERichCurveTangentWeightMode Mode = RCTWM_WeightedNone;
			if (!WeightMode(Key.WeightMode, Mode))
			{
				return Context.Reader.Fail(TEXT("Sequencer.InvalidKey"), FString::Printf(TEXT("'%s' is not a tangent weight mode (Arrive, Leave, Both)."),
					*Key.WeightMode), Path);
			}
			Value.Tangent.TangentWeightMode = Mode;
			if (Mode != RCTWM_WeightedNone)
			{
				Value.Tangent.ArriveTangentWeight = Key.ArriveWeight != nullptr && IsNumber(*Key.ArriveWeight) ? static_cast<float>(NumberOf(*Key.ArriveWeight)) : 0.f;
				Value.Tangent.LeaveTangentWeight = Key.LeaveWeight != nullptr && IsNumber(*Key.LeaveWeight) ? static_cast<float>(NumberOf(*Key.LeaveWeight)) : 0.f;
			}
		}
		// Keys stay in time order (later keys at an equal time after the earlier ones, as AddKey inserts them).
		const int32 At = Algo::UpperBound(Times, FFrameNumber(Key.Ticks));
		Times.Insert(FFrameNumber(Key.Ticks), At);
		Values.Insert(Value, At);
		Stated.Insert(bStated, At);
	}
	TArray<FMovieSceneTangentData> Tangents;
	for (const FKeyValueType& Value : Values)
	{
		Tangents.Add(Value.Tangent);
	}
	Channel.Set(MoveTemp(Times), MoveTemp(Values));
	ComputeTangents(Channel, Stated, Tangents);
	return true;
}

bool WriteDiscrete(const FChannelStruct& Info, void* Channel, const TArray<FKeyIn>& Keys, FDecodeContext& Context)
{
	TArray<FFrameNumber>& Times = *Info.Times->ContainerPtrToValuePtr<TArray<FFrameNumber>>(Channel);
	FScriptArrayHelper Values(Info.Values, Info.Values->ContainerPtrToValuePtr<void>(Channel));
	for (const FKeyIn& Key : Keys)
	{
		const int32 At = Algo::UpperBound(Times, FFrameNumber(Key.Ticks));
		Times.Insert(FFrameNumber(Key.Ticks), At);
		Values.InsertValues(At, 1);
		FDecodeContext Inner = Context;
		Inner.Path = Context.Path + TEXT("/") + Key.Path;
		if (!DecodeValue(*Info.Values->Inner, *Key.Value, Values.GetRawPtr(At), Inner))
		{
			return false;
		}
	}
	return true;
}

bool SetExtrapolation(const FProperty* Property, void* Channel, const FValue* Token, const FString& Path, FDecodeContext& Context)
{
	const FByteProperty* Byte = CastField<FByteProperty>(Property);
	if (Byte == nullptr)
	{
		return Token == nullptr || Context.Reader.Fail(TEXT("Sequencer.InvalidKey"), TEXT("This channel has no extrapolation."), Path);
	}
	uint8 Value = RCCE_Constant;
	if (Token != nullptr)
	{
		const int64 Found = StaticEnum<ERichCurveExtrapolation>()->GetValueByNameString(TEXT("RCCE_") + Token->StringValue);
		if (Token->Kind != EAssetDocumentRuntimeValueKind::String || Found == INDEX_NONE
			|| Found == StaticEnum<ERichCurveExtrapolation>()->GetMaxEnumValue())
		{
			return Context.Reader.Fail(TEXT("Sequencer.InvalidKey"), TEXT("Extrapolation is Cycle, CycleWithOffset, Oscillate, Linear, Constant or None."),
				Path);
		}
		Value = static_cast<uint8>(Found);
	}
	Byte->SetPropertyValue_InContainer(Channel, Value);
	return true;
}
} // namespace

TArray<TPair<FString, bool>> ChannelNames(const UMovieSceneSection& Section)
{
	using namespace SequencerChannelsLocal;
	TArray<TPair<FString, bool>> Names;
	for (const FChannelRef& Ref : CollectChannels(Section))
	{
		Names.Emplace(Ref.Name, Ref.IsSupported());
	}
	return Names;
}

void ClearChannels(UMovieSceneSection& Section, const TSet<FString>& Keep)
{
	using namespace SequencerChannelsLocal;
	for (const FChannelRef& Ref : CollectChannels(Section))
	{
		if (!Ref.IsSupported() || Keep.Contains(Ref.Name))
		{
			continue;
		}
		Ref.Channel->Reset();
		if (const FByteProperty* Pre = CastField<FByteProperty>(Ref.Info->PreInfinity))
		{
			Pre->SetPropertyValue_InContainer(Ref.Channel, RCCE_Constant);
		}
		if (const FByteProperty* Post = CastField<FByteProperty>(Ref.Info->PostInfinity))
		{
			Post->SetPropertyValue_InContainer(Ref.Channel, RCCE_Constant);
		}
	}
}

namespace SequencerChannelsLocal
{
bool WriteChannelData(FName TypeName, const FChannelStruct* InfoPtr, void* Memory, const FValue& Channel, bool bAppend,
	TConstArrayView<int32> StatedBefore, FDecodeContext& Context)
{
	if (Channel.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidKey"), TEXT("A channel is an object ({Keys?, Default?, PreInfinity?, PostInfinity?})."), Context.Path);
	}
	if (InfoPtr == nullptr || InfoPtr->Times == nullptr || InfoPtr->Values == nullptr || Memory == nullptr)
	{
		if (FindField(Channel, TEXT("Unsupported")) != nullptr && Channel.ObjectKeys.Num() == 1)
		{
			return true; // kept as it is
		}
		return Context.Reader.Fail(TEXT("Sequencer.UnsupportedChannel"), FString::Printf(TEXT("Channel type %s has no reflected key arrays; it is kept "
			"as it is and cannot be written (list it as {\"Unsupported\": \"%s\"})."), *TypeName.ToString(), *TypeName.ToString()), Context.Path);
	}
	for (const FString& Key : Channel.ObjectKeys)
	{
		static const TCHAR* Allowed[] = {TEXT("Keys"), TEXT("Default"), TEXT("PreInfinity"), TEXT("PostInfinity"), TEXT("Frames"), TEXT("Values"),
			TEXT("Interp"), TEXT("Arrive"), TEXT("Leave"), TEXT("ArriveWeight"), TEXT("LeaveWeight"), TEXT("WeightMode")};
		bool bKnown = false;
		for (const TCHAR* Field : Allowed)
		{
			bKnown |= Key == Field;
		}
		if (!bKnown)
		{
			return Context.Reader.Fail(TEXT("Graph.UnknownField"), FString::Printf(TEXT("A channel takes Keys (or the columnar Frames, Values, Interp, "
				"...), Default, PreInfinity and PostInfinity; '%s' is not one of them."), *Key), Context.Path + TEXT("/") + Escape(Key));
		}
	}
	const FChannelStruct& Info = *InfoPtr;
	FMovieSceneChannel* Base = static_cast<FMovieSceneChannel*>(Memory);
	const FStructProperty* ValueStruct = CastField<FStructProperty>(Info.Values->Inner);
	const bool bEventChannel = ValueStruct != nullptr && ValueStruct->Struct == FMovieSceneEvent::StaticStruct();
	TArray<FKeyIn> Keys;
	if (!GatherKeys(Channel, bEventChannel, Context, Keys))
	{
		return false;
	}
	if (!bAppend && !Info.bCurve)
	{
		Base->Reset(); // curves are reset in WriteCurve
	}
	if (Info.bDouble)
	{
		if (!WriteCurve<FMovieSceneDoubleChannel, FMovieSceneDoubleValue>(*static_cast<FMovieSceneDoubleChannel*>(Memory), Keys, bAppend, StatedBefore, Context))
		{
			return false;
		}
	}
	else if (Info.bCurve)
	{
		if (!WriteCurve<FMovieSceneFloatChannel, FMovieSceneFloatValue>(*static_cast<FMovieSceneFloatChannel*>(Memory), Keys, bAppend, StatedBefore, Context))
		{
			return false;
		}
	}
	else if (!WriteDiscrete(Info, Memory, Keys, Context))
	{
		return false;
	}
	if (bAppend)
	{
		return true;
	}
	if (const FValue* Default = FindField(Channel, TEXT("Default")))
	{
		if (Info.Default == nullptr || Info.HasDefault == nullptr)
		{
			return Context.Reader.Fail(TEXT("Sequencer.InvalidKey"), TEXT("This channel has no default value."), Context.Path + TEXT("/Default"));
		}
		FDecodeContext Inner = Context;
		Inner.Path = Context.Path + TEXT("/Default");
		void* DefaultPtr = Info.Default->ContainerPtrToValuePtr<void>(Memory);
		if (const FFloatProperty* Float = CastField<FFloatProperty>(Info.Default))
		{
			if (!IsNumber(*Default))
			{
				return Context.Reader.Fail(TEXT("Sequencer.InvalidKey"), TEXT("Default is a number."), Inner.Path);
			}
			Float->SetPropertyValue(DefaultPtr, static_cast<float>(NumberOf(*Default)));
		}
		else if (!DecodeValue(*Info.Default, *Default, DefaultPtr, Inner))
		{
			return false;
		}
		Info.HasDefault->SetPropertyValue_InContainer(Memory, true);
	}
	if (!SetExtrapolation(Info.PreInfinity, Memory, FindField(Channel, TEXT("PreInfinity")), Context.Path + TEXT("/PreInfinity"), Context)
		|| !SetExtrapolation(Info.PostInfinity, Memory, FindField(Channel, TEXT("PostInfinity")), Context.Path + TEXT("/PostInfinity"), Context))
	{
		return false;
	}
	if (!Info.bCurve)
	{
		Base->PostEditChange(); // a curve's PostEditChange is AutoSetTangents, which WriteCurve ran (keeping stated tangents)
	}
	return true;
}
} // namespace

bool WriteChannel(UMovieSceneSection& Section, const FString& Name, const FValue& Channel, bool bAppend, FDecodeContext& Context,
	TConstArrayView<int32> Stated)
{
	using namespace SequencerChannelsLocal;
	const TArray<FChannelRef> Refs = CollectChannels(Section);
	const FChannelRef* Ref = Refs.FindByPredicate([&Name](const FChannelRef& Item) { return Item.Name == Name; });
	if (Ref == nullptr)
	{
		TArray<FString> Names;
		for (const FChannelRef& Item : Refs)
		{
			Names.Add(Item.Name);
		}
		return Context.Reader.Fail(TEXT("Sequencer.UnknownChannel"), FString::Printf(TEXT("%s has no channel '%s'. Channels: %s.%s"),
			*Section.GetClass()->GetName(), *Name, Names.IsEmpty() ? TEXT("(none)") : *FString::Join(Names, TEXT(", ")),
			UsesParameters(Section) ? TEXT(" Parameter channels are written in Parameters.") : TEXT("")), Context.Path);
	}
	return WriteChannelData(Ref->TypeName, Ref->Info, Ref->Channel, Channel, bAppend, Stated, Context);
}

bool WriteChannelValue(FName TypeName, void* Channel, const FValue& Value, bool bAppend, TConstArrayView<int32> Stated, FDecodeContext& Context)
{
	using namespace SequencerChannelsLocal;
	return WriteChannelData(TypeName, FindChannelStruct(TypeName), Channel, Value, bAppend, Stated, Context);
}

bool IsSupportedChannelType(FName TypeName)
{
	using namespace SequencerChannelsLocal;
	const FChannelStruct* Info = FindChannelStruct(TypeName);
	return Info != nullptr && Info->Times != nullptr && Info->Values != nullptr;
}

namespace SequencerChannelsLocal
{
void ReadOneChannel(const FString& Name, FName TypeName, const FChannelStruct* Info, const void* Channel, TConstArrayView<int32> Parameter,
	FEncodeContext& Context, FObjectBuilder& OutChannels, TArray<FValue>& OutKeyRecords)
{
	const FString ChannelPath = Context.Path + TEXT("/Channels/") + Escape(Name);
	FObjectBuilder Entry;
	if (Channel == nullptr || Info == nullptr || Info->Times == nullptr || Info->Values == nullptr)
	{
		Context.Warnings.Add(TEXT("Sequencer.UnsupportedChannel"), FString::Printf(TEXT("Channel type %s has no reflected key arrays; it "
			"is listed and kept as it is."), *TypeName.ToString()), ChannelPath);
		Entry.AddString(TEXT("Unsupported"), TypeName.ToString());
		OutChannels.Add(Name, Entry.Build());
		return;
	}
	if (Info->HasDefault != nullptr && Info->Default != nullptr && Info->HasDefault->GetPropertyValue_InContainer(Channel))
	{
		FEncodeContext DefaultContext{Context.Model, Context.Time, Context.Warnings, ChannelPath + TEXT("/Default")};
		const void* DefaultPtr = Info->Default->ContainerPtrToValuePtr<void>(Channel);
		if (const FFloatProperty* Float = CastField<FFloatProperty>(Info->Default))
		{
			Entry.Add(TEXT("Default"), MakeNumber(ShortestFloat(Float->GetPropertyValue(DefaultPtr))));
		}
		else
		{
			Entry.Add(TEXT("Default"), EncodeValue(*Info->Default, DefaultPtr, DefaultContext));
		}
	}
	const FString Pre = ExtrapolationToken(Info->PreInfinity, Channel);
	const FString Post = ExtrapolationToken(Info->PostInfinity, Channel);
	if (!Pre.IsEmpty())
	{
		Entry.AddString(TEXT("PreInfinity"), Pre);
	}
	if (!Post.IsEmpty())
	{
		Entry.AddString(TEXT("PostInfinity"), Post);
	}
	OutChannels.Add(Name, Entry.Build());
	FEncodeContext KeyContext{Context.Model, Context.Time, Context.Warnings, Context.Path};
	ReadKeys(*Info, Channel, Name, Parameter, KeyContext, OutKeyRecords);
}
} // namespace

void ReadChannelValue(FName TypeName, const void* Channel, const FString& Name, TConstArrayView<int32> Parameter, FEncodeContext& Context,
	FObjectBuilder& OutChannels, TArray<FValue>& OutKeyRecords)
{
	using namespace SequencerChannelsLocal;
	ReadOneChannel(Name, TypeName, FindChannelStruct(TypeName), Channel, Parameter, Context, OutChannels, OutKeyRecords);
}

void ReadChannels(const UMovieSceneSection& Section, FEncodeContext& Context, FObjectBuilder& OutChannels, TArray<FValue>& OutKeyRecords)
{
	using namespace SequencerChannelsLocal;
	ForEachChannel(Section, [&](const FString& Name, bool bRenamed, FName TypeName, const FChannelStruct* Info, const void* Channel)
	{
		if (bRenamed)
		{
			Context.Warnings.Add(TEXT("Sequencer.DuplicateChannelName"), FString::Printf(TEXT("Two channels of this section are named '%s'; "
				"the later one is keyed '%s'."), *Name.Left(Name.Find(TEXT("#"), ESearchCase::CaseSensitive, ESearchDir::FromEnd)), *Name),
				Context.Path + TEXT("/Channels/") + Escape(Name));
		}
		ReadOneChannel(Name, TypeName, Info, Channel, {}, Context, OutChannels, OutKeyRecords);
	});
}

TArray<TSharedPtr<FJsonValue>> DescribeChannels(const UMovieSceneSection& Section)
{
	using namespace SequencerChannelsLocal;
	TArray<TSharedPtr<FJsonValue>> Rows;
	ForEachChannel(Section, [&Rows](const FString& Name, bool, FName TypeName, const FChannelStruct* Info, const void*)
	{
		const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Name"), Name);
		FString Type = TypeName.ToString();
		Type.RemoveFromStart(TEXT("MovieScene"));
		Type.RemoveFromEnd(TEXT("Channel"));
		Row->SetStringField(TEXT("Type"), Type);
		if (Info == nullptr || Info->Times == nullptr || Info->Values == nullptr)
		{
			Row->SetBoolField(TEXT("Unsupported"), true);
		}
		else if (Info->bCurve)
		{
			Row->SetBoolField(TEXT("Curve"), true);
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	});
	return Rows;
}

void ForEachChannelKeyTimes(const UMovieSceneSection& Section, TFunctionRef<void(const FString& Channel, TConstArrayView<FFrameNumber> Times)> Visit)
{
	using namespace SequencerChannelsLocal;
	ForEachChannel(Section, [&Visit](const FString& Name, bool, FName, const FChannelStruct* Info, const void* Channel)
	{
		if (Info == nullptr || Info->Times == nullptr || Channel == nullptr)
		{
			return;
		}
		const TArray<FFrameNumber>& Times = *Info->Times->ContainerPtrToValuePtr<TArray<FFrameNumber>>(Channel);
		Visit(Name, Times);
	}, true);
}
} // namespace UE::AssetDocument::Graph::Sequencer
