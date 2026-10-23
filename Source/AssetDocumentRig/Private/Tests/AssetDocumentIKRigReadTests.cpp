// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// UEAssetDoc.IKRig.Read: an editor-built IK Rig (UE4 tutorial mannequin, Auto Characterize +
//   Auto FBIK) and a retargeter between two such rigs: Extract / Inspect / Diff through the Python Profiles, stable
//   twice and after reload, no side effects, status.
// UEAssetDoc.IKRig.Corpus: the shipped IK content (design §6.1) duplicated into /Game: extract
//   stable twice and after reload, Diff empty, Inspect works, reading dirties nothing; per-asset timings.
// UEAssetDoc.IKRig.ListTypes: list_types kinds ikrig_solver / ikretarget_op.
// UEAssetDoc.IKRig.Status: the processor status (design §5): Unknown without a mesh, warnings at
//   document paths, the retargeter's asset ops untouched by its status.

#include "AssetDocumentIKRigTestSupport.h"

#include "Retargeter/IKRetargetOps.h"
#include "StructUtils/InstancedStruct.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentIKRigTests
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
	FString Status = TEXT("-");
	if (First.Compile.IsValid())
	{
		Status = First.Compile->GetStringField(TEXT("Status"));
		const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
		if (First.Compile->TryGetArrayField(TEXT("Messages"), Messages))
		{
			Status += FString::Printf(TEXT(" (%d messages)"), Messages->Num());
		}
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
	OutTimings = FString::Printf(TEXT("extract %.0f ms, inspect %.0f ms, %d chars, status %s"), ExtractMs, InspectMs,
		Baseline.Len(), *Status);
	return Problems;
}
} // namespace AssetDocumentIKRigTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRigReadTest, "UEAssetDoc.IKRig.Read",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRigReadTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRigTests;
	FFixtures Fixtures;
	const FTarget& RigTarget = Fixtures.Add(TEXT("IK_Tutorial"));
	const FTarget& RetargeterTarget = Fixtures.Add(TEXT("RTG_Tutorial"));
	UIKRigDefinition* Rig = MakeRig(*this, RigTarget, TutorialMesh, true);
	if (Rig == nullptr)
	{
		return false;
	}
	UIKRetargeter* Retargeter = MakeRetargeter(*this, RetargeterTarget, Rig, Rig);
	if (Retargeter == nullptr)
	{
		return false;
	}

	// IK Rig.
	FString Timings;
	TSharedPtr<FJsonObject> Extracted;
	for (const FString& Problem : CheckStable(*this, RigTarget, TEXT("IK_Tutorial"), Timings, Extracted))
	{
		AddError(FString::Printf(TEXT("IK_Tutorial: %s"), *Problem));
	}
	AddInfo(FString::Printf(TEXT("IK_Tutorial: %s"), *Timings));
	AddInfo(SerializePretty(Extracted));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("Class"), Extracted->GetStringField(TEXT("Class")), FString(IKRigClass));
	const TSharedPtr<FJsonObject> Body = BodyOf(Extracted);
	TestEqual(TEXT("preview mesh"), Body->GetStringField(TEXT("PreviewMesh")), FString(TutorialMesh));
	TestTrue(TEXT("auto characterize made chains"), CountField(Body, TEXT("Chains")) >= 10);
	TestTrue(TEXT("auto FBIK made goals"), CountField(Body, TEXT("Goals")) >= 4);
	TestTrue(TEXT("auto FBIK made a solver"), CountField(Body, TEXT("Solvers")) >= 1);
	TestEqual(TEXT("retarget root"), Body->GetStringField(TEXT("RetargetRoot")), FString(TEXT("pelvis")));
	const TArray<TSharedPtr<FJsonValue>>* Solvers = nullptr;
	if (Body->TryGetArrayField(TEXT("Solvers"), Solvers) && !Solvers->IsEmpty())
	{
		const TSharedPtr<FJsonObject> Solver = (*Solvers)[0]->AsObject();
		TestEqual(TEXT("FBIK short type name"), Solver->GetStringField(TEXT("Type")), FString(TEXT("FullBodyIK")));
		TestTrue(TEXT("FBIK goals"), CountField(Solver, TEXT("Goals")) >= 4);
	}
	const FAssetDocumentResult Inspected = Inspect(RigTarget.ObjectPath);
	if (TestTrue(TEXT("inspect"), Inspected.IsSuccess() && Inspected.Payload.IsValid()))
	{
		const TArray<TSharedPtr<FJsonValue>>* Skeleton = nullptr;
		TestTrue(TEXT("inspect lists the skeleton"), Inspected.Payload->TryGetArrayField(TEXT("Skeleton"), Skeleton) && Skeleton->Num() > 50);
		TestTrue(TEXT("inspect lists chain bones"), Inspected.Payload->HasTypedField<EJson::Object>(TEXT("ChainBones")));
		TestTrue(TEXT("inspect has a status"), Inspected.Payload->HasTypedField<EJson::Object>(TEXT("Compile")));
		TestTrue(TEXT("inspect suggests the auto setup"), Inspected.Payload->HasTypedField<EJson::Object>(TEXT("Suggestions")));
	}

	// Retargeter.
	TSharedPtr<FJsonObject> RetargeterDocument;
	for (const FString& Problem : CheckStable(*this, RetargeterTarget, TEXT("RTG_Tutorial"), Timings, RetargeterDocument))
	{
		AddError(FString::Printf(TEXT("RTG_Tutorial: %s"), *Problem));
	}
	AddInfo(FString::Printf(TEXT("RTG_Tutorial: %s"), *Timings));
	AddInfo(SerializePretty(RetargeterDocument));
	if (RetargeterDocument.IsValid())
	{
		const TSharedPtr<FJsonObject> RetargeterBody = BodyOf(RetargeterDocument);
		TestEqual(TEXT("source rig"), RetargeterBody->GetObjectField(TEXT("Source"))->GetStringField(TEXT("IKRig")), RigTarget.ObjectPath);
		TestTrue(TEXT("default op stack"), CountField(RetargeterBody, TEXT("Ops")) >= 4);
		bool bRunIKChild = false;
		bool bMapping = false;
		for (const TSharedPtr<FJsonValue>& Op : RetargeterBody->GetArrayField(TEXT("Ops")))
		{
			bRunIKChild |= Op->AsObject()->GetStringField(TEXT("Type")) == TEXT("RunIKRig") && CountField(Op->AsObject(), TEXT("Children")) >= 1;
			bMapping |= Op->AsObject()->HasTypedField<EJson::Object>(TEXT("ChainMapping"));
		}
		TestTrue(TEXT("Run IK Rig carries its child ops"), bRunIKChild);
		TestTrue(TEXT("chain mappings are extracted"), bMapping);
		TestTrue(TEXT("poses"), RetargeterBody->HasTypedField<EJson::Object>(TEXT("Poses")));
	}
	const FAssetDocumentResult InspectedRetargeter = Inspect(RetargeterTarget.ObjectPath);
	if (TestTrue(TEXT("inspect retargeter"), InspectedRetargeter.IsSuccess() && InspectedRetargeter.Payload.IsValid()))
	{
		TestTrue(TEXT("profiles listed (read only)"), InspectedRetargeter.Payload->HasTypedField<EJson::Object>(TEXT("Profiles")));
		TestTrue(TEXT("op facts"), InspectedRetargeter.Payload->HasTypedField<EJson::Array>(TEXT("OpFacts")));
		TestTrue(TEXT("inspect suggests auto align / auto map"), InspectedRetargeter.Payload->HasTypedField<EJson::Object>(TEXT("Suggestions")));
	}

	// Unchanged documents apply as no-ops; a retargeter change applies (phase R2).
	if (RetargeterDocument.IsValid())
	{
		const TSharedPtr<FJsonObject> Same = Extract(*this, RetargeterTarget.ObjectPath, TEXT("before no-op apply"));
		const FAssetDocumentResult Unchanged = Apply(RetargeterTarget, Same);
		TestTrue(*FString::Printf(TEXT("unchanged retargeter document applies: %s"), *DiagnosticsText(Unchanged)), Unchanged.IsSuccess());
		TestFalse(TEXT("unchanged retargeter document is a no-op"), Unchanged.bSavedAsset);
		const TSharedPtr<FJsonObject> Changed = Parse(Serialize(Same));
		Changed->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Ops"))[0]->AsObject()->SetBoolField(TEXT("Enabled"), false);
		const FAssetDocumentResult Edited = Apply(RetargeterTarget, Changed);
		TestTrue(*FString::Printf(TEXT("a retargeter change applies: %s"), *DiagnosticsText(Edited)), Edited.IsSuccess());
		const TSharedPtr<FJsonObject> After = Extract(*this, RetargeterTarget.ObjectPath, TEXT("after edit"));
		TestTrue(TEXT("the op is disabled"), After.IsValid()
			&& !BodyOf(After)->GetArrayField(TEXT("Ops"))[0]->AsObject()->GetBoolField(TEXT("Enabled")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRigCorpusTest, "UEAssetDoc.IKRig.Corpus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRigCorpusTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRigTests;
	int32 Tried = 0;
	// MoverExamples' RTG_Mannequin references /Game/Characters/Mannequins/Rigs/IK_Mannequin, which the plugin does
	// not ship (its rigs stay unset; the status is Unknown).
	AddExpectedMessagePlain(TEXT("a dependent package /Game/Characters/Mannequins/Rigs/IK_Mannequin was not available"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, -1);
	for (const FCorpusEntry& Entry : Corpus)
	{
		const FString Path = Entry.Path;
		const FString Name = FString(Entry.Plugin) + TEXT(":") + FPackageName::ObjectPathToObjectName(Path);
		// A plugin that is not enabled has no mount point: skip without loading (no LoadPackage or DoesPackageExist warnings).
		const FString Package = FPackageName::ObjectPathToPackageName(Path);
		UObject* Source = FPackageName::IsValidLongPackageName(Package) && FPackageName::DoesPackageExist(Package) ? LoadQuiet(Path) : nullptr;
		if (Source == nullptr)
		{
			AddInfo(FString::Printf(TEXT("SKIP %s: the %s plugin is not enabled in this project"), *Name, Entry.Plugin));
			continue;
		}
		++Tried;
		FFixtures Fixtures;
		const FTarget& Target = Fixtures.Add(Source->IsA<UIKRetargeter>() ? TEXT("RTG_Corpus") : TEXT("IK_Corpus"));
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
		const TSharedPtr<FJsonObject> Body = BodyOf(Extracted);
		AddInfo(FString::Printf(TEXT("%s %s (%s): %s; goals %d, solvers %d, chains %d, ops %d"),
			Problems.IsEmpty() ? TEXT("PASS") : TEXT("FAIL"), *Name, Entry.Why, *Timings, CountField(Body, TEXT("Goals")),
			CountField(Body, TEXT("Solvers")), CountField(Body, TEXT("Chains")), CountField(Body, TEXT("Ops"))));
	}
	if (Tried == 0)
	{
		AddInfo(TEXT("no corpus plugin is enabled (MoverExamples, MoverTests, MetaHumanCharacter); covered by the editor-built fixtures of IKRig.Read"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRigListTypesTest, "UEAssetDoc.IKRig.ListTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRigListTypesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRigTests;
	const FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	auto List = [&](const TMap<FString, FString>& Query, const TCHAR* Stage) -> TArray<TSharedPtr<FJsonValue>>
	{
		const FAssetDocumentResult Result = Service.ListTypes(Query);
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!TestTrue(*FString::Printf(TEXT("%s: lists"), Stage), Result.IsSuccess() && Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("Types"), Rows)))
		{
			AddInfo(DiagnosticsText(Result));
			return {};
		}
		return *Rows;
	};
	auto Find = [](const TArray<TSharedPtr<FJsonValue>>& Rows, const TCHAR* Name) -> TSharedPtr<FJsonObject>
	{
		for (const TSharedPtr<FJsonValue>& Row : Rows)
		{
			if (Row->AsObject()->GetStringField(TEXT("Name")) == Name)
			{
				return Row->AsObject();
			}
		}
		return nullptr;
	};
	const TArray<TSharedPtr<FJsonValue>> Solvers = List({{TEXT("kind"), TEXT("ikrig_solver")}}, TEXT("solvers"));
	for (const TCHAR* Name : {TEXT("FullBodyIK"), TEXT("LimbIK"), TEXT("Pole"), TEXT("BodyMover"), TEXT("SetTransform"), TEXT("StretchLimb")})
	{
		TestNotNull(*FString::Printf(TEXT("solver %s listed"), Name), Find(Solvers, Name).Get());
	}
	if (const TSharedPtr<FJsonObject> FBIK = Find(Solvers, TEXT("FullBodyIK")))
	{
		AddInfo(Serialize(FBIK));
		TestEqual(TEXT("FBIK path"), FBIK->GetStringField(TEXT("Path")), FString(TEXT("/Script/IKRig.IKRigFullBodyIKSolver")));
		TestTrue(TEXT("FBIK uses a start bone"), FBIK->GetBoolField(TEXT("UsesStartBone")));
		TestTrue(TEXT("FBIK settings with defaults"), FBIK->HasTypedField<EJson::Array>(TEXT("Settings")) && FBIK->GetArrayField(TEXT("Settings")).Num() > 3);
		TestTrue(TEXT("FBIK goal settings"), FBIK->HasTypedField<EJson::Array>(TEXT("GoalSettings")));
		TestTrue(TEXT("FBIK bone settings"), FBIK->HasTypedField<EJson::Array>(TEXT("BoneSettings")));
	}
	const TArray<TSharedPtr<FJsonValue>> Ops = List({{TEXT("kind"), TEXT("ikretarget_op")}}, TEXT("ops"));
	for (const TCHAR* Name : {TEXT("FKChains"), TEXT("RunIKRig"), TEXT("IKChains"), TEXT("PelvisMotion"), TEXT("RootMotion"),
		TEXT("CurveRemap"), TEXT("PinBone"), TEXT("StrideWarping"), TEXT("FloorConstraint")})
	{
		TestNotNull(*FString::Printf(TEXT("op %s listed"), Name), Find(Ops, Name).Get());
	}
	if (const TSharedPtr<FJsonObject> IKChains = Find(Ops, TEXT("IKChains")))
	{
		AddInfo(Serialize(IKChains));
		TestEqual(TEXT("IK Chains is a child of Run IK Rig"), IKChains->GetStringField(TEXT("ParentType")), FString(TEXT("RunIKRig")));
		TestTrue(TEXT("IK Chains has chain settings"), IKChains->HasTypedField<EJson::Array>(TEXT("ChainSettings")));
	}
	if (const TSharedPtr<FJsonObject> FK = Find(Ops, TEXT("FKChains")))
	{
		TestTrue(TEXT("FK Chains has a chain mapping"), FK->GetBoolField(TEXT("ChainMapping")));
	}
	const TArray<TSharedPtr<FJsonValue>> Filtered = List({{TEXT("kind"), TEXT("ikrig_solver")}, {TEXT("name"), TEXT("Limb")}}, TEXT("filtered"));
	TestTrue(TEXT("name filter"), Filtered.Num() >= 2 && Filtered.Num() < Solvers.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRigStatusTest, "UEAssetDoc.IKRig.Status",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRigStatusTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRigTests;
	FFixtures Fixtures;
	const FTarget& EmptyTarget = Fixtures.Add(TEXT("IK_Empty"));
	const FTarget& RigTarget = Fixtures.Add(TEXT("IK_Status"));
	const FTarget& RetargeterTarget = Fixtures.Add(TEXT("RTG_Status"));

	// A rig without a mesh: nothing can be initialized.
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* Empty = AssetTools.CreateAsset(EmptyTarget.AssetName, FPackageName::GetLongPackagePath(EmptyTarget.PackageName),
		UIKRigDefinition::StaticClass(), NewObject<UIKRigDefinitionFactory>());
	if (!TestNotNull(TEXT("empty rig"), Empty))
	{
		return false;
	}
	Save(*Empty);
	const FAssetDocumentResult EmptyResult = ExtractResult(EmptyTarget.ObjectPath);
	TestTrue(TEXT("empty rig extracts"), EmptyResult.IsSuccess());
	TestTrue(TEXT("empty rig status Unknown"), EmptyResult.Compile.IsValid() && EmptyResult.Compile->GetStringField(TEXT("Status")) == TEXT("Unknown"));
	TestEqual(TEXT("empty rig body is empty"), BodyOf(EmptyResult.Payload)->Values.Num(), 0);

	// A goal no solver uses: the processor's warning, attributed to the goal.
	UIKRigDefinition* Rig = MakeRig(*this, RigTarget, TutorialMesh, true);
	if (Rig == nullptr)
	{
		return false;
	}
	UIKRigController* Controller = UIKRigController::GetController(Rig);
	const FName Loose = Controller->AddNewGoal(TEXT("LooseGoal"), TEXT("head"));
	TestEqual(TEXT("goal added"), Loose, FName(TEXT("LooseGoal")));
	Save(*Rig);
	const FAssetDocumentResult Warned = ExtractResult(RigTarget.ObjectPath);
	if (TestTrue(TEXT("rig status"), Warned.Compile.IsValid()))
	{
		AddInfo(Serialize(Warned.Compile));
		TestEqual(TEXT("status with warnings"), Warned.Compile->GetStringField(TEXT("Status")), FString(TEXT("UpToDateWithWarnings")));
		bool bAttributed = false;
		for (const TSharedPtr<FJsonValue>& Message : Warned.Compile->GetArrayField(TEXT("Messages")))
		{
			FString Path;
			bAttributed |= Message->AsObject()->TryGetStringField(TEXT("Path"), Path) && Path == TEXT("/Body/Goals/LooseGoal");
		}
		TestTrue(TEXT("the warning carries the goal's document path"), bAttributed);
	}

	// Retargeter: the status runs the processor on a copy; the asset's ops do not get its editor instances.
	UIKRetargeter* Retargeter = MakeRetargeter(*this, RetargeterTarget, Rig, Rig);
	if (Retargeter == nullptr)
	{
		return false;
	}
	const FReadSideEffects SideEffects;
	TArray<const void*> InstancesBefore;
	for (const FInstancedStruct& Op : Retargeter->GetRetargetOps())
	{
		const FIKRetargetOpBase* Base = Op.GetPtr<FIKRetargetOpBase>();
		InstancesBefore.Add(Base != nullptr ? Base->EditorInstance : nullptr);
	}
	const FAssetDocumentResult RetargeterResult = ExtractResult(RetargeterTarget.ObjectPath);
	if (TestTrue(TEXT("retargeter status"), RetargeterResult.Compile.IsValid()))
	{
		AddInfo(Serialize(RetargeterResult.Compile));
		TestNotEqual(TEXT("retargeter status is known (meshes come from the rigs)"), RetargeterResult.Compile->GetStringField(TEXT("Status")), FString(TEXT("Unknown")));
		TestNotEqual(TEXT("retargeter initializes"), RetargeterResult.Compile->GetStringField(TEXT("Status")), FString(TEXT("Error")));
	}
	TArray<const void*> InstancesAfter;
	for (const FInstancedStruct& Op : Retargeter->GetRetargetOps())
	{
		const FIKRetargetOpBase* Base = Op.GetPtr<FIKRetargetOpBase>();
		InstancesAfter.Add(Base != nullptr ? Base->EditorInstance : nullptr);
	}
	TestTrue(TEXT("the status leaves the asset ops' editor instances unchanged"), InstancesBefore == InstancesAfter);
	TArray<FString> Problems;
	SideEffects.Check(Problems);
	for (const FString& Problem : Problems)
	{
		AddError(Problem);
	}
	return true;
}

#endif
