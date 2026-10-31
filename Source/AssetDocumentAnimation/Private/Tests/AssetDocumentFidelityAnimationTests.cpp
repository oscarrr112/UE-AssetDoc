// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Editor-made animation content the documents carry beyond the basic regions: a sequence's transform curves and
// animated bone attributes (read-only lists, kept by every apply) and an Anim Composite's curves, notify tracks and
// notifies (authored as on a sequence); a blend space axis's analysis function and settings; a Montage's asset user
// data (in Properties) and a Montage with two tracks on one slot (Persona's Duplicate Slot). Each asset is built through the editor's own calls, saved, read back, and
// edited through the production Service.

#include "AssetDocumentTolerantTestSupport.h"

#include "Animation/AnimComposite.h"
#include "Animation/AnimData/AttributeIdentifier.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/BuiltInAttributeTypes.h"
#include "Animation/Skeleton.h"
#include "BlendSpaceAnalysis.h"
#include "Engine/AssetUserData.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentFidelityAnimationTestsLocal
{
using namespace AssetDocumentTolerantTestSupport;

const TCHAR* FidelitySkeleton = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
const TCHAR* FidelityIdleAnim = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");

// The Content Browser's Duplicate of an engine sequence into the target's package (the engine asset is never edited).
UAnimSequence* FidelitySequenceCopy(const FTarget& Target, const TCHAR* SourcePath)
{
	UAnimSequence* Source = LoadObject<UAnimSequence>(nullptr, SourcePath);
	if (Source == nullptr)
	{
		return nullptr;
	}
	UAnimSequence* Copy = DuplicateObject<UAnimSequence>(Source, CreatePackage(*Target.PackageName), *Target.AssetName);
	Copy->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Copy);
	Copy->MarkPackageDirty();
	return Copy;
}

FString FidelityStringAt(const TSharedPtr<FJsonObject>& Document, const TCHAR* Pointer)
{
	const TSharedPtr<FJsonValue> Value = At(Document, Pointer);
	return Value.IsValid() && Value->Type == EJson::String ? Value->AsString() : FString();
}

double FidelityNumberAt(const TSharedPtr<FJsonObject>& Document, const TCHAR* Pointer)
{
	const TSharedPtr<FJsonValue> Value = At(Document, Pointer);
	double Number = -1.0;
	return Value.IsValid() && Value->TryGetNumber(Number) ? Number : -1.0;
}

TSharedPtr<FJsonObject> FidelityRow(const TSharedPtr<FJsonObject>& Document, const TCHAR* Region, int32 Index)
{
	const TSharedPtr<FJsonValue> Row = At(Document, FString::Printf(TEXT("/Body/%s/%d"), Region, Index));
	return Row.IsValid() && Row->Type == EJson::Object ? Row->AsObject() : nullptr;
}

// A Montage on a saved copy of the engine skeleton (which lists DefaultSlot and plays the engine animations), with
// one DefaultSlot track playing half a second of Idle and a Default section, as the Montage editor makes it.
UAnimMontage* FidelityMontage(FAutomationTestBase& Test, const FTarget& SkeletonTarget, const FTarget& Target)
{
	USkeleton* EngineSkeletonAsset = LoadObject<USkeleton>(nullptr, FidelitySkeleton);
	UAnimSequence* Idle = LoadObject<UAnimSequence>(nullptr, FidelityIdleAnim);
	if (!Test.TestNotNull(TEXT("engine skeleton"), EngineSkeletonAsset) || !Test.TestNotNull(TEXT("idle"), Idle))
	{
		return nullptr;
	}
	USkeleton* Skeleton = DuplicateObject<USkeleton>(EngineSkeletonAsset, CreatePackage(*SkeletonTarget.PackageName), *SkeletonTarget.AssetName);
	Skeleton->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Skeleton);
	Skeleton->AddCompatibleSkeleton(EngineSkeletonAsset);
	Skeleton->RegisterSlotNode(FAnimSlotGroup::DefaultSlotName);
	Skeleton->MarkPackageDirty();
	if (!Test.TestTrue(TEXT("skeleton saved"), SkeletonTarget.Save()))
	{
		return nullptr;
	}
	UAnimMontage* Montage = Target.Create<UAnimMontage>();
	Montage->SetSkeleton(Skeleton);
	Montage->SlotAnimTracks.Reset();
	Montage->CompositeSections.Reset();
	FSlotAnimationTrack& Track = Montage->SlotAnimTracks.AddDefaulted_GetRef();
	Track.SlotName = FAnimSlotGroup::DefaultSlotName;
	FAnimSegment& Segment = Track.AnimTrack.AnimSegments.AddDefaulted_GetRef();
	Segment.SetAnimReference(Idle, true);
	Segment.AnimStartTime = 0.f;
	Segment.AnimEndTime = 0.5f;
	Segment.AnimPlayRate = 1.f;
	Segment.LoopingCount = 1;
	Segment.StartPos = 0.f;
	IAnimationDataController& Controller = Montage->GetController();
	Controller.InitializeModel();
	Controller.SetFrameRate(FFrameRate(30, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(15), false);
	Controller.NotifyPopulated();
	FCompositeSection& Section = Montage->CompositeSections.AddDefaulted_GetRef();
	Section.SectionName = TEXT("Default");
	Section.Link(Montage, 0.f, 0);
	return Montage;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityAnimSequenceTransformCurvesTest,
	"UEAssetDoc.Fidelity.AnimSequence.TransformCurvesAndAttributes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelityAnimSequenceTransformCurvesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentFidelityAnimationTestsLocal;
	const FTarget Target(TEXT("AS_Fidelity"));
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UAnimSequence* Sequence = FidelitySequenceCopy(Target, FidelityIdleAnim);
	if (!TestNotNull(TEXT("sequence copy"), Sequence))
	{
		return false;
	}
	// A bone offset keyed on the additive layer tracks (the Animation editor's "Add Key" on a bone track), and an
	// integer attribute on the root bone as an import of timecode writes it.
	IAnimationDataController& Controller = Sequence->GetController();
	const FAnimationCurveIdentifier PelvisCurve(TEXT("pelvis"), ERawCurveTrackTypes::RCT_Transform);
	const FAnimationAttributeIdentifier FrameAttribute = UAnimationAttributeIdentifierExtensions::CreateAttributeIdentifier(
		Sequence, TEXT("TCFrame"), TEXT("root"), FIntegerAnimationAttribute::StaticStruct());
	FIntegerAnimationAttribute FrameValue;
	FrameValue.Value = 12;
	Controller.OpenBracket(NSLOCTEXT("AssetDocumentFidelityTests", "Keys", "Fixture keys"), false);
	const bool bCurve = Controller.AddCurve(PelvisCurve, AACF_DefaultCurve, false)
		&& Controller.SetTransformCurveKey(PelvisCurve, 0.f, FTransform::Identity, false)
		&& Controller.SetTransformCurveKey(PelvisCurve, 0.5f, FTransform(FVector(0.0, 0.0, 10.0)), false);
	const bool bAttribute = FrameAttribute.IsValid() && Controller.AddAttribute(FrameAttribute, false)
		&& Controller.SetTypedAttributeKey<FIntegerAnimationAttribute>(FrameAttribute, 0.f, FrameValue, false);
	Controller.CloseBracket(false);
	if (!TestTrue(TEXT("transform curve keyed"), bCurve) || !TestTrue(TEXT("attribute keyed"), bAttribute)
		|| !TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Document = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Sequence"));
	if (!Document.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("transform curve listed"), FidelityStringAt(Document, TEXT("/Body/TransformCurves/0/Name")), FString(TEXT("pelvis")));
	TestEqual(TEXT("its key count"), FidelityNumberAt(Document, TEXT("/Body/TransformCurves/0/KeyCount")), 2.0);
	TestEqual(TEXT("its last key time"), FidelityNumberAt(Document, TEXT("/Body/TransformCurves/0/End")), 0.5, 1e-4);
	TestEqual(TEXT("attribute bone"), FidelityStringAt(Document, TEXT("/Body/Attributes/0/Bone")), FString(TEXT("root")));
	TestEqual(TEXT("attribute name"), FidelityStringAt(Document, TEXT("/Body/Attributes/0/Name")), FString(TEXT("TCFrame")));
	TestEqual(TEXT("attribute type"), FidelityStringAt(Document, TEXT("/Body/Attributes/0/Type")),
		FIntegerAnimationAttribute::StaticStruct()->GetPathName());
	TestEqual(TEXT("attribute key count"), FidelityNumberAt(Document, TEXT("/Body/Attributes/0/KeyCount")), 1.0);

	// Another edit keeps both.
	if (TestTrue(TEXT("rate scale edit applies"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Edited)
	{
		Edited->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("Playback"))->SetNumberField(TEXT("RateScale"), 1.5);
	}, TEXT("Edit")).IsSuccess()))
	{
		Sequence = Target.Get<UAnimSequence>();
		const IAnimationDataModel* Model = Sequence != nullptr ? Sequence->GetDataModel() : nullptr;
		if (TestNotNull(TEXT("model"), Model))
		{
			TestEqual(TEXT("rate scale"), Sequence->RateScale, 1.5f);
			TestEqual(TEXT("transform curve kept"), Model->GetTransformCurves().Num(), 1);
			TestEqual(TEXT("attribute kept"), Model->GetAttributes().Num(), 1);
		}
	}

	// The lists are read-only: a changed row is refused.
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Edited)
	{
		FidelityRow(Edited, TEXT("TransformCurves"), 0)->SetNumberField(TEXT("KeyCount"), 3);
	}, TEXT("Changed transform curve")), TEXT("AnimSequence.ReadOnlyRegion"), TEXT("/Body/TransformCurves"), TEXT("Changed transform curve"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityAnimCompositeTimelineTest,
	"UEAssetDoc.Fidelity.AnimComposite.Timeline", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelityAnimCompositeTimelineTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentFidelityAnimationTestsLocal;
	const FTarget Target(TEXT("AC_Fidelity"));
	ON_SCOPE_EXIT { Target.Cleanup(); };

	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, FidelitySkeleton);
	UAnimSequence* Idle = LoadObject<UAnimSequence>(nullptr, FidelityIdleAnim);
	if (!TestNotNull(TEXT("skeleton"), Skeleton) || !TestNotNull(TEXT("idle"), Idle))
	{
		return false;
	}
	UAnimComposite* Composite = Target.Create<UAnimComposite>();
	Composite->SetSkeleton(Skeleton);
	FAnimSegment Segment;
	Segment.SetAnimReference(Idle, true);
	Segment.StartPos = 0.f;
	Composite->AnimationTrack.AnimSegments.Add(Segment);
	Composite->SetCompositeLength(Composite->AnimationTrack.GetLength());
	// The Notifies panel: one track and a named notify on it; the Curves panel: a float curve with two keys.
	Composite->AnimNotifyTracks.Reset();
	Composite->AnimNotifyTracks.Add(FAnimNotifyTrack(TEXT("1"), FLinearColor::White));
	FAnimNotifyEvent& Event = Composite->Notifies.AddDefaulted_GetRef();
	Event.NotifyName = TEXT("Footstep");
	Event.Link(Composite, 0.25f);
	Event.TriggerTimeOffset = GetTriggerTimeOffsetForType(Composite->CalculateOffsetForNotify(0.25f));
	Event.TrackIndex = 0;
	Event.Guid = FGuid::NewGuid();
	const FAnimationCurveIdentifier SpeedCurve(TEXT("Speed"), ERawCurveTrackTypes::RCT_Float);
	IAnimationDataController& Controller = Composite->GetController();
	const bool bCurve = Controller.AddCurve(SpeedCurve, AACF_DefaultCurve, false)
		&& Controller.SetCurveKey(SpeedCurve, FRichCurveKey(0.f, 0.f), false)
		&& Controller.SetCurveKey(SpeedCurve, FRichCurveKey(0.25f, 1.f), false);
	Composite->RefreshCacheData();
	if (!TestTrue(TEXT("curve keyed"), bCurve) || !TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Document = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Composite"));
	if (!Document.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("notify track listed"), FidelityStringAt(Document, TEXT("/Body/NotifyTracks/0/Name")), FString(TEXT("1")));
	TestEqual(TEXT("notify listed"), FidelityStringAt(Document, TEXT("/Body/Notifies/0/Name")), FString(TEXT("Footstep")));
	TestEqual(TEXT("notify time"), FidelityNumberAt(Document, TEXT("/Body/Notifies/0/Time")), 0.25, 1e-4);
	TestEqual(TEXT("curve listed"), FidelityStringAt(Document, TEXT("/Body/Curves/0/Name")), FString(TEXT("Speed")));

	// A new track; the notify moves to it and to a later time. The curve is kept.
	if (TestTrue(TEXT("timeline edit applies"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Edited)
	{
		TArray<TSharedPtr<FJsonValue>> Tracks = Edited->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("NotifyTracks"));
		const TSharedPtr<FJsonObject> Feet = MakeShared<FJsonObject>();
		Feet->SetStringField(TEXT("Name"), TEXT("Feet"));
		Tracks.Add(MakeShared<FJsonValueObject>(Feet));
		Edited->GetObjectField(TEXT("Body"))->SetArrayField(TEXT("NotifyTracks"), Tracks);
		const TSharedPtr<FJsonObject> Row = FidelityRow(Edited, TEXT("Notifies"), 0);
		Row->SetStringField(TEXT("Track"), TEXT("Feet"));
		Row->SetNumberField(TEXT("Time"), 0.5);
	}, TEXT("Edit")).IsSuccess()))
	{
		Composite = Target.Get<UAnimComposite>();
		if (TestNotNull(TEXT("composite"), Composite) && TestEqual(TEXT("one notify"), Composite->Notifies.Num(), 1))
		{
			TestEqual(TEXT("two notify tracks"), Composite->AnimNotifyTracks.Num(), 2);
			TestEqual(TEXT("notify name"), Composite->Notifies[0].NotifyName, FName(TEXT("Footstep")));
			TestEqual(TEXT("notify time"), Composite->Notifies[0].GetTime(), 0.5f, 1e-4f);
			TestEqual(TEXT("notify track"), Composite->Notifies[0].TrackIndex, 1);
			TestEqual(TEXT("curve kept"), Composite->GetDataModel()->GetFloatCurves().Num(), 1);
		}
	}

	// The sequence's rules: a notify after the composite's end is refused at the row.
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Edited)
	{
		FidelityRow(Edited, TEXT("Notifies"), 0)->SetNumberField(TEXT("Time"), 1000.0);
	}, TEXT("Notify out of range")), TEXT("AnimationProvider.Authoring.TimeOutOfRange"), TEXT("/Body/Notifies/0/Time"), TEXT("Notify out of range"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityBlendSpaceAnalysisTest,
	"UEAssetDoc.Fidelity.BlendSpace.Analysis", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelityBlendSpaceAnalysisTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentFidelityAnimationTestsLocal;
	const FTarget Target(TEXT("BS_Fidelity"));
	ON_SCOPE_EXIT { Target.Cleanup(); };

	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, FidelitySkeleton);
	if (!TestNotNull(TEXT("skeleton"), Skeleton))
	{
		return false;
	}
	UBlendSpace* BlendSpace = Target.Create<UBlendSpace>();
	BlendSpace->SetSkeleton(Skeleton);
	// The Details panel's function picker on the horizontal axis (Velocity), then its Axis row.
	ULinearAnalysisProperties* Velocity = NewObject<ULinearAnalysisProperties>(BlendSpace);
	Velocity->Function = TEXT("Velocity");
	Velocity->FunctionAxis = EAnalysisLinearAxis::PlusY;
	Velocity->SetFlags(RF_Transactional);
	BlendSpace->AnalysisProperties[0] = Velocity;
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Document = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Blend space"));
	if (!Document.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("analysis function"), FidelityStringAt(Document, TEXT("/Body/Axes/X/Analysis/Function")), FString(TEXT("Velocity")));
	const FString AxisForm = FidelityStringAt(Document, TEXT("/Body/Axes/X/Analysis/Properties/FunctionAxis"));
	TestTrue(TEXT("analysis axis setting"), AxisForm.Contains(TEXT("PlusY")));
	TestFalse(TEXT("no analysis on Y"), At(Document, TEXT("/Body/Axes/Y/Analysis")).IsValid());

	// A setting changes; the vertical axis gets a function through the picker.
	if (TestTrue(TEXT("analysis edit applies"), ApplyEdited(*this, Target, [&AxisForm](const TSharedPtr<FJsonObject>& Edited)
	{
		const TSharedPtr<FJsonObject> Axes = Edited->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("Axes"));
		Axes->GetObjectField(TEXT("X"))->GetObjectField(TEXT("Analysis"))->GetObjectField(TEXT("Properties"))
			->SetStringField(TEXT("FunctionAxis"), AxisForm.Replace(TEXT("PlusY"), TEXT("MinusZ")));
		const TSharedPtr<FJsonObject> Orientation = MakeShared<FJsonObject>();
		Orientation->SetStringField(TEXT("Function"), TEXT("Orientation"));
		Axes->GetObjectField(TEXT("Y"))->SetObjectField(TEXT("Analysis"), Orientation);
	}, TEXT("Edit")).IsSuccess()))
	{
		BlendSpace = Target.Get<UBlendSpace>();
		const ULinearAnalysisProperties* X = BlendSpace != nullptr ? Cast<ULinearAnalysisProperties>(BlendSpace->AnalysisProperties[0]) : nullptr;
		const UEulerAnalysisProperties* Y = BlendSpace != nullptr ? Cast<UEulerAnalysisProperties>(BlendSpace->AnalysisProperties[1]) : nullptr;
		if (TestNotNull(TEXT("X keeps its settings object"), X))
		{
			TestEqual(TEXT("X function"), X->Function, FString(TEXT("Velocity")));
			TestTrue(TEXT("X axis setting"), X->FunctionAxis == EAnalysisLinearAxis::MinusZ);
		}
		if (TestNotNull(TEXT("Y has an Euler settings object"), Y))
		{
			TestEqual(TEXT("Y function"), Y->Function, FString(TEXT("Orientation")));
		}
	}

	// null removes the function, as picking None does.
	if (TestTrue(TEXT("analysis removal applies"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Edited)
	{
		Edited->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("Axes"))->GetObjectField(TEXT("X"))
			->SetField(TEXT("Analysis"), MakeShared<FJsonValueNull>());
	}, TEXT("Remove")).IsSuccess()))
	{
		BlendSpace = Target.Get<UBlendSpace>();
		TestTrue(TEXT("X has no analysis"), BlendSpace != nullptr && BlendSpace->AnalysisProperties[0] == nullptr);
	}

	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Edited)
	{
		const TSharedPtr<FJsonObject> Unknown = MakeShared<FJsonObject>();
		Unknown->SetStringField(TEXT("Function"), TEXT("NoSuchFunction"));
		Edited->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("Axes"))->GetObjectField(TEXT("X"))->SetObjectField(TEXT("Analysis"), Unknown);
	}, TEXT("Unknown function")), TEXT("BlendSpace.InvalidAnalysis"), TEXT("/Body/Axes/X/Analysis/Function"), TEXT("Unknown function"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityAnimMontageAssetUserDataTest,
	"UEAssetDoc.Fidelity.AnimMontage.AssetUserData", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelityAnimMontageAssetUserDataTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentFidelityAnimationTestsLocal;
	const FTarget SkeletonTarget(TEXT("SK_FidelityMontage"));
	const FTarget Target(TEXT("AM_FidelityUserData"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		SkeletonTarget.Cleanup();
	};
	// A project user data class of the test module, found by path.
	const TCHAR* UserDataPath = TEXT("/Script/AssetDocumentTests.AssetDocumentSequenceLifecycleUserDataTest");
	UClass* UserDataClass = FindObject<UClass>(nullptr, UserDataPath);
	UAnimMontage* Montage = FidelityMontage(*this, SkeletonTarget, Target);
	if (!TestNotNull(TEXT("user data class"), UserDataClass) || Montage == nullptr)
	{
		return false;
	}
	// The Asset Details panel's Asset User Data list: one new entry of the class, its First row set to 9.
	UAssetUserData* Data = NewObject<UAssetUserData>(Montage, UserDataClass, NAME_None, RF_Transactional);
	FIntProperty* First = FindFProperty<FIntProperty>(UserDataClass, TEXT("First"));
	if (!TestNotNull(TEXT("First row"), First))
	{
		return false;
	}
	First->SetPropertyValue_InContainer(Data, 9);
	Montage->AddAssetUserData(Data);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Document = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Montage"));
	if (!Document.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("user data class listed"), FidelityStringAt(Document, TEXT("/Properties/AssetUserData/0/Class")), FString(UserDataPath));
	TestEqual(TEXT("user data value listed"), FidelityNumberAt(Document, TEXT("/Properties/AssetUserData/0/Properties/First")), 9.0);

	if (TestTrue(TEXT("user data edit applies"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Edited)
	{
		const TSharedPtr<FJsonValue> Row = At(Edited, TEXT("/Properties/AssetUserData/0/Properties"));
		if (Row.IsValid() && Row->Type == EJson::Object)
		{
			Row->AsObject()->SetNumberField(TEXT("First"), 11);
		}
	}, TEXT("Edit")).IsSuccess()))
	{
		Montage = Target.Get<UAnimMontage>();
		const TArray<UAssetUserData*>* List = Montage != nullptr ? Montage->GetAssetUserDataArray() : nullptr;
		if (TestTrue(TEXT("one user data"), List != nullptr && List->Num() == 1 && (*List)[0] != nullptr))
		{
			TestEqual(TEXT("its class"), (*List)[0]->GetClass(), UserDataClass);
			TestEqual(TEXT("its value"), First->GetPropertyValue_InContainer((*List)[0]), 11);
			TestTrue(TEXT("owned by the montage"), (*List)[0]->IsIn(Montage));
		}
	}

	if (TestTrue(TEXT("user data removal applies"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Edited)
	{
		Edited->GetObjectField(TEXT("Properties"))->SetArrayField(TEXT("AssetUserData"), TArray<TSharedPtr<FJsonValue>>());
	}, TEXT("Remove")).IsSuccess()))
	{
		Montage = Target.Get<UAnimMontage>();
		const TArray<UAssetUserData*>* List = Montage != nullptr ? Montage->GetAssetUserDataArray() : nullptr;
		TestTrue(TEXT("no user data"), List == nullptr || List->IsEmpty());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityAnimMontageDuplicateSlotTest,
	"UEAssetDoc.Tolerant.AnimMontage.DuplicateSlot", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelityAnimMontageDuplicateSlotTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentFidelityAnimationTestsLocal;
	const FTarget SkeletonTarget(TEXT("SK_FidelityDuplicate"));
	const FTarget Target(TEXT("AM_FidelityDuplicate"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		SkeletonTarget.Cleanup();
	};
	UAnimMontage* Montage = FidelityMontage(*this, SkeletonTarget, Target);
	if (Montage == nullptr)
	{
		return false;
	}
	// SAnimMontagePanel::DuplicateMontageSlot: a new DefaultSlot track with a copy of the first track's segments.
	FSlotAnimationTrack& Duplicate = Montage->AddSlot(FAnimSlotGroup::DefaultSlotName);
	Duplicate.AnimTrack = Montage->SlotAnimTracks[0].AnimTrack;
	if (!TestEqual(TEXT("two tracks"), Montage->SlotAnimTracks.Num(), 2) || !TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	FExpected Expected;
	Expected.Warnings = {{TEXT("AnimMontage.DuplicateSlot"), TEXT("/Body/SlotAnimTracks/1/SlotName")}};
	const TSharedPtr<FJsonObject> Document = TestTolerantRoundTrip(*this, Target, Expected, TEXT("Montage"));
	if (!Document.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("second track reads its slot"), FidelityStringAt(Document, TEXT("/Body/SlotAnimTracks/1/SlotName")),
		FAnimSlotGroup::DefaultSlotName.ToString());

	// Another edit keeps both tracks.
	if (TestTrue(TEXT("rate scale edit applies"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Edited)
	{
		Edited->GetObjectField(TEXT("Properties"))->SetNumberField(TEXT("RateScale"), 1.5);
	}, TEXT("Edit")).IsSuccess()))
	{
		Montage = Target.Get<UAnimMontage>();
		if (TestNotNull(TEXT("montage"), Montage) && TestEqual(TEXT("tracks kept"), Montage->SlotAnimTracks.Num(), 2))
		{
			TestEqual(TEXT("rate scale"), Montage->RateScale, 1.5f);
			TestEqual(TEXT("second track's slot"), Montage->SlotAnimTracks[1].SlotName, FAnimSlotGroup::DefaultSlotName);
			TestEqual(TEXT("second track's segments"), Montage->SlotAnimTracks[1].AnimTrack.AnimSegments.Num(), 1);
		}
		TestStillShows(*this, Target, TEXT("AnimMontage.DuplicateSlot"), TEXT("/Body/SlotAnimTracks/1/SlotName"), TEXT("After edit"));
	}

	// A slot track edit cannot carry two tracks on one slot.
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Edited)
	{
		const TSharedPtr<FJsonValue> Segment = At(Edited, TEXT("/Body/SlotAnimTracks/0/Segments/0"));
		if (Segment.IsValid() && Segment->Type == EJson::Object)
		{
			Segment->AsObject()->SetNumberField(TEXT("AnimPlayRate"), 2.0);
		}
	}, TEXT("Changed tracks")), TEXT("AnimMontage.DuplicateSlot"), TEXT("/Body/SlotAnimTracks/1/SlotName"), TEXT("Changed tracks"));
	return !HasAnyErrors();
}

#endif
