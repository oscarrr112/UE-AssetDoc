// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Blackboard/AssetDocumentAIBlackboardPrimitives.h"
#include "Provider/Blackboard/AuthorityException/AssetDocumentAIBlackboardStructuralPublication.h"

FAssetDocumentPrimitiveDescriptor MakeAssetDocumentAIBlackboardEntryRemoveV2Descriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = TEXT("ai.blackboard.entry.remove.v2");
	Descriptor.Version = 2;
	Descriptor.ProviderId = TEXT("ai");
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::AI;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = EAssetDocumentPrimitiveAccess::Mutating;
	Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply};
	Descriptor.bRequiresActiveTransaction = true;
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v2");
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v2");
	Descriptor.MutationFootprintSchemaId = TEXT("assetdocument.ai.blackboard.entry.remove.v2");
	FAssetDocumentPrimitiveHandleArgumentSchema Asset;
	Asset.Name = TEXT("asset");
	Asset.Kind = EAssetDocumentHandleKind::Asset;
	Asset.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	Descriptor.HandleArgumentSchemas = {MoveTemp(Asset)};
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
	Descriptor.StableDiagnosticCodes = {
		TEXT("AIProvider.InvalidValueShape"), TEXT("AIProvider.InvalidContext"),
		TEXT("AIProvider.InvalidTransaction"), TEXT("AIProvider.InvalidHandle"),
		TEXT("AIProvider.AuthorityUnavailable"), TEXT("AIProvider.OperationRejected"),
		TEXT("AIProvider.Blackboard.KeyMissing"), TEXT("AIProvider.Blackboard.KeyInherited"),
		TEXT("AIProvider.Blackboard.OrderedKeysLifecycleUnavailable"),
		TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
		TEXT("AIProvider.Blackboard.EntryChangedSetMismatch")};
	Descriptor.RequiredConsumerIds = {TEXT("ai_profile")};
	FAssetDocumentPrimitiveAdmissionEvidence& A = Descriptor.Admission;
	A.Disposition = EAssetDocumentPrimitiveDisposition::AdmittedBridge;
	A.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::BridgeAdmitted;
	A.ExactEngineVersion = TEXT("5.7.4");
	A.PhysicalOwner = EAssetDocumentPrimitiveOwner::AI;
	A.PublicModule = TEXT("AssetDocumentAI+AIModule+PropertyEditor");
	A.PublicHeader = FString(AssetDocumentAIBlackboardStructuralPublication::PublicHeaderPrefix);
	A.ExportedAuthoritativeApi = TEXT(
		"AssetDocumentAIBlackboardStructuralPublication::PublishLocalKeysSnapshot");
	A.AuthorityContextId = Descriptor.PrimitiveId + TEXT(".context.game-thread-editor-ready.v2");
	A.PrimaryOperationId = Descriptor.PrimitiveId;
	A.IdentityLifetimeContractId =
		Descriptor.PrimitiveId + TEXT(".identity.ha-request-transaction-revision.v2");
	A.TransactionContractId = Descriptor.PrimitiveId + TEXT(".transaction.active-ha.v2");
	A.RollbackContractId = TEXT("assetdocument.ai.blackboard.entry.remove.rollback.v2");
	A.CompletionContractId =
		TEXT("ai.blackboard.entry.remove.v2.completion.absent_name_full_readback_and_order.v2");
	A.SaveReloadEvidenceId =
		TEXT("ai.blackboard.entry.remove.v2.provider-no-persistence-fresh-reload.v2");
	A.FailureInjectionTestIds = {
		TEXT("UEAssetDoc.AIProvider.BlackboardV2.Remove.Failure")};
	A.VerificationTestIds = {
		TEXT("UEAssetDoc.AIProvider.Authority.PublicSurface"),
		TEXT("UEAssetDoc.AIProvider.BlackboardV2.StructuralSourceCheck"),
		TEXT("UEAssetDoc.AIProvider.BlackboardV2.Remove")};
	A.SourceMarker = TEXT(
		"ASSETDOCUMENT_AUTHORITY_BRIDGE_ADMISSION(AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1)");
	A.RawLifecycleAllowlistId = TEXT(
		"ASSETDOCUMENT_AUTHORITY_BRIDGE_CALLSITE(AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1|ai.blackboard.entry.remove.v2)");
	A.GovernanceId = TEXT("AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1");
	A.IndependentCandidateReviewId =
		TEXT("candidate-review.initial/AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1");
	A.IndependentBridgeReviewId =
		TEXT("bridge-review.initial/AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1");
	A.HelperReplacementCondition = TEXT(
		"Replace only with a stock public/exported Blackboard-specific lifecycle that fully covers this row.");
	A.RemovalCondition = TEXT(
		"Remove only after migrated row evidence passes and its obsolete quarantine dependency and allowlist are deleted.");
	A.AuthorityFingerprint = TEXT(
		"ue-5.7.4-launcher-cl-51494982|ai|ai.blackboard.entry.remove.v2|"
		"AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1|"
		"WithAssetDocumentEditorAssetIdentity>exact owned-local/full-snapshot preflight>"
		"CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage>"
		"EnlistFootprint(UObjectState[LocalKeysAggregate])>")
		+ FString(AssetDocumentAIBlackboardStructuralPublication::PublicationExportedAuthoritativeApi)
		+ TEXT(">UBlackboardData::UpdateIfHasSynchronizedKeys()>"
			"UBlackboardData::PropagateKeyChangesToDerivedBlackboardAssets()>"
			"complete loaded-consumer completion>absent-name full canonical retained-entry/order readback>"
			"AD.AI.BLACKBOARD.KEYTYPE-STRUCT-CANONICAL-READ-v1");
	return Descriptor;
}
