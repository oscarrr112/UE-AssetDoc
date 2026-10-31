// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Editor-made animation assets that break rules written for new input: a sequence with a duplicated notify track
// name and a hidden RefPoseSeq; a composite segment whose animation was
// deleted and one with loop count 0; a Pose Asset with no source animation; a blend space sample whose animation
// was deleted and one outside a narrowed axis; a Skeleton without DefaultGroup and with an imported curve name
// ending in a space; a Montage whose preview mesh and one segment's animation were deleted, with a track on a slot
// its Skeleton does not list and a section linked to a removed track. Each reads with warnings, its own extract
// re-applies as a no-op, other edits work, and the rules still refuse new input. A sequence notify whose Details hold
// a value the document has no form for (a project notify of the AssetDocumentTests module, found by path) reads as a
// marker and keeps its object and value when a row is inserted before it.

#include "AssetDocumentTolerantTestSupport.h"

#include "Animation/AnimComposite.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/PoseAsset.h"
#include "Animation/Skeleton.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentTolerantAnimationTestsLocal
{
using namespace AssetDocumentTolerantTestSupport;

const TCHAR* EngineSkeleton = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
const TCHAR* IdleAnim = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");
const TCHAR* WalkAnim = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Walk_Fwd.Tutorial_Walk_Fwd");

// The Content Browser's Duplicate of an engine asset into the target's package (the engine asset is never edited).
template <typename T>
T* CopyOf(const FTarget& Target, const TCHAR* SourcePath)
{
	T* Source = LoadObject<T>(nullptr, SourcePath);
	if (Source == nullptr)
	{
		return nullptr;
	}
	T* Copy = DuplicateObject<T>(Source, CreatePackage(*Target.PackageName), *Target.AssetName);
	Copy->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Copy);
	Copy->MarkPackageDirty();
	return Copy;
}

TSharedPtr<FJsonObject> Body(const TSharedPtr<FJsonObject>& Document)
{
	return Document->GetObjectField(TEXT("Body"));
}

TSharedPtr<FJsonObject> RowAt(const TSharedPtr<FJsonObject>& Document, const TCHAR* Region, int32 Index)
{
	return Body(Document)->GetArrayField(Region)[Index]->AsObject();
}

// Appends Row to Body.<Region>; returns its index.
int32 Append(const TSharedPtr<FJsonObject>& Document, const TCHAR* Region, const TSharedPtr<FJsonObject>& Row)
{
	TArray<TSharedPtr<FJsonValue>> Rows = Body(Document)->GetArrayField(Region);
	Rows.Add(MakeShared<FJsonValueObject>(Row));
	Body(Document)->SetArrayField(Region, Rows);
	return Rows.Num() - 1;
}

void AddNotify(UAnimSequence& Sequence, const TCHAR* Name, float Time, int32 Track)
{
	FAnimNotifyEvent& Event = Sequence.Notifies.AddDefaulted_GetRef();
	Event.NotifyName = Name;
	Event.Link(&Sequence, Time);
	Event.TriggerTimeOffset = GetTriggerTimeOffsetForType(Sequence.CalculateOffsetForNotify(Time));
	Event.TrackIndex = Track;
#if WITH_EDITORONLY_DATA
	Event.Guid = FGuid::NewGuid();
#endif
}

TArray<FBlendSample>& SampleData(UBlendSpace& BlendSpace)
{
	FArrayProperty* Property = FindFProperty<FArrayProperty>(UBlendSpace::StaticClass(), TEXT("SampleData"));
	return *Property->ContainerPtrToValuePtr<TArray<FBlendSample>>(&BlendSpace);
}

FBlendParameter& Axis(UBlendSpace& BlendSpace, int32 Index)
{
	FStructProperty* Property = FindFProperty<FStructProperty>(UBlendSpace::StaticClass(), TEXT("BlendParameters"));
	return *Property->ContainerPtrToValuePtr<FBlendParameter>(&BlendSpace, Index);
}

// The stored PreviewSkeletalMesh soft path, as the asset saves it (the editor's picker writes this property).
FSoftObjectPtr& PreviewMeshPath(UAnimationAsset& Asset)
{
	FSoftObjectProperty* Property = FindFProperty<FSoftObjectProperty>(UAnimationAsset::StaticClass(), TEXT("PreviewSkeletalMesh"));
	return *Property->ContainerPtrToValuePtr<FSoftObjectPtr>(&Asset);
}

FAnimSegment Segment(UAnimSequenceBase* Animation, float StartPos)
{
	FAnimSegment Out;
	if (Animation != nullptr)
	{
		Out.SetAnimReference(Animation, true);
	}
	Out.AnimStartTime = 0.f;
	Out.AnimEndTime = 0.5f;
	Out.AnimPlayRate = 1.f;
	Out.LoopingCount = 1;
	Out.StartPos = StartPos;
	return Out;
}

void AddSection(UAnimMontage& Montage, const TCHAR* Name, float Time, int32 Slot)
{
	FCompositeSection& Section = Montage.CompositeSections.AddDefaulted_GetRef();
	Section.SectionName = Name;
	Section.Link(&Montage, Time, Slot);
}

const TCHAR* const FieldNotifyClass = TEXT("/Script/AssetDocumentTests.AssetDocumentSequenceFieldNotify");

// The sequence's notify of the field notify class (nullptr when there is none).
const FAnimNotifyEvent* FieldNotifyEvent(const UAnimSequence& Sequence, const UClass& NotifyClass)
{
	return Sequence.Notifies.FindByPredicate([&NotifyClass](const FAnimNotifyEvent& Event)
	{
		return Event.Notify != nullptr && Event.Notify->GetClass() == &NotifyClass;
	});
}

FString PropertyText(const UObject& Object, const TCHAR* Name)
{
	FString Text;
	if (const FProperty* Property = Object.GetClass()->FindPropertyByName(Name))
	{
		Property->ExportText_InContainer(0, Text, &Object, nullptr, nullptr, PPF_None);
	}
	return Text;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantAnimSequenceTest, "UEAssetDoc.Tolerant.AnimSequence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantAnimSequenceTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantAnimationTestsLocal;
	const FTarget Target(TEXT("AS_Tolerant"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UAnimSequence* Sequence = CopyOf<UAnimSequence>(Target, IdleAnim);
	if (!TestNotNull(TEXT("sequence copy"), Sequence))
	{
		return false;
	}
	// A track renamed to a used name in the Notifies panel, two notifies snapped onto one frame of a track, and a
	// RefPoseSeq left from an older engine on a sequence that is not additive. The engine moves one of the snapped
	// notifies to another track (UAnimSequenceBase::RefreshCacheData, also run on load), so they read on two tracks.
	Sequence->Modify();
	Sequence->AnimNotifyTracks.Reset();
	Sequence->AnimNotifyTracks.Add(FAnimNotifyTrack(TEXT("1"), FLinearColor::White));
	Sequence->AnimNotifyTracks.Add(FAnimNotifyTrack(TEXT("FX"), FLinearColor::White));
	Sequence->AnimNotifyTracks.Add(FAnimNotifyTrack(TEXT("FX"), FLinearColor::White));
	Sequence->Notifies.Reset();
	AddNotify(*Sequence, TEXT("StepLeft"), 0.1f, 0);
	AddNotify(*Sequence, TEXT("StepRight"), 0.1f, 0);
	AddNotify(*Sequence, TEXT("Dust"), 0.2f, 2);
	Sequence->RefreshCacheData();
	Sequence->AdditiveAnimType = AAT_None;
	Sequence->RefPoseSeq = LoadObject<UAnimSequence>(nullptr, WalkAnim);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	FExpected Expected;
	Expected.Warnings = {
		{TEXT("AnimSequence.DuplicateNotifyTrack"), TEXT("/Body/NotifyTracks/2")},
		{TEXT("AnimSequence.RefPoseSeqHidden"), TEXT("/Body/Additive/RefPoseSeq")}};
	if (!TestTolerantRoundTrip(*this, Target, Expected, TEXT("Sequence")).IsValid())
	{
		return false;
	}

	if (TestTrue(TEXT("another notify moves"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		RowAt(Document, TEXT("Notifies"), 2)->SetNumberField(TEXT("Time"), 0.4);
	}, TEXT("Edit")).IsSuccess()))
	{
		Sequence = Target.Get<UAnimSequence>();
		if (TestNotNull(TEXT("sequence"), Sequence))
		{
			TestEqual(TEXT("notifies kept"), Sequence->Notifies.Num(), 3);
			TestTrue(TEXT("the moved notify"), Sequence->Notifies.ContainsByPredicate([](const FAnimNotifyEvent& Event)
			{
				return Event.NotifyName == TEXT("Dust") && FMath::IsNearlyEqual(Event.GetTime(), 0.4f, 0.02f) && Event.TrackIndex == 2;
			}));
			TestEqual(TEXT("tracks kept"), Sequence->AnimNotifyTracks.Num(), 3);
			TestNotNull(TEXT("RefPoseSeq kept"), Sequence->RefPoseSeq.Get());
		}
		TestStillShows(*this, Target, TEXT("AnimSequence.DuplicateNotifyTrack"), TEXT("/Body/NotifyTracks/2"), TEXT("After edit"));
	}

	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		Append(Document, TEXT("Notifies"), Parse(Serialize(RowAt(Document, TEXT("Notifies"), 0))));
	}, TEXT("New overlap")), TEXT("AnimSequence.OverlappingNotifies"), TEXT("/Body/Notifies/3/Time"), TEXT("New overlap"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimSequenceNotifyIdentityTest, "UEAssetDoc.Codec.AnimSequence.NotifyIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimSequenceNotifyIdentityTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantAnimationTestsLocal;
	const FTarget Target(TEXT("AS_NotifyIdentity"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UClass* NotifyClass = FindObject<UClass>(nullptr, FieldNotifyClass);
	if (!TestNotNull(TEXT("the test notify class is loaded"), NotifyClass))
	{
		return false;
	}
	UAnimSequence* Sequence = CopyOf<UAnimSequence>(Target, IdleAnim);
	if (!TestNotNull(TEXT("sequence copy"), Sequence))
	{
		return false;
	}
	// The Notifies panel's Add Notify of the project notify on track 1, its Field picked in the notify's Details.
	Sequence->Modify();
	Sequence->AnimNotifyTracks.Reset();
	Sequence->AnimNotifyTracks.Add(FAnimNotifyTrack(TEXT("1"), FLinearColor::White));
	Sequence->Notifies.Reset();
	UAnimNotify* Notify = NewObject<UAnimNotify>(Sequence, NotifyClass, NAME_None, RF_Transactional);
	const FProperty* FieldProperty = NotifyClass->FindPropertyByName(TEXT("Field"));
	if (!TestNotNull(TEXT("Field property"), FieldProperty)
		|| !TestNotNull(TEXT("field set"), FieldProperty->ImportText_InContainer(TEXT("/Script/Engine.Actor:Tags"), Notify, Notify, PPF_None)))
	{
		return false;
	}
	FAnimNotifyEvent& Event = Sequence->Notifies.AddDefaulted_GetRef();
	Event.Notify = Notify;
	Event.NotifyName = FName(*Notify->GetNotifyName());
	Event.Link(Sequence, 0.5f);
	Event.TriggerTimeOffset = GetTriggerTimeOffsetForType(Sequence->CalculateOffsetForNotify(0.5f));
	Event.TrackIndex = 0;
#if WITH_EDITORONLY_DATA
	Event.Guid = FGuid::NewGuid();
	const FGuid NotifyGuid = Event.Guid;
#endif
	// A later notify of the same class saved without a Guid (zero), as an older asset holds it.
	UAnimNotify* ZeroGuidNotify = NewObject<UAnimNotify>(Sequence, NotifyClass, NAME_None, RF_Transactional);
	{
		FAnimNotifyEvent& ZeroGuidEvent = Sequence->Notifies.AddDefaulted_GetRef();
		ZeroGuidEvent.Notify = ZeroGuidNotify;
		ZeroGuidEvent.NotifyName = FName(*ZeroGuidNotify->GetNotifyName());
		ZeroGuidEvent.Link(Sequence, 0.8f);
		ZeroGuidEvent.TriggerTimeOffset = GetTriggerTimeOffsetForType(Sequence->CalculateOffsetForNotify(0.8f));
		ZeroGuidEvent.TrackIndex = 0;
	}
	Sequence->RefreshCacheData();
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}
	const TWeakObjectPtr<UAnimNotify> Original(Notify);
	const TWeakObjectPtr<UAnimNotify> ZeroGuidOriginal(ZeroGuidNotify);

	const FString FieldMarker = TEXT("/Body/Notifies/0/Properties/Field");
	FExpected Expected;
	Expected.Warnings = {{TEXT("AnimSequence.Unrepresented"), FieldMarker}};
	Expected.Markers = {FieldMarker};
	if (!TestTolerantRoundTrip(*this, Target, Expected, TEXT("Sequence")).IsValid())
	{
		return false;
	}

	// A named notify inserted before it (first row, earlier time), and its Bias changed: it keeps its object, Guid and
	// Field; the notify without a Guid keeps its object too.
	if (TestTrue(TEXT("a notify is inserted before it"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		TArray<TSharedPtr<FJsonValue>> Rows = Body(Document)->GetArrayField(TEXT("Notifies"));
		Rows[0]->AsObject()->GetObjectField(TEXT("Properties"))->SetNumberField(TEXT("Bias"), 2);
		const TSharedPtr<FJsonObject> Inserted = MakeShared<FJsonObject>();
		Inserted->SetNumberField(TEXT("Time"), 0.1);
		Inserted->SetStringField(TEXT("Name"), TEXT("Step"));
		Rows.Insert(MakeShared<FJsonValueObject>(Inserted), 0);
		Body(Document)->SetArrayField(TEXT("Notifies"), Rows);
	}, TEXT("Insert")).IsSuccess()))
	{
		Sequence = Target.Get<UAnimSequence>();
		const FAnimNotifyEvent* Kept = Sequence != nullptr ? FieldNotifyEvent(*Sequence, *NotifyClass) : nullptr;
		if (TestNotNull(TEXT("the field notify"), Kept) && TestNotNull(TEXT("its object"), Kept->Notify.Get()))
		{
			TestEqual(TEXT("all notifies"), Sequence->Notifies.Num(), 3);
			TestTrue(TEXT("the same notify object"), Original.IsValid() && Kept->Notify.Get() == Original.Get());
#if WITH_EDITORONLY_DATA
			TestEqual(TEXT("its Guid kept"), Kept->Guid, NotifyGuid);
#endif
			TestTrue(TEXT("its Field kept"), PropertyText(*Kept->Notify, TEXT("Field")).Contains(TEXT("Tags")));
			const FFloatProperty* BiasProperty = FindFProperty<FFloatProperty>(NotifyClass, TEXT("Bias"));
			TestTrue(TEXT("its Bias written"), BiasProperty != nullptr && BiasProperty->GetPropertyValue_InContainer(Kept->Notify.Get()) == 2.f);
		}
		const FAnimNotifyEvent* KeptZeroGuid = Sequence != nullptr && ZeroGuidOriginal.IsValid()
			? Sequence->Notifies.FindByPredicate([&ZeroGuidOriginal](const FAnimNotifyEvent& Candidate)
			{
				return Candidate.Notify.Get() == ZeroGuidOriginal.Get();
			})
			: nullptr;
		TestTrue(TEXT("the notify without a Guid keeps its object"),
			KeptZeroGuid != nullptr && FMath::IsNearlyEqual(KeptZeroGuid->GetTime(), 0.8f, 0.02f));
		TestStillShows(*this, Target, TEXT("AnimSequence.Unrepresented"), TEXT("/Body/Notifies/1/Properties/Field"), TEXT("After insert"));
	}
	else
	{
		// The refusals below edit the inserted layout.
		return false;
	}

	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		RowAt(Document, TEXT("Notifies"), 1)->GetObjectField(TEXT("Properties"))->GetObjectField(TEXT("Field"))
			->SetStringField(TEXT("Text"), TEXT("/Script/Engine.Actor:Layer"));
	}, TEXT("Edited marker")), TEXT("AnimSequence.UnrepresentedValue"), TEXT("/Body/Notifies/1/Properties/Field"), TEXT("Edited marker"));
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Row = Parse(Serialize(RowAt(Document, TEXT("Notifies"), 1)));
		Row->SetNumberField(TEXT("Time"), 0.9);
		Append(Document, TEXT("Notifies"), Row);
	}, TEXT("New marked notify")), TEXT("AnimSequence.UnrepresentedValue"), TEXT("/Body/Notifies/3/Properties/Field"), TEXT("New marked notify"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantAnimCompositeTest, "UEAssetDoc.Tolerant.AnimComposite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantAnimCompositeTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantAnimationTestsLocal;
	const FTarget Target(TEXT("AC_Tolerant"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, EngineSkeleton);
	UAnimSequence* Idle = LoadObject<UAnimSequence>(nullptr, IdleAnim);
	if (!TestNotNull(TEXT("skeleton"), Skeleton) || !TestNotNull(TEXT("idle"), Idle))
	{
		return false;
	}
	UAnimComposite* Composite = Target.Create<UAnimComposite>();
	Composite->SetSkeleton(Skeleton);
	// The first segment's animation was deleted (the segment stays with no animation); the second has a loop
	// count of 0, which the segment Details row accepts.
	FAnimSegment Missing;
	Missing.AnimStartTime = 0.f;
	Missing.AnimEndTime = 0.5f;
	Missing.AnimPlayRate = 1.f;
	Missing.LoopingCount = 1;
	Missing.StartPos = 0.f;
	Composite->AnimationTrack.AnimSegments.Add(Missing);
	FAnimSegment Silent;
	Silent.SetAnimReference(Idle, true);
	Silent.LoopingCount = 0;
	Silent.StartPos = 0.5f;
	Composite->AnimationTrack.AnimSegments.Add(Silent);
	Composite->SetCompositeLength(Composite->AnimationTrack.GetLength());
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	FExpected Expected;
	Expected.Warnings = {
		{TEXT("AnimComposite.MissingReference"), TEXT("/Body/Segments/0/Animation")},
		{TEXT("AnimComposite.InvalidSegment"), TEXT("/Body/Segments/1/LoopCount")}};
	if (!TestTolerantRoundTrip(*this, Target, Expected, TEXT("Composite")).IsValid())
	{
		return false;
	}

	if (TestTrue(TEXT("a segment's play rate changes"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		RowAt(Document, TEXT("Segments"), 1)->SetNumberField(TEXT("PlayRate"), 2);
	}, TEXT("Edit")).IsSuccess()))
	{
		Composite = Target.Get<UAnimComposite>();
		const TArray<FAnimSegment>* Segments = Composite != nullptr ? &Composite->AnimationTrack.AnimSegments : nullptr;
		if (TestTrue(TEXT("both segments kept"), Segments != nullptr && Segments->Num() == 2))
		{
			TestNull(TEXT("the first still has no animation"), (*Segments)[0].GetAnimReference().Get());
			TestEqual(TEXT("its range kept"), (*Segments)[0].AnimEndTime, 0.5f);
			TestEqual(TEXT("the play rate"), (*Segments)[1].AnimPlayRate, 2.f);
			TestEqual(TEXT("the loop count kept"), (*Segments)[1].LoopingCount, 0);
		}
		TestStillShows(*this, Target, TEXT("AnimComposite.MissingReference"), TEXT("/Body/Segments/0/Animation"), TEXT("After edit"));
	}

	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetField(TEXT("Animation"), MakeShared<FJsonValueNull>());
		Row->SetNumberField(TEXT("EndTime"), 0.25);
		Append(Document, TEXT("Segments"), Row);
	}, TEXT("New missing animation")), TEXT("AnimComposite.MissingReference"), TEXT("/Body/Segments/2/Animation"), TEXT("New missing animation"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantPoseAssetTest, "UEAssetDoc.Tolerant.PoseAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantPoseAssetTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantAnimationTestsLocal;
	const FTarget Target(TEXT("PA_Tolerant"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, EngineSkeleton);
	if (!TestNotNull(TEXT("skeleton"), Skeleton))
	{
		return false;
	}
	// A Pose Asset made from the viewport pose has no source animation.
	UPoseAsset* PoseAsset = Target.Create<UPoseAsset>();
	PoseAsset->SetSkeleton(Skeleton);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Pose Asset"));
	if (Extracted.IsValid())
	{
		FString Source;
		TestTrue(TEXT("SourceAnimation reads as empty"), Body(Extracted)->TryGetStringField(TEXT("SourceAnimation"), Source) && Source.IsEmpty());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantBlendSpaceTest, "UEAssetDoc.Tolerant.BlendSpace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantBlendSpaceTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantAnimationTestsLocal;
	const FTarget Target(TEXT("BS_Tolerant"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	// The engine reports the editor-made anomalies when it loads or checks the samples (as the editor's message log does).
	AddExpectedMessage(TEXT("has a sample with no/invalid animation"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, -1, false);
	AddExpectedMessage(TEXT("invalid due to being out of bounds"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, EngineSkeleton);
	UAnimSequence* Idle = LoadObject<UAnimSequence>(nullptr, IdleAnim);
	UAnimSequence* Walk = LoadObject<UAnimSequence>(nullptr, WalkAnim);
	if (!TestNotNull(TEXT("skeleton"), Skeleton) || !TestNotNull(TEXT("idle"), Idle) || !TestNotNull(TEXT("walk"), Walk))
	{
		return false;
	}
	UBlendSpace1D* BlendSpace = Target.Create<UBlendSpace1D>();
	BlendSpace->SetSkeleton(Skeleton);
	BlendSpace->AddSample(Idle, FVector(0, 0, 0));
	BlendSpace->AddSample(Idle, FVector(25, 0, 0));
	BlendSpace->AddSample(Walk, FVector(100, 0, 0));
	// The animation of the middle sample was deleted, and the axis was then narrowed: the editor keeps both
	// samples (shown red).
	SampleData(*BlendSpace)[1].Animation = nullptr;
	Axis(*BlendSpace, 0).Max = 90.f;
	BlendSpace->ValidateSampleData();
	if (!TestEqual(TEXT("three samples"), BlendSpace->GetNumberOfBlendSamples(), 3) || !TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	FExpected Expected;
	Expected.Warnings = {{TEXT("BlendSpace.MissingReference"), TEXT("/Body/Samples/1/Animation")}};
	if (!TestTolerantRoundTrip(*this, Target, Expected, TEXT("Blend space")).IsValid())
	{
		return false;
	}

	if (TestTrue(TEXT("a sample's rate scale changes"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		RowAt(Document, TEXT("Samples"), 0)->SetNumberField(TEXT("RateScale"), 1.5);
	}, TEXT("Edit")).IsSuccess()))
	{
		BlendSpace = Target.Get<UBlendSpace1D>();
		if (TestTrue(TEXT("samples kept"), BlendSpace != nullptr && BlendSpace->GetNumberOfBlendSamples() == 3))
		{
			TestEqual(TEXT("rate scale"), BlendSpace->GetBlendSample(0).RateScale, 1.5f);
			TestNull(TEXT("the middle sample still has no animation"), BlendSpace->GetBlendSample(1).Animation.Get());
			TestEqual(TEXT("the outside sample kept"), BlendSpace->GetBlendSample(2).SampleValue.X, 100.0);
		}
		TestStillShows(*this, Target, TEXT("BlendSpace.MissingReference"), TEXT("/Body/Samples/1/Animation"), TEXT("After edit"));
	}

	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Row = Parse(Serialize(RowAt(Document, TEXT("Samples"), 2)));
		Row->SetNumberField(TEXT("X"), 95);
		Append(Document, TEXT("Samples"), Row);
	}, TEXT("New outside sample")), TEXT("BlendSpace.SampleOutOfRange"), TEXT("/Body/Samples/3/X"), TEXT("New outside sample"));
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Row = Parse(Serialize(RowAt(Document, TEXT("Samples"), 0)));
		Row->SetField(TEXT("Animation"), MakeShared<FJsonValueNull>());
		Row->SetNumberField(TEXT("X"), 50);
		Append(Document, TEXT("Samples"), Row);
	}, TEXT("New missing animation")), TEXT("BlendSpace.MissingReference"), TEXT("/Body/Samples/3/Animation"), TEXT("New missing animation"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantSkeletonTest, "UEAssetDoc.Tolerant.Skeleton",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantSkeletonTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantAnimationTestsLocal;
	const FTarget Target(TEXT("SK_Tolerant"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	USkeleton* Skeleton = CopyOf<USkeleton>(Target, EngineSkeleton);
	if (!TestNotNull(TEXT("skeleton copy"), Skeleton))
	{
		return false;
	}
	// Slots made only in a new group (no DefaultGroup), and a curve imported with a trailing space.
	Skeleton->Modify();
	if (Skeleton->FindAnimSlotGroup(FAnimSlotGroup::DefaultGroupName) != nullptr)
	{
		Skeleton->RemoveSlotGroup(FAnimSlotGroup::DefaultGroupName);
	}
	Skeleton->AddSlotGroupName(TEXT("Face"));
	Skeleton->SetSlotGroupName(TEXT("Mouth"), TEXT("Face"));
	Skeleton->AddCurveMetaData(TEXT("Blink "), false);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	FExpected Expected;
	Expected.Warnings = {
		{TEXT("Skeleton.DefaultGroupRequired"), TEXT("/Body/SlotGroups")},
		{TEXT("Skeleton.InvalidName"), FString()}};
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, Expected, TEXT("Skeleton"));
	if (!Extracted.IsValid())
	{
		return false;
	}

	if (TestTrue(TEXT("a slot is added"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		for (const TSharedPtr<FJsonValue>& Group : Body(Document)->GetArrayField(TEXT("SlotGroups")))
		{
			if (Group->AsObject()->GetStringField(TEXT("Name")) == TEXT("Face"))
			{
				TArray<TSharedPtr<FJsonValue>> Slots = Group->AsObject()->GetArrayField(TEXT("Slots"));
				Slots.Add(MakeShared<FJsonValueString>(TEXT("Eyes")));
				Group->AsObject()->SetArrayField(TEXT("Slots"), Slots);
			}
		}
	}, TEXT("Edit")).IsSuccess()))
	{
		Skeleton = Target.Get<USkeleton>();
		if (TestNotNull(TEXT("skeleton"), Skeleton))
		{
			TestEqual(TEXT("the new slot"), Skeleton->GetSlotGroupName(TEXT("Eyes")), FName(TEXT("Face")));
			TestNull(TEXT("still no DefaultGroup"), Skeleton->FindAnimSlotGroup(FAnimSlotGroup::DefaultGroupName));
			TestNotNull(TEXT("the imported curve kept"), Skeleton->GetCurveMetaData(TEXT("Blink ")));
		}
		TestStillShows(*this, Target, TEXT("Skeleton.DefaultGroupRequired"), TEXT("/Body/SlotGroups"), TEXT("After edit"));
	}

	int32 NewCurve = INDEX_NONE;
	const FAssetDocumentResult Refused = ApplyEdited(*this, Target, [&NewCurve](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("Name"), TEXT("Smile "));
		NewCurve = Append(Document, TEXT("Curves"), Row);
	}, TEXT("New spaced curve"));
	TestRefused(*this, Refused, TEXT("Skeleton.InvalidName"), FString::Printf(TEXT("/Body/Curves/%d/Name"), NewCurve), TEXT("New spaced curve"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantAnimMontageTest, "UEAssetDoc.Tolerant.AnimMontage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantAnimMontageTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantAnimationTestsLocal;
	const FTarget SkeletonTarget(TEXT("SK_TolerantMontage"));
	const FTarget Target(TEXT("AM_Tolerant"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	// Loading a deleted preview mesh logs that its package is missing.
	AddExpectedMessage(TEXT("SK_TolerantMissing"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		SkeletonTarget.Cleanup();
	};

	const FName RemovedSlot(TEXT("TolerantRemovedSlot"));
	USkeleton* Skeleton = CopyOf<USkeleton>(SkeletonTarget, EngineSkeleton);
	USkeleton* EngineSkeletonAsset = LoadObject<USkeleton>(nullptr, EngineSkeleton);
	UAnimSequence* Idle = LoadObject<UAnimSequence>(nullptr, IdleAnim);
	if (!TestNotNull(TEXT("skeleton copy"), Skeleton) || !TestNotNull(TEXT("engine skeleton"), EngineSkeletonAsset)
		|| !TestNotNull(TEXT("idle"), Idle))
	{
		return false;
	}
	// The copy plays the engine skeleton's animations (its Compatible Skeletons setting), has DefaultSlot, and does
	// not list the slot of the second track (removed in the Anim Slot Manager after the track was made).
	Skeleton->Modify();
	Skeleton->AddCompatibleSkeleton(EngineSkeletonAsset);
	Skeleton->RegisterSlotNode(FAnimSlotGroup::DefaultSlotName);
	if (Skeleton->ContainsSlotName(RemovedSlot))
	{
		Skeleton->RemoveSlotName(RemovedSlot);
	}
	if (!TestTrue(TEXT("skeleton saved"), SkeletonTarget.Save()))
	{
		return false;
	}

	UAnimMontage* Montage = Target.Create<UAnimMontage>();
	Montage->SetSkeleton(Skeleton);
	Montage->SlotAnimTracks.Reset();
	Montage->CompositeSections.Reset();
	// DefaultSlot: Idle, then a segment whose animation was deleted; a track on the removed slot; a third track that
	// a section is linked to and that is then removed.
	FSlotAnimationTrack& First = Montage->SlotAnimTracks.AddDefaulted_GetRef();
	First.SlotName = FAnimSlotGroup::DefaultSlotName;
	First.AnimTrack.AnimSegments.Add(Segment(Idle, 0.f));
	First.AnimTrack.AnimSegments.Add(Segment(nullptr, First.AnimTrack.AnimSegments[0].GetEndPos()));
	FSlotAnimationTrack& Second = Montage->SlotAnimTracks.AddDefaulted_GetRef();
	Second.SlotName = RemovedSlot;
	Second.AnimTrack.AnimSegments.Add(Segment(Idle, 0.f));
	Montage->SlotAnimTracks.AddDefaulted_GetRef().SlotName = TEXT("TolerantRemovedTrack");
	IAnimationDataController& Controller = Montage->GetController();
	Controller.InitializeModel();
	Controller.SetFrameRate(FFrameRate(30, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(30), false);
	Controller.NotifyPopulated();
	AddSection(*Montage, TEXT("Default"), 0.f, 0);
	AddSection(*Montage, TEXT("Linked"), 0.25f, 2);
	AddSection(*Montage, TEXT("Second"), 0.5f, 0);
	Montage->SlotAnimTracks.RemoveAt(2);
	// The preview mesh picked in the Asset Details panel was deleted afterwards.
	const FString MissingName = FString::Printf(TEXT("SK_TolerantMissing_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString MissingMesh = FString::Printf(TEXT("/Game/%s/%s.%s"), FolderName, *MissingName, *MissingName);
	PreviewMeshPath(*Montage) = FSoftObjectPtr(FSoftObjectPath(MissingMesh));
	if (!TestEqual(TEXT("the section's track is gone"), Montage->CompositeSections[1].GetSlotIndex(), 2)
		|| !TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	FExpected Expected;
	Expected.Warnings = {
		{TEXT("AnimMontage.MissingReference"), TEXT("/Body/PreviewMesh")},
		{TEXT("AnimMontage.MissingReference"), TEXT("/Body/SlotAnimTracks/0/Segments/1/AnimReference")},
		{TEXT("AnimMontage.UnknownSlot"), TEXT("/Body/SlotAnimTracks/1/SlotName")},
		{TEXT("AnimMontage.UnknownSectionSlot"), TEXT("/Body/CompositeSections/1/SlotName")}};
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, Expected, TEXT("Montage"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("PreviewMesh reads as the stored path"), Body(Extracted)->GetStringField(TEXT("PreviewMesh")), MissingMesh);
	const TSharedPtr<FJsonValue> MissingAnimation = At(Extracted, TEXT("/Body/SlotAnimTracks/0/Segments/1/AnimReference"));
	TestTrue(TEXT("the segment reads AnimReference null"), MissingAnimation.IsValid() && MissingAnimation->Type == EJson::Null);

	if (TestTrue(TEXT("a section moves"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		RowAt(Document, TEXT("CompositeSections"), 2)->SetNumberField(TEXT("LinkableTime"), 0.75);
	}, TEXT("Edit")).IsSuccess()))
	{
		Montage = Target.Get<UAnimMontage>();
		if (TestNotNull(TEXT("montage"), Montage))
		{
			if (TestEqual(TEXT("tracks kept"), Montage->SlotAnimTracks.Num(), 2))
			{
				const TArray<FAnimSegment>& Segments = Montage->SlotAnimTracks[0].AnimTrack.AnimSegments;
				if (TestEqual(TEXT("segments kept"), Segments.Num(), 2))
				{
					TestNull(TEXT("the second segment still has no animation"), Segments[1].GetAnimReference().Get());
					TestEqual(TEXT("its range kept"), Segments[1].AnimEndTime, 0.5f);
				}
				TestEqual(TEXT("the track on the removed slot kept"), Montage->SlotAnimTracks[1].SlotName, RemovedSlot);
			}
			TestEqual(TEXT("the preview mesh path kept"), PreviewMeshPath(*Montage).ToSoftObjectPath().ToString(), MissingMesh);
			const FCompositeSection* Moved = Montage->CompositeSections.FindByPredicate([](const FCompositeSection& Section)
			{
				return Section.SectionName == TEXT("Second");
			});
			TestTrue(TEXT("the moved section"), Moved != nullptr && FMath::IsNearlyEqual(Moved->GetTime(), 0.75f, 0.001f));
		}
		TestStillShows(*this, Target, TEXT("AnimMontage.MissingReference"), TEXT("/Body/PreviewMesh"), TEXT("After edit"));
		TestStillShows(*this, Target, TEXT("AnimMontage.MissingReference"), TEXT("/Body/SlotAnimTracks/0/Segments/1/AnimReference"), TEXT("After edit"));
	}

	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Track = RowAt(Document, TEXT("SlotAnimTracks"), 0);
		TArray<TSharedPtr<FJsonValue>> Segments = Track->GetArrayField(TEXT("Segments"));
		const TSharedPtr<FJsonObject> Row = Parse(Serialize(Segments[1]->AsObject()));
		// Right after the segment it copies (both play 0.5 s of no animation at rate 1).
		Row->SetNumberField(TEXT("StartPos"), Row->GetNumberField(TEXT("StartPos")) + 0.5);
		Segments.Add(MakeShared<FJsonValueObject>(Row));
		Track->SetArrayField(TEXT("Segments"), Segments);
	}, TEXT("New missing animation")), TEXT("AnimMontage.MissingReference"), TEXT("/Body/SlotAnimTracks/0/Segments/2/AnimReference"),
		TEXT("New missing animation"));
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		Body(Document)->SetStringField(TEXT("PreviewMesh"), FString::Printf(TEXT("/Game/%s/SK_TolerantMissingNew.SK_TolerantMissingNew"), FolderName));
	}, TEXT("New missing preview mesh")), TEXT("AnimMontage.MissingReference"), TEXT("/Body/PreviewMesh"), TEXT("New missing preview mesh"));
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Track = Parse(Serialize(RowAt(Document, TEXT("SlotAnimTracks"), 1)));
		Track->SetStringField(TEXT("SlotName"), TEXT("TolerantNewSlot"));
		Append(Document, TEXT("SlotAnimTracks"), Track);
	}, TEXT("New unknown slot")), TEXT("AnimMontage.UnknownSlot"), TEXT("/Body/SlotAnimTracks/2/SlotName"), TEXT("New unknown slot"));
	return !HasAnyErrors();
}

#endif
