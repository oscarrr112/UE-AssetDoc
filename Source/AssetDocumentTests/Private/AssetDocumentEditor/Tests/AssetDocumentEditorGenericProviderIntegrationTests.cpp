// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentEditorPrimitiveProvider.h"

#include "AssetDocumentEditorGenericProviderTestTypes.h"
#include "AssetDocumentEditorProviderTestSupport.h"
#include "Engine/PrimaryAssetLabel.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include <type_traits>

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorGenericProviderIntegrationTests
{
using namespace UE::AssetDocument::EditorPrimitive;

template <typename Type, typename = void>
struct THasCloseSurface : std::false_type {};

template <typename Type>
struct THasCloseSurface<Type, std::void_t<decltype(&Type::Close)>>
	: std::true_type {};

template <typename Type, typename = void>
struct THasInvalidationSurface : std::false_type {};

template <typename Type>
struct THasInvalidationSurface<Type, std::void_t<decltype(&Type::Invalidate)>>
	: std::true_type {};

static_assert(!THasCloseSurface<IAssetDocumentPrimitiveProvider>::value);
static_assert(!THasInvalidationSurface<IAssetDocumentPrimitiveProvider>::value);

FAssetDocumentRuntimeValueDto MakeString(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeBoolean(bool bValue)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Result.BooleanValue = bValue;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeInteger(int64 Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Integer;
	Result.IntegerValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeObject(
	TArray<FAssetDocumentEditorValueField> Fields)
{
	FAssetDocumentRuntimeValueDto Result;
	FString DiagnosticCode;
	verify(TryBuildAssetDocumentEditorValueObject(
		Fields, Result, DiagnosticCode));
	return Result;
}

FAssetDocumentRuntimeValueDto MakeArray(
	TArray<FAssetDocumentRuntimeValueDto> Elements)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Array;
	Result.Elements = MoveTemp(Elements);
	return Result;
}

FAssetDocumentRuntimeValueDto MakePropertyBatchWriteArguments()
{
	return MakeObject({
		{TEXT("properties"), MakeArray({
			MakeObject({
				{TEXT("name"), MakeString(TEXT("BooleanValue"))},
				{TEXT("value"), MakeBoolean(false)},
			}),
			MakeObject({
				{TEXT("name"), MakeString(TEXT("IntegerValue"))},
				{TEXT("value"), MakeInteger(41)},
			}),
		})},
	});
}

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

const FAssetDocumentOpaqueHandle* FindSingleHandle(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	FStringView Name)
{
	for (const FAssetDocumentPrimitiveHandleResult& Result :
		Outcome.HandleResults)
	{
		if (Result.Name == Name && Result.Handles.Num() == 1)
		{
			return &Result.Handles[0];
		}
	}
	return nullptr;
}

const FAssetDocumentRuntimeValueDto* FindField(
	const FAssetDocumentRuntimeValueDto& Object,
	FStringView Name)
{
	const int32 Index = Object.ObjectKeys.Find(FString(Name));
	return Object.Elements.IsValidIndex(Index) ? &Object.Elements[Index] : nullptr;
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

bool IsTrueBoolean(const FAssetDocumentRuntimeValueDto* Value)
{
	return Value != nullptr
		&& Value->Kind == EAssetDocumentRuntimeValueKind::Boolean
		&& Value->BooleanValue;
}

bool IsFalseBoolean(const FAssetDocumentRuntimeValueDto* Value)
{
	return Value != nullptr
		&& Value->Kind == EAssetDocumentRuntimeValueKind::Boolean
		&& !Value->BooleanValue;
}

class FObjectPayload final : public INativeIdentityPayload
{
public:
	explicit FObjectPayload(UObject& InObject)
		: Object(&InObject)
		, CanonicalPath(InObject.GetPathName())
	{
	}

	virtual FName GetPayloadTypeId() const override
	{
		return AssetPayloadTypeId;
	}

	virtual bool IsAlive() const override
	{
		return Object.IsValid();
	}

	virtual bool TryGetObjectIdentity(FObjectIdentityView& OutView)
		const override
	{
		OutView.Object = Object.Get();
		OutView.CanonicalObjectPath = CanonicalPath;
		OutView.TypeRevision = OutView.Object != nullptr
			? static_cast<uint64>(OutView.Object->GetUniqueID()) : 0;
		return OutView.Object != nullptr;
	}

private:
	TWeakObjectPtr<UObject> Object;
	FString CanonicalPath;
};

class FAlivePayload final : public IAssetDocumentNativeHandlePayload
{
public:
	virtual FName GetPayloadTypeId() const override
	{
		return FName(TEXT("assetdocument.test.integration.alive.v1"));
	}

	virtual bool IsAlive() const override
	{
		return true;
	}
};

class FIntegrationTransaction final : public IAssetDocumentAtomicTransaction
{
public:
	explicit FIntegrationTransaction(
		const FAssetDocumentEditorProviderTestContext& Context)
		: ReportedId(Context.TransactionId)
	{
		Token.RequestId = Context.RequestId;
		Token.TransactionId = Context.TransactionId;
		Token.RuntimeRevision = Context.RuntimeRevision;
		RollbackActionEnlistmentResult.bValid = true;
		PostRollbackFinalizerEnlistmentResult.bValid = true;
	}

	void BreakReportedId()
	{
		ReportedId = FGuid::NewGuid();
	}

	void BreakMutationToken()
	{
		++Token.RuntimeRevision;
	}

	int32 GetEnlistCount() const
	{
		return EnlistCount;
	}

	const TArray<FAssetDocumentMutationFootprint>& GetEnlistedFootprint() const
	{
		return EnlistedFootprint;
	}

	virtual FGuid GetTransactionId() const override
	{
		return ReportedId;
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
		++EnlistCount;
		for (const FAssetDocumentMutationFootprint& Item : Footprint)
		{
			EnlistedFootprint.Add(Item);
		}
		State = EAssetDocumentAtomicState::Mutating;
		return MakeValid();
	}

	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override
	{
		State = EAssetDocumentAtomicState::ReadyToCommit;
		return MakeValid();
	}

	virtual FAssetDocumentContractValidationResult Commit(
		const FAssetDocumentDurableCompletionReceipt&) override
	{
		State = EAssetDocumentAtomicState::Committed;
		return MakeValid();
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView) override
	{
		State = EAssetDocumentAtomicState::RolledBack;
		return MakeValid();
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

	TArray<FString> EnlistedRollbackActionComponentIds;
	FAssetDocumentContractValidationResult RollbackActionEnlistmentResult;
	FAssetDocumentContractValidationResult PostRollbackFinalizerEnlistmentResult;

private:
	static FAssetDocumentContractValidationResult MakeValid()
	{
		FAssetDocumentContractValidationResult Result;
		Result.bValid = true;
		return Result;
	}

	FGuid ReportedId;
	FAssetDocumentMutationToken Token;
	EAssetDocumentAtomicState State = EAssetDocumentAtomicState::Live;
	TArray<FAssetDocumentMutationFootprint> EnlistedFootprint;
	int32 EnlistCount = 0;
};

class FDiagnosticResolver final : public IAssetDocumentHandleResolver
{
public:
	explicit FDiagnosticResolver(FString InCode)
		: Code(MoveTemp(InCode))
	{
	}

	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind,
		EAssetDocumentHandleAccess,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&&) override
	{
		return {};
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle&,
		const FAssetDocumentHandleValidationContext&) override
	{
		FAssetDocumentVerifiedLeaseResult Result;
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/Handle");
		Diagnostic.Code = Code;
		Diagnostic.Message = TEXT("Injected Host resolver rejection.");
		return Result;
	}

	virtual FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation&) override
	{
		return {};
	}

private:
	FString Code;
};

class FScopedEditableProperties final
{
public:
	FScopedEditableProperties()
	{
		for (TFieldIterator<FProperty> It(
			UAssetDocumentEditorCodecTestObject::StaticClass()); It; ++It)
		{
			if (!It->HasAnyPropertyFlags(CPF_Edit))
			{
				Changed.Add(*It);
				It->SetPropertyFlags(CPF_Edit);
			}
		}
	}

	~FScopedEditableProperties()
	{
		for (FProperty* Property : Changed)
		{
			Property->ClearPropertyFlags(CPF_Edit);
		}
	}

private:
	TArray<FProperty*> Changed;
};

struct FIntegrationScope
{
	FIntegrationScope()
		: Handles(Context)
		, Provider(CreateAssetDocumentEditorPrimitiveProvider())
	{
	}

	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles;
	TSharedRef<IAssetDocumentPrimitiveProvider> Provider;
	TOptional<FAssetDocumentOpaqueHandle> BooleanProperty;
};

FAssetDocumentPrimitiveOutcome Invoke(
	FIntegrationScope& Scope,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	FAssetDocumentRuntimeValueDto Value,
	TArray<FAssetDocumentPrimitiveHandleArgument> Handles,
	IAssetDocumentAtomicTransaction* Transaction)
{
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(
			Descriptor, Scope.Context);
	Invocation.ValueArguments = MoveTemp(Value);
	Invocation.HandleArguments = MoveTemp(Handles);
	return Scope.Provider->Invoke(Invocation, Scope.Handles, Transaction);
}

bool ReadClassIdentity(
	FAutomationTestBase& Test,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentHandleValidationContext& LeaseContext,
	UClass& ExpectedClass,
	FString& OutCanonicalPath)
{
	FAssetDocumentVerifiedLeaseResult Lease =
		Handles.AcquireVerifiedLease(Handle, LeaseContext);
	if (!Test.TestTrue(TEXT("class handle leases"), Lease.Lease.IsValid()))
	{
		return false;
	}
	INativeIdentityPayload* Payload = TryGetNativeIdentityPayload(
		Lease.Lease->GetPayloadChecked(), ClassPayloadTypeId);
	if (!Test.TestNotNull(TEXT("class payload is exact"), Payload))
	{
		return false;
	}
	FClassIdentityView Identity;
	if (!Test.TestTrue(TEXT("class identity resolves"),
		Payload->TryGetClassIdentity(Identity)))
	{
		return false;
	}
	Test.TestTrue(TEXT("class identity is exact"),
		Identity.Class == &ExpectedClass);
	OutCanonicalPath = FString(Identity.CanonicalClassPath);
	return Identity.Class == &ExpectedClass;
}

TOptional<FAssetDocumentOpaqueHandle> ResolveClass(
	FAutomationTestBase& Test,
	FIntegrationScope& Scope,
	UClass& Class,
	FString& OutCanonicalPath)
{
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(
		*Scope.Provider, TEXT("editor.class.resolve_canonical.v1"));
	if (!Test.TestNotNull(TEXT("class resolve descriptor"), Descriptor))
	{
		return {};
	}
	const FAssetDocumentPrimitiveOutcome Outcome = Invoke(
		Scope, *Descriptor,
		MakeObject({
			{TEXT("class"), MakeString(Class.GetPathName())},
			{TEXT("expected_base"), MakeString(UObject::StaticClass()->GetPathName())},
		}),
		{},
		nullptr);
	const FAssetDocumentOpaqueHandle* Handle =
		FindSingleHandle(Outcome, TEXT("class"));
	if (!Test.TestEqual(TEXT("class resolution succeeds"), Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return {};
	}
	if (!Test.TestNotNull(TEXT("class handle exists"), Handle))
	{
		return {};
	}
	FAssetDocumentHandleValidationContext LeaseContext;
	LeaseContext.RequestId = Scope.Context.RequestId;
	LeaseContext.TransactionId = Scope.Context.TransactionId;
	LeaseContext.RuntimeRevision = Scope.Context.RuntimeRevision;
	LeaseContext.ProviderId = Scope.Context.ProviderId;
	LeaseContext.Phase = EAssetDocumentRuntimeRequestPhase::Validate;
	LeaseContext.RequiredKind = EAssetDocumentHandleKind::Class;
	LeaseContext.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
	if (!ReadClassIdentity(
		Test, Scope.Handles, *Handle, LeaseContext, Class, OutCanonicalPath))
	{
		return {};
	}
	return *Handle;
}

bool CaptureBooleanPropertyToken(
	FAutomationTestBase& Test,
	const FAssetDocumentRuntimeValueDto& Rows,
	const FAssetDocumentPrimitiveHandleResult& Tokens,
	FIntegrationScope& Scope)
{
	Test.TestEqual(TEXT("property rows and tokens stay parallel"),
		Rows.Elements.Num(), Tokens.Handles.Num());
	if (Rows.Elements.Num() != Tokens.Handles.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Rows.Elements.Num(); ++Index)
	{
		const FAssetDocumentRuntimeValueDto* Name =
			FindField(Rows.Elements[Index], TEXT("name"));
		if (Name != nullptr && Name->StringValue == TEXT("BooleanValue"))
		{
			Scope.BooleanProperty = Tokens.Handles[Index];
		}
	}
	return Test.TestTrue(TEXT("enumeration supplies BooleanValue token"),
		Scope.BooleanProperty.IsSet());
}

bool TestEnumerationOutcome(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	FIntegrationScope& Scope)
{
	if (!Test.TestEqual(TEXT("same-request enumeration succeeds"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto* Rows =
		FindField(Outcome.Value, TEXT("properties"));
	if (!Test.TestNotNull(TEXT("enumeration returns rows"), Rows))
	{
		return false;
	}
	const FAssetDocumentPrimitiveHandleResult* Tokens =
		Outcome.HandleResults.FindByPredicate([](const auto& Result)
		{
			return Result.Name == TEXT("properties");
		});
	if (!Test.TestNotNull(TEXT("enumeration returns tokens"), Tokens))
	{
		return false;
	}
	return CaptureBooleanPropertyToken(Test, *Rows, *Tokens, Scope);
}

bool TestClassToPropertyEnumeration(
	FAutomationTestBase& Test,
	FIntegrationScope& Scope)
{
	FString ClassPath;
	if (!ResolveClass(
		Test, Scope, *UAssetDocumentEditorCodecTestObject::StaticClass(),
		ClassPath).IsSet())
	{
		return false;
	}
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(
		*Scope.Provider, TEXT("editor.property.enumerate_admitted.v1"));
	if (!Test.TestNotNull(TEXT("property enumerate descriptor"), Descriptor))
	{
		return false;
	}
	const FAssetDocumentPrimitiveOutcome Outcome = Invoke(
		Scope, *Descriptor,
		MakeObject({{TEXT("exact_asset_class_path"), MakeString(ClassPath)}}),
		{},
		nullptr);
	return TestEnumerationOutcome(Test, Outcome, Scope);
}

FAssetDocumentPrimitiveOutcome InvokeProperty(
	FIntegrationScope& Scope,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentOpaqueHandle& Asset,
	bool bWrite,
	IAssetDocumentAtomicTransaction* Transaction)
{
	return Invoke(
		Scope, Descriptor,
		bWrite
			? MakeObject({{TEXT("value"), MakeBoolean(true)}})
			: MakeObject({}),
		{
			{TEXT("asset"), Asset},
			{TEXT("property"), Scope.BooleanProperty.GetValue()},
		},
		Transaction);
}

bool TestRejectedTransaction(
	FAutomationTestBase& Test,
	FIntegrationScope& Scope,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentOpaqueHandle& Asset,
	UAssetDocumentEditorCodecTestObject& Object,
	FIntegrationTransaction* Transaction)
{
	const FAssetDocumentPrimitiveOutcome Outcome = InvokeProperty(
		Scope, Descriptor, Asset, true, Transaction);
	Test.TestEqual(TEXT("invalid transaction fails before handler"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestFalse(TEXT("invalid transaction cannot mutate"), Object.BooleanValue);
	if (Transaction == nullptr)
	{
		return true;
	}
	return Test.TestEqual(TEXT("invalid transaction enlists nothing"),
		Transaction->GetEnlistCount(), 0);
}

bool TestPropertyPrerequisites(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor* Read,
	const FAssetDocumentPrimitiveDescriptor* Write,
	const FAssetDocumentIssuedHandleResult& ReadAsset,
	const FAssetDocumentIssuedHandleResult& MutableAsset)
{
	bool bPassed = Test.TestNotNull(TEXT("read descriptor"), Read);
	bPassed &= Test.TestNotNull(TEXT("write descriptor"), Write);
	bPassed &= Test.TestTrue(TEXT("read asset handle"),
		ReadAsset.Handle.IsSet());
	bPassed &= Test.TestTrue(TEXT("mutable asset handle"),
		MutableAsset.Handle.IsSet());
	return bPassed;
}

bool TestTransactionGuards(
	FAutomationTestBase& Test,
	FIntegrationScope& Scope,
	const FAssetDocumentPrimitiveDescriptor& Write,
	const FAssetDocumentOpaqueHandle& Asset,
	UAssetDocumentEditorCodecTestObject& Object)
{
	FIntegrationTransaction WrongId(Scope.Context);
	WrongId.BreakReportedId();
	FIntegrationTransaction WrongToken(Scope.Context);
	WrongToken.BreakMutationToken();
	bool bPassed = TestRejectedTransaction(
		Test, Scope, Write, Asset, Object, nullptr);
	bPassed &= TestRejectedTransaction(
		Test, Scope, Write, Asset, Object, &WrongId);
	bPassed &= TestRejectedTransaction(
		Test, Scope, Write, Asset, Object, &WrongToken);
	return bPassed;
}

bool TestPropertyBatchReadWrite(
	FAutomationTestBase& Test,
	FIntegrationScope& Scope,
	UAssetDocumentEditorCodecTestObject& Object,
	const FAssetDocumentOpaqueHandle& ReadAsset,
	const FAssetDocumentOpaqueHandle& MutableAsset)
{
	const FAssetDocumentPrimitiveDescriptor* EnumerateBatch = FindDescriptor(
		*Scope.Provider, TEXT("editor.property.enumerate_admitted_batch.v1"));
	const FAssetDocumentPrimitiveDescriptor* WriteBatch = FindDescriptor(
		*Scope.Provider, TEXT("editor.property.write_admitted_batch.v1"));
	if (!Test.TestNotNull(TEXT("batch enumerate descriptor"), EnumerateBatch)
		|| !Test.TestNotNull(TEXT("batch write descriptor"), WriteBatch))
	{
		return false;
	}

	const FAssetDocumentPrimitiveOutcome Admission = Invoke(
		Scope,
		*EnumerateBatch,
		MakeObject({}),
		{{TEXT("asset"), ReadAsset}},
		nullptr);
	if (!Test.TestEqual(TEXT("real producer admits a property composite"),
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
	if (!Test.TestNotNull(TEXT("batch producer returns property rows"), Rows)
		|| !Test.TestNotNull(
			TEXT("batch producer returns scalar property handles"), ScalarHandles)
		|| !Test.TestNotNull(
			TEXT("batch producer returns a composite handle"), BatchHandles))
	{
		return false;
	}
	if (!Test.TestEqual(TEXT("batch producer rows are an array"),
			Rows->Kind, EAssetDocumentRuntimeValueKind::Array)
		|| !Test.TestEqual(TEXT("batch producer rows and handles stay parallel"),
			Rows->Elements.Num(), ScalarHandles->Handles.Num())
		|| !Test.TestEqual(TEXT("batch producer returns exactly one composite"),
			BatchHandles->Handles.Num(), 1))
	{
		return false;
	}
	const int32 BooleanIndex =
		FindPropertyRowIndex(*Rows, TEXT("BooleanValue"));
	const int32 IntegerIndex =
		FindPropertyRowIndex(*Rows, TEXT("IntegerValue"));
	if (!Test.TestTrue(TEXT("batch producer admits two fixture properties"),
			BooleanIndex != INDEX_NONE && IntegerIndex != INDEX_NONE)
		|| !Test.TestTrue(TEXT("batch producer preserves property token order"),
			BooleanIndex < IntegerIndex))
	{
		return false;
	}

	Object.BooleanValue = true;
	Object.IntegerValue = 0;
	FIntegrationTransaction BatchTransaction(Scope.Context);
	const FAssetDocumentPrimitiveOutcome Written = Invoke(
		Scope,
		*WriteBatch,
		MakePropertyBatchWriteArguments(),
		{
			{TEXT("asset"), MutableAsset},
			{TEXT("property_batch"), BatchHandles->Handles[0]},
		},
		&BatchTransaction);
	bool bPassed = Test.TestEqual(TEXT("two-property batch succeeds"),
		Written.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	bPassed &= Test.TestFalse(TEXT("batch writes BooleanValue"),
		Object.BooleanValue);
	bPassed &= Test.TestEqual(TEXT("batch writes IntegerValue"),
		Object.IntegerValue, 41ll);
	bPassed &= Test.TestEqual(TEXT("batch transaction enlists once"),
		BatchTransaction.GetEnlistCount(), 1);
	bPassed &= Test.TestEqual(TEXT("batch transaction enlists exact footprint"),
		BatchTransaction.GetEnlistedFootprint().Num(), 2);
	bPassed &= Test.TestTrue(TEXT("batch publishes an empty success object"),
		Written.Value.Kind == EAssetDocumentRuntimeValueKind::Object
			&& Written.Value.ObjectKeys.IsEmpty()
			&& Written.Value.Elements.IsEmpty());
	bPassed &= Test.TestTrue(TEXT("batch publishes no handles"),
		Written.HandleResults.IsEmpty());
	bPassed &= Test.TestEqual(TEXT("batch publishes two changed records"),
		Written.ChangedSet.Num(), 2);
	if (Written.ChangedSet.Num() == 2)
	{
		bPassed &= Test.TestEqual(TEXT("BooleanValue change is first"),
			Written.ChangedSet[0].Path, FString(TEXT("/BooleanValue")));
		bPassed &= Test.TestEqual(TEXT("IntegerValue change is second"),
			Written.ChangedSet[1].Path, FString(TEXT("/IntegerValue")));
		bPassed &= Test.TestEqual(TEXT("BooleanValue uses batch authority"),
			Written.ChangedSet[0].AuthorityId,
			FString(TEXT("editor.property.write_admitted_batch.v1")));
		bPassed &= Test.TestEqual(TEXT("IntegerValue uses batch authority"),
			Written.ChangedSet[1].AuthorityId,
			FString(TEXT("editor.property.write_admitted_batch.v1")));
	}
	return bPassed;
}

bool TestPropertyReadWriteAndTransactions(
	FAutomationTestBase& Test,
	FIntegrationScope& Scope)
{
	if (!Scope.BooleanProperty.IsSet())
	{
		return Test.TestTrue(TEXT("property token prerequisite"), false);
	}
	TStrongObjectPtr<UAssetDocumentEditorCodecTestObject> Object(
		NewObject<UAssetDocumentEditorCodecTestObject>(
			GetTransientPackage(), NAME_None, RF_Transactional));
	const bool bDirtyBefore = Object->GetOutermost()->IsDirty();
	ON_SCOPE_EXIT
	{
		Object->BooleanValue = false;
		Object->IntegerValue = 0;
		Object->GetOutermost()->SetDirtyFlag(bDirtyBefore);
	};
	const FAssetDocumentPrimitiveDescriptor* Read = FindDescriptor(
		*Scope.Provider, TEXT("editor.property.read_admitted.v1"));
	const FAssetDocumentPrimitiveDescriptor* Write = FindDescriptor(
		*Scope.Provider, TEXT("editor.property.write_admitted.v1"));
	const FAssetDocumentIssuedHandleResult ReadAsset = Scope.Handles.Issue(
		EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly,
		MakeUnique<FObjectPayload>(*Object));
	const FAssetDocumentIssuedHandleResult MutableAsset = Scope.Handles.Issue(
		EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable,
		MakeUnique<FObjectPayload>(*Object));
	if (!TestPropertyPrerequisites(
		Test, Read, Write, ReadAsset, MutableAsset))
	{
		return false;
	}
	Test.TestEqual(TEXT("exact read handle succeeds"),
		InvokeProperty(
			Scope, *Read, ReadAsset.Handle.GetValue(), false, nullptr).Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	const bool bGuards = TestTransactionGuards(
		Test, Scope, *Write, MutableAsset.Handle.GetValue(), *Object);
	FIntegrationTransaction Matching(Scope.Context);
	const FAssetDocumentPrimitiveOutcome Written = InvokeProperty(
		Scope, *Write, MutableAsset.Handle.GetValue(), true, &Matching);
	bool bPassed = bGuards;
	bPassed &= Test.TestEqual(TEXT("exact mutable write succeeds"),
		Written.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	bPassed &= Test.TestTrue(TEXT("exact mutable write changes value"),
		Object->BooleanValue);
	bPassed &= Test.TestEqual(TEXT("exact mutable write enlists once"),
		Matching.GetEnlistCount(), 1);
	bPassed &= TestPropertyBatchReadWrite(
		Test,
		Scope,
		*Object,
		ReadAsset.Handle.GetValue(),
		MutableAsset.Handle.GetValue());
	return bPassed;
}

bool TestStructSchemaValidation(
	FAutomationTestBase& Test,
	FIntegrationScope& Scope)
{
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(
		*Scope.Provider, TEXT("editor.fragment.struct_codec.v1"));
	if (!Test.TestNotNull(TEXT("struct descriptor"), Descriptor))
	{
		return false;
	}
	const FAssetDocumentPrimitiveOutcome Resolved = Invoke(
		Scope, *Descriptor,
		MakeObject({
			{TEXT("operation"), MakeString(TEXT("resolve_schema"))},
			{TEXT("struct"), MakeString(
				FAssetDocumentEditorCodecNestedValue::StaticStruct()->GetPathName())},
		}),
		{},
		nullptr);
	const FAssetDocumentOpaqueHandle* Schema =
		FindSingleHandle(Resolved, TEXT("schema"));
	if (!Test.TestNotNull(TEXT("struct schema handle"), Schema))
	{
		return false;
	}
	FAssetDocumentPrimitiveInvocation Validate =
		MakeAssetDocumentEditorProviderTestInvocation(*Descriptor, Scope.Context);
	Validate.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
	Validate.ValueArguments = MakeObject({
		{TEXT("operation"), MakeString(TEXT("validate"))},
		{TEXT("value"), MakeObject({
			{TEXT("Count"), MakeInteger(17)},
			{TEXT("Label"), MakeString(TEXT("integration"))},
		})},
	});
	Validate.HandleArguments = {{TEXT("schema"), *Schema}};
	const FAssetDocumentPrimitiveOutcome Outcome =
		Scope.Provider->Invoke(Validate, Scope.Handles, nullptr);
	return Test.TestEqual(TEXT("schema-bound validation succeeds"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
}

struct FLifecycleTarget
{
	const FAssetDocumentPrimitiveDescriptor* Create = nullptr;
	const FAssetDocumentPrimitiveDescriptor* Rollback = nullptr;
	FAssetDocumentOpaqueHandle Class;
	FAssetDocumentOpaqueHandle Asset;
	FAssetDocumentOpaqueHandle Package;
	FString PackagePath;
	FString Name;
	FString CanonicalPath;
};

bool PrepareLifecycleTarget(
	FAutomationTestBase& Test,
	FIntegrationScope& Scope,
	FLifecycleTarget& OutTarget)
{
	FString ClassPath;
	const TOptional<FAssetDocumentOpaqueHandle> Class = ResolveClass(
		Test, Scope, *UPrimaryAssetLabel::StaticClass(), ClassPath);
	if (!Class.IsSet())
	{
		return false;
	}
	OutTarget.Create = FindDescriptor(
		*Scope.Provider, TEXT("editor.asset.create_by_factory.v1"));
	OutTarget.Rollback = FindDescriptor(
		*Scope.Provider, TEXT("editor.asset.rollback_created.v1"));
	bool bDescriptors = Test.TestNotNull(
		TEXT("factory descriptor"), OutTarget.Create);
	bDescriptors &= Test.TestNotNull(
		TEXT("rollback descriptor"), OutTarget.Rollback);
	if (!bDescriptors)
	{
		return false;
	}
	OutTarget.Class = Class.GetValue();
	OutTarget.PackagePath =
		TEXT("/Game/AssetDocumentTests/GenericProvider/Integration");
	OutTarget.Name = FString::Printf(
		TEXT("DA_Integration_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	OutTarget.CanonicalPath = FString::Printf(
		TEXT("%s/%s.%s"),
		*OutTarget.PackagePath, *OutTarget.Name, *OutTarget.Name);
	return true;
}

bool CreateLifecycleTarget(
	FAutomationTestBase& Test,
	FIntegrationScope& Scope,
	FIntegrationTransaction& Transaction,
	FLifecycleTarget& Target)
{
	const FAssetDocumentPrimitiveOutcome Created = Invoke(
		Scope, *Target.Create,
		MakeObject({
			{TEXT("asset_name"), MakeString(Target.Name)},
			{TEXT("factory_id"), MakeString(TEXT("data_asset_factory.v1"))},
			{TEXT("package_path"), MakeString(Target.PackagePath)},
		}),
		{{TEXT("asset_class"), Target.Class}}, &Transaction);
	if (!Test.TestEqual(TEXT("factory create succeeds"), Created.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return false;
	}
	const FAssetDocumentOpaqueHandle* Asset =
		FindSingleHandle(Created, TEXT("asset_mutable"));
	if (!Test.TestNotNull(TEXT("created mutable asset"), Asset))
	{
		return false;
	}
	const FAssetDocumentOpaqueHandle* Package =
		FindSingleHandle(Created, TEXT("package"));
	if (!Test.TestNotNull(TEXT("created mutable package"), Package))
	{
		return false;
	}
	Target.Asset = *Asset;
	Target.Package = *Package;
	return true;
}

void CleanupLifecycleTarget(const FLifecycleTarget& Target)
{
	if (UObject* Residue =
		FSoftObjectPath(Target.CanonicalPath).ResolveObject())
	{
		ObjectTools::DeleteObjects(
			{Residue}, false,
			ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
	}
	const FString PackageName = FString::Printf(
		TEXT("%s/%s"), *Target.PackagePath, *Target.Name);
	if (UPackage* Package = FindPackage(nullptr, *PackageName))
	{
		Package->SetDirtyFlag(false);
		UPackageTools::UnloadPackages({Package});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	IFileManager::Get().Delete(*Filename, false, true);
}

bool TestCreationRecordRollback(
	FAutomationTestBase& Test,
	FIntegrationScope& Scope)
{
	FLifecycleTarget Target;
	if (!PrepareLifecycleTarget(Test, Scope, Target))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		CleanupLifecycleTarget(Target);
	};
	FIntegrationTransaction Transaction(Scope.Context);
	if (!CreateLifecycleTarget(Test, Scope, Transaction, Target))
	{
		return false;
	}
	const FAssetDocumentPrimitiveOutcome Deleted = Invoke(
		Scope, *Target.Rollback, MakeObject({}),
		{{TEXT("asset"), Target.Asset}, {TEXT("package"), Target.Package}},
		&Transaction);
	const FAssetDocumentRuntimeValueDto* ObjectAbsent =
		FindField(Deleted.Value, TEXT("object_absent"));
	const FAssetDocumentRuntimeValueDto* FileAbsent =
		FindField(Deleted.Value, TEXT("file_absent"));
	const FAssetDocumentRuntimeValueDto* PackageAbsent =
		FindField(Deleted.Value, TEXT("package_absent"));
	const FAssetDocumentRuntimeValueDto* RegistryAbsent =
		FindField(Deleted.Value, TEXT("registry_absent"));
	const FAssetDocumentRuntimeValueDto* DirtyResidue =
		FindField(Deleted.Value, TEXT("dirty_residue"));
	bool bPassed = Test.TestEqual(TEXT("creation-record rollback succeeds"),
		Deleted.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	bPassed &= Test.TestTrue(TEXT("rollback reports object absence"),
		IsTrueBoolean(ObjectAbsent));
	bPassed &= Test.TestTrue(TEXT("rollback reports package absence"),
		IsTrueBoolean(PackageAbsent));
	bPassed &= Test.TestTrue(TEXT("rollback reports registry absence"),
		IsTrueBoolean(RegistryAbsent));
	bPassed &= Test.TestTrue(TEXT("rollback reports file absence"),
		IsTrueBoolean(FileAbsent));
	bPassed &= Test.TestTrue(TEXT("rollback reports no dirty residue"),
		IsFalseBoolean(DirtyResidue));
	bPassed &= Test.TestNull(TEXT("rolled-back object is absent"),
		FSoftObjectPath(Target.CanonicalPath).ResolveObject());
	return bPassed;
}

FAssetDocumentOpaqueHandle MakeHandle(
	const FAssetDocumentEditorProviderTestContext& Context,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access,
	uint32 Slot)
{
	FAssetDocumentOpaqueHandle Handle;
	Handle.RequestId = Context.RequestId;
	Handle.TransactionId = Context.TransactionId;
	Handle.RuntimeRevision = Context.RuntimeRevision;
	Handle.ProviderId = Context.ProviderId;
	Handle.Kind = Kind;
	Handle.Access = Access;
	Handle.Slot = Slot;
	Handle.Generation = 1;
	return Handle;
}

bool TestResolverDiagnosticPropagation(FAutomationTestBase& Test)
{
	static const TCHAR* Codes[] = {
		TEXT("HandleMetadata.StaleHandle"),
		TEXT("HandleMetadata.KindMismatch"),
		TEXT("HandleMetadata.AccessMismatch"),
		TEXT("HandleMetadata.RequestMismatch"),
		TEXT("HandleMetadata.TransactionMismatch"),
		TEXT("HandleMetadata.RuntimeRevisionMismatch"),
	};
	const FAssetDocumentEditorProviderTestContext Context;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(
		*Provider, TEXT("editor.property.read_admitted.v1"));
	if (!Test.TestNotNull(TEXT("diagnostic descriptor"), Descriptor))
	{
		return false;
	}
	for (const TCHAR* Code : Codes)
	{
		FDiagnosticResolver Resolver(Code);
		FAssetDocumentPrimitiveInvocation Invocation =
			MakeAssetDocumentEditorProviderTestInvocation(*Descriptor, Context);
		Invocation.HandleArguments = {
			{TEXT("asset"), MakeHandle(
				Context, EAssetDocumentHandleKind::Asset,
				EAssetDocumentHandleAccess::ReadOnly, 1)},
			{TEXT("property"), MakeHandle(
				Context, EAssetDocumentHandleKind::Property,
				EAssetDocumentHandleAccess::ReadOnly, 2)},
		};
		const FAssetDocumentPrimitiveOutcome Outcome =
			Provider->Invoke(Invocation, Resolver, nullptr);
		Test.TestEqual(TEXT("resolver rejection fails"),
			Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
		if (Test.TestEqual(TEXT("resolver returns one diagnostic"),
			Outcome.Diagnostics.Num(), 1))
		{
			Test.TestEqual(TEXT("resolver diagnostic propagates unchanged"),
				Outcome.Diagnostics[0].Code, FString(Code));
		}
	}
	return true;
}

bool TestTerminalPrerequisites(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor* Descriptor,
	const FAssetDocumentIssuedHandleResult& Asset,
	const FAssetDocumentIssuedHandleResult& Property)
{
	bool bPassed = Test.TestNotNull(
		TEXT("terminal descriptor"), Descriptor);
	bPassed &= Test.TestTrue(
		TEXT("terminal asset handle"), Asset.Handle.IsSet());
	bPassed &= Test.TestTrue(
		TEXT("terminal property handle"), Property.Handle.IsSet());
	return bPassed;
}

bool TestTerminalInvalidation(
	FAutomationTestBase& Test,
	EAssetDocumentHandleInvalidationReason Reason)
{
	FIntegrationScope Scope;
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(
		*Scope.Provider, TEXT("editor.property.read_admitted.v1"));
	const FAssetDocumentIssuedHandleResult Asset = Scope.Handles.Issue(
		EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly,
		MakeUnique<FAlivePayload>());
	const FAssetDocumentIssuedHandleResult Property = Scope.Handles.Issue(
		EAssetDocumentHandleKind::Property, EAssetDocumentHandleAccess::ReadOnly,
		MakeUnique<FAlivePayload>());
	if (!TestTerminalPrerequisites(
		Test, Descriptor, Asset, Property))
	{
		return false;
	}
	FAssetDocumentHandleInvalidation Event;
	Event.Reason = Reason;
	Event.RequestId = Scope.Context.RequestId;
	Event.TransactionId = Scope.Context.TransactionId;
	Event.RuntimeRevision = Scope.Context.RuntimeRevision;
	Event.ProviderId = Scope.Context.ProviderId;
	Scope.Handles.Invalidate(Event);
	Scope.BooleanProperty = Property.Handle;
	const FAssetDocumentPrimitiveOutcome Outcome = InvokeProperty(
		Scope, *Descriptor, Asset.Handle.GetValue(), false, nullptr);
	bool bPassed = Test.TestEqual(TEXT("terminal event rejects old handles"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	bPassed &= Test.TestEqual(TEXT("terminal event emits one diagnostic"),
		Outcome.Diagnostics.Num(), 1);
	if (Outcome.Diagnostics.Num() == 1)
	{
		bPassed &= Test.TestEqual(TEXT("terminal event is stale"),
			Outcome.Diagnostics[0].Code,
			FString(TEXT("HandleMetadata.StaleHandle")));
	}
	return bPassed;
}

bool TestNoProviderCloseDescriptorSurface(FAutomationTestBase& Test)
{
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor :
		Provider->DescribePrimitives())
	{
		Test.TestFalse(TEXT("provider exposes no close primitive"),
			Descriptor.PrimitiveId.Contains(TEXT("close")));
		Test.TestFalse(TEXT("provider exposes no invalidation primitive"),
			Descriptor.PrimitiveId.Contains(TEXT("invalidate")));
	}
	return true;
}
} // namespace AssetDocumentEditorGenericProviderIntegrationTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderCrossTrackFlowTest,
	"UEAssetDoc.GenericProvider.Integration.CrossTrackFlow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderCrossTrackFlowTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericProviderIntegrationTests;
	FIntegrationScope Scope;
	FScopedEditableProperties EditableProperties;
	TestEqual(TEXT("integrated provider is available"),
		Scope.Provider->InspectAvailability().State,
		EAssetDocumentProviderAvailabilityState::Available);
	const bool bClassProperty = TestClassToPropertyEnumeration(*this, Scope);
	const bool bProperty = TestPropertyReadWriteAndTransactions(*this, Scope);
	const bool bStruct = TestStructSchemaValidation(*this, Scope);
	const bool bLifecycle = TestCreationRecordRollback(*this, Scope);
	return bClassProperty && bProperty && bStruct && bLifecycle;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderCommitInvalidationTest,
	"UEAssetDoc.GenericProvider.Integration.CommitInvalidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderCommitInvalidationTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericProviderIntegrationTests;
	return TestTerminalInvalidation(
		*this, EAssetDocumentHandleInvalidationReason::Commit)
		&& TestNoProviderCloseDescriptorSurface(*this);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderRollbackInvalidationTest,
	"UEAssetDoc.GenericProvider.Integration.RollbackInvalidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderRollbackInvalidationTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericProviderIntegrationTests;
	return TestTerminalInvalidation(
		*this, EAssetDocumentHandleInvalidationReason::Rollback);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderRuntimeInvalidationsTest,
	"UEAssetDoc.GenericProvider.Integration.RuntimeInvalidations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderRuntimeInvalidationsTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericProviderIntegrationTests;
	const EAssetDocumentHandleInvalidationReason Reasons[] = {
		EAssetDocumentHandleInvalidationReason::GarbageCollection,
		EAssetDocumentHandleInvalidationReason::PackageReload,
		EAssetDocumentHandleInvalidationReason::Reinstance,
		EAssetDocumentHandleInvalidationReason::ProviderUnload,
	};
	bool bPassed = TestResolverDiagnosticPropagation(*this);
	for (const EAssetDocumentHandleInvalidationReason Reason : Reasons)
	{
		bPassed &= TestTerminalInvalidation(*this, Reason);
	}
	return bPassed;
}
#endif
