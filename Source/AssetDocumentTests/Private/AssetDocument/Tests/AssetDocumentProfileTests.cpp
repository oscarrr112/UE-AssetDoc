// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentProfile.h"

#include "AssetDocumentModule.h"
#include "AssetDocumentRuntimeModule.h"
#include "AssetDocumentService.h"
#include "TestDataAsset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
const TSharedPtr<FJsonObject> FindProfileObjectByStringField(const TArray<TSharedPtr<FJsonValue>>& Values, const FString& FieldName, const FString& ExpectedValue)
{
	for (const TSharedPtr<FJsonValue>& Value : Values)
	{
		const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Object.IsValid())
		{
			continue;
		}

		FString ActualValue;
		if (Object->TryGetStringField(FieldName, ActualValue) && ActualValue == ExpectedValue)
		{
			return Object;
		}
	}
	return nullptr;
}

void TestProfileHasNoAgentFacingOperationFields(FAutomationTestBase* Test, const FString& Context, const TSharedPtr<FJsonObject>& Object)
{
	if (!Test || !Object.IsValid())
	{
		return;
	}

	for (const TCHAR* FieldName : {TEXT("patch"), TEXT("op"), TEXT("Patch"), TEXT("Op"), TEXT("Operations")})
	{
		Test->TestFalse(FString::Printf(TEXT("%s does not expose %s field"), *Context, FieldName), Object->HasField(FieldName));
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericInspectProfileTest,
	"UEAssetDoc.Profile.GenericInspect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericInspectProfileTest::RunTest(const FString& Parameters)
{
	IAssetDocumentRuntimeModule* Integration =
		FModuleManager::LoadModulePtr<IAssetDocumentRuntimeModule>(
			TEXT("AssetDocumentIntegration"));
	TestNotNull(TEXT("generic Inspect loads the Integration runtime owner"), Integration);
	TestTrue(
		TEXT("generic Inspect uses the active Integration runtime"),
		Integration && Integration->HasActiveRuntimeRegistration());
	if (!Integration || !Integration->HasActiveRuntimeRegistration())
	{
		return false;
	}
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FAssetDocumentProfileRequest Request;
	Request.ClassOrAsset = TEXT("/Script/AssetDocumentTests.TestActorBase");

	const FAssetDocumentResult Result = Service.InspectProfile(Request);
	TestTrue(TEXT("InspectProfile succeeds for reflected TestActorBase class"), Result.IsSuccess());
	TestTrue(TEXT("InspectProfile returns a payload"), Result.Payload.IsValid());

	if (!Result.Payload.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("Profile reports canonical class path"), Result.Payload->GetStringField(TEXT("Class")), FString(TEXT("/Script/AssetDocumentTests.TestActorBase")));

	const TSharedPtr<FJsonObject>* DocumentShape = nullptr;
	TestTrue(TEXT("Profile includes DocumentShape"), Result.Payload->TryGetObjectField(TEXT("DocumentShape"), DocumentShape));
	if (DocumentShape && DocumentShape->IsValid())
	{
		TestEqual(TEXT("Definitions shape is generic map"), (*DocumentShape)->GetStringField(TEXT("Definitions")), FString(TEXT("map<string, Fragment>")));
		TestEqual(TEXT("Properties shape describes reflected CDO-diff properties"), (*DocumentShape)->GetStringField(TEXT("Properties")), FString(TEXT("reflected CDO-diff properties")));

		const TSharedPtr<FJsonObject>* BodyShape = nullptr;
		TestTrue(TEXT("Body shape is an object"), (*DocumentShape)->TryGetObjectField(TEXT("Body"), BodyShape));
		if (BodyShape && BodyShape->IsValid())
		{
			TestEqual(TEXT("Generic Body shape has no entries"), (*BodyShape)->Values.Num(), 0);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* BodySections = nullptr;
	TestTrue(TEXT("Profile includes BodySections"), Result.Payload->TryGetArrayField(TEXT("BodySections"), BodySections));
	if (BodySections)
	{
		TestEqual(TEXT("Generic profile has no BodySections"), BodySections->Num(), 0);
	}

	const TArray<TSharedPtr<FJsonValue>>* FragmentKinds = nullptr;
	TestTrue(TEXT("Profile includes FragmentKinds"), Result.Payload->TryGetArrayField(TEXT("FragmentKinds"), FragmentKinds));
	if (FragmentKinds)
	{
		TestEqual(TEXT("Generic profile exposes five fragment kinds"), FragmentKinds->Num(), 5);
	}

	const TArray<TSharedPtr<FJsonValue>>* RegionPolicies = nullptr;
	TestTrue(TEXT("Generic profile includes RegionPolicies"), Result.Payload->TryGetArrayField(TEXT("RegionPolicies"), RegionPolicies));
	if (RegionPolicies)
	{
		TestEqual(TEXT("Generic profile has no exact region policies"), RegionPolicies->Num(), 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericExtractDoesNotForceSyncRegionsTest,
	"UEAssetDoc.Profile.GenericExtractDoesNotForceSyncRegions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericExtractDoesNotForceSyncRegionsTest::RunTest(const FString& Parameters)
{
	IAssetDocumentRuntimeModule* Integration =
		FModuleManager::LoadModulePtr<IAssetDocumentRuntimeModule>(
			TEXT("AssetDocumentIntegration"));
	TestNotNull(TEXT("generic Extract loads the Integration runtime owner"), Integration);
	TestTrue(
		TEXT("generic Extract uses the active Integration runtime"),
		Integration && Integration->HasActiveRuntimeRegistration());
	if (!Integration || !Integration->HasActiveRuntimeRegistration())
	{
		return false;
	}
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	const FString AssetName = FString::Printf(
		TEXT("DA_GenericExtract_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString PackageName = FString::Printf(
		TEXT("/Game/AssetDocumentTests/GenericExtract/%s"),
		*AssetName);
	UPackage* Package = CreatePackage(*PackageName);
	UTestDataAsset* Asset = NewObject<UTestDataAsset>(
		Package,
		*AssetName,
		RF_Public | RF_Standalone);
	TestNotNull(TEXT("Generic extract creates a test-owned canonical fixture"), Asset);
	if (Asset == nullptr)
	{
		if (Package)
		{
			Package->MarkAsGarbage();
		}
		return false;
	}
	FAssetRegistryModule::AssetCreated(Asset);
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Asset->GetPathName();
	Request.bDiffOnly = true;
	Request.bIncludeAllWritable = false;

	const FAssetDocumentResult Result = Service.Extract(Request);
	FAssetRegistryModule::AssetDeleted(Asset);
	Asset->ClearFlags(RF_Public | RF_Standalone);
	Asset->MarkAsGarbage();
	Package->SetDirtyFlag(false);
	Package->MarkAsGarbage();
	TestTrue(TEXT("Generic extract succeeds for canonical test asset"), Result.IsSuccess());
	TestTrue(TEXT("Generic extract returns payload"), Result.Payload.IsValid());
	if (!Result.Payload.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Meta = nullptr;
	if (Result.Payload->TryGetObjectField(TEXT("_meta"), Meta) && Meta && Meta->IsValid())
	{
		const TSharedPtr<FJsonObject>* Sync = nullptr;
		if ((*Meta)->TryGetObjectField(TEXT("sync"), Sync) && Sync && Sync->IsValid())
		{
			const TSharedPtr<FJsonObject>* Regions = nullptr;
			TestFalse(TEXT("Generic extract does not force sync regions"), (*Sync)->TryGetObjectField(TEXT("regions"), Regions) && Regions && Regions->IsValid() && (*Regions)->Values.Num() > 0);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericCreateTemplateTest,
	"UEAssetDoc.Profile.GenericTemplate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericCreateTemplateTest::RunTest(const FString& Parameters)
{
	IAssetDocumentRuntimeModule* Integration =
		FModuleManager::LoadModulePtr<IAssetDocumentRuntimeModule>(
			TEXT("AssetDocumentIntegration"));
	TestNotNull(TEXT("generic Template loads the Integration runtime owner"), Integration);
	TestTrue(
		TEXT("generic Template uses the active Integration runtime"),
		Integration && Integration->HasActiveRuntimeRegistration());
	if (!Integration || !Integration->HasActiveRuntimeRegistration())
	{
		return false;
	}
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	FAssetDocumentTemplateRequest Request;
	Request.Class = TEXT("/Script/AssetDocumentTests.TestActorBase");
	Request.Target = TEXT("/Game/AssetDocumentTests/DA_Template.DA_Template");

	const FAssetDocumentResult Result = Service.CreateTemplate(Request);
	TestTrue(TEXT("CreateTemplate succeeds for reflected TestActorBase class"), Result.IsSuccess());
	TestTrue(TEXT("CreateTemplate returns a payload"), Result.Payload.IsValid());

	if (!Result.Payload.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("Template schema version is canonical"), static_cast<int32>(Result.Payload->GetNumberField(TEXT("SchemaVersion"))), 1);
	TestEqual(TEXT("Template target is normalized to package path"), Result.Payload->GetStringField(TEXT("Target")), FString(TEXT("/Game/AssetDocumentTests/DA_Template")));
	TestEqual(TEXT("Result target is normalized to package path"), Result.Target, FString(TEXT("/Game/AssetDocumentTests/DA_Template")));
	TestEqual(TEXT("Template class is canonical"), Result.Payload->GetStringField(TEXT("Class")), FString(TEXT("/Script/AssetDocumentTests.TestActorBase")));
	TestEqual(TEXT("Template action is CreateOrUpdate"), Result.Payload->GetStringField(TEXT("Action")), FString(TEXT("CreateOrUpdate")));
	TestFalse(TEXT("Generic template does not include AssetType"), Result.Payload->HasField(TEXT("AssetType")));

	const TSharedPtr<FJsonObject>* Definitions = nullptr;
	TestTrue(TEXT("Template includes Definitions object"), Result.Payload->TryGetObjectField(TEXT("Definitions"), Definitions));
	if (Definitions && Definitions->IsValid())
	{
		TestEqual(TEXT("Template Definitions starts empty"), (*Definitions)->Values.Num(), 0);
	}

	const TSharedPtr<FJsonObject>* Properties = nullptr;
	TestTrue(TEXT("Template includes Properties object"), Result.Payload->TryGetObjectField(TEXT("Properties"), Properties));
	if (Properties && Properties->IsValid())
	{
		TestEqual(TEXT("Template Properties starts empty"), (*Properties)->Values.Num(), 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericCreateTemplateRejectsInvalidTargetTest,
	"UEAssetDoc.Profile.GenericTemplateRejectsInvalidTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericCreateTemplateRejectsInvalidTargetTest::RunTest(const FString& Parameters)
{
	const FAssetDocumentService Service;
	FAssetDocumentTemplateRequest Request;
	Request.Class = TEXT("/Script/AssetDocumentTests.TestDataAsset");
	Request.Target = TEXT("AssetDocumentTests/DA_InvalidTarget");

	const FAssetDocumentResult Result = Service.CreateTemplate(Request);
	TestFalse(TEXT("CreateTemplate rejects non-/Game target"), Result.IsSuccess());
	TestTrue(TEXT("CreateTemplate reports long package name error"), Result.Message.Contains(TEXT("must be a long package name under /Game")));
	TestEqual(TEXT("Invalid result target is normalized request target"), Result.Target, Request.Target);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericSchemaTest,
	"UEAssetDoc.Profile.Schema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericSchemaTest::RunTest(const FString& Parameters)
{
	const FAssetDocumentService Service;

	const FAssetDocumentResult Result = Service.GetSchema();
	TestTrue(TEXT("GetSchema succeeds"), Result.IsSuccess());
	TestTrue(TEXT("GetSchema returns a payload"), Result.Payload.IsValid());

	if (!Result.Payload.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("Schema version is 1"), static_cast<int32>(Result.Payload->GetNumberField(TEXT("schema_version"))), 1);

	const TArray<TSharedPtr<FJsonValue>>* RegionPolicyPresets = nullptr;
	TestTrue(TEXT("Schema includes RegionPolicyPresets"), Result.Payload->TryGetArrayField(TEXT("RegionPolicyPresets"), RegionPolicyPresets));
	if (RegionPolicyPresets)
	{
		const TSharedPtr<FJsonObject> DefaultDiffPreset = FindProfileObjectByStringField(*RegionPolicyPresets, TEXT("PresetName"), TEXT("DefaultDiff"));
		TestTrue(TEXT("Schema includes DefaultDiff policy preset"), DefaultDiffPreset.IsValid());
		if (DefaultDiffPreset.IsValid())
		{
			const TSharedPtr<FJsonObject>* Defaults = nullptr;
			TestTrue(TEXT("DefaultDiff preset includes Defaults"), DefaultDiffPreset->TryGetObjectField(TEXT("Defaults"), Defaults));
			if (Defaults && Defaults->IsValid())
			{
				TestEqual(TEXT("DefaultDiff preset reducer is exported"), (*Defaults)->GetStringField(TEXT("ReducerMode")), FString(TEXT("DefaultDiff")));
				TestEqual(TEXT("DefaultDiff preset apply mode is exported"), (*Defaults)->GetStringField(TEXT("ApplyMode")), FString(TEXT("SetProperty")));
				TestProfileHasNoAgentFacingOperationFields(this, TEXT("DefaultDiff preset"), *Defaults);
			}
		}

		TestTrue(TEXT("Schema includes ManagedRegion policy preset"), FindProfileObjectByStringField(*RegionPolicyPresets, TEXT("PresetName"), TEXT("ManagedRegion")).IsValid());
		TestTrue(TEXT("Schema includes ExtensionHook policy preset"), FindProfileObjectByStringField(*RegionPolicyPresets, TEXT("PresetName"), TEXT("ExtensionHook")).IsValid());
	}

	const TSharedPtr<FJsonObject>* FieldNaming = nullptr;
	TestTrue(TEXT("Schema includes field_naming"), Result.Payload->TryGetObjectField(TEXT("field_naming"), FieldNaming));
	if (FieldNaming && FieldNaming->IsValid())
	{
		TestTrue(TEXT("Schema bans abbreviations"), (*FieldNaming)->GetBoolField(TEXT("ban_abbreviations")));
		TestTrue(TEXT("Schema uses UE stable field names"), (*FieldNaming)->GetBoolField(TEXT("use_ue_stable_field_names")));
	}

	return true;
}

#endif
