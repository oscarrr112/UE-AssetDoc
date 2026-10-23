// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// In-process persistence round trip through the production Service:
// ApplyFile (saved) -> Extract -> unload the package -> load it from disk -> Extract.
// The extract of the reloaded asset must equal the extract taken right after the apply.
// Replaces the former two-process FreshWrite/FreshRead pairs.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentPersistenceRoundTripTests
{
struct FFixture
{
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	explicit FFixture(const FString& Prefix)
		: AssetName(FString::Printf(TEXT("%s_%s"), *Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		PackageName = FString(TEXT("/Game/AssetDocumentRoundTrip/")) + AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentRoundTrip"),
			AssetName + TEXT(".assetdoc.json"));
	}

	void Cleanup() const
	{
		if (UObject* Asset = FSoftObjectPath(ObjectPath).ResolveObject())
		{
			if (IFileManager::Get().FileExists(*PackageFile))
			{
				UPackageTools::UnloadPackages({Asset->GetOutermost()});
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			}
			else
			{
				ObjectTools::DeleteObjects({Asset}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
			}
		}
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			IFileManager::Get().Delete(*PackageFile, false, true);
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		}
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};

struct FCase
{
	const TCHAR* Name;
	// Create document; {Target} is replaced by the target package name.
	const TCHAR* Document;
	// A value that must appear in the condensed extract, proving the authored content was applied.
	const TCHAR* AppliedWitness;
};

const FCase Cases[] = {
	{TEXT("BehaviorTree"),
		TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"{Target}\",\"Class\":\"/Script/AIModule.BehaviorTree\",")
		TEXT("\"Action\":\"Create\",\"Properties\":{},\"Body\":{\"BlackboardAsset\":null,\"Tree\":{")
		TEXT("\"GraphGuid\":\"0000000000000000000000000000a100\",\"Comments\":[],")
		TEXT("\"Root\":{\"Id\":\"0000000000000000000000000000a001\",\"Class\":\"/Script/AIModule.BTComposite_Sequence\",")
		TEXT("\"Children\":[{\"Id\":\"0000000000000000000000000000a002\",\"Class\":\"/Script/AIModule.BTTask_Wait\"}]}}}}"),
		TEXT("0000000000000000000000000000a002")},
	{TEXT("BlackboardData"),
		TEXT("{\"SchemaVersion\":1,\"Target\":\"{Target}\",\"Class\":\"/Script/AIModule.BlackboardData\",")
		TEXT("\"Action\":\"Create\",\"Definitions\":{},\"Properties\":{},\"Body\":{\"Parent\":null,\"Keys\":[")
		TEXT("{\"Category\":\"RoundTrip\",\"Description\":\"persisted\",\"KeyTypeClass\":{\"Kind\":\"ClassRef\",")
		TEXT("\"Path\":\"/Script/AIModule.BlackboardKeyType_Int\"},\"KeyTypeProperties\":{\"DefaultValue\":7},")
		TEXT("\"Name\":\"RoundTripKey\",\"bInstanceSynced\":false}]}}"),
		TEXT("RoundTripKey")},
	{TEXT("CurveFloat"),
		TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"{Target}\",\"Class\":\"/Script/Engine.CurveFloat\",")
		TEXT("\"Action\":\"Create\",\"Properties\":{\"bIsEventCurve\":true,\"FloatCurve\":{\"Keys\":[")
		TEXT("{\"Time\":0,\"Value\":1.5},{\"Time\":2,\"Value\":-3.25}],\"PostInfinityExtrap\":\"Cycle\"}}}"),
		TEXT("-3.25")},
};

void Report(FAutomationTestBase& Test, const FAssetDocumentResult& Result, const FString& Stage)
{
	if (Result.IsSuccess())
	{
		return;
	}
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s"), *Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
}

FString Serialize(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	if (Object.IsValid())
	{
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Text;
}

TSharedPtr<FJsonObject> Extract(FAutomationTestBase& Test, const FFixture& Fixture, const FString& Stage)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Fixture.ObjectPath;
	const FAssetDocumentResult Extracted = FAssetDocumentModule::Get().GetService().Extract(Request);
	Report(Test, Extracted, Stage);
	return Test.TestTrue(*(Stage + TEXT(" extract succeeds")), Extracted.IsSuccess() && Extracted.Payload.IsValid())
		? Extracted.Payload : nullptr;
}

bool RunCase(FAutomationTestBase& Test, const FCase& Case)
{
	const FString Stage(Case.Name);
	const FFixture Fixture(FString(TEXT("RT_")) + Case.Name);
	ON_SCOPE_EXIT { Fixture.Cleanup(); };

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Fixture.SidecarPath), true);
	const FString Document = FString(Case.Document).Replace(TEXT("{Target}"), *Fixture.PackageName);
	if (!Test.TestTrue(*(Stage + TEXT(" writes sidecar")), FFileHelper::SaveStringToFile(
		Document, *Fixture.SidecarPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)))
	{
		return false;
	}
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Fixture.SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Applied = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	Report(Test, Applied, Stage + TEXT(" apply"));
	if (!Test.TestTrue(*(Stage + TEXT(" apply succeeds and saves")), Applied.IsSuccess() && Applied.bSavedAsset)
		|| !Test.TestTrue(*(Stage + TEXT(" package file exists")), IFileManager::Get().FileExists(*Fixture.PackageFile)))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Before = Extract(Test, Fixture, Stage + TEXT(" applied"));
	if (!Before.IsValid())
	{
		return false;
	}
	const FString BeforeText = Serialize(Before);
	Test.TestTrue(*(Stage + TEXT(" extract contains the applied content")), BeforeText.Contains(Case.AppliedWitness));

	UPackage* Package = FindPackage(nullptr, *Fixture.PackageName);
	if (!Test.TestNotNull(*(Stage + TEXT(" package is loaded after apply")), Package))
	{
		return false;
	}
	const TWeakObjectPtr<UObject> OldAsset = FSoftObjectPath(Fixture.ObjectPath).ResolveObject();
	Test.TestFalse(*(Stage + TEXT(" package is clean after save")), Package->IsDirty());
	Test.TestTrue(*(Stage + TEXT(" package unloads")), UPackageTools::UnloadPackages({Package}));
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	if (!Test.TestNull(*(Stage + TEXT(" package is gone before reload")), FindPackage(nullptr, *Fixture.PackageName))
		|| !Test.TestFalse(*(Stage + TEXT(" old asset object was collected")), OldAsset.IsValid()))
	{
		return false;
	}

	UObject* Reloaded = FSoftObjectPath(Fixture.ObjectPath).TryLoad();
	if (!Test.TestNotNull(*(Stage + TEXT(" asset reloads from disk")), Reloaded))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> After = Extract(Test, Fixture, Stage + TEXT(" reloaded"));
	if (!After.IsValid())
	{
		return false;
	}
	const FString AfterText = Serialize(After);
	if (!Test.TestEqual(*(Stage + TEXT(" reloaded extract equals the applied extract")), AfterText, BeforeText))
	{
		Test.AddInfo(Stage + TEXT(" applied:  ") + BeforeText);
		Test.AddInfo(Stage + TEXT(" reloaded: ") + AfterText);
		return false;
	}
	return true;
}
} // namespace AssetDocumentPersistenceRoundTripTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentPersistenceRoundTripTest,
	"UEAssetDoc.Persistence.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPersistenceRoundTripTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentPersistenceRoundTripTests;
	bool bPassed = true;
	for (const FCase& Case : Cases)
	{
		bPassed &= RunCase(*this, Case);
	}
	return bPassed;
}
#endif
