# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

import unittest

from asset_document import bootstrap
from asset_document.dto import PrimitiveRequirement
from asset_document.meta import ProfileMeta
from asset_document.profiles import behavior_tree_execution
from asset_document.profiles.behavior_tree_production import BEHAVIOR_TREE_PROFILE_META
from asset_document.registry import ProfileRegistryBuilder
from asset_document.regions.primitive_catalog import GENERIC_REQUIRED_PRIMITIVES


def _rows(route, version, *ids):
    return [(primitive_id, version, route) for primitive_id in ids]


# One-off parity snapshot of the (originally 48) tuples the Integration bootstrap used to
# hard-code in C++ (MakeGenericRequirements / Append*Requirements). The only
# intended difference: the BehaviorTree selector batch resolver is now declared
# by the BehaviorTree Profile under its own route instead of the auxiliary
# "ai_blackboard_binding_repair" consumer. Since the generic factory lifecycle
# (factory_generic.v1) the Generic route also replaced the DataAsset-only
# clone/create_from_class scratches with the class-neutral lifecycle rows.
LEGACY_CPP_REQUIREMENTS = frozenset(
    _rows("route.generic.v1", 1,
          "editor.class.resolve_canonical.v1", "editor.fragment.struct_codec.v1",
          "editor.property.enumerate_admitted.v1", "editor.property.enumerate_admitted_batch.v1",
          "editor.property.preflight_admitted.v1", "editor.property.read_admitted.v1",
          "editor.property.write_admitted_batch.v1", "editor.reference.resolve_asset.v1",
          "editor.scratch.create_by_factory.v1",
          "editor.asset.resolve_existing.v1", "editor.asset.create_by_factory.v1",
          "editor.property.inspect_evidence.v1", "ai.behavior_tree.selector_inspect_evidence.v1")
    + _rows("route.generic.v1", 2,
            "editor.scratch.clone_asset.v2", "editor.asset.create_by_factory.v2")
    + _rows("ai_profile", 2,
            "ai.blackboard.entry.readback.v2", "ai.blackboard.parent.readback.v2",
            "ai.blackboard.local_keys.readback.v2", "ai.blackboard.entry.update_existing.v2",
            "ai.blackboard.parent.set.v2", "ai.blackboard.entry.add.v2",
            "ai.blackboard.entry.replace_class.v2", "ai.blackboard.entry.remove.v2",
            "ai.blackboard.local_keys.reorder.v2")
    + _rows("animation_profile", 1,
            "editor.class.resolve_canonical.v1", "editor.asset.resolve_existing.v1",
            "animation.montage.profile.snapshot.v1", "animation.asset.preview_mesh.set.v1",
            "animation.asset.skeleton.set.v1", "animation.montage.slot_tracks.replace.v1",
            "animation.montage.sections.mutate.v1", "animation.montage.notifies.replace.v1",
            "animation.montage.notify_states.replace.v1")
    + [("animation.curve.float.replace.v2", 2, "animation_profile")]
    + _rows("ai_profile", 1,
            "ai.behavior_tree.blackboard.readback.v1", "ai.behavior_tree.binding.inspect.v1",
            "ai.behavior_tree.selector.inventory.v1", "ai.behavior_tree.graph.create.v1",
            "ai.behavior_tree.graph.author_scratch.v1", "ai.behavior_tree.graph.install.v1",
            "ai.behavior_tree.blackboard.set.v1")
    + _rows("ai_profile", 2,
            "ai.behavior_tree.authored_graph.readback.v2", "ai.behavior_tree.node_class.inspect.v2",
            "ai.behavior_tree.selector.resolve_batch.v2")
    + [("ai.behavior_tree.value_or_key.refresh.v3", 3, "ai_profile")]
)
# The registered UBlueprint Profile (profile.ublueprint.v2) declares its own closure under
# blueprint_profile: lifecycle + resolvers, the 20 graph primitives and 10 member primitives.
UBLUEPRINT_REQUIREMENTS = frozenset(
    _rows("blueprint_profile", 1,
          "editor.class.resolve_canonical.v1", "editor.asset.resolve_existing.v1",
          "editor.blueprint.scratch.clone.v1", "editor.blueprint.scratch.create.v1",
          "editor.blueprint.asset.create.v1", "editor.asset.rollback_created.v1",
          "graph.list.v1", "graph.query.v1", "graph.actions.enumerate.v1", "graph.node.spawn.v1",
          "graph.node.remove.v1", "graph.pin.connect.v1", "graph.pin.break.v1", "graph.pin.default.set.v1",
          "graph.node.property.get.v1", "graph.node.property.set.v1", "graph.node.layout.set.v1",
          "graph.node.rename.v1", "graph.node.user_pins.set.v1", "graph.node.dynamic_pin.add.v1",
          "graph.node.dynamic_pin.remove.v1", "graph.create.v1", "graph.remove.v1", "graph.rename.v1",
          "graph.function.flags.set.v1", "graph.finalize.v1", "graph.node.pin_type.set.v1",
          "graph.link.check.v1",
          *(f"blueprint.member.{name}.v1" for name in (
              "query", "parent.set", "interface.add", "interface.remove", "variable.set", "variable.remove",
              "component.add", "component.attach", "component.remove", "property.set",
              "dispatcher.set", "dispatcher.remove", "timeline.set", "timeline.remove", "rename")))
)
# Declared after the cutover: AnimMontage settings restore (generic Editor
# property batch for plain fields, one Montage settings primitive) and Validate's
# read-only timeline check.
MONTAGE_SETTINGS_REQUIREMENTS = frozenset(_rows(
    "animation_profile", 1,
    "editor.property.enumerate_admitted_batch.v1", "editor.property.write_admitted_batch.v1",
    "animation.montage.settings.set.v1", "animation.montage.structure.check.v1"))
# AnimMontage moved from its own four editor.animation_montage.* lifecycle
# primitives onto the shared generic factory lifecycle (Skeleton as factory
# context) and creates rich documents in one apply (class-level admission).
MONTAGE_CREATE_REQUIREMENTS = frozenset(
    _rows("animation_profile", 1,
          "editor.scratch.create_by_factory.v1", "editor.asset.rollback_created.v1",
          "editor.property.enumerate_admitted.v1",
          # A slot the Skeleton lacks is reported with the Skeleton's slots (AnimMontage.UnknownSlot).
          "animation_asset.skeleton.read.v1")
    + _rows("animation_profile", 2,
            "editor.scratch.clone_asset.v2", "editor.asset.create_by_factory.v2"))
# WidgetBlueprint (profile.widget_blueprint.v1): the Blueprint rows above plus the UMG Designer
# and animation primitives, on the same route.
WIDGET_BLUEPRINT_REQUIREMENTS = frozenset(_rows(
    "blueprint_profile", 1,
    "umg.tree.query.v1", "umg.tree.add.v1", "umg.tree.move.v1", "umg.tree.remove.v1",
    "umg.widget.rename.v1", "umg.widget.variable.set.v1", "umg.widget.property.set.v1", "umg.widget.navigation.set.v1",
    "umg.binding.set.v1", "umg.binding.remove.v1", "umg.desired_focus.set.v1", "umg.palette_category.set.v1", "umg.design_size.set.v1", "umg.anim.query.v1", "umg.anim.set.v1",
    "umg.anim.remove.v1", "umg.anim.track.set.v1", "umg.anim.track.remove.v1"))
# AnimSequence (profile.anim_sequence.v1): one authoring snapshot and one
# snapshot-covered authoring edit; everything else is already declared above.
ANIM_SEQUENCE_REQUIREMENTS = frozenset(_rows(
    "animation_profile", 1,
    "animation.asset.authoring.snapshot.v1", "animation.asset.authoring.edit.v1"))

# AnimBlueprint (profile.anim_blueprint.v1) shares the blueprint_profile route; it adds the
# Animation Blueprint settings write.
ANIM_BLUEPRINT_REQUIREMENTS = frozenset(_rows("blueprint_profile", 1, "blueprint.member.setting.set.v1"))

# Data table family (DataTable / CompositeDataTable / MirrorDataTable, CurveTable /
# CompositeCurveTable, StringTable), MaterialInstanceConstant and Material / MaterialFunction on the
# data_profile route: the
# "data" provider rows, the RowStruct / Parent resolver for the factory context and the generic
# factory lifecycle.
DATA_TABLE_REQUIREMENTS = frozenset(
    _rows("data_profile", 1,
          "data.data_table.read.v1", "data.data_table.edit.v1", "data.curve_table.read.v1",
          "data.curve_table.edit.v1", "data.string_table.read.v1", "data.string_table.edit.v1",
          "data.user_struct.read.v1", "data.user_struct.edit.v1", "data.user_enum.read.v1",
          "data.user_enum.edit.v1",
          "data.material_instance.read.v1", "data.material_instance.edit.v1",
          "data.material.read.v1", "data.material.edit.v1", "data.material.compile.v1",
          "editor.class.resolve_canonical.v1", "editor.asset.resolve_existing.v1",
          "editor.reference.resolve_asset.v1", "editor.scratch.create_by_factory.v1",
          "editor.asset.rollback_created.v1")
    + _rows("data_profile", 2, "editor.scratch.clone_asset.v2", "editor.asset.create_by_factory.v2"))

# Standalone animation assets (Skeleton, the blend space classes, PoseAsset, AnimComposite) on the
# animation_asset_profile route: the "animation_asset" provider rows, the context resolver for the
# create-time Skeleton / source animation and the generic factory lifecycle.
ANIMATION_ASSET_REQUIREMENTS = frozenset(
    _rows("animation_asset_profile", 1,
          "animation_asset.skeleton.read.v1", "animation_asset.skeleton.edit.v1",
          "animation_asset.blend_space.read.v1", "animation_asset.blend_space.edit.v1",
          "animation_asset.pose_asset.read.v1", "animation_asset.pose_asset.edit.v1",
          "animation_asset.composite.read.v1", "animation_asset.composite.edit.v1",
          "editor.class.resolve_canonical.v1", "editor.asset.resolve_existing.v1",
          "editor.reference.resolve_asset.v1", "editor.scratch.create_by_factory.v1",
          "editor.asset.rollback_created.v1")
    + _rows("animation_asset_profile", 2, "editor.scratch.clone_asset.v2", "editor.asset.create_by_factory.v2"))

# StateTree (profile.state_tree.v1) on the state_tree_profile route: the ai.state_tree.* rows, the schema
# class resolvers for the factory context and the generic factory lifecycle.
STATE_TREE_REQUIREMENTS = frozenset(
    _rows("state_tree_profile", 1,
          "ai.state_tree.read.v1", "ai.state_tree.edit.v1", "ai.state_tree.compile.v1",
          "editor.class.resolve_canonical.v1", "editor.asset.resolve_existing.v1",
          "editor.reference.resolve_asset.v1", "editor.scratch.create_by_factory.v1",
          "editor.asset.rollback_created.v1")
    + _rows("state_tree_profile", 2, "editor.scratch.clone_asset.v2", "editor.asset.create_by_factory.v2"))

# EnvQuery (profile.env_query.v1) on the env_query_profile route: the ai.env_query.* rows, the class resolvers and
# the generic factory lifecycle.
ENV_QUERY_REQUIREMENTS = frozenset(
    _rows("env_query_profile", 1,
          "ai.env_query.read.v1", "ai.env_query.edit.v1", "ai.env_query.finalize.v1",
          "editor.class.resolve_canonical.v1", "editor.asset.resolve_existing.v1",
          "editor.scratch.create_by_factory.v1", "editor.asset.rollback_created.v1")
    + _rows("env_query_profile", 2, "editor.scratch.clone_asset.v2", "editor.asset.create_by_factory.v2"))

# Niagara (profile.niagara_system / _emitter / _script / _parameter_collection.v1) on the fx_profile route: the fx
# read, the system-level edit and compile, and the generic factory lifecycle.
NIAGARA_REQUIREMENTS = frozenset(
    _rows("fx_profile", 1,
          "fx.niagara.read.v1", "fx.niagara.edit.v1", "fx.niagara.compile.v1", "editor.class.resolve_canonical.v1", "editor.asset.resolve_existing.v1",
          "editor.scratch.create_by_factory.v1", "editor.asset.rollback_created.v1",
          # Script graphs and scratch pads through the shared graph primitives.
          "graph.list.v1", "graph.query.v1", "graph.actions.enumerate.v1", "graph.node.spawn.v1",
          "graph.node.remove.v1", "graph.pin.connect.v1", "graph.pin.break.v1", "graph.pin.default.set.v1",
          "graph.node.property.get.v1", "graph.node.property.set.v1", "graph.node.layout.set.v1",
          "graph.node.rename.v1", "graph.node.user_pins.set.v1", "graph.node.dynamic_pin.add.v1",
          "graph.node.dynamic_pin.remove.v1", "graph.create.v1", "graph.remove.v1", "graph.rename.v1",
          "graph.function.flags.set.v1", "graph.finalize.v1", "graph.node.pin_type.set.v1")
    + _rows("fx_profile", 2, "editor.scratch.clone_asset.v2", "editor.asset.create_by_factory.v2"))

# MetaSound Source / Patch (profile.metasound_source.v1 / profile.metasound_patch.v1) on the audio_profile
# route: the "audio" provider (read, catalog, edit, compile), the preset parent resolver for the factory
# context and the generic factory lifecycle. SoundCue (profile.sound_cue.v1) on the same route: the cue read
# and the shared graph primitives.
AUDIO_REQUIREMENTS = frozenset(
    _rows("audio_profile", 1,
          "audio.metasound.read.v1", "audio.metasound.catalog.v1", "audio.metasound.edit.v1",
          "audio.metasound.compile.v1", "audio.soundcue.read.v1",
          "editor.class.resolve_canonical.v1", "editor.asset.resolve_existing.v1",
          "editor.reference.resolve_asset.v1", "editor.scratch.create_by_factory.v1",
          "editor.asset.rollback_created.v1",
          "graph.list.v1", "graph.query.v1", "graph.actions.enumerate.v1", "graph.node.spawn.v1",
          "graph.node.remove.v1", "graph.pin.connect.v1", "graph.pin.break.v1", "graph.pin.default.set.v1",
          "graph.node.property.get.v1", "graph.node.property.set.v1", "graph.node.layout.set.v1",
          "graph.node.rename.v1", "graph.node.user_pins.set.v1", "graph.node.dynamic_pin.add.v1",
          "graph.node.dynamic_pin.remove.v1", "graph.create.v1", "graph.remove.v1", "graph.rename.v1",
          "graph.function.flags.set.v1", "graph.finalize.v1", "graph.node.pin_type.set.v1")
    + _rows("audio_profile", 2, "editor.scratch.clone_asset.v2", "editor.asset.create_by_factory.v2"))

# IK Rig / IK Retargeter / Control Rig (profile.ik_rig.v1 / profile.ik_retargeter.v1 / profile.control_rig.v1) on
# the rig_profile route: the "rig" provider (reads, edits, status, catalogs, suggestions) and the generic factory
# lifecycle.
RIG_REQUIREMENTS = frozenset(
    _rows("rig_profile", 1,
          "rig.ikrig.read.v1", "rig.ikrig.edit.v1", "rig.ikretargeter.read.v1", "rig.ikretargeter.edit.v1",
          "rig.status.v1", "rig.catalog.v1", "rig.suggest.v1", "rig.controlrig.read.v1", "rig.controlrig.catalog.v1",
          "rig.controlrig.edit.v1", "rig.controlrig.compile.v1",
          "editor.class.resolve_canonical.v1", "editor.asset.resolve_existing.v1",
          "editor.reference.resolve_asset.v1",  # Control Rig: the factory's parent class (modular rigs)
          "editor.scratch.create_by_factory.v1", "editor.asset.rollback_created.v1")
    + _rows("rig_profile", 2, "editor.scratch.clone_asset.v2", "editor.asset.create_by_factory.v2"))

# Physics Asset (profile.physics_asset.v1) on the physics_profile route: the "physics" provider (read, edit, status)
# and the generic factory lifecycle with the skeletal mesh as factory context (resolved as a reference).
PHYSICS_REQUIREMENTS = frozenset(
    _rows("physics_profile", 1,
          "physics.asset.read.v1", "physics.asset.edit.v1", "physics.status.v1",
          "editor.class.resolve_canonical.v1", "editor.asset.resolve_existing.v1", "editor.reference.resolve_asset.v1",
          "editor.scratch.create_by_factory.v1", "editor.asset.rollback_created.v1")
    + _rows("physics_profile", 2, "editor.scratch.clone_asset.v2", "editor.asset.create_by_factory.v2"))

# Level Sequence (profile.level_sequence.v1) on the seq_profile route: the "seq" provider (read, inspect, catalog,
# edit, status), the generic factory lifecycle and the hosted Director Blueprint's member and graph rows.
SEQ_REQUIREMENTS = frozenset(
    _rows("seq_profile", 1,
          "seq.read.v1", "seq.inspect.v1", "seq.catalog.v1", "seq.edit.v1", "seq.status.v1",
          "editor.class.resolve_canonical.v1", "editor.asset.resolve_existing.v1",
          "editor.reference.resolve_asset.v1",  # Template Sequence: the factory's bound actor class
          "editor.scratch.create_by_factory.v1", "editor.asset.rollback_created.v1",
          "graph.list.v1", "graph.query.v1", "graph.actions.enumerate.v1", "graph.node.spawn.v1",
          "graph.node.remove.v1", "graph.pin.connect.v1", "graph.pin.break.v1", "graph.pin.default.set.v1",
          "graph.node.property.get.v1", "graph.node.property.set.v1", "graph.node.layout.set.v1",
          "graph.node.rename.v1", "graph.node.user_pins.set.v1", "graph.node.dynamic_pin.add.v1",
          "graph.node.dynamic_pin.remove.v1", "graph.create.v1", "graph.remove.v1", "graph.rename.v1",
          "graph.function.flags.set.v1", "graph.finalize.v1", "graph.node.pin_type.set.v1",
          *(f"blueprint.member.{name}.v1" for name in (
              "query", "interface.add", "interface.remove", "variable.set", "variable.remove", "property.set",
              "dispatcher.set", "dispatcher.remove", "rename")))
    + _rows("seq_profile", 2, "editor.scratch.clone_asset.v2", "editor.asset.create_by_factory.v2"))

# Validate's trial apply (registered_runtime.TRIAL_TARGET), declared by the registered route itself.
VALIDATE_TRIAL_REQUIREMENTS = frozenset(_rows("route.generic.v1", 1, "editor.scratch.trial_target.v1"))
# The generic profile's Enhanced Input warnings (regions/enhanced_input_checks.py).
ENHANCED_INPUT_REQUIREMENTS = frozenset(_rows("route.generic.v1", 1, "editor.enhanced_input.check_domain.v1"))


class RequiredPrimitiveDeclarationTests(unittest.TestCase):
    def test_declared_set_matches_the_retired_cpp_seed(self) -> None:
        self.assertEqual(len(LEGACY_CPP_REQUIREMENTS), 45)
        declared = {(row.primitive_id, row.version, row.required_by_route_id)
                    for row in bootstrap.REQUIRED_PRIMITIVES}
        self.assertEqual(declared, LEGACY_CPP_REQUIREMENTS | UBLUEPRINT_REQUIREMENTS
                         | MONTAGE_SETTINGS_REQUIREMENTS | MONTAGE_CREATE_REQUIREMENTS | ANIM_SEQUENCE_REQUIREMENTS
                         | WIDGET_BLUEPRINT_REQUIREMENTS | ANIM_BLUEPRINT_REQUIREMENTS | DATA_TABLE_REQUIREMENTS
                         | ANIMATION_ASSET_REQUIREMENTS | STATE_TREE_REQUIREMENTS | ENV_QUERY_REQUIREMENTS | NIAGARA_REQUIREMENTS
                         | AUDIO_REQUIREMENTS | RIG_REQUIREMENTS | PHYSICS_REQUIREMENTS | SEQ_REQUIREMENTS
                         | VALIDATE_TRIAL_REQUIREMENTS | ENHANCED_INPUT_REQUIREMENTS)

    def test_wire_rows_are_exact_sorted_and_unique(self) -> None:
        wire = bootstrap.dispatch.describe_required_primitives()
        self.assertIs(type(wire), tuple)
        self.assertEqual(len(wire), 45 + len(ENHANCED_INPUT_REQUIREMENTS) + len(UBLUEPRINT_REQUIREMENTS)
                         + len(MONTAGE_SETTINGS_REQUIREMENTS) + len(MONTAGE_CREATE_REQUIREMENTS)
                         + len(ANIM_SEQUENCE_REQUIREMENTS) + len(WIDGET_BLUEPRINT_REQUIREMENTS)
                         + len(ANIM_BLUEPRINT_REQUIREMENTS) + len(DATA_TABLE_REQUIREMENTS)
                         + len(ANIMATION_ASSET_REQUIREMENTS) + len(STATE_TREE_REQUIREMENTS) + len(ENV_QUERY_REQUIREMENTS)
                         + len(NIAGARA_REQUIREMENTS) + len(AUDIO_REQUIREMENTS) + len(RIG_REQUIREMENTS)
                         + len(PHYSICS_REQUIREMENTS) + len(SEQ_REQUIREMENTS) + len(VALIDATE_TRIAL_REQUIREMENTS))
        for row in wire:
            self.assertIs(type(row), dict)
            self.assertEqual(tuple(row), ("PrimitiveId", "Version", "RequiredByRouteId"))
            self.assertIs(type(row["Version"]), int)
        keys = [(row["PrimitiveId"], row["Version"], row["RequiredByRouteId"]) for row in wire]
        self.assertEqual(keys, sorted(set(keys)))
        self.assertEqual(wire, bootstrap.dispatch.describe_required_primitives())

    def test_behavior_tree_selector_is_declared_by_its_profile(self) -> None:
        self.assertIn(behavior_tree_execution.SELECTOR_REQUIREMENT, BEHAVIOR_TREE_PROFILE_META.required_primitives)
        self.assertIn(behavior_tree_execution.REFRESH_REQUIREMENT, BEHAVIOR_TREE_PROFILE_META.required_primitives)

    def test_new_profile_requirements_flow_into_the_declared_set(self) -> None:
        extra = PrimitiveRequirement("editor.example.new_profile_only.v1", 1, "route.example.v1")
        profile = ProfileMeta("profile.example.v1", "route.example.v1", "/Script/Example.Asset", (), (extra,))
        builder = ProfileRegistryBuilder()
        builder.register(profile)
        declared = bootstrap.declared_required_primitives(builder.freeze())
        self.assertIn(extra, declared)
        self.assertTrue(set(GENERIC_REQUIRED_PRIMITIVES) <= set(declared))
        self.assertEqual(declared, tuple(sorted(set(declared))))


if __name__ == "__main__":
    unittest.main()
