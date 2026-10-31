// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Widget animation primitives (provider "umg"): what the UMG Animations tab and its
// Sequencer do when the user adds an animation, sets its display rate / playback range,
// binds a widget (or its slot), adds a property track and keys it.
//
// Frames are display-rate frames (what the Sequencer shows); the movie scene keeps its
// own tick resolution. Every property track with one section whose channels are float,
// double, bool, byte or integer channels is supported, on a widget, its slot or the user
// widget itself (target "root"), as is a material parameter track on a widget's brush
// (type "Material"). Anything else is reported with "supported": false, a reason and its
// keys read-only ("sections").
//
// DTOs (all carry "schema_version": 1):
//   umg.anim.query.v1        {} -> {animations: [{name, tracks}]}
//                            {name, track_offset?} -> {name, display_rate{numerator, denominator},
//                              playback{start, end}, total_tracks, next_track_offset, tracks[]}
//                            track: {widget, target: "widget"|"slot"|"root", property, type, supported, reason?,
//                              deactivated?, parameters?: [{name, kind}],
//                              section?: {start?, end?, active?, when_finished?, pre_roll?, post_roll?},
//                              channels: [{name, kind, default?, keys: [{frame, value, interp?, tangent?}]}],
//                              sections?: [{start?, end?, channels}] (read-only, unsupported tracks)}
//   umg.anim.set.v1          {name, display_rate?, playback?}            create or update the header
//   umg.anim.remove.v1       {name}
//   umg.anim.track.set.v1    {animation, widget, target?, property, type, deactivated?, parameters?, section?,
//                             channels: [{name, keys, default?}]}  without "section" the section grows to its keys
//   umg.anim.track.remove.v1 {animation, widget, target?, property, type}

#include "AssetDocumentWidgetPrimitives.h"

#include "AssetDocumentGraphPropertyCodec.h"
#include "AssetDocumentWidgetRawEdits.h"

#include "Algo/Count.h"
#include "Algo/Find.h"
#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/MovieSceneMarginTrack.h"
#include "Animation/MovieSceneWidgetMaterialTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "Channels/MovieSceneByteChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneEventChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneIntegerChannel.h"
#include "Channels/MovieSceneStringChannel.h"
#include "Blueprint/UserWidget.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "Components/PanelSlot.h"
#include "Components/Widget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Layout/Margin.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "Sections/MovieSceneParameterSection.h"
#include "Styling/SlateBrush.h"
#include "Slate/WidgetTransform.h"
#include "Styling/SlateColor.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Tracks/MovieSceneByteTrack.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneDoubleTrack.h"
#include "Tracks/MovieSceneEnumTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneIntegerTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditorUtils.h"

namespace UE::AssetDocument::Graph::Widget
{
namespace
{
// ---- track types ------------------------------------------------------------------------

struct FTrackType
{
	const TCHAR* Name;
	UClass* (*Class)();
};

UClass* FloatTrack() { return UMovieSceneFloatTrack::StaticClass(); }
UClass* DoubleTrack() { return UMovieSceneDoubleTrack::StaticClass(); }
UClass* BoolTrack() { return UMovieSceneBoolTrack::StaticClass(); }
UClass* ByteTrack() { return UMovieSceneByteTrack::StaticClass(); }
UClass* EnumTrack() { return UMovieSceneEnumTrack::StaticClass(); }
UClass* IntegerTrack() { return UMovieSceneIntegerTrack::StaticClass(); }
UClass* ColorTrack() { return UMovieSceneColorTrack::StaticClass(); }
UClass* TransformTrack() { return UMovieScene2DTransformTrack::StaticClass(); }
UClass* MarginTrack() { return UMovieSceneMarginTrack::StaticClass(); }
UClass* MaterialTrack() { return UMovieSceneWidgetMaterialTrack::StaticClass(); }

const FTrackType TrackTypes[] = {
	{TEXT("Float"), &FloatTrack}, {TEXT("Double"), &DoubleTrack}, {TEXT("Bool"), &BoolTrack}, {TEXT("Byte"), &ByteTrack},
	{TEXT("Enum"), &EnumTrack}, {TEXT("Integer"), &IntegerTrack}, {TEXT("Color"), &ColorTrack},
	{TEXT("Transform"), &TransformTrack}, {TEXT("Margin"), &MarginTrack}, {TEXT("Material"), &MaterialTrack},
};

FString TrackTypeName(const UClass* Class)
{
	for (const FTrackType& Type : TrackTypes)
	{
		if (Type.Class() == Class)
		{
			return Type.Name;
		}
	}
	return Class->GetPathName();
}

UClass* ResolveTrackClass(const FString& Type)
{
	for (const FTrackType& Type_ : TrackTypes)
	{
		if (Type.Equals(Type_.Name, ESearchCase::IgnoreCase))
		{
			return Type_.Class();
		}
	}
	UClass* Class = FindObject<UClass>(nullptr, *Type);
	return Class != nullptr && Class->IsChildOf(UMovieScenePropertyTrack::StaticClass()) && !Class->HasAnyClassFlags(CLASS_Abstract) ? Class : nullptr;
}

// A material track's brush property path ("Brush", "WidgetStyle.Normal"), as a track address property.
FString BrushPath(const UMovieSceneWidgetMaterialTrack& Track)
{
	return FString::JoinBy(Track.GetBrushPropertyNamePath(), TEXT("."), [](FName Name) { return Name.ToString(); });
}

bool IsStruct(const FProperty* Property, std::initializer_list<UScriptStruct*> Structs)
{
	const FStructProperty* Struct = CastField<FStructProperty>(Property);
	return Struct != nullptr && Algo::Find(Structs, Struct->Struct) != nullptr;
}

// The property kind a track class animates (as the Sequencer's property track editors pick them).
bool IsCompatible(UClass* TrackClass, const FProperty* Leaf, const TCHAR*& OutExpected)
{
	struct FRule
	{
		UClass* Track;
		const TCHAR* Expected;
		bool bOk;
	};
	const FRule Rules[] = {
		{FloatTrack(), TEXT("a float property"), Leaf->IsA<FFloatProperty>()},
		{DoubleTrack(), TEXT("a double property"), Leaf->IsA<FDoubleProperty>()},
		{BoolTrack(), TEXT("a bool property"), Leaf->IsA<FBoolProperty>()},
		{IntegerTrack(), TEXT("an int32 property"), Leaf->IsA<FIntProperty>()},
		{ByteTrack(), TEXT("a byte property"), Leaf->IsA<FByteProperty>()},
		{EnumTrack(), TEXT("an enum class property"), Leaf->IsA<FEnumProperty>()},
		{ColorTrack(), TEXT("a FLinearColor, FSlateColor or FColor property"),
			IsStruct(Leaf, {TBaseStructure<FLinearColor>::Get(), FSlateColor::StaticStruct(), TBaseStructure<FColor>::Get()})},
		{TransformTrack(), TEXT("a FWidgetTransform property"), IsStruct(Leaf, {FWidgetTransform::StaticStruct()})},
		{MarginTrack(), TEXT("a FMargin property"), IsStruct(Leaf, {FMargin::StaticStruct()})},
		{MaterialTrack(), TEXT("a FSlateBrush property"), IsStruct(Leaf, {FSlateBrush::StaticStruct()})},
	};
	for (const FRule& Rule : Rules)
	{
		if (Rule.Track == TrackClass)
		{
			OutExpected = Rule.Expected;
			return Rule.bOk;
		}
	}
	return true;
}

UWidgetAnimation* FindAnimation(UWidgetBlueprint& Blueprint, FName Name)
{
	for (UWidgetAnimation* Animation : Blueprint.Animations)
	{
		if (Animation != nullptr && Animation->GetFName() == Name)
		{
			return Animation;
		}
	}
	return nullptr;
}

UWidgetAnimation* ReadAnimation(FCall& Call, UWidgetBlueprint& Blueprint, FStringView Key)
{
	FString Name;
	if (!Call.Args.String(Key, Name))
	{
		return nullptr;
	}
	UWidgetAnimation* Animation = FindAnimation(Blueprint, FName(*Name));
	if (Animation == nullptr || Animation->GetMovieScene() == nullptr)
	{
		Call.Args.Fail(TEXT("Widget.AnimationNotFound"), FString::Printf(TEXT("No animation named '%s'."), *Name), Key);
		return nullptr;
	}
	return Animation;
}

FValue FrameValue(double Frames)
{
	const double Rounded = FMath::RoundToDouble(Frames);
	return FMath::IsNearlyEqual(Frames, Rounded, 1e-6) ? MakeInt(static_cast<int64>(Rounded)) : MakeNumber(Frames);
}

FValue RateValue(const FFrameRate& Rate)
{
	return FObjectBuilder().AddInt(TEXT("numerator"), Rate.Numerator).AddInt(TEXT("denominator"), Rate.Denominator).Build();
}

// ---- channels ----------------------------------------------------------------------------

const TCHAR* InterpName(ERichCurveInterpMode Mode)
{
	return Mode == RCIM_Linear ? TEXT("Linear") : Mode == RCIM_Constant ? TEXT("Constant") : TEXT("Cubic");
}

const TCHAR* TangentModeName(ERichCurveTangentMode Mode)
{
	switch (Mode)
	{
	case RCTM_User: return TEXT("User");
	case RCTM_Break: return TEXT("Break");
	case RCTM_SmartAuto: return TEXT("SmartAuto");
	default: return TEXT("Auto");
	}
}

template <typename ChannelType>
FValue EncodeCurveChannel(const ChannelType& Channel, const FFrameRate& Tick, const FFrameRate& Display, FString& OutReason)
{
	if (Channel.PreInfinityExtrap != RCCE_Constant || Channel.PostInfinityExtrap != RCCE_Constant)
	{
		OutReason = TEXT("a channel has pre/post-infinity extrapolation");
	}
	const auto Data = Channel.GetData();
	TArray<FValue> Keys;
	for (int32 Index = 0; Index < Data.GetTimes().Num(); ++Index)
	{
		const auto& Value = Data.GetValues()[Index];
		FObjectBuilder Key;
		Key.Add(TEXT("frame"), FrameValue(TicksToDisplay(Data.GetTimes()[Index].Value, Tick, Display)))
			.Add(TEXT("value"), MakeNumber(Value.Value));
		if (Value.InterpMode != RCIM_Linear)
		{
			Key.AddString(TEXT("interp"), InterpName(Value.InterpMode));
		}
		if (Value.InterpMode == RCIM_Cubic && Value.TangentMode != RCTM_Auto)
		{
			FObjectBuilder Tangent;
			Tangent.AddString(TEXT("mode"), TangentModeName(Value.TangentMode));
			if (Value.TangentMode == RCTM_User || Value.TangentMode == RCTM_Break)
			{
				Tangent.Add(TEXT("arrive"), MakeNumber(Value.Tangent.ArriveTangent)).Add(TEXT("leave"), MakeNumber(Value.Tangent.LeaveTangent));
			}
			Key.Add(TEXT("tangent"), Tangent.Build());
		}
		if (Value.Tangent.TangentWeightMode != RCTWM_WeightedNone)
		{
			OutReason = TEXT("a key has weighted tangents");
		}
		Keys.Add(Key.Build());
	}
	FObjectBuilder Out;
	if (Channel.GetDefault().IsSet())
	{
		Out.Add(TEXT("default"), MakeNumber(Channel.GetDefault().GetValue()));
	}
	Out.Add(TEXT("keys"), MakeArray(MoveTemp(Keys)));
	return Out.Build();
}

template <typename ChannelType>
FValue EncodeDiscreteChannel(const ChannelType& Channel, const FFrameRate& Tick, const FFrameRate& Display, TFunctionRef<FValue(int64)> Encode)
{
	const auto Data = Channel.GetData();
	TArray<FValue> Keys;
	for (int32 Index = 0; Index < Data.GetTimes().Num(); ++Index)
	{
		Keys.Add(FObjectBuilder()
			.Add(TEXT("frame"), FrameValue(TicksToDisplay(Data.GetTimes()[Index].Value, Tick, Display)))
			.Add(TEXT("value"), Encode(static_cast<int64>(Data.GetValues()[Index])))
			.Build());
	}
	FObjectBuilder Out;
	if (Channel.GetDefault().IsSet())
	{
		Out.Add(TEXT("default"), Encode(static_cast<int64>(Channel.GetDefault().GetValue())));
	}
	Out.Add(TEXT("keys"), MakeArray(MoveTemp(Keys)));
	return Out.Build();
}

FValue EnumValue(const UEnum* Enum, int64 Raw)
{
	const FString Name = Enum != nullptr ? Enum->GetNameStringByValue(Raw) : FString();
	return Name.IsEmpty() ? MakeInt(Raw) : MakeString(Name);
}

const TCHAR* ChannelKind(FName TypeName)
{
	if (TypeName == FMovieSceneFloatChannel::StaticStruct()->GetFName()) return TEXT("float");
	if (TypeName == FMovieSceneDoubleChannel::StaticStruct()->GetFName()) return TEXT("double");
	if (TypeName == FMovieSceneBoolChannel::StaticStruct()->GetFName()) return TEXT("bool");
	if (TypeName == FMovieSceneByteChannel::StaticStruct()->GetFName()) return TEXT("byte");
	if (TypeName == FMovieSceneIntegerChannel::StaticStruct()->GetFName()) return TEXT("integer");
	return nullptr;
}

// One channel of a section: its entry type and enabled flag.
struct FChannelRef
{
	FName TypeName;
	FMovieSceneChannel* Channel = nullptr;
	FName Name;
	bool bEnabled = true;
};

TArray<FChannelRef> ListChannels(UMovieSceneSection& Section)
{
	TArray<FChannelRef> Out;
	for (const FMovieSceneChannelEntry& Entry : Section.GetChannelProxy().GetAllEntries())
	{
		const TArrayView<FMovieSceneChannel* const> Channels = Entry.GetChannels();
		const TArrayView<const FMovieSceneChannelMetaData> Meta = Entry.GetMetaData();
		for (int32 Index = 0; Index < Channels.Num(); ++Index)
		{
			FChannelRef& Ref = Out.AddDefaulted_GetRef();
			Ref.TypeName = Entry.GetChannelTypeName();
			Ref.Channel = Channels[Index];
			Ref.Name = Meta.IsValidIndex(Index) && !Meta[Index].Name.IsNone() ? Meta[Index].Name
				: FName(Index == 0 ? FString(TEXT("Value")) : FString::Printf(TEXT("Value%d"), Index)); // single-channel sections are unnamed
			Ref.bEnabled = !Meta.IsValidIndex(Index) || Meta[Index].bEnabled;
		}
	}
	return Out;
}

FValue EncodeChannel(const FChannelRef& Ref, const FFrameRate& Tick, const FFrameRate& Display, FString& OutReason)
{
	const TCHAR* Kind = ChannelKind(Ref.TypeName);
	FValue Body;
	if (Kind == nullptr)
	{
		OutReason = FString::Printf(TEXT("channel type %s is not supported"), *Ref.TypeName.ToString());
		return MakeEmptyObject();
	}
	const FString K(Kind);
	if (K == TEXT("float"))
	{
		Body = EncodeCurveChannel(*static_cast<FMovieSceneFloatChannel*>(Ref.Channel), Tick, Display, OutReason);
	}
	else if (K == TEXT("double"))
	{
		Body = EncodeCurveChannel(*static_cast<FMovieSceneDoubleChannel*>(Ref.Channel), Tick, Display, OutReason);
	}
	else if (K == TEXT("bool"))
	{
		Body = EncodeDiscreteChannel(*static_cast<FMovieSceneBoolChannel*>(Ref.Channel), Tick, Display, [](int64 V) { return MakeBool(V != 0); });
	}
	else if (K == TEXT("byte"))
	{
		const FMovieSceneByteChannel& Byte = *static_cast<FMovieSceneByteChannel*>(Ref.Channel);
		const UEnum* Enum = Byte.GetEnum();
		Body = EncodeDiscreteChannel(Byte, Tick, Display, [Enum](int64 V) { return EnumValue(Enum, V); });
	}
	else
	{
		Body = EncodeDiscreteChannel(*static_cast<FMovieSceneIntegerChannel*>(Ref.Channel), Tick, Display, [](int64 V) { return MakeInt(V); });
	}
	FValue Out = FObjectBuilder().AddString(TEXT("name"), Ref.Name.ToString()).AddString(TEXT("kind"), Kind).Build();
	for (int32 Index = 0; Index < Body.ObjectKeys.Num(); ++Index)
	{
		Out.ObjectKeys.Add(Body.ObjectKeys[Index]);
		Out.Elements.Add(Body.Elements[Index]);
	}
	return Out;
}

// The function or custom event an event key calls (its endpoint in the widget Blueprint).
FString EventEndpointName(const FMovieSceneEvent& Event)
{
#if WITH_EDITORONLY_DATA
	const UObject* Endpoint = Event.WeakEndpoint.Get();
	if (const UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(Endpoint))
	{
		return Custom->CustomFunctionName.ToString();
	}
	if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Endpoint); Entry != nullptr && Entry->GetGraph() != nullptr)
	{
		return Entry->GetGraph()->GetName();
	}
#endif
	return Event.Ptrs.Function != nullptr ? Event.Ptrs.Function->GetName() : FString();
}

// A channel of a track AssetDoc cannot edit, read-only: the keys of an editable kind as EncodeChannel writes them,
// string keys and event keys (the function each one calls) with their values, other keys by frame only.
FValue EncodeReadOnlyChannel(const FChannelRef& Ref, const FFrameRate& Tick, const FFrameRate& Display)
{
	FString Ignored;
	if (ChannelKind(Ref.TypeName) != nullptr)
	{
		return EncodeChannel(Ref, Tick, Display, Ignored);
	}
	FObjectBuilder Out;
	Out.AddString(TEXT("name"), Ref.Name.ToString());
	TArray<FValue> Keys;
	auto AddKey = [&Keys, &Tick, &Display](FFrameNumber Time, const FValue* Value)
	{
		FObjectBuilder Key;
		Key.Add(TEXT("frame"), FrameValue(TicksToDisplay(Time.Value, Tick, Display)));
		if (Value != nullptr)
		{
			Key.Add(TEXT("value"), *Value);
		}
		Keys.Add(Key.Build());
	};
	if (Ref.TypeName == FMovieSceneStringChannel::StaticStruct()->GetFName())
	{
		const FMovieSceneStringChannel& Channel = *static_cast<const FMovieSceneStringChannel*>(Ref.Channel);
		const auto Data = Channel.GetData();
		for (int32 Index = 0; Index < Data.GetTimes().Num(); ++Index)
		{
			const FValue Value = MakeString(Data.GetValues()[Index]);
			AddKey(Data.GetTimes()[Index], &Value);
		}
		Out.AddString(TEXT("kind"), TEXT("string"));
		if (Channel.GetDefault().IsSet())
		{
			Out.AddString(TEXT("default"), Channel.GetDefault().GetValue());
		}
	}
	else if (Ref.TypeName == FMovieSceneEventChannel::StaticStruct()->GetFName())
	{
		const FMovieSceneEventChannel& Channel = *static_cast<const FMovieSceneEventChannel*>(Ref.Channel);
		const auto Data = Channel.GetData();
		for (int32 Index = 0; Index < Data.GetTimes().Num(); ++Index)
		{
			const FValue Value = MakeString(EventEndpointName(Data.GetValues()[Index]));
			AddKey(Data.GetTimes()[Index], &Value);
		}
		Out.AddString(TEXT("kind"), TEXT("event"));
	}
	else
	{
		TArray<FFrameNumber> Times;
		Ref.Channel->GetKeys(TRange<FFrameNumber>::All(), &Times, nullptr);
		for (const FFrameNumber& Time : Times)
		{
			AddKey(Time, nullptr);
		}
		Out.AddString(TEXT("kind"), Ref.TypeName.ToString());
	}
	Out.Add(TEXT("keys"), MakeArray(MoveTemp(Keys)));
	return Out.Build();
}

// A material parameter track's parameters, by the kind each one is keyed as.
struct FMaterialParameter
{
	FName Name;
	const TCHAR* Kind;
};

TArray<FMaterialParameter> ListMaterialParameters(const UMovieSceneParameterSection& Section)
{
	TArray<FMaterialParameter> Out;
	for (const FScalarParameterNameAndCurve& Item : Section.GetScalarParameterNamesAndCurves())
	{
		Out.Add({Item.ParameterName, TEXT("Scalar")});
	}
	for (const FBoolParameterNameAndCurve& Item : Section.GetBoolParameterNamesAndCurves())
	{
		Out.Add({Item.ParameterName, TEXT("Bool")});
	}
	for (const FVector2DParameterNameAndCurves& Item : Section.GetVector2DParameterNamesAndCurves())
	{
		Out.Add({Item.ParameterName, TEXT("Vector2D")});
	}
	for (const FVectorParameterNameAndCurves& Item : Section.GetVectorParameterNamesAndCurves())
	{
		Out.Add({Item.ParameterName, TEXT("Vector")});
	}
	for (const FColorParameterNameAndCurves& Item : Section.GetColorParameterNamesAndCurves())
	{
		Out.Add({Item.ParameterName, TEXT("Color")});
	}
	for (const FTransformParameterNameAndCurves& Item : Section.GetTransformParameterNamesAndCurves())
	{
		Out.Add({Item.ParameterName, TEXT("Transform")});
	}
	return Out;
}

bool HasData(const FChannelRef& Ref)
{
	if (Ref.TypeName == FMovieSceneFloatChannel::StaticStruct()->GetFName())
	{
		const FMovieSceneFloatChannel& C = *static_cast<FMovieSceneFloatChannel*>(Ref.Channel);
		return C.GetNumKeys() > 0 || C.GetDefault().IsSet();
	}
	if (Ref.TypeName == FMovieSceneDoubleChannel::StaticStruct()->GetFName())
	{
		const FMovieSceneDoubleChannel& C = *static_cast<FMovieSceneDoubleChannel*>(Ref.Channel);
		return C.GetNumKeys() > 0 || C.GetDefault().IsSet();
	}
	return Ref.Channel->GetNumKeys() > 0;
}

// ---- tracks ------------------------------------------------------------------------------

struct FTrackRef
{
	UMovieSceneTrack* Track = nullptr;
	FGuid Guid;
	FString Widget;
	bool bSlot = false;
	bool bRoot = false;
	bool bDeactivated = false;
	FString Property;
	FString Type;
	FString Reason;
};

const FWidgetAnimationBinding* FindAnimationBinding(const UWidgetAnimation& Animation, const FGuid& Guid)
{
	return Animation.AnimationBindings.FindByPredicate([&Guid](const FWidgetAnimationBinding& Binding) { return Binding.AnimationGuid == Guid; });
}

TArray<FTrackRef> ListTracks(UWidgetAnimation& Animation)
{
	TArray<FTrackRef> Out;
	UMovieScene* MovieScene = Animation.GetMovieScene();
	for (UMovieSceneTrack* Track : MovieScene->GetTracks())
	{
		FTrackRef& Ref = Out.AddDefaulted_GetRef();
		Ref.Track = Track;
		Ref.Type = Track->GetClass()->GetPathName();
		Ref.Reason = TEXT("track is not bound to a widget");
	}
	for (const FMovieSceneBinding& Binding : static_cast<const UMovieScene*>(MovieScene)->GetBindings())
	{
		const FWidgetAnimationBinding* WidgetBinding = FindAnimationBinding(Animation, Binding.GetObjectGuid());
		for (UMovieSceneTrack* Track : Binding.GetTracks())
		{
			FTrackRef& Ref = Out.AddDefaulted_GetRef();
			Ref.Track = Track;
			Ref.Guid = Binding.GetObjectGuid();
			Ref.Type = TrackTypeName(Track->GetClass());
			if (WidgetBinding == nullptr)
			{
				Ref.Reason = TEXT("the possessable has no widget binding");
				continue;
			}
			Ref.bRoot = WidgetBinding->bIsRootWidget;
			Ref.Widget = Ref.bRoot ? FString() : WidgetBinding->WidgetName.ToString();
			Ref.bSlot = !Ref.bRoot && !WidgetBinding->SlotWidgetName.IsNone();
			// Deactivated in the Sequencer (saved with the asset); a muted track is the editor's view only.
			Ref.bDeactivated = Track->IsEvalDisabled(/*bInCheckLocal=*/false);
			if (WidgetBinding->DynamicBinding.Function != nullptr)
			{
				Ref.Reason = TEXT("the binding is dynamic");
			}
			if (const UMovieSceneWidgetMaterialTrack* Material = Cast<UMovieSceneWidgetMaterialTrack>(Track))
			{
				Ref.Property = BrushPath(*Material);
			}
			else if (const UMovieScenePropertyTrack* Property = Cast<UMovieScenePropertyTrack>(Track))
			{
				Ref.Property = Property->GetPropertyPath().ToString();
			}
			else
			{
				Ref.Reason = FString::Printf(TEXT("%s is not a property track"), *Track->GetClass()->GetName());
				continue;
			}
			if (Ref.Reason.IsEmpty() && Track->GetAllSections().Num() != 1)
			{
				Ref.Reason = FString::Printf(TEXT("the track has %d sections"), Track->GetAllSections().Num());
			}
			if (Ref.Reason.IsEmpty() && Track->IsRowEvalDisabled(Track->GetAllSections()[0]->GetRowIndex(), /*bInCheckLocal=*/false))
			{
				Ref.Reason = TEXT("a row of the track is deactivated");
			}
		}
	}
	// Two tracks with one identity cannot be told apart by a document.
	for (FTrackRef& Ref : Out)
	{
		const int32 Same = Algo::CountIf(Out, [&Ref](const FTrackRef& Other)
		{
			return Other.Widget == Ref.Widget && Other.bSlot == Ref.bSlot && Other.bRoot == Ref.bRoot && Other.Property == Ref.Property
				&& Other.Type == Ref.Type;
		});
		if (Same > 1 && Ref.Reason.IsEmpty())
		{
			Ref.Reason = TEXT("another track has the same widget, target, property and type");
		}
	}
	return Out;
}

// A section's Details (Section): its range (open on a side without a bound), Active, When Finished and pre / post
// roll, the ones that differ from a new section's (infinite, active, project default, no roll).
FValue EncodeSectionSettings(const UMovieSceneSection& Section, const UMovieScene& MovieScene)
{
	const FFrameRate Tick = MovieScene.GetTickResolution();
	const FFrameRate Display = MovieScene.GetDisplayRate();
	FObjectBuilder Out;
	const TRange<FFrameNumber> Range = Section.GetRange();
	if (Range.HasLowerBound())
	{
		Out.Add(TEXT("start"), FrameValue(TicksToDisplay(UE::MovieScene::DiscreteInclusiveLower(Range).Value, Tick, Display)));
	}
	if (Range.HasUpperBound())
	{
		Out.Add(TEXT("end"), FrameValue(TicksToDisplay(UE::MovieScene::DiscreteExclusiveUpper(Range).Value, Tick, Display)));
	}
	if (!Section.IsActive())
	{
		Out.AddBool(TEXT("active"), false);
	}
	if (Section.GetCompletionMode() != EMovieSceneCompletionMode::ProjectDefault)
	{
		Out.AddString(TEXT("when_finished"), StaticEnum<EMovieSceneCompletionMode>()->GetNameStringByValue(static_cast<int64>(Section.GetCompletionMode())));
	}
	if (Section.GetPreRollFrames() != 0)
	{
		Out.Add(TEXT("pre_roll"), FrameValue(TicksToDisplay(Section.GetPreRollFrames(), Tick, Display)));
	}
	if (Section.GetPostRollFrames() != 0)
	{
		Out.Add(TEXT("post_roll"), FrameValue(TicksToDisplay(Section.GetPostRollFrames(), Tick, Display)));
	}
	return Out.Build();
}

FValue EncodeTrack(UMovieScene& MovieScene, FTrackRef& Ref)
{
	FObjectBuilder Builder;
	Builder.AddString(TEXT("widget"), Ref.Widget)
		.AddString(TEXT("target"), Ref.bRoot ? TEXT("root") : Ref.bSlot ? TEXT("slot") : TEXT("widget"))
		.AddString(TEXT("property"), Ref.Property)
		.AddString(TEXT("type"), Ref.Type);
	if (Ref.bDeactivated)
	{
		Builder.AddBool(TEXT("deactivated"), true);
	}
	TArray<FValue> Channels;
	if (Ref.Reason.IsEmpty())
	{
		UMovieSceneSection* Section = Ref.Track->GetAllSections()[0];
		FValue SectionSettings = EncodeSectionSettings(*Section, MovieScene);
		if (SectionSettings.ObjectKeys.Num() > 0)
		{
			Builder.Add(TEXT("section"), MoveTemp(SectionSettings));
		}
		if (const UMovieSceneParameterSection* Parameters = Cast<UMovieSceneParameterSection>(Section))
		{
			TArray<FValue> Rows;
			for (const FMaterialParameter& Parameter : ListMaterialParameters(*Parameters))
			{
				if (FCString::Strcmp(Parameter.Kind, TEXT("Transform")) == 0)
				{
					Ref.Reason = FString::Printf(TEXT("transform parameter %s is not supported"), *Parameter.Name.ToString());
				}
				Rows.Add(FObjectBuilder().AddString(TEXT("name"), Parameter.Name.ToString()).AddString(TEXT("kind"), Parameter.Kind).Build());
			}
			if (Ref.Reason.IsEmpty())
			{
				Builder.Add(TEXT("parameters"), MakeArray(MoveTemp(Rows)));
			}
		}
		for (const FChannelRef& Channel : ListChannels(*Section))
		{
			if (!Channel.bEnabled || !HasData(Channel))
			{
				continue;
			}
			Channels.Add(EncodeChannel(Channel, MovieScene.GetTickResolution(), MovieScene.GetDisplayRate(), Ref.Reason));
		}
	}
	Builder.AddBool(TEXT("supported"), Ref.Reason.IsEmpty());
	if (!Ref.Reason.IsEmpty())
	{
		Builder.AddString(TEXT("reason"), Ref.Reason);
		Channels.Reset();
		TArray<FValue> Sections;
		const FFrameRate Tick = MovieScene.GetTickResolution();
		const FFrameRate Display = MovieScene.GetDisplayRate();
		for (UMovieSceneSection* Section : Ref.Track->GetAllSections())
		{
			FObjectBuilder Row;
			const TRange<FFrameNumber> Range = Section->GetRange();
			if (Range.HasLowerBound())
			{
				Row.Add(TEXT("start"), FrameValue(TicksToDisplay(UE::MovieScene::DiscreteInclusiveLower(Range).Value, Tick, Display)));
			}
			if (Range.HasUpperBound())
			{
				Row.Add(TEXT("end"), FrameValue(TicksToDisplay(UE::MovieScene::DiscreteExclusiveUpper(Range).Value, Tick, Display)));
			}
			TArray<FValue> SectionChannels;
			for (const FChannelRef& Channel : ListChannels(*Section))
			{
				if (Channel.Channel->GetNumKeys() > 0 || (ChannelKind(Channel.TypeName) != nullptr && HasData(Channel)))
				{
					SectionChannels.Add(EncodeReadOnlyChannel(Channel, Tick, Display));
				}
			}
			Row.Add(TEXT("channels"), MakeArray(MoveTemp(SectionChannels)));
			Sections.Add(Row.Build());
		}
		Builder.Add(TEXT("sections"), MakeArray(MoveTemp(Sections)));
	}
	Builder.Add(TEXT("channels"), MakeArray(MoveTemp(Channels)));
	return Builder.Build();
}

// ---- handlers ------------------------------------------------------------------------------
} // namespace

bool HandleAnimationQuery(FCall& Call, FObjectBuilder& Result)
{
	UWidgetBlueprint* Blueprint = GetWidgetBlueprint(Call);
	int64 Offset = 0;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("name"), TEXT("track_offset")}) || !Call.Args.Int(TEXT("track_offset"), Offset, false))
	{
		return false;
	}
	if (!Call.Args.Has(TEXT("name")))
	{
		TArray<FValue> Animations;
		for (UWidgetAnimation* Animation : Blueprint->Animations)
		{
			if (Animation != nullptr && Animation->GetMovieScene() != nullptr)
			{
				Animations.Add(FObjectBuilder().AddString(TEXT("name"), Animation->GetName())
					.AddInt(TEXT("tracks"), ListTracks(*Animation).Num()).Build());
			}
		}
		Result.Add(TEXT("animations"), MakeArray(MoveTemp(Animations)));
		return true;
	}
	UWidgetAnimation* Animation = ReadAnimation(Call, *Blueprint, TEXT("name"));
	if (Animation == nullptr)
	{
		return false;
	}
	UMovieScene& MovieScene = *Animation->GetMovieScene();
	const FFrameRate Tick = MovieScene.GetTickResolution();
	const FFrameRate Display = MovieScene.GetDisplayRate();
	const TRange<FFrameNumber> Range = MovieScene.GetPlaybackRange();
	Result.AddString(TEXT("name"), Animation->GetName())
		.Add(TEXT("display_rate"), RateValue(Display))
		.Add(TEXT("playback"), FObjectBuilder()
			.Add(TEXT("start"), FrameValue(TicksToDisplay(UE::MovieScene::DiscreteInclusiveLower(Range).Value, Tick, Display)))
			.Add(TEXT("end"), FrameValue(TicksToDisplay(UE::MovieScene::DiscreteExclusiveUpper(Range).Value, Tick, Display)))
			.Build());
	TArray<FTrackRef> Tracks = ListTracks(*Animation);
	Result.AddInt(TEXT("total_tracks"), Tracks.Num());
	int32 Budget = QueryValueNodeBudget - 20;
	TArray<FValue> Encoded;
	int64 Next = FMath::Max<int64>(Offset, 0);
	for (; Next < Tracks.Num(); ++Next)
	{
		FValue Track = EncodeTrack(MovieScene, Tracks[Next]);
		const int32 Nodes = CountValueNodes(Track);
		if (!Encoded.IsEmpty() && Nodes > Budget)
		{
			break;
		}
		Budget -= Nodes;
		Encoded.Add(MoveTemp(Track));
	}
	Result.Add(TEXT("tracks"), MakeArray(MoveTemp(Encoded)));
	Result.AddInt(TEXT("next_track_offset"), Next < Tracks.Num() ? Next : -1);
	return true;
}

namespace
{
bool ReadRate(FReader& Reader, FFrameRate& Out)
{
	int64 Numerator = 0;
	int64 Denominator = 1;
	if (!Reader.OnlyFields({TEXT("numerator"), TEXT("denominator")}) || !Reader.Int(TEXT("numerator"), Numerator)
		|| !Reader.Int(TEXT("denominator"), Denominator, false))
	{
		return false;
	}
	if (Numerator <= 0 || Denominator <= 0 || Numerator > 1000000 || Denominator > 1000000)
	{
		return Reader.Fail(TEXT("Widget.InvalidFrameRate"), TEXT("The frame rate needs a positive numerator and denominator."));
	}
	Out = FFrameRate(static_cast<uint32>(Numerator), static_cast<uint32>(Denominator));
	return true;
}

// VerifyAnimationRename (AnimationTabSummoner.cpp) for a new animation name.
bool ValidateAnimationName(FCall& Call, UWidgetBlueprint& Blueprint, FName Name)
{
	const FString Text = Name.ToString();
	if (Text != SlugStringForValidName(Text) || Text.IsEmpty())
	{
		return Call.Args.Fail(TEXT("Widget.InvalidAnimationName"), FString::Printf(TEXT("'%s' contains characters an object name cannot have."), *Text), TEXT("name"));
	}
	if (Blueprint.WidgetTree->FindWidget(Name) != nullptr)
	{
		return Call.Args.Fail(TEXT("Widget.InvalidAnimationName"), FString::Printf(TEXT("A widget is named '%s'."), *Text), TEXT("name"));
	}
	if (UWidgetAnimation* Stale = FindObjectFast<UWidgetAnimation>(&Blueprint, Name))
	{
		// Not in Animations (FindAnimation failed): an unreachable leftover; move it away.
		Stale->Rename(nullptr, GetTransientPackage());
	}
	FProperty* Bound = Blueprint.ParentClass != nullptr ? Blueprint.ParentClass->FindPropertyByName(Name) : nullptr;
	if (Bound != nullptr && FWidgetBlueprintEditorUtils::IsBindWidgetAnimProperty(Bound))
	{
		return true;
	}
	FKismetNameValidator Validator(&Blueprint);
	const EValidatorResult Valid = Validator.IsValid(Text);
	if (Valid != EValidatorResult::Ok)
	{
		return Call.Args.Fail(TEXT("Widget.InvalidAnimationName"), FKismetNameValidator::GetErrorString(Text, Valid), TEXT("name"));
	}
	return true;
}
} // namespace

bool HandleAnimationSet(FCall& Call, FObjectBuilder& Result)
{
	UWidgetBlueprint* Blueprint = GetWidgetBlueprint(Call);
	FString NameText;
	const FValue* RateValuePtr = nullptr;
	const FValue* Playback = nullptr;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("name"), TEXT("display_rate"), TEXT("playback")})
		|| !Call.Args.String(TEXT("name"), NameText) || !Call.Args.Object(TEXT("display_rate"), RateValuePtr, false)
		|| !Call.Args.Object(TEXT("playback"), Playback, false))
	{
		return false;
	}
	const FName Name(*NameText);
	UWidgetAnimation* Animation = FindAnimation(*Blueprint, Name);
	FFrameRate Display = Animation != nullptr && Animation->GetMovieScene() ? Animation->GetMovieScene()->GetDisplayRate() : FFrameRate(20, 1);
	if (RateValuePtr != nullptr)
	{
		FReader Reader = Call.Args.Child(TEXT("display_rate"), *RateValuePtr);
		if (!ReadRate(Reader, Display))
		{
			return false;
		}
	}
	double Start = 0.0;
	double End = 0.0;
	if (Playback != nullptr)
	{
		FReader Reader = Call.Args.Child(TEXT("playback"), *Playback);
		if (!Reader.OnlyFields({TEXT("start"), TEXT("end")}) || !Reader.Number(TEXT("start"), Start) || !Reader.Number(TEXT("end"), End))
		{
			return false;
		}
		if (End < Start)
		{
			return Reader.Fail(TEXT("Graph.InvalidArgument"), TEXT("playback end must not be before start."), TEXT("end"));
		}
	}
	if (Animation == nullptr)
	{
		if (!ValidateAnimationName(Call, *Blueprint, Name))
		{
			return false;
		}
		Animation = Raw::CreateAnimation(*Blueprint, Name);
		Result.AddBool(TEXT("created"), true);
	}
	UMovieScene* MovieScene = Animation->GetMovieScene();
	Animation->Modify();
	MovieScene->Modify();
	if (MovieScene->GetDisplayRate() != Display)
	{
		MovieScene->SetDisplayRate(Display);
	}
	if (Playback != nullptr)
	{
		const FFrameRate Tick = MovieScene->GetTickResolution();
		const TRange<FFrameNumber> Range(FFrameNumber(DisplayToTicks(Start, Tick, Display)), FFrameNumber(DisplayToTicks(End, Tick, Display)));
		if (MovieScene->GetPlaybackRange() != Range)
		{
			MovieScene->SetPlaybackRange(Range);
			MovieScene->GetEditorData().WorkStart = Tick.AsSeconds(Range.GetLowerBoundValue());
			MovieScene->GetEditorData().WorkEnd = Tick.AsSeconds(Range.GetUpperBoundValue());
		}
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	return true;
}

bool HandleAnimationRemove(FCall& Call, FObjectBuilder& Result)
{
	UWidgetBlueprint* Blueprint = GetWidgetBlueprint(Call);
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("name")}))
	{
		return false;
	}
	UWidgetAnimation* Animation = ReadAnimation(Call, *Blueprint, TEXT("name"));
	if (Animation == nullptr)
	{
		return false;
	}
	Raw::DeleteAnimation(*Blueprint, *Animation);
	return true;
}

namespace
{
struct FTrackAddress
{
	UWidgetAnimation* Animation = nullptr;
	UWidget* Widget = nullptr;
	UObject* Bound = nullptr;
	bool bSlot = false;
	bool bRoot = false;
	FName RootName;
	FString Property;
	UClass* TrackClass = nullptr;
	FProperty* Leaf = nullptr;
};

bool ReadTrackAddress(FCall& Call, UWidgetBlueprint& Blueprint, FTrackAddress& Out)
{
	FString WidgetName;
	FString Target = TEXT("widget");
	FString Type;
	Out.Animation = ReadAnimation(Call, Blueprint, TEXT("animation"));
	if (Out.Animation == nullptr || !Call.Args.String(TEXT("target"), Target, false) || !Call.Args.String(TEXT("widget"), WidgetName, Target != TEXT("root"))
		|| !Call.Args.String(TEXT("property"), Out.Property) || !Call.Args.String(TEXT("type"), Type))
	{
		return false;
	}
	if (Target != TEXT("widget") && Target != TEXT("slot") && Target != TEXT("root"))
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("target is widget, slot or root."), TEXT("target"));
	}
	Out.bRoot = Target == TEXT("root");
	if (Out.bRoot)
	{
		// The user widget itself: its properties resolve on the generated class's default object.
		Out.RootName = Blueprint.GetFName();
		Out.Bound = Blueprint.GeneratedClass != nullptr ? Blueprint.GeneratedClass->GetDefaultObject() : nullptr;
		if (Out.Bound == nullptr)
		{
			return Call.Args.Fail(TEXT("Widget.NotFound"), TEXT("The widget Blueprint has no generated class yet; compile it first."), TEXT("target"));
		}
	}
	else
	{
		Out.Widget = FindSourceWidget(Blueprint, FName(*WidgetName));
		if (Out.Widget == nullptr)
		{
			return Call.Args.Fail(TEXT("Widget.NotFound"), FString::Printf(TEXT("No widget named '%s' to animate."), *WidgetName), TEXT("widget"));
		}
		Out.bSlot = Target == TEXT("slot");
		Out.Bound = Out.bSlot ? static_cast<UObject*>(Out.Widget->Slot) : Out.Widget;
		if (Out.Bound == nullptr)
		{
			return Call.Args.Fail(TEXT("Widget.NoSlot"), FString::Printf(TEXT("'%s' has no slot to animate."), *WidgetName), TEXT("target"));
		}
	}
	Out.TrackClass = ResolveTrackClass(Type);
	if (Out.TrackClass == nullptr)
	{
		return Call.Args.Fail(TEXT("Widget.InvalidTrack"), FString::Printf(TEXT("'%s' is not a track type (Float, Double, Bool, Byte, Enum, Integer, Color, Transform, Margin, Material or a property track class path)."), *Type), TEXT("type"));
	}
	if (Out.TrackClass == MaterialTrack() && Out.bSlot)
	{
		return Call.Args.Fail(TEXT("Widget.InvalidTrack"), TEXT("A Material track animates a brush of the widget, not of its slot."), TEXT("target"));
	}
	FResolvedPropertyPath Resolved;
	FString Error;
	if (!ResolvePropertyPath(*Out.Bound, Out.Property, Resolved, Error))
	{
		return Call.Args.Fail(TEXT("Widget.InvalidTrack"), Error, TEXT("property"));
	}
	Out.Leaf = Resolved.Leaf;
	const TCHAR* Expected = TEXT("");
	if (!IsCompatible(Out.TrackClass, Out.Leaf, Expected))
	{
		return Call.Args.Fail(TEXT("Widget.InvalidTrack"), FString::Printf(TEXT("A %s track animates %s; %s is %s."), *Type, Expected,
			*Out.Property, *Out.Leaf->GetCPPType()), TEXT("type"));
	}
	return true;
}

FGuid FindPossessable(UWidgetAnimation& Animation, const FTrackAddress& Address)
{
	const FName WidgetName = Address.bRoot ? NAME_None : Address.Widget->GetFName();
	const FName SlotName = Address.bSlot ? Address.Bound->GetFName() : NAME_None;
	for (const FWidgetAnimationBinding& Binding : Animation.AnimationBindings)
	{
		const bool bSame = Address.bRoot ? Binding.bIsRootWidget
			: Binding.WidgetName == WidgetName && Binding.SlotWidgetName == SlotName && !Binding.bIsRootWidget;
		if (bSame && Animation.GetMovieScene()->FindPossessable(Binding.AnimationGuid) != nullptr)
		{
			return Binding.AnimationGuid;
		}
	}
	return FGuid();
}

// FSequencerUtilities::CreateBinding + UWidgetAnimation::BindPossessableObject; a slot is
// shown under the widget it holds (UWidgetAnimation::GetParentObject).
FGuid FindOrAddPossessable(UWidgetAnimation& Animation, const FTrackAddress& Address)
{
	FGuid Guid = FindPossessable(Animation, Address);
	if (Guid.IsValid())
	{
		return Guid;
	}
	UMovieScene* MovieScene = Animation.GetMovieScene();
	FGuid ParentGuid;
	if (Address.bSlot)
	{
		FTrackAddress WidgetAddress = Address;
		WidgetAddress.bSlot = false;
		WidgetAddress.Bound = Address.Widget;
		ParentGuid = FindOrAddPossessable(Animation, WidgetAddress);
	}
	Guid = MovieScene->AddPossessable(Address.bRoot ? Address.RootName.ToString() : Address.Bound->GetName(), Address.Bound->GetClass());
	if (ParentGuid.IsValid())
	{
		if (FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Guid))
		{
			Possessable->SetParent(ParentGuid, MovieScene);
		}
	}
	FWidgetAnimationBinding Binding;
	Binding.AnimationGuid = Guid;
	// UWidgetAnimation::BindPossessableObject: the user widget itself is the root binding.
	Binding.WidgetName = Address.bRoot ? Address.RootName : Address.Widget->GetFName();
	Binding.SlotWidgetName = Address.bSlot ? Address.Bound->GetFName() : NAME_None;
	Binding.bIsRootWidget = Address.bRoot;
	Animation.AnimationBindings.Add(Binding);
	return Guid;
}

UMovieSceneTrack* FindTrack(UWidgetAnimation& Animation, const FGuid& Guid, const FTrackAddress& Address)
{
	const FMovieSceneBinding* Binding = Guid.IsValid() ? Animation.GetMovieScene()->FindBinding(Guid) : nullptr;
	if (Binding == nullptr)
	{
		return nullptr;
	}
	for (UMovieSceneTrack* Track : Binding->GetTracks())
	{
		const UMovieScenePropertyTrack* Property = Cast<UMovieScenePropertyTrack>(Track);
		const UMovieSceneWidgetMaterialTrack* Material = Cast<UMovieSceneWidgetMaterialTrack>(Track);
		const FString Path = Property != nullptr ? Property->GetPropertyPath().ToString() : Material != nullptr ? BrushPath(*Material) : FString();
		if (Track->GetClass() == Address.TrackClass && (Property != nullptr || Material != nullptr) && Path == Address.Property)
		{
			return Track;
		}
	}
	return nullptr;
}

UEnum* PropertyEnum(const FProperty* Leaf)
{
	if (const FEnumProperty* Enum = CastField<FEnumProperty>(Leaf))
	{
		return Enum->GetEnum();
	}
	if (const FByteProperty* Byte = CastField<FByteProperty>(Leaf))
	{
		return Byte->Enum;
	}
	return nullptr;
}

bool ParseInterp(FReader& Reader, ERichCurveInterpMode& OutInterp, ERichCurveTangentMode& OutTangentMode, float& OutArrive, float& OutLeave)
{
	FString Interp = TEXT("Linear");
	const FValue* Tangent = nullptr;
	if (!Reader.String(TEXT("interp"), Interp, false) || !Reader.Object(TEXT("tangent"), Tangent, false))
	{
		return false;
	}
	OutTangentMode = RCTM_Auto;
	OutArrive = OutLeave = 0.f;
	if (Interp == TEXT("Linear")) OutInterp = RCIM_Linear;
	else if (Interp == TEXT("Constant")) OutInterp = RCIM_Constant;
	else if (Interp == TEXT("Cubic")) OutInterp = RCIM_Cubic;
	else return Reader.Fail(TEXT("Widget.InvalidKey"), TEXT("interp is Linear, Cubic or Constant."), TEXT("interp"));
	if (Tangent == nullptr)
	{
		return true;
	}
	if (OutInterp != RCIM_Cubic)
	{
		return Reader.Fail(TEXT("Widget.InvalidKey"), TEXT("tangent applies to Cubic keys only."), TEXT("tangent"));
	}
	FReader TangentReader = Reader.Child(TEXT("tangent"), *Tangent);
	FString Mode;
	double Arrive = 0.0;
	double Leave = 0.0;
	if (!TangentReader.OnlyFields({TEXT("mode"), TEXT("arrive"), TEXT("leave")}) || !TangentReader.String(TEXT("mode"), Mode)
		|| !TangentReader.Number(TEXT("arrive"), Arrive, false) || !TangentReader.Number(TEXT("leave"), Leave, false))
	{
		return false;
	}
	if (Mode == TEXT("Auto")) OutTangentMode = RCTM_Auto;
	else if (Mode == TEXT("SmartAuto")) OutTangentMode = RCTM_SmartAuto;
	else if (Mode == TEXT("User")) OutTangentMode = RCTM_User;
	else if (Mode == TEXT("Break")) OutTangentMode = RCTM_Break;
	else return TangentReader.Fail(TEXT("Widget.InvalidKey"), TEXT("tangent mode is Auto, SmartAuto, User or Break."), TEXT("mode"));
	OutArrive = static_cast<float>(Arrive);
	OutLeave = static_cast<float>(Leave);
	return true;
}

struct FKeySpec
{
	int32 Tick = 0;
	double Number = 0.0;
	int64 Integer = 0;
	bool bBool = false;
	ERichCurveInterpMode Interp = RCIM_Linear;
	ERichCurveTangentMode TangentMode = RCTM_Auto;
	float Arrive = 0.f;
	float Leave = 0.f;
};

struct FChannelSpec
{
	TArray<FKeySpec> Keys;
	bool bHasDefault = false;
	FKeySpec Default;
};

bool ReadScalar(FReader& Reader, FStringView Key, const TCHAR* Kind, const UEnum* Enum, FKeySpec& Out)
{
	const FValue* Value = Reader.Find(Key);
	if (Value == nullptr)
	{
		return Reader.Fail(TEXT("Widget.InvalidKey"), TEXT("A value is required."), Key);
	}
	const FString K(Kind);
	if (K == TEXT("bool"))
	{
		if (Value->Kind != EAssetDocumentRuntimeValueKind::Boolean)
		{
			return Reader.Fail(TEXT("Widget.InvalidKey"), TEXT("bool channel values are true/false."), Key);
		}
		Out.bBool = Value->BooleanValue;
		return true;
	}
	if (K == TEXT("byte") && Value->Kind == EAssetDocumentRuntimeValueKind::String)
	{
		const int64 Raw = Enum != nullptr ? Enum->GetValueByNameString(Value->StringValue) : INDEX_NONE;
		if (Raw == INDEX_NONE)
		{
			return Reader.Fail(TEXT("Widget.InvalidKey"), FString::Printf(TEXT("'%s' is not a %s value."), *Value->StringValue, Enum ? *Enum->GetName() : TEXT("byte")), Key);
		}
		Out.Integer = Raw;
		return true;
	}
	double Number = 0.0;
	if (!Reader.Number(Key, Number))
	{
		return false;
	}
	Out.Number = Number;
	Out.Integer = static_cast<int64>(FMath::RoundToDouble(Number));
	if ((K == TEXT("byte") || K == TEXT("integer")) && static_cast<double>(Out.Integer) != Number)
	{
		return Reader.Fail(TEXT("Widget.InvalidKey"), TEXT("Integer channel values are whole numbers."), Key);
	}
	return true;
}

bool ReadChannelSpec(FReader& Reader, const TCHAR* Kind, const UEnum* Enum, const FFrameRate& Tick, const FFrameRate& Display, FChannelSpec& Out)
{
	const FValue* Keys = nullptr;
	if (!Reader.OnlyFields({TEXT("name"), TEXT("keys"), TEXT("default")}) || !Reader.Array(TEXT("keys"), Keys, false))
	{
		return false;
	}
	const bool bCurve = FString(Kind) == TEXT("float") || FString(Kind) == TEXT("double");
	if (Reader.Has(TEXT("default")))
	{
		Out.bHasDefault = true;
		if (!ReadScalar(Reader, TEXT("default"), Kind, Enum, Out.Default))
		{
			return false;
		}
	}
	if (Keys == nullptr)
	{
		return true;
	}
	for (int32 Index = 0; Index < Keys->Elements.Num(); ++Index)
	{
		FReader KeyReader = Reader.Child(FString::Printf(TEXT("keys/%d"), Index), Keys->Elements[Index]);
		FKeySpec& Key = Out.Keys.AddDefaulted_GetRef();
		double Frame = 0.0;
		if (!KeyReader.OnlyFields({TEXT("frame"), TEXT("value"), TEXT("interp"), TEXT("tangent")}) || !KeyReader.Number(TEXT("frame"), Frame)
			|| !ReadScalar(KeyReader, TEXT("value"), Kind, Enum, Key))
		{
			return false;
		}
		if (bCurve)
		{
			if (!ParseInterp(KeyReader, Key.Interp, Key.TangentMode, Key.Arrive, Key.Leave))
			{
				return false;
			}
		}
		else if (KeyReader.Has(TEXT("interp")) || KeyReader.Has(TEXT("tangent")))
		{
			return KeyReader.Fail(TEXT("Widget.InvalidKey"), TEXT("Only float and double channels interpolate."), TEXT("interp"));
		}
		Key.Tick = DisplayToTicks(Frame, Tick, Display);
		if (Index > 0 && Key.Tick <= Out.Keys[Index - 1].Tick)
		{
			return KeyReader.Fail(TEXT("Widget.InvalidKey"), TEXT("Keys must be in increasing frame order (two keys cannot share a frame)."), TEXT("frame"));
		}
	}
	return true;
}

template <typename ChannelType, typename ValueType>
void WriteCurve(ChannelType& Channel, const FChannelSpec& Spec, const FFrameRate& Tick)
{
	auto Data = Channel.GetData();
	Data.Reset();
	for (const FKeySpec& Key : Spec.Keys)
	{
		ValueType Value;
		Value.Value = static_cast<decltype(Value.Value)>(Key.Number);
		Value.InterpMode = Key.Interp;
		Value.TangentMode = Key.TangentMode;
		Value.Tangent.ArriveTangent = Key.Arrive;
		Value.Tangent.LeaveTangent = Key.Leave;
		Data.AddKey(FFrameNumber(Key.Tick), Value);
	}
	if (Spec.bHasDefault)
	{
		Channel.SetDefault(static_cast<decltype(ValueType().Value)>(Spec.Default.Number));
	}
	else
	{
		Channel.RemoveDefault();
	}
	Channel.SetTickResolution(Tick);
	Channel.AutoSetTangents();
}

template <typename ChannelType, typename ScalarType>
void WriteDiscrete(ChannelType& Channel, const FChannelSpec& Spec, TFunctionRef<ScalarType(const FKeySpec&)> Value)
{
	auto Data = Channel.GetData();
	Data.Reset();
	for (const FKeySpec& Key : Spec.Keys)
	{
		Data.AddKey(FFrameNumber(Key.Tick), Value(Key));
	}
	if (Spec.bHasDefault)
	{
		Channel.SetDefault(Value(Spec.Default));
	}
	else
	{
		Channel.RemoveDefault();
	}
}

// The material parameters a Material track keys, as listed: unlisted ones (or one listed as another kind) are
// removed, and a new one is added with a placeholder key that the channel write then replaces.
bool SyncMaterialParameters(FCall& Call, UMovieSceneParameterSection& Section, const FValue& Wanted)
{
	struct FWanted
	{
		FName Name;
		FString Kind;
	};
	TArray<FWanted> Rows;
	for (int32 Index = 0; Index < Wanted.Elements.Num(); ++Index)
	{
		FReader Reader = Call.Args.Child(FString::Printf(TEXT("parameters/%d"), Index), Wanted.Elements[Index]);
		FString Name;
		FString Kind;
		if (!Reader.OnlyFields({TEXT("name"), TEXT("kind")}) || !Reader.String(TEXT("name"), Name) || !Reader.String(TEXT("kind"), Kind))
		{
			return false;
		}
		if (Kind != TEXT("Scalar") && Kind != TEXT("Bool") && Kind != TEXT("Vector2D") && Kind != TEXT("Vector") && Kind != TEXT("Color"))
		{
			return Reader.Fail(TEXT("Widget.InvalidTrack"), TEXT("A material parameter kind is Scalar, Bool, Vector2D, Vector or Color."), TEXT("kind"));
		}
		if (Name.IsEmpty() || Rows.ContainsByPredicate([&Name](const FWanted& Row) { return Row.Name == FName(*Name); }))
		{
			return Reader.Fail(TEXT("Widget.InvalidTrack"), TEXT("Each material parameter is named once."), TEXT("name"));
		}
		Rows.Add({FName(*Name), Kind});
	}
	for (const FMaterialParameter& Have : ListMaterialParameters(Section))
	{
		if (Rows.ContainsByPredicate([&Have](const FWanted& Row) { return Row.Name == Have.Name && Row.Kind == Have.Kind; }))
		{
			continue;
		}
		const FString Kind(Have.Kind);
		if (Kind == TEXT("Scalar")) Section.RemoveScalarParameter(Have.Name);
		else if (Kind == TEXT("Bool")) Section.RemoveBoolParameter(Have.Name);
		else if (Kind == TEXT("Vector2D")) Section.RemoveVector2DParameter(Have.Name);
		else if (Kind == TEXT("Vector")) Section.RemoveVectorParameter(Have.Name);
		else if (Kind == TEXT("Color")) Section.RemoveColorParameter(Have.Name);
		else Section.RemoveTransformParameter(Have.Name);
	}
	const TArray<FMaterialParameter> Kept = ListMaterialParameters(Section);
	for (const FWanted& Row : Rows)
	{
		if (Kept.ContainsByPredicate([&Row](const FMaterialParameter& Have) { return Have.Name == Row.Name; }))
		{
			continue;
		}
		const FFrameNumber Zero(0);
		if (Row.Kind == TEXT("Scalar")) Section.AddScalarParameterKey(Row.Name, Zero, 0.f);
		else if (Row.Kind == TEXT("Bool")) Section.AddBoolParameterKey(Row.Name, Zero, false);
		else if (Row.Kind == TEXT("Vector2D")) Section.AddVector2DParameterKey(Row.Name, Zero, FVector2D::ZeroVector);
		else if (Row.Kind == TEXT("Vector")) Section.AddVectorParameterKey(Row.Name, Zero, FVector::ZeroVector);
		else Section.AddColorParameterKey(Row.Name, Zero, FLinearColor::White);
	}
	return true;
}

// A section's Details as listed: a missing bound is open (infinite), missing settings are a new section's.
bool WriteSectionSettings(FReader& Reader, UMovieSceneSection& Section, const FFrameRate& Tick, const FFrameRate& Display)
{
	const FValue* Start = Reader.Find(TEXT("start"));
	const FValue* End = Reader.Find(TEXT("end"));
	double StartFrame = 0.0;
	double EndFrame = 0.0;
	double PreRoll = 0.0;
	double PostRoll = 0.0;
	bool bActive = true;
	FString WhenFinished = TEXT("ProjectDefault");
	if (!Reader.OnlyFields({TEXT("start"), TEXT("end"), TEXT("active"), TEXT("when_finished"), TEXT("pre_roll"), TEXT("post_roll")})
		|| (Start != nullptr && !Reader.Number(TEXT("start"), StartFrame)) || (End != nullptr && !Reader.Number(TEXT("end"), EndFrame))
		|| !Reader.Bool(TEXT("active"), bActive, false) || !Reader.String(TEXT("when_finished"), WhenFinished, false)
		|| !Reader.Number(TEXT("pre_roll"), PreRoll, false) || !Reader.Number(TEXT("post_roll"), PostRoll, false))
	{
		return false;
	}
	const int64 Completion = StaticEnum<EMovieSceneCompletionMode>()->GetValueByNameString(WhenFinished);
	if (Completion == INDEX_NONE)
	{
		return Reader.Fail(TEXT("Widget.InvalidTrack"), TEXT("when_finished is KeepState, RestoreState or ProjectDefault."), TEXT("when_finished"));
	}
	if (Start != nullptr && End != nullptr && EndFrame < StartFrame)
	{
		return Reader.Fail(TEXT("Widget.InvalidTrack"), TEXT("The section end must not be before its start."), TEXT("end"));
	}
	if (PreRoll < 0.0 || PostRoll < 0.0)
	{
		return Reader.Fail(TEXT("Widget.InvalidTrack"), TEXT("Pre and post roll are not negative."), TEXT("pre_roll"));
	}
	const TRange<FFrameNumber> Range(
		Start != nullptr ? TRangeBound<FFrameNumber>::Inclusive(FFrameNumber(DisplayToTicks(StartFrame, Tick, Display))) : TRangeBound<FFrameNumber>::Open(),
		End != nullptr ? TRangeBound<FFrameNumber>::Exclusive(FFrameNumber(DisplayToTicks(EndFrame, Tick, Display))) : TRangeBound<FFrameNumber>::Open());
	if (Section.GetRange() != Range)
	{
		Section.SetRange(Range);
	}
	if (Section.IsActive() != bActive)
	{
		Section.SetIsActive(bActive);
	}
	Section.SetCompletionMode(static_cast<EMovieSceneCompletionMode>(Completion));
	Section.SetPreRollFrames(DisplayToTicks(PreRoll, Tick, Display));
	Section.SetPostRollFrames(DisplayToTicks(PostRoll, Tick, Display));
	return true;
}

void WriteChannel(const FChannelRef& Ref, const FChannelSpec& Spec, const FFrameRate& Tick)
{
	const FString Kind(ChannelKind(Ref.TypeName));
	if (Kind == TEXT("float"))
	{
		WriteCurve<FMovieSceneFloatChannel, FMovieSceneFloatValue>(*static_cast<FMovieSceneFloatChannel*>(Ref.Channel), Spec, Tick);
	}
	else if (Kind == TEXT("double"))
	{
		WriteCurve<FMovieSceneDoubleChannel, FMovieSceneDoubleValue>(*static_cast<FMovieSceneDoubleChannel*>(Ref.Channel), Spec, Tick);
	}
	else if (Kind == TEXT("bool"))
	{
		WriteDiscrete<FMovieSceneBoolChannel, bool>(*static_cast<FMovieSceneBoolChannel*>(Ref.Channel), Spec, [](const FKeySpec& Key) { return Key.bBool; });
	}
	else if (Kind == TEXT("byte"))
	{
		WriteDiscrete<FMovieSceneByteChannel, uint8>(*static_cast<FMovieSceneByteChannel*>(Ref.Channel), Spec,
			[](const FKeySpec& Key) { return static_cast<uint8>(Key.Integer); });
	}
	else
	{
		WriteDiscrete<FMovieSceneIntegerChannel, int32>(*static_cast<FMovieSceneIntegerChannel*>(Ref.Channel), Spec,
			[](const FKeySpec& Key) { return static_cast<int32>(Key.Integer); });
	}
}
} // namespace

bool HandleAnimationTrackSet(FCall& Call, FObjectBuilder& Result)
{
	UWidgetBlueprint* Blueprint = GetWidgetBlueprint(Call);
	const FValue* Channels = nullptr;
	const FValue* ParametersArg = nullptr;
	const FValue* SectionArg = nullptr;
	bool bDeactivated = false;
	if (Blueprint == nullptr
		|| !Call.Args.OnlyFields({TEXT("animation"), TEXT("widget"), TEXT("target"), TEXT("property"), TEXT("type"), TEXT("deactivated"),
			TEXT("parameters"), TEXT("section"), TEXT("channels")})
		|| !Call.Args.Array(TEXT("channels"), Channels) || !Call.Args.Array(TEXT("parameters"), ParametersArg, false)
		|| !Call.Args.Object(TEXT("section"), SectionArg, false) || !Call.Args.Bool(TEXT("deactivated"), bDeactivated, false))
	{
		return false;
	}
	FTrackAddress Address;
	if (!ReadTrackAddress(Call, *Blueprint, Address))
	{
		return false;
	}
	UWidgetAnimation& Animation = *Address.Animation;
	UMovieScene* MovieScene = Animation.GetMovieScene();
	const FFrameRate Tick = MovieScene->GetTickResolution();
	const FFrameRate Display = MovieScene->GetDisplayRate();
	UMovieSceneTrack* Track = FindTrack(Animation, FindPossessable(Animation, Address), Address);
	if (Track != nullptr && Track->GetAllSections().Num() != 1)
	{
		return Call.Args.Fail(TEXT("Widget.TrackReadOnly"), FString::Printf(TEXT("The %s track of %s has %d sections; edit it in the Sequencer."),
			*Address.Property, *Address.Widget->GetName(), Track->GetAllSections().Num()), TEXT("property"));
	}

	Animation.Modify();
	MovieScene->Modify();
	UMovieSceneSection* Section = nullptr;
	if (Track == nullptr)
	{
		const FGuid Guid = FindOrAddPossessable(Animation, Address);
		Track = MovieScene->AddTrack(Address.TrackClass, Guid);
		UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track);
		UMovieSceneWidgetMaterialTrack* MaterialTrackObject = Cast<UMovieSceneWidgetMaterialTrack>(Track);
		if (PropertyTrack == nullptr && MaterialTrackObject == nullptr)
		{
			return Call.Args.Fail(TEXT("Widget.InvalidTrack"), TEXT("The track could not be created."), TEXT("type"));
		}
		if (MaterialTrackObject != nullptr)
		{
			// FWidgetBlueprintEditor::AddMaterialTrack: the brush's property name path, displayed by the brush's name.
			TArray<FString> Parts;
			Address.Property.ParseIntoArray(Parts, TEXT("."));
			TArray<FName> NamePath;
			for (const FString& Part : Parts)
			{
				NamePath.Add(FName(*Part));
			}
			MaterialTrackObject->SetBrushPropertyNamePath(NamePath);
			MaterialTrackObject->SetDisplayName(Address.Leaf->GetDisplayNameText());
		}
		else
		{
			PropertyTrack->SetPropertyNameAndPath(Address.Leaf->GetFName(), Address.Property);
		}
		if (UMovieSceneEnumTrack* Enum = Cast<UMovieSceneEnumTrack>(Track))
		{
			Enum->SetEnum(PropertyEnum(Address.Leaf));
		}
		else if (UMovieSceneByteTrack* Byte = Cast<UMovieSceneByteTrack>(Track))
		{
			Byte->SetEnum(PropertyEnum(Address.Leaf));
		}
		Section = Track->CreateNewSection();
		Section->SetRange(TRange<FFrameNumber>::All());
		Track->AddSection(*Section);
	}
	else
	{
		Track->Modify();
		Section = Track->GetAllSections()[0];
	}
	Section->Modify();
	if (Call.Args.Has(TEXT("deactivated")) && Track->IsEvalDisabled(/*bInCheckLocal=*/false) != bDeactivated)
	{
		Track->SetEvalDisabled(bDeactivated);
	}
	if (UMovieSceneParameterSection* Parameters = Cast<UMovieSceneParameterSection>(Section); Parameters != nullptr && ParametersArg != nullptr)
	{
		if (!SyncMaterialParameters(Call, *Parameters, *ParametersArg))
		{
			return false;
		}
	}

	// Every listed channel must exist; the 2D transform mask follows the listed channels.
	TArray<FChannelRef> Live = ListChannels(*Section);
	TMap<FName, FChannelSpec> Specs;
	for (int32 Index = 0; Index < Channels->Elements.Num(); ++Index)
	{
		FReader Reader = Call.Args.Child(FString::Printf(TEXT("channels/%d"), Index), Channels->Elements[Index]);
		FString Name;
		if (!Reader.String(TEXT("name"), Name))
		{
			return false;
		}
		const FChannelRef* Ref = Live.FindByPredicate([&Name](const FChannelRef& Candidate) { return Candidate.Name.ToString() == Name; });
		if (Ref == nullptr || ChannelKind(Ref->TypeName) == nullptr)
		{
			return Reader.Fail(TEXT("Widget.InvalidTrack"), FString::Printf(TEXT("The %s track has no channel '%s'. Channels: %s."), *TrackTypeName(Address.TrackClass), *Name,
				*FString::JoinBy(Live, TEXT(", "), [](const FChannelRef& Candidate) { return Candidate.Name.ToString(); })), TEXT("name"));
		}
		const UEnum* Enum = Ref->TypeName == FMovieSceneByteChannel::StaticStruct()->GetFName()
			? static_cast<FMovieSceneByteChannel*>(Ref->Channel)->GetEnum() : nullptr;
		FChannelSpec& Spec = Specs.Add(Ref->Name);
		if (!ReadChannelSpec(Reader, ChannelKind(Ref->TypeName), Enum, Tick, Display, Spec))
		{
			return false;
		}
	}
	if (UMovieScene2DTransformSection* Transform = Cast<UMovieScene2DTransformSection>(Section))
	{
		EMovieScene2DTransformChannel Mask = EMovieScene2DTransformChannel::None;
		for (const TPair<FName, FChannelSpec>& Spec : Specs)
		{
			Mask |= Transform->GetMaskByName(Spec.Key).GetChannels();
		}
		Transform->SetMask(FMovieScene2DTransformMask(Mask));
		Live = ListChannels(*Section);
	}
	const FChannelSpec Empty;
	for (const FChannelRef& Ref : Live)
	{
		if (ChannelKind(Ref.TypeName) != nullptr)
		{
			const FChannelSpec* Spec = Specs.Find(Ref.Name);
			WriteChannel(Ref, Spec != nullptr ? *Spec : Empty, Tick);
		}
	}
	if (SectionArg != nullptr)
	{
		FReader Reader = Call.Args.Child(TEXT("section"), *SectionArg);
		if (!WriteSectionSettings(Reader, *Section, Tick, Display))
		{
			return false;
		}
	}
	else
	{
		// As keying in the Sequencer does: the section grows to cover its keys.
		for (const FChannelRef& Ref : Live)
		{
			TArray<FFrameNumber> Times;
			Ref.Channel->GetKeys(TRange<FFrameNumber>::All(), &Times, nullptr);
			for (const FFrameNumber& Time : Times)
			{
				if (!Section->GetRange().Contains(Time))
				{
					Section->ExpandToFrame(Time);
				}
			}
		}
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	return true;
}

// Addressed by identity only: the widget may already be gone from the tree (deleting a
// widget leaves its animation tracks behind, as in the editor).
bool HandleAnimationTrackRemove(FCall& Call, FObjectBuilder& Result)
{
	UWidgetBlueprint* Blueprint = GetWidgetBlueprint(Call);
	FString WidgetName;
	FString Target = TEXT("widget");
	FString Property;
	FString Type;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("animation"), TEXT("widget"), TEXT("target"), TEXT("property"), TEXT("type")})
		|| !Call.Args.String(TEXT("target"), Target, false) || !Call.Args.String(TEXT("widget"), WidgetName, Target != TEXT("root"))
		|| !Call.Args.String(TEXT("property"), Property) || !Call.Args.String(TEXT("type"), Type))
	{
		return false;
	}
	UWidgetAnimation* Animation = ReadAnimation(Call, *Blueprint, TEXT("animation"));
	if (Animation == nullptr)
	{
		return false;
	}
	UClass* TrackClass = ResolveTrackClass(Type);
	const TArray<FTrackRef> Tracks = ListTracks(*Animation);
	const FTrackRef* Found = Tracks.FindByPredicate([&](const FTrackRef& Ref)
	{
		return Ref.Guid.IsValid() && Ref.Widget == WidgetName && Ref.bSlot == (Target == TEXT("slot")) && Ref.bRoot == (Target == TEXT("root"))
			&& Ref.Property == Property && Ref.Track->GetClass() == TrackClass;
	});
	if (Found == nullptr)
	{
		return Call.Args.Fail(TEXT("Widget.TrackNotFound"), FString::Printf(TEXT("%s has no %s track for %s.%s."), *Animation->GetName(),
			*Type, *WidgetName, *Property), TEXT("property"));
	}
	UMovieScene* MovieScene = Animation->GetMovieScene();
	const FGuid Guid = Found->Guid;
	Animation->Modify();
	MovieScene->Modify();
	MovieScene->RemoveTrack(*Found->Track);
	// Drop the possessables this left empty (the slot first, then its widget).
	auto PruneIfEmpty = [Animation, MovieScene](const FGuid& Candidate)
	{
		const FMovieSceneBinding* Binding = static_cast<const UMovieScene*>(MovieScene)->FindBinding(Candidate);
		bool bHasChildren = false;
		for (int32 Index = 0; Index < MovieScene->GetPossessableCount(); ++Index)
		{
			bHasChildren |= MovieScene->GetPossessable(Index).GetParent() == Candidate;
		}
		if (Binding != nullptr && Binding->GetTracks().IsEmpty() && !bHasChildren)
		{
			MovieScene->RemovePossessable(Candidate);
			Animation->AnimationBindings.RemoveAll([&Candidate](const FWidgetAnimationBinding& Existing) { return Existing.AnimationGuid == Candidate; });
		}
	};
	const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Guid);
	const FGuid Parent = Possessable != nullptr ? Possessable->GetParent() : FGuid();
	PruneIfEmpty(Guid);
	if (Parent.IsValid())
	{
		PruneIfEmpty(Parent);
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	return true;
}
} // namespace UE::AssetDocument::Graph::Widget
