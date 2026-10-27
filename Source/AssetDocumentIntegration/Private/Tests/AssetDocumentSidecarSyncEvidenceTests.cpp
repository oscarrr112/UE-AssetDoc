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
#include "Runtime/AssetDocumentRuntimeTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace SidecarSyncEvidenceTestsLocal
{
TMap<FString, FString> RegionsOf(const FAssetDocumentRuntimeResultDto& Result)
{
	TMap<FString, FString> Regions;
	if (!Result.CanonicalValue.IsSet())
	{
		return Regions;
	}
	const FAssetDocumentRuntimeValueDto& Value = Result.CanonicalValue.GetValue();
	const int32 Index = Value.ObjectKeys.IndexOfByKey(FString(TEXT("Regions")));
	if (Index == INDEX_NONE || !Value.Elements.IsValidIndex(Index))
	{
		return Regions;
	}
	const FAssetDocumentRuntimeValueDto& Object = Value.Elements[Index];
	for (int32 Key = 0; Key < Object.ObjectKeys.Num() && Key < Object.Elements.Num(); ++Key)
	{
		Regions.Add(Object.ObjectKeys[Key], Object.Elements[Key].StringValue);
	}
	return Regions;
}

FString TextOf(TConstArrayView<uint8> Bytes)
{
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	return FString(Converted.Length(), Converted.Get());
}

TSharedRef<FJsonObject> Options(const TCHAR* Mode, const FString& Text)
{
	const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("Mode"), Mode);
	Result->SetStringField(TEXT("Text"), Text);
	return Result;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentSidecarSyncMirrorEvidenceTest,
	"UEAssetDoc.SidecarSync.Evidence.MirrorEqualsExtract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSidecarSyncMirrorEvidenceTest::RunTest(const FString& Parameters)
{
	using namespace SidecarSyncEvidenceTestsLocal;
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	const FString AssetName = FString::Printf(TEXT("DA_SyncEvidence_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString Target = FString::Printf(TEXT("/Game/AssetDocumentSidecarSyncTests/%s"), *AssetName);
	const FString SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentSidecarSyncTests"), AssetName + TEXT(".assetdoc.json"));
	const FString PackageFile = FPackageName::LongPackageNameToFilename(Target, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*SidecarPath, false, true);
		IFileManager::Get().Delete(*PackageFile, false, true);
	};
	const FString Document = FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",\"Class\":\"/Script/AssetDocumentTests.TestDataAsset\",")
		TEXT("\"Action\":\"CreateOrUpdate\",\"Properties\":{\"TestString\":\"evidence\",\"TestInt\":3}}"),
		*Target);
	TestTrue(TEXT("sidecar written"), FFileHelper::SaveStringToFile(Document, *SidecarPath));
	FAssetDocumentApplyFileRequest Apply;
	Apply.FilePath = SidecarPath;
	Apply.bBypassSidecarSync = true;
	const FAssetDocumentResult Created = Service.ApplyFile(Apply);
	if (!TestTrue(FString::Printf(TEXT("asset created: %s"), *Created.Message), Created.IsSuccess()))
	{
		return false;
	}

	const FAssetDocumentRuntimeResultDto Extracted = Service.ExtractForMirror(Target);
	if (!TestTrue(TEXT("the asset is extracted as sidecar bytes"),
			Extracted.TerminalStatus == EAssetDocumentRuntimeTerminalStatus::Succeeded && !Extracted.CanonicalBytes.IsEmpty()))
	{
		return false;
	}
	const FString ExtractText = TextOf(Extracted.CanonicalBytes);
	const FAssetDocumentRuntimeResultDto ExtractEvidence = Service.DispatchSidecarSync(Options(TEXT("evidence"), ExtractText));
	const TSharedRef<FJsonObject> MirrorOptions = Options(TEXT("mirror"), ExtractText);
	MirrorOptions->SetStringField(TEXT("Package"), TEXT("package-hash"));
	const FAssetDocumentRuntimeResultDto Mirror = Service.DispatchSidecarSync(MirrorOptions);
	TestTrue(TEXT("the extract's evidence is computed"), ExtractEvidence.TerminalStatus == EAssetDocumentRuntimeTerminalStatus::Succeeded);
	TestTrue(TEXT("the mirror is computed"), Mirror.TerminalStatus == EAssetDocumentRuntimeTerminalStatus::Succeeded && !Mirror.CanonicalBytes.IsEmpty());
	const TMap<FString, FString> ExtractRegions = RegionsOf(ExtractEvidence);
	TestTrue(TEXT("the extract has the /Class and /Properties regions"),
		ExtractRegions.Contains(TEXT("/Class")) && ExtractRegions.Contains(TEXT("/Properties")));
	TestTrue(TEXT("the mirror's evidence equals the extract's"), RegionsOf(Mirror).OrderIndependentCompareEqual(ExtractRegions));
	const FAssetDocumentRuntimeResultDto Reread = Service.DispatchSidecarSync(Options(TEXT("evidence"), TextOf(Mirror.CanonicalBytes)));
	TestTrue(TEXT("the written mirror reads back to the same evidence"), RegionsOf(Reread).OrderIndependentCompareEqual(ExtractRegions));
	TSharedPtr<FJsonObject> MirrorJson;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TextOf(Mirror.CanonicalBytes)), MirrorJson);
	const TSharedPtr<FJsonObject>* Meta = nullptr;
	const TSharedPtr<FJsonObject>* Sync = nullptr;
	FString Package;
	TestTrue(TEXT("the mirror carries _meta.sync with the package hash"),
		MirrorJson.IsValid() && MirrorJson->TryGetObjectField(TEXT("_meta"), Meta) && (*Meta)->TryGetObjectField(TEXT("sync"), Sync)
		&& (*Sync)->TryGetStringField(TEXT("Package"), Package) && Package == TEXT("package-hash"));

	const FAssetDocumentRuntimeResultDto Invalid = Service.DispatchSidecarSync(Options(TEXT("evidence"), TEXT("{")));
	TestTrue(TEXT("invalid text fails"), Invalid.TerminalStatus != EAssetDocumentRuntimeTerminalStatus::Succeeded);
	TestTrue(TEXT("with Sync.InvalidSidecar"), Invalid.Diagnostics.ContainsByPredicate([](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == TEXT("Sync.InvalidSidecar");
	}));
	return true;
}

#endif
