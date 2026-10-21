from __future__ import annotations

from copy import deepcopy
import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.graphs.live import LiveAsset, decode_live_asset
from asset_document.graphs.reconcile import reconcile
from asset_document.graphs.schemas import k2
from asset_document.tests.graph_support import (
    ACTOR_CLASS, G_EVENT, N_BEGIN, N_BRANCH, N_ENTRY, N_PRINT, PRINT, fixture, live_asset,
)


EVENT = "UbergraphPages/EventGraph"
FUNC = "FunctionGraphs/AddHealth"
SIGNATURE = ("graph.query.v1.k2_function_signature.json", "graph.list.v1.k2_function.json")
# UE5 Blueprint "Float" (the Type shorthand "float") is real/double.
FLOAT = {"category": "real", "sub_category": "double", "sub_category_object": "", "container": "none",
         "is_ref": False, "is_const": False}
TICK = "K2:Event:/Script/Engine.Actor:ReceiveTick"


def plan_for(body, query="graph.query.v1.k2_empty_actor.json", listing="graph.list.v1.k2_empty_actor.json", edit=None):
    graphs, roles = k2.decode_body(body, self_class=ACTOR_CLASS)
    value = deepcopy(fixture(query))
    if edit is not None:
        edit(value)
    return reconcile(graphs, decode_live_asset(fixture(listing), value), managed_roles=roles, adapter=k2.K2_ADAPTER,
                     self_class=ACTOR_CLASS, role_paths={role: f"/Body/{s}" for s, role in k2.SECTIONS})


def live_node(query, guid):
    return next(node for graph in fixture(query)["graphs"] for node in graph["nodes"] if node["node_guid"] == guid)


def add_health(**signature):
    return {"FunctionGraphs": [{"Name": "AddHealth", "Signature": {
        "Inputs": [{"Name": "Amount", "Type": "float"}], "Outputs": [{"Name": "NewHealth", "Type": "float"}],
        **signature}, "Links": [{"From": "Entry:then", "To": "Result:execute"}]}]}


def edit_entry(**flags):
    def edit(value):
        entry = next(node for graph in value["graphs"] for node in graph["nodes"] if node["node_guid"] == N_ENTRY)
        entry["extras"]["signature_flags"].update(flags)
    return edit


def connect(a, ap, b, bp):
    return {"from": {"node": a, "pin": ap, "direction": "output"}, "to": {"node": b, "pin": bp, "direction": "input"}}


def begin_branch_print_body(text="Hi"):
    return {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
        {"Id": "BeginPlay", "Action": "K2:BeginPlay"},
        {"Id": "Branch", "Action": "K2:Branch"},
        {"Id": "Print", "Action": "K2:PrintString", "PinOverrides": [{"Pin": "InString", "DefaultValue": text}]}],
        "Links": [{"From": "BeginPlay:then", "To": "Branch:execute"}, {"From": "Branch:then", "To": "Print:execute"}]}]}


class ReconcileGoldenTests(unittest.TestCase):
    def test_empty_blueprint_to_begin_play_branch_print(self):
        plan = plan_for(begin_branch_print_body())
        # Unplaced nodes are laid out once they exist (graph.layout.auto, with the editor's node sizes),
        # together with the reused ghost BeginPlay whose place the editor picked.
        self.assertEqual(plan.briefs(), [
            ("graph.node.spawn", EVENT, {"node": "Branch", "action_key": "K2:IfThenElse"}),
            ("graph.node.spawn", EVENT, {"node": "Print", "action_key": PRINT}),
            ("graph.pin.connect", EVENT, connect("BeginPlay", "then", "Branch", "execute")),
            ("graph.pin.connect", EVENT, connect("Branch", "then", "Print", "execute")),
            ("graph.pin.default.set", EVENT, {"pin": {"node": "Print", "pin": "InString", "direction": "input"},
                                              "kind": "value", "value": "Hi"}),
            ("graph.layout.auto", EVENT, {"nodes": ["BeginPlay", "Branch", "Print"]}),
            ("graph.finalize", "", {}),
        ])
        # The ghost BeginPlay of a new Blueprint is reused (linking it makes it real); ghost
        # Tick/Overlap are neither removed nor recreated.
        self.assertEqual(dict(plan.node_bindings), {(EVENT, "BeginPlay"): N_BEGIN})
        self.assertEqual([op.path for op in plan.ops], [
            "/Body/UbergraphPages/0/Nodes/1", "/Body/UbergraphPages/0/Nodes/2", "/Body/UbergraphPages/0/Links/0",
            "/Body/UbergraphPages/0/Links/1", "/Body/UbergraphPages/0/Nodes/2/PinOverrides/0", "/Body/UbergraphPages/0",
            "/Body"])

    def test_deleting_branch_and_linking_directly_only_removes_and_connects(self):
        for with_guids in (True, False):
            with self.subTest(with_guids=with_guids):
                begin = {"Id": "BeginPlay", "Action": "K2:BeginPlay"}
                printer = {"Id": "Print", "Action": "K2:PrintString",
                           "PinOverrides": [{"Pin": "InString", "DefaultValue": "Hello from AssetDocument"}]}
                if with_guids:
                    begin["NodeGuid"], printer["NodeGuid"] = N_BEGIN.lower(), N_PRINT
                plan = plan_for({"UbergraphPages": [{"Name": "EventGraph", "Nodes": [begin, printer],
                                                     "Links": [{"From": "BeginPlay:then", "To": "Print:execute"}]}]},
                                "graph.query.v1.k2_branch.json")
                self.assertEqual(plan.briefs(), [
                    ("graph.node.remove", EVENT, {"node_guid": N_BRANCH, "action_key": "K2:IfThenElse"}),
                    ("graph.pin.connect", EVENT, connect("BeginPlay", "then", "Print", "execute")),
                    ("graph.finalize", "", {}),
                ])
                self.assertEqual(dict(plan.node_bindings), {(EVENT, "BeginPlay"): N_BEGIN, (EVENT, "Print"): N_PRINT})

    def test_rewiring_kept_nodes_breaks_then_connects(self):
        body = begin_branch_print_body("Hello from AssetDocument")
        body["UbergraphPages"][0]["Links"][1] = {"From": "Branch:else", "To": "Print:execute"}
        plan = plan_for(body, "graph.query.v1.k2_branch.json")
        self.assertEqual(plan.briefs(), [
            ("graph.pin.break", EVENT, {"from": {"node": "Branch", "pin": "then", "direction": "output"},
                                        "to": {"node": "Print", "pin": "execute", "direction": "input"}}),
            ("graph.pin.connect", EVENT, connect("Branch", "else", "Print", "execute")),
            ("graph.finalize", "", {}),
        ])

    def test_omitted_override_resets_authored_default_and_position_is_generated_metadata(self):
        body = begin_branch_print_body()
        body["UbergraphPages"][0]["Nodes"][2] = {"Id": "Print", "Action": "K2:PrintString"}
        body["UbergraphPages"][0]["Nodes"][1]["Position"] = {"X": 320, "Y": 64}
        plan = plan_for(body, "graph.query.v1.k2_branch.json")
        self.assertEqual(plan.briefs(), [
            ("graph.pin.default.set", EVENT, {"pin": {"node": "Print", "pin": "InString", "direction": "input"},
                                              "kind": "reset"}),
            ("graph.node.layout.set", EVENT, {"node": "Branch", "position": {"x": 320, "y": 64}}),
            ("graph.finalize", "", {}),
        ])

    def test_function_with_entry_result_signature_on_new_graph(self):
        body = {"FunctionGraphs": [{"Name": "AddHealth", "Signature": {
            "Inputs": [{"Name": "Amount", "Type": "float"}], "Outputs": [{"Name": "NewHealth", "Type": "float"}]},
            "Links": [{"From": "Entry:then", "To": "Result:execute"}]}]}
        plan = plan_for(body)
        self.assertEqual(plan.briefs(), [
            ("graph.create", FUNC, {"role": "function", "name": "AddHealth"}),
            ("graph.node.user_pins.set", FUNC, {"implicit": "entry", "pins": [
                {"name": "Amount", "direction": "output", "type": FLOAT}]}),
            ("graph.node.user_pins.set", FUNC, {"implicit": "result", "spawn_action": "K2:FunctionResult", "pins": [
                {"name": "NewHealth", "direction": "input", "type": FLOAT}]}),
            ("graph.pin.connect", FUNC, connect("Entry", "then", "Result", "execute")),
            ("graph.layout.auto", FUNC, {"nodes": ["Entry", "Result"]}),
            ("graph.finalize", "", {}),
        ])
        # UserConstructionScript is managed (FunctionGraphs present) but not removable.
        self.assertNotIn("graph.remove", [op.op for op in plan.ops])
        same = plan_for(body, "graph.query.v1.k2_function_signature.json", "graph.list.v1.k2_function.json")
        self.assertEqual(same.briefs(), [])

    def test_function_with_two_return_nodes_on_new_graph(self):
        # The editor lets a function hold several return nodes (an early return from a Branch); the
        # first is the signature's own, every further one is placed with K2:FunctionResult.
        body = {"FunctionGraphs": [{"Name": "AddHealth", "Signature": {
            "Outputs": [{"Name": "NewHealth", "Type": "float"}]},
            "Nodes": [{"Id": "Branch", "Action": "K2:IfThenElse"},
                      {"Id": "Early", "Implicit": "result", "Position": {"X": 400, "Y": 200}}],
            "Links": [{"From": "Entry:then", "To": "Branch:execute"}, {"From": "Branch:then", "To": "Result:execute"},
                      {"From": "Branch:else", "To": "Early:execute"}]}]}
        plan = plan_for(body)
        spawns = [(op.args["node"], op.args["action_key"]) for op in plan.ops if op.op == "graph.node.spawn"]
        self.assertEqual(spawns, [("Early", "K2:FunctionResult"), ("Branch", "K2:IfThenElse")])
        user_pins = [op.args for op in plan.ops if op.op == "graph.node.user_pins.set"]
        self.assertEqual([args["implicit"] for args in user_pins], ["result"])
        self.assertIn(("graph.pin.connect", FUNC, connect("Branch", "else", "Early", "execute")), plan.briefs())

    def test_format_text_format_is_set_before_links_to_its_argument_pins(self):
        # UK2Node_FormatText makes the "num" input pin from its Format text "{num}".
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Format", "Action": "K2:FormatText", "PinOverrides": [{"Pin": "Format", "DefaultTextValue": "{num}"}]},
            {"Id": "Count", "Action": "K2:VariableGet:Self:Count"}],
            "Links": [{"From": "Count:Count", "To": "Format:num"}]}]}
        ops = [op.op for op in plan_for(body, edit=lambda value: None).ops if op.op.startswith("graph.pin")]
        self.assertEqual(ops, ["graph.pin.default.set", "graph.pin.connect"])

    def test_reroute_links_into_target_pins_are_made_last(self):
        # A reroute takes its type from its first link: in MakeArray -> Reroute -> Reroute_2 -> Print:self
        # (an array of targets) the link into "self", which also takes a single object, comes last.
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "MakeArray", "Action": "K2:MakeArray"}, {"Id": "Reroute", "Action": "K2:Reroute"},
            {"Id": "Reroute_2", "Action": "K2:Reroute"}, {"Id": "Print", "Action": "K2:PrintString"}],
            "Links": [{"From": "Reroute_2:OutputPin", "To": "Print:self"},
                      {"From": "Reroute:OutputPin", "To": "Reroute_2:InputPin"},
                      {"From": "MakeArray:Array", "To": "Reroute:InputPin"}]}]}
        connects = [(op.args["from"]["node"], op.args["to"]["node"]) for op in plan_for(body).ops
                    if op.op == "graph.pin.connect"]
        self.assertEqual(connects, [("Reroute", "Reroute_2"), ("MakeArray", "Reroute"), ("Reroute_2", "Print")])

    def test_an_operator_output_is_linked_after_its_operands(self):
        # A promotable operator picks its function from the types linked into it: Divide's output linked
        # into a Multiply before Divide's operands would take the Multiply's type (BP_ExtraMovementInducer).
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Divide", "Action": "K2:Divide"}, {"Id": "Multiply", "Action": "K2:Multiply"},
            {"Id": "Get_A", "Action": "K2:Get:A"}, {"Id": "Get_B", "Action": "K2:Get:B"},
            {"Id": "Get_V", "Action": "K2:Get:V"}, {"Id": "Print", "Action": "K2:PrintString"}],
            "Links": [{"From": "Multiply:ReturnValue", "To": "Print:InString"},
                      {"From": "Get_B:B", "To": "Divide:B"},
                      {"From": "Divide:ReturnValue", "To": "Multiply:B"},
                      {"From": "Get_V:V", "To": "Multiply:A"},
                      {"From": "Get_A:A", "To": "Divide:A"}]}]}
        connects = [(op.args["from"]["node"], op.args["to"]["node"]) for op in plan_for(body).ops
                    if op.op == "graph.pin.connect"]
        self.assertEqual(connects, [("Get_B", "Divide"), ("Get_V", "Multiply"), ("Get_A", "Divide"),
                                    ("Divide", "Multiply"), ("Multiply", "Print")])

    def test_an_operator_output_through_a_reroute_is_linked_after_its_operands(self):
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Divide", "Action": "K2:Divide"}, {"Id": "Multiply", "Action": "K2:Multiply"},
            {"Id": "Reroute", "Action": "K2:Reroute"},
            {"Id": "Get_A", "Action": "K2:Get:A"}, {"Id": "Get_V", "Action": "K2:Get:V"}],
            "Links": [{"From": "Reroute:OutputPin", "To": "Multiply:B"},
                      {"From": "Divide:ReturnValue", "To": "Reroute:InputPin"},
                      {"From": "Get_V:V", "To": "Multiply:A"},
                      {"From": "Get_A:A", "To": "Divide:A"}]}]}
        connects = [(op.args["from"]["node"], op.args["to"]["node"]) for op in plan_for(body).ops
                    if op.op == "graph.pin.connect"]
        self.assertEqual(connects, [("Get_V", "Multiply"), ("Get_A", "Divide"), ("Divide", "Reroute"),
                                    ("Reroute", "Multiply")])

    def test_a_keyless_node_naming_what_does_not_exist_is_refused(self):
        # Extraction of a variable get whose variable is gone (JumpFlood_Component2D's World): no palette
        # action places it, so a Create refuses it by name rather than as an unknown node.
        def body(extras):
            return {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
                {"Id": "Get_World", "NodeGuid": "A" * 32, "Extras": extras}], "Links": []}]}
        with self.assertRaises(AssetDocumentContractError) as caught:
            plan_for(body({"Class": "/Script/BlueprintGraph.K2Node_VariableGet", "Unresolved": "variable World"}))
        self.assertEqual(caught.exception.diagnostic.code, "Graph.UnresolvedReference")
        self.assertIn("variable World", caught.exception.diagnostic.message)
        with self.assertRaises(AssetDocumentContractError) as caught:
            plan_for(body({"Class": "/Script/BlueprintGraph.K2Node_VariableGet"}))
        self.assertEqual(caught.exception.diagnostic.code, "Graph.NodeNotFound")

    def test_a_container_function_is_linked_after_its_container(self):
        # Map Find's Value linked into a Texture input first would make the map Name->Texture, and the
        # Name->Texture2D map would no longer fit (Landmass_FunctionLibrary).
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Find", "Action": "K2:CallFunction:/Script/Engine.BlueprintMapLibrary:Map_Find"},
            {"Id": "Print", "Action": "K2:PrintString"}, {"Id": "Get_M", "Action": "K2:Get:M"}],
            "Links": [{"From": "Find:Value", "To": "Print:InString"},
                      {"From": "Get_M:M", "To": "Find:TargetMap"}]}]}
        connects = [(op.args["from"]["node"], op.args["to"]["node"]) for op in plan_for(body).ops
                    if op.op == "graph.pin.connect"]
        self.assertEqual(connects, [("Get_M", "Find"), ("Find", "Print")])

    def test_a_container_is_linked_into_its_function_before_an_element(self):
        # Array Add's Texture 2D NewItem linked first would type its array Texture 2D, and the Object
        # array would no longer fit (LandmassBrush_CopyRegion).
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Add", "Action": "K2:CallArrayFunction:/Script/Engine.KismetArrayLibrary:Array_Add"},
            {"Id": "Get_T", "Action": "K2:Get:T"}, {"Id": "Get_A", "Action": "K2:Get:A"}],
            "Links": [{"From": "Get_T:T", "To": "Add:NewItem"},
                      {"From": "Get_A:A", "To": "Add:TargetArray"}]}]}
        connects = [(op.args["from"]["node"], op.args["to"]["pin"]) for op in plan_for(body).ops
                    if op.op == "graph.pin.connect"]
        self.assertEqual(connects, [("Get_A", "TargetArray"), ("Get_T", "NewItem")])

    def test_make_array_into_a_target_pin_keeps_document_order(self):
        # A Make Array takes its element type from its first link: linked into "self" first it holds the
        # target class, and its sibling-class elements then fit (FS_AnchorField_Generic).
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "MakeArray", "Action": "K2:MakeArray"}, {"Id": "Print", "Action": "K2:PrintString"},
            {"Id": "Get_A", "Action": "K2:Get:A"}],
            "Links": [{"From": "MakeArray:Array", "To": "Print:self"},
                      {"From": "Get_A:A", "To": "MakeArray:[0]"}]}]}
        connects = [(op.args["from"]["node"], op.args["to"]["node"]) for op in plan_for(body).ops
                    if op.op == "graph.pin.connect"]
        self.assertEqual(connects, [("MakeArray", "Print"), ("Get_A", "MakeArray")])

    def test_signature_change_and_function_removal(self):
        body = {"FunctionGraphs": [{"Name": "AddHealth", "Signature": {
            "Inputs": [{"Name": "Amount", "Type": "int"}], "Outputs": [{"Name": "NewHealth", "Type": "float"}]},
            "Links": [{"From": "Entry:then", "To": "Result:execute"}]}]}
        plan = plan_for(body, *SIGNATURE)
        self.assertEqual([(op.op, op.args.get("implicit")) for op in plan.ops],
                         [("graph.node.user_pins.set", "entry"), ("graph.finalize", None)])
        # Single precision is spelled float32 and differs from the live double pin.
        single = add_health()
        single["FunctionGraphs"][0]["Signature"]["Outputs"][0]["Type"] = "float32"
        self.assertEqual([(op.op, op.args.get("implicit")) for op in plan_for(single, *SIGNATURE).ops],
                         [("graph.node.user_pins.set", "result"), ("graph.finalize", None)])
        removal = plan_for({"FunctionGraphs": []}, "graph.query.v1.k2_function_signature.json",
                           "graph.list.v1.k2_function.json")
        self.assertEqual(removal.briefs(), [
            ("graph.remove", "", {"graph_guid": "A0000000000000000000000000000003", "name": "AddHealth"}),
            ("graph.finalize", "", {})])
        self.assertEqual(removal.ops[0].path, "/Body/FunctionGraphs")

    def test_unreferenced_entry_links_are_broken(self):
        body = {"FunctionGraphs": [{"Name": "AddHealth", "Signature": {
            "Inputs": [{"Name": "Amount", "Type": "float"}], "Outputs": [{"Name": "NewHealth", "Type": "float"}]}}]}
        plan = plan_for(body, "graph.query.v1.k2_function_signature.json", "graph.list.v1.k2_function.json")
        self.assertEqual([op.op for op in plan.ops], ["graph.pin.break", "graph.finalize"])
        self.assertIn("node_guid", plan.ops[0].args["from"])

    def test_legacy_class_member_form_produces_the_action_form_plan(self):
        legacy = {"UbergraphPages": [{"Name": "EventGraph", "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2", "Nodes": [
            {"Id": "BeginPlay", "Class": "/Script/BlueprintGraph.K2Node_Event",
             "Member": {"Kind": "MemberRef", "OwnerClass": "/Script/Engine.Actor", "Name": "ReceiveBeginPlay"}},
            {"Id": "Branch", "Class": "/Script/BlueprintGraph.K2Node_IfThenElse"},
            {"Id": "Print", "Class": "/Script/BlueprintGraph.K2Node_CallFunction",
             "Member": {"Kind": "MemberRef", "OwnerClass": "/Script/Engine.KismetSystemLibrary", "Name": "PrintString"},
             "PinOverrides": [{"Pin": "InString", "DefaultValue": "Hi"}]}],
            "Links": [{"From": {"Node": "BeginPlay", "Pin": "then"}, "To": {"Node": "Branch", "Pin": "execute"}},
                      {"From": {"Node": "Branch", "Pin": "then"}, "To": {"Node": "Print", "Pin": "execute"}}]}]}
        self.assertEqual(plan_for(legacy).briefs(), plan_for(begin_branch_print_body()).briefs())

    def test_reversed_link_and_pin_case_are_canonicalized_against_live_pins(self):
        body = begin_branch_print_body("Hello from AssetDocument")
        body["UbergraphPages"][0]["Links"] = [{"From": "Branch:Execute", "To": "BeginPlay:Then"},
                                              {"From": "Branch:then", "To": "Print:execute"}]
        self.assertEqual(plan_for(body, "graph.query.v1.k2_branch.json").briefs(), [])

    def test_guid_with_changed_action_replaces_the_node(self):
        body = begin_branch_print_body("Hello from AssetDocument")
        body["UbergraphPages"][0]["Nodes"][1] = {"Id": "Branch", "Action": "K2:Sequence", "NodeGuid": N_BRANCH}
        body["UbergraphPages"][0]["Links"][1] = {"From": "Branch:then_0", "To": "Print:execute"}
        plan = plan_for(body, "graph.query.v1.k2_branch.json")
        self.assertEqual([op.op for op in plan.ops][:2], ["graph.node.remove", "graph.node.spawn"])
        self.assertEqual(plan.ops[1].args["action_key"], "K2:ExecutionSequence")

    def test_exec_links_connect_before_data_links_and_defaults_follow_links(self):
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Rand", "Action": "K2:CallFunction:/Script/Engine.KismetMathLibrary:RandomBool"},
            {"Id": "BeginPlay", "Action": "K2:BeginPlay"},
            {"Id": "Branch", "Action": "K2:Branch", "PinOverrides": [{"Pin": "Condition", "DefaultValue": "false"}]}],
            "Links": [{"From": "Rand:ReturnValue", "To": "Branch:Condition"},
                      {"From": "BeginPlay:then", "To": "Branch:execute"}]}]}
        ops = [op.op + ":" + op.path.rsplit("/", 2)[-2] + "/" + op.path.rsplit("/", 1)[-1] for op in plan_for(body).ops]
        self.assertEqual(ops, ["graph.node.spawn:Nodes/0", "graph.node.spawn:Nodes/2", "graph.pin.connect:Links/1",
                               "graph.pin.connect:Links/0", "graph.pin.default.set:PinOverrides/0",
                               "graph.layout.auto:UbergraphPages/0", "graph.finalize:/Body"])

    def test_properties_rename_comment_and_parent_order(self):
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Child", "Action": "K2:PrintString", "Parent": "Owner"},
            {"Id": "Owner", "Action": "K2:CustomEvent", "Name": "OnPing", "Comment": "entry point",
             "Properties": {"bCallInEditor": True}}]}]}
        plan = plan_for(body)
        self.assertEqual([(op.op, op.args.get("node")) for op in plan.ops], [
            ("graph.node.spawn", "Owner"), ("graph.node.spawn", "Child"),
            ("graph.node.property.set", "Owner"), ("graph.node.rename", "Owner"),
            ("graph.node.layout.set", "Owner"), ("graph.layout.auto", None), ("graph.finalize", None)])
        self.assertEqual(plan.ops[1].args["parent"], "Owner")
        self.assertEqual(plan.ops[2].path, "/Body/UbergraphPages/0/Nodes/1/Properties/bCallInEditor")

    def test_a_called_custom_event_is_made_before_the_nodes_of_every_graph(self):
        # PathFollowingMannyPawn: an event graph calls "MovePawn", a custom event of a later page; the
        # call is placed from the event's function, which exists once the event is spawned and named.
        body = {"UbergraphPages": [
            {"Name": "EventGraph", "Nodes": [
                {"Id": "BeginPlay", "Action": "K2:BeginPlay"},
                {"Id": "Call", "Action": "K2:CallFunction:Self:MovePawn"}],
             "Links": [{"From": "BeginPlay:then", "To": "Call:execute"}]},
            {"Name": "Movement", "Nodes": [
                {"Id": "Print", "Action": "K2:PrintString"},
                {"Id": "MovePawn", "Action": "K2:CustomEvent", "Name": "MovePawn",
                 "Inputs": [{"Name": "Speed", "Type": "float"}]},
                {"Id": "Unused", "Action": "K2:CustomEvent", "Name": "NotCalled"}],
             "Links": [{"From": "MovePawn:then", "To": "Print:execute"}]}]}
        ops = [(op.op, op.args.get("node")) for op in plan_for(body).ops
               if op.op in ("graph.node.spawn", "graph.node.rename", "graph.node.user_pins.set")]
        self.assertEqual(ops[:3], [("graph.node.spawn", "MovePawn"), ("graph.node.rename", "MovePawn"),
                                   ("graph.node.user_pins.set", "MovePawn")])
        self.assertEqual(ops.count(("graph.node.rename", "MovePawn")), 1)
        self.assertLess(ops.index(("graph.node.user_pins.set", "MovePawn")), ops.index(("graph.node.spawn", "Call")))
        # An event no call names keeps its place (renamed with the other node settings).
        self.assertGreater(ops.index(("graph.node.spawn", "Unused")), ops.index(("graph.node.spawn", "Call")))

    def test_absent_sections_are_unmanaged_and_empty_live_creates_graph(self):
        self.assertEqual(plan_for({}).briefs(), [])
        graphs, roles = k2.decode_body(begin_branch_print_body())
        plan = reconcile(graphs, LiveAsset(), managed_roles=roles, adapter=k2.K2_ADAPTER)
        self.assertEqual([op.op for op in plan.ops][:4], ["graph.create", "graph.node.spawn", "graph.node.spawn",
                                                          "graph.node.spawn"])

    def test_empty_ubergraph_pages_clears_the_kept_event_graph(self):
        # The EventGraph cannot be deleted, so "UbergraphPages": [] removes every node in it
        # (what a user deleting all its nodes does); an absent section still leaves it alone.
        self.assertEqual(plan_for({"UbergraphPages": []}, "graph.query.v1.k2_branch.json").briefs(), [
            ("graph.node.remove", "ubergraph:A0000000000000000000000000000001",
             {"node_guid": N_BEGIN, "action_key": "K2:Event:/Script/Engine.Actor:ReceiveBeginPlay"}),
            ("graph.node.remove", "ubergraph:A0000000000000000000000000000001",
             {"node_guid": N_BRANCH, "action_key": "K2:IfThenElse"}),
            ("graph.node.remove", "ubergraph:A0000000000000000000000000000001",
             {"node_guid": N_PRINT, "action_key": "K2:CallFunction:/Script/Engine.KismetSystemLibrary:PrintString"}),
            ("graph.finalize", "", {}),
        ])
        self.assertEqual(plan_for({}, "graph.query.v1.k2_branch.json").briefs(), [])
        self.assertEqual(plan_for({"UbergraphPages": []}).briefs(), [])

    def test_user_pin_defaults_compare_against_autogenerated_live_defaults(self):
        # Live NewHealth carries the editor's autogenerated "0.0": omitting Default matches it.
        self.assertEqual(plan_for(add_health(), *SIGNATURE).briefs(), [])

        def authored(value):
            entry = next(node for graph in value["graphs"] for node in graph["nodes"] if node["node_guid"] == N_ENTRY)
            entry["pins"][1].update(default_value="5.0", is_autogenerated_default=False)
        # An authored live default is reset when the document omits it, kept when it states it.
        plan = plan_for(add_health(), *SIGNATURE, edit=authored)
        self.assertEqual(plan.briefs()[0], ("graph.node.user_pins.set", FUNC, {"implicit": "entry", "pins": [
            {"name": "Amount", "direction": "output", "type": FLOAT}]}))
        stated = add_health()
        stated["FunctionGraphs"][0]["Signature"]["Inputs"][0]["Default"] = "5.0"
        self.assertEqual(plan_for(stated, *SIGNATURE, edit=authored).briefs(), [])

    def test_signature_flags_emit_partial_flags_set_only_when_different(self):
        plan = plan_for(add_health(Pure=True, Category="Health", Access="public"), *SIGNATURE)
        self.assertEqual(plan.briefs(), [
            ("graph.function.flags.set", FUNC, {"flags": {"pure": True, "category": "Health"}}),
            ("graph.finalize", "", {})])
        self.assertEqual(plan.ops[0].path, "/Body/FunctionGraphs/0/Signature")
        self.assertEqual(plan_for(add_health(Pure=True), *SIGNATURE, edit=edit_entry(pure=True)).briefs(), [])
        # A present Signature states every flag: an omitted flag returns to its default.
        self.assertEqual(plan_for(add_health(), *SIGNATURE, edit=edit_entry(const=True, tooltip="x")).briefs(), [
            ("graph.function.flags.set", FUNC, {"flags": {"const": False, "tooltip": ""}}),
            ("graph.finalize", "", {})])
        # A new function graph starts with default flags.
        created = plan_for(add_health(CallInEditor=True))
        self.assertEqual([op.op for op in created.ops][:4], ["graph.create", "graph.node.user_pins.set",
                                                             "graph.node.user_pins.set", "graph.function.flags.set"])
        self.assertEqual(created.ops[3].args, {"flags": {"call_in_editor": True}})

    def test_signature_flag_diagnostics(self):
        cases = (
            (add_health(Bogus=1), ("Graph.UnknownField", "/Body/FunctionGraphs/0/Signature/Bogus")),
            (add_health(Pure="yes"), ("Graph.InvalidSignature", "/Body/FunctionGraphs/0/Signature/Pure")),
            (add_health(Access="internal"), ("Graph.InvalidSignature", "/Body/FunctionGraphs/0/Signature/Access")),
            ({"MacroGraphs": [{"Name": "M", "Signature": {"Pure": True}}]},
             ("Graph.InvalidSignature", "/Body/MacroGraphs/0/Signature")),
        )
        for body, expected in cases:
            with self.subTest(expected=expected):
                with self.assertRaises(AssetDocumentContractError) as caught:
                    plan_for(body, *SIGNATURE)
                self.assertEqual((caught.exception.diagnostic.code, caught.exception.diagnostic.path), expected)

    def test_declared_ghost_event_without_links_is_reused_by_spawn(self):
        body = begin_branch_print_body()
        body["UbergraphPages"][0]["Nodes"].append({"Id": "Tick", "Action": "K2:Tick"})
        plan = plan_for(body)
        tick = live_node("graph.query.v1.k2_empty_actor.json", "B0000000000000000000000000000003")
        self.assertTrue(tick["extras"]["ghost"])
        self.assertEqual(plan.briefs()[0], ("graph.node.spawn", EVENT, {"node": "Tick", "action_key": TICK}))
        self.assertEqual(plan.node_bindings[(EVENT, "Tick")], tick["node_guid"])
        self.assertEqual([op.args.get("node") for op in plan.ops if op.op == "graph.node.spawn"], ["Tick", "Branch", "Print"])

    def test_graph_without_rename_capability_is_not_renamed(self):
        # A case-only change is a rename too (the editor renames "EventGraph" to "eventgraph"): refused, not ignored.
        for body in ({"UbergraphPages": [{"Name": "eventgraph"}]},
                     {"UbergraphPages": [{"Name": "Main", "GraphGuid": G_EVENT}]}):
            with self.assertRaises(AssetDocumentContractError) as caught:
                plan_for(body)
            self.assertEqual((caught.exception.diagnostic.code, caught.exception.diagnostic.path),
                             ("Graph.GraphOperationUnsupported", "/Body/UbergraphPages/0/Name"))

    def test_plans_are_deterministic(self):
        first = plan_for(begin_branch_print_body()).briefs()
        for _ in range(3):
            self.assertEqual(plan_for(begin_branch_print_body()).briefs(), first)


if __name__ == "__main__":
    unittest.main()
