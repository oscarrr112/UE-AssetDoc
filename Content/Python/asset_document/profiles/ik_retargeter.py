"""Registered IK Retargeter Profile (profile.ik_retargeter.v1, class /Script/IKRig.IKRetargeter); editable.

Document (MCP/schemas/assetdocument/IKRig.md)::

    "Body": {
      "Source": {"IKRig": "/Game/Rigs/IK_Manny.IK_Manny"},
      "Target": {"IKRig": "/Game/Rigs/IK_Hero.IK_Hero", "PreviewMesh": "/Game/Meshes/SKM_Hero.SKM_Hero"},
      "Ops": [
        {"Name": "Pelvis Motion", "Type": "PelvisMotion", "Settings": {"TranslationAlpha": 0.9}},
        {"Name": "FK Chains", "Type": "FKChains", "ChainMapping": {"Spine": "Spine", "LeftArm": null},
         "Chains": {"Spine": {"RotationMode": "MatchChain"}}},
        {"Name": "Run IK Rig", "Type": "RunIKRig", "ChainMapping": {"LeftLeg": "LeftLeg"},
         "Children": [{"Name": "Retarget IK Goals", "Type": "IKChains"}]}],
      "Poses": {"Source": {"Current": "Default Pose", "Poses": {"Default Pose": {}}},
                "Target": {"Current": "A-Pose", "Poses": {"Default Pose": {},
                           "A-Pose": {"RootOffset": 2.5, "Bones": {"upperarm_l": {"X": 0, "Y": 0, "Z": -0.34, "W": 0.94}}}}}}
    }

* ``Source`` / ``Target``: the side's IK Rig and preview mesh. ``PreviewMesh`` is listed only when it differs from
  the rig's preview mesh (picking a rig sets that mesh; ``null`` = no preview mesh).
* Ops are the op stack as the editor shows it: top-level ops in stack order, child ops under ``Children``.
  ``Settings`` is sparse over a new op of that type added to this retargeter (so auto-filled bones are omitted
  when they equal what the editor fills in); ``Chains`` lists per target chain the chain settings that differ from
  the defaults; ``TargetIKRig`` appears only when an op uses another rig than the default target rig;
  ``ChainMapping`` (target chain -> source chain or null) is kept as it is when a document leaves it out.
* Poses per side: ``Current`` and the named poses (``Default Pose`` always), each a vertical ``RootOffset`` and the
  local rotation offsets of ``Bones`` (quaternions; unlisted bones keep the reference pose).
* Retarget profiles are read only (Inspect lists them; ``IKRetargeter.ProfilesNotAuthored``). Inspect adds the
  editor's automatic setup as ``Suggestions`` (auto-align, auto-map); the Body is applied by
  ``ik_retargeter_apply``.
"""
from __future__ import annotations

import math

from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, IdentityPolicy, OmissionPolicy, OrderPolicy, ProfileMeta,
    RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document.profiles import data_table_common as common
from asset_document.profiles import ik_common as ik

PROFILE_ID = "profile.ik_retargeter.v1"
CLASS = ik.RETARGETER_CLASS
PREFIX = "IKRetargeter"
BODY_KEYS = ("Source", "Target", "Ops", "Poses")
SIDE_KEYS = ("IKRig", "PreviewMesh")
OP_KEYS = ("Name", "Type", "Enabled", "RenamedFrom", "Parent", "Settings", "ChainMapping", "Chains", "TargetIKRig", "Children")
POSE_SET_KEYS = ("Current", "Poses")
POSE_KEYS = ("RootOffset", "Bones", "RenamedFrom")
OP_DEFAULTS = {"Enabled": True, "Parent": "", "Settings": {}, "Chains": {}, "Children": [], "TargetIKRig": ""}
POSE_DEFAULTS = {"RootOffset": 0.0, "Bones": {}}
DEFAULT_POSE = "Default Pose"
BODY_OWNED = {"SourceIKRigAsset": "Body/Source/IKRig", "SourcePreviewMesh": "Body/Source/PreviewMesh",
              "TargetIKRigAsset": "Body/Target/IKRig", "TargetPreviewMesh": "Body/Target/PreviewMesh",
              "RetargetOps": "Body/Ops", "SourceRetargetPoses": "Body/Poses/Source",
              "TargetRetargetPoses": "Body/Poses/Target", "CurrentSourceRetargetPose": "Body/Poses/Source/Current",
              "CurrentTargetRetargetPose": "Body/Poses/Target/Current"}
NOT_AUTHORED = ("Controller", "Version", "BoneDrawSize", "Profiles", "CurrentProfile", "RetargetPoses",
                "CurrentRetargetPose", "ThumbnailInfo")


# Default of an omitted side PreviewMesh: the side's IK Rig's preview mesh (what picking the rig stores).
RIG_MESH = "(the IK Rig's preview mesh)"
SIDE_DEFAULTS = {"IKRig": None, "PreviewMesh": RIG_MESH}


def error(path, message, code):
    return common.error(path, message, code)


def _properties():
    profiles = ("IKRetargeter.ProfilesNotAuthored", "Retarget profiles are read only: Inspect lists them (Profiles), "
                "a document cannot change them (the 5.7 editor has no UI for them).")
    return ik.RigProperties(CLASS, BODY_OWNED, NOT_AUTHORED, PREFIX,
                            {"Profiles": profiles, "CurrentProfile": profiles})


# ---------------------------------------------------------------------------
# Document shape
# ---------------------------------------------------------------------------
def _validate_ops(ops, path, names, child=False):
    if type(ops) is not list:
        raise error(path, "Ops is an ordered list of ops." if not child else "Children is an ordered list of ops.",
                    "IKRetargeter.InvalidBody")
    for index, entry in enumerate(ops):
        where = f"{path}/{index}"
        ik.check_keys(ik.object_value(entry, where, "An op", PREFIX), OP_KEYS, where, "op", PREFIX)
        name = ik.name_value(entry.get("Name"), f"{where}/Name", "An op's Name", PREFIX)
        ik.name_value(entry.get("Type"), f"{where}/Type", "An op's Type", PREFIX)
        if name.lower() in names:
            raise error(f"{where}/Name", f"Op names are unique (case-insensitive): '{name}' is also {names[name.lower()]}.",
                        "IKRetargeter.DuplicateName")
        names[name.lower()] = where
        if "Enabled" in entry and type(entry["Enabled"]) is not bool:
            raise error(f"{where}/Enabled", "Enabled is true or false.", "IKRetargeter.InvalidBody")
        for key in ("Parent", "RenamedFrom"):
            if key in entry:
                ik.name_value(entry[key], f"{where}/{key}", key, PREFIX, allow_empty=key == "Parent")
        if "TargetIKRig" in entry and entry["TargetIKRig"] not in ("", None):
            ik.path_or_null(entry["TargetIKRig"], f"{where}/TargetIKRig", "TargetIKRig", PREFIX)
        for key in ("Settings", "ChainMapping", "Chains"):
            if key in entry:
                ik.object_value(entry[key], f"{where}/{key}", key, PREFIX)
        for chain, row in entry.get("Chains", {}).items():
            ik.object_value(row, f"{where}/Chains/{ik.escape(chain)}", "A chain's settings", PREFIX)
        for chain, source in entry.get("ChainMapping", {}).items():
            if source is not None and type(source) is not str:
                raise error(f"{where}/ChainMapping/{ik.escape(chain)}", "A chain mapping maps a target chain to a source "
                            "chain name or null (unmapped).", "IKRetargeter.InvalidBody")
        if "Children" in entry:
            if child:
                raise error(f"{where}/Children", "Only a top-level op (Run IK Rig) has child ops.",
                            "IKRetargeter.InvalidParentOp")
            _validate_ops(entry["Children"], f"{where}/Children", names, child=True)


QUAT_KEYS = ("X", "Y", "Z", "W")
QUAT_NORMALIZE_TOLERANCE = 1.0e-4   # |size^2 - 1| above this is normalized on write (rig.ikretargeter.edit.v1)
ROTATION_TOLERANCE = 1.0e-6         # a converted rotation (rotator, normalized quaternion) equals the stored one


def read_bones(bones):
    """Pose bone rotations as read; -0 reads as 0 (a JSON document cannot keep the sign of zero)."""
    return {bone: {key: value + 0.0 for key, value in rotation.items()} for bone, rotation in bones.items()}


def rotation_quat(rotation):
    """(quaternion, converted): the quaternion the edit primitive stores for a document rotation. A rotator converts
    as FRotator::Quaternion and an approximate quaternion is normalized (FQuat::Normalize); both are ``converted``:
    the engine's math may differ from this one in the last digits."""
    if "X" in rotation or "W" in rotation:
        quat = {key: float(rotation.get(key, 0.0)) + 0.0 for key in QUAT_KEYS}
        size = sum(value * value for value in quat.values())
        if size == 0.0 or abs(size - 1.0) <= QUAT_NORMALIZE_TOLERANCE:
            return quat, False
        scale = 1.0 / math.sqrt(size)
        return {key: value * scale for key, value in quat.items()}, True
    half = [math.fmod(float(rotation.get(key, 0.0)), 360.0) * math.pi / 360.0 for key in ("Pitch", "Yaw", "Roll")]
    (sp, sy, sr), (cp, cy, cr) = [math.sin(angle) for angle in half], [math.cos(angle) for angle in half]
    return {"X": cr * sp * sy - sr * cp * cy + 0.0, "Y": -cr * sp * cy - sr * cp * sy + 0.0,
            "Z": cr * cp * sy - sr * sp * cy + 0.0, "W": cr * cp * cy + sr * sp * sy + 0.0}, True


def canonical_bones(bones, live):
    """Authored pose ``Bones`` as the quaternions they store, compared with the ``live`` ones: a quaternion equal to
    the live one is kept, a converted rotation within ROTATION_TOLERANCE of the live rotation is that rotation;
    identity rotations are not listed (as read)."""
    result = {}
    for bone, rotation in bones.items():
        if type(rotation) is not dict:
            result[bone] = rotation
            continue
        have = live.get(bone)
        if type(have) is dict and set(rotation) == set(QUAT_KEYS) and all(
                type(rotation[key]) in (int, float) and rotation[key] == have.get(key) for key in QUAT_KEYS):
            result[bone] = dict(have)   # the rotation the pose holds is kept as it is (not re-normalized)
            continue
        quat, converted = rotation_quat(rotation)
        if converted and type(have) is dict and all(
                type(have.get(key)) in (int, float) and abs(quat[key] - have[key]) <= ROTATION_TOLERANCE for key in QUAT_KEYS):
            quat = dict(have)
        if quat != {"X": 0.0, "Y": 0.0, "Z": 0.0, "W": 1.0}:
            result[bone] = quat
    return result


def canonical_poses(poses, live):
    """A side's authored ``Poses`` with canonical ``Bones`` (see canonical_bones) against the live side's poses."""
    have = {name.lower(): pose for name, pose in live.items()}
    result = {}
    for name, pose in poses.items():
        if type(pose) is dict and type(pose.get("Bones")) is dict:
            theirs = have.get(name.lower(), {}) if type(have.get(name.lower())) is dict else {}
            pose = {**pose, "Bones": canonical_bones(pose["Bones"], theirs.get("Bones", {}))}
        result[name] = pose
    return result


def _validate_bones(bones, path):
    for bone, rotation in ik.object_value(bones, path, "Bones", PREFIX).items():
        at = f"{path}/{ik.escape(bone)}"
        ik.object_value(rotation, at, "A bone rotation offset", PREFIX)
        keys = set(rotation)
        if not (keys == {"X", "Y", "Z", "W"} or (keys and keys <= {"Pitch", "Yaw", "Roll"})) or not all(
                type(value) in (int, float) for value in rotation.values()):
            raise error(at, "A bone rotation offset is a quaternion {X, Y, Z, W} or a rotator {Pitch, Yaw, Roll} "
                        "(degrees).", "IKRetargeter.InvalidBody")


def validate_body(body):
    path = "/Body"
    for side in ("Source", "Target"):
        if side in body:
            value = ik.object_value(body[side], f"{path}/{side}", side, PREFIX)
            ik.check_keys(value, SIDE_KEYS, f"{path}/{side}", side, PREFIX)
            for key in SIDE_KEYS:
                if key in value:
                    ik.path_or_null(value[key], f"{path}/{side}/{key}", key, PREFIX)
    if "Ops" in body:
        _validate_ops(body["Ops"], f"{path}/Ops", {})
    if "Poses" in body:
        poses = ik.object_value(body["Poses"], f"{path}/Poses", "Poses", PREFIX)
        ik.check_keys(poses, ("Source", "Target"), f"{path}/Poses", "Poses", PREFIX)
        for side, pose_set in poses.items():
            where = f"{path}/Poses/{side}"
            ik.check_keys(ik.object_value(pose_set, where, "A pose set", PREFIX), POSE_SET_KEYS, where, "pose set",
                          PREFIX)
            named = ik.object_value(pose_set.get("Poses", {}), f"{where}/Poses", "Poses", PREFIX)
            seen = {}
            for name, pose in named.items():
                at = f"{where}/Poses/{ik.escape(name)}"
                if not name:
                    raise error(at, "A pose has a name.", "IKRetargeter.InvalidBody")
                if name.lower() in seen:
                    raise error(at, f"Pose names are unique (case-insensitive): '{name}' and '{seen[name.lower()]}'.",
                                "IKRetargeter.DuplicateName")
                seen[name.lower()] = name
                ik.check_keys(ik.object_value(pose, at, "A pose", PREFIX), POSE_KEYS, at, "pose", PREFIX)
                if "RootOffset" in pose and type(pose["RootOffset"]) not in (int, float, dict):
                    raise error(f"{at}/RootOffset", "RootOffset is the root's vertical offset (a number).",
                                "IKRetargeter.InvalidBody")
                if "Bones" in pose:
                    _validate_bones(pose["Bones"], f"{at}/Bones")
                if "RenamedFrom" in pose:
                    ik.name_value(pose["RenamedFrom"], f"{at}/RenamedFrom", "RenamedFrom", PREFIX)
            if "Poses" in pose_set and DEFAULT_POSE not in named:
                raise error(f"{where}/Poses", f"Every side keeps its '{DEFAULT_POSE}' (the pose menu cannot remove it): "
                            f"list it (as {{}} for the reference pose).", "IKRetargeter.DefaultPoseRequired")
            if "Current" in pose_set:
                current = ik.name_value(pose_set["Current"], f"{where}/Current", "Current", PREFIX)
                if "Poses" in pose_set and current not in named:
                    raise error(f"{where}/Current", f"Current names one of the side's Poses ('{current}' is not listed)."
                                f"{ik.describe_candidates(current, sorted(named), 'Poses')}", "IKRetargeter.PoseNotFound")


def canonicalize(document):
    body = common.validate_header(document, (CLASS,), PREFIX, BODY_KEYS)
    validate_body(body)
    canonical = common.snapshot(document)
    canonical.setdefault("Definitions", {})
    canonical.setdefault("Properties", {})
    canonical.setdefault("Body", {})
    return canonical


# ---------------------------------------------------------------------------
# Read records -> document
# ---------------------------------------------------------------------------
def _side(value):
    side = {"IKRig": value.get("ik_rig") or None}
    stored, rig_mesh = value.get("preview_mesh") or "", value.get("rig_mesh") or ""
    if stored != rig_mesh:
        side["PreviewMesh"] = stored or None
    return side


def assemble(records):
    """Body and Inspect facts of rig.ikretargeter.read.v1 records."""
    header = records[0]
    body = {"Source": _side(header.get("source", {})), "Target": _side(header.get("target", {}))}
    facts = {"profiles": {"Names": header.get("profiles", []), "Current": header.get("current_profile", "")},
             "ops": {}, "chains": {}, "defaults": {},
             "sides": {"Source": header.get("source", {}), "Target": header.get("target", {})}}
    entries, order, poses = {}, [], {}
    for record in records[1:]:
        kind = record.get("kind")
        if kind == "op":
            entries[record["index"]] = dict(record["entry"])
            order.append(record["index"])
            if "settings_defaults" in record:
                facts["defaults"][record["entry"]["Name"]] = {"Settings": record["settings_defaults"],
                                                              "Chains": record.get("chain_defaults", {})}
        elif kind == "op_chains":
            entries[record["index"]].setdefault("Chains", {}).update(record["chains"])
        elif kind == "pose":
            pose_set = poses.setdefault(record["side"], {"Current": "", "Poses": {}})
            pose = {}
            if "root_offset" in record:
                pose["RootOffset"] = record["root_offset"]
            pose_set["Poses"][record["name"]] = pose
            if record.get("current"):
                pose_set["Current"] = record["name"]
        elif kind == "pose_bones":
            poses[record["side"]]["Poses"][record["name"]].setdefault("Bones", {}).update(read_bones(record["bones"]))
        elif kind == "op_facts":
            facts["ops"][record["index"]] = record
        elif kind == "chains":
            facts["chains"][record["side"]] = {"IKRig": record.get("rig", ""), "Chains": record.get("chains", [])}
    # Nest child ops under their parent (the op stack panel; the stack keeps children before their parent), keeping
    # stack order. A child whose parent is missing stays top-level and keeps its Parent.
    names = {entries[index]["Name"] for index in order}
    ops, children = [], {}
    for index in order:
        entry = entries[index]
        parent = entry.get("Parent", "")
        if parent and parent != entry["Name"] and parent in names:
            del entry["Parent"]
            children.setdefault(parent, []).append(entry)
        else:
            ops.append(entry)
    for entry in [entries[index] for index in order]:
        if entry["Name"] in children:
            entry["Children"] = children[entry["Name"]]
    body["Ops"] = ops
    body["Poses"] = {side: poses[side] for side in ("Source", "Target") if side in poses}
    return body, facts


def read_body(calls, detail="document"):
    return assemble(ik.read_records(calls, ik.RETARGETER_READ, detail))


def extract(target, read, context, include_defaults=True):
    body, _ = read_body(ik.RigCalls(context, read, prefix=PREFIX))
    return common.document(target, CLASS, _properties().extract(read, context, include_defaults), body)


def extract_with_defaults(target, read, context):
    """The document and, per op name, the defaults its sparse Settings and Chains are read over (what "Add New"
    makes for this retargeter; a Root Motion op's bones are filled in from the preview meshes)."""
    body, facts = read_body(ik.RigCalls(context, read, prefix=PREFIX), "verify")
    return common.document(target, CLASS, _properties().extract(read, context, True), body), facts["defaults"]


# ---------------------------------------------------------------------------
# Diff
# ---------------------------------------------------------------------------
def _ops_diff(path, authored, live, exact, changes, live_defaults=None):
    for index in range(max(len(authored), len(live))):
        where = f"{path}/{index}"
        if index >= len(live):
            changes.append(common.Difference(where, common.DiffKind.ADD, True, authored[index], False, None))
        elif index >= len(authored):
            changes.append(common.Difference(where, common.DiffKind.REMOVE, False, None, True, live[index]))
        else:
            mine = {key: value for key, value in authored[index].items() if key not in ("RenamedFrom", "Children")}
            # A ChainMapping the document leaves out is kept as it is (the editor auto-maps new chains).
            theirs = {key: value for key, value in live[index].items()
                      if key != "Children" and (key != "ChainMapping" or key in mine)}
            defaults = (live_defaults or {}).get(live[index].get("Name"))
            if defaults is not None and mine.get("Type") == theirs.get("Type"):
                # Settings and chain rows are sparse over the op's defaults on both sides.
                ik.sparse_diff(f"{where}/Settings", mine.pop("Settings", {}), theirs.pop("Settings", {}),
                               defaults["Settings"], exact, changes)
                ik.sparse_map_diff(f"{where}/Chains", mine.pop("Chains", {}), theirs.pop("Chains", {}),
                                   defaults["Chains"], exact, changes)
            ik.value_diff(where, mine, theirs, exact, changes, OP_DEFAULTS)
            _ops_diff(f"{where}/Children", authored[index].get("Children", []), live[index].get("Children", []),
                      exact, changes, live_defaults)


def body_diff(authored, live, exact, live_defaults=None):
    changes = []
    for key in BODY_KEYS:
        if key not in authored:
            continue
        where = f"/Body/{key}"
        if key in ("Source", "Target"):
            ik.value_diff(where, authored[key], live.get(key, {}), exact, changes, SIDE_DEFAULTS)
        elif key == "Ops":
            _ops_diff(where, authored[key], live.get(key, []), exact, changes, live_defaults)
        elif key == "Poses":
            for side, pose_set in authored[key].items():
                theirs = live.get(key, {}).get(side, {"Current": DEFAULT_POSE, "Poses": {}})
                at = f"{where}/{side}"
                if "Current" in pose_set:
                    ik.value_diff(f"{at}/Current", pose_set["Current"], theirs.get("Current", ""), exact, changes)
                if "Poses" in pose_set:
                    ik.map_diff(f"{at}/Poses", canonical_poses(pose_set["Poses"], theirs.get("Poses", {})),
                                theirs.get("Poses", {}), exact, changes, POSE_DEFAULTS)
    return changes


def diff(authored, extracted, exact, live_defaults=None):
    changes = common.header_diff(authored, extracted)
    changes.extend(body_diff(authored.get("Body", {}), extracted.get("Body", {}), exact, live_defaults))
    changes.extend(common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact))
    return tuple(changes)


# ---------------------------------------------------------------------------
# Inspect
# ---------------------------------------------------------------------------
def suggestions(calls):
    """The editor's automatic setup on a transient copy (rig.suggest.v1): Auto Align All Bones (ChainToChain, the
    editor's default method) per side and Auto Map Chains (Exact, Fuzzy) per op with a chain mapping."""
    records = ik.read_records(calls, ik.SUGGEST)
    align, mapping = {}, {}
    for record in records[1:]:
        if record.get("kind") == "align":
            pose = align.setdefault(record["side"], {}).setdefault(record["method"], {})
            if "root_offset" in record:
                pose["RootOffset"] = record["root_offset"]
            pose.setdefault("Bones", {}).update(read_bones(record.get("bones", {})))
        elif record.get("kind") == "map":
            mapping.setdefault(record["op"], {})[record["type"]] = record.get("mapping", {})
    missing = [side for side in ("Source", "Target") if side not in align]
    result = {"AutoAlign": align, "AutoMap": mapping}
    if missing:
        result["AutoAlignUnavailable"] = ("Auto-align needs both sides' IK Rigs and preview meshes "
                                          f"(missing for {', '.join(missing)}).")
    return result


def inspect_payload(document, calls):
    _, facts = read_body(calls, "inspect")
    op_facts = []
    for index in sorted(facts["ops"]):
        record = facts["ops"][index]
        row = {"Index": index, "DisplayName": record.get("display_name", ""), "Singleton": record.get("singleton", False),
               "CanHaveChildren": record.get("can_have_children", False)}
        if record.get("parent_type"):
            row["ParentType"] = record["parent_type"]
        if "mapping_ready" in record:
            row["MappingReady"] = record["mapping_ready"]
        op_facts.append(row)
    # Retarget profiles have no editor UI in 5.7: listed here, not authored (IKRetargeter.ProfilesNotAuthored).
    return {**document, "Profiles": facts["profiles"], "OpFacts": op_facts, "RigChains": facts["chains"],
            "Suggestions": suggestions(calls)}


# ---------------------------------------------------------------------------
# Handler
# ---------------------------------------------------------------------------
class IKRetargeterRuntimeProfile:
    runtime_entry_module = "asset_document/bootstrap.py"
    preflight_scratch = False
    exact_asset_class_path = CLASS
    profile_id = PROFILE_ID

    def required_primitives(self):
        return ik.REQUIREMENTS

    def inspect_profile(self):
        return {"Class": CLASS, "BodySections": list(BODY_KEYS), "ReadOnlySections": []}

    def create_template(self, target):
        return common.document(common.require_target(target, PREFIX), CLASS, {}, {})

    def validate(self, document):
        canonicalize(document)

    def canonicalize(self, document):
        return canonicalize(document)

    def create_target_context(self, document, context):
        canonicalize(document)
        return ()

    def preflight(self, document, scratch_handle, context):
        canonical = canonicalize(document)
        _properties().validate(canonical.get("Properties", {}), context)
        _apply.preflight(canonical, ik.existing(context, canonical, CLASS, PREFIX), context)
        return SNAPSHOT_COVERED

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        return _apply.apply(canonicalize(document), read_asset_handle, mutable_asset_handle, context)

    def repair(self, document, asset_handle, context):
        canonicalize(document)
        return ()

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        return extract(common.require_target(target, PREFIX), asset_handle, context)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != CLASS or editor_asset_handle is None:
            raise error("/Inspect", "IK Retargeter inspection reads a live IK Retargeter.",
                        "IKRetargeter.InspectNotAdmitted")
        document = canonicalize(self.extract(target=exact_asset_object_path.rsplit(".", 1)[0],
                                             asset_handle=editor_asset_handle, context=context))
        return inspect_payload(document, ik.RigCalls(context, editor_asset_handle, prefix=PREFIX))

    def diff(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=False)

    def diff_exact(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=True)

    def extract_with_defaults(self, *, target, asset_handle, context):
        return extract_with_defaults(common.require_target(target, PREFIX), asset_handle, context)

    def diff_with_defaults(self, authored, extracted, live_defaults, exact):
        return diff(canonicalize(authored), canonicalize(extracted), exact=exact, live_defaults=live_defaults)


def _meta():
    rows = [("source", "/Body/Source"), ("target", "/Body/Target"), ("ops", "/Body/Ops"), ("poses", "/Body/Poses")]
    regions = tuple(RegionMeta(f"ik_retargeter.{name}.v1", path, RegionOwnership.MANAGED, IdentityPolicy.STABLE_KEY,
                               OrderPolicy.CANONICAL, DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE,
                               DeletePolicy.EXPLICIT_ONLY, ComparisonPolicy.CANONICAL)
                    for name, path in rows)
    return ProfileMeta(PROFILE_ID, ik.ROUTE, CLASS, regions, ik.REQUIREMENTS)


IK_RETARGETER_RUNTIME_REGISTRATION = RuntimeProfileRegistration(_meta(), IKRetargeterRuntimeProfile,
                                                                RuntimeTargetPolicy.CREATE_OR_UPDATE, ik.LIFECYCLE)

__all__ = ("IK_RETARGETER_RUNTIME_REGISTRATION", "IKRetargeterRuntimeProfile", "assemble", "body_diff",
           "canonicalize", "diff", "suggestions")

# Module-level on purpose (after everything ik_retargeter_apply uses from this module): the native runtime restores
# sys.modules between phase calls, so an in-call import would load a second copy of the primitive catalog.
from asset_document.profiles import ik_retargeter_apply as _apply  # noqa: E402
