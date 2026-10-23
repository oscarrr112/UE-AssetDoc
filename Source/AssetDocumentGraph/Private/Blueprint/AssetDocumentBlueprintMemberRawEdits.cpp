// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Blueprint/AssetDocumentBlueprintMemberRawEdits.h"

#include "Curves/CurveBase.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "Curves/RichCurve.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_Timeline.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace UE::AssetDocument::Graph::BlueprintRawEdits
{
bool SupportsEventDispatchers(const UBlueprint& Blueprint)
{
	return Blueprint.SupportsDelegates() && Blueprint.BlueprintType != BPTYPE_Interface
		&& Blueprint.BlueprintType != BPTYPE_MacroLibrary && Blueprint.BlueprintType != BPTYPE_FunctionLibrary;
}

UEdGraph* AddEventDispatcher(UBlueprint& Blueprint, FName Name)
{
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	Blueprint.Modify();
	FEdGraphPinType DelegateType;
	DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
	if (!FBlueprintEditorUtils::AddMemberVariable(&Blueprint, Name, DelegateType))
	{
		return nullptr;
	}
	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(&Blueprint, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (Graph == nullptr)
	{
		FBlueprintEditorUtils::RemoveMemberVariable(&Blueprint, Name);
		return nullptr;
	}
	Graph->bEditable = false;
	Schema->CreateDefaultNodesForGraph(*Graph);
	Schema->CreateFunctionGraphTerminators(*Graph, static_cast<UClass*>(nullptr));
	Schema->AddExtraFunctionFlags(Graph, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
	Schema->MarkFunctionEntryAsEditable(Graph, true);
	Blueprint.DelegateSignatureGraphs.Add(Graph);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
	return Graph;
}

void RemoveEventDispatcher(UBlueprint& Blueprint, UEdGraph& SignatureGraph)
{
	Blueprint.Modify();
	SignatureGraph.Modify();
	FBlueprintEditorUtils::RemoveMemberVariable(&Blueprint, SignatureGraph.GetFName());
	FBlueprintEditorUtils::RemoveGraph(&Blueprint, &SignatureGraph, EGraphRemoveFlags::Recompile);
	// The editor refreshes every live Create Event node; only this Blueprint's can reference its dispatcher.
	TArray<UK2Node_CreateDelegate*> CreateDelegateNodes;
	FBlueprintEditorUtils::GetAllNodesOfClass(&Blueprint, CreateDelegateNodes);
	for (UK2Node_CreateDelegate* Node : CreateDelegateNodes)
	{
		if (IsValid(Node) && IsValid(Node->GetGraph()))
		{
			Node->HandleAnyChange();
		}
	}
}

UK2Node_Timeline* AddTimeline(UBlueprint& Blueprint, UEdGraph& Graph, FName Name, const FVector2D& Location)
{
	if (!FBlueprintEditorUtils::DoesSupportTimelines(&Blueprint))
	{
		return nullptr;
	}
	// UBlueprintNodeSpawner::SpawnEdGraphNode with the timeline customize delegate; the name the
	// customize step would pick (FindUniqueTimelineName) is replaced by the caller's.
	UK2Node_Timeline* Node = NewObject<UK2Node_Timeline>(&Graph);
	Node->CreateNewGuid();
	Node->NodePosX = static_cast<int32>(Location.X);
	Node->NodePosY = static_cast<int32>(Location.Y);
	Node->TimelineName = Name;
	if (FBlueprintEditorUtils::AddNewTimeline(&Blueprint, Name) == nullptr)
	{
		Node->MarkAsGarbage();
		return nullptr;
	}
	Node->ErrorMsg.Empty();
	Node->bHasCompilerMessage = false;
	Node->SetFlags(RF_Transactional);
	Node->AllocateDefaultPins();
	Node->PostPlacedNewNode();
	Graph.Modify();
	Graph.AddNode(Node, /*bFromUI=*/true, /*bSelectNewNode=*/false);
	return Node;
}

namespace
{
UK2Node_Timeline* NodeFor(UBlueprint& Blueprint, UTimelineTemplate& Timeline)
{
	return FBlueprintEditorUtils::FindNodeForTimeline(&Blueprint, &Timeline);
}
} // namespace

void SetTimelineFlag(UBlueprint& Blueprint, UTimelineTemplate& Timeline, ETimelineFlag Flag, bool bValue)
{
	Timeline.Modify();
	UK2Node_Timeline* Node = NodeFor(Blueprint, Timeline);
	if (Node != nullptr)
	{
		Node->Modify();
	}
	switch (Flag)
	{
	case ETimelineFlag::AutoPlay:
		Timeline.bAutoPlay = bValue;
		if (Node) { Node->bAutoPlay = bValue; }
		break;
	case ETimelineFlag::Loop:
		Timeline.bLoop = bValue;
		if (Node) { Node->bLoop = bValue; }
		break;
	case ETimelineFlag::Replicated:
		Timeline.bReplicated = bValue;
		if (Node) { Node->bReplicated = bValue; }
		break;
	case ETimelineFlag::IgnoreTimeDilation:
		Timeline.bIgnoreTimeDilation = bValue;
		if (Node) { Node->bIgnoreTimeDilation = bValue; }
		break;
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(&Blueprint);
}

void SetTimelineLength(UBlueprint& Blueprint, UTimelineTemplate& Timeline, float Length)
{
	Timeline.Modify();
	Timeline.TimelineLength = Length;
	FBlueprintEditorUtils::MarkBlueprintAsModified(&Blueprint);
}

void SetTimelineUseLastKeyframe(UBlueprint& Blueprint, UTimelineTemplate& Timeline, bool bUseLastKeyframe)
{
	Timeline.Modify();
	Timeline.LengthMode = bUseLastKeyframe ? ETimelineLengthMode::TL_LastKeyFrame : ETimelineLengthMode::TL_TimelineLength;
	FBlueprintEditorUtils::MarkBlueprintAsModified(&Blueprint);
}

void SetTimelineTickGroup(UBlueprint& Blueprint, UTimelineTemplate& Timeline, ETickingGroup TickGroup)
{
	Timeline.Modify();
	Timeline.TimelineTickGroup = TickGroup;
	FBlueprintEditorUtils::MarkBlueprintAsModified(&Blueprint);
}

UCurveBase* CreateTrackCurve(UBlueprint& Blueprint, FTTTrackBase::ETrackType Type)
{
	UClass* Owner = Blueprint.GeneratedClass;
	if (Owner == nullptr)
	{
		return nullptr;
	}
	switch (Type)
	{
	case FTTTrackBase::TT_Event:
	{
		UCurveFloat* Curve = NewObject<UCurveFloat>(Owner, NAME_None, RF_Public);
		Curve->bIsEventCurve = true;  // STimelineEditor::CreateNewTrack marks event-track curves
		return Curve;
	}
	case FTTTrackBase::TT_FloatInterp:
		return NewObject<UCurveFloat>(Owner, NAME_None, RF_Public);
	case FTTTrackBase::TT_VectorInterp:
		return NewObject<UCurveVector>(Owner, NAME_None, RF_Public);
	case FTTTrackBase::TT_LinearColorInterp:
		return NewObject<UCurveLinearColor>(Owner, NAME_None, RF_Public);
	}
	return nullptr;
}

int32 AddTimelineTrack(UBlueprint& Blueprint, UTimelineTemplate& Timeline, FTTTrackBase::ETrackType Type, FName Name,
	UCurveBase* ExternalCurve)
{
	UCurveBase* Curve = ExternalCurve != nullptr ? ExternalCurve : CreateTrackCurve(Blueprint, Type);
	if (Curve == nullptr || !Timeline.IsNewTrackNameValid(Name))
	{
		return INDEX_NONE;
	}
	Timeline.Modify();
	FTTTrackId Id;
	Id.TrackType = Type;
	switch (Type)
	{
	case FTTTrackBase::TT_Event:
	{
		UCurveFloat* Float = Cast<UCurveFloat>(Curve);
		if (Float == nullptr)
		{
			return INDEX_NONE;
		}
		Id.TrackIndex = Timeline.EventTracks.Num();
		FTTEventTrack Track;
		Track.SetTrackName(Name, &Timeline);
		Track.CurveKeys = Float;
		Track.bIsExternalCurve = ExternalCurve != nullptr;
		Timeline.EventTracks.Add(Track);
		break;
	}
	case FTTTrackBase::TT_FloatInterp:
	{
		UCurveFloat* Float = Cast<UCurveFloat>(Curve);
		if (Float == nullptr)
		{
			return INDEX_NONE;
		}
		Id.TrackIndex = Timeline.FloatTracks.Num();
		FTTFloatTrack Track;
		Track.SetTrackName(Name, &Timeline);
		Track.CurveFloat = Float;
		Track.bIsExternalCurve = ExternalCurve != nullptr;
		Timeline.FloatTracks.Add(Track);
		break;
	}
	case FTTTrackBase::TT_VectorInterp:
	{
		UCurveVector* Vector = Cast<UCurveVector>(Curve);
		if (Vector == nullptr)
		{
			return INDEX_NONE;
		}
		Id.TrackIndex = Timeline.VectorTracks.Num();
		FTTVectorTrack Track;
		Track.SetTrackName(Name, &Timeline);
		Track.CurveVector = Vector;
		Track.bIsExternalCurve = ExternalCurve != nullptr;
		Timeline.VectorTracks.Add(Track);
		break;
	}
	case FTTTrackBase::TT_LinearColorInterp:
	{
		UCurveLinearColor* Color = Cast<UCurveLinearColor>(Curve);
		if (Color == nullptr)
		{
			return INDEX_NONE;
		}
		Id.TrackIndex = Timeline.LinearColorTracks.Num();
		FTTLinearColorTrack Track;
		Track.SetTrackName(Name, &Timeline);
		Track.CurveLinearColor = Color;
		Track.bIsExternalCurve = ExternalCurve != nullptr;
		Timeline.LinearColorTracks.Add(Track);
		break;
	}
	}
	Timeline.AddDisplayTrack(Id);
	return Timeline.GetNumDisplayTracks() - 1;
}

void RemoveTimelineTrack(UTimelineTemplate& Timeline, int32 DisplayIndex)
{
	const FTTTrackId Id = Timeline.GetDisplayTrackId(DisplayIndex);
	if (Id.TrackIndex < 0)
	{
		return;
	}
	Timeline.Modify();
	Timeline.RemoveDisplayTrack(DisplayIndex);
	switch (static_cast<FTTTrackBase::ETrackType>(Id.TrackType))
	{
	case FTTTrackBase::TT_Event: Timeline.EventTracks.RemoveAt(Id.TrackIndex); break;
	case FTTTrackBase::TT_FloatInterp: Timeline.FloatTracks.RemoveAt(Id.TrackIndex); break;
	case FTTTrackBase::TT_VectorInterp: Timeline.VectorTracks.RemoveAt(Id.TrackIndex); break;
	case FTTTrackBase::TT_LinearColorInterp: Timeline.LinearColorTracks.RemoveAt(Id.TrackIndex); break;
	}
}

void MoveTimelineTrack(UTimelineTemplate& Timeline, int32 DisplayIndex, int32 DirectionDelta)
{
	Timeline.Modify();
	Timeline.MoveDisplayTrack(DisplayIndex, DirectionDelta);
}

namespace
{
FTTTrackBase* TrackAt(UTimelineTemplate& Timeline, int32 DisplayIndex)
{
	const FTTTrackId Id = Timeline.GetDisplayTrackId(DisplayIndex);
	switch (static_cast<FTTTrackBase::ETrackType>(Id.TrackType))
	{
	case FTTTrackBase::TT_Event:
		if (Timeline.EventTracks.IsValidIndex(Id.TrackIndex))
		{
			return &Timeline.EventTracks[Id.TrackIndex];
		}
		break;
	case FTTTrackBase::TT_FloatInterp:
		if (Timeline.FloatTracks.IsValidIndex(Id.TrackIndex))
		{
			return &Timeline.FloatTracks[Id.TrackIndex];
		}
		break;
	case FTTTrackBase::TT_VectorInterp:
		if (Timeline.VectorTracks.IsValidIndex(Id.TrackIndex))
		{
			return &Timeline.VectorTracks[Id.TrackIndex];
		}
		break;
	case FTTTrackBase::TT_LinearColorInterp:
		if (Timeline.LinearColorTracks.IsValidIndex(Id.TrackIndex))
		{
			return &Timeline.LinearColorTracks[Id.TrackIndex];
		}
		break;
	}
	return nullptr;
}
} // namespace

UCurveBase* GetTrackCurve(UTimelineTemplate& Timeline, int32 DisplayIndex, bool* bOutExternal)
{
	const FTTTrackId Id = Timeline.GetDisplayTrackId(DisplayIndex);
	FTTTrackBase* Track = TrackAt(Timeline, DisplayIndex);
	if (Track == nullptr)
	{
		return nullptr;
	}
	if (bOutExternal != nullptr)
	{
		*bOutExternal = Track->bIsExternalCurve;
	}
	switch (static_cast<FTTTrackBase::ETrackType>(Id.TrackType))
	{
	case FTTTrackBase::TT_Event: return Timeline.EventTracks[Id.TrackIndex].CurveKeys;
	case FTTTrackBase::TT_FloatInterp: return Timeline.FloatTracks[Id.TrackIndex].CurveFloat;
	case FTTTrackBase::TT_VectorInterp: return Timeline.VectorTracks[Id.TrackIndex].CurveVector;
	case FTTTrackBase::TT_LinearColorInterp: return Timeline.LinearColorTracks[Id.TrackIndex].CurveLinearColor;
	}
	return nullptr;
}

void SetTrackCurve(UTimelineTemplate& Timeline, int32 DisplayIndex, UCurveBase& Curve, bool bExternal)
{
	const FTTTrackId Id = Timeline.GetDisplayTrackId(DisplayIndex);
	FTTTrackBase* Track = TrackAt(Timeline, DisplayIndex);
	if (Track == nullptr)
	{
		return;
	}
	Timeline.Modify();
	switch (static_cast<FTTTrackBase::ETrackType>(Id.TrackType))
	{
	case FTTTrackBase::TT_Event: Timeline.EventTracks[Id.TrackIndex].CurveKeys = Cast<UCurveFloat>(&Curve); break;
	case FTTTrackBase::TT_FloatInterp: Timeline.FloatTracks[Id.TrackIndex].CurveFloat = Cast<UCurveFloat>(&Curve); break;
	case FTTTrackBase::TT_VectorInterp: Timeline.VectorTracks[Id.TrackIndex].CurveVector = Cast<UCurveVector>(&Curve); break;
	case FTTTrackBase::TT_LinearColorInterp: Timeline.LinearColorTracks[Id.TrackIndex].CurveLinearColor = Cast<UCurveLinearColor>(&Curve); break;
	}
	Track->bIsExternalCurve = bExternal;
}

void SetCurveKeys(UCurveBase& Curve, FRichCurve& Channel, const TArray<FRichCurveKey>& Keys)
{
	Curve.Modify();
	Channel.SetKeys(Keys);
	Channel.AutoSetTangents();
}

void RefreshTimelineNode(UBlueprint& Blueprint, UTimelineTemplate& Timeline)
{
	if (UK2Node_Timeline* Node = NodeFor(Blueprint, Timeline))
	{
		Node->Modify();
		Node->ReconstructNode();
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(&Blueprint);
}
} // namespace UE::AssetDocument::Graph::BlueprintRawEdits
