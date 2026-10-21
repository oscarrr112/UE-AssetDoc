// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentCoreRuntimeComposition.h"

#include "Runtime/AssetDocumentPackageSnapshotRollbackAction.h"
#include "Runtime/AssetDocumentPackageStaging.h"
#include "Runtime/AssetDocumentPersistenceJournal.h"
#include "Runtime/AssetDocumentPersistenceRuntime.h"
#include "Runtime/AssetDocumentRequestCoordinatorRuntime.h"
#include "Runtime/AssetDocumentSidecarByteIo.h"
#include "Runtime/AssetDocumentTypedSyncRuntime.h"

#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
FAssetDocumentRuntimeDiagnostic MakeCoreRuntimeCompositionDiagnostic(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/CoreRuntimeComposition");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Error;
	return Diagnostic;
}

FAssetDocumentRuntimeDiagnostic MakeCorePersistenceTargetDiagnostic(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic =
		MakeCoreRuntimeCompositionDiagnostic(Code, Message);
	Diagnostic.Path = TEXT("/CoreRuntimeComposition/PersistenceTarget");
	return Diagnostic;
}

FAssetDocumentRuntimeResultDto MakeCoreRuntimeCompositionNotImplementedResult(
	const FAssetDocumentRuntimeRequestDto& Request)
{
	FAssetDocumentRuntimeResultDto Result;
	Result.RequestId = Request.RequestId;
	Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed;
	Result.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
	Result.RuntimeRevision = Request.ExpectedRuntimeRevision != 0
		? Request.ExpectedRuntimeRevision
		: 1;
	Result.Diagnostics.Add(MakeCoreRuntimeCompositionDiagnostic(
		TEXT("CoreRuntimeComposition.NotImplemented"),
		TEXT("The Core runtime composition is not implemented.")));
	return Result;
}

FAssetDocumentRuntimeResultDto MakeCoreRuntimeCompositionFailure(
	const FAssetDocumentRuntimeRequestDto& Request,
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	FAssetDocumentRuntimeResultDto Result;
	Result.RequestId = Request.RequestId;
	Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Failed;
	Result.FinalPhase = EAssetDocumentRuntimeRequestPhase::Failed;
	Result.RuntimeRevision = Request.ExpectedRuntimeRevision != 0
		? Request.ExpectedRuntimeRevision
		: 1;
	Result.Diagnostics = MoveTemp(Diagnostics);
	return Result;
}

FAssetDocumentContractValidationResult MakeCoreValidationFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.Diagnostics.Add(
		MakeCoreRuntimeCompositionDiagnostic(Code, Message));
	return Result;
}

FAssetDocumentContractValidationResult MakeCoreOperationValidationFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result =
		MakeCoreValidationFailure(Code, Message);
	Result.Diagnostics[0].Path =
		TEXT("/CoreRuntimeComposition/Operation");
	return Result;
}

FAssetDocumentContractValidationResult
MakeCorePersistenceTargetValidationFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.Diagnostics.Add(
		MakeCorePersistenceTargetDiagnostic(Code, Message));
	return Result;
}

FAssetDocumentContractValidationResult MakeCoreValidationSuccess()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

bool CoreFootprintItemsMatch(
	const FAssetDocumentMutationFootprint& Left,
	const FAssetDocumentMutationFootprint& Right)
{
	return Left.Kind == Right.Kind
		&& Left.StableId == Right.StableId
		&& Left.RollbackComponentId == Right.RollbackComponentId;
}

bool CoreFootprintsMatch(
	TConstArrayView<FAssetDocumentMutationFootprint> Left,
	TConstArrayView<FAssetDocumentMutationFootprint> Right)
{
	if (Left.Num() != Right.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Left.Num(); ++Index)
	{
		if (!CoreFootprintItemsMatch(Left[Index], Right[Index]))
		{
			return false;
		}
	}
	return true;
}

bool IsMutationOperation(EAssetDocumentRuntimeOperation Operation)
{
	return Operation == EAssetDocumentRuntimeOperation::Apply
		|| Operation == EAssetDocumentRuntimeOperation::RegenerateSidecar
		|| Operation == EAssetDocumentRuntimeOperation::Sync;
}

bool RequiresMutationInputs(EAssetDocumentRuntimeOperation Operation)
{
	return Operation == EAssetDocumentRuntimeOperation::Preflight
		|| IsMutationOperation(Operation);
}

bool HasSameCoreDiagnosticIdentity(
	const FAssetDocumentRuntimeDiagnostic& Left,
	const FAssetDocumentRuntimeDiagnostic& Right)
{
	if (Left.Path != Right.Path)
	{
		return false;
	}
	if (Left.Code != Right.Code)
	{
		return false;
	}
	if (Left.Message != Right.Message)
	{
		return false;
	}
	return Left.Severity == Right.Severity;
}

bool IsContextFreeDuplicate(
	const FAssetDocumentRuntimeDiagnostic& Left,
	const FAssetDocumentRuntimeDiagnostic& Right)
{
	if (!Left.Context.IsEmpty())
	{
		return false;
	}
	if (!Right.Context.IsEmpty())
	{
		return false;
	}
	return HasSameCoreDiagnosticIdentity(Left, Right);
}

bool ContainsContextFreeDuplicate(
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Existing,
	const FAssetDocumentRuntimeDiagnostic& Candidate)
{
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Existing)
	{
		if (IsContextFreeDuplicate(Diagnostic, Candidate))
		{
			return true;
		}
	}
	return false;
}

void AppendDistinctCoreDiagnostics(
	TArray<FAssetDocumentRuntimeDiagnostic>& Target,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Added)
{
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Added)
	{
		if (!ContainsContextFreeDuplicate(Target, Diagnostic))
		{
			Target.Add(Diagnostic);
		}
	}
}

class FCoreInvalidationOnlyHandleResolver final
	: public IAssetDocumentHandleResolver
{
public:
	explicit FCoreInvalidationOnlyHandleResolver(
		FAssetDocumentCoreHandleInvalidator InInvalidator)
		: Invalidator(MoveTemp(InInvalidator))
	{
	}

	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
	{
		FAssetDocumentIssuedHandleResult Result;
		Result.Diagnostics.Add(MakeCoreRuntimeCompositionDiagnostic(
			TEXT("CoreRuntimeComposition.HandleIssueUnavailable"),
			TEXT("Core runtime composition cannot issue handles.")));
		return Result;
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleValidationContext& Context) override
	{
		FAssetDocumentVerifiedLeaseResult Result;
		Result.Diagnostics.Add(MakeCoreRuntimeCompositionDiagnostic(
			TEXT("CoreRuntimeComposition.HandleLeaseUnavailable"),
			TEXT("Core runtime composition cannot acquire handle leases.")));
		return Result;
	}

	virtual FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation& Event) override
	{
		return Invalidator(Event);
	}

private:
	FAssetDocumentCoreHandleInvalidator Invalidator;
};

bool IsInstalledSyncIdentity(
	const FAssetDocumentTypedSyncStateRecord& Record,
	const FGuid& RequestId,
	const FGuid& TransactionId,
	FStringView JournalId)
{
	if (Record.RequestId != RequestId)
	{
		return false;
	}
	if (Record.TransactionId != TransactionId)
	{
		return false;
	}
	return Record.JournalId == JournalId;
}

FAssetDocumentContractValidationResult ValidateLoadedSyncRecord(
	const FAssetDocumentContractValidationResult& Loaded,
	const TOptional<FAssetDocumentTypedSyncStateRecord>& Record,
	bool bExisted,
	const FGuid& RequestId,
	const FGuid& TransactionId,
	FStringView JournalId)
{
	if (!Loaded.bValid || !bExisted || !Record.IsSet())
	{
		return MakeCoreValidationFailure(
			TEXT("Atomic.Poisoned.RecoveryFailed"),
			TEXT("Installed typed sync state identity is invalid."));
	}
	return IsInstalledSyncIdentity(
			Record.GetValue(),
			RequestId,
			TransactionId,
			JournalId)
		? MakeCoreValidationSuccess()
		: MakeCoreValidationFailure(
			TEXT("Atomic.Poisoned.RecoveryFailed"),
			TEXT("Installed typed sync state identity is invalid."));
}

FAssetDocumentContractValidationResult ValidateInstalledSyncRecord(
	IAssetDocumentTypedSyncStateStore& Store,
	const FAssetDocumentSyncStateKey& Key,
	const FGuid& RequestId,
	const FGuid& TransactionId,
	FStringView JournalId)
{
	TOptional<FAssetDocumentTypedSyncStateRecord> Record;
	TArray<uint8> Bytes;
	bool bExisted = false;
	const FAssetDocumentContractValidationResult Loaded =
		Store.Load(Key, Record, Bytes, bExisted);
	return ValidateLoadedSyncRecord(
		Loaded,
		Record,
		bExisted,
		RequestId,
		TransactionId,
		JournalId);
}

class FCoreDeferredPersistenceCoordinator final
	: public IAssetDocumentPersistenceCoordinator
{
public:
	FCoreDeferredPersistenceCoordinator(
		FAssetDocumentSidecarByteIo& InSidecarByteIo,
		FAssetDocumentPersistenceJournal& InJournal,
		IAssetDocumentTypedSyncStateStore& InTypedStore,
		IAssetDocumentSyncCoordinator& InSync,
		IAssetDocumentHandleResolver& InHandles,
		FAssetDocumentSyncStateKey InSyncKey,
		const FAssetDocumentCoreMutationInputs& Mutation,
		TFunction<void(const FAssetDocumentRuntimeDiagnostic&)> InDiagnostics)
		: SidecarByteIo(InSidecarByteIo)
		, Journal(InJournal)
		, TypedStore(InTypedStore)
		, Sync(InSync)
		, Handles(InHandles)
		, SyncKey(MoveTemp(InSyncKey))
		, InstalledEvidence(Mutation.InstalledEvidence)
		, JournalBackupRoot(Mutation.JournalBackupRoot)
		, FreshHandleRevision(Mutation.FreshHandleRevision)
		, Diagnostics(MoveTemp(InDiagnostics))
	{
	}

	bool AdoptValidatedTarget(UPackage& Package, UObject* Asset)
	{
		if (PackageBackend || PackageStager)
		{
			return false;
		}
		PackageBackend =
			MakeUnique<FAssetDocumentPublicPackageSaveBackend>(Package, Asset);
		PackageStager =
			MakeUnique<FAssetDocumentPackageStager>(*PackageBackend);
		return true;
	}

	virtual FAssetDocumentDurableCompletionReceipt Persist(
		const FAssetDocumentPersistencePlan& Plan,
		IAssetDocumentStagedArtifactVerifier& Verifier) override
	{
		if (bPersisted || !PackageStager)
		{
			return MakeInvalidDurableReceipt();
		}
		bPersisted = true;
		FAssetDocumentPersistenceDependencies Dependencies{
			*PackageStager,
			SidecarByteIo,
			Journal,
			TypedStore,
			Sync,
			Handles,
			SyncKey,
			InstalledEvidence,
			JournalBackupRoot,
			FreshHandleRevision,
			Plan.RequestId,
			Plan.TransactionId,
			Plan.JournalId,
			Plan.StagingRoot,
			Plan.TargetPackagePath,
			Plan.TargetSidecarPath,
			{},
			[this](
				EAssetDocumentPersistenceStep Step,
				const FAssetDocumentRuntimeDiagnostic& Diagnostic)
			{
				if (Diagnostics)
				{
					Diagnostics(Diagnostic);
				}
			}};
		FAssetDocumentPersistenceCoordinatorRuntime Runtime(
			MoveTemp(Dependencies));
		return Runtime.Persist(Plan, Verifier);
	}

private:
	TUniquePtr<FAssetDocumentPublicPackageSaveBackend> PackageBackend;
	TUniquePtr<FAssetDocumentPackageStager> PackageStager;
	FAssetDocumentSidecarByteIo& SidecarByteIo;
	FAssetDocumentPersistenceJournal& Journal;
	IAssetDocumentTypedSyncStateStore& TypedStore;
	IAssetDocumentSyncCoordinator& Sync;
	IAssetDocumentHandleResolver& Handles;
	FAssetDocumentSyncStateKey SyncKey;
	FAssetDocumentSyncEvidencePair InstalledEvidence;
	FString JournalBackupRoot;
	uint64 FreshHandleRevision = 0;
	TFunction<void(const FAssetDocumentRuntimeDiagnostic&)> Diagnostics;
	bool bPersisted = false;
};

bool IsPersistenceTargetEvidenceAligned(
	const FAssetDocumentCorePersistenceTargetBinding& Binding,
	const FAssetDocumentCorePersistenceTargetRequest& Request)
{
	return Binding.RequestId == Request.RequestId
		&& Binding.TransactionId == Request.TransactionId
		&& Binding.RuntimeRevision == Request.RuntimeRevision;
}

bool IsPersistenceTargetObjectAligned(
	const FAssetDocumentCorePersistenceTargetBinding& Binding,
	const FAssetDocumentCorePersistenceTargetRequest& Request)
{
	if (Binding.Asset == nullptr || Binding.Package == nullptr)
	{
		return false;
	}
	return Binding.Asset->GetPathName() == Request.CanonicalObjectPath
		&& Binding.Asset->GetClass()->GetPathName()
			== Request.ExactAssetClassPath
		&& Binding.Asset->GetOutermost() == Binding.Package
		&& Binding.Package->GetName() == Request.TargetPackagePath;
}

void AddPersistenceTargetDiagnosticIfMissing(
	TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics,
	const TCHAR* Code,
	const TCHAR* Message)
{
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Diagnostics)
	{
		if (Diagnostic.Code == Code)
		{
			return;
		}
	}
	Diagnostics.Add(MakeCorePersistenceTargetDiagnostic(Code, Message));
}

class FCoreMutationOwnerGraph final
{
public:
	FCoreMutationOwnerGraph(
		FAssetDocumentCoreRuntimeDependencies& InDependencies,
		const FAssetDocumentRuntimeRequestDto& InRequest,
		FAssetDocumentCoreMutationInputs& Mutation,
		IAssetDocumentHandleResolver& InHandles,
		IAssetDocumentAtomicTransactionFactory& InTransactionFactory,
		TArray<FAssetDocumentRuntimeDiagnostic>& InDiagnostics)
		: Dependencies(InDependencies)
		, Request(InRequest)
		, Handles(InHandles)
		, TransactionFactory(InTransactionFactory)
		, TargetPackagePath(
			Mutation.PersistencePlanTemplate.TargetPackagePath)
		, PersistenceTargetBinder(
			MoveTemp(Mutation.BindPersistenceTarget))
		, TypedStore(Mutation.SyncStateRoot)
		, SyncKey{Mutation.PersistencePlanTemplate.TargetPackagePath,
			Mutation.SyncRegionId}
		, Sync(SyncKey, TypedStore)
		, Journal(
			Mutation.JournalRoot,
			[this](TArrayView64<const uint8> Bytes, bool bExisted)
			{
				return TypedStore.Restore(SyncKey, Bytes, bExisted);
			},
			[this](
				const FGuid& RequestId,
				const FGuid& TransactionId,
				FStringView JournalId)
			{
				return ValidateInstalledSyncRecord(
					TypedStore,
					SyncKey,
					RequestId,
					TransactionId,
					JournalId);
			})
		, Persistence(
			SidecarByteIo,
			Journal,
			TypedStore,
			Sync,
			Handles,
			SyncKey,
			Mutation,
			[&InDiagnostics](
				const FAssetDocumentRuntimeDiagnostic& Diagnostic)
			{
				InDiagnostics.Add(Diagnostic);
			})
		, Coordinator(FAssetDocumentRequestCoordinatorRuntimeBindings{
				Dependencies.CreatePhaseContext,
				MoveTemp(Mutation.OpenMutationCapture),
				[this](
					const FAssetDocumentRuntimeRequestDto& BoundRequest,
					const FGuid& TransactionId,
					TConstArrayView<FAssetDocumentChangedRecord> ChangedSet,
					TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
				{
					return BindPersistenceTarget(
						BoundRequest,
						TransactionId,
						ChangedSet,
						OutDiagnostics);
				},
				&Mutation.PreflightReceipt,
				&Mutation.PersistencePlanTemplate,
				&Mutation.Verifier})
	{
	}

	bool BindPersistenceTarget(
		const FAssetDocumentRuntimeRequestDto& BoundRequest,
		const FGuid& TransactionId,
		TConstArrayView<FAssetDocumentChangedRecord> ChangedSet,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
	{
		const FAssetDocumentCorePersistenceTargetRequest BindingRequest{
			BoundRequest.RequestId,
			TransactionId,
			BoundRequest.ExpectedRuntimeRevision,
			BoundRequest.TargetAssetPath,
			BoundRequest.RequestedAssetClassPath,
			TargetPackagePath,
			ChangedSet};
		TOptional<FAssetDocumentCorePersistenceTargetBinding> Binding =
			PersistenceTargetBinder(BindingRequest, OutDiagnostics);
		if (!Binding.IsSet())
		{
			AddPersistenceTargetDiagnosticIfMissing(
				OutDiagnostics,
				TEXT("CoreRuntimeComposition.PersistenceTargetBindingFailed"),
				TEXT("Persistence target binding failed."));
			return false;
		}
		if (!IsPersistenceTargetEvidenceAligned(
				Binding.GetValue(), BindingRequest)
			|| !IsPersistenceTargetObjectAligned(
				Binding.GetValue(), BindingRequest))
		{
			AddPersistenceTargetDiagnosticIfMissing(
				OutDiagnostics,
				TEXT("CoreRuntimeComposition.PersistenceTargetIdentityMismatch"),
				TEXT("Persistence target identity does not match the request."));
			return false;
		}
		if (!Persistence.AdoptValidatedTarget(*Binding->Package, Binding->Asset))
		{
			return false;
		}
		PersistedPackage.Reset(Binding->Package);
		return true;
	}

	void ReleaseCompletionAnchorForRollback()
	{
		// Creation rollback must be able to delete its package. This anchor
		// owns successful-save completion only, never rollback lifetime.
		PersistedPackage.Reset();
	}

	FAssetDocumentRuntimeResultDto Execute()
	{
		FAssetDocumentRequestCoordinatorDependencies CoordinatorDependencies{
			Dependencies.Host,
			Dependencies.PrimitiveRegistry,
			Handles,
			TransactionFactory,
			Persistence,
			Sync,
			Dependencies.Cancellation,
			Dependencies.Observer};
		FAssetDocumentRuntimeResultDto Result =
			Coordinator.Execute(Request, CoordinatorDependencies);
		// Staging deliberately keeps dirty state. Only the completed durable
		// transaction has saved this exact, already-validated live package.
		// Like UE's own save (SavePackage2), the installed package is no longer
		// newly created: it is on disk, so a later rollback may reload it.
		if (Result.TerminalStatus == EAssetDocumentRuntimeTerminalStatus::Succeeded
			&& PersistedPackage.IsValid())
		{
			PersistedPackage->SetDirtyFlag(false);
			PersistedPackage->ClearPackageFlags(PKG_NewlyCreated);
		}
		return Result;
	}

private:
	FAssetDocumentCoreRuntimeDependencies& Dependencies;
	const FAssetDocumentRuntimeRequestDto& Request;
	IAssetDocumentHandleResolver& Handles;
	IAssetDocumentAtomicTransactionFactory& TransactionFactory;
	FString TargetPackagePath;
	TStrongObjectPtr<UPackage> PersistedPackage;
	FAssetDocumentCorePersistenceTargetBinder PersistenceTargetBinder;
	FAssetDocumentSidecarByteIo SidecarByteIo;
	FAssetDocumentJournaledTypedSyncStateStore TypedStore;
	FAssetDocumentSyncStateKey SyncKey;
	FAssetDocumentSyncCoordinatorRuntime Sync;
	FAssetDocumentPersistenceJournal Journal;
	FCoreDeferredPersistenceCoordinator Persistence;
	FAssetDocumentRequestCoordinatorRuntime Coordinator;
};

FAssetDocumentContractValidationResult ValidateMutationEvidence(
	const FAssetDocumentSyncEvidencePair& Evidence)
{
	FAssetDocumentContractValidationResult Result =
		ValidateAssetDocumentRegionEvidence(Evidence.Asset);
	if (!Result.bValid)
	{
		return Result;
	}
	Result = ValidateAssetDocumentRegionEvidence(Evidence.Sidecar);
	if (!Result.bValid)
	{
		return Result;
	}
	if (Evidence.Asset.Kind == EAssetDocumentEvidenceKind::EvidenceUnavailable
		|| Evidence.Sidecar.Kind
			== EAssetDocumentEvidenceKind::EvidenceUnavailable)
	{
		return MakeCoreValidationFailure(
			TEXT("CoreRuntimeComposition.InvalidPersistence"),
			TEXT("Installed sync evidence must be comparable."));
	}
	return MakeCoreValidationSuccess();
}

bool AreMutationRequestIdsAligned(
	const FAssetDocumentRuntimeRequestDto& Request,
	const FAssetDocumentCoreMutationInputs& Mutation)
{
	const FAssetDocumentPersistencePlan& Plan =
		Mutation.PersistencePlanTemplate;
	return Mutation.PreflightReceipt.RequestId == Request.RequestId
		&& Plan.RequestId == Request.RequestId;
}

FAssetDocumentContractValidationResult ValidateMutationRequestIdentity(
	const FAssetDocumentRuntimeRequestDto& Request,
	const FAssetDocumentCoreMutationInputs& Mutation)
{
	if (!AreMutationRequestIdsAligned(Request, Mutation))
	{
		return MakeCoreValidationFailure(
			TEXT("CoreRuntimeComposition.IdentityMismatch"),
			TEXT("Mutation request identities do not match."));
	}
	if (Mutation.PreflightReceipt.RuntimeRevision
		!= Request.ExpectedRuntimeRevision)
	{
		return MakeCoreValidationFailure(
			TEXT("CoreRuntimeComposition.IdentityMismatch"),
			TEXT("Mutation request identities do not match."));
	}
	if (Mutation.PersistencePlanTemplate.TransactionId.IsValid())
	{
		return MakeCoreValidationFailure(
			TEXT("CoreRuntimeComposition.IdentityMismatch"),
			TEXT("Mutation request identities do not match."));
	}
	return MakeCoreValidationSuccess();
}

FAssetDocumentContractValidationResult ValidateMutationIdentities(
	const FAssetDocumentRuntimeRequestDto& Request,
	const FAssetDocumentCoreMutationInputs& Mutation)
{
	return ValidateMutationRequestIdentity(Request, Mutation);
}

bool AreMutationStorageRootsPresent(
	const FAssetDocumentCoreMutationInputs& Mutation)
{
	return !Mutation.JournalRoot.TrimStartAndEnd().IsEmpty()
		&& !Mutation.JournalBackupRoot.TrimStartAndEnd().IsEmpty()
		&& !Mutation.SyncStateRoot.TrimStartAndEnd().IsEmpty()
		&& !Mutation.SyncRegionId.TrimStartAndEnd().IsEmpty();
}

FAssetDocumentContractValidationResult ValidateMutationStorage(
	const FAssetDocumentCoreMutationInputs& Mutation)
{
	if (!AreMutationStorageRootsPresent(Mutation))
	{
		return MakeCoreValidationFailure(
			TEXT("CoreRuntimeComposition.InvalidStorageRoot"),
			TEXT("Mutation persistence roots and sync region are required."));
	}
	if (Mutation.FreshHandleRevision == 0)
	{
		return MakeCoreValidationFailure(
			TEXT("CoreRuntimeComposition.InvalidPersistence"),
			TEXT("Fresh handle revision must be non-zero."));
	}
	if (!Mutation.BindPersistenceTarget)
	{
		return MakeCorePersistenceTargetValidationFailure(
			TEXT("CoreRuntimeComposition.PersistenceTargetBinderMissing"),
			TEXT("Persistence target binder is required."));
	}
	if (!Mutation.OpenMutationCapture)
	{
		return MakeCoreValidationFailure(
			TEXT("CoreRuntimeComposition.MissingMutationCaptureOpener"),
			TEXT("Mutation capture opener is required."));
	}
	return MakeCoreValidationSuccess();
}

FAssetDocumentContractValidationResult ValidateMutationRequestContracts(
	const FAssetDocumentRuntimeRequestDto& Request,
	const FAssetDocumentCoreMutationInputs& Mutation)
{
	FAssetDocumentContractValidationResult Result =
		ValidateAssetDocumentRuntimeRequest(Request);
	if (!Result.bValid)
	{
		return Result;
	}
	if (Request.Operation == EAssetDocumentRuntimeOperation::Preflight)
	{
		return MakeCoreOperationValidationFailure(
			TEXT("CoreRuntimeComposition.PreflightRouteNotActive"),
			TEXT("Preflight routing is not active in Core composition."));
	}
	if (!IsMutationOperation(Request.Operation))
	{
		return MakeCoreValidationFailure(
			TEXT("CoreRuntimeComposition.MutationInputsRequired"),
			TEXT("Mutation execution requires a mutation operation."));
	}
	return ValidateAssetDocumentPreflightReceipt(
		Mutation.PreflightReceipt);
}

FAssetDocumentContractValidationResult
ValidatePersistenceTemplateLocations(
	const FAssetDocumentPersistencePlan& Plan)
{
	using namespace UE::AssetDocument::PersistenceCoordinator::Private;
	FAssetDocumentContractValidationResult Result =
		ValidatePlanTargets(Plan);
	if (!Result.bValid)
	{
		return Result;
	}
	Result = ValidatePlanStagingRoot(Plan);
	if (!Result.bValid)
	{
		return Result;
	}
	Result = ValidateStagingIsolation(Plan);
	return Result.bValid ? ValidatePlanJournal(Plan) : Result;
}

FAssetDocumentContractValidationResult
ValidatePersistenceTemplatePayload(
	const FAssetDocumentPersistencePlan& Plan)
{
	using namespace UE::AssetDocument::PersistenceCoordinator::Private;
	const FAssetDocumentContractValidationResult Sidecar =
		ValidatePlanSidecar(Plan.Sidecar);
	return Sidecar.bValid
		? ValidatePersistenceFootprint(Plan.Footprint)
		: Sidecar;
}

FAssetDocumentContractValidationResult ValidateMutationFootprintAndPlan(
	const FAssetDocumentCoreMutationInputs& Mutation)
{
	if (!CoreFootprintsMatch(
		Mutation.PreflightReceipt.CompleteFootprint,
		Mutation.PersistencePlanTemplate.Footprint))
	{
		return MakeCoreValidationFailure(
			TEXT("CoreRuntimeComposition.FootprintMismatch"),
			TEXT("Preflight and persistence footprints do not match."));
	}
	const FAssetDocumentContractValidationResult Locations =
		ValidatePersistenceTemplateLocations(
			Mutation.PersistencePlanTemplate);
	return Locations.bValid
		? ValidatePersistenceTemplatePayload(
			Mutation.PersistencePlanTemplate)
		: Locations;
}

FAssetDocumentContractValidationResult ValidateMutationInputs(
	const FAssetDocumentRuntimeRequestDto& Request,
	const FAssetDocumentCoreMutationInputs& Mutation)
{
	FAssetDocumentContractValidationResult Result =
		ValidateMutationRequestContracts(Request, Mutation);
	if (!Result.bValid)
	{
		return Result;
	}
	Result = ValidateMutationIdentities(Request, Mutation);
	if (!Result.bValid)
	{
		return Result;
	}
	Result = ValidateMutationFootprintAndPlan(Mutation);
	if (!Result.bValid)
	{
		return Result;
	}
	Result = ValidateMutationEvidence(Mutation.InstalledEvidence);
	return Result.bValid ? ValidateMutationStorage(Mutation) : Result;
}

FAssetDocumentContractValidationResult AppendPackageSnapshotFootprint(
	FAssetDocumentCoreMutationInputs& Mutation)
{
	TSet<FString> Seen;
	for (const FString& PackageName : Mutation.SnapshotPackageNames)
	{
		if (!FPackageName::IsValidLongPackageName(PackageName) || Seen.Contains(PackageName))
		{
			return MakeCoreValidationFailure(
				TEXT("CoreRuntimeComposition.InvalidSnapshotPackage"),
				TEXT("Snapshot package names must be unique long package names."));
		}
		Seen.Add(PackageName);
		const FAssetDocumentMutationFootprint Row{
			EAssetDocumentMutationFootprintKind::Package,
			FAssetDocumentPackageSnapshotRollbackAction::MakeStableId(PackageName),
			FAssetDocumentPackageSnapshotRollbackAction::ComponentId};
		Mutation.PreflightReceipt.CompleteFootprint.Add(Row);
		Mutation.PersistencePlanTemplate.Footprint.Add(Row);
	}
	return MakeCoreValidationSuccess();
}

// Captures the requested packages when the live transaction begins and
// enlists their snapshot before any provider can enlist an inverse; the
// atomic runtime rolls targets back in reverse, so snapshots restore last.
class FCorePackageSnapshotTransactionFactory final
	: public IAssetDocumentAtomicTransactionFactory
{
public:
	FCorePackageSnapshotTransactionFactory(
		IAssetDocumentAtomicTransactionFactory& InInner,
		TArray<FString> InPackageNames,
		TArray<FAssetDocumentRuntimeDiagnostic>& InDiagnostics)
		: Inner(InInner)
		, PackageNames(MoveTemp(InPackageNames))
		, Diagnostics(InDiagnostics)
	{
	}

	virtual TUniquePtr<IAssetDocumentAtomicTransaction> BeginLiveAfterPreflight(
		const FAssetDocumentPreflightReceipt& Receipt,
		IAssetDocumentCancellationView& Cancellation) override
	{
		TUniquePtr<IAssetDocumentAtomicTransaction> Transaction =
			Inner.BeginLiveAfterPreflight(Receipt, Cancellation);
		if (!Transaction || PackageNames.IsEmpty())
		{
			return Transaction;
		}
		TArray<TUniquePtr<FAssetDocumentPackageSnapshot>> Snapshots;
		TArray<FAssetDocumentMutationFootprint> Rows;
		for (const FString& PackageName : PackageNames)
		{
			UPackage* Package = FindPackage(nullptr, *PackageName);
			if (!Package)
			{
				Diagnostics.Add(MakeCoreRuntimeCompositionDiagnostic(
					TEXT("CoreRuntimeComposition.PackageSnapshotUnavailable"),
					TEXT("A package requested for snapshot rollback is not loaded.")));
				return nullptr;
			}
			TUniquePtr<FAssetDocumentPackageSnapshot> Snapshot =
				FAssetDocumentPackageSnapshot::Capture(*Package, Diagnostics);
			if (!Snapshot)
			{
				return nullptr;
			}
			Snapshots.Add(MoveTemp(Snapshot));
			Rows.Add({EAssetDocumentMutationFootprintKind::Package,
				FAssetDocumentPackageSnapshotRollbackAction::MakeStableId(PackageName),
				FAssetDocumentPackageSnapshotRollbackAction::ComponentId});
		}
		FAssetDocumentContractValidationResult Enlisted =
			Transaction->EnlistRollbackAction(
				MakeShared<FAssetDocumentPackageSnapshotRollbackAction>(MoveTemp(Snapshots)));
		if (Enlisted.bValid)
		{
			Enlisted = Transaction->EnlistFootprint(Rows);
		}
		if (!Enlisted.bValid)
		{
			Diagnostics.Append(Enlisted.Diagnostics);
			return nullptr;
		}
		return Transaction;
	}

private:
	IAssetDocumentAtomicTransactionFactory& Inner;
	TArray<FString> PackageNames;
	TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics;
};

class FCoreMutationExecution final
{
public:
	FCoreMutationExecution(
		FAssetDocumentCoreRuntimeDependencies& InDependencies,
		const FAssetDocumentRuntimeRequestDto& InRequest,
		FAssetDocumentCoreMutationInputs& InMutation)
		: Dependencies(InDependencies)
		, Request(InRequest)
		, Mutation(InMutation)
	{
	}

	FAssetDocumentRuntimeResultDto Run()
	{
		const FAssetDocumentContractValidationResult SnapshotRows =
			AppendPackageSnapshotFootprint(Mutation);
		if (!SnapshotRows.bValid)
		{
			return MakeCoreRuntimeCompositionFailure(
				Request,
				SnapshotRows.Diagnostics);
		}
		const FAssetDocumentContractValidationResult Validation =
			ValidateMutationInputs(Request, Mutation);
		if (!Validation.bValid)
		{
			return MakeCoreRuntimeCompositionFailure(
				Request,
				Validation.Diagnostics);
		}
		FCoreMutationOwnerGraph* ActiveGraph = nullptr;
		FCoreInvalidationOnlyHandleResolver Handles(
			[this, &ActiveGraph](const FAssetDocumentHandleInvalidation& Event)
			{
				// Atomic invalidates before invoking any rollback action.
				if (Event.Reason == EAssetDocumentHandleInvalidationReason::Rollback
					&& ActiveGraph)
				{
					ActiveGraph->ReleaseCompletionAnchorForRollback();
				}
				return Dependencies.InvalidateHandles(Event);
			});
		FAssetDocumentRollbackActionComposition TransactionFactory(
			Handles,
			MoveTemp(Mutation.RollbackActions),
			[this](
				EAssetDocumentRuntimeRequestPhase Phase,
				const FAssetDocumentRuntimeDiagnostic& Diagnostic)
			{
				Diagnostics.Add(Diagnostic);
			});
		if (!TransactionFactory.IsValid())
		{
			return MakeCoreRuntimeCompositionFailure(
				Request,
				TransactionFactory.GetValidation().Diagnostics);
		}
		FCorePackageSnapshotTransactionFactory SnapshotTransactionFactory(
			TransactionFactory,
			MoveTemp(Mutation.SnapshotPackageNames),
			Diagnostics);
		FCoreMutationOwnerGraph Graph(
			Dependencies,
			Request,
			Mutation,
			Handles,
			SnapshotTransactionFactory,
			Diagnostics);
		TGuardValue<FCoreMutationOwnerGraph*> ActiveGraphGuard(ActiveGraph, &Graph);
		FAssetDocumentRuntimeResultDto Result = Graph.Execute();
		AppendDistinctCoreDiagnostics(Result.Diagnostics, Diagnostics);
		return Result;
	}

private:
	FAssetDocumentCoreRuntimeDependencies& Dependencies;
	const FAssetDocumentRuntimeRequestDto& Request;
	FAssetDocumentCoreMutationInputs& Mutation;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};
} // namespace

struct FAssetDocumentCoreMutationCaptureLease::FImpl
{
	explicit FImpl(TUniqueFunction<void()>&& InClose)
		: CloseCallback(MoveTemp(InClose))
	{
	}

	~FImpl()
	{
		Close();
	}

	bool IsValid() const
	{
		return static_cast<bool>(CloseCallback);
	}

	void Close()
	{
		if (!bClosed && CloseCallback)
		{
			bClosed = true;
			CloseCallback();
		}
	}

	TUniqueFunction<void()> CloseCallback;
	bool bClosed = false;
};

FAssetDocumentCoreMutationCaptureLease::
	FAssetDocumentCoreMutationCaptureLease(
		TUniqueFunction<void()>&& InClose)
	: Impl(MakeUnique<FImpl>(MoveTemp(InClose)))
{
}

FAssetDocumentCoreMutationCaptureLease::
	~FAssetDocumentCoreMutationCaptureLease() = default;

FAssetDocumentCoreMutationCaptureLease::
	FAssetDocumentCoreMutationCaptureLease(
		FAssetDocumentCoreMutationCaptureLease&& Other) = default;

FAssetDocumentCoreMutationCaptureLease&
FAssetDocumentCoreMutationCaptureLease::operator=(
	FAssetDocumentCoreMutationCaptureLease&& Other) = default;

bool FAssetDocumentCoreMutationCaptureLease::IsValid() const
{
	return Impl && Impl->IsValid();
}

void FAssetDocumentCoreMutationCaptureLease::Close()
{
	if (Impl)
	{
		Impl->Close();
	}
}

struct FAssetDocumentCoreRuntimeComposition::FImpl
{
	explicit FImpl(FAssetDocumentCoreRuntimeDependencies InDependencies)
		: Dependencies(MoveTemp(InDependencies))
	{
		Validation.bValid = true;
		if (!Dependencies.CreatePhaseContext)
		{
			Validation.bValid = false;
			Validation.Diagnostics.Add(MakeCoreRuntimeCompositionDiagnostic(
				TEXT("CoreRuntimeComposition.MissingContextFactory"),
				TEXT("Core runtime composition requires a phase context factory.")));
		}
		if (!Dependencies.InvalidateHandles)
		{
			Validation.bValid = false;
			Validation.Diagnostics.Add(MakeCoreRuntimeCompositionDiagnostic(
				TEXT("CoreRuntimeComposition.MissingHandleInvalidator"),
				TEXT("Core runtime composition requires a handle invalidator.")));
		}
	}

	FAssetDocumentCoreRuntimeDependencies Dependencies;
	FAssetDocumentContractValidationResult Validation;
};

FAssetDocumentCoreRuntimeComposition::
	FAssetDocumentCoreRuntimeComposition(
		FAssetDocumentCoreRuntimeDependencies InDependencies)
	: Impl(MakeUnique<FImpl>(MoveTemp(InDependencies)))
{
}

FAssetDocumentCoreRuntimeComposition::
	~FAssetDocumentCoreRuntimeComposition() = default;

FAssetDocumentCoreRuntimeComposition::
	FAssetDocumentCoreRuntimeComposition(
		FAssetDocumentCoreRuntimeComposition&& Other) = default;

FAssetDocumentCoreRuntimeComposition&
FAssetDocumentCoreRuntimeComposition::operator=(
	FAssetDocumentCoreRuntimeComposition&& Other) = default;

bool FAssetDocumentCoreRuntimeComposition::IsValid() const
{
	return Impl && Impl->Validation.bValid;
}

const FAssetDocumentContractValidationResult&
FAssetDocumentCoreRuntimeComposition::GetValidation() const
{
	check(Impl);
	return Impl->Validation;
}

FAssetDocumentRuntimeResultDto
FAssetDocumentCoreRuntimeComposition::ExecuteReadOnly(
	const FAssetDocumentRuntimeRequestDto& Request)
{
	if (RequiresMutationInputs(Request.Operation))
	{
		FAssetDocumentContractValidationResult Validation =
			MakeCoreOperationValidationFailure(
				TEXT("CoreRuntimeComposition.MutationInputsRequired"),
				TEXT("Mutation inputs are required for this operation."));
		return MakeCoreRuntimeCompositionFailure(
			Request,
			MoveTemp(Validation.Diagnostics));
	}
	FAssetDocumentRequestCoordinatorRuntimeBindings Bindings;
	Bindings.CreatePhaseContext = Impl->Dependencies.CreatePhaseContext;
	FAssetDocumentRequestCoordinatorRuntime Coordinator(MoveTemp(Bindings));
	FAssetDocumentRequestCoordinatorReadOnlyDependencies Dependencies{
		Impl->Dependencies.Host,
		Impl->Dependencies.Cancellation,
		Impl->Dependencies.Observer};
	return Coordinator.ExecuteReadOnly(Request, Dependencies);
}

FAssetDocumentRuntimeResultDto
FAssetDocumentCoreRuntimeComposition::ExecuteMutation(
	const FAssetDocumentRuntimeRequestDto& Request,
	FAssetDocumentCoreMutationInputs&& Mutation)
{
	FCoreMutationExecution Execution(
		Impl->Dependencies,
		Request,
		Mutation);
	return Execution.Run();
}

FAssetDocumentRuntimeResultDto ExecuteAssetDocumentCoreEqualDelta(
	FAssetDocumentCoreRuntimeDependencies& Dependencies,
	const FAssetDocumentRuntimeRequestDto& Request,
	FAssetDocumentCoreEqualDeltaInputs&& Inputs)
{
	// The same privately bound host/context/observer dependencies are used without
	// constructing the mutation owner graph, journal, typed store or persistence.
	FAssetDocumentRequestCoordinatorRuntimeBindings Bindings;
	Bindings.CreatePhaseContext = Dependencies.CreatePhaseContext;
	FAssetDocumentRequestCoordinatorRuntime Coordinator(MoveTemp(Bindings));
	FAssetDocumentRequestCoordinatorReadOnlyDependencies ReadOnly{
		Dependencies.Host, Dependencies.Cancellation, Dependencies.Observer};
	return Coordinator.ExecuteEqualDelta(Request, ReadOnly, MoveTemp(Inputs));
}
