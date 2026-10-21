// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentHandleRegistryRuntime.h"

#include "Misc/ScopeLock.h"

struct FAssetDocumentHandleLeaseRevocationState final
{
	IAssetDocumentNativeHandlePayload* Payload = nullptr;
};

namespace
{
struct FAssetDocumentNativeHandlePayloadStorage final
{
	TUniquePtr<IAssetDocumentNativeHandlePayload> Payload;
};

FAssetDocumentRuntimeDiagnostic MakeDiagnostic(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/Handle");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Diagnostic;
}

FAssetDocumentRuntimeDiagnostic MakeHandoffDiagnostic(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic = MakeDiagnostic(Code, Message);
	Diagnostic.Path = TEXT("/HandleHandoff");
	return Diagnostic;
}

FAssetDocumentContractValidationResult MakeContractFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.Diagnostics.Add(MakeDiagnostic(Code, Message));
	return Result;
}

FAssetDocumentContractValidationResult MakeHandoffFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.Diagnostics.Add(MakeHandoffDiagnostic(Code, Message));
	return Result;
}

FAssetDocumentContractValidationResult MakeContractSuccess()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

bool HasSameIdentityMetadata(
	const FAssetDocumentOpaqueHandle& Left,
	const FAssetDocumentOpaqueHandle& Right)
{
	return Left.RequestId == Right.RequestId
		&& Left.TransactionId == Right.TransactionId
		&& Left.RuntimeRevision == Right.RuntimeRevision
		&& Left.ProviderId == Right.ProviderId;
}

bool HasSameStorageMetadata(
	const FAssetDocumentOpaqueHandle& Left,
	const FAssetDocumentOpaqueHandle& Right)
{
	return Left.Slot == Right.Slot
		&& Left.Generation == Right.Generation
		&& Left.Kind == Right.Kind
		&& Left.Access == Right.Access;
}

bool IsSameHandle(
	const FAssetDocumentOpaqueHandle& Left,
	const FAssetDocumentOpaqueHandle& Right)
{
	return HasSameIdentityMetadata(Left, Right)
		&& HasSameStorageMetadata(Left, Right);
}

uint64 MakeHandleKey(const FAssetDocumentOpaqueHandle& Handle)
{
	return (static_cast<uint64>(Handle.Slot) << 32)
		| static_cast<uint64>(Handle.Generation);
}

bool IsBindingLive(const FAssetDocumentHandleResolverBinding& Binding)
{
	return Binding.RequestId.IsValid()
		&& Binding.TransactionId.IsValid()
		&& Binding.RuntimeRevision > 0
		&& !Binding.ProviderId.IsEmpty()
		&& static_cast<uint8>(Binding.Phase)
			<= static_cast<uint8>(EAssetDocumentRuntimeRequestPhase::Failed);
}

bool IsIssueAdmitted(
	const FAssetDocumentHandleResolverBinding& Binding,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access)
{
	for (const FAssetDocumentPrimitiveHandleResultSchema& Schema : Binding.AllowedResults)
	{
		if (Schema.Kind == Kind && Schema.GrantedAccess == Access)
		{
			return true;
		}
	}
	return false;
}

const FAssetDocumentPrimitiveHandleResultSchema* FindResultSchema(
	TConstArrayView<FAssetDocumentPrimitiveHandleResultSchema> Schemas,
	FStringView Name)
{
	for (const FAssetDocumentPrimitiveHandleResultSchema& Schema : Schemas)
	{
		if (Schema.Name == Name)
		{
			return &Schema;
		}
	}
	return nullptr;
}

bool IsCardinalityValid(
	EAssetDocumentHandleResultCardinality Cardinality,
	int32 Count)
{
	switch (Cardinality)
	{
	case EAssetDocumentHandleResultCardinality::ExactlyOne:
		return Count == 1;
	case EAssetDocumentHandleResultCardinality::ZeroOrOne:
		return Count <= 1;
	case EAssetDocumentHandleResultCardinality::ZeroOrMore:
		return true;
	case EAssetDocumentHandleResultCardinality::OneOrMore:
		return Count >= 1;
	default:
		return false;
	}
}

bool IsRequiredResultSchema(
	const FAssetDocumentPrimitiveHandleResultSchema& Schema)
{
	return Schema.Cardinality == EAssetDocumentHandleResultCardinality::ExactlyOne
		|| Schema.Cardinality == EAssetDocumentHandleResultCardinality::OneOrMore;
}

bool IsKnownUniqueResultGroup(
	const FAssetDocumentPrimitiveHandleResultSchema* Schema,
	const TSet<FString>& SeenNames,
	const FString& Name)
{
	return Schema != nullptr && !SeenNames.Contains(Name);
}

bool MatchesGuidScope(const FGuid& Scope, const FGuid& Value)
{
	return !Scope.IsValid() || Scope == Value;
}

bool MatchesRevisionScope(uint64 Scope, uint64 Value)
{
	return Scope == 0 || Scope == Value;
}

bool MatchesProviderScope(FStringView Scope, FStringView Value)
{
	return Scope.IsEmpty() || Scope == Value;
}

bool IsLiveScratchHandleForContext(
	const TSharedPtr<FAssetDocumentNativeHandlePayloadStorage>& Storage,
	const FAssetDocumentOpaqueHandle& Handle,
	const FGuid& RequestId,
	const FGuid& TransactionId,
	uint64 RuntimeRevision)
{
	return Storage.IsValid()
		&& Storage->Payload.IsValid()
		&& Handle.Kind == EAssetDocumentHandleKind::Scratch
		&& Handle.RequestId == RequestId
		&& Handle.TransactionId == TransactionId
		&& Handle.RuntimeRevision == RuntimeRevision;
}

class FAssetDocumentVerifiedHandleLease final
	: public IAssetDocumentVerifiedHandleLease
{
public:
	FAssetDocumentVerifiedHandleLease(
		const FAssetDocumentOpaqueHandle& InHandle,
		const TSharedRef<FAssetDocumentHandleLeaseRevocationState>& InState)
		: Handle(InHandle)
		, State(InState)
	{
	}

	const FAssetDocumentOpaqueHandle& GetHandle() const override
	{
		return Handle;
	}

	IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override
	{
		check(State->Payload != nullptr);
		return *State->Payload;
	}

private:
	FAssetDocumentOpaqueHandle Handle;
	TSharedRef<FAssetDocumentHandleLeaseRevocationState> State;
};
} // namespace

struct FAssetDocumentHandleRegistryRuntime::FSlot final
{
	FAssetDocumentOpaqueHandle Handle;
	TSharedPtr<FAssetDocumentNativeHandlePayloadStorage> Storage;
	TOptional<FAssetDocumentOpaqueHandle> HandoffSource;
	TArray<TWeakPtr<FAssetDocumentHandleLeaseRevocationState>> LeaseStates;
};

FAssetDocumentInvocationHandleSession::FAssetDocumentInvocationHandleSession(
	FAssetDocumentHandleRegistryRuntime& InRegistry,
	const FAssetDocumentHandleResolverBinding& InBinding)
	: Registry(InRegistry)
	, Binding(InBinding)
{
}

FAssetDocumentInvocationHandleSession::~FAssetDocumentInvocationHandleSession()
{
	Close(EAssetDocumentInvocationHandleCloseDisposition::Failed);
}

FAssetDocumentIssuedHandleResult FAssetDocumentInvocationHandleSession::Issue(
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access,
	TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload)
{
	check(IsInGameThread());
	FAssetDocumentIssuedHandleResult Result;
	if (bClosed || !IsBindingLive(Binding) || !IsIssueAdmitted(Binding, Kind, Access))
	{
		Result.Diagnostics.Add(MakeDiagnostic(
			TEXT("AD.RUNTIME.HANDLE_ISSUE_REJECTED"),
			TEXT("The invocation session cannot issue this handle kind or access.")));
		return Result;
	}
	Result = Registry.Issue(Binding, Kind, Access, MoveTemp(Payload));
	if (Result.Handle.IsSet())
	{
		IssuedHandles.Add(Result.Handle.GetValue());
	}
	return Result;
}

FAssetDocumentVerifiedLeaseResult
FAssetDocumentInvocationHandleSession::AcquireVerifiedLease(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentHandleValidationContext& Context)
{
	check(IsInGameThread());
	FAssetDocumentVerifiedLeaseResult Result;
	if (bClosed)
	{
		Result.Diagnostics.Add(MakeDiagnostic(
			TEXT("AD.RUNTIME.STALE_HANDLE"),
			TEXT("The invocation session is closed.")));
		return Result;
	}
	TSharedPtr<FAssetDocumentHandleLeaseRevocationState> State;
	Result = Registry.AcquireVerifiedLease(Handle, Context, State);
	if (State.IsValid())
	{
		TrackLease(State.ToSharedRef());
	}
	return Result;
}

FAssetDocumentHandleInvalidationResult
FAssetDocumentInvocationHandleSession::Invalidate(
	const FAssetDocumentHandleInvalidation& Event)
{
	check(IsInGameThread());
	return Registry.Invalidate(Event);
}

FAssetDocumentContractValidationResult
FAssetDocumentInvocationHandleSession::AcquireAndHoldInput(
	const FAssetDocumentOpaqueHandle& Handle,
	EAssetDocumentHandleKind RequiredKind,
	EAssetDocumentHandleAccess RequiredAccess)
{
	check(IsInGameThread());
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Binding.RequestId;
	Context.TransactionId = Binding.TransactionId;
	Context.RuntimeRevision = Binding.RuntimeRevision;
	Context.ProviderId = Binding.ProviderId;
	Context.Phase = Binding.Phase;
	Context.RequiredKind = RequiredKind;
	Context.RequiredAccess = RequiredAccess;
	FAssetDocumentVerifiedLeaseResult LeaseResult = AcquireVerifiedLease(Handle, Context);
	if (!LeaseResult.Lease.IsValid())
	{
		FAssetDocumentContractValidationResult Failure;
		Failure.Diagnostics = MoveTemp(LeaseResult.Diagnostics);
		return Failure;
	}
	HeldInputLeases.Add(MoveTemp(LeaseResult.Lease));
	return MakeContractSuccess();
}

FAssetDocumentContractValidationResult
FAssetDocumentInvocationHandleSession::MarkValidatedReturnedHandles(
	TConstArrayView<FAssetDocumentPrimitiveHandleResult> Results)
{
	check(IsInGameThread());
	if (bClosed || bMarkedReturnedHandles)
	{
		return MakeContractFailure(
			TEXT("AD.RUNTIME.PRIMITIVE_OUTCOME_REJECTED"),
			TEXT("Returned handles were already marked or the session is closed."));
	}
	const FAssetDocumentContractValidationResult Provenance =
		ValidateReturnedProvenance(Results);
	if (!Provenance.bValid)
	{
		return Provenance;
	}
	TArray<FAssetDocumentOpaqueHandle> ValidatedHandles;
	const FAssetDocumentContractValidationResult Schemas =
		ValidateReturnedSchemas(Results, ValidatedHandles);
	if (!Schemas.bValid)
	{
		return Schemas;
	}
	ReturnedHandles = MoveTemp(ValidatedHandles);
	bMarkedReturnedHandles = true;
	return MakeContractSuccess();
}

void FAssetDocumentInvocationHandleSession::Close(
	EAssetDocumentInvocationHandleCloseDisposition Disposition)
{
	check(IsInGameThread());
	if (bClosed)
	{
		return;
	}
	bClosed = true;
	for (const TSharedRef<FAssetDocumentHandleLeaseRevocationState>& State : SessionLeaseStates)
	{
		State->Payload = nullptr;
	}
	SessionLeaseStates.Reset();
	HeldInputLeases.Reset();

	TArray<FAssetDocumentOpaqueHandle> InvalidatedHandles;
	for (const FAssetDocumentOpaqueHandle& Handle : IssuedHandles)
	{
		if (!ShouldRetainIssuedHandle(Disposition, Handle))
		{
			InvalidatedHandles.Add(Handle);
		}
	}
	Registry.InvalidateHandles(InvalidatedHandles);
}

bool FAssetDocumentInvocationHandleSession::HasIssued(
	const FAssetDocumentOpaqueHandle& Handle) const
{
	return IssuedHandles.ContainsByPredicate(
		[&Handle](const FAssetDocumentOpaqueHandle& Candidate)
		{
			return IsSameHandle(Handle, Candidate);
		});
}

FAssetDocumentContractValidationResult
FAssetDocumentInvocationHandleSession::ValidateReturnedProvenance(
	TConstArrayView<FAssetDocumentPrimitiveHandleResult> Results) const
{
	TSet<uint64> SeenHandles;
	for (const FAssetDocumentPrimitiveHandleResult& Result : Results)
	{
		const FAssetDocumentContractValidationResult Group =
			ValidateResultProvenance(Result, SeenHandles);
		if (!Group.bValid)
		{
			return Group;
		}
	}
	return MakeContractSuccess();
}

FAssetDocumentContractValidationResult
FAssetDocumentInvocationHandleSession::ValidateResultProvenance(
	const FAssetDocumentPrimitiveHandleResult& Result,
	TSet<uint64>& SeenHandles) const
{
	for (const FAssetDocumentOpaqueHandle& Handle : Result.Handles)
	{
		const uint64 Key = MakeHandleKey(Handle);
		if (SeenHandles.Contains(Key) || !HasIssued(Handle))
		{
			return MakeContractFailure(
				TEXT("AD.RUNTIME.PRIMITIVE_OUTCOME_REJECTED"),
				TEXT("A returned handle was duplicated or was not issued by this session."));
		}
		SeenHandles.Add(Key);
	}
	return MakeContractSuccess();
}

FAssetDocumentContractValidationResult
FAssetDocumentInvocationHandleSession::ValidateReturnedSchemas(
	TConstArrayView<FAssetDocumentPrimitiveHandleResult> Results,
	TArray<FAssetDocumentOpaqueHandle>& ValidatedHandles) const
{
	TSet<FString> SeenNames;
	for (const FAssetDocumentPrimitiveHandleResult& Result : Results)
	{
		const FAssetDocumentContractValidationResult Group =
			ValidateReturnedGroup(Result, SeenNames, ValidatedHandles);
		if (!Group.bValid)
		{
			return Group;
		}
	}
	if (!HasAllRequiredReturnedGroups(SeenNames))
	{
		return MakeContractFailure(
			TEXT("AD.RUNTIME.PRIMITIVE_OUTCOME_REJECTED"),
			TEXT("A required returned-handle group is missing."));
	}
	return MakeContractSuccess();
}

FAssetDocumentContractValidationResult
FAssetDocumentInvocationHandleSession::ValidateReturnedGroup(
	const FAssetDocumentPrimitiveHandleResult& Result,
	TSet<FString>& SeenNames,
	TArray<FAssetDocumentOpaqueHandle>& ValidatedHandles) const
{
	const FAssetDocumentPrimitiveHandleResultSchema* Schema =
		FindResultSchema(Binding.AllowedResults, Result.Name);
	if (!IsKnownUniqueResultGroup(Schema, SeenNames, Result.Name))
	{
		return MakeContractFailure(
			TEXT("AD.RUNTIME.PRIMITIVE_OUTCOME_REJECTED"),
			TEXT("Returned handle groups do not match the frozen result schema."));
	}
	if (!IsCardinalityValid(Schema->Cardinality, Result.Handles.Num()))
	{
		return MakeContractFailure(
			TEXT("AD.RUNTIME.PRIMITIVE_OUTCOME_REJECTED"),
			TEXT("Returned handle groups do not match the frozen result schema."));
	}
	SeenNames.Add(Result.Name);
	for (const FAssetDocumentOpaqueHandle& Handle : Result.Handles)
	{
		if (!IsReturnedHandleValid(Handle, *Schema))
		{
			return MakeContractFailure(
				TEXT("AD.RUNTIME.PRIMITIVE_OUTCOME_REJECTED"),
				TEXT("A returned handle is stale or violates the frozen result schema."));
		}
		ValidatedHandles.Add(Handle);
	}
	return MakeContractSuccess();
}

bool FAssetDocumentInvocationHandleSession::HasAllRequiredReturnedGroups(
	const TSet<FString>& SeenNames) const
{
	for (const FAssetDocumentPrimitiveHandleResultSchema& Schema : Binding.AllowedResults)
	{
		if (IsRequiredResultSchema(Schema) && !SeenNames.Contains(Schema.Name))
		{
			return false;
		}
	}
	return true;
}

bool FAssetDocumentInvocationHandleSession::IsReturnedHandleValid(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentPrimitiveHandleResultSchema& Schema) const
{
	return Handle.Kind == Schema.Kind
		&& Handle.Access == Schema.GrantedAccess
		&& Registry.IsIssuedAndLive(Handle);
}

bool FAssetDocumentInvocationHandleSession::ShouldRetainIssuedHandle(
	EAssetDocumentInvocationHandleCloseDisposition Disposition,
	const FAssetDocumentOpaqueHandle& Handle) const
{
	const bool bReturned = ReturnedHandles.ContainsByPredicate(
		[&Handle](const FAssetDocumentOpaqueHandle& Candidate)
		{
			return IsSameHandle(Handle, Candidate);
		});
	return Disposition == EAssetDocumentInvocationHandleCloseDisposition::Succeeded
		&& bMarkedReturnedHandles
		&& bReturned;
}

void FAssetDocumentInvocationHandleSession::TrackLease(
	const TSharedRef<FAssetDocumentHandleLeaseRevocationState>& State)
{
	SessionLeaseStates.Add(State);
}

FAssetDocumentHandleRegistryRuntime::FAssetDocumentHandleRegistryRuntime() = default;

FAssetDocumentHandleRegistryRuntime::~FAssetDocumentHandleRegistryRuntime() = default;

TUniquePtr<FAssetDocumentInvocationHandleSession>
FAssetDocumentHandleRegistryRuntime::CreateInvocationSession(
	const FAssetDocumentHandleResolverBinding& Binding)
{
	return TUniquePtr<FAssetDocumentInvocationHandleSession>(
		new FAssetDocumentInvocationHandleSession(*this, Binding));
}

FAssetDocumentIssuedHandleResult FAssetDocumentHandleRegistryRuntime::Issue(
	const FAssetDocumentHandleResolverBinding& Binding,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access,
	TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload)
{
	check(IsInGameThread());
	FAssetDocumentIssuedHandleResult Result;
	if (!Payload.IsValid() || Payload->GetPayloadTypeId().IsNone() || !Payload->IsAlive())
	{
		Result.Diagnostics.Add(MakeDiagnostic(
			TEXT("AD.RUNTIME.HANDLE_ISSUE_REJECTED"),
			TEXT("A handle payload must be typed, non-null, and live.")));
		return Result;
	}
	FScopeLock Lock(&Mutex);
	if (NextSlot == 0)
	{
		Result.Diagnostics.Add(MakeDiagnostic(
			TEXT("AD.RUNTIME.HANDLE_ISSUE_REJECTED"),
			TEXT("The handle slot space is exhausted.")));
		return Result;
	}
	FAssetDocumentOpaqueHandle Handle;
	Handle.RequestId = Binding.RequestId;
	Handle.TransactionId = Binding.TransactionId;
	Handle.RuntimeRevision = Binding.RuntimeRevision;
	Handle.ProviderId = Binding.ProviderId;
	Handle.Slot = NextSlot++;
	Handle.Generation = 1;
	Handle.Kind = Kind;
	Handle.Access = Access;
	TUniquePtr<FSlot> Slot = MakeUnique<FSlot>();
	Slot->Handle = Handle;
	Slot->Storage = MakeShared<FAssetDocumentNativeHandlePayloadStorage>();
	Slot->Storage->Payload = MoveTemp(Payload);
	Slots.Add(Handle.Slot, MoveTemp(Slot));
	Result.Handle = Handle;
	return Result;
}

FAssetDocumentVerifiedLeaseResult
FAssetDocumentHandleRegistryRuntime::AcquireVerifiedLease(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentHandleValidationContext& Context,
	TSharedPtr<FAssetDocumentHandleLeaseRevocationState>& OutState)
{
	check(IsInGameThread());
	FAssetDocumentVerifiedLeaseResult Result;
	const FAssetDocumentContractValidationResult Metadata =
		ValidateAssetDocumentHandleMetadata(Handle, Context);
	if (!Metadata.bValid)
	{
		Result.Diagnostics = Metadata.Diagnostics;
		return Result;
	}

	IAssetDocumentNativeHandlePayload* Payload = FindPayload(Handle);
	if (Payload == nullptr || !Payload->IsAlive())
	{
		Result.Diagnostics.Add(MakeDiagnostic(
			TEXT("AD.RUNTIME.STALE_HANDLE"),
			TEXT("The handle identity is no longer live.")));
		return Result;
	}

	return CreateLease(Handle, Payload, OutState);
}

IAssetDocumentNativeHandlePayload* FAssetDocumentHandleRegistryRuntime::FindPayload(
	const FAssetDocumentOpaqueHandle& Handle) const
{
	FScopeLock Lock(&Mutex);
	const TUniquePtr<FSlot>* SlotEntry = Slots.Find(Handle.Slot);
	if (SlotEntry == nullptr || !IsSameHandle((*SlotEntry)->Handle, Handle)
		|| !(*SlotEntry)->Storage.IsValid()
		|| !(*SlotEntry)->Storage->Payload.IsValid())
	{
		return nullptr;
	}
	return (*SlotEntry)->Storage->Payload.Get();
}

FAssetDocumentVerifiedLeaseResult FAssetDocumentHandleRegistryRuntime::CreateLease(
	const FAssetDocumentOpaqueHandle& Handle,
	IAssetDocumentNativeHandlePayload* Payload,
	TSharedPtr<FAssetDocumentHandleLeaseRevocationState>& OutState)
{
	FAssetDocumentVerifiedLeaseResult Result;
	FScopeLock Lock(&Mutex);
	TUniquePtr<FSlot>* SlotEntry = Slots.Find(Handle.Slot);
	if (SlotEntry == nullptr || !IsSameHandle((*SlotEntry)->Handle, Handle)
		|| !(*SlotEntry)->Storage.IsValid()
		|| (*SlotEntry)->Storage->Payload.Get() != Payload)
	{
		Result.Diagnostics.Add(MakeDiagnostic(
			TEXT("AD.RUNTIME.STALE_HANDLE"),
			TEXT("The handle identity is no longer live.")));
		return Result;
	}
	FSlot& Slot = **SlotEntry;
	TSharedRef<FAssetDocumentHandleLeaseRevocationState> State =
		MakeShared<FAssetDocumentHandleLeaseRevocationState>();
	State->Payload = Slot.Storage->Payload.Get();
	Slot.LeaseStates.Add(State);
	OutState = State;
	Result.Lease = MakeUnique<FAssetDocumentVerifiedHandleLease>(Handle, State);
	return Result;
}

FAssetDocumentHandleInvalidationResult
FAssetDocumentHandleRegistryRuntime::Invalidate(
	const FAssetDocumentHandleInvalidation& Event)
{
	check(IsInGameThread());
	return InvalidateMatching(
		[&Event](const FSlot& Slot)
		{
			return Slot.Storage.IsValid()
				&& DoesEventMatch(Event, Slot.Handle);
		});
}

void FAssetDocumentHandleRegistryRuntime::RetireScratchHandlesForContext(
	const FGuid& RequestId,
	const FGuid& TransactionId,
	uint64 RuntimeRevision)
{
	check(IsInGameThread());
	TArray<TSharedPtr<FAssetDocumentNativeHandlePayloadStorage>> RetiredStorages;
	FScopeLock Lock(&Mutex);
	for (TPair<uint32, TUniquePtr<FSlot>>& Entry : Slots)
	{
		FSlot& Slot = *Entry.Value;
		const FAssetDocumentOpaqueHandle& Handle = Slot.Handle;
		if (!IsLiveScratchHandleForContext(
			Slot.Storage, Handle, RequestId, TransactionId, RuntimeRevision))
		{
			continue;
		}
		++Slot.Handle.Generation;
		RevokeSlotLeases(Slot);
		RetiredStorages.Add(MoveTemp(Slot.Storage));
	}
}

FAssetDocumentIssuedHandleResult FAssetDocumentHandleRegistryRuntime::Handoff(
	const FAssetDocumentOpaqueHandle& SourceHandle,
	const FAssetDocumentHandleValidationContext& SourceContext,
	FStringView TargetProviderId,
	EAssetDocumentHandleAccess RequestedAccess)
{
	check(IsInGameThread());
	FAssetDocumentIssuedHandleResult Result;
	const FAssetDocumentContractValidationResult Metadata =
		ValidateAssetDocumentHandleMetadata(SourceHandle, SourceContext);
	if (!Metadata.bValid)
	{
		Result.Diagnostics = Metadata.Diagnostics;
		return Result;
	}
	const FAssetDocumentContractValidationResult Admission =
		ValidateHandoffRequest(
			SourceHandle, TargetProviderId, RequestedAccess);
	if (!Admission.bValid)
	{
		Result.Diagnostics = Admission.Diagnostics;
		return Result;
	}
	return IssueHandoffFromLiveSource(
		SourceHandle, TargetProviderId, RequestedAccess);
}

FAssetDocumentContractValidationResult
FAssetDocumentHandleRegistryRuntime::ValidateHandoffRequest(
	const FAssetDocumentOpaqueHandle& SourceHandle,
	FStringView TargetProviderId,
	EAssetDocumentHandleAccess RequestedAccess)
{
	if (SourceHandle.Kind != EAssetDocumentHandleKind::Asset)
	{
		return MakeHandoffFailure(
			TEXT("AD.RUNTIME.HANDOFF_KIND_UNSUPPORTED"),
			TEXT("Only Asset handles support provider handoff."));
	}
	if (static_cast<uint8>(RequestedAccess)
		> static_cast<uint8>(EAssetDocumentHandleAccess::Mutable)
		|| TargetProviderId.IsEmpty()
		|| TargetProviderId == SourceHandle.ProviderId)
	{
		return MakeHandoffFailure(
			TEXT("AD.RUNTIME.HANDOFF_STORAGE_REJECTED"),
			TEXT("The requested target identity is not admissible."));
	}
	if (SourceHandle.Access == EAssetDocumentHandleAccess::ReadOnly
		&& RequestedAccess == EAssetDocumentHandleAccess::Mutable)
	{
		return MakeHandoffFailure(
			TEXT("AD.RUNTIME.HANDOFF_ACCESS_ESCALATION"),
			TEXT("Handle handoff cannot escalate read-only access."));
	}
	return MakeContractSuccess();
}

FAssetDocumentIssuedHandleResult
FAssetDocumentHandleRegistryRuntime::IssueHandoffFromLiveSource(
	const FAssetDocumentOpaqueHandle& SourceHandle,
	FStringView TargetProviderId,
	EAssetDocumentHandleAccess RequestedAccess)
{
	FAssetDocumentIssuedHandleResult Result;
	FScopeLock Lock(&Mutex);
	TUniquePtr<FSlot>* SourceEntry = Slots.Find(SourceHandle.Slot);
	if (SourceEntry == nullptr
		|| !IsSameHandle((*SourceEntry)->Handle, SourceHandle)
		|| !(*SourceEntry)->Storage.IsValid()
		|| !(*SourceEntry)->Storage->Payload.IsValid()
		|| !(*SourceEntry)->Storage->Payload->IsAlive())
	{
		Result.Diagnostics.Add(MakeDiagnostic(
			TEXT("AD.RUNTIME.STALE_HANDLE"),
			TEXT("The handle identity is no longer live.")));
		return Result;
	}
	if (NextSlot == 0)
	{
		Result.Diagnostics.Add(MakeHandoffDiagnostic(
			TEXT("AD.RUNTIME.HANDOFF_STORAGE_REJECTED"),
			TEXT("The handle slot space is exhausted.")));
		return Result;
	}

	FAssetDocumentOpaqueHandle Target = SourceHandle;
	Target.ProviderId = TargetProviderId;
	Target.Slot = NextSlot++;
	Target.Generation = 1;
	Target.Access = RequestedAccess;
	TUniquePtr<FSlot> TargetSlot = MakeUnique<FSlot>();
	TargetSlot->Handle = Target;
	TargetSlot->Storage = (*SourceEntry)->Storage;
	TargetSlot->HandoffSource = SourceHandle;
	Slots.Add(Target.Slot, MoveTemp(TargetSlot));
	Result.Handle = MoveTemp(Target);
	return Result;
}

#if WITH_DEV_AUTOMATION_TESTS
bool FAssetDocumentHandleRegistryRuntime::HasLiveLeaseForTesting(
	const FAssetDocumentOpaqueHandle& Handle) const
{
	check(IsInGameThread());
	FScopeLock Lock(&Mutex);
	const TUniquePtr<FSlot>* SlotEntry = Slots.Find(Handle.Slot);
	if (SlotEntry == nullptr)
	{
		return false;
	}
	FAssetDocumentOpaqueHandle StableIdentity = Handle;
	StableIdentity.Generation = (*SlotEntry)->Handle.Generation;
	if (!IsSameHandle((*SlotEntry)->Handle, StableIdentity))
	{
		return false;
	}
	return (*SlotEntry)->LeaseStates.ContainsByPredicate(
		[](const TWeakPtr<FAssetDocumentHandleLeaseRevocationState>& WeakState)
		{
			const TSharedPtr<FAssetDocumentHandleLeaseRevocationState> State = WeakState.Pin();
			return State.IsValid() && State->Payload != nullptr;
		});
}

void FAssetDocumentHandleRegistryRuntime::ExhaustSlotSpaceForTesting()
{
	check(IsInGameThread());
	FScopeLock Lock(&Mutex);
	NextSlot = 0;
}
#endif

bool FAssetDocumentHandleRegistryRuntime::DoesEventMatch(
	const FAssetDocumentHandleInvalidation& Event,
	const FAssetDocumentOpaqueHandle& Handle)
{
	return MatchesGuidScope(Event.RequestId, Handle.RequestId)
		&& MatchesGuidScope(Event.TransactionId, Handle.TransactionId)
		&& MatchesRevisionScope(Event.RuntimeRevision, Handle.RuntimeRevision)
		&& MatchesProviderScope(Event.ProviderId, Handle.ProviderId);
}

void FAssetDocumentHandleRegistryRuntime::RevokeSlotLeases(FSlot& Slot)
{
	for (const TWeakPtr<FAssetDocumentHandleLeaseRevocationState>& WeakState : Slot.LeaseStates)
	{
		if (TSharedPtr<FAssetDocumentHandleLeaseRevocationState> State = WeakState.Pin())
		{
			State->Payload = nullptr;
		}
	}
	Slot.LeaseStates.Reset();
}

FAssetDocumentHandleInvalidationResult
FAssetDocumentHandleRegistryRuntime::InvalidateMatching(
	TFunctionRef<bool(const FSlot&)> Matches)
{
	return RetireInvalidationClosure(SelectInvalidationClosure(Matches));
}

TSet<uint32> FAssetDocumentHandleRegistryRuntime::SelectInvalidationClosure(
	TFunctionRef<bool(const FSlot&)> Matches) const
{
	FScopeLock Lock(&Mutex);
	TSet<uint32> ClosedSlots;
	for (const TPair<uint32, TUniquePtr<FSlot>>& Entry : Slots)
	{
		if (Entry.Value->Storage.IsValid() && Matches(*Entry.Value))
		{
			ClosedSlots.Add(Entry.Key);
		}
	}
	bool bAddedDescendant = true;
	while (bAddedDescendant)
	{
		bAddedDescendant = false;
		for (const TPair<uint32, TUniquePtr<FSlot>>& Entry : Slots)
		{
			const FSlot& Slot = *Entry.Value;
			if (!ClosedSlots.Contains(Entry.Key)
				&& IsDescendantOfClosedSlot(Slot, ClosedSlots))
			{
				ClosedSlots.Add(Entry.Key);
				bAddedDescendant = true;
			}
		}
	}
	return ClosedSlots;
}

bool FAssetDocumentHandleRegistryRuntime::IsDescendantOfClosedSlot(
	const FSlot& Slot,
	const TSet<uint32>& ClosedSlots) const
{
	if (!Slot.Storage.IsValid() || !Slot.HandoffSource.IsSet())
	{
		return false;
	}
	const FAssetDocumentOpaqueHandle& Parent = Slot.HandoffSource.GetValue();
	const TUniquePtr<FSlot>* ParentEntry = Slots.Find(Parent.Slot);
	return ParentEntry != nullptr
		&& ClosedSlots.Contains(Parent.Slot)
		&& IsSameHandle((*ParentEntry)->Handle, Parent);
}

FAssetDocumentHandleInvalidationResult
FAssetDocumentHandleRegistryRuntime::RetireInvalidationClosure(
	const TSet<uint32>& ClosedSlots)
{
	FAssetDocumentHandleInvalidationResult Result;
	TArray<TSharedPtr<FAssetDocumentNativeHandlePayloadStorage>> RetiredStorages;
	FScopeLock Lock(&Mutex);
	for (uint32 SlotId : ClosedSlots)
	{
		TUniquePtr<FSlot>* Entry = Slots.Find(SlotId);
		if (Entry == nullptr || !(*Entry)->Storage.IsValid())
		{
			continue;
		}
		++(*Entry)->Handle.Generation;
		RevokeSlotLeases(**Entry);
		RetiredStorages.Add(MoveTemp((*Entry)->Storage));
		(*Entry)->HandoffSource.Reset();
		++Result.InvalidatedHandleCount;
	}
	return Result;
}

bool FAssetDocumentHandleRegistryRuntime::IsIssuedAndLive(
	const FAssetDocumentOpaqueHandle& Handle) const
{
	check(IsInGameThread());
	IAssetDocumentNativeHandlePayload* Payload = FindPayload(Handle);
	return Payload != nullptr && Payload->IsAlive();
}

void FAssetDocumentHandleRegistryRuntime::InvalidateHandles(
	TConstArrayView<FAssetDocumentOpaqueHandle> Handles)
{
	check(IsInGameThread());
	InvalidateMatching(
		[Handles](const FSlot& Slot)
		{
			return Handles.ContainsByPredicate(
				[&Slot](const FAssetDocumentOpaqueHandle& Handle)
				{
					return IsSameHandle(Slot.Handle, Handle);
				});
		});
}
