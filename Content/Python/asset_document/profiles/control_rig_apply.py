# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Control Rig apply (profile.control_rig.v1): the document as the Control Rig editor's edits.

Authorable: Class Settings (Properties; ``PreviewSkeletalMesh`` as the editor's preview mesh picker), the rig type
(Rig <-> Module: "Turn into Rig Module" / "Turn into Standalone Rig"), Module settings, member Variables, the
whole Hierarchy (every element type, parents and weights, poses, control settings / values / limits, spaces,
channel hosts, metadata), the event Graphs and the local Functions (control_rig_graph.py: graphs, function signature
/ access / header / local variables, nodes of every kind including function and collapse nodes, links, pin values,
bindings, pin types, aggregate operands, traits, variable maps). A changed public function that other rigs call is
reported (``ControlRig.DependentsAffected``); they are not edited. A document without Graphs on a rig whose graphs are
empty gets the Forwards Solve event the editor adds on open. Modular rigs: control_rig_modular.py (modules,
connections, settings; editing a rig module that loaded modular rigs use refreshes them, as the editor's compile does:
reported, or refused while one of them has unsaved changes).

Each phase is a batch of ``rig.controlrig.edit.v1`` ops planned against a fresh read of the rig:

  FactoryContext import (a new rig) -> preview mesh -> Forwards Solve -> rig type conversion -> module settings -> Properties -> variables (renames,
  removes, adds, types, details / defaults, order) -> hierarchy (renames, removes, adds, element writes, order)
  -> functions and graphs (dependents, function renames / adds, function interfaces, structure rounds, node shape,
  wiring, values, function removes) -> modules (control_rig_modular.PHASES) -> compile.

The rig is compiled (``rig.controlrig.compile.v1``, the editor's Compile button) before and after the edits: new
compile errors fail the apply (``ControlRig.CompileFailed``, rolled back by Core); errors the rig already had and
warnings come back as warnings.
"""
from __future__ import annotations

import json

from asset_document.diagnostics import DiagnosticSeverity, RuntimeDiagnostic, with_diagnostics
from asset_document.profiles import control_rig as cr
from asset_document.profiles import control_rig_graph as crg
from asset_document.profiles import control_rig_modular as crm
from asset_document.profiles import data_table_common as common
from asset_document.profiles import ik_common as ik
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument

escape = common.escape
PREVIEW_MESH = "PreviewSkeletalMesh"
# Element types whose parents are weighted (multi-parent elements).
WEIGHTED = ("Null", "Control")
# Elements "Turn into Rig Module" keeps (it refuses any other element).
MODULE_KEEPS = ("Curve", "Connector")
VARIABLE_FIELDS = (("Public", "public", False), ("ReadOnly", "read_only", False),
                   ("ExposeToCinematics", "expose", False), ("Category", "category", ""), ("Tooltip", "tooltip", ""))
FORWARDS_SOLVE = "RigUnit_BeginExecution"
ROOT_MODULE = "/ControlRig/Modules/Modules56/Root.Root_C"


def error(path, message, code):
    return common.error(path, message, code)


# ---------------------------------------------------------------------------
# Transport
# ---------------------------------------------------------------------------
class Op:
    """One edit op with the document path its errors are reported at: ``fields`` maps an op field to a path,
    ``nested`` an op field holding a document object (entry, settings) to that object's path."""

    def __init__(self, op, path, fields=None, nested=None):
        self.op = op
        self.path = path
        self.fields = fields or {}
        self.nested = nested or {}


def op_path(primitive_path, ops):
    """/ops/<n>/<field>/... -> the document path of op n."""
    parts = [part for part in (primitive_path or "").split("/") if part]
    if len(parts) >= 2 and parts[0] == "ops" and parts[1].isdigit() and int(parts[1]) < len(ops):
        item = ops[int(parts[1])]
        rest = parts[2:]
        if rest and rest[0] in item.nested:
            return item.nested[rest[0]] + "".join(f"/{part}" for part in rest[1:])
        if rest and rest[0] in item.fields:
            return item.fields[rest[0]]
        return item.path
    return "/Body"


class Edits:
    """Runs op batches and collects the changed set and the edit's notes (warnings)."""

    def __init__(self, calls):
        self.calls = calls
        self.changed = []
        self.warnings = []

    def run(self, ops):
        if not ops:
            return False
        for chunk in common.chunk_ops([(item.op, item) for item in ops]):
            items = [item for _, item in chunk]

            def where(primitive_path, items=items):
                return op_path(primitive_path, items)
            outcome = common.invoke(self.calls.context, ik.BY_ID, ik.CONTROLRIG_EDIT,
                                    {"schema_version": 1, "ops": [op for op, _ in chunk]},
                                    (NamedOpaqueHandleArgument("asset", self.calls.write),), paths=where,
                                    prefix=cr.PREFIX)
            value = outcome.value if type(outcome.value) is dict else {}
            # A batch that changed nothing (the op found the state already there) leaves the asset unsaved.
            if value.get("changed") is not False:
                self.changed.extend(outcome.changed_set)
            for note in value.get("notes") or ():
                if type(note) is dict:
                    field = note.get("field") or ""
                    self.warnings.append(RuntimeDiagnostic(
                        path=where(f"/ops/{note.get('op', '')}" + (f"/{field}" if field else "")),
                        code=str(note.get("code") or "ControlRig.EditNote"), message=str(note.get("message") or ""),
                        severity=DiagnosticSeverity.WARNING))
        return True


def compile_rig(calls):
    """The editor's Compile: {Status, Messages}."""
    outcome = common.invoke(calls.context, ik.BY_ID, ik.CONTROLRIG_COMPILE, {"schema_version": 1},
                            (NamedOpaqueHandleArgument("asset", calls.write),), prefix=cr.PREFIX)
    value = outcome.value if type(outcome.value) is dict else {}
    return value.get("compile") or {}, tuple(outcome.changed_set)


def read_live(calls):
    """The rig's Body as Apply compares it: template nodes with their complete pin types."""
    body, facts = cr.read_body(calls)
    return cr.with_full_types(body, facts["pin_types"])


def _mesh_path(value):
    """A soft object path value as a string ("" for none)."""
    if value is None:
        return ""
    if type(value) is dict:
        value = value.get("Path") or value.get("path") or ""
    return value if type(value) is str and value != "None" else ""


# ---------------------------------------------------------------------------
# Keys and validation
# ---------------------------------------------------------------------------
def element_key(entry):
    return f"{entry.get('Type', '')}({entry.get('Name', '')})"


def parents_of(entry):
    """The parent keys of an element entry, in order."""
    if type(entry.get("Parent")) is str and entry["Parent"]:
        return [entry["Parent"]]
    return [item.get("Parent") for item in entry.get("Parents", []) if type(item) is dict]


def first_parent(entry):
    parents = parents_of(entry)
    return parents[0] if parents else ""


def _references(entry):
    """(field, key) of every element an entry names."""
    refs = []
    if type(entry.get("Parent")) is str:
        refs.append(("Parent", entry["Parent"]))
    for index, item in enumerate(entry.get("Parents", []) if type(entry.get("Parents")) is list else []):
        if type(item) is dict and type(item.get("Parent")) is str:
            refs.append((f"Parents/{index}/Parent", item["Parent"]))
    for field, inner in (("Spaces", "Space"), ("Hosts", "Space")):
        for index, item in enumerate(entry.get(field, []) if type(entry.get(field)) is list else []):
            key = item.get(inner) if type(item) is dict else item
            if type(key) is str:
                refs.append((f"{field}/{index}" + (f"/{inner}" if type(item) is dict else ""), key))
    return refs


def _renamed_from(entry):
    old = entry.get("RenamedFrom")
    return old if type(old) is str and old and old != entry.get("Name") else None


def check_hierarchy(hierarchy, live_keys, mesh_keys=False):
    """Stable refusals of a Hierarchy region before any write: duplicate keys, references to elements the region
    does not list, parents listed after their children, procedural elements. ``mesh_keys`` (a FactoryContext
    document): the rig's imported bones and curves, which it need not list (None when not imported yet: any Bone /
    Curve key passes, and the edit refuses one the rig does not have); False for other documents."""
    seen = {}
    for index, entry in enumerate(hierarchy):
        where = f"/Body/Hierarchy/{index}"
        if entry.get("Type") == "Reference":
            raise error(f"{where}/Type", "Reference elements are made by graph code at runtime, never authored.",
                        "ControlRig.ProceduralElement")
        key = element_key(entry)
        if key in seen:
            raise error(f"{where}/Name", f"{key} is listed twice (also at /Body/Hierarchy/{seen[key]}).",
                        "ControlRig.DuplicateElement")
        seen[key] = index
        old = _renamed_from(entry)
        if old is not None and f"{entry['Type']}({old})" not in live_keys and key not in live_keys:
            raise error(f"{where}/RenamedFrom", f"RenamedFrom names {entry['Type']}({old}), which the rig does not "
                        "have.", "ControlRig.ElementNotFound")
    for index, entry in enumerate(hierarchy):
        where = f"/Body/Hierarchy/{index}"
        for field, key in _references(entry):
            if key not in seen and mesh_keys is not False and (
                    key in (mesh_keys or ()) or (mesh_keys is None and key.startswith(("Bone(", "Curve(")))):
                continue
            if key not in seen:
                close = ik.close_names(key, list(seen) + sorted(mesh_keys or ()))
                hint = f" Did you mean {', '.join(close)}? Element names are matched exactly." if close else ""
                scope = ("neither listed in the Hierarchy nor imported from FactoryContext" if mesh_keys is not False
                         else "not an element of the Hierarchy (the region is the complete hierarchy: list every element)")
                raise error(f"{where}/{field}", f"{key} is {scope}.{hint}", "ControlRig.ElementNotFound")
        parent = first_parent(entry)
        if parent and parent in seen and seen[parent] > index:
            raise error(f"{where}/{'Parent' if 'Parent' in entry else 'Parents/0/Parent'}",
                        f"{parent} is listed after its child {element_key(entry)}; list parents first (the editor's "
                        "hierarchy order).", "ControlRig.ParentAfterChild")


def check_variables(variables):
    seen = {}
    for index, entry in enumerate(variables):
        name = entry["Name"]
        if name in seen:
            raise error(f"/Body/Variables/{index}/Name", f"Variable '{name}' is listed twice.",
                        "ControlRig.DuplicateVariable")
        seen[name] = index


# ---------------------------------------------------------------------------
# Refusals and the graph rule
# ---------------------------------------------------------------------------
def fresh(live):
    """A rig as the Content Browser's factory makes it (nothing authored yet)."""
    return (live.get("RigType", "Rig") == "Rig" and not live.get("Hierarchy") and not live.get("Variables")
            and pristine(live))


def pristine(live):
    """A rig whose graphs the editor would fill on open: one model, no nodes, no functions."""
    graphs = live.get("Graphs", {})
    return len(graphs) == 1 and not live.get("Functions") and all(not item.get("Nodes") for item in graphs.values())


def _region_differs(body, live, key):
    if key not in body:
        return False
    return bool(cr.body_diff({key: body[key]}, {key: live.get(key, {})}, exact=False))


def plan(body, live, creating):
    """(add Forwards Solve, conversion op or None); raises the refusals that need no write."""
    live_type = live.get("RigType", "Rig")
    wanted_type = body.get("RigType", live_type)
    if (wanted_type == "Modular") != (live_type == "Modular"):
        raise error("/Body/RigType", f"A {live_type} rig cannot become {wanted_type}: modular rigs are a different kind "
                    "of asset (create a new one).", "ControlRig.RigTypeImmutable")
    if "ParentClass" in body:
        live_parent = live.get("ParentClass", cr.MODULAR_PARENT if live_type == "Modular" else cr.RIG_PARENT)
        if body["ParentClass"] != live_parent:
            raise error("/Body/ParentClass", f"The rig's parent class is {live_parent}; the Control Rig editor does not "
                        "reparent rigs (create the rig from the other class in the Content Browser).",
                        "ControlRig.ParentClassImmutable")
    # A document without RigType keeps the rig's type (a sparse Update of a modular rig need not repeat it).
    cr.check_regions(body, wanted_type)
    if live_type == "Modular":
        crm.check(body, live)
        return False, None
    conversion = None
    if wanted_type != live_type:
        conversion = "rig.to_module" if wanted_type == "Module" else "rig.to_standalone"
        if conversion == "rig.to_module" and "Hierarchy" not in body:
            blocking = [element_key(entry) for entry in live.get("Hierarchy", []) if _blocks_module(entry)]
            if blocking:
                raise error("/Body/RigType", "Turn into Rig Module keeps only imported bones, curves and connectors; "
                            f"this rig also has {', '.join(blocking[:8])}. List the module's Hierarchy in the document "
                            "(the others are removed first).", "ControlRig.ConversionRefused")
    # A document without Graphs on an empty rig: the event the editor's first open adds. With Graphs, they are the
    # complete state (control_rig_graph.py).
    forwards_solve = pristine(live) and "Graphs" not in body
    crg.check(body, live)
    return forwards_solve, conversion


def _blocks_module(entry):
    if entry.get("Type") == "Bone":
        return not entry.get("Imported")
    return entry.get("Type") not in MODULE_KEEPS


# ---------------------------------------------------------------------------
# Phases
# ---------------------------------------------------------------------------
def phase_module(body, live):
    if "Module" not in body or not _region_differs(body, live, "Module"):
        return []
    return [Op({"op": "module.settings", "settings": body["Module"]}, "/Body/Module", nested={"settings": "/Body/Module"})]


def _variable_renames(variables, have):
    return {old: entry["Name"] for entry in variables
            for old in [entry.get("RenamedFrom")] if type(old) is str and old and old != entry["Name"] and old in have}


def phase_variables(body, live):
    """Renames, removes, adds, type changes (one batch, before the details)."""
    if "Variables" not in body:
        return []
    variables = body["Variables"]
    have = {entry["Name"]: entry for entry in live.get("Variables", [])}
    renames = _variable_renames(variables, have)
    wanted = {entry["Name"] for entry in variables}
    ops = []
    for old, new in renames.items():
        index = next(i for i, entry in enumerate(variables) if entry["Name"] == new)
        if new in have and new not in renames:
            ops.append(Op({"op": "variable.remove", "name": new}, f"/Body/Variables/{index}"))
        ops.append(Op({"op": "variable.rename", "name": old, "to": new}, f"/Body/Variables/{index}",
                      {"to": f"/Body/Variables/{index}/Name"}))
    after = {renames.get(name, name): entry for name, entry in have.items()}
    for name in have:
        if name not in renames and name not in wanted:
            ops.append(Op({"op": "variable.remove", "name": name}, "/Body/Variables"))
            after.pop(name, None)
    for index, entry in enumerate(variables):
        where = f"/Body/Variables/{index}"
        current = after.get(entry["Name"])
        if current is None:
            ops.append(Op({"op": "variable.add", "name": entry["Name"], "type": entry["Type"]}, where,
                          {"type": f"{where}/Type", "name": f"{where}/Name"}))
        elif current.get("Type") != entry["Type"]:
            ops.append(Op({"op": "variable.type", "name": entry["Name"], "type": entry["Type"]}, f"{where}/Type"))
    return ops


def phase_variable_details(body, live):
    if "Variables" not in body:
        return []
    have = {entry["Name"]: entry for entry in live.get("Variables", [])}
    ops = []
    for index, entry in enumerate(body["Variables"]):
        where = f"/Body/Variables/{index}"
        current = have.get(entry["Name"], {})
        op = {"op": "variable.set", "name": entry["Name"]}
        fields = {}
        for key, field, default in VARIABLE_FIELDS:
            if not common.same(entry.get(key, default), current.get(key, default), False):
                op[field] = entry.get(key, default)
                fields[field] = f"{where}/{key}"
        if "Default" in entry and ("Default" not in current or not common.same(entry["Default"], current["Default"], False)):
            op["default"] = entry["Default"]
        elif "Default" not in entry and "Default" in current:
            op["default"] = None
        if len(op) > 2:
            ops.append(Op(op, where, fields, {"default": f"{where}/Default"}))
    names = [entry["Name"] for entry in body["Variables"]]
    live_names = [entry["Name"] for entry in live.get("Variables", []) if entry["Name"] in set(names)]
    if len(names) > 1 and names != live_names:
        ops.append(Op({"op": "variable.order", "names": names}, "/Body/Variables"))
    return ops


def _wire(entry):
    """An element entry as the edit primitive takes it (RenamedFrom is an instruction, not state)."""
    return {key: value for key, value in entry.items() if key != "RenamedFrom"}


def phase_element_renames(body, live):
    if "Hierarchy" not in body:
        return []
    have = {element_key(entry) for entry in live.get("Hierarchy", [])}
    ops = []
    for index, entry in enumerate(body["Hierarchy"]):
        old = _renamed_from(entry)
        if old is None or f"{entry['Type']}({old})" not in have:
            continue
        where = f"/Body/Hierarchy/{index}"
        key = element_key(entry)
        if key in have:
            ops.append(Op({"op": "element.remove", "key": key}, where))
        ops.append(Op({"op": "element.rename", "key": f"{entry['Type']}({old})", "to": entry["Name"]}, where,
                      {"to": f"{where}/Name", "key": f"{where}/RenamedFrom"}))
    return ops


def phase_element_removes(body, live, keep=None, unlisted_kept=None):
    """Elements the document does not list, children first. ``keep`` filters what stays (the module pre-removal);
    ``unlisted_kept`` keeps unlisted elements it accepts (a FactoryContext document's imported bones and curves)."""
    if "Hierarchy" not in body and keep is None:
        return []
    wanted = {element_key(entry) for entry in body.get("Hierarchy", [])} if keep is None else set()
    ops = []
    for entry in reversed(live.get("Hierarchy", [])):
        key = element_key(entry)
        if key in wanted or (keep is not None and keep(entry)) or (unlisted_kept is not None and unlisted_kept(entry)):
            continue
        ops.append(Op({"op": "element.remove", "key": key}, "/Body/Hierarchy"))
    return ops


def _reduced(entry):
    """What an element is added with: its first parent only, no spaces / hosts (they may name later elements)."""
    wire = {key: value for key, value in _wire(entry).items() if key not in ("Spaces", "Hosts", "Parents")}
    if type(entry.get("Parents")) is list and entry["Parents"]:
        wire["Parent"] = entry["Parents"][0]["Parent"]
    return wire


def phase_element_adds(body, live):
    if "Hierarchy" not in body:
        return []
    have = {element_key(entry) for entry in live.get("Hierarchy", [])}
    return [Op({"op": "element.add", "entry": _reduced(entry)}, f"/Body/Hierarchy/{index}",
               nested={"entry": f"/Body/Hierarchy/{index}"})
            for index, entry in enumerate(body["Hierarchy"]) if element_key(entry) not in have]


def phase_element_sets(body, live):
    if "Hierarchy" not in body:
        return []
    have = {element_key(entry): entry for entry in live.get("Hierarchy", [])}
    ops = []
    for index, entry in enumerate(body["Hierarchy"]):
        wire = _wire(entry)
        changes = []
        ik.value_diff("", wire, have.get(element_key(entry), {}), False, changes)
        if changes:
            ops.append(Op({"op": "element.set", "entry": wire}, f"/Body/Hierarchy/{index}",
                          nested={"entry": f"/Body/Hierarchy/{index}"}))
    return ops


def phase_element_order(body, live):
    if "Hierarchy" not in body:
        return []
    groups = {}
    for entry in body["Hierarchy"]:
        groups.setdefault(first_parent(entry), []).append(element_key(entry))
    live_groups = {}
    for entry in live.get("Hierarchy", []):
        live_groups.setdefault(first_parent(entry), []).append(element_key(entry))
    ops = []
    for parent, keys in groups.items():
        current = [key for key in live_groups.get(parent, []) if key in set(keys)]
        if len(keys) > 1 and current != keys:
            ops.append(Op({"op": "element.order", "keys": keys}, "/Body/Hierarchy"))
    return ops


# ---------------------------------------------------------------------------
# Outcome
# ---------------------------------------------------------------------------
def _messages(status):
    return [item for item in (status or {}).get("Messages", []) if type(item) is dict]


def validation_outcome(before, after):
    """(fatal error or None, warnings): new compile errors fail the apply; errors the rig already had and warnings
    are reported."""
    def key(item):
        return item.get("Message"), item.get("Path", "")
    old = {key(item) for item in _messages(before) if item.get("Severity") == "Error"}
    errors = [item for item in _messages(after) if item.get("Severity") == "Error"]
    new = [item for item in errors if key(item) not in old]
    if not new and (after or {}).get("Status") == "Error" and (before or {}).get("Status") != "Error":
        new = [{"Message": "the compiler reported an error without a message"}]
    if new:
        message = "The Control Rig does not compile: " + " | ".join(item.get("Message", "") for item in new[:8])
        return error(new[0].get("Path") or "/Body", message, "ControlRig.CompileFailed"), ()
    warnings = [RuntimeDiagnostic(path=item.get("Path") or "/Body", code="ControlRig.CompileError",
                                  message=item.get("Message", ""), severity=DiagnosticSeverity.WARNING) for item in errors]
    warnings += [RuntimeDiagnostic(path=item.get("Path") or "/Body", code="ControlRig.CompileWarning",
                                   message=item.get("Message", ""), severity=DiagnosticSeverity.WARNING)
                 for item in _messages(after) if item.get("Severity") == "Warning"]
    return None, tuple(warnings)


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------

def _difference_text(item):
    """One graph difference for the GraphDiffers warning: path, the document's value, the rig's value."""
    before, after = item.before, item.after
    if type(before) is list and type(after) is list:
        # Lists (a graph's Order) from their first difference.
        first = next((index for index, pair in enumerate(zip(before, after)) if pair[0] != pair[1]), min(len(before), len(after)))
        if first:
            before, after = [f"[{first}]..."] + before[first:], [f"[{first}]..."] + after[first:]

    def text(present, value):
        return json.dumps(value, sort_keys=True)[:120] if present else "absent"
    return f"{item.path}: document {text(item.before_present, before)}, rig {text(item.after_present, after)}"


def _make(op, path, fields):
    return Op(op, path, fields, {"value": fields["value"]} if "value" in fields else None)


def read_modular_live(calls):
    """The live body with the rig's primary connectors (control_rig_modular.PRIMARIES), which order the connects."""
    body, facts = cr.read_body(calls)
    body[crm.PRIMARIES] = list(facts["header"].get("primary_connectors") or ())
    return body


def run_modular(edits, calls, body, live):
    """Modules, connections and settings (control_rig_modular.PHASES), re-reading the rig after each batch."""
    for name, planner in crm.PHASES:
        ops = planner(body, live, _make)
        if ops and name == "connects" and crm.PRIMARIES not in live:
            live = read_modular_live(calls)
            ops = planner(body, live, _make)
        if edits.run(ops):
            live = read_modular_live(calls)
    live.pop(crm.PRIMARIES, None)
    return live


def mesh_keys(live):
    return {element_key(entry) for entry in live.get("Hierarchy", []) if cr.imported_kept(entry)}


def apply(canonical, read, mutable, context, creating=False):
    """Core calls apply when the document differs from the asset (and after creating it). ``creating``: the call
    that created the rig."""
    body = canonical.get("Body", {})
    factory = canonical.get("FactoryContext") or None
    region = cr._properties()
    properties = dict(canonical.get("Properties", {}))
    mesh = properties.pop(PREVIEW_MESH, None) if PREVIEW_MESH in properties else False
    imports = {name: properties.pop(name) for name in cr.SOURCE_IMPORTS if name in properties}
    region.check_owned(properties)
    calls = ik.RigCalls(context, read, mutable, prefix=cr.PREFIX)
    live = read_live(calls)
    if "Variables" in body:
        check_variables(body["Variables"])
    # A new modular rig whose import source is set was made by "Create Modular Rig" on its preview mesh (a preview mesh
    # picked later imports the mesh's sockets too and sets no import source): that mesh is its FactoryContext.
    if (factory is None and _mesh_path(mesh) and _mesh_path(imports.get("SourceHierarchyImport"))
            and body.get("RigType") == "Modular" and (creating or fresh(live))):
        factory, factory_path = _mesh_path(mesh), f"/Properties/{PREVIEW_MESH}"
    else:
        factory_path = "/FactoryContext"
    # FactoryContext on a new rig: "Create Control Rig" on the mesh (its bones and curves), before anything else.
    importing = factory is not None and not any(entry.get("Type") == "Bone" for entry in live.get("Hierarchy", [])) and (
        creating or fresh(live))
    if "Hierarchy" in body:
        check_hierarchy(body["Hierarchy"], {element_key(entry) for entry in live.get("Hierarchy", [])},
                        None if importing else (mesh_keys(live) if factory else False))
    creating = creating or fresh(live)
    forwards_solve, conversion = plan(body, live, creating)
    before, changed = compile_rig(calls)
    changed = list(changed)
    edits = Edits(calls)
    warnings = []
    if live.get("RigType") == "Module":
        # Before any write: loaded modular rigs that use this module (refused while one has unsaved changes).
        edits.run([Op({"op": "module.dependents"}, "/Body")])

    if importing:
        edits.run([Op({"op": "hierarchy.import", "mesh": factory}, factory_path, {"mesh": factory_path})])
        live = read_live(calls)
    if mesh is not False and live.get("RigType") == "Modular" and _mesh_path(mesh):
        # A modular rig's preview mesh is its hierarchy (the editor imports it on a new preview mesh).
        edits.run([Op({"op": "modular.preview_mesh", "mesh": _mesh_path(mesh)}, f"/Properties/{PREVIEW_MESH}",
                      {"mesh": f"/Properties/{PREVIEW_MESH}"})])
        live = read_live(calls)
    elif mesh is not False:
        # The edit sets the mesh only when it differs (Properties are not readable in the Apply phase).
        edits.run([Op({"op": "preview_mesh", "mesh": _mesh_path(mesh)}, f"/Properties/{PREVIEW_MESH}")])
    # Preview mesh and import sources before the hierarchy: an imported bone without Transform takes the reference
    # pose of SourceHierarchyImport. The edit sets only a different value.
    edits.run([Op({"op": "source_import", "property": name, "path": _mesh_path(value)}, f"/Properties/{name}")
               for name, value in imports.items()])
    if forwards_solve:
        edits.run([Op({"op": "graph.forwards_solve"}, "/Body/Graphs")])
    if conversion is not None:
        if conversion == "rig.to_module" and "Hierarchy" in body:
            if edits.run(phase_element_removes(body, live, keep=lambda entry: not _blocks_module(entry))):
                live = read_live(calls)
        edits.run([Op({"op": conversion}, "/Body/RigType")])
        live = read_live(calls)
    edits.run(phase_module(body, live))
    changed.extend(region.apply(properties, read, mutable, context))

    if edits.run(phase_variables(body, live)):
        live = read_live(calls)
    if edits.run(phase_variable_details(body, live)):
        live = read_live(calls)

    if edits.run(phase_element_renames(body, live)):
        live = read_live(calls)
    if edits.run(phase_element_removes(body, live, unlisted_kept=cr.imported_kept if factory else None)):
        live = read_live(calls)
    if edits.run(phase_element_adds(body, live)):
        live = read_live(calls)
    if edits.run(phase_element_sets(body, live)):
        live = read_live(calls)
    if edits.run(phase_element_order(body, live)):
        live = read_live(calls)
    graph_warnings = []
    if "Graphs" in body or "Functions" in body:
        crg.check(body, live)
        for name in crg.PHASES:
            if name != "structure":
                if edits.run(crg.phase(name, body, live, Op, deprecated=creating)):
                    live = read_live(calls)
                continue
            # Rounds: a new collapse node's graph exists (and is planned) after the round that added the node.
            planned = set()
            for _ in range(crg.MAX_STRUCTURE_ROUNDS):
                count = len(planned)
                if edits.run(crg.phase(name, body, live, Op, deprecated=creating, planned=planned)):
                    live = read_live(calls)
                if len(planned) == count:
                    break
        for item in cr.order_kept(body, live):
            graph_warnings.append(RuntimeDiagnostic(
                path=item.path, code="ControlRig.OrderKept", severity=DiagnosticSeverity.WARNING,
                message=f"{item.path} lists existing {'graphs' if item.path.endswith('GraphOrder') else 'nodes'} in "
                        f"another order than the rig has them (the rig: {json.dumps(item.after)[:300]}): they keep their place (the editor "
                        "cannot reorder them; new ones are added last, in the document's order), and the order is not "
                        "compared. Extract shows the rig's order."))
        differences = crg.differences(body, live)
        if differences:
            graph_warnings.append(RuntimeDiagnostic(
                path=differences[0].path, code="ControlRig.GraphDiffers", severity=DiagnosticSeverity.WARNING,
                message=f"The graphs differ from the document after the edit at {len(differences)} place(s) "
                        f"({'; '.join(_difference_text(item) for item in differences[:4])}): the editor normalized or "
                        "refused part of it silently; Extract shows the rig's state."))

    if live.get("RigType") == "Modular":
        live = run_modular(edits, calls, body, live)

    after, compiled = compile_rig(calls)
    changed.extend(edits.changed)
    changed.extend(compiled)
    failure, compile_warnings = validation_outcome(before, after)
    if failure is not None:
        raise failure
    return with_diagnostics(changed, tuple(warnings) + tuple(edits.warnings) + tuple(graph_warnings)
                            + tuple(compile_warnings))


def preflight(canonical, read, context):
    """Refusals that need no write (an existing rig: read; a new one: the document alone)."""
    body = canonical.get("Body", {})
    if "Variables" in body:
        check_variables(body["Variables"])
    factory = canonical.get("FactoryContext") or None
    if read is None:
        if "Hierarchy" in body:
            check_hierarchy(body["Hierarchy"], set(), None if factory else False)
        if body.get("RigType") == "Modular":
            # The factory's modular rig: module Root (the Control Rig settings' DefaultRootModule).
            new = {"RigType": "Modular", "Modules": [{"Name": "Root", "Class": ROOT_MODULE}], "Connections": {}}
        else:
            new = {"Graphs": {"Rig": {"Nodes": {}}}, "Functions": {}}
        plan(body, new, True)
        return
    live = read_live(ik.RigCalls(context, read, prefix=cr.PREFIX))
    if "Hierarchy" in body:
        check_hierarchy(body["Hierarchy"], {element_key(entry) for entry in live.get("Hierarchy", [])},
                        mesh_keys(live) if factory else False)
    plan(body, live, False)


__all__ = ("apply", "check_hierarchy", "mesh_keys", "run_modular", "first_parent", "op_path", "phase_element_order", "phase_element_sets",
           "phase_variable_details", "phase_variables", "plan", "preflight", "pristine", "validation_outcome")
