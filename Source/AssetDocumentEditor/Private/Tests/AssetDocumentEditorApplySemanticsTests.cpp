// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Production-Service regressions for the apply semantics:
//   - CreateOrUpdate: a missing target is created and a present one updated, for every
//     route (generic, BlackboardData, DataTable, Blueprint).
//   - ReadsLeaveNoPackage: Extract / Inspect of a missing asset do not leave an empty
//     package behind, and a stale empty package does not block Create.
//   - Validate: runs Apply's preflight without mutating and reports what Apply would do.
//   - Diff: a missing target diffs as "would create"; before = asset, after = document.
//   - WatcherAndExplicitApply: the sidecar file watcher and an explicit apply of the same
//     unchanged file apply it once (direction doc §4.5).
//   - WatcherFailureNotice: a failed file watcher apply is kept as a pending notice until the sidecar is applied
//     again (get_editor_context, the next tool result).
//   - WatcherWriteBack: a Create applied from a sidecar is written back as CreateOrUpdate, so
//     editing the written-back sidecar and letting the watcher apply it again succeeds.

#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"
#include "AssetDocumentHttpServer.h"
#include "AssetDocumentPendingNotices.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
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
#include "UObject/UnrealType.h"
#include "Utils/AssetDocumentJsonKeys.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorApplySemanticsTests
{
const TCHAR* TestDataAssetClass = TEXT("/Script/AssetDocumentTests.TestDataAsset");
const TCHAR* DataTableClass = TEXT("/Script/Engine.DataTable");
const TCHAR* TestRow = TEXT("/Script/AssetDocumentTests.AssetDocumentDataTestRow");
const TCHAR* BlackboardClass = TEXT("/Script/AIModule.BlackboardData");
const TCHAR* BlueprintClass = TEXT("/Script/Engine.Blueprint");

void ReleaseUndoHistory()
{
	if (GEditor != nullptr && GEditor->Trans != nullptr && !GIsTransacting && !GEditor->IsTransactionActive()
		&& GEditor->Trans->GetQueueLength() > 0)
	{
		GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentApplySemanticsTests", "Cleanup", "AssetDocument apply semantics test cleanup"));
	}
}

struct FTarget
{
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	explicit FTarget(const TCHAR* Prefix)
		: AssetName(FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
	{
		PackageName = FString(TEXT("/Game/AssetDocumentApplySemantics/")) + AssetName;
		ObjectPath = PackageName + TEXT(".") + AssetName;
		PackageFile = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("AssetDocumentApplySemantics"),
			AssetName + TEXT(".assetdoc.json"));
	}

	UObject* Get() const { return FSoftObjectPath(ObjectPath).ResolveObject(); }

	FString Generic(const TCHAR* Action, const FString& Properties) const
	{
		return FString::Printf(
			TEXT("{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\",\"Target\":\"%s\",\"Class\":\"%s\",")
			TEXT("\"Action\":\"%s\",\"Properties\":%s}"),
			*PackageName, TestDataAssetClass, Action, *Properties);
	}

	FString DataTable(const TCHAR* Action, const FString& Body) const
	{
		return FString::Printf(
			TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"%s\",\"Action\":\"%s\",\"Properties\":{},\"Body\":{%s}}"),
			*PackageName, DataTableClass, Action, *Body);
	}

	void Cleanup() const
	{
		ReleaseUndoHistory();
		if (UObject* Asset = Get())
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

TSharedPtr<FJsonObject> Parse(const FString& Text)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
	return Object;
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

void Report(FAutomationTestBase& Test, const TCHAR* Stage, const FAssetDocumentResult& Result)
{
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), Stage, *Result.Message));
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s"), Stage, *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
}

bool WriteSidecar(FAutomationTestBase& Test, const FTarget& Target, const FString& Json)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target.SidecarPath), true);
	return Test.TestTrue(TEXT("writes sidecar"), FFileHelper::SaveStringToFile(Json, *Target.SidecarPath));
}

// The file watcher's call (ApplyWatchedSidecar) or an explicit apply_asset_document_file.
FAssetDocumentResult ApplyFile(FAutomationTestBase& Test, const FTarget& Target, bool bByWatcher, const TCHAR* Stage)
{
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = Target.SidecarPath;
	Request.bSaveAsset = true;
	Request.bTriggeredByWatcher = bByWatcher;
	FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
	Report(Test, Stage, Result);
	return Result;
}

FAssetDocumentResult Apply(FAutomationTestBase& Test, const FTarget& Target, const FString& Json, const TCHAR* Stage)
{
	if (!WriteSidecar(Test, Target, Json))
	{
		return FAssetDocumentResult::Failure(TEXT("sidecar not written"));
	}
	return ApplyFile(Test, Target, false, Stage);
}

FAssetDocumentResult Validate(FAutomationTestBase& Test, const FString& Json, const TCHAR* Stage)
{
	FAssetDocumentValidateRequest Request;
	Request.Document = Parse(Json);
	FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
	Report(Test, Stage, Result);
	return Result;
}

FAssetDocumentResult Diff(FAutomationTestBase& Test, const FString& Json, const TCHAR* Stage)
{
	FAssetDocumentDiffRequest Request;
	Request.Document = Parse(Json);
	FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Diff(Request);
	Report(Test, Stage, Result);
	return Result;
}

FAssetDocumentResult Extract(const FTarget& Target)
{
	FAssetDocumentExtractRequest Request;
	Request.AssetPath = Target.ObjectPath;
	Request.bDiffOnly = false;
	return FAssetDocumentModule::Get().GetService().Extract(Request);
}

FString TestString(const FTarget& Target)
{
	UObject* Asset = Target.Get();
	const FStrProperty* Property = Asset != nullptr
		? FindFProperty<FStrProperty>(Asset->GetClass(), TEXT("TestString"))
		: nullptr;
	return Property != nullptr ? Property->GetPropertyValue_InContainer(Asset) : FString(TEXT("<missing>"));
}

bool HasCode(const FAssetDocumentResult& Result, const FString& Code)
{
	return Result.Diagnostics.ContainsByPredicate(
		[&Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}

bool HasPathContaining(const FAssetDocumentResult& Result, const TCHAR* Text)
{
	return Result.Diagnostics.ContainsByPredicate(
		[Text](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Path.Contains(Text); });
}

bool PayloadBool(const FAssetDocumentResult& Result, const TCHAR* Field)
{
	bool bValue = false;
	return Result.Payload.IsValid() && Result.Payload->TryGetBoolField(Field, bValue) && bValue;
}

FString PayloadString(const FAssetDocumentResult& Result, const TCHAR* Field)
{
	FString Value;
	if (Result.Payload.IsValid())
	{
		Result.Payload->TryGetStringField(Field, Value);
	}
	return Value;
}

TArray<TSharedPtr<FJsonValue>> PayloadArray(const FAssetDocumentResult& Result, const TCHAR* Field)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Result.Payload.IsValid() && Result.Payload->TryGetArrayField(Field, Values))
	{
		return *Values;
	}
	return {};
}

bool ExistsAnywhere(const FTarget& Target)
{
	return Target.Get() != nullptr || IFileManager::Get().FileExists(*Target.PackageFile)
		|| FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Target.ObjectPath)).IsValid();
}

void TestNothingLeft(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Stage)
{
	Test.TestFalse(*FString::Printf(TEXT("%s: no asset, file or registry entry"), Stage), ExistsAnywhere(Target));
	Test.TestNull(*FString::Printf(TEXT("%s: no package"), Stage), FindPackage(nullptr, *Target.PackageName));
}

TSharedPtr<FJsonObject> Template(FAutomationTestBase& Test, const FTarget& Target, const TCHAR* Class)
{
	FAssetDocumentTemplateRequest Request;
	Request.Class = Class;
	Request.Target = Target.PackageName;
	const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().CreateTemplate(Request);
	Report(Test, TEXT("template"), Result);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s template"), Class), Result.IsSuccess() && Result.Payload.IsValid()))
	{
		return nullptr;
	}
	return Parse(Serialize(Result.Payload));
}
} // namespace AssetDocumentEditorApplySemanticsTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentApplySemanticsCreateOrUpdateTest,
	"UEAssetDoc.ApplySemantics.CreateOrUpdate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentApplySemanticsCreateOrUpdateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorApplySemanticsTests;
	const FTarget Generic(TEXT("DA_CreateOrUpdate"));
	const FTarget Table(TEXT("DT_CreateOrUpdate"));
	const FTarget Blackboard(TEXT("BB_CreateOrUpdate"));
	const FTarget Blueprint(TEXT("BP_CreateOrUpdate"));
	ON_SCOPE_EXIT
	{
		Generic.Cleanup();
		Table.Cleanup();
		Blackboard.Cleanup();
		Blueprint.Cleanup();
	};

	// Generic profile (the route that used to refuse a missing target).
	TestTrue(TEXT("generic: creates the missing target"),
		Apply(*this, Generic, Generic.Generic(TEXT("CreateOrUpdate"), TEXT("{\"TestString\":\"one\"}")), TEXT("generic create")).IsSuccess());
	TestEqual(TEXT("generic: created value"), TestString(Generic), FString(TEXT("one")));
	TestTrue(TEXT("generic: saved"), IFileManager::Get().FileExists(*Generic.PackageFile));
	TestTrue(TEXT("generic: updates the present target"),
		Apply(*this, Generic, Generic.Generic(TEXT("CreateOrUpdate"), TEXT("{\"TestString\":\"two\"}")), TEXT("generic update")).IsSuccess());
	TestEqual(TEXT("generic: updated value"), TestString(Generic), FString(TEXT("two")));

	// DataTable (registered profile).
	const FString Rows1 = FString::Printf(TEXT("\"RowStruct\":\"%s\",\"Rows\":[{\"Name\":\"A\",\"Values\":{\"Count\":1}}]"), TestRow);
	const FString Rows2 = FString::Printf(TEXT("\"RowStruct\":\"%s\",\"Rows\":[{\"Name\":\"A\",\"Values\":{\"Count\":2}},{\"Name\":\"B\"}]"), TestRow);
	TestTrue(TEXT("datatable: creates the missing target"),
		Apply(*this, Table, Table.DataTable(TEXT("CreateOrUpdate"), Rows1), TEXT("datatable create")).IsSuccess());
	TestNotNull(TEXT("datatable: exists"), Table.Get());
	TestTrue(TEXT("datatable: updates the present target"),
		Apply(*this, Table, Table.DataTable(TEXT("CreateOrUpdate"), Rows2), TEXT("datatable update")).IsSuccess());
	{
		const FAssetDocumentResult Extracted = Extract(Table);
		const FString Text = Serialize(Extracted.Payload);
		TestTrue(*FString::Printf(TEXT("datatable: second row present (%s)"), *Text), Text.Contains(TEXT("\"B\"")));
	}

	// BlackboardData (unregistered specialised profile).
	if (const TSharedPtr<FJsonObject> Document = Template(*this, Blackboard, BlackboardClass))
	{
		Document->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));
		TestTrue(TEXT("blackboard: creates the missing target"),
			Apply(*this, Blackboard, Serialize(Document), TEXT("blackboard create")).IsSuccess());
		TestNotNull(TEXT("blackboard: exists"), Blackboard.Get());
		const TSharedPtr<FJsonObject> Body = Document->GetObjectField(TEXT("Body"));
		const TSharedPtr<FJsonObject> Key = Parse(TEXT(
			"{\"Category\":\"\",\"Description\":\"\",\"Name\":\"Health\",\"bInstanceSynced\":false,"
			"\"KeyTypeClass\":{\"Kind\":\"ClassRef\",\"Path\":\"/Script/AIModule.BlackboardKeyType_Object\"},"
			"\"KeyTypeProperties\":{\"BaseClass\":{\"Kind\":\"ClassRef\",\"Path\":\"/Script/Engine.Actor\"},\"DefaultValue\":null}}"));
		Body->SetArrayField(TEXT("Keys"), {MakeShared<FJsonValueObject>(Key)});
		TestTrue(TEXT("blackboard: updates the present target"),
			Apply(*this, Blackboard, Serialize(Document), TEXT("blackboard update")).IsSuccess());
		TestTrue(TEXT("blackboard: key added"), Serialize(Extract(Blackboard).Payload).Contains(TEXT("Health")));
	}

	// Blueprint family.
	if (const TSharedPtr<FJsonObject> Document = Template(*this, Blueprint, BlueprintClass))
	{
		Document->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));
		TestTrue(TEXT("blueprint: creates the missing target"),
			Apply(*this, Blueprint, Serialize(Document), TEXT("blueprint create")).IsSuccess());
		TestNotNull(TEXT("blueprint: exists"), Blueprint.Get());
		Document->GetObjectField(TEXT("Properties"))->SetStringField(TEXT("BlueprintDescription"), TEXT("Updated by CreateOrUpdate"));
		TestTrue(TEXT("blueprint: updates the present target"),
			Apply(*this, Blueprint, Serialize(Document), TEXT("blueprint update")).IsSuccess());
		TestTrue(TEXT("blueprint: description updated"),
			Serialize(Extract(Blueprint).Payload).Contains(TEXT("Updated by CreateOrUpdate")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentApplySemanticsReadsLeaveNoPackageTest,
	"UEAssetDoc.ApplySemantics.ReadsLeaveNoPackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentApplySemanticsReadsLeaveNoPackageTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorApplySemanticsTests;
	const FTarget Target(TEXT("DA_MissingRead"));
	const FTarget Stale(TEXT("DA_StaleEmpty"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		Stale.Cleanup();
	};

	// Extract and Inspect of a missing asset fail and leave nothing behind.
	const FAssetDocumentResult Extracted = Extract(Target);
	Report(*this, TEXT("extract missing"), Extracted);
	TestFalse(TEXT("extract of a missing asset fails"), Extracted.IsSuccess());
	TestNothingLeft(*this, Target, TEXT("after extract"));
	FAssetDocumentInspectRequest InspectRequest;
	InspectRequest.ClassOrAsset = Target.ObjectPath;
	const FAssetDocumentResult Inspected = FAssetDocumentModule::Get().GetService().Inspect(InspectRequest);
	Report(*this, TEXT("inspect missing"), Inspected);
	TestNothingLeft(*this, Target, TEXT("after inspect"));
	{
		FAssetDocumentExtractRequest PackagePathRequest;
		PackagePathRequest.AssetPath = Target.PackageName;
		FAssetDocumentModule::Get().GetService().Extract(PackagePathRequest);
		TestNothingLeft(*this, Target, TEXT("after extract by package path"));
	}

	// The trial's sequence: a read, then Create.
	TestTrue(TEXT("create after a failed read"),
		Apply(*this, Target, Target.Generic(TEXT("Create"), TEXT("{\"TestString\":\"made\"}")), TEXT("create after read")).IsSuccess());
	TestEqual(TEXT("created after read"), TestString(Target), FString(TEXT("made")));

	// An empty, unsaved, clean package left by any other read does not block Create.
	UPackage* Empty = CreatePackage(*Stale.PackageName);
	Empty->ClearDirtyFlag();
	const FAssetDocumentResult Created =
		Apply(*this, Stale, Stale.Generic(TEXT("Create"), TEXT("{\"TestString\":\"made\"}")), TEXT("create over empty package"));
	TestTrue(TEXT("create over a stale empty package"), Created.IsSuccess());
	TestFalse(TEXT("no TargetExists"), HasCode(Created, TEXT("CutoverPreparation.TargetExists")));
	TestEqual(TEXT("created over empty package"), TestString(Stale), FString(TEXT("made")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentApplySemanticsValidateTest,
	"UEAssetDoc.ApplySemantics.Validate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentApplySemanticsValidateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorApplySemanticsTests;
	const FTarget Target(TEXT("DA_Validate"));
	const FTarget Table(TEXT("DT_Validate"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		Table.Cleanup();
	};

	// Missing target: valid documents report a create and create nothing.
	const FAssetDocumentResult WouldCreate =
		Validate(*this, Target.Generic(TEXT("CreateOrUpdate"), TEXT("{\"TestString\":\"one\"}")), TEXT("validate would create"));
	TestTrue(TEXT("valid create"), WouldCreate.IsSuccess());
	TestTrue(TEXT("reports would_create"), PayloadBool(WouldCreate, TEXT("would_create")));
	TestTrue(*FString::Printf(TEXT("message says create (%s)"), *WouldCreate.Message), WouldCreate.Message.Contains(TEXT("would create")));
	TestNothingLeft(*this, Target, TEXT("after validate of a create"));

	// Errors Apply's preflight finds are found by Validate, with the same diagnostics.
	const FAssetDocumentResult Unknown =
		Validate(*this, Target.Generic(TEXT("Create"), TEXT("{\"NotAProperty\":1}")), TEXT("validate unknown property"));
	TestFalse(TEXT("unknown property is invalid"), Unknown.IsSuccess());
	TestTrue(TEXT("unknown property diagnostic names it"), HasPathContaining(Unknown, TEXT("NotAProperty")));
	const FAssetDocumentResult WrongType =
		Validate(*this, Target.Generic(TEXT("Create"), TEXT("{\"TestString\":{\"Nested\":true}}")), TEXT("validate wrong type"));
	TestFalse(TEXT("wrong type is invalid"), WrongType.IsSuccess());
	TestTrue(TEXT("wrong type diagnostic names the property"), HasPathContaining(WrongType, TEXT("TestString")));
	AddExpectedError(TEXT("Failed to find object"), EAutomationExpectedErrorFlags::Contains, 0);
	const FAssetDocumentResult BadStruct = Validate(*this,
		Table.DataTable(TEXT("Create"), TEXT("\"RowStruct\":\"/Script/AssetDocumentData.NoSuchRowStruct\",\"Rows\":[]")),
		TEXT("validate bad row struct"));
	TestFalse(TEXT("bad RowStruct is invalid"), BadStruct.IsSuccess());
	TestTrue(TEXT("bad RowStruct diagnostic names it"), HasPathContaining(BadStruct, TEXT("RowStruct")));
	const FAssetDocumentResult UpdateMissing =
		Validate(*this, Target.Generic(TEXT("Update"), TEXT("{\"TestString\":\"one\"}")), TEXT("validate update of missing"));
	TestFalse(TEXT("Update of a missing target is invalid"), UpdateMissing.IsSuccess());
	TestNothingLeft(*this, Target, TEXT("after invalid validates"));
	TestNothingLeft(*this, Table, TEXT("after invalid datatable validate"));

	// Present target: counts what would change; nothing is applied.
	if (!TestTrue(TEXT("create"),
			Apply(*this, Target, Target.Generic(TEXT("Create"), TEXT("{\"TestString\":\"one\"}")), TEXT("create")).IsSuccess()))
	{
		return false;
	}
	const FAssetDocumentResult Same =
		Validate(*this, Target.Generic(TEXT("Update"), TEXT("{\"TestString\":\"one\"}")), TEXT("validate unchanged"));
	TestTrue(TEXT("unchanged is valid"), Same.IsSuccess());
	TestTrue(*FString::Printf(TEXT("message says nothing changes (%s)"), *Same.Message), Same.Message.Contains(TEXT("would change nothing")));
	const FAssetDocumentResult Changes =
		Validate(*this, Target.Generic(TEXT("Update"), TEXT("{\"TestString\":\"two\"}")), TEXT("validate change"));
	TestTrue(TEXT("change is valid"), Changes.IsSuccess());
	TestTrue(*FString::Printf(TEXT("message counts the change (%s)"), *Changes.Message), Changes.Message.Contains(TEXT("would change 1 value")));
	TestFalse(TEXT("message does not claim it applied"), Changes.Message.Contains(TEXT("applied")));
	TestEqual(TEXT("validate did not apply"), TestString(Target), FString(TEXT("one")));
	TestFalse(TEXT("validate left the package clean"), Target.Get()->GetOutermost()->IsDirty());
	const FAssetDocumentResult CreateExisting =
		Validate(*this, Target.Generic(TEXT("Create"), TEXT("{\"TestString\":\"one\"}")), TEXT("validate create of existing"));
	TestFalse(TEXT("Create of a present target is invalid"), CreateExisting.IsSuccess());
	TestTrue(TEXT("Create of a present target reports TargetExists"), HasCode(CreateExisting, TEXT("CutoverPreparation.TargetExists")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentApplySemanticsDiffTest,
	"UEAssetDoc.ApplySemantics.Diff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentApplySemanticsDiffTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorApplySemanticsTests;
	const FTarget Target(TEXT("DA_Diff"));
	ON_SCOPE_EXIT { Target.Cleanup(); };

	const FAssetDocumentResult Missing =
		Diff(*this, Target.Generic(TEXT("CreateOrUpdate"), TEXT("{\"TestString\":\"one\"}")), TEXT("diff missing"));
	TestTrue(TEXT("diff of a missing target succeeds"), Missing.IsSuccess());
	TestTrue(TEXT("diff reports would_create"), PayloadBool(Missing, TEXT("would_create")));
	const TArray<TSharedPtr<FJsonValue>> Created = PayloadArray(Missing, TEXT("changed"));
	if (TestEqual(TEXT("one whole-document change"), Created.Num(), 1))
	{
		const TSharedPtr<FJsonObject> Entry = Created[0]->AsObject();
		TestEqual(TEXT("at the root"), Entry->GetStringField(TEXT("path")), FString(TEXT("/")));
		TestEqual(TEXT("an Add"), Entry->GetStringField(TEXT("kind")), FString(TEXT("Add")));
		TestTrue(TEXT("after is the document"), Serialize(Entry->GetObjectField(TEXT("after"))).Contains(TEXT("\"one\"")));
	}
	TestNothingLeft(*this, Target, TEXT("after diff of a missing target"));
	const FAssetDocumentResult UpdateMissing =
		Diff(*this, Target.Generic(TEXT("Update"), TEXT("{\"TestString\":\"one\"}")), TEXT("diff update missing"));
	TestFalse(TEXT("diff of an Update of a missing target fails"), UpdateMissing.IsSuccess());
	TestTrue(TEXT("reports UpdateTargetMissing"), HasCode(UpdateMissing, TEXT("GenericRuntime.UpdateTargetMissing")));
	TestNothingLeft(*this, Target, TEXT("after failed diff"));

	if (!TestTrue(TEXT("create"),
			Apply(*this, Target, Target.Generic(TEXT("Create"), TEXT("{\"TestString\":\"asset\"}")), TEXT("create")).IsSuccess()))
	{
		return false;
	}
	const FAssetDocumentResult Changed =
		Diff(*this, Target.Generic(TEXT("Update"), TEXT("{\"TestString\":\"document\"}")), TEXT("diff change"));
	TestTrue(TEXT("diff succeeds"), Changed.IsSuccess());
	const TArray<TSharedPtr<FJsonValue>> Entries = PayloadArray(Changed, TEXT("changed"));
	if (TestEqual(*FString::Printf(TEXT("one change (%s)"), *Serialize(Changed.Payload)), Entries.Num(), 1))
	{
		const TSharedPtr<FJsonObject> Entry = Entries[0]->AsObject();
		TestEqual(TEXT("smallest path"), Entry->GetStringField(TEXT("path")), FString(TEXT("/Properties/TestString")));
		TestEqual(TEXT("before is the asset"), Entry->GetStringField(TEXT("before")), FString(TEXT("asset")));
		TestEqual(TEXT("after is the document"), Entry->GetStringField(TEXT("after")), FString(TEXT("document")));
	}

	// Extract diff_only lists only values that differ from the class defaults.
	auto ExtractProperties = [&Target](bool bDiffOnly)
	{
		FAssetDocumentExtractRequest Request;
		Request.AssetPath = Target.ObjectPath;
		Request.bDiffOnly = bDiffOnly;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Extract(Request);
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		return Result.Payload.IsValid() && Result.Payload->TryGetObjectField(TEXT("Properties"), Properties)
			? *Properties : TSharedPtr<FJsonObject>();
	};
	const TSharedPtr<FJsonObject> Sparse = ExtractProperties(true);
	const TSharedPtr<FJsonObject> Full = ExtractProperties(false);
	if (TestTrue(TEXT("both extracts have Properties"), Sparse.IsValid() && Full.IsValid()))
	{
		TArray<FString> SparseNames;
		AssetDocumentServerJson::GetKeys(*Sparse, SparseNames);
		TestTrue(*FString::Printf(TEXT("diff_only lists only TestString (%s)"), *Serialize(Sparse)),
			SparseNames.Num() == 1 && SparseNames[0] == TEXT("TestString"));
		TestTrue(*FString::Printf(TEXT("full extract lists defaults too (%s)"), *Serialize(Full)),
			Full->Values.Num() > 1 && Full->HasField(TEXT("TestName")));
	}

	// An authored value equal to the class default verifies (it reads back as present).
	TestTrue(TEXT("apply of a default value"),
		Apply(*this, Target, Target.Generic(TEXT("Update"), TEXT("{\"TestString\":\"\"}")), TEXT("apply default")).IsSuccess());
	TestEqual(TEXT("default value applied"), TestString(Target), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentApplySemanticsWatcherTest,
	"UEAssetDoc.ApplySemantics.WatcherAndExplicitApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentApplySemanticsWatcherTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorApplySemanticsTests;
	const FTarget Target(TEXT("DA_Watcher"));
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// 1. The trial's race: the watcher applies a Create first, then the AI's explicit apply.
	if (!WriteSidecar(*this, Target, Target.Generic(TEXT("Create"), TEXT("{\"TestString\":\"one\"}"))))
	{
		return false;
	}
	const FAssetDocumentResult ByWatcher = ApplyFile(*this, Target, true, TEXT("watcher create"));
	TestTrue(TEXT("watcher creates"), ByWatcher.IsSuccess());
	const FAssetDocumentResult Explicit = ApplyFile(*this, Target, false, TEXT("explicit after watcher"));
	TestTrue(TEXT("explicit apply reports the watcher's success, not TargetExists"), Explicit.IsSuccess());
	TestFalse(TEXT("no TargetExists"), HasCode(Explicit, TEXT("CutoverPreparation.TargetExists")));
	TestEqual(TEXT("marked as applied by the watcher"), PayloadString(Explicit, TEXT("applied_by")), FString(TEXT("file_watcher")));
	TestTrue(TEXT("message says who applied it"), Explicit.Message.Contains(TEXT("file watcher")));
	TestEqual(TEXT("value applied once"), TestString(Target), FString(TEXT("one")));

	// 2. Explicit apply first (new content), then the watcher for the same content: skipped.
	if (!WriteSidecar(*this, Target, Target.Generic(TEXT("Update"), TEXT("{\"TestString\":\"two\"}"))))
	{
		return false;
	}
	const FAssetDocumentResult ExplicitFirst = ApplyFile(*this, Target, false, TEXT("explicit update"));
	TestTrue(TEXT("explicit applies new content"), ExplicitFirst.IsSuccess());
	TestTrue(TEXT("explicit ran itself"), PayloadString(ExplicitFirst, TEXT("applied_by")).IsEmpty());
	TestEqual(TEXT("updated"), TestString(Target), FString(TEXT("two")));
	const FAssetDocumentResult WatcherLater = ApplyFile(*this, Target, true, TEXT("watcher after explicit"));
	TestTrue(TEXT("watcher run succeeds"), WatcherLater.IsSuccess());
	TestTrue(TEXT("watcher run is skipped"), PayloadBool(WatcherLater, TEXT("skipped_already_applied")));

	// 3. Explicit after explicit always runs (a repeated explicit apply is a normal re-apply).
	const FAssetDocumentResult Again = ApplyFile(*this, Target, false, TEXT("explicit again"));
	TestTrue(TEXT("explicit re-apply succeeds"), Again.IsSuccess());
	TestTrue(TEXT("explicit re-apply ran itself"), PayloadString(Again, TEXT("applied_by")).IsEmpty());

	// 4. The target changed since the watcher's run: the explicit apply runs again.
	if (!WriteSidecar(*this, Target, Target.Generic(TEXT("Update"), TEXT("{\"TestString\":\"three\"}"))))
	{
		return false;
	}
	TestTrue(TEXT("watcher applies"), ApplyFile(*this, Target, true, TEXT("watcher update")).IsSuccess());
	TestEqual(TEXT("updated by watcher"), TestString(Target), FString(TEXT("three")));
	IFileManager::Get().SetTimeStamp(*Target.PackageFile, FDateTime::UtcNow() + FTimespan::FromMinutes(1));
	const FAssetDocumentResult AfterChange = ApplyFile(*this, Target, false, TEXT("explicit after target change"));
	TestTrue(TEXT("explicit apply after a target change succeeds"), AfterChange.IsSuccess());
	TestTrue(TEXT("explicit apply after a target change ran itself"), PayloadString(AfterChange, TEXT("applied_by")).IsEmpty());

	// 5. A failed watcher run does not stand in for the explicit apply.
	if (!WriteSidecar(*this, Target, Target.Generic(TEXT("Update"), TEXT("{\"NotAProperty\":1}"))))
	{
		return false;
	}
	TestFalse(TEXT("watcher run fails"), ApplyFile(*this, Target, true, TEXT("watcher invalid")).IsSuccess());
	const FAssetDocumentResult ExplicitInvalid = ApplyFile(*this, Target, false, TEXT("explicit invalid"));
	TestFalse(TEXT("explicit run fails too"), ExplicitInvalid.IsSuccess());
	TestTrue(TEXT("explicit run reports its own diagnostics"), HasPathContaining(ExplicitInvalid, TEXT("NotAProperty")));
	TestTrue(TEXT("explicit run is not a replay"), PayloadString(ExplicitInvalid, TEXT("applied_by")).IsEmpty());
	return true;
}

// A failed file watcher apply has no caller: it stays a pending notice (file, error code, message) that
// get_editor_context lists and the next tool result reports, until the sidecar is applied again (an explicit apply,
// which reports its own result, or a watcher apply that succeeds).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentApplySemanticsWatcherFailureNoticeTest,
	"UEAssetDoc.ApplySemantics.WatcherFailureNotice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentApplySemanticsWatcherFailureNoticeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorApplySemanticsTests;
	const FTarget Target(TEXT("DA_WatcherFailure"));
	const FString File = FPaths::ConvertRelativePathToFull(Target.SidecarPath);
	// Each of the two failed watcher applies is also logged as a warning.
	AddExpectedErrorPlain(TEXT("AssetDocument file watcher apply of"), EAutomationExpectedErrorFlags::Contains, 2);
	ON_SCOPE_EXIT
	{
		FAssetDocumentPendingNotices::Resolve(FAssetDocumentService::WatcherApplyFailedNotice, File);
		Target.Cleanup();
	};
	const auto FindNotice = [&File]() -> TOptional<FAssetDocumentPendingNotice>
	{
		for (const FAssetDocumentPendingNotice& Notice : FAssetDocumentPendingNotices::GetOpen())
		{
			if (Notice.Kind == FAssetDocumentService::WatcherApplyFailedNotice && Notice.File.Equals(File, ESearchCase::IgnoreCase))
			{
				return Notice;
			}
		}
		return {};
	};
	const auto ContextLists = [&File]()
	{
		int32 Status = 0;
		const TSharedPtr<FJsonObject> Context = FAssetDocumentHttpServer::CallRoute(TEXT("GET"), TEXT("/context"), FString(), {}, 0, Status);
		const TArray<TSharedPtr<FJsonValue>>* Notices = nullptr;
		return Context.IsValid() && Context->TryGetArrayField(TEXT("pendingNotices"), Notices)
			&& Notices->ContainsByPredicate([&File](const TSharedPtr<FJsonValue>& Notice)
			{
				return Notice->AsObject()->GetStringField(TEXT("file")).Equals(File, ESearchCase::IgnoreCase);
			});
	};

	// 1. The watcher applies a broken sidecar: the failure is kept with its file, code and message.
	if (!WriteSidecar(*this, Target, Target.Generic(TEXT("Create"), TEXT("{\"NotAProperty\":1}"))))
	{
		return false;
	}
	const FAssetDocumentResult Failed = ApplyFile(*this, Target, true, TEXT("watcher invalid"));
	TestFalse(TEXT("the watcher apply fails"), Failed.IsSuccess());
	const TOptional<FAssetDocumentPendingNotice> Notice = FindNotice();
	if (TestTrue(TEXT("the failed watcher apply is a pending notice"), Notice.IsSet()))
	{
		TestFalse(TEXT("the failure has a code"), Notice->Code.IsEmpty());
		TestTrue(*FString::Printf(TEXT("the code is one of the apply's diagnostics (%s)"), *Notice->Code), HasCode(Failed, Notice->Code));
		TestTrue(*FString::Printf(TEXT("the message is the apply's (%s)"), *Notice->Message), Notice->Message.Contains(Failed.Message));
		TestTrue(TEXT("the next tool result reports it"),
			FAssetDocumentPendingNotices::DescribeForToolResult(FAssetDocumentPendingNotices::TakeUnreported()).Contains(File));
	}
	TestTrue(TEXT("get_editor_context lists it"), ContextLists());

	// 2. A watcher apply that succeeds resolves it.
	if (!WriteSidecar(*this, Target, Target.Generic(TEXT("Create"), TEXT("{\"TestString\":\"one\"}"))))
	{
		return false;
	}
	TestTrue(TEXT("the corrected watcher apply succeeds"), ApplyFile(*this, Target, true, TEXT("watcher fixed")).IsSuccess());
	TestFalse(TEXT("a successful watcher apply resolves the notice"), FindNotice().IsSet());
	TestFalse(TEXT("get_editor_context no longer lists it"), ContextLists());

	// 3. A failed watcher apply followed by the explicit apply: the explicit result is the report, the notice goes.
	if (!WriteSidecar(*this, Target, Target.Generic(TEXT("Update"), TEXT("{\"NotAProperty\":2}"))))
	{
		return false;
	}
	TestFalse(TEXT("the watcher apply fails again"), ApplyFile(*this, Target, true, TEXT("watcher invalid again")).IsSuccess());
	TestTrue(TEXT("a new failure is a pending notice again"), FindNotice().IsSet());
	TestFalse(TEXT("the explicit apply reports its own failure"), ApplyFile(*this, Target, false, TEXT("explicit invalid")).IsSuccess());
	TestFalse(TEXT("an explicit apply resolves the notice"), FindNotice().IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentApplySemanticsWatcherWriteBackTest,
	"UEAssetDoc.ApplySemantics.WatcherWriteBack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentApplySemanticsWatcherWriteBackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorApplySemanticsTests;
	const FTarget Target(TEXT("DA_WriteBack"));
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// 1. The watcher applies a Create; the sidecar is written back in canonical form.
	if (!WriteSidecar(*this, Target, Target.Generic(TEXT("Create"), TEXT("{\"TestString\":\"one\"}"))))
	{
		return false;
	}
	const FAssetDocumentResult Created = ApplyFile(*this, Target, true, TEXT("watcher create"));
	TestTrue(TEXT("watcher creates"), Created.IsSuccess());
	TestTrue(TEXT("the sidecar is written back"), Created.bWroteSidecar);
	FString WrittenBack;
	TestTrue(TEXT("reads the written-back sidecar"), FFileHelper::LoadFileToString(WrittenBack, *Target.SidecarPath));
	const TSharedPtr<FJsonObject> Document = Parse(WrittenBack);
	if (!TestTrue(TEXT("written-back sidecar parses"), Document.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("a Create is written back as CreateOrUpdate"), Document->GetStringField(TEXT("Action")),
		FString(TEXT("CreateOrUpdate")));

	// 2. The user edits the written-back sidecar; the watcher applies the edit.
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	if (!TestTrue(TEXT("written-back sidecar has Properties"), Document->TryGetObjectField(TEXT("Properties"), Properties)))
	{
		return false;
	}
	(*Properties)->SetStringField(TEXT("TestString"), TEXT("two"));
	if (!WriteSidecar(*this, Target, Serialize(Document)))
	{
		return false;
	}
	const FAssetDocumentResult Edited = ApplyFile(*this, Target, true, TEXT("watcher edit"));
	TestTrue(TEXT("the watcher applies the edited sidecar"), Edited.IsSuccess());
	TestFalse(TEXT("no TargetExists"), HasCode(Edited, TEXT("CutoverPreparation.TargetExists")));
	TestEqual(TEXT("the edit is applied"), TestString(Target), FString(TEXT("two")));
	return true;
}

// A client extracted a DataTable, someone else then added a row, and the client's old document
// (one comment changed) deleted that row while validate said "would change 1 value(s)". Now:
//   - the extract a client gets carries BaseRevision (after Target; the same for every extract option), and an apply or
//     validate of a document whose asset changed since is refused with Document.StaleBase, naming what changed, before
//     anything changes;
//   - a fresh document applies and returns the new base_revision; a second document on the same base (another client,
//     or the same document still carrying the old base) is refused, its follow-up with the returned base_revision
//     applies, and the same file applied again is a no-op; a forgotten base (editor restart) is still refused;
//   - a document without BaseRevision applies as before, and validate and diff count and name the rows it removes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentApplySemanticsStaleDocumentTest,
	"UEAssetDoc.ApplySemantics.StaleDocument",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentApplySemanticsStaleDocumentTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorApplySemanticsTests;
	const FTarget Table(TEXT("DT_Stale"));
	ON_SCOPE_EXIT { Table.Cleanup(); };
	const FString RowStruct = FString::Printf(TEXT("\"RowStruct\":\"%s\",\"Rows\":"), TestRow);
	auto ClientExtract = [&Table](bool bDiffOnly)
	{
		FAssetDocumentExtractRequest Request;
		Request.AssetPath = Table.ObjectPath;
		Request.bDiffOnly = bDiffOnly;
		Request.bIncludeBaseRevision = true;
		return FAssetDocumentModule::Get().GetService().Extract(Request);
	};
	auto WithLabel = [](const TSharedPtr<FJsonObject>& Document, const TCHAR* Row, const TCHAR* Label)
	{
		const TSharedPtr<FJsonObject> Edited = Parse(Serialize(Document));
		for (const TSharedPtr<FJsonValue>& Entry : Edited->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Rows")))
		{
			const TSharedPtr<FJsonObject> RowObject = Entry->AsObject();
			if (RowObject->GetStringField(TEXT("Name")) == Row)
			{
				const TSharedPtr<FJsonObject>* Values = nullptr;
				const TSharedPtr<FJsonObject> NewValues = RowObject->TryGetObjectField(TEXT("Values"), Values)
					? *Values : MakeShared<FJsonObject>();
				NewValues->SetStringField(TEXT("Label"), Label);
				RowObject->SetObjectField(TEXT("Values"), NewValues);
			}
		}
		return Serialize(Edited);
	};
	auto LiveText = [&Table]() { return Serialize(Extract(Table).Payload); };

	if (!TestTrue(TEXT("create"), Apply(*this, Table, Table.DataTable(TEXT("Create"),
			RowStruct + TEXT("[{\"Name\":\"Sword\"},{\"Name\":\"Bow\"}]")), TEXT("create")).IsSuccess()))
	{
		return false;
	}
	const FAssetDocumentResult First = ClientExtract(true);
	FString Base;
	if (!TestTrue(TEXT("the client's extract carries BaseRevision"), First.IsSuccess() && First.Payload.IsValid()
			&& First.Payload->TryGetStringField(TEXT("BaseRevision"), Base) && Base.Len() == 16))
	{
		return false;
	}
	TArray<FString> Keys;
	AssetDocumentServerJson::GetKeys(*First.Payload, Keys);
	TestEqual(TEXT("BaseRevision follows Target"), Keys.IndexOfByKey(TEXT("BaseRevision")), Keys.IndexOfByKey(TEXT("Target")) + 1);
	FString FullBase;
	const FAssetDocumentResult Full = ClientExtract(false);
	TestTrue(TEXT("a full extract has the same revision"), Full.Payload.IsValid() && Full.Payload->TryGetStringField(TEXT("BaseRevision"), FullBase) && FullBase == Base);
	TestFalse(TEXT("internal extracts carry no BaseRevision"), Extract(Table).Payload->HasField(TEXT("BaseRevision")));
	const FAssetDocumentResult Unchanged = Validate(*this, Serialize(First.Payload), TEXT("validate the extract"));
	TestTrue(TEXT("the unchanged extract validates and changes nothing"), Unchanged.IsSuccess() && Unchanged.Message.Contains(TEXT("would change nothing")));

	// Someone else adds a row; a document without BaseRevision applies as before.
	TestTrue(TEXT("another change applies"), Apply(*this, Table, Table.DataTable(TEXT("Update"),
		RowStruct + TEXT("[{\"Name\":\"Sword\"},{\"Name\":\"Bow\"},{\"Name\":\"Poison\"}]")), TEXT("other change")).IsSuccess());
	const FString AfterOther = LiveText();

	// The old extract, edited: refused before any change, naming the added row.
	const FString Stale = WithLabel(First.Payload, TEXT("Sword"), TEXT("Edited"));
	for (const bool bApply : {false, true})
	{
		const FAssetDocumentResult Refused = bApply ? Apply(*this, Table, Stale, TEXT("stale apply")) : Validate(*this, Stale, TEXT("stale validate"));
		const FAssetDocumentDiagnostic* Found = Refused.Diagnostics.FindByPredicate(
			[](const FAssetDocumentDiagnostic& D) { return D.Code == TEXT("Document.StaleBase"); });
		if (TestTrue(*FString::Printf(TEXT("%s of a stale document is refused"), bApply ? TEXT("apply") : TEXT("validate")),
				!Refused.IsSuccess() && Found != nullptr))
		{
			TestEqual(TEXT("at BaseRevision"), Found->Path, FString(TEXT("/BaseRevision")));
			TestTrue(*FString::Printf(TEXT("names the added row (%s)"), *Found->Message), Found->Message.Contains(TEXT("Poison")));
			TestTrue(TEXT("says what to do"), Found->Message.Contains(TEXT("Extract the asset again")) && Found->Message.Contains(TEXT("leave BaseRevision out")));
		}
	}
	TestEqual(TEXT("the refusals changed nothing"), LiveText(), AfterOther);

	auto FindStale = [](const FAssetDocumentResult& Result)
	{
		return Result.Diagnostics.FindByPredicate([](const FAssetDocumentDiagnostic& D) { return D.Code == TEXT("Document.StaleBase"); });
	};

	// Two clients extract the same revision. A applies; B then applies its own edit on the same base and is
	// refused (it would undo A's change), and the message names the revision A's apply returned. A's own follow-up
	// carries that returned base_revision and applies; the same file again is a no-op.
	const FAssetDocumentResult Fresh = ClientExtract(true);
	const FAssetDocumentResult Applied = Apply(*this, Table, WithLabel(Fresh.Payload, TEXT("Sword"), TEXT("Fresh")), TEXT("client A apply"));
	TestTrue(TEXT("a fresh document applies"), Applied.IsSuccess());
	const FString AfterA = PayloadString(Applied, TEXT("base_revision"));
	TestFalse(TEXT("the apply returns the new revision"), AfterA.IsEmpty());
	TestEqual(TEXT("the returned revision is the asset's (a fresh extract's BaseRevision)"), PayloadString(ClientExtract(true), TEXT("BaseRevision")), AfterA);
	const FString AfterAText = LiveText();
	for (const TCHAR* Label : {TEXT("ClientB"), TEXT("AgainOnOldBase")})
	{
		// B's edit of the same extract, and A's further edit that still carries the old base, look the same.
		const FAssetDocumentResult SameBase = Apply(*this, Table, WithLabel(Fresh.Payload, TEXT("Bow"), Label), TEXT("same-base apply"));
		const FAssetDocumentDiagnostic* Found = FindStale(SameBase);
		if (TestTrue(*FString::Printf(TEXT("%s: a second apply on the same base is refused"), Label), !SameBase.IsSuccess() && Found != nullptr))
		{
			TestTrue(*FString::Printf(TEXT("names the label A changed (%s)"), *Found->Message), Found->Message.Contains(TEXT("Label")));
			TestTrue(TEXT("names the revision A's apply returned"), Found->Message.Contains(AfterA));
			TestTrue(TEXT("says what to do"), Found->Message.Contains(TEXT("Extract the asset again")));
			TestTrue(TEXT("flags the apply of the same base"), PayloadBool(SameBase, TEXT("changed_by_apply_of_same_base")));
		}
		TestEqual(TEXT("the refusal changed nothing"), LiveText(), AfterAText);
	}
	const TSharedPtr<FJsonObject> FollowUp = Parse(WithLabel(Fresh.Payload, TEXT("Sword"), TEXT("Again")));
	FollowUp->SetStringField(TEXT("BaseRevision"), AfterA);
	const FAssetDocumentResult Second = Apply(*this, Table, Serialize(FollowUp), TEXT("A's follow-up apply"));
	TestTrue(TEXT("A's follow-up with the returned base_revision applies"), Second.IsSuccess());
	TestTrue(TEXT("the same file again is a no-op"), ApplyFile(*this, Table, false, TEXT("again")).IsSuccess());
	TestTrue(TEXT("the edit and the other row are there"), LiveText().Contains(TEXT("Again")) && LiveText().Contains(TEXT("Poison")));

	// After an editor restart the remembered extracts are gone: a stale document is still refused (the message cannot
	// list the changes) and says what to do. A revision the editor never produced stands for one it forgot.
	{
		const TSharedPtr<FJsonObject> Forgotten = Parse(WithLabel(Fresh.Payload, TEXT("Bow"), TEXT("AfterRestart")));
		Forgotten->SetStringField(TEXT("BaseRevision"), TEXT("0123456789abcdef"));
		const FString BeforeRestartCase = LiveText();
		const FAssetDocumentResult Refused = Apply(*this, Table, Serialize(Forgotten), TEXT("forgotten base"));
		const FAssetDocumentDiagnostic* Found = FindStale(Refused);
		if (TestTrue(TEXT("a stale document with a forgotten base is refused"), !Refused.IsSuccess() && Found != nullptr))
		{
			TestTrue(*FString::Printf(TEXT("says it cannot list the changes (%s)"), *Found->Message), Found->Message.Contains(TEXT("no longer has that extract")));
			TestTrue(TEXT("says what to do"), Found->Message.Contains(TEXT("Extract the asset again")) && Found->Message.Contains(TEXT("leave BaseRevision out")));
		}
		TestEqual(TEXT("the refusal changed nothing"), LiveText(), BeforeRestartCase);
	}

	// Removals are counted and named (here a document without BaseRevision, which overwrites as before).
	const FString DropPoison = Table.DataTable(TEXT("Update"), RowStruct + TEXT("[{\"Name\":\"Sword\",\"Values\":{\"Label\":\"Again\"}},{\"Name\":\"Bow\"}]"));
	const FAssetDocumentResult Summary = Validate(*this, DropPoison, TEXT("validate removal"));
	TestTrue(*FString::Printf(TEXT("validate names the removed row (%s)"), *Summary.Message),
		Summary.IsSuccess() && Summary.Message.Contains(TEXT("removing 1 entry")) && Summary.Message.Contains(TEXT("Poison")));
	TestEqual(TEXT("validate lists the removal"), PayloadArray(Summary, TEXT("removals")).Num(), 1);
	const FAssetDocumentResult Diffed = Diff(*this, DropPoison, TEXT("diff removal"));
	TestTrue(*FString::Printf(TEXT("diff names the removed row (%s)"), *Diffed.Message), Diffed.IsSuccess() && Diffed.Message.Contains(TEXT("Poison")));
	TestTrue(TEXT("a document without BaseRevision applies as before"), Apply(*this, Table, DropPoison, TEXT("remove row")).IsSuccess());
	TestFalse(TEXT("the row is removed"), LiveText().Contains(TEXT("Poison")));

	// Numbers in a list are values: a changed element is replaced, not removed; a shorter list
	// removes as many elements as it loses.
	auto WithLevels = [&Table, &RowStruct](const TCHAR* Levels)
	{
		return Table.DataTable(TEXT("Update"), RowStruct + FString::Printf(TEXT("[{\"Name\":\"Sword\",\"Values\":{\"Label\":\"Again\",\"Levels\":%s}},{\"Name\":\"Bow\"}]"), Levels));
	};
	TestTrue(TEXT("levels apply"), Apply(*this, Table, WithLevels(TEXT("[1,2,3]")), TEXT("levels")).IsSuccess());
	const FAssetDocumentResult ValueChanged = Validate(*this, WithLevels(TEXT("[1,5,3]")), TEXT("validate a changed level"));
	TestTrue(*FString::Printf(TEXT("a changed number in a list is not a removal (%s)"), *ValueChanged.Message),
		ValueChanged.IsSuccess() && ValueChanged.Message.Contains(TEXT("would change")) && !ValueChanged.Message.Contains(TEXT("removing")));
	TestEqual(TEXT("a changed number lists no removal"), PayloadArray(ValueChanged, TEXT("removals")).Num(), 0);
	const FAssetDocumentResult Shorter = Validate(*this, WithLevels(TEXT("[1,3]")), TEXT("validate a dropped level"));
	TestTrue(*FString::Printf(TEXT("a shorter list removes one element (%s)"), *Shorter.Message),
		Shorter.IsSuccess() && Shorter.Message.Contains(TEXT("removing 1 entry")) && Shorter.Message.Contains(TEXT("/Levels/")));
	TestEqual(TEXT("a shorter list lists one removal"), PayloadArray(Shorter, TEXT("removals")).Num(), 1);
	return true;
}

// Summary and base revision:
//   - validate and diff of a DataTable document that changes two rows counted "1 value": every changed value is
//     counted and every changed, added and removed row named (the generic summary CurveTable and StringTable share);
//   - every successful apply and create returns base_revision (the BaseRevision of the next apply), not only
//     the apply of a document that carried one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentApplySemanticsChangedValueSummaryTest,
	"UEAssetDoc.ApplySemantics.ChangedValueSummary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentApplySemanticsChangedValueSummaryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorApplySemanticsTests;
	const FTarget Table(TEXT("DT_Summary"));
	const FTarget Generic(TEXT("DA_Summary"));
	ON_SCOPE_EXIT
	{
		Table.Cleanup();
		Generic.Cleanup();
	};
	const auto Revision = [](const FTarget& Target)
	{
		FAssetDocumentExtractRequest Request;
		Request.AssetPath = Target.ObjectPath;
		Request.bDiffOnly = true;
		Request.bIncludeBaseRevision = true;
		const FAssetDocumentResult Extracted = FAssetDocumentModule::Get().GetService().Extract(Request);
		FString Base;
		return Extracted.Payload.IsValid() && Extracted.Payload->TryGetStringField(TEXT("BaseRevision"), Base) ? Base : FString();
	};
	const FString RowStruct = FString::Printf(TEXT("\"RowStruct\":\"%s\",\"Rows\":"), TestRow);

	// Item 6: a create and an update without BaseRevision return the revision a fresh extract carries.
	const FAssetDocumentResult Created = Apply(*this, Table, Table.DataTable(TEXT("Create"), RowStruct
		+ TEXT("[{\"Name\":\"A\",\"Values\":{\"Count\":1}},{\"Name\":\"B\"},{\"Name\":\"C\",\"Values\":{\"Count\":3}}]")), TEXT("create"));
	if (!TestTrue(TEXT("create"), Created.IsSuccess()))
	{
		return false;
	}
	TestFalse(TEXT("a create returns base_revision"), PayloadString(Created, TEXT("base_revision")).IsEmpty());
	TestEqual(TEXT("the create's base_revision is the asset's"), PayloadString(Created, TEXT("base_revision")), Revision(Table));
	const FAssetDocumentResult GenericCreated = Apply(*this, Generic, Generic.Generic(TEXT("Create"), TEXT("{\"TestString\":\"one\"}")),
		TEXT("generic create"));
	TestTrue(TEXT("generic create"), GenericCreated.IsSuccess());
	TestEqual(TEXT("a generic create returns the asset's base_revision"), PayloadString(GenericCreated, TEXT("base_revision")), Revision(Generic));
	const FAssetDocumentResult GenericUpdated = Apply(*this, Generic, Generic.Generic(TEXT("Update"), TEXT("{\"TestString\":\"two\"}")),
		TEXT("generic update"));
	TestTrue(TEXT("generic update"), GenericUpdated.IsSuccess());
	TestEqual(TEXT("an update without BaseRevision returns the new base_revision"), PayloadString(GenericUpdated, TEXT("base_revision")),
		Revision(Generic));
	TestNotEqual(TEXT("the revision moved"), PayloadString(GenericUpdated, TEXT("base_revision")), PayloadString(GenericCreated, TEXT("base_revision")));

	// Item 3: two changed rows, one added and one removed.
	const FString Edit = Table.DataTable(TEXT("Update"), RowStruct
		+ TEXT("[{\"Name\":\"A\",\"Values\":{\"Count\":10}},{\"Name\":\"C\",\"Values\":{\"Count\":30}},{\"Name\":\"D\"}]"));
	const FAssetDocumentResult Summary = Validate(*this, Edit, TEXT("validate"));
	TestTrue(*FString::Printf(TEXT("validate counts every change (%s)"), *Summary.Message),
		Summary.IsSuccess() && Summary.Message.Contains(TEXT("would change 4 value(s)")));
	TestTrue(*FString::Printf(TEXT("validate names the changed and added rows (%s)"), *Summary.Message),
		Summary.Message.Contains(TEXT("in rows A, C (adding D)")));
	TestTrue(*FString::Printf(TEXT("validate names the removed row (%s)"), *Summary.Message), Summary.Message.Contains(TEXT("removing 1 entry: /Body/Rows (B)")));
	TestEqual(TEXT("validate lists every change"), PayloadArray(Summary, TEXT("changes")).Num(), 4);
	const FAssetDocumentResult Diffed = Diff(*this, Edit, TEXT("diff"));
	TestTrue(*FString::Printf(TEXT("diff names the rows (%s)"), *Diffed.Message),
		Diffed.IsSuccess() && Diffed.Message.Contains(TEXT("rows A, C (adding D)")) && Diffed.Message.Contains(TEXT("B")));
	TestEqual(TEXT("diff lists every difference"), PayloadArray(Diffed, TEXT("changed")).Num() + PayloadArray(Diffed, TEXT("added")).Num()
		+ PayloadArray(Diffed, TEXT("removed")).Num(), 4);
	const FAssetDocumentResult Updated = Apply(*this, Table, Edit, TEXT("apply"));
	TestTrue(TEXT("the edit applies"), Updated.IsSuccess());
	TestEqual(TEXT("an update returns the new base_revision"), PayloadString(Updated, TEXT("base_revision")), Revision(Table));
	return true;
}
#endif
