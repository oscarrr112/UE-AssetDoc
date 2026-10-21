"""K2 node state the editor sets after placing a node: class picks, Pin Conversions of promotable
operators, purity, function local variables and collapsed graphs (reconcile / codec / extract level)."""
from __future__ import annotations

from copy import deepcopy
import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.graphs.extract import extract_graph
from asset_document.graphs.live import decode_live_asset
from asset_document.graphs.model import LocalVariable, PinType
from asset_document.graphs.schemas import k2
from asset_document.tests.graph_support import ACTOR_CLASS, N_ENTRY, N_RESULT, fixture, ok, pin, ptype
from asset_document.tests.test_graph_reconcile import SIGNATURE, plan_for


FLOAT = ptype("real", "double")
VECTOR = ptype("struct", "", "/Script/CoreUObject.Vector")
MULTIPLY = "K2:PromotableOperator:Multiply"


def live_node(guid, action, pins, extras=None, cls="/Script/BlueprintGraph.K2Node_PromotableOperator"):
    return {"node_guid": guid, "class": cls, "action_key": action, "title": action, "implicit_role": None,
            "position": {"x": 0, "y": 0}, "comment": "", "enabled_state": "enabled", "parent_node_guid": None,
            "subnode_index": None, "subgraph_guids": [], "can_add_pin": True, "extras": extras or {}, "pins": pins}


def multiply(guid, a, b, result, wired=False):
    return live_node(guid, MULTIPLY, [pin("A", "input", a), pin("B", "input", b), pin("ReturnValue", "output", result)],
                     {"typed_pins": ["A", "B", "ReturnValue"], "promotable": True, "wired_to_operator": wired})


def add_nodes(*nodes, links=()):
    def edit(value):
        graph = value["graphs"][0]
        graph["nodes"].extend(deepcopy(nodes))
        graph["links"].extend({"from": {"node_guid": a, "pin_name": ap}, "to": {"node_guid": b, "pin_name": bp}}
                              for a, ap, b, bp in links)
        for a, ap, b, bp in links:
            for node in graph["nodes"]:
                for item in node["pins"]:
                    if (node["node_guid"], item["name"]) == (a, ap):
                        item["linked_to"].append({"node_guid": b, "pin_name": bp})
                    if (node["node_guid"], item["name"]) == (b, bp):
                        item["linked_to"].append({"node_guid": a, "pin_name": ap})
    return edit


def live_graph(edit, query="graph.query.v1.k2_empty_actor.json", listing="graph.list.v1.k2_empty_actor.json"):
    value = deepcopy(fixture(query))
    edit(value)
    asset = decode_live_asset(fixture(listing), value)
    return asset, asset.graphs[0]


class ClassPickTests(unittest.TestCase):
    def test_class_pick_is_set_before_links_and_again_after(self):
        # Add Component by Class: the picked Class types ReturnValue, which the link into a typed target needs.
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Add", "Action": "K2:AddComponentByClass",
             "PinOverrides": [{"Pin": "Class", "DefaultObject": "/Script/Harmonix.MusicClockComponent"}]},
            {"Id": "Start", "Action": "K2:CallFunction:/Script/Harmonix.MusicClockComponent:Start"}],
            "Links": [{"From": "Add:ReturnValue", "To": "Start:self"}]}]}
        ops = [(op.op, op.args.get("optional", False)) for op in plan_for(body).ops if op.op.startswith("graph.pin")]
        self.assertEqual(ops, [("graph.pin.default.set", True), ("graph.pin.connect", False),
                               ("graph.pin.default.set", False)])

    def test_value_defaults_still_follow_the_links(self):
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Print", "Action": "K2:PrintString", "PinOverrides": [{"Pin": "InString", "DefaultValue": "Hi"}]}],
            "Links": []}]}
        ops = [op for op in plan_for(body).ops if op.op == "graph.pin.default.set"]
        self.assertEqual([op.args.get("optional", False) for op in ops], [False])


class PromotableOperatorTests(unittest.TestCase):
    def test_only_a_mixed_operator_states_its_pin_types(self):
        _, graph = live_graph(add_nodes(multiply("B" + "1" * 31, VECTOR, FLOAT, VECTOR),
                                        multiply("B" + "2" * 31, FLOAT, FLOAT, FLOAT)))
        mixed, uniform = (graph.node("B" + "1" * 31), graph.node("B" + "2" * 31))
        self.assertEqual([name for name, _ in k2.K2_ADAPTER.node_pin_types(mixed)], ["A", "B", "ReturnValue"])
        self.assertEqual(k2.K2_ADAPTER.node_pin_types(uniform), ())
        self.assertEqual(len(k2.K2_ADAPTER.live_pin_types(uniform)), 3)

    def test_an_operator_wired_to_an_operator_states_its_pin_types(self):
        _, graph = live_graph(add_nodes(multiply("B" + "1" * 31, FLOAT, FLOAT, FLOAT, wired=True)))
        self.assertEqual([name for name, _ in k2.K2_ADAPTER.node_pin_types(graph.node("B" + "1" * 31))],
                         ["A", "B", "ReturnValue"])

    def test_pin_types_of_a_spawned_operator_come_before_its_links(self):
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Multiply", "Action": "K2:Multiply", "PinTypes": {
                "A": "struct:/Script/CoreUObject.Vector", "B": "float", "ReturnValue": "struct:/Script/CoreUObject.Vector"}},
            {"Id": "Get_V", "Action": "K2:Get:V"}],
            "Links": [{"From": "Get_V:V", "To": "Multiply:A"}]}]}
        ops = [op.op for op in plan_for(body).ops if op.op in ("graph.node.pin_type.set", "graph.pin.connect")]
        self.assertEqual(ops, ["graph.node.pin_type.set"] * 3 + ["graph.pin.connect"])

    def test_links_of_an_operator_with_stated_types_keep_them(self):
        # Each link re-picks a promotable operator's function from everything linked so far; a later link
        # would undo a Pin Conversion (Multiply's float B next to a vector A read back as a vector:
        # FS_BombField_Prototype). Links at an end whose types the document states ask to keep them.
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Multiply", "Action": "K2:Multiply", "PinTypes": {
                "A": "struct:/Script/CoreUObject.Vector", "B": "double", "ReturnValue": "struct:/Script/CoreUObject.Vector"}},
            {"Id": "Add", "Action": "K2:Add"},
            {"Id": "Get_V", "Action": "K2:Get:V"}, {"Id": "Get_F", "Action": "K2:Get:F"}],
            "Links": [{"From": "Get_V:V", "To": "Multiply:A"}, {"From": "Get_F:F", "To": "Multiply:B"},
                      {"From": "Multiply:ReturnValue", "To": "Add:A"}]}]}
        connects = {(op.args["from"]["node"], op.args["to"]["node"]):
                    (op.args.get("keep_from_types"), op.args.get("keep_to_types"))
                    for op in plan_for(body).ops if op.op == "graph.pin.connect"}
        vector, double = (k2.decode_pin_type(text, "/").to_dto() for text in ("struct:/Script/CoreUObject.Vector", "double"))
        stated = {"A": vector, "B": double, "ReturnValue": vector}
        self.assertEqual(connects, {("Get_V", "Multiply"): (None, stated), ("Get_F", "Multiply"): (None, stated),
                                    ("Multiply", "Add"): (stated, None)})

    def test_a_default_on_a_typed_extra_pin_is_set_before_the_links(self):
        # Links re-evaluate the operator, which conforms an undetermined extra pin to its function (vector);
        # the default makes the float C pin determined, as when the author typed it before wiring.
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Multiply", "Action": "K2:Multiply", "PinOverrides": [{"Pin": "C", "DefaultValue": "0.5"}],
             "PinTypes": {"A": "struct:/Script/CoreUObject.Vector", "B": "float", "C": "float",
                          "ReturnValue": "struct:/Script/CoreUObject.Vector"}},
            {"Id": "Get_V", "Action": "K2:Get:V"}],
            "Links": [{"From": "Get_V:V", "To": "Multiply:A"}]}]}
        ops = [op.op for op in plan_for(body).ops
               if op.op in ("graph.node.pin_type.set", "graph.pin.connect", "graph.pin.default.set")]
        self.assertEqual(ops, ["graph.node.pin_type.set"] * 4 + ["graph.pin.default.set", "graph.pin.connect",
                                                                 "graph.pin.default.set"])

    def test_a_converted_live_operator_is_kept_and_its_links_made_again(self):
        guid, source = "B" + "1" * 31, "B" + "3" * 31
        getter = live_node(source, "K2:VariableGet:/Game/BP_Test.BP_Test_C:V", [pin("V", "output", VECTOR)],
                           cls="/Script/BlueprintGraph.K2Node_VariableGet")
        edit = add_nodes(multiply(guid, VECTOR, VECTOR, VECTOR), getter, links=[(source, "V", guid, "A")])
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Multiply", "Action": "K2:Multiply", "NodeGuid": guid, "PinTypes": {
                "A": "struct:/Script/CoreUObject.Vector", "B": "float", "ReturnValue": "struct:/Script/CoreUObject.Vector"}},
            {"Id": "Get_V", "Action": "K2:Get:V", "NodeGuid": source}],
            "Links": [{"From": "Get_V:V", "To": "Multiply:A"}]}]}
        plan = plan_for(body, edit=edit)
        ops = [(op.op, op.args.get("pin")) for op in plan.ops
               if op.op in ("graph.node.pin_type.set", "graph.pin.connect", "graph.node.remove", "graph.node.spawn")]
        self.assertEqual(ops, [("graph.node.pin_type.set", "B"), ("graph.pin.connect", None)])


class CreateDelegateTests(unittest.TestCase):
    def test_the_function_is_picked_once_the_delegate_is_linked(self):
        # An unlinked Create Event clears its function (AnimatedMannyPawnExtended: "missing a function/event
        # name" when it was set before the link).
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "CreateDelegate", "Action": "K2:CreateDelegate", "Properties": {"Function": "OnStance"}},
            {"Id": "Bind", "Action": "K2:AddDelegate:/Script/Engine.Actor:OnDestroyed"}],
            "Links": [{"From": "CreateDelegate:OutputDelegate", "To": "Bind:Delegate"}]}]}
        ops = [(op.op, op.args.get("property_path")) for op in plan_for(body).ops
               if op.op in ("graph.node.property.set", "graph.pin.connect")]
        self.assertEqual(ops, [("graph.pin.connect", None), ("graph.node.property.set", "Function")])


class NodeIdTests(unittest.TestCase):
    def test_a_custom_event_keeps_its_name_as_id_wherever_it_sits(self):
        # A Create places called custom events before the rest of their graph; a comment titled like the
        # event must not take its id in one order and leave it in the other (WaveFoamRenderer).
        comment = live_node("C" + "1" * 31, "K2:EdGraphNode_Comment", [], {"name": "Initialize"},
                            cls="/Script/UnrealEd.EdGraphNode_Comment")
        comment["comment"] = "Initialize"
        event = live_node("C" + "2" * 31, "K2:CustomEvent", [pin("then", "output", ptype("exec"))],
                          {"name": "Initialize"}, cls="/Script/BlueprintGraph.K2Node_CustomEvent")
        for order in ((comment, event), (event, comment)):
            asset, graph = live_graph(add_nodes(*order))
            doc = extract_graph(graph, asset, graph_id="U", path="/Body/UbergraphPages/0", adapter=k2.K2_ADAPTER)
            ids = {node.guid: node.id for node in doc.nodes}
            self.assertEqual((ids[event["node_guid"]], ids[comment["node_guid"]]), ("Initialize", "Initialize_2"))


class PurityTests(unittest.TestCase):
    def node(self, pure, default=True):
        _, graph = live_graph(add_nodes(live_node(
            "B" + "4" * 31, "K2:VariableGet:/Game/BP_Test.BP_Test_C:Clock",
            [pin("execute", "input", ptype("exec")), pin("Clock", "output", ptype("object", "", "/Script/Engine.Actor"))],
            {"properties": {"Pure": pure}, "property_defaults": {"Pure": default}},
            cls="/Script/BlueprintGraph.K2Node_VariableGet")))
        return graph.node("B" + "4" * 31)

    def test_only_a_toggled_purity_is_extracted(self):
        self.assertEqual(dict(k2.K2_ADAPTER.extract_properties(self.node(False), None).node), {"Pure": False})
        self.assertEqual(dict(k2.K2_ADAPTER.extract_properties(self.node(True), None).node), {})

    def test_purity_is_compared_against_the_node_setting(self):
        self.assertEqual(k2.K2_ADAPTER.live_properties(self.node(False), "node")["Pure"], False)

    def test_stated_purity_is_set_before_links(self):
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "BeginPlay", "Action": "K2:BeginPlay"},
            {"Id": "Get_Clock", "Action": "K2:Get:Clock", "Properties": {"Pure": False}}],
            "Links": [{"From": "BeginPlay:then", "To": "Get_Clock:execute"}]}]}
        ops = [op.op for op in plan_for(body).ops if op.op in ("graph.node.property.set", "graph.pin.connect")]
        self.assertEqual(ops, ["graph.node.property.set", "graph.pin.connect"])


class LocalVariableTests(unittest.TestCase):
    BODY = {"FunctionGraphs": [{"Name": "AddHealth", "LocalVariables": [
        {"Name": "Count", "Type": "int", "Default": "3"}, {"Name": "Label", "Type": "string", "Tooltip": "Shown"}],
        "Links": [{"From": "Entry:then", "To": "Result:execute"}]}]}

    def test_codec_round_trip(self):
        graphs, _ = k2.decode_body(self.BODY, self_class=ACTOR_CLASS)
        self.assertEqual(graphs[0].local_variables, (
            LocalVariable("Count", PinType("int"), "3"), LocalVariable("Label", PinType("string"), tooltip="Shown")))
        self.assertEqual(k2.encode_graph(graphs[0])["LocalVariables"], self.BODY["FunctionGraphs"][0]["LocalVariables"])

    def test_duplicate_names_are_refused(self):
        body = {"FunctionGraphs": [{"Name": "F", "LocalVariables": [{"Name": "A", "Type": "int"}, {"Name": "a", "Type": "int"}]}]}
        with self.assertRaises(AssetDocumentContractError) as caught:
            k2.decode_body(body, self_class=ACTOR_CLASS)
        self.assertEqual(caught.exception.diagnostic.code, "Graph.DuplicateLocalVariable")

    def test_locals_are_set_after_the_signature_and_before_nodes(self):
        plan = plan_for(self.BODY, *SIGNATURE)
        ops = [op.op for op in plan.ops]
        self.assertIn("graph.local_variables.set", ops)
        variables = next(op for op in plan.ops if op.op == "graph.local_variables.set").args["variables"]
        self.assertEqual([item["name"] for item in variables], ["Count", "Label"])
        self.assertEqual(variables[0]["type"]["category"], "int")

    def test_equal_live_locals_plan_nothing_and_extract_back(self):
        def edit(value):
            entry = next(node for graph in value["graphs"] for node in graph["nodes"] if node["node_guid"] == N_ENTRY)
            entry["extras"]["local_variables"] = [
                {"name": "Count", "type": ptype("int"), "default": "3", "category": "", "tooltip": ""},
                {"name": "Label", "type": ptype("string"), "default": "", "category": "", "tooltip": "Shown"}]
        plan = plan_for(self.BODY, *SIGNATURE, edit=edit)
        self.assertNotIn("graph.local_variables.set", [op.op for op in plan.ops])
        value = deepcopy(fixture(SIGNATURE[0]))
        edit(value)
        asset = decode_live_asset(fixture(SIGNATURE[1]), value)
        graph = next(item for item in asset.graphs if item.node(N_ENTRY) is not None)
        doc = extract_graph(graph, asset, graph_id="F", path="/Body/FunctionGraphs/0", adapter=k2.K2_ADAPTER)
        self.assertEqual([item.name for item in doc.local_variables], ["Count", "Label"])

    def test_map_locals_carry_their_value_type(self):
        rows = [{"Name": "TimeToTempo", "Type": {"Category": "int", "Container": "map", "ValueType": "float"},
                 "Default": "((8, 120.000000))"}]
        body = {"FunctionGraphs": [{"Name": "AddHealth", "LocalVariables": rows,
                                    "Links": [{"From": "Entry:then", "To": "Result:execute"}]}]}
        graphs, _ = k2.decode_body(body, self_class=ACTOR_CLASS)
        self.assertEqual(k2.encode_graph(graphs[0])["LocalVariables"], rows)
        variables = next(op for op in plan_for(body, *SIGNATURE).ops if op.op == "graph.local_variables.set").args["variables"]
        self.assertEqual(variables[0]["type"]["container"], "map")
        self.assertEqual(variables[0]["type"]["value_type"]["category"], "real")

        def edit(value):
            entry = next(node for graph in value["graphs"] for node in graph["nodes"] if node["node_guid"] == N_ENTRY)
            entry["extras"]["local_variables"] = [{"name": "TimeToTempo", "type": variables[0]["type"],
                                                   "default": "((8, 120.000000))", "category": "", "tooltip": ""}]
        self.assertNotIn("graph.local_variables.set", [op.op for op in plan_for(body, *SIGNATURE, edit=edit).ops])

    def test_map_locals_need_a_value_type(self):
        body = {"FunctionGraphs": [{"Name": "F", "LocalVariables": [{"Name": "M", "Type": {"Category": "int", "Container": "map"}}]}]}
        with self.assertRaises(AssetDocumentContractError) as caught:
            k2.decode_body(body, self_class=ACTOR_CLASS)
        self.assertEqual(caught.exception.diagnostic.code, "Graph.InvalidLocalVariable")

    def test_omitted_locals_are_kept(self):
        body = {"FunctionGraphs": [{"Name": "AddHealth", "Links": [{"From": "Entry:then", "To": "Result:execute"}]}]}
        self.assertNotIn("graph.local_variables.set", [op.op for op in plan_for(body, *SIGNATURE).ops])


class ReturnNodeTests(unittest.TestCase):
    def spawns(self, body):
        return [op.args["action_key"] for op in plan_for(body).ops
                if op.op == "graph.node.spawn" and op.args["node"] == "Result"]

    def test_a_return_node_without_outputs_is_placed(self):
        body = {"FunctionGraphs": [{"Name": "Flush", "Links": [{"From": "Entry:then", "To": "Result:execute"}]}]}
        self.assertEqual(self.spawns(body), ["K2:FunctionResult"])
        spawn = next(op for op in plan_for(body).ops if op.op == "graph.node.spawn")
        self.assertEqual(spawn.args["adopt_implicit"], "result")  # unless the new graph came with one

    def _spawn_result(self, *, graph_has_result: bool):
        from asset_document.graphs.executor import GraphExecutor
        from asset_document.graphs.reconcile import PlanOp
        from asset_document.tests.graph_support import FakeGraphContext
        context = FakeGraphContext(SIGNATURE[1], SIGNATURE[0])
        guid = "A0000000000000000000000000000003"  # AddHealth
        if not graph_has_result:
            graph = context.graphs[guid]
            graph["nodes"] = [node for node in graph["nodes"] if node.get("implicit_role") != "result"]
            graph["links"] = [link for link in graph["links"] if N_RESULT not in (link[0], link[2])]
        executor = GraphExecutor(context, read_handle=object(), write_handle=object(), route_id="route.blueprint.v1",
                                 adapter=k2.K2_ADAPTER, self_class=ACTOR_CLASS)
        executor.graphs["FunctionGraphs/AddHealth"] = guid
        executor.step(PlanOp("graph.node.spawn", "FunctionGraphs/AddHealth", "/Body/FunctionGraphs/0/Nodes/1",
                             {"node": "Result", "action_key": "K2:FunctionResult", "adopt_implicit": "result"}))
        return context, executor.nodes[("FunctionGraphs/AddHealth", "Result")]

    def test_a_return_node_the_new_graph_came_with_is_taken(self):
        # An override of a parent function with outputs: the created graph already has its return node.
        context, guid = self._spawn_result(graph_has_result=True)
        self.assertEqual(guid, N_RESULT)
        self.assertEqual(context.calls_of("graph.node.spawn.v1"), [])

    def test_a_return_node_the_new_graph_lacks_is_spawned(self):
        context, guid = self._spawn_result(graph_has_result=False)
        self.assertNotEqual(guid, N_RESULT)
        self.assertEqual(len(context.calls_of("graph.node.spawn.v1")), 1)

    def test_a_return_node_with_outputs_comes_with_the_signature(self):
        body = {"FunctionGraphs": [{"Name": "Flush", "Signature": {"Outputs": [{"Name": "Done", "Type": "bool"}]},
                                    "Links": [{"From": "Entry:then", "To": "Result:execute"}]}]}
        self.assertEqual(self.spawns(body), [])


class CollapsedGraphTests(unittest.TestCase):
    def test_collapsed_graph_is_built_before_the_links_to_its_pins(self):
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "BeginPlay", "Action": "K2:BeginPlay"},
            {"Id": "Setup", "Action": "K2:Composite", "Name": "Setup"}],
            "Links": [{"From": "BeginPlay:then", "To": "Setup:execute"}],
            "Subgraphs": [{"Name": "Setup", "Owner": "Setup",
                           "Signature": {"Inputs": [{"Name": "execute", "Type": "exec"}]},
                           "Links": [], "Nodes": []}]}]}
        ops = [op.op for op in plan_for(body).ops]
        self.assertLess(ops.index("graph.node.rename"), ops.index("graph.subgraph.sync"))
        self.assertLess(ops.index("graph.subgraph.sync"), ops.index("graph.pin.connect"))

    def test_collapsed_nodes_are_named(self):
        self.assertIn("Composite", k2.NAMED_KINDS)
        self.assertTrue(k2.K2_ADAPTER.owned_subgraphs_follow_owner)


if __name__ == "__main__":
    unittest.main()


class NodeStateExecutorTests(unittest.TestCase):
    def test_local_variables_are_set_on_the_entry_node(self):
        from asset_document.tests.graph_support import FakeGraphContext
        from asset_document.tests.test_graph_executor import execute
        context = FakeGraphContext(SIGNATURE[1], SIGNATURE[0])
        execute(context, LocalVariableTests.BODY)
        call = next(args for name, args in context.calls if name == "graph.node.property.set.v1")
        self.assertEqual((call["node_guid"], call["container"], call["property_path"]), (N_ENTRY, "node", "LocalVariables"))
        self.assertEqual([item["name"] for item in call["value"]], ["Count", "Label"])

    def test_a_link_to_a_wildcard_pin_leaves_the_snapshot_stale_until_needed(self):
        # The wildcard end takes the link's type; the graph is re-read when an op needs types or
        # defaults (here the default after the links), not after every link of a large graph.
        from asset_document.tests.graph_support import RANDOM_BOOL, FakeGraphContext
        from asset_document.tests.test_graph_executor import execute
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
        execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Select", "Action": "K2:Select", "PinOverrides": [{"Pin": "Index", "DefaultValue": "1"}]},
            {"Id": "Random", "Action": RANDOM_BOOL}],
            "Links": [{"From": "Random:ReturnValue", "To": "Select:Option 0"},
                      {"From": "Random:ReturnValue", "To": "Select:Option 1"}]}]})
        names = [name for name, _ in context.calls]
        first = names.index("graph.pin.connect.v1")
        second = names.index("graph.pin.connect.v1", first + 1)
        self.assertNotIn("graph.query.v1", names[first + 1:second])
        self.assertIn("graph.query.v1", names[second + 1:names.index("graph.pin.default.set.v1")])

    def test_a_link_to_a_member_of_a_wildcard_pin_waits_for_the_link_that_types_it(self):
        # ForEachLoop "Array Element_StartMs" listed before the Array link: made after it.
        from asset_document.tests.graph_support import PRINT, RANDOM_BOOL, FakeGraphContext
        from asset_document.tests.test_graph_executor import execute
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
        connect = context._graph_pin_connect_v1

        def typed_connect(args):
            for ref in (args["from"], args["to"]):
                _, node, found = context._pin(ref)
                if found is None and node is not None and ref["pin_name"] == "ReturnValue_X":
                    node["pins"].append(pin("ReturnValue_X", "output", ptype("string")))  # split on connect
            result = connect(args)
            _, select, _ = context._pin(args["to"])
            if args["to"]["pin_name"] == "Option 0":
                next(p for p in select["pins"] if p["name"] == "ReturnValue")["type"] = ptype(
                    "struct", "", "/Script/CoreUObject.Vector")
            return result
        context._graph_pin_connect_v1 = typed_connect
        execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Select", "Action": "K2:Select"}, {"Id": "Random", "Action": RANDOM_BOOL},
            {"Id": "Print", "Action": PRINT}],
            "Links": [{"From": "Select:ReturnValue_X", "To": "Print:InString"},
                      {"From": "Random:ReturnValue", "To": "Select:Option 0"}]}]})
        connects = [(args["from"]["pin_name"], args["to"]["pin_name"]) for name, args in context.calls
                    if name == "graph.pin.connect.v1"]
        self.assertEqual(connects, [("ReturnValue", "Option 0"), ("ReturnValue_X", "InString")])

    def test_a_member_of_a_wildcard_pin_that_never_gets_a_type_fails(self):
        from asset_document.tests.graph_support import PRINT, FakeGraphContext
        from asset_document.tests.test_graph_executor import execute
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
        with self.assertRaises(AssetDocumentContractError) as caught:
            execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
                {"Id": "Select", "Action": "K2:Select"}, {"Id": "Print", "Action": PRINT}],
                "Links": [{"From": "Select:ReturnValue_X", "To": "Print:InString"}]}]})
        self.assertEqual(caught.exception.diagnostic.code, "Graph.PinNotFound")

    def test_a_default_on_a_member_of_an_unsplit_struct_pin_is_written(self):
        from asset_document.tests.graph_support import FakeGraphContext, TEMPLATES
        from asset_document.tests.test_graph_executor import execute
        key = "K2:CallFunction:/Script/Engine.SceneComponent:K2_SetRelativeLocation"
        TEMPLATES[key] = {"action_key": key, "implicit_role": None, "can_add_pin": False,
                          "pins": [pin("NewLocation", "input", ptype("struct", "", "/Script/CoreUObject.Vector"))],
                          "extras": {}}
        try:
            context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
            written = []
            context._graph_pin_default_set_v1 = lambda args: (written.append(args), ok({"schema_version": 1}))[1]
            execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
                {"Id": "Move", "Action": key, "PinOverrides": [{"Pin": "NewLocation_Z", "DefaultValue": "-50.0"},
                                                                 # split before a case-only rename of the pin
                                                                 {"Pin": "newLocation_Y", "DefaultValue": "2.0"}]}],
                "Links": []}]})
            self.assertEqual(sorted((args["pin"]["pin_name"], args["value"]) for args in written),
                             [("NewLocation_Z", "-50.0"), ("newLocation_Y", "2.0")])
        finally:
            del TEMPLATES[key]

    def test_a_wildcard_into_an_operator_waits_for_its_own_source(self):
        # Reroute -> Multiply listed before Random -> Reroute: the reroute is wired to its source first.
        from asset_document.tests.graph_support import RANDOM_BOOL, FakeGraphContext, TEMPLATES
        from asset_document.tests.test_graph_executor import execute
        TEMPLATES[MULTIPLY] = {"action_key": MULTIPLY, "implicit_role": None, "can_add_pin": True,
                               "pins": [pin("A", "input", ptype("wildcard")), pin("B", "input", ptype("wildcard")),
                                        pin("ReturnValue", "output", ptype("wildcard"))],
                               "extras": {"typed_pins": ["A", "B", "ReturnValue"], "promotable": True}}
        TEMPLATES["K2:Knot"] = {"action_key": "K2:Knot", "implicit_role": None, "can_add_pin": False,
                                "pins": [pin("InputPin", "input", ptype("wildcard")),
                                         pin("OutputPin", "output", ptype("wildcard"))], "extras": {}}
        try:
            context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
            execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
                {"Id": "Multiply", "Action": MULTIPLY}, {"Id": "Reroute", "Action": "K2:Knot"},
                {"Id": "Random", "Action": RANDOM_BOOL}],
                "Links": [{"From": "Reroute:OutputPin", "To": "Multiply:A"},
                          {"From": "Random:ReturnValue", "To": "Reroute:InputPin"}]}]})
            connects = [(args["from"]["pin_name"], args["to"]["pin_name"]) for name, args in context.calls
                        if name == "graph.pin.connect.v1"]
            self.assertEqual(connects, [("ReturnValue", "InputPin"), ("OutputPin", "A")])
        finally:
            del TEMPLATES[MULTIPLY], TEMPLATES["K2:Knot"]

    def test_a_link_between_two_wildcards_waits_for_the_link_that_types_one(self):
        # Select -> Select listed before Random -> the first Select: a link between two wildcards types
        # neither (the editor does not carry a type along a chain later), so the first is wired first.
        from asset_document.tests.graph_support import RANDOM_BOOL, FakeGraphContext
        from asset_document.tests.test_graph_executor import execute
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
        connect = context._graph_pin_connect_v1

        def typed_connect(args):
            result = connect(args)
            for ref in (args["from"], args["to"]):
                _, node, found = context._pin(ref)
                if found is not None and found["type"]["category"] == "wildcard" and node is not None:
                    for item in node["pins"]:
                        if item["type"]["category"] == "wildcard" and item["name"] != "Index":
                            item["type"] = ptype("bool")
            return result
        context._graph_pin_connect_v1 = typed_connect
        execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Outer", "Action": "K2:Select"}, {"Id": "Inner", "Action": "K2:Select"},
            {"Id": "Random", "Action": RANDOM_BOOL}],
            "Links": [{"From": "Inner:ReturnValue", "To": "Outer:Option 0"},
                      {"From": "Random:ReturnValue", "To": "Inner:Option 0"}]}]})
        connects = [(args["from"]["pin_name"], args["to"]["pin_name"]) for name, args in context.calls
                    if name == "graph.pin.connect.v1"]
        self.assertEqual(connects, [("ReturnValue", "Option 0"), ("ReturnValue", "Option 0")])
        first, second = [args for name, args in context.calls if name == "graph.pin.connect.v1"]
        self.assertEqual(first["to"]["node_guid"], second["from"]["node_guid"])  # Random -> Inner, then Inner -> Outer

    def test_a_link_between_two_wildcards_that_never_get_a_type_is_still_made(self):
        from asset_document.tests.graph_support import FakeGraphContext
        from asset_document.tests.test_graph_executor import execute
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
        execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Outer", "Action": "K2:Select"}, {"Id": "Inner", "Action": "K2:Select"}],
            "Links": [{"From": "Inner:ReturnValue", "To": "Outer:Option 0"}]}]})
        self.assertEqual(len([name for name, _ in context.calls if name == "graph.pin.connect.v1"]), 1)

    def test_a_further_operand_is_added_before_its_type_is_converted(self):
        from asset_document.tests.graph_support import FakeGraphContext, TEMPLATES
        from asset_document.tests.test_graph_executor import execute, mutations
        TEMPLATES[MULTIPLY] = {"action_key": MULTIPLY, "implicit_role": None, "can_add_pin": True,
                               "pins": [pin("A", "input", ptype("wildcard")), pin("B", "input", ptype("wildcard")),
                                        pin("ReturnValue", "output", ptype("wildcard"))],
                               "extras": {"typed_pins": ["A", "B", "C", "ReturnValue"], "promotable": True}}
        try:
            context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")

            def add_pin(args):
                _, node = context._find(args["node_guid"])
                node["pins"].insert(2, pin("C", "input", ptype("wildcard")))
                return ok({"schema_version": 1, "node": deepcopy(node)})
            context._graph_node_dynamic_pin_add_v1 = add_pin
            execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
                {"Id": "Add", "Action": "K2:Multiply", "PinTypes": {"C": "int64"}}], "Links": []}]})
            self.assertEqual(mutations(context)[:3],
                             ["graph.node.spawn.v1", "graph.node.dynamic_pin.add.v1", "graph.node.pin_type.set.v1"])
        finally:
            del TEMPLATES[MULTIPLY]


class StaleObjectDefaultTests(unittest.TestCase):
    """An object pin's text DefaultValue saved by an old editor ("0" on StandardMacros' IsDedicatedServer
    WorldContextObject) is no value of the pin: not extracted, never reset."""
    GUID = "C" + "1" * 31
    ACTION = "K2:CallFunction:/Script/Engine.KismetSystemLibrary:IsDedicatedServer"

    def edit(self, value):
        node = live_node(self.GUID, self.ACTION, [
            pin("WorldContextObject", "input", ptype("object", "", "/Script/CoreUObject.Object"), default="0", autogen=False),
            pin("Message", "input", ptype("string"), default="Hi", autogen=False),
            pin("ReturnValue", "output", ptype("bool"))], cls="/Script/BlueprintGraph.K2Node_CallFunction")
        add_nodes(node)(value)

    def test_the_stale_object_default_is_not_extracted(self):
        asset, graph = live_graph(self.edit)
        doc = extract_graph(graph, asset, graph_id="U", path="/Body/UbergraphPages/0", adapter=k2.K2_ADAPTER)
        node = next(item for item in doc.nodes if item.guid == self.GUID)
        self.assertEqual([(item.pin, item.payload) for item in node.pin_overrides], [("Message", "Hi")])

    def test_an_omitted_stale_object_default_is_not_reset(self):
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "IsDedicatedServer", "Action": self.ACTION, "NodeGuid": self.GUID,
             "PinOverrides": [{"Pin": "Message", "DefaultValue": "Hi"}]}], "Links": []}]}
        ops = [op for op in plan_for(body, edit=self.edit).ops if op.op == "graph.pin.default.set"]
        self.assertEqual(ops, [])
