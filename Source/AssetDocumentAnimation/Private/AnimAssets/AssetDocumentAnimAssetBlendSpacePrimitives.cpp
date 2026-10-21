// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// animation_asset.blend_space.read.v1 / edit.v1: the Blend Space editor (SBlendSpaceEditor sample
// add / remove / replace, FBlendSpaceDetails axis settings, FBlendSampleDetails sample settings) for
// BlendSpace, BlendSpace1D, AimOffsetBlendSpace and AimOffsetBlendSpace1D. The details-panel writes
// are AssetDocumentBlendSpaceEdits, shared with the Animation Blueprint Blend Space Graph node.
// Samples are identified by their position (the editor keeps positions unique).

#include "AssetDocumentEditorTrialScope.h"
#include "AnimAssets/AssetDocumentAnimAssetPrimitives.h"
#include "AnimAssets/AssetDocumentBlendSpaceEdits.h"

#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "AssetDocumentBlendSpace"

namespace UE::AssetDocument::Graph::AnimAssets
{
namespace
{
namespace Edits = UE::AssetDocument::Animation::BlendSpaceEdits;

// Same-position tolerance; UBlendSpace::IsSameSamplePoint compares with KINDA_SMALL_NUMBER.
constexpr double PositionTolerance = 1.0e-4;

UBlendSpace* GetBlendSpace(FCall& Call)
{
	UBlendSpace* BlendSpace = Cast<UBlendSpace>(&Call.Asset);
	// Validate's trial edits a transient copy, which stands in for the asset.
	if (BlendSpace == nullptr || (!BlendSpace->IsAsset() && !FAssetDocumentEditorTrialScope::IsTrialTarget(*BlendSpace)))
	{
		Fail(Call, TEXT("AnimationAsset.UnsupportedAsset"), TEXT("Blend space primitives edit Blend Space assets."), TEXT("/HandleArguments/asset"));
		return nullptr;
	}
	return BlendSpace;
}

FString Position(const UBlendSpace& BlendSpace, const FVector& Value)
{
	return Edits::NumAxes(BlendSpace) == 1 ? FString::Printf(TEXT("X=%g"), Value.X) : FString::Printf(TEXT("X=%g, Y=%g"), Value.X, Value.Y);
}

bool ReadPosition(FReader& Op, const UBlendSpace& BlendSpace, FVector& Out)
{
	double X = 0.0;
	double Y = 0.0;
	if (!ReadFloat(Op, TEXT("x"), X))
	{
		return false;
	}
	if (Edits::NumAxes(BlendSpace) == 2)
	{
		if (!ReadFloat(Op, TEXT("y"), Y))
		{
			return false;
		}
	}
	else if (Op.Has(TEXT("y")))
	{
		return Op.Fail(TEXT("BlendSpace.InvalidSample"), TEXT("A 1D blend space sample has only x."), TEXT("y"));
	}
	Out = FVector(X, Y, 0.0);
	return true;
}

int32 FindSample(const UBlendSpace& BlendSpace, const FVector& Value)
{
	const TArray<FBlendSample>& Samples = BlendSpace.GetBlendSamples();
	for (int32 Index = 0; Index < Samples.Num(); ++Index)
	{
		if (FMath::Abs(Samples[Index].SampleValue.X - Value.X) <= PositionTolerance && FMath::Abs(Samples[Index].SampleValue.Y - Value.Y) <= PositionTolerance)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

bool RequireSample(FReader& Op, const UBlendSpace& BlendSpace, int32& OutIndex)
{
	FVector Value = FVector::ZeroVector;
	if (!ReadPosition(Op, BlendSpace, Value))
	{
		return false;
	}
	OutIndex = FindSample(BlendSpace, Value);
	if (OutIndex == INDEX_NONE)
	{
		return Op.Fail(TEXT("BlendSpace.SampleNotFound"), FString::Printf(TEXT("%s has no sample at %s."),
			*BlendSpace.GetPathName(), *Position(BlendSpace, Value)), TEXT("x"));
	}
	return true;
}

bool ReadAnimation(FReader& Op, const UBlendSpace& BlendSpace, UAnimSequence*& Out)
{
	Out = Cast<UAnimSequence>(LoadReference(Op, TEXT("animation"), UAnimSequence::StaticClass(), TEXT("an Animation Sequence")));
	if (Out == nullptr)
	{
		return false;
	}
	if (!BlendSpace.ValidateAnimationSequence(Out))
	{
		return Op.Fail(TEXT("BlendSpace.IncompatibleAnimation"), FString::Printf(
			TEXT("%s cannot be a sample of %s: its skeleton (%s) must be compatible with %s and its additive type must match the blend space's existing samples."),
			*Out->GetPathName(), *BlendSpace.GetPathName(), *PathOf(Out->GetSkeleton()), *PathOf(BlendSpace.GetSkeleton())), TEXT("animation"));
	}
	return true;
}

bool Write(FReader& Op, bool bWritten, const FString& Error, const TCHAR* Key)
{
	return bWritten || Op.Fail(TEXT("Graph.InvalidArgument"), Error, Key);
}

// {op: axis, axis, name?, min?, max?, grid?, snap?, wrap?}: FBlendSpaceDetails' axis rows. While
// snapping, UBlendSpace::PostEditChangeProperty re-derives Min/Max from the previous grid spacing (on
// both axes), so a range edit runs with snapping off and snapping is restored last - the order a user
// follows to change a snapped range.
bool SetAxis(FReader& Op, UBlendSpace& BlendSpace)
{
	int64 Axis = 0;
	if (!Op.OnlyFields({TEXT("op"), TEXT("axis"), TEXT("name"), TEXT("min"), TEXT("max"), TEXT("grid"), TEXT("snap"), TEXT("wrap")})
		|| !Op.Int(TEXT("axis"), Axis))
	{
		return false;
	}
	const int32 NumAxes = Edits::NumAxes(BlendSpace);
	if (Axis < 0 || Axis >= NumAxes)
	{
		return Op.Fail(TEXT("BlendSpace.InvalidAxis"), FString::Printf(TEXT("%s has %d axis/axes; axis must be 0%s."),
			*BlendSpace.GetPathName(), NumAxes, NumAxes == 2 ? TEXT(" or 1") : TEXT("")), TEXT("axis"));
	}
	const int32 AxisIndex = static_cast<int32>(Axis);
	const FBlendParameter Current = BlendSpace.GetBlendParameter(AxisIndex);
	FString Error;
	if (Op.Has(TEXT("name")))
	{
		FString Name;
		if (!Op.String(TEXT("name"), Name))
		{
			return false;
		}
		if (Current.DisplayName != Name && !Write(Op, Edits::WriteAxisField(BlendSpace, AxisIndex, TEXT("DisplayName"), MakeString(Name), Error), Error, TEXT("name")))
		{
			return false;
		}
	}
	if (Op.Has(TEXT("wrap")))
	{
		bool bWrap = false;
		if (!Op.Bool(TEXT("wrap"), bWrap, true))
		{
			return false;
		}
		if (Current.bWrapInput != bWrap && !Write(Op, Edits::WriteAxisField(BlendSpace, AxisIndex, TEXT("bWrapInput"), MakeBool(bWrap), Error), Error, TEXT("wrap")))
		{
			return false;
		}
	}
	double Min = Current.Min;
	double Max = Current.Max;
	int64 Grid = Current.GridNum;
	bool bSnap = Current.bSnapToGrid;
	if (!ReadFloat(Op, TEXT("min"), Min, false) || !ReadFloat(Op, TEXT("max"), Max, false) || (Op.Has(TEXT("grid")) && !Op.Int(TEXT("grid"), Grid))
		|| !Op.Bool(TEXT("snap"), bSnap, false))
	{
		return false;
	}
	if (Min >= Max)
	{
		return Op.Fail(TEXT("BlendSpace.InvalidAxis"), FString::Printf(TEXT("Axis %d minimum (%g) must be below its maximum (%g)."), AxisIndex, Min, Max), TEXT("min"));
	}
	if (Grid < 1 || Grid > 1000)
	{
		return Op.Fail(TEXT("BlendSpace.InvalidAxis"), TEXT("grid (Grid Divisions) must be within 1..1000."), TEXT("grid"));
	}
	const bool bRangeChanges = static_cast<float>(Min) != Current.Min || static_cast<float>(Max) != Current.Max || Grid != Current.GridNum;
	TArray<int32, TInlineAllocator<2>> Unsnapped;
	if (bRangeChanges)
	{
		for (int32 Index = 0; Index < NumAxes; ++Index)
		{
			if (BlendSpace.GetBlendParameter(Index).bSnapToGrid)
			{
				Unsnapped.Add(Index);
				if (!Write(Op, Edits::WriteAxisField(BlendSpace, Index, TEXT("bSnapToGrid"), MakeBool(false), Error), Error, TEXT("snap")))
				{
					return false;
				}
			}
		}
		// Keep Min < Max between the two writes.
		struct FBound
		{
			const TCHAR* Field;
			const TCHAR* Key;
			double Value;
		};
		const FBound MinBound{TEXT("Min"), TEXT("min"), Min};
		const FBound MaxBound{TEXT("Max"), TEXT("max"), Max};
		const bool bMaxFirst = Min >= Current.Max;
		for (const FBound& Bound : {bMaxFirst ? MaxBound : MinBound, bMaxFirst ? MinBound : MaxBound})
		{
			const FBlendParameter& Live = BlendSpace.GetBlendParameter(AxisIndex);
			const float LiveValue = Bound.Field == MinBound.Field ? Live.Min : Live.Max;
			if (LiveValue != static_cast<float>(Bound.Value)
				&& !Write(Op, Edits::WriteAxisField(BlendSpace, AxisIndex, Bound.Field, MakeNumber(Bound.Value), Error), Error, Bound.Key))
			{
				return false;
			}
		}
		if (BlendSpace.GetBlendParameter(AxisIndex).GridNum != Grid
			&& !Write(Op, Edits::WriteAxisField(BlendSpace, AxisIndex, TEXT("GridNum"), MakeInt(Grid), Error), Error, TEXT("grid")))
		{
			return false;
		}
	}
	for (const int32 Index : Unsnapped)
	{
		const bool bWanted = Index == AxisIndex ? bSnap : true;
		if (bWanted && !Write(Op, Edits::WriteAxisField(BlendSpace, Index, TEXT("bSnapToGrid"), MakeBool(true), Error), Error, TEXT("snap")))
		{
			return false;
		}
	}
	if (BlendSpace.GetBlendParameter(AxisIndex).bSnapToGrid != bSnap
		&& !Write(Op, Edits::WriteAxisField(BlendSpace, AxisIndex, TEXT("bSnapToGrid"), MakeBool(bSnap), Error), Error, TEXT("snap")))
	{
		return false;
	}
	return true;
}

// {op: sample_set, x, y?, animation?, rate_scale?, single_frame?, analyse_all?}: the grid's drag-drop
// replace (SBlendSpaceEditor::OnSampleReplaced) and FBlendSampleDetails rows.
bool SetSample(FReader& Op, UBlendSpace& BlendSpace)
{
	int32 Index = INDEX_NONE;
	if (!Op.OnlyFields({TEXT("op"), TEXT("x"), TEXT("y"), TEXT("animation"), TEXT("rate_scale"), TEXT("single_frame"), TEXT("analyse_all")})
		|| !RequireSample(Op, BlendSpace, Index))
	{
		return false;
	}
	FString Error;
	if (Op.Has(TEXT("animation")))
	{
		UAnimSequence* Animation = nullptr;
		if (!ReadAnimation(Op, BlendSpace, Animation))
		{
			return false;
		}
		if (BlendSpace.GetBlendSample(Index).Animation != Animation)
		{
			BlendSpace.Modify();
			BlendSpace.ReplaceSampleAnimation(Index, Animation);
			BlendSpace.ResampleData();
			BlendSpace.ValidateSampleData();
		}
	}
	if (Op.Has(TEXT("rate_scale")))
	{
		double Rate = 1.0;
		if (!ReadFloat(Op, TEXT("rate_scale"), Rate))
		{
			return false;
		}
		if (Rate < 0.01 || Rate > 64.0)
		{
			return Op.Fail(TEXT("BlendSpace.InvalidSample"), TEXT("rate_scale must be within 0.01..64 (the details panel clamp)."), TEXT("rate_scale"));
		}
		if (BlendSpace.GetBlendSample(Index).RateScale != static_cast<float>(Rate)
			&& !Write(Op, Edits::WriteSampleField(BlendSpace, Index, TEXT("RateScale"), MakeNumber(Rate), Error), Error, TEXT("rate_scale")))
		{
			return false;
		}
	}
	// Find, not Has: null (blend all frames) is a value here (Has() is false for null).
	if (Op.Find(TEXT("single_frame")) != nullptr)
	{
		const FValue* Value = Op.Find(TEXT("single_frame"));
		const bool bUse = Value->Kind != EAssetDocumentRuntimeValueKind::Null;
		int64 Frame = 0;
		if (bUse && (!Op.Int(TEXT("single_frame"), Frame) || Frame < 0 || Frame > MAX_int32))
		{
			return Op.Fail(TEXT("BlendSpace.InvalidSample"), TEXT("single_frame is a frame index (>= 0) or null."), TEXT("single_frame"));
		}
		const FBlendSample& Sample = BlendSpace.GetBlendSample(Index);
		if (bUse && Sample.FrameIndexToSample != static_cast<uint32>(Frame)
			&& !Write(Op, Edits::WriteSampleField(BlendSpace, Index, TEXT("FrameIndexToSample"), MakeInt(Frame), Error), Error, TEXT("single_frame")))
		{
			return false;
		}
		if (BlendSpace.GetBlendSample(Index).bUseSingleFrameForBlending != bUse
			&& !Write(Op, Edits::WriteSampleField(BlendSpace, Index, TEXT("bUseSingleFrameForBlending"), MakeBool(bUse), Error), Error, TEXT("single_frame")))
		{
			return false;
		}
	}
	if (Op.Has(TEXT("analyse_all")))
	{
		bool bValue = true;
		if (!Op.Bool(TEXT("analyse_all"), bValue, true))
		{
			return false;
		}
		if (static_cast<bool>(BlendSpace.GetBlendSample(Index).bIncludeInAnalyseAll) != bValue
			&& !Write(Op, Edits::WriteSampleField(BlendSpace, Index, TEXT("bIncludeInAnalyseAll"), MakeBool(bValue), Error), Error, TEXT("analyse_all")))
		{
			return false;
		}
	}
	return true;
}

bool ApplyBlendSpaceOp(FReader& Op, const FString& Kind, UBlendSpace& BlendSpace)
{
	if (Kind == TEXT("axis"))
	{
		return SetAxis(Op, BlendSpace);
	}
	if (Kind == TEXT("sample_remove"))
	{
		int32 Index = INDEX_NONE;
		if (!Op.OnlyFields({TEXT("op"), TEXT("x"), TEXT("y")}) || !RequireSample(Op, BlendSpace, Index))
		{
			return false;
		}
		// SBlendSpaceEditor::OnSampleRemoved.
		BlendSpace.Modify();
		BlendSpace.DeleteSample(Index);
		BlendSpace.ResampleData();
		BlendSpace.ValidateSampleData();
		BlendSpace.PostEditChange();
		return true;
	}
	if (Kind == TEXT("sample_add"))
	{
		FVector Value = FVector::ZeroVector;
		UAnimSequence* Animation = nullptr;
		if (!Op.OnlyFields({TEXT("op"), TEXT("x"), TEXT("y"), TEXT("animation")}) || !ReadPosition(Op, BlendSpace, Value)
			|| !ReadAnimation(Op, BlendSpace, Animation))
		{
			return false;
		}
		// UBlendSpace::AddSample widens the axis range for an outside position; the editor's grid
		// only offers positions inside it, so an outside position is refused instead.
		if (!BlendSpace.IsSampleWithinBounds(Value))
		{
			TArray<FString> Ranges;
			for (int32 Axis = 0; Axis < Edits::NumAxes(BlendSpace); ++Axis)
			{
				Ranges.Add(FString::Printf(TEXT("%s %g..%g"), Axis == 0 ? TEXT("X") : TEXT("Y"), BlendSpace.GetBlendParameter(Axis).Min,
					BlendSpace.GetBlendParameter(Axis).Max));
			}
			return Op.Fail(TEXT("BlendSpace.SampleOutOfRange"), FString::Printf(TEXT("Sample position %s is outside the axis range of %s (%s)."),
				*Position(BlendSpace, Value), *BlendSpace.GetPathName(), *FString::Join(Ranges, TEXT(", "))), TEXT("x"));
		}
		if (FindSample(BlendSpace, Value) != INDEX_NONE)
		{
			return Op.Fail(TEXT("BlendSpace.InvalidSample"), FString::Printf(TEXT("%s already has a sample at %s."),
				*BlendSpace.GetPathName(), *Position(BlendSpace, Value)), TEXT("x"));
		}
		// SBlendSpaceEditor::OnSampleAdded (no analysis: the position is authored).
		BlendSpace.Modify();
		if (BlendSpace.AddSample(Animation, Value) == INDEX_NONE)
		{
			return Op.Fail(TEXT("BlendSpace.InvalidSample"), FString::Printf(TEXT("%s refused a sample at %s."),
				*BlendSpace.GetPathName(), *Position(BlendSpace, Value)), TEXT("x"));
		}
		BlendSpace.ResampleData();
		BlendSpace.ValidateSampleData();
		BlendSpace.PostEditChange();
		return true;
	}
	if (Kind == TEXT("sample_set"))
	{
		return SetSample(Op, BlendSpace);
	}
	if (Kind == TEXT("refresh"))
	{
		// SBlendSpaceEditor::NotifyPostChange after details-panel edits of the asset's own properties.
		if (!Op.OnlyFields({TEXT("op")}))
		{
			return false;
		}
		Edits::NotifyPostChange(BlendSpace);
		return true;
	}
	if (Kind == TEXT("preview_mesh"))
	{
		return SetPreviewMesh(Op, BlendSpace);
	}
	return Op.Fail(TEXT("AnimationAsset.InvalidOp"), FString::Printf(TEXT("Unknown blend space op '%s'."), *Kind), TEXT("op"));
}
} // namespace

bool HandleBlendSpaceRead(FCall& Call, FObjectBuilder& Result)
{
	UBlendSpace* BlendSpace = GetBlendSpace(Call);
	if (BlendSpace == nullptr || !Call.Args.OnlyFields({}))
	{
		return false;
	}
	AddCommonFields(*BlendSpace, Result);
	const int32 NumAxes = Edits::NumAxes(*BlendSpace);
	TArray<FValue> Axes;
	for (int32 Axis = 0; Axis < NumAxes; ++Axis)
	{
		const FBlendParameter& Parameter = BlendSpace->GetBlendParameter(Axis);
		FObjectBuilder Row;
		Row.AddString(TEXT("name"), Parameter.DisplayName);
		Row.Add(TEXT("min"), FloatValue(Parameter.Min));
		Row.Add(TEXT("max"), FloatValue(Parameter.Max));
		Row.AddInt(TEXT("grid"), Parameter.GridNum);
		Row.AddBool(TEXT("snap"), Parameter.bSnapToGrid);
		Row.AddBool(TEXT("wrap"), Parameter.bWrapInput);
		Axes.Add(Row.Build());
	}
	Result.Add(TEXT("axes"), MakeArray(MoveTemp(Axes)));
	TArray<FValue> Samples;
	for (const FBlendSample& Sample : BlendSpace->GetBlendSamples())
	{
		FObjectBuilder Row;
		Row.AddString(TEXT("animation"), PathOf(Sample.Animation));
		Row.Add(TEXT("x"), MakeNumber(Sample.SampleValue.X));
		if (NumAxes == 2)
		{
			Row.Add(TEXT("y"), MakeNumber(Sample.SampleValue.Y));
		}
		Row.Add(TEXT("rate_scale"), FloatValue(Sample.RateScale));
		Row.Add(TEXT("single_frame"), Sample.bUseSingleFrameForBlending ? MakeInt(Sample.FrameIndexToSample) : MakeNull());
		Row.AddBool(TEXT("analyse_all"), Sample.bIncludeInAnalyseAll != 0);
		Row.AddBool(TEXT("valid"), Sample.bIsValid != 0);
		Samples.Add(Row.Build());
	}
	Result.Add(TEXT("samples"), MakeArray(MoveTemp(Samples)));
	return true;
}

bool HandleBlendSpaceEdit(FCall& Call, FObjectBuilder& Result)
{
	UBlendSpace* BlendSpace = GetBlendSpace(Call);
	const FValue* Ops = nullptr;
	if (BlendSpace == nullptr || !Call.Args.OnlyFields({TEXT("ops")}) || !Call.Args.Array(TEXT("ops"), Ops))
	{
		return false;
	}
	// One editor transaction for the call: the Blend Space editor's per-edit transactions nest into it.
	const FScopedTransaction CallTransaction(LOCTEXT("EditBlendSpace", "Edit Blend Space"));
	for (int32 Index = 0; Index < Ops->Elements.Num(); ++Index)
	{
		FReader Op = Call.Args.Child(FString::Printf(TEXT("ops/%d"), Index), Ops->Elements[Index]);
		FString Kind;
		if (!Op.IsObject())
		{
			return Op.Fail(TEXT("AnimationAsset.InvalidOp"), TEXT("Each op is an object."));
		}
		if (!Op.String(TEXT("op"), Kind) || !ApplyBlendSpaceOp(Op, Kind, *BlendSpace))
		{
			return false;
		}
	}
	if (!Ops->Elements.IsEmpty())
	{
		BlendSpace->MarkPackageDirty();
	}
	Result.AddInt(TEXT("applied"), Ops->Elements.Num());
	return true;
}
} // namespace UE::AssetDocument::Graph::AnimAssets

#undef LOCTEXT_NAMESPACE
