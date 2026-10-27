// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentHttpServer.h"
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

namespace SidecarSyncRunnerTestsLocal
{
// Mirroring settings for the test, restored after it (the background run the editor starts when they change in the
// Project Settings is not started).
struct FScopedMirrorSettings
{
	FScopedMirrorSettings()
	{
		const UAssetDocumentSettings* Settings = GetDefault<UAssetDocumentSettings>();
		bWas = Settings->bMirrorSidecars;
		Paths = Settings->MirrorExcludedPaths;
		Classes = Settings->MirrorExcludedClasses;
		MaxKB = Settings->MaxMirrorFileSizeKB;
		GetMutableDefault<UAssetDocumentSettings>()->bMirrorSidecars = true;
	}
	~FScopedMirrorSettings()
	{
		FAssetDocumentModule::Get().GetService().GetSidecarSyncRunner().DrainForTest();
		UAssetDocumentSettings* Settings = GetMutableDefault<UAssetDocumentSettings>();
		Settings->bMirrorSidecars = bWas;
		Settings->MirrorExcludedPaths = Paths;
		Settings->MirrorExcludedClasses = Classes;
		Settings->MaxMirrorFileSizeKB = MaxKB;
	}
	bool bWas = false;
	TArray<FString> Paths;
	TArray<FSoftClassPath> Classes;
	int32 MaxKB = 8192;
};

// A folder of test assets under /Game/AssetDocumentSidecarSyncTests, deleted with its files at the end.
struct FTestFolder
{
	FString Root;
	FString Directory;

	FTestFolder()
	{
		const FString Name = FString::Printf(TEXT("Run_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		Root = FString::Printf(TEXT("/Game/AssetDocumentSidecarSyncTests/%s"), *Name);
		Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentSidecarSyncTests"), Name));
		FPaths::NormalizeDirectoryName(Directory);
	}
	~FTestFolder()
	{
		FAssetDocumentSidecarSyncRunner& Runner = FAssetDocumentModule::Get().GetService().GetSidecarSyncRunner();
		Runner.Cancel();
		Runner.DrainForTest();
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.assetdoc.json"), true, false);
		for (const FString& File : Files)
		{
			FAssetDocumentPendingNotices::Resolve(FAssetDocumentSidecarSync::ConflictNotice, File);
			FAssetDocumentPendingNotices::Resolve(FAssetDocumentSidecarSync::FailedNotice, File);
		}
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	}

	FString Target(const TCHAR* Name) const { return Root / Name; }
	FString Sidecar(const TCHAR* Name) const { return Directory / (FString(Name) + TEXT(".assetdoc.json")); }
	FString PackageFile(const TCHAR* Name) const
	{
		return FPackageName::LongPackageNameToFilename(Target(Name), FPackageName::GetAssetPackageExtension());
	}
	FString ObjectPath(const TCHAR* Name) const { return Target(Name) + TEXT(".") + Name; }

	FString Read(const TCHAR* Name) const
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *Sidecar(Name));
		return Text;
	}

	// Creates a TestDataAsset (Properties is the inside of its Properties object) with no sidecar and no queued pass.
	bool Create(FAutomationTestBase& Test, const TCHAR* Name, const FString& Properties) const
	{
		const FString Document = FString::Printf(
			TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",")
			TEXT("\"Class\":\"/Script/AssetDocumentTests.TestDataAsset\",\"Action\":\"CreateOrUpdate\",\"Properties\":{%s}}"),
			*Target(Name), *Properties);
		IFileManager::Get().MakeDirectory(*Directory, true);
		FFileHelper::SaveStringToFile(Document, *Sidecar(Name), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		// With mirroring off the save of the new package queues no pass.
		UAssetDocumentSettings* Settings = GetMutableDefault<UAssetDocumentSettings>();
		const bool bMirroring = Settings->bMirrorSidecars;
		Settings->bMirrorSidecars = false;
		FAssetDocumentApplyFileRequest Apply;
		Apply.FilePath = Sidecar(Name);
		Apply.bBypassSidecarSync = true;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Apply);
		Settings->bMirrorSidecars = bMirroring;
		IFileManager::Get().Delete(*Sidecar(Name), false, true);
		return Test.TestTrue(FString::Printf(TEXT("%s created: %s"), Name, *Result.Message), Result.IsSuccess());
	}

	FString Value(const TCHAR* Name, const TCHAR* Property) const
	{
		UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath(Name));
		const FProperty* Found = Object ? Object->GetClass()->FindPropertyByName(Property) : nullptr;
		FString Text;
		if (Found)
		{
			Found->ExportTextItem_Direct(Text, Found->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
		}
		return Text;
	}

	bool SetAndSave(const TCHAR* Name, const TCHAR* Property, const TCHAR* Value) const
	{
		UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath(Name));
		FProperty* Found = Object ? Object->GetClass()->FindPropertyByName(Property) : nullptr;
		if (!Found)
		{
			return false;
		}
		Found->ImportText_Direct(Value, Found->ContainerPtrToValuePtr<void>(Object), Object, PPF_None);
		Object->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Object->GetOutermost(), Object, *PackageFile(Name), Args);
	}
};

FAssetDocumentSidecarSyncRunner& Runner()
{
	return FAssetDocumentModule::Get().GetService().GetSidecarSyncRunner();
}

FAssetDocumentService& Service()
{
	return FAssetDocumentModule::Get().GetService();
}

TSharedPtr<FJsonObject> LastRun()
{
	const TSharedPtr<FJsonObject>* Last = nullptr;
	const TSharedRef<FJsonObject> Status = Runner().GetStatusJson();
	return Status->TryGetObjectField(TEXT("last_run"), Last) ? *Last : nullptr;
}

int32 LastRunCount(const TCHAR* Bucket, const TCHAR* Key)
{
	const TSharedPtr<FJsonObject> Last = LastRun();
	const TSharedPtr<FJsonObject>* Counts = nullptr;
	int32 Count = 0;
	if (Last.IsValid() && Last->TryGetObjectField(Bucket, Counts))
	{
		(*Counts)->TryGetNumberField(Key, Count);
	}
	return Count;
}

TSharedPtr<FJsonObject> CallSync(const FString& Body, int32& OutStatus)
{
	return FAssetDocumentHttpServer::CallRoute(TEXT("POST"), TEXT("/assetdocument/sync"), Body, {}, 0, OutStatus);
}

FString FirstCode(const TSharedPtr<FJsonObject>& Result)
{
	const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
	FString Code;
	if (Result.IsValid() && Result->TryGetArrayField(TEXT("diagnostics"), Diagnostics) && !Diagnostics->IsEmpty())
	{
		(*Diagnostics)[0]->AsObject()->TryGetStringField(TEXT("code"), Code);
	}
	return Code;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncRunnerReconcileTest,
	"UEAssetDoc.SidecarSync.Runner.ReconcileFastPathAndOfflineChanges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncRunnerReconcileTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncRunnerTestsLocal;
	const FScopedMirrorSettings Settings;
	const FTestFolder Folder;
	if (!Folder.Create(*this, TEXT("DA_Same"), TEXT("\"TestString\":\"same\""))
		|| !Folder.Create(*this, TEXT("DA_FileEdit"), TEXT("\"TestString\":\"before\""))
		|| !Folder.Create(*this, TEXT("DA_AssetEdit"), TEXT("\"TestString\":\"before\"")))
	{
		return false;
	}
	TestEqual(TEXT("the first reconcile materializes the three assets"), Runner().ReconcileNowForTest(Folder.Root), 3);
	Runner().DrainForTest();
	TestEqual(TEXT("three mirrors written"), LastRunCount(TEXT("states"), TEXT("wrote")), 3);
	TestTrue(TEXT("a mirror exists"), Folder.Read(TEXT("DA_Same")).Contains(TEXT("\"same\"")));

	const int32 Extracts = FAssetDocumentSidecarSync::GetExtractCountForTest();
	const int32 FastPaths = FAssetDocumentSidecarSyncRunner::GetFastPathCountForTest();
	TestEqual(TEXT("unchanged pairs need no pass"), Runner().ReconcileNowForTest(Folder.Root), 0);
	TestEqual(TEXT("each answered by the index"), FAssetDocumentSidecarSyncRunner::GetFastPathCountForTest() - FastPaths, 3);
	TestEqual(TEXT("with no extract"), FAssetDocumentSidecarSync::GetExtractCountForTest(), Extracts);

	// Changes made while the editor was closed: a sidecar edit, and a package changed without the editor's save pass.
	FString Edited = Folder.Read(TEXT("DA_FileEdit")).Replace(TEXT("\"before\""), TEXT("\"offline_file\""));
	FFileHelper::SaveStringToFile(Edited, *Folder.Sidecar(TEXT("DA_FileEdit")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	GetMutableDefault<UAssetDocumentSettings>()->bMirrorSidecars = false;
	TestTrue(TEXT("package changed while mirroring was off"), Folder.SetAndSave(TEXT("DA_AssetEdit"), TEXT("TestString"), TEXT("offline_asset")));
	GetMutableDefault<UAssetDocumentSettings>()->bMirrorSidecars = true;
	TestFalse(TEXT("that save queued nothing"), Runner().IsQueued(Folder.Target(TEXT("DA_AssetEdit"))));

	TestEqual(TEXT("the reconcile takes the two changed pairs"), Runner().ReconcileNowForTest(Folder.Root), 2);
	Runner().DrainForTest();
	TestEqual(TEXT("the offline sidecar edit is applied"), Folder.Value(TEXT("DA_FileEdit"), TEXT("TestString")), FString(TEXT("offline_file")));
	TestTrue(TEXT("the offline package change is mirrored"), Folder.Read(TEXT("DA_AssetEdit")).Contains(TEXT("\"offline_asset\"")));
	TestEqual(TEXT("then nothing is left"), Runner().ReconcileNowForTest(Folder.Root), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncRunnerScopeTest,
	"UEAssetDoc.SidecarSync.Runner.MaterializeSkipsAndScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncRunnerScopeTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncRunnerTestsLocal;
	const FScopedMirrorSettings Settings;
	const FTestFolder Folder;
	const FString Long = FString::ChrN(3000, TEXT('x'));
	if (!Folder.Create(*this, TEXT("DA_Normal"), TEXT("\"TestString\":\"normal\""))
		|| !Folder.Create(*this, TEXT("DA_Empty"), TEXT(""))
		|| !Folder.Create(*this, TEXT("DA_Large"), FString::Printf(TEXT("\"TestString\":\"%s\""), *Long)))
	{
		return false;
	}
	UAssetDocumentSettings* Mutable = GetMutableDefault<UAssetDocumentSettings>();
	Mutable->MaxMirrorFileSizeKB = 1;
	TestEqual(TEXT("three assets without sidecars"), Runner().ReconcileNowForTest(Folder.Root), 3);
	Runner().DrainForTest();
	TestTrue(TEXT("the normal asset is mirrored"), IFileManager::Get().FileExists(*Folder.Sidecar(TEXT("DA_Normal"))));
	TestFalse(TEXT("an empty extract writes no file"), IFileManager::Get().FileExists(*Folder.Sidecar(TEXT("DA_Empty"))));
	TestFalse(TEXT("a mirror over the size limit is not written"), IFileManager::Get().FileExists(*Folder.Sidecar(TEXT("DA_Large"))));
	TestEqual(TEXT("skipped as empty"), LastRunCount(TEXT("skipped"), TEXT("empty")), 1);
	TestEqual(TEXT("skipped as too large"), LastRunCount(TEXT("skipped"), TEXT("too_large")), 1);
	TestEqual(TEXT("skipped assets are not retried while their package is unchanged"), Runner().ReconcileNowForTest(Folder.Root), 0);

	Mutable->MaxMirrorFileSizeKB = 8192;
	TestEqual(TEXT("materialize re-runs a skipped asset"), Runner().StartMaterialize({Folder.Target(TEXT("DA_Large"))}), 1);
	Runner().DrainForTest();
	TestTrue(TEXT("now within the limit"), IFileManager::Get().FileExists(*Folder.Sidecar(TEXT("DA_Large"))));

	FAssetDocumentSidecarSyncRequest Request;
	Request.PackageName = Folder.Target(TEXT("DA_Normal"));
	Mutable->MirrorExcludedPaths.Add(Folder.Root);
	FAssetDocumentSidecarSyncOutcome Excluded = Service().GetSidecarSync().Sync(Request);
	TestEqual(TEXT("an excluded folder is not synced"), Excluded.State, FString(TEXT("excluded")));
	TestEqual(TEXT("reason"), Excluded.Reason, FString(TEXT("excluded_path")));
	TestEqual(TEXT("nor reconciled"), Runner().ReconcileNowForTest(Folder.Root), 0);
	Mutable->MirrorExcludedPaths.Remove(Folder.Root);
	Mutable->MirrorExcludedClasses.Add(FSoftClassPath(TEXT("/Script/Engine.DataAsset")));
	Excluded = Service().GetSidecarSync().Sync(Request);
	TestEqual(TEXT("an excluded class (a parent class) is not synced"), Excluded.Reason, FString(TEXT("excluded_class")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncRunnerCancelTest,
	"UEAssetDoc.SidecarSync.Runner.CancelAndCollect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncRunnerCancelTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncRunnerTestsLocal;
	const FScopedMirrorSettings Settings;
	const FTestFolder Folder;
	if (!Folder.Create(*this, TEXT("DA_One"), TEXT("\"TestString\":\"one\""))
		|| !Folder.Create(*this, TEXT("DA_Two"), TEXT("\"TestString\":\"two\"")))
	{
		return false;
	}
	TestEqual(TEXT("two assets queued"), Runner().ReconcileNowForTest(Folder.Root), 2);
	const TSharedRef<FJsonObject> Running = Runner().GetStatusJson();
	TestEqual(TEXT("the status shows the run"), Running->GetStringField(TEXT("state")), FString(TEXT("reconciling")));
	TestEqual(TEXT("with its total"), static_cast<int32>(Running->GetNumberField(TEXT("total"))), 2);
	Runner().Cancel();
	TestFalse(TEXT("cancel stops the run"), Runner().IsRunActive());
	TestEqual(TEXT("the status is idle"), Runner().GetStatusJson()->GetStringField(TEXT("state")), FString(TEXT("idle")));
	TestTrue(TEXT("the last run says cancelled"), LastRun().IsValid() && LastRun()->GetBoolField(TEXT("cancelled")));
	TestFalse(TEXT("nothing was written"), IFileManager::Get().FileExists(*Folder.Sidecar(TEXT("DA_One"))));

	const int32 Collections = FAssetDocumentSidecarSyncRunner::GetCollectionCountForTest();
	TestEqual(TEXT("the next start resumes"), Runner().ReconcileNowForTest(Folder.Root), 2);
	Runner().DrainForTest();
	TestTrue(TEXT("both mirrored"), IFileManager::Get().FileExists(*Folder.Sidecar(TEXT("DA_One"))) && IFileManager::Get().FileExists(*Folder.Sidecar(TEXT("DA_Two"))));
	TestTrue(TEXT("the run collected garbage after its batch"), FAssetDocumentSidecarSyncRunner::GetCollectionCountForTest() > Collections);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncRunnerToolTest,
	"UEAssetDoc.SidecarSync.Runner.ToolActionsAndHealth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncRunnerToolTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncRunnerTestsLocal;
	const FScopedMirrorSettings Settings;
	const FTestFolder Folder;
	int32 Status = 0;

	GetMutableDefault<UAssetDocumentSettings>()->bMirrorSidecars = false;
	TSharedPtr<FJsonObject> Off = CallSync(TEXT("{\"action\":\"status\"}"), Status);
	TestTrue(TEXT("status works with mirroring off"), Off.IsValid() && Off->GetBoolField(TEXT("success")));
	Off = CallSync(TEXT("{\"action\":\"materialize\"}"), Status);
	TestEqual(TEXT("materialize needs mirroring on"), FirstCode(Off), FString(TEXT("Sync.MirroringOff")));
	GetMutableDefault<UAssetDocumentSettings>()->bMirrorSidecars = true;

	TestEqual(TEXT("an unknown action"), FirstCode(CallSync(TEXT("{\"action\":\"other\"}"), Status)), FString(TEXT("Sync.InvalidAction")));
	TestEqual(TEXT("accept needs assets, not a folder"),
		FirstCode(CallSync(FString::Printf(TEXT("{\"action\":\"accept_sidecar\",\"targets\":[\"%s\"]}"), TEXT("/Game")), Status)),
		FString(TEXT("Sync.InvalidTarget")));
	TestEqual(TEXT("targets are /Game paths"),
		FirstCode(CallSync(TEXT("{\"action\":\"status\",\"targets\":[\"/Engine/BasicShapes\"]}"), Status)), FString(TEXT("Sync.InvalidTarget")));

	if (!Folder.Create(*this, TEXT("DA_Tool"), TEXT("\"TestString\":\"tool\"")))
	{
		return false;
	}
	const FString Target = Folder.Target(TEXT("DA_Tool"));
	auto AssetState = [&](const TSharedPtr<FJsonObject>& Result)
	{
		const TSharedPtr<FJsonObject>* Payload = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
		FString State;
		if (Result.IsValid() && Result->TryGetObjectField(TEXT("payload"), Payload) && (*Payload)->TryGetArrayField(TEXT("assets"), Assets)
			&& Assets->Num() == 1)
		{
			(*Assets)[0]->AsObject()->TryGetStringField(TEXT("state"), State);
		}
		return State;
	};
	const FString StatusBody = FString::Printf(TEXT("{\"action\":\"status\",\"targets\":[\"%s\"]}"), *Folder.Root);
	TestEqual(TEXT("an asset without its sidecar is unmirrored"), AssetState(CallSync(StatusBody, Status)), FString(TEXT("unmirrored")));

	TSharedPtr<FJsonObject> Started = CallSync(FString::Printf(TEXT("{\"action\":\"materialize\",\"targets\":[\"%s\"]}"), *Folder.Root), Status);
	TestTrue(TEXT("materialize starts"), Started.IsValid() && Started->GetBoolField(TEXT("success")));
	const TSharedPtr<FJsonObject> Health = FAssetDocumentHttpServer::CallRoute(TEXT("GET"), TEXT("/health"), FString(), {}, 0, Status);
	Runner().PublishStatus();
	const TSharedPtr<FJsonObject> Published = FAssetDocumentSidecarSyncRunner::GetPublishedStatus();
	TestTrue(TEXT("the progress is published for health_check"), Published.IsValid() && Published->GetStringField(TEXT("state")) == TEXT("materializing"));
	TestTrue(TEXT("health_check has the sidecar_sync field"), Health.IsValid() && Health->HasField(TEXT("sidecar_sync")));
	Runner().DrainForTest();
	TestEqual(TEXT("then in sync"), AssetState(CallSync(StatusBody, Status)), FString(TEXT("in_sync")));
	TestEqual(TEXT("the published status is idle again"), FAssetDocumentSidecarSyncRunner::GetPublishedStatus()->GetStringField(TEXT("state")), FString(TEXT("idle")));

	// A conflict, then its resolution through the tool.
	TestTrue(TEXT("asset changed"), Folder.SetAndSave(TEXT("DA_Tool"), TEXT("TestString"), TEXT("from_asset")));
	Runner().DrainForTest();
	FString Text = Folder.Read(TEXT("DA_Tool"));
	TestTrue(TEXT("the save was mirrored"), Text.Contains(TEXT("\"from_asset\"")));
	TestTrue(TEXT("asset changed again"), Folder.SetAndSave(TEXT("DA_Tool"), TEXT("TestString"), TEXT("asset_again")));
	FFileHelper::SaveStringToFile(Text.Replace(TEXT("\"from_asset\""), TEXT("\"from_file\"")), *Folder.Sidecar(TEXT("DA_Tool")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	TestEqual(TEXT("status reports the conflict"), AssetState(CallSync(StatusBody, Status)), FString(TEXT("conflict")));
	const TSharedPtr<FJsonObject> Resolved = CallSync(FString::Printf(
		TEXT("{\"action\":\"accept_sidecar\",\"targets\":[\"%s\"],\"regions\":[\"/Properties\"]}"), *Target), Status);
	TestTrue(FString::Printf(TEXT("accept_sidecar resolves (%s)"), *FirstCode(Resolved)), Resolved.IsValid() && Resolved->GetBoolField(TEXT("success")));
	TestEqual(TEXT("the asset has the file's value"), Folder.Value(TEXT("DA_Tool"), TEXT("TestString")), FString(TEXT("from_file")));
	Runner().DrainForTest();

	const TSharedPtr<FJsonObject> Counts = CallSync(TEXT("{\"action\":\"status\"}"), Status);
	const TSharedPtr<FJsonObject>* Payload = nullptr;
	TestTrue(TEXT("status without targets has counts and issues"), Counts.IsValid() && Counts->TryGetObjectField(TEXT("payload"), Payload)
		&& (*Payload)->HasField(TEXT("counts")) && (*Payload)->HasField(TEXT("issues")) && (*Payload)->HasField(TEXT("runner")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncRunnerTurnOnTest,
	"UEAssetDoc.SidecarSync.Runner.TurningOnSchedulesTheRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncRunnerTurnOnTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncRunnerTestsLocal;
	const FScopedMirrorSettings Settings;
	// As the Project Settings checkbox does: the value changes, then the settings object hears of it.
	UAssetDocumentSettings* Mutable = GetMutableDefault<UAssetDocumentSettings>();
	FProperty* Mirror = UAssetDocumentSettings::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UAssetDocumentSettings, bMirrorSidecars));
	auto Change = [&](bool bOn)
	{
		Mutable->bMirrorSidecars = bOn;
		FPropertyChangedEvent Changed(Mirror, EPropertyChangeType::ValueSet);
		Mutable->PostEditChangeProperty(Changed);
	};
	Change(true);
	TSharedRef<FJsonObject> Status = Runner().GetStatusJson();
	TestTrue(TEXT("turning mirroring on enables it"), Status->GetBoolField(TEXT("enabled")));
	TestEqual(TEXT("and schedules the background run for the next ready tick"), Status->GetStringField(TEXT("state")), FString(TEXT("waiting")));
	Change(false);
	Status = Runner().GetStatusJson();
	TestFalse(TEXT("turning it off disables it"), Status->GetBoolField(TEXT("enabled")));
	TestEqual(TEXT("and drops the scheduled run"), Status->GetStringField(TEXT("state")), FString(TEXT("idle")));
	Mutable->bMirrorSidecars = true;
	TestEqual(TEXT("nothing is left scheduled"), Runner().GetStatusJson()->GetStringField(TEXT("state")), FString(TEXT("idle")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncRunnerDiscoveryTimeSlicedTest,
	"UEAssetDoc.SidecarSync.Runner.ReconcileDiscoveryIsTimeSliced",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncRunnerDiscoveryTimeSlicedTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncRunnerTestsLocal;
	const FScopedMirrorSettings Settings;
	const FTestFolder Folder;
	if (!Folder.Create(*this, TEXT("DA_A"), TEXT("\"TestString\":\"a\""))
		|| !Folder.Create(*this, TEXT("DA_B"), TEXT("\"TestString\":\"b\""))
		|| !Folder.Create(*this, TEXT("DA_C"), TEXT("\"TestString\":\"c\"")))
	{
		return false;
	}
	Runner().StartReconcile(Folder.Root);
	Runner().DrainForTest();
	TestEqual(TEXT("the three assets are mirrored"), LastRunCount(TEXT("states"), TEXT("wrote")), 3);

	const int32 FastPaths = FAssetDocumentSidecarSyncRunner::GetFastPathCountForTest();
	Runner().StartReconcile(Folder.Root);
	TestEqual(TEXT("starting the reconcile checks no sidecar yet"), FAssetDocumentSidecarSyncRunner::GetFastPathCountForTest() - FastPaths, 0);
	TestTrue(TEXT("the run is active while it looks for work"), Runner().IsRunActive());
	TestEqual(TEXT("and shown as reconciling"), Runner().GetStatusJson()->GetStringField(TEXT("state")), FString(TEXT("reconciling")));
	Runner().TickForTest(0.0);
	TestTrue(TEXT("a tick without time left takes one step"), FAssetDocumentSidecarSyncRunner::GetFastPathCountForTest() - FastPaths <= 1);
	Runner().DrainForTest();
	TestEqual(TEXT("over the ticks each pair is answered by the index"), FAssetDocumentSidecarSyncRunner::GetFastPathCountForTest() - FastPaths, 3);
	TestFalse(TEXT("then the run is over"), Runner().IsRunActive());
	TestEqual(TEXT("with nothing to sync"), LastRun().IsValid() ? static_cast<int32>(LastRun()->GetNumberField(TEXT("total"))) : -1, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncRunnerStatusCheapTest,
	"UEAssetDoc.SidecarSync.Runner.StatusChecksWithoutLoading",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncRunnerStatusCheapTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncRunnerTestsLocal;
	const FScopedMirrorSettings Settings;
	const FTestFolder Folder;
	if (!Folder.Create(*this, TEXT("DA_Status"), TEXT("\"TestString\":\"status\"")))
	{
		return false;
	}
	int32 Status = 0;
	const FString Body = FString::Printf(TEXT("{\"action\":\"status\",\"targets\":[\"%s\"]}"), *Folder.Root);
	auto AssetState = [&]()
	{
		const TSharedPtr<FJsonObject> Result = CallSync(Body, Status);
		const TSharedPtr<FJsonObject>* Payload = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
		FString State;
		if (Result.IsValid() && Result->TryGetObjectField(TEXT("payload"), Payload) && (*Payload)->TryGetArrayField(TEXT("assets"), Assets)
			&& Assets->Num() == 1)
		{
			(*Assets)[0]->AsObject()->TryGetStringField(TEXT("state"), State);
		}
		return State;
	};

	const int32 Extracts = FAssetDocumentSidecarSync::GetExtractCountForTest();
	GetMutableDefault<UAssetDocumentSettings>()->bMirrorSidecars = false;
	TestEqual(TEXT("with mirroring off an asset is reported as such"), AssetState(), FString(TEXT("off")));
	GetMutableDefault<UAssetDocumentSettings>()->bMirrorSidecars = true;
	TestEqual(TEXT("with mirroring on an asset without its sidecar is unmirrored"), AssetState(), FString(TEXT("unmirrored")));
	TestEqual(TEXT("neither loads nor extracts the asset"), FAssetDocumentSidecarSync::GetExtractCountForTest(), Extracts);

	Runner().StartMaterialize({Folder.Target(TEXT("DA_Status"))});
	Runner().DrainForTest();
	const int32 AfterMirror = FAssetDocumentSidecarSync::GetExtractCountForTest();
	TestEqual(TEXT("a mirrored, unchanged asset is in sync"), AssetState(), FString(TEXT("in_sync")));
	TestEqual(TEXT("without an extract"), FAssetDocumentSidecarSync::GetExtractCountForTest(), AfterMirror);
	return true;
}

#endif
