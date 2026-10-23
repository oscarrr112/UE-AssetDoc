# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""IK Retargeter apply (profile.ik_retargeter.v1): the document Body as the IK Retarget editor's edits (design §7.2).

The Body is reconciled in phases, each a batch of ``rig.ikretargeter.edit.v1`` ops planned against a fresh read:

  side IK Rigs (Details pick + "assign to all ops") -> side preview meshes -> op renames -> op removals (ops the
  document drops, ops whose Type changed) -> op adds (parents before children) -> op parents -> op stack order (one
  move at a time, re-planned from a fresh read: the stack re-sorts after each move) -> op enable -> op target IK Rigs
  -> op settings -> chain mappings and chain settings -> poses (renames, removals, adds, rotation offsets, current).

Ops are matched by name (case-insensitive, as the op stack compares names; ``RenamedFrom`` renames); a matched op
whose Type differs is replaced. Poses are matched by name per side. The status before and after (rig.status.v1, the
editor's processor initialization) decides the outcome: new errors fail the apply (``IKRetargeter.ValidationFailed``,
rolled back by Core); errors the retargeter already had and warnings come back as warnings.
"""
from __future__ import annotations

from asset_document.diagnostics import DiagnosticSeverity, RuntimeDiagnostic, with_diagnostics
from asset_document.profiles import data_table_common as common
from asset_document.profiles import ik_common as ik
from asset_document.profiles import ik_retargeter as rtg
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument

escape = ik.escape
SIDES = ("Source", "Target")
# Op fields whose errors point into the document object under the op (op path + "/<document key>/...").
NESTED = {"settings": "Settings", "mapping": "ChainMapping", "bones": "Bones"}
MAX_MOVES = 200


def error(path, message, code):
    return common.error(path, message, code)


# ---------------------------------------------------------------------------
# Transport
# ---------------------------------------------------------------------------
class Op:
    """One edit op with the document path its errors are reported at (``fields`` maps an op field to a path;
    ``nested`` is the document object a settings / mapping / bones field maps into)."""

    def __init__(self, op, path, fields=None, nested=None):
        self.op = op
        self.path = path
        self.fields = fields or {}
        self.nested = nested


def op_path(primitive_path, ops):
    """/ops/<n>/<field>/... -> the document path of op n."""
    parts = [part for part in (primitive_path or "").split("/") if part]
    if len(parts) >= 2 and parts[0] == "ops" and parts[1].isdigit() and int(parts[1]) < len(ops):
        item = ops[int(parts[1])]
        rest = parts[2:]
        if rest and rest[0] in NESTED and item.nested is not None:
            return item.nested + "".join(f"/{part}" for part in rest[1:])
        if rest and rest[0] in item.fields:
            return item.fields[rest[0]]
        return item.path
    return "/Body"


class Edits:
    def __init__(self, calls):
        self.calls = calls
        self.changed = []

    def run(self, ops):
        if not ops:
            return False
        for chunk in common.chunk_ops([(item.op, item) for item in ops]):
            items = [item for _, item in chunk]

            def where(primitive_path, items=items):
                return op_path(primitive_path, items)
            outcome = common.invoke(self.calls.context, ik.BY_ID, ik.RETARGETER_EDIT,
                                    {"schema_version": 1, "ops": [op for op, _ in chunk]},
                                    (NamedOpaqueHandleArgument("asset", self.calls.write),), paths=where,
                                    prefix=rtg.PREFIX)
            self.changed.extend(outcome.changed_set)
        return True


class Live:
    """A read of the retargeter: the Body, the raw side facts and the flat op stack."""

    def __init__(self, body, sides):
        self.body = body
        self.sides = sides
        self.stack = flatten(body.get("Ops", []))
        self.by_name = {entry["Name"].lower(): (entry, parent) for entry, parent in self.stack}

    @staticmethod
    def read(calls):
        body, facts = rtg.read_body(calls)
        return Live(body, facts["sides"])


def flatten(ops):
    """[(entry, parent name)] in stack order: each top-level op's children, then the op (the stack keeps children
    before their parent)."""
    stack = []
    for entry in ops:
        for child in entry.get("Children", []):
            stack.append((child, entry["Name"]))
        stack.append((entry, entry.get("Parent", "")))
    return stack


def _doc_ops(body):
    """[(entry, parent name, document path)] of the document's ops, parents before children."""
    out = []
    for index, entry in enumerate(body.get("Ops", [])):
        where = f"/Body/Ops/{index}"
        out.append((entry, entry.get("Parent", ""), where))
        for child_index, child in enumerate(entry.get("Children", [])):
            out.append((child, entry["Name"], f"{where}/Children/{child_index}"))
    return out


def _differs(authored, live):
    return not common.same(authored, live, exact=False)


# ---------------------------------------------------------------------------
# Phases (each: (body, live) -> [Op])
# ---------------------------------------------------------------------------
def phase_rigs(body, live):
    ops = []
    for side in SIDES:
        if side in body and "IKRig" in body[side]:
            wanted = body[side]["IKRig"] or ""
            if wanted != (live.body.get(side, {}).get("IKRig") or ""):
                ops.append(Op({"op": "rig.set", "side": side, "rig": wanted}, f"/Body/{side}/IKRig"))
    return ops


def phase_meshes(body, live):
    ops = []
    for side in SIDES:
        if side not in body:
            continue
        facts = live.sides.get(side, {})
        stored = facts.get("preview_mesh") or ""
        wanted = body[side]["PreviewMesh"] or "" if "PreviewMesh" in body[side] else facts.get("rig_mesh") or ""
        if wanted != stored:
            ops.append(Op({"op": "preview_mesh", "side": side, "mesh": wanted}, f"/Body/{side}/PreviewMesh"))
    return ops


def phase_op_renames(body, live):
    if "Ops" not in body:
        return []
    wanted = {entry["Name"].lower() for entry, _, _ in _doc_ops(body)}
    ops = []
    renames = []
    for entry, _, where in _doc_ops(body):
        old = entry.get("RenamedFrom")
        if not old or old.lower() == entry["Name"].lower() or old.lower() not in live.by_name:
            continue
        if old.lower() in wanted:
            continue  # the old name stays in the document: no rename
        renames.append((live.by_name[old.lower()][0]["Name"], entry["Name"], where))
    # A rename onto the name of a live op the document drops: that op goes first.
    for _, new, where in renames:
        if new.lower() in live.by_name and new.lower() not in {old.lower() for old, _, _ in renames}:
            ops.append(Op({"op": "op.remove", "name": live.by_name[new.lower()][0]["Name"]}, where))
    ops += [Op({"op": "op.rename", "name": old, "to": new}, where, {"to": f"{where}/Name"}) for old, new, where in renames]
    return ops


def phase_op_removals(body, live):
    if "Ops" not in body:
        return []
    wanted = {entry["Name"].lower(): entry for entry, _, _ in _doc_ops(body)}

    def dropped(entry):
        keep = wanted.get(entry["Name"].lower())
        return keep is None or keep["Type"] != entry.get("Type")
    # Removing an op removes its children (UIKRetargeterController::RemoveRetargetOp).
    removed_parents = {entry["Name"].lower() for entry, parent in live.stack if not parent and dropped(entry)}
    return [Op({"op": "op.remove", "name": entry["Name"]}, "/Body/Ops") for entry, parent in live.stack
            if dropped(entry) and not (parent and parent.lower() in removed_parents)]


def phase_op_adds(body, live):
    ops = []
    for entry, parent, where in _doc_ops(body):
        if entry["Name"].lower() in live.by_name:
            continue
        op = {"op": "op.add", "type": entry["Type"], "name": entry["Name"]}
        if parent:
            op["parent"] = parent
        ops.append(Op(op, where, {"type": f"{where}/Type", "name": f"{where}/Name", "parent": f"{where}/Parent"}))
    return ops


def phase_op_parents(body, live):
    ops = []
    for entry, parent, where in _doc_ops(body):
        have = live.by_name.get(entry["Name"].lower())
        if have is not None and parent and have[1].lower() != parent.lower():
            ops.append(Op({"op": "op.parent", "name": have[0]["Name"], "parent": parent}, where,
                          {"parent": f"{where}/Parent"}))
    return ops


def plan_move(body, live):
    """The next op stack move (an op, the stack index to move it to) that brings the stack closer to the document's
    order, or None when the order matches: top-level ops in document order first, then each op's children."""
    names = [entry["Name"].lower() for entry, _ in live.stack]
    parents = [parent.lower() for _, parent in live.stack]

    def move_before(want, before):
        source, target = names.index(want), names.index(before)
        return live.stack[source][0]["Name"], target - 1 if source < target else target

    tops = [name for name, parent in zip(names, parents) if not parent]
    doc_tops = [entry["Name"].lower() for entry in body.get("Ops", []) if entry["Name"].lower() in names]
    for index, want in enumerate(doc_tops):
        if index < len(tops) and tops[index] != want:
            return move_before(want, tops[index])
    for entry in body.get("Ops", []):
        parent = entry["Name"].lower()
        children = [name for name, owner in zip(names, parents) if owner == parent]
        doc_children = [child["Name"].lower() for child in entry.get("Children", []) if child["Name"].lower() in children]
        for index, want in enumerate(doc_children):
            if children[index] != want:
                return move_before(want, children[index])
    return None


def phase_op_enable(body, live):
    ops = []
    for entry, _, where in _doc_ops(body):
        have = live.by_name.get(entry["Name"].lower())
        if have is not None and entry.get("Enabled", True) != have[0].get("Enabled", True):
            ops.append(Op({"op": "op.enable", "name": have[0]["Name"], "enabled": entry.get("Enabled", True)},
                          f"{where}/Enabled"))
    return ops


def phase_op_target_rigs(body, live):
    ops = []
    for entry, _, where in _doc_ops(body):
        have = live.by_name.get(entry["Name"].lower())
        if have is None or "TargetIKRig" not in entry:
            continue
        wanted = entry["TargetIKRig"] or ""
        if wanted != (have[0].get("TargetIKRig") or ""):
            ops.append(Op({"op": "op.target_rig", "name": have[0]["Name"], "rig": wanted}, f"{where}/TargetIKRig"))
    return ops


def phase_op_settings(body, live):
    ops = []
    for entry, _, where in _doc_ops(body):
        have = live.by_name.get(entry["Name"].lower())
        if have is not None and _differs(entry.get("Settings", {}), have[0].get("Settings", {})):
            ops.append(Op({"op": "op.settings", "name": have[0]["Name"], "settings": entry.get("Settings", {})},
                          f"{where}/Settings", nested=f"{where}/Settings"))
    return ops


def phase_op_chains(body, live):
    ops = []
    for entry, _, where in _doc_ops(body):
        have = live.by_name.get(entry["Name"].lower())
        if have is None:
            continue
        name, current = have[0]["Name"], have[0]
        if "ChainMapping" in entry and _differs(entry["ChainMapping"], current.get("ChainMapping", {})):
            ops.append(Op({"op": "op.map", "name": name, "mapping": entry["ChainMapping"]}, f"{where}/ChainMapping",
                          nested=f"{where}/ChainMapping"))
        chains, live_chains = entry.get("Chains", {}), current.get("Chains", {})
        for chain in list(chains) + [chain for chain in live_chains if chain not in chains]:
            if _differs(chains.get(chain, {}), live_chains.get(chain, {})):
                at = f"{where}/Chains/{escape(chain)}"
                ops.append(Op({"op": "op.chain_settings", "name": name, "chain": chain, "settings": chains.get(chain, {})},
                              at, {"chain": at}, nested=at))
    return ops


def _pose_sets(body, live):
    for side in SIDES:
        pose_set = body.get("Poses", {}).get(side)
        if pose_set is not None and "Poses" in pose_set:
            have = live.body.get("Poses", {}).get(side, {"Current": rtg.DEFAULT_POSE, "Poses": {}})
            yield side, pose_set["Poses"], {name.lower(): name for name in have.get("Poses", {})}, have


def phase_pose_structure(body, live):
    ops = []
    for side, poses, have_names, _ in _pose_sets(body, live):
        wanted = {name.lower() for name in poses}
        renames = []
        for name, pose in poses.items():
            old = (pose.get("RenamedFrom") or "").lower()
            if old and old != name.lower() and old in have_names and old not in wanted:
                renames.append((have_names[old], name))
        renamed_to = {new.lower() for _, new in renames}
        for name_lower, name in have_names.items():
            if name_lower not in wanted and name_lower not in {old.lower() for old, _ in renames} \
                    and name != rtg.DEFAULT_POSE:
                ops.append(Op({"op": "pose.remove", "side": side, "pose": name}, f"/Body/Poses/{side}/Poses"))
        for old, new in renames:
            where = f"/Body/Poses/{side}/Poses/{escape(new)}"
            ops.append(Op({"op": "pose.rename", "side": side, "pose": old, "to": new}, where,
                          {"to": f"{where}/RenamedFrom"}))
        for name in poses:
            if name.lower() not in have_names and name.lower() not in renamed_to:
                ops.append(Op({"op": "pose.add", "side": side, "pose": name}, f"/Body/Poses/{side}/Poses/{escape(name)}"))
    return ops


def phase_pose_content(body, live):
    ops = []
    for side, poses, have_names, have in _pose_sets(body, live):
        for name, pose in poses.items():
            current = have["Poses"].get(have_names.get(name.lower(), name))
            if current is None:
                continue
            root, bones = pose.get("RootOffset", 0.0), pose.get("Bones", {})
            stored = rtg.canonical_bones(bones, current.get("Bones", {})) if type(bones) is dict else bones
            if _differs(root, current.get("RootOffset", 0.0)) or _differs(stored, current.get("Bones", {})):
                where = f"/Body/Poses/{side}/Poses/{escape(name)}"
                ops.append(Op({"op": "pose.set", "side": side, "pose": have_names.get(name.lower(), name), "root": root,
                               "bones": bones}, where, {"root": f"{where}/RootOffset"}, nested=f"{where}/Bones"))
    return ops


def phase_pose_current(body, live, initial):
    ops = []
    for side in SIDES:
        pose_set = body.get("Poses", {}).get(side, {})
        have = live.body.get("Poses", {}).get(side, {})
        names = {name.lower(): name for name in have.get("Poses", {})}
        wanted = pose_set.get("Current") or initial.get(side) or rtg.DEFAULT_POSE
        wanted = names.get(wanted.lower(), rtg.DEFAULT_POSE)
        if have and wanted != have.get("Current"):
            ops.append(Op({"op": "pose.current", "side": side, "pose": wanted}, f"/Body/Poses/{side}/Current"))
    return ops


PHASES = (phase_rigs, phase_meshes, phase_op_renames, phase_op_removals, phase_op_adds, phase_op_parents)
OP_CONTENT_PHASES = (phase_op_enable, phase_op_target_rigs, phase_op_settings, phase_op_chains)
POSE_PHASES = (phase_pose_structure, phase_pose_content)


# ---------------------------------------------------------------------------
# Refusals that need no write
# ---------------------------------------------------------------------------
def check_op_types(body, calls):
    """Op Types are the list_types names (FKChains, RunIKRig, ...); parent rules come from the catalog."""
    if "Ops" not in body:
        return
    rows = calls.query(ik.CATALOG, {"kind": "op", "limit": 400}).get("rows", [])
    by_name = {row.get("Name"): row for row in rows}
    by_path = {row.get("Path"): row.get("Name") for row in rows}
    singletons = {}
    for entry, parent, where in _doc_ops(body):
        kind = entry["Type"]
        if kind not in by_name:
            if kind in by_path:
                raise error(f"{where}/Type", f"Op types are named as list_types kind ikretarget_op names them: "
                            f"'{by_path[kind]}', not '{kind}'.", "IKRetargeter.UnknownOpType")
            raise error(f"{where}/Type", f"'{kind}' is not a retarget op type. Types: "
                        f"{', '.join(sorted(name for name in by_name if name))} (list_types kind ikretarget_op).",
                        "IKRetargeter.UnknownOpType")
        row = by_name[kind]
        if row.get("Singleton"):
            if kind in singletons:
                raise error(f"{where}/Type", f"A retargeter has at most one '{kind}' op ({singletons[kind]} is one).",
                            "IKRetargeter.SingletonOp")
            singletons[kind] = where
        parent_type = row.get("ParentType") or ""
        nested = "/Children/" in where
        if parent_type and not nested and not entry.get("Parent"):
            raise error(where, f"A '{kind}' op runs under a '{parent_type}' op: list it in that op's Children.",
                        "IKRetargeter.InvalidParentOp")
        if not parent_type and nested:
            raise error(where, f"A '{kind}' op is a top-level op (it has no parent op).", "IKRetargeter.InvalidParentOp")
        if nested:
            owner = next(item for item, _, at in _doc_ops(body) if where.startswith(at + "/Children/"))
            if owner["Type"] != parent_type:
                raise error(where, f"A '{kind}' op runs under a '{parent_type}' op; '{owner['Name']}' is a "
                            f"'{owner['Type']}' op.", "IKRetargeter.InvalidParentOp")


# ---------------------------------------------------------------------------
# Outcome
# ---------------------------------------------------------------------------
def _messages(status):
    return [item for item in (status or {}).get("Messages", []) if type(item) is dict]


def validation_outcome(before, after):
    """(fatal error or None, warnings): new initialization errors fail the apply; errors the retargeter already had
    and warnings are reported."""
    def key(item):
        return item.get("Message"), item.get("Path", "")
    old = {key(item) for item in _messages(before) if item.get("Severity") == "Error"}
    errors = [item for item in _messages(after) if item.get("Severity") == "Error"]
    new = [item for item in errors if key(item) not in old]
    if new:
        message = "The IK Retargeter does not initialize: " + " | ".join(item.get("Message", "") for item in new[:8])
        return error(new[0].get("Path") or "/Body", message, "IKRetargeter.ValidationFailed"), ()
    warnings = [RuntimeDiagnostic(path=item.get("Path") or "/Body", code="IKRetargeter.ValidationError",
                                  message=item.get("Message", ""), severity=DiagnosticSeverity.WARNING) for item in errors]
    warnings += [RuntimeDiagnostic(path=item.get("Path") or "/Body", code="IKRetargeter.ValidationWarning",
                                   message=item.get("Message", ""), severity=DiagnosticSeverity.WARNING)
                 for item in _messages(after) if item.get("Severity") == "Warning"]
    return None, tuple(warnings)


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------
def reconcile(body, calls, edits):
    """Runs every phase against fresh reads."""
    live = Live.read(calls)
    initial = {side: live.body.get("Poses", {}).get(side, {}).get("Current", "") for side in SIDES}

    def step(ops):
        nonlocal live
        if edits.run(ops):
            live = Live.read(calls)

    for phase in PHASES:
        step(phase(body, live))
    if "Ops" in body:
        for _ in range(MAX_MOVES):
            move = plan_move(body, live)
            if move is None:
                break
            step([Op({"op": "op.move", "name": move[0], "to": move[1]}, "/Body/Ops")])
        else:
            raise error("/Body/Ops", "The op stack did not reach the document's order.", "IKRetargeter.EditFailed")
    for phase in OP_CONTENT_PHASES:
        step(phase(body, live))
    for phase in POSE_PHASES:
        step(phase(body, live))
    step(phase_pose_current(body, live, initial))


def apply(canonical, read, mutable, context):
    """Core calls apply only when the document differs from the asset. The Body first (phases against fresh reads),
    then the Properties as the Details panel writes them."""
    body = canonical.get("Body", {})
    region = rtg._properties()
    properties = canonical.get("Properties", {})
    region.check_owned(properties)
    calls = ik.RigCalls(context, read, mutable, prefix=rtg.PREFIX)
    live, _ = rtg.read_body(calls)
    changed = []
    warnings = []
    if rtg.body_diff(body, live, exact=False):
        check_op_types(body, calls)
        before = ik.status(calls)
        edits = Edits(calls)
        reconcile(body, calls, edits)
        changed.extend(edits.changed)
        failure, outcome = validation_outcome(before, ik.status(calls))
        if failure is not None:
            raise failure
        warnings.extend(outcome)
    changed.extend(region.apply(properties, read, mutable, context))
    return with_diagnostics(changed, warnings)


def preflight(canonical, read, context):
    """Refusals that need no write."""
    body = canonical.get("Body", {})
    if read is None:
        return
    calls = ik.RigCalls(context, read, prefix=rtg.PREFIX)
    live, _ = rtg.read_body(calls)
    if rtg.body_diff(body, live, exact=False):
        check_op_types(body, calls)


__all__ = ("Live", "OP_CONTENT_PHASES", "PHASES", "POSE_PHASES", "apply", "check_op_types", "flatten", "op_path",
           "plan_move", "preflight", "reconcile", "validation_outcome")
