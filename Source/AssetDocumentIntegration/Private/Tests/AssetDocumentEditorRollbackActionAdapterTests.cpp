// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorRollbackActionAdapter.h"

#include "AssetDocumentEditorPrimitiveProvider.h"
#include "Engine/PrimaryAssetLabel.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorRollbackActionAdapterTests
{
struct FInvocationContext
{
	FGuid RequestId = FGuid::NewGuid();
	FGuid TransactionId = FGuid::NewGuid();
	uint64 RuntimeRevision = 1;
};

FAssetDocumentRuntimeDiagnostic MakeHandleDiagnostic(const TCHAR* Code)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/Handle");
	Diagnostic.Code = Code;
	Diagnostic.Message = TEXT("The cross-layer test resolver rejected the handle.");
	return Diagnostic;
}

class FCrossLayerHandleResolver final : public IAssetDocumentHandleResolver
{
	struct FEntry
	{
		FAssetDocumentOpaqueHandle Handle;
		TUniquePtr<IAssetDocumentNativeHandlePayload> Payload;
	};

	class FLease final : public IAssetDocumentVerifiedHandleLease
	{
	public:
		FLease(
			FAssetDocumentOpaqueHandle InHandle,
			IAssetDocumentNativeHandlePayload& InPayload)
			: Handle(MoveTemp(InHandle))
			, Payload(&InPayload)
		{
		}

		virtual const FAssetDocumentOpaqueHandle& GetHandle() const override
		{
			return Handle;
		}

		virtual IAssetDocumentNativeHandlePayload&
		GetPayloadChecked() const override
		{
			check(Payload != nullptr);
			return *Payload;
		}

	private:
		FAssetDocumentOpaqueHandle Handle;
		IAssetDocumentNativeHandlePayload* Payload = nullptr;
	};

public:
	void SetIssuingContext(const FInvocationContext& InContext)
	{
		Context = InContext;
	}

	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
	{
		FAssetDocumentIssuedHandleResult Result;
		if (!Payload || !Payload->IsAlive())
		{
			Result.Diagnostics.Add(
				MakeHandleDiagnostic(TEXT("CrossLayer.InvalidPayload")));
			return Result;
		}
		FEntry& Entry = Entries.AddDefaulted_GetRef();
		Entry.Handle.RequestId = Context.RequestId;
		Entry.Handle.TransactionId = Context.TransactionId;
		Entry.Handle.RuntimeRevision = Context.RuntimeRevision;
		Entry.Handle.ProviderId = TEXT("editor");
		Entry.Handle.Slot = Entries.Num();
		Entry.Handle.Generation = 1;
		Entry.Handle.Kind = Kind;
		Entry.Handle.Access = Access;
		Entry.Payload = MoveTemp(Payload);
		Result.Handle = Entry.Handle;
		return Result;
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleValidationContext& ValidationContext) override
	{
		++AcquireCount;
		FAssetDocumentVerifiedLeaseResult Result;
		const FAssetDocumentContractValidationResult Metadata =
			ValidateAssetDocumentHandleMetadata(Handle, ValidationContext);
		if (!Metadata.bValid)
		{
			Result.Diagnostics = Metadata.Diagnostics;
			return Result;
		}
		const int32 Index = static_cast<int32>(Handle.Slot) - 1;
		FEntry* Entry = FindLiveEntry(Index, Handle.Generation);
		if (Entry == nullptr)
		{
			Result.Diagnostics.Add(
				MakeHandleDiagnostic(TEXT("HandleMetadata.StaleHandle")));
			return Result;
		}
		Result.Lease =
			MakeUnique<FLease>(Handle, *Entry->Payload);
		return Result;
	}

	virtual FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation& Event) override
	{
		FAssetDocumentHandleInvalidationResult Result;
		++InvalidationCount;
		Trace.Add(Event.Reason
			== EAssetDocumentHandleInvalidationReason::Rollback
				? TEXT("Invalidate.Rollback")
				: TEXT("Invalidate.Other"));
		for (FEntry& Entry : Entries)
		{
			if (MatchesInvalidation(Entry.Handle, Event) && Entry.Payload)
			{
				Entry.Payload.Reset();
				++Entry.Handle.Generation;
				++Result.InvalidatedHandleCount;
			}
		}
		return Result;
	}

	bool IsLive(const FAssetDocumentOpaqueHandle& Handle) const
	{
		const int32 Index = static_cast<int32>(Handle.Slot) - 1;
		if (!Entries.IsValidIndex(Index))
		{
			return false;
		}
		const FEntry& Entry = Entries[Index];
		return Entry.Handle.Generation == Handle.Generation
			&& Entry.Payload
			&& Entry.Payload->IsAlive();
	}

	int32 AcquireCount = 0;
	int32 InvalidationCount = 0;
	TArray<FString> Trace;

private:
	FEntry* FindLiveEntry(int32 Index, uint32 Generation)
	{
		if (!Entries.IsValidIndex(Index))
		{
			return nullptr;
		}
		FEntry& Entry = Entries[Index];
		if (Entry.Handle.Generation != Generation)
		{
			return nullptr;
		}
		if (!Entry.Payload)
		{
			return nullptr;
		}
		if (!Entry.Payload->IsAlive())
		{
			return nullptr;
		}
		return &Entry;
	}

	static bool MatchesInvalidation(
		const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleInvalidation& Event)
	{
		return Handle.RequestId == Event.RequestId
			&& Handle.TransactionId == Event.TransactionId
			&& Handle.RuntimeRevision == Event.RuntimeRevision;
	}

	FInvocationContext Context;
	TArray<FEntry> Entries;
};

class FObservedAtomicTransaction final
	: public IAssetDocumentAtomicTransaction
{
public:
	explicit FObservedAtomicTransaction(
		TUniquePtr<IAssetDocumentAtomicTransaction> InInner)
		: Inner(MoveTemp(InInner))
	{
	}

	virtual FGuid GetTransactionId() const override
	{
		return Inner->GetTransactionId();
	}

	virtual EAssetDocumentAtomicState GetState() const override
	{
		return Inner->GetState();
	}

	virtual const FAssetDocumentMutationToken&
	GetMutationToken() const override
	{
		return Inner->GetMutationToken();
	}

	virtual FAssetDocumentContractValidationResult EnlistFootprint(
		TConstArrayView<FAssetDocumentMutationFootprint> Footprint) override
	{
		++EnlistCount;
		return Inner->EnlistFootprint(Footprint);
	}

	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override
	{
		return Inner->MarkReadyToCommit();
	}

	virtual FAssetDocumentContractValidationResult Commit(
		const FAssetDocumentDurableCompletionReceipt& Receipt) override
	{
		return Inner->Commit(Receipt);
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode) override
	{
		return Inner->Rollback(StableFailureCode);
	}

	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(
		TSharedRef<IAssetDocumentRollbackAction> Action) override
	{
		++RollbackActionEnlistCount;
		EnlistedRollbackActionComponentIds.Add(
			FString(Action->GetRollbackComponentId()));
		return Inner->EnlistRollbackAction(MoveTemp(Action));
	}

	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer> Finalizer) override
	{
		return Inner->EnlistPostRollbackFinalizer(MoveTemp(Finalizer));
	}

	int32 EnlistCount = 0;
	int32 RollbackActionEnlistCount = 0;
	TArray<FString> EnlistedRollbackActionComponentIds;

private:
	TUniquePtr<IAssetDocumentAtomicTransaction> Inner;
};

class FObservedRollbackSeed final
	: public IAssetDocumentEditorCutoverRollbackSeed
{
public:
	FObservedRollbackSeed(
		TSharedRef<IAssetDocumentEditorCutoverRollbackSeed> InInner,
		TArray<FString>& InTrace)
		: Inner(MoveTemp(InInner))
		, Trace(InTrace)
	{
	}

	virtual FStringView GetRollbackComponentId() const override
	{
		return Inner->GetRollbackComponentId();
	}

	virtual TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>
	OpenMutationCapture(const FGuid& TransactionId) override
	{
		return Inner->OpenMutationCapture(TransactionId);
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode) override
	{
		++RollbackCount;
		if (BeforeRollback)
		{
			BeforeRollback();
		}
		Trace.Add(FString::Printf(
			TEXT("Rollback.%s"),
			*FString(GetRollbackComponentId())));
		LastResult = Inner->Rollback(StableFailureCode);
		return LastResult;
	}

	void SetBeforeRollback(TFunction<void()> InBeforeRollback)
	{
		BeforeRollback = MoveTemp(InBeforeRollback);
	}

	int32 RollbackCount = 0;
	FAssetDocumentContractValidationResult LastResult;

private:
	TSharedRef<IAssetDocumentEditorCutoverRollbackSeed> Inner;
	TArray<FString>& Trace;
	TFunction<void()> BeforeRollback;
};

class FCancellationView final : public IAssetDocumentCancellationView
{
public:
	virtual bool IsCancellationRequested() const override
	{
		return false;
	}
};

FAssetDocumentContractValidationResult MakeSeedResult(
	bool bValid,
	const TCHAR* Code = TEXT(""))
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = bValid;
	if (!bValid)
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/TestSeed");
		Diagnostic.Code = Code;
		Diagnostic.Message = TEXT("Seed result.");
		Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Warning;
	}
	return Result;
}

class FTestRollbackSeed final
	: public IAssetDocumentEditorCutoverRollbackSeed
{
public:
	FTestRollbackSeed(
		FString InComponentId,
		FAssetDocumentContractValidationResult InRollbackResult)
		: ComponentId(MoveTemp(InComponentId))
		, RollbackResult(MoveTemp(InRollbackResult))
	{
	}

	virtual FStringView GetRollbackComponentId() const override
	{
		return ComponentId;
	}

	virtual TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>
	OpenMutationCapture(const FGuid& TransactionId) override
	{
		(void)TransactionId;
		++OpenMutationCaptureCount;
		return nullptr;
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode) override
	{
		++RollbackCount;
		LastFailureCode = StableFailureCode;
		if (ActionDuringRollback != nullptr)
		{
			bProbedConsumptionBeforeDelegate = true;
			ReentrantResult =
				ActionDuringRollback->Rollback(TEXT("Test.Reentrant"));
		}
		return RollbackResult;
	}

	FString ComponentId;
	FAssetDocumentContractValidationResult RollbackResult;
	IAssetDocumentRollbackAction* ActionDuringRollback = nullptr;
	int32 RollbackCount = 0;
	int32 OpenMutationCaptureCount = 0;
	FString LastFailureCode;
	bool bProbedConsumptionBeforeDelegate = false;
	FAssetDocumentContractValidationResult ReentrantResult;
};

bool HasExactDiagnostic(
	const FAssetDocumentContractValidationResult& Result,
	const TCHAR* Path,
	const TCHAR* Code)
{
	return !Result.bValid && Result.Diagnostics.Num() == 1
		&& Result.Diagnostics[0].Path == Path
		&& Result.Diagnostics[0].Code == Code;
}

FAssetDocumentRuntimeValueDto MakeStringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeNullValue()
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Null;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeBooleanValue(bool bValue)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Boolean;
	Result.BooleanValue = bValue;
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
	TArray<FAssetDocumentRuntimeValueDto> Elements)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Elements);
	return Result;
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

FAssetDocumentPrimitiveOutcome Invoke(
	IAssetDocumentPrimitiveProvider& Provider,
	FCrossLayerHandleResolver& Handles,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FInvocationContext& Context,
	FAssetDocumentRuntimeValueDto Value,
	TArray<FAssetDocumentPrimitiveHandleArgument> Arguments,
	IAssetDocumentAtomicTransaction* Transaction)
{
	Handles.SetIssuingContext(Context);
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = Descriptor.PrimitiveId;
	Invocation.Version = Descriptor.Version;
	Invocation.RequestId = Context.RequestId;
	Invocation.TransactionId = Context.TransactionId;
	Invocation.RuntimeRevision = Context.RuntimeRevision;
	Invocation.Phase = Descriptor.AllowedPhases[0];
	Invocation.ValueArguments = MoveTemp(Value);
	Invocation.HandleArguments = MoveTemp(Arguments);
	return Provider.Invoke(Invocation, Handles, Transaction);
}

FAssetDocumentRuntimeValueDto MakeClassResolveArguments()
{
	return MakeObjectValue(
		{TEXT("class"), TEXT("expected_base")},
		{MakeStringValue(UPrimaryAssetLabel::StaticClass()->GetPathName()),
			MakeNullValue()});
}

FAssetDocumentRuntimeValueDto MakeFactoryArguments(const FString& Name)
{
	return MakeObjectValue(
		{TEXT("factory_id"), TEXT("package_path"), TEXT("asset_name")},
		{MakeStringValue(TEXT("data_asset_factory.v1")),
			MakeStringValue(TEXT("/Game/AssetDocumentTests")),
			MakeStringValue(Name)});
}

FAssetDocumentRuntimeValueDto MakeResolveArguments(
	const FString& CanonicalPath)
{
	return MakeObjectValue(
		{TEXT("path"), TEXT("expected_class")},
		{MakeStringValue(CanonicalPath),
			MakeStringValue(
				UPrimaryAssetLabel::StaticClass()->GetPathName())});
}

FAssetDocumentRuntimeValueDto MakeBatchWriteArguments(
	bool bLabelDirectory,
	bool bRuntimeLabel)
{
	auto Row = [](const TCHAR* Name, bool bValue)
	{
		return MakeObjectValue(
			{TEXT("name"), TEXT("value")},
			{MakeStringValue(Name), MakeBooleanValue(bValue)});
	};
	return MakeObjectValue(
		{TEXT("properties")},
		{MakeArrayValue({
			Row(TEXT("bIsRuntimeLabel"), bRuntimeLabel),
			Row(TEXT("bLabelAssetsInMyDirectory"), bLabelDirectory)})});
}

FAssetDocumentRuntimeValueDto MakeBatchWriteArguments(
	bool bLabelDirectory,
	bool bRuntimeLabel,
	TConstArrayView<FString> ExplicitAssetPaths)
{
	auto Row = [](const TCHAR* Name, FAssetDocumentRuntimeValueDto Value)
	{
		return MakeObjectValue(
			{TEXT("name"), TEXT("value")},
			{MakeStringValue(Name), MoveTemp(Value)});
	};
	TArray<FAssetDocumentRuntimeValueDto> ExplicitAssets;
	for (const FString& Path : ExplicitAssetPaths)
	{
		ExplicitAssets.Add(MakeStringValue(Path));
	}
	return MakeObjectValue(
		{TEXT("properties")},
		{MakeArrayValue({
			Row(TEXT("bIsRuntimeLabel"), MakeBooleanValue(bRuntimeLabel)),
			Row(TEXT("bLabelAssetsInMyDirectory"),
				MakeBooleanValue(bLabelDirectory)),
			Row(TEXT("ExplicitAssets"),
				MakeArrayValue(MoveTemp(ExplicitAssets)))})});
}

FInvocationContext MakeTransactionContext(
	const IAssetDocumentAtomicTransaction& Transaction)
{
	const FAssetDocumentMutationToken& Token =
		Transaction.GetMutationToken();
	FInvocationContext Context;
	Context.RequestId = Token.RequestId;
	Context.TransactionId = Token.TransactionId;
	Context.RuntimeRevision = Token.RuntimeRevision;
	return Context;
}

FAssetDocumentEditorCutoverMutationIntent MakeCreateIntent(
	const FInvocationContext& Context,
	const FString& Name,
	bool bWithProperties)
{
	FAssetDocumentEditorCutoverMutationIntent Intent;
	Intent.RequestId = Context.RequestId;
	Intent.RuntimeRevision = Context.RuntimeRevision;
	Intent.CanonicalObjectPath = FString::Printf(
		TEXT("/Game/AssetDocumentTests/%s.%s"), *Name, *Name);
	Intent.ExactAssetClassPath =
		UPrimaryAssetLabel::StaticClass()->GetPathName();
	Intent.FactoryId = TEXT("data_asset_factory.v1");
	Intent.bCreateByFactory = true;
	Intent.bWriteAdmittedProperties = bWithProperties;
	return Intent;
}

FAssetDocumentEditorCutoverMutationIntent MakePropertyIntent(
	const FInvocationContext& Context,
	const UPrimaryAssetLabel& Asset)
{
	FAssetDocumentEditorCutoverMutationIntent Intent;
	Intent.RequestId = Context.RequestId;
	Intent.RuntimeRevision = Context.RuntimeRevision;
	Intent.CanonicalObjectPath = Asset.GetPathName();
	Intent.ExactAssetClassPath = Asset.GetClass()->GetPathName();
	Intent.bWriteAdmittedProperties = true;
	return Intent;
}

struct FObservedSeedSet
{
	TArray<TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>> Seeds;
	TArray<TSharedRef<FObservedRollbackSeed>> Observers;
};

FObservedSeedSet ObserveSeeds(
	const FAssetDocumentEditorCutoverTransactionPreparation& Preparation,
	TArray<FString>& Trace)
{
	FObservedSeedSet Result;
	for (const TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>& Seed :
		Preparation.RollbackSeeds)
	{
		TSharedRef<FObservedRollbackSeed> Observer =
			MakeShared<FObservedRollbackSeed>(Seed, Trace);
		Result.Seeds.Add(Observer);
		Result.Observers.Add(MoveTemp(Observer));
	}
	return Result;
}

struct FAtomicFlow
{
	TUniquePtr<FAssetDocumentRollbackActionComposition> Composition;
	TUniquePtr<FObservedAtomicTransaction> Transaction;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

bool StartAtomicFlow(
	FAutomationTestBase& Test,
	const FInvocationContext& Context,
	const FAssetDocumentEditorCutoverTransactionPreparation& Preparation,
	FCrossLayerHandleResolver& Handles,
	const FObservedSeedSet& Observed,
	FAtomicFlow& OutFlow,
	bool bDynamicallyEnlistActions = false)
{
	TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions =
		AdaptAssetDocumentEditorRollbackSeeds(Observed.Seeds);
	if (!Test.TestEqual(
		TEXT("all prepared seeds adapt to actions"),
		Actions.Num(), Preparation.RollbackSeeds.Num()))
	{
		return false;
	}
	TArray<TSharedRef<IAssetDocumentRollbackAction>> InitialActions;
	if (!bDynamicallyEnlistActions)
	{
		InitialActions = Actions;
	}
	OutFlow.Composition =
		MakeUnique<FAssetDocumentRollbackActionComposition>(
			Handles,
			MoveTemp(InitialActions),
			[&OutFlow](
				EAssetDocumentRuntimeRequestPhase,
				const FAssetDocumentRuntimeDiagnostic& Diagnostic)
			{
				OutFlow.Diagnostics.Add(Diagnostic);
			});
	FAssetDocumentPreflightReceipt Receipt;
	Receipt.RequestId = Context.RequestId;
	Receipt.RuntimeRevision = Context.RuntimeRevision;
	Receipt.CompleteFootprint = Preparation.CompleteFootprint;
	Receipt.ReceiptHash = TEXT("sha256:cross-layer-composition");
	Receipt.bSucceeded = true;
	FCancellationView Cancellation;
	TUniquePtr<IAssetDocumentAtomicTransaction> Inner =
		OutFlow.Composition->BeginLiveAfterPreflight(
			Receipt, Cancellation);
	if (!Test.TestNotNull(
		TEXT("real Atomic begin has complete rollback coverage"),
		Inner.Get()))
	{
		return false;
	}
	OutFlow.Transaction =
		MakeUnique<FObservedAtomicTransaction>(MoveTemp(Inner));
	if (bDynamicallyEnlistActions)
	{
		for (TSharedRef<IAssetDocumentRollbackAction>& Action : Actions)
		{
			const FAssetDocumentContractValidationResult Enlisted =
				OutFlow.Transaction->EnlistRollbackAction(MoveTemp(Action));
			if (!Test.TestTrue(
				TEXT("captured editor action enlists into live transaction"),
				Enlisted.bValid))
			{
				return false;
			}
		}
	}
	return true;
}

bool OpenAllCaptures(
	FAutomationTestBase& Test,
	const FObservedSeedSet& Observed,
	const FGuid& TransactionId,
	TArray<TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>>& OutCaptures)
{
	for (const TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>& Seed :
		Observed.Seeds)
	{
		TUniquePtr<IAssetDocumentEditorCutoverMutationCapture> Capture =
			Seed->OpenMutationCapture(TransactionId);
		if (!Test.TestTrue(
			TEXT("every rollback component opens capture"),
			Capture.IsValid()))
		{
			return false;
		}
		OutCaptures.Add(MoveTemp(Capture));
	}
	return true;
}

void CloseAllCaptures(
	TArray<TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>>& Captures)
{
	for (TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>& Capture :
		Captures)
	{
		Capture.Reset();
	}
}

TOptional<FAssetDocumentOpaqueHandle> ResolvePrimaryAssetLabelClass(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	FCrossLayerHandleResolver& Handles,
	const FInvocationContext& Context)
{
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(
		Provider, TEXT("editor.class.resolve_canonical.v1"));
	if (!Test.TestNotNull(TEXT("class resolve descriptor exists"), Descriptor))
	{
		return {};
	}
	const FAssetDocumentPrimitiveOutcome Outcome = Invoke(
		Provider, Handles, *Descriptor, Context,
		MakeClassResolveArguments(), {}, nullptr);
	const FAssetDocumentOpaqueHandle* Handle =
		FindSingleHandle(Outcome, TEXT("class"));
	if (!Test.TestEqual(
			TEXT("class resolves through admitted provider"),
			Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		|| !Test.TestNotNull(TEXT("class resolve returns one handle"), Handle))
	{
		return {};
	}
	return *Handle;
}

struct FCreatedAssetHandles
{
	FAssetDocumentOpaqueHandle Read;
	FAssetDocumentOpaqueHandle Mutable;
	FAssetDocumentOpaqueHandle Package;
	bool bValid = false;
};

bool PopulateCreatedHandles(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	FCreatedAssetHandles& Out)
{
	if (!Test.TestEqual(TEXT("factory creates through admitted provider"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return false;
	}
	const FAssetDocumentOpaqueHandle* Read =
		FindSingleHandle(Outcome, TEXT("asset_read"));
	const FAssetDocumentOpaqueHandle* Mutable =
		FindSingleHandle(Outcome, TEXT("asset_mutable"));
	const FAssetDocumentOpaqueHandle* Package =
		FindSingleHandle(Outcome, TEXT("package"));
	if (!Test.TestNotNull(TEXT("factory returns asset_read"), Read))
	{
		return false;
	}
	if (!Test.TestNotNull(TEXT("factory returns asset_mutable"), Mutable))
	{
		return false;
	}
	if (!Test.TestNotNull(TEXT("factory returns package"), Package))
	{
		return false;
	}
	Out.Read = *Read;
	Out.Mutable = *Mutable;
	Out.Package = *Package;
	Out.bValid = true;
	return true;
}

FCreatedAssetHandles CreatePrimaryAssetLabel(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	FCrossLayerHandleResolver& Handles,
	const FInvocationContext& Context,
	const FString& Name,
	IAssetDocumentAtomicTransaction& Transaction)
{
	FCreatedAssetHandles Result;
	const TOptional<FAssetDocumentOpaqueHandle> Class =
		ResolvePrimaryAssetLabelClass(Test, Provider, Handles, Context);
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(
		Provider, TEXT("editor.asset.create_by_factory.v1"));
	if (!Class.IsSet())
	{
		return Result;
	}
	if (!Test.TestNotNull(TEXT("factory descriptor exists"), Descriptor))
	{
		return Result;
	}
	const FAssetDocumentPrimitiveOutcome Outcome = Invoke(
		Provider, Handles, *Descriptor, Context, MakeFactoryArguments(Name),
		{{TEXT("asset_class"), Class.GetValue()}}, &Transaction);
	PopulateCreatedHandles(Test, Outcome, Result);
	return Result;
}

struct FExistingAssetHandles
{
	FAssetDocumentOpaqueHandle Read;
	FAssetDocumentOpaqueHandle Mutable;
	bool bValid = false;
};

FExistingAssetHandles ResolveExistingAsset(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	FCrossLayerHandleResolver& Handles,
	const FInvocationContext& Context,
	const FString& CanonicalPath)
{
	FExistingAssetHandles Result;
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(
		Provider, TEXT("editor.asset.resolve_existing.v1"));
	if (!Test.TestNotNull(
		TEXT("resolve-existing descriptor exists"), Descriptor))
	{
		return Result;
	}
	const FAssetDocumentPrimitiveOutcome Outcome = Invoke(
		Provider, Handles, *Descriptor, Context,
		MakeResolveArguments(CanonicalPath), {}, nullptr);
	const FAssetDocumentOpaqueHandle* Read =
		FindSingleHandle(Outcome, TEXT("asset_read"));
	const FAssetDocumentOpaqueHandle* Mutable =
		FindSingleHandle(Outcome, TEXT("asset_mutable"));
	if (!Test.TestEqual(TEXT("existing asset resolves through provider"),
			Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		|| !Read || !Mutable)
	{
		return Result;
	}
	Result.Read = *Read;
	Result.Mutable = *Mutable;
	Result.bValid = true;
	return Result;
}

TOptional<FAssetDocumentOpaqueHandle> AdmitPropertyBatch(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	FCrossLayerHandleResolver& Handles,
	const FInvocationContext& Context,
	const FAssetDocumentOpaqueHandle& ReadAsset)
{
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(
		Provider, TEXT("editor.property.enumerate_admitted_batch.v1"));
	if (!Test.TestNotNull(
		TEXT("property batch descriptor exists"), Descriptor))
	{
		return {};
	}
	const FAssetDocumentPrimitiveOutcome Outcome = Invoke(
		Provider, Handles, *Descriptor, Context,
		MakeObjectValue({}, {}),
		{{TEXT("asset"), ReadAsset}}, nullptr);
	const FAssetDocumentOpaqueHandle* Batch =
		FindSingleHandle(Outcome, TEXT("property_batch"));
	if (!Test.TestEqual(TEXT("property batch is admitted"),
			Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		|| !Test.TestNotNull(
			TEXT("property admission returns composite handle"), Batch))
	{
		return {};
	}
	return *Batch;
}

bool WritePropertyBatchWithArguments(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	FCrossLayerHandleResolver& Handles,
	const FInvocationContext& Context,
	const FExistingAssetHandles& Asset,
	IAssetDocumentAtomicTransaction& Transaction,
	FAssetDocumentRuntimeValueDto Arguments)
{
	const TOptional<FAssetDocumentOpaqueHandle> Batch =
		AdmitPropertyBatch(Test, Provider, Handles, Context, Asset.Read);
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(
		Provider, TEXT("editor.property.write_admitted_batch.v1"));
	if (!Batch.IsSet()
		|| !Test.TestNotNull(
			TEXT("property batch write descriptor exists"), Descriptor))
	{
		return false;
	}
	const FAssetDocumentPrimitiveOutcome Outcome = Invoke(
		Provider, Handles, *Descriptor, Context,
		MoveTemp(Arguments),
		{{TEXT("asset"), Asset.Mutable},
			{TEXT("property_batch"), Batch.GetValue()}},
		&Transaction);
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic :
		Outcome.Diagnostics)
	{
		Test.AddError(FString::Printf(
			TEXT("property batch diagnostic %s at %s"),
			*Diagnostic.Code, *Diagnostic.Path));
	}
	return Test.TestEqual(
		TEXT("real admitted property batch mutates"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
}

bool WritePropertyBatch(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	FCrossLayerHandleResolver& Handles,
	const FInvocationContext& Context,
	const FExistingAssetHandles& Asset,
	IAssetDocumentAtomicTransaction& Transaction,
	bool bLabelDirectory,
	bool bRuntimeLabel)
{
	return WritePropertyBatchWithArguments(
		Test,
		Provider,
		Handles,
		Context,
		Asset,
		Transaction,
		MakeBatchWriteArguments(bLabelDirectory, bRuntimeLabel));
}

bool WritePropertyBatch(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	FCrossLayerHandleResolver& Handles,
	const FInvocationContext& Context,
	const FExistingAssetHandles& Asset,
	IAssetDocumentAtomicTransaction& Transaction,
	bool bLabelDirectory,
	bool bRuntimeLabel,
	TConstArrayView<FString> ExplicitAssetPaths)
{
	return WritePropertyBatchWithArguments(
		Test,
		Provider,
		Handles,
		Context,
		Asset,
		Transaction,
		MakeBatchWriteArguments(
			bLabelDirectory, bRuntimeLabel, ExplicitAssetPaths));
}

FString MakeCrossLayerAssetName(const TCHAR* Prefix)
{
	return FString::Printf(
		TEXT("%s_%s"), Prefix,
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

class FScopedPrimaryAssetLabelFixture final
{
public:
	FScopedPrimaryAssetLabelFixture(
		IAssetDocumentPrimitiveProvider& InProvider,
		FCrossLayerHandleResolver& InHandles)
		: Provider(InProvider)
		, Handles(InHandles)
		, Name(MakeCrossLayerAssetName(TEXT("DA_CrossLayerExisting")))
		, Intent(MakeCreateIntent(Context, Name, false))
	{
	}

	~FScopedPrimaryAssetLabelFixture()
	{
		CloseAllCaptures(Captures);
		if (Flow.Transaction
			&& (Flow.Transaction->GetState() == EAssetDocumentAtomicState::Live
				|| Flow.Transaction->GetState()
					== EAssetDocumentAtomicState::Mutating))
		{
			Flow.Transaction->Rollback(TEXT("Test.FixtureCleanup"));
		}
	}

	bool Initialize(FAutomationTestBase& Test)
	{
		if (!PrepareFixture(Test))
		{
			return false;
		}
		Observed = ObserveSeeds(Preparation, FixtureTrace);
		if (!BeginFixtureFlow(Test))
		{
			return false;
		}
		const FInvocationContext TransactionContext =
			MakeTransactionContext(*Flow.Transaction);
		const FCreatedAssetHandles Created = CreatePrimaryAssetLabel(
			Test, Provider, Handles, TransactionContext, Name,
			*Flow.Transaction);
		CloseAllCaptures(Captures);
		if (!Created.bValid)
		{
			return false;
		}
		Asset = Cast<UPrimaryAssetLabel>(
			FSoftObjectPath(Intent.CanonicalObjectPath).ResolveObject());
		return Test.TestNotNull(
			TEXT("fixture resolves exact PrimaryAssetLabel"), Asset);
	}

	UPrimaryAssetLabel* GetAsset() const
	{
		return Asset;
	}

private:
	bool PrepareFixture(FAutomationTestBase& Test)
	{
		Preparation =
			PrepareAssetDocumentEditorCutoverTransaction(Intent);
		if (!Test.TestTrue(
			TEXT("fixture create prepares"), Preparation.bPrepared))
		{
			return false;
		}
		if (!Test.TestEqual(
			TEXT("fixture has one created seed"),
			Preparation.RollbackSeeds.Num(), 1))
		{
			return false;
		}
		return true;
	}

	bool BeginFixtureFlow(FAutomationTestBase& Test)
	{
		if (!StartAtomicFlow(
			Test, Context, Preparation, Handles, Observed, Flow))
		{
			return false;
		}
		if (!OpenAllCaptures(
			Test, Observed, Flow.Transaction->GetTransactionId(), Captures))
		{
			return false;
		}
		return true;
	}

	IAssetDocumentPrimitiveProvider& Provider;
	FCrossLayerHandleResolver& Handles;
	FInvocationContext Context;
	FString Name;
	FAssetDocumentEditorCutoverMutationIntent Intent;
	FAssetDocumentEditorCutoverTransactionPreparation Preparation;
	TArray<FString> FixtureTrace;
	FObservedSeedSet Observed;
	FAtomicFlow Flow;
	TArray<TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>> Captures;
	UPrimaryAssetLabel* Asset = nullptr;
};

bool HaveEqualDiagnostics(
	const FAssetDocumentRuntimeDiagnostic& Left,
	const FAssetDocumentRuntimeDiagnostic& Right)
{
	return Left.Path == Right.Path
		&& Left.Code == Right.Code
		&& Left.Message == Right.Message;
}

bool HaveEqualValidationResults(
	const FAssetDocumentContractValidationResult& Left,
	const FAssetDocumentContractValidationResult& Right)
{
	if (Left.bValid != Right.bValid
		|| Left.Diagnostics.Num() != Right.Diagnostics.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Left.Diagnostics.Num(); ++Index)
	{
		if (!HaveEqualDiagnostics(
			Left.Diagnostics[Index], Right.Diagnostics[Index]))
		{
			return false;
		}
	}
	return true;
}

bool HasDiagnostic(
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics,
	FStringView Code)
{
	return Diagnostics.ContainsByPredicate(
		[Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code;
		});
}

void TestExactRollbackTrace(
	FAutomationTestBase& Test,
	const TArray<FString>& Trace,
	TConstArrayView<FString> Components)
{
	Test.TestEqual(
		TEXT("rollback trace count"), Trace.Num(), Components.Num() + 1);
	if (Trace.Num() != Components.Num() + 1)
	{
		return;
	}
	Test.TestEqual(
		TEXT("handle invalidation precedes every action"),
		Trace[0], FString(TEXT("Invalidate.Rollback")));
	for (int32 Index = 0; Index < Components.Num(); ++Index)
	{
		Test.TestEqual(
			*FString::Printf(TEXT("rollback action %d"), Index),
			Trace[Index + 1],
			FString::Printf(TEXT("Rollback.%s"), *Components[Index]));
	}
}

struct FPropertyFlowState
{
	FInvocationContext Context;
	FAssetDocumentEditorCutoverTransactionPreparation Preparation;
	FObservedSeedSet Observed;
	FAtomicFlow Atomic;
	TArray<TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>> Captures;
};

bool PreparePropertyFlow(
	FAutomationTestBase& Test,
	FCrossLayerHandleResolver& Handles,
	UPrimaryAssetLabel& Asset,
	FPropertyFlowState& Out)
{
	Asset.bLabelAssetsInMyDirectory = false;
	Asset.bIsRuntimeLabel = false;
	Asset.GetOutermost()->SetDirtyFlag(true);
	Out.Preparation = PrepareAssetDocumentEditorCutoverTransaction(
		MakePropertyIntent(Out.Context, Asset));
	if (!Test.TestTrue(
		TEXT("property-only preparation succeeds"),
		Out.Preparation.bPrepared))
	{
		return false;
	}
	if (!Test.TestEqual(
		TEXT("property-only has exact two seeds"),
		Out.Preparation.RollbackSeeds.Num(), 2))
	{
		return false;
	}
	Out.Observed = ObserveSeeds(Out.Preparation, Handles.Trace);
	return true;
}

bool BeginPropertyFlow(
	FAutomationTestBase& Test,
	FCrossLayerHandleResolver& Handles,
	FPropertyFlowState& Out,
	bool bDynamicallyEnlistActions = false)
{
	if (!StartAtomicFlow(
		Test, Out.Context, Out.Preparation, Handles,
		Out.Observed, Out.Atomic, bDynamicallyEnlistActions))
	{
		return false;
	}
	if (!OpenAllCaptures(
		Test, Out.Observed,
		Out.Atomic.Transaction->GetTransactionId(), Out.Captures))
	{
		return false;
	}
	return true;
}

bool MutatePropertyFlow(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	FCrossLayerHandleResolver& Handles,
	UPrimaryAssetLabel& Asset,
	FPropertyFlowState& Out)
{
	const FInvocationContext TransactionContext =
		MakeTransactionContext(*Out.Atomic.Transaction);
	const FExistingAssetHandles Existing = ResolveExistingAsset(
		Test, Provider, Handles, TransactionContext, Asset.GetPathName());
	if (!Existing.bValid)
	{
		return false;
	}
	if (!WritePropertyBatch(
		Test, Provider, Handles, TransactionContext, Existing,
		*Out.Atomic.Transaction, true, true))
	{
		return false;
	}
	CloseAllCaptures(Out.Captures);
	return true;
}

bool TestMutatedPropertyValues(
	FAutomationTestBase& Test,
	const UPrimaryAssetLabel& Asset)
{
	const bool bDirectory = Test.TestTrue(
		TEXT("batch mutates directory label"),
		Asset.bLabelAssetsInMyDirectory);
	const bool bRuntime = Test.TestTrue(
		TEXT("batch mutates runtime label"), Asset.bIsRuntimeLabel);
	return bDirectory && bRuntime;
}

bool PrepareAndMutatePropertyFlow(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	FCrossLayerHandleResolver& Handles,
	UPrimaryAssetLabel& Asset,
	FPropertyFlowState& Out,
	bool bDynamicallyEnlistActions = false)
{
	if (!PreparePropertyFlow(Test, Handles, Asset, Out))
	{
		return false;
	}
	if (!BeginPropertyFlow(
		Test, Handles, Out, bDynamicallyEnlistActions))
	{
		return false;
	}
	if (!MutatePropertyFlow(Test, Provider, Handles, Asset, Out))
	{
		return false;
	}
	return TestMutatedPropertyValues(Test, Asset);
}

TArray<FString> SnapshotExplicitAssetPaths(
	const UPrimaryAssetLabel& Asset)
{
	TArray<FString> Paths;
	for (const TSoftObjectPtr<UObject>& ExplicitAsset :
		Asset.ExplicitAssets)
	{
		Paths.Add(ExplicitAsset.ToSoftObjectPath().ToString());
	}
	return Paths;
}

void SetExplicitAssetPaths(
	UPrimaryAssetLabel& Asset,
	TConstArrayView<FString> Paths)
{
	Asset.ExplicitAssets.Reset();
	for (const FString& Path : Paths)
	{
		Asset.ExplicitAssets.Add(
			TSoftObjectPtr<UObject>(FSoftObjectPath(Path)));
	}
}

FAssetDocumentHandleValidationContext MakeValidationContext(
	const FAssetDocumentOpaqueHandle& Handle)
{
	FAssetDocumentHandleValidationContext Validation;
	Validation.RequestId = Handle.RequestId;
	Validation.TransactionId = Handle.TransactionId;
	Validation.RuntimeRevision = Handle.RuntimeRevision;
	Validation.ProviderId = Handle.ProviderId;
	Validation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Validation.RequiredKind = Handle.Kind;
	Validation.RequiredAccess = Handle.Access;
	return Validation;
}

void TestDynamicPropertyRestoreExactState(FAutomationTestBase& Test)
{
	TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	FCrossLayerHandleResolver Handles;
	FScopedPrimaryAssetLabelFixture Fixture(*Provider, Handles);
	if (!Fixture.Initialize(Test))
	{
		return;
	}
	UPrimaryAssetLabel* const OriginalIdentity = Fixture.GetAsset();
	const FString OriginalObjectPath = OriginalIdentity->GetPathName();
	const TArray<FString> OriginalOrder = {
		TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"),
		TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"),
	};
	const TArray<FString> MutatedOrder = {
		TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"),
		TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"),
		TEXT("/Engine/EngineResources/DefaultTextureCube.DefaultTextureCube"),
	};
	SetExplicitAssetPaths(*OriginalIdentity, OriginalOrder);

	FPropertyFlowState Property;
	if (!PreparePropertyFlow(Test, Handles, *OriginalIdentity, Property)
		|| !BeginPropertyFlow(Test, Handles, Property, true))
	{
		return;
	}
	Test.TestEqual(
		TEXT("all real preimage actions enlist dynamically"),
		Property.Atomic.Transaction->RollbackActionEnlistCount,
		Property.Preparation.RollbackSeeds.Num());
	Test.TestEqual(
		TEXT("dynamic action ids preserve captured seed order"),
		Property.Atomic.Transaction->EnlistedRollbackActionComponentIds,
		TArray<FString>({TEXT("rollback.uobject"), TEXT("rollback.package")}));

	const FInvocationContext TransactionContext =
		MakeTransactionContext(*Property.Atomic.Transaction);
	const FExistingAssetHandles Existing = ResolveExistingAsset(
		Test, *Provider, Handles, TransactionContext, OriginalObjectPath);
	if (!Existing.bValid)
	{
		return;
	}
	Test.TestTrue(
		TEXT("pre-mutation handle starts live"),
		Handles.IsLive(Existing.Read));
	bool bEveryActionSawStaleHandle = true;
	int32 BeforeRollbackProbeCount = 0;
	for (const TSharedRef<FObservedRollbackSeed>& Observer :
		Property.Observed.Observers)
	{
		Observer->SetBeforeRollback(
			[&Handles, &Existing, &bEveryActionSawStaleHandle,
				&BeforeRollbackProbeCount]()
			{
				++BeforeRollbackProbeCount;
				bEveryActionSawStaleHandle =
					bEveryActionSawStaleHandle
					&& !Handles.IsLive(Existing.Read);
			});
	}
	if (!WritePropertyBatch(
		Test,
		*Provider,
		Handles,
		TransactionContext,
		Existing,
		*Property.Atomic.Transaction,
		true,
		true,
		MutatedOrder))
	{
		return;
	}
	CloseAllCaptures(Property.Captures);
	Test.TestTrue(
		TEXT("dynamic flow mutates scalar value"),
		OriginalIdentity->bIsRuntimeLabel);
	Test.TestEqual(
		TEXT("dynamic flow mutates ordered collection"),
		SnapshotExplicitAssetPaths(*OriginalIdentity),
		MutatedOrder);
	Test.TestEqual(
		TEXT("provider mutation preserves stable object identity"),
		Fixture.GetAsset(), OriginalIdentity);
	OriginalIdentity->GetOutermost()->SetDirtyFlag(false);

	Handles.Trace.Reset();
	const FAssetDocumentContractValidationResult Rollback =
		Property.Atomic.Transaction->Rollback(
			TEXT("Test.InjectedProviderRequestFailure"));
	Test.TestTrue(
		TEXT("dynamic editor rollback succeeds"), Rollback.bValid);
	Test.TestEqual(
		TEXT("dynamic editor rollback reaches RolledBack"),
		Property.Atomic.Transaction->GetState(),
		EAssetDocumentAtomicState::RolledBack);
	Test.TestFalse(
		TEXT("dynamic rollback restores scalar value"),
		OriginalIdentity->bIsRuntimeLabel);
	Test.TestEqual(
		TEXT("dynamic rollback restores ordered collection exactly"),
		SnapshotExplicitAssetPaths(*OriginalIdentity),
		OriginalOrder);
	Test.TestEqual(
		TEXT("dynamic rollback restores exact object identity"),
		FSoftObjectPath(OriginalObjectPath).ResolveObject(),
		static_cast<UObject*>(OriginalIdentity));
	Test.TestTrue(
		TEXT("dynamic rollback restores original dirty state"),
		OriginalIdentity->GetOutermost()->IsDirty());
	Test.TestEqual(
		TEXT("every dynamic action begins after invalidation"),
		BeforeRollbackProbeCount,
		Property.Observed.Observers.Num());
	Test.TestTrue(
		TEXT("pre-mutation handle is stale before every action"),
		bEveryActionSawStaleHandle);
	for (const TSharedRef<FObservedRollbackSeed>& Observer :
		Property.Observed.Observers)
	{
		Test.TestEqual(
			TEXT("each dynamically enlisted action executes once"),
			Observer->RollbackCount,
			1);
	}
	TestExactRollbackTrace(
		Test,
		Handles.Trace,
		{TEXT("rollback.package"), TEXT("rollback.uobject")});

	FAssetDocumentVerifiedLeaseResult Reused =
		Handles.AcquireVerifiedLease(
			Existing.Read, MakeValidationContext(Existing.Read));
	Test.TestFalse(
		TEXT("pre-mutation handle cannot be reused after rollback"),
		Reused.Lease.IsValid());
	Test.TestTrue(
		TEXT("post-rollback handle rejection is stale-handle exact"),
		HasDiagnostic(
			Reused.Diagnostics, TEXT("HandleMetadata.StaleHandle")));
}

void TestCrossLayerPropertyRestoreSuccess(FAutomationTestBase& Test)
{
	TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	FCrossLayerHandleResolver Handles;
	FScopedPrimaryAssetLabelFixture Fixture(*Provider, Handles);
	if (!Fixture.Initialize(Test))
	{
		return;
	}
	FPropertyFlowState Property;
	if (!PrepareAndMutatePropertyFlow(
		Test, *Provider, Handles, *Fixture.GetAsset(), Property))
	{
		return;
	}
	Fixture.GetAsset()->GetOutermost()->SetDirtyFlag(false);
	const int32 AcquireBeforeRollback = Handles.AcquireCount;
	Handles.Trace.Reset();
	const FAssetDocumentContractValidationResult Rollback =
		Property.Atomic.Transaction->Rollback(TEXT("Test.CrossLayerSuccess"));

	Test.TestTrue(TEXT("cross-layer property rollback succeeds"),
		Rollback.bValid);
	Test.TestEqual(TEXT("real property footprint enlists once"),
		Property.Atomic.Transaction->EnlistCount, 1);
	Test.TestEqual(TEXT("rollback performs zero handle acquire"),
		Handles.AcquireCount, AcquireBeforeRollback);
	Test.TestFalse(TEXT("rollback restores directory label"),
		Fixture.GetAsset()->bLabelAssetsInMyDirectory);
	Test.TestFalse(TEXT("rollback restores runtime label"),
		Fixture.GetAsset()->bIsRuntimeLabel);
	Test.TestTrue(TEXT("rollback restores dirty-unsaved state"),
		Fixture.GetAsset()->GetOutermost()->IsDirty());
	TestExactRollbackTrace(
		Test, Handles.Trace,
		{TEXT("rollback.package"), TEXT("rollback.uobject")});
}

void TestPropertyFailureAtomicResult(
	FAutomationTestBase& Test,
	const FPropertyFlowState& Property,
	const FAssetDocumentContractValidationResult& Rollback,
	const FCrossLayerHandleResolver& Handles,
	int32 AcquireBeforeRollback)
{
	Test.TestFalse(TEXT("provider restore failure poisons rollback"),
		Rollback.bValid);
	Test.TestEqual(TEXT("poison sibling uses dynamically enlisted actions"),
		Property.Atomic.Transaction->RollbackActionEnlistCount,
		Property.Preparation.RollbackSeeds.Num());
	Test.TestEqual(TEXT("poisoned Atomic state is retained"),
		Property.Atomic.Transaction->GetState(),
		EAssetDocumentAtomicState::Poisoned);
	Test.TestTrue(TEXT("Atomic publishes component failure"),
		HasDiagnostic(
			Property.Atomic.Diagnostics,
			TEXT("Atomic.Poisoned.ComponentFailed")));
	Test.TestTrue(TEXT("Atomic publishes rollback failure"),
		HasDiagnostic(
			Property.Atomic.Diagnostics,
			TEXT("Atomic.Poisoned.RollbackFailed")));
	Test.TestEqual(TEXT("failed restore performs zero acquire"),
		Handles.AcquireCount, AcquireBeforeRollback);
}

void TestPropertyFailureObserverResults(
	FAutomationTestBase& Test,
	const FObservedSeedSet& Observed)
{
	Test.TestEqual(
		TEXT("property failure has two observed actions"),
		Observed.Observers.Num(), 2);
	if (Observed.Observers.Num() != 2)
	{
		return;
	}
	Test.TestEqual(TEXT("first component action was attempted once"),
		Observed.Observers[0]->RollbackCount, 1);
	Test.TestEqual(TEXT("second component action was attempted once"),
		Observed.Observers[1]->RollbackCount, 1);
	Test.TestTrue(TEXT("both wrappers return one cached provider result"),
		HaveEqualValidationResults(
			Observed.Observers[0]->LastResult,
			Observed.Observers[1]->LastResult));
	Test.TestTrue(TEXT("provider failure is exact"),
		HasDiagnostic(
			Observed.Observers[0]->LastResult.Diagnostics,
			TEXT("PropertyPreimage.AccessChanged")));
}

void TestCrossLayerPropertyRestoreFailure(FAutomationTestBase& Test)
{
	TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	FCrossLayerHandleResolver Handles;
	FScopedPrimaryAssetLabelFixture Fixture(*Provider, Handles);
	if (!Fixture.Initialize(Test))
	{
		return;
	}
	FPropertyFlowState Property;
	if (!PrepareAndMutatePropertyFlow(
		Test, *Provider, Handles, *Fixture.GetAsset(), Property, true))
	{
		return;
	}
	FProperty* RuntimeLabel = FindFProperty<FProperty>(
		UPrimaryAssetLabel::StaticClass(), TEXT("bIsRuntimeLabel"));
	if (!Test.TestNotNull(TEXT("failure injection property exists"),
		RuntimeLabel))
	{
		return;
	}
	RuntimeLabel->SetPropertyFlags(CPF_EditConst);
	ON_SCOPE_EXIT
	{
		RuntimeLabel->ClearPropertyFlags(CPF_EditConst);
	};
	Fixture.GetAsset()->GetOutermost()->SetDirtyFlag(false);
	const int32 AcquireBeforeRollback = Handles.AcquireCount;
	Handles.Trace.Reset();
	const FAssetDocumentContractValidationResult Rollback =
		Property.Atomic.Transaction->Rollback(TEXT("Test.CrossLayerFailure"));

	TestPropertyFailureAtomicResult(
		Test, Property, Rollback, Handles, AcquireBeforeRollback);
	TestPropertyFailureObserverResults(Test, Property.Observed);
	TestExactRollbackTrace(
		Test, Handles.Trace,
		{TEXT("rollback.package"), TEXT("rollback.uobject")});
}

struct FCombinedFlowState
{
	FInvocationContext Context;
	FString Name = MakeCrossLayerAssetName(TEXT("DA_CrossLayerCombined"));
	FAssetDocumentEditorCutoverMutationIntent Intent =
		MakeCreateIntent(Context, Name, true);
	FAssetDocumentEditorCutoverTransactionPreparation Preparation;
	FObservedSeedSet Observed;
	FAtomicFlow Atomic;
	TArray<TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>> Captures;
};

bool PrepareCombinedFlow(
	FAutomationTestBase& Test,
	FCrossLayerHandleResolver& Handles,
	FCombinedFlowState& Out)
{
	Out.Preparation =
		PrepareAssetDocumentEditorCutoverTransaction(Out.Intent);
	const bool bPrepared = Test.TestTrue(
		TEXT("combined preparation succeeds"), Out.Preparation.bPrepared);
	const bool bExactCount = Test.TestEqual(
		TEXT("combined preparation has exact three aliases"),
		Out.Preparation.RollbackSeeds.Num(), 3);
	if (!bPrepared || !bExactCount)
	{
		return false;
	}
	Test.TestEqual(TEXT("combined created component"),
		FString(Out.Preparation.RollbackSeeds[0]->
			GetRollbackComponentId()),
		FString(TEXT("editor.asset.rollback_created.v1")));
	Test.TestEqual(TEXT("combined uobject component"),
		FString(Out.Preparation.RollbackSeeds[1]->
			GetRollbackComponentId()),
		FString(TEXT("rollback.uobject")));
	Test.TestEqual(TEXT("combined package component"),
		FString(Out.Preparation.RollbackSeeds[2]->
			GetRollbackComponentId()),
		FString(TEXT("rollback.package")));
	Out.Observed = ObserveSeeds(Out.Preparation, Handles.Trace);
	return StartAtomicFlow(
			Test, Out.Context, Out.Preparation, Handles,
			Out.Observed, Out.Atomic)
		&& OpenAllCaptures(
			Test, Out.Observed, Out.Atomic.Transaction->GetTransactionId(),
			Out.Captures);
}

bool MutateCombinedFlow(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	FCrossLayerHandleResolver& Handles,
	FCombinedFlowState& Combined)
{
	const FInvocationContext TransactionContext =
		MakeTransactionContext(*Combined.Atomic.Transaction);
	const FCreatedAssetHandles Created = CreatePrimaryAssetLabel(
		Test, Provider, Handles, TransactionContext, Combined.Name,
		*Combined.Atomic.Transaction);
	if (!Created.bValid)
	{
		return false;
	}
	const FExistingAssetHandles Existing = ResolveExistingAsset(
		Test, Provider, Handles, TransactionContext,
		Combined.Intent.CanonicalObjectPath);
	if (!Existing.bValid)
	{
		return false;
	}
	if (!WritePropertyBatch(
		Test, Provider, Handles, TransactionContext, Existing,
		*Combined.Atomic.Transaction, true, true))
	{
		return false;
	}
	CloseAllCaptures(Combined.Captures);
	return true;
}

void TestCombinedObserverResults(
	FAutomationTestBase& Test,
	const FObservedSeedSet& Observed)
{
	Test.TestEqual(
		TEXT("combined rollback has three observed actions"),
		Observed.Observers.Num(), 3);
	if (Observed.Observers.Num() != 3)
	{
		return;
	}
	Test.TestEqual(TEXT("created action runs once"),
		Observed.Observers[0]->RollbackCount, 1);
	Test.TestEqual(TEXT("uobject action runs once"),
		Observed.Observers[1]->RollbackCount, 1);
	Test.TestEqual(TEXT("package action runs once"),
		Observed.Observers[2]->RollbackCount, 1);
	Test.TestTrue(TEXT("created and uobject aliases share terminal"),
		HaveEqualValidationResults(
			Observed.Observers[0]->LastResult,
			Observed.Observers[1]->LastResult));
	Test.TestTrue(TEXT("uobject and package aliases share terminal"),
		HaveEqualValidationResults(
			Observed.Observers[1]->LastResult,
			Observed.Observers[2]->LastResult));
}

void TestCombinedRollbackResult(
	FAutomationTestBase& Test,
	const FCombinedFlowState& Combined,
	const FAssetDocumentContractValidationResult& Rollback,
	const FCrossLayerHandleResolver& Handles,
	int32 AcquireBeforeRollback)
{
	Test.TestTrue(TEXT("combined rollback succeeds"), Rollback.bValid);
	Test.TestEqual(TEXT("factory and property each enlist once"),
		Combined.Atomic.Transaction->EnlistCount, 2);
	Test.TestEqual(TEXT("combined rollback acquires no handles"),
		Handles.AcquireCount, AcquireBeforeRollback);
	Test.TestNull(TEXT("combined rollback deletes target once"),
		FSoftObjectPath(Combined.Intent.CanonicalObjectPath).ResolveObject());
	TestCombinedObserverResults(Test, Combined.Observed);
	TestExactRollbackTrace(
		Test, Handles.Trace,
		{TEXT("rollback.package"), TEXT("rollback.uobject"),
			TEXT("editor.asset.rollback_created.v1")});
}

void TestCrossLayerCombinedRollback(FAutomationTestBase& Test)
{
	TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	FCrossLayerHandleResolver Handles;
	FCombinedFlowState Combined;
	if (!PrepareCombinedFlow(Test, Handles, Combined))
	{
		return;
	}
	if (!MutateCombinedFlow(Test, *Provider, Handles, Combined))
	{
		return;
	}
	const int32 AcquireBeforeRollback = Handles.AcquireCount;
	Handles.Trace.Reset();
	const FAssetDocumentContractValidationResult Rollback =
		Combined.Atomic.Transaction->Rollback(TEXT("Test.CrossLayerCombined"));

	TestCombinedRollbackResult(
		Test, Combined, Rollback, Handles, AcquireBeforeRollback);
}

void TestEmptyAndOrderedMapping(FAutomationTestBase& Test)
{
	const TArray<TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>> Empty;
	Test.TestEqual(
		TEXT("empty seeds map to empty actions"),
		AdaptAssetDocumentEditorRollbackSeeds(Empty).Num(),
		0);

	const TSharedRef<FTestRollbackSeed> First = MakeShared<FTestRollbackSeed>(
		TEXT("rollback.first"),
		MakeSeedResult(true));
	const TSharedRef<FTestRollbackSeed> Second = MakeShared<FTestRollbackSeed>(
		TEXT("rollback.second"),
		MakeSeedResult(true));
	const TArray<TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>> Seeds{
		First,
		Second};
	const TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions =
		AdaptAssetDocumentEditorRollbackSeeds(Seeds);
	Test.TestEqual(TEXT("adapter preserves count"), Actions.Num(), 2);
	Test.TestEqual(
		TEXT("adapter preserves first component id"),
		FString(Actions[0]->GetRollbackComponentId()),
		FString(TEXT("rollback.first")));
	Test.TestEqual(
		TEXT("adapter preserves second component id"),
		FString(Actions[1]->GetRollbackComponentId()),
		FString(TEXT("rollback.second")));

	First->ComponentId = TEXT("rollback.changed");
	Test.TestEqual(
		TEXT("action freezes component id"),
		FString(Actions[0]->GetRollbackComponentId()),
		FString(TEXT("rollback.first")));
}

void TestResultForwardingAndOnceOnly(FAutomationTestBase& Test)
{
	const TSharedRef<FTestRollbackSeed> Seed = MakeShared<FTestRollbackSeed>(
		TEXT("rollback.uobject"),
		MakeSeedResult(true));
	const TArray<TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>> Seeds{
		Seed};
	const TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions =
		AdaptAssetDocumentEditorRollbackSeeds(Seeds);
	const FAssetDocumentContractValidationResult First =
		Actions[0]->Rollback(TEXT("Test.Adapter"));
	Test.TestTrue(TEXT("provider success is returned verbatim"), First.bValid);
	Test.TestEqual(TEXT("provider called once"), Seed->RollbackCount, 1);
	Test.TestEqual(
		TEXT("failure code forwarded exactly"),
		Seed->LastFailureCode,
		FString(TEXT("Test.Adapter")));
	Test.TestEqual(
		TEXT("adapter does not open mutation capture"),
		Seed->OpenMutationCaptureCount,
		0);

	const FAssetDocumentContractValidationResult Second =
		Actions[0]->Rollback(TEXT("Test.Second"));
	Test.TestTrue(
		TEXT("second direct call fails with exact terminal"),
		HasExactDiagnostic(
			Second,
			TEXT("/EditorRollbackActionAdapter"),
			TEXT("EditorRollbackActionAdapter.AlreadyConsumed")));
	Test.TestEqual(TEXT("second call does not reach provider"), Seed->RollbackCount, 1);
}

void TestFailureForwardingAndConsumptionOrder(FAutomationTestBase& Test)
{
	const TSharedRef<FTestRollbackSeed> Seed = MakeShared<FTestRollbackSeed>(
		TEXT("rollback.package"),
		MakeSeedResult(false, TEXT("TestSeed.RestoreFailed")));
	const TArray<TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>> Seeds{
		Seed};
	const TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions =
		AdaptAssetDocumentEditorRollbackSeeds(Seeds);
	Seed->ActionDuringRollback = &Actions[0].Get();
	const FAssetDocumentContractValidationResult Result =
		Actions[0]->Rollback(TEXT("Test.ProviderFailure"));

	Test.TestTrue(
		TEXT("provider failure is returned verbatim"),
		HasExactDiagnostic(
			Result,
			TEXT("/TestSeed"),
			TEXT("TestSeed.RestoreFailed")));
	Test.TestEqual(
		TEXT("provider failure severity is preserved"),
		Result.Diagnostics[0].Severity,
		EAssetDocumentDiagnosticSeverity::Warning);
	Test.TestTrue(
		TEXT("action is consumed before provider delegation"),
		Seed->bProbedConsumptionBeforeDelegate
			&& HasExactDiagnostic(
				Seed->ReentrantResult,
				TEXT("/EditorRollbackActionAdapter"),
				TEXT("EditorRollbackActionAdapter.AlreadyConsumed")));
	Test.TestEqual(TEXT("reentrant call does not reach provider"), Seed->RollbackCount, 1);
}

// Under Core's package snapshot the typed replay is skipped: the action
// succeeds once without reaching the provider, and stays once-only.
void TestPackageSnapshotSkipsReplay(FAutomationTestBase& Test)
{
	const TSharedRef<FTestRollbackSeed> Seed = MakeShared<FTestRollbackSeed>(
		TEXT("rollback.uobject"),
		MakeSeedResult(false, TEXT("TestSeed.RestoreFailed")));
	const TArray<TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>> Seeds{
		Seed};
	const TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions =
		AdaptAssetDocumentEditorRollbackSeeds(Seeds, true);
	Test.TestEqual(
		TEXT("snapshot-covered action keeps component id"),
		FString(Actions[0]->GetRollbackComponentId()),
		FString(TEXT("rollback.uobject")));
	const FAssetDocumentContractValidationResult First =
		Actions[0]->Rollback(TEXT("Test.SnapshotCovered"));
	Test.TestTrue(TEXT("snapshot-covered action succeeds"), First.bValid && First.Diagnostics.IsEmpty());
	Test.TestEqual(TEXT("snapshot-covered action does not replay"), Seed->RollbackCount, 0);
	const FAssetDocumentContractValidationResult Second =
		Actions[0]->Rollback(TEXT("Test.SnapshotCovered"));
	Test.TestTrue(
		TEXT("snapshot-covered action is once-only"),
		HasExactDiagnostic(
			Second,
			TEXT("/EditorRollbackActionAdapter"),
			TEXT("EditorRollbackActionAdapter.AlreadyConsumed")));
}

void TestActionKeepsSeedAlive(FAutomationTestBase& Test)
{
	TWeakPtr<FTestRollbackSeed> WeakSeed;
	TSharedPtr<IAssetDocumentRollbackAction> Action;
	{
		const TSharedRef<FTestRollbackSeed> Seed =
			MakeShared<FTestRollbackSeed>(
				TEXT("rollback.uobject"),
				MakeSeedResult(true));
		WeakSeed = Seed;
		const TArray<TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>> Seeds{
			Seed};
		const TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions =
			AdaptAssetDocumentEditorRollbackSeeds(Seeds);
		Action = Actions[0];
	}
	Test.TestTrue(TEXT("action keeps opaque seed alive"), WeakSeed.IsValid());
	Action.Reset();
	Test.TestFalse(TEXT("seed releases with action"), WeakSeed.IsValid());
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentEditorRollbackActionAdapterTest,
	"UEAssetDoc.Integration.EditorRollbackActionAdapter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorRollbackActionAdapterTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorRollbackActionAdapterTests;
	TestEmptyAndOrderedMapping(*this);
	TestResultForwardingAndOnceOnly(*this);
	TestFailureForwardingAndConsumptionOrder(*this);
	TestActionKeepsSeedAlive(*this);
	TestPackageSnapshotSkipsReplay(*this);
	TestCrossLayerPropertyRestoreSuccess(*this);
	TestDynamicPropertyRestoreExactState(*this);
	TestCrossLayerPropertyRestoreFailure(*this);
	TestCrossLayerCombinedRollback(*this);
	return true;
}
#endif
