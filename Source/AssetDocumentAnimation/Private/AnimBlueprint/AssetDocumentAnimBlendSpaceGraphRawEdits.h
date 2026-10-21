// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

// Editor-equivalent raw edits: the blend space embedded in an Animation Blueprint by a
// Blend Space Graph node (UAnimGraphNode_BlendSpaceGraphBase: an internal UBlendSpace, a dummy
// UBlendSpaceGraph for navigation and one sample graph per blend sample).
//
//   Blend space settings ("BlendSpace.<Property>", "BlendSpace.<Axis Property>.<X|Y>.<Field>"):
//     the blend space details panel of the Blend Space Graph editor tab (FBlendSpaceDetails:
//     Axis Settings per used axis, Input Interpolation, Sample Smoothing; the categories the
//     panel hides for an internal blend space are not reported) with SBlendSpaceEditor's
//     NotifyPostChange (ValidateSampleData + ResampleData).
//   Samples ([{"Name", "X", "Y"?}], a sample is identified by its sample graph's name):
//     SBlendSpaceEditor::OnSampleAdded / OnSampleRemoved / OnSampleMoved with the Animation
//     Blueprint editor's callbacks (FBlendSpaceDocumentTabFactory: AddGraph / RemoveGraph).
//     UAnimGraphNode_BlendSpaceGraphBase::RemoveGraph needs an open Blueprint editor (it closes
//     the sample graph's document tab), so its edit -- Graphs.RemoveAtSwap (mirroring
//     UBlendSpace::DeleteSample) + FBlueprintEditorUtils::RemoveGraph -- is done here; Graphs is
//     protected and reached by reflection.
// Behaviour test: UEAssetDoc.AnimBlueprint.Adapters.BlendSpaceGraphRawEdit.

#include "CoreMinimal.h"
#include "AssetDocumentGraphAdapters.h"

class UAnimGraphNode_BlendSpaceGraphBase;
class UBlendSpace;
class UEdGraph;
class UEdGraphNode;

namespace UE::AssetDocument::AnimBlueprint::BlendSpaceRawEdits
{
// The node's internal blend space (null for a node without one).
UBlendSpace* GetBlendSpace(const UAnimGraphNode_BlendSpaceGraphBase& Node);

// The sample graphs, in sample index order.
TArray<UEdGraph*> GetSampleGraphs(const UAnimGraphNode_BlendSpaceGraphBase& Node);

// Adds the node's non-default blend space settings and "Samples" to Out (an object value).
void QueryBlendSpace(const UAnimGraphNode_BlendSpaceGraphBase& Node, FAssetDocumentRuntimeValueDto& Out);

// Applies one "BlendSpace.*" setting.
bool SetBlendSpaceSetting(UAnimGraphNode_BlendSpaceGraphBase& Node, FStringView Path,
	const FAssetDocumentRuntimeValueDto& Value, FAssetDocumentGraphError& OutError);

// Makes the samples exactly Value: removes unlisted samples (with their graphs), moves listed
// samples, adds new samples with an empty sample graph named after the sample.
bool SetSamples(UAnimGraphNode_BlendSpaceGraphBase& Node, const FAssetDocumentRuntimeValueDto& Value,
	FAssetDocumentGraphError& OutError);

// The removal half of OnSampleRemoved: Graphs.RemoveAtSwap(Index) + FBlueprintEditorUtils::RemoveGraph
// (the blend space sample itself is removed by UBlendSpace::DeleteSample).
bool RemoveSampleGraph(UAnimGraphNode_BlendSpaceGraphBase& Node, int32 Index);
} // namespace UE::AssetDocument::AnimBlueprint::BlendSpaceRawEdits
