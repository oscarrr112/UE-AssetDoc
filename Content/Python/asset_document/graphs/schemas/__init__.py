"""Python-side schema adapters: the only place where graph schemas differ.

``SchemaAdapter`` supplies defaults that are correct for any EdGraph schema;
schema modules (``k2``, later ``bt``/``sm``) override only what they know
better.  Adapters are stateless and pure.
"""
from __future__ import annotations

import re

from asset_document.graphs.actions import (
    action_field_name, action_kind, normalize_action, preferred_alias, split_disambiguator,
)
from typing import Mapping

from asset_document.graphs.live import LiveGraph, LiveNode, LivePin, decode_pin_type_dto
from asset_document.graphs.model import LocalVariable, NodeDoc, NodeProperties, PinType, thaw_json


_SANITIZE = re.compile(r"[^A-Za-z0-9_-]")


def sanitize_id(text: str, fallback: str = "Node") -> str:
    value = _SANITIZE.sub("_", text or "").strip("_") or fallback
    return value if value[0].isalpha() or value[0] == "_" else f"Node_{value}"


class SchemaAdapter:
    """Defaults for a schema without special knowledge."""

    tag = "Generic"
    implicit_ids = {"entry": "Entry", "result": "Result", "root": "Root", "sm_entry": "Entry"}
    # True when a node-owned subgraph is identified by its owner node and named after it
    # (state machine / state / transition graphs): reconcile then matches it through the
    # owner binding and never renames it (renaming the owner renames the graph).
    owned_subgraphs_follow_owner = False
    # True when a graph's node-owned subgraphs are reconciled before the graph's links: their
    # signature makes the owner's pins (a K2 collapsed graph's tunnels mirror onto its node).
    subgraphs_before_links = False
    # True when a present graph region that does not list a live graph the editor will not delete
    # empties that graph instead (its deletable nodes are removed).
    clears_kept_graphs = False
    # True when an apply records each node's document Id and authored action key with the node (graph.finalize.v1
    # node_meta) and extract writes them back: a round trip keeps the author's Ids and action spellings.
    keeps_authored_ids = False

    def normalize_action(self, key: str, *, self_class: str | None, path: str) -> str:
        return normalize_action(key, self_class=self_class, path=path)

    # Matching ------------------------------------------------------------
    def semantic_key(self, node: NodeDoc, normalized_action: str | None) -> tuple | None:
        if normalized_action is None:
            return None
        return (split_disambiguator(normalized_action)[0], node.name)

    def live_semantic_key(self, node: LiveNode) -> tuple | None:
        if node.action_key is None:
            return None
        return (split_disambiguator(node.action_key)[0], self.live_name(node))

    def placed_with_graph(self, node: LiveNode) -> bool:
        """A node the editor puts into a graph it creates, besides the implicit ones (removed by a Create:
        the document's own nodes are spawned)."""
        return False

    def id_rank(self, node: LiveNode) -> int:
        """Extraction gives ids to lower ranks first (``_2`` suffixes go to the higher rank on collision)."""
        return 0

    def live_name(self, node: LiveNode) -> str | None:
        value = node.extras.get("name")
        return value if type(value) is str and value else None

    def extract_name(self, node: LiveNode) -> str | None:
        """Name written to an extracted NodeDoc (must agree with ``live_name`` for round trips)."""
        return self.live_name(node)

    def is_ignorable_live(self, node: LiveNode, graph: LiveGraph) -> bool:
        """Live nodes that are neither extracted nor removed (e.g. editor ghost nodes)."""
        return node.extras.get("ghost") is True

    def matches_in_order(self, key: tuple) -> bool:
        """Whether nodes sharing this semantic key are told apart by their order.

        Normally a key that is not unique on both sides matches nothing (the nodes are replaced).
        Nodes the editor allows several of with no name of their own (two transitions between the
        same states) pair up in order instead: the n-th document node with the n-th live node."""
        return False

    def node_matches_live(self, node: NodeDoc, live: LiveNode) -> bool:
        """Whether a NodeGuid match may keep the live node (False: replace it)."""
        return True

    def spawn_priority(self, node: NodeDoc) -> int:
        """Spawn order within one parent depth (lower first; stable otherwise)."""
        return 0

    # Properties ------------------------------------------------------------
    def live_properties(self, node: LiveNode, container: str) -> Mapping[str, object]:
        """Current values reconcile compares authored Properties against."""
        return node.property_container(container)

    def extract_properties(self, node: LiveNode, hint: NodeDoc | None) -> NodeProperties | None:
        """Properties written by extraction; None keeps the default (only what the hint authored)."""
        return None

    def omitted_property_resets(self, node: LiveNode, doc: NodeDoc, linked_pins: set) -> Mapping[str, object]:
        """Node settings the live node holds at a non-default value that ``doc`` leaves out, with the
        default each is reset to (an omitted setting is the default). None by default: omitted = keep."""
        return {}

    def property_follows_links(self, action: str | None, container: str, key: str) -> bool:
        """A property the node only keeps once its links are made (set after the graph's links)."""
        return False

    def missing_pin_hint(self, node: LiveNode, pin: str) -> str:
        """Appended to Graph.PinNotFound: how the node can get the pin (empty when the schema knows no way)."""
        return ""

    def hides_pin(self, action: str | None, container: str, key: str, value) -> bool:
        """Setting the property to ``value`` removes one of the node's pins (set after the graph's links, so the
        links the pin had are broken first)."""
        return False

    def subgraph_name(self, graph: LiveGraph, owner: LiveNode | None) -> str:
        """Document name of a live subgraph (unique among its siblings)."""
        return graph.name

    def subgraph_identity(self, graph_role: str, name: str) -> str | None:
        """The live graph name an owned subgraph document named ``name`` must have, when one owner
        holds several subgraphs of ``graph_role`` (None: the owner holds one graph of that role)."""
        return None

    # Pins ----------------------------------------------------------------
    def is_exec_pin_name(self, name: str) -> bool:
        return False

    def ignores_default(self, pin: LivePin) -> bool:
        """A stored pin default that is no value of its pin for this schema: extraction reports no
        default for it and reconcile leaves it as it is (it never resets it)."""
        return False

    def user_pins(self, node: LiveNode, graph_role: str) -> tuple[LivePin, ...]:
        """User-defined signature pins of an implicit entry/result node, in ``extras.user_pins`` order."""
        names = node.extras.get("user_pins")
        direction = "output" if node.implicit_role == "entry" else "input"
        if isinstance(names, tuple):
            output = []
            for name in names:
                pin = node.find_pin(name, direction) if type(name) is str else None
                if pin is not None and pin not in output:
                    output.append(pin)
            return tuple(output)
        return tuple(pin for pin in node.pins if pin.direction == direction and pin.type.category != "exec")

    def node_pin_types(self, node: LiveNode) -> tuple[tuple[str, PinType], ...]:
        """Types picked on a pin itself (``extras.typed_pins``, graph.node.pin_type.set.v1) that are
        set; a pin still a wildcard has no type of its own."""
        output = []
        for name in node.extras.get("typed_pins") or ():
            pin = node.find_pin(name) if type(name) is str else None
            if pin is not None and pin.type.category != "wildcard":
                output.append((pin.name, pin.type))
        return tuple(output)

    def live_pin_types(self, node: LiveNode) -> tuple[tuple[str, PinType], ...]:
        """Every picked pin type of a live node, which reconcile compares stated PinTypes against."""
        return self.node_pin_types(node)

    def pin_types_fit(self, node: NodeDoc, live: LiveNode) -> bool:
        """A live node can become ``node`` only if each stated pin type is its type or still unpicked
        (the editor cannot change a picked index type: a Select of another index type is a new node)."""
        have = dict((name.casefold(), item) for name, item in self.node_pin_types(live))
        return all(have.get(name.casefold(), item) == item for name, item in node.pin_types)

    def local_variables(self, node: LiveNode | None) -> tuple[LocalVariable, ...] | None:
        """Graph-scoped variables an entry node reports (``extras.local_variables``); None when the
        node has none to report (the graph cannot hold them)."""
        rows = node.extras.get("local_variables") if node is not None else None
        if rows is None:
            return None
        output = []
        for index, row in enumerate(thaw_json(rows)):
            path = f"/local_variables/{index}/type"
            pin_type = dict(row["type"])
            value = pin_type.pop("value_type", None)
            value_type = decode_pin_type_dto(dict(value, is_ref=False, is_const=False), f"{path}/value_type") \
                if value is not None else None
            output.append(LocalVariable(row["name"], decode_pin_type_dto(pin_type, path), row.get("default") or "",
                                        row.get("category") or "", row.get("tooltip") or "", value_type))
        return tuple(output)

    def node_user_pins(self, node: LiveNode) -> tuple[LivePin, ...] | None:
        """User-defined pins of a non-implicit node whose signature documents state (None: not applicable)."""
        return None

    def signature_flags(self, node: LiveNode) -> dict:
        """``extras.signature_flags`` of a function entry (empty when the node reports none)."""
        flags = node.extras.get("signature_flags")
        return thaw_json(flags) if flags is not None and hasattr(flags, "items") else {}

    def signature_flag_defaults(self, graph_role: str) -> dict | None:
        """Flag values of a freshly created graph of ``graph_role``; None when the schema has no flags."""
        return None

    def implicit_spawn_action(self, implicit_role: str, graph_role: str) -> str | None:
        return None

    def link_waits(self, from_action: str | None, to_pin: str) -> bool:
        """True when a link from a node of this action into this pin is made after the other links."""
        return False

    def link_leads(self, to_action: str | None, to_pin: str) -> bool:
        """True when a link into this pin of a node of this action is made before the node's other input
        links of the same rank (the pin that types the node's other pins)."""
        return False

    def types_from_operands(self, action: str | None) -> bool:
        """True when a node of this (normalized) action takes its types from the links into it, so the
        links into it are made before the links out of it."""
        return False

    def passes_link_type(self, action: str | None) -> bool:
        """True when a node of this (normalized) action passes the type linked into it on (a reroute)."""
        return False

    def override_shapes_pins(self, action: str | None, pin: str) -> bool:
        """True when setting this pin's default makes the node's other pins (applied before links)."""
        return False

    # Extraction ----------------------------------------------------------
    def extract_action(self, key: str | None, *, self_class: str | None = None) -> str | None:
        """Action written by extraction: the AI-friendly alias when one exists."""
        return preferred_alias(key, self_class=self_class)

    def node_base_id(self, node: LiveNode) -> str:
        if node.implicit_role:
            return self.implicit_ids.get(node.implicit_role, sanitize_id(node.implicit_role.title()))
        name = self.live_name(node)
        if name:
            return sanitize_id(name)
        alias = preferred_alias(node.action_key)
        if alias != node.action_key and alias.count(":") == 1:
            return sanitize_id(alias.partition(":")[2])  # "K2:Branch" -> "Branch"
        field = action_field_name(node.action_key)
        if field:
            return sanitize_id(field.rsplit(".", 1)[-1].rsplit("/", 1)[-1])
        kind = action_kind(node.action_key)
        if kind:
            return sanitize_id(kind)
        return sanitize_id(node.class_path.rsplit(".", 1)[-1])


GENERIC_ADAPTER = SchemaAdapter()
