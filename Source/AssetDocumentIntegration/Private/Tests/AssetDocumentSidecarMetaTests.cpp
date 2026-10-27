// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace SidecarMetaTestsLocal
{
const TCHAR* const MetaObject =
	TEXT("\"_meta\":{\"sync\":{\"Version\":1,\"Package\":\"0\",\"Regions\":{\"/Properties\":{\"Sidecar\":\"x\",\"Asset\":\"y\"}}},\"Skipped\":{}}");

FString MakeDocument(const FString& Target, const TCHAR* TestString, const TCHAR* Extra)
{
	return FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",")
		TEXT("\"Class\":\"/Script/AssetDocumentTests.TestDataAsset\",\"Action\":\"CreateOrUpdate\",")
		TEXT("\"Properties\":{\"TestString\":\"%s\"}%s}"),
		*Target, TestString, Extra);
}

TSharedPtr<FJsonObject> Parse(const FString& Text)
{
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	FJsonSerializer::Deserialize(Reader, Object);
	return Object;
}

int32 CountChanged(const FAssetDocumentResult& Result)
{
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	return Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("changed"), Changed) ? Changed->Num() : -1;
}

bool HasCode(const FAssetDocumentResult& Result, const TCHAR* Code)
{
	return Result.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == Code;
	});
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentSidecarMetaIgnoredTest,
	"UEAssetDoc.SidecarSync.Meta.IgnoredByApplyValidateDiff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSidecarMetaIgnoredTest::RunTest(const FString& Parameters)
{
	using namespace SidecarMetaTestsLocal;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	const FString AssetName = FString::Printf(TEXT("DA_SidecarMeta_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString Target = FString::Printf(TEXT("/Game/AssetDocumentSidecarSyncTests/%s"), *AssetName);
	const FString SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentSidecarSyncTests"), AssetName + TEXT(".assetdoc.json"));
	const FString PackageFile = FPackageName::LongPackageNameToFilename(Target, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*SidecarPath, false, true);
		IFileManager::Get().Delete(*PackageFile, false, true);
	};

	const FString WithMeta = MakeDocument(Target, TEXT("meta"), *(FString(TEXT(",")) + MetaObject));
	TestTrue(TEXT("sidecar written"), FFileHelper::SaveStringToFile(WithMeta, *SidecarPath));
	FAssetDocumentApplyFileRequest Apply;
	Apply.FilePath = SidecarPath;
	const FAssetDocumentResult Created = Service.ApplyFile(Apply);
	TestTrue(FString::Printf(TEXT("a sidecar with _meta applies: %s"), *Created.Message), Created.IsSuccess());

	FAssetDocumentValidateRequest ValidateWith;
	ValidateWith.Document = Parse(WithMeta);
	FAssetDocumentValidateRequest ValidateWithout;
	ValidateWithout.Document = Parse(MakeDocument(Target, TEXT("meta"), TEXT("")));
	const FAssetDocumentResult ValidWith = Service.Validate(ValidateWith);
	const FAssetDocumentResult ValidWithout = Service.Validate(ValidateWithout);
	TestTrue(FString::Printf(TEXT("validate ignores _meta: %s"), *ValidWith.Message), ValidWith.IsSuccess());
	TestEqual(TEXT("validate says the same with and without _meta"), ValidWith.Message, ValidWithout.Message);

	FAssetDocumentDiffRequest DiffWith;
	DiffWith.Document = Parse(MakeDocument(Target, TEXT("changed"), *(FString(TEXT(",")) + MetaObject)));
	FAssetDocumentDiffRequest DiffWithout;
	DiffWithout.Document = Parse(MakeDocument(Target, TEXT("changed"), TEXT("")));
	const FAssetDocumentResult DiffedWith = Service.Diff(DiffWith);
	TestTrue(FString::Printf(TEXT("diff ignores _meta: %s"), *DiffedWith.Message), DiffedWith.IsSuccess());
	TestEqual(TEXT("diff lists the same changes with and without _meta"), CountChanged(DiffedWith), CountChanged(Service.Diff(DiffWithout)));
	TestEqual(TEXT("diff lists exactly the authored change"), CountChanged(DiffedWith), 1);

	FAssetDocumentApplyFileRequest Again;
	Again.FilePath = SidecarPath;
	TestTrue(TEXT("sidecar with _meta rewritten"), FFileHelper::SaveStringToFile(
		MakeDocument(Target, TEXT("meta"), *(FString(TEXT(",")) + MetaObject)) + TEXT(" "), *SidecarPath));
	const FAssetDocumentResult Reapplied = Service.ApplyFile(Again);
	TestTrue(FString::Printf(TEXT("re-applying an unchanged document with _meta succeeds: %s"), *Reapplied.Message), Reapplied.IsSuccess());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentSidecarMetaInvalidTest,
	"UEAssetDoc.SidecarSync.Meta.NonObjectRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSidecarMetaInvalidTest::RunTest(const FString& Parameters)
{
	using namespace SidecarMetaTestsLocal;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	const FString Target = FString::Printf(TEXT("/Game/AssetDocumentSidecarSyncTests/DA_SidecarMetaBad_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FAssetDocumentValidateRequest Validate;
	Validate.Document = Parse(MakeDocument(Target, TEXT("x"), TEXT(",\"_meta\":\"stamp\"")));
	const FAssetDocumentResult Result = Service.Validate(Validate);
	TestFalse(TEXT("a non-object _meta is refused"), Result.IsSuccess());
	TestTrue(FString::Printf(TEXT("with Document.InvalidMeta: %s"), *Result.Message), HasCode(Result, TEXT("Document.InvalidMeta")));
	TestTrue(TEXT("the root cause leads"), !Result.Diagnostics.IsEmpty() && Result.Diagnostics[0].Code == TEXT("Document.InvalidMeta"));
	return true;
}

#endif
