"""Pure reconciliation: desired GraphDocs + live snapshot -> ordered primitive plan.

Node matching order: NodeGuid -> implicit role -> adapter semantic key (when
unique among unmatched on both sides, or in order where the adapter says nodes
sharing the key are told apart by their order) -> spawn.  Editor ghost nodes
(``extras.ghost``) are never removed; a desired node matched to one is reused
(linking or re-spawning the same event makes it real, contract section 4).

Operation order (contract section 3/8):
  graph.create/rename + signatures (user_pins, function flags) for every graph
  per graph: remove surplus nodes -> spawn (parents first) -> pin types -> properties/rename
  -> dynamic pins -> break surplus links -> connect new links (exec first)
  -> pin defaults -> layout -> automatic layout -> subgraphs (recursive)
  then graph.remove for surplus graphs, then one graph.finalize per asset.

Nodes the document gives no Position are laid out by ``graphs.layout`` once the graph holds them
(``graph.layout.auto``, run by the executor on the live graph with the editor's node sizes): the
nodes this plan places, and with ``"Layout": "Auto"`` every node.  A relayout of nodes that all
exist already is planned here as ``graph.node.layout.set`` moves (none when they are in place).

Ops reference desired nodes symbolically by doc id (``{"node": "Branch"}``);
the executor binds ids to GUIDs.  Live-only nodes are referenced by
``{"node_guid": ...}``.  The functions here never touch a context.
"""
from __future__ import annotations

from dataclasses import dataclass, field, replace
from types import MappingProxyType
from typing import Mapping

from asset_document.diagnostics import AssetDocumentContractError, contract_error
from asset_document.diff.semantic_compare import number_text, same_scalar_text
from asset_document.graphs.actions import action_kind, actions_equal, split_disambiguator
from asset_document.graphs.layout import live_positions
from asset_document.graphs.live import LiveAsset, LiveGraph, LiveNode
from asset_document.graphs.model import GraphDoc, LinkDoc, NodeDoc, Position, UserPin, thaw_json
from asset_document.graphs.schemas import GENERIC_ADAPTER, SchemaAdapter


CLASS_PIN_CATEGORIES = frozenset({"class", "softclass"})


@dataclass(frozen=True, slots=True)
class PlanOp:
    op: str
    graph: str
    path: str
    args: Mapping[str, object] = field(default_factory=dict)

    def brief(self) -> tuple:
        return (self.op, self.graph, dict(self.args))


@dataclass(frozen=True, slots=True)
class Plan:
    ops: tuple[PlanOp, ...]
    graphs: tuple[GraphDoc, ...]
    node_bindings: Mapping[tuple[str, str], str]
    graph_bindings: Mapping[str, str]

    @property
    def empty(self) -> bool:
        return not self.ops

    def briefs(self) -> list[tuple]:
        return [op.brief() for op in self.ops]


def _ref(node_id: str) -> dict:
    return {"node": node_id}


def _user_pin_dto(pin: UserPin, direction: str) -> dict:
    output = {"name": pin.name, "direction": direction, "type": pin.type.to_dto()}
    if pin.default is not None:
        output["default"] = pin.default
    return output


@dataclass(slots=True)
class _NodeMatch:
    matched: dict[str, LiveNode]      # doc node id -> live node
    surplus: list[LiveNode]           # live nodes to remove
    pseudo: dict[str, str]            # live guid -> pseudo id for unmatched implicit nodes


def _loose_pin_name(node: LiveNode | None, name: str, direction: str | None) -> str | None:
    """The live name of the pin a loose spelling names (``pin_names``), or None when the name is exact
    (case aside) or picks no single pin."""
    if node is None or any(pin.name.casefold() == name.casefold() for pin in node.pins):
        return None
    pin = node.find_pin(name, direction)
    return pin.name if pin is not None and pin.name != name else None


def _canonical_pin_names(doc: GraphDoc, match: _NodeMatch) -> GraphDoc:
    """Links and pin defaults of matched nodes name live pins by their live names (a user struct member's
    GUID name for its friendly name, ``then`` for "Row Found"), so they compare with the live graph."""
    def endpoint(item, direction):
        name = _loose_pin_name(match.matched.get(item.node), item.pin, direction)
        return item if name is None else replace(item, pin=name)
    links = tuple(LinkDoc(endpoint(link.from_, None), endpoint(link.to, None)) for link in doc.links)
    nodes = []
    for node in doc.nodes:
        found = match.matched.get(node.id)
        overrides = tuple(item if (name := _loose_pin_name(found, item.pin, item.direction)) is None
                          else replace(item, pin=name) for item in node.pin_overrides)
        nodes.append(node if overrides == node.pin_overrides else replace(node, pin_overrides=overrides))
    if links == doc.links and tuple(nodes) == doc.nodes:
        return doc
    return replace(doc, links=links, nodes=tuple(nodes))


class _Planner:
    def __init__(self, live: LiveAsset, adapter: SchemaAdapter, self_class: str | None,
                 called: frozenset[str] = frozenset()):
        self.live = live
        self.adapter = adapter
        self.self_class = self_class
        # Names of the asset's own functions / events that call nodes of the document call (casefolded).
        self.called = called
        self.prelude: list[PlanOp] = []
        # Custom events those calls name, made (spawned, named, given their inputs) before any other node
        # of any graph: a call node is placed from the event's function, which exists once it is named.
        self.definitions: list[PlanOp] = []
        self.defined: set[tuple[str, str]] = set()
        self.top_level = True
        self.body: list[PlanOp] = []
        self.removals: list[PlanOp] = []
        self.node_bindings: dict[tuple[str, str], str] = {}
        self.graph_bindings: dict[str, str] = {}
        self.retyped: set[tuple[str, str]] = set()
        # Links broken before a signature change removes their pin: (from guid, pin, to guid, pin), casefolded.
        self.broken_early: set[tuple[str, str, str, str]] = set()
        self._keys: frozenset[str] | None = None

    # Graph level -------------------------------------------------------
    def match_graphs(self, docs: tuple[GraphDoc, ...], candidates: tuple[LiveGraph, ...],
                     parent: GraphDoc | None = None):
        pairs, used = [], set()
        for doc in docs:
            # An owned subgraph the adapter identifies by name (a blend space sample graph) is that
            # graph only while the name holds, whatever GraphGuid the document carries.
            identity = self.adapter.subgraph_identity(doc.role, doc.name) if doc.owner is not None else None
            named = lambda g: identity is None or g.name.casefold() == identity.casefold()  # noqa: E731
            found = next((g for g in candidates if doc.guid and g.guid == doc.guid and g.guid not in used
                          and named(g)), None)
            if found is None and parent is not None and doc.owner is not None and self.adapter.owned_subgraphs_follow_owner:
                owner = self.node_bindings.get((parent.id, doc.owner))
                found = next((g for g in candidates if owner and g.owner_node_guid == owner and g.role == doc.role
                              and g.guid not in used and named(g)), None)
            if found is None:
                found = next((g for g in candidates if g.guid not in used and g.role == doc.role
                              and g.name.casefold() == doc.name.casefold()), None)
            if found is not None:
                used.add(found.guid)
            pairs.append((doc, found))
        return pairs, [g for g in candidates if g.guid not in used]

    def graph(self, doc: GraphDoc, live: LiveGraph | None, parent: GraphDoc | None) -> None:
        if live is None:
            args = {"role": doc.role, "name": doc.name}
            if parent is not None:
                args["parent"] = parent.id
            self.prelude.append(PlanOp("graph.create", doc.id, doc.path, args))
        else:
            self.graph_bindings[doc.id] = live.guid
            follows_owner = (self.adapter.owned_subgraphs_follow_owner and doc.owner is not None
                             and live.owner_node_guid is not None)
            if live.name != doc.name and not follows_owner:
                if live.can("rename"):
                    self.prelude.append(PlanOp("graph.rename", doc.id, doc.path, {"name": doc.name}))
                else:
                    raise contract_error(f"{doc.path}/Name", "Graph.GraphOperationUnsupported",
                                         f"Graph '{live.name}' cannot be renamed.")
        self.signature(doc, live)
        self.local_variables(doc, live)
        doc = self._localize(doc, live)
        self.top_level = parent is None
        match = self.match_nodes(doc, live)
        doc = _canonical_pin_names(doc, match)
        for node_id, node in match.matched.items():
            self.node_bindings[(doc.id, node_id)] = node.guid
        self.nodes(doc, live, match)
        self.defaults(doc, live, match, shaping=True)
        if self.adapter.subgraphs_before_links:
            self.subgraphs(doc, live)
        self.links(doc, live, match)
        for node in doc.nodes:
            self._properties(doc, node, match.matched.get(node.id), after_links=True)
        self.defaults(doc, live, match, shaping=False)
        self.layout(doc, match)
        self.auto_layout(doc, live, match)
        if not self.adapter.subgraphs_before_links:
            self.subgraphs(doc, live)

    def subgraphs(self, doc: GraphDoc, live: LiveGraph | None) -> None:
        children = self.live.children(live.guid) if live is not None else ()
        pairs, surplus = self.match_graphs(doc.subgraphs, children, doc)
        for child, found in pairs:
            if found is None and child.owner is not None:
                # Owned subgraphs appear when their owner node is spawned; plan them after execution.
                self.body.append(PlanOp("graph.subgraph.sync", child.id, child.path,
                                        {"parent": doc.id, "owner": child.owner, "name": child.name}))
                continue
            prelude_mark, body_mark = len(self.prelude), len(self.body)
            self.graph(child, found, doc)
            # A subgraph's create/signature ops run right before its own node work,
            # after the parent graph's nodes exist, not in the global prelude.
            nested = self.prelude[prelude_mark:]
            del self.prelude[prelude_mark:]
            self.body[body_mark:body_mark] = nested
        for graph in surplus:
            if graph.owner_node_guid is None and graph.can("remove"):
                self.removals.append(PlanOp("graph.remove", doc.id, f"{doc.path}/Subgraphs",
                                            {"graph_guid": graph.guid, "name": graph.name}))

    # Signatures --------------------------------------------------------
    def signature(self, doc: GraphDoc, live: LiveGraph | None) -> None:
        signature = doc.signature
        if signature is None:
            return
        path = f"{doc.path}/Signature"
        implicit = {node.implicit_role: node for node in (live.nodes if live else ()) if node.implicit_role}
        for role, pins, direction in (("entry", signature.inputs, "output"), ("result", signature.outputs, "input")):
            node = implicit.get(role)
            if node is not None and self._user_pins_equal(node, pins, doc.role):
                continue
            if node is None and not pins:
                continue
            if node is not None and live is not None:
                self._break_links_of_removed_pins(doc, live, node, pins, path)
            args = {"implicit": role, "pins": [_user_pin_dto(pin, direction) for pin in pins]}
            spawn = self.adapter.implicit_spawn_action(role, doc.role)
            if node is None and spawn is not None:
                args["spawn_action"] = spawn
            self.prelude.append(PlanOp("graph.node.user_pins.set", doc.id, path, args))
        self._flags(doc, implicit.get("entry"), path)

    def local_variables(self, doc: GraphDoc, live: LiveGraph | None) -> None:
        """Stated graph-scoped variables (K2 function locals), right after the signature: nodes that
        get or set them are spawned by keys naming them."""
        if doc.local_variables is None:
            return
        entry = next((node for node in (live.nodes if live else ()) if node.implicit_role == "entry"), None)
        have = self.adapter.local_variables(entry) if entry is not None else ()
        if have == doc.local_variables or (have is None and not doc.local_variables):
            return
        self.prelude.append(PlanOp("graph.local_variables.set", doc.id, f"{doc.path}/LocalVariables",
                                   {"variables": [item.to_dto() for item in doc.local_variables]}))

    def _localize(self, doc: GraphDoc, live: LiveGraph | None) -> GraphDoc:
        """A document that leaves a function's LocalVariables out keeps the live ones: its ``K2:Get:X``
        names the local ``X`` when the function has one (k2.localize_action)."""
        if doc.local_variables is not None or live is None or self.self_class is None:
            return doc
        entry = next((node for node in live.nodes if node.implicit_role == "entry"), None)
        names = frozenset(item.name for item in (self.adapter.local_variables(entry) or ())) if entry else frozenset()
        if not names:
            return doc
        from asset_document.graphs.schemas.k2 import localize_action
        nodes = []
        for node in doc.nodes:
            action = None
            if node.action is not None:
                try:
                    action = self.adapter.normalize_action(node.action, self_class=self.self_class, path="/Action")
                except AssetDocumentContractError:
                    action = None
            local = localize_action(action, names, graph=doc.name, self_class=self.self_class) if action else None
            nodes.append(replace(node, action=local) if local is not None and local != action else node)
        return replace(doc, nodes=tuple(nodes)) if tuple(nodes) != doc.nodes else doc

    def _flags(self, doc: GraphDoc, entry: LiveNode | None, path: str) -> None:
        """A present Signature states every flag: omitted flags mean the schema default."""
        wanted = thaw_json(doc.signature.flags)
        defaults = self.adapter.signature_flag_defaults(doc.role)
        if defaults is None:
            if wanted:
                raise contract_error(path, "Graph.InvalidSignature",
                                     f"{doc.role} graphs have no function flags ({', '.join(sorted(wanted))}).")
            return
        desired = {**defaults, **wanted}
        current = dict(defaults)  # a graph created by this plan starts with default flags
        if entry is not None:
            current.update(self.adapter.signature_flags(entry))
        changed = {key: value for key, value in desired.items() if current.get(key) != value}
        if changed:
            self.prelude.append(PlanOp("graph.function.flags.set", doc.id, path, {"flags": changed}))

    def _user_pins_equal(self, node: LiveNode, pins: tuple[UserPin, ...], graph_role: str) -> bool:
        live = self.adapter.user_pins(node, graph_role)
        if len(live) != len(pins):
            return False
        for have, want in zip(live, pins):
            if have.name != want.name or have.type != want.type:
                return False
            if want.default is None:
                # No authored default: only the editor's autogenerated default is acceptable.
                if have.default_value and not have.is_autogenerated_default:
                    return False
            elif not same_scalar_text(have.default_value, want.default) and not (
                    not have.default_value and is_type_zero(want.type, want.default)):
                return False
        return True

    # Nodes -------------------------------------------------------------
    def action(self, node: NodeDoc, path: str) -> str | None:
        if node.action is None:
            return None
        return self._inherited(self.adapter.normalize_action(node.action, self_class=self.self_class,
                                                             path=f"{path}/Action"))

    def _inherited(self, action: str) -> str:
        """A short form naming the asset's own member (``K2:Get:Combat``) that a parent class declares is
        that class's member: its live nodes are keyed by the declaring class (the native spawn resolves
        the short form the same way)."""
        from asset_document.graphs.schemas.k2 import declared_call_key
        base, sep, suffix = action.partition("#")
        declared = declared_call_key(base, self._live_keys())
        if declared is not None:
            return declared + sep + suffix
        if not self.live.class_chain or not self.self_class:
            return action
        parts = base.split(":")
        if len(parts) != 4 or parts[2].casefold() != self.self_class.casefold() or base in self._live_keys():
            return action
        for ancestor in self.live.class_chain:
            key = ":".join(parts[:2] + [ancestor, parts[3]])
            if key in self._live_keys():
                return key + sep + suffix
        return action

    def _live_keys(self) -> frozenset[str]:
        if self._keys is None:
            self._keys = frozenset(split_disambiguator(node.action_key)[0] for graph in self.live.graphs
                                   for node in graph.nodes if node.action_key)
        return self._keys

    def match_nodes(self, doc: GraphDoc, live: LiveGraph | None) -> _NodeMatch:
        candidates = list(live.nodes) if live else []
        matched: dict[str, LiveNode] = {}
        taken: set[str] = set()
        actions = {node.id: self.action(node, doc.node_path(node.id)) for node in doc.nodes}

        def take(node_id: str, found: LiveNode) -> None:
            matched[node_id] = found
            taken.add(found.guid)

        for node in doc.nodes:
            if node.guid is None:
                continue
            found = next((n for n in candidates if n.guid == node.guid and n.guid not in taken), None)
            if found is None:
                if node.action is None and node.implicit is None and node.extras.get("Unresolved"):
                    # Refused: no palette action places a node naming what does not exist.
                    raise contract_error(f"{doc.node_path(node.id)}/NodeGuid", "Graph.UnresolvedReference",
                                         f"The node names {node.extras['Unresolved']}, which does not exist, so the "
                                         "editor cannot place it; remove the node or restore what it names.")
                if node.action is None and node.implicit is None:
                    raise contract_error(f"{doc.node_path(node.id)}/NodeGuid", "Graph.NodeNotFound",
                                         "No live node has this NodeGuid and the node has no Action to recreate it.")
                continue
            if (actions[node.id] is None or actions_equal(actions[node.id], found.action_key)) \
                    and self.adapter.node_matches_live(node, found) and self.adapter.pin_types_fit(node, found):
                take(node.id, found)
        by_role: dict[str, list[str]] = {}
        for node in doc.nodes:
            if node.id not in matched and node.implicit is not None:
                by_role.setdefault(node.implicit, []).append(node.id)
        for role, ids in by_role.items():
            # In order: the n-th unmatched implicit node of a role (a function's return nodes) is the n-th live one.
            options = [n for n in candidates if n.implicit_role == role and n.guid not in taken]
            for node_id, found in zip(ids, options):
                take(node_id, found)
        wanted: dict[tuple, list[str]] = {}
        for node in doc.nodes:
            if node.id not in matched and node.implicit is None:
                key = self.adapter.semantic_key(node, actions[node.id])
                if key is not None:
                    wanted.setdefault(key, []).append(node.id)
        offered: dict[tuple, list[LiveNode]] = {}
        for found in candidates:
            if found.guid not in taken and found.implicit_role is None:
                key = self.adapter.live_semantic_key(found)
                if key is not None:
                    offered.setdefault(key, []).append(found)
        by_id = {node.id: node for node in doc.nodes}
        for key, ids in wanted.items():
            options = offered.get(key, ())
            if len(ids) == 1 and len(options) == 1:
                if self.adapter.pin_types_fit(by_id[ids[0]], options[0]):
                    take(ids[0], options[0])
            elif self.adapter.matches_in_order(key):
                # Same key, no name of their own: the n-th unmatched document node is the n-th live one.
                for node_id, found in zip(ids, options):
                    if self.adapter.pin_types_fit(by_id[node_id], found):
                        take(node_id, found)
        surplus = [n for n in candidates if n.guid not in taken and n.implicit_role is None
                   and not self.adapter.is_ignorable_live(n, live)]
        pseudo = {n.guid: f"~{n.implicit_role}" for n in candidates if n.guid not in taken and n.implicit_role}
        return _NodeMatch(matched, surplus, pseudo)

    def _ghost_companions(self, live: LiveGraph | None, match: _NodeMatch) -> list[LiveNode]:
        """Unmatched editor ghosts linked to a ghost the document reuses: the "Parent: <event>" call the editor
        placed with a child Blueprint's default event. Placing that event from the palette removes the ghost
        and what it links (UBlueprintEventNodeSpawner::Invoke, RemoveGhostNodes); linking the ghost would make
        them real instead. A document that wants the parent call lists it."""
        if live is None:
            return []
        by_guid = {node.guid: node for node in live.nodes}
        matched = {found.guid for found in match.matched.values()}
        pending = [found for found in match.matched.values() if self.adapter.is_ignorable_live(found, live)]
        seen: set[str] = {found.guid for found in pending}
        companions = []
        while pending:
            node = pending.pop()
            for pin in node.pins:
                for ref in pin.linked_to:
                    other = by_guid.get(ref.node_guid)
                    if (other is None or other.guid in seen or other.guid in matched
                            or not self.adapter.is_ignorable_live(other, live)):
                        continue
                    seen.add(other.guid)
                    companions.append(other)
                    pending.append(other)
        return companions

    def nodes(self, doc: GraphDoc, live: LiveGraph | None, match: _NodeMatch) -> None:
        for node in self._ghost_companions(live, match):
            self.body.append(PlanOp("graph.node.remove", doc.id, f"{doc.path}/Nodes",
                                    {"node_guid": node.guid, "action_key": node.action_key or node.class_path}))
        removed = {node.guid for node in match.surplus}
        for node in match.surplus:
            if node.parent_guid in removed:
                continue  # removed together with its parent
            self.body.append(PlanOp("graph.node.remove", doc.id, f"{doc.path}/Nodes",
                                    {"node_guid": node.guid, "action_key": node.action_key or node.class_path}))
        linked = {link.from_.node for link in doc.links} | {link.to.node for link in doc.links}
        for node in doc.nodes:
            found = match.matched.get(node.id)
            if found is not None and live is not None and self.adapter.is_ignorable_live(found, live) \
                    and node.id not in linked and found.action_key:
                # A reused editor ghost (e.g. the default BeginPlay of a new Blueprint) becomes a real node
                # when linked; without links, spawning the same event converts it (spawn reports reused).
                self.body.append(PlanOp("graph.node.spawn", doc.id, doc.node_path(node.id),
                                        {"node": node.id, "action_key": found.action_key}))
        primary = {node.implicit_role for node in (live.nodes if live else ()) if node.implicit_role}
        # A further node of an implicit role (a function's extra return node, K2:FunctionResult) is
        # placed; the first one, preferably the one with the reserved id (Result), is the graph's own
        # (made with the graph or by its signature; placed too when no signature pin makes it, as a
        # function's return node without outputs).
        signature = doc.signature
        made = {"entry": True, "result": signature is not None and bool(signature.outputs)}
        for node in sorted(doc.nodes, key=lambda item: item.id != self.adapter.implicit_ids.get(item.implicit)):
            if node.implicit is None or node.id in match.matched:
                continue
            adopt = False
            if node.implicit not in primary:
                primary.add(node.implicit)
                if made.get(node.implicit, True):
                    continue
                # Without stated outputs the graph may still come with it (an override of a parent
                # function that has outputs): that one is taken, else it is placed.
                adopt = True
            spawn = self.adapter.implicit_spawn_action(node.implicit, doc.role)
            if spawn is None:
                continue
            args = {"node": node.id, "action_key": spawn}
            if adopt:
                args["adopt_implicit"] = node.implicit
            if node.position is not None:
                args["position"] = node.position.to_dto()
            self.body.append(PlanOp("graph.node.spawn", doc.id, doc.node_path(node.id), args))
        spawned = [node for node in self._parent_first(doc.nodes, self.adapter) if node.id not in match.matched
                   and node.implicit is None]
        for node in spawned:
            path = doc.node_path(node.id)
            action = self.action(node, path)
            if action is None:
                raise contract_error(f"{path}/Action", "Graph.NodeActionMissing",
                                     "This node does not exist yet; an Action is required to create it.")
            args = {"node": node.id, "action_key": action}
            if node.position is not None:
                args["position"] = node.position.to_dto()
            if node.parent is not None:
                args["parent"] = node.parent
            if self._is_called_event(node, action):
                self.definitions.append(PlanOp("graph.node.spawn", doc.id, path, args))
                self.definitions.append(PlanOp("graph.node.rename", doc.id, f"{path}/Name",
                                               {"node": node.id, "name": node.name}))
                mark = len(self.body)
                self._node_user_pins(doc, node, None)
                self.definitions.extend(self.body[mark:])
                del self.body[mark:]
                self.defined.add((doc.id, node.id))
                continue
            self.body.append(PlanOp("graph.node.spawn", doc.id, path, args))
        for node in doc.nodes:
            self._pin_types(doc, node, match.matched.get(node.id))
        for node in doc.nodes:
            self._properties(doc, node, match.matched.get(node.id))
        for node in doc.nodes:
            if (doc.id, node.id) not in self.defined:
                self._node_user_pins(doc, node, match.matched.get(node.id))
        for node in doc.nodes:
            self._dynamic_pins(doc, node, match.matched.get(node.id))

    def _is_called_event(self, node: NodeDoc, action: str) -> bool:
        """A new custom event of a top-level graph that a call node of the document names."""
        return (self.top_level and node.parent is None and node.name is not None
                and action_kind(action) == "CustomEvent" and node.name.casefold() in self.called)

    @staticmethod
    def _parent_first(nodes: tuple[NodeDoc, ...], adapter: SchemaAdapter = GENERIC_ADAPTER) -> list[NodeDoc]:
        by_id = {node.id: node for node in nodes}

        def depth(node: NodeDoc) -> int:
            count = 0
            while node.parent is not None:
                node, count = by_id[node.parent], count + 1
            return count
        # stable: doc order within one depth and spawn priority
        return sorted(nodes, key=lambda node: (depth(node), adapter.spawn_priority(node)))

    def auto_layout(self, doc: GraphDoc, live: LiveGraph | None, match: _NodeMatch) -> None:
        """Lay out the nodes the document does not place: those this plan creates (every node of a graph it
        creates; an editor ghost it reuses is placed with them), or with ``"Layout": "Auto"`` every node
        without a Position.  Nodes the plan places are laid out by the executor once they exist; a relayout
        of existing nodes is planned here, as the moves it makes."""
        relayout = doc.layout == "Auto"
        new = [node.id for node in doc.nodes if node.position is None and node.id not in match.matched]
        if not new and not relayout:
            return
        ghosts = {node_id for node_id, found in match.matched.items()
                  if live is not None and self.adapter.is_ignorable_live(found, live)}
        place = [node.id for node in doc.nodes if node.position is None and (
            relayout or live is None or node.id not in match.matched or node.id in ghosts)]
        if not place:
            return
        # A relayout keeps the graph's first existing node where it is and arranges the others around it.
        keep = next((node_id for node_id in place if node_id in match.matched
                     and match.matched[node_id].position is not None), None) if relayout else None
        if new or live is None:
            args = {"nodes": place}
            if keep is not None:
                args["keep"] = keep
            self.body.append(PlanOp("graph.layout.auto", doc.id, doc.path, args))
            return
        for node_id, (x, y) in _relayout_moves(doc, live, match.matched, match.surplus, place, keep).items():
            self.body.append(PlanOp("graph.node.layout.set", doc.id, doc.node_path(node_id),
                                    {"node": node_id, "position": {"x": x, "y": y}}))

    def _properties(self, doc: GraphDoc, node: NodeDoc, live: LiveNode | None, *, after_links: bool = False) -> None:
        path = doc.node_path(node.id)
        action = node.action or (live.action_key if live else None)
        for container, key, value in node.properties.items():
            wanted = thaw_json(value)
            follows = (self.adapter.property_follows_links(action, container, key)
                       or self.adapter.hides_pin(action, container, key, wanted))
            if follows != after_links:
                continue
            if live is not None:
                have = self.adapter.live_properties(live, container)
                if key in have and same_json(thaw_json(have[key]), wanted):
                    continue
            label = "Properties" if container == "node" else "TemplateProperties"
            self.body.append(PlanOp("graph.node.property.set", doc.id, f"{path}/{label}/{key}",
                                    {"node": node.id, "container": container, "property_path": key, "value": wanted}))
        if live is not None:
            # Settings the document leaves out are the node's defaults (where the schema reports them). One that
            # hides a pin goes after the graph's links: the links it had are broken first.
            linked = {end.pin.casefold() for link in doc.links for end in (link.from_, link.to) if end.node == node.id}
            for key, default in self.adapter.omitted_property_resets(live, node, linked).items():
                if self.adapter.hides_pin(action, "node", key, thaw_json(default)) != after_links:
                    continue
                self.body.append(PlanOp("graph.node.property.set", doc.id, f"{path}/Properties/{key}",
                                        {"node": node.id, "container": "node", "property_path": key,
                                         "value": thaw_json(default)}))
        if after_links:
            return
        if node.name is not None and (doc.id, node.id) not in self.defined \
                and (live is None or self.adapter.live_name(live) != node.name):
            self.body.append(PlanOp("graph.node.rename", doc.id, f"{path}/Name", {"node": node.id, "name": node.name}))

    def _pin_types(self, doc: GraphDoc, node: NodeDoc, live: LiveNode | None) -> None:
        """Stated pin types (``PinTypes``) right after spawn: they reshape the node's other pins."""
        have = dict((name.casefold(), item) for name, item in self.adapter.live_pin_types(live)) if live else {}
        for pin, wanted in node.pin_types:
            if have.get(pin.casefold()) != wanted:
                if live is not None:
                    self.retyped.add((doc.id, node.id))  # changing a pin's type may break its links
                self.body.append(PlanOp("graph.node.pin_type.set", doc.id, f"{doc.node_path(node.id)}/PinTypes/{pin}",
                                        {"node": node.id, "pin": pin, "type": wanted.to_dto()}))

    def _node_user_pins(self, doc: GraphDoc, node: NodeDoc, live: LiveNode | None) -> None:
        """A stated custom event signature (``Inputs``): user pins are outputs of the node."""
        if node.user_pins is None:
            return
        if live is not None and user_pins_equal(self.adapter.node_user_pins(live), node.user_pins):
            return
        self.body.append(PlanOp("graph.node.user_pins.set", doc.id, f"{doc.node_path(node.id)}/Inputs",
                                {"node": node.id, "pins": [_user_pin_dto(pin, "output") for pin in node.user_pins]}))

    def _dynamic_pins(self, doc: GraphDoc, node: NodeDoc, live: LiveNode | None) -> None:
        if live is None or not live.can_add_pin:
            return  # spawned nodes get dynamic pins lazily while connecting
        referenced = [link.from_.pin for link in doc.links if link.from_.node == node.id]
        referenced += [link.to.pin for link in doc.links if link.to.node == node.id]
        referenced += [item.pin for item in node.pin_overrides]
        missing = []
        for name in referenced:
            if live.find_pin(name) is None and name not in missing:
                missing.append(name)
        if missing:
            self.body.append(PlanOp("graph.node.dynamic_pin.add", doc.id, doc.node_path(node.id),
                                    {"node": node.id, "pins": missing}))

    # Links -------------------------------------------------------------
    def _canonical_link(self, link: LinkDoc, match: _NodeMatch) -> LinkDoc:
        """Flip a link the author wrote input->output when live pins prove it."""
        source, target = match.matched.get(link.from_.node), match.matched.get(link.to.node)
        if source is None or target is None:
            return link
        if source.find_pin(link.from_.pin, "output") or not source.find_pin(link.from_.pin, "input"):
            return link
        if target.find_pin(link.to.pin, "output"):
            return LinkDoc(link.to, link.from_)
        return link

    def _live_link_keys(self, live: LiveGraph | None, match: _NodeMatch) -> dict[tuple, dict]:
        ids = {node.guid: node_id for node_id, node in match.matched.items()}
        ids.update(match.pseudo)
        output = {}
        for link in (live.links if live else ()):
            source, target = ids.get(link.from_.node_guid), ids.get(link.to.node_guid)
            if source is None or target is None:
                continue  # touches a node that is removed (links go with it) or ignored
            key = (source, link.from_.pin_name.casefold(), target, link.to.pin_name.casefold())
            output[key] = {"from": self._pin_ref(source, link.from_, "output"),
                           "to": self._pin_ref(target, link.to, "input")}
            if (link.from_.node_guid, key[1], link.to.node_guid, key[3]) in self.broken_early:
                output[key]["early"] = True  # broken before the signature change
        return output

    @staticmethod
    def _pin_ref(node_id: str, ref, direction: str) -> dict:
        base = {"node_guid": ref.node_guid} if node_id.startswith("~") else {"node": node_id}
        return {**base, "pin": ref.pin_name, "direction": direction}

    def _is_exec(self, link: LinkDoc, match: _NodeMatch) -> bool:
        source = match.matched.get(link.from_.node)
        pin = source.find_pin(link.from_.pin, "output") if source else None
        if pin is not None:
            return pin.type.category == "exec"
        return self.adapter.is_exec_pin_name(link.from_.pin) or self.adapter.is_exec_pin_name(link.to.pin)

    def _operand_depths(self, doc: GraphDoc, match: _NodeMatch) -> dict[str, float]:
        """Per node typed by its operands: 1 + the deepest such node feeding it; a node that passes its
        input's type on (a reroute) has the depth of what feeds it; others are 0."""
        typed, passing = set(), set()
        for node in doc.nodes:
            try:
                action = self.adapter.normalize_action(node.action, self_class=self.self_class, path="/Action") \
                    if node.action is not None else None
            except AssetDocumentContractError:
                continue
            if self.adapter.types_from_operands(action):
                typed.add(node.id)
            elif self.adapter.passes_link_type(action):
                passing.add(node.id)
        if not typed:
            return {}
        feeds: dict[str, set] = {}
        for link in doc.links:
            if (link.to.node in typed or link.to.node in passing) and not self._is_exec(link, match):
                feeds.setdefault(link.to.node, set()).add(link.from_.node)
        depth: dict[str, float] = {}

        def of(node_id: str, seen: frozenset) -> float:
            if node_id not in typed and node_id not in passing or node_id in seen:
                return 0
            if node_id not in depth:
                deepest = max((of(source, seen | {node_id}) for source in feeds.get(node_id, ())), default=0)
                # (A reroute an operator feeds is linked onwards after the operator's link into it.)
                depth[node_id] = deepest + 1 if node_id in typed else deepest + 0.5 if deepest else 0
            return depth[node_id]
        for node_id in typed | passing:
            of(node_id, frozenset())
        return depth

    def _break_links_of_removed_pins(self, doc: GraphDoc, live: LiveGraph, node: LiveNode,
                                     pins: tuple[UserPin, ...], path: str) -> None:
        """A signature pin the document no longer has goes with the signature change, which runs before the
        graph's links: its links are broken first, as one deletes a wire before the parameter it leaves."""
        kept = {pin.name.casefold() for pin in pins}
        removed = {pin.name.casefold() for pin in self.adapter.user_pins(node, doc.role)} - kept
        if not removed:
            return
        for link in live.links:
            if not ((link.from_.node_guid == node.guid and link.from_.pin_name.casefold() in removed)
                    or (link.to.node_guid == node.guid and link.to.pin_name.casefold() in removed)):
                continue
            self.broken_early.add((link.from_.node_guid, link.from_.pin_name.casefold(),
                                   link.to.node_guid, link.to.pin_name.casefold()))
            self.prelude.append(PlanOp("graph.pin.break", doc.id, f"{doc.path}/Links", {
                "from": {"node_guid": link.from_.node_guid, "pin": link.from_.pin_name, "direction": "output"},
                "to": {"node_guid": link.to.node_guid, "pin": link.to.pin_name, "direction": "input"}}))

    def links(self, doc: GraphDoc, live: LiveGraph | None, match: _NodeMatch) -> None:
        current = self._live_link_keys(live, match)
        desired = {}
        for index, link in enumerate(doc.links):
            link = self._canonical_link(link, match)
            desired[link.key()] = (index, link)
        for key, refs in current.items():
            if key not in desired and refs.pop("early", None) is None:
                self.body.append(PlanOp("graph.pin.break", doc.id, f"{doc.path}/Links", refs))
        # Links of a node whose pin types change are made again (connecting a linked pair is a no-op).
        new = [(index, link) for key, (index, link) in desired.items() if key not in current
               or (doc.id, link.from_.node) in self.retyped or (doc.id, link.to.node) in self.retyped]
        # Some links wait for the rest: a reroute linked into a target ("self") pin first would become a
        # single object (the pin also takes an array) and refuse the array the value flow brings it
        # (MakeArray -> Reroute -> SetTextRenderColor:self). A Make Array linked there first is right:
        # it takes its element type from that target, as when the author wired it.
        actions = {node.id: node.action for node in doc.nodes}
        # Operators that take their types from their operands get those links first, as data flows: an
        # operator's output is linked once its inputs are (Divide -> Multiply after Divide's operands).
        depth = self._operand_depths(doc, match)
        new.sort(key=lambda item: (not self._is_exec(item[1], match),
                                   self.adapter.link_waits(actions.get(item[1].from_.node), item[1].to.pin),
                                   depth.get(item[1].from_.node, 0),
                                   not self.adapter.link_leads(actions.get(item[1].to.node), item[1].to.pin), item[0]))
        # An end whose node states its pin types (a promotable operator's Pin Conversions) takes them back
        # when the link re-picks them and its links fit them.
        stated = {node.id: {pin: item.to_dto() for pin, item in node.pin_types} for node in doc.nodes if node.pin_types}
        for index, link in new:
            args = {"from": {**_ref(link.from_.node), "pin": link.from_.pin, "direction": "output"},
                    "to": {**_ref(link.to.node), "pin": link.to.pin, "direction": "input"}}
            if link.from_.node in stated:
                args["keep_from_types"] = stated[link.from_.node]
            if link.to.node in stated:
                args["keep_to_types"] = stated[link.to.node]
            self.body.append(PlanOp("graph.pin.connect", doc.id, f"{doc.path}/Links/{index}", args))

    # Pin defaults --------------------------------------------------------
    def defaults(self, doc: GraphDoc, live: LiveGraph | None, match: _NodeMatch, *, shaping: bool = False) -> None:
        """Pin overrides; with ``shaping`` only those that make a node's pins (a Format Text's Format,
        whose arguments become pins), which run before the links that use those pins."""
        linked = {(link.to.node, link.to.pin.casefold()) for link in doc.links}
        linked |= {(link.from_.node, link.from_.pin.casefold()) for link in doc.links}
        for node in doc.nodes:
            path = doc.node_path(node.id)
            found = match.matched.get(node.id)
            action = node.action or (found.action_key if found else None)
            overridden = set()
            for index, item in enumerate(node.pin_overrides):
                pin = found.find_pin(item.pin, item.direction) if found else None
                overridden.add((item.pin.casefold(), item.direction))
                shapes = self.adapter.override_shapes_pins(action, item.pin)
                # A class picked on a class pin (Create Widget / Add Component by Class / Spawn Actor "Class")
                # types the node's outputs, as when the author picked it before wiring: set it before the
                # links too (optional there: skipped unless it is a class pin then), and again after them
                # (the executor skips a default the pin already has).
                early_class = shaping and not shapes and item.kind == "object"
                # A default on a pin whose type the document states (PinTypes) holds that type while links
                # reshape the node (a promotable operator conforms an undetermined extra pin to its
                # function): set it before the links too.
                early_typed = shaping and not shapes and node.pin_type(item.pin) is not None
                if early_typed:
                    early_class = False
                if shapes != shaping and not early_class and not early_typed:
                    continue
                if pin is not None:
                    if same_pin_value(pin, item.kind, item.payload):
                        continue
                    if early_class and pin.type.category not in CLASS_PIN_CATEGORIES:
                        continue
                args = {"pin": {**_ref(node.id), "pin": item.pin, "direction": item.direction},
                        "kind": item.kind, "value": item.payload}
                if early_class:
                    args["optional"] = True
                self.body.append(PlanOp("graph.pin.default.set", doc.id, f"{path}/PinOverrides/{index}", args))
            terminator = found is not None and found.implicit_role == "result"
            for pin in (found.pins if found and not shaping else ()):
                if ((pin.name.casefold(), pin.direction) in overridden or (node.id, pin.name.casefold()) in linked
                        or not is_authored_default(pin, self.adapter, terminator=terminator)):
                    continue
                zero = type_zero(pin.type) if terminator else None
                # A return node's value is its own autogenerated default: an omitted one is set to the type's
                # zero value (a reset would keep it).
                reset = {"kind": "reset"} if zero is None else {"kind": "value", "value": zero}
                self.body.append(PlanOp("graph.pin.default.set", doc.id, path, {
                    "pin": {**_ref(node.id), "pin": pin.name, "direction": pin.direction}, **reset}))

    # Layout ----------------------------------------------------------------
    def layout(self, doc: GraphDoc, match: _NodeMatch) -> None:
        for node in doc.nodes:
            found = match.matched.get(node.id)
            args = {}
            if node.position is not None and (
                    (found is None and node.implicit is not None)  # created with its graph, never spawned
                    or (found is not None and (found.position is None
                                               or found.position.to_dto() != node.position.to_dto()))):
                args["position"] = node.position.to_dto()
            if node.comment is not None and (found is None or found.comment != node.comment):
                args["comment"] = node.comment
            if node.size is not None and (found is None or found.box_size != (node.size.width, node.size.height)):
                args["size"] = node.size.to_dto()
            if node.comment_bubble_visible is not None and (
                    found.comment_bubble_visible if found is not None else False) != node.comment_bubble_visible:
                args["comment_bubble_visible"] = node.comment_bubble_visible
            if args:
                self.body.append(PlanOp("graph.node.layout.set", doc.id, doc.node_path(node.id),
                                        {"node": node.id, **args}))


def same_json(have, wanted) -> bool:
    """A live property value holds an authored one: numbers compare to float precision (a colour channel
    0.1 reads back as the float 0.1000000015), other values exactly."""
    numbers = (int, float)
    if type(have) in numbers and type(wanted) in numbers:
        return abs(have - wanted) <= 1e-6 * max(1.0, abs(have), abs(wanted))
    if type(have) is dict and type(wanted) is dict:
        return have.keys() == wanted.keys() and all(same_json(have[key], wanted[key]) for key in have)
    if type(have) is list and type(wanted) is list:
        return len(have) == len(wanted) and all(same_json(a, b) for a, b in zip(have, wanted))
    return have == wanted


def user_pins_equal(live_pins, wanted: tuple[UserPin, ...]) -> bool:
    """Live user pins (LivePin tuple or None) hold exactly the wanted names and types, in order."""
    if live_pins is None:
        return False
    return len(live_pins) == len(wanted) and all(
        have.name == want.name and have.type == want.type for have, want in zip(live_pins, wanted))


_TYPE_ZERO = {"bool": "false", "int": "0", "int64": "0", "real": "0.0", "float": "0.0", "double": "0.0", "name": "None"}


def type_zero(pin_type) -> str | None:
    """The value a fresh pin of a plain scalar type holds (K2 SetPinAutogeneratedDefaultValueBasedOnType), or
    None for other types (an enum's first entry, a struct, an object)."""
    if pin_type.container != "none":
        return None
    if pin_type.category == "byte":
        return None if pin_type.sub_category_object else "0"
    return _TYPE_ZERO.get(pin_type.category)


def is_type_zero(pin_type, text: str) -> bool:
    """Whether ``text`` states the type's zero value (``""``, ``"0"``, ``"0.000000"``, ``"False"``, ``"None"``)."""
    zero = type_zero(pin_type)
    if zero is None:
        return False
    stripped = (text or "").strip()
    if not stripped:
        return True
    if zero == "false":
        return stripped.lower() == "false"
    if zero == "None":
        return stripped == "None"
    number = number_text(stripped)
    return number is not None and number == 0


def same_pin_value(pin, kind: str, payload: str) -> bool:
    """A live pin holds an authored value: the same text, or for a number the same number (``"1.0"`` and
    ``"1.000000"``); an empty real pin that matches its autogenerated default holds 0.0 (K2 reads it so)."""
    have = {"value": pin.default_value, "object": pin.default_object, "text": pin.default_text}[kind]
    if have == payload:
        return True
    if kind == "value" and getattr(pin, "default_display", "") and pin.default_display.casefold() == payload.casefold():
        return True  # an enum value named as the editor shows it
    if kind != "value" or type_zero(pin.type) is None or pin.type.category == "name":
        return False
    if not have and pin.is_autogenerated_default and pin.type.category in ("real", "float", "double"):
        have = "0.0"
    return same_scalar_text(have, payload)


def is_authored_default(pin, adapter: SchemaAdapter | None = None, defaults: bool = False,
                        terminator: bool = False) -> bool:
    """A live input default that extract reports and reconcile resets when omitted. ``defaults``:
    also when it equals the pin's autogenerated default (what Verify compares an authored value to).
    ``terminator``: a pin of a return node, whose value the editor keeps as that node's own user-pin default
    (its autogenerated default): a value there is the node's when it is not the type's zero value."""
    if not (pin.direction == "input" and not pin.hidden and not pin.orphaned and not pin.linked_to
            and pin.type.category != "exec" and bool(pin.default_value or pin.default_object or pin.default_text)
            and not (adapter is not None and adapter.ignores_default(pin))):
        return False
    if terminator and type_zero(pin.type) is not None:
        return defaults or not is_type_zero(pin.type, pin.default_value)
    return defaults or not pin.is_autogenerated_default


def _self_calls(desired: tuple[GraphDoc, ...], adapter: SchemaAdapter, self_class: str | None) -> frozenset[str]:
    """Names (casefolded) of the asset's own members that the document's call nodes call."""
    if not self_class:
        return frozenset()
    names = set()
    for graph in desired:
        for sub in graph.walk():
            for node in sub.nodes:
                if node.action is None:
                    continue
                try:
                    action = adapter.normalize_action(node.action, self_class=self_class, path="/Action")
                except AssetDocumentContractError:
                    continue  # reported where the node is planned
                parts = split_disambiguator(action)[0].partition("@")[0].split(":")
                if len(parts) == 4 and parts[1] == "CallFunction" and parts[2].casefold() == self_class.casefold():
                    names.add(parts[3].casefold())
    return frozenset(names)


def reconcile(desired: tuple[GraphDoc, ...], live: LiveAsset, *, managed_roles: frozenset[str],
              adapter: SchemaAdapter = GENERIC_ADAPTER, self_class: str | None = None,
              role_paths: Mapping[str, str] | None = None) -> Plan:
    """Diff top-level desired graphs against the live asset and return an ordered plan.

    Only live top-level graphs whose role is in ``managed_roles`` are candidates
    for matching and removal.  ``role_paths`` maps a role to its Body pointer
    (used for graph.remove diagnostics).
    """
    if type(desired) is not tuple or any(type(item) is not GraphDoc for item in desired):
        raise TypeError("reconcile requires a tuple of GraphDoc values")
    planner = _Planner(live, adapter, self_class, _self_calls(desired, adapter, self_class))
    candidates = tuple(g for g in live.graphs if g.parent_guid is None and g.role in managed_roles)
    pairs, surplus = planner.match_graphs(desired, candidates)
    for doc, found in pairs:
        planner.graph(doc, found, None)
    for graph in surplus:
        path = (role_paths or {}).get(graph.role, "/Body")
        if graph.can("remove"):
            planner.removals.append(PlanOp("graph.remove", "", path, {"graph_guid": graph.guid, "name": graph.name}))
        elif adapter.clears_kept_graphs:
            # A graph the editor does not let the user delete (the EventGraph, the construction script,
            # an interface function) stays, with every node a user can delete removed: a present region
            # is the complete state, so an unlisted graph holds nothing.
            planner.graph(GraphDoc(id=f"{graph.role}:{graph.guid}", role=graph.role, name=graph.name,
                                   schema_tag=adapter.tag, guid=graph.guid, path=path), graph, None)
    ops = planner.prelude + planner.definitions + planner.body + planner.removals
    if ops:
        ops.append(PlanOp("graph.finalize", "", "/Body", {}))
    return Plan(tuple(ops), desired, MappingProxyType(dict(planner.node_bindings)),
                MappingProxyType(dict(planner.graph_bindings)))


def reconcile_graph(doc: GraphDoc, live_graph: LiveGraph | None, live: LiveAsset, *,
                    adapter: SchemaAdapter = GENERIC_ADAPTER, self_class: str | None = None,
                    parent: GraphDoc | None = None) -> Plan:
    """Plan a single (sub)graph without finalize; used by the executor for deferred subgraphs."""
    planner = _Planner(live, adapter, self_class)
    planner.graph(doc, live_graph, parent)
    return Plan(tuple(planner.prelude + planner.body + planner.removals), (doc,),
                MappingProxyType(dict(planner.node_bindings)), MappingProxyType(dict(planner.graph_bindings)))


def _relayout_moves(doc: GraphDoc, live: LiveGraph, matched: Mapping[str, LiveNode], surplus, place: list[str],
                    keep: str | None) -> dict[str, tuple[int, int]]:
    """Where a relayout of existing nodes puts the ``place`` nodes that it moves (doc id -> top-left corner)."""
    guids = {node_id: found.guid for node_id, found in matched.items()}
    order = [guids[node.id] for node in doc.nodes if node.id in guids]
    stated = {guids[node.id]: (node.position.x, node.position.y) for node in doc.nodes
              if node.id in guids and node.position is not None}
    kept = set(order)
    removed = {node.guid for node in surplus}
    obstacles = [node.guid for node in live.nodes if node.guid not in kept and node.guid not in removed]
    moves = live_positions(live, order=order, movable=[guids[node_id] for node_id in place if node_id in guids],
                           positions=stated, obstacles=obstacles, keep=guids.get(keep) if keep is not None else None)
    ids = {guid: node_id for node_id, guid in guids.items()}
    output = {}
    for guid, (x, y) in moves.items():
        current = live.node(guid).position
        if current is None or (int(round(current.x)), int(round(current.y))) != (x, y):
            output[ids[guid]] = (x, y)
    return output


def pending_relayout(desired: tuple[GraphDoc, ...], live: LiveAsset, *, managed_roles: frozenset[str],
                     adapter: SchemaAdapter = GENERIC_ADAPTER, self_class: str | None = None) -> dict[str, list]:
    """Graphs (and subgraphs: collapsed graphs, an anim state's graph) with ``"Layout": "Auto"`` whose relayout
    would move nodes: doc path -> the moves (``{"Node": id, "Position": {"X", "Y"}}``).

    For a Diff (an empty Diff proves an apply changes nothing): ``Layout`` is a request, not state, so only
    laying the live graph out tells whether it is already laid out.  ``live`` must carry node sizes
    (``include_sizes``).  Document nodes pair with live nodes as apply pairs them, and nodes of the same
    action without a NodeGuid (which apply replaces) pair up in order: the question is where the graph's
    nodes are, not which ones apply would recreate.  Subgraphs pair as apply pairs them (by GraphGuid, owner
    node or name).  A graph or node apply creates is a difference of its own."""
    if not any(graph.layout == "Auto" for doc in desired for graph in doc.walk()):
        return {}
    planner = _Planner(live, adapter, self_class)
    candidates = tuple(g for g in live.graphs if g.parent_guid is None and g.role in managed_roles)
    moved: dict[str, list] = {}

    def visit(pairs) -> None:
        for doc, found in pairs:
            if found is None:
                continue
            doc = planner._localize(doc, found)
            match = planner.match_nodes(doc, found)
            matched = dict(match.matched)
            surplus = list(match.surplus)
            loose: dict[tuple, list[LiveNode]] = {}
            for node in surplus:
                key = adapter.live_semantic_key(node)
                if key is not None:
                    loose.setdefault(key, []).append(node)
            for node in doc.nodes:
                if node.id in matched or node.implicit is not None:
                    continue
                key = adapter.semantic_key(node, planner.action(node, doc.node_path(node.id)))
                options = loose.get(key) if key is not None else None
                if options:
                    matched[node.id] = options.pop(0)
            for node_id, node in matched.items():
                planner.node_bindings[(doc.id, node_id)] = node.guid
            if doc.layout == "Auto":
                surplus = [node for node in surplus if node not in matched.values()]
                place = [node.id for node in doc.nodes if node.position is None and node.id in matched]
                keep = next((node_id for node_id in place if matched[node_id].position is not None), None)
                moves = _relayout_moves(doc, found, matched, surplus, place, keep)
                if moves:
                    moved[doc.path] = [{"Node": node_id, "Position": {"X": x, "Y": y}}
                                       for node_id, (x, y) in moves.items()]
            if any(child.layout == "Auto" for sub in doc.subgraphs for child in sub.walk()):
                visit(planner.match_graphs(doc.subgraphs, planner.live.children(found.guid), doc)[0])

    try:
        visit(planner.match_graphs(desired, candidates)[0])
    except AssetDocumentContractError:
        return {}  # the document's other differences (or its apply) report the problem
    return moved
