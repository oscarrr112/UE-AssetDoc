// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentEditorSync.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentRuntimeModule.h"
#include "AssetDocumentService.h"
#include "AssetDocumentSidecar.h"

#include "AssetDocumentPropertyAdapter.h"
#include "TestDataAsset.h"

#include "Dom/JsonValue.h"
#include "ObjectTools.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
TSharedPtr<FJsonObject> MakeGenericAssetDocument(const FString& Target)
{
	TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
	Document->SetNumberField(TEXT("SchemaVersion"), 1);
	Document->SetStringField(TEXT("AssetType"), TEXT("GenericAsset"));
	if (!Target.IsEmpty())
	{
		Document->SetStringField(TEXT("Target"), Target);
	}
	return Document;
}

TSharedPtr<FJsonObject> MakeApplyDocument(const FString& Target, const FString& ClassName, const FString& Action)
{
	TSharedPtr<FJsonObject> Document = MakeGenericAssetDocument(Target);
	Document->SetStringField(TEXT("Class"), ClassName);
	Document->SetStringField(TEXT("Action"), Action);
	Document->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
	return Document;
}

void SetProperty(TSharedPtr<FJsonObject> Document, const FString& PropertyName, TSharedPtr<FJsonValue> Value)
{
	TSharedPtr<FJsonObject> Properties = Document->GetObjectField(TEXT("Properties"));
	Properties->SetField(PropertyName, Value);
}

FString AssetDocumentServiceTestGetObjectPath(const FString& Target)
{
	return FString::Printf(TEXT("%s.%s"), *Target, *FPackageName::GetLongPackageAssetName(Target));
}

void CleanupTestAsset(const FString& Target)
{
	const FString ObjectPath = AssetDocumentServiceTestGetObjectPath(Target);
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(Target, FPackageName::GetAssetPackageExtension());
	const FString SidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(Target);
	TUniquePtr<FAssetDocumentEditorSync::FScopedSidecarWrite> SidecarWriteGuard;
	if (!SidecarPath.IsEmpty())
	{
		SidecarWriteGuard =
			MakeUnique<FAssetDocumentEditorSync::FScopedSidecarWrite>(
				SidecarPath);
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}

	UObject* ExistingAsset = FindObject<UObject>(nullptr, *ObjectPath);
	if (!ExistingAsset && IFileManager::Get().FileExists(*PackageFileName))
	{
		ExistingAsset = LoadObject<UObject>(nullptr, *ObjectPath);
	}

	if (ExistingAsset)
	{
		TArray<UObject*> ObjectsToDelete;
		ObjectsToDelete.Add(ExistingAsset);
		ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete);
	}

	if (IFileManager::Get().FileExists(*PackageFileName))
	{
		IFileManager::Get().Delete(*PackageFileName, false, true);
	}
}

class FScopedCanonicalSidecarFixture
{
public:
	explicit FScopedCanonicalSidecarFixture(const FString& InTarget)
		: Target(InTarget)
		, SidecarPath(FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(InTarget))
	{
	}

	~FScopedCanonicalSidecarFixture()
	{
		CleanupTestAsset(Target);
	}

	FAssetDocumentResult Apply(FAssetDocumentService& Service, const TSharedPtr<FJsonObject>& Document) const
	{
		if (SidecarPath.IsEmpty() || !IFileManager::Get().MakeDirectory(*FPaths::GetPath(SidecarPath), true))
		{
			return FAssetDocumentResult::Failure(TEXT("ApplyFile failed to create canonical sidecar directory"));
		}

		FString WriteError;
		{
			FAssetDocumentEditorSync::FScopedSidecarWrite Guard(SidecarPath);
			if (!FAssetDocumentSidecar::WriteJsonFile(SidecarPath, Document, WriteError))
			{
				return FAssetDocumentResult::Failure(FString::Printf(
					TEXT("ApplyFile failed to write canonical sidecar: %s"),
					*WriteError));
			}
		}

		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = SidecarPath;
		Request.bSaveAsset = true;
		return Service.ApplyFile(Request);
	}

private:
	FString Target;
	FString SidecarPath;
};

FAssetDocumentService* RequireActiveAssetDocumentService(
	FAutomationTestBase& Test)
{
	IAssetDocumentRuntimeModule* Python =
		FModuleManager::LoadModulePtr<IAssetDocumentRuntimeModule>(
			TEXT("AssetDocumentPython"));
	IAssetDocumentRuntimeModule* Integration =
		FModuleManager::LoadModulePtr<IAssetDocumentRuntimeModule>(
			TEXT("AssetDocumentIntegration"));
	Test.TestNotNull(
		TEXT("Loads the AssetDocument Python host module"),
		Python);
	Test.TestNotNull(
		TEXT("Loads the AssetDocument Integration runtime owner"),
		Integration);
	Test.TestTrue(
		TEXT("Uses the active AssetDocument Integration runtime"),
		Integration && Integration->HasActiveRuntimeRegistration());
	return Python && Integration
			&& Integration->HasActiveRuntimeRegistration()
			? &FAssetDocumentModule::Get().GetService()
			: nullptr;
}

FString DescribeResult(const FAssetDocumentResult& Result)
{
	FString Description = Result.Message;
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Description += FString::Printf(
			TEXT(" [%s %s: %s]"),
			*Diagnostic.Path,
			*Diagnostic.Code,
			*Diagnostic.Message);
	}
	return Description;
}

bool HasDiagnosticCode(
	const FAssetDocumentResult& Result,
	const TCHAR* Code)
{
	return Result.Diagnostics.ContainsByPredicate(
		[Code](const FAssetDocumentDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code;
		});
}

TSharedPtr<FJsonObject> AssetDocumentServiceTestFindObjectByStringField(const TArray<TSharedPtr<FJsonValue>>& Values, const FString& FieldName, const FString& FieldValue)
{
	for (const TSharedPtr<FJsonValue>& Value : Values)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (Object.IsValid() && Object->GetStringField(FieldName) == FieldValue)
		{
			return Object;
		}
	}
	return nullptr;
}

bool HasArrayField(TSharedPtr<FJsonObject> Object, const FString& FieldName)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	return Object.IsValid() && Object->TryGetArrayField(FieldName, Values) && Values;
}

TSharedPtr<FJsonObject> GetPayloadObject(TSharedPtr<FJsonObject> Payload, const FString& FieldName)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (Payload.IsValid() && Payload->TryGetObjectField(FieldName, Object) && Object)
	{
		return *Object;
	}
	return nullptr;
}

bool WriteJsonObjectToFile(TSharedPtr<FJsonObject> Document, const FString& FilePath)
{
	FString JsonText;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
	if (!FJsonSerializer::Serialize(Document.ToSharedRef(), Writer))
	{
		return false;
	}
	return FFileHelper::SaveStringToFile(JsonText, *FilePath);
}

FString MakeFullNormalizedPath(const FString& FilePath)
{
	FString NormalizedPath = FPaths::ConvertRelativePathToFull(FilePath);
	FPaths::NormalizeFilename(NormalizedPath);
	return NormalizedPath;
}

struct FScopedAdditionalPropertyFlags
{
	FProperty* Property = nullptr;
	EPropertyFlags Flags = CPF_None;
	uint64 OriginalMaskedFlags = 0;

	FScopedAdditionalPropertyFlags(FProperty* InProperty, EPropertyFlags InFlags)
		: Property(InProperty)
		, Flags(InFlags)
	{
		if (Property)
		{
			OriginalMaskedFlags = Property->GetPropertyFlags() & Flags;
			Property->SetPropertyFlags(Flags);
		}
	}

	~FScopedAdditionalPropertyFlags()
	{
		if (Property)
		{
			Property->ClearPropertyFlags(Flags);
			Property->SetPropertyFlags(static_cast<EPropertyFlags>(OriginalMaskedFlags));
		}
	}
};

struct FScopedClearedPropertyFlags
{
	FProperty* Property = nullptr;
	EPropertyFlags Flags = CPF_None;
	uint64 OriginalMaskedFlags = 0;

	FScopedClearedPropertyFlags(FProperty* InProperty, EPropertyFlags InFlags)
		: Property(InProperty)
		, Flags(InFlags)
	{
		if (Property)
		{
			OriginalMaskedFlags = Property->GetPropertyFlags() & Flags;
			Property->ClearPropertyFlags(Flags);
		}
	}

	~FScopedClearedPropertyFlags()
	{
		if (Property)
		{
			Property->ClearPropertyFlags(Flags);
			Property->SetPropertyFlags(static_cast<EPropertyFlags>(OriginalMaskedFlags));
		}
	}
};

FString GetTestSidecarPath()
{
	return MakeFullNormalizedPath(
		FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Data/DA_Test.assetdoc.json")));
}

FString MakeUniqueTestFolderName()
{
	return FString::Printf(TEXT("AssetDocumentSidecarTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

// Generic-route failures carry a fixed summary message; the detail lives in the diagnostics.
bool ResultMentions(const FAssetDocumentResult& Result, const TCHAR* Text)
{
	if (Result.Message.Contains(Text))
	{
		return true;
	}
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		if (Diagnostic.Path.Contains(Text) || Diagnostic.Code.Contains(Text) || Diagnostic.Message.Contains(Text))
		{
			return true;
		}
	}
	return false;
}

FString MakeSidecarJson(const FString& Target)
{
	return FString::Printf(
		TEXT("{\n")
		TEXT("\t\"SchemaVersion\": 1,\n")
		TEXT("\t\"AssetType\": \"GenericAsset\",\n")
		TEXT("\t\"Target\": \"%s\",\n")
		TEXT("\t\"Class\": \"/Script/AssetDocumentTests.TestDataAsset\",\n")
		TEXT("\t\"Action\": \"CreateOrUpdate\"\n")
		TEXT("}\n"),
		*Target);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentReadTest,
	"UEAssetDoc.Read",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentReadTest::RunTest(const FString& Parameters)
{
	const FString Target = TEXT("/Game/AssetDocumentTests/DA_ReadSide");
	const FString MissingTarget = TEXT("/Game/AssetDocumentTests/DA_ReadMissing");
	const FString ObjectPath = AssetDocumentServiceTestGetObjectPath(Target);
	const FString SidecarPath = FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(Target);
	CleanupTestAsset(Target);

	FAssetDocumentService* ActiveService =
		RequireActiveAssetDocumentService(*this);
	if (!ActiveService)
	{
		return false;
	}
	FAssetDocumentService& Service = *ActiveService;
	FScopedCanonicalSidecarFixture SidecarFixture(Target);

	TSharedPtr<FJsonObject> CreateDocument = MakeApplyDocument(Target, TEXT("/Script/AssetDocumentTests.TestDataAsset"), TEXT("Create"));
	SetProperty(CreateDocument, TEXT("TestString"), MakeShared<FJsonValueString>(TEXT("read-side")));
	SetProperty(CreateDocument, TEXT("TestInt"), MakeShared<FJsonValueNumber>(12));

	const FAssetDocumentResult CreateResult = SidecarFixture.Apply(Service, CreateDocument);
	TestTrue(TEXT("Creates fixture asset for read-side tests"), CreateResult.IsSuccess());

	UPackage* Package = FindPackage(nullptr, *Target);
	UTestDataAsset* Asset = LoadObject<UTestDataAsset>(nullptr, *ObjectPath);
	TestNotNull(TEXT("Read-side fixture asset loads"), Asset);
	FProperty* TestStringProperty = FindFProperty<FProperty>(UTestDataAsset::StaticClass(), GET_MEMBER_NAME_CHECKED(UTestDataAsset, TestString));
	FProperty* TestFloatProperty = FindFProperty<FProperty>(UTestDataAsset::StaticClass(), GET_MEMBER_NAME_CHECKED(UTestDataAsset, TestFloat));
	if (!Package && Asset)
	{
		Package = Asset->GetOutermost();
	}
	if (Asset)
	{
		Asset->TestFloat = 7.25f;
	}
	if (Package)
	{
		Package->ClearDirtyFlag();
	}

	{
		FAssetDocumentInspectRequest Request;
		Request.ClassOrAsset = Target;

		const FAssetDocumentResult Result = Service.Inspect(Request);

		TestTrue(TEXT("Inspect succeeds for a TestDataAsset asset path"), Result.IsSuccess());
		TestTrue(TEXT("Inspect returns payload"), Result.Payload.IsValid());
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		TestTrue(TEXT("Inspect payload has properties array"), Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("properties"), Properties));
		TestTrue(TEXT("Inspect payload has skipped array"), HasArrayField(Result.Payload, TEXT("skipped")));
		if (Properties)
		{
			TSharedPtr<FJsonObject> StringRow = AssetDocumentServiceTestFindObjectByStringField(*Properties, TEXT("name"), TEXT("TestString"));
			TestTrue(TEXT("Inspect includes TestString row"), StringRow.IsValid());
			if (StringRow.IsValid())
			{
				TestEqual(TEXT("Inspect row has name"), StringRow->GetStringField(TEXT("name")), FString(TEXT("TestString")));
				TestFalse(TEXT("Inspect row has ue_type"), StringRow->GetStringField(TEXT("ue_type")).IsEmpty());
				TestEqual(TEXT("Inspect row has type_token"), StringRow->GetStringField(TEXT("type_token")), FString(TEXT("String")));
				TestTrue(TEXT("Inspect row has current_value"), StringRow->HasField(TEXT("current_value")));
				TestTrue(TEXT("Inspect row has default_value"), StringRow->HasField(TEXT("default_value")));
				TestTrue(TEXT("Inspect row is writable"), StringRow->GetBoolField(TEXT("writable")));
				TestEqual(TEXT("Inspect current value comes from asset"), StringRow->GetStringField(TEXT("current_value")), FString(TEXT("read-side")));
				TestEqual(TEXT("Inspect default value comes from CDO"), StringRow->GetStringField(TEXT("default_value")), FString());
			}

			TSharedPtr<FJsonObject> IntRow = AssetDocumentServiceTestFindObjectByStringField(*Properties, TEXT("name"), TEXT("TestInt"));
			TestTrue(TEXT("Inspect includes TestInt row"), IntRow.IsValid());
			if (IntRow.IsValid())
			{
				TestEqual(TEXT("Inspect maps integer token"), IntRow->GetStringField(TEXT("type_token")), FString(TEXT("Int")));
			}
		}
		TestFalse(TEXT("Inspect does not dirty asset package"), Package && Package->IsDirty());
	}

	{
		TestTrue(TEXT("Adapter treats normal editable property as writable"), FAssetDocumentPropertyAdapter::IsWritableProperty(TestStringProperty));
		{
			FScopedAdditionalPropertyFlags EditConstScope(TestStringProperty, CPF_EditConst);
			TestFalse(TEXT("Adapter treats EditConst property as non-writable"), FAssetDocumentPropertyAdapter::IsWritableProperty(TestStringProperty));
			TestEqual(TEXT("Adapter reports EditConst reason"), FAssetDocumentPropertyAdapter::GetNonWritableReason(TestStringProperty), FString(TEXT("edit-const")));
		}
		{
			FScopedAdditionalPropertyFlags TransientScope(TestStringProperty, CPF_Transient);
			TestFalse(TEXT("Adapter treats transient property as non-writable"), FAssetDocumentPropertyAdapter::IsWritableProperty(TestStringProperty));
			TestEqual(TEXT("Adapter reports transient reason"), FAssetDocumentPropertyAdapter::GetNonWritableReason(TestStringProperty), FString(TEXT("transient")));
		}
		{
			FScopedAdditionalPropertyFlags DeprecatedScope(TestStringProperty, CPF_Deprecated);
			TestFalse(TEXT("Adapter treats deprecated property as non-writable"), FAssetDocumentPropertyAdapter::IsWritableProperty(TestStringProperty));
			TestEqual(TEXT("Adapter reports deprecated reason"), FAssetDocumentPropertyAdapter::GetNonWritableReason(TestStringProperty), FString(TEXT("deprecated")));
		}
		{
			FScopedClearedPropertyFlags MissingEditScope(TestStringProperty, CPF_Edit);
			TestFalse(TEXT("Adapter treats property without CPF_Edit as non-writable"), FAssetDocumentPropertyAdapter::IsWritableProperty(TestStringProperty));
			TestEqual(TEXT("Adapter reports non-editable reason"), FAssetDocumentPropertyAdapter::GetNonWritableReason(TestStringProperty), FString(TEXT("non-editable")));
		}
		TestTrue(TEXT("Adapter restores test property writability after scoped flag changes"), FAssetDocumentPropertyAdapter::IsWritableProperty(TestStringProperty));
	}

	{
		FAssetDocumentExtractRequest Request;
		Request.AssetPath = Target;
		Request.bDiffOnly = true;

		FAssetDocumentResult Result;
		{
			FScopedAdditionalPropertyFlags EditConstScope(TestFloatProperty, CPF_EditConst);
			Result = Service.Extract(Request);
		}

		TestTrue(TEXT("Extract succeeds for TestDataAsset"), Result.IsSuccess());
		TestTrue(TEXT("Extract returns draft payload"), Result.Payload.IsValid());
		if (Result.Payload.IsValid())
		{
			TestEqual(TEXT("Extract draft schema version"), Result.Payload->GetNumberField(TEXT("SchemaVersion")), 1.0);
			TestEqual(TEXT("Extract draft asset type"), Result.Payload->GetStringField(TEXT("AssetType")), FString(TEXT("GenericAsset")));
			TestEqual(TEXT("Extract draft target"), Result.Payload->GetStringField(TEXT("Target")), Target);
			TestEqual(TEXT("Extract draft class"), Result.Payload->GetStringField(TEXT("Class")), FString(TEXT("/Script/AssetDocumentTests.TestDataAsset")));
			TestEqual(TEXT("Extract draft action"), Result.Payload->GetStringField(TEXT("Action")), FString(TEXT("CreateOrUpdate")));

			TSharedPtr<FJsonObject> Properties = GetPayloadObject(Result.Payload, TEXT("Properties"));
			TestTrue(TEXT("Extract draft has Properties object"), Properties.IsValid());
			if (Properties.IsValid())
			{
				TestTrue(TEXT("Extract diff-only includes changed string"), Properties->HasField(TEXT("TestString")));
				TestFalse(TEXT("Extract omits EditConst property even when non-default"), Properties->HasField(TEXT("TestFloat")));
			}
		}
		TestFalse(TEXT("Extract does not dirty asset package"), Package && Package->IsDirty());
	}

	{
		const FDateTime TimestampBefore = IFileManager::Get().GetTimeStamp(*FPackageName::LongPackageNameToFilename(Target, FPackageName::GetAssetPackageExtension()));

		FAssetDocumentValidateRequest Request;
		Request.Document = MakeApplyDocument(Target, TEXT("/Script/AssetDocumentTests.TestDataAsset"), TEXT("CreateOrUpdate"));

		const FAssetDocumentResult Result = Service.Validate(Request);

		const FDateTime TimestampAfter = IFileManager::Get().GetTimeStamp(*FPackageName::LongPackageNameToFilename(Target, FPackageName::GetAssetPackageExtension()));
		TestTrue(TEXT("Validate succeeds for inline document"), Result.IsSuccess());
		TestFalse(TEXT("Validate does not dirty asset package"), Package && Package->IsDirty());
		TestEqual(TEXT("Validate does not save asset package"), TimestampAfter, TimestampBefore);
	}

	{
		// Generic Diff reports JSON-pointer rows ({path, kind, before, after}) for the authored
		// properties that differ from the asset, and rejects what Apply would reject.
		auto DiffWith = [&](TFunctionRef<void(FJsonObject&)> AddProperties)
		{
			TSharedPtr<FJsonObject> DiffDocument = MakeGenericAssetDocument(Target);
			DiffDocument->SetStringField(TEXT("Class"), TEXT("/Script/AssetDocumentTests.TestDataAsset"));
			DiffDocument->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));
			TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
			Properties->SetStringField(TEXT("TestString"), TEXT("after-diff"));
			Properties->SetNumberField(TEXT("TestInt"), 12);
			AddProperties(*Properties);
			DiffDocument->SetObjectField(TEXT("Properties"), Properties);
			TestTrue(TEXT("Writes temporary diff sidecar"), WriteJsonObjectToFile(DiffDocument, SidecarPath));
			FAssetDocumentDiffRequest Request;
			Request.FilePath = SidecarPath;
			FScopedAdditionalPropertyFlags EditConstScope(TestFloatProperty, CPF_EditConst);
			return Service.Diff(Request);
		};
		auto HasRow = [](const FAssetDocumentResult& Result, const TCHAR* Path)
		{
			const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
			return Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("changed"), Changed)
				&& AssetDocumentServiceTestFindObjectByStringField(*Changed, TEXT("path"), Path).IsValid();
		};
		auto HasDiagnostic = [](const FAssetDocumentResult& Result, const TCHAR* Path, const TCHAR* Code)
		{
			return Result.Diagnostics.ContainsByPredicate([Path, Code](const FAssetDocumentDiagnostic& Diagnostic)
			{
				return Diagnostic.Path == Path && Diagnostic.Code == Code;
			});
		};

		const FAssetDocumentResult Result = DiffWith([](FJsonObject&) {});
		TestTrue(TEXT("Diff succeeds for TestDataAsset sidecar"), Result.IsSuccess());
		TestTrue(TEXT("Diff payload has changed array"), HasArrayField(Result.Payload, TEXT("changed")));
		TestTrue(TEXT("Diff reports changed property"), HasRow(Result, TEXT("/Properties/TestString")));
		TestFalse(TEXT("Diff omits unchanged property"), HasRow(Result, TEXT("/Properties/TestInt")));

		const FAssetDocumentResult EditConst = DiffWith([](FJsonObject& Properties)
		{
			Properties.SetNumberField(TEXT("TestFloat"), 11.5);
		});
		TestFalse(TEXT("Diff rejects EditConst property"), EditConst.IsSuccess());
		TestTrue(TEXT("Diff names the EditConst property"),
			HasDiagnostic(EditConst, TEXT("/Properties/TestFloat"), TEXT("Generic.NonWritableProperty")));

		const FAssetDocumentResult Missing = DiffWith([](FJsonObject& Properties)
		{
			Properties.SetStringField(TEXT("DefinitelyMissing"), TEXT("missing"));
		});
		TestFalse(TEXT("Diff rejects missing property"), Missing.IsSuccess());
		TestTrue(TEXT("Diff names the missing property"),
			HasDiagnostic(Missing, TEXT("/Properties/DefinitelyMissing"), TEXT("Generic.UnknownProperty")));

		TestFalse(TEXT("Diff does not dirty asset package"), Package && Package->IsDirty());
	}

	IFileManager::Get().Delete(*SidecarPath, false, true);
	CleanupTestAsset(Target);
	CleanupTestAsset(MissingTarget);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentSidecarValidationTest,
	"UEAssetDoc.Sidecar.Validate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSidecarValidationTest::RunTest(const FString& Parameters)
{
	const FString SidecarPath = GetTestSidecarPath();
	const FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();

	{
		FAssetDocumentValidateRequest Request;
		Request.FilePath = SidecarPath;
		Request.Document = MakeApplyDocument(TEXT(""), TEXT("/Script/AssetDocumentTests.TestDataAsset"), TEXT("CreateOrUpdate"));

		const FAssetDocumentResult Result = Service.Validate(Request);

		TestFalse(TEXT("Missing Target fails when validating a sidecar file"), Result.IsSuccess());
		TestTrue(TEXT("Missing Target reports a Target validation error"), ResultMentions(Result, TEXT("Target")));
	}

	{
		FAssetDocumentValidateRequest Request;
		Request.FilePath = SidecarPath;
		Request.Document = MakeApplyDocument(TEXT("/Game/Data/DA_Other"), TEXT("/Script/AssetDocumentTests.TestDataAsset"), TEXT("CreateOrUpdate"));

		const FAssetDocumentResult Result = Service.Validate(Request);

		TestFalse(TEXT("Target mismatch fails when sidecar path resolves to a different object path"), Result.IsSuccess());
		TestTrue(TEXT("Target mismatch reports the expected sidecar object path"), ResultMentions(Result, TEXT("/Game/Data/DA_Test")));
	}

	{
		FAssetDocumentValidateRequest Request;
		Request.FilePath = SidecarPath;
		Request.Document = MakeApplyDocument(TEXT("/Game/Data/DA_Test"), TEXT("/Script/AssetDocumentTests.TestDataAsset"), TEXT("CreateOrUpdate"));

		const FAssetDocumentResult Result = Service.Validate(Request);

		TestTrue(TEXT("Matching Target succeeds for a sidecar file"), Result.IsSuccess());
		TestEqual(TEXT("Matching sidecar result target"), Result.Target, FString(TEXT("/Game/Data/DA_Test")));
		TestTrue(TEXT("Matching sidecar result includes payload"), Result.Payload.IsValid());
		if (Result.Payload.IsValid())
		{
			TestEqual(TEXT("Payload target is normalized"), Result.Payload->GetStringField(TEXT("target")), FString(TEXT("/Game/Data/DA_Test")));
			TestEqual(TEXT("Payload sidecar path is normalized"), Result.Payload->GetStringField(TEXT("sidecar_file_path")), SidecarPath);
		}
	}

	{
		FAssetDocumentValidateRequest Request;
		Request.Document = MakeApplyDocument(TEXT("/Game/Data/DA_Test"), TEXT("TestDataAsset"), TEXT("CreateOrUpdate"));

		const FAssetDocumentResult Result = Service.Validate(Request);

		TestFalse(TEXT("Short class name fails validation"), Result.IsSuccess());
		TestTrue(TEXT("Short class name names the exact class path"),
			ResultMentions(Result, TEXT("/Script/AssetDocumentTests.TestDataAsset")));
	}

	{
		FAssetDocumentValidateRequest Request;
		Request.Document = MakeApplyDocument(TEXT(""), TEXT("/Script/AssetDocumentTests.TestDataAsset"), TEXT("CreateOrUpdate"));

		const FAssetDocumentResult Result = Service.Validate(Request);

		TestFalse(TEXT("Inline document without Target fails validation"), Result.IsSuccess());
		TestTrue(TEXT("Inline document without Target reports Target validation error"), ResultMentions(Result, TEXT("Target")));
	}

	{
		FAssetDocumentValidateRequest Request;
		// AssetType is optional for reflected documents; when present it must be GenericAsset.
		Request.Document = MakeGenericAssetDocument(TEXT("/Game/Data/DA_Test"));
		Request.Document->SetStringField(TEXT("AssetType"), TEXT("LegacyReflected"));
		Request.Document->SetStringField(TEXT("Class"), TEXT("/Script/AssetDocumentTests.TestDataAsset"));
		Request.Document->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));

		const FAssetDocumentResult Result = Service.Validate(Request);

		TestFalse(TEXT("Reflected document with a non-generic AssetType fails validation"), Result.IsSuccess());
		TestTrue(TEXT("Non-generic AssetType reports the AssetType rule"), ResultMentions(Result, TEXT("AssetType must be GenericAsset")));
	}

	{
		FAssetDocumentValidateRequest Request;
		Request.Document = MakeGenericAssetDocument(TEXT("/Game/Data/DA_Test"));
		Request.Document->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));

		const FAssetDocumentResult Result = Service.Validate(Request);

		TestFalse(TEXT("Inline document without Class fails validation"), Result.IsSuccess());
		TestTrue(TEXT("Inline document without Class reports Class validation error"), ResultMentions(Result, TEXT("Class")));
	}

	{
		FAssetDocumentValidateRequest Request;
		Request.Document = MakeGenericAssetDocument(TEXT("/Game/Data/DA_Test"));
		Request.Document->SetStringField(TEXT("Class"), TEXT("/Script/AssetDocumentTests.TestDataAsset"));

		const FAssetDocumentResult Result = Service.Validate(Request);

		TestFalse(TEXT("Inline document without Action fails validation"), Result.IsSuccess());
		TestTrue(TEXT("Inline document without Action reports Action validation error"), ResultMentions(Result, TEXT("Action")));
	}

	{
		FAssetDocumentValidateRequest Request;
		Request.Document = MakeApplyDocument(TEXT("/Game/Data/DA_Test"), TEXT("/Script/AssetDocumentTests.TestDataAsset"), TEXT("CreateOrUpdate"));
		Request.Document->SetStringField(TEXT("Properties"), TEXT("not an object"));

		const FAssetDocumentResult Result = Service.Validate(Request);

		TestFalse(TEXT("Inline document with non-object Properties fails validation"), Result.IsSuccess());
		TestTrue(TEXT("Inline document with non-object Properties reports Properties validation error"), ResultMentions(Result, TEXT("Properties")));
	}

	{
		const FString TestFolder = MakeUniqueTestFolderName();
		const FString Target = FString::Printf(TEXT("/Game/%s/DA_FileLoad"), *TestFolder);
		FString TestDirectory = MakeFullNormalizedPath(FPaths::Combine(FPaths::ProjectContentDir(), TestFolder));
		FString FilePath = FPaths::Combine(TestDirectory, TEXT("DA_FileLoad.assetdoc.json"));
		FPaths::NormalizeFilename(FilePath);

		IFileManager& FileManager = IFileManager::Get();
		TestTrue(TEXT("Creates temporary sidecar test directory"), FileManager.MakeDirectory(*TestDirectory, true));
		TestTrue(TEXT("Writes temporary sidecar document"), FFileHelper::SaveStringToFile(MakeSidecarJson(Target), *FilePath));

		FAssetDocumentValidateRequest Request;
		Request.FilePath = FilePath;

		const FAssetDocumentResult Result = Service.Validate(Request);

		TestTrue(TEXT("Validate loads a sidecar document from disk when Document is null"), Result.IsSuccess());
		TestEqual(TEXT("File-loaded sidecar result target"), Result.Target, Target);
		TestTrue(TEXT("File-loaded sidecar result includes payload"), Result.Payload.IsValid());
		if (Result.Payload.IsValid())
		{
			TestEqual(TEXT("File-loaded payload target is normalized"), Result.Payload->GetStringField(TEXT("target")), Target);
			TestEqual(TEXT("File-loaded payload sidecar path is normalized"), Result.Payload->GetStringField(TEXT("sidecar_file_path")), FilePath);
		}

		TestTrue(TEXT("Cleans up temporary sidecar test directory"), FileManager.DeleteDirectory(*TestDirectory, false, true));
	}

	{
		const FString TestFolder = MakeUniqueTestFolderName();
		FString MissingFilePath = FPaths::Combine(FPaths::ProjectContentDir(), TestFolder, TEXT("Missing.assetdoc.json"));
		FPaths::NormalizeFilename(MissingFilePath);

		FAssetDocumentValidateRequest Request;
		Request.FilePath = MissingFilePath;

		const FAssetDocumentResult Result = Service.Validate(Request);

		TestFalse(TEXT("Validate fails when requested sidecar file is missing"), Result.IsSuccess());
		TestTrue(TEXT("Missing sidecar file reports a read failure"), Result.Message.Contains(TEXT("Failed to read JSON file")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentApplyTest,
	"UEAssetDoc.Apply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentApplyTest::RunTest(const FString& Parameters)
{
	const FString InvalidClassTarget = TEXT("/Game/AssetDocumentTests/DA_InvalidClass");
	const FString AbstractClassTarget = TEXT("/Game/AssetDocumentTests/DA_AbstractClass");
	const FString EmptyInMemoryPackageTarget = TEXT("/Game/AssetDocumentTests/DA_EmptyInMemoryPackage");
	const FString CreateTarget = TEXT("/Game/AssetDocumentTests/DA_CreateSucceeds");
	const FString UpdateTarget = TEXT("/Game/AssetDocumentTests/DA_UpdatePreserves");
	const FString WrongShapeStringTarget = TEXT("/Game/AssetDocumentTests/DA_WrongShapeStringFails");
	const FString FailedWrongShapeRetryTarget = TEXT("/Game/AssetDocumentTests/DA_FailedWrongShapeRetry");
	const FString FailedUnknownPropertyRetryTarget = TEXT("/Game/AssetDocumentTests/DA_FailedUnknownPropertyRetry");
	const FString FailedNonWritableRetryTarget = TEXT("/Game/AssetDocumentTests/DA_FailedNonWritableRetry");
	const FString WrongShapeIntegerTarget = TEXT("/Game/AssetDocumentTests/DA_WrongShapeIntegerFails");
	const TArray<FString> Targets = {
		InvalidClassTarget,
		AbstractClassTarget,
		EmptyInMemoryPackageTarget,
		CreateTarget,
		UpdateTarget,
		WrongShapeStringTarget,
		FailedWrongShapeRetryTarget,
		FailedUnknownPropertyRetryTarget,
		FailedNonWritableRetryTarget,
		WrongShapeIntegerTarget
	};

	for (const FString& Target : Targets)
	{
		CleanupTestAsset(Target);
	}

	FAssetDocumentService* ActiveService =
		RequireActiveAssetDocumentService(*this);
	if (!ActiveService)
	{
		return false;
	}
	FAssetDocumentService& Service = *ActiveService;

	{
		FScopedCanonicalSidecarFixture SidecarFixture(InvalidClassTarget);
		TSharedPtr<FJsonObject> Document = MakeApplyDocument(InvalidClassTarget, TEXT("DefinitelyMissingAssetDocumentClass"), TEXT("Create"));

		const FAssetDocumentResult Result = SidecarFixture.Apply(Service, Document);

		TestFalse(TEXT("Invalid class fails"), Result.IsSuccess());
		TestTrue(TEXT("Invalid class reports class resolution failure"), Result.Message.Contains(TEXT("class"), ESearchCase::IgnoreCase));
	}

	{
		FScopedCanonicalSidecarFixture SidecarFixture(AbstractClassTarget);
		TSharedPtr<FJsonObject> Document = MakeApplyDocument(
			AbstractClassTarget,
			TEXT("/Script/Engine.DataAsset"),
			TEXT("Create"));

		const FAssetDocumentResult Result = SidecarFixture.Apply(Service, Document);

		TestFalse(TEXT("Abstract class fails"), Result.IsSuccess());
		TestTrue(TEXT("Abstract class reports abstract rejection"), Result.Message.Contains(TEXT("abstract"), ESearchCase::IgnoreCase));
	}

	{
		UPackage* EmptyPackage = CreatePackage(*EmptyInMemoryPackageTarget);
		TestNotNull(TEXT("Creates an empty in-memory package placeholder"), EmptyPackage);

		FScopedCanonicalSidecarFixture SidecarFixture(EmptyInMemoryPackageTarget);
		TSharedPtr<FJsonObject> Document = MakeApplyDocument(
			EmptyInMemoryPackageTarget,
			TEXT("/Script/AssetDocumentTests.TestDataAsset"),
			TEXT("Create"));
		const FAssetDocumentResult Result = SidecarFixture.Apply(Service, Document);

		// An empty, unsaved, clean package under the target name (what a failed load of
		// the missing asset leaves) holds no asset: Create replaces it (direction doc §4.5).
		TestTrue(
			FString::Printf(
				TEXT("Create replaces an empty, unsaved, clean in-memory package: %s"),
				*DescribeResult(Result)),
			Result.IsSuccess());
		TestFalse(
			TEXT("Empty-package create reports no target-exists diagnostic"),
			HasDiagnosticCode(Result, TEXT("CutoverPreparation.TargetExists")));
		TestNotNull(
			TEXT("Empty-package create creates the asset"),
			FindObject<UTestDataAsset>(
				nullptr,
				*AssetDocumentServiceTestGetObjectPath(EmptyInMemoryPackageTarget)));
	}

	{
		const FString OccupiedPackageTarget =
			TEXT("/Game/AssetDocumentTests/DA_OccupiedPackage");
		UPackage* OccupiedPackage = CreatePackage(*OccupiedPackageTarget);
		UTestDataAsset* OccupyingAsset = OccupiedPackage
			? NewObject<UTestDataAsset>(
				OccupiedPackage,
				TEXT("DifferentAsset"),
				RF_Public | RF_Standalone)
			: nullptr;
		TestNotNull(TEXT("Creates a package occupied by a different object path"), OccupyingAsset);

		FScopedCanonicalSidecarFixture SidecarFixture(OccupiedPackageTarget);
		TSharedPtr<FJsonObject> Document = MakeApplyDocument(
			OccupiedPackageTarget,
			TEXT("/Script/AssetDocumentTests.TestDataAsset"),
			TEXT("Create"));
		const FAssetDocumentResult Result = SidecarFixture.Apply(Service, Document);

		TestFalse(TEXT("Create rejects an occupied package with no expected object"), Result.IsSuccess());
		TestTrue(
			FString::Printf(
				TEXT("Occupied-package rejection reports the path conflict: %s"),
				*DescribeResult(Result)),
			HasDiagnosticCode(Result, TEXT("CutoverPreparation.TargetExists")));
		TestEqual(
			TEXT("Occupied-package rejection preserves the unrelated object"),
			FindObject<UTestDataAsset>(OccupiedPackage, TEXT("DifferentAsset")),
			OccupyingAsset);
		TestEqual(
			TEXT("Occupied-package rejection preserves the package"),
			FindPackage(nullptr, *OccupiedPackageTarget),
			OccupiedPackage);
		if (OccupyingAsset)
		{
			FAssetDocumentEditorSync::FScopedSidecarWrite Guard(
				FAssetDocumentSidecar::ResolveSidecarPathFromObjectPath(
					OccupiedPackageTarget));
			ObjectTools::DeleteObjects(
				{OccupyingAsset},
				false,
				ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
		}
	}

	{
		FScopedCanonicalSidecarFixture SidecarFixture(CreateTarget);
		TSharedPtr<FJsonObject> Document = MakeApplyDocument(
			CreateTarget,
			TEXT("/Script/AssetDocumentTests.TestDataAsset"),
			TEXT("Create"));
		SetProperty(Document, TEXT("TestString"), MakeShared<FJsonValueString>(TEXT("hello")));
		SetProperty(Document, TEXT("TestInt"), MakeShared<FJsonValueNumber>(42));
		SetProperty(Document, TEXT("TestFloat"), MakeShared<FJsonValueNumber>(3.5));
		SetProperty(Document, TEXT("bTestBool"), MakeShared<FJsonValueBoolean>(true));
		SetProperty(Document, TEXT("TestName"), MakeShared<FJsonValueString>(TEXT("CreatedName")));

		const FAssetDocumentResult Result = SidecarFixture.Apply(Service, Document);

		TestTrue(
			FString::Printf(
				TEXT("Create TestDataAsset succeeds: %s"),
				*DescribeResult(Result)),
			Result.IsSuccess());
		TestTrue(TEXT("Create saves asset"), Result.bSavedAsset);

		UTestDataAsset* Asset = LoadObject<UTestDataAsset>(nullptr, *AssetDocumentServiceTestGetObjectPath(CreateTarget));
		TestNotNull(TEXT("Created TestDataAsset loads"), Asset);
		if (Asset)
		{
			TestEqual(TEXT("Created string property"), Asset->TestString, FString(TEXT("hello")));
			TestEqual(TEXT("Created int property"), Asset->TestInt, 42);
			TestEqual(TEXT("Created float property"), Asset->TestFloat, 3.5f);
			TestTrue(TEXT("Created bool property"), Asset->bTestBool);
			TestEqual(TEXT("Created name property"), Asset->TestName, FName(TEXT("CreatedName")));
		}
	}

	{
		FScopedCanonicalSidecarFixture SidecarFixture(UpdateTarget);
		TSharedPtr<FJsonObject> CreateDocument = MakeApplyDocument(UpdateTarget, TEXT("/Script/AssetDocumentTests.TestDataAsset"), TEXT("Create"));
		SetProperty(CreateDocument, TEXT("TestString"), MakeShared<FJsonValueString>(TEXT("before")));
		SetProperty(CreateDocument, TEXT("TestInt"), MakeShared<FJsonValueNumber>(7));

		const FAssetDocumentResult CreateResult = SidecarFixture.Apply(Service, CreateDocument);
		TestTrue(TEXT("Create fixture for update succeeds"), CreateResult.IsSuccess());
		UTestDataAsset* UpdateBaseline = FindObject<UTestDataAsset>(
			nullptr,
			*AssetDocumentServiceTestGetObjectPath(UpdateTarget));
		TestNotNull(TEXT("Create fixture resolves the update baseline"), UpdateBaseline);
		if (UpdateBaseline)
		{
			UpdateBaseline->GetOutermost()->ClearDirtyFlag();
			TestFalse(
				TEXT("Update fixture starts from a clean package"),
				UpdateBaseline->GetOutermost()->IsDirty());
		}

		TSharedPtr<FJsonObject> UpdateDocument = MakeApplyDocument(UpdateTarget, TEXT("/Script/AssetDocumentTests.TestDataAsset"), TEXT("Update"));
		SetProperty(UpdateDocument, TEXT("TestString"), MakeShared<FJsonValueString>(TEXT("after")));

		const FAssetDocumentResult UpdateResult = SidecarFixture.Apply(Service, UpdateDocument);

		TestTrue(TEXT("Update one property succeeds"), UpdateResult.IsSuccess());

		UTestDataAsset* Asset = LoadObject<UTestDataAsset>(nullptr, *AssetDocumentServiceTestGetObjectPath(UpdateTarget));
		TestNotNull(TEXT("Updated TestDataAsset loads"), Asset);
		if (Asset)
		{
			TestEqual(TEXT("Updated specified property"), Asset->TestString, FString(TEXT("after")));
			TestEqual(TEXT("Update leaves unspecified property unchanged"), Asset->TestInt, 7);
		}
	}

	{
		FScopedCanonicalSidecarFixture SidecarFixture(WrongShapeStringTarget);
		TSharedPtr<FJsonObject> Document = MakeApplyDocument(
			WrongShapeStringTarget,
			TEXT("/Script/AssetDocumentTests.TestDataAsset"),
			TEXT("Create"));

		TSharedPtr<FJsonObject> WrongShapeValue = MakeShared<FJsonObject>();
		WrongShapeValue->SetStringField(TEXT("unexpected"), TEXT("object"));
		SetProperty(Document, TEXT("TestString"), MakeShared<FJsonValueObject>(WrongShapeValue));

		const FAssetDocumentResult Result = SidecarFixture.Apply(Service, Document);

		TestFalse(TEXT("Object-shaped value for a scalar string property fails"), Result.IsSuccess());
		TestTrue(
			FString::Printf(
				TEXT("Wrong-shaped string value reports the scalar codec failure: %s"),
				*DescribeResult(Result)),
			HasDiagnosticCode(Result, TEXT("PropertyPreflight.DecodeFailed")));
	}

	{
		FScopedCanonicalSidecarFixture SidecarFixture(FailedWrongShapeRetryTarget);
		TSharedPtr<FJsonObject> BadDocument = MakeApplyDocument(
			FailedWrongShapeRetryTarget,
			TEXT("/Script/AssetDocumentTests.TestDataAsset"),
			TEXT("Create"));

		TSharedPtr<FJsonObject> WrongShapeValue = MakeShared<FJsonObject>();
		WrongShapeValue->SetStringField(TEXT("unexpected"), TEXT("object"));
		SetProperty(BadDocument, TEXT("TestString"), MakeShared<FJsonValueObject>(WrongShapeValue));

		const FAssetDocumentResult BadResult = SidecarFixture.Apply(Service, BadDocument);
		TestFalse(TEXT("Failed Create with a wrong-shaped string value fails"), BadResult.IsSuccess());
		TestNull(TEXT("Failed Create with a wrong-shaped string value leaves no live asset"), FindObject<UObject>(nullptr, *AssetDocumentServiceTestGetObjectPath(FailedWrongShapeRetryTarget)));

		TSharedPtr<FJsonObject> RetryDocument = MakeApplyDocument(
			FailedWrongShapeRetryTarget,
			TEXT("/Script/AssetDocumentTests.TestDataAsset"),
			TEXT("Create"));
		SetProperty(RetryDocument, TEXT("TestString"), MakeShared<FJsonValueString>(TEXT("retry ok")));

		const FAssetDocumentResult RetryResult = SidecarFixture.Apply(Service, RetryDocument);
		TestTrue(TEXT("Valid Create after wrong-shaped string value succeeds"), RetryResult.IsSuccess());
	}

	{
		FScopedCanonicalSidecarFixture SidecarFixture(FailedUnknownPropertyRetryTarget);
		TSharedPtr<FJsonObject> BadDocument = MakeApplyDocument(
			FailedUnknownPropertyRetryTarget,
			TEXT("/Script/AssetDocumentTests.TestDataAsset"),
			TEXT("Create"));
		SetProperty(BadDocument, TEXT("DefinitelyUnknownProperty"), MakeShared<FJsonValueString>(TEXT("bad")));

		const FAssetDocumentResult BadResult = SidecarFixture.Apply(Service, BadDocument);
		TestFalse(TEXT("Failed Create with unknown property fails"), BadResult.IsSuccess());
		TestNull(TEXT("Failed Create with unknown property leaves no live asset"), FindObject<UObject>(nullptr, *AssetDocumentServiceTestGetObjectPath(FailedUnknownPropertyRetryTarget)));

		TSharedPtr<FJsonObject> RetryDocument = MakeApplyDocument(
			FailedUnknownPropertyRetryTarget,
			TEXT("/Script/AssetDocumentTests.TestDataAsset"),
			TEXT("Create"));
		SetProperty(RetryDocument, TEXT("TestString"), MakeShared<FJsonValueString>(TEXT("retry ok")));

		const FAssetDocumentResult RetryResult = SidecarFixture.Apply(Service, RetryDocument);
		TestTrue(TEXT("Valid Create after unknown property succeeds"), RetryResult.IsSuccess());
	}

	{
		FProperty* TestFloatProperty = FindFProperty<FProperty>(UTestDataAsset::StaticClass(), GET_MEMBER_NAME_CHECKED(UTestDataAsset, TestFloat));
		FScopedCanonicalSidecarFixture SidecarFixture(FailedNonWritableRetryTarget);
		TSharedPtr<FJsonObject> BadDocument = MakeApplyDocument(
			FailedNonWritableRetryTarget,
			TEXT("/Script/AssetDocumentTests.TestDataAsset"),
			TEXT("Create"));
		SetProperty(BadDocument, TEXT("TestFloat"), MakeShared<FJsonValueNumber>(9.25));

		FAssetDocumentResult BadResult;
		{
			FScopedAdditionalPropertyFlags EditConstScope(TestFloatProperty, CPF_EditConst);
			BadResult = SidecarFixture.Apply(Service, BadDocument);
		}
		TestFalse(TEXT("Failed Create with non-writable property fails"), BadResult.IsSuccess());
		TestTrue(TEXT("Failed Create with non-writable property returns diagnostics"), BadResult.Diagnostics.Num() > 0);
		TestNull(TEXT("Failed Create with non-writable property leaves no live asset"), FindObject<UObject>(nullptr, *AssetDocumentServiceTestGetObjectPath(FailedNonWritableRetryTarget)));

		TSharedPtr<FJsonObject> RetryDocument = MakeApplyDocument(
			FailedNonWritableRetryTarget,
			TEXT("/Script/AssetDocumentTests.TestDataAsset"),
			TEXT("Create"));
		SetProperty(RetryDocument, TEXT("TestString"), MakeShared<FJsonValueString>(TEXT("retry ok")));

		const FAssetDocumentResult RetryResult = SidecarFixture.Apply(Service, RetryDocument);
		TestTrue(TEXT("Valid Create after non-writable property succeeds"), RetryResult.IsSuccess());
	}

	{
		FAssetDocumentValidateRequest Request;
		Request.Document = MakeApplyDocument(TEXT("AssetDocumentTests/DA_InvalidTarget"), TEXT("/Script/AssetDocumentTests.TestDataAsset"), TEXT("Create"));
		SetProperty(Request.Document, TEXT("TestString"), MakeShared<FJsonValueString>(TEXT("bad target")));

		const FAssetDocumentResult Result = Service.Validate(Request);

		TestFalse(TEXT("Validate rejects an invalid target path"), Result.IsSuccess());
		TestTrue(
			FString::Printf(
				TEXT("Validate reports the exact non-canonical target diagnostic: %s"),
				*DescribeResult(Result)),
			HasDiagnosticCode(Result, TEXT("GenericRuntime.Request.InvalidValidateTarget")));
		TestNull(TEXT("Validate does not create a package for an invalid target"), FindPackage(nullptr, TEXT("AssetDocumentTests/DA_InvalidTarget")));
	}

	{
		FScopedCanonicalSidecarFixture SidecarFixture(WrongShapeIntegerTarget);
		TSharedPtr<FJsonObject> Document = MakeApplyDocument(
			WrongShapeIntegerTarget,
			TEXT("/Script/AssetDocumentTests.TestDataAsset"),
			TEXT("Create"));

		TSharedPtr<FJsonObject> WrongShapeValue = MakeShared<FJsonObject>();
		WrongShapeValue->SetNumberField(TEXT("unexpected"), 5);
		SetProperty(Document, TEXT("TestInt"), MakeShared<FJsonValueObject>(WrongShapeValue));

		const FAssetDocumentResult Result = SidecarFixture.Apply(Service, Document);

		TestFalse(TEXT("Object-shaped value for a scalar integer property fails"), Result.IsSuccess());
		TestTrue(
			FString::Printf(
				TEXT("Wrong-shaped integer value reports the scalar codec failure: %s"),
				*DescribeResult(Result)),
			HasDiagnosticCode(Result, TEXT("PropertyPreflight.DecodeFailed")));
		TestTrue(TEXT("Wrong-shaped integer value returns diagnostics"), Result.Diagnostics.Num() > 0);
	}

	for (const FString& Target : Targets)
	{
		CleanupTestAsset(Target);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentApplyFileTest,
	"UEAssetDoc.ApplyFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentApplyFileTest::RunTest(const FString& Parameters)
{
	const FString TestFolder = MakeUniqueTestFolderName();
	const FString ApplyTarget = FString::Printf(TEXT("/Game/%s/DA_ApplyFile"), *TestFolder);
	const FString MismatchTarget = FString::Printf(TEXT("/Game/%s/DA_Mismatch"), *TestFolder);
	const FString TestDirectory = MakeFullNormalizedPath(FPaths::Combine(FPaths::ProjectContentDir(), TestFolder));
	const FString ApplyFilePath = FPaths::Combine(TestDirectory, TEXT("DA_ApplyFile.assetdoc.json"));
	const FString InvalidJsonPath = FPaths::Combine(TestDirectory, TEXT("DA_InvalidJson.assetdoc.json"));
	const FString MismatchFilePath = FPaths::Combine(TestDirectory, TEXT("DA_Expected.assetdoc.json"));

	IFileManager& FileManager = IFileManager::Get();
	TestTrue(TEXT("Creates ApplyFile test directory"), FileManager.MakeDirectory(*TestDirectory, true));
	CleanupTestAsset(ApplyTarget);
	CleanupTestAsset(MismatchTarget);

	FAssetDocumentService* ActiveService =
		RequireActiveAssetDocumentService(*this);
	if (!ActiveService)
	{
		return false;
	}
	FAssetDocumentService& Service = *ActiveService;

	{
		TSharedPtr<FJsonObject> Document = MakeApplyDocument(
			ApplyTarget,
			TEXT("/Script/AssetDocumentTests.TestDataAsset"),
			TEXT("Create"));
		SetProperty(Document, TEXT("TestString"), MakeShared<FJsonValueString>(TEXT("from apply-file")));
		SetProperty(Document, TEXT("TestInt"), MakeShared<FJsonValueNumber>(27));
		TestTrue(TEXT("Writes ApplyFile sidecar"), WriteJsonObjectToFile(Document, ApplyFilePath));

		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = ApplyFilePath;
		Request.bSaveAsset = true;

		const FAssetDocumentResult Result = Service.ApplyFile(Request);

		TestTrue(TEXT("ApplyFile succeeds for matching sidecar"), Result.IsSuccess());
		TestEqual(TEXT("ApplyFile reports target"), Result.Target, ApplyTarget);
		TestEqual(TEXT("ApplyFile reports sidecar path"), Result.SidecarFilePath, ApplyFilePath);
		TestTrue(TEXT("ApplyFile saves asset when requested"), Result.bSavedAsset);
		TestTrue(TEXT("ApplyFile returns payload"), Result.Payload.IsValid());

		UTestDataAsset* Asset = LoadObject<UTestDataAsset>(nullptr, *AssetDocumentServiceTestGetObjectPath(ApplyTarget));
		TestNotNull(TEXT("ApplyFile-created asset loads"), Asset);
		if (Asset)
		{
			TestEqual(TEXT("ApplyFile applies string property"), Asset->TestString, FString(TEXT("from apply-file")));
			TestEqual(TEXT("ApplyFile applies int property"), Asset->TestInt, 27);
		}
	}

	{
		TestTrue(TEXT("Writes invalid JSON sidecar"), FFileHelper::SaveStringToFile(TEXT("{ definitely invalid json"), *InvalidJsonPath));

		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = InvalidJsonPath;
		Request.bSaveAsset = true;

		const FAssetDocumentResult Result = Service.ApplyFile(Request);

		TestFalse(TEXT("ApplyFile fails invalid JSON"), Result.IsSuccess());
		TestEqual(TEXT("Invalid JSON result reports sidecar path"), Result.SidecarFilePath, InvalidJsonPath);
		TestTrue(TEXT("Invalid JSON reports parse/read failure"), Result.Message.Contains(TEXT("JSON"), ESearchCase::IgnoreCase) || Result.Message.Contains(TEXT("parse"), ESearchCase::IgnoreCase));
	}

	{
		TSharedPtr<FJsonObject> Document = MakeApplyDocument(
			MismatchTarget,
			TEXT("/Script/AssetDocumentTests.TestDataAsset"),
			TEXT("Create"));
		SetProperty(Document, TEXT("TestString"), MakeShared<FJsonValueString>(TEXT("should not apply")));
		TestTrue(TEXT("Writes mismatched sidecar"), WriteJsonObjectToFile(Document, MismatchFilePath));

		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = MismatchFilePath;
		Request.bSaveAsset = true;

		const FAssetDocumentResult Result = Service.ApplyFile(Request);

		TestFalse(TEXT("ApplyFile fails when Target does not match sidecar path"), Result.IsSuccess());
		TestEqual(TEXT("Target mismatch reports sidecar path"), Result.SidecarFilePath, MismatchFilePath);
		TestTrue(TEXT("Target mismatch reports expected path"), Result.Message.Contains(TEXT("/Game/")) && Result.Message.Contains(TEXT("expected"), ESearchCase::IgnoreCase));
		TestNull(TEXT("Target mismatch does not create target asset"), FindObject<UObject>(nullptr, *AssetDocumentServiceTestGetObjectPath(MismatchTarget)));
	}

	CleanupTestAsset(ApplyTarget);
	CleanupTestAsset(MismatchTarget);
	TestTrue(TEXT("Cleans up ApplyFile test directory"), FileManager.DeleteDirectory(*TestDirectory, false, true));

	return true;
}

#endif
