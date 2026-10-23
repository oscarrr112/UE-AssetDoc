# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Niagara document shapes (docs/architecture/asset-document-niagara-design.md §3), built from the rows of
``fx.niagara.read.v1``.  Pure functions: rows in, document Body / Inspect facts out.

Row kinds (every row has ``kind``): ``system`` (header), ``user_parameter``, ``declared_parameter``, ``emitter``, ``module``,
``renderer``, ``event_handler``, ``simulation_stage``, ``library`` and ``compile`` (Inspect), and the script
graph rows ``script``, ``parameter``, ``node``, ``link``, ``pin_default`` (with ``scope``: ``""`` for a script
asset, ``"<emitter>/<scratch pad>"`` or ``"/<scratch pad>"`` for scratch pads).  Parameter collections:
``parameter`` rows without ``scope``.
"""
from __future__ import annotations

from contextlib import contextmanager

STACK_GROUPS = ("EmitterSpawn", "EmitterUpdate", "ParticleSpawn", "ParticleUpdate")
SYSTEM_GROUPS = ("SystemSpawn", "SystemUpdate")


def _put(out, key, value, keep=True):
    if keep:
        out[key] = value


# Set while an extract with diff_only shapes its document (sparse_data_interfaces).
_SPARSE = []


@contextmanager
def sparse_data_interfaces(sparse=True):
    """Within it, a data interface's Properties are written sparse (the read's ``SparseProperties``: struct members
    equal to the class default's left out, also in a curve's keys; an apply edits the class default's values). Outside
    it (an apply's comparisons, Verify / Diff) they are whole."""
    _SPARSE.append(sparse)
    try:
        yield
    finally:
        _SPARSE.pop()


def _data_interface(value):
    out = dict(value)
    sparse = out.pop("SparseProperties", None)
    if _SPARSE and _SPARSE[-1] and type(sparse) is dict:
        out["Properties"] = sparse
    return out


# ---------------------------------------------------------------------------
# Module inputs (the stack's value modes)
# ---------------------------------------------------------------------------
def input_value(row):
    """Document value form of one stack input (design §3.2 table)."""
    mode = row.get("mode")
    if mode == "Local" and "value" in row:
        return row["value"]
    if mode == "Linked":
        out = {"Link": row.get("link", "")}
        _put(out, "Type", row.get("link_type"), "link_type" in row)
        return out
    if mode == "Dynamic":
        dynamic = row.get("dynamic", {})
        out = {"Script": dynamic.get("script", "")}
        _put(out, "Version", dynamic.get("version"), "version" in dynamic)
        out["Inputs"] = inputs_map(dynamic.get("inputs", []))
        return {"Dynamic": out}
    if mode == "Expression":
        return {"Expression": row.get("expression", "")}
    if mode == "Data" and "data_interface" in row:
        return {"DataInterface": _data_interface(row["data_interface"])}
    if mode == "ObjectAsset":
        return {"Object": row.get("object", "")}
    # DefaultFunction / InvalidOverride / UnsupportedDefault, or a value the reader could not encode:
    # listed, never silently dropped.
    return {"Unsupported": mode or "None"}


def inputs_map(rows):
    return {row["name"]: input_value(row) for row in rows if row.get("overridden", True)}


def module_entry(row):
    out = {"Name": row["name"]}
    if "assign" in row:
        out["Assign"] = [dict(item) for item in row["assign"]]
    else:
        out["Script"] = row.get("script", "")
    _put(out, "Version", row.get("version"), "version" in row)
    _put(out, "Enabled", False, row.get("enabled", True) is False)
    _put(out, "DisplayName", row.get("display_name"), "display_name" in row)
    _put(out, "Note", row.get("note"), "note" in row)
    inputs = inputs_map(row.get("inputs", []))
    _put(out, "Inputs", inputs, bool(inputs))
    return out


# ---------------------------------------------------------------------------
# Script graphs (script assets and scratch pads)
# ---------------------------------------------------------------------------
def _node_entry(row):
    out = {"Id": row["id"], "Action": row["action"], "Position": [row.get("x", 0), row.get("y", 0)]}
    _put(out, "Version", row.get("version"), "version" in row)
    _put(out, "Comment", row.get("comment"), bool(row.get("comment")))
    _put(out, "Properties", row.get("properties"), bool(row.get("properties")))
    _put(out, "Pins", row.get("pins"), "pins" in row)
    return out


def _parameter_entry(row):
    out = {"Name": row["name"], "Type": row["type"]}
    _put(out, "Default", row.get("default"), "default" in row)
    _put(out, "DefaultMode", row.get("default_mode"), "default_mode" in row)
    # A binding the parameter keeps from an earlier Binding mode does nothing in another mode (the Details hide it).
    _put(out, "DefaultBinding", row.get("default_binding"),
         "default_binding" in row and row.get("default_mode") == "Binding")
    _put(out, "StaticSwitchDefault", row.get("static_switch_default"), "static_switch_default" in row)
    _put(out, "Meta", row.get("meta"), bool(row.get("meta")))
    return out


def script_bodies(rows):
    """scope -> script graph body {Usage, Settings, Parameters, Graph}, in row order."""
    bodies = {}
    for row in rows:
        scope = row.get("scope")
        kind = row["kind"]
        if scope is None or kind not in ("script", "parameter", "node", "link", "pin_default"):
            continue
        body = bodies.setdefault(scope, {"Usage": "", "Settings": {}, "Parameters": [],
                                         "Graph": {"Nodes": [], "Links": [], "PinDefaults": {}}})
        if kind == "script":
            body["Usage"] = row.get("usage", "")
            body["Settings"] = dict(row.get("settings", {}))
            _put(body, "Versions", row.get("versions"), "versions" in row)
        elif kind == "parameter":
            body["Parameters"].append(_parameter_entry(row))
        elif kind == "node":
            body["Graph"]["Nodes"].append(_node_entry(row))
        elif kind == "link":
            body["Graph"]["Links"].append({"From": row["from"], "To": row["to"]})
        else:
            body["Graph"]["PinDefaults"][row["pin"]] = row["value"]
    return bodies


def _scratch_pads(bodies, owner):
    """Scratch pads of one owner ("" = system level) as [{Name, ...body}]."""
    pads = []
    for scope, body in bodies.items():
        holder, _, name = scope.rpartition("/")
        if scope and holder == owner:
            pads.append({"Name": name, **body})
    return pads


# ---------------------------------------------------------------------------
# Systems and emitters
# ---------------------------------------------------------------------------
def _emitter_entry(row):
    out = {"Name": row["name"]}
    _put(out, "Enabled", False, row.get("enabled", True) is False)
    if row.get("mode") == "Stateless":
        out["Mode"] = "Stateless"
        out["Stateless"] = dict(row.get("stateless", {}))
        return out
    _put(out, "Parent", row.get("parent"), "parent" in row)
    out["Properties"] = dict(row.get("properties", {}))
    for group in STACK_GROUPS:
        out[group] = []
    out["Renderers"] = []
    return out


def _renderer_entry(row):
    out = {"Name": row["name"], "Class": row["class"]}
    _put(out, "Enabled", False, row.get("enabled", True) is False)
    # Always listed (an apply keeps the Properties of a renderer entry that leaves them out).
    out["Properties"] = dict(row.get("properties", {}))
    return out


def _stage_entry(row):
    out = {"Name": row["name"], "Class": row["class"]}
    _put(out, "Enabled", False, row.get("enabled", True) is False)
    _put(out, "Properties", row.get("properties"), bool(row.get("properties")))
    out["Modules"] = []
    return out


def _user_parameter(row):
    out = {"Name": row["name"], "Type": row["type"]}
    if "value" in row:
        out["Value"] = row["value"]
    elif "data_interface" in row:
        out["Value"] = {"DataInterface": _data_interface(row["data_interface"])}
    elif "object" in row:
        out["Value"] = {"Object": row["object"]}
    _put(out, "Description", row.get("description"), "description" in row)
    _put(out, "Category", row.get("category"), "category" in row)
    return out


def _place_module(row, system, emitters):
    entry = module_entry(row)
    group = row["group"]
    if not row.get("emitter"):
        system.setdefault(group, []).append(entry)
        return
    emitter = emitters[row["emitter"]]
    if group == "EventHandler":
        handlers = emitter.setdefault("EventHandlers", [])
        while len(handlers) <= row["event"]:
            handlers.append({"Properties": {}, "Modules": []})
        handlers[row["event"]]["Modules"].append(entry)
    elif group == "SimulationStage":
        for stage in emitter.get("SimulationStages", []):
            if stage["Name"] == row["stage"]:
                stage["Modules"].append(entry)
    else:
        emitter.setdefault(group, []).append(entry)


def _place_event_handler(row, emitters):
    handlers = emitters[row["emitter"]].setdefault("EventHandlers", [])
    while len(handlers) <= row["index"]:
        handlers.append({"Properties": {}, "Modules": []})
    handlers[row["index"]]["Properties"] = dict(row.get("properties", {}))


def stack_body(rows):
    """(system-level body, {emitter name: emitter entry}) of a system or emitter asset read."""
    system = {"UserParameters": [], "Parameters": [], "SystemSpawn": [], "SystemUpdate": []}
    emitters = {}
    for row in rows:
        kind = row["kind"]
        if kind == "user_parameter":
            system["UserParameters"].append(_user_parameter(row))
        elif kind == "declared_parameter":
            declared = {"Name": row["name"], "Type": row["type"]}
            _put(declared, "Description", row.get("description"), "description" in row)
            system["Parameters"].append(declared)
        elif kind == "emitter":
            emitters[row["name"]] = _emitter_entry(row)
        elif kind == "renderer":
            emitters[row["emitter"]]["Renderers"].append(_renderer_entry(row))
        elif kind == "simulation_stage":
            emitters[row["emitter"]].setdefault("SimulationStages", []).append(_stage_entry(row))
        elif kind == "event_handler":
            _place_event_handler(row, emitters)
    for row in rows:
        if row["kind"] == "module":
            _place_module(row, system, emitters)
    bodies = script_bodies(rows)
    for name, emitter in emitters.items():
        pads = _scratch_pads(bodies, name)
        _put(emitter, "ScratchPads", pads, bool(pads))
    pads = _scratch_pads(bodies, "")
    _put(system, "ScratchPads", pads, bool(pads))
    return system, emitters


def system_body(rows):
    system, emitters = stack_body(rows)
    system["Emitters"] = list(emitters.values())
    return system


def emitter_asset_row(rows):
    """The emitter asset row of an emitter read: {name (the handle name the edit ops use), new, versions?}."""
    return next((row for row in rows if row["kind"] == "emitter_asset"), {"name": "", "new": False})


def emitter_asset_body(rows):
    """An emitter asset's Body: its one emitter entry without Name / Enabled / Mode (design §3.2), with the version
    menu of a versioned emitter (``Versions``; the body describes the exposed version)."""
    _, emitters = stack_body(rows)
    if not emitters:
        return {}
    entry = dict(next(iter(emitters.values())))
    for key in ("Name", "Enabled", "Mode"):
        entry.pop(key, None)
    asset = emitter_asset_row(rows)
    _put(entry, "Versions", asset.get("versions"), "versions" in asset)
    return entry


def script_asset_body(rows):
    return script_bodies(rows).get("", {"Usage": "", "Settings": {}, "Parameters": [],
                                        "Graph": {"Nodes": [], "Links": [], "PinDefaults": {}}})


def collection_body(rows):
    parameters = []
    for row in rows:
        if row["kind"] != "parameter":
            continue
        out = {"Name": row["name"], "Type": row["type"]}
        if "value" in row:
            out["Value"] = row["value"]
        elif "data_interface" in row:
            out["Value"] = {"DataInterface": _data_interface(row["data_interface"])}
        elif "object" in row:
            out["Value"] = {"Object": row["object"]}
        _put(out, "MaterialCollection", True, row.get("material") is True)
        parameters.append(out)
    return {"Parameters": parameters}


# ---------------------------------------------------------------------------
# Inspect facts
# ---------------------------------------------------------------------------
def _input_fact(row):
    out = {"Name": row["name"], "Type": row["type"], "Mode": row.get("mode", "None"),
           "Overridden": bool(row.get("overridden")), "Static": bool(row.get("static")),
           "Visible": row.get("visible", True)}
    _put(out, "InlineEditConditionToggle", True, row.get("inline_edit_condition") is True)
    _put(out, "EditConditionEnabled", row.get("edit_condition"), "edit_condition" in row)
    if row.get("mode") not in (None, "None"):
        out["Value"] = input_value(row)
    return out


# The state modules' lifecycle inputs (SystemState in System Update, EmitterState in Emitter Update).
LIFECYCLE_GROUPS = ("SystemUpdate", "EmitterUpdate")
LOOPING_BEHAVIORS = ("Infinite", "Multiple")


def lifecycle_fact(row):
    """Whether a state module loops, with its Loop* inputs as the stack shows them (default or not): the
    document lists overridden inputs only, so a system that loops by default shows no Loop Behavior there."""
    inputs = {item["name"]: item for item in row.get("inputs", [])
              if type(item.get("name")) is str and item["name"].startswith("Loop ")}
    behavior = inputs.get("Loop Behavior")
    if row.get("group") not in LIFECYCLE_GROUPS or behavior is None or type(behavior.get("value")) is not str:
        return None
    fact = {"Emitter": row.get("emitter", ""), "Module": row["name"], "Loops": behavior["value"] in LOOPING_BEHAVIORS}
    for name in sorted(inputs):
        item = inputs[name]
        if "value" in item:
            fact[name] = item["value"] if item.get("overridden") else {"Value": item["value"], "Default": True}
    return fact


SUMMARY_NOTE = ("NiagaraDetails Modules keep the overridden inputs' values and the other inputs' names (detail: every "
                "input's Type, Mode and Value), Library its counts")


def _module_summary(fact):
    """A module of the summarized inspect: its overridden inputs with their values and the names of the other
    inputs the stack shows (hidden ones left out), instead of every input's full row."""
    out = {key: fact[key] for key in ("Emitter", "Stack", "Module")}
    _put(out, "Inherited", True, fact["Inherited"])
    for key in ("Event", "SimulationStage"):
        _put(out, key, fact.get(key), key in fact)
    inputs = fact["Inputs"]
    overridden = {item["Name"]: item.get("Value", {"Mode": item["Mode"]}) for item in inputs if item["Overridden"]}
    _put(out, "Overridden", overridden, bool(overridden))
    others = [item["Name"] for item in inputs if not item["Overridden"] and item["Visible"]]
    _put(out, "Inputs", others, bool(others))
    return out


def summarize(details):
    """inspect_facts summarized (inspect's HTTP default; detail true returns them whole): per module only the
    overridden inputs with their values and the other visible inputs' names, per group only the library's size."""
    out = dict(details)
    if type(details.get("Modules")) is list:
        out["Modules"] = [_module_summary(fact) for fact in details["Modules"]]
    if type(details.get("Library")) is dict:
        out["Library"] = {group: len(scripts) for group, scripts in details["Library"].items()}
    return out


def inspect_facts(rows, page):
    """Inspect-only facts: every module's inputs (name, type, value mode), each group's module library,
    compile status, the Details properties the reader could not encode and whether the system and its emitters
    loop (Lifecycle)."""
    modules = []
    lifecycle = []
    library = {}
    compile_status = None
    for row in rows:
        kind = row["kind"]
        if kind == "module":
            fact = {"Emitter": row.get("emitter", ""), "Stack": row["group"], "Module": row["name"],
                    "Inherited": bool(row.get("inherited")), "Inputs": [_input_fact(item) for item in row.get("inputs", [])]}
            _put(fact, "Event", row.get("event"), "event" in row)
            _put(fact, "SimulationStage", row.get("stage"), "stage" in row)
            modules.append(fact)
            loop = lifecycle_fact(row)
            if loop is not None:
                lifecycle.append(loop)
        elif kind == "library":
            library[row["group"]] = list(row.get("scripts", []))
        elif kind == "compile":
            compile_status = dict(row["compile"])
    out = {"Modules": modules, "Library": library,
           "UnsupportedProperties": list(page.get("unsupported_properties", []))}
    _put(out, "Lifecycle", lifecycle, bool(lifecycle))
    _put(out, "Compile", compile_status, compile_status is not None)
    return out
