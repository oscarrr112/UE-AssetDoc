// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/AssetDocumentAnimationPrimitiveCatalog.h"

#include "Provider/Asset/AssetDocumentAnimationAssetOperations.h"
#include "Provider/Curve/AssetDocumentAnimationCurveOperations.h"
#include "Provider/Curve/AssetDocumentAnimationFloatCurveOperations.h"
#include "Provider/Asset/AssetDocumentAnimationSkeletonOperations.h"
#include "Provider/Asset/AssetDocumentAnimationPreviewMeshOperations.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceAdditiveCompletion.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetSourceAsset.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceRetargetSourceName.h"
#include "Provider/Sequence/AssetDocumentAnimationSequenceCompression.h"
#include "Provider/Montage/MontageStructuralProvider.h"
#include "Provider/Montage/MontageProfileSnapshot.h"

namespace AssetDocumentAnimationPrimitiveCatalog
{
constexpr uint32 ProviderVersion = 1;
constexpr const TCHAR* AnimationProfileConsumer = TEXT("animation_profile");
constexpr const TCHAR* AnimationProviderDesignConsumer =
	TEXT("animation_provider_design");

struct FReadOnlyDescriptorSpec
{
	const TCHAR* PrimitiveId;
	const TCHAR* DomainDiagnostic;
	const TCHAR* PublicHeader;
	const TCHAR* ExportedAuthoritativeApi;
	const TCHAR* VerificationTestId;
	const TCHAR* ResultName;
	EAssetDocumentHandleKind ResultKind;
};

FAssetDocumentPrimitiveAdmissionEvidence MakeReadOnlyAdmissionEvidence(
	const FReadOnlyDescriptorSpec& Spec)
{
	FAssetDocumentPrimitiveAdmissionEvidence Evidence;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath =
		EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Evidence.ExactEngineVersion = TEXT("5.7.4");
	Evidence.PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation;
	Evidence.PublicModule = TEXT("Engine");
	Evidence.PublicHeader = Spec.PublicHeader;
	Evidence.ExportedAuthoritativeApi = Spec.ExportedAuthoritativeApi;
	Evidence.AuthorityContextId = TEXT("animation.editor-game-thread.read-only.v1");
	Evidence.PrimaryOperationId = Spec.PrimitiveId;
	Evidence.IdentityLifetimeContractId = TEXT("animation.provider-lease.read-only.v1");
	Evidence.TransactionContractId = TEXT("animation.read-only.no-active-transaction.v1");
	Evidence.RollbackContractId = TEXT("animation.read-only.no-rollback.v1");
	Evidence.CompletionContractId = TEXT("animation.authoritative-readback.v1");
	Evidence.SaveReloadEvidenceId = FString(Spec.PrimitiveId)
		+ TEXT(".read-only-no-persistence.v1");
	Evidence.FailureInjectionTestIds = {Spec.VerificationTestId};
	Evidence.VerificationTestIds = {
		TEXT("UEAssetDoc.AnimationProvider.Authority"),
		TEXT("UEAssetDoc.AnimationProvider.Descriptors"),
		Spec.VerificationTestId};
	Evidence.AuthorityFingerprint = FString::Printf(
		TEXT("ue-5.7.4|animation|Engine|%s|%s|%s"),
		Spec.PublicHeader, Spec.ExportedAuthoritativeApi, Spec.PrimitiveId);
	return Evidence;
}

FAssetDocumentPrimitiveDescriptor MakeReadOnlyDescriptor(
	const FReadOnlyDescriptorSpec& Spec)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = Spec.PrimitiveId;
	Descriptor.Version = ProviderVersion;
	Descriptor.ProviderId = TEXT("animation");
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::Animation;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = EAssetDocumentPrimitiveAccess::ReadOnly;
	Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Verify};
	Descriptor.AllowedHandleKinds = {
		EAssetDocumentHandleKind::Asset, Spec.ResultKind};
	Descriptor.HandleArgumentSchemas = {{
		TEXT("asset"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly, true}};
	Descriptor.HandleResultSchemas = {{
		Spec.ResultName, Spec.ResultKind, EAssetDocumentHandleAccess::ReadOnly,
		EAssetDocumentHandleResultCardinality::ZeroOrMore}};
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v1");
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v1");
	Descriptor.StableDiagnosticCodes = {
		TEXT("AnimationProvider.InvalidValueShape"),
		TEXT("AnimationProvider.InvalidHandlePayload"),
		TEXT("AnimationProvider.TargetClassMismatch"),
		TEXT("AnimationProvider.AuthorityUnavailable"),
		TEXT("AnimationProvider.OperationRejected"),
		TEXT("AnimationProvider.CompletionFailed"),
		TEXT("AnimationProvider.ReadbackMismatch"),
		Spec.DomainDiagnostic};
	Descriptor.RequiredConsumerIds = {AnimationProfileConsumer};
	Descriptor.Admission = MakeReadOnlyAdmissionEvidence(Spec);
	return Descriptor;
}

TArray<FAssetDocumentAnimationDecision> BuildDecisions()
{
	return {
		{TEXT("animation.asset.preview_mesh.set.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("ANIMATION-ASSET"), TEXT("")},
		{TEXT("animation.sequence.retarget_source_asset.set.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("ANIMATION-ASSET"), TEXT("AD.ANIMATION.BLOCK.RETARGET_SOURCE_AUTHORITY")},
        {TEXT("animation.sequence.retarget_source_asset.set.v2"), 2, EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("ANIMATION-ASSET"), TEXT("")},
        {TEXT("animation.sequence.retarget_source_name.set.v1"), 1, EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("ANIMATION-ASSET"), TEXT("")},
        {TEXT("animation.sequence.compression.settings.set.v1"), 1, EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("ANIMATION-SEQUENCE"), TEXT("")},
		{TEXT("animation.curve.float.enumerate.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("ANIMATION-DATA"), TEXT("")},
		{TEXT("animation.curve.float.enumerate.v2"), 2, EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("ANIMATION-DATA"), TEXT("")},
        {TEXT("animation.curve.float.replace.v2"), 2, EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("ANIMATION-CURVE-EXACT"), TEXT("")},
		{TEXT("animation.curve.float.replace.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("ANIMATION-DATA"), TEXT("AD.ANIMATION.BLOCK.ROLLBACK_COMPOSITION")},
		{TEXT("animation.notify_track.enumerate.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("ANIMATION-TIMELINE"), TEXT("AD.ANIMATION.BLOCK.NOTIFY_LIBRARY_DEPENDENCY")},
		{TEXT("animation.notify_track.replace.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("ANIMATION-TIMELINE"), TEXT("AD.ANIMATION.BLOCK.NOTIFY_LIBRARY_DEPENDENCY")},
		{TEXT("animation.notify_event.enumerate.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("ANIMATION-TIMELINE"), TEXT("AD.ANIMATION.BLOCK.NOTIFY_LIBRARY_DEPENDENCY")},
		{TEXT("animation.notify_event.remove.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("ANIMATION-TIMELINE"), TEXT("AD.ANIMATION.BLOCK.RUNTIME_HANDLE_BOOTSTRAP")},
		{TEXT("animation.notify_event.add.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("ANIMATION-TIMELINE"), TEXT("AD.ANIMATION.BLOCK.RUNTIME_HANDLE_BOOTSTRAP")},
		{TEXT("animation.sync_marker.enumerate.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("ANIMATION-TIMELINE"), TEXT("AD.ANIMATION.BLOCK.NOTIFY_LIBRARY_DEPENDENCY")},
		{TEXT("animation.sync_marker.replace.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("ANIMATION-TIMELINE"), TEXT("AD.ANIMATION.BLOCK.NOTIFY_LIBRARY_DEPENDENCY")},
		{TEXT("animation.metadata.enumerate.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("ANIMATION-METADATA"), TEXT("")},
		{TEXT("animation.asset_metadata.replace.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("ANIMATION-METADATA"), TEXT("AD.ANIMATION.BLOCK.RUNTIME_HANDLE_BOOTSTRAP")},
		{TEXT("animation.montage.section_metadata.replace.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::FrozenStableBlocker, AnimationProviderDesignConsumer, TEXT("FROZEN-AUTHORITY"), TEXT("AD.BLOCK.ANIMATION.SECTION_METADATA_AUTHORITY")},
		{TEXT("animation.asset_user_data.enumerate.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("ANIMATION-METADATA"), TEXT("")},
		{TEXT("animation.asset_user_data.replace_managed.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::FrozenStableBlocker, AnimationProviderDesignConsumer, TEXT("FROZEN-AUTHORITY"), TEXT("AD.BLOCK.ANIMATION.ASSET_USER_DATA_LIFECYCLE")},
		{TEXT("animation.sequence.complete_edit.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("ANIMATION-SEQUENCE"), TEXT("AD.ANIMATION.BLOCK.SEQUENCE_COMPLETION_AUTHORITY")},
        {TEXT("animation.sequence.complete_edit.v2"), 2, EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("ANIMATION-SEQUENCE"), TEXT("")},
		{TEXT("animation.montage.slot_tracks.replace.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("MONTAGE-STRUCTURAL"), TEXT("")},
		{TEXT("animation.montage.sections.mutate.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("MONTAGE-STRUCTURAL"), TEXT("")},
		{TEXT("animation.montage.profile.snapshot.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("MONTAGE-PROFILE"), TEXT("")},
		{TEXT("animation.montage.sync_cache.complete.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::FrozenStableBlocker, AnimationProviderDesignConsumer, TEXT("FROZEN-AUTHORITY"), TEXT("AD.BLOCK.ANIMATION.SYNC_CACHE_AUTHORITY")},
		{TEXT("animation.montage.time_stretch.configure_bake.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::FrozenStableBlocker, AnimationProviderDesignConsumer, TEXT("FROZEN-AUTHORITY"), TEXT("AD.BLOCK.ANIMATION.TIME_STRETCH_CONFIG_AUTHORITY")},
		{TEXT("animation.linked_anim_layer.action.enumerate.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("ANIMATION-GRAPH"), TEXT("AD.ANIMATION.BLOCK.RUNTIME_HANDLE_BOOTSTRAP")},
		{TEXT("animation.anim_graph.lifecycle.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("ANIMATION-GRAPH"), TEXT("AD.ANIMATION.BLOCK.RUNTIME_HANDLE_BOOTSTRAP")},
		{TEXT("animation.state_machine.node.create.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::FrozenStableBlocker, AnimationProviderDesignConsumer, TEXT("FROZEN-AUTHORITY"), TEXT("AD.BLOCK.ANIMATION.STATE_MACHINE_NODE_AUTHORITY")},
		{TEXT("animation.linked_anim_layer.edit.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("ANIMATION-GRAPH"), TEXT("AD.ANIMATION.BLOCK.RUNTIME_HANDLE_BOOTSTRAP")},
		{TEXT("animation.anim_parent_override.replace.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::FrozenStableBlocker, AnimationProviderDesignConsumer, TEXT("FROZEN-AUTHORITY"), TEXT("AD.BLOCK.ANIMATION.PARENT_OVERRIDE_LIFECYCLE")},
		{TEXT("animation.state_transition.connect.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("ANIMATION-GRAPH"), TEXT("AD.ANIMATION.BLOCK.RUNTIME_HANDLE_BOOTSTRAP")},
		{TEXT("animation.asset.skeleton.set.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("ANIMATION-ASSET"), TEXT("")},
		{TEXT("animation.state_entry.connect.v1"), ProviderVersion, EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("ANIMATION-GRAPH"), TEXT("AD.ANIMATION.BLOCK.RUNTIME_HANDLE_BOOTSTRAP")},
	};
}

FAssetDocumentPrimitiveDescriptor MakeCurveSnapshotDescriptor()
{
 auto D = MakeReadOnlyDescriptor({
  TEXT("animation.curve.float.enumerate.v2"), TEXT("AnimationProvider.Curve.DataModelUnavailable"),
  TEXT("Animation/AnimData/IAnimationDataModel.h;Animation/AnimSequenceBase.h"),
  TEXT("UAnimSequenceBase::GetDataModel;IAnimationDataModel::GetFloatCurves"),
  TEXT("UEAssetDoc.AnimationProvider.CurveSnapshot"), TEXT(""), EAssetDocumentHandleKind::Asset});
 D.Version = 2;
 D.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
 D.HandleResultSchemas.Reset();
 D.InputSchemaId = TEXT("animation.curve.float.enumerate.input.v2");
 D.OutputSchemaId = TEXT("animation.curve.float.enumerate.output.v2");
 return D;
}

TArray<FAssetDocumentPrimitiveDescriptor> BuildDescriptors()
{
	return {
		MakeAnimationSkeletonSetDescriptor(),
		MakeAnimationPreviewMeshSetDescriptor(),
        MakeAnimationSequenceAdditiveCompletionDescriptor(),
        MakeAnimationSequenceRetargetSourceAssetDescriptor(),
        MakeAnimationSequenceRetargetSourceNameDescriptor(),
        MakeAnimationSequenceCompressionDescriptor(),
        MakeAnimationMontageSlotsDescriptor(),
        MakeAnimationMontageSectionsDescriptor(),
        MakeAnimationMontageProfileSnapshotDescriptor(),
		MakeCurveSnapshotDescriptor(),
        MakeAnimationFloatCurveReplaceDescriptor(),
		MakeReadOnlyDescriptor({
			TEXT("animation.curve.float.enumerate.v1"),
			TEXT("AnimationProvider.Curve.DataModelUnavailable"),
			TEXT("Animation/AnimData/IAnimationDataModel.h;Animation/AnimSequenceBase.h"),
			TEXT("UAnimSequenceBase::GetDataModel;IAnimationDataModel::GetFloatCurves"),
			TEXT("UEAssetDoc.AnimationProvider.DataController"),
			TEXT("curves"), EAssetDocumentHandleKind::Curve}),
		MakeReadOnlyDescriptor({
			TEXT("animation.metadata.enumerate.v1"),
			TEXT("AnimationProvider.Metadata.EnumerationFailed"),
			TEXT("Animation/AnimationAsset.h"), TEXT("UAnimationAsset::GetMetaData"),
			TEXT("UEAssetDoc.AnimationProvider.MetadataLifecycle"),
			TEXT("metadata"), EAssetDocumentHandleKind::Subobject}),
		MakeReadOnlyDescriptor({
			TEXT("animation.asset_user_data.enumerate.v1"),
			TEXT("AnimationProvider.AssetUserData.EnumerationFailed"),
			TEXT("Animation/AnimationAsset.h"),
			TEXT("UAnimationAsset::GetAssetUserDataArray"),
			TEXT("UEAssetDoc.AnimationProvider.AssetLifecycle"),
			TEXT("user_data"),
			EAssetDocumentHandleKind::Subobject}),
	};
}

TArray<FAssetDocumentAnimationPrimitiveBinding> BuildBindings()
{
	return {
		{TEXT("animation.asset.skeleton.set.v1"), ProviderVersion, &InvokeAnimationSkeletonSet},
		{TEXT("animation.asset.preview_mesh.set.v1"), ProviderVersion, &InvokeAnimationPreviewMeshSet},
        {TEXT("animation.sequence.complete_edit.v2"), 2, &InvokeAnimationSequenceAdditiveCompletion},
        {TEXT("animation.sequence.retarget_source_asset.set.v2"), 2, &InvokeAnimationSequenceRetargetSourceAsset},
        {TEXT("animation.sequence.retarget_source_name.set.v1"), 1, &InvokeAnimationSequenceRetargetSourceName},
        {TEXT("animation.sequence.compression.settings.set.v1"), 1, &InvokeAnimationSequenceCompression},
        {TEXT("animation.montage.slot_tracks.replace.v1"), ProviderVersion, &InvokeAnimationMontageStructural},
        {TEXT("animation.montage.sections.mutate.v1"), ProviderVersion, &InvokeAnimationMontageStructural},
        {TEXT("animation.montage.profile.snapshot.v1"), ProviderVersion, &InvokeAnimationMontageProfileSnapshot},
		{TEXT("animation.curve.float.enumerate.v2"), 2, &InvokeAnimationFloatCurveEnumerateV2},
        {TEXT("animation.curve.float.replace.v2"), 2, &InvokeAnimationFloatCurveReplace},
		{TEXT("animation.curve.float.enumerate.v1"), ProviderVersion,
			&InvokeAnimationFloatCurveEnumerate},
		{TEXT("animation.metadata.enumerate.v1"), ProviderVersion,
			&InvokeAnimationMetadataEnumerate},
		{TEXT("animation.asset_user_data.enumerate.v1"), ProviderVersion,
			&InvokeAnimationAssetUserDataEnumerate},
	};
}
} // namespace AssetDocumentAnimationPrimitiveCatalog

TConstArrayView<FAssetDocumentAnimationDecision>
GetAssetDocumentAnimationDecisions()
{
	static const TArray<FAssetDocumentAnimationDecision> Decisions =
		AssetDocumentAnimationPrimitiveCatalog::BuildDecisions();
	return Decisions;
}

TConstArrayView<FAssetDocumentPrimitiveDescriptor>
GetAssetDocumentAnimationPrimitiveDescriptors()
{
	static const TArray<FAssetDocumentPrimitiveDescriptor> Descriptors =
		AssetDocumentAnimationPrimitiveCatalog::BuildDescriptors();
	return Descriptors;
}

TConstArrayView<FAssetDocumentAnimationPrimitiveBinding>
GetAssetDocumentAnimationPrimitiveBindings()
{
	static const TArray<FAssetDocumentAnimationPrimitiveBinding> Bindings =
		AssetDocumentAnimationPrimitiveCatalog::BuildBindings();
	return Bindings;
}
