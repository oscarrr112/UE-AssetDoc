// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Profile-declared property admission: a Profile names serialized non-edit
// properties through the optional "admit_properties" value argument. Declared
// properties are enumerated, batch-tokenized, written through the editor
// write sequence and accepted by registered mutation preparation (including
// whole-struct writes); undeclared non-edit properties stay rejected.

#include "AssetDocumentEditorMutationPreparation.h"
#include "AssetDocumentEditorPrimitiveProvider.h"

#include "AssetDocumentEditorGenericProviderTestTypes.h"
#include "AssetDocumentEditorProviderTestSupport.h"
#include "Curves/CurveFloat.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "UObject/UObjectGlobals.h"
#include "Provider/ClassReference/AssetDocumentEditorAssetPayload.h"
#include "Provider/Property/AssetDocumentEditorPropertyAdmission.h"
#include "Provider/Property/AssetDocumentEditorDeclaredPropertyAccess.h"
#include "Provider/Property/AssetDocumentEditorPropertyEdit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorDeclaredPropertyAdmissionTests
{
FAssetDocumentRuntimeValueDto MakeString(const FString& Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeInteger(int64 Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Integer;
	Result.IntegerValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeObject(TArray<FString> Keys, TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Values);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeNames(TArray<FString> Names)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Array;
	for (const FString& Name : Names)
	{
		Result.Elements.Add(MakeString(Name));
	}
	return Result;
}

FAssetDocumentRuntimeValueDto MakeNested(int64 Count, const FString& Label)
{
	return MakeObject({TEXT("Count"), TEXT("Label")}, {MakeInteger(Count), MakeString(Label)});
}

FAssetDocumentRuntimeValueDto MakeRows(TArray<TPair<FString, FAssetDocumentRuntimeValueDto>> Rows)
{
	FAssetDocumentRuntimeValueDto Array;
	Array.Kind = EAssetDocumentRuntimeValueKind::Array;
	for (TPair<FString, FAssetDocumentRuntimeValueDto>& Row : Rows)
	{
		Array.Elements.Add(MakeObject({TEXT("name"), TEXT("value")}, {MakeString(Row.Key), MoveTemp(Row.Value)}));
	}
	return Array;
}

const FAssetDocumentPrimitiveDescriptor* FindDescriptor(const IAssetDocumentPrimitiveProvider& Provider, FStringView Id)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Provider.DescribePrimitives())
	{
		if (Descriptor.PrimitiveId == Id)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

bool HasCode(const FAssetDocumentPrimitiveOutcome& Outcome, FStringView Code)
{
	return Outcome.Diagnostics.ContainsByPredicate(
		[Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}

const FAssetDocumentRuntimeValueDto* FindRow(const FAssetDocumentPrimitiveOutcome& Outcome, FStringView Name)
{
	const int32 PropertiesIndex = Outcome.Value.ObjectKeys.IndexOfByKey(TEXT("properties"));
	if (!Outcome.Value.Elements.IsValidIndex(PropertiesIndex))
	{
		return nullptr;
	}
	for (const FAssetDocumentRuntimeValueDto& Row : Outcome.Value.Elements[PropertiesIndex].Elements)
	{
		const int32 NameIndex = Row.ObjectKeys.IndexOfByKey(TEXT("name"));
		if (Row.Elements.IsValidIndex(NameIndex) && Row.Elements[NameIndex].StringValue == Name)
		{
			return &Row;
		}
	}
	return nullptr;
}

bool IsWritableRow(const FAssetDocumentRuntimeValueDto* Row)
{
	if (Row == nullptr)
	{
		return false;
	}
	const int32 Index = Row->ObjectKeys.IndexOfByKey(TEXT("writable"));
	return Row->Elements.IsValidIndex(Index) && Row->Elements[Index].BooleanValue;
}

// Restores the original flags of one fixture property on scope exit.
class FScopedPropertyFlags
{
public:
	FScopedPropertyFlags(FProperty& InProperty, EPropertyFlags Added)
		: Property(InProperty), Original(InProperty.GetPropertyFlags())
	{
		Property.SetPropertyFlags(Added);
	}
	~FScopedPropertyFlags()
	{
		Property.ClearPropertyFlags(CPF_AllFlags);
		Property.SetPropertyFlags(Original);
	}

private:
	FProperty& Property;
	EPropertyFlags Original;
};

struct FAsset
{
	TStrongObjectPtr<UPackage> Package;
	TStrongObjectPtr<UAssetDocumentEditorCodecTestObject> Object;

	FAsset()
		: Package(CreatePackage(*FString::Printf(TEXT("/Game/AssetDocumentTests/DeclaredAdmission_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits))))
		, Object(NewObject<UAssetDocumentEditorCodecTestObject>(Package.Get(), TEXT("Target"), RF_Transactional))
	{
		Package->SetDirtyFlag(false);
	}
	~FAsset()
	{
		Package->SetDirtyFlag(false);
	}
};

FAssetDocumentPrimitiveOutcome Enumerate(IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentEditorProviderTestContext& Context, FAssetDocumentEditorProviderTestHandleResolver& Handles,
	const FAssetDocumentRuntimeValueDto* Declared)
{
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindDescriptor(Provider, TEXT("editor.property.enumerate_admitted.v1"));
	check(Descriptor != nullptr);
	FAssetDocumentPrimitiveInvocation Invocation = MakeAssetDocumentEditorProviderTestInvocation(*Descriptor, Context);
	Invocation.ValueArguments = MakeObject({TEXT("exact_asset_class_path")},
		{MakeString(UAssetDocumentEditorCodecTestObject::StaticClass()->GetPathName())});
	if (Declared != nullptr)
	{
		Invocation.ValueArguments.ObjectKeys.Add(TEXT("admit_properties"));
		Invocation.ValueArguments.Elements.Add(*Declared);
	}
	return Provider.Invoke(Invocation, Handles, nullptr);
}

FAssetDocumentPrimitiveOutcome EnumerateBatch(IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentEditorProviderTestContext& Context, FAssetDocumentEditorProviderTestHandleResolver& Handles,
	const FAssetDocumentOpaqueHandle& Asset, const FAssetDocumentRuntimeValueDto* Declared)
{
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindDescriptor(Provider, TEXT("editor.property.enumerate_admitted_batch.v1"));
	check(Descriptor != nullptr);
	FAssetDocumentPrimitiveInvocation Invocation = MakeAssetDocumentEditorProviderTestInvocation(*Descriptor, Context);
	if (Declared != nullptr)
	{
		Invocation.ValueArguments = MakeObject({TEXT("admit_properties")}, {*Declared});
	}
	Invocation.HandleArguments = {{TEXT("asset"), Asset}};
	return Provider.Invoke(Invocation, Handles, nullptr);
}

FAssetDocumentPrimitiveOutcome WriteBatch(IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentEditorProviderTestContext& Context, FAssetDocumentEditorProviderTestHandleResolver& Handles,
	const FAssetDocumentOpaqueHandle& Asset, const FAssetDocumentOpaqueHandle& Batch,
	FAssetDocumentRuntimeValueDto Arguments, IAssetDocumentAtomicTransaction& Transaction)
{
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindDescriptor(Provider, TEXT("editor.property.write_admitted_batch.v1"));
	check(Descriptor != nullptr);
	FAssetDocumentPrimitiveInvocation Invocation = MakeAssetDocumentEditorProviderTestInvocation(*Descriptor, Context);
	Invocation.ValueArguments = MoveTemp(Arguments);
	Invocation.HandleArguments = {{TEXT("asset"), Asset}, {TEXT("property_batch"), Batch}};
	return Provider.Invoke(Invocation, Handles, &Transaction);
}

TOptional<FAssetDocumentOpaqueHandle> BatchHandle(const FAssetDocumentPrimitiveOutcome& Outcome)
{
	const FAssetDocumentPrimitiveHandleResult* Group = Outcome.HandleResults.FindByPredicate(
		[](const FAssetDocumentPrimitiveHandleResult& Result) { return Result.Name == TEXT("property_batch"); });
	return Group != nullptr && Group->Handles.Num() == 1
		? TOptional<FAssetDocumentOpaqueHandle>(Group->Handles[0])
		: TOptional<FAssetDocumentOpaqueHandle>();
}

FAssetDocumentMutationIntent MakeIntent(FAssetDocumentRuntimeValueDto Rows, const FAssetDocumentRuntimeValueDto* Declared)
{
	FAssetDocumentMutationIntent Intent;
	Intent.StepIndex = 0;
	Intent.Requirement = {TEXT("editor.property.write_admitted_batch.v1"), 1, TEXT("route.generic.v1")};
	Intent.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Intent.InputSchemaId = TEXT("editor.property.write_admitted_batch.v1.input.v1");
	Intent.Intent = MakeObject({TEXT("properties")}, {MoveTemp(Rows)});
	if (Declared != nullptr)
	{
		Intent.Intent.ObjectKeys.Insert(TEXT("admit_properties"), 0);
		Intent.Intent.Elements.Insert(*Declared, 0);
	}
	return Intent;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDeclaredPropertyEnumerationTest,
	"UEAssetDoc.GenericProvider.Property.DeclaredAdmission.Enumerate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentDeclaredPropertyEnumerationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorDeclaredPropertyAdmissionTests;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider = CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);

	const FAssetDocumentPrimitiveOutcome Plain = Enumerate(*Provider, Context, Handles, nullptr);
	TestEqual(TEXT("enumeration without declaration succeeds"), Plain.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestNull(TEXT("undeclared non-edit property stays unadmitted"), FindRow(Plain, TEXT("IntegerValue")));

	const FAssetDocumentRuntimeValueDto Declared = MakeNames({TEXT("IntegerValue"), TEXT("NestedValue")});
	const FAssetDocumentPrimitiveOutcome Admitted = Enumerate(*Provider, Context, Handles, &Declared);
	TestEqual(TEXT("declared enumeration succeeds"), Admitted.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestTrue(TEXT("declared scalar is admitted writable"), IsWritableRow(FindRow(Admitted, TEXT("IntegerValue"))));
	TestTrue(TEXT("declared struct is admitted writable"), IsWritableRow(FindRow(Admitted, TEXT("NestedValue"))));
	TestNull(TEXT("other undeclared property stays unadmitted"), FindRow(Admitted, TEXT("BooleanValue")));
	TestEqual(TEXT("one handle per admitted row"), Admitted.HandleResults.Num() == 1 ? Admitted.HandleResults[0].Handles.Num() : -1, 2);

	const FAssetDocumentRuntimeValueDto Unknown = MakeNames({TEXT("NoSuchProperty")});
	const FAssetDocumentPrimitiveOutcome Missing = Enumerate(*Provider, Context, Handles, &Unknown);
	TestEqual(TEXT("unknown declared name fails"), Missing.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("unknown declared name has a stable code"), HasCode(Missing, TEXT("PropertyAdmission.DeclaredPropertyUnknown")));

	UClass* Class = UAssetDocumentEditorCodecTestObject::StaticClass();
	{
		FScopedPropertyFlags Transient(*FindFProperty<FProperty>(Class, TEXT("StringValue")), CPF_Transient);
		const FAssetDocumentRuntimeValueDto Names = MakeNames({TEXT("StringValue")});
		const FAssetDocumentPrimitiveOutcome Rejected = Enumerate(*Provider, Context, Handles, &Names);
		TestTrue(TEXT("transient declared property is rejected"), HasCode(Rejected, TEXT("PropertyAdmission.DeclaredPropertyRejected")));
	}
	{
		FScopedPropertyFlags EditConst(*FindFProperty<FProperty>(Class, TEXT("NameValue")), CPF_Edit | CPF_EditConst);
		const FAssetDocumentRuntimeValueDto Names = MakeNames({TEXT("NameValue")});
		const FAssetDocumentPrimitiveOutcome Rejected = Enumerate(*Provider, Context, Handles, &Names);
		TestTrue(TEXT("EditConst declared property is rejected"), HasCode(Rejected, TEXT("PropertyAdmission.DeclaredPropertyRejected")));
	}
	FAssetDocumentRuntimeValueDto Duplicate = MakeNames({TEXT("IntegerValue"), TEXT("IntegerValue")});
	TestTrue(TEXT("duplicate declared names are a value-shape failure"),
		HasCode(Enumerate(*Provider, Context, Handles, &Duplicate), TEXT("PrimitiveInvocation.InvalidValueShape")));
	const FAssetDocumentRuntimeValueDto NotArray = MakeString(TEXT("IntegerValue"));
	TestTrue(TEXT("non-array declaration is a value-shape failure"),
		HasCode(Enumerate(*Provider, Context, Handles, &NotArray), TEXT("PrimitiveInvocation.InvalidValueShape")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDeclaredPropertyBatchWriteTest,
	"UEAssetDoc.GenericProvider.Property.DeclaredAdmission.BatchWrite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentDeclaredPropertyBatchWriteTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorDeclaredPropertyAdmissionTests;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider = CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FAsset Asset;
	const FAssetDocumentIssuedHandleResult AssetHandle = Handles.Issue(EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable, MakeAssetDocumentEditorAssetPayload(*Asset.Object.Get()));
	const FAssetDocumentIssuedHandleResult ReadHandle = Handles.Issue(EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly, MakeAssetDocumentEditorAssetPayload(*Asset.Object.Get()));
	if (!TestTrue(TEXT("mutable asset handle"), AssetHandle.Handle.IsSet())
		|| !TestTrue(TEXT("read asset handle"), ReadHandle.Handle.IsSet()))
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto Declared = MakeNames({TEXT("IntegerValue"), TEXT("NestedValue")});
	const FAssetDocumentPrimitiveOutcome Token =
		EnumerateBatch(*Provider, Context, Handles, ReadHandle.Handle.GetValue(), &Declared);
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Token.Diagnostics)
	{
		AddInfo(FString::Printf(TEXT("batch token: [%s] %s"), *Diagnostic.Code, *Diagnostic.Message));
	}
	const TOptional<FAssetDocumentOpaqueHandle> Batch = BatchHandle(Token);
	if (!TestEqual(TEXT("declared batch token succeeds"), Token.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		|| !TestTrue(TEXT("declared batch token handle"), Batch.IsSet()))
	{
		return false;
	}

	int32 Notifications = 0;
	const FDelegateHandle Observer = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda(
		[&Notifications, Target = Asset.Object.Get()](UObject* Object, FPropertyChangedEvent&)
		{
			Notifications += Object == Target ? 1 : 0;
		});
	ON_SCOPE_EXIT { FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(Observer); };

	auto Arguments = [&Declared](int64 Count, const FString& Label, bool bDeclare)
	{
		FAssetDocumentRuntimeValueDto Rows = MakeRows({
			{TEXT("IntegerValue"), MakeInteger(Count)},
			{TEXT("NestedValue"), MakeNested(Count, Label)}});
		FAssetDocumentRuntimeValueDto Result = MakeObject({TEXT("properties")}, {MoveTemp(Rows)});
		if (bDeclare)
		{
			Result.ObjectKeys.Add(TEXT("admit_properties"));
			Result.Elements.Add(Declared);
		}
		return Result;
	};
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	const FAssetDocumentPrimitiveOutcome Written = WriteBatch(*Provider, Context, Handles,
		AssetHandle.Handle.GetValue(), Batch.GetValue(), Arguments(7, TEXT("seven"), true), Transaction);
	TestEqual(TEXT("declared batch write succeeds"), Written.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestEqual(TEXT("declared scalar is written"), Asset.Object->IntegerValue, int64(7));
	TestEqual(TEXT("declared struct is written whole"), Asset.Object->NestedValue.Count, 7);
	TestEqual(TEXT("declared struct label is written"), Asset.Object->NestedValue.Label, FString(TEXT("seven")));
	TestTrue(TEXT("editor write dirties the package (Modify via PreEditChange)"), Asset.Package->IsDirty());
	TestEqual(TEXT("PostEditChange notifies once per written property"), Notifications, 2);
	TestEqual(TEXT("two changed records"), Written.ChangedSet.Num(), 2);

	Asset.Package->SetDirtyFlag(false);
	Notifications = 0;
	FAssetDocumentEditorProviderTestTransaction Identical(Context);
	const FAssetDocumentPrimitiveOutcome Same = WriteBatch(*Provider, Context, Handles,
		AssetHandle.Handle.GetValue(), Batch.GetValue(), Arguments(7, TEXT("seven"), true), Identical);
	TestEqual(TEXT("identical declared write succeeds"), Same.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestFalse(TEXT("identical declared write leaves the package clean"), Asset.Package->IsDirty());
	TestEqual(TEXT("identical declared write emits no edit notification"), Notifications, 0);

	FAssetDocumentEditorProviderTestTransaction Undeclared(Context);
	const FAssetDocumentPrimitiveOutcome Missing = WriteBatch(*Provider, Context, Handles,
		AssetHandle.Handle.GetValue(), Batch.GetValue(), Arguments(8, TEXT("eight"), false), Undeclared);
	TestTrue(TEXT("declared row without the write declaration is rejected"),
		HasCode(Missing, TEXT("PropertyBatchWrite.UndeclaredProperty")));
	TestEqual(TEXT("rejected write leaves the value"), Asset.Object->IntegerValue, int64(7));

	const FAssetDocumentPrimitiveOutcome PlainToken =
		EnumerateBatch(*Provider, Context, Handles, ReadHandle.Handle.GetValue(), nullptr);
	const TOptional<FAssetDocumentOpaqueHandle> PlainBatch = BatchHandle(PlainToken);
	if (TestTrue(TEXT("plain batch token"), PlainBatch.IsSet()))
	{
		FAssetDocumentEditorProviderTestTransaction PlainTransaction(Context);
		const FAssetDocumentPrimitiveOutcome Unknown = WriteBatch(*Provider, Context, Handles,
			AssetHandle.Handle.GetValue(), PlainBatch.GetValue(), Arguments(9, TEXT("nine"), false), PlainTransaction);
		TestTrue(TEXT("undeclared non-edit property stays rejected"),
			HasCode(Unknown, TEXT("PropertyBatchWrite.UnknownProperty")));
		TestEqual(TEXT("undeclared write leaves the value"), Asset.Object->IntegerValue, int64(7));
	}

	// Read and scratch preflight use the same declared token semantics.
	TArray<FAssetDocumentEditorPropertyBatchTokenFieldView> Fields;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	const TArray<FString> Names = {TEXT("NestedValue")};
	if (TestTrue(TEXT("declared field projection"), TryGetAssetDocumentEditorAdmittedPropertyFields(
			*Asset.Object->GetClass(), Names, Fields, Diagnostics)))
	{
		const FAssetDocumentEditorPropertyBatchTokenFieldView* Field = Fields.FindByPredicate(
			[](const FAssetDocumentEditorPropertyBatchTokenFieldView& View) { return View.Property->GetName() == TEXT("NestedValue"); });
		if (TestNotNull(TEXT("declared struct field projected"), Field))
		{
			FAssetDocumentEditorPropertyTokenView TokenView;
			TokenView.DeclaringClass = Field->DeclaringClass;
			TokenView.Property = Field->Property;
			TokenView.CodecId = Field->CodecId;
			TokenView.TypeRevision = Field->TypeRevision;
			TokenView.bReadable = true;
			TokenView.bWritable = true;
			TokenView.bDeclared = true;
			const FAssetDocumentEditorPropertyEditResult Read = ReadAssetDocumentEditorAdmittedProperty(*Asset.Object.Get(), TokenView);
			TestTrue(TEXT("declared read succeeds"), Read.bSucceeded);
			TStrongObjectPtr<UAssetDocumentEditorCodecTestObject> Scratch(
				NewObject<UAssetDocumentEditorCodecTestObject>(GetTransientPackage()));
			const FAssetDocumentEditorPropertyEditResult Preflight = PreflightAssetDocumentEditorAdmittedProperty(
				*Scratch.Get(), TokenView, MakeNested(3, TEXT("scratch")));
			TestTrue(TEXT("declared scratch preflight succeeds"), Preflight.bSucceeded);
			TestEqual(TEXT("declared scratch preflight writes the scratch"), Scratch->NestedValue.Count, 3);
			TokenView.bDeclared = false;
			TestFalse(TEXT("the same non-edit field without declaration cannot be preflighted"),
				PreflightAssetDocumentEditorAdmittedProperty(*Scratch.Get(), TokenView, MakeNested(4, TEXT("x"))).bSucceeded);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDeclaredPropertyMutationPreparationTest,
	"UEAssetDoc.Editor.MutationPreparation.DeclaredStructField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentDeclaredPropertyMutationPreparationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorDeclaredPropertyAdmissionTests;
	FAsset Asset;
	FAssetDocumentRuntimeRequestDto Request;
	Request.RequestId = FGuid::NewGuid();
	Request.ExpectedRuntimeRevision = 1;
	Request.Operation = EAssetDocumentRuntimeOperation::Apply;
	Request.TargetAssetPath = Asset.Object->GetPathName();
	Request.RequestedAssetClassPath = Asset.Object->GetClass()->GetPathName();
	const FAssetDocumentRuntimeValueDto Declared = MakeNames({TEXT("NestedValue")});
	const FAssetDocumentRuntimeValueDto Rows = MakeRows({{TEXT("NestedValue"), MakeNested(5, TEXT("five"))}});

	FAssetDocumentPreparedMutationBatch Batch;
	const TArray<FAssetDocumentMutationIntent> DeclaredIntents = {MakeIntent(Rows, &Declared)};
	const FAssetDocumentContractValidationResult Prepared =
		PrepareAssetDocumentEditorMutationBatch(Request, DeclaredIntents, Batch);
	TestTrue(TEXT("declared whole-struct write prepares"), Prepared.bValid);
	if (Batch.Contributions.Num() == 1)
	{
		TestEqual(TEXT("declared struct owns its surface"), Batch.Contributions[0].OwnedSurfaceIds.Num(), 1);
		TestFalse(TEXT("declared struct enlists a footprint"), Batch.Contributions[0].RawFootprint.IsEmpty());
	}

	const TArray<FAssetDocumentMutationIntent> UndeclaredIntents = {MakeIntent(Rows, nullptr)};
	const FAssetDocumentContractValidationResult Undeclared =
		PrepareAssetDocumentEditorMutationBatch(Request, UndeclaredIntents, Batch);
	TestFalse(TEXT("undeclared non-edit struct is rejected"), Undeclared.bValid);
	TestTrue(TEXT("undeclared rejection is PropertyNotAdmitted"), Undeclared.Diagnostics.ContainsByPredicate(
		[](const FAssetDocumentRuntimeDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("MutationPreparation.PropertyNotAdmitted"); }));

	const FAssetDocumentRuntimeValueDto Unknown = MakeNames({TEXT("NoSuchProperty")});
	const TArray<FAssetDocumentMutationIntent> UnknownIntents = {MakeIntent(Rows, &Unknown)};
	const FAssetDocumentContractValidationResult UnknownResult =
		PrepareAssetDocumentEditorMutationBatch(Request, UnknownIntents, Batch);
	TestTrue(TEXT("unknown declaration fails with the admission code"), !UnknownResult.bValid
		&& UnknownResult.Diagnostics.ContainsByPredicate([](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
			{ return Diagnostic.Code == TEXT("PropertyAdmission.DeclaredPropertyUnknown"); }));
	TestFalse(TEXT("preparation never writes"), Asset.Package->IsDirty());
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDeclaredPropertyTransientStructMemberTest,
	"UEAssetDoc.GenericProvider.Property.DeclaredAdmission.TransientStructMembers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// A struct with a transient member (FRichCurve::KeyHandlesToIndices) reads back
// without that member and the same value preflights: the codec shape matches
// the JSON converter, which never exports transient or deprecated members.
bool FAssetDocumentDeclaredPropertyTransientStructMemberTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TStrongObjectPtr<UCurveFloat> Curve(NewObject<UCurveFloat>(GetTransientPackage()));
	Curve->FloatCurve.AddKey(0.f, 1.f);
	Curve->FloatCurve.AddKey(2.f, 0.5f);
	TArray<FAssetDocumentEditorPropertyBatchTokenFieldView> Fields;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	const TArray<FString> Names = {TEXT("FloatCurve")};
	if (!TestTrue(TEXT("declared FRichCurve is admitted"), TryGetAssetDocumentEditorAdmittedPropertyFields(
			*UCurveFloat::StaticClass(), Names, Fields, Diagnostics)))
	{
		return false;
	}
	const FAssetDocumentEditorPropertyBatchTokenFieldView* Field = Fields.FindByPredicate(
		[](const FAssetDocumentEditorPropertyBatchTokenFieldView& View) { return View.Property->GetName() == TEXT("FloatCurve"); });
	if (!TestNotNull(TEXT("FloatCurve field projected"), Field))
	{
		return false;
	}
	FAssetDocumentEditorPropertyTokenView Token;
	Token.DeclaringClass = Field->DeclaringClass;
	Token.Property = Field->Property;
	Token.CodecId = Field->CodecId;
	Token.TypeRevision = Field->TypeRevision;
	Token.bReadable = true;
	Token.bWritable = true;
	Token.bDeclared = true;
	const FAssetDocumentEditorPropertyEditResult Read = ReadAssetDocumentEditorAdmittedProperty(*Curve.Get(), Token);
	if (!TestTrue(TEXT("FRichCurve reads back"), Read.bSucceeded))
	{
		return false;
	}
	// Reads publish {"value": <typed value>}.
	const int32 ValueIndex = Read.Value.ObjectKeys.IndexOfByKey(TEXT("value"));
	if (!TestTrue(TEXT("read publishes one value"), Read.Value.Elements.IsValidIndex(ValueIndex)))
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto* Value = &Read.Value.Elements[ValueIndex];
	TestFalse(TEXT("transient KeyHandlesToIndices is not part of the value"), Value->ObjectKeys.Contains(TEXT("KeyHandlesToIndices")));
	TestTrue(TEXT("serialized Keys are part of the value"), Value->ObjectKeys.Contains(TEXT("Keys")));
	TStrongObjectPtr<UCurveFloat> Scratch(NewObject<UCurveFloat>(GetTransientPackage()));
	const FAssetDocumentEditorPropertyEditResult Preflight =
		PreflightAssetDocumentEditorAdmittedProperty(*Scratch.Get(), Token, *Value);
	TestTrue(TEXT("the read value preflights"), Preflight.bSucceeded);
	TestEqual(TEXT("preflight writes both keys"), Scratch->FloatCurve.GetNumKeys(), 2);

	// The editor write sequence accepts asset instances (which PropertyAccessUtil
	// treats as "templates" for default-only edits) and rejects class defaults.
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/AssetDocumentDeclaredCurve_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UCurveFloat* Asset = NewObject<UCurveFloat>(Package, TEXT("DeclaredCurve"), RF_Public | RF_Standalone | RF_Transactional);
	ON_SCOPE_EXIT
	{
		Asset->ClearFlags(RF_Public | RF_Standalone);
		Asset->MarkAsGarbage();
		Package->SetDirtyFlag(false);
		Package->MarkAsGarbage();
	};
	const void* Source = Field->Property->ContainerPtrToValuePtr<void>(Scratch.Get());
	TestTrue(TEXT("declared write reaches an asset instance"), SetAssetDocumentEditorDeclaredPropertyValue(
		*Asset, *Field->Property, Source, EPropertyAccessChangeNotifyMode::Default));
	TestEqual(TEXT("asset receives both keys"), Asset->FloatCurve.GetNumKeys(), 2);
	TestTrue(TEXT("the editor write dirties the asset package"), Package->IsDirty());
	TestFalse(TEXT("declared write never reaches a class default object"), SetAssetDocumentEditorDeclaredPropertyValue(
		*GetMutableDefault<UCurveFloat>(), *Field->Property, Source, EPropertyAccessChangeNotifyMode::Never));
	return true;
}
#endif
