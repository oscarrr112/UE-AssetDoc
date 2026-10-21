"""Execute a reconcile Plan through ``context.invoke`` (graph primitives v1).

The executor owns the doc-id -> GUID binding table, re-queries a graph after
structural operations (pins can change on ReconstructNode), resolves pin
names at call time and maps every primitive diagnostic back to the Body JSON
pointer of the plan operation so the AI sees *its* mistake, not a DTO path.
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass

from asset_document.diagnostics import (
    AssetDocumentContractError, DiagnosticContext, DiagnosticSeverity, RuntimeDiagnostic, contract_error,
)
from asset_document.dto import ChangedRecord, PrimitiveRequirement
from asset_document.graphs.live import LiveAsset, LiveGraph, LiveGraphInfo, LiveNode, decode_class_chain, \
    list_graphs, query_graphs
from asset_document.graphs.extract import extract_graphs
from asset_document.graphs.layout import live_positions
from asset_document.graphs.model import GraphDoc, thaw_json
from asset_document.graphs.actions import action_kind, split_disambiguator
from asset_document.graphs.reconcile import CLASS_PIN_CATEGORIES, Plan, PlanOp, reconcile, reconcile_graph, \
    user_pins_equal
from asset_document.graphs.schemas import GENERIC_ADAPTER, SchemaAdapter
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument, PrimitiveOutcome, PrimitiveOutcomeStatus


GRAPH_PRIMITIVES = (
    "graph.list.v1", "graph.query.v1", "graph.actions.enumerate.v1", "graph.node.spawn.v1", "graph.node.remove.v1",
    "graph.pin.connect.v1", "graph.pin.break.v1", "graph.pin.default.set.v1", "graph.node.property.get.v1",
    "graph.node.property.set.v1", "graph.node.layout.set.v1", "graph.node.rename.v1", "graph.node.user_pins.set.v1",
    "graph.node.dynamic_pin.add.v1", "graph.node.dynamic_pin.remove.v1", "graph.create.v1", "graph.remove.v1",
    "graph.rename.v1", "graph.function.flags.set.v1", "graph.finalize.v1", "graph.node.pin_type.set.v1",
)
# Read-only: the links the editor would make only through a conversion node (Validate).
LINK_CHECK = "graph.link.check.v1"
MAX_DYNAMIC_PIN_ADDS = 32
# graph.query.v1 node_limit used after Graph.ResultTooLarge; halved on every further refusal.
QUERY_NODE_LIMIT = 256
# graph.finalize.v1 node_meta entries per invocation (each entry is up to 4 of the 4096 values one invocation carries).
NODE_META_BATCH = 512


def conversion_message(output_end: str, input_end: str, facts: dict) -> str:
    """Graph.ConnectNeedsConversion in document terms: the conversion node to add and the two links through it.

    ``facts`` are the primitive's context (from_type, to_type, reason, conversion_action, conversion_input,
    conversion_output)."""
    from_type, to_type = facts.get("from_type") or "?", facts.get("to_type") or "?"
    reason = facts.get("reason") or "conversion"
    head = (f"{output_end} ({from_type}) -> {input_end} ({to_type}) needs a conversion node ({reason}). The editor "
            "inserts it when the link is dragged; a document only has the nodes it lists.")
    action = facts.get("conversion_action")
    if not action:
        return f"{head} Add that conversion node and link through it, or link a pin of type {to_type}."
    node_id = action.rsplit(":", 1)[-1] if action.startswith("K2:CallFunction:") else "Convert"
    node = f'{{"Id": "{node_id}", "Action": "{action}"}}'
    pins = (facts.get("conversion_input"), facts.get("conversion_output"))
    if all(pins):
        links = (f'{{"From": "{output_end}", "To": "{node_id}:{pins[0]}"}} and '
                 f'{{"From": "{node_id}:{pins[1]}", "To": "{input_end}"}}')
        return f"{head} Add the node {node} with the links {links} instead, or link a pin of type {to_type}."
    return f"{head} Add the node {node} and link through it, or link a pin of type {to_type}."


def pin_default_message(node: str, pin: str, kind: str, value: str, message: str, facts: dict) -> str:
    """Graph.PinDefaultInvalid in document terms: an input that takes no literal (a by-ref or container input)
    gets the node to link into it instead.

    ``facts`` are the primitive's context (needs_link, pin_type, make_action, make_input, make_output)."""
    need = facts.get("needs_link")
    if need not in ("by_ref", "container"):
        return message
    pin_type = facts.get("pin_type") or "?"
    end = f"{node}:{pin}"
    head = (f"{message} {end} ({pin_type}) is a {'by-ref' if need == 'by_ref' else 'container'} input: it takes no "
            "literal in PinOverrides, only a link.")
    action, output = facts.get("make_action"), facts.get("make_output")
    if not action or not output:
        return f"{head} Link a variable or another node's output of type {pin_type} into it."
    node_id = node + "_" + re.sub(r"\W", "", pin)
    entry = f'"Id": "{node_id}", "Action": "{action}"'
    if facts.get("make_input"):
        field = "DefaultTextValue" if kind == "text" else "DefaultValue"
        entry += f', "PinOverrides": [{{"Pin": "{facts["make_input"]}", "{field}": {json.dumps(value)}}}]'
        then = ""
    else:
        then = " and link the values into it"
    link = f'{{"From": "{node_id}:{output}", "To": "{end}"}}'
    return f"{head} Add the node {{{entry}}} with the link {link}{then} instead, or link a variable of type {pin_type}."


def graph_requirements(route_id: str) -> tuple[PrimitiveRequirement, ...]:
    """The graph primitive requirements a Profile route declares."""
    return tuple(PrimitiveRequirement(primitive_id, 1, route_id) for primitive_id in GRAPH_PRIMITIVES)


@dataclass(frozen=True, slots=True)
class ExecutionReport:
    node_bindings: dict
    graph_bindings: dict
    changed_set: tuple[ChangedRecord, ...]
    diagnostics: tuple[RuntimeDiagnostic, ...]
    finalize_status: str | None


def _value(row: object, key: str, op_path: str, kind: type = str):
    if type(row) is not dict or type(row.get(key)) is not kind:
        raise contract_error(op_path, "Graph.InvalidPrimitiveResult", f"Primitive result is missing '{key}'.")
    return row[key]


class GraphExecutor:
    def __init__(self, context, *, read_handle, write_handle=None, route_id: str,
                 adapter: SchemaAdapter = GENERIC_ADAPTER, self_class: str | None = None):
        self.context = context
        self.read_handle = read_handle
        self.write_handle = write_handle
        self.adapter = adapter
        self.self_class = self_class
        self.requirements = {item.primitive_id: item for item in graph_requirements(route_id)}
        self.live = LiveAsset()
        self.dirty: set[str] = set()
        # Graphs whose snapshot still has every node and pin, but maybe not the pin types, defaults or
        # new links that the editor gave since (a link to a wildcard pin, a Pin Conversion): re-read only
        # when an op needs those (refreshing a large graph after each link is quadratic).
        self.stale: set[str] = set()
        # Links waiting for a wildcard pin's type: (op, error if it never comes, the wildcards' nodes).
        self.deferred: list[tuple[PlanOp, AssetDocumentContractError | None, frozenset[str]]] = []
        self.touched: set[str] = set()  # nodes the last links were made to
        self.nodes: dict[tuple[str, str], str] = {}
        self.removed: set[str] = set()
        self.graphs: dict[str, str] = {}
        self.docs: dict[str, GraphDoc] = {}
        # Optional callable(executor) that binds document node ids to live GUIDs on demand (bare compile in repair).
        self.binder = None
        self.parents: dict[str, GraphDoc] = {}
        self.changed: list[ChangedRecord] = []
        self.diagnostics: list[RuntimeDiagnostic] = []
        self.finalize_status: str | None = None
        self.include_properties = False
        self.node_limit: int | None = None
        self.adopted: set[tuple[str, str]] = set()

    # Transport ---------------------------------------------------------
    def invoke(self, primitive: str, values: dict, op: PlanOp | None = None, *, write: bool = False) -> object:
        handle = self.write_handle if write else self.read_handle
        if handle is None:
            raise contract_error(op.path if op else "/Body", "Graph.HandleUnavailable",
                                 f"{primitive} needs a {'mutable' if write else 'read-only'} asset handle.")
        outcome = self.context.invoke(
            requirement=self.requirements[primitive], value_arguments={"schema_version": 1, **values},
            handle_arguments=(NamedOpaqueHandleArgument("asset", handle),))
        if type(outcome) is not PrimitiveOutcome:
            raise contract_error(op.path if op else "/Body", "Graph.InvalidPrimitiveResult",
                                 "A typed primitive outcome is required.")
        if outcome.status is not PrimitiveOutcomeStatus.SUCCEEDED:
            raise self._mapped(outcome, primitive, op)
        self.changed.extend(outcome.changed_set)
        return outcome.value

    def _mapped(self, outcome: PrimitiveOutcome, primitive: str, op: PlanOp | None) -> AssetDocumentContractError:
        source = outcome.diagnostics[0] if outcome.diagnostics else RuntimeDiagnostic(
            "/PrimitiveOutcome", "Graph.PrimitiveFailed", f"{primitive} did not succeed.")
        context = source.context + (DiagnosticContext("primitive", primitive),
                                    DiagnosticContext("primitive_path", source.path))
        if op is not None and op.op != "graph.finalize":
            context += (DiagnosticContext("operation", op.op),)
        return AssetDocumentContractError(RuntimeDiagnostic(
            op.path if op else "/Body", source.code, source.message, source.severity, context))

    # Live state --------------------------------------------------------
    def _query(self, values: dict, op: PlanOp | None = None) -> object:
        """graph.query.v1 with a node_limit fallback when the runtime value budget refuses the result."""
        while True:
            request = dict(values)
            if self.node_limit is not None:
                request["node_limit"] = self.node_limit
            try:
                return self.invoke("graph.query.v1", request, op)
            except AssetDocumentContractError as error:
                if error.diagnostic.code != "Graph.ResultTooLarge":
                    raise
                if self.node_limit == 1:
                    raise AssetDocumentContractError(RuntimeDiagnostic(
                        error.diagnostic.path, error.diagnostic.code,
                        "A single graph node exceeds the runtime value budget even when graphs are read one node "
                        "per call, so the document cannot be read whole; this asset needs a profile that reads "
                        "the node in parts.", error.diagnostic.severity, error.diagnostic.context)) from None
                self.node_limit = QUERY_NODE_LIMIT if self.node_limit is None else max(1, self.node_limit // 2)

    def read_live(self, *, include_properties: bool = False, include_sizes: bool = False) -> LiveAsset:
        """Every graph of the asset; ``include_sizes`` also reads each node's drawn size (a relayout)."""
        listed: list = []

        def list_page(values: dict) -> object:
            value = self.invoke("graph.list.v1", values)
            listed.append(value)
            return value

        infos = list_graphs(list_page)
        self.live = LiveAsset(query_graphs(self._query, include_properties=include_properties, infos=infos,
                                           include_sizes=include_sizes),
                              decode_class_chain(listed[-1]) if listed else ())
        self.include_properties = include_properties
        self.dirty.clear()
        return self.live

    def _info(self, graph: LiveGraph) -> LiveGraphInfo:
        return LiveGraphInfo(graph.guid, graph.name, graph.path, graph.role, "", graph.schema_tag,
                             graph.parent_guid, graph.owner_node_guid, graph.caps)

    def _refresh(self, guid: str, info: LiveGraphInfo | None = None, *, include_sizes: bool = False) -> LiveGraph:
        existing = self.live.graph(guid)
        info = info or (self._info(existing) if existing else None)
        rows = query_graphs(self._query, graphs=[{"graph_guid": guid}], include_properties=self.include_properties,
                            infos=(info,) if info else (), include_sizes=include_sizes)
        if len(rows) != 1 or rows[0].guid != guid:
            raise contract_error("/Body", "Graph.InvalidPrimitiveResult", "graph.query must return the requested graph.")
        graph = rows[0]
        self.live = self.live.replace(graph)
        self.dirty.discard(guid)
        self.stale.discard(guid)
        return graph

    def graph(self, graph_id: str, op: PlanOp) -> LiveGraph:
        guid = self.graphs.get(graph_id)
        if guid is None:
            raise contract_error(op.path, "Graph.GraphNotFound", f"Graph '{graph_id}' does not exist in the asset.")
        graph = self.live.graph(guid)
        if graph is None or guid in self.dirty:
            graph = self._refresh(guid)
        return graph

    def node_guid(self, graph_id: str, ref: dict, op: PlanOp) -> str:
        if "node_guid" in ref:
            return ref["node_guid"]
        key = (graph_id, ref["node"])
        if key in self.nodes:
            return self.nodes[key]
        doc = self.docs.get(graph_id)
        node = next((item for item in doc.nodes if item.id == ref["node"]), None) if doc else None
        if node is not None and node.implicit is not None:
            claimed = {guid for (graph, _), guid in self.nodes.items() if graph == graph_id}
            found = next((n for n in self.graph(graph_id, op).nodes
                          if n.implicit_role == node.implicit and n.guid not in claimed), None)
            if found is not None:
                self.nodes[key] = found.guid
                return found.guid
        raise contract_error(op.path, "Graph.NodeNotFound", f"Node '{ref['node']}' has no live node to act on.")

    def _fresh(self, graph_id: str) -> bool:
        """Re-read a stale graph at its next use; True when it was stale."""
        guid = self.graphs.get(graph_id)
        if guid in self.stale:
            self.dirty.add(guid)
            return True
        return False

    def live_node(self, graph_id: str, guid: str, op: PlanOp) -> LiveNode:
        node = self.graph(graph_id, op).node(guid)
        if node is None:
            raise contract_error(op.path, "Graph.NodeNotFound", f"Node {guid} disappeared from the graph.")
        return node

    def pin(self, graph_id: str, ref: dict, op: PlanOp, *, add_missing: bool = True) -> dict:
        guid = self.node_guid(graph_id, ref, op)
        node = self.live_node(graph_id, guid, op)
        pin = node.find_pin(ref["pin"], ref["direction"])
        if pin is None and self._fresh(graph_id):
            node = self.live_node(graph_id, guid, op)
            pin = node.find_pin(ref["pin"], ref["direction"])
        # A member of a pin the node has ("B_Z" of a struct or wildcard B) is not a pin to add: adding pins
        # until one is named so would grow the node by every pin it can take (AnimatedMannyPawnExtended's
        # Subtract, linked into B_Z).
        member = pin is None and any(p.direction == ref["direction"] and p.type.category in ("struct", "wildcard")
                                     and p.type.container == "none"
                                     and ref["pin"].casefold().startswith(p.name.casefold() + "_") for p in node.pins)
        adds = 0
        while pin is None and add_missing and not member and node.can_add_pin and adds < MAX_DYNAMIC_PIN_ADDS:
            self.invoke("graph.node.dynamic_pin.add.v1", {"node_guid": guid}, op, write=True)
            self.dirty.add(self.graphs[graph_id])
            node, adds = self.live_node(graph_id, guid, op), adds + 1
            pin = node.find_pin(ref["pin"], ref["direction"])
        if pin is None and any(p.direction == ref["direction"] and p.type.category == "struct" and p.type.container == "none"
                               and ref["pin"].casefold().startswith(p.name.casefold() + "_") for p in node.pins):
            # A member of a struct pin that is not split yet ("ReturnValue_X"): the connect / default
            # primitive splits it first ("Split Struct Pin").
            self.dirty.add(self.graphs[graph_id])
            return {"node_guid": guid, "pin_name": ref["pin"], "direction": ref["direction"]}
        if pin is None:
            names = ", ".join(p.name for p in node.pins if p.direction == ref["direction"] and not p.hidden)
            raise AssetDocumentContractError(RuntimeDiagnostic(
                op.path, "Graph.PinNotFound",
                f"Node '{ref.get('node', guid)}' has no {ref['direction']} pin '{ref['pin']}'. Available: {names}."
                + self.adapter.missing_pin_hint(node, ref["pin"]),
                context=(DiagnosticContext("node_guid", guid),)))
        return {"node_guid": guid, "pin_name": pin.name, "direction": pin.direction}

    # Plan execution ------------------------------------------------------
    def run(self, desired: tuple[GraphDoc, ...], *, managed_roles: frozenset[str], role_paths=None) -> ExecutionReport:
        """Read live, reconcile and execute; the normal Profile entry point."""
        include = any(item for graph in desired for sub in graph.walk() for node in sub.nodes
                      for item in node.properties.items())
        relayout = any(sub.layout == "Auto" for graph in desired for sub in graph.walk())
        live = self.read_live(include_properties=include, include_sizes=relayout)
        plan = reconcile(desired, live, managed_roles=managed_roles, adapter=self.adapter,
                         self_class=self.self_class, role_paths=role_paths)
        return self.execute(plan)

    def execute(self, plan: Plan) -> ExecutionReport:
        for graph in plan.graphs:
            self._index(graph, None)
        self.nodes.update(plan.node_bindings)
        self.graphs.update(plan.graph_bindings)
        self._run_ops(plan.ops)
        return ExecutionReport(dict(self.nodes), dict(self.graphs), tuple(self.changed), tuple(self.diagnostics),
                               self.finalize_status)

    def _index(self, graph: GraphDoc, parent: GraphDoc | None) -> None:
        self.docs[graph.id] = graph
        if parent is not None:
            self.parents[graph.id] = parent
        for child in graph.subgraphs:
            self._index(child, graph)

    def _run_ops(self, ops) -> None:
        """Run ops in order; a link waiting for its wildcard pin's type is retried after each later link."""
        for op in ops:
            if op.op != "graph.pin.connect":
                self._flush_deferred()
            self.step(op)
            while op.op == "graph.pin.connect" and self.deferred and self.touched:
                touched, self.touched = self.touched, set()
                self._retry_deferred(touched)
        self._flush_deferred()

    def step(self, op: PlanOp) -> None:
        handler = getattr(self, "_op_" + op.op.replace(".", "_"), None)
        if handler is None:
            raise contract_error(op.path, "Graph.UnknownOperation", f"Unknown plan operation {op.op}.")
        handler(op)

    def _write(self, primitive: str, values: dict, op: PlanOp, graph_id: str | None = None):
        result = self.invoke(primitive, values, op, write=True)
        if graph_id is not None and graph_id in self.graphs:
            self.dirty.add(self.graphs[graph_id])
        return result

    def _op_graph_create(self, op: PlanOp) -> None:
        values = {"role": op.args["role"], "name": op.args["name"]}
        parent_guid = self.graphs.get(op.args["parent"]) if "parent" in op.args else None
        if parent_guid is not None:
            values["parent"] = {"graph_guid": parent_guid}
        result = self._write("graph.create.v1", values, op)
        guid = _value(result, "graph_guid", op.path).upper()
        self.graphs[op.graph] = guid
        path = result.get("path", op.args["name"]) if type(result) is dict else op.args["name"]
        live = self._refresh(guid, LiveGraphInfo(guid, op.args["name"], path, op.args["role"], "", self.adapter.tag,
                                                 parent_guid, None, {}))
        self._conform_new_flags(op, live)
        # Nodes the editor placed in the new graph on its own (an override's "Parent: <function>" call,
        # UEdGraphSchema_K2::CreateFunctionGraphTerminators): the document's own nodes are spawned by the
        # plan, so the graph holds exactly what the document lists.
        placed = [node.guid for node in live.nodes if self.adapter.placed_with_graph(node)]
        for node_guid in placed:
            self._write("graph.node.remove.v1", {"node_guid": node_guid}, op, op.graph)
        if placed:
            self.dirty.add(guid)

    def _conform_new_flags(self, op: PlanOp, live: LiveGraph | None) -> None:
        """The plan takes a new function to start with the schema's default flags; a Blueprint type may
        give it others (an editor utility Blueprint's new functions are Call In Editor:
        FBlueprintEditorUtils::AddFunctionGraph). Those the document does not state are set back."""
        defaults = self.adapter.signature_flag_defaults(op.args["role"])
        doc = self.docs.get(op.graph)
        entry = next((node for node in (live.nodes if live is not None else ()) if node.implicit_role == "entry"), None)
        if defaults is None or doc is None or entry is None:
            return
        have = self.adapter.signature_flags(entry)
        wanted = {**defaults, **(thaw_json(doc.signature.flags) if doc.signature is not None else {})}
        changed = {key: value for key, value in defaults.items() if key in have and have[key] != value
                   and wanted.get(key) == value}
        if changed:
            self._write("graph.function.flags.set.v1", {"graph": {"graph_guid": live.guid}, "flags": changed}, op, op.graph)

    def _op_graph_rename(self, op: PlanOp) -> None:
        self._write("graph.rename.v1", {"graph": {"graph_guid": self.graphs[op.graph]}, "name": op.args["name"]}, op,
                    op.graph)

    def _op_graph_remove(self, op: PlanOp) -> None:
        self.invoke("graph.remove.v1", {"graph": {"graph_guid": op.args["graph_guid"]}}, op, write=True)

    def _op_graph_function_flags_set(self, op: PlanOp) -> None:
        self._write("graph.function.flags.set.v1", {"graph": {"graph_guid": self.graph(op.graph, op).guid},
                                                    "flags": dict(op.args["flags"])}, op, op.graph)

    def _op_graph_node_user_pins_set(self, op: PlanOp) -> None:
        if "node" in op.args:
            # A custom event's Inputs; an adopted companion event may already have them.
            guid = self.node_guid(op.graph, op.args, op)
            doc = self.docs[op.graph].node(op.args["node"])
            if doc.user_pins is not None and user_pins_equal(
                    self.adapter.node_user_pins(self.live_node(op.graph, guid, op)), doc.user_pins):
                return
            self._write("graph.node.user_pins.set.v1", {"node_guid": guid, "pins": op.args["pins"]}, op, op.graph)
            return
        role = op.args["implicit"]
        node = next((n for n in self.graph(op.graph, op).nodes if n.implicit_role == role), None)
        if node is None and op.args.get("spawn_action"):
            self._spawn(op, op.args["spawn_action"], None, None)
            node = next((n for n in self.graph(op.graph, op).nodes if n.implicit_role == role), None)
        if node is None:
            raise contract_error(op.path, "Graph.ImplicitNodeMissing", f"The graph has no '{role}' node for this signature.")
        self._write("graph.node.user_pins.set.v1", {"node_guid": node.guid, "pins": op.args["pins"]}, op, op.graph)

    def _spawn(self, op: PlanOp, action_key: str, position, parent_guid) -> str:
        values = {"graph": {"graph_guid": self.graph(op.graph, op).guid}, "action_key": action_key}
        if position is not None:
            values["position"] = position
        if parent_guid is not None:
            values["context"] = {"parent_node_guid": parent_guid}
        result = self._write("graph.node.spawn.v1", values, op, op.graph)
        return _value(result, "node_guid", op.path).upper()

    def _op_graph_node_spawn(self, op: PlanOp) -> None:
        key = (op.graph, op.args["node"])
        if key in self.adopted:
            # Created as another node's companion (e.g. an Assign node's event): only place it.
            if op.args.get("position") is not None:
                self._write("graph.node.layout.set.v1", {"node_guid": self.nodes[key], "position": op.args["position"]},
                            op, op.graph)
            return
        role = op.args.get("adopt_implicit")
        if role is not None:
            # The graph's own node of that role when it came with one (a function override's return node).
            self._fresh(op.graph)
            claimed = {guid for (graph, _), guid in self.nodes.items() if graph == op.graph}
            found = next((n for n in self.graph(op.graph, op).nodes
                          if n.implicit_role == role and n.guid not in claimed), None)
            if found is not None:
                self.nodes[key] = found.guid
                if op.args.get("position") is not None:
                    self._write("graph.node.layout.set.v1", {"node_guid": found.guid, "position": op.args["position"]},
                                op, op.graph)
                return
        parent = self.node_guid(op.graph, {"node": op.args["parent"]}, op) if "parent" in op.args else None
        values = {"graph": {"graph_guid": self.graph(op.graph, op).guid}, "action_key": op.args["action_key"]}
        if op.args.get("position") is not None:
            values["position"] = op.args["position"]
        if parent is not None:
            values["context"] = {"parent_node_guid": parent}
        result = self._write("graph.node.spawn.v1", values, op, op.graph)
        guid = _value(result, "node_guid", op.path).upper()
        self.nodes[key] = guid
        wanted = op.args.get("position")
        placed = result.get("node", {}).get("position") if type(result) is dict and type(result.get("node")) is dict else None
        if wanted is not None and type(placed) is dict and (placed.get("x"), placed.get("y")) != (wanted["x"], wanted["y"]):
            # The spawner placed the node relative to the click (a comment is centred on it): move it
            # to the stated position, as dragging it there in the editor does.
            self._write("graph.node.layout.set.v1", {"node_guid": guid, "position": wanted}, op, op.graph)
        companions = [str(item).upper() for item in result.get("created_node_guids", [])] if type(result) is dict else []
        if companions:
            self._adopt_companions(op, guid, companions)

    def _adopt_companions(self, op: PlanOp, guid: str, companions: list[str]) -> None:
        """Nodes the spawned node created on its own (UK2Node_AssignDelegate places a linked custom event).

        A companion becomes the document node that the document links to the spawned node through
        the same pins (and that is not created yet); a companion the document does not describe is
        removed, so the graph holds exactly what the document lists.
        """
        doc = self.docs.get(op.graph)
        graph = self.graph(op.graph, op)
        node_id = op.args["node"]
        for companion in companions:
            live = graph.node(companion)
            if live is None or action_kind(live.action_key) != "CustomEvent":
                continue  # only a placed custom event is a document-visible companion
            found = None
            for link in (doc.links if doc else ()):
                for mine, other, live_link in ((link.to, link.from_, lambda l: (l.to, l.from_)),
                                               (link.from_, link.to, lambda l: (l.from_, l.to))):
                    if mine.node != node_id or (op.graph, other.node) in self.nodes or found is not None:
                        continue
                    candidate = doc.node(other.node)
                    wanted = self.adapter.normalize_action(candidate.action, self_class=self.self_class,
                                                           path=op.path) if candidate.action else None
                    if wanted is None or action_kind(split_disambiguator(wanted)[0]) != action_kind(live.action_key):
                        continue
                    if any(live_link(item)[0].node_guid == guid and live_link(item)[0].pin_name.casefold() == mine.pin.casefold()
                           and live_link(item)[1].node_guid == companion
                           and live_link(item)[1].pin_name.casefold() == other.pin.casefold() for item in graph.links):
                        found = other.node
            if found is not None:
                self.nodes[(op.graph, found)] = companion
                self.adopted.add((op.graph, found))
            else:
                self._write("graph.node.remove.v1", {"node_guid": companion}, op, op.graph)

    def _op_graph_node_remove(self, op: PlanOp) -> None:
        if op.args["node_guid"] in self.removed:
            return  # already removed with an earlier node (e.g. a state's transitions)
        result = self._write("graph.node.remove.v1", {"node_guid": op.args["node_guid"]}, op, op.graph)
        removed = {str(item).upper() for item in result.get("removed_node_guids", [])} if type(result) is dict else set()
        removed.add(op.args["node_guid"])
        self.removed |= removed
        for key in [key for key, guid in self.nodes.items() if guid in removed]:
            del self.nodes[key]

    def _op_graph_node_property_set(self, op: PlanOp) -> None:
        self._write("graph.node.property.set.v1", {
            "node_guid": self.node_guid(op.graph, op.args, op), "container": op.args["container"],
            "property_path": op.args["property_path"], "value": op.args["value"]}, op, op.graph)

    def _op_graph_node_rename(self, op: PlanOp) -> None:
        guid = self.node_guid(op.graph, op.args, op)
        if self.adapter.live_name(self.live_node(op.graph, guid, op)) == op.args["name"]:
            return  # spawned or adopted under that name already (e.g. "<Dispatcher>_Event")
        self._write("graph.node.rename.v1", {"node_guid": guid, "name": op.args["name"]}, op, op.graph)

    def _op_graph_node_pin_type_set(self, op: PlanOp) -> None:
        guid = self.node_guid(op.graph, op.args, op)
        node = self.live_node(op.graph, guid, op)
        name = op.args["pin"]
        found = node.find_pin(name)
        if found is None and node.can_add_pin:
            # A further operand ("C") is added first, as the author adds it before converting it.
            found = node.find_pin(self.pin(op.graph, {"node": op.args["node"], "pin": name, "direction": "input"}, op)["pin_name"])
        promotable = node.extras.get("promotable") is True
        graph_guid = self.graphs[op.graph]
        current = found.type.to_dto() if found is not None else None
        # (A stale promotable operator: its primitive leaves a pin of the wanted type as it is.)
        if current == op.args["type"] and not (promotable and graph_guid in self.stale):
            return
        self.invoke("graph.node.pin_type.set.v1", {"node_guid": guid, "pin_name": found.name if found else name,
                                                   "type": op.args["type"]}, op, write=True)
        if promotable and found is not None and not found.linked_to:
            self.stale.add(graph_guid)  # a Pin Conversion retypes pins in place (and breaks only that pin's links)
        else:
            self.dirty.add(graph_guid)

    def _op_graph_local_variables_set(self, op: PlanOp) -> None:
        entry = next((node for node in self.graph(op.graph, op).nodes if node.implicit_role == "entry"), None)
        if entry is None:
            raise contract_error(op.path, "Graph.GraphOperationUnsupported",
                                 "Only a graph with an entry node (a function) has local variables.")
        self._write("graph.node.property.set.v1", {"node_guid": entry.guid, "container": "node",
                                                   "property_path": "LocalVariables", "value": op.args["variables"]},
                    op, op.graph)

    def _op_graph_node_dynamic_pin_add(self, op: PlanOp) -> None:
        # pin() adds dynamic pins until the name resolves, or raises a mapped Graph.PinNotFound.
        for name in op.args["pins"]:
            self.pin(op.graph, {"node": op.args["node"], "pin": name, "direction": self._direction_hint(op, name)}, op)

    def _direction_hint(self, op: PlanOp, pin: str) -> str:
        doc = self.docs[op.graph]
        node_id = op.args["node"]
        return "output" if any(link.from_.node == node_id and link.from_.pin == pin for link in doc.links) else "input"

    def _op_graph_pin_break(self, op: PlanOp) -> None:
        self._write("graph.pin.break.v1", {"from": self.pin(op.graph, op.args["from"], op, add_missing=False),
                                           "to": self.pin(op.graph, op.args["to"], op, add_missing=False)},
                    op, op.graph)

    def _retry_deferred(self, touched: set[str], *, final: bool = False) -> None:
        """Retry the links waiting on a node a link was just made to (all of them when ``final``)."""
        waiting, self.deferred = self.deferred, []
        for op, error, guids in waiting:
            if final or guids & touched:
                self._op_graph_pin_connect(op, final=final)
            else:
                self.deferred.append((op, error, guids))

    def _flush_deferred(self) -> None:
        """Once the links that could type their wildcard are made, the waiting links are made as they are
        (or fail with their error)."""
        if self.deferred:
            self._retry_deferred(set(), final=True)
        self.touched = set()

    def _wildcard_member(self, graph_id: str, ref: dict, op: PlanOp) -> str | None:
        """The node whose wildcard pin ``ref`` names a member of (a split struct the pin becomes once linked)."""
        try:
            node = self.live_node(graph_id, self.node_guid(graph_id, ref, op), op)
        except AssetDocumentContractError:
            return None
        return node.guid if any(p.direction == ref["direction"] and p.type.category == "wildcard"
                                and ref["pin"].casefold().startswith(p.name.casefold() + "_") for p in node.pins) else None

    def _op_graph_pin_connect(self, op: PlanOp, *, final: bool = False) -> None:
        source, target = op.args["from"], op.args["to"]
        keep = (op.args.get("keep_from_types"), op.args.get("keep_to_types"))
        try:
            pins = (self.pin(op.graph, source, op), self.pin(op.graph, target, op))
        except AssetDocumentContractError as original:
            waiting = None if final or original.diagnostic.code != "Graph.PinNotFound" else next(
                (guid for ref in (source, target) if (guid := self._wildcard_member(op.graph, ref, op))), None)
            if waiting is not None:
                # Made once a later link types the wildcard (a ForEachLoop's Array), as an author would.
                self.deferred.append((op, original, frozenset((waiting,))))
                return
            # Authors sometimes write a link input->output; accept it when the swapped direction resolves.
            try:
                pins = (self.pin(op.graph, {**target, "direction": "output"}, op, add_missing=False),
                        self.pin(op.graph, {**source, "direction": "input"}, op, add_missing=False))
                keep = keep[::-1]
            except AssetDocumentContractError:
                raise original from None
            source, target = target, source
        source_pin = self.live_node(op.graph, pins[0]["node_guid"], op).find_pin(pins[0]["pin_name"], pins[0]["direction"])
        if source_pin is not None and any(ref.node_guid == pins[1]["node_guid"] and ref.pin_name == pins[1]["pin_name"]
                                          for ref in source_pin.linked_to):
            return  # already linked (an adopted companion node comes linked)
        ends = [(self.live_node(op.graph, ref["node_guid"], op), ref) for ref in pins]
        waiting = None if final else self._untyped_operand_end(op, ends) or self._untyped_ends(op, ends)
        if waiting is not None:
            # A wildcard (a reroute not linked to its source yet) into an operator would retype the operator,
            # and a link between two wildcards (Select into Select) types neither: the author wires the
            # wildcard's source first.
            self.deferred.append((op, None, waiting if type(waiting) is frozenset else frozenset((waiting,))))
            return
        # A wildcard end takes its type from the link (Get a copy's Output becomes the array's struct,
        # whose split members later links use); a promotable operator re-evaluates its pins.
        retypes = any(node.extras.get("promotable") is True
                      or ((pin := node.find_pin(ref["pin_name"], ref["direction"])) is not None
                          and pin.type.category == "wildcard") for node, ref in ends)
        args = {"from": pins[0], "to": pins[1]}
        for key, kept in zip(("keep_from_types", "keep_to_types"), keep):
            if kept:
                args[key] = kept
        try:
            result = self.invoke("graph.pin.connect.v1", args, op, write=True)
        except AssetDocumentContractError as error:
            if error.diagnostic.code not in ("Graph.ConnectNeedsConversion", "Graph.ConnectDisallowed"):
                raise
            facts = {item.key: item.value for item in error.diagnostic.context if type(item.value) is str}
            ends = [f"{ref.get('node', ref.get('node_guid'))}:{ref['pin']}" for ref in (source, target)]
            if error.diagnostic.code == "Graph.ConnectNeedsConversion":
                message = conversion_message(ends[0], ends[1], facts)
            else:
                # The primitive names the pins by node GUID; the document names them by node Id.
                reason = error.diagnostic.message.partition(": ")[2] or error.diagnostic.message
                message = f"Cannot connect {ends[0]} to {ends[1]}: {reason}"
            raise AssetDocumentContractError(RuntimeDiagnostic(
                error.diagnostic.path, error.diagnostic.code, message,
                error.diagnostic.severity, error.diagnostic.context)) from None
        if type(result) is dict and (result.get("reconstructed_node_guids") or result.get("created_node_guids")
                                     or result.get("broken_links")):
            # ReconstructNode may rename, add or drop pins: resolve later ops against a fresh snapshot.
            self.dirty.add(self.graphs[op.graph])
        elif retypes:
            self.stale.add(self.graphs[op.graph])
        self.touched |= {pins[0]["node_guid"], pins[1]["node_guid"]}

    def _untyped_operand_end(self, op: PlanOp, ends) -> str | None:
        """The node of a wildcard end of a link whose other end is a promotable operator."""
        for (node, ref), (other, _) in ((ends[0], ends[1]), (ends[1], ends[0])):
            pin = node.find_pin(ref["pin_name"], ref["direction"])
            if other.extras.get("promotable") is not True or node.extras.get("promotable") is True \
                    or pin is None or pin.type.category != "wildcard":
                continue
            if self._fresh(op.graph):  # stale: the wildcard may have its type by now
                pin = self.live_node(op.graph, node.guid, op).find_pin(ref["pin_name"], ref["direction"])
                if pin is None or pin.type.category != "wildcard":
                    continue
            return node.guid
        return None

    def _untyped_ends(self, op: PlanOp, ends) -> frozenset[str] | None:
        """Both nodes of a link between two wildcard pins (made once a link types either of them)."""
        def wildcard(node, ref) -> bool:
            pin = node.find_pin(ref["pin_name"], ref["direction"])
            return pin is not None and pin.type.category == "wildcard" and node.extras.get("promotable") is not True
        if not all(wildcard(node, ref) for node, ref in ends):
            return None
        if self._fresh(op.graph):  # stale: either may have its type by now
            ends = [(self.live_node(op.graph, node.guid, op), ref) for node, ref in ends]
            if not all(wildcard(node, ref) for node, ref in ends):
                return None
        return frozenset(node.guid for node, _ in ends)

    def _op_graph_pin_default_set(self, op: PlanOp) -> None:
        if op.args.get("optional"):
            # An early class pick (before the links): only on a class pin that is there now.
            guid = self.node_guid(op.graph, op.args["pin"], op)
            found = self.live_node(op.graph, guid, op).find_pin(op.args["pin"]["pin"], op.args["pin"]["direction"])
            if found is None or found.type.category not in CLASS_PIN_CATEGORIES:
                return
        self._fresh(op.graph)  # the current default after links and Pin Conversions
        ref = self.pin(op.graph, op.args["pin"], op)
        kind = op.args["kind"]
        if kind != "reset":
            pin = self.live_node(op.graph, ref["node_guid"], op).find_pin(ref["pin_name"], ref["direction"])
            # (None: a member of a struct pin the primitive splits first.)
            current = {"value": pin.default_value, "object": pin.default_object, "text": pin.default_text}[kind] \
                if pin is not None else None
            if current == op.args["value"]:
                return
        values = {"pin": ref, "kind": kind}
        if kind != "reset":
            values["value"] = op.args["value"]
        try:
            self._write("graph.pin.default.set.v1", values, op, op.graph)
        except AssetDocumentContractError as error:
            if error.diagnostic.code != "Graph.PinDefaultInvalid" or kind == "reset":
                raise
            facts = {item.key: item.value for item in error.diagnostic.context if type(item.value) is str}
            node = op.args["pin"].get("node", op.args["pin"].get("node_guid"))
            raise AssetDocumentContractError(RuntimeDiagnostic(
                error.diagnostic.path, error.diagnostic.code,
                pin_default_message(node, op.args["pin"]["pin"], kind, op.args["value"], error.diagnostic.message, facts),
                error.diagnostic.severity, error.diagnostic.context)) from None

    def _op_graph_node_layout_set(self, op: PlanOp) -> None:
        values = {"node_guid": self.node_guid(op.graph, op.args, op)}
        for key in ("position", "size", "comment", "comment_bubble_visible"):
            if key in op.args:
                values[key] = op.args[key]
        self._write("graph.node.layout.set.v1", values, op, op.graph)

    def _op_graph_layout_auto(self, op: PlanOp) -> None:
        """Lay out the listed nodes on the graph as it is now (every node exists, with its pins and the size
        the editor draws it at), then move each one that is not where the layout puts it."""
        self._flush_deferred()
        guid = self.graphs.get(op.graph)
        if guid is None:
            return
        graph = self._refresh(guid, include_sizes=True)
        doc = self.docs.get(op.graph)
        bound: dict[str, str] = {}
        for node in (doc.nodes if doc else ()):
            found = self.nodes.get((op.graph, node.id))
            if found is None and node.implicit is not None:
                try:
                    found = self.node_guid(op.graph, {"node": node.id}, op)
                except AssetDocumentContractError:
                    found = None
            if found is not None and graph.node(found) is not None and found not in bound.values():
                bound[node.id] = found
        order = list(bound.values())
        kept = set(order)
        obstacles = [node.guid for node in graph.nodes if node.guid not in kept]
        movable = [bound[node_id] for node_id in op.args["nodes"] if node_id in bound]
        keep = bound.get(op.args.get("keep"))
        moves = live_positions(graph, order=order, movable=movable, obstacles=obstacles, keep=keep)
        for node_guid, (x, y) in moves.items():
            current = graph.node(node_guid).position
            if current is not None and (int(round(current.x)), int(round(current.y))) == (x, y):
                continue
            self._write("graph.node.layout.set.v1", {"node_guid": node_guid, "position": {"x": x, "y": y}}, op, op.graph)

    def _op_graph_subgraph_sync(self, op: PlanOp) -> None:
        parent_guid = self.graphs.get(op.args["parent"])
        owner_guid = self.nodes.get((op.args["parent"], op.args["owner"]))
        infos = list_graphs(lambda values: self.invoke("graph.list.v1", values, op))
        doc = self.docs[op.graph]
        # The owner's subgraph of the same role first, then any sibling with the document name.  When
        # the owner holds several subgraphs of the role, the adapter's identity (live name) picks one.
        identity = self.adapter.subgraph_identity(doc.role, doc.name)
        if identity is not None:
            found = next((info for info in infos if info.parent_guid == parent_guid and owner_guid is not None
                          and info.owner_node_guid == owner_guid and info.role == doc.role
                          and info.name.casefold() == identity.casefold()), None)
        else:
            found = next((info for info in infos if info.parent_guid == parent_guid and owner_guid is not None
                          and info.owner_node_guid == owner_guid and info.role == doc.role), None) or next(
                (info for info in infos if info.parent_guid == parent_guid and (
                    info.owner_node_guid == owner_guid or info.name.casefold() == op.args["name"].casefold())), None)
        live_graph = None
        if found is not None:
            live_graph = self._refresh(found.guid, found)
        elif doc.owner is not None:
            raise contract_error(op.path, "Graph.GraphNotFound",
                                 f"Node '{op.args['owner']}' did not create a subgraph named '{op.args['name']}'.")
        plan = reconcile_graph(doc, live_graph, self.live, adapter=self.adapter, self_class=self.self_class,
                               parent=self.parents.get(op.graph))
        self.nodes.update(plan.node_bindings)
        self.graphs.update(plan.graph_bindings)
        self._run_ops(plan.ops)
        if parent_guid is not None:
            # The subgraph's signature (its tunnels' pins) reshapes the owner node in the parent graph:
            # the parent's links to the owner resolve against a fresh read.
            self.stale.add(parent_guid)

    def _op_graph_finalize(self, op: PlanOp) -> None:
        """Compile; compiler errors fail the apply, warnings join ``self.diagnostics``.

        The primitive fails on compile errors and carries the compiler messages in its error context; both
        outcomes are mapped onto the document node (and pin) the compiler pointed at.
        """
        meta = self._node_meta()
        # Batches of NODE_META_BATCH: the leading ones only record, the last one also compiles.
        while len(meta) > NODE_META_BATCH:
            self.invoke("graph.finalize.v1", {"node_meta": meta[:NODE_META_BATCH], "compile": False}, op, write=True)
            meta = meta[NODE_META_BATCH:]
        try:
            result = self.invoke("graph.finalize.v1", {"node_meta": meta} if meta else {}, op, write=True)
        except AssetDocumentContractError as error:
            if error.diagnostic.code != "Graph.FinalizeFailed":
                raise
            raw = next((item.value for item in error.diagnostic.context if item.key == "messages"), ())
            self._report_finalize("errors", raw, error.diagnostic)
            raise
        self._report_finalize(_value(result, "status", op.path), result.get("messages", ()), None)

    def _node_meta(self) -> list[dict]:
        """The document Id and authored action key of each node the apply placed or matched, for the nodes whose
        recorded ones differ (an adapter that ``keeps_authored_ids``): extract writes them back."""
        if not self.adapter.keeps_authored_ids:
            return []
        recorded = {node.guid: node for graph in (self.live.graphs if self.live is not None else ()) for node in graph.nodes}
        meta = []
        for (graph_id, node_id), guid in sorted(self.nodes.items()):
            doc = self.docs.get(graph_id)
            node = next((item for item in doc.nodes if item.id == node_id), None) if doc is not None else None
            if node is None:
                continue
            live = recorded.get(guid)
            if node.implicit is not None and node.id.casefold() == node.implicit.casefold() and (
                    live is None or not live.doc_id):
                continue  # Entry / Result: extract names them so
            # A document node without an Action (named by its NodeGuid) keeps the recorded action.
            action = node.authored_action or node.action
            if action is None:
                action = live.doc_action if live is not None else None
            if live is not None and live.doc_id == node.id and live.doc_action == action:
                continue
            meta.append({"node_guid": guid, "id": node.id, **({"action": action} if action else {})})
        return meta

    def _report_finalize(self, status: str, raw, failure: RuntimeDiagnostic | None) -> None:
        messages = [item for item in raw if type(item) is dict] if type(raw) in (list, tuple) else []
        paths = self._node_paths()
        if self.binder is not None and any((item.get("node_guid") or "").upper() not in paths for item in messages
                                           if item.get("node_guid")):
            binder, self.binder = self.binder, None
            try:
                binder(self)
            except AssetDocumentContractError:
                pass  # diagnostics fall back to /Body when the document cannot be bound
            paths = self._node_paths()
        labels = self._node_labels()
        errors = []
        for message in messages:
            guid = (message.get("node_guid") or "").upper()
            severity = {"error": DiagnosticSeverity.ERROR, "warning": DiagnosticSeverity.WARNING}.get(
                str(message.get("severity", "")).casefold(), DiagnosticSeverity.INFO)
            text = str(message.get("message", ""))
            code = "Graph.FinalizeMessage"
            if message.get("orphaned") is True and message.get("pin"):
                code, text = _orphan_text(severity, labels.get(guid, guid or "a node"), str(message["pin"]), text)
            elif message.get("id") == "PrunedExecInUse":
                code, text = _pruned_text(labels.get(guid, guid or "a node"), text)
            diagnostic = RuntimeDiagnostic(paths.get(guid, "/Body"), code, text, severity,
                                           (DiagnosticContext("primitive", "graph.finalize.v1"),))
            (errors if severity is DiagnosticSeverity.ERROR else self.diagnostics).append(diagnostic)
        self.finalize_status = status
        if failure is not None or status == "errors" or errors:
            first = errors[0] if errors else RuntimeDiagnostic("/Body", "Graph.FinalizeFailed", "Finalize failed.")
            code = first.code if first.code.startswith("Graph.Orphaned") else "Graph.FinalizeFailed"
            raise AssetDocumentContractError(RuntimeDiagnostic(
                first.path, code, first.message, DiagnosticSeverity.ERROR,
                (DiagnosticContext("messages", [{"path": item.path, "message": item.message} for item in errors]),)))

    def bind_document(self, desired: tuple[GraphDoc, ...], *, managed_roles: frozenset[str], role_paths=None,
                      sections=None) -> None:
        """Bind document node ids to live GUIDs without executing anything (diagnostic paths after a bare compile).

        Graphs the document does not mention (omitted region = keep) are bound to what an Extract would show.
        """
        live = self.read_live()
        plan = reconcile(desired, live, managed_roles=managed_roles, adapter=self.adapter,
                         self_class=self.self_class, role_paths=role_paths)
        for graph in plan.graphs:
            self._index(graph, None)
        self.nodes.update(plan.node_bindings)
        self.graphs.update(plan.graph_bindings)
        if sections:
            bound = set(self.graphs.values())
            for graph in extract_graphs(live, sections=sections, hints=desired, adapter=self.adapter,
                                        self_class=self.self_class):
                if graph.guid in bound:
                    continue
                self._index(graph, None)
                for sub in graph.walk():
                    for node in sub.nodes:
                        if node.guid:
                            self.nodes[(sub.id, node.id)] = node.guid

    def _node_labels(self) -> dict[str, str]:
        return {guid: node_id for (_, node_id), guid in self.nodes.items()}

    def _node_paths(self) -> dict[str, str]:
        output = {}
        for (graph_id, node_id), guid in self.nodes.items():
            doc = self.docs.get(graph_id)
            if doc is not None and any(node.id == node_id for node in doc.nodes):
                output[guid] = doc.node_path(node_id)
        return output


def _pruned_text(node: str, compiler: str) -> tuple[str, str]:
    """The compiler's PrunedExecInUse warning, in document terms: a node with exec pins runs only when executed."""
    return "Graph.PrunedNodeValueRead", (
        f"Node '{node}' has exec pins but its exec input is not linked, so it never runs: the compiler prunes it and every "
        f"value read from its outputs is the pin's default ({compiler}). Link its exec input (e.g. 'then' of the previous "
        f"node to its 'execute'), or use a pure node that has no exec pins.")


def _orphan_text(severity: DiagnosticSeverity, node: str, pin: str, compiler: str) -> tuple[str, str]:
    """An orphan pin is the editor's way of keeping what a signature edit would otherwise silently drop."""
    if severity is DiagnosticSeverity.ERROR:
        return "Graph.OrphanedPinLinked", (
            f"Pin '{pin}' no longer exists on node '{node}' but is still linked, which the editor refuses to compile "
            f"({compiler}). Remove the link(s) to this pin in the same apply, or keep the signature input.")
    return "Graph.OrphanedPinDefault", (
        f"Pin '{pin}' no longer exists on node '{node}'; the node keeps it as an orphan pin with its saved value "
        f"({compiler}). Recreate the node or the input if the value is still needed.")
