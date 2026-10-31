# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Validate's layout warnings: where the nodes a document places would overlap (pure).

A node the document *places* is one apply puts where the document says: a new node with a ``Position``, an
existing node whose ``Position`` differs from where it is, a comment box whose ``Position`` or ``Size`` changes.
Only those are judged; nodes the document leaves where they are, nodes apply lays out itself (no
``Position``: automatic layout overlaps nothing) are not, though they are in the way of placed nodes (nodes apply
lays out only once it has placed them). The editor's placeholder ("ghost") events are nodes like any other: one the
document lists stays and is in the way (moved, it is judged); one a listed graph leaves out is removed by apply, so it
is in nobody's way.

Warnings, never errors (codes are stable):

- ``Graph.LayoutNodeOverlap``: a placed node's box overlaps another node's box.
- ``Graph.LayoutCommentOverlap``: two comment boxes, one of them placed, overlap without one holding the other.
- ``Graph.LayoutNodeOutsideComment``: a node is partly outside the comment box that holds most of it (the
  node or the box placed).

Each names both nodes and the overlap rectangle (also in ``details``: ``nodes``, ``overlap``), at the placed
node's ``Position`` (a resized box: its ``Size``). Boxes are the editor's: a live node's drawn size, a comment
box's own size, a new node's that of its palette template (``measure``; pin values the document sets may make
it larger). A node whose size is not known is not judged. Wires running through nodes are not judged.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Iterable, Mapping

from asset_document.diagnostics import (
    AssetDocumentContractError, DiagnosticContext, DiagnosticSeverity, RuntimeDiagnostic,
)
from asset_document.graphs.layout import Box
from asset_document.graphs.live import LiveAsset, LiveNode
from asset_document.graphs.model import GraphDoc, NodeDoc
from asset_document.graphs.reconcile import GraphPairing, paired_graphs
from asset_document.graphs.schemas import GENERIC_ADAPTER, SchemaAdapter

NODE_OVERLAP = "Graph.LayoutNodeOverlap"
COMMENT_OVERLAP = "Graph.LayoutCommentOverlap"
OUTSIDE_COMMENT = "Graph.LayoutNodeOutsideComment"
LIMITED = "Graph.LayoutWarningsLimited"
# Warnings listed per document; the rest are counted in one LIMITED warning.
MAX_WARNINGS = 20


@dataclass(frozen=True, slots=True)
class _Item:
    label: str                 # how a message names the node ("node 'Print'")
    name: str                  # the node's document Id (a live-only node: its title)
    box: Box
    comment: bool              # a comment box
    placed: bool               # the document places it (judged)
    path: str                  # where a warning about it points


@dataclass(frozen=True, slots=True)
class PlacedGraph:
    """A paired graph with at least one node the document places, and the action keys of its new nodes."""
    pairing: GraphPairing
    new_keys: tuple[str, ...]


def _measured(node: LiveNode | None) -> tuple[float, float] | None:
    """A live node's size as the editor draws it, when measured (a comment box: its own size)."""
    if node is None:
        return None
    if node.box_size is not None:
        return float(node.box_size[0]), float(node.box_size[1])
    if node.widget_size is not None:
        return float(node.widget_size[0]), float(node.widget_size[1])
    return None


def _same_position(node: NodeDoc, found: LiveNode) -> bool:
    return found.position is not None and found.position.to_dto() == node.position.to_dto()


def _places(node: NodeDoc, found: LiveNode | None) -> bool:
    """The document puts the node somewhere other than where it is (new, moved, or a box resized)."""
    if found is None:
        return node.position is not None
    moved = node.position is not None and not _same_position(node, found)
    resized = node.size is not None and found.box_size != (node.size.width, node.size.height)
    return moved or resized


def _key(node: NodeDoc, path: str, adapter: SchemaAdapter, self_class: str | None) -> str | None:
    if node.action is None:
        return None
    try:
        return adapter.normalize_action(node.action, self_class=self_class, path=path)
    except AssetDocumentContractError:
        return None  # Validate's other checks or Apply report the key


def placed_graphs(desired: tuple[GraphDoc, ...], live: LiveAsset, *, managed_roles: frozenset[str],
                  adapter: SchemaAdapter = GENERIC_ADAPTER, self_class: str | None = None) -> list[PlacedGraph]:
    """The graphs (subgraphs included) in which the document places nodes, paired as apply pairs them."""
    output = []
    try:
        for pairing in paired_graphs(desired, live, managed_roles=managed_roles, adapter=adapter,
                                     self_class=self_class):
            placed = [node for node in pairing.doc.nodes if _places(node, pairing.matched.get(node.id))]
            if not placed:
                continue
            keys = tuple(dict.fromkeys(
                key for node in placed if node.id not in pairing.matched and node.size is None
                for key in (_key(node, pairing.doc.node_path(node.id), adapter, self_class),) if key is not None))
            output.append(PlacedGraph(pairing, keys))
    except AssetDocumentContractError:
        return []  # the document's other checks (or its apply) report the problem
    return output


def _items(placed: PlacedGraph, sized: Mapping[str, LiveNode], templates: Mapping[str, tuple],
           adapter: SchemaAdapter, self_class: str | None) -> list[_Item]:
    """The boxes of a placed graph after the apply, where known: its document's nodes and the live nodes apply
    keeps without the document listing them (implicit nodes; the live nodes apply removes are left out). ``sized``: live nodes with
    their drawn sizes by NodeGuid; ``templates``: action key -> (width, height, box) of new nodes' templates."""
    pairing = placed.pairing
    doc = pairing.doc
    items = []
    for node in doc.nodes:
        matched = pairing.matched.get(node.id)
        found = sized.get(matched.guid, matched) if matched is not None else None
        position = node.position or (found.position if found is not None and doc.layout != "Auto" else None)
        if position is None:
            continue  # placed by the automatic layout, which overlaps nothing
        comment = node.size is not None or (found is not None and found.box_size is not None)
        size = (float(node.size.width), float(node.size.height)) if node.size is not None else _measured(found)
        if size is None and found is None:
            template = templates.get(_key(node, doc.node_path(node.id), adapter, self_class) or "")
            if template is not None:
                size, comment = (float(template[0]), float(template[1])), bool(template[2])
        if size is None:
            continue
        placed_here = _places(node, matched)
        path = doc.node_path(node.id) + ("/Position" if node.position is not None else "/Size")
        what = "comment box" if comment else "the editor's placeholder node" if node.ghost else "node"
        items.append(_Item(f"{what} '{node.id}'", node.id,
                           Box(position.x, position.y, size[0], size[1]), comment, placed_here, path))
    taken = {node.guid for node in pairing.matched.values()} | {node.guid for node in pairing.surplus}
    for node in pairing.live.nodes if pairing.live is not None else ():
        if node.guid in taken or node.position is None:
            continue
        size = _measured(sized.get(node.guid, node))
        if size is None:
            continue
        what = "comment box" if node.box_size is not None else "node"
        items.append(_Item(f"{what} '{node.title}'", node.title, Box(node.position.x, node.position.y, *size),
                           node.box_size is not None, False, doc.path))
    return items


def _intersection(a: Box, b: Box) -> Box | None:
    left, top = max(a.x, b.x), max(a.y, b.y)
    right, bottom = min(a.right, b.right), min(a.bottom, b.bottom)
    return Box(left, top, right - left, bottom - top) if right - left >= 1 and bottom - top >= 1 else None


def _contains(outer: Box, inner: Box) -> bool:
    return outer.x <= inner.x and outer.y <= inner.y and inner.right <= outer.right and inner.bottom <= outer.bottom


def _rect(box: Box) -> dict:
    return {"X": int(round(box.x)), "Y": int(round(box.y)), "Width": int(round(box.width)),
            "Height": int(round(box.height))}


def _describe(box: Box) -> str:
    rect = _rect(box)
    return f"{rect['Width']} x {rect['Height']} at ({rect['X']}, {rect['Y']})"


def _warning(code: str, subject: _Item, other: _Item, overlap: Box, text: str) -> RuntimeDiagnostic:
    return RuntimeDiagnostic(
        subject.path, code, f"{text}: the overlap is {_describe(overlap)} ({subject.label}: {_describe(subject.box)}; "
        f"{other.label}: {_describe(other.box)}).", DiagnosticSeverity.WARNING,
        (DiagnosticContext("nodes", [subject.name, other.name]), DiagnosticContext("overlap", _rect(overlap))))


def _capital(text: str) -> str:
    return text[:1].upper() + text[1:]


def _ordered(a: _Item, b: _Item) -> tuple[_Item, _Item]:
    """(the placed one, the other); both placed: document order."""
    return (a, b) if a.placed else (b, a)


def _graph_warnings(items: list[_Item]) -> Iterable[RuntimeDiagnostic]:
    nodes = [item for item in items if not item.comment]
    comments = [item for item in items if item.comment]
    for index, a in enumerate(nodes):
        for b in nodes[index + 1:]:
            overlap = (a.placed or b.placed) and _intersection(a.box, b.box)
            if overlap:
                subject, other = _ordered(a, b)
                yield _warning(NODE_OVERLAP, subject, other, overlap, f"{_capital(subject.label)} overlaps {other.label}")
    for index, a in enumerate(comments):
        for b in comments[index + 1:]:
            overlap = (a.placed or b.placed) and _intersection(a.box, b.box)
            if overlap and not _contains(a.box, b.box) and not _contains(b.box, a.box):
                subject, other = _ordered(a, b)
                yield _warning(COMMENT_OVERLAP, subject, other, overlap,
                               f"{_capital(subject.label)} partly overlaps {other.label}")
    for node in nodes:
        area = node.box.width * node.box.height
        best, best_area = None, 0.0
        for comment in comments:
            overlap = _intersection(node.box, comment.box)
            if overlap is not None and overlap.width * overlap.height > best_area:
                best, best_area = (comment, overlap), overlap.width * overlap.height
        if best is None or not (node.placed or best[0].placed) or best_area * 2 < area or _contains(best[0].box, node.box):
            continue
        comment, overlap = best
        subject, other = (node, comment) if node.placed else (comment, node)
        yield _warning(OUTSIDE_COMMENT, subject, other, overlap,
                       f"Node '{node.name}' is partly outside comment box '{comment.name}', which holds most of it")


def layout_warnings(placed: list[PlacedGraph], sized: Mapping[str, LiveNode], templates: Mapping[str, tuple], *,
                    adapter: SchemaAdapter = GENERIC_ADAPTER, self_class: str | None = None) -> tuple:
    """The warnings of ``placed_graphs``' graphs. ``sized``: their live nodes read with drawn sizes, by NodeGuid;
    ``templates``: action key -> (width, height, is a comment box) of the palette templates of new nodes."""
    found = []
    for graph in placed:
        found.extend(_graph_warnings(_items(graph, sized, templates, adapter, self_class)))
    if len(found) <= MAX_WARNINGS:
        return tuple(found)
    rest = len(found) - MAX_WARNINGS
    return tuple(found[:MAX_WARNINGS]) + (RuntimeDiagnostic(
        found[MAX_WARNINGS].path, LIMITED, f"{rest} more layout warning{'s' if rest != 1 else ''} not listed; fix the "
        "ones above and validate again.", DiagnosticSeverity.WARNING),)


def check(desired: tuple[GraphDoc, ...], live: LiveAsset, *, managed_roles: frozenset[str],
          measure_graphs: Callable[[list[str]], LiveAsset], measure_templates: Callable[[list[str]], Mapping],
          adapter: SchemaAdapter = GENERIC_ADAPTER, self_class: str | None = None) -> tuple:
    """Validate's layout warnings of ``desired`` on ``live`` (read without sizes). ``measure_graphs(guids)``
    reads those live graphs again with their nodes' drawn sizes; ``measure_templates(keys)`` gives the palette
    template sizes of action keys (key -> (width, height, box)). Neither is called when nothing is placed."""
    placed = placed_graphs(desired, live, managed_roles=managed_roles, adapter=adapter, self_class=self_class)
    if not placed:
        return ()
    guids = [graph.pairing.live.guid for graph in placed if graph.pairing.live is not None]
    measured = measure_graphs(guids) if guids else live
    sized = {node.guid: node for guid in guids for graph in (measured.graph(guid),) if graph is not None
             for node in graph.nodes}
    keys = list(dict.fromkeys(key for graph in placed for key in graph.new_keys))
    templates = measure_templates(keys) if keys else {}
    return layout_warnings(placed, sized, templates, adapter=adapter, self_class=self_class)
