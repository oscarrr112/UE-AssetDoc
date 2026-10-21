// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "AssetDocumentSidecar.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentManagedPropertyPartitionTests
{
struct FManagedPropertyCase
{
	const TCHAR* ClassPath;
	const TCHAR* PropertyName;
	TSharedPtr<FJsonValue> Value;
};

FString MakeUniqueTarget(const TCHAR* AssetStem)
{
	return FString::Printf(
		TEXT("/Game/AssetDocumentTests/Task7a/%s_%s"),
		AssetStem,
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

FString ToObjectPath(const FString& Target)
{
	return FString::Printf(TEXT("%s.%s"), *Target, *FPackageName::GetLongPackageAssetName(Target));
}

void CleanupAsset(const FString& Target)
{
	const FString ObjectPath = ToObjectPath(Target);
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(Target, FPackageName::GetAssetPackageExtension());
	const FString SidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(Target);
	UObject* ExistingAsset = FindObject<UObject>(nullptr, *ObjectPath);
	if (!ExistingAsset && IFileManager::Get().FileExists(*PackageFileName))
	{
		ExistingAsset = LoadObject<UObject>(nullptr, *ObjectPath);
	}

	if (ExistingAsset)
	{
		TArray<UObject*> ObjectsToDelete{ExistingAsset};
		ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete);
	}

	IFileManager::Get().Delete(*PackageFileName, false, true);
	IFileManager::Get().Delete(*SidecarPath, false, true);
	IFileManager::Get().DeleteDirectory(*FPaths::GetPath(SidecarPath), false, false);
}

FAssetDocumentResult ApplyDocument(FAssetDocumentService& Service, const TSharedPtr<FJsonObject>& Document)
{
	if (!Document.IsValid())
	{
		return FAssetDocumentResult::Failure(TEXT("ApplyDocument requires a valid document"));
	}

	FString Target;
	if (!Document->TryGetStringField(TEXT("Target"), Target) || Target.IsEmpty())
	{
		return FAssetDocumentResult::Failure(TEXT("ApplyDocument requires a non-empty Target"));
	}

	const FString SidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(Target);
	const FString SidecarDirectory = FPaths::GetPath(SidecarPath);
	if (SidecarPath.IsEmpty() || SidecarDirectory.IsEmpty() || !IFileManager::Get().MakeDirectory(*SidecarDirectory, true))
	{
		return FAssetDocumentResult::Failure(TEXT("ApplyDocument failed to create canonical sidecar directory"));
	}

	FString WriteError;
	if (!FAssetDocumentSidecar::WriteJsonFile(SidecarPath, Document, WriteError))
	{
		return FAssetDocumentResult::Failure(FString::Printf(
			TEXT("ApplyDocument failed to write canonical sidecar: %s"),
			*WriteError));
	}

	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = SidecarPath;
	Request.bSaveAsset = true;
	return Service.ApplyFile(Request);
}

TSharedPtr<FJsonObject> MakeDocument(
	const FString& Target,
	const FString& ClassPath,
	const FString& Action = TEXT("Create"))
{
	TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
	Document->SetNumberField(TEXT("SchemaVersion"), 1);
	Document->SetStringField(TEXT("AssetType"), TEXT("GenericAsset"));
	Document->SetStringField(TEXT("Target"), Target);
	Document->SetStringField(TEXT("Class"), ClassPath);
	Document->SetStringField(TEXT("Action"), Action);
	Document->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
	return Document;
}

const FAssetDocumentDiagnostic* FindDiagnostic(
	const FAssetDocumentResult& Result,
	const FString& Path,
	const FString& Code)
{
	return Result.Diagnostics.FindByPredicate(
		[&Path, &Code](const FAssetDocumentDiagnostic& Diagnostic)
		{
			return Diagnostic.Path == Path && Diagnostic.Code == Code;
		});
}

bool HasNamedRow(
	const TSharedPtr<FJsonObject>& Payload,
	const FString& ArrayField,
	const FString& PropertyName)
{
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (!Payload.IsValid() || !Payload->TryGetArrayField(ArrayField, Rows) || !Rows)
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& RowValue : *Rows)
	{
		const TSharedPtr<FJsonObject> Row = RowValue.IsValid() ? RowValue->AsObject() : nullptr;
		FString Name;
		if (Row.IsValid() && Row->TryGetStringField(TEXT("name"), Name) && Name == PropertyName)
		{
			return true;
		}
	}
	return false;
}

TSharedPtr<FJsonObject> GetObjectField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
{
	const TSharedPtr<FJsonObject>* Value = nullptr;
	return Object.IsValid() && Object->TryGetObjectField(FieldName, Value) && Value ? *Value : nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentManagedPropertiesRejectBodyOwnedPropertiesTest,
	"UEAssetDoc.ManagedProperties.RejectsBodyOwnedProperties",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentManagedPropertiesRejectBodyOwnedPropertiesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentManagedPropertyPartitionTests;

	const TArray<FManagedPropertyCase> Cases = {
		{TEXT("/Script/AIModule.BlackboardData"), TEXT("Parent"), MakeShared<FJsonValueNull>()},
		{TEXT("/Script/AIModule.BlackboardData"), TEXT("Keys"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>())},
		{TEXT("/Script/AIModule.BehaviorTree"), TEXT("BlackboardAsset"), MakeShared<FJsonValueNull>()},
		{TEXT("/Script/AIModule.BehaviorTree"), TEXT("BTGraph"), MakeShared<FJsonValueNull>()},
		{TEXT("/Script/AIModule.BehaviorTree"), TEXT("RootNode"), MakeShared<FJsonValueNull>()},
		{TEXT("/Script/AIModule.BehaviorTree"), TEXT("RootDecorators"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>())},
		{TEXT("/Script/AIModule.BehaviorTree"), TEXT("RootDecoratorOps"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>())},
		{TEXT("/Script/AIModule.BlackboardData"), TEXT("parent"), MakeShared<FJsonValueNull>()},
	};

	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	for (int32 CaseIndex = 0; CaseIndex < Cases.Num(); ++CaseIndex)
	{
		const FManagedPropertyCase& TestCase = Cases[CaseIndex];
		const FString Target = MakeUniqueTarget(*FString::Printf(TEXT("Rejected_%d"), CaseIndex));
		ON_SCOPE_EXIT
		{
			CleanupAsset(Target);
		};
		TSharedPtr<FJsonObject> Document = MakeDocument(Target, TestCase.ClassPath);
		Document->GetObjectField(TEXT("Properties"))->SetField(TestCase.PropertyName, TestCase.Value);

		FAssetDocumentValidateRequest ValidateRequest;
		ValidateRequest.Document = Document;
		const FAssetDocumentResult ValidateResult = Service.Validate(ValidateRequest);

		FAssetDocumentDiffRequest DiffRequest;
		DiffRequest.Document = Document;
		const FAssetDocumentResult DiffResult = Service.Diff(DiffRequest);

		const FAssetDocumentResult ApplyResult = ApplyDocument(Service, Document);

		const FString ExpectedPath = FString::Printf(TEXT("/Properties/%s"), TestCase.PropertyName);
		const FString Context = FString::Printf(TEXT("%s.%s"), TestCase.ClassPath, TestCase.PropertyName);
		TestFalse(*FString::Printf(TEXT("Validate rejects %s"), *Context), ValidateResult.IsSuccess());
		TestFalse(*FString::Printf(TEXT("Diff rejects %s"), *Context), DiffResult.IsSuccess());
		TestFalse(*FString::Printf(TEXT("Apply rejects %s"), *Context), ApplyResult.IsSuccess());

		const FAssetDocumentDiagnostic* ValidateDiagnostic =
			FindDiagnostic(ValidateResult, ExpectedPath, TEXT("BodyOwnedProperty"));
		const FAssetDocumentDiagnostic* DiffDiagnostic =
			FindDiagnostic(DiffResult, ExpectedPath, TEXT("BodyOwnedProperty"));
		const FAssetDocumentDiagnostic* ApplyDiagnostic =
			FindDiagnostic(ApplyResult, ExpectedPath, TEXT("BodyOwnedProperty"));
		TestNotNull(*FString::Printf(TEXT("Validate reports exact managed-property diagnostic for %s"), *Context), ValidateDiagnostic);
		TestNotNull(*FString::Printf(TEXT("Diff reports exact managed-property diagnostic for %s"), *Context), DiffDiagnostic);
		TestNotNull(*FString::Printf(TEXT("Apply reports exact managed-property diagnostic for %s"), *Context), ApplyDiagnostic);
		if (ValidateDiagnostic && DiffDiagnostic && ApplyDiagnostic)
		{
			TestEqual(*FString::Printf(TEXT("Diff diagnostic matches Validate for %s"), *Context), DiffDiagnostic->Message, ValidateDiagnostic->Message);
			TestEqual(*FString::Printf(TEXT("Apply diagnostic matches Validate for %s"), *Context), ApplyDiagnostic->Message, ValidateDiagnostic->Message);
		}

		TestNull(*FString::Printf(TEXT("Rejected Apply creates no asset for %s"), *Context), FindObject<UObject>(nullptr, *ToObjectPath(Target)));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentManagedPropertiesPartitionReadSurfacesTest,
	"UEAssetDoc.ManagedProperties.PartitionsInspectAndExtract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentManagedPropertiesPartitionReadSurfacesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentManagedPropertyPartitionTests;

	struct FProfileCase
	{
		const TCHAR* ClassPath;
		const TCHAR* AssetStem;
		UClass* AssetClass;
		TArray<FString> ManagedProperties;
	};
	const TArray<FProfileCase> Cases = {
		{TEXT("/Script/AIModule.BlackboardData"), TEXT("BB_ReadPartition"), UBlackboardData::StaticClass(), {TEXT("Parent"), TEXT("Keys")}},
		{TEXT("/Script/AIModule.BehaviorTree"), TEXT("BT_ReadPartition"), UBehaviorTree::StaticClass(), {
			TEXT("BlackboardAsset"),
			TEXT("BTGraph"),
			TEXT("RootNode"),
			TEXT("RootDecorators"),
			TEXT("RootDecoratorOps"),
		}},
	};

	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	for (const FProfileCase& TestCase : Cases)
	{
		FAssetDocumentInspectRequest InspectRequest;
		InspectRequest.ClassOrAsset = TestCase.ClassPath;
		const FAssetDocumentResult InspectResult = Service.Inspect(InspectRequest);
		TestTrue(*FString::Printf(TEXT("Inspect succeeds for %s"), TestCase.ClassPath), InspectResult.IsSuccess());
		for (const FString& PropertyName : TestCase.ManagedProperties)
		{
			TestFalse(
				*FString::Printf(TEXT("Inspect properties omit Body-owned %s.%s"), TestCase.ClassPath, *PropertyName),
				HasNamedRow(InspectResult.Payload, TEXT("properties"), PropertyName));
			TestFalse(
				*FString::Printf(TEXT("Inspect skipped rows omit Body-owned %s.%s"), TestCase.ClassPath, *PropertyName),
				HasNamedRow(InspectResult.Payload, TEXT("skipped"), PropertyName));
		}

		const FString Target = MakeUniqueTarget(TestCase.AssetStem);
		UPackage* Package = CreatePackage(*Target);
		UObject* Asset = NewObject<UObject>(
			Package,
			TestCase.AssetClass,
			*FPackageName::GetLongPackageAssetName(Target),
			RF_Public | RF_Standalone);
		TestNotNull(*FString::Printf(TEXT("Creates read partition fixture for %s"), TestCase.ClassPath), Asset);

		FAssetDocumentExtractRequest ExtractRequest;
		ExtractRequest.AssetPath = Target;
		ExtractRequest.bIncludeAllWritable = true;
		ExtractRequest.bDiffOnly = false;
		const FAssetDocumentResult ExtractResult = Service.Extract(ExtractRequest);
		TestTrue(*FString::Printf(TEXT("include-all Extract succeeds for %s"), TestCase.ClassPath), ExtractResult.IsSuccess());
		const TSharedPtr<FJsonObject> ExtractedProperties = GetObjectField(ExtractResult.Payload, TEXT("Properties"));
		const TSharedPtr<FJsonObject> ExtractedBody = GetObjectField(ExtractResult.Payload, TEXT("Body"));
		TestTrue(*FString::Printf(TEXT("Extract includes structured Body for %s"), TestCase.ClassPath), ExtractedBody.IsValid());
		for (const FString& PropertyName : TestCase.ManagedProperties)
		{
			TestFalse(
				*FString::Printf(TEXT("include-all Extract omits Body-owned %s.%s from Properties"), TestCase.ClassPath, *PropertyName),
				ExtractedProperties.IsValid() && ExtractedProperties->HasField(PropertyName));
		}

		CleanupAsset(Target);
	}

	return true;
}

#endif
