// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// UEAssetDoc.PhysicsAsset.CreateFidelity: design §6.2 as written: every source (generated fixtures,
//   the tutorial asset, the corpus) is extracted and created anew from that extraction (Action Create, FactoryContext
//   = its preview mesh): equal extract and status, equal runtime facts (bodies, per-body mass on a skeletal mesh
//   component, constraints, bounds bodies), unchanged re-apply no-op, reload stable; the mesh is never assigned.
// UEAssetDoc.PhysicsAsset.FromScratch: design §6.4: an AI-written ragdoll created on the tutorial
//   mannequin (11 bodies, 10 constraints with cone / twist limits, collision disabled between neighbours): UpToDate,
//   simulates 60 frames without NaNs; a Create without Bodies (Generation) then two bodies edited; Create with fitted
//   bodies (Fit); Inspect's Generation suggestion equals what a Create without Bodies makes; Validate of a Create
//   makes nothing; typical errors at document paths with nothing left behind.

#include "AssetDocumentPhysicsTestSupport.h"

#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonValue.h"
#include "Engine/CollisionProfile.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PreviewScene.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/AssetDocumentJsonKeys.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentPhysicsCreateTests
{
using namespace AssetDocumentPhysicsTests;

TSharedPtr<FJsonObject> CreateDocument(const FTarget& Target, const FString& Mesh, const FString& Properties, const FString& Body)
{
	TSharedPtr<FJsonObject> Document = Parse(FString::Printf(TEXT(R"({"SchemaVersion": 1, "Class": "%s", "Action": "Create", "Target": "%s", "Properties": %s, "Body": %s})"),
		PhysicsAssetClass, *Target.PackageName, *Properties, *Body));
	if (Document.IsValid() && !Mesh.IsEmpty())
	{
		Document->SetStringField(TEXT("FactoryContext"), Mesh);
	}
	return Document;
}

// An extraction as a Create of Target, made for the extraction's preview mesh.
TSharedPtr<FJsonObject> ForCreate(const TSharedPtr<FJsonObject>& Document, const FTarget& Target)
{
	const TSharedPtr<FJsonObject> Copy = ForTarget(Document, Target, TEXT("Create"));
	FString Mesh;
	if (BodyOf(Copy)->TryGetStringField(TEXT("PreviewMesh"), Mesh))
	{
		Copy->SetStringField(TEXT("FactoryContext"), Mesh);
	}
	return Copy;
}

bool ApplyOk(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const FString& Stage,
	FAssetDocumentResult* OutResult = nullptr)
{
	const FAssetDocumentResult Result = Apply(Target, Document);
	if (OutResult != nullptr)
	{
		*OutResult = Result;
	}
	if (!Test.TestTrue(*FString::Printf(TEXT("%s applies"), *Stage), Result.IsSuccess()))
	{
		Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *DiagnosticsText(Result)));
		return false;
	}
	return true;
}

// What a skeletal mesh component sees of an asset (design §6.2 "runtime"): per body its mass on a component with the
// preview mesh, the constraints (joint, child, parent) and the bounds bodies, as sorted text.
FString RuntimeFacts(UPhysicsAsset& Asset)
{
	TStrongObjectPtr<USkeletalMeshComponent> Component(NewObject<USkeletalMeshComponent>(GetTransientPackage()));
	Component->SetSkeletalMeshAsset(Asset.GetPreviewMesh());
	Component->SetPhysicsAsset(&Asset);
	TArray<FString> Lines;
	for (const USkeletalBodySetup* Body : Asset.SkeletalBodySetups)
	{
		Lines.Add(FString::Printf(TEXT("body %s %.3f"), *Body->BoneName.ToString(), Body->CalculateMass(Component.Get())));
	}
	for (const UPhysicsConstraintTemplate* Constraint : Asset.ConstraintSetup)
	{
		Lines.Add(FString::Printf(TEXT("constraint %s %s %s"), *Constraint->DefaultInstance.JointName.ToString(),
			*Constraint->DefaultInstance.ConstraintBone1.ToString(), *Constraint->DefaultInstance.ConstraintBone2.ToString()));
	}
	for (const int32 Index : Asset.BoundsBodies)
	{
		Lines.Add(FString::Printf(TEXT("bounds %s"), Asset.SkeletalBodySetups.IsValidIndex(Index) ? *Asset.SkeletalBodySetups[Index]->BoneName.ToString() : TEXT("?")));
	}
	Lines.Sort();
	return FString::Join(Lines, TEXT("\n"));
}

// A failed or refused Create leaves nothing: no live asset, no package file, no registry entry.
void ExpectNothingCreated(FAutomationTestBase& Test, const FTarget& Target, const FString& Stage)
{
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	const UObject* Left = Target.Get();
	Test.TestTrue(*FString::Printf(TEXT("%s: no asset left"), *Stage), Left == nullptr || !IsValid(Left));
	Test.TestFalse(*FString::Printf(TEXT("%s: no package file"), *Stage), IFileManager::Get().FileExists(*Target.PackageFile));
	Test.TestFalse(*FString::Printf(TEXT("%s: no registry entry"), *Stage),
		FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Target.ObjectPath)).IsValid());
}

void ExpectCreateFailure(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const TCHAR* Code,
	const FString& Path, const FString& Stage)
{
	const FAssetDocumentResult Result = Apply(Target, Document);
	Test.TestFalse(*FString::Printf(TEXT("%s fails"), *Stage), Result.IsSuccess());
	Test.TestTrue(*FString::Printf(TEXT("%s: %s (got %s)"), *Stage, Code, *DiagnosticsText(Result)), HasCode(Result, Code));
	Test.TestTrue(*FString::Printf(TEXT("%s: at %s (got %s)"), *Stage, *Path, *FirstPath(Result)), FirstPath(Result).StartsWith(Path));
	ExpectNothingCreated(Test, Target, Stage);
}

// The ragdoll an AI writes for the tutorial mannequin (design §6.4): pelvis, chest, head, upper and lower arms, thighs
// and calves; each joint named after its child bone (the editor's name), cone / twist limits, elbows and knees hinged.
FString RagdollBody()
{
	auto Capsule = [](double Radius, double Length) {
		return FString::Printf(TEXT(R"({"Capsules": [{"Rotation": {"Pitch": 90, "Yaw": 0, "Roll": 0}, "Radius": %g, "Length": %g}]})"), Radius, Length);
	};
	const FString Joint = TEXT(R"({"ProfileInstance": {"ConeLimit": {"Swing1Motion": "ACM_Limited", "Swing2Motion": "ACM_Limited", "Swing1LimitDegrees": 30, "Swing2LimitDegrees": 30}, "TwistLimit": {"TwistMotion": "ACM_Limited", "TwistLimitDegrees": 20}}})");
	const FString Hinge = TEXT(R"({"ProfileInstance": {"ConeLimit": {"Swing1Motion": "ACM_Locked", "Swing2Motion": "ACM_Limited", "Swing2LimitDegrees": 70}, "TwistLimit": {"TwistMotion": "ACM_Locked"}}})");
	const TCHAR* const Pairs[][2] = {{TEXT("spine_03"), TEXT("pelvis")}, {TEXT("head"), TEXT("spine_03")}, {TEXT("upperarm_l"), TEXT("spine_03")},
		{TEXT("lowerarm_l"), TEXT("upperarm_l")}, {TEXT("upperarm_r"), TEXT("spine_03")}, {TEXT("lowerarm_r"), TEXT("upperarm_r")},
		{TEXT("thigh_l"), TEXT("pelvis")}, {TEXT("calf_l"), TEXT("thigh_l")}, {TEXT("thigh_r"), TEXT("pelvis")}, {TEXT("calf_r"), TEXT("thigh_r")}};
	TArray<FString> Constraints;
	TArray<FString> Neighbours;
	for (const auto& Pair : Pairs)
	{
		const bool bHinge = FString(Pair[0]).StartsWith(TEXT("lowerarm")) || FString(Pair[0]).StartsWith(TEXT("calf"));
		Constraints.Add(FString::Printf(TEXT(R"("%s": {"Child": "%s", "Parent": "%s", "Properties": %s})"), Pair[0], Pair[0], Pair[1],
			bHinge ? *Hinge : *Joint));
		Neighbours.Add(FString::Printf(TEXT(R"(["%s", "%s"])"), Pair[1], Pair[0]));
	}
	return FString::Printf(TEXT(R"({"Bodies": {
		"pelvis": {"Shapes": {"Boxes": [{"X": 20, "Y": 30, "Z": 20}]}},
		"spine_03": {"Shapes": %s},
		"head": {"Shapes": {"Spheres": [{"Center": {"X": 8, "Y": 0, "Z": 0}, "Radius": 11}]}},
		"upperarm_l": {"Shapes": %s}, "lowerarm_l": {"Shapes": %s},
		"upperarm_r": {"Shapes": %s}, "lowerarm_r": {"Shapes": %s},
		"thigh_l": {"Shapes": %s}, "calf_l": {"Shapes": %s},
		"thigh_r": {"Shapes": %s}, "calf_r": {"Shapes": %s}},
		"Constraints": {%s},
		"CollisionDisabled": [%s]})"),
		*Capsule(12, 14), *Capsule(5, 18), *Capsule(4.5, 16), *Capsule(5, 18), *Capsule(4.5, 16), *Capsule(7, 26), *Capsule(6, 26),
		*Capsule(7, 26), *Capsule(6, 26), *FString::Join(Constraints, TEXT(", ")), *FString::Join(Neighbours, TEXT(", ")));
}

// A skeletal mesh component with the asset simulates Frames frames in a preview world (the Physics Asset Editor's
// simulation): the problems found (NaN transforms, nothing simulated).
TArray<FString> Simulate(UPhysicsAsset& Asset, int32 Frames)
{
	TArray<FString> Problems;
	FPreviewScene Scene(FPreviewScene::ConstructionValues().SetCreatePhysicsScene(true).ShouldSimulatePhysics(true).SetEditor(false));
	USkeletalMeshComponent* Component = NewObject<USkeletalMeshComponent>(GetTransientPackage());
	Component->SetSkeletalMeshAsset(Asset.GetPreviewMesh());
	Component->SetPhysicsAsset(&Asset);
	// A skinned mesh component has no collision by default (no physics state); the editor's preview simulates with collision.
	Component->SetCollisionProfileName(UCollisionProfile::PhysicsActor_ProfileName);
	Component->SetMobility(EComponentMobility::Movable);
	Scene.AddComponent(Component, FTransform::Identity);
	// FPhysicsAssetEditorSharedData::EnableSimulation (world solver).
	Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Component->SetSimulatePhysics(true);
	Component->ResetAllBodiesSimulatePhysics();
	Component->WakeAllRigidBodies();
	if (Component->Bodies.Num() != Asset.SkeletalBodySetups.Num())
	{
		Problems.Add(FString::Printf(TEXT("%d bodies instanced, %d in the asset"), Component->Bodies.Num(), Asset.SkeletalBodySetups.Num()));
	}
	TArray<FVector> Start;
	for (const FBodyInstance* Body : Component->Bodies)
	{
		Start.Add(Body != nullptr ? Body->GetUnrealWorldTransform().GetLocation() : FVector::ZeroVector);
	}
	FString Trace;
	for (int32 Frame = 0; Frame < Frames; ++Frame)
	{
		// The world's physics step as its start / end physics tick functions run it (a whole UWorld::Tick from inside
		// a test advanced the solver only once: the end-physics tick does not wait for the solver task there).
		UWorld& World = *Scene.GetWorld();
		World.SetupPhysicsTickFunctions(1.0f / 60.0f);
		World.StartPhysicsSim();
		World.GetPhysicsScene()->WaitPhysScenes();
		World.FinishPhysicsSim();
		if ((Frame % 10 == 0 || Frame == Frames - 1) && Component->Bodies.Num() > 0 && Component->Bodies[0] != nullptr)
		{
			Trace += FString::Printf(TEXT(" f%d z=%.2f vz=%.1f;"), Frame,
				Component->Bodies[0]->GetUnrealWorldTransform().GetLocation().Z, Component->Bodies[0]->GetUnrealWorldVelocity().Z);
		}
	}
	double Moved = 0.0;
	for (int32 Index = 0; Index < Component->Bodies.Num(); ++Index)
	{
		const FBodyInstance* Body = Component->Bodies[Index];
		if (Body == nullptr)
		{
			continue;
		}
		const FTransform Transform = Body->GetUnrealWorldTransform();
		if (Transform.ContainsNaN() || Body->GetUnrealWorldVelocity().ContainsNaN())
		{
			Problems.Add(FString::Printf(TEXT("body %d has NaN after %d frames"), Index, Frames));
		}
		Moved = FMath::Max(Moved, FVector::Dist(Transform.GetLocation(), Start[Index]));
	}
	if (Moved < 1.0)
	{
		int32 Simulating = 0;
		for (const FBodyInstance* Body : Component->Bodies)
		{
			Simulating += Body != nullptr && Body->IsInstanceSimulatingPhysics() ? 1 : 0;
		}
		Problems.Add(FString::Printf(TEXT("no body moved (%.3f cm, %d of %d bodies simulating, gravity %.0f, trace%s): the ragdoll did not simulate"),
			Moved, Simulating, Component->Bodies.Num(), Scene.GetWorld()->GetGravityZ(), *Trace));
	}
	Scene.RemoveComponent(Component);
	return Problems;
}

TSharedPtr<FJsonObject> Field(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
{
	const TSharedPtr<FJsonObject>* Child = nullptr;
	return Object.IsValid() && Object->TryGetObjectField(Name, Child) ? *Child : MakeShared<FJsonObject>();
}

// Every shape type a body has (Spheres, Boxes, ...).
TSet<FString> ShapeTypes(const TSharedPtr<FJsonObject>& Body)
{
	TSet<FString> Types;
	for (const auto& Entry : Field(Body, TEXT("Bodies"))->Values)
	{
		for (const auto& Shapes : Field(Entry.Value->AsObject(), TEXT("Shapes"))->Values)
		{
			Types.Add(FString(*Shapes.Key));
		}
	}
	return Types;
}
} // namespace AssetDocumentPhysicsCreateTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPhysicsCreateFidelityTest, "UEAssetDoc.PhysicsAsset.CreateFidelity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPhysicsCreateFidelityTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPhysicsCreateTests;
	AddExpectedMessagePlain(TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	const FTemplatePacks Packs = CorpusPacks();
	FFixtures Fixtures;
	TArray<TPair<FString, FString>> Sources; // name, object path
	const FTarget& Capsules = Fixtures.Add(TEXT("PA_Generated"));
	if (Generate(*this, Capsules, TutorialMesh, FPhysAssetCreateParams()) != nullptr)
	{
		Sources.Emplace(TEXT("generated (capsules)"), Capsules.ObjectPath);
	}
	const FTarget& Convexes = Fixtures.Add(TEXT("PA_Convex"));
	FPhysAssetCreateParams ConvexParams;
	ConvexParams.GeomType = EFG_SingleConvexHull;
	if (Generate(*this, Convexes, TutorialMesh, ConvexParams) != nullptr)
	{
		Sources.Emplace(TEXT("generated (convex hulls)"), Convexes.ObjectPath);
	}
	for (const FCorpusEntry& Entry : Corpus)
	{
		if (LoadCorpus(Entry) != nullptr)
		{
			Sources.Emplace(CorpusName(Entry), Entry.Path);
		}
		else
		{
			AddInfo(FString::Printf(TEXT("SKIP %s: not installed / enabled"), *CorpusName(Entry)));
		}
	}
	for (const TPair<FString, FString>& Source : Sources)
	{
		const FString& Name = Source.Key;
		const FAssetDocumentResult Original = ExtractResult(Source.Value);
		UPhysicsAsset* SourceAsset = Cast<UPhysicsAsset>(LoadQuiet(Source.Value));
		if (!TestTrue(*FString::Printf(TEXT("%s extracts"), *Name), Original.IsSuccess() && Original.Payload.IsValid() && SourceAsset != nullptr))
		{
			AddInfo(DiagnosticsText(Original));
			continue;
		}
		// Held across the applies (each persisted apply ends with a full GC).
		const TStrongObjectPtr<USkeletalMesh> Mesh(SourceAsset->GetPreviewMesh());
		if (!Mesh.IsValid())
		{
			AddInfo(FString::Printf(TEXT("SKIP create %s: no preview mesh (a physics asset is made for a mesh)"), *Name));
			continue;
		}
		const UPhysicsAsset* MeshAssetBefore = Mesh->GetPhysicsAsset();
		const bool bMeshDirtyBefore = Mesh->GetOutermost()->IsDirty();
		const FString SourceFacts = RuntimeFacts(*SourceAsset);
		const FTarget& Created = Fixtures.Add(TEXT("PA_Created"));
		const double Start = FPlatformTime::Seconds();
		FAssetDocumentResult Applied;
		if (!ApplyOk(*this, Created, ForCreate(Original.Payload, Created), Name + TEXT(" created"), &Applied))
		{
			continue;
		}
		const double CreateMs = Milliseconds(Start);
		TestTrue(*FString::Printf(TEXT("%s: saved"), *Name), Applied.bSavedAsset);
		const FAssetDocumentResult Recreated = ExtractResult(Created.ObjectPath);
		const FString Expected = WithoutTarget(Original.Payload);
		const FString Got = WithoutTarget(Recreated.Payload);
		if (!TestEqual(*FString::Printf(TEXT("%s: the created asset extracts as the source"), *Name), Got, Expected))
		{
			AddInfo(FirstDifference(Expected, Got));
		}
		const FString SourceName = FPackageName::ObjectPathToObjectName(Source.Value);
		TestEqual(*FString::Printf(TEXT("%s: equal status"), *Name), StatusOf(Recreated).Replace(*Created.AssetName, TEXT("")),
			StatusOf(Original).Replace(*SourceName, TEXT("")));
		if (UPhysicsAsset* CreatedAsset = Created.GetAsset())
		{
			const FString CreatedFacts = RuntimeFacts(*CreatedAsset);
			if (!TestEqual(*FString::Printf(TEXT("%s: equal runtime facts"), *Name), CreatedFacts, SourceFacts))
			{
				AddInfo(FirstDifference(SourceFacts, CreatedFacts));
			}
		}
		TestTrue(*FString::Printf(TEXT("%s: the mesh keeps its physics asset"), *Name), Mesh->GetPhysicsAsset() == MeshAssetBefore);
		TestEqual(*FString::Printf(TEXT("%s: the mesh package is untouched"), *Name), Mesh->GetOutermost()->IsDirty(), bMeshDirtyBefore);
		const FAssetDocumentResult Again = Apply(Created, ForTarget(Recreated.Payload, Created));
		TestTrue(*FString::Printf(TEXT("%s: unchanged re-apply (%s)"), *Name, *DiagnosticsText(Again)), Again.IsSuccess() && !Again.bSavedAsset);
		TestNotNull(*FString::Printf(TEXT("%s: reload"), *Name), Reload(Created));
		TestEqual(*FString::Printf(TEXT("%s: equal after reload"), *Name), WithoutTarget(ExtractResult(Created.ObjectPath).Payload), Expected);
		AddInfo(FString::Printf(TEXT("PASS %s: create %.0f ms, %d chars, status %s%s"), *Name, CreateMs, Expected.Len(), *StatusName(Recreated),
			Applied.Diagnostics.IsEmpty() ? TEXT("") : *(TEXT("; apply warnings: ") + DiagnosticsText(Applied).Left(400))));
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPhysicsFromScratchTest, "UEAssetDoc.PhysicsAsset.FromScratch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPhysicsFromScratchTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPhysicsCreateTests;
	// The unknown-mesh case loads a package that does not exist.
	AddExpectedMessagePlain(TEXT("/Game/NoSuchFolder/SKM_None"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	FFixtures Fixtures;
	const TStrongObjectPtr<USkeletalMesh> Mesh(LoadObject<USkeletalMesh>(nullptr, TutorialMesh));
	if (!TestTrue(TEXT("tutorial mesh"), Mesh.IsValid()))
	{
		return false;
	}
	const UPhysicsAsset* MeshAssetBefore = Mesh->GetPhysicsAsset();

	// 1. The hand-written ragdoll: Validate makes nothing, Create makes it as written.
	const FTarget& Ragdoll = Fixtures.Add(TEXT("PA_Ragdoll"));
	const TSharedPtr<FJsonObject> RagdollDocument = CreateDocument(Ragdoll, TutorialMesh, TEXT("{}"), RagdollBody());
	{
		FAssetDocumentValidateRequest Request;
		Request.Document = RagdollDocument;
		const FAssetDocumentResult Validated = FAssetDocumentModule::Get().GetService().Validate(Request);
		TestTrue(*FString::Printf(TEXT("Validate of the Create: %s"), *DiagnosticsText(Validated)), Validated.IsSuccess());
		ExpectNothingCreated(*this, Ragdoll, TEXT("Validate"));
	}
	double Start = FPlatformTime::Seconds();
	FAssetDocumentResult Applied;
	if (ApplyOk(*this, Ragdoll, RagdollDocument, TEXT("ragdoll"), &Applied))
	{
		const double CreateMs = Milliseconds(Start);
		const FAssetDocumentResult Extracted = ExtractResult(Ragdoll.ObjectPath);
		const TSharedPtr<FJsonObject> Body = BodyOf(Extracted.Payload);
		TestEqual(TEXT("11 bodies"), CountField(Body, TEXT("Bodies")), 11);
		TestEqual(TEXT("10 constraints"), CountField(Body, TEXT("Constraints")), 10);
		TestEqual(TEXT("10 collision pairs"), CountField(Body, TEXT("CollisionDisabled")), 10);
		TestEqual(*FString::Printf(TEXT("status UpToDate: %s"), *StatusOf(Extracted)), StatusName(Extracted), FString(TEXT("UpToDate")));
		FString DiffText;
		TestEqual(*FString::Printf(TEXT("Diff of the written document is empty: %s"), *DiffText), DiffCount(ForTarget(RagdollDocument, Ragdoll), DiffText), 0);
		const FString Elbow = Serialize(Field(Field(Body, TEXT("Constraints")), TEXT("lowerarm_l")));
		TestTrue(*FString::Printf(TEXT("elbow hinge kept: %s"), *Elbow), Elbow.Contains(TEXT("\"Swing2LimitDegrees\":70")));
		if (UPhysicsAsset* Asset = Ragdoll.GetAsset())
		{
			TestTrue(TEXT("preview mesh"), Asset->GetPreviewMesh() == Mesh.Get());
			const TArray<FString> Problems = Simulate(*Asset, 60);
			TestTrue(*FString::Printf(TEXT("simulates 60 frames: %s"), *FString::Join(Problems, TEXT("; "))), Problems.IsEmpty());
		}
		AddInfo(FString::Printf(TEXT("ragdoll: create %.0f ms, %d chars, status %s"), CreateMs, Serialize(Extracted.Payload).Len(), *StatusName(Extracted)));
	}

	// 2. Without Bodies: the editor's generation with the document's settings, then two bodies edited.
	const FTarget& Boxes = Fixtures.Add(TEXT("PA_Boxes"));
	Start = FPlatformTime::Seconds();
	if (ApplyOk(*this, Boxes, CreateDocument(Boxes, TutorialMesh, TEXT("{}"), TEXT(R"({"Generation": {"GeomType": "EFG_Box", "MinBoneSize": 10}})")),
		TEXT("generation")))
	{
		const double CreateMs = Milliseconds(Start);
		const TSharedPtr<FJsonObject> Generated = Extract(*this, Boxes.ObjectPath, TEXT("generated"));
		const TSharedPtr<FJsonObject> Bodies = Field(BodyOf(Generated), TEXT("Bodies"));
		TArray<FString> Bones;
		AssetDocumentServerJson::GetKeys(*Bodies, Bones);
		if (!TestTrue(TEXT("bodies generated"), Bones.Num() > 5))
		{
			return false;
		}
		const TSet<FString> Types = ShapeTypes(BodyOf(Generated));
		TestTrue(*FString::Printf(TEXT("only boxes: %s"), *FString::Join(Types.Array(), TEXT(", "))), Types.Num() == 1 && Types.Contains(TEXT("Boxes")));
		TestFalse(TEXT("Generation is not extracted"), Serialize(Generated).Contains(TEXT("Generation")));
		const FString First = Bones[0];
		const FString Second = Bones[1];
		const TSharedPtr<FJsonObject> Edit = ForTarget(Generated, Boxes);
		const TSharedPtr<FJsonObject> EditBodies = Field(BodyOf(Edit), TEXT("Bodies"));
		EditBodies->GetObjectField(First)->SetObjectField(TEXT("Properties"), Parse(TEXT(R"({"PhysicsType": "PhysType_Kinematic"})")));
		EditBodies->GetObjectField(Second)->SetObjectField(TEXT("Shapes"), Parse(TEXT(R"({"Boxes": [{"X": 12, "Y": 9, "Z": 7}]})")));
		if (ApplyOk(*this, Boxes, Edit, TEXT("two bodies edited")))
		{
			const TSharedPtr<FJsonObject> After = Field(BodyOf(ExtractResult(Boxes.ObjectPath).Payload), TEXT("Bodies"));
			TestTrue(TEXT("kinematic"), Serialize(After->GetObjectField(First)).Contains(TEXT("PhysType_Kinematic")));
			const TArray<TSharedPtr<FJsonValue>>* BoxElements = nullptr;
			const bool bOneBox = Field(After->GetObjectField(Second), TEXT("Shapes"))->TryGetArrayField(TEXT("Boxes"), BoxElements) && BoxElements->Num() == 1;
			const TSharedPtr<FJsonObject> Box = bOneBox ? (*BoxElements)[0]->AsObject() : MakeShared<FJsonObject>();
			TestTrue(*FString::Printf(TEXT("box resized: %s"), *Serialize(After->GetObjectField(Second))), bOneBox
				&& Box->GetNumberField(TEXT("X")) == 12.0 && Box->GetNumberField(TEXT("Y")) == 9.0 && Box->GetNumberField(TEXT("Z")) == 7.0);
		}
		AddInfo(FString::Printf(TEXT("generation: create %.0f ms, %d bodies (%s, %s edited)"), CreateMs, Bodies->Values.Num(), *First, *Second));
	}

	// 3. Fitted bodies: Fit without Shapes is the editor's Add Body fit, named as the editor names them.
	const FTarget& Fitted = Fixtures.Add(TEXT("PA_Fitted"));
	if (ApplyOk(*this, Fitted, CreateDocument(Fitted, TutorialMesh, TEXT("{}"), TEXT(R"({"Bodies": {
			"pelvis": {"Fit": {}}, "spine_03": {"Fit": {"GeomType": "EFG_Sphere"}}, "head": {"Shapes": {"Spheres": [{"Radius": 10}]}}}})")),
		TEXT("fitted bodies")))
	{
		const UPhysicsAsset* Asset = Fitted.GetAsset();
		const int32 Pelvis = Asset->FindBodyIndex(TEXT("pelvis"));
		const int32 Spine = Asset->FindBodyIndex(TEXT("spine_03"));
		if (TestTrue(TEXT("fitted bodies exist"), Pelvis != INDEX_NONE && Spine != INDEX_NONE))
		{
			const FKAggregateGeom& PelvisGeom = Asset->SkeletalBodySetups[Pelvis]->AggGeom;
			const FKAggregateGeom& SpineGeom = Asset->SkeletalBodySetups[Spine]->AggGeom;
			TestTrue(TEXT("pelvis: the project's generation settings (a capsule)"), PelvisGeom.SphylElems.Num() == 1
				&& PelvisGeom.SphylElems[0].GetName() == TEXT("pelvis_capsule"));
			TestTrue(TEXT("spine_03: a fitted sphere"), SpineGeom.SphereElems.Num() == 1 && SpineGeom.SphereElems[0].GetName() == TEXT("spine_03_sphere"));
		}
		TestEqual(TEXT("editor constraints of the added bodies"), Asset->ConstraintSetup.Num(), 2);
	}

	// 4. Inspect's Generation suggestion is what a Create without Bodies and Generation makes.
	const FTarget& Defaults = Fixtures.Add(TEXT("PA_Defaults"));
	if (ApplyOk(*this, Defaults, CreateDocument(Defaults, TutorialMesh, TEXT("{}"), TEXT("{}")), TEXT("default generation")))
	{
		const FReadSideEffects SideEffects;
		const FAssetDocumentResult Inspected = Inspect(Defaults.ObjectPath);
		const TSharedPtr<FJsonObject> Suggestion = Inspected.Payload.IsValid() ? Field(Inspected.Payload, TEXT("Generation")) : nullptr;
		if (TestTrue(*FString::Printf(TEXT("Inspect has Generation: %s"), *DiagnosticsText(Inspected)), Suggestion.IsValid() && Suggestion->HasField(TEXT("Body"))))
		{
			TestTrue(TEXT("suggestion settings"), Field(Suggestion, TEXT("Settings"))->HasField(TEXT("GeomType")));
			const TSharedPtr<FJsonObject> Made = Parse(Serialize(BodyOf(ExtractResult(Defaults.ObjectPath).Payload)));
			Made->RemoveField(TEXT("PreviewMesh"));
			// Same values (key order and number spelling differ: Inspect's payload is not re-serialized canonically).
			const TSharedPtr<FJsonObject> Suggested = Field(Suggestion, TEXT("Body"));
			TestTrue(*FString::Printf(TEXT("the suggestion is the generated asset: %s"), *FirstDifference(Serialize(Suggested), Serialize(Made))),
				Suggested.IsValid() && FJsonValue::CompareEqual(FJsonValueObject(Suggested), FJsonValueObject(Made)));
		}
		TArray<FString> Problems;
		SideEffects.Check(Problems);
		TestTrue(*FString::Printf(TEXT("Inspect dirties nothing: %s"), *FString::Join(Problems, TEXT("; "))), Problems.IsEmpty());
	}
	TestTrue(TEXT("the mesh keeps its physics asset"), Mesh->GetPhysicsAsset() == MeshAssetBefore);

	// 5. Errors at document paths; a failed Create leaves nothing.
	auto Bad = [this, &Fixtures](const TCHAR* Body, const TCHAR* Code, const TCHAR* Path, const TCHAR* Stage, const TCHAR* Properties = TEXT("{}"),
		const FString& FactoryContext = FString(TutorialMesh))
	{
		const FTarget& Target = Fixtures.Add(TEXT("PA_Bad"));
		ExpectCreateFailure(*this, Target, CreateDocument(Target, FactoryContext, Properties, Body), Code, Path, Stage);
	};
	Bad(TEXT(R"({"Bodies": {"pelvis": {"Fit": {}}, "no_such_bone": {"Shapes": {"Spheres": [{"Radius": 3}]}}}})"), TEXT("PhysicsAsset.BoneNotFound"),
		TEXT("/Body/Bodies/no_such_bone"), TEXT("unknown bone"));
	Bad(TEXT(R"({"Bodies": {"pelvis": {"Fit": {}}}, "Constraints": {"pelvis": {"Child": "pelvis", "Parent": "head"}}})"), TEXT("PhysicsAsset.BodyNotFound"),
		TEXT("/Body/Constraints/pelvis/Parent"), TEXT("constraint to a missing body"));
	Bad(TEXT(R"({"Bodies": {"pelvis": {"Properties": {"PhysicsType": "PhysType_Simulated"}, "Shapes": {}}}})"), TEXT("PhysicsAsset.ValidationFailed"),
		TEXT("/Body/Bodies/pelvis"), TEXT("empty Shapes on a simulated body"));
	Bad(TEXT(R"({"Bodies": {"pelvis": {"Fit": {}}}, "CollisionDisabled": [["pelvis", "head"]]})"), TEXT("PhysicsAsset.BodyNotFound"),
		TEXT("/Body/CollisionDisabled/0/1"), TEXT("collision pair with a missing body"));
	Bad(TEXT(R"({"Bodies": {"pelvis": {"Fit": {}, "PhysicalAnimation": {"HitReact": {}}}}})"), TEXT("PhysicsAsset.UnknownProfile"),
		TEXT("/Body/Bodies/pelvis/PhysicalAnimation/HitReact"), TEXT("profile entry for an undeclared profile"));
	Bad(TEXT(R"({"Generation": {"LodIndex": 99}})"), TEXT("PhysicsAsset.InvalidValue"), TEXT("/Body/Generation/LodIndex"), TEXT("bad Generation setting"));
	Bad(TEXT(R"({"Bodies": {"pelvis": {"Fit": {}}}})"), TEXT("PhysicsAsset.MeshRequired"), TEXT("/FactoryContext"), TEXT("no mesh"), TEXT("{}"), FString());
	Bad(TEXT("{}"), TEXT("PhysicsAsset.UnknownMesh"), TEXT("/FactoryContext"), TEXT("unknown mesh"), TEXT("{}"),
		FString(TEXT("/Game/NoSuchFolder/SKM_None.SKM_None")));
	return true;
}

#endif
