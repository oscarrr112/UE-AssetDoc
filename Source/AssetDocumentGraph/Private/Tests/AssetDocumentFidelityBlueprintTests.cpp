// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Editor-made Blueprints holding Details-panel state of functions, macros, variables, components, custom events and
// nodes: extract shows it, the extract re-applies as a no-op, and an edit applies it as the Details panel does.

#include "AssetDocumentTolerantTestSupport.h"

#include "BlueprintComponentNodeSpawner.h"
#include "Components/ChildActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "ComponentTypeRegistry.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/EngineTypes.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "K2Node_AddComponent.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_SwitchString.h"
#include "K2Node_Tunnel.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentFidelityBlueprintTests
{
// The Body.<Section> entry whose Name is Name (nullptr if none).
TSharedPtr<FJsonObject> FindNamed(const TSharedPtr<FJsonObject>& Document, const TCHAR* Section, const FString& Name)
{
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (!Document.IsValid() || !Document->TryGetObjectField(TEXT("Body"), Body) || !(*Body)->TryGetArrayField(Section, Rows))
	{
		return nullptr;
	}
	for (const TSharedPtr<FJsonValue>& Row : *Rows)
	{
		FString Found;
		if (Row->Type == EJson::Object && Row->AsObject()->TryGetStringField(TEXT("Name"), Found) && Found == Name)
		{
			return Row->AsObject();
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> SignatureOf(const TSharedPtr<FJsonObject>& Document, const TCHAR* Section, const FString& Name)
{
	const TSharedPtr<FJsonObject> Graph = FindNamed(Document, Section, Name);
	const TSharedPtr<FJsonObject>* Signature = nullptr;
	return Graph.IsValid() && Graph->TryGetObjectField(TEXT("Signature"), Signature) ? *Signature : nullptr;
}

UBlueprint* MakeActorBlueprint(const AssetDocumentTolerantTestSupport::FTarget& Target)
{
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*Target.PackageName),
		FName(*Target.AssetName), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (Blueprint != nullptr)
	{
		FAssetRegistryModule::AssetCreated(Blueprint);
	}
	return Blueprint;
}

UK2Node_FunctionEntry* FunctionEntry(UBlueprint* Blueprint, FName Name)
{
	const TObjectPtr<UEdGraph>* Graph = Blueprint != nullptr ? Blueprint->FunctionGraphs.FindByPredicate([Name](const UEdGraph* Item) { return Item->GetFName() == Name; }) : nullptr;
	TArray<UK2Node_FunctionEntry*> Entries;
	if (Graph != nullptr)
	{
		(*Graph)->GetNodesOfClass(Entries);
	}
	return Entries.Num() > 0 ? Entries[0] : nullptr;
}

// The node named Name in the graph named Graph of Body.<Section> (nullptr if none).
TSharedPtr<FJsonObject> FindNode(const TSharedPtr<FJsonObject>& Document, const TCHAR* Section, const FString& Graph, const FString& Name)
{
	const TSharedPtr<FJsonObject> GraphRow = FindNamed(Document, Section, Graph);
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (!GraphRow.IsValid() || !GraphRow->TryGetArrayField(TEXT("Nodes"), Nodes))
	{
		return nullptr;
	}
	for (const TSharedPtr<FJsonValue>& Node : *Nodes)
	{
		FString Found;
		if (Node->Type == EJson::Object && Node->AsObject()->TryGetStringField(TEXT("Name"), Found) && Found == Name)
		{
			return Node->AsObject();
		}
	}
	return nullptr;
}

UK2Node_CustomEvent* FindCustomEvent(UBlueprint* Blueprint, FName Name)
{
	TArray<UK2Node_CustomEvent*> Events;
	if (Blueprint != nullptr)
	{
		FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Events);
	}
	UK2Node_CustomEvent* const* Found = Events.FindByPredicate([Name](const UK2Node_CustomEvent* Event) { return Event->CustomFunctionName == Name; });
	return Found != nullptr ? *Found : nullptr;
}

UK2Node_CustomEvent* AddCustomEvent(UEdGraph& Graph, FName Name, int32 PositionY)
{
	FGraphNodeCreator<UK2Node_CustomEvent> Creator(Graph);
	UK2Node_CustomEvent* Event = Creator.CreateNode();
	Event->CustomFunctionName = Name;
	Event->NodePosY = PositionY;
	Creator.Finalize();
	return Event;
}

FKismetUserDeclaredFunctionMetadata* MacroMetadata(UBlueprint* Blueprint, FName Name)
{
	const TObjectPtr<UEdGraph>* Graph = Blueprint != nullptr ? Blueprint->MacroGraphs.FindByPredicate([Name](const UEdGraph* Item) { return Item->GetFName() == Name; }) : nullptr;
	return Graph != nullptr ? FBlueprintEditorUtils::GetGraphFunctionMetaData(*Graph) : nullptr;
}

// The event graph node of Document whose Position.Y is PositionY (nullptr if none).
TSharedPtr<FJsonObject> FindNodeAtY(const TSharedPtr<FJsonObject>& Document, int32 PositionY)
{
	const TSharedPtr<FJsonObject> GraphRow = FindNamed(Document, TEXT("UbergraphPages"), TEXT("EventGraph"));
	for (const TSharedPtr<FJsonValue>& Node : GraphRow.IsValid() ? GraphRow->GetArrayField(TEXT("Nodes")) : TArray<TSharedPtr<FJsonValue>>())
	{
		const TSharedPtr<FJsonObject>* Position = nullptr;
		if (Node->AsObject()->TryGetObjectField(TEXT("Position"), Position) && FMath::RoundToInt((*Position)->GetNumberField(TEXT("Y"))) == PositionY)
		{
			return Node->AsObject();
		}
	}
	return nullptr;
}

// The event graph node of Blueprint at PositionY (nullptr if none).
UEdGraphNode* NodeAtY(UBlueprint* Blueprint, int32 PositionY)
{
	UEdGraph* Graph = Blueprint != nullptr ? FBlueprintEditorUtils::FindEventGraph(Blueprint) : nullptr;
	const TObjectPtr<UEdGraphNode>* Found = Graph != nullptr
		? Graph->Nodes.FindByPredicate([PositionY](const UEdGraphNode* Node) { return Node != nullptr && Node->NodePosY == PositionY; }) : nullptr;
	return Found != nullptr ? Found->Get() : nullptr;
}

// Applies Document as a Create of Fresh (a copy of the extracted asset onto a new one).
FAssetDocumentResult CreateCopy(FAutomationTestBase& Test, const AssetDocumentTolerantTestSupport::FTarget& Fresh,
	const TSharedPtr<FJsonObject>& Document, const FString& Stage)
{
	using namespace AssetDocumentTolerantTestSupport;
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Document));
	Copy->SetStringField(TEXT("Target"), Fresh.PackageName);
	Copy->SetStringField(TEXT("Action"), TEXT("Create"));
	return ApplyText(Test, Fresh, Serialize(Copy), Stage);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityBlueprintGraphDetailsTest, "UEAssetDoc.Fidelity.Blueprint.GraphDetails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// A function marked Thread Safe, Exec and Deprecated with a Compact Node Title and an Instance Color, and a macro
// filed under a Category with a Description, Keywords and a Compact Node Title, as the Details panel sets them.
bool FAssetDocumentFidelityBlueprintGraphDetailsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelityBlueprintTests;
	const FTarget Target(TEXT("BP_GraphDetails"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UBlueprint* Blueprint = MakeActorBlueprint(Target);
	if (!TestNotNull(TEXT("blueprint created"), Blueprint))
	{
		return false;
	}
	UEdGraph* Function = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("GetDamage"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Function, /*bIsUserCreated=*/true, nullptr);
	UEdGraph* Macro = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("ForEachWithDelay"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddMacroGraph(Blueprint, Macro, /*bIsUserCreated=*/true, nullptr);
	UK2Node_FunctionEntry* Entry = FunctionEntry(Blueprint, TEXT("GetDamage"));
	FKismetUserDeclaredFunctionMetadata* MacroData = MacroMetadata(Blueprint, TEXT("ForEachWithDelay"));
	if (!TestNotNull(TEXT("function entry"), Entry) || !TestNotNull(TEXT("macro metadata"), MacroData))
	{
		return false;
	}
	Entry->MetaData.bThreadSafe = true;
	Entry->MetaData.bIsDeprecated = true;
	Entry->MetaData.DeprecationMessage = TEXT("Use GetDamageV2");
	Entry->MetaData.CompactNodeTitle = FText::FromString(TEXT("DMG"));
	Entry->MetaData.InstanceTitleColor = FLinearColor(1.0f, 0.0f, 0.0f, 1.0f);
	Entry->SetExtraFlags(Entry->GetExtraFlags() | FUNC_Exec);
	FBlueprintEditorUtils::SetBlueprintFunctionOrMacroCategory(Macro, FText::FromString(TEXT("Flow")), /*bDontRecompile=*/true);
	MacroData->ToolTip = FText::FromString(TEXT("Loops with a delay"));
	MacroData->Keywords = FText::FromString(TEXT("loop wait"));
	MacroData->CompactNodeTitle = FText::FromString(TEXT("EACH"));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Graph details"));
	const TSharedPtr<FJsonObject> FunctionSignature = SignatureOf(Extracted, TEXT("FunctionGraphs"), TEXT("GetDamage"));
	const TSharedPtr<FJsonObject> MacroSignature = SignatureOf(Extracted, TEXT("MacroGraphs"), TEXT("ForEachWithDelay"));
	if (!TestTrue(TEXT("function signature extracted"), FunctionSignature.IsValid())
		|| !TestTrue(TEXT("macro signature extracted"), MacroSignature.IsValid()))
	{
		return false;
	}
	bool bFlag = false;
	TestTrue(TEXT("ThreadSafe extracted"), FunctionSignature->TryGetBoolField(TEXT("ThreadSafe"), bFlag) && bFlag);
	TestTrue(TEXT("Exec extracted"), FunctionSignature->TryGetBoolField(TEXT("Exec"), bFlag) && bFlag);
	TestTrue(TEXT("Deprecated extracted"), FunctionSignature->TryGetBoolField(TEXT("Deprecated"), bFlag) && bFlag);
	TestEqual(TEXT("DeprecationMessage extracted"), FunctionSignature->GetStringField(TEXT("DeprecationMessage")), FString(TEXT("Use GetDamageV2")));
	TestEqual(TEXT("CompactNodeTitle extracted"), FunctionSignature->GetStringField(TEXT("CompactNodeTitle")), FString(TEXT("DMG")));
	TestTrue(TEXT("InstanceColor extracted"), FunctionSignature->HasTypedField<EJson::Object>(TEXT("InstanceColor")));
	TestEqual(TEXT("macro Category extracted"), MacroSignature->GetStringField(TEXT("Category")), FString(TEXT("Flow")));
	TestEqual(TEXT("macro Tooltip extracted"), MacroSignature->GetStringField(TEXT("Tooltip")), FString(TEXT("Loops with a delay")));
	TestEqual(TEXT("macro Keywords extracted"), MacroSignature->GetStringField(TEXT("Keywords")), FString(TEXT("loop wait")));
	TestEqual(TEXT("macro CompactNodeTitle extracted"), MacroSignature->GetStringField(TEXT("CompactNodeTitle")), FString(TEXT("EACH")));

	// Clear Thread Safe, leave Compact Node Title out (kept) and refile the macro.
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		if (const TSharedPtr<FJsonObject> Signature = SignatureOf(Document, TEXT("FunctionGraphs"), TEXT("GetDamage")))
		{
			Signature->SetBoolField(TEXT("ThreadSafe"), false);
			Signature->RemoveField(TEXT("CompactNodeTitle"));
		}
		if (const TSharedPtr<FJsonObject> Signature = SignatureOf(Document, TEXT("MacroGraphs"), TEXT("ForEachWithDelay")))
		{
			Signature->SetStringField(TEXT("Category"), TEXT("Loops"));
			Signature->SetStringField(TEXT("CompactNodeTitle"), TEXT("EACH+"));
		}
	}, TEXT("Edit graph details"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		UBlueprint* After = Target.Get<UBlueprint>();
		UK2Node_FunctionEntry* AfterEntry = FunctionEntry(After, TEXT("GetDamage"));
		FKismetUserDeclaredFunctionMetadata* AfterMacro = MacroMetadata(After, TEXT("ForEachWithDelay"));
		if (TestNotNull(TEXT("entry after"), AfterEntry) && TestNotNull(TEXT("macro after"), AfterMacro))
		{
			TestFalse(TEXT("Thread Safe cleared"), AfterEntry->MetaData.bThreadSafe);
			TestEqual(TEXT("omitted Compact Node Title kept"), AfterEntry->MetaData.CompactNodeTitle.ToString(), FString(TEXT("DMG")));
			TestTrue(TEXT("Exec kept"), (AfterEntry->GetFunctionFlags() & FUNC_Exec) != 0);
			TestEqual(TEXT("macro refiled"), AfterMacro->Category.ToString(), FString(TEXT("Loops")));
			TestEqual(TEXT("macro compact title set"), AfterMacro->CompactNodeTitle.ToString(), FString(TEXT("EACH+")));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityBlueprintVariableDetailsTest, "UEAssetDoc.Fidelity.Blueprint.VariableDetails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// A variable made RepNotify with an Owner Only condition, SaveGame, and a slider range, as the Details panel sets them.
bool FAssetDocumentFidelityBlueprintVariableDetailsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelityBlueprintTests;
	const FTarget Target(TEXT("BP_VariableDetails"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UBlueprint* Blueprint = MakeActorBlueprint(Target);
	if (!TestNotNull(TEXT("blueprint created"), Blueprint))
	{
		return false;
	}
	FEdGraphPinType FloatType;
	FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
	FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
	if (!TestTrue(TEXT("variable added"), FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Health"), FloatType)))
	{
		return false;
	}
	// Replication: RepNotify (FBlueprintVarActionDetails::OnChangeReplication creates OnRep_Health).
	UEdGraph* OnRep = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("OnRep_Health"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, OnRep, /*bIsUserCreated=*/false, nullptr);
	FBlueprintEditorUtils::SetBlueprintVariableRepNotifyFunc(Blueprint, TEXT("Health"), TEXT("OnRep_Health"));
	const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("Health"));
	Blueprint->NewVariables[Index].PropertyFlags |= CPF_Net | CPF_RepNotify;
	Blueprint->NewVariables[Index].ReplicationCondition = COND_OwnerOnly;
	FBlueprintEditorUtils::SetVariableSaveGameFlag(Blueprint, TEXT("Health"), true);
	FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, TEXT("Health"), nullptr, TEXT("ClampMin"), TEXT("0"));
	FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, TEXT("Health"), nullptr, TEXT("UIMax"), TEXT("100"));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Variable details"));
	const TSharedPtr<FJsonObject> Row = FindNamed(Extracted, TEXT("Variables"), TEXT("Health"));
	if (!TestTrue(TEXT("variable extracted"), Row.IsValid()))
	{
		return false;
	}
	bool bFlag = false;
	TestEqual(TEXT("Replication extracted"), Row->GetStringField(TEXT("Replication")), FString(TEXT("RepNotify")));
	TestEqual(TEXT("RepNotifyFunc extracted"), Row->GetStringField(TEXT("RepNotifyFunc")), FString(TEXT("OnRep_Health")));
	TestEqual(TEXT("ReplicationCondition extracted"), Row->GetStringField(TEXT("ReplicationCondition")), FString(TEXT("OwnerOnly")));
	TestTrue(TEXT("SaveGame extracted"), Row->TryGetBoolField(TEXT("SaveGame"), bFlag) && bFlag);
	const TSharedPtr<FJsonObject>* Metadata = nullptr;
	TestTrue(TEXT("Metadata extracted"), Row->TryGetObjectField(TEXT("Metadata"), Metadata)
		&& (*Metadata)->GetStringField(TEXT("ClampMin")) == TEXT("0") && (*Metadata)->GetStringField(TEXT("UIMax")) == TEXT("100"));

	// Skip Owner, no SaveGame, the slider's minimum removed; the replication itself is left out (kept).
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		if (const TSharedPtr<FJsonObject> Variable = FindNamed(Document, TEXT("Variables"), TEXT("Health")))
		{
			Variable->RemoveField(TEXT("Replication"));
			Variable->RemoveField(TEXT("RepNotifyFunc"));
			Variable->SetStringField(TEXT("ReplicationCondition"), TEXT("SkipOwner"));
			Variable->SetBoolField(TEXT("SaveGame"), false);
			const TSharedPtr<FJsonObject> Kept = MakeShared<FJsonObject>();
			Kept->SetStringField(TEXT("UIMax"), TEXT("100"));
			Variable->SetObjectField(TEXT("Metadata"), Kept);
		}
	}, TEXT("Edit variable details"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		UBlueprint* After = Target.Get<UBlueprint>();
		const int32 AfterIndex = After != nullptr ? FBlueprintEditorUtils::FindNewVariableIndex(After, TEXT("Health")) : INDEX_NONE;
		if (TestTrue(TEXT("variable after"), AfterIndex != INDEX_NONE))
		{
			const FBPVariableDescription& Variable = After->NewVariables[AfterIndex];
			TestTrue(TEXT("RepNotify kept"), (Variable.PropertyFlags & CPF_RepNotify) != 0 && Variable.RepNotifyFunc == TEXT("OnRep_Health"));
			TestEqual(TEXT("condition changed"), static_cast<int32>(Variable.ReplicationCondition), static_cast<int32>(COND_SkipOwner));
			TestEqual(TEXT("SaveGame cleared"), Variable.PropertyFlags & CPF_SaveGame, static_cast<uint64>(0));
			TestFalse(TEXT("ClampMin removed"), Variable.HasMetaData(TEXT("ClampMin")));
			TestTrue(TEXT("UIMax kept"), Variable.HasMetaData(TEXT("UIMax")));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityBlueprintComponentDetailsTest, "UEAssetDoc.Fidelity.Blueprint.ComponentDetails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// A component filed under a Category with a Tooltip, and a DefaultSceneRoot whose Mobility the user set to Static.
bool FAssetDocumentFidelityBlueprintComponentDetailsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelityBlueprintTests;
	const FTarget Target(TEXT("BP_ComponentDetails"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UBlueprint* Blueprint = MakeActorBlueprint(Target);
	USimpleConstructionScript* SCS = Blueprint != nullptr ? Blueprint->SimpleConstructionScript.Get() : nullptr;
	USCS_Node* Root = SCS != nullptr ? SCS->GetDefaultSceneRootNode() : nullptr;
	if (!TestNotNull(TEXT("default scene root"), Root))
	{
		return false;
	}
	USCS_Node* Door = SCS->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("Door"));
	Root->AddChildNode(Door);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	// Components panel > Door > Details > Variable: Category and Tooltip; DefaultSceneRoot > Mobility: Static.
	FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, TEXT("Door"), nullptr, FText::FromString(TEXT("Door")));
	FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, TEXT("Door"), nullptr, FBlueprintMetadata::MD_Tooltip, TEXT("The door leaf"));
	CastChecked<USceneComponent>(Root->ComponentTemplate.Get())->SetMobility(EComponentMobility::Static);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Component details"));
	const TSharedPtr<FJsonObject> DoorRow = FindNamed(Extracted, TEXT("Components"), TEXT("Door"));
	const TSharedPtr<FJsonObject> RootRow = FindNamed(Extracted, TEXT("Components"), TEXT("DefaultSceneRoot"));
	if (!TestTrue(TEXT("door extracted"), DoorRow.IsValid()) || !TestTrue(TEXT("default scene root extracted"), RootRow.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Category extracted"), DoorRow->GetStringField(TEXT("Category")), FString(TEXT("Door")));
	TestEqual(TEXT("Tooltip extracted"), DoorRow->GetStringField(TEXT("Tooltip")), FString(TEXT("The door leaf")));
	TestEqual(TEXT("root scope"), RootRow->GetStringField(TEXT("Scope")), FString(TEXT("Default")));
	const TSharedPtr<FJsonObject>* RootProperties = nullptr;
	TestTrue(TEXT("root Mobility extracted"), RootRow->TryGetObjectField(TEXT("Properties"), RootProperties)
		&& (*RootProperties)->HasField(TEXT("Mobility")));

	// Refile the door and make the root Movable.
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		if (const TSharedPtr<FJsonObject> Row = FindNamed(Document, TEXT("Components"), TEXT("Door")))
		{
			Row->SetStringField(TEXT("Category"), TEXT("Leaf"));
		}
		if (const TSharedPtr<FJsonObject> Row = FindNamed(Document, TEXT("Components"), TEXT("DefaultSceneRoot")))
		{
			const TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
			Properties->SetStringField(TEXT("Mobility"), TEXT("Movable"));
			Row->SetObjectField(TEXT("Properties"), Properties);
		}
	}, TEXT("Edit component details"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		UBlueprint* After = Target.Get<UBlueprint>();
		USimpleConstructionScript* AfterSCS = After != nullptr ? After->SimpleConstructionScript.Get() : nullptr;
		USCS_Node* AfterDoor = AfterSCS != nullptr ? AfterSCS->FindSCSNode(TEXT("Door")) : nullptr;
		USCS_Node* AfterRoot = AfterSCS != nullptr ? AfterSCS->GetDefaultSceneRootNode() : nullptr;
		if (TestNotNull(TEXT("door after"), AfterDoor) && TestNotNull(TEXT("root after"), AfterRoot))
		{
			TestEqual(TEXT("door refiled"), AfterDoor->CategoryName.ToString(), FString(TEXT("Leaf")));
			TestEqual(TEXT("tooltip kept"), AfterDoor->GetMetaData(FBlueprintMetadata::MD_Tooltip), FString(TEXT("The door leaf")));
			TestEqual(TEXT("root is Movable"), static_cast<int32>(CastChecked<USceneComponent>(AfterRoot->ComponentTemplate.Get())->Mobility),
				static_cast<int32>(EComponentMobility::Movable));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityBlueprintCustomEventSettingsTest, "UEAssetDoc.Fidelity.Blueprint.CustomEventSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// A custom event set to Run on Server and Reliable, and one marked Call In Editor and Deprecated with a message, as
// the event's Details panel sets them; an edit changes them and places a new Run on owning Client event.
bool FAssetDocumentFidelityBlueprintCustomEventSettingsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelityBlueprintTests;
	const FTarget Target(TEXT("BP_CustomEventSettings"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UBlueprint* Blueprint = MakeActorBlueprint(Target);
	UEdGraph* EventGraph = Blueprint != nullptr ? FBlueprintEditorUtils::FindEventGraph(Blueprint) : nullptr;
	if (!TestNotNull(TEXT("event graph"), EventGraph))
	{
		return false;
	}
	UK2Node_CustomEvent* ServerFire = AddCustomEvent(*EventGraph, TEXT("ServerFire"), 600);
	ServerFire->FunctionFlags |= FUNC_Net | FUNC_NetServer | FUNC_NetReliable;
	UK2Node_CustomEvent* Rebuild = AddCustomEvent(*EventGraph, TEXT("RebuildPreview"), 800);
	Rebuild->bCallInEditor = true;
	Rebuild->bIsDeprecated = true;
	Rebuild->DeprecationMessage = TEXT("Use RefreshPreview");
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Custom event settings"));
	const TSharedPtr<FJsonObject> FireNode = FindNode(Extracted, TEXT("UbergraphPages"), TEXT("EventGraph"), TEXT("ServerFire"));
	const TSharedPtr<FJsonObject> RebuildNode = FindNode(Extracted, TEXT("UbergraphPages"), TEXT("EventGraph"), TEXT("RebuildPreview"));
	const TSharedPtr<FJsonObject>* FireSettings = nullptr;
	const TSharedPtr<FJsonObject>* RebuildSettings = nullptr;
	if (!TestTrue(TEXT("ServerFire settings extracted"), FireNode.IsValid() && FireNode->TryGetObjectField(TEXT("Properties"), FireSettings))
		|| !TestTrue(TEXT("RebuildPreview settings extracted"), RebuildNode.IsValid() && RebuildNode->TryGetObjectField(TEXT("Properties"), RebuildSettings)))
	{
		return false;
	}
	bool bFlag = false;
	TestEqual(TEXT("Replication extracted"), (*FireSettings)->GetStringField(TEXT("Replication")), FString(TEXT("Server")));
	TestTrue(TEXT("Reliable extracted"), (*FireSettings)->TryGetBoolField(TEXT("Reliable"), bFlag) && bFlag);
	TestFalse(TEXT("default settings left out"), (*FireSettings)->HasField(TEXT("CallInEditor")));
	TestTrue(TEXT("CallInEditor extracted"), (*RebuildSettings)->TryGetBoolField(TEXT("CallInEditor"), bFlag) && bFlag);
	TestTrue(TEXT("Deprecated extracted"), (*RebuildSettings)->TryGetBoolField(TEXT("Deprecated"), bFlag) && bFlag);
	TestEqual(TEXT("DeprecationMessage extracted"), (*RebuildSettings)->GetStringField(TEXT("DeprecationMessage")), FString(TEXT("Use RefreshPreview")));

	// Multicast and unreliable; Call In Editor left out (kept), not deprecated; a new Run on owning Client event.
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		if (const TSharedPtr<FJsonObject> Node = FindNode(Document, TEXT("UbergraphPages"), TEXT("EventGraph"), TEXT("ServerFire")))
		{
			const TSharedPtr<FJsonObject> Settings = Node->GetObjectField(TEXT("Properties"));
			Settings->SetStringField(TEXT("Replication"), TEXT("Multicast"));
			Settings->SetBoolField(TEXT("Reliable"), false);
		}
		if (const TSharedPtr<FJsonObject> Node = FindNode(Document, TEXT("UbergraphPages"), TEXT("EventGraph"), TEXT("RebuildPreview")))
		{
			const TSharedPtr<FJsonObject> Settings = Node->GetObjectField(TEXT("Properties"));
			Settings->RemoveField(TEXT("CallInEditor"));
			Settings->SetBoolField(TEXT("Deprecated"), false);
			Settings->SetStringField(TEXT("DeprecationMessage"), TEXT(""));
		}
		if (const TSharedPtr<FJsonObject> Graph = FindNamed(Document, TEXT("UbergraphPages"), TEXT("EventGraph")))
		{
			const TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
			Settings->SetStringField(TEXT("Replication"), TEXT("Client"));
			Settings->SetBoolField(TEXT("Reliable"), true);
			const TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
			Node->SetStringField(TEXT("Id"), TEXT("ClientNotify"));
			Node->SetStringField(TEXT("Action"), TEXT("K2:CustomEvent"));
			Node->SetStringField(TEXT("Name"), TEXT("ClientNotify"));
			Node->SetObjectField(TEXT("Properties"), Settings);
			TArray<TSharedPtr<FJsonValue>> Nodes = Graph->GetArrayField(TEXT("Nodes"));
			Nodes.Add(MakeShared<FJsonValueObject>(Node));
			Graph->SetArrayField(TEXT("Nodes"), Nodes);
		}
	}, TEXT("Edit custom event settings"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		UBlueprint* After = Target.Get<UBlueprint>();
		const UK2Node_CustomEvent* AfterFire = FindCustomEvent(After, TEXT("ServerFire"));
		const UK2Node_CustomEvent* AfterRebuild = FindCustomEvent(After, TEXT("RebuildPreview"));
		const UK2Node_CustomEvent* Client = FindCustomEvent(After, TEXT("ClientNotify"));
		if (TestNotNull(TEXT("ServerFire after"), AfterFire) && TestNotNull(TEXT("RebuildPreview after"), AfterRebuild)
			&& TestNotNull(TEXT("ClientNotify placed"), Client))
		{
			TestEqual(TEXT("ServerFire multicast"), AfterFire->FunctionFlags & static_cast<uint32>(FUNC_NetFuncFlags),
				static_cast<uint32>(FUNC_Net | FUNC_NetMulticast));
			TestTrue(TEXT("omitted Call In Editor kept"), AfterRebuild->bCallInEditor);
			TestFalse(TEXT("deprecation cleared"), AfterRebuild->bIsDeprecated);
			TestTrue(TEXT("deprecation message cleared"), AfterRebuild->DeprecationMessage.IsEmpty());
			TestEqual(TEXT("ClientNotify runs on owning client, reliable"), Client->FunctionFlags & static_cast<uint32>(FUNC_NetFuncFlags),
				static_cast<uint32>(FUNC_Net | FUNC_NetClient | FUNC_NetReliable));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityBlueprintNodeEnabledStateTest, "UEAssetDoc.Fidelity.Blueprint.NodeEnabledState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// A node disabled and one set to Development Only from the graph's context menu (the new Blueprint's placeholder
// events are disabled too, but not by the user): extract states the user's choices only; an edit re-enables one.
bool FAssetDocumentFidelityBlueprintNodeEnabledStateTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelityBlueprintTests;
	const FTarget Target(TEXT("BP_NodeEnabledState"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UBlueprint* Blueprint = MakeActorBlueprint(Target);
	UEdGraph* EventGraph = Blueprint != nullptr ? FBlueprintEditorUtils::FindEventGraph(Blueprint) : nullptr;
	if (!TestNotNull(TEXT("event graph"), EventGraph))
	{
		return false;
	}
	AddCustomEvent(*EventGraph, TEXT("DebugDraw"), 600)->SetEnabledState(ENodeEnabledState::Disabled);
	AddCustomEvent(*EventGraph, TEXT("DevCheats"), 800)->SetEnabledState(ENodeEnabledState::DevelopmentOnly);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Node enabled state"));
	const TSharedPtr<FJsonObject> Debug = FindNode(Extracted, TEXT("UbergraphPages"), TEXT("EventGraph"), TEXT("DebugDraw"));
	const TSharedPtr<FJsonObject> Dev = FindNode(Extracted, TEXT("UbergraphPages"), TEXT("EventGraph"), TEXT("DevCheats"));
	if (!TestTrue(TEXT("nodes extracted"), Debug.IsValid() && Dev.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Disabled extracted"), Debug->GetStringField(TEXT("Enabled")), FString(TEXT("Disabled")));
	TestEqual(TEXT("DevelopmentOnly extracted"), Dev->GetStringField(TEXT("Enabled")), FString(TEXT("DevelopmentOnly")));
	bool bPlaceholderStated = false;
	const TSharedPtr<FJsonObject> Graph = FindNamed(Extracted, TEXT("UbergraphPages"), TEXT("EventGraph"));
	for (const TSharedPtr<FJsonValue>& Node : Graph.IsValid() ? Graph->GetArrayField(TEXT("Nodes")) : TArray<TSharedPtr<FJsonValue>>())
	{
		FString Name;
		bPlaceholderStated |= Node->AsObject()->HasField(TEXT("Enabled")) && !(Node->AsObject()->TryGetStringField(TEXT("Name"), Name)
			&& (Name == TEXT("DebugDraw") || Name == TEXT("DevCheats")));
	}
	TestFalse(TEXT("placeholder events state no Enabled"), bPlaceholderStated);

	// Re-enable DebugDraw; DevCheats leaves Enabled out (kept).
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		if (const TSharedPtr<FJsonObject> Node = FindNode(Document, TEXT("UbergraphPages"), TEXT("EventGraph"), TEXT("DebugDraw")))
		{
			Node->SetStringField(TEXT("Enabled"), TEXT("Enabled"));
		}
		if (const TSharedPtr<FJsonObject> Node = FindNode(Document, TEXT("UbergraphPages"), TEXT("EventGraph"), TEXT("DevCheats")))
		{
			Node->RemoveField(TEXT("Enabled"));
		}
	}, TEXT("Edit node enabled state"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		UBlueprint* After = Target.Get<UBlueprint>();
		const UK2Node_CustomEvent* AfterDebug = FindCustomEvent(After, TEXT("DebugDraw"));
		const UK2Node_CustomEvent* AfterDev = FindCustomEvent(After, TEXT("DevCheats"));
		if (TestNotNull(TEXT("DebugDraw after"), AfterDebug) && TestNotNull(TEXT("DevCheats after"), AfterDev))
		{
			TestTrue(TEXT("DebugDraw enabled"), AfterDebug->GetDesiredEnabledState() == ENodeEnabledState::Enabled);
			TestTrue(TEXT("DevCheats kept"), AfterDev->GetDesiredEnabledState() == ENodeEnabledState::DevelopmentOnly);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityBlueprintDevelopmentOnlyNodesTest, "UEAssetDoc.Fidelity.Blueprint.DevelopmentOnlyNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Print String is placed Development Only (its function says so) without the user setting it. One left so, one the
// user enabled: extract states the user's choice only, a Create copy reproduces both, a new Print String stating
// "Enabled" is enabled, and a document stating "Enabled" on the untouched one is a difference.
bool FAssetDocumentFidelityBlueprintDevelopmentOnlyNodesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelityBlueprintTests;
	const FTarget Target(TEXT("BP_DevelopmentOnly"));
	const FTarget Fresh(TEXT("BP_DevelopmentOnlyCopy"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); Fresh.Cleanup(); };

	UBlueprint* Blueprint = MakeActorBlueprint(Target);
	UEdGraph* EventGraph = Blueprint != nullptr ? FBlueprintEditorUtils::FindEventGraph(Blueprint) : nullptr;
	const UFunction* PrintString = UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString));
	if (!TestNotNull(TEXT("event graph"), EventGraph) || !TestNotNull(TEXT("PrintString"), PrintString))
	{
		return false;
	}
	auto AddPrint = [EventGraph, PrintString](int32 PositionY)
	{
		FGraphNodeCreator<UK2Node_CallFunction> Creator(*EventGraph);
		UK2Node_CallFunction* Node = Creator.CreateNode();
		Node->SetFromFunction(PrintString);
		Node->NodePosY = PositionY;
		Creator.Finalize();
		return Node;
	};
	const UK2Node_CallFunction* Kept = AddPrint(600);
	UK2Node_CallFunction* Enabled = AddPrint(800);
	if (!TestTrue(TEXT("placed Development Only, not by the user"), Kept->GetDesiredEnabledState() == ENodeEnabledState::DevelopmentOnly
		&& !Kept->HasUserSetTheEnabledState()))
	{
		return false;
	}
	Enabled->SetEnabledState(ENodeEnabledState::Enabled);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Development Only nodes"));
	const TSharedPtr<FJsonObject> KeptRow = FindNodeAtY(Extracted, 600);
	const TSharedPtr<FJsonObject> EnabledRow = FindNodeAtY(Extracted, 800);
	if (!TestTrue(TEXT("nodes extracted"), KeptRow.IsValid() && EnabledRow.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("a node keeping its placed state states no Enabled"), KeptRow->HasField(TEXT("Enabled")));
	TestEqual(TEXT("the user's Enabled is extracted"), EnabledRow->GetStringField(TEXT("Enabled")), FString(TEXT("Enabled")));

	// "Enabled" stated on the untouched node is a difference.
	{
		const TSharedPtr<FJsonObject> Stated = Parse(Serialize(Extracted));
		Stated->SetStringField(TEXT("Action"), TEXT("Update"));
		FindNodeAtY(Stated, 600)->SetStringField(TEXT("Enabled"), TEXT("Enabled"));
		FAssetDocumentDiffRequest Request;
		Request.Document = Stated;
		const FAssetDocumentResult Diffed = FAssetDocumentModule::Get().GetService().Diff(Request);
		const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
		TestTrue(TEXT("Enabled on a node placed Development Only is a difference"), Diffed.IsSuccess() && Diffed.Payload.IsValid()
			&& Diffed.Payload->TryGetArrayField(TEXT("changed"), Changed) && !Changed->IsEmpty());
	}

	// A Create copy, with a new Print String stating "Enabled".
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Extracted));
	{
		const TSharedPtr<FJsonObject> Added = Parse(Serialize(EnabledRow));
		Added->SetStringField(TEXT("Id"), TEXT("NewPrint"));
		Added->RemoveField(TEXT("NodeGuid"));
		Added->GetObjectField(TEXT("Position"))->SetNumberField(TEXT("Y"), 1000);
		TArray<TSharedPtr<FJsonValue>> Nodes = FindNamed(Copy, TEXT("UbergraphPages"), TEXT("EventGraph"))->GetArrayField(TEXT("Nodes"));
		Nodes.Add(MakeShared<FJsonValueObject>(Added));
		FindNamed(Copy, TEXT("UbergraphPages"), TEXT("EventGraph"))->SetArrayField(TEXT("Nodes"), Nodes);
	}
	if (TestTrue(TEXT("the copy applies"), CreateCopy(*this, Fresh, Copy, TEXT("Create copy")).IsSuccess()))
	{
		UBlueprint* After = Fresh.Get<UBlueprint>();
		const UEdGraphNode* AfterKept = NodeAtY(After, 600);
		const UEdGraphNode* AfterEnabled = NodeAtY(After, 800);
		const UEdGraphNode* AfterNew = NodeAtY(After, 1000);
		if (TestNotNull(TEXT("kept copy"), AfterKept) && TestNotNull(TEXT("enabled copy"), AfterEnabled) && TestNotNull(TEXT("new node"), AfterNew))
		{
			TestTrue(TEXT("the untouched node is placed Development Only"), AfterKept->GetDesiredEnabledState() == ENodeEnabledState::DevelopmentOnly
				&& !AfterKept->HasUserSetTheEnabledState());
			TestTrue(TEXT("the user-enabled node is enabled by the user"), AfterEnabled->GetDesiredEnabledState() == ENodeEnabledState::Enabled
				&& AfterEnabled->HasUserSetTheEnabledState());
			TestTrue(TEXT("the new node stating Enabled is enabled"), AfterNew->GetDesiredEnabledState() == ENodeEnabledState::Enabled);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityBlueprintAddChildActorTemplateTest, "UEAssetDoc.Fidelity.Blueprint.AddChildActorTemplate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// An "Add Child Actor Component" node with its Child Actor Class picked in its Details panel: extract states the
// class, not the child actor template the component owns, and a Create copy onto a new Blueprint builds its own.
bool FAssetDocumentFidelityBlueprintAddChildActorTemplateTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelityBlueprintTests;
	const FTarget Target(TEXT("BP_AddChildActor"));
	const FTarget Fresh(TEXT("BP_AddChildActorCopy"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); Fresh.Cleanup(); };

	UBlueprint* Blueprint = MakeActorBlueprint(Target);
	UEdGraph* EventGraph = Blueprint != nullptr ? FBlueprintEditorUtils::FindEventGraph(Blueprint) : nullptr;
	if (!TestNotNull(TEXT("event graph"), EventGraph))
	{
		return false;
	}
	FComponentTypeEntry Entry;
	Entry.ComponentName = TEXT("ChildActorComponent");
	Entry.ComponentClass = UChildActorComponent::StaticClass();
	UBlueprintComponentNodeSpawner* Spawner = UBlueprintComponentNodeSpawner::Create(Entry);
	UK2Node_AddComponent* AddNode = Spawner != nullptr
		? Cast<UK2Node_AddComponent>(Spawner->Invoke(EventGraph, IBlueprintNodeBinder::FBindingSet(), FVector2D(0.0, 600.0))) : nullptr;
	UChildActorComponent* Template = AddNode != nullptr ? Cast<UChildActorComponent>(AddNode->GetTemplateFromNode()) : nullptr;
	if (!TestNotNull(TEXT("component template"), Template))
	{
		return false;
	}
	Template->Modify();
	Template->SetChildActorClass(AStaticMeshActor::StaticClass());
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Add Child Actor template"));
	const TSharedPtr<FJsonObject> Row = FindNodeAtY(Extracted, 600);
	const TSharedPtr<FJsonObject>* Values = nullptr;
	if (!TestTrue(TEXT("template properties extracted"), Row.IsValid() && Row->TryGetObjectField(TEXT("TemplateProperties"), Values)))
	{
		return false;
	}
	FString ClassPath;
	TestTrue(TEXT("child actor class extracted"), (*Values)->TryGetStringField(TEXT("ChildActorClass"), ClassPath)
		&& ClassPath.Contains(TEXT("StaticMeshActor")));
	TestFalse(TEXT("the owned child actor template is not stated"), (*Values)->HasField(TEXT("ChildActorTemplate")));

	if (TestTrue(TEXT("the copy applies"), CreateCopy(*this, Fresh, Extracted, TEXT("Create copy")).IsSuccess()))
	{
		const UK2Node_AddComponent* AfterNode = Cast<UK2Node_AddComponent>(NodeAtY(Fresh.Get<UBlueprint>(), 600));
		const UChildActorComponent* AfterTemplate = AfterNode != nullptr ? Cast<UChildActorComponent>(AfterNode->GetTemplateFromNode()) : nullptr;
		if (TestNotNull(TEXT("copied template"), AfterTemplate))
		{
			TestTrue(TEXT("child actor class copied"), AfterTemplate->GetChildActorClass() == AStaticMeshActor::StaticClass());
			const AActor* ChildTemplate = AfterTemplate->GetChildActorTemplate();
			TestTrue(TEXT("no reference into the source Blueprint"), ChildTemplate == nullptr
				|| ChildTemplate->GetOutermost() == AfterTemplate->GetOutermost());
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityBlueprintPureCastTest, "UEAssetDoc.Fidelity.Blueprint.PureCast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// A cast converted to a pure cast in the graph: extract states it; an edit makes it impure and places a new pure cast.
bool FAssetDocumentFidelityBlueprintPureCastTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelityBlueprintTests;
	const FTarget Target(TEXT("BP_PureCast"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UBlueprint* Blueprint = MakeActorBlueprint(Target);
	UEdGraph* EventGraph = Blueprint != nullptr ? FBlueprintEditorUtils::FindEventGraph(Blueprint) : nullptr;
	if (!TestNotNull(TEXT("event graph"), EventGraph))
	{
		return false;
	}
	FGraphNodeCreator<UK2Node_DynamicCast> Creator(*EventGraph);
	UK2Node_DynamicCast* AsPawn = Creator.CreateNode();
	AsPawn->TargetType = APawn::StaticClass();
	AsPawn->NodePosY = 600;
	Creator.Finalize();
	AsPawn->SetPurity(true);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const FString PawnCast = FString(TEXT("K2:DynamicCast:")) + APawn::StaticClass()->GetPathName();
	auto FindCast = [&PawnCast](const TSharedPtr<FJsonObject>& Document) -> TSharedPtr<FJsonObject>
	{
		const TSharedPtr<FJsonObject> Graph = FindNamed(Document, TEXT("UbergraphPages"), TEXT("EventGraph"));
		for (const TSharedPtr<FJsonValue>& Node : Graph.IsValid() ? Graph->GetArrayField(TEXT("Nodes")) : TArray<TSharedPtr<FJsonValue>>())
		{
			FString Action;
			if (Node->AsObject()->TryGetStringField(TEXT("Action"), Action) && Action == PawnCast)
			{
				return Node->AsObject();
			}
		}
		return nullptr;
	};
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Pure cast"));
	const TSharedPtr<FJsonObject> CastRow = FindCast(Extracted);
	const TSharedPtr<FJsonObject>* Settings = nullptr;
	bool bPure = false;
	TestTrue(TEXT("pure cast extracted"), CastRow.IsValid() && CastRow->TryGetObjectField(TEXT("Properties"), Settings)
		&& (*Settings)->TryGetBoolField(TEXT("Pure"), bPure) && bPure);

	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [&FindCast](const TSharedPtr<FJsonObject>& Document)
	{
		if (const TSharedPtr<FJsonObject> Node = FindCast(Document))
		{
			Node->GetObjectField(TEXT("Properties"))->SetBoolField(TEXT("Pure"), false);
		}
		if (const TSharedPtr<FJsonObject> Graph = FindNamed(Document, TEXT("UbergraphPages"), TEXT("EventGraph")))
		{
			const TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
			Properties->SetBoolField(TEXT("Pure"), true);
			const TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
			Node->SetStringField(TEXT("Id"), TEXT("AsCharacter"));
			Node->SetStringField(TEXT("Action"), FString(TEXT("K2:DynamicCast:")) + ACharacter::StaticClass()->GetPathName());
			Node->SetObjectField(TEXT("Properties"), Properties);
			TArray<TSharedPtr<FJsonValue>> Nodes = Graph->GetArrayField(TEXT("Nodes"));
			Nodes.Add(MakeShared<FJsonValueObject>(Node));
			Graph->SetArrayField(TEXT("Nodes"), Nodes);
		}
	}, TEXT("Edit cast purity"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		TArray<UK2Node_DynamicCast*> Casts;
		FBlueprintEditorUtils::GetAllNodesOfClass(Target.Get<UBlueprint>(), Casts);
		UK2Node_DynamicCast* const* Pawn = Casts.FindByPredicate([](const UK2Node_DynamicCast* Node) { return Node->TargetType == APawn::StaticClass(); });
		UK2Node_DynamicCast* const* Character = Casts.FindByPredicate([](const UK2Node_DynamicCast* Node) { return Node->TargetType == ACharacter::StaticClass(); });
		TestTrue(TEXT("pawn cast made impure"), Pawn != nullptr && !(*Pawn)->IsNodePure());
		TestTrue(TEXT("new cast placed pure"), Character != nullptr && (*Character)->IsNodePure());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityBlueprintNodeDetailsTest, "UEAssetDoc.Fidelity.Blueprint.NodeDetails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// A Switch on String with cases and Case Sensitive set, and a Break Struct with a member pin hidden, in their Details
// panels: extract states them; an edit adds a case and shows the hidden pin.
bool FAssetDocumentFidelityBlueprintNodeDetailsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelityBlueprintTests;
	const FTarget Target(TEXT("BP_NodeDetails"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UBlueprint* Blueprint = MakeActorBlueprint(Target);
	UEdGraph* EventGraph = Blueprint != nullptr ? FBlueprintEditorUtils::FindEventGraph(Blueprint) : nullptr;
	if (!TestNotNull(TEXT("event graph"), EventGraph))
	{
		return false;
	}
	FGraphNodeCreator<UK2Node_SwitchString> SwitchCreator(*EventGraph);
	UK2Node_SwitchString* Switch = SwitchCreator.CreateNode();
	Switch->NodePosY = 600;
	SwitchCreator.Finalize();
	Switch->PinNames = {TEXT("Fire"), TEXT("Jump")};
	Switch->bIsCaseSensitive = true;
	Switch->ReconstructNode();
	FGraphNodeCreator<UK2Node_BreakStruct> BreakCreator(*EventGraph);
	UK2Node_BreakStruct* Break = BreakCreator.CreateNode();
	Break->StructType = FWalkableSlopeOverride::StaticStruct();
	Break->NodePosY = 800;
	BreakCreator.Finalize();
	for (FOptionalPinFromProperty& Field : Break->ShowPinForProperties)
	{
		Field.bShowPin = Field.PropertyName != TEXT("WalkableSlopeAngle");
	}
	Break->ReconstructNode();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	// The node whose Properties state Key.
	auto FindStating = [](const TSharedPtr<FJsonObject>& Document, const TCHAR* Key) -> TSharedPtr<FJsonObject>
	{
		const TSharedPtr<FJsonObject> Graph = FindNamed(Document, TEXT("UbergraphPages"), TEXT("EventGraph"));
		for (const TSharedPtr<FJsonValue>& Node : Graph.IsValid() ? Graph->GetArrayField(TEXT("Nodes")) : TArray<TSharedPtr<FJsonValue>>())
		{
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (Node->AsObject()->TryGetObjectField(TEXT("Properties"), Properties) && (*Properties)->HasField(Key))
			{
				return *Properties;
			}
		}
		return nullptr;
	};
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Node details"));
	const TSharedPtr<FJsonObject> SwitchSettings = FindStating(Extracted, TEXT("PinNames"));
	const TSharedPtr<FJsonObject> BreakSettings = FindStating(Extracted, TEXT("HiddenFields"));
	if (!TestTrue(TEXT("switch cases extracted"), SwitchSettings.IsValid()) || !TestTrue(TEXT("hidden pins extracted"), BreakSettings.IsValid()))
	{
		return false;
	}
	bool bFlag = false;
	TestEqual(TEXT("two cases"), SwitchSettings->GetArrayField(TEXT("PinNames")).Num(), 2);
	TestTrue(TEXT("case sensitive extracted"), SwitchSettings->TryGetBoolField(TEXT("bIsCaseSensitive"), bFlag) && bFlag);
	const TArray<TSharedPtr<FJsonValue>> Hidden = BreakSettings->GetArrayField(TEXT("HiddenFields"));
	TestTrue(TEXT("hidden member extracted"), Hidden.Num() == 1 && Hidden[0]->AsString() == TEXT("WalkableSlopeAngle"));

	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [&FindStating](const TSharedPtr<FJsonObject>& Document)
	{
		if (const TSharedPtr<FJsonObject> Settings = FindStating(Document, TEXT("PinNames")))
		{
			TArray<TSharedPtr<FJsonValue>> Cases = Settings->GetArrayField(TEXT("PinNames"));
			Cases.Add(MakeShared<FJsonValueString>(TEXT("Crouch")));
			Settings->SetArrayField(TEXT("PinNames"), Cases);
		}
		if (const TSharedPtr<FJsonObject> Settings = FindStating(Document, TEXT("HiddenFields")))
		{
			Settings->SetArrayField(TEXT("HiddenFields"), TArray<TSharedPtr<FJsonValue>>());
		}
	}, TEXT("Edit node details"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		TArray<UK2Node_SwitchString*> Switches;
		TArray<UK2Node_BreakStruct*> Breaks;
		FBlueprintEditorUtils::GetAllNodesOfClass(Target.Get<UBlueprint>(), Switches);
		FBlueprintEditorUtils::GetAllNodesOfClass(Target.Get<UBlueprint>(), Breaks);
		TestTrue(TEXT("case added with its pin"), Switches.Num() == 1 && Switches[0]->PinNames.Num() == 3
			&& Switches[0]->FindPin(TEXT("Crouch")) != nullptr);
		TestTrue(TEXT("hidden pin shown"), Breaks.Num() == 1 && Breaks[0]->FindPin(TEXT("WalkableSlopeAngle")) != nullptr);
	}

	// The shown member wired into a Make Struct; one apply removes the link and hides the pin again.
	{
		UBlueprint* After = Target.Get<UBlueprint>();
		TArray<UK2Node_BreakStruct*> Breaks;
		FBlueprintEditorUtils::GetAllNodesOfClass(After, Breaks);
		UEdGraph* Graph = After != nullptr ? FBlueprintEditorUtils::FindEventGraph(After) : nullptr;
		if (!TestTrue(TEXT("break node"), Breaks.Num() == 1 && Graph != nullptr))
		{
			return false;
		}
		FGraphNodeCreator<UK2Node_MakeStruct> MakeCreator(*Graph);
		UK2Node_MakeStruct* Make = MakeCreator.CreateNode();
		Make->StructType = FWalkableSlopeOverride::StaticStruct();
		Make->NodePosY = 1000;
		MakeCreator.Finalize();
		UEdGraphPin* From = Breaks[0]->FindPin(TEXT("WalkableSlopeAngle"));
		UEdGraphPin* To = Make->FindPin(TEXT("WalkableSlopeAngle"), EGPD_Input);
		if (!TestTrue(TEXT("member pins linked"), From != nullptr && To != nullptr && Graph->GetSchema()->TryCreateConnection(From, To)))
		{
			return false;
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(After);
		FKismetEditorUtilities::CompileBlueprint(After);
		if (!TestTrue(TEXT("saved with the link"), Target.Save()))
		{
			return false;
		}
	}
	const FAssetDocumentResult Hidden2 = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Graph = FindNamed(Document, TEXT("UbergraphPages"), TEXT("EventGraph"));
		if (!Graph.IsValid())
		{
			return;
		}
		TArray<TSharedPtr<FJsonValue>> Links = Graph->GetArrayField(TEXT("Links"));
		Links.RemoveAll([](const TSharedPtr<FJsonValue>& Link) { return Link->AsObject()->GetStringField(TEXT("From")).EndsWith(TEXT(":WalkableSlopeAngle")); });
		Graph->SetArrayField(TEXT("Links"), Links);
		for (const TSharedPtr<FJsonValue>& Node : Graph->GetArrayField(TEXT("Nodes")))
		{
			if (Node->AsObject()->GetStringField(TEXT("Action")).StartsWith(TEXT("K2:BreakStruct")))
			{
				const TSharedPtr<FJsonObject>* Existing = nullptr;
				const TSharedPtr<FJsonObject> Settings = Node->AsObject()->TryGetObjectField(TEXT("Properties"), Existing)
					? *Existing : MakeShared<FJsonObject>();
				Settings->SetArrayField(TEXT("HiddenFields"), {MakeShared<FJsonValueString>(TEXT("WalkableSlopeAngle"))});
				Node->AsObject()->SetObjectField(TEXT("Properties"), Settings);
			}
		}
	}, TEXT("Unlink and hide a member pin"));
	if (TestTrue(TEXT("unlink and hide applies"), Hidden2.IsSuccess()))
	{
		TArray<UK2Node_BreakStruct*> Breaks;
		TArray<UK2Node_MakeStruct*> Makes;
		FBlueprintEditorUtils::GetAllNodesOfClass(Target.Get<UBlueprint>(), Breaks);
		FBlueprintEditorUtils::GetAllNodesOfClass(Target.Get<UBlueprint>(), Makes);
		TestTrue(TEXT("member pin hidden"), Breaks.Num() == 1 && Breaks[0]->FindPin(TEXT("WalkableSlopeAngle")) == nullptr);
		const UEdGraphPin* Input = Makes.Num() == 1 ? Makes[0]->FindPin(TEXT("WalkableSlopeAngle"), EGPD_Input) : nullptr;
		TestTrue(TEXT("its link removed"), Input != nullptr && Input->LinkedTo.IsEmpty());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityBlueprintAddComponentTemplateTest, "UEAssetDoc.Fidelity.Blueprint.AddComponentTemplate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// An "Add Static Mesh Component" node placed from the palette with its Static Mesh and Cast Shadow set in its Details
// panel: extract states them as TemplateProperties; an edit picks another mesh.
bool FAssetDocumentFidelityBlueprintAddComponentTemplateTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelityBlueprintTests;
	const FTarget Target(TEXT("BP_AddComponentTemplate"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");
	const TCHAR* const SpherePath = TEXT("/Engine/BasicShapes/Sphere.Sphere");
	UBlueprint* Blueprint = MakeActorBlueprint(Target);
	UEdGraph* EventGraph = Blueprint != nullptr ? FBlueprintEditorUtils::FindEventGraph(Blueprint) : nullptr;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
	if (!TestNotNull(TEXT("event graph"), EventGraph) || !TestNotNull(TEXT("cube mesh"), Cube))
	{
		return false;
	}
	FComponentTypeEntry Entry;
	Entry.ComponentName = TEXT("StaticMeshComponent");
	Entry.ComponentClass = UStaticMeshComponent::StaticClass();
	UBlueprintComponentNodeSpawner* Spawner = UBlueprintComponentNodeSpawner::Create(Entry);
	UK2Node_AddComponent* AddNode = Spawner != nullptr
		? Cast<UK2Node_AddComponent>(Spawner->Invoke(EventGraph, IBlueprintNodeBinder::FBindingSet(), FVector2D(0.0, 600.0))) : nullptr;
	UStaticMeshComponent* Template = AddNode != nullptr ? Cast<UStaticMeshComponent>(AddNode->GetTemplateFromNode()) : nullptr;
	if (!TestNotNull(TEXT("component template"), Template))
	{
		return false;
	}
	// The Details panel's writes: the Static Mesh picker and the Cast Shadow checkbox.
	FObjectProperty* MeshProperty = CastField<FObjectProperty>(UStaticMeshComponent::StaticClass()->FindPropertyByName(TEXT("StaticMesh")));
	FBoolProperty* ShadowProperty = CastField<FBoolProperty>(UPrimitiveComponent::StaticClass()->FindPropertyByName(TEXT("CastShadow")));
	if (!TestNotNull(TEXT("mesh property"), MeshProperty) || !TestNotNull(TEXT("shadow property"), ShadowProperty))
	{
		return false;
	}
	Template->Modify();
	MeshProperty->SetObjectPropertyValue_InContainer(Template, Cube);
	ShadowProperty->SetPropertyValue_InContainer(Template, false);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	auto FindTemplate = [](const TSharedPtr<FJsonObject>& Document) -> TSharedPtr<FJsonObject>
	{
		const TSharedPtr<FJsonObject> Graph = FindNamed(Document, TEXT("UbergraphPages"), TEXT("EventGraph"));
		for (const TSharedPtr<FJsonValue>& Node : Graph.IsValid() ? Graph->GetArrayField(TEXT("Nodes")) : TArray<TSharedPtr<FJsonValue>>())
		{
			const TSharedPtr<FJsonObject>* Values = nullptr;
			if (Node->AsObject()->TryGetObjectField(TEXT("TemplateProperties"), Values))
			{
				return *Values;
			}
		}
		return nullptr;
	};
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Add Component template"));
	const TSharedPtr<FJsonObject> Values = FindTemplate(Extracted);
	if (!TestTrue(TEXT("template properties extracted"), Values.IsValid()))
	{
		return false;
	}
	FString Mesh;
	bool bShadow = true;
	TestTrue(TEXT("static mesh extracted"), Values->TryGetStringField(TEXT("StaticMesh"), Mesh) && Mesh.Contains(TEXT("BasicShapes/Cube")));
	TestTrue(TEXT("cast shadow extracted"), Values->TryGetBoolField(TEXT("CastShadow"), bShadow) && !bShadow);
	TestFalse(TEXT("unchanged values are not stated"), Values->HasField(TEXT("Mobility")));

	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [&FindTemplate, SpherePath](const TSharedPtr<FJsonObject>& Document)
	{
		if (const TSharedPtr<FJsonObject> Template = FindTemplate(Document))
		{
			Template->SetStringField(TEXT("StaticMesh"), SpherePath);
		}
	}, TEXT("Edit Add Component template"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		TArray<UK2Node_AddComponent*> Nodes;
		FBlueprintEditorUtils::GetAllNodesOfClass(Target.Get<UBlueprint>(), Nodes);
		const UStaticMeshComponent* Edit = Nodes.Num() == 1 ? Cast<UStaticMeshComponent>(Nodes[0]->GetTemplateFromNode()) : nullptr;
		TestTrue(TEXT("mesh replaced"), Edit != nullptr && Edit->GetStaticMesh() != nullptr
			&& Edit->GetStaticMesh()->GetPathName() == SpherePath);
		TestTrue(TEXT("cast shadow unchanged"), Edit != nullptr && !Edit->CastShadow);
	}
	return !HasAnyErrors();
}

#endif
