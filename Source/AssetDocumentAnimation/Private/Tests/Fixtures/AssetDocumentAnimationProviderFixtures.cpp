// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/Fixtures/AssetDocumentAnimationProviderFixtures.h"

namespace
{
constexpr uint8 OneRequiredHandle = 1;
constexpr uint8 TwoRequiredHandles = 2;
constexpr uint8 ThreeRequiredHandles = 3;
constexpr uint8 FourRequiredHandles = 4;
constexpr EAssetDocumentAnimationBootstrapDependency NoBootstrap =
	EAssetDocumentAnimationBootstrapDependency::None;
constexpr EAssetDocumentAnimationBootstrapDependency AssetBootstrap =
	EAssetDocumentAnimationBootstrapDependency::Asset;
constexpr EAssetDocumentAnimationBootstrapDependency AssetAndDerivedBootstrap =
	EAssetDocumentAnimationBootstrapDependency::AssetAndProviderDerived;
constexpr EAssetDocumentAnimationBootstrapDependency AssetPairBootstrap =
	EAssetDocumentAnimationBootstrapDependency::AssetPair;
constexpr EAssetDocumentAnimationBootstrapDependency AssetAndClassBootstrap =
	EAssetDocumentAnimationBootstrapDependency::AssetAndClass;
constexpr EAssetDocumentAnimationBootstrapDependency GraphOrNodeBootstrap =
	EAssetDocumentAnimationBootstrapDependency::GraphOrNode;
constexpr EAssetDocumentAnimationBootstrapDisposition FrozenNotApplicable =
	EAssetDocumentAnimationBootstrapDisposition::FrozenNotApplicable;
constexpr EAssetDocumentAnimationBootstrapDisposition VerifiedAssetIdentityReady =
	EAssetDocumentAnimationBootstrapDisposition::VerifiedAssetIdentityReady;
constexpr EAssetDocumentAnimationBootstrapDisposition RuntimeHandleBootstrapBlocked =
	EAssetDocumentAnimationBootstrapDisposition::RuntimeHandleBootstrapBlocked;
constexpr EAssetDocumentAnimationFinalDisposition Admitted =
	EAssetDocumentAnimationFinalDisposition::Admitted;
constexpr EAssetDocumentAnimationFinalDisposition StableBlocker =
	EAssetDocumentAnimationFinalDisposition::StableBlocker;
constexpr const TCHAR* NoFinalDiagnostic = TEXT("none");
constexpr const TCHAR* RuntimeHandleBootstrapDiagnostic =
	TEXT("AD.ANIMATION.BLOCK.RUNTIME_HANDLE_BOOTSTRAP");
constexpr const TCHAR* RollbackCompositionDiagnostic =
	TEXT("AD.ANIMATION.BLOCK.ROLLBACK_COMPOSITION");
} // namespace

TConstArrayView<FAssetDocumentAnimationAuthorityDecisionRecord>
GetAssetDocumentAnimationAuthorityDecisionRecords()
{
	static const TArray<FAssetDocumentAnimationAuthorityDecisionRecord> Records = {
		{TEXT("animation.asset.preview_mesh.set.v1"), TEXT("ANIMATION-ASSET"), TEXT("AD.ANIMATION.BLOCK.PREVIEW_MESH_AUTHORITY"), false, true, OneRequiredHandle, AssetPairBootstrap, VerifiedAssetIdentityReady, Admitted, NoFinalDiagnostic},
		{TEXT("animation.sequence.retarget_source_asset.set.v1"), TEXT("ANIMATION-ASSET"), TEXT("AD.ANIMATION.BLOCK.RETARGET_SOURCE_AUTHORITY"), false, true, OneRequiredHandle, AssetBootstrap, VerifiedAssetIdentityReady, StableBlocker, TEXT("AD.ANIMATION.BLOCK.RETARGET_SOURCE_AUTHORITY")},
		{TEXT("animation.curve.float.enumerate.v1"), TEXT("ANIMATION-DATA"), TEXT("AD.ANIMATION.BLOCK.DATA_MODEL_AUTHORITY"), false, false, OneRequiredHandle, AssetBootstrap, VerifiedAssetIdentityReady, Admitted, NoFinalDiagnostic},
		{TEXT("animation.curve.float.enumerate.v2"), TEXT("ANIMATION-DATA"), TEXT("AD.ANIMATION.BLOCK.DATA_MODEL_AUTHORITY"), false, false, OneRequiredHandle, AssetBootstrap, VerifiedAssetIdentityReady, Admitted, NoFinalDiagnostic},
		{TEXT("animation.curve.float.replace.v2"), TEXT("ANIMATION-CURVE-EXACT"), TEXT("none"), false, true, OneRequiredHandle, AssetBootstrap, VerifiedAssetIdentityReady, Admitted, NoFinalDiagnostic},
		{TEXT("animation.curve.float.replace.v1"), TEXT("ANIMATION-DATA"), TEXT("AD.ANIMATION.BLOCK.DATA_CONTROLLER_AUTHORITY"), false, true, OneRequiredHandle, AssetBootstrap, VerifiedAssetIdentityReady, StableBlocker, RollbackCompositionDiagnostic},
		{TEXT("animation.notify_track.enumerate.v1"), TEXT("ANIMATION-TIMELINE"), TEXT("AD.ANIMATION.BLOCK.NOTIFY_LIBRARY_DEPENDENCY"), false, false, OneRequiredHandle, AssetBootstrap, VerifiedAssetIdentityReady, StableBlocker, TEXT("AD.ANIMATION.BLOCK.NOTIFY_LIBRARY_DEPENDENCY")},
		{TEXT("animation.notify_track.replace.v1"), TEXT("ANIMATION-TIMELINE"), TEXT("AD.ANIMATION.BLOCK.NOTIFY_LIBRARY_DEPENDENCY"), false, true, OneRequiredHandle, AssetBootstrap, VerifiedAssetIdentityReady, StableBlocker, TEXT("AD.ANIMATION.BLOCK.NOTIFY_LIBRARY_DEPENDENCY")},
		{TEXT("animation.notify_event.enumerate.v1"), TEXT("ANIMATION-TIMELINE"), TEXT("AD.ANIMATION.BLOCK.NOTIFY_LIBRARY_DEPENDENCY"), false, false, OneRequiredHandle, AssetBootstrap, VerifiedAssetIdentityReady, StableBlocker, TEXT("AD.ANIMATION.BLOCK.NOTIFY_LIBRARY_DEPENDENCY")},
		{TEXT("animation.notify_event.remove.v1"), TEXT("ANIMATION-TIMELINE"), TEXT("AD.ANIMATION.BLOCK.NOTIFY_EVENT_IDENTITY_AUTHORITY"), false, true, TwoRequiredHandles, AssetAndDerivedBootstrap, RuntimeHandleBootstrapBlocked, StableBlocker, RuntimeHandleBootstrapDiagnostic},
		{TEXT("animation.notify_event.add.v1"), TEXT("ANIMATION-TIMELINE"), TEXT("AD.ANIMATION.BLOCK.NOTIFY_LIBRARY_DEPENDENCY"), false, true, TwoRequiredHandles, AssetAndClassBootstrap, RuntimeHandleBootstrapBlocked, StableBlocker, RuntimeHandleBootstrapDiagnostic},
		{TEXT("animation.sync_marker.enumerate.v1"), TEXT("ANIMATION-TIMELINE"), TEXT("AD.ANIMATION.BLOCK.NOTIFY_LIBRARY_DEPENDENCY"), false, false, OneRequiredHandle, AssetBootstrap, VerifiedAssetIdentityReady, StableBlocker, TEXT("AD.ANIMATION.BLOCK.NOTIFY_LIBRARY_DEPENDENCY")},
		{TEXT("animation.sync_marker.replace.v1"), TEXT("ANIMATION-TIMELINE"), TEXT("AD.ANIMATION.BLOCK.NOTIFY_LIBRARY_DEPENDENCY"), false, true, OneRequiredHandle, AssetBootstrap, VerifiedAssetIdentityReady, StableBlocker, TEXT("AD.ANIMATION.BLOCK.NOTIFY_LIBRARY_DEPENDENCY")},
		{TEXT("animation.metadata.enumerate.v1"), TEXT("ANIMATION-METADATA"), TEXT("AD.ANIMATION.BLOCK.METADATA_OBSERVATION_AUTHORITY"), false, false, OneRequiredHandle, AssetBootstrap, VerifiedAssetIdentityReady, Admitted, NoFinalDiagnostic},
		{TEXT("animation.asset_metadata.replace.v1"), TEXT("ANIMATION-METADATA"), TEXT("AD.ANIMATION.BLOCK.ASSET_METADATA_LIFECYCLE"), false, true, OneRequiredHandle, AssetAndDerivedBootstrap, RuntimeHandleBootstrapBlocked, StableBlocker, RuntimeHandleBootstrapDiagnostic},
		{TEXT("animation.montage.section_metadata.replace.v1"), TEXT("ANIMATION-METADATA"), TEXT("AD.BLOCK.ANIMATION.SECTION_METADATA_AUTHORITY"), true, false, 0, NoBootstrap, FrozenNotApplicable, StableBlocker, TEXT("AD.BLOCK.ANIMATION.SECTION_METADATA_AUTHORITY")},
		{TEXT("animation.asset_user_data.enumerate.v1"), TEXT("ANIMATION-METADATA"), TEXT("AD.ANIMATION.BLOCK.ASSET_USER_DATA_OBSERVATION_AUTHORITY"), false, false, OneRequiredHandle, AssetBootstrap, VerifiedAssetIdentityReady, Admitted, NoFinalDiagnostic},
		{TEXT("animation.asset_user_data.replace_managed.v1"), TEXT("ANIMATION-METADATA"), TEXT("AD.BLOCK.ANIMATION.ASSET_USER_DATA_LIFECYCLE"), true, false, 0, NoBootstrap, FrozenNotApplicable, StableBlocker, TEXT("AD.BLOCK.ANIMATION.ASSET_USER_DATA_LIFECYCLE")},
		{TEXT("animation.sequence.complete_edit.v1"), TEXT("ANIMATION-SEQUENCE"), TEXT("AD.ANIMATION.BLOCK.SEQUENCE_COMPLETION_AUTHORITY"), false, true, OneRequiredHandle, AssetBootstrap, VerifiedAssetIdentityReady, StableBlocker, TEXT("AD.ANIMATION.BLOCK.SEQUENCE_COMPLETION_AUTHORITY")},
		{TEXT("animation.montage.slot_tracks.replace.v1"), TEXT("MONTAGE-STRUCTURAL"), TEXT("AD.BLOCK.ANIMATION.SLOT_TRACK_LIFECYCLE"), false, true, OneRequiredHandle, AssetBootstrap, VerifiedAssetIdentityReady, Admitted, NoFinalDiagnostic},
		{TEXT("animation.montage.sections.mutate.v1"), TEXT("MONTAGE-STRUCTURAL"), TEXT("AD.BLOCK.ANIMATION.SECTION_LIFECYCLE"), false, true, OneRequiredHandle, AssetBootstrap, VerifiedAssetIdentityReady, Admitted, NoFinalDiagnostic},
		{TEXT("animation.montage.sync_cache.complete.v1"), TEXT("ANIMATION-SEQUENCE"), TEXT("AD.BLOCK.ANIMATION.SYNC_CACHE_AUTHORITY"), true, false, 0, NoBootstrap, FrozenNotApplicable, StableBlocker, TEXT("AD.BLOCK.ANIMATION.SYNC_CACHE_AUTHORITY")},
		{TEXT("animation.montage.time_stretch.configure_bake.v1"), TEXT("ANIMATION-SEQUENCE"), TEXT("AD.BLOCK.ANIMATION.TIME_STRETCH_CONFIG_AUTHORITY"), true, false, 0, NoBootstrap, FrozenNotApplicable, StableBlocker, TEXT("AD.BLOCK.ANIMATION.TIME_STRETCH_CONFIG_AUTHORITY")},
		{TEXT("animation.linked_anim_layer.action.enumerate.v1"), TEXT("ANIMATION-GRAPH"), TEXT("AD.ANIMATION.BLOCK.LINKED_LAYER_ACTION_AUTHORITY"), false, false, TwoRequiredHandles, GraphOrNodeBootstrap, RuntimeHandleBootstrapBlocked, StableBlocker, RuntimeHandleBootstrapDiagnostic},
		{TEXT("animation.anim_graph.lifecycle.v1"), TEXT("ANIMATION-GRAPH"), TEXT("AD.ANIMATION.BLOCK.ANIM_GRAPH_LIFECYCLE_AUTHORITY"), false, true, OneRequiredHandle, GraphOrNodeBootstrap, RuntimeHandleBootstrapBlocked, StableBlocker, RuntimeHandleBootstrapDiagnostic},
		{TEXT("animation.state_machine.node.create.v1"), TEXT("ANIMATION-GRAPH"), TEXT("AD.BLOCK.ANIMATION.STATE_MACHINE_NODE_AUTHORITY"), true, false, 0, NoBootstrap, FrozenNotApplicable, StableBlocker, TEXT("AD.BLOCK.ANIMATION.STATE_MACHINE_NODE_AUTHORITY")},
		{TEXT("animation.linked_anim_layer.edit.v1"), TEXT("ANIMATION-GRAPH"), TEXT("AD.ANIMATION.BLOCK.LINKED_LAYER_EDIT_AUTHORITY"), false, true, TwoRequiredHandles, GraphOrNodeBootstrap, RuntimeHandleBootstrapBlocked, StableBlocker, RuntimeHandleBootstrapDiagnostic},
		{TEXT("animation.anim_parent_override.replace.v1"), TEXT("ANIMATION-GRAPH"), TEXT("AD.BLOCK.ANIMATION.PARENT_OVERRIDE_LIFECYCLE"), true, false, 0, NoBootstrap, FrozenNotApplicable, StableBlocker, TEXT("AD.BLOCK.ANIMATION.PARENT_OVERRIDE_LIFECYCLE")},
		{TEXT("animation.state_transition.connect.v1"), TEXT("ANIMATION-GRAPH"), TEXT("AD.ANIMATION.BLOCK.STATE_TRANSITION_AUTHORITY"), false, true, FourRequiredHandles, GraphOrNodeBootstrap, RuntimeHandleBootstrapBlocked, StableBlocker, RuntimeHandleBootstrapDiagnostic},
		{TEXT("animation.asset.skeleton.set.v1"), TEXT("ANIMATION-ASSET"), TEXT("AD.ANIMATION.BLOCK.SKELETON_SET_AUTHORITY"), false, true, OneRequiredHandle, AssetPairBootstrap, VerifiedAssetIdentityReady, Admitted, NoFinalDiagnostic},
		{TEXT("animation.state_entry.connect.v1"), TEXT("ANIMATION-GRAPH"), TEXT("AD.ANIMATION.BLOCK.STATE_ENTRY_AUTHORITY"), false, true, ThreeRequiredHandles, GraphOrNodeBootstrap, RuntimeHandleBootstrapBlocked, StableBlocker, RuntimeHandleBootstrapDiagnostic},
	};
	return Records;
}

TConstArrayView<FAssetDocumentAnimationAuthorityEvidence>
GetAssetDocumentAnimationAuthorityEvidence()
{
	static const TArray<FAssetDocumentAnimationAuthorityEvidence> EvidenceRows = {
		{
			TEXT("ANIMATION-HANDLE"),
			TEXT("AssetDocument; AssetDocumentEditor"),
			TEXT("Runtime/AssetDocumentRequestCoordinator.h; Runtime/AssetDocumentHandleRegistry.h; AssetDocumentEditorNativeState.h"),
			TEXT("ASSETDOCUMENT_API; ASSETDOCUMENTEDITOR_API"),
			TEXT("IAssetDocumentNativeRequestContext::Handoff; IAssetDocumentVerifiedHandleLease::GetPayloadChecked; WithAssetDocumentEditorAssetIdentity; FAssetDocumentEditorAssetIdentityView::{GetObject,GetCanonicalObjectPath,GetTypeRevision}"),
			TEXT("request-local Asset handoff followed by the public Editor typed-identity callback for an Animation descriptor lease"),
			TEXT("the callback supplies a noncopyable exact UObject identity bound to the verified Asset lease, request, transaction, runtime revision, and access"),
			TEXT("mutable handoff preserves the active transaction identity; read-only handoff does not require a transaction"),
			TEXT("the real integrated EditorMutableToAnimationTypedObjectLease scope proves Asset and AssetPair bootstrap; no public derivation proves derived, Class, Graph, or Node bootstrap"),
			TEXT("the public AssetDocument and AssetDocumentEditor symbols compile and link; 13 Asset/AssetPair rows are bootstrap-ready, direct foreign mismatch remains HandleMetadata.ProviderMismatch, and 2 derived plus 1 Class plus 5 GraphNode rows remain blocked"),
			TEXT("UEAssetDoc.AnimationProvider.HandleContract; UEAssetDoc.HandleHandoff.EditorMutableToAnimationTypedObjectLease; UEAssetDoc.HandleHandoff.ForeignDirectProviderMismatch"),
			TEXT("AD.ANIMATION.BLOCK.RUNTIME_HANDLE_BOOTSTRAP"),
			false,
		},
		{
			TEXT("ANIMATION-ROLLBACK"),
			TEXT("AssetDocument; AssetDocumentEditor"),
			TEXT("AssetDocumentCoreRuntimeComposition.h; AssetDocumentRollbackAction.h; Runtime/AssetDocumentAtomicTransaction.h; AssetDocumentEditorNativeState.h"),
			TEXT("ASSETDOCUMENT_API; ASSETDOCUMENTEDITOR_API"),
			TEXT("FAssetDocumentRollbackActionComposition::BeginLiveAfterPreflight; IAssetDocumentAtomicTransaction::{EnlistRollbackAction,EnlistFootprint,Rollback}; CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage"),
			TEXT("the shared AssetDocument action-before-footprint enlistment prerequisite is closed before any provider-owned mutation"),
			TEXT("the live transaction strongly owns an enlisted action, but the narrow typed preimage helper requires an exact IPropertyHandle and PropertyEditor row authority inaccessible to the Animation provider under the frozen allowlist"),
			TEXT("missing coverage fails before mutation and preserves Live; callback existence alone does not admit an Animation mutator"),
			TEXT("Skeleton, Preview, Curves and Montage structural operations have their own typed rollback composition; remaining mutators retain their exact blockers"),
			TEXT("the public shared transaction contract compiles, links, rejects uncovered footprint, then accepts action-before-footprint and rolls back once; the typed preimage callback is not sufficient Animation provider authority"),
			TEXT("UEAssetDoc.AnimationProvider.RollbackContract"),
			TEXT("AD.ANIMATION.BLOCK.ROLLBACK_COMPOSITION"),
			false,
		},
		{
			TEXT("ANIMATION-ASSET"),
			TEXT("Engine"),
			TEXT("Animation/AnimationAsset.h; Animation/AnimSequence.h"),
			TEXT("ENGINE_API"),
			TEXT("SetPreviewMesh; SetSkeleton; SetRetargetSourceAsset; ClearRetargetSourceAsset; UpdateRetargetSourceAssetData"),
			TEXT("editor game thread with compatible object handles"),
			TEXT("asset, mesh, and skeleton UObject lifetimes; compile/rebuild invalidates dependent handles"),
			TEXT("target UObject and package preimage required"),
			TEXT("setter-specific compatibility, refresh, and readback remain row-gated"),
			TEXT("all named public symbols compile and link through frozen Engine dependency"),
			TEXT("UEAssetDoc.AnimationProvider.Authority"),
			TEXT("AD.ANIMATION.BLOCK.SKELETON_SET_AUTHORITY"),
			false,
		},
		{
			TEXT("ANIMATION-DATA"),
			TEXT("Engine"),
			TEXT("Animation/AnimData/IAnimationDataController.h; Animation/AnimData/IAnimationDataModel.h; Animation/AnimSequenceBase.h"),
			TEXT("public interface virtuals; ENGINE_API GetDataModel"),
			TEXT("GetDataModel; GetFloatCurves; FScopedBracket; AddCurve; RemoveCurve; SetCurveFlags; SetCurveKeys"),
			TEXT("editor game thread, controller bracket with bShouldTransact=false"),
			TEXT("sequence handle owns the model/controller observation lifetime"),
			TEXT("outer AssetDocument transaction owns rollback; controller undo is disabled"),
			TEXT("controller notifications followed by model readback"),
			TEXT("all frozen data-model and controller signatures compile and link"),
			TEXT("UEAssetDoc.AnimationProvider.Authority"),
			TEXT("none"),
			true,
		},
		{
			TEXT("ANIMATION-TIMELINE"),
			TEXT("AnimationBlueprintLibrary"),
			TEXT("AnimationBlueprintLibrary.h"),
			TEXT("ANIMATIONBLUEPRINTLIBRARY_API"),
			TEXT("UAnimationBlueprintLibrary notify-track, notify-event, and sync-marker helpers"),
			TEXT("editor game thread"),
			TEXT("event identity must survive ordered readback and exact removal"),
			TEXT("asset/package/event preimages required for mutators"),
			TEXT("library refresh and stable ordered readback"),
			TEXT("header is not reachable from the frozen module dependency graph"),
			TEXT("UEAssetDoc.AnimationProvider.Authority"),
			TEXT("AD.ANIMATION.BLOCK.NOTIFY_LIBRARY_DEPENDENCY"),
			false,
		},
		{
			TEXT("ANIMATION-METADATA"),
			TEXT("Engine"),
			TEXT("Animation/AnimationAsset.h"),
			TEXT("ENGINE_API on mutation and user-data observation; public inline metadata observation"),
			TEXT("GetMetaData; AddMetaData; RemoveMetaData; GetAssetUserDataArray"),
			TEXT("editor game thread"),
			TEXT("asset owns attached metadata/user-data object lifetimes"),
			TEXT("asset/package/attachment preimage required for metadata mutation"),
			TEXT("ordered attachment readback"),
			TEXT("all admitted observation and metadata attachment signatures compile and link"),
			TEXT("UEAssetDoc.AnimationProvider.Authority"),
			TEXT("none"),
			true,
		},
		{
			TEXT("ANIMATION-SEQUENCE"),
			TEXT("Engine"),
			TEXT("Animation/AnimSequence.h; Animation/AnimSequenceBase.h"),
			TEXT("ENGINE_API"),
			TEXT("RefreshCacheData; RefreshSyncMarkerDataFromAuthored; public retarget refresh APIs"),
			TEXT("editor game thread"),
			TEXT("sequence and rebuild-invalidated dependent handles"),
			TEXT("sequence/package/cache preimage required"),
			TEXT("no single public completion chain covers every frozen completion_kind with fresh readback"),
			TEXT("partial public completion symbols compile and link; complete-edit authority remains open"),
			TEXT("UEAssetDoc.AnimationProvider.Authority"),
			TEXT("AD.ANIMATION.BLOCK.SEQUENCE_COMPLETION_AUTHORITY"),
			false,
		},
		{
			TEXT("ANIMATION-GRAPH"),
			TEXT("AnimGraph"),
			TEXT("AnimStateTransitionNode.h; AnimStateEntryNode.h"),
			TEXT("ANIMGRAPH_API"),
			TEXT("CreateConnections; GetPreviousState; GetNextState; GetOutputNode; GetOutputPin"),
			TEXT("editor game thread with schema-owned graph/node handles"),
			TEXT("compile/reinstance invalidates graph, node, pin, and action handles"),
			TEXT("blueprint/package/graph/node preimages required"),
			TEXT("partial structural symbols do not provide a complete allowed lifecycle, action filter, schema entry relink, compile, and reacquire chain"),
			TEXT("named partial AnimGraph symbols compile and link; five exact graph rows retain their row blockers"),
			TEXT("UEAssetDoc.AnimationProvider.Authority"),
			TEXT("AD.ANIMATION.BLOCK.ANIM_GRAPH_LIFECYCLE_AUTHORITY"),
			false,
		},
		{
			TEXT("MONTAGE-STRUCTURAL"),
			TEXT("AssetDocumentAnimation; Engine"),
			TEXT("Private/Provider/Montage/MontageStructuralProvider.h; Animation/AnimMontage.h"),
			TEXT("private typed publication; ENGINE_API public completion"),
			TEXT("MakeAnimationMontageSlotsDescriptor; MakeAnimationMontageSectionsDescriptor; PublishMontageSlots; PublishMontageSections; PublishMontageNotifyEvents"),
			TEXT("Editor game thread, prepared Mutating transaction, verified mutable asset target"),
			TEXT("target lease plus prepared loaded-path pinned animation dependencies; no dynamic animation_N handle arguments"),
			TEXT("existing prepared typed inverse contribution and unique package finalizer"),
			TEXT("Slot length, retained Section/Notify links and public completion share one prepared structural operation"),
			TEXT("source admission projected; this fixture does not claim build or runtime execution"),
			TEXT("UEAssetDoc.AnimationProvider.Descriptors; UEAssetDoc.AnimationProvider.MontageCompletion"),
			TEXT("none"),
			true,
		},
        {
            TEXT("ANIMATION-CURVE-EXACT"),
            TEXT("AssetDocumentAnimation; Engine"),
            TEXT("Private/Provider/Curve/AssetDocumentAnimationFloatCurveExactState.h; Animation/AnimData/AnimDataModel.h; Animation/AnimData/IAnimationDataController.h"),
            TEXT("ANIMATION-FLOAT-CURVE-EXACT-STATE-v1; ENGINE_API public controller"),
            TEXT("PublishExactFloatCurveState; SetCurveAttributes; GetKey; SetDefaultValue; ClearDefaultValue"),
            TEXT("Editor GameThread, exact stock model/controller, prepared mutable Asset transaction"),
            TEXT("verified target/model owner plus complete index-to-handle mapping; no mutable member escapes"),
            TEXT("one forward and one typed inverse caller; full preimage and existing coalesced object/package footprint"),
            TEXT("public SetCurveAttributes notification and complete native readback after bracket close"),
            TEXT("source admission projected; build and runtime validation remain external evidence"),
            TEXT("UEAssetDoc.AnimationProvider.Descriptors"),
            TEXT("none"),
            true,
        },
	};
	return EvidenceRows;
}

#endif
