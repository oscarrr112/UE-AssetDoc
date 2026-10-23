// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// UEAssetDoc.PhysicsAsset.Read: the tutorial mannequin's physics asset (duplicate) and assets generated
//   as the editor's Create Physics Asset does (capsules; single convex hulls): Extract / Inspect / Diff through the
//   Python Profile, stable twice and after reload, no side effects, the document shape, Inspect facts.
// UEAssetDoc.PhysicsAsset.Corpus: design §6.1 (template packs copied in, optional plugins): extract
//   stable twice and after reload, Diff empty, Inspect works, reading dirties nothing; per-asset timings.
// UEAssetDoc.PhysicsAsset.Status: physics.status.v1 (design §5.1): Unknown without a preview mesh,
//   errors and warnings at document paths.

#include "AssetDocumentPhysicsTestSupport.h"

#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentPhysicsTests
{
// Extract twice, Diff, Inspect, reload, extract again: the problems found (empty = stable).
TArray<FString> CheckStable(FAutomationTestBase& Test, const FTarget& Target, const FString& Name, FString& OutTimings,
	TSharedPtr<FJsonObject>& OutExtracted)
{
	TArray<FString> Problems;
	const FReadSideEffects SideEffects;
	double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult First = ExtractResult(Target.ObjectPath);
	const double ExtractMs = Milliseconds(Start);
	if (!First.IsSuccess() || !First.Payload.IsValid())
	{
		Problems.Add(FString::Printf(TEXT("extract failed: %s"), *DiagnosticsText(First)));
		return Problems;
	}
	OutExtracted = First.Payload;
	const FString Baseline = Serialize(First.Payload);
	const FString Again = Serialize(Extract(Test, Target.ObjectPath, Name + TEXT(" again")));
	if (Again != Baseline)
	{
		Problems.Add(FString::Printf(TEXT("second extraction differs %s"), *FirstDifference(Baseline, Again)));
	}
	Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Inspected = Inspect(Target.ObjectPath);
	const double InspectMs = Milliseconds(Start);
	if (!Inspected.IsSuccess())
	{
		Problems.Add(FString::Printf(TEXT("inspect failed: %s"), *DiagnosticsText(Inspected)));
	}
	FString DiffText;
	const int32 Changes = DiffCount(First.Payload, DiffText);
	if (Changes != 0)
	{
		Problems.Add(FString::Printf(TEXT("Diff of the extraction reports %d changes: %s"), Changes, *DiffText.Left(600)));
	}
	SideEffects.Check(Problems);
	FString Status = StatusName(First);
	const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
	if (First.Compile.IsValid() && First.Compile->TryGetArrayField(TEXT("Messages"), Messages))
	{
		Status += FString::Printf(TEXT(" (%d messages)"), Messages->Num());
	}
	if (Reload(Target) == nullptr)
	{
		Problems.Add(TEXT("reload failed"));
	}
	else
	{
		const FString Reloaded = Serialize(Extract(Test, Target.ObjectPath, Name + TEXT(" after reload")));
		if (Reloaded != Baseline)
		{
			Problems.Add(FString::Printf(TEXT("extraction after reload differs %s"), *FirstDifference(Baseline, Reloaded)));
		}
	}
	OutTimings = FString::Printf(TEXT("extract %.0f ms, inspect %.0f ms, %d chars, status %s"), ExtractMs, InspectMs, Baseline.Len(), *Status);
	return Problems;
}

int32 CountShapes(const TSharedPtr<FJsonObject>& Body, const TCHAR* Kind)
{
	int32 Count = 0;
	const TSharedPtr<FJsonObject>* Bodies = nullptr;
	if (!Body->TryGetObjectField(TEXT("Bodies"), Bodies))
	{
		return 0;
	}
	for (const auto& Entry : (*Bodies)->Values)
	{
		const TSharedPtr<FJsonObject>* Shapes = nullptr;
		if (Entry.Value->AsObject()->TryGetObjectField(TEXT("Shapes"), Shapes))
		{
			Count += CountField(*Shapes, Kind);
		}
	}
	return Count;
}

FString Counts(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonObject> Body = BodyOf(Document);
	return FString::Printf(TEXT("bodies %d, constraints %d, disabled pairs %d, shapes sphere %d / box %d / capsule %d / tapered %d / convex %d"),
		CountField(Body, TEXT("Bodies")), CountField(Body, TEXT("Constraints")), CountField(Body, TEXT("CollisionDisabled")),
		CountShapes(Body, TEXT("Spheres")), CountShapes(Body, TEXT("Boxes")), CountShapes(Body, TEXT("Capsules")),
		CountShapes(Body, TEXT("TaperedCapsules")), CountShapes(Body, TEXT("Convexes")));
}

TArray<FString> StatusPaths(const FAssetDocumentResult& Result, const TCHAR* Severity)
{
	TArray<FString> Paths;
	const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
	if (Result.Compile.IsValid() && Result.Compile->TryGetArrayField(TEXT("Messages"), Messages))
	{
		for (const TSharedPtr<FJsonValue>& Message : *Messages)
		{
			if (Message->AsObject()->GetStringField(TEXT("Severity")) == Severity)
			{
				Paths.Add(Message->AsObject()->GetStringField(TEXT("Path")));
			}
		}
	}
	return Paths;
}
} // namespace AssetDocumentPhysicsTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPhysicsReadTest, "UEAssetDoc.PhysicsAsset.Read",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPhysicsReadTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPhysicsTests;
	FFixtures Fixtures;
	UObject* Tutorial = LoadQuiet(TutorialPhysicsAsset);
	if (!TestNotNull(TEXT("tutorial physics asset"), Tutorial))
	{
		return false;
	}

	// Inspect of the engine asset itself (read only): the mesh that uses it is listed.
	const FAssetDocumentResult EngineInspect = Inspect(TutorialPhysicsAsset);
	if (TestTrue(*FString::Printf(TEXT("inspect the engine asset: %s"), *DiagnosticsText(EngineInspect)), EngineInspect.IsSuccess() && EngineInspect.Payload.IsValid()))
	{
		const TArray<TSharedPtr<FJsonValue>>* Users = nullptr;
		bool bMeshListed = false;
		if (EngineInspect.Payload->TryGetArrayField(TEXT("MeshUsers"), Users))
		{
			for (const TSharedPtr<FJsonValue>& User : *Users)
			{
				bool bUses = false;
				bMeshListed |= User->AsObject()->GetStringField(TEXT("Path")) == TutorialMesh
					&& (!User->AsObject()->TryGetBoolField(TEXT("UsesAsset"), bUses) || bUses);
			}
		}
		TestTrue(TEXT("Inspect lists TutorialTPP as a mesh using the asset"), bMeshListed);
		const TArray<TSharedPtr<FJsonValue>>* Skeleton = nullptr;
		TestTrue(TEXT("Inspect lists the skeleton"), EngineInspect.Payload->TryGetArrayField(TEXT("Skeleton"), Skeleton) && Skeleton->Num() > 50);
		TestTrue(TEXT("Inspect has body facts"), EngineInspect.Payload->HasTypedField<EJson::Object>(TEXT("BodyFacts")));
		TestTrue(TEXT("Inspect has a status"), EngineInspect.Payload->HasTypedField<EJson::Object>(TEXT("Status")));
	}

	TArray<TPair<FString, const FTarget*>> Sources;
	const FTarget& Copy = Fixtures.Add(TEXT("PA_Tutorial"));
	if (TestNotNull(TEXT("duplicate tutorial"), Duplicate(*this, Tutorial, Copy)))
	{
		Sources.Emplace(TEXT("TutorialTPP_PhysicsAsset (duplicate)"), &Copy);
	}
	const FTarget& Capsules = Fixtures.Add(TEXT("PA_Generated"));
	if (Generate(*this, Capsules, TutorialMesh, FPhysAssetCreateParams()) != nullptr)
	{
		Sources.Emplace(TEXT("generated (default settings: capsules)"), &Capsules);
	}
	const FTarget& Convexes = Fixtures.Add(TEXT("PA_Convex"));
	FPhysAssetCreateParams ConvexParams;
	ConvexParams.GeomType = EFG_SingleConvexHull;
	if (Generate(*this, Convexes, TutorialMesh, ConvexParams) != nullptr)
	{
		Sources.Emplace(TEXT("generated (single convex hulls)"), &Convexes);
	}
	for (const TPair<FString, const FTarget*>& Source : Sources)
	{
		FString Timings;
		TSharedPtr<FJsonObject> Extracted;
		for (const FString& Problem : CheckStable(*this, *Source.Value, Source.Key, Timings, Extracted))
		{
			AddError(FString::Printf(TEXT("%s: %s"), *Source.Key, *Problem));
		}
		AddInfo(FString::Printf(TEXT("%s: %s; %s"), *Source.Key, *Timings, *Counts(Extracted)));
		if (!Extracted.IsValid())
		{
			continue;
		}
		TestEqual(TEXT("Class"), Extracted->GetStringField(TEXT("Class")), FString(PhysicsAssetClass));
		const TSharedPtr<FJsonObject> Body = BodyOf(Extracted);
		TestEqual(*FString::Printf(TEXT("%s: preview mesh"), *Source.Key), Body->GetStringField(TEXT("PreviewMesh")), FString(TutorialMesh));
		TestTrue(*FString::Printf(TEXT("%s: bodies"), *Source.Key), CountField(Body, TEXT("Bodies")) >= 10);
		TestTrue(*FString::Printf(TEXT("%s: constraints"), *Source.Key), CountField(Body, TEXT("Constraints")) >= 9);
		TestTrue(*FString::Printf(TEXT("%s: collision table"), *Source.Key), CountField(Body, TEXT("CollisionDisabled")) >= 1);
		TestFalse(*FString::Printf(TEXT("%s: Profile lists are Properties"), *Source.Key),
			PropertiesOf(Extracted)->HasField(TEXT("SkeletalBodySetups")) || PropertiesOf(Extracted)->HasField(TEXT("ConstraintSetup")));
		// Every body carries its shapes; auto names are omitted; the generated assets' constraints sit on the snapped
		// frames (no Frames key).
		for (const auto& Entry : Body->GetObjectField(TEXT("Bodies"))->Values)
		{
			TestTrue(*FString::Printf(TEXT("%s: %s has shapes"), *Source.Key, *Entry.Key), Entry.Value->AsObject()->HasTypedField<EJson::Object>(TEXT("Shapes")));
		}
		if (Source.Value != &Copy)
		{
			const FString Text = Serialize(Body);
			TestFalse(*FString::Printf(TEXT("%s: auto shape names omitted"), *Source.Key), Text.Contains(TEXT("\"Name\":")));
			TestFalse(*FString::Printf(TEXT("%s: snapped frames omitted"), *Source.Key), Text.Contains(TEXT("\"Frames\":")));
		}
		if (Source.Value == &Convexes)
		{
			TestTrue(TEXT("convex hulls extracted"), CountShapes(Body, TEXT("Convexes")) >= 10);
			TestTrue(TEXT("convex hulls carry their points"), Serialize(Body).Contains(TEXT("\"VertexData\":[{")));
		}
		AddInfo(SerializePretty(Extracted).Left(6000));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPhysicsCorpusTest, "UEAssetDoc.PhysicsAsset.Corpus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPhysicsCorpusTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPhysicsTests;
	// MoverExamples' PA_Mannequin names /Game/Characters/Mannequins/Meshes/SKM_Manny as its preview mesh, which the
	// plugin does not ship (the template pack has SKM_Manny_Simple).
	AddExpectedMessagePlain(TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	const FTemplatePacks Packs = CorpusPacks();
	int32 Tried = 0;
	for (const FCorpusEntry& Entry : Corpus)
	{
		const FString Name = CorpusName(Entry);
		UPhysicsAsset* Source = LoadCorpus(Entry);
		if (Source == nullptr)
		{
			AddInfo(FString::Printf(TEXT("SKIP %s: %s is not installed / enabled in this project"), *Name, Entry.Source));
			continue;
		}
		++Tried;
		FFixtures Fixtures;
		const FTarget& Target = Fixtures.Add(TEXT("PA_Corpus"));
		if (Duplicate(*this, Source, Target) == nullptr)
		{
			AddError(FString::Printf(TEXT("%s: duplicate failed"), *Name));
			continue;
		}
		FString Timings;
		TSharedPtr<FJsonObject> Extracted;
		const TArray<FString> Problems = CheckStable(*this, Target, Name, Timings, Extracted);
		for (const FString& Problem : Problems)
		{
			AddError(FString::Printf(TEXT("%s: %s"), *Name, *Problem));
		}
		AddInfo(FString::Printf(TEXT("%s %s (%s): %s; %s"), Problems.IsEmpty() ? TEXT("PASS") : TEXT("FAIL"), *Name, Entry.Why, *Timings,
			*Counts(Extracted)));
	}
	TestTrue(TEXT("the engine's tutorial asset is always read"), Tried >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPhysicsStatusTest, "UEAssetDoc.PhysicsAsset.Status",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPhysicsStatusTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentPhysicsTests;
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("PA_Status"));
	if (Generate(*this, Target, TutorialMesh, FPhysAssetCreateParams()) == nullptr)
	{
		return false;
	}
	const FAssetDocumentResult Clean = ExtractResult(Target.ObjectPath);
	TestTrue(*FString::Printf(TEXT("a generated asset is up to date (%s)"), *StatusOf(Clean)), StatusName(Clean).StartsWith(TEXT("UpToDate")));

	// Broken states the editor can leave (each made with the engine calls; nothing is saved).
	UPhysicsAsset* Asset = Target.GetAsset();
	USkeletalBodySetup* Body = Asset->SkeletalBodySetups.Last();
	const FString Bone = Body->BoneName.ToString();
	Body->AggGeom.EmptyElements();
	UPhysicsConstraintTemplate* Constraint = Asset->ConstraintSetup[0];
	Constraint->DefaultInstance.ConstraintBone2 = TEXT("no_such_body");
	Constraint->AddConstraintProfile(TEXT("Unlisted"));
	const FString Joint = Constraint->DefaultInstance.JointName.ToString();
	const FAssetDocumentResult Broken = ExtractResult(Target.ObjectPath);
	TestEqual(TEXT("broken: Error"), StatusName(Broken), FString(TEXT("Error")));
	const TArray<FString> Errors = StatusPaths(Broken, TEXT("Error"));
	TestTrue(*FString::Printf(TEXT("no shapes at the body's Shapes (%s)"), *FString::Join(Errors, TEXT(", "))),
		Errors.Contains(FString::Printf(TEXT("/Body/Bodies/%s/Shapes"), *Bone)));
	TestTrue(TEXT("missing body at the constraint's Parent"), Errors.Contains(FString::Printf(TEXT("/Body/Constraints/%s/Parent"), *Joint)));
	TestTrue(TEXT("unlisted profile at the constraint's Profiles"), Errors.Contains(FString::Printf(TEXT("/Body/Constraints/%s/Profiles/Unlisted"), *Joint)));

	// Without a preview mesh: Unknown (bone checks skipped) unless something else is an error.
	const FTarget& NoMesh = Fixtures.Add(TEXT("PA_NoMesh"));
	if (Generate(*this, NoMesh, TutorialMesh, FPhysAssetCreateParams()) != nullptr)
	{
		NoMesh.GetAsset()->PreviewSkeletalMesh = nullptr;
		const FAssetDocumentResult Unknown = ExtractResult(NoMesh.ObjectPath);
		TestEqual(TEXT("no preview mesh: Unknown"), StatusName(Unknown), FString(TEXT("Unknown")));
		TestTrue(TEXT("warning at /Body/PreviewMesh"), StatusPaths(Unknown, TEXT("Warning")).Contains(TEXT("/Body/PreviewMesh")));
		NoMesh.GetAsset()->GetOutermost()->ClearDirtyFlag();
	}
	Target.GetAsset()->GetOutermost()->ClearDirtyFlag();
	return true;
}

#endif
