// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentRequiredPrimitiveProviderView.h"

namespace RequiredPrimitiveProviderViewLocal
{
class FRequiredPrimitiveProviderView final : public IAssetDocumentPrimitiveProvider
{
public:
	FRequiredPrimitiveProviderView(
		TSharedRef<IAssetDocumentPrimitiveProvider> InProvider,
		TArray<FAssetDocumentPrimitiveDescriptor> InDescriptors,
  FAssetDocumentPreparedInvocation InInvokePrepared)
		: Provider(MoveTemp(InProvider)), Descriptors(MoveTemp(InDescriptors)), InvokePrepared(MoveTemp(InInvokePrepared))
	{
	}

	FString GetProviderId() const override { return Provider->GetProviderId(); }
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> DescribePrimitives() const override
	{
		return Descriptors;
	}
	FAssetDocumentProviderAvailability InspectAvailability() const override
	{
		return Provider->InspectAvailability();
	}
	FAssetDocumentPrimitiveOutcome Invoke(
		const FAssetDocumentPrimitiveInvocation& Invocation,
		IAssetDocumentHandleResolver& Handles,
		IAssetDocumentAtomicTransaction* ActiveTransaction) override
	{
		for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
		{
			if (Descriptor.PrimitiveId == Invocation.PrimitiveId
				&& Descriptor.Version == Invocation.Version)
			{
				if (Descriptor.Access == EAssetDocumentPrimitiveAccess::Mutating && InvokePrepared)
    return InvokePrepared(Provider->GetProviderId(), Invocation, Handles, ActiveTransaction,
     [&]() { return Provider->Invoke(Invocation, Handles, ActiveTransaction); });
    return Provider->Invoke(Invocation, Handles, ActiveTransaction);
			}
		}
		FAssetDocumentPrimitiveOutcome Outcome;
		Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Unavailable;
		FAssetDocumentRuntimeDiagnostic& Diagnostic = Outcome.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Code = TEXT("AD.RUNTIME.PRIMITIVE_UNAVAILABLE");
		Diagnostic.Path = TEXT("/Primitive");
		Diagnostic.Message = TEXT("Primitive is not selected in the active provider projection.");
		return Outcome;
	}

private:
	TSharedRef<IAssetDocumentPrimitiveProvider> Provider;
	const TArray<FAssetDocumentPrimitiveDescriptor> Descriptors;
 const FAssetDocumentPreparedInvocation InvokePrepared;
};

FAssetDocumentRequiredPrimitiveProjectionResult Reject(const TCHAR* Message)
{
	FAssetDocumentRequiredPrimitiveProjectionResult Result;
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Code = TEXT("AD.RUNTIME.PRIMITIVE_UNAVAILABLE");
	Diagnostic.Path = TEXT("/PrimitiveRegistry");
	Diagnostic.Message = Message;
	return Result;
}

FAssetDocumentRuntimeDiagnostic MakeMissingRequirement(int32 Index,
	const FAssetDocumentPrimitiveRequirement& Requirement)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Code = TEXT("PrimitiveRegistry.MissingRequiredPrimitive");
	Diagnostic.Path = FString::Printf(TEXT("/PrimitiveRegistry/RequiredPrimitives/%d"), Index);
	Diagnostic.Message = FString::Printf(
		TEXT("Route '%s' requires primitive '%s' version %u, but no active provider describes it."),
		*Requirement.RequiredByRouteId, *Requirement.PrimitiveId, Requirement.Version);
	auto AddString = [&Diagnostic](const TCHAR* Key, const FString& Value)
	{
		FAssetDocumentRuntimeDiagnosticContext& Context = Diagnostic.Context.AddDefaulted_GetRef();
		Context.Key = Key;
		Context.Value.Kind = EAssetDocumentRuntimeValueKind::String;
		Context.Value.StringValue = Value;
	};
	AddString(TEXT("PrimitiveId"), Requirement.PrimitiveId);
	FAssetDocumentRuntimeDiagnosticContext& Version = Diagnostic.Context.AddDefaulted_GetRef();
	Version.Key = TEXT("Version");
	Version.Value.Kind = EAssetDocumentRuntimeValueKind::Integer;
	Version.Value.IntegerValue = Requirement.Version;
	AddString(TEXT("RequiredByRouteId"), Requirement.RequiredByRouteId);
	return Diagnostic;
}

bool SameRequirement(const FAssetDocumentPrimitiveRequirement& A,
	const FAssetDocumentPrimitiveRequirement& B)
{
	return A.PrimitiveId == B.PrimitiveId && A.Version == B.Version
		&& A.RequiredByRouteId == B.RequiredByRouteId;
}
}

FAssetDocumentRequiredPrimitiveProjectionResult BuildAssetDocumentRequiredPrimitiveProjection(
	TConstArrayView<TSharedRef<IAssetDocumentPrimitiveProvider>> Providers,
	TConstArrayView<FAssetDocumentPrimitiveRequirement> Seed,
	TConstArrayView<FAssetDocumentAuthorityCandidateRecord> AuthorityCatalog,
 TConstArrayView<FAssetDocumentMutationPreparationBinding> PreparationBindings,
 FAssetDocumentPreparedInvocation InvokePrepared)
{
	using namespace RequiredPrimitiveProviderViewLocal;
	if (Providers.IsEmpty() || Seed.IsEmpty())
	{
		return Reject(TEXT("Providers and seed requirements must be non-empty."));
	}
	TSet<FString> ProviderIds;
	TArray<FAssetDocumentPrimitiveDescriptor> Catalog;
	TArray<FAssetDocumentProviderAvailability> Availability;
	for (const TSharedRef<IAssetDocumentPrimitiveProvider>& Provider : Providers)
	{
		const FString Id = Provider->GetProviderId();
		const FAssetDocumentProviderAvailability State = Provider->InspectAvailability();
		if (Id.IsEmpty() || ProviderIds.Contains(Id) || State.ProviderId != Id
			|| State.State != EAssetDocumentProviderAvailabilityState::Available)
		{
			return Reject(TEXT("Provider identity must be unique, exact and available."));
		}
		ProviderIds.Add(Id);
		Availability.Add(State);
		for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Provider->DescribePrimitives())
		{
			if (Descriptor.ProviderId != Id || Catalog.ContainsByPredicate(
				[&](const FAssetDocumentPrimitiveDescriptor& Existing)
				{
					return Existing.PrimitiveId == Descriptor.PrimitiveId
						&& Existing.Version == Descriptor.Version;
				}))
			{
				return Reject(TEXT("Descriptor identity is duplicated or belongs to another provider."));
			}
			Catalog.Add(Descriptor);
		}
	}

	TSharedRef<FAssetDocumentRequiredPrimitiveProjection> Candidate =
		MakeShared<FAssetDocumentRequiredPrimitiveProjection>();
	TArray<FAssetDocumentRuntimeDiagnostic> Missing;
	for (int32 Index = 0; Index < Seed.Num(); ++Index)
	{
		const FAssetDocumentPrimitiveRequirement& Requirement = Seed[Index];
		if (Requirement.PrimitiveId.IsEmpty() || Requirement.Version == 0
			|| Requirement.RequiredByRouteId.IsEmpty())
		{
			return Reject(TEXT("Seed requirements must contain exact non-empty tuples."));
		}
		for (int32 Previous = 0; Previous < Index; ++Previous)
		{
			if (SameRequirement(Requirement, Seed[Previous]))
			{
				return Reject(TEXT("Seed requirements must not contain duplicate tuples."));
			}
		}
		const FAssetDocumentPrimitiveDescriptor* Match = Catalog.FindByPredicate(
			[&](const FAssetDocumentPrimitiveDescriptor& Descriptor)
			{
				return Descriptor.PrimitiveId == Requirement.PrimitiveId
					&& Descriptor.Version == Requirement.Version;
			});
		if (Match == nullptr)
		{
			Missing.Add(MakeMissingRequirement(Index, Requirement));
			continue;
		}
		FAssetDocumentPrimitiveDescriptor* Selected = Candidate->Descriptors.FindByPredicate(
			[&](const FAssetDocumentPrimitiveDescriptor& Descriptor)
			{
				return Descriptor.PrimitiveId == Match->PrimitiveId && Descriptor.Version == Match->Version;
			});
		if (Selected == nullptr)
		{
			// Consumers are the routes that declare the primitive in this revision;
			// the provider's own consumer list is structural only.
			Selected = &Candidate->Descriptors.Add_GetRef(*Match);
			Selected->RequiredConsumerIds.Reset();
		}
		Selected->RequiredConsumerIds.AddUnique(Requirement.RequiredByRouteId);
		Candidate->Requirements.Add(Requirement);
	}
	if (!Missing.IsEmpty())
	{
		FAssetDocumentRequiredPrimitiveProjectionResult Result;
		Result.Diagnostics = MoveTemp(Missing);
		return Result;
	}
	for (FAssetDocumentPrimitiveDescriptor& Descriptor : Candidate->Descriptors)
	{
		Descriptor.RequiredConsumerIds.Sort();
	}
	Candidate->Descriptors.Sort([](const FAssetDocumentPrimitiveDescriptor& A,
		const FAssetDocumentPrimitiveDescriptor& B)
	{
		return A.PrimitiveId == B.PrimitiveId ? A.Version < B.Version : A.PrimitiveId < B.PrimitiveId;
	});
	Candidate->Requirements.Sort([](const FAssetDocumentPrimitiveRequirement& A,
		const FAssetDocumentPrimitiveRequirement& B)
	{
		if (A.PrimitiveId != B.PrimitiveId) { return A.PrimitiveId < B.PrimitiveId; }
		return A.Version == B.Version ? A.RequiredByRouteId < B.RequiredByRouteId : A.Version < B.Version;
	});
	Availability.Sort([](const FAssetDocumentProviderAvailability& A,
		const FAssetDocumentProviderAvailability& B) { return A.ProviderId < B.ProviderId; });
	for (const FAssetDocumentProviderAvailability& State : Availability)
	{
		TArray<FAssetDocumentPrimitiveDescriptor> Selected;
		for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Candidate->Descriptors)
		{
			if (Descriptor.ProviderId == State.ProviderId) { Selected.Add(Descriptor); }
		}
		if (Selected.IsEmpty()) { continue; }
		const TSharedRef<IAssetDocumentPrimitiveProvider>* Provider = Providers.FindByPredicate(
			[&](const TSharedRef<IAssetDocumentPrimitiveProvider>& Item)
			{ return Item->GetProviderId() == State.ProviderId; });
		Candidate->Availability.Add(State);
		auto View = MakeShared<FRequiredPrimitiveProviderView>(*Provider, MoveTemp(Selected), InvokePrepared);
  Candidate->Providers.Add(View);
  for (const auto& Binding : PreparationBindings)
  {
   if (&Binding.Provider.Get() == &Provider->Get())
   {
    if (!Binding.Prepare || Candidate->PreparationViews.ContainsByPredicate([&](const auto& Item){ return Item.Provider->GetProviderId() == State.ProviderId; }))
     return Reject(TEXT("Preparation must bind exactly once to a selected provider instance."));
    Candidate->PreparationViews.Add({View, Binding.Prepare});
   }
  }
	}
	if (Candidate->PreparationViews.Num() != PreparationBindings.Num())
  return Reject(TEXT("Every preparation binding must belong to one selected provider instance."));
 const FAssetDocumentContractValidationResult Validation = ValidateAssetDocumentPrimitiveFreezeInput(
		Candidate->Descriptors, Candidate->Requirements, Candidate->Availability, AuthorityCatalog);
	FAssetDocumentRequiredPrimitiveProjectionResult Result;
	if (!Validation.bValid)
	{
		Result.Diagnostics = Validation.Diagnostics;
		return Result;
	}
	Result.Projection = Candidate;
	return Result;
}
