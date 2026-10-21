// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace GenericFullLifecycleTestsLocal
{
struct FFileTreeSnapshot
{
	TArray<FString> Paths;
	TArray<TArray<uint8>> Contents;

	bool operator==(const FFileTreeSnapshot& Other) const
	{
		return Paths == Other.Paths && Contents == Other.Contents;
	}
};

FString MakeLifecycleDocument(
	const FString& Target,
	const TCHAR* Action,
	const TCHAR* TestString)
{
	return FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",")
		TEXT("\"Target\":\"%s\",")
		TEXT("\"Class\":\"/Script/AssetDocumentTests.TestDataAsset\",")
		TEXT("\"Action\":\"%s\",\"Properties\":{\"TestString\":\"%s\"}}"),
		*Target,
		Action,
		TestString);
}

FFileTreeSnapshot SnapshotFiles(const FString& Root)
{
	FFileTreeSnapshot Snapshot;
	if (IFileManager::Get().FileExists(*Root))
	{
		Snapshot.Paths.Add(Root);
	}
	else
	{
		IFileManager::Get().FindFilesRecursive(
			Snapshot.Paths,
			*Root,
			TEXT("*"),
			true,
			false,
			false);
	}
	Snapshot.Paths.Sort();
	for (const FString& Path : Snapshot.Paths)
	{
		TArray<uint8>& Bytes = Snapshot.Contents.AddDefaulted_GetRef();
		FFileHelper::LoadFileToArray(Bytes, *Path);
	}
	return Snapshot;
}

void ReportFailure(
	FAutomationTestBase& Test,
	const TCHAR* Stage,
	const FAssetDocumentResult& Result)
{
	if (Result.IsSuccess())
	{
		return;
	}
	if (Result.Diagnostics.IsEmpty())
	{
		Test.AddInfo(FString::Printf(
			TEXT("%s RED result: %s [<none>] <none>"),
			Stage,
			*Result.Message));
	}
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(
			TEXT("%s RED result: %s [%s] %s"),
			Stage,
			*Result.Message,
			*Diagnostic.Code,
			*Diagnostic.Message));
	}
}

bool HasDiagnostic(
	const FAssetDocumentResult& Result,
	const TCHAR* Code,
	const TCHAR* Path)
{
	return Result.Diagnostics.ContainsByPredicate(
		[Code, Path](const FAssetDocumentDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code && Diagnostic.Path == Path;
		});
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericFullLifecycleTest,
	"UEAssetDoc.GenericRuntime.FullLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericFullLifecycleTest::RunTest(
	const FString& Parameters)
{
	using namespace GenericFullLifecycleTestsLocal;
	(void)Parameters;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	const FString AssetName = FString::Printf(
		TEXT("DA_FullLifecycle_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString Target = FString::Printf(
		TEXT("/Game/AssetDocumentGenericCutover/%s"),
		*AssetName);
	const FString ObjectPath = Target + TEXT(".") + AssetName;
	const FString SidecarPath = FPaths::Combine(
		FPaths::ProjectContentDir(),
		TEXT("AssetDocumentGenericCutover"),
		AssetName + TEXT(".assetdoc.json"));
	const FString TargetPackagePath = FPackageName::LongPackageNameToFilename(
		Target,
		FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SidecarPath), true);
	IFileManager::Get().Delete(*SidecarPath, false, true);
	IFileManager::Get().Delete(*TargetPackagePath, false, true);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*SidecarPath, false, true);
		IFileManager::Get().Delete(*TargetPackagePath, false, true);
	};
	TestTrue(
		TEXT("writes create lifecycle sidecar"),
		FFileHelper::SaveStringToFile(
			MakeLifecycleDocument(Target, TEXT("Create"), TEXT("created")),
			*SidecarPath));
	FString CreateSidecarText;
	TestTrue(
		TEXT("reads the actual create lifecycle sidecar"),
		FFileHelper::LoadFileToString(CreateSidecarText, *SidecarPath));
	AddInfo(FString::Printf(
		TEXT("Create lifecycle sidecar bytes: %s"),
		*CreateSidecarText));
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Create = Service.ApplyFile(Request);
	ReportFailure(*this, TEXT("Create"), Create);
	TestTrue(TEXT("production Service create completes"), Create.IsSuccess());
	if (!Create.IsSuccess())
	{
		return false;
	}
	UObject* const AssetBefore = FSoftObjectPath(ObjectPath).ResolveObject();
	TestNotNull(TEXT("durable create resolves its exact generic target"), AssetBefore);
	if (AssetBefore == nullptr)
	{
		return false;
	}
	FStrProperty* const TestStringProperty =
		FindFProperty<FStrProperty>(AssetBefore->GetClass(), TEXT("TestString"));
	FIntProperty* const TestIntProperty =
		FindFProperty<FIntProperty>(AssetBefore->GetClass(), TEXT("TestInt"));
	TestNotNull(TEXT("durable target exposes TestString"), TestStringProperty);
	TestNotNull(TEXT("durable target exposes TestInt"), TestIntProperty);
	if (TestStringProperty == nullptr || TestIntProperty == nullptr)
	{
		return false;
	}
	const FString BaselineTestString =
		TestStringProperty->GetPropertyValue_InContainer(AssetBefore);
	const int32 BaselineTestInt =
		TestIntProperty->GetPropertyValue_InContainer(AssetBefore);
	AssetBefore->GetOutermost()->ClearDirtyFlag();

	TestTrue(
		TEXT("writes update lifecycle sidecar"),
		FFileHelper::SaveStringToFile(
			MakeLifecycleDocument(Target, TEXT("Update"), TEXT("updated")),
			*SidecarPath));
	const FAssetDocumentResult Update = Service.ApplyFile(Request);
	ReportFailure(*this, TEXT("Update"), Update);
	TestTrue(TEXT("production Service update completes"), Update.IsSuccess());
	if (!Update.IsSuccess())
	{
		return false;
	}
	TestEqual(
		TEXT("durable update applies TestString"),
		TestStringProperty->GetPropertyValue_InContainer(AssetBefore),
		FString(TEXT("updated")));
	AssetBefore->GetOutermost()->ClearDirtyFlag();

	const FFileTreeSnapshot PackageBefore = SnapshotFiles(TargetPackagePath);
	TestTrue(
		TEXT("writes durable dirty-update sidecar"),
		FFileHelper::SaveStringToFile(
			MakeLifecycleDocument(Target, TEXT("Update"), TEXT("persisted dirty update")),
			*SidecarPath));
	const FFileTreeSnapshot UpdateSidecarBefore = SnapshotFiles(SidecarPath);
	TestStringProperty->SetPropertyValue_InContainer(
		AssetBefore,
		FString(TEXT("unsaved dirty value")));
	AssetBefore->MarkPackageDirty();
	TestTrue(
		TEXT("dirty attempt starts from a dirty package"),
		AssetBefore->GetOutermost()->IsDirty());

	const FAssetDocumentResult DirtyUpdate = Service.ApplyFile(Request);
	ReportFailure(*this, TEXT("DirtyUpdate"), DirtyUpdate);
	TestFalse(TEXT("dirty durable update fails closed"), DirtyUpdate.IsSuccess());
	TestTrue(
		TEXT("dirty durable update reports the stable target diagnostic"),
		HasDiagnostic(
			DirtyUpdate,
			TEXT("DirtyExistingAssetTransactionUnsupported"),
			TEXT("/Target")));
	// The message says what to do, in the MCP tools' words.
	TestTrue(
		TEXT("dirty durable update names the ways out"),
		DirtyUpdate.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == TEXT("DirtyExistingAssetTransactionUnsupported")
				&& Diagnostic.Message.Contains(TEXT("unsaved changes made outside this document"))
				&& Diagnostic.Message.Contains(TEXT("save_assets")) && Diagnostic.Message.Contains(TEXT("reload_assets"))
				&& !Diagnostic.Message.Contains(TEXT("generic target")) && !Diagnostic.Message.Contains(TEXT("fails closed"));
		}));
	UObject* const AssetAfterDirtyUpdate = FSoftObjectPath(ObjectPath).ResolveObject();
	TestTrue(
		TEXT("dirty durable update keeps the live object identity"),
		AssetAfterDirtyUpdate == AssetBefore);
	TestTrue(
		TEXT("dirty durable update keeps its target resolved"),
		AssetAfterDirtyUpdate != nullptr);
	if (AssetAfterDirtyUpdate == nullptr)
	{
		return false;
	}
	TestEqual(
		TEXT("dirty durable update preserves the unsaved TestString"),
		TestStringProperty->GetPropertyValue_InContainer(AssetAfterDirtyUpdate),
		FString(TEXT("unsaved dirty value")));
	TestEqual(
		TEXT("dirty durable update preserves the other live value"),
		TestIntProperty->GetPropertyValue_InContainer(AssetAfterDirtyUpdate),
		BaselineTestInt);
	TestTrue(
		TEXT("dirty durable update preserves the dirty package state"),
		AssetAfterDirtyUpdate != nullptr
			&& AssetAfterDirtyUpdate->GetOutermost()->IsDirty());
	TestTrue(
		TEXT("dirty durable update preserves package bytes"),
		PackageBefore == SnapshotFiles(TargetPackagePath));
	TestTrue(
		TEXT("dirty durable update preserves Update sidecar bytes"),
		UpdateSidecarBefore == SnapshotFiles(SidecarPath));
	TestEqual(
		TEXT("durable create established the expected baseline TestString"),
		BaselineTestString,
		FString(TEXT("created")));
	TestEqual(
		TEXT("durable create established the expected baseline TestInt"),
		BaselineTestInt,
		0);
	const FString RuntimeRoot =
		FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("AssetDocument/Runtime"));
	TestTrue(
		TEXT("writes dry-run lifecycle sidecar"),
		FFileHelper::SaveStringToFile(
			MakeLifecycleDocument(Target, TEXT("Update"), TEXT("dry-run")),
			*SidecarPath));
	const FFileTreeSnapshot DryRunPackageBefore =
		SnapshotFiles(TargetPackagePath);
	const FFileTreeSnapshot DryRunSidecarBefore =
		SnapshotFiles(SidecarPath);
	const FFileTreeSnapshot RuntimeBefore =
		SnapshotFiles(RuntimeRoot);
	const bool bDirtyBefore =
		AssetBefore != nullptr && AssetBefore->GetOutermost()->IsDirty();

	Request.bSaveAsset = false;
	const FAssetDocumentResult DryRun = Service.ApplyFile(Request);
	ReportFailure(*this, TEXT("DryRun"), DryRun);
	TestTrue(TEXT("production Service dry-run completes"), DryRun.IsSuccess());
	TestTrue(
		TEXT("dry-run leaves package bytes unchanged"),
		DryRunPackageBefore == SnapshotFiles(TargetPackagePath));
	TestTrue(
		TEXT("dry-run leaves sidecar bytes unchanged"),
		DryRunSidecarBefore == SnapshotFiles(SidecarPath));
	TestTrue(
		TEXT("dry-run leaves runtime residue unchanged"),
		RuntimeBefore == SnapshotFiles(RuntimeRoot));
	UObject* const AssetAfter = FSoftObjectPath(ObjectPath).ResolveObject();
	TestTrue(
		TEXT("dry-run leaves the same target object"),
		AssetAfter == AssetBefore);
	TestEqual(
		TEXT("dry-run leaves package dirty state unchanged"),
		AssetAfter != nullptr && AssetAfter->GetOutermost()->IsDirty(),
		bDirtyBefore);
	return true;
}

#endif
