// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Control Rig function graphs, node order and Animation Blueprint pins.
// UEAssetDoc.ControlRig.ReplaceFunctions: the template's CR_Mannequin_FootIK duplicated (and
//   calling another rig's public function, its variable mapped to the rig's Gain), then a document with
//   `Functions: {}` and a new Rig graph (GetTransform, math, PBIK) that fails after every edit ran (in verification, on
//   a pin expansion the editor ignores): the rollback restores the rig (extract,
//   compile status, the function store the restored reference nodes resolve through); the corrected document, which
//   also removes the variables, then applies (which used to crash: "Could not find graph function data", then a RigVM assert
//   removing a variable), and so does the document as it was written.
// UEAssetDoc.ControlRig.RemoveCalledFunction: removing functions the kept graph calls removes the
//   nodes that call them, as the editor does.
// UEAssetDoc.ControlRig.UnmappedVariableFails: on the duplicate (editor graphs bound), a
//   retyped variable unmaps the call to the host's function; the VM compiler's error fails the apply at the node.
// UEAssetDoc.ControlRig.OrderKept: an Order that moves existing nodes applies with a
//   ControlRig.OrderKept warning; the nodes keep their place and Order is not compared.
// UEAssetDoc.ControlRig.AnimNodePins: an Animation Blueprint's Control Rig node exposes a
//   public variable the rig got later as an input pin through "ShowPin.<Variable>", linked in the same apply.
// UEAssetDoc.ControlRig.AnimNodeRigSpelling: the node's rig named by class or by asset reference
//   applies on every engine, re-applies as no change in the other spelling, and extracts in the node's own spelling.

#include "AssetDocumentIKRigTestSupport.h"
#include "AssetDocumentControlRig.h"

#include "Algo/Reverse.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "ControlRig.h"
#include "ControlRigBlueprintLegacy.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "RigVMCore/RigVMGraphFunctionHost.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentControlRigTests
{
struct FTemplateContent;
TSharedPtr<FTemplateContent> ShareCorpusTemplates();
UControlRigBlueprint* MakeControlRig(FAutomationTestBase& Test, const AssetDocumentIKRigTests::FTarget& Target, const TCHAR* MeshPath, bool bModular);
} // namespace AssetDocumentControlRigTests

namespace AssetDocumentControlRigFunctionApplyTests
{
using namespace AssetDocumentIKRigTests;

const TCHAR* const FootIKPath = TEXT("/Game/Characters/Mannequins/Rigs/CR_Mannequin_FootIK.CR_Mannequin_FootIK");

// A spear grip graph (CR_SpearGrip): GetTransform, vector math and a PBIK on the left arm.
const ANSICHAR* const SpearGripGraph = R"J({"Nodes": {"RigUnit_BeginExecution": {"Unit": "RigUnit_BeginExecution","Position": [-1800.0,-600.0]},"GetGun": {"Unit": "RigUnit_GetTransform","Position": [-1800.0,-300.0],"Defaults": {"Item": "Bone(ik_hand_gun)"}},"PaW": {"Unit": "RigVMFunction_MathTransformTransformVector","Position": [-1500.0,-350.0],"Defaults": {"Location": {"X": 0.0,"Y": 0.0,"Z": -52.2}},"Inputs": {"Transform": "GetGun.Transform"}},"PbW": {"Unit": "RigVMFunction_MathTransformTransformVector","Position": [-1500.0,-200.0],"Defaults": {"Location": {"X": 0.0,"Y": 0.0,"Z": 40.0}},"Inputs": {"Transform": "GetGun.Transform"}},"AxisD": {"Unit": "RigVMFunction_MathVectorSub","Position": [-1250.0,-250.0],"Inputs": {"A": "PbW.Result","B": "PaW.Result"}},"DotDD": {"Unit": "RigVMFunction_MathVectorDot","Position": [-1050.0,-250.0],"Inputs": {"A": "AxisD.Result","B": "AxisD.Result"}},"GetHandL": {"Unit": "RigUnit_GetTransform","Position": [-1800.0,700.0],"Defaults": {"Item": "Bone(hand_l)"}},"PalmL": {"Unit": "RigVMFunction_MathTransformTransformVector","Position": [-1500.0,700.0],"Defaults": {"Location": {"X": 9.07,"Y": -1.93,"Z": 0.14}},"Inputs": {"Transform": "GetHandL.Transform"}},"ToPalmL": {"Unit": "RigVMFunction_MathVectorSub","Position": [-1250.0,700.0],"Inputs": {"A": "PalmL.Result","B": "PaW.Result"}},"DotL": {"Unit": "RigVMFunction_MathVectorDot","Position": [-1050.0,700.0],"Inputs": {"A": "ToPalmL.Result","B": "AxisD.Result"}},"SlideL": {"Unit": "RigVMFunction_MathFloatDiv","Position": [-850.0,700.0],"Inputs": {"A": "DotL.Result","B": "DotDD.Result"}},"ClampL": {"Unit": "RigVMFunction_MathFloatClamp","Position": [-650.0,700.0],"Defaults": {"Maximum": 1.2,"Minimum": -0.38},"Inputs": {"Value": "SlideL.Result"}},"AlongL": {"Unit": "RigVMFunction_MathVectorScale","Position": [-450.0,700.0],"Inputs": {"Value": "AxisD.Result","Factor": "ClampL.Result"}},"OnShaftL": {"Unit": "RigVMFunction_MathVectorAdd","Position": [-250.0,700.0],"Inputs": {"A": "PaW.Result","B": "AlongL.Result"}},"DeltaL": {"Unit": "RigVMFunction_MathVectorSub","Position": [-50.0,800.0],"Inputs": {"A": "OnShaftL.Result","B": "PalmL.Result"}},"GapL": {"Unit": "RigVMFunction_MathVectorLength","Position": [150.0,900.0],"Inputs": {"Value": "DeltaL.Result"}},"AlphaL": {"Unit": "RigVMFunction_MathFloatRemap","Position": [350.0,900.0],"Defaults": {"SourceMaximum": 35.0,"TargetMinimum": 1.0,"bClamp": true,"TargetMaximum": 0.0,"SourceMinimum": 18.0},"Inputs": {"Value": "GapL.Result"}},"TargetL": {"Unit": "RigVMFunction_MathVectorAdd","Position": [350.0,700.0],"Inputs": {"A": "GetHandL.Transform.Translation","B": "DeltaL.Result"}},"PBIK_L": {"Unit": "RigUnit_PBIK","Position": [650.0,600.0],"Defaults": {"Root": "clavicle_l","BoneSettings": [],"ExcludedBones": [],"Effectors.0.Bone": "hand_l","Effectors.0.ChainDepth": 0,"Effectors.0.PinRotation": 0,"Effectors.0.PullChainAlpha": 0,"Effectors.0.RotationAlpha": 0,"Effectors.0.StrengthAlpha": 1,"Effectors.0.Transform.Scale3D": {"X": 1.0,"Y": 1.0,"Z": 1.0},"Effectors.0.Transform.Rotation": {"W": 1.0,"X": 0.0,"Y": 0.0,"Z": 0.0}},"Inputs": {"ExecutePin": "RigUnit_BeginExecution.ExecutePin","Effectors.0.Transform.Translation": "TargetL.Result","Effectors.0.PositionAlpha": "AlphaL.Result"},"Expanded": ["Effectors","Effectors.0","Effectors.0.Transform"]}},"Order": ["RigUnit_BeginExecution","GetHandL","PaW","PbW","AxisD","DotDD","GetGun","PalmL","ToPalmL","DotL","SlideL","ClampL","AlongL","OnShaftL","DeltaL","GapL","AlphaL","TargetL","PBIK_L"]})J";

TSharedPtr<FJsonObject> Json(const ANSICHAR* Text)
{
	return Parse(FString(UTF8_TO_TCHAR(Text)));
}

TSharedPtr<FJsonObject> ForTarget(const TSharedPtr<FJsonObject>& Document, const FTarget& Target)
{
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Document));
	Copy->SetStringField(TEXT("Target"), Target.PackageName);
	Copy->SetStringField(TEXT("Action"), TEXT("Update"));
	return Copy;
}

FString StatusName(const FAssetDocumentResult& Extracted)
{
	return Extracted.Compile.IsValid() ? Extracted.Compile->GetStringField(TEXT("Status")) : FString(TEXT("-"));
}

// Library functions of the rig whose function store (on the generated class: what reference nodes resolve through)
// has no entry.
TArray<FString> FunctionsMissingFromStore(UControlRigBlueprint& Blueprint)
{
	TArray<FString> Missing;
	URigVMBlueprint& RigVM = Blueprint;
	const URigVMFunctionLibrary* Library = RigVM.GetLocalFunctionLibrary();
	const IRigVMGraphFunctionHost* Host = Blueprint.GeneratedClass != nullptr ? Cast<IRigVMGraphFunctionHost>(Blueprint.GeneratedClass) : nullptr;
	const FRigVMGraphFunctionStore* Store = Host != nullptr ? Host->GetRigVMGraphFunctionStore() : nullptr;
	for (URigVMLibraryNode* Function : Library != nullptr ? Library->GetFunctions() : TArray<URigVMLibraryNode*>())
	{
		if (Store == nullptr || Store->FindFunction(Function->GetFunctionIdentifier()) == nullptr)
		{
			Missing.Add(Function->GetName());
		}
	}
	return Missing;
}

FString NamesText(const TArray<TSharedPtr<FJsonValue>>& Names)
{
	TArray<FString> Text;
	for (const TSharedPtr<FJsonValue>& Name : Names)
	{
		Text.Add(Name.IsValid() ? Name->AsString() : FString());
	}
	return FString::Join(Text, TEXT(","));
}

int32 CountFunctionReferences(UControlRigBlueprint& Blueprint)
{
	int32 Count = 0;
	URigVMBlueprint& RigVM = Blueprint;
	for (const URigVMGraph* Model : RigVM.GetAllModels())
	{
		for (const URigVMNode* Node : Model != nullptr ? Model->GetNodes() : TArray<URigVMNode*>())
		{
			Count += Node->IsA<URigVMFunctionReferenceNode>() ? 1 : 0;
		}
	}
	return Count;
}

// A rig with a public function that reads its member variable: a call from another rig maps it to one of that rig's
// variables; retyping that variable unmaps it, a compile error ("has unmapped variables") the apply finds only after
// every edit ran.
UControlRigBlueprint* MakeFunctionHost(FAutomationTestBase& Test, const FTarget& Target)
{
	UControlRigBlueprint* Rig = AssetDocumentControlRigTests::MakeControlRig(Test, Target, nullptr, false);
	if (Rig == nullptr)
	{
		return nullptr;
	}
	URigVMBlueprint& VM = *Rig;
	VM.AddMemberVariable(TEXT("Amount"), TEXT("float"), true, false, TEXT("1.0"));
	URigVMFunctionLibrary* Library = VM.GetLocalFunctionLibrary();
	URigVMController* LibraryController = Library != nullptr ? VM.GetOrCreateController(Library) : nullptr;
	URigVMLibraryNode* Function = LibraryController != nullptr
		? LibraryController->AddFunctionToLibrary(TEXT("ReadAmount"), false, FVector2D::ZeroVector, false) : nullptr;
	if (!Test.TestNotNull(TEXT("host function"), Function))
	{
		return nullptr;
	}
	URigVMController* Body = VM.GetOrCreateController(Function->GetContainedGraph());
	Body->AddExposedPin(TEXT("Out"), ERigVMPinDirection::Output, TEXT("float"), NAME_None, TEXT("0.0"), false);
	Body->AddVariableNode(TEXT("Amount"), TEXT("float"), nullptr, true, FString(), FVector2D(-200.0, 0.0), TEXT("GetAmount"), false);
	Test.TestTrue(TEXT("host function link"), Body->AddLink(TEXT("GetAmount.Value"), TEXT("Return.Out"), false));
	VM.MarkFunctionPublic(Function->GetFName(), true);
	FKismetEditorUtilities::CompileBlueprint(Rig);
	Test.TestTrue(TEXT("host saved"), Save(*Rig));
	return Rig;
}

// The duplicate of the template rig, its state, and checks that it has what the rig had (local functions called
// from the Rig graph).
UControlRigBlueprint* DuplicateFootIK(FAutomationTestBase& Test, const FTarget& Target)
{
	UObject* Source = LoadQuiet(FootIKPath);
	if (!Test.TestNotNull(TEXT("template rig CR_Mannequin_FootIK loads"), Source))
	{
		return nullptr;
	}
	UControlRigBlueprint* Copy = Cast<UControlRigBlueprint>(Duplicate(Test, Source, Target));
	if (!Test.TestNotNull(TEXT("duplicate"), Copy))
	{
		return nullptr;
	}
	URigVMBlueprint& RigVM = *Copy;
	Test.TestTrue(TEXT("the rig has local functions"), RigVM.GetLocalFunctionLibrary() != nullptr
		&& !RigVM.GetLocalFunctionLibrary()->GetFunctions().IsEmpty());
	Test.TestTrue(TEXT("its graphs call them"), CountFunctionReferences(*Copy) > 0);
	return Copy;
}

// The duplicate also calls the host's function from its Rig graph (CallHost), the host's variable mapped to its Gain.
bool CallHostFunction(FAutomationTestBase& Test, UControlRigBlueprint& Host, UControlRigBlueprint& Rig)
{
	URigVMBlueprint& HostVM = Host;
	URigVMLibraryNode* Function = HostVM.GetLocalFunctionLibrary() != nullptr
		? HostVM.GetLocalFunctionLibrary()->FindFunction(TEXT("ReadAmount")) : nullptr;
	URigVMBlueprint& VM = Rig;
	VM.AddMemberVariable(TEXT("Gain"), TEXT("float"), true, false, TEXT("1.0"));
	URigVMController* Controller = VM.GetOrCreateController(VM.GetDefaultModel());
	URigVMFunctionReferenceNode* Call = Function != nullptr && Controller != nullptr
		? Controller->AddFunctionReferenceNode(Function, FVector2D(900.0, 0.0), TEXT("CallHost"), false) : nullptr;
	if (!Test.TestNotNull(TEXT("call to the host's function"), Call))
	{
		return false;
	}
	Test.TestTrue(TEXT("host variable mapped"), UE::AssetDocument::Graph::Rig::ControlRig::SetFunctionVariableMapping(*Controller, *Call, TEXT("Amount"), TEXT("Gain"), false));
	FKismetEditorUtilities::CompileBlueprint(&Rig);
	return Test.TestTrue(TEXT("duplicate saved"), Save(Rig));
}

void ExpectRestored(FAutomationTestBase& Test, const FTarget& Target, const FString& Before, const FString& BeforeStatus, const FString& Stage)
{
	const FAssetDocumentResult After = ExtractResult(Target.ObjectPath);
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: the rig extracts as before"), *Stage), Serialize(After.Payload), Before))
	{
		Test.AddInfo(FirstDifference(Before, Serialize(After.Payload)));
	}
	Test.TestEqual(*FString::Printf(TEXT("%s: compile status as before"), *Stage), StatusName(After), BeforeStatus);
	if (UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Target.Get()))
	{
		const TArray<FString> Missing = FunctionsMissingFromStore(*Rig);
		Test.TestTrue(*FString::Printf(TEXT("%s: every local function is in the function store (missing: %s)"), *Stage,
			*FString::Join(Missing, TEXT(", "))), Missing.IsEmpty());
		Test.TestFalse(*FString::Printf(TEXT("%s: package clean"), *Stage), Rig->GetOutermost()->IsDirty());
	}
}
} // namespace AssetDocumentControlRigFunctionApplyTests

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigReplaceFunctionsTest, "UEAssetDoc.ControlRig.ReplaceFunctions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigReplaceFunctionsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigFunctionApplyTests;
	// CallHost reports its variable until it is mapped (setup).
	AssetDocumentIKRigTests::ExpectUnmappedVariableErrors(*this, 0);
	const TSharedPtr<AssetDocumentControlRigTests::FTemplateContent> Templates = AssetDocumentControlRigTests::ShareCorpusTemplates();
	FFixtures Fixtures;
	const FTarget& Host = Fixtures.Add(TEXT("CR_Host"));
	const FTarget& Target = Fixtures.Add(TEXT("CR_SpearGrip"));
	const FTarget& Second = Fixtures.Add(TEXT("CR_SpearGrip2"));
	UControlRigBlueprint* HostRig = MakeFunctionHost(*this, Host);
	UControlRigBlueprint* Rig = DuplicateFootIK(*this, Target);
	if (HostRig == nullptr || Rig == nullptr || !CallHostFunction(*this, *HostRig, *Rig))
	{
		return false;
	}

	// 1. Replace the functions and the graph (CallHost and Gain kept); the document also expands a pin that has no
	// sub-pins, which the editor ignores: the apply fails in verification, after every edit ran.
	const FAssetDocumentResult Before = ExtractResult(Target.ObjectPath);
	if (!TestTrue(TEXT("extract"), Before.IsSuccess() && Before.Payload.IsValid()))
	{
		return false;
	}
	const FString BeforeText = Serialize(Before.Payload);
	const FString BeforeStatus = StatusName(Before);
	AddInfo(FString::Printf(TEXT("duplicate compiles: %s"), *BeforeStatus));
	const TSharedPtr<FJsonObject> Replaced = ForTarget(Before.Payload, Target);
	BodyOf(Replaced)->SetObjectField(TEXT("Functions"), MakeShared<FJsonObject>());
	BodyOf(Replaced)->SetArrayField(TEXT("Variables"), TArray<TSharedPtr<FJsonValue>>());
	BodyOf(Replaced)->RemoveField(TEXT("GraphOrder"));
	const TSharedPtr<FJsonObject> Graphs = MakeShared<FJsonObject>();
	Graphs->SetObjectField(TEXT("Rig"), Json(SpearGripGraph));
	BodyOf(Replaced)->SetObjectField(TEXT("Graphs"), Graphs);
	const TSharedPtr<FJsonObject> Failing = Parse(Serialize(Replaced));
	const TSharedPtr<FJsonObject>* BeforeGraphs = nullptr;
	const TSharedPtr<FJsonObject>* BeforeRig = nullptr;
	const TSharedPtr<FJsonObject>* BeforeNodes = nullptr;
	const TSharedPtr<FJsonObject>* BeforeCall = nullptr;
	if (!TestTrue(TEXT("the duplicate's CallHost extracts"), BodyOf(Before.Payload)->TryGetObjectField(TEXT("Graphs"), BeforeGraphs)
		&& (*BeforeGraphs)->TryGetObjectField(TEXT("Rig"), BeforeRig) && (*BeforeRig)->TryGetObjectField(TEXT("Nodes"), BeforeNodes)
		&& (*BeforeNodes)->TryGetObjectField(TEXT("CallHost"), BeforeCall)))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> FailingNodes = BodyOf(Failing)->GetObjectField(TEXT("Graphs"))->GetObjectField(TEXT("Rig"))->GetObjectField(TEXT("Nodes"));
	FailingNodes->SetObjectField(TEXT("CallHost"), Parse(Serialize(*BeforeCall)));
	TArray<TSharedPtr<FJsonValue>> Leaf;
	Leaf.Add(MakeShared<FJsonValueString>(TEXT("Item.Name")));
	FailingNodes->GetObjectField(TEXT("GetGun"))->SetArrayField(TEXT("Expanded"), Leaf);
	BodyOf(Failing)->SetArrayField(TEXT("Variables"), BodyOf(Before.Payload)->GetArrayField(TEXT("Variables")));
	const FAssetDocumentResult Failed = Apply(Target, Failing);
	AddInfo(FString::Printf(TEXT("failing apply: %s"), *DiagnosticsText(Failed)));
	TestFalse(TEXT("the failing apply fails"), Failed.IsSuccess());
	ExpectRestored(*this, Target, BeforeText, BeforeStatus, TEXT("after the failed apply"));

	// 2. The corrected document (no CallHost, no variables) applies over the restored rig (which used to hit a RigVM assert
	// removing a variable in the second apply).
	const FAssetDocumentResult Applied = Apply(Target, Replaced);
	if (!TestTrue(TEXT("the corrected document applies"), Applied.IsSuccess()))
	{
		AddInfo(DiagnosticsText(Applied));
	}
	const FAssetDocumentResult AfterApply = ExtractResult(Target.ObjectPath);
	AddInfo(FString::Printf(TEXT("after apply: %s"), *StatusName(AfterApply)));
	TestTrue(TEXT("the rig compiles"), StatusName(AfterApply) == TEXT("UpToDate") || StatusName(AfterApply) == TEXT("UpToDateWithWarnings"));
	if (UControlRigBlueprint* Replacement = Cast<UControlRigBlueprint>(Target.Get()))
	{
		TestEqual(TEXT("no function reference left"), CountFunctionReferences(*Replacement), 0);
	}
	FString DiffText;
	TestEqual(TEXT("the document diffs clean"), DiffCount(Replaced, DiffText), 0);
	AddInfo(DiffText);

	// 3. The document as written on a fresh duplicate (no forced failure).
	if (DuplicateFootIK(*this, Second) != nullptr)
	{
		const FAssetDocumentResult SecondBefore = ExtractResult(Second.ObjectPath);
		const TSharedPtr<FJsonObject> AsWritten = ForTarget(Replaced, Second);
		const FAssetDocumentResult First = Apply(Second, AsWritten);
		AddInfo(FString::Printf(TEXT("document: %s"), *DiagnosticsText(First)));
		if (!First.IsSuccess())
		{
			ExpectRestored(*this, Second, Serialize(SecondBefore.Payload), StatusName(SecondBefore), TEXT("document refused"));
		}
		const FAssetDocumentResult Again = Apply(Second, AsWritten);
		AddInfo(FString::Printf(TEXT("document again: %s"), *DiagnosticsText(Again)));
		TestTrue(TEXT("the document applies (again)"), Again.IsSuccess());
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigRemoveCalledFunctionTest, "UEAssetDoc.ControlRig.RemoveCalledFunction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigRemoveCalledFunctionTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigFunctionApplyTests;
	// 0n: removing a function the kept graphs still call removes the nodes that call it, as the editor does (Graphs
	// left out: the graph is kept otherwise); no function node is left without its function.
	const TSharedPtr<AssetDocumentControlRigTests::FTemplateContent> Templates = AssetDocumentControlRigTests::ShareCorpusTemplates();
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("CR_Remove"));
	UControlRigBlueprint* Rig = DuplicateFootIK(*this, Target);
	if (Rig == nullptr)
	{
		return false;
	}
	const FAssetDocumentResult Before = ExtractResult(Target.ObjectPath);
	const TSharedPtr<FJsonObject> Removed = ForTarget(Before.Payload, Target);
	BodyOf(Removed)->SetObjectField(TEXT("Functions"), MakeShared<FJsonObject>());
	BodyOf(Removed)->RemoveField(TEXT("Graphs"));
	BodyOf(Removed)->RemoveField(TEXT("GraphOrder"));
	const FAssetDocumentResult Applied = Apply(Target, Removed);
	AddInfo(FString::Printf(TEXT("functions removed: %s"), *DiagnosticsText(Applied)));
	TestTrue(TEXT("the document applies"), Applied.IsSuccess());
	if (UControlRigBlueprint* After = Cast<UControlRigBlueprint>(Target.Get()))
	{
		TestEqual(TEXT("no function node is left"), CountFunctionReferences(*After), 0);
		URigVMBlueprint& VM = *After;
		TestTrue(TEXT("no local function is left"), VM.GetLocalFunctionLibrary() == nullptr || VM.GetLocalFunctionLibrary()->GetFunctions().IsEmpty());
	}
	const FAssetDocumentResult Extracted = ExtractResult(Target.ObjectPath);
	TestTrue(TEXT("the rig compiles"), StatusName(Extracted) == TEXT("UpToDate") || StatusName(Extracted) == TEXT("UpToDateWithWarnings"));
	FString DiffText;
	TestEqual(TEXT("the document diffs clean"), DiffCount(Removed, DiffText), 0);
	AddInfo(DiffText);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigUnmappedVariableFailsTest, "UEAssetDoc.ControlRig.UnmappedVariableFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigUnmappedVariableFailsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigFunctionApplyTests;
	// KI-10: on a rig whose editor graphs are bound (the template's FootIK duplicated), retyping the variable a call
	// to another rig's function is mapped to unmaps it ("Node @@ has unmapped variables"). The VM compiler reports it
	// during the compile; the apply fails on it (ControlRig.CompileFailed at the node) and is rolled back.
	AssetDocumentIKRigTests::ExpectUnmappedVariableErrors(*this, 0);
	const TSharedPtr<AssetDocumentControlRigTests::FTemplateContent> Templates = AssetDocumentControlRigTests::ShareCorpusTemplates();
	FFixtures Fixtures;
	const FTarget& Host = Fixtures.Add(TEXT("CR_UnmappedHost"));
	const FTarget& Target = Fixtures.Add(TEXT("CR_UnmappedUser"));
	UControlRigBlueprint* HostRig = MakeFunctionHost(*this, Host);
	UControlRigBlueprint* Rig = DuplicateFootIK(*this, Target);
	if (HostRig == nullptr || Rig == nullptr || !CallHostFunction(*this, *HostRig, *Rig))
	{
		return false;
	}
	const FAssetDocumentResult Before = ExtractResult(Target.ObjectPath);
	if (!TestTrue(TEXT("extract"), Before.IsSuccess() && Before.Payload.IsValid()))
	{
		return false;
	}
	const FString BeforeStatus = StatusName(Before);
	AddInfo(FString::Printf(TEXT("duplicate compiles: %s"), *BeforeStatus));
	TestTrue(TEXT("the duplicate compiles without errors"), BeforeStatus == TEXT("UpToDate") || BeforeStatus == TEXT("UpToDateWithWarnings"));

	// Graphs left out (kept as they are): the document does not map CallHost's variable to Gain again.
	const TSharedPtr<FJsonObject> Retyped = ForTarget(Before.Payload, Target);
	BodyOf(Retyped)->RemoveField(TEXT("Graphs"));
	BodyOf(Retyped)->RemoveField(TEXT("GraphOrder"));
	bool bRetyped = false;
	for (const TSharedPtr<FJsonValue>& Variable : BodyOf(Retyped)->GetArrayField(TEXT("Variables")))
	{
		const TSharedPtr<FJsonObject> Entry = Variable->AsObject();
		if (Entry.IsValid() && Entry->GetStringField(TEXT("Name")) == TEXT("Gain"))
		{
			Entry->SetStringField(TEXT("Type"), TEXT("FString"));
			Entry->RemoveField(TEXT("Default"));
			bRetyped = true;
		}
	}
	if (!TestTrue(TEXT("the document has Gain"), bRetyped))
	{
		return false;
	}
	const FAssetDocumentResult Failed = Apply(Target, Retyped);
	AddInfo(FString::Printf(TEXT("retyped Gain: %s"), *DiagnosticsText(Failed)));
	TestFalse(TEXT("the apply fails"), Failed.IsSuccess());
	TestTrue(TEXT("ControlRig.CompileFailed"), HasCode(Failed, TEXT("ControlRig.CompileFailed")));
	const FAssetDocumentDiagnostic* Compile = Failed.Diagnostics.FindByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == TEXT("ControlRig.CompileFailed");
	});
	TestTrue(TEXT("at the function node"), Compile != nullptr && Compile->Path == TEXT("/Body/Graphs/Rig/Nodes/CallHost"));
	TestTrue(TEXT("with the compiler's message"), Compile != nullptr && Compile->Message.Contains(TEXT("unmapped variables")));
	ExpectRestored(*this, Target, Serialize(Before.Payload), BeforeStatus, TEXT("after the failed apply"));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigOrderKeptTest, "UEAssetDoc.ControlRig.OrderKept",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigOrderKeptTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigFunctionApplyTests;
	// 0o: an Order that moves existing nodes (the editor cannot reorder them) applies: the nodes keep their place, the
	// apply says so (ControlRig.OrderKept, with the rig's order) and Order is not compared (no VerificationMismatch).
	const TSharedPtr<AssetDocumentControlRigTests::FTemplateContent> Templates = AssetDocumentControlRigTests::ShareCorpusTemplates();
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("CR_Order"));
	if (DuplicateFootIK(*this, Target) == nullptr)
	{
		return false;
	}
	const FAssetDocumentResult Before = ExtractResult(Target.ObjectPath);
	const TSharedPtr<FJsonObject>* Graphs = nullptr;
	const TSharedPtr<FJsonObject>* RigGraph = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Order = nullptr;
	if (!TestTrue(TEXT("the rig graph has an Order"), Before.IsSuccess() && BodyOf(Before.Payload)->TryGetObjectField(TEXT("Graphs"), Graphs)
		&& (*Graphs)->TryGetObjectField(TEXT("Rig"), RigGraph) && (*RigGraph)->TryGetArrayField(TEXT("Order"), Order) && Order->Num() > 1))
	{
		return false;
	}
	const FString BeforeOrder = NamesText(*Order);
	const TSharedPtr<FJsonObject> Reordered = ForTarget(Before.Payload, Target);
	TArray<TSharedPtr<FJsonValue>> Reversed = *Order;
	Algo::Reverse(Reversed);
	const TSharedPtr<FJsonObject> ReorderedRig = BodyOf(Reordered)->GetObjectField(TEXT("Graphs"))->GetObjectField(TEXT("Rig"));
	ReorderedRig->SetArrayField(TEXT("Order"), Reversed);
	// With an edit (a node moved): the apply runs and says the order is kept. Without one, the document is unchanged.
	const FString Moved = (*Order)[0]->AsString();
	TArray<TSharedPtr<FJsonValue>> Position;
	Position.Add(MakeShared<FJsonValueNumber>(-2000.0));
	Position.Add(MakeShared<FJsonValueNumber>(-1000.0));
	ReorderedRig->GetObjectField(TEXT("Nodes"))->GetObjectField(Moved)->SetArrayField(TEXT("Position"), Position);
	const FAssetDocumentResult Applied = Apply(Target, Reordered);
	AddInfo(FString::Printf(TEXT("reordered: %s"), *DiagnosticsText(Applied)));
	TestTrue(TEXT("the document applies"), Applied.IsSuccess());
	TestTrue(TEXT("ControlRig.OrderKept says the nodes keep their place"), HasCode(Applied, TEXT("ControlRig.OrderKept")));
	const FAssetDocumentResult After = ExtractResult(Target.ObjectPath);
	const TSharedPtr<FJsonObject>* AfterGraphs = nullptr;
	const TSharedPtr<FJsonObject>* AfterRig = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* AfterOrder = nullptr;
	TestTrue(TEXT("the nodes keep their order"), After.IsSuccess() && BodyOf(After.Payload)->TryGetObjectField(TEXT("Graphs"), AfterGraphs)
		&& (*AfterGraphs)->TryGetObjectField(TEXT("Rig"), AfterRig) && (*AfterRig)->TryGetArrayField(TEXT("Order"), AfterOrder)
		&& NamesText(*AfterOrder) == BeforeOrder);
	TestTrue(TEXT("the node moved"), Serialize(After.Payload).Contains(TEXT("[-2000.0,-1000.0]")) || Serialize(After.Payload).Contains(TEXT("[-2000,-1000]")));
	const FAssetDocumentResult Again = Apply(Target, Reordered);
	AddInfo(FString::Printf(TEXT("again: %s"), *DiagnosticsText(Again)));
	TestTrue(TEXT("applying it again changes nothing"), Again.IsSuccess() && !Again.bSavedAsset);
	FString DiffText;
	TestEqual(TEXT("Order is not compared"), DiffCount(Reordered, DiffText), 0);
	AddInfo(DiffText);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

namespace AssetDocumentControlRigFunctionApplyTests
{
const TCHAR* const TutorialSkeleton = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
const TCHAR* const TutorialIdle = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");

// An Animation Blueprint: idle -> Control Rig node (the rig class, Settings) -> output; Link adds a link into the node.
// The rig is named by class ("Node.ControlRigClass") or, with bRigReference, by asset reference
// ("Node.ControlRigAssetReference"); either spelling applies to the node of every engine.
TSharedPtr<FJsonObject> AnimDocument(const FTarget& Target, const FString& RigClass, const FString& Settings, const FString& Link,
	bool bRigReference = false)
{
	const FString Rig = bRigReference
		? FString::Printf(TEXT(R"json("Node.ControlRigAssetReference":{"BlueprintRigClass":"%s","ControlRigAsset":null})json"), *RigClass)
		: FString::Printf(TEXT(R"json("Node.ControlRigClass":"%s")json"), *RigClass);
	return Parse(FString::Printf(TEXT(R"json({"SchemaVersion":1,"Target":"%s","Class":"/Script/Engine.AnimBlueprint","Action":"CreateOrUpdate",
"Body":{"ParentClass":"/Script/Engine.AnimInstance","TargetSkeleton":"%s",
 "Variables":[{"Name":"Strength","Type":"float","DefaultValue":0.5}],
 "AnimGraph":{"Nodes":[
  {"Id":"Play","Action":"Anim:SequencePlayer","Properties":{"Node.Sequence":"%s"}},
  {"Id":"Rig","Action":"Anim:ControlRig","Properties":{%s%s}},
  {"Id":"GetStrength","Action":"K2:Get:Strength"}],
  "Links":[{"From":"Play:Pose","To":"Rig:Source"},{"From":"Rig:Pose","To":"Output:Result"}%s]}}})json"),
		*Target.PackageName, TutorialSkeleton, TutorialIdle, *Rig, *Settings, *Link));
}

// The Control Rig node's pin of that name in the Animation Blueprint's anim graph (null when it has none).
const UEdGraphPin* RigNodePin(const FTarget& Target, const TCHAR* Name)
{
	const UBlueprint* Blueprint = Cast<UBlueprint>(Target.Get());
	TArray<UEdGraph*> Graphs;
	if (Blueprint != nullptr)
	{
		Blueprint->GetAllGraphs(Graphs);
	}
	for (const UEdGraph* Graph : Graphs)
	{
		for (const UEdGraphNode* Node : Graph != nullptr ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
		{
			if (Node != nullptr && Node->GetClass()->GetName() == TEXT("AnimGraphNode_ControlRig"))
			{
				const UEdGraphPin* Pin = Node->FindPin(Name, EGPD_Input);
				return Pin != nullptr && !Pin->bHidden ? Pin : nullptr;
			}
		}
	}
	return nullptr;
}

bool AnimCompilesClean(const FTarget& Target)
{
	const UBlueprint* Blueprint = Cast<UBlueprint>(Target.Get());
	return Blueprint != nullptr && (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
}
} // namespace AssetDocumentControlRigFunctionApplyTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigAnimNodePinsTest, "UEAssetDoc.ControlRig.AnimNodePins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigAnimNodePinsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigFunctionApplyTests;
	// 0s: a public variable the rig gets after the Animation Blueprint's Control Rig node was made is exposed as an
	// input pin by "ShowPin.<Variable>" (Details > Input, "use pin"), linked in the same apply, and hidden again when the
	// document leaves the setting out.
	FFixtures Fixtures;
	const FTarget& RigTarget = Fixtures.Add(TEXT("CR_AnimPins"));
	const FTarget& Anim = Fixtures.Add(TEXT("ABP_RigPins"));
	UControlRigBlueprint* Rig = AssetDocumentControlRigTests::MakeControlRig(*this, RigTarget, nullptr, false);
	if (Rig == nullptr || Rig->GeneratedClass == nullptr)
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Rig);
	TestTrue(TEXT("rig saved"), Save(*Rig));
	const FString RigClass = Rig->GeneratedClass->GetPathName();

	// 1. The node before the rig has the variable.
	const TSharedPtr<FJsonObject> Plain = AnimDocument(Anim, RigClass, FString(), FString());
	const FAssetDocumentResult Made = Apply(Anim, Plain);
	if (!TestTrue(TEXT("the Animation Blueprint applies"), Made.IsSuccess()))
	{
		AddInfo(DiagnosticsText(Made));
		return false;
	}

	// 2. The rig gets a public variable (the editor's "+ Variable", Public); the node is not rebuilt.
	URigVMBlueprint& VM = *Rig;
	VM.AddMemberVariable(TEXT("Gain"), TEXT("float"), true, false, TEXT("1.0"));
	FKismetEditorUtilities::CompileBlueprint(Rig);
	TestTrue(TEXT("rig saved with Gain"), Save(*Rig));

	// 3. A link to the pin while it is not shown is refused, and the message says how to show it ("no pin
	// 'Gain'. Available: Source, Alpha").
	const FString Link = TEXT(",{\"From\":\"GetStrength:Strength\",\"To\":\"Rig:Gain\"}");
	const FAssetDocumentResult NotShown = Apply(Anim, AnimDocument(Anim, RigClass, FString(), Link));
	TestFalse(TEXT("a link to the hidden pin is refused"), NotShown.IsSuccess());
	TestTrue(TEXT("the refusal points at ShowPin"), DiagnosticsText(NotShown).Contains(TEXT("ShowPin.")));
	AddInfo(DiagnosticsText(NotShown));

	// 4. Expose it and link to it in one apply.
	const TSharedPtr<FJsonObject> Exposed = AnimDocument(Anim, RigClass, TEXT(",\"ShowPin.Gain\":true"), Link);
	const FAssetDocumentResult Shown = Apply(Anim, Exposed);
	if (!TestTrue(TEXT("ShowPin.Gain and the link apply"), Shown.IsSuccess()))
	{
		AddInfo(DiagnosticsText(Shown));
	}
	const UEdGraphPin* Gain = RigNodePin(Anim, TEXT("Gain"));
	TestTrue(TEXT("the node shows the Gain pin, linked"), Gain != nullptr && Gain->LinkedTo.Num() == 1);
	TestTrue(TEXT("the Animation Blueprint compiles"), AnimCompilesClean(Anim));
	const FString Extracted = Serialize(ExtractResult(Anim.ObjectPath).Payload);
	TestTrue(TEXT("extract reports ShowPin.Gain"), Extracted.Contains(TEXT("\"ShowPin.Gain\":true")));
	FString DiffText;
	TestEqual(TEXT("the document diffs clean"), DiffCount(Exposed, DiffText), 0);
	AddInfo(DiffText);
	const FAssetDocumentResult Again = Apply(Anim, Exposed);
	TestTrue(TEXT("applying it again changes nothing"), Again.IsSuccess() && !Again.bSavedAsset);

	// 5. Left out: the pin is hidden again (its default).
	const FAssetDocumentResult Hidden = Apply(Anim, Plain);
	if (!TestTrue(TEXT("omitting ShowPin.Gain applies"), Hidden.IsSuccess()))
	{
		AddInfo(DiagnosticsText(Hidden));
	}
	TestNull(TEXT("the Gain pin is hidden"), RigNodePin(Anim, TEXT("Gain")));
	TestEqual(TEXT("the plain document diffs clean"), DiffCount(Plain, DiffText), 0);
	AddInfo(DiffText);

	// 6. A name the rig does not have: the error lists the rig's public variables.
	const FAssetDocumentResult Missing = Apply(Anim, AnimDocument(Anim, RigClass, TEXT(",\"ShowPin.Gian\":true"), FString()));
	TestFalse(TEXT("an unknown variable is refused"), Missing.IsSuccess());
	TestTrue(TEXT("the error lists the rig's public variables"), DiagnosticsText(Missing).Contains(TEXT("public variables and controls: Gain")));
	AddInfo(DiagnosticsText(Missing));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentControlRigAnimNodeRigSpellingTest, "UEAssetDoc.ControlRig.AnimNodeRigSpelling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentControlRigAnimNodeRigSpellingTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentControlRigFunctionApplyTests;
	// Engines name the Control Rig node's rig by class (Node.ControlRigClass) or by asset reference
	// (Node.ControlRigAssetReference): a document in either spelling sets the node's rig on every engine.
	FFixtures Fixtures;
	const FTarget& RigTarget = Fixtures.Add(TEXT("CR_AnimSpelling"));
	UControlRigBlueprint* Rig = AssetDocumentControlRigTests::MakeControlRig(*this, RigTarget, nullptr, false);
	if (Rig == nullptr || Rig->GeneratedClass == nullptr)
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Rig);
	TestTrue(TEXT("rig saved"), Save(*Rig));
	const FString RigClass = Rig->GeneratedClass->GetPathName();
	// Extraction reports the spelling of this engine's node.
	const UScriptStruct* NodeStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/ControlRig.AnimNode_ControlRig"));
	if (!TestNotNull(TEXT("the Control Rig anim node struct"), NodeStruct))
	{
		return false;
	}
	const bool bNodeNamesClass = NodeStruct->FindPropertyByName(TEXT("ControlRigAssetReference")) == nullptr;
	const FString OwnKey = bNodeNamesClass ? TEXT("\"Node.ControlRigClass\"") : TEXT("\"Node.ControlRigAssetReference\"");
	const FString OtherKey = bNodeNamesClass ? TEXT("\"Node.ControlRigAssetReference\"") : TEXT("\"Node.ControlRigClass\"");

	for (const bool bRigReference : {false, true})
	{
		const FString Spelling = bRigReference ? TEXT("asset reference") : TEXT("class");
		const FTarget& Anim = Fixtures.Add(bRigReference ? TEXT("ABP_RigByReference") : TEXT("ABP_RigByClass"));
		const TSharedPtr<FJsonObject> Document = AnimDocument(Anim, RigClass, FString(), FString(), bRigReference);
		const FAssetDocumentResult Made = Apply(Anim, Document);
		if (!TestTrue(FString::Printf(TEXT("the rig named by %s applies"), *Spelling), Made.IsSuccess()))
		{
			AddInfo(DiagnosticsText(Made));
			continue;
		}
		TestTrue(FString::Printf(TEXT("by %s: the Animation Blueprint compiles"), *Spelling), AnimCompilesClean(Anim));
		const FString Extracted = Serialize(ExtractResult(Anim.ObjectPath).Payload);
		if (!TestTrue(FString::Printf(TEXT("by %s: extract names the rig in the node's own spelling"), *Spelling),
			Extracted.Contains(OwnKey) && !Extracted.Contains(OtherKey) && Extracted.Contains(RigClass)))
		{
			AddInfo(Extracted);
		}
		FString DiffText;
		TestEqual(FString::Printf(TEXT("by %s: the document diffs clean"), *Spelling), DiffCount(Document, DiffText), 0);
		AddInfo(DiffText);
		const FAssetDocumentResult Again = Apply(Anim, Document);
		TestTrue(FString::Printf(TEXT("by %s: applying it again changes nothing"), *Spelling), Again.IsSuccess() && !Again.bSavedAsset);
		const TSharedPtr<FJsonObject> OtherSpelling = AnimDocument(Anim, RigClass, FString(), FString(), !bRigReference);
		const FAssetDocumentResult Other = Apply(Anim, OtherSpelling);
		TestTrue(FString::Printf(TEXT("by %s: the other spelling changes nothing"), *Spelling), Other.IsSuccess() && !Other.bSavedAsset);
		AddInfo(DiagnosticsText(Other));
		TestEqual(FString::Printf(TEXT("by %s: the other spelling diffs clean"), *Spelling), DiffCount(OtherSpelling, DiffText), 0);
		AddInfo(DiffText);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
