// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
constexpr const TCHAR* FreshTarget =
	TEXT("/Game/AssetDocumentGenericCutover/DA_FreshReload");
constexpr const TCHAR* FreshObjectPath =
	TEXT("/Game/AssetDocumentGenericCutover/DA_FreshReload.DA_FreshReload");
constexpr const TCHAR* FreshClassPath =
	TEXT("/Script/AssetDocumentTests.TestDataAsset");

FString MakeFreshReloadDocument(
	const TCHAR* Action,
	const TCHAR* Properties)
{
	return FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",")
		TEXT("\"Target\":\"%s\",\"Class\":\"%s\",\"Action\":\"%s\",")
		TEXT("\"Definitions\":{},\"Properties\":%s,")
		TEXT("\"Body\":{}}"),
		FreshTarget,
		FreshClassPath,
		Action,
		Properties);
}

void ReportFreshFailure(
	FAutomationTestBase& Test,
	const TCHAR* Stage,
	const FAssetDocumentResult& Result)
{
	if (Result.IsSuccess())
	{
		return;
	}
	Test.AddInfo(FString::Printf(
		TEXT("%s failed: %s"),
		Stage,
		*Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(
			TEXT("[%s] %s: %s"),
			*Diagnostic.Code,
			*Diagnostic.Path,
			*Diagnostic.Message));
	}
}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericFreshReloadTest,
	"UEAssetDoc.GenericRuntime.FreshReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericFreshReloadTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	const FString SidecarPath = FPaths::Combine(
		FPaths::ProjectContentDir(),
		TEXT("AssetDocumentGenericCutover/DA_FreshReload.assetdoc.json"));
	const FString PackagePath = FPackageName::LongPackageNameToFilename(
		FreshTarget,
		FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SidecarPath), true);
	IFileManager::Get().Delete(*PackagePath, false, true);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*SidecarPath, false, true);
		IFileManager::Get().Delete(*PackagePath, false, true);
	};

	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = SidecarPath;
	Request.bSaveAsset = true;
	TestTrue(
		TEXT("writes create fresh-reload sidecar"),
		FFileHelper::SaveStringToFile(
			MakeFreshReloadDocument(TEXT("Create"), TEXT("{}")),
			*SidecarPath));
	const FAssetDocumentResult Create = Service.ApplyFile(Request);
	ReportFreshFailure(*this, TEXT("Create"), Create);
	TestTrue(TEXT("production create succeeds"), Create.IsSuccess());
	TestTrue(TEXT("production create reports durable save"), Create.bSavedAsset);
	TestTrue(TEXT("production create reports durable sidecar write"), Create.bWroteSidecar);
	UObject* const CreatedAsset = FSoftObjectPath(FreshObjectPath).ResolveObject();
	TestNotNull(TEXT("created target resolves before update"), CreatedAsset);
	if (CreatedAsset == nullptr)
	{
		return false;
	}
	CreatedAsset->GetOutermost()->ClearDirtyFlag();
	TestFalse(TEXT("update starts from a clean package baseline"), CreatedAsset->GetOutermost()->IsDirty());

	TestTrue(
		TEXT("writes update fresh-reload sidecar"),
		FFileHelper::SaveStringToFile(
			MakeFreshReloadDocument(
				TEXT("Update"),
				TEXT("{\"TestString\":\"fresh reload updated\"}")),
			*SidecarPath));
	const FAssetDocumentResult Update = Service.ApplyFile(Request);
	ReportFreshFailure(*this, TEXT("Update"), Update);
	TestTrue(TEXT("production update succeeds"), Update.IsSuccess());
	TestTrue(TEXT("production update reports durable save"), Update.bSavedAsset);
	TestTrue(TEXT("production update reports durable sidecar write"), Update.bWroteSidecar);
	TestFalse(TEXT("production update reports a sidecar path"), Update.SidecarFilePath.IsEmpty());
	TestEqual(
		TEXT("production update keeps the normalized sidecar path"),
		Update.SidecarFilePath,
		FPaths::ConvertRelativePathToFull(SidecarPath));
	TestTrue(
		TEXT("durable package exists"),
		IFileManager::Get().FileExists(*PackagePath));

	UObject* const Asset = FSoftObjectPath(FreshObjectPath).ResolveObject();
	TestNotNull(TEXT("durable target resolves after verified save"), Asset);
	if (Asset == nullptr)
	{
		return false;
	}
	TestEqual(
		TEXT("durable target keeps exact object identity"),
		Asset->GetPathName(),
		FString(FreshObjectPath));
	TestEqual(
		TEXT("durable target keeps exact class identity"),
		Asset->GetClass()->GetPathName(),
		FString(FreshClassPath));
	TestEqual(
		TEXT("durable target keeps exact package identity"),
		Asset->GetOutermost()->GetName(),
		FString(FreshTarget));
	const FStrProperty* const TestStringProperty =
		FindFProperty<FStrProperty>(Asset->GetClass(), TEXT("TestString"));
	TestNotNull(TEXT("durable target exposes TestString"), TestStringProperty);
	if (TestStringProperty != nullptr)
	{
		TestEqual(
			TEXT("durable target contains the updated value"),
			TestStringProperty->GetPropertyValue_InContainer(Asset),
			FString(TEXT("fresh reload updated")));
	}

	FString CanonicalSidecar;
	TestTrue(
		TEXT("reads Python-emitted canonical sidecar bytes"),
		FFileHelper::LoadFileToString(CanonicalSidecar, *SidecarPath));
	TestEqual(
		TEXT("durable sidecar is the production Python canonical document"),
		CanonicalSidecar,
		FString::Printf(
			TEXT("{\n  \"Action\": \"Update\",\n  \"AssetType\": \"GenericAsset\",\n")
			TEXT("  \"Class\": \"%s\",\n  \"Definitions\": {},\n")
			TEXT("  \"Properties\": {\n    \"TestString\": \"fresh reload updated\"\n  },\n")
			TEXT("  \"SchemaVersion\": 1,\n  \"Target\": \"%s\"\n}\n"),
			FreshClassPath,
			FreshTarget));

	FAssetDocumentExtractRequest ExtractRequest;
	ExtractRequest.AssetPath = FreshTarget;
	ExtractRequest.bDiffOnly = false;
	ExtractRequest.bIncludeAllWritable = true;
	const FAssetDocumentResult Extract = Service.Extract(ExtractRequest);
	ReportFreshFailure(*this, TEXT("Extract"), Extract);
	TestTrue(TEXT("saved target extracts"), Extract.IsSuccess());

	FAssetDocumentDiffRequest DiffRequest;
	DiffRequest.FilePath = SidecarPath;
	const FAssetDocumentResult Diff = Service.Diff(DiffRequest);
	ReportFreshFailure(*this, TEXT("Diff"), Diff);
	TestTrue(TEXT("saved target diffs"), Diff.IsSuccess());
	return true;
}

#endif
