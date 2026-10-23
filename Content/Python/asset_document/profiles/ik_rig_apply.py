# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""IK Rig apply (profile.ik_rig.v1): the document Body as the IK Rig editor's edits (design §4.1 / §4.2).

The Body is reconciled in phases, each a batch of ``rig.ikrig.edit.v1`` ops planned against a fresh read of the rig:

  PreviewMesh -> ExcludedBones -> goal renames / adds / bone moves -> chain renames -> RetargetRoot -> chain adds /
  endpoint and goal changes -> solver stack (removes, adds, moves, enable) -> goal connections -> start / end bones
  -> bone settings add / remove -> settings, goal settings, bone settings (struct writes) -> goal Details and
  offsets -> removals of goals and chains not listed.

Connections come before start / end bones because connecting a goal may set a solver's bones. Solvers have no
identity: each document solver takes the first unused live solver of its Type, unmatched live solvers are removed,
new ones appended, and moves bring the stack into document order; a reorder of same-type solvers is a settings
rewrite (functional equivalence). The status before and after (rig.status.v1, the editor's processor initialization) decides the
outcome: new errors fail the apply (``IKRig.ValidationFailed``, rolled back by Core), errors the rig already had
and warnings come back as warnings. Renamed or removed chains report the IK Retargeters that reference the rig
(``IKRig.DependentsAffected``); other packages are never edited.
"""
from __future__ import annotations

from asset_document.diagnostics import DiagnosticSeverity, RuntimeDiagnostic, with_diagnostics
from asset_document.profiles import data_table_common as common
from asset_document.profiles import ik_common as ik
from asset_document.profiles import ik_rig as rig
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument

escape = ik.escape
GOAL_FIELDS = ("PositionAlpha", "RotationAlpha", "PreviewMode", "SizeMultiplier", "ThicknessMultiplier",
               "ExposePosition", "ExposeRotation")
# Body keys that name bones of the preview mesh's skeleton.
BONE_REGIONS = ("ExcludedBones", "Goals", "Chains", "RetargetRoot")


def error(path, message, code):
    return common.error(path, message, code)


# ---------------------------------------------------------------------------
# Transport
# ---------------------------------------------------------------------------
class Op:
    """One edit op with the document path its errors are reported at (``fields`` maps an op field to a path)."""

    def __init__(self, op, path, fields=None, settings=None):
        self.op = op
        self.path = path
        self.fields = fields or {}
        self.settings = settings  # document path of the op's settings / fields object


def _doc_key(field):
    return {"bExposePosition": "ExposePosition", "bExposeRotation": "ExposeRotation"}.get(field, field)


def op_path(primitive_path, ops):
    """/ops/<n>/<field>/... -> the document path of op n."""
    parts = [part for part in (primitive_path or "").split("/") if part]
    if len(parts) >= 2 and parts[0] == "ops" and parts[1].isdigit() and int(parts[1]) < len(ops):
        item = ops[int(parts[1])]
        rest = parts[2:]
        if rest and rest[0] in ("settings", "fields", "offset") and item.settings is not None:
            return item.settings + "".join(f"/{escape(_doc_key(part))}" for part in rest[1:])
        if rest and rest[0] in item.fields:
            return item.fields[rest[0]]
        return item.path
    return "/Body"


class Edits:
    """Runs op batches; collects the changed set and the dependents report."""

    def __init__(self, calls):
        self.calls = calls
        self.changed = []
        self.dependents = []
        self.ran = False

    def run(self, ops):
        if not ops:
            return False
        for chunk in common.chunk_ops([(item.op, item) for item in ops]):
            items = [item for _, item in chunk]

            def where(primitive_path, items=items):
                return op_path(primitive_path, items)
            outcome = common.invoke(self.calls.context, ik.BY_ID, ik.IKRIG_EDIT,
                                    {"schema_version": 1, "ops": [op for op, _ in chunk]},
                                    (NamedOpaqueHandleArgument("asset", self.calls.write),), paths=where,
                                    prefix=rig.PREFIX)
            self.changed.extend(outcome.changed_set)
            value = outcome.value if type(outcome.value) is dict else {}
            self.dependents.extend(item for item in value.get("dependents", []) if type(item) is dict)
        self.ran = True
        return True


def _live(calls):
    body, _ = rig.read_body(calls)
    return body


# ---------------------------------------------------------------------------
# Refusals that need no write
# ---------------------------------------------------------------------------
def _names_bones(body):
    for key in BONE_REGIONS:
        if body.get(key):
            return f"/Body/{key}"
    for index, solver in enumerate(body.get("Solvers", [])):
        for key in ("StartBone", "EndBone", "Bones"):
            if solver.get(key):
                return f"/Body/Solvers/{index}/{key}"
    return None


def refuse(body, live):
    """Stable refusals of what the IK Rig editor cannot do."""
    mesh = body.get("PreviewMesh", live.get("PreviewMesh", "")) or ""
    if "PreviewMesh" in body and not body["PreviewMesh"] and live.get("PreviewMesh"):
        raise error("/Body/PreviewMesh", "The IK Rig editor cannot clear the preview mesh; name the Skeletal Mesh the rig is "
                    "for (it may be another mesh of a compatible skeleton).", "IKRig.CannotClear")
    if not mesh:
        where = _names_bones(body)
        if where is not None:
            raise error(where, "The Body names bones but the IK Rig has no preview mesh: set Body.PreviewMesh (the "
                        "skeleton the bones come from), as the editor's New IK Rig dialog asks for a mesh.",
                        "IKRig.PreviewMeshRequired")
    for name, chain in body.get("Chains", {}).items():
        live_chain = live.get("Chains", {}).get(chain.get("RenamedFrom", name)) or live.get("Chains", {}).get(name)
        if live_chain is None:
            continue
        for key in ("Start", "End"):
            if not chain.get(key) and live_chain.get(key):
                raise error(f"/Body/Chains/{escape(name)}/{key}", f"The chain list cannot clear a chain's {key} bone; "
                            "pick another bone, or remove the chain and add it again under a new name.",
                            "IKRig.CannotClear")


# ---------------------------------------------------------------------------
# Phases
# ---------------------------------------------------------------------------
def phase_mesh(body, live):
    mesh = body.get("PreviewMesh")
    if "PreviewMesh" not in body or not mesh or mesh == live.get("PreviewMesh", ""):
        return []
    return [Op({"op": "set_mesh", "mesh": mesh}, "/Body/PreviewMesh")]


def phase_excluded(body, live):
    if "ExcludedBones" not in body:
        return []
    wanted, have = body["ExcludedBones"], set(live.get("ExcludedBones", []))
    ops = [Op({"op": "exclude", "bone": bone, "excluded": True}, f"/Body/ExcludedBones/{index}")
           for index, bone in enumerate(wanted) if bone not in have]
    ops += [Op({"op": "exclude", "bone": bone, "excluded": False}, "/Body/ExcludedBones")
            for bone in live.get("ExcludedBones", []) if bone not in set(wanted)]
    return ops


def _renames(region, live_region):
    """{old: new} of the entries the document renames (RenamedFrom names a live entry and the new name is free or is
    a live entry the document does not keep)."""
    return {entry["RenamedFrom"]: name for name, entry in region.items()
            if entry.get("RenamedFrom") and entry["RenamedFrom"] != name and entry["RenamedFrom"] in live_region}


def phase_goals(body, live):
    if "Goals" not in body:
        return []
    goals, have = body["Goals"], live.get("Goals", {})
    renames = _renames(goals, have)
    ops = []
    # A rename onto the name of a live goal the document drops: that goal goes first.
    for old, new in renames.items():
        if new in have and new not in renames:
            ops.append(Op({"op": "goal.remove", "goal": new}, f"/Body/Goals/{escape(new)}"))
    for old, new in renames.items():
        ops.append(Op({"op": "goal.rename", "goal": old, "to": new}, f"/Body/Goals/{escape(new)}",
                      {"to": f"/Body/Goals/{escape(new)}/RenamedFrom"}))
    renamed = {new: have[old] for old, new in renames.items()}
    for name, entry in goals.items():
        where = f"/Body/Goals/{escape(name)}"
        current = renamed.get(name) or (have.get(name) if name not in renames.values() else None)
        if current is None:
            ops.append(Op({"op": "goal.add", "goal": name, "bone": entry["Bone"]}, where, {"bone": f"{where}/Bone"}))
        elif current.get("Bone") != entry["Bone"]:
            ops.append(Op({"op": "goal.set_bone", "goal": name, "bone": entry["Bone"]}, f"{where}/Bone"))
    return ops


def phase_chain_renames(body, live):
    if "Chains" not in body:
        return []
    chains, have = body["Chains"], live.get("Chains", {})
    renames = _renames(chains, have)
    ops = []
    for old, new in renames.items():
        if new in have and new not in renames:
            ops.append(Op({"op": "chain.remove", "chain": new}, f"/Body/Chains/{escape(new)}"))
    ops += [Op({"op": "chain.rename", "chain": old, "to": new}, f"/Body/Chains/{escape(new)}",
               {"to": f"/Body/Chains/{escape(new)}/RenamedFrom"}) for old, new in renames.items()]
    return ops


def phase_root(body, live):
    if "RetargetRoot" not in body or (body["RetargetRoot"] or "") == (live.get("RetargetRoot") or ""):
        return []
    return [Op({"op": "root", "bone": body["RetargetRoot"] or ""}, "/Body/RetargetRoot")]


def phase_chains(body, live):
    if "Chains" not in body:
        return []
    have = live.get("Chains", {})
    ops = []
    for name, entry in body["Chains"].items():
        where = f"/Body/Chains/{escape(name)}"
        fields = {"start": f"{where}/Start", "end": f"{where}/End", "goal": f"{where}/Goal"}
        current = have.get(name)
        if current is None:
            op = {"op": "chain.add", "chain": name, "start": entry.get("Start") or "", "end": entry.get("End") or ""}
            if entry.get("Goal"):
                op["goal"] = entry["Goal"]
            ops.append(Op(op, where, fields))
            continue
        op = {"op": "chain.set", "chain": name}
        for key, field in (("Start", "start"), ("End", "end")):
            if entry.get(key) and entry[key] != current.get(key):
                op[field] = entry[key]
        if (entry.get("Goal") or "") != (current.get("Goal") or ""):
            op["goal"] = entry.get("Goal") or ""
        if len(op) > 2:
            ops.append(Op(op, where, fields))
    return ops


def match_solvers(live_types, authored_types):
    """{live index: authored index}: each authored solver takes the first unused live solver of its Type, so a
    reorder is a move and keeps the solver's settings."""
    used, pairs = set(), {}
    for j, kind in enumerate(authored_types):
        for i, have in enumerate(live_types):
            if i not in used and have == kind:
                used.add(i)
                pairs[i] = j
                break
    return pairs


def plan_solver_stack(live_types, authored_types):
    """(removes, adds, moves): live indices to remove (descending), authored indices to append (in order), then
    (from, to) moves (MoveSolverInStack: remove, insert) that bring the stack into document order."""
    pairs = match_solvers(live_types, authored_types)
    removes = sorted((i for i in range(len(live_types)) if i not in pairs), reverse=True)
    matched = set(pairs.values())
    adds = [j for j in range(len(authored_types)) if j not in matched]
    stack = [pairs[i] for i in sorted(pairs)] + adds
    moves = []
    for position in range(len(authored_types)):
        current = stack.index(position)
        if current != position:
            moves.append((current, position))
            stack.insert(position, stack.pop(current))
    return removes, adds, moves


def phase_solver_stack(body, live):
    if "Solvers" not in body:
        return []
    authored, have = body["Solvers"], live.get("Solvers", [])
    removes, adds, moves = plan_solver_stack([item.get("Type", "") for item in have],
                                             [item["Type"] for item in authored])
    ops = [Op({"op": "solver.remove", "index": index}, "/Body/Solvers") for index in removes]
    ops += [Op({"op": "solver.add", "type": authored[j]["Type"]}, f"/Body/Solvers/{j}",
               {"type": f"/Body/Solvers/{j}/Type"}) for j in adds]
    ops += [Op({"op": "solver.move", "index": source, "to": target}, f"/Body/Solvers/{target}")
            for source, target in moves]
    return ops


def _solver_pairs(body, live):
    """(index, authored, live) of the aligned stack (after phase_solver_stack)."""
    have = live.get("Solvers", [])
    return [(index, entry, have[index] if index < len(have) else {}) for index, entry in enumerate(body["Solvers"])]


def phase_solver_enable(body, live):
    if "Solvers" not in body:
        return []
    return [Op({"op": "solver.enable", "index": index, "enabled": entry.get("Enabled", True)},
               f"/Body/Solvers/{index}/Enabled")
            for index, entry, current in _solver_pairs(body, live)
            if entry.get("Enabled", True) != current.get("Enabled", True)]


def phase_connections(body, live):
    if "Solvers" not in body:
        return []
    ops = []
    for index, entry, current in _solver_pairs(body, live):
        wanted, have = entry.get("Goals", {}), current.get("Goals", {})
        where = f"/Body/Solvers/{index}/Goals"
        ops += [Op({"op": "solver.disconnect", "index": index, "goal": goal}, where) for goal in have if goal not in wanted]
        ops += [Op({"op": "solver.connect", "index": index, "goal": goal}, f"{where}/{escape(goal)}")
                for goal in wanted if goal not in have]
    return ops


def phase_solver_bones(body, live):
    if "Solvers" not in body:
        return []
    ops = []
    for index, entry, current in _solver_pairs(body, live):
        for key, kind in (("StartBone", "solver.start_bone"), ("EndBone", "solver.end_bone")):
            where = f"/Body/Solvers/{index}/{key}"
            wanted, have = entry.get(key) or "", current.get(key) or ""
            if wanted == have:
                continue
            if not wanted:
                raise error(where, f"The solver stack cannot clear a solver's {key}; name a bone (or remove the solver "
                            "and add a new one).", "IKRig.CannotClear")
            ops.append(Op({"op": kind, "index": index, "bone": wanted}, where))
    return ops


def phase_bone_settings(body, live):
    if "Solvers" not in body:
        return []
    ops = []
    for index, entry, current in _solver_pairs(body, live):
        wanted, have = entry.get("Bones", {}), current.get("Bones", {})
        where = f"/Body/Solvers/{index}/Bones"
        ops += [Op({"op": "solver.bone_setting.remove", "index": index, "bone": bone}, where)
                for bone in have if bone not in wanted]
        ops += [Op({"op": "solver.bone_setting.add", "index": index, "bone": bone}, f"{where}/{escape(bone)}")
                for bone in wanted if bone not in have]
    return ops


def _differs(authored, live):
    changes = []
    ik.value_diff("", authored or {}, live or {}, False, changes)
    return bool(changes)


def phase_settings(body, live):
    if "Solvers" not in body:
        return []
    ops = []
    for index, entry, current in _solver_pairs(body, live):
        where = f"/Body/Solvers/{index}"
        if _differs(entry.get("Settings"), current.get("Settings")):
            ops.append(Op({"op": "solver.settings", "index": index, "settings": entry.get("Settings", {})},
                          f"{where}/Settings", settings=f"{where}/Settings"))
        for goal, settings in entry.get("Goals", {}).items():
            if _differs(settings, current.get("Goals", {}).get(goal)):
                at = f"{where}/Goals/{escape(goal)}"
                ops.append(Op({"op": "solver.goal_settings", "index": index, "goal": goal, "settings": settings}, at,
                              settings=at))
        for bone, settings in entry.get("Bones", {}).items():
            if _differs(settings, current.get("Bones", {}).get(bone)):
                at = f"{where}/Bones/{escape(bone)}"
                ops.append(Op({"op": "solver.bone_settings", "index": index, "bone": bone, "settings": settings}, at,
                              settings=at))
    return ops


def phase_goal_details(body, live):
    if "Goals" not in body:
        return []
    have = live.get("Goals", {})
    ops = []
    for name, entry in body["Goals"].items():
        current = have.get(name, {})
        where = f"/Body/Goals/{escape(name)}"
        fields = {key: entry[key] for key in GOAL_FIELDS if key in entry}
        live_fields = {key: current[key] for key in GOAL_FIELDS if key in current}
        changes = []
        ik.value_diff(where, fields, live_fields, False, changes, {key: rig.GOAL_DEFAULTS[key] for key in GOAL_FIELDS})
        if changes:
            ops.append(Op({"op": "goal.set", "goal": name, "fields": fields}, where, settings=where))
    return ops


def phase_removals(body, live):
    ops = []
    if "Goals" in body:
        ops += [Op({"op": "goal.remove", "goal": name}, "/Body/Goals") for name in live.get("Goals", {})
                if name not in body["Goals"]]
    if "Chains" in body:
        ops += [Op({"op": "chain.remove", "chain": name}, "/Body/Chains") for name in live.get("Chains", {})
                if name not in body["Chains"]]
    return ops


def phase_goal_offsets(body, live):
    # Last: every controller edit re-initializes the rig (BroadcastNeedsReinitialized -> ResetInitialGoalTransforms),
    # which re-bases each goal on its bone and, through a ref pose rotation that is not quite unit length, moves a goal
    # offset by a few millionths. The offsets are set after every other edit, from the live state of that moment.
    if "Goals" not in body:
        return []
    have = live.get("Goals", {})
    ops = []
    for name, entry in body["Goals"].items():
        if _differs(entry.get("Offset"), have.get(name, {}).get("Offset")):
            where = f"/Body/Goals/{escape(name)}/Offset"
            ops.append(Op({"op": "goal.offset", "goal": name, "offset": entry.get("Offset", {})}, where, settings=where))
    return ops


PHASES = (phase_mesh, phase_excluded, phase_goals, phase_chain_renames, phase_root, phase_chains, phase_solver_stack,
          phase_solver_enable, phase_connections, phase_solver_bones, phase_bone_settings, phase_settings,
          phase_goal_details, phase_removals, phase_goal_offsets)


def check_solver_types(body, calls):
    """Solver Types are the list_types names (FullBodyIK, LimbIK, ...): a struct path of a listed type is refused
    with its name, so an extract of the result equals the document."""
    if "Solvers" not in body:
        return
    rows = calls.query(ik.CATALOG, {"kind": "solver", "limit": 400}).get("rows", [])
    names = {row.get("Name") for row in rows}
    by_path = {row.get("Path"): row.get("Name") for row in rows}
    for index, entry in enumerate(body["Solvers"]):
        kind = entry["Type"]
        if kind in names:
            continue
        where = f"/Body/Solvers/{index}/Type"
        if kind in by_path:
            raise error(where, f"Solver types are named as list_types kind ikrig_solver names them: '{by_path[kind]}', "
                        f"not '{kind}'.", "IKRig.UnknownSolverType")
        raise error(where, f"'{kind}' is not a solver type. Types: {', '.join(sorted(name for name in names if name))} "
                    "(list_types kind ikrig_solver).", "IKRig.UnknownSolverType")


# ---------------------------------------------------------------------------
# Outcome
# ---------------------------------------------------------------------------
def _messages(status):
    return [item for item in (status or {}).get("Messages", []) if type(item) is dict]


def validation_outcome(before, after):
    """(fatal error or None, warnings): new initialization errors fail the apply; errors the rig already had and
    warnings are reported."""
    def key(item):
        return item.get("Message"), item.get("Path", "")
    old = {key(item) for item in _messages(before) if item.get("Severity") == "Error"}
    errors = [item for item in _messages(after) if item.get("Severity") == "Error"]
    new = [item for item in errors if key(item) not in old]
    if new:
        message = "The IK Rig does not initialize: " + " | ".join(item.get("Message", "") for item in new[:8])
        return error(new[0].get("Path") or "/Body", message, "IKRig.ValidationFailed"), ()
    warnings = [RuntimeDiagnostic(path=item.get("Path") or "/Body", code="IKRig.ValidationError",
                                  message=item.get("Message", ""), severity=DiagnosticSeverity.WARNING) for item in errors]
    warnings += [RuntimeDiagnostic(path=item.get("Path") or "/Body", code="IKRig.ValidationWarning",
                                   message=item.get("Message", ""), severity=DiagnosticSeverity.WARNING)
                 for item in _messages(after) if item.get("Severity") == "Warning"]
    return None, tuple(warnings)


def dependents_warnings(dependents):
    """IKRig.DependentsAffected: the retargeters that reference the rig after a chain rename / removal (decision
    §8.2: reported, never edited)."""
    warnings = []
    for item in dependents:
        chains = [row for row in item.get("chains", []) if type(row) is dict]
        what = ", ".join(f"'{row.get('chain')}' -> '{row['to']}'" if row.get("to") else f"'{row.get('chain')}' removed"
                         for row in chains)
        where = "/Body/Chains"
        if chains:
            last = chains[-1]
            where = f"/Body/Chains/{escape(last.get('to') or last.get('chain', ''))}" if last.get("to") else where
        detail = ""
        if item.get("loaded"):
            detail = f" (loaded; uses this rig as {'/'.join(item.get('sides', [])) or 'neither side'})"
        warnings.append(RuntimeDiagnostic(
            path=where, code="IKRig.DependentsAffected", severity=DiagnosticSeverity.WARNING,
            message=f"IK Retargeter {item.get('package', '')}{detail} references this IK Rig; its op chain mappings of "
                    f"{what} are not updated (the editor updates only retargeters open in an editor; a closed one "
                    "re-syncs on load and loses the mapping). Apply that retargeter's document to map the chains again."))
    return warnings


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------
def apply(canonical, read, mutable, context):
    """Core calls apply only when the document differs from the asset. The Body is applied first (phases against
    fresh reads), then the Properties as the Details panel writes them."""
    body = canonical.get("Body", {})
    region = rig._properties()
    properties = canonical.get("Properties", {})
    region.check_owned(properties)
    calls = ik.RigCalls(context, read, mutable, prefix=rig.PREFIX)
    live = _live(calls)
    changed = []
    warnings = []
    if rig.body_diff(body, live, exact=False):
        refuse(body, live)
        check_solver_types(body, calls)
        before = ik.status(calls)
        edits = Edits(calls)
        for phase in PHASES:
            if edits.run(phase(body, live)):
                live = _live(calls)
        changed.extend(edits.changed)
        failure, outcome = validation_outcome(before, ik.status(calls))
        if failure is not None:
            raise failure
        warnings.extend(outcome)
        warnings.extend(dependents_warnings(edits.dependents))
    changed.extend(region.apply(properties, read, mutable, context))
    return with_diagnostics(changed, warnings)


def preflight(canonical, read, context):
    """Refusals that need no write (an existing rig: read; a new one: the document alone)."""
    body = canonical.get("Body", {})
    if read is None:
        refuse(body, {})
        return
    live = _live(ik.RigCalls(context, read, prefix=rig.PREFIX))
    if rig.body_diff(body, live, exact=False):
        refuse(body, live)


__all__ = ("PHASES", "apply", "dependents_warnings", "match_solvers", "op_path", "plan_solver_stack", "preflight",
           "refuse", "validation_outcome")
