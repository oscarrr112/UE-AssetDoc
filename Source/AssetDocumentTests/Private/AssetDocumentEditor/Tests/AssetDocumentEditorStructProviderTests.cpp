// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorPrimitiveProvider.h"

#include "Animation/AnimSequence.h"
#include "AssetDocumentEditorGenericProviderTestTypes.h"
#include "AssetDocumentEditorProviderTestSupport.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorStructProviderTests
{
const FAssetDocumentPrimitiveDescriptor* FindStructDescriptor()
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor :
		CreateAssetDocumentEditorPrimitiveProvider()->DescribePrimitives())
	{
		if (Descriptor.PrimitiveId == TEXT("editor.fragment.struct_codec.v1"))
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

FAssetDocumentRuntimeValueDto MakeStringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeIntegerValue(int64 Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Integer;
	Result.IntegerValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeNumberValue(double Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Number;
	Result.NumberValue = Value;
	return Result;
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
	TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Values);
	return Result;
}

FAssetDocumentPrimitiveInvocation MakeStructInvocation(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentRuntimeValueDto Arguments);

bool AreValuesEqual(
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
		if (!AreValuesEqual(Left.Elements[Index], Right.Elements[Index]))
		{
			return false;
		}
	}
	return true;
}

const FAssetDocumentRuntimeValueDto* FindObjectField(
	const FAssetDocumentRuntimeValueDto& Object,
	FStringView Name)
{
	const int32 Index = Object.ObjectKeys.Find(FString(Name));
	return Object.Elements.IsValidIndex(Index)
		? &Object.Elements[Index]
		: nullptr;
}

TOptional<FAssetDocumentOpaqueHandle> ResolveStructSchemaToken(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorProviderTestContext& Context,
	IAssetDocumentPrimitiveProvider& Provider,
	FAssetDocumentEditorProviderTestHandleResolver& Resolver,
	const FString& StructPath)
{
	const FAssetDocumentPrimitiveOutcome Resolve = Provider.Invoke(
		MakeStructInvocation(
			Descriptor,
			Context,
			MakeObjectValue(
				{TEXT("operation"), TEXT("struct")},
				{MakeStringValue(TEXT("resolve_schema")),
				 MakeStringValue(StructPath)})),
		Resolver,
		nullptr);
	Test.TestEqual(TEXT("typed schema resolves"), Resolve.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	if (Resolve.HandleResults.Num() != 1
		|| Resolve.HandleResults[0].Handles.Num() != 1)
	{
		Test.AddError(TEXT("typed schema must issue exactly one token"));
		return {};
	}
	return Resolve.HandleResults[0].Handles[0];
}

FAssetDocumentPrimitiveOutcome InvokeTypedStructValidation(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorProviderTestContext& Context,
	IAssetDocumentPrimitiveProvider& Provider,
	FAssetDocumentEditorProviderTestHandleResolver& Resolver,
	const FAssetDocumentOpaqueHandle& SchemaHandle,
	const FAssetDocumentRuntimeValueDto& StructValue)
{
	FAssetDocumentPrimitiveInvocation Validate = MakeStructInvocation(
		Descriptor,
		Context,
		MakeObjectValue(
			{TEXT("operation"), TEXT("value")},
			{MakeStringValue(TEXT("validate")), StructValue}));
	Validate.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
	FAssetDocumentPrimitiveHandleArgument& SchemaArgument =
		Validate.HandleArguments.AddDefaulted_GetRef();
	SchemaArgument.Name = TEXT("schema");
	SchemaArgument.Handle = SchemaHandle;
	return Provider.Invoke(Validate, Resolver, nullptr);
}

bool TestTypedStructRoundTrip(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorProviderTestContext& Context,
	IAssetDocumentPrimitiveProvider& Provider,
	FAssetDocumentEditorProviderTestHandleResolver& Resolver,
	UScriptStruct& Schema,
	const FAssetDocumentRuntimeValueDto& StructValue)
{
	const TOptional<FAssetDocumentOpaqueHandle> SchemaHandle =
		ResolveStructSchemaToken(
			Test, Descriptor, Context, Provider, Resolver,
			Schema.GetPathName());
	if (!SchemaHandle.IsSet())
	{
		return false;
	}
	const FAssetDocumentPrimitiveOutcome Outcome =
		InvokeTypedStructValidation(
			Descriptor, Context, Provider, Resolver,
			SchemaHandle.GetValue(), StructValue);
	const bool bSucceeded = Test.TestEqual(
		TEXT("typed struct validates"), Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	const FAssetDocumentRuntimeValueDto* Canonical =
		FindObjectField(Outcome.Value, TEXT("canonical_value"));
	const bool bCanonical = Test.TestTrue(
		TEXT("typed roundtrip preserves the canonical value"),
		Canonical != nullptr && AreValuesEqual(StructValue, *Canonical));
	const bool bNoHandles = Test.TestTrue(
		TEXT("validate returns no handles"),
		Outcome.HandleResults.IsEmpty());
	return bSucceeded && bCanonical && bNoHandles;
}

bool HasDiagnostic(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const TCHAR* Code)
{
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Outcome.Diagnostics)
	{
		if (Diagnostic.Code == Code)
		{
			return true;
		}
	}
	return false;
}

FAssetDocumentPrimitiveInvocation MakeStructInvocation(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentRuntimeValueDto Arguments)
{
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(Descriptor, Context);
	Invocation.ValueArguments = MoveTemp(Arguments);
	return Invocation;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderStructFixedDescriptorShapeTest,
	"UEAssetDoc.GenericProvider.Struct.FixedDescriptorShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderStructFixedDescriptorShapeTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorStructProviderTests;
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindStructDescriptor();
	if (!TestNotNull(TEXT("struct descriptor exists"), Descriptor))
	{
		return false;
	}
	TestEqual(TEXT("struct access is scratch"), Descriptor->Access,
		EAssetDocumentPrimitiveAccess::Scratch);
	TestFalse(TEXT("struct descriptor requires no transaction"),
		Descriptor->bRequiresActiveTransaction);
	TestEqual(TEXT("struct phases are fixed"),
		Descriptor->AllowedPhases,
		TArray<EAssetDocumentRuntimeRequestPhase>({
			EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Verify}));
	TestEqual(TEXT("one optional schema argument"),
		Descriptor->HandleArgumentSchemas.Num(), 1);
	if (Descriptor->HandleArgumentSchemas.Num() == 1)
	{
		const FAssetDocumentPrimitiveHandleArgumentSchema& Schema =
			Descriptor->HandleArgumentSchemas[0];
		TestEqual(TEXT("schema argument name"), Schema.Name, FString(TEXT("schema")));
		TestEqual(TEXT("schema argument kind"), Schema.Kind,
			EAssetDocumentHandleKind::Struct);
		TestEqual(TEXT("schema argument access"), Schema.RequiredAccess,
			EAssetDocumentHandleAccess::ReadOnly);
		TestFalse(TEXT("schema argument is optional"), Schema.bRequired);
	}
	TestEqual(TEXT("one schema result"), Descriptor->HandleResultSchemas.Num(), 1);
	if (Descriptor->HandleResultSchemas.Num() == 1)
	{
		const FAssetDocumentPrimitiveHandleResultSchema& Schema =
			Descriptor->HandleResultSchemas[0];
		TestEqual(TEXT("schema result name"), Schema.Name, FString(TEXT("schema")));
		TestEqual(TEXT("schema result kind"), Schema.Kind,
			EAssetDocumentHandleKind::Struct);
		TestEqual(TEXT("schema result access"), Schema.GrantedAccess,
			EAssetDocumentHandleAccess::ReadOnly);
		TestEqual(TEXT("schema result is optional"), Schema.Cardinality,
			EAssetDocumentHandleResultCardinality::ZeroOrOne);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderStructSchemaTokenRequiredTest,
	"UEAssetDoc.GenericProvider.Struct.SchemaTokenRequired",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderStructSchemaTokenRequiredTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorStructProviderTests;
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindStructDescriptor();
	if (!TestNotNull(TEXT("struct descriptor exists"), Descriptor))
	{
		return false;
	}
	const FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Resolver(Context);
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FString StructPath =
		FAssetDocumentEditorCodecNestedValue::StaticStruct()->GetPathName();
	const FAssetDocumentRuntimeValueDto StructValue = MakeObjectValue(
		{TEXT("Count"), TEXT("Label")},
		{MakeIntegerValue(31), MakeStringValue(TEXT("schema-bound"))});

	const FAssetDocumentPrimitiveOutcome MissingToken = Provider->Invoke(
		MakeStructInvocation(
			*Descriptor,
			Context,
			MakeObjectValue(
				{TEXT("operation"), TEXT("value")},
				{MakeStringValue(TEXT("validate")), StructValue})),
		Resolver,
		nullptr);
	TestEqual(TEXT("validate without token fails"), MissingToken.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("validate without token has stable diagnostic"),
		HasDiagnostic(MissingToken, TEXT("StructCodec.SchemaTokenRequired")));

	const FAssetDocumentPrimitiveOutcome PathOnly = Provider->Invoke(
		MakeStructInvocation(
			*Descriptor,
			Context,
			MakeObjectValue(
				{TEXT("operation"), TEXT("struct"), TEXT("value")},
				{MakeStringValue(TEXT("validate")),
				 MakeStringValue(StructPath), StructValue})),
		Resolver,
		nullptr);
	TestEqual(TEXT("path-only validate fails"), PathOnly.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("path-only validate issues no handle"),
		PathOnly.HandleResults.IsEmpty());

	const FAssetDocumentPrimitiveOutcome Resolve = Provider->Invoke(
		MakeStructInvocation(
			*Descriptor,
			Context,
			MakeObjectValue(
				{TEXT("operation"), TEXT("struct")},
				{MakeStringValue(TEXT("resolve_schema")),
				 MakeStringValue(StructPath)})),
		Resolver,
		nullptr);
	TestEqual(TEXT("canonical schema resolves"), Resolve.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	if (Resolve.HandleResults.Num() != 1
		|| Resolve.HandleResults[0].Handles.Num() != 1)
	{
		AddError(TEXT("resolve must issue exactly one schema token"));
		return true;
	}

	FAssetDocumentPrimitiveInvocation Validate = MakeStructInvocation(
		*Descriptor,
		Context,
		MakeObjectValue(
			{TEXT("operation"), TEXT("value")},
			{MakeStringValue(TEXT("validate")), StructValue}));
	FAssetDocumentPrimitiveHandleArgument& SchemaArgument =
		Validate.HandleArguments.AddDefaulted_GetRef();
	SchemaArgument.Name = TEXT("schema");
	SchemaArgument.Handle = Resolve.HandleResults[0].Handles[0];
	Validate.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
	const FAssetDocumentPrimitiveOutcome Validated =
		Provider->Invoke(Validate, Resolver, nullptr);
	TestEqual(TEXT("token-bound validate succeeds"), Validated.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestTrue(TEXT("validate returns no handles"),
		Validated.HandleResults.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderStructTypedRoundTripTest,
	"UEAssetDoc.GenericProvider.Struct.TypedRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderStructTypedRoundTripTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorStructProviderTests;
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindStructDescriptor();
	if (!TestNotNull(TEXT("struct descriptor exists"), Descriptor))
	{
		return false;
	}
	const FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Resolver(Context);
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentRuntimeValueDto NestedValue = MakeObjectValue(
		{TEXT("Count"), TEXT("Label")},
		{MakeIntegerValue(37), MakeStringValue(TEXT("nested"))});
	const bool bNested = TestTypedStructRoundTrip(
		*this, *Descriptor, Context, *Provider, Resolver,
		*FAssetDocumentEditorCodecNestedValue::StaticStruct(), NestedValue);
	const FAssetDocumentRuntimeValueDto AggregateValue = MakeObjectValue(
		{TEXT("ByteStream"), TEXT("Mins"), TEXT("Ranges"), TEXT("Times")},
		{
			MakeArrayValue({MakeIntegerValue(3), MakeIntegerValue(5)}),
			MakeArrayValue({
				MakeNumberValue(1.0),
				MakeNumberValue(2.0),
				MakeNumberValue(3.0)}),
			MakeArrayValue({
				MakeNumberValue(4.0),
				MakeNumberValue(5.0),
				MakeNumberValue(6.0)}),
			MakeArrayValue({MakeNumberValue(0.25), MakeNumberValue(0.75)}),
		});
	const bool bFixedAndContainer = TestTypedStructRoundTrip(
		*this, *Descriptor, Context, *Provider, Resolver,
		*FCompressedTrack::StaticStruct(), AggregateValue);
	return bNested && bFixedAndContainer;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderStructRaiiLifetimeTest,
	"UEAssetDoc.GenericProvider.Struct.RaiiLifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderStructRaiiLifetimeTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorStructProviderTests;
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindStructDescriptor();
	if (!TestNotNull(TEXT("struct descriptor exists"), Descriptor))
	{
		return false;
	}
	const FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Resolver(Context);
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const TOptional<FAssetDocumentOpaqueHandle> SchemaHandle =
		ResolveStructSchemaToken(
			*this, *Descriptor, Context, *Provider, Resolver,
			FAssetDocumentEditorCodecTrackedValue::StaticStruct()->GetPathName());
	if (!SchemaHandle.IsSet())
	{
		return false;
	}

	const FAssetDocumentRuntimeValueDto ValidValue = MakeObjectValue(
		{TEXT("EnumValue"), TEXT("Value")},
		{MakeStringValue(TEXT("Second")), MakeIntegerValue(41)});
	FAssetDocumentEditorCodecTrackedValue::ResetLifetimeCounts();
	const FAssetDocumentPrimitiveOutcome Success =
		InvokeTypedStructValidation(
			*Descriptor, Context, *Provider, Resolver,
			SchemaHandle.GetValue(), ValidValue);
	TestEqual(TEXT("valid tracked struct succeeds"), Success.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestEqual(TEXT("success initializes the struct exactly once"),
		FAssetDocumentEditorCodecTrackedValue::ConstructionCount(), 1);
	TestEqual(TEXT("success destroys the struct exactly once"),
		FAssetDocumentEditorCodecTrackedValue::DestructionCount(), 1);

	// The typed value check rejects the unknown enumerator before the JSON
	// importer runs, so no LogJson error is expected.
	const FAssetDocumentRuntimeValueDto InvalidValue = MakeObjectValue(
		{TEXT("EnumValue"), TEXT("Value")},
		{MakeStringValue(TEXT("DoesNotExist")), MakeIntegerValue(43)});
	FAssetDocumentEditorCodecTrackedValue::ResetLifetimeCounts();
	const FAssetDocumentPrimitiveOutcome Failure =
		InvokeTypedStructValidation(
			*Descriptor, Context, *Provider, Resolver,
			SchemaHandle.GetValue(), InvalidValue);
	TestEqual(TEXT("invalid tracked struct fails"), Failure.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestEqual(TEXT("failure initializes the struct exactly once"),
		FAssetDocumentEditorCodecTrackedValue::ConstructionCount(), 1);
	TestEqual(TEXT("failure destroys the struct exactly once"),
		FAssetDocumentEditorCodecTrackedValue::DestructionCount(), 1);
	TestTrue(TEXT("both validations return no handles"),
		Success.HandleResults.IsEmpty() && Failure.HandleResults.IsEmpty());
	return true;
}
#endif
