// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// animation_asset.pose_asset.* : the Pose Asset editor (SPoseViewer rename / delete poses,
// FPoseAssetDetails "Apply" additive settings and "Update Source"), poses and curves read only
// otherwise (the editor cannot author new poses outside a source animation).
// animation_asset.composite.* : the Anim Composite editor's segment track (SAnimSegmentsPanel add /
// remove / replace segment, segment Details, then SAnimCompositePanel::PostAnimUpdate: sort, common
// frame rate, FAnimModel_AnimComposite::RecalculateSequenceLength). Segments are written as one
// positional replace of the track; their start positions follow from the editor's collapse.

#include "AssetDocumentEditorTrialScope.h"
#include "AnimAssets/AssetDocumentAnimAssetPrimitives.h"

#include "Animation/AnimComposite.h"
#include "Animation/AnimSequence.h"
#include "Animation/PoseAsset.h"
#include "Animation/Skeleton.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "AssetDocumentPoseComposite"

namespace UE::AssetDocument::Graph::AnimAssets
{
namespace
{
const TCHAR* ReferencePoseName = TEXT("");

UPoseAsset* GetPoseAsset(FCall& Call)
{
	UPoseAsset* PoseAsset = Cast<UPoseAsset>(&Call.Asset);
	if (PoseAsset == nullptr)
	{
		Fail(Call, TEXT("AnimationAsset.UnsupportedAsset"), TEXT("Pose asset primitives edit Pose Assets."), TEXT("/HandleArguments/asset"));
	}
	return PoseAsset;
}

UAnimComposite* GetComposite(FCall& Call)
{
	UAnimComposite* Composite = Cast<UAnimComposite>(&Call.Asset);
	if (Composite == nullptr)
	{
		Fail(Call, TEXT("AnimationAsset.UnsupportedAsset"), TEXT("Composite primitives edit Anim Composite assets."), TEXT("/HandleArguments/asset"));
	}
	return Composite;
}

FString PoseList(const UPoseAsset& PoseAsset)
{
	TArray<FString> Names;
	for (const FName Name : PoseAsset.GetPoseFNames())
	{
		Names.Add(Name.ToString());
	}
	return Names.IsEmpty() ? FString(TEXT("(none)")) : FString::Join(Names, TEXT(", "));
}

bool ReadPoseName(FReader& Op, const TCHAR* Key, FName& Out)
{
	FString Text;
	if (!Op.String(Key, Text))
	{
		return false;
	}
	const bool bControl = Text.Contains(TEXT("\n")) || Text.Contains(TEXT("\r")) || Text.Contains(TEXT("\t"));
	if (Text.IsEmpty() || Text.TrimStartAndEnd() != Text || bControl || Text.Len() >= NAME_SIZE || Text.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		return Op.Fail(TEXT("PoseAsset.InvalidPoseName"), FString::Printf(TEXT("'%s' is not a valid pose name."), *Text), Key);
	}
	Out = FName(*Text);
	return true;
}

bool RequirePose(FReader& Op, const UPoseAsset& PoseAsset, FName Name, const TCHAR* Key)
{
	if (PoseAsset.ContainsPose(Name))
	{
		return true;
	}
	return Op.Fail(TEXT("PoseAsset.PoseNotFound"), FString::Printf(TEXT("%s has no pose '%s'. Poses: %s."),
		*PoseAsset.GetPathName(), *Name.ToString(), *PoseList(PoseAsset)), Key);
}

bool ReadSourceAnimation(FReader& Op, const UPoseAsset& PoseAsset, UAnimSequence*& Out)
{
	Out = Cast<UAnimSequence>(LoadReference(Op, TEXT("animation"), UAnimSequence::StaticClass(), TEXT("an Animation Sequence")));
	if (Out == nullptr)
	{
		return false;
	}
	// FPoseAssetDetails::ShouldFilterAsset / OnUpdatePoseSourceAnimation.
	const USkeleton* Skeleton = PoseAsset.GetSkeleton();
	if (Skeleton == nullptr || Out->GetSkeleton() == nullptr || !Skeleton->IsCompatibleForEditor(Out->GetSkeleton()))
	{
		return Op.Fail(TEXT("AnimationAsset.IncompatibleSkeleton"), FString::Printf(
			TEXT("%s uses skeleton %s, which is not compatible with %s's skeleton %s."), *Out->GetPathName(),
			*PathOf(Out->GetSkeleton()), *PoseAsset.GetPathName(), *PathOf(Skeleton)), TEXT("animation"));
	}
	return true;
}

bool ApplyPoseOp(FReader& Op, const FString& Kind, UPoseAsset& PoseAsset)
{
	if (Kind == TEXT("rename"))
	{
		FName Name;
		FName To;
		if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("to")}) || !ReadPoseName(Op, TEXT("name"), Name) || !ReadPoseName(Op, TEXT("to"), To)
			|| !RequirePose(Op, PoseAsset, Name, TEXT("name")))
		{
			return false;
		}
		// SPoseViewer::ModifyName asks before reusing a name ("can cause conflict of curve data"); refused here.
		if (PoseAsset.ContainsPose(To))
		{
			return Op.Fail(TEXT("PoseAsset.PoseExists"), FString::Printf(TEXT("%s already has a pose '%s'."), *PoseAsset.GetPathName(), *To.ToString()), TEXT("to"));
		}
		PoseAsset.Modify();
		if (!PoseAsset.ModifyPoseName(Name, To))
		{
			return Op.Fail(TEXT("AD.RUNTIME.PRIMITIVE_FAILED"), FString::Printf(TEXT("Pose '%s' could not be renamed."), *Name.ToString()), TEXT("name"));
		}
		return true;
	}
	if (Kind == TEXT("remove"))
	{
		const FValue* Names = nullptr;
		if (!Op.OnlyFields({TEXT("op"), TEXT("names")}) || !Op.Array(TEXT("names"), Names))
		{
			return false;
		}
		TArray<FName> ToDelete;
		for (int32 Index = 0; Index < Names->Elements.Num(); ++Index)
		{
			const FValue& Element = Names->Elements[Index];
			const FString Key = FString::Printf(TEXT("names/%d"), Index);
			if (Element.Kind != EAssetDocumentRuntimeValueKind::String)
			{
				return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("names lists pose names."), *Key);
			}
			const FName Name(*Element.StringValue);
			if (!RequirePose(Op, PoseAsset, Name, *Key))
			{
				return false;
			}
			ToDelete.Add(Name);
		}
		// SPoseViewer::OnDeletePoses.
		PoseAsset.Modify();
		PoseAsset.DeletePoses(ToDelete);
		return true;
	}
	if (Kind == TEXT("additive"))
	{
		bool bAdditive = false;
		FString BasePose;
		if (!Op.OnlyFields({TEXT("op"), TEXT("additive"), TEXT("base_pose")}) || !Op.Bool(TEXT("additive"), bAdditive, true)
			|| !Op.String(TEXT("base_pose"), BasePose))
		{
			return false;
		}
		int32 BaseIndex = INDEX_NONE;
		if (BasePose != ReferencePoseName)
		{
			if (!RequirePose(Op, PoseAsset, FName(*BasePose), TEXT("base_pose")))
			{
				return false;
			}
			BaseIndex = PoseAsset.GetPoseIndexByName(FName(*BasePose));
		}
		// FPoseAssetDetails::OnApplyAdditiveSettings.
		PoseAsset.Modify();
		PoseAsset.ConvertSpace(bAdditive, BaseIndex);
		return true;
	}
	if (Kind == TEXT("update_source"))
	{
		UAnimSequence* Animation = nullptr;
		if (!Op.OnlyFields({TEXT("op"), TEXT("animation")}) || !ReadSourceAnimation(Op, PoseAsset, Animation))
		{
			return false;
		}
		// The Source Animation property row, then "Update Source" (FPoseAssetDetails::OnUpdatePoseSourceAnimation).
		FProperty* Property = UPoseAsset::StaticClass()->FindPropertyByName(TEXT("SourceAnimation"));
		PoseAsset.Modify();
		PoseAsset.PreEditChange(Property);
		PoseAsset.SourceAnimation = Animation;
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		PoseAsset.PostEditChangeProperty(Event);
		PoseAsset.UpdatePoseFromAnimation(Animation);
		return true;
	}
	if (Kind == TEXT("preview_mesh"))
	{
		return SetPreviewMesh(Op, PoseAsset);
	}
	return Op.Fail(TEXT("AnimationAsset.InvalidOp"), FString::Printf(TEXT("Unknown pose asset op '%s'."), *Kind), TEXT("op"));
}

// Whether Candidate (directly or through nested composites) plays Composite.
bool PlaysComposite(const UAnimSequenceBase& Candidate, const UAnimComposite& Composite, int32 Depth = 0)
{
	if (&Candidate == &Composite)
	{
		return true;
	}
	const UAnimComposite* Nested = Cast<UAnimComposite>(&Candidate);
	if (Nested == nullptr || Depth > 32)
	{
		return Nested != nullptr;
	}
	for (const FAnimSegment& Segment : Nested->AnimationTrack.AnimSegments)
	{
		if (const UAnimSequenceBase* Reference = Segment.GetAnimReference(); Reference != nullptr && PlaysComposite(*Reference, Composite, Depth + 1))
		{
			return true;
		}
	}
	return false;
}

bool ReadSegment(FReader& Op, const UAnimComposite& Composite, FAnimTrack& Track, FAnimSegment& Out)
{
	if (!Op.IsObject())
	{
		return Op.Fail(TEXT("AnimComposite.InvalidSegment"), TEXT("Each segment is an object."));
	}
	if (!Op.OnlyFields({TEXT("animation"), TEXT("start"), TEXT("end"), TEXT("rate"), TEXT("loops")}))
	{
		return false;
	}
	UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(LoadReference(Op, TEXT("animation"), UAnimSequenceBase::StaticClass(),
		TEXT("an Animation Sequence or Anim Composite")));
	if (Animation == nullptr)
	{
		return false;
	}
	FText Reason;
	if (Animation->GetSkeleton() == nullptr || Composite.GetSkeleton() == nullptr || !Composite.GetSkeleton()->IsCompatibleForEditor(Animation->GetSkeleton()))
	{
		return Op.Fail(TEXT("AnimComposite.IncompatibleAnimation"), FString::Printf(TEXT("%s uses skeleton %s, which is not compatible with %s's skeleton %s."),
			*Animation->GetPathName(), *PathOf(Animation->GetSkeleton()), *Composite.GetPathName(), *PathOf(Composite.GetSkeleton())), TEXT("animation"));
	}
	if (!Track.IsValidToAdd(Animation, &Reason))
	{
		return Op.Fail(TEXT("AnimComposite.IncompatibleAnimation"), FString::Printf(TEXT("%s cannot be a segment of %s: %s"),
			*Animation->GetPathName(), *Composite.GetPathName(), Reason.IsEmpty() ? TEXT("the track refuses it.") : *Reason.ToString()), TEXT("animation"));
	}
	// Validate's trial edits a transient copy: a reference to the asset itself is a self reference too.
	const UAnimComposite* TrialSource = Cast<UAnimComposite>(FAssetDocumentEditorTrialScope::FindTrialSource(Composite));
	if (PlaysComposite(*Animation, Composite) || (TrialSource != nullptr && PlaysComposite(*Animation, *TrialSource)))
	{
		return Op.Fail(TEXT("AnimComposite.IncompatibleAnimation"), FString::Printf(TEXT("%s plays %s itself (recursive reference)."),
			*Animation->GetPathName(), *(TrialSource != nullptr ? TrialSource : &Composite)->GetPathName()), TEXT("animation"));
	}
	// SAnimSegmentsPanel::AddAnimSegment initialises the segment from the animation, then the
	// segment Details rows set the times / rate / loop count.
	Out.SetAnimReference(Animation, true);
	const double PlayLength = Animation->GetPlayLength();
	double Start = Out.AnimStartTime;
	double End = Out.AnimEndTime;
	double Rate = Out.AnimPlayRate;
	int64 Loops = Out.LoopingCount;
	if (!ReadFloat(Op, TEXT("start"), Start, false) || !ReadFloat(Op, TEXT("end"), End, false) || !ReadFloat(Op, TEXT("rate"), Rate, false)
		|| (Op.Has(TEXT("loops")) && !Op.Int(TEXT("loops"), Loops)))
	{
		return false;
	}
	if (Start < 0.0 || End <= Start || static_cast<float>(End) > static_cast<float>(PlayLength) + UE_KINDA_SMALL_NUMBER)
	{
		return Op.Fail(TEXT("AnimComposite.InvalidSegment"), FString::Printf(TEXT("Segment times must satisfy 0 <= start < end <= %g (the length of %s)."),
			PlayLength, *Animation->GetPathName()), Start < 0.0 || End <= Start ? TEXT("start") : TEXT("end"));
	}
	if (Rate == 0.0)
	{
		return Op.Fail(TEXT("AnimComposite.InvalidSegment"), TEXT("rate must not be 0."), TEXT("rate"));
	}
	if (Loops < 1 || Loops > 100000)
	{
		return Op.Fail(TEXT("AnimComposite.InvalidSegment"), TEXT("loops must be within 1..100000."), TEXT("loops"));
	}
	Out.AnimStartTime = static_cast<float>(Start);
	Out.AnimEndTime = static_cast<float>(End);
	Out.AnimPlayRate = static_cast<float>(Rate);
	Out.LoopingCount = static_cast<int32>(Loops);
	return true;
}

bool ApplyCompositeOp(FReader& Op, const FString& Kind, UAnimComposite& Composite)
{
	if (Kind == TEXT("segments"))
	{
		const FValue* Segments = nullptr;
		if (!Op.OnlyFields({TEXT("op"), TEXT("segments")}) || !Op.Array(TEXT("segments"), Segments))
		{
			return false;
		}
		FAnimTrack Track;
		float Position = 0.f;
		for (int32 Index = 0; Index < Segments->Elements.Num(); ++Index)
		{
			FReader SegmentReader = Op.Child(FString::Printf(TEXT("segments/%d"), Index), Segments->Elements[Index]);
			FAnimSegment Segment;
			if (!ReadSegment(SegmentReader, Composite, Track, Segment))
			{
				return false;
			}
			Segment.StartPos = Position;
			Position += Segment.GetLength();
			Track.AnimSegments.Add(MoveTemp(Segment));
		}
		// SAnimCompositePanel::PreAnimUpdate / PostAnimUpdate around the segment edits.
		Composite.Modify();
		Composite.AnimationTrack.AnimSegments = MoveTemp(Track.AnimSegments);
		Composite.AnimationTrack.CollapseAnimSegments();
		Composite.MarkPackageDirty();
		Composite.AnimationTrack.SortAnimSegments();
		Composite.UpdateCommonTargetFrameRate();
		Composite.InvalidateRecursiveAsset();
		const float Length = Composite.AnimationTrack.GetLength();
		if (Length != Composite.GetPlayLength())
		{
			Composite.SetCompositeLength(Length);
		}
		return true;
	}
	if (Kind == TEXT("preview_mesh"))
	{
		return SetPreviewMesh(Op, Composite);
	}
	return Op.Fail(TEXT("AnimationAsset.InvalidOp"), FString::Printf(TEXT("Unknown composite op '%s'."), *Kind), TEXT("op"));
}

template <typename TAsset, typename TApply>
bool RunEdit(FCall& Call, FObjectBuilder& Result, TAsset* Asset, const FText& Title, TApply Apply)
{
	const FValue* Ops = nullptr;
	if (Asset == nullptr || !Call.Args.OnlyFields({TEXT("ops")}) || !Call.Args.Array(TEXT("ops"), Ops))
	{
		return false;
	}
	// One editor transaction for the call: the editor's per-edit transactions nest into it.
	const FScopedTransaction CallTransaction(Title);
	for (int32 Index = 0; Index < Ops->Elements.Num(); ++Index)
	{
		FReader Op = Call.Args.Child(FString::Printf(TEXT("ops/%d"), Index), Ops->Elements[Index]);
		FString Kind;
		if (!Op.IsObject())
		{
			return Op.Fail(TEXT("AnimationAsset.InvalidOp"), TEXT("Each op is an object."));
		}
		if (!Op.String(TEXT("op"), Kind) || !Apply(Op, Kind, *Asset))
		{
			return false;
		}
	}
	if (!Ops->Elements.IsEmpty())
	{
		Asset->MarkPackageDirty();
	}
	Result.AddInt(TEXT("applied"), Ops->Elements.Num());
	return true;
}
} // namespace

bool HandlePoseAssetRead(FCall& Call, FObjectBuilder& Result)
{
	UPoseAsset* PoseAsset = GetPoseAsset(Call);
	if (PoseAsset == nullptr || !Call.Args.OnlyFields({}))
	{
		return false;
	}
	AddCommonFields(*PoseAsset, Result);
	Result.AddString(TEXT("source_animation"), PathOf(PoseAsset->SourceAnimation));
	TArray<FString> Poses;
	for (const FName Name : PoseAsset->GetPoseFNames())
	{
		Poses.Add(Name.ToString());
	}
	Result.Add(TEXT("poses"), MakeStringArray(Poses));
	Result.AddBool(TEXT("additive"), PoseAsset->IsValidAdditive());
	const FName BasePose = PoseAsset->GetPoseNameByIndex(PoseAsset->GetBasePoseIndex());
	Result.AddString(TEXT("base_pose"), BasePose != NAME_None ? BasePose.ToString() : FString(ReferencePoseName));
	TArray<FString> Curves;
	for (const FName Name : PoseAsset->GetCurveFNames())
	{
		Curves.Add(Name.ToString());
	}
	Result.Add(TEXT("curves"), MakeStringArray(Curves));
	Result.AddInt(TEXT("track_count"), PoseAsset->GetNumTracks());
	return true;
}

bool HandlePoseAssetEdit(FCall& Call, FObjectBuilder& Result)
{
	return RunEdit(Call, Result, GetPoseAsset(Call), LOCTEXT("EditPoseAsset", "Edit Pose Asset"), &ApplyPoseOp);
}

bool HandleCompositeRead(FCall& Call, FObjectBuilder& Result)
{
	UAnimComposite* Composite = GetComposite(Call);
	if (Composite == nullptr || !Call.Args.OnlyFields({}))
	{
		return false;
	}
	AddCommonFields(*Composite, Result);
	Result.Add(TEXT("length"), FloatValue(Composite->GetPlayLength()));
	TArray<FValue> Segments;
	for (const FAnimSegment& Segment : Composite->AnimationTrack.AnimSegments)
	{
		FObjectBuilder Row;
		Row.AddString(TEXT("animation"), PathOf(Segment.GetAnimReference()));
		Row.Add(TEXT("start_pos"), FloatValue(Segment.StartPos));
		Row.Add(TEXT("start"), FloatValue(Segment.AnimStartTime));
		Row.Add(TEXT("end"), FloatValue(Segment.AnimEndTime));
		Row.Add(TEXT("rate"), FloatValue(Segment.AnimPlayRate));
		Row.AddInt(TEXT("loops"), Segment.LoopingCount);
		Segments.Add(Row.Build());
	}
	Result.Add(TEXT("segments"), MakeArray(MoveTemp(Segments)));
	return true;
}

bool HandleCompositeEdit(FCall& Call, FObjectBuilder& Result)
{
	return RunEdit(Call, Result, GetComposite(Call), LOCTEXT("EditAnimComposite", "Edit Anim Composite"), &ApplyCompositeOp);
}
} // namespace UE::AssetDocument::Graph::AnimAssets

#undef LOCTEXT_NAMESPACE
