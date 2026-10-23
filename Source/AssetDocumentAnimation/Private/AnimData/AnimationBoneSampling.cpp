// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AnimData/AnimationBoneSampling.h"

#include "Algo/Transform.h"
#include "AnimPose.h"
#include "Animation/AnimCompositeBase.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/Skeleton.h"
#include "Async/Async.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::AssetDocument::AnimData
{
namespace BoneSamplingPrivate
{
constexpr int32 MaxBones = 64;
constexpr int32 MaxTimes = 1000;
constexpr int32 MaxTransforms = 2000;
constexpr double TimeTolerance = 1e-4;

enum class ESpace : uint8 { Component, Local, Root };

double RoundTo(double Value, double Step)
{
	const double Rounded = FMath::RoundToDouble(Value / Step) * Step;
	return FMath::IsNearlyZero(Rounded, Step * 0.5) ? 0.0 : Rounded;
}

TSharedPtr<FJsonValue> Vector3(const FVector& Value, double Step)
{
	return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(RoundTo(Value.X, Step)),
		MakeShared<FJsonValueNumber>(RoundTo(Value.Y, Step)), MakeShared<FJsonValueNumber>(RoundTo(Value.Z, Step))});
}

// Location [X, Y, Z] cm, Rotation [Pitch, Yaw, Roll] degrees, Scale only when it is not 1.
TSharedRef<FJsonObject> TransformJson(const FTransform& Transform)
{
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetField(TEXT("Location"), Vector3(Transform.GetLocation(), 0.01));
	const FRotator Rotation = Transform.GetRotation().Rotator();
	Out->SetField(TEXT("Rotation"), Vector3(FVector(Rotation.Pitch, Rotation.Yaw, Rotation.Roll), 0.01));
	if (!Transform.GetScale3D().Equals(FVector::OneVector, 1e-3))
	{
		Out->SetField(TEXT("Scale"), Vector3(Transform.GetScale3D(), 0.001));
	}
	return Out;
}

FAssetDocumentDiagnostic Problem(const FString& Path, const FString& Code, const FString& Message)
{
	FAssetDocumentDiagnostic Diagnostic;
	Diagnostic.Path = Path;
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	Diagnostic.Severity = TEXT("error");
	return Diagnostic;
}

FAssetDocumentResult Fail(const FString& Path, const FString& Code, const FString& Message)
{
	return FAssetDocumentResult::FailureFromDiagnostics({Problem(Path, Code, Message)}, Message);
}

// "/Game/A/AS_Walk" names "/Game/A/AS_Walk.AS_Walk".
UObject* LoadAsset(const FString& Path)
{
	const FString Trimmed = Path.TrimStartAndEnd();
	if (!Trimmed.StartsWith(TEXT("/")))
	{
		return nullptr;
	}
	const FString ObjectPath = Trimmed.Contains(TEXT("."))
		? Trimmed
		: FString::Printf(TEXT("%s.%s"), *Trimmed, *FPackageName::GetLongPackageAssetName(Trimmed));
	return LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

FString ObjectPathOf(const UObject* Object)
{
	return Object != nullptr ? Object->GetPathName() : FString();
}

// Up to 10 bone names that look like Name (substring either way, or the same first three letters).
TArray<FString> CloseBoneNames(const FReferenceSkeleton& Skeleton, const FString& Name)
{
	TArray<FString> Close;
	const FString Lower = Name.ToLower();
	for (int32 Index = 0; Index < Skeleton.GetNum() && Close.Num() < 10; ++Index)
	{
		const FString Bone = Skeleton.GetBoneName(Index).ToString();
		const FString BoneLower = Bone.ToLower();
		if (BoneLower.Contains(Lower) || Lower.Contains(BoneLower) || (Lower.Len() >= 3 && BoneLower.StartsWith(Lower.Left(3))))
		{
			Close.Add(Bone);
		}
	}
	return Close;
}

// One requested name: a bone, or a socket (skeleton or mesh) on a bone.
struct FSampledName
{
	FName Name;
	FName Bone;
	bool bSocket = false;
	FTransform SocketLocal = FTransform::Identity;
};

const TCHAR* RootLockName(ERootMotionRootLock::Type Lock)
{
	switch (Lock)
	{
	case ERootMotionRootLock::AnimFirstFrame: return TEXT("AnimFirstFrame");
	case ERootMotionRootLock::Zero: return TEXT("Zero");
	default: return TEXT("RefPose");
	}
}

// One evaluated pose, as the engine's pose evaluation returns it; bones are looked up by name.
struct FBoneSamplingPose
{
	FAnimPose Pose;
	TSet<FName> Bones;

	bool IsValid() const
	{
		return !Bones.IsEmpty() && UAnimPoseExtensions::IsValid(Pose);
	}

	FTransform Bone(FName Name, bool bComponentSpace) const
	{
		if (!Bones.Contains(Name))
		{
			return FTransform::Identity;
		}
		return UAnimPoseExtensions::GetBonePose(Pose, Name, bComponentSpace ? EAnimPoseSpaces::World : EAnimPoseSpaces::Local);
	}
};

// Poses at the given times from the engine's pose evaluation (UAnimPoseExtensions::GetAnimPoseAtTimeIntervals, what
// the animation editor's scripting uses): raw (uncompressed) data, retargeted to the proportions of the mesh when one
// is given (else the skeleton's), curves skipped, an additive sequence applied to its base pose. Root "pose": the
// root bone as authored (root motion kept in the pose). Root "extracted": the root reset as when the animation's
// root motion is extracted (bEnableRootMotion with RootMotionRootLock, or bForceRootLock). Times outside the
// animation are clamped to it.
void EvaluatePoses(const UAnimSequenceBase* Animation, const TArray<double>& Times, bool bExtractRootMotion, USkeletalMesh* Mesh,
	TArray<FBoneSamplingPose>& OutPoses)
{
	OutPoses.Reset();
	OutPoses.SetNum(Times.Num());
	if (Animation == nullptr || Animation->GetSkeleton() == nullptr || Times.IsEmpty())
	{
		return;
	}
	FAnimPoseEvaluationOptions Options;
	Options.EvaluationType = EAnimDataEvalType::Raw;
	Options.bShouldRetarget = true;
	Options.bEvaluateCurves = false;
	Options.OptionalSkeletalMesh = Mesh;
	Options.bExtractRootMotion = bExtractRootMotion;
	Options.bIncorporateRootMotionIntoPose = !bExtractRootMotion;
	// Only a sequence has an additive base pose to apply the additive pose to.
	Options.bRetrieveAdditiveAsFullPose = Animation->IsA<UAnimSequence>();

	const double Length = Animation->GetPlayLength();
	TArray<double> InRange;
	Algo::Transform(Times, InRange, [Length](double Time) { return FMath::Clamp(Time, 0.0, Length); });
	TArray<FAnimPose> Evaluated;
	UAnimPoseExtensions::GetAnimPoseAtTimeIntervals(Animation, InRange, Options, Evaluated);
	for (int32 Index = 0; Index < OutPoses.Num() && Index < Evaluated.Num(); ++Index)
	{
		FBoneSamplingPose& Out = OutPoses[Index];
		Out.Pose = MoveTemp(Evaluated[Index]);
		TArray<FName> Names;
		UAnimPoseExtensions::GetBoneNames(Out.Pose, Names);
		Out.Bones.Append(Names);
	}
}

bool RootLockApplies(const UAnimSequenceBase* Animation, bool bExtractRootMotion)
{
	const UAnimSequence* Sequence = Cast<UAnimSequence>(Animation);
	return bExtractRootMotion && Sequence != nullptr && (Sequence->bEnableRootMotion || Sequence->bForceRootLock);
}

FTransform ComponentOf(const FBoneSamplingPose& Pose, const FSampledName& Entry)
{
	const FTransform Bone = Pose.Bone(Entry.Bone, true);
	return Entry.bSocket ? Entry.SocketLocal * Bone : Bone;
}

FTransform InSpace(const FBoneSamplingPose& Pose, const FSampledName& Entry, ESpace Space, FName RootBone)
{
	switch (Space)
	{
	case ESpace::Local:
		return Entry.bSocket ? Entry.SocketLocal : Pose.Bone(Entry.Bone, false);
	case ESpace::Root:
		return ComponentOf(Pose, Entry).GetRelativeTransform(Pose.Bone(RootBone, true));
	default:
		return ComponentOf(Pose, Entry);
	}
}

bool ReadNumber(const TSharedPtr<FJsonObject>& Request, const TCHAR* Field, double& Out, bool& bPresent, TArray<FAssetDocumentDiagnostic>& Problems)
{
	bPresent = Request->HasField(Field);
	if (!bPresent)
	{
		return true;
	}
	if (!Request->TryGetNumberField(Field, Out))
	{
		Problems.Add(Problem(FString::Printf(TEXT("/%s"), Field), TEXT("AnimationSample.InvalidArgument"),
			FString::Printf(TEXT("%s must be a number (seconds)."), Field)));
		return false;
	}
	return true;
}

// The pelvis of a skeleton: a bone named pelvis / hips (any case, any prefix such as "Bip001 " or "mixamorig:"),
// else the root's first child.
int32 FindPelvis(const FReferenceSkeleton& Skeleton)
{
	for (const TCHAR* Word : {TEXT("pelvis"), TEXT("hips")})
	{
		for (int32 Index = 0; Index < Skeleton.GetNum(); ++Index)
		{
			if (Skeleton.GetBoneName(Index).ToString().ToLower().EndsWith(Word))
			{
				return Index;
			}
		}
	}
	for (int32 Index = 1; Index < Skeleton.GetNum(); ++Index)
	{
		if (Skeleton.GetParentIndex(Index) == 0)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

TSharedRef<FJsonObject> ResultJson(const FAssetDocumentResult& Result)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("success"), Result.IsSuccess());
	Json->SetStringField(TEXT("message"), Result.Message);
	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("path"), Diagnostic.Path);
		Row->SetStringField(TEXT("code"), Diagnostic.Code);
		Row->SetStringField(TEXT("message"), Diagnostic.Message);
		Diagnostics.Add(MakeShared<FJsonValueObject>(Row));
	}
	Json->SetArrayField(TEXT("diagnostics"), Diagnostics);
	if (Result.Payload.IsValid())
	{
		Json->SetObjectField(TEXT("payload"), Result.Payload);
	}
	return Json;
}
} // namespace BoneSamplingPrivate

FAssetDocumentResult SampleBones(const TSharedPtr<FJsonObject>& Request)
{
	using namespace BoneSamplingPrivate;
	if (!Request.IsValid())
	{
		return Fail(TEXT(""), TEXT("AnimationSample.InvalidArgument"), TEXT("The request must be a JSON object."));
	}
	FString AssetPath;
	if (!Request->TryGetStringField(TEXT("asset"), AssetPath) || AssetPath.IsEmpty())
	{
		return Fail(TEXT("/asset"), TEXT("AnimationSample.InvalidArgument"), TEXT("asset (an AnimSequence, AnimComposite or AnimMontage path) is required."));
	}
	UObject* Asset = LoadAsset(AssetPath);
	if (Asset == nullptr)
	{
		return Fail(TEXT("/asset"), TEXT("AnimationSample.AssetNotFound"), FString::Printf(TEXT("No asset at '%s' (find_assets lists animations)."), *AssetPath));
	}
	UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(Asset);
	if (Animation == nullptr)
	{
		return Fail(TEXT("/asset"), TEXT("AnimationSample.NotAnimation"), FString::Printf(TEXT("'%s' is a %s; bones are sampled from an AnimSequence, AnimComposite or AnimMontage."),
			*AssetPath, *Asset->GetClass()->GetName()));
	}
	USkeleton* Skeleton = Animation->GetSkeleton();
	if (Skeleton == nullptr)
	{
		return Fail(TEXT("/asset"), TEXT("AnimationSample.NoSkeleton"), FString::Printf(TEXT("'%s' has no Skeleton."), *AssetPath));
	}
	UAnimMontage* Montage = Cast<UAnimMontage>(Animation);
	TArray<FAssetDocumentDiagnostic> Problems;

	// Optional mesh: its proportions (retargeted as the editor previews on that mesh) and its sockets.
	USkeletalMesh* Mesh = nullptr;
	FString MeshPath;
	if (Request->TryGetStringField(TEXT("mesh"), MeshPath) && !MeshPath.IsEmpty())
	{
		Mesh = Cast<USkeletalMesh>(LoadAsset(MeshPath));
		if (Mesh == nullptr)
		{
			return Fail(TEXT("/mesh"), TEXT("AnimationSample.MeshNotFound"), FString::Printf(TEXT("No skeletal mesh at '%s'."), *MeshPath));
		}
		if (Mesh->GetSkeleton() != Skeleton && (Mesh->GetSkeleton() == nullptr || !Skeleton->IsCompatibleForEditor(Mesh->GetSkeleton())))
		{
			return Fail(TEXT("/mesh"), TEXT("AnimationSample.MeshNotCompatible"), FString::Printf(TEXT("'%s' uses Skeleton %s, not %s (or a compatible one)."),
				*MeshPath, *ObjectPathOf(Mesh->GetSkeleton()), *ObjectPathOf(Skeleton)));
		}
	}
	const FReferenceSkeleton& RefSkeleton = Mesh != nullptr ? Mesh->GetRefSkeleton() : Skeleton->GetReferenceSkeleton();
	if (RefSkeleton.GetNum() == 0)
	{
		return Fail(TEXT("/asset"), TEXT("AnimationSample.NoSkeleton"), TEXT("The skeleton has no bones."));
	}
	const FName RootBone = RefSkeleton.GetBoneName(0);

	// Bones (and sockets).
	const TArray<TSharedPtr<FJsonValue>>* BoneValues = nullptr;
	if (!Request->TryGetArrayField(TEXT("bones"), BoneValues) || BoneValues->IsEmpty())
	{
		return Fail(TEXT("/bones"), TEXT("AnimationSample.InvalidArgument"), TEXT("bones (a list of bone or socket names) is required."));
	}
	if (BoneValues->Num() > MaxBones)
	{
		return Fail(TEXT("/bones"), TEXT("AnimationSample.TooManySamples"), FString::Printf(TEXT("%d bones; at most %d per call."), BoneValues->Num(), MaxBones));
	}
	TArray<FSampledName> Entries;
	for (int32 Index = 0; Index < BoneValues->Num(); ++Index)
	{
		const FString Path = FString::Printf(TEXT("/bones/%d"), Index);
		FString Name;
		if (!(*BoneValues)[Index]->TryGetString(Name) || Name.IsEmpty())
		{
			Problems.Add(Problem(Path, TEXT("AnimationSample.InvalidArgument"), TEXT("Each entry of bones is a bone or socket name.")));
			continue;
		}
		FSampledName Entry;
		Entry.Name = FName(*Name);
		if (Entries.ContainsByPredicate([&Entry](const FSampledName& Each) { return Each.Name == Entry.Name; }))
		{
			continue;
		}
		if (RefSkeleton.FindBoneIndex(Entry.Name) != INDEX_NONE)
		{
			Entry.Bone = Entry.Name;
			Entries.Add(Entry);
			continue;
		}
		const USkeletalMeshSocket* Socket = Mesh != nullptr ? Mesh->FindSocket(Entry.Name) : Skeleton->FindSocket(Entry.Name);
		if (Socket != nullptr && RefSkeleton.FindBoneIndex(Socket->BoneName) != INDEX_NONE)
		{
			Entry.Bone = Socket->BoneName;
			Entry.bSocket = true;
			Entry.SocketLocal = Socket->GetSocketLocalTransform();
			Entries.Add(Entry);
			continue;
		}
		const TArray<FString> Close = CloseBoneNames(RefSkeleton, Name);
		Problems.Add(Problem(Path, TEXT("AnimationSample.BoneNotFound"), FString::Printf(TEXT("'%s' is not a bone or socket of %s%s."), *Name,
			*ObjectPathOf(Mesh != nullptr ? static_cast<UObject*>(Mesh) : Skeleton),
			Close.IsEmpty() ? TEXT(" (inspect_asset_document_target on the Skeleton lists its bones and sockets)") : *FString::Printf(TEXT("; close: %s"), *FString::Join(Close, TEXT(", "))))));
	}

	// Space and root mode.
	ESpace Space = ESpace::Component;
	FString SpaceText;
	if (Request->TryGetStringField(TEXT("space"), SpaceText) && !SpaceText.IsEmpty())
	{
		if (SpaceText.Equals(TEXT("component"), ESearchCase::IgnoreCase)) { Space = ESpace::Component; }
		else if (SpaceText.Equals(TEXT("local"), ESearchCase::IgnoreCase)) { Space = ESpace::Local; }
		else if (SpaceText.Equals(TEXT("root"), ESearchCase::IgnoreCase)) { Space = ESpace::Root; }
		else
		{
			Problems.Add(Problem(TEXT("/space"), TEXT("AnimationSample.InvalidArgument"), FString::Printf(TEXT("space '%s': use component, local or root."), *SpaceText)));
		}
	}
	bool bExtractRootMotion = false;
	FString RootText;
	if (Request->TryGetStringField(TEXT("root"), RootText) && !RootText.IsEmpty())
	{
		if (RootText.Equals(TEXT("extracted"), ESearchCase::IgnoreCase)) { bExtractRootMotion = true; }
		else if (!RootText.Equals(TEXT("pose"), ESearchCase::IgnoreCase))
		{
			Problems.Add(Problem(TEXT("/root"), TEXT("AnimationSample.InvalidArgument"), FString::Printf(TEXT("root '%s': use pose (as authored) or extracted."), *RootText)));
		}
	}
	bool bSpeed = false;
	Request->TryGetBoolField(TEXT("speed"), bSpeed);

	// Montage: the slot track whose segments are sampled.
	const FSlotAnimationTrack* Slot = nullptr;
	if (Montage != nullptr)
	{
		FString SlotName;
		Request->TryGetStringField(TEXT("slot"), SlotName);
		TArray<FString> SlotNames;
		for (const FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
		{
			SlotNames.Add(Track.SlotName.ToString());
			if (Slot == nullptr && (SlotName.IsEmpty() || Track.SlotName == FName(*SlotName)))
			{
				Slot = &Track;
			}
		}
		if (Slot == nullptr)
		{
			Problems.Add(Problem(TEXT("/slot"), TEXT("AnimationSample.SlotNotFound"), FString::Printf(TEXT("The Montage has no slot track '%s' (slots: %s)."),
				*SlotName, *FString::Join(SlotNames, TEXT(", ")))));
		}
	}

	// Times on the asset's timeline.
	const double PlayLength = Animation->GetPlayLength();
	TArray<double> Times;
	const TArray<TSharedPtr<FJsonValue>>* TimeValues = nullptr;
	double Start = 0.0, End = PlayLength, Step = 0.0;
	bool bHasStart = false, bHasEnd = false, bHasStep = false;
	ReadNumber(Request, TEXT("start"), Start, bHasStart, Problems);
	ReadNumber(Request, TEXT("end"), End, bHasEnd, Problems);
	ReadNumber(Request, TEXT("step"), Step, bHasStep, Problems);
	if (Request->TryGetArrayField(TEXT("times"), TimeValues))
	{
		if (bHasStart || bHasEnd || bHasStep)
		{
			Problems.Add(Problem(TEXT("/times"), TEXT("AnimationSample.InvalidArgument"), TEXT("Give times or start/end/step, not both.")));
		}
		if (TimeValues->Num() > MaxTimes)
		{
			return Fail(TEXT("/times"), TEXT("AnimationSample.TooManySamples"), FString::Printf(TEXT("%d times; at most %d per call."), TimeValues->Num(), MaxTimes));
		}
		for (int32 Index = 0; Index < TimeValues->Num(); ++Index)
		{
			double Time = 0.0;
			if (!(*TimeValues)[Index]->TryGetNumber(Time))
			{
				Problems.Add(Problem(FString::Printf(TEXT("/times/%d"), Index), TEXT("AnimationSample.InvalidArgument"), TEXT("Each time is a number of seconds.")));
				continue;
			}
			Times.Add(Time);
		}
	}
	else if (bHasStart || bHasEnd || bHasStep)
	{
		if (!bHasStep)
		{
			const UAnimSequence* Sequence = Cast<UAnimSequence>(Animation);
			const double Rate = Sequence != nullptr ? Sequence->GetSamplingFrameRate().AsDecimal() : 30.0;
			Step = Rate > 0.0 ? 1.0 / Rate : 1.0 / 30.0;
		}
		if (Step <= 0.0)
		{
			Problems.Add(Problem(TEXT("/step"), TEXT("AnimationSample.InvalidArgument"), TEXT("step must be greater than 0.")));
		}
		else if (End < Start)
		{
			Problems.Add(Problem(TEXT("/end"), TEXT("AnimationSample.InvalidArgument"), FString::Printf(TEXT("end %g is before start %g."), End, Start)));
		}
		else
		{
			const double Count = FMath::FloorToDouble((End - Start) / Step + TimeTolerance) + 1.0;
			if (Count > MaxTimes)
			{
				return Fail(TEXT("/step"), TEXT("AnimationSample.TooManySamples"), FString::Printf(TEXT("start %g to end %g by %g is %.0f times; at most %d per call: use a larger step or a shorter range."),
					Start, End, Step, Count, MaxTimes));
			}
			for (int32 Index = 0; Index < static_cast<int32>(Count); ++Index)
			{
				Times.Add(Start + Step * Index);
			}
			if (Times.Last() < End - TimeTolerance)
			{
				Times.Add(End);
			}
		}
	}
	else
	{
		Problems.Add(Problem(TEXT("/times"), TEXT("AnimationSample.InvalidArgument"),
			FString::Printf(TEXT("Give times (seconds) or start/end/step; the asset is %.4f s long."), PlayLength)));
	}
	for (int32 Index = 0; Index < Times.Num(); ++Index)
	{
		if (Times[Index] < -TimeTolerance || Times[Index] > PlayLength + TimeTolerance)
		{
			Problems.Add(Problem(FString::Printf(TEXT("/times/%d"), Index), TEXT("AnimationSample.TimeOutOfRange"),
				FString::Printf(TEXT("Time %g is outside the asset (0 to %.4f s on its timeline)."), Times[Index], PlayLength)));
		}
		Times[Index] = FMath::Clamp(Times[Index], 0.0, PlayLength);
	}
	if (!Problems.IsEmpty())
	{
		return FAssetDocumentResult::FailureFromDiagnostics(Problems, TEXT("Nothing was sampled."));
	}
	if (Entries.Num() * Times.Num() > MaxTransforms)
	{
		return Fail(TEXT("/bones"), TEXT("AnimationSample.TooManySamples"), FString::Printf(TEXT("%d bones x %d times = %d transforms; at most %d per call: use a larger step, fewer bones, or several calls."),
			Entries.Num(), Times.Num(), Entries.Num() * Times.Num(), MaxTransforms));
	}

	// Evaluate: one pass per animation (a montage: per segment animation, at the segment's animation time).
	struct FRowSource
	{
		const UAnimSequenceBase* Animation = nullptr;
		int32 Segment = INDEX_NONE;
		double AnimTime = 0.0;
		int32 PoseIndex = INDEX_NONE;
	};
	TArray<FRowSource> Rows;
	Rows.SetNum(Times.Num());
	TMap<const UAnimSequenceBase*, TArray<double>> TimesByAnimation;
	for (int32 Index = 0; Index < Times.Num(); ++Index)
	{
		FRowSource& Row = Rows[Index];
		if (Montage == nullptr)
		{
			Row.Animation = Animation;
			Row.AnimTime = Times[Index];
		}
		else
		{
			const TArray<FAnimSegment>& Segments = Slot->AnimTrack.AnimSegments;
			for (int32 SegmentIndex = 0; SegmentIndex < Segments.Num(); ++SegmentIndex)
			{
				const FAnimSegment& Segment = Segments[SegmentIndex];
				if (Segment.IsValid() && Segment.GetAnimReference() != nullptr && !Segment.GetAnimReference()->IsA<UAnimMontage>()
					&& Segment.IsInRange(static_cast<float>(Times[Index])))
				{
					Row.Animation = Segment.GetAnimReference();
					Row.Segment = SegmentIndex;
					Row.AnimTime = FMath::Clamp(static_cast<double>(Segment.ConvertTrackPosToAnimPos(static_cast<float>(Times[Index]))), 0.0,
						static_cast<double>(Row.Animation->GetPlayLength()));
					break;
				}
			}
		}
		if (Row.Animation != nullptr)
		{
			TArray<double>& List = TimesByAnimation.FindOrAdd(Row.Animation);
			Row.PoseIndex = List.Add(Row.AnimTime);
		}
	}
	TMap<const UAnimSequenceBase*, TArray<FBoneSamplingPose>> PosesByAnimation;
	for (const TPair<const UAnimSequenceBase*, TArray<double>>& Pair : TimesByAnimation)
	{
		EvaluatePoses(Pair.Key, Pair.Value, bExtractRootMotion, Mesh, PosesByAnimation.Add(Pair.Key));
	}

	// Play time: the asset's timeline at its RateScale (a montage's segments already carry their play rates).
	const double RateScale = Animation->RateScale;
	const bool bScaled = !FMath::IsNearlyEqual(RateScale, 1.0, 1e-4) && !FMath::IsNearlyZero(RateScale);
	auto PlayTimeOf = [RateScale, bScaled, PlayLength](double Time)
	{
		if (!bScaled)
		{
			return Time;
		}
		return RateScale > 0.0 ? Time / RateScale : (PlayLength - Time) / -RateScale;
	};

	TArray<TSharedPtr<FJsonValue>> Samples;
	TMap<FName, FVector> Previous;
	double PreviousPlayTime = 0.0;
	bool bHavePrevious = false;
	for (int32 Index = 0; Index < Times.Num(); ++Index)
	{
		const FRowSource& Row = Rows[Index];
		TSharedRef<FJsonObject> Sample = MakeShared<FJsonObject>();
		Sample->SetNumberField(TEXT("Time"), RoundTo(Times[Index], 0.0001));
		const double PlayTime = PlayTimeOf(Times[Index]);
		if (bScaled)
		{
			Sample->SetNumberField(TEXT("PlayTime"), RoundTo(PlayTime, 0.0001));
		}
		if (Montage != nullptr)
		{
			if (Row.Animation == nullptr)
			{
				Sample->SetBoolField(TEXT("Gap"), true);
				Samples.Add(MakeShared<FJsonValueObject>(Sample));
				bHavePrevious = false;
				continue;
			}
			Sample->SetNumberField(TEXT("Segment"), Row.Segment);
			Sample->SetStringField(TEXT("Anim"), ObjectPathOf(Row.Animation));
			Sample->SetNumberField(TEXT("AnimTime"), RoundTo(Row.AnimTime, 0.0001));
		}
		const FBoneSamplingPose& Pose = PosesByAnimation.FindChecked(Row.Animation)[Row.PoseIndex];
		const double DeltaTime = PlayTime - PreviousPlayTime;
		for (const FSampledName& Entry : Entries)
		{
			const FTransform Transform = InSpace(Pose, Entry, Space, RootBone);
			TSharedRef<FJsonObject> Value = TransformJson(Transform);
			if (bSpeed && bHavePrevious && FMath::Abs(DeltaTime) > KINDA_SMALL_NUMBER)
			{
				Value->SetNumberField(TEXT("Speed"), RoundTo(FVector::Dist(Transform.GetLocation(), Previous.FindRef(Entry.Name)) / FMath::Abs(DeltaTime), 0.1));
			}
			Previous.Add(Entry.Name, Transform.GetLocation());
			Sample->SetObjectField(Entry.Name.ToString(), Value);
		}
		PreviousPlayTime = PlayTime;
		bHavePrevious = true;
		Samples.Add(MakeShared<FJsonValueObject>(Sample));
	}

	// What applied.
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("Asset"), ObjectPathOf(Animation));
	Payload->SetStringField(TEXT("Class"), Animation->GetClass()->GetName());
	Payload->SetStringField(TEXT("Skeleton"), ObjectPathOf(Skeleton));
	if (Mesh != nullptr)
	{
		Payload->SetStringField(TEXT("Mesh"), ObjectPathOf(Mesh));
	}
	Payload->SetNumberField(TEXT("PlayLength"), RoundTo(PlayLength, 0.0001));
	Payload->SetNumberField(TEXT("RateScale"), RateScale);
	if (Slot != nullptr)
	{
		Payload->SetStringField(TEXT("Slot"), Slot->SlotName.ToString());
	}
	Payload->SetStringField(TEXT("Space"), Space == ESpace::Local ? TEXT("local") : Space == ESpace::Root ? TEXT("root") : TEXT("component"));
	TArray<TSharedPtr<FJsonValue>> Applied;
	auto Note = [&Applied](const FString& Text) { Applied.Add(MakeShared<FJsonValueString>(Text)); };
	Note(Space == ESpace::Local ? TEXT("local: relative to the parent bone (a socket: to its bone)")
		: Space == ESpace::Root ? FString::Printf(TEXT("root: relative to the root bone %s"), *RootBone.ToString())
		: TEXT("component: the skeletal mesh component's space (the mesh's own axes, before the actor's mesh offset)"));
	if (bScaled)
	{
		Note(FString::Printf(TEXT("RateScale %g: Time is the asset's timeline (where its notifies are), PlayTime = seconds of play%s"), RateScale,
			bSpeed ? TEXT("; Speed is cm per second of play") : TEXT("")));
	}
	else if (bSpeed)
	{
		Note(TEXT("Speed: cm per second between this sample and the previous one"));
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("Mode"), bExtractRootMotion ? TEXT("extracted") : TEXT("pose"));
	if (const UAnimSequence* Sequence = Cast<UAnimSequence>(Animation))
	{
		Root->SetBoolField(TEXT("EnableRootMotion"), Sequence->bEnableRootMotion);
		Root->SetStringField(TEXT("RootMotionRootLock"), RootLockName(Sequence->RootMotionRootLock.GetValue()));
		Root->SetBoolField(TEXT("ForceRootLock"), Sequence->bForceRootLock);
		const bool bLocked = RootLockApplies(Sequence, bExtractRootMotion);
		Root->SetBoolField(TEXT("RootLocked"), bLocked);
		Note(bLocked
			? FString::Printf(TEXT("root extracted: the root bone is reset to %s (%s)"), Sequence->bEnableRootMotion ? RootLockName(Sequence->RootMotionRootLock.GetValue()) : TEXT("RefPose"),
				Sequence->bEnableRootMotion ? TEXT("bEnableRootMotion") : TEXT("bForceRootLock"))
			: bExtractRootMotion ? FString(TEXT("root extracted: the sequence has neither bEnableRootMotion nor bForceRootLock, so the root moves as authored"))
			: FString(TEXT("root pose: the root bone moves as authored (root motion kept in the pose)")));
		if (Sequence->IsValidAdditive())
		{
			Note(TEXT("additive: applied to its base pose"));
		}
	}
	else
	{
		Note(bExtractRootMotion ? TEXT("root extracted: each segment's sequence resets its root as its root motion settings say")
			: TEXT("root pose: the root bone moves as authored"));
	}
	if (Montage != nullptr)
	{
		Note(TEXT("montage: Time is the montage position; each sample names the segment, its animation and AnimTime"));
	}
	Payload->SetObjectField(TEXT("Root"), Root);
	Payload->SetArrayField(TEXT("Applied"), Applied);
	TArray<TSharedPtr<FJsonValue>> Names;
	TSharedRef<FJsonObject> Sockets = MakeShared<FJsonObject>();
	for (const FSampledName& Entry : Entries)
	{
		Names.Add(MakeShared<FJsonValueString>(Entry.Name.ToString()));
		if (Entry.bSocket)
		{
			Sockets->SetStringField(Entry.Name.ToString(), Entry.Bone.ToString());
		}
	}
	Payload->SetArrayField(TEXT("Bones"), Names);
	if (Sockets->Values.Num() > 0)
	{
		Payload->SetObjectField(TEXT("Sockets"), Sockets);
	}
	Payload->SetArrayField(TEXT("Samples"), Samples);

	FAssetDocumentResult Result = FAssetDocumentResult::Success(FString::Printf(TEXT("%d samples of %d bones"), Samples.Num(), Entries.Num()));
	Result.Payload = Payload;
	return Result;
}

TSharedPtr<FJsonObject> DescribeMotion(const UObject& Asset)
{
	using namespace BoneSamplingPrivate;
	const UAnimSequence* Sequence = Cast<UAnimSequence>(&Asset);
	if (Sequence == nullptr || Sequence->GetSkeleton() == nullptr || Sequence->GetPlayLength() <= 0.0)
	{
		return nullptr;
	}
	const FReferenceSkeleton& RefSkeleton = Sequence->GetSkeleton()->GetReferenceSkeleton();
	if (RefSkeleton.GetNum() == 0)
	{
		return nullptr;
	}
	TArray<FBoneSamplingPose> Poses;
	EvaluatePoses(Sequence, {0.0, static_cast<double>(Sequence->GetPlayLength())}, false, nullptr, Poses);
	if (Poses.Num() != 2 || !Poses[0].IsValid() || !Poses[1].IsValid())
	{
		return nullptr;
	}
	const FName RootBone = RefSkeleton.GetBoneName(0);
	const FTransform RootFirst = Poses[0].Bone(RootBone, true);
	const FTransform RootLast = Poses[1].Bone(RootBone, true);
	TSharedPtr<FJsonObject> Motion = MakeShared<FJsonObject>();
	Motion->SetStringField(TEXT("RootBone"), RootBone.ToString());
	const FVector Displacement = RootLast.GetLocation() - RootFirst.GetLocation();
	Motion->SetField(TEXT("RootDisplacement"), Vector3(Displacement, 0.01));
	Motion->SetNumberField(TEXT("RootDistance"), RoundTo(Displacement.Size(), 0.01));
	Motion->SetNumberField(TEXT("RootYawChange"), RoundTo(FRotator::NormalizeAxis(RootLast.Rotator().Yaw - RootFirst.Rotator().Yaw), 0.01));
	const int32 PelvisIndex = FindPelvis(RefSkeleton);
	if (PelvisIndex != INDEX_NONE)
	{
		const FName Pelvis = RefSkeleton.GetBoneName(PelvisIndex);
		const FVector First = Poses[0].Bone(Pelvis, true).GetLocation();
		const FVector Last = Poses[1].Bone(Pelvis, true).GetLocation();
		TSharedRef<FJsonObject> PelvisJson = MakeShared<FJsonObject>();
		PelvisJson->SetStringField(TEXT("Bone"), Pelvis.ToString());
		PelvisJson->SetField(TEXT("First"), Vector3(First, 0.01));
		PelvisJson->SetField(TEXT("Last"), Vector3(Last, 0.01));
		PelvisJson->SetField(TEXT("Displacement"), Vector3(Last - First, 0.01));
		Motion->SetObjectField(TEXT("Pelvis"), PelvisJson);
	}
	Motion->SetStringField(TEXT("Note"), TEXT("component space at the first and last frame, root as authored; sample_animation_bones samples any bone at any time"));
	return Motion;
}

TSharedPtr<FJsonObject> HandleSampleBonesRoute(const FString& RequestBody, const TMap<FString, FString>& QueryParams, int32& OutStatusCode)
{
	using namespace BoneSamplingPrivate;
	TSharedPtr<FJsonObject> Request;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(RequestBody), Request) || !Request.IsValid())
	{
		OutStatusCode = 400;
		return ResultJson(Fail(TEXT(""), TEXT("AnimationSample.InvalidArgument"), TEXT("The request body must be a JSON object.")));
	}
	FAssetDocumentResult Result;
	auto Run = [&Request, &Result]() { Result = SampleBones(Request); };
	if (IsInGameThread())
	{
		Run();
	}
	else
	{
		FEvent* Done = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [&Run, Done]()
		{
			Run();
			Done->Trigger();
		});
		Done->Wait();
		FPlatformProcess::ReturnSynchEventToPool(Done);
	}
	OutStatusCode = Result.IsSuccess() ? 200 : 400;
	return ResultJson(Result);
}
}
