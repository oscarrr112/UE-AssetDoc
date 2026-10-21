// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentAIPrimitiveProvider.h"

#include "Provider/AssetDocumentAIPrimitiveCatalog.h"
#include "Provider/AssetDocumentAIPrimitiveHandler.h"
#include "Provider/AssetDocumentAIProviderContracts.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreePrimitives.h"
#include "EnvQuery/AssetDocumentEnvQueryPrimitives.h"
#include "StateTree/AssetDocumentStateTreePrimitives.h"

namespace AssetDocumentAIPrimitiveProvider
{
FAssetDocumentPrimitiveOutcome MakeFailure(
	const TCHAR* Code,
	const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics = {})
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	if (Diagnostics.IsEmpty())
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic = Outcome.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/Primitive");
		Diagnostic.Code = Code;
		Diagnostic.Message = Message;
	}
	else
	{
		Outcome.Diagnostics.Append(Diagnostics.GetData(), Diagnostics.Num());
	}
	return Outcome;
}

const FAssetDocumentPrimitiveDescriptor* FindDescriptor(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor :
		GetAssetDocumentAIPrimitiveDescriptors())
	{
		if (Descriptor.PrimitiveId == Invocation.PrimitiveId
			&& Descriptor.Version == Invocation.Version)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

namespace
{
constexpr const TCHAR* BlackboardV2InvocationDiagnosticNormalizationFingerprint = TEXT(
	"AD.P-AI.BLACKBOARD.V2.INVOCATION-DIAGNOSTIC-NORMALIZATION.v2|phase=AIProvider.InvalidContext|value=AIProvider.InvalidValueShape|transaction=AIProvider.InvalidTransaction|handle=AIProvider.InvalidHandle|v1=passthrough");

TOptional<FAssetDocumentPrimitiveOutcome> NormalizeBlackboardV2InvocationFailure(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	if (Invocation.Version != 2
		|| !Invocation.PrimitiveId.StartsWith(
			TEXT("ai.blackboard."), ESearchCase::CaseSensitive))
	{
		return {};
	}

	(void)BlackboardV2InvocationDiagnosticNormalizationFingerprint;
	auto Failure = [](const TCHAR* Code)
	{
		return MakeAssetDocumentAIPrimitiveFailure(
			EAssetDocumentPrimitiveOutcomeStatus::Failed,
			Code,
			TEXT("The Blackboard v2 invocation failed syntactic admission."));
	};

	const FAssetDocumentContractValidationResult DescriptorValidation =
		ValidateAssetDocumentPrimitiveDescriptor(Descriptor);
	const bool bCallableDisposition =
		Descriptor.Admission.Disposition
			!= EAssetDocumentPrimitiveDisposition::StableBlocker
		&& Descriptor.Admission.Disposition
			!= EAssetDocumentPrimitiveDisposition::GovernanceCandidate;
	if (!DescriptorValidation.bValid || !bCallableDisposition
		|| Descriptor.PrimitiveId != Invocation.PrimitiveId
		|| Descriptor.Version != Invocation.Version)
	{
		return Failure(TEXT("AIProvider.OperationRejected"));
	}
	if (!Descriptor.AllowedPhases.Contains(Invocation.Phase) || !IsInGameThread())
	{
		return Failure(TEXT("AIProvider.InvalidContext"));
	}
	if (Descriptor.bRequiresActiveTransaction && !Invocation.TransactionId.IsValid())
	{
		return Failure(TEXT("AIProvider.InvalidTransaction"));
	}
	if (!Invocation.RequestId.IsValid() || Invocation.RuntimeRevision == 0)
	{
		return Failure(TEXT("AIProvider.InvalidValueShape"));
	}

	const FAssetDocumentContractValidationResult InvocationValidation =
		ValidateAssetDocumentPrimitiveInvocation(Invocation, Descriptor);
	if (InvocationValidation.bValid)
	{
		return {};
	}
	if (InvocationValidation.Diagnostics.Num() != 1)
	{
		return Failure(TEXT("AIProvider.OperationRejected"));
	}

	const FString& RuntimeCode = InvocationValidation.Diagnostics[0].Code;
	if (RuntimeCode == TEXT("PrimitiveInvocation.InvalidIdentity")
		|| RuntimeCode == TEXT("PrimitiveInvocation.InvalidValueArguments"))
	{
		return Failure(TEXT("AIProvider.InvalidValueShape"));
	}
	if (RuntimeCode == TEXT("PrimitiveInvocation.InvalidHandleArguments"))
	{
		return Failure(TEXT("AIProvider.InvalidHandle"));
	}
	return Failure(TEXT("AIProvider.OperationRejected"));
}
} // namespace

class FProvider final : public IAssetDocumentPrimitiveProvider
{
public:
	virtual FString GetProviderId() const override
	{
		return TEXT("ai");
	}

	virtual TConstArrayView<FAssetDocumentPrimitiveDescriptor> DescribePrimitives() const override
	{
		// The Blackboard / BehaviorTree catalog, then the StateTree (ai.state_tree.*) and Environment Query
		// (ai.env_query.*) tables.
		static const TArray<FAssetDocumentPrimitiveDescriptor> Descriptors = []
		{
			TArray<FAssetDocumentPrimitiveDescriptor> Result(GetAssetDocumentAIPrimitiveDescriptors());
			Result.Append(UE::AssetDocument::Graph::StateTree::GetDescriptors());
			Result.Append(UE::AssetDocument::Graph::EnvQuery::GetDescriptors());
			return Result;
		}();
		return Descriptors;
	}

	virtual FAssetDocumentProviderAvailability InspectAvailability() const override
	{
		FAssetDocumentProviderAvailability Availability;
		Availability.ProviderId = GetProviderId();
		Availability.State = EnsureAssetDocumentAIProviderReady()
			? EAssetDocumentProviderAvailabilityState::Available
			: EAssetDocumentProviderAvailabilityState::Unavailable;
		if (Availability.State == EAssetDocumentProviderAvailabilityState::Unavailable)
		{
			FAssetDocumentRuntimeDiagnostic Diagnostic;
			Diagnostic.Path = TEXT("/Primitive");
			Diagnostic.Code = TEXT("AIProvider.AuthorityUnavailable");
			Diagnostic.Message =
				TEXT("The AI provider assembly or Editor-bound authority is unavailable.");
			Availability.Diagnostic = MoveTemp(Diagnostic);
		}
		return Availability;
	}

	virtual FAssetDocumentPrimitiveOutcome Invoke(
		const FAssetDocumentPrimitiveInvocation& Invocation,
		IAssetDocumentHandleResolver& Handles,
		IAssetDocumentAtomicTransaction* ActiveTransaction) override
	{
		if (Invocation.PrimitiveId.StartsWith(UE::AssetDocument::Graph::StateTree::PrimitivePrefix, ESearchCase::CaseSensitive))
		{
			return StateTree->Invoke(Invocation, Handles, ActiveTransaction);
		}
		if (Invocation.PrimitiveId.StartsWith(UE::AssetDocument::Graph::EnvQuery::PrimitivePrefix, ESearchCase::CaseSensitive))
		{
			return EnvQuery->Invoke(Invocation, Handles, ActiveTransaction);
		}
		const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(Invocation);
		if (Descriptor == nullptr)
		{
			return MakeFailure(
				TEXT("GenericProvider.UnknownPrimitive"),
				TEXT("No exact AI primitive id and version exists."));
		}
		if (Descriptor->Admission.Disposition
			== EAssetDocumentPrimitiveDisposition::StableBlocker)
		{
			return MakeAssetDocumentAIPrimitiveFailure(
				EAssetDocumentPrimitiveOutcomeStatus::Unavailable,
				*Descriptor->StableDiagnosticCodes[0],
				TEXT("This frozen G1 AI primitive has no callable authority."));
		}

		const FAssetDocumentAIPrimitiveBinding* Binding =
			FindAssetDocumentAIPrimitiveBinding(
				Invocation.PrimitiveId, Invocation.Version);
		if (Binding == nullptr || Binding->Handler == nullptr)
		{
			return MakeAssetDocumentAIPrimitiveFailure(
				EAssetDocumentPrimitiveOutcomeStatus::Failed,
				TEXT("AIProvider.AuthorityUnavailable"),
				TEXT("The ordinary AI primitive handler is not yet admitted."));
		}
		if (TOptional<FAssetDocumentPrimitiveOutcome> BlackboardV2Failure =
			NormalizeBlackboardV2InvocationFailure(Invocation, *Descriptor))
		{
			return MoveTemp(BlackboardV2Failure.GetValue());
		}

		const bool bInstall = Invocation.PrimitiveId == TEXT("ai.behavior_tree.graph.install.v1");
		if (((Invocation.PrimitiveId == TEXT("ai.behavior_tree.value_or_key.refresh.v1") || bInstall) && Invocation.Version == 1)
			|| (Invocation.PrimitiveId == TEXT("ai.behavior_tree.value_or_key.refresh.v2") && Invocation.Version == 2)
			|| (Invocation.PrimitiveId == TEXT("ai.behavior_tree.value_or_key.refresh.v3") && Invocation.Version == 3))
		{
			if (!ValidateAssetDocumentPrimitiveDescriptor(*Descriptor).bValid)
				return MakeFailure(TEXT("AIProvider.AuthorityUnavailable"), TEXT("The CurrentGraph descriptor is not admitted."));
			if (auto Rejected = bInstall ? RejectAssetDocumentAIBehaviorTreeGraphInstallInvocation(Invocation, ActiveTransaction)
				: RejectAssetDocumentAIBehaviorTreeValueOrKeyInvocation(Invocation, ActiveTransaction))
				return MoveTemp(Rejected.GetValue());
			const auto Validation = ValidateAssetDocumentPrimitiveInvocation(Invocation, *Descriptor);
			if (!Validation.bValid)
			{
				const bool bHandle = Validation.Diagnostics.ContainsByPredicate([](const auto& Diagnostic)
				{ return Diagnostic.Code == TEXT("PrimitiveInvocation.InvalidHandleArguments"); });
				return MakeFailure(bHandle ? TEXT("AIProvider.InvalidHandle") : TEXT("AIProvider.InvalidValueShape"),
					TEXT("The CurrentGraph invocation failed closed admission."));
			}
			return NormalizeAssetDocumentAIPrimitiveOutcome(Invocation, *Descriptor,
				Binding->Handler(Invocation, Handles, ActiveTransaction));
		}

		const bool bAuthoredReadback = (Invocation.PrimitiveId == TEXT("ai.behavior_tree.authored_graph.readback.v1")
			&& Invocation.Version == 1) || (Invocation.PrimitiveId == TEXT("ai.behavior_tree.authored_graph.readback.v2") && Invocation.Version == 2);
		const bool bDecoratorRefresh = Invocation.PrimitiveId == TEXT("ai.behavior_tree.decorator.refresh.v1")
			&& Invocation.Version == 1;
		const bool bSelectorBatch = (Invocation.PrimitiveId == TEXT("ai.behavior_tree.selector.resolve_batch.v1") && Invocation.Version == 1)
			|| (Invocation.PrimitiveId == TEXT("ai.behavior_tree.selector.resolve_batch.v2") && Invocation.Version == 2);
		if ((bAuthoredReadback || bDecoratorRefresh || bSelectorBatch) && (!Descriptor->AllowedPhases.Contains(Invocation.Phase) || !IsInGameThread()))
			return MakeFailure(TEXT("AIProvider.InvalidContext"), TEXT("The primitive requires its admitted phase and game thread."));
		if ((bDecoratorRefresh || bSelectorBatch) && !IsAssetDocumentAITransactionValid(Invocation, ActiveTransaction))
			return MakeFailure(TEXT("AIProvider.InvalidTransaction"), bSelectorBatch
				? TEXT("Selector batch requires its matching active transaction.")
				: TEXT("Decorator refresh requires its matching active transaction."));
		const FAssetDocumentContractValidationResult InvocationValidation =
			ValidateAssetDocumentPrimitiveInvocation(Invocation, *Descriptor);
		if (!InvocationValidation.bValid)
		{
			if (bAuthoredReadback || bDecoratorRefresh || bSelectorBatch)
			{
				const bool bHandle = InvocationValidation.Diagnostics.ContainsByPredicate([](const auto& Diagnostic)
				{ return Diagnostic.Code == TEXT("PrimitiveInvocation.InvalidHandleArguments"); });
				return MakeFailure(bHandle ? TEXT("AIProvider.InvalidHandle") : TEXT("AIProvider.InvalidValueShape"),
					TEXT("The invocation failed closed syntactic admission."));
			}
			return MakeAssetDocumentAIPrimitiveFailure(
				EAssetDocumentPrimitiveOutcomeStatus::Failed,
				TEXT("AIProvider.InvalidValueShape"),
				TEXT("The AI primitive invocation does not match its descriptor."),
				InvocationValidation.Diagnostics);
		}
		if (Descriptor->bRequiresActiveTransaction
			&& !IsAssetDocumentAITransactionValid(Invocation, ActiveTransaction))
		{
			return MakeAssetDocumentAIPrimitiveFailure(
				EAssetDocumentPrimitiveOutcomeStatus::Failed,
				TEXT("AIProvider.InvalidTransaction"),
				TEXT("The active transaction does not match the AI invocation."));
		}

		return NormalizeAssetDocumentAIPrimitiveOutcome(
			Invocation,
			*Descriptor,
			Binding->Handler(Invocation, Handles, ActiveTransaction));
	}

private:
	// ai.state_tree.* (AssetDocumentStateTreeCatalog.cpp): table-driven like the data / graph providers.
	const TSharedRef<IAssetDocumentPrimitiveProvider> StateTree = UE::AssetDocument::Graph::StateTree::CreateProvider();
	// ai.env_query.* (AssetDocumentEnvQueryCatalog.cpp): the same table plumbing.
	const TSharedRef<IAssetDocumentPrimitiveProvider> EnvQuery = UE::AssetDocument::Graph::EnvQuery::CreateProvider();
};
} // namespace AssetDocumentAIPrimitiveProvider

TSharedRef<IAssetDocumentPrimitiveProvider>
CreateAssetDocumentAIPrimitiveProvider()
{
	return MakeShared<AssetDocumentAIPrimitiveProvider::FProvider>();
}
