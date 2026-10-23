// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentGenericRuntimeBootstrap.h"
#include "AssetDocumentRequiredPrimitiveProviderView.h"
#include "AssetDocumentAnimationRequestBinding.h"
#include "AssetDocumentStagedMontageReadBinding.h"
#include "AssetDocumentStagedBehaviorTreeReadBinding.h"

#include "AssetDocumentCoreRuntimeComposition.h"
#include "AssetDocumentAIPrimitiveProvider.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentGraphProvider.h"
#include "AssetDocumentWidgetProvider.h"
#include "AssetDocumentDataProvider.h"
#include "AssetDocumentAudioProvider.h"
#include "AssetDocumentAnimationAssetProvider.h"
#include "AssetDocumentNiagaraProvider.h"
#include "AssetDocumentRigProvider.h"
#include "AssetDocumentPhysicsProvider.h"
#include "AssetDocumentSequencerProvider.h"
#include "AssetDocumentEditorMutationPreparation.h"
#include "AssetDocumentGenericRuntimeRoute.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentPythonRuntime.h"
#include "AssetDocumentService.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Runtime/AssetDocumentAuthorityExceptionRegistry.h"
#include "AssetDocumentPluginPaths.h"

namespace
{
constexpr uint64 GenericRuntimeRevision = 2;

#if WITH_DEV_AUTOMATION_TESTS
FAssetDocumentGenericRuntimeBootstrap*
	ActiveGenericRuntimeBootstrapForTests = nullptr;
#endif

class FNoCancellation final : public IAssetDocumentCancellationView
{
public:
	bool IsCancellationRequested() const override { return false; }
};

class FNoopObserver final : public IAssetDocumentRequestObserver
{
public:
	void OnPhaseChanged(
		FGuid RequestId,
		EAssetDocumentRuntimeRequestPhase Phase) override
	{
		(void)RequestId;
		(void)Phase;
	}
};

class FFrozenRegistryView final : public IAssetDocumentPrimitiveRegistry
{
public:
	explicit FFrozenRegistryView(
		const IAssetDocumentPrimitiveRegistry& InRegistry)
		: Registry(InRegistry)
	{
	}

	FAssetDocumentPrimitiveRegistrySnapshot Snapshot() const override
	{
		return Registry.Snapshot();
	}

	FAssetDocumentPrimitiveResolution Resolve(
		const FAssetDocumentPrimitiveRequirement& Requirement) const override
	{
		return Registry.Resolve(Requirement);
	}

private:
	const IAssetDocumentPrimitiveRegistry& Registry;
};

FAssetDocumentPythonRuntimeConfiguration MakeRuntimeConfiguration()
{
	FAssetDocumentPythonRuntimeConfiguration Configuration;
	Configuration.RepositoryRoot =
		GetAssetDocumentPluginBaseDir();
	Configuration.PackageRootRelativePath = TEXT("Content/Python");
	return Configuration;
}

FAssetDocumentPythonRevisionCandidate MakeRevisionCandidate()
{
	FAssetDocumentPythonRevisionCandidate Candidate;
	Candidate.RuntimeRevision = GenericRuntimeRevision;
	Candidate.EntryModuleRelativePath = TEXT("asset_document/bootstrap.py");
	Candidate.AuthorityCatalog.Append(GetFrozenAuthorityExceptionRecords());
	return Candidate;
}

bool HasExactActiveCatalog(
	const IAssetDocumentPythonRuntime& Runtime,
	const FAssetDocumentRequiredPrimitiveProjection& Projection)
{
	const IAssetDocumentPrimitiveRegistry* Registry = Runtime.GetActivePrimitiveRegistry();
	if (Registry == nullptr || Projection.Requirements.IsEmpty()) { return false; }
	const FAssetDocumentPrimitiveRegistrySnapshot Snapshot = Registry->Snapshot();
	if (!Snapshot.bFrozen || Snapshot.RuntimeRevision != GenericRuntimeRevision
		|| Snapshot.Providers.Num() != Projection.Availability.Num()
		|| Snapshot.Descriptors.Num() != Projection.Descriptors.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Snapshot.Providers.Num(); ++Index)
	{
		if (Snapshot.Providers[Index].ProviderId != Projection.Availability[Index].ProviderId
			|| Snapshot.Providers[Index].State != Projection.Availability[Index].State)
		{
			return false;
		}
	}
	for (int32 Index = 0; Index < Snapshot.Descriptors.Num(); ++Index)
	{
		const FAssetDocumentPrimitiveDescriptor& Actual = Snapshot.Descriptors[Index];
		const FAssetDocumentPrimitiveDescriptor& Expected = Projection.Descriptors[Index];
		if (Actual.PrimitiveId != Expected.PrimitiveId || Actual.Version != Expected.Version
			|| Actual.ProviderId != Expected.ProviderId
			|| Actual.RequiredConsumerIds != Expected.RequiredConsumerIds
			|| Actual.Admission.Disposition != Expected.Admission.Disposition
			|| Actual.Admission.AuthorityFingerprint != Expected.Admission.AuthorityFingerprint
			|| Actual.Admission.ExportedAuthoritativeApi != Expected.Admission.ExportedAuthoritativeApi)
		{
			return false;
		}
	}
	if (!ValidateAssetDocumentPrimitiveFreezeInput(Snapshot.Descriptors,
		Projection.Requirements, Snapshot.Providers, GetFrozenAuthorityExceptionRecords()).bValid)
	{
		return false;
	}
	for (const FAssetDocumentPrimitiveRequirement& Requirement : Projection.Requirements)
	{
		if (!Registry->Resolve(Requirement).bResolved) { return false; }
	}
	return true;
}

void LogActivationDiagnostics(
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Diagnostics)
	{
		UE_LOG(LogTemp, Error, TEXT("%s: %s"),
			*Diagnostic.Code, *Diagnostic.Message);
	}
}
}

struct FAssetDocumentGenericRuntimeBootstrap::FImpl
{
	enum class EActivationState : uint8
	{
		Inactive,
		WaitingForPython,
		Active,
		TerminalFailure,
	};

	TUniquePtr<IAssetDocumentPythonRuntime> Runtime;
	TArray<TSharedPtr<IAssetDocumentPrimitiveProvider>> Providers;
	TSharedPtr<FAssetDocumentAnimationRequestBinding> AnimationRequestBinding;
	TSharedPtr<FAssetDocumentStagedMontageReadBinding> StagedMontageReadBinding;
	TSharedPtr<FAssetDocumentStagedBehaviorTreeReadBinding> StagedBehaviorTreeReadBinding;
	TUniquePtr<FFrozenRegistryView> RegistryView;
 TSharedPtr<const FAssetDocumentRequiredPrimitiveProjection> PreparationProjection;
	// Declared set of the last activated revision; kept across Deactivate().
	TArray<FAssetDocumentPrimitiveRequirement> DeclaredRequirements;
	TUniquePtr<FAssetDocumentCoreRuntimeComposition> Composition;
	TSharedPtr<FAssetDocumentGenericRuntimeRoute> Route;
	FNoCancellation Cancellation;
	FNoopObserver Observer;
	FDelegateHandle EnginePreExitHandle;
	bool bRouteBound = false;
	EActivationState ActivationState = EActivationState::Inactive;
};

FAssetDocumentGenericRuntimeBootstrap::
	FAssetDocumentGenericRuntimeBootstrap()
	: Impl(MakeUnique<FImpl>())
{
	Impl->EnginePreExitHandle = FCoreDelegates::OnEnginePreExit.AddRaw(
		this, &FAssetDocumentGenericRuntimeBootstrap::Deactivate);
#if WITH_DEV_AUTOMATION_TESTS
	ActiveGenericRuntimeBootstrapForTests = this;
#endif
}

FAssetDocumentGenericRuntimeBootstrap::
	~FAssetDocumentGenericRuntimeBootstrap()
{
	if (Impl->EnginePreExitHandle.IsValid())
	{
		FCoreDelegates::OnEnginePreExit.Remove(
			Impl->EnginePreExitHandle);
		Impl->EnginePreExitHandle.Reset();
	}
	Deactivate();
#if WITH_DEV_AUTOMATION_TESTS
	if (ActiveGenericRuntimeBootstrapForTests == this)
	{
		ActiveGenericRuntimeBootstrapForTests = nullptr;
	}
#endif
}

bool FAssetDocumentGenericRuntimeBootstrap::Activate()
{
	if (IsActive())
	{
		return true;
	}
	if (Impl->ActivationState == FImpl::EActivationState::TerminalFailure)
	{
		return false;
	}
	return InitializeRuntimeBinding()
		&& ActivateFrozenRevision()
		&& ComposeAndBindRoute();
}

bool FAssetDocumentGenericRuntimeBootstrap::InitializeRuntimeBinding()
{
	Impl->Runtime =
		CreateAssetDocumentPythonRuntime(MakeRuntimeConfiguration());
	const FAssetDocumentContractValidationResult Binding =
		Impl->Runtime->InitializePythonBinding();
	if (!Binding.bValid)
	{
		const bool bPythonNotReady = Binding.Diagnostics.Num() == 1
			&& Binding.Diagnostics[0].Code
				== TEXT("AD.RUNTIME.PYTHON_NOT_READY");
		if (!bPythonNotReady)
		{
			LogActivationDiagnostics(Binding.Diagnostics);
		}
		Deactivate();
		Impl->ActivationState = bPythonNotReady
			? FImpl::EActivationState::WaitingForPython
			: FImpl::EActivationState::TerminalFailure;
		return false;
	}
	return true;
}

bool FAssetDocumentGenericRuntimeBootstrap::ActivateFrozenRevision()
{
	Impl->AnimationRequestBinding = MakeShared<FAssetDocumentAnimationRequestBinding>();
	Impl->StagedMontageReadBinding = MakeShared<FAssetDocumentStagedMontageReadBinding>(CreateAssetDocumentEditorPrimitiveProvider());
	Impl->StagedBehaviorTreeReadBinding = MakeShared<FAssetDocumentStagedBehaviorTreeReadBinding>(Impl->StagedMontageReadBinding.ToSharedRef());
	const TArray<TSharedRef<IAssetDocumentPrimitiveProvider>> Providers = {
		Impl->StagedBehaviorTreeReadBinding.ToSharedRef(),
		WrapAssetDocumentBehaviorTreeStagedAIReadProvider(CreateAssetDocumentAIPrimitiveProvider(), Impl->StagedBehaviorTreeReadBinding.ToSharedRef()),
		Impl->AnimationRequestBinding.ToSharedRef(),
		// Blueprint graph and member edits; whole-package snapshot rollback.
		CreateAssetDocumentGraphPrimitiveProvider(),
		CreateAssetDocumentBlueprintMemberPrimitiveProvider(),
		// Widget Blueprint Designer tree, bindings and animations; whole-package snapshot rollback.
		CreateAssetDocumentWidgetPrimitiveProvider(),
		// Data table / curve table / string table rows; whole-package snapshot rollback.
		CreateAssetDocumentDataPrimitiveProvider(),
		// Skeleton / blend space / pose asset / anim composite editors; whole-package snapshot rollback.
		CreateAssetDocumentAnimationAssetPrimitiveProvider(),
		// Niagara systems / emitters / scripts / parameter collections.
		CreateAssetDocumentNiagaraPrimitiveProvider(),
		// MetaSound Source / Patch documents.
		CreateAssetDocumentAudioPrimitiveProvider(),
		// IK Rig / IK Retargeter documents; whole-package snapshot rollback.
		CreateAssetDocumentRigPrimitiveProvider(),
		// Physics Asset documents; whole-package snapshot rollback.
		CreateAssetDocumentPhysicsPrimitiveProvider(),
		// Level Sequence documents.
		CreateAssetDocumentSequencerPrimitiveProvider()};
	const TArray<FAssetDocumentMutationPreparationBinding> PreparationBindings{
		{Providers[0], PrepareAssetDocumentEditorMutationBatch},
		{Providers[2], [Binding = Impl->AnimationRequestBinding](const auto& Request, auto Intents, auto& Batch)
			{ return Binding->Prepare(Request, Intents, Batch); }}};
	const FAssetDocumentPreparedInvocation InvokePrepared =
		[this](const FString& ProviderId, const FAssetDocumentPrimitiveInvocation& Invocation,
			IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Transaction,
			TFunctionRef<FAssetDocumentPrimitiveOutcome()> Forward)
		{
			return Impl->Route ? Impl->Route->InvokePreparedMutation(ProviderId, Invocation, Handles, Transaction, Forward) : Forward();
		};
	// The required set comes from the revision's Python declarations, read after
	// import and before the registry freezes.
	TSharedPtr<const FAssetDocumentRequiredPrimitiveProjection> Projection;
	FAssetDocumentPythonRevisionCandidate Candidate = MakeRevisionCandidate();
	Candidate.DeriveRegistryInput = [&](TConstArrayView<FAssetDocumentPrimitiveRequirement> Declared,
		const FAssetDocumentRuntimeValueDto& Registrations,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics) -> TOptional<FAssetDocumentPythonDerivedRegistryInput>
	{
		(void)Registrations;
		const FAssetDocumentRequiredPrimitiveProjectionResult Projected =
			BuildAssetDocumentRequiredPrimitiveProjection(Providers, Declared,
				GetFrozenAuthorityExceptionRecords(), PreparationBindings, InvokePrepared);
		if (!Projected.Projection.IsValid())
		{
			OutDiagnostics.Append(Projected.Diagnostics);
			return NullOpt;
		}
		Projection = Projected.Projection;
		return FAssetDocumentPythonDerivedRegistryInput{Projection->Requirements, Projection->Providers};
	};
	const FAssetDocumentPythonRevisionActivationResult Activation =
		Impl->Runtime->TryActivateRevision(Candidate);
	if (!Activation.bActivated)
	{
		LogActivationDiagnostics(Activation.Diagnostics);
		Deactivate();
		Impl->ActivationState = FImpl::EActivationState::TerminalFailure;
		return false;
	}
	if (!Projection.IsValid() || !HasExactActiveCatalog(*Impl->Runtime, *Projection))
	{
		UE_LOG(LogTemp, Error, TEXT("Generic runtime catalog validation failed."));
		Deactivate();
		Impl->ActivationState = FImpl::EActivationState::TerminalFailure;
		return false;
	}
	Impl->PreparationProjection = Projection;
	Impl->DeclaredRequirements = Projection->Requirements;
	for (const TSharedRef<IAssetDocumentPrimitiveProvider>& Provider : Projection->Providers)
	{
		Impl->Providers.Add(Provider);
	}
	return true;
}

bool FAssetDocumentGenericRuntimeBootstrap::ComposeAndBindRoute()
{
	const IAssetDocumentPrimitiveRegistry* Registry =
		Impl->Runtime->GetActivePrimitiveRegistry();
	check(Registry != nullptr);
	Impl->RegistryView = MakeUnique<FFrozenRegistryView>(*Registry);
	Impl->Route =
		MakeShared<FAssetDocumentGenericRuntimeRoute>(*Impl->Runtime);
 Impl->Route->BindMutationPreparationProjection(Impl->PreparationProjection);
	Impl->Route->BindAnimationRequestProvider(Impl->AnimationRequestBinding, Impl->StagedMontageReadBinding);
	Impl->Route->BindBehaviorTreeStagedReadProvider(Impl->StagedBehaviorTreeReadBinding);
	TArray<FAssetDocumentRuntimeTargetActivation> Activations;
	TArray<FAssetDocumentRuntimeDiagnostic> ActivationDiagnostics;
	for (const auto& Row : Impl->Runtime->GetActiveRuntimeRegistrationProjection().Elements)
	{
		auto Activation = ParseAndValidateRuntimeTargetActivation(
			Row, GetAssetDocumentEditorTargetLifecycleCatalog(), ActivationDiagnostics);
		if (!Activation.IsSet())
		{
			LogActivationDiagnostics(ActivationDiagnostics);
			Deactivate();
			Impl->ActivationState = FImpl::EActivationState::TerminalFailure;
			return false;
		}
		Activations.Add(MoveTemp(Activation.GetValue()));
	}
	const auto Bound = Impl->Route->BindRuntimeTargetActivations(MoveTemp(Activations));
	if (!Bound.bValid)
	{
		LogActivationDiagnostics(Bound.Diagnostics);
		Deactivate();
		Impl->ActivationState = FImpl::EActivationState::TerminalFailure;
		return false;
	}
	FAssetDocumentCoreRuntimeDependencies Dependencies{
		*Impl->Route,
		*Impl->RegistryView,
		Impl->Cancellation,
		Impl->Observer,
		[this](
			const FAssetDocumentRequestContextMetadata& Metadata,
			const FGuid& TransactionId,
			IAssetDocumentAtomicTransaction* ActiveTransaction,
			TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
		{
			IAssetDocumentAtomicTransaction* const RoutedTransaction =
				Impl->Route->ResolveTransactionForPhase(
					Metadata, ActiveTransaction, OutDiagnostics);
			if (ActiveTransaction != nullptr && RoutedTransaction == nullptr)
			{
				return TUniquePtr<IAssetDocumentNativeRequestContext>();
			}
			const FGuid HandleScopeId = TransactionId.IsValid()
				? TransactionId
				: FGuid::NewGuid();
			FAssetDocumentPythonRequestContextCreateResult Result =
				Impl->Runtime->CreateRequestContext(
					Metadata, HandleScopeId, RoutedTransaction);
			OutDiagnostics.Append(Result.Diagnostics);
			return MoveTemp(Result.Context);
		},
		[this](const FAssetDocumentHandleInvalidation& Event)
		{
			return Impl->Runtime->InvalidateHandles(Event);
		}};
	Impl->Composition =
		MakeUnique<FAssetDocumentCoreRuntimeComposition>(
			Dependencies);
	if (!Impl->Composition->IsValid())
	{
		Deactivate();
		Impl->ActivationState = FImpl::EActivationState::TerminalFailure;
		return false;
	}
	Impl->Route->BindComposition(*Impl->Composition);
	FAssetDocumentModule::Get().GetService().BindGenericRuntimeRoute(
		Impl->Route.ToSharedRef());
	Impl->bRouteBound = true;
	Impl->ActivationState = FImpl::EActivationState::Active;
	return true;
}

void FAssetDocumentGenericRuntimeBootstrap::Deactivate()
{
	if (Impl->bRouteBound && Impl->Route.IsValid()
		&& FAssetDocumentModule::IsAvailable())
	{
		FAssetDocumentModule::Get().GetService().UnbindGenericRuntimeRoute(
			Impl->Route.ToSharedRef());
	}
	Impl->bRouteBound = false;
	if (Impl->Route.IsValid())
	{
		Impl->Route->UnbindComposition();
	}
	Impl->Route.Reset();
	Impl->Composition.Reset();
	Impl->RegistryView.Reset();
	Impl->Providers.Reset();
 Impl->PreparationProjection.Reset();
	Impl->AnimationRequestBinding.Reset();
	Impl->StagedMontageReadBinding.Reset();
	Impl->StagedBehaviorTreeReadBinding.Reset();
	if (Impl->Runtime.IsValid())
	{
		Impl->Runtime->ShutdownPythonBinding();
		Impl->Runtime.Reset();
	}
}

bool FAssetDocumentGenericRuntimeBootstrap::IsActive() const
{
	return Impl->ActivationState == FImpl::EActivationState::Active
		&& GetActiveRuntimeCount() == 1
		&& Impl->PreparationProjection.IsValid()
		&& GetProviderCount() > 0
		&& GetProviderCount() == Impl->PreparationProjection->Providers.Num()
		&& GetCoreCompositionCount() == 1
		&& GetRouteBindingCount() == 1;
}

bool FAssetDocumentGenericRuntimeBootstrap::IsWaitingForPython() const
{
	return Impl->ActivationState
		== FImpl::EActivationState::WaitingForPython;
}

int32 FAssetDocumentGenericRuntimeBootstrap::GetActiveRuntimeCount() const
{
	return Impl->Runtime.IsValid()
		&& Impl->Runtime->GetActiveRuntimeRevision() != 0 ? 1 : 0;
}

int32 FAssetDocumentGenericRuntimeBootstrap::GetProviderCount() const
{
	int32 ProviderCount = 0;
	for (const TSharedPtr<IAssetDocumentPrimitiveProvider>& Provider :
		Impl->Providers)
	{
		ProviderCount += Provider.IsValid() ? 1 : 0;
	}
	return ProviderCount;
}

int32 FAssetDocumentGenericRuntimeBootstrap::GetCoreCompositionCount() const
{
	return Impl->Composition.IsValid() ? 1 : 0;
}

int32 FAssetDocumentGenericRuntimeBootstrap::GetRouteBindingCount() const
{
	return Impl->bRouteBound && Impl->Route.IsValid() ? 1 : 0;
}

TArray<FAssetDocumentPrimitiveRequirement>
FAssetDocumentGenericRuntimeBootstrap::GetDeclaredRequirements() const
{
	return Impl->DeclaredRequirements;
}

#if WITH_DEV_AUTOMATION_TESTS
FAssetDocumentGenericRuntimeBootstrap*
GetAssetDocumentActiveGenericRuntimeBootstrapForTests()
{
	return ActiveGenericRuntimeBootstrapForTests;
}

uint64 GetAssetDocumentActiveGenericRuntimeRevisionForTests()
{
	return ActiveGenericRuntimeBootstrapForTests != nullptr
		&& ActiveGenericRuntimeBootstrapForTests->IsActive()
		? GenericRuntimeRevision : 0;
}

TArray<FAssetDocumentPrimitiveRequirement> GetAssetDocumentGenericRequirementsForTests()
{
	return ActiveGenericRuntimeBootstrapForTests != nullptr
		? ActiveGenericRuntimeBootstrapForTests->GetDeclaredRequirements()
		: TArray<FAssetDocumentPrimitiveRequirement>();
}
int32 GetAssetDocumentGenericRequirementCountForTests()
{
	return GetAssetDocumentGenericRequirementsForTests().Num();
}
// Generic route plus Blackboard rows of the active declared set (no Animation/BehaviorTree).
TArray<FAssetDocumentPrimitiveRequirement> GetAssetDocumentLegacyGenericRequirementsForTests()
{
	TArray<FAssetDocumentPrimitiveRequirement> Requirements = GetAssetDocumentGenericRequirementsForTests();
	Requirements.RemoveAll([](const FAssetDocumentPrimitiveRequirement& Requirement)
	{
		return Requirement.RequiredByRouteId != TEXT("route.generic.v1")
			&& !Requirement.PrimitiveId.StartsWith(TEXT("ai.blackboard."));
	});
	return Requirements;
}

#endif
