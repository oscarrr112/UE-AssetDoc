// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Open-editor apply of the Niagara assets (Runtime/AssetDocumentOpenEditor.h, AssetDocumentNiagaraOpenEditor.cpp).
//   UEAssetDoc.Niagara.OpenEditorApply.System      a system with a scratch pad, open in the system
//        editor: an apply (an emitter disabled) closes the editor and reopens it (Niagara.EditorReopened); the reopened
//        editor's view model shows the applied system and its scratch pad copy is fresh. Unapplied scratch pad edits
//        refuse an apply before any change (Niagara.OpenInEditorUnsavedEdits) and stay in the editor; its Apply Scratch
//        writes them. A failed apply (staging save vetoed) rolls back and reopens the editor; the editor's undo / redo
//        after the apply stay whole.
//   UEAssetDoc.Niagara.OpenEditorApply.EmitterAsset an emitter asset open in the emitter editor (a
//        working copy in a placeholder system): an apply reopens the editor on a copy of the applied emitter; a human
//        edit of the copy is pending (its Apply is enabled) and refuses an apply; the editor's Apply writes it and keeps
//        the applied value; a failed apply rolls back and reopens the editor; the editor's undo of a copy edit.
//   UEAssetDoc.Niagara.OpenEditorApply.Script       a module open in the script editor (a working
//        copy): an apply reopens the editor on a copy of the applied script; a human graph edit in the copy is pending
//        and refuses an apply; the editor's Apply writes it and keeps the applied settings; a failed apply rolls back.
//   UEAssetDoc.Niagara.OpenEditorApply.Collection   a parameter collection open in its editor (it edits
//        the collection itself): an apply (a parameter added) is made in place and the editor's parameter list shows
//        it; a failed apply rolls back and the list shows the rolled-back collection; the editor's undo / redo of its
//        own edit keep the list in step.
//   UEAssetDoc.Niagara.OpenEditorApply.Dependents   a parent emitter applied while a child emitter
//        asset is open in the emitter editor and a system using the child is open in the system editor: both editors
//        follow the parent (Niagara.DependentsUpdated), as after the parent editor's own Apply.
//   asset_editor's ways out of pending edits (AssetDocumentOpenEditor.h pending edits): the refusals name them; apply_editor
//   presses the editor's Apply / Apply Scratch; close with discard_unapplied closes the system and emitter editors without
//   them (the script editor has no such close).
// Every test closes its editors without a prompt.

#include "AssetDocumentNiagaraTestEditing.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/UICommandInfo.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/Docking/TabManager.h"
#include "GraphEditor.h"
#include "NiagaraGraph.h"
#include "NiagaraParameterCollection.h"
#include "Runtime/AssetDocumentOpenEditor.h"
#include "TickableEditorObject.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "UObject/ObjectKey.h"
#include "ViewModels/NiagaraScratchPadScriptViewModel.h"
#include "ViewModels/NiagaraScratchPadViewModel.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Views/STableViewBase.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentNiagaraOpenEditorTests
{
using namespace AssetDocumentNiagaraTests;

const FName SystemEditorName(TEXT("Niagara"));
const FName ScriptEditorName(TEXT("NiagaraScriptEditor"));
const FName NiagaraCommands(TEXT("NiagaraEditor"));
const FName ApplyCommand(TEXT("Apply"));
const FName ApplyScratchCommand(TEXT("ApplyScratchPadChanges"));

UAssetEditorSubsystem* Editors()
{
	return GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
}

void SettleSystem(UNiagaraSystem& System)
{
	const double Start = FPlatformTime::Seconds();
	while (System.HasOutstandingCompilationRequests(false) && FPlatformTime::Seconds() - Start < 600.0)
	{
		System.WaitForCompilationComplete(false, false);
	}
}

// Ticks the editor UI (Slate and the editor tickables) and finishes the compiles of every loaded system.
void TickEditorUi()
{
	for (int32 Tick = 0; Tick < 3; ++Tick)
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().Tick();
		}
		FTickableEditorObject::TickObjects(0.f);
	}
	for (TObjectIterator<UNiagaraSystem> It; It; ++It)
	{
		if (IsValid(*It) && It->HasOutstandingCompilationRequests(false))
		{
			SettleSystem(**It);
		}
	}
}

FAssetEditorToolkit* ToolkitOf(UObject* Asset, FName EditorName)
{
	UAssetEditorSubsystem* Subsystem = Editors();
	if (Subsystem == nullptr || Asset == nullptr)
	{
		return nullptr;
	}
	for (IAssetEditorInstance* Instance : Subsystem->FindEditorsForAsset(Asset))
	{
		if (Instance != nullptr && Instance->GetEditorName() == EditorName)
		{
			return static_cast<FAssetEditorToolkit*>(Instance);
		}
	}
	return nullptr;
}

// The editor's toolbar command (mapped in its toolkit commands) can run now.
bool CanExecute(UObject* Asset, FName EditorName, FName Name)
{
	FAssetEditorToolkit* Toolkit = ToolkitOf(Asset, EditorName);
	const TSharedPtr<FUICommandInfo> Command = FInputBindingManager::Get().FindCommandInContext(NiagaraCommands, Name);
	return Toolkit != nullptr && Command.IsValid() && Toolkit->GetToolkitCommands()->IsActionMapped(Command)
		&& Toolkit->GetToolkitCommands()->CanExecuteAction(Command.ToSharedRef());
}

bool Execute(UObject* Asset, FName EditorName, FName Name)
{
	FAssetEditorToolkit* Toolkit = ToolkitOf(Asset, EditorName);
	const TSharedPtr<FUICommandInfo> Command = FInputBindingManager::Get().FindCommandInContext(NiagaraCommands, Name);
	return Toolkit != nullptr && Command.IsValid() && Toolkit->GetToolkitCommands()->ExecuteAction(Command.ToSharedRef());
}

// The editor's pending edits are applied (no prompt on close), the package is left clean and the editors close.
// The refusal of pending edits names asset_editor's ways out: apply_editor, and discard_unapplied for an editor
// that closes without them (else the apply_editor / close / reload_assets sequence).
bool NamesWaysOut(const FAssetDocumentResult& Result, bool bCanDiscard)
{
	return Result.Diagnostics.ContainsByPredicate([bCanDiscard](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == TEXT("Niagara.OpenInEditorUnsavedEdits") && Diagnostic.Message.Contains(TEXT("apply_editor"))
			&& Diagnostic.Message.Contains(bCanDiscard ? TEXT("discard_unapplied") : TEXT("reload_assets"))
			// It names the asset, not the editor's transient working copy.
			&& !Diagnostic.Message.Contains(TEXT("/Engine/Transient"));
	});
}

bool ApplyEditor(UObject* Asset)
{
	FString Reason;
	return Asset != nullptr && UE::AssetDocument::OpenEditor::ApplyPendingEdits(*Asset, Reason);
}

void CloseQuietly(const FTarget& Target)
{
	UObject* Asset = Target.Get();
	if (Asset == nullptr || Editors() == nullptr)
	{
		return;
	}
	for (const FName Editor : {SystemEditorName, ScriptEditorName})
	{
		for (const FName Command : {ApplyCommand, ApplyScratchCommand})
		{
			if (CanExecute(Asset, Editor, Command))
			{
				Execute(Asset, Editor, Command);
			}
		}
	}
	TickEditorUi();
	if (UObject* Live = Target.Get())
	{
		Live->GetOutermost()->ClearDirtyFlag();
		Editors()->CloseAllEditorsForAsset(Live);
	}
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

FAssetDocumentResult FailingApply(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const FString& Stage)
{
	Test.AddExpectedMessage(TEXT("is not allowed to save"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	FStagingSaveVeto Veto(Target.PackageName);
	const FAssetDocumentResult Result = ApplyDocument(Test, Target, Document, Stage);
	Test.TestTrue(*FString::Printf(TEXT("%s: the staging save was vetoed"), *Stage), Veto.bTriggered);
	return Result;
}

bool OpenEditor(FAutomationTestBase& Test, UObject* Asset, const TCHAR* What)
{
	if (Asset == nullptr || Editors() == nullptr || !Editors()->OpenEditorForAsset(Asset))
	{
		Test.AddWarning(FString::Printf(TEXT("The %s did not open in this session; the open-editor apply was not exercised."), What));
		return false;
	}
	TickEditorUi();
	return true;
}

void TestOpenedClean(FAutomationTestBase& Test, const UObject* Asset)
{
	const bool bDirty = Asset != nullptr && Asset->GetOutermost()->IsDirty();
	if (bDirty)
	{
		Test.AddInfo(FString::Printf(TEXT("the editor dirtied %s while opening (only open-time changes: %d)"), *Asset->GetName(),
			UE::AssetDocument::OpenEditor::HasOnlyOpenTimeChanges(*Asset->GetOutermost()) ? 1 : 0));
	}
	Test.TestFalse(TEXT("opening the editor leaves the package clean"), bDirty);
}

// UEdGraph::NotifyGraphChanged(Action) is protected; the editor's graph edits reach it through
// UNiagaraGraph::NotifyGraphNeedsRecompile (not exported). The virtual call runs UNiagaraGraph's override.
struct FGraphChangeAccess : UEdGraph
{
	static void Notify(UEdGraph& Graph, const FEdGraphEditAction& Action)
	{
		void (UEdGraph::*NotifyChanged)(const FEdGraphEditAction&) = &FGraphChangeAccess::NotifyGraphChanged;
		(Graph.*NotifyChanged)(Action);
	}
};

// A graph edit as the editor's graph panel reports one (UNiagaraGraph::NotifyGraphNeedsRecompile): the first node moved.
bool EditGraphLikeTheEditor(UNiagaraGraph* Graph, int32 DeltaX)
{
	if (Graph == nullptr || Graph->Nodes.IsEmpty())
	{
		return false;
	}
	TArray<UEdGraphNode*> Nodes = Graph->Nodes;
	Nodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.GetName() < B.GetName(); });
	Nodes[0]->Modify();
	Nodes[0]->NodePosX += DeltaX;
	FEdGraphEditAction Action;
	Action.Action = static_cast<EEdGraphActionType>(GRAPHACTION_GenericNeedsRecompile);
	FGraphChangeAccess::Notify(*Graph, Action);
	return true;
}

int32 FirstNodeX(UNiagaraGraph* Graph)
{
	if (Graph == nullptr || Graph->Nodes.IsEmpty())
	{
		return INT32_MIN;
	}
	TArray<UEdGraphNode*> Nodes = Graph->Nodes;
	Nodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.GetName() < B.GetName(); });
	return Nodes[0]->NodePosX;
}

bool IsLocalSpace(const UNiagaraEmitter* Emitter)
{
	const FVersionedNiagaraEmitterData* Data = Emitter != nullptr ? Emitter->GetLatestEmitterData() : nullptr;
	return Data != nullptr && Data->bLocalSpace;
}

bool Determinism(const UNiagaraEmitter* Emitter)
{
	const FVersionedNiagaraEmitterData* Data = Emitter != nullptr ? Emitter->GetLatestEmitterData() : nullptr;
	return Data != nullptr && Data->bDeterminism;
}

// The emitter Details edit (its Emitter Properties): bDeterminism.
void SetDeterminism(UNiagaraEmitter& Emitter, bool bValue, bool bTransaction)
{
	FVersionedNiagaraEmitterData* Data = Emitter.GetLatestEmitterData();
	if (Data == nullptr)
	{
		return;
	}
	TOptional<FScopedTransaction> Transaction;
	if (bTransaction)
	{
		Transaction.Emplace(NSLOCTEXT("AssetDocumentNiagaraOpenEditorTests", "Details", "Details"));
	}
	Emitter.Modify();
	Data->bDeterminism = bValue;
	FProperty* Property = FindFProperty<FProperty>(FVersionedNiagaraEmitterData::StaticStruct(), TEXT("bDeterminism"));
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	Emitter.PostEditChangeVersionedProperty(Event, Data->Version.VersionGuid);
}

TSharedPtr<FJsonObject> EmitterProperties(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonObject> Body = Child(Document, TEXT("Body"));
	if (!Body->HasTypedField<EJson::Object>(TEXT("Properties")))
	{
		Body->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
	}
	return Child(Body, TEXT("Properties"));
}

TSharedPtr<FNiagaraSystemViewModel> SystemEditorViewModel(UObject* System)
{
	UNiagaraSystem* AsSystem = Cast<UNiagaraSystem>(System);
	return AsSystem != nullptr ? FNiagaraEditorModule::Get().GetExistingViewModelForSystem(AsSystem) : nullptr;
}

// The working copies of the open emitter editors (the emitter of each one's placeholder system), optionally only the
// copies inheriting from Parent.
TArray<UNiagaraEmitter*> EmitterEditorCopies(const UNiagaraEmitter* Parent = nullptr)
{
	TArray<UNiagaraEmitter*> Copies;
	for (TObjectIterator<UNiagaraSystem> It; It; ++It)
	{
		UNiagaraSystem* Placeholder = *It;
		if (!IsValid(Placeholder) || Placeholder->GetOutermost() != GetTransientPackage())
		{
			continue;
		}
		const TSharedPtr<FNiagaraSystemViewModel> ViewModel = FNiagaraEditorModule::Get().GetExistingViewModelForSystem(Placeholder);
		if (!ViewModel.IsValid() || ViewModel->GetEditMode() != ENiagaraSystemViewModelEditMode::EmitterAsset
			|| ViewModel->GetEmitterHandleViewModels().Num() != 1)
		{
			continue;
		}
		UNiagaraEmitter* Copy = ViewModel->GetEmitterHandleViewModels()[0]->GetEmitterViewModel()->GetEmitter().Emitter;
		const FVersionedNiagaraEmitterData* Data = Copy != nullptr ? Copy->GetLatestEmitterData() : nullptr;
		if (Data != nullptr && (Parent == nullptr || Data->GetParent().Emitter == Parent))
		{
			Copies.Add(Copy);
		}
	}
	return Copies;
}

UNiagaraEmitter* OnlyEmitterEditorCopy(FAutomationTestBase& Test, const UNiagaraEmitter* Parent = nullptr)
{
	const TArray<UNiagaraEmitter*> Copies = EmitterEditorCopies(Parent);
	Test.TestEqual(TEXT("one open emitter editor"), Copies.Num(), 1);
	return Copies.Num() == 1 ? Copies[0] : nullptr;
}

TSet<FObjectKey> TransientScripts()
{
	TSet<FObjectKey> Keys;
	for (TObjectIterator<UNiagaraScript> It; It; ++It)
	{
		if (It->GetOuter() == GetTransientPackage())
		{
			Keys.Add(FObjectKey(*It));
		}
	}
	return Keys;
}

// The script editor's working copy of Script: a transient duplicate (same versions) opened after Before was taken,
// whose graph is shown in a graph editor (or the only such duplicate).
UNiagaraScript* ScriptEditorCopy(FAutomationTestBase& Test, const UNiagaraScript& Script, const TSet<FObjectKey>& Before)
{
	const FGuid Version = Script.GetExposedVersion().VersionGuid;
	TArray<UNiagaraScript*> Candidates;
	for (TObjectIterator<UNiagaraScript> It; It; ++It)
	{
		if (It->GetOuter() == GetTransientPackage() && IsValid(*It) && !Before.Contains(FObjectKey(*It)) && It->GetUsage() == Script.GetUsage()
			&& It->GetExposedVersion().VersionGuid == Version)
		{
			Candidates.Add(*It);
		}
	}
	for (UNiagaraScript* Candidate : Candidates)
	{
		UNiagaraGraph* Graph = UE::AssetDocument::Niagara::ScriptEditGraph(*Candidate);
		if (Graph != nullptr && SGraphEditor::FindGraphEditorForGraph(Graph).IsValid())
		{
			return Candidate;
		}
	}
	Test.AddInfo(FString::Printf(TEXT("script editor copies without a drawn graph editor: %d"), Candidates.Num()));
	return Candidates.Num() == 1 ? Candidates[0] : nullptr;
}

FString ScriptDescription(UNiagaraScript* Script)
{
	const FVersionedNiagaraScriptData* Data = Script != nullptr ? Script->GetScriptData(Script->GetExposedVersion().VersionGuid) : nullptr;
	return Data != nullptr ? Data->Description.ToString() : FString(TEXT("<none>"));
}

// STableViewBase::GetNumItemsBeingObserved (the size of the list's items source) is protected: read it as a derived
// list would. Never instantiated.
struct FObservedItems : STableViewBase
{
	static int32 Of(const STableViewBase& View)
	{
		int32 (STableViewBase::*Count)() const = &FObservedItems::GetNumItemsBeingObserved;
		return (View.*Count)();
	}
};

// The number of rows of the collection editor's parameter list (its SListView over the view model's parameters).
int32 ListedRows(const TSharedRef<SWidget>& Widget)
{
	if (Widget->GetType().ToString().StartsWith(TEXT("SListView")))
	{
		return FObservedItems::Of(static_cast<const STableViewBase&>(Widget.Get()));
	}
	FChildren* Children = Widget->GetChildren();
	for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
	{
		const int32 Rows = ListedRows(Children->GetChildAt(Index));
		if (Rows >= 0)
		{
			return Rows;
		}
	}
	return -1;
}

int32 CollectionEditorRows(UObject* Collection)
{
	FAssetEditorToolkit* Toolkit = ToolkitOf(Collection, SystemEditorName);
	const TSharedPtr<FTabManager> Tabs = Toolkit != nullptr ? Toolkit->GetTabManager() : nullptr;
	const TSharedPtr<SDockTab> Main = Tabs.IsValid() ? Tabs->FindExistingLiveTab(FTabId(TEXT("NiagaraParameterCollectionEditor_Main"))) : nullptr;
	return Main.IsValid() ? ListedRows(Main->GetContent()) : -1;
}

int32 CollectionParameters(UObject* Collection)
{
	const UNiagaraParameterCollection* AsCollection = Cast<UNiagaraParameterCollection>(Collection);
	return AsCollection != nullptr ? AsCollection->GetParameters().Num() : -1;
}
} // namespace AssetDocumentNiagaraOpenEditorTests

// ---- System -----------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraOpenEditorApplySystemTest, "UEAssetDoc.Niagara.OpenEditorApply.System",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraOpenEditorApplySystemTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraOpenEditorTests;
	if (Editors() == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	const FTarget Target(TEXT("NS_OpenEd"));
	ON_SCOPE_EXIT
	{
		CloseQuietly(Target);
		Target.Cleanup();
	};
	const TSharedPtr<FJsonObject> Create = Parse(FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s",
		"Class": "/Script/Niagara.NiagaraSystem", "Action": "Create", "Definitions": {}, "Properties": {},
		"Body": {"ScratchPads": [{"Name": "Swirl", "Usage": "Module"}],
		         "Emitters": [{"Name": "Sparks", "From": "Minimal", "ParticleUpdate": [{"Name": "Swirl", "Script": "ScratchPad:Swirl"}]}]}})json"),
		*Target.PackageName));
	if (!TestTrue(TEXT("the system is created"), ApplyDocument(*this, Target, Create, TEXT("create")).IsSuccess()))
	{
		return false;
	}
	ReleaseUndoHistory();
	const TSharedPtr<FJsonObject> Created = Extract(*this, Target, TEXT("created"));
	if (!Created.IsValid() || !OpenEditor(*this, Target.Get(), TEXT("Niagara system editor")))
	{
		return !HasAnyErrors();
	}
	TestOpenedClean(*this, Target.Get());
	{
		const TSharedPtr<FNiagaraSystemViewModel> ViewModel = SystemEditorViewModel(Target.Get());
		if (!TestTrue(TEXT("the system editor's view model"), ViewModel.IsValid()))
		{
			return false;
		}
		TestTrue(TEXT("the editor shows Sparks enabled"), HandleNamed(*ViewModel, TEXT("Sparks")).IsValid() && HandleNamed(*ViewModel, TEXT("Sparks"))->GetIsEnabled());
		TestEqual(TEXT("the editor holds the scratch pad"), ViewModel->GetScriptScratchPadViewModel()->GetScriptViewModels().Num(), 1);
	}

	// 1. An apply disables Sparks: the editor is closed for it and reopened on the applied system.
	const TSharedPtr<FJsonObject> Disabled = Retargeted(Created, Target, TEXT("Update"));
	Named(List(Child(Disabled, TEXT("Body")), TEXT("Emitters")), TEXT("Sparks"))->SetBoolField(TEXT("Enabled"), false);
	const FAssetDocumentResult Applied = ApplyDocument(*this, Target, Disabled, TEXT("apply while open"));
	if (!TestTrue(TEXT("apply while open"), Applied.IsSuccess()))
	{
		return false;
	}
	TestTrue(TEXT("the result reports the reopened editor"), HasDiagnostic(Applied, TEXT("Niagara.EditorReopened")));
	{
		// Trial 7: the reopened editor requests a compile (compile for edit); the result reported Dirty while
		// the next inspect said UpToDate. The result waits for that compile and reports its outcome.
		const FString Status = Applied.Compile.IsValid() ? Applied.Compile->GetStringField(TEXT("Status")) : FString();
		AddInfo(FString::Printf(TEXT("compile status after the apply: %s"), *Status));
		TestTrue(TEXT("the result's compile status is settled (not Dirty)"), Status.StartsWith(TEXT("UpToDate")) || Status == TEXT("Error"));
	}
	UNiagaraSystem* System = Cast<UNiagaraSystem>(Target.Get());
	if (!TestTrue(TEXT("the system has one emitter"), System != nullptr && System->GetEmitterHandles().Num() == 1))
	{
		return false;
	}
	TestFalse(TEXT("the asset's Sparks is disabled"), System->GetEmitterHandles()[0].GetIsEnabled());
	TestFalse(TEXT("the apply saved the package"), System->GetOutermost()->IsDirty());
	TestFalse(TEXT("the apply is not an undo step"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	TickEditorUi();
	TSharedPtr<FNiagaraSystemViewModel> ViewModel = SystemEditorViewModel(System);
	if (!TestTrue(TEXT("the system editor is open again"), ViewModel.IsValid()))
	{
		return false;
	}
	TSharedPtr<FNiagaraEmitterHandleViewModel> Sparks = HandleNamed(*ViewModel, TEXT("Sparks"));
	TestTrue(TEXT("the reopened editor shows Sparks disabled"), Sparks.IsValid() && !Sparks->GetIsEnabled());
	TestTrue(TEXT("the reopened editor's handle is the system's"), Sparks.IsValid() && Sparks->GetEmitterHandle() == &System->GetEmitterHandles()[0]);
	UNiagaraScratchPadViewModel* Pads = ViewModel->GetScriptScratchPadViewModel();
	TSharedPtr<FNiagaraScratchPadScriptViewModel> Pad = Pads->GetScriptViewModels().Num() == 1 ? Pads->GetScriptViewModels()[0].ToSharedPtr() : nullptr;
	TestTrue(TEXT("the reopened editor's scratch pad copy is fresh"), Pad.IsValid() && !Pad->HasUnappliedChanges()
		&& System->ScratchPadScripts.Num() == 1 && Pad->GetOriginalScript() == System->ScratchPadScripts[0]);
	TestFalse(TEXT("nothing to apply in the reopened editor"), CanExecute(System, SystemEditorName, ApplyScratchCommand));
	const FString AppliedContent = Content(Extract(*this, Target, TEXT("after apply while open")));

	// 2. Unapplied scratch pad edits refuse an apply before any change and stay in the editor; Apply Scratch writes them.
	if (Pad.IsValid() && TestTrue(TEXT("a human edit of the scratch pad"), EditGraphLikeTheEditor(UE::AssetDocument::Niagara::ScriptEditGraph(*Pad->GetEditScript().Script), 64)))
	{
		TestTrue(TEXT("the scratch pad edit is pending"), Pad->HasUnappliedChanges() && CanExecute(System, SystemEditorName, ApplyScratchCommand));
		const FAssetDocumentResult Pending = ApplyDocument(*this, Target, Retargeted(Created, Target, TEXT("Update")), TEXT("apply over the pending edit"));
		TestTrue(TEXT("an apply refuses the pending scratch pad edit"), !Pending.IsSuccess() && HasDiagnostic(Pending, TEXT("Niagara.OpenInEditorUnsavedEdits")));
		TestFalse(TEXT("the refused apply did not reopen the editor"), HasDiagnostic(Pending, TEXT("Niagara.EditorReopened")));
		TestTrue(TEXT("the editor stays open with the pending edit"), SystemEditorViewModel(Target.Get()) == ViewModel && Pad->HasUnappliedChanges());
		TestTextEqual(*this, TEXT("the refusal changes nothing"), Content(Extract(*this, Target, TEXT("after the refusal"))), AppliedContent);
		TestTrue(TEXT("the refusal names the ways out"), NamesWaysOut(Pending, true));
		TestEqual(TEXT("the pending edits' button"), UE::AssetDocument::OpenEditor::GetPendingEditsButton(*System), FString(TEXT("Apply Scratch")));
		TestTrue(TEXT("the editor's Apply Scratch (asset_editor apply_editor)"), ApplyEditor(System));
		TickEditorUi();
		TestFalse(TEXT("nothing is pending after Apply Scratch"), CanExecute(System, SystemEditorName, ApplyScratchCommand));
		TestFalse(TEXT("Apply Scratch keeps Sparks disabled"), System->GetEmitterHandles()[0].GetIsEnabled());
		if (System->GetOutermost()->IsDirty())
		{
			TestTrue(TEXT("the human save"), SaveAsset(*System, Target.PackageFile));
		}
	}
	ReleaseUndoHistory();
	const FString HumanContent = Content(Extract(*this, Target, TEXT("after the human edit")));

	// 3. A failed apply rolls back and reopens the editor on the unchanged system.
	{
		const TSharedPtr<FJsonObject> Enabled = Retargeted(Extract(*this, Target, TEXT("before the failing apply")), Target, TEXT("Update"));
		Named(List(Child(Enabled, TEXT("Body")), TEXT("Emitters")), TEXT("Sparks"))->SetBoolField(TEXT("Enabled"), true);
		const FAssetDocumentResult Failed = FailingApply(*this, Target, Enabled, TEXT("failing apply while open"));
		TestFalse(TEXT("failing apply while open fails"), Failed.IsSuccess());
		TestTrue(TEXT("the failed result reports the reopened editor"), HasDiagnostic(Failed, TEXT("Niagara.EditorReopened")));
		TestTextEqual(*this, TEXT("the rollback restores the system"), Content(Extract(*this, Target, TEXT("after the rollback"))), HumanContent);
		System = Cast<UNiagaraSystem>(Target.Get());
		TestFalse(TEXT("after the rollback the package is clean"), System == nullptr || System->GetOutermost()->IsDirty());
		TickEditorUi();
		ViewModel = SystemEditorViewModel(System);
		Sparks = ViewModel.IsValid() ? HandleNamed(*ViewModel, TEXT("Sparks")) : nullptr;
		TestTrue(TEXT("after the rollback the reopened editor shows Sparks disabled"), Sparks.IsValid() && !Sparks->GetIsEnabled());
	}

	// 4. The editor's own undo / redo after the apply (its emitter's enabled check box).
	if (Sparks.IsValid() && System != nullptr)
	{
		ReleaseUndoHistory();
		TestTrue(TEXT("the human enables Sparks in the editor"), Sparks->SetIsEnabled(true));
		TestTrue(TEXT("Sparks is enabled"), System->GetEmitterHandles()[0].GetIsEnabled());
		TestTrue(TEXT("the editor's undo"), GEditor->Trans->CanUndo() && GEditor->UndoTransaction());
		TickEditorUi();
		TestTrue(TEXT("after the editor's undo Sparks is disabled again"), !System->GetEmitterHandles()[0].GetIsEnabled() && !Sparks->GetIsEnabled());
		TestTextEqual(*this, TEXT("after the editor's undo the system is as applied"), Content(Extract(*this, Target, TEXT("after undo"))), HumanContent);
		TestTrue(TEXT("the editor's redo"), GEditor->Trans->CanRedo() && GEditor->RedoTransaction());
		TickEditorUi();
		TestTrue(TEXT("after the editor's redo Sparks is enabled"), System->GetEmitterHandles()[0].GetIsEnabled() && Sparks->GetIsEnabled());
		ReleaseUndoHistory();
	}
	// 5. asset_editor close with discard_unapplied: the editor closes without its pending scratch pad edit.
	System = Cast<UNiagaraSystem>(Target.Get());
	ViewModel = System != nullptr ? SystemEditorViewModel(System) : nullptr;
	Pads = ViewModel.IsValid() ? ViewModel->GetScriptScratchPadViewModel() : nullptr;
	Pad = Pads != nullptr && Pads->GetScriptViewModels().Num() == 1 ? Pads->GetScriptViewModels()[0].ToSharedPtr() : nullptr;
	if (Pad.IsValid())
	{
		if (System->GetOutermost()->IsDirty())
		{
			TestTrue(TEXT("saved before the discarded edit"), SaveAsset(*System, Target.PackageFile));
		}
		const FString Kept = Content(Extract(*this, Target, TEXT("before the discarded edit")));
		if (TestTrue(TEXT("a scratch pad edit to discard"), EditGraphLikeTheEditor(UE::AssetDocument::Niagara::ScriptEditGraph(*Pad->GetEditScript().Script), 32)))
		{
			FString Reason;
			TestTrue(TEXT("the editor closes without the pending edit"), UE::AssetDocument::OpenEditor::CloseDiscardingPendingEdits(*System, Reason));
			TestFalse(TEXT("no system editor is left open"), SystemEditorViewModel(Target.Get()).IsValid());
			TestTextEqual(*this, TEXT("the system keeps what it had"), Content(Extract(*this, Target, TEXT("after the discard"))), Kept);
			TestFalse(TEXT("the package stays saved"), Target.Get()->GetOutermost()->IsDirty());
		}
	}
	CloseQuietly(Target);
	TestFalse(TEXT("the editor closed"), SystemEditorViewModel(Target.Get()).IsValid());
	return !HasAnyErrors();
}

// ---- Emitter asset ----------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraOpenEditorApplyEmitterAssetTest, "UEAssetDoc.Niagara.OpenEditorApply.EmitterAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraOpenEditorApplyEmitterAssetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraOpenEditorTests;
	UObject* Source = LoadObject<UObject>(nullptr, FountainEmitter);
	if (!TestNotNull(TEXT("Fountain loads"), Source) || Editors() == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	const FTarget Target(TEXT("NE_OpenEd"));
	ON_SCOPE_EXIT
	{
		CloseQuietly(Target);
		Target.Cleanup();
	};
	if (!TestNotNull(TEXT("copy"), DuplicateTo(*Source, Target)))
	{
		return false;
	}
	ReleaseUndoHistory();
	const bool bWasLocal = IsLocalSpace(Cast<UNiagaraEmitter>(Target.Get()));
	const bool bHumanDeterminism = !Determinism(Cast<UNiagaraEmitter>(Target.Get()));
	const TSharedPtr<FJsonObject> Baseline = Extract(*this, Target, TEXT("baseline"));
	if (!Baseline.IsValid() || !OpenEditor(*this, Target.Get(), TEXT("Niagara emitter editor")))
	{
		return !HasAnyErrors();
	}
	TestOpenedClean(*this, Target.Get());
	UNiagaraEmitter* Copy = OnlyEmitterEditorCopy(*this);
	if (!TestNotNull(TEXT("the emitter editor's working copy"), Copy))
	{
		return false;
	}
	TestTrue(TEXT("the copy is not the asset"), Copy != Target.Get());
	TestEqual(TEXT("the copy has the asset's local space"), IsLocalSpace(Copy), bWasLocal);
	TestFalse(TEXT("a fresh editor has nothing to apply"), CanExecute(Target.Get(), SystemEditorName, ApplyCommand));

	// 1. An apply changes Local Space: the editor is closed for it and reopened on a copy of the applied emitter.
	const TSharedPtr<FJsonObject> Changed = Retargeted(Baseline, Target, TEXT("Update"));
	EmitterProperties(Changed)->SetBoolField(TEXT("bLocalSpace"), !bWasLocal);
	const FAssetDocumentResult Applied = ApplyDocument(*this, Target, Changed, TEXT("apply while open"));
	if (!TestTrue(TEXT("apply while open"), Applied.IsSuccess()))
	{
		return false;
	}
	TestTrue(TEXT("the result reports the reopened editor"), HasDiagnostic(Applied, TEXT("Niagara.EditorReopened")));
	UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Target.Get());
	TestEqual(TEXT("the asset holds the new value"), IsLocalSpace(Emitter), !bWasLocal);
	TestFalse(TEXT("the apply saved the package"), Emitter == nullptr || Emitter->GetOutermost()->IsDirty());
	TestFalse(TEXT("the apply is not an undo step"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	TickEditorUi();
	Copy = OnlyEmitterEditorCopy(*this);
	TestEqual(TEXT("the reopened editor's copy holds the new value"), IsLocalSpace(Copy), !bWasLocal);
	TestFalse(TEXT("the reopened editor has nothing to apply"), CanExecute(Emitter, SystemEditorName, ApplyCommand));
	const FString AppliedContent = Content(Extract(*this, Target, TEXT("after apply while open")));

	// 2. A human Details edit of the copy is pending (Apply is enabled): an apply refuses it before any change and the
	//    editor keeps it; the editor's Apply writes it and keeps the applied value.
	if (Copy != nullptr && Emitter != nullptr)
	{
		SetDeterminism(*Copy, bHumanDeterminism, false);
		TestTrue(TEXT("the human edit is pending"), CanExecute(Emitter, SystemEditorName, ApplyCommand));
		const FAssetDocumentResult Pending = ApplyDocument(*this, Target, Retargeted(Baseline, Target, TEXT("Update")), TEXT("apply over the pending edit"));
		TestTrue(TEXT("an apply refuses the pending edit"), !Pending.IsSuccess() && HasDiagnostic(Pending, TEXT("Niagara.OpenInEditorUnsavedEdits")));
		TestFalse(TEXT("the refused apply did not reopen the editor"), HasDiagnostic(Pending, TEXT("Niagara.EditorReopened")));
		TestTrue(TEXT("the editor keeps the pending edit"), OnlyEmitterEditorCopy(*this) == Copy && Determinism(Copy) == bHumanDeterminism
			&& CanExecute(Emitter, SystemEditorName, ApplyCommand));
		TestTextEqual(*this, TEXT("the refusal changes nothing"), Content(Extract(*this, Target, TEXT("after the refusal"))), AppliedContent);
		TestTrue(TEXT("the refusal names the ways out"), NamesWaysOut(Pending, true));
		TestTrue(TEXT("the editor's Apply (asset_editor apply_editor)"), ApplyEditor(Emitter));
		TickEditorUi();
		Emitter = Cast<UNiagaraEmitter>(Target.Get());
		TestEqual(TEXT("the editor's Apply writes the human edit"), Determinism(Emitter), bHumanDeterminism);
		TestEqual(TEXT("the editor's Apply keeps the applied value"), IsLocalSpace(Emitter), !bWasLocal);
		TestFalse(TEXT("nothing is pending after the editor's Apply"), CanExecute(Emitter, SystemEditorName, ApplyCommand));
		if (Emitter != nullptr && Emitter->GetOutermost()->IsDirty())
		{
			TestTrue(TEXT("the human save"), SaveAsset(*Emitter, Target.PackageFile));
		}
	}
	ReleaseUndoHistory();
	const FString HumanContent = Content(Extract(*this, Target, TEXT("after the human edit")));

	// 3. A failed apply rolls back and reopens the editor on the unchanged emitter.
	{
		const TSharedPtr<FJsonObject> Back = Retargeted(Extract(*this, Target, TEXT("before the failing apply")), Target, TEXT("Update"));
		EmitterProperties(Back)->SetBoolField(TEXT("bLocalSpace"), bWasLocal);
		const FAssetDocumentResult Failed = FailingApply(*this, Target, Back, TEXT("failing apply while open"));
		TestFalse(TEXT("failing apply while open fails"), Failed.IsSuccess());
		TestTrue(TEXT("the failed result reports the reopened editor"), HasDiagnostic(Failed, TEXT("Niagara.EditorReopened")));
		TestTextEqual(*this, TEXT("the rollback restores the emitter"), Content(Extract(*this, Target, TEXT("after the rollback"))), HumanContent);
		Emitter = Cast<UNiagaraEmitter>(Target.Get());
		TestFalse(TEXT("after the rollback the package is clean"), Emitter == nullptr || Emitter->GetOutermost()->IsDirty());
		TickEditorUi();
		Copy = OnlyEmitterEditorCopy(*this);
		TestTrue(TEXT("after the rollback the reopened editor's copy is the restored emitter"), Copy != nullptr && IsLocalSpace(Copy) == !bWasLocal
			&& Determinism(Copy) == bHumanDeterminism);
	}

	// 4. The editor's undo / redo of an edit of its copy.
	if (Copy != nullptr && Emitter != nullptr)
	{
		ReleaseUndoHistory();
		SetDeterminism(*Copy, !bHumanDeterminism, true);
		TestTrue(TEXT("the editor's undo"), GEditor->Trans->CanUndo() && GEditor->UndoTransaction());
		TickEditorUi();
		TestEqual(TEXT("after the editor's undo the copy is as applied"), Determinism(Copy), bHumanDeterminism);
		// (The editor's Apply stays enabled: it follows the copy's change id, which its undo changes as well.)
		TestTrue(TEXT("the editor's redo"), GEditor->Trans->CanRedo() && GEditor->RedoTransaction());
		TickEditorUi();
		TestEqual(TEXT("after the editor's redo the copy has the edit"), Determinism(Copy), !bHumanDeterminism);
		TestTextEqual(*this, TEXT("the editor's undo / redo of its copy leave the asset"), Content(Extract(*this, Target, TEXT("after redo"))), HumanContent);
		ReleaseUndoHistory();
		// 5. asset_editor close with discard_unapplied: the copy's pending edit (the redone one) is dropped with the editor.
		Emitter = Cast<UNiagaraEmitter>(Target.Get());
		if (Emitter != nullptr && TestEqual(TEXT("the copy's edit is pending"), UE::AssetDocument::OpenEditor::GetPendingEditsButton(*Emitter), FString(TEXT("Apply"))))
		{
			FString Reason;
			TestTrue(TEXT("the editor closes without the pending edit"), UE::AssetDocument::OpenEditor::CloseDiscardingPendingEdits(*Emitter, Reason));
			TestEqual(TEXT("no emitter editor is left open"), EmitterEditorCopies().Num(), 0);
			TestTextEqual(*this, TEXT("the emitter keeps what it had"), Content(Extract(*this, Target, TEXT("after the discard"))), HumanContent);
			TestFalse(TEXT("the package stays saved"), Target.Get()->GetOutermost()->IsDirty());
		}
	}
	CloseQuietly(Target);
	TestEqual(TEXT("the editor closed"), EmitterEditorCopies().Num(), 0);
	return !HasAnyErrors();
}

// ---- Script -----------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraOpenEditorApplyScriptTest, "UEAssetDoc.Niagara.OpenEditorApply.Script",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraOpenEditorApplyScriptTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraOpenEditorTests;
	if (Editors() == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	const FTarget Target(TEXT("NM_OpenEd"));
	ON_SCOPE_EXIT
	{
		CloseQuietly(Target);
		Target.Cleanup();
	};
	const TSharedPtr<FJsonObject> Create = Parse(FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s",
		"Class": "/Script/Niagara.NiagaraScript", "Action": "Create", "Definitions": {}, "Properties": {},
		"Body": {"Usage": "Module", "Settings": {"Description": "Created"}}})json"), *Target.PackageName));
	if (!TestTrue(TEXT("a module is created"), ApplyDocument(*this, Target, Create, TEXT("create")).IsSuccess()))
	{
		return false;
	}
	ReleaseUndoHistory();
	const TSharedPtr<FJsonObject> Created = Extract(*this, Target, TEXT("created"));
	TSet<FObjectKey> Before = TransientScripts();
	if (!Created.IsValid() || !OpenEditor(*this, Target.Get(), TEXT("Niagara script editor")))
	{
		return !HasAnyErrors();
	}
	TestOpenedClean(*this, Target.Get());
	UNiagaraScript* Script = Cast<UNiagaraScript>(Target.Get());
	UNiagaraScript* Copy = Script != nullptr ? ScriptEditorCopy(*this, *Script, Before) : nullptr;
	if (!TestNotNull(TEXT("the script editor's working copy"), Copy))
	{
		return false;
	}
	TestFalse(TEXT("a fresh editor has nothing to apply"), CanExecute(Script, ScriptEditorName, ApplyCommand));

	TestEqual(TEXT("the editor's copy has the created description"), ScriptDescription(Copy), FString(TEXT("Created")));

	// 1. An apply changes the module's description: the editor is closed for it and reopened on a copy of the applied script.
	const TSharedPtr<FJsonObject> Described = Retargeted(Created, Target, TEXT("Update"));
	Child(Child(Described, TEXT("Body")), TEXT("Settings"))->SetStringField(TEXT("Description"), TEXT("Applied while open"));
	Before = TransientScripts();
	const FAssetDocumentResult Applied = ApplyDocument(*this, Target, Described, TEXT("apply while open"));
	if (!TestTrue(TEXT("apply while open"), Applied.IsSuccess()))
	{
		return false;
	}
	TestTrue(TEXT("the result reports the reopened editor"), HasDiagnostic(Applied, TEXT("Niagara.EditorReopened")));
	Script = Cast<UNiagaraScript>(Target.Get());
	TestEqual(TEXT("the asset holds the description"), ScriptDescription(Script), FString(TEXT("Applied while open")));
	TestFalse(TEXT("the apply saved the package"), Script == nullptr || Script->GetOutermost()->IsDirty());
	TestFalse(TEXT("the apply is not an undo step"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	TickEditorUi();
	Copy = Script != nullptr ? ScriptEditorCopy(*this, *Script, Before) : nullptr;
	TestEqual(TEXT("the reopened editor's copy holds the description"), ScriptDescription(Copy), FString(TEXT("Applied while open")));
	TestFalse(TEXT("the reopened editor has nothing to apply"), CanExecute(Script, ScriptEditorName, ApplyCommand));
	const FString AppliedContent = Content(Extract(*this, Target, TEXT("after apply while open")));

	// 2. A human graph edit in the copy is pending (Apply is enabled): an apply refuses it before any change and the
	//    editor keeps it; the editor's Apply writes it and keeps the applied description.
	int32 HumanX = INT32_MIN;
	if (Copy != nullptr && Script != nullptr && TestTrue(TEXT("a human graph edit"), EditGraphLikeTheEditor(UE::AssetDocument::Niagara::ScriptEditGraph(*Copy), 96)))
	{
		HumanX = FirstNodeX(UE::AssetDocument::Niagara::ScriptEditGraph(*Copy));
		TestTrue(TEXT("the human edit is pending"), CanExecute(Script, ScriptEditorName, ApplyCommand));
		const FAssetDocumentResult Pending = ApplyDocument(*this, Target, Retargeted(Created, Target, TEXT("Update")), TEXT("apply over the pending edit"));
		TestTrue(TEXT("an apply refuses the pending edit"), !Pending.IsSuccess() && HasDiagnostic(Pending, TEXT("Niagara.OpenInEditorUnsavedEdits")));
		TestFalse(TEXT("the refused apply did not reopen the editor"), HasDiagnostic(Pending, TEXT("Niagara.EditorReopened")));
		TestTrue(TEXT("the editor keeps the pending edit"), CanExecute(Script, ScriptEditorName, ApplyCommand)
			&& FirstNodeX(UE::AssetDocument::Niagara::ScriptEditGraph(*Copy)) == HumanX);
		TestTextEqual(*this, TEXT("the refusal changes nothing"), Content(Extract(*this, Target, TEXT("after the refusal"))), AppliedContent);
		// The script editor cannot close without its prompt: the refusal names apply_editor, close, reload_assets.
		TestTrue(TEXT("the refusal names the ways out"), NamesWaysOut(Pending, false));
		{
			FString Reason;
			TestFalse(TEXT("the script editor does not close without the pending edit"), UE::AssetDocument::OpenEditor::CloseDiscardingPendingEdits(*Script, Reason));
			TestTrue(TEXT("the editor stays open with the pending edit"), CanExecute(Script, ScriptEditorName, ApplyCommand));
		}
		TestTrue(TEXT("the editor's Apply (asset_editor apply_editor)"), ApplyEditor(Script));
		TickEditorUi();
		Script = Cast<UNiagaraScript>(Target.Get());
		TestEqual(TEXT("the editor's Apply writes the human edit"), Script != nullptr ? FirstNodeX(UE::AssetDocument::Niagara::ScriptEditGraph(*Script)) : INT32_MIN, HumanX);
		TestEqual(TEXT("the editor's Apply keeps the applied description"), ScriptDescription(Script), FString(TEXT("Applied while open")));
		TestFalse(TEXT("nothing is pending after the editor's Apply"), CanExecute(Script, ScriptEditorName, ApplyCommand));
		if (Script != nullptr && Script->GetOutermost()->IsDirty())
		{
			TestTrue(TEXT("the human save"), SaveAsset(*Script, Target.PackageFile));
		}
	}
	ReleaseUndoHistory();
	const FString HumanContent = Content(Extract(*this, Target, TEXT("after the human edit")));

	// 3. A failed apply rolls back and reopens the editor on the unchanged script.
	{
		Before = TransientScripts();
		const FAssetDocumentResult Failed = FailingApply(*this, Target, Retargeted(Created, Target, TEXT("Update")), TEXT("failing apply while open"));
		TestFalse(TEXT("failing apply while open fails"), Failed.IsSuccess());
		TestTrue(TEXT("the failed result reports the reopened editor"), HasDiagnostic(Failed, TEXT("Niagara.EditorReopened")));
		TestTextEqual(*this, TEXT("the rollback restores the script"), Content(Extract(*this, Target, TEXT("after the rollback"))), HumanContent);
		Script = Cast<UNiagaraScript>(Target.Get());
		TestFalse(TEXT("after the rollback the package is clean"), Script == nullptr || Script->GetOutermost()->IsDirty());
		TickEditorUi();
		Copy = Script != nullptr ? ScriptEditorCopy(*this, *Script, Before) : nullptr;
		TestEqual(TEXT("after the rollback the reopened editor's copy holds the description"), ScriptDescription(Copy), FString(TEXT("Applied while open")));
		TestFalse(TEXT("after the rollback the reopened editor has nothing to apply"), CanExecute(Script, ScriptEditorName, ApplyCommand));
	}
	CloseQuietly(Target);
	TestNull(TEXT("the editor closed"), ToolkitOf(Target.Get(), ScriptEditorName));
	return !HasAnyErrors();
}

// ---- Parameter collection ---------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraOpenEditorApplyCollectionTest, "UEAssetDoc.Niagara.OpenEditorApply.Collection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraOpenEditorApplyCollectionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraOpenEditorTests;
	if (Editors() == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	const FTarget Target(TEXT("NPC_OpenEd"));
	ON_SCOPE_EXIT
	{
		CloseQuietly(Target);
		Target.Cleanup();
	};
	auto CollectionDocument = [&Target](const TCHAR* Action, const TCHAR* ParametersJson)
	{
		return Parse(FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s", "Class": "/Script/Niagara.NiagaraParameterCollection",
			"Action": "%s", "Definitions": {}, "Properties": {}, "Body": {"Parameters": %s}})json"), *Target.PackageName, Action, ParametersJson));
	};
	if (!TestTrue(TEXT("the collection is created"), ApplyDocument(*this, Target,
		CollectionDocument(TEXT("Create"), TEXT(R"json([{"Name": "Speed", "Type": "float", "Value": 2.5}])json")), TEXT("create")).IsSuccess()))
	{
		return false;
	}
	ReleaseUndoHistory();
	if (!OpenEditor(*this, Target.Get(), TEXT("Niagara parameter collection editor")))
	{
		return !HasAnyErrors();
	}
	TestOpenedClean(*this, Target.Get());
	TestEqual(TEXT("the editor lists one parameter"), CollectionEditorRows(Target.Get()), 1);
	const FAssetEditorToolkit* Toolkit = ToolkitOf(Target.Get(), SystemEditorName);

	// 1. An apply adds a parameter, in place: the editor stays open and its parameter list shows it.
	const FAssetDocumentResult Applied = ApplyDocument(*this, Target, CollectionDocument(TEXT("Update"), TEXT(R"json([
		{"Name": "Speed", "Type": "float", "Value": 2.5}, {"Name": "Tint", "Type": "LinearColor", "Value": {"R": 1.0, "G": 0.5, "B": 0.25, "A": 1.0}}])json")),
		TEXT("apply while open"));
	if (!TestTrue(TEXT("apply while open"), Applied.IsSuccess()))
	{
		return false;
	}
	TestFalse(TEXT("the editor is not reopened"), HasDiagnostic(Applied, TEXT("Niagara.EditorReopened")));
	TestEqual(TEXT("the collection has two parameters"), CollectionParameters(Target.Get()), 2);
	TestFalse(TEXT("the apply saved the package"), Target.Get()->GetOutermost()->IsDirty());
	TestFalse(TEXT("the apply is not an undo step"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	TestTrue(TEXT("the same editor is still open"), ToolkitOf(Target.Get(), SystemEditorName) == Toolkit);
	TestEqual(TEXT("the editor's list shows the new parameter"), CollectionEditorRows(Target.Get()), 2);
	TickEditorUi();
	const FString AppliedContent = Content(Extract(*this, Target, TEXT("after apply while open")));

	// 2. A failed apply (the new parameter deleted) rolls back; the editor's list shows the rolled-back collection.
	{
		const FAssetDocumentResult Failed = FailingApply(*this, Target, CollectionDocument(TEXT("Update"),
			TEXT(R"json([{"Name": "Speed", "Type": "float", "Value": 2.5}])json")), TEXT("failing apply while open"));
		TestFalse(TEXT("failing apply while open fails"), Failed.IsSuccess());
		TestTextEqual(*this, TEXT("the rollback restores the collection"), Content(Extract(*this, Target, TEXT("after the rollback"))), AppliedContent);
		TestFalse(TEXT("after the rollback the package is clean"), Target.Get()->GetOutermost()->IsDirty());
		TestEqual(TEXT("after the rollback the editor's list shows both parameters"), CollectionEditorRows(Target.Get()), 2);
		TickEditorUi();
	}

	// 3. The editor's own edit (its "+": a parameter added) and its undo / redo keep the list in step.
	if (UNiagaraParameterCollection* Collection = Cast<UNiagaraParameterCollection>(Target.Get()))
	{
		ReleaseUndoHistory();
		{
			const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentNiagaraOpenEditorTests", "AddParameter", "Add Parameter"));
			Collection->Modify();
			Collection->AddParameter(Collection->ConditionalAddFullNamespace(TEXT("Human")), FNiagaraTypeDefinition::GetFloatDef());
		}
		TestEqual(TEXT("the human parameter is added"), CollectionParameters(Collection), 3);
		TestTrue(TEXT("the editor's undo"), GEditor->Trans->CanUndo() && GEditor->UndoTransaction());
		TickEditorUi();
		TestEqual(TEXT("after the editor's undo the collection is as applied"), CollectionParameters(Target.Get()), 2);
		TestEqual(TEXT("after the editor's undo the editor's list shows the applied collection"), CollectionEditorRows(Target.Get()), 2);
		TestTrue(TEXT("the editor's redo"), GEditor->Trans->CanRedo() && GEditor->RedoTransaction());
		TickEditorUi();
		TestEqual(TEXT("after the editor's redo the editor's list shows the human parameter"), CollectionEditorRows(Target.Get()), 3);
		ReleaseUndoHistory();
	}
	CloseQuietly(Target);
	TestNull(TEXT("the editor closed"), ToolkitOf(Target.Get(), SystemEditorName));
	return !HasAnyErrors();
}

// ---- Dependents -------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraOpenEditorApplyDependentsTest, "UEAssetDoc.Niagara.OpenEditorApply.Dependents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraOpenEditorApplyDependentsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraOpenEditorTests;
	// (Minimal: Fountain's compiled scripts carry data interfaces their parameter stores lack here, "Data interface count
	// mismatch during script presave", and a recompile of a system with a preview component then ensures.)
	UObject* Source = LoadObject<UObject>(nullptr, *(FString(TemplateEmitters) + TEXT("/Minimal.Minimal")));
	if (!TestNotNull(TEXT("Minimal loads"), Source) || Editors() == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	const FTarget Parent(TEXT("NE_OpenEdParent"));
	const FTarget ChildTarget(TEXT("NE_OpenEdChild"));
	const FTarget User(TEXT("NS_OpenEdUser"));
	ON_SCOPE_EXIT
	{
		CloseQuietly(User);
		CloseQuietly(ChildTarget);
		User.Cleanup();
		ChildTarget.Cleanup();
		Parent.Cleanup();
	};
	if (!TestNotNull(TEXT("parent copy"), DuplicateTo(*Source, Parent)))
	{
		return false;
	}
	{
		const TSharedPtr<FJsonObject> Options = Retargeted(Extract(*this, Parent, TEXT("parent options")), Parent, TEXT("Update"));
		Child(Options, TEXT("Properties"))->SetBoolField(TEXT("bIsInheritable"), true);
		if (!TestTrue(TEXT("the parent is made inheritable"), ApplyDocument(*this, Parent, Options, TEXT("inheritable")).IsSuccess()))
		{
			return false;
		}
	}
	const TSharedPtr<FJsonObject> ChildCreate = Parse(FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s", "Class": "/Script/Niagara.NiagaraEmitter",
		"Action": "Create", "Definitions": {}, "Properties": {}, "Body": {"Parent": "%s"}})json"), *ChildTarget.PackageName, *Parent.PackageName));
	const TSharedPtr<FJsonObject> UserCreate = Parse(FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Target": "%s",
		"Class": "/Script/Niagara.NiagaraSystem", "Action": "Create", "Definitions": {}, "Properties": {},
		"Body": {"Emitters": [{"Name": "Child", "Parent": "%s"}]}})json"), *User.PackageName, *ChildTarget.PackageName));
	if (!TestTrue(TEXT("a child emitter asset is created"), ApplyDocument(*this, ChildTarget, ChildCreate, TEXT("child")).IsSuccess())
		|| !TestTrue(TEXT("a system uses the child"), ApplyDocument(*this, User, UserCreate, TEXT("user system")).IsSuccess()))
	{
		return false;
	}
	ReleaseUndoHistory();
	UNiagaraEmitter* ParentEmitter = Cast<UNiagaraEmitter>(Parent.Get());
	const bool bWasLocal = IsLocalSpace(ParentEmitter);
	if (!OpenEditor(*this, ChildTarget.Get(), TEXT("Niagara emitter editor")) || !OpenEditor(*this, User.Get(), TEXT("Niagara system editor")))
	{
		return !HasAnyErrors();
	}
	TestOpenedClean(*this, ChildTarget.Get());
	TestOpenedClean(*this, User.Get());
	UNiagaraEmitter* ChildCopy = OnlyEmitterEditorCopy(*this, ParentEmitter);
	TestEqual(TEXT("the child editor's copy has the parent's local space"), IsLocalSpace(ChildCopy), bWasLocal);

	// The parent's edit merges into the child, the system and both open editors (as the parent editor's Apply does).
	const TSharedPtr<FJsonObject> Edit = Retargeted(Extract(*this, Parent, TEXT("parent")), Parent, TEXT("Update"));
	EmitterProperties(Edit)->SetBoolField(TEXT("bLocalSpace"), !bWasLocal);
	const FAssetDocumentResult Applied = ApplyDocument(*this, Parent, Edit, TEXT("parent edit"));
	TestTrue(TEXT("the parent applies and refreshes its dependents"), Applied.IsSuccess() && HasDiagnostic(Applied, TEXT("Niagara.DependentsUpdated")));
	TestFalse(TEXT("open dependents are not refused"), HasDiagnostic(Applied, TEXT("Niagara.DependentOpenInEditor")));
	TickEditorUi();
	TestEqual(TEXT("the child follows its parent"), IsLocalSpace(Cast<UNiagaraEmitter>(ChildTarget.Get())), !bWasLocal);
	UNiagaraSystem* UserSystem = Cast<UNiagaraSystem>(User.Get());
	const FNiagaraEmitterHandle* Handle = UserSystem != nullptr && UserSystem->GetEmitterHandles().Num() == 1 ? &UserSystem->GetEmitterHandles()[0] : nullptr;
	TestTrue(TEXT("the system's emitter follows"), Handle != nullptr && Handle->GetEmitterData() != nullptr && Handle->GetEmitterData()->bLocalSpace == !bWasLocal);
	ChildCopy = OnlyEmitterEditorCopy(*this, ParentEmitter);
	TestEqual(TEXT("the child editor's copy follows the parent"), IsLocalSpace(ChildCopy), !bWasLocal);
	const TSharedPtr<FNiagaraSystemViewModel> UserEditor = SystemEditorViewModel(UserSystem);
	const TSharedPtr<FNiagaraEmitterHandleViewModel> Shown = UserEditor.IsValid() ? HandleNamed(*UserEditor, TEXT("Child")) : nullptr;
	TestTrue(TEXT("the system editor shows the system's merged emitter"), Shown.IsValid() && Handle != nullptr
		&& Shown->GetEmitterHandle() == Handle && Shown->GetEmitterViewModel()->GetEmitter().Emitter == Handle->GetInstance().Emitter);
	// The child editor's Apply after the merge writes the merged copy: the child keeps the parent's value.
	if (CanExecute(ChildTarget.Get(), SystemEditorName, ApplyCommand))
	{
		TestTrue(TEXT("the child editor's Apply"), Execute(ChildTarget.Get(), SystemEditorName, ApplyCommand));
		TickEditorUi();
	}
	TestEqual(TEXT("after the child editor's Apply the child keeps the parent's value"), IsLocalSpace(Cast<UNiagaraEmitter>(ChildTarget.Get())), !bWasLocal);
	CloseQuietly(User);
	CloseQuietly(ChildTarget);
	TestEqual(TEXT("the editors closed"), EmitterEditorCopies(ParentEmitter).Num(), 0);
	return !HasAnyErrors();
}
#endif // WITH_DEV_AUTOMATION_TESTS
