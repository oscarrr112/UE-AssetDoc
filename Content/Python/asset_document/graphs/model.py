"""Schema-agnostic, immutable desired-state graph model.

The model is shared by every graph schema (K2, BehaviorTree, state machines,
...).  Schema specifics live in ``graphs.schemas.*`` codecs and adapters; this
module only knows graphs, nodes, links, pin defaults, signatures and
subgraphs.  Every value is validated on construction; graph-level diagnostics
carry the Body JSON pointer stored in ``GraphDoc.path`` so the AI can locate
its mistake.
"""
from __future__ import annotations

from dataclasses import dataclass, field
import re
from types import MappingProxyType
from typing import Mapping

from asset_document.diagnostics import contract_error


GRAPH_ROLES = frozenset({
    "ubergraph", "function", "macro", "delegate_signature", "anim_graph", "anim_layer", "state_machine",
    "state", "transition", "custom_transition", "conduit", "blend_space_sample", "bt_root", "other",
})
# (graph role, implicit role) pairs a graph may hold more than once: a function may have several
# return nodes (the editor places extra ones from the palette and keeps their pins in sync).
REPEATABLE_IMPLICIT_ROLES = frozenset({("function", "result")})
PIN_DIRECTIONS = ("input", "output")
PIN_CONTAINERS = ("none", "array", "set", "map")
_ID = re.compile(r"[A-Za-z_][A-Za-z0-9_-]*\Z")
_TOKEN = re.compile(r"[a-z][a-z0-9_]*\Z")
_TAG = re.compile(r"[A-Za-z][A-Za-z0-9]*\Z")
_GUID = re.compile(r"[0-9A-Fa-f]{32}\Z")


def normalize_guid(value: object, path: str, code: str = "Graph.InvalidGuid") -> str | None:
    """GUIDs are 32 hex digits without hyphens; input is case-insensitive, storage is upper case."""
    if value is None:
        return None
    if type(value) is not str or not _GUID.fullmatch(value):
        raise contract_error(path, code, "GUIDs must be 32 hexadecimal digits without hyphens.")
    return value.upper()


def require_id(value: object, path: str, code: str = "Graph.InvalidId") -> str:
    if type(value) is not str or not _ID.fullmatch(value):
        raise contract_error(path, code,
                             "Ids must start with a letter or '_' and contain only letters, digits, '_' or '-'.")
    return value


def require_text(value: object, path: str, code: str = "Graph.InvalidType", *, allow_empty: bool = False) -> str:
    if type(value) is not str or (not value and not allow_empty):
        raise contract_error(path, code, "A string is required." if allow_empty else "A non-empty string is required.")
    return value


def freeze_json(value: object, path: str = "/") -> object:
    """Deep-freeze a JSON value: objects become read-only mappings, arrays tuples."""
    if value is None or type(value) in (bool, str, int):
        return value
    if type(value) is float:
        if value != value or value in (float("inf"), float("-inf")):
            raise contract_error(path, "Graph.InvalidJson", "Non-finite numbers are not JSON.")
        return value
    if type(value) in (list, tuple):
        return tuple(freeze_json(item, f"{path}/{index}") for index, item in enumerate(value))
    if type(value) in (dict, MappingProxyType):
        output = {}
        for key, item in value.items():
            if type(key) is not str:
                raise contract_error(path, "Graph.InvalidJson", "JSON object keys must be strings.")
            output[key] = freeze_json(item, f"{path}/{key}")
        return MappingProxyType(output)
    raise contract_error(path, "Graph.InvalidJson", "Only JSON values are admitted.")


def thaw_json(value: object) -> object:
    if type(value) is tuple:
        return [thaw_json(item) for item in value]
    if type(value) is MappingProxyType:
        return {key: thaw_json(item) for key, item in value.items()}
    return value


_EMPTY = MappingProxyType({})


@dataclass(frozen=True, slots=True)
class Position:
    x: float
    y: float

    def __post_init__(self) -> None:
        for value in (self.x, self.y):
            if type(value) not in (int, float) or value != value or abs(value) == float("inf"):
                raise contract_error("/Position", "Graph.InvalidPosition", "Position needs finite numeric X and Y.")
        object.__setattr__(self, "x", float(self.x))
        object.__setattr__(self, "y", float(self.y))

    def to_dto(self) -> dict:
        return {"x": int(round(self.x)), "y": int(round(self.y))}


@dataclass(frozen=True, slots=True)
class Size:
    """A resizable node's size (a comment box), in graph units."""
    width: int
    height: int

    def __post_init__(self) -> None:
        for value in (self.width, self.height):
            if type(value) is not int or not 1 <= value <= 100000:
                raise contract_error("/", "Graph.InvalidSize", "Size needs integer Width and Height from 1 to 100000.")

    def to_dto(self) -> dict:
        return {"width": self.width, "height": self.height}


# Graph layout requests: "Auto" lays out every node the document gives no Position (graph.layout.auto).
LAYOUT_MODES = ("Auto",)


@dataclass(frozen=True, slots=True)
class PinType:
    category: str
    sub_category: str = ""
    sub_category_object: str = ""
    container: str = "none"
    is_ref: bool = False
    is_const: bool = False
    # The value type of a map (category / sub-category / object only); ``None`` for other containers.
    value_type: "PinType | None" = None

    def __post_init__(self) -> None:
        require_text(self.category, "/Type/Category", "Graph.InvalidPinType")
        for value in (self.sub_category, self.sub_category_object):
            require_text(value, "/Type", "Graph.InvalidPinType", allow_empty=True)
        if self.container not in PIN_CONTAINERS:
            raise contract_error("/Type/Container", "Graph.InvalidPinType", "Container is none, array, set or map.")
        if type(self.is_ref) is not bool or type(self.is_const) is not bool:
            raise contract_error("/Type", "Graph.InvalidPinType", "IsRef and IsConst are booleans.")
        if self.value_type is not None and (
                self.container != "map" or type(self.value_type) is not PinType or self.value_type.container != "none"
                or self.value_type.is_ref or self.value_type.is_const or self.value_type.value_type is not None):
            raise contract_error("/Type/ValueType", "Graph.InvalidPinType",
                                 "Only a map type has a ValueType, and it is a plain type (category, sub-category, object).")

    def to_dto(self) -> dict:
        output = {"category": self.category, "sub_category": self.sub_category,
                  "sub_category_object": self.sub_category_object, "container": self.container,
                  "is_ref": self.is_ref, "is_const": self.is_const}
        if self.value_type is not None:
            output["value_type"] = {key: getattr(self.value_type, key)
                                    for key in ("category", "sub_category", "sub_category_object")}
        return output


@dataclass(frozen=True, slots=True)
class PinOverride:
    """An authored pin default; exactly one of value/object/text is set."""
    pin: str
    direction: str = "input"
    value: str | None = None
    object: str | None = None
    text: str | None = None
    # Other spellings of the same read-back value (an enum pin's stored enumerator name next to the
    # display name extract reports); compared, never written.
    alternatives: tuple[str, ...] = field(default=(), compare=False)

    def __post_init__(self) -> None:
        require_text(self.pin, "/Pin", "Graph.InvalidPinOverride")
        if self.direction not in PIN_DIRECTIONS:
            raise contract_error("/Direction", "Graph.InvalidPinOverride", "Direction is input or output.")
        present = [item for item in (self.value, self.object, self.text) if item is not None]
        if len(present) != 1 or type(present[0]) is not str:
            raise contract_error("/", "Graph.InvalidPinOverride",
                                 "A pin override sets exactly one string DefaultValue, DefaultObject or DefaultTextValue.")

    @property
    def kind(self) -> str:
        return "value" if self.value is not None else "object" if self.object is not None else "text"

    @property
    def payload(self) -> str:
        return self.value if self.value is not None else self.object if self.object is not None else self.text


@dataclass(frozen=True, slots=True)
class UserPin:
    name: str
    type: PinType
    default: str | None = None

    def __post_init__(self) -> None:
        require_text(self.name, "/Name", "Graph.InvalidSignature")
        if type(self.type) is not PinType:
            raise contract_error("/Type", "Graph.InvalidSignature", "User pins need a PinType.")
        if self.default is not None and type(self.default) is not str:
            raise contract_error("/Default", "Graph.InvalidSignature", "Defaults are strings.")


@dataclass(frozen=True, slots=True)
class LocalVariable:
    """A variable scoped to one graph (K2: a function's local variable, My Blueprint > Local Variables).
    ``default`` is the editor's default value string; empty strings mean the editor defaults.
    ``value_type`` is the value type of a map (``type`` is then its key type)."""
    name: str
    type: PinType
    default: str = ""
    category: str = ""
    tooltip: str = ""
    value_type: PinType | None = None

    def __post_init__(self) -> None:
        require_text(self.name, "/Name", "Graph.InvalidLocalVariable")
        if type(self.type) is not PinType:
            raise contract_error("/Type", "Graph.InvalidLocalVariable", "Local variables need a PinType.")
        if (self.type.container == "map") != (self.value_type is not None) \
                or (self.value_type is not None and (type(self.value_type) is not PinType
                                                     or self.value_type.container != "none")):
            raise contract_error("/Type/ValueType", "Graph.InvalidLocalVariable",
                                 "A map local variable states its ValueType (a plain type); other types have none.")
        for label, value in (("Default", self.default), ("Category", self.category), ("Tooltip", self.tooltip)):
            require_text(value, f"/{label}", "Graph.InvalidLocalVariable", allow_empty=True)

    def to_dto(self) -> dict:
        pin_type = self.type.to_dto()
        if self.value_type is not None:
            pin_type["value_type"] = {key: getattr(self.value_type, key)
                                      for key in ("category", "sub_category", "sub_category_object")}
        return {"name": self.name, "type": pin_type, "default": self.default, "category": self.category,
                "tooltip": self.tooltip}


@dataclass(frozen=True, slots=True)
class Signature:
    """User-defined pins of the implicit entry (inputs) and result (outputs) nodes.

    ``flags`` holds the function flags the document states, keyed like
    ``graph.function.flags.set.v1`` (``pure``, ``const``, ``call_in_editor``,
    ``access``, ``category``, ``tooltip``, ``keywords``); omitted flags mean the
    schema default.
    """
    inputs: tuple[UserPin, ...] = ()
    outputs: tuple[UserPin, ...] = ()
    flags: Mapping[str, object] = field(default_factory=lambda: _EMPTY)

    def __post_init__(self) -> None:
        for name, pins in (("Inputs", self.inputs), ("Outputs", self.outputs)):
            if type(pins) is not tuple or any(type(item) is not UserPin for item in pins):
                raise contract_error(f"/Signature/{name}", "Graph.InvalidSignature", "Signature pins are UserPin tuples.")
            names = [item.name.casefold() for item in pins]
            if len(set(names)) != len(names):
                raise contract_error(f"/Signature/{name}", "Graph.DuplicatePin", "Signature pin names must be unique.")
        object.__setattr__(self, "flags", freeze_json(dict(self.flags), "/Signature"))


@dataclass(frozen=True, slots=True)
class NodeProperties:
    node: Mapping[str, object] = field(default_factory=lambda: _EMPTY)
    template: Mapping[str, object] = field(default_factory=lambda: _EMPTY)

    def __post_init__(self) -> None:
        object.__setattr__(self, "node", freeze_json(dict(self.node), "/Properties"))
        object.__setattr__(self, "template", freeze_json(dict(self.template), "/TemplateProperties"))

    def items(self):
        """Yield (container, property_path, value) in deterministic order."""
        for container, values in (("node", self.node), ("template", self.template)):
            for key in sorted(values):
                yield container, key, values[key]


@dataclass(frozen=True, slots=True)
class NodeDoc:
    id: str
    action: str | None = None
    implicit: str | None = None
    guid: str | None = None
    parent: str | None = None
    position: Position | None = None
    pin_overrides: tuple[PinOverride, ...] = ()
    properties: NodeProperties = field(default_factory=NodeProperties)
    name: str | None = None
    comment: str | None = None
    extras: Mapping[str, object] = field(default_factory=lambda: _EMPTY)
    # User-defined output pins of a node whose signature the author edits (K2 custom event
    # "Inputs"); None = not stated (live pins are kept and not compared).
    user_pins: tuple[UserPin, ...] | None = None
    # Types picked on a pin itself (K2: a Select node's Index type), as (pin name, PinType) pairs;
    # empty = not stated (the live types are kept).
    pin_types: tuple[tuple[str, PinType], ...] = ()
    # A comment box's size; None = not stated (kept).
    size: Size | None = None
    # The node comment bubble pinned open; None = not stated (kept).
    comment_bubble_visible: bool | None = None
    # The Action as the document wrote it (``K2:Min``; ``action`` is its normalized key), recorded with the node so
    # extract writes it back. Not compared.
    authored_action: str | None = field(default=None, compare=False)

    def __post_init__(self) -> None:
        require_id(self.id, "/Id", "Graph.InvalidNodeId")
        if self.action is not None:
            require_text(self.action, "/Action", "Graph.InvalidAction")
        if self.implicit is not None and (type(self.implicit) is not str or not _TOKEN.fullmatch(self.implicit)):
            raise contract_error("/Implicit", "Graph.InvalidImplicitRole",
                                 "Implicit roles are lower-case tokens such as entry or result.")
        if self.action is None and self.implicit is None and self.guid is None:
            raise contract_error("/Action", "Graph.NodeActionMissing",
                                 "A node needs an Action (for example \"K2:IfThenElse\"), "
                                 "an Implicit role or an existing NodeGuid.")
        object.__setattr__(self, "guid", normalize_guid(self.guid, "/NodeGuid", "Graph.InvalidNodeGuid"))
        if self.parent is not None:
            require_id(self.parent, "/Parent", "Graph.InvalidNodeId")
        if self.position is not None and type(self.position) is not Position:
            raise contract_error("/Position", "Graph.InvalidPosition", "Position must be a Position value.")
        if self.size is not None and type(self.size) is not Size:
            raise contract_error("/Size", "Graph.InvalidSize", "Size must be a Size value.")
        if self.comment_bubble_visible is not None and type(self.comment_bubble_visible) is not bool:
            raise contract_error("/CommentBubbleVisible", "Graph.InvalidType", "CommentBubbleVisible is true or false.")
        if type(self.pin_overrides) is not tuple or any(type(item) is not PinOverride for item in self.pin_overrides):
            raise contract_error("/PinOverrides", "Graph.InvalidPinOverride", "Pin overrides are a PinOverride tuple.")
        keys = [(item.pin.casefold(), item.direction) for item in self.pin_overrides]
        if len(set(keys)) != len(keys):
            raise contract_error("/PinOverrides", "Graph.DuplicatePinOverride", "Each pin may be overridden once.")
        if type(self.properties) is not NodeProperties:
            raise contract_error("/Properties", "Graph.InvalidProperties", "Properties must be NodeProperties.")
        for name, value in (("Name", self.name), ("Comment", self.comment)):
            if value is not None and type(value) is not str:
                raise contract_error(f"/{name}", "Graph.InvalidType", f"{name} must be a string.")
        object.__setattr__(self, "extras", freeze_json(dict(self.extras), "/Extras"))
        if self.user_pins is not None:
            if type(self.user_pins) is not tuple or any(type(item) is not UserPin for item in self.user_pins):
                raise contract_error("/Inputs", "Graph.InvalidSignature", "Node user pins are a UserPin tuple.")
            names = [item.name.casefold() for item in self.user_pins]
            if len(set(names)) != len(names):
                raise contract_error("/Inputs", "Graph.DuplicatePin", "Pin names must be unique.")
        if type(self.pin_types) is not tuple or any(
                type(item) is not tuple or len(item) != 2 or type(item[0]) is not str or not item[0]
                or type(item[1]) is not PinType for item in self.pin_types):
            raise contract_error("/PinTypes", "Graph.InvalidPinType", "PinTypes are (pin name, PinType) pairs.")
        names = [item[0].casefold() for item in self.pin_types]
        if len(set(names)) != len(names):
            raise contract_error("/PinTypes", "Graph.DuplicatePin", "Each pin's type is stated once.")

    def pin_type(self, pin: str) -> PinType | None:
        return next((item[1] for item in self.pin_types if item[0].casefold() == pin.casefold()), None)


@dataclass(frozen=True, slots=True)
class Endpoint:
    node: str
    pin: str

    def __post_init__(self) -> None:
        require_id(self.node, "/Node", "Graph.InvalidLinkNodeId")
        require_text(self.pin, "/Pin", "Graph.InvalidLinkPin")

    def key(self) -> tuple[str, str]:
        return self.node, self.pin.casefold()

    def __str__(self) -> str:
        return f"{self.node}:{self.pin}"


@dataclass(frozen=True, slots=True)
class LinkDoc:
    """A directed link; ``from_`` is the output pin, ``to`` the input pin."""
    from_: Endpoint
    to: Endpoint

    def __post_init__(self) -> None:
        if type(self.from_) is not Endpoint or type(self.to) is not Endpoint:
            raise contract_error("/Links", "Graph.InvalidLink", "Links need From and To endpoints.")

    def key(self) -> tuple:
        return self.from_.key() + self.to.key()


@dataclass(frozen=True, slots=True)
class GraphDoc:
    """Desired state of one graph.

    ``id`` is the stable document key (unique across the asset), ``name`` the
    UE graph name, ``path`` the Body JSON pointer used for diagnostics and
    ``owner`` the id of the node in the parent graph that owns this subgraph.
    """
    id: str
    role: str
    name: str
    schema_tag: str
    guid: str | None = None
    nodes: tuple[NodeDoc, ...] = ()
    links: tuple[LinkDoc, ...] = ()
    subgraphs: tuple["GraphDoc", ...] = ()
    signature: Signature | None = None
    owner: str | None = None
    path: str = ""
    # Graph-scoped variables (K2 function locals); None = not stated (live ones are kept).
    local_variables: tuple[LocalVariable, ...] | None = None
    # "Auto": lay out every node without a Position (a relayout); None = only nodes placed by the apply.
    layout: str | None = None

    def __post_init__(self) -> None:
        base = self.path
        require_text(self.id, base or "/", "Graph.InvalidGraphId")
        if self.role not in GRAPH_ROLES:
            raise contract_error(f"{base}/Role", "Graph.InvalidRole", "Unknown graph role.")
        require_text(self.name, f"{base}/Name", "Graph.InvalidGraphName")
        if type(self.schema_tag) is not str or not _TAG.fullmatch(self.schema_tag):
            raise contract_error(f"{base}/Schema", "Graph.InvalidSchemaTag", "Schema tags are alphanumeric such as K2.")
        object.__setattr__(self, "guid", normalize_guid(self.guid, f"{base}/GraphGuid", "Graph.InvalidGraphGuid"))
        if self.signature is not None and type(self.signature) is not Signature:
            raise contract_error(f"{base}/Signature", "Graph.InvalidSignature", "Signature must be a Signature value.")
        if self.owner is not None:
            require_id(self.owner, f"{base}/Owner", "Graph.InvalidNodeId")
        if self.layout is not None and self.layout not in LAYOUT_MODES:
            raise contract_error(f"{base}/Layout", "Graph.InvalidLayout",
                                 "Layout is \"Auto\" (lay out every node without a Position) or omitted.")
        if self.local_variables is not None:
            if type(self.local_variables) is not tuple or any(type(item) is not LocalVariable
                                                               for item in self.local_variables):
                raise contract_error(f"{base}/LocalVariables", "Graph.InvalidLocalVariable",
                                     "Local variables are LocalVariable values.")
            names = [item.name.casefold() for item in self.local_variables]
            if len(set(names)) != len(names):
                raise contract_error(f"{base}/LocalVariables", "Graph.DuplicateLocalVariable",
                                     "Local variable names must be unique.")
        self._validate_nodes(base)
        self._validate_links(base)
        self._validate_subgraphs(base)

    def _validate_nodes(self, base: str) -> None:
        if type(self.nodes) is not tuple or any(type(item) is not NodeDoc for item in self.nodes):
            raise contract_error(f"{base}/Nodes", "Graph.InvalidGraph", "Nodes are NodeDoc values.")
        seen_ids, seen_guids, roles = set(), set(), set()
        for index, node in enumerate(self.nodes):
            path = f"{base}/Nodes/{index}"
            if node.id in seen_ids:
                raise contract_error(f"{path}/Id", "Graph.DuplicateNodeId", f"Node id '{node.id}' is used twice.")
            if node.guid is not None and node.guid in seen_guids:
                raise contract_error(f"{path}/NodeGuid", "Graph.DuplicateNodeGuid", "NodeGuid values must be unique.")
            if node.implicit is not None and node.implicit in roles \
                    and (self.role, node.implicit) not in REPEATABLE_IMPLICIT_ROLES:
                raise contract_error(f"{path}/Implicit", "Graph.DuplicateImplicitNode",
                                     f"Only one '{node.implicit}' node may exist in a graph.")
            seen_ids.add(node.id)
            seen_guids.add(node.guid)
            roles.add(node.implicit)
        by_id = {node.id: node for node in self.nodes}
        for index, node in enumerate(self.nodes):
            cursor, hops = node.parent, 0
            if cursor is not None and cursor not in by_id:
                raise contract_error(f"{base}/Nodes/{index}/Parent", "Graph.UnknownNode",
                                     f"Parent node '{cursor}' is not declared in this graph.")
            while cursor is not None:
                hops += 1
                if cursor == node.id or hops > len(by_id):
                    raise contract_error(f"{base}/Nodes/{index}/Parent", "Graph.ParentCycle", "Node parents form a cycle.")
                cursor = by_id[cursor].parent

    def _validate_links(self, base: str) -> None:
        if type(self.links) is not tuple or any(type(item) is not LinkDoc for item in self.links):
            raise contract_error(f"{base}/Links", "Graph.InvalidGraph", "Links are LinkDoc values.")
        node_ids = {node.id for node in self.nodes}
        seen = set()
        for index, link in enumerate(self.links):
            for side, endpoint in (("From", link.from_), ("To", link.to)):
                if endpoint.node not in node_ids:
                    raise contract_error(f"{base}/Links/{index}/{side}", "Graph.UnknownNode",
                                         f"Link endpoint node '{endpoint.node}' is not declared in Nodes.")
            if link.key() in seen:
                raise contract_error(f"{base}/Links/{index}", "Graph.DuplicateLink", "The same link is declared twice.")
            seen.add(link.key())

    def _validate_subgraphs(self, base: str) -> None:
        if type(self.subgraphs) is not tuple or any(type(item) is not GraphDoc for item in self.subgraphs):
            raise contract_error(f"{base}/Subgraphs", "Graph.InvalidGraph", "Subgraphs are GraphDoc values.")
        names = [item.name.casefold() for item in self.subgraphs]
        if len(set(names)) != len(names):
            raise contract_error(f"{base}/Subgraphs", "Graph.DuplicateGraphName", "Subgraph names must be unique.")
        node_ids = {node.id for node in self.nodes}
        for index, child in enumerate(self.subgraphs):
            if child.owner is not None and child.owner not in node_ids:
                raise contract_error(f"{base}/Subgraphs/{index}/Owner", "Graph.UnknownNode",
                                     f"Owner node '{child.owner}' is not declared in this graph.")

    # Lookup helpers ------------------------------------------------------
    def node_index(self, node_id: str) -> int:
        for index, node in enumerate(self.nodes):
            if node.id == node_id:
                return index
        raise KeyError(node_id)

    def node_path(self, node_id: str) -> str:
        return f"{self.path}/Nodes/{self.node_index(node_id)}"

    def node(self, node_id: str) -> NodeDoc:
        return self.nodes[self.node_index(node_id)]

    def walk(self):
        """Yield this graph and all subgraphs depth-first."""
        yield self
        for child in self.subgraphs:
            yield from child.walk()
