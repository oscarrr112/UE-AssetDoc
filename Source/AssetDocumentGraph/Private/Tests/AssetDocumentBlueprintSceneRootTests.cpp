// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An Actor Blueprint's SCS has one scene root, and the editor attaches a new scene component that has no parent
// to that root. A document whose scene components name no AttachTo is read the same way: the first one becomes the
// root (it replaces the DefaultSceneRoot), each later one is attached to it, on create and on update, and an omitted
// AttachTo is the scene root, so the document is the result (Diff empty, the apply verifies).
//   Blueprint.Authoring.UnattachedSceneComponents

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "../../../AssetDocument/Private/Tests/AssetDocumentDiffCountTestSupport.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentBlueprintSceneRootTestsLocal
{
struct FFixture
{
	FAutomationTestBase& Test;
	const FString Folder = FString(TEXT("AssetDocumentP93/")) + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = FString(TEXT("/Game/")) / Folder / TEXT("BP_Roots");
	const FString ObjectPath = PackageName + TEXT(".BP_Roots");
	const FString PackageFile = FPaths::ConvertRelativePathToFull(
		FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()));
	const FString SidecarPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), Folder, TEXT("BP_Roots.assetdoc.json")));

	explicit FFixture(FAutomationTestBase& InTest) : Test(InTest) {}

	~FFixture()
	{
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			Package->SetDirtyFlag(false);
			UPackageTools::UnloadPackages({Package});
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		IFileManager::Get().Delete(*PackageFile, false, true);
		IFileManager::Get().Delete(*SidecarPath, false, true);
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		IFileManager::Get().DeleteDirectory(*FPaths::Combine(FPaths::ProjectContentDir(), Folder), false, true);
	}

	FString Document(const TCHAR* Action, const FString& Components) const
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\","
			"\"Body\":{\"ParentClass\":\"/Script/Engine.Actor\",\"Components\":[%s]}}"), *PackageName, Action, *Components);
	}

	void Report(const FString& Stage, const FAssetDocumentResult& Result) const
	{
		Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
		{
			Test.AddInfo(FString::Printf(TEXT("%s: %s %s %s"), *Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
		}
	}

	static TSharedPtr<FJsonObject> Parse(const FString& Json)
	{
		TSharedPtr<FJsonObject> Parsed;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Parsed);
		return Parsed;
	}

	FAssetDocumentResult Validate(const FString& Json, const FString& Stage) const
	{
		FAssetDocumentValidateRequest Request;
		Request.Document = Parse(Json);
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
		Report(Stage, Result);
		return Result;
	}

	FAssetDocumentResult Apply(const FString& Json, const FString& Stage) const
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(SidecarPath), true);
		FFileHelper::SaveStringToFile(Json, *SidecarPath);
		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = SidecarPath;
		Request.bSaveAsset = true;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
		Report(Stage, Result);
		return Result;
	}

	int32 DiffCount(const FString& Json, const FString& Stage) const
	{
		FAssetDocumentDiffRequest Request;
		Request.Document = Parse(Json);
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Diff(Request);
		Report(Stage, Result);
		return AssetDocumentDiffCountTestSupport::CountDiffEntries(Result);
	}

	// The SCS: Trigger is the scene root (no DefaultSceneRoot left) and each of Children is attached to it.
	void CheckTree(const FString& Stage, TConstArrayView<const TCHAR*> Children) const
	{
		const UBlueprint* Blueprint = Cast<UBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject());
		USimpleConstructionScript* SCS = Blueprint ? Blueprint->SimpleConstructionScript.Get() : nullptr;
		if (!Test.TestNotNull(*FString::Printf(TEXT("%s: the Blueprint has an SCS"), *Stage), SCS))
		{
			return;
		}
		USCS_Node* Root = nullptr;
		SCS->GetSceneRootComponentTemplate(false, &Root);
		USCS_Node* Trigger = SCS->FindSCSNode(TEXT("Trigger"));
		Test.TestTrue(*FString::Printf(TEXT("%s: Trigger is the scene root"), *Stage), Trigger != nullptr && Root == Trigger);
		Test.TestFalse(*FString::Printf(TEXT("%s: the DefaultSceneRoot is gone"), *Stage),
			SCS->GetRootNodes().Contains(SCS->GetDefaultSceneRootNode()));
		for (const TCHAR* Name : Children)
		{
			USCS_Node* Child = SCS->FindSCSNode(Name);
			Test.TestTrue(*FString::Printf(TEXT("%s: %s is attached to Trigger"), *Stage, Name),
				Child != nullptr && Trigger != nullptr && SCS->FindParentNode(Child) == Trigger);
		}
	}
};
} // namespace AssetDocumentBlueprintSceneRootTestsLocal

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintUnattachedSceneComponentsTest,
	"UEAssetDoc.Blueprint.Authoring.UnattachedSceneComponents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintUnattachedSceneComponentsTest::RunTest(const FString&)
{
	using namespace AssetDocumentBlueprintSceneRootTestsLocal;
	const FFixture Fixture(*this);
	const FString Two = TEXT("{\"Name\":\"Trigger\",\"Class\":\"/Script/Engine.BoxComponent\"},"
		"{\"Name\":\"Sensor\",\"Class\":\"/Script/Engine.SphereComponent\"}");
	const FString Create = Fixture.Document(TEXT("Create"), Two);
	TestTrue(TEXT("two scene components without AttachTo validate"), Fixture.Validate(Create, TEXT("validate create")).IsSuccess());
	if (!TestTrue(TEXT("and apply: Sensor is attached to the root Trigger"), Fixture.Apply(Create, TEXT("create")).IsSuccess()))
	{
		return false;
	}
	Fixture.CheckTree(TEXT("created"), {TEXT("Sensor")});
	TestEqual(TEXT("the document is the result: Diff is empty"), Fixture.DiffCount(Fixture.Document(TEXT("Update"), Two), TEXT("diff created")), 0);

	// An update adds a third scene component without AttachTo: attached to the root too.
	const FString Three = Fixture.Document(TEXT("Update"), Two + TEXT(",{\"Name\":\"Extra\",\"Class\":\"/Script/Engine.SphereComponent\"}"));
	TestTrue(TEXT("the update validates"), Fixture.Validate(Three, TEXT("validate update")).IsSuccess());
	TestTrue(TEXT("and applies"), Fixture.Apply(Three, TEXT("update")).IsSuccess());
	Fixture.CheckTree(TEXT("updated"), {TEXT("Sensor"), TEXT("Extra")});
	TestEqual(TEXT("Diff of the update is empty"), Fixture.DiffCount(Three, TEXT("diff updated")), 0);
	return !HasAnyErrors();
}
#endif
