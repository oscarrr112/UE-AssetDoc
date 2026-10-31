# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Registered AnimComposite Profile (profile.anim_composite.v1): the Anim Composite editor's segment
track.

Document (docs/architecture/asset-document-animation-assets-matrix.md)::

    "Body": {
      "Skeleton": "/Game/Characters/SK_Hero_Skeleton.SK_Hero_Skeleton",   # create: the New Asset dialog's
      "PreviewMesh": "",                                                   #   skeleton; immutable after
      "Segments": [                                                        # the ordered segment track
        {"Animation": "/Game/Anims/Jump_Start.Jump_Start"},
        {"Animation": "/Game/Anims/Jump_Loop.Jump_Loop", "StartTime": 0.1, "EndTime": 0.6, "PlayRate": 1.5,
         "LoopCount": 3}
      ],
      "Curves": [...], "NotifyTracks": [...], "Notifies": [...], "NotifyStates": [...]
    },
    "Properties": {"bLoop": false, ...}

* Segments play back to back in list order (the editor collapses the track); their positions on the
  timeline follow and are read only (Inspect lists them).  ``StartTime`` / ``EndTime`` are times in
  the segment's animation (default: the whole animation), ``PlayRate`` (default 1, not 0) and
  ``LoopCount`` (default 1).  Omitted on a segment whose index and animation are unchanged = keep.
* Animations are Animation Sequences or other Anim Composites (not this one, directly or through
  nested composites), with a compatible skeleton and one additive type.
* ``Curves``, ``NotifyTracks``, ``Notifies`` and ``NotifyStates`` take the AnimSequence Profile's forms and
  rules (anim_sequence_codec); apply writes them through the sequence authoring code (the curve and notify
  panels), after the segments, so times are checked against the new length.  Omitted = keep.

Writes are covered by Core's package snapshot (existing target) or the factory rollback (created
target); preflight returns ``SNAPSHOT_COVERED``.
"""
from __future__ import annotations

from asset_document.diff.model import Difference, DiffKind
from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, IdentityPolicy, OmissionPolicy, OrderPolicy, ProfileMeta,
    RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document import markers
from asset_document.profiles import anim_asset_common as anim
from asset_document.profiles import anim_sequence as sequence
from asset_document.profiles import anim_sequence_codec as timeline_codec
from asset_document.profiles import data_table_common as common
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements
from asset_document import tolerance
from asset_document.tolerance import authored_rule

CLASS = "/Script/Engine.AnimComposite"
PROFILE_ID = "profile.anim_composite.v1"
PREFIX = "AnimComposite"
TIMELINE_KEYS = timeline_codec.TIMELINE_REGIONS
BODY_KEYS = ("Skeleton", "PreviewMesh", "Segments") + TIMELINE_KEYS
SEGMENT_FIELDS = {"StartTime": "start", "EndTime": "end", "PlayRate": "rate", "LoopCount": "loops"}
LIFECYCLE = generic_factory_lifecycle(anim.ROUTE, ("factory_context",))
REQUIREMENTS = (anim.requirements(anim.CLASS_RESOLVE, anim.RESOLVE, anim.REFERENCE_RESOLVE, anim.COMPOSITE_READ,
                                  anim.COMPOSITE_EDIT)
                + lifecycle_requirements(LIFECYCLE))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}
BODY_OWNED = {"AnimationTrack": "Body.Segments", "Skeleton": "Body.Skeleton", "PreviewSkeletalMesh": "Body.PreviewMesh",
              "Notifies": "Body.Notifies", "AnimNotifyTracks": "Body.NotifyTracks"}


def _error(path, message, code):
    return common.error(path, message, code)


def _segment(row, path):
    if type(row) is not dict:
        raise _error(path, "A segment is an object with Animation and optional StartTime, EndTime, PlayRate, LoopCount.",
                     "AnimComposite.InvalidSegment")
    for key in row:
        if key != "Animation" and key not in SEGMENT_FIELDS:
            raise _error(f"{path}/{common.escape(key)}", f"Unknown segment key '{key}'. Keys: Animation, StartTime, "
                         "EndTime, PlayRate, LoopCount.", "AnimComposite.InvalidSegment")
    item = {"Animation": anim.reference(row.get("Animation"), f"{path}/Animation", "AnimComposite.InvalidSegment",
                                        "Animation", PREFIX)}
    for key in ("StartTime", "EndTime", "PlayRate"):
        if key in row:
            item[key] = anim.number(row[key], f"{path}/{key}", "AnimComposite.InvalidSegment", key)
    # The segment Details rows refuse these; an older asset can still hold them (read as they are).
    if "StartTime" in item and item["StartTime"] < 0:
        authored_rule(f"{path}/StartTime", "AnimComposite.InvalidSegment", "StartTime is >= 0.")
    if "StartTime" in item and "EndTime" in item and not item["StartTime"] < item["EndTime"]:
        authored_rule(f"{path}/EndTime", "AnimComposite.InvalidSegment", "EndTime must be after StartTime.",
                      judged=(f"{path}/StartTime", f"{path}/EndTime"))
    if item.get("PlayRate") == 0:
        authored_rule(f"{path}/PlayRate", "AnimComposite.InvalidSegment", "PlayRate must not be 0.")
    if "LoopCount" in row:
        item["LoopCount"] = anim.integer(row["LoopCount"])
        if item["LoopCount"] is None:
            raise _error(f"{path}/LoopCount", "LoopCount is an integer within 1..100000.", "AnimComposite.InvalidSegment")
        if not 1 <= item["LoopCount"] <= 100000:
            authored_rule(f"{path}/LoopCount", "AnimComposite.InvalidSegment", "LoopCount is an integer within "
                          "1..100000.")
    return item


def canonical_body(body):
    out = {}
    if "Skeleton" in body:
        out["Skeleton"] = anim.reference(body["Skeleton"], "/Body/Skeleton", "AnimComposite.InvalidSkeleton", "Skeleton",
                                         PREFIX)
    if "PreviewMesh" in body:
        out["PreviewMesh"] = anim.optional_path(body["PreviewMesh"], "/Body/PreviewMesh",
                                                "AnimComposite.InvalidPreviewMesh", "PreviewMesh")
    if "Segments" in body:
        rows = body["Segments"]
        if type(rows) is not list:
            raise _error("/Body/Segments", "Segments is the ordered list of segments.", "AnimComposite.InvalidSegment")
        out["Segments"] = [_segment(row, f"/Body/Segments/{index}") for index, row in enumerate(rows)]
    out.update(timeline_body(body))
    return out


def timeline_body(body):
    """The authored timeline regions, validated as the AnimSequence Profile validates them."""
    return timeline_codec.canonical_body({key: body[key] for key in TIMELINE_KEYS if key in body})


def live_timeline(live):
    return timeline_codec.timeline_to_document(live["timeline"]) if "timeline" in live else {}


def timeline_matches(authored, live):
    """Whether the authored (canonical) timeline regions hold on the live ones (document form, as read)."""
    with tolerance.settled():
        # The live rows are read as they are; the authored-input rules are not judged on them.
        live = timeline_body(live)
    return all(timeline_codec.region_matches(key, authored[key], live.get(key, []), authored, live)
               for key in TIMELINE_KEYS if key in authored)


def timeline_op(authored, live):
    """The timeline edit: the sequence authoring intent of the authored regions (rows keep the notify objects they
    pair with, as on a sequence)."""
    reuse = None
    if any(key in authored for key in timeline_codec.REUSED_REGIONS):
        reuse = timeline_codec.reuse_plan(authored, live.get("timeline"))
    intent, _ = timeline_codec.lower(authored, {}, set(), reuse)
    return {"op": "timeline", "intent": intent}


def canonicalize(document):
    body = common.validate_header(document, (CLASS,), PREFIX, BODY_KEYS)
    canonical_body(body)
    _properties().check_owned(document.get("Properties", {}))
    return common.snapshot(document)


def _properties():
    return common.PropertiesRegion(CLASS, BODY_OWNED, PREFIX)


def read_live(calls):
    return calls.query(anim.COMPOSITE_READ)


def live_segments(live):
    return [{"Animation": anim.live_reference(row["animation"]), "StartTime": row["start"], "EndTime": row["end"],
             "PlayRate": row["rate"], "LoopCount": row["loops"]} for row in live.get("segments", [])]


def body_from_live(live):
    return {"Skeleton": anim.live_reference(live.get("skeleton")), "PreviewMesh": live.get("preview_mesh", ""),
            "Segments": live_segments(live), **live_timeline(live)}


def segments_match(authored, live, exact=False):
    if len(authored) != len(live):
        return False
    for row, have in zip(authored, live):
        if row["Animation"] != have["Animation"]:
            return False
        if any(not common.same(value, have[key], exact) for key, value in row.items() if key in SEGMENT_FIELDS):
            return False
    return True


def segments_op(authored, live):
    """The positional replace. A segment the track already holds (same animation) is written from that segment
    ({"keep": live index} plus the fields that change), so values it keeps are not judged again: a segment whose
    animation is missing or whose values the Details rows would refuse survives an edit of another value. Other
    segments are written whole; omitted fields keep the live value where index and animation match."""
    rows = []
    pairs = tolerance.pair_lists(authored, live)
    for index, row in enumerate(authored):
        partner = pairs[index]
        if partner is not None and live[partner]["Animation"] == row["Animation"]:
            item = {"keep": partner}
            for key, arg in SEGMENT_FIELDS.items():
                if key in row and not common.same(row[key], live[partner][key], exact=True):
                    item[arg] = row[key]
            rows.append(item)
            continue
        have = live[index] if index < len(live) and live[index]["Animation"] == row["Animation"] else {}
        item = {"animation": row["Animation"]}
        for key, arg in SEGMENT_FIELDS.items():
            if key in row:
                item[arg] = row[key]
            elif key in have:
                item[arg] = have[key]
        rows.append(item)
    return {"op": "segments", "segments": rows}


def map_path(primitive_path, op_paths):
    parts = [part for part in (primitive_path or "").split("/") if part]
    if len(parts) >= 2 and parts[0] == "ops" and parts[1].isdigit() and int(parts[1]) < len(op_paths):
        path = op_paths[int(parts[1])]
        if len(parts) >= 4 and parts[2] == "segments" and parts[3].isdigit():
            path = f"/Body/Segments/{parts[3]}"
            fields = {"animation": "/Animation", "start": "/StartTime", "end": "/EndTime", "rate": "/PlayRate",
                      "loops": "/LoopCount"}
            if len(parts) > 4 and parts[4] in fields:
                path += fields[parts[4]]
        elif len(parts) >= 3 and parts[2] == "intent":
            path = sequence.document_path("/" + "/".join(parts[3:]))
        return path
    return "/Body"


def edit(calls, ops):
    op_paths = [path for _, path in ops]
    outcome = common.invoke(calls.context, calls.by_id, anim.COMPOSITE_EDIT,
                            {"schema_version": 1, "ops": [op for op, _ in ops]},
                            (common.NamedOpaqueHandleArgument("asset", calls.write),),
                            paths=lambda where: map_path(where, op_paths), prefix=PREFIX)
    return list(outcome.changed_set)


def apply(document, read, mutable, context):
    body = canonical_body(document.get("Body", {}))
    properties = document.get("Properties", {})
    region = _properties()
    region.check_owned(properties)
    calls = anim.AnimCalls(context, BY_ID, read, mutable, PREFIX)
    live = read_live(calls)
    anim.require_unchanged(body, "Skeleton", anim.live_reference(live.get("skeleton")), PREFIX,
                           "An Anim Composite's Skeleton is chosen when it is created and the editor offers no change")
    ops = []
    current = live_segments(live)
    if "Segments" in body and not segments_match(body["Segments"], current):
        ops.append((segments_op(body["Segments"], current), "/Body/Segments"))
    if "PreviewMesh" in body and body["PreviewMesh"] != live.get("preview_mesh", ""):
        ops.append(({"op": "preview_mesh", "path": body["PreviewMesh"]}, "/Body/PreviewMesh"))
    authored_timeline = {key: body[key] for key in TIMELINE_KEYS if key in body}
    if authored_timeline and not timeline_matches(authored_timeline, live_timeline(live)):
        ops.append((timeline_op(authored_timeline, live), "/Body"))
    changes = edit(calls, ops) if ops else []
    changes.extend(region.apply(properties, read, mutable, context))
    return tuple(changes)


def create_target_context(document, context):
    body = canonical_body(document.get("Body", {}))
    if "Skeleton" not in body:
        raise _error("/Body/Skeleton", "Creating an Anim Composite requires Body.Skeleton (the skeleton the New Anim "
                     "Composite dialog asks for).", "AnimComposite.SkeletonRequired")
    return anim.resolve_context(context, BY_ID, body["Skeleton"], "/Body/Skeleton", anim.SKELETON_CLASS, PREFIX, "Skeleton")


def extract(target, read, context, include_defaults=True):
    live = read_live(anim.AnimCalls(context, BY_ID, read, prefix=PREFIX))
    body = body_from_live(live)
    for region in timeline_codec.REUSED_REGIONS:
        if region in body:
            markers.warn_all(body[region], PREFIX, f"/Body/{region}")
    return common.document(target, CLASS, _properties().extract(read, context, include_defaults), body)


def diff(authored, extracted, exact):
    changes = common.header_diff(authored, extracted)
    body = canonical_body(authored.get("Body", {}))
    live = canonical_body(extracted.get("Body", {}))
    for key in ("Skeleton", "PreviewMesh"):
        if key in body and body[key] != live.get(key, ""):
            changes.append(Difference(f"/Body/{key}", DiffKind.REPLACE, True, body[key], key in live, live.get(key)))
    if "Segments" in body and not segments_match(body["Segments"], live.get("Segments", []), exact):
        changes.append(Difference("/Body/Segments", DiffKind.REPLACE, True, body["Segments"], True, live.get("Segments")))
    for key in TIMELINE_KEYS:
        if key in body and (key not in live or not timeline_codec.region_matches(key, body[key], live[key], body, live)):
            raw = authored.get("Body", {}), extracted.get("Body", {})
            changes.append(Difference(f"/Body/{key}", DiffKind.REPLACE, True, raw[0][key], key in raw[1], raw[1].get(key)))
    changes.extend(common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact))
    return tuple(changes)


class AnimCompositeRuntimeProfile:
    profile_id = PROFILE_ID
    exact_asset_class_path = CLASS
    runtime_entry_module = "asset_document/bootstrap.py"
    preflight_scratch = False

    def required_primitives(self):
        return REQUIREMENTS

    def inspect_profile(self):
        return {"Class": CLASS, "BodySections": list(BODY_KEYS), "ReadOnlySections": [],
                "CreateContext": "Body.Skeleton"}

    def create_template(self, target):
        return common.document(common.require_target(target, PREFIX), CLASS, {},
                               {"Skeleton": "/Game/Characters/SK_Hero_Skeleton.SK_Hero_Skeleton", "Segments": []})

    def validate(self, document):
        canonicalize(document)

    def canonicalize(self, document):
        return canonicalize(document)

    def create_target_context(self, document, context):
        return create_target_context(canonicalize(document), context)

    def preflight(self, document, scratch_handle, context):
        canonical = canonicalize(document)
        _properties().validate(canonical.get("Properties", {}), context)
        return SNAPSHOT_COVERED

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        return apply(canonicalize(document), read_asset_handle, mutable_asset_handle, context)

    def repair(self, document, asset_handle, context):
        canonicalize(document)
        return ()

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        return extract(common.require_target(target, PREFIX), asset_handle, context, include_defaults)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != CLASS or editor_asset_handle is None:
            raise _error("/Inspect", "Anim Composite inspection reads a live Anim Composite.", "AnimComposite.InspectNotAdmitted")
        document = self.extract(target=exact_asset_object_path.rsplit(".", 1)[0], asset_handle=editor_asset_handle,
                                context=context, include_defaults=True)
        live = read_live(anim.AnimCalls(context, BY_ID, editor_asset_handle, prefix=PREFIX))
        timeline = [{"Animation": row["animation"], "Position": row["start_pos"]} for row in live.get("segments", [])]
        return {**canonicalize(document), "CompositeDetails": {"Length": live.get("length", 0), "Timeline": timeline}}

    def diff(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=False)

    def diff_exact(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=True)


def create_anim_composite_runtime_handler():
    return AnimCompositeRuntimeProfile()


ANIM_COMPOSITE_PROFILE_META = ProfileMeta(
    PROFILE_ID, anim.ROUTE, CLASS,
    tuple(RegionMeta(f"anim_composite.{name}.v1", f"/Body/{key}", RegionOwnership.MANAGED, identity, order,
                     DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE, DeletePolicy.EXPLICIT_ONLY, comparison)
          for name, key, identity, order, comparison in (
              ("skeleton", "Skeleton", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
              ("preview_mesh", "PreviewMesh", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
              ("segments", "Segments", IdentityPolicy.POSITIONAL, OrderPolicy.PRESERVE, ComparisonPolicy.ORDERED),
              ("curves", "Curves", IdentityPolicy.STABLE_KEY, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
              ("notify_tracks", "NotifyTracks", IdentityPolicy.STABLE_KEY, OrderPolicy.PRESERVE, ComparisonPolicy.ORDERED),
              ("notifies", "Notifies", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
              ("notify_states", "NotifyStates", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL,
               ComparisonPolicy.CANONICAL))),
    REQUIREMENTS)
ANIM_COMPOSITE_RUNTIME_REGISTRATION = RuntimeProfileRegistration(
    ANIM_COMPOSITE_PROFILE_META, create_anim_composite_runtime_handler, RuntimeTargetPolicy.CREATE_OR_UPDATE, LIFECYCLE)

__all__ = ("ANIM_COMPOSITE_RUNTIME_REGISTRATION", "AnimCompositeRuntimeProfile", "segments_op")
