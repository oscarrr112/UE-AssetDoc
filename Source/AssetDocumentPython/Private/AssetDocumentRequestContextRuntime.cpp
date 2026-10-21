// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentRequestContextRuntime.h"

namespace
{
FAssetDocumentPrimitiveOutcome MakeInvocationFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Outcome.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/PrimitiveInvocation");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Outcome;
}

FAssetDocumentPrimitiveOutcome MakeInvocationFailure(
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	Outcome.Diagnostics = Diagnostics;
	return Outcome;
}

FAssetDocumentIssuedHandleResult MakeRequestContextHandoffFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentIssuedHandleResult Result;
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/HandleHandoff");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Result;
}

bool IsHandoffPhase(EAssetDocumentRuntimeRequestPhase Phase)
{
	return Phase == EAssetDocumentRuntimeRequestPhase::Preflight
		|| Phase == EAssetDocumentRuntimeRequestPhase::Apply
		|| Phase == EAssetDocumentRuntimeRequestPhase::Repair
		|| Phase == EAssetDocumentRuntimeRequestPhase::Verify;
}

bool IsValidHandoffAccess(EAssetDocumentHandleAccess Access)
{
	return static_cast<uint8>(Access)
		<= static_cast<uint8>(EAssetDocumentHandleAccess::Mutable);
}

bool IsLiveTransactionForContext(
	const FAssetDocumentRequestContextMetadata& Metadata,
	const FGuid& TransactionId,
	const IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	if (ActiveTransaction == nullptr
		|| ActiveTransaction->GetTransactionId() != TransactionId
		|| static_cast<uint8>(ActiveTransaction->GetState())
			> static_cast<uint8>(EAssetDocumentAtomicState::ReadyToCommit))
	{
		return false;
	}
	const FAssetDocumentMutationToken& Token =
		ActiveTransaction->GetMutationToken();
	return Token.RequestId == Metadata.RequestId
		&& Token.TransactionId == TransactionId
		&& Token.RuntimeRevision == Metadata.PinnedRuntimeRevision;
}

FAssetDocumentHandleValidationContext MakeHandoffSourceContext(
	const FAssetDocumentRequestContextMetadata& Metadata,
	const FGuid& TransactionId,
	const FAssetDocumentOpaqueHandle& SourceHandle)
{
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Metadata.RequestId;
	Context.TransactionId = TransactionId;
	Context.RuntimeRevision = Metadata.PinnedRuntimeRevision;
	Context.ProviderId = SourceHandle.ProviderId;
	Context.Phase = Metadata.Phase;
	Context.RequiredKind = EAssetDocumentHandleKind::Asset;
	Context.RequiredAccess = SourceHandle.Access;
	return Context;
}

const FAssetDocumentPrimitiveHandleArgumentSchema* FindInputSchema(
	TConstArrayView<FAssetDocumentPrimitiveHandleArgumentSchema> Schemas,
	FStringView Name)
{
	for (const FAssetDocumentPrimitiveHandleArgumentSchema& Schema : Schemas)
	{
		if (Schema.Name == Name)
		{
			return &Schema;
		}
	}
	return nullptr;
}

FAssetDocumentHandleResolverBinding MakeBinding(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	FAssetDocumentHandleResolverBinding Binding;
	Binding.RequestId = Invocation.RequestId;
	Binding.TransactionId = Invocation.TransactionId;
	Binding.RuntimeRevision = Invocation.RuntimeRevision;
	Binding.ProviderId = Descriptor.ProviderId;
	Binding.Phase = Invocation.Phase;
	Binding.AllowedResults = Descriptor.HandleResultSchemas;
	return Binding;
}

bool IsTransactionAvailable(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	return !Descriptor.bRequiresActiveTransaction || ActiveTransaction != nullptr;
}

FAssetDocumentPrimitiveOutcome MakeNestedFailure(
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const TCHAR* Code = Descriptor.Access == EAssetDocumentPrimitiveAccess::Mutating
		? TEXT("AD.RUNTIME.NESTED_MUTATION")
		: TEXT("AD.RUNTIME.PRIMITIVE_INVOCATION_REJECTED");
	return MakeInvocationFailure(Code, TEXT("Nested primitive invocation is not allowed."));
}

FAssetDocumentContractValidationResult ValidateContextInvocation(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	FAssetDocumentContractValidationResult Result =
		ValidateAssetDocumentPrimitiveInvocation(Invocation, Descriptor);
	if (!Result.bValid || IsTransactionAvailable(Descriptor, ActiveTransaction))
	{
		return Result;
	}
	Result.bValid = false;
	Result.Diagnostics.Reset();
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/PrimitiveInvocation");
	Diagnostic.Code = TEXT("AD.RUNTIME.PRIMITIVE_INVOCATION_REJECTED");
	Diagnostic.Message = TEXT("The primitive requires an active atomic transaction.");
	return Result;
}

struct FInvokeGuard final
{
	explicit FInvokeGuard(bool& InActive)
		: Active(InActive)
	{
		Active = true;
	}
	~FInvokeGuard()
	{
		Active = false;
	}
	bool& Active;
};
} // namespace

FAssetDocumentNativeRequestContextRuntime::FAssetDocumentNativeRequestContextRuntime(
	const FAssetDocumentRequestContextMetadata& InMetadata,
	const FGuid& InTransactionId,
	const TSharedRef<FAssetDocumentPrimitiveRegistryRuntime>& InRegistry,
	const TSharedRef<FAssetDocumentHandleRegistryRuntime>& InHandles,
	IAssetDocumentAtomicTransaction* InActiveTransaction)
	: Metadata(InMetadata)
	, TransactionId(InTransactionId)
	, Registry(InRegistry)
	, Handles(InHandles)
	, ActiveTransaction(InActiveTransaction)
{
}

FAssetDocumentNativeRequestContextRuntime::~FAssetDocumentNativeRequestContextRuntime()
{
	check(IsInGameThread());
	Handles->RetireScratchHandlesForContext(
		Metadata.RequestId,
		TransactionId,
		Metadata.PinnedRuntimeRevision);
}

const FAssetDocumentRequestContextMetadata&
FAssetDocumentNativeRequestContextRuntime::GetMetadata() const
{
	return Metadata;
}

FAssetDocumentPrimitiveOutcome FAssetDocumentNativeRequestContextRuntime::Invoke(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	check(IsInGameThread());
	const FAssetDocumentPrimitiveInvocation Pinned = PinInvocation(Invocation);
	FAssetDocumentPrimitiveRequirement Requirement;
	Requirement.PrimitiveId = Pinned.PrimitiveId;
	Requirement.Version = Pinned.Version;
	Requirement.RequiredByRouteId = TEXT("asset-document-native-context");
	const FAssetDocumentPrimitiveResolution Resolution = Registry->Resolve(Requirement);
	if (!Resolution.bResolved)
	{
		return MakeInvocationFailure(Resolution.Diagnostics);
	}
	if (bInvokeActive)
	{
		return MakeNestedFailure(Resolution.Descriptor);
	}
	const FAssetDocumentContractValidationResult Validation =
		ValidateContextInvocation(Pinned, Resolution.Descriptor, ActiveTransaction);
	if (!Validation.bValid)
	{
		return MakeInvocationFailure(Validation.Diagnostics);
	}
	TSharedPtr<IAssetDocumentPrimitiveProvider> Provider =
		Registry->FindProvider(Resolution.ProviderId);
	if (!Provider.IsValid())
	{
		return MakeInvocationFailure(
			TEXT("AD.RUNTIME.PROVIDER_UNAVAILABLE"),
			TEXT("The frozen provider is unavailable."));
	}
	FInvokeGuard Guard(bInvokeActive);
	return InvokeResolved(Pinned, Resolution.Descriptor, Provider.ToSharedRef());
}

FAssetDocumentPrimitiveRegistrySnapshot
FAssetDocumentNativeRequestContextRuntime::InspectCatalog() const
{
	check(IsInGameThread());
	return Registry->Snapshot();
}

FAssetDocumentIssuedHandleResult
FAssetDocumentNativeRequestContextRuntime::Handoff(
	const FAssetDocumentOpaqueHandle& SourceHandle,
	FStringView TargetProviderId,
	EAssetDocumentHandleAccess RequestedAccess)
{
	check(IsInGameThread());
	const bool bValidateReadOnly = Metadata.Phase == EAssetDocumentRuntimeRequestPhase::Validate
		&& ActiveTransaction == nullptr
		&& SourceHandle.Kind == EAssetDocumentHandleKind::Asset
		&& SourceHandle.Access == EAssetDocumentHandleAccess::ReadOnly
		&& RequestedAccess == EAssetDocumentHandleAccess::ReadOnly;
	if (bInvokeActive || (!IsHandoffPhase(Metadata.Phase) && !bValidateReadOnly))
	{
		return MakeRequestContextHandoffFailure(
			TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT"),
			TEXT("Handle handoff is not admitted in the active request context."));
	}
	if (TargetProviderId.IsEmpty()
		|| TargetProviderId == SourceHandle.ProviderId)
	{
		return MakeRequestContextHandoffFailure(
			TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT"),
			TEXT("Handle handoff requires a different non-empty target provider."));
	}
	if (SourceHandle.Kind != EAssetDocumentHandleKind::Asset)
	{
		return MakeRequestContextHandoffFailure(
			TEXT("AD.RUNTIME.HANDOFF_KIND_UNSUPPORTED"),
			TEXT("Only Asset handles support provider handoff."));
	}
	if (!IsValidHandoffAccess(RequestedAccess))
	{
		return MakeRequestContextHandoffFailure(
			TEXT("AD.RUNTIME.HANDOFF_INVALID_CONTEXT"),
			TEXT("The requested handle access is invalid."));
	}
	if (RequestedAccess == EAssetDocumentHandleAccess::Mutable
		&& ((Metadata.Phase != EAssetDocumentRuntimeRequestPhase::Apply
				&& Metadata.Phase != EAssetDocumentRuntimeRequestPhase::Repair)
			|| !IsLiveTransactionForContext(
				Metadata, TransactionId, ActiveTransaction)))
	{
		return MakeRequestContextHandoffFailure(
			TEXT("AD.RUNTIME.HANDOFF_TRANSACTION_REQUIRED"),
			TEXT("Mutable handle handoff requires an active matching transaction."));
	}
	if (SourceHandle.Access == EAssetDocumentHandleAccess::ReadOnly
		&& RequestedAccess == EAssetDocumentHandleAccess::Mutable)
	{
		return MakeRequestContextHandoffFailure(
			TEXT("AD.RUNTIME.HANDOFF_ACCESS_ESCALATION"),
			TEXT("Handle handoff cannot escalate read-only access."));
	}
	if (!Registry->FindProvider(TargetProviderId).IsValid())
	{
		return MakeRequestContextHandoffFailure(
			TEXT("AD.RUNTIME.HANDOFF_TARGET_UNAVAILABLE"),
			TEXT("The target provider is absent from the frozen registry."));
	}

	FInvokeGuard Guard(bInvokeActive);
	return Handles->Handoff(
		SourceHandle,
		MakeHandoffSourceContext(Metadata, TransactionId, SourceHandle),
		TargetProviderId,
		RequestedAccess);
}

FAssetDocumentPrimitiveInvocation
FAssetDocumentNativeRequestContextRuntime::PinInvocation(
	const FAssetDocumentPrimitiveInvocation& Invocation) const
{
	FAssetDocumentPrimitiveInvocation Pinned = Invocation;
	Pinned.RequestId = Metadata.RequestId;
	Pinned.TransactionId = TransactionId;
	Pinned.RuntimeRevision = Metadata.PinnedRuntimeRevision;
	Pinned.Phase = Metadata.Phase;
	return Pinned;
}

FAssetDocumentPrimitiveOutcome
FAssetDocumentNativeRequestContextRuntime::InvokeResolved(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const TSharedRef<IAssetDocumentPrimitiveProvider>& Provider)
{
	TUniquePtr<FAssetDocumentInvocationHandleSession> Session =
		Handles->CreateInvocationSession(MakeBinding(Invocation, Descriptor));
	const FAssetDocumentContractValidationResult Inputs =
		AcquireInputs(*Session, Invocation, Descriptor);
	if (!Inputs.bValid)
	{
		Session->Close(EAssetDocumentInvocationHandleCloseDisposition::Failed);
		return MakeInvocationFailure(Inputs.Diagnostics);
	}
	IAssetDocumentHandleResolver& ProviderResolver = *Session;
	FAssetDocumentPrimitiveOutcome Outcome =
		Provider->Invoke(Invocation, ProviderResolver, ActiveTransaction);
	const FAssetDocumentContractValidationResult OutcomeValidation =
		ValidateAssetDocumentPrimitiveOutcome(Invocation, Descriptor, Outcome);
	if (!OutcomeValidation.bValid)
	{
		Session->Close(EAssetDocumentInvocationHandleCloseDisposition::Failed);
		return MakeInvocationFailure(OutcomeValidation.Diagnostics);
	}
	if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		Session->Close(EAssetDocumentInvocationHandleCloseDisposition::Failed);
		return Outcome;
	}
	const FAssetDocumentContractValidationResult Marked =
		Session->MarkValidatedReturnedHandles(Outcome.HandleResults);
	if (!Marked.bValid)
	{
		Session->Close(EAssetDocumentInvocationHandleCloseDisposition::Failed);
		return MakeInvocationFailure(Marked.Diagnostics);
	}
	Session->Close(EAssetDocumentInvocationHandleCloseDisposition::Succeeded);
	return Outcome;
}

FAssetDocumentContractValidationResult
FAssetDocumentNativeRequestContextRuntime::AcquireInputs(
	FAssetDocumentInvocationHandleSession& Session,
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor) const
{
	for (const FAssetDocumentPrimitiveHandleArgument& Argument : Invocation.HandleArguments)
	{
		const FAssetDocumentPrimitiveHandleArgumentSchema* Schema =
			FindInputSchema(Descriptor.HandleArgumentSchemas, Argument.Name);
		check(Schema != nullptr);
		const FAssetDocumentContractValidationResult Acquired = Session.AcquireAndHoldInput(
			Argument.Handle, Schema->Kind, Schema->RequiredAccess);
		if (!Acquired.bValid)
		{
			return Acquired;
		}
	}
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}
