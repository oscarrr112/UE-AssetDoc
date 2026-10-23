# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Registered Control Rig Profile (profile.control_rig.v1, class /Script/ControlRigDeveloper.ControlRigBlueprint).

Extract, Inspect and Diff for the three rig types (Rig, Module, Modular). Apply and Create (see
docs/architecture/asset-document-controlrig-design.md, control_rig_apply.py, control_rig_graph.py,
control_rig_modular.py) author rigs and rig modules: Class Settings, rig type, module settings, variables, the
hierarchy, the event graphs and the local functions (signature, access, header, local variables, bodies), collapse
nodes and function nodes (local and other rigs' public functions); and modular rigs: modules, connections, modular
settings. Top-level ``FactoryContext`` (a Skeletal Mesh or Skeleton) on a new rig is the Content Browser's "Create
Control Rig" on that asset (its bones and curves imported, the preview mesh and the import sources set); with it the
Hierarchy need not list the imported bones and curves.

Document (MCP/schemas/assetdocument/ControlRig.md)::

    "Body": {
      "RigType": "Module",                                   # omitted = "Rig"; "Modular" for modular rigs
      "Module": {"Category": "Limbs"},                       # rig module settings (RigType Module)
      "Variables": [{"Name": "AimWeight", "Type": "float", "Default": 1.0, "Public": true}],
      "Hierarchy": [{"Type": "Bone", "Name": "root", "Imported": true},
                    {"Type": "Control", "Name": "root_ctrl", "Parent": "Bone(root)",
                     "Settings": {"ControlType": "EulerTransform", "ShapeName": "Circle_Thick"}}],
      "Graphs": {"Rig": {"Nodes": {"BeginExecution": {"Unit": "RigUnit_BeginExecution", "Position": [0, 0]}}}},
      "Functions": {"AlignChain": {"Public": true, "Inputs": [...], "Outputs": [...], "LocalVariables": [...],
                                   "Nodes": {...}, "Order": [...]}},
      "Modules": [...], "Connections": {...}, "ModularSettings": {}   # modular rigs
    }

* Regions are complete state when present. Graph nodes are keyed by the model's node names; elements are an
  ordered list identified by Type + Name ("Bone(spine_01)" in references); modules by Name.
* Extract reads the rig as stored after load (the editor's view); nothing compiles. Imported bones without
  ``Transform`` have the reference pose of ``SourceHierarchyImport``.
* ``RenamedFrom`` on a variable, element, graph, node, function, exposed pin or local variable renames it (an
  instruction, not compared).
* Node ``Position``, ``Size``, ``Title``, ``Color``, ``Expanded`` and ``PinTypes`` omitted keep the rig's; so do a
  function's ``Color`` and a function / collapse graph's ``Entry`` and ``Return``.
"""
from __future__ import annotations

import copy

from dataclasses import replace

from asset_document import inspect_summary
from asset_document.diff.model import Difference, DiffKind
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, IdentityPolicy, OmissionPolicy, OrderPolicy, ProfileMeta,
    RegionMeta, RegionOwnership,
)
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document.profiles import data_table_common as common
from asset_document.profiles import control_rig_modular as modular
from asset_document.profiles import ik_common as ik
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument, _single_handle
from asset_document.regions.target_lifecycle import generic_factory_lifecycle

PROFILE_ID = "profile.control_rig.v1"
CLASS = ik.CONTROLRIG_CLASS
PREFIX = "ControlRig"
READ = ik.CONTROLRIG_READ
RIG_TYPES = ("Rig", "Module", "Modular")
RIG_PARENT = "/Script/ControlRig.ControlRig"
MODULAR_PARENT = "/Script/ControlRig.ModularRig"
BODY_KEYS = ("RigType", "ParentClass", "Module", "Variables", "Hierarchy", "Graphs", "GraphOrder", "Functions", "Modules",
             "Connections", "ModularSettings")
READ_ONLY_SECTIONS = ()
CLASS_CLASS = "/Script/CoreUObject.Class"
# The factory's ParentClass (its class picker) is the optional create context: ModularRig for a modular rig, or a
# ParentClass the document names.
LIFECYCLE = replace(generic_factory_lifecycle(ik.ROUTE, ("factory_context",)),
                    optional_create_context_handle_names=("factory_context",))
# Regions per rig type (design §3.2): a modular rig has no graphs or hierarchy of its own; modules / connections
# belong to modular rigs only.
GRAPH_REGIONS = ("Hierarchy", "Graphs", "GraphOrder", "Functions")
MODULAR_REGIONS = ("Modules", "Connections", "ModularSettings")
LIST_REGIONS = ("Variables", "Hierarchy", "GraphOrder", "Modules")
MAP_REGIONS = ("Graphs", "Functions", "Connections", "ModularSettings", "Module")

VARIABLE_DEFAULTS = {"Public": False, "ReadOnly": False, "ExposeToCinematics": False, "Category": "", "Tooltip": ""}
NODE_DEFAULTS = {"Title": "", "Defaults": {}, "Inputs": {}, "Bindings": {}, "Expanded": [], "Traits": {}, "Injections": {},
                 "VariableMap": {}, "Setter": False, "Method": "Execute", "FontSize": 18, "Bubble": False,
                 "ColorBubble": False}
# Node fields an authored node may omit to keep the rig's (layout and editor-derived state).
NODE_KEEP = ("Position", "Size", "Title", "Color", "Expanded", "PinTypes")
FUNCTION_DEFAULTS = {"Public": False, "Category": "", "Keywords": "", "Description": "", "LocalVariables": [],
                     "Inputs": [], "Outputs": [], "Nodes": {}}
COLLAPSE_DEFAULTS = {"Category": "", "Keywords": "", "Description": "", "Inputs": [], "Outputs": [], "LocalVariables": []}
# Function / collapse graph fields an authored one may omit to keep the rig's.
CONTAINER_KEEP = ("Color", "Entry", "Return")
MODULE_DEFAULTS = modular.MODULE_DEFAULTS

# Body-owned (design §3.2) and derived (§2.1) properties of the blueprint: never generic Properties.
BODY_OWNED = {"RigVMClient": "Body/Graphs and Body/Functions", "Hierarchy": "Body/Hierarchy",
              "ModularRigModel": "Body/Modules and Body/Connections", "ModularRigSettings": "Body/ModularSettings",
              "RigModuleSettings": "Body/Module", "NewVariables": "Body/Variables", "ParentClass": "Body/ParentClass"}
NOT_AUTHORED = ("UbergraphPages", "FunctionGraphs", "GeneratedClass", "ControlRigType", "ItemTypeDisplayName",
                "bExposesAnimatableControls", "ModuleReferenceData", "CustomThumbnail", "ArrayConnectionMap",
                "bSupportsInversion", "bSupportsControls", "Validator", "DebugBoneRadius", "Influences",
                "PublicGraphFunctions", "FunctionReferenceNodeData", "SupportedEventNames", "ReferencedObjectPathsStored",
                "UserDefinedStructGuidToPathName", "UserDefinedEnumToPathName", "ModulesRecompilationBracket",
                "ThumbnailInfo")


def error(path, message, code):
    return common.error(path, message, code)


escape = common.escape


def _properties():
    return ik.RigProperties(CLASS, BODY_OWNED, NOT_AUTHORED, PREFIX)


# ---------------------------------------------------------------------------
# Document shape
# ---------------------------------------------------------------------------
def validate_body(body):
    path = "/Body"
    rig_type = body.get("RigType", "Rig")
    if rig_type not in RIG_TYPES:
        raise error(f"{path}/RigType", f"RigType must be one of {', '.join(RIG_TYPES)}.", "ControlRig.InvalidBody")
    for key in LIST_REGIONS:
        if key in body and type(body[key]) is not list:
            raise error(f"{path}/{key}", f"{key} is an ordered array.", "ControlRig.InvalidBody")
    for key in MAP_REGIONS:
        if key in body and type(body[key]) is not dict:
            raise error(f"{path}/{key}", f"{key} is an object.", "ControlRig.InvalidBody")
    if "ParentClass" in body and (type(body["ParentClass"]) is not str or not body["ParentClass"].startswith("/")):
        raise error(f"{path}/ParentClass", "ParentClass is a class path (/Script/...).", "ControlRig.InvalidBody")
    if "RigType" in body:
        # Omitted, the rig keeps its type (an Update): control_rig_apply.plan checks the regions against the rig's.
        check_regions(body, rig_type)
    for index, variable in enumerate(body.get("Variables", [])):
        where = f"{path}/Variables/{index}"
        if type(variable) is not dict or type(variable.get("Name")) is not str or type(variable.get("Type")) is not str:
            raise error(where, "A variable is {Name, Type, Default?, ...}.", "ControlRig.InvalidBody")
        _check_renamed(variable, where)
    for index, element in enumerate(body.get("Hierarchy", [])):
        where = f"{path}/Hierarchy/{index}"
        if type(element) is not dict or type(element.get("Type")) is not str or type(element.get("Name")) is not str:
            raise error(where, "An element is {Type, Name, ...}.", "ControlRig.InvalidBody")
        _check_renamed(element, where)
    modular.validate(body)
    order = body.get("GraphOrder", [])
    if "GraphOrder" in body and "Graphs" not in body:
        raise error(f"{path}/GraphOrder", "GraphOrder orders the graphs Graphs lists: list Graphs with it, or leave both "
                    "out to keep the rig's graphs.", "ControlRig.InvalidBody")
    if type(order) is not list or any(type(name) is not str for name in order) or len(set(order)) != len(order):
        raise error(f"{path}/GraphOrder", "GraphOrder is a list of graph names, each once.", "ControlRig.InvalidBody")
    for index, name in enumerate(order):
        if "Graphs" in body and name not in body["Graphs"]:
            raise error(f"{path}/GraphOrder/{index}", f"GraphOrder lists '{name}', which is not in Graphs."
                        f"{ik.describe_candidates(name, sorted(body['Graphs']), 'Graphs')}", "ControlRig.GraphNotFound")
    for name, graph in body.get("Graphs", {}).items():
        _validate_container(graph, f"{path}/Graphs/{escape(name)}", "A graph")
    for name, function in body.get("Functions", {}).items():
        _validate_container(function, f"{path}/Functions/{escape(name)}", "A function")


def check_regions(body, rig_type):
    """Regions of another rig type: graphs and hierarchy are a modular rig's modules', modules and connections belong
    to modular rigs, Module settings to rig modules."""
    if rig_type == "Modular":
        for key in GRAPH_REGIONS:
            if key in body:
                raise error(f"/Body/{key}", f"A modular rig has no {key} of its own (its modules build them).",
                            "ControlRig.ModularRigRegionUnsupported")
    else:
        for key in MODULAR_REGIONS:
            if key in body:
                raise error(f"/Body/{key}", f"{key} belongs to modular rigs (RigType Modular); this rig is a {rig_type} "
                            "rig.", "ControlRig.ModularRegionUnsupported")
    if "Module" in body and rig_type != "Module":
        raise error("/Body/Module", "Module settings belong to rig modules (RigType Module).", "ControlRig.InvalidBody")


def _check_renamed(entry, path):
    if "RenamedFrom" in entry and (type(entry["RenamedFrom"]) is not str or not entry["RenamedFrom"]):
        raise error(f"{path}/RenamedFrom", "RenamedFrom is the entry's current name.", "ControlRig.InvalidBody")


def _validate_container(container, path, what):
    if type(container) is not dict:
        raise error(path, f"{what} is an object with Nodes.", "ControlRig.InvalidBody")
    nodes = container.get("Nodes", {})
    if type(nodes) is not dict:
        raise error(f"{path}/Nodes", "Nodes is an object keyed by node name.", "ControlRig.InvalidBody")
    for name, node in nodes.items():
        where = f"{path}/Nodes/{escape(name)}"
        if type(node) is not dict:
            raise error(where, "A node is an object.", "ControlRig.InvalidBody")
        if type(node.get("Collapse")) is dict:
            _validate_container(node["Collapse"], f"{where}/Collapse", "A collapsed graph")


def canonicalize(document):
    body = common.validate_header(document, (CLASS,), PREFIX, BODY_KEYS, ("FactoryContext",))
    if "FactoryContext" in document:
        value = document["FactoryContext"]
        # An object path or, as the editor's asset picker takes it, a package path (the package's mesh or skeleton,
        # resolved by the hierarchy.import op).
        if type(value) is not str or not value.startswith("/") or value.endswith(("/", ".")) or "//" in value:
            raise error("/FactoryContext", "FactoryContext is the path of the Skeletal Mesh or Skeleton the rig is made for "
                        "(/Game/Characters/SKM_Hero or /Game/Characters/SKM_Hero.SKM_Hero).",
                        "ControlRig.InvalidFactoryContext")
    validate_body(body)
    canonical = common.snapshot(document)
    canonical.setdefault("Definitions", {})
    canonical.setdefault("Properties", {})
    canonical.setdefault("Body", {})
    if type(canonical["Body"].get("Connections")) is dict:
        canonical["Body"]["Connections"] = modular.normalize_connections(canonical["Body"]["Connections"])
    return canonical


# ---------------------------------------------------------------------------
# Read records -> document
# ---------------------------------------------------------------------------
def read_records(calls, detail="document"):
    """Every record of rig.controlrig.read.v1 (header first). Pages after the first reuse the first page's records
    through its token."""
    records = []
    offset = 0
    token = None
    while True:
        values = {"offset": offset}
        if detail != "document":
            values["detail"] = detail
        if token:
            values["token"] = token
        page = calls.query(READ, values)
        records += page.get("records", [])
        token = page.get("token") or token
        offset = page.get("next_offset", -1)
        if type(offset) is not int or offset < 0:
            break
    if not records or records[0].get("kind") != "header":
        raise error("/Body", f"{READ} returned no header.", "ControlRig.InvalidPrimitiveResult")
    return records


def _container(body, scope):
    """The node container a record's scope names: ["Graphs", g] or ["Functions", f], then ["Nodes", n, "Collapse"]*."""
    container = body.setdefault(scope[0], {}).setdefault(scope[1], {"Nodes": {}})
    rest = list(scope[2:])
    while len(rest) >= 3:
        container = container["Nodes"][rest[1]][rest[2]]
        rest = rest[3:]
    return container


def _node_path(scope, name):
    return "/Body/" + "/".join(escape(item) for item in scope) + f"/Nodes/{escape(name)}"


def assemble(records):
    """Body and Inspect facts of rig.controlrig.read.v1 records."""
    header = records[0]
    rig_type = header.get("rig_type", "Rig")
    body = {}
    facts = {"header": header, "pins": {}, "globals": {}, "references": {}, "pin_types": []}
    if rig_type != "Rig":
        body["RigType"] = rig_type
    parent = header.get("parent_class", "")
    if parent and parent != (MODULAR_PARENT if rig_type == "Modular" else RIG_PARENT):
        body["ParentClass"] = parent
    if rig_type == "Module":
        body["Module"] = header.get("module", {})
    body["Variables"] = []
    if rig_type == "Modular":
        body["Modules"] = []
        body["Connections"] = {}
        body["ModularSettings"] = header.get("modular_settings", {})
    else:
        body["Hierarchy"] = []
        body["Graphs"] = {}
        body["Functions"] = {}
    for record in records[1:]:
        kind = record.get("kind")
        if kind == "variable":
            body["Variables"].append(record["entry"])
        elif kind == "element":
            entry = record["entry"]
            body["Hierarchy"].append(entry)
            if "global" in record:
                facts["globals"][f"{entry['Type']}({entry['Name']})"] = record["global"]
            if "reference" in record:
                facts["references"][f"{entry['Type']}({entry['Name']})"] = record["reference"]
        elif kind == "graph":
            body["Graphs"][record["name"]] = {"Nodes": {}}
        elif kind == "function":
            body["Functions"][record["name"]] = {**record["entry"], "Nodes": {}}
        elif kind == "node":
            entry = record["entry"]
            if type(entry.get("Collapse")) is dict:
                entry["Collapse"].setdefault("Nodes", {})
            _container(body, record["scope"]).setdefault("Nodes", {})[record["name"]] = entry
            if "pins" in record:
                facts["pins"][_node_path(record["scope"], record["name"])] = record["pins"]
            if type(record.get("pin_types")) is dict:
                facts["pin_types"].append((tuple(record["scope"]), record["name"], record["pin_types"]))
        elif kind == "interface":
            _container(body, record["scope"])[record["name"]] = record["entry"]
        elif kind == "module":
            body["Modules"].append(record["entry"])
        elif kind == "connections":
            body["Connections"] = record.get("entries", {})
    for section in ("Graphs", "Functions"):
        for container in body.get(section, {}).values():
            _add_order(container)
    if len(body.get("Graphs", {})) > 1:
        # The model order (the canonical form sorts the Graphs keys): the compiler visits the graphs' events in it.
        body["GraphOrder"] = list(body["Graphs"])
    return body, facts


def _add_order(container):
    """The model's node order of a graph and its collapse graphs (records come in it; the canonical form sorts the
    Nodes keys). The interface nodes are not listed."""
    nodes = container.get("Nodes", {})
    if len(nodes) > 1:
        container["Order"] = list(nodes)
    for entry in nodes.values():
        if type(entry.get("Collapse")) is dict:
            _add_order(entry["Collapse"])


def read_body(calls, detail="document"):
    return assemble(read_records(calls, detail))


def with_full_types(body, pin_types):
    """The body with each template node's PinTypes complete (the read writes only the pins that decide the others):
    what Apply and Diff compare an authored pin type with."""
    for scope, name, types in pin_types:
        try:
            entry = _container(body, scope)["Nodes"][name]
        except (KeyError, TypeError):
            continue
        if type(entry) is dict:
            entry["PinTypes"] = dict(types)
    return body


def with_references(authored_body, live_body, references):
    """The live Hierarchy with the reference-pose Transform of an imported bone the read leaves out, where the
    authored element writes one (an authored Transform equal to the reference pose matches the omission)."""
    if not references or type(authored_body.get("Hierarchy")) is not list or type(live_body.get("Hierarchy")) is not list:
        return live_body
    wanted = {_element_key(entry) for entry in authored_body["Hierarchy"] if type(entry) is dict and "Transform" in entry}
    hierarchy = []
    for entry in live_body["Hierarchy"]:
        key = _element_key(entry)
        if key in wanted and "Transform" not in entry and key in references:
            entry = {**entry, "Transform": references[key]}
        hierarchy.append(entry)
    return {**live_body, "Hierarchy": hierarchy}


# Class Settings import sources. Duplicate-transient, so the generic Properties region does not admit them; the
# profile reads them from rig.controlrig.read.v1's header and writes them with rig.controlrig.edit.v1 source_import.
# Imported bones without Transform are at the reference pose of SourceHierarchyImport, so it
# belongs to the document.
SOURCE_IMPORTS = {"SourceHierarchyImport": "source_hierarchy_import", "SourceCurveImport": "source_curve_import"}


def extract(target, read, context, include_defaults=True):
    return extract_with_defaults(target, read, context, include_defaults)[0]


def extract_with_defaults(target, read, context, include_defaults=True):
    """The document and what its sparse read leaves out: the reference pose of imported bones without Transform and
    the complete pin types of template nodes (Diff compares authored values with them)."""
    body, facts = read_body(ik.RigCalls(context, read, prefix=PREFIX))
    properties = dict(_properties().extract(read, context, include_defaults))
    for name, field in SOURCE_IMPORTS.items():
        path = facts["header"].get(field, "")
        if type(path) is str and path and path != "None":
            properties[name] = path
    defaults = {"references": facts["references"], "pin_types": facts["pin_types"]}
    return common.document(target, CLASS, properties, body), defaults


# ---------------------------------------------------------------------------
# Diff
# ---------------------------------------------------------------------------
def _keyed(items, key):
    return {key(item): item for item in items}


def _element_key(element):
    return f"{element.get('Type', '')}({element.get('Name', '')})"


def _first_parent(element):
    if type(element.get("Parent")) is str:
        return element["Parent"]
    parents = element.get("Parents")
    return parents[0].get("Parent", "") if type(parents) is list and parents and type(parents[0]) is dict else ""


def renamed(path, mine, theirs, changes, old_key=None):
    """``theirs`` (live entries by identity, in order) with each entry ``mine`` renames (``RenamedFrom`` naming a live
    entry the document no longer lists) under its new identity, and the rename as a change of ``<new>/Name`` (asset:
    the old name, document: the new one), so a rename reads as one, not as a remove and an add."""
    renames = {}
    for name, entry in mine.items():
        old = entry.get("RenamedFrom") if type(entry) is dict else None
        if type(old) is not str or not old:
            continue
        old = old_key(entry, old) if old_key is not None else old
        if old != name and name not in theirs and old in theirs and old not in mine and old not in renames:
            renames[old] = name
            if "Name" not in entry:
                # (List entries carry their Name, which the entry comparison reports.)
                changes.append(Difference(f"{path}/{escape(name)}/Name", DiffKind.REPLACE, True, name, True, old))
    if not renames:
        return theirs
    return {renames.get(name, name): entry for name, entry in theirs.items()}


def _list_diff(path, authored, live, key, defaults, exact, changes, group=None, old_key=None):
    """An ordered, identified list (complete state): entries by identity, then the order of the common ones (within
    each ``group`` when given: the hierarchy lists elements depth first, so its order is the order of siblings)."""
    mine, theirs = _keyed(authored, key), _keyed(live, key)
    theirs = renamed(path, mine, theirs, changes, old_key)
    for name in list(mine) + [name for name in theirs if name not in mine]:
        where = f"{path}/{escape(name)}"
        if name not in theirs:
            changes.append(Difference(where, DiffKind.ADD, True, mine[name], False, None))
        elif name not in mine:
            changes.append(Difference(where, DiffKind.REMOVE, False, None, True, theirs[name]))
        else:
            entry = {item: value for item, value in mine[name].items() if item != "RenamedFrom"}
            ik.value_diff(where, entry, theirs[name], exact, changes, defaults)
    def grouped(entries):
        groups = {}
        for name, entry in entries.items():
            if name in mine and name in theirs:
                groups.setdefault(group(entry) if group is not None else "", []).append(name)
        return groups
    order_mine, order_theirs = grouped(mine), grouped(theirs)
    for parent in order_mine:
        if order_mine[parent] != order_theirs.get(parent):
            changes.append(Difference(path, DiffKind.REPLACE, True, [name for names in order_mine.values() for name in names],
                                      True, [name for names in order_theirs.values() for name in names]))
            break


def _nodes_diff(path, authored, live, exact, changes):
    live = renamed(path, authored, live, changes)
    for name in list(authored) + [name for name in live if name not in authored]:
        where = f"{path}/{escape(name)}"
        if name not in live:
            changes.append(Difference(where, DiffKind.ADD, True, authored[name], False, None))
        elif name not in authored:
            changes.append(Difference(where, DiffKind.REMOVE, False, None, True, live[name]))
        else:
            _node_diff(where, authored[name], live[name], exact, changes)
            if type(authored[name].get("Collapse")) is dict and type(live[name].get("Collapse")) is dict:
                _container_diff(f"{where}/Collapse", authored[name]["Collapse"], live[name]["Collapse"],
                                COLLAPSE_DEFAULTS, exact, changes)
            elif "Collapse" in authored[name] or "Collapse" in live[name]:
                ik.value_diff(f"{where}/Collapse", authored[name].get("Collapse"), live[name].get("Collapse"), exact,
                              changes)


def injections_as_authored(authored, live):
    """The live injected units of each pin as the document writes them: an injection that omits its Name (a new
    one the editor names) or its Method (Execute) is compared without it."""
    result = {}
    for pin, units in live.items():
        mine = authored.get(pin)
        if type(units) is not list or type(mine) is not list:
            result[pin] = units
            continue
        shown = []
        for index, unit in enumerate(units):
            spec = mine[index] if index < len(mine) else None
            if type(unit) is dict and type(spec) is dict:
                unit = {key: value for key, value in unit.items() if key in spec or key not in ("Name", "Method")}
            shown.append(unit)
        result[pin] = shown
    return result


def _node_diff(where, authored, live, exact, changes):
    """A node entry (or an Entry / Return node): fields NODE_KEEP lists omitted keep the rig's."""
    mine = {key: value for key, value in authored.items() if key not in ("RenamedFrom", "Collapse")}
    theirs = {key: value for key, value in live.items() if key != "Collapse" and (key in mine or key not in NODE_KEEP)}
    # PinTypes lists the pins that decide the others: the authored pins compare with the node's types of them.
    if type(mine.get("PinTypes")) is dict and type(theirs.get("PinTypes")) is dict:
        theirs["PinTypes"] = {pin: value for pin, value in theirs["PinTypes"].items() if pin in mine["PinTypes"]}
    # Expanded is a set of pins (read in pin order, which a node made today may list in another order).
    for side in (mine, theirs):
        if type(side.get("Expanded")) is list and all(type(pin) is str for pin in side["Expanded"]):
            side["Expanded"] = sorted(side["Expanded"])
    theirs_injections = theirs.get("Injections")
    if type(mine.get("Injections")) is dict and type(theirs_injections) is dict:
        theirs["Injections"] = injections_as_authored(mine["Injections"], theirs_injections)
    ik.value_diff(where, mine, theirs, exact, changes, NODE_DEFAULTS)


def _without_renames(items):
    return [{key: value for key, value in item.items() if key != "RenamedFrom"} if type(item) is dict else item
            for item in items] if type(items) is list else items


def _order_diff(path, authored, live, changes):
    """A graph's Order against the rig's: the listed nodes in the listed order, among the rig's nodes of those names
    (not compared by diff: see order_kept)."""
    if type(authored.get("Order")) is not list:
        return
    nodes = authored.get("Nodes", {})
    mine = [name for name in authored["Order"] if name in nodes]
    listed = set(mine)
    live_nodes = live.get("Nodes", {})
    # Renamed nodes keep their place.
    renames = {entry["RenamedFrom"]: name for name, entry in nodes.items()
               if type(entry) is dict and type(entry.get("RenamedFrom")) is str and name not in live_nodes}
    theirs = [renames.get(name, name) for name in (live.get("Order") or list(live_nodes))]
    theirs = [name for name in theirs if name in listed]
    if mine != theirs:
        changes.append(Difference(f"{path}/Order", DiffKind.REPLACE, True, mine, True, theirs))


def _container_diff(path, authored, live, defaults, exact, changes):
    skip = ("Nodes", "RenamedFrom", "Order", "Entry", "Return")
    mine = {key: _without_renames(value) if key in ("Inputs", "Outputs", "LocalVariables") else value
            for key, value in authored.items() if key not in skip}
    theirs = {key: value for key, value in live.items() if key not in skip and (key in mine or key not in CONTAINER_KEEP)}
    ik.value_diff(path, mine, theirs, exact, changes, defaults)
    for name in ("Entry", "Return"):
        if type(authored.get(name)) is dict:
            _node_diff(f"{path}/{name}", authored[name], live.get(name) or {}, exact, changes)
    _nodes_diff(f"{path}/Nodes", authored.get("Nodes", {}), live.get("Nodes", {}), exact, changes)


def _containers_diff(path, authored, live, defaults, exact, changes):
    live = renamed(path, authored, live, changes)
    for name in list(authored) + [name for name in live if name not in authored]:
        where = f"{path}/{escape(name)}"
        if name not in live:
            changes.append(Difference(where, DiffKind.ADD, True, authored[name], False, None))
        elif name not in authored:
            changes.append(Difference(where, DiffKind.REMOVE, False, None, True, live[name]))
        else:
            _container_diff(where, authored[name], live[name], defaults, exact, changes)


def body_diff(authored, live, exact):
    """Differences of the authored Body regions (omitted region = keep) from the live Body."""
    changes = []
    path = "/Body"
    for key, default in (("RigType", "Rig"), ("ParentClass", None)):
        if key in authored:
            fallback = default if default is not None else (
                MODULAR_PARENT if live.get("RigType") == "Modular" else RIG_PARENT)
            ik.value_diff(f"{path}/{key}", authored[key], live.get(key, fallback), exact, changes)
    for key in ("Module", "Connections"):
        if key in authored:
            ik.value_diff(f"{path}/{key}", authored[key], live.get(key, {}), exact, changes)
    if "ModularSettings" in authored:
        ik.value_diff(f"{path}/ModularSettings", authored["ModularSettings"], live.get("ModularSettings", {}), exact,
                      changes, modular.SETTINGS_DEFAULTS)
    if "Variables" in authored:
        _list_diff(f"{path}/Variables", authored["Variables"], live.get("Variables", []), lambda item: item.get("Name", ""),
                   VARIABLE_DEFAULTS, exact, changes)
    if "Hierarchy" in authored:
        _list_diff(f"{path}/Hierarchy", authored["Hierarchy"], live.get("Hierarchy", []), _element_key, {}, exact, changes,
                   group=_first_parent, old_key=lambda entry, old: f"{entry.get('Type', '')}({old})")
    if "Modules" in authored:
        _list_diff(f"{path}/Modules", authored["Modules"], live.get("Modules", []), lambda item: item.get("Name", ""),
                   MODULE_DEFAULTS, exact, changes, group=lambda item: item.get("Parent", ""))
    if "Graphs" in authored:
        _containers_diff(f"{path}/Graphs", authored["Graphs"], live.get("Graphs", {}), {}, exact, changes)
    if "Functions" in authored:
        _containers_diff(f"{path}/Functions", authored["Functions"], live.get("Functions", {}), FUNCTION_DEFAULTS,
                         exact, changes)
    return changes


def _containers_order(path, authored, live, changes):
    for name, container in authored.items():
        if type(container) is dict and type(live.get(name)) is dict:
            _container_order(f"{path}/{escape(name)}", container, live[name], changes)


def _container_order(path, authored, live, changes):
    _order_diff(path, authored, live, changes)
    live_nodes = live.get("Nodes", {})
    for name, entry in authored.get("Nodes", {}).items():
        theirs = live_nodes.get(name)
        if type(entry) is dict and type(entry.get("Collapse")) is dict and type(theirs) is dict \
                and type(theirs.get("Collapse")) is dict:
            _container_order(f"{path}/Nodes/{escape(name)}/Collapse", entry["Collapse"], theirs["Collapse"], changes)


def order_kept(authored, live):
    """Where the Body's Order (per graph, function and collapse graph) or GraphOrder lists nodes / graphs the rig has
    in another order than the rig has them. Order and GraphOrder are not compared (diff, verification): the editor has
    no command that reorders nodes or models, so existing ones keep their place and new ones are added last, in the
    document's order. Apply reports these as ControlRig.OrderKept warnings."""
    changes = []
    if type(authored.get("Graphs")) is dict:
        _containers_order("/Body/Graphs", authored["Graphs"], live.get("Graphs", {}), changes)
    if type(authored.get("GraphOrder")) is list:
        mine = list(authored["GraphOrder"])
        listed = set(mine)
        theirs = [name for name in (live.get("GraphOrder") or list(live.get("Graphs", {}))) if name in listed]
        if mine != theirs:
            changes.append(Difference("/Body/GraphOrder", DiffKind.REPLACE, True, mine, True, theirs))
    if type(authored.get("Functions")) is dict:
        _containers_order("/Body/Functions", authored["Functions"], live.get("Functions", {}), changes)
    return changes


def imported_kept(entry):
    """A mesh element a document with FactoryContext need not list: an imported bone or a curve."""
    return entry.get("Type") == "Curve" or (entry.get("Type") == "Bone" and entry.get("Imported") is True)


def without_unlisted_imports(authored_body, live_body):
    """The live Body as a FactoryContext document states it: imported bones and curves its Hierarchy does not list
    are the mesh's (kept, not compared)."""
    if type(authored_body.get("Hierarchy")) is not list or type(live_body.get("Hierarchy")) is not list:
        return live_body
    listed = {_element_key(entry) for entry in authored_body["Hierarchy"]}
    return {**live_body, "Hierarchy": [entry for entry in live_body["Hierarchy"]
                                       if _element_key(entry) in listed or not imported_kept(entry)]}


def diff(authored, extracted, exact, live_defaults=None):
    changes = common.header_diff(authored, extracted)
    live_body = extracted.get("Body", {})
    if live_defaults:
        live_body = with_references(authored.get("Body", {}), live_body, live_defaults.get("references"))
        live_body = with_full_types(copy.deepcopy(live_body), live_defaults.get("pin_types", ()))
    if authored.get("FactoryContext"):
        live_body = without_unlisted_imports(authored.get("Body", {}), live_body)
    changes.extend(body_diff(authored.get("Body", {}), live_body, exact))
    changes.extend(common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact))
    return tuple(changes)


# ---------------------------------------------------------------------------
# Inspect
# ---------------------------------------------------------------------------
def inspect_payload(document, calls):
    _, facts = read_body(calls, "inspect")
    header = facts["header"]
    found = header.get("facts", {})
    payload = {**document, "Counts": header.get("counts", {}), "Pins": facts["pins"],
               "GlobalTransforms": facts["globals"], "Shapes": found.get("shapes", []),
               "PublicFunctions": found.get("public_functions", []), "Events": found.get("events", [])}
    if header.get("skipped"):
        payload["Skipped"] = header["skipped"]
    if "modular" in found:
        modular_facts = found["modular"]
        # Suggestions only (never applied implicitly): copy AutoConnect into Body.Connections to accept it. Candidates
        # are the targets left once AutoConnect is accepted.
        payload["ModularRigSuggestions"] = {"AutoConnect": modular_facts.get("auto_connect", {}),
                                            "Candidates": modular_facts.get("candidates", {}),
                                            "ConnectPrimaryFirst": modular_facts.get("unresolved_primary", [])}
        if modular_facts.get("check"):
            # AutoConnect bones above where their module attaches (the editor picks them too): check before accepting.
            payload["ModularRigSuggestions"]["Check"] = modular_facts["check"]
        if modular_facts.get("error"):
            payload["ModularRigSuggestions"]["Error"] = modular_facts["error"]
        # What the modules build (the asset's own hierarchy is only the mesh's): the elements in a module's namespace,
        # as the constructed rig has them, with their initial global transforms.
        if "elements" in modular_facts:
            payload["GlobalTransforms"] = {**payload["GlobalTransforms"], **modular_facts["elements"]}
            payload["Counts"] = {**payload["Counts"], "module_elements": len(modular_facts["elements"]),
                                 "constructed_elements": modular_facts.get("element_count", 0)}
    return payload


def inspect_class(context):
    """Class-path Inspect (no asset): what a new rig is made of before any rig exists: the Class Settings with their
    defaults (the Properties a document may write), the Body sections, the rig types and how to create each."""
    region = _properties()
    skipped = region.skipped()
    settings = {}
    for item in region.adapter.enumerate(CLASS, context):
        if item.name in skipped or item.name in NOT_AUTHORED or item.name.endswith("_DEPRECATED") or not item.writable:
            continue
        settings[item.name] = item.default_value.value if item.default_value.is_set else None
    return {"Class": CLASS, "ClassSettings": dict(sorted(settings.items())),
            "ImportSources": list(SOURCE_IMPORTS), "BodySections": list(BODY_KEYS), "RigTypes": list(RIG_TYPES),
            "Create": {"Rig": "Action Create; Body.RigType omitted",
                       "Module": "Body.RigType \"Module\" (Turn into Rig Module on the new rig)",
                       "Modular": "Body.RigType \"Modular\" (the factory with ParentClass ModularRig adds module Root)",
                       "FactoryContext": "top-level object path of a Skeletal Mesh or Skeleton: its bones and curves are "
                                         "imported as Create Control Rig on that mesh does (preview mesh and import sources "
                                         "set); the Hierarchy then lists only what the rig adds"}}


# A create's document (create_target_context runs for new rigs only, in the same call as apply).
_CREATING = {}


def take_creating(document):
    return _CREATING.pop(id(document), None) is document


def create_target_context(document, context):
    """The factory's ParentClass (its class picker): ModularRig for RigType Modular, else the document's ParentClass;
    none for a plain rig (the factory's default, ControlRig)."""
    canonical = canonicalize(document)
    _CREATING[id(document)] = document
    body = canonical["Body"]
    parent = body.get("ParentClass") or (MODULAR_PARENT if body.get("RigType") == "Modular" else None)
    if parent is None or parent == RIG_PARENT:
        return ()
    where = "/Body/ParentClass" if "ParentClass" in body else "/Body/RigType"
    outcome = common.invoke(context, ik.BY_ID, common.REFERENCE_RESOLVE, {"expected_class": CLASS_CLASS, "path": parent},
                            path=where, prefix=PREFIX)
    return (NamedOpaqueHandleArgument("factory_context", _single_handle(outcome, "asset")),)


# ---------------------------------------------------------------------------
# Handler
# ---------------------------------------------------------------------------
class ControlRigRuntimeProfile:
    runtime_entry_module = "asset_document/bootstrap.py"
    preflight_scratch = False
    exact_asset_class_path = CLASS
    profile_id = PROFILE_ID

    def required_primitives(self):
        return ik.REQUIREMENTS

    def inspect_profile(self):
        return {"Class": CLASS, "BodySections": list(BODY_KEYS), "ReadOnlySections": list(READ_ONLY_SECTIONS)}

    def create_template(self, target):
        return common.document(common.require_target(target, PREFIX), CLASS, {}, {})

    def validate(self, document):
        canonicalize(document)

    def canonicalize(self, document):
        return canonicalize(document)

    def create_target_context(self, document, context):
        return create_target_context(document, context)

    def preflight(self, document, scratch_handle, context):
        canonical = canonicalize(document)
        properties = {name: value for name, value in canonical.get("Properties", {}).items()
                      if name != _apply.PREVIEW_MESH and name not in SOURCE_IMPORTS}
        _properties().validate(properties, context)
        _apply.preflight(canonical, ik.existing(context, canonical, CLASS, PREFIX), context)
        return SNAPSHOT_COVERED

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        creating = take_creating(document)
        return _apply.apply(canonicalize(document), read_asset_handle, mutable_asset_handle, context, creating)

    def repair(self, document, asset_handle, context):
        canonicalize(document)
        return ()

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        return extract(common.require_target(target, PREFIX), asset_handle, context, include_defaults)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode == "class_defaults" and exact_class_path == CLASS:
            return inspect_class(context)
        if mode != "live_asset" or exact_class_path != CLASS or editor_asset_handle is None:
            raise error("/Inspect", "Control Rig inspection reads a live Control Rig.", "ControlRig.InspectNotAdmitted")
        document = canonicalize(self.extract(target=exact_asset_object_path.rsplit(".", 1)[0],
                                             asset_handle=editor_asset_handle, context=context))
        return inspect_payload(document, ik.RigCalls(context, editor_asset_handle, prefix=PREFIX))

    def summarize_inspect(self, inspected):
        """The inspect over HTTP (unless detail is true): the shared summary without the per-node Pins and the
        GlobalTransforms (the bulk of a large rig's inspect)."""
        return inspect_summary.summarize(inspected, drop=("Pins", "GlobalTransforms"))

    def diff(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=False)

    def diff_exact(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=True)

    def extract_with_defaults(self, *, target, asset_handle, context):
        return extract_with_defaults(common.require_target(target, PREFIX), asset_handle, context, False)

    def diff_with_defaults(self, authored, extracted, live_defaults, exact):
        return diff(canonicalize(authored), canonicalize(extracted), exact, live_defaults)


def _meta():
    rows = [("rig_type", "/Body/RigType"), ("parent_class", "/Body/ParentClass"), ("module", "/Body/Module"),
            ("variables", "/Body/Variables"), ("hierarchy", "/Body/Hierarchy"), ("graphs", "/Body/Graphs"),
            ("functions", "/Body/Functions"), ("modules", "/Body/Modules"), ("connections", "/Body/Connections"),
            ("modular_settings", "/Body/ModularSettings")]
    regions = tuple(RegionMeta(f"control_rig.{name}.v1", path, RegionOwnership.MANAGED, IdentityPolicy.STABLE_KEY,
                               OrderPolicy.CANONICAL, DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE,
                               DeletePolicy.EXPLICIT_ONLY, ComparisonPolicy.CANONICAL)
                    for name, path in rows)
    return ProfileMeta(PROFILE_ID, ik.ROUTE, CLASS, regions, ik.REQUIREMENTS)


CONTROL_RIG_RUNTIME_REGISTRATION = RuntimeProfileRegistration(_meta(), ControlRigRuntimeProfile,
                                                              RuntimeTargetPolicy.CREATE_OR_UPDATE, LIFECYCLE)

__all__ = ("CONTROL_RIG_RUNTIME_REGISTRATION", "ControlRigRuntimeProfile", "assemble", "body_diff", "canonicalize",
           "diff", "order_kept")

# Module-level on purpose (after everything control_rig_apply uses from this module): the native runtime restores
# sys.modules between phase calls, so an in-call import would load a second copy of the primitive catalog.
from asset_document.profiles import control_rig_apply as _apply  # noqa: E402
