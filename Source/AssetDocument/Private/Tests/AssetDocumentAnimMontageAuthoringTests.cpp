// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
//
// Montage authoring on the production route
// (FAssetDocumentModule service -> Generic Runtime -> Python profile.anim_montage.v1 -> C++ primitives).
//   - SameAnimationSegments: several segments of one animation (one -> two segments, a notify-only change, a
//     start/end-only change, a forward + reverse loop) apply like the Montage editor's in-place segment edits:
//     segment i of a slot stays segment i (no "Competing/Ambiguous segment correspondence").
//   - SegmentStartTolerance: a StartPos computed in double precision (within 1e-5 s of the engine's float32
//     segment end) is the contiguous start; a real gap is AnimationProvider.Montage.SegmentGap naming the
//     segment and the expected StartPos, at validate as at apply.
//   - SegmentDiagnostics: InvalidSegment / InvalidIntent name the segment or section, the field, the sequence
//     length and the overflow, at validate as at apply.
//   - NotifyTracks: Body.NotifyTracks creates, renames and removes notify tracks (the editor's Insert Notify Track).

#include "AssetDocumentJson.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "AssetDocumentTypes.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ReferenceSkeleton.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentMontageAuthoringTests
{
FAssetDocumentService& MontageAuthoringService() { return FAssetDocumentModule::Get().GetService(); }

FString MontageAuthoringJson(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid()) FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
	return Text;
}

// A copy parsed as the HTTP routes and sidecar files parse documents (numbers keep their literal: 1 is an integer).
TSharedPtr<FJsonObject> MontageAuthoringCopy(const TSharedPtr<FJsonObject>& Object)
{
	TSharedPtr<FJsonObject> Out;
	FAssetDocumentJson::ParseJsonString(MontageAuthoringJson(Object), Out);
	return Out;
}

void MontageAuthoringReport(FAutomationTestBase& Test, const FAssetDocumentResult& Result)
{
	for (const auto& D : Result.Diagnostics) Test.AddInfo(D.Path + TEXT(" ") + D.Code + TEXT(": ") + D.Message);
}

// A diagnostic whose code contains Code, at a path containing Path, whose message contains every Fragment.
bool MontageAuthoringHas(const FAssetDocumentResult& Result, const FString& Path, const FString& Code, TArray<FString> Fragments = {})
{
	return Result.Diagnostics.ContainsByPredicate([&](const FAssetDocumentDiagnostic& D)
	{
		if (!D.Code.Contains(Code) || (!Path.IsEmpty() && !D.Path.Contains(Path))) return false;
		for (const FString& Fragment : Fragments) if (!D.Message.Contains(Fragment)) return false;
		return true;
	});
}

bool MontageAuthoringSave(UObject& Object)
{
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const FString File = FPackageName::LongPackageNameToFilename(Object.GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
	return UPackage::SavePackage(Object.GetOutermost(), &Object, *File, Args);
}

TSharedPtr<FJsonValue> MANum(double Value) { return MakeShared<FJsonValueNumber>(Value); }
TSharedPtr<FJsonValue> MAStr(const FString& Value) { return MakeShared<FJsonValueString>(Value); }
TSharedPtr<FJsonValue> MAObj(const TSharedRef<FJsonObject>& Value) { return MakeShared<FJsonValueObject>(Value); }

struct FMontageAuthoringFixture
{
	FString Root = TEXT("/Game/AssetDocumentTests/MontageAuthoring/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FString Sidecar = FPackageName::LongPackageNameToFilename(Root + TEXT("/Montage"), TEXT(".assetdoc.json"));
	TStrongObjectPtr<USkeleton> Skeleton;
	TStrongObjectPtr<UAnimSequence> Source;
	TStrongObjectPtr<UAnimMontage> Montage;
	TArray<FString> CreatedPackages;

	template <typename T>
	T* Create(const TCHAR* Name)
	{
		T* Asset = NewObject<T>(CreatePackage(*(Root + TEXT("/") + Name)), Name, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Asset);
		return Asset;
	}

	// Skeleton (DefaultSlot), a one-second 30 fps sequence and a Montage with one segment [0, 1] of it and the
	// section "Default" at 0.
	bool Setup(FAutomationTestBase& Test)
	{
		Skeleton.Reset(Create<USkeleton>(TEXT("Skeleton")));
		{
			FReferenceSkeletonModifier Modifier(Skeleton.Get());
			Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
		}
		Skeleton->RegisterSlotNode(TEXT("DefaultSlot"));
		if (!Test.TestTrue(TEXT("save fixture skeleton"), MontageAuthoringSave(*Skeleton))) return false;
		Source.Reset(Create<UAnimSequence>(TEXT("Source")));
		Source->SetSkeleton(Skeleton.Get());
		auto& SourceController = Source->GetController();
		SourceController.InitializeModel();
		SourceController.SetFrameRate(FFrameRate(30, 1), false);
		SourceController.SetNumberOfFrames(FFrameNumber(30), false);
		if (!SourceController.AddBoneCurve(TEXT("root"), false)) return false;
		TArray<FVector3f> Positions, Scales;
		TArray<FQuat4f> Rotations;
		Positions.Init(FVector3f::ZeroVector, 31); Scales.Init(FVector3f::OneVector, 31); Rotations.Init(FQuat4f::Identity, 31);
		if (!SourceController.SetBoneTrackKeys(TEXT("root"), Positions, Rotations, Scales, false)) return false;
		SourceController.NotifyPopulated();
		Source->WaitOnExistingCompression();
		if (!Test.TestTrue(TEXT("save fixture sequence"), MontageAuthoringSave(*Source))) return false;
		Montage.Reset(Create<UAnimMontage>(TEXT("Montage")));
		Montage->SetSkeleton(Skeleton.Get());
		auto& Controller = Montage->GetController();
		Controller.InitializeModel();
		Controller.SetFrameRate(FFrameRate(30, 1), false);
		Controller.SetNumberOfFrames(FFrameNumber(30), false);
		Controller.NotifyPopulated();
		Montage->SlotAnimTracks.Reset();
		auto& Slot = Montage->SlotAnimTracks.AddDefaulted_GetRef();
		Slot.SlotName = TEXT("DefaultSlot");
		FAnimSegment Segment;
		Segment.SetAnimReference(Source.Get());
		Segment.StartPos = 0.f; Segment.AnimStartTime = 0.f; Segment.AnimEndTime = 1.f;
		Segment.AnimPlayRate = 1.f; Segment.LoopingCount = 1;
		Slot.AnimTrack.AnimSegments.Add(Segment);
		Montage->CompositeSections.Reset();
		Montage->AddAnimCompositeSection(TEXT("Default"), 0.f);
		Montage->RefreshCacheData();
		if (!Test.TestTrue(TEXT("save fixture montage"), MontageAuthoringSave(*Montage))) return false;
		return Test.TestEqual(TEXT("fixture montage plays one second"), Montage->GetPlayLength(), 1.f);
	}

	~FMontageAuthoringFixture()
	{
		IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(Root), false, true);
		TArray<UObject*> Assets{Montage.Get(), Source.Get(), Skeleton.Get()};
		for (const FString& Package : CreatedPackages)
			if (UObject* Created = FindObject<UObject>(nullptr, *(Package + TEXT(".") + FPackageName::GetShortName(Package)))) Assets.Insert(Created, 0);
		Montage.Reset(); Source.Reset(); Skeleton.Reset();
		for (UObject* Asset : Assets)
		{
			if (!IsValid(Asset)) continue;
			UPackage* Package = Asset->GetOutermost();
			FAssetRegistryModule::AssetDeleted(Asset);
			Asset->ClearFlags(RF_Public | RF_Standalone);
			Asset->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
			Asset->MarkAsGarbage();
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
		IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(Root), false, true);
	}

	TSharedPtr<FJsonObject> Extract(FAutomationTestBase& Test) const
	{
		FAssetDocumentExtractRequest Request;
		Request.AssetPath = Montage->GetPathName();
		Request.bDiffOnly = false;
		Request.bIncludeAllWritable = true;
		const FAssetDocumentResult Result = MontageAuthoringService().Extract(Request);
		MontageAuthoringReport(Test, Result);
		if (!Test.TestTrue(TEXT("Extract succeeds"), Result.IsSuccess() && Result.Payload.IsValid())) return {};
		return Result.Payload;
	}

	FAssetDocumentResult Validate(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Document) const
	{
		FAssetDocumentValidateRequest Request;
		Request.Document = MontageAuthoringCopy(Document);
		Request.Document->SetStringField(TEXT("Action"), TEXT("Update"));
		const FAssetDocumentResult Result = MontageAuthoringService().Validate(Request);
		MontageAuthoringReport(Test, Result);
		return Result;
	}

	FAssetDocumentResult ApplyAt(FAutomationTestBase& Test, const FString& File, const TSharedPtr<FJsonObject>& Document) const
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
		if (!Test.TestTrue(TEXT("write authoring sidecar"), FFileHelper::SaveStringToFile(MontageAuthoringJson(Document), *File))) return {};
		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = File;
		Request.bSaveAsset = true;
		const FAssetDocumentResult Result = MontageAuthoringService().ApplyFile(Request);
		MontageAuthoringReport(Test, Result);
		return Result;
	}

	FAssetDocumentResult Apply(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Document) const
	{
		Document->SetStringField(TEXT("Action"), TEXT("Update"));
		return ApplyAt(Test, Sidecar, Document);
	}
};

TSharedRef<FJsonObject> MASegment(const FString& Animation, double Start, double SourceStart, double SourceEnd, double Rate = 1.0)
{
	TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetStringField(TEXT("AnimReference"), Animation);
	Row->SetNumberField(TEXT("StartPos"), Start);
	Row->SetNumberField(TEXT("AnimStartTime"), SourceStart);
	Row->SetNumberField(TEXT("AnimEndTime"), SourceEnd);
	Row->SetNumberField(TEXT("AnimPlayRate"), Rate);
	Row->SetNumberField(TEXT("LoopingCount"), 1);
	return Row;
}

void MASetSegments(const TSharedPtr<FJsonObject>& Document, TArray<TSharedRef<FJsonObject>> Segments)
{
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const auto& Segment : Segments) Rows.Add(MAObj(Segment));
	TSharedRef<FJsonObject> Slot = MakeShared<FJsonObject>();
	Slot->SetStringField(TEXT("SlotName"), TEXT("DefaultSlot"));
	Slot->SetArrayField(TEXT("Segments"), Rows);
	Document->GetObjectField(TEXT("Body"))->SetArrayField(TEXT("SlotAnimTracks"), {MAObj(Slot)});
}

TSharedPtr<FJsonValue> MANotifyState(double Time, double Duration, int32 Track = 0)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("Class"), TEXT("/Script/Engine.AnimNotifyState_Trail"));
	Object->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
	TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetNumberField(TEXT("Time"), Time);
	Row->SetNumberField(TEXT("Duration"), Duration);
	Row->SetNumberField(TEXT("TrackIndex"), Track);
	Row->SetObjectField(TEXT("Object"), Object);
	return MAObj(Row);
}

TSharedPtr<FJsonValue> MANotify(double Time, int32 Track)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("Class"), TEXT("/Script/Engine.AnimNotify_PlaySound"));
	Object->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
	TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetNumberField(TEXT("Time"), Time);
	Row->SetNumberField(TEXT("TrackIndex"), Track);
	Row->SetObjectField(TEXT("Object"), Object);
	return MAObj(Row);
}

TSharedPtr<FJsonValue> MASection(const FString& Name, double Time)
{
	TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetStringField(TEXT("Name"), Name);
	Row->SetField(TEXT("NextSectionName"), MakeShared<FJsonValueNull>());
	Row->SetNumberField(TEXT("LinkableTime"), Time);
	Row->SetStringField(TEXT("SlotName"), TEXT("DefaultSlot"));
	Row->SetNumberField(TEXT("LinkMethod"), 0);
	return MAObj(Row);
}

// Rejects this package's staging save once (after every apply step published), so the apply rolls back.
class FMontageAuthoringSaveVeto
{
	FCoreUObjectDelegates::FIsPackageOKToSaveDelegate Previous;
	FString Package;
public:
	bool bTriggered = false;
	explicit FMontageAuthoringSaveVeto(FString InPackage)
		: Previous(FCoreUObjectDelegates::IsPackageOKToSaveDelegate), Package(MoveTemp(InPackage))
	{
		FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindLambda([this](UPackage* Saved, const FString& Filename, FOutputDevice* Error)
		{
			if (Previous.IsBound() && !Previous.Execute(Saved, Filename, Error)) return false;
			if (!bTriggered && Saved && Saved->GetName() == Package
				&& FPaths::ConvertRelativePathToFull(Filename).Contains(TEXT("/Saved/AssetDocument/Runtime/Staging/")))
			{
				bTriggered = true;
				return false;
			}
			return true;
		});
	}
	~FMontageAuthoringSaveVeto() { FCoreUObjectDelegates::IsPackageOKToSaveDelegate = Previous; }
};

// The live segments of the one slot, as (StartPos, AnimStartTime, AnimEndTime, AnimPlayRate).
TArray<FVector4f> MALiveSegments(const UAnimMontage& Montage)
{
	TArray<FVector4f> Out;
	if (Montage.SlotAnimTracks.Num() == 1)
		for (const FAnimSegment& Segment : Montage.SlotAnimTracks[0].AnimTrack.AnimSegments)
			Out.Emplace(Segment.StartPos, Segment.AnimStartTime, Segment.AnimEndTime, Segment.AnimPlayRate);
	return Out;
}
} // namespace AssetDocumentMontageAuthoringTests


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimMontageAuthoringSameAnimationSegmentsTest,
	"UEAssetDoc.AnimMontage.Authoring.SameAnimationSegments",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimMontageAuthoringSameAnimationSegmentsTest::RunTest(const FString&)
{
	using namespace AssetDocumentMontageAuthoringTests;
	FMontageAuthoringFixture Fixture;
	if (!Fixture.Setup(*this)) return false;
	const FString Animation = Fixture.Source->GetPathName();
	TSharedPtr<FJsonObject> Document = Fixture.Extract(*this);
	if (!Document) return false;
	TSharedPtr<FJsonObject> Body = Document->GetObjectField(TEXT("Body"));
	auto ValidateThenApply = [&](const TCHAR* Step)
	{
		const FAssetDocumentResult Validated = Fixture.Validate(*this, Document);
		TestTrue(*FString::Printf(TEXT("%s: validate passes"), Step), Validated.IsSuccess());
		const FAssetDocumentResult Applied = Fixture.Apply(*this, Document);
		TestTrue(*FString::Printf(TEXT("%s: apply passes"), Step), Applied.IsSuccess());
		TestFalse(*FString::Printf(TEXT("%s: no segment correspondence refusal"), Step),
			MontageAuthoringHas(Applied, FString(), TEXT("DependencyUnavailable")) || MontageAuthoringHas(Applied, FString(), TEXT("Correspondence")));
		return Applied.IsSuccess();
	};
	// One segment -> two segments of the same animation (a slow start), with a notify state.
	MASetSegments(Document, {MASegment(Animation, 0.0, 0.0, 0.3, 0.5), MASegment(Animation, 0.6, 0.3, 1.0)});
	Body->SetArrayField(TEXT("NotifyStates"), {MANotifyState(0.2, 0.3)});
	if (!ValidateThenApply(TEXT("one to two segments of one animation"))) return false;
	TestEqual(TEXT("two live segments"), MALiveSegments(*Fixture.Montage).Num(), 2);
	// Afterwards, a notify-only change of that Montage (was "Ambiguous segment correspondence").
	Body->SetArrayField(TEXT("NotifyStates"), {MANotifyState(0.25, 0.3)});
	if (!ValidateThenApply(TEXT("notify-only change"))) return false;
	// Only the segments' start/end times change.
	MASetSegments(Document, {MASegment(Animation, 0.0, 0.0, 0.25, 0.5), MASegment(Animation, 0.5, 0.25, 1.0)});
	if (!ValidateThenApply(TEXT("start/end-only change"))) return false;
	const TArray<FVector4f> Trimmed = MALiveSegments(*Fixture.Montage);
	TestTrue(TEXT("the trimmed segments are live"), Trimmed.Num() == 2 && Trimmed[0].Z == 0.25f && Trimmed[1].X == 0.5f && Trimmed[1].Y == 0.25f);
	// Three segments of one animation, hit + forward + reverse (negative rate) loop.
	MASetSegments(Document, {MASegment(Animation, 0.0, 0.0, 0.4), MASegment(Animation, 0.4, 0.4, 1.0),
		MASegment(Animation, 1.0, 0.4, 1.0, -0.4)});
	Body->SetArrayField(TEXT("CompositeSections"), {MASection(TEXT("Default"), 0.0), MASection(TEXT("Loop"), 0.4)});
	if (!ValidateThenApply(TEXT("forward + reverse loop of one animation"))) return false;
	const TArray<FVector4f> Loop = MALiveSegments(*Fixture.Montage);
	TestTrue(TEXT("the reverse segment is live"), Loop.Num() == 3 && Loop[2].W == -0.4f && Loop[2].X == 1.f);
	TestTrue(TEXT("the Montage is 2.5 s long"), FMath::IsNearlyEqual(Fixture.Montage->GetPlayLength(), 2.5f, 1e-4f));
	// Re-applying the same document changes nothing.
	const FAssetDocumentResult Again = Fixture.Apply(*this, Document);
	TestTrue(TEXT("re-apply passes"), Again.IsSuccess());
	TestFalse(TEXT("re-apply saves nothing"), Again.bSavedAsset);
	// A sparse document with only SlotAnimTracks keeps the sections and notifies (absolute links keep their time,
	// as in the editor), with segments of the same animation in the live Montage and in the document.
	TSharedPtr<FJsonObject> Sparse = MontageAuthoringCopy(Document);
	TSharedRef<FJsonObject> SparseBody = MakeShared<FJsonObject>();
	Sparse->SetObjectField(TEXT("Body"), SparseBody);
	MASetSegments(Sparse, {MASegment(Animation, 0.0, 0.0, 0.5), MASegment(Animation, 0.5, 0.5, 1.0)});
	TestTrue(TEXT("sparse slot edit: validate passes"), Fixture.Validate(*this, Sparse).IsSuccess());
	const FAssetDocumentResult SparseApplied = Fixture.Apply(*this, Sparse);
	TestTrue(TEXT("sparse slot edit applies"), SparseApplied.IsSuccess());
	TestEqual(TEXT("the sections are kept"), Fixture.Montage->CompositeSections.Num(), 2);
	TestEqual(TEXT("the notify state is kept"), Fixture.Montage->Notifies.Num(), 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimMontageAuthoringSegmentStartToleranceTest,
	"UEAssetDoc.AnimMontage.Authoring.SegmentStartTolerance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimMontageAuthoringSegmentStartToleranceTest::RunTest(const FString&)
{
	using namespace AssetDocumentMontageAuthoringTests;
	FMontageAuthoringFixture Fixture;
	if (!Fixture.Setup(*this)) return false;
	const FString Animation = Fixture.Source->GetPathName();
	TSharedPtr<FJsonObject> Document = Fixture.Extract(*this);
	if (!Document) return false;
	// The second StartPos is the first segment's length computed in double precision (0.666 / 0.595 =
	// 1.11932773...); the engine's float32 length is one float32 step lower.
	const double DoubleStart = 0.666 / 0.595;
	const float EngineStart = (1.f * (0.666f - 0.f)) / FMath::Abs(0.595f);
	if (!TestTrue(TEXT("the double-precision start is not the engine's float32 start"), float(DoubleStart) != EngineStart)) return false;
	MASetSegments(Document, {MASegment(Animation, 0.0, 0.0, 0.666, 0.595), MASegment(Animation, DoubleStart, 0.0, 0.5)});
	TestTrue(TEXT("validate accepts the double-precision start"), Fixture.Validate(*this, Document).IsSuccess());
	const FAssetDocumentResult Applied = Fixture.Apply(*this, Document);
	if (!TestTrue(TEXT("apply accepts the double-precision start"), Applied.IsSuccess())) return false;
	const TArray<FVector4f> Live = MALiveSegments(*Fixture.Montage);
	TestTrue(TEXT("the second segment starts exactly at the first one's float32 end (as the editor and a reload place it)"),
		Live.Num() == 2 && Live[1].X == Live[0].X + Fixture.Montage->SlotAnimTracks[0].AnimTrack.AnimSegments[0].GetLength());
	const FAssetDocumentResult Again = Fixture.Apply(*this, Document);
	TestTrue(TEXT("re-apply passes"), Again.IsSuccess());
	TestFalse(TEXT("re-apply saves nothing"), Again.bSavedAsset);
	// A real gap (here 0.0005 s, more than the 1e-5 s tolerance) is its own code, naming the segment and the StartPos
	// the engine would give it, at validate as at apply.
	MASetSegments(Document, {MASegment(Animation, 0.0, 0.0, 0.5), MASegment(Animation, 0.5005, 0.0, 0.5)});
	for (const bool bValidate : {true, false})
	{
		const FAssetDocumentResult Gap = bValidate ? Fixture.Validate(*this, Document) : Fixture.Apply(*this, Document);
		const TCHAR* Phase = bValidate ? TEXT("validate") : TEXT("apply");
		TestFalse(*FString::Printf(TEXT("%s refuses the gap"), Phase), Gap.IsSuccess());
		TestTrue(*FString::Printf(TEXT("%s: SegmentGap at the StartPos, with the expected start"), Phase),
			MontageAuthoringHas(Gap, TEXT("/Body/SlotAnimTracks/0/Segments/1/StartPos"), TEXT("Montage.SegmentGap"), {TEXT("0.5005"), TEXT("0.5")}));
		TestFalse(*FString::Printf(TEXT("%s: not DependencyUnavailable"), Phase), MontageAuthoringHas(Gap, FString(), TEXT("DependencyUnavailable")));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimMontageAuthoringSegmentDiagnosticsTest,
	"UEAssetDoc.AnimMontage.Authoring.SegmentDiagnostics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimMontageAuthoringSegmentDiagnosticsTest::RunTest(const FString&)
{
	using namespace AssetDocumentMontageAuthoringTests;
	FMontageAuthoringFixture Fixture;
	if (!Fixture.Setup(*this)) return false;
	const FString Animation = Fixture.Source->GetPathName();
	TSharedPtr<FJsonObject> Document = Fixture.Extract(*this);
	if (!Document) return false;
	// AnimEndTime a little after the end of the one-second sequence.
	MASetSegments(Document, {MASegment(Animation, 0.0, 0.0, 1.001)});
	for (const bool bValidate : {true, false})
	{
		const FAssetDocumentResult Overflow = bValidate ? Fixture.Validate(*this, Document) : Fixture.Apply(*this, Document);
		const TCHAR* Phase = bValidate ? TEXT("validate") : TEXT("apply");
		TestFalse(*FString::Printf(TEXT("%s refuses the overflow"), Phase), Overflow.IsSuccess());
		TestTrue(*FString::Printf(TEXT("%s: InvalidSegment names the field, the sequence length and the overflow"), Phase),
			MontageAuthoringHas(Overflow, TEXT("/Body/SlotAnimTracks/0/Segments/0/AnimEndTime"), TEXT("Montage.InvalidSegment"),
				{TEXT("AnimEndTime 1.001"), TEXT("1 s"), TEXT("0.001")}));
	}
	// A section after the end of a (shorter) slot timeline names the section and the length.
	MASetSegments(Document, {MASegment(Animation, 0.0, 0.0, 0.5)});
	Document->GetObjectField(TEXT("Body"))->SetArrayField(TEXT("CompositeSections"), {MASection(TEXT("Default"), 0.0), MASection(TEXT("Attack"), 0.8)});
	for (const bool bValidate : {true, false})
	{
		const FAssetDocumentResult Beyond = bValidate ? Fixture.Validate(*this, Document) : Fixture.Apply(*this, Document);
		const TCHAR* Phase = bValidate ? TEXT("validate") : TEXT("apply");
		TestFalse(*FString::Printf(TEXT("%s refuses the section"), Phase), Beyond.IsSuccess());
		TestTrue(*FString::Printf(TEXT("%s: the section, its time and the Montage length"), Phase),
			MontageAuthoringHas(Beyond, TEXT("/Body/CompositeSections/1"), TEXT("Montage.InvalidIntent"), {TEXT("'Attack'"), TEXT("0.8"), TEXT("0.5")}));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimMontageAuthoringNotifyTracksTest,
	"UEAssetDoc.AnimMontage.Authoring.NotifyTracks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAnimMontageAuthoringNotifyTracksTest::RunTest(const FString&)
{
	using namespace AssetDocumentMontageAuthoringTests;
	FMontageAuthoringFixture Fixture;
	if (!Fixture.Setup(*this)) return false;
	TSharedPtr<FJsonObject> Document = Fixture.Extract(*this);
	if (!Document) return false;
	TSharedPtr<FJsonObject> Body = Document->GetObjectField(TEXT("Body"));
	const TArray<TSharedPtr<FJsonValue>>* Extracted = nullptr;
	if (!TestTrue(TEXT("Extract lists the notify tracks"), Body->TryGetArrayField(TEXT("NotifyTracks"), Extracted))) return false;
	TestTrue(TEXT("a Montage starts with the editor's track \"1\""), Extracted->Num() == 1
		&& (*Extracted)[0]->AsObject()->GetStringField(TEXT("Name")) == TEXT("1"));
	auto Track = [](const TCHAR* Name)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Name"), Name);
		return MAObj(Row);
	};
	auto LiveTracks = [&Fixture]()
	{
		TArray<FString> Names;
		for (const FAnimNotifyTrack& Item : Fixture.Montage->AnimNotifyTracks) Names.Add(Item.TrackName.ToString());
		return FString::Join(Names, TEXT(","));
	};
	// Four tracks (Danger / Perfect / Hit / Cancel) with a notify on each.
	Body->SetArrayField(TEXT("NotifyTracks"), {Track(TEXT("Danger")), Track(TEXT("Perfect")), Track(TEXT("Hit")), Track(TEXT("Cancel"))});
	Body->SetArrayField(TEXT("Notifies"), {MANotify(0.1, 0), MANotify(0.2, 1), MANotify(0.3, 2)});
	Body->SetArrayField(TEXT("NotifyStates"), {MANotifyState(0.4, 0.2, 3)});
	TestTrue(TEXT("validate passes"), Fixture.Validate(*this, Document).IsSuccess());
	const FAssetDocumentResult Applied = Fixture.Apply(*this, Document);
	if (!TestTrue(TEXT("notifies on new tracks apply"), Applied.IsSuccess())) return false;
	TestEqual(TEXT("the live tracks"), LiveTracks(), FString(TEXT("Danger,Perfect,Hit,Cancel")));
	TestTrue(TEXT("each notify is on its track"), Fixture.Montage->Notifies.Num() == 4
		&& Fixture.Montage->AnimNotifyTracks[1].Notifies.Num() == 1 && Fixture.Montage->AnimNotifyTracks[3].Notifies.Num() == 1);
	const TSharedPtr<FJsonObject> Reread = Fixture.Extract(*this);
	const TArray<TSharedPtr<FJsonValue>>* Read = nullptr;
	TestTrue(TEXT("Extract reads the tracks back"), Reread && Reread->GetObjectField(TEXT("Body"))->TryGetArrayField(TEXT("NotifyTracks"), Read)
		&& Read->Num() == 4 && (*Read)[3]->AsObject()->GetStringField(TEXT("Name")) == TEXT("Cancel"));
	const FAssetDocumentResult Again = Fixture.Apply(*this, Document);
	TestTrue(TEXT("re-apply passes"), Again.IsSuccess());
	TestFalse(TEXT("re-apply saves nothing"), Again.bSavedAsset);
	// Rename one track and drop the last one (its notify state moves to track 0).
	Body->SetArrayField(TEXT("NotifyTracks"), {Track(TEXT("Danger")), Track(TEXT("Parry")), Track(TEXT("Hit"))});
	Body->SetArrayField(TEXT("NotifyStates"), {MANotifyState(0.4, 0.2, 0)});
	TestTrue(TEXT("rename and remove apply"), Fixture.Apply(*this, Document).IsSuccess());
	TestEqual(TEXT("the renamed tracks"), LiveTracks(), FString(TEXT("Danger,Parry,Hit")));
	// A failed apply (here its staging save) puts the tracks back with the notifies on them.
	{
		Body->SetArrayField(TEXT("NotifyTracks"), {Track(TEXT("A")), Track(TEXT("B")), Track(TEXT("C")), Track(TEXT("D"))});
		Body->SetArrayField(TEXT("NotifyStates"), {MANotifyState(0.4, 0.2, 3)});
		FMontageAuthoringSaveVeto Veto(Fixture.Montage->GetOutermost()->GetName());
		const FAssetDocumentResult Vetoed = Fixture.Apply(*this, Document);
		TestTrue(TEXT("the staging save veto ran"), Veto.bTriggered);
		TestFalse(TEXT("the vetoed apply fails"), Vetoed.IsSuccess());
	}
	TestEqual(TEXT("rollback restores the tracks"), LiveTracks(), FString(TEXT("Danger,Parry,Hit")));
	TestTrue(TEXT("rollback restores the notifies on their tracks"), Fixture.Montage->Notifies.Num() == 4
		&& Fixture.Montage->AnimNotifyTracks[0].Notifies.Num() == 2 && Fixture.Montage->AnimNotifyTracks[1].Notifies.Num() == 1
		&& Fixture.Montage->AnimNotifyTracks[2].Notifies.Num() == 1);
	Body->SetArrayField(TEXT("NotifyTracks"), {Track(TEXT("Danger")), Track(TEXT("Parry")), Track(TEXT("Hit"))});
	// A TrackIndex past the listed tracks is refused at validate, naming the track count.
	Body->SetArrayField(TEXT("NotifyStates"), {MANotifyState(0.4, 0.2, 3)});
	const FAssetDocumentResult Missing = Fixture.Validate(*this, Document);
	TestFalse(TEXT("TrackIndex past NotifyTracks is refused"), Missing.IsSuccess());
	TestTrue(TEXT("NotifyTrackMissing names the tracks"), MontageAuthoringHas(Missing, TEXT("/Body/NotifyStates/0/TrackIndex"), TEXT("NotifyTrackMissing"), {TEXT("3")}));
	// A new Montage lists its tracks in its Create.
	const FString NewPackage = Fixture.Root + TEXT("/Created");
	Fixture.CreatedPackages.Add(NewPackage);
	TSharedPtr<FJsonObject> Create = MontageAuthoringCopy(Document);
	Create->SetStringField(TEXT("Target"), NewPackage);
	Create->SetStringField(TEXT("Action"), TEXT("Create"));
	Create->GetObjectField(TEXT("Body"))->SetArrayField(TEXT("NotifyStates"), {MANotifyState(0.4, 0.2, 2)});
	const FAssetDocumentResult Created = Fixture.ApplyAt(*this, FPackageName::LongPackageNameToFilename(NewPackage, TEXT(".assetdoc.json")), Create);
	TestTrue(TEXT("a Create with NotifyTracks applies"), Created.IsSuccess());
	const UAnimMontage* New = FindObject<UAnimMontage>(nullptr, *(NewPackage + TEXT(".Created")));
	if (!TestNotNull(TEXT("the created Montage"), New)) return false;
	TestEqual(TEXT("the created Montage has the three tracks"), New->AnimNotifyTracks.Num(), 3);
	// Track 2 'Hit' holds the notify at 0.3 s and the notify state at 0.4 s.
	TestTrue(TEXT("the created Montage's track 2 is 'Hit' with its two notifies"), New->AnimNotifyTracks.Num() == 3
		&& New->AnimNotifyTracks[2].TrackName == FName(TEXT("Hit")) && New->AnimNotifyTracks[2].Notifies.Num() == 2);
	return !HasAnyErrors();
}

#endif
