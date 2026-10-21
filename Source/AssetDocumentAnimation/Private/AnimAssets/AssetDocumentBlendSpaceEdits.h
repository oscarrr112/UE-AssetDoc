// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

// Editor-equivalent raw edits of a UBlendSpace, shared by the standalone Blend Space asset
// primitives (animation_asset.blend_space.*) and the Blend Space Graph node of an Animation
// Blueprint (AnimBlueprint/AssetDocumentAnimBlendSpaceGraphRawEdits.*): one implementation of
// "a Blend Space details panel edit".
//
//   Details edit: FBlendSpaceDetails / FBlendSampleDetails write through a property handle
//     (Modify, PreEditChange, value write, PostEditChangeProperty with the member property), then
//     SBlendSpaceEditor::NotifyPostChange (ValidateSampleData + ResampleData + MarkPackageDirty).
//   BlendParameters (axis settings), InterpolationParam and SampleData are protected UPROPERTYs:
//     they are reached by reflection, by name, as the property handles reach them.
// Behaviour test: UEAssetDoc.AnimationAssets.RawEdits.BlendSpace.

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

class FProperty;
class FStructProperty;
class UBlendSpace;
struct FBlendParameter;
struct FBlendSample;

namespace UE::AssetDocument::Animation::BlendSpaceEdits
{
using FValue = FAssetDocumentRuntimeValueDto;

// Axes a blend space uses: 1 for BlendSpace1D / AimOffsetBlendSpace1D, else 2.
int32 NumAxes(const UBlendSpace& BlendSpace);

// The BlendParameters / InterpolationParam / SampleData properties (null if the class changed shape).
FStructProperty* AxisProperty();
FStructProperty* InterpolationProperty();
FProperty* SampleDataProperty();

// Mutable access through reflection (BlendParameters[Axis], SampleData[Index]); null when out of range.
FBlendParameter* GetAxis(UBlendSpace& BlendSpace, int32 Axis);
FBlendSample* GetSample(UBlendSpace& BlendSpace, int32 Index);

// SBlendSpaceEditor::NotifyPostChange: ValidateSampleData, ResampleData, MarkPackageDirty.
void NotifyPostChange(UBlendSpace& BlendSpace);

// One details-panel edit: Modify, PreEditChange(Leaf), decode Value into LeafPtr (the graph property
// codec), PostEditChangeProperty(ValueSet, MemberProperty = Top), then NotifyPostChange. Returns false
// with OutError when the value does not decode (the PostEditChange still runs, as a reverted edit).
bool WriteDetailsValue(UBlendSpace& BlendSpace, FProperty& Top, FProperty& Leaf, void* LeafPtr, const FValue& Value, FString& OutError);

// Axis field (DisplayName, Min, Max, GridNum, bSnapToGrid, bWrapInput) of BlendParameters[Axis].
bool WriteAxisField(UBlendSpace& BlendSpace, int32 Axis, FName Field, const FValue& Value, FString& OutError);

// Sample field (RateScale, bUseSingleFrameForBlending, FrameIndexToSample, bIncludeInAnalyseAll) of SampleData[Index].
bool WriteSampleField(UBlendSpace& BlendSpace, int32 Index, FName Field, const FValue& Value, FString& OutError);
} // namespace UE::AssetDocument::Animation::BlendSpaceEdits
