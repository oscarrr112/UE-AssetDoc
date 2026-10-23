// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorPrimitiveProvider.h"

#include "Provider/AssetDocumentEditorPrimitiveCatalog.h"
#include "Provider/AssetDocumentEditorPrimitiveHandler.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"

namespace AssetDocumentEditorPrimitiveProvider
{
FAssetDocumentRuntimeDiagnostic MakeDiagnostic(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/Primitive");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Diagnostic;
}

FAssetDocumentPrimitiveOutcome MakeFailure(
	const TCHAR* Code,
	const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	if (Diagnostics.IsEmpty())
	{
		Outcome.Diagnostics.Add(MakeDiagnostic(Code, Message));
	}
	else
	{
		Outcome.Diagnostics.Append(Diagnostics.GetData(), Diagnostics.Num());
	}
	return Outcome;
}

static const FAssetDocumentPrimitiveDescriptor* FindDescriptor(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor :
		GetAssetDocumentEditorPrimitiveDescriptors())
	{
		if (Descriptor.PrimitiveId == Invocation.PrimitiveId
			&& Descriptor.Version == Invocation.Version)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

static const FAssetDocumentEditorPrimitiveBinding* FindBinding(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	for (const FAssetDocumentEditorPrimitiveBinding& Binding :
		GetAssetDocumentEditorPrimitiveBindings())
	{
		if (Binding.PrimitiveId == Invocation.PrimitiveId
			&& Binding.Version == Invocation.Version)
		{
			return &Binding;
		}
	}
	return nullptr;
}

static bool HasCallableHandler(
	const FAssetDocumentEditorPrimitiveBinding* Binding)
{
	return Binding != nullptr && Binding->Handler != nullptr;
}

bool FindDispatch(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor*& OutDescriptor,
	const FAssetDocumentEditorPrimitiveBinding*& OutBinding)
{
	if (const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(Invocation))
	{
		OutDescriptor = Descriptor;
	}
	if (const FAssetDocumentEditorPrimitiveBinding* Binding = FindBinding(Invocation))
	{
		OutBinding = Binding;
	}
	return OutDescriptor != nullptr
		&& HasCallableHandler(OutBinding);
}

bool DoesTransactionTokenMatch(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentMutationToken& Token)
{
	return Token.RequestId == Invocation.RequestId
		&& Token.TransactionId == Invocation.TransactionId
		&& Token.RuntimeRevision == Invocation.RuntimeRevision;
}

static bool HasMatchingTransactionIdentity(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	return ActiveTransaction != nullptr
		&& ActiveTransaction->GetTransactionId() == Invocation.TransactionId;
}

static bool IsActiveTransactionState(
	const IAssetDocumentAtomicTransaction& ActiveTransaction)
{
	const EAssetDocumentAtomicState State = ActiveTransaction.GetState();
	return State == EAssetDocumentAtomicState::Live
		|| State == EAssetDocumentAtomicState::Mutating;
}

bool IsTransactionValid(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	if (!Descriptor.bRequiresActiveTransaction)
	{
		return true;
	}
	if (!HasMatchingTransactionIdentity(Invocation, ActiveTransaction))
	{
		return false;
	}
	if (!IsActiveTransactionState(*ActiveTransaction))
	{
		return false;
	}
	const FAssetDocumentMutationToken& Token =
		ActiveTransaction->GetMutationToken();
	return DoesTransactionTokenMatch(Invocation, Token);
}

FAssetDocumentPrimitiveOutcome NormalizeNonSucceeded(
	FAssetDocumentPrimitiveOutcome&& Outcome)
{
	Outcome.Value = {};
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	Outcome.HandleResults.Reset();
	Outcome.ChangedSet.Reset();
	if (Outcome.Diagnostics.IsEmpty())
	{
		Outcome.Diagnostics.Add(MakeDiagnostic(
			TEXT("AD.RUNTIME.PRIMITIVE_FAILED"),
			TEXT("The primitive did not succeed.")));
	}
	return MoveTemp(Outcome);
}

FAssetDocumentPrimitiveOutcome InvokeValidatedHandler(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorPrimitiveBinding& Binding,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	FAssetDocumentPrimitiveOutcome Outcome =
		Binding.Handler(Invocation, Handles, ActiveTransaction);
	const FAssetDocumentContractValidationResult OutcomeValidation =
		ValidateAssetDocumentPrimitiveOutcome(Invocation, Descriptor, Outcome);
	if (!OutcomeValidation.bValid)
	{
		return MakeFailure(
			TEXT("GenericProvider.InvalidOutcome"),
			TEXT("The primitive handler returned an invalid outcome."),
			OutcomeValidation.Diagnostics);
	}
	if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		return NormalizeNonSucceeded(MoveTemp(Outcome));
	}
	return Outcome;
}

bool IsCatalogValid()
{
	const TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors =
		GetAssetDocumentEditorPrimitiveDescriptors();
	for (int32 Index = 0; Index < Descriptors.Num(); ++Index)
	{
		if (!ValidateAssetDocumentPrimitiveDescriptor(
			Descriptors[Index], Descriptors.Left(Index)).bValid)
		{
			return false;
		}
	}
	return true;
}

class FProvider final : public IAssetDocumentPrimitiveProvider
{
public:
	virtual FString GetProviderId() const override
	{
		return TEXT("editor");
	}

	virtual TConstArrayView<FAssetDocumentPrimitiveDescriptor>
	DescribePrimitives() const override
	{
		return GetAssetDocumentEditorPrimitiveDescriptors();
	}

	virtual FAssetDocumentProviderAvailability InspectAvailability() const override
	{
		FAssetDocumentProviderAvailability Availability;
		Availability.ProviderId = GetProviderId();
		if (IsCatalogValid() && IsAssetDocumentEditorPrimitiveAssemblyValid())
		{
			Availability.State = EAssetDocumentProviderAvailabilityState::Available;
			return Availability;
		}
		Availability.State = EAssetDocumentProviderAvailabilityState::Unavailable;
		Availability.Diagnostic = MakeDiagnostic(
			TEXT("GenericProvider.InvalidAssembly"),
			TEXT("The immutable descriptor and binding tables are not bijective."));
		return Availability;
	}

	virtual FAssetDocumentPrimitiveOutcome Invoke(
		const FAssetDocumentPrimitiveInvocation& Invocation,
		IAssetDocumentHandleResolver& Handles,
		IAssetDocumentAtomicTransaction* ActiveTransaction) override
	{
		const FAssetDocumentPrimitiveDescriptor* Descriptor = nullptr;
		const FAssetDocumentEditorPrimitiveBinding* Binding = nullptr;
		if (!FindDispatch(Invocation, Descriptor, Binding))
		{
			return MakeFailure(
				TEXT("GenericProvider.UnknownPrimitive"),
				TEXT("No exact primitive id and version binding exists."),
				{});
		}
		const FAssetDocumentContractValidationResult InvocationValidation =
			ValidateAssetDocumentPrimitiveInvocation(Invocation, *Descriptor);
		if (!InvocationValidation.bValid)
		{
			return MakeFailure(
				TEXT("GenericProvider.InvalidInvocation"),
				TEXT("The primitive invocation does not match its descriptor."),
				InvocationValidation.Diagnostics);
		}
		if (!IsTransactionValid(Invocation, *Descriptor, ActiveTransaction))
		{
			return MakeFailure(
				TEXT("GenericProvider.InvalidTransaction"),
				TEXT("The active transaction identity, token, revision, or state is invalid."),
				{});
		}
		return InvokeValidatedHandler(
			Invocation, *Descriptor, *Binding, Handles, ActiveTransaction);
	}
};
} // namespace AssetDocumentEditorPrimitiveProvider

TSharedRef<IAssetDocumentPrimitiveProvider>
CreateAssetDocumentEditorPrimitiveProvider()
{
	return MakeShared<AssetDocumentEditorPrimitiveProvider::FProvider>();
}
