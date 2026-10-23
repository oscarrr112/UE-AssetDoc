// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentAnimationPrimitiveProvider.h"

#include "Provider/AssetDocumentAnimationPrimitiveCatalog.h"
#include "Provider/AssetDocumentAnimationProviderCommon.h"

namespace AssetDocumentAnimationPrimitiveProvider
{
const FAssetDocumentAnimationDecision* FindDecision(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	for (const FAssetDocumentAnimationDecision& Decision :
		GetAssetDocumentAnimationDecisions())
	{
		if (Decision.PrimitiveId == Invocation.PrimitiveId
			&& Decision.Version == Invocation.Version)
		{
			return &Decision;
		}
	}
	return nullptr;
}

const FAssetDocumentPrimitiveDescriptor* FindDescriptor(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor :
		GetAssetDocumentAnimationPrimitiveDescriptors())
	{
		if (Descriptor.PrimitiveId == Invocation.PrimitiveId
			&& Descriptor.Version == Invocation.Version)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

const FAssetDocumentAnimationPrimitiveBinding* FindBinding(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	for (const FAssetDocumentAnimationPrimitiveBinding& Binding :
		GetAssetDocumentAnimationPrimitiveBindings())
	{
		if (Binding.PrimitiveId == Invocation.PrimitiveId
			&& Binding.Version == Invocation.Version)
		{
			return &Binding;
		}
	}
	return nullptr;
}

bool HasMatchingTransaction(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const IAssetDocumentAtomicTransaction* Transaction)
{
	if (Transaction == nullptr
		|| Transaction->GetState() != EAssetDocumentAtomicState::Mutating
		|| Transaction->GetTransactionId() != Invocation.TransactionId)
	{
		return false;
	}
	const FAssetDocumentMutationToken& Token = Transaction->GetMutationToken();
	return Token.RequestId == Invocation.RequestId
		&& Token.TransactionId == Invocation.TransactionId
		&& Token.RuntimeRevision == Invocation.RuntimeRevision;
}

bool IsAssemblyValid()
{
	const TConstArrayView<FAssetDocumentAnimationDecision> Decisions =
		GetAssetDocumentAnimationDecisions();
	const TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors =
		GetAssetDocumentAnimationPrimitiveDescriptors();
	return ValidateAnimationDecisionClosure(Decisions).bValid
		&& ValidateAnimationDescriptorBindingBijection(
			Decisions, Descriptors,
			GetAssetDocumentAnimationPrimitiveBindings()).bValid;
}

class FProvider final : public IAssetDocumentPrimitiveProvider
{
public:
	FProvider() : bAssemblyValid(IsAssemblyValid()) {}

	virtual FString GetProviderId() const override
	{
		return TEXT("animation");
	}

	virtual TConstArrayView<FAssetDocumentPrimitiveDescriptor>
	DescribePrimitives() const override
	{
		return GetAssetDocumentAnimationPrimitiveDescriptors();
	}

	virtual FAssetDocumentProviderAvailability InspectAvailability() const override
	{
		FAssetDocumentProviderAvailability Availability;
		Availability.ProviderId = GetProviderId();
		Availability.State = bAssemblyValid
			? EAssetDocumentProviderAvailabilityState::Available
			: EAssetDocumentProviderAvailabilityState::Unavailable;
		if (!bAssemblyValid)
		{
			FAssetDocumentRuntimeDiagnostic Diagnostic;
			Diagnostic.Path = TEXT("/Primitive");
			Diagnostic.Code = TEXT("AnimationProvider.AuthorityUnavailable");
			Diagnostic.Message = TEXT("The Animation provider assembly is invalid.");
			Availability.Diagnostic = MoveTemp(Diagnostic);
		}
		return Availability;
	}

	virtual FAssetDocumentPrimitiveOutcome Invoke(
		const FAssetDocumentPrimitiveInvocation& Invocation,
		IAssetDocumentHandleResolver& Handles,
		IAssetDocumentAtomicTransaction* ActiveTransaction) override
	{
		const FAssetDocumentAnimationDecision* Decision = FindDecision(Invocation);
		if (Decision != nullptr && Decision->Disposition !=
			EAssetDocumentAnimationDecisionDisposition::Admitted)
		{
			return MakeAnimationProviderFailure(
				Decision->StableBlockerDiagnostic,
				TEXT("The exact Animation decision remains non-callable."));
		}
		const FAssetDocumentPrimitiveDescriptor* Descriptor =
			FindDescriptor(Invocation);
		const FAssetDocumentAnimationPrimitiveBinding* Binding =
			FindBinding(Invocation);
		if (!bAssemblyValid || Descriptor == nullptr || Binding == nullptr
			|| Binding->Handler == nullptr)
		{
			return MakeAnimationProviderFailure(
				TEXT("AnimationProvider.AuthorityUnavailable"),
				TEXT("No exact admitted Animation primitive binding exists."));
		}
		if (Descriptor->bRequiresActiveTransaction
			&& !HasMatchingTransaction(Invocation, ActiveTransaction))
		{
			return MakeAnimationProviderFailure(
				TEXT("AnimationProvider.TransactionUnavailable"),
				TEXT("A matching active Animation transaction is required."));
		}
		if (!ValidateAssetDocumentPrimitiveInvocation(
			Invocation, *Descriptor).bValid)
		{
			return MakeAnimationProviderFailure(
				TEXT("AnimationProvider.InvalidValueShape"),
				TEXT("The invocation does not match the Animation descriptor."));
		}
		return FinalizeAnimationPrimitiveOutcome(
			Invocation, *Descriptor,
			Binding->Handler(Invocation, Handles, ActiveTransaction));
	}

private:
	bool bAssemblyValid = false;
};
} // namespace AssetDocumentAnimationPrimitiveProvider

TSharedRef<IAssetDocumentPrimitiveProvider>
CreateAssetDocumentAnimationPrimitiveProvider()
{
	return MakeShared<AssetDocumentAnimationPrimitiveProvider::FProvider>();
}
