// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Core package-snapshot rollback of a Blueprint through the registered
// derived-state rebuilder: after a failure in the middle of
// graph edits, or after a compile, the restored Blueprint compiles and is
// functionally identical to the pre-edit state (variables, defaults, components,
// graphs), and nothing created by the edit stays in the package.

#include "Tests/AssetDocumentGraphTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/SphereComponent.h"
#include "EdGraphSchema_K2.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"
#include "Utils/AssetDocumentEngineCompat.h"

namespace UE::AssetDocument::Graph::Tests
{
namespace GraphBlueprintRebuilderTestsLocal
{
const TCHAR* BeginPlayKey = TEXT("K2:Event:/Script/Engine.Actor:ReceiveBeginPlay");
const TCHAR* BranchKey = TEXT("K2:IfThenElse");
const TCHAR* PrintStringKey = TEXT("K2:CallFunction:/Script/Engine.KismetSystemLibrary:PrintString");

FString ExportValue(UObject* Object, FName PropertyName)
{
	FProperty* Property = Object ? FindFProperty<FProperty>(Object->GetClass(), PropertyName) : nullptr;
	FString Text;
	if (Property != nullptr)
	{
		Property->ExportText_InContainer(0, Text, Object, Object, Object, PPF_None);
	}
	return Property != nullptr ? Text : TEXT("<missing>");
}

bool ImportValue(UObject* Object, FName PropertyName, const TCHAR* Text)
{
	FProperty* Property = Object ? FindFProperty<FProperty>(Object->GetClass(), PropertyName) : nullptr;
	return Property != nullptr && Property->ImportText_InContainer(Text, Object, Object, PPF_None) != nullptr;
}

UObject* Defaults(UBlueprint& Blueprint)
{
	return Blueprint.GeneratedClass ? Blueprint.GeneratedClass->GetDefaultObject() : nullptr;
}

void Compile(UBlueprint& Blueprint)
{
	FKismetEditorUtilities::CompileBlueprint(&Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
}

// Order-insensitive functional fingerprint of everything a document extracts.
FString Fingerprint(UBlueprint& Blueprint)
{
	TArray<FString> Lines;
	Lines.Add(TEXT("parent=") + GetPathNameSafe(Blueprint.ParentClass));
	UObject* CDO = Defaults(Blueprint);
	for (const FBPVariableDescription& Variable : Blueprint.NewVariables)
	{
		Lines.Add(FString::Printf(TEXT("var %s %s:%s default=%s"), *Variable.VarName.ToString(),
			*Variable.VarType.PinCategory.ToString(), *Variable.VarType.PinSubCategory.ToString(),
			*ExportValue(CDO, Variable.VarName)));
	}
	Lines.Add(TEXT("InitialLifeSpan=") + ExportValue(CDO, TEXT("InitialLifeSpan")));
	if (USimpleConstructionScript* SCS = Blueprint.SimpleConstructionScript)
	{
		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			const USCS_Node* Parent = SCS->FindParentNode(Node);
			Lines.Add(FString::Printf(TEXT("component %s %s parent=%s radius=%s"), *Node->GetVariableName().ToString(),
				*GetNameSafe(Node->ComponentClass), Parent ? *Parent->GetVariableName().ToString() : TEXT("-"),
				Node->ComponentTemplate && Node->ComponentTemplate->IsA<USphereComponent>()
					? *ExportValue(Node->ComponentTemplate, TEXT("SphereRadius")) : TEXT("-")));
		}
	}
	TArray<UEdGraph*> Graphs;
	Blueprint.GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			Lines.Add(FString::Printf(TEXT("node %s/%s %s"), *Graph->GetName(), *Node->NodeGuid.ToString(), *Node->GetClass()->GetName()));
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin->DefaultValue.IsEmpty() && Pin->Direction == EGPD_Input)
				{
					Lines.Add(FString::Printf(TEXT("default %s.%s=%s"), *Node->NodeGuid.ToString(), *Pin->PinName.ToString(), *Pin->DefaultValue));
				}
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (Pin->Direction == EGPD_Output && Linked && Linked->GetOwningNodeUnchecked())
					{
						Lines.Add(FString::Printf(TEXT("link %s.%s->%s.%s"), *Node->NodeGuid.ToString(), *Pin->PinName.ToString(),
							*Linked->GetOwningNode()->NodeGuid.ToString(), *Linked->PinName.ToString()));
					}
				}
			}
		}
	}
	Lines.Sort();
	return FString::Join(Lines, TEXT("\n"));
}

// Baseline: Health = 100, InitialLifeSpan = 5, Sensor sphere (radius 250) under the
// default scene root, BeginPlay -> PrintString("Before").
bool BuildBaseline(FAutomationTestBase& Test, UBlueprint& Blueprint)
{
	FEdGraphPinType Real;
	Real.PinCategory = UEdGraphSchema_K2::PC_Real;
	Real.PinSubCategory = UEdGraphSchema_K2::PC_Double;
	FBlueprintEditorUtils::AddMemberVariable(&Blueprint, TEXT("Health"), Real);
	USCS_Node* Sensor = Blueprint.SimpleConstructionScript->CreateNode(USphereComponent::StaticClass(), TEXT("Sensor"));
	Blueprint.SimpleConstructionScript->GetDefaultSceneRootNode()->AddChildNode(Sensor);
	ImportValue(Sensor->ComponentTemplate, TEXT("SphereRadius"), TEXT("250.0"));
	Compile(Blueprint);
	FSession Session(Test, Blueprint);
	const FString BeginPlay = Session.IsReady() ? Str(Session.CallOrNull(TEXT("graph.node.spawn.v1"),
		FString::Printf(TEXT("{\"graph\":%s,\"action_key\":\"%s\"}"), *GraphJson(TEXT("EventGraph")), BeginPlayKey)), TEXT("node_guid")) : FString();
	const FString Print = Session.IsReady() ? Str(Session.CallOrNull(TEXT("graph.node.spawn.v1"),
		FString::Printf(TEXT("{\"graph\":%s,\"action_key\":\"%s\",\"position\":{\"x\":320,\"y\":0}}"), *GraphJson(TEXT("EventGraph")), PrintStringKey)), TEXT("node_guid")) : FString();
	if (BeginPlay.IsEmpty() || Print.IsEmpty())
	{
		return false;
	}
	Session.CallOrNull(TEXT("graph.pin.connect.v1"), FString::Printf(TEXT("{\"from\":%s,\"to\":%s}"),
		*PinJson(BeginPlay, TEXT("then"), TEXT("output")), *PinJson(Print, TEXT("execute"), TEXT("input"))));
	Session.CallOrNull(TEXT("graph.pin.default.set.v1"), FString::Printf(TEXT("{\"pin\":%s,\"kind\":\"value\",\"value\":\"Before\"}"),
		*PinJson(Print, TEXT("InString"), TEXT("input"))));
	Compile(Blueprint);
	return Test.TestTrue(TEXT("baseline Health default"), ImportValue(Defaults(Blueprint), TEXT("Health"), TEXT("100.0")))
		&& Test.TestTrue(TEXT("baseline InitialLifeSpan"), ImportValue(Defaults(Blueprint), TEXT("InitialLifeSpan"), TEXT("5.0")))
		&& Test.TestTrue(TEXT("baseline compiles"), Blueprint.Status == BS_UpToDate || Blueprint.Status == BS_UpToDateWithWarnings);
}

// Structural, graph and default edits as an apply would make them; bCompile
// simulates a failure after the compile/repair boundary.
void EditBlueprint(FAutomationTestBase& Test, UBlueprint& Blueprint, bool bCompile)
{
	FEdGraphPinType Bool;
	Bool.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	FBlueprintEditorUtils::AddMemberVariable(&Blueprint, TEXT("Stamina"), Bool);
	USimpleConstructionScript* SCS = Blueprint.SimpleConstructionScript;
	for (USCS_Node* Node : SCS->GetAllNodes())
	{
		if (Node->GetVariableName() == TEXT("Sensor"))
		{
			SCS->RemoveNodeAndPromoteChildren(Node);
			break;
		}
	}
	USCS_Node* Vision = SCS->CreateNode(USphereComponent::StaticClass(), TEXT("Vision"));
	SCS->GetDefaultSceneRootNode()->AddChildNode(Vision);
	ImportValue(Defaults(Blueprint), TEXT("Health"), TEXT("50.0"));
	ImportValue(Defaults(Blueprint), TEXT("InitialLifeSpan"), TEXT("9.0"));
	FSession Session(Test, Blueprint);
	const FValue Graph = Session.CallOrNull(TEXT("graph.query.v1"), FString::Printf(TEXT("{\"graphs\":[%s]}"), *GraphJson(TEXT("EventGraph"))));
	const FValue* EventGraph = FindGraph(Graph, TEXT("EventGraph"));
	const FValue* Print = EventGraph ? FindNode(*EventGraph, TEXT("action_key"), PrintStringKey) : nullptr;
	const FString Branch = Str(Session.CallOrNull(TEXT("graph.node.spawn.v1"),
		FString::Printf(TEXT("{\"graph\":%s,\"action_key\":\"%s\",\"position\":{\"x\":640,\"y\":0}}"), *GraphJson(TEXT("EventGraph")), BranchKey)), TEXT("node_guid"));
	if (Print != nullptr && !Branch.IsEmpty())
	{
		Session.CallOrNull(TEXT("graph.pin.connect.v1"), FString::Printf(TEXT("{\"from\":%s,\"to\":%s}"),
			*PinJson(Str(*Print, TEXT("node_guid")), TEXT("then"), TEXT("output")), *PinJson(Branch, TEXT("execute"), TEXT("input"))));
		Session.CallOrNull(TEXT("graph.pin.default.set.v1"), FString::Printf(TEXT("{\"pin\":%s,\"kind\":\"value\",\"value\":\"After\"}"),
			*PinJson(Str(*Print, TEXT("node_guid")), TEXT("InString"), TEXT("input"))));
	}
	if (bCompile)
	{
		Compile(Blueprint);
	}
}

void ExpectRestored(FAutomationTestBase& Test, const TCHAR* Label, UBlueprint& Blueprint, const FString& Before)
{
	Test.TestEqual(FString::Printf(TEXT("%s: Blueprint is functionally identical"), Label), Fingerprint(Blueprint), Before);
	Test.TestTrue(FString::Printf(TEXT("%s: Blueprint compiles"), Label),
		Blueprint.Status == BS_UpToDate || Blueprint.Status == BS_UpToDateWithWarnings);
	Test.TestNull(FString::Printf(TEXT("%s: generated class lost the added variable"), Label),
		FindFProperty<FProperty>(Blueprint.GeneratedClass, TEXT("Stamina")));
	Test.TestNotNull(FString::Printf(TEXT("%s: generated class keeps Health"), Label),
		FindFProperty<FProperty>(Blueprint.GeneratedClass, TEXT("Health")));
	bool bResidue = false;
	ForEachObjectWithPackage(Blueprint.GetOutermost(), [&bResidue](UObject* Object)
	{
		bResidue |= Object->GetName().StartsWith(TEXT("Vision"));
		return true;
	}, AssetDocumentObjects::IncludeNested, RF_NoFlags, EInternalObjectFlags::Garbage);
	Test.TestFalse(FString::Printf(TEXT("%s: no object created by the edit stays in the package"), Label), bResidue);
}

void RunScenario(FAutomationTestBase& Test, const TCHAR* Label, bool bCompile)
{
	FTestBlueprint Blueprint(Label);
	if (!Test.TestNotNull(TEXT("test Blueprint"), Blueprint.Get()) || !BuildBaseline(Test, *Blueprint.Get()))
	{
		return;
	}
	UPackage& Package = *Blueprint->GetOutermost();
	Test.TestTrue(FString::Printf(TEXT("%s: a Blueprint package is restorable"), Label), FAssetDocumentPackageSnapshot::CanRestorePackage(Package));
	const FString Before = Fingerprint(*Blueprint.Get());
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	TUniquePtr<FAssetDocumentPackageSnapshot> Snapshot = FAssetDocumentPackageSnapshot::Capture(Package, Diagnostics);
	if (!Test.TestNotNull(FString::Printf(TEXT("%s: snapshot captured"), Label), Snapshot.Get()))
	{
		return;
	}
	EditBlueprint(Test, *Blueprint.Get(), bCompile);
	Test.TestNotEqual(FString::Printf(TEXT("%s: the edit changed the Blueprint"), Label), Fingerprint(*Blueprint.Get()), Before);
	const FAssetDocumentContractValidationResult Restored = Snapshot->Restore();
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Restored.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: %s %s"), Label, *Diagnostic.Code, *Diagnostic.Message));
	}
	Test.TestTrue(FString::Printf(TEXT("%s: restore succeeded"), Label), Restored.bValid);
	ExpectRestored(Test, Label, *Blueprint.Get(), Before);
	// The restored Blueprint keeps working: a later compile is still clean.
	Compile(*Blueprint.Get());
	Test.TestEqual(FString::Printf(TEXT("%s: stable after another compile"), Label), Fingerprint(*Blueprint.Get()), Before);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphBlueprintRebuilderTest,
	"UEAssetDoc.Graph.BlueprintRebuilder.RestoresEditedBlueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphBlueprintRebuilderTest::RunTest(const FString& Parameters)
{
	using namespace GraphBlueprintRebuilderTestsLocal;
	RunScenario(*this, TEXT("MidGraphEdit"), /*bCompile=*/false);
	RunScenario(*this, TEXT("AfterCompile"), /*bCompile=*/true);
	return true;
}
} // namespace UE::AssetDocument::Graph::Tests

#endif // WITH_DEV_AUTOMATION_TESTS
