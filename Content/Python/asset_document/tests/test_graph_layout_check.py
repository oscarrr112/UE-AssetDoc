# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Node sizes beside an extract (graphs.node_layout) and Validate's layout warnings (graphs.layout_check)."""
from __future__ import annotations

import unittest

from asset_document.diagnostics import DiagnosticSeverity
from asset_document.graphs import layout_check, node_layout
from asset_document.graphs.executor import GraphExecutor
from asset_document.graphs.extract import extract_graphs
from asset_document.graphs.schemas import k2
from asset_document.tests.graph_support import ACTOR_CLASS, FakeGraphContext
from asset_document.tests.test_graph_executor import execute, read_live

EVENT_GRAPH = "A0000000000000000000000000000001"


def _context():
    return FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")


def _graph(*nodes, links=(), **fields):
    return {"UbergraphPages": [{"Name": "EventGraph", "Nodes": list(nodes), "Links": list(links), **fields}]}


def _node(node_id, action, x=None, y=None, **fields):
    node = {"Id": node_id, "Action": action, **fields}
    if x is not None:
        node["Position"] = {"X": x, "Y": y}
    return node


def _measure_all(context, width=200, height=100):
    """Every stored node drawn at width x height (the editor's measure)."""
    for graph in context.graphs.values():
        for node in graph["nodes"]:
            node["widget_size"] = {"width": width, "height": height}


def _add_live_comment(context, guid, x, y, width, height, title="Movement"):
    context.graphs[EVENT_GRAPH]["nodes"].append({
        "node_guid": guid, "class": "/Script/UnrealEd.EdGraphNode_Comment", "action_key": "K2:EdGraphNode_Comment",
        "title": title, "implicit_role": None, "position": {"x": x, "y": y}, "comment": title,
        "enabled_state": "enabled", "parent_node_guid": None, "subnode_index": None, "subgraph_guids": [],
        "can_add_pin": False, "extras": {}, "pins": [], "box_size": {"width": width, "height": height}})


class _Check:
    """layout_check.check on a fake context, recording what it measured."""

    def __init__(self, context, template=(150, 90, False)):
        self.context = context
        self.template = template
        self.graphs_measured = []
        self.keys_measured = []

    def measure_graphs(self, guids):
        self.graphs_measured.append(list(guids))
        return read_live(self.context)

    def measure_templates(self, keys):
        self.keys_measured.append(list(keys))
        return {key: self.template for key in keys}

    def __call__(self, body):
        graphs, roles = k2.decode_body(body, self_class=ACTOR_CLASS)
        return layout_check.check(graphs, read_live(self.context), managed_roles=roles,
                                  measure_graphs=self.measure_graphs, measure_templates=self.measure_templates,
                                  adapter=k2.K2_ADAPTER, self_class=ACTOR_CLASS)


def _codes(found):
    return [item.code for item in found]


def _details(item):
    return {context.key: context.value for context in item.context}


class NodeSizeTests(unittest.TestCase):
    def test_an_extract_that_asks_reads_and_reports_every_node_size(self):
        context = _context()
        execute(context, _graph(_node("Begin", "K2:BeginPlay", 0, 0), _node("Print", "K2:PrintString", 300, 0),
                                links=[{"From": "Begin:then", "To": "Print:execute"}]))
        print_ = next(node for node in context.graphs[EVENT_GRAPH]["nodes"] if node["action_key"].endswith("PrintString"))
        print_["widget_size"] = {"width": 225, "height": 120}
        _add_live_comment(context, "C0000000000000000000000000000077", -40, -80, 640, 320)
        before = len(context.calls)
        with node_layout.collecting() as sink:
            self.assertTrue(node_layout.requested())
            executor = GraphExecutor(context, read_handle=object(), route_id="route.blueprint.v1", adapter=k2.K2_ADAPTER)
            live = executor.read_live()
            graphs = extract_graphs(live, sections=k2.SECTION_BY_ROLE, adapter=k2.K2_ADAPTER, self_class=ACTOR_CLASS)
        queries = [args for name, args in context.calls[before:] if name == "graph.query.v1"]
        self.assertTrue(queries and all(args.get("include_sizes") is True for args in queries))
        sizes = node_layout.layout_value(sink)["Graphs"]
        event_graph = next(graph for graph in graphs if graph.name == "EventGraph")
        ids = {node.guid: node.id for node in event_graph.nodes}
        self.assertEqual(sizes["UbergraphPages/EventGraph"][ids[print_["node_guid"]]], {"Width": 225, "Height": 120})
        # A comment box: its own size (the document's Size).
        self.assertEqual(sizes["UbergraphPages/EventGraph"][ids["C0000000000000000000000000000077"]],
                         {"Width": 640, "Height": 320})
        # A node the editor did not measure: an estimate, marked.
        begin = sizes["UbergraphPages/EventGraph"][ids[next(node["node_guid"] for node in context.graphs[EVENT_GRAPH]["nodes"]
                                                             if node["action_key"].endswith("ReceiveBeginPlay"))]]
        self.assertTrue(begin["Estimated"])
        self.assertGreater(begin["Width"], 0)
        # Every extracted node has a size; the sizes are not part of the document.
        self.assertEqual(set(sizes["UbergraphPages/EventGraph"]), {node.id for node in event_graph.nodes})
        encoded = k2.encode_body(graphs)["UbergraphPages"][0]["Nodes"]
        self.assertNotIn("Size", next(node for node in encoded if node["Id"] == ids[print_["node_guid"]]))
        self.assertNotIn("Estimated", str(encoded))

    def test_an_extract_that_does_not_ask_reads_no_sizes(self):
        context = _context()
        self.assertFalse(node_layout.requested())
        with node_layout.collecting(False) as sink:
            self.assertFalse(node_layout.requested())
            live = read_live(context)
            extract_graphs(live, sections=k2.SECTION_BY_ROLE, adapter=k2.K2_ADAPTER, self_class=ACTOR_CLASS)
        self.assertIsNone(node_layout.layout_value(sink))
        self.assertTrue(all("include_sizes" not in args for name, args in context.calls if name == "graph.query.v1"))


class LayoutWarningTests(unittest.TestCase):
    def setUp(self):
        self.context = _context()
        # BeginPlay (the editor's placeholder, made real) at (0, 0); Print at (300, 0); the unlisted placeholder
        # events ActorBeginOverlap and Tick are removed. Every node is drawn 200 x 100.
        self.placed = [_node("Begin", "K2:BeginPlay", 0, 0), _node("Print", "K2:PrintString", 300, 0)]
        execute(self.context, _graph(*self.placed, links=[{"From": "Begin:then", "To": "Print:execute"}]))
        _measure_all(self.context)
        self.check = _Check(self.context)

    def test_a_document_that_moves_nothing_is_not_judged_or_measured(self):
        self.assertEqual(self.check(_graph(*self.placed)), ())
        self.assertEqual(self.check(_graph(_node("Begin", "K2:BeginPlay"), _node("Print", "K2:PrintString"))), ())
        self.assertEqual((self.check.graphs_measured, self.check.keys_measured), ([], []))

    def test_a_new_node_placed_on_another_names_both_and_the_overlap(self):
        found = self.check(_graph(*self.placed, _node("Branch", "K2:Branch", 400, 50)))
        self.assertEqual(_codes(found), [layout_check.NODE_OVERLAP])
        warning = found[0]
        self.assertEqual(warning.severity, DiagnosticSeverity.WARNING)
        self.assertEqual(warning.path, "/Body/UbergraphPages/0/Nodes/2/Position")
        self.assertIn("node 'Branch'", warning.message.replace("Node", "node"))
        self.assertIn("node 'Print'", warning.message)
        # Branch (template 150 x 90) at (400, 50) and Print (200 x 100) at (300, 0): 100 x 50 at (400, 50).
        self.assertIn("100 x 50 at (400, 50)", warning.message)
        self.assertEqual(_details(warning), {"nodes": ["Branch", "Print"],
                                             "overlap": {"X": 400, "Y": 50, "Width": 100, "Height": 50}})
        self.assertEqual(self.check.keys_measured, [["K2:IfThenElse"]])
        self.assertEqual(self.check.graphs_measured, [[EVENT_GRAPH]])

    def _placeholders(self):
        """A new Actor Blueprint's three placeholder events as its extract lists them (Ghost, Position), drawn
        200 x 100, and a check of documents on it."""
        context = _context()
        nodes = k2.encode_body(extract_graphs(read_live(context), sections=k2.SECTION_BY_ROLE, adapter=k2.K2_ADAPTER,
                                              self_class=ACTOR_CLASS))["UbergraphPages"][0]["Nodes"]
        self.assertEqual([(node["Id"], node.get("Ghost")) for node in nodes],
                         [("BeginPlay", True), ("ActorBeginOverlap", True), ("Tick", True)])
        _measure_all(context)
        return {node["Id"]: node for node in nodes}, _Check(context)

    def test_a_listed_placeholder_event_is_in_the_way_and_judged_when_moved(self):
        ghosts, check = self._placeholders()
        overlap = ghosts["ActorBeginOverlap"]["Position"]
        late = _node("Late", "K2:PrintString", overlap["X"] + 100, overlap["Y"] + 42)
        found = check(_graph(*ghosts.values(), late))
        self.assertEqual(_codes(found), [layout_check.NODE_OVERLAP])
        self.assertIn("the editor's placeholder node 'ActorBeginOverlap'", found[0].message)
        self.assertEqual(_details(found[0])["overlap"], {"X": overlap["X"] + 100, "Y": overlap["Y"] + 42,
                                                         "Width": 100, "Height": 58})
        # A listed placeholder moved onto another is judged like any node.
        tick = ghosts["Tick"]["Position"]
        moved = dict(ghosts["ActorBeginOverlap"], Position={"X": tick["X"] + 50, "Y": tick["Y"]})
        found = check(_graph(ghosts["BeginPlay"], moved, ghosts["Tick"]))
        self.assertEqual(_codes(found), [layout_check.NODE_OVERLAP])
        self.assertEqual(found[0].path, "/Body/UbergraphPages/0/Nodes/1/Position")
        self.assertIn("the editor's placeholder node 'Tick'", found[0].message)

    def test_a_placeholder_event_the_document_leaves_out_is_not_in_the_way(self):
        # Apply removes it, so a node placed where it is overlaps nothing.
        ghosts, check = self._placeholders()
        overlap = ghosts["ActorBeginOverlap"]["Position"]
        late = _node("Late", "K2:PrintString", overlap["X"] + 100, overlap["Y"] + 42)
        self.assertEqual(check(_graph(ghosts["BeginPlay"], ghosts["Tick"], late)), ())

    def test_untouched_overlaps_are_not_judged_but_a_move_into_one_is(self):
        piled = [_node("Begin", "K2:BeginPlay", 0, 0), _node("Print", "K2:PrintString", 100, 20)]
        execute(self.context, _graph(*piled, links=[{"From": "Begin:then", "To": "Print:execute"}]))
        self.assertEqual(self.check(_graph(*piled)), ())
        moved = [piled[0], _node("Print", "K2:PrintString", 120, 20)]
        found = self.check(_graph(*moved))
        self.assertEqual(_codes(found), [layout_check.NODE_OVERLAP])
        self.assertEqual(found[0].path, "/Body/UbergraphPages/0/Nodes/1/Position")
        # Moving it clear: nothing.
        self.assertEqual(self.check(_graph(piled[0], _node("Print", "K2:PrintString", 300, 0))), ())

    def test_nodes_the_layout_places_are_not_judged(self):
        found = self.check(_graph(*self.placed, _node("Branch", "K2:Branch")))
        self.assertEqual(found, ())
        self.assertEqual(self.check(_graph(_node("Begin", "K2:BeginPlay"), _node("Print", "K2:PrintString", 900, 0),
                                           Layout="Auto")), ())

    def test_comment_boxes(self):
        box = lambda node_id, x, y, w, h: _node(node_id, "K2:Comment", x, y, Size={"Width": w, "Height": h})  # noqa: E731
        # Two boxes crossing: one warning. A box inside another: none.
        found = self.check(_graph(*self.placed, box("Left", -100, -500, 400, 300), box("Right", 200, -450, 400, 300),
                                  box("Inner", -80, -460, 100, 100)))
        self.assertEqual(_codes(found), [layout_check.COMMENT_OVERLAP])
        self.assertEqual(_details(found[0]), {"nodes": ["Left", "Right"],
                                              "overlap": {"X": 200, "Y": -450, "Width": 100, "Height": 250}})
        self.assertIn("Comment box 'Left' partly overlaps comment box 'Right'", found[0].message)
        # A node mostly in a box but sticking out of it (Print spans 300..500, the box ends at 450).
        found = self.check(_graph(*self.placed, box("Frame", 250, -80, 200, 300)))
        self.assertEqual(_codes(found), [layout_check.OUTSIDE_COMMENT])
        self.assertEqual(found[0].path, "/Body/UbergraphPages/0/Nodes/2/Position")
        self.assertEqual(_details(found[0])["nodes"], ["Frame", "Print"])
        # A box holding the nodes whole: nothing; a node only touched by a box's edge: nothing.
        self.assertEqual(self.check(_graph(*self.placed, box("Around", -40, -80, 600, 240))), ())
        self.assertEqual(self.check(_graph(*self.placed, box("Beside", 480, -80, 300, 240))), ())

    def test_a_live_comment_box_resized_is_judged(self):
        _add_live_comment(self.context, "C0000000000000000000000000000077", -40, -80, 600, 240)
        live = read_live(self.context)
        node_id = "Movement"
        box = next(node for graph in live.graphs for node in graph.nodes if node.box_size is not None)
        unchanged = _node(node_id, "K2:Comment", -40, -80, NodeGuid=box.guid, Size={"Width": 600, "Height": 240})
        self.assertEqual(self.check(_graph(*self.placed, unchanged)), ())
        shrunk = dict(unchanged, Size={"Width": 500, "Height": 240})
        found = self.check(_graph(*self.placed, shrunk))
        self.assertEqual(_codes(found), [layout_check.OUTSIDE_COMMENT])
        self.assertEqual(found[0].path, "/Body/UbergraphPages/0/Nodes/2/Position")
        self.assertEqual(_details(found[0])["nodes"], ["Movement", "Print"])

    def test_many_warnings_are_limited(self):
        nodes = [_node(f"P{index}", "K2:PrintString", 1000, 1000) for index in range(8)]
        found = self.check(_graph(*self.placed, *nodes))
        self.assertEqual(len(found), layout_check.MAX_WARNINGS + 1)
        self.assertEqual(found[-1].code, layout_check.LIMITED)
        self.assertIn("8 more layout warnings", found[-1].message)  # 8 * 7 / 2 = 28 pairs


if __name__ == "__main__":
    unittest.main()
