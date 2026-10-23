// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Applying an unchanged extract through the production Service must succeed
// without mutating anything: no transaction mutation, no dirty package, no
// package save and no sidecar rewrite (even when the rewrite is allowed).

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

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorNoOpApplyTests
{
struct FFixture
{
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	explicit FFixture(const TCHAR* Prefix)
		: AssetName(FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		PackageName = FString(TEXT("/Game/AssetDocumentNoOpApply/")) + AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentNoOpApply"),
			AssetName + TEXT(".assetdoc.json"));
	}

	void Cleanup() const
	{
		UObject* Asset = FSoftObjectPath(ObjectPath).ResolveObject();
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			if (Asset != nullptr)
			{
				UPackageTools::UnloadPackages({Asset->GetOutermost()});
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			}
			IFileManager::Get().Delete(*PackageFile, false, true);
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		}
		else if (Asset != nullptr)
		{
			ObjectTools::DeleteObjects({Asset}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
		}
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};

void Report(FAutomationTestBase& Test, const FAssetDocumentResult& Result, const TCHAR* Stage)
{
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s"), Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
}

FAssetDocumentResult Apply(FAutomationTestBase& Test, const FFixture& Fixture, const FString& Json,
	const TCHAR* Stage)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Fixture.SidecarPath), true);
	Test.TestTrue(*FString::Printf(TEXT("%s writes sidecar"), Stage),
		FFileHelper::SaveStringToFile(Json, *Fixture.SidecarPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Fixture.SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	Report(Test, Result, Stage);
	return Result;
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

TSharedPtr<FJsonObject> Extract(FAutomationTestBase& Test, const FFixture& Fixture, const TCHAR* Stage)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Fixture.ObjectPath;
	const FAssetDocumentResult Extracted = FAssetDocumentModule::Get().GetService().Extract(Request);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s extract succeeds"), Stage), Extracted.IsSuccess() && Extracted.Payload.IsValid()))
	{
		Report(Test, Extracted, Stage);
		return nullptr;
	}
	return Extracted.Payload;
}

struct FDiskState
{
	TArray<uint8> Package;
	FDateTime PackageTime;
	TArray<uint8> Sidecar;
	FDateTime SidecarTime;
};

FDiskState Capture(const FFixture& Fixture)
{
	FDiskState State;
	FFileHelper::LoadFileToArray(State.Package, *Fixture.PackageFile);
	State.PackageTime = IFileManager::Get().GetTimeStamp(*Fixture.PackageFile);
	FFileHelper::LoadFileToArray(State.Sidecar, *Fixture.SidecarPath);
	State.SidecarTime = IFileManager::Get().GetTimeStamp(*Fixture.SidecarPath);
	return State;
}

// Writes the current extract verbatim as the sidecar and applies it again.
void RunNoOp(FAutomationTestBase& Test, const FFixture& Fixture, const TCHAR* Stage)
{
	const TSharedPtr<FJsonObject> Before = Extract(Test, Fixture, Stage);
	if (!Before.IsValid())
	{
		return;
	}
	const FString Unchanged = Serialize(Before);
	Test.AddInfo(FString::Printf(TEXT("%s unchanged extract: %s"), Stage, *Unchanged));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Fixture.SidecarPath), true);
	Test.TestTrue(TEXT("writes the unchanged extract"),
		FFileHelper::SaveStringToFile(Unchanged, *Fixture.SidecarPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	const FDiskState BeforeDisk = Capture(Fixture);
	const UPackage* Package = FindPackage(nullptr, *Fixture.PackageName);
	Test.TestTrue(*FString::Printf(TEXT("%s: package is clean before"), Stage), Package != nullptr && !Package->IsDirty());

	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Fixture.SidecarPath;
	Request.bSaveAsset = true;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	Report(Test, Result, Stage);
	Test.TestTrue(*FString::Printf(TEXT("%s: unchanged apply succeeds"), Stage), Result.IsSuccess());
	Test.TestFalse(*FString::Printf(TEXT("%s: unchanged apply saves nothing"), Stage), Result.bSavedAsset);
	Test.TestFalse(*FString::Printf(TEXT("%s: unchanged apply rewrites no sidecar"), Stage), Result.bWroteSidecar);
	Package = FindPackage(nullptr, *Fixture.PackageName);
	Test.TestTrue(*FString::Printf(TEXT("%s: package stays clean"), Stage), Package != nullptr && !Package->IsDirty());
	const FDiskState AfterDisk = Capture(Fixture);
	Test.TestTrue(*FString::Printf(TEXT("%s: package file untouched"), Stage),
		AfterDisk.Package == BeforeDisk.Package && AfterDisk.PackageTime == BeforeDisk.PackageTime);
	Test.TestTrue(*FString::Printf(TEXT("%s: sidecar file untouched"), Stage),
		AfterDisk.Sidecar == BeforeDisk.Sidecar && AfterDisk.SidecarTime == BeforeDisk.SidecarTime);
	const TSharedPtr<FJsonObject> After = Extract(Test, Fixture, Stage);
	Test.TestTrue(*FString::Printf(TEXT("%s: extract is unchanged"), Stage),
		After.IsValid() && FJsonValue::CompareEqual(FJsonValueObject(After), FJsonValueObject(Before)));
}

FString GenericDocument(const FFixture& Fixture, const TCHAR* Class, const TCHAR* Action, const TCHAR* Properties)
{
	return FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",\"Class\":\"%s\",")
		TEXT("\"Action\":\"%s\",\"Properties\":%s}"),
		*Fixture.PackageName, Class, Action, Properties);
}

void RunGeneric(FAutomationTestBase& Test, const TCHAR* Prefix, const TCHAR* Class, const TCHAR* Properties)
{
	const FFixture Fixture(Prefix);
	ON_SCOPE_EXIT { Fixture.Cleanup(); };
	const FAssetDocumentResult Created =
		Apply(Test, Fixture, GenericDocument(Fixture, Class, TEXT("Create"), Properties), TEXT("Create"));
	if (!Test.TestTrue(FString::Printf(TEXT("%s is created"), Class), Created.IsSuccess()))
	{
		return;
	}
	RunNoOp(Test, Fixture, Class);
	// An authored subset equal to the asset is a no-op as well.
	const FDiskState BeforeDisk = Capture(Fixture);
	const FAssetDocumentResult Subset =
		Apply(Test, Fixture, GenericDocument(Fixture, Class, TEXT("Update"), Properties), TEXT("UnchangedSubset"));
	Test.TestTrue(TEXT("unchanged authored subset succeeds"), Subset.IsSuccess());
	Test.TestFalse(TEXT("unchanged authored subset saves nothing"), Subset.bSavedAsset);
	const FDiskState AfterDisk = Capture(Fixture);
	Test.TestTrue(TEXT("unchanged authored subset leaves the package file untouched"),
		AfterDisk.PackageTime == BeforeDisk.PackageTime && AfterDisk.Package == BeforeDisk.Package);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNoOpApplyDataAssetTest,
	"UEAssetDoc.GenericRuntime.NoOpApply.DataAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNoOpApplyDataAssetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorNoOpApplyTests;
	RunGeneric(*this, TEXT("DA_NoOp"), TEXT("/Script/AssetDocumentTests.TestDataAsset"),
		TEXT("{\"TestInt\":3,\"TestString\":\"unchanged\"}"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNoOpApplyCurveFloatTest,
	"UEAssetDoc.GenericRuntime.NoOpApply.CurveFloat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNoOpApplyCurveFloatTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorNoOpApplyTests;
	RunGeneric(*this, TEXT("Curve_NoOp"), TEXT("/Script/Engine.CurveFloat"), TEXT("{}"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNoOpApplyBlackboardTest,
	"UEAssetDoc.Python.BlackboardData.NoOpApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNoOpApplyBlackboardTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorNoOpApplyTests;
	const FFixture Fixture(TEXT("BB_NoOp"));
	ON_SCOPE_EXIT { Fixture.Cleanup(); };
	const FString Document = FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/AIModule.BlackboardData\",")
		TEXT("\"Action\":\"Create\",\"Definitions\":{},\"Properties\":{},\"Body\":{\"Parent\":null,\"Keys\":[")
		TEXT("{\"Category\":\"NoOp\",\"Description\":\"first\",\"KeyTypeClass\":{\"Kind\":\"ClassRef\",")
		TEXT("\"Path\":\"/Script/AIModule.BlackboardKeyType_Int\"},\"KeyTypeProperties\":{\"DefaultValue\":0},")
		TEXT("\"Name\":\"First\",\"bInstanceSynced\":false}]}}"),
		*Fixture.PackageName);
	const FAssetDocumentResult Created = Apply(*this, Fixture, Document, TEXT("Create"));
	if (!TestTrue(TEXT("Blackboard is created"), Created.IsSuccess()))
	{
		return false;
	}
	RunNoOp(*this, Fixture, TEXT("BlackboardData"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNoOpApplyBehaviorTreeTest,
	"UEAssetDoc.Python.BehaviorTree.NoOpApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNoOpApplyBehaviorTreeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorNoOpApplyTests;
	const FFixture Fixture(TEXT("BT_NoOp"));
	ON_SCOPE_EXIT { Fixture.Cleanup(); };
	const FString Document = FString::Printf(
		TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",\"Class\":\"/Script/AIModule.BehaviorTree\",")
		TEXT("\"Action\":\"Create\",\"Properties\":{},\"Body\":{\"BlackboardAsset\":null,\"Tree\":{")
		TEXT("\"GraphGuid\":\"11111111111111111111111111111111\",\"Root\":{},\"Comments\":[]}}}"),
		*Fixture.PackageName);
	const FAssetDocumentResult Created = Apply(*this, Fixture, Document, TEXT("Create"));
	if (!TestTrue(TEXT("BehaviorTree is created"), Created.IsSuccess()))
	{
		return false;
	}
	RunNoOp(*this, Fixture, TEXT("BehaviorTree"));
	return true;
}
#endif
