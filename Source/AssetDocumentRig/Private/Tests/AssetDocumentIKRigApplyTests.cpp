// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// IK Rig apply (design §6, phase R1):
// UEAssetDoc.IKRig.Fidelity: §6.2 on the editor-built rig fixture and the corpus rigs: extract,
//   Create a new rig from the document, extract equal and equal status; unchanged re-apply is a no-op; reload stable.
// UEAssetDoc.IKRig.FreeEdits: §6.3 rig edits made once through the IK Rig controller calls the editor's
//   UI makes and once through a document, on two duplicates: equal state and status.
// UEAssetDoc.IKRig.FromScratch: §6.4 (rig part): an AI-written rig for the tutorial mannequin (chains,
//   FBIK with foot goals and bone limits, a limb solver on a hand goal) is created, initializes without errors and
//   re-applies as a no-op; typical authoring errors fail at their document paths and change nothing; a chain the
//   skeleton cannot resolve comes back as a warning; a chain rename reports the retargeter that references the rig.
// UEAssetDoc.IKRig.Undo: the undo-history rules around a human edit, an AI apply and a failed apply.
// UEAssetDoc.IKRig.StructWrites: a settings write equals the IK Rig editor's struct-wrapper edit.

#include "AssetDocumentIKRigTestSupport.h"

#include "AssetDocumentRigPrimitives.h"
#include "Rig/Solvers/IKRigSolverBase.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ScopedTransaction.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentIKRigApplyTests
{
using namespace AssetDocumentIKRigTests;

TSharedPtr<FJsonObject> ForTarget(const TSharedPtr<FJsonObject>& Document, const FTarget& Target, const TCHAR* Action)
{
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Document));
	Copy->SetStringField(TEXT("Target"), Target.PackageName);
	Copy->SetStringField(TEXT("Action"), Action);
	return Copy;
}

TSharedPtr<FJsonObject> RigDocument(const FTarget& Target, const TCHAR* Action, const FString& Body)
{
	return Parse(FString::Printf(TEXT(R"({"SchemaVersion": 1, "Class": "%s", "Action": "%s", "Target": "%s", "Body": %s})"),
		IKRigClass, Action, *Target.PackageName, *Body));
}

FString WarningsText(const FAssetDocumentResult& Result)
{
	return DiagnosticsText(Result);
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

FString StatusOf(const FAssetDocumentResult& Extracted)
{
	if (!Extracted.Compile.IsValid())
	{
		return TEXT("-");
	}
	return Serialize(Extracted.Compile);
}

// Refusal at a document path that changes nothing: the existing rig extracts as before and its package is clean.
void ExpectRefused(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const TCHAR* Code,
	const FString& Path, const FString& Stage)
{
	const FString Before = Serialize(ExtractResult(Target.ObjectPath).Payload);
	const FAssetDocumentResult Result = Apply(Target, Document);
	Test.TestFalse(*FString::Printf(TEXT("%s is refused"), *Stage), Result.IsSuccess());
	Test.TestTrue(*FString::Printf(TEXT("%s: %s (got %s)"), *Stage, Code, *DiagnosticsText(Result)), HasCode(Result, Code));
	Test.TestEqual(*FString::Printf(TEXT("%s: path"), *Stage), FirstPath(Result), Path);
	const FString After = Serialize(ExtractResult(Target.ObjectPath).Payload);
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: the rig is unchanged"), *Stage), After, Before))
	{
		Test.AddInfo(FirstDifference(Before, After));
	}
	if (const UObject* Asset = Target.Get())
	{
		Test.TestFalse(*FString::Printf(TEXT("%s: package clean"), *Stage), Asset->GetOutermost()->IsDirty());
	}
}

const UScriptStruct* SolverType(const TCHAR* Name)
{
	return UE::AssetDocument::Graph::Rig::FindSolverType(Name);
}

// A field of a struct instance set from its text form (as a Details edit types it).
bool SetField(FAutomationTestBase& Test, const UScriptStruct* Type, void* Memory, const TCHAR* Name, const TCHAR* Text)
{
	const FProperty* Property = Type != nullptr ? Type->FindPropertyByName(Name) : nullptr;
	const bool bSet = Property != nullptr && Memory != nullptr
		&& Property->ImportText_Direct(Text, Property->ContainerPtrToValuePtr<void>(Memory), nullptr, PPF_None) != nullptr;
	Test.TestTrue(*FString::Printf(TEXT("%s.%s = %s"), Type != nullptr ? *Type->GetName() : TEXT("?"), Name, Text), bSet);
	return bSet;
}

int32 SolverIndexOf(const UIKRigDefinition& Rig, const UScriptStruct* Type)
{
	const TArray<FInstancedStruct>& Solvers = Rig.GetSolverStructs();
	for (int32 Index = 0; Index < Solvers.Num(); ++Index)
	{
		if (Solvers[Index].GetScriptStruct() == Type)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

const TCHAR* const ScratchBody = TEXT(R"({
	"PreviewMesh": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP",
	"Goals": {
		"LeftFootIK": {"Bone": "foot_l"},
		"RightFootIK": {"Bone": "foot_r"},
		"RightHandIK": {"Bone": "hand_r", "RotationAlpha": 0.5, "Offset": {"Location": {"X": 0, "Y": 0, "Z": 10}}}},
	"Solvers": [
		{"Type": "FullBodyIK", "StartBone": "pelvis", "Settings": {"Iterations": 30},
		 "Goals": {"LeftFootIK": {}, "RightFootIK": {"PullChainAlpha": 0.5}},
		 "Bones": {"calf_l": {"X": "Limited", "MinX": -10, "MaxX": 10}, "calf_r": {}}},
		{"Type": "LimbIK", "StartBone": "upperarm_r", "Goals": {"RightHandIK": {}}}],
	"RetargetRoot": "pelvis",
	"Chains": {
		"Spine": {"Start": "spine_01", "End": "spine_03"},
		"LeftLeg": {"Start": "thigh_l", "End": "foot_l", "Goal": "LeftFootIK"},
		"RightLeg": {"Start": "thigh_r", "End": "foot_r", "Goal": "RightFootIK"},
		"RightArm": {"Start": "upperarm_r", "End": "hand_r", "Goal": "RightHandIK"}}})");
} // namespace AssetDocumentIKRigApplyTests

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRigFidelityTest, "UEAssetDoc.IKRig.Fidelity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRigFidelityTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRigApplyTests;
	FFixtures Fixtures;
	TArray<TPair<FString, const FTarget*>> Sources;
	const FTarget& Tutorial = Fixtures.Add(TEXT("IK_Tutorial"));
	if (MakeRig(*this, Tutorial, TutorialMesh, true) != nullptr)
	{
		Sources.Emplace(TEXT("fixture IK_Tutorial (Auto Characterize + Auto FBIK)"), &Tutorial);
	}
	const FTarget& Plain = Fixtures.Add(TEXT("IK_Plain"));
	if (MakeRig(*this, Plain, TutorialMesh, false) != nullptr)
	{
		Sources.Emplace(TEXT("fixture IK_Plain (New IK Rig on a mesh)"), &Plain);
	}
	for (const FCorpusEntry& Entry : Corpus)
	{
		const FString Path = Entry.Path;
		// A plugin that is not enabled has no mount point (DoesPackageExist warns on a path outside every mount point).
		const FString Package = FPackageName::ObjectPathToPackageName(Path);
		if (!FPackageName::IsValidLongPackageName(Package) || !FPackageName::DoesPackageExist(Package))
		{
			AddInfo(FString::Printf(TEXT("SKIP %s:%s: the plugin is not enabled in this project"), Entry.Plugin, *FPackageName::ObjectPathToObjectName(Path)));
			continue;
		}
		UObject* Source = LoadQuiet(Path);
		if (Source == nullptr || !Source->IsA<UIKRigDefinition>())
		{
			continue;
		}
		const FTarget& Copy = Fixtures.Add(TEXT("IK_Corpus"));
		if (Duplicate(*this, Source, Copy) != nullptr)
		{
			Sources.Emplace(FString(Entry.Plugin) + TEXT(":") + FPackageName::ObjectPathToObjectName(Path), &Copy);
		}
	}
	for (const TPair<FString, const FTarget*>& Source : Sources)
	{
		const FString& Name = Source.Key;
		const FAssetDocumentResult Original = ExtractResult(Source.Value->ObjectPath);
		if (!TestTrue(*FString::Printf(TEXT("%s extracts"), *Name), Original.IsSuccess() && Original.Payload.IsValid()))
		{
			continue;
		}
		const FTarget& Created = Fixtures.Add(TEXT("IK_Recreated"));
		double Start = FPlatformTime::Seconds();
		FAssetDocumentResult Applied;
		if (!ApplyOk(*this, Created, ForTarget(Original.Payload, Created, TEXT("Create")), Name + TEXT(" Create"), &Applied))
		{
			continue;
		}
		const double CreateMs = Milliseconds(Start);
		const FAssetDocumentResult Recreated = ExtractResult(Created.ObjectPath);
		const FString Expected = WithoutTarget(Original.Payload);
		const FString Got = WithoutTarget(Recreated.Payload);
		if (!TestEqual(*FString::Printf(TEXT("%s: the created rig extracts as the source"), *Name), Got, Expected))
		{
			AddInfo(FirstDifference(Expected, Got));
		}
		TestEqual(*FString::Printf(TEXT("%s: equal status"), *Name), StatusOf(Recreated).Replace(*Created.PackageName, TEXT("")),
			StatusOf(Original).Replace(*Source.Value->PackageName, TEXT("")));
		Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Again = Apply(Created, ForTarget(Recreated.Payload, Created, TEXT("Update")));
		const double ReapplyMs = Milliseconds(Start);
		TestTrue(*FString::Printf(TEXT("%s: unchanged re-apply succeeds"), *Name), Again.IsSuccess());
		TestFalse(*FString::Printf(TEXT("%s: unchanged re-apply is a no-op"), *Name), Again.bSavedAsset);
		TestNotNull(*FString::Printf(TEXT("%s: reload"), *Name), Reload(Created));
		TestEqual(*FString::Printf(TEXT("%s: equal after reload"), *Name), WithoutTarget(ExtractResult(Created.ObjectPath).Payload), Expected);
		AddInfo(FString::Printf(TEXT("PASS %s: Create %.0f ms, unchanged re-apply %.0f ms, %d chars, status %s%s"), *Name, CreateMs, ReapplyMs,
			Expected.Len(), Recreated.Compile.IsValid() ? *Recreated.Compile->GetStringField(TEXT("Status")) : TEXT("-"),
			Applied.Diagnostics.IsEmpty() ? TEXT("") : *(TEXT("; apply warnings: ") + DiagnosticsText(Applied).Left(400))));
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRigFreeEditsTest, "UEAssetDoc.IKRig.FreeEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRigFreeEditsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRigApplyTests;
	FFixtures Fixtures;
	const FTarget& Editor = Fixtures.Add(TEXT("IK_EditorSide"));
	const FTarget& Document = Fixtures.Add(TEXT("IK_DocumentSide"));
	UIKRigDefinition* Rig = MakeRig(*this, Editor, TutorialMesh, true);
	if (Rig == nullptr || !TestNotNull(TEXT("duplicate"), Duplicate(*this, Rig, Document)))
	{
		return false;
	}
	UIKRigController* Controller = UIKRigController::GetController(Rig);
	const int32 FBIK = SolverIndexOf(*Rig, SolverType(TEXT("FullBodyIK")));
	if (!TestTrue(TEXT("Auto FBIK made a Full Body IK solver"), FBIK != INDEX_NONE) || !TestTrue(TEXT("auto rig has goals"), Rig->GetGoalArray().Num() > 0))
	{
		return false;
	}
	// The editor's calls (design §4.1), as its UI makes them.
	// Goals: a hand goal connected to FBIK with a pull-chain setting; rename a goal a chain and FBIK use.
	TestEqual(TEXT("add goal"), Controller->AddNewGoal(TEXT("HandGoal"), TEXT("hand_r")), FName(TEXT("HandGoal")));
	TestTrue(TEXT("connect"), Controller->ConnectGoalToSolver(TEXT("HandGoal"), FBIK));
	{
		// Details edit of the goal settings (struct wrapper: Modify, write, copy back).
		FScopedTransaction Transaction(NSLOCTEXT("IKRigTests", "GoalSettings", "Goal settings"));
		Rig->Modify();
		FIKRigSolverBase* Solver = Controller->GetSolverAtIndex(FBIK);
		SetField(*this, Solver->GetGoalSettingsType(), Solver->GetGoalSettings(TEXT("HandGoal")), TEXT("PullChainAlpha"), TEXT("0.25"));
	}
	FName Renamed = Rig->GetGoalArray()[0]->GoalName;
	for (const FBoneChain& Chain : Rig->GetRetargetChains())
	{
		if (!Chain.IKGoalName.IsNone())
		{
			Renamed = Chain.IKGoalName;
			break;
		}
	}
	TestEqual(TEXT("rename goal"), Controller->RenameGoal(Renamed, TEXT("RenamedGoal")), FName(TEXT("RenamedGoal")));
	// Solvers: add two, delete the middle one, move the last to the top, disable it.
	Controller->AddSolver(const_cast<UScriptStruct*>(SolverType(TEXT("LimbIK"))));
	Controller->AddSolver(const_cast<UScriptStruct*>(SolverType(TEXT("Pole"))));
	const int32 Count = Controller->GetNumSolvers();
	TestTrue(TEXT("remove middle solver"), Controller->RemoveSolver(Count - 2));
	TestTrue(TEXT("move solver up"), Controller->MoveSolverInStack(Controller->GetNumSolvers() - 1, 0));
	TestTrue(TEXT("disable solver"), Controller->SetSolverEnabled(0, false));
	const int32 FullBody = SolverIndexOf(*Rig, SolverType(TEXT("FullBodyIK")));
	// Bone settings: add limits on calf_l, remove another bone's settings.
	TestTrue(TEXT("bone setting calf_l"), Controller->GetSolverAtIndex(FullBody)->HasSettingsOnBone(TEXT("calf_l"))
		|| Controller->AddBoneSetting(TEXT("calf_l"), FullBody));
	{
		FScopedTransaction Transaction(NSLOCTEXT("IKRigTests", "BoneSettings", "Bone settings"));
		Rig->Modify();
		FIKRigSolverBase* Solver = Controller->GetSolverAtIndex(FullBody);
		const UScriptStruct* BoneType = Solver->GetBoneSettingsType();
		void* Settings = Solver->GetBoneSettings(TEXT("calf_l"));
		SetField(*this, BoneType, Settings, TEXT("X"), TEXT("Limited"));
		SetField(*this, BoneType, Settings, TEXT("MinX"), TEXT("-20"));
		SetField(*this, BoneType, Settings, TEXT("MaxX"), TEXT("30"));
	}
	TSet<FName> WithSettings;
	Controller->GetSolverAtIndex(FullBody)->GetBonesWithSettings(WithSettings);
	for (const FName& Bone : WithSettings)
	{
		if (Bone != TEXT("calf_l"))
		{
			TestTrue(TEXT("remove bone setting"), Controller->RemoveBoneSetting(Bone, FullBody));
			break;
		}
	}
	// Bones and chains.
	TestTrue(TEXT("exclude a bone"), Controller->SetBoneExcluded(Rig->GetSkeleton().BoneNames.Last(), true));
	TestEqual(TEXT("add chain"), Controller->AddRetargetChain(TEXT("NewChain"), TEXT("spine_01"), TEXT("spine_02"), NAME_None), FName(TEXT("NewChain")));
	const TArray<FBoneChain> Chains = Rig->GetRetargetChains();
	if (!TestTrue(TEXT("auto rig has chains"), Chains.Num() >= 3))
	{
		return false;
	}
	const FName RenamedChain = Chains[0].ChainName;
	TestEqual(TEXT("rename chain"), Controller->RenameRetargetChain(RenamedChain, TEXT("RenamedChain")), FName(TEXT("RenamedChain")));
	TestTrue(TEXT("re-point chain"), Controller->SetRetargetChainEndBone(TEXT("NewChain"), TEXT("spine_03")));
	TestTrue(TEXT("remove chain"), Controller->RemoveRetargetChain(Chains[1].ChainName));
	TestTrue(TEXT("retarget root"), Controller->SetRetargetRoot(TEXT("spine_01")));
	TestTrue(TEXT("editor side saved"), Save(*Rig));

	// The same state as a document (renames carried with RenamedFrom) on the other duplicate.
	const TSharedPtr<FJsonObject> EditorState = Extract(*this, Editor.ObjectPath, TEXT("editor side"));
	if (!EditorState.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Edit = ForTarget(EditorState, Document, TEXT("Update"));
	BodyOf(Edit)->GetObjectField(TEXT("Goals"))->GetObjectField(TEXT("RenamedGoal"))->SetStringField(TEXT("RenamedFrom"), Renamed.ToString());
	BodyOf(Edit)->GetObjectField(TEXT("Chains"))->GetObjectField(TEXT("RenamedChain"))->SetStringField(TEXT("RenamedFrom"), RenamedChain.ToString());
	const double Start = FPlatformTime::Seconds();
	FAssetDocumentResult Applied;
	if (!ApplyOk(*this, Document, Edit, TEXT("document side"), &Applied))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("document side applied in %.0f ms; diagnostics: %s"), Milliseconds(Start), *DiagnosticsText(Applied).Left(600)));
	const FAssetDocumentResult EditorSide = ExtractResult(Editor.ObjectPath);
	const FAssetDocumentResult DocumentSide = ExtractResult(Document.ObjectPath);
	const FString Expected = WithoutTarget(EditorSide.Payload);
	const FString Got = WithoutTarget(DocumentSide.Payload);
	if (!TestEqual(TEXT("both duplicates extract equal"), Got, Expected))
	{
		AddInfo(FirstDifference(Expected, Got));
	}
	TestEqual(TEXT("equal status"), StatusOf(DocumentSide).Replace(*Document.PackageName, TEXT("")),
		StatusOf(EditorSide).Replace(*Editor.PackageName, TEXT("")));
	// The renamed goal kept its FBIK goal settings (a rename, not remove + add).
	UIKRigDefinition* DocumentRig = Cast<UIKRigDefinition>(Document.Get());
	if (TestNotNull(TEXT("document rig"), DocumentRig))
	{
		TestTrue(TEXT("renamed goal still connected"), UIKRigController::GetController(DocumentRig)->IsGoalConnectedToAnySolver(TEXT("RenamedGoal")));
	}
	FString DiffText;
	TestEqual(TEXT("Diff of the document is empty"), DiffCount(ForTarget(EditorState, Document, TEXT("Update")), DiffText), 0);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRigFromScratchTest, "UEAssetDoc.IKRig.FromScratch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRigFromScratchTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRigApplyTests;
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("IK_Scratch"));
	const TSharedPtr<FJsonObject> Document = RigDocument(Target, TEXT("Create"), ScratchBody);
	FAssetDocumentResult Created;
	if (!TestTrue(TEXT("document parses"), Document.IsValid()) || !ApplyOk(*this, Target, Document, TEXT("AI Create"), &Created))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("Create diagnostics: %s"), *DiagnosticsText(Created)));
	const FAssetDocumentResult Extracted = ExtractResult(Target.ObjectPath);
	TestTrue(TEXT("status has no errors"), Extracted.Compile.IsValid() && Extracted.Compile->GetStringField(TEXT("Status")) != TEXT("Error")
		&& Extracted.Compile->GetStringField(TEXT("Status")) != TEXT("Unknown"));
	AddInfo(FString::Printf(TEXT("status %s"), *StatusOf(Extracted)));
	FString DiffText;
	if (!TestEqual(TEXT("the created rig has no differences from the document"), DiffCount(ForTarget(Document, Target, TEXT("Update")), DiffText), 0))
	{
		AddInfo(DiffText.Left(1200));
		AddInfo(SerializePretty(Extracted.Payload).Left(3000));
	}
	const FAssetDocumentResult Again = Apply(Target, ForTarget(Document, Target, TEXT("Update")));
	TestTrue(TEXT("re-apply succeeds"), Again.IsSuccess());
	TestFalse(TEXT("re-apply is a no-op"), Again.bSavedAsset);
	UIKRigDefinition* Rig = Cast<UIKRigDefinition>(Target.Get());
	if (!TestNotNull(TEXT("rig"), Rig))
	{
		return false;
	}
	TestTrue(TEXT("limb solver drives the hand goal"), UIKRigController::GetController(Rig)->IsGoalConnectedToSolver(TEXT("RightHandIK"), 1));
	const UIKRigEffectorGoal* Hand = UIKRigController::GetController(Rig)->GetGoal(TEXT("RightHandIK"));
	TestTrue(TEXT("hand goal offset is 10 cm up"), Hand != nullptr
		&& Hand->CurrentTransform.GetRelativeTransform(Hand->InitialTransform).GetTranslation().Equals(FVector(0, 0, 10), 1.0e-3));

	// Authoring errors at their document paths; the rig is unchanged.
	auto Edited = [&Target](const TFunction<void(const TSharedPtr<FJsonObject>&)>& Change)
	{
		const TSharedPtr<FJsonObject> Copy = RigDocument(Target, TEXT("Update"), ScratchBody);
		Change(BodyOf(Copy));
		return Copy;
	};
	ExpectRefused(*this, Target, Edited([](const TSharedPtr<FJsonObject>& Body)
	{
		Body->GetObjectField(TEXT("Goals"))->SetObjectField(TEXT("Bad"), Parse(TEXT(R"({"Bone": "foot_lx"})")));
	}), TEXT("IKRig.BoneNotFound"), TEXT("/Body/Goals/Bad/Bone"), TEXT("unknown bone"));
	ExpectRefused(*this, Target, Edited([](const TSharedPtr<FJsonObject>& Body)
	{
		// A goal is added in an earlier phase; the unknown solver type is refused before any edit.
		Body->GetObjectField(TEXT("Goals"))->SetObjectField(TEXT("Extra"), Parse(TEXT(R"({"Bone": "head"})")));
		TArray<TSharedPtr<FJsonValue>> Solvers = Body->GetArrayField(TEXT("Solvers"));
		Solvers.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"({"Type": "NoSuchSolver"})"))));
		Body->SetArrayField(TEXT("Solvers"), Solvers);
	}), TEXT("IKRig.UnknownSolverType"), TEXT("/Body/Solvers/2/Type"), TEXT("unknown solver type"));
	ExpectRefused(*this, Target, Edited([](const TSharedPtr<FJsonObject>& Body)
	{
		// The goal is added (the goals pass) and the solver connected before the limb solver refuses a second goal: rolled back.
		Body->GetObjectField(TEXT("Goals"))->SetObjectField(TEXT("Extra"), Parse(TEXT(R"({"Bone": "head"})")));
		Body->GetArrayField(TEXT("Solvers"))[1]->AsObject()->GetObjectField(TEXT("Goals"))->SetObjectField(TEXT("LeftFootIK"), MakeShared<FJsonObject>());
	}), TEXT("IKRig.SolverGoalLimit"), TEXT("/Body/Solvers/1/Goals/LeftFootIK"), TEXT("second goal on a limb solver"));
	ExpectRefused(*this, Target, Edited([](const TSharedPtr<FJsonObject>& Body)
	{
		Body->SetStringField(TEXT("PreviewMesh"), TEXT("/Engine/BasicShapes/Cube.Cube"));
	}), TEXT("IKRig.UnknownMesh"), TEXT("/Body/PreviewMesh"), TEXT("not a skeletal mesh"));
	ExpectRefused(*this, Target, Edited([](const TSharedPtr<FJsonObject>& Body)
	{
		Body->GetArrayField(TEXT("Solvers"))[0]->AsObject()->GetObjectField(TEXT("Settings"))->SetStringField(TEXT("RootBone"), TEXT("spine_01"));
	}), TEXT("IKRig.FieldNotEditable"), TEXT("/Body/Solvers/0/Settings/RootBone"), TEXT("read-only solver setting"));
	ExpectRefused(*this, Target, Edited([](const TSharedPtr<FJsonObject>& Body)
	{
		Body->GetArrayField(TEXT("Solvers"))[0]->AsObject()->GetObjectField(TEXT("Settings"))->SetStringField(TEXT("Iterations"), TEXT("many"));
	}), TEXT("IKRig.InvalidValue"), TEXT("/Body/Solvers/0/Settings/Iterations"), TEXT("wrong value type"));
	ExpectRefused(*this, Target, Edited([](const TSharedPtr<FJsonObject>& Body)
	{
		Body->GetObjectField(TEXT("Chains"))->GetObjectField(TEXT("Spine"))->SetStringField(TEXT("Goal"), TEXT("NoSuchGoal"));
	}), TEXT("IKRig.GoalNotFound"), TEXT("/Body/Chains/Spine/Goal"), TEXT("chain goal that does not exist"));

	// A chain the skeleton cannot resolve (end is not below start) applies with the editor's warning at its path.
	FAssetDocumentResult Warned;
	if (ApplyOk(*this, Target, Edited([](const TSharedPtr<FJsonObject>& Body)
	{
		Body->GetObjectField(TEXT("Chains"))->SetObjectField(TEXT("Backwards"), Parse(TEXT(R"({"Start": "foot_l", "End": "thigh_l"})")));
	}), TEXT("backwards chain"), &Warned))
	{
		const bool bWarned = Warned.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == TEXT("IKRig.ValidationWarning") && Diagnostic.Path == TEXT("/Body/Chains/Backwards");
		});
		TestTrue(*FString::Printf(TEXT("backwards chain warns at its path: %s"), *DiagnosticsText(Warned)), bWarned);
	}

	// A chain rename reports the retargeter that references the rig; the retargeter is not edited.
	const FTarget& RetargeterTarget = Fixtures.Add(TEXT("RTG_Scratch"));
	UIKRetargeter* Retargeter = MakeRetargeter(*this, RetargeterTarget, Rig, Rig);
	if (Retargeter != nullptr)
	{
		const FString RetargeterBefore = Serialize(ExtractResult(RetargeterTarget.ObjectPath).Payload);
		FAssetDocumentResult Renamed;
		if (ApplyOk(*this, Target, Edited([](const TSharedPtr<FJsonObject>& Body)
		{
			const TSharedPtr<FJsonObject> Chains = Body->GetObjectField(TEXT("Chains"));
			const TSharedPtr<FJsonObject> Leg = Chains->GetObjectField(TEXT("LeftLeg"));
			Leg->SetStringField(TEXT("RenamedFrom"), TEXT("LeftLeg"));
			Chains->RemoveField(TEXT("LeftLeg"));
			Chains->SetObjectField(TEXT("LegL"), Leg);
		}), TEXT("chain rename"), &Renamed))
		{
			TestTrue(*FString::Printf(TEXT("chain rename reports the retargeter: %s"), *DiagnosticsText(Renamed)),
				HasCode(Renamed, TEXT("IKRig.DependentsAffected")) && DiagnosticsText(Renamed).Contains(RetargeterTarget.PackageName));
		}
		TestFalse(TEXT("the retargeter package is not dirtied"), Retargeter->GetOutermost()->IsDirty());
		AddInfo(FString::Printf(TEXT("retargeter unchanged on disk; in memory %s"),
			Serialize(ExtractResult(RetargeterTarget.ObjectPath).Payload) == RetargeterBefore ? TEXT("unchanged") : TEXT("re-synced (not saved)")));
	}

	// A failed Create leaves nothing behind.
	const FTarget& Failed = Fixtures.Add(TEXT("IK_FailedCreate"));
	const TSharedPtr<FJsonObject> Bad = RigDocument(Failed, TEXT("Create"), ScratchBody);
	BodyOf(Bad)->GetObjectField(TEXT("Goals"))->GetObjectField(TEXT("LeftFootIK"))->SetStringField(TEXT("Bone"), TEXT("nope"));
	const FAssetDocumentResult FailedCreate = Apply(Failed, Bad);
	TestFalse(TEXT("failing Create fails"), FailedCreate.IsSuccess());
	TestTrue(*FString::Printf(TEXT("failing Create: BoneNotFound (%s)"), *DiagnosticsText(FailedCreate)), HasCode(FailedCreate, TEXT("IKRig.BoneNotFound")));
	TestNull(TEXT("failing Create leaves no object"), Failed.Get());
	TestFalse(TEXT("failing Create leaves no file"), IFileManager::Get().FileExists(*Failed.PackageFile));
	// Bones without a preview mesh.
	const TSharedPtr<FJsonObject> Meshless = RigDocument(Failed, TEXT("Create"), TEXT(R"({"Goals": {"Foot": {"Bone": "foot_l"}}})"));
	const FAssetDocumentResult NoMesh = Apply(Failed, Meshless);
	TestTrue(*FString::Printf(TEXT("bones need a preview mesh (%s)"), *DiagnosticsText(NoMesh)), !NoMesh.IsSuccess() && HasCode(NoMesh, TEXT("IKRig.PreviewMeshRequired")));
	TestNull(TEXT("refused Create leaves no object"), Failed.Get());
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRigUndoTest, "UEAssetDoc.IKRig.Undo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRigUndoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRigApplyTests;
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("IK_Undo"));
	UIKRigDefinition* Rig = MakeRig(*this, Target, TutorialMesh, true);
	if (Rig == nullptr)
	{
		return false;
	}
	ReleaseUndoHistory();
	auto UndoRedoAll = [this, &Target](const TCHAR* Stage)
	{
		int32 Undone = 0;
		while (Undone < 16 && GEditor->Trans->CanUndo() && GEditor->UndoTransaction())
		{
			++Undone;
			TestTrue(*FString::Printf(TEXT("%s: whole after undo %d"), Stage, Undone), ExtractResult(Target.ObjectPath).IsSuccess());
		}
		int32 Redone = 0;
		while (Redone < Undone && GEditor->Trans->CanRedo() && GEditor->RedoTransaction())
		{
			++Redone;
		}
		TestEqual(*FString::Printf(TEXT("%s: every undone step redoes"), Stage), Redone, Undone);
	};
	// 1. The human changes the retarget root in the editor and saves; the AI edits: history referencing the rig is
	// cleared, the human edit is kept.
	UIKRigController* Controller = UIKRigController::GetController(Rig);
	TestTrue(TEXT("human edit"), Controller->SetRetargetRoot(TEXT("spine_01")));
	TestTrue(TEXT("human save"), UEditorLoadingAndSavingUtils::SavePackages({Rig->GetOutermost()}, false));
	TestTrue(TEXT("the human edit is in the undo history"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const TSharedPtr<FJsonObject> HumanState = Extract(*this, Target.ObjectPath, TEXT("after human edit"));
	const TSharedPtr<FJsonObject> AiEdit = ForTarget(HumanState, Target, TEXT("Update"));
	BodyOf(AiEdit)->GetObjectField(TEXT("Goals"))->SetObjectField(TEXT("Extra"), Parse(TEXT(R"({"Bone": "head", "PositionAlpha": 0.5})")));
	if (!ApplyOk(*this, Target, AiEdit, TEXT("AI edit")))
	{
		return false;
	}
	TestFalse(TEXT("after AI edit: no undo history references the rig"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const FString AfterEdit = Serialize(Extract(*this, Target.ObjectPath, TEXT("after AI edit")));
	TestTrue(TEXT("human edit kept"), AfterEdit.Contains(TEXT("\"RetargetRoot\":\"spine_01\"")));
	UndoRedoAll(TEXT("after AI edit"));
	TestEqual(TEXT("after AI edit + undo/redo"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after undo/redo"))), AfterEdit);

	// 2. The human edits again; an AI apply fails and rolls back to the human state; undo stays safe.
	ReleaseUndoHistory();
	TestTrue(TEXT("second human edit"), Controller->SetRetargetRoot(TEXT("pelvis")));
	const FString SecondHuman = Serialize(Extract(*this, Target.ObjectPath, TEXT("after second human edit")));
	const TSharedPtr<FJsonObject> Failing = ForTarget(Parse(SecondHuman), Target, TEXT("Update"));
	BodyOf(Failing)->GetObjectField(TEXT("Goals"))->SetObjectField(TEXT("Bad"), Parse(TEXT(R"({"Bone": "no_such_bone"})")));
	const FAssetDocumentResult Failed = Apply(Target, Failing);
	TestFalse(TEXT("failing apply fails"), Failed.IsSuccess());
	TestEqual(TEXT("after rollback: the human state"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after rollback"))), SecondHuman);
	UndoRedoAll(TEXT("after rollback"));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRigStructWritesTest, "UEAssetDoc.IKRig.StructWrites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRigStructWritesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRigApplyTests;
	FFixtures Fixtures;
	const FTarget& Editor = Fixtures.Add(TEXT("IK_WrapperSide"));
	const FTarget& Document = Fixtures.Add(TEXT("IK_DocumentSide"));
	UIKRigDefinition* Rig = MakeRig(*this, Editor, TutorialMesh, true);
	if (Rig == nullptr || !TestNotNull(TEXT("duplicate"), Duplicate(*this, Rig, Document)))
	{
		return false;
	}
	const int32 FBIK = SolverIndexOf(*Rig, SolverType(TEXT("FullBodyIK")));
	FIKRigSolverBase* Solver = FBIK != INDEX_NONE ? UIKRigController::GetController(Rig)->GetSolverAtIndex(FBIK) : nullptr;
	if (!TestNotNull(TEXT("FBIK solver"), Solver))
	{
		return false;
	}
	// The class the IK Rig editor shows FBIK settings with (the struct's UIWrapper metadata) and its "Settings"
	// member, as FIKRigEditorController::ShowDetailsForSolvers initializes it.
	const UScriptStruct* SettingsType = Solver->GetSolverSettingsType();
	const FString WrapperPath = SettingsType->GetMetaData(TEXT("UIWrapper"));
	UClass* WrapperClass = WrapperPath.IsEmpty() ? nullptr : LoadObject<UClass>(nullptr, *WrapperPath);
	if (!TestNotNull(*FString::Printf(TEXT("UIWrapper class of %s ('%s')"), *SettingsType->GetName(), *WrapperPath), WrapperClass))
	{
		return false;
	}
	const FStructProperty* WrapperSettings = CastField<FStructProperty>(WrapperClass->FindPropertyByName(TEXT("Settings")));
	if (!TestTrue(TEXT("the wrapper's Settings is the solver settings struct"), WrapperSettings != nullptr && WrapperSettings->Struct == SettingsType))
	{
		return false;
	}
	// Kept alive across the applies below (an apply collects garbage when it returns), as the editor's details view holds it.
	const TStrongObjectPtr<UObject> Wrapper(NewObject<UObject>(GetTransientPackage(), WrapperClass));
	void* WrapperMemory = WrapperSettings->ContainerPtrToValuePtr<void>(Wrapper.Get());
	// UpdateWrapperStructWithLatestValues (on Initialize), the details edit on the wrapper's copy inside the
	// struct viewer's transaction (PreChange: BeginTransaction + Owner->Modify()), then PostEditChangeProperty's
	// UpdateWrappedStructWithLatestValues (CopyScriptStruct back into the solver), EndTransaction.
	SettingsType->CopyScriptStruct(WrapperMemory, Solver->GetSolverSettings());
	GEditor->BeginTransaction(NSLOCTEXT("IKRigTests", "Wrapper", "Edited property."));
	Rig->Modify();
	SetField(*this, SettingsType, WrapperMemory, TEXT("Iterations"), TEXT("42"));
	SetField(*this, SettingsType, WrapperMemory, TEXT("RootBehavior"), TEXT("Free"));
	SetField(*this, SettingsType, WrapperMemory, TEXT("PrePullRootSettings"), TEXT("(RotationAlpha=0.25)"));
	SetField(*this, SettingsType, WrapperMemory, TEXT("bAllowStretch"), TEXT("True"));
	SettingsType->CopyScriptStruct(Solver->GetSolverSettings(), WrapperMemory);
	GEditor->EndTransaction();
	TestTrue(TEXT("wrapper side saved"), Save(*Rig));

	// The same Details edit as a document on the duplicate.
	const TSharedPtr<FJsonObject> WrapperState = Extract(*this, Editor.ObjectPath, TEXT("wrapper side"));
	if (!WrapperState.IsValid() || !ApplyOk(*this, Document, ForTarget(WrapperState, Document, TEXT("Update")), TEXT("document side")))
	{
		return false;
	}
	UIKRigDefinition* DocumentRig = Cast<UIKRigDefinition>(Document.Get());
	FIKRigSolverBase* DocumentSolver = DocumentRig != nullptr ? UIKRigController::GetController(DocumentRig)->GetSolverAtIndex(FBIK) : nullptr;
	if (TestNotNull(TEXT("document solver"), DocumentSolver))
	{
		TestTrue(TEXT("solver settings equal the wrapper edit"), SettingsType->CompareScriptStruct(Solver->GetSolverSettings(),
			DocumentSolver->GetSolverSettings(), PPF_None));
	}
	TestEqual(TEXT("both extract equal"), WithoutTarget(ExtractResult(Document.ObjectPath).Payload), WithoutTarget(WrapperState));
	Wrapper->MarkAsGarbage();
	return true;
}

#endif
