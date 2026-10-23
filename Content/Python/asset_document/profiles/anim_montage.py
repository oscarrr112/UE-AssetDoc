# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Scoped, explicitly registered Montage authoring Profile."""

from __future__ import annotations

import re

from asset_document.canonical.document_codec import DocumentCodec, integral_numbers_as_integers
from asset_document.diagnostics import AssetDocumentContractError, contract_error
from asset_document.diff.canonical_diff import canonical_diff
from asset_document.diff.model import Difference, DiffKind
from asset_document.diff.semantic_compare import as_authored, same_number, same_value
from asset_document.dto import PrimitiveRequirement
from asset_document.meta import DocumentAction



ANIM_MONTAGE_CLASS = "/Script/Engine.AnimMontage"
ANIM_MONTAGE_ROUTE_ID = "animation_profile"
from asset_document.profiles import anim_montage_execution as execution
from asset_document.profiles.anim_montage_codec import (lower_body, snapshot_to_document_parts, validate_notify_times,
                                                         validate_notify_tracks)
from asset_document.profiles import anim_montage_settings as settings_codec

ANIM_MONTAGE_REQUIRED_PRIMITIVES = execution.REQUIREMENTS
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
_ALIAS_ONLY = {"References": ("Skeleton",)}


class AnimMontageProfileCandidate:
    """Stateless runtime handler for the Montage structural and settings surfaces."""
    profile_id = "profile.anim_montage.v1"
    exact_asset_class_path = ANIM_MONTAGE_CLASS
    runtime_entry_module = "asset_document/bootstrap.py"

    def inspect_profile(self) -> dict:
        return {"Class": ANIM_MONTAGE_CLASS, "BodySections": list(CANONICAL_BODY_KEYS)}

    def create_template(self, target: str) -> dict:
        return {"SchemaVersion": 1, "Target": _require_target(target),
                "Class": ANIM_MONTAGE_CLASS, "Action": DocumentAction.CREATE_OR_UPDATE.value,
                "Definitions": {}, "Properties": {}, "Body": _template_body()}

    def validate(self, document: object) -> None:
        if type(document) is not dict:
            raise contract_error("/", "AnimMontage.InvalidDocumentType", "AnimMontage document must be an exact object")
        _validate_header(document)
        _validate_body(document.get("Body", {}))
        DocumentCodec().validate(document)
        body = document.get("Body", {})
        validate_notify_times({key: body[key] for key in ("SlotAnimTracks", "Notifies", "NotifyStates") if key in body})
        # A new Montage has one notify track (the editor's track "1") unless NotifyTracks lists its tracks.
        validate_notify_tracks(body, document.get("Action") == DocumentAction.CREATE.value)
        if document.get("Action") == DocumentAction.CREATE.value:
            # Create targets a new Montage: its curves are exactly the authored ones.
            body = document.get("Body", {})
            time_stretch = body.get("TimeStretch")
            settings_codec.validate_time_stretch_curve(
                {"TimeStretch": time_stretch} if type(time_stretch) is dict else {},
                execution.created_body(body), execution.NATIVE_TIME_STRETCH_CURVE)

    def canonicalize(self, document: object) -> dict:
        self.validate(document)
        canonical = _snapshot(document)
        if "Body" in canonical:
            _resolve_aliases(canonical["Body"])
        return canonical

    def schema(self) -> dict:
        return {"SchemaVersion": 1, "Class": ANIM_MONTAGE_CLASS, "Type": "object",
                "Required": ["SchemaVersion", "Target", "Class", "Action"],
                "Properties": {"Body": {"type": "object", "keys": list(CANONICAL_BODY_KEYS)}}}

    def required_primitives(self) -> tuple[PrimitiveRequirement, ...]:
        return ANIM_MONTAGE_REQUIRED_PRIMITIVES

    def create_target_context(self, document, context):
        return execution.create_target_context(_executable(self.canonicalize(document)), context)

    def preflight(self, document, scratch_handle, context):
        return execution.preflight(_executable(self.canonicalize(document)), context)

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        return execution.apply(_executable(self.canonicalize(document)), read_asset_handle, mutable_asset_handle, context)

    def repair(self, document, asset_handle, context):
        _executable(self.canonicalize(document))
        # Coordinator always visits Repair; admitted Apply primitives already own
        # completion, so this phase has no additional mutation contribution.
        return ()

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        body, properties = snapshot_to_document_parts(execution.read_snapshot(context, asset_handle))
        return {"SchemaVersion": 1, "Target": _require_target(target), "Class": ANIM_MONTAGE_CLASS,
                "Action": DocumentAction.CREATE_OR_UPDATE.value, "Definitions": {}, "Properties": properties,
                "Body": body}

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != ANIM_MONTAGE_CLASS or editor_asset_handle is None:
            raise execution.error("Scoped Montage inspection requires a live exact Montage.", "/Inspect", "AnimMontage.InspectNotAdmitted")
        return self.extract(target=exact_asset_object_path.rsplit(".", 1)[0], asset_handle=editor_asset_handle, context=context)

    def diff(self, authored, extracted):
        # A Diff request's document may carry integral numbers as floats (LoopingCount 1.0); Apply's does not.
        before = self.canonicalize(integral_numbers_as_integers(authored))
        after = self.canonicalize(integral_numbers_as_integers(extracted))
        return _scoped_diff(before, after)


def _executable(document):
    if document.get("Definitions"):
        raise execution.error("Definitions are not authored by the AnimMontage Profile.", "/Definitions",
                              "AnimMontage.UnsupportedDefinitions")
    body = document.get("Body", {})
    for key, allowed in _ALIAS_ONLY.items():
        for name in body.get(key, {}):
            if name not in allowed:
                raise contract_error(f"/Body/{key}/{name}", "AnimMontage.UnsupportedAuthoredField",
                                     f"Body.{key}.{name} is not a supported field")
    # Strict operation schema, distinct from historical validation of blocked documents.
    execution.lower_document(document)
    return document


def _scoped_diff(before, after):
    before_body, after_body = before.get("Body", {}), after.get("Body", {})
    headers = []
    for document in (before, after):
        header = {key: value for key, value in document.items() if key not in ("Body", "Action", "Properties")}
        header.setdefault("Definitions", {})
        headers.append(header)
    changes = list(canonical_diff(*headers))
    before_properties, after_properties = before.get("Properties", {}), after.get("Properties", {})
    if settings_codec.properties_differ(before_properties, after_properties):
        changes.append(Difference("/Properties", DiffKind.REPLACE, True, before_properties,
                                  "Properties" in after, after_properties))
    native_keys = ("skeleton", "preview_mesh", "curves", "slots", "sections", "notifies", "notify_states", "notify_tracks")
    for canonical, key in zip(execution.SUPPORTED, native_keys):
        if canonical not in before_body:
            continue
        expected, actual = before_body[canonical], after_body.get(canonical)
        normalized = True
        try:
            expected = lower_body({canonical: expected})[key]
            actual = lower_body({canonical: actual})[key] if canonical in after_body else None
        except AssetDocumentContractError:
            # Historical read-only documents remain comparable, without inferring
            # the missing fields required for an executable full replacement.
            expected, actual = before_body[canonical], after_body.get(canonical)
            normalized = False
        if normalized and key == "slots" and type(actual) is list:
            _align_segment_starts(expected, actual)
        if (normalized and key in ("notifies", "notify_states") and type(actual) is list
                and all(type(row) is dict and "object" in row for row in expected + actual)):
            changes.extend(_notify_differences(canonical, expected, actual, before_body[canonical], after_body[canonical]))
            continue
        if canonical_diff(expected, actual):
            changes.append(Difference("/Body/" + canonical, DiffKind.REPLACE, True, before_body[canonical],
                                      canonical in after_body, after_body.get(canonical)))
    # Settings regions are sparse: only authored fields (and authored metadata
    # properties) are compared, at stored float32 precision.
    for key in settings_codec.SETTINGS_BODY_KEYS:
        if key not in before_body:
            continue
        try:
            expected = settings_codec.normalize_settings({key: before_body[key]})
            actual = settings_codec.normalize_settings({key: after_body[key]}) if key in after_body else {}
            changed = bool(settings_codec.changed_settings_regions(expected, actual))
        except AssetDocumentContractError:
            changed = bool(canonical_diff(before_body[key], after_body.get(key)))
        if changed:
            changes.append(Difference("/Body/" + key, DiffKind.REPLACE, True, before_body[key],
                                      key in after_body, after_body.get(key)))
    if "References" in before_body:
        expected = {name: value for name, value in before_body["References"].items() if name != "Skeleton"}
        actual = {name: value for name, value in after_body.get("References", {}).items() if name != "Skeleton"}
        if canonical_diff(expected, actual):
            changes.append(Difference("/Body/References", DiffKind.REPLACE, True, before_body["References"],
                                      "References" in after_body, after_body.get("References")))
    return tuple(changes)


# A segment StartPos within this many seconds of the engine's (the previous segment's float32 end) is that start:
# Apply moves it there (AnimationProvider.Montage.SegmentGap otherwise), so the no-op check compares it the same way.
SEGMENT_START_TOLERANCE = 1e-5


def _align_segment_starts(expected: list, actual: list) -> None:
    for slot, observed in zip(expected, actual):
        if type(slot) is not dict or type(observed) is not dict or slot.get("name") != observed.get("name"):
            continue
        for segment, live in zip(slot.get("segments", []), observed.get("segments", [])):
            if (type(segment) is dict and type(live) is dict and type(segment.get("start")) is float
                    and type(live.get("start")) is float and abs(segment["start"] - live["start"]) <= SEGMENT_START_TOLERANCE):
                segment["start"] = live["start"]


def _notify_key(row: dict) -> tuple:
    return (row["object"]["class"], row.get("track_index", 0))


def _notify_matches(wanted: dict, observed: dict) -> bool:
    """One notify row as authored: the same time (and duration), track and class, and the properties the
    document sets with the values it gives (a property it omits keeps its class default)."""
    if _notify_key(wanted) != _notify_key(observed) or not same_number(wanted["time"], observed["time"]):
        return False
    if ("duration" in wanted) != ("duration" in observed) or (
            "duration" in wanted and not same_number(wanted["duration"], observed["duration"])):
        return False
    authored = wanted["object"]["properties"]
    live = observed["object"]["properties"]
    return all(name in live and same_value(value, as_authored(value, live[name])) for name, value in authored.items())


def _notify_differences(canonical: str, expected: list, actual: list, document_rows: list, actual_rows: list) -> list:
    """Notifies are a set keyed by (time, track, class, authored properties), not a sequence: the editor keeps them
    sorted by time, so a document may list them in any order. A notify the asset lacks is reported at its document
    index with the read-back notify of the same class and track nearest in time (the one the author most likely
    meant); a read-back notify the document does not list is reported as an addition."""
    used: set[int] = set()
    missing = []
    for index, wanted in enumerate(expected):
        other = next((other for other in range(len(actual)) if other not in used and _notify_matches(wanted, actual[other])), None)
        if other is None:
            missing.append(index)
        else:
            used.add(other)
    changes = []
    for index in missing:
        free = [other for other in range(len(actual)) if other not in used and _notify_key(actual[other]) == _notify_key(expected[index])]
        other = min(free, key=lambda candidate: abs(actual[candidate]["time"] - expected[index]["time"]), default=None)
        if other is not None:
            used.add(other)
        changes.append(Difference(f"/Body/{canonical}/{index}", DiffKind.REPLACE, True, document_rows[index],
                                  other is not None, actual_rows[other] if other is not None else None))
    for other in range(len(actual)):
        if other not in used:
            changes.append(Difference(f"/Body/{canonical}/-", DiffKind.ADD, False, None, True, actual_rows[other]))
    return changes


def _snapshot(value: dict) -> dict:
    codec = DocumentCodec()
    return codec.decode(codec.encode(value))


def _require_target(value: object) -> str:
    if admits_read_only(value):
        return value
    if type(value) is not str or not value:
        raise contract_error("/Target", "Meta.EmptyTarget", "Target must not be empty")
    if re.fullmatch(r"/Game/[A-Za-z0-9_/-]+", value) is None:
        raise contract_error(
            "/Target",
            "AnimMontage.NonCanonicalTarget",
            "Target must be a canonical /Game package path",
        )
    return value


def _validate_header(document: dict) -> None:
    if type(document.get("SchemaVersion")) is not int or document["SchemaVersion"] != 1:
        raise contract_error(
            "/SchemaVersion", "Meta.InvalidSchemaVersion", "SchemaVersion must be exact integer 1"
        )
    _require_target(document.get("Target"))
    if document.get("Class") != ANIM_MONTAGE_CLASS:
        raise contract_error(
            "/Class", "AnimMontage.ClassMismatch", "Class must be /Script/Engine.AnimMontage"
        )
    if document.get("Action") not in {item.value for item in DocumentAction}:
        raise contract_error(
            "/Action", "AnimMontage.InvalidAction", "Action must be an existing DocumentAction value"
        )
    for key in ("Definitions", "Properties"):
        if type(document.get(key, {})) is not dict:
            raise contract_error(
                f"/{key}", f"AnimMontage.Invalid{key}Type", f"{key} must be an exact object"
            )


def _validate_body(body: object) -> None:
    if type(body) is not dict:
        raise contract_error("/Body", "AnimMontage.InvalidBodyType", "Body must be an exact object")
    for key, value in body.items():
        path = f"/Body/{key}"
        if key == "_Skipped":
            raise contract_error(path, "AnimMontage.ExtractOnlyBodyKey", "Body._Skipped cannot be authored")
        if key not in CANONICAL_BODY_KEYS:
            raise contract_error(path, "AnimMontage.UnknownBodyKey", "Body key is not canonical")
        DocumentCodec().validate(value)
    _validate_alias_containers(body)
    _validate_sections(body)


def _validate_alias_containers(body: dict) -> None:
    for key, nested_alias in (("References", "Skeleton"), ("Preview", "PreviewMesh")):
        if key in body and type(body[key]) is not dict:
            raise contract_error(f"/Body/{key}", "AnimMontage.InvalidBodyRegion", f"{key} must be an exact object")
        if (
            key in body
            and nested_alias in body[key]
            and body[key][nested_alias] is not None
            and type(body[key][nested_alias]) is not str
        ):
            raise contract_error(
                f"/Body/{key}/{nested_alias}", "AnimMontage.InvalidAlias", f"{nested_alias} must be a string or null"
            )
    for key in ("Skeleton", "PreviewMesh"):
        if key in body and body[key] is not None and type(body[key]) is not str:
            raise contract_error(f"/Body/{key}", "AnimMontage.InvalidAlias", f"{key} must be a string or null")


def _validate_sections(body: dict) -> None:
    sections = body.get("CompositeSections")
    if sections is not None and type(sections) is not list:
        raise contract_error("/Body/CompositeSections", "AnimMontage.InvalidSections", "CompositeSections must be an array")
    if sections is None:
        return
    names = _section_names(sections)
    for index, section in enumerate(sections):
        next_name = section.get("NextSectionName")
        if next_name is not None and next_name not in names:
            raise contract_error(
                f"/Body/CompositeSections/{index}/NextSectionName",
                "AnimMontage.InvalidSectionLink",
                "NextSectionName must name a managed CompositeSection or be null",
            )
    metadata = body.get("SectionMetadata", {})
    if type(metadata) is not dict or any(key not in names for key in metadata):
        raise contract_error(
            "/Body/SectionMetadata", "AnimMontage.InvalidSectionMetadata", "SectionMetadata keys must name managed CompositeSections"
        )


def _section_names(sections: list) -> set[str]:
    names: set[str] = set()
    for index, section in enumerate(sections):
        if type(section) is not dict or type(section.get("Name")) is not str or not section["Name"]:
            raise contract_error(
                f"/Body/CompositeSections/{index}", "AnimMontage.InvalidSection", "CompositeSections require a non-empty Name"
            )
        if section["Name"] in names:
            raise contract_error(
                f"/Body/CompositeSections/{index}/Name", "AnimMontage.DuplicateSection", "CompositeSection names must be unique"
            )
        names.add(section["Name"])
    return names


def _resolve_aliases(body: dict) -> None:
    references = body.get("References")
    if type(references) is dict and "Skeleton" in references:
        body["Skeleton"] = references["Skeleton"]
    preview = body.get("Preview")
    if type(preview) is dict and "PreviewMesh" in preview:
        body["PreviewMesh"] = preview["PreviewMesh"]


def _template_body() -> dict:
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


from asset_document.meta import (ProfileMeta, RegionMeta, RegionOwnership, IdentityPolicy,
    OrderPolicy, DefaultPolicy, OmissionPolicy, DeletePolicy, ComparisonPolicy)
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document.read_only_extract import admits_read_only

# Preserve the existing successor's region identities and comparison policies.
_REGION_ROWS = (
    ("skeleton", "Skeleton", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
    ("preview_mesh", "PreviewMesh", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
    ("references", "References", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
    ("preview", "Preview", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
    ("sync", "Sync", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
    ("root_motion", "RootMotion", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
    ("metadata", "Metadata", IdentityPolicy.STABLE_ID, OrderPolicy.PRESERVE, ComparisonPolicy.ORDERED),
    ("section_metadata", "SectionMetadata", IdentityPolicy.STABLE_KEY, OrderPolicy.IDENTITY_KEYED, ComparisonPolicy.CANONICAL),
    ("time_stretch", "TimeStretch", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
    ("curves", "Curves", IdentityPolicy.STABLE_KEY, OrderPolicy.PRESERVE, ComparisonPolicy.ORDERED),
    ("slot_anim_tracks", "SlotAnimTracks", IdentityPolicy.STABLE_KEY, OrderPolicy.PRESERVE, ComparisonPolicy.ORDERED),
    ("composite_sections", "CompositeSections", IdentityPolicy.STABLE_KEY, OrderPolicy.PRESERVE, ComparisonPolicy.ORDERED),
    ("notifies", "Notifies", IdentityPolicy.STABLE_ID, OrderPolicy.PRESERVE, ComparisonPolicy.ORDERED),
    ("notify_states", "NotifyStates", IdentityPolicy.STABLE_ID, OrderPolicy.PRESERVE, ComparisonPolicy.ORDERED),
    ("notify_tracks", "NotifyTracks", IdentityPolicy.POSITIONAL, OrderPolicy.PRESERVE, ComparisonPolicy.ORDERED),
    ("blend", "Blend", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
)
ANIM_MONTAGE_PROFILE_META = ProfileMeta(
    "profile.anim_montage.v1", ANIM_MONTAGE_ROUTE_ID, ANIM_MONTAGE_CLASS,
    tuple(RegionMeta("anim_montage." + name + ".v1", "/Body/" + key,
        RegionOwnership.MANAGED, identity, order, DefaultPolicy.EXPLICIT,
        OmissionPolicy.PRESERVE, DeletePolicy.EXPLICIT_ONLY, comparison)
        for name, key, identity, order, comparison in _REGION_ROWS), ANIM_MONTAGE_REQUIRED_PRIMITIVES)

def create_anim_montage_runtime_handler():
    return AnimMontageProfileCandidate()

ANIM_MONTAGE_RUNTIME_REGISTRATION = RuntimeProfileRegistration(
    ANIM_MONTAGE_PROFILE_META, create_anim_montage_runtime_handler, RuntimeTargetPolicy.CREATE_OR_UPDATE,
    execution.LIFECYCLE)
