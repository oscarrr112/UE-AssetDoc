// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TimelineTemplate.h"

class UBlueprint;
class UCurveBase;
class UEdGraph;
class UK2Node_Timeline;
struct FRichCurve;
struct FRichCurveKey;

// Edits of Blueprint event dispatchers and timelines. Each
// function leaves the Blueprint as one editor action leaves it and names that
// action; the editor performs these in Slate widgets / the Blueprint editor,
// which are not exported. Covered by UEAssetDoc.UBlueprint.MemberRawEdits.
namespace UE::AssetDocument::Graph::BlueprintRawEdits
{
// "+ Event Dispatcher" can be added to this Blueprint (FBlueprintEditor::AddNewDelegateIsVisible).
bool SupportsEventDispatchers(const UBlueprint& Blueprint);

// FBlueprintEditor::OnAddNewDelegate with the name the user then types: the PC_MCDelegate member
// variable plus its non-editable delegate signature graph (entry + result, BlueprintCallable|
// BlueprintEvent|Public, editable entry). Returns the signature graph, nullptr when nothing was added.
UEdGraph* AddEventDispatcher(UBlueprint& Blueprint, FName Name);

// SMyBlueprint::OnDeleteDelegate: removes the member variable and the signature graph together and
// lets Create Event nodes of this Blueprint notice the change.
void RemoveEventDispatcher(UBlueprint& Blueprint, UEdGraph& SignatureGraph);

// The K2Node_Timeline menu action (UK2Node_Timeline::GetMenuActions customize + UBlueprintNodeSpawner::
// SpawnEdGraphNode) followed by the user's rename: places a timeline node named Name in Graph together
// with its UTimelineTemplate. Returns nullptr when the Blueprint does not support timelines or the
// template could not be added.
UK2Node_Timeline* AddTimeline(UBlueprint& Blueprint, UEdGraph& Graph, FName Name, const FVector2D& Location);

// STimelineEditor check boxes / length box / tick group combo: template field, the cached copy on the
// node (where the editor keeps one) and MarkBlueprintAsModified.
enum class ETimelineFlag : uint8 { AutoPlay, Loop, Replicated, IgnoreTimeDilation };
void SetTimelineFlag(UBlueprint& Blueprint, UTimelineTemplate& Timeline, ETimelineFlag Flag, bool bValue);
void SetTimelineLength(UBlueprint& Blueprint, UTimelineTemplate& Timeline, float Length);
void SetTimelineUseLastKeyframe(UBlueprint& Blueprint, UTimelineTemplate& Timeline, bool bUseLastKeyframe);
void SetTimelineTickGroup(UBlueprint& Blueprint, UTimelineTemplate& Timeline, ETickingGroup TickGroup);

// STimelineEditor::CreateNewCurve: an internal track curve (outer: the generated class, RF_Public).
UCurveBase* CreateTrackCurve(UBlueprint& Blueprint, FTTTrackBase::ETrackType Type);

// STimelineEditor::CreateNewTrack (named by the caller) / CreateNewTrackFromAsset when ExternalCurve
// is given. Appends the track to the display order; returns its display index or INDEX_NONE.
int32 AddTimelineTrack(UBlueprint& Blueprint, UTimelineTemplate& Timeline, FTTTrackBase::ETrackType Type, FName Name,
	UCurveBase* ExternalCurve);
// STimelineEditor::OnDeleteSelectedTracks for one display index.
void RemoveTimelineTrack(UTimelineTemplate& Timeline, int32 DisplayIndex);
// STimelineEditor::OnReorderTracks.
void MoveTimelineTrack(UTimelineTemplate& Timeline, int32 DisplayIndex, int32 DirectionDelta);

// The curve of a display track (nullptr for an invalid index) and whether it is an external asset.
UCurveBase* GetTrackCurve(UTimelineTemplate& Timeline, int32 DisplayIndex, bool* bOutExternal = nullptr);
// STimelineEdTrack::SwitchToExternalCurve / UseInternalCurve: points the track at Curve and flags it
// external or internal.
void SetTrackCurve(UTimelineTemplate& Timeline, int32 DisplayIndex, UCurveBase& Curve, bool bExternal);

// Curve editor key edits on a track curve: replaces the keys and recomputes auto tangents
// (FRichCurveEditorModel / SCurveEditor call AutoSetTangents after every key change).
void SetCurveKeys(UCurveBase& Curve, FRichCurve& Channel, const TArray<FRichCurveKey>& Keys);

// Refreshes the timeline node pins after track edits (STimelineEditor calls ReconstructNode).
void RefreshTimelineNode(UBlueprint& Blueprint, UTimelineTemplate& Timeline);
} // namespace UE::AssetDocument::Graph::BlueprintRawEdits
