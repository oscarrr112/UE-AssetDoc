# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from copy import deepcopy

from asset_document.dto import JsonValue
from asset_document.dto import RuntimePhase
from asset_document.regions.primitive_catalog import (
    NamedOpaqueHandleArgument,
    NamedOpaqueHandleResult,
    PrimitiveOutcome,
    PrimitiveOutcomeStatus,
)
from asset_document.diagnostics import RuntimeDiagnostic, contract_error


EXACT_CLASS = "/Script/Engine.AnimMontage"
ROUTE_ID = "animation_profile"
TARGET = "/Game/Animation/AM_Combat"

CANONICAL_BODY_KEYS = (
    "Skeleton",
    "PreviewMesh",
    "References",
    "Preview",
    "Sync",
    "RootMotion",
    "Metadata",
    "SectionMetadata",
    "TimeStretch",
    "Curves",
    "SlotAnimTracks",
    "CompositeSections",
    "Notifies",
    "NotifyStates",
    "NotifyTracks",
    "Blend",
)


def template_body() -> dict[str, JsonValue]:
    return {
        "Skeleton": None,
        "PreviewMesh": None,
        "References": {},
        "Preview": {},
        "Sync": {},
        "RootMotion": {},
        "Metadata": [],
        "SectionMetadata": {},
        "TimeStretch": {},
        "Curves": [],
        "SlotAnimTracks": [],
        "CompositeSections": [],
        "Notifies": [],
        "NotifyStates": [],
        "NotifyTracks": [{"Name": "1"}],
        "Blend": {},
    }


def document(body: dict[str, JsonValue] | None = None) -> dict[str, JsonValue]:
    return {
        "SchemaVersion": 1,
        "Target": TARGET,
        "Class": EXACT_CLASS,
        "Action": "CreateOrUpdate",
        "Definitions": {},
        "Properties": {},
        "Body": template_body() if body is None else body,
    }


def rich_document() -> dict[str, JsonValue]:
    body = template_body()
    body.update(
        {
            "Skeleton": "/Game/Animation/SK_Legacy",
            "PreviewMesh": "/Game/Animation/SK_PreviewLegacy",
            "References": {"Skeleton": "/Game/Animation/SK_Combat"},
            "Preview": {"PreviewMesh": "/Game/Animation/SK_CombatPreview"},
            "Sync": {"SyncGroup": "Combat", "SyncSlotIndex": 0},
            "SectionMetadata": {"AttackA": {"Tag": "Opening"}},
            "TimeStretch": {"Curve": "/Game/Animation/TS_Combat"},
            "Curves": [{"Name": "Speed", "Keys": []}],
            "SlotAnimTracks": [{"SlotName": "UpperBody", "Segments": []}],
            "CompositeSections": [
                {"Name": "AttackA", "NextSectionName": "AttackB"},
                {"Name": "AttackB", "NextSectionName": None},
            ],
            "Notifies": [{"Name": "Swing", "Track": "Default", "Time": 0.1}],
            "NotifyStates": [
                {"Name": "Invulnerable", "Track": "Default", "Time": 0.2}
            ],
            "Blend": {"BlendInTime": 0.1},
        }
    )
    return document(body)


class InvocationProbe:
    def __init__(self) -> None:
        self.calls = 0

    def invoke(self, *args: object, **kwargs: object) -> None:
        del args, kwargs
        self.calls += 1
        raise AssertionError("blocked AnimMontage apply must not invoke a primitive")


class AnimationReadOnlyContext:
    """Test-only port that enforces the frozen read-only animation contract."""

    def __init__(
        self,
        *,
        phase: RuntimePhase = RuntimePhase.VERIFY,
        provider_id: str = "animation",
        access: str = "ReadOnly",
        asset_class: str = EXACT_CLASS,
        stale: bool = False,
        rejection: RuntimeDiagnostic | None = None,
    ) -> None:
        self.phase = phase
        self.provider_id = provider_id
        self.access = access
        self.asset_class = asset_class
        self.stale = stale
        self.rejection = rejection
        self.asset_handle = object()
        self.calls: list[str] = []
        self.handler_calls = 0

    def handoff(self, *, source_handle, target_provider_id, access):
        if target_provider_id != "animation" or access != "read_only":
            _catalog_error("PrimitiveCatalog.InvalidHandleAccess")
        return source_handle

    def invoke(
        self,
        *,
        requirement: object,
        value_arguments: object,
        handle_arguments: tuple[NamedOpaqueHandleArgument, ...],
    ) -> PrimitiveOutcome:
        primitive_id = getattr(requirement, "primitive_id", None)
        self._validate(primitive_id, value_arguments, handle_arguments)
        self.calls.append(primitive_id)
        if self.rejection is not None:
            return PrimitiveOutcome(
                PrimitiveOutcomeStatus.FAILED,
                None,
                (),
                (),
                (self.rejection,),
            )
        self.handler_calls += 1
        return PrimitiveOutcome(
            PrimitiveOutcomeStatus.SUCCEEDED,
            _read_only_value(primitive_id),
            (),
            (),
            (),
        )

    def _validate(
        self,
        primitive_id: object,
        value_arguments: object,
        handle_arguments: tuple[NamedOpaqueHandleArgument, ...],
    ) -> None:
        if self.provider_id != "animation":
            _catalog_error("PrimitiveCatalog.ProviderMismatch")
        if self.stale:
            _catalog_error("PrimitiveCatalog.StaleOpaqueHandle")
        if self.phase not in (RuntimePhase.PREFLIGHT, RuntimePhase.VERIFY):
            _catalog_error("PrimitiveCatalog.InvalidPhase")
        if self.access != "ReadOnly":
            _catalog_error("PrimitiveCatalog.InvalidHandleAccess")
        if self.asset_class != EXACT_CLASS:
            _catalog_error("PrimitiveCatalog.WrongClass")
        if primitive_id not in READ_ONLY_PRIMITIVE_IDS:
            _catalog_error("PrimitiveCatalog.UnknownRequirement")
        if value_arguments != {} or len(handle_arguments) != 1:
            _catalog_error("PrimitiveCatalog.InvalidInvocation")
        argument = handle_arguments[0]
        if argument.name != "asset" or argument.handle is not self.asset_handle:
            _catalog_error("PrimitiveCatalog.InvalidOpaqueHandle")


READ_ONLY_PRIMITIVE_IDS = ("animation.montage.profile.snapshot.v1",)


def _catalog_error(code: str) -> None:
    raise contract_error("/HandleArguments", code, "read-only animation catalog rejected request")


def _read_only_value(primitive_id: str) -> dict[str, JsonValue]:
    assert primitive_id == READ_ONLY_PRIMITIVE_IDS[0]
    return native_snapshot()


def native_snapshot():
    """Independent observed fixture, never generated from authored input or codec."""
    return {
        "skeleton": "/Game/Animation/SK_Combat.SK_Combat", "preview_mesh": None,
        "curves": [{"name": "Speed", "flags": 4,
                    "color": {"r": {"bits": 1065353216}, "g": {"bits": 0},
                              "b": {"bits": 0}, "a": {"bits": 1065353216}},
                    "comment": "observed", "default_value": {"has_value": False, "float32_bits": None},
                    "pre_extrapolation": 0, "post_extrapolation": 0, "keys": []}],
        "slots": [{"name": "DefaultSlot", "segments": [{
            "animation": "/Game/Animation/AS_Attack.AS_Attack", "start": 0.0,
            "source_start": 0.0, "source_end": 1.0, "rate": 1.0, "loops": 1,
            "previous_slot": "DefaultSlot", "previous_segment": 0}]}],
        "sections": [{"name": "Start", "next": "None", "time": 0.0,
                      "slot_name": "DefaultSlot", "method": 0}],
        "notifies": [{"time": 0.25, "track_index": 0,
                      "object": {"class": "/Script/Engine.AnimNotify", "properties": {"NotifyColor": "red"}}}],
        "notify_states": [{"time": 0.5, "duration": 0.25, "track_index": 0,
                           "object": {"class": "/Script/Engine.AnimNotifyState", "properties": {}}}],
        "notify_tracks": [{"name": "1"}],
    }


def observed_body():
    """Literal expected canonical observation, independent of production codec."""
    return {
        "Skeleton": "/Game/Animation/SK_Combat.SK_Combat", "PreviewMesh": None,
        "Curves": [{"Name": "Speed", "Flags": 4,
                    "Color": {"R": {"Bits": 1065353216}, "G": {"Bits": 0},
                              "B": {"Bits": 0}, "A": {"Bits": 1065353216}},
                    "Comment": "observed", "DefaultValue": {"HasValue": False, "Float32Bits": None},
                    "PreExtrapolation": 0, "PostExtrapolation": 0, "Keys": []}],
        "SlotAnimTracks": [{"SlotName": "DefaultSlot", "Segments": [{
            "AnimReference": "/Game/Animation/AS_Attack.AS_Attack", "StartPos": 0.0,
            "AnimStartTime": 0.0, "AnimEndTime": 1.0, "AnimPlayRate": 1.0, "LoopingCount": 1}]}],
        "CompositeSections": [{"Name": "Start", "NextSectionName": None, "LinkableTime": 0.0,
                               "SlotName": "DefaultSlot", "LinkMethod": 0}],
        "Notifies": [{"Time": 0.25, "TrackIndex": 0,
                      "Object": {"Class": "/Script/Engine.AnimNotify", "Properties": {"NotifyColor": "red"}}}],
        "NotifyStates": [{"Time": 0.5, "Duration": 0.25, "TrackIndex": 0,
                           "Object": {"Class": "/Script/Engine.AnimNotifyState", "Properties": {}}}],
        "NotifyTracks": [{"Name": "1"}],
    }


def native_settings():
    """Independent observed native settings fixture (snapshot "settings" object)."""
    return {
        "blend_in": {"time": 0.25, "option": "Linear", "custom_curve": None},
        "blend_out": {"time": 0.5, "option": "Cubic", "custom_curve": None},
        "blend_mode_in": "Standard", "blend_mode_out": "Inertialization",
        "blend_out_trigger_time": -1.0, "enable_auto_blend_out": True,
        "blend_profile_in": None, "blend_profile_out": "Upper",
        "sync_group": "None", "sync_slot_index": 0,
        "root_motion": {"translation": True, "rotation": False, "root_lock": "RefPose"},
        "preview_base_pose": None,
        "time_stretch": {"curve_name": "Speed", "sampling_rate": 60.0, "precision": 0.25},
        "metadata": [{"class": "/Script/Mod.MetaA", "properties": {"Label": "a", "Count": 2}}],
        "section_metadata": [{"section": "Start", "metadata": []}],
        "properties": {"RateScale": 1.5, "bLoop": False, "SyncGroup": "None", "BlendModeIn": "Standard"},
    }


def observed_settings_body():
    """Literal canonical projection of native_settings(), independent of the codec."""
    return {
        "Preview": {"PreviewBasePose": None},
        "Blend": {
            "BlendInTime": 0.25, "BlendOutTime": 0.5, "BlendInOption": "Linear", "BlendOutOption": "Cubic",
            "BlendInCustomCurve": None, "BlendOutCustomCurve": None,
            "BlendModeIn": "Standard", "BlendModeOut": "Inertialization",
            "BlendOutTriggerTime": -1.0, "bEnableAutoBlendOut": True,
            "BlendProfileIn": None, "BlendProfileOut": "Upper",
        },
        "Sync": {"SyncGroup": "None", "SyncSlotIndex": 0},
        "RootMotion": {"bEnableRootMotionTranslation": True, "bEnableRootMotionRotation": False,
                       "RootMotionRootLock": "RefPose"},
        "TimeStretch": {"TimeStretchCurveName": "Speed", "SamplingRate": 60.0,
                        "CurveValueMinPrecision": 0.25},
        "Metadata": [{"Class": "/Script/Mod.MetaA", "Properties": {"Label": "a", "Count": 2}}],
        "SectionMetadata": {"Start": []},
    }


# Native admission rows of the Montage, in provider (case-insensitive name) order.
ADMITTED_ROWS = [
    {"name": name, "declaring_class": "/Script/Engine.AnimMontage", "codec_id": codec,
     "readable": True, "writable": True}
    for name, codec in (("bEnableAutoBlendOut", "bool.v1"), ("BlendModeIn", "enum.v1"),
                        ("BlendModeOut", "enum.v1"), ("BlendOutTriggerTime", "number.v1"),
                        ("bLoop", "bool.v1"), ("RateScale", "number.v1"), ("SyncGroup", "name.v1"),
                        ("SyncSlotIndex", "integer.v1"), ("TimeStretchCurveName", "name.v1"),
                        ("PreviewBasePose", "object_path.v1"))
]


class MontageContext(AnimationReadOnlyContext):
    """Typed transport double with fixed independent snapshot and handle results."""
    def __init__(self, *, phase=RuntimePhase.PREFLIGHT, exists=True, settings=False):
        super().__init__(phase=phase)
        self.exists = exists
        self.settings = settings
        self.batch_handle = object()
        self.invocations = []
        self.handoffs = []

    # Slots of every Skeleton in this double (the Anim Slot Manager's list).
    skeleton_slots = ("DefaultSlot", "UpperBody", "Other")

    def handoff(self, *, source_handle, target_provider_id, access):
        assert target_provider_id in ("animation", "animation_asset")
        assert access in ("read_only", "mutable")
        self.handoffs.append((source_handle, access))
        return source_handle

    def invoke(self, *, requirement, value_arguments, handle_arguments):
        primitive = requirement.primitive_id
        self.calls.append(primitive)
        self.invocations.append((primitive, deepcopy(value_arguments), handle_arguments))
        value, handles = None, ()
        if primitive == "editor.asset.resolve_existing.v1":
            present = self.exists if value_arguments["path"] == TARGET + ".AM_Combat" else True
            value = {"exists": present}
            handles = tuple(NamedOpaqueHandleResult(name, (self.asset_handle,) if present else ())
                            for name in ("asset_read", "asset_mutable"))
        elif primitive == "editor.class.resolve_canonical.v1":
            handles = (NamedOpaqueHandleResult("class", (self.asset_handle,)),)
        elif primitive == "animation.montage.profile.snapshot.v1":
            assert self.phase in (RuntimePhase.PREFLIGHT, RuntimePhase.VERIFY)
            value = native_snapshot()
            if self.settings:
                value["settings"] = self.settings if type(self.settings) is dict else native_settings()
        elif primitive == "animation.montage.structure.check.v1":
            # Validate's (and Apply's Preflight) read-only timeline check on the existing target.
            assert self.phase is RuntimePhase.PREFLIGHT and set(value_arguments) <= {
                "slots", "sections", "notifies", "notify_states", "notify_tracks"}, primitive
            value = {}
        elif primitive == "animation_asset.skeleton.read.v1":
            value = {"slot_groups": [{"name": "DefaultGroup", "slots": list(self.skeleton_slots)}]}
        elif primitive == "editor.property.enumerate_admitted.v1":
            assert value_arguments == {"exact_asset_class_path": "/Script/Engine.AnimMontage"}
            value = {"properties": deepcopy(ADMITTED_ROWS)}
            handles = (NamedOpaqueHandleResult("properties", tuple(object() for _ in ADMITTED_ROWS)),)
        elif primitive == "editor.property.enumerate_admitted_batch.v1":
            value = {"properties": deepcopy(ADMITTED_ROWS)}
            handles = (NamedOpaqueHandleResult("properties", tuple(object() for _ in ADMITTED_ROWS)),
                       NamedOpaqueHandleResult("property_batch", (self.batch_handle,)))
        else:
            assert self.phase is RuntimePhase.APPLY, primitive
        return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED, value, handles, (), ())
