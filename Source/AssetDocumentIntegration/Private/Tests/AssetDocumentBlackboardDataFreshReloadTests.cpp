// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

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
#include "AssetDocumentPluginPaths.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace BlackboardDataFreshReloadTestsLocal
{
FString MakeTarget()
{
	return FString::Printf(TEXT("/Game/AssetDocumentBlackboardDataTests/BB_FreshReload_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

FString MakeObjectPath(const FString& Target)
{
	return FString::Printf(TEXT("%s.%s"), *Target,
		*FPackageName::GetLongPackageAssetName(Target));
}

FString MakeDocument(const FString& Target)
{
	return FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",")
		TEXT("\"Class\":\"/Script/AIModule.BlackboardData\",\"Action\":\"Create\",")
		TEXT("\"Definitions\":{},\"Properties\":{},\"Body\":{\"Parent\":null,\"Keys\":[")
		TEXT("{\"Category\":\"Reload\",\"Description\":\"durable value\",\"KeyTypeClass\":")
		TEXT("{\"Kind\":\"ClassRef\",\"Path\":\"/Script/AIModule.BlackboardKeyType_Int\"},")
		TEXT("\"KeyTypeProperties\":{\"DefaultValue\":0},\"Name\":\"PersistentKey\",\"bInstanceSynced\":false}]}}"),
		*Target);
}

bool HasNoLegacyRegistration(FAutomationTestBase& Test)
{
	FString Source;
	const FString Path = FPaths::Combine(GetAssetDocumentPluginBaseDir(), TEXT("Source/AssetDocument/Private/AssetDocumentModule.cpp"));
	return Test.TestTrue(TEXT("reads production Blackboard registration source"),
		FFileHelper::LoadFileToString(Source, *Path))
		&& Test.TestFalse(TEXT("fresh reload has no C++ Blackboard route"),
			Source.Contains(TEXT("FBlackboardDataAssetDocumentProfile")));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBlackboardDataFreshReloadTest,
	"UEAssetDoc.Python.BlackboardData.FreshReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBlackboardDataFreshReloadTest::RunTest(
	const FString& Parameters)
{
	using namespace BlackboardDataFreshReloadTestsLocal;
	(void)Parameters;
	if (!HasNoLegacyRegistration(*this))
	{
		return false;
	}
	const FString Target = MakeTarget();
	const FString ObjectPath = MakeObjectPath(Target);
	const FString SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(),
		TEXT("AssetDocumentBlackboardDataTests"),
		FPackageName::GetLongPackageAssetName(Target) + TEXT(".assetdoc.json"));
	const FString PackagePath = FPackageName::LongPackageNameToFilename(Target,
		FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*SidecarPath, false, true);
		IFileManager::Get().Delete(*PackagePath, false, true);
	};
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SidecarPath), true);
	if (!TestTrue(TEXT("writes ProductionRoute canonical sidecar"),
		FFileHelper::SaveStringToFile(MakeDocument(Target), *SidecarPath)))
	{
		return false;
	}
	FAssetDocumentApplyFileRequest ApplyRequest;
	ApplyRequest.FilePath = SidecarPath;
	ApplyRequest.bSaveAsset = true;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	const FAssetDocumentResult Applied = Service.ApplyFile(ApplyRequest);
	if (!TestTrue(TEXT("ProductionRoute applies through public ApplyFile"), Applied.IsSuccess()))
	{
		return false;
	}
	TestTrue(TEXT("ProductionRoute saves the Blackboard package"),
		IFileManager::Get().FileExists(*PackagePath));
	UPackage* const LoadedPackage = FindPackage(nullptr, *Target);
	if (!TestNotNull(TEXT("ProductionRoute loads the saved package"), LoadedPackage))
	{
		return false;
	}
	ResetLoaders(LoadedPackage);
	LoadedPackage->ClearFlags(RF_Standalone);
	CollectGarbage(RF_NoFlags);
	TestNull(TEXT("ProductionRoute unloads before fresh readback"),
		FindPackage(nullptr, *Target));
	UObject* const Reloaded = FSoftObjectPath(ObjectPath).TryLoad();
	if (!TestNotNull(TEXT("ProductionRoute reloads exact Blackboard identity"), Reloaded))
	{
		return false;
	}
	TestEqual(TEXT("ProductionRoute reload keeps the exact Blackboard class"),
		Reloaded->GetClass()->GetPathName(),
		FString(TEXT("/Script/AIModule.BlackboardData")));
	FAssetDocumentExtractRequest ExtractRequest;
	ExtractRequest.AssetPath = Target;
	const FAssetDocumentResult Extracted = Service.Extract(ExtractRequest);
	TestTrue(TEXT("ProductionRoute fresh extract succeeds"), Extracted.IsSuccess());
	TestTrue(TEXT("ProductionRoute fresh extract keeps the authored key"),
		Extracted.Payload.IsValid()
			&& Extracted.Payload->Values.Contains(TEXT("Body")));
	FAssetDocumentDiffRequest DiffRequest;
	DiffRequest.FilePath = SidecarPath;
	const FAssetDocumentResult Diffed = Service.Diff(DiffRequest);
	TestTrue(TEXT("ProductionRoute fresh diff succeeds"), Diffed.IsSuccess());
	return true;
}

#endif
