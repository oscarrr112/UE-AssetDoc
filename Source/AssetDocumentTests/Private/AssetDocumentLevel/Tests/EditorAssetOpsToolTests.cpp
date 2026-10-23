// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Asset tools (AssetOps/EditorAssetOps.h): save_assets, reload_assets, asset_editor, export_assets.
//
//   SaveAssets: listed assets with changes are saved, clean ones only with force; all_dirty "content"
//     saves every changed content package; a missing asset is refused on its own (207).
//   ReloadAssets: a reload reads the file (memory changes without the dirty flag are replaced); unsaved
//     changes are refused unless force; the open map is refused; an asset not in memory needs nothing.
//   FreshTextureReloadDelete: a new texture whose data is still building is reloaded with reload_assets and
//     another is deleted as delete_assets does: the build is finished first, and the engine does not report the
//     texture as still compiling.
//   AssetEditor: open / list / close of an asset's editor.
//   ExportAssets: a sound to .wav and a texture to .png with the engine's exporters; an existing file
//     needs overwrite; a folder outside the project is refused, as are a '..' segment and a project folder
//     that links out of the project; a class without a default names formats.
//   ExportAssetsLinkedFile: in a valid export folder, an export file that is a link to a file outside the project
//     is refused, also with overwrite, and the outside file is unchanged; so is a file the exporter writes beside the
//     main one (a static mesh's lightmap-UV OBJ); once the link is gone the same exports write their files.

#include "AssetDocumentAssetDelete.h"
#include "AssetDocumentPathsTestSupport.h"
#include "AssetImportTestSupport.h"
#include "AssetOps/EditorAssetOps.h"
#include "AssetToolsModule.h"
#include "Engine/Texture2D.h"
#include "Factories/Texture2dFactoryNew.h"
#include "EditorAssetOpsTestSupport.h"
#include "EditorAssetOpsTestTypes.h"
#include "IMaterialEditor.h"
#include "Materials/Material.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "LevelTestSupport.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/SavePackage.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentAssetOpsToolTests
{
using namespace UE::AssetDocumentLevel::Private;

FLevelToolResult RunTool(FAutomationTestBase& Test, FLevelToolResult (*Run)(const TSharedPtr<FJsonObject>&), const FString& Json,
	const TCHAR* Stage)
{
	const FLevelToolResult Result = Run(AssetDocumentLevelTests::ParseJsonObject(Json));
	Test.AddInfo(FString::Printf(TEXT("%s: %d %s"), Stage, Result.HttpStatus, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("  %s at %s: %s"), *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
	return Result;
}

TArray<FString> Strings(const FLevelToolResult& Result, const TCHAR* Field)
{
	TArray<FString> Values;
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (Result.Payload.IsValid() && Result.Payload->TryGetArrayField(Field, Array))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			Values.Add(Value->AsString());
		}
	}
	return Values;
}

bool HasCode(const FLevelToolResult& Result, const TCHAR* Code)
{
	return Result.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}

UAssetDocumentAssetOpsTestRefAsset* Resolve(const FString& ObjectPath)
{
	return FindObject<UAssetDocumentAssetOpsTestRefAsset>(nullptr, *ObjectPath);
}

// Counts the editor's saves of one package.
class FSaveCounter
{
public:
	explicit FSaveCounter(FString InPackage) : Package(MoveTemp(InPackage))
	{
		Handle = UPackage::PackageSavedWithContextEvent.AddRaw(this, &FSaveCounter::OnSaved);
	}
	~FSaveCounter() { UPackage::PackageSavedWithContextEvent.Remove(Handle); }
	FSaveCounter(const FSaveCounter&) = delete;
	FSaveCounter& operator=(const FSaveCounter&) = delete;

	int32 Count = 0;

private:
	void OnSaved(const FString& Filename, UPackage* Saved, FObjectPostSaveContext Context)
	{
		Count += Saved != nullptr && Saved->GetName() == Package ? 1 : 0;
	}
	FString Package;
	FDelegateHandle Handle;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAssetOpsSaveAssetsTest, "UEAssetDoc.AssetOps.SaveAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAssetOpsSaveAssetsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAssetOpsToolTests;
	using namespace AssetDocumentAssetOpsTestSupport;
	FScopedTestAssets Assets;
	UAssetDocumentAssetOpsTestRefAsset* Changed = Assets.NewRefAsset(TEXT("DA_Changed"));
	UAssetDocumentAssetOpsTestRefAsset* Clean = Assets.NewRefAsset(TEXT("DA_Clean"));
	if (!TestTrue(TEXT("fixture saved"), FScopedTestAssets::Save({Changed, Clean})))
	{
		return false;
	}
	Changed->Value = 5;
	Changed->MarkPackageDirty();
	FSaveCounter CleanSaves(Assets.PackageName(TEXT("DA_Clean")));
	const FLevelToolResult Listed = RunTool(*this, &RunSaveAssets, FString::Printf(TEXT(R"({"assets": ["%s", "%s", "%s"]})"),
		*Assets.PackageName(TEXT("DA_Changed")), *Assets.ObjectPath(TEXT("DA_Clean")), *Assets.PackageName(TEXT("DA_Missing"))), TEXT("listed"));
	TestEqual(TEXT("one missing asset: partial result"), Listed.HttpStatus, 207);
	TestTrue(TEXT("Assets.NotFound"), HasCode(Listed, TEXT("Assets.NotFound")));
	TestEqual(TEXT("the changed asset is saved"), Strings(Listed, TEXT("saved")), TArray<FString>({Assets.PackageName(TEXT("DA_Changed"))}));
	TestEqual(TEXT("the clean asset is reported clean"), Strings(Listed, TEXT("clean")), TArray<FString>({Assets.PackageName(TEXT("DA_Clean"))}));
	TestFalse(TEXT("no unsaved changes remain"), Changed->GetPackage()->IsDirty());
	TestEqual(TEXT("the clean asset was not written"), CleanSaves.Count, 0);

	const FLevelToolResult Forced = RunTool(*this, &RunSaveAssets, FString::Printf(TEXT(R"({"assets": ["%s"], "force": true})"),
		*Assets.PackageName(TEXT("DA_Clean"))), TEXT("force"));
	TestTrue(TEXT("force succeeds"), Forced.bSuccess);
	TestEqual(TEXT("force re-saves the clean asset"), CleanSaves.Count, 1);

	// all_dirty "content": every changed content package, a never-saved one included.
	UAssetDocumentAssetOpsTestRefAsset* Fresh = Assets.NewRefAsset(TEXT("DA_Fresh"));
	Clean->Value = 3;
	Clean->MarkPackageDirty();
	const FLevelToolResult All = RunTool(*this, &RunSaveAssets, TEXT(R"({"all_dirty": true, "only": "content"})"), TEXT("all_dirty"));
	TestTrue(TEXT("all_dirty succeeds"), All.bSuccess);
	const TArray<FString> Saved = Strings(All, TEXT("saved"));
	TestTrue(TEXT("the never-saved asset is saved"), Saved.Contains(Assets.PackageName(TEXT("DA_Fresh"))));
	TestTrue(TEXT("the changed asset is saved"), Saved.Contains(Assets.PackageName(TEXT("DA_Clean"))));
	TestTrue(TEXT("its file exists"), IFileManager::Get().FileExists(*Assets.File(TEXT("DA_Fresh"))));
	TestFalse(TEXT("nothing left dirty"), Fresh->GetPackage()->IsDirty() || Clean->GetPackage()->IsDirty());

	TestEqual(TEXT("assets and all_dirty together are invalid"), RunTool(*this, &RunSaveAssets, FString::Printf(
		TEXT(R"({"assets": ["%s"], "all_dirty": true})"), *Assets.PackageName(TEXT("DA_Clean"))), TEXT("both")).HttpStatus, 400);
	TestEqual(TEXT("an unknown field is invalid"), RunTool(*this, &RunSaveAssets, TEXT(R"({"all_dirty": true, "maps": true})"),
		TEXT("unknown")).HttpStatus, 400);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAssetOpsReloadAssetsTest, "UEAssetDoc.AssetOps.ReloadAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAssetOpsReloadAssetsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAssetOpsToolTests;
	using namespace AssetDocumentAssetOpsTestSupport;
	FScopedTestAssets Assets;
	const FString Path = Assets.ObjectPath(TEXT("DA_Reload"));
	UAssetDocumentAssetOpsTestRefAsset* Asset = Assets.NewRefAsset(TEXT("DA_Reload"));
	Asset->Value = 7;
	if (!TestTrue(TEXT("fixture saved"), FScopedTestAssets::Save({Asset})))
	{
		return false;
	}

	// Memory differs from the file without the dirty flag (as after a change on disk): a reload reads the file.
	Asset->Value = 3;
	Asset->GetPackage()->SetDirtyFlag(false);
	const FLevelToolResult Stale = RunTool(*this, &RunReloadAssets, FString::Printf(TEXT(R"({"assets": ["%s"]})"), *Path), TEXT("stale"));
	TestTrue(TEXT("reloaded"), Stale.bSuccess && Strings(Stale, TEXT("reloaded")).Contains(Assets.PackageName(TEXT("DA_Reload"))));
	Asset = Resolve(Path);
	TestTrue(TEXT("the file's value is back"), Asset != nullptr && Asset->Value == 7);

	// Unsaved changes: refused, kept; force discards them.
	if (Asset == nullptr)
	{
		return false;
	}
	Asset->Value = 9;
	Asset->MarkPackageDirty();
	const FLevelToolResult Dirty = RunTool(*this, &RunReloadAssets, FString::Printf(TEXT(R"({"assets": ["%s"]})"), *Path), TEXT("dirty"));
	TestTrue(TEXT("Assets.UnsavedChanges"), !Dirty.bSuccess && HasCode(Dirty, TEXT("Assets.UnsavedChanges")));
	Asset = Resolve(Path);
	TestTrue(TEXT("the change is kept"), Asset != nullptr && Asset->Value == 9 && Asset->GetPackage()->IsDirty());
	const FLevelToolResult Forced = RunTool(*this, &RunReloadAssets, FString::Printf(TEXT(R"({"assets": ["%s"], "force": true})"), *Path),
		TEXT("force"));
	TestTrue(TEXT("force reloads"), Forced.bSuccess && Strings(Forced, TEXT("discarded_changes")).Num() == 1);
	Asset = Resolve(Path);
	TestTrue(TEXT("the change is discarded"), Asset != nullptr && Asset->Value == 7 && !Asset->GetPackage()->IsDirty());

	// Not in memory: nothing to reload.
	if (UPackage* Package = FindPackage(nullptr, *Assets.PackageName(TEXT("DA_Reload"))))
	{
		UPackageTools::UnloadPackages({Package});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	const FLevelToolResult Unloaded = RunTool(*this, &RunReloadAssets, FString::Printf(TEXT(R"({"assets": ["%s"]})"), *Path), TEXT("unloaded"));
	TestTrue(TEXT("not loaded"), Unloaded.bSuccess && Strings(Unloaded, TEXT("not_loaded")).Num() == 1);

	// The open map is reopened with open_level, not reloaded.
	AssetDocumentLevelTests::FScopedTestMaps Maps;
	UWorld* World = Maps.NewMap(TEXT("Room"), false);
	if (TestNotNull(TEXT("map"), World) && TestTrue(TEXT("map saved"), AssetDocumentLevelTests::FScopedTestMaps::SaveWorld(*World)))
	{
		const FLevelToolResult Map = RunTool(*this, &RunReloadAssets, FString::Printf(TEXT(R"({"assets": ["%s"]})"),
			*Maps.PackageName(TEXT("Room"))), TEXT("open map"));
		TestTrue(TEXT("Assets.OpenMap"), !Map.bSuccess && HasCode(Map, TEXT("Assets.OpenMap")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAssetOpsAssetEditorTest, "UEAssetDoc.AssetOps.AssetEditor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAssetOpsAssetEditorTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAssetOpsToolTests;
	using namespace AssetDocumentAssetOpsTestSupport;
	FScopedTestAssets Assets;
	UAssetDocumentAssetOpsTestRefAsset* Asset = Assets.NewRefAsset(TEXT("DA_Edited"));
	if (!TestTrue(TEXT("fixture saved"), FScopedTestAssets::Save({Asset})))
	{
		return false;
	}
	const FString Path = Asset->GetPathName();
	const FLevelToolResult Opened = RunTool(*this, &RunAssetEditor, FString::Printf(TEXT(R"({"action": "open", "assets": ["%s"]})"), *Path),
		TEXT("open"));
	if (!Opened.bSuccess)
	{
		// A session without editor windows: the refusal names it.
		TestTrue(TEXT("Assets.EditorNotOpened"), HasCode(Opened, TEXT("Assets.EditorNotOpened")));
		return true;
	}
	TestEqual(TEXT("opened"), Strings(Opened, TEXT("opened")), TArray<FString>({Path}));
	TestTrue(TEXT("listed as open"), Strings(RunTool(*this, &RunAssetEditor, TEXT(R"({"action": "list"})"), TEXT("list")), TEXT("open")).Contains(Path));
	const FLevelToolResult Closed = RunTool(*this, &RunAssetEditor, FString::Printf(TEXT(R"({"action": "close", "assets": ["%s"]})"), *Path),
		TEXT("close"));
	TestEqual(TEXT("closed"), Strings(Closed, TEXT("closed")), TArray<FString>({Path}));
	TestFalse(TEXT("no longer open"), Strings(Closed, TEXT("open")).Contains(Path));
	const FLevelToolResult Again = RunTool(*this, &RunAssetEditor, FString::Printf(TEXT(R"({"action": "close", "assets": ["%s"]})"), *Path),
		TEXT("close again"));
	TestTrue(TEXT("closing a closed editor is not an error"), Again.bSuccess && Strings(Again, TEXT("not_open")).Num() == 1);
	TestEqual(TEXT("an unknown action is invalid"), RunTool(*this, &RunAssetEditor, TEXT(R"({"action": "focus"})"), TEXT("bad")).HttpStatus, 400);
	return true;
}

// Without an audio device (-NoSound) the MetaSound editor crashes the engine while it builds its node palette
// (KI-16, Harmonix KeyzoneSettings assert): asset_editor open refuses it instead.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAssetOpsAssetEditorMetaSoundNoAudioTest, "UEAssetDoc.AssetOps.AssetEditorMetaSoundNoAudio",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAssetOpsAssetEditorMetaSoundNoAudioTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAssetOpsToolTests;
	if (FApp::CanEverRenderAudio())
	{
		AddInfo(TEXT("this editor has an audio device: the MetaSound editor opens normally (not exercised)"));
		return true;
	}
	const FString Path = TEXT("/Metasound/DefaultAssets/Sources/MSS_PlayRandom_Oneshot.MSS_PlayRandom_Oneshot");
	const FLevelToolResult Opened = RunTool(*this, &RunAssetEditor, FString::Printf(TEXT(R"({"action": "open", "assets": ["%s"]})"), *Path),
		TEXT("open MetaSound"));
	TestFalse(TEXT("refused"), Opened.bSuccess);
	TestTrue(TEXT("Assets.EditorNeedsAudio"), HasCode(Opened, TEXT("Assets.EditorNeedsAudio")));
	TestFalse(TEXT("no MetaSound editor opened"),
		Strings(RunTool(*this, &RunAssetEditor, TEXT(R"({"action": "list"})"), TEXT("list")), TEXT("open")).Contains(Path));
	return true;
}

// asset_editor and an editor holding unapplied edits: the Material Editor edits a preview copy and asks
// before it closes while the copy has edits not yet applied. close reports the truth (Assets.EditorNotClosed, naming the
// ways out) instead of "closed"; apply_editor presses the editor's Apply; close with discard_unapplied closes without
// the edits.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAssetOpsAssetEditorPendingEditsTest, "UEAssetDoc.AssetOps.AssetEditorPendingEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAssetOpsAssetEditorPendingEditsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAssetOpsToolTests;
	using namespace AssetDocumentAssetOpsTestSupport;
	FScopedTestAssets Assets;
	UPackage* Package = CreatePackage(*Assets.PackageName(TEXT("M_Pending")));
	UMaterial* Material = NewObject<UMaterial>(Package, TEXT("M_Pending"), RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Material);
	Package->MarkPackageDirty();
	if (!TestTrue(TEXT("fixture saved"), FScopedTestAssets::Save({Material})))
	{
		return false;
	}
	const FString Path = Material->GetPathName();
	const FString Assets1 = FString::Printf(TEXT(R"("assets": ["%s"])"), *Path);
	UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	auto MaterialEditor = [Editors, Material]() -> IMaterialEditor*
	{
		IAssetEditorInstance* Instance = Editors->FindEditorForAsset(Material, false);
		return Instance != nullptr && Instance->GetEditorName() == TEXT("MaterialEditor")
			? static_cast<IMaterialEditor*>(static_cast<FAssetEditorToolkit*>(Instance)) : nullptr;
	};
	ON_SCOPE_EXIT
	{
		if (Editors->FindEditorForAsset(Material, false) != nullptr)
		{
			RunTool(*this, &RunAssetEditor, FString::Printf(TEXT(R"({"action": "close", "discard_unapplied": true, %s})"), *Assets1), TEXT("cleanup"));
		}
	};
	const FLevelToolResult Opened = RunTool(*this, &RunAssetEditor, FString::Printf(TEXT(R"({"action": "open", %s})"), *Assets1), TEXT("open"));
	if (!Opened.bSuccess || MaterialEditor() == nullptr)
	{
		AddInfo(TEXT("No Material Editor in this session; the pending-edit cases were not exercised."));
		return true;
	}
	{
		// The list names the user's assets only, not an editor's working copy (PreviewMaterial_N).
		const FLevelToolResult Listed = RunTool(*this, &RunAssetEditor, TEXT(R"({"action": "list"})"), TEXT("list"));
		const TArray<FString> Open = Strings(Listed, TEXT("open"));
		TestTrue(TEXT("the material is listed"), Open.Contains(Path));
		TestFalse(*FString::Printf(TEXT("no editor working copy is listed (%s)"), *FString::Join(Open, TEXT(", "))),
			Open.ContainsByPredicate([](const FString& Listed) { return Listed.StartsWith(TEXT("/Engine/Transient")) || Listed.Contains(TEXT("PreviewMaterial")); }));
	}
	TestEqual(TEXT("apply_editor with nothing pending is refused"),
		RunTool(*this, &RunAssetEditor, FString::Printf(TEXT(R"({"action": "apply_editor", %s})"), *Assets1), TEXT("apply_editor (nothing)")).Diagnostics.ContainsByPredicate(
			[](const FAssetDocumentDiagnostic& D) { return D.Code == TEXT("Assets.NothingToApply"); }), true);

	// 1. An unapplied edit: close does not report "closed"; the editor stays open and the refusal names the ways out.
	MaterialEditor()->MarkMaterialDirty();
	const FLevelToolResult Refused = RunTool(*this, &RunAssetEditor, FString::Printf(TEXT(R"({"action": "close", %s})"), *Assets1), TEXT("close (pending)"));
	TestFalse(TEXT("close with an unapplied edit fails"), Refused.bSuccess);
	TestTrue(TEXT("Assets.EditorNotClosed"), HasCode(Refused, TEXT("Assets.EditorNotClosed")));
	TestTrue(TEXT("the refusal names apply_editor and discard_unapplied"), Refused.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& D)
		{
			return D.Message.Contains(TEXT("apply_editor")) && D.Message.Contains(TEXT("discard_unapplied"));
		}));
	TestFalse(TEXT("the refusal names the material, not the editor's preview copy"), Refused.Diagnostics.ContainsByPredicate(
		[](const FAssetDocumentDiagnostic& D) { return D.Message.Contains(TEXT("PreviewMaterial")) || D.Path.Contains(TEXT("PreviewMaterial")); }));
	TestTrue(TEXT("not reported as closed"), Strings(Refused, TEXT("closed")).IsEmpty());
	TestTrue(TEXT("the editor is still open"), MaterialEditor() != nullptr && Strings(Refused, TEXT("open")).Contains(Path));

	// 2. apply_editor: the editor's Apply writes the edit to the material, whose package then has unsaved changes; the
	//    editor then closes.
	const FLevelToolResult Applied = RunTool(*this, &RunAssetEditor, FString::Printf(TEXT(R"({"action": "apply_editor", %s})"), *Assets1), TEXT("apply_editor"));
	TestEqual(TEXT("applied"), Strings(Applied, TEXT("applied")), TArray<FString>({Path}));
	TestTrue(TEXT("the material has unsaved changes"), Material->GetOutermost()->IsDirty());
	const FLevelToolResult Closed = RunTool(*this, &RunAssetEditor, FString::Printf(TEXT(R"({"action": "close", %s})"), *Assets1), TEXT("close (applied)"));
	TestEqual(TEXT("closed after apply_editor"), Strings(Closed, TEXT("closed")), TArray<FString>({Path}));
	TestNull(TEXT("no editor left open"), Editors->FindEditorForAsset(Material, false));
	TestTrue(TEXT("saved"), FScopedTestAssets::Save({Material}));

	// 3. close with discard_unapplied: the editor closes without the edit, and the material stays saved.
	RunTool(*this, &RunAssetEditor, FString::Printf(TEXT(R"({"action": "open", %s})"), *Assets1), TEXT("open again"));
	if (MaterialEditor() != nullptr)
	{
		MaterialEditor()->MarkMaterialDirty();
		const FLevelToolResult Discarded = RunTool(*this, &RunAssetEditor,
			FString::Printf(TEXT(R"({"action": "close", "discard_unapplied": true, %s})"), *Assets1), TEXT("close (discard)"));
		TestEqual(TEXT("closed"), Strings(Discarded, TEXT("closed")), TArray<FString>({Path}));
		TestEqual(TEXT("discarded_unapplied"), Strings(Discarded, TEXT("discarded_unapplied")), TArray<FString>({Path}));
		TestNull(TEXT("no editor left open"), Editors->FindEditorForAsset(Material, false));
		TestFalse(TEXT("the material stays saved"), Material->GetOutermost()->IsDirty());
	}
	TestEqual(TEXT("discard_unapplied goes with close"),
		RunTool(*this, &RunAssetEditor, FString::Printf(TEXT(R"({"action": "open", "discard_unapplied": true, %s})"), *Assets1), TEXT("bad")).HttpStatus, 400);
	TestEqual(TEXT("apply_editor on a closed editor: not_open"),
		Strings(RunTool(*this, &RunAssetEditor, FString::Printf(TEXT(R"({"action": "apply_editor", %s})"), *Assets1), TEXT("apply_editor (closed)")), TEXT("not_open")).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAssetOpsFreshTextureReloadDeleteTest, "UEAssetDoc.AssetOps.FreshTextureReloadDelete",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAssetOpsFreshTextureReloadDeleteTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAssetOpsToolTests;
	using namespace AssetDocumentAssetOpsTestSupport;
	FScopedTestAssets Assets;
	// New textures as the Content Browser's "Texture" creates them, saved, then edited (Details panel) with the
	// package's unsaved flag cleared: their data is building again, asynchronously, when the tools reload or delete them.
	const auto NewBuildingTexture = [this, &Assets](const TCHAR* Name) -> UTexture2D*
	{
		UTexture2D* Texture = Cast<UTexture2D>(IAssetTools::Get().CreateAsset(Name, FPackageName::GetLongPackagePath(Assets.PackageName(Name)),
			UTexture2D::StaticClass(), GetMutableDefault<UTexture2DFactoryNew>()));
		if (Texture == nullptr)
		{
			return nullptr;
		}
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Texture->GetPackage(), Texture, *Assets.File(Name), SaveArgs))
		{
			return nullptr;
		}
		Texture->PreEditChange(nullptr);
		Texture->CompressionSettings = TC_Grayscale;
		Texture->PostEditChange();
		Texture->GetPackage()->SetDirtyFlag(false);
		AddInfo(FString::Printf(TEXT("%s reads as compiling: %d"), Name, Texture->IsCompiling()));
		return Texture;
	};
	UTexture2D* Reloaded = NewBuildingTexture(TEXT("T_FreshReload"));
	UTexture2D* Deleted = NewBuildingTexture(TEXT("T_FreshDelete"));
	if (!TestTrue(TEXT("textures created and saved"), Reloaded != nullptr && Deleted != nullptr))
	{
		return false;
	}
	FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({Assets.File(TEXT("T_FreshReload")), Assets.File(TEXT("T_FreshDelete"))});

	const FLevelToolResult Reload = RunTool(*this, &RunReloadAssets, FString::Printf(TEXT(R"({"assets": ["%s"]})"),
		*Assets.ObjectPath(TEXT("T_FreshReload"))), TEXT("reload"));
	TestTrue(TEXT("the new texture reloads"), Reload.bSuccess
		&& Strings(Reload, TEXT("reloaded")).Contains(Assets.PackageName(TEXT("T_FreshReload"))));
	TestNotNull(TEXT("the reloaded texture is loaded"), FindObject<UTexture2D>(nullptr, *Assets.ObjectPath(TEXT("T_FreshReload"))));

	const TArray<FAssetDocumentDeleteOutcome> Outcomes = AssetDocumentAssetDelete::DeleteAssetsAsEditor({Deleted});
	TestTrue(TEXT("the new texture is deleted"), Outcomes.Num() == 1 && Outcomes[0].bDeleted);
	if (Outcomes.Num() == 1 && !Outcomes[0].bDeleted)
	{
		AddInfo(AssetDocumentAssetDelete::DescribeDeleteBlockers(Outcomes[0].Blockers));
	}
	TestFalse(TEXT("its file is gone"), IFileManager::Get().FileExists(*Assets.File(TEXT("T_FreshDelete"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAssetOpsExportAssetsTest, "UEAssetDoc.AssetOps.ExportAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAssetOpsExportAssetsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAssetOpsToolTests;
	using namespace AssetDocumentImportTests;
	FScopedImportTest Scope;
	const FString Wav = Scope.Source(TEXT("SFX_Beep.wav"));
	const FString Png = Scope.Source(TEXT("T_Ramp.png"));
	if (!TestTrue(TEXT("sources written"), WriteWav(Wav, 22050, 0.25) && WriteImage(Png, 16, 8)))
	{
		return false;
	}
	const FLevelToolResult Imported = RunImport(*this, FString::Printf(TEXT(R"({"files": [%s, %s], "destination": "%s"})"), *Quote(Wav),
		*Quote(Png), *Scope.Folder(TEXT("Src"))), TEXT("import"));
	const TSharedPtr<FJsonObject> Sound = ResultAsset(Imported, TEXT("SoundWave"));
	const TSharedPtr<FJsonObject> Texture = ResultAsset(Imported, TEXT("Texture2D"));
	if (!TestTrue(TEXT("imported"), Sound.IsValid() && Texture.IsValid()))
	{
		return false;
	}
	const FString Folder = TEXT("Saved/AssetDocumentServerTests/Export/") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
	const FString FullFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Folder);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*FullFolder, false, true);
	};
	const FString Request = FString::Printf(TEXT(R"({"assets": ["%s", "%s"], "folder": "%s"})"), *AssetPath(Sound), *AssetPath(Texture), *Folder);
	const FLevelToolResult Exported = RunTool(*this, &RunExportAssets, Request, TEXT("export"));
	TestTrue(TEXT("exported"), Exported.bSuccess);
	TArray<uint8> Bytes;
	TestTrue(TEXT("a RIFF/WAVE file"), FFileHelper::LoadFileToArray(Bytes, *(FullFolder / TEXT("SFX_Beep.wav"))) && Bytes.Num() > 44
		&& FMemory::Memcmp(Bytes.GetData(), "RIFF", 4) == 0 && FMemory::Memcmp(Bytes.GetData() + 8, "WAVE", 4) == 0);
	const uint8 PngSignature[4] = {0x89, 'P', 'N', 'G'};
	TestTrue(TEXT("a PNG file"), FFileHelper::LoadFileToArray(Bytes, *(FullFolder / TEXT("T_Ramp.png"))) && Bytes.Num() > 8
		&& FMemory::Memcmp(Bytes.GetData(), PngSignature, 4) == 0);

	const FLevelToolResult Existing = RunTool(*this, &RunExportAssets, Request, TEXT("existing"));
	TestTrue(TEXT("existing files are not overwritten"), !Existing.bSuccess && HasCode(Existing, TEXT("Assets.FileExists")));
	const FLevelToolResult Overwritten = RunTool(*this, &RunExportAssets, FString::Printf(TEXT(R"({"assets": ["%s"], "folder": "%s",
		"overwrite": true})"), *AssetPath(Sound), *Folder), TEXT("overwrite"));
	TestTrue(TEXT("overwrite"), Overwritten.bSuccess);

	TestEqual(TEXT("a folder outside the project is refused"), RunTool(*this, &RunExportAssets, FString::Printf(
		TEXT(R"({"assets": ["%s"], "folder": "../Outside"})"), *AssetPath(Sound)), TEXT("outside")).HttpStatus, 400);
	TestEqual(TEXT("a '..' segment is refused"), RunTool(*this, &RunExportAssets, FString::Printf(
		TEXT(R"({"assets": ["%s"], "folder": "%s/../Up"})"), *AssetPath(Sound), *Folder), TEXT("parent segment")).HttpStatus, 400);
	{
		// A folder in the project that links out of it: the files would land outside.
		using namespace AssetDocumentPathsTestSupport;
		const FString Outside = MakeOutsideDirectory(TEXT("Export"));
		const FString Link = FullFolder / TEXT("out");
		ON_SCOPE_EXIT
		{
			RemoveDirectoryLink(Link);
			IFileManager::Get().DeleteDirectory(*Outside, false, true);
		};
		if (TestTrue(TEXT("a linked export folder"), MakeDirectoryLink(Link, Outside)))
		{
			const FLevelToolResult Linked = RunTool(*this, &RunExportAssets, FString::Printf(TEXT(R"({"assets": ["%s"], "folder": "%s/out"})"),
				*AssetPath(Sound), *Folder), TEXT("linked out"));
			TestEqual(TEXT("a folder that links out of the project is refused"), Linked.HttpStatus, 400);
			TestFalse(TEXT("nothing is written outside the project"), IFileManager::Get().FileExists(*(Outside / TEXT("SFX_Beep.wav"))));
			const FLevelToolResult Below = RunTool(*this, &RunExportAssets, FString::Printf(TEXT(R"({"assets": ["%s"], "folder": "%s/out/Audio"})"),
				*AssetPath(Sound), *Folder), TEXT("below the link"));
			TestEqual(TEXT("a new folder below that link is refused"), Below.HttpStatus, 400);
			TestFalse(TEXT("no folder is made outside the project"), IFileManager::Get().DirectoryExists(*(Outside / TEXT("Audio"))));
		}
	}
	const FLevelToolResult NoFormat = RunTool(*this, &RunExportAssets, FString::Printf(TEXT(R"({"assets": ["%s"], "folder": "%s",
		"format": "fbx"})"), *AssetPath(Sound), *Folder), TEXT("no exporter"));
	TestTrue(TEXT("Assets.NoExporter"), !NoFormat.bSuccess && HasCode(NoFormat, TEXT("Assets.NoExporter")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAssetOpsExportAssetsLinkedFileTest, "UEAssetDoc.AssetOps.ExportAssetsLinkedFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAssetOpsExportAssetsLinkedFileTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAssetOpsToolTests;
	using namespace AssetDocumentImportTests;
	using namespace AssetDocumentPathsTestSupport;
	FScopedImportTest Scope;
	const FString Png = Scope.Source(TEXT("T_Linked.png"));
	if (!TestTrue(TEXT("source written"), WriteImage(Png, 16, 8)))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Texture = ResultAsset(RunImport(*this, FString::Printf(TEXT(R"({"files": [%s], "destination": "%s"})"),
		*Quote(Png), *Scope.Folder(TEXT("Src"))), TEXT("import")), TEXT("Texture2D"));
	if (!TestTrue(TEXT("imported"), Texture.IsValid()))
	{
		return false;
	}
	const FString Folder = TEXT("Saved/AssetDocumentServerTests/ExportLinked/") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
	const FString FullFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Folder);
	const FString Outside = MakeOutsideDirectory(TEXT("ExportLeaf"));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*FullFolder, false, true);
		IFileManager::Get().DeleteDirectory(*Outside, false, true);
	};
	const FString OutsideText = TEXT("a file outside the project");
	const auto OutsideUnchanged = [this, &OutsideText](const FString& File, const TCHAR* What)
	{
		FString Text;
		TestTrue(What, FFileHelper::LoadFileToString(Text, *File) && Text == OutsideText);
	};

	// The file named for the asset links out of the project.
	const FString Victim = Outside / TEXT("victim.png");
	const FString Leaf = FullFolder / TEXT("Textures") / TEXT("T_Linked.png");
	TestTrue(TEXT("an outside file"), FFileHelper::SaveStringToFile(OutsideText, *Victim));
	const FString TextureRequest = FString::Printf(TEXT(R"({"assets": ["%s"], "folder": "%s/Textures", "overwrite": true})"),
		*AssetPath(Texture), *Folder);
	if (!MakeFileLink(Leaf, Victim))
	{
		AddInfo(TEXT("this system does not let the test make a file link; the linked-file checks are skipped"));
		return true;
	}
	const FLevelToolResult Linked = RunTool(*this, &RunExportAssets, TextureRequest, TEXT("leaf links out"));
	TestTrue(TEXT("a file that links out of the project is refused, also with overwrite"), !Linked.bSuccess
		&& HasCode(Linked, TEXT("Assets.FileOutsideProject")));
	OutsideUnchanged(Victim, TEXT("the outside file is unchanged"));
	RemoveFileLink(Leaf);
	const FLevelToolResult Plain = RunTool(*this, &RunExportAssets, TextureRequest, TEXT("no link"));
	TestTrue(TEXT("without the link the texture exports"), Plain.bSuccess && IFileManager::Get().FileSize(*Leaf) > 8);
	OutsideUnchanged(Victim, TEXT("the outside file is still unchanged"));

	// A file the exporter writes beside the main one: the static mesh OBJ exporter writes <Name>_UV1.obj (the
	// lightmap UVs) next to <Name>.obj.
	const FString Cube = TEXT("/Engine/BasicShapes/Cube.Cube");
	const FString MeshRequest = FString::Printf(TEXT(R"({"assets": ["%s"], "folder": "%s/Meshes", "format": "obj", "overwrite": true})"),
		*Cube, *Folder);
	const FLevelToolResult Probe = RunTool(*this, &RunExportAssets, MeshRequest, TEXT("mesh"));
	const FString Beside = FullFolder / TEXT("Meshes") / TEXT("Cube_UV1.obj");
	if (!TestTrue(TEXT("the cube exports to OBJ"), Probe.bSuccess) || !IFileManager::Get().FileExists(*Beside))
	{
		AddInfo(TEXT("the OBJ exporter wrote no lightmap-UV file; the check of files beside the main one is skipped"));
		return true;
	}
	bool bListed = false;
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (Probe.Payload.IsValid() && Probe.Payload->TryGetArrayField(TEXT("exported"), Rows) && Rows->Num() == 1)
	{
		const TArray<TSharedPtr<FJsonValue>>* Written = nullptr;
		if ((*Rows)[0]->AsObject()->TryGetArrayField(TEXT("files"), Written))
		{
			bListed = Written->ContainsByPredicate([](const TSharedPtr<FJsonValue>& Value) { return Value->AsString().EndsWith(TEXT("/Cube_UV1.obj")); });
		}
	}
	TestTrue(TEXT("the result lists the file written beside the main one"), bListed);
	IFileManager::Get().Delete(*Beside);
	const FString BesideVictim = Outside / TEXT("victim_UV1.obj");
	TestTrue(TEXT("another outside file"), FFileHelper::SaveStringToFile(OutsideText, *BesideVictim));
	if (TestTrue(TEXT("a file link beside the main file"), MakeFileLink(Beside, BesideVictim)))
	{
		IFileManager::Get().Delete(*(FullFolder / TEXT("Meshes") / TEXT("Cube.obj")));
		const FLevelToolResult BesideLinked = RunTool(*this, &RunExportAssets, MeshRequest, TEXT("beside links out"));
		TestTrue(TEXT("an export whose extra file links out of the project is refused"), !BesideLinked.bSuccess
			&& HasCode(BesideLinked, TEXT("Assets.FileOutsideProject")));
		OutsideUnchanged(BesideVictim, TEXT("the outside file the extra file links to is unchanged"));
		TestFalse(TEXT("nothing of that export is written"), IFileManager::Get().FileExists(*(FullFolder / TEXT("Meshes") / TEXT("Cube.obj"))));
		RemoveFileLink(Beside);
	}
	TArray<FString> Left;
	IFileManager::Get().FindFilesRecursive(Left, *FullFolder, TEXT("*"), false, true);
	TestFalse(TEXT("no export folder is left behind"), Left.ContainsByPredicate([](const FString& Directory)
	{
		return FPaths::GetCleanFilename(Directory).StartsWith(TEXT(".export-"));
	}));
	return true;
}
#endif
