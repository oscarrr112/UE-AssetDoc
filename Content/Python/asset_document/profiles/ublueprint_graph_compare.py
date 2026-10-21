"""Structural comparison of authored and extracted K2 graph sections (pure).

Authored node ids are free names; extraction derives its own ids, so graphs are
compared up to node renaming: node labels (action, custom event name, implicit
role) are refined over the link structure (Weisfeiler-Lehman colour refinement)
and the colour multisets, links, pin overrides and signatures must agree.
Positions, comment box sizes, NodeGuids, GraphGuids, comments, node names, Extras and node property
bags are compared only in strict mode (the Diff operation), and only where the
document states them: Diff then reports any authored value the live graph does
not hold, so an empty Diff means applying the document would change nothing.
Post-apply verification stays lenient about these editor-normalised fields.
An extracted graph the document does not list is tolerated only while it holds
nothing but what the editor creates on its own: implicit nodes (EventGraph /
construction script / interface stubs) and a child Blueprint's "Parent: <graph>"
call.
"""
from __future__ import annotations

import json
import math

from asset_document.graphs.actions import action_kind, preferred_alias, split_disambiguator
from asset_document.graphs.model import GraphDoc, NodeDoc, thaw_json
from asset_document.graphs.pin_names import node_pin_key
from asset_document.graphs.reconcile import type_zero
from asset_document.graphs.schemas.k2 import (
    NAMED_KINDS, SIGNATURE_FLAG_DEFAULTS, decode_section, encode_pin_type, node_identity_action,
)

_ROUNDS = 8


def _norm_payload(kind: str, payload: str) -> str:
    text = payload.strip()
    if kind == "object":
        if "." not in text.rsplit("/", 1)[-1] and text.startswith("/"):
            text = f"{text}.{text.rsplit('/', 1)[-1]}"
        return text
    if text.lower() in ("true", "false"):
        return text.lower()
    try:
        number = float(text)
    except ValueError:
        return text
    return repr(round(number, 5)) if math.isfinite(number) else text


def _overrides(node: NodeDoc, strict: bool = False, extracted: bool = False) -> frozenset:
    facts = {(node_pin_key(item.pin, action=node.action), item.direction, item.kind,
              _norm_payload(item.kind, payload)) for item in node.pin_overrides
             for payload in (item.payload, *item.alternatives)}
    user_pins = node.user_pins
    if user_pins is None and extracted and node.action and action_kind(split_disambiguator(node.action)[0]) == "CustomEvent":
        user_pins = ()  # extract leaves out an empty Inputs list: a read-back custom event without it has none
    if user_pins is not None:
        # A custom event's Inputs: compared where the document states them ("Inputs": [] states none).
        facts.add(("@Inputs", tuple((pin.name.casefold(), pin.type) for pin in user_pins)))
    # A Select's Index type (PinTypes): compared where the document states it.
    facts |= {("@PinType", pin.casefold(), item) for pin, item in node.pin_types}
    return frozenset(facts | _metadata(node)) if strict else frozenset(facts)


def _round_numbers(value):
    """Float properties read back as float32 (0.4 -> 0.4000000059604645); compare like pin defaults."""
    if type(value) is float and math.isfinite(value):
        rounded = round(value, 5)
        return int(rounded) if rounded.is_integer() else rounded
    if type(value) is dict:
        return {key: _round_numbers(item) for key, item in value.items()}
    if type(value) is list:
        return [_round_numbers(item) for item in value]
    return value


def _json(value) -> str:
    return json.dumps(_round_numbers(thaw_json(value)), sort_keys=True, separators=(",", ":"))


def _metadata(node: NodeDoc) -> set:
    """Strict-mode facts of a node: its stated metadata, layout and property values."""
    facts = set()
    if node.position is not None:
        facts.add(("@Position", int(round(node.position.x)), int(round(node.position.y))))
    for key, value in (("@NodeGuid", node.guid), ("@Comment", node.comment), ("@Name", node.name)):
        if value is not None:
            facts.add((key, value))
    if node.comment_bubble_visible:  # extract states a shown bubble only
        facts.add(("@CommentBubbleVisible",))
    if node.size is not None:
        facts.add(("@Size", node.size.width, node.size.height))
    for container, key, value in node.properties.items():
        facts.add(("@Property", container, key, _json(value)))
    for key, value in node.extras.items():
        facts.add(("@Extra", key, _json(value)))
    return facts


def _implicit_metadata_match(authored: GraphDoc, extracted: GraphDoc) -> bool:
    """Strict mode: implicit nodes outside the compared shape still carry their stated metadata."""
    have: dict[str, list[NodeDoc]] = {}
    for node in extracted.nodes:  # a function may hold several return nodes
        if node.implicit is not None:
            have.setdefault(node.implicit, []).append(node)
    for node in authored.nodes:
        if node.implicit is None or not _metadata(node):
            continue
        if not any(_metadata(node) <= _metadata(other) for other in have.get(node.implicit, ())):
            return False
    return True


def _label(node: NodeDoc) -> tuple:
    action = node_identity_action(split_disambiguator(node.action)[0]) if node.action else ""
    name = node.name if node.name and action_kind(action) in NAMED_KINDS else ""
    return (node.implicit or "", action, name.casefold() if name else "")


def _shape(graph: GraphDoc):
    actions = {node.id: node.action for node in graph.nodes}
    # Pins compare by their loose spelling (pin_names): a user struct member's name or GUID name, a
    # cast's Result or As<Class>, Get Data Table Row's RowFound or then.
    links = [(link.from_.node, node_pin_key(link.from_.pin, action=actions.get(link.from_.node)),
              link.to.node, node_pin_key(link.to.pin, action=actions.get(link.to.node))) for link in graph.links]
    linked = {item[0] for item in links} | {item[2] for item in links}
    nodes = {node.id: node for node in graph.nodes
             if node.implicit is None or node.id in linked or node.pin_overrides}
    links = [item for item in links if item[0] in nodes and item[2] in nodes]
    return nodes, links


def _refine(sides, intern, label=_label):
    colours = [{node_id: intern(("L",) + label(node)) for node_id, node in nodes.items()} for nodes, _ in sides]
    for _ in range(_ROUNDS):
        updated = []
        for (nodes, links), colour in zip(sides, colours):
            neighbours = {node_id: [] for node_id in nodes}
            for source, out_pin, target, in_pin in links:
                neighbours[source].append(("o", out_pin, in_pin, colour[target]))
                neighbours[target].append(("i", in_pin, out_pin, colour[source]))
            updated.append({node_id: intern((colour[node_id], tuple(sorted(neighbours[node_id]))))
                            for node_id in nodes})
        if all(len(set(a.values())) == len(set(b.values())) for a, b in zip(colours, updated)):
            colours = updated
            break
        colours = updated
    return colours


def _overrides_match(authored: list, extracted: list) -> bool:
    """Every authored node finds a distinct extracted node of its colour with the same overrides."""
    remaining = list(extracted)
    for wanted in sorted(authored, key=len, reverse=True):
        found = next((index for index, have in enumerate(remaining) if have == wanted), None)
        if found is None:
            found = next((index for index, have in enumerate(remaining) if wanted <= have), None)
        if found is None:
            return False
        remaining.pop(found)
    return True


def graphs_match(authored: GraphDoc, extracted: GraphDoc, strict: bool = False, label=_label) -> bool:
    """``label`` gives a node's identity label (schemas whose node names are identity pass their own)."""
    if strict and authored.guid is not None and authored.guid != extracted.guid:
        return False
    if strict and not _implicit_metadata_match(authored, extracted):
        return False
    sides = (_shape(authored), _shape(extracted))
    table: dict = {}
    colours = _refine(sides, lambda key: table.setdefault(key, len(table)), label)
    if sorted(colours[0].values()) != sorted(colours[1].values()):
        return False
    link_sets = []
    for (nodes, links), colour in zip(sides, colours):
        link_sets.append(sorted((colour[s], op, colour[t], ip) for s, op, t, ip in links))
    if link_sets[0] != link_sets[1]:
        return False
    for colour in set(colours[0].values()):
        wanted = [_overrides(sides[0][0][node_id], strict) for node_id, value in colours[0].items() if value == colour]
        have = [_overrides(sides[1][0][node_id], strict, True) for node_id, value in colours[1].items() if value == colour]
        if not _overrides_match(wanted, have):
            return False
    if not _signatures_match(authored, extracted):
        return False
    return _subgraphs_match(authored.subgraphs, extracted.subgraphs, strict, label)


def _signature_key(graph: GraphDoc):
    signature = graph.signature
    if graph.role != "function":
        return None
    inputs = tuple((pin.name.casefold(), pin.type) for pin in signature.inputs) if signature else ()
    outputs = tuple((pin.name.casefold(), pin.type) for pin in signature.outputs) if signature else ()
    flags = dict(SIGNATURE_FLAG_DEFAULTS)
    flags.update(dict(signature.flags) if signature else {})
    return inputs, outputs, tuple(sorted(flags.items()))


def _default_differences(authored: GraphDoc, extracted: GraphDoc) -> list[tuple[str, str, str]]:
    """(pin, document default, read-back default) of the signature pins whose stated Default is not the
    read-back one. A read-back pin without a Default holds its type's zero value ("0.0", "false")."""
    if authored.signature is None:
        return []
    have = {pin.name.casefold(): pin for pin in (extracted.signature.inputs + extracted.signature.outputs)} \
        if extracted.signature else {}
    out = []
    for pin in authored.signature.inputs + authored.signature.outputs:
        other = have.get(pin.name.casefold())
        if pin.default is None or other is None:
            continue
        value = other.default if other.default is not None else (type_zero(other.type) or "")
        if _norm_payload("value", pin.default) != _norm_payload("value", value):
            out.append((pin.name, pin.default, value))
    return out


def _signatures_match(authored: GraphDoc, extracted: GraphDoc) -> bool:
    if _signature_key(authored) != _signature_key(extracted):
        return False
    return not _default_differences(authored, extracted)


def _signature_difference(authored: GraphDoc, extracted: GraphDoc) -> str:
    wanted, have = _signature_key(authored), _signature_key(extracted)
    defaults = "; ".join(f"{name} Default {mine!r}, read back {theirs!r}"
                         for name, mine, theirs in _default_differences(authored, extracted))
    if wanted is None or have is None:
        return defaults or "default values"
    parts = []
    for label, index in (("inputs", 0), ("outputs", 1)):
        if wanted[index] != have[index]:
            parts.append(f"{label} {[(name, encode_pin_type(kind)) for name, kind in wanted[index]]}, read back "
                         f"{[(name, encode_pin_type(kind)) for name, kind in have[index]]}")
    flags = [(name, value, dict(have[2]).get(name)) for name, value in wanted[2] if dict(have[2]).get(name) != value]
    if flags:
        parts.append("flags " + ", ".join(f"{name} {value!r} read back {other!r}" for name, value, other in flags))
    if defaults:
        parts.append(defaults)
    return "; ".join(parts) or "default values"


def _is_editor_default(graph: GraphDoc, node: NodeDoc) -> bool:
    """Implicit nodes, and the "Parent: <graph>" call the editor puts into a child Blueprint's graph."""
    if node.implicit is not None:
        return True
    action = split_disambiguator(node.action)[0] if node.action else ""
    return action_kind(action) == "CallParentFunction" and action.rsplit(":", 1)[-1].casefold() == graph.name.casefold()


def _is_stub(graph: GraphDoc) -> bool:
    return all(_is_editor_default(graph, node) for node in graph.nodes) and not graph.subgraphs


def _subgraphs_match(authored: tuple, extracted: tuple, strict: bool = False, label=_label) -> bool:
    have = {graph.name.casefold(): graph for graph in extracted}
    for graph in authored:
        other = have.pop(graph.name.casefold(), None)
        if other is None or not graphs_match(graph, other, strict, label):
            return False
    return all(_is_stub(graph) for graph in have.values())


def resolve_short_members(authored: GraphDoc, extracted: GraphDoc, self_class: str) -> GraphDoc:
    """Read the authored short forms as apply resolved them: ``K2:Get:X`` of a function that leaves its
    LocalVariables out names the live local ``X``; a key naming the asset's own member that the live
    graph only holds as a parent class's member (``K2:Get:Combat`` of an inherited component) is that
    member."""
    if not self_class:
        return authored
    from dataclasses import replace
    from asset_document.graphs.schemas.k2 import declared_call_key, localize_action
    locals_ = frozenset(item.name for item in (extracted.local_variables or ())) \
        if authored.local_variables is None else frozenset()
    have = {split_disambiguator(node.action)[0] for node in extracted.nodes if node.action}
    nodes = []
    for node in authored.nodes:
        action = node.action
        if action:
            action = localize_action(action, locals_, graph=authored.name, self_class=self_class)
            base, sep, suffix = action.partition("#")
            parts = base.split(":")
            if len(parts) == 4 and parts[2].casefold() == self_class.casefold() and base not in have:
                inherited = {key for key in have if key.split(":")[:2] == parts[:2]
                             and key.count(":") == 3 and key.rsplit(":", 1)[-1] == parts[3]}
                if len(inherited) == 1:
                    action = inherited.pop() + sep + suffix
            elif (declared := declared_call_key(base, have)) is not None:
                action = declared + sep + suffix
        nodes.append(node if action == node.action else replace(node, action=action))
    return authored if tuple(nodes) == authored.nodes else replace(authored, nodes=tuple(nodes))


# Verify's defaults key (extract_with_defaults): the stored enumerator names of the live enum pins, {name: display name}
# (None when two enums give one name different display names).
PIN_ENUM_NAMES = "@PinEnumNames"


def pin_enum_names(live) -> dict:
    """{stored enumerator name (casefold): display name} of the live graphs' enum pin defaults (LiveAsset)."""
    out: dict = {}
    for graph in live.graphs:
        for node in graph.nodes:
            for pin in node.pins:
                if pin.default_display and pin.default_value:
                    key = pin.default_value.casefold()
                    out[key] = pin.default_display if out.get(key, pin.default_display) == pin.default_display else None
    return out


def display_enum_values(authored: GraphDoc, names: dict | None) -> GraphDoc:
    """The authored graph with enum pin defaults written as stored enumerator names (``NewEnumerator1``, as documents
    wrote them before display names were accepted) read as the display names extract reports."""
    if not names:
        return authored
    from dataclasses import replace

    def node_of(node):
        overrides = tuple(replace(item, value=names[item.value.casefold()])
                          if item.value is not None and names.get(item.value.casefold()) else item
                          for item in node.pin_overrides)
        return node if overrides == node.pin_overrides else replace(node, pin_overrides=overrides)

    def graph_of(graph):
        nodes = tuple(node_of(node) for node in graph.nodes)
        subgraphs = tuple(graph_of(item) for item in graph.subgraphs)
        return graph if nodes == graph.nodes and subgraphs == graph.subgraphs else replace(graph, nodes=nodes, subgraphs=subgraphs)
    return graph_of(authored)


def section_mismatches(section: str, authored: list, extracted: list, *, self_class: str,
                       strict: bool = False) -> list[str]:
    """Names of graphs of one Body section that do not match (missing, different or unexpected)."""
    return list(section_mismatch_reasons(section, authored, extracted, self_class=self_class, strict=strict))


def section_mismatch_reasons(section: str, authored: list, extracted: list, *, self_class: str,
                             strict: bool = False, enum_names: dict | None = None) -> dict[str, str]:
    """Graphs of one Body section that do not match, each with the first difference found. A graph whose
    GraphGuid names a live graph of another name is that graph renamed ("renamed from <old name>"), not a
    missing graph and an unexpected one (apply renames it, keeping its GUID)."""
    wanted = decode_section(section, authored, self_class=self_class)
    have = {graph.name.casefold(): graph for graph in decode_section(section, extracted, self_class=self_class)}
    names = {graph.name.casefold() for graph in wanted}
    out = {}
    for graph in wanted:
        other = have.pop(graph.name.casefold(), None)
        graph = display_enum_values(graph, enum_names)
        if other is not None:
            graph = resolve_short_members(graph, other, self_class)
        if other is None and graph.guid is not None:
            renamed = next((key for key, live in have.items() if live.guid == graph.guid and key not in names), None)
            if renamed is not None:
                other = have.pop(renamed)
                out[graph.name] = f"renamed from {other.name}" + (
                    "" if graphs_match(graph, other, strict) else f"; {mismatch_reason(graph, other, strict)}")
                continue
        if other is None:
            out[graph.name] = "missing"
        elif other.name != graph.name:
            # A case-only rename of a top-level graph is a change (graph.rename).
            out[graph.name] = f"named {other.name}"
        elif not graphs_match(graph, other, strict):
            out[graph.name] = mismatch_reason(graph, other, strict)
    out.update({graph.name: "unexpected" for graph in have.values() if not _is_stub(graph)})
    return out


def _few(items, limit: int = 3) -> str:
    items = sorted(items)
    return ", ".join(items[:limit]) + (f" (+{len(items) - limit} more)" if len(items) > limit else "")


def _counted_difference(wanted: list, have: list) -> tuple[list, list]:
    remaining = list(have)
    missing = []
    for item in wanted:
        if item in remaining:
            remaining.remove(item)
        else:
            missing.append(item)
    return missing, remaining


def _label_text(label_value: tuple) -> str:
    implicit, action, name = label_value
    action = preferred_alias(action) or action if action else action
    return "/".join(part for part in (implicit, action, name) if part) or "?"


def mismatch_reason(authored: GraphDoc, extracted: GraphDoc, strict: bool = False, label=_label) -> str:
    """Why ``graphs_match`` fails, for diagnostics: the first kind of difference, with a few examples."""
    if strict and authored.guid is not None and authored.guid != extracted.guid:
        return "GraphGuid differs"
    if strict and not _implicit_metadata_match(authored, extracted):
        return "implicit node metadata differs"
    sides = (_shape(authored), _shape(extracted))
    labels = [[_label_text(label(node)) for node in nodes.values()] for nodes, _ in sides]
    missing, unexpected = _counted_difference(*labels)
    if missing or unexpected:
        return f"nodes: missing [{_few(missing)}], unexpected [{_few(unexpected)}]"
    named = [{node_id: _label_text(label(node)) for node_id, node in nodes.items()} for nodes, _ in sides]
    link_text = [[f"{names[s]}.{op} -> {names[t]}.{ip}" for s, op, t, ip in links]
                 for (_, links), names in zip(sides, named)]
    missing, unexpected = _counted_difference(*link_text)
    if missing or unexpected:
        return f"links: missing [{_few(missing)}], unexpected [{_few(unexpected)}]"
    table: dict = {}
    colours = _refine(sides, lambda key: table.setdefault(key, len(table)), label)
    if sorted(colours[0].values()) != sorted(colours[1].values()):
        return "link structure differs (same nodes and label-level links, different wiring)"
    for colour in set(colours[0].values()):
        ids = [[node_id for node_id, value in side.items() if value == colour] for side in colours]
        wanted = [_overrides(sides[0][0][node_id], strict) for node_id in ids[0]]
        have = [_overrides(sides[1][0][node_id], strict, True) for node_id in ids[1]]
        if not _overrides_match(wanted, have):
            union = frozenset().union(*have) if have else frozenset()
            node_id, lost = next(((node_id, facts - union) for node_id, facts in zip(ids[0], wanted) if facts - union),
                                 (ids[0][0], frozenset()))
            found = {str(fact) for facts in have for fact in facts}
            return (f"pin values of {node_id} ({named[0][node_id]}): [{_few(str(fact) for fact in lost)}] not found; "
                    f"read back [{_few(found)}]")
    if not _signatures_match(authored, extracted):
        return "signature differs: " + _signature_difference(authored, extracted)
    have = {graph.name.casefold(): graph for graph in extracted.subgraphs}
    for graph in authored.subgraphs:
        other = have.pop(graph.name.casefold(), None)
        if other is None:
            return f"subgraph {graph.name}: missing"
        if not graphs_match(graph, other, strict, label):
            return f"subgraph {graph.name}: {mismatch_reason(graph, other, strict, label)}"
    extra = [graph.name for graph in have.values() if not _is_stub(graph)]
    return f"unexpected subgraphs [{_few(extra)}]" if extra else "differs"
