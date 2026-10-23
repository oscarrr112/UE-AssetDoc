// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentUMGPrimitiveProvider.h"

#include "Provider/AssetDocumentUMGPrimitiveCatalog.h"
#include "Provider/AssetDocumentUMGPrimitiveHandler.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"

namespace AssetDocumentUMGPrimitiveProvider
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

const FAssetDocumentPrimitiveDescriptor* FindDescriptor(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor :
		GetAssetDocumentUMGPrimitiveDescriptors())
	{
		if (Descriptor.PrimitiveId == Invocation.PrimitiveId
			&& Descriptor.Version == Invocation.Version)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

const FAssetDocumentUMGPrimitiveBinding* FindBinding(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	for (const FAssetDocumentUMGPrimitiveBinding& Binding :
		GetAssetDocumentUMGPrimitiveBindings())
	{
		if (Binding.PrimitiveId == Invocation.PrimitiveId
			&& Binding.Version == Invocation.Version)
		{
			return &Binding;
		}
	}
	return nullptr;
}

bool IsCatalogValid()
{
	const TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors =
		GetAssetDocumentUMGPrimitiveDescriptors();
	for (int32 Index = 0; Index < Descriptors.Num(); ++Index)
	{
		if (!ValidateAssetDocumentPrimitiveDescriptor(
			Descriptors[Index], Descriptors.Left(Index)).bValid)
		{
			return false;
		}
	}
	return Descriptors.Num() == 16;
}

bool HasMatchingMutatingTransaction(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	if (ActiveTransaction == nullptr
		|| ActiveTransaction->GetState() !=
			EAssetDocumentAtomicState::Mutating
		|| ActiveTransaction->GetTransactionId() != Invocation.TransactionId)
	{
		return false;
	}
	const FAssetDocumentMutationToken& Token =
		ActiveTransaction->GetMutationToken();
	return Token.RequestId == Invocation.RequestId
		&& Token.TransactionId == Invocation.TransactionId
		&& Token.RuntimeRevision == Invocation.RuntimeRevision;
}

FAssetDocumentPrimitiveOutcome NormalizeFailed(
	FAssetDocumentPrimitiveOutcome&& Outcome)
{
	Outcome.Value = {};
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	Outcome.HandleResults.Reset();
	Outcome.ChangedSet.Reset();
	if (Outcome.Diagnostics.IsEmpty())
	{
		Outcome.Diagnostics.Add(MakeDiagnostic(
			TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"),
			TEXT("The UMG primitive did not complete successfully.")));
	}
	return MoveTemp(Outcome);
}

FAssetDocumentPrimitiveOutcome InvokeValidated(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentUMGPrimitiveBinding& Binding,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	FAssetDocumentPrimitiveOutcome Outcome =
		Binding.Handler(Invocation, Handles, ActiveTransaction);
	const FAssetDocumentContractValidationResult Validation =
		ValidateAssetDocumentPrimitiveOutcome(
			Invocation, Descriptor, Outcome);
	if (!Validation.bValid)
	{
		return MakeAssetDocumentUMGFailure(
			TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"),
			TEXT("The UMG handler returned an invalid outcome."),
			Validation.Diagnostics);
	}
	return Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded
		? MoveTemp(Outcome)
		: NormalizeFailed(MoveTemp(Outcome));
}

class FProvider final : public IAssetDocumentPrimitiveProvider
{
public:
	FProvider()
		: bAssemblyValid(
			IsAssetDocumentUMGDecisionProjectionValid()
			&& IsCatalogValid()
			&& IsAssetDocumentUMGPrimitiveAssemblyValid())
	{
	}

	virtual FString GetProviderId() const override
	{
		return TEXT("umg");
	}

	virtual TConstArrayView<FAssetDocumentPrimitiveDescriptor>
	DescribePrimitives() const override
	{
		return GetAssetDocumentUMGPrimitiveDescriptors();
	}

	virtual FAssetDocumentProviderAvailability InspectAvailability() const override
	{
		FAssetDocumentProviderAvailability Availability;
		Availability.ProviderId = GetProviderId();
		if (bAssemblyValid)
		{
			Availability.State =
				EAssetDocumentProviderAvailabilityState::Available;
			return Availability;
		}
		Availability.State =
			EAssetDocumentProviderAvailabilityState::Unavailable;
		Availability.Diagnostic = MakeDiagnostic(
			TEXT("AD.UMG.PROVIDER.AUTHORITY_UNAVAILABLE"),
			TEXT("The frozen UMG projection, authority catalog, and handler table are not bijective."));
		return Availability;
	}

	virtual FAssetDocumentPrimitiveOutcome Invoke(
		const FAssetDocumentPrimitiveInvocation& Invocation,
		IAssetDocumentHandleResolver& Handles,
		IAssetDocumentAtomicTransaction* ActiveTransaction) override
	{
		if (const FAssetDocumentUMGDecisionRow* Blocker =
			FindAssetDocumentUMGStableBlocker(
				Invocation.PrimitiveId, Invocation.Version))
		{
			return MakeAssetDocumentUMGFailure(
				*Blocker->StableBlockerDiagnostic,
				TEXT("The exact P-UMG decision remains a stable governance blocker."));
		}
		const FAssetDocumentPrimitiveDescriptor* Descriptor =
			FindDescriptor(Invocation);
		const FAssetDocumentUMGPrimitiveBinding* Binding =
			FindBinding(Invocation);
		if (Descriptor == nullptr || Binding == nullptr
			|| Binding->Handler == nullptr)
		{
			return MakeAssetDocumentUMGFailure(
				TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"),
				TEXT("No exact UMG primitive id and version binding exists."));
		}
		const FAssetDocumentContractValidationResult InvocationValidation =
			ValidateAssetDocumentPrimitiveInvocation(
				Invocation, *Descriptor);
		if (!InvocationValidation.bValid)
		{
			return MakeAssetDocumentUMGFailure(
				TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"),
				TEXT("The invocation does not match the frozen UMG descriptor."),
				InvocationValidation.Diagnostics);
		}
		if (Descriptor->bRequiresActiveTransaction
			&& !HasMatchingMutatingTransaction(
				Invocation, ActiveTransaction))
		{
			return MakeAssetDocumentUMGFailure(
				TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
				TEXT("A matching active Mutating transaction is required."));
		}
		return InvokeValidated(
			Invocation,
			*Descriptor,
			*Binding,
			Handles,
			ActiveTransaction);
	}

private:
	bool bAssemblyValid = false;
};
} // namespace AssetDocumentUMGPrimitiveProvider

FAssetDocumentPrimitiveOutcome MakeAssetDocumentUMGFailure(
	const TCHAR* Code,
	const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	if (Diagnostics.IsEmpty())
	{
		Outcome.Diagnostics.Add(
			AssetDocumentUMGPrimitiveProvider::MakeDiagnostic(Code, Message));
	}
	else
	{
		Outcome.Diagnostics.Append(
			Diagnostics.GetData(), Diagnostics.Num());
	}
	return Outcome;
}

FAssetDocumentPrimitiveOutcome MakeAssetDocumentUMGSuccess(
	FAssetDocumentRuntimeValueDto&& Value)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = MoveTemp(Value);
	return Outcome;
}

TArray<FAssetDocumentUMGPrimitiveBinding>
BuildAssetDocumentUMGHandlerTable()
{
	TArray<FAssetDocumentUMGPrimitiveBinding> Bindings;
	auto Append = [&Bindings](
		TConstArrayView<FAssetDocumentUMGPrimitiveBinding> Domain)
	{
		Bindings.Append(Domain.GetData(), Domain.Num());
	};
	Append(GetAssetDocumentUMGWidgetTreePrimitiveBindings());
	Append(GetAssetDocumentUMGDesignerBindingPrimitiveBindings());
	Append(GetAssetDocumentUMGWidgetAnimationPrimitiveBindings());
	Append(GetAssetDocumentUMGAnimationBindingPrimitiveBindings());
	Append(GetAssetDocumentUMGMovieScenePrimitiveBindings());
	return Bindings;
}

TConstArrayView<FAssetDocumentUMGPrimitiveBinding>
GetAssetDocumentUMGPrimitiveBindings()
{
	static const TArray<FAssetDocumentUMGPrimitiveBinding> Bindings =
		BuildAssetDocumentUMGHandlerTable();
	return Bindings;
}

bool IsAssetDocumentUMGPrimitiveAssemblyValid()
{
	const TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors =
		GetAssetDocumentUMGPrimitiveDescriptors();
	const TConstArrayView<FAssetDocumentUMGPrimitiveBinding> Bindings =
		GetAssetDocumentUMGPrimitiveBindings();
	if (Descriptors.Num() != 16 || Bindings.Num() != Descriptors.Num())
	{
		return false;
	}
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
	{
		int32 MatchCount = 0;
		for (const FAssetDocumentUMGPrimitiveBinding& Binding : Bindings)
		{
			MatchCount += Binding.Handler != nullptr
				&& Binding.Version == Descriptor.Version
				&& Binding.PrimitiveId == Descriptor.PrimitiveId;
		}
		if (MatchCount != 1)
		{
			return false;
		}
	}
	for (const FAssetDocumentUMGPrimitiveBinding& Binding : Bindings)
	{
		int32 MatchCount = 0;
		for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
		{
			MatchCount += Binding.Version == Descriptor.Version
				&& Binding.PrimitiveId == Descriptor.PrimitiveId;
		}
		if (Binding.Handler == nullptr || MatchCount != 1)
		{
			return false;
		}
	}
	return true;
}

TSharedRef<IAssetDocumentPrimitiveProvider>
CreateAssetDocumentUMGPrimitiveProvider()
{
	return MakeShared<AssetDocumentUMGPrimitiveProvider::FProvider>();
}
