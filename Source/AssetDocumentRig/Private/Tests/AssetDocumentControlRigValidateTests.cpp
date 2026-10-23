// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Control Rig Validate, diff-only Extract, class Inspect and FactoryContext.
// UEAssetDoc.ControlRig.Validate: validate_asset_document runs apply's checks on a transient trial rig
//   for a Create (nothing is made) and an Update (the rig, its package and the undo history are untouched): unit, pin,
//   node and element errors come back at their document paths, with close names and the node's pins.
// UEAssetDoc.ControlRig.DiffOnly: Extract with diff_only leaves Class Settings at their class defaults
//   out; sizes with and without; a Create from the diff-only document extracts back to it.
// UEAssetDoc.ControlRig.ClassInspect: Inspect of the Control Rig class (no asset): Class Settings with
//   defaults and the ways to create a rig.
// UEAssetDoc.ControlRig.FactoryContext: Create with FactoryContext = a mesh equals "Create Control Rig"
//   on that mesh plus the same hierarchy edit; the document lists only what the rig adds.

#include "AssetDocumentIKRigTestSupport.h"

#include "ControlRig.h"
#include "ControlRigBlueprintLegacy.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "RigVMModel/RigVMController.h"
#include "UObject/UObjectIterator.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentControlRigTests
{
UControlRigBlueprint* MakeControlRig(FAutomationTestBase& Test, const AssetDocumentIKRigTests::FTarget& Target, const TCHAR* MeshPath, bool bModular);
} // namespace AssetDocumentControlRigTests

namespace AssetDocumentControlRigValidateTests
{
using namespace AssetDocumentIKRigTests;

const TCHAR* const ControlRigClassPath = TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint");

TSharedPtr<FJsonObject> Json(const ANSICHAR* Text)
{
	return Parse(FString(UTF8_TO_TCHAR(Text)));
}

// Two rigs' documents to compare: without Target and the asset variant's Guid (each new asset gets its own).
FString WithoutIdentity(const TSharedPtr<FJsonObject>& Document)
{
	if (!Document.IsValid())
	{
		return FString();
	}
	const TSharedPtr<FJsonObject> Copy = Parse(WithoutTarget(Document));
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	const TSharedPtr<FJsonObject>* Variant = nullptr;
	if (Copy->TryGetObjectField(TEXT("Properties"), Properties) && (*Properties)->TryGetObjectField(TEXT("AssetVariant"), Variant))
	{
		(*Variant)->RemoveField(TEXT("Guid"));
	}
	return Serialize(Copy);
}

TSharedPtr<FJsonObject> ForTarget(const TSharedPtr<FJsonObject>& Document, const FTarget& Target, const TCHAR* Action)
{
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Document));
	Copy->SetStringField(TEXT("Target"), Target.PackageName);
	Copy->SetStringField(TEXT("Action"), Action);
	return Copy;
}

TSharedPtr<FJsonObject> NewRig(const FTarget& Target, const ANSICHAR* Body)
{
	const TSharedPtr<FJsonObject> Document = Json(R"J({"SchemaVersion": 1, "Action": "Create", "Properties": {}})J");
	Document->SetStringField(TEXT("Class"), ControlRigClassPath);
	Document->SetStringField(TEXT("Target"), Target.PackageName);
	Document->SetObjectField(TEXT("Body"), Json(Body));
	return Document;
}

FAssetDocumentResult Validate(const TSharedPtr<FJsonObject>& Document)
{
	FAssetDocumentValidateRequest Request;
	Request.Document = Document;
	return FAssetDocumentModule::Get().GetService().Validate(Request);
}

FAssetDocumentResult ExtractWith(const FString& ObjectPath, bool bDiffOnly)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = ObjectPath;
	Request.bDiffOnly = bDiffOnly;
	return FAssetDocumentModule::Get().GetService().Extract(Request);
}

TSharedPtr<FJsonObject> NodesOf(const TSharedPtr<FJsonObject>& Document)
{
	return BodyOf(Document)->GetObjectField(TEXT("Graphs"))->GetObjectField(TEXT("Rig"))->GetObjectField(TEXT("Nodes"));
}

int32 TrialRigs()
{
	int32 Count = 0;
	for (TObjectIterator<UControlRigBlueprint> It; It; ++It)
	{
		Count += It->GetName().StartsWith(TEXT("Trial_")) ? 1 : 0;
	}
	return Count;
}

// Validate refuses Document with Code at Path; the message contains every Needle.
void ExpectInvalid(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Document, const TCHAR* Code, const TCHAR* Path,
	std::initializer_list<const TCHAR*> Needles, const TCHAR* Stage)
{
	const FAssetDocumentResult Result = Validate(Document);
	const FString Text = DiagnosticsText(Result);
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), Stage, *Text));
	Test.TestFalse(*FString::Printf(TEXT("%s: invalid"), Stage), Result.IsSuccess());
	Test.TestTrue(*FString::Printf(TEXT("%s: %s"), Stage, Code), HasCode(Result, Code));
	Test.TestTrue(*FString::Printf(TEXT("%s: at %s (got %s)"), Stage, Path, *FirstPath(Result)), FirstPath(Result).StartsWith(Path));
	for (const TCHAR* Needle : Needles)
	{
		Test.TestTrue(*FString::Printf(TEXT("%s: the message names '%s'"), Stage, Needle), Text.Contains(Needle));
	}
}

const ANSICHAR* const RigBody = R"J({"Variables": [{"Name": "Weight", "Type": "float", "Default": 0.5}],
	"Graphs": {"Rig": {"Nodes": {
		"Begin": {"Unit": "RigUnit_BeginExecution", "Position": [0, 0]},
		"Set": {"Unit": "RigUnit_SetTransform", "Position": [300, 0], "Inputs": {"ExecutePin": "Begin.ExecutePin"}},
		"Add": {"Unit": "RigVMFunction_MathFloatAdd", "Position": [0, 300], "Defaults": {"A": 1.0}}}}}})J";
} // namespace AssetDocumentControlRigValidateTests

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigValidateTest, "UEAssetDoc.ControlRig.Validate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigValidateTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigValidateTests;
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("CR_Validate"));

	// 1. Create: a valid document validates and nothing is made; the errors of apply come back at validate.
	const TSharedPtr<FJsonObject> Good = NewRig(Target, RigBody);
	double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Valid = Validate(Good);
	const double ValidateMs = Milliseconds(Start);
	TestTrue(*FString::Printf(TEXT("valid Create validates (%s)"), *DiagnosticsText(Valid)), Valid.IsSuccess());
	TestNull(TEXT("validate made no asset"), Target.Get());
	TestFalse(TEXT("validate wrote no package"), IFileManager::Get().FileExists(*Target.PackageFile));
	{
		const TSharedPtr<FJsonObject> Bad = Parse(Serialize(Good));
		NodesOf(Bad)->GetObjectField(TEXT("Set"))->SetStringField(TEXT("Unit"), TEXT("RigUnit_SetTransfrom"));
		ExpectInvalid(*this, Bad, TEXT("ControlRig.UnknownUnit"), TEXT("/Body/Graphs/Rig/Nodes/Set"), {TEXT("Close:"), TEXT("RigUnit_SetTransform")},
			TEXT("Create: unknown unit"));
	}
	{
		const TSharedPtr<FJsonObject> Bad = Parse(Serialize(Good));
		NodesOf(Bad)->GetObjectField(TEXT("Set"))->SetObjectField(TEXT("Inputs"), Json(R"J({"ExecutePin": "Begin.ExecutPin"})J"));
		ExpectInvalid(*this, Bad, TEXT("ControlRig.PinNotFound"), TEXT("/Body/Graphs/Rig/Nodes/Set"), {TEXT("Begin.ExecutePin"), TEXT("Pins of")},
			TEXT("Create: unknown source pin"));
	}
	{
		const TSharedPtr<FJsonObject> Bad = Parse(Serialize(Good));
		NodesOf(Bad)->GetObjectField(TEXT("Set"))->SetObjectField(TEXT("Defaults"), Json(R"J({"Weigth": 0.5})J"));
		ExpectInvalid(*this, Bad, TEXT("ControlRig.PinNotFound"), TEXT("/Body/Graphs/Rig/Nodes/Set"), {TEXT("Weight")},
			TEXT("Create: unknown default pin"));
	}
	{
		const TSharedPtr<FJsonObject> Bad = Parse(Serialize(Good));
		NodesOf(Bad)->GetObjectField(TEXT("Set"))->SetObjectField(TEXT("Inputs"), Json(R"J({"ExecutePin": "Begn.ExecutePin"})J"));
		ExpectInvalid(*this, Bad, TEXT("ControlRig.NodeNotFound"), TEXT("/Body/Graphs/Rig/Nodes/Set/Inputs"), {TEXT("Close: Begin")},
			TEXT("Create: unknown source node"));
	}
	TestNull(TEXT("no asset after the failed validates"), Target.Get());
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestEqual(TEXT("no trial rig outlives validate"), TrialRigs(), 0);

	// 2. Update: the rig exists; validate leaves it, its package and the undo history alone.
	FAssetDocumentResult Created = Apply(Target, Good);
	if (!TestTrue(*FString::Printf(TEXT("Create (%s)"), *DiagnosticsText(Created)), Created.IsSuccess()))
	{
		return false;
	}
	ReleaseUndoHistory();
	const FString Before = Serialize(ExtractResult(Target.ObjectPath).Payload);
	const TSharedPtr<FJsonObject> State = ForTarget(Parse(Before), Target, TEXT("Update"));
	const int32 UndoBefore = GEditor != nullptr && GEditor->Trans != nullptr ? GEditor->Trans->GetQueueLength() : 0;
	{
		const TSharedPtr<FJsonObject> Changed = Parse(Serialize(State));
		NodesOf(Changed)->SetObjectField(TEXT("Get"), Json(R"J({"Unit": "RigUnit_GetTransform", "Position": [0, 600]})J"));
		Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Ok = Validate(Changed);
		AddInfo(FString::Printf(TEXT("Update validate %.0f ms (Create validate %.0f ms)"), Milliseconds(Start), ValidateMs));
		TestTrue(*FString::Printf(TEXT("valid Update validates (%s)"), *DiagnosticsText(Ok)), Ok.IsSuccess());
	}
	{
		const TSharedPtr<FJsonObject> Bad = Parse(Serialize(State));
		NodesOf(Bad)->SetObjectField(TEXT("Get"), Json(R"J({"Unit": "RigUnit_GetTransform", "Position": [0, 600]})J"));
		NodesOf(Bad)->GetObjectField(TEXT("Set"))->SetObjectField(TEXT("Inputs"), Json(R"J({"ExecutePin": "Begin.ExecutePin", "Value": "Get.Transfrom"})J"));
		ExpectInvalid(*this, Bad, TEXT("ControlRig.PinNotFound"), TEXT("/Body/Graphs/Rig/Nodes/Set"), {TEXT("Get.Transform"), TEXT("Pins of 'Get'")},
			TEXT("Update: unknown pin of a new node"));
	}
	{
		const TSharedPtr<FJsonObject> Bad = Parse(Serialize(State));
		TArray<TSharedPtr<FJsonValue>> Elements;
		Elements.Add(MakeShared<FJsonValueObject>(Json(R"J({"Type": "Null", "Name": "a_null"})J")));
		Elements.Add(MakeShared<FJsonValueObject>(Json(R"J({"Type": "Null", "Name": "b_null", "Parent": "Null(a_nul)"})J")));
		BodyOf(Bad)->SetArrayField(TEXT("Hierarchy"), Elements);
		ExpectInvalid(*this, Bad, TEXT("ControlRig.ElementNotFound"), TEXT("/Body/Hierarchy/1"), {TEXT("a_null")}, TEXT("Update: unknown parent element"));
	}
	TestEqual(TEXT("the rig is unchanged"), Serialize(ExtractResult(Target.ObjectPath).Payload), Before);
	TestFalse(TEXT("the package is clean"), Target.Get() != nullptr && Target.Get()->GetOutermost()->IsDirty());
	TestEqual(TEXT("the undo history is untouched"), GEditor != nullptr && GEditor->Trans != nullptr ? GEditor->Trans->GetQueueLength() : 0, UndoBefore);
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestEqual(TEXT("no trial rig outlives validate"), TrialRigs(), 0);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigDiffOnlyTest, "UEAssetDoc.ControlRig.DiffOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigDiffOnlyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigValidateTests;
	FFixtures Fixtures;
	for (const TCHAR* Path : {TEXT("/MoverExamples/Characters/Mannequins/Rigs/CR_Mannequin_BasicFootIK.CR_Mannequin_BasicFootIK"),
			 TEXT("/ControlRigModules/Modules56/Hinge.Hinge")})
	{
		UObject* Source = LoadQuiet(Path);
		if (Source == nullptr)
		{
			AddInfo(FString::Printf(TEXT("%s is not mounted; skipped"), Path));
			continue;
		}
		const FTarget& Copy = Fixtures.Add(TEXT("CR_DiffOnly"));
		if (!TestNotNull(*FString::Printf(TEXT("%s duplicated"), Path), Duplicate(*this, Source, Copy)))
		{
			continue;
		}
		const FAssetDocumentResult Full = ExtractWith(Copy.ObjectPath, false);
		const FAssetDocumentResult Diff = ExtractWith(Copy.ObjectPath, true);
		if (!TestTrue(TEXT("both extracts succeed"), Full.IsSuccess() && Diff.IsSuccess()))
		{
			continue;
		}
		const int32 FullProperties = Full.Payload->GetObjectField(TEXT("Properties"))->Values.Num();
		const int32 DiffProperties = Diff.Payload->GetObjectField(TEXT("Properties"))->Values.Num();
		AddInfo(FString::Printf(TEXT("%s: full extract %d chars (%d Properties), diff_only %d chars (%d Properties): %s"), Path,
			Serialize(Full.Payload).Len(), FullProperties, Serialize(Diff.Payload).Len(), DiffProperties,
			*Serialize(Diff.Payload->GetObjectField(TEXT("Properties")))));
		TestTrue(TEXT("diff_only leaves class defaults out"), DiffProperties < FullProperties);
		TestEqual(TEXT("the Body is the same"), Serialize(BodyOf(Diff.Payload)), Serialize(BodyOf(Full.Payload)));
		// Round trip of the diff-only document.
		const FTarget& Created = Fixtures.Add(TEXT("CR_DiffOnlyCreated"));
		const FAssetDocumentResult Applied = Apply(Created, ForTarget(Diff.Payload, Created, TEXT("Create")));
		if (TestTrue(*FString::Printf(TEXT("Create from the diff-only document (%s)"), *DiagnosticsText(Applied)), Applied.IsSuccess()))
		{
			const FString Back = WithoutTarget(ExtractWith(Created.ObjectPath, true).Payload);
			if (!TestEqual(TEXT("the created rig extracts to the diff-only document"), Back, WithoutTarget(Diff.Payload)))
			{
				AddInfo(FirstDifference(WithoutTarget(Diff.Payload), Back));
			}
			TestEqual(TEXT("and to the full one"), WithoutTarget(ExtractWith(Created.ObjectPath, false).Payload), WithoutTarget(Full.Payload));
		}
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigClassInspectTest, "UEAssetDoc.ControlRig.ClassInspect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigClassInspectTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigValidateTests;
	const FAssetDocumentResult Inspected = Inspect(ControlRigClassPath);
	if (!TestTrue(*FString::Printf(TEXT("class inspect succeeds (%s)"), *DiagnosticsText(Inspected)), Inspected.IsSuccess() && Inspected.Payload.IsValid()))
	{
		return false;
	}
	AddInfo(SerializePretty(Inspected.Payload).Left(6000));
	const TSharedPtr<FJsonObject>* Settings = nullptr;
	const TSharedPtr<FJsonObject>* Create = nullptr;
	TestTrue(TEXT("ClassSettings"), Inspected.Payload->TryGetObjectField(TEXT("ClassSettings"), Settings) && (*Settings)->Values.Num() > 5);
	TestTrue(TEXT("ClassSettings has PreviewSkeletalMesh"), Settings != nullptr && (*Settings)->HasField(TEXT("PreviewSkeletalMesh")));
	TestTrue(TEXT("Create explains FactoryContext and the rig types"), Inspected.Payload->TryGetObjectField(TEXT("Create"), Create)
		&& (*Create)->HasField(TEXT("FactoryContext")) && (*Create)->HasField(TEXT("Modular")));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigFactoryContextTest, "UEAssetDoc.ControlRig.FactoryContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigFactoryContextTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigValidateTests;
	FFixtures Fixtures;
	// The editor: "Create Control Rig" on the mesh, a null added under the head in the hierarchy tab.
	const FTarget& Editor = Fixtures.Add(TEXT("CR_FromMeshEditor"));
	UControlRigBlueprint* EditorRig = AssetDocumentControlRigTests::MakeControlRig(*this, Editor, TutorialMesh, false);
	if (EditorRig == nullptr)
	{
		return false;
	}
	EditorRig->GetSourceCurveImport() = EditorRig->GetSourceHierarchyImport();
	// The editor's first open of the new rig adds the Forwards Solve event (a document without Graphs gets the same).
	URigVMBlueprint& EditorVM = *EditorRig;
	TestNotNull(TEXT("forwards solve event"), EditorVM.GetOrCreateController(EditorVM.GetDefaultModel())->AddUnitNodeFromStructPath(
		TEXT("/Script/ControlRig.RigUnit_BeginExecution"), TEXT("Execute"), FVector2D::ZeroVector, TEXT("RigUnit_BeginExecution"), false));
	EditorRig->GetHierarchy()->GetController(true)->AddNull(TEXT("aim_null"), FRigElementKey(TEXT("head"), ERigElementType::Bone),
		FTransform(FVector(0.0, 20.0, 0.0)), false, false);
	FKismetEditorUtilities::CompileBlueprint(EditorRig);
	TestTrue(TEXT("editor rig saved"), Save(*EditorRig));
	// The AI: FactoryContext and only the null.
	const FTarget& Target = Fixtures.Add(TEXT("CR_FromMeshDocument"));
	const TSharedPtr<FJsonObject> Document = NewRig(Target, R"J({"Hierarchy": [{"Type": "Null", "Name": "aim_null", "Parent": "Bone(head)",
		"Transform": {"Translation": {"X": 0.0, "Y": 20.0, "Z": 0.0}}}]})J");
	Document->SetStringField(TEXT("FactoryContext"), TutorialMesh);
	const FAssetDocumentResult Valid = Validate(Document);
	TestTrue(*FString::Printf(TEXT("validate (%s)"), *DiagnosticsText(Valid)), Valid.IsSuccess());
	{
		const TSharedPtr<FJsonObject> Bad = Parse(Serialize(Document));
		BodyOf(Bad)->GetArrayField(TEXT("Hierarchy"))[0]->AsObject()->SetStringField(TEXT("Parent"), TEXT("Bone(haed)"));
		ExpectInvalid(*this, Bad, TEXT("ControlRig.ElementNotFound"), TEXT("/Body/Hierarchy/0"), {TEXT("Bone(head)")},
			TEXT("FactoryContext: misspelled mesh bone"));
	}
	const double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Applied = Apply(Target, Document);
	if (!TestTrue(*FString::Printf(TEXT("Create with FactoryContext (%s)"), *DiagnosticsText(Applied)), Applied.IsSuccess()))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("Create with FactoryContext %.0f ms"), Milliseconds(Start)));
	const FString Expected = WithoutIdentity(ExtractResult(Editor.ObjectPath).Payload);
	const FString Got = WithoutIdentity(ExtractResult(Target.ObjectPath).Payload);
	if (!TestEqual(TEXT("the document rig equals Create Control Rig + the edit"), Got, Expected))
	{
		AddInfo(FirstDifference(Expected, Got));
	}
	// Re-applying the short document is a no-op (the imported bones are not the document's to remove).
	const FAssetDocumentResult Again = Apply(Target, ForTarget(Document, Target, TEXT("Update")));
	TestTrue(*FString::Printf(TEXT("re-apply of the short document (%s)"), *DiagnosticsText(Again)), Again.IsSuccess());
	TestFalse(TEXT("re-apply of the short document is a no-op"), Again.bSavedAsset);
	FString DiffText;
	TestEqual(TEXT("Diff of the short document is empty"), DiffCount(ForTarget(Document, Target, TEXT("Update")), DiffText), 0);
	AddInfo(DiffText.Left(600));
	return true;
}

#endif
