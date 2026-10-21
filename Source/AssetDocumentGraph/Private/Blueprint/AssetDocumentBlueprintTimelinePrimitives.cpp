// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Timeline primitives (provider "blueprint"). A timeline is a UTimelineTemplate in
// UBlueprint::Timelines plus the K2Node_Timeline that owns it; they are created and
// removed together (deleting the node deletes the template, UK2Node_Timeline::DestroyNode).
// Settings and tracks are the Timeline editor's (STimelineEditor) edits; keys are the
// curve editor's. Only Blueprints that support timelines (actor based, with event graphs:
// FBlueprintEditorUtils::DoesSupportTimelines) accept them.
//
//   blueprint.member.timeline.set.v1 {name, graph?, position?: {x, y}, length?, use_last_keyframe?,
//       auto_play?, loop?, replicated?, ignore_time_dilation?, tick_group?,
//       tracks?: [{name, type: "event"|"float"|"vector"|"linear_color", curve?: <asset path>,
//                  channels?: [[{time, value?, interp_mode?, tangent_mode?, tangent_weight_mode?,
//                                arrive_tangent?, arrive_tangent_weight?, leave_tangent?, leave_tangent_weight?}]]}]}
//       -> {created?}. graph/position place a new timeline node (default: the first event graph).
//       "tracks" is the complete, ordered track list; a track with "curve" uses that external curve
//       asset, otherwise an internal curve whose channels (1 for event/float, X/Y/Z, R/G/B/A) get the keys.
//   blueprint.member.timeline.remove.v1 {name}
//   query "timelines": [{name, graph, length, use_last_keyframe, auto_play, loop, replicated,
//       ignore_time_dilation, tick_group, tracks: [{name, type, curve, channels?}]}]

#include "Blueprint/AssetDocumentBlueprintMemberExtensions.h"
#include "Blueprint/AssetDocumentBlueprintMemberRawEdits.h"

#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "Curves/RichCurve.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/TimelineTemplate.h"
#include "K2Node_Timeline.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/SoftObjectPath.h"

namespace UE::AssetDocument::Graph::BlueprintTimelines
{
namespace
{
// UK2Node_Timeline's fixed pins: a track cannot take one of these names (STimelineEditor::OnVerifyTrackNameCommit).
const TCHAR* const FixedPinNames[] = {TEXT("Play"), TEXT("PlayFromStart"), TEXT("Stop"), TEXT("Reverse"),
	TEXT("ReverseFromEnd"), TEXT("Update"), TEXT("Finished"), TEXT("SetNewTime"), TEXT("NewTime"), TEXT("Direction")};

struct FTypeName
{
	FTTTrackBase::ETrackType Type;
	const TCHAR* Name;
	int32 Channels;
};
const FTypeName TrackTypes[] = {
	{FTTTrackBase::TT_Event, TEXT("event"), 1},
	{FTTTrackBase::TT_FloatInterp, TEXT("float"), 1},
	{FTTTrackBase::TT_VectorInterp, TEXT("vector"), 3},
	{FTTTrackBase::TT_LinearColorInterp, TEXT("linear_color"), 4},
};

const FTypeName* FindType(int32 Type)
{
	for (const FTypeName& Item : TrackTypes)
	{
		if (Item.Type == Type)
		{
			return &Item;
		}
	}
	return nullptr;
}

const FTypeName* FindType(const FString& Name)
{
	for (const FTypeName& Item : TrackTypes)
	{
		if (Name == Item.Name)
		{
			return &Item;
		}
	}
	return nullptr;
}

UClass* CurveClassFor(FTTTrackBase::ETrackType Type)
{
	switch (Type)
	{
	case FTTTrackBase::TT_VectorInterp: return UCurveVector::StaticClass();
	case FTTTrackBase::TT_LinearColorInterp: return UCurveLinearColor::StaticClass();
	default: return UCurveFloat::StaticClass();
	}
}

// The FRichCurve channels of a track curve (1 for float/event, 3 for vector, 4 for linear color).
TArray<FRichCurve*> Channels(UCurveBase* Curve)
{
	TArray<FRichCurve*> Out;
	if (UCurveFloat* Float = Cast<UCurveFloat>(Curve))
	{
		Out.Add(&Float->FloatCurve);
	}
	else if (UCurveVector* Vector = Cast<UCurveVector>(Curve))
	{
		for (FRichCurve& Channel : Vector->FloatCurves)
		{
			Out.Add(&Channel);
		}
	}
	else if (UCurveLinearColor* Color = Cast<UCurveLinearColor>(Curve))
	{
		for (FRichCurve& Channel : Color->FloatCurves)
		{
			Out.Add(&Channel);
		}
	}
	return Out;
}

template <typename EnumType>
FString EnumName(EnumType Value)
{
	return StaticEnum<EnumType>()->GetNameStringByValue(static_cast<int64>(Value));
}

template <typename EnumType>
bool ParseEnum(FReader& Reader, const TCHAR* Key, EnumType& Out)
{
	FString Text;
	if (!Reader.String(Key, Text, false))
	{
		return false;
	}
	if (!Reader.Has(Key))
	{
		return true;
	}
	const UEnum* Enum = StaticEnum<EnumType>();
	const int64 Value = Enum->GetValueByNameString(Text, EGetByNameFlags::None);
	if (Value == INDEX_NONE || Enum->HasMetaData(TEXT("Hidden"), Enum->GetIndexByValue(Value)))
	{
		return Reader.Fail(TEXT("BlueprintMember.InvalidValue"), FString::Printf(TEXT("'%s' is not a %s value."), *Text, *Enum->GetName()), Key);
	}
	Out = static_cast<EnumType>(Value);
	return true;
}

FValue EncodeKey(const FRichCurveKey& Key)
{
	return FObjectBuilder()
		.Add(TEXT("time"), MakeNumber(Key.Time))
		.Add(TEXT("value"), MakeNumber(Key.Value))
		.AddString(TEXT("interp_mode"), EnumName(Key.InterpMode.GetValue()))
		.AddString(TEXT("tangent_mode"), EnumName(Key.TangentMode.GetValue()))
		.AddString(TEXT("tangent_weight_mode"), EnumName(Key.TangentWeightMode.GetValue()))
		.Add(TEXT("arrive_tangent"), MakeNumber(Key.ArriveTangent))
		.Add(TEXT("arrive_tangent_weight"), MakeNumber(Key.ArriveTangentWeight))
		.Add(TEXT("leave_tangent"), MakeNumber(Key.LeaveTangent))
		.Add(TEXT("leave_tangent_weight"), MakeNumber(Key.LeaveTangentWeight))
		.Build();
}

bool ReadKeys(FCall& Call, const FValue& Array, const FString& Path, TArray<FRichCurveKey>& Out)
{
	if (Array.Kind != EAssetDocumentRuntimeValueKind::Array)
	{
		FReader Reader(Array, Path, Call.Error);
		return Reader.Fail(TEXT("Graph.InvalidArgument"), TEXT("A channel is an array of keys."));
	}
	for (int32 Index = 0; Index < Array.Elements.Num(); ++Index)
	{
		FReader Reader(Array.Elements[Index], FString::Printf(TEXT("%s/%d"), *Path, Index), Call.Error);
		if (!Reader.OnlyFields({TEXT("time"), TEXT("value"), TEXT("interp_mode"), TEXT("tangent_mode"), TEXT("tangent_weight_mode"),
				TEXT("arrive_tangent"), TEXT("arrive_tangent_weight"), TEXT("leave_tangent"), TEXT("leave_tangent_weight")}))
		{
			return false;
		}
		FRichCurveKey Key;
		double Time = 0.0, Value = 0.0, Arrive = 0.0, ArriveWeight = 0.0, Leave = 0.0, LeaveWeight = 0.0;
		ERichCurveInterpMode Interp = RCIM_Linear;
		ERichCurveTangentMode Tangent = RCTM_Auto;
		ERichCurveTangentWeightMode Weight = RCTWM_WeightedNone;
		if (!Reader.Number(TEXT("time"), Time) || !Reader.Number(TEXT("value"), Value, false)
			|| !Reader.Number(TEXT("arrive_tangent"), Arrive, false) || !Reader.Number(TEXT("arrive_tangent_weight"), ArriveWeight, false)
			|| !Reader.Number(TEXT("leave_tangent"), Leave, false) || !Reader.Number(TEXT("leave_tangent_weight"), LeaveWeight, false)
			|| !ParseEnum(Reader, TEXT("interp_mode"), Interp) || !ParseEnum(Reader, TEXT("tangent_mode"), Tangent)
			|| !ParseEnum(Reader, TEXT("tangent_weight_mode"), Weight))
		{
			return false;
		}
		Key.Time = static_cast<float>(Time);
		Key.Value = static_cast<float>(Value);
		Key.InterpMode = Interp;
		Key.TangentMode = Tangent;
		Key.TangentWeightMode = Weight;
		Key.ArriveTangent = static_cast<float>(Arrive);
		Key.ArriveTangentWeight = static_cast<float>(ArriveWeight);
		Key.LeaveTangent = static_cast<float>(Leave);
		Key.LeaveTangentWeight = static_cast<float>(LeaveWeight);
		if (Out.Num() > 0 && Key.Time <= Out.Last().Time)
		{
			return Reader.Fail(TEXT("BlueprintMember.InvalidTrack"), TEXT("Key times must be strictly ascending."), TEXT("time"));
		}
		Out.Add(Key);
	}
	return true;
}

bool KeysEqual(const FRichCurve& Curve, const TArray<FRichCurveKey>& Keys)
{
	const TArray<FRichCurveKey>& Have = Curve.GetConstRefOfKeys();
	if (Have.Num() != Keys.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Keys.Num(); ++Index)
	{
		const FRichCurveKey& A = Have[Index];
		const FRichCurveKey& B = Keys[Index];
		const bool bAutoTangents = (B.TangentMode == RCTM_Auto || B.TangentMode == RCTM_SmartAuto) && B.InterpMode == RCIM_Cubic;
		if (A.Time != B.Time || A.Value != B.Value || A.InterpMode != B.InterpMode || A.TangentMode != B.TangentMode
			|| A.TangentWeightMode != B.TangentWeightMode
			|| (!bAutoTangents && (A.ArriveTangent != B.ArriveTangent || A.LeaveTangent != B.LeaveTangent))
			|| A.ArriveTangentWeight != B.ArriveTangentWeight || A.LeaveTangentWeight != B.LeaveTangentWeight)
		{
			return false;
		}
	}
	return true;
}

struct FTrackRequest
{
	FName Name;
	const FTypeName* Type = nullptr;
	UCurveBase* External = nullptr;
	bool bHasChannels = false;
	TArray<TArray<FRichCurveKey>> Channels;
	FString Path;
};

bool ReadTracks(FCall& Call, const FValue& Array, TArray<FTrackRequest>& Out)
{
	TSet<FName> Seen;
	for (int32 Index = 0; Index < Array.Elements.Num(); ++Index)
	{
		const FString Path = FString::Printf(TEXT("/tracks/%d"), Index);
		FReader Reader(Array.Elements[Index], Path, Call.Error);
		FString Name, Type, Curve;
		const FValue* ChannelArray = nullptr;
		if (!Reader.OnlyFields({TEXT("name"), TEXT("type"), TEXT("curve"), TEXT("channels")}) || !Reader.String(TEXT("name"), Name)
			|| !Reader.String(TEXT("type"), Type) || !Reader.String(TEXT("curve"), Curve, false)
			|| !Reader.Array(TEXT("channels"), ChannelArray, false))
		{
			return false;
		}
		FTrackRequest& Track = Out.AddDefaulted_GetRef();
		Track.Name = FName(*Name);
		Track.Path = Path;
		Track.Type = FindType(Type);
		if (Track.Type == nullptr)
		{
			return Reader.Fail(TEXT("BlueprintMember.InvalidTrack"), TEXT("type is event, float, vector or linear_color."), TEXT("type"));
		}
		if (Name.IsEmpty() || Seen.Contains(Track.Name))
		{
			return Reader.Fail(TEXT("BlueprintMember.InvalidTrack"), Name.IsEmpty() ? FString(TEXT("A track needs a name."))
				: FString::Printf(TEXT("Track name '%s' is used twice."), *Name), TEXT("name"));
		}
		for (const TCHAR* Fixed : FixedPinNames)
		{
			if (Track.Name == FName(Fixed))
			{
				return Reader.Fail(TEXT("BlueprintMember.InvalidTrack"),
					FString::Printf(TEXT("'%s' is already in use as a default pin of the timeline node."), *Name), TEXT("name"));
			}
		}
		Seen.Add(Track.Name);
		if (!Curve.IsEmpty())
		{
			UClass* CurveClass = CurveClassFor(Track.Type->Type);
			Track.External = Cast<UCurveBase>(FSoftObjectPath(Curve).TryLoad());
			if (Track.External == nullptr || !Track.External->IsA(CurveClass) || Track.External->GetOutermost()->HasAnyPackageFlags(PKG_CompiledIn))
			{
				return Reader.Fail(TEXT("BlueprintMember.InvalidTrack"),
					FString::Printf(TEXT("'%s' is not a %s asset."), *Curve, *CurveClass->GetName()), TEXT("curve"));
			}
			if (ChannelArray != nullptr)
			{
				return Reader.Fail(TEXT("BlueprintMember.InvalidTrack"), TEXT("A track uses an external curve or its own keys, not both."), TEXT("channels"));
			}
		}
		if (ChannelArray != nullptr)
		{
			Track.bHasChannels = true;
			if (ChannelArray->Elements.Num() != Track.Type->Channels)
			{
				return Reader.Fail(TEXT("BlueprintMember.InvalidTrack"), FString::Printf(
					TEXT("A %s track has %d channel(s)."), Track.Type->Name, Track.Type->Channels), TEXT("channels"));
			}
			for (int32 Channel = 0; Channel < ChannelArray->Elements.Num(); ++Channel)
			{
				if (!ReadKeys(Call, ChannelArray->Elements[Channel], FString::Printf(TEXT("%s/channels/%d"), *Path, Channel),
					Track.Channels.AddDefaulted_GetRef()))
				{
					return false;
				}
			}
		}
	}
	return true;
}

int32 FindDisplayTrack(UTimelineTemplate& Timeline, FName Name, int32* OutType = nullptr)
{
	for (int32 Index = 0; Index < Timeline.GetNumDisplayTracks(); ++Index)
	{
		const FTTTrackId Id = Timeline.GetDisplayTrackId(Index);
		FName TrackName;
		switch (static_cast<FTTTrackBase::ETrackType>(Id.TrackType))
		{
		case FTTTrackBase::TT_Event: TrackName = Timeline.EventTracks.IsValidIndex(Id.TrackIndex) ? Timeline.EventTracks[Id.TrackIndex].GetTrackName() : NAME_None; break;
		case FTTTrackBase::TT_FloatInterp: TrackName = Timeline.FloatTracks.IsValidIndex(Id.TrackIndex) ? Timeline.FloatTracks[Id.TrackIndex].GetTrackName() : NAME_None; break;
		case FTTTrackBase::TT_VectorInterp: TrackName = Timeline.VectorTracks.IsValidIndex(Id.TrackIndex) ? Timeline.VectorTracks[Id.TrackIndex].GetTrackName() : NAME_None; break;
		case FTTTrackBase::TT_LinearColorInterp: TrackName = Timeline.LinearColorTracks.IsValidIndex(Id.TrackIndex) ? Timeline.LinearColorTracks[Id.TrackIndex].GetTrackName() : NAME_None; break;
		}
		if (TrackName == Name)
		{
			if (OutType != nullptr)
			{
				*OutType = Id.TrackType;
			}
			return Index;
		}
	}
	return INDEX_NONE;
}

FName DisplayTrackName(UTimelineTemplate& Timeline, int32 Index)
{
	const FTTTrackId Id = Timeline.GetDisplayTrackId(Index);
	switch (static_cast<FTTTrackBase::ETrackType>(Id.TrackType))
	{
	case FTTTrackBase::TT_Event: return Timeline.EventTracks.IsValidIndex(Id.TrackIndex) ? Timeline.EventTracks[Id.TrackIndex].GetTrackName() : NAME_None;
	case FTTTrackBase::TT_FloatInterp: return Timeline.FloatTracks.IsValidIndex(Id.TrackIndex) ? Timeline.FloatTracks[Id.TrackIndex].GetTrackName() : NAME_None;
	case FTTTrackBase::TT_VectorInterp: return Timeline.VectorTracks.IsValidIndex(Id.TrackIndex) ? Timeline.VectorTracks[Id.TrackIndex].GetTrackName() : NAME_None;
	case FTTTrackBase::TT_LinearColorInterp: return Timeline.LinearColorTracks.IsValidIndex(Id.TrackIndex) ? Timeline.LinearColorTracks[Id.TrackIndex].GetTrackName() : NAME_None;
	}
	return NAME_None;
}

// Brings the template's tracks to the requested list (STimelineEditor delete / add / reorder /
// external-internal curve switches, then curve editor key edits). Returns whether the node pins changed.
bool ApplyTracks(FCall& Call, UBlueprint& Blueprint, UTimelineTemplate& Timeline, const TArray<FTrackRequest>& Tracks, bool& bOutChanged)
{
	bool bStructural = false;
	for (int32 Index = Timeline.GetNumDisplayTracks() - 1; Index >= 0; --Index)
	{
		const FName Name = DisplayTrackName(Timeline, Index);
		const int32 Type = Timeline.GetDisplayTrackId(Index).TrackType;
		const bool bKeep = Tracks.ContainsByPredicate([Name, Type](const FTrackRequest& Track) { return Track.Name == Name && Track.Type->Type == Type; });
		if (!bKeep)
		{
			BlueprintRawEdits::RemoveTimelineTrack(Timeline, Index);
			bStructural = true;
		}
	}
	for (const FTrackRequest& Track : Tracks)
	{
		if (FindDisplayTrack(Timeline, Track.Name) == INDEX_NONE)
		{
			if (BlueprintRawEdits::AddTimelineTrack(Blueprint, Timeline, Track.Type->Type, Track.Name, Track.External) == INDEX_NONE)
			{
				Call.Error.Set(TEXT("BlueprintMember.InvalidTrack"), FString::Printf(TEXT("Track '%s' could not be added."), *Track.Name.ToString()), Track.Path);
				return false;
			}
			bStructural = true;
		}
	}
	for (int32 Wanted = 0; Wanted < Tracks.Num(); ++Wanted)
	{
		int32 Current = FindDisplayTrack(Timeline, Tracks[Wanted].Name);
		while (Current > Wanted)
		{
			BlueprintRawEdits::MoveTimelineTrack(Timeline, Current, -1);
			--Current;
			bStructural = true;
		}
	}
	for (int32 Index = 0; Index < Tracks.Num(); ++Index)
	{
		const FTrackRequest& Track = Tracks[Index];
		bool bExternal = false;
		UCurveBase* Curve = BlueprintRawEdits::GetTrackCurve(Timeline, Index, &bExternal);
		if (Track.External != nullptr)
		{
			if (Curve != Track.External || !bExternal)
			{
				BlueprintRawEdits::SetTrackCurve(Timeline, Index, *Track.External, true);
				bOutChanged = true;
			}
			continue;
		}
		if (Curve == nullptr || bExternal)
		{
			// STimelineEdTrack::UseInternalCurve: a new internal curve holding the external curve's keys.
			UCurveBase* Internal = BlueprintRawEdits::CreateTrackCurve(Blueprint, Track.Type->Type);
			if (Internal == nullptr)
			{
				Call.Error.Set(TEXT("BlueprintMember.InvalidTrack"), TEXT("The Blueprint has no generated class to own the track curve."), Track.Path);
				return false;
			}
			if (Curve != nullptr)
			{
				const TArray<FRichCurve*> From = Channels(Curve);
				const TArray<FRichCurve*> To = Channels(Internal);
				for (int32 Channel = 0; Channel < FMath::Min(From.Num(), To.Num()); ++Channel)
				{
					To[Channel]->SetKeys(From[Channel]->GetConstRefOfKeys());
				}
			}
			BlueprintRawEdits::SetTrackCurve(Timeline, Index, *Internal, false);
			Curve = Internal;
			bOutChanged = true;
		}
		if (!Track.bHasChannels)
		{
			continue;
		}
		const TArray<FRichCurve*> Have = Channels(Curve);
		for (int32 Channel = 0; Channel < FMath::Min(Have.Num(), Track.Channels.Num()); ++Channel)
		{
			if (!KeysEqual(*Have[Channel], Track.Channels[Channel]))
			{
				BlueprintRawEdits::SetCurveKeys(*Curve, *Have[Channel], Track.Channels[Channel]);
				bOutChanged = true;
			}
		}
	}
	if (bStructural)
	{
		BlueprintRawEdits::RefreshTimelineNode(Blueprint, Timeline);
		bOutChanged = true;
	}
	return true;
}

UEdGraph* FindUbergraph(UBlueprint& Blueprint, const FString& Name)
{
	for (UEdGraph* Graph : Blueprint.UbergraphPages)
	{
		if (Graph != nullptr && (Name.IsEmpty() || Graph->GetName().Equals(Name, ESearchCase::IgnoreCase)))
		{
			return Graph;
		}
	}
	return nullptr;
}

FVector2D DefaultLocation(const UEdGraph& Graph)
{
	int32 MaxX = TNumericLimits<int32>::Lowest();
	for (const UEdGraphNode* Node : Graph.Nodes)
	{
		if (Node != nullptr)
		{
			MaxX = FMath::Max(MaxX, Node->NodePosX);
		}
	}
	return FVector2D(MaxX == TNumericLimits<int32>::Lowest() ? 0 : MaxX + 400, 0);
}

bool RequireSupport(FCall& Call, UBlueprint& Blueprint)
{
	if (FBlueprintEditorUtils::DoesSupportTimelines(&Blueprint))
	{
		return true;
	}
	Call.Error.Set(TEXT("BlueprintMember.TimelinesUnsupported"), FString::Printf(
		TEXT("%s has no timelines: the editor offers them only in actor-based Blueprints with an event graph."),
		*Blueprint.GetName()), TEXT("/name"));
	return false;
}

bool HandleSet(FCall& Call, FObjectBuilder& Result)
{
	UBlueprint* Blueprint = GetMemberBlueprint(Call);
	FString NameText, GraphName;
	const FValue* Position = nullptr;
	const FValue* TracksValue = nullptr;
	if (Blueprint == nullptr
		|| !Call.Args.OnlyFields({TEXT("name"), TEXT("graph"), TEXT("position"), TEXT("length"), TEXT("use_last_keyframe"),
			TEXT("auto_play"), TEXT("loop"), TEXT("replicated"), TEXT("ignore_time_dilation"), TEXT("tick_group"), TEXT("tracks")})
		|| !Call.Args.String(TEXT("name"), NameText) || !Call.Args.String(TEXT("graph"), GraphName, false)
		|| !Call.Args.Object(TEXT("position"), Position, false) || !Call.Args.Array(TEXT("tracks"), TracksValue, false)
		|| !RequireSupport(Call, *Blueprint))
	{
		return false;
	}
	if (NameText.IsEmpty())
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("Name must not be empty."), TEXT("name"));
	}
	double Length = 0.0;
	bool bUseLastKeyframe = false, bAutoPlay = false, bLoop = false, bReplicated = false, bIgnoreDilation = false;
	ETickingGroup TickGroup = TG_PrePhysics;
	if (!Call.Args.Number(TEXT("length"), Length, false) || !Call.Args.Bool(TEXT("use_last_keyframe"), bUseLastKeyframe)
		|| !Call.Args.Bool(TEXT("auto_play"), bAutoPlay) || !Call.Args.Bool(TEXT("loop"), bLoop)
		|| !Call.Args.Bool(TEXT("replicated"), bReplicated) || !Call.Args.Bool(TEXT("ignore_time_dilation"), bIgnoreDilation)
		|| !ParseEnum(Call.Args, TEXT("tick_group"), TickGroup))
	{
		return false;
	}
	if (Call.Args.Has(TEXT("length")) && !(Length > UE_KINDA_SMALL_NUMBER))
	{
		return Call.Args.Fail(TEXT("BlueprintMember.InvalidValue"), TEXT("Length must be greater than 0 (the Timeline editor ignores other lengths)."), TEXT("length"));
	}
	TArray<FTrackRequest> Tracks;
	if (TracksValue != nullptr && !ReadTracks(Call, *TracksValue, Tracks))
	{
		return false;
	}
	const FName Name(*NameText);
	UTimelineTemplate* Timeline = Blueprint->FindTimelineTemplateByVariableName(Name);
	bool bChanged = false;
	if (Timeline == nullptr)
	{
		UEdGraph* Graph = FindUbergraph(*Blueprint, GraphName);
		if (Graph == nullptr)
		{
			return Call.Args.Fail(TEXT("BlueprintMember.InvalidValue"), GraphName.IsEmpty()
				? FString(TEXT("The Blueprint has no event graph to place the timeline node in."))
				: FString::Printf(TEXT("No event graph named '%s'."), *GraphName), TEXT("graph"));
		}
		if (!ValidateNewMemberName(Call, *Blueprint, Name, TEXT("name")))
		{
			return false;
		}
		FVector2D Location = DefaultLocation(*Graph);
		if (Position != nullptr)
		{
			FReader PositionReader = Call.Args.Child(TEXT("position"), *Position);
			double X = 0.0, Y = 0.0;
			if (!PositionReader.OnlyFields({TEXT("x"), TEXT("y")}) || !PositionReader.Number(TEXT("x"), X) || !PositionReader.Number(TEXT("y"), Y))
			{
				return false;
			}
			Location = FVector2D(X, Y);
		}
		if (BlueprintRawEdits::AddTimeline(*Blueprint, *Graph, Name, Location) == nullptr
			|| (Timeline = Blueprint->FindTimelineTemplateByVariableName(Name)) == nullptr)
		{
			return Call.Args.Fail(TEXT("BlueprintMember.InvalidName"), FString::Printf(TEXT("Timeline '%s' could not be added."), *NameText), TEXT("name"));
		}
		Result.AddBool(TEXT("created"), true);
		bChanged = true;
	}
	using BlueprintRawEdits::ETimelineFlag;
	const auto Flag = [&](const TCHAR* Key, bool bWanted, bool bHave, ETimelineFlag Which)
	{
		if (Call.Args.Has(Key) && bWanted != bHave)
		{
			BlueprintRawEdits::SetTimelineFlag(*Blueprint, *Timeline, Which, bWanted);
			bChanged = true;
		}
	};
	Flag(TEXT("auto_play"), bAutoPlay, Timeline->bAutoPlay, ETimelineFlag::AutoPlay);
	Flag(TEXT("loop"), bLoop, Timeline->bLoop, ETimelineFlag::Loop);
	Flag(TEXT("replicated"), bReplicated, Timeline->bReplicated, ETimelineFlag::Replicated);
	Flag(TEXT("ignore_time_dilation"), bIgnoreDilation, Timeline->bIgnoreTimeDilation, ETimelineFlag::IgnoreTimeDilation);
	if (Call.Args.Has(TEXT("length")) && Timeline->TimelineLength != static_cast<float>(Length))
	{
		BlueprintRawEdits::SetTimelineLength(*Blueprint, *Timeline, static_cast<float>(Length));
		bChanged = true;
	}
	if (Call.Args.Has(TEXT("use_last_keyframe")) && (Timeline->LengthMode == TL_LastKeyFrame) != bUseLastKeyframe)
	{
		BlueprintRawEdits::SetTimelineUseLastKeyframe(*Blueprint, *Timeline, bUseLastKeyframe);
		bChanged = true;
	}
	if (Call.Args.Has(TEXT("tick_group")) && Timeline->TimelineTickGroup != TickGroup)
	{
		BlueprintRawEdits::SetTimelineTickGroup(*Blueprint, *Timeline, TickGroup);
		bChanged = true;
	}
	if (TracksValue != nullptr && !ApplyTracks(Call, *Blueprint, *Timeline, Tracks, bChanged))
	{
		return false;
	}
	if (bChanged)
	{
		NotifyMemberStructural(*Blueprint);
	}
	return true;
}

bool HandleRemove(FCall& Call, FObjectBuilder& Result)
{
	UBlueprint* Blueprint = GetMemberBlueprint(Call);
	FString NameText;
	if (Blueprint == nullptr || !Call.Args.OnlyFields({TEXT("name")}) || !Call.Args.String(TEXT("name"), NameText))
	{
		return false;
	}
	UTimelineTemplate* Timeline = Blueprint->FindTimelineTemplateByVariableName(FName(*NameText));
	if (Timeline == nullptr)
	{
		Call.Error.Set(TEXT("BlueprintMember.TimelineNotFound"), FString::Printf(TEXT("No timeline '%s'."), *NameText), TEXT("/name"));
		return false;
	}
	if (UK2Node_Timeline* Node = FBlueprintEditorUtils::FindNodeForTimeline(Blueprint, Timeline))
	{
		// Deleting the node deletes its template (UK2Node_Timeline::DestroyNode), as in the graph editor.
		FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile=*/true);
	}
	else
	{
		FBlueprintEditorUtils::RemoveTimeline(Blueprint, Timeline, /*bDontRecompile=*/true);
		Timeline->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
	}
	NotifyMemberStructural(*Blueprint);
	return true;
}

FValue EncodeTracks(UTimelineTemplate& Timeline)
{
	TArray<FValue> Tracks;
	for (int32 Index = 0; Index < Timeline.GetNumDisplayTracks(); ++Index)
	{
		const FTypeName* Type = FindType(Timeline.GetDisplayTrackId(Index).TrackType);
		bool bExternal = false;
		UCurveBase* Curve = BlueprintRawEdits::GetTrackCurve(Timeline, Index, &bExternal);
		if (Type == nullptr)
		{
			continue;
		}
		FObjectBuilder Track;
		Track.AddString(TEXT("name"), DisplayTrackName(Timeline, Index).ToString())
			.AddString(TEXT("type"), Type->Name)
			.AddString(TEXT("curve"), bExternal ? ObjectPathOrEmpty(Curve) : FString());
		if (!bExternal)
		{
			TArray<FValue> ChannelValues;
			for (const FRichCurve* Channel : Channels(Curve))
			{
				TArray<FValue> Keys;
				for (const FRichCurveKey& Key : Channel->GetConstRefOfKeys())
				{
					Keys.Add(EncodeKey(Key));
				}
				ChannelValues.Add(MakeArray(MoveTemp(Keys)));
			}
			Track.Add(TEXT("channels"), MakeArray(MoveTemp(ChannelValues)));
		}
		Tracks.Add(Track.Build());
	}
	return MakeArray(MoveTemp(Tracks));
}
} // namespace

FValue Query(UBlueprint& Blueprint)
{
	TArray<FValue> Out;
	for (UTimelineTemplate* Timeline : Blueprint.Timelines)
	{
		if (Timeline == nullptr)
		{
			continue;
		}
		const UK2Node_Timeline* Node = FBlueprintEditorUtils::FindNodeForTimeline(&Blueprint, Timeline);
		const UEdGraph* Graph = Node != nullptr ? Node->GetGraph() : nullptr;
		Out.Add(FObjectBuilder()
			.AddString(TEXT("name"), Timeline->GetVariableName().ToString())
			.AddString(TEXT("graph"), Graph != nullptr ? Graph->GetName() : FString())
			.Add(TEXT("length"), MakeNumber(Timeline->TimelineLength))
			.AddBool(TEXT("use_last_keyframe"), Timeline->LengthMode == TL_LastKeyFrame)
			.AddBool(TEXT("auto_play"), Timeline->bAutoPlay)
			.AddBool(TEXT("loop"), Timeline->bLoop)
			.AddBool(TEXT("replicated"), Timeline->bReplicated)
			.AddBool(TEXT("ignore_time_dilation"), Timeline->bIgnoreTimeDilation)
			.AddString(TEXT("tick_group"), EnumName(Timeline->TimelineTickGroup.GetValue()))
			.Add(TEXT("tracks"), EncodeTracks(*Timeline))
			.Build());
	}
	return MakeArray(MoveTemp(Out));
}

void AppendDescriptors(TArray<FAssetDocumentPrimitiveDescriptor>& Out)
{
	Out.Add(MakeBlueprintMemberDescriptor(TEXT("blueprint.member.timeline.set.v1"), true,
		TEXT("FBlueprintEditorUtils::AddNewTimeline;UTimelineTemplate::AddDisplayTrack;FRichCurve::SetKeys;UK2Node_Timeline::ReconstructNode")));
	Out.Add(MakeBlueprintMemberDescriptor(TEXT("blueprint.member.timeline.remove.v1"), true,
		TEXT("FBlueprintEditorUtils::RemoveNode;UK2Node_Timeline::DestroyNode;FBlueprintEditorUtils::RemoveTimeline")));
}

void AppendBindings(TArray<FBinding>& Out)
{
	Out.Add({TEXT("blueprint.member.timeline.set.v1"), &HandleSet});
	Out.Add({TEXT("blueprint.member.timeline.remove.v1"), &HandleRemove});
}
} // namespace UE::AssetDocument::Graph::BlueprintTimelines
