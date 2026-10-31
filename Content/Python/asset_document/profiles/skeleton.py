# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Registered Skeleton Profile (profile.skeleton.v1): the Skeleton editor's authorable surface.

Document (docs/architecture/asset-document-animation-assets-matrix.md)::

    "Body": {
      "SlotGroups": [                                   # Anim Slot Manager (omit = keep)
        {"Name": "DefaultGroup", "Slots": ["DefaultSlot", "UpperBody"]},
        {"Name": "Face", "Slots": [{"Name": "Mouth", "RenamedFrom": "Jaw"}]}
      ],
      "Sockets": [                                      # skeleton tree sockets
        {"Name": "hand_r_weapon", "Bone": "hand_r", "Location": {"X": 0, "Y": 5, "Z": 0},
         "Rotation": {"Pitch": 0, "Yaw": 90, "Roll": 0}, "Scale": {"X": 1, "Y": 1, "Z": 1},
         "ForceAlwaysAnimated": true}
      ],
      "VirtualBones": [{"Name": "VB hand_ik", "Source": "root", "Target": "hand_r"}],
      "BlendProfiles": [{"Name": "UpperBody", "Mode": "WeightFactor", "Bones": {"spine_01": 1.5}}],
      "BoneRetargeting": {"pelvis": "AnimationScaled", "root": "Skeleton"},
      "Curves": [{"Name": "CTRL_Blink", "Material": false, "MorphTarget": true,
                  "LinkedBones": ["eyelid_l"], "MaxLOD": 2}],     # LinkedBones / MaxLOD: left out when unset
      "PreviewMesh": "/Game/Characters/SK_Hero.SK_Hero",  # "" = none
      "AdditionalPreviewMeshes": "",                    # a preview mesh collection, "" = none
      "AnimNotifyNames": ["Footstep"],                  # the Animation Notifies tab (names left out are kept)
      "PreviewAttachedAssets": [{"Asset": "/Game/Props/SM_Sword.SM_Sword", "AttachedTo": "hand_r"}],  # read only
      "RetargetSources": [{"Name": "Tall", "Mesh": "/Game/Characters/SK_Tall.SK_Tall"}]               # read only
    },
    "Properties": {"PreviewForwardAxis": "X", "CompatibleSkeletons": [...], "AssetUserData": [...], ...}

* Each Body section is the full list of its kind (omitted section = keep; ``[]`` / ``{}`` clears what
  the editor can clear).  Names identify entries; ``RenamedFrom`` renames (the editor's rename keeps
  the entry) instead of remove + add.  List order is not meaningful.
* Slots: slot and group names are unique across both; ``DefaultGroup`` is kept once it exists.  A slot that a
  montage or an Animation Blueprint slot node uses cannot be renamed or removed
  (``Skeleton.SlotInUse`` names the users), as in the Anim Slot Manager.
* Sockets: Location / Rotation / Scale / ForceAlwaysAnimated omitted on a new socket = editor
  defaults (0 / 0 / 1 / true), omitted on an existing socket = keep.
* VirtualBones: names start with ``VB `` (the skeleton tree's prefix); Source / Target are bones
  (virtual bones may chain).  Changing Source / Target replaces the virtual bone.
* BlendProfiles: Mode is TimeFactor, WeightFactor or BlendMask; Bones lists the non-default bone
  scales (default 1, BlendMask 0); listed bones that are not given are reset.
* BoneRetargeting: translation retargeting mode per bone (Animation, Skeleton, AnimationScaled,
  AnimationRelative, OrientAndScale); bones not listed use Animation (the default).
* Curves: curve metadata (Material / MorphTarget flags; the Anim Curves tab's bone links: LinkedBones and MaxLOD,
  255 = every LOD).  LinkedBones / MaxLOD omitted on a new curve = none / 255, on an existing curve = keep.  Curve
  data inside animations is not touched.
* PreviewMesh / AdditionalPreviewMeshes: the Skeleton editor's preview mesh and preview mesh collection.
  AnimNotifyNames adds notify names to the Skeleton (the tab's delete also removes notifies from every animation,
  so names left out are kept).  PreviewAttachedAssets (the preview scene's attached assets) and RetargetSources
  (the Retarget Sources tab) are read only.
* ``Properties.AssetUserData`` lists the Skeleton's asset user data except its curve metadata object (that is
  ``Curves``); an apply keeps the curve metadata object.
* The bone hierarchy is imported and read only (Inspect lists it).  Creating a Skeleton is not
  supported (``Skeleton.CreateUnsupported``): it comes from a Skeletal Mesh import.

Writes are covered by Core's package snapshot; preflight returns ``SNAPSHOT_COVERED``.
"""
from __future__ import annotations

from asset_document.diagnostics import read_warning
from asset_document.diff.model import Difference, DiffKind
from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, IdentityPolicy, OmissionPolicy, OrderPolicy, ProfileMeta,
    RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document.profiles import anim_asset_common as anim
from asset_document.tolerance import authored_rule, key as judged_key
from asset_document.profiles import data_table_common as common
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements

PROFILE_ID = "profile.skeleton.v1"
PREFIX = "Skeleton"
CLASS = anim.SKELETON_CLASS
BODY_KEYS = ("SlotGroups", "Sockets", "VirtualBones", "BlendProfiles", "BoneRetargeting", "Curves", "PreviewMesh",
             "AdditionalPreviewMeshes", "AnimNotifyNames", "PreviewAttachedAssets", "RetargetSources")
CURVE_KEYS = ("Name", "Material", "MorphTarget", "LinkedBones", "MaxLOD", "RenamedFrom")
CURVE_DEFAULTS = {"LinkedBones": [], "MaxLOD": 255}
CURVE_METADATA_CLASS = "/Script/Engine.AnimCurveMetaData"
DEFAULT_GROUP = "DefaultGroup"
RETARGETING_MODES = ("Animation", "Skeleton", "AnimationScaled", "AnimationRelative", "OrientAndScale")
BLEND_PROFILE_MODES = ("TimeFactor", "WeightFactor", "BlendMask")
SOCKET_KEYS = ("Name", "Bone", "Location", "Rotation", "Scale", "ForceAlwaysAnimated", "RenamedFrom")
VECTOR_FIELDS = {"Location": ("X", "Y", "Z"), "Rotation": ("Pitch", "Yaw", "Roll"), "Scale": ("X", "Y", "Z")}
SOCKET_DEFAULTS = {"Location": {"X": 0, "Y": 0, "Z": 0}, "Rotation": {"Pitch": 0, "Yaw": 0, "Roll": 0},
                   "Scale": {"X": 1, "Y": 1, "Z": 1}, "ForceAlwaysAnimated": True}
LIFECYCLE = generic_factory_lifecycle(anim.ROUTE)
REQUIREMENTS = (anim.requirements(anim.CLASS_RESOLVE, anim.RESOLVE, anim.SKELETON_READ, anim.SKELETON_EDIT)
                + lifecycle_requirements(LIFECYCLE))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}
# Skeleton UPROPERTYs the Body (or nothing: the imported bone tree) owns.
BODY_OWNED = {"BoneTree": "the imported bone hierarchy (read only; Inspect lists it)",
              "Sockets": "Body.Sockets", "VirtualBones": "Body.VirtualBones", "BlendProfiles": "Body.BlendProfiles",
              "SlotGroups": "Body.SlotGroups", "PreviewSkeletalMesh": "Body.PreviewMesh",
              "AdditionalPreviewSkeletalMeshes": "Body.AdditionalPreviewMeshes",
              "AnimationNotifies": "Body.AnimNotifyNames",
              "PreviewAttachedAssetContainer": "Body.PreviewAttachedAssets (read only)"}


def _error(path, message, code):
    return common.error(path, message, code)


def _refuse_create():
    raise _error("/Target", "Creating a Skeleton is not supported: a Skeleton is created by importing a Skeletal "
                 "Mesh (or from a mesh in the editor). Use Action Update on an existing Skeleton; for a new one, "
                 "copy an existing Skeleton (engine and plugin ones too) into /Game with duplicate_asset.",
                 "Skeleton.CreateUnsupported")


def validate_name(value, path, what, code="Skeleton.InvalidName", reference=False, as_key=False):
    """A name as AssetDoc writes new ones. Names imported from a DCC tool can break the spacing and length rules; a
    name the asset already holds is read as it is (``tolerance.authored_rule``). ``reference``: a name that refers to
    an existing one (``RenamedFrom``), not judged; ``as_key``: the name is the object member key at ``path``."""
    message = f"A {what} name is a non-empty single-line name without leading or trailing spaces (got {value!r})."
    if type(value) is not str or not value.strip() or value.lower() == "none":
        raise _error(path, message, code)
    if not reference and (value.strip() != value or len(value) >= 1024 or any(char in value for char in "\n\r\t")):
        authored_rule(path, code, message, judged=(judged_key(path),) if as_key else None)
    return value


# ---------------------------------------------------------------------------
# Document shape
# ---------------------------------------------------------------------------
def _entries(rows, path, what, keys, item_of):
    """A named list: canonical items, unique names and RenamedFrom sources."""
    if type(rows) is not list:
        raise _error(path, f"{path.rsplit('/', 1)[-1]} is a list of {what} objects.", f"Skeleton.Invalid{what.title().replace(' ', '')}")
    names, sources, out = set(), set(), []
    for index, row in enumerate(rows):
        where = f"{path}/{index}"
        if type(row) is not dict:
            raise _error(where, f"A {what} is an object with Name.", f"Skeleton.Invalid{what.title().replace(' ', '')}")
        for key in row:
            if key not in keys:
                raise _error(f"{where}/{common.escape(key)}", f"Unknown {what} key '{key}'. Keys: {', '.join(keys)}.",
                             "Skeleton.UnknownKey")
        name = validate_name(row.get("Name"), f"{where}/Name", what)
        if name in names:
            raise _error(f"{where}/Name", f"{what.capitalize()} '{name}' appears more than once.", "Skeleton.DuplicateName")
        names.add(name)
        item = item_of(row, where)
        item["Name"] = name
        if "RenamedFrom" in row:
            source = validate_name(row["RenamedFrom"], f"{where}/RenamedFrom", what, reference=True)
            if source == name or source in sources:
                raise _error(f"{where}/RenamedFrom", "RenamedFrom names another entry, once.", "Skeleton.InvalidRename")
            sources.add(source)
            item["RenamedFrom"] = source
        out.append(item)
    return out


def _vector(value, path, fields):
    if type(value) is not dict or set(value) != set(fields):
        raise _error(path, f"{path.rsplit('/', 1)[-1]} is an object with exactly {', '.join(fields)}.", "Skeleton.InvalidSocket")
    return {field: anim.number(value[field], f"{path}/{field}", "Skeleton.InvalidSocket", field) for field in fields}


# The bone of a socket attached to no bone: engine data (Fab Paragon skeletons have such sockets) that the editor
# loads and shows; the document keeps it as read, an apply leaves it alone and never writes it.
NO_BONE = "None"


def _socket(row, where):
    bone = row.get("Bone")
    item = {"Bone": NO_BONE if bone == NO_BONE else validate_name(bone, f"{where}/Bone", "bone", "Skeleton.InvalidSocket")}
    for key, fields in VECTOR_FIELDS.items():
        if key in row:
            item[key] = _vector(row[key], f"{where}/{key}", fields)
    if "ForceAlwaysAnimated" in row:
        if type(row["ForceAlwaysAnimated"]) is not bool:
            raise _error(f"{where}/ForceAlwaysAnimated", "ForceAlwaysAnimated is true or false.", "Skeleton.InvalidSocket")
        item["ForceAlwaysAnimated"] = row["ForceAlwaysAnimated"]
    return item


def _virtual_bone(row, where):
    name = row.get("Name")
    if type(name) is str and not name.startswith("VB "):
        raise _error(f"{where}/Name", f"Virtual bone names start with \"VB \" (as the skeleton tree names them), "
                     f"e.g. \"VB {name}\".", "Skeleton.InvalidVirtualBone")
    return {"Source": validate_name(row.get("Source"), f"{where}/Source", "bone", "Skeleton.InvalidVirtualBone"),
            "Target": validate_name(row.get("Target"), f"{where}/Target", "bone", "Skeleton.InvalidVirtualBone")}


def _blend_profile(row, where):
    item = {}
    if "Mode" in row:
        if row["Mode"] not in BLEND_PROFILE_MODES:
            raise _error(f"{where}/Mode", f"Mode is one of {', '.join(BLEND_PROFILE_MODES)}.", "Skeleton.InvalidBlendProfileMode")
        item["Mode"] = row["Mode"]
    if "Bones" in row:
        bones = row["Bones"]
        if type(bones) is not dict:
            raise _error(f"{where}/Bones", "Bones is an object of bone name -> blend scale.", "Skeleton.InvalidBlendProfile")
        item["Bones"] = {validate_name(bone, f"{where}/Bones/{common.escape(str(bone))}", "bone", as_key=True):
                         anim.number(scale, f"{where}/Bones/{common.escape(bone)}", "Skeleton.InvalidBlendProfile", "A blend scale")
                         for bone, scale in bones.items()}
    return item


def _curve(row, where):
    item = {}
    for key in ("Material", "MorphTarget"):
        if key in row:
            if type(row[key]) is not bool:
                raise _error(f"{where}/{key}", f"{key} is true or false.", "Skeleton.InvalidCurve")
            item[key] = row[key]
    if "LinkedBones" in row:
        bones = row["LinkedBones"]
        if type(bones) is not list:
            raise _error(f"{where}/LinkedBones", "LinkedBones is a list of bone names.", "Skeleton.InvalidCurve")
        for index, bone in enumerate(bones):
            validate_name(bone, f"{where}/LinkedBones/{index}", "bone", "Skeleton.InvalidCurve", reference=True)
            if bone in bones[:index]:
                raise _error(f"{where}/LinkedBones/{index}", f"Bone '{bone}' appears more than once.", "Skeleton.InvalidCurve")
        item["LinkedBones"] = list(bones)
    if "MaxLOD" in row:
        if type(row["MaxLOD"]) is not int or not 0 <= row["MaxLOD"] <= 255:
            raise _error(f"{where}/MaxLOD", "MaxLOD is a LOD index 0..255 (255: every LOD).", "Skeleton.InvalidCurve")
        item["MaxLOD"] = row["MaxLOD"]
    return item


def _names(rows, path, what, code):
    if type(rows) is not list:
        raise _error(path, f"{path.rsplit('/', 1)[-1]} is a list of {what} names.", code)
    for index, name in enumerate(rows):
        validate_name(name, f"{path}/{index}", what, code)
        if name in rows[:index]:
            raise _error(f"{path}/{index}", f"{what.capitalize()} '{name}' appears more than once.", code)
    return list(rows)


def _path_rows(rows, path, keys, code):
    """A read-only list of objects with exactly ``keys`` (string values)."""
    if type(rows) is not list:
        raise _error(path, f"{path.rsplit('/', 1)[-1]} is a list of {{{', '.join(keys)}}} objects.", code)
    for index, row in enumerate(rows):
        if type(row) is not dict or set(row) != set(keys) or any(type(row[key]) is not str for key in keys):
            raise _error(f"{path}/{index}", f"An entry is an object with exactly {', '.join(keys)} (strings).", code)
    return [dict(row) for row in rows]


def _slot_groups(rows):
    if type(rows) is not list:
        raise _error("/Body/SlotGroups", "SlotGroups is a list of {\"Name\", \"Slots\"} objects.", "Skeleton.InvalidSlotGroups")
    groups, seen, sources = [], {}, set()
    for index, row in enumerate(rows):
        where = f"/Body/SlotGroups/{index}"
        if type(row) is not dict or set(row) - {"Name", "Slots"}:
            raise _error(where, "A slot group is an object with Name and Slots.", "Skeleton.InvalidSlotGroups")
        name = validate_name(row.get("Name"), f"{where}/Name", "slot group")
        if name in seen:
            authored_rule(f"{where}/Name", "Skeleton.DuplicateName", f"Slot or group name '{name}' appears more than "
                          "once (slot and group names are unique across both).", judged=(seen[name], f"{where}/Name"))
        else:
            seen[name] = f"{where}/Name"
        slots = row.get("Slots", [])
        if type(slots) is not list:
            raise _error(f"{where}/Slots", "Slots is a list of slot names (or {\"Name\", \"RenamedFrom\"}).",
                         "Skeleton.InvalidSlotGroups")
        out = []
        for slot_index, slot in enumerate(slots):
            slot_path = f"{where}/Slots/{slot_index}"
            if type(slot) is dict:
                if set(slot) - {"Name", "RenamedFrom"}:
                    raise _error(slot_path, "A slot is a name or {\"Name\", \"RenamedFrom\"}.", "Skeleton.InvalidSlotGroups")
                item = {"Name": validate_name(slot.get("Name"), f"{slot_path}/Name", "slot")}
                if "RenamedFrom" in slot:
                    source = validate_name(slot["RenamedFrom"], f"{slot_path}/RenamedFrom", "slot", reference=True)
                    if source == item["Name"] or source in sources:
                        raise _error(f"{slot_path}/RenamedFrom", "RenamedFrom names another slot, once.", "Skeleton.InvalidRename")
                    sources.add(source)
                    item["RenamedFrom"] = source
            else:
                item = {"Name": validate_name(slot, slot_path, "slot")}
            name_path = f"{slot_path}/Name" if type(slot) is dict else slot_path
            if item["Name"] in seen:
                authored_rule(slot_path, "Skeleton.DuplicateName", f"Slot or group name '{item['Name']}' appears more "
                              "than once (slot and group names are unique across both).",
                              judged=(seen[item["Name"]], name_path))
            else:
                seen[item["Name"]] = name_path
            out.append(item)
        groups.append({"Name": name, "Slots": out})
    # The editor cannot delete DefaultGroup; a Skeleton whose slots were all made in other groups (the Anim Slot
    # Manager on a fresh Skeleton) never had it, and keeps those groups as they are.
    if groups and DEFAULT_GROUP not in {group["Name"] for group in groups}:
        authored_rule("/Body/SlotGroups", "Skeleton.DefaultGroupRequired", "SlotGroups must keep DefaultGroup (the "
                      "editor cannot delete it).",
                      judged=tuple(f"/Body/SlotGroups/{index}/Name" for index in range(len(groups))))
    return groups


def canonical_body(body):
    out = {}
    if "SlotGroups" in body:
        out["SlotGroups"] = _slot_groups(body["SlotGroups"])
    if "Sockets" in body:
        out["Sockets"] = _entries(body["Sockets"], "/Body/Sockets", "socket", SOCKET_KEYS, _socket)
    if "VirtualBones" in body:
        out["VirtualBones"] = _entries(body["VirtualBones"], "/Body/VirtualBones", "virtual bone",
                                       ("Name", "Source", "Target", "RenamedFrom"), _virtual_bone)
    if "BlendProfiles" in body:
        out["BlendProfiles"] = _entries(body["BlendProfiles"], "/Body/BlendProfiles", "blend profile",
                                        ("Name", "Mode", "Bones", "RenamedFrom"), _blend_profile)
    if "BoneRetargeting" in body:
        modes = body["BoneRetargeting"]
        if type(modes) is not dict:
            raise _error("/Body/BoneRetargeting", "BoneRetargeting is an object of bone name -> translation retargeting "
                         "mode.", "Skeleton.InvalidRetargetingMode")
        for bone, mode in modes.items():
            validate_name(bone, f"/Body/BoneRetargeting/{common.escape(str(bone))}", "bone", as_key=True)
            if mode not in RETARGETING_MODES:
                raise _error(f"/Body/BoneRetargeting/{common.escape(bone)}", f"A translation retargeting mode is one of "
                             f"{', '.join(RETARGETING_MODES)}.", "Skeleton.InvalidRetargetingMode")
        out["BoneRetargeting"] = {bone: mode for bone, mode in modes.items() if mode != "Animation"}
    if "Curves" in body:
        out["Curves"] = _entries(body["Curves"], "/Body/Curves", "curve", CURVE_KEYS, _curve)
    for key in ("PreviewMesh", "AdditionalPreviewMeshes"):
        if key in body:
            if type(body[key]) is not str:
                raise _error(f"/Body/{key}", f"{key} is an object path, or \"\" for none.", "Skeleton.InvalidPreviewMesh")
            out[key] = body[key]
    if "AnimNotifyNames" in body:
        out["AnimNotifyNames"] = _names(body["AnimNotifyNames"], "/Body/AnimNotifyNames", "notify", "Skeleton.InvalidNotifyName")
    if "PreviewAttachedAssets" in body:
        out["PreviewAttachedAssets"] = _path_rows(body["PreviewAttachedAssets"], "/Body/PreviewAttachedAssets",
                                                  ("Asset", "AttachedTo"), "Skeleton.InvalidPreviewAttachedAssets")
    if "RetargetSources" in body:
        out["RetargetSources"] = _path_rows(body["RetargetSources"], "/Body/RetargetSources", ("Name", "Mesh"),
                                            "Skeleton.InvalidRetargetSources")
    return out


def canonicalize(document):
    body = common.validate_header(document, (CLASS,), PREFIX, BODY_KEYS)
    if document.get("Action") == "Create":
        _refuse_create()
    canonical_body(body)
    _properties().check_owned(document.get("Properties", {}))
    return common.snapshot(document)


def _is_curve_metadata(element):
    return type(element) is dict and element.get("Class") == CURVE_METADATA_CLASS


class SkeletonProperties(common.PropertiesRegion):
    """Properties with ``AssetUserData`` read and written without the curve metadata object (``Body.Curves``): an
    apply moves that object aside while the generic codec writes the array, then puts it back at its index."""

    def check_owned(self, properties):
        super().check_owned(properties)
        rows = properties.get("AssetUserData")
        for index, element in enumerate(rows if type(rows) is list else ()):
            if _is_curve_metadata(element):
                raise _error(f"/Properties/AssetUserData/{index}", "The curve metadata object is authored as Body.Curves; "
                             "leave it out of AssetUserData (an apply keeps it).", f"{PREFIX}.BodyOwnedProperty")

    def extract(self, read, context, include_defaults=True):
        values = super().extract(read, context, include_defaults)
        if type(values.get("AssetUserData")) is list:
            values["AssetUserData"] = [element for element in values["AssetUserData"] if not _is_curve_metadata(element)]
            if not values["AssetUserData"] and not include_defaults:
                del values["AssetUserData"]
        return values

    def apply(self, properties, read, mutable, context, calls=None, live=None):
        index = (live or {}).get("curve_metadata_index", -1)
        if type(properties.get("AssetUserData")) is not list or type(index) is not int or index < 0:
            return super().apply(properties, read, mutable, context)
        self.check_owned(properties)
        rest = {name: value for name, value in properties.items() if name != "AssetUserData"}
        wanted = properties["AssetUserData"]
        if any(common.same(wanted, self.extract(read, context, full).get("AssetUserData", []), exact=False)
               for full in (True, False)):
            # AssetUserData as the asset has it: nothing is parked, so an unchanged apply leaves the package clean.
            return list(super().apply(rest, read, mutable, context)) if rest else []
        # The curve metadata object is parked in AssetUserDataEditorOnly while AssetUserData is written element by
        # element, then moved back to its index (clamped to the new length).
        where = "/Properties/AssetUserData"
        changes = edit(calls, [({"op": "curve_metadata_park", "index": index}, where)])
        try:
            changes.extend(super().apply({"AssetUserData": properties["AssetUserData"]}, read, mutable, context))
        finally:
            changes.extend(edit(calls, [({"op": "curve_metadata_unpark", "index": index}, where)]))
        if rest:
            changes.extend(super().apply(rest, read, mutable, context))
        return changes


def _properties():
    return SkeletonProperties(CLASS, BODY_OWNED, PREFIX)


# ---------------------------------------------------------------------------
# Live state
# ---------------------------------------------------------------------------
def read_live(calls):
    return calls.query(anim.SKELETON_READ)


def body_from_live(live):
    return {
        "SlotGroups": [{"Name": group["name"], "Slots": sorted(group["slots"])} for group in live.get("slot_groups", [])],
        "Sockets": [{"Name": row["name"], "Bone": row["bone"], "Location": row["location"], "Rotation": row["rotation"],
                     "Scale": row["scale"], "ForceAlwaysAnimated": row["force_always_animated"]}
                    for row in sorted(live.get("sockets", []), key=lambda item: item["name"])],
        "VirtualBones": [{"Name": row["name"], "Source": row["source"], "Target": row["target"]}
                         for row in live.get("virtual_bones", [])],
        "BlendProfiles": [{"Name": row["name"], "Mode": row["mode"], "Bones": dict(sorted(row["bones"].items()))}
                          for row in sorted(live.get("blend_profiles", []), key=lambda item: item["name"])],
        "BoneRetargeting": dict(sorted(live.get("retargeting", {}).items())),
        "Curves": [_live_curve(row) for row in live.get("curves", [])],
        "PreviewMesh": live.get("preview_mesh", ""),
        "AdditionalPreviewMeshes": live.get("additional_preview_meshes", ""),
        **_read_only_sections(live),
    }


def _live_curve(row):
    curve = {"Name": row["name"], "Material": row["material"], "MorphTarget": row["morph_target"]}
    if row.get("linked_bones"):
        curve["LinkedBones"] = list(row["linked_bones"])
    if row.get("max_lod", 255) != 255:
        curve["MaxLOD"] = row["max_lod"]
    return curve


def _read_only_sections(live):
    """The lists that are left out when empty."""
    sections = {
        "AnimNotifyNames": sorted(live.get("notifies", [])),
        "PreviewAttachedAssets": [{"Asset": row["asset"], "AttachedTo": row["attached_to"]}
                                  for row in live.get("preview_attached_assets", [])],
        "RetargetSources": [{"Name": row["name"], "Mesh": row["mesh"]} for row in live.get("retarget_sources", [])]}
    return {key: value for key, value in sections.items() if value}


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------
def _match(authored, live_names):
    """{authored index: live name} by Name, then by RenamedFrom; and the live names left over."""
    claimed, used = {}, set()
    for index, row in enumerate(authored):
        if row["Name"] in live_names:
            claimed[index] = row["Name"]
            used.add(row["Name"])
    for index, row in enumerate(authored):
        source = row.get("RenamedFrom")
        if index not in claimed and source in live_names and source not in used:
            claimed[index] = source
            used.add(source)
    return claimed, [name for name in live_names if name not in used]


def slot_ops(groups, live):
    """Anim Slot Manager ops: new groups, removed slots, renames, slot moves / adds, removed groups."""
    ops = []
    live_groups = {group["name"]: list(group["slots"]) for group in live.get("slot_groups", [])}
    live_slot_group = {slot: group for group, slots in live_groups.items() for slot in slots}
    wanted = [(group["Name"], slot, gi, si) for gi, group in enumerate(groups) for si, slot in enumerate(group["Slots"])]
    wanted_names = {slot["Name"] for _, slot, _, _ in wanted}
    renames = {}
    for group_name, slot, gi, si in wanted:
        source = slot.get("RenamedFrom")
        if slot["Name"] not in live_slot_group and source in live_slot_group and source not in wanted_names:
            renames[slot["Name"]] = source
    for index, group in enumerate(groups):
        if group["Name"] not in live_groups:
            ops.append(({"op": "slot_group_add", "name": group["Name"]}, f"/Body/SlotGroups/{index}/Name"))
    sources = set(renames.values())
    for slot in live_slot_group:
        if slot not in wanted_names and slot not in sources:
            ops.append(({"op": "slot_remove", "name": slot}, "/Body/SlotGroups"))
    for group_name, slot, gi, si in wanted:
        path = f"/Body/SlotGroups/{gi}/Slots/{si}"
        source = renames.get(slot["Name"])
        if source is not None:
            ops.append(({"op": "slot_rename", "name": source, "to": slot["Name"]}, path))
            in_place = live_slot_group[source] == group_name
        else:
            # A slot an older asset lists in two groups stays where it is listed.
            in_place = slot["Name"] in live_groups.get(group_name, ())
        if not in_place:
            ops.append(({"op": "slot_set", "name": slot["Name"], "group": group_name}, path))
    wanted_groups = {group["Name"] for group in groups}
    for group in live_groups:
        if group not in wanted_groups:
            ops.append(({"op": "slot_group_remove", "name": group}, "/Body/SlotGroups"))
    return ops


def _named_ops(authored, live_rows, section, kind, set_op):
    """Remove / rename / add / set ops of one named section."""
    live_by_name = {row["name"]: row for row in live_rows}
    claimed, removed = _match(authored, list(live_by_name))
    ops = [({"op": f"{kind}_remove", "name": name}, f"/Body/{section}") for name in removed]
    for index, row in enumerate(authored):
        path = f"/Body/{section}/{index}"
        if index in claimed and claimed[index] != row["Name"]:
            ops.append(({"op": f"{kind}_rename", "name": claimed[index], "to": row["Name"]}, path))
    for index, row in enumerate(authored):
        path = f"/Body/{section}/{index}"
        live = live_by_name.get(claimed.get(index))
        ops.extend((op, path) for op in set_op(row, live))
    return ops


def _socket_ops(row, live):
    ops = []
    if live is None:
        ops.append({"op": "socket_add", "name": row["Name"], "bone": row["Bone"]})
        live = {"bone": row["Bone"], "location": SOCKET_DEFAULTS["Location"], "rotation": SOCKET_DEFAULTS["Rotation"],
                "scale": SOCKET_DEFAULTS["Scale"], "force_always_animated": True}
    change = {}
    if row["Bone"] != live["bone"]:
        change["bone"] = row["Bone"]
    for key, field in (("Location", "location"), ("Rotation", "rotation"), ("Scale", "scale")):
        if key in row and not anim.same_number(row[key], live[field]):
            change[field] = row[key]
    if "ForceAlwaysAnimated" in row and row["ForceAlwaysAnimated"] != live["force_always_animated"]:
        change["force_always_animated"] = row["ForceAlwaysAnimated"]
    if change:
        ops.append({"op": "socket_set", "name": row["Name"], **change})
    return ops


def _virtual_bone_ops(row, live):
    if live is None:
        return [{"op": "virtual_bone_add", "name": row["Name"], "source": row["Source"], "target": row["Target"]}]
    if (live["source"], live["target"]) != (row["Source"], row["Target"]):
        # The skeleton tree has no "change source / target": the virtual bone is replaced.
        return [{"op": "virtual_bone_remove", "name": row["Name"]},
                {"op": "virtual_bone_add", "name": row["Name"], "source": row["Source"], "target": row["Target"]}]
    return []


def _default_scale(mode):
    return 0 if mode == "BlendMask" else 1


def profile_bones(row, mode):
    """Authored blend scales without the entries that equal the mode's default (the editor drops them)."""
    default = _default_scale(mode)
    return {bone: scale for bone, scale in row.get("Bones", {}).items() if not anim.same_number(scale, default)}


def _blend_profile_ops(row, live):
    ops = []
    if live is None:
        ops.append({"op": "blend_profile_add", "name": row["Name"]})
        live = {"mode": "TimeFactor", "bones": {}}
    mode = row.get("Mode", live["mode"])
    change = {}
    if mode != live["mode"]:
        change["mode"] = mode
    if "Bones" in row:
        bones = profile_bones(row, mode)
        if set(bones) != set(live["bones"]) or any(not anim.same_number(scale, live["bones"][bone])
                                                    for bone, scale in bones.items()) or "mode" in change:
            change["bones"] = bones
    if change:
        ops.append({"op": "blend_profile_set", "name": row["Name"], **change})
    return ops


def _curve_ops(row, live):
    ops = []
    if live is None:
        ops.append({"op": "curve_add", "name": row["Name"]})
        live = {"material": False, "morph_target": False}
    change = {}
    if "Material" in row and row["Material"] != live["material"]:
        change["material"] = row["Material"]
    if "MorphTarget" in row and row["MorphTarget"] != live["morph_target"]:
        change["morph_target"] = row["MorphTarget"]
    if "LinkedBones" in row and row["LinkedBones"] != live.get("linked_bones", []):
        change["linked_bones"] = row["LinkedBones"]
    if "MaxLOD" in row and row["MaxLOD"] != live.get("max_lod", 255):
        change["max_lod"] = row["MaxLOD"]
    if change:
        ops.append({"op": "curve_set", "name": row["Name"], **change})
    return ops


def _retarget_ops(modes, live):
    current = live.get("retargeting", {})
    ops = [({"op": "retarget", "bone": bone, "mode": "Animation"}, "/Body/BoneRetargeting")
           for bone in current if bone not in modes]
    ops += [({"op": "retarget", "bone": bone, "mode": mode}, f"/Body/BoneRetargeting/{common.escape(bone)}")
            for bone, mode in modes.items() if current.get(bone, "Animation") != mode]
    return ops


def check_socket_bones(authored, live_rows):
    """A socket on no bone (Bone "None") is only kept as it is: an existing one that the document leaves on no bone.
    A new socket, or one moved to no bone, is refused as the editor's socket bone picker does not offer it."""
    live_by_name = {row["name"]: row for row in live_rows}
    claimed, _ = _match(authored, list(live_by_name))
    for index, row in enumerate(authored):
        live = live_by_name.get(claimed.get(index))
        if row["Bone"] == NO_BONE and (live is None or live["bone"] != NO_BONE):
            raise _error(f"/Body/Sockets/{index}/Bone", f"Socket '{row['Name']}' names no bone (\"None\"). A socket is "
                         "attached to a bone of the Skeleton; only an existing socket that already has no bone (engine "
                         "data) is kept as it is.", "Skeleton.InvalidSocket")


def body_ops(body, live):
    ops = []
    if "SlotGroups" in body:
        ops += slot_ops(body["SlotGroups"], live)
    if "VirtualBones" in body:
        ops += _named_ops(body["VirtualBones"], live.get("virtual_bones", []), "VirtualBones", "virtual_bone", _virtual_bone_ops)
    if "Sockets" in body:
        check_socket_bones(body["Sockets"], live.get("sockets", []))
        ops += _named_ops(body["Sockets"], live.get("sockets", []), "Sockets", "socket", _socket_ops)
    if "BlendProfiles" in body:
        ops += _named_ops(body["BlendProfiles"], live.get("blend_profiles", []), "BlendProfiles", "blend_profile",
                          _blend_profile_ops)
    if "BoneRetargeting" in body:
        ops += _retarget_ops(body["BoneRetargeting"], live)
    if "Curves" in body:
        ops += _named_ops(body["Curves"], live.get("curves", []), "Curves", "curve", _curve_ops)
    for key, field, op in (("PreviewMesh", "preview_mesh", "preview_mesh"),
                           ("AdditionalPreviewMeshes", "additional_preview_meshes", "additional_preview_meshes")):
        if key in body and body[key] != live.get(field, ""):
            ops.append(({"op": op, "path": body[key]}, f"/Body/{key}"))
    if "AnimNotifyNames" in body:
        have = set(live.get("notifies", []))
        ops += [({"op": "notify_add", "name": name}, f"/Body/AnimNotifyNames/{index}")
                for index, name in enumerate(body["AnimNotifyNames"]) if name not in have]
    check_read_only(body, live)
    return ops


def _attachments_after(body, live):
    """The live preview attachments as the document's socket edits leave them: a renamed socket keeps its attached
    assets (the skeleton tree's rename moves them), a removed socket's are detached."""
    rows = [{"Asset": row["asset"], "AttachedTo": row["attached_to"]} for row in live.get("preview_attached_assets", [])]
    if "Sockets" not in body:
        return rows
    claimed, removed = _match(body["Sockets"], [row["name"] for row in live.get("sockets", [])])
    renamed = {source: body["Sockets"][index]["Name"] for index, source in claimed.items()}
    return [{**row, "AttachedTo": renamed.get(row["AttachedTo"], row["AttachedTo"])} for row in rows
            if row["AttachedTo"] not in removed]


def _unordered(rows):
    return sorted(tuple(sorted(row.items())) for row in rows)


def check_read_only(body, live):
    """PreviewAttachedAssets and RetargetSources are read only: the document gives them as they are (or leaves them out)."""
    for key, have, code, what in (
            ("PreviewAttachedAssets", _attachments_after(body, live), "Skeleton.PreviewAttachedAssetsReadOnly",
             "the preview scene's attached assets (Skeleton editor viewport)"),
            ("RetargetSources", _read_only_sections(live).get("RetargetSources", []), "Skeleton.RetargetSourcesReadOnly",
             "the Retarget Sources tab")):
        if key in body and _unordered(body[key]) != _unordered(have):
            raise _error(f"/Body/{key}", f"{key} is read only ({what}); give it as the asset has it after this apply "
                         f"({have}) or leave it out.", code)


def _field_paths():
    fields = {"location": "/Location", "rotation": "/Rotation", "scale": "/Scale", "bone": "/Bone",
              "force_always_animated": "/ForceAlwaysAnimated", "mode": "/Mode", "bones": "/Bones",
              "material": "/Material", "morph_target": "/MorphTarget", "source": "/Source", "target": "/Target",
              "linked_bones": "/LinkedBones", "max_lod": "/MaxLOD"}
    return fields


def map_path(primitive_path, op_paths):
    parts = [part for part in (primitive_path or "").split("/") if part]
    if len(parts) >= 2 and parts[0] == "ops" and parts[1].isdigit() and int(parts[1]) < len(op_paths):
        path = op_paths[int(parts[1])]
        rest = parts[2:]
        if rest and rest[0] in _field_paths() and not path.endswith(("/BoneRetargeting", "/Sockets", "/SlotGroups")):
            path += _field_paths()[rest[0]]
            if rest[0] in ("bones", "linked_bones") and len(rest) > 1:
                path += f"/{rest[1]}"
        return path
    return "/Body"


def edit(calls, ops):
    changed = []
    for chunk in common.chunk_ops(ops):
        op_paths = [path for _, path in chunk]
        outcome = common.invoke(calls.context, calls.by_id, anim.SKELETON_EDIT,
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
    ops = body_ops(body, read_live(calls))
    changes = edit(calls, ops) if ops else []
    # Body edits can create the curve metadata object (a new curve), so the Asset User Data is matched after them.
    live = read_live(calls) if type(properties.get("AssetUserData")) is list else None
    changes.extend(region.apply(properties, read, mutable, context, calls, live))
    return tuple(changes)


# ---------------------------------------------------------------------------
# Extract / Diff / Inspect
# ---------------------------------------------------------------------------
def warn_sockets_without_bone(body):
    """Read warnings for the sockets on no bone (the rest of the Skeleton is read as usual)."""
    for index, row in enumerate(body.get("Sockets", [])):
        if row["Bone"] == NO_BONE:
            read_warning(f"/Body/Sockets/{index}/Bone", "Skeleton.SocketWithoutBone",
                         f"Socket '{row['Name']}' is attached to no bone (Bone \"None\"), as the asset was saved. The "
                         "document lists it as it is and an apply keeps it; to use it, give it a bone of the Skeleton.")


def extract(target, read, context, include_defaults=True):
    live = read_live(anim.AnimCalls(context, BY_ID, read, prefix=PREFIX))
    body = body_from_live(live)
    warn_sockets_without_bone(body)
    return common.document(target, CLASS, _properties().extract(read, context, include_defaults), body)


def _keyed(rows):
    return {row["Name"]: row for row in rows}


def diff(authored, extracted, exact):
    changes = common.header_diff(authored, extracted)
    body = canonical_body(authored.get("Body", {}))
    live = canonical_body(extracted.get("Body", {}))
    if "SlotGroups" in body:
        want = {group["Name"]: sorted(slot["Name"] for slot in group["Slots"]) for group in body["SlotGroups"]}
        have = {group["Name"]: sorted(slot["Name"] for slot in group["Slots"]) for group in live.get("SlotGroups", [])}
        if want != have:
            changes.append(Difference("/Body/SlotGroups", DiffKind.REPLACE, True, body["SlotGroups"], True,
                                      live.get("SlotGroups")))
    for section in ("Sockets", "VirtualBones", "BlendProfiles", "Curves"):
        if section not in body:
            continue
        have = _keyed(live.get(section, []))
        if set(_keyed(body[section])) != set(have):
            changes.append(Difference(f"/Body/{section}", DiffKind.REPLACE, True, body[section], True, live.get(section)))
            continue
        for index, row in enumerate(body[section]):
            observed = have[row["Name"]]
            for key, value in row.items():
                if key in ("Name", "RenamedFrom"):
                    continue
                if section == "BlendProfiles" and key == "Bones":
                    value = profile_bones(row, row.get("Mode", observed.get("Mode")))
                seen = observed.get(key, CURVE_DEFAULTS.get(key)) if section == "Curves" else observed.get(key)
                if not common.same(value, seen, exact):
                    changes.append(Difference(f"/Body/{section}/{index}/{key}", DiffKind.REPLACE, True, value,
                                              key in observed, seen))
    if "BoneRetargeting" in body and body["BoneRetargeting"] != live.get("BoneRetargeting", {}):
        changes.append(Difference("/Body/BoneRetargeting", DiffKind.REPLACE, True, body["BoneRetargeting"], True,
                                  live.get("BoneRetargeting")))
    for key in ("PreviewMesh", "AdditionalPreviewMeshes"):
        if key in body and body[key] != live.get(key, ""):
            changes.append(Difference(f"/Body/{key}", DiffKind.REPLACE, True, body[key], key in live, live.get(key, "")))
    for key in ("PreviewAttachedAssets", "RetargetSources"):
        if key in body and _unordered(body[key]) != _unordered(live.get(key, [])):
            changes.append(Difference(f"/Body/{key}", DiffKind.REPLACE, True, body[key], key in live, live.get(key, [])))
    missing = [name for name in body.get("AnimNotifyNames", []) if name not in live.get("AnimNotifyNames", [])]
    if missing:
        changes.append(Difference("/Body/AnimNotifyNames", DiffKind.REPLACE, True, body["AnimNotifyNames"], True,
                                  live.get("AnimNotifyNames", [])))
    changes.extend(common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact))
    return tuple(changes)


def inspect_details(live):
    return {"BoneCount": live.get("bone_count", 0),
            "Note": "Bones are imported and read only; the Skeleton facts of this payload list them with the slots "
                    "and sockets. AnimMontage slot tracks name a slot of their Skeleton (Body.SlotGroups); sockets, "
                    "virtual bones, blend profiles and BoneRetargeting name bones."}


class SkeletonRuntimeProfile:
    profile_id = PROFILE_ID
    exact_asset_class_path = CLASS
    runtime_entry_module = "asset_document/bootstrap.py"
    preflight_scratch = False

    def required_primitives(self):
        return REQUIREMENTS

    def inspect_profile(self):
        return {"Class": CLASS, "BodySections": list(BODY_KEYS),
                "ReadOnlySections": ["Bones (Inspect: Skeleton facts)", "PreviewAttachedAssets", "RetargetSources"],
                "CreateContext": "not supported (a Skeleton comes from a Skeletal Mesh import)"}

    def create_template(self, target):
        return common.document(common.require_target(target, PREFIX), CLASS, {},
                               {"SlotGroups": [{"Name": DEFAULT_GROUP, "Slots": ["DefaultSlot"]}]})

    def validate(self, document):
        canonicalize(document)

    def canonicalize(self, document):
        return canonicalize(document)

    def create_target_context(self, document, context):
        _refuse_create()

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
            raise _error("/Inspect", "Skeleton inspection reads a live Skeleton.", "Skeleton.InspectNotAdmitted")
        document = self.extract(target=exact_asset_object_path.rsplit(".", 1)[0], asset_handle=editor_asset_handle,
                                context=context, include_defaults=True)
        live = read_live(anim.AnimCalls(context, BY_ID, editor_asset_handle, prefix=PREFIX))
        return {**canonicalize(document), "SkeletonDetails": inspect_details(live)}

    def diff(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=False)

    def diff_exact(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=True)


def create_skeleton_runtime_handler():
    return SkeletonRuntimeProfile()


SKELETON_PROFILE_META = ProfileMeta(
    PROFILE_ID, anim.ROUTE, CLASS,
    tuple(RegionMeta(f"skeleton.{name}.v1", f"/Body/{key}", RegionOwnership.MANAGED, IdentityPolicy.STABLE_KEY,
                     OrderPolicy.CANONICAL, DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE, DeletePolicy.EXPLICIT_ONLY,
                     ComparisonPolicy.CANONICAL)
          for name, key in (("slot_groups", "SlotGroups"), ("sockets", "Sockets"), ("virtual_bones", "VirtualBones"),
                            ("blend_profiles", "BlendProfiles"), ("bone_retargeting", "BoneRetargeting"),
                            ("curves", "Curves"), ("preview_mesh", "PreviewMesh"),
                            ("additional_preview_meshes", "AdditionalPreviewMeshes"),
                            ("anim_notify_names", "AnimNotifyNames"), ("preview_attached_assets", "PreviewAttachedAssets"),
                            ("retarget_sources", "RetargetSources"))),
    REQUIREMENTS)
SKELETON_RUNTIME_REGISTRATION = RuntimeProfileRegistration(
    SKELETON_PROFILE_META, create_skeleton_runtime_handler, RuntimeTargetPolicy.CREATE_OR_UPDATE, LIFECYCLE)

__all__ = ("SKELETON_RUNTIME_REGISTRATION", "SkeletonRuntimeProfile", "body_ops", "canonical_body", "slot_ops")
