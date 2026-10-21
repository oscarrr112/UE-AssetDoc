// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentCanonicalJson.h"
#include "AssetDocumentPolicy.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace CanonicalJsonTestsLocal
{
TSharedPtr<FJsonValue> CanonicalJsonTestMakeObjectValue(TSharedRef<FJsonObject> Object)
{
	return MakeShared<FJsonValueObject>(Object);
}

TSharedPtr<FJsonValue> MakeArrayValue(TArray<TSharedPtr<FJsonValue>> Values)
{
	return MakeShared<FJsonValueArray>(MoveTemp(Values));
}

TSharedPtr<FJsonValue> MakeStringValue(const FString& Value)
{
	return MakeShared<FJsonValueString>(Value);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentCanonicalJsonObjectOrderStableTest,
	"UEAssetDoc.CanonicalJson.ObjectOrderStable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCanonicalJsonObjectOrderStableTest::RunTest(const FString& Parameters)
{
	using namespace CanonicalJsonTestsLocal;
	TSharedRef<FJsonObject> FirstObject = MakeShared<FJsonObject>();
	FirstObject->SetNumberField(TEXT("A"), 1.0);
	FirstObject->SetNumberField(TEXT("B"), 2.0);

	TSharedRef<FJsonObject> SecondObject = MakeShared<FJsonObject>();
	SecondObject->SetNumberField(TEXT("B"), 2.0);
	SecondObject->SetNumberField(TEXT("A"), 1.0);

	const FString FirstHash = FAssetDocumentCanonicalJson::HashJsonValue(CanonicalJsonTestMakeObjectValue(FirstObject));
	const FString SecondHash = FAssetDocumentCanonicalJson::HashJsonValue(CanonicalJsonTestMakeObjectValue(SecondObject));

	TestTrue(TEXT("Hash uses an explicit algorithm prefix"), FirstHash.StartsWith(TEXT("sha1:")));
	TestEqual(TEXT("Object key order does not change hash"), FirstHash, SecondHash);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentCanonicalJsonArrayOrderMattersTest,
	"UEAssetDoc.CanonicalJson.ArrayOrderMatters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCanonicalJsonArrayOrderMattersTest::RunTest(const FString& Parameters)
{
	using namespace CanonicalJsonTestsLocal;
	const TSharedPtr<FJsonValue> FirstArray = MakeArrayValue({
		MakeShared<FJsonValueNumber>(1.0),
		MakeShared<FJsonValueNumber>(2.0)
	});
	const TSharedPtr<FJsonValue> SecondArray = MakeArrayValue({
		MakeShared<FJsonValueNumber>(2.0),
		MakeShared<FJsonValueNumber>(1.0)
	});

	TestNotEqual(
		TEXT("Array item order changes hash"),
		FAssetDocumentCanonicalJson::HashJsonValue(FirstArray),
		FAssetDocumentCanonicalJson::HashJsonValue(SecondArray));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentCanonicalJsonExtractOnlyFieldsIgnoredTest,
	"UEAssetDoc.CanonicalJson.ExtractOnlyFieldsIgnored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCanonicalJsonExtractOnlyFieldsIgnoredTest::RunTest(const FString& Parameters)
{
	using namespace CanonicalJsonTestsLocal;
	TSharedRef<FJsonObject> BaselineObject = MakeShared<FJsonObject>();
	BaselineObject->SetStringField(TEXT("Name"), TEXT("ManagedNotify"));
	BaselineObject->SetNumberField(TEXT("Frame"), 12.0);

	TSharedRef<FJsonObject> WithExtractOnlyObject = MakeShared<FJsonObject>();
	WithExtractOnlyObject->SetStringField(TEXT("Name"), TEXT("ManagedNotify"));
	WithExtractOnlyObject->SetNumberField(TEXT("Frame"), 12.0);
	WithExtractOnlyObject->SetObjectField(TEXT("_Skipped"), MakeShared<FJsonObject>());
	WithExtractOnlyObject->SetStringField(TEXT("_meta"), TEXT("extract diagnostics"));
	WithExtractOnlyObject->SetStringField(TEXT("_ProjectionMetrics"), TEXT("diagnostic"));

	FAssetDocumentRegionPolicy Policy;
	Policy.ExtractOnlyFields.Add(TEXT("_ProjectionMetrics"));

	const FString BaselineHash = FAssetDocumentCanonicalJson::HashJsonValue(CanonicalJsonTestMakeObjectValue(BaselineObject), &Policy);
	const FString ExtractOnlyHash = FAssetDocumentCanonicalJson::HashJsonValue(CanonicalJsonTestMakeObjectValue(WithExtractOnlyObject), &Policy);
	TestEqual(TEXT("Default and policy extract-only fields do not change hash"), BaselineHash, ExtractOnlyHash);

	TSharedPtr<FJsonValue> Clone = FAssetDocumentCanonicalJson::CloneWithoutExtractOnlyFields(CanonicalJsonTestMakeObjectValue(WithExtractOnlyObject), &Policy);
	TestTrue(TEXT("Clone remains an object"), Clone.IsValid() && Clone->Type == EJson::Object);
	if (Clone.IsValid() && Clone->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> CloneObject = Clone->AsObject();
		TestFalse(TEXT("Clone removes _Skipped"), CloneObject->HasField(TEXT("_Skipped")));
		TestFalse(TEXT("Clone removes _meta"), CloneObject->HasField(TEXT("_meta")));
		TestFalse(TEXT("Clone removes policy ExtractOnlyFields"), CloneObject->HasField(TEXT("_ProjectionMetrics")));
		TestTrue(TEXT("Clone keeps ordinary fields"), CloneObject->HasField(TEXT("Name")));
	}

	TSharedRef<FJsonObject> ChangedObject = MakeShared<FJsonObject>();
	ChangedObject->SetStringField(TEXT("Name"), TEXT("ManagedNotify"));
	ChangedObject->SetNumberField(TEXT("Frame"), 13.0);
	ChangedObject->SetObjectField(TEXT("_Skipped"), MakeShared<FJsonObject>());
	ChangedObject->SetStringField(TEXT("_meta"), TEXT("extract diagnostics"));
	ChangedObject->SetStringField(TEXT("_ProjectionMetrics"), TEXT("diagnostic"));

	TestNotEqual(
		TEXT("Ordinary field changes still change hash"),
		BaselineHash,
		FAssetDocumentCanonicalJson::HashJsonValue(CanonicalJsonTestMakeObjectValue(ChangedObject), &Policy));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentCanonicalJsonWritesExpectedStringsTest,
	"UEAssetDoc.CanonicalJson.WritesExpectedStrings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCanonicalJsonWritesExpectedStringsTest::RunTest(const FString& Parameters)
{
	using namespace CanonicalJsonTestsLocal;
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("B"), 2.0);
	Object->SetNumberField(TEXT("A"), 1.0);
	TestEqual(
		TEXT("Canonical object keys sort lexically"),
		FAssetDocumentCanonicalJson::WriteCanonicalJson(CanonicalJsonTestMakeObjectValue(Object)),
		FString(TEXT("{\"A\":1,\"B\":2}")));

	TestEqual(
		TEXT("Canonical arrays preserve item order"),
		FAssetDocumentCanonicalJson::WriteCanonicalJson(MakeArrayValue({
			MakeShared<FJsonValueNumber>(2.0),
			MakeShared<FJsonValueNumber>(1.0)
		})),
		FString(TEXT("[2,1]")));

	TestEqual(
		TEXT("Canonical escaping handles control characters"),
		FAssetDocumentCanonicalJson::WriteCanonicalJson(MakeStringValue(FString(TEXT("line\n\t\"quote\"\\")))),
		FString(TEXT("\"line\\n\\t\\\"quote\\\"\\\\\"")));

	TSharedRef<FJsonObject> FilteredObject = MakeShared<FJsonObject>();
	FilteredObject->SetNumberField(TEXT("Value"), 7.0);
	FilteredObject->SetStringField(TEXT("_Skipped"), TEXT("diagnostic"));
	FilteredObject->SetStringField(TEXT("_ProjectionMetrics"), TEXT("diagnostic"));
	FAssetDocumentRegionPolicy Policy;
	Policy.ExtractOnlyFields.Add(TEXT("_ProjectionMetrics"));
	TestEqual(
		TEXT("Canonical writing filters extract-only fields"),
		FAssetDocumentCanonicalJson::WriteCanonicalJson(CanonicalJsonTestMakeObjectValue(FilteredObject), &Policy),
		FString(TEXT("{\"Value\":7}")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentCanonicalJsonFormatsNumbersTest,
	"UEAssetDoc.CanonicalJson.FormatsNumbers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCanonicalJsonFormatsNumbersTest::RunTest(const FString& Parameters)
{
	TestEqual(
		TEXT("Canonical number preserves simple fraction"),
		FAssetDocumentCanonicalJson::WriteCanonicalJson(MakeShared<FJsonValueNumber>(1.25)),
		FString(TEXT("1.25")));
	TestEqual(
		TEXT("Canonical number normalizes large exponent"),
		FAssetDocumentCanonicalJson::WriteCanonicalJson(MakeShared<FJsonValueNumber>(1.0e20)),
		FString(TEXT("1e20")));
	TestEqual(
		TEXT("Canonical number normalizes small exponent"),
		FAssetDocumentCanonicalJson::WriteCanonicalJson(MakeShared<FJsonValueNumber>(1.0e-7)),
		FString(TEXT("1e-7")));
	TestEqual(
		TEXT("Canonical integer double omits fractional suffix"),
		FAssetDocumentCanonicalJson::WriteCanonicalJson(MakeShared<FJsonValueNumber>(42.0)),
		FString(TEXT("42")));
	TestEqual(
		TEXT("Canonical negative zero normalizes to zero"),
		FAssetDocumentCanonicalJson::WriteCanonicalJson(MakeShared<FJsonValueNumber>(-0.0)),
		FString(TEXT("0")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentCanonicalJsonEscapesSurrogatesTest,
	"UEAssetDoc.CanonicalJson.EscapesSurrogates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCanonicalJsonEscapesSurrogatesTest::RunTest(const FString& Parameters)
{
	using namespace CanonicalJsonTestsLocal;
	FString ValidPair;
	ValidPair.AppendChar(static_cast<TCHAR>(0xD83D));
	ValidPair.AppendChar(static_cast<TCHAR>(0xDE00));
	TestEqual(
		TEXT("Canonical string escapes valid surrogate pairs"),
		FAssetDocumentCanonicalJson::WriteCanonicalJson(MakeStringValue(ValidPair)),
		FString(TEXT("\"\\ud83d\\ude00\"")));

	FString UnpairedHigh;
	UnpairedHigh.AppendChar(static_cast<TCHAR>(0xD83D));
	TestEqual(
		TEXT("Canonical string escapes unpaired high surrogate"),
		FAssetDocumentCanonicalJson::WriteCanonicalJson(MakeStringValue(UnpairedHigh)),
		FString(TEXT("\"\\ud83d\"")));

	FString UnpairedLow;
	UnpairedLow.AppendChar(static_cast<TCHAR>(0xDE00));
	TestEqual(
		TEXT("Canonical string escapes unpaired low surrogate"),
		FAssetDocumentCanonicalJson::WriteCanonicalJson(MakeStringValue(UnpairedLow)),
		FString(TEXT("\"\\ude00\"")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentCanonicalJsonFiltersReservedFieldsDeepTest,
	"UEAssetDoc.CanonicalJson.FiltersReservedFieldsDeep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCanonicalJsonFiltersReservedFieldsDeepTest::RunTest(const FString& Parameters)
{
	using namespace CanonicalJsonTestsLocal;
	TSharedRef<FJsonObject> NestedObject = MakeShared<FJsonObject>();
	NestedObject->SetStringField(TEXT("_meta"), TEXT("nested diagnostic"));
	NestedObject->SetStringField(TEXT("Kept"), TEXT("nested value"));

	TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetObjectField(TEXT("Nested"), NestedObject);
	RootObject->SetStringField(TEXT("_meta"), TEXT("root diagnostic"));

	TestEqual(
		TEXT("Canonical filtering removes reserved names at any depth"),
		FAssetDocumentCanonicalJson::WriteCanonicalJson(CanonicalJsonTestMakeObjectValue(RootObject)),
		FString(TEXT("{\"Nested\":{\"Kept\":\"nested value\"}}")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentCanonicalJsonNormalizesNullInputTest,
	"UEAssetDoc.CanonicalJson.NormalizesNullInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCanonicalJsonNormalizesNullInputTest::RunTest(const FString& Parameters)
{
	TestEqual(
		TEXT("Invalid values write as JSON null"),
		FAssetDocumentCanonicalJson::WriteCanonicalJson(nullptr),
		FString(TEXT("null")));

	const TSharedPtr<FJsonValue> Clone = FAssetDocumentCanonicalJson::CloneWithoutExtractOnlyFields(nullptr);
	TestTrue(TEXT("Invalid values clone to a valid null value"), Clone.IsValid() && Clone->Type == EJson::Null);

	return true;
}

#endif
