# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Niagara script graphs on the shared graph component: Body ``Graph`` <-> GraphDoc, the schema adapter and the
executor that edits one script graph (a module / function / dynamic input asset, or one scratch pad of a system).

Body ``Graph`` (design §3.5; fx.niagara.read.v1 rows shape it, ``niagara_shape``)::

    {"Nodes": [{"Id", "Action", "Position": [x, y], "Version"?, "Comment"?, "Properties"?, "Pins"?}],
     "Links": [{"From": "Id:Pin", "To": "Id:Pin"}],
     "PinDefaults": {"Id:Pin": "<value>"}}

Node ids are the node objects' names (the AssetDocumentNiagara graph adapters rename a new node to its id).  The
document is complete: nodes, links and pin defaults it leaves out are removed / reset.  ``Properties`` are sparse
(a listed value is written, an omitted one is kept).  ``Pins`` lists the pins the user added on a node with an Add pin
(Map Get / Set, custom HLSL, ...) per direction it states: ``{"Inputs"?: [..], "Outputs"?: [..]}`` with a pin as
``"Name"`` or ``{"Name", "Type"?}`` (a new pin needs a type unless the graph already has a parameter of that name).

Every node setting goes through ``graph.node.property.set.v1`` (container ``node``): ``Name`` (object name),
``Pins``, ``Version`` (a function call's script version, "" = exposed) and ``Properties/<name>`` (node Details);
the schema adapter reports the same values as ``extras`` of graph.query.
"""
from __future__ import annotations

from types import MappingProxyType
from typing import Mapping

from asset_document.diagnostics import contract_error
from asset_document.graphs.executor import GraphExecutor, ExecutionReport
from asset_document.graphs.live import LiveAsset, LiveGraph, LiveNode, list_graphs, query_graphs
from asset_document.graphs.model import (
    Endpoint, GraphDoc, LinkDoc, NodeDoc, NodeProperties, PinOverride, Position, thaw_json,
)
from asset_document.graphs.reconcile import Plan, reconcile
from asset_document.graphs.schemas import SchemaAdapter

TAG = "Niagara"
ROLE = "other"
NODE_KEYS = frozenset({"Id", "Action", "Position", "Version", "Comment", "Properties", "Pins"})
GRAPH_KEYS = frozenset({"Nodes", "Links", "PinDefaults"})
# Function calls of a script (asset path or scratch pad): they have a version setting.
_VERSIONED_PREFIX = "Niagara:Function:"
_UNVERSIONED = ("Niagara:Function:Signature:", "Niagara:Function:ScratchPad:")


def _object(value: object, path: str) -> dict:
    if type(value) is not dict:
        raise contract_error(path, "Graph.InvalidType", "Expected a JSON object.")
    return value


def _array(value: object, path: str) -> list:
    if type(value) is not list:
        raise contract_error(path, "Graph.InvalidType", "Expected a JSON array.")
    return value


def _known(row: dict, allowed: frozenset, path: str) -> None:
    for key in row:
        if key not in allowed:
            raise contract_error(f"{path}/{key}", "Graph.UnknownField",
                                 f"Unknown field '{key}'. Allowed: {', '.join(sorted(allowed))}.")


def _rebase(error, path: str):
    diagnostic = error.diagnostic
    if diagnostic.path.startswith("/Body"):
        return error
    suffix = "" if diagnostic.path == "/" else diagnostic.path
    return contract_error(path + suffix, diagnostic.code, diagnostic.message)


def has_version_setting(action: str | None) -> bool:
    return bool(action) and action.startswith(_VERSIONED_PREFIX) and not action.startswith(_UNVERSIONED)


# ---------------------------------------------------------------------------
# Pins
# ---------------------------------------------------------------------------
def _pin_entries(value: object, path: str) -> list[dict]:
    output = []
    for index, item in enumerate(_array(value, path)):
        if type(item) is str and item:
            output.append({"Name": item})
            continue
        row = _object(item, f"{path}/{index}")
        _known(row, frozenset({"Name", "Type"}), f"{path}/{index}")
        if type(row.get("Name")) is not str or not row["Name"]:
            raise contract_error(f"{path}/{index}/Name", "Graph.MissingField", "A pin needs a Name.")
        if "Type" in row and (type(row["Type"]) is not str or not row["Type"]):
            raise contract_error(f"{path}/{index}/Type", "Graph.InvalidType", "Type is a Niagara type token.")
        output.append(dict(row))
    names = [item["Name"] for item in output]
    if len(set(names)) != len(names):
        raise contract_error(path, "Graph.DuplicatePin", "Each pin is listed once.")
    return output


def check_pins(value: object, path: str) -> dict:
    row = _object(value, path)
    _known(row, frozenset({"Inputs", "Outputs"}), path)
    return {key: _pin_entries(row[key], f"{path}/{key}") for key in ("Inputs", "Outputs") if key in row}


def pins_hold(live: object, wanted: object) -> bool:
    """Does the live ``Pins`` value hold what the document states?  Directions the document leaves out are
    kept; a pin the document lists without a Type matches any type."""
    if not isinstance(live, Mapping) or not isinstance(wanted, Mapping):
        return False
    for direction, pins in wanted.items():
        have = list(live.get(direction, ()))
        pins = list(pins)
        if len(have) != len(pins):
            return False
        for got, want in zip(have, pins):
            want = {"Name": want} if type(want) is str else want
            if not isinstance(got, Mapping) or got.get("Name") != want.get("Name"):
                return False
            if "Type" in want and got.get("Type") != want["Type"]:
                return False
    return True


# ---------------------------------------------------------------------------
# Codec
# ---------------------------------------------------------------------------
def _position(value: object, path: str) -> Position:
    if type(value) is not list or len(value) != 2 or any(type(item) not in (int, float) for item in value):
        raise contract_error(path, "Graph.InvalidPosition", "Position is [x, y].")
    return Position(value[0], value[1])


def _endpoint(value: object, path: str) -> Endpoint:
    if type(value) is not str:
        raise contract_error(path, "Graph.InvalidLink", "An endpoint is \"NodeId:PinName\".")
    node, sep, pin = value.partition(":")
    if not sep or not pin:
        raise contract_error(path, "Graph.InvalidLink", "An endpoint is \"NodeId:PinName\".")
    try:
        return Endpoint(node, pin)
    except Exception as error:
        if hasattr(error, "diagnostic"):
            raise _rebase(error, path) from None
        raise


def _node(value: object, path: str, overrides: list[PinOverride]) -> NodeDoc:
    row = _object(value, path)
    _known(row, NODE_KEYS, path)
    for key in ("Id", "Action"):
        if type(row.get(key)) is not str or not row[key]:
            raise contract_error(f"{path}/{key}", "Graph.MissingField", f"{key} is a non-empty string.")
    action = row["Action"]
    settings: dict = {"Name": row["Id"]}
    pins = check_pins(row["Pins"], f"{path}/Pins") if "Pins" in row else None
    if "Version" in row:
        if type(row["Version"]) is not str:
            raise contract_error(f"{path}/Version", "Graph.InvalidType", "Version is a version guid or \"M.m\".")
        if not has_version_setting(action):
            raise contract_error(f"{path}/Version", "Graph.InvalidArgument", "Only a call of a script asset has a Version.")
    if has_version_setting(action):
        settings["Version"] = row.get("Version", "")
    properties = row.get("Properties", {})
    for key, item in _object(properties, f"{path}/Properties").items():
        settings[f"Properties/{key}"] = item
    if pins is not None:
        # After the Details (e.g. a Select node's selector type, which types its pins), as a user sets them first.
        settings["Pins"] = pins
    comment = row.get("Comment", "")
    if type(comment) is not str:
        raise contract_error(f"{path}/Comment", "Graph.InvalidType", "Comment is a string.")
    try:
        return NodeDoc(id=row["Id"], action=action,
                       position=_position(row["Position"], f"{path}/Position") if "Position" in row else None,
                       pin_overrides=tuple(overrides), properties=NodeProperties(node=settings), comment=comment)
    except Exception as error:
        if hasattr(error, "diagnostic"):
            raise _rebase(error, path) from None
        raise


def decode_graph(value: object, path: str, *, graph_id: str, name: str, guid: str | None) -> GraphDoc:
    """Body ``Graph`` -> GraphDoc (the live graph's name and guid: one graph per script)."""
    row = _object(value, path)
    _known(row, GRAPH_KEYS, path)
    defaults: dict[str, list[PinOverride]] = {}
    for key, item in _object(row.get("PinDefaults", {}), f"{path}/PinDefaults").items():
        where = f"{path}/PinDefaults/{key}"
        node, sep, pin = key.partition(":")
        if not sep or not pin or type(item) is not str:
            raise contract_error(where, "Graph.InvalidPinOverride", "PinDefaults maps \"NodeId:Pin\" to a string value.")
        defaults.setdefault(node, []).append(PinOverride(pin, "input", value=item))
    nodes = []
    for index, item in enumerate(_array(row.get("Nodes", []), f"{path}/Nodes")):
        node_id = item.get("Id") if type(item) is dict else None
        nodes.append(_node(item, f"{path}/Nodes/{index}", defaults.pop(node_id, []) if type(node_id) is str else []))
    if defaults:
        missing = sorted(defaults)[0]
        raise contract_error(f"{path}/PinDefaults", "Graph.UnknownNode", f"PinDefaults names node '{missing}', which Nodes does not list.")
    links = []
    for index, item in enumerate(_array(row.get("Links", []), f"{path}/Links")):
        where = f"{path}/Links/{index}"
        link = _object(item, where)
        _known(link, frozenset({"From", "To"}), where)
        links.append(LinkDoc(_endpoint(link.get("From"), f"{where}/From"), _endpoint(link.get("To"), f"{where}/To")))
    try:
        return GraphDoc(id=graph_id, role=ROLE, name=name, schema_tag=TAG, guid=guid, nodes=tuple(nodes),
                        links=tuple(links), path=path)
    except Exception as error:
        if hasattr(error, "diagnostic"):
            raise _rebase(error, path) from None
        raise


def check_graph(value: object, path: str) -> None:
    """Validate a Body ``Graph`` without a live graph (canonicalize / preflight)."""
    decode_graph(value, path, graph_id="Graph", name="Graph", guid=None)


# ---------------------------------------------------------------------------
# Adapter
# ---------------------------------------------------------------------------
class NiagaraSchemaAdapter(SchemaAdapter):
    """Node identity = object name; settings come from graph.query extras."""

    tag = TAG
    implicit_ids = {}

    def normalize_action(self, key: str, *, self_class: str | None, path: str) -> str:
        if type(key) is not str or ":" not in key or not key.strip() or key != key.strip():
            raise contract_error(path, "Graph.InvalidAction",
                                 "A Niagara action is a key from graph.actions.enumerate.v1, e.g. \"Niagara:MapGet\".")
        return key

    def semantic_key(self, node: NodeDoc, normalized_action: str | None) -> tuple | None:
        return None if normalized_action is None else (normalized_action.partition("#")[0], node.id)

    def live_semantic_key(self, node: LiveNode) -> tuple | None:
        return None if node.action_key is None else (node.action_key, self.live_name(node))

    def live_name(self, node: LiveNode) -> str | None:
        value = node.extras.get("object")
        return value if type(value) is str and value else None

    def extract_name(self, node: LiveNode) -> str | None:
        return self.live_name(node)

    def live_properties(self, node: LiveNode, container: str) -> Mapping[str, object]:
        if container != "node":
            return MappingProxyType({})
        output: dict = {"Name": self.live_name(node)}
        if "pins" in node.extras:
            output["Pins"] = thaw_json(node.extras["pins"])
        if has_version_setting(node.action_key):
            version = node.extras.get("version")
            output["Version"] = version if type(version) is str else ""
        for key, value in thaw_json(node.extras.get("properties") or {}).items():
            output[f"Properties/{key}"] = value
        return output

    def extract_action(self, key: str | None, *, self_class: str | None = None) -> str | None:
        return key

    def node_base_id(self, node: LiveNode) -> str:
        return self.live_name(node) or super().node_base_id(node)


NIAGARA_ADAPTER = NiagaraSchemaAdapter()


def bind_by_action(doc: GraphDoc, live: LiveGraph | None, adapter: SchemaAdapter = NIAGARA_ADAPTER) -> GraphDoc:
    """Nodes the document names differently from the live ones (an AI document reusing the nodes a new script starts
    with: its input map, Map Get / Set and output) keep the live node when the action is unique among the unmatched
    nodes on both sides; the node is then renamed to the document id."""
    if live is None:
        return doc
    names = {(node.action_key, adapter.live_name(node)) for node in live.nodes}
    unmatched_doc = [node for node in doc.nodes if node.guid is None and (node.action, node.id) not in names]
    claimed = {(node.action, node.id) for node in doc.nodes}
    unmatched_live = [node for node in live.nodes if (node.action_key, adapter.live_name(node)) not in claimed]
    bound = {}
    for node in unmatched_doc:
        same_doc = [item for item in unmatched_doc if item.action == node.action]
        same_live = [item for item in unmatched_live if item.action_key == node.action]
        if len(same_doc) == 1 and len(same_live) == 1:
            bound[node.id] = same_live[0].guid
    if not bound:
        return doc
    nodes = tuple(NodeDoc(id=node.id, action=node.action, implicit=node.implicit, guid=bound.get(node.id, node.guid),
                          parent=node.parent, position=node.position, pin_overrides=node.pin_overrides,
                          properties=node.properties, name=node.name, comment=node.comment, extras=node.extras,
                          user_pins=node.user_pins, pin_types=node.pin_types) for node in doc.nodes)
    return GraphDoc(id=doc.id, role=doc.role, name=doc.name, schema_tag=doc.schema_tag, guid=doc.guid, nodes=nodes,
                    links=doc.links, subgraphs=doc.subgraphs, signature=doc.signature, owner=doc.owner, path=doc.path)


class NiagaraGraphExecutor(GraphExecutor):
    """Edits the one graph at ``graph_path`` (``Graph`` for a script asset, ``/<pad>`` / ``<emitter>/<pad>`` for a
    system's scratch pads)."""

    def __init__(self, context, *, read_handle, write_handle=None, route_id: str, graph_path: str):
        super().__init__(context, read_handle=read_handle, write_handle=write_handle, route_id=route_id,
                         adapter=NIAGARA_ADAPTER)
        self.graph_path = graph_path

    def live_graph(self) -> LiveGraph | None:
        infos = [info for info in list_graphs(lambda values: self.invoke("graph.list.v1", values))
                 if info.path == self.graph_path]
        if not infos:
            self.live = LiveAsset()
            return None
        rows = query_graphs(self._query, graphs=[{"graph_guid": infos[0].guid}], include_properties=False,
                            infos=(infos[0],))
        self.live = LiveAsset(rows)
        self.include_properties = False
        self.dirty.clear()
        return rows[0] if rows else None

    def plan(self, body_graph: object, path: str, graph_id: str = "Graph", *, hold_default_pins: bool = False) -> Plan:
        live = self.live_graph()
        if live is None:
            raise contract_error(path, "Graph.GraphNotFound", f"The asset has no script graph '{self.graph_path}'.")
        doc = bind_by_action(decode_graph(body_graph, path, graph_id=graph_id, name=live.name, guid=live.guid), live)
        plan = reconcile((doc,), self.live, managed_roles=frozenset({ROLE}), adapter=self.adapter)
        if hold_default_pins:
            plan = _hold_default_pins(plan)
        return _drop_equal_pins(_pins_last(plan), live)

    def _query(self, values: dict, op=None) -> object:
        return alias_default_pins(super()._query(values, op))

    def _op_graph_pin_break(self, op) -> None:
        # A pin the node's Pins setting removed earlier in the plan (properties run before link breaks) took its
        # links with it, as removing the pin in the editor does: nothing is left to break.
        for end in (op.args["from"], op.args["to"]):
            node = self.live_node(op.graph, self.node_guid(op.graph, end, op), op)
            if node.find_pin(end["pin"], end["direction"]) is None:
                return
        super()._op_graph_pin_break(op)

    def apply(self, body_graph: object, path: str, graph_id: str = "Graph", *,
              hold_default_pins: bool = False) -> ExecutionReport | None:
        """Reconcile the graph with the document; None when nothing differs.  With ``hold_default_pins`` the links
        into and values of Map Get default pins wait (see ``_hold_default_pins``)."""
        plan = self.plan(body_graph, path, graph_id, hold_default_pins=hold_default_pins)
        if plan.empty:
            return None
        return self.execute(plan)


def alias_default_pins(result: object) -> object:
    """Map Get default value pins have no name (several "None" inputs on one node): graph.query reports them under
    their output pin's name (the node's extras ``default_pins``, in pin order), as Body ``PinDefaults`` / ``Links``
    name them; the AssetDocumentNiagara adapter resolves that alias (ResolvePinAlias).  A default pin whose value is
    the parameter's Default (not in ``custom_default_pins``) reads as holding its generated default: never reset."""
    result = thaw_json(result)
    if type(result) is not dict or type(result.get("graphs")) is not list:
        return result
    graphs = []
    for graph in result["graphs"]:
        if type(graph) is not dict:
            graphs.append(graph)
            continue
        aliases = {}
        nodes = []
        for node in graph.get("nodes", []):
            extras = node.get("extras") if type(node) is dict else None
            node = _mark_type_default_pins(node, extras)
            names = extras.get("default_pins") if type(extras) is dict else None
            if type(names) is not list or type(node.get("pins")) is not list:
                nodes.append(node)
                continue
            custom = set(extras.get("custom_default_pins") or ())
            pins, index = [], 0
            for pin in node["pins"]:
                if type(pin) is dict and pin.get("direction") == "input" and pin.get("name") in ("None", ""):
                    name = names[index] if index < len(names) else ""
                    index += 1
                    if type(name) is str and name:
                        pin = dict(pin, name=name)
                        if name not in custom:
                            pin["is_autogenerated_default"] = True
                        for linked in pin.get("linked_to") or ():
                            if type(linked) is dict:
                                aliases[(node.get("node_guid"), linked.get("node_guid"), linked.get("pin_name"))] = name
                pins.append(pin)
            nodes.append(dict(node, pins=pins))
        links = []
        for link in graph.get("links", []):
            to = link.get("to") if type(link) is dict else None
            source = link.get("from") if type(link) is dict else None
            if type(to) is dict and type(source) is dict and to.get("pin_name") in ("None", ""):
                name = aliases.get((to.get("node_guid"), source.get("node_guid"), source.get("pin_name")))
                if name:
                    link = dict(link, to=dict(to, pin_name=name))
            links.append(link)
        graphs.append(dict(graph, nodes=nodes, links=links))
    return dict(result, graphs=graphs)


def _mark_type_default_pins(node: object, extras: object) -> object:
    """Pins of older assets that hold their type's default with no autogenerated default recorded (extras
    ``type_default_pins``) read as holding their generated default, as Body ``PinDefaults`` leaves them out."""
    names = extras.get("type_default_pins") if type(extras) is dict else None
    if type(names) is not list or not names or type(node.get("pins")) is not list:
        return node
    wanted = set(names)
    pins = [dict(pin, is_autogenerated_default=True)
            if type(pin) is dict and pin.get("direction") == "input" and pin.get("name") in wanted else pin
            for pin in node["pins"]]
    return dict(node, pins=pins)


def _pins_last(plan: Plan) -> Plan:
    """A node's ``Pins`` after its other settings (Details such as a static switch's or Select node's type, which
    make its pins, and ``Version``), as a user sets them first; the settings otherwise run in key order."""
    ops = list(plan.ops)
    start = 0
    while start < len(ops):
        end = start
        op = ops[start]
        while (end < len(ops) and ops[end].op == "graph.node.property.set" and op.op == "graph.node.property.set"
               and ops[end].graph == op.graph and ops[end].args.get("node") == op.args.get("node")):
            end += 1
        if end - start > 1:
            ops[start:end] = sorted(ops[start:end], key=lambda item: item.args.get("property_path") == "Pins")
        start = max(end, start + 1)
    return Plan(tuple(ops), plan.graphs, plan.node_bindings, plan.graph_bindings)


def _hold_default_pins(plan: Plan) -> Plan:
    """Without the links into and values of Map Get default pins: such a pin takes them once its parameter's
    Default Mode is Custom (the parameter's Details, set after the graph), as in the editor, where the pin shows
    only then.  The caller applies the full graph again after the parameters."""
    actions = {(doc.id, node.id): node.action for doc in plan.graphs for node in doc.nodes}

    def held(graph: str, end: object) -> bool:
        return (type(end) is dict and end.get("direction", "input") == "input" and end.get("pin") != "Source"
                and actions.get((graph, end.get("node"))) == "Niagara:MapGet")

    ops = tuple(op for op in plan.ops
                if not (op.op == "graph.pin.connect" and held(op.graph, op.args.get("to")))
                and not (op.op == "graph.pin.default.set" and held(op.graph, op.args.get("pin"))))
    return Plan(ops, plan.graphs, plan.node_bindings, plan.graph_bindings)


def _drop_equal_pins(plan: Plan, live: LiveGraph) -> Plan:
    """Pins settings the live node already holds (types the document leaves out are not compared)."""
    by_guid = {node.guid: node for node in live.nodes}
    ops = []
    for op in plan.ops:
        if op.op == "graph.node.property.set" and op.args.get("property_path") == "Pins":
            guid = plan.node_bindings.get((op.graph, op.args.get("node")))
            node = by_guid.get(guid) if guid else None
            if node is not None and pins_hold(NIAGARA_ADAPTER.live_properties(node, "node").get("Pins"), op.args["value"]):
                continue
        ops.append(op)
    if all(op.op == "graph.finalize" for op in ops):
        ops = []
    return Plan(tuple(ops), plan.graphs, plan.node_bindings, plan.graph_bindings)


__all__ = ("NIAGARA_ADAPTER", "NiagaraGraphExecutor", "NiagaraSchemaAdapter", "bind_by_action", "check_graph",
           "check_pins", "decode_graph", "has_version_setting", "pins_hold")
