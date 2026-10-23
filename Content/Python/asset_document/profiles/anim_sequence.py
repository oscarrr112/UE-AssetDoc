# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Registered AnimSequence Profile (profile.anim_sequence.v1): read, create and update exact AnimSequences.

Surfaces and their owners (docs/architecture/asset-document-anim-sequence-matrix.md):

* plain reflected fields (Body settings fields and Properties) -> the generic Editor
  property batch (``editor.property.*_admitted_batch.v1``);
* frame data, curves, notify tracks, notifies, notify states, sync markers, preview
  mesh, hard object references, metadata and asset user data -> one
  ``animation.asset.authoring.edit.v1`` call (provider "animation");
* Extract / Inspect / Diff read ``animation.asset.authoring.snapshot.v1``.

Writes are covered by Core's package snapshot (existing target) or the factory
rollback (created target); preflight therefore returns ``SNAPSHOT_COVERED``.
"""
from __future__ import annotations

import re

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.diagnostics import contract_error, primitive_error
from asset_document.diff.canonical_diff import canonical_diff
from asset_document.diff.model import Difference, DiffKind
from asset_document.dto import PrimitiveRequirement
from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, DocumentAction, IdentityPolicy, OmissionPolicy, OrderPolicy,
    ProfileMeta, RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document.profiles import anim_sequence_codec as codec
from asset_document.regions.primitive_catalog import (
    NamedOpaqueHandleArgument, PrimitiveOutcome, PrimitiveOutcomeStatus, _optional_handle,
)
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements
from asset_document.read_only_extract import admits_read_only

ANIM_SEQUENCE_CLASS = codec.CLASS
ROUTE = "animation_profile"
PROFILE_ID = "profile.anim_sequence.v1"
CLASS_RESOLVE = "editor.class.resolve_canonical.v1"
RESOLVE = "editor.asset.resolve_existing.v1"
EDITOR_ENUMERATE = "editor.property.enumerate_admitted_batch.v1"
EDITOR_WRITE = "editor.property.write_admitted_batch.v1"
SNAPSHOT = "animation.asset.authoring.snapshot.v1"
EDIT = "animation.asset.authoring.edit.v1"
# The shared Editor-factory lifecycle; the Skeleton is the factory context
# (UAnimSequenceFactory::TargetSkeleton), as the New Animation Sequence dialog sets it.
LIFECYCLE = generic_factory_lifecycle(ROUTE, ("factory_context",))
REQUIREMENTS = (tuple(PrimitiveRequirement(item, 1, ROUTE)
                      for item in (CLASS_RESOLVE, RESOLVE, EDITOR_ENUMERATE, EDITOR_WRITE, SNAPSHOT, EDIT))
                + lifecycle_requirements(LIFECYCLE))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}
_TARGET = re.compile(r"/Game/[A-Za-z0-9_/-]+\Z")
_DOCUMENT_KEYS = ("SchemaVersion", "Target", "Class", "Action", "Definitions", "Properties", "Body")


def _error(path, message, code="AnimSequence.InvalidDocument"):
    return contract_error(path, code, message)


def _require_target(value):
    if admits_read_only(value):
        return value
    if type(value) is not str or _TARGET.fullmatch(value) is None:
        raise _error("/Target", "Target must be a canonical /Game package path such as /Game/Anims/Run.",
                     "AnimSequence.NonCanonicalTarget")
    return value


def _snapshot(value):
    document_codec = DocumentCodec()
    return document_codec.decode(document_codec.encode(value))


def _escape(name):
    return name.replace("~", "~0").replace("/", "~1")


# ---------------------------------------------------------------------------
# Document validation
# ---------------------------------------------------------------------------
def _validate_header(document):
    if type(document) is not dict:
        raise _error("/", "An AnimSequence document must be an object.", "AnimSequence.InvalidDocumentType")
    for key in document:
        if key not in _DOCUMENT_KEYS:
            raise _error(f"/{_escape(key)}", f"Unknown document key '{key}'.", "AnimSequence.UnknownDocumentKey")
    if type(document.get("SchemaVersion")) is not int or document["SchemaVersion"] != 1:
        raise _error("/SchemaVersion", "SchemaVersion must be 1.", "Meta.InvalidSchemaVersion")
    _require_target(document.get("Target"))
    if document.get("Class") != ANIM_SEQUENCE_CLASS:
        raise _error("/Class", f"Class must be {ANIM_SEQUENCE_CLASS}.", "AnimSequence.ClassMismatch")
    if document.get("Action") not in {item.value for item in DocumentAction}:
        raise _error("/Action", "Action must be Create, Update or CreateOrUpdate.", "AnimSequence.InvalidAction")
    if document.get("Definitions", {}) != {}:
        raise _error("/Definitions", "Definitions are not authored by the AnimSequence Profile.",
                     "AnimSequence.UnsupportedDefinitions")
    for key in ("Properties", "Body"):
        if type(document.get(key, {})) is not dict:
            raise _error(f"/{key}", f"{key} must be an object.", f"AnimSequence.Invalid{key}Type")


def canonicalize(document):
    """Validate the whole document; returns a detached copy in the authored spelling."""
    _validate_header(document)
    DocumentCodec().validate(document)
    codec.canonical_body(document.get("Body", {}))
    codec.validate_properties(document.get("Properties", {}))
    return _snapshot(document)


# ---------------------------------------------------------------------------
# Primitive transport
# ---------------------------------------------------------------------------
_INTENT_REGIONS = {
    "skeleton": "/Body/References/Skeleton", "preview_mesh": "/Body/Preview/PreviewMesh",
    "frame_rate": "/Body/Playback/FrameRate", "number_of_frames": "/Body/Playback/NumberOfFrames",
    "curves": "/Body/Curves", "notify_tracks": "/Body/NotifyTracks", "notifies": "/Body/Notifies",
    "notify_states": "/Body/NotifyStates", "sync_markers": "/Body/SyncMarkers", "metadata": "/Body/Metadata",
    "asset_user_data": "/Body/AssetUserData",
}
_INTENT_FIELDS = {"time": "Time", "duration": "Duration", "track": "Track", "class": "Class", "name": "Name",
                  "properties": "Properties", "keys": "Keys", "flags": "Flags", "color": "Color", "comment": "Comment"}


def region_path(name):
    """The document path that authors one reflected field."""
    for region, fields in codec.REGION_FIELDS.items():
        if name in fields:
            return f"/Body/{region}/{name}"
    return f"/Properties/{_escape(name)}"


def document_path(primitive_path):
    """Map an authoring-intent path (/notifies/0/properties/X) to the document path the AI wrote."""
    parts = [part for part in (primitive_path or "").split("/") if part]
    if not parts:
        return "/Body"
    if parts[0] == "object_properties":
        return region_path(parts[1]) if len(parts) > 1 else "/Body"
    base = _INTENT_REGIONS.get(parts[0])
    if base is None:
        return "/Body"
    out = [base]
    rest = parts[1:]
    while rest:
        part = rest.pop(0)
        if part == "trigger":
            continue
        if part == "properties":
            out.append("Properties")
            out.extend(rest)
            break
        out.append(_INTENT_FIELDS.get(part, part))
    return "/".join(out)


def _invoke(context, primitive, values, handles=(), path=None, paths=None):
    outcome = context.invoke(requirement=BY_ID[primitive], value_arguments=values, handle_arguments=handles)
    if type(outcome) is not PrimitiveOutcome:
        raise _error("/PrimitiveOutcome", "A typed primitive outcome is required.", "AnimSequence.InvalidPrimitiveResult")
    if outcome.status is not PrimitiveOutcomeStatus.SUCCEEDED:
        if outcome.diagnostics:
            source = outcome.diagnostics[0]
            where = path if path is not None else (paths(source.path) if paths else "/Body")
            # Report the document location the AI wrote; the primitive detail goes to the diagnostic context.
            raise primitive_error(where, source, primitive)
        raise _error(path or "/Body", f"{primitive} did not succeed.", "AnimSequence.PrimitiveFailed")
    return outcome


def _animation(context, handle, access="read_only"):
    return context.handoff(source_handle=handle, target_provider_id="animation", access=access)


def read_snapshot(context, editor_asset):
    outcome = _invoke(context, SNAPSHOT, {}, (NamedOpaqueHandleArgument("asset", _animation(context, editor_asset)),),
                      path="/Body")
    if outcome.changed_set or outcome.handle_results or type(outcome.value) is not dict:
        raise _error("/PrimitiveOutcome", "The authoring snapshot must be a read-only object.", "AnimSequence.InvalidSnapshot")
    return outcome.value


def _resolve(context, path, expected_class, document_path_):
    outcome = _invoke(context, RESOLVE, {"path": codec.object_path(path, document_path_, nullable=False),
                                         "expected_class": expected_class}, path=document_path_)
    if type(outcome.value) is not dict or type(outcome.value.get("exists")) is not bool:
        raise _error("/PrimitiveOutcome", "Resolver existence evidence is missing.", "AnimSequence.InvalidPrimitiveResult")
    return outcome.value["exists"], _optional_handle(outcome, "asset_read")


def _admitted_batch(context, read):
    outcome = _invoke(context, EDITOR_ENUMERATE, {}, (NamedOpaqueHandleArgument("asset", read),), path="/Properties")
    rows = outcome.value.get("properties") if type(outcome.value) is dict else None
    groups = [group for group in outcome.handle_results if getattr(group, "name", None) == "property_batch"]
    if type(rows) is not list or len(groups) != 1 or len(groups[0].handles) != 1:
        raise _error("/PrimitiveOutcome", "Property batch enumeration must return rows and one property_batch handle.",
                     "AnimSequence.InvalidPrimitiveResult")
    return rows, groups[0].handles[0]


def editor_rows(fields, admitted):
    """Authored values in native admission order; integral numbers coerced for number codecs."""
    rows = []
    for row in admitted:
        name = row.get("name") if type(row) is dict else None
        if name in fields:
            value = fields[name]
            if row.get("codec_id") == "number.v1" and type(value) is int:
                value = float(value)
            rows.append({"name": name, "value": value})
    return rows


# ---------------------------------------------------------------------------
# Phases
# ---------------------------------------------------------------------------
def create_target_context(document, context):
    skeleton = codec.settings(document.get("Body", {})).get("References", {}).get("Skeleton")
    if skeleton is None:
        raise _error("/Body/References/Skeleton", "Creating an AnimSequence requires Body.References.Skeleton.",
                     "AnimSequence.SkeletonRequired")
    exists, read = _resolve(context, skeleton, "/Script/Engine.Skeleton", "/Body/References/Skeleton")
    if not exists or read is None:
        raise _error("/Body/References/Skeleton", f"Skeleton '{skeleton}' does not exist.",
                     "AnimSequence.ReferenceUnavailable")
    return (NamedOpaqueHandleArgument("factory_context", read),)


def preflight(document, context):
    """Checks that need the live target; every write is snapshot-covered."""
    canonical = codec.canonical_body(document.get("Body", {}))
    if "BoneTracks" not in canonical:
        return SNAPSHOT_COVERED
    exists, read = _resolve(context, document["Target"], ANIM_SEQUENCE_CLASS, "/Target")
    live = read_snapshot(context, read)["bone_tracks"] if exists else []
    if canonical["BoneTracks"] != live:
        raise _error("/Body/BoneTracks", "BoneTracks is read-only (bone animation data comes from an import); "
                     "omit it or give the current bone list.", "AnimSequence.ReadOnlyRegion")
    return SNAPSHOT_COVERED


def _plain_fields_authored(canonical, properties):
    return bool(properties) or any(name in codec.PLAIN_FIELDS.get(region, ())
                                   for region, values in canonical.items() if region in codec.SETTINGS_REGIONS
                                   for name in values)


def apply(document, read, mutable, context):
    canonical = codec.canonical_body(document.get("Body", {}))
    properties = codec.validate_properties(document.get("Properties", {}))
    admitted, batch, writable = [], None, set()
    if _plain_fields_authored(canonical, properties):
        admitted, batch = _admitted_batch(context, read)
        writable = {row.get("name") for row in admitted if type(row) is dict and row.get("writable") is True}
    intent, fields = codec.lower(canonical, properties, writable)
    for name in fields:
        if name not in writable:
            raise _error(region_path(name), f"'{name}' is not an admitted writable AnimSequence property.",
                         "AnimSequence.UnknownProperty")
    changes = []
    # Plain fields first, then timeline and references, as a human edits the
    # details panel: UAnimSequence clears RefPoseSeq unless the additive type and
    # base pose type that show it are already set.
    if fields:
        rows = editor_rows(fields, admitted)
        names = [row["name"] for row in rows]

        def field_path(primitive_path):
            match = re.match(r"/properties/(\d+)", primitive_path or "")
            index = int(match.group(1)) if match else -1
            return region_path(names[index]) if 0 <= index < len(names) else "/Properties"
        changes.extend(_invoke(context, EDITOR_WRITE, {"properties": rows}, (
            NamedOpaqueHandleArgument("asset", mutable), NamedOpaqueHandleArgument("property_batch", batch)),
            paths=field_path).changed_set)
    if intent:
        target = _animation(context, mutable, "mutable")
        changes.extend(_invoke(context, EDIT, intent, (NamedOpaqueHandleArgument("asset", target),),
                               paths=document_path).changed_set)
    return tuple(changes)


def extract(target, read, context):
    body, properties = codec.snapshot_to_document_parts(read_snapshot(context, read))
    return {"SchemaVersion": 1, "Target": target, "Class": ANIM_SEQUENCE_CLASS,
            "Action": DocumentAction.CREATE_OR_UPDATE.value, "Definitions": {}, "Properties": properties,
            "Body": body}


def diff(authored, extracted):
    """Every region and field the document states must hold on the live asset (omitted = not compared)."""
    headers = [{key: value for key, value in document.items() if key in ("SchemaVersion", "Target", "Class")}
               for document in (authored, extracted)]
    changes = list(canonical_diff(*headers))
    body, live = authored.get("Body", {}), extracted.get("Body", {})
    expected, actual = codec.canonical_body(body), codec.canonical_body(live)
    for region in codec.BODY_KEYS:
        if region not in expected:
            continue
        if region in codec.SETTINGS_REGIONS:
            have = actual.get(region, {})
            for name, value in expected[region].items():
                if name not in have or not codec.region_matches(region, {name: value}, {name: have[name]}, expected, actual):
                    changes.append(Difference(f"/Body/{region}/{name}", DiffKind.REPLACE, True, body[region][name],
                                              name in live.get(region, {}), live.get(region, {}).get(name)))
            continue
        if region not in actual or not codec.region_matches(region, expected[region], actual[region], expected, actual):
            changes.append(Difference(f"/Body/{region}", DiffKind.REPLACE, True, body[region],
                                      region in live, live.get(region)))
    authored_properties, live_properties = authored.get("Properties", {}), extracted.get("Properties", {})
    for name, value in authored_properties.items():
        if name not in live_properties or not codec.properties_match({name: value}, {name: live_properties[name]}):
            changes.append(Difference(f"/Properties/{_escape(name)}", DiffKind.REPLACE, True, value,
                                      name in live_properties, live_properties.get(name)))
    return tuple(changes)


# ---------------------------------------------------------------------------
# Handler
# ---------------------------------------------------------------------------
def _template_body():
    return {"References": {}, "Preview": {}, "Playback": {"NumberOfFrames": 30, "FrameRate": 30},
            "Additive": {}, "RootMotion": {}, "Compression": {}, "Curves": [], "NotifyTracks": [{"Name": "1"}],
            "Notifies": [], "NotifyStates": [], "SyncMarkers": [], "Metadata": [], "AssetUserData": []}


class AnimSequenceRuntimeProfile:
    profile_id = PROFILE_ID
    exact_asset_class_path = ANIM_SEQUENCE_CLASS
    runtime_entry_module = "asset_document/bootstrap.py"
    # Snapshot-covered: preflight never reads a preview, so the runtime skips the
    # scratch (a clone would duplicate and recompress the whole sequence).
    preflight_scratch = False

    def required_primitives(self):
        return REQUIREMENTS

    def inspect_profile(self):
        return {"Class": ANIM_SEQUENCE_CLASS, "BodySections": list(codec.BODY_KEYS),
                "ReadOnlySections": ["BoneTracks"]}

    def create_template(self, target):
        return {"SchemaVersion": 1, "Target": _require_target(target), "Class": ANIM_SEQUENCE_CLASS,
                "Action": DocumentAction.CREATE_OR_UPDATE.value, "Definitions": {}, "Properties": {},
                "Body": _template_body()}

    def validate(self, document):
        canonicalize(document)

    def canonicalize(self, document):
        return canonicalize(document)

    def schema(self):
        return {"SchemaVersion": 1, "Class": ANIM_SEQUENCE_CLASS, "Type": "object",
                "Required": ["SchemaVersion", "Target", "Class", "Action"],
                "Properties": {"Body": {"type": "object", "keys": list(codec.BODY_KEYS),
                                        "settings": {region: list(fields) for region, fields in codec.REGION_FIELDS.items()},
                                        "read_only": ["BoneTracks"]}}}

    def create_target_context(self, document, context):
        return create_target_context(canonicalize(document), context)

    def preflight(self, document, scratch_handle, context):
        return preflight(canonicalize(document), context)

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        return apply(canonicalize(document), read_asset_handle, mutable_asset_handle, context)

    def repair(self, document, asset_handle, context):
        canonicalize(document)
        # The authoring primitive and the Editor batch own completion (cache refresh, PostEditChange).
        return ()

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        return extract(_require_target(target), asset_handle, context)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != ANIM_SEQUENCE_CLASS or editor_asset_handle is None:
            raise _error("/Inspect", "AnimSequence inspection reads a live AnimSequence asset.",
                         "AnimSequence.InspectNotAdmitted")
        return self.canonicalize(self.extract(target=exact_asset_object_path.rsplit(".", 1)[0],
                                              asset_handle=editor_asset_handle, context=context))

    def diff(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted))

    def diff_exact(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted))


def create_anim_sequence_runtime_handler():
    return AnimSequenceRuntimeProfile()


_REGION_ROWS = tuple((key, IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL)
                     for key in codec.SETTINGS_REGIONS) + (
    ("Curves", IdentityPolicy.STABLE_KEY, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
    ("NotifyTracks", IdentityPolicy.STABLE_KEY, OrderPolicy.PRESERVE, ComparisonPolicy.ORDERED),
    ("Notifies", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
    ("NotifyStates", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
    ("SyncMarkers", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
    ("Metadata", IdentityPolicy.POSITIONAL, OrderPolicy.PRESERVE, ComparisonPolicy.ORDERED),
    ("AssetUserData", IdentityPolicy.STABLE_KEY, OrderPolicy.PRESERVE, ComparisonPolicy.ORDERED),
)


def _region_name(key):
    return re.sub(r"(?<!^)(?=[A-Z])", "_", key).lower()


ANIM_SEQUENCE_PROFILE_META = ProfileMeta(
    PROFILE_ID, ROUTE, ANIM_SEQUENCE_CLASS,
    tuple(RegionMeta(f"anim_sequence.{_region_name(key)}.v1", f"/Body/{key}", RegionOwnership.MANAGED, identity, order,
                     DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE, DeletePolicy.EXPLICIT_ONLY, comparison)
          for key, identity, order, comparison in _REGION_ROWS),
    REQUIREMENTS)
ANIM_SEQUENCE_RUNTIME_REGISTRATION = RuntimeProfileRegistration(
    ANIM_SEQUENCE_PROFILE_META, create_anim_sequence_runtime_handler, RuntimeTargetPolicy.CREATE_OR_UPDATE, LIFECYCLE)

__all__ = ("ANIM_SEQUENCE_RUNTIME_REGISTRATION", "AnimSequenceRuntimeProfile", "ANIM_SEQUENCE_CLASS", "PROFILE_ID")
