# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Node sizes of an Extract: how big the graph editor draws each node, beside the document.

A node's drawn size depends on fonts, DPI scale, the engine version and its pins' value widgets, so it is
not part of the document (applying an extract must change nothing, and a Diff must not see it). An Extract
that asks for it (``include_layout``) gets it out of band: while ``collecting()`` runs, graph reads measure
their nodes (``GraphExecutor.read_live``) and ``extract_graphs`` records each graph's sizes here; the runtime
hands them to the native side under ``KEY``, which moves them out of the document into the result's
``layout`` (FAssetDocumentService::Extract).

``layout`` is ``{"Graphs": {<graph id>: {<node Id>: {"Width", "Height"}}}}``: a graph id is the graph's
section and name (``UbergraphPages/EventGraph``; a subgraph adds its own name). A comment box's size is its
own (NodeWidth / NodeHeight, the document's ``Size``); any other node's is its widget's, as the editor lays it
out without zoom. A node the editor cannot measure here (an Anim Graph or state machine node) gets an
estimate from its title and pins, marked ``"Estimated": true``.
"""
from __future__ import annotations

from contextlib import contextmanager
from contextvars import ContextVar
from typing import Iterable, Iterator

from asset_document.graphs.layout import live_node_size
from asset_document.graphs.live import LiveAsset, LiveNode
from asset_document.graphs.model import GraphDoc

# The top-level key of an extracted document that carries the sizes to the native side (never applied).
KEY = "_Layout"

_SINK: ContextVar[dict | None] = ContextVar("asset_document_node_layout", default=None)


def requested() -> bool:
    """An Extract that reports node sizes is running: graph reads measure their nodes."""
    return _SINK.get() is not None


@contextmanager
def collecting(enabled: bool = True) -> Iterator[dict]:
    """Collects the node sizes ``record`` reports while it runs (graph id -> node Id -> size); nothing when not
    ``enabled``."""
    sink: dict = {}
    token = _SINK.set(sink) if enabled else None
    try:
        yield sink
    finally:
        if token is not None:
            _SINK.reset(token)


def node_size(node: LiveNode) -> dict:
    """``{"Width", "Height"}`` of a live node as the editor draws it (``"Estimated": true`` when not measured)."""
    width, height = live_node_size(node)
    size = {"Width": int(round(width)), "Height": int(round(height))}
    if node.box_size is None and node.widget_size is None:
        size["Estimated"] = True
    return size


def graph_sizes(graphs: Iterable[GraphDoc], live: LiveAsset) -> dict:
    """graph id -> node Id -> size of the extracted ``graphs`` (subgraphs included), from the live nodes they were
    read from (paired by NodeGuid)."""
    nodes = {node.guid: node for graph in live.graphs for node in graph.nodes}
    output = {}
    for root in graphs:
        for graph in root.walk():
            sizes = {doc.id: node_size(nodes[doc.guid]) for doc in graph.nodes if doc.guid in nodes}
            if sizes:
                output[graph.id] = sizes
    return output


def record(graphs: Iterable[GraphDoc], live: LiveAsset) -> None:
    """Report the sizes of extracted graphs to a running ``collecting()`` (else nothing)."""
    sink = _SINK.get()
    if sink is not None:
        sink.update(graph_sizes(graphs, live))


def layout_value(sink: dict) -> dict | None:
    """The ``layout`` value of what ``collecting()`` gathered (None: no graph)."""
    return {"Graphs": dict(sink)} if sink else None
