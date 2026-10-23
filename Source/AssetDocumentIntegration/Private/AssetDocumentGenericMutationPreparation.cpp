// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentGenericMutationPreparation.h"
#include "AssetDocumentDirtyTargetMessage.h"
#include "AssetDocumentMutationIntentProjection.h"

#include "AssetDocumentBehaviorTreeOperationClassifier.h"
#include "AssetDocumentBehaviorTreeProductionFootprintPlanner.h"
#include "AssetDocumentEditorCutoverTransactionPreparation.h"
#include "AssetDocumentEditorRollbackActionAdapter.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace GenericMutationPreparationLocal
{
TAutoConsoleVariable<bool> CVarCorePackageSnapshot(
	TEXT("AssetDocument.CorePackageSnapshot"),
	true,
	TEXT("Snapshot the package of an existing generic ApplyFile target so Core can roll back any mutation of it."));

// The loaded package of an existing target, or empty when Core package snapshot
// rollback is disabled or the target is created by this request.
FString ResolveSnapshotPackageName(const FString& CanonicalObjectPath, bool bCreate)
{
	if (bCreate || !CVarCorePackageSnapshot.GetValueOnGameThread())
	{
		return FString();
	}
	const FSoftObjectPath TargetPath(CanonicalObjectPath);
	UObject* Target = TargetPath.ResolveObject();
	if (!Target && IAssetRegistry::GetChecked().GetAssetByObjectPath(TargetPath).IsValid())
	{
		Target = TargetPath.TryLoad();
	}
	// Packages with derived state (e.g. Blueprint generated classes) need a
	// registered rebuilder before the snapshot can restore them equivalently.
	return Target && FAssetDocumentPackageSnapshot::CanRestorePackage(*Target->GetOutermost())
		? Target->GetOutermost()->GetName()
		: FString();
}

// Providers without a preparation binding (e.g. the AI provider) still mutate
// the target; the frozen object/package rows plus the package snapshot cover them.
void AddSnapshotCoveredContributions(
	const FAssetDocumentRuntimeRequestDto& Request,
	const IAssetDocumentPrimitiveRegistry& Registry,
	const FAssetDocumentRequiredPrimitiveProjection& Projection,
	TConstArrayView<FAssetDocumentMutationIntent> Steps,
	TSet<int32>& PreparedSteps,
	TArray<FAssetDocumentMutationComposition::FProviderBatch>& Batches)
{
	const FString ObjectId = TEXT("Object:") + Request.TargetAssetPath;
	const FString PackageId = TEXT("Package:") + FPackageName::ObjectPathToPackageName(Request.TargetAssetPath);
	for (const TSharedRef<IAssetDocumentPrimitiveProvider>& Provider : Projection.Providers)
	{
		const FString ProviderId = Provider->GetProviderId();
		if (Projection.PreparationViews.ContainsByPredicate([&](const auto& View) { return View.Provider->GetProviderId() == ProviderId; }))
		{
			continue;
		}
		FAssetDocumentMutationComposition::FProviderBatch Batch;
		for (const FAssetDocumentMutationIntent& Step : Steps)
		{
			const FAssetDocumentPrimitiveResolution Resolved = Registry.Resolve(Step.Requirement);
			if (PreparedSteps.Contains(Step.StepIndex) || !Resolved.bResolved || Resolved.ProviderId != ProviderId)
			{
				continue;
			}
			FAssetDocumentPreparedMutationContribution& Contribution = Batch.Batch.Contributions.AddDefaulted_GetRef();
			Contribution.StepIndex = Step.StepIndex;
			Contribution.RawFootprint = {
				{EAssetDocumentMutationFootprintKind::UObjectState, ObjectId, TEXT("rollback.uobject")},
				{EAssetDocumentMutationFootprintKind::Package, PackageId, TEXT("rollback.package")}};
			Contribution.OwnedSurfaceIds = {FString::Printf(TEXT("%s::step.%d"), *Request.TargetAssetPath, Step.StepIndex)};
			Contribution.ValidateInvocation = [](const FAssetDocumentPrimitiveInvocation&, IAssetDocumentHandleResolver&)
			{
				FAssetDocumentContractValidationResult Valid;
				Valid.bValid = true;
				return Valid;
			};
			Contribution.bCoveredByPackageSnapshot = true;
			PreparedSteps.Add(Step.StepIndex);
		}
		if (!Batch.Batch.Contributions.IsEmpty())
		{
			Batch.ProviderId = ProviderId;
			Batch.ActualProvider = Provider;
			Batches.Add(MoveTemp(Batch));
		}
	}
}

struct FMutationCaptureSet
{
	TArray<TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>> Captures;
};

FAssetDocumentCoreMutationCaptureOpener MakeCaptureOpener(
	TArray<TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>> Seeds)
{
	return [Seeds = MoveTemp(Seeds)](
		const FGuid& TransactionId,
		TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
		-> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
	{
		TSharedRef<FMutationCaptureSet> CaptureSet =
			MakeShared<FMutationCaptureSet>();
		for (const TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>& Seed :
			Seeds)
		{
			TUniquePtr<IAssetDocumentEditorCutoverMutationCapture> Capture =
				Seed->OpenMutationCapture(TransactionId);
			if (!Capture)
			{
				FAssetDocumentRuntimeDiagnostic& Diagnostic =
					OutDiagnostics.AddDefaulted_GetRef();
				Diagnostic.Path = TEXT("/GenericRuntime/MutationCapture");
				Diagnostic.Code =
					TEXT("GenericRuntime.MutationCaptureOpenFailed");
				Diagnostic.Message =
					TEXT("Editor mutation capture could not be opened.");
				return {};
			}
			CaptureSet->Captures.Add(MoveTemp(Capture));
		}
		return MakeUnique<FAssetDocumentCoreMutationCaptureLease>(
			[CaptureSet]() mutable
			{
				CaptureSet->Captures.Reset();
			});
	};
}

bool IsCreateAction(const FAssetDocumentRuntimeValueDto& Document)
{
	const int32 ActionIndex = Document.ObjectKeys.IndexOfByKey(TEXT("Action"));
	return Document.Kind == EAssetDocumentRuntimeValueKind::Object
		&& Document.Elements.IsValidIndex(ActionIndex)
		&& Document.Elements[ActionIndex].Kind
			== EAssetDocumentRuntimeValueKind::String
		&& Document.Elements[ActionIndex].StringValue == TEXT("Create");
}

bool IsCreateOrUpdateAction(const FAssetDocumentRuntimeValueDto& Document)
{
	const int32 ActionIndex = Document.ObjectKeys.IndexOfByKey(TEXT("Action"));
	return Document.Kind == EAssetDocumentRuntimeValueKind::Object
		&& Document.Elements.IsValidIndex(ActionIndex)
		&& Document.Elements[ActionIndex].Kind
			== EAssetDocumentRuntimeValueKind::String
		&& Document.Elements[ActionIndex].StringValue == TEXT("CreateOrUpdate");
}

// Whether this request creates its target: Create always, CreateOrUpdate when the
// target is absent. Existence is the asset registry entry, the same evidence the Python
// resolve-existing primitive and the registered target decision use.
bool IsCreateDecision(const FAssetDocumentRuntimeRequestDto& Request)
{
	if (IsCreateAction(Request.CanonicalDocument))
	{
		return true;
	}
	return IsCreateOrUpdateAction(Request.CanonicalDocument)
		&& !IAssetRegistry::GetChecked().GetAssetByObjectPath(
			FSoftObjectPath(Request.TargetAssetPath)).IsValid();
}

bool HasEmptyCanonicalProperties(
	const FAssetDocumentRuntimeValueDto& Document)
{
	const int32 PropertiesIndex =
		Document.ObjectKeys.IndexOfByKey(TEXT("Properties"));
	return Document.Kind == EAssetDocumentRuntimeValueKind::Object
		&& Document.Elements.IsValidIndex(PropertiesIndex)
		&& Document.Elements[PropertiesIndex].Kind
			== EAssetDocumentRuntimeValueKind::Object
		&& Document.Elements[PropertiesIndex].ObjectKeys.IsEmpty()
		&& Document.Elements[PropertiesIndex].Elements.IsEmpty();
}

bool HasOnlyInvalidTargetDiagnostic(
	const FAssetDocumentEditorCutoverTransactionPreparation& Preparation)
{
	return !Preparation.bPrepared && Preparation.Diagnostics.Num() == 1
		&& Preparation.Diagnostics[0].Code == TEXT("CutoverPreparation.InvalidTarget");
}

FAssetDocumentEditorCutoverMutationIntent MakeEditorIntent(
	const FAssetDocumentRuntimeRequestDto& Request)
{
	const bool bCreateByFactory = IsCreateDecision(Request);
	FAssetDocumentEditorCutoverMutationIntent Intent;
	Intent.RequestId = Request.RequestId;
	Intent.RuntimeRevision = Request.ExpectedRuntimeRevision;
	Intent.CanonicalObjectPath = Request.TargetAssetPath;
	Intent.ExactAssetClassPath = Request.RequestedAssetClassPath;
	// Blackboard keeps its exact data-asset factory; every other unregistered class
	// is created through its catalog lifecycle (the class-neutral generic factory).
	const FAssetDocumentEditorTargetLifecycleCatalogEntry* Lifecycle =
		FindAssetDocumentEditorTargetLifecycle(Request.RequestedAssetClassPath);
	Intent.FactoryId = !bCreateByFactory ? FString()
		: Request.RequestedAssetClassPath == TEXT("/Script/AIModule.BlackboardData") || Lifecycle == nullptr
			? FString(TEXT("data_asset_factory.v1"))
			: Lifecycle->FactoryId;
	Intent.bCreateByFactory = bCreateByFactory;
	Intent.bWriteAdmittedProperties =
		!bCreateByFactory
		|| !HasEmptyCanonicalProperties(Request.CanonicalDocument);
	return Intent;
}

FAssetDocumentEditorCutoverMutationIntent MakeBehaviorTreeFactoryIntent(
	const FAssetDocumentRuntimeRequestDto& Request,
	bool bWriteAdmittedProperties)
{
	FAssetDocumentEditorCutoverMutationIntent Intent;
	Intent.RequestId = Request.RequestId;
	Intent.RuntimeRevision = Request.ExpectedRuntimeRevision;
	Intent.CanonicalObjectPath = Request.TargetAssetPath;
	Intent.ExactAssetClassPath = Request.RequestedAssetClassPath;
	Intent.FactoryId = TEXT("behavior_tree_factory.v1");
	Intent.bCreateByFactory = true;
	Intent.bWriteAdmittedProperties = bWriteAdmittedProperties;
	return Intent;
}

FAssetDocumentPersistencePlan MakePersistencePlan(
	const FAssetDocumentApplyFileRequest& ServiceRequest,
	const FAssetDocumentRuntimeRequestDto& RuntimeRequest,
	const FAssetDocumentRuntimeResultDto& Validated,
	TConstArrayView<FAssetDocumentMutationFootprint> Footprint)
{
	const FString RequestKey =
		RuntimeRequest.RequestId.ToString(EGuidFormats::Digits);
	FAssetDocumentPersistencePlan Plan;
	Plan.RequestId = RuntimeRequest.RequestId;
	Plan.TargetPackagePath =
		FSoftObjectPath(RuntimeRequest.TargetAssetPath).GetLongPackageName();
	Plan.TargetSidecarPath = ServiceRequest.FilePath;
	Plan.StagingRoot = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AssetDocument/Runtime/Staging"),
		RequestKey);
	Plan.JournalId = RequestKey;
	Plan.Sidecar.Utf8Bytes = Validated.CanonicalBytes;
	Plan.Sidecar.SemanticHash = Validated.SemanticHash;
	Plan.Sidecar.CodecVersion = Validated.CodecVersion;
	Plan.Footprint.Append(Footprint);
	return Plan;
}

FAssetDocumentSyncEvidencePair MakeInstalledEvidence(
	const FAssetDocumentRuntimeResultDto& Validated)
{
	FAssetDocumentRegionEvidence Evidence;
	Evidence.Kind = EAssetDocumentEvidenceKind::Present;
	Evidence.SemanticHash = Validated.SemanticHash;
	Evidence.CodecVersion = Validated.CodecVersion;
	return {Evidence, Evidence};
}

bool IsExactBlackboardApply(const FAssetDocumentRuntimeRequestDto& Request)
{
	return Request.Operation == EAssetDocumentRuntimeOperation::Apply
		&& Request.RequestedAssetClassPath
			== TEXT("/Script/AIModule.BlackboardData");
}

bool ParseBlackboardProjection(
	const FAssetDocumentRuntimeRequestDto& RuntimeRequest,
	const FAssetDocumentRuntimeResultDto& PreflightResult,
	FAssetDocumentBlackboardFootprintProjection& OutProjection,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	const FAssetDocumentContractValidationResult Validation =
		ParseAssetDocumentBlackboardFootprintProjection(
			RuntimeRequest, PreflightResult, OutProjection);
	OutDiagnostics.Append(Validation.Diagnostics);
	return Validation.bValid;
}
}

FAssetDocumentGenericMutationPreparationResult
FAssetDocumentGenericMutationPreparation::Prepare(
	const FAssetDocumentApplyFileRequest& ServiceRequest,
	const FAssetDocumentRuntimeRequestDto& RuntimeRequest,
	const FAssetDocumentRuntimeResultDto& Validated,
	const FAssetDocumentRuntimeResultDto& PreflightResult,
	IAssetDocumentStagedArtifactVerifier& Verifier,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics,
	const FAssetDocumentRuntimeTargetActivation* TargetActivation,
 const IAssetDocumentPrimitiveRegistry* Registry,
 const FAssetDocumentRequiredPrimitiveProjection* ProviderProjection)
{
	using namespace GenericMutationPreparationLocal;
	FAssetDocumentGenericMutationPreparationResult Result;
	const bool bBlackboardApply = TargetActivation == nullptr && IsExactBlackboardApply(RuntimeRequest);
	// Legacy Generic factory preflight has no canonical payload; BB owns its separate
 // closed projection. Any new payload must validate, including a missing marker.
 const bool bMutationProjection = !bBlackboardApply && PreflightResult.CanonicalValue.IsSet();
 FAssetDocumentMutationIntentProjection MutationProjection;
 if (bMutationProjection)
 {
  if (!Registry || !ProviderProjection)
  {
   auto& D = OutDiagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/MutationComposition");
   D.Code = TEXT("MutationComposition.PreparationUnavailable"); D.Message = TEXT("The frozen provider preparation projection is required."); return Result;
  }
  TArray<FAssetDocumentPrimitiveRequirement> RequestRequirements;
  if (TargetActivation) RequestRequirements = TargetActivation->RequiredPrimitives;
  else
   for (const auto& Requirement : ProviderProjection->Requirements)
    if (Requirement.RequiredByRouteId == TEXT("route.generic.v1")) RequestRequirements.Add(Requirement);
  auto Parsed = ParseAssetDocumentMutationIntentProjection(RuntimeRequest, PreflightResult, Validated.SemanticHash,
   *Registry, RequestRequirements, MutationProjection);
  OutDiagnostics.Append(Parsed.Diagnostics); if (!Parsed.bValid) return Result;
 }
 FAssetDocumentBlackboardFootprintProjection BlackboardProjection;
	if (bBlackboardApply
		&& !ParseBlackboardProjection(
			RuntimeRequest, PreflightResult, BlackboardProjection, OutDiagnostics))
	{
		return Result;
	}
	FAssetDocumentEditorCutoverMutationIntent Intent = MakeEditorIntent(RuntimeRequest);
	if (TargetActivation != nullptr)
	{
		Result.TargetDecision = PrepareAssetDocumentRuntimeTargetDecision(*TargetActivation, RuntimeRequest, OutDiagnostics);
		if (!Result.TargetDecision.IsSet()) return Result;
		const FAssetDocumentEditorTargetLifecycleCatalogEntry* Lifecycle = TargetActivation->bExistingOnly ? nullptr
			: FindAssetDocumentEditorTargetLifecycle(TargetActivation->ExactAssetClassPath, TargetActivation->LifecycleId);
		if (!TargetActivation->bExistingOnly && !Lifecycle)
		{
			auto& D = OutDiagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/GenericRuntime");
			D.Code = TEXT("GenericRuntime.TargetActivationInvalid"); D.Message = TEXT("Registered lifecycle is absent from the Editor catalog.");
			return Result;
		}
		Intent.bCreateByFactory = Result.TargetDecision->bCreate;
		Intent.FactoryId = Intent.bCreateByFactory ? Lifecycle->FactoryId : TEXT("");
		// The factory lifecycle owns target acquisition/creation. Future domain Body mutation owns its own footprint.
		Intent.bWriteAdmittedProperties = false;
	}
	if (bMutationProjection) Intent.bWriteAdmittedProperties = false;
 const FAssetDocumentBehaviorTreeOperationClassification BehaviorTree =
		ClassifyAssetDocumentBehaviorTreeOperation(
			RuntimeRequest.RequestedAssetClassPath, RuntimeRequest.CanonicalDocument);
	const bool bBehaviorTree = TargetActivation == nullptr && BehaviorTree.Shape
		!= EAssetDocumentBehaviorTreeOperationShape::NotBehaviorTree;
	const bool bNoPropertyWrite = BehaviorTree.PropertyMode
		== EAssetDocumentBehaviorTreeTopLevelPropertyMode::NoTopLevelPropertyWrite;
	if (bBehaviorTree)
	{
		if (BehaviorTree.Shape
			== EAssetDocumentBehaviorTreeOperationShape::InvalidBehaviorTreeShape
			|| (BehaviorTree.Shape
				== EAssetDocumentBehaviorTreeOperationShape::RetainedTreeOverlay
				&& IsCreateDecision(RuntimeRequest)))
		{
			FAssetDocumentRuntimeDiagnostic& Diagnostic =
				OutDiagnostics.AddDefaulted_GetRef();
			Diagnostic.Path = TEXT("/BehaviorTree");
			Diagnostic.Code = TEXT("GenericRuntime.BehaviorTreeFootprint.InvalidCanonicalPlan");
			Diagnostic.Message = TEXT("BehaviorTree creation requires a complete canonical Tree.");
			return Result;
		}
		Intent.bWriteAdmittedProperties = !bNoPropertyWrite;
		if (IsCreateDecision(RuntimeRequest))
		{
			Intent = MakeBehaviorTreeFactoryIntent(RuntimeRequest, !bNoPropertyWrite);
		}
	}
	if (!bBlackboardApply && !Intent.bCreateByFactory
		&& (Intent.bWriteAdmittedProperties || bBehaviorTree || TargetActivation != nullptr))
	{
		const FSoftObjectPath TargetPath(Intent.CanonicalObjectPath);
		const bool bMayLoadExistingTarget = TargetPath.ResolveObject() != nullptr
			|| IAssetRegistry::GetChecked().GetAssetByObjectPath(TargetPath).IsValid();
		if (bMayLoadExistingTarget)
		{
			(void)TargetPath.TryLoad();
		}
	}
	FAssetDocumentEditorCutoverTransactionPreparation Editor;
	if ((!bBlackboardApply && !bMutationProjection) || Intent.bCreateByFactory)
	{
		Editor = PrepareAssetDocumentEditorCutoverTransaction(Intent);
		if (bBehaviorTree && !Intent.bCreateByFactory
			&& BehaviorTree.Shape
				== EAssetDocumentBehaviorTreeOperationShape::FullTreeLifecycle
			&& IsCreateOrUpdateAction(RuntimeRequest.CanonicalDocument)
			&& HasOnlyInvalidTargetDiagnostic(Editor))
		{
			Editor = PrepareAssetDocumentEditorCutoverTransaction(
				MakeBehaviorTreeFactoryIntent(RuntimeRequest, !bNoPropertyWrite));
		}
		OutDiagnostics.Append(Editor.Diagnostics);
		if (!Editor.bPrepared)
		{
			return Result;
		}
	}
	if (!bBlackboardApply && !Intent.bCreateByFactory)
	{
		UObject* const Target =
			FSoftObjectPath(Intent.CanonicalObjectPath).ResolveObject();
		if (Target != nullptr && Target->GetOutermost()->IsDirty())
		{
			FAssetDocumentRuntimeDiagnostic& Diagnostic =
				OutDiagnostics.AddDefaulted_GetRef();
			Diagnostic.Path = TEXT("/Target");
			Diagnostic.Code = TEXT("DirtyExistingAssetTransactionUnsupported");
			Diagnostic.Message = DescribeAssetDocumentDirtyTarget(Target->GetOutermost()->GetName());
			return Result;
		}
	}
	// Blueprint packages are restored through the derived-state rebuilder only for
	// snapshot-covered (Python profile) requests.
	const bool bSnapshotEligible = RuntimeRequest.RequestedAssetClassPath != TEXT("/Script/Engine.Blueprint")
		|| (bMutationProjection && MutationProjection.bSnapshotCovered);
	const FString SnapshotPackageName = bSnapshotEligible
		? ResolveSnapshotPackageName(Intent.CanonicalObjectPath, Intent.bCreateByFactory) : FString();
	TArray<FAssetDocumentMutationFootprint> CompleteFootprint;
	if (bBlackboardApply)
	{
		if (Intent.bCreateByFactory)
		{
			CompleteFootprint.Append(Editor.CompleteFootprint);
		}
		CompleteFootprint.Append(BlackboardProjection.GetQualifiedFootprint());
	}
	else
	{
		CompleteFootprint = Editor.CompleteFootprint;
	}
	if (bMutationProjection && MutationProjection.bSnapshotCovered)
	{
		// The handler composes primitives dynamically (e.g. graph reconcile); Core's
		// package snapshot (existing) or the factory rollback (create) covers them.
		// Function-argument evaluation order is unspecified (MSVC evaluates right to
		// left): adapt the seeds before the opener moves them out, never in one call.
		TArray<TSharedRef<IAssetDocumentRollbackAction>> SnapshotFactoryActions;
		FAssetDocumentCoreMutationCaptureOpener SnapshotFactoryOpener;
		if (Intent.bCreateByFactory)
		{
			SnapshotFactoryActions = AdaptAssetDocumentEditorRollbackSeeds(Editor.RollbackSeeds);
			SnapshotFactoryOpener = MakeCaptureOpener(MoveTemp(Editor.RollbackSeeds));
		}
		Result.MutationComposition = FAssetDocumentMutationComposition::FreezeSnapshotCovered(RuntimeRequest,
			Intent.bCreateByFactory ? Editor.CompleteFootprint : TArray<FAssetDocumentMutationFootprint>(),
			MoveTemp(SnapshotFactoryActions), MoveTemp(SnapshotFactoryOpener),
			!SnapshotPackageName.IsEmpty(), OutDiagnostics);
		if (!Result.MutationComposition) return Result;
		CompleteFootprint.Reset(); CompleteFootprint.Append(Result.MutationComposition->GetFootprint());
	}
	else if (bMutationProjection)
 {
  TArray<FAssetDocumentMutationComposition::FProviderBatch> Batches;
  TSet<int32> PreparedSteps;
  for (const auto& View : ProviderProjection->PreparationViews)
  {
   TArray<FAssetDocumentMutationIntent> Selected;
   for (const auto& Step : MutationProjection.Steps)
   {
    const auto Resolved = Registry->Resolve(Step.Requirement);
    if (Resolved.bResolved && Resolved.ProviderId == View.Provider->GetProviderId())
    { Selected.Add(Step); PreparedSteps.Add(Step.StepIndex); }
   }
   // An empty sparse Montage update still needs its request owner prepared for
   // the subsequent staged snapshot, without adding a mutation contribution.
   const bool bPrepareEmptyMontageRead = !Intent.bCreateByFactory && MutationProjection.Steps.IsEmpty()
    && RuntimeRequest.RequestedAssetClassPath == TEXT("/Script/Engine.AnimMontage")
    && View.Provider->GetProviderId() == TEXT("animation");
   if (Selected.IsEmpty() && !bPrepareEmptyMontageRead
    && View.Provider->GetProviderId() != TEXT("editor")) continue;
   FAssetDocumentMutationComposition::FProviderBatch Batch;
   Batch.ProviderId = View.Provider->GetProviderId();
   Batch.ActualProvider = View.Provider;
   const FAssetDocumentContractValidationResult R = View.Prepare(RuntimeRequest, Selected, Batch.Batch);
   OutDiagnostics.Append(R.Diagnostics); if (!R.bValid) return Result;
   for (const auto& Contribution : Batch.Batch.Contributions)
    if (!Selected.ContainsByPredicate([&](const auto& Step){ return Step.StepIndex == Contribution.StepIndex; }))
    {
     auto& D = OutDiagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/MutationComposition");
     D.Code = TEXT("MutationComposition.ProviderCoverageMismatch"); D.Message = TEXT("Provider returned another provider's step."); return Result;
    }
   Batches.Add(MoveTemp(Batch));
  }
  if (!SnapshotPackageName.IsEmpty())
   AddSnapshotCoveredContributions(RuntimeRequest, *Registry, *ProviderProjection, MutationProjection.Steps, PreparedSteps, Batches);
  if (PreparedSteps.Num() != MutationProjection.Steps.Num())
  {
   auto& D = OutDiagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/MutationComposition");
   D.Code = TEXT("MutationComposition.PreparationUnavailable"); D.Message = TEXT("Every mutating step requires its selected provider preparation callback."); return Result;
  }
  // Unspecified argument evaluation order (MSVC: right to left): adapt the seeds
  // before the opener moves them out, never inside one call expression.
  TArray<TSharedRef<IAssetDocumentRollbackAction>> FreezeFactoryActions;
  FAssetDocumentCoreMutationCaptureOpener FreezeFactoryOpener;
  if (Intent.bCreateByFactory)
  {
   FreezeFactoryActions = AdaptAssetDocumentEditorRollbackSeeds(Editor.RollbackSeeds);
   FreezeFactoryOpener = MakeCaptureOpener(MoveTemp(Editor.RollbackSeeds));
  }
  Result.MutationComposition = FAssetDocumentMutationComposition::Freeze(RuntimeRequest, MoveTemp(MutationProjection.Steps),
   MoveTemp(Batches), Intent.bCreateByFactory ? Editor.CompleteFootprint : TArray<FAssetDocumentMutationFootprint>(),
   MoveTemp(FreezeFactoryActions), MoveTemp(FreezeFactoryOpener), OutDiagnostics,
   !SnapshotPackageName.IsEmpty());
  if (!Result.MutationComposition) return Result;
  CompleteFootprint.Reset(); CompleteFootprint.Append(Result.MutationComposition->GetFootprint());
 }
 if (bBehaviorTree)
 {
  if (!Editor.bPrepared)
  {
   auto& D = OutDiagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/BehaviorTree");
   D.Code = TEXT("GenericRuntime.BehaviorTreeFootprint.InvalidCanonicalPlan");
   D.Message = TEXT("BehaviorTree footprint planning requires successful Editor preparation.");
   return Result;
  }
  const auto Planned = FAssetDocumentBehaviorTreeProductionFootprintPlanner::Plan(
   RuntimeRequest, BehaviorTree, Editor.CompleteFootprint, CompleteFootprint);
  OutDiagnostics.Append(Planned.Diagnostics);
  if (!Planned.bValid) return Result;
 }
 FAssetDocumentPreflightReceipt Receipt;
	Receipt.RequestId = RuntimeRequest.RequestId;
	Receipt.RuntimeRevision = RuntimeRequest.ExpectedRuntimeRevision;
	Receipt.CompleteFootprint = CompleteFootprint;
	Receipt.ReceiptHash = Validated.SemanticHash;
	Receipt.bSucceeded = true;
	const FString RequestKey =
		RuntimeRequest.RequestId.ToString(EGuidFormats::Digits);
	Result.Mutation = MakeUnique<FAssetDocumentCoreMutationInputs>(
		FAssetDocumentCoreMutationInputs{
			MoveTemp(Receipt),
			MakePersistencePlan(
				ServiceRequest,
				RuntimeRequest,
				Validated,
				CompleteFootprint),
			Verifier,
			{},
			MakeInstalledEvidence(Validated),
			FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("AssetDocument/Runtime/Journals")),
			FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("AssetDocument/Runtime/Journals/Backups"),
				RequestKey),
			FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("AssetDocument/Runtime/Sync")),
			TEXT("document"),
			RuntimeRequest.ExpectedRuntimeRevision,
			Result.MutationComposition ? Result.MutationComposition->MakeInitialActions()
				: AdaptAssetDocumentEditorRollbackSeeds(Editor.RollbackSeeds,
					!SnapshotPackageName.IsEmpty() && !Intent.bCreateByFactory),
			Result.MutationComposition ? Result.MutationComposition->MakeCaptureOpener() : MakeCaptureOpener(MoveTemp(Editor.RollbackSeeds))});
	if (!SnapshotPackageName.IsEmpty())
	{
		Result.Mutation->SnapshotPackageNames.Add(SnapshotPackageName);
	}
	if (bBlackboardApply)
	{
		if (Intent.bCreateByFactory)
		{
			Result.OptionalBlackboardCreatePrefix = Editor.CompleteFootprint;
		}
		Result.BlackboardProjection = MoveTemp(BlackboardProjection);
	}
	return Result;
}
