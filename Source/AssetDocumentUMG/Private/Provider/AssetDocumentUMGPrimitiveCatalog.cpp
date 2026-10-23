// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/AssetDocumentUMGPrimitiveCatalog.h"

namespace AssetDocumentUMGPrimitiveCatalog
{
constexpr uint32 ProviderVersion = 1;

struct FDescriptorSpec
{
	const TCHAR* PrimitiveId;
	const TCHAR* InputSchemaId;
	const TCHAR* OutputSchemaId;
	EAssetDocumentPrimitiveAccess Access;
	const TCHAR* PublicModule;
	const TCHAR* PublicHeader;
	const TCHAR* AuthoritativeApi;
	const TCHAR* VerificationTestId;
	std::initializer_list<const TCHAR*> Diagnostics;
};

FAssetDocumentPrimitiveAdmissionEvidence MakeEvidence(const FDescriptorSpec& Spec)
{
	FAssetDocumentPrimitiveAdmissionEvidence Evidence;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath =
		EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Evidence.ExactEngineVersion = TEXT("5.7.3");
	Evidence.PhysicalOwner = EAssetDocumentPrimitiveOwner::UMG;
	Evidence.PublicModule = Spec.PublicModule;
	Evidence.PublicHeader = Spec.PublicHeader;
	Evidence.ExportedAuthoritativeApi = Spec.AuthoritativeApi;
	Evidence.AuthorityContextId =
		TEXT("umg.editor-game-thread.headless.v1");
	Evidence.PrimaryOperationId = Spec.PrimitiveId;
	Evidence.IdentityLifetimeContractId = TEXT("umg.handle-lifetime.v1");
	Evidence.TransactionContractId =
		TEXT("atomic.active-mutation-token.v1");
	Evidence.RollbackContractId =
		TEXT("rollback.atomic-owner-governed.v1");
	Evidence.CompletionContractId =
		TEXT("umg.direct-provider-local-readback.v1");
	Evidence.SaveReloadEvidenceId = FString(Spec.PrimitiveId)
		+ TEXT(".fresh-reload.required.v1");
	Evidence.FailureInjectionTestIds = {
		FString(Spec.PrimitiveId) + TEXT(".failure-injection.v1")
	};
	Evidence.VerificationTestIds = {Spec.VerificationTestId};
	Evidence.AuthorityFingerprint = FString::Printf(
		TEXT("ue-5.7.3|umg|%s|%s|%s|%s"),
		Spec.PublicModule,
		Spec.PublicHeader,
		Spec.AuthoritativeApi,
		Spec.PrimitiveId);
	return Evidence;
}

FAssetDocumentPrimitiveDescriptor MakeDescriptor(const FDescriptorSpec& Spec)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = Spec.PrimitiveId;
	Descriptor.Version = ProviderVersion;
	Descriptor.ProviderId = TEXT("umg");
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::UMG;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = Spec.Access;
	Descriptor.bRequiresActiveTransaction =
		Spec.Access == EAssetDocumentPrimitiveAccess::Mutating;
	Descriptor.AllowedPhases =
		Descriptor.bRequiresActiveTransaction
			? TArray<EAssetDocumentRuntimeRequestPhase>{
				EAssetDocumentRuntimeRequestPhase::Apply}
			: TArray<EAssetDocumentRuntimeRequestPhase>{
				EAssetDocumentRuntimeRequestPhase::Validate,
				EAssetDocumentRuntimeRequestPhase::Preflight,
				EAssetDocumentRuntimeRequestPhase::Verify};
	Descriptor.InputSchemaId = Spec.InputSchemaId;
	Descriptor.OutputSchemaId = Spec.OutputSchemaId;
	if (Descriptor.bRequiresActiveTransaction)
	{
		Descriptor.MutationFootprintSchemaId =
			FString(Spec.InputSchemaId).Replace(
				TEXT(".input.v1"), TEXT(".footprint.v1"));
	}
	Descriptor.StableDiagnosticCodes = {
		TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"),
		TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
		TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
		TEXT("AD.UMG.PROVIDER.AUTHORITY_UNAVAILABLE"),
		TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"),
		TEXT("AD.UMG.PROVIDER.ROLLBACK_FAILED")
	};
	for (const TCHAR* Diagnostic : Spec.Diagnostics)
	{
		Descriptor.StableDiagnosticCodes.Add(Diagnostic);
	}
	Descriptor.RequiredConsumerIds = {TEXT("umg_profile")};
	Descriptor.Admission = MakeEvidence(Spec);
	return Descriptor;
}

void AddArgument(
	FAssetDocumentPrimitiveDescriptor& Descriptor,
	const TCHAR* Name,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access,
	bool bRequired = true)
{
	Descriptor.AllowedHandleKinds.AddUnique(Kind);
	Descriptor.HandleArgumentSchemas.Add({Name, Kind, Access, bRequired});
}

void AddResult(
	FAssetDocumentPrimitiveDescriptor& Descriptor,
	const TCHAR* Name,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access,
	EAssetDocumentHandleResultCardinality Cardinality)
{
	Descriptor.AllowedHandleKinds.AddUnique(Kind);
	Descriptor.HandleResultSchemas.Add({Name, Kind, Access, Cardinality});
}

FAssetDocumentPrimitiveDescriptor MakeWidgetTreeQuery()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeDescriptor({
		TEXT("umg.widget_tree.query.v1"),
		TEXT("asset_document.umg.widget_tree.query.input.v1"),
		TEXT("asset_document.umg.widget_tree.query.output.v1"),
		EAssetDocumentPrimitiveAccess::ReadOnly,
		TEXT("UMG"),
		TEXT("Blueprint/WidgetTree.h"),
		TEXT("UWidgetTree typed traversal, root, named-slot and child queries"),
		TEXT("UEAssetDoc.UMGProvider.WidgetTreeLifecycle"),
		{TEXT("AD.UMG.WIDGET_TREE.INVALID_BLUEPRINT"),
			TEXT("AD.UMG.WIDGET_TREE.INVALID_TREE")}});
	AddArgument(Descriptor, TEXT("widget_blueprint"),
		EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly);
	AddResult(Descriptor, TEXT("widget"), EAssetDocumentHandleKind::Node,
		EAssetDocumentHandleAccess::ReadOnly,
		EAssetDocumentHandleResultCardinality::ZeroOrMore);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeAddSourceWidget()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeDescriptor({
		TEXT("umg.widget_tree.add_source_widget.v1"),
		TEXT("asset_document.umg.widget_tree.add_source_widget.input.v1"),
		TEXT("asset_document.umg.widget_tree.add_source_widget.output.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		TEXT("Blutility"),
		TEXT("EditorUtilityLibrary.h"),
		TEXT("UEditorUtilityLibrary::FindSourceWidgetByName;UEditorUtilityLibrary::AddSourceWidget"),
		TEXT("UEAssetDoc.UMGProvider.WidgetTreeLifecycle"),
		{TEXT("AD.UMG.WIDGET_TREE.NAME_COLLISION"),
			TEXT("AD.UMG.WIDGET_TREE.INVALID_CLASS"),
			TEXT("AD.UMG.WIDGET_TREE.INVALID_PARENT"),
			TEXT("AD.UMG.WIDGET_TREE.INVALID_INDEX"),
			TEXT("AD.UMG.WIDGET_TREE.ADD_HELPER_FAILED")}});
	AddArgument(Descriptor, TEXT("widget_blueprint"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable);
	AddArgument(Descriptor, TEXT("widget_class"), EAssetDocumentHandleKind::Class,
		EAssetDocumentHandleAccess::ReadOnly);
	AddArgument(Descriptor, TEXT("parent_widget"), EAssetDocumentHandleKind::Node,
		EAssetDocumentHandleAccess::Mutable, false);
	AddResult(Descriptor, TEXT("widget"), EAssetDocumentHandleKind::Node,
		EAssetDocumentHandleAccess::Mutable,
		EAssetDocumentHandleResultCardinality::ExactlyOne);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeDeleteWidget()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeDescriptor({
		TEXT("umg.widget_tree.delete_widget.v1"),
		TEXT("asset_document.umg.widget_tree.delete_widget.input.v1"),
		TEXT("asset_document.umg.widget_tree.delete_widget.output.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		TEXT("UMGEditor"),
		TEXT("WidgetBlueprintEditorUtils.h"),
		TEXT("FWidgetBlueprintEditorUtils::DeleteWidgets"),
		TEXT("UEAssetDoc.UMGProvider.WidgetTreeLifecycle"),
		{TEXT("AD.UMG.WIDGET_TREE.WIDGET_NOT_FOUND"),
			TEXT("AD.UMG.WIDGET_TREE.ROOT_MISMATCH"),
			TEXT("AD.UMG.WIDGET_TREE.DELETE_HELPER_FAILED")}});
	AddArgument(Descriptor, TEXT("widget_blueprint"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable);
	AddArgument(Descriptor, TEXT("widget"), EAssetDocumentHandleKind::Node,
		EAssetDocumentHandleAccess::Mutable);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakePanelAttach()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeDescriptor({
		TEXT("umg.widget_tree.panel_child.attach.v1"),
		TEXT("asset_document.umg.widget_tree.panel_child_attach.input.v1"),
		TEXT("asset_document.umg.widget_tree.panel_child_attach.output.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		TEXT("UMG"),
		TEXT("Components/PanelWidget.h"),
		TEXT("UPanelWidget::AddChild;UPanelWidget::InsertChildAt;UPanelWidget::ShiftChild"),
		TEXT("UEAssetDoc.UMGProvider.WidgetTreeLifecycle"),
		{TEXT("AD.UMG.WIDGET_TREE.INVALID_PANEL"),
			TEXT("AD.UMG.WIDGET_TREE.CHILD_ALREADY_PARENTED"),
			TEXT("AD.UMG.WIDGET_TREE.INVALID_INDEX")}});
	AddArgument(Descriptor, TEXT("widget_blueprint"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable);
	AddArgument(Descriptor, TEXT("parent_widget"), EAssetDocumentHandleKind::Node,
		EAssetDocumentHandleAccess::Mutable);
	AddArgument(Descriptor, TEXT("child_widget"), EAssetDocumentHandleKind::Node,
		EAssetDocumentHandleAccess::Mutable);
	AddResult(Descriptor, TEXT("slot"), EAssetDocumentHandleKind::Subobject,
		EAssetDocumentHandleAccess::Mutable,
		EAssetDocumentHandleResultCardinality::ZeroOrOne);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeContentSet()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeDescriptor({
		TEXT("umg.widget_tree.content.set.v1"),
		TEXT("asset_document.umg.widget_tree.content_set.input.v1"),
		TEXT("asset_document.umg.widget_tree.content_set.output.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		TEXT("UMG"),
		TEXT("Components/ContentWidget.h"),
		TEXT("UContentWidget::SetContent;UContentWidget::GetContent"),
		TEXT("UEAssetDoc.UMGProvider.WidgetTreeLifecycle"),
		{TEXT("AD.UMG.WIDGET_TREE.INVALID_CONTENT_WIDGET"),
			TEXT("AD.UMG.WIDGET_TREE.SELF_CYCLE"),
			TEXT("AD.UMG.WIDGET_TREE.CHILD_ALREADY_PARENTED")}});
	AddArgument(Descriptor, TEXT("widget_blueprint"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable);
	AddArgument(Descriptor, TEXT("parent_widget"), EAssetDocumentHandleKind::Node,
		EAssetDocumentHandleAccess::Mutable);
	AddArgument(Descriptor, TEXT("child_widget"), EAssetDocumentHandleKind::Node,
		EAssetDocumentHandleAccess::Mutable, false);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeNamedSlotSet()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeDescriptor({
		TEXT("umg.widget_tree.named_slot.set.v1"),
		TEXT("asset_document.umg.widget_tree.named_slot_set.input.v1"),
		TEXT("asset_document.umg.widget_tree.named_slot_set.output.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		TEXT("UMG"),
		TEXT("Blueprint/WidgetTree.h"),
		TEXT("UWidgetTree::GetSlotNames;UWidgetTree::GetContentForSlot;UWidgetTree::SetContentForSlot"),
		TEXT("UEAssetDoc.UMGProvider.WidgetTreeLifecycle"),
		{TEXT("AD.UMG.WIDGET_TREE.UNKNOWN_NAMED_SLOT"),
			TEXT("AD.UMG.WIDGET_TREE.CHILD_ALREADY_PARENTED")}});
	AddArgument(Descriptor, TEXT("widget_blueprint"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable);
	AddArgument(Descriptor, TEXT("child_widget"), EAssetDocumentHandleKind::Node,
		EAssetDocumentHandleAccess::Mutable, false);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeDesignerQuery()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeDescriptor({
		TEXT("umg.designer_binding.query.v1"),
		TEXT("asset_document.umg.designer_binding.query.input.v1"),
		TEXT("asset_document.umg.designer_binding.query.output.v1"),
		EAssetDocumentPrimitiveAccess::ReadOnly,
		TEXT("UMGEditor"),
		TEXT("WidgetBlueprint.h"),
		TEXT("UWidgetBlueprint::Bindings read-only traversal"),
		TEXT("UEAssetDoc.UMGProvider.DesignerBindingLifecycle"),
		{TEXT("AD.UMG.DESIGNER_BINDING.DUPLICATE_IDENTITY"),
			TEXT("AD.UMG.DESIGNER_BINDING.EVIDENCE_UNAVAILABLE")}});
	AddArgument(Descriptor, TEXT("widget_blueprint"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeVariableNotify()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeDescriptor({
		TEXT("umg.variable.notify_lifecycle.v1"),
		TEXT("asset_document.umg.variable.notify_lifecycle.input.v1"),
		TEXT("asset_document.umg.variable.notify_lifecycle.output.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		TEXT("UMGEditor"),
		TEXT("WidgetBlueprint.h"),
		TEXT("UWidgetBlueprint::OnVariableAdded;OnVariableRenamed;OnVariableRemoved"),
		TEXT("UEAssetDoc.UMGProvider.VariableGuidIsEngineObserved"),
		{TEXT("AD.UMG.VARIABLE.NAME_COLLISION"),
			TEXT("AD.UMG.VARIABLE.NOT_FOUND"),
			TEXT("AD.UMG.VARIABLE.OBSERVED_GUID_MISMATCH")}});
	AddArgument(Descriptor, TEXT("widget_blueprint"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeAnimationQuery()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeDescriptor({
		TEXT("umg.animation.query.v1"),
		TEXT("asset_document.umg.animation.query.input.v1"),
		TEXT("asset_document.umg.animation.query.output.v1"),
		EAssetDocumentPrimitiveAccess::ReadOnly,
		TEXT("UMGEditor;UMG;MovieScene;MovieSceneTracks"),
		TEXT("WidgetBlueprint.h;Animation/WidgetAnimation.h;MovieScene.h;MovieSceneTrack.h;MovieSceneSection.h;Channels/MovieSceneFloatChannel.h;Tracks/MovieSceneFloatTrack.h;Sections/MovieSceneFloatSection.h;Animation/MovieScene2DTransformTrack.h;Animation/MovieScene2DTransformSection.h"),
		TEXT("UWidgetBlueprint::Animations;UWidgetAnimation and typed MovieScene readback"),
		TEXT("UEAssetDoc.UMGProvider.WidgetAnimationLifecycle"),
		{TEXT("AD.UMG.ANIMATION.NOT_FOUND"),
			TEXT("AD.UMG.ANIMATION.EVIDENCE_UNAVAILABLE")}});
	AddArgument(Descriptor, TEXT("widget_blueprint"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly);
	AddResult(Descriptor, TEXT("widget_animation"),
		EAssetDocumentHandleKind::Subobject,
		EAssetDocumentHandleAccess::ReadOnly,
		EAssetDocumentHandleResultCardinality::ZeroOrMore);
	AddResult(Descriptor, TEXT("movie_scene_track"),
		EAssetDocumentHandleKind::Track,
		EAssetDocumentHandleAccess::ReadOnly,
		EAssetDocumentHandleResultCardinality::ZeroOrMore);
	AddResult(Descriptor, TEXT("movie_scene_section"),
		EAssetDocumentHandleKind::Section,
		EAssetDocumentHandleAccess::ReadOnly,
		EAssetDocumentHandleResultCardinality::ZeroOrMore);
	AddResult(Descriptor, TEXT("movie_scene_channel"),
		EAssetDocumentHandleKind::Channel,
		EAssetDocumentHandleAccess::ReadOnly,
		EAssetDocumentHandleResultCardinality::ZeroOrMore);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakePossessableAdd()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeDescriptor({
		TEXT("umg.movie_scene.possessable.add.v1"),
		TEXT("asset_document.umg.movie_scene.possessable_add.input.v1"),
		TEXT("asset_document.umg.movie_scene.possessable_add.output.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		TEXT("MovieScene"),
		TEXT("MovieScene.h"),
		TEXT("UMovieScene::AddPossessable"),
		TEXT("UEAssetDoc.UMGProvider.AnimationBindingLifecycle"),
		{TEXT("AD.UMG.MOVIE_SCENE.POSSESSABLE_IDENTITY_COLLISION"),
			TEXT("AD.UMG.MOVIE_SCENE.INVALID_CLASS"),
			TEXT("AD.UMG.MOVIE_SCENE.INVALID_RETURNED_GUID")}});
	AddArgument(Descriptor, TEXT("widget_animation"),
		EAssetDocumentHandleKind::Subobject,
		EAssetDocumentHandleAccess::Mutable);
	AddArgument(Descriptor, TEXT("widget_class"), EAssetDocumentHandleKind::Class,
		EAssetDocumentHandleAccess::ReadOnly);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakePossessableRemove()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeDescriptor({
		TEXT("umg.movie_scene.possessable.remove.v1"),
		TEXT("asset_document.umg.movie_scene.possessable_remove.input.v1"),
		TEXT("asset_document.umg.movie_scene.possessable_remove.output.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		TEXT("MovieScene"),
		TEXT("MovieScene.h"),
		TEXT("UMovieScene::RemovePossessable"),
		TEXT("UEAssetDoc.UMGProvider.AnimationBindingLifecycle"),
		{TEXT("AD.UMG.MOVIE_SCENE.POSSESSABLE_NOT_FOUND"),
			TEXT("AD.UMG.MOVIE_SCENE.BINDING_STILL_PRESENT")}});
	AddArgument(Descriptor, TEXT("widget_animation"),
		EAssetDocumentHandleKind::Subobject,
		EAssetDocumentHandleAccess::Mutable);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeSequenceBind(bool bBind)
{
	const TCHAR* PrimitiveId = bBind
		? TEXT("umg.animation.sequence.bind.v1")
		: TEXT("umg.animation.sequence.unbind.v1");
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeDescriptor({
		PrimitiveId,
		bBind
			? TEXT("asset_document.umg.animation.sequence_bind.input.v1")
			: TEXT("asset_document.umg.animation.sequence_unbind.input.v1"),
		bBind
			? TEXT("asset_document.umg.animation.sequence_bind.output.v1")
			: TEXT("asset_document.umg.animation.sequence_unbind.output.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		TEXT("MovieScene;UMG"),
		TEXT("MovieSceneSequence.h;Animation/WidgetAnimation.h"),
		bBind
			? TEXT("UMovieSceneSequence::BindPossessableObject virtual dispatch to UWidgetAnimation")
			: TEXT("UMovieSceneSequence::UnbindPossessableObjects virtual dispatch to UWidgetAnimation"),
		TEXT("UEAssetDoc.UMGProvider.AnimationBindingLifecycle"),
		bBind
			? std::initializer_list<const TCHAR*>{
				TEXT("AD.UMG.ANIMATION_BINDING.GUID_MISMATCH"),
				TEXT("AD.UMG.ANIMATION_BINDING.PREVIEW_CONTEXT_MISMATCH"),
				TEXT("AD.UMG.ANIMATION_BINDING.DUPLICATE_BINDING")}
			: std::initializer_list<const TCHAR*>{
				TEXT("AD.UMG.ANIMATION_BINDING.BINDING_NOT_FOUND"),
				TEXT("AD.UMG.ANIMATION_BINDING.COMPANION_REMAINS")}});
	AddArgument(Descriptor, TEXT("widget_animation"),
		EAssetDocumentHandleKind::Subobject,
		EAssetDocumentHandleAccess::Mutable);
	AddArgument(Descriptor, TEXT("preview_root"),
		EAssetDocumentHandleKind::Scratch,
		EAssetDocumentHandleAccess::Mutable);
	if (bBind)
	{
		AddArgument(Descriptor, TEXT("widget"), EAssetDocumentHandleKind::Node,
			EAssetDocumentHandleAccess::ReadOnly);
	}
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeTrackAdd(bool bTransform)
{
	const TCHAR* PrimitiveId = bTransform
		? TEXT("umg.movie_scene.transform_track.add.v1")
		: TEXT("umg.movie_scene.float_track.add.v1");
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeDescriptor({
		PrimitiveId,
		bTransform
			? TEXT("asset_document.umg.movie_scene.transform_track_add.input.v1")
			: TEXT("asset_document.umg.movie_scene.float_track_add.input.v1"),
		bTransform
			? TEXT("asset_document.umg.movie_scene.transform_track_add.output.v1")
			: TEXT("asset_document.umg.movie_scene.float_track_add.output.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		bTransform ? TEXT("MovieScene;MovieSceneTracks;UMG")
			: TEXT("MovieScene;MovieSceneTracks"),
		bTransform
			? TEXT("MovieScene.h;Tracks/MovieScenePropertyTrack.h;MovieSceneTrack.h;MovieSceneSection.h;Channels/MovieSceneFloatChannel.h;Channels/MovieSceneChannelProxy.h;Animation/MovieScene2DTransformTrack.h;Animation/MovieScene2DTransformSection.h")
			: TEXT("MovieScene.h;Tracks/MovieScenePropertyTrack.h;MovieSceneTrack.h;Sections/MovieSceneFloatSection.h;Channels/MovieSceneFloatChannel.h"),
		bTransform
			? TEXT("public 2D transform track, section, metadata channel, mask and float channel chain")
			: TEXT("public float track, section and float channel chain"),
		TEXT("UEAssetDoc.UMGProvider.MovieSceneLifecycle"),
		bTransform
			? std::initializer_list<const TCHAR*>{
				TEXT("AD.UMG.MOVIE_SCENE.TRACK_IDENTITY_COLLISION"),
				TEXT("AD.UMG.MOVIE_SCENE.INVALID_RANGE"),
				TEXT("AD.UMG.MOVIE_SCENE.DUPLICATE_KEY_FRAME"),
				TEXT("AD.UMG.MOVIE_SCENE.UNSUPPORTED_INTERPOLATION"),
				TEXT("AD.UMG.MOVIE_SCENE.UNSUPPORTED_CHANNEL")}
			: std::initializer_list<const TCHAR*>{
				TEXT("AD.UMG.MOVIE_SCENE.TRACK_IDENTITY_COLLISION"),
				TEXT("AD.UMG.MOVIE_SCENE.INVALID_RANGE"),
				TEXT("AD.UMG.MOVIE_SCENE.DUPLICATE_KEY_FRAME"),
				TEXT("AD.UMG.MOVIE_SCENE.UNSUPPORTED_INTERPOLATION")}});
	AddArgument(Descriptor, TEXT("widget_animation"),
		EAssetDocumentHandleKind::Subobject,
		EAssetDocumentHandleAccess::Mutable);
	AddResult(Descriptor, TEXT("movie_scene_track"),
		EAssetDocumentHandleKind::Track,
		EAssetDocumentHandleAccess::Mutable,
		EAssetDocumentHandleResultCardinality::ExactlyOne);
	AddResult(Descriptor, TEXT("movie_scene_section"),
		EAssetDocumentHandleKind::Section,
		EAssetDocumentHandleAccess::Mutable,
		EAssetDocumentHandleResultCardinality::ExactlyOne);
	AddResult(Descriptor, TEXT("movie_scene_channel"),
		EAssetDocumentHandleKind::Channel,
		EAssetDocumentHandleAccess::Mutable,
		bTransform
			? EAssetDocumentHandleResultCardinality::OneOrMore
			: EAssetDocumentHandleResultCardinality::ExactlyOne);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeTrackSectionRemove()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeDescriptor({
		TEXT("umg.movie_scene.track_section.remove.v1"),
		TEXT("asset_document.umg.movie_scene.track_section_remove.input.v1"),
		TEXT("asset_document.umg.movie_scene.track_section_remove.output.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		TEXT("MovieScene"),
		TEXT("MovieScene.h;MovieSceneTrack.h"),
		TEXT("UMovieScene::RemoveTrack;UMovieSceneTrack::RemoveSection"),
		TEXT("UEAssetDoc.UMGProvider.MovieSceneLifecycle"),
		{TEXT("AD.UMG.MOVIE_SCENE.OWNERSHIP_MISMATCH"),
			TEXT("AD.UMG.MOVIE_SCENE.TARGET_NOT_FOUND")}});
	AddArgument(Descriptor, TEXT("widget_animation"),
		EAssetDocumentHandleKind::Subobject,
		EAssetDocumentHandleAccess::Mutable);
	AddArgument(Descriptor, TEXT("movie_scene_track"),
		EAssetDocumentHandleKind::Track,
		EAssetDocumentHandleAccess::Mutable);
	AddArgument(Descriptor, TEXT("movie_scene_section"),
		EAssetDocumentHandleKind::Section,
		EAssetDocumentHandleAccess::Mutable, false);
	return Descriptor;
}

void AddOrdinary(
	TArray<FAssetDocumentUMGDecisionRow>& Rows,
	const TCHAR* DecisionId)
{
	Rows.Add({
		DecisionId,
		ProviderVersion,
		EAssetDocumentPrimitiveOwner::UMG,
		EAssetDocumentUMGDecisionDisposition::Ordinary,
		{TEXT("umg_profile")},
		TEXT("")});
}

void AddBlocker(
	TArray<FAssetDocumentUMGDecisionRow>& Rows,
	const TCHAR* DecisionId,
	const TCHAR* Diagnostic)
{
	Rows.Add({
		DecisionId,
		ProviderVersion,
		EAssetDocumentPrimitiveOwner::UMG,
		EAssetDocumentUMGDecisionDisposition::StableBlocker,
		{TEXT("umg_provider_design")},
		Diagnostic});
}
} // namespace AssetDocumentUMGPrimitiveCatalog

TArray<FAssetDocumentUMGDecisionRow>
BuildAssetDocumentUMGDecisionProjection()
{
	using namespace AssetDocumentUMGPrimitiveCatalog;
	TArray<FAssetDocumentUMGDecisionRow> Rows;
	Rows.Reserve(20);
	AddOrdinary(Rows, TEXT("umg.widget_tree.query.v1"));
	AddOrdinary(Rows, TEXT("umg.widget_tree.add_source_widget.v1"));
	AddOrdinary(Rows, TEXT("umg.widget_tree.delete_widget.v1"));
	AddOrdinary(Rows, TEXT("umg.widget_tree.panel_child.attach.v1"));
	AddOrdinary(Rows, TEXT("umg.widget_tree.content.set.v1"));
	AddOrdinary(Rows, TEXT("umg.widget_tree.named_slot.set.v1"));
	AddBlocker(Rows, TEXT("umg.widget_tree.replace.v1"),
		TEXT("AD.BLOCK.UMG.WIDGET_REPLACE_HEADLESS_AUTHORITY"));
	AddOrdinary(Rows, TEXT("umg.designer_binding.query.v1"));
	AddBlocker(Rows, TEXT("UMG-DESIGNER-BINDING-APPLY-DELTA-v1"),
		TEXT("AD.BLOCK.UMG.DESIGNER_BINDING_EVIDENCE"));
	AddOrdinary(Rows, TEXT("umg.variable.notify_lifecycle.v1"));
	AddBlocker(Rows, TEXT("UMG.VAR.FORCE_GUID_V1"),
		TEXT("AD.BLOCK.UMG.CALLER_SELECTED_GUID_UNSUPPORTED"));
	AddOrdinary(Rows, TEXT("umg.animation.query.v1"));
	AddBlocker(Rows, TEXT("UMG-WIDGET-ANIMATION-DELETE-v1"),
		TEXT("AD.BLOCK.UMG.ANIMATION_DELETE_EVIDENCE"));
	AddOrdinary(Rows, TEXT("umg.movie_scene.possessable.add.v1"));
	AddOrdinary(Rows, TEXT("umg.movie_scene.possessable.remove.v1"));
	AddOrdinary(Rows, TEXT("umg.animation.sequence.bind.v1"));
	AddOrdinary(Rows, TEXT("umg.animation.sequence.unbind.v1"));
	AddOrdinary(Rows, TEXT("umg.movie_scene.float_track.add.v1"));
	AddOrdinary(Rows, TEXT("umg.movie_scene.transform_track.add.v1"));
	AddOrdinary(Rows, TEXT("umg.movie_scene.track_section.remove.v1"));
	return Rows;
}

TArray<FAssetDocumentPrimitiveDescriptor>
BuildAssetDocumentUMGPrimitiveCatalog()
{
	using namespace AssetDocumentUMGPrimitiveCatalog;
	return {
		MakeWidgetTreeQuery(),
		MakeAddSourceWidget(),
		MakeDeleteWidget(),
		MakePanelAttach(),
		MakeContentSet(),
		MakeNamedSlotSet(),
		MakeDesignerQuery(),
		MakeVariableNotify(),
		MakeAnimationQuery(),
		MakePossessableAdd(),
		MakePossessableRemove(),
		MakeSequenceBind(true),
		MakeSequenceBind(false),
		MakeTrackAdd(false),
		MakeTrackAdd(true),
		MakeTrackSectionRemove()
	};
}

TConstArrayView<FAssetDocumentUMGDecisionRow>
GetAssetDocumentUMGDecisionProjection()
{
	static const TArray<FAssetDocumentUMGDecisionRow> Rows =
		BuildAssetDocumentUMGDecisionProjection();
	return Rows;
}

TConstArrayView<FAssetDocumentPrimitiveDescriptor>
GetAssetDocumentUMGPrimitiveDescriptors()
{
	static const TArray<FAssetDocumentPrimitiveDescriptor> Descriptors =
		BuildAssetDocumentUMGPrimitiveCatalog();
	return Descriptors;
}

const FAssetDocumentUMGDecisionRow*
FindAssetDocumentUMGDecision(FStringView DecisionId, uint32 Version)
{
	for (const FAssetDocumentUMGDecisionRow& Row :
		GetAssetDocumentUMGDecisionProjection())
	{
		if (Row.DecisionId == DecisionId && Row.Version == Version)
		{
			return &Row;
		}
	}
	return nullptr;
}

const FAssetDocumentUMGDecisionRow*
FindAssetDocumentUMGStableBlocker(FStringView DecisionId, uint32 Version)
{
	const FAssetDocumentUMGDecisionRow* Row =
		FindAssetDocumentUMGDecision(DecisionId, Version);
	return Row != nullptr
		&& Row->Disposition == EAssetDocumentUMGDecisionDisposition::StableBlocker
		? Row
		: nullptr;
}

bool IsAssetDocumentUMGDecisionProjectionValid()
{
	TSet<FString> Identities;
	int32 OrdinaryCount = 0;
	int32 BlockerCount = 0;
	for (const FAssetDocumentUMGDecisionRow& Row :
		GetAssetDocumentUMGDecisionProjection())
	{
		const FString Identity = FString::Printf(
			TEXT("%s|%u"), *Row.DecisionId, Row.Version);
		if (Identities.Contains(Identity)
			|| Row.Owner != EAssetDocumentPrimitiveOwner::UMG
			|| Row.ConsumerIds.Num() != 1)
		{
			return false;
		}
		Identities.Add(Identity);
		if (Row.Disposition == EAssetDocumentUMGDecisionDisposition::Ordinary)
		{
			++OrdinaryCount;
			if (Row.ConsumerIds[0] != TEXT("umg_profile")
				|| !Row.StableBlockerDiagnostic.IsEmpty())
			{
				return false;
			}
		}
		else
		{
			++BlockerCount;
			if (Row.ConsumerIds[0] != TEXT("umg_provider_design")
				|| Row.StableBlockerDiagnostic.IsEmpty())
			{
				return false;
			}
		}
	}
	return OrdinaryCount == 16 && BlockerCount == 4;
}
