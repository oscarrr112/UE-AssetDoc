"""Registered PoseAsset Profile (profile.pose_asset.v1): the Pose Asset editor's poses and additive
settings.

Document (docs/architecture/asset-document-animation-assets-matrix.md)::

    "Body": {
      "SourceAnimation": "/Game/Anims/Face_Poses.Face_Poses",   # create: the Create Pose Asset source;
      "Skeleton": "/Game/Characters/SK_Face_Skeleton.SK_Face_Skeleton",   # read only (the source's)
      "PreviewMesh": "",
      "Poses": ["Neutral", {"Name": "Smile", "RenamedFrom": "Pose_1"}],
      "Additive": true,
      "BasePose": "Neutral"                                       # "" = the reference pose
    },
    "Properties": {"RetargetSource": "None", ...}

* Poses come from the source animation (one per frame) and are renamed or deleted, as in the Pose
  Asset editor; the editor cannot add or reorder poses (``PoseAsset.PoseAddUnsupported``,
  ``PoseAsset.PoseOrderUnsupported``).  Changing ``SourceAnimation`` runs the editor's
  "Update Source": poses are re-read from the new animation, keeping their names.
* ``Additive`` / ``BasePose`` are applied together (the Details panel's Apply button).
* Pose curves and bone tracks are read only (Inspect lists them).

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
BODY_KEYS = ("SourceAnimation", "Skeleton", "PreviewMesh", "Poses", "Additive", "BasePose")
LIFECYCLE = generic_factory_lifecycle(anim.ROUTE, ("factory_context",))
REQUIREMENTS = (anim.requirements(anim.CLASS_RESOLVE, anim.RESOLVE, anim.REFERENCE_RESOLVE, anim.POSE_ASSET_READ,
                                  anim.POSE_ASSET_EDIT)
                + lifecycle_requirements(LIFECYCLE))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}
BODY_OWNED = {"bAdditivePose": "Body.Additive", "SourceAnimation": "Body.SourceAnimation", "Skeleton": "Body.Skeleton",
              "PreviewSkeletalMesh": "Body.PreviewMesh"}


def _error(path, message, code):
    return common.error(path, message, code)


def canonical_body(body):
    out = {}
    for key in ("SourceAnimation", "Skeleton"):
        if key in body:
            out[key] = anim.object_path(body[key], f"/Body/{key}", f"PoseAsset.Invalid{key}", key)
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
                if set(row) - {"Name", "RenamedFrom"}:
                    raise _error(where, "A pose is a name or {\"Name\", \"RenamedFrom\"}.", "PoseAsset.InvalidPoses")
                item = {"Name": validate_name(row.get("Name"), f"{where}/Name", "pose", "PoseAsset.InvalidPoseName")}
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


def body_from_live(live):
    return {"SourceAnimation": live.get("source_animation", ""), "Skeleton": live.get("skeleton", ""),
            "PreviewMesh": live.get("preview_mesh", ""), "Poses": list(live.get("poses", [])),
            "Additive": bool(live.get("additive")), "BasePose": live.get("base_pose", "")}


def pose_ops(poses, live):
    """Rename / remove ops; the authored order must be the order the editor keeps."""
    current = list(live.get("poses", []))
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
    for index, row in enumerate(poses):
        if index not in claimed:
            raise _error(f"/Body/Poses/{index}", f"Pose '{row['Name']}' does not exist; the Pose Asset editor cannot add "
                         f"poses (they come from SourceAnimation). Poses: {', '.join(current) or '(none)'}. Use "
                         "RenamedFrom to rename one.", "PoseAsset.PoseAddUnsupported")
    order = [current.index(claimed[index]) for index in range(len(poses))]
    if order != sorted(order):
        raise _error("/Body/Poses", "The Pose Asset editor cannot reorder poses; list them in the asset's order: "
                     f"{', '.join(current)}.", "PoseAsset.PoseOrderUnsupported")
    removed = [name for name in current if name not in used]
    ops = [({"op": "remove", "names": removed}, "/Body/Poses")] if removed else []
    ops += [({"op": "rename", "name": claimed[index], "to": row["Name"]}, f"/Body/Poses/{index}")
            for index, row in enumerate(poses) if claimed[index] != row["Name"]]
    return ops


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
        changes += edit(calls, [({"op": "update_source", "animation": body["SourceAnimation"]}, "/Body/SourceAnimation")])
        live = read_live(calls)
    ops = pose_ops(body["Poses"], live) if "Poses" in body else []
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
    if "SourceAnimation" not in body:
        raise _error("/Body/SourceAnimation", "Creating a Pose Asset requires Body.SourceAnimation (the animation the "
                     "Create Pose Asset dialog reads one pose per frame from).", "PoseAsset.SourceAnimationRequired")
    return anim.resolve_context(context, BY_ID, body["SourceAnimation"], "/Body/SourceAnimation", anim.ANIM_SEQUENCE_CLASS,
                                PREFIX, "SourceAnimation")


def extract(target, read, context, include_defaults=True):
    live = read_live(anim.AnimCalls(context, BY_ID, read, prefix=PREFIX))
    return common.document(target, CLASS, _properties().extract(read, context, include_defaults), body_from_live(live))


def diff(authored, extracted, exact):
    changes = common.header_diff(authored, extracted)
    body = canonical_body(authored.get("Body", {}))
    live = canonical_body(extracted.get("Body", {}))
    for key in ("SourceAnimation", "Skeleton", "PreviewMesh", "Additive", "BasePose"):
        if key in body and body[key] != live.get(key):
            changes.append(Difference(f"/Body/{key}", DiffKind.REPLACE, True, body[key], key in live, live.get(key)))
    if "Poses" in body and [row["Name"] for row in body["Poses"]] != [row["Name"] for row in live.get("Poses", [])]:
        changes.append(Difference("/Body/Poses", DiffKind.REPLACE, True, body["Poses"], True, live.get("Poses")))
    changes.extend(common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact))
    return tuple(changes)


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
                                     "Note": "Curves and bone tracks are read only; poses come from SourceAnimation."}}

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
              ("additive", "Additive", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
              ("base_pose", "BasePose", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL))),
    REQUIREMENTS)
POSE_ASSET_RUNTIME_REGISTRATION = RuntimeProfileRegistration(
    POSE_ASSET_PROFILE_META, create_pose_asset_runtime_handler, RuntimeTargetPolicy.CREATE_OR_UPDATE, LIFECYCLE)

__all__ = ("POSE_ASSET_RUNTIME_REGISTRATION", "PoseAssetRuntimeProfile", "pose_ops")
