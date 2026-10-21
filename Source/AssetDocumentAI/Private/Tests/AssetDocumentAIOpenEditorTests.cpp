// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Open-editor apply of the AI profiles (Runtime/AssetDocumentOpenEditor.h, AssetDocumentAIOpenEditor.cpp):
// UEAssetDoc.StateTree.OpenEditorApply: with the tree open in the StateTree editor, an apply (states
//   replaced) and a value-only apply land in the editor's view model: its last OnAssetChanged (what the tree view,
//   Details and compiler results rebuild from) saw the applied tree, and a selected state the apply removed is no
//   longer selected. A failed apply (staging save vetoed) rolls back and the views see the rolled-back tree; the
//   editor's own undo / redo after the apply stay whole; the editor closes.
// UEAssetDoc.BehaviorTree.OpenEditorApply: an apply installs a new BTGraph, so the Behavior Tree editor is
//   closed for the apply and reopened (BehaviorTree.EditorReopened); the reopened editor shows the applied graph, and
//   the editor's own rebuild of the tree from its graph (UBehaviorTreeGraph::UpdateAsset, run after every edit in the
//   editor) keeps the apply. A failed apply rolls back and reopens the editor on the unchanged tree; the editor's own
//   undo / redo stay whole; the editor closes.

#include "AssetDocumentOpenEditorTestSupport.h"

#include "AIGraphNode.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "BehaviorTreeEditor.h"
#include "BehaviorTreeGraph.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Runtime/AssetDocumentOpenEditor.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StateTree.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeViewModel.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentAIOpenEditorApplyTests
{
const TCHAR* const FolderName = TEXT("AssetDocumentAIOpenEditor");

void ReleaseUndoHistory()
{
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
		&& GEditor->Trans->GetQueueLength() > 0)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentAIOpenEditorTests", "Cleanup", "AssetDocument AI open-editor test cleanup"));
	}
}

UAssetEditorSubsystem* Editors()
{
	return GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
}

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
	FTarget(const FTarget&) = delete;
	FTarget& operator=(const FTarget&) = delete;

	UObject* Get() const { return FSoftObjectPath(ObjectPath).ResolveObject(); }

	// Editors closed; saved assets unloaded and unsaved ones deleted, as the Content Browser does.
	~FTarget()
	{
		ReleaseUndoHistory();
		if (UObject* Asset = Get())
		{
			if (UAssetEditorSubsystem* Subsystem = Editors())
			{
				Subsystem->CloseAllEditorsForAsset(Asset);
			}
			Asset->GetOutermost()->ClearDirtyFlag();
			if (IFileManager::Get().FileExists(*PackageFile))
			{
				UPackageTools::UnloadPackages({Asset->GetOutermost()});
			}
			else
			{
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

FString ExtractText(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Stage)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Target.ObjectPath;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Extract(Request);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract succeeds"), Stage), Result.IsSuccess() && Result.Payload.IsValid()))
	{
		LogResult(Test, Stage, Result);
		return FString();
	}
	return Serialize(Result.Payload);
}

bool HasCode(const FAssetDocumentResult& Result, const TCHAR* Code)
{
	return Result.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}

bool SaveAsset(FAutomationTestBase& Test, UObject* Asset, const TCHAR* Stage)
{
	return Test.TestTrue(*FString::Printf(TEXT("%s saved"), Stage),
		Asset != nullptr && UEditorLoadingAndSavingUtils::SavePackages({Asset->GetOutermost()}, false));
}

// A failure after every apply step: the apply's save to its staging file is vetoed (once), and the apply rolls back.
class FStagingSaveVeto
{
public:
	explicit FStagingSaveVeto(FString InPackageName)
		: Previous(FCoreUObjectDelegates::IsPackageOKToSaveDelegate), PackageName(MoveTemp(InPackageName))
	{
		FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindLambda([this](UPackage* Saved, const FString& Filename, FOutputDevice* Error)
		{
			if (Previous.IsBound() && !Previous.Execute(Saved, Filename, Error))
			{
				return false;
			}
			if (!bTriggered && Saved != nullptr && Saved->GetName() == PackageName
				&& FPaths::ConvertRelativePathToFull(Filename).Contains(TEXT("/Saved/AssetDocument/Runtime/Staging/")))
			{
				bTriggered = true;
				return false;
			}
			return true;
		});
	}
	~FStagingSaveVeto() { FCoreUObjectDelegates::IsPackageOKToSaveDelegate = Previous; }
	FStagingSaveVeto(const FStagingSaveVeto&) = delete;
	FStagingSaveVeto& operator=(const FStagingSaveVeto&) = delete;

	bool bTriggered = false;

private:
	FCoreUObjectDelegates::FIsPackageOKToSaveDelegate Previous;
	FString PackageName;
};

// ---- StateTree -------------------------------------------------------------------------------------------------------

FString StateTreeDocument(const FTarget& Target, const TCHAR* Action, const TCHAR* Second, double Deviation = 0.5)
{
	return FString::Printf(TEXT(R"json({"SchemaVersion":1,"Target":"%s","Class":"/Script/StateTreeModule.StateTree","Action":"%s","Properties":{},"Body":{
		"Schema":{"Class":"/Script/GameplayStateTreeModule.StateTreeComponentSchema"},
		"States":[{"Name":"Root","Children":[
		 {"Name":"Idle","Tasks":[{"Type":"/Script/StateTreeModule.StateTreeDelayTask","Name":"Wait","Instance":{"RandomDeviation":%g}}],
		  "Transitions":[{"Trigger":"OnStateCompleted","To":"Root/%s"}]},
		 {"Name":"%s"}]}]}})json"), *Target.PackageName, Action, Deviation, Second, Second);
}

void CollectStates(const TArray<TObjectPtr<UStateTreeState>>& Level, TSet<const UStateTreeState*>& Out)
{
	for (const UStateTreeState* State : Level)
	{
		if (State != nullptr)
		{
			Out.Add(State);
			CollectStates(State->Children, Out);
		}
	}
}

// The names of every state of the tree, sorted ("" when there is no editor data).
FString StateNamesOf(const UStateTree* Tree)
{
	TSet<const UStateTreeState*> States;
	if (const UStateTreeEditorData* EditorData = Tree != nullptr ? Cast<UStateTreeEditorData>(Tree->EditorData) : nullptr)
	{
		CollectStates(EditorData->SubTrees, States);
	}
	TArray<FString> Names;
	for (const UStateTreeState* State : States)
	{
		Names.Add(State->Name.ToString());
	}
	Names.Sort();
	return FString::Join(Names, TEXT(","));
}

UStateTreeState* FindChildState(const UStateTree* Tree, const TCHAR* Name);

// What the editor's views show after a refresh: every state name, and Idle's Delay task RandomDeviation (reflected:
// the task's instance data struct is private to the StateTree module).
FString TreeFingerprintOf(const UStateTree* Tree)
{
	FString Deviation = TEXT("-");
	const UStateTreeState* Idle = FindChildState(Tree, TEXT("Idle"));
	if (Idle != nullptr && Idle->Tasks.Num() > 0)
	{
		const FInstancedStruct& Instance = Idle->Tasks[0].Instance;
		const FFloatProperty* Property = Instance.GetScriptStruct() != nullptr
			? CastField<FFloatProperty>(Instance.GetScriptStruct()->FindPropertyByName(TEXT("RandomDeviation"))) : nullptr;
		if (Property != nullptr)
		{
			Deviation = FString::SanitizeFloat(Property->GetPropertyValue_InContainer(Instance.GetMemory()));
		}
	}
	return StateNamesOf(Tree) + TEXT("|") + Deviation;
}

UStateTreeState* FindChildState(const UStateTree* Tree, const TCHAR* Name)
{
	const UStateTreeEditorData* EditorData = Tree != nullptr ? Cast<UStateTreeEditorData>(Tree->EditorData) : nullptr;
	if (EditorData == nullptr || EditorData->SubTrees.IsEmpty() || EditorData->SubTrees[0] == nullptr)
	{
		return nullptr;
	}
	for (UStateTreeState* Child : EditorData->SubTrees[0]->Children)
	{
		if (Child != nullptr && Child->Name.ToString() == Name)
		{
			return Child;
		}
	}
	return nullptr;
}

TSharedPtr<FStateTreeViewModel> ViewModelOf(const UStateTree* Tree)
{
	UStateTreeEditingSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UStateTreeEditingSubsystem>() : nullptr;
	return Subsystem != nullptr && Tree != nullptr ? Subsystem->FindViewModel(Tree) : nullptr;
}

// Every state the view model has selected is a state of the tree.
bool SelectionIsLive(const FStateTreeViewModel& ViewModel, const UStateTree& Tree, int32& OutSelected)
{
	TSet<const UStateTreeState*> Live;
	if (const UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(Tree.EditorData))
	{
		CollectStates(EditorData->SubTrees, Live);
	}
	TArray<TWeakObjectPtr<UStateTreeState>> Selected;
	ViewModel.GetSelectedStates(Selected);
	OutSelected = Selected.Num();
	for (const TWeakObjectPtr<UStateTreeState>& State : Selected)
	{
		if (!State.IsValid() || !Live.Contains(State.Get()))
		{
			return false;
		}
	}
	return true;
}

// ---- BehaviorTree ----------------------------------------------------------------------------------------------------

FString BlackboardDocument(const FTarget& Target)
{
	return FString::Printf(TEXT(R"({"SchemaVersion": 1, "Target": "%s", "Class": "/Script/AIModule.BlackboardData",
		"Action": "CreateOrUpdate", "Definitions": {}, "Properties": {}, "Body": {"Parent": null, "Keys": [
		{"Name": "Target", "Category": "", "Description": "", "bInstanceSynced": false,
		 "KeyTypeClass": {"Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Object"},
		 "KeyTypeProperties": {"BaseClass": {"Kind": "ClassRef", "Path": "/Script/Engine.Actor"}, "DefaultValue": null}}]}})"), *Target.PackageName);
}

// A Sequence with one Wait task, or two with bSecond.
FString BehaviorTreeDocument(const FTarget& Target, const FTarget& Blackboard, bool bSecond)
{
	const TCHAR* Second = bSecond
		? TEXT(R"(, {"Id": "00000000000000000000000000000004", "Class": "/Script/AIModule.BTTask_Wait", "Editor": {"Position": {"X": 600, "Y": 200}}})")
		: TEXT("");
	return FString::Printf(TEXT(R"({"SchemaVersion": 1, "Target": "%s", "Class": "/Script/AIModule.BehaviorTree",
		"Action": "CreateOrUpdate", "Definitions": {}, "Properties": {}, "Body": {
		"BlackboardAsset": {"Kind": "AssetRef", "Path": "%s"},
		"Tree": {"Root": {"Id": "00000000000000000000000000000001", "Class": "/Script/AIModule.BTComposite_Sequence", "Children": [
			{"Id": "00000000000000000000000000000003", "Class": "/Script/AIModule.BTTask_Wait", "Editor": {"Position": {"X": 300, "Y": 200}}}%s]}}}})"),
		*Target.PackageName, *Blackboard.ObjectPath, Second);
}

int32 RootChildren(const UBehaviorTree* Tree)
{
	return Tree != nullptr && Tree->RootNode != nullptr ? Tree->RootNode->GetChildrenNum() : INDEX_NONE;
}

// The Behavior Tree editor open on Tree (FBehaviorTreeEditor, toolkit "Behavior Tree"), or null.
FBehaviorTreeEditor* BehaviorTreeEditorOf(UBehaviorTree* Tree)
{
	UAssetEditorSubsystem* Subsystem = Editors();
	IAssetEditorInstance* Instance = Subsystem != nullptr && Tree != nullptr ? Subsystem->FindEditorForAsset(Tree, false) : nullptr;
	if (Instance == nullptr || Instance->GetEditorName() != FName(TEXT("Behavior Tree")))
	{
		return nullptr;
	}
	return static_cast<FBehaviorTreeEditor*>(static_cast<FAssetEditorToolkit*>(Instance));
}

// The graph the Behavior Tree editor's focused graph tab shows.
UEdGraph* FocusedGraphOf(UBehaviorTree* Tree)
{
	const FBehaviorTreeEditor* Editor = BehaviorTreeEditorOf(Tree);
	const TSharedPtr<SGraphEditor> GraphEditor = Editor != nullptr ? Editor->GetFocusedGraphPtr().Pin() : nullptr;
	return GraphEditor.IsValid() ? GraphEditor->GetCurrentGraph() : nullptr;
}
} // namespace AssetDocumentAIOpenEditorApplyTests

// Test bodies name the helpers through this alias: other test files of the module pull their own helper namespaces
// into the global scope, which unity builds would make ambiguous.
namespace AIOpenEd = AssetDocumentAIOpenEditorApplyTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentStateTreeOpenEditorApplyTest, "UEAssetDoc.StateTree.OpenEditorApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentStateTreeOpenEditorApplyTest::RunTest(const FString& Parameters)
{
	UAssetEditorSubsystem* Subsystem = AIOpenEd::Editors();
	if (!TestNotNull(TEXT("asset editor subsystem"), Subsystem) || GEditor->Trans == nullptr)
	{
		return false;
	}
	const AIOpenEd::FTarget Target(TEXT("ST_Open"));
	if (!TestTrue(TEXT("create applies"), AIOpenEd::Apply(*this, Target, AIOpenEd::StateTreeDocument(Target, TEXT("Create"), TEXT("Walk")), TEXT("create")).IsSuccess()))
	{
		return false;
	}
	UStateTree* Tree = Cast<UStateTree>(Target.Get());
	if (!TestNotNull(TEXT("tree"), Tree) || !Subsystem->OpenEditorForAsset(Tree))
	{
		AddWarning(TEXT("The StateTree editor did not open in this session; the open-editor apply was not exercised."));
		return true;
	}
	ON_SCOPE_EXIT
	{
		if (UObject* Open = Target.Get())
		{
			AIOpenEd::Editors()->CloseAllEditorsForAsset(Open);
		}
	};
	UE::AssetDocument::OpenEditorTests::TickEditorUi();
	// The human does not save after opening: the StateTree editor changes nothing while opening.
	TestFalse(TEXT("opening the editor leaves the package clean"), Tree->GetOutermost()->IsDirty());
	AIOpenEd::ReleaseUndoHistory();
	TSharedPtr<FStateTreeViewModel> ViewModel = AIOpenEd::ViewModelOf(Tree);
	if (!TestTrue(TEXT("the open editor has its view model"), ViewModel.IsValid()))
	{
		return false;
	}
	// The views (tree view, Details, compiler results) rebuild from the asset on OnAssetChanged: what they show is the
	// tree as it was at the last broadcast.
	int32 AssetChanged = 0;
	FString ShownTree;
	const FDelegateHandle Handle = ViewModel->GetOnAssetChanged().AddLambda([&AssetChanged, &ShownTree, &Target]()
	{
		++AssetChanged;
		ShownTree = AIOpenEd::TreeFingerprintOf(Cast<UStateTree>(Target.Get()));
	});
	const TWeakPtr<FStateTreeViewModel> WeakViewModel = ViewModel;
	ON_SCOPE_EXIT
	{
		if (const TSharedPtr<FStateTreeViewModel> Pinned = WeakViewModel.Pin())
		{
			Pinned->GetOnAssetChanged().Remove(Handle);
		}
	};

	// The human selects Walk in the open editor; an apply replaces Walk with Run.
	ViewModel->SetSelection(AIOpenEd::FindChildState(Tree, TEXT("Walk")));
	int32 Selected = 0;
	TestTrue(TEXT("the human's selection"), AIOpenEd::SelectionIsLive(*ViewModel, *Tree, Selected) && Selected == 1);
	if (!TestTrue(TEXT("apply while open"), AIOpenEd::Apply(*this, Target, AIOpenEd::StateTreeDocument(Target, TEXT("Update"), TEXT("Run")), TEXT("apply while open")).IsSuccess()))
	{
		return false;
	}
	Tree = Cast<UStateTree>(Target.Get());
	if (!TestNotNull(TEXT("tree after the apply"), Tree))
	{
		return false;
	}
	TestNotNull(TEXT("the tree has Run"), AIOpenEd::FindChildState(Tree, TEXT("Run")));
	TestNull(TEXT("the tree has no Walk"), AIOpenEd::FindChildState(Tree, TEXT("Walk")));
	TestFalse(TEXT("the apply is not an undo step"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	TestTrue(TEXT("the editor is still open"), Subsystem->FindEditorForAsset(Tree, false) != nullptr);
	TestTrue(TEXT("the open editor's view model is the same"), AIOpenEd::ViewModelOf(Tree) == ViewModel);
	TestTrue(TEXT("the open editor's views were told the asset changed (as its Undo does)"), AssetChanged > 0);
	TestEqual(TEXT("the open editor's views were last refreshed on the applied tree"), ShownTree, AIOpenEd::TreeFingerprintOf(Tree));
	TestTrue(TEXT("the open editor no longer selects the removed state"), AIOpenEd::SelectionIsLive(*ViewModel, *Tree, Selected));
	AddInfo(FString::Printf(TEXT("after the apply: %d asset-changed broadcast(s), %d selected state(s)"), AssetChanged, Selected));
	UE::AssetDocument::OpenEditorTests::TickEditorUi();
	FString Applied = AIOpenEd::ExtractText(*this, Target, TEXT("after apply while open"));

	// A failure after every step (the staging save is vetoed) rolls back with the editor open.
	{
		ViewModel->SetSelection(AIOpenEd::FindChildState(Tree, TEXT("Run")));
		AddExpectedMessage(TEXT("is not allowed to save"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
		const int32 ChangedBefore = AssetChanged;
		FAssetDocumentResult Failed;
		{
			AIOpenEd::FStagingSaveVeto Veto(Target.PackageName);
			// A value edit only (no state added, removed or renamed, which the editor broadcasts on its own).
			Failed = AIOpenEd::Apply(*this, Target, AIOpenEd::StateTreeDocument(Target, TEXT("Update"), TEXT("Run"), 0.9), TEXT("failing apply while open"));
			TestTrue(TEXT("the staging save was vetoed"), Veto.bTriggered);
		}
		TestFalse(TEXT("failing apply while open fails"), Failed.IsSuccess());
		Tree = Cast<UStateTree>(Target.Get());
		if (!TestNotNull(TEXT("tree after the rollback"), Tree))
		{
			return false;
		}
		TestEqual(TEXT("after the rollback the tree is as applied"), AIOpenEd::ExtractText(*this, Target, TEXT("after the rollback")), Applied);
		TestTrue(TEXT("after the rollback the open editor's views were refreshed"), AssetChanged > ChangedBefore);
		TestEqual(TEXT("after the rollback the open editor's views were last refreshed on the rolled-back tree"), ShownTree, AIOpenEd::TreeFingerprintOf(Tree));
		TestTrue(TEXT("after the rollback the open editor selects only states of the tree"), AIOpenEd::SelectionIsLive(*ViewModel, *Tree, Selected));
		TestFalse(TEXT("after the rollback the package is clean"), Tree->GetOutermost()->IsDirty());
		UE::AssetDocument::OpenEditorTests::TickEditorUi();
	}

	// A value edit alone (no state added, removed or renamed) applies with the editor open.
	{
		const int32 ChangedBefore = AssetChanged;
		if (!TestTrue(TEXT("value apply while open"), AIOpenEd::Apply(*this, Target, AIOpenEd::StateTreeDocument(Target, TEXT("Update"), TEXT("Run"), 0.25), TEXT("value apply while open")).IsSuccess()))
		{
			return false;
		}
		Tree = Cast<UStateTree>(Target.Get());
		if (!TestNotNull(TEXT("tree after the value apply"), Tree))
		{
			return false;
		}
		TestTrue(TEXT("the value apply landed"), AIOpenEd::TreeFingerprintOf(Tree).EndsWith(TEXT("|0.25")));
		TestTrue(TEXT("after the value apply the open editor's views were refreshed"), AssetChanged > ChangedBefore);
		TestEqual(TEXT("after the value apply the open editor's views were last refreshed on the applied tree"), ShownTree, AIOpenEd::TreeFingerprintOf(Tree));
		UE::AssetDocument::OpenEditorTests::TickEditorUi();
		Applied = AIOpenEd::ExtractText(*this, Target, TEXT("after the value apply"));
	}

	// The editor's own edit and undo / redo after the apply.
	{
		UStateTreeState* Idle = AIOpenEd::FindChildState(Tree, TEXT("Idle"));
		if (TestNotNull(TEXT("Idle"), Idle))
		{
			const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentAIOpenEditorTests", "UserEdit", "Edit State"));
			Idle->Modify();
			Idle->Description = TEXT("Human");
			FPropertyChangedEvent Changed(UStateTreeState::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UStateTreeState, Description)));
			Idle->PostEditChangeProperty(Changed);
		}
		const FString Human = AIOpenEd::ExtractText(*this, Target, TEXT("after the human edit"));
		TestNotEqual(TEXT("the human edit landed"), Human, Applied);
		const int32 ChangedBefore = AssetChanged;
		TestTrue(TEXT("the human edit undoes"), GEditor->Trans->CanUndo() && GEditor->UndoTransaction());
		TestEqual(TEXT("undo restores the applied tree"), AIOpenEd::ExtractText(*this, Target, TEXT("after undo")), Applied);
		TestTrue(TEXT("the human edit redoes"), GEditor->Trans->CanRedo() && GEditor->RedoTransaction());
		TestEqual(TEXT("redo restores the human edit"), AIOpenEd::ExtractText(*this, Target, TEXT("after redo")), Human);
		TestTrue(TEXT("the editor's undo / redo refreshed its views"), AssetChanged > ChangedBefore);
		UE::AssetDocument::OpenEditorTests::TickEditorUi();
	}
	Tree = Cast<UStateTree>(Target.Get());
	if (Tree != nullptr)
	{
		TestTrue(TEXT("the tree is still open in its editor"), Subsystem->FindEditorForAsset(Tree, false) != nullptr);
		Subsystem->CloseAllEditorsForAsset(Tree);
		TestNull(TEXT("the editor closed"), Subsystem->FindEditorForAsset(Tree, false));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBehaviorTreeOpenEditorApplyTest, "UEAssetDoc.BehaviorTree.OpenEditorApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeOpenEditorApplyTest::RunTest(const FString& Parameters)
{
	UAssetEditorSubsystem* Subsystem = AIOpenEd::Editors();
	if (!TestNotNull(TEXT("asset editor subsystem"), Subsystem) || GEditor->Trans == nullptr)
	{
		return false;
	}
	const AIOpenEd::FTarget Blackboard(TEXT("BB_Open"));
	const AIOpenEd::FTarget Target(TEXT("BT_Open"));
	if (!TestTrue(TEXT("blackboard applies"), AIOpenEd::Apply(*this, Blackboard, AIOpenEd::BlackboardDocument(Blackboard), TEXT("blackboard")).IsSuccess())
		|| !TestTrue(TEXT("tree applies"), AIOpenEd::Apply(*this, Target, AIOpenEd::BehaviorTreeDocument(Target, Blackboard, false), TEXT("tree")).IsSuccess()))
	{
		return false;
	}
	UBehaviorTree* Tree = Cast<UBehaviorTree>(Target.Get());
	if (!TestNotNull(TEXT("tree"), Tree) || !Subsystem->OpenEditorForAsset(Tree))
	{
		AddWarning(TEXT("The Behavior Tree editor did not open in this session; the open-editor apply was not exercised."));
		return true;
	}
	ON_SCOPE_EXIT
	{
		if (UObject* Open = Target.Get())
		{
			AIOpenEd::Editors()->CloseAllEditorsForAsset(Open);
		}
	};
	UE::AssetDocument::OpenEditorTests::TickEditorUi();
	// The graph an apply writes has the current graph version, as the editor's own new graph has: opening changes nothing.
	TestFalse(TEXT("opening the editor on an applied tree leaves the package clean"), Tree->GetOutermost()->IsDirty());

	// A tree saved with an older graph version (authored before the editor's last graph upgrade): opening upgrades the
	// graph (UBehaviorTreeGraph::UpdateVersion's Modify), which dirties the package although the human changed nothing.
	// The human does not save: the apply accepts these open-time changes (it replaces the graph).
	{
		Subsystem->CloseAllEditorsForAsset(Tree);
		UBehaviorTreeGraph* Older = Cast<UBehaviorTreeGraph>(Tree->BTGraph);
		if (!TestNotNull(TEXT("the tree's graph"), Older))
		{
			return false;
		}
		Older->MarkVersion();
		Older->GraphVersion -= 1;
		AIOpenEd::SaveAsset(*this, Tree, TEXT("an older graph version"));
		TestFalse(TEXT("the older graph is saved"), Tree->GetOutermost()->IsDirty());
		if (!TestTrue(TEXT("the editor opens again"), Subsystem->OpenEditorForAsset(Tree)))
		{
			return false;
		}
		UE::AssetDocument::OpenEditorTests::TickEditorUi();
		TestTrue(TEXT("opening upgrades the older graph and dirties the package"), Tree->GetOutermost()->IsDirty());
		TestTrue(TEXT("the package is dirty only from the editor's open"), UE::AssetDocument::OpenEditor::HasOnlyOpenTimeChanges(*Tree->GetOutermost()));
	}
	AIOpenEd::ReleaseUndoHistory();
	if (!TestNotNull(TEXT("the Behavior Tree editor"), AIOpenEd::BehaviorTreeEditorOf(Tree))
		|| !TestTrue(TEXT("the open editor shows the tree's graph"), AIOpenEd::FocusedGraphOf(Tree) != nullptr && AIOpenEd::FocusedGraphOf(Tree) == Tree->BTGraph))
	{
		return false;
	}
	const UEdGraph* GraphBefore = Tree->BTGraph;

	// An apply adds a second Wait: the editor is closed for the apply and reopened on the applied tree.
	const FAssetDocumentResult Result = AIOpenEd::Apply(*this, Target, AIOpenEd::BehaviorTreeDocument(Target, Blackboard, true), TEXT("apply while open"));
	if (!TestTrue(TEXT("apply while open"), Result.IsSuccess()))
	{
		return false;
	}
	TestTrue(TEXT("the result reports the reopened editor"), AIOpenEd::HasCode(Result, TEXT("BehaviorTree.EditorReopened")));
	Tree = Cast<UBehaviorTree>(Target.Get());
	if (!TestNotNull(TEXT("tree after the apply"), Tree))
	{
		return false;
	}
	TestEqual(TEXT("the tree runs two tasks"), AIOpenEd::RootChildren(Tree), 2);
	TestFalse(TEXT("the apply saved the editor's open-time changes with its own"), Tree->GetOutermost()->IsDirty());
	TestFalse(TEXT("the apply is not an undo step"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	AddInfo(FString::Printf(TEXT("the apply %s the tree's graph object"), Tree->BTGraph != GraphBefore ? TEXT("replaced") : TEXT("kept")));
	TestNotNull(TEXT("the Behavior Tree editor is open again"), AIOpenEd::BehaviorTreeEditorOf(Tree));
	TestTrue(TEXT("the reopened editor shows the applied graph"), AIOpenEd::FocusedGraphOf(Tree) != nullptr && AIOpenEd::FocusedGraphOf(Tree) == Tree->BTGraph);
	UE::AssetDocument::OpenEditorTests::TickEditorUi();
	const FString Applied = AIOpenEd::ExtractText(*this, Target, TEXT("after apply while open"));

	// The editor rebuilds the tree from the graph it shows after each of its own edits: that keeps the apply.
	if (UBehaviorTreeGraph* Shown = Cast<UBehaviorTreeGraph>(AIOpenEd::FocusedGraphOf(Tree)))
	{
		Shown->UpdateAsset();
		TestEqual(TEXT("the editor's rebuild from its graph keeps the two tasks"), AIOpenEd::RootChildren(Tree), 2);
		TestEqual(TEXT("the editor's rebuild from its graph keeps the applied tree"), AIOpenEd::ExtractText(*this, Target, TEXT("after the editor's rebuild")), Applied);
		if (Tree->GetOutermost()->IsDirty())
		{
			AIOpenEd::SaveAsset(*this, Tree, TEXT("after the editor's rebuild"));
		}
	}

	// A failure after every step (the staging save is vetoed) rolls back and reopens the editor on the unchanged tree.
	{
		AddExpectedMessage(TEXT("is not allowed to save"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
		FAssetDocumentResult Failed;
		{
			AIOpenEd::FStagingSaveVeto Veto(Target.PackageName);
			Failed = AIOpenEd::Apply(*this, Target, AIOpenEd::BehaviorTreeDocument(Target, Blackboard, false), TEXT("failing apply while open"));
			TestTrue(TEXT("the staging save was vetoed"), Veto.bTriggered);
		}
		TestFalse(TEXT("failing apply while open fails"), Failed.IsSuccess());
		TestTrue(TEXT("the failed result reports the reopened editor"), AIOpenEd::HasCode(Failed, TEXT("BehaviorTree.EditorReopened")));
		Tree = Cast<UBehaviorTree>(Target.Get());
		if (!TestNotNull(TEXT("tree after the rollback"), Tree))
		{
			return false;
		}
		TestEqual(TEXT("after the rollback the tree runs two tasks"), AIOpenEd::RootChildren(Tree), 2);
		TestEqual(TEXT("after the rollback the tree is as applied"), AIOpenEd::ExtractText(*this, Target, TEXT("after the rollback")), Applied);
		TestTrue(TEXT("after the rollback the reopened editor shows the tree's graph"), AIOpenEd::FocusedGraphOf(Tree) != nullptr && AIOpenEd::FocusedGraphOf(Tree) == Tree->BTGraph);
		TestFalse(TEXT("after the rollback the package is clean"), Tree->GetOutermost()->IsDirty());
		UE::AssetDocument::OpenEditorTests::TickEditorUi();
	}

	// The editor's own edit and undo / redo after the apply.
	{
		UAIGraphNode* Node = nullptr;
		if (Tree->BTGraph != nullptr)
		{
			for (UEdGraphNode* Candidate : Tree->BTGraph->Nodes)
			{
				UAIGraphNode* AINode = Cast<UAIGraphNode>(Candidate);
				Node = AINode != nullptr && AINode->NodeInstance != nullptr && AINode->NodeInstance->IsA<UBTTask_Wait>() ? AINode : Node;
			}
		}
		if (TestNotNull(TEXT("a Wait task node to move"), Node))
		{
			const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentAITests", "MoveNode", "Move Node"));
			Node->Modify();
			Node->NodePosX += 64;
		}
		const FString Human = AIOpenEd::ExtractText(*this, Target, TEXT("after the human edit"));
		TestNotEqual(TEXT("the human edit landed"), Human, Applied);
		TestTrue(TEXT("the human edit undoes"), GEditor->Trans->CanUndo() && GEditor->UndoTransaction());
		TestEqual(TEXT("undo restores the applied tree"), AIOpenEd::ExtractText(*this, Target, TEXT("after undo")), Applied);
		TestTrue(TEXT("the human edit redoes"), GEditor->Trans->CanRedo() && GEditor->RedoTransaction());
		TestEqual(TEXT("redo restores the human edit"), AIOpenEd::ExtractText(*this, Target, TEXT("after redo")), Human);
		UE::AssetDocument::OpenEditorTests::TickEditorUi();
		// The human edit is a change of the human's own: an apply now refuses the unsaved package, with the editor left open.
		TestTrue(TEXT("the human edit left the package unsaved"), Tree->GetOutermost()->IsDirty()
			&& !UE::AssetDocument::OpenEditor::HasOnlyOpenTimeChanges(*Tree->GetOutermost()));
		const FAssetDocumentResult Unsaved = AIOpenEd::Apply(*this, Target, AIOpenEd::BehaviorTreeDocument(Target, Blackboard, true), TEXT("apply over the unsaved edit"));
		TestTrue(TEXT("an apply refuses the unsaved human edit"), !Unsaved.IsSuccess() && AIOpenEd::HasCode(Unsaved, TEXT("DirtyExistingAssetTransactionUnsupported")));
		TestFalse(TEXT("the refused apply did not reopen the editor"), AIOpenEd::HasCode(Unsaved, TEXT("BehaviorTree.EditorReopened")));
		TestTrue(TEXT("after the refusal the package is still unsaved"), Tree->GetOutermost()->IsDirty());
		TestEqual(TEXT("after the refusal the tree keeps the human edit"), AIOpenEd::ExtractText(*this, Target, TEXT("after the refusal")), Human);
	}
	Tree = Cast<UBehaviorTree>(Target.Get());
	if (Tree != nullptr)
	{
		TestNotNull(TEXT("the tree is still open in its editor"), AIOpenEd::BehaviorTreeEditorOf(Tree));
		Subsystem->CloseAllEditorsForAsset(Tree);
		TestNull(TEXT("the editor closed"), Subsystem->FindEditorForAsset(Tree, false));
	}
	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
