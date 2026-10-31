# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Registered PoseAsset Profile (profile.pose_asset.v1): the Pose Asset editor's poses, curves and additive
settings.

Document (docs/architecture/asset-document-animation-assets-matrix.md)::

    "Body": {
      "SourceAnimation": "/Game/Anims/Face_Poses.Face_Poses",   # create: the Create Pose Asset source; "" = none
      "Skeleton": "/Game/Characters/SK_Face_Skeleton.SK_Face_Skeleton",   # read only (the source's)
      "PreviewMesh": "",
      "Poses": ["Neutral", {"Name": "Smile", "RenamedFrom": "Pose_1",
                            "Curves": {"CTRL_Smile": 1.0},                # read only: non-zero curve values
                            "Bones": {"jaw": {"Rotation": {"Pitch": 0, "Yaw": 0, "Roll": -12}}}}],  # read only
      "Curves": ["CTRL_Smile", "CTRL_Blink"],                     # the asset's curves; leave one out to delete it
      "Additive": true,
      "BasePose": "Neutral"                                       # "" = the reference pose
    },
    "Properties": {"RetargetSource": "None", ...}

* Poses come from the source animation (one per frame) and are renamed or deleted, as in the Pose
  Asset editor; a Pose Asset without a source animation also adds poses from the reference pose (the pose list's
  Add Pose > Add Pose from Reference), at the end of the list. The editor cannot reorder poses
  (``PoseAsset.PoseOrderUnsupported``). Changing ``SourceAnimation`` runs the editor's "Update Source": poses are
  re-read from the new animation, keeping their names; ``""`` clears the Source Animation row and keeps the poses.
* A pose's ``Curves`` (curve -> value, curves at 0 left out) and ``Bones`` (bone -> offset from the Skeleton's
  reference pose: Location added, Rotation applied on top, Scale factor; identity parts left out) are the saved pose
  data, read only (``PoseAsset.PoseDataReadOnly``); omitted = not compared.
* ``Curves`` lists the asset's curves; a curve left out is deleted (the pose list's Delete Curves). Curves come from
  the source animation and cannot be added (``PoseAsset.CurveAddUnsupported``).
* ``Additive`` / ``BasePose`` are applied together (the Details panel's Apply button).

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
from asset_document.profiles import anim_asset_common as anim
from asset_document.profiles import data_table_common as common
from asset_document.profiles.skeleton import validate_name
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements

CLASS = "/Script/Engine.PoseAsset"
PROFILE_ID = "profile.pose_asset.v1"
PREFIX = "PoseAsset"
BODY_KEYS = ("SourceAnimation", "Skeleton", "PreviewMesh", "Poses", "Curves", "Additive", "BasePose")
POSE_KEYS = ("Name", "RenamedFrom", "Curves", "Bones")
BONE_FIELDS = {"Location": ("X", "Y", "Z"), "Rotation": ("Pitch", "Yaw", "Roll"), "Scale": ("X", "Y", "Z")}
LIFECYCLE = generic_factory_lifecycle(anim.ROUTE, ("factory_context",))
REQUIREMENTS = (anim.requirements(anim.CLASS_RESOLVE, anim.RESOLVE, anim.REFERENCE_RESOLVE, anim.POSE_ASSET_READ,
                                  anim.POSE_ASSET_EDIT)
                + lifecycle_requirements(LIFECYCLE))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}
BODY_OWNED = {"bAdditivePose": "Body.Additive", "SourceAnimation": "Body.SourceAnimation", "Skeleton": "Body.Skeleton",
              "PreviewSkeletalMesh": "Body.PreviewMesh"}


def _error(path, message, code):
    return common.error(path, message, code)


def _pose_curves(value, where):
    if type(value) is not dict:
        raise _error(where, "A pose's Curves is an object of curve name -> value.", "PoseAsset.InvalidPoses")
    return {name: anim.number(item, f"{where}/{common.escape(name)}", "PoseAsset.InvalidPoses", "A curve value")
            for name, item in value.items()}


def _pose_bones(value, where):
    if type(value) is not dict:
        raise _error(where, "A pose's Bones is an object of bone name -> {Location?, Rotation?, Scale?}.",
                     "PoseAsset.InvalidPoses")
    out = {}
    for bone, parts in value.items():
        at = f"{where}/{common.escape(bone)}"
        if type(parts) is not dict or not parts or set(parts) - set(BONE_FIELDS):
            raise _error(at, "A bone's offset is an object with Location, Rotation and / or Scale.", "PoseAsset.InvalidPoses")
        out[bone] = {}
        for key, vector in parts.items():
            fields = BONE_FIELDS[key]
            if type(vector) is not dict or set(vector) != set(fields):
                raise _error(f"{at}/{key}", f"{key} is an object with exactly {', '.join(fields)}.", "PoseAsset.InvalidPoses")
            out[bone][key] = {field: anim.number(vector[field], f"{at}/{key}/{field}", "PoseAsset.InvalidPoses", field)
                              for field in fields}
    return out


def _curve_names(value):
    if type(value) is not list:
        raise _error("/Body/Curves", "Curves is the list of the asset's curve names.", "PoseAsset.InvalidCurves")
    seen = set()
    for index, name in enumerate(value):
        if type(name) is not str or not name:
            raise _error(f"/Body/Curves/{index}", "A curve is named by a non-empty string.", "PoseAsset.InvalidCurves")
        if name in seen:
            raise _error(f"/Body/Curves/{index}", f"Curve '{name}' appears more than once.", "PoseAsset.InvalidCurves")
        seen.add(name)
    return list(value)


def canonical_body(body):
    out = {}
    # A Pose Asset made from the viewport pose, or whose source was deleted, has no SourceAnimation: "".
    if "SourceAnimation" in body:
        out["SourceAnimation"] = anim.optional_path(body["SourceAnimation"], "/Body/SourceAnimation",
                                                    "PoseAsset.InvalidSourceAnimation", "SourceAnimation")
    if "Skeleton" in body:
        out["Skeleton"] = anim.object_path(body["Skeleton"], "/Body/Skeleton", "PoseAsset.InvalidSkeleton", "Skeleton")
    if "PreviewMesh" in body:
        out["PreviewMesh"] = anim.optional_path(body["PreviewMesh"], "/Body/PreviewMesh", "PoseAsset.InvalidPreviewMesh",
                                                "PreviewMesh")
    if "Poses" in body:
        rows = body["Poses"]
        if type(rows) is not list:
            raise _error("/Body/Poses", "Poses is the ordered list of pose names (or {\"Name\", \"RenamedFrom\"}).",
                         "PoseAsset.InvalidPoses")
        poses, names, sources = [], set(), set()
        for index, row in enumerate(rows):
            where = f"/Body/Poses/{index}"
            if type(row) is dict:
                if set(row) - set(POSE_KEYS):
                    raise _error(where, "A pose is a name or an object with Name (and RenamedFrom, Curves, Bones).",
                                 "PoseAsset.InvalidPoses")
                item = {"Name": validate_name(row.get("Name"), f"{where}/Name", "pose", "PoseAsset.InvalidPoseName")}
                if "Curves" in row:
                    item["Curves"] = _pose_curves(row["Curves"], f"{where}/Curves")
                if "Bones" in row:
                    item["Bones"] = _pose_bones(row["Bones"], f"{where}/Bones")
                if "RenamedFrom" in row:
                    source = validate_name(row["RenamedFrom"], f"{where}/RenamedFrom", "pose", "PoseAsset.InvalidPoseName")
                    if source == item["Name"] or source in sources:
                        raise _error(f"{where}/RenamedFrom", "RenamedFrom names another pose, once.", "PoseAsset.InvalidRename")
                    sources.add(source)
                    item["RenamedFrom"] = source
            else:
                item = {"Name": validate_name(row, where, "pose", "PoseAsset.InvalidPoseName")}
            if item["Name"] in names:
                raise _error(where, f"Pose '{item['Name']}' appears more than once.", "PoseAsset.DuplicatePose")
            names.add(item["Name"])
            poses.append(item)
        out["Poses"] = poses
    if "Curves" in body:
        out["Curves"] = _curve_names(body["Curves"])
    if "Additive" in body:
        if type(body["Additive"]) is not bool:
            raise _error("/Body/Additive", "Additive is true or false.", "PoseAsset.InvalidAdditive")
        out["Additive"] = body["Additive"]
    if "BasePose" in body:
        if type(body["BasePose"]) is not str:
            raise _error("/Body/BasePose", "BasePose is a pose name, or \"\" for the reference pose.", "PoseAsset.InvalidBasePose")
        out["BasePose"] = body["BasePose"]
    return out


def canonicalize(document):
    body = common.validate_header(document, (CLASS,), PREFIX, BODY_KEYS)
    canonical_body(body)
    _properties().check_owned(document.get("Properties", {}))
    return common.snapshot(document)


def _properties():
    return common.PropertiesRegion(CLASS, BODY_OWNED, PREFIX)


def read_live(calls):
    return calls.query(anim.POSE_ASSET_READ)


def read_pose_data(calls):
    """{pose name: {"Curves": {...}, "Bones": {...}}} of the saved pose data (paged records of the read)."""
    names = list(read_live(calls).get("poses", []))
    records, offset = [], 0
    while True:
        page = calls.query(anim.POSE_ASSET_READ, {"offset": offset})
        records += page.get("records", [])
        offset = page.get("next_offset", -1)
        if type(offset) is not int or offset < 0:
            break
    return pose_data(names, records)


def pose_data(names, records):
    data = {name: {"Curves": {}, "Bones": {}} for name in names}
    for record in records:
        index = record.get("pose")
        if type(index) is not int or not 0 <= index < len(names):
            continue
        entry = data[names[index]]
        if "curve" in record:
            entry["Curves"][record["curve"]] = record["value"]
        elif "bone" in record:
            entry["Bones"][record["bone"]] = {key: record[field] for key, field in
                                              (("Location", "location"), ("Rotation", "rotation"), ("Scale", "scale"))
                                              if field in record}
    return data


def body_from_live(live, data=None):
    """The Body of a read; ``data`` (``pose_data``) adds each pose's Curves and Bones."""
    poses = []
    for name in live.get("poses", []):
        entry = (data or {}).get(name, {})
        extra = {key: entry[key] for key in ("Curves", "Bones") if entry.get(key)}
        poses.append({"Name": name, **extra} if extra else name)
    return {"SourceAnimation": live.get("source_animation", ""), "Skeleton": live.get("skeleton", ""),
            "PreviewMesh": live.get("preview_mesh", ""), "Poses": poses, "Curves": list(live.get("curves", [])),
            "Additive": bool(live.get("additive")), "BasePose": live.get("base_pose", "")}


def claim_poses(poses, current):
    """{authored index: live pose name}, by Name, then by RenamedFrom."""
    claimed, used = {}, set()
    for index, row in enumerate(poses):
        if row["Name"] in current:
            claimed[index] = row["Name"]
            used.add(row["Name"])
    for index, row in enumerate(poses):
        source = row.get("RenamedFrom")
        if index not in claimed and source in current and source not in used:
            claimed[index] = source
            used.add(source)
    return claimed


def pose_ops(poses, live):
    """Remove / rename / add ops; the authored order must be the order the editor keeps. A Pose Asset without a source
    animation adds a pose that is not in the asset from the reference pose, at the end of the list."""
    current = list(live.get("poses", []))
    claimed = claim_poses(poses, current)
    can_add = not live.get("source_animation")
    added = [index for index in range(len(poses)) if index not in claimed]
    if added and not can_add:
        index = added[0]
        raise _error(f"/Body/Poses/{index}", f"Pose '{poses[index]['Name']}' does not exist. This Pose Asset reads its "
                     f"poses from SourceAnimation ({live.get('source_animation')}); the Pose Asset editor adds poses only "
                     f"to a Pose Asset without a source animation. Poses: {', '.join(current) or '(none)'}. Use "
                     "RenamedFrom to rename one.", "PoseAsset.PoseAddUnsupported")
    order = [current.index(claimed[index]) for index in range(len(poses)) if index in claimed]
    if order != sorted(order) or (added and any(index in claimed for index in range(added[0], len(poses)))):
        raise _error("/Body/Poses", "The Pose Asset editor cannot reorder poses (a new one is added at the end); list "
                     f"them in the asset's order: {', '.join(current)}.", "PoseAsset.PoseOrderUnsupported")
    used = set(claimed.values())
    removed = [name for name in current if name not in used]
    ops = [({"op": "remove", "names": removed}, "/Body/Poses")] if removed else []
    ops += [({"op": "rename", "name": claimed[index], "to": row["Name"]}, f"/Body/Poses/{index}")
            for index, row in enumerate(poses) if index in claimed and claimed[index] != row["Name"]]
    ops += [({"op": "add_reference_pose", "name": poses[index]["Name"]}, f"/Body/Poses/{index}") for index in added]
    return ops


def curve_ops(curves, live):
    """The pose list's Delete Curves for the curves the document leaves out; curves cannot be added."""
    current = list(live.get("curves", []))
    for index, name in enumerate(curves):
        if name not in current:
            raise _error(f"/Body/Curves/{index}", f"The Pose Asset has no curve '{name}'; its curves come from the source "
                         f"animation and cannot be added. Curves: {', '.join(current) or '(none)'}.",
                         "PoseAsset.CurveAddUnsupported")
    removed = [name for name in current if name not in curves]
    return [({"op": "remove_curves", "names": removed}, "/Body/Curves")] if removed else []


def _kept_curves(values, kept):
    return {name: value for name, value in values.items() if kept is None or name in kept}


def check_pose_data(body, live, data):
    """A pose's Curves and Bones are read only: an existing pose's must be the asset's (curves the document deletes are
    not compared), a new pose (from the reference pose) has none."""
    poses = body.get("Poses", [])
    current = list(live.get("poses", []))
    claimed = claim_poses(poses, current)
    kept = set(body["Curves"]) if "Curves" in body else None
    for index, row in enumerate(poses):
        for key in ("Curves", "Bones"):
            if key not in row:
                continue
            where = f"/Body/Poses/{index}/{key}"
            if index not in claimed:
                raise _error(where, f"Pose '{row['Name']}' is new: it is added from the reference pose, so its {key} are "
                             "read from it; leave them out.", "PoseAsset.PoseDataReadOnly")
            have = data.get(claimed[index], {}).get(key, {})
            mine = row[key]
            if key == "Curves":
                mine, have = _kept_curves(mine, kept), _kept_curves(have, kept)
            if not common.same(mine, have, exact=False):
                raise _error(where, f"The {key} of pose '{row['Name']}' are read only (the saved pose data; the editor "
                             "edits a pose from the viewport). Leave them as extracted, or leave them out.",
                             "PoseAsset.PoseDataReadOnly")


def check_source_change(body):
    """A new SourceAnimation (Update Source) re-reads every pose and the curve list: pose data and a Curves list
    written for the old source are stale (Curves would remove the new source's curves it leaves out)."""
    if not body.get("SourceAnimation"):
        return
    stale = _has_pose_data(body)
    if stale is not None:
        raise _error(f"/Body/Poses/{stale}", "A new SourceAnimation re-reads every pose (Update Source), so the "
                     "poses' Curves and Bones of the old source cannot be kept: leave them out, and extract again "
                     "afterwards.", "PoseAsset.PoseDataReadOnly")
    if "Curves" in body:
        raise _error("/Body/Curves", "A new SourceAnimation re-reads the curves (Update Source), so a Curves list of "
                     "the old source is stale: it would remove the new source's curves it leaves out. Leave Curves out, "
                     "and extract again afterwards.", "PoseAsset.PoseDataReadOnly")


def _has_pose_data(body):
    return next((index for index, row in enumerate(body.get("Poses", [])) if "Curves" in row or "Bones" in row), None)


def map_path(primitive_path, op_paths):
    parts = [part for part in (primitive_path or "").split("/") if part]
    if len(parts) >= 2 and parts[0] == "ops" and parts[1].isdigit() and int(parts[1]) < len(op_paths):
        path = op_paths[int(parts[1])]
        if len(parts) > 2 and parts[2] == "base_pose":
            return "/Body/BasePose"
        return path
    return "/Body"


def edit(calls, ops):
    changed = []
    for chunk in common.chunk_ops(ops):
        op_paths = [path for _, path in chunk]
        outcome = common.invoke(calls.context, calls.by_id, anim.POSE_ASSET_EDIT,
                                {"schema_version": 1, "ops": [op for op, _ in chunk]},
                                (common.NamedOpaqueHandleArgument("asset", calls.write),),
                                paths=lambda where, op_paths=op_paths: map_path(where, op_paths), prefix=PREFIX)
        changed.extend(outcome.changed_set)
    return changed


def apply(document, read, mutable, context):
    body = canonical_body(document.get("Body", {}))
    properties = document.get("Properties", {})
    region = _properties()
    region.check_owned(properties)
    calls = anim.AnimCalls(context, BY_ID, read, mutable, PREFIX)
    live = read_live(calls)
    anim.require_unchanged(body, "Skeleton", live.get("skeleton", ""), PREFIX,
                           "A Pose Asset uses its source animation's Skeleton")
    changes = []
    if "SourceAnimation" in body and body["SourceAnimation"] != live.get("source_animation", ""):
        check_source_change(body)
        if _has_pose_data(body) is not None:
            check_pose_data(body, live, read_pose_data(calls))
        # "" clears the Source Animation row (the poses stay); a path is the row set, then Update Source.
        op = ({"op": "update_source", "animation": body["SourceAnimation"]} if body["SourceAnimation"]
              else {"op": "clear_source"})
        changes += edit(calls, [(op, "/Body/SourceAnimation")])
        live = read_live(calls)
    elif _has_pose_data(body) is not None:
        check_pose_data(body, live, read_pose_data(calls))
    ops = curve_ops(body["Curves"], live) if "Curves" in body else []
    ops += pose_ops(body["Poses"], live) if "Poses" in body else []
    names_after = [row["Name"] for row in body["Poses"]] if "Poses" in body else list(live.get("poses", []))
    additive = body.get("Additive", bool(live.get("additive")))
    # The base pose is kept by index, so a rename carries it (omitted BasePose = keep).
    live_base = live.get("base_pose", "")
    renamed_base = next((row["Name"] for row in body.get("Poses", []) if row.get("RenamedFrom") == live_base
                         and live_base not in names_after), live_base)
    base = body.get("BasePose", renamed_base)
    if base and base not in names_after:
        raise _error("/Body/BasePose", f"BasePose '{base}' is not a pose of the asset ({', '.join(names_after) or 'none'}); "
                     "use \"\" for the reference pose.", "PoseAsset.PoseNotFound")
    if additive != bool(live.get("additive")) or (additive and base != renamed_base):
        ops.append(({"op": "additive", "additive": additive, "base_pose": base}, "/Body/Additive"))
    if "PreviewMesh" in body and body["PreviewMesh"] != live.get("preview_mesh", ""):
        ops.append(({"op": "preview_mesh", "path": body["PreviewMesh"]}, "/Body/PreviewMesh"))
    if ops:
        changes += edit(calls, ops)
    changes.extend(region.apply(properties, read, mutable, context))
    return tuple(changes)


def create_target_context(document, context):
    body = canonical_body(document.get("Body", {}))
    if not body.get("SourceAnimation"):
        raise _error("/Body/SourceAnimation", "Creating a Pose Asset requires Body.SourceAnimation (the animation the "
                     "Create Pose Asset dialog reads one pose per frame from).", "PoseAsset.SourceAnimationRequired")
    return anim.resolve_context(context, BY_ID, body["SourceAnimation"], "/Body/SourceAnimation", anim.ANIM_SEQUENCE_CLASS,
                                PREFIX, "SourceAnimation")


def extract(target, read, context, include_defaults=True):
    calls = anim.AnimCalls(context, BY_ID, read, prefix=PREFIX)
    body = body_from_live(read_live(calls), read_pose_data(calls))
    return common.document(target, CLASS, _properties().extract(read, context, include_defaults), body)


def diff(authored, extracted, exact):
    changes = common.header_diff(authored, extracted)
    body = canonical_body(authored.get("Body", {}))
    live = canonical_body(extracted.get("Body", {}))
    for key in ("SourceAnimation", "Skeleton", "PreviewMesh", "Additive", "BasePose"):
        if key in body and body[key] != live.get(key):
            changes.append(Difference(f"/Body/{key}", DiffKind.REPLACE, True, body[key], key in live, live.get(key)))
    if "Poses" in body and [row["Name"] for row in body["Poses"]] != [row["Name"] for row in live.get("Poses", [])]:
        changes.append(Difference("/Body/Poses", DiffKind.REPLACE, True, body["Poses"], True, live.get("Poses")))
    if "Curves" in body and set(body["Curves"]) != set(live.get("Curves", [])):
        changes.append(Difference("/Body/Curves", DiffKind.REPLACE, True, body["Curves"], True, live.get("Curves")))
    changes.extend(_pose_data_diff(body, live, exact))
    changes.extend(common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact))
    return tuple(changes)


def _pose_data_diff(body, live, exact):
    """A pose's Curves / Bones the document gives, against the asset's pose of that name (curves the document's
    Curves leaves out are not compared)."""
    have = {row["Name"]: row for row in live.get("Poses", [])}
    kept = set(body["Curves"]) if "Curves" in body else None
    changes = []
    for index, row in enumerate(body.get("Poses", [])):
        if row["Name"] not in have:
            continue
        for key in ("Curves", "Bones"):
            if key not in row:
                continue
            mine, theirs = row[key], have[row["Name"]].get(key, {})
            if key == "Curves":
                mine, theirs = _kept_curves(mine, kept), _kept_curves(theirs, kept)
            if not common.same(mine, theirs, exact):
                changes.append(Difference(f"/Body/Poses/{index}/{key}", DiffKind.REPLACE, True, mine, True, theirs))
    return changes


class PoseAssetRuntimeProfile:
    profile_id = PROFILE_ID
    exact_asset_class_path = CLASS
    runtime_entry_module = "asset_document/bootstrap.py"
    preflight_scratch = False

    def required_primitives(self):
        return REQUIREMENTS

    def inspect_profile(self):
        return {"Class": CLASS, "BodySections": list(BODY_KEYS), "ReadOnlySections": ["Skeleton"],
                "CreateContext": "Body.SourceAnimation"}

    def create_template(self, target):
        return common.document(common.require_target(target, PREFIX), CLASS, {},
                               {"SourceAnimation": "/Game/Anims/Face_Poses.Face_Poses", "Additive": False, "BasePose": ""})

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
            raise _error("/Inspect", "Pose Asset inspection reads a live Pose Asset.", "PoseAsset.InspectNotAdmitted")
        document = self.extract(target=exact_asset_object_path.rsplit(".", 1)[0], asset_handle=editor_asset_handle,
                                context=context, include_defaults=True)
        live = read_live(anim.AnimCalls(context, BY_ID, editor_asset_handle, prefix=PREFIX))
        return {**canonicalize(document),
                "PoseAssetDetails": {"Curves": list(live.get("curves", [])), "TrackCount": live.get("track_count", 0),
                                     "Note": "Body.Poses lists each pose's non-zero curve values and its bone offsets "
                                             "from the reference pose (read only)."}}

    def diff(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=False)

    def diff_exact(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=True)


def create_pose_asset_runtime_handler():
    return PoseAssetRuntimeProfile()


POSE_ASSET_PROFILE_META = ProfileMeta(
    PROFILE_ID, anim.ROUTE, CLASS,
    tuple(RegionMeta(f"pose_asset.{name}.v1", f"/Body/{key}", RegionOwnership.MANAGED, identity, order,
                     DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE, DeletePolicy.EXPLICIT_ONLY, comparison)
          for name, key, identity, order, comparison in (
              ("source_animation", "SourceAnimation", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
              ("preview_mesh", "PreviewMesh", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
              ("poses", "Poses", IdentityPolicy.STABLE_KEY, OrderPolicy.PRESERVE, ComparisonPolicy.ORDERED),
              ("curves", "Curves", IdentityPolicy.STABLE_KEY, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
              ("additive", "Additive", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
              ("base_pose", "BasePose", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL))),
    REQUIREMENTS)
POSE_ASSET_RUNTIME_REGISTRATION = RuntimeProfileRegistration(
    POSE_ASSET_PROFILE_META, create_pose_asset_runtime_handler, RuntimeTargetPolicy.CREATE_OR_UPDATE, LIFECYCLE)

__all__ = ("POSE_ASSET_RUNTIME_REGISTRATION", "PoseAssetRuntimeProfile", "pose_ops")
