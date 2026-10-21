"""Automatic layout of graph nodes: the pure layout, the K2 fields it brings (Layout, comment boxes) and
how apply uses it (nodes without Position, ``"Layout": "Auto"``)."""
from __future__ import annotations

import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.graphs import layout
from asset_document.graphs.layout import Box, LayoutEdge, LayoutNode, crossing_count, estimate_size, overlap_count, place
from asset_document.graphs.reconcile import pending_relayout
from asset_document.graphs.schemas import k2
from asset_document.tests.graph_support import ACTOR_CLASS, FakeGraphContext
from asset_document.tests.test_graph_executor import execute, mutations, read_live


def _node(node_id, title, inputs=(), outputs=(), exec_=True):
    width, height = estimate_size(title, inputs, outputs)
    return LayoutNode(node_id, width, height, exec_)


class _Graph:
    """A graph made of events, each a small script like the ones in a player character Blueprint."""

    def __init__(self):
        self.nodes: list[LayoutNode] = []
        self.edges: list[LayoutEdge] = []
        self.bands: list[list[str]] = []

    def add(self, node):
        self.nodes.append(node)
        self.bands[-1].append(node.id)
        return node.id

    def exec_(self, source, target, pin=0):
        self.edges.append(LayoutEdge(source, target, True, pin, 0))

    def data(self, source, target, pin=1):
        self.edges.append(LayoutEdge(source, target, False, 0, pin))

    def event(self, name):
        self.bands.append([])
        return self.add(_node(name, name, (), ("then",)))

    def branchy(self, name):
        """Event -> Branch(Health > 0) -> true: Set Health(Health + 1) -> Print / false: Print (9 nodes)."""
        event = self.event(name)
        branch = self.add(_node(f"{name}.Branch", "Branch", ("execute", "Condition"), ("True", "False")))
        greater = self.add(_node(f"{name}.Greater", ">", ("A", "B"), ("ReturnValue",), False))
        get1 = self.add(_node(f"{name}.Get1", "Health", (), ("Health",), False))
        set_ = self.add(_node(f"{name}.Set", "SET", ("execute", "Health"), ("then", "Health")))
        add = self.add(_node(f"{name}.Add", "+", ("A", "B"), ("ReturnValue",), False))
        get2 = self.add(_node(f"{name}.Get2", "Health", (), ("Health",), False))
        print_true = self.add(_node(f"{name}.PrintTrue", "Print String", ("execute", "In String"), ("then",)))
        print_false = self.add(_node(f"{name}.PrintFalse", "Print String", ("execute", "In String"), ("then",)))
        self.exec_(event, branch)
        self.exec_(branch, set_, 0)
        self.exec_(branch, print_false, 1)
        self.exec_(set_, print_true)
        self.data(greater, branch)
        self.data(get1, greater)
        self.data(add, set_)
        self.data(get2, add)

    def sequence(self, name):
        """Event -> Sequence -> then 0: Print / then 1: Delay -> Print (5 nodes)."""
        event = self.event(name)
        sequence = self.add(_node(f"{name}.Sequence", "Sequence", ("execute",), ("Then 0", "Then 1")))
        first = self.add(_node(f"{name}.Print1", "Print String", ("execute", "In String"), ("then",)))
        delay = self.add(_node(f"{name}.Delay", "Delay", ("execute", "Duration"), ("Completed",)))
        second = self.add(_node(f"{name}.Print2", "Print String", ("execute", "In String"), ("then",)))
        self.exec_(event, sequence)
        self.exec_(sequence, first, 0)
        self.exec_(sequence, delay, 1)
        self.exec_(delay, second)

    def scaled(self, name):
        """Event -> Set Speed(Speed * Scale) -> Print (6 nodes)."""
        event = self.event(name)
        set_ = self.add(_node(f"{name}.Set", "SET", ("execute", "Speed"), ("then", "Speed")))
        multiply = self.add(_node(f"{name}.Multiply", "*", ("A", "B"), ("ReturnValue",), False))
        speed = self.add(_node(f"{name}.Speed", "Speed", (), ("Speed",), False))
        scale = self.add(_node(f"{name}.Scale", "Scale", (), ("Scale",), False))
        print_ = self.add(_node(f"{name}.Print", "Print String", ("execute", "In String"), ("then",)))
        self.exec_(event, set_)
        self.exec_(set_, print_)
        self.data(multiply, set_)
        self.data(speed, multiply, 0)
        self.data(scale, multiply, 1)


def player_like_graph() -> _Graph:
    """123 nodes in 16 event bands (the size of a player Blueprint's event graph)."""
    graph = _Graph()
    for index in range(16):
        name = f"Event{index:02d}"
        (graph.branchy if index < 10 else graph.sequence if index < 13 else graph.scaled)(name)
    return graph


def boxes_of(nodes, positions):
    return {node.id: Box(*positions[node.id], node.width, node.height) for node in nodes}


def legacy_grid(nodes, edges):
    """The fallback before the automatic layout (a 320 x 160 grid per link depth, every node assumed to fit one cell)."""
    order = [node.id for node in nodes]
    depth = {node_id: 0 for node_id in order}
    for _ in order:
        changed = False
        for edge in edges:
            if depth[edge.target] < depth[edge.source] + 1:
                depth[edge.target] = min(depth[edge.source] + 1, len(order))
                changed = True
        if not changed:
            break
    occupied: list[tuple[float, float]] = []
    known: dict[str, tuple[float, float]] = {}
    ranked = sorted(edges, key=lambda edge: not edge.exec)
    upstream = {node_id: [edge.source for edge in ranked if edge.target == node_id] for node_id in order}
    downstream = {node_id: [edge.target for edge in ranked if edge.source == node_id] for node_id in order}

    def put(x, y):
        while any(abs(px - x) < 320 and abs(py - y) < 160 for px, py in occupied):
            y += 160
        occupied.append((x, y))
        return x, y

    targets = sorted(order, key=depth.__getitem__)
    for node_id in targets:
        anchor = next((known[item] for item in upstream[node_id] if item in known), None)
        if anchor is not None:
            known[node_id] = put(anchor[0] + 320, anchor[1])
    for node_id in reversed(targets):
        anchor = next((known[item] for item in downstream[node_id] if item in known), None)
        if node_id not in known and anchor is not None:
            known[node_id] = put(anchor[0] - 320, anchor[1] + 160)
    base = max((py for _, py in occupied), default=-320) + 320
    for node_id in targets:
        if node_id not in known:
            known[node_id] = put(depth[node_id] * 320, base)
    return known


def band_boxes(graph, boxes):
    output = []
    for members in graph.bands:
        items = [boxes[node_id] for node_id in members]
        left, top = min(box.x for box in items), min(box.y for box in items)
        output.append(Box(left, top, max(box.right for box in items) - left, max(box.bottom for box in items) - top))
    return output


class LayoutTests(unittest.TestCase):
    def test_player_sized_graph_has_no_overlaps_and_separate_bands(self):
        graph = player_like_graph()
        self.assertEqual((len(graph.nodes), len(graph.bands)), (123, 16))
        positions = place(graph.nodes, graph.edges, movable=[node.id for node in graph.nodes], fixed={})
        boxes = boxes_of(graph.nodes, positions)
        self.assertEqual(overlap_count(boxes), 0)
        bands = band_boxes(graph, boxes)
        self.assertEqual(sum(1 for i, a in enumerate(bands) for b in bands[i + 1:] if a.overlaps(b)), 0)
        # Bands are stacked top to bottom in document order.
        self.assertEqual([band.y for band in bands], sorted(band.y for band in bands))
        by_id = {node.id: node for node in graph.nodes}
        for edge in graph.edges:
            if edge.exec:
                self.assertLess(boxes[edge.source].right, boxes[edge.target].x, edge)       # exec flows left to right
            elif not by_id[edge.source].has_exec:
                self.assertLessEqual(boxes[edge.source].right, boxes[edge.target].x, edge)  # pure left of consumer
        # Branch outputs fan out in pin order: True's row above False's row.
        self.assertLess(boxes["Event00.Set"].y, boxes["Event00.PrintFalse"].y)
        self.assertEqual(boxes["Event00.Set"].y, boxes["Event00.Branch"].y)
        self.assertLess(boxes["Event10.Print1"].y, boxes["Event10.Delay"].y)
        # Grid-snapped and deterministic.
        self.assertTrue(all(x % layout.GRID == 0 and y % layout.GRID == 0 for x, y in positions.values()))
        self.assertEqual(positions, place(graph.nodes, graph.edges, movable=[node.id for node in graph.nodes], fixed={}))

    def test_quality_against_the_previous_fallback(self):
        graph = player_like_graph()
        after = boxes_of(graph.nodes, place(graph.nodes, graph.edges, movable=[n.id for n in graph.nodes], fixed={}))
        before = boxes_of(graph.nodes, legacy_grid(graph.nodes, graph.edges))
        def mixed(boxes):
            bands = band_boxes(graph, boxes)
            return sum(1 for i, a in enumerate(bands) for b in bands[i + 1:] if a.overlaps(b))
        # With these estimated sizes (all within the old 320 x 160 cell) the old grid overlaps no node, but its
        # events run into each other (81 pairs of bands share area) and its wires cross 194 times (13 now).
        self.assertGreater(mixed(before), 40)
        self.assertEqual((overlap_count(after), mixed(after)), (0, 0))
        self.assertLess(crossing_count(after, graph.edges) * 5, crossing_count(before, graph.edges))

    def test_new_nodes_go_next_to_their_anchor_and_clear_everything(self):
        nodes = [_node("Begin", "Event BeginPlay", (), ("then",)), _node("Print", "Print String", ("execute", "In String"), ("then",)),
                 _node("Delay", "Delay", ("execute", "Duration"), ("Completed",)),
                 _node("Print2", "Print String", ("execute", "In String"), ("then",)),
                 _node("Other", "Custom Event", (), ("then",))]
        edges = [LayoutEdge("Begin", "Print", True), LayoutEdge("Print", "Delay", True), LayoutEdge("Delay", "Print2", True)]
        fixed = {"Begin": (0, 0), "Print": (400, 0), "Other": (760, 0)}
        blocker = Box(1200, 300, 200, 100)
        moved = place(nodes, edges, movable=["Delay", "Print2"], fixed=fixed, obstacles=[blocker])
        self.assertEqual(set(moved), {"Delay", "Print2"})
        boxes = boxes_of(nodes, {**fixed, **moved})
        self.assertEqual(overlap_count(boxes), 0)
        self.assertFalse(any(boxes[node_id].overlaps(blocker) for node_id in moved))
        self.assertGreater(boxes["Delay"].x, boxes["Print"].right)
        self.assertLess(boxes["Delay"].x - boxes["Print"].right, 2 * layout.COLUMN_GAP)  # next to Print, moved down only
        self.assertLess(boxes["Delay"].x, boxes["Print2"].x)

    def test_a_band_without_staying_nodes_goes_below_everything(self):
        nodes = [_node("A", "Event A", (), ("then",)), _node("B", "Print String", ("execute",), ("then",)),
                 _node("C", "Custom Event", (), ("then",)), _node("D", "Print String", ("execute",), ("then",))]
        edges = [LayoutEdge("A", "B", True), LayoutEdge("C", "D", True)]
        fixed = {"A": (100, 100), "B": (500, 100)}
        moved = place(nodes, edges, movable=["C", "D"], fixed=fixed)
        bottom = max(Box(*fixed[n.id], n.width, n.height).bottom for n in nodes[:2])
        self.assertEqual(moved["C"][0], 96)  # left edge of what is there (snapped)
        self.assertGreaterEqual(moved["C"][1], bottom + layout.BAND_GAP - layout.GRID)
        self.assertLess(moved["C"][0], moved["D"][0])

    def test_data_flow_graph_and_cycles(self):
        # No exec flow: the sink is the band root, its sources lie left of it.
        nodes = [_node("Out", "Output Pose", ("Result",), (), False), _node("Blend", "Blend", ("A", "B"), ("Pose",), False),
                 _node("A", "Idle", (), ("Pose",), False), _node("B", "Run", (), ("Pose",), False)]
        edges = [LayoutEdge("Blend", "Out"), LayoutEdge("A", "Blend", target_pin=0), LayoutEdge("B", "Blend", target_pin=1)]
        boxes = boxes_of(nodes, place(nodes, edges, movable=[n.id for n in nodes], fixed={}))
        self.assertEqual(overlap_count(boxes), 0)
        self.assertLess(boxes["Blend"].right, boxes["Out"].x)
        self.assertLess(boxes["A"].right, boxes["Blend"].x)
        self.assertLess(boxes["A"].y, boxes["B"].y)
        # An exec loop (a back edge) terminates and stays left to right along the forward edges.
        loop = [_node("E", "Event", (), ("then",)), _node("X", "Gate", ("execute",), ("then",)),
                _node("Y", "Print String", ("execute",), ("then",))]
        loop_edges = [LayoutEdge("E", "X", True), LayoutEdge("X", "Y", True), LayoutEdge("Y", "X", True)]
        boxes = boxes_of(loop, place(loop, loop_edges, movable=["E", "X", "Y"], fixed={}))
        self.assertLess(boxes["E"].x, boxes["X"].x)
        self.assertLess(boxes["X"].x, boxes["Y"].x)
        self.assertEqual(overlap_count(boxes), 0)

    def test_comment_boxes_never_block(self):
        nodes = [_node("A", "Event", (), ("then",)), LayoutNode("Box", 2000, 2000, False, True),
                 _node("B", "Print String", ("execute",), ("then",))]
        moved = place(nodes, [LayoutEdge("A", "B", True)], movable=["B"], fixed={"A": (0, 0), "Box": (-100, -100)})
        self.assertEqual(moved["B"][1], 0)  # right of A, not pushed below the comment box


BEGIN_PRINT = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
    {"Id": "BeginPlay", "Action": "K2:BeginPlay"}, {"Id": "Print", "Action": "K2:PrintString"},
    {"Id": "Branch", "Action": "K2:Branch"}],
    "Links": [{"From": "BeginPlay:then", "To": "Print:execute"}, {"From": "Print:then", "To": "Branch:execute"}]}]}


def positions(context):
    live = read_live(context)
    return {node.action_key: (node.position.x, node.position.y) for graph in live.graphs for node in graph.nodes
            if node.position is not None}


class ApplyLayoutTests(unittest.TestCase):
    def context(self):
        return FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")

    def test_nodes_without_position_stay_put_unless_layout_auto(self):
        context = self.context()
        execute(context, BEGIN_PRINT)
        first = positions(context)
        # Move Print by hand; a document without Positions keeps it there.
        guid = next(node.guid for graph in read_live(context).graphs for node in graph.nodes
                    if node.action_key.endswith(":PrintString"))
        context._graph_node_layout_set_v1({"node_guid": guid, "position": {"x": -2000, "y": -2000}})
        before = len(context.calls)
        execute(context, BEGIN_PRINT)
        self.assertEqual(mutations_since(context, before), [])
        # "Layout": "Auto" lays the graph out again (moves Print back next to BeginPlay) ...
        relayout = {"UbergraphPages": [dict(BEGIN_PRINT["UbergraphPages"][0], Layout="Auto")]}
        self.assertEqual([move["Node"] for move in pending(context, relayout)["/Body/UbergraphPages/0"]],
                         ["Print"])  # what Diff reports
        before = len(context.calls)
        execute(context, relayout)
        self.assertIn("graph.node.layout.set.v1", mutations_since(context, before))
        after = positions(context)
        begin = next(key for key in after if key.endswith("ReceiveBeginPlay"))
        print_ = next(key for key in after if key.endswith(":PrintString"))
        self.assertGreater(after[print_][0], after[begin][0])
        self.assertEqual(after[print_][1], after[begin][1])
        # ... and doing it again changes nothing.
        before = len(context.calls)
        execute(context, relayout)
        self.assertEqual(mutations_since(context, before), [])
        self.assertEqual(pending(context, relayout), {})
        self.assertEqual(first[begin], after[begin])

    def test_layout_auto_on_a_new_graph_is_stable(self):
        context = self.context()
        relayout = {"UbergraphPages": [dict(BEGIN_PRINT["UbergraphPages"][0], Layout="Auto")]}
        execute(context, relayout)
        before = len(context.calls)
        execute(context, relayout)
        self.assertEqual(mutations_since(context, before), [])
        self.assertTrue(mutations(context))


    def test_layout_auto_in_a_subgraph_is_reported_and_applied(self):
        # KI-11: a subgraph's "Layout": "Auto" (a collapsed graph's, a state's) is a Diff difference at its own path.
        context = self.context()
        inner = {"Name": "Inner", "Role": "other", "Nodes": [{"Id": "First", "Action": "K2:PrintString"},
                                                             {"Id": "Second", "Action": "K2:Branch"}],
                 "Links": [{"From": "First:then", "To": "Second:execute"}]}
        body = {"UbergraphPages": [dict(BEGIN_PRINT["UbergraphPages"][0], Subgraphs=[inner])]}
        execute(context, body)
        subgraph = next(guid for guid, graph in context.graphs.items() if graph["info"]["name"] == "Inner")
        for node in context.graphs[subgraph]["nodes"]:
            context._graph_node_layout_set_v1({"node_guid": node["node_guid"], "position": {"x": 500, "y": 500}})
        self.assertEqual(pending(context, body), {})  # no Layout: the human's piled nodes stay
        relayout = {"UbergraphPages": [dict(BEGIN_PRINT["UbergraphPages"][0], Subgraphs=[dict(inner, Layout="Auto")])]}
        moves = pending(context, relayout)
        self.assertEqual(list(moves), ["/Body/UbergraphPages/0/Subgraphs/0"])
        self.assertIn("Second", [move["Node"] for move in moves["/Body/UbergraphPages/0/Subgraphs/0"]])
        before = len(context.calls)
        execute(context, relayout)
        self.assertIn("graph.node.layout.set.v1", mutations_since(context, before))
        self.assertEqual(pending(context, relayout), {})
        before = len(context.calls)
        execute(context, relayout)
        self.assertEqual(mutations_since(context, before), [])

def pending(context, body):
    graphs, roles = k2.decode_body(body, self_class=ACTOR_CLASS)
    return pending_relayout(graphs, read_live(context), managed_roles=roles, adapter=k2.K2_ADAPTER, self_class=ACTOR_CLASS)


def mutations_since(context, start):
    return [call[0] for call in context.calls[start:] if call[0] not in ("graph.list.v1", "graph.query.v1")]


class K2LayoutFieldTests(unittest.TestCase):
    def decode(self, graph):
        return k2.decode_body({"UbergraphPages": [graph]}, self_class=ACTOR_CLASS)[0][0]

    def error(self, graph):
        with self.assertRaises(AssetDocumentContractError) as caught:
            self.decode(graph)
        return caught.exception.diagnostic.code, caught.exception.diagnostic.path

    def test_comment_box_fields_round_trip(self):
        graph = self.decode({"Name": "EventGraph", "Layout": "Auto", "Nodes": [
            {"Id": "Box", "Action": "K2:Comment", "Comment": "Movement", "Position": {"X": -32, "Y": -64},
             "Size": {"Width": 640, "Height": 320}, "Properties": {"FontSize": 24}},
            {"Id": "Print", "Action": "K2:PrintString", "Comment": "note", "CommentBubbleVisible": True}], "Links": []})
        self.assertEqual(graph.layout, "Auto")
        box, print_ = graph.nodes
        self.assertEqual(box.action, "K2:EdGraphNode_Comment")
        self.assertEqual((box.size.width, box.size.height), (640, 320))
        self.assertTrue(print_.comment_bubble_visible)
        encoded = k2.encode_node(box)
        self.assertEqual(encoded["Size"], {"Width": 640, "Height": 320})
        self.assertEqual(k2.encode_node(print_)["CommentBubbleVisible"], True)

    def test_field_errors(self):
        self.assertEqual(self.error({"Name": "EventGraph", "Nodes": [
            {"Id": "Print", "Action": "K2:PrintString", "Size": {"Width": 10, "Height": 10}}], "Links": []}),
            ("Graph.NodeNotResizable", "/Body/UbergraphPages/0/Nodes/0/Size"))
        self.assertEqual(self.error({"Name": "EventGraph", "Nodes": [
            {"Id": "Box", "Action": "K2:Comment", "CommentBubbleVisible": True}], "Links": []})[0], "Graph.UnknownField")
        self.assertEqual(self.error({"Name": "EventGraph", "Nodes": [
            {"Id": "Box", "Action": "K2:Comment", "Size": {"Width": 0, "Height": 10}}], "Links": []}),
            ("Graph.InvalidSize", "/Body/UbergraphPages/0/Nodes/0/Size"))
        # JSON numbers read through the editor arrive as floats: a whole number is accepted.
        box = self.decode({"Name": "EventGraph", "Nodes": [
            {"Id": "Box", "Action": "K2:Comment", "Size": {"Width": 640.0, "Height": 320.0}}], "Links": []}).nodes[0]
        self.assertEqual((type(box.size.width), box.size.width, box.size.height), (int, 640, 320))
        self.assertEqual(self.error({"Name": "EventGraph", "Layout": "Grid", "Nodes": [], "Links": []})[0],
                         "Graph.InvalidLayout")


class AnimLayoutFieldTests(unittest.TestCase):
    def test_anim_graphs_and_nested_graphs_accept_layout(self):
        from asset_document.graphs.schemas import anim
        graph = anim.decode_anim_graph({"Layout": "Auto", "Nodes": [
            {"Id": "Locomotion", "Action": "Anim:StateMachine", "Graph": {"Layout": "Auto", "Nodes": [
                {"Id": "Idle", "Action": "SM:State", "Graph": {"Layout": "Auto", "Nodes": []}}], "Links": []}}],
            "Links": [{"From": "Locomotion:Pose", "To": "Output:Result"}]})
        self.assertEqual([item.layout for item in graph.walk()], ["Auto", "Auto", "Auto"])
        self.assertEqual(graph.subgraphs[0].path, "/Body/AnimGraph/Nodes/0/Graph")
        layers = anim.decode_anim_layers([{"Name": "Upper", "Layout": "Auto", "Nodes": []}])
        self.assertEqual(layers[0].layout, "Auto")
        with self.assertRaises(AssetDocumentContractError) as caught:
            anim.decode_anim_graph({"Layout": True, "Nodes": []})
        self.assertEqual((caught.exception.diagnostic.code, caught.exception.diagnostic.path),
                         ("Graph.InvalidLayout", "/Body/AnimGraph/Layout"))


class RelayoutDifferenceTests(unittest.TestCase):
    MOVES = {"/Body/FunctionGraphs/0/Subgraphs/1": [{"Node": "B", "Position": {"X": 300, "Y": 0}}],
             "/Body/AnimGraph": [{"Node": "Blend", "Position": {"X": -300, "Y": 0}}]}

    def test_relayout_differences_name_each_graph(self):
        from asset_document.profiles.ublueprint_runtime import PENDING_RELAYOUT, relayout_differences
        found = relayout_differences({PENDING_RELAYOUT: self.MOVES})
        self.assertEqual([item.path for item in found], ["/Body/AnimGraph/Layout", "/Body/FunctionGraphs/0/Subgraphs/1/Layout"])
        self.assertEqual(found[1].after, {"Moves": self.MOVES["/Body/FunctionGraphs/0/Subgraphs/1"]})
        self.assertEqual(relayout_differences(None), [])

    def test_widget_and_anim_diffs_report_a_pending_relayout_only_for_the_diff_operation(self):
        from asset_document.profiles import anim_blueprint_runtime, widget_blueprint_runtime
        from asset_document.profiles.ublueprint_runtime import PENDING_RELAYOUT
        defaults = {PENDING_RELAYOUT: {"/Body/UbergraphPages/0": self.MOVES["/Body/AnimGraph"]}}
        for module, target in ((widget_blueprint_runtime, "/Game/UI/WBP_Hud"), (anim_blueprint_runtime, "/Game/ABP_Hero")):
            document = {"Target": target, "Body": {}, "Properties": {}}
            self.assertEqual([item.path for item in module.diff(document, document, True, defaults)],
                             ["/Body/UbergraphPages/0/Layout"], module.__name__)
            self.assertEqual(module.diff(document, document, False, defaults), (), module.__name__)


if __name__ == "__main__":
    unittest.main()
