// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// An inline document (the body of a validate or diff request) must reach the
// profiles with the number kinds it was written with, exactly like the same
// document read from its sidecar file: 1 is an integer, 1.0 a floating-point
// number. Otherwise an integer field compares unequal (1 vs 1.0) in Diff and
// Validate while Apply of the same text reports nothing to change.

#include "AssetDocumentJson.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentRequestNumberKindTests
{
bool IsIntegerLexeme(const TSharedPtr<FJsonValue>& Value, const TCHAR* Expected)
{
	FString Lexeme;
	return Value.IsValid() && Value->Type == EJson::Number && Value->PreferStringRepresentation()
		&& Value->TryGetString(Lexeme) && Lexeme == Expected;
}

bool IsFloatingNumber(const TSharedPtr<FJsonValue>& Value, double Expected)
{
	double Number = 0.0;
	return Value.IsValid() && Value->Type == EJson::Number && !Value->PreferStringRepresentation()
		&& Value->TryGetNumber(Number) && Number == Expected;
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

int32 ChangedCount(const FAssetDocumentResult& Result)
{
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	return Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("changed"), Changed) ? Changed->Num() : -1;
}

struct FTable
{
	FString AssetName = FString::Printf(TEXT("DT_NumberKinds_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FString PackageName = FString(TEXT("/Game/AssetDocumentRequestNumberKinds/")) + AssetName;
	FString ObjectPath = PackageName + TEXT(".") + AssetName;
	FString PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FString SidecarPath = FPaths::Combine(
		FPaths::ProjectContentDir(), TEXT("AssetDocumentRequestNumberKinds"), AssetName + TEXT(".assetdoc.json"));

	// Count is an int32, Weight a float and Precise a double, each written the way
	// an author (or JSON.stringify) writes it: integral values without a fraction.
	FString Document(const TCHAR* Action) const
	{
		return FString::Printf(
			TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.DataTable\",\"Action\":\"%s\",")
			TEXT("\"Properties\":{},\"Body\":{\"RowStruct\":\"/Script/AssetDocumentTests.AssetDocumentDataTestRow\",")
			TEXT("\"Rows\":[{\"Name\":\"A\",\"Values\":{\"Count\":3,\"Weight\":2.5,\"Precise\":4}}]}}"),
			*PackageName, Action);
	}

	void Cleanup() const
	{
		if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
			&& GEditor->Trans->GetQueueLength() > 0)
		{
			GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentRequestNumberKindTests", "Cleanup", "AssetDocument number kind test cleanup"));
		}
		if (UObject* Asset = FSoftObjectPath(ObjectPath).ResolveObject())
		{
			if (IFileManager::Get().FileExists(*PackageFile))
			{
				UPackageTools::UnloadPackages({Asset->GetOutermost()});
			}
			else
			{
				Asset->GetOutermost()->ClearDirtyFlag();
				ObjectTools::DeleteObjects({Asset}, false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		if (IFileManager::Get().FileExists(*PackageFile))
		{
			IFileManager::Get().Delete(*PackageFile, false, true);
			FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({PackageFile});
		}
		if (UPackage* Stale = FindPackage(nullptr, *PackageName))
		{
			Stale->ClearDirtyFlag();
			Stale->MarkAsGarbage();
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		IFileManager::Get().Delete(*SidecarPath, false, true);
	}
};
} // namespace AssetDocumentRequestNumberKindTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentRequestNumberKindParseTest,
	"UEAssetDoc.ApplySemantics.RequestNumberKinds.Parse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRequestNumberKindParseTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentRequestNumberKindTests;
	TSharedPtr<FJsonObject> Json;
	if (!TestTrue(TEXT("parses"),
			FAssetDocumentJson::ParseJsonString(TEXT("{\"a\":1,\"b\":1.0,\"c\":[2,2.5,1e3],\"d\":{\"e\":-3}}"), Json)))
	{
		return false;
	}
	TestTrue(TEXT("1 stays an integer"), IsIntegerLexeme(Json->TryGetField(TEXT("a")), TEXT("1")));
	TestTrue(TEXT("1.0 stays a floating-point number"), IsFloatingNumber(Json->TryGetField(TEXT("b")), 1.0));
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (TestTrue(TEXT("array"), Json->TryGetArrayField(TEXT("c"), Array) && Array->Num() == 3))
	{
		TestTrue(TEXT("array integer"), IsIntegerLexeme((*Array)[0], TEXT("2")));
		TestTrue(TEXT("array fraction"), IsFloatingNumber((*Array)[1], 2.5));
		TestTrue(TEXT("array exponent is floating-point"), IsFloatingNumber((*Array)[2], 1000.0));
	}
	const TSharedPtr<FJsonObject>* Nested = nullptr;
	if (TestTrue(TEXT("object"), Json->TryGetObjectField(TEXT("d"), Nested)))
	{
		TestTrue(TEXT("nested negative integer"), IsIntegerLexeme((*Nested)->TryGetField(TEXT("e")), TEXT("-3")));
	}
	TSharedPtr<FJsonObject> Invalid;
	TestFalse(TEXT("invalid text is refused"), FAssetDocumentJson::ParseJsonString(TEXT("{\"a\":"), Invalid));
	TestFalse(TEXT("refused text leaves no object"), Invalid.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentRequestNumberKindDiffTest,
	"UEAssetDoc.ApplySemantics.RequestNumberKinds.DiffAndValidate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentRequestNumberKindDiffTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentRequestNumberKindTests;
	const FTable Table;
	ON_SCOPE_EXIT { Table.Cleanup(); };
	FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();

	// Apply the document from its sidecar file.
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Table.SidecarPath), true);
	if (!TestTrue(TEXT("writes sidecar"), FFileHelper::SaveStringToFile(Table.Document(TEXT("CreateOrUpdate")), *Table.SidecarPath)))
	{
		return false;
	}
	FAssetDocumentApplyFileRequest ApplyRequest;
	ApplyRequest.FilePath = Table.SidecarPath;
	ApplyRequest.bSaveAsset = true;
	const FAssetDocumentResult Applied = Service.ApplyFile(ApplyRequest);
	if (!TestTrue(*FString::Printf(TEXT("apply (%s)"), *Applied.Message), Applied.IsSuccess()))
	{
		return false;
	}

	// The same text as a request body, parsed the way the validate and diff routes parse it.
	TSharedPtr<FJsonObject> Body;
	if (!TestTrue(TEXT("request body parses"), FAssetDocumentJson::ParseJsonString(Table.Document(TEXT("Update")), Body)))
	{
		return false;
	}
	FAssetDocumentDiffRequest DiffRequest;
	DiffRequest.Document = Body;
	const FAssetDocumentResult Diffed = Service.Diff(DiffRequest);
	TestTrue(*FString::Printf(TEXT("diff succeeds (%s)"), *Diffed.Message), Diffed.IsSuccess());
	TestEqual(*FString::Printf(TEXT("diff of the applied text changes nothing (%s)"), *Serialize(Diffed.Payload)),
		ChangedCount(Diffed), 0);

	FAssetDocumentValidateRequest ValidateRequest;
	ValidateRequest.Document = Body;
	const FAssetDocumentResult Validated = Service.Validate(ValidateRequest);
	TestTrue(*FString::Printf(TEXT("validate succeeds (%s)"), *Validated.Message), Validated.IsSuccess());
	TestTrue(*FString::Printf(TEXT("validate says nothing would change (%s)"), *Validated.Message),
		Validated.Message.Contains(TEXT("would change nothing")));

	// A real change is still a change, and only that one.
	TSharedPtr<FJsonObject> Changed;
	const FString ChangedText = Table.Document(TEXT("Update")).Replace(TEXT("\"Count\":3"), TEXT("\"Count\":4"));
	if (TestTrue(TEXT("changed body parses"), FAssetDocumentJson::ParseJsonString(ChangedText, Changed)))
	{
		DiffRequest.Document = Changed;
		const FAssetDocumentResult ChangedDiff = Service.Diff(DiffRequest);
		TestTrue(*FString::Printf(TEXT("changed diff succeeds (%s)"), *ChangedDiff.Message), ChangedDiff.IsSuccess());
		const FString ChangedPayload = Serialize(ChangedDiff.Payload);
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (TestTrue(*FString::Printf(TEXT("changed diff reports one change (%s)"), *ChangedPayload),
				ChangedDiff.Payload.IsValid() && ChangedDiff.Payload->TryGetArrayField(TEXT("changed"), Entries) && Entries->Num() == 1))
		{
			// The path names Count only; Weight and Precise (written 4, stored 4.0) are not changes.
			TestEqual(TEXT("the change is Count"), (*Entries)[0]->AsObject()->GetStringField(TEXT("path")),
				FString(TEXT("/Body/Rows/0/Values/Count")));
		}
	}

	// Before the fix the routes deserialised every number as a double. The DataTable
	// profile happens to read cells by their row field type, so this is only logged;
	// profiles that compare canonical JSON (int 3 != float 3.0) depended on the kind.
	TSharedPtr<FJsonObject> Doubles;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Table.Document(TEXT("Update"))), Doubles);
	DiffRequest.Document = Doubles;
	const FAssetDocumentResult DoublesDiff = Service.Diff(DiffRequest);
	AddInfo(FString::Printf(TEXT("all-doubles reading: %s %s"), *DoublesDiff.Message, *Serialize(DoublesDiff.Payload)));
	return true;
}
#endif
