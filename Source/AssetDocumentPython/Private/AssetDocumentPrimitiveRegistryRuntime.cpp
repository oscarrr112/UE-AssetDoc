// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentPrimitiveRegistryRuntime.h"

namespace
{
constexpr int32 MaxCatalogEntries = 4096;

FAssetDocumentRuntimeDiagnostic MakeRegistryDiagnostic(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/PrimitiveRegistry");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Diagnostic;
}

FAssetDocumentPrimitiveRegistryBuildResult MakeRejectedBuild(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentPrimitiveRegistryBuildResult Result;
	Result.Diagnostics.Add(MakeRegistryDiagnostic(Code, Message));
	return Result;
}

FAssetDocumentPrimitiveRegistryBuildResult MakeRejectedBuild(
	const FAssetDocumentContractValidationResult& Validation)
{
	FAssetDocumentPrimitiveRegistryBuildResult Result;
	Result.Diagnostics = Validation.Diagnostics;
	return Result;
}

bool ExceedsCatalogLimit(
	int32 DescriptorCount,
	int32 RequirementCount,
	int32 ProviderCount)
{
	return DescriptorCount > MaxCatalogEntries
		|| RequirementCount > MaxCatalogEntries
		|| ProviderCount > MaxCatalogEntries;
}

bool CollectProvider(
	const TSharedRef<IAssetDocumentPrimitiveProvider>& Provider,
	TArray<FAssetDocumentPrimitiveDescriptor>& OutDescriptors,
	TArray<FAssetDocumentProviderAvailability>& OutAvailability,
	TMap<FString, TSharedRef<IAssetDocumentPrimitiveProvider>>& OutProviders)
{
	const FString ProviderId = Provider->GetProviderId();
	if (ProviderId.IsEmpty() || OutProviders.Contains(ProviderId))
	{
		return false;
	}
	OutProviders.Add(ProviderId, Provider);
	OutDescriptors.Append(Provider->DescribePrimitives());
	OutAvailability.Add(Provider->InspectAvailability());
	return true;
}

bool CollectProviders(
	TConstArrayView<TSharedRef<IAssetDocumentPrimitiveProvider>> Providers,
	TArray<FAssetDocumentPrimitiveDescriptor>& OutDescriptors,
	TArray<FAssetDocumentProviderAvailability>& OutAvailability,
	TMap<FString, TSharedRef<IAssetDocumentPrimitiveProvider>>& OutProviders)
{
	for (const TSharedRef<IAssetDocumentPrimitiveProvider>& Provider : Providers)
	{
		if (!CollectProvider(Provider, OutDescriptors, OutAvailability, OutProviders))
		{
			return false;
		}
	}
	return true;
}

void SortCatalog(
	TArray<FAssetDocumentPrimitiveDescriptor>& Descriptors,
	TArray<FAssetDocumentProviderAvailability>& Providers)
{
	Descriptors.Sort([](const FAssetDocumentPrimitiveDescriptor& Left,
		const FAssetDocumentPrimitiveDescriptor& Right)
	{
		return Left.PrimitiveId == Right.PrimitiveId
			? Left.Version < Right.Version
			: Left.PrimitiveId < Right.PrimitiveId;
	});
	Providers.Sort([](const FAssetDocumentProviderAvailability& Left,
		const FAssetDocumentProviderAvailability& Right)
	{
		return Left.ProviderId < Right.ProviderId;
	});
}

FAssetDocumentPrimitiveResolution MakeResolutionFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentPrimitiveResolution Resolution;
	Resolution.Diagnostics.Add(MakeRegistryDiagnostic(Code, Message));
	return Resolution;
}

const FAssetDocumentPrimitiveDescriptor* FindExactDescriptor(
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors,
	const FAssetDocumentPrimitiveRequirement& Requirement)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
	{
		if (Descriptor.PrimitiveId == Requirement.PrimitiveId && Descriptor.Version == Requirement.Version)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

bool ContainsPrimitiveId(
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors,
	FStringView PrimitiveId)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
	{
		if (Descriptor.PrimitiveId == PrimitiveId)
		{
			return true;
		}
	}
	return false;
}
}

FAssetDocumentPrimitiveRegistryBuildResult FAssetDocumentPrimitiveRegistryRuntime::CreateFrozen(
	const FAssetDocumentPrimitiveRegistryBuildInput& Input)
{
	if (Input.RuntimeRevision == 0)
	{
		return MakeRejectedBuild(TEXT("AD.RUNTIME.REVISION_INVALID"), TEXT("Runtime revision must be nonzero."));
	}
	TArray<FAssetDocumentPrimitiveDescriptor> CollectedDescriptors;
	TArray<FAssetDocumentProviderAvailability> CollectedAvailability;
	TMap<FString, TSharedRef<IAssetDocumentPrimitiveProvider>> CollectedProviders;
	if (!CollectProviders(Input.Providers, CollectedDescriptors, CollectedAvailability, CollectedProviders))
	{
		return MakeRejectedBuild(TEXT("PrimitiveRegistry.ProviderCardinality"),
			TEXT("Provider ids must be non-empty and unique."));
	}
	if (ExceedsCatalogLimit(CollectedDescriptors.Num(), Input.Required.Num(),
		CollectedAvailability.Num()))
	{
		return MakeRejectedBuild(TEXT("AD.RUNTIME.HOST_TRANSPORT_LIMIT_EXCEEDED"),
			TEXT("Primitive catalog exceeds the Host transport limit."));
	}
	const FAssetDocumentContractValidationResult Validation = ValidateAssetDocumentPrimitiveFreezeInput(
		CollectedDescriptors, Input.Required, CollectedAvailability, Input.AuthorityCatalog);
	if (!Validation.bValid)
	{
		return MakeRejectedBuild(Validation);
	}
	SortCatalog(CollectedDescriptors, CollectedAvailability);
	TSharedPtr<FAssetDocumentPrimitiveRegistryRuntime> Registry =
		MakeShareable(new FAssetDocumentPrimitiveRegistryRuntime());
	Registry->RuntimeRevision = Input.RuntimeRevision;
	Registry->Descriptors = MoveTemp(CollectedDescriptors);
	Registry->ProviderAvailability = MoveTemp(CollectedAvailability);
	Registry->ProvidersById = MoveTemp(CollectedProviders);
	FAssetDocumentPrimitiveRegistryBuildResult Result;
	Result.Registry = MoveTemp(Registry);
	return Result;
}

FAssetDocumentPrimitiveRegistrySnapshot FAssetDocumentPrimitiveRegistryRuntime::Snapshot() const
{
	FAssetDocumentPrimitiveRegistrySnapshot Result;
	Result.RuntimeRevision = RuntimeRevision;
	Result.bFrozen = true;
	Result.Descriptors = Descriptors;
	Result.Providers = ProviderAvailability;
	return Result;
}

FAssetDocumentPrimitiveResolution FAssetDocumentPrimitiveRegistryRuntime::Resolve(
	const FAssetDocumentPrimitiveRequirement& Requirement) const
{
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindExactDescriptor(Descriptors, Requirement);
	if (Descriptor == nullptr)
	{
		return ContainsPrimitiveId(Descriptors, Requirement.PrimitiveId)
			? MakeResolutionFailure(TEXT("AD.RUNTIME.PRIMITIVE_VERSION_MISMATCH"),
				TEXT("The primitive id exists at a different version."))
			: MakeResolutionFailure(TEXT("AD.RUNTIME.PRIMITIVE_NOT_FOUND"),
				TEXT("The primitive id is not in the frozen registry."));
	}
	const TSharedPtr<IAssetDocumentPrimitiveProvider> Provider = FindProvider(Descriptor->ProviderId);
	if (!Provider.IsValid())
	{
		return MakeResolutionFailure(TEXT("AD.RUNTIME.PROVIDER_UNAVAILABLE"),
			TEXT("The frozen primitive provider is unavailable."));
	}
	FAssetDocumentPrimitiveResolution Resolution;
	Resolution.bResolved = true;
	Resolution.Descriptor = *Descriptor;
	Resolution.ProviderId = Descriptor->ProviderId;
	return Resolution;
}

TSharedPtr<IAssetDocumentPrimitiveProvider> FAssetDocumentPrimitiveRegistryRuntime::FindProvider(
	FStringView ProviderId) const
{
	const TSharedRef<IAssetDocumentPrimitiveProvider>* Provider = ProvidersById.Find(FString(ProviderId));
	return Provider != nullptr ? TSharedPtr<IAssetDocumentPrimitiveProvider>(*Provider) : nullptr;
}
