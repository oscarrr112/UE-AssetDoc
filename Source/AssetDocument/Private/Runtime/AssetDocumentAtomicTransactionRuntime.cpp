// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Runtime/AssetDocumentAtomicTransactionRuntime.h"

#include "AssetDocumentRollbackAction.h"

namespace
{
FAssetDocumentContractValidationResult MakeAtomicResult(
	bool bValid,
	const TCHAR* Code = TEXT(""),
	const TCHAR* Message = TEXT(""))
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = bValid;
	if (!bValid)
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/AtomicTransaction");
		Diagnostic.Code = Code;
		Diagnostic.Message = Message;
	}
	return Result;
}

bool FootprintsMatch(
	const FAssetDocumentMutationFootprint& Left,
	const FAssetDocumentMutationFootprint& Right)
{
	return Left.Kind == Right.Kind
		&& Left.StableId == Right.StableId
		&& Left.RollbackComponentId == Right.RollbackComponentId;
}

bool HasDuplicateStableIds(TConstArrayView<FAssetDocumentMutationFootprint> Footprint)
{
	TSet<FString> StableIds;
	for (const FAssetDocumentMutationFootprint& Item : Footprint)
	{
		if (StableIds.Contains(Item.StableId))
		{
			return true;
		}
		StableIds.Add(Item.StableId);
	}
	return false;
}

FAssetDocumentContractValidationResult ValidateInitialRollbackActions(
	TConstArrayView<TSharedRef<IAssetDocumentRollbackAction>> Actions)
{
	TSet<FString> ComponentIds;
	for (const TSharedRef<IAssetDocumentRollbackAction>& Action : Actions)
	{
		const FString ComponentId(Action->GetRollbackComponentId());
		if (ComponentId.IsEmpty())
		{
			return MakeAtomicResult(
				false,
				TEXT("Atomic.RollbackActionEmptyComponentId"),
				TEXT("Rollback actions require a non-empty component identity."));
		}
		if (ComponentIds.Contains(ComponentId))
		{
			return MakeAtomicResult(
				false,
				TEXT("Atomic.RollbackActionDuplicateComponentId"),
				TEXT("Only one rollback action may cover a component."));
		}
		ComponentIds.Add(ComponentId);
	}
	return MakeAtomicResult(true);
}
}

FAssetDocumentAtomicTransactionRuntime::FRollbackTarget::FRollbackTarget(
	FString InComponentId,
	TSharedRef<IAssetDocumentRollbackAction> InAction)
	: ComponentId(MoveTemp(InComponentId))
	, Target(
		TInPlaceType<TSharedRef<IAssetDocumentRollbackAction>>(),
		MoveTemp(InAction))
{
}

FAssetDocumentAtomicTransactionRuntime::FRollbackTarget::FRollbackTarget(
	FString InComponentId,
	IAssetDocumentRollbackParticipant* InParticipant)
	: ComponentId(MoveTemp(InComponentId))
	, Target(
		TInPlaceType<IAssetDocumentRollbackParticipant*>(),
		InParticipant)
{
	check(InParticipant != nullptr);
}

FAssetDocumentContractValidationResult
FAssetDocumentAtomicTransactionRuntime::FRollbackTarget::Rollback(
	FStringView StableFailureCode) const
{
	if (Target.IsType<TSharedRef<IAssetDocumentRollbackAction>>())
	{
		return Target.Get<TSharedRef<IAssetDocumentRollbackAction>>()->Rollback(
			StableFailureCode);
	}
	return Target.Get<IAssetDocumentRollbackParticipant*>()->Rollback(
		StableFailureCode);
}

FAssetDocumentAtomicTransactionRuntime::FAssetDocumentAtomicTransactionRuntime(
	const FAssetDocumentPreflightReceipt& Receipt,
	IAssetDocumentHandleResolver& InHandleResolver,
	IAssetDocumentRollbackParticipantRegistry& InParticipantRegistry,
	FAssetDocumentAtomicDiagnosticSink InDiagnosticSink,
	TArray<TSharedRef<IAssetDocumentRollbackAction>> InInitialActions)
	: PreflightReceipt(Receipt)
	, HandleResolver(InHandleResolver)
	, ParticipantRegistry(InParticipantRegistry)
	, DiagnosticSink(MoveTemp(InDiagnosticSink))
{
	MutationToken.RequestId = Receipt.RequestId;
	MutationToken.TransactionId = FGuid::NewGuid();
	MutationToken.RuntimeRevision = Receipt.RuntimeRevision;
	for (TSharedRef<IAssetDocumentRollbackAction>& Action : InInitialActions)
	{
		InitialComponentIds.Add(FString(Action->GetRollbackComponentId()));
		RollbackActions.Add(
			FString(Action->GetRollbackComponentId()),
			MoveTemp(Action));
	}
}

FAssetDocumentAtomicTransactionRuntime::~FAssetDocumentAtomicTransactionRuntime()
{
	if (!IsTerminal())
	{
		RollbackLive(TEXT("Atomic.ScopeExit"));
	}
}

FGuid FAssetDocumentAtomicTransactionRuntime::GetTransactionId() const
{
	return MutationToken.TransactionId;
}

EAssetDocumentAtomicState FAssetDocumentAtomicTransactionRuntime::GetState() const
{
	return State;
}

const FAssetDocumentMutationToken& FAssetDocumentAtomicTransactionRuntime::GetMutationToken() const
{
	return MutationToken;
}

FAssetDocumentContractValidationResult
FAssetDocumentAtomicTransactionRuntime::EnlistRollbackAction(
	TSharedRef<IAssetDocumentRollbackAction> Action)
{
	if (!IsMutableState())
	{
		return Reject(
			EAssetDocumentRuntimeRequestPhase::Apply,
			TEXT("Atomic.RollbackActionInvalidState"),
			TEXT("Rollback actions require a live or mutating transaction."));
	}
	const FString ComponentId(Action->GetRollbackComponentId());
	if (ComponentId.IsEmpty())
	{
		return Reject(
			EAssetDocumentRuntimeRequestPhase::Apply,
			TEXT("Atomic.RollbackActionEmptyComponentId"),
			TEXT("Rollback actions require a non-empty component identity."));
	}
	if (!HasDeclaredComponent(ComponentId))
	{
		return Reject(
			EAssetDocumentRuntimeRequestPhase::Apply,
			TEXT("Atomic.RollbackActionComponentNotDeclared"),
			TEXT("Rollback action component was not declared by preflight."));
	}
	if (EnlistedComponentIds.Contains(ComponentId))
	{
		return Reject(
			EAssetDocumentRuntimeRequestPhase::Apply,
			TEXT("Atomic.RollbackActionLateEnlistment"),
			TEXT("Rollback action must be enlisted before its first footprint."));
	}
	if (RollbackActions.Contains(ComponentId)
		|| (PostRollbackFinalizer.IsValid()
			&& ComponentId == PostRollbackFinalizerComponentId)
		|| ParticipantRegistry.Resolve(ComponentId) != nullptr)
	{
		return Reject(
			EAssetDocumentRuntimeRequestPhase::Apply,
			TEXT("Atomic.RollbackActionDuplicateComponentId"),
			TEXT("Only one rollback target may cover a component."));
	}
	RollbackActions.Add(ComponentId, MoveTemp(Action));
	return MakeAtomicResult(true);
}

FAssetDocumentContractValidationResult
FAssetDocumentAtomicTransactionRuntime::EnlistPostRollbackFinalizer(
	TSharedRef<IAssetDocumentPostRollbackFinalizer> Finalizer)
{
	if (!IsMutableState())
	{
		return Reject(
			EAssetDocumentRuntimeRequestPhase::Apply,
			TEXT("Atomic.PostRollbackFinalizerInvalidState"),
			TEXT("Post-rollback finalizers require a live or mutating transaction."));
	}
	const FString ComponentId(Finalizer->GetRollbackComponentId());
	if (ComponentId.IsEmpty())
	{
		return Reject(
			EAssetDocumentRuntimeRequestPhase::Apply,
			TEXT("Atomic.PostRollbackFinalizerEmptyComponentId"),
			TEXT("Post-rollback finalizers require a non-empty component identity."));
	}
	if (!HasDeclaredComponent(ComponentId))
	{
		return Reject(
			EAssetDocumentRuntimeRequestPhase::Apply,
			TEXT("Atomic.PostRollbackFinalizerComponentNotDeclared"),
			TEXT("Post-rollback finalizer component was not declared by preflight."));
	}
	if (EnlistedComponentIds.Contains(ComponentId))
	{
		return Reject(
			EAssetDocumentRuntimeRequestPhase::Apply,
			TEXT("Atomic.PostRollbackFinalizerLateEnlistment"),
			TEXT("Post-rollback finalizer must be enlisted before its first footprint."));
	}
	if (PostRollbackFinalizer.IsValid()
		|| RollbackActions.Contains(ComponentId)
		|| ParticipantRegistry.Resolve(ComponentId) != nullptr)
	{
		return Reject(
			EAssetDocumentRuntimeRequestPhase::Apply,
			TEXT("Atomic.PostRollbackFinalizerDuplicateComponentId"),
			TEXT("Only one rollback target or post-rollback finalizer may cover a component."));
	}
	PostRollbackFinalizer = Finalizer;
	PostRollbackFinalizerComponentId = ComponentId;
	return MakeAtomicResult(true);
}

FAssetDocumentContractValidationResult FAssetDocumentAtomicTransactionRuntime::EnlistFootprint(
	TConstArrayView<FAssetDocumentMutationFootprint> Footprint)
{
	const FAssetDocumentContractValidationResult Request = ValidateEnlistmentRequest(Footprint);
	if (!Request.bValid)
	{
		return Request;
	}
	if (HasDuplicateStableIds(Footprint))
	{
		return Reject(EAssetDocumentRuntimeRequestPhase::Apply, TEXT("Atomic.FootprintAlreadyEnlisted"),
			TEXT("Mutation footprint was already enlisted."));
	}
	const FAssetDocumentContractValidationResult Items = ValidateEnlistmentItems(Footprint);
	if (!Items.bValid)
	{
		return Items;
	}
	TArray<FRollbackTarget> NewTargets;
	const FAssetDocumentContractValidationResult Coverage =
		ResolveRollbackTargets(Footprint, NewTargets);
	if (!Coverage.bValid)
	{
		return Coverage;
	}
	for (const FAssetDocumentMutationFootprint& Item : Footprint)
	{
		EnlistItem(Item);
		if (PostRollbackFinalizer.IsValid()
			&& Item.RollbackComponentId == PostRollbackFinalizerComponentId)
		{
			EnlistedComponentIds.Add(PostRollbackFinalizerComponentId);
			bPostRollbackFinalizerActive = true;
		}
	}
	for (FRollbackTarget& Target : NewTargets)
	{
		EnlistedComponentIds.Add(Target.ComponentId);
		RollbackTargets.Add(MoveTemp(Target));
	}
	State = EAssetDocumentAtomicState::Mutating;
	return MakeAtomicResult(true);
}

FAssetDocumentContractValidationResult FAssetDocumentAtomicTransactionRuntime::ValidateEnlistmentRequest(
	TConstArrayView<FAssetDocumentMutationFootprint> Footprint) const
{
	if (State == EAssetDocumentAtomicState::Poisoned)
	{
		return Reject(EAssetDocumentRuntimeRequestPhase::Apply, TEXT("Atomic.Poisoned"), TEXT("A poisoned transaction cannot mutate."));
	}
	if (!IsMutableState())
	{
		return Reject(EAssetDocumentRuntimeRequestPhase::Apply, TEXT("Atomic.InvalidStateTransition"), TEXT("Footprint enlistment requires a live or mutating transaction."));
	}
	if (Footprint.IsEmpty())
	{
		return Reject(EAssetDocumentRuntimeRequestPhase::Apply, TEXT("Atomic.FootprintNotDeclared"), TEXT("Footprint enlistment cannot be empty."));
	}
	return MakeAtomicResult(true);
}

FAssetDocumentContractValidationResult FAssetDocumentAtomicTransactionRuntime::ValidateEnlistmentItems(
	TConstArrayView<FAssetDocumentMutationFootprint> Footprint) const
{
	for (const FAssetDocumentMutationFootprint& Item : Footprint)
	{
		const FAssetDocumentMutationFootprint* Declared = FindDeclared(Item.StableId);
		if (Declared == nullptr)
		{
			return Reject(EAssetDocumentRuntimeRequestPhase::Apply, TEXT("Atomic.FootprintNotDeclared"), TEXT("Mutation footprint was not declared by preflight."));
		}
		if (!FootprintsMatch(*Declared, Item))
		{
			return Reject(EAssetDocumentRuntimeRequestPhase::Apply, TEXT("Atomic.FootprintNotDeclared"), TEXT("Mutation footprint was not declared by preflight."));
		}
		if (EnlistedStableIds.Contains(Item.StableId))
		{
			return Reject(EAssetDocumentRuntimeRequestPhase::Apply, TEXT("Atomic.FootprintAlreadyEnlisted"), TEXT("Mutation footprint was already enlisted."));
		}
	}
	return MakeAtomicResult(true);
}

FAssetDocumentContractValidationResult
FAssetDocumentAtomicTransactionRuntime::ResolveRollbackTargets(
	TConstArrayView<FAssetDocumentMutationFootprint> Footprint,
	TArray<FRollbackTarget>& OutTargets) const
{
	TSet<FString> ResolvedComponentIds = EnlistedComponentIds;
	for (const FAssetDocumentMutationFootprint& Item : Footprint)
	{
		if (ResolvedComponentIds.Contains(Item.RollbackComponentId))
		{
			continue;
		}
		if (PostRollbackFinalizer.IsValid()
			&& Item.RollbackComponentId == PostRollbackFinalizerComponentId)
		{
			ResolvedComponentIds.Add(Item.RollbackComponentId);
			continue;
		}
		if (const TSharedRef<IAssetDocumentRollbackAction>* Action =
			RollbackActions.Find(Item.RollbackComponentId))
		{
			OutTargets.Emplace(Item.RollbackComponentId, *Action);
			ResolvedComponentIds.Add(Item.RollbackComponentId);
			continue;
		}
		if (IAssetDocumentRollbackParticipant* Participant =
			ParticipantRegistry.Resolve(Item.RollbackComponentId))
		{
			OutTargets.Emplace(Item.RollbackComponentId, Participant);
			ResolvedComponentIds.Add(Item.RollbackComponentId);
			continue;
		}
		OutTargets.Reset();
		return Reject(
			EAssetDocumentRuntimeRequestPhase::Apply,
			TEXT("Atomic.IncompleteRollbackCoverage"),
			TEXT("Every enlisted footprint requires current rollback coverage."));
	}
	return MakeAtomicResult(true);
}

void FAssetDocumentAtomicTransactionRuntime::EnlistItem(const FAssetDocumentMutationFootprint& Item)
{
	EnlistedStableIds.Add(Item.StableId);
}

bool FAssetDocumentAtomicTransactionRuntime::IsMutableState() const
{
	if (State == EAssetDocumentAtomicState::Live)
	{
		return true;
	}
	return State == EAssetDocumentAtomicState::Mutating;
}

bool FAssetDocumentAtomicTransactionRuntime::HasDeclaredComponent(
	FStringView ComponentId) const
{
	return PreflightReceipt.CompleteFootprint.ContainsByPredicate(
		[ComponentId](const FAssetDocumentMutationFootprint& Item)
		{
			return Item.RollbackComponentId == ComponentId;
		});
}

FAssetDocumentContractValidationResult FAssetDocumentAtomicTransactionRuntime::MarkReadyToCommit()
{
	if (State == EAssetDocumentAtomicState::Poisoned)
	{
		return Reject(EAssetDocumentRuntimeRequestPhase::Persist, TEXT("Atomic.Poisoned"), TEXT("A poisoned transaction cannot advance."));
	}
	if (State != EAssetDocumentAtomicState::Mutating)
	{
		return Reject(EAssetDocumentRuntimeRequestPhase::Persist, TEXT("Atomic.InvalidStateTransition"), TEXT("Only a mutating transaction may become ready to commit."));
	}
	if (EnlistedStableIds.Num() != PreflightReceipt.CompleteFootprint.Num())
	{
		return Reject(EAssetDocumentRuntimeRequestPhase::Persist, TEXT("Atomic.IncompleteFootprint"), TEXT("The complete preflight footprint must be enlisted before commit."));
	}
	State = EAssetDocumentAtomicState::ReadyToCommit;
	return MakeAtomicResult(true);
}

FAssetDocumentContractValidationResult FAssetDocumentAtomicTransactionRuntime::Commit(
	const FAssetDocumentDurableCompletionReceipt& Receipt)
{
	if (IsTerminal())
	{
		return Reject(EAssetDocumentRuntimeRequestPhase::Complete, TEXT("Atomic.TerminalAlreadyChosen"), TEXT("The atomic transaction already selected a terminal state."));
	}
	if (State != EAssetDocumentAtomicState::ReadyToCommit)
	{
		return Reject(EAssetDocumentRuntimeRequestPhase::Complete, TEXT("Atomic.InvalidStateTransition"), TEXT("Only a ready transaction may commit."));
	}
	if (!HasMatchingReceiptIdentity(Receipt))
	{
		return Reject(EAssetDocumentRuntimeRequestPhase::Complete, TEXT("Atomic.InvalidDurableCompletion"), TEXT("Durable completion identity and digests must be complete before commit."));
	}
	if (!HasDurableReceiptPayload(Receipt))
	{
		return Reject(EAssetDocumentRuntimeRequestPhase::Complete, TEXT("Atomic.InvalidDurableCompletion"), TEXT("Durable completion identity and digests must be complete before commit."));
	}
	Invalidate(EAssetDocumentHandleInvalidationReason::Commit);
	State = EAssetDocumentAtomicState::Committed;
	ReleaseRollbackActionOwnership();
	return MakeAtomicResult(true);
}

bool FAssetDocumentAtomicTransactionRuntime::HasMatchingReceiptIdentity(
	const FAssetDocumentDurableCompletionReceipt& Receipt) const
{
	if (Receipt.RequestId != MutationToken.RequestId)
	{
		return false;
	}
	return Receipt.TransactionId == MutationToken.TransactionId;
}

bool FAssetDocumentAtomicTransactionRuntime::HasDurableReceiptPayload(
	const FAssetDocumentDurableCompletionReceipt& Receipt) const
{
	if (Receipt.JournalId.IsEmpty())
	{
		return false;
	}
	if (Receipt.PackageContentDigest.IsEmpty())
	{
		return false;
	}
	if (Receipt.SidecarContentDigest.IsEmpty())
	{
		return false;
	}
	return Receipt.bSyncStateAdvanced;
}

FAssetDocumentContractValidationResult FAssetDocumentAtomicTransactionRuntime::Rollback(FStringView StableFailureCode)
{
	if (IsTerminal())
	{
		return Reject(EAssetDocumentRuntimeRequestPhase::Failed, TEXT("Atomic.TerminalAlreadyChosen"), TEXT("The atomic transaction already selected a terminal state."));
	}
	if (StableFailureCode == TEXTVIEW("Atomic.CancelledAtBoundary"))
	{
		Reject(EAssetDocumentRuntimeRequestPhase::Apply, TEXT("Atomic.CancelledAtBoundary"),
			TEXT("The request was cancelled at an atomic section boundary."));
	}
	return RollbackLive(StableFailureCode);
}

FAssetDocumentContractValidationResult FAssetDocumentAtomicTransactionRuntime::Reject(
	EAssetDocumentRuntimeRequestPhase Phase,
	const TCHAR* Code,
	const TCHAR* Message) const
{
	FAssetDocumentContractValidationResult Result = MakeAtomicResult(false, Code, Message);
	if (DiagnosticSink)
	{
		DiagnosticSink(Phase, Result.Diagnostics[0]);
	}
	return Result;
}

void FAssetDocumentAtomicTransactionRuntime::Invalidate(EAssetDocumentHandleInvalidationReason Reason)
{
	FAssetDocumentHandleInvalidation Event;
	Event.Reason = Reason;
	Event.RequestId = MutationToken.RequestId;
	Event.TransactionId = MutationToken.TransactionId;
	Event.RuntimeRevision = MutationToken.RuntimeRevision;
	HandleResolver.Invalidate(Event);
}

void FAssetDocumentAtomicTransactionRuntime::ReleaseRollbackActionOwnership()
{
	RollbackTargets.Reset();
	RollbackActions.Reset();
	PostRollbackFinalizer.Reset();
	PostRollbackFinalizerComponentId.Reset();
	bPostRollbackFinalizerActive = false;
}

FAssetDocumentContractValidationResult FAssetDocumentAtomicTransactionRuntime::RollbackLive(
	FStringView StableFailureCode)
{
	Invalidate(EAssetDocumentHandleInvalidationReason::Rollback);
	bool bFailed = false;
	TArray<FAssetDocumentRuntimeDiagnostic> RollbackDiagnostics;
	TSet<FString> Restored;
	while (!RollbackTargets.IsEmpty())
	{
		const FString ComponentId = RollbackTargets.Last().ComponentId;
		if (InitialComponentIds.Contains(ComponentId))
		{
			// An initial action (a rollback seed) is the outermost layer and may delete the asset the request
			// created. Provider actions that already restored, or never covered a footprint, are done: release
			// them first so the objects they retain cannot keep that asset alive. Until then every action lives,
			// since later rollback actions may still observe earlier ones.
			for (auto It = RollbackActions.CreateIterator(); It; ++It)
			{
				if (!InitialComponentIds.Contains(It.Key())
					&& (Restored.Contains(It.Key()) || !EnlistedComponentIds.Contains(It.Key())))
				{
					It.RemoveCurrent();
				}
			}
		}
		const FAssetDocumentContractValidationResult Result =
			RollbackTargets.Last().Rollback(StableFailureCode);
		RollbackDiagnostics.Append(Result.Diagnostics);
		if (!Result.bValid)
		{
			bFailed = true;
			Reject(EAssetDocumentRuntimeRequestPhase::Failed, TEXT("Atomic.Poisoned.ComponentFailed"), TEXT("A rollback participant failed while restoring its declared footprint."));
		}
		Restored.Add(ComponentId);
		RollbackTargets.Pop(EAllowShrinking::No);
	}
	if (bPostRollbackFinalizerActive && PostRollbackFinalizer.IsValid())
	{
		const FAssetDocumentContractValidationResult Result =
			PostRollbackFinalizer->FinalizePostRollback(StableFailureCode);
		RollbackDiagnostics.Append(Result.Diagnostics);
		if (!Result.bValid)
		{
			bFailed = true;
			Reject(
				EAssetDocumentRuntimeRequestPhase::Failed,
				TEXT("Atomic.Poisoned.PostRollbackFinalizerFailed"),
				TEXT("The post-rollback finalizer failed after ordinary rollback targets."));
		}
	}
	State = bFailed ? EAssetDocumentAtomicState::Poisoned : EAssetDocumentAtomicState::RolledBack;
	ReleaseRollbackActionOwnership();
	FAssetDocumentContractValidationResult Result = bFailed
		? Reject(EAssetDocumentRuntimeRequestPhase::Failed, TEXT("Atomic.Poisoned.RollbackFailed"), TEXT("Atomic rollback could not restore the complete footprint."))
		: MakeAtomicResult(true);
	// Keep the stable aggregate first; callers forward returned details to their sink.
	Result.Diagnostics.Append(MoveTemp(RollbackDiagnostics));
	return Result;
}

const FAssetDocumentMutationFootprint* FAssetDocumentAtomicTransactionRuntime::FindDeclared(
	FStringView StableId) const
{
	for (const FAssetDocumentMutationFootprint& Item : PreflightReceipt.CompleteFootprint)
	{
		if (Item.StableId == StableId)
		{
			return &Item;
		}
	}
	return nullptr;
}

bool FAssetDocumentAtomicTransactionRuntime::IsTerminal() const
{
	return State == EAssetDocumentAtomicState::Committed
		|| State == EAssetDocumentAtomicState::RolledBack
		|| State == EAssetDocumentAtomicState::Poisoned;
}

FAssetDocumentAtomicTransactionFactoryRuntime::FAssetDocumentAtomicTransactionFactoryRuntime(
	IAssetDocumentHandleResolver& InHandleResolver,
	IAssetDocumentRollbackParticipantRegistry& InParticipantRegistry,
	FAssetDocumentAtomicDiagnosticSink InDiagnosticSink,
	TArray<TSharedRef<IAssetDocumentRollbackAction>> InInitialActions)
	: HandleResolver(InHandleResolver)
	, ParticipantRegistry(InParticipantRegistry)
	, DiagnosticSink(MoveTemp(InDiagnosticSink))
	, InitialActions(MoveTemp(InInitialActions))
	, InitialActionsValidation(ValidateInitialRollbackActions(InitialActions))
{
}

TUniquePtr<IAssetDocumentAtomicTransaction>
FAssetDocumentAtomicTransactionFactoryRuntime::BeginLiveAfterPreflight(
	const FAssetDocumentPreflightReceipt& Receipt,
	IAssetDocumentCancellationView& Cancellation)
{
	const FAssetDocumentContractValidationResult Validation =
		ValidateAssetDocumentPreflightReceipt(Receipt);
	if (!Validation.bValid)
	{
		return RejectBegin(Validation);
	}
	if (Cancellation.IsCancellationRequested())
	{
		const FAssetDocumentContractValidationResult Result = MakeAtomicResult(false, TEXT("Atomic.CancelledBeforeBegin"), TEXT("The request was cancelled before a live transaction began."));
		return RejectBegin(Result);
	}
	if (!InitialActionsValidation.bValid)
	{
		return RejectBegin(InitialActionsValidation);
	}
	return MakeUnique<FAssetDocumentAtomicTransactionRuntime>(
		Receipt,
		HandleResolver,
		ParticipantRegistry,
		DiagnosticSink,
		InitialActions);
}

TUniquePtr<IAssetDocumentAtomicTransaction> FAssetDocumentAtomicTransactionFactoryRuntime::RejectBegin(
	const FAssetDocumentContractValidationResult& Validation) const
{
	if (DiagnosticSink)
	{
		DiagnosticSink(EAssetDocumentRuntimeRequestPhase::Preflight, Validation.Diagnostics[0]);
	}
	return nullptr;
}
