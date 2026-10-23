# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Control Rig graph and function planning (profile.control_rig.v1; see
docs/architecture/asset-document-controlrig-design.md).

``Body/Graphs`` and ``Body/Functions`` as the Control Rig editor's edits, through the ``rig.controlrig.edit.v1`` graph
and function ops (URigVMController with undo off; the apply's transaction covers the call). Phases, each planned
against a fresh read of the rig:

  dependents: rigs that call a public function the document renames, removes, makes private or re-pins
      (``ControlRig.DependentsAffected``, warn only)
  functions: function renames and adds (My Blueprint's Functions "+": an empty function, with the editor's execute
      pin when the document's function has an ``ExecuteContext`` IO input)
  interface: each function's access, colour, category / keywords / description, exposed pins (Details Inputs /
      Outputs: renames, removes, retypes, adds, order, argument defaults) and local variables
  structure (repeated until every graph exists): graph renames / removes / adds, then per graph (event graphs, function
      bodies, collapse graphs, parents first): a collapse graph's pins and header, node renames, node removes (also
      nodes whose kind changes: they are re-made), node adds in the graph's ``Order`` (palette spawn; a new collapse
      node is empty and its graph is planned in the next round; a node's pin types and operands right after it)
  shape: wildcard pin types (``PinTypes``), aggregate operands (``Aggregate``), traits
  wiring: broken links / unbound pins / reset values, array sizes, new links, new bindings (the ``Return`` node's
      inputs included)
  values: pin values (``Defaults``), function variable maps, position, size, title, colour, comment text / font /
      bubble, pin expansion (``Entry`` / ``Return`` included)
  cleanup: functions the document does not list are removed (after the graphs stopped calling them)

Graphs are named by scope as the read's records are: ("Graphs", g) / ("Functions", f), then ("Nodes", <collapse node>,
"Collapse") per level. Node entries (the read's form): exactly one kind key (Unit (+Method), Template (+PinTypes),
Variable (+Setter), Reroute, Enum, InvokeEvent, Comment, Function (+VariableMap), Collapse {Inputs, Outputs, Category?,
Keywords?, Description?, Entry?, Return?, Nodes, Order?}), ``Aggregate``, ``Traits``, ``Defaults`` {pin: value},
``Inputs`` {pin: "<node>.<pin>"}, ``Bindings`` {pin: variable}, ``Injections`` {pin: [{Unit, Method?, Input, Output,
Name?, Defaults?}]} (units injected into a top-level pin; links read and write on the host pin), ``Expanded`` [pin],
``Position``, ``Size``, ``Title``,
``Color``, ``FontSize``, ``Bubble``, ``ColorBubble``. ``Position``, ``Size``, ``Title``, ``Color``, ``Expanded`` and
``PinTypes`` omitted keep the rig's; ``RenamedFrom`` renames a node, a graph, a function, an exposed pin or a local
variable. A function / collapse graph's ``Entry`` and ``Return`` (its interface nodes) omitted keep the rig's.

A graph's ``Order`` is the model's node order: the compiler visits events and unlinked nodes in it, so it decides the
VM's instruction and memory order (``ComputeVMHash``). New nodes are added last, in ``Order`` (unlisted ones after the
listed in name order; names Nodes does not have are ignored). Existing nodes keep their place: the editor has no
command that reorders nodes (a node removed and added again goes last), so ``Order`` (and ``GraphOrder``) is not
compared (diff, verification); an ``Order`` that moves existing nodes is a ``ControlRig.OrderKept`` warning with the
rig's order (``control_rig.order_kept``). The interface nodes of a function or collapse graph come first (the editor
makes them first) and are not listed in ``Order``.
"""
from __future__ import annotations

from asset_document.profiles import data_table_common as common
from asset_document.profiles import ik_common as ik

escape = common.escape
DEFAULT_GRAPH = "Rig"
KIND_KEYS = ("Unit", "Template", "Variable", "Reroute", "Enum", "InvokeEvent", "Comment", "Function", "Collapse",
             "Unknown")
FUNCTION_KINDS = ("Function", "Collapse")
# Omitted = keep the rig's (layout and editor-derived state).
KEEP = ("Position", "Size", "Title", "Color", "Expanded", "PinTypes")
NODE_KEYS = KIND_KEYS + ("Method", "Setter", "PinTypes", "VariableMap", "Aggregate", "Traits", "Injections", "Defaults", "Inputs",
                         "Bindings", "Expanded", "Position", "Size", "Title", "Color", "FontSize", "Bubble",
                         "ColorBubble", "RenamedFrom")
GRAPH_KEYS = ("Nodes", "Order", "RenamedFrom")
HEADER_KEYS = ("Category", "Keywords", "Description")
COLLAPSE_KEYS = ("Inputs", "Outputs", "Entry", "Return", "Nodes", "Order", "LocalVariables") + HEADER_KEYS
FUNCTION_KEYS = COLLAPSE_KEYS + ("Public", "Color", "RenamedFrom")
INTERFACE_NODES = ("Entry", "Return")
INTERFACE_NODE_KEYS = ("Position", "Inputs", "Defaults", "Expanded")
PIN_KEYS = ("Name", "Type", "Direction", "Default", "RenamedFrom")
LOCAL_KEYS = ("Name", "Type", "Default", "RenamedFrom")
EXECUTE_PIN = "ExecuteContext"
COMMENT_DEFAULTS = {"FontSize": 18, "Bubble": False, "ColorBubble": False}
LAYOUT_STEP = (400.0, 150.0)
# Rounds of the structure phase: one per nesting level of new collapse graphs (and one to find nothing left).
MAX_STRUCTURE_ROUNDS = 16


def error(path, message, code):
    return common.error(path, message, code)


# ---------------------------------------------------------------------------
# Scopes
# ---------------------------------------------------------------------------
def scope_path(scope):
    return "/Body/" + "/".join(escape(part) for part in scope)


def graph_path(graph):
    return scope_path(("Graphs", graph))


def node_path(scope, node):
    return f"{scope_path(scope)}/Nodes/{escape(node)}"


def graph_field(scope):
    """An op's graph: an event graph by name, any other graph by scope."""
    return scope[1] if len(scope) == 2 and scope[0] == "Graphs" else list(scope)


def is_interface(scope):
    """Function bodies and collapse graphs have exposed pins and Entry / Return nodes."""
    return len(scope) > 2 or scope[0] == "Functions"


def containers(body):
    """(scope, container) of every graph of the Body: event graphs, function bodies and collapse graphs (parents
    first)."""
    result = []
    for section in ("Graphs", "Functions"):
        for name, container in (body.get(section) or {}).items():
            if type(container) is dict:
                _walk((section, name), container, result)
    return result


def _walk(scope, container, result):
    result.append((scope, container))
    nodes = container.get("Nodes")
    for name, entry in (nodes.items() if type(nodes) is dict else ()):
        if type(entry) is dict and type(entry.get("Collapse")) is dict:
            _walk(scope + ("Nodes", name, "Collapse"), entry["Collapse"], result)


def container_at(body, scope):
    container = (body.get(scope[0]) or {}).get(scope[1])
    rest = scope[2:]
    while type(container) is dict and rest:
        entry = (container.get("Nodes") or {}).get(rest[1])
        container = entry.get("Collapse") if type(entry) is dict else None
        rest = rest[3:]
    return container if type(container) is dict else None


def kind_of(entry):
    keys = [key for key in KIND_KEYS if key in entry]
    return keys[0] if len(keys) == 1 else None


def identity(entry):
    """What a node is (a change re-makes the node, as the editor has no edit for it)."""
    kind = kind_of(entry)
    if kind in ("Comment", "Collapse"):
        return (kind,)
    return (kind, entry.get(kind), entry.get("Method", "Execute") or "Execute", bool(entry.get("Setter", False)))


def _renamed(entry, name):
    old = entry.get("RenamedFrom") if type(entry) is dict else None
    return old if type(old) is str and old and old != name else None


def _split_source(value, nodes):
    """"<node>.<pin path>" -> (node, pin path), the longest node name of ``nodes`` that prefixes it."""
    best = None
    for name in nodes:
        if value.startswith(name + ".") and (best is None or len(name) > len(best)):
            best = name
    return (best, value[len(best) + 1:]) if best is not None else (None, None)


def _parents(path):
    parts = path.split(".")
    return [".".join(parts[:index]) for index in range(1, len(parts))]


def _is_exec_type(type_name):
    return type(type_name) is str and "ExecuteContext" in type_name


def pins_of(container):
    """[(pin entry, direction)] of a function / collapse graph's exposed pins, Inputs then Outputs."""
    pins = []
    for key, fallback in (("Inputs", "Input"), ("Outputs", "Output")):
        for pin in container.get(key) or []:
            if type(pin) is dict:
                pins.append((pin, pin.get("Direction") or fallback))
    return pins


# ---------------------------------------------------------------------------
# Shape checks and refusals (no write)
# ---------------------------------------------------------------------------
def validate_graphs(body):
    """Shape of Body/Graphs and Body/Functions: graphs {Nodes, Order?, RenamedFrom?}, functions and collapse graphs
    with their interface, node entries with one kind and well-formed pin maps."""
    for graph, container in (body.get("Graphs") or {}).items():
        _validate_container(("Graphs", graph), container, GRAPH_KEYS, "A graph")
    for function, container in (body.get("Functions") or {}).items():
        _validate_container(("Functions", function), container, FUNCTION_KEYS, "A function")


def _validate_container(scope, container, keys, what):
    where = scope_path(scope)
    if type(container) is not dict:
        raise error(where, f"{what} is an object.", "ControlRig.InvalidBody")
    for key in container:
        if key not in keys:
            raise error(f"{where}/{escape(key)}", f"{what} has {', '.join(keys)}; '{key}' is not one of them.",
                        "ControlRig.InvalidBody")
    if "RenamedFrom" in container and (type(container["RenamedFrom"]) is not str or not container["RenamedFrom"]):
        raise error(f"{where}/RenamedFrom", "RenamedFrom is the current name.", "ControlRig.InvalidBody")
    if "Color" in container:
        _validate_color(container["Color"], f"{where}/Color")
    if "Order" in container:
        order = container["Order"]
        if type(order) is not list or any(type(name) is not str for name in order) or len(set(order)) != len(order):
            raise error(f"{where}/Order", "Order is a list of the graph's node names, each once.", "ControlRig.InvalidBody")
    if type(container.get("Nodes", {})) is not dict:
        raise error(f"{where}/Nodes", "Nodes is an object keyed by node name.", "ControlRig.InvalidBody")
    if is_interface(scope):
        _validate_interface(scope, container)
    for name, entry in container.get("Nodes", {}).items():
        if name in INTERFACE_NODES and is_interface(scope):
            raise error(node_path(scope, name), f"'{name}' is the graph's interface node: write it as the graph's {name}, "
                        "not under Nodes.", "ControlRig.InvalidBody")
        _validate_node(entry, node_path(scope, name))
        if kind_of(entry) == "Collapse":
            _validate_container(scope + ("Nodes", name, "Collapse"), entry["Collapse"], COLLAPSE_KEYS, "A collapse node")


def _validate_color(color, where):
    if type(color) is not dict or not color or any(key not in "RGBA" or len(key) != 1 or type(value) not in (int, float)
                                                   or type(value) is bool for key, value in color.items()):
        raise error(where, "Color is {R, G, B, A} numbers; omit it to keep the current color.", "ControlRig.InvalidBody")


def _validate_interface(scope, container):
    where = scope_path(scope)
    names = set()
    for key, directions in (("Inputs", ("Input", "IO")), ("Outputs", ("Output",))):
        pins = container.get(key, [])
        if type(pins) is not list:
            raise error(f"{where}/{key}", f"{key} is a list of pins {{Name, Type, Default?}}.", "ControlRig.InvalidBody")
        for index, pin in enumerate(pins):
            at = f"{where}/{key}/{index}"
            if type(pin) is not dict or type(pin.get("Name")) is not str or not pin["Name"] or type(pin.get("Type")) is not str:
                raise error(at, "A pin is {Name, Type, Direction?, Default?}.", "ControlRig.InvalidBody")
            for field in pin:
                if field not in PIN_KEYS:
                    raise error(f"{at}/{escape(field)}", f"A pin has {', '.join(PIN_KEYS)}; '{field}' is not one of them.",
                                "ControlRig.InvalidBody")
            if pin.get("Direction", directions[0]) not in directions:
                raise error(f"{at}/Direction", f"Direction in {key} is {' or '.join(directions)}.", "ControlRig.InvalidBody")
            if pin["Name"] in names:
                raise error(f"{at}/Name", f"Pin '{pin['Name']}' is listed twice.", "ControlRig.InvalidBody")
            if "Default" in pin and (scope[0] != "Functions" or len(scope) > 2):
                raise error(f"{at}/Default", "A collapse node's pins take their values on the node (Defaults).",
                            "ControlRig.InvalidBody")
            names.add(pin["Name"])
    for name in INTERFACE_NODES:
        if name not in container:
            continue
        entry = container[name]
        if type(entry) is not dict or any(key not in INTERFACE_NODE_KEYS for key in entry):
            raise error(f"{where}/{name}", f"{name} has {', '.join(INTERFACE_NODE_KEYS)}.", "ControlRig.InvalidBody")
        _validate_node(dict(entry, Comment=""), f"{where}/{name}")
    if "LocalVariables" in container:
        locals_ = container["LocalVariables"]
        if type(locals_) is not list:
            raise error(f"{where}/LocalVariables", "LocalVariables is a list {Name, Type, Default?}.", "ControlRig.InvalidBody")
        seen = set()
        for index, local in enumerate(locals_):
            at = f"{where}/LocalVariables/{index}"
            if type(local) is not dict or type(local.get("Name")) is not str or not local["Name"] \
                    or type(local.get("Type")) is not str or any(key not in LOCAL_KEYS for key in local):
                raise error(at, "A local variable is {Name, Type, Default?}.", "ControlRig.InvalidBody")
            if local["Name"] in seen:
                raise error(f"{at}/Name", f"Local variable '{local['Name']}' is listed twice.", "ControlRig.InvalidBody")
            seen.add(local["Name"])
    for key in HEADER_KEYS:
        if key in container and type(container[key]) is not str:
            raise error(f"{where}/{key}", f"{key} is a string.", "ControlRig.InvalidBody")
    if "Public" in container and type(container["Public"]) is not bool:
        raise error(f"{where}/Public", "Public is true or false.", "ControlRig.InvalidBody")


def _validate_node(entry, where):
    if type(entry) is not dict:
        raise error(where, "A node is an object.", "ControlRig.InvalidBody")
    for key in entry:
        if key not in NODE_KEYS:
            raise error(f"{where}/{escape(key)}", f"'{key}' is not a node field (fields: {', '.join(NODE_KEYS)}).",
                        "ControlRig.InvalidBody")
    kinds = [key for key in KIND_KEYS if key in entry]
    if len(kinds) != 1:
        raise error(where, "A node has exactly one kind: Unit, Template, Variable, Reroute, Enum, InvokeEvent, Comment, "
                    f"Function or Collapse{'; it has ' + ', '.join(kinds) if kinds else ''}.", "ControlRig.InvalidBody")
    kind = kinds[0]
    if kind == "Collapse":
        if type(entry[kind]) is not dict:
            raise error(f"{where}/Collapse", "Collapse is the collapsed graph {Inputs, Outputs, Nodes, ...}.",
                        "ControlRig.InvalidBody")
    elif type(entry[kind]) is not str:
        raise error(f"{where}/{kind}", f"{kind} is a string.", "ControlRig.InvalidBody")
    if "VariableMap" in entry and kind != "Function":
        raise error(f"{where}/VariableMap", "VariableMap belongs to function nodes.", "ControlRig.InvalidBody")
    for key in ("Defaults", "Inputs", "Bindings", "Traits", "PinTypes", "VariableMap"):
        if key in entry and type(entry[key]) is not dict:
            raise error(f"{where}/{key}", f"{key} is an object keyed by pin path.", "ControlRig.InvalidBody")
    for key in ("Inputs", "Bindings", "PinTypes", "VariableMap"):
        for pin, value in entry.get(key, {}).items():
            if type(value) is not str or not value:
                raise error(f"{where}/{key}/{escape(pin)}", f"A {key} value is a non-empty string.", "ControlRig.InvalidBody")
    for key in ("Expanded", "Aggregate"):
        if key in entry and (type(entry[key]) is not list or any(type(item) is not str for item in entry[key])):
            raise error(f"{where}/{key}", f"{key} is a list of pin names.", "ControlRig.InvalidBody")
    for key in ("Position", "Size"):
        if key in entry and (type(entry[key]) is not list or len(entry[key]) != 2
                             or any(type(item) not in (int, float) or type(item) is bool for item in entry[key])):
            raise error(f"{where}/{key}", f"{key} is [x, y].", "ControlRig.InvalidBody")
    # No reset to default for these in the editor: a null is refused, not dropped (omit the field to keep the value).
    if "Title" in entry and type(entry["Title"]) is not str:
        raise error(f"{where}/Title", "Title is a string; omit it to keep the node's title.", "ControlRig.InvalidBody")
    if "Color" in entry:
        _validate_color(entry["Color"], f"{where}/Color")
    for pin in entry.get("Defaults", {}):
        for other in ("Inputs", "Bindings"):
            if pin in entry.get(other, {}):
                raise error(f"{where}/Defaults/{escape(pin)}", f"Pin {pin} is in Defaults and {other}: a linked or bound "
                            "pin has no value of its own.", "ControlRig.InvalidBody")
    for pin in entry.get("Inputs", {}):
        if pin in entry.get("Bindings", {}):
            raise error(f"{where}/Inputs/{escape(pin)}", f"Pin {pin} is linked and bound; it takes one source.",
                        "ControlRig.InvalidBody")
    if "RenamedFrom" in entry and (type(entry["RenamedFrom"]) is not str or not entry["RenamedFrom"]):
        raise error(f"{where}/RenamedFrom", "RenamedFrom is the node's current name.", "ControlRig.InvalidBody")
    injections = entry.get("Injections", {})
    if type(injections) is not dict:
        raise error(f"{where}/Injections", "Injections is an object keyed by pin name.", "ControlRig.InvalidBody")
    for pin, units in injections.items():
        if type(units) is not list or any(type(unit) is not dict or type(unit.get("Unit")) is not str
                                          or type(unit.get("Input")) is not str or type(unit.get("Output")) is not str
                                          for unit in units):
            raise error(f"{where}/Injections/{escape(pin)}", "A pin's injections are a list of {Unit, Method?, Input, "
                        "Output, Name?, Defaults?}.", "ControlRig.InvalidBody")


def live_names(entries, live_entries):
    """Document name -> the rig's name it edits (RenamedFrom, else the same name; None for a new entry)."""
    result = {}
    for name, entry in entries.items():
        old = _renamed(entry, name)
        if old is not None and old in live_entries:
            result[name] = old
        else:
            result[name] = name if name in live_entries else None
    return result


live_graph_names = live_names
live_node_names = live_names


def ordered(container):
    """The graph's node names in its Order (the model's node order, which the compiler visits events and unlinked
    nodes in), the nodes it does not list after them in name order. Names Nodes does not have are ignored (a node
    removed from Nodes and left in Order is removed)."""
    nodes = container.get("Nodes", {})
    order = [name for name in container.get("Order", []) if name in nodes] if type(container.get("Order")) is list else []
    listed = set(order)
    return order + sorted(name for name in nodes if name not in listed)


def _remade(entry, current):
    """True when a node must be re-made: another kind / unit / method / variable / function / getter-setter, a pin
    type the node has resolved otherwise, or an aggregate node the document lists without operands."""
    if identity(entry) != identity(current):
        return True
    types = entry.get("PinTypes")
    if type(types) is dict:
        have = current.get("PinTypes", {})
        if any(pin in have and have[pin] != value for pin, value in types.items()):
            return True
    return "Aggregate" in current and "Aggregate" not in entry


def check(body, live):
    """Refusals of Body/Graphs and Body/Functions that need no write: their shape, link sources, reroutes, the default
    graph, RenamedFrom names the rig does not have."""
    if "Graphs" not in body and "Functions" not in body:
        return
    validate_graphs(body)
    for scope, container in containers(body):
        _check_links(scope, container)
    for section, what, code in (("Graphs", "graph", "ControlRig.GraphNotFound"),
                                ("Functions", "function", "ControlRig.FunctionNotFound")):
        have = live.get(section) or {}
        for name, container in (body.get(section) or {}).items():
            old = _renamed(container, name)
            if old is not None and old not in have and name not in have:
                raise error(f"{scope_path((section, name))}/RenamedFrom", f"RenamedFrom names {what} '{old}', which the "
                            f"rig does not have.{ik.describe_candidates(old, sorted(have), section)}", code)
    if "Graphs" in body:
        graphs = body["Graphs"]
        live_graphs = live.get("Graphs", {})
        mapping = live_names(graphs, live_graphs)
        if DEFAULT_GRAPH in live_graphs and DEFAULT_GRAPH not in graphs:
            raise error("/Body/Graphs", "Graphs does not list the Rig graph (the rig's default graph), which cannot be "
                        "removed or renamed.", "ControlRig.InvalidBody")
        if mapping.get(DEFAULT_GRAPH) not in (DEFAULT_GRAPH, None) or DEFAULT_GRAPH in [
                old for name, old in mapping.items() if name != DEFAULT_GRAPH]:
            raise error(f"{graph_path(DEFAULT_GRAPH)}/RenamedFrom", "The Rig graph keeps its name.", "ControlRig.InvalidBody")
    for scope, container in containers(body):
        live_container = container_at(live, live_scope(body, live, scope) or ("", ""))
        live_nodes = (live_container or {}).get("Nodes", {})
        names = live_names(container.get("Nodes", {}), live_nodes)
        for name, entry in container.get("Nodes", {}).items():
            if names[name] is not None:
                continue
            where = node_path(scope, name)
            old = _renamed(entry, name)
            if old is not None and live_container is not None:
                raise error(f"{where}/RenamedFrom", f"RenamedFrom names node '{old}', which the graph does not have."
                            f"{ik.describe_candidates(old, sorted(live_nodes), 'Nodes')}", "ControlRig.NodeNotFound")
            if kind_of(entry) == "Unknown":
                raise error(f"{where}/Unknown", "A node of an unknown kind cannot be added.", "ControlRig.InvalidBody")


def live_scope(body, live, scope):
    """The rig's scope a document scope edits (graph, function and collapse node renames applied), None when the
    graph is new."""
    section, name = scope[0], scope[1]
    doc = (body.get(section) or {}).get(name) or {}
    have = live.get(section) or {}
    current = live_names({name: doc}, have)[name]
    if current is None:
        return None
    result = (section, current)
    doc_container, live_container = doc, have[current]
    rest = scope[2:]
    while rest:
        node = rest[1]
        entry = (doc_container.get("Nodes") or {}).get(node) or {}
        live_nodes = live_container.get("Nodes") or {}
        current = live_names({node: entry}, live_nodes)[node]
        if current is None or type(live_nodes[current].get("Collapse")) is not dict:
            return None
        result += ("Nodes", current, "Collapse")
        doc_container, live_container = entry.get("Collapse") or {}, live_nodes[current]["Collapse"]
        rest = rest[3:]
    return result


def _check_links(scope, container):
    """Link sources name document nodes (or the graph's Entry); reroutes the editor keeps on load
    (PatchRerouteNodesOnLoad: a value reroute needs its top-level input linked and no sub-pin links, else load removes
    it or turns it into a constant / make struct / make array)."""
    nodes = container.get("Nodes", {})
    sources = list(nodes) + (["Entry"] if is_interface(scope) else [])
    sub_targets = {}
    targets = list(nodes.items())
    if is_interface(scope) and type(container.get("Return")) is dict:
        targets.append(("Return", container["Return"]))
    for name, entry in targets:
        where = f"{scope_path(scope)}/Return" if name == "Return" and name not in nodes else node_path(scope, name)
        for pin, source in entry.get("Inputs", {}).items():
            node, source_pin = _split_source(source, sources)
            if node is None:
                wanted = source.split(".", 1)[0] if type(source) is str else ""
                hint = ik.describe_candidates(wanted, sources, "Nodes") if type(source) is str else ""
                raise error(f"{where}/Inputs/{escape(pin)}", f"'{source}' names no node of the graph (a source is "
                            f"\"<node>.<pin>\").{hint}", "ControlRig.NodeNotFound")
            if "." in source_pin:
                sub_targets.setdefault(node, pin)
    for name, entry in nodes.items():
        if kind_of(entry) != "Reroute" or _is_exec_type(entry.get("Reroute")):
            continue
        where = node_path(scope, name)
        inputs = entry.get("Inputs", {})
        if "Value" not in inputs:
            raise error(f"{where}/Inputs", f"Reroute '{name}' has no linked input: on load the editor removes it, or turns it "
                        "into a constant / make struct / make array. Link its Value input, or use a Constant node.",
                        "ControlRig.UnstableReroute")
        if any(pin != "Value" for pin in inputs) or name in sub_targets:
            raise error(f"{where}/Inputs", f"Reroute '{name}' has sub-pin links: on load the editor turns it into make / break "
                        "struct nodes. Link its whole Value pin only.", "ControlRig.UnstableReroute")


# ---------------------------------------------------------------------------
# Functions: dependents, library, interface, cleanup
# ---------------------------------------------------------------------------
def _signature(container):
    return [(pin.get("Name"), pin.get("Type"), direction) for pin, direction in pins_of(container)]


def _dependents(body, live, make_op):
    """The public functions the document renames, removes, makes private or re-pins, for the dependents report."""
    if "Functions" not in body:
        return []
    functions = body["Functions"]
    have = live.get("Functions", {})
    mapping = live_names(functions, have)
    by_live = {old: name for name, old in mapping.items() if old is not None}
    changes = []
    for name, entry in have.items():
        if not entry.get("Public"):
            continue
        new = by_live.get(name)
        if new is None:
            changes.append({"name": name, "change": "removed"})
            continue
        doc = functions[new]
        what = []
        if new != name:
            what.append(f"renamed to '{new}'")
        if not doc.get("Public", False):
            what.append("made private")
        if _signature(doc) != _signature(entry):
            what.append("changed (its pins)")
        if what:
            changes.append({"name": name, "change": " and ".join(what)})
    if not changes:
        return []
    return [make_op({"op": "function.dependents", "functions": changes}, "/Body/Functions")]


def _mutable(container):
    """The editor's New Function (an execute pin, linked from Entry to Return) when the function's first input is the
    ExecuteContext IO pin."""
    pins = pins_of(container)
    return bool(pins) and pins[0][0].get("Name") == EXECUTE_PIN and pins[0][1] == "IO" and _is_exec_type(pins[0][0].get("Type"))


def _functions(body, live, make_op):
    """Function renames (a function of the target name the document does not keep is removed first) and adds."""
    if "Functions" not in body:
        return []
    functions = body["Functions"]
    have = live.get("Functions", {})
    mapping = live_names(functions, have)
    renamed_away = {old for name, old in mapping.items() if old is not None and old != name}
    removes, renames, adds = [], [], []
    for name, old in mapping.items():
        where = scope_path(("Functions", name))
        if old is None or old == name:
            continue
        if name in have and name not in renamed_away:
            removes.append(make_op({"op": "function.remove", "name": name}, where))
        renames.append(make_op({"op": "function.rename", "name": old, "to": name}, where, {"to": f"{where}/RenamedFrom"}))
    for name, old in mapping.items():
        if old is None:
            where = scope_path(("Functions", name))
            adds.append(make_op({"op": "function.add", "name": name, "mutable": _mutable(functions[name])}, where,
                                {"name": where}))
    return removes + renames + adds


def _header_ops(scope, container, current, make_op, function):
    where = scope_path(scope)
    op = {"op": "function.set", "graph": list(scope)}
    fields = {}
    for key in HEADER_KEYS:
        if container.get(key, "") != current.get(key, ""):
            op[key.lower()] = container.get(key, "")
            fields[key.lower()] = f"{where}/{key}"
    if function:
        if bool(container.get("Public", False)) != bool(current.get("Public", False)):
            op["public"] = bool(container.get("Public", False))
            fields["public"] = f"{where}/Public"
        if "Color" in container and not common.same(container["Color"], current.get("Color"), False):
            op["color"] = container["Color"]
            fields["color"] = f"{where}/Color"
    return [make_op(op, where, fields)] if len(op) > 2 else []


def _pin_ops(scope, container, current, make_op, function):
    """Exposed pins as the Details panel's Inputs / Outputs edit them: renames, removes (also pins whose direction
    changes), retypes, adds, order, argument defaults."""
    graph = list(scope)
    where = scope_path(scope)
    wanted = pins_of(container)
    have = {pin["Name"]: (pin, direction) for pin, direction in pins_of(current)}
    paths = {}
    for key in ("Inputs", "Outputs"):
        for index, pin in enumerate(container.get(key) or []):
            paths[pin["Name"]] = f"{where}/{key}/{index}"
    renames, removes, types, adds, defaults = [], [], [], [], []
    after = dict(have)
    names = {pin["Name"] for pin, _ in wanted}
    for pin, _ in wanted:
        old = _renamed(pin, pin["Name"])
        if old is not None and old in have and pin["Name"] not in have:
            renames.append(make_op({"op": "function.pin.rename", "graph": graph, "name": old, "to": pin["Name"]},
                                   paths[pin["Name"]], {"to": f"{paths[pin['Name']]}/Name"}))
            after[pin["Name"]] = after.pop(old)
    for name, (pin, direction) in list(after.items()):
        target = next((item for item in wanted if item[0]["Name"] == name), None)
        if name not in names or target[1] != direction:
            removes.append(make_op({"op": "function.pin.remove", "graph": graph, "name": name},
                                   paths.get(name, where)))
            del after[name]
    for pin, direction in wanted:
        at = paths[pin["Name"]]
        present = after.get(pin["Name"])
        if present is None:
            op = {"op": "function.pin.add", "graph": graph, "name": pin["Name"], "direction": direction, "type": pin["Type"]}
            if function and "Default" in pin:
                op["default"] = pin["Default"]
            adds.append(make_op(op, at, {"name": f"{at}/Name", "type": f"{at}/Type", "direction": f"{at}/Direction"},
                                {"default": f"{at}/Default"}))
            continue
        if present[0].get("Type") != pin["Type"]:
            types.append(make_op({"op": "function.pin.type", "graph": graph, "name": pin["Name"], "type": pin["Type"]},
                                 f"{at}/Type"))
        if function and not _is_exec_type(pin["Type"]):
            if "Default" in pin and ("Default" not in present[0] or present[0].get("Type") != pin["Type"]
                                     or not common.same(pin["Default"], present[0]["Default"], False)):
                defaults.append(make_op({"op": "function.pin.default", "graph": graph, "name": pin["Name"],
                                         "value": pin["Default"]}, f"{at}/Default", nested={"value": f"{at}/Default"}))
            elif "Default" not in pin and "Default" in present[0] and present[0].get("Type") == pin["Type"]:
                defaults.append(make_op({"op": "function.pin.default", "graph": graph, "name": pin["Name"], "value": None},
                                        at))
    # Order: new pins are appended; the pins move only when the order within the inputs or the outputs differs (the
    # editor's pin list interleaves them as they were added).
    renamed = {_renamed(pin, pin["Name"]): pin["Name"] for pin, _ in wanted if _renamed(pin, pin["Name"]) in have}
    sequence = [renamed.get(pin["Name"], pin["Name"]) for pin, _ in pins_of(current)]
    sequence = [name for name in sequence if name in after] + [pin["Name"] for pin, _ in wanted if pin["Name"] not in after]
    directions = {pin["Name"]: direction for pin, direction in wanted}

    def groups(names_in_order):
        return ([name for name in names_in_order if directions.get(name) != "Output"],
                [name for name in names_in_order if directions.get(name) == "Output"])
    order = []
    if groups(sequence) != groups([pin["Name"] for pin, _ in wanted]):
        order = [make_op({"op": "function.pin.index", "graph": graph, "name": pin["Name"], "index": index},
                         paths[pin["Name"]]) for index, (pin, _) in enumerate(wanted)]
    return renames + removes + types + adds + order + defaults


def _local_ops(scope, container, current, make_op):
    """Local variables as My Blueprint's Local Variables edit them: renames, removes, retypes, adds, defaults."""
    graph = list(scope)
    where = f"{scope_path(scope)}/LocalVariables"
    wanted = container.get("LocalVariables") or []
    have = {local["Name"]: local for local in current.get("LocalVariables") or []}
    renames, removes, types, adds, defaults = [], [], [], [], []
    after = dict(have)
    names = {local["Name"] for local in wanted}
    for index, local in enumerate(wanted):
        old = _renamed(local, local["Name"])
        if old is not None and old in have and local["Name"] not in have:
            renames.append(make_op({"op": "local.rename", "graph": graph, "name": old, "to": local["Name"]},
                                   f"{where}/{index}", {"to": f"{where}/{index}/Name"}))
            after[local["Name"]] = after.pop(old)
    for name in list(after):
        if name not in names:
            removes.append(make_op({"op": "local.remove", "graph": graph, "name": name}, where))
            del after[name]
    for index, local in enumerate(wanted):
        at = f"{where}/{index}"
        present = after.get(local["Name"])
        if present is None:
            op = {"op": "local.add", "graph": graph, "name": local["Name"], "type": local["Type"]}
            if "Default" in local:
                op["default"] = local["Default"]
            adds.append(make_op(op, at, {"name": f"{at}/Name", "type": f"{at}/Type"}, {"default": f"{at}/Default"}))
            continue
        retyped = present.get("Type") != local["Type"]
        if retyped:
            types.append(make_op({"op": "local.type", "graph": graph, "name": local["Name"], "type": local["Type"]},
                                 f"{at}/Type"))
        if "Default" in local and (retyped or "Default" not in present
                                   or not common.same(local["Default"], present["Default"], False)):
            defaults.append(make_op({"op": "local.default", "graph": graph, "name": local["Name"], "value": local["Default"]},
                                    f"{at}/Default", nested={"value": f"{at}/Default"}))
        elif "Default" not in local and "Default" in present and not retyped:
            defaults.append(make_op({"op": "local.default", "graph": graph, "name": local["Name"], "value": None}, at))
    return renames + removes + types + adds + defaults


def _interface(body, live, make_op):
    """Each function's header, access, colour, exposed pins and local variables (after renames and adds)."""
    ops = []
    have = live.get("Functions", {})
    for name, container in (body.get("Functions") or {}).items():
        current = have.get(name)
        if current is None:
            continue
        scope = ("Functions", name)
        ops += _header_ops(scope, container, current, make_op, True)
        ops += _pin_ops(scope, container, current, make_op, True)
        ops += _local_ops(scope, container, current, make_op)
    return ops


def _cleanup(body, live, make_op):
    """Functions the document does not list (the graphs no longer call them)."""
    if "Functions" not in body:
        return []
    kept = set(live_names(body["Functions"], live.get("Functions", {})).values()) | set(body["Functions"])
    return [make_op({"op": "function.remove", "name": name}, "/Body/Functions")
            for name in live.get("Functions", {}) if name not in kept]


# ---------------------------------------------------------------------------
# Graph ops
# ---------------------------------------------------------------------------
def _layout_x(live):
    return max([entry["Position"][0] for _, container in containers(live) for entry in container.get("Nodes", {}).values()
                if type(entry) is dict and type(entry.get("Position")) is list] or [-LAYOUT_STEP[0]])


def _structure(body, live, deprecated, make_op, planned=None):
    """Graph renames / removes / adds, then per graph that exists and was not planned yet (``planned``, the scopes of
    earlier rounds): a collapse graph's pins and header, node renames / removes (and re-made nodes), node adds."""
    planned = set() if planned is None else planned
    renames, removes, adds = [], [], []
    graph_ops = []
    max_x = _layout_x(live)
    if "Graphs" in body and ("Graphs",) not in planned:
        planned.add(("Graphs",))
        graphs = body["Graphs"]
        live_graphs = live.get("Graphs", {})
        mapping = live_names(graphs, live_graphs)
        for graph, old in mapping.items():
            if old is not None and old != graph:
                renames.append(make_op({"op": "graph.rename", "name": old, "to": graph}, graph_path(graph),
                                       {"to": f"{graph_path(graph)}/RenamedFrom"}))
        for name in live_graphs:
            if name not in mapping.values() and name != DEFAULT_GRAPH:
                removes.append(make_op({"op": "graph.remove", "name": name}, "/Body/Graphs"))
        # New graphs in GraphOrder (the model order the compiler visits events in); existing graphs keep their place.
        order = [name for name in body.get("GraphOrder", []) if name in mapping]
        for graph in order + [name for name in mapping if name not in order]:
            if mapping[graph] is None:
                adds.append(make_op({"op": "graph.add", "name": graph}, graph_path(graph)))
        for graph, old in mapping.items():
            scope = ("Graphs", graph)
            live_container = (live_graphs.get(old) or {}) if old is not None else {}
            planned.add(scope)
            graph_ops.append(_container_structure(scope, graphs[graph], live_container, deprecated, make_op, max_x))
    interface_ops = []
    for scope, container in containers(body):
        if scope in planned or scope[0] == "Graphs" and len(scope) == 2:
            continue
        current = container_at(live, scope)
        if current is None:
            continue
        planned.add(scope)
        if len(scope) > 2:
            _check_collapse_locals(scope, container, current)
            interface_ops += _header_ops(scope, container, current, make_op, False)
            interface_ops += _pin_ops(scope, container, current, make_op, False)
        graph_ops.append(_container_structure(scope, container, current, deprecated, make_op, max_x))
    node_removes = [op for item in graph_ops for op in item[0]]
    node_renames = [op for item in graph_ops for op in item[1]]
    node_adds = [op for item in graph_ops for op in item[2]]
    # Removes free the names renames take (they touch disjoint names: removed ones are not kept).
    return removes + renames + node_removes + node_renames + adds + interface_ops + node_adds


def _check_collapse_locals(scope, container, current):
    """A collapse graph keeps the local variables it has: the editor adds, renames and removes local variables on
    functions only (older engines left some on collapse graphs made from functions)."""
    wanted = [dict(local) for local in container.get("LocalVariables") or []]
    have = [dict(local) for local in current.get("LocalVariables") or []]
    if len(wanted) != len(have) or any(
            mine.get("Name") != theirs.get("Name") or mine.get("Type") != theirs.get("Type")
            or ("Default" in mine) != ("Default" in theirs)
            or ("Default" in mine and not common.same(mine["Default"], theirs["Default"], False))
            for mine, theirs in zip(wanted, have)):
        raise error(f"{scope_path(scope)}/LocalVariables", "The editor gives local variables to functions only: a collapse "
                    "graph keeps the local variables it has (list them unchanged, or move the nodes into a function).",
                    "ControlRig.EditRefused")


def _container_structure(scope, container, current, deprecated, make_op, max_x):
    """(node removes, node renames, node adds) of one graph against its ``current`` entry ({} for a new graph)."""
    graph = graph_field(scope)
    nodes = container.get("Nodes", {})
    live_nodes = current.get("Nodes", {})
    names = live_names(nodes, live_nodes)
    remade = {name for name, entry in nodes.items()
              if names[name] is not None and _remade(entry, live_nodes[names[name]])}
    kept = {names[name] for name in nodes if names[name] is not None and name not in remade}
    node_renames, node_removes, node_adds = [], [], []
    for name, entry in nodes.items():
        old = names[name]
        if old is None or old == name or name in remade:
            continue
        node_renames.append(make_op({"op": "node.rename", "graph": graph, "name": old, "to": name}, node_path(scope, name),
                                    {"to": f"{node_path(scope, name)}/RenamedFrom"}))
    for name in live_nodes:
        if name not in kept:
            node_removes.append(make_op({"op": "node.remove", "graph": graph, "name": name},
                                        node_path(scope, name) if name in nodes else f"{scope_path(scope)}/Nodes"))
    row = 0
    # New nodes go last in the model, as the palette adds them: in the graph's Order.
    for name in ordered(container):
        entry = nodes[name]
        if names[name] is not None and name not in remade:
            continue
        where = node_path(scope, name)
        wire = {key: entry[key] for key in KIND_KEYS + ("Method", "Setter", "Size", "Color") if key in entry}
        if "Collapse" in wire:
            # An empty collapse node: its pins, header and nodes are planned once it exists.
            wire["Collapse"] = {}
        if "Position" in entry:
            wire["Position"] = entry["Position"]
        else:
            wire["Position"] = [max_x + LAYOUT_STEP[0], row * LAYOUT_STEP[1]]
            row += 1
        defaults = {pin: value for pin, value in entry.get("Defaults", {}).items() if "." not in pin}
        if defaults and "Unit" in entry:
            wire["Defaults"] = defaults
        op = {"op": "node.add", "graph": graph, "name": name, "entry": wire}
        if deprecated:
            op["deprecated"] = "warn"
        node_adds.append(make_op(op, where, {"name": where}, {"entry": where}))
        # Its pin types and operands right away, as the editor does on a node it just placed: the first operand
        # added turns the node into an aggregate node, which goes last in the model.
        node_adds += _shape_ops(scope, name, entry, {}, make_op)
    return node_removes, node_renames, node_adds


def _pairs(body, live, interface_nodes=False):
    """(scope, name, entry, live entry) of the document nodes the rig has (by name, after the structure phase), with
    ``interface_nodes`` also the Entry / Return the document lists."""
    for scope, container in containers(body):
        current = container_at(live, scope)
        if current is None:
            continue
        live_nodes = current.get("Nodes", {})
        for name, entry in container.get("Nodes", {}).items():
            if name in live_nodes:
                yield scope, name, entry, live_nodes[name]
        if interface_nodes and is_interface(scope):
            for name in INTERFACE_NODES:
                if type(container.get(name)) is dict and type(current.get(name)) is dict:
                    yield scope, name, container[name], current[name]


def _entry_path(scope, name):
    return f"{scope_path(scope)}/{name}" if name in INTERFACE_NODES and is_interface(scope) else node_path(scope, name)


def _shape(body, live, make_op):
    """Wildcard pin types, aggregate operands, traits."""
    ops = []
    for scope, name, entry, current in _pairs(body, live):
        ops += _shape_ops(scope, name, entry, current, make_op)
    return ops


def _shape_ops(scope, name, entry, current, make_op):
    """Pin types, aggregate operands and traits of one node against its ``current`` entry ({} for a new node)."""
    ops = []
    graph = graph_field(scope)
    where = node_path(scope, name)
    types = entry.get("PinTypes")
    if type(types) is dict:
        have = current.get("PinTypes", {})
        wanted = {pin: value for pin, value in types.items() if have.get(pin) != value}
        if wanted:
            ops.append(make_op({"op": "node.types", "graph": graph, "name": name, "types": wanted}, f"{where}/PinTypes",
                               nested={"types": f"{where}/PinTypes"}))
    if "Aggregate" in entry and entry["Aggregate"] != current.get("Aggregate"):
        ops.append(make_op({"op": "node.aggregate", "graph": graph, "name": name, "pins": entry["Aggregate"]},
                           f"{where}/Aggregate"))
    if not common.same(entry.get("Traits", {}), current.get("Traits", {}), False):
        ops.append(make_op({"op": "node.traits", "graph": graph, "name": name, "traits": entry.get("Traits", {})},
                           f"{where}/Traits", nested={"traits": f"{where}/Traits"}))
    injections = entry.get("Injections", {})
    have = current.get("Injections", {})
    if type(have) is dict and type(injections) is dict:
        from asset_document.profiles import control_rig as cr
        have = cr.injections_as_authored(injections, have)
    if not common.same(injections, have, False):
        ops.append(make_op({"op": "node.injections", "graph": graph, "name": name, "injections": injections},
                           f"{where}/Injections", nested={"injections": f"{where}/Injections"}))
    return ops


def _array_sizes(entry):
    """{array pin path: size} the document's indexed pin paths imply (the read writes an array's last element when it
    lists elements one by one)."""
    sizes = {}
    for key in ("Defaults", "Inputs", "Bindings"):
        for pin in entry.get(key, {}):
            parts = pin.split(".")
            for index in range(1, len(parts)):
                if parts[index].isdigit():
                    array = ".".join(parts[:index])
                    sizes[array] = max(sizes.get(array, 0), int(parts[index]) + 1)
    return sizes


def _wiring(body, live, make_op):
    """Broken links, unbound pins, reset values, array sizes, new links, new bindings."""
    breaks, unbinds, resets, arrays, links, binds = [], [], [], [], [], []
    for scope, name, entry, current in _pairs(body, live, interface_nodes=True):
        graph = graph_field(scope)
        where = _entry_path(scope, name)
        inputs, have_inputs = entry.get("Inputs", {}), current.get("Inputs", {})
        for pin, source in have_inputs.items():
            if inputs.get(pin) != source:
                breaks.append(make_op({"op": "link.break", "graph": graph, "from": source, "to": f"{name}.{pin}"},
                                      f"{where}/Inputs/{escape(pin)}" if pin in inputs else f"{where}/Inputs"))
        bindings, have_bindings = entry.get("Bindings", {}), current.get("Bindings", {})
        for pin, variable in have_bindings.items():
            if bindings.get(pin) != variable:
                unbinds.append(make_op({"op": "pin.bind", "graph": graph, "pin": f"{name}.{pin}", "variable": ""},
                                       f"{where}/Bindings"))
        defaults = entry.get("Defaults", {})
        sources = set(defaults) | set(inputs) | set(bindings)
        for pin in current.get("Defaults", {}):
            if pin in defaults or any(parent in sources for parent in _parents(pin)) or pin in inputs or pin in bindings:
                continue
            resets.append(make_op({"op": "pin.default", "graph": graph, "pin": f"{name}.{pin}", "value": None},
                                  f"{where}/Defaults"))
        have_sizes = _array_sizes(current)
        for array, size in sorted(_array_sizes(entry).items(), key=lambda item: item[0].count(".")):
            if array in defaults:
                continue
            if have_sizes.get(array) != size:
                arrays.append(make_op({"op": "pin.array", "graph": graph, "pin": f"{name}.{array}", "size": size},
                                      f"{where}/Defaults"))
        for pin, source in inputs.items():
            if have_inputs.get(pin) != source:
                links.append(make_op({"op": "link.add", "graph": graph, "from": source, "to": f"{name}.{pin}"},
                                     f"{where}/Inputs/{escape(pin)}"))
        for pin, variable in bindings.items():
            if have_bindings.get(pin) != variable:
                binds.append(make_op({"op": "pin.bind", "graph": graph, "pin": f"{name}.{pin}", "variable": variable},
                                     f"{where}/Bindings/{escape(pin)}"))
    return breaks + unbinds + resets + arrays + links + binds


def _values(body, live, make_op):
    """Pin values, function variable maps, then the node's layout and display fields."""
    values, remaps, sets = [], [], []
    for scope, name, entry, current in _pairs(body, live, interface_nodes=True):
        graph = graph_field(scope)
        where = _entry_path(scope, name)
        have = current.get("Defaults", {})
        for pin, value in entry.get("Defaults", {}).items():
            if pin not in have or not common.same(value, have[pin], False):
                values.append(make_op({"op": "pin.default", "graph": graph, "pin": f"{name}.{pin}", "value": value},
                                      f"{where}/Defaults/{escape(pin)}", {"value": f"{where}/Defaults/{escape(pin)}"}))
        if kind_of(entry) == "Function" and not common.same(entry.get("VariableMap", {}), current.get("VariableMap", {}), False):
            remaps.append(make_op({"op": "node.remap", "graph": graph, "name": name, "map": entry.get("VariableMap", {})},
                                  f"{where}/VariableMap", nested={"map": f"{where}/VariableMap"}))
        op = {"op": "node.set", "graph": graph, "name": name}
        fields = {}
        for key, field in (("Position", "position"), ("Size", "size"), ("Title", "title"), ("Color", "color")):
            if key in entry and not common.same(entry[key], current.get(key, "" if key == "Title" else None), False):
                op[field] = entry[key]
                fields[field] = f"{where}/{key}"
        if kind_of(entry) == "Comment":
            if entry["Comment"] != current.get("Comment"):
                op["comment"] = entry["Comment"]
                fields["comment"] = f"{where}/Comment"
            for key, field in (("FontSize", "font_size"), ("Bubble", "bubble"), ("ColorBubble", "color_bubble")):
                default = COMMENT_DEFAULTS[key]
                if not common.same(entry.get(key, default), current.get(key, default), False):
                    op[field] = entry.get(key, default)
                    fields[field] = f"{where}/{key}"
        if "Expanded" in entry:
            wanted, have_expanded = set(entry["Expanded"]), set(current.get("Expanded", []))
            changes = {pin: True for pin in entry["Expanded"] if pin not in have_expanded}
            changes.update({pin: False for pin in current.get("Expanded", []) if pin not in wanted})
            if changes:
                op["expanded"] = changes
                fields["expanded"] = f"{where}/Expanded"
        if len(op) > 3:
            sets.append(make_op(op, where, fields))
    return values + remaps + sets


PHASES = ("dependents", "functions", "interface", "structure", "shape", "wiring", "values", "cleanup")


def phase(name, body, live, make_op, deprecated=False, planned=None):
    """The ops of one phase planned against ``live`` (a fresh read). ``planned`` (structure) carries the graphs earlier
    rounds planned: run the structure phase until it plans no new graph."""
    if "Graphs" not in body and "Functions" not in body:
        return []
    if name == "dependents":
        return _dependents(body, live, make_op)
    if name == "functions":
        return _functions(body, live, make_op)
    if name == "interface":
        return _interface(body, live, make_op)
    if name == "structure":
        return _structure(body, live, deprecated, make_op, planned)
    if name == "shape":
        return _shape(body, live, make_op)
    if name == "wiring":
        return _wiring(body, live, make_op)
    if name == "values":
        return _values(body, live, make_op)
    return _cleanup(body, live, make_op)


def differences(body, live):
    """Differences of Body/Graphs and Body/Functions from the rig after the phases (what the editor did otherwise than
    asked)."""
    from asset_document.profiles import control_rig as cr
    authored = {key: body[key] for key in ("Graphs", "GraphOrder", "Functions") if key in body}
    if not authored:
        return ()
    return tuple(cr.body_diff(authored, {key: live[key] for key in ("Graphs", "GraphOrder", "Functions") if key in live},
                              exact=False))


__all__ = ("FUNCTION_KINDS", "KEEP", "KIND_KEYS", "MAX_STRUCTURE_ROUNDS", "PHASES", "check", "containers",
           "differences", "identity", "kind_of", "ordered", "phase", "scope_path", "validate_graphs")
