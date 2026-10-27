// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentModule.h"
#include "AssetDocumentPendingNotices.h"
#include "AssetDocumentService.h"
#include "AssetDocumentSettings.h"
#include "AssetDocumentSidecarSync.h"
#include "AssetDocumentSidecarSyncRunner.h"
#include "AssetDocumentSidecarSyncState.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace SidecarSyncTriggerTestsLocal
{
const TCHAR* const Folder = TEXT("AssetDocumentSidecarSyncTests");

// Turns mirroring on (or off) for the test, without the background materialize that turning it on in the editor
// starts.
struct FScopedMirroring
{
	explicit FScopedMirroring(bool bOn)
		: bWas(GetDefault<UAssetDocumentSettings>()->bMirrorSidecars)
	{
		GetMutableDefault<UAssetDocumentSettings>()->bMirrorSidecars = bOn;
	}
	~FScopedMirroring()
	{
		FAssetDocumentModule::Get().GetService().GetSidecarSyncRunner().DrainForTest();
		GetMutableDefault<UAssetDocumentSettings>()->bMirrorSidecars = bWas;
	}
	bool bWas;
};

struct FTriggerAsset
{
	FString Name;
	FString Target;
	FString ObjectPath;
	FString SidecarPath;
	FString PackageFile;

	explicit FTriggerAsset(const TCHAR* Stem)
		: Name(FString::Printf(TEXT("%s_%s"), Stem, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		Target = FString::Printf(TEXT("/Game/%s/%s"), Folder, *Name);
		ObjectPath = Target + TEXT(".") + Name;
		SidecarPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), Folder, Name + TEXT(".assetdoc.json")));
		FPaths::NormalizeFilename(SidecarPath);
		PackageFile = FPackageName::LongPackageNameToFilename(Target, FPackageName::GetAssetPackageExtension());
	}

	~FTriggerAsset()
	{
		IFileManager::Get().Delete(*SidecarPath, false, true);
		IFileManager::Get().Delete(*(SidecarPath.LeftChop(FCString::Strlen(TEXT(".assetdoc.json"))) + TEXT(".assetdoc.asset.json")), false, true);
		IFileManager::Get().Delete(*PackageFile, false, true);
		FAssetDocumentPendingNotices::Resolve(FAssetDocumentSidecarSync::ConflictNotice, SidecarPath);
		FAssetDocumentPendingNotices::Resolve(FAssetDocumentSidecarSync::FailedNotice, SidecarPath);
		FAssetDocumentPendingNotices::Resolve(FAssetDocumentService::WatcherApplyFailedNotice, SidecarPath);
	}

	FString Document(const TCHAR* TestString) const
	{
		return FString::Printf(
			TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",\"Class\":\"/Script/AssetDocumentTests.TestDataAsset\",")
			TEXT("\"Action\":\"CreateOrUpdate\",\"Properties\":{\"TestString\":\"%s\"}}"),
			*Target, TestString);
	}

	bool Write(const FString& Text) const
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(SidecarPath), true);
		return FFileHelper::SaveStringToFile(Text, *SidecarPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	FString Read() const
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *SidecarPath);
		return Text;
	}

	FAssetDocumentResult Apply(bool bWatcher = false, bool bBypass = false) const
	{
		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = SidecarPath;
		Request.bTriggeredByWatcher = bWatcher;
		Request.bBypassSidecarSync = bBypass;
		return FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	}

	UObject* Load() const
	{
		return StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
	}

	FString Value() const
	{
		UObject* Object = Load();
		const FProperty* Property = Object ? Object->GetClass()->FindPropertyByName(TEXT("TestString")) : nullptr;
		FString Text;
		if (Property)
		{
			Property->ExportTextItem_Direct(Text, Property->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
		}
		return Text;
	}

	bool SetValue(const TCHAR* Value, bool bSave) const
	{
		UObject* Object = Load();
		FProperty* Property = Object ? Object->GetClass()->FindPropertyByName(TEXT("TestString")) : nullptr;
		if (!Property)
		{
			return false;
		}
		Property->ImportText_Direct(Value, Property->ContainerPtrToValuePtr<void>(Object), Object, PPF_None);
		Object->MarkPackageDirty();
		if (!bSave)
		{
			return true;
		}
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Object->GetOutermost(), Object, *PackageFile, Args);
	}

	bool EditSidecarString(const TCHAR* Value) const
	{
		TSharedPtr<FJsonObject> Document;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Read()), Document) || !Document.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		const TSharedPtr<FJsonObject> Edited = Document->TryGetObjectField(TEXT("Properties"), Properties) ? *Properties : MakeShared<FJsonObject>();
		Edited->SetStringField(TEXT("TestString"), Value);
		Document->SetObjectField(TEXT("Properties"), Edited);
		FString Text;
		FJsonSerializer::Serialize(Document.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
		return Write(Text);
	}
};

FAssetDocumentSidecarSyncRunner& Runner()
{
	return FAssetDocumentModule::Get().GetService().GetSidecarSyncRunner();
}

FString SyncState(const FAssetDocumentResult& Result)
{
	const TSharedPtr<FJsonObject>* Sync = nullptr;
	FString State;
	if (Result.Payload.IsValid() && Result.Payload->TryGetObjectField(TEXT("sidecar_sync"), Sync))
	{
		(*Sync)->TryGetStringField(TEXT("state"), State);
	}
	return State;
}

// An asset created without the pass, then mirrored, with nothing queued.
bool CreateMirrored(FAutomationTestBase& Test, const FTriggerAsset& Asset, const TCHAR* Value)
{
	if (!Test.TestTrue(TEXT("sidecar written"), Asset.Write(Asset.Document(Value))))
	{
		return false;
	}
	const FAssetDocumentResult Created = Asset.Apply(false, true);
	if (!Test.TestTrue(FString::Printf(TEXT("asset created: %s"), *Created.Message), Created.IsSuccess()))
	{
		return false;
	}
	IFileManager::Get().Delete(*Asset.SidecarPath, false, true);
	Runner().DrainForTest();
	FAssetDocumentSidecarSyncRequest Request;
	Request.PackageName = Asset.Target;
	Request.Trigger = EAssetDocumentSidecarSyncTrigger::Materialize;
	const FAssetDocumentSidecarSyncOutcome Outcome = FAssetDocumentModule::Get().GetService().GetSidecarSync().Sync(Request);
	return Test.TestEqual(FString::Printf(TEXT("mirror written (%s)"), *Outcome.Message), Outcome.State, FString(TEXT("wrote")));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncTriggerSaveTest,
	"UEAssetDoc.SidecarSync.Triggers.SaveQueuesOnePass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncTriggerSaveTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncTriggerTestsLocal;
	const FScopedMirroring Mirroring(true);
	const FTriggerAsset Asset(TEXT("DA_TriggerSave"));
	if (!CreateMirrored(*this, Asset, TEXT("first")))
	{
		return false;
	}
	TestTrue(TEXT("first save"), Asset.SetValue(TEXT("saved_once"), true));
	TestTrue(TEXT("a save queues a pass"), Runner().IsQueued(Asset.Target));
	TestTrue(TEXT("second save"), Asset.SetValue(TEXT("saved_twice"), true));
	TestEqual(TEXT("saves before the pass runs share one pass"), Runner().GetQueuedCount(), 1);
	TestTrue(TEXT("the save itself did not write the sidecar"), !Asset.Read().Contains(TEXT("saved_")));
	TestEqual(TEXT("one pass runs"), Runner().DrainForTest(), 1);
	TestTrue(TEXT("the mirror has the saved value"), Asset.Read().Contains(TEXT("saved_twice")));

	TestTrue(TEXT("an unsaved edit after a save"), Asset.SetValue(TEXT("saved_then_dirty"), true) && Asset.SetValue(TEXT("dirty"), false));
	const FString Before = Asset.Read();
	Runner().DrainForTest();
	TestEqual(TEXT("a package dirty again when its pass runs is not mirrored"), Asset.Read(), Before);
	if (UObject* Object = Asset.Load())
	{
		Object->GetOutermost()->SetDirtyFlag(false);
	}

	FString Why;
	TestFalse(TEXT("a save to another file is not a save of the package"),
		FAssetDocumentSidecarSyncRunner::ShouldSyncSave(FPaths::ProjectSavedDir() / TEXT("Copy.uasset"), Asset.Load()->GetOutermost(), false, false, &Why));
	TestEqual(TEXT("reason"), Why, FString(TEXT("other_file")));
	TestFalse(TEXT("procedural saves are ignored"),
		FAssetDocumentSidecarSyncRunner::ShouldSyncSave(Asset.PackageFile, Asset.Load()->GetOutermost(), true, false));
	TestFalse(TEXT("autosaves are ignored"),
		FAssetDocumentSidecarSyncRunner::ShouldSyncSave(Asset.PackageFile, Asset.Load()->GetOutermost(), false, true));
	TestTrue(TEXT("a save of the package's own file counts"),
		FAssetDocumentSidecarSyncRunner::ShouldSyncSave(Asset.PackageFile, Asset.Load()->GetOutermost(), false, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncTriggerApplyTest,
	"UEAssetDoc.SidecarSync.Triggers.ApplyRunsThePass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncTriggerApplyTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncTriggerTestsLocal;
	const FScopedMirroring Mirroring(true);
	const FTriggerAsset Asset(TEXT("DA_TriggerApply"));
	if (!CreateMirrored(*this, Asset, TEXT("first")) || !TestTrue(TEXT("edit"), Asset.EditSidecarString(TEXT("edited"))))
	{
		return false;
	}
	const FAssetDocumentResult Applied = Asset.Apply();
	TestTrue(FString::Printf(TEXT("the explicit apply succeeds: %s"), *Applied.Message), Applied.IsSuccess());
	TestEqual(TEXT("through the sync pass"), SyncState(Applied), FString(TEXT("applied")));
	TestEqual(TEXT("the asset has the value"), Asset.Value(), FString(TEXT("edited")));
	TestTrue(TEXT("the file is the mirror again"), Asset.Read().Contains(TEXT("\"_meta\"")));
	TestFalse(TEXT("the pass's own saves queue nothing"), Runner().IsQueued(Asset.Target));

	const FAssetDocumentResult Again = Asset.Apply();
	TestTrue(TEXT("applying the mirror again succeeds"), Again.IsSuccess());
	TestEqual(TEXT("as in sync"), SyncState(Again), FString(TEXT("in_sync")));

	// Both sides changed: the apply refuses with the conflict instead of overwriting the editor's change.
	TestTrue(TEXT("asset changed and saved"), Asset.SetValue(TEXT("from_asset"), true));
	TestTrue(TEXT("file changed"), Asset.EditSidecarString(TEXT("from_file")));
	const FAssetDocumentResult Conflict = Asset.Apply();
	TestFalse(TEXT("the apply is refused"), Conflict.IsSuccess());
	TestTrue(FString::Printf(TEXT("with Sync.Conflict: %s"), *Conflict.Message), Conflict.Diagnostics.ContainsByPredicate(
		[](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("Sync.Conflict"); }));
	TestEqual(TEXT("the asset keeps its change"), Asset.Value(), FString(TEXT("from_asset")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncTriggerWatcherTest,
	"UEAssetDoc.SidecarSync.Triggers.WatcherThenExplicitApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncTriggerWatcherTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncTriggerTestsLocal;
	const FScopedMirroring Mirroring(true);
	const FTriggerAsset Asset(TEXT("DA_TriggerWatch"));
	if (!CreateMirrored(*this, Asset, TEXT("first")) || !TestTrue(TEXT("edit"), Asset.EditSidecarString(TEXT("watched"))))
	{
		return false;
	}
	const FAssetDocumentResult Watched = Asset.Apply(true);
	TestTrue(FString::Printf(TEXT("the watcher pass applies: %s"), *Watched.Message), Watched.IsSuccess());
	TestEqual(TEXT("through the sync pass"), SyncState(Watched), FString(TEXT("applied")));
	TestEqual(TEXT("the asset has the value"), Asset.Value(), FString(TEXT("watched")));
	const FAssetDocumentResult Explicit = Asset.Apply();
	TestTrue(TEXT("the explicit apply after it succeeds"), Explicit.IsSuccess());
	FString AppliedBy;
	TestTrue(TEXT("and returns the watcher's result"),
		Explicit.Payload.IsValid() && Explicit.Payload->TryGetStringField(TEXT("applied_by"), AppliedBy) && AppliedBy == TEXT("file_watcher"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncTriggerOffTest,
	"UEAssetDoc.SidecarSync.Triggers.MirroringOffUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncTriggerOffTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncTriggerTestsLocal;
	const FScopedMirroring Mirroring(false);
	const FTriggerAsset Asset(TEXT("DA_TriggerOff"));
	TestTrue(TEXT("sidecar written"), Asset.Write(Asset.Document(TEXT("off"))));
	const FAssetDocumentResult Applied = Asset.Apply();
	TestTrue(FString::Printf(TEXT("apply works as before: %s"), *Applied.Message), Applied.IsSuccess());
	TestTrue(TEXT("without the sync pass"), SyncState(Applied).IsEmpty());
	TestFalse(TEXT("and the file gets no _meta"), Asset.Read().Contains(TEXT("\"_meta\"")));
	TestTrue(TEXT("save"), Asset.SetValue(TEXT("saved"), true));
	TestFalse(TEXT("a save queues nothing"), Runner().IsQueued(Asset.Target));
	return true;
}

#endif
