// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

// Shared harness of the open-editor apply behaviour tests (<Group>.OpenEditorApply): the asset is
// open in its asset editor while documents are applied through the production Service, exactly as
// the MCP tools send them; the tests then read what the open editor itself holds (its panels,
// lists, previews) through the toolkit and its Slate widgets. Header only, test builds only. Its includers depend on
// GraphEditor, PropertyEditor and Slate.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/UICommandInfo.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/Docking/TabManager.h"
#include "GraphEditor.h"
#include "HAL/FileManager.h"
#include "IDetailsView.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "PropertyPath.h"
#include "Runtime/AssetDocumentRuntimeTestHooks.h"
#include "SGraphActionMenu.h"
#include "SGraphPanel.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Templates/UnrealTemplate.h"
#include "TickableEditorObject.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWidget.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace UE::AssetDocument::OpenEditorTests
{
// One test asset under /Game/AssetDocumentOpenEditor/<run>/.
struct FTarget
{
	FString Name;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	FTarget(const FString& Folder, const TCHAR* InName)
		: Name(InName)
	{
		PackageName = FString(TEXT("/Game/")) / Folder / Name;
		ObjectPath = PackageName + TEXT(".") + Name;
		PackageFile = FPaths::ConvertRelativePathToFull(
			FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()));
		SidecarPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), Folder, Name + TEXT(".assetdoc.json")));
	}

	UObject* Get() const { return FSoftObjectPath(ObjectPath).ResolveObject(); }

	template <typename T>
	T* Get() const { return Cast<T>(Get()); }

	UPackage* Package() const { return FindPackage(nullptr, *PackageName); }
};

inline FString NewFolder()
{
	return FString(TEXT("AssetDocumentOpenEditor/")) + FGuid::NewGuid().ToString(EGuidFormats::Digits);
}

inline void ReleaseUndoHistory()
{
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
		&& GEditor->Trans->GetQueueLength() > 0)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentOpenEditorTests", "Cleanup", "AssetDocument open-editor test cleanup"));
	}
}

// Ticks the editor's tickable objects (toolkits, previews, deferred refreshes) a few times.
inline void Tick()
{
	for (int32 Index = 0; Index < 3; ++Index)
	{
		FTickableEditorObject::TickObjects(0.f);
	}
}

// Ticks the editor UI: Slate (a graph panel rebuilds its node widgets on a timer after NotifyGraphChanged, when drawn)
// and the editor tickables.
inline void TickEditorUi()
{
	for (int32 Index = 0; Index < 3; ++Index)
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().Tick();
		}
		FTickableEditorObject::TickObjects(0.f);
	}
}

inline UAssetEditorSubsystem* Editors()
{
	return GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
}

inline IAssetEditorInstance* FindEditor(UObject* Asset)
{
	UAssetEditorSubsystem* Subsystem = Editors();
	return Subsystem != nullptr && Asset != nullptr ? Subsystem->FindEditorForAsset(Asset, false) : nullptr;
}

// An apply's close and reopen of the editor open on a package, seen through the asset editor events while the watch
// lives: the open editor closes, then an editor opens on the package again. An editor opened after the close is a new
// editor even when it reuses the closed editor's address, which comparing editor pointers cannot tell.
class FEditorReopenWatch
{
public:
	FEditorReopenWatch(const FString& InPackageName, const IAssetEditorInstance* InOpenEditor)
		: PackageName(InPackageName)
		, OpenEditor(InOpenEditor)
	{
		if (UAssetEditorSubsystem* Subsystem = Editors())
		{
			WatchedSubsystem = Subsystem;
			ClosedHandle = Subsystem->OnAssetClosedInEditor().AddRaw(this, &FEditorReopenWatch::HandleClosed);
			OpenedHandle = Subsystem->OnAssetOpenedInEditor().AddRaw(this, &FEditorReopenWatch::HandleOpened);
		}
	}

	~FEditorReopenWatch()
	{
		Stop();
	}

	FEditorReopenWatch(const FEditorReopenWatch&) = delete;
	FEditorReopenWatch& operator=(const FEditorReopenWatch&) = delete;

	void Stop()
	{
		if (UAssetEditorSubsystem* Subsystem = WatchedSubsystem.Get())
		{
			Subsystem->OnAssetClosedInEditor().Remove(ClosedHandle);
			Subsystem->OnAssetOpenedInEditor().Remove(OpenedHandle);
		}
		WatchedSubsystem.Reset();
	}

	// The editor open when the watch started closed.
	bool Closed() const { return bClosed; }

	// The editor that opened on the package after that close (the last one), or null.
	const IAssetEditorInstance* Reopened() const { return ReopenedEditor; }

private:
	bool IsWatched(const UObject* Asset) const
	{
		return Asset != nullptr && Asset->GetOutermost()->GetName() == PackageName;
	}

	void HandleClosed(UObject* Asset, IAssetEditorInstance* Instance)
	{
		if (!bClosed && Instance == OpenEditor && IsWatched(Asset))
		{
			bClosed = true;
		}
	}

	void HandleOpened(UObject* Asset, IAssetEditorInstance* Instance)
	{
		if (bClosed && IsWatched(Asset))
		{
			ReopenedEditor = Instance;
		}
	}

	FString PackageName;
	const IAssetEditorInstance* OpenEditor = nullptr;
	TWeakObjectPtr<UAssetEditorSubsystem> WatchedSubsystem;
	FDelegateHandle ClosedHandle;
	FDelegateHandle OpenedHandle;
	bool bClosed = false;
	const IAssetEditorInstance* ReopenedEditor = nullptr;
};

// The asset editor toolkit editing Asset (every stock asset editor is an FAssetEditorToolkit).
inline FAssetEditorToolkit* FindToolkit(UObject* Asset)
{
	return static_cast<FAssetEditorToolkit*>(FindEditor(Asset));
}

// Opens Asset in its asset editor (UAssetEditorSubsystem::OpenEditorForAsset, as a double click in the
// Content Browser does); null when no editor opened.
inline FAssetEditorToolkit* Open(FAutomationTestBase& Test, UObject* Asset)
{
	UAssetEditorSubsystem* Subsystem = Editors();
	if (Subsystem == nullptr || Asset == nullptr || !Subsystem->OpenEditorForAsset(Asset))
	{
		Test.AddError(FString::Printf(TEXT("%s did not open in its asset editor"), *GetPathNameSafe(Asset)));
		return nullptr;
	}
	Tick();
	FAssetEditorToolkit* Toolkit = FindToolkit(Asset);
	Test.TestNotNull(*FString::Printf(TEXT("%s is open in its asset editor"), *GetPathNameSafe(Asset)), Toolkit);
	if (Toolkit != nullptr)
	{
		Test.AddInfo(FString::Printf(TEXT("opened %s in %s"), *GetPathNameSafe(Asset), *Toolkit->GetToolkitFName().ToString()));
	}
	return Toolkit;
}

// The editor's own Save (its toolbar Save, Ctrl+S): the asset editor's SaveAsset command
// (FAssetEditorCommonCommands, context "AssetEditor") on the toolkit's command list, which runs the toolkit's
// SaveAsset_Execute (the Level Sequence editor's: Sequencer's SaveCurrentMovieScene). Both save through
// FEditorFileUtils::PromptForCheckoutAndSave, which in an unattended session (FApp::IsUnattended: -unattended, as
// automation runs) saves nothing and returns Cancelled unless an unattended script is running
// (GIsRunningUnattendedScript: it then saves without prompts through UEditorLoadingAndSavingUtils::SavePackages).
// So SaveFromEditor runs the command as such a script. True when the command ran and Asset's package is saved (not
// dirty).
inline bool RunSaveCommand(FAssetEditorToolkit& Toolkit)
{
	const TSharedPtr<FUICommandInfo> SaveAsset = FInputBindingManager::Get().FindCommandInContext(TEXT("AssetEditor"), TEXT("SaveAsset"));
	return SaveAsset.IsValid() && Toolkit.GetToolkitCommands()->ExecuteAction(SaveAsset.ToSharedRef());
}

inline bool SaveFromEditor(FAssetEditorToolkit& Toolkit, UObject& Asset)
{
	TGuardValue<bool> UnattendedScript(GIsRunningUnattendedScript, true);
	return RunSaveCommand(Toolkit) && !Asset.GetOutermost()->IsDirty();
}

// Test of the cause above: in an unattended session the bare command saves nothing (true when it ran and Asset's
// package is still dirty, or when the session is attended and so not checked).
inline bool BareSaveCommandSavesNothing(FAssetEditorToolkit& Toolkit, UObject& Asset)
{
	if (!FApp::IsUnattended() || GIsRunningUnattendedScript)
	{
		return true;
	}
	return RunSaveCommand(Toolkit) && Asset.GetOutermost()->IsDirty();
}

// SaveFromEditor in the asset editor open on Asset; false when none is open.
inline bool SaveFromEditor(UObject& Asset)
{
	FAssetEditorToolkit* Toolkit = FindToolkit(&Asset);
	return Toolkit != nullptr && SaveFromEditor(*Toolkit, Asset);
}

inline void Close(UObject* Asset)
{
	if (UAssetEditorSubsystem* Subsystem = Editors(); Subsystem != nullptr && Asset != nullptr)
	{
		Subsystem->CloseAllEditorsForAsset(Asset);
		Tick();
	}
}

inline void LogResult(FAutomationTestBase& Test, const FString& Stage, const FAssetDocumentResult& Result)
{
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s %s"), *Stage, *Diagnostic.Severity, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
}

// The document goes into the target's sidecar and is applied as apply_asset_document does.
inline FAssetDocumentResult Apply(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const FString& Stage)
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

inline bool ApplyOk(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const FString& Stage)
{
	return Test.TestTrue(*FString::Printf(TEXT("%s: applies"), *Stage), Apply(Test, Target, Json, Stage).IsSuccess());
}

// An apply that fails after it changed the asset (RequestCoordinator.TestInjectedFailure at the
// Repair phase, right after the profile's mutation): Core rolls it back.
inline FAssetDocumentResult ApplyFailingAfterMutation(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const FString& Stage)
{
	FAssetDocumentRuntimeTestHooks::FailNextRequestAtPhase(EAssetDocumentRuntimeRequestPhase::Repair);
	const FAssetDocumentResult Result = Apply(Test, Target, Json, Stage);
	FAssetDocumentRuntimeTestHooks::Reset();
	Test.TestFalse(*FString::Printf(TEXT("%s: fails"), *Stage), Result.IsSuccess());
	Test.TestTrue(*FString::Printf(TEXT("%s: fails at the injected point (after the mutation)"), *Stage),
		Result.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == TEXT("RequestCoordinator.TestInjectedFailure");
		}));
	return Result;
}

inline FString Extract(FAutomationTestBase& Test, const FTarget& Target, const FString& Stage)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Target.ObjectPath;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Extract(Request);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract succeeds"), *Stage), Result.IsSuccess() && Result.Payload.IsValid()))
	{
		LogResult(Test, Stage, Result);
		return FString();
	}
	FString Text;
	const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Result.Payload.ToSharedRef(), Writer);
	return Text;
}

// Editor Undo after an apply: the apply itself is never an Undo entry (AssetDocumentUndoHistory.h),
// so Undo / Redo of whatever the history still holds must keep the applied state.
inline void TestUndoKeepsApply(FAutomationTestBase& Test, const FTarget& Target, const FString& Applied, const FString& Stage)
{
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return;
	}
	int32 Undone = 0;
	while (Undone < 8 && GEditor->Trans->CanUndo() && GEditor->UndoTransaction())
	{
		++Undone;
	}
	int32 Redone = 0;
	while (Redone < Undone && GEditor->Trans->CanRedo() && GEditor->RedoTransaction())
	{
		++Redone;
	}
	Tick();
	Test.TestEqual(*FString::Printf(TEXT("%s: Undo / Redo keep the applied state (%d undone)"), *Stage, Undone),
		Extract(Test, Target, Stage), Applied);
}

// Every widget of the given Slate type name (SWidget::GetType, e.g. "SDetailsView") under Root.
inline void FindWidgets(const TSharedRef<SWidget>& Root, const FName Type, TArray<TSharedRef<SWidget>>& Out)
{
	if (Root->GetType() == Type)
	{
		Out.Add(Root);
	}
	FChildren* Children = Root->GetChildren();
	for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
	{
		FindWidgets(Children->GetChildAt(Index), Type, Out);
	}
}

// True when an action menu (SGraphActionMenu: My Blueprint's members, the Blackboard editor's keys, ...)
// under Root lists an item of that name. Selects it, as a click in the list does.
inline bool ActionMenuLists(const TSharedRef<SWidget>& Root, const FName Item)
{
	TArray<TSharedRef<SWidget>> Menus;
	FindWidgets(Root, TEXT("SGraphActionMenu"), Menus);
	for (const TSharedRef<SWidget>& Menu : Menus)
	{
		if (static_cast<SGraphActionMenu&>(Menu.Get()).SelectItemByName(Item))
		{
			return true;
		}
	}
	return false;
}

// The content of one of the toolkit's tabs (spawned or not yet drawn), or null.
inline TSharedPtr<SWidget> TabContent(FAssetEditorToolkit& Toolkit, const FName TabId)
{
	const TSharedPtr<FTabManager> Tabs = Toolkit.GetTabManager();
	const TSharedPtr<SDockTab> Tab = Tabs.IsValid() ? Tabs->FindExistingLiveTab(FTabId(TabId)) : nullptr;
	return Tab.IsValid() ? TSharedPtr<SWidget>(Tab->GetContent()) : nullptr;
}

// Widgets of Type in the toolkit's tab TabId (NAME_None: in the whole editor window content).
inline TArray<TSharedRef<SWidget>> FindInEditor(FAssetEditorToolkit& Toolkit, const FName Type, const FName TabId = NAME_None)
{
	TArray<TSharedRef<SWidget>> Found;
	TSharedPtr<SWidget> Root;
	if (!TabId.IsNone())
	{
		Root = TabContent(Toolkit, TabId);
	}
	else if (const TSharedPtr<FTabManager> Tabs = Toolkit.GetTabManager(); Tabs.IsValid() && Tabs->GetOwnerTab().IsValid())
	{
		Root = Tabs->GetOwnerTab()->GetContent();
	}
	if (Root.IsValid())
	{
		FindWidgets(Root.ToSharedRef(), Type, Found);
	}
	return Found;
}

// The details views (SDetailsView / SStructureDetailsView, both IDetailsView) in a tab.
inline TArray<IDetailsView*> DetailsViewsIn(FAssetEditorToolkit& Toolkit, const FName TabId)
{
	TArray<IDetailsView*> Views;
	for (const FName Type : {FName(TEXT("SDetailsView")), FName(TEXT("SStructureDetailsView"))})
	{
		for (const TSharedRef<SWidget>& Widget : FindInEditor(Toolkit, Type, TabId))
		{
			Views.Add(static_cast<IDetailsView*>(&Widget.Get()));
		}
	}
	return Views;
}

// The property names (leaf FProperty of each displayed row) a details view shows.
inline TArray<FString> DisplayedProperties(IDetailsView& View)
{
	TArray<FString> Names;
	for (const FPropertyPath& Path : View.GetPropertiesInOrderDisplayed())
	{
		if (const FProperty* Leaf = Path.GetLeafMostProperty().Property.Get())
		{
			Names.Add(Leaf->GetName());
		}
	}
	return Names;
}

// The open graph panel showing Graph has a node widget for every node of the graph. Panels build their node widgets
// when drawn; an unattended session may never draw them.
inline bool PanelShowsEveryNode(const UEdGraph& Graph)
{
	const TSharedPtr<SGraphEditor> GraphEditor = SGraphEditor::FindGraphEditorForGraph(&Graph);
	const SGraphPanel* Panel = GraphEditor.IsValid() ? GraphEditor->GetGraphPanel() : nullptr;
	if (Panel == nullptr || Graph.Nodes.IsEmpty())
	{
		return false;
	}
	for (const UEdGraphNode* Node : Graph.Nodes)
	{
		if (Node != nullptr && !Panel->GetNodeWidgetFromGuid(Node->NodeGuid).IsValid())
		{
			return false;
		}
	}
	return true;
}

// The open graph panel showing Graph holds a node widget with Guid.
inline bool PanelHasNodeWidget(const UEdGraph& Graph, const FGuid& Guid)
{
	const TSharedPtr<SGraphEditor> GraphEditor = SGraphEditor::FindGraphEditorForGraph(&Graph);
	const SGraphPanel* Panel = GraphEditor.IsValid() ? GraphEditor->GetGraphPanel() : nullptr;
	return Panel != nullptr && Panel->GetNodeWidgetFromGuid(Guid).IsValid();
}

// Unloads (or, never saved, deletes) the test assets and removes their files.
inline void Cleanup(const TArray<FTarget>& Targets)
{
	ReleaseUndoHistory();
	TArray<UPackage*> Packages;
	for (const FTarget& Target : Targets)
	{
		if (UObject* Asset = Target.Get())
		{
			Close(Asset);
		}
		if (UPackage* Package = Target.Package())
		{
			Package->SetDirtyFlag(false);
			Packages.Add(Package);
		}
	}
	if (!Packages.IsEmpty())
	{
		UPackageTools::UnloadPackages(Packages);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	TArray<FString> Files;
	for (const FTarget& Target : Targets)
	{
		IFileManager::Get().Delete(*Target.PackageFile, false, true);
		IFileManager::Get().Delete(*Target.SidecarPath, false, true);
		Files.Add(Target.PackageFile);
	}
	FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(Files);
	if (!Targets.IsEmpty())
	{
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Targets[0].SidecarPath), false, true);
	}
}
} // namespace UE::AssetDocument::OpenEditorTests
#endif // WITH_DEV_AUTOMATION_TESTS
