// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentEditorPrimitiveProvider.h"

#include "AssetDocumentEditorCutoverTransactionPreparation.h"
#include "AssetDocumentEditorGenericProviderTestTypes.h"
#include "AssetDocumentEditorProviderTestSupport.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/Property/AssetDocumentEditorPropertyAdmission.h"
#include "Provider/Property/AssetDocumentEditorPropertyEdit.h"
#include "Provider/Property/AssetDocumentEditorPropertyTokenPayload.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#include <type_traits>

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorPropertyBatchWriteTests
{
static_assert(
	!std::is_copy_constructible_v<
		FAssetDocumentEditorPreparedPropertyWrite>);
static_assert(
	!std::is_copy_assignable_v<
		FAssetDocumentEditorPreparedPropertyWrite>);
static_assert(
	std::is_move_constructible_v<
		FAssetDocumentEditorPreparedPropertyWrite>);
static_assert(
	std::is_move_assignable_v<
		FAssetDocumentEditorPreparedPropertyWrite>);
static_assert(
	!std::is_copy_constructible_v<
		FAssetDocumentEditorPropertyPreimage>);
static_assert(
	!std::is_copy_assignable_v<
		FAssetDocumentEditorPropertyPreimage>);
static_assert(
	std::is_move_constructible_v<
		FAssetDocumentEditorPropertyPreimage>);
static_assert(
	std::is_move_assignable_v<
		FAssetDocumentEditorPropertyPreimage>);

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

const FAssetDocumentRuntimeValueDto* FindField(
	const FAssetDocumentRuntimeValueDto& Object,
	FStringView Name)
{
	const int32 Index = Object.ObjectKeys.IndexOfByPredicate(
		[Name](const FString& Key) { return Key == Name; });
	return Object.Elements.IsValidIndex(Index)
		? &Object.Elements[Index]
		: nullptr;
}

int32 FindPropertyRowIndex(
	const FAssetDocumentRuntimeValueDto& Rows,
	FStringView AuthoredName)
{
	for (int32 Index = 0; Index < Rows.Elements.Num(); ++Index)
	{
		const FAssetDocumentRuntimeValueDto* Name =
			FindField(Rows.Elements[Index], TEXT("name"));
		if (Name != nullptr
			&& Name->Kind == EAssetDocumentRuntimeValueKind::String
			&& Name->StringValue == AuthoredName)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

bool HasDiagnostic(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	FStringView Code)
{
	return Outcome.Diagnostics.ContainsByPredicate(
		[Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code;
		});
}

bool HasEditDiagnostic(
	const FAssetDocumentEditorPropertyEditResult& Result,
	FStringView Code)
{
	return Result.Diagnostics.ContainsByPredicate(
		[Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code;
		});
}

bool HasContractDiagnostic(
	const FAssetDocumentContractValidationResult& Result,
	FStringView Code)
{
	return Result.Diagnostics.ContainsByPredicate(
		[Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code;
		});
}

bool HasPreimageCaptureDiagnostic(
	const FAssetDocumentEditorPropertyPreimageCaptureResult& Result,
	FStringView Code)
{
	return Result.Diagnostics.ContainsByPredicate(
		[Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code;
		});
}

bool HasArgument(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	FStringView Name,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access)
{
	return Descriptor.HandleArgumentSchemas.ContainsByPredicate(
		[Name, Kind, Access](
			const FAssetDocumentPrimitiveHandleArgumentSchema& Schema)
		{
			return Schema.Name == Name
				&& Schema.Kind == Kind
				&& Schema.RequiredAccess == Access
				&& Schema.bRequired;
		});
}

void TestExactBatchWriteDescriptor(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	Test.TestEqual(TEXT("batch write version is exact"), Descriptor.Version, 1u);
	Test.TestEqual(TEXT("batch write provider is exact"),
		Descriptor.ProviderId, FString(TEXT("editor")));
	Test.TestEqual(TEXT("batch write owner is Editor"),
		Descriptor.Owner, EAssetDocumentPrimitiveOwner::Editor);
	Test.TestEqual(TEXT("batch write runs on the GameThread"),
		Descriptor.ExecutionThread, EAssetDocumentExecutionThread::GameThread);
	Test.TestEqual(TEXT("batch write access is mutating"),
		Descriptor.Access, EAssetDocumentPrimitiveAccess::Mutating);
	Test.TestTrue(TEXT("batch write requires an active transaction"),
		Descriptor.bRequiresActiveTransaction);
	Test.TestEqual(TEXT("batch write phases are exact"),
		Descriptor.AllowedPhases,
		TArray<EAssetDocumentRuntimeRequestPhase>({
			EAssetDocumentRuntimeRequestPhase::Apply,
			EAssetDocumentRuntimeRequestPhase::Repair}));
	Test.TestEqual(TEXT("batch write has exactly two handle arguments"),
		Descriptor.HandleArgumentSchemas.Num(), 2);
	Test.TestTrue(TEXT("batch write requires a mutable asset"),
		HasArgument(
			Descriptor,
			TEXT("asset"),
			EAssetDocumentHandleKind::Asset,
			EAssetDocumentHandleAccess::Mutable));
	Test.TestTrue(TEXT("batch write requires a read-only composite token"),
		HasArgument(
			Descriptor,
			TEXT("property_batch"),
			EAssetDocumentHandleKind::Property,
			EAssetDocumentHandleAccess::ReadOnly));
	Test.TestTrue(TEXT("batch write returns no handles"),
		Descriptor.HandleResultSchemas.IsEmpty());
	Test.TestEqual(TEXT("batch write consumer is exact"),
		Descriptor.RequiredConsumerIds,
		TArray<FString>({TEXT("route.generic.v1"), TEXT("animation_profile")}));
	Test.TestEqual(TEXT("batch write public module is exact"),
		Descriptor.Admission.PublicModule, FString(TEXT("CoreUObject")));
	Test.TestEqual(TEXT("batch write public header is exact"),
		Descriptor.Admission.PublicHeader,
		FString(TEXT("UObject/PropertyAccessUtil.h")));
	Test.TestEqual(TEXT("batch write authority is exact"),
		Descriptor.Admission.ExportedAuthoritativeApi,
		FString(TEXT("PropertyAccessUtil::SetPropertyValue_Object")));
	Test.TestEqual(TEXT("batch write primary operation is exact"),
		Descriptor.Admission.PrimaryOperationId,
		FString(TEXT("coreuobject.property.write-admitted-batch.v1")));
	Test.TestEqual(TEXT("batch write verification is exact"),
		Descriptor.Admission.VerificationTestIds,
		TArray<FString>({
			TEXT("UEAssetDoc.GenericProvider.Property.BatchWrite")}));
}

FAssetDocumentRuntimeValueDto MakeBooleanValue(bool bValue)
{
	FAssetDocumentRuntimeValueDto Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Value.BooleanValue = bValue;
	return Value;
}

FAssetDocumentRuntimeValueDto MakeIntegerValue(int64 Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Integer;
	Result.IntegerValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeScalarWriteArguments(
	FAssetDocumentRuntimeValueDto Value)
{
	FAssetDocumentRuntimeValueDto Arguments;
	Arguments.Kind = EAssetDocumentRuntimeValueKind::Object;
	Arguments.ObjectKeys = {TEXT("value")};
	Arguments.Elements.Add(MoveTemp(Value));
	return Arguments;
}

FAssetDocumentRuntimeValueDto MakeBatchRow(
	FString Name,
	FAssetDocumentRuntimeValueDto Value)
{
	FAssetDocumentRuntimeValueDto Row;
	Row.Kind = EAssetDocumentRuntimeValueKind::Object;
	Row.ObjectKeys = {TEXT("name"), TEXT("value")};
	FAssetDocumentRuntimeValueDto NameValue;
	NameValue.Kind = EAssetDocumentRuntimeValueKind::String;
	NameValue.StringValue = MoveTemp(Name);
	Row.Elements.Add(MoveTemp(NameValue));
	Row.Elements.Add(MoveTemp(Value));
	return Row;
}

FAssetDocumentRuntimeValueDto MakeBatchWriteArguments(
	TArray<FAssetDocumentRuntimeValueDto> Rows)
{
	FAssetDocumentRuntimeValueDto RowArray;
	RowArray.Kind = EAssetDocumentRuntimeValueKind::Array;
	RowArray.Elements = MoveTemp(Rows);
	FAssetDocumentRuntimeValueDto Arguments;
	Arguments.Kind = EAssetDocumentRuntimeValueKind::Object;
	Arguments.ObjectKeys = {TEXT("properties")};
	Arguments.Elements.Add(MoveTemp(RowArray));
	return Arguments;
}

FAssetDocumentRuntimeValueDto MakeBatchWriteArguments()
{
	return MakeBatchWriteArguments({
		MakeBatchRow(TEXT("BooleanValue"), MakeBooleanValue(false)),
		MakeBatchRow(TEXT("IntegerValue"), MakeIntegerValue(41)),
	});
}

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

class FBatchWriteTestAssetPayload final
	: public UE::AssetDocument::EditorPrimitive::INativeIdentityPayload
{
public:
	explicit FBatchWriteTestAssetPayload(UObject& InObject)
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

TOptional<FAssetDocumentOpaqueHandle> IssueAsset(
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	UObject& Asset,
	EAssetDocumentHandleAccess Access)
{
	FAssetDocumentIssuedHandleResult Issued = Handles.Issue(
		EAssetDocumentHandleKind::Asset,
		Access,
		MakeUnique<FBatchWriteTestAssetPayload>(Asset));
	return MoveTemp(Issued.Handle);
}

bool TryReadPropertyTokenView(
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	const FAssetDocumentEditorProviderTestContext& Context,
	const FAssetDocumentOpaqueHandle& Handle,
	FAssetDocumentEditorPropertyTokenView& OutToken)
{
	FAssetDocumentHandleValidationContext Validation;
	Validation.RequestId = Context.RequestId;
	Validation.TransactionId = Context.TransactionId;
	Validation.RuntimeRevision = Context.RuntimeRevision;
	Validation.ProviderId = Context.ProviderId;
	Validation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Validation.RequiredKind = EAssetDocumentHandleKind::Property;
	Validation.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
	FAssetDocumentVerifiedLeaseResult Lease =
		Handles.AcquireVerifiedLease(Handle, Validation);
	if (!Lease.Lease.IsValid())
	{
		return false;
	}
	IAssetDocumentNativeHandlePayload& Payload =
		Lease.Lease->GetPayloadChecked();
	if (Payload.GetPayloadTypeId()
		!= UE::AssetDocument::EditorPrimitive::PropertyTokenPayloadTypeId)
	{
		return false;
	}
	return static_cast<FAssetDocumentEditorPropertyTokenPayload&>(Payload)
		.TryGetView(OutToken);
}

class FDuplicateRejectingTransaction final
	: public IAssetDocumentAtomicTransaction
{
public:
	explicit FDuplicateRejectingTransaction(
		const FAssetDocumentEditorProviderTestContext& Context,
		bool bInRejectEnlist = false)
		: bRejectEnlist(bInRejectEnlist)
	{
		Token.RequestId = Context.RequestId;
		Token.TransactionId = Context.TransactionId;
		Token.RuntimeRevision = Context.RuntimeRevision;
		RollbackActionEnlistmentResult.bValid = true;
		PostRollbackFinalizerEnlistmentResult.bValid = true;
	}

	virtual FGuid GetTransactionId() const override
	{
		return Token.TransactionId;
	}

	virtual EAssetDocumentAtomicState GetState() const override
	{
		return State;
	}

	virtual const FAssetDocumentMutationToken& GetMutationToken() const override
	{
		return Token;
	}

	virtual FAssetDocumentContractValidationResult EnlistFootprint(
		TConstArrayView<FAssetDocumentMutationFootprint> Footprint) override
	{
		++EnlistCallCount;
		if (bRejectEnlist)
		{
			FAssetDocumentContractValidationResult Failure;
			FAssetDocumentRuntimeDiagnostic& Diagnostic =
				Failure.Diagnostics.AddDefaulted_GetRef();
			Diagnostic.Path = TEXT("/Footprint");
			Diagnostic.Code = TEXT("Atomic.TestInjectedEnlistFailure");
			Diagnostic.Message =
				TEXT("The test transaction rejected the footprint.");
			return Failure;
		}
		for (const FAssetDocumentMutationFootprint& Item : Footprint)
		{
			if (EnlistedStableIds.Contains(Item.StableId))
			{
				FAssetDocumentContractValidationResult Failure;
				FAssetDocumentRuntimeDiagnostic& Diagnostic =
					Failure.Diagnostics.AddDefaulted_GetRef();
				Diagnostic.Path = TEXT("/Footprint");
				Diagnostic.Code = TEXT("Atomic.FootprintAlreadyEnlisted");
				Diagnostic.Message =
					TEXT("Mutation footprint was already enlisted.");
				return Failure;
			}
		}
		for (const FAssetDocumentMutationFootprint& Item : Footprint)
		{
			EnlistedStableIds.Add(Item.StableId);
			EnlistedFootprint.Add(Item);
		}
		State = EAssetDocumentAtomicState::Mutating;
		return MakeValidResult();
	}

	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override
	{
		return MakeValidResult();
	}

	virtual FAssetDocumentContractValidationResult Commit(
		const FAssetDocumentDurableCompletionReceipt&) override
	{
		return MakeValidResult();
	}

	virtual FAssetDocumentContractValidationResult Rollback(FStringView) override
	{
		return MakeValidResult();
	}

	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(
		TSharedRef<IAssetDocumentRollbackAction> Action) override
	{
		EnlistedRollbackActionComponentIds.Add(
			FString(Action->GetRollbackComponentId()));
		return RollbackActionEnlistmentResult;
	}

	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer>) override
	{
		return PostRollbackFinalizerEnlistmentResult;
	}

	int32 GetEnlistCallCount() const
	{
		return EnlistCallCount;
	}

	const TArray<FAssetDocumentMutationFootprint>& GetEnlistedFootprint() const
	{
		return EnlistedFootprint;
	}

	TArray<FString> EnlistedRollbackActionComponentIds;
	FAssetDocumentContractValidationResult RollbackActionEnlistmentResult;
	FAssetDocumentContractValidationResult PostRollbackFinalizerEnlistmentResult;

private:
	static FAssetDocumentContractValidationResult MakeValidResult()
	{
		FAssetDocumentContractValidationResult Result;
		Result.bValid = true;
		return Result;
	}

	FAssetDocumentMutationToken Token;
	EAssetDocumentAtomicState State = EAssetDocumentAtomicState::Live;
	TSet<FString> EnlistedStableIds;
	TArray<FAssetDocumentMutationFootprint> EnlistedFootprint;
	int32 EnlistCallCount = 0;
	bool bRejectEnlist = false;
};

class FScopedBatchPropertyObserver final
{
public:
	explicit FScopedBatchPropertyObserver(UObject& InObject)
		: Object(&InObject)
	{
		Handle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda(
			[this](UObject* ChangedObject, FPropertyChangedEvent& Event)
			{
				if (ChangedObject == Object)
				{
					ChangedProperties.Add(Event.GetPropertyName().ToString());
				}
			});
	}

	~FScopedBatchPropertyObserver()
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(Handle);
	}

	int32 Num() const
	{
		return ChangedProperties.Num();
	}

	TConstArrayView<FString> Since(int32 Start) const
	{
		return MakeArrayView(ChangedProperties).RightChop(Start);
	}

private:
	UObject* Object = nullptr;
	FDelegateHandle Handle;
	TArray<FString> ChangedProperties;
};

class FScopedPreimageTestAsset final
{
public:
	FScopedPreimageTestAsset()
		: Package(CreatePackage(*FString::Printf(
			TEXT("/Game/AssetDocumentTests/PropertyPreimage_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits))))
		, Asset(NewObject<UAssetDocumentEditorCodecTestObject>(
			Package.Get(), NAME_None, RF_Transactional))
	{
	}

	~FScopedPreimageTestAsset()
	{
		Package->SetDirtyFlag(false);
		Asset.Reset();
		Package.Reset();
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}

	UAssetDocumentEditorCodecTestObject& GetAsset() const
	{
		return *Asset.Get();
	}

	UPackage& GetPackage() const
	{
		return *Package.Get();
	}

private:
	TStrongObjectPtr<UPackage> Package;
	TStrongObjectPtr<UAssetDocumentEditorCodecTestObject> Asset;
};

FAssetDocumentPrimitiveOutcome InvokeBatchEnumeration(
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	FAssetDocumentOpaqueHandle Asset)
{
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(Descriptor, Context);
	Invocation.HandleArguments = {
		{TEXT("asset"), MoveTemp(Asset)},
	};
	return Provider.Invoke(Invocation, Handles, nullptr);
}

FAssetDocumentPrimitiveOutcome InvokeScalarWrite(
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	FAssetDocumentOpaqueHandle Asset,
	FAssetDocumentOpaqueHandle Property,
	FAssetDocumentRuntimeValueDto DesiredValue,
	IAssetDocumentAtomicTransaction& Transaction)
{
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(Descriptor, Context);
	Invocation.ValueArguments =
		MakeScalarWriteArguments(MoveTemp(DesiredValue));
	Invocation.HandleArguments = {
		{TEXT("asset"), MoveTemp(Asset)},
		{TEXT("property"), MoveTemp(Property)},
	};
	return Provider.Invoke(Invocation, Handles, &Transaction);
}

FAssetDocumentPrimitiveOutcome InvokeBatchWrite(
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	FAssetDocumentOpaqueHandle Asset,
	FAssetDocumentOpaqueHandle PropertyBatch,
	FAssetDocumentRuntimeValueDto Arguments,
	IAssetDocumentAtomicTransaction* Transaction)
{
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(Descriptor, Context);
	Invocation.ValueArguments = MoveTemp(Arguments);
	Invocation.HandleArguments = {
		{TEXT("asset"), MoveTemp(Asset)},
		{TEXT("property_batch"), MoveTemp(PropertyBatch)},
	};
	return Provider.Invoke(Invocation, Handles, Transaction);
}

FAssetDocumentPrimitiveOutcome InvokeBatchWrite(
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	FAssetDocumentOpaqueHandle Asset,
	FAssetDocumentOpaqueHandle PropertyBatch,
	IAssetDocumentAtomicTransaction& Transaction)
{
	return InvokeBatchWrite(
		Provider,
		Descriptor,
		Context,
		Handles,
		MoveTemp(Asset),
		MoveTemp(PropertyBatch),
		MakeBatchWriteArguments(),
		&Transaction);
}

void TestNoValueOrHandles(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const TCHAR* Label)
{
	Test.TestTrue(
		*FString::Printf(TEXT("%s publishes no value"), Label),
		Outcome.Value.Kind == EAssetDocumentRuntimeValueKind::Object
			&& Outcome.Value.ObjectKeys.IsEmpty()
			&& Outcome.Value.Elements.IsEmpty());
	Test.TestTrue(
		*FString::Printf(TEXT("%s publishes no handles"), Label),
		Outcome.HandleResults.IsEmpty());
}

void TestNoPublication(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const TCHAR* Label)
{
	TestNoValueOrHandles(Test, Outcome, Label);
	Test.TestTrue(
		*FString::Printf(TEXT("%s publishes no changed records"), Label),
		Outcome.ChangedSet.IsEmpty());
}

void TestBatchFailureNoProgress(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	FStringView DiagnosticCode,
	const FDuplicateRejectingTransaction& Transaction,
	const UAssetDocumentEditorCodecTestObject& Asset,
	bool bBooleanBefore,
	int64 IntegerBefore,
	const FScopedBatchPropertyObserver& Observer,
	int32 ObserverBefore,
	int32 ExpectedEnlistCalls,
	const TCHAR* Label)
{
	Test.TestEqual(
		*FString::Printf(TEXT("%s fails"), Label),
		Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestTrue(
		*FString::Printf(TEXT("%s reports the stable diagnostic"), Label),
		HasDiagnostic(Outcome, DiagnosticCode));
	TestNoPublication(Test, Outcome, Label);
	Test.TestEqual(
		*FString::Printf(TEXT("%s preserves the boolean"), Label),
		Asset.BooleanValue,
		bBooleanBefore);
	Test.TestEqual(
		*FString::Printf(TEXT("%s preserves the integer"), Label),
		Asset.IntegerValue,
		IntegerBefore);
	Test.TestEqual(
		*FString::Printf(TEXT("%s emits no property callback"), Label),
		Observer.Num(),
		ObserverBefore);
	Test.TestEqual(
		*FString::Printf(TEXT("%s has exact enlist calls"), Label),
		Transaction.GetEnlistCallCount(),
		ExpectedEnlistCalls);
}

bool TestBatchWriteGreenMatrix(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& BatchDescriptor,
	const FAssetDocumentPrimitiveDescriptor& ScalarDescriptor,
	const FAssetDocumentPrimitiveDescriptor& EnumerateDescriptor,
	const FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	UAssetDocumentEditorCodecTestObject& Asset,
	const FAssetDocumentOpaqueHandle& MutableAsset,
	const FAssetDocumentOpaqueHandle& Composite,
	const FAssetDocumentOpaqueHandle& BooleanScalar,
	FProperty& BooleanProperty,
	FProperty& IntegerProperty)
{
	FScopedBatchPropertyObserver Observer(Asset);
	Asset.BooleanValue = true;
	Asset.IntegerValue = 0;
	const int32 TwoRowObserverBefore = Observer.Num();
	FDuplicateRejectingTransaction TwoRowTransaction(Context);
	const FAssetDocumentPrimitiveOutcome TwoRows = InvokeBatchWrite(
		Provider,
		BatchDescriptor,
		Context,
		Handles,
		MutableAsset,
		Composite,
		TwoRowTransaction);
	Test.TestEqual(TEXT("two-row batch succeeds"), TwoRows.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	Test.TestFalse(TEXT("two-row batch writes the boolean"), Asset.BooleanValue);
	Test.TestEqual(TEXT("two-row batch writes the integer"), Asset.IntegerValue, 41ll);
	Test.TestEqual(TEXT("two-row batch enlists once"),
		TwoRowTransaction.GetEnlistCallCount(), 1);
	Test.TestEqual(TEXT("two-row batch enlists the exact footprint"),
		TwoRowTransaction.GetEnlistedFootprint().Num(), 2);
	TestNoValueOrHandles(Test, TwoRows, TEXT("two-row batch"));
	Test.TestEqual(TEXT("two-row batch publishes two changes"),
		TwoRows.ChangedSet.Num(), 2);
	if (TwoRows.ChangedSet.Num() == 2)
	{
		Test.TestEqual(TEXT("boolean change is first"),
			TwoRows.ChangedSet[0].Path, FString(TEXT("/BooleanValue")));
		Test.TestEqual(TEXT("integer change is second"),
			TwoRows.ChangedSet[1].Path, FString(TEXT("/IntegerValue")));
		Test.TestEqual(TEXT("batch authority is exact"),
			TwoRows.ChangedSet[0].AuthorityId,
			FString(TEXT("editor.property.write_admitted_batch.v1")));
		Test.TestEqual(TEXT("batch authority is stable across rows"),
			TwoRows.ChangedSet[1].AuthorityId,
			FString(TEXT("editor.property.write_admitted_batch.v1")));
	}
	Test.TestEqual(TEXT("two-row callback order follows token order"),
		TArray<FString>(Observer.Since(TwoRowObserverBefore)),
		TArray<FString>({TEXT("BooleanValue"), TEXT("IntegerValue")}));

	const bool bEmptyBooleanBefore = Asset.BooleanValue;
	const int64 EmptyIntegerBefore = Asset.IntegerValue;
	const int32 EmptyObserverBefore = Observer.Num();
	FDuplicateRejectingTransaction EmptyTransaction(Context);
	const FAssetDocumentPrimitiveOutcome Empty = InvokeBatchWrite(
		Provider,
		BatchDescriptor,
		Context,
		Handles,
		MutableAsset,
		Composite,
		MakeBatchWriteArguments({}),
		&EmptyTransaction);
	Test.TestEqual(TEXT("empty batch succeeds"), Empty.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestNoPublication(Test, Empty, TEXT("empty batch"));
	Test.TestEqual(TEXT("empty batch does not enlist"),
		EmptyTransaction.GetEnlistCallCount(), 0);
	Test.TestEqual(TEXT("empty batch preserves boolean"),
		Asset.BooleanValue, bEmptyBooleanBefore);
	Test.TestEqual(TEXT("empty batch preserves integer"),
		Asset.IntegerValue, EmptyIntegerBefore);
	Test.TestEqual(TEXT("empty batch emits no callback"),
		Observer.Num(), EmptyObserverBefore);

	Asset.BooleanValue = false;
	const int32 OneRowObserverBefore = Observer.Num();
	FDuplicateRejectingTransaction OneRowTransaction(Context);
	const FAssetDocumentPrimitiveOutcome OneRow = InvokeBatchWrite(
		Provider,
		BatchDescriptor,
		Context,
		Handles,
		MutableAsset,
		Composite,
		MakeBatchWriteArguments({
			MakeBatchRow(TEXT("BooleanValue"), MakeBooleanValue(true)),
		}),
		&OneRowTransaction);
	Test.TestEqual(TEXT("one-row batch succeeds"), OneRow.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	Test.TestTrue(TEXT("one-row batch mutates"), Asset.BooleanValue);
	Test.TestEqual(TEXT("one-row batch enlists once"),
		OneRowTransaction.GetEnlistCallCount(), 1);
	Test.TestEqual(TEXT("one-row batch publishes one change"),
		OneRow.ChangedSet.Num(), 1);
	Test.TestEqual(TEXT("one-row batch emits one callback"),
		Observer.Num(), OneRowObserverBefore + 1);

	Asset.BooleanValue = false;
	Asset.IntegerValue = 0;
	const int32 DecodeObserverBefore = Observer.Num();
	FDuplicateRejectingTransaction DecodeTransaction(Context);
	const FAssetDocumentPrimitiveOutcome DecodeFailure = InvokeBatchWrite(
		Provider,
		BatchDescriptor,
		Context,
		Handles,
		MutableAsset,
		Composite,
		MakeBatchWriteArguments({
			MakeBatchRow(TEXT("BooleanValue"), MakeBooleanValue(true)),
			MakeBatchRow(TEXT("IntegerValue"), MakeBooleanValue(true)),
		}),
		&DecodeTransaction);
	TestBatchFailureNoProgress(
		Test,
		DecodeFailure,
		TEXT("PropertyWrite.DecodeFailed"),
		DecodeTransaction,
		Asset,
		false,
		0,
		Observer,
		DecodeObserverBefore,
		0,
		TEXT("final-row decode failure"));

	TArray<FAssetDocumentRuntimeValueDto> MalformedArguments;
	FAssetDocumentRuntimeValueDto TopArray;
	TopArray.Kind = EAssetDocumentRuntimeValueKind::Array;
	MalformedArguments.Add(MoveTemp(TopArray));
	MalformedArguments.Add(MakeBatchWriteArguments({
		MakeBatchRow(TEXT(""), MakeBooleanValue(true)),
	}));
	FAssetDocumentRuntimeValueDto WrongName = MakeBatchWriteArguments({
		MakeBatchRow(TEXT("BooleanValue"), MakeBooleanValue(true)),
	});
	WrongName.Elements[0].Elements[0].Elements[0] = MakeIntegerValue(7);
	MalformedArguments.Add(MoveTemp(WrongName));
	FAssetDocumentRuntimeValueDto ExtraRowKey = MakeBatchWriteArguments({
		MakeBatchRow(TEXT("BooleanValue"), MakeBooleanValue(true)),
	});
	ExtraRowKey.Elements[0].Elements[0].ObjectKeys.Add(TEXT("extra"));
	ExtraRowKey.Elements[0].Elements[0].Elements.Add(MakeBooleanValue(true));
	MalformedArguments.Add(MoveTemp(ExtraRowKey));
	FAssetDocumentRuntimeValueDto BytesValue = MakeBatchWriteArguments({
		MakeBatchRow(TEXT("BooleanValue"), MakeBooleanValue(true)),
	});
	BytesValue.Elements[0].Elements[0].Elements[1] = {};
	BytesValue.Elements[0].Elements[0].Elements[1].Kind =
		EAssetDocumentRuntimeValueKind::Bytes;
	BytesValue.Elements[0].Elements[0].Elements[1].BytesValue = {0x01};
	MalformedArguments.Add(MoveTemp(BytesValue));
	for (int32 Index = 0; Index < MalformedArguments.Num(); ++Index)
	{
		const bool bBefore = Asset.BooleanValue;
		const int64 IntegerBefore = Asset.IntegerValue;
		const int32 ObserverBefore = Observer.Num();
		FDuplicateRejectingTransaction Transaction(Context);
		const FAssetDocumentPrimitiveOutcome Outcome = InvokeBatchWrite(
			Provider,
			BatchDescriptor,
			Context,
			Handles,
			MutableAsset,
			Composite,
			MoveTemp(MalformedArguments[Index]),
			&Transaction);
		TestBatchFailureNoProgress(
			Test,
			Outcome,
			Index == 0
				? TEXT("PrimitiveInvocation.InvalidValueArguments")
				: TEXT("PrimitiveInvocation.InvalidValueShape"),
			Transaction,
			Asset,
			bBefore,
			IntegerBefore,
			Observer,
			ObserverBefore,
			0,
			*FString::Printf(TEXT("malformed batch %d"), Index));
	}

	struct FNamedRowsFailure
	{
		const TCHAR* Label;
		const TCHAR* Code;
		TArray<FAssetDocumentRuntimeValueDto> Rows;
	};
	TArray<FNamedRowsFailure> NamedFailures;
	NamedFailures.Add({
		TEXT("duplicate batch row"),
		TEXT("PropertyBatchWrite.DuplicateProperty"),
		{
			MakeBatchRow(TEXT("BooleanValue"), MakeBooleanValue(true)),
			MakeBatchRow(TEXT("BooleanValue"), MakeBooleanValue(false)),
		}});
	NamedFailures.Add({
		TEXT("unknown batch row"),
		TEXT("PropertyBatchWrite.UnknownProperty"),
		{MakeBatchRow(TEXT("UnknownValue"), MakeBooleanValue(true))}});
	NamedFailures.Add({
		TEXT("out-of-order batch row"),
		TEXT("PropertyBatchWrite.OutOfOrder"),
		{
			MakeBatchRow(TEXT("IntegerValue"), MakeIntegerValue(5)),
			MakeBatchRow(TEXT("BooleanValue"), MakeBooleanValue(true)),
		}});
	for (FNamedRowsFailure& Case : NamedFailures)
	{
		const bool bBefore = Asset.BooleanValue;
		const int64 IntegerBefore = Asset.IntegerValue;
		const int32 ObserverBefore = Observer.Num();
		FDuplicateRejectingTransaction Transaction(Context);
		const FAssetDocumentPrimitiveOutcome Outcome = InvokeBatchWrite(
			Provider,
			BatchDescriptor,
			Context,
			Handles,
			MutableAsset,
			Composite,
			MakeBatchWriteArguments(MoveTemp(Case.Rows)),
			&Transaction);
		TestBatchFailureNoProgress(
			Test,
			Outcome,
			Case.Code,
			Transaction,
			Asset,
			bBefore,
			IntegerBefore,
			Observer,
			ObserverBefore,
			0,
			Case.Label);
	}

	const bool bBooleanEditConst =
		BooleanProperty.HasAnyPropertyFlags(CPF_EditConst);
	BooleanProperty.SetPropertyFlags(CPF_EditConst);
	const TOptional<FAssetDocumentOpaqueHandle> NonWritableReadOnlyAsset =
		IssueAsset(Handles, Asset, EAssetDocumentHandleAccess::ReadOnly);
	const FAssetDocumentPrimitiveOutcome NonWritableAdmission =
		InvokeBatchEnumeration(
			Provider,
			EnumerateDescriptor,
			Context,
			Handles,
			NonWritableReadOnlyAsset.GetValue());
	if (!bBooleanEditConst)
	{
		BooleanProperty.ClearPropertyFlags(CPF_EditConst);
	}
	const FAssetDocumentPrimitiveHandleResult* NonWritableBatch =
		FindHandleGroup(NonWritableAdmission, TEXT("property_batch"));
	if (!Test.TestNotNull(
		TEXT("non-writable admission returns a composite token"),
		NonWritableBatch)
		|| !Test.TestEqual(
			TEXT("non-writable admission returns one token"),
			NonWritableBatch != nullptr ? NonWritableBatch->Handles.Num() : 0,
			1))
	{
		return false;
	}
	const bool bNonWritableBefore = Asset.BooleanValue;
	const int64 NonWritableIntegerBefore = Asset.IntegerValue;
	const int32 NonWritableObserverBefore = Observer.Num();
	FDuplicateRejectingTransaction NonWritableTransaction(Context);
	const FAssetDocumentPrimitiveOutcome NonWritable = InvokeBatchWrite(
		Provider,
		BatchDescriptor,
		Context,
		Handles,
		MutableAsset,
		NonWritableBatch->Handles[0],
		MakeBatchWriteArguments({
			MakeBatchRow(TEXT("BooleanValue"), MakeBooleanValue(true)),
		}),
		&NonWritableTransaction);
	TestBatchFailureNoProgress(
		Test,
		NonWritable,
		TEXT("PropertyToken.InvalidPayload"),
		NonWritableTransaction,
		Asset,
		bNonWritableBefore,
		NonWritableIntegerBefore,
		Observer,
		NonWritableObserverBefore,
		0,
		TEXT("non-writable admitted row"));

	const bool bSubtypeBefore = Asset.BooleanValue;
	const int64 SubtypeIntegerBefore = Asset.IntegerValue;
	const int32 SubtypeObserverBefore = Observer.Num();
	FDuplicateRejectingTransaction ScalarInBatchTransaction(Context);
	const FAssetDocumentPrimitiveOutcome ScalarInBatch = InvokeBatchWrite(
		Provider,
		BatchDescriptor,
		Context,
		Handles,
		MutableAsset,
		BooleanScalar,
		MakeBatchWriteArguments({}),
		&ScalarInBatchTransaction);
	TestBatchFailureNoProgress(
		Test,
		ScalarInBatch,
		TEXT("PropertyToken.InvalidPayload"),
		ScalarInBatchTransaction,
		Asset,
		bSubtypeBefore,
		SubtypeIntegerBefore,
		Observer,
		SubtypeObserverBefore,
		0,
		TEXT("scalar token in batch lane"));

	FDuplicateRejectingTransaction CompositeInScalarTransaction(Context);
	const FAssetDocumentPrimitiveOutcome CompositeInScalar =
		InvokeScalarWrite(
			Provider,
			ScalarDescriptor,
			Context,
			Handles,
			MutableAsset,
			Composite,
			MakeBooleanValue(true),
			CompositeInScalarTransaction);
	TestBatchFailureNoProgress(
		Test,
		CompositeInScalar,
		TEXT("PropertyToken.InvalidPayload"),
		CompositeInScalarTransaction,
		Asset,
		bSubtypeBefore,
		SubtypeIntegerBefore,
		Observer,
		SubtypeObserverBefore,
		0,
		TEXT("composite token in scalar lane"));

	TStrongObjectPtr<UAssetDocumentEditorCodecTestObject> OtherTarget(
		NewObject<UAssetDocumentEditorCodecTestObject>(
			GetTransientPackage(), NAME_None, RF_Transactional));
	const TOptional<FAssetDocumentOpaqueHandle> OtherMutable =
		IssueAsset(Handles, *OtherTarget, EAssetDocumentHandleAccess::Mutable);
	FDuplicateRejectingTransaction OtherTargetTransaction(Context);
	const FAssetDocumentPrimitiveOutcome OtherTargetOutcome = InvokeBatchWrite(
		Provider,
		BatchDescriptor,
		Context,
		Handles,
		OtherMutable.GetValue(),
		Composite,
		MakeBatchWriteArguments({}),
		&OtherTargetTransaction);
	Test.TestEqual(TEXT("other target batch fails"),
		OtherTargetOutcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestTrue(TEXT("other target batch reports target mismatch"),
		HasDiagnostic(
			OtherTargetOutcome, TEXT("PropertyBatchWrite.TargetMismatch")));
	TestNoPublication(Test, OtherTargetOutcome, TEXT("other target batch"));
	Test.TestEqual(TEXT("other target batch does not enlist"),
		OtherTargetTransaction.GetEnlistCallCount(), 0);

	const auto TestStaleHandle =
		[&](const TCHAR* Label,
			FAssetDocumentOpaqueHandle Stale,
			const TCHAR* Code)
		{
			const bool bBefore = Asset.BooleanValue;
			const int64 IntegerBefore = Asset.IntegerValue;
			const int32 ObserverBefore = Observer.Num();
			FDuplicateRejectingTransaction Transaction(Context);
			const FAssetDocumentPrimitiveOutcome Outcome = InvokeBatchWrite(
				Provider,
				BatchDescriptor,
				Context,
				Handles,
				MutableAsset,
				MoveTemp(Stale),
				MakeBatchWriteArguments({}),
				&Transaction);
			TestBatchFailureNoProgress(
				Test,
				Outcome,
				Code,
				Transaction,
				Asset,
				bBefore,
				IntegerBefore,
				Observer,
				ObserverBefore,
				0,
				Label);
		};
	FAssetDocumentOpaqueHandle StaleRequest = Composite;
	StaleRequest.RequestId = FGuid::NewGuid();
	TestStaleHandle(
		TEXT("stale request"), MoveTemp(StaleRequest),
		TEXT("HandleMetadata.RequestMismatch"));
	FAssetDocumentOpaqueHandle StaleTransaction = Composite;
	StaleTransaction.TransactionId = FGuid::NewGuid();
	TestStaleHandle(
		TEXT("stale transaction"), MoveTemp(StaleTransaction),
		TEXT("HandleMetadata.TransactionMismatch"));
	FAssetDocumentOpaqueHandle StaleRevision = Composite;
	++StaleRevision.RuntimeRevision;
	TestStaleHandle(
		TEXT("stale runtime revision"), MoveTemp(StaleRevision),
		TEXT("HandleMetadata.RuntimeRevisionMismatch"));
	FAssetDocumentOpaqueHandle StaleProvider = Composite;
	StaleProvider.ProviderId = TEXT("other-provider");
	TestStaleHandle(
		TEXT("stale provider"), MoveTemp(StaleProvider),
		TEXT("HandleMetadata.ProviderMismatch"));
	FAssetDocumentOpaqueHandle StaleGeneration = Composite;
	++StaleGeneration.Generation;
	TestStaleHandle(
		TEXT("stale generation"), MoveTemp(StaleGeneration),
		TEXT("HandleMetadata.StaleHandle"));

	Asset.BooleanValue = false;
	Asset.IntegerValue = 0;
	const int32 EnlistObserverBefore = Observer.Num();
	FDuplicateRejectingTransaction EnlistFailureTransaction(Context, true);
	const FAssetDocumentPrimitiveOutcome EnlistFailure = InvokeBatchWrite(
		Provider,
		BatchDescriptor,
		Context,
		Handles,
		MutableAsset,
		Composite,
		MakeBatchWriteArguments({
			MakeBatchRow(TEXT("BooleanValue"), MakeBooleanValue(true)),
		}),
		&EnlistFailureTransaction);
	TestBatchFailureNoProgress(
		Test,
		EnlistFailure,
		TEXT("Atomic.TestInjectedEnlistFailure"),
		EnlistFailureTransaction,
		Asset,
		false,
		0,
		Observer,
		EnlistObserverBefore,
		1,
		TEXT("enlist rejection"));

	Asset.BooleanValue = false;
	Asset.IntegerValue = 0;
	const int32 ApplyObserverBefore = Observer.Num();
	const bool bIntegerEditConst =
		IntegerProperty.HasAnyPropertyFlags(CPF_EditConst);
	IntegerProperty.SetPropertyFlags(CPF_EditConst);
	FDuplicateRejectingTransaction ApplyFailureTransaction(Context);
	const FAssetDocumentPrimitiveOutcome ApplyFailure = InvokeBatchWrite(
		Provider,
		BatchDescriptor,
		Context,
		Handles,
		MutableAsset,
		Composite,
		MakeBatchWriteArguments({
			MakeBatchRow(TEXT("BooleanValue"), MakeBooleanValue(true)),
			MakeBatchRow(TEXT("IntegerValue"), MakeIntegerValue(73)),
		}),
		&ApplyFailureTransaction);
	if (!bIntegerEditConst)
	{
		IntegerProperty.ClearPropertyFlags(CPF_EditConst);
	}
	Test.TestEqual(TEXT("second-row apply failure fails"),
		ApplyFailure.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestTrue(TEXT("second-row apply failure has stable diagnostic"),
		HasDiagnostic(ApplyFailure, TEXT("PropertyWrite.AccessFailed")));
	TestNoPublication(Test, ApplyFailure, TEXT("second-row apply failure"));
	Test.TestTrue(TEXT("first row remains for outer rollback"),
		Asset.BooleanValue);
	Test.TestEqual(TEXT("failed second row remains unchanged"),
		Asset.IntegerValue, 0ll);
	Test.TestEqual(TEXT("apply failure enlists only once"),
		ApplyFailureTransaction.GetEnlistCallCount(), 1);
	Test.TestEqual(TEXT("apply failure does not retry"),
		Observer.Num(), ApplyObserverBefore + 1);

	const FAssetDocumentPrimitiveOutcome NoTransaction = InvokeBatchWrite(
		Provider,
		BatchDescriptor,
		Context,
		Handles,
		MutableAsset,
		Composite,
		MakeBatchWriteArguments({}),
		nullptr);
	Test.TestEqual(TEXT("batch requires an active transaction"),
		NoTransaction.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestTrue(TEXT("missing transaction reports stable failure"),
		HasDiagnostic(
			NoTransaction, TEXT("GenericProvider.InvalidTransaction")));
	return true;
}

bool TestPropertyOnlyPreparationShape(
	FAutomationTestBase& Test,
	UObject& ExactTarget)
{
	FAssetDocumentEditorCutoverMutationIntent Intent;
	Intent.RequestId = FGuid::NewGuid();
	Intent.RuntimeRevision = 1;
	Intent.CanonicalObjectPath = ExactTarget.GetPathName();
	Intent.ExactAssetClassPath = ExactTarget.GetClass()->GetPathName();
	Intent.bWriteAdmittedProperties = true;
	const FAssetDocumentEditorCutoverTransactionPreparation Preparation =
		PrepareAssetDocumentEditorCutoverTransaction(Intent);
	if (!Test.TestTrue(
			TEXT("property-only preparation succeeds"),
			Preparation.bPrepared)
		|| !Test.TestEqual(
			TEXT("property-only preparation has two footprint rows"),
			Preparation.CompleteFootprint.Num(), 2))
	{
		return false;
	}
	Test.TestEqual(
		TEXT("property-only object rollback component is exact"),
		Preparation.CompleteFootprint[0].RollbackComponentId,
		FString(TEXT("rollback.uobject")));
	Test.TestEqual(
		TEXT("property-only package rollback component is exact"),
		Preparation.CompleteFootprint[1].RollbackComponentId,
		FString(TEXT("rollback.package")));
	if (!Test.TestEqual(
			TEXT("property-only preparation has two rollback seeds"),
			Preparation.RollbackSeeds.Num(), 2))
	{
		return false;
	}
	Test.TestEqual(
		TEXT("property-only object rollback seed is exact"),
		Preparation.RollbackSeeds[0]->GetRollbackComponentId(),
		FString(TEXT("rollback.uobject")));
	Test.TestEqual(
		TEXT("property-only package rollback seed is exact"),
		Preparation.RollbackSeeds[1]->GetRollbackComponentId(),
		FString(TEXT("rollback.package")));
	return true;
}

bool TestPreimageAdmissionSurface(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveOutcome& Admission,
	const FAssetDocumentRuntimeValueDto* Rows,
	const FAssetDocumentPrimitiveHandleResult* Batch,
	const FAssetDocumentPrimitiveHandleResult* Scalars)
{
	bool bValid = Test.TestEqual(
		TEXT("preimage admission succeeds"),
		Admission.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	bValid &= Test.TestNotNull(
		TEXT("preimage admission returns rows"), Rows);
	bValid &= Test.TestNotNull(
		TEXT("preimage admission returns batch handle"), Batch);
	bValid &= Test.TestNotNull(
		TEXT("preimage admission returns scalar handles"), Scalars);
	const int32 BatchCount = Batch != nullptr ? Batch->Handles.Num() : 0;
	bValid &= Test.TestEqual(
		TEXT("preimage admission returns one batch handle"),
		BatchCount, 1);
	return bValid;
}

bool IsWritablePreimageRow(const FAssetDocumentRuntimeValueDto* Writable)
{
	if (Writable == nullptr)
	{
		return false;
	}
	if (Writable->Kind != EAssetDocumentRuntimeValueKind::Boolean)
	{
		return false;
	}
	return Writable->BooleanValue;
}

bool IsPreimageScalarIndexValid(
	int32 Index,
	const FAssetDocumentPrimitiveHandleResult& Scalars)
{
	if (Index == INDEX_NONE)
	{
		return false;
	}
	return Scalars.Handles.IsValidIndex(Index);
}

bool TryReadPreimagePropertyContract(
	FAutomationTestBase& Test,
	const FAssetDocumentRuntimeValueDto& Rows,
	const FAssetDocumentPrimitiveHandleResult& Scalars,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	const FAssetDocumentEditorProviderTestContext& Context,
	FProperty*& OutBooleanProperty,
	FProperty*& OutIntegerProperty)
{
	const int32 StringIndex = FindPropertyRowIndex(Rows, TEXT("StringValue"));
	const int32 BooleanIndex = FindPropertyRowIndex(Rows, TEXT("BooleanValue"));
	const int32 IntegerIndex = FindPropertyRowIndex(Rows, TEXT("IntegerValue"));
	const FAssetDocumentRuntimeValueDto* Writable = StringIndex != INDEX_NONE
		? FindField(Rows.Elements[StringIndex], TEXT("writable"))
		: nullptr;
	bool bValid = Test.TestTrue(
		TEXT("unchanged string is admitted"), StringIndex != INDEX_NONE);
	bValid &= Test.TestTrue(
		TEXT("unchanged string is writable"),
		IsWritablePreimageRow(Writable));
	bValid &= Test.TestTrue(
		TEXT("boolean scalar token exists"),
		IsPreimageScalarIndexValid(BooleanIndex, Scalars));
	bValid &= Test.TestTrue(
		TEXT("integer scalar token exists"),
		IsPreimageScalarIndexValid(IntegerIndex, Scalars));
	bValid &= Test.TestTrue(
		TEXT("later-ordered IntegerValue follows BooleanValue"),
		IntegerIndex > BooleanIndex);
	if (!bValid)
	{
		return false;
	}
	FAssetDocumentEditorPropertyTokenView BooleanToken;
	FAssetDocumentEditorPropertyTokenView IntegerToken;
	const bool bBooleanRead = TryReadPropertyTokenView(
		Handles, Context, Scalars.Handles[BooleanIndex], BooleanToken);
	const bool bIntegerRead = TryReadPropertyTokenView(
		Handles, Context, Scalars.Handles[IntegerIndex], IntegerToken);
	OutBooleanProperty = BooleanToken.Property;
	OutIntegerProperty = IntegerToken.Property;
	bValid = Test.TestTrue(
		TEXT("boolean scalar token exposes exact property"), bBooleanRead);
	bValid &= Test.TestTrue(
		TEXT("integer scalar token exposes exact property"), bIntegerRead);
	return bValid;
}

bool TestPreimageAdmissionProjection(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	UAssetDocumentEditorCodecTestObject& Asset,
	FAssetDocumentOpaqueHandle& OutComposite,
	FProperty*& OutBooleanProperty,
	FProperty*& OutIntegerProperty,
	FAssetDocumentEditorPropertyPreimageCaptureResult& OutCapture)
{
	const TOptional<FAssetDocumentOpaqueHandle> ReadOnlyAsset =
		IssueAsset(Handles, Asset, EAssetDocumentHandleAccess::ReadOnly);
	if (!Test.TestTrue(TEXT("preimage read-only asset handle issues"),
			ReadOnlyAsset.IsSet()))
	{
		return false;
	}
	const FAssetDocumentPrimitiveOutcome Admission = InvokeBatchEnumeration(
		Provider, Descriptor, Context, Handles, ReadOnlyAsset.GetValue());
	const FAssetDocumentRuntimeValueDto* Rows =
		FindField(Admission.Value, TEXT("properties"));
	const FAssetDocumentPrimitiveHandleResult* Batch =
		FindHandleGroup(Admission, TEXT("property_batch"));
	const FAssetDocumentPrimitiveHandleResult* Scalars =
		FindHandleGroup(Admission, TEXT("properties"));
	if (!TestPreimageAdmissionSurface(
		Test, Admission, Rows, Batch, Scalars))
	{
		return false;
	}
	if (!TryReadPreimagePropertyContract(
		Test, *Rows, *Scalars, Handles, Context,
		OutBooleanProperty, OutIntegerProperty))
	{
		return false;
	}
	OutComposite = Batch->Handles[0];
	OutCapture = CaptureAssetDocumentEditorWritablePropertyPreimage(Asset);
	const TOptional<FAssetDocumentOpaqueHandle> AfterCapture =
		IssueAsset(Handles, Asset, EAssetDocumentHandleAccess::ReadOnly);
	return Test.TestTrue(TEXT("post-capture handle issues"),
			AfterCapture.IsSet())
		&& Test.TestEqual(TEXT("capture issues no handle"),
			AfterCapture->Slot, Batch->Handles[0].Slot + 1);
}

bool TestPreimageCaptureIsPassive(
	FAutomationTestBase& Test,
	const UAssetDocumentEditorCodecTestObject& Asset,
	const UPackage& Package,
	const FScopedBatchPropertyObserver& Observer,
	const FDuplicateRejectingTransaction& Transaction,
	bool bOriginalDirty,
	const FAssetDocumentEditorPropertyPreimageCaptureResult& Capture)
{
	Test.TestFalse(TEXT("capture no longer reports NotImplemented"),
		HasPreimageCaptureDiagnostic(
			Capture, TEXT("PropertyPreimage.NotImplemented")));
	Test.TestTrue(TEXT("preimage capture succeeds"), Capture.bSucceeded);
	Test.TestTrue(TEXT("preimage capture publishes ready state"),
		Capture.Preimage.IsReady());
	Test.TestTrue(TEXT("successful capture has no diagnostic"),
		Capture.Diagnostics.IsEmpty());
	Test.TestFalse(TEXT("capture preserves boolean"), Asset.BooleanValue);
	Test.TestEqual(TEXT("capture preserves integer"), Asset.IntegerValue, 7ll);
	Test.TestEqual(TEXT("capture preserves string"), Asset.StringValue,
		FString(TEXT("preserved")));
	Test.TestEqual(TEXT("capture preserves dirty state"),
		Package.IsDirty(), bOriginalDirty);
	Test.TestEqual(TEXT("capture emits no notification"), Observer.Num(), 0);
	Test.TestEqual(TEXT("capture does not enlist"),
		Transaction.GetEnlistCallCount(), 0);
	return Capture.bSucceeded && Capture.Preimage.IsReady();
}

FAssetDocumentRuntimeValueDto MakePreimageMutationArguments()
{
	return MakeBatchWriteArguments({
		MakeBatchRow(TEXT("BooleanValue"), MakeBooleanValue(true)),
		MakeBatchRow(TEXT("IntegerValue"), MakeIntegerValue(73)),
	});
}

bool TryGetPreimageDescriptors(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor*& OutEnumerate,
	const FAssetDocumentPrimitiveDescriptor*& OutBatchWrite)
{
	OutEnumerate = FindDescriptor(
		Provider, TEXT("editor.property.enumerate_admitted_batch.v1"));
	OutBatchWrite = FindDescriptor(
		Provider, TEXT("editor.property.write_admitted_batch.v1"));
	bool bValid = Test.TestNotNull(
		TEXT("preimage enumerate descriptor exists"), OutEnumerate);
	bValid &= Test.TestNotNull(
		TEXT("preimage batch descriptor exists"), OutBatchWrite);
	return bValid;
}

bool TestPreimageBatchMutation(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& BatchWrite,
	const FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	FAssetDocumentOpaqueHandle Composite,
	FDuplicateRejectingTransaction& Transaction,
	UAssetDocumentEditorCodecTestObject& Asset,
	UPackage& Package)
{
	const TOptional<FAssetDocumentOpaqueHandle> MutableAsset =
		IssueAsset(Handles, Asset, EAssetDocumentHandleAccess::Mutable);
	if (!Test.TestTrue(TEXT("preimage mutable asset handle issues"),
		MutableAsset.IsSet()))
	{
		return false;
	}
	const FAssetDocumentPrimitiveOutcome Mutation = InvokeBatchWrite(
		Provider, BatchWrite, Context, Handles, MutableAsset.GetValue(),
		MoveTemp(Composite), MakePreimageMutationArguments(), &Transaction);
	Test.TestEqual(TEXT("real preimage batch mutation succeeds"), Mutation.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	Test.TestTrue(TEXT("real batch mutates boolean"), Asset.BooleanValue);
	Test.TestEqual(TEXT("real batch mutates integer"), Asset.IntegerValue, 73ll);
	Test.TestEqual(TEXT("real batch preserves string"), Asset.StringValue,
		FString(TEXT("preserved")));
	Test.TestEqual(TEXT("real batch enlists once"),
		Transaction.GetEnlistCallCount(), 1);
	Test.TestTrue(TEXT("real batch dirties package"), Package.IsDirty());
	return Mutation.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
}

bool TestPreimageRestoreSuccess(
	FAutomationTestBase& Test,
	FAssetDocumentEditorPropertyPreimage& Preimage,
	const UAssetDocumentEditorCodecTestObject& Asset,
	const UPackage& Package,
	const FScopedBatchPropertyObserver& Observer,
	int32 RestoreObserverBefore,
	bool bOriginalDirty)
{
	const FAssetDocumentContractValidationResult Restore = Preimage.Restore();
	Test.TestTrue(TEXT("first preimage restore succeeds"), Restore.bValid);
	Test.TestTrue(TEXT("successful restore has no diagnostic"),
		Restore.Diagnostics.IsEmpty());
	Test.TestFalse(TEXT("restore recovers boolean"), Asset.BooleanValue);
	Test.TestEqual(TEXT("restore recovers integer"), Asset.IntegerValue, 7ll);
	Test.TestEqual(TEXT("restore preserves unchanged string"), Asset.StringValue,
		FString(TEXT("preserved")));
	Test.TestEqual(TEXT("restore recovers original dirty state"),
		Package.IsDirty(), bOriginalDirty);
	Test.TestEqual(TEXT("restore notifies only changed fields"),
		TArray<FString>(Observer.Since(RestoreObserverBefore)),
		TArray<FString>({TEXT("BooleanValue"), TEXT("IntegerValue")}));
	return Restore.bValid;
}

void TestPreimageAlreadyConsumed(
	FAutomationTestBase& Test,
	FAssetDocumentEditorPropertyPreimage& Preimage,
	const UAssetDocumentEditorCodecTestObject& Asset,
	const UPackage& Package,
	const FScopedBatchPropertyObserver& Observer)
{
	const bool bBooleanBefore = Asset.BooleanValue;
	const int64 IntegerBefore = Asset.IntegerValue;
	const bool bDirtyBefore = Package.IsDirty();
	const int32 ObserverBefore = Observer.Num();
	const FAssetDocumentContractValidationResult Second = Preimage.Restore();
	Test.TestFalse(TEXT("second preimage restore fails closed"), Second.bValid);
	Test.TestEqual(TEXT("second restore has one diagnostic"),
		Second.Diagnostics.Num(), 1);
	Test.TestTrue(TEXT("second restore reports already consumed"),
		HasContractDiagnostic(Second, TEXT("PropertyPreimage.AlreadyConsumed")));
	Test.TestEqual(TEXT("second restore preserves boolean"),
		Asset.BooleanValue, bBooleanBefore);
	Test.TestEqual(TEXT("second restore preserves integer"),
		Asset.IntegerValue, IntegerBefore);
	Test.TestEqual(TEXT("second restore preserves dirty state"),
		Package.IsDirty(), bDirtyBefore);
	Test.TestEqual(TEXT("second restore emits no notification"),
		Observer.Num(), ObserverBefore);
}

void TestPreimageFailureTerminal(
	FAutomationTestBase& Test,
	FAssetDocumentEditorPropertyPreimage& Preimage,
	const UAssetDocumentEditorCodecTestObject& Asset,
	const UPackage& Package,
	const FScopedBatchPropertyObserver& Observer,
	FStringView ExpectedCode)
{
	const int32 ObserverBefore = Observer.Num();
	const FAssetDocumentContractValidationResult First = Preimage.Restore();
	Test.TestFalse(TEXT("drifted restore fails closed"), First.bValid);
	Test.TestEqual(TEXT("first failure has one diagnostic"),
		First.Diagnostics.Num(), 1);
	Test.TestTrue(TEXT("first failure reports exact diagnostic"),
		HasContractDiagnostic(First, ExpectedCode));
	Test.TestTrue(TEXT("failed restore preserves current boolean"),
		Asset.BooleanValue);
	Test.TestEqual(TEXT("failed restore preserves current integer"),
		Asset.IntegerValue, 73ll);
	Test.TestTrue(TEXT("failed restore leaves package dirty"),
		Package.IsDirty());
	Test.TestEqual(TEXT("failed restore emits no notification"),
		Observer.Num(), ObserverBefore);
	TestPreimageAlreadyConsumed(Test, Preimage, Asset, Package, Observer);
}

void InitializePreimageTestValues(
	UAssetDocumentEditorCodecTestObject& Asset,
	UPackage& Package,
	bool bDirty)
{
	Asset.BooleanValue = false;
	Asset.IntegerValue = 7;
	Asset.StringValue = TEXT("preserved");
	Package.SetDirtyFlag(bDirty);
}

bool TryCaptureAndMutatePreimage(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& Enumerate,
	const FAssetDocumentPrimitiveDescriptor& BatchWrite,
	const FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	UAssetDocumentEditorCodecTestObject& Asset,
	UPackage& Package,
	FAssetDocumentEditorPropertyPreimage& OutPreimage,
	FProperty*& OutBooleanProperty,
	FProperty*& OutIntegerProperty)
{
	FAssetDocumentOpaqueHandle Composite;
	FAssetDocumentEditorPropertyPreimageCaptureResult Capture;
	if (!TestPreimageAdmissionProjection(
			Test, Provider, Enumerate, Context, Handles, Asset,
			Composite, OutBooleanProperty, OutIntegerProperty, Capture))
	{
		return false;
	}
	if (!Test.TestTrue(TEXT("drift fixture capture succeeds"),
			Capture.bSucceeded)
		|| !Test.TestTrue(TEXT("drift fixture preimage is ready"),
			Capture.Preimage.IsReady()))
	{
		return false;
	}
	OutPreimage = MoveTemp(Capture.Preimage);
	FDuplicateRejectingTransaction Transaction(Context);
	return TestPreimageBatchMutation(
		Test, Provider, BatchWrite, Context, Handles,
		Composite, Transaction, Asset, Package);
}

bool RunPreimageSuccessCase(
	FAutomationTestBase& Test,
	bool bOriginalDirty)
{
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Enumerate = nullptr;
	const FAssetDocumentPrimitiveDescriptor* BatchWrite = nullptr;
	if (!TryGetPreimageDescriptors(
		Test, *Provider, Enumerate, BatchWrite))
	{
		return false;
	}
	const FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FScopedPreimageTestAsset Fixture;
	UAssetDocumentEditorCodecTestObject& Asset = Fixture.GetAsset();
	UPackage& Package = Fixture.GetPackage();
	InitializePreimageTestValues(Asset, Package, bOriginalDirty);
	FScopedBatchPropertyObserver Observer(Asset);
	FDuplicateRejectingTransaction Transaction(Context);
	FAssetDocumentOpaqueHandle Composite;
	FProperty* BooleanProperty = nullptr;
	FProperty* IntegerProperty = nullptr;
	FAssetDocumentEditorPropertyPreimageCaptureResult Capture;
	if (!TestPreimageAdmissionProjection(
			Test, *Provider, *Enumerate, Context, Handles, Asset,
			Composite, BooleanProperty, IntegerProperty, Capture))
	{
		return false;
	}
	if (!TestPreimageCaptureIsPassive(
		Test, Asset, Package, Observer, Transaction, bOriginalDirty, Capture))
	{
		return false;
	}
	FAssetDocumentEditorPropertyPreimage Preimage = MoveTemp(Capture.Preimage);
	const FAssetDocumentContractValidationResult MovedFrom =
		Capture.Preimage.Restore();
	Test.TestTrue(TEXT("moved-from restore reports invalid state"),
		HasContractDiagnostic(MovedFrom, TEXT("PropertyPreimage.InvalidState")));
	if (!TestPreimageBatchMutation(
		Test, *Provider, *BatchWrite, Context, Handles,
		Composite, Transaction, Asset, Package))
	{
		return false;
	}
	const int32 RestoreObserverBefore = Observer.Num();
	const bool bRestored = TestPreimageRestoreSuccess(
			Test, Preimage, Asset, Package, Observer,
			RestoreObserverBefore, bOriginalDirty);
	TestPreimageAlreadyConsumed(Test, Preimage, Asset, Package, Observer);
	return bRestored;
}

bool RunPreimageIdentityDriftCase(FAutomationTestBase& Test)
{
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Enumerate = nullptr;
	const FAssetDocumentPrimitiveDescriptor* BatchWrite = nullptr;
	if (!TryGetPreimageDescriptors(
		Test, *Provider, Enumerate, BatchWrite))
	{
		return false;
	}
	const FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FScopedPreimageTestAsset Fixture;
	UAssetDocumentEditorCodecTestObject& Asset = Fixture.GetAsset();
	UPackage& Package = Fixture.GetPackage();
	InitializePreimageTestValues(Asset, Package, false);
	FScopedBatchPropertyObserver Observer(Asset);
	FAssetDocumentEditorPropertyPreimage Preimage;
	FProperty* BooleanProperty = nullptr;
	FProperty* IntegerProperty = nullptr;
	if (!TryCaptureAndMutatePreimage(
			Test, *Provider, *Enumerate, *BatchWrite, Context, Handles,
			Asset, Package, Preimage, BooleanProperty, IntegerProperty))
	{
		return false;
	}
	const FName OriginalName = Asset.GetFName();
	UObject* const OriginalOuter = Asset.GetOuter();
	const FString OriginalPath = Asset.GetPathName();
	const FString TemporaryName = FString::Printf(
		TEXT("PropertyPreimageIdentityDrift_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	constexpr ERenameFlags RenameFlags =
		REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty;
	const bool bRenamed = Asset.Rename(
		*TemporaryName, OriginalOuter, RenameFlags);
	ON_SCOPE_EXIT
	{
		const bool bRestored = Asset.Rename(
			*OriginalName.ToString(), OriginalOuter, RenameFlags);
		Test.TestTrue(TEXT("identity drift cleanup rename succeeds"), bRestored);
		Test.TestEqual(TEXT("identity drift cleanup restores path"),
			Asset.GetPathName(), OriginalPath);
	};
	Test.TestTrue(TEXT("identity drift rename succeeds"), bRenamed);
	Test.TestTrue(TEXT("identity drift changes object path"),
		Asset.GetPathName() != OriginalPath);
	Test.TestTrue(TEXT("identity drift preserves exact outer"),
		Asset.GetOuter() == OriginalOuter);
	Test.TestTrue(TEXT("identity drift preserves exact package"),
		Asset.GetOutermost() == &Package);
	TestPreimageFailureTerminal(
		Test, Preimage, Asset, Package, Observer,
		TEXT("PropertyPreimage.IdentityChanged"));
	return true;
}

void TestPreimageFieldIdentityUnchanged(
	FAutomationTestBase& Test,
	UAssetDocumentEditorCodecTestObject& Asset,
	UPackage& Package,
	FProperty& Property,
	UObject* OriginalTarget,
	UClass* OriginalClass,
	UClass* OriginalOwner,
	FStringView OriginalObjectPath)
{
	Test.TestTrue(TEXT("field drift preserves target pointer"),
		&Asset == OriginalTarget);
	Test.TestTrue(TEXT("field drift preserves package pointer"),
		Asset.GetOutermost() == &Package);
	Test.TestTrue(TEXT("field drift preserves class pointer"),
		Asset.GetClass() == OriginalClass);
	Test.TestEqual(TEXT("field drift preserves object path"),
		Asset.GetPathName(), FString(OriginalObjectPath));
	Test.TestTrue(TEXT("field drift preserves field owner"),
		Property.GetOwnerClass() == OriginalOwner);
}

bool RunPreimageFieldContractDriftCase(FAutomationTestBase& Test)
{
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Enumerate = nullptr;
	const FAssetDocumentPrimitiveDescriptor* BatchWrite = nullptr;
	if (!TryGetPreimageDescriptors(
			Test, *Provider, Enumerate, BatchWrite))
	{
		return false;
	}
	const FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FScopedPreimageTestAsset Fixture;
	UAssetDocumentEditorCodecTestObject& Asset = Fixture.GetAsset();
	UPackage& Package = Fixture.GetPackage();
	InitializePreimageTestValues(Asset, Package, false);
	FScopedBatchPropertyObserver Observer(Asset);
	FAssetDocumentEditorPropertyPreimage Preimage;
	FProperty* BooleanProperty = nullptr;
	FProperty* IntegerProperty = nullptr;
	if (!TryCaptureAndMutatePreimage(
			Test, *Provider, *Enumerate, *BatchWrite, Context, Handles,
			Asset, Package, Preimage, BooleanProperty, IntegerProperty))
	{
		return false;
	}
	if (!Test.TestNotNull(TEXT("field drift exact property exists"),
			BooleanProperty))
	{
		return false;
	}
	UObject* const OriginalTarget = &Asset;
	UClass* const OriginalClass = Asset.GetClass();
	UClass* const OriginalOwner = BooleanProperty->GetOwnerClass();
	const FString OriginalObjectPath = Asset.GetPathName();
	const FName OriginalFieldName = BooleanProperty->GetFName();
	const FName TemporaryFieldName(*FString::Printf(
		TEXT("BooleanValue_PreimageDrift_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	BooleanProperty->Rename(TemporaryFieldName);
	ON_SCOPE_EXIT
	{
		BooleanProperty->Rename(OriginalFieldName);
		Test.TestEqual(TEXT("field drift cleanup restores name"),
			BooleanProperty->GetFName(), OriginalFieldName);
	};
	TestPreimageFieldIdentityUnchanged(
		Test, Asset, Package, *BooleanProperty, OriginalTarget,
		OriginalClass, OriginalOwner, OriginalObjectPath);
	TestPreimageFailureTerminal(
		Test, Preimage, Asset, Package, Observer,
		TEXT("PropertyPreimage.TypeOrCodecChanged"));
	return true;
}

bool RunPreimageLaterAccessDriftCase(FAutomationTestBase& Test)
{
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Enumerate = nullptr;
	const FAssetDocumentPrimitiveDescriptor* BatchWrite = nullptr;
	if (!TryGetPreimageDescriptors(
			Test, *Provider, Enumerate, BatchWrite))
	{
		return false;
	}
	const FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FScopedPreimageTestAsset Fixture;
	UAssetDocumentEditorCodecTestObject& Asset = Fixture.GetAsset();
	UPackage& Package = Fixture.GetPackage();
	InitializePreimageTestValues(Asset, Package, false);
	FScopedBatchPropertyObserver Observer(Asset);
	FAssetDocumentEditorPropertyPreimage Preimage;
	FProperty* BooleanProperty = nullptr;
	FProperty* IntegerProperty = nullptr;
	if (!TryCaptureAndMutatePreimage(
			Test, *Provider, *Enumerate, *BatchWrite, Context, Handles,
			Asset, Package, Preimage, BooleanProperty, IntegerProperty))
	{
		return false;
	}
	if (!Test.TestNotNull(TEXT("access drift exact integer exists"),
			IntegerProperty))
	{
		return false;
	}
	const bool bHadEditConst =
		IntegerProperty->HasAnyPropertyFlags(CPF_EditConst);
	IntegerProperty->SetPropertyFlags(CPF_EditConst);
	ON_SCOPE_EXIT
	{
		if (!bHadEditConst)
		{
			IntegerProperty->ClearPropertyFlags(CPF_EditConst);
		}
		Test.TestEqual(TEXT("access drift cleanup restores original flag"),
			IntegerProperty->HasAnyPropertyFlags(CPF_EditConst),
			bHadEditConst);
	};
	TestPreimageFailureTerminal(
		Test, Preimage, Asset, Package, Observer,
		TEXT("PropertyPreimage.AccessChanged"));
	return true;
}

bool RunPreimageInvalidTargetCase(FAutomationTestBase& Test)
{
	UAssetDocumentEditorCodecTestObject& Cdo =
		*GetMutableDefault<UAssetDocumentEditorCodecTestObject>();
	UPackage& Package = *Cdo.GetOutermost();
	const bool bBooleanBefore = Cdo.BooleanValue;
	const int64 IntegerBefore = Cdo.IntegerValue;
	const bool bDirtyBefore = Package.IsDirty();
	FScopedBatchPropertyObserver Observer(Cdo);
	const FAssetDocumentEditorPropertyPreimageCaptureResult Capture =
		CaptureAssetDocumentEditorWritablePropertyPreimage(Cdo);
	Test.TestFalse(TEXT("CDO preimage capture fails closed"),
		Capture.bSucceeded);
	Test.TestFalse(TEXT("CDO preimage is not ready"),
		Capture.Preimage.IsReady());
	Test.TestEqual(TEXT("CDO capture has one diagnostic"),
		Capture.Diagnostics.Num(), 1);
	Test.TestTrue(TEXT("CDO capture reports invalid target"),
		HasPreimageCaptureDiagnostic(
			Capture, TEXT("PropertyPreimage.InvalidTarget")));
	Test.TestEqual(TEXT("CDO capture preserves boolean"),
		Cdo.BooleanValue, bBooleanBefore);
	Test.TestEqual(TEXT("CDO capture preserves integer"),
		Cdo.IntegerValue, IntegerBefore);
	Test.TestEqual(TEXT("CDO capture preserves dirty state"),
		Package.IsDirty(), bDirtyBefore);
	Test.TestEqual(TEXT("CDO capture emits no notification"),
		Observer.Num(), 0);
	return !Capture.bSucceeded && !Capture.Preimage.IsReady();
}
} // namespace AssetDocumentEditorPropertyBatchWriteTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderPropertyBatchWriteTest,
	"UEAssetDoc.GenericProvider.Property.BatchWrite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderPropertyBatchWriteTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorPropertyBatchWriteTests;

	FScopedEditableFixtureProperties EditableFixture;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* EnumerateDescriptor =
		FindDescriptor(
			*Provider, TEXT("editor.property.enumerate_admitted_batch.v1"));
	const FAssetDocumentPrimitiveDescriptor* ScalarWriteDescriptor =
		FindDescriptor(
			*Provider, TEXT("editor.property.write_admitted.v1"));
	const FAssetDocumentPrimitiveDescriptor* BatchWriteDescriptor =
		FindDescriptor(
			*Provider, TEXT("editor.property.write_admitted_batch.v1"));
	if (!TestNotNull(TEXT("batch enumeration descriptor exists"),
			EnumerateDescriptor)
		|| !TestNotNull(TEXT("scalar write descriptor exists"),
			ScalarWriteDescriptor)
		|| !TestNotNull(TEXT("batch write descriptor exists"),
			BatchWriteDescriptor))
	{
		return false;
	}
	TestExactBatchWriteDescriptor(*this, *BatchWriteDescriptor);

	const FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	TStrongObjectPtr<UAssetDocumentEditorCodecTestObject> Asset(
		NewObject<UAssetDocumentEditorCodecTestObject>(
			GetTransientPackage(), NAME_None, RF_Transactional));
	UPackage* const Package = Asset->GetOutermost();
	const bool bPackageDirtyBefore = Package->IsDirty();
	ON_SCOPE_EXIT
	{
		Package->SetDirtyFlag(bPackageDirtyBefore);
	};

	const TOptional<FAssetDocumentOpaqueHandle> ReadOnlyAsset =
		IssueAsset(
			Handles, *Asset, EAssetDocumentHandleAccess::ReadOnly);
	if (!TestTrue(TEXT("read-only asset handle issues"),
		ReadOnlyAsset.IsSet()))
	{
		return false;
	}
	const FAssetDocumentPrimitiveOutcome Admission =
		InvokeBatchEnumeration(
			*Provider,
			*EnumerateDescriptor,
			Context,
			Handles,
			ReadOnlyAsset.GetValue());
	if (!TestEqual(TEXT("real provider admits a composite property batch"),
		Admission.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto* Rows =
		FindField(Admission.Value, TEXT("properties"));
	const FAssetDocumentPrimitiveHandleResult* ScalarHandles =
		FindHandleGroup(Admission, TEXT("properties"));
	const FAssetDocumentPrimitiveHandleResult* BatchHandles =
		FindHandleGroup(Admission, TEXT("property_batch"));
	if (!TestNotNull(TEXT("admission returns property rows"), Rows)
		|| !TestNotNull(TEXT("admission returns scalar property handles"),
			ScalarHandles)
		|| !TestNotNull(TEXT("admission returns one composite handle group"),
			BatchHandles))
	{
		return false;
	}
	const int32 BooleanIndex =
		FindPropertyRowIndex(*Rows, TEXT("BooleanValue"));
	const int32 IntegerIndex =
		FindPropertyRowIndex(*Rows, TEXT("IntegerValue"));
	if (!TestTrue(TEXT("admission includes two writable scalar fixtures"),
		BooleanIndex != INDEX_NONE
			&& IntegerIndex != INDEX_NONE
			&& BooleanIndex != IntegerIndex)
		|| !TestTrue(TEXT("scalar handles parallel the admitted rows"),
			ScalarHandles->Handles.IsValidIndex(BooleanIndex)
			&& ScalarHandles->Handles.IsValidIndex(IntegerIndex))
		|| !TestEqual(TEXT("admission returns exactly one composite token"),
			BatchHandles->Handles.Num(), 1))
	{
		return false;
	}

	const TOptional<FAssetDocumentOpaqueHandle> MutableAsset =
		IssueAsset(
			Handles, *Asset, EAssetDocumentHandleAccess::Mutable);
	if (!TestTrue(TEXT("mutable asset handle issues"), MutableAsset.IsSet()))
	{
		return false;
	}
	FDuplicateRejectingTransaction Transaction(Context);
	FAssetDocumentEditorPropertyTokenView BooleanToken;
	FAssetDocumentEditorPropertyTokenView IntegerToken;
	if (!TestTrue(TEXT("boolean token exposes its admitted native view"),
		TryReadPropertyTokenView(
			Handles,
			Context,
			ScalarHandles->Handles[BooleanIndex],
			BooleanToken))
		|| !TestTrue(TEXT("integer token exposes its admitted native view"),
			TryReadPropertyTokenView(
				Handles,
				Context,
				ScalarHandles->Handles[IntegerIndex],
				IntegerToken)))
	{
		return false;
	}

	const bool bPreparedValueBefore = Asset->BooleanValue;
	const bool bPreparedDirtyBefore = Package->IsDirty();
	FAssetDocumentEditorPreparedPropertyWrite Prepared;
	FAssetDocumentEditorPropertyEditResult PrepareFailure;
	if (!TestTrue(TEXT("provider-private prepare accepts an admitted value"),
		TryPrepareAssetDocumentEditorAdmittedPropertyWrite(
			*Asset,
			BooleanToken,
			MakeBooleanValue(true),
			Prepared,
			PrepareFailure)))
	{
		return false;
	}
	TestEqual(TEXT("prepare does not mutate the admitted value"),
		Asset->BooleanValue, bPreparedValueBefore);
	TestEqual(TEXT("prepare does not dirty the target package"),
		Package->IsDirty(), bPreparedDirtyBefore);
	TestEqual(TEXT("prepare does not enlist a transaction"),
		Transaction.GetEnlistCallCount(), 0);
	TestEqual(TEXT("prepared write owns the exact footprint"),
		Prepared.GetFootprint().Num(), 2);

	const FAssetDocumentEditorPropertyEditResult PreparedApply =
		ApplyAssetDocumentEditorPreparedPropertyWriteWithoutEnlist(
			*Asset,
			BooleanToken,
			MoveTemp(Prepared),
			TEXT("editor.property.write_admitted_batch.v1"));
	if (!TestTrue(TEXT("prepared write applies without enlist"),
		PreparedApply.bSucceeded))
	{
		return false;
	}
	TestTrue(TEXT("prepared apply mutates the admitted property"),
		Asset->BooleanValue);
	TestEqual(TEXT("prepared apply still does not enlist"),
		Transaction.GetEnlistCallCount(), 0);
	if (!TestEqual(TEXT("prepared apply returns one changed record"),
		PreparedApply.ChangedSet.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("prepared apply uses the caller authority"),
		PreparedApply.ChangedSet[0].AuthorityId,
		FString(TEXT("editor.property.write_admitted_batch.v1")));
	const FAssetDocumentEditorPropertyEditResult ConsumedApply =
		ApplyAssetDocumentEditorPreparedPropertyWriteWithoutEnlist(
			*Asset,
			BooleanToken,
			MoveTemp(Prepared),
			TEXT("editor.property.write_admitted_batch.v1"));
	TestFalse(TEXT("prepared write cannot be applied twice"),
		ConsumedApply.bSucceeded);
	TestTrue(TEXT("second prepared apply fails closed"),
		HasEditDiagnostic(
			ConsumedApply, TEXT("PropertyWrite.PreparedAlreadyConsumed")));

	Asset->BooleanValue = false;
	Package->SetDirtyFlag(bPreparedDirtyBefore);
	FAssetDocumentEditorPreparedPropertyWrite MismatchPrepared;
	FAssetDocumentEditorPropertyEditResult MismatchPrepareFailure;
	if (!TestTrue(TEXT("mismatch fixture prepares"),
		TryPrepareAssetDocumentEditorAdmittedPropertyWrite(
			*Asset,
			BooleanToken,
			MakeBooleanValue(true),
			MismatchPrepared,
			MismatchPrepareFailure)))
	{
		return false;
	}
	const int64 MismatchIntegerBefore = Asset->IntegerValue;
	const FAssetDocumentEditorPropertyEditResult MismatchApply =
		ApplyAssetDocumentEditorPreparedPropertyWriteWithoutEnlist(
			*Asset,
			IntegerToken,
			MoveTemp(MismatchPrepared),
			TEXT("editor.property.write_admitted_batch.v1"));
	TestFalse(TEXT("prepared/token exact-property mismatch fails"),
		MismatchApply.bSucceeded);
	TestTrue(TEXT("prepared/token mismatch has a stable diagnostic"),
		HasEditDiagnostic(
			MismatchApply, TEXT("PropertyWrite.PreparedTokenMismatch")));
	TestFalse(TEXT("prepared/token mismatch cannot mutate source"),
		Asset->BooleanValue);
	TestEqual(TEXT("prepared/token mismatch cannot mutate alternate field"),
		Asset->IntegerValue, MismatchIntegerBefore);
	TestEqual(TEXT("prepared/token mismatch does not enlist"),
		Transaction.GetEnlistCallCount(), 0);

	const FAssetDocumentPrimitiveOutcome FirstScalar =
		InvokeScalarWrite(
			*Provider,
			*ScalarWriteDescriptor,
			Context,
			Handles,
			MutableAsset.GetValue(),
			ScalarHandles->Handles[BooleanIndex],
			MakeBooleanValue(true),
			Transaction);
	if (!TestEqual(TEXT("first admitted scalar write succeeds"),
		FirstScalar.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return false;
	}
	TestTrue(TEXT("first scalar write mutates its admitted property"),
		Asset->BooleanValue);
	TestEqual(TEXT("first scalar write enlists once"),
		Transaction.GetEnlistCallCount(), 1);

	const int64 IntegerBefore = Asset->IntegerValue;
	const FAssetDocumentPrimitiveOutcome SecondScalar =
		InvokeScalarWrite(
			*Provider,
			*ScalarWriteDescriptor,
			Context,
			Handles,
			MutableAsset.GetValue(),
			ScalarHandles->Handles[IntegerIndex],
			MakeIntegerValue(23),
			Transaction);
	TestEqual(TEXT("second scalar write fails on duplicate footprint"),
		SecondScalar.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("second scalar write reports the real atomic blocker"),
		HasDiagnostic(
			SecondScalar, TEXT("Atomic.FootprintAlreadyEnlisted")));
	TestEqual(TEXT("duplicate footprint fails before second mutation"),
		Asset->IntegerValue, IntegerBefore);
	TestEqual(TEXT("repeated scalar path attempts a second enlist"),
		Transaction.GetEnlistCallCount(), 2);
	TestNoPublication(*this, SecondScalar, TEXT("duplicate scalar write"));

	return TestBatchWriteGreenMatrix(
		*this,
		*Provider,
		*BatchWriteDescriptor,
		*ScalarWriteDescriptor,
		*EnumerateDescriptor,
		Context,
		Handles,
		*Asset,
		MutableAsset.GetValue(),
		BatchHandles->Handles[0],
		ScalarHandles->Handles[BooleanIndex],
		*BooleanToken.Property,
		*IntegerToken.Property);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderPropertyPreimageTest,
	"UEAssetDoc.GenericProvider.Property.Preimage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderPropertyPreimageTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorPropertyBatchWriteTests;

	FScopedEditableFixtureProperties EditableFixture;
	FScopedPreimageTestAsset PreparationFixture;
	if (!TestPropertyOnlyPreparationShape(
		*this, PreparationFixture.GetAsset()))
	{
		return false;
	}

	FAssetDocumentEditorPropertyPreimage DefaultPreimage;
	const FAssetDocumentContractValidationResult DefaultRestore =
		DefaultPreimage.Restore();
	TestFalse(TEXT("default preimage restore fails closed"),
		DefaultRestore.bValid);
	TestTrue(TEXT("default preimage restore reports invalid state"),
		HasContractDiagnostic(
			DefaultRestore, TEXT("PropertyPreimage.InvalidState")));
	bool bPassed = RunPreimageSuccessCase(*this, false);
	bPassed &= RunPreimageSuccessCase(*this, true);
	bPassed &= RunPreimageIdentityDriftCase(*this);
	bPassed &= RunPreimageFieldContractDriftCase(*this);
	bPassed &= RunPreimageLaterAccessDriftCase(*this);
	bPassed &= RunPreimageInvalidTargetCase(*this);
	return bPassed;
}
#endif
