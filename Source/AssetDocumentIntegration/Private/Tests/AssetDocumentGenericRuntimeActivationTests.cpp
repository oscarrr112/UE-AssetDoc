// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentAIPrimitiveProvider.h"
#include "AssetDocumentAnimationRequestBinding.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentGraphProvider.h"
#include "AssetDocumentWidgetProvider.h"
#include "AssetDocumentDataProvider.h"
#include "AssetDocumentAnimationAssetProvider.h"
#include "AssetDocumentNiagaraProvider.h"
#include "AssetDocumentAudioProvider.h"
#include "AssetDocumentRigProvider.h"
#include "AssetDocumentPhysicsProvider.h"
#include "AssetDocumentSequencerProvider.h"
#include "AssetDocumentGenericRuntimeBootstrap.h"
#include "AssetDocumentRequiredPrimitiveProviderView.h"
#include "AssetDocumentGenericRuntimeRoute.h"
#include "AssetDocumentGenericRequestAdapter.h"
#include "AssetDocumentGenericReadOnlyDispatcher.h"

#include "../../../AssetDocument/Private/AssetDocumentIngressQueue.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentPythonRuntime.h"
#include "AssetDocumentRuntimeModule.h"
#include "AssetDocumentService.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"

#include <type_traits>
#include "AssetDocumentPluginPaths.h"

#if WITH_DEV_AUTOMATION_TESTS
FAssetDocumentGenericRuntimeBootstrap*
GetAssetDocumentActiveGenericRuntimeBootstrapForTests();

uint64 GetAssetDocumentActiveGenericRuntimeRevisionForTests();

int32 GetAssetDocumentGenericRequirementCountForTests();
TArray<FAssetDocumentPrimitiveRequirement> GetAssetDocumentGenericRequirementsForTests();

namespace
{
using namespace UE::AssetDocument::RuntimeTypes::Private;

const FAssetDocumentRuntimeValueDto* FindObjectField(
	const FAssetDocumentRuntimeValueDto& Object,
	const FString& Key)
{
	const int32 Index = Object.ObjectKeys.IndexOfByKey(Key);
	return Object.Kind == EAssetDocumentRuntimeValueKind::Object
		&& Object.Elements.IsValidIndex(Index)
		? &Object.Elements[Index]
		: nullptr;
}

class FReadOnlyContext final : public IAssetDocumentNativeRequestContext
{
public:
	explicit FReadOnlyContext(
		const FAssetDocumentRequestContextMetadata& InMetadata)
		: Metadata(InMetadata)
	{
	}

	const FAssetDocumentRequestContextMetadata& GetMetadata() const override
	{
		return Metadata;
	}

	FAssetDocumentPrimitiveOutcome Invoke(
		const FAssetDocumentPrimitiveInvocation& Invocation) override
	{
		(void)Invocation;
		return {};
	}

	FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override
	{
		return {};
	}

private:
	FAssetDocumentRequestContextMetadata Metadata;
};

class FReadOnlyHost final : public IAssetDocumentPythonRuntimeHost
{
public:
	FAssetDocumentRuntimeResultDto Dispatch(
		const FAssetDocumentRuntimeRequestDto& Request,
		IAssetDocumentNativeRequestContext& Context) override
	{
		++DispatchCount;
		ObservedPhase = Context.GetMetadata().Phase;
		FAssetDocumentRuntimeResultDto Result;
		Result.RequestId = Request.RequestId;
		Result.TerminalStatus = EAssetDocumentRuntimeTerminalStatus::Succeeded;
		Result.FinalPhase = Context.GetMetadata().Phase;
		Result.RuntimeRevision = Request.ExpectedRuntimeRevision;
		Result.CanonicalValue = Request.CanonicalDocument;
		Result.CanonicalBytes = {1, 2, 3};
		Result.SemanticHash = TEXT("python-semantic-hash");
		Result.CodecVersion = 1;
		return Result;
	}

	int32 DispatchCount = 0;
	EAssetDocumentRuntimeRequestPhase ObservedPhase =
		EAssetDocumentRuntimeRequestPhase::Failed;
};

class FReadOnlyRuntime final : public IAssetDocumentPythonRuntime
{
public:
	FAssetDocumentContractValidationResult InitializePythonBinding() override
	{
		return {};
	}

	FAssetDocumentPythonRevisionActivationResult TryActivateRevision(
		const FAssetDocumentPythonRevisionCandidate& Candidate) override
	{
		(void)Candidate;
		return {};
	}

	uint64 GetActiveRuntimeRevision() const override { return 17; }
	bool HasActiveRequests() const override { return false; }
	IAssetDocumentPythonRuntimeHost& GetHost() override { return Host; }

	const IAssetDocumentPrimitiveRegistry* GetActivePrimitiveRegistry()
		const override
	{
		return nullptr;
	}

	FAssetDocumentPythonRequestContextCreateResult CreateRequestContext(
		const FAssetDocumentRequestContextMetadata& Metadata,
		const FGuid& TransactionId,
		IAssetDocumentAtomicTransaction* ActiveTransaction) override
	{
		++ContextCreateCount;
		ObservedMetadata = Metadata;
		bObservedValidTransaction = TransactionId.IsValid();
		bObservedActiveTransaction = ActiveTransaction != nullptr;
		FAssetDocumentPythonRequestContextCreateResult Result;
		Result.Context = MakeUnique<FReadOnlyContext>(Metadata);
		return Result;
	}

	FAssetDocumentHandleInvalidationResult InvalidateHandles(
		const FAssetDocumentHandleInvalidation& Event) override
	{
		(void)Event;
		++HandleInvalidationCount;
		return {};
	}

	void ShutdownPythonBinding() override {}

	FReadOnlyHost Host;
	FAssetDocumentRequestContextMetadata ObservedMetadata;
	int32 ContextCreateCount = 0;
	int32 HandleInvalidationCount = 0;
	bool bObservedValidTransaction = false;
	bool bObservedActiveTransaction = false;
};

// One-off parity snapshot of the 48 tuples the bootstrap hard-coded before the
// required set was derived from Python. The selector batch resolver moved from the
// auxiliary ai_blackboard_binding_repair consumer to the BehaviorTree route.
TArray<FString> RetiredHardCodedRequirementTuples()
{
	TArray<FString> Rows;
	auto Add = [&Rows](const TCHAR* Route, uint32 Version, std::initializer_list<const TCHAR*> Ids)
	{
		for (const TCHAR* Id : Ids) { Rows.Add(FString::Printf(TEXT("%s|%u|%s"), Id, Version, Route)); }
	};
	Add(TEXT("route.generic.v1"), 1, {TEXT("editor.class.resolve_canonical.v1"), TEXT("editor.fragment.struct_codec.v1"),
		TEXT("editor.property.enumerate_admitted.v1"), TEXT("editor.property.enumerate_admitted_batch.v1"),
		TEXT("editor.property.preflight_admitted.v1"), TEXT("editor.property.read_admitted.v1"),
		TEXT("editor.property.write_admitted_batch.v1"), TEXT("editor.reference.resolve_asset.v1"),
		TEXT("editor.scratch.create_by_factory.v1"),
		TEXT("editor.asset.resolve_existing.v1"), TEXT("editor.asset.create_by_factory.v1"),
		TEXT("editor.property.inspect_evidence.v1"), TEXT("ai.behavior_tree.selector_inspect_evidence.v1"),
		TEXT("editor.enhanced_input.check_domain.v1")});
	// Generic factory lifecycle (factory_generic.v1) replaced the DataAsset-only scratches.
	Add(TEXT("route.generic.v1"), 2, {TEXT("editor.scratch.clone_asset.v2"), TEXT("editor.asset.create_by_factory.v2")});
	// Validate's trial apply (every snapshot-covered Profile edits a transient copy) is a generic-route need.
	Add(TEXT("route.generic.v1"), 1, {TEXT("editor.scratch.trial_target.v1")});
	Add(TEXT("ai_profile"), 2, {TEXT("ai.blackboard.entry.readback.v2"), TEXT("ai.blackboard.parent.readback.v2"),
		TEXT("ai.blackboard.local_keys.readback.v2"), TEXT("ai.blackboard.entry.update_existing.v2"),
		TEXT("ai.blackboard.parent.set.v2"), TEXT("ai.blackboard.entry.add.v2"),
		TEXT("ai.blackboard.entry.replace_class.v2"), TEXT("ai.blackboard.entry.remove.v2"),
		TEXT("ai.blackboard.local_keys.reorder.v2")});
	// AnimMontage shares the generic factory lifecycle (factory_generic.v1) under its own route.
	Add(TEXT("animation_profile"), 1, {TEXT("editor.class.resolve_canonical.v1"), TEXT("editor.asset.resolve_existing.v1"),
		TEXT("editor.scratch.create_by_factory.v1"), TEXT("editor.asset.rollback_created.v1"),
		TEXT("editor.property.enumerate_admitted.v1"),
		TEXT("animation.montage.profile.snapshot.v1"), TEXT("animation.asset.preview_mesh.set.v1"),
		TEXT("animation.asset.skeleton.set.v1"), TEXT("animation.montage.slot_tracks.replace.v1"),
		TEXT("animation.montage.sections.mutate.v1"), TEXT("animation.montage.notifies.replace.v1"),
		TEXT("animation.montage.notify_states.replace.v1")});
	Add(TEXT("animation_profile"), 2, {TEXT("animation.curve.float.replace.v2"),
		TEXT("editor.scratch.clone_asset.v2"), TEXT("editor.asset.create_by_factory.v2")});
	// Added by the Python Montage Profile after the retirement (settings + Properties restore).
	Add(TEXT("animation_profile"), 1, {TEXT("editor.property.enumerate_admitted_batch.v1"),
		TEXT("editor.property.write_admitted_batch.v1"), TEXT("animation.montage.settings.set.v1"),
		TEXT("animation.montage.structure.check.v1")});
	// AnimSequence Python Profile (profile.anim_sequence.v1): authoring snapshot + snapshot-covered edit.
	Add(TEXT("animation_profile"), 1, {TEXT("animation.asset.authoring.snapshot.v1"), TEXT("animation.asset.authoring.edit.v1")});
	Add(TEXT("ai_profile"), 1, {TEXT("ai.behavior_tree.blackboard.readback.v1"), TEXT("ai.behavior_tree.binding.inspect.v1"),
		TEXT("ai.behavior_tree.selector.inventory.v1"), TEXT("ai.behavior_tree.graph.create.v1"),
		TEXT("ai.behavior_tree.graph.author_scratch.v1"), TEXT("ai.behavior_tree.graph.install.v1"),
		TEXT("ai.behavior_tree.blackboard.set.v1")});
	Add(TEXT("ai_profile"), 2, {TEXT("ai.behavior_tree.authored_graph.readback.v2"), TEXT("ai.behavior_tree.node_class.inspect.v2"),
		TEXT("ai.behavior_tree.selector.resolve_batch.v2")});
	Add(TEXT("ai_profile"), 3, {TEXT("ai.behavior_tree.value_or_key.refresh.v3")});
	// UBlueprint Python Profile (profile.ublueprint.v2): lifecycle, Blueprint members and K2 graphs;
	// AnimBlueprint (profile.anim_blueprint.v1) shares the route and adds the settings write.
	Add(TEXT("blueprint_profile"), 1, {TEXT("editor.class.resolve_canonical.v1"), TEXT("editor.asset.resolve_existing.v1"),
		TEXT("editor.asset.rollback_created.v1"), TEXT("editor.blueprint.asset.create.v1"),
		TEXT("editor.blueprint.scratch.clone.v1"), TEXT("editor.blueprint.scratch.create.v1"),
		TEXT("blueprint.member.query.v1"), TEXT("blueprint.member.parent.set.v1"),
		TEXT("blueprint.member.interface.add.v1"), TEXT("blueprint.member.interface.remove.v1"),
		TEXT("blueprint.member.variable.set.v1"), TEXT("blueprint.member.variable.remove.v1"),
		TEXT("blueprint.member.component.add.v1"), TEXT("blueprint.member.component.remove.v1"),
		TEXT("blueprint.member.component.attach.v1"), TEXT("blueprint.member.property.set.v1"),
		TEXT("blueprint.member.setting.set.v1"),
		TEXT("blueprint.member.dispatcher.set.v1"), TEXT("blueprint.member.dispatcher.remove.v1"),
		TEXT("blueprint.member.timeline.set.v1"), TEXT("blueprint.member.timeline.remove.v1"), TEXT("blueprint.member.rename.v1"),
		TEXT("graph.list.v1"), TEXT("graph.query.v1"), TEXT("graph.actions.enumerate.v1"), TEXT("graph.create.v1"),
		TEXT("graph.remove.v1"), TEXT("graph.rename.v1"), TEXT("graph.finalize.v1"), TEXT("graph.function.flags.set.v1"),
		TEXT("graph.node.spawn.v1"), TEXT("graph.node.remove.v1"), TEXT("graph.node.rename.v1"),
		TEXT("graph.node.layout.set.v1"), TEXT("graph.node.property.get.v1"), TEXT("graph.node.property.set.v1"),
		TEXT("graph.node.user_pins.set.v1"), TEXT("graph.node.dynamic_pin.add.v1"), TEXT("graph.node.dynamic_pin.remove.v1"),
		TEXT("graph.node.pin_type.set.v1"), TEXT("graph.link.check.v1"),
		TEXT("graph.pin.connect.v1"), TEXT("graph.pin.break.v1"), TEXT("graph.pin.default.set.v1")});
	// WidgetBlueprint Python Profile (profile.widget_blueprint.v1): the umg Designer / animation primitives.
	Add(TEXT("blueprint_profile"), 1, {TEXT("umg.tree.query.v1"), TEXT("umg.tree.add.v1"), TEXT("umg.tree.move.v1"),
		TEXT("umg.tree.remove.v1"), TEXT("umg.widget.rename.v1"), TEXT("umg.widget.variable.set.v1"), TEXT("umg.widget.property.set.v1"),
		TEXT("umg.widget.navigation.set.v1"), TEXT("umg.binding.set.v1"), TEXT("umg.binding.remove.v1"),
		TEXT("umg.desired_focus.set.v1"), TEXT("umg.palette_category.set.v1"), TEXT("umg.design_size.set.v1"),
		TEXT("umg.anim.query.v1"), TEXT("umg.anim.set.v1"), TEXT("umg.anim.remove.v1"),
		TEXT("umg.anim.track.set.v1"), TEXT("umg.anim.track.remove.v1")});
	// DataTable family, MaterialInstanceConstant and Material graph Python Profiles (data_profile): the "data" provider and the generic factory lifecycle.
	Add(TEXT("data_profile"), 1, {TEXT("data.data_table.read.v1"), TEXT("data.data_table.edit.v1"),
		TEXT("data.curve_table.read.v1"), TEXT("data.curve_table.edit.v1"),
		TEXT("data.string_table.read.v1"), TEXT("data.string_table.edit.v1"),
		TEXT("data.user_struct.read.v1"), TEXT("data.user_struct.edit.v1"),
		TEXT("data.user_enum.read.v1"), TEXT("data.user_enum.edit.v1"),
		TEXT("data.material_instance.read.v1"), TEXT("data.material_instance.edit.v1"),
		TEXT("data.material.read.v1"), TEXT("data.material.edit.v1"), TEXT("data.material.compile.v1"),
		TEXT("editor.class.resolve_canonical.v1"), TEXT("editor.asset.resolve_existing.v1"),
		TEXT("editor.reference.resolve_asset.v1"), TEXT("editor.scratch.create_by_factory.v1"),
		TEXT("editor.asset.rollback_created.v1")});
	Add(TEXT("data_profile"), 2, {TEXT("editor.scratch.clone_asset.v2"), TEXT("editor.asset.create_by_factory.v2")});
	// Skeleton / BlendSpace / PoseAsset / AnimComposite Python Profiles (animation_asset_profile): the
	// "animation_asset" provider and the generic factory lifecycle. The Montage Profile reads the skeleton's slots.
	Add(TEXT("animation_asset_profile"), 1, {TEXT("animation_asset.skeleton.read.v1"), TEXT("animation_asset.skeleton.edit.v1"),
		TEXT("animation_asset.blend_space.read.v1"), TEXT("animation_asset.blend_space.edit.v1"),
		TEXT("animation_asset.pose_asset.read.v1"), TEXT("animation_asset.pose_asset.edit.v1"),
		TEXT("animation_asset.composite.read.v1"), TEXT("animation_asset.composite.edit.v1"),
		TEXT("editor.class.resolve_canonical.v1"), TEXT("editor.asset.resolve_existing.v1"),
		TEXT("editor.reference.resolve_asset.v1"), TEXT("editor.scratch.create_by_factory.v1"),
		TEXT("editor.asset.rollback_created.v1")});
	Add(TEXT("animation_asset_profile"), 2, {TEXT("editor.scratch.clone_asset.v2"), TEXT("editor.asset.create_by_factory.v2")});
	Add(TEXT("animation_profile"), 1, {TEXT("animation_asset.skeleton.read.v1")});
	// StateTree Python Profile (state_tree_profile): the ai.state_tree.* rows of the AI provider and the generic factory lifecycle.
	Add(TEXT("state_tree_profile"), 1, {TEXT("ai.state_tree.read.v1"), TEXT("ai.state_tree.edit.v1"), TEXT("ai.state_tree.compile.v1"),
		TEXT("editor.class.resolve_canonical.v1"), TEXT("editor.asset.resolve_existing.v1"),
		TEXT("editor.reference.resolve_asset.v1"), TEXT("editor.scratch.create_by_factory.v1"),
		TEXT("editor.asset.rollback_created.v1")});
	Add(TEXT("state_tree_profile"), 2, {TEXT("editor.scratch.clone_asset.v2"), TEXT("editor.asset.create_by_factory.v2")});
	// Environment Query Python Profile (env_query_profile): the ai.env_query.* rows of the AI provider and the generic
	// factory lifecycle.
	Add(TEXT("env_query_profile"), 1, {TEXT("ai.env_query.read.v1"), TEXT("ai.env_query.edit.v1"), TEXT("ai.env_query.finalize.v1"),
		TEXT("editor.class.resolve_canonical.v1"), TEXT("editor.asset.resolve_existing.v1"),
		TEXT("editor.scratch.create_by_factory.v1"), TEXT("editor.asset.rollback_created.v1")});
	Add(TEXT("env_query_profile"), 2, {TEXT("editor.scratch.clone_asset.v2"), TEXT("editor.asset.create_by_factory.v2")});
	// Niagara Python Profiles (fx_profile): the "fx" provider, the generic factory lifecycle and (script graphs and
	// scratch pads) the shared graph primitives.
	Add(TEXT("fx_profile"), 1, {TEXT("fx.niagara.read.v1"), TEXT("fx.niagara.edit.v1"), TEXT("fx.niagara.compile.v1"),
		TEXT("editor.class.resolve_canonical.v1"), TEXT("editor.asset.resolve_existing.v1"),
		TEXT("editor.scratch.create_by_factory.v1"), TEXT("editor.asset.rollback_created.v1"),
		TEXT("graph.list.v1"), TEXT("graph.query.v1"), TEXT("graph.actions.enumerate.v1"), TEXT("graph.create.v1"),
		TEXT("graph.remove.v1"), TEXT("graph.rename.v1"), TEXT("graph.finalize.v1"), TEXT("graph.function.flags.set.v1"),
		TEXT("graph.node.spawn.v1"), TEXT("graph.node.remove.v1"), TEXT("graph.node.rename.v1"),
		TEXT("graph.node.layout.set.v1"), TEXT("graph.node.property.get.v1"), TEXT("graph.node.property.set.v1"),
		TEXT("graph.node.user_pins.set.v1"), TEXT("graph.node.dynamic_pin.add.v1"), TEXT("graph.node.dynamic_pin.remove.v1"),
		TEXT("graph.node.pin_type.set.v1"),
		TEXT("graph.pin.connect.v1"), TEXT("graph.pin.break.v1"), TEXT("graph.pin.default.set.v1")});
	Add(TEXT("fx_profile"), 2, {TEXT("editor.scratch.clone_asset.v2"), TEXT("editor.asset.create_by_factory.v2")});
	// MetaSound Source / Patch Python Profiles (audio_profile): the "audio" provider reads, graph edit and compile,
	// the preset parent resolver for the factory context and the generic factory lifecycle. SoundCue Python Profile
	// (audio_profile): audio.soundcue.read.v1 and the shared graph primitives.
	Add(TEXT("audio_profile"), 1, {TEXT("audio.metasound.read.v1"), TEXT("audio.metasound.catalog.v1"),
		TEXT("audio.metasound.edit.v1"), TEXT("audio.metasound.compile.v1"), TEXT("audio.soundcue.read.v1"),
		TEXT("editor.class.resolve_canonical.v1"), TEXT("editor.asset.resolve_existing.v1"),
		TEXT("editor.reference.resolve_asset.v1"), TEXT("editor.scratch.create_by_factory.v1"),
		TEXT("editor.asset.rollback_created.v1"),
		TEXT("graph.list.v1"), TEXT("graph.query.v1"), TEXT("graph.actions.enumerate.v1"), TEXT("graph.create.v1"),
		TEXT("graph.remove.v1"), TEXT("graph.rename.v1"), TEXT("graph.finalize.v1"), TEXT("graph.function.flags.set.v1"),
		TEXT("graph.node.spawn.v1"), TEXT("graph.node.remove.v1"), TEXT("graph.node.rename.v1"),
		TEXT("graph.node.layout.set.v1"), TEXT("graph.node.property.get.v1"), TEXT("graph.node.property.set.v1"),
		TEXT("graph.node.user_pins.set.v1"), TEXT("graph.node.dynamic_pin.add.v1"), TEXT("graph.node.dynamic_pin.remove.v1"),
		TEXT("graph.node.pin_type.set.v1"),
		TEXT("graph.pin.connect.v1"), TEXT("graph.pin.break.v1"), TEXT("graph.pin.default.set.v1")});
	Add(TEXT("audio_profile"), 2, {TEXT("editor.scratch.clone_asset.v2"), TEXT("editor.asset.create_by_factory.v2")});
	// IK Rig / IK Retargeter / Control Rig Python Profiles (rig_profile): the "rig" provider reads, edits, status,
	// catalogs and suggestions and the generic factory lifecycle.
	Add(TEXT("rig_profile"), 1, {TEXT("rig.ikrig.read.v1"), TEXT("rig.ikrig.edit.v1"), TEXT("rig.ikretargeter.read.v1"),
		TEXT("rig.ikretargeter.edit.v1"), TEXT("rig.status.v1"), TEXT("rig.catalog.v1"), TEXT("rig.suggest.v1"),
		TEXT("rig.controlrig.read.v1"), TEXT("rig.controlrig.catalog.v1"), TEXT("rig.controlrig.edit.v1"),
		TEXT("rig.controlrig.compile.v1"),
		TEXT("editor.class.resolve_canonical.v1"), TEXT("editor.asset.resolve_existing.v1"), TEXT("editor.reference.resolve_asset.v1"),
		TEXT("editor.scratch.create_by_factory.v1"), TEXT("editor.asset.rollback_created.v1")});
	Add(TEXT("rig_profile"), 2, {TEXT("editor.scratch.clone_asset.v2"), TEXT("editor.asset.create_by_factory.v2")});
	// Physics Asset Python Profile (physics_profile): the "physics" provider read, edit and status and the generic
	// factory lifecycle (the skeletal mesh as factory context, resolved as a reference).
	Add(TEXT("physics_profile"), 1, {TEXT("physics.asset.read.v1"), TEXT("physics.asset.edit.v1"), TEXT("physics.status.v1"),
		TEXT("editor.class.resolve_canonical.v1"), TEXT("editor.asset.resolve_existing.v1"), TEXT("editor.reference.resolve_asset.v1"),
		TEXT("editor.scratch.create_by_factory.v1"), TEXT("editor.asset.rollback_created.v1")});
	Add(TEXT("physics_profile"), 2, {TEXT("editor.scratch.clone_asset.v2"), TEXT("editor.asset.create_by_factory.v2")});
	// Level / Template Sequence Python Profiles (seq_profile): the "seq" provider read, inspect, catalog, edit and status,
	// the generic factory lifecycle (Create and Update; a Template Sequence's factory takes its bound actor class)
	// and the Director Blueprint's members and graphs (the hosted Blueprint of the "blueprint" / "graph" providers).
	Add(TEXT("seq_profile"), 1, {TEXT("seq.read.v1"), TEXT("seq.inspect.v1"), TEXT("seq.catalog.v1"), TEXT("seq.edit.v1"), TEXT("seq.status.v1"),
		TEXT("editor.class.resolve_canonical.v1"), TEXT("editor.asset.resolve_existing.v1"), TEXT("editor.reference.resolve_asset.v1"),
		TEXT("editor.scratch.create_by_factory.v1"), TEXT("editor.asset.rollback_created.v1"),
		TEXT("blueprint.member.query.v1"), TEXT("blueprint.member.interface.add.v1"), TEXT("blueprint.member.interface.remove.v1"),
		TEXT("blueprint.member.variable.set.v1"), TEXT("blueprint.member.variable.remove.v1"), TEXT("blueprint.member.property.set.v1"),
		TEXT("blueprint.member.dispatcher.set.v1"), TEXT("blueprint.member.dispatcher.remove.v1"), TEXT("blueprint.member.rename.v1"),
		TEXT("graph.list.v1"), TEXT("graph.query.v1"), TEXT("graph.actions.enumerate.v1"), TEXT("graph.create.v1"),
		TEXT("graph.remove.v1"), TEXT("graph.rename.v1"), TEXT("graph.finalize.v1"), TEXT("graph.function.flags.set.v1"),
		TEXT("graph.node.spawn.v1"), TEXT("graph.node.remove.v1"), TEXT("graph.node.rename.v1"),
		TEXT("graph.node.layout.set.v1"), TEXT("graph.node.property.get.v1"), TEXT("graph.node.property.set.v1"),
		TEXT("graph.node.user_pins.set.v1"), TEXT("graph.node.dynamic_pin.add.v1"), TEXT("graph.node.dynamic_pin.remove.v1"),
		TEXT("graph.node.pin_type.set.v1"),
		TEXT("graph.pin.connect.v1"), TEXT("graph.pin.break.v1"), TEXT("graph.pin.default.set.v1")});
	Add(TEXT("seq_profile"), 2, {TEXT("editor.scratch.clone_asset.v2"), TEXT("editor.asset.create_by_factory.v2")});
	Rows.Sort();
	return Rows;
}

TArray<FString> RequirementTuples(TConstArrayView<FAssetDocumentPrimitiveRequirement> Requirements)
{
	TArray<FString> Rows;
	for (const auto& Row : Requirements)
	{
		Rows.Add(FString::Printf(TEXT("%s|%u|%s"), *Row.PrimitiveId, Row.Version, *Row.RequiredByRouteId));
	}
	Rows.Sort();
	return Rows;
}

bool IsRejectedProviderClosure(
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors,
	TConstArrayView<FAssetDocumentPrimitiveRequirement> Requirements,
	TConstArrayView<FAssetDocumentProviderAvailability> Providers)
{
	return !ValidateAssetDocumentPrimitiveFreezeInput(
		Descriptors, Requirements, Providers, GetFrozenAuthorityExceptionRecords()).bValid;
}
}

static_assert(
	std::is_final_v<FAssetDocumentGenericRuntimeRoute>,
	"The production generic route has one Integration owner.");

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentIntegrationNormalEditorShutdownTest,
	"UEAssetDoc.GenericRuntime.Activation.NormalEditorShutdown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIntegrationNormalEditorShutdownTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FString BootstrapSourcePath = FPaths::Combine(
		GetAssetDocumentPluginBaseDir(),
		TEXT("Source/AssetDocumentIntegration/Private/"
			"AssetDocumentGenericRuntimeBootstrap.cpp"));
	FString BootstrapSource;
	if (!TestTrue(TEXT("reads bootstrap shutdown source"),
		FFileHelper::LoadFileToString(BootstrapSource, *BootstrapSourcePath)))
	{
		return false;
	}

	const FString PreExitBinding =
		TEXT("FCoreDelegates::OnEnginePreExit.AddRaw(\n"
			"\t\tthis, &FAssetDocumentGenericRuntimeBootstrap::Deactivate)");
	const FString PreExitRemoval =
		TEXT("FCoreDelegates::OnEnginePreExit.Remove(\n"
			"\t\t\tImpl->EnginePreExitHandle)");
	const int32 BindingIndex = BootstrapSource.Find(PreExitBinding);
	const int32 RemovalIndex = BootstrapSource.Find(PreExitRemoval);
	const int32 DestructorIndex = BootstrapSource.Find(
		TEXT("FAssetDocumentGenericRuntimeBootstrap::\n"
			"\t~FAssetDocumentGenericRuntimeBootstrap()"));
	const int32 DestructorDeactivateIndex = BootstrapSource.Find(
		TEXT("\tDeactivate();"), ESearchCase::CaseSensitive,
		ESearchDir::FromStart, DestructorIndex);
	const int32 PythonShutdownIndex = BootstrapSource.Find(
		TEXT("Impl->Runtime->ShutdownPythonBinding();"));
	const FString IdempotentPythonShutdown = TEXT(
		"if (Impl->Runtime.IsValid())\n"
		"\t{\n"
		"\t\tImpl->Runtime->ShutdownPythonBinding();\n"
		"\t\tImpl->Runtime.Reset();\n"
		"\t}");

	TestTrue(TEXT("normal engine pre-exit directly deactivates bootstrap"),
		BindingIndex != INDEX_NONE);
	TestEqual(TEXT("bootstrap binds exactly one pre-exit callback"),
		BootstrapSource.Find(PreExitBinding, ESearchCase::CaseSensitive,
			ESearchDir::FromStart, BindingIndex + PreExitBinding.Len()),
		INDEX_NONE);
	TestTrue(TEXT("pre-exit callback remains registered until destructor"),
		BindingIndex != INDEX_NONE && BindingIndex < DestructorIndex);
	TestTrue(TEXT("destructor removes pre-exit callback before fallback deactivation"),
		RemovalIndex > DestructorIndex
			&& RemovalIndex < DestructorDeactivateIndex);
	TestTrue(TEXT("deactivation owns Python binding shutdown"),
		PythonShutdownIndex != INDEX_NONE);
	TestTrue(TEXT("fallback deactivation cannot shut Python down twice"),
		BootstrapSource.Contains(IdempotentPythonShutdown));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentIntegrationSoleRuntimeOwnerTest,
	"UEAssetDoc.GenericRuntime.Activation.SoleRuntimeOwner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIntegrationSoleRuntimeOwnerTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	IAssetDocumentRuntimeModule* Python =
		FModuleManager::LoadModulePtr<IAssetDocumentRuntimeModule>(
			TEXT("AssetDocumentPython"));
	IAssetDocumentRuntimeModule* Integration =
		FModuleManager::LoadModulePtr<IAssetDocumentRuntimeModule>(
			TEXT("AssetDocumentIntegration"));
	TestNotNull(TEXT("Python module loads"), Python);
	TestNotNull(TEXT("Integration module loads"), Integration);
	if (Python != nullptr)
	{
		TestFalse(
			TEXT("Python module owns zero active runtimes"),
			Python->HasActiveRuntimeRegistration());
	}
	if (Integration != nullptr)
	{
		TestTrue(
			TEXT("Integration owns the sole active runtime"),
			Integration->HasActiveRuntimeRegistration());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentIntegrationTwoProviderCatalogTest,
	"UEAssetDoc.GenericRuntime.Activation.TwoProviderCatalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIntegrationTwoProviderCatalogTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Editor =
		CreateAssetDocumentEditorPrimitiveProvider();
	const TSharedRef<IAssetDocumentPrimitiveProvider> AI =
		CreateAssetDocumentAIPrimitiveProvider();
	const TArray<TSharedRef<IAssetDocumentPrimitiveProvider>> SourceProviders = {Editor, AI, MakeShared<FAssetDocumentAnimationRequestBinding>(),
		CreateAssetDocumentGraphPrimitiveProvider(), CreateAssetDocumentBlueprintMemberPrimitiveProvider(),
		CreateAssetDocumentWidgetPrimitiveProvider(), CreateAssetDocumentDataPrimitiveProvider(),
		CreateAssetDocumentAnimationAssetPrimitiveProvider(), CreateAssetDocumentNiagaraPrimitiveProvider(),
		CreateAssetDocumentAudioPrimitiveProvider(), CreateAssetDocumentRigPrimitiveProvider(),
		CreateAssetDocumentPhysicsPrimitiveProvider(), CreateAssetDocumentSequencerPrimitiveProvider()};
	const TArray<FAssetDocumentPrimitiveRequirement> Seed = GetAssetDocumentGenericRequirementsForTests();
	const auto Built = BuildAssetDocumentRequiredPrimitiveProjection(
		SourceProviders, Seed, GetFrozenAuthorityExceptionRecords());
	if (!TestTrue(TEXT("production seed projects successfully"), Built.Projection.IsValid()))
	{
		return false;
	}
	const auto& Descriptors = Built.Projection->Descriptors;
	const auto& Requirements = Built.Projection->Requirements;
	const auto& Providers = Built.Projection->Availability;
	const TCHAR* ExpectedGenericIds[] = {
		TEXT("editor.class.resolve_canonical.v1"), TEXT("editor.fragment.struct_codec.v1"),
		TEXT("editor.property.enumerate_admitted.v1"), TEXT("editor.property.enumerate_admitted_batch.v1"),
		TEXT("editor.property.preflight_admitted.v1"), TEXT("editor.property.read_admitted.v1"),
		TEXT("editor.property.write_admitted_batch.v1"), TEXT("editor.reference.resolve_asset.v1"),
		TEXT("editor.scratch.create_by_factory.v1"),
		TEXT("editor.asset.resolve_existing.v1"), TEXT("editor.asset.create_by_factory.v1"),
		TEXT("editor.property.inspect_evidence.v1"), TEXT("ai.behavior_tree.selector_inspect_evidence.v1"),
		TEXT("editor.enhanced_input.check_domain.v1")};
	const TCHAR* ExpectedGenericV2Ids[] = {
		TEXT("editor.scratch.clone_asset.v2"), TEXT("editor.asset.create_by_factory.v2")};
	const TCHAR* ExpectedBlackboardIds[] = {
		TEXT("ai.blackboard.entry.readback.v2"), TEXT("ai.blackboard.parent.readback.v2"),
		TEXT("ai.blackboard.local_keys.readback.v2"), TEXT("ai.blackboard.entry.update_existing.v2"),
		TEXT("ai.blackboard.parent.set.v2"), TEXT("ai.blackboard.entry.add.v2"),
		TEXT("ai.blackboard.entry.replace_class.v2"), TEXT("ai.blackboard.entry.remove.v2"),
		TEXT("ai.blackboard.local_keys.reorder.v2")};
	TestTrue(TEXT("Python declares a non-empty required set"), Seed.Num() > 0);
	const TCHAR* ExpectedAnimationIds[] = {
		TEXT("editor.class.resolve_canonical.v1"), TEXT("editor.asset.resolve_existing.v1"),
		TEXT("editor.scratch.clone_asset.v2"), TEXT("editor.scratch.create_by_factory.v1"),
		TEXT("editor.asset.create_by_factory.v2"), TEXT("editor.asset.rollback_created.v1"),
		TEXT("editor.property.enumerate_admitted.v1"),
		TEXT("animation.montage.profile.snapshot.v1"), TEXT("animation.curve.float.replace.v2"),
		TEXT("animation.asset.preview_mesh.set.v1"), TEXT("animation.asset.skeleton.set.v1"),
		TEXT("animation.montage.slot_tracks.replace.v1"), TEXT("animation.montage.sections.mutate.v1"),
		TEXT("animation.montage.notifies.replace.v1"), TEXT("animation.montage.notify_states.replace.v1")};
	for (const TCHAR* Id : ExpectedAnimationIds)
	{
		const uint32 Version = FString(Id).EndsWith(TEXT(".v2")) ? 2 : 1;
		TestTrue(TEXT("exact production Animation tuple"), Seed.ContainsByPredicate([&](const auto& Row)
		{ return Row.PrimitiveId == Id && Row.Version == Version && Row.RequiredByRouteId == TEXT("animation_profile"); }));
	}
	for (const TCHAR* Id : ExpectedGenericIds)
	{
		TestTrue(TEXT("exact generic tuple retained"), Seed.ContainsByPredicate([&](const auto& Row)
		{ return Row.PrimitiveId == Id && Row.Version == 1 && Row.RequiredByRouteId == TEXT("route.generic.v1"); }));
	}
	for (const TCHAR* Id : ExpectedGenericV2Ids)
	{
		TestTrue(TEXT("exact generic lifecycle tuple"), Seed.ContainsByPredicate([&](const auto& Row)
		{ return Row.PrimitiveId == Id && Row.Version == 2 && Row.RequiredByRouteId == TEXT("route.generic.v1"); }));
	}
	for (const TCHAR* Id : ExpectedBlackboardIds)
	{
		TestTrue(TEXT("exact Blackboard tuple retained"), Seed.ContainsByPredicate([&](const auto& Row)
		{ return Row.PrimitiveId == Id && Row.Version == 2 && Row.RequiredByRouteId == TEXT("ai_profile"); }));
	}
	const TCHAR* ExpectedAnimationAssetIds[] = {
		TEXT("animation_asset.skeleton.read.v1"), TEXT("animation_asset.skeleton.edit.v1"),
		TEXT("animation_asset.blend_space.read.v1"), TEXT("animation_asset.blend_space.edit.v1"),
		TEXT("animation_asset.pose_asset.read.v1"), TEXT("animation_asset.pose_asset.edit.v1"),
		TEXT("animation_asset.composite.read.v1"), TEXT("animation_asset.composite.edit.v1")};
	for (const TCHAR* Id : ExpectedAnimationAssetIds)
	{
		TestTrue(TEXT("exact animation asset tuple"), Seed.ContainsByPredicate([&](const auto& Row)
		{ return Row.PrimitiveId == Id && Row.Version == 1 && Row.RequiredByRouteId == TEXT("animation_asset_profile"); }));
	}
	TSet<FString> SeededOperations;
	for (const auto& Row : Seed) { SeededOperations.Add(FString::Printf(TEXT("%s|%u"), *Row.PrimitiveId, Row.Version)); }
	TestEqual(TEXT("only the seeded operations are selected"), Descriptors.Num(), SeededOperations.Num());
	if (!TestEqual(TEXT("exactly thirteen projected provider rows"), Providers.Num(), 13)) { return false; }
	TestEqual(TEXT("canonical AI provider first"), Providers[0].ProviderId, FString(TEXT("ai")));
	TestEqual(TEXT("canonical Animation provider second"), Providers[1].ProviderId, FString(TEXT("animation")));
	TestEqual(TEXT("canonical Animation asset provider third"), Providers[2].ProviderId, FString(TEXT("animation_asset")));
	TestEqual(TEXT("canonical Audio provider fourth"), Providers[3].ProviderId, FString(TEXT("audio")));
	TestEqual(TEXT("canonical Blueprint provider fifth"), Providers[4].ProviderId, FString(TEXT("blueprint")));
	TestEqual(TEXT("canonical Data provider sixth"), Providers[5].ProviderId, FString(TEXT("data")));
	TestEqual(TEXT("canonical Editor provider seventh"), Providers[6].ProviderId, FString(TEXT("editor")));
	TestEqual(TEXT("canonical FX provider eighth"), Providers[7].ProviderId, FString(TEXT("fx")));
	TestEqual(TEXT("canonical Graph provider ninth"), Providers[8].ProviderId, FString(TEXT("graph")));
	TestEqual(TEXT("canonical Physics provider tenth"), Providers[9].ProviderId, FString(TEXT("physics")));
	TestEqual(TEXT("canonical Rig provider eleventh"), Providers[10].ProviderId, FString(TEXT("rig")));
	TestEqual(TEXT("canonical Sequencer provider twelfth"), Providers[11].ProviderId, FString(TEXT("seq")));
	TestEqual(TEXT("canonical UMG provider thirteenth"), Providers[12].ProviderId, FString(TEXT("umg")));
	int32 DeclaredTupleCount = 0;
	for (const auto& Descriptor : Descriptors)
	{
		TestTrue(TEXT("no unseeded operation"), Seed.ContainsByPredicate([&](const auto& Row)
		{ return Row.PrimitiveId == Descriptor.PrimitiveId && Row.Version == Descriptor.Version; }));
		DeclaredTupleCount += Descriptor.RequiredConsumerIds.Num();
		for (const auto& Consumer : Descriptor.RequiredConsumerIds)
		{
			TestTrue(TEXT("every original consumer tuple is present"), Requirements.ContainsByPredicate([&](const auto& Row)
			{ return Row.PrimitiveId == Descriptor.PrimitiveId && Row.Version == Descriptor.Version && Row.RequiredByRouteId == Consumer; }));
		}
	}
	TestEqual(TEXT("manifest has exactly the selected declared consumer union"), Requirements.Num(), DeclaredTupleCount);
	TestTrue(TEXT("manifest is exactly the declared set"), RequirementTuples(Requirements) == RequirementTuples(Seed));
	TestTrue(TEXT("production projection passes full Core freeze contract"),
		ValidateAssetDocumentPrimitiveFreezeInput(Descriptors, Requirements, Providers,
			GetFrozenAuthorityExceptionRecords()).bValid);

	const TCHAR* const ProviderNames[] = {TEXT("AI"), TEXT("Animation"), TEXT("Animation asset"), TEXT("Audio"),
		TEXT("Blueprint"), TEXT("Data"), TEXT("Editor"), TEXT("FX"), TEXT("Graph"), TEXT("Physics"), TEXT("Rig"), TEXT("Sequencer"), TEXT("UMG")};
	for (int32 Index = 0; Index < Providers.Num(); ++Index)
	{
		TArray<FAssetDocumentProviderAvailability> Missing = Providers;
		Missing.RemoveAt(Index);
		TestTrue(FString::Printf(TEXT("missing %s provider fails closed"), ProviderNames[Index]),
			IsRejectedProviderClosure(Descriptors, Requirements, Missing));
	}
	TArray<FAssetDocumentProviderAvailability> DuplicateEditor = Providers;
	DuplicateEditor.Add(Providers[6]);
	TestTrue(TEXT("duplicate editor provider fails closed"),
		IsRejectedProviderClosure(
			Descriptors, Requirements, DuplicateEditor));
	TArray<FAssetDocumentProviderAvailability> DuplicateAI = Providers;
	DuplicateAI.Add(Providers[0]);
	TestTrue(TEXT("duplicate AI provider fails closed"),
		IsRejectedProviderClosure(
			Descriptors, Requirements, DuplicateAI));

	FAssetDocumentGenericRuntimeBootstrap* Bootstrap =
		GetAssetDocumentActiveGenericRuntimeBootstrapForTests();
	TestNotNull(TEXT("active bootstrap is observable in automation"),
		Bootstrap);
	if (Bootstrap == nullptr)
	{
		return false;
	}
	TestEqual(TEXT("runtime revision is two"),
		GetAssetDocumentActiveGenericRuntimeRevisionForTests(), 2ull);
	TestEqual(TEXT("aggregate route freezes every declared requirement"),
		GetAssetDocumentGenericRequirementCountForTests(), Seed.Num());
	TestEqual(TEXT("one active runtime"),
		Bootstrap->GetActiveRuntimeCount(), 1);
	TestEqual(TEXT("thirteen active providers"),
		Bootstrap->GetProviderCount(), 13);
	TestEqual(TEXT("one core composition"),
		Bootstrap->GetCoreCompositionCount(), 1);
	TestEqual(TEXT("one route binding"),
		Bootstrap->GetRouteBindingCount(), 1);

	Bootstrap->Deactivate();
	TestEqual(TEXT("deactivation releases runtime"),
		Bootstrap->GetActiveRuntimeCount(), 0);
	TestEqual(TEXT("deactivation releases all providers"),
		Bootstrap->GetProviderCount(), 0);
	TestEqual(TEXT("deactivation releases composition"),
		Bootstrap->GetCoreCompositionCount(), 0);
	TestEqual(TEXT("deactivation releases route"),
		Bootstrap->GetRouteBindingCount(), 0);
	TestTrue(TEXT("same bootstrap reactivates exact closure"),
		Bootstrap->Activate());
	TestTrue(TEXT("reactivated bootstrap is active"),
		Bootstrap->IsActive());
	TestEqual(TEXT("reactivation restores revision two"),
		GetAssetDocumentActiveGenericRuntimeRevisionForTests(), 2ull);
	TestEqual(TEXT("reactivation restores thirteen providers"),
		Bootstrap->GetProviderCount(), 13);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentIntegrationDerivedRequirementParityTest,
	"UEAssetDoc.GenericRuntime.Activation.DerivedRequirementParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIntegrationDerivedRequirementParityTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FAssetDocumentGenericRuntimeBootstrap* Bootstrap =
		GetAssetDocumentActiveGenericRuntimeBootstrapForTests();
	if (!TestNotNull(TEXT("active bootstrap"), Bootstrap) || !TestTrue(TEXT("bootstrap active"), Bootstrap->IsActive()))
	{
		return false;
	}
	const TArray<FString> Expected = RetiredHardCodedRequirementTuples();
	const TArray<FString> Derived = RequirementTuples(Bootstrap->GetDeclaredRequirements());
	for (const FString& Row : Expected)
	{
		TestTrue(*FString::Printf(TEXT("derived set keeps %s"), *Row), Derived.Contains(Row));
	}
	for (const FString& Row : Derived)
	{
		TestTrue(*FString::Printf(TEXT("derived set adds nothing unexpected: %s"), *Row), Expected.Contains(Row));
	}
	TestTrue(TEXT("Python-derived required set equals the retired C++ list"), Derived == Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBlackboardDataCatalogClosureTest,
	"UEAssetDoc.Python.BlackboardData.CatalogClosure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBlackboardDataCatalogClosureTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FAssetDocumentGenericRuntimeBootstrap* Bootstrap =
		GetAssetDocumentActiveGenericRuntimeBootstrapForTests();
	if (!TestNotNull(
			TEXT("Blackboard catalog closure has an active Integration bootstrap"),
			Bootstrap))
	{
		return false;
	}
	TestTrue(
		TEXT("Blackboard catalog closure keeps the Integration bootstrap active"),
		Bootstrap->IsActive());
	const TArray<FAssetDocumentPrimitiveRequirement> Declared = GetAssetDocumentGenericRequirementsForTests();
	TestEqual(TEXT("active catalog declares the nine Blackboard v2 requirements"),
		Declared.FilterByPredicate([](const FAssetDocumentPrimitiveRequirement& Row)
		{
			return Row.PrimitiveId.StartsWith(TEXT("ai.blackboard.")) && Row.Version == 2
				&& Row.RequiredByRouteId == TEXT("ai_profile");
		}).Num(), 9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericRequestAdapterShapeTest,
	"UEAssetDoc.GenericRuntime.Activation.RequestAdapterPreservesShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericRequestAdapterShapeTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	IAssetDocumentRuntimeModule* Integration =
		FModuleManager::LoadModulePtr<IAssetDocumentRuntimeModule>(
			TEXT("AssetDocumentIntegration"));
	TestNotNull(TEXT("Integration module loads for public Service fixtures"),
		Integration);
	if (Integration == nullptr)
	{
		return false;
	}
	TestTrue(TEXT("Integration route is active for public Service fixtures"),
		Integration->HasActiveRuntimeRegistration());
	if (!Integration->HasActiveRuntimeRegistration())
	{
		return false;
	}
	FAssetDocumentService& Service =
		FAssetDocumentModule::Get().GetService();
	const FAssetDocumentResult Schema = Service.GetSchema();
	TestTrue(TEXT("schema fixture succeeds"), Schema.IsSuccess());
	TestTrue(TEXT("schema fixture has payload"), Schema.Payload.IsValid());

	const FString FixtureId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString FixtureDirectory = FPaths::Combine(
		FPaths::ProjectContentDir(),
		TEXT("AssetDocumentGenericCutover/RequestAdapter"),
		FixtureId);
	const FString FixtureTargetRoot = FString::Printf(
		TEXT("/Game/AssetDocumentGenericCutover/RequestAdapter/%s"),
		*FixtureId);
	const FString ShapeTarget = FixtureTargetRoot / TEXT("Shape");
	const FString ManifestTarget = FixtureTargetRoot / TEXT("Manifest");
	const FString TemplateTarget = FixtureTargetRoot / TEXT("Template");
	const FString ShapeSidecarPath = FPaths::Combine(
		FixtureDirectory,
		TEXT("Shape.assetdoc.json"));
	const FString ManifestSidecarPath = FPaths::Combine(
		FixtureDirectory,
		TEXT("Manifest.assetdoc.json"));
	const FString TemplateSidecarPath = FPaths::Combine(
		FixtureDirectory,
		TEXT("Template.assetdoc.json"));
	IFileManager::Get().MakeDirectory(*FixtureDirectory, true);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*FixtureDirectory, false, true);
	};
	TestTrue(
		TEXT("writes shape sidecar"),
		FFileHelper::SaveStringToFile(
			FString::Printf(
				TEXT("{\"schema_version\":1.0,\"asset_type\":\"GenericAsset\",")
				TEXT("\"asset_document_tools\":[\"get_asset_document_schema\",")
				TEXT("\"inspect_asset_document_target\"],\"field_naming\":")
				TEXT("{\"ban_abbreviations\":true},\"Target\":\"%s\"}"),
				*ShapeTarget),
			*ShapeSidecarPath));
	TestTrue(
		TEXT("writes manifest sidecar"),
		FFileHelper::SaveStringToFile(
			FString::Printf(
				TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\"}"),
				*ManifestTarget),
			*ManifestSidecarPath));
	TestTrue(
		TEXT("writes template sidecar"),
		FFileHelper::SaveStringToFile(
			FString::Printf(TEXT("{\"Target\":\"%s\"}"), *TemplateTarget),
			*TemplateSidecarPath));

	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = ShapeSidecarPath;
	Request.bSaveAsset = false;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	const TOptional<FAssetDocumentRuntimeRequestDto> Adapted =
		FAssetDocumentGenericRequestAdapter::AdaptApplyFile(
			Request,
			17,
			Diagnostics);

	TestTrue(TEXT("valid service request adapts"), Adapted.IsSet());
	TestEqual(TEXT("adapter emits no diagnostics"), Diagnostics.Num(), 0);
	if (!Adapted.IsSet())
	{
		return false;
	}
	TestEqual(
		TEXT("operation remains Apply"),
		Adapted->Operation,
		EAssetDocumentRuntimeOperation::Apply);
	TestEqual(TEXT("revision is pinned"), Adapted->ExpectedRuntimeRevision, 17ull);

	const FAssetDocumentRuntimeValueDto* Version =
		FindObjectField(Adapted->CanonicalDocument, TEXT("schema_version"));
	const FAssetDocumentRuntimeValueDto* AssetType =
		FindObjectField(Adapted->CanonicalDocument, TEXT("asset_type"));
	const FAssetDocumentRuntimeValueDto* Tools =
		FindObjectField(Adapted->CanonicalDocument, TEXT("asset_document_tools"));
	const FAssetDocumentRuntimeValueDto* FieldNaming =
		FindObjectField(Adapted->CanonicalDocument, TEXT("field_naming"));
	TestNotNull(TEXT("nested object is retained"), FieldNaming);
	if (FieldNaming == nullptr)
	{
		return false;
	}
	const FAssetDocumentRuntimeValueDto* Boolean =
		FindObjectField(*FieldNaming, TEXT("ban_abbreviations"));
	TestTrue(
		TEXT("boolean shape is retained"),
		Boolean && IsBooleanShape(*Boolean) && Boolean->BooleanValue);
	TestTrue(
		TEXT("number shape is retained"),
		Version && IsNumberShape(*Version) && Version->NumberValue == 1.0);
	TestTrue(
		TEXT("string shape is retained"),
		AssetType && IsStringShape(*AssetType)
			&& AssetType->StringValue == TEXT("GenericAsset"));
	TestTrue(
		TEXT("array shape and ordering are retained"),
		Tools
			&& Tools->Kind == EAssetDocumentRuntimeValueKind::Array
			&& Tools->Elements.Num() > 1
			&& IsStringShape(Tools->Elements[0])
			&& Tools->Elements[0].StringValue
				== TEXT("get_asset_document_schema")
			&& IsStringShape(Tools->Elements[1])
			&& Tools->Elements[1].StringValue
				== TEXT("inspect_asset_document_target"));
	FAssetDocumentApplyFileRequest ManifestRequest;
	ManifestRequest.FilePath = ManifestSidecarPath;
	TArray<FAssetDocumentRuntimeDiagnostic> ManifestDiagnostics;
	const TOptional<FAssetDocumentRuntimeRequestDto> AdaptedManifest =
		FAssetDocumentGenericRequestAdapter::AdaptApplyFile(
			ManifestRequest,
			17,
			ManifestDiagnostics);
	const FAssetDocumentRuntimeValueDto* ManifestVersion =
		AdaptedManifest.IsSet()
			? FindObjectField(
				AdaptedManifest->CanonicalDocument,
				TEXT("SchemaVersion"))
			: nullptr;
	TestTrue(
		TEXT("AssetDocument SchemaVersion retains integer semantics"),
		ManifestVersion
			&& ManifestVersion->Kind
				== EAssetDocumentRuntimeValueKind::Integer
			&& ManifestVersion->IntegerValue == 1);

	FAssetDocumentTemplateRequest TemplateRequest;
	TemplateRequest.Target = TemplateTarget + TEXT(".Template");
	TemplateRequest.Class =
		TEXT("/Script/AssetDocumentTests.TestDataAsset");
	const FAssetDocumentResult Template = Service.CreateTemplate(TemplateRequest);
	FString TemplateFailure = Template.Message;
	for (const FAssetDocumentDiagnostic& Diagnostic : Template.Diagnostics)
	{
		TemplateFailure += FString::Printf(
			TEXT(" | %s @ %s: %s"),
			*Diagnostic.Code,
			*Diagnostic.Path,
			*Diagnostic.Message);
	}
	TestTrue(
		*FString::Printf(
			TEXT("generic template fixture succeeds (%s)"),
			*TemplateFailure),
		Template.IsSuccess());
	TestTrue(
		TEXT("generic template fixture has a payload"),
		Template.Payload.IsValid());
	FAssetDocumentApplyFileRequest TemplateApply;
	TemplateApply.FilePath = TemplateSidecarPath;
	TArray<FAssetDocumentRuntimeDiagnostic> TemplateDiagnostics;
	const TOptional<FAssetDocumentRuntimeRequestDto> AdaptedTemplate =
		FAssetDocumentGenericRequestAdapter::AdaptApplyFile(
			TemplateApply,
			17,
			TemplateDiagnostics);
	TestEqual(
		TEXT("document package target projects to runtime object identity"),
		AdaptedTemplate.IsSet()
			? AdaptedTemplate->TargetAssetPath
			: FString(),
		TemplateRequest.Target);
	const FAssetDocumentRuntimeValueDto* DocumentTarget =
		AdaptedTemplate.IsSet()
			? FindObjectField(
				AdaptedTemplate->CanonicalDocument,
				TEXT("Target"))
			: nullptr;
	TestTrue(
		TEXT("canonical document retains its package target"),
		DocumentTarget
			&& IsStringShape(*DocumentTarget)
			&& DocumentTarget->StringValue
				== TemplateTarget);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericReadOnlySinglePhaseTest,
	"UEAssetDoc.GenericRuntime.Activation.ReadOnlySinglePhase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericReadOnlySinglePhaseTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FReadOnlyRuntime Runtime;
	FAssetDocumentRuntimeRequestDto Request;
	Request.RequestId = FGuid::NewGuid();
	Request.Operation = EAssetDocumentRuntimeOperation::Validate;
	Request.TargetAssetPath = TEXT("/Game/Generic/DA_ReadOnly");
	Request.RequestedAssetClassPath =
		TEXT("/Script/AssetDocumentTests.TestDataAsset");
	Request.ExpectedRuntimeRevision = 17;
	Request.CanonicalDocument.Kind = EAssetDocumentRuntimeValueKind::Object;
	const FAssetDocumentRuntimeResultDto Result =
		FAssetDocumentGenericReadOnlyDispatcher::DispatchValidate(
			Runtime,
			Request);

	TestEqual(
		TEXT("one phase creates exactly one context"),
		Runtime.ContextCreateCount,
		1);
	TestEqual(
		TEXT("one phase calls Host exactly once"),
		Runtime.Host.DispatchCount,
		1);
	TestEqual(
		TEXT("frozen phase is Validate"),
		Runtime.ObservedMetadata.Phase,
		EAssetDocumentRuntimeRequestPhase::Validate);
	TestEqual(
		TEXT("Host sees the same frozen phase"),
		Runtime.Host.ObservedPhase,
		EAssetDocumentRuntimeRequestPhase::Validate);
	TestTrue(
		TEXT("read-only phase has a valid handle scope"),
		Runtime.bObservedValidTransaction);
	TestFalse(
		TEXT("read-only phase has no active transaction"),
		Runtime.bObservedActiveTransaction);
	TestEqual(
		TEXT("read-only phase performs no handle invalidation"),
		Runtime.HandleInvalidationCount,
		0);
	TestTrue(
		TEXT("Python canonical bytes pass through unchanged"),
		Result.CanonicalBytes == TArray<uint8>({1, 2, 3}));
	TestEqual(
		TEXT("Python semantic hash passes through unchanged"),
		Result.SemanticHash,
		FString(TEXT("python-semantic-hash")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentIngressSingleCoordinatorTest,
	"UEAssetDoc.GenericRuntime.IngressSingleCoordinator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIngressSingleCoordinatorTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	TSharedRef<FAssetDocumentService> Service =
		MakeShared<FAssetDocumentService>();
	TSharedRef<FAssetDocumentIngressQueue> Ingress =
		MakeShared<FAssetDocumentIngressQueue>(Service);
	TSharedRef<FAssetDocumentIngressQueue> SharedIngress = Ingress;
	TestEqual(
		TEXT("all ingress aliases share one coordinator"),
		Ingress->GetCoordinatorId(),
		SharedIngress->GetCoordinatorId());

	TSharedRef<FAssetDocumentGenericRuntimeRoute> Route =
		MakeShared<FAssetDocumentGenericRuntimeRoute>();
	Service->BindGenericRuntimeRoute(Route);
	const FString SidecarPath = FPaths::Combine(
		FPaths::ProjectContentDir(),
		TEXT("AssetDocumentGenericCutover/DA_Ingress.assetdoc.json"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SidecarPath), true);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*SidecarPath, false, true);
	};
	const FString Json = TEXT(
		"{\"SchemaVersion\":1,\"AssetType\":\"GenericAsset\","
		"\"Target\":\"/Game/AssetDocumentGenericCutover/DA_Ingress\","
		"\"Class\":\"/Script/AssetDocumentTests.TestDataAsset\",\"Action\":\"Create\","
		"\"Properties\":{}}");
	TestTrue(
		TEXT("writes ingress sidecar"),
		FFileHelper::SaveStringToFile(Json, *SidecarPath));
	FAssetDocumentApplyFileRequest Request;
	Request.FilePath = SidecarPath;
	const FAssetDocumentResult Result =
		Ingress->SubmitApplyFile(Request);
	TestFalse(TEXT("uncomposed test route is terminal"), Result.IsSuccess());
	TestEqual(
		TEXT("uncomposed route reports inactive"),
		Result.Diagnostics.IsEmpty()
			? FString()
			: Result.Diagnostics[0].Code,
		FString(TEXT("GenericRuntime.Inactive")));
	return true;
}
#endif
