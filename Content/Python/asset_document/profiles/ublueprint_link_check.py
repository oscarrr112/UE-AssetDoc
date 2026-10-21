"""Validate's graph check of the UBlueprint Profile: action keys, pin names and links, read without editing.

A Blueprint edit is not tried on a copy (the Blueprint action database only knows actions of real
assets), so Validate asks ``graph.link.check.v1`` about the document's links instead: their pins are
those of the palette's template node of each end's action key, read on the existing target. Only ends
whose pin types the document cannot change are judged: nodes of the Blueprint's own members (their
types may change in the same document), nodes whose pins the document shapes (user pins, pin types,
class picks, properties) and implicit nodes are left to Apply, which refuses the same link with the
same message. Pin defaults (PinOverrides) get the check Apply makes before it sets them, on the template's
pin.
"""
from __future__ import annotations

from asset_document.diagnostics import AssetDocumentContractError, RuntimeDiagnostic, contract_errors
from asset_document.graphs.executor import LINK_CHECK, conversion_message, pin_default_message
from asset_document.graphs.model import GraphDoc, NodeDoc
from asset_document.graphs.pin_names import resolve_pin_name
from asset_document.graphs.schemas.k2 import K2_ADAPTER

# Links per graph.link.check.v1 call (each call reads the action database once).
CHUNK = 512


def _shaped(node: NodeDoc) -> bool:
    """A node whose pin types may differ from its template's once the document's settings are applied."""
    return (node.action is None or node.implicit is not None or node.name is not None or node.user_pins is not None
            or bool(node.pin_types) or bool(node.extras) or any(True for _ in node.properties.items())
            or any(item.kind != "value" or item.payload.startswith("/") for item in node.pin_overrides))


def _fixed_pins(node: NodeDoc, key: str) -> bool:
    """A node whose pins keep their template's types whatever its pin defaults are (a class picked on a class
    pin adds or retypes other pins, not the ones it leaves)."""
    return (node.implicit is None and node.name is None and node.user_pins is None and not node.pin_types
            and not node.extras and not any(True for _ in node.properties.items())
            and not any(K2_ADAPTER.override_shapes_pins(key, item.pin) for item in node.pin_overrides))


def _key(node: NodeDoc, path: str, *, self_class: str | None, target: str, skip=None) -> str | None:
    if _shaped(node):
        return None
    try:
        key = K2_ADAPTER.normalize_action(node.action, self_class=self_class, path=path)
    except AssetDocumentContractError:
        return None  # Apply reports the key
    owner = f"{target}."
    return None if "#" in key or owner.casefold() in key.casefold() or (skip is not None and skip(key)) else key


def checked_links(graphs: tuple[GraphDoc, ...], *, self_class: str | None, target: str, skip=None) -> list[tuple]:
    """(Body path of the link, output end text, input end text, check request) for each judgeable link."""
    out = []
    for root in graphs:
        for graph in root.walk():
            keys = {node.id: _key(node, graph.node_path(node.id), self_class=self_class, target=target, skip=skip)
                    for node in graph.nodes}
            for index, link in enumerate(graph.links):
                ends = (keys.get(link.from_.node), keys.get(link.to.node))
                if None in ends:
                    continue
                request = {"from": {"action_key": ends[0], "pin_name": link.from_.pin},
                           "to": {"action_key": ends[1], "pin_name": link.to.pin}}
                out.append((f"{graph.path}/Links/{index}", str(link.from_), str(link.to), request))
    return out


def _template_pin(pins: list, name: str, direction: str | None, action: str):
    """The template pin a document's pin name names, by the rules Apply uses (graphs.live.find_pin)."""
    candidates = [pin for pin in pins if direction is None or pin.direction == direction]
    exact = [pin for pin in candidates if pin.name.casefold() == name.casefold()]
    if exact:
        return exact[0]
    found = resolve_pin_name(name, candidates, action=action)
    if found is not None:
        return found
    # A member of a struct pin the connect splits first ("ReturnValue_X"), or of a wildcard pin.
    return next((pin for pin in candidates if pin.category in ("struct", "wildcard")
                 and name.casefold().startswith(pin.name.casefold() + "_")), None)


def _pin_list(pins: list, direction: str | None) -> str:
    names = [pin.name + (f" ({pin.display_name})" if pin.display_name else "") for pin in pins
             if not pin.hidden and (direction is None or pin.direction == direction)]
    return ", ".join(names) or "none"


def _node_key(node: NodeDoc, path: str, *, self_class: str | None, target: str, skip=None) -> str | None:
    """The key whose resolution Validate checks: not the Blueprint's own members (they may be made by the same
    document) nor implicit nodes, nor keys the Profile leaves to Apply (``skip``)."""
    if node.action is None or node.implicit is not None:
        return None
    try:
        key = K2_ADAPTER.normalize_action(node.action, self_class=self_class, path=path)
    except AssetDocumentContractError:
        return None  # decode reports a malformed key
    return None if f"{target}.".casefold() in key.casefold() or (skip is not None and skip(key)) else key


def check_graphs(invoke, graphs: tuple[GraphDoc, ...], *, self_class: str | None, target: str, skip=None) -> list:
    """Every independent problem Apply would refuse in the document's graphs, read without editing: action keys
    that place no node (with suggestions), pin names a node's palette template does not have, links the
    editor refuses or makes only through a conversion node, and pin defaults the pin refuses (a literal on a
    by-ref input). Returns RuntimeDiagnostics (empty: nothing found).

    ``invoke(primitive, values)`` calls ``graph.link.check.v1`` on a Blueprint (the target, or a transient
    Blueprint of the document's parent class for a new one); None skips the check. ``skip(key)``: keys the Profile
    leaves to Apply (a Widget Blueprint's bound events of widgets the same document may add)."""
    nodes = []  # (graph, node, key)
    for root in graphs:
        for graph in root.walk():
            for node in graph.nodes:
                key = _node_key(node, f"{graph.node_path(node.id)}/Action", self_class=self_class, target=target,
                                skip=skip)
                if key is not None:
                    nodes.append((graph, node, key))
    keys = sorted({key for _, _, key in nodes})
    links = checked_links(graphs, self_class=self_class, target=target, skip=skip)
    if not keys and not links:
        return []
    templates, unresolved = {}, {}
    if keys:
        result = invoke(LINK_CHECK, {"describe": keys}) or {}
        for item in result.get("unresolved") or ():
            if type(item) is dict and type(item.get("key")) is str:
                unresolved[item["key"]] = item
        for item in result.get("templates") or ():
            if type(item) is dict and type(item.get("key")) is str and type(item.get("pins")) is list:
                templates[item["key"]] = item
    pins_of = {key: [_TemplatePin(row) for row in item["pins"] if type(row) is dict]
               for key, item in templates.items()}
    out, conversions = [], []
    for start in range(0, len(links), CHUNK):
        chunk = links[start:start + CHUNK]
        requests = [_template_names(item[3], pins_of) for item in chunk]
        result = invoke(LINK_CHECK, {"links": requests}) or {}
        for facts in sorted((item for item in result.get("conversions") or () if type(item) is dict),
                            key=lambda item: item.get("index", 0)):
            path, from_end, to_end, _ = chunk[facts["index"]]
            if facts.get("from_pin", "").casefold() != requests[facts["index"]]["from"]["pin_name"].casefold():
                from_end, to_end = to_end, from_end  # written input -> output
            conversions.append(RuntimeDiagnostic(path, "Graph.ConnectNeedsConversion", conversion_message(
                from_end, to_end, {k: v for k, v in facts.items() if type(v) is str})))
        for facts in sorted((item for item in result.get("refused") or ()
                             if type(item) is dict and type(item.get("index")) is int and 0 <= item["index"] < len(chunk)),
                            key=lambda item: item["index"]):
            path, from_end, to_end, _ = chunk[facts["index"]]
            if str(facts.get("from_pin", "")).casefold() != requests[facts["index"]]["from"]["pin_name"].casefold():
                from_end, to_end = to_end, from_end
            reason = str(facts.get("reason") or "the editor refuses this link").rstrip(".")
            conversions.append(RuntimeDiagnostic(path, "Graph.ConnectDisallowed", (
                f"Cannot connect {from_end} ({facts.get('from_type') or '?'}) to {to_end} ({facts.get('to_type') or '?'}): "
                f"{reason}.")))
    for graph, node, key in nodes:
        if key in unresolved:
            item = unresolved[key]
            suggestions = [text for text in item.get("suggestions") or () if type(text) is str]
            message = str(item.get("message") or f"No K2 action has key {key}.")
            if suggestions:
                message += f" Did you mean: {', '.join(suggestions[:5])}?"
            out.append(RuntimeDiagnostic(f"{graph.node_path(node.id)}/Action",
                                         str(item.get("code") or "Graph.ActionNotFound"), message))
    for graph, node, key in nodes:
        pins = None if templates.get(key, {}).get("can_add_pin") else pins_of.get(key)
        if pins is None or _shaped(node) or any(K2_ADAPTER.override_shapes_pins(key, item.pin) for item in node.pin_overrides):
            continue
        for index, item in enumerate(node.pin_overrides):
            if _template_pin(pins, item.pin, item.direction, key) is None:
                out.append(RuntimeDiagnostic(f"{graph.node_path(node.id)}/PinOverrides/{index}", "Graph.PinNotFound",
                                             f"Node '{node.id}' ({key}) has no {item.direction} pin '{item.pin}'. "
                                             f"Pins: {_pin_list(pins, item.direction)}."))
        for index, link in enumerate(graph.links):
            for end, direction in ((link.from_, "output"), (link.to, "input")):
                if end.node != node.id or _template_pin(pins, end.pin, None, key) is not None:
                    continue
                out.append(RuntimeDiagnostic(f"{graph.path}/Links/{index}", "Graph.PinNotFound",
                                             f"Node '{node.id}' ({key}) has no pin '{end.pin}'. "
                                             f"Pins: {_pin_list(pins, direction)}."))
    return out + conversions + _check_defaults(invoke, nodes, pins_of)


def _check_defaults(invoke, nodes: list, pins_of: dict) -> list:
    """Graph.PinDefaultInvalid for each pin override the template's pin refuses, as Apply words it."""
    requests, at = [], []
    for graph, node, key in nodes:
        pins = pins_of.get(key)
        if not pins or not _fixed_pins(node, key):
            continue
        for index, item in enumerate(node.pin_overrides):
            candidates = [pin for pin in pins if pin.direction == item.direction]
            pin = next((pin for pin in candidates if pin.name.casefold() == item.pin.casefold()), None) \
                or resolve_pin_name(item.pin, candidates, action=key)
            if pin is None:
                continue  # Graph.PinNotFound above, or a member of a struct pin Apply splits
            requests.append({"action_key": key, "pin_name": pin.name, "kind": item.kind, "value": item.payload})
            at.append((f"{graph.node_path(node.id)}/PinOverrides/{index}", node.id, item))
    out = []
    for start in range(0, len(requests), CHUNK):
        result = invoke(LINK_CHECK, {"defaults": requests[start:start + CHUNK]}) or {}
        for facts in sorted((item for item in result.get("invalid_defaults") or ()
                             if type(item) is dict and type(item.get("index")) is int),
                            key=lambda item: item["index"]):
            if not 0 <= facts["index"] < len(requests[start:start + CHUNK]):
                continue
            path, node_id, item = at[start + facts["index"]]
            message = pin_default_message(node_id, item.pin, item.kind, item.payload, str(facts.get("message") or ""),
                                          {k: v for k, v in facts.items() if type(v) is str})
            out.append(RuntimeDiagnostic(path, str(facts.get("code") or "Graph.PinDefaultInvalid"), message))
    return out


def _template_names(request: dict, pins_of: dict) -> dict:
    """The link request with each end's pin named as its template names it (``Result`` -> ``AsCharacter``)."""
    out = {}
    for side in ("from", "to"):
        end = request[side]
        pin = _template_pin(pins_of.get(end["action_key"], []), end["pin_name"], None, end["action_key"])
        out[side] = dict(end, pin_name=pin.name) if pin is not None else end
    return out


class _TemplatePin:
    __slots__ = ("name", "direction", "category", "display_name", "hidden")

    def __init__(self, row: dict) -> None:
        self.name = str(row.get("name", ""))
        self.direction = str(row.get("direction", ""))
        self.category = str(row.get("category", ""))
        self.display_name = str(row.get("display_name", ""))
        self.hidden = bool(row.get("hidden", False))


def check_links(invoke, graphs: tuple[GraphDoc, ...], *, self_class: str | None, target: str, skip=None) -> None:
    """Raise every problem ``check_graphs`` finds (the first leads)."""
    found = check_graphs(invoke, graphs, self_class=self_class, target=target, skip=skip)
    if found:
        raise contract_errors(found)
