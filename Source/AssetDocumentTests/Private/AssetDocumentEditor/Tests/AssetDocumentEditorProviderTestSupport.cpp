// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentEditorProviderTestSupport.h"

#include "AssetDocumentEditorGenericProviderTestTypes.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Provider/AssetDocumentEditorTypedValueCodec.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "UObject/UnrealType.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorFoundationValueDtoTests
{
FAssetDocumentRuntimeValueDto MakeIntegerValue(int64 Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Integer;
	Result.IntegerValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeBooleanValue(bool bValue);
FAssetDocumentRuntimeValueDto MakeStringValue(FString Value);
bool AreRuntimeValuesEqual(
	const FAssetDocumentRuntimeValueDto& Left,
	const FAssetDocumentRuntimeValueDto& Right);

namespace AssetDocumentEditorFoundationTypedCodecTests
{
FProperty* FindFixtureProperty(const TCHAR* Name)
{
	for (TFieldIterator<FProperty> It(
		UAssetDocumentEditorCodecTestObject::StaticClass()); It; ++It)
	{
		if (It->GetAuthoredName() == Name)
		{
			return *It;
		}
	}
	return nullptr;
}

FAssetDocumentRuntimeValueDto MakeArrayValue(
	TArray<FAssetDocumentRuntimeValueDto> Elements)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Array;
	Result.Elements = MoveTemp(Elements);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeObjectValue(
	TArray<FString> Keys,
	TArray<FAssetDocumentRuntimeValueDto> Elements)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Elements);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeNumberValue(double Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Number;
	Result.NumberValue = Value;
	return Result;
}

void TestAdmittedRoundTrip(
	FAutomationTestBase& Test,
	const TCHAR* PropertyName,
	const FAssetDocumentRuntimeValueDto& Input)
{
	FProperty* Property = FindFixtureProperty(PropertyName);
	if (!Test.TestNotNull(
		FString::Printf(TEXT("%s property exists"), PropertyName), Property))
	{
		return;
	}
	const FAssetDocumentEditorTypedValueCodecResult Admission =
		InspectAssetDocumentEditorTypedValueProperty(*Property);
	Test.TestTrue(
		FString::Printf(TEXT("%s is admitted"), PropertyName),
		Admission.bSucceeded);
	Test.TestFalse(
		FString::Printf(TEXT("%s codec id is non-empty"), PropertyName),
		Admission.CodecId.IsEmpty());

	TUniquePtr<FAssetDocumentEditorScopedPropertyValue> NativeValue;
	const FAssetDocumentEditorTypedValueCodecResult Decode =
		DecodeAssetDocumentEditorTypedValue(*Property, Input, NativeValue);
	Test.TestTrue(
		FString::Printf(TEXT("%s decodes"), PropertyName),
		Decode.bSucceeded);
	if (!NativeValue)
	{
		return;
	}
	FAssetDocumentRuntimeValueDto Output;
	const FAssetDocumentEditorTypedValueCodecResult Encode =
		EncodeAssetDocumentEditorTypedValue(*NativeValue, Output);
	Test.TestTrue(
		FString::Printf(TEXT("%s encodes"), PropertyName),
		Encode.bSucceeded);
	Test.TestTrue(
		FString::Printf(TEXT("%s roundtrips"), PropertyName),
		AssetDocumentEditorFoundationValueDtoTests::AreRuntimeValuesEqual(
			Input, Output));
}

void TestUnsupportedProperty(
	FAutomationTestBase& Test,
	const TCHAR* PropertyName)
{
	FProperty* Property = FindFixtureProperty(PropertyName);
	if (!Test.TestNotNull(
		FString::Printf(TEXT("%s property exists"), PropertyName), Property))
	{
		return;
	}
	const FAssetDocumentEditorTypedValueCodecResult Admission =
		InspectAssetDocumentEditorTypedValueProperty(*Property);
	Test.TestFalse(
		FString::Printf(TEXT("%s fails closed before conversion"), PropertyName),
		Admission.bSucceeded);
	Test.TestEqual(
		FString::Printf(TEXT("%s uses stable unsupported diagnostic"), PropertyName),
		Admission.DiagnosticCode,
		FString(TEXT("TypedValueCodec.UnsupportedProperty")));
}
}

FAssetDocumentRuntimeValueDto MakeBooleanValue(bool bValue)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Result.BooleanValue = bValue;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeStringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

bool AreRuntimeValuesEqual(
	const FAssetDocumentRuntimeValueDto& Left,
	const FAssetDocumentRuntimeValueDto& Right)
{
	if (Left.Kind != Right.Kind
		|| Left.BooleanValue != Right.BooleanValue
		|| Left.IntegerValue != Right.IntegerValue
		|| Left.NumberValue != Right.NumberValue
		|| Left.StringValue != Right.StringValue
		|| Left.BytesValue != Right.BytesValue
		|| Left.ObjectKeys != Right.ObjectKeys
		|| Left.Elements.Num() != Right.Elements.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Left.Elements.Num(); ++Index)
	{
		if (!AreRuntimeValuesEqual(Left.Elements[Index], Right.Elements[Index]))
		{
			return false;
		}
	}
	return true;
}

FAssetDocumentRuntimeValueDto MakeNestedValidObject()
{
	FAssetDocumentRuntimeValueDto Nested;
	Nested.Kind = EAssetDocumentRuntimeValueKind::Object;
	Nested.ObjectKeys = {TEXT("flag")};
	Nested.Elements = {MakeBooleanValue(true)};
	return Nested;
}

void TestInvalidRuntimeShapes(
	FAutomationTestBase& Test,
	const FAssetDocumentRuntimeValueDto& ValidObject)
{
	FString DiagnosticCode;
	TSharedPtr<FJsonValue> JsonValue;

	FAssetDocumentRuntimeValueDto Bytes;
	Bytes.Kind = EAssetDocumentRuntimeValueKind::Bytes;
	Bytes.BytesValue = {0x01, 0x7f};
	Test.TestFalse(TEXT("bytes are not T01 JSON values"),
		TryConvertAssetDocumentEditorValueToJson(Bytes, JsonValue, DiagnosticCode));

	FAssetDocumentRuntimeValueDto NonFinite;
	NonFinite.Kind = EAssetDocumentRuntimeValueKind::Number;
	NonFinite.NumberValue = std::numeric_limits<double>::infinity();
	Test.TestFalse(TEXT("nonfinite number fails closed"),
		TryConvertAssetDocumentEditorValueToJson(NonFinite, JsonValue, DiagnosticCode));

	FAssetDocumentRuntimeValueDto NestedInvalid = ValidObject;
	NestedInvalid.Elements[1].Elements[0].IntegerValue = 1;
	Test.TestFalse(TEXT("nested inactive scalar field fails closed"),
		TryConvertAssetDocumentEditorValueToJson(NestedInvalid, JsonValue, DiagnosticCode));
}

void TestExactObjectReader(
	FAutomationTestBase& Test,
	const FAssetDocumentRuntimeValueDto& ValidObject)
{
	const TArray<FAssetDocumentEditorValueFieldSpec> ExactSpecs = {
		{TEXT("alpha"), EAssetDocumentRuntimeValueKind::Integer},
		{TEXT("nested"), EAssetDocumentRuntimeValueKind::Object},
	};
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString DiagnosticCode;
	Test.TestTrue(TEXT("exact keys and kinds are accepted"),
		TryReadAssetDocumentEditorExactObject(
			ValidObject, ExactSpecs, Values, DiagnosticCode));
	Test.TestEqual(TEXT("exact reader returns both values"), Values.Num(), 2);

	const TArray<FAssetDocumentEditorValueFieldSpec> MissingSpecs = {
		{TEXT("alpha"), EAssetDocumentRuntimeValueKind::Integer},
	};
	Test.TestFalse(TEXT("extra object key is rejected"),
		TryReadAssetDocumentEditorExactObject(
			ValidObject, MissingSpecs, Values, DiagnosticCode));

	const TArray<FAssetDocumentEditorValueFieldSpec> ExtraSpecs = {
		{TEXT("alpha"), EAssetDocumentRuntimeValueKind::Integer},
		{TEXT("nested"), EAssetDocumentRuntimeValueKind::Object},
		{TEXT("missing"), EAssetDocumentRuntimeValueKind::String},
	};
	Test.TestFalse(TEXT("missing object key is rejected"),
		TryReadAssetDocumentEditorExactObject(
			ValidObject, ExtraSpecs, Values, DiagnosticCode));

	TArray<FAssetDocumentEditorValueFieldSpec> WrongTypeSpecs = ExactSpecs;
	WrongTypeSpecs[0].Kind = EAssetDocumentRuntimeValueKind::String;
	Test.TestFalse(TEXT("wrong field kind is rejected"),
		TryReadAssetDocumentEditorExactObject(
			ValidObject, WrongTypeSpecs, Values, DiagnosticCode));
}

void TestDuplicateKeys(
	FAutomationTestBase& Test,
	const FAssetDocumentRuntimeValueDto& ValidObject)
{
	FAssetDocumentRuntimeValueDto Duplicate = ValidObject;
	Duplicate.ObjectKeys[1] = TEXT("alpha");
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	const TArray<FAssetDocumentEditorValueFieldSpec> DuplicateSpecs = {
		{TEXT("alpha"), EAssetDocumentRuntimeValueKind::Integer},
		{TEXT("alpha"), EAssetDocumentRuntimeValueKind::Object},
	};
	FString DiagnosticCode;
	Test.TestFalse(TEXT("duplicate object storage key is rejected"),
		TryReadAssetDocumentEditorExactObject(
			Duplicate, DuplicateSpecs, Values, DiagnosticCode));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderFoundationValueDtoShapeTest,
	"UEAssetDoc.GenericProvider.Foundation.ValueDtoShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderFoundationValueDtoShapeTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorFoundationValueDtoTests;

	const TArray<FAssetDocumentEditorValueField> Fields = {
		{TEXT("nested"), MakeNestedValidObject()},
		{TEXT("alpha"), MakeIntegerValue(7)},
	};
	FAssetDocumentRuntimeValueDto Expected;
	Expected.Kind = EAssetDocumentRuntimeValueKind::Object;
	Expected.ObjectKeys = {TEXT("alpha"), TEXT("nested")};
	Expected.Elements = {MakeIntegerValue(7), MakeNestedValidObject()};
	FAssetDocumentRuntimeValueDto Object;
	FString DiagnosticCode;
	TestTrue(TEXT("valid exact object builds"),
		TryBuildAssetDocumentEditorValueObject(Fields, Object, DiagnosticCode));
	TestEqual(TEXT("builder emits deterministic key order"),
		Object.ObjectKeys, TArray<FString>({TEXT("alpha"), TEXT("nested")}));

	TestExactObjectReader(*this, Expected);
	TestDuplicateKeys(*this, Expected);
	TestInvalidRuntimeShapes(*this, Expected);

	TSharedPtr<FJsonValue> JsonValue;
	FAssetDocumentRuntimeValueDto RoundTripped;
	TestTrue(TEXT("valid object projects to JSON"),
		TryConvertAssetDocumentEditorValueToJson(Expected, JsonValue, DiagnosticCode));
	TestTrue(TEXT("valid JSON projects to a runtime value"),
		TryConvertAssetDocumentEditorJsonToValue(
			JsonValue, RoundTripped, DiagnosticCode));
	TestTrue(TEXT("valid object roundtrip preserves canonical value"),
		AreRuntimeValuesEqual(Expected, RoundTripped));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderFoundationStrictTypedCodecTest,
	"UEAssetDoc.GenericProvider.Foundation.StrictTypedCodec",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderFoundationStrictTypedCodecTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorFoundationValueDtoTests::
		AssetDocumentEditorFoundationTypedCodecTests;
	using namespace AssetDocumentEditorFoundationValueDtoTests;

	TestAdmittedRoundTrip(*this, TEXT("BooleanValue"), MakeBooleanValue(true));
	TestAdmittedRoundTrip(*this, TEXT("IntegerValue"), MakeIntegerValue(71));
	TestAdmittedRoundTrip(*this, TEXT("NumberValue"), MakeNumberValue(3.25));
	TestAdmittedRoundTrip(*this, TEXT("StringValue"), MakeStringValue(TEXT("typed")));
	TestAdmittedRoundTrip(*this, TEXT("NameValue"), MakeStringValue(TEXT("Named")));
	TestAdmittedRoundTrip(*this, TEXT("EnumValue"), MakeStringValue(TEXT("Second")));
	TestAdmittedRoundTrip(
		*this, TEXT("SoftObjectValue"),
		MakeStringValue(TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture")));
	TestAdmittedRoundTrip(
		*this, TEXT("SoftClassValue"),
		MakeStringValue(TEXT("/Script/CoreUObject.Object")));
	TestAdmittedRoundTrip(
		*this, TEXT("FixedArrayValue"),
		MakeArrayValue({MakeIntegerValue(3), MakeIntegerValue(5)}));
	TestAdmittedRoundTrip(
		*this, TEXT("ArrayValue"),
		MakeArrayValue({MakeIntegerValue(7), MakeIntegerValue(11)}));
	TestAdmittedRoundTrip(
		*this, TEXT("SetValue"),
		MakeArrayValue({MakeStringValue(TEXT("only"))}));
	TestAdmittedRoundTrip(
		*this, TEXT("StringMapValue"),
		MakeObjectValue({TEXT("key")}, {MakeIntegerValue(13)}));
	TestAdmittedRoundTrip(
		*this, TEXT("NestedValue"),
		MakeObjectValue(
			{TEXT("Count"), TEXT("Label")},
			{MakeIntegerValue(17), MakeStringValue(TEXT("nested"))}));

	// Hard, weak, lazy and interface references travel as object paths or null.
	const FAssetDocumentRuntimeValueDto NullReference;
	TestAdmittedRoundTrip(
		*this, TEXT("HardObjectValue"),
		MakeStringValue(TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture")));
	TestAdmittedRoundTrip(*this, TEXT("HardObjectValue"), NullReference);
	TestAdmittedRoundTrip(
		*this, TEXT("WeakObjectValue"),
		MakeStringValue(TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture")));
	TestAdmittedRoundTrip(*this, TEXT("LazyObjectValue"), NullReference);
	TestAdmittedRoundTrip(*this, TEXT("InterfaceValue"), NullReference);

	// Integer map keys (FPerQualityLevelInt::PerQuality) travel as their decimal text; only that canonical
	// spelling decodes. (Runtime object keys are sorted, so the input lists them in that order.)
	TestAdmittedRoundTrip(
		*this, TEXT("IntegerMapValue"),
		MakeObjectValue({TEXT("-1"), TEXT("0"), TEXT("3")},
			{MakeStringValue(TEXT("minus one")), MakeStringValue(TEXT("zero")), MakeStringValue(TEXT("three"))}));
	if (FProperty* IntegerMap = FindFixtureProperty(TEXT("IntegerMapValue")))
	{
		for (const TCHAR* Key : {TEXT("03"), TEXT("+3"), TEXT("three"), TEXT("2147483648"), TEXT("-0")})
		{
			TUniquePtr<FAssetDocumentEditorScopedPropertyValue> NativeValue;
			const FAssetDocumentEditorTypedValueCodecResult Decode = DecodeAssetDocumentEditorTypedValue(
				*IntegerMap, MakeObjectValue({Key}, {MakeStringValue(TEXT("x"))}), NativeValue);
			TestFalse(FString::Printf(TEXT("integer map key '%s' is refused"), Key), Decode.bSucceeded);
			TestTrue(FString::Printf(TEXT("integer map key '%s' refusal points at the key"), Key),
				Decode.ValuePath.Contains(Key) && Decode.Message.Contains(TEXT("decimal integer")));
		}
	}

	for (const TCHAR* Unsupported : {
		TEXT("DelegateValue")})
	{
		TestUnsupportedProperty(*this, Unsupported);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderFoundationTypedCodecRaiiFailureTest,
	"UEAssetDoc.GenericProvider.Foundation.TypedCodecRaiiFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderFoundationTypedCodecRaiiFailureTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorFoundationValueDtoTests;
	using namespace AssetDocumentEditorFoundationValueDtoTests::
		AssetDocumentEditorFoundationTypedCodecTests;

	FProperty* Property = FindFixtureProperty(TEXT("TrackedValue"));
	if (!TestNotNull(TEXT("tracked struct property exists"), Property))
	{
		return false;
	}

	const FAssetDocumentRuntimeValueDto ValidValue = MakeObjectValue(
		{TEXT("EnumValue"), TEXT("Value")},
		{MakeStringValue(TEXT("Second")), MakeIntegerValue(23)});
	FAssetDocumentEditorCodecTrackedValue::ResetLifetimeCounts();
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue> NativeValue;
	const FAssetDocumentEditorTypedValueCodecResult Success =
		DecodeAssetDocumentEditorTypedValue(*Property, ValidValue, NativeValue);
	TestTrue(TEXT("tracked native temporary initializes successfully"),
		Success.bSucceeded);
	TestEqual(TEXT("success initializes exactly once"),
		FAssetDocumentEditorCodecTrackedValue::ConstructionCount(), 1);
	NativeValue.Reset();
	TestEqual(TEXT("success destroys exactly once"),
		FAssetDocumentEditorCodecTrackedValue::DestructionCount(), 1);

	const FAssetDocumentRuntimeValueDto InvalidValue = MakeObjectValue(
		{TEXT("EnumValue"), TEXT("Value")},
		{MakeStringValue(TEXT("DoesNotExist")), MakeIntegerValue(29)});
	FAssetDocumentEditorCodecTrackedValue::ResetLifetimeCounts();
	const FAssetDocumentEditorTypedValueCodecResult Failure =
		DecodeAssetDocumentEditorTypedValue(*Property, InvalidValue, NativeValue);
	TestFalse(TEXT("converter failure fails closed"), Failure.bSucceeded);
	TestNull(TEXT("converter failure returns no native temporary"), NativeValue.Get());
	// The value check rejects the unknown enumerator before a temporary exists;
	// whatever was constructed must be destroyed exactly once.
	TestTrue(TEXT("failure initializes at most once"),
		FAssetDocumentEditorCodecTrackedValue::ConstructionCount() <= 1);
	TestEqual(TEXT("failure destroys every temporary it initialized"),
		FAssetDocumentEditorCodecTrackedValue::DestructionCount(),
		FAssetDocumentEditorCodecTrackedValue::ConstructionCount());
	return true;
}
#endif
