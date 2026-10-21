from __future__ import annotations

from dataclasses import FrozenInstanceError
import unittest

from asset_document.diagnostics import AssetDocumentContractError, contract_error
from asset_document.graphs.live import (
    QUERY_GRAPH_CHUNK, decode_graph_list, decode_live_asset, decode_node, decode_query, list_graphs, query_graphs,
)
from asset_document.graphs.model import (
    Endpoint, GraphDoc, LinkDoc, NodeDoc, NodeProperties, PinOverride, PinType, Position, Signature, UserPin,
)
from asset_document.tests.graph_support import G_EVENT, N_BEGIN, N_BRANCH, N_PRINT, fixture, live_asset


def code_of(test, call):
    with test.assertRaises(AssetDocumentContractError) as caught:
        call()
    return caught.exception.diagnostic.code


class GraphModelTests(unittest.TestCase):
    def test_schema_agnostic_graph_doc(self):
        graph = GraphDoc(id="Tree", role="bt_root", name="BehaviorTree", schema_tag="BT", nodes=(
            NodeDoc("Root", implicit="root"),
            NodeDoc("Seq", action="BT:Composite:/Script/AIModule.BTComposite_Sequence", parent="Root",
                    properties=NodeProperties(template={"bIgnoreRestart": True})),
            NodeDoc("Wait", action="BT:Task:/Script/AIModule.BTTask_Wait", parent="Seq", guid="ab" * 16)),
            links=(LinkDoc(Endpoint("Root", "out"), Endpoint("Seq", "in")),), path="/Body/Tree")
        self.assertEqual(graph.node("Wait").guid, "AB" * 16)
        self.assertEqual(graph.node_path("Wait"), "/Body/Tree/Nodes/2")
        self.assertEqual(list(graph.node("Seq").properties.items()), [("template", "bIgnoreRestart", True)])
        with self.assertRaises(FrozenInstanceError):
            graph.name = "x"
        with self.assertRaises(TypeError):
            graph.node("Seq").properties.template["bIgnoreRestart"] = False

    def test_validation_codes(self):
        node = NodeDoc("A", action="K2:Self")
        cases = (
            (lambda: NodeDoc("1bad", action="K2:Self"), "Graph.InvalidNodeId"),
            (lambda: NodeDoc("A"), "Graph.NodeActionMissing"),
            (lambda: NodeDoc("A", implicit="Entry"), "Graph.InvalidImplicitRole"),
            (lambda: PinOverride("P", value="a", object="b"), "Graph.InvalidPinOverride"),
            (lambda: PinType("int", container="list"), "Graph.InvalidPinType"),
            (lambda: Position(float("nan"), 0), "Graph.InvalidPosition"),
            (lambda: Signature((UserPin("A", PinType("int")), UserPin("a", PinType("int")))), "Graph.DuplicatePin"),
            (lambda: GraphDoc("G", "event", "G", "K2"), "Graph.InvalidRole"),
            (lambda: GraphDoc("G", "ubergraph", "G", "K2", nodes=(node, node)), "Graph.DuplicateNodeId"),
            (lambda: GraphDoc("G", "ubergraph", "G", "K2", nodes=(NodeDoc("A", implicit="entry"),
                                                                    NodeDoc("B", implicit="entry"))),
             "Graph.DuplicateImplicitNode"),
            (lambda: GraphDoc("G", "ubergraph", "G", "K2", nodes=(NodeDoc("A", action="K2:Self", parent="B"),
                                                                    NodeDoc("B", action="K2:Self", parent="A"))),
             "Graph.ParentCycle"),
            (lambda: GraphDoc("G", "ubergraph", "G", "K2", nodes=(node,),
                              links=(LinkDoc(Endpoint("A", "x"), Endpoint("A", "y")),) * 2), "Graph.DuplicateLink"),
            (lambda: GraphDoc("G", "ubergraph", "G", "K2", subgraphs=(GraphDoc("S", "other", "S", "K2", owner="Z"),)),
             "Graph.UnknownNode"),
        )
        for call, code in cases:
            with self.subTest(code=code):
                self.assertEqual(code_of(self, call), code)


    def test_a_function_may_hold_several_return_nodes(self):
        graph = GraphDoc("G", "function", "G", "K2", nodes=(NodeDoc("Result", implicit="result"),
                                                           NodeDoc("Result_2", implicit="result")))
        self.assertEqual([node.implicit for node in graph.nodes], ["result", "result"])


class LiveDecodeTests(unittest.TestCase):
    def test_query_fixture_decodes_contract_shapes(self):
        live = live_asset("graph.query.v1.k2_branch.json")
        event = live.graph(G_EVENT)
        self.assertEqual((event.name, event.role, event.schema_tag, event.can("remove")), ("EventGraph", "ubergraph", "K2", False))
        branch = event.node(N_BRANCH)
        self.assertEqual((branch.action_key, branch.position, branch.can_add_pin), ("K2:IfThenElse", Position(320, 0), False))
        then = branch.find_pin("Then", "output")
        self.assertEqual((then.name, then.type.category, [(r.node_guid, r.pin_name) for r in then.linked_to]),
                         ("then", "exec", [(N_PRINT, "execute")]))
        self.assertEqual([(l.from_.node_guid, l.to.node_guid) for l in event.links], [(N_BEGIN, N_BRANCH), (N_BRANCH, N_PRINT)])

    def test_real_provider_fields_are_decoded_or_tolerated(self):
        live = live_asset("graph.query.v1.k2_function_signature.json", "graph.list.v1.k2_function.json")
        self.assertFalse(live.graph(G_EVENT).can("rename"))
        add_health = live.graph("A0000000000000000000000000000003")
        self.assertTrue(add_health.can("rename"))
        entry, result = add_health.nodes
        self.assertEqual((entry.action_key, entry.implicit_role, result.action_key, result.implicit_role),
                         (None, "entry", "K2:FunctionResult", "result"))
        self.assertEqual(entry.extras["user_pins"], ("Amount",))
        self.assertEqual(entry.extras["signature_flags"]["access"], "public")
        tick = live.graph(G_EVENT).node("B0000000000000000000000000000003")
        self.assertEqual((tick.enabled_state, tick.extras["ghost"], tick.position), ("disabled", True, Position(0, 416)))
        printer = live_asset("graph.query.v1.k2_branch.json").graph(G_EVENT).node(N_PRINT)
        self.assertEqual(printer.enabled_state, "development_only")
        self.assertTrue(printer.find_pin("Key", "input").type.is_const)

    def test_query_pages_are_followed_until_next_offset_is_null(self):
        value = fixture("graph.query.v1.k2_branch.json")
        whole = decode_query(value)
        event = value["graphs"][0]

        def page(offset):
            nodes = event["nodes"][offset:offset + 2]
            guids = {node["node_guid"] for node in nodes}
            following = offset + 2 if offset + 2 < len(event["nodes"]) else None
            return {**event, "nodes": nodes, "next_node_offset": following,
                    "links": [link for link in event["links"] if link["from"]["node_guid"] in guids]}
        calls = []

        def query(values):
            calls.append(values)
            if "node_offset" in values:
                return {"schema_version": 1, "graphs": [page(values["node_offset"])], "truncated": True}
            return {"schema_version": 1, "graphs": [page(0), value["graphs"][1]], "truncated": True}
        self.assertEqual(query_graphs(query), whole)
        self.assertEqual([item.get("node_offset") for item in calls], [None, 2, 4])

        def stuck(values):
            return {"schema_version": 1, "graphs": [{**page(0), "next_node_offset": 0}], "truncated": True}
        self.assertEqual(code_of(self, lambda: query_graphs(stuck)), "Graph.InvalidSnapshot")

    def test_large_graph_lists_are_paged_by_offset(self):
        entry = fixture("graph.list.v1.k2_function.json")["graphs"][0]
        rows = [{**entry, "graph_guid": f"{index:032X}", "name": f"Fn{index}"} for index in range(7)]
        calls = []

        def invoke(values):
            calls.append(values)
            if "offset" not in values:
                raise contract_error("/Body", "Graph.ResultTooLarge", "too large")
            start = values["offset"]
            following = start + 3 if start + 3 < len(rows) else None
            return {"schema_version": 1, "graphs": rows[start:start + 3], "total": len(rows), "next_offset": following}
        self.assertEqual([info.name for info in list_graphs(invoke)], [f"Fn{index}" for index in range(7)])
        self.assertEqual(calls, [{}, {"offset": 0}, {"offset": 3}, {"offset": 6}])
        # A small list stays one call; other errors and stuck pagination are reported.
        self.assertEqual(len(list_graphs(lambda values: {"schema_version": 1, "graphs": rows[:2]})), 2)

        def refused(values):
            raise contract_error("/Body", "Graph.PieActive", "pie")
        self.assertEqual(code_of(self, lambda: list_graphs(refused)), "Graph.PieActive")

        def stuck(values):
            if "offset" not in values:
                raise contract_error("/Body", "Graph.ResultTooLarge", "too large")
            return {"schema_version": 1, "graphs": rows[:1], "next_offset": values["offset"]}
        self.assertEqual(code_of(self, lambda: list_graphs(stuck)), "Graph.InvalidSnapshot")

    def test_many_graphs_are_queried_in_chunks(self):
        listing = fixture("graph.list.v1.k2_function.json")["graphs"][0]
        snapshot = {**fixture("graph.query.v1.k2_branch.json")["graphs"][0], "nodes": [], "links": [],
                    "node_count": 0, "next_node_offset": None}
        count = QUERY_GRAPH_CHUNK * 2 + 5
        guids = [f"{index:032X}" for index in range(count)]
        infos = decode_graph_list({"schema_version": 1, "graphs": [
            {**listing, "graph_guid": guid, "name": f"Fn{index}"} for index, guid in enumerate(guids)]})
        calls = []

        def query(values):
            calls.append(values)
            if "graphs" not in values:
                raise contract_error("/Body", "Graph.ResultTooLarge", "too large")
            return {"schema_version": 1, "truncated": False, "graphs": [
                {**snapshot, "graph_guid": address["graph_guid"]} for address in values["graphs"]]}
        graphs = query_graphs(query, infos=infos)
        self.assertEqual([graph.guid for graph in graphs], guids)
        self.assertEqual([graph.name for graph in graphs][-1], f"Fn{count - 1}")
        self.assertEqual([len(call["graphs"]) for call in calls], [QUERY_GRAPH_CHUNK, QUERY_GRAPH_CHUNK, 5])

    def test_every_fixture_decodes(self):
        decode_graph_list(fixture("graph.list.v1.k2_function.json"))
        decode_query(fixture("graph.query.v1.k2_function_signature.json"))
        self.assertEqual(decode_node(fixture("graph.node.spawn.v1.k2_branch.json")["node"], "/node").guid, N_BRANCH)

    def test_lowercase_guids_normalize_and_bad_snapshots_fail(self):
        value = fixture("graph.query.v1.k2_empty_actor.json")
        value["graphs"][0]["graph_guid"] = value["graphs"][0]["graph_guid"].lower()
        self.assertEqual(decode_query(value)[0].guid, G_EVENT)
        del value["graphs"][0]["nodes"][0]["node_guid"]
        self.assertEqual(code_of(self, lambda: decode_query(value)), "Graph.InvalidSnapshot")
        self.assertEqual(code_of(self, lambda: decode_live_asset({"schema_version": 2, "graphs": []}, {"graphs": []})),
                         "Graph.InvalidSnapshot")


if __name__ == "__main__":
    unittest.main()
