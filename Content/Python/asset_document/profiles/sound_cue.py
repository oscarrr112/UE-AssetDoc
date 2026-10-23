# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Registered SoundCue Profile (profile.sound_cue.v1) on the ``audio_profile`` route.

The cue's editor graph is read and edited through the shared graph primitives (the SoundCue schema and host
adapters of AssetDocumentAudio); ``audio.soundcue.read.v1`` adds what the graph does not hold (validation status,
derived values, waves, the defaults of a new node).  Document shape and Diff: ``sound_cue_model``; Apply:
``sound_cue_apply``.  Create goes through the editor's New Sound Cue factory (an empty cue with its Output node).
"""
from __future__ import annotations

from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, IdentityPolicy, OmissionPolicy, OrderPolicy, ProfileMeta,
    RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document.profiles import data_table_common as common
from asset_document.profiles import sound_cue_model as model
from asset_document.profiles.sound_cue_model import CLASS, PREFIX, error


class _SoundCueProperties(common.PropertiesRegion):
    """The cue's Details panel (generic property admission); the graph and derived values are not Properties."""

    def check_owned(self, properties):
        for name in sorted(properties):
            if name in model.NOT_AUTHORED:
                owner = model.NOT_AUTHORED[name]
                raise error(f"/Properties/{common.escape(name)}", (
                    f"'{name}' is authored as {owner}." if owner else
                    f"'{name}' is derived by the editor from the graph (recomputed on every change); leave it out."),
                    f"{PREFIX}.NotAuthoredProperty")
        super().check_owned(properties)

    def skipped(self):
        return super().skipped() | set(model.NOT_AUTHORED)


def properties():
    return _SoundCueProperties(CLASS, {}, PREFIX)


def canonicalize(document):
    body = common.validate_header(document, (CLASS,), PREFIX, model.BODY_KEYS)
    canonical = common.snapshot(document)
    canonical["Body"] = model.check_body(body)
    properties().check_owned(canonical.get("Properties", {}))
    if "Nodes" in canonical["Body"]:
        model.check_references(canonical["Body"])
    return canonical


def extract(target, read, context, include_defaults=True):
    calls = model.Calls(context, read)
    _, body = model.read_live(calls)
    return common.document(target, CLASS, properties().extract(read, context, include_defaults), body)


def diff(authored, extracted, exact):
    changes = list(common.header_diff(authored, extracted))
    changes += common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact)
    changes += model.body_diff(authored.get("Body", {}), extracted.get("Body", {}), exact)
    return tuple(changes)


def inspect_payload(document, calls, live):
    """The extract plus what the Sound Cue editor shows around the graph."""
    state = calls.read()
    facts = {}
    for key, node in live.nodes.items():
        limits = node.extras.get("child_limits", {})
        facts[key] = {"Class": node.extras.get("class", ""), "InputPins": list(node.extras.get("input_pins", ())),
                      "MinInputs": limits.get("min", 0), "MaxInputs": limits.get("max", 0),
                      "Reachable": node.extras.get("reachable") is True}
    messages = [{"Severity": str(item.get("severity", "")).title(), "Message": item.get("message", ""),
                 **({"Node": item["node"]} if item.get("node") else {}), **({"Path": item["path"]} if item.get("path") else {})}
                for item in state.get("messages", []) if type(item) is dict]
    waves = [{"Node": item.get("node"), "Wave": item.get("path"), **({"Duration": item["duration"]} if "duration" in item else {}),
              "Looping": item.get("looping") is True} for item in state.get("waves", []) if type(item) is dict]
    return {**document, "NodeFacts": facts, "Waves": waves,
            "Status": {"Status": state.get("status", "Unknown"), "Messages": messages},
            "Derived": dict(state.get("derived", {})), "QualityLevels": list(state.get("quality_levels", [])),
            "OpenInEditor": state.get("open_in_editor") is True}


class SoundCueRuntimeProfile:
    runtime_entry_module = "asset_document/bootstrap.py"
    preflight_scratch = False
    profile_id = model.PROFILE_ID
    exact_asset_class_path = CLASS

    def required_primitives(self):
        return model.REQUIREMENTS

    def inspect_profile(self):
        return {"Class": CLASS, "BodySections": list(model.BODY_KEYS), "ReadOnlySections": [], "ApplyAvailable": True}

    def create_template(self, target):
        # The New Sound Cue factory makes an empty cue: its Output node and nothing else.
        return common.document(common.require_target(target, PREFIX), CLASS, {},
                               {"Root": {"Input": None}, "Nodes": {}, "Comments": {}})

    def validate(self, document):
        canonicalize(document)

    def canonicalize(self, document):
        return canonicalize(document)

    def create_target_context(self, document, context):
        canonicalize(document)
        return ()

    def preflight(self, document, scratch_handle, context):
        canonical = canonicalize(document)
        properties().validate(canonical.get("Properties", {}), context)
        return SNAPSHOT_COVERED

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        return _apply.apply(canonicalize(document), read_asset_handle, mutable_asset_handle, context, properties())

    def repair(self, document, asset_handle, context):
        canonicalize(document)
        return ()

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        return extract(common.require_target(target, PREFIX), asset_handle, context, include_defaults)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != CLASS or editor_asset_handle is None:
            raise error("/Inspect", "SoundCue inspection reads a live Sound Cue.", f"{PREFIX}.InspectNotAdmitted")
        calls = model.Calls(context, editor_asset_handle)
        live, body = model.read_live(calls)
        document = common.document(exact_asset_object_path.rsplit(".", 1)[0], CLASS,
                                   properties().extract(editor_asset_handle, context), body)
        return inspect_payload(document, calls, live)

    def diff(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=False)

    def diff_exact(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=True)


def _meta():
    rows = (("root", "/Body/Root", IdentityPolicy.STABLE_KEY), ("nodes", "/Body/Nodes", IdentityPolicy.STABLE_KEY),
            ("comments", "/Body/Comments", IdentityPolicy.STABLE_KEY))
    regions = tuple(RegionMeta(f"sound_cue.{name}.v1", path, RegionOwnership.MANAGED, identity, OrderPolicy.PRESERVE,
                               DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE, DeletePolicy.EXPLICIT_ONLY,
                               ComparisonPolicy.CANONICAL)
                    for name, path, identity in rows)
    return ProfileMeta(model.PROFILE_ID, model.ROUTE, CLASS, regions, model.REQUIREMENTS)


# Module-level on purpose (see metasound.py): the native runtime restores sys.modules between phase calls.
from asset_document.profiles import sound_cue_apply as _apply  # noqa: E402

SOUND_CUE_RUNTIME_REGISTRATION = RuntimeProfileRegistration(_meta(), SoundCueRuntimeProfile,
                                                            RuntimeTargetPolicy.CREATE_OR_UPDATE, model.LIFECYCLE)

__all__ = ("SOUND_CUE_RUNTIME_REGISTRATION", "SoundCueRuntimeProfile", "canonicalize", "diff")
