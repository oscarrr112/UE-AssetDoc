from __future__ import annotations

from asset_document.profiles.blackboard_data import (
    BLACKBOARD_DATA_PROFILE_META,
    create_blackboard_data_profile,
)
from asset_document.profiles.behavior_tree_production import (
    BEHAVIOR_TREE_PROFILE_META,
    create_behavior_tree_profile,
)
from asset_document.runtime import (
    configure_profile_registry,
    _clear_behavior_tree_expected_for_terminal,
)
from asset_document.registry import FrozenProfileRegistry, ProfileRegistryBuilder
from asset_document.registered_runtime import TRIAL_TARGET, build_dispatch
from asset_document.regions.primitive_catalog import GENERIC_REQUIRED_PRIMITIVES
from asset_document.profiles.anim_montage import ANIM_MONTAGE_RUNTIME_REGISTRATION
from asset_document.profiles.anim_sequence import ANIM_SEQUENCE_RUNTIME_REGISTRATION
from asset_document.profiles.ublueprint_runtime import UBLUEPRINT_RUNTIME_REGISTRATION
from asset_document.profiles.anim_blueprint_runtime import ANIM_BLUEPRINT_RUNTIME_REGISTRATION
from asset_document.profiles.widget_blueprint_runtime import WIDGET_BLUEPRINT_RUNTIME_REGISTRATION
from asset_document.profiles.data_table import (
    COMPOSITE_DATA_TABLE_RUNTIME_REGISTRATION, DATA_TABLE_RUNTIME_REGISTRATION, MIRROR_DATA_TABLE_RUNTIME_REGISTRATION,
)
from asset_document.profiles.curve_table import (
    COMPOSITE_CURVE_TABLE_RUNTIME_REGISTRATION, CURVE_TABLE_RUNTIME_REGISTRATION,
)
from asset_document.profiles.string_table import STRING_TABLE_RUNTIME_REGISTRATION
from asset_document.profiles.user_defined_enum import USER_DEFINED_ENUM_RUNTIME_REGISTRATION
from asset_document.profiles.user_defined_struct import USER_DEFINED_STRUCT_RUNTIME_REGISTRATION
from asset_document.profiles.anim_composite import ANIM_COMPOSITE_RUNTIME_REGISTRATION
from asset_document.profiles.blend_space import BLEND_SPACE_RUNTIME_REGISTRATIONS
from asset_document.profiles.material_instance import MATERIAL_INSTANCE_RUNTIME_REGISTRATION
from asset_document.profiles.pose_asset import POSE_ASSET_RUNTIME_REGISTRATION
from asset_document.profiles.skeleton import SKELETON_RUNTIME_REGISTRATION
from asset_document.profiles.state_tree import STATE_TREE_RUNTIME_REGISTRATION
from asset_document.profiles.env_query import ENV_QUERY_RUNTIME_REGISTRATION
from asset_document.profiles.niagara import NIAGARA_RUNTIME_REGISTRATIONS
from asset_document.profiles.material import (
    MATERIAL_FUNCTION_RUNTIME_REGISTRATION, MATERIAL_LAYER_BLEND_RUNTIME_REGISTRATION, MATERIAL_LAYER_RUNTIME_REGISTRATION,
    MATERIAL_RUNTIME_REGISTRATION,
)
from asset_document.profiles.control_rig import CONTROL_RIG_RUNTIME_REGISTRATION
from asset_document.profiles.ik_retargeter import IK_RETARGETER_RUNTIME_REGISTRATION
from asset_document.profiles.ik_rig import IK_RIG_RUNTIME_REGISTRATION
from asset_document.profiles.physics_asset import PHYSICS_ASSET_RUNTIME_REGISTRATION
from asset_document.profiles.level_sequence import LEVEL_SEQUENCE_RUNTIME_REGISTRATION
from asset_document.profiles.template_sequence import (
    CAMERA_ANIMATION_SEQUENCE_RUNTIME_REGISTRATION, TEMPLATE_SEQUENCE_RUNTIME_REGISTRATION,
)
from asset_document.profiles.metasound import (
    METASOUND_PATCH_RUNTIME_REGISTRATION, METASOUND_SOURCE_RUNTIME_REGISTRATION,
)
from asset_document.profiles.sound_cue import SOUND_CUE_RUNTIME_REGISTRATION


_PROFILE_BINDINGS = (
    (BLACKBOARD_DATA_PROFILE_META, create_blackboard_data_profile),
    (BEHAVIOR_TREE_PROFILE_META, create_behavior_tree_profile),
)
configure_profile_registry(_PROFILE_BINDINGS)
_builder = ProfileRegistryBuilder()
_builder.register(BLACKBOARD_DATA_PROFILE_META)
_builder.register(BEHAVIOR_TREE_PROFILE_META)
_builder.register_runtime(ANIM_MONTAGE_RUNTIME_REGISTRATION)
_builder.register_runtime(ANIM_SEQUENCE_RUNTIME_REGISTRATION)
_builder.register_runtime(UBLUEPRINT_RUNTIME_REGISTRATION)
_builder.register_runtime(ANIM_BLUEPRINT_RUNTIME_REGISTRATION)
_builder.register_runtime(WIDGET_BLUEPRINT_RUNTIME_REGISTRATION)
for _registration in (DATA_TABLE_RUNTIME_REGISTRATION, COMPOSITE_DATA_TABLE_RUNTIME_REGISTRATION,
                      MIRROR_DATA_TABLE_RUNTIME_REGISTRATION, CURVE_TABLE_RUNTIME_REGISTRATION,
                      COMPOSITE_CURVE_TABLE_RUNTIME_REGISTRATION, STRING_TABLE_RUNTIME_REGISTRATION,
                      USER_DEFINED_STRUCT_RUNTIME_REGISTRATION, USER_DEFINED_ENUM_RUNTIME_REGISTRATION,
                      SKELETON_RUNTIME_REGISTRATION, *BLEND_SPACE_RUNTIME_REGISTRATIONS, POSE_ASSET_RUNTIME_REGISTRATION,
                      ANIM_COMPOSITE_RUNTIME_REGISTRATION, STATE_TREE_RUNTIME_REGISTRATION,
                      ENV_QUERY_RUNTIME_REGISTRATION):
    _builder.register_runtime(_registration)
_builder.register_runtime(MATERIAL_INSTANCE_RUNTIME_REGISTRATION)
for _registration in (MATERIAL_RUNTIME_REGISTRATION, MATERIAL_FUNCTION_RUNTIME_REGISTRATION,
                      MATERIAL_LAYER_RUNTIME_REGISTRATION, MATERIAL_LAYER_BLEND_RUNTIME_REGISTRATION,
                      *NIAGARA_RUNTIME_REGISTRATIONS):
    _builder.register_runtime(_registration)
for _registration in (METASOUND_SOURCE_RUNTIME_REGISTRATION, METASOUND_PATCH_RUNTIME_REGISTRATION,
                      SOUND_CUE_RUNTIME_REGISTRATION):
    _builder.register_runtime(_registration)
for _registration in (IK_RIG_RUNTIME_REGISTRATION, IK_RETARGETER_RUNTIME_REGISTRATION,
                      CONTROL_RIG_RUNTIME_REGISTRATION, PHYSICS_ASSET_RUNTIME_REGISTRATION,
                      LEVEL_SEQUENCE_RUNTIME_REGISTRATION, TEMPLATE_SEQUENCE_RUNTIME_REGISTRATION,
                      CAMERA_ANIMATION_SEQUENCE_RUNTIME_REGISTRATION):
    _builder.register_runtime(_registration)
PROFILE_REGISTRY = _builder.freeze()
_registered_dispatch = build_dispatch(PROFILE_REGISTRY)


def declared_required_primitives(registry: FrozenProfileRegistry):
    """Sorted unique (primitive, version, route) rows declared by Profiles and the generic route."""
    # TRIAL_TARGET: Validate's trial apply of Profiles that check while they edit (registered_runtime).
    rows = set(registry.required_primitives()) | set(GENERIC_REQUIRED_PRIMITIVES) | {TRIAL_TARGET}
    for binding in registry.runtime_registrations():
        rows.update(binding.profile_meta.required_primitives)
    return tuple(sorted(rows))


REQUIRED_PRIMITIVES = declared_required_primitives(PROFILE_REGISTRY)


def describe_required_primitives():
    # Native wire read once at revision activation; C++ freezes the registry from it.
    return tuple(
        {'PrimitiveId': item.primitive_id, 'Version': item.version,
         'RequiredByRouteId': item.required_by_route_id}
        for item in REQUIRED_PRIMITIVES
    )


def dispatch(request, context):
    return _registered_dispatch(request, context)


dispatch.describe_runtime_registrations = _registered_dispatch.describe_runtime_registrations
dispatch.describe_required_primitives = describe_required_primitives
dispatch._clear_behavior_tree_expected_for_terminal = _clear_behavior_tree_expected_for_terminal

__all__ = ("dispatch",)
