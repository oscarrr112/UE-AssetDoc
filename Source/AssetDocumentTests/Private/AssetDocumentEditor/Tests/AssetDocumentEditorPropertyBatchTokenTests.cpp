// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentEditorPrimitiveProvider.h"

#include "AssetDocumentEditorGenericProviderTestTypes.h"
#include "AssetDocumentEditorProviderTestSupport.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/Property/AssetDocumentEditorPropertyBatchTokenPayload.h"
#include "Provider/Property/AssetDocumentEditorPropertyTokenPayload.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/GarbageCollection.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorPropertyBatchTokenTests
{
const FAssetDocumentPrimitiveDescriptor* FindDescriptor(
	const IAssetDocumentPrimitiveProvider& Provider,
	FStringView PrimitiveId)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor :
		Provider.DescribePrimitives())
	{
		if (Descriptor.PrimitiveId == PrimitiveId)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

const FAssetDocumentRuntimeValueDto* FindField(
	const FAssetDocumentRuntimeValueDto& Object,
	FStringView Name)
{
	const int32 Index = Object.ObjectKeys.IndexOfByPredicate(
		[Name](const FString& Key) { return Key == Name; });
	return Object.Elements.IsValidIndex(Index) ? &Object.Elements[Index] : nullptr;
}

const FAssetDocumentPrimitiveHandleResult* FindHandleGroup(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	FStringView Name)
{
	return Outcome.HandleResults.FindByPredicate(
		[Name](const FAssetDocumentPrimitiveHandleResult& Result)
		{
			return Result.Name == Name;
		});
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

FAssetDocumentRuntimeValueDto MakeExactClassArguments(FString ClassPath)
{
	FAssetDocumentRuntimeValueDto Arguments;
	Arguments.Kind = EAssetDocumentRuntimeValueKind::Object;
	Arguments.ObjectKeys = {TEXT("exact_asset_class_path")};
	Arguments.Elements.SetNum(1);
	Arguments.Elements[0].Kind = EAssetDocumentRuntimeValueKind::String;
	Arguments.Elements[0].StringValue = MoveTemp(ClassPath);
	return Arguments;
}

class FPropertyBatchTokenTestAssetPayload final
	: public UE::AssetDocument::EditorPrimitive::INativeIdentityPayload
{
public:
	explicit FPropertyBatchTokenTestAssetPayload(UObject& InObject)
		: Object(&InObject)
		, CanonicalPath(InObject.GetPathName())
		, TypeRevision(static_cast<uint64>(InObject.GetUniqueID()))
	{
	}

	virtual FName GetPayloadTypeId() const override
	{
		return UE::AssetDocument::EditorPrimitive::AssetPayloadTypeId;
	}

	virtual bool IsAlive() const override
	{
		return Object.IsValid();
	}

	virtual bool TryGetObjectIdentity(
		UE::AssetDocument::EditorPrimitive::FObjectIdentityView& OutView)
		const override
	{
		UObject* Resolved = Object.Get();
		if (Resolved == nullptr)
		{
			return false;
		}
		OutView.Object = Resolved;
		OutView.CanonicalObjectPath = CanonicalPath;
		OutView.TypeRevision = TypeRevision;
		return true;
	}

private:
	TWeakObjectPtr<UObject> Object;
	FString CanonicalPath;
	uint64 TypeRevision = 0;
};

class FScopedEditableFixtureProperties final
{
public:
	FScopedEditableFixtureProperties()
	{
		for (TFieldIterator<FProperty> It(
			UAssetDocumentEditorCodecTestObject::StaticClass()); It; ++It)
		{
			FProperty& Property = **It;
			if (!Property.HasAnyPropertyFlags(CPF_Edit))
			{
				Changed.Add(&Property);
				Property.SetPropertyFlags(CPF_Edit);
			}
		}
	}

	~FScopedEditableFixtureProperties()
	{
		for (FProperty* Property : Changed)
		{
			Property->ClearPropertyFlags(CPF_Edit);
		}
	}

private:
	TArray<FProperty*> Changed;
};

TOptional<FAssetDocumentOpaqueHandle> IssueAsset(
	IAssetDocumentHandleResolver& Handles,
	UObject& Object,
	EAssetDocumentHandleKind Kind = EAssetDocumentHandleKind::Asset,
	EAssetDocumentHandleAccess Access = EAssetDocumentHandleAccess::ReadOnly)
{
	FAssetDocumentIssuedHandleResult Issued = Handles.Issue(
		Kind,
		Access,
		MakeUnique<FPropertyBatchTokenTestAssetPayload>(Object));
	return MoveTemp(Issued.Handle);
}

class FPropertyBatchTokenIssueFailureResolver final
	: public IAssetDocumentHandleResolver
{
public:
	FPropertyBatchTokenIssueFailureResolver(
		FAssetDocumentEditorProviderTestContext Context,
		int32 InSuccessfulIssuesBeforeFailure)
		: Inner(MoveTemp(Context))
		, SuccessfulIssuesBeforeFailure(InSuccessfulIssuesBeforeFailure)
	{
	}

	TOptional<FAssetDocumentOpaqueHandle> IssueInitialAsset(UObject& Object)
	{
		return IssueAsset(Inner, Object);
	}

	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
	{
		if (SuccessfulIssues >= SuccessfulIssuesBeforeFailure)
		{
			FAssetDocumentIssuedHandleResult Result;
			FAssetDocumentRuntimeDiagnostic& Diagnostic =
				Result.Diagnostics.AddDefaulted_GetRef();
			Diagnostic.Path = TEXT("/Handle");
			Diagnostic.Code = TEXT("TestHandle.InjectedIssueFailure");
			Diagnostic.Message = TEXT("The bounded test resolver rejected handle issuance.");
			return Result;
		}
		++SuccessfulIssues;
		return Inner.Issue(Kind, Access, MoveTemp(Payload));
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleValidationContext& ValidationContext) override
	{
		return Inner.AcquireVerifiedLease(Handle, ValidationContext);
	}

	virtual FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation& Event) override
	{
		return Inner.Invalidate(Event);
	}

private:
	FAssetDocumentEditorProviderTestHandleResolver Inner;
	int32 SuccessfulIssuesBeforeFailure = 0;
	int32 SuccessfulIssues = 0;
};

FAssetDocumentPrimitiveInvocation MakeInvocation(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentOpaqueHandle Asset)
{
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(Descriptor, Context);
	Invocation.HandleArguments = {{TEXT("asset"), MoveTemp(Asset)}};
	return Invocation;
}

void TestNoPublication(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const TCHAR* Label)
{
	Test.TestEqual(Label, Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestTrue(FString::Printf(TEXT("%s has no rows"), Label),
		Outcome.Value.ObjectKeys.IsEmpty() && Outcome.Value.Elements.IsEmpty());
	Test.TestTrue(FString::Printf(TEXT("%s has no handle groups"), Label),
		Outcome.HandleResults.IsEmpty());
}

struct FExpectedPropertyRow
{
	const TCHAR* Name;
	const TCHAR* CodecId;
	bool bReadable;
	bool bWritable;
};

TConstArrayView<FExpectedPropertyRow> GetExpectedExistingV1Rows()
{
	static const FExpectedPropertyRow ExpectedRows[] = {
		{TEXT("ArrayValue"), TEXT("array.v1"), true, true},
		{TEXT("BooleanValue"), TEXT("bool.v1"), true, true},
		{TEXT("EnumValue"), TEXT("enum.v1"), true, true},
		{TEXT("FixedArrayValue"), TEXT("fixed_array.v1"), true, true},
		{TEXT("HardObjectValue"), TEXT("object_path.v1"), true, true},
		{TEXT("IntegerMapValue"), TEXT("string_key_map.v1"), true, true},
		{TEXT("IntegerValue"), TEXT("integer.v1"), true, true},
		{TEXT("InterfaceValue"), TEXT("object_path.v1"), true, true},
		{TEXT("LazyObjectValue"), TEXT("object_path.v1"), true, true},
		{TEXT("NameValue"), TEXT("name.v1"), true, true},
		{TEXT("NestedValue"), TEXT("struct.v1"), true, true},
		{TEXT("NumberValue"), TEXT("number.v1"), true, true},
		{TEXT("SetValue"), TEXT("set.v1"), true, true},
		{TEXT("SoftClassValue"), TEXT("soft_class_path.v1"), true, true},
		{TEXT("SoftObjectValue"), TEXT("soft_object_path.v1"), true, true},
		{TEXT("StringMapValue"), TEXT("string_key_map.v1"), true, true},
		{TEXT("StringValue"), TEXT("string.v1"), true, true},
		{TEXT("TrackedValue"), TEXT("struct.v1"), true, true},
		{TEXT("WeakObjectValue"), TEXT("object_path.v1"), true, true},
	};
	return ExpectedRows;
}

bool TryReadPropertyRow(
	const FAssetDocumentRuntimeValueDto& Row,
	const FAssetDocumentRuntimeValueDto*& OutName,
	const FAssetDocumentRuntimeValueDto*& OutDeclaringClass,
	const FAssetDocumentRuntimeValueDto*& OutCodecId,
	const FAssetDocumentRuntimeValueDto*& OutReadable,
	const FAssetDocumentRuntimeValueDto*& OutWritable)
{
	// "default" (the class default value) is present when it could be encoded.
	static const TArray<FString> ExactObjectKeys = {
		TEXT("codec_id"),
		TEXT("declaring_class"),
		TEXT("name"),
		TEXT("readable"),
		TEXT("writable"),
	};
	static const TArray<FString> ExactObjectKeysWithDefault = {
		TEXT("codec_id"),
		TEXT("declaring_class"),
		TEXT("default"),
		TEXT("name"),
		TEXT("readable"),
		TEXT("writable"),
	};
	if (Row.Kind != EAssetDocumentRuntimeValueKind::Object
		|| (Row.ObjectKeys != ExactObjectKeys && Row.ObjectKeys != ExactObjectKeysWithDefault)
		|| Row.Elements.Num() != Row.ObjectKeys.Num())
	{
		return false;
	}
	OutName = FindField(Row, TEXT("name"));
	OutDeclaringClass = FindField(Row, TEXT("declaring_class"));
	OutCodecId = FindField(Row, TEXT("codec_id"));
	OutReadable = FindField(Row, TEXT("readable"));
	OutWritable = FindField(Row, TEXT("writable"));
	return OutName != nullptr
		&& OutName->Kind == EAssetDocumentRuntimeValueKind::String
		&& OutDeclaringClass != nullptr
		&& OutDeclaringClass->Kind == EAssetDocumentRuntimeValueKind::String
		&& OutCodecId != nullptr
		&& OutCodecId->Kind == EAssetDocumentRuntimeValueKind::String
		&& OutReadable != nullptr
		&& OutReadable->Kind == EAssetDocumentRuntimeValueKind::Boolean
		&& OutWritable != nullptr
		&& OutWritable->Kind == EAssetDocumentRuntimeValueKind::Boolean;
}

bool CanAcquireLiveBatchView(
	FAutomationTestBase& Test,
	IAssetDocumentHandleResolver& Handles,
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentHandleValidationContext& Context)
{
	FAssetDocumentVerifiedLeaseResult Lease =
		Handles.AcquireVerifiedLease(Handle, Context);
	if (!Lease.Lease.IsValid())
	{
		return false;
	}
	IAssetDocumentNativeHandlePayload& Payload =
		Lease.Lease->GetPayloadChecked();
	if (!Test.TestEqual(
		TEXT("reacquired composite payload type remains exact"),
		Payload.GetPayloadTypeId(),
		FName(TEXT("assetdocument.editor.property_batch_token.v1"))))
	{
		return false;
	}
	FAssetDocumentEditorPropertyBatchTokenView View;
	return static_cast<FAssetDocumentEditorPropertyBatchTokenPayload&>(Payload)
		.TryGetView(View);
}

void TestExactDescriptor(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	Test.TestTrue(TEXT("batch descriptor validates"),
		ValidateAssetDocumentPrimitiveDescriptor(Descriptor).bValid);
	Test.TestEqual(TEXT("batch descriptor version"), Descriptor.Version, 1u);
	Test.TestEqual(TEXT("batch descriptor provider"), Descriptor.ProviderId, FString(TEXT("editor")));
	Test.TestEqual(TEXT("batch descriptor owner"), Descriptor.Owner,
		EAssetDocumentPrimitiveOwner::Editor);
	Test.TestEqual(TEXT("batch descriptor thread"), Descriptor.ExecutionThread,
		EAssetDocumentExecutionThread::GameThread);
	Test.TestEqual(TEXT("batch descriptor access"), Descriptor.Access,
		EAssetDocumentPrimitiveAccess::ReadOnly);
	Test.TestFalse(TEXT("batch descriptor needs no transaction"),
		Descriptor.bRequiresActiveTransaction);
	Test.TestEqual(TEXT("batch descriptor allowed phases"), Descriptor.AllowedPhases,
		TArray<EAssetDocumentRuntimeRequestPhase>({
			EAssetDocumentRuntimeRequestPhase::Validate,
			EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply,
			EAssetDocumentRuntimeRequestPhase::Repair,
			EAssetDocumentRuntimeRequestPhase::Verify}));
	Test.TestEqual(TEXT("batch descriptor has one required asset"),
		Descriptor.HandleArgumentSchemas.Num(), 1);
	Test.TestEqual(TEXT("batch descriptor has two results"),
		Descriptor.HandleResultSchemas.Num(), 2);
	if (Descriptor.HandleArgumentSchemas.Num() == 1)
	{
		const FAssetDocumentPrimitiveHandleArgumentSchema& Asset =
			Descriptor.HandleArgumentSchemas[0];
		Test.TestEqual(TEXT("batch argument name"), Asset.Name, FString(TEXT("asset")));
		Test.TestEqual(TEXT("batch argument kind"), Asset.Kind, EAssetDocumentHandleKind::Asset);
		Test.TestEqual(TEXT("batch argument access"), Asset.RequiredAccess,
			EAssetDocumentHandleAccess::ReadOnly);
		Test.TestTrue(TEXT("batch argument required"), Asset.bRequired);
	}
	if (Descriptor.HandleResultSchemas.Num() == 2)
	{
		const FAssetDocumentPrimitiveHandleResultSchema& Properties =
			Descriptor.HandleResultSchemas[0];
		const FAssetDocumentPrimitiveHandleResultSchema& Batch =
			Descriptor.HandleResultSchemas[1];
		Test.TestEqual(TEXT("scalar result name"), Properties.Name, FString(TEXT("properties")));
		Test.TestEqual(TEXT("scalar result kind"), Properties.Kind,
			EAssetDocumentHandleKind::Property);
		Test.TestEqual(TEXT("scalar result access"), Properties.GrantedAccess,
			EAssetDocumentHandleAccess::ReadOnly);
		Test.TestEqual(TEXT("scalar result cardinality"), Properties.Cardinality,
			EAssetDocumentHandleResultCardinality::ZeroOrMore);
		Test.TestEqual(TEXT("batch result name"), Batch.Name, FString(TEXT("property_batch")));
		Test.TestEqual(TEXT("batch result kind"), Batch.Kind, EAssetDocumentHandleKind::Property);
		Test.TestEqual(TEXT("batch result access"), Batch.GrantedAccess,
			EAssetDocumentHandleAccess::ReadOnly);
		Test.TestEqual(TEXT("batch result cardinality"), Batch.Cardinality,
			EAssetDocumentHandleResultCardinality::ExactlyOne);
	}
	Test.TestEqual(TEXT("batch descriptor exact consumers"), Descriptor.RequiredConsumerIds,
		TArray<FString>({TEXT("route.generic.v1"), TEXT("animation_profile")}));
	Test.TestEqual(TEXT("batch authority module"), Descriptor.Admission.PublicModule,
		FString(TEXT("CoreUObject")));
	Test.TestEqual(TEXT("batch authority headers"), Descriptor.Admission.PublicHeader,
		FString(TEXT("UObject/UnrealType.h;UObject/PropertyAccessUtil.h")));
	Test.TestEqual(TEXT("batch authority API"), Descriptor.Admission.ExportedAuthoritativeApi,
		FString(TEXT("TFieldIterator<FProperty>;PropertyAccessUtil::CanGetPropertyValue;PropertyAccessUtil::CanSetPropertyValue")));
	Test.TestEqual(TEXT("batch primary operation"), Descriptor.Admission.PrimaryOperationId,
		FString(TEXT("coreuobject.property.enumerate-admitted-batch.v1")));
	Test.TestEqual(TEXT("batch focused verification"), Descriptor.Admission.VerificationTestIds,
		TArray<FString>({TEXT("UEAssetDoc.GenericProvider.Property.BatchToken")}));
}

void TestExistingV1Descriptor(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	Test.TestTrue(TEXT("v1 descriptor validates"),
		ValidateAssetDocumentPrimitiveDescriptor(Descriptor).bValid);
	Test.TestEqual(TEXT("v1 id remains exact"), Descriptor.PrimitiveId,
		FString(TEXT("editor.property.enumerate_admitted.v1")));
	Test.TestEqual(TEXT("v1 version remains exact"), Descriptor.Version, 1u);
	Test.TestEqual(TEXT("v1 provider remains exact"), Descriptor.ProviderId, FString(TEXT("editor")));
	Test.TestEqual(TEXT("v1 owner remains editor"), Descriptor.Owner,
		EAssetDocumentPrimitiveOwner::Editor);
	Test.TestEqual(TEXT("v1 thread remains game thread"),
		Descriptor.ExecutionThread, EAssetDocumentExecutionThread::GameThread);
	Test.TestEqual(TEXT("v1 access remains read-only"), Descriptor.Access,
		EAssetDocumentPrimitiveAccess::ReadOnly);
	Test.TestFalse(TEXT("v1 remains transaction-free"), Descriptor.bRequiresActiveTransaction);
	Test.TestEqual(TEXT("v1 allowed phases remain exact"), Descriptor.AllowedPhases,
		TArray<EAssetDocumentRuntimeRequestPhase>({
			EAssetDocumentRuntimeRequestPhase::Validate,
			EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply,
			EAssetDocumentRuntimeRequestPhase::Repair,
			EAssetDocumentRuntimeRequestPhase::Verify}));
	Test.TestEqual(TEXT("v1 allowed handle kinds remain exact"),
		Descriptor.AllowedHandleKinds,
		TArray<EAssetDocumentHandleKind>({EAssetDocumentHandleKind::Property}));
	Test.TestEqual(TEXT("v1 remains argument-free"),
		Descriptor.HandleArgumentSchemas.Num(), 0);
	Test.TestEqual(TEXT("v1 remains one-result descriptor"), Descriptor.HandleResultSchemas.Num(), 1);
	if (Descriptor.HandleResultSchemas.Num() == 1)
	{
		const FAssetDocumentPrimitiveHandleResultSchema& Properties =
			Descriptor.HandleResultSchemas[0];
		Test.TestEqual(TEXT("v1 scalar result name"), Properties.Name, FString(TEXT("properties")));
		Test.TestEqual(TEXT("v1 scalar result kind"), Properties.Kind,
			EAssetDocumentHandleKind::Property);
		Test.TestEqual(TEXT("v1 scalar result access"), Properties.GrantedAccess,
			EAssetDocumentHandleAccess::ReadOnly);
		Test.TestEqual(TEXT("v1 scalar result cardinality"), Properties.Cardinality,
			EAssetDocumentHandleResultCardinality::ZeroOrMore);
	}
	Test.TestEqual(TEXT("v1 input schema remains exact"), Descriptor.InputSchemaId,
		FString(TEXT("editor.property.enumerate_admitted.v1.input.v1")));
	Test.TestEqual(TEXT("v1 output schema remains exact"), Descriptor.OutputSchemaId,
		FString(TEXT("editor.property.enumerate_admitted.v1.output.v1")));
	Test.TestTrue(TEXT("v1 mutation footprint remains empty"),
		Descriptor.MutationFootprintSchemaId.IsEmpty());
	Test.TestEqual(TEXT("v1 stable diagnostics remain exact"),
		Descriptor.StableDiagnosticCodes,
		TArray<FString>({
			TEXT("AD.RUNTIME.PRIMITIVE_FAILED"),
			TEXT("AD.RUNTIME.PRIMITIVE_UNAVAILABLE")}));
	// The Montage profile admits class fields with it when creating a Montage.
	Test.TestEqual(TEXT("v1 consumer remains exact"), Descriptor.RequiredConsumerIds,
		TArray<FString>({TEXT("route.generic.v1"), TEXT("animation_profile")}));
	Test.TestEqual(TEXT("v1 authority module remains exact"),
		Descriptor.Admission.PublicModule, FString(TEXT("CoreUObject")));
	Test.TestEqual(TEXT("v1 authority headers remain exact"),
		Descriptor.Admission.PublicHeader,
		FString(TEXT("UObject/UnrealType.h;UObject/PropertyAccessUtil.h")));
	Test.TestEqual(TEXT("v1 authority API remains exact"),
		Descriptor.Admission.ExportedAuthoritativeApi,
		FString(TEXT("TFieldIterator<FProperty>;PropertyAccessUtil::CanGetPropertyValue;PropertyAccessUtil::CanSetPropertyValue")));
	Test.TestEqual(TEXT("v1 primary operation remains exact"),
		Descriptor.Admission.PrimaryOperationId,
		FString(TEXT("coreuobject.property.enumerate-admitted.v1")));
	Test.TestEqual(TEXT("v1 focused verification remains exact"),
		Descriptor.Admission.VerificationTestIds,
		TArray<FString>({
			TEXT("UEAssetDoc.GenericProvider.Property.DeterministicEnumeration")}));
}
} // namespace AssetDocumentEditorPropertyBatchTokenTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderPropertyBatchTokenTest,
	"UEAssetDoc.GenericProvider.Property.BatchToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderPropertyBatchTokenTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorPropertyBatchTokenTests;

	FScopedEditableFixtureProperties EditableFixture;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(
		*Provider, TEXT("editor.property.enumerate_admitted_batch.v1"));
	if (!TestNotNull(TEXT("batch token descriptor exists"), Descriptor))
	{
		return false;
	}
	TestExactDescriptor(*this, *Descriptor);

	const FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	TStrongObjectPtr<UAssetDocumentEditorCodecTestObject> Asset(
		NewObject<UAssetDocumentEditorCodecTestObject>(GetTransientPackage()));
	const TOptional<FAssetDocumentOpaqueHandle> AssetHandle =
		IssueAsset(Handles, *Asset.Get());
	if (!TestTrue(TEXT("test read-only asset handle issues"), AssetHandle.IsSet()))
	{
		return false;
	}
	const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
		MakeInvocation(*Descriptor, Context, AssetHandle.GetValue()), Handles, nullptr);

	// RED anchor: every later assertion below is the frozen success contract.
	if (!TestEqual(TEXT("real asset returns successful immutable batch snapshot"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		AddInfo(TEXT("expected RED diagnostic: PropertyBatchToken.NotImplemented"));
		TestTrue(TEXT("RED scaffold reports the required missing behavior"),
			HasDiagnostic(Outcome, TEXT("PropertyBatchToken.NotImplemented")));
		TestNoPublication(*this, Outcome, TEXT("NotImplemented batch scaffold"));
		return false;
	}

	const FAssetDocumentRuntimeValueDto* Rows = FindField(Outcome.Value, TEXT("properties"));
	const FAssetDocumentPrimitiveHandleResult* ScalarHandles =
		FindHandleGroup(Outcome, TEXT("properties"));
	const FAssetDocumentPrimitiveHandleResult* BatchHandles =
		FindHandleGroup(Outcome, TEXT("property_batch"));
	if (!TestNotNull(TEXT("batch success returns property rows"), Rows)
		|| !TestNotNull(TEXT("batch success returns scalar property handles"), ScalarHandles)
		|| !TestNotNull(TEXT("batch success returns composite handle group"), BatchHandles))
	{
		return false;
	}
	if (!TestEqual(TEXT("batch rows are an array"),
		Rows->Kind, EAssetDocumentRuntimeValueKind::Array)
		|| !TestTrue(TEXT("batch returns at least one admitted row"),
			!Rows->Elements.IsEmpty())
		|| !TestEqual(TEXT("batch scalar handles exactly match rows"),
			ScalarHandles->Handles.Num(), Rows->Elements.Num())
		|| !TestEqual(TEXT("batch returns exactly one composite token"),
			BatchHandles->Handles.Num(), 1))
	{
		return false;
	}

	FAssetDocumentHandleValidationContext BatchContext;
	BatchContext.RequestId = Context.RequestId;
	BatchContext.TransactionId = Context.TransactionId;
	BatchContext.RuntimeRevision = Context.RuntimeRevision;
	BatchContext.ProviderId = TEXT("editor");
	BatchContext.Phase = EAssetDocumentRuntimeRequestPhase::Validate;
	BatchContext.RequiredKind = EAssetDocumentHandleKind::Property;
	BatchContext.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
	FAssetDocumentVerifiedLeaseResult BatchLease = Handles.AcquireVerifiedLease(
		BatchHandles->Handles[0], BatchContext);
	if (!TestTrue(TEXT("batch composite leases through the real resolver"),
		BatchLease.Lease.IsValid()))
	{
		return false;
	}
	IAssetDocumentNativeHandlePayload& Payload = BatchLease.Lease->GetPayloadChecked();
	if (!TestEqual(TEXT("batch payload type is exact"), Payload.GetPayloadTypeId(),
		FName(TEXT("assetdocument.editor.property_batch_token.v1"))))
	{
		return false;
	}
	FAssetDocumentEditorPropertyBatchTokenView View;
	if (!TestTrue(TEXT("batch payload exposes its immutable view"),
		static_cast<FAssetDocumentEditorPropertyBatchTokenPayload&>(Payload).TryGetView(View)))
	{
		return false;
	}
	TestEqual(TEXT("batch target identity is exact"), View.Target, static_cast<UObject*>(Asset.Get()));
	TestEqual(TEXT("batch target class is exact"), View.TargetClass, Asset->GetClass());
	TestEqual(TEXT("batch target revision is exact"), View.TargetTypeRevision,
		static_cast<uint64>(Asset->GetUniqueID()));
	if (!TestEqual(TEXT("batch immutable fields exactly match rows"),
		View.Fields.Num(), Rows->Elements.Num()))
	{
		return false;
	}
	for (int32 Index = 0; Index < View.Fields.Num(); ++Index)
	{
		const FAssetDocumentEditorPropertyBatchTokenFieldView& Field = View.Fields[Index];
		const FAssetDocumentRuntimeValueDto* Name = nullptr;
		const FAssetDocumentRuntimeValueDto* DeclaringClass = nullptr;
		const FAssetDocumentRuntimeValueDto* CodecId = nullptr;
		const FAssetDocumentRuntimeValueDto* Readable = nullptr;
		const FAssetDocumentRuntimeValueDto* Writable = nullptr;
		if (!TestTrue(TEXT("batch row has exact observable shape"),
			TryReadPropertyRow(
				Rows->Elements[Index],
				Name,
				DeclaringClass,
				CodecId,
				Readable,
				Writable))
			|| !TestNotNull(TEXT("batch field view has a property"), Field.Property)
			|| !TestNotNull(TEXT("batch field declaring class exists"), Field.DeclaringClass))
		{
			return false;
		}
		TestEqual(TEXT("batch field declaring class is exact"), Field.DeclaringClass, Asset->GetClass());
		TestEqual(TEXT("batch field order matches row name"),
			Name->StringValue, Field.Property->GetAuthoredName());
		TestEqual(TEXT("batch field class path matches row"),
			DeclaringClass->StringValue, Field.DeclaringClass->GetPathName());
		TestEqual(TEXT("batch field codec matches row"),
			CodecId->StringValue, FString(Field.CodecId));
		TestEqual(TEXT("batch field readable bit matches row"),
			Readable->BooleanValue, Field.bReadable);
		TestEqual(TEXT("batch field writable bit matches row"),
			Writable->BooleanValue, Field.bWritable);
		TestEqual(TEXT("batch field revision matches declaring class"),
			Field.TypeRevision,
			GetAssetDocumentEditorPropertyTypeRevision(
				*Field.DeclaringClass));
		if (!TestEqual(TEXT("parallel scalar kind"),
			ScalarHandles->Handles[Index].Kind,
			EAssetDocumentHandleKind::Property)
			|| !TestEqual(TEXT("parallel scalar access"),
				ScalarHandles->Handles[Index].Access,
				EAssetDocumentHandleAccess::ReadOnly))
		{
			return false;
		}
		FAssetDocumentVerifiedLeaseResult ScalarLease = Handles.AcquireVerifiedLease(
			ScalarHandles->Handles[Index], BatchContext);
		if (!TestTrue(TEXT("parallel scalar leases through the real resolver"),
			ScalarLease.Lease.IsValid()))
		{
			return false;
		}
		IAssetDocumentNativeHandlePayload& ScalarPayload =
			ScalarLease.Lease->GetPayloadChecked();
		if (!TestEqual(TEXT("parallel scalar payload type is exact"),
			ScalarPayload.GetPayloadTypeId(),
			UE::AssetDocument::EditorPrimitive::PropertyTokenPayloadTypeId))
		{
			return false;
		}
		FAssetDocumentEditorPropertyTokenView ScalarView;
		if (!TestTrue(TEXT("parallel scalar payload exposes its native view"),
			static_cast<FAssetDocumentEditorPropertyTokenPayload&>(ScalarPayload)
				.TryGetView(ScalarView)))
		{
			return false;
		}
		if (!TestNotNull(TEXT("parallel scalar declaring class exists"),
			ScalarView.DeclaringClass)
			|| !TestNotNull(TEXT("parallel scalar property exists"),
				ScalarView.Property))
		{
			return false;
		}
		TestEqual(TEXT("parallel scalar class matches composite field"),
			ScalarView.DeclaringClass, Field.DeclaringClass);
		TestEqual(TEXT("parallel scalar property matches composite field"),
			ScalarView.Property, Field.Property);
		TestEqual(TEXT("parallel scalar revision matches composite field"),
			ScalarView.TypeRevision, Field.TypeRevision);
		TestEqual(TEXT("parallel scalar codec matches composite field"),
			FString(ScalarView.CodecId), FString(Field.CodecId));
		TestEqual(TEXT("parallel scalar readable matches composite field"),
			ScalarView.bReadable, Field.bReadable);
		TestEqual(TEXT("parallel scalar writable matches composite field"),
			ScalarView.bWritable, Field.bWritable);
		TestEqual(TEXT("parallel scalar class path matches row"),
			ScalarView.DeclaringClass->GetPathName(), DeclaringClass->StringValue);
		TestEqual(TEXT("parallel scalar codec matches row"),
			FString(ScalarView.CodecId), CodecId->StringValue);
		TestEqual(TEXT("parallel scalar readable matches row"),
			ScalarView.bReadable, Readable->BooleanValue);
		TestEqual(TEXT("parallel scalar writable matches row"),
			ScalarView.bWritable, Writable->BooleanValue);
	}

	const FAssetDocumentPrimitiveDescriptor* ReadDescriptor = FindDescriptor(
		*Provider, TEXT("editor.property.read_admitted.v1"));
	if (!TestNotNull(TEXT("existing scalar reader remains registered"), ReadDescriptor))
	{
		return false;
	}
	FAssetDocumentPrimitiveInvocation Read =
		MakeAssetDocumentEditorProviderTestInvocation(*ReadDescriptor, Context);
	Read.HandleArguments = {
		{TEXT("asset"), AssetHandle.GetValue()},
		{TEXT("property"), BatchHandles->Handles[0]},
	};
	const FAssetDocumentPrimitiveOutcome SubtypeFailure = Provider->Invoke(Read, Handles, nullptr);
	TestNoPublication(*this, SubtypeFailure, TEXT("composite token rejected by scalar reader"));
	TestTrue(TEXT("scalar reader rejects composite payload type"),
		HasDiagnostic(SubtypeFailure, TEXT("PropertyToken.InvalidPayload")));
	FAssetDocumentOpaqueHandle StaleScalar = ScalarHandles->Handles[0];
	++StaleScalar.Generation;
	Read.HandleArguments[1].Handle = StaleScalar;
	TestNoPublication(*this, Provider->Invoke(Read, Handles, nullptr),
		TEXT("stale field scalar token fails closed"));

	FAssetDocumentPrimitiveInvocation MissingAsset =
		MakeAssetDocumentEditorProviderTestInvocation(*Descriptor, Context);
	TestNoPublication(*this, Provider->Invoke(MissingAsset, Handles, nullptr),
		TEXT("missing asset fails before publication"));
	FAssetDocumentPrimitiveInvocation ExtraValue =
		MakeInvocation(*Descriptor, Context, AssetHandle.GetValue());
	ExtraValue.ValueArguments.ObjectKeys = {TEXT("extra")};
	ExtraValue.ValueArguments.Elements.SetNum(1);
	ExtraValue.ValueArguments.Elements[0].Kind = EAssetDocumentRuntimeValueKind::String;
	ExtraValue.ValueArguments.Elements[0].StringValue = TEXT("forbidden");
	TestNoPublication(*this, Provider->Invoke(ExtraValue, Handles, nullptr),
		TEXT("extra value key fails before publication"));
	const TOptional<FAssetDocumentOpaqueHandle> WrongKind = IssueAsset(
		Handles, *Asset.Get(), EAssetDocumentHandleKind::Class);
	const TOptional<FAssetDocumentOpaqueHandle> WrongAccess = IssueAsset(
		Handles, *Asset.Get(), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable);
	if (TestTrue(TEXT("wrong-shape asset handles issue"),
		WrongKind.IsSet() && WrongAccess.IsSet()))
	{
		TestNoPublication(*this, Provider->Invoke(
			MakeInvocation(*Descriptor, Context, WrongKind.GetValue()), Handles, nullptr),
			TEXT("wrong-kind asset fails before publication"));
		TestNoPublication(*this, Provider->Invoke(
			MakeInvocation(*Descriptor, Context, WrongAccess.GetValue()), Handles, nullptr),
			TEXT("wrong-access asset fails before publication"));
	}

	FAssetDocumentOpaqueHandle CrossRequest = AssetHandle.GetValue();
	CrossRequest.RequestId = FGuid::NewGuid();
	TestNoPublication(*this, Provider->Invoke(
		MakeInvocation(*Descriptor, Context, MoveTemp(CrossRequest)), Handles, nullptr),
		TEXT("cross-request asset fails before publication"));
	FAssetDocumentOpaqueHandle CrossTransaction = AssetHandle.GetValue();
	CrossTransaction.TransactionId = FGuid::NewGuid();
	TestNoPublication(*this, Provider->Invoke(
		MakeInvocation(*Descriptor, Context, MoveTemp(CrossTransaction)), Handles, nullptr),
		TEXT("cross-transaction asset fails before publication"));
	FAssetDocumentOpaqueHandle CrossRevision = AssetHandle.GetValue();
	++CrossRevision.RuntimeRevision;
	TestNoPublication(*this, Provider->Invoke(
		MakeInvocation(*Descriptor, Context, MoveTemp(CrossRevision)), Handles, nullptr),
		TEXT("cross-revision asset fails before publication"));
	FAssetDocumentOpaqueHandle CrossProvider = AssetHandle.GetValue();
	CrossProvider.ProviderId = TEXT("different-provider");
	TestNoPublication(*this, Provider->Invoke(
		MakeInvocation(*Descriptor, Context, MoveTemp(CrossProvider)), Handles, nullptr),
		TEXT("cross-provider asset fails before publication"));
	FAssetDocumentOpaqueHandle StaleGeneration = AssetHandle.GetValue();
	++StaleGeneration.Generation;
	TestNoPublication(*this, Provider->Invoke(
		MakeInvocation(*Descriptor, Context, MoveTemp(StaleGeneration)), Handles, nullptr),
		TEXT("stale-generation asset fails before publication"));

	FAssetDocumentOpaqueHandle StaleComposite = BatchHandles->Handles[0];
	++StaleComposite.Generation;
	Read.HandleArguments[1].Handle = StaleComposite;
	TestNoPublication(*this, Provider->Invoke(Read, Handles, nullptr),
		TEXT("stale composite field token fails closed"));

	FPropertyBatchTokenIssueFailureResolver ScalarIssueFailure(Context, 0);
	const TOptional<FAssetDocumentOpaqueHandle> ScalarIssueAsset =
		ScalarIssueFailure.IssueInitialAsset(*Asset.Get());
	if (TestTrue(TEXT("scalar issue-failure asset issues"), ScalarIssueAsset.IsSet()))
	{
		TestNoPublication(*this, Provider->Invoke(
			MakeInvocation(*Descriptor, Context, ScalarIssueAsset.GetValue()),
			ScalarIssueFailure, nullptr),
			TEXT("first scalar issue failure publishes nothing"));
	}
	FPropertyBatchTokenIssueFailureResolver CompositeIssueFailure(
		Context, Rows->Elements.Num());
	const TOptional<FAssetDocumentOpaqueHandle> CompositeIssueAsset =
		CompositeIssueFailure.IssueInitialAsset(*Asset.Get());
	if (TestTrue(TEXT("composite issue-failure asset issues"), CompositeIssueAsset.IsSet()))
	{
		TestNoPublication(*this, Provider->Invoke(
			MakeInvocation(*Descriptor, Context, CompositeIssueAsset.GetValue()),
			CompositeIssueFailure, nullptr),
			TEXT("final composite issue failure publishes nothing"));
	}

	const FAssetDocumentPrimitiveDescriptor* ExistingDescriptor = FindDescriptor(
		*Provider, TEXT("editor.property.enumerate_admitted.v1"));
	if (!TestNotNull(TEXT("existing v1 descriptor remains registered"), ExistingDescriptor))
	{
		return false;
	}
	TestExistingV1Descriptor(*this, *ExistingDescriptor);
	FAssetDocumentEditorProviderTestHandleResolver ExistingHandles(Context);
	FAssetDocumentPrimitiveInvocation ExistingFirst =
		MakeAssetDocumentEditorProviderTestInvocation(*ExistingDescriptor, Context);
	ExistingFirst.ValueArguments = MakeExactClassArguments(Asset->GetClass()->GetPathName());
	const FAssetDocumentPrimitiveOutcome ExistingOne = Provider->Invoke(
		ExistingFirst, ExistingHandles, nullptr);
	if (!TestEqual(TEXT("existing v1 remains successful"), ExistingOne.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto* ExistingRows =
		FindField(ExistingOne.Value, TEXT("properties"));
	const FAssetDocumentPrimitiveHandleResult* ExistingHandleGroup =
		FindHandleGroup(ExistingOne, TEXT("properties"));
	if (!TestNotNull(TEXT("existing v1 rows remain present"), ExistingRows)
		|| !TestNotNull(TEXT("existing v1 scalar group remains present"), ExistingHandleGroup))
	{
		return false;
	}
	const TConstArrayView<FExpectedPropertyRow> ExpectedRows =
		GetExpectedExistingV1Rows();
	if (!TestEqual(TEXT("existing v1 rows remain an array"),
		ExistingRows->Kind, EAssetDocumentRuntimeValueKind::Array)
		|| !TestEqual(TEXT("existing v1 row count matches independent golden"),
			ExistingRows->Elements.Num(), ExpectedRows.Num())
		|| !TestEqual(TEXT("existing v1 scalar handles match independent golden"),
			ExistingHandleGroup->Handles.Num(), ExpectedRows.Num()))
	{
		return false;
	}
	FAssetDocumentHandleValidationContext ExistingContext = BatchContext;
	ExistingContext.Phase = EAssetDocumentRuntimeRequestPhase::Validate;
	UClass* ExpectedClass =
		UAssetDocumentEditorCodecTestObject::StaticClass();
	const FString ExpectedClassPath = ExpectedClass->GetPathName();
	const uint64 ExpectedTypeRevision =
		static_cast<uint64>(ExpectedClass->GetUniqueID());
	for (int32 Index = 0; Index < ExpectedRows.Num(); ++Index)
	{
		const FExpectedPropertyRow& Expected = ExpectedRows[Index];
		const FAssetDocumentRuntimeValueDto* Name = nullptr;
		const FAssetDocumentRuntimeValueDto* DeclaringClass = nullptr;
		const FAssetDocumentRuntimeValueDto* CodecId = nullptr;
		const FAssetDocumentRuntimeValueDto* Readable = nullptr;
		const FAssetDocumentRuntimeValueDto* Writable = nullptr;
		if (!TestTrue(TEXT("existing v1 row shape matches independent golden"),
			TryReadPropertyRow(
				ExistingRows->Elements[Index],
				Name,
				DeclaringClass,
				CodecId,
				Readable,
				Writable)))
		{
			return false;
		}
		TestEqual(TEXT("existing v1 row name and order match independent golden"),
			Name->StringValue, FString(Expected.Name));
		TestEqual(TEXT("existing v1 row class matches independent golden"),
			DeclaringClass->StringValue, ExpectedClassPath);
		TestEqual(TEXT("existing v1 row codec matches independent golden"),
			CodecId->StringValue, FString(Expected.CodecId));
		TestEqual(TEXT("existing v1 row readable bit matches independent golden"),
			Readable->BooleanValue, Expected.bReadable);
		TestEqual(TEXT("existing v1 row writable bit matches independent golden"),
			Writable->BooleanValue, Expected.bWritable);
		if (!TestEqual(TEXT("existing v1 scalar kind remains property"),
			ExistingHandleGroup->Handles[Index].Kind,
			EAssetDocumentHandleKind::Property)
			|| !TestEqual(TEXT("existing v1 scalar access remains read-only"),
				ExistingHandleGroup->Handles[Index].Access,
				EAssetDocumentHandleAccess::ReadOnly))
		{
			return false;
		}
		FAssetDocumentVerifiedLeaseResult ExistingLease =
			ExistingHandles.AcquireVerifiedLease(ExistingHandleGroup->Handles[Index], ExistingContext);
		if (!TestTrue(TEXT("existing v1 scalar lease remains valid"),
			ExistingLease.Lease.IsValid()))
		{
			return false;
		}
		FAssetDocumentEditorPropertyTokenView ExistingView;
		IAssetDocumentNativeHandlePayload& ExistingPayload =
			ExistingLease.Lease->GetPayloadChecked();
		if (!TestEqual(TEXT("existing v1 scalar payload type remains exact"),
			ExistingPayload.GetPayloadTypeId(),
			UE::AssetDocument::EditorPrimitive::PropertyTokenPayloadTypeId))
		{
			return false;
		}
		if (!TestTrue(TEXT("existing v1 scalar view remains valid"),
			static_cast<FAssetDocumentEditorPropertyTokenPayload&>(ExistingPayload)
				.TryGetView(ExistingView)))
		{
			return false;
		}
		if (!TestNotNull(TEXT("existing v1 scalar class exists"),
			ExistingView.DeclaringClass)
			|| !TestNotNull(TEXT("existing v1 scalar property exists"),
				ExistingView.Property))
		{
			return false;
		}
		TestEqual(TEXT("existing v1 scalar class matches independent golden"),
			ExistingView.DeclaringClass, ExpectedClass);
		TestEqual(TEXT("existing v1 scalar name and order match independent golden"),
			ExistingView.Property->GetAuthoredName(), FString(Expected.Name));
		TestEqual(TEXT("existing v1 scalar codec matches independent golden"),
			FString(ExistingView.CodecId), FString(Expected.CodecId));
		TestEqual(TEXT("existing v1 scalar readable matches independent golden"),
			ExistingView.bReadable, Expected.bReadable);
		TestEqual(TEXT("existing v1 scalar writable matches independent golden"),
			ExistingView.bWritable, Expected.bWritable);
		TestEqual(TEXT("existing v1 scalar revision matches independent golden"),
			ExistingView.TypeRevision, ExpectedTypeRevision);
	}

	const FName BlueprintName = MakeUniqueObjectName(
		GetTransientPackage(),
		UBlueprint::StaticClass(),
		TEXT("AssetDocumentPropertyBatchTokenFixture"));
	TStrongObjectPtr<UBlueprint> Blueprint(
		FKismetEditorUtilities::CreateBlueprint(
			UObject::StaticClass(),
			GetTransientPackage(),
			BlueprintName,
			BPTYPE_Normal));
	if (!TestNotNull(TEXT("transient Blueprint fixture is created"),
		Blueprint.Get()))
	{
		return false;
	}
	const FName RemovedFieldName(TEXT("BatchValue"));
	FEdGraphPinType BooleanPinType;
	BooleanPinType.PinCategory = FName(TEXT("bool"));
	if (!TestTrue(TEXT("transient Blueprint adds admitted member"),
		FBlueprintEditorUtils::AddMemberVariable(
			Blueprint.Get(),
			RemovedFieldName,
			BooleanPinType)))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
	if (!TestNotNull(TEXT("compiled transient Blueprint has generated class"),
		Blueprint->GeneratedClass.Get())
		|| !TestNotNull(TEXT("compiled transient Blueprint has admitted member"),
			FindFProperty<FProperty>(
				Blueprint->GeneratedClass.Get(),
				RemovedFieldName)))
	{
		return false;
	}
	TStrongObjectPtr<UObject> BlueprintTarget(
		NewObject<UObject>(
			GetTransientPackage(),
			Blueprint->GeneratedClass.Get()));
	if (!TestNotNull(TEXT("transient Blueprint target is created"),
		BlueprintTarget.Get()))
	{
		return false;
	}

	FAssetDocumentEditorProviderTestHandleResolver BlueprintHandles(Context);
	const TOptional<FAssetDocumentOpaqueHandle> BlueprintAssetHandle =
		IssueAsset(BlueprintHandles, *BlueprintTarget.Get());
	if (!TestTrue(TEXT("transient Blueprint asset handle issues"),
		BlueprintAssetHandle.IsSet()))
	{
		return false;
	}
	const FAssetDocumentPrimitiveOutcome BlueprintOutcome = Provider->Invoke(
		MakeInvocation(
			*Descriptor,
			Context,
			BlueprintAssetHandle.GetValue()),
		BlueprintHandles,
		nullptr);
	if (!TestEqual(TEXT("real provider issues transient Blueprint batch"),
		BlueprintOutcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return false;
	}
	const FAssetDocumentPrimitiveHandleResult* BlueprintBatchHandles =
		FindHandleGroup(BlueprintOutcome, TEXT("property_batch"));
	if (!TestNotNull(TEXT("transient Blueprint batch group exists"),
		BlueprintBatchHandles)
		|| !TestEqual(TEXT("transient Blueprint returns one composite token"),
			BlueprintBatchHandles->Handles.Num(), 1))
	{
		return false;
	}
	const FAssetDocumentOpaqueHandle BlueprintBatch =
		BlueprintBatchHandles->Handles[0];
	if (!TestTrue(TEXT("transient Blueprint composite starts live"),
		CanAcquireLiveBatchView(
			*this,
			BlueprintHandles,
			BlueprintBatch,
			BatchContext)))
	{
		return false;
	}

	UClass* IssuedBlueprintClass = BlueprintTarget->GetClass();
	const uint64 IssuedBlueprintRevision =
		GetAssetDocumentEditorPropertyTypeRevision(*IssuedBlueprintClass);
	FBlueprintEditorUtils::RemoveMemberVariable(
		Blueprint.Get(),
		RemovedFieldName);
	FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
	if (!TestNotNull(TEXT("recompiled transient Blueprint has generated class"),
		Blueprint->GeneratedClass.Get())
		|| !TestTrue(TEXT("recompile changes the admitted field identity"),
			FindFProperty<FProperty>(
				Blueprint->GeneratedClass.Get(),
				RemovedFieldName) == nullptr)
		|| !TestTrue(TEXT("recompile changes class, revision, or field path"),
			Blueprint->GeneratedClass.Get() != IssuedBlueprintClass
			|| GetAssetDocumentEditorPropertyTypeRevision(
				*Blueprint->GeneratedClass.Get()) != IssuedBlueprintRevision
			|| FindFProperty<FProperty>(
				Blueprint->GeneratedClass.Get(),
				RemovedFieldName) == nullptr))
	{
		return false;
	}
	TestFalse(TEXT("old composite fails after public compile and field removal"),
		CanAcquireLiveBatchView(
			*this,
			BlueprintHandles,
			BlueprintBatch,
			BatchContext));

	const FAssetDocumentOpaqueHandle CollectedTargetBatch =
		BatchHandles->Handles[0];
	TWeakObjectPtr<UObject> CollectedTarget(Asset.Get());
	Asset.Reset();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	if (!TestFalse(TEXT("real batch target is collected"),
		CollectedTarget.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("old composite fails after releasing target and real GC"),
		CanAcquireLiveBatchView(
			*this,
			Handles,
			CollectedTargetBatch,
			BatchContext));
	return true;
}
#endif
