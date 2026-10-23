# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Registered IK Rig Profile (profile.ik_rig.v1, class /Script/IKRig.IKRigDefinition).

Document (MCP/schemas/assetdocument/IKRig.md)::

    "Body": {
      "PreviewMesh": "/Game/Characters/Meshes/SKM_Hero.SKM_Hero",
      "ExcludedBones": ["ik_foot_root"],
      "Goals": {"LeftFootIK": {"Bone": "foot_l"}, "LeftHandIK": {"Bone": "hand_l", "RotationAlpha": 0.5}},
      "Solvers": [{"Type": "FullBodyIK", "StartBone": "pelvis", "Settings": {"RootBehavior": "Free"},
                   "Goals": {"LeftFootIK": {}, "LeftHandIK": {"PullChainAlpha": 0.5}},
                   "Bones": {"calf_l": {"X": "Locked"}}}],
      "RetargetRoot": "pelvis",
      "Chains": {"LeftLeg": {"Start": "thigh_l", "End": "ball_l", "Goal": "LeftFootIK"}}
    }

* Every Body region is complete state when present and kept when omitted. Goals and chains are keyed by name;
  solvers are an ordered stack. Settings, goal settings, bone settings and goal Details are sparse: an omitted field
  has the value a new solver / goal of that type has.
* Extract reads the rig as stored (no re-initialization): ``Offset`` is the goal's gizmo offset from its bone.
* Apply (``ik_rig_apply``) makes the IK Rig editor's edits; Create makes the rig with the Content Browser's factory
  and sets ``PreviewMesh`` first.
"""
from __future__ import annotations

from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, IdentityPolicy, OmissionPolicy, OrderPolicy, ProfileMeta,
    RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document.profiles import data_table_common as common
from asset_document.profiles import ik_common as ik

PROFILE_ID = "profile.ik_rig.v1"
CLASS = ik.IKRIG_CLASS
PREFIX = "IKRig"
BODY_KEYS = ("PreviewMesh", "ExcludedBones", "Goals", "Solvers", "RetargetRoot", "Chains")
GOAL_KEYS = ("Bone", "PositionAlpha", "RotationAlpha", "Offset", "PreviewMode", "ExposePosition", "ExposeRotation",
             "SizeMultiplier", "ThicknessMultiplier", "RenamedFrom")
SOLVER_KEYS = ("Type", "Enabled", "StartBone", "EndBone", "Settings", "Goals", "Bones")
CHAIN_KEYS = ("Start", "End", "Goal", "RenamedFrom")
GOAL_DEFAULTS = {"PositionAlpha": 1.0, "RotationAlpha": 1.0, "PreviewMode": "Additive", "SizeMultiplier": 1.0,
                 "ThicknessMultiplier": 0.7, "ExposePosition": False, "ExposeRotation": False, "Offset": {}}
SOLVER_DEFAULTS = {"Enabled": True, "Settings": {}, "Goals": {}, "Bones": {}}
CHAIN_DEFAULTS = {"Goal": ""}
# The rig's authored state is the Body; these are never generic Properties.
BODY_OWNED = {"PreviewSkeletalMesh": "Body/PreviewMesh", "Skeleton": "Body (ExcludedBones; the skeleton comes from "
              "the preview mesh)", "Goals": "Body/Goals", "SolverStack": "Body/Solvers",
              "RetargetDefinition": "Body/RetargetRoot and Body/Chains"}
NOT_AUTHORED = ("Controller", "BoneSize", "Solvers_DEPRECATED", "ThumbnailInfo")


def error(path, message, code):
    return common.error(path, message, code)


def _properties():
    return ik.RigProperties(CLASS, BODY_OWNED, NOT_AUTHORED, PREFIX)


# ---------------------------------------------------------------------------
# Document shape
# ---------------------------------------------------------------------------
def _names(value, path, what):
    if type(value) is not list or any(type(item) is not str or not item for item in value):
        raise error(path, f"{what} is a list of bone names.", "IKRig.InvalidBody")
    if len(set(value)) != len(value):
        raise error(path, f"{what} lists a bone twice.", "IKRig.InvalidBody")
    return value


def validate_body(body):
    path = "/Body"
    if "PreviewMesh" in body:
        ik.path_or_null(body["PreviewMesh"], f"{path}/PreviewMesh", "PreviewMesh", PREFIX)
    if "ExcludedBones" in body:
        _names(body["ExcludedBones"], f"{path}/ExcludedBones", "ExcludedBones")
    if "RetargetRoot" in body:
        ik.name_value(body["RetargetRoot"], f"{path}/RetargetRoot", "RetargetRoot", PREFIX, allow_empty=True)
    if "Goals" in body:
        for name, entry in ik.object_value(body["Goals"], f"{path}/Goals", "Goals", PREFIX).items():
            where = f"{path}/Goals/{ik.escape(name)}"
            ik.check_keys(ik.object_value(entry, where, "A goal", PREFIX), GOAL_KEYS, where, "goal", PREFIX)
            ik.name_value(entry.get("Bone"), f"{where}/Bone", "A goal's Bone", PREFIX)
            if "Offset" in entry:
                offset = ik.object_value(entry["Offset"], f"{where}/Offset", "Offset", PREFIX)
                ik.check_keys(offset, ("Location", "Rotation"), f"{where}/Offset", "Offset", PREFIX)
    if "Solvers" in body:
        if type(body["Solvers"]) is not list:
            raise error(f"{path}/Solvers", "Solvers is an ordered list of solvers.", "IKRig.InvalidBody")
        for index, entry in enumerate(body["Solvers"]):
            where = f"{path}/Solvers/{index}"
            ik.check_keys(ik.object_value(entry, where, "A solver", PREFIX), SOLVER_KEYS, where, "solver", PREFIX)
            ik.name_value(entry.get("Type"), f"{where}/Type", "A solver's Type", PREFIX)
            for key in ("Settings", "Goals", "Bones"):
                if key in entry:
                    region = ik.object_value(entry[key], f"{where}/{key}", key, PREFIX)
                    if key != "Settings":
                        for name, value in region.items():
                            ik.object_value(value, f"{where}/{key}/{ik.escape(name)}", f"A {key[:-1].lower()}'s settings",
                                            PREFIX)
            if "Enabled" in entry and type(entry["Enabled"]) is not bool:
                raise error(f"{where}/Enabled", "Enabled is true or false.", "IKRig.InvalidBody")
            for key in ("StartBone", "EndBone"):
                if key in entry:
                    ik.name_value(entry[key], f"{where}/{key}", key, PREFIX, allow_empty=True)
    if "Chains" in body:
        for name, entry in ik.object_value(body["Chains"], f"{path}/Chains", "Chains", PREFIX).items():
            where = f"{path}/Chains/{ik.escape(name)}"
            ik.check_keys(ik.object_value(entry, where, "A chain", PREFIX), CHAIN_KEYS, where, "chain", PREFIX)
            for key in ("Start", "End"):
                ik.name_value(entry.get(key), f"{where}/{key}", f"A chain's {key}", PREFIX, allow_empty=True)
            if "Goal" in entry:
                ik.name_value(entry["Goal"], f"{where}/Goal", "A chain's Goal", PREFIX, allow_empty=True)


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
def assemble(records):
    """Body and Inspect facts of rig.ikrig.read.v1 records."""
    header = records[0]
    body = {}
    facts = {"skeleton": [], "chain_bones": {}, "solvers": {}, "defaults": {}}
    if header.get("preview_mesh"):
        body["PreviewMesh"] = header["preview_mesh"]
    if header.get("excluded_bones"):
        body["ExcludedBones"] = list(header["excluded_bones"])
    goals, solvers, chains = {}, [], {}
    for record in records[1:]:
        kind = record.get("kind")
        if kind == "goal":
            goals[record["name"]] = record["entry"]
        elif kind == "solver":
            solvers.append(dict(record["entry"]))
            if "settings_defaults" in record or "goal_defaults" in record or "bone_defaults" in record:
                facts["defaults"][record["index"]] = {"Settings": record.get("settings_defaults", {}),
                                                      "Goals": record.get("goal_defaults", {}),
                                                      "Bones": record.get("bone_defaults", {})}
        elif kind == "solver_bones":
            solvers[record["index"]].setdefault("Bones", {}).update(record["bones"])
        elif kind == "chain":
            chains[record["name"]] = record["entry"]
        elif kind == "skeleton":
            facts["skeleton"] += record["bones"]
        elif kind == "chain_bones":
            facts["chain_bones"][record["name"]] = {"Valid": record["valid"], "Bones": record["bones"]}
        elif kind == "solver_facts":
            facts["solvers"][record["index"]] = record
    if goals:
        body["Goals"] = goals
    if solvers:
        body["Solvers"] = solvers
    if header.get("retarget_root"):
        body["RetargetRoot"] = header["retarget_root"]
    if chains:
        body["Chains"] = chains
    return body, facts


def read_body(calls, detail="document"):
    return assemble(ik.read_records(calls, ik.IKRIG_READ, detail))


def extract(target, read, context, include_defaults=True):
    body, _ = read_body(ik.RigCalls(context, read, prefix=PREFIX))
    return common.document(target, CLASS, _properties().extract(read, context, include_defaults), body)


def extract_with_defaults(target, read, context):
    """The document and, per solver index, the defaults its sparse Settings, goal settings and bone settings are
    read over (a new solver of its type)."""
    body, facts = read_body(ik.RigCalls(context, read, prefix=PREFIX), "verify")
    return common.document(target, CLASS, _properties().extract(read, context, True), body), facts["defaults"]


# ---------------------------------------------------------------------------
# Diff
# ---------------------------------------------------------------------------
def _solver_diff(at, mine, theirs, exact, changes, defaults):
    """One solver; with the live defaults its Settings, Goals and Bones compare field by field over them (an
    authored value equal to its default matches a read that omits it)."""
    if defaults is None or mine.get("Type") != theirs.get("Type"):
        ik.value_diff(at, mine, theirs, exact, changes, SOLVER_DEFAULTS)
        return
    mine, theirs = dict(mine), dict(theirs)
    ik.sparse_diff(f"{at}/Settings", mine.pop("Settings", {}), theirs.pop("Settings", {}), defaults["Settings"],
                   exact, changes)
    if "Goals" in mine or "Goals" in theirs:
        ik.sparse_map_diff(f"{at}/Goals", mine.pop("Goals", {}), theirs.pop("Goals", {}), defaults["Goals"], exact,
                           changes, missing_is_default=False)
    if "Bones" in mine or "Bones" in theirs:
        ik.sparse_map_diff(f"{at}/Bones", mine.pop("Bones", {}), theirs.pop("Bones", {}), defaults["Bones"], exact,
                           changes, missing_is_default=False)
    ik.value_diff(at, mine, theirs, exact, changes, SOLVER_DEFAULTS)


def body_diff(authored, live, exact, live_defaults=None):
    """Differences of the authored Body regions (omitted region = keep) from the live Body."""
    changes = []
    path = "/Body"
    for key in BODY_KEYS:
        if key not in authored:
            continue
        where = f"{path}/{key}"
        if key in ("PreviewMesh", "RetargetRoot"):
            ik.value_diff(where, authored[key] or "", live.get(key) or "", exact, changes)
        elif key == "ExcludedBones":
            if set(authored[key]) != set(live.get(key, [])):
                changes.append(common.Difference(where, common.DiffKind.REPLACE, True, authored[key],
                                                 key in live, live.get(key)))
        elif key == "Goals":
            ik.map_diff(where, authored[key], live.get(key, {}), exact, changes, GOAL_DEFAULTS)
        elif key == "Chains":
            ik.map_diff(where, authored[key], live.get(key, {}), exact, changes, CHAIN_DEFAULTS)
        elif key == "Solvers":
            mine, theirs = authored[key], live.get(key, [])
            for index in range(max(len(mine), len(theirs))):
                at = f"{where}/{index}"
                if index >= len(theirs):
                    changes.append(common.Difference(at, common.DiffKind.ADD, True, mine[index], False, None))
                elif index >= len(mine):
                    changes.append(common.Difference(at, common.DiffKind.REMOVE, False, None, True, theirs[index]))
                else:
                    _solver_diff(at, mine[index], theirs[index], exact, changes, (live_defaults or {}).get(index))
    return changes


def diff(authored, extracted, exact, live_defaults=None):
    changes = common.header_diff(authored, extracted)
    changes.extend(body_diff(authored.get("Body", {}), extracted.get("Body", {}), exact, live_defaults))
    changes.extend(common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact))
    return tuple(changes)


# ---------------------------------------------------------------------------
# Inspect
# ---------------------------------------------------------------------------
def inspect_payload(document, calls):
    body, facts = read_body(calls, "inspect")
    users = {}
    for index, solver in enumerate(body.get("Solvers", [])):
        for goal in solver.get("Goals", {}):
            users.setdefault(goal, {"Solvers": [], "Chains": []})["Solvers"].append(index)
    for name, chain in body.get("Chains", {}).items():
        if chain.get("Goal"):
            users.setdefault(chain["Goal"], {"Solvers": [], "Chains": []})["Chains"].append(name)
    solver_facts = []
    for index in range(len(body.get("Solvers", []))):
        record = facts["solvers"].get(index, {})
        row = {"Index": index, "DisplayName": record.get("display_name", ""),
               "UsesStartBone": record.get("uses_start_bone", False), "UsesEndBone": record.get("uses_end_bone", False),
               "UsesGoalSettings": record.get("uses_goal_settings", False),
               "UsesBoneSettings": record.get("uses_bone_settings", False)}
        if record.get("warning"):
            row["Warning"] = record["warning"]
        solver_facts.append(row)
    payload = {**document, "Skeleton": facts["skeleton"], "GoalUsers": users, "SolverFacts": solver_facts,
               "ChainBones": facts["chain_bones"]}
    if body.get("PreviewMesh"):
        payload["Suggestions"] = suggestions(calls)
    return payload


def suggestions(calls):
    """The IK Rig editor's "Auto Create Retarget Chains" then "Auto Create IK" on a transient copy (rig.suggest.v1):
    the Body they would make (copy what fits into the document) and what the characterization found."""
    records = ik.read_records(calls, ik.SUGGEST)
    body, _ = assemble(records)
    result = {"Body": body}
    for record in records:
        if record.get("kind") == "characterization":
            result["Characterization"] = {
                "UsedTemplate": record.get("used_template", False), "Template": record.get("template", ""),
                "Score": record.get("score", 0), "MatchingBones": record.get("matching_bones", 0),
                "MissingBones": record.get("missing_bones", []),
                "BonesWithMissingParent": record.get("bones_with_missing_parent", []),
                "ExpandedChains": record.get("expanded_chains", {})}
        elif record.get("kind") == "fbik":
            result["AutoIK"] = {"Outcome": record.get("outcome", ""), "MissingChains": record.get("missing_chains", [])}
    return result


# ---------------------------------------------------------------------------
# Handler
# ---------------------------------------------------------------------------
class IKRigRuntimeProfile:
    runtime_entry_module = "asset_document/bootstrap.py"
    # Snapshot-covered: preflight never reads a preview.
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
            raise error("/Inspect", "IK Rig inspection reads a live IK Rig.", "IKRig.InspectNotAdmitted")
        document = canonicalize(self.extract(target=exact_asset_object_path.rsplit(".", 1)[0],
                                             asset_handle=editor_asset_handle, context=context))
        return inspect_payload(document, ik.RigCalls(context, editor_asset_handle, prefix=PREFIX))

    def diff(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=False)

    def extract_with_defaults(self, *, target, asset_handle, context):
        return extract_with_defaults(common.require_target(target, PREFIX), asset_handle, context)

    def diff_with_defaults(self, authored, extracted, live_defaults, exact):
        return diff(canonicalize(authored), canonicalize(extracted), exact=exact, live_defaults=live_defaults)

    def diff_exact(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=True)


def _meta():
    rows = [("preview_mesh", "/Body/PreviewMesh"), ("excluded_bones", "/Body/ExcludedBones"), ("goals", "/Body/Goals"),
            ("solvers", "/Body/Solvers"), ("retarget_root", "/Body/RetargetRoot"), ("chains", "/Body/Chains")]
    regions = tuple(RegionMeta(f"ik_rig.{name}.v1", path, RegionOwnership.MANAGED, IdentityPolicy.STABLE_KEY,
                               OrderPolicy.CANONICAL, DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE,
                               DeletePolicy.EXPLICIT_ONLY, ComparisonPolicy.CANONICAL)
                    for name, path in rows)
    return ProfileMeta(PROFILE_ID, ik.ROUTE, CLASS, regions, ik.REQUIREMENTS)


IK_RIG_RUNTIME_REGISTRATION = RuntimeProfileRegistration(_meta(), IKRigRuntimeProfile,
                                                         RuntimeTargetPolicy.CREATE_OR_UPDATE, ik.LIFECYCLE)

__all__ = ("IK_RIG_RUNTIME_REGISTRATION", "IKRigRuntimeProfile", "assemble", "body_diff", "canonicalize", "diff")

# Module-level on purpose (after everything ik_rig_apply uses from this module): the native runtime restores
# sys.modules between phase calls, so an in-call import would load a second copy of the primitive catalog.
from asset_document.profiles import ik_rig_apply as _apply  # noqa: E402
