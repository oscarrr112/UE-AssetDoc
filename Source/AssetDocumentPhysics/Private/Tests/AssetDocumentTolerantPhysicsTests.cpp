// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An editor-made Physics Asset holding content new input may not add: two constraints with one joint name, a
// constraint whose two bones are one, profile data of a constraint profile the asset no longer lists, and a level set
// shape (made only by the editor's fit tools). It reads with warnings and a marker for the level set, its own extract
// re-applies as a no-op, another edit works and keeps all of it, and new input of the same kind is still refused.
// A second asset holds two bodies on one bone (tool-made content): the later reads under its own key with a warning,
// and is kept and edited by that key; removing the later removes only that body (the bone's constraints, the other
// bodies and the collision table stay as they were); removing the first keeps the later one, which then reads under
// the bone's key.

#include "AssetDocumentTolerantTestSupport.h"

#include "Misc/ScopeExit.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentTolerantPhysicsTestsLocal
{
const TCHAR* TutorialPhysicsAsset = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_PhysicsAsset.TutorialTPP_PhysicsAsset");

TSharedPtr<FJsonObject> Region(const TSharedPtr<FJsonObject>& Document, const TCHAR* Name)
{
	return Document->GetObjectField(TEXT("Body"))->GetObjectField(Name);
}

// Sets a body entry's mass override in the document.
void SetMassOverride(const TSharedPtr<FJsonObject>& Entry, double Mass)
{
	const TSharedPtr<FJsonObject> Properties = Entry->HasField(TEXT("Properties")) ? Entry->GetObjectField(TEXT("Properties")) : MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject> Instance = Properties->HasField(TEXT("DefaultInstance"))
		? Properties->GetObjectField(TEXT("DefaultInstance")) : MakeShared<FJsonObject>();
	Instance->SetBoolField(TEXT("bOverrideMass"), true);
	Instance->SetNumberField(TEXT("MassInKgOverride"), Mass);
	Properties->SetObjectField(TEXT("DefaultInstance"), Instance);
	Entry->SetObjectField(TEXT("Properties"), Properties);
}

// Each constraint of the asset as "<joint>: <child> -> <parent>", in array order.
TArray<FString> JointRows(const UPhysicsAsset& Asset)
{
	TArray<FString> Rows;
	for (const UPhysicsConstraintTemplate* Joint : Asset.ConstraintSetup)
	{
		Rows.Add(Joint == nullptr ? FString() : FString::Printf(TEXT("%s: %s -> %s"), *Joint->DefaultInstance.JointName.ToString(),
			*Joint->DefaultInstance.ConstraintBone1.ToString(), *Joint->DefaultInstance.ConstraintBone2.ToString()));
	}
	return Rows;
}

// The number of constraints with Bone as their child or parent.
int32 JointsOnBone(const UPhysicsAsset& Asset, const FString& Bone)
{
	int32 Count = 0;
	for (const UPhysicsConstraintTemplate* Joint : Asset.ConstraintSetup)
	{
		if (Joint != nullptr && (Joint->DefaultInstance.ConstraintBone1.ToString() == Bone || Joint->DefaultInstance.ConstraintBone2.ToString() == Bone))
		{
			++Count;
		}
	}
	return Count;
}

// The bodies on Bone.
TArray<const USkeletalBodySetup*> BodiesOnBone(const UPhysicsAsset& Asset, const FString& Bone)
{
	TArray<const USkeletalBodySetup*> Bodies;
	for (const USkeletalBodySetup* Body : Asset.SkeletalBodySetups)
	{
		if (Body != nullptr && Body->BoneName.ToString() == Bone)
		{
			Bodies.Add(Body);
		}
	}
	return Bodies;
}

// The Content Browser's Duplicate of the tutorial mannequin's physics asset, with a tool's copy of the first body
// appended on the same bone and given a sphere of its own. Null when the engine asset is missing.
UPhysicsAsset* MakeTwinBodyAsset(const AssetDocumentTolerantTestSupport::FTarget& Target, FString& OutBone, int32& OutFirstSpheres)
{
	UPhysicsAsset* Source = LoadObject<UPhysicsAsset>(nullptr, TutorialPhysicsAsset);
	if (Source == nullptr || Source->SkeletalBodySetups.Num() < 2)
	{
		return nullptr;
	}
	UPhysicsAsset* Asset = DuplicateObject<UPhysicsAsset>(Source, CreatePackage(*Target.PackageName), *Target.AssetName);
	Asset->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Asset);
	Asset->Modify();
	OutBone = Asset->SkeletalBodySetups[0]->BoneName.ToString();
	OutFirstSpheres = Asset->SkeletalBodySetups[0]->AggGeom.SphereElems.Num();
	USkeletalBodySetup* Twin = DuplicateObject<USkeletalBodySetup>(Asset->SkeletalBodySetups[0], Asset);
	Twin->AggGeom.SphereElems.Add(FKSphereElem(3.f));
	Asset->SkeletalBodySetups.Add(Twin);
	Asset->UpdateBodySetupIndexMap();
	Asset->UpdateBoundsBodiesArray();
	Asset->MarkPackageDirty();
	return Asset;
}

// A document region as JSON text ("" when the region is missing).
FString RegionText(const TSharedPtr<FJsonObject>& Document, const TCHAR* Name)
{
	if (!Document.IsValid() || !Document->HasField(TEXT("Body")))
	{
		return FString();
	}
	const TSharedPtr<FJsonObject> Body = Document->GetObjectField(TEXT("Body"));
	if (!Body.IsValid() || !Body->HasField(Name))
	{
		return FString();
	}
	const TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
	Wrapper->SetField(Name, Body->TryGetField(Name));
	return AssetDocumentTolerantTestSupport::Serialize(Wrapper);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantPhysicsAssetTest, "UEAssetDoc.Tolerant.PhysicsAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantPhysicsAssetTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantPhysicsTestsLocal;
	const FTarget Target(TEXT("PA_Tolerant"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// The Content Browser's Duplicate of the tutorial mannequin's physics asset.
	UPhysicsAsset* Source = LoadObject<UPhysicsAsset>(nullptr, TutorialPhysicsAsset);
	if (!TestNotNull(TEXT("tutorial physics asset"), Source) || !TestTrue(TEXT("constraints"), Source->ConstraintSetup.Num() >= 2))
	{
		return false;
	}
	UPhysicsAsset* Asset = DuplicateObject<UPhysicsAsset>(Source, CreatePackage(*Target.PackageName), *Target.AssetName);
	Asset->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Asset);

	// A rigger's second constraint on a joint that has one, a constraint left on one bone by a bone rename, profile
	// data of a profile since dropped from the list, and a level set fitted to the last body.
	Asset->Modify();
	const FString Joint = Asset->ConstraintSetup[0]->DefaultInstance.JointName.ToString();
	UPhysicsConstraintTemplate* Twin = DuplicateObject<UPhysicsConstraintTemplate>(Asset->ConstraintSetup[0], Asset);
	Asset->ConstraintSetup.Add(Twin);
	UPhysicsConstraintTemplate* Self = DuplicateObject<UPhysicsConstraintTemplate>(Asset->ConstraintSetup[1], Asset);
	Self->DefaultInstance.JointName = TEXT("self_joint");
	Self->DefaultInstance.ConstraintBone2 = Self->DefaultInstance.ConstraintBone1;
	Asset->ConstraintSetup.Add(Self);
	Asset->ConstraintSetup[1]->AddConstraintProfile(TEXT("Retired"));
	USkeletalBodySetup* LastBody = Asset->SkeletalBodySetups.Last();
	const FString Bone = LastBody->BoneName.ToString();
	// A fitted level set holds a grid (an element without one does not serialize).
	FKLevelSetElem LevelSet;
	LevelSet.BuildLevelSet(FTransform::Identity, TArray<double>({-1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0}), FIntVector(2, 2, 2), 4.f);
	LastBody->AggGeom.LevelSetElems.Add(LevelSet);
	Asset->UpdateBodySetupIndexMap();
	Asset->UpdateBoundsBodiesArray();
	Asset->MarkPackageDirty();
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const FString TwinKey = FString::Printf(TEXT("/Body/Constraints/%s~02"), *Joint);
	const FString LevelSets = FString::Printf(TEXT("/Body/Bodies/%s/Shapes/LevelSets"), *Bone);
	FExpected Expected;
	Expected.Warnings = {
		{TEXT("PhysicsAsset.DuplicateJointName"), TwinKey},
		{TEXT("PhysicsAsset.InvalidBody"), TEXT("/Body/Constraints/self_joint/Parent")},
		{TEXT("PhysicsAsset.Unrepresented"), LevelSets}};
	Expected.Markers = {LevelSets};
	if (!TestTolerantRoundTrip(*this, Target, Expected, TEXT("Physics asset")).IsValid())
	{
		return false;
	}

	const FString FirstBone = Asset->SkeletalBodySetups[0]->BoneName.ToString();
	if (TestTrue(TEXT("another body's mass changes"), ApplyEdited(*this, Target, [&FirstBone](const TSharedPtr<FJsonObject>& Document)
	{
		SetMassOverride(Region(Document, TEXT("Bodies"))->GetObjectField(FirstBone), 25);
	}, TEXT("Edit")).IsSuccess()))
	{
		Asset = Target.Get<UPhysicsAsset>();
		if (TestNotNull(TEXT("asset"), Asset))
		{
			TestEqual(TEXT("mass"), Asset->SkeletalBodySetups[0]->DefaultInstance.GetMassOverride(), 25.f);
			TestEqual(TEXT("constraints kept"), Asset->ConstraintSetup.Num(), Source->ConstraintSetup.Num() + 2);
			const int32 Body = Asset->FindBodyIndex(*Bone);
			TestTrue(TEXT("level set kept"), Body != INDEX_NONE && Asset->SkeletalBodySetups[Body]->AggGeom.LevelSetElems.Num() == 1);
			TestTrue(TEXT("retired profile data kept"), Asset->ConstraintSetup[1]->ContainsConstraintProfile(TEXT("Retired")));
		}
		TestStillShows(*this, Target, TEXT("PhysicsAsset.DuplicateJointName"), TwinKey, TEXT("After edit"));
	}

	TestRefused(*this, ApplyEdited(*this, Target, [&Joint](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Constraints = Region(Document, TEXT("Constraints"));
		Constraints->SetObjectField(TEXT("extra~2"), Parse(Serialize(Constraints->GetObjectField(Joint))));
	}, TEXT("New repeated name")), TEXT("PhysicsAsset.InvalidBody"), TEXT("/Body/Constraints/extra~02"), TEXT("New repeated name"));
	TestRefused(*this, ApplyEdited(*this, Target, [&Bone](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Shapes = Region(Document, TEXT("Bodies"))->GetObjectField(Bone)->GetObjectField(TEXT("Shapes"));
		Shapes->GetObjectField(TEXT("LevelSets"))->SetStringField(TEXT("Unsupported"), TEXT("edited"));
	}, TEXT("Edited marker")), TEXT("PhysicsAsset.UnrepresentedValue"), LevelSets, TEXT("Edited marker"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPhysicsDuplicateBoneBodiesTest, "UEAssetDoc.Codec.PhysicsAsset.DuplicateBoneBodies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPhysicsDuplicateBoneBodiesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantPhysicsTestsLocal;
	const FTarget Target(TEXT("PA_DuplicateBone"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// The Content Browser's Duplicate of the tutorial mannequin's physics asset.
	UPhysicsAsset* Source = LoadObject<UPhysicsAsset>(nullptr, TutorialPhysicsAsset);
	if (!TestNotNull(TEXT("tutorial physics asset"), Source) || !TestTrue(TEXT("bodies"), Source->SkeletalBodySetups.Num() >= 2))
	{
		return false;
	}
	UPhysicsAsset* Asset = DuplicateObject<UPhysicsAsset>(Source, CreatePackage(*Target.PackageName), *Target.AssetName);
	Asset->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Asset);

	// A tool's copy of the first body appended on the same bone, with a sphere of its own.
	Asset->Modify();
	const FString Bone = Asset->SkeletalBodySetups[0]->BoneName.ToString();
	const int32 FirstSpheres = Asset->SkeletalBodySetups[0]->AggGeom.SphereElems.Num();
	USkeletalBodySetup* Twin = DuplicateObject<USkeletalBodySetup>(Asset->SkeletalBodySetups[0], Asset);
	Twin->AggGeom.SphereElems.Add(FKSphereElem(3.f));
	Asset->SkeletalBodySetups.Add(Twin);
	Asset->UpdateBodySetupIndexMap();
	Asset->UpdateBoundsBodiesArray();
	Asset->MarkPackageDirty();
	const int32 BodyCount = Asset->SkeletalBodySetups.Num();
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const FString TwinKey = Bone + TEXT("~2");
	const FString TwinPath = FString::Printf(TEXT("/Body/Bodies/%s~02"), *Bone);
	FExpected Expected;
	Expected.Warnings = {{TEXT("PhysicsAsset.DuplicateBoneBody"), TwinPath}};
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, Expected, TEXT("Physics asset"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Bodies = Region(Extracted, TEXT("Bodies"));
	TestTrue(TEXT("both bodies read"), Bodies->HasField(Bone) && Bodies->HasField(TwinKey));
	const TSharedPtr<FJsonValue> TwinSpheres = At(Extracted, TwinPath + TEXT("/Shapes/Spheres"));
	TestTrue(TEXT("the later body reads its own shapes"), TwinSpheres.IsValid() && TwinSpheres->Type == EJson::Array
		&& TwinSpheres->AsArray().Num() == FirstSpheres + 1);

	if (TestTrue(TEXT("the later body's mass changes"), ApplyEdited(*this, Target, [&TwinKey](const TSharedPtr<FJsonObject>& Document)
	{
		SetMassOverride(Region(Document, TEXT("Bodies"))->GetObjectField(TwinKey), 12);
	}, TEXT("Edit")).IsSuccess()))
	{
		Asset = Target.Get<UPhysicsAsset>();
		if (TestNotNull(TEXT("asset"), Asset) && TestEqual(TEXT("bodies kept"), Asset->SkeletalBodySetups.Num(), BodyCount))
		{
			const USkeletalBodySetup* Later = Asset->SkeletalBodySetups.Last();
			const USkeletalBodySetup* First = Asset->SkeletalBodySetups[0];
			TestTrue(TEXT("both on the bone"), First->BoneName.ToString() == Bone && Later->BoneName.ToString() == Bone);
			TestEqual(TEXT("the later body's mass"), Later->DefaultInstance.GetMassOverride(), 12.f);
			TestEqual(TEXT("the later body keeps its sphere"), Later->AggGeom.SphereElems.Num(), FirstSpheres + 1);
			TestEqual(TEXT("the first body's mass is unchanged"), First->DefaultInstance.GetMassOverride(),
				Source->SkeletalBodySetups[0]->DefaultInstance.GetMassOverride());
			TestEqual(TEXT("the first body's shapes kept"), First->AggGeom.SphereElems.Num(), FirstSpheres);
		}
		TestStillShows(*this, Target, TEXT("PhysicsAsset.DuplicateBoneBody"), TwinPath, TEXT("After edit"));
	}

	const FString Other = Source->SkeletalBodySetups[1]->BoneName.ToString();
	TestRefused(*this, ApplyEdited(*this, Target, [&Other](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Entries = Region(Document, TEXT("Bodies"));
		Entries->SetObjectField(Other + TEXT("~2"), Parse(Serialize(Entries->GetObjectField(Other))));
	}, TEXT("New repeated bone")), TEXT("PhysicsAsset.InvalidBody"), FString::Printf(TEXT("/Body/Bodies/%s~02"), *Other), TEXT("New repeated bone"));

	// Removing the later body removes only that body: the bone keeps its first body and every constraint on it.
	Asset = Target.Get<UPhysicsAsset>();
	const FAssetDocumentResult Before = ExtractResult(Target);
	if (!TestNotNull(TEXT("asset before removal"), Asset) || !TestTrue(TEXT("extract before removal"), Before.IsSuccess() && Before.Payload.IsValid()))
	{
		return false;
	}
	const TArray<FString> JointsBefore = JointRows(*Asset);
	const int32 BoneJoints = JointsOnBone(*Asset, Bone);
	TestTrue(TEXT("the bone has constraints"), BoneJoints > 0);
	const TSharedPtr<FJsonObject> BodiesBefore = Region(Before.Payload, TEXT("Bodies"));
	if (TestTrue(TEXT("the later body is removed"), ApplyEdited(*this, Target, [&TwinKey](const TSharedPtr<FJsonObject>& Document)
	{
		Region(Document, TEXT("Bodies"))->RemoveField(TwinKey);
	}, TEXT("Remove the later body")).IsSuccess()))
	{
		Asset = Target.Get<UPhysicsAsset>();
		if (TestNotNull(TEXT("asset after removal"), Asset))
		{
			TestEqual(TEXT("one body fewer"), Asset->SkeletalBodySetups.Num(), BodyCount - 1);
			const TArray<const USkeletalBodySetup*> Kept = BodiesOnBone(*Asset, Bone);
			if (TestEqual(TEXT("one body on the bone"), Kept.Num(), 1))
			{
				TestEqual(TEXT("the first body is the one kept"), Kept[0]->AggGeom.SphereElems.Num(), FirstSpheres);
			}
			TestEqual(TEXT("the bone's constraints kept"), JointsOnBone(*Asset, Bone), BoneJoints);
			TestEqual(TEXT("constraint count"), Asset->ConstraintSetup.Num(), JointsBefore.Num());
			TestTrue(TEXT("constraints keep their names, children and parents"), JointRows(*Asset) == JointsBefore);
		}
		const FAssetDocumentResult After = ExtractResult(Target);
		if (TestTrue(TEXT("extract after removal"), After.IsSuccess() && After.Payload.IsValid()))
		{
			TestFalse(TEXT("no repeated body left"), HasDiagnostic(After, TEXT("PhysicsAsset.DuplicateBoneBody"), FString()));
			TestEqual(TEXT("constraint data unchanged"), RegionText(After.Payload, TEXT("Constraints")), RegionText(Before.Payload, TEXT("Constraints")));
			TestEqual(TEXT("collision table unchanged"), RegionText(After.Payload, TEXT("CollisionDisabled")),
				RegionText(Before.Payload, TEXT("CollisionDisabled")));
			const TSharedPtr<FJsonObject> BodiesAfter = Region(After.Payload, TEXT("Bodies"));
			TestFalse(TEXT("the later key is gone"), BodiesAfter->HasField(TwinKey));
			TestEqual(TEXT("the other bodies remain"), BodiesAfter->Values.Num(), BodiesBefore->Values.Num() - 1);
			for (const auto& Entry : BodiesBefore->Values)
			{
				const FString EntryKey(*Entry.Key);
				if (EntryKey == TwinKey)
				{
					continue;
				}
				const FString KeptText = BodiesAfter->HasField(EntryKey) ? Serialize(BodiesAfter->GetObjectField(EntryKey)) : FString();
				TestEqual(*FString::Printf(TEXT("body %s unchanged"), *EntryKey), KeptText, Serialize(Entry.Value->AsObject()));
			}
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityPhysicsRemoveFirstRepeatedBodyTest, "UEAssetDoc.Fidelity.PhysicsAsset.RemoveFirstRepeatedBody",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelityPhysicsRemoveFirstRepeatedBodyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantPhysicsTestsLocal;
	(void)Parameters;
	const FTarget Target(TEXT("PA_RemoveFirstTwin"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	FString Bone;
	int32 FirstSpheres = 0;
	UPhysicsAsset* Asset = MakeTwinBodyAsset(Target, Bone, FirstSpheres);
	if (!TestNotNull(TEXT("asset"), Asset) || !TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}
	const int32 BodyCount = Asset->SkeletalBodySetups.Num();
	const TArray<FString> JointsBefore = JointRows(*Asset);
	const int32 BoneJoints = JointsOnBone(*Asset, Bone);
	const TWeakObjectPtr<USkeletalBodySetup> Twin = Asset->SkeletalBodySetups.Last().Get();
	const FString TwinKey = Bone + TEXT("~2");

	// The first body goes; the later one, with its own shapes and settings, stays and takes the bone's key.
	const FAssetDocumentResult Removed = ApplyEdited(*this, Target, [&Bone, &TwinKey](const TSharedPtr<FJsonObject>& Document)
	{
		Region(Document, TEXT("Bodies"))->RemoveField(Bone);
		SetMassOverride(Region(Document, TEXT("Bodies"))->GetObjectField(TwinKey), 9);
	}, TEXT("Remove the first body"));
	if (TestTrue(TEXT("the first body is removed"), Removed.IsSuccess()))
	{
		Asset = Target.Get<UPhysicsAsset>();
		if (TestNotNull(TEXT("asset after removal"), Asset))
		{
			TestEqual(TEXT("one body fewer"), Asset->SkeletalBodySetups.Num(), BodyCount - 1);
			const TArray<const USkeletalBodySetup*> Kept = BodiesOnBone(*Asset, Bone);
			if (TestEqual(TEXT("one body on the bone"), Kept.Num(), 1))
			{
				TestTrue(TEXT("the later body is the one kept"), Twin.IsValid() && Kept[0] == Twin.Get());
				TestEqual(TEXT("its own shapes"), Kept[0]->AggGeom.SphereElems.Num(), FirstSpheres + 1);
				TestEqual(TEXT("its edited mass"), Kept[0]->DefaultInstance.GetMassOverride(), 9.f);
			}
			TestEqual(TEXT("the bone's constraints kept"), JointsOnBone(*Asset, Bone), BoneJoints);
			TestTrue(TEXT("constraints keep their names, children and parents"), JointRows(*Asset) == JointsBefore);
		}
		const FAssetDocumentResult After = ExtractResult(Target);
		if (TestTrue(TEXT("extract after removal"), After.IsSuccess() && After.Payload.IsValid()))
		{
			TestFalse(TEXT("no repeated body left"), HasDiagnostic(After, TEXT("PhysicsAsset.DuplicateBoneBody"), FString()));
			const TSharedPtr<FJsonObject> Bodies = Region(After.Payload, TEXT("Bodies"));
			TestTrue(TEXT("the bone's key names the kept body"), Bodies->HasField(Bone) && !Bodies->HasField(TwinKey));
			const TSharedPtr<FJsonValue> Spheres = At(After.Payload, FString::Printf(TEXT("/Body/Bodies/%s/Shapes/Spheres"), *Bone));
			TestTrue(TEXT("the kept body's shapes read"), Spheres.IsValid() && Spheres->Type == EJson::Array && Spheres->AsArray().Num() == FirstSpheres + 1);
		}
		TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("After removal"));
	}
	return !HasAnyErrors();
}

#endif
