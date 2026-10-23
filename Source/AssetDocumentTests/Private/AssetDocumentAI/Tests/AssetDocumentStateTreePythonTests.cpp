// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// End-to-end StateTree documents through the production Service and the Python Profile
// (state_tree_profile, ai.state_tree.* primitives):
//
// UEAssetDoc.StateTree.Python: one ApplyFile creates a tree with its schema (the New
// State Tree dialog's choice), parameters, an evaluator, a global task, nested states of every type,
// enter conditions, tasks with instance data, considerations, transitions of every target kind and
// bindings (parameter, context, evaluator output, property function, state parameter, linked asset
// parameters); Extract / Diff agree; re-applying the extraction is a no-op; a second apply renames a
// state (the transitions follow its ID), moves and removes states, edits a task value and rebinds;
// an unknown node struct, a binding type mismatch and a transition to a missing state fail with the
// document path and change nothing; a compile error fails the apply and rolls back; the saved
// package reloads to the same document; failed creates leave nothing behind.
// UEAssetDoc.StateTree.NodeConstruction: the raw editor-style node
// construction (AssetDocumentStateTreeNodes.cpp InitializeNode) equals the editor's own; a Blueprint
// task or condition class becomes its wrapper struct with an instance of the class.
// UEAssetDoc.StateTree.Undo: a human edit + save, then an AI apply; Ctrl+Z
// afterwards must not corrupt the tree; a failed AI apply keeps the human's undo step.
// UEAssetDoc.StateTree.Discovery: inspect of a node struct lists the node fields and
// the instance data fields an author writes (with Usage), not the node base internals; list_types
// with a node base omits the Hidden bases and test-suite types, and with a schema only lists what that
// schema's node picker offers.
// UEAssetDoc.StateTree.SchemaContext: setting the schema's ContextActorClass
// refreshes the Actor context data as the Details panel does (the schema's PostEditChangeChainProperty), so a
// binding to a member of that class ("Context/Actor:BaseEyeHeight") resolves in the same apply, on create and
// when an update changes the class.
// UEAssetDoc.StateTree.OmittedFields: a state field, a task flag and an instance value the
// document leaves out are a new state's / task's: Diff reports them and the apply resets them.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "StateTree/AssetDocumentStateTreePrimitives.h"
#include "Tests/AssetDocumentStateTreeTestTypes.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/StateTreeConditionBlueprintBase.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "Conditions/StateTreeCommonConditions.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Optional.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorModule.h"
#include "StateTreeEditorNode.h"
#include "StateTreeTaskBase.h"
#include "StateTreeNodeClassCache.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentStateTreePythonTests
{
const TCHAR* FolderName = TEXT("AssetDocumentStateTreePython");
const TCHAR* StateTreeClass = TEXT("/Script/StateTreeModule.StateTree");

void ReleaseUndoHistory()
{
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
		&& GEditor->Trans->GetQueueLength() > 0)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentStateTreeTests", "Cleanup", "AssetDocument StateTree test cleanup"));
	}
}

// The test evaluator is meta=(Hidden) so users never see it in the node picker; the profile refuses
// Hidden types as the picker does. The tests that author it lift Hidden for their own duration only.
struct FScopedTestTypesVisible
{
	FScopedTestTypesVisible() { FAssetDocumentStateTreeTestEvaluator::StaticStruct()->RemoveMetaData(TEXT("Hidden")); }
	~FScopedTestTypesVisible() { FAssetDocumentStateTreeTestEvaluator::StaticStruct()->SetMetaData(TEXT("Hidden"), TEXT("")); }
	FScopedTestTypesVisible(const FScopedTestTypesVisible&) = delete;
	FScopedTestTypesVisible& operator=(const FScopedTestTypesVisible&) = delete;
};

struct FTarget
{
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	explicit FTarget(const TCHAR* Prefix)
		: AssetName(FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		PackageName = FString(TEXT("/Game/")) + FolderName + TEXT("/") + AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), FolderName, AssetName + TEXT(".assetdoc.json"));
	}

	UStateTree* Get() const { return Cast<UStateTree>(FSoftObjectPath(ObjectPath).ResolveObject()); }

	// Saved trees are unloaded and unsaved ones deleted as the Content Browser does.
	void Cleanup() const
	{
		ReleaseUndoHistory();
		if (UStateTree* Asset = Get())
		{
			if (IFileManager::Get().FileExists(*PackageFile))
			{
				UPackageTools::UnloadPackages({Asset->GetOutermost()});
			}
			else
			{
				Asset->GetOutermost()->ClearDirtyFlag();
				ObjectTools::DeleteObjects({Asset}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			IFileManager::Get().Delete(*PackageFile, false, true);
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		}
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};

FString Serialize(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Text;
}

TSharedPtr<FJsonObject> Parse(const FString& Json)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
	return Object;
}

void LogResult(FAutomationTestBase& Test, const TCHAR* Stage, const FAssetDocumentResult& Result)
{
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s"), Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
}

FAssetDocumentResult Apply(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const TCHAR* Stage)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	FFileHelper::SaveStringToFile(Json, *Target.SidecarPath);
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	LogResult(Test, Stage, Result);
	return Result;
}

// Validate of an inline document (no sidecar written).
FAssetDocumentResult Validate(FAutomationTestBase& Test, const FString& Json, const TCHAR* Stage)
{
	FAssetDocumentValidateRequest Request;
	Request.Document = Parse(Json);
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
	LogResult(Test, *FString::Printf(TEXT("%s (validate)"), Stage), Result);
	return Result;
}

TSharedPtr<FJsonObject> Extract(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Stage)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Target.ObjectPath;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Extract(Request);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract succeeds"), Stage), Result.IsSuccess() && Result.Payload.IsValid()))
	{
		LogResult(Test, Stage, Result);
		return nullptr;
	}
	return Result.Payload;
}

void TestNoDiff(FAutomationTestBase& Test, const FString& Json, const TCHAR* Stage)
{
	FAssetDocumentDiffRequest Request;
	Request.Document = Parse(Json);
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Diff(Request);
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	const bool bOk = Result.IsSuccess() && Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("changed"), Changed);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: diff succeeds"), Stage), bOk))
	{
		LogResult(Test, Stage, Result);
		return;
	}
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: live asset equals the document"), Stage), Changed->Num(), 0))
	{
		Test.AddInfo(Serialize(Result.Payload));
	}
}

bool ExtractEquals(FAutomationTestBase& Test, const FTarget& Target, const FString& Expected, const TCHAR* Stage)
{
	const FString Actual = Serialize(Extract(Test, Target, Stage));
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: extraction is unchanged"), Stage), Actual, Expected))
	{
		Test.AddInfo(FString::Printf(TEXT("expected %s"), *Expected));
		Test.AddInfo(FString::Printf(TEXT("actual   %s"), *Actual));
		return false;
	}
	return true;
}

// The failure carries Code at Path (an empty Path accepts any path).
bool TestFailsWith(FAutomationTestBase& Test, const FAssetDocumentResult& Result, const TCHAR* Code, const TCHAR* Path, const TCHAR* Stage)
{
	Test.TestFalse(*FString::Printf(TEXT("%s: fails"), Stage), Result.IsSuccess());
	const bool bFound = Result.Diagnostics.ContainsByPredicate([Code, Path](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == Code && (*Path == TEXT('\0') || Diagnostic.Path == Path);
	});
	return Test.TestTrue(*FString::Printf(TEXT("%s: diagnostic %s at %s"), Stage, Code, *Path ? Path : TEXT("(any)")), bFound);
}

// Re-applying the extraction (Action Update) saves nothing and leaves the package file alone;
// returns the extraction as CreateOrUpdate (the Extract baseline).
FString TestNoOpReapply(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Stage)
{
	const TSharedPtr<FJsonObject> Extracted = Extract(Test, Target, Stage);
	UStateTree* Asset = Target.Get();
	if (!Extracted.IsValid() || Asset == nullptr)
	{
		return FString();
	}
	const FString Baseline = Serialize(Extracted);
	Extracted->SetStringField(TEXT("Action"), TEXT("Update"));
	const FString Update = Serialize(Extracted);
	Test.AddInfo(FString::Printf(TEXT("%s: extracted %s"), Stage, *Update));
	Test.TestFalse(*FString::Printf(TEXT("%s: package is saved clean"), Stage), Asset->GetOutermost()->IsDirty());
	const FDateTime SavedStamp = IFileManager::Get().GetTimeStamp(*Target.PackageFile);
	const FAssetDocumentResult Reapplied = Apply(Test, Target, Update, Stage);
	Test.TestTrue(*FString::Printf(TEXT("%s: extraction re-applies"), Stage), Reapplied.IsSuccess());
	Test.TestFalse(*FString::Printf(TEXT("%s: no-op re-apply reports no save"), Stage), Reapplied.bSavedAsset);
	Test.TestFalse(*FString::Printf(TEXT("%s: no-op re-apply leaves the package clean"), Stage), Asset->GetOutermost()->IsDirty());
	Test.TestEqual(*FString::Printf(TEXT("%s: no-op re-apply does not rewrite the package file"), Stage),
		IFileManager::Get().GetTimeStamp(*Target.PackageFile), SavedStamp);
	Test.TestTrue(*FString::Printf(TEXT("%s: same asset object"), Stage), Target.Get() == Asset);
	ExtractEquals(Test, Target, Baseline, Stage);
	TestNoDiff(Test, Update, Stage);
	return Baseline;
}

void TestNothingCreated(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Stage)
{
	Test.TestNull(*FString::Printf(TEXT("%s: no asset remains"), Stage), Target.Get());
	Test.TestFalse(*FString::Printf(TEXT("%s: no package file"), Stage), IFileManager::Get().FileExists(*Target.PackageFile));
	Test.TestFalse(*FString::Printf(TEXT("%s: no registry entry"), Stage),
		FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Target.ObjectPath)).IsValid());
}

FString Document(const FTarget& Target, const TCHAR* Action, const FString& Body)
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"%s\",\"Action\":\"%s\",\"Properties\":{},\"Body\":{%s}}"),
		*Target.PackageName, StateTreeClass, Action, *Body);
}

FString CompileStatus(const FAssetDocumentResult& Result)
{
	FString Status;
	if (Result.Compile.IsValid())
	{
		Result.Compile->TryGetStringField(TEXT("Status"), Status);
	}
	return Status;
}

UStateTreeEditorData* EditorDataOf(const UStateTree* Tree)
{
	return Tree != nullptr ? Cast<UStateTreeEditorData>(Tree->EditorData) : nullptr;
}

// The state at a slash-separated path of names ("Root/Idle").
UStateTreeState* FindState(const UStateTree* Tree, const FString& Path)
{
	const UStateTreeEditorData* EditorData = EditorDataOf(Tree);
	if (EditorData == nullptr)
	{
		return nullptr;
	}
	TArray<FString> Names;
	Path.ParseIntoArray(Names, TEXT("/"));
	const TArray<TObjectPtr<UStateTreeState>>* Level = &EditorData->SubTrees;
	UStateTreeState* Found = nullptr;
	for (const FString& Name : Names)
	{
		const TObjectPtr<UStateTreeState>* Match = Level->FindByPredicate([&Name](const TObjectPtr<UStateTreeState>& State)
		{
			return State != nullptr && State->Name.ToString() == Name;
		});
		if (Match == nullptr)
		{
			return nullptr;
		}
		Found = Match->Get();
		Level = &Found->Children;
	}
	return Found;
}

FString ChildNames(const UStateTreeState* State)
{
	TArray<FString> Names;
	if (State != nullptr)
	{
		for (const TObjectPtr<UStateTreeState>& Child : State->Children)
		{
			Names.Add(Child != nullptr ? Child->Name.ToString() : FString(TEXT("<null>")));
		}
	}
	return FString::Join(Names, TEXT(","));
}

const FStateTreeEditorNode* FindTask(const UStateTreeState* State, const TCHAR* Name)
{
	return State != nullptr ? State->Tasks.FindByPredicate([Name](const FStateTreeEditorNode& Node) { return Node.GetName() == FName(Name); }) : nullptr;
}

const TCHAR* LinkedBody = TEXT(R"json(
"Schema":{"Class":"/Script/GameplayStateTreeModule.StateTreeComponentSchema"},
"Parameters":[{"Name":"Speed","Type":"float","Value":1.0}],
"States":[{"Name":"Root","Tasks":[{"Type":"/Script/StateTreeModule.StateTreeDelayTask","Name":"Hold"}]}])json");

// %LINKED% is the linked tree's object path.
const TCHAR* CreatedBody = TEXT(R"json(
"Schema":{"Class":"/Script/GameplayStateTreeModule.StateTreeComponentSchema","Properties":{"ContextActorClass":"/Script/Engine.Pawn"}},
"Parameters":[{"Name":"WaitTime","Type":"float","Value":2.0},{"Name":"Limit","Type":"int","Value":3},{"Name":"Label","Type":"string","Value":"hello"}],
"Evaluators":[{"Type":"/Script/AssetDocumentTests.AssetDocumentStateTreeTestEvaluator","Name":"Sensor","Instance":{"Threshold":0.5}}],
"GlobalTasks":[{"Type":"/Script/StateTreeModule.StateTreeDelayTask","Name":"Forever","Instance":{"bRunForever":true}}],
"States":[
 {"Name":"Root","Children":[
  {"Name":"Idle","Description":"Waits",
   "EnterConditions":[{"Type":"/Script/StateTreeModule.StateTreeCompareIntCondition","Name":"Enough","Instance":{"Right":2},"Bindings":{"Left":"Parameters:Limit"}}],
   "Tasks":[
    {"Type":"/Script/StateTreeModule.StateTreeDelayTask","Name":"Wait","Instance":{"RandomDeviation":0.5},"Bindings":{"Duration":"Parameters:WaitTime"}},
    {"Type":"/Script/StateTreeModule.StateTreeDebugTextTask","Name":"Say","Node":{"Text":"Idle","FontScale":2},
     "Bindings":{"ReferenceActor":"Context/Actor","BindableText":"Parameters:Label"}}],
   "Transitions":[
    {"Trigger":"OnStateCompleted","To":"Root/Walk"},
    {"Trigger":"OnEvent","Event":{"Payload":"/Script/CoreUObject.Vector"},"To":"Succeeded","Priority":"High","Delay":{"Duration":0.5}},
    {"Trigger":"OnTick","To":"NextState","Conditions":[{"Type":"/Script/StateTreeModule.StateTreeCompareIntCondition","Instance":{"Right":5},
      "Bindings":{"Left":"Evaluators/Sensor:Count"}}]},
    {"Trigger":"OnStateFailed","To":"Failed"}]},
  {"Name":"Walk","Weight":2,"TickRate":0.5,
   "Tasks":[{"Type":"/Script/StateTreeModule.StateTreeDelayTask","Name":"Step",
     "Bindings":{"Duration":{"Function":"/Script/StateTreeModule.StateTreeAddFloatPropertyFunction","Instance":{"Right":1},
       "Bindings":{"Left":"Parameters:WaitTime"}}}}],
   "Transitions":[{"Trigger":"OnStateSucceeded","To":"NextSelectableState"},{"Trigger":"OnTick","To":"None"}]},
  {"Name":"Choose","Type":"Group","Selection":"TrySelectChildrenWithHighestUtility","Children":[
   {"Name":"A","Considerations":[{"Type":"/Script/StateTreeModule.StateTreeConstantConsideration","Instance":{"Constant":0.75}}]},
   {"Name":"B","Enabled":false,"Considerations":[{"Type":"/Script/StateTreeModule.StateTreeConstantConsideration","Instance":{"Constant":0.25}}]}]},
  {"Name":"CallSub","Type":"Linked","LinkedSubtree":"Sub","ParameterValues":{"Speed":2.5}},
  {"Name":"CallAsset","Type":"LinkedAsset","LinkedAsset":"%LINKED%","ParameterBindings":{"Speed":"Parameters:WaitTime"}}]},
 {"Name":"Sub","Type":"Subtree","Parameters":[{"Name":"Speed","Type":"float","Value":1.0}],
  "Tasks":[{"Type":"/Script/StateTreeModule.StateTreeDelayTask","Name":"Pace","Bindings":{"Duration":"Sub/Parameters:Speed"}}]}])json");

// Rename Walk -> Run, move Choose/B to Root, remove CallSub; %IDLE% is the Idle entry.
const TCHAR* EditedBody = TEXT(R"json(
"States":[
 {"Name":"Root","Children":[
  %IDLE%,
  {"Name":"Run","RenamedFrom":"Walk"},
  {"Name":"Choose","Children":[{"Name":"A"}]},
  {"Name":"B","RenamedFrom":"Root/Choose/B"},
  {"Name":"CallAsset"}]},
 {"Name":"Sub"}])json");

// Edit a task value and rebind it; the transitions (omitted) are kept and follow Walk's ID.
const TCHAR* EditedIdle = TEXT(R"json({"Name":"Idle","Tasks":[
 {"Type":"/Script/StateTreeModule.StateTreeDelayTask","Name":"Wait","Instance":{"RandomDeviation":0.25},"Bindings":{"Duration":"Evaluators/Sensor:Distance"}},
 {"Type":"/Script/StateTreeModule.StateTreeDebugTextTask","Name":"Say"}]})json");

FString Edited(const TCHAR* Idle)
{
	return FString(EditedBody).Replace(TEXT("%IDLE%"), Idle);
}
} // namespace AssetDocumentStateTreePythonTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentStateTreePythonTest, "UEAssetDoc.StateTree.Python",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentStateTreePythonTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentStateTreePythonTests;
	const FTarget Target(TEXT("ST_Brain"));
	const FTarget Linked(TEXT("ST_Linked"));
	const FTarget NoSchema(TEXT("ST_NoSchema"));
	const FTarget BadSchema(TEXT("ST_BadSchema"));
	const FTarget HiddenType(TEXT("ST_HiddenType"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		Linked.Cleanup();
		NoSchema.Cleanup();
		BadSchema.Cleanup();
		HiddenType.Cleanup();
	};

	// 0. The test evaluator is invisible to users: the node class cache knows it, but it is Hidden, so
	//    the node picker (SStateTreeNodeTypePicker::CacheNodeTypes skips Hidden structs) does not
	//    offer it, and the profile refuses it the same way.
	{
		const UScriptStruct* TestEvaluator = FAssetDocumentStateTreeTestEvaluator::StaticStruct();
		TestTrue(TEXT("test evaluator is Hidden"), TestEvaluator->HasMetaData(TEXT("Hidden")));
		TArray<TSharedPtr<FStateTreeNodeClassData>> Evaluators;
		FModuleManager::GetModuleChecked<FStateTreeEditorModule>(TEXT("StateTreeEditorModule")).GetNodeClassCache()->GetScripStructs(
			FStateTreeEvaluatorBase::StaticStruct(), Evaluators);
		bool bCached = false;
		bool bOffered = false;
		for (const TSharedPtr<FStateTreeNodeClassData>& Data : Evaluators)
		{
			const UStruct* Struct = Data.IsValid() ? Data->GetStruct(true) : nullptr;
			bCached |= Struct == TestEvaluator;
			bOffered |= Struct == TestEvaluator && !Struct->HasMetaData(TEXT("Hidden"));
		}
		TestTrue(TEXT("node class cache knows the test evaluator"), bCached);
		TestFalse(TEXT("node picker does not offer the test evaluator"), bOffered);
		TestFailsWith(*this, Apply(*this, HiddenType, Document(HiddenType, TEXT("Create"), TEXT(R"json(
"Schema":{"Class":"/Script/GameplayStateTreeModule.StateTreeComponentSchema"},
"Evaluators":[{"Type":"/Script/AssetDocumentTests.AssetDocumentStateTreeTestEvaluator","Name":"Sensor"}],
"States":[{"Name":"Root"}])json")), TEXT("HiddenType")), TEXT("StateTree.UnknownType"), TEXT("/Body/Evaluators/0/Type"), TEXT("hidden type"));
		TestNothingCreated(*this, HiddenType, TEXT("hidden type"));
	}
	TOptional<FScopedTestTypesVisible> TestTypesVisible;
	TestTypesVisible.Emplace();

	// 1. The linked tree, then the tree: one apply each, created with the schema and compiled.
	if (!TestTrue(TEXT("linked tree applies"), Apply(*this, Linked, Document(Linked, TEXT("Create"), LinkedBody), TEXT("CreateLinked")).IsSuccess()))
	{
		return false;
	}
	const FString Created = Document(Target, TEXT("Create"), FString(CreatedBody).Replace(TEXT("%LINKED%"), *Linked.ObjectPath));
	const FAssetDocumentResult CreateResult = Apply(*this, Target, Created, TEXT("Create"));
	if (!TestTrue(TEXT("create applies"), CreateResult.IsSuccess()))
	{
		return false;
	}
	UStateTree* Tree = Target.Get();
	UStateTreeEditorData* EditorData = EditorDataOf(Tree);
	if (!TestNotNull(TEXT("StateTree with editor data exists"), EditorData))
	{
		return false;
	}
	TestTrue(TEXT("package is saved"), IFileManager::Get().FileExists(*Target.PackageFile));
	TestTrue(TEXT("schema is the dialog's choice"), EditorData->Schema != nullptr
		&& EditorData->Schema->GetClass()->GetPathName() == TEXT("/Script/GameplayStateTreeModule.StateTreeComponentSchema"));
	TestTrue(TEXT("compiled"), Tree->LastCompiledEditorDataHash != 0);
	TestTrue(TEXT("compile status is reported"), CompileStatus(CreateResult).StartsWith(TEXT("UpToDate")));
	TestEqual(TEXT("root children in document order"), ChildNames(FindState(Tree, TEXT("Root"))), FString(TEXT("Idle,Walk,Choose,CallSub,CallAsset")));
	TestEqual(TEXT("subtrees"), EditorData->SubTrees.Num(), 2);
	TestEqual(TEXT("evaluators"), EditorData->Evaluators.Num(), 1);
	TestEqual(TEXT("global tasks"), EditorData->GlobalTasks.Num(), 1);
	UStateTreeState* Idle = FindState(Tree, TEXT("Root/Idle"));
	UStateTreeState* Walk = FindState(Tree, TEXT("Root/Walk"));
	UStateTreeState* Sub = FindState(Tree, TEXT("Sub"));
	UStateTreeState* B = FindState(Tree, TEXT("Root/Choose/B"));
	const UStateTreeState* CallSub = FindState(Tree, TEXT("Root/CallSub"));
	const UStateTreeState* CallAsset = FindState(Tree, TEXT("Root/CallAsset"));
	const UStateTreeState* Choose = FindState(Tree, TEXT("Root/Choose"));
	if (!TestTrue(TEXT("states exist"), Idle && Walk && Sub && B && CallSub && CallAsset && Choose))
	{
		return false;
	}
	TestEqual(TEXT("enter conditions"), Idle->EnterConditions.Num(), 1);
	TestEqual(TEXT("tasks"), Idle->Tasks.Num(), 2);
	TestTrue(TEXT("state description"), Idle->Description == TEXT("Waits"));
	TestTrue(TEXT("group type and utility selection"), Choose->Type == EStateTreeStateType::Group
		&& Choose->SelectionBehavior == EStateTreeStateSelectionBehavior::TrySelectChildrenWithHighestUtility);
	TestTrue(TEXT("considerations and disabled state"), B->Considerations.Num() == 1 && !B->bEnabled);
	TestTrue(TEXT("weight and custom tick rate"), Walk->Weight == 2.f && Walk->bHasCustomTickRate && Walk->CustomTickRate == 0.5f);
	TestTrue(TEXT("linked subtree"), CallSub->Type == EStateTreeStateType::Linked && CallSub->LinkedSubtree.ID == Sub->ID);
	TestTrue(TEXT("subtree type"), Sub->Type == EStateTreeStateType::Subtree);
	TestTrue(TEXT("linked asset"), CallAsset->Type == EStateTreeStateType::LinkedAsset && CallAsset->LinkedAsset == Linked.Get());
	if (TestEqual(TEXT("Idle transitions"), Idle->Transitions.Num(), 4))
	{
		TestTrue(TEXT("goto state targets Walk's ID"), Idle->Transitions[0].State.LinkType == EStateTreeTransitionType::GotoState
			&& Idle->Transitions[0].State.ID == Walk->ID);
		const FStateTreeTransition& OnEvent = Idle->Transitions[1];
		TestTrue(TEXT("event transition"), OnEvent.Trigger == EStateTreeTransitionTrigger::OnEvent
			&& OnEvent.RequiredEvent.PayloadStruct == TBaseStructure<FVector>::Get() && OnEvent.Priority == EStateTreeTransitionPriority::High
			&& OnEvent.bDelayTransition && OnEvent.DelayDuration == 0.5f && OnEvent.State.LinkType == EStateTreeTransitionType::Succeeded);
		TestTrue(TEXT("next state with condition"), Idle->Transitions[2].State.LinkType == EStateTreeTransitionType::NextState
			&& Idle->Transitions[2].Conditions.Num() == 1);
		TestTrue(TEXT("failed"), Idle->Transitions[3].State.LinkType == EStateTreeTransitionType::Failed);
	}
	if (TestEqual(TEXT("Walk transitions"), Walk->Transitions.Num(), 2))
	{
		TestTrue(TEXT("next selectable state"), Walk->Transitions[0].State.LinkType == EStateTreeTransitionType::NextSelectableState);
		TestTrue(TEXT("none"), Walk->Transitions[1].State.LinkType == EStateTreeTransitionType::None);
	}
	const FGuid WalkId = Walk->ID;
	const FGuid BId = B->ID;
	const FStateTreeEditorNode* Wait = FindTask(Idle, TEXT("Wait"));
	const FGuid WaitId = Wait != nullptr ? Wait->ID : FGuid();
	TestTrue(TEXT("Wait has its instance data"), Wait != nullptr && Wait->Instance.GetScriptStruct() != nullptr);

	// 2. Extract / Diff agree, the extraction carries every binding kind, and re-applying it is a no-op.
	TestNoDiff(*this, Created, TEXT("created"));
	const FString Baseline = TestNoOpReapply(*this, Target, TEXT("NoOp"));
	for (const TCHAR* Expected : {
		TEXT("\"Duration\":\"Parameters:WaitTime\""), TEXT("\"ReferenceActor\":\"Context/Actor\""), TEXT("\"Left\":\"Evaluators/Sensor:Count\""),
		TEXT("\"Function\":\"/Script/StateTreeModule.StateTreeAddFloatPropertyFunction\""), TEXT("\"Speed\":\"Parameters:WaitTime\""),
		TEXT("\"Duration\":\"Sub/Parameters:Speed\""), TEXT("\"To\":\"Root/Walk\""), TEXT("\"LinkedSubtree\":\"Sub\"")})
	{
		TestTrue(*FString::Printf(TEXT("extraction carries %s"), Expected), Baseline.Contains(Expected));
	}
	{
		FAssetDocumentInspectRequest Request;
		Request.ClassOrAsset = Target.ObjectPath;
		const FAssetDocumentResult Inspected = FAssetDocumentModule::Get().GetService().Inspect(Request);
		TestTrue(TEXT("inspect lists the tree details"), Inspected.IsSuccess() && Serialize(Inspected.Payload).Contains(TEXT("\"TreeDetails\"")));
	}

	// 3. Rename (transitions follow the ID), move, remove, edit a task value and rebind.
	const FAssetDocumentResult EditResult = Apply(*this, Target, Document(Target, TEXT("Update"), Edited(EditedIdle)), TEXT("Edit"));
	if (!TestTrue(TEXT("edit applies"), EditResult.IsSuccess()))
	{
		return false;
	}
	Tree = Target.Get();
	Idle = FindState(Tree, TEXT("Root/Idle"));
	const UStateTreeState* Run = FindState(Tree, TEXT("Root/Run"));
	const UStateTreeState* Moved = FindState(Tree, TEXT("Root/B"));
	TestEqual(TEXT("edited root children"), ChildNames(FindState(Tree, TEXT("Root"))), FString(TEXT("Idle,Run,Choose,B,CallAsset")));
	TestEqual(TEXT("Choose keeps A"), ChildNames(FindState(Tree, TEXT("Root/Choose"))), FString(TEXT("A")));
	TestTrue(TEXT("renamed state keeps its ID"), Run != nullptr && Run->ID == WalkId);
	TestTrue(TEXT("moved state keeps its ID"), Moved != nullptr && Moved->ID == BId);
	TestTrue(TEXT("transition follows the renamed state"), Idle != nullptr && Idle->Transitions.Num() == 4
		&& Idle->Transitions[0].State.ID == WalkId);
	Wait = FindTask(Idle, TEXT("Wait"));
	TestTrue(TEXT("matched task keeps its ID"), Wait != nullptr && Wait->ID == WaitId);
	const FString AfterEdit = TestNoOpReapply(*this, Target, TEXT("NoOpAfterEdit"));
	TestTrue(TEXT("transition extracts to the new path"), AfterEdit.Contains(TEXT("\"To\":\"Root/Run\"")));
	TestTrue(TEXT("task value edited"), AfterEdit.Contains(TEXT("\"RandomDeviation\":0.25")));
	TestTrue(TEXT("task rebound"), AfterEdit.Contains(TEXT("\"Duration\":\"Evaluators/Sensor:Distance\"")));
	TestFalse(TEXT("removed state is gone"), AfterEdit.Contains(TEXT("CallSub")));

	// 4. Invalid documents fail with the document path and change nothing.
	struct FInvalid
	{
		const TCHAR* Stage;
		const TCHAR* IdleEntry;
		const TCHAR* Code;
		const TCHAR* Path;
	};
	const FInvalid Invalid[] = {
		{TEXT("UnknownNodeStruct"), TEXT(R"json({"Name":"Idle","Tasks":[{"Type":"/Script/StateTreeModule.StateTreeDelayTask","Name":"Wait"},
			{"Type":"/Script/StateTreeModule.StateTreeDebugTextTask","Name":"Say"},{"Type":"/Script/StateTreeModule.AssetDocumentNoSuchTask"}]})json"),
			TEXT("StateTree.UnknownType"), TEXT("/Body/States/0/Children/0/Tasks/2/Type")},
		{TEXT("BindingTypeMismatch"), TEXT(R"json({"Name":"Idle","Tasks":[{"Type":"/Script/StateTreeModule.StateTreeDelayTask","Name":"Wait",
			"Bindings":{"Duration":"Parameters:Label"}},{"Type":"/Script/StateTreeModule.StateTreeDebugTextTask","Name":"Say"}]})json"),
			TEXT("StateTree.BindingTypeMismatch"), TEXT("/Body/States/0/Children/0/Tasks/0/Bindings/Duration")},
		{TEXT("MissingTransitionTarget"), TEXT(R"json({"Name":"Idle","Transitions":[{"Trigger":"OnStateCompleted","To":"Root/Nowhere"}]})json"),
			TEXT("StateTree.TransitionTargetNotFound"), TEXT("/Body/States/0/Children/0/Transitions/0/To")},
	};
	for (const FInvalid& Case : Invalid)
	{
		// Validate reports what Apply reports (its trial apply edits a discarded copy) and changes nothing.
		const FAssetDocumentUndoHistoryMark UndoBefore = UE::AssetDocument::UndoHistory::Capture();
		TestFailsWith(*this, Validate(*this, Document(Target, TEXT("Update"), Edited(Case.IdleEntry)), Case.Stage), Case.Code, Case.Path,
			*FString::Printf(TEXT("%s (validate)"), Case.Stage));
		TestTrue(*FString::Printf(TEXT("%s: validate leaves the undo history"), Case.Stage), UE::AssetDocument::UndoHistory::Capture() == UndoBefore);
		TestTrue(*FString::Printf(TEXT("%s: validate leaves the package clean"), Case.Stage), Target.Get() != nullptr && !Target.Get()->GetOutermost()->IsDirty());
		const FAssetDocumentResult Failed = Apply(*this, Target, Document(Target, TEXT("Update"), Edited(Case.IdleEntry)), Case.Stage);
		TestFailsWith(*this, Failed, Case.Code, Case.Path, Case.Stage);
		TestTrue(*FString::Printf(TEXT("%s: package is clean"), Case.Stage), Target.Get() != nullptr && !Target.Get()->GetOutermost()->IsDirty());
		ExtractEquals(*this, Target, AfterEdit, Case.Stage);
	}

	// 5. A compile error (an Input property without a binding) fails the apply; the tree rolls back
	//    and is compiled again.
	AddExpectedMessage(TEXT("Failed to compile"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0, false);
	AddExpectedMessage(TEXT("is expected to have a binding"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	const FString CompileErrorDocument = Document(Target, TEXT("Update"), Edited(TEXT(R"json({"Name":"Idle",
		"EnterConditions":[{"Type":"/Script/StateTreeModule.StateTreeCompareIntCondition","Name":"Enough"},
		{"Type":"/Script/StateTreeModule.StateTreeCompareIntCondition","Name":"Unbound"}]})json")));
	TestFailsWith(*this, Validate(*this, CompileErrorDocument, TEXT("CompileError")), TEXT("StateTree.CompileError"), TEXT(""),
		TEXT("compile error (validate)"));
	TestTrue(TEXT("compile error: validate leaves the package clean"), Target.Get() != nullptr && !Target.Get()->GetOutermost()->IsDirty());
	const FAssetDocumentResult CompileFailed = Apply(*this, Target, CompileErrorDocument, TEXT("CompileError"));
	TestFailsWith(*this, CompileFailed, TEXT("StateTree.CompileError"), TEXT(""), TEXT("compile error"));
	Tree = Target.Get();
	TestTrue(TEXT("rolled-back package is clean"), Tree != nullptr && !Tree->GetOutermost()->IsDirty());
	TestTrue(TEXT("rolled-back tree is compiled"), Tree != nullptr && Tree->LastCompiledEditorDataHash != 0);
	const UStateTreeState* RolledBackIdle = FindState(Tree, TEXT("Root/Idle"));
	TestEqual(TEXT("rolled-back enter conditions"), RolledBackIdle != nullptr ? RolledBackIdle->EnterConditions.Num() : INDEX_NONE, 1);
	ExtractEquals(*this, Target, AfterEdit, TEXT("after compile error rollback"));

	// 6. The saved package, unloaded and loaded back from disk, extracts to the same document.
	ReleaseUndoHistory();
	if (Tree == nullptr)
	{
		return false;
	}
	UPackageTools::UnloadPackages({Tree->GetOutermost()});
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestNull(TEXT("tree is unloaded"), Target.Get());
	ExtractEquals(*this, Target, AfterEdit, TEXT("after reload from disk"));

	// 7. Failed creates leave nothing behind.
	TestFailsWith(*this, Apply(*this, NoSchema, Document(NoSchema, TEXT("Create"), TEXT("\"States\":[]")), TEXT("NoSchema")),
		TEXT("StateTree.SchemaRequired"), TEXT("/Body/Schema/Class"), TEXT("no schema"));
	TestNothingCreated(*this, NoSchema, TEXT("no schema"));
	TestFailsWith(*this, Apply(*this, BadSchema, Document(BadSchema, TEXT("Create"), TEXT("\"Schema\":{\"Class\":\"/Script/Engine.Actor\"}")),
		TEXT("BadSchema")), TEXT("StateTree.SchemaNotFound"), TEXT("/Body/Schema/Class"), TEXT("not a schema"));
	TestNothingCreated(*this, BadSchema, TEXT("not a schema"));

	// 8. With the evaluator's type hidden again (as for any type that becomes hidden after authoring),
	//    the existing node is kept: the extraction re-applies as a no-op and the node and its siblings
	//    stay editable. Only a new node or a type change must use a type the picker offers.
	TestTypesVisible.Reset();
	const FString HiddenBaseline = TestNoOpReapply(*this, Target, TEXT("HiddenNoOp"));
	TestTrue(TEXT("extraction lists the hidden-typed node"), HiddenBaseline.Contains(TEXT("AssetDocumentStateTreeTestEvaluator")));
	const TCHAR* KeptEvaluators = TEXT(R"json("Evaluators":[{"Type":"/Script/AssetDocumentTests.AssetDocumentStateTreeTestEvaluator","Name":"Sensor","Instance":{"Threshold":0.75}}])json");
	const FAssetDocumentResult HiddenEdit = Apply(*this, Target, Document(Target, TEXT("Update"), FString(KeptEvaluators)
		+ TEXT(R"json(,"GlobalTasks":[{"Type":"/Script/StateTreeModule.StateTreeDelayTask","Name":"Forever","Instance":{"RandomDeviation":0.125}}])json")),
		TEXT("HiddenEdit"));
	TestTrue(TEXT("hidden-typed node and its sibling are edited"), HiddenEdit.IsSuccess());
	const FString AfterHiddenEdit = TestNoOpReapply(*this, Target, TEXT("HiddenNoOpAfterEdit"));
	TestTrue(TEXT("hidden-typed node value edited"), AfterHiddenEdit.Contains(TEXT("\"Threshold\":0.75")));
	TestTrue(TEXT("sibling value edited"), AfterHiddenEdit.Contains(TEXT("\"RandomDeviation\":0.125")));
	struct FHiddenCase
	{
		const TCHAR* Stage;
		const TCHAR* Body;
		const TCHAR* Path;
	};
	const FHiddenCase HiddenCases[] = {
		{TEXT("NewHiddenNode"), TEXT(R"json("Evaluators":[{"Type":"/Script/AssetDocumentTests.AssetDocumentStateTreeTestEvaluator","Name":"Sensor"},
			{"Type":"/Script/AssetDocumentTests.AssetDocumentStateTreeTestEvaluator","Name":"Probe"}])json"), TEXT("/Body/Evaluators/1/Type")},
		{TEXT("ChangeToHiddenType"), TEXT(R"json("GlobalTasks":[{"Type":"/Script/GameplayStateTreeModule.StateTreeAITaskBase","Name":"Forever"}])json"),
			TEXT("/Body/GlobalTasks/0/Type")},
	};
	for (const FHiddenCase& Case : HiddenCases)
	{
		TestFailsWith(*this, Apply(*this, Target, Document(Target, TEXT("Update"), Case.Body), Case.Stage), TEXT("StateTree.UnknownType"), Case.Path,
			Case.Stage);
		TestTrue(*FString::Printf(TEXT("%s: package is clean"), Case.Stage), Target.Get() != nullptr && !Target.Get()->GetOutermost()->IsDirty());
		ExtractEquals(*this, Target, AfterHiddenEdit, Case.Stage);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentStateTreeNodeConstructionTest, "UEAssetDoc.StateTree.NodeConstruction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentStateTreeNodeConstructionTest::RunTest(const FString& Parameters)
{
	// The editor's own construction (UStateTreeState::AddEnterCondition<T>, UStateTreeEditorData::
	// AddEvaluator<T>) and InitializeNode give the same node, instance and runtime data.
	UStateTree* Tree = NewObject<UStateTree>(GetTransientPackage(), NAME_None, RF_Transient);
	UStateTreeEditorData* EditorData = NewObject<UStateTreeEditorData>(Tree, NAME_None, RF_Transient);
	Tree->EditorData = EditorData;
	ON_SCOPE_EXIT
	{
		EditorData->MarkAsGarbage();
		Tree->MarkAsGarbage();
	};
	UStateTreeState& State = EditorData->AddRootState();
	auto Compare = [this](const TCHAR* Label, const FStateTreeEditorNode& Editor, const FStateTreeEditorNode& Raw)
	{
		TestTrue(*FString::Printf(TEXT("%s: raw node has an ID"), Label), Raw.ID.IsValid());
		TestTrue(*FString::Printf(TEXT("%s: node struct"), Label), Raw.Node.GetScriptStruct() == Editor.Node.GetScriptStruct());
		TestTrue(*FString::Printf(TEXT("%s: instance struct"), Label), Raw.Instance.GetScriptStruct() == Editor.Instance.GetScriptStruct());
		TestTrue(*FString::Printf(TEXT("%s: runtime data struct"), Label),
			Raw.ExecutionRuntimeData.GetScriptStruct() == Editor.ExecutionRuntimeData.GetScriptStruct());
		TestTrue(*FString::Printf(TEXT("%s: no instance objects"), Label), Raw.InstanceObject == nullptr && Editor.InstanceObject == nullptr);
		TestTrue(*FString::Printf(TEXT("%s: same node values"), Label), Raw.Node.Identical(&Editor.Node, PPF_None));
		TestTrue(*FString::Printf(TEXT("%s: same instance values"), Label), Raw.Instance.Identical(&Editor.Instance, PPF_None));
	};
	{
		const FStateTreeEditorNode Editor = State.AddEnterCondition<FStateTreeCompareIntCondition>();
		FStateTreeEditorNode Raw;
		TestTrue(TEXT("condition initializes"), UE::AssetDocument::Graph::StateTree::InitializeNode(
			Raw, *FStateTreeCompareIntCondition::StaticStruct(), FGuid::NewGuid(), State));
		Compare(TEXT("condition"), Editor, Raw);
	}
	{
		const FStateTreeEditorNode Editor = EditorData->AddEvaluator<FAssetDocumentStateTreeTestEvaluator>();
		FStateTreeEditorNode Raw;
		TestTrue(TEXT("evaluator initializes"), UE::AssetDocument::Graph::StateTree::InitializeNode(
			Raw, *FAssetDocumentStateTreeTestEvaluator::StaticStruct(), FGuid::NewGuid(), *EditorData));
		Compare(TEXT("evaluator"), Editor, Raw);
	}
	{
		FStateTreeEditorNode Raw;
		TestFalse(TEXT("a non-node class is refused"), UE::AssetDocument::Graph::StateTree::InitializeNode(
			Raw, *UStateTreeState::StaticClass(), FGuid::NewGuid(), State));
	}
	// A Blueprint node class becomes its kind's wrapper struct naming the class, with an instance of
	// the class (outered to the given owner) as the node's instance object.
	UPackage* BlueprintPackage = CreatePackage(*FString::Printf(TEXT("/Temp/AssetDocumentAI/StateTreeNodeBlueprints_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	ON_SCOPE_EXIT
	{
		BlueprintPackage->MarkAsGarbage();
	};
	auto MakeNodeBlueprintClass = [BlueprintPackage](UClass* Base, const TCHAR* Name) -> UClass*
	{
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(Base, BlueprintPackage, FName(Name), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		return Blueprint != nullptr ? Blueprint->GeneratedClass.Get() : nullptr;
	};
	UClass* TaskClass = MakeNodeBlueprintClass(UStateTreeTaskBlueprintBase::StaticClass(), TEXT("BPT_NodeConstruction"));
	UClass* ConditionClass = MakeNodeBlueprintClass(UStateTreeConditionBlueprintBase::StaticClass(), TEXT("BPC_NodeConstruction"));
	TestNotNull(TEXT("Blueprint task class is created"), TaskClass);
	TestNotNull(TEXT("Blueprint condition class is created"), ConditionClass);
	if (TaskClass != nullptr)
	{
		FStateTreeEditorNode Raw;
		const FGuid Id = FGuid::NewGuid();
		TestTrue(TEXT("Blueprint task initializes"), UE::AssetDocument::Graph::StateTree::InitializeNode(Raw, *TaskClass, Id, State));
		const FStateTreeBlueprintTaskWrapper* Wrapper = Raw.Node.GetPtr<FStateTreeBlueprintTaskWrapper>();
		TestTrue(TEXT("Blueprint task: wrapper struct"), Raw.Node.GetScriptStruct() == FStateTreeBlueprintTaskWrapper::StaticStruct());
		TestTrue(TEXT("Blueprint task: wrapper names the class"), Wrapper != nullptr && Wrapper->TaskClass.Get() == TaskClass);
		TestTrue(TEXT("Blueprint task: instance object of the class owned by the state"),
			Raw.InstanceObject != nullptr && Raw.InstanceObject->GetClass() == TaskClass && Raw.InstanceObject->GetOuter() == &State);
		TestFalse(TEXT("Blueprint task: no instance struct"), Raw.Instance.IsValid());
		TestTrue(TEXT("Blueprint task: ID kept"), Raw.ID == Id);
	}
	if (ConditionClass != nullptr)
	{
		FStateTreeEditorNode Raw;
		TestTrue(TEXT("Blueprint condition initializes"),
			UE::AssetDocument::Graph::StateTree::InitializeNode(Raw, *ConditionClass, FGuid::NewGuid(), State));
		const FStateTreeBlueprintConditionWrapper* Wrapper = Raw.Node.GetPtr<FStateTreeBlueprintConditionWrapper>();
		TestTrue(TEXT("Blueprint condition: wrapper names the class"), Wrapper != nullptr && Wrapper->ConditionClass.Get() == ConditionClass);
		TestTrue(TEXT("Blueprint condition: instance object of the class"),
			Raw.InstanceObject != nullptr && Raw.InstanceObject->GetClass() == ConditionClass);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentStateTreeUndoTest, "UEAssetDoc.StateTree.Undo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentStateTreeUndoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentStateTreePythonTests;
	const FTarget Target(TEXT("ST_Undo"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	const TCHAR* Body = TEXT(R"json("Schema":{"Class":"/Script/GameplayStateTreeModule.StateTreeComponentSchema"},
		"Parameters":[{"Name":"WaitTime","Type":"float","Value":2.0}],
		"States":[{"Name":"Root","Children":[
		 {"Name":"Idle","Tasks":[{"Type":"/Script/StateTreeModule.StateTreeDelayTask","Name":"Wait","Instance":{"RandomDeviation":%DEVIATION%},
		   "Bindings":{"Duration":"Parameters:WaitTime"}}],%EXTRA%"Transitions":[{"Trigger":"OnStateCompleted","To":"Root/Walk"}]},
		 {"Name":"Walk"}]}])json");
	auto Make = [&Target, Body](const TCHAR* Action, const TCHAR* Deviation, const TCHAR* Extra)
	{
		return Document(Target, Action, FString(Body).Replace(TEXT("%DEVIATION%"), Deviation).Replace(TEXT("%EXTRA%"), Extra));
	};
	if (GEditor == nullptr || GEditor->Trans == nullptr
		|| !TestTrue(TEXT("create applies"), Apply(*this, Target, Make(TEXT("Create"), TEXT("0.5"), TEXT("")), TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	ReleaseUndoHistory();
	auto Idle = [&Target]() { return FindState(Target.Get(), TEXT("Root/Idle")); };
	auto SetDescription = [&Idle](const TCHAR* Text)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentStateTreeTests", "UserEdit", "Edit State"));
		UStateTreeState* State = Idle();
		State->Modify();
		State->Description = Text;
		FPropertyChangedEvent Changed(UStateTreeState::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UStateTreeState, Description)));
		State->PostEditChangeProperty(Changed);
	};
	// Undo (then redo) everything the history holds; the tree must stay whole at every step.
	auto UndoRedoAll = [this, &Target](const TCHAR* Stage)
	{
		int32 Undone = 0;
		while (Undone < 16 && GEditor->Trans->CanUndo() && GEditor->UndoTransaction())
		{
			++Undone;
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			TestTrue(*FString::Printf(TEXT("%s: tree whole after undo %d"), Stage, Undone),
				FindState(Target.Get(), TEXT("Root/Idle")) != nullptr && FindState(Target.Get(), TEXT("Root/Walk")) != nullptr);
		}
		int32 Redone = 0;
		while (Redone < Undone && GEditor->Trans->CanRedo() && GEditor->RedoTransaction())
		{
			++Redone;
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		TestEqual(*FString::Printf(TEXT("%s: every undone step redoes"), Stage), Redone, Undone);
	};

	// 1. The human edits a state in the StateTree editor and saves.
	SetDescription(TEXT("Human"));
	TestTrue(TEXT("human save"), UEditorLoadingAndSavingUtils::SavePackages({Target.Get()->GetOutermost()}, false));
	TestTrue(TEXT("the human edit is in the undo history"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));

	// 2. The AI edits the task (the description is not in the document: omitted = default); history
	//    referencing the tree is cleared.
	if (!TestTrue(TEXT("AI edit applies"), Apply(*this, Target, Make(TEXT("Update"), TEXT("0.25"), TEXT("")), TEXT("Edit")).IsSuccess()))
	{
		return false;
	}
	TestTrue(TEXT("AI edit resets the omitted description"), Idle() != nullptr && Idle()->Description.IsEmpty());
	TestFalse(TEXT("after AI edit: no undo history references the tree"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const FString AfterEdit = Serialize(Extract(*this, Target, TEXT("after AI edit")));
	UndoRedoAll(TEXT("after AI edit"));
	ExtractEquals(*this, Target, AfterEdit, TEXT("after AI edit + undo/redo"));

	// 3. The human edits again and saves (an unsaved human edit makes ApplyFile refuse the tree before
	//    any change: DirtyExistingAssetTransactionUnsupported). An AI apply that fails before editing
	//    (a transition to a missing state) keeps the human's step, which undoes onto the unchanged tree.
	ReleaseUndoHistory();
	SetDescription(TEXT("Human2"));
	TestTrue(TEXT("human save again"), UEditorLoadingAndSavingUtils::SavePackages({Target.Get()->GetOutermost()}, false));
	const FString AfterHuman = Serialize(Extract(*this, Target, TEXT("after human edit")));
	const int32 HumanSteps = GEditor->Trans->GetQueueLength();
	const FAssetDocumentResult Refused = Apply(*this, Target, Document(Target, TEXT("Update"),
		TEXT("\"States\":[{\"Name\":\"Root\",\"Children\":[{\"Name\":\"Idle\",\"Transitions\":[{\"Trigger\":\"OnStateCompleted\",\"To\":\"Root/Nowhere\"}]},{\"Name\":\"Walk\"}]}]")),
		TEXT("MissingTarget"));
	TestFailsWith(*this, Refused, TEXT("StateTree.TransitionTargetNotFound"), TEXT("/Body/States/0/Children/0/Transitions/0/To"), TEXT("missing target"));
	TestEqual(TEXT("refused apply: the human history is kept"), GEditor->Trans->GetQueueLength(), HumanSteps);
	ExtractEquals(*this, Target, AfterHuman, TEXT("after refused apply"));
	TestTrue(TEXT("Ctrl+Z undoes the human edit"), GEditor->UndoTransaction());
	TestTrue(TEXT("after undo: the AI edit's description"), Idle() != nullptr && Idle()->Description.IsEmpty());
	ExtractEquals(*this, Target, AfterEdit, TEXT("after refused apply + undo"));
	TestTrue(TEXT("Ctrl+Y redoes the human edit"), GEditor->RedoTransaction());
	TestTrue(TEXT("after redo: the human description"), Idle() != nullptr && Idle()->Description == TEXT("Human2"));

	// 4. An AI apply whose edits went through but whose compile fails rolls back; its recorded steps
	//    would undo onto the restored tree, so the history that references the tree is cleared
	//    (AssetDocumentUndoHistory.h) and Ctrl+Z cannot corrupt it.
	AddExpectedMessage(TEXT("Failed to compile"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0, false);
	AddExpectedMessage(TEXT("is expected to have a binding"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	const FAssetDocumentResult Failed = Apply(*this, Target, Make(TEXT("Update"), TEXT("0.25"),
		TEXT("\"EnterConditions\":[{\"Type\":\"/Script/StateTreeModule.StateTreeCompareIntCondition\"}],")), TEXT("CompileError"));
	TestFailsWith(*this, Failed, TEXT("StateTree.CompileError"), TEXT(""), TEXT("compile error"));
	TestFalse(TEXT("after rollback: the package is clean"), Target.Get() == nullptr || Target.Get()->GetOutermost()->IsDirty());
	TestTrue(TEXT("after rollback: no enter condition"), Idle() != nullptr && Idle()->EnterConditions.Num() == 0);
	TestTrue(TEXT("after rollback: compiled"), Target.Get() != nullptr && Target.Get()->LastCompiledEditorDataHash != 0);
	TestFalse(TEXT("after rollback: no undo history references the tree"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	ExtractEquals(*this, Target, AfterHuman, TEXT("after compile error rollback"));
	UndoRedoAll(TEXT("after compile error rollback"));
	ExtractEquals(*this, Target, AfterHuman, TEXT("after compile error rollback + undo/redo"));
	return !HasAnyErrors();
}

namespace AssetDocumentStateTreeDiscoveryTests
{
TArray<FString> RowNames(const TSharedPtr<FJsonObject>& Payload, const TCHAR* ArrayKey, const TCHAR* FieldKey)
{
	TArray<FString> Names;
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (Payload.IsValid() && Payload->TryGetArrayField(ArrayKey, Rows))
	{
		for (const TSharedPtr<FJsonValue>& Row : *Rows)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FString Value;
			if (Row->TryGetObject(Object) && (*Object)->TryGetStringField(FieldKey, Value))
			{
				Names.Add(Value);
			}
		}
	}
	return Names;
}

FString FieldUsage(const TSharedPtr<FJsonObject>& Payload, const FString& Name)
{
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (Payload.IsValid() && Payload->TryGetArrayField(TEXT("InstanceFields"), Rows))
	{
		for (const TSharedPtr<FJsonValue>& Row : *Rows)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (Row->TryGetObject(Object) && (*Object)->GetStringField(TEXT("Name")) == Name)
			{
				return (*Object)->GetStringField(TEXT("Usage"));
			}
		}
	}
	return FString();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentStateTreeDiscoveryTest, "UEAssetDoc.StateTree.Discovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentStateTreeDiscoveryTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentStateTreeDiscoveryTests;
	const FAssetDocumentService Service;

	// inspect of a node struct: node fields + instance data fields, as the node Details panel.
	FAssetDocumentInspectRequest Inspect;
	Inspect.ClassOrAsset = TEXT("/Script/StateTreeModule.StateTreeCompareBoolCondition");
	const FAssetDocumentResult Bool = Service.Inspect(Inspect);
	TestTrue(TEXT("node struct inspects"), Bool.IsSuccess());
	const TArray<FString> NodeFields = RowNames(Bool.Payload, TEXT("Fields"), TEXT("Name"));
	TestTrue(TEXT("node field bInvert is listed"), NodeFields.Contains(TEXT("bInvert")));
	for (const TCHAR* Internal : {TEXT("Name"), TEXT("BindingsBatch"), TEXT("InstanceTemplateIndex"), TEXT("InstanceDataHandle")})
	{
		TestFalse(FString::Printf(TEXT("node base internal %s is not listed"), Internal), NodeFields.Contains(Internal));
	}
	TestEqual(TEXT("InstanceType names the instance data struct"),
		Bool.Payload.IsValid() ? Bool.Payload->GetStringField(TEXT("InstanceType")) : FString(),
		FString(TEXT("/Script/StateTreeModule.StateTreeCompareBoolConditionInstanceData")));
	TestEqual(TEXT("bLeft is an Input (must be bound)"), FieldUsage(Bool.Payload, TEXT("bLeft")), FString(TEXT("Input")));
	TestEqual(TEXT("bRight is a Parameter"), FieldUsage(Bool.Payload, TEXT("bRight")), FString(TEXT("Parameter")));

	// A task with outputs: outputs are listed apart (binding sources), not as authored instance fields.
	Inspect.ClassOrAsset = TEXT("/Script/StateTreeModule.StateTreeDelayTask");
	const FAssetDocumentResult Delay = Service.Inspect(Inspect);
	TestTrue(TEXT("Delay task Duration is an instance field"), RowNames(Delay.Payload, TEXT("InstanceFields"), TEXT("Name")).Contains(TEXT("Duration")));

	// A non-node struct keeps the plain reflected description.
	Inspect.ClassOrAsset = TEXT("/Script/CoreUObject.Vector");
	const FAssetDocumentResult Vector = Service.Inspect(Inspect);
	TestTrue(TEXT("plain struct has no InstanceFields"), Vector.IsSuccess() && !Vector.Payload->HasField(TEXT("InstanceFields")));

	// list_types with a node base: no Hidden bases, no test-suite types.
	const FAssetDocumentResult Tasks = Service.ListTypes({
		{TEXT("kind"), TEXT("struct")}, {TEXT("base"), TEXT("/Script/StateTreeModule.StateTreeTaskBase")}, {TEXT("limit"), TEXT("500")}});
	TestTrue(TEXT("task structs listed"), Tasks.IsSuccess());
	const TArray<FString> TaskPaths = RowNames(Tasks.Payload, TEXT("Types"), TEXT("Path"));
	TestTrue(TEXT("Delay task listed"), TaskPaths.Contains(TEXT("/Script/StateTreeModule.StateTreeDelayTask")));
	TestFalse(TEXT("Hidden base StateTreeTaskBase not listed"), TaskPaths.Contains(TEXT("/Script/StateTreeModule.StateTreeTaskBase")));
	TestFalse(TEXT("Hidden base StateTreeTaskCommonBase not listed"), TaskPaths.Contains(TEXT("/Script/StateTreeModule.StateTreeTaskCommonBase")));
	TestFalse(TEXT("no test-suite types"), TaskPaths.ContainsByPredicate([](const FString& Path) { return Path.StartsWith(TEXT("/Script/StateTreeTestSuite.")); }));
	if (FindObject<UScriptStruct>(nullptr, TEXT("/Script/StateTreeTestSuite.TestTask_Stand")) != nullptr)
	{
		AddInfo(TEXT("StateTreeTestSuite is loaded; its tasks were filtered out"));
	}

	// schema: what the picker of that schema offers (the component schema refuses the Blueprint wrapper structs).
	TestTrue(TEXT("without schema the Blueprint task wrapper struct is listed"),
		TaskPaths.Contains(TEXT("/Script/StateTreeModule.StateTreeBlueprintTaskWrapper")));
	const FAssetDocumentResult Allowed = Service.ListTypes({
		{TEXT("kind"), TEXT("struct")}, {TEXT("base"), TEXT("/Script/StateTreeModule.StateTreeTaskBase")},
		{TEXT("schema"), TEXT("/Script/GameplayStateTreeModule.StateTreeComponentSchema")}, {TEXT("limit"), TEXT("500")}});
	const TArray<FString> AllowedPaths = RowNames(Allowed.Payload, TEXT("Types"), TEXT("Path"));
	TestTrue(TEXT("schema filter keeps Delay task"), AllowedPaths.Contains(TEXT("/Script/StateTreeModule.StateTreeDelayTask")));
	TestFalse(TEXT("schema filter drops the wrapper"), AllowedPaths.Contains(TEXT("/Script/StateTreeModule.StateTreeBlueprintTaskWrapper")));
	const FAssetDocumentResult NotSchema = Service.ListTypes({
		{TEXT("kind"), TEXT("struct")}, {TEXT("schema"), TEXT("/Script/Engine.Actor")}});
	TestTrue(TEXT("a non-schema class is an invalid query"),
		!NotSchema.IsSuccess() && !NotSchema.Diagnostics.IsEmpty() && NotSchema.Diagnostics[0].Code == TEXT("Discovery.InvalidQuery"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentStateTreeSchemaContextTest, "UEAssetDoc.StateTree.SchemaContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentStateTreeSchemaContextTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentStateTreePythonTests;
	const FTarget Target(TEXT("ST_SchemaContext"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
	};
	auto Body = [](const TCHAR* ActorClass, const TCHAR* Member)
	{
		return FString::Printf(TEXT(R"json(
"Schema":{"Class":"/Script/GameplayStateTreeModule.StateTreeComponentSchema","Properties":{"ContextActorClass":"%s"}},
"States":[{"Name":"Root","Tasks":[{"Type":"/Script/StateTreeModule.StateTreeDelayTask","Name":"Hold"}],
  "EnterConditions":[{"Type":"/Script/StateTreeModule.StateTreeCompareFloatCondition","Name":"Tall","Instance":{"Right":1.0},
    "Bindings":{"Left":"Context/Actor:%s"}}]}])json"), ActorClass, Member);
	};
	auto ContextActorStruct = [&Target]() -> const UStruct*
	{
		const UStateTreeEditorData* EditorData = EditorDataOf(Target.Get());
		if (EditorData == nullptr || EditorData->Schema == nullptr)
		{
			return nullptr;
		}
		for (const FStateTreeExternalDataDesc& Desc : EditorData->Schema->GetContextDataDescs())
		{
			if (Desc.Name == FName(TEXT("Actor")))
			{
				return Desc.Struct;
			}
		}
		return nullptr;
	};
	const FAssetDocumentResult Created = Apply(*this, Target, Document(Target, TEXT("Create"), Body(TEXT("/Script/Engine.Pawn"), TEXT("BaseEyeHeight"))),
		TEXT("create with a Pawn context"));
	if (!TestTrue(TEXT("a binding to a member of the schema's ContextActorClass applies"), Created.IsSuccess()))
	{
		return false;
	}
	TestTrue(TEXT("the Actor context is the Pawn"), ContextActorStruct() == APawn::StaticClass());
	TestNoOpReapply(*this, Target, TEXT("pawn context"));
	const FAssetDocumentResult Changed = Apply(*this, Target, Document(Target, TEXT("Update"),
		Body(TEXT("/Script/Engine.Character"), TEXT("CrouchedEyeHeight"))), TEXT("update to a Character context"));
	if (!TestTrue(TEXT("changing the class and binding a member of the new class applies"), Changed.IsSuccess()))
	{
		return false;
	}
	TestTrue(TEXT("the Actor context is the Character"), ContextActorStruct() == ACharacter::StaticClass());
	TestNoOpReapply(*this, Target, TEXT("character context"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentStateTreeOmittedFieldsTest, "UEAssetDoc.StateTree.OmittedFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentStateTreeOmittedFieldsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentStateTreePythonTests;
	const FTarget Target(TEXT("ST_OmittedFields"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
	};
	auto Body = [](const TCHAR* StateFields, const TCHAR* TaskFields)
	{
		return FString::Printf(TEXT(R"json(
"Schema":{"Class":"/Script/GameplayStateTreeModule.StateTreeComponentSchema"},
"States":[{"Name":"Root"%s,"Tasks":[{"Type":"/Script/StateTreeModule.StateTreeDelayTask","Name":"Hold"%s}]}])json"), StateFields, TaskFields);
	};
	const FString Set = Document(Target, TEXT("Create"), Body(TEXT(R"json(,"Description":"Waits","Weight":2,"TickRate":0.5)json"),
		TEXT(R"json(,"Enabled":false,"Instance":{"Duration":3.0,"RandomDeviation":0.5})json")));
	if (!TestTrue(TEXT("non-default fields apply"), Apply(*this, Target, Set, TEXT("set")).IsSuccess()))
	{
		return false;
	}
	TestNoDiff(*this, Set, TEXT("set"));
	// The same state and task with the fields left out are a new state's / task's (before: kept, Diff empty).
	const FString Omitted = Document(Target, TEXT("Update"), Body(TEXT(""), TEXT(R"json(,"Instance":{"Duration":3.0})json")));
	FAssetDocumentDiffRequest Request;
	Request.Document = Parse(Omitted);
	const FAssetDocumentResult Diff = FAssetDocumentModule::Get().GetService().Diff(Request);
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	TestTrue(TEXT("omitting non-default fields is a Diff"), Diff.IsSuccess() && Diff.Payload.IsValid()
		&& Diff.Payload->TryGetArrayField(TEXT("changed"), Changed) && !Changed->IsEmpty());
	const FAssetDocumentResult Reset = Apply(*this, Target, Omitted, TEXT("omitted"));
	if (!TestTrue(TEXT("omitting the fields applies"), Reset.IsSuccess()))
	{
		return false;
	}
	TestTrue(TEXT("omitting the fields is a change"), Reset.bSavedAsset);
	const UStateTreeState* Root = FindState(Target.Get(), TEXT("Root"));
	TestTrue(TEXT("the description and weight are a new state's"), Root != nullptr && Root->Description.IsEmpty()
		&& FMath::IsNearlyEqual(Root->Weight, GetDefault<UStateTreeState>()->Weight));
	TestTrue(TEXT("the custom tick rate is off again"), Root != nullptr && !Root->bHasCustomTickRate);
	const FStateTreeEditorNode* Hold = FindTask(Root, TEXT("Hold"));
	const FStateTreeTaskBase* Task = Hold != nullptr ? Hold->Node.GetPtr<FStateTreeTaskBase>() : nullptr;
	TestTrue(TEXT("the task is enabled again"), Task != nullptr && Task->bTaskEnabled);
	const TSharedPtr<FJsonObject> Extracted = Extract(*this, Target, TEXT("omitted"));
	const FString Text = Extracted.IsValid() ? Serialize(Extracted) : FString();
	TestFalse(TEXT("the random deviation is the default again"), Text.Contains(TEXT("RandomDeviation")));
	TestTrue(TEXT("the listed duration is kept"), Text.Contains(TEXT("Duration")));
	TestNoDiff(*this, Omitted, TEXT("omitted"));
	TestNoOpReapply(*this, Target, TEXT("omitted"));
	return !HasAnyErrors();
}

#endif
