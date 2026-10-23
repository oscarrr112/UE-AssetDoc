# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Animation Blueprint graph schema adapter and Body codec (AnimGraph, anim layers, state machines).

Body shape::

    "AnimGraph": {"Nodes": [...], "Links": [...]},
    "AnimLayers": [{"Name": "UpperBody", "Nodes": [...], "Links": [...]}]

Nodes use the K2 node fields (``Id``, ``Action``, ``PinOverrides``, ``Properties``,
``Position``, ``Name``, ...) plus:

* ``Graph`` -- the graph a node owns, nested: a state machine's states and
  transitions, a state's pose graph, a transition's or conduit's rule graph
  (``{"Nodes": [...], "Links": [...]}``).
* ``From`` / ``To`` -- on a ``SM:Transition`` node, the ids of the states it
  connects (sugar for the links ``From:Out -> T:In`` and ``T:Out -> To:In``).
* ``BlendGraph`` -- on a ``SM:Transition`` node, its custom blend graph (Transition
  Details > Blend Logic = Custom; implies ``Properties.LogicType = "TLT_Custom"``).
  Reserved ids ``Output`` (the result), ``SourcePose`` and ``DestinationPose``.
* ``Samples`` -- on a Blend Space Graph node (``Anim:BlendSpaceGraph:BlendSpace``,
  ``Anim:BlendSpaceGraph:BlendSpace1D``, ``Anim:RotationOffsetBlendSpaceGraph:AimOffsetBlendSpace``
  ...): ``[{"Name": "Idle", "X": 0, "Y": 0, "Graph": {...}}]``, the blend samples (``Y`` only
  for 2D blend spaces) and each sample's pose graph (reserved id ``Output``).  A sample is
  identified by its name; the list is complete (unlisted samples are removed).  The blend
  space's axis and smoothing settings are node ``Properties``
  (``"BlendSpace.BlendParameters.X.Max"``, ``"BlendSpace.InterpolationParam.X.InterpolationTime"``,
  ``"BlendSpace.bInterpolateUsingGrid"`` ...).

Actions: ``Anim:<Kind>`` for anim graph nodes (``Anim:SequencePlayer``,
``Anim:StateMachine``, ``Anim:LayeredBoneBlend`` ...), ``Anim:SaveCachedPose:<Cache>``,
``Anim:UseCachedPose:<Cache>``, ``Anim:LinkedAnimLayer:<Layer>``,
``Anim:BlendSpaceGraph:<BlendSpaceClass>``, ``Anim:LinkedInputPose`` (a layer's input
pose; ``Properties`` ``Node.Name`` and ``Inputs``); ``SM:State``,
``SM:Conduit``, ``SM:StateAlias``, ``SM:Transition`` in state machines; ``K2:...``
for Blueprint nodes (variable getters, math, rule getters).

Reserved node ids: ``Output`` (the Output Pose / state result node) in anim, layer
and state graphs, ``Entry`` in state machines, ``Result`` in transition and conduit
rule graphs.  State machines, states, conduits and aliases are named by ``Name``
(default: the node ``Id``).  Node ``Properties`` are the node's details-panel values:
``"Node.<Field>"`` (the anim node struct), top-level properties, ``"ShowPin.<Field>"``
(expose a property as a pin) and ``"Bindings"`` (``{"PlayRate": {"Path": "Speed"}}``; a path
ending in a pure function binds the function); an anim layer's ``Output`` takes
``"LayerGroup"`` (``""`` = ungrouped); extraction reports every non-default value.
"""
from __future__ import annotations

from dataclasses import replace
import re

from asset_document.diagnostics import contract_error
from asset_document.graphs.actions import action_kind
from asset_document.graphs.live import LiveGraph, LiveNode
from asset_document.graphs.model import Endpoint, GraphDoc, LinkDoc, NodeDoc, NodeProperties, thaw_json
from asset_document.graphs.schemas import SchemaAdapter, sanitize_id
from asset_document.graphs.schemas import k2
from asset_document.graphs.schemas.k2 import K2SchemaAdapter, encode_node


CUSTOM_BLEND = "custom_transition"
SAMPLE = "blend_space_sample"
ANIM_ROLES = frozenset({"anim_graph", "anim_layer", "state_machine", "state", "transition", "conduit",
                        CUSTOM_BLEND, SAMPLE})
# Roles of graphs the Body nests under their owner node ("Graph"); a transition's custom blend
# graph is its "BlendGraph", a blend space's sample graphs are in its "Samples".
NESTED_ROLES = {"Anim:StateMachine": "state_machine", "SM:State": "state", "SM:Conduit": "conduit",
                "SM:Transition": "transition"}
RESERVED_BY_ROLE = {
    "anim_graph": {"Output": "root"}, "anim_layer": {"Output": "root"}, "state": {"Output": "root"},
    "state_machine": {"Entry": "sm_entry"}, "transition": {"Result": "result"}, "conduit": {"Result": "result"},
    CUSTOM_BLEND: {"Output": "root", "SourcePose": "source_pose", "DestinationPose": "destination_pose"},
    SAMPLE: {"Output": "root"},
}
SCHEMA_TAG_BY_ROLE = {"state_machine": "SM"}
BLEND_SPACE_KINDS = ("Anim:BlendSpaceGraph", "Anim:RotationOffsetBlendSpaceGraph")
NAMED_ACTIONS = frozenset({"SM:State", "SM:Conduit", "SM:StateAlias", "Anim:StateMachine", *BLEND_SPACE_KINDS})
TRANSITION = "SM:Transition"
CUSTOM_LOGIC = "TLT_Custom"
# A layer's input poses are told apart by their pose name (FAnimNode_LinkedInputPose::Name).
INPUT_POSE = "Anim:LinkedInputPose"
DEFAULT_INPUT_POSE = "InPose"
_ANIM_NODE_FIELDS = k2._NODE_FIELDS | {"Graph", "From", "To", "BlendGraph", "Samples"}
_SAMPLE_FIELDS = frozenset({"Name", "X", "Y", "Graph"})
# Characters an object (graph) name cannot contain (INVALID_OBJECTNAME_CHARACTERS).
_INVALID_SAMPLE_NAME = re.compile(r"[\"' ,/.:|&!~\n\r\t@#(){}\[\]=;^%$`]")
_DUPLICATE_SUFFIX = re.compile(r" #\d+\Z")
# "Layout": "Auto" lays out every node without a Position (as in K2 graphs), in any anim graph or nested graph.
_NESTED_FIELDS = frozenset({"GraphGuid", "Nodes", "Links", "Layout"})
_TOP_FIELDS = frozenset({"Name", "GraphGuid", "Nodes", "Links", "Layout"})
# A Control Rig node's rig: some engine versions name it by class ("Node.ControlRigClass": <class path>), others by
# an asset reference ("Node.ControlRigAssetReference": {"BlueprintRigClass": <class path>, "ControlRigAsset": <rig
# asset>}). A document may use either spelling on any engine: graphs compare the reference spelling (a class is the
# reference {"BlueprintRigClass": <class>, "ControlRigAsset": null}), the editor writes the property its node has and
# extraction reports the node's own spelling.
CONTROL_RIG = "Anim:ControlRig"
RIG_CLASS = "Node.ControlRigClass"
RIG_REFERENCE = "Node.ControlRigAssetReference"


def rig_reference_spelling(properties, path: str | None = None):
    """A Control Rig node's settings with its rig class stated as the rig's asset reference (see RIG_CLASS).
    A document (``path`` given) that states both must name one rig; a live node that reports both (an engine whose
    node keeps the retired class field next to the reference) is read by its reference."""
    if not hasattr(properties, "items") or RIG_CLASS not in properties:
        return properties
    reference = {"BlueprintRigClass": thaw_json(properties[RIG_CLASS]), "ControlRigAsset": None}
    out = {key: thaw_json(value) for key, value in properties.items() if key != RIG_CLASS}
    if RIG_REFERENCE in out:
        if path is not None and out[RIG_REFERENCE] != reference:
            raise contract_error(f"{path}/{RIG_CLASS}", "Graph.ConflictingSetting",
                                 f"{RIG_CLASS} and {RIG_REFERENCE} name different rigs; state the rig once.")
        return out
    out[RIG_REFERENCE] = reference
    return out


def is_anim_action(key: str | None) -> bool:
    return bool(key) and (key.startswith("Anim:") or key.startswith("SM:"))


def _base(key: str | None) -> str | None:
    return key.partition("#")[0] if key else key


def _kind(key: str | None) -> str | None:
    """The node kind of an action key: a Blend Space Graph key without its blend space class."""
    base = _base(key)
    for kind in BLEND_SPACE_KINDS:
        if base is not None and base.startswith(kind + ":"):
            return kind
    return base


def is_blend_space_action(key: str | None) -> bool:
    return _kind(key) in BLEND_SPACE_KINDS


def sample_name(graph_name: str) -> str:
    """The sample a blend space sample graph's document name ("<BlendSpace>/<Sample>[ #n]") stands for."""
    return _DUPLICATE_SUFFIX.sub("", graph_name).rsplit("/", 1)[-1]


def _input_pose_key(properties) -> tuple:
    name = properties.get("Node.Name", DEFAULT_INPUT_POSE) if hasattr(properties, "get") else DEFAULT_INPUT_POSE
    return (INPUT_POSE, str(name).casefold())


def _sorted_samples(value):
    rows = thaw_json(value)
    if type(rows) is list and all(type(row) is dict and type(row.get("Name")) is str for row in rows):
        return sorted(rows, key=lambda row: row["Name"].casefold())
    return rows


# --------------------------------------------------------------------------
# Adapter
# --------------------------------------------------------------------------
def omitted_anim_settings(live, defaults, authored, linked_pins=frozenset()) -> dict:
    """The live non-default settings of an animation node that ``authored`` leaves out, mapped to their
    defaults. Only settings with a reported default qualify (structural ones such as blend space samples
    or a layer group have none and are kept)."""
    if not live or not hasattr(live, "items") or not defaults or not hasattr(defaults, "items"):
        return {}
    # A pin the document links or binds stays shown (the link / binding needs it).
    bindings = authored.get("Bindings") if hasattr(authored, "get") else None
    kept_pins = set(linked_pins) | {str(name).casefold() for name in (bindings if hasattr(bindings, "keys") else ())}
    out = {}
    for key, value in live.items():
        if key in authored or key not in defaults or thaw_json(defaults[key]) == thaw_json(value):
            continue
        if key.startswith("ShowPin.") and key[len("ShowPin."):].casefold() in kept_pins:
            continue
        out[key] = defaults[key]
    return out


class AnimSchemaAdapter(K2SchemaAdapter):
    """K2 adapter (Blueprint graphs of the ABP) extended with Anim:/SM: node semantics."""

    tag = "Anim"
    implicit_ids = {"entry": "Entry", "result": "Result", "root": "Output", "sm_entry": "Entry",
                    "source_pose": "SourcePose", "destination_pose": "DestinationPose"}
    owned_subgraphs_follow_owner = True

    def semantic_key(self, node, normalized_action):
        if not is_anim_action(normalized_action):
            return super().semantic_key(node, normalized_action)
        base = _base(normalized_action)
        if base == TRANSITION:
            return (TRANSITION, node.extras.get("from"), node.extras.get("to"))
        if base == INPUT_POSE:
            return _input_pose_key(node.properties.node)
        return (base, node.name if _kind(base) in NAMED_ACTIONS else None)

    def live_semantic_key(self, node):
        if not is_anim_action(node.action_key):
            return super().live_semantic_key(node)
        base = _base(node.action_key)
        if base == TRANSITION:
            return (TRANSITION, node.extras.get("from"), node.extras.get("to"))
        if base == INPUT_POSE:
            return _input_pose_key(self.live_properties(node, "node"))
        return (base, self.live_name(node) if _kind(base) in NAMED_ACTIONS else None)

    def matches_in_order(self, key):
        # The editor allows several transitions between the same two states (each drag from A to B adds
        # one) and they have no name: "the n-th A->B transition" is their identity, in document order on
        # one side and graph order on the other (Extract writes them in graph order, " #n" in the rule
        # graph name). A NodeGuid, when written, still wins.
        return bool(key) and key[0] == TRANSITION

    def live_name(self, node):
        if is_anim_action(node.action_key):
            return SchemaAdapter.live_name(self, node)
        return super().live_name(node)

    def extract_name(self, node):
        if is_anim_action(node.action_key):
            return self.live_name(node) if _kind(node.action_key) in NAMED_ACTIONS else None
        return super().extract_name(node)

    def node_matches_live(self, node, live):
        # A transition is identified by the states it connects: rewiring it is a new transition
        # (the editor destroys a transition that loses a connection).
        if _base(node.action) == TRANSITION and "from" in node.extras:
            return (node.extras.get("from"), node.extras.get("to")) == (live.extras.get("from"), live.extras.get("to"))
        return True

    def spawn_priority(self, node):
        # Use Cached Pose nodes need their Save Cached Pose node.
        return 0 if node.action and node.action.startswith("Anim:SaveCachedPose") else 1

    def user_pins(self, node, graph_role):
        if graph_role in ANIM_ROLES:
            return ()
        return super().user_pins(node, graph_role)

    def live_properties(self, node, container):
        if container != "node" or not is_anim_action(node.action_key) and node.implicit_role is None:
            return super().live_properties(node, container)
        output = dict(node.extras.get("properties") or {})
        if "Samples" in output:
            output["Samples"] = _sorted_samples(output["Samples"])
        for key, value in node.property_container("node").items():
            if key == "Node" and hasattr(value, "items"):
                for field, item in value.items():
                    output.setdefault(f"Node.{field}", item)
            else:
                output.setdefault(key, value)
        if _base(node.action_key) == CONTROL_RIG:
            output = rig_reference_spelling(output)
        return output

    def missing_pin_hint(self, node, pin):
        if not (node.action_key or "").startswith("Anim:"):
            return super().missing_pin_hint(node, pin)
        return (f' An animation node shows an optional pin (a Control Rig node: a public variable or control of its rig) '
                f'once its Properties have "ShowPin.{pin}": true.')

    def hides_pin(self, action, container, key, value):
        # "ShowPin.<Property>": false removes the pin (a Control Rig node rebuilds without orphan pins).
        return container == "node" and key.startswith("ShowPin.") and value is False \
            and (action or "").startswith("Anim:")

    def omitted_property_resets(self, node, doc, linked_pins):
        # An animation node's settings (Details panel) are the node class defaults unless the document
        # states them: a setting the document leaves out goes back to its default, as "reset to default"
        # in the Details panel. A pin the document links stays shown. State machine nodes (states,
        # conduits, aliases, transitions) likewise, against a newly placed node.
        if not (node.action_key or "").startswith(("Anim:", "SM:")):
            return {}
        live, defaults = node.extras.get("properties"), node.extras.get("property_defaults")
        if _base(node.action_key) == CONTROL_RIG:
            live, defaults = rig_reference_spelling(live), rig_reference_spelling(defaults)
        return omitted_anim_settings(live, defaults, doc.properties.node, linked_pins)

    def extract_properties(self, node, hint):
        properties = node.extras.get("properties")
        if properties is None or not hasattr(properties, "items"):
            return None
        output = thaw_json(properties)
        if "Samples" in output:
            output["Samples"] = _sorted_samples(output["Samples"])
        return NodeProperties(output)

    def subgraph_identity(self, graph_role: str, name: str) -> str | None:
        # A blend space owns one sample graph per sample: the sample name tells them apart.
        return sample_name(name) if graph_role == SAMPLE else None

    def subgraph_name(self, graph: LiveGraph, owner: LiveNode | None) -> str:
        if owner is None:
            return graph.name
        if graph.role == SAMPLE:
            return f"{self.live_name(owner) or owner.guid}/{graph.name}"
        if _base(owner.action_key) == TRANSITION:
            name = f"{owner.extras.get('from', '?')}->{owner.extras.get('to', '?')}"
            return name if graph.role == "transition" else f"{name} ({graph.role})"
        name = owner.extras.get("name")
        return name if type(name) is str and name else graph.name

    def node_base_id(self, node: LiveNode) -> str:
        key = _base(node.action_key)
        if node.implicit_role or not is_anim_action(key):
            return super().node_base_id(node)
        if key == TRANSITION:
            return sanitize_id(f"{node.extras.get('from', 'State')}_To_{node.extras.get('to', 'State')}")
        name = self.live_name(node)
        if name:
            return sanitize_id(name)
        parts = key.split(":", 2)
        return sanitize_id(f"{parts[1]}_{parts[2]}" if len(parts) == 3 else parts[1])

    def extract_action(self, key, *, self_class=None):
        return key if is_anim_action(key) else super().extract_action(key, self_class=self_class)


ANIM_ADAPTER = AnimSchemaAdapter()


def anim_label(node: NodeDoc) -> tuple:
    """Identity label for structural comparison: named Anim/SM nodes compare by name too."""
    action = _base(node.action) or ""
    if _kind(action) in NAMED_ACTIONS or (action == TRANSITION and node.extras):
        name = node.name or node.id if action != TRANSITION else f"{node.extras.get('from')}->{node.extras.get('to')}"
    else:
        action = k2.node_identity_action(action) or ""
        name = node.name if node.name and action_kind(action) in k2.NAMED_KINDS else ""
    return (node.implicit or "", action, name.casefold() if name else "")


# --------------------------------------------------------------------------
# Decode (Body -> GraphDoc)
# --------------------------------------------------------------------------
def _state_name(node: NodeDoc) -> str:
    return node.name if node.name else node.id


def _decode_node(value: object, path: str, role: str, self_class: str | None) -> tuple[NodeDoc, dict]:
    row = k2._object(value, path)
    k2._known(row, _ANIM_NODE_FIELDS, path)
    extra = {key: row[key] for key in ("Graph", "From", "To", "BlendGraph", "Samples") if key in row}
    core = {key: item for key, item in row.items() if key not in extra}
    if "BlendGraph" in extra or "Samples" in extra:
        core["Properties"] = _implied_properties(row, extra, path)
    reserved = RESERVED_BY_ROLE.get(role, {})
    if core.get("Id") in reserved and "Action" not in core and "Class" not in core and "Implicit" not in core:
        core["Implicit"] = reserved[core["Id"]]
    node = k2._decode_node(core, path, self_class)
    action = _base(node.action)
    if action == CONTROL_RIG and RIG_CLASS in node.properties.node:
        node = replace(node, properties=NodeProperties(rig_reference_spelling(node.properties.node, f"{path}/Properties"),
                                                       node.properties.template))
    if node.name is None and _kind(action) in NAMED_ACTIONS:
        node = replace(node, name=node.id)  # states and state machines are named after their id by default
    if ("From" in extra or "To" in extra) and action != TRANSITION:
        raise contract_error(f"{path}/From", "Graph.UnknownField", "From/To are only valid on SM:Transition nodes.")
    if action == TRANSITION and ("From" not in extra or "To" not in extra):
        raise contract_error(f"{path}/From", "Graph.MissingField", "A SM:Transition needs From and To state ids.")
    if "Graph" in extra and action not in NESTED_ROLES:
        raise contract_error(f"{path}/Graph", "Graph.UnknownField",
                             f"Only {', '.join(sorted(NESTED_ROLES))} nodes own a nested Graph.")
    if "BlendGraph" in extra and action != TRANSITION:
        raise contract_error(f"{path}/BlendGraph", "Graph.UnknownField",
                             "Only SM:Transition nodes own a custom BlendGraph.")
    if "Samples" in extra and not is_blend_space_action(action):
        raise contract_error(f"{path}/Samples", "Graph.UnknownField",
                             "Only Blend Space Graph nodes (Anim:BlendSpaceGraph:<Class>) have Samples.")
    return node, extra


def _implied_properties(row: dict, extra: dict, path: str) -> dict:
    """Node Properties with what BlendGraph / Samples state: LogicType = TLT_Custom, the sample points."""
    properties = dict(k2._object(row.get("Properties", {}), f"{path}/Properties"))
    if "BlendGraph" in extra:
        logic = properties.setdefault("LogicType", CUSTOM_LOGIC)
        if logic != CUSTOM_LOGIC:
            raise contract_error(f"{path}/Properties/LogicType", "Graph.ConflictingSetting",
                                 f"A transition with a BlendGraph has LogicType {CUSTOM_LOGIC} (not {logic}).")
    if "Samples" in extra:
        if "Samples" in properties:
            raise contract_error(f"{path}/Properties/Samples", "Graph.ConflictingSetting",
                                 "Blend space samples are the node's Samples, not a property.")
        properties["Samples"] = [{key: item for key, item in sample.items() if key != "Graph"}
                                 for sample in _samples(extra["Samples"], f"{path}/Samples")]
    return properties


def _samples(value: object, path: str) -> list[dict]:
    rows, names = [], set()
    for index, item in enumerate(k2._array(value, path)):
        item_path = f"{path}/{index}"
        row = k2._object(item, item_path)
        k2._known(row, _SAMPLE_FIELDS, item_path)
        name = k2._string(row, "Name", item_path, required=True)
        if _INVALID_SAMPLE_NAME.search(name):
            raise contract_error(f"{item_path}/Name", "Graph.InvalidGraphName",
                                 f"Sample name '{name}' names its sample graph: no spaces or characters such as . / # : ( )")
        if name.casefold() in names:
            raise contract_error(f"{item_path}/Name", "Graph.DuplicateGraphName", f"Sample '{name}' is listed twice.")
        names.add(name.casefold())
        for axis in ("X", "Y"):
            number = row.get(axis)
            if (axis == "X" or axis in row) and (type(number) not in (int, float) or type(number) is bool):
                raise contract_error(f"{item_path}/{axis}", "Graph.InvalidType", f"Sample {axis} is a number.")
        if "Graph" in row:
            k2._object(row["Graph"], f"{item_path}/Graph")
        rows.append(row)
    return sorted(rows, key=lambda row: row["Name"].casefold())


def _decode_graph(value: object, path: str, *, graph_id: str, role: str, name: str, self_class: str | None,
                  top: bool, owner: str | None = None) -> GraphDoc:
    row = k2._object(value, path)
    k2._known(row, _TOP_FIELDS if top else _NESTED_FIELDS, path)
    decoded = [_decode_node(item, f"{path}/Nodes/{index}", role, self_class)
               for index, item in enumerate(k2._array(row.get("Nodes", []), f"{path}/Nodes"))]
    nodes = [node for node, _ in decoded]
    by_id = {node.id: node for node in nodes}
    links = list(k2._decode_links(row.get("Links", []), f"{path}/Links"))
    declared = set(by_id)
    for link in links:
        for endpoint in (link.from_, link.to):
            implicit = RESERVED_BY_ROLE.get(role, {}).get(endpoint.node)
            if implicit is not None and endpoint.node not in declared:
                nodes.append(NodeDoc(id=endpoint.node, implicit=implicit))
                declared.add(endpoint.node)
    subgraphs, names = [], {}
    for index, (node, extra) in enumerate(decoded):
        node_path = f"{path}/Nodes/{index}"
        if _base(node.action) == TRANSITION:
            ends = []
            for side in ("From", "To"):
                target = extra[side]
                if type(target) is not str or target not in by_id or target == node.id:
                    raise contract_error(f"{node_path}/{side}", "Graph.UnknownNode",
                                         f"{side} must be the Id of a state, conduit or alias in this state machine.")
                ends.append(by_id[target])
            links.append(LinkDoc(Endpoint(ends[0].id, "Out"), Endpoint(node.id, "In")))
            links.append(LinkDoc(Endpoint(node.id, "Out"), Endpoint(ends[1].id, "In")))
            node = replace(node, extras={**thaw_json(node.extras), "from": _state_name(ends[0]),
                                         "to": _state_name(ends[1])})
            nodes[nodes.index(by_id[node.id])] = node
            by_id[node.id] = node
        nested = []
        if "Graph" in extra:
            child_role = NESTED_ROLES[_base(node.action)]
            nested.append((extra["Graph"], f"{node_path}/Graph", child_role,
                           f"{node.extras['from']}->{node.extras['to']}" if child_role == "transition"
                           else _state_name(node)))
        if "BlendGraph" in extra:
            nested.append((extra["BlendGraph"], f"{node_path}/BlendGraph", CUSTOM_BLEND,
                           f"{node.extras['from']}->{node.extras['to']} ({CUSTOM_BLEND})"))
        for sample_index, sample in enumerate(_samples(extra.get("Samples", []), f"{node_path}/Samples")):
            if "Graph" in sample:
                index_in_doc = next(index for index, item in enumerate(extra["Samples"])
                                    if type(item) is dict and item.get("Name") == sample["Name"])
                nested.append((sample["Graph"], f"{node_path}/Samples/{index_in_doc}/Graph", SAMPLE,
                               f"{_state_name(node)}/{sample['Name']}"))
        for child_value, child_path, child_role, child_name in nested:
            folded = child_name.casefold()
            names[folded] = names.get(folded, 0) + 1
            if names[folded] > 1:
                child_name = f"{child_name} #{names[folded]}"
            subgraphs.append(_decode_graph(child_value, child_path, graph_id=f"{graph_id}/{child_name}",
                                           role=child_role, name=child_name, self_class=self_class, top=False,
                                           owner=node.id))
    layout = row.get("Layout")
    if layout is not None and type(layout) is not str:
        raise contract_error(f"{path}/Layout", "Graph.InvalidLayout",
                             "Layout is \"Auto\" (lay out every node without a Position) or omitted.")
    try:
        return GraphDoc(id=graph_id, role=role, name=name, schema_tag=SCHEMA_TAG_BY_ROLE.get(role, "Anim"),
                        guid=row.get("GraphGuid"), nodes=tuple(nodes), links=tuple(links),
                        subgraphs=tuple(subgraphs), owner=owner, path=path, layout=layout)
    except Exception as error:
        if hasattr(error, "diagnostic"):
            raise k2._rebase(error, path) from None
        raise


def decode_anim_graph(value: object, *, self_class: str | None = None) -> GraphDoc:
    """Decode ``Body.AnimGraph``."""
    row = k2._object(value, "/Body/AnimGraph")
    if "Name" in row and row["Name"] != "AnimGraph":
        raise contract_error("/Body/AnimGraph/Name", "Graph.InvalidGraphName", "The anim graph is always named AnimGraph.")
    return _decode_graph(value, "/Body/AnimGraph", graph_id="AnimGraph/AnimGraph", role="anim_graph",
                         name="AnimGraph", self_class=self_class, top=True)


def decode_anim_layers(value: object, *, self_class: str | None = None) -> tuple[GraphDoc, ...]:
    """Decode ``Body.AnimLayers``."""
    output, names = [], {}
    for index, item in enumerate(k2._array(value, "/Body/AnimLayers")):
        path = f"/Body/AnimLayers/{index}"
        name = k2._string(k2._object(item, path), "Name", path, required=True)
        if name.casefold() in names or name == "AnimGraph":
            raise contract_error(f"{path}/Name", "Graph.DuplicateGraphName", f"Anim layer '{name}' is declared twice.")
        names[name.casefold()] = index
        output.append(_decode_graph(item, path, graph_id=f"AnimLayers/{name}", role="anim_layer", name=name,
                                    self_class=self_class, top=True))
    return tuple(output)


# --------------------------------------------------------------------------
# Encode (GraphDoc -> Body)
# --------------------------------------------------------------------------
def _encode_graph(graph: GraphDoc, *, top: bool) -> dict:
    output: dict = {}
    if top and graph.role == "anim_layer":
        output["Name"] = graph.name
    if graph.guid is not None:
        output["GraphGuid"] = graph.guid
    transitions = {node.id for node in graph.nodes if _base(node.action) == TRANSITION}
    ends: dict[str, dict] = {node_id: {} for node_id in transitions}
    links = []
    for link in graph.links:
        if link.to.node in transitions and link.to.pin.casefold() == "in" and "From" not in ends[link.to.node]:
            ends[link.to.node]["From"] = link.from_.node
        elif link.from_.node in transitions and link.from_.pin.casefold() == "out" and "To" not in ends[link.from_.node]:
            ends[link.from_.node]["To"] = link.to.node
        else:
            links.append(link)
    nodes = []
    for node in graph.nodes:
        extras = {key: item for key, item in thaw_json(node.extras).items() if key not in ("from", "to")}
        owned = [sub for sub in graph.subgraphs if sub.owner == node.id]
        blend = next((sub for sub in owned if sub.role == CUSTOM_BLEND), None)
        properties = thaw_json(node.properties.node)
        samples = properties.pop("Samples", None) if is_blend_space_action(node.action) else None
        if blend is not None and properties.get("LogicType") == CUSTOM_LOGIC:
            del properties["LogicType"]  # implied by the BlendGraph
        row = encode_node(replace(node, extras=extras,
                                  properties=NodeProperties(properties, thaw_json(node.properties.template))))
        if RESERVED_BY_ROLE.get(graph.role, {}).get(node.id) == node.implicit:
            row.pop("Implicit", None)
        if row.get("Name") == node.id and _kind(node.action) in NAMED_ACTIONS:
            del row["Name"]
        if node.id in transitions:
            row.update(ends[node.id])
        role = NESTED_ROLES.get(_base(node.action))
        child = next((sub for sub in owned if sub.role == role), None)
        if child is not None:
            row["Graph"] = _encode_graph(child, top=False)
        if blend is not None:
            row["BlendGraph"] = _encode_graph(blend, top=False)
        if type(samples) is list:
            rows = []
            for sample in _sorted_samples(samples):
                sample = dict(sample)
                graph_doc = next((sub for sub in owned if sub.role == SAMPLE
                                  and sample_name(sub.name).casefold() == str(sample.get("Name", "")).casefold()), None)
                if graph_doc is not None:
                    sample["Graph"] = _encode_graph(graph_doc, top=False)
                rows.append(sample)
            row["Samples"] = rows
        nodes.append(row)
    output["Nodes"] = nodes
    output["Links"] = [{"From": str(link.from_), "To": str(link.to)} for link in links]
    return output


def encode_anim_graph(graph: GraphDoc) -> dict:
    return _encode_graph(graph, top=True)


__all__ = ("ANIM_ADAPTER", "ANIM_ROLES", "AnimSchemaAdapter", "decode_anim_graph", "decode_anim_layers",
           "encode_anim_graph", "is_anim_action", "is_blend_space_action", "anim_label", "sample_name")
