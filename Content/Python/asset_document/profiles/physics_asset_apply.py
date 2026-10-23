# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Physics Asset apply (profile.physics_asset.v1): the document as the Physics Asset Editor's edits (design §4.2).

The Body is reconciled in phases, each a batch of ``physics.asset.edit.v1`` ops planned against a fresh read:

  PreviewMesh -> profile lists (Properties.ConstraintProfiles / PhysicalAnimationProfiles: a changed entry at the same
  position is the Details panel's rename) -> constraint renames -> constraints not listed removed -> bodies not listed
  removed (the editor's Delete Body, children first) -> new bodies (the editor's Add Body, parents first) -> body
  shapes, Details and physical animation -> constraints: new ones, bones, frames, Details, profiles; constraints the
  editor's fix-ups made that the document does not list removed -> CollisionDisabled -> the other Properties.

A new asset (Create: the factory's asset, made for the document's mesh) whose document has no Bodies first gets the
editor's generation (``generate`` with Body.Generation over the project's generation settings), then the other
regions. A new body without Shapes is fitted from the mesh with its Fit, as the editor's Add Body does. Fit on a body
that exists or has Shapes, and Generation on an existing asset, are ignored with a warning (create-time helpers).

The status before and after (physics.status.v1) decides the outcome: new errors fail the apply
(``PhysicsAsset.ValidationFailed``, rolled back by Core); errors the asset already had and warnings come back as
warnings.
"""
from __future__ import annotations

from asset_document.diagnostics import DiagnosticSeverity, RuntimeDiagnostic, with_diagnostics
from asset_document.profiles import data_table_common as common
from asset_document.profiles import physics_asset as physics
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument

escape = physics.escape


def error(path, message, code):
    return common.error(path, message, code)


def generic_properties(properties):
    """Top-level Properties the generic Details admission writes (the profile lists have their own op)."""
    return {name: value for name, value in properties.items() if name not in physics.PROFILE_LISTS}


# ---------------------------------------------------------------------------
# Transport
# ---------------------------------------------------------------------------
class Op:
    """One edit op with the document path its errors are reported at: ``fields`` maps an op field to a path,
    ``values`` an op field holding a document object to the path of that object (nested errors keep their tail)."""

    def __init__(self, op, path, fields=None, values=None):
        self.op = op
        self.path = path
        self.fields = fields or {}
        self.values = values or {}


def op_path(primitive_path, ops):
    """/ops/<n>/<field>/... -> the document path of op n (``fields`` / ``values`` keys may name a sub-path)."""
    parts = [part for part in (primitive_path or "").split("/") if part]
    if len(parts) >= 2 and parts[0] == "ops" and parts[1].isdigit() and int(parts[1]) < len(ops):
        item = ops[int(parts[1])]
        rest = parts[2:]
        # The longest mapped prefix ("bones/2" before "bones").
        for size in range(len(rest), 0, -1):
            key = "/".join(rest[:size])
            if key in item.fields:
                return item.fields[key]
            if key in item.values:
                return item.values[key] + "".join(f"/{part}" for part in rest[size:])
        return item.path
    return "/Body"


class Edits:
    """Runs op batches; collects the changed set."""

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
            outcome = common.invoke(self.calls.context, physics.BY_ID, physics.EDIT,
                                    {"schema_version": 1, "ops": [op for op, _ in chunk]},
                                    (NamedOpaqueHandleArgument("asset", self.calls.write),), paths=where,
                                    prefix=physics.PREFIX)
            self.changed.extend(outcome.changed_set)
        return True


def _live(calls):
    body, facts = physics.read_body(calls)
    return {**body, "_profiles": {"ConstraintProfiles": facts["header"].get("constraint_profiles", []),
                                  "PhysicalAnimationProfiles": facts["header"].get("physical_animation_profiles", [])}}


def _differs(authored, live, defaults=None):
    """Whether the authored sparse value differs from the live one (an explicitly authored default is equal)."""
    changes = []
    physics.sparse_diff("", authored, live, defaults, False, changes)
    return bool(changes)


# ---------------------------------------------------------------------------
# Refusals that need no write
# ---------------------------------------------------------------------------
def refuse(body, live):
    mesh = body.get("PreviewMesh", live.get("PreviewMesh")) or ""
    if not mesh:
        new_bodies = [bone for bone in body.get("Bodies", {}) if bone not in live.get("Bodies", {})]
        if new_bodies:
            raise error(f"/Body/Bodies/{escape(new_bodies[0])}", "Adding a body needs Body.PreviewMesh (the skeleton its "
                        "bone and constraints come from), as in the editor.", "PhysicsAsset.PreviewMeshRequired")
    for bone, entry in body.get("Bodies", {}).items():
        if bone not in live.get("Bodies", {}) and "Shapes" not in entry and "Fit" not in entry:
            raise error(f"/Body/Bodies/{escape(bone)}", "A new body needs Shapes, or Fit to fit its shapes from the mesh as "
                        "the editor's Add Body does (\"Fit\": {} uses the project's generation settings).",
                        "PhysicsAsset.ShapesRequired")
    profiles = dict(live.get("_profiles", {}))
    for bone, entry in body.get("Bodies", {}).items():
        for name in entry.get("PhysicalAnimation", {}):
            if name not in profiles.get("PhysicalAnimationProfiles", []):
                raise error(f"/Body/Bodies/{escape(bone)}/PhysicalAnimation/{escape(name)}",
                            f"'{name}' is not a physical animation profile of the asset: list it in "
                            "Properties.PhysicalAnimationProfiles.", "PhysicsAsset.UnknownProfile")
    for joint, entry in body.get("Constraints", {}).items():
        for name in entry.get("Profiles", {}):
            if name not in profiles.get("ConstraintProfiles", []):
                raise error(f"/Body/Constraints/{escape(joint)}/Profiles/{escape(name)}",
                            f"'{name}' is not a constraint profile of the asset: list it in Properties.ConstraintProfiles.",
                            "PhysicsAsset.UnknownProfile")
    bodies = set(body.get("Bodies", live.get("Bodies", {})))
    for joint, entry in body.get("Constraints", {}).items():
        for key in ("Child", "Parent"):
            if entry[key] not in bodies:
                raise error(f"/Body/Constraints/{escape(joint)}/{key}", f"'{entry[key]}' has no body: a constraint joins "
                            f"two bodies.{common.describe_candidates(entry[key], sorted(bodies), 'Bodies')}",
                            "PhysicsAsset.BodyNotFound")
    for index, pair in enumerate(body.get("CollisionDisabled", [])):
        for side, bone in enumerate(pair):
            if bone not in bodies:
                raise error(f"/Body/CollisionDisabled/{index}/{side}",
                            f"'{bone}' has no body.{common.describe_candidates(bone, sorted(bodies), 'Bodies')}",
                            "PhysicsAsset.BodyNotFound")


def _with_profiles(live, properties):
    """The profile lists the apply ends with (live, or the document's)."""
    profiles = dict(live.get("_profiles", {}))
    for name in physics.PROFILE_LISTS:
        if name in properties:
            profiles[name] = list(properties[name])
    return {**live, "_profiles": profiles}


# ---------------------------------------------------------------------------
# Phases
# ---------------------------------------------------------------------------
def phase_mesh(body, properties, live):
    if "PreviewMesh" not in body or (body["PreviewMesh"] or "") == (live.get("PreviewMesh") or ""):
        return []
    return [Op({"op": "set_mesh", "mesh": body["PreviewMesh"] or ""}, "/Body/PreviewMesh")]


def profile_renames(wanted, have):
    """[(from, to)]: an entry changed at the same list position (the Details panel's edit of one entry)."""
    renames = []
    for index in range(min(len(wanted), len(have))):
        old, new = have[index], wanted[index]
        if old != new and old not in wanted and new not in have:
            renames.append((old, new))
    return renames


def phase_profiles(body, properties, live):
    ops = []
    for name, kind in physics.PROFILE_LISTS.items():
        if name not in properties:
            continue
        wanted, have = list(properties[name]), list(live.get("_profiles", {}).get(name, []))
        where = f"/Properties/{name}"
        if type(wanted) is not list or any(type(item) is not str or not item for item in wanted):
            raise error(where, f"{name} is a list of profile names.", "PhysicsAsset.InvalidValue")
        for old, new in profile_renames(wanted, have):
            ops.append(Op({"op": "profiles.rename", "kind": kind, "from": old, "to": new}, where))
        if wanted != have:
            ops.append(Op({"op": "profiles.set", "kind": kind, "names": wanted}, where,
                          {"names": where}))
    return ops


def _renames(constraints, live_constraints):
    return {entry["RenamedFrom"]: name for name, entry in constraints.items()
            if entry.get("RenamedFrom") and entry["RenamedFrom"] != name and entry["RenamedFrom"] in live_constraints}


def phase_constraint_renames(body, properties, live):
    if "Constraints" not in body:
        return []
    constraints, have = body["Constraints"], live.get("Constraints", {})
    renames = _renames(constraints, have)
    ops = []
    for old, new in renames.items():
        if new in have and new not in renames:
            ops.append(Op({"op": "constraint.remove", "joint": new}, f"/Body/Constraints/{escape(new)}"))
    ops += [Op({"op": "constraint.rename", "joint": old, "to": new}, f"/Body/Constraints/{escape(new)}",
               {"to": f"/Body/Constraints/{escape(new)}/RenamedFrom"}) for old, new in renames.items()]
    return ops


def phase_constraint_removals(body, properties, live):
    if "Constraints" not in body:
        return []
    return [Op({"op": "constraint.remove", "joint": joint}, "/Body/Constraints")
            for joint in live.get("Constraints", {}) if joint not in body["Constraints"]]


def phase_body_removals(body, properties, live):
    if "Bodies" not in body:
        return []
    # Extract order is the bone order: children (later) first.
    return [Op({"op": "body.remove", "bone": bone}, "/Body/Bodies")
            for bone in reversed(list(live.get("Bodies", {}))) if bone not in body["Bodies"]]


def phase_body_adds(body, properties, live):
    if "Bodies" not in body:
        return []
    bones = [bone for bone in body["Bodies"] if bone not in live.get("Bodies", {})]
    if not bones:
        return []
    op = {"op": "bodies.add", "bones": bones}
    # A new body without Shapes is fitted from the mesh (the editor's Add Body); with Shapes its Fit is ignored.
    fits = {bone: body["Bodies"][bone]["Fit"] for bone in bones
            if "Fit" in body["Bodies"][bone] and "Shapes" not in body["Bodies"][bone]}
    if fits:
        op["fit"] = fits
    return [Op(op, "/Body/Bodies", {f"bones/{index}": f"/Body/Bodies/{escape(bone)}" for index, bone in enumerate(bones)},
               {f"fit/{escape(bone)}": f"/Body/Bodies/{escape(bone)}/Fit" for bone in fits})]


def phase_body_updates(body, properties, live):
    if "Bodies" not in body:
        return []
    ops = []
    for bone, entry in body["Bodies"].items():
        current = live.get("Bodies", {}).get(bone, {})
        where = f"/Body/Bodies/{escape(bone)}"
        if "Shapes" in entry:
            changes = []
            physics.shapes_diff("", entry["Shapes"], current.get("Shapes", {}), False, changes, bone)
            if changes:
                shapes = {kind: elements for kind, elements in entry["Shapes"].items()}
                ops.append(Op({"op": "body.shapes", "bone": bone, "shapes": shapes}, f"{where}/Shapes",
                              values={"shapes": f"{where}/Shapes"}))
        if _differs(entry.get("Properties", {}), current.get("Properties", {}), physics.body_defaults()["Properties"]):
            ops.append(Op({"op": "body.set", "bone": bone, "properties": entry.get("Properties", {})},
                          f"{where}/Properties", values={"properties": f"{where}/Properties"}))
        if _differs(entry.get("PhysicalAnimation", {}), current.get("PhysicalAnimation", {}),
                    physics.body_defaults()["PhysicalAnimation"]):
            ops.append(Op({"op": "body.physical_animation", "bone": bone, "profiles": entry.get("PhysicalAnimation", {})},
                          f"{where}/PhysicalAnimation", values={"profiles": f"{where}/PhysicalAnimation"}))
    return ops


def phase_constraint_structure(body, properties, live):
    """New constraints and bone changes (frames, Details and profiles follow on a fresh read)."""
    if "Constraints" not in body:
        return []
    ops = []
    for joint, entry in body["Constraints"].items():
        current = live.get("Constraints", {}).get(joint)
        where = f"/Body/Constraints/{escape(joint)}"
        fields = {"child": f"{where}/Child", "parent": f"{where}/Parent", "joint": where}
        if current is None:
            ops.append(Op({"op": "constraint.add", "joint": joint, "child": entry["Child"], "parent": entry["Parent"]},
                          where, fields))
        elif (current.get("Child"), current.get("Parent")) != (entry["Child"], entry["Parent"]):
            ops.append(Op({"op": "constraint.bones", "joint": joint, "child": entry["Child"], "parent": entry["Parent"]},
                          where, fields))
    return ops


def phase_constraint_details(body, properties, live):
    if "Constraints" not in body:
        return []
    ops = []
    for joint, entry in body["Constraints"].items():
        current = live.get("Constraints", {}).get(joint, {})
        where = f"/Body/Constraints/{escape(joint)}"
        if _differs(entry.get("Frames", {}), current.get("Frames", {})):
            op = {"op": "constraint.frames", "joint": joint}
            if entry.get("Frames"):
                op["frames"] = entry["Frames"]
            ops.append(Op(op, f"{where}/Frames", values={"frames": f"{where}/Frames"}))
        if _differs(entry.get("Properties", {}), current.get("Properties", {}),
                    physics.constraint_defaults(current)["Properties"]):
            ops.append(Op({"op": "constraint.set", "joint": joint, "properties": entry.get("Properties", {})},
                          f"{where}/Properties", values={"properties": f"{where}/Properties"}))
    return ops


def phase_constraint_profiles(body, properties, live):
    # After the Details: a new membership copies the constraint's current values (AddConstraintProfile).
    if "Constraints" not in body:
        return []
    ops = []
    for joint, entry in body["Constraints"].items():
        current = live.get("Constraints", {}).get(joint, {})
        where = f"/Body/Constraints/{escape(joint)}"
        if _differs(entry.get("Profiles", {}), current.get("Profiles", {}), physics.constraint_defaults(current)["Profiles"]):
            ops.append(Op({"op": "constraint.profiles", "joint": joint, "profiles": entry.get("Profiles", {})},
                          f"{where}/Profiles", values={"profiles": f"{where}/Profiles"}))
    return ops


def phase_collision(body, properties, live):
    if "CollisionDisabled" not in body:
        return []
    wanted = physics._pair_set(body["CollisionDisabled"])
    have = physics._pair_set(live.get("CollisionDisabled", []))
    ops = []
    enable = [sorted(pair) for pair in have - wanted]
    disable = [pair for pair in body["CollisionDisabled"] if frozenset(pair) not in have]
    if enable:
        ops.append(Op({"op": "collision.set", "pairs": enable, "enabled": True}, "/Body/CollisionDisabled"))
    if disable:
        ops.append(Op({"op": "collision.set", "pairs": disable, "enabled": False}, "/Body/CollisionDisabled"))
    return ops


# Each phase is planned against a fresh read of the asset after the previous one ran.
PHASES = (phase_mesh, phase_profiles, phase_constraint_renames, phase_constraint_removals, phase_body_removals,
          phase_body_adds, phase_body_updates, phase_constraint_structure, phase_constraint_details,
          phase_constraint_profiles, phase_constraint_removals, phase_collision)


def generation_op(body):
    """The editor's generation of a new asset (no Bodies in the document), with Body.Generation's settings."""
    op = {"op": "generate"}
    if "Generation" in body:
        op["params"] = body["Generation"]
        return Op(op, "/Body/Generation", values={"params": "/Body/Generation"})
    return Op(op, "/Body")


def _warning(path, code, message):
    return RuntimeDiagnostic(path=path, code=code, message=message, severity=DiagnosticSeverity.WARNING)


def create_only_warnings(body, live, generated):
    """Create-time helpers that do nothing here: Generation on an existing asset, Fit on a body that exists or has
    Shapes (``live`` is the asset before the bodies are added)."""
    warnings = []
    if "Generation" in body and not generated:
        warnings.append(_warning("/Body/Generation", "PhysicsAsset.GenerationIgnored", "Generation only makes the bodies of "
                                 "a new Physics Asset (Create without Bodies); the asset exists, so it was ignored."))
    for bone, entry in body.get("Bodies", {}).items():
        if "Fit" not in entry:
            continue
        where = f"/Body/Bodies/{escape(bone)}/Fit"
        if bone in live.get("Bodies", {}):
            warnings.append(_warning(where, "PhysicsAsset.FitIgnored", f"Fit only shapes a new body; '{bone}' has a body, "
                                     "so it was ignored (give Shapes, or remove the body and add it again)."))
        elif "Shapes" in entry:
            warnings.append(_warning(where, "PhysicsAsset.FitIgnored", f"'{bone}' has Shapes, which are its shapes; Fit was "
                                     "ignored."))
    return warnings


# ---------------------------------------------------------------------------
# Outcome
# ---------------------------------------------------------------------------
def _messages(status):
    return [item for item in (status or {}).get("Messages", []) if type(item) is dict]


def validation_outcome(before, after):
    """(fatal error or None, warnings): new errors fail the apply; errors the asset already had and warnings are
    reported."""
    def key(item):
        return item.get("Message"), item.get("Path", "")
    old = {key(item) for item in _messages(before) if item.get("Severity") == "Error"}
    errors = [item for item in _messages(after) if item.get("Severity") == "Error"]
    new = [item for item in errors if key(item) not in old]
    if new:
        message = "The physics asset does not validate: " + " | ".join(item.get("Message", "") for item in new[:8])
        return error(new[0].get("Path") or "/Body", message, "PhysicsAsset.ValidationFailed"), ()
    warnings = [RuntimeDiagnostic(path=item.get("Path") or "/Body", code="PhysicsAsset.ValidationError",
                                  message=item.get("Message", ""), severity=DiagnosticSeverity.WARNING) for item in errors]
    warnings += [RuntimeDiagnostic(path=item.get("Path") or "/Body", code="PhysicsAsset.ValidationWarning",
                                   message=item.get("Message", ""), severity=DiagnosticSeverity.WARNING)
                 for item in _messages(after) if item.get("Severity") == "Warning"]
    return None, tuple(warnings)


def _profiles_differ(properties, live):
    return any(name in properties and list(properties[name]) != list(live.get("_profiles", {}).get(name, []))
               for name in physics.PROFILE_LISTS)


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------
def apply(canonical, read, mutable, context, created=False):
    """Core calls apply only when the document differs from the asset (always for a created asset). A created asset
    whose document has no Bodies gets the editor's generation first. The Body (with the profile lists) is applied
    phase by phase against fresh reads, then the other Properties as the Details panel writes them."""
    body = canonical.get("Body", {})
    region = physics._properties()
    properties = canonical.get("Properties", {})
    region.check_owned(properties)
    calls = physics.PhysicsCalls(context, read, mutable)
    live = _live(calls)
    changed = []
    generate = created and "Bodies" not in body
    warnings = create_only_warnings(body, live, generate)
    if generate or physics.body_diff(body, live, exact=False) or _profiles_differ(properties, live):
        before = physics.status(calls)
        edits = Edits(calls)
        if generate and edits.run([generation_op(body)]):
            live = _live(calls)
        refuse(body, _with_profiles(live, properties))
        for phase in PHASES:
            if edits.run(phase(body, properties, live)):
                live = _live(calls)
        changed.extend(edits.changed)
        failure, outcome = validation_outcome(before, physics.status(calls))
        if failure is not None:
            raise failure
        warnings.extend(outcome)
    changed.extend(region.apply(generic_properties(properties), read, mutable, context))
    return with_diagnostics(changed, warnings)


def preflight(canonical, read, context):
    """Refusals that need no write."""
    body = canonical.get("Body", {})
    properties = canonical.get("Properties", {})
    live = _live(physics.PhysicsCalls(context, read))
    if physics.body_diff(body, live, exact=False) or _profiles_differ(properties, live):
        refuse(body, _with_profiles(live, properties))


def preflight_create(canonical):
    """Refusals of a create that need no asset: the document's bodies on a new asset made for its mesh (without Bodies
    the bodies are generated, so constraints and collision pairs are checked on apply)."""
    body = canonical.get("Body", {})
    if "Bodies" not in body:
        return
    mesh, _ = physics.factory_mesh(canonical)
    live = {"PreviewMesh": mesh, "Bodies": {}, "Constraints": {}, "CollisionDisabled": [],
            "_profiles": {name: [] for name in physics.PROFILE_LISTS}}
    refuse(body, _with_profiles(live, canonical.get("Properties", {})))


__all__ = ("PHASES", "apply", "create_only_warnings", "generation_op", "generic_properties", "op_path", "preflight",
           "preflight_create", "profile_renames", "refuse", "validation_outcome")
