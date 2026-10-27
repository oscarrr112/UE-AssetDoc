// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentModule.h"
#include "AssetDocumentPathsTestSupport.h"
#include "AssetDocumentPendingNotices.h"
#include "AssetDocumentService.h"
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
#include "Misc/ScopeExit.h"
#include "Runtime/AssetDocumentRuntimeTestHooks.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace SidecarSyncPassTestsLocal
{
const TCHAR* const Folder = TEXT("AssetDocumentSidecarSyncTests");
const TCHAR* const DataClass = TEXT("/Script/AssetDocumentTests.TestDataAsset");

FString FullPath(const FString& Path)
{
	FString Full = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Full);
	return Full;
}

// One test asset: its package, sidecar and conflict file are deleted, and its notices closed, at the end.
struct FSyncAsset
{
	FString Name;
	FString Target;
	FString ObjectPath;
	FString SidecarPath;
	FString ConflictFile;
	FString PackageFile;

	explicit FSyncAsset(const TCHAR* Stem)
		: Name(FString::Printf(TEXT("%s_%s"), Stem, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		Target = FString::Printf(TEXT("/Game/%s/%s"), Folder, *Name);
		ObjectPath = Target + TEXT(".") + Name;
		SidecarPath = FullPath(FPaths::Combine(FPaths::ProjectContentDir(), Folder, Name + TEXT(".assetdoc.json")));
		ConflictFile = FullPath(FPaths::Combine(FPaths::ProjectContentDir(), Folder, Name + TEXT(".assetdoc.asset.json")));
		PackageFile = FPackageName::LongPackageNameToFilename(Target, FPackageName::GetAssetPackageExtension());
	}

	~FSyncAsset()
	{
		IFileManager::Get().Delete(*SidecarPath, false, true);
		IFileManager::Get().Delete(*ConflictFile, false, true);
		IFileManager::Get().Delete(*PackageFile, false, true);
		FAssetDocumentPendingNotices::Resolve(FAssetDocumentSidecarSync::ConflictNotice, SidecarPath);
		FAssetDocumentPendingNotices::Resolve(FAssetDocumentSidecarSync::FailedNotice, SidecarPath);
	}

	FString GenericDocument(const FString& Properties) const
	{
		return FString::Printf(
			TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",\"Class\":\"%s\",\"Action\":\"CreateOrUpdate\",\"Properties\":{%s}}"),
			*Target, DataClass, *Properties);
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

	TSharedPtr<FJsonObject> ReadJson() const
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Read()), Object);
		return Object;
	}

	// Creates (or updates) the asset from Document without the sync pass, then removes the sidecar.
	bool CreateAsset(FAutomationTestBase& Test, FAssetDocumentService& Service, const FString& Document) const
	{
		if (!Write(Document))
		{
			return false;
		}
		FAssetDocumentApplyFileRequest Apply;
		Apply.FilePath = SidecarPath;
		Apply.bBypassSidecarSync = true;
		const FAssetDocumentResult Result = Service.ApplyFile(Apply);
		IFileManager::Get().Delete(*SidecarPath, false, true);
		return Test.TestTrue(FString::Printf(TEXT("%s created: %s"), *Name, *Result.Message), Result.IsSuccess());
	}
};

FAssetDocumentSidecarSyncOutcome RunSync(const FSyncAsset& Asset,
	EAssetDocumentSidecarSyncTrigger Trigger,
	EAssetDocumentSidecarSyncAccept Accept = EAssetDocumentSidecarSyncAccept::None,
	bool bDryRun = false)
{
	FAssetDocumentSidecarSyncRequest Request;
	Request.PackageName = Asset.Target;
	Request.Trigger = Trigger;
	Request.Accept = Accept;
	Request.bDryRun = bDryRun;
	return FAssetDocumentModule::Get().GetService().GetSidecarSync().Sync(Request);
}

FString Describe(const FAssetDocumentSidecarSyncOutcome& Outcome)
{
	return FString::Printf(TEXT("state=%s code=%s reason=%s message=%s"), *Outcome.State, *Outcome.Code, *Outcome.Reason, *Outcome.Message);
}

bool ExpectState(FAutomationTestBase& Test, const TCHAR* What, const FAssetDocumentSidecarSyncOutcome& Outcome, const TCHAR* State)
{
	return Test.TestEqual(FString::Printf(TEXT("%s (%s)"), What, *Describe(Outcome)), Outcome.State, FString(State));
}

// Creates a TestDataAsset with Properties and its mirror (a "wrote" pass of the materialize trigger).
bool CreateMirrored(FAutomationTestBase& Test, const FSyncAsset& Asset, const FString& Properties)
{
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	return Asset.CreateAsset(Test, Service, Asset.GenericDocument(Properties))
		&& ExpectState(Test, TEXT("the new mirror is written"), RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Materialize), TEXT("wrote"));
}

UObject* LoadAsset(const FSyncAsset& Asset)
{
	return StaticLoadObject(UObject::StaticClass(), nullptr, *Asset.ObjectPath);
}

FString GetAssetValue(const FSyncAsset& Asset, const TCHAR* Property)
{
	UObject* Object = LoadAsset(Asset);
	const FProperty* Found = Object ? Object->GetClass()->FindPropertyByName(Property) : nullptr;
	FString Text;
	if (Found)
	{
		Found->ExportTextItem_Direct(Text, Found->ContainerPtrToValuePtr<void>(Object), nullptr, Object, PPF_None);
	}
	return Text;
}

// Changes one property of the asset and saves it, as an editor edit and save would.
bool SetAssetValueAndSave(FAutomationTestBase& Test, const FSyncAsset& Asset, const TCHAR* Property, const TCHAR* Value, bool bSave = true)
{
	UObject* Object = LoadAsset(Asset);
	FProperty* Found = Object ? Object->GetClass()->FindPropertyByName(Property) : nullptr;
	if (!Test.TestNotNull(FString::Printf(TEXT("%s has %s"), *Asset.Name, Property), Found))
	{
		return false;
	}
	// As an edit in the Details panel: the asset's own change handling runs (a Blackboard drops keys its new parent
	// already has).
	Object->PreEditChange(Found);
	Found->ImportText_Direct(Value, Found->ContainerPtrToValuePtr<void>(Object), Object, PPF_None);
	FPropertyChangedEvent Changed(Found, EPropertyChangeType::ValueSet);
	Object->PostEditChangeProperty(Changed);
	Object->MarkPackageDirty();
	if (!bSave)
	{
		return true;
	}
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	return Test.TestTrue(TEXT("asset saved"), UPackage::SavePackage(Object->GetOutermost(), Object, *Asset.PackageFile, Args));
}

// Sets Properties.<Name> in the sidecar file, keeping everything else (its _meta included).
bool EditSidecar(FAutomationTestBase& Test, const FSyncAsset& Asset, const TCHAR* Name, const TSharedRef<FJsonValue>& Value)
{
	const TSharedPtr<FJsonObject> Document = Asset.ReadJson();
	if (!Test.TestTrue(TEXT("the sidecar reads"), Document.IsValid()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	TSharedPtr<FJsonObject> Edited = Document->TryGetObjectField(TEXT("Properties"), Properties) ? *Properties : MakeShared<FJsonObject>();
	Edited->SetField(Name, Value);
	Document->SetObjectField(TEXT("Properties"), Edited);
	FString Text;
	FJsonSerializer::Serialize(Document.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
	return Test.TestTrue(TEXT("the sidecar is edited"), Asset.Write(Text));
}

FString SidecarValue(const FSyncAsset& Asset, const TCHAR* Name)
{
	const TSharedPtr<FJsonObject> Document = Asset.ReadJson();
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	FString Value;
	if (Document.IsValid() && Document->TryGetObjectField(TEXT("Properties"), Properties))
	{
		const TSharedPtr<FJsonValue> Field = (*Properties)->TryGetField(Name);
		if (Field.IsValid())
		{
			Value = Field->Type == EJson::Number ? FString::Printf(TEXT("%lld"), static_cast<int64>(Field->AsNumber())) : Field->AsString();
		}
	}
	return Value;
}

TOptional<FAssetDocumentSidecarSyncMeta> SidecarMeta(const FSyncAsset& Asset)
{
	const TSharedPtr<FJsonObject> Document = Asset.ReadJson();
	return Document.IsValid() ? FAssetDocumentSidecarSyncMeta::Read(*Document) : TOptional<FAssetDocumentSidecarSyncMeta>();
}

bool HasNotice(const TCHAR* Kind, const FSyncAsset& Asset)
{
	return FAssetDocumentPendingNotices::GetOpen().ContainsByPredicate([Kind, &Asset](const FAssetDocumentPendingNotice& Notice)
	{
		return Notice.Kind == Kind && Notice.File.Equals(Asset.SidecarPath, ESearchCase::IgnoreCase);
	});
}

TSharedRef<FJsonValue> Str(const TCHAR* Value)
{
	return MakeShared<FJsonValueString>(Value);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPassNewMirrorTest,
	"UEAssetDoc.SidecarSync.Pass.NewMirrorThenInSync",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPassNewMirrorTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncPassTestsLocal;
	const FSyncAsset Asset(TEXT("DA_SyncNew"));
	if (!CreateMirrored(*this, Asset, TEXT("\"TestString\":\"first\"")))
	{
		return false;
	}
	const TOptional<FAssetDocumentSidecarSyncMeta> Meta = SidecarMeta(Asset);
	if (!TestTrue(TEXT("the mirror carries _meta.sync"), Meta.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("stamped with the package's saved hash"), Meta->Package, UE::AssetDocument::SidecarSync::GetPackageSavedHash(Asset.Target));
	TestTrue(TEXT("with the /Properties region"), Meta->Regions.Contains(TEXT("/Properties")));
	TestEqual(TEXT("the mirror holds the asset's value"), SidecarValue(Asset, TEXT("TestString")), FString(TEXT("first")));
	TestTrue(TEXT("the mirror ends with a newline (LF)"), Asset.Read().EndsWith(TEXT("}\n")) && !Asset.Read().Contains(TEXT("\r\n")));

	const FString Before = Asset.Read();
	const int32 Extracts = FAssetDocumentSidecarSync::GetExtractCountForTest();
	const FAssetDocumentSidecarSyncOutcome Again = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::SidecarChanged);
	ExpectState(*this, TEXT("an unchanged pair is in sync"), Again, TEXT("in_sync"));
	TestFalse(TEXT("nothing is written"), Again.bWroteSidecar);
	TestEqual(TEXT("the file is unchanged"), Asset.Read(), Before);
	TestEqual(TEXT("and the unchanged package is not extracted"), FAssetDocumentSidecarSync::GetExtractCountForTest(), Extracts);
	ExpectState(*this, TEXT("a save without change is in sync too"), RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Save), TEXT("in_sync"));
	TestEqual(TEXT("the file is still unchanged"), Asset.Read(), Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPassSidecarChangeTest,
	"UEAssetDoc.SidecarSync.Pass.SidecarChangeApplied",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPassSidecarChangeTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncPassTestsLocal;
	const FSyncAsset Asset(TEXT("DA_SyncFile"));
	if (!CreateMirrored(*this, Asset, TEXT("\"TestString\":\"first\"")) || !EditSidecar(*this, Asset, TEXT("TestString"), Str(TEXT("edited"))))
	{
		return false;
	}
	const FString Edited = Asset.Read();
	const FAssetDocumentSidecarSyncOutcome Status = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Reconcile,
		EAssetDocumentSidecarSyncAccept::None, true);
	ExpectState(*this, TEXT("a dry run reports the apply"), Status, TEXT("would_apply"));
	TestEqual(TEXT("a dry run writes nothing"), Asset.Read(), Edited);
	TestEqual(TEXT("and applies nothing"), GetAssetValue(Asset, TEXT("TestString")), FString(TEXT("first")));

	const FAssetDocumentSidecarSyncOutcome Outcome = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::SidecarChanged);
	ExpectState(*this, TEXT("the sidecar change is applied"), Outcome, TEXT("applied"));
	TestEqual(TEXT("changed regions"), Outcome.SidecarChangedRegions, TArray<FString>{TEXT("/Properties")});
	TestEqual(TEXT("the asset has the file's value"), GetAssetValue(Asset, TEXT("TestString")), FString(TEXT("edited")));
	TestEqual(TEXT("the file keeps it"), SidecarValue(Asset, TEXT("TestString")), FString(TEXT("edited")));
	const TOptional<FAssetDocumentSidecarSyncMeta> Meta = SidecarMeta(Asset);
	TestTrue(TEXT("restamped with the saved package"),
		Meta.IsSet() && Meta->Package == UE::AssetDocument::SidecarSync::GetPackageSavedHash(Asset.Target));
	ExpectState(*this, TEXT("then in sync"), RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Reconcile), TEXT("in_sync"));
	TestFalse(TEXT("no conflict file"), IFileManager::Get().FileExists(*Asset.ConflictFile));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPassAssetChangeTest,
	"UEAssetDoc.SidecarSync.Pass.AssetChangeMirrored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPassAssetChangeTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncPassTestsLocal;
	const FSyncAsset Asset(TEXT("DA_SyncAsset"));
	if (!CreateMirrored(*this, Asset, TEXT("\"TestString\":\"first\"")) || !SetAssetValueAndSave(*this, Asset, TEXT("TestInt"), TEXT("7")))
	{
		return false;
	}
	const FAssetDocumentSidecarSyncOutcome Outcome = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Save);
	ExpectState(*this, TEXT("the asset change is mirrored"), Outcome, TEXT("wrote"));
	TestEqual(TEXT("changed regions"), Outcome.AssetChangedRegions, TArray<FString>{TEXT("/Properties")});
	TestTrue(TEXT("the sidecar was written"), Outcome.bWroteSidecar);
	TestEqual(TEXT("the file has the new value"), SidecarValue(Asset, TEXT("TestInt")), FString(TEXT("7")));
	TestEqual(TEXT("and keeps the others"), SidecarValue(Asset, TEXT("TestString")), FString(TEXT("first")));
	ExpectState(*this, TEXT("then in sync"), RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Reconcile), TEXT("in_sync"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPassConvergedTest,
	"UEAssetDoc.SidecarSync.Pass.ConvergedIsNotAConflict",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPassConvergedTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncPassTestsLocal;
	const FSyncAsset Asset(TEXT("DA_SyncSame"));
	if (!CreateMirrored(*this, Asset, TEXT("\"TestString\":\"first\""))
		|| !SetAssetValueAndSave(*this, Asset, TEXT("TestString"), TEXT("same"))
		|| !EditSidecar(*this, Asset, TEXT("TestString"), Str(TEXT("same"))))
	{
		return false;
	}
	const FAssetDocumentSidecarSyncOutcome Outcome = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::SidecarChanged);
	ExpectState(*this, TEXT("both sides changed to the same content: in sync"), Outcome, TEXT("in_sync"));
	TestTrue(TEXT("no conflict regions"), Outcome.ConflictRegions.IsEmpty());
	TestFalse(TEXT("no conflict file"), IFileManager::Get().FileExists(*Asset.ConflictFile));
	const TOptional<FAssetDocumentSidecarSyncMeta> Meta = SidecarMeta(Asset);
	TestTrue(TEXT("stamped with the new package"),
		Meta.IsSet() && Meta->Package == UE::AssetDocument::SidecarSync::GetPackageSavedHash(Asset.Target));
	ExpectState(*this, TEXT("then in sync without a write"), RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Reconcile), TEXT("in_sync"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPassConflictTest,
	"UEAssetDoc.SidecarSync.Pass.ConflictThenAcceptSidecar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPassConflictTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncPassTestsLocal;
	const FSyncAsset Asset(TEXT("DA_SyncConflict"));
	if (!CreateMirrored(*this, Asset, TEXT("\"TestString\":\"first\""))
		|| !SetAssetValueAndSave(*this, Asset, TEXT("TestString"), TEXT("from_asset"))
		|| !EditSidecar(*this, Asset, TEXT("TestString"), Str(TEXT("from_file"))))
	{
		return false;
	}
	const FString Edited = Asset.Read();
	const FAssetDocumentSidecarSyncOutcome Outcome = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::SidecarChanged);
	ExpectState(*this, TEXT("different changes on both sides conflict"), Outcome, TEXT("conflict"));
	TestEqual(TEXT("code"), Outcome.Code, FString(TEXT("Sync.Conflict")));
	TestEqual(TEXT("regions"), Outcome.ConflictRegions, TArray<FString>{TEXT("/Properties")});
	TestEqual(TEXT("the conflict file is reported"), Outcome.ConflictFile, Asset.ConflictFile);
	TestEqual(TEXT("the sidecar is not touched"), Asset.Read(), Edited);
	TestEqual(TEXT("the asset is not touched"), GetAssetValue(Asset, TEXT("TestString")), FString(TEXT("from_asset")));
	TestTrue(TEXT("a conflict notice is open"), HasNotice(FAssetDocumentSidecarSync::ConflictNotice, Asset));
	FString Conflict;
	TestTrue(TEXT("the conflict file exists"), FFileHelper::LoadFileToString(Conflict, *Asset.ConflictFile));
	TestTrue(TEXT("with the asset's value"), Conflict.Contains(TEXT("from_asset")));
	TestTrue(TEXT("and the conflicted regions"), Conflict.Contains(TEXT("\"conflict\"")) && Conflict.Contains(TEXT("/Properties")));

	ExpectState(*this, TEXT("a pass without resolution still conflicts"), RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Reconcile), TEXT("conflict"));
	TestEqual(TEXT("and still touches nothing"), Asset.Read(), Edited);

	const FAssetDocumentSidecarSyncOutcome Accepted = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Explicit, EAssetDocumentSidecarSyncAccept::Sidecar);
	ExpectState(*this, TEXT("accept_sidecar applies the file"), Accepted, TEXT("applied"));
	TestEqual(TEXT("the asset has the file's value"), GetAssetValue(Asset, TEXT("TestString")), FString(TEXT("from_file")));
	TestEqual(TEXT("so does the mirror"), SidecarValue(Asset, TEXT("TestString")), FString(TEXT("from_file")));
	TestFalse(TEXT("the conflict file is removed"), IFileManager::Get().FileExists(*Asset.ConflictFile));
	TestFalse(TEXT("the notice is resolved"), HasNotice(FAssetDocumentSidecarSync::ConflictNotice, Asset));
	ExpectState(*this, TEXT("then in sync"), RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Reconcile), TEXT("in_sync"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPassAcceptAssetTest,
	"UEAssetDoc.SidecarSync.Pass.ConflictThenAcceptAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPassAcceptAssetTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncPassTestsLocal;
	const FSyncAsset Asset(TEXT("DA_SyncAcceptAsset"));
	if (!CreateMirrored(*this, Asset, TEXT("\"TestString\":\"first\""))
		|| !SetAssetValueAndSave(*this, Asset, TEXT("TestString"), TEXT("from_asset"))
		|| !EditSidecar(*this, Asset, TEXT("TestString"), Str(TEXT("from_file"))))
	{
		return false;
	}
	ExpectState(*this, TEXT("conflict"), RunSync(Asset, EAssetDocumentSidecarSyncTrigger::SidecarChanged), TEXT("conflict"));
	const FAssetDocumentSidecarSyncOutcome Accepted = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Explicit, EAssetDocumentSidecarSyncAccept::Asset);
	ExpectState(*this, TEXT("accept_asset rewrites the file"), Accepted, TEXT("wrote"));
	TestEqual(TEXT("the file has the asset's value"), SidecarValue(Asset, TEXT("TestString")), FString(TEXT("from_asset")));
	TestEqual(TEXT("the asset keeps it"), GetAssetValue(Asset, TEXT("TestString")), FString(TEXT("from_asset")));
	TestFalse(TEXT("the conflict file is removed"), IFileManager::Get().FileExists(*Asset.ConflictFile));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPassInitializationTest,
	"UEAssetDoc.SidecarSync.Pass.InitializationWithoutState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPassInitializationTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncPassTestsLocal;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	const FSyncAsset Asset(TEXT("DA_SyncInit"));
	const FString Document = Asset.GenericDocument(TEXT("\"TestString\":\"init\""));
	if (!Asset.CreateAsset(*this, Service, Document))
	{
		return false;
	}

	// The same content without _meta: stamped.
	Asset.Write(Document);
	const FAssetDocumentSidecarSyncOutcome Equal = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Reconcile);
	ExpectState(*this, TEXT("equal content without state is in sync"), Equal, TEXT("in_sync"));
	TestTrue(TEXT("and rewritten as the mirror"), Equal.bWroteSidecar && SidecarMeta(Asset).IsSet());

	// A file without the Properties region: the asset's region is taken.
	Asset.Write(FString::Printf(TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",\"Class\":\"%s\",\"Action\":\"CreateOrUpdate\"}"), *Asset.Target, DataClass));
	const FAssetDocumentSidecarSyncOutcome Absent = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Reconcile);
	ExpectState(*this, TEXT("a region absent from the file is taken from the asset"), Absent, TEXT("wrote"));
	TestEqual(TEXT("the file has the asset's value"), SidecarValue(Asset, TEXT("TestString")), FString(TEXT("init")));

	// Different content without _meta: an initialization conflict, never a guess.
	const FString Different = Asset.GenericDocument(TEXT("\"TestString\":\"other\""));
	Asset.Write(Different);
	const FAssetDocumentSidecarSyncOutcome Conflict = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Reconcile);
	ExpectState(*this, TEXT("different content without state conflicts"), Conflict, TEXT("conflict"));
	TestEqual(TEXT("code"), Conflict.Code, FString(TEXT("Sync.InitConflict")));
	TestEqual(TEXT("the file is not touched"), Asset.Read(), Different);
	TestEqual(TEXT("the asset is not touched"), GetAssetValue(Asset, TEXT("TestString")), FString(TEXT("init")));

	// A hand-written file in another key order and form is still equal.
	Asset.Write(FString::Printf(TEXT("{\"Properties\":{\"TestString\":\"init\"},\"Class\":\"%s\",\"Target\":\"%s\",\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\"}"), DataClass, *Asset.ObjectPath));
	ExpectState(*this, TEXT("a reordered equal file is in sync"), RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Reconcile), TEXT("in_sync"));

	// A file whose state has another codec version is treated as without state.
	FString Mirror = Asset.Read();
	Mirror.ReplaceInline(TEXT("adsh:v1:region:"), TEXT("adsh:v0:region:"));
	Asset.Write(Mirror.Replace(TEXT("\"init\""), TEXT("\"stale\"")));
	ExpectState(*this, TEXT("untrusted state with different content conflicts"),
		RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Reconcile), TEXT("conflict"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPassAssetMissingTest,
	"UEAssetDoc.SidecarSync.Pass.AssetMissing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPassAssetMissingTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncPassTestsLocal;
	const FSyncAsset WithState(TEXT("DA_SyncGoneMeta"));
	const FString Stamped = WithState.GenericDocument(TEXT("\"TestString\":\"x\"")).LeftChop(1)
		+ TEXT(",\"_meta\":{\"sync\":{\"Version\":1,\"Package\":\"0\",\"Regions\":{}}}}");
	WithState.Write(Stamped);
	const FAssetDocumentSidecarSyncOutcome Missing = RunSync(WithState, EAssetDocumentSidecarSyncTrigger::Reconcile);
	ExpectState(*this, TEXT("a synced sidecar whose asset is gone is reported"), Missing, TEXT("asset_missing"));
	TestEqual(TEXT("code"), Missing.Code, FString(TEXT("Sync.AssetMissing")));
	TestFalse(TEXT("the asset is not recreated"), FPackageName::DoesPackageExist(WithState.Target));
	TestEqual(TEXT("the file is not touched"), WithState.Read(), Stamped);
	TestTrue(TEXT("a notice is open"), HasNotice(FAssetDocumentSidecarSync::FailedNotice, WithState));

	const FSyncAsset WithoutState(TEXT("DA_SyncGoneNew"));
	WithoutState.Write(WithoutState.GenericDocument(TEXT("\"TestString\":\"created\"")));
	const FAssetDocumentSidecarSyncOutcome Created = RunSync(WithoutState, EAssetDocumentSidecarSyncTrigger::Reconcile);
	ExpectState(*this, TEXT("a sidecar without state creates its asset"), Created, TEXT("applied"));
	TestEqual(TEXT("with the file's value"), GetAssetValue(WithoutState, TEXT("TestString")), FString(TEXT("created")));
	TestTrue(TEXT("and becomes its mirror"), SidecarMeta(WithoutState).IsSet());

	const FSyncAsset Neither(TEXT("DA_SyncNeither"));
	ExpectState(*this, TEXT("neither side"), RunSync(Neither, EAssetDocumentSidecarSyncTrigger::Reconcile), TEXT("absent"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPassUnavailableTest,
	"UEAssetDoc.SidecarSync.Pass.EvidenceUnavailableAndDirty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPassUnavailableTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncPassTestsLocal;
	const FSyncAsset Asset(TEXT("DA_SyncBad"));
	if (!CreateMirrored(*this, Asset, TEXT("\"TestString\":\"first\"")))
	{
		return false;
	}
	Asset.Write(TEXT("{\"Class\": "));
	const FAssetDocumentSidecarSyncOutcome Invalid = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::SidecarChanged);
	ExpectState(*this, TEXT("an unreadable sidecar makes the pass unavailable"), Invalid, TEXT("unavailable"));
	TestEqual(TEXT("code"), Invalid.Code, FString(TEXT("Sync.InvalidSidecar")));
	TestEqual(TEXT("the file is not touched"), Asset.Read(), FString(TEXT("{\"Class\": ")));
	TestEqual(TEXT("the asset is not touched"), GetAssetValue(Asset, TEXT("TestString")), FString(TEXT("first")));
	TestTrue(TEXT("a failure notice is open"), HasNotice(FAssetDocumentSidecarSync::FailedNotice, Asset));

	Asset.Write(Asset.GenericDocument(TEXT("\"TestString\":\"first\"")));
	ExpectState(*this, TEXT("a fixed file syncs and resolves the notice"), RunSync(Asset, EAssetDocumentSidecarSyncTrigger::SidecarChanged), TEXT("in_sync"));
	TestFalse(TEXT("the notice is resolved"), HasNotice(FAssetDocumentSidecarSync::FailedNotice, Asset));

	if (!SetAssetValueAndSave(*this, Asset, TEXT("TestString"), TEXT("unsaved"), false))
	{
		return false;
	}
	const FString Before = Asset.Read();
	const FAssetDocumentSidecarSyncOutcome Dirty = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Save);
	ExpectState(*this, TEXT("an asset with unsaved changes waits for its save"), Dirty, TEXT("deferred"));
	TestEqual(TEXT("code"), Dirty.Code, FString(TEXT("Sync.TargetDirty")));
	TestEqual(TEXT("the file is not touched"), Asset.Read(), Before);
	if (UObject* Object = LoadAsset(Asset))
	{
		Object->GetOutermost()->SetDirtyFlag(false);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPassInterruptedTest,
	"UEAssetDoc.SidecarSync.Pass.InterruptedAfterApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPassInterruptedTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncPassTestsLocal;
	const FSyncAsset Asset(TEXT("DA_SyncStop"));
	if (!CreateMirrored(*this, Asset, TEXT("\"TestString\":\"first\"")) || !EditSidecar(*this, Asset, TEXT("TestString"), Str(TEXT("edited"))))
	{
		return false;
	}
	FAssetDocumentSidecarSync::StopAfterStepForTest(TEXT("apply"));
	const FAssetDocumentSidecarSyncOutcome Stopped = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::SidecarChanged);
	FAssetDocumentSidecarSync::StopAfterStepForTest(nullptr);
	ExpectState(*this, TEXT("the pass stops after the apply"), Stopped, TEXT("interrupted"));
	TestEqual(TEXT("the asset was applied"), GetAssetValue(Asset, TEXT("TestString")), FString(TEXT("edited")));

	const FAssetDocumentSidecarSyncOutcome Next = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Reconcile);
	TestTrue(FString::Printf(TEXT("the next pass finishes without a conflict (%s)"), *Describe(Next)),
		Next.State == TEXT("in_sync") || Next.State == TEXT("applied"));
	TestEqual(TEXT("the file keeps the edit"), SidecarValue(Asset, TEXT("TestString")), FString(TEXT("edited")));
	TestEqual(TEXT("so does the asset"), GetAssetValue(Asset, TEXT("TestString")), FString(TEXT("edited")));
	const TOptional<FAssetDocumentSidecarSyncMeta> Meta = SidecarMeta(Asset);
	TestTrue(TEXT("the file is stamped with the saved package"),
		Meta.IsSet() && Meta->Package == UE::AssetDocument::SidecarSync::GetPackageSavedHash(Asset.Target));
	ExpectState(*this, TEXT("then in sync"), RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Reconcile), TEXT("in_sync"));
	return true;
}

namespace SidecarSyncPassTestsLocal
{
FString BlackboardDocument(const FSyncAsset& Asset, const FString& Parent, const TArray<FString>& KeyNames)
{
	TArray<FString> Keys;
	for (const FString& Key : KeyNames)
	{
		Keys.Add(FString::Printf(
			TEXT("{\"Category\":\"\",\"Description\":\"\",\"KeyTypeClass\":{\"Kind\":\"ClassRef\",\"Path\":\"/Script/AIModule.BlackboardKeyType_Bool\"},")
			TEXT("\"KeyTypeProperties\":{\"bDefaultValue\":false},\"Name\":\"%s\",\"bInstanceSynced\":false}"), *Key));
	}
	return FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/AIModule.BlackboardData\",\"Action\":\"CreateOrUpdate\",")
		TEXT("\"Definitions\":{},\"Properties\":{},\"Body\":{\"Parent\":%s,\"Keys\":[%s]}}"),
		*Asset.Target, Parent.IsEmpty() ? TEXT("null") : *FString::Printf(TEXT("{\"Kind\":\"AssetRef\",\"Path\":\"%s\"}"), *Parent),
		*FString::Join(Keys, TEXT(",")));
}

// Adds a Bool key to Body.Keys of the sidecar file, keeping everything else.
bool AddSidecarKey(FAutomationTestBase& Test, const FSyncAsset& Asset, const TCHAR* Name)
{
	const TSharedPtr<FJsonObject> Document = Asset.ReadJson();
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
	if (!Test.TestTrue(TEXT("the Blackboard sidecar has Body.Keys"), Document.IsValid()
			&& Document->TryGetObjectField(TEXT("Body"), Body) && (*Body)->TryGetArrayField(TEXT("Keys"), Keys) && !Keys->IsEmpty()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
	Copy->Values = (*Keys)[0]->AsObject()->Values;
	Copy->SetStringField(TEXT("Name"), Name);
	TArray<TSharedPtr<FJsonValue>> Edited = *Keys;
	Edited.Add(MakeShared<FJsonValueObject>(Copy));
	(*Body)->SetArrayField(TEXT("Keys"), Edited);
	FString Text;
	FJsonSerializer::Serialize(Document.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
	return Test.TestTrue(TEXT("the sidecar is edited"), Asset.Write(Text));
}

TArray<FString> SidecarKeyNames(const FSyncAsset& Asset)
{
	TArray<FString> Names;
	const TSharedPtr<FJsonObject> Document = Asset.ReadJson();
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
	if (Document.IsValid() && Document->TryGetObjectField(TEXT("Body"), Body) && (*Body)->TryGetArrayField(TEXT("Keys"), Keys))
	{
		for (const TSharedPtr<FJsonValue>& Key : *Keys)
		{
			Names.Add(Key->AsObject()->GetStringField(TEXT("Name")));
		}
	}
	return Names;
}

FString SidecarParent(const FSyncAsset& Asset)
{
	const TSharedPtr<FJsonObject> Document = Asset.ReadJson();
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TSharedPtr<FJsonObject>* Parent = nullptr;
	FString Path;
	if (Document.IsValid() && Document->TryGetObjectField(TEXT("Body"), Body) && (*Body)->TryGetObjectField(TEXT("Parent"), Parent))
	{
		(*Parent)->TryGetStringField(TEXT("Path"), Path);
	}
	return Path;
}

// A Blackboard with key A and its mirror; the sidecar then gains key B and the asset gains a parent (saved).
bool PrepareMixedDirections(FAutomationTestBase& Test, const FSyncAsset& Asset, const FSyncAsset& Parent)
{
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	return Parent.CreateAsset(Test, Service, BlackboardDocument(Parent, FString(), {TEXT("ParentKey")}))
		&& Asset.CreateAsset(Test, Service, BlackboardDocument(Asset, FString(), {TEXT("A")}))
		&& ExpectState(Test, TEXT("the Blackboard mirror is written"), RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Materialize), TEXT("wrote"))
		&& AddSidecarKey(Test, Asset, TEXT("B"))
		&& SetAssetValueAndSave(Test, Asset, TEXT("Parent"), *Parent.ObjectPath);
}

bool CheckMixedResult(FAutomationTestBase& Test, const FSyncAsset& Asset, const FSyncAsset& Parent)
{
	bool bOk = Test.TestEqual(TEXT("the mirror has the sidecar's new key"), SidecarKeyNames(Asset), TArray<FString>{TEXT("A"), TEXT("B")});
	bOk &= Test.TestEqual(TEXT("and the asset's new parent"), SidecarParent(Asset), Parent.ObjectPath);
	bOk &= Test.TestTrue(TEXT("the asset has the parent"), GetAssetValue(Asset, TEXT("Parent")).Contains(Parent.Name));
	const FString Keys = GetAssetValue(Asset, TEXT("Keys"));
	bOk &= Test.TestTrue(FString::Printf(TEXT("and the key (%s)"), *Keys), Keys.Contains(TEXT("EntryName=\"B\"")) || Keys.Contains(TEXT("EntryName=B,")));
	return bOk;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPassMixedTest,
	"UEAssetDoc.SidecarSync.Pass.MultiRegionMixedDirections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPassMixedTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncPassTestsLocal;
	const FSyncAsset Parent(TEXT("BB_SyncParent"));
	const FSyncAsset Asset(TEXT("BB_SyncMixed"));
	if (!PrepareMixedDirections(*this, Asset, Parent))
	{
		return false;
	}
	const FAssetDocumentSidecarSyncOutcome Outcome = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::SidecarChanged);
	ExpectState(*this, TEXT("one region from each side: applied"), Outcome, TEXT("applied"));
	TestEqual(TEXT("the sidecar changed Keys"), Outcome.SidecarChangedRegions, TArray<FString>{TEXT("/Body/Keys")});
	TestEqual(TEXT("the asset changed Parent"), Outcome.AssetChangedRegions, TArray<FString>{TEXT("/Body/Parent")});
	CheckMixedResult(*this, Asset, Parent);
	ExpectState(*this, TEXT("then in sync"), RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Reconcile), TEXT("in_sync"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPassMergeInterruptedTest,
	"UEAssetDoc.SidecarSync.Pass.InterruptedAfterMerge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPassMergeInterruptedTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncPassTestsLocal;
	const FSyncAsset Parent(TEXT("BB_SyncStopParent"));
	const FSyncAsset Asset(TEXT("BB_SyncStopMerge"));
	if (!PrepareMixedDirections(*this, Asset, Parent))
	{
		return false;
	}
	FAssetDocumentSidecarSync::StopAfterStepForTest(TEXT("merge"));
	const FAssetDocumentSidecarSyncOutcome Stopped = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::SidecarChanged);
	FAssetDocumentSidecarSync::StopAfterStepForTest(nullptr);
	ExpectState(*this, TEXT("the pass stops after the merge"), Stopped, TEXT("interrupted"));
	TestEqual(TEXT("the file has the asset's parent merged in"), SidecarParent(Asset), Parent.ObjectPath);
	TestEqual(TEXT("and keeps its own key"), SidecarKeyNames(Asset), TArray<FString>{TEXT("A"), TEXT("B")});

	const FAssetDocumentSidecarSyncOutcome Next = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Reconcile);
	ExpectState(*this, TEXT("the next pass applies without a conflict"), Next, TEXT("applied"));
	CheckMixedResult(*this, Asset, Parent);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPassLinkedSidecarTest,
	"UEAssetDoc.SidecarSync.Pass.LinkedSidecarIsNotRead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPassLinkedSidecarTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncPassTestsLocal;
	const FSyncAsset Asset(TEXT("DA_SyncLinked"));
	if (!CreateMirrored(*this, Asset, TEXT("\"TestString\":\"first\"")))
	{
		return false;
	}
	// The sidecar becomes a link to a file outside Content.
	const FString Outside = FullPath(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocumentSidecarSyncTests"), Asset.Name + TEXT(".json")));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*Outside, false, true);
	};
	FFileHelper::SaveStringToFile(Asset.Read(), *Outside, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	TArray<uint8> OutsideBytes;
	FFileHelper::LoadFileToArray(OutsideBytes, *Outside);
	IFileManager::Get().Delete(*Asset.SidecarPath, false, true);
	if (!AssetDocumentPathsTestSupport::MakeFileLink(Asset.SidecarPath, Outside))
	{
		AddInfo(TEXT("file links cannot be made here; the check is skipped"));
		return true;
	}
	const FAssetDocumentSidecarSyncOutcome Outcome = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::SidecarChanged);
	ExpectState(*this, TEXT("a linked sidecar is not synced"), Outcome, TEXT("unavailable"));
	const FAssetDocumentSidecarSyncIndexEntry* Entry = FAssetDocumentModule::Get().GetService().GetSidecarSync().GetIndex().Find(Asset.Target);
	if (TestNotNull(TEXT("the pass is recorded"), Entry))
	{
		TestNotEqual(TEXT("the record does not hold the digest of the file outside Content"), Entry->FileDigest,
			UE::AssetDocument::SidecarSync::DigestBytes(OutsideBytes));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPassAcceptNamedRegionTest,
	"UEAssetDoc.SidecarSync.Pass.AcceptNamedRegionWithoutConflict",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPassAcceptNamedRegionTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncPassTestsLocal;
	auto Accept = [](const FSyncAsset& Asset, EAssetDocumentSidecarSyncAccept Side)
	{
		FAssetDocumentSidecarSyncRequest Request;
		Request.PackageName = Asset.Target;
		Request.Trigger = EAssetDocumentSidecarSyncTrigger::Explicit;
		Request.Accept = Side;
		Request.AcceptRegions = {TEXT("/Properties")};
		return FAssetDocumentModule::Get().GetService().GetSidecarSync().Sync(Request);
	};

	// Only the sidecar changed the region; accept_asset of it takes the asset's version.
	{
		const FSyncAsset Asset(TEXT("DA_SyncAcceptNamedAsset"));
		if (!CreateMirrored(*this, Asset, TEXT("\"TestString\":\"first\"")) || !EditSidecar(*this, Asset, TEXT("TestString"), Str(TEXT("rejected"))))
		{
			return false;
		}
		ExpectState(*this, TEXT("accept_asset of a region only the sidecar changed rewrites it"), Accept(Asset, EAssetDocumentSidecarSyncAccept::Asset), TEXT("wrote"));
		TestEqual(TEXT("the file has the asset's value"), SidecarValue(Asset, TEXT("TestString")), FString(TEXT("first")));
		ExpectState(*this, TEXT("a later pass has nothing to do"), RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Save), TEXT("in_sync"));
		TestEqual(TEXT("the rejected edit is never applied"), GetAssetValue(Asset, TEXT("TestString")), FString(TEXT("first")));
	}

	// Only the asset changed the region; accept_sidecar of it applies the sidecar's version.
	{
		const FSyncAsset Asset(TEXT("DA_SyncAcceptNamedSidecar"));
		if (!CreateMirrored(*this, Asset, TEXT("\"TestString\":\"first\"")) || !SetAssetValueAndSave(*this, Asset, TEXT("TestString"), TEXT("rejected")))
		{
			return false;
		}
		ExpectState(*this, TEXT("accept_sidecar of a region only the asset changed applies the file"), Accept(Asset, EAssetDocumentSidecarSyncAccept::Sidecar), TEXT("applied"));
		TestEqual(TEXT("the asset has the file's value"), GetAssetValue(Asset, TEXT("TestString")), FString(TEXT("first")));
		ExpectState(*this, TEXT("a later pass has nothing to do"), RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Save), TEXT("in_sync"));
		TestEqual(TEXT("the file keeps its value"), SidecarValue(Asset, TEXT("TestString")), FString(TEXT("first")));
	}
	return true;
}

namespace SidecarSyncPassTestsLocal
{
// Writes TestString = Value into the sidecar the first time the pass reaches Step, as an agent editing the file while
// the pass runs; removed at the end of the scope.
struct FEditDuringPass
{
	FEditDuringPass(FAutomationTestBase& Test, const FSyncAsset& Asset, const TCHAR* InStep, const TCHAR* Value)
	{
		FAssetDocumentSidecarSync::SetStepHookForTest([&Test, &Asset, Step = FString(InStep), Edit = FString(Value), this](const FString& Reached)
		{
			if (!bDone && Reached == Step)
			{
				bDone = true;
				EditSidecar(Test, Asset, TEXT("TestString"), MakeShared<FJsonValueString>(Edit));
				Written = Asset.Read();
			}
		});
	}
	~FEditDuringPass()
	{
		FAssetDocumentSidecarSync::SetStepHookForTest(nullptr);
	}
	bool bDone = false;
	FString Written;
};

// The edit made during the pass survived, and its asset has another pass queued.
void ExpectEditKept(FAutomationTestBase& Test, const FSyncAsset& Asset, const FEditDuringPass& Edit,
	const FAssetDocumentSidecarSyncOutcome& Outcome)
{
	Test.TestTrue(TEXT("the edit was made during the pass"), Edit.bDone);
	ExpectState(Test, TEXT("the pass stops"), Outcome, TEXT("changed"));
	Test.TestEqual(TEXT("the edit is still in the file"), Asset.Read(), Edit.Written);
	Test.TestTrue(TEXT("another pass is queued"), FAssetDocumentModule::Get().GetService().GetSidecarSyncRunner().IsQueued(Asset.Target));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPassEditBeforeApplyTest,
	"UEAssetDoc.SidecarSync.Pass.EditDuringPassBeforeApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPassEditBeforeApplyTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncPassTestsLocal;
	const FSyncAsset Asset(TEXT("DA_SyncEditBeforeApply"));
	if (!CreateMirrored(*this, Asset, TEXT("\"TestString\":\"first\"")) || !EditSidecar(*this, Asset, TEXT("TestString"), Str(TEXT("edited"))))
	{
		return false;
	}
	{
		const FEditDuringPass Edit(*this, Asset, TEXT("before_apply"), TEXT("edited again"));
		const FAssetDocumentSidecarSyncOutcome Outcome = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::SidecarChanged);
		ExpectEditKept(*this, Asset, Edit, Outcome);
		TestEqual(TEXT("the content the pass read is not applied"), GetAssetValue(Asset, TEXT("TestString")), FString(TEXT("first")));
	}
	ExpectState(*this, TEXT("the next pass applies the new content"), RunSync(Asset, EAssetDocumentSidecarSyncTrigger::SidecarChanged), TEXT("applied"));
	TestEqual(TEXT("the asset has it"), GetAssetValue(Asset, TEXT("TestString")), FString(TEXT("edited again")));
	FAssetDocumentModule::Get().GetService().GetSidecarSyncRunner().DrainForTest();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPassEditAfterApplyTest,
	"UEAssetDoc.SidecarSync.Pass.EditDuringPassAfterApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPassEditAfterApplyTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncPassTestsLocal;
	const FSyncAsset Asset(TEXT("DA_SyncEditAfterApply"));
	if (!CreateMirrored(*this, Asset, TEXT("\"TestString\":\"first\"")) || !EditSidecar(*this, Asset, TEXT("TestString"), Str(TEXT("edited"))))
	{
		return false;
	}
	{
		// Lands after the apply rewrote the file and before the mirror is written.
		const FEditDuringPass Edit(*this, Asset, TEXT("after_apply"), TEXT("edited again"));
		const FAssetDocumentSidecarSyncOutcome Outcome = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::SidecarChanged);
		ExpectEditKept(*this, Asset, Edit, Outcome);
		TestEqual(TEXT("the content the pass read was applied"), GetAssetValue(Asset, TEXT("TestString")), FString(TEXT("edited")));
	}
	// The new content is a sidecar change since that apply, not a conflict with it.
	ExpectState(*this, TEXT("the next pass applies the new content"), RunSync(Asset, EAssetDocumentSidecarSyncTrigger::SidecarChanged), TEXT("applied"));
	TestEqual(TEXT("the asset has it"), GetAssetValue(Asset, TEXT("TestString")), FString(TEXT("edited again")));
	ExpectState(*this, TEXT("then the pair is in sync"), RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Save), TEXT("in_sync"));
	FAssetDocumentModule::Get().GetService().GetSidecarSyncRunner().DrainForTest();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPassEditBeforeMirrorWriteTest,
	"UEAssetDoc.SidecarSync.Pass.EditDuringPassBeforeMirrorWrite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPassEditBeforeMirrorWriteTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncPassTestsLocal;
	const FSyncAsset Asset(TEXT("DA_SyncEditBeforeWrite"));
	if (!CreateMirrored(*this, Asset, TEXT("\"TestString\":\"first\"")) || !SetAssetValueAndSave(*this, Asset, TEXT("TestString"), TEXT("saved")))
	{
		return false;
	}
	{
		// The asset was saved; the pass would rewrite the file from it.
		const FEditDuringPass Edit(*this, Asset, TEXT("before_write"), TEXT("agent"));
		const FAssetDocumentSidecarSyncOutcome Outcome = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::Save);
		ExpectEditKept(*this, Asset, Edit, Outcome);
	}
	// Both sides changed the same region since the last sync: the next pass reports it and keeps the file.
	const FString Edited = Asset.Read();
	const FAssetDocumentSidecarSyncOutcome Next = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::SidecarChanged);
	ExpectState(*this, TEXT("the next pass sees both changes"), Next, TEXT("conflict"));
	TestEqual(TEXT("the file is still the edit"), Asset.Read(), Edited);
	TestEqual(TEXT("and the asset keeps its save"), GetAssetValue(Asset, TEXT("TestString")), FString(TEXT("saved")));
	FAssetDocumentModule::Get().GetService().GetSidecarSyncRunner().DrainForTest();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSidecarSyncPassEditWhileApplyingTest,
	"UEAssetDoc.SidecarSync.Pass.EditDuringPassWhileApplying",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSidecarSyncPassEditWhileApplyingTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncPassTestsLocal;
	const FSyncAsset Asset(TEXT("DA_SyncEditWhileApplying"));
	if (!CreateMirrored(*this, Asset, TEXT("\"TestString\":\"first\"")) || !EditSidecar(*this, Asset, TEXT("TestString"), Str(TEXT("edited"))))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		FAssetDocumentRuntimeTestHooks::Reset();
	};
	{
		// Lands inside the apply, after it read the file and just before it rewrites it.
		FEditDuringPass Edit(*this, Asset, TEXT("unused"), TEXT("edited again"));
		FAssetDocumentRuntimeTestHooks::SetPersistenceEventObserver([this, &Asset, &Edit](FStringView Event)
		{
			if (!Edit.bDone && Event == TEXT("InstallSidecar"))
			{
				Edit.bDone = true;
				EditSidecar(*this, Asset, TEXT("TestString"), MakeShared<FJsonValueString>(TEXT("edited again")));
				Edit.Written = Asset.Read();
			}
		});
		const FAssetDocumentSidecarSyncOutcome Outcome = RunSync(Asset, EAssetDocumentSidecarSyncTrigger::SidecarChanged);
		FAssetDocumentRuntimeTestHooks::SetPersistenceEventObserver(nullptr);
		ExpectEditKept(*this, Asset, Edit, Outcome);
		TestEqual(TEXT("the content the pass read was applied"), GetAssetValue(Asset, TEXT("TestString")), FString(TEXT("edited")));
	}
	ExpectState(*this, TEXT("the next pass applies the new content"), RunSync(Asset, EAssetDocumentSidecarSyncTrigger::SidecarChanged), TEXT("applied"));
	TestEqual(TEXT("the asset has it"), GetAssetValue(Asset, TEXT("TestString")), FString(TEXT("edited again")));
	FAssetDocumentModule::Get().GetService().GetSidecarSyncRunner().DrainForTest();
	return true;
}

#endif
