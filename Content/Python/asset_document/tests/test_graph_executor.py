# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import unittest
from unittest import mock

from asset_document.diagnostics import AssetDocumentContractError, DiagnosticSeverity
from asset_document.graphs import executor as executor_module
from asset_document.graphs.executor import GraphExecutor, graph_requirements
from asset_document.graphs.extract import extract_graphs
from asset_document.graphs.reconcile import PlanOp
from asset_document.graphs.schemas import k2
from asset_document.tests.graph_support import (
    ACTOR_CLASS, G_EVENT, N_BEGIN, N_BRANCH, N_PRINT, RANDOM_BOOL, FakeGraphContext,
)


ROLE_PATHS = {role: f"/Body/{section}" for section, role in k2.SECTIONS}


def execute(context, body):
    graphs, roles = k2.decode_body(body, self_class=ACTOR_CLASS)
    executor = GraphExecutor(context, read_handle=object(), write_handle=object(), route_id="route.blueprint.v1",
                             adapter=k2.K2_ADAPTER, self_class=ACTOR_CLASS)
    return executor.run(graphs, managed_roles=roles, role_paths=ROLE_PATHS)


def mutations(context):
    return [name for name in context.primitive_sequence() if name not in ("graph.list.v1", "graph.query.v1")]


def read_live(context):
    executor = GraphExecutor(context, read_handle=object(), route_id="route.blueprint.v1", adapter=k2.K2_ADAPTER)
    return executor.read_live()


def snapshot_links(context, graph_guid):
    graph = context.graphs[graph_guid]
    # FunctionEntry has no action_key (it cannot be spawned); it is identified by its implicit role.
    names = {node["node_guid"]: node["action_key"] or node["implicit_role"] for node in graph["nodes"]}
    return sorted((names[a], ap, names[b], bp) for a, ap, b, bp in graph["links"])


class GraphExecutorTests(unittest.TestCase):
    def test_requirements_cover_every_contract_primitive_on_the_route(self):
        requirements = graph_requirements("route.blueprint.v1")
        self.assertEqual(len(requirements), 21)
        self.assertIn("graph.function.flags.set.v1", [item.primitive_id for item in requirements])
        self.assertTrue(all(item.version == 1 and item.required_by_route_id == "route.blueprint.v1"
                            for item in requirements))

    def test_builds_begin_play_branch_print_and_binds_guids(self):
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
        report = execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "BeginPlay", "Action": "K2:BeginPlay"},
            {"Id": "Branch", "Action": "K2:Branch"},
            {"Id": "Print", "Action": "K2:PrintString", "PinOverrides": [{"Pin": "InString", "DefaultValue": "Hi"}]}],
            "Links": [{"From": "BeginPlay:Then", "To": "Branch:execute"}, {"From": "Branch:then", "To": "Print:execute"}]}]})
        # The new nodes and the reused ghost BeginPlay are laid out once they exist (one move each).
        self.assertEqual(mutations(context), ["graph.node.spawn.v1", "graph.node.spawn.v1", "graph.pin.connect.v1",
                                              "graph.pin.connect.v1", "graph.pin.default.set.v1"]
                         + ["graph.node.layout.set.v1"] * 3 + ["graph.finalize.v1"])
        bindings = report.node_bindings
        self.assertEqual(bindings[("UbergraphPages/EventGraph", "BeginPlay")], N_BEGIN)
        # Pin names are resolved against live pins at call time ("Then" -> "then").
        first = context.calls_of("graph.pin.connect.v1")[0]
        self.assertEqual(first["from"], {"node_guid": N_BEGIN, "pin_name": "then", "direction": "output"})
        self.assertEqual(first["to"]["node_guid"], bindings[("UbergraphPages/EventGraph", "Branch")])
        self.assertEqual(report.finalize_status, "ok")
        self.assertEqual(snapshot_links(context, "A0000000000000000000000000000001"), [
            ("K2:Event:/Script/Engine.Actor:ReceiveBeginPlay", "then", "K2:IfThenElse", "execute"),
            ("K2:IfThenElse", "then", "K2:CallFunction:/Script/Engine.KismetSystemLibrary:PrintString", "execute")])
        # Linking the reused ghost BeginPlay made it a real node; the other ghosts are untouched.
        ghosts = {node["node_guid"]: node["extras"].get("ghost", False) for node in context.graphs[G_EVENT]["nodes"]}
        self.assertEqual([ghosts[N_BEGIN], ghosts["B0000000000000000000000000000002"],
                          ghosts["B0000000000000000000000000000003"]], [False, True, True])

    def test_delete_branch_keeps_guids_of_kept_nodes(self):
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_branch.json")
        report = execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "BeginPlay", "Action": "K2:BeginPlay"},
            {"Id": "Print", "Action": "K2:PrintString",
             "PinOverrides": [{"Pin": "InString", "DefaultValue": "Hello from AssetDocument"}]}],
            "Links": [{"From": "BeginPlay:then", "To": "Print:execute"}]}]})
        self.assertEqual(mutations(context), ["graph.node.remove.v1", "graph.pin.connect.v1", "graph.finalize.v1"])
        self.assertEqual(context.calls_of("graph.node.remove.v1")[0]["node_guid"], N_BRANCH)
        self.assertEqual(report.node_bindings[("UbergraphPages/EventGraph", "Print")], N_PRINT)

    def test_pin_renamed_by_reconstruct_is_resolved_after_requery(self):
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
        execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Rand", "Action": RANDOM_BOOL},
            {"Id": "Select", "Action": "K2:Select", "PinOverrides": [
                {"Pin": "True", "DefaultValue": "yes"}, {"Pin": "False", "DefaultValue": "no"}]}],
            "Links": [{"From": "Rand:ReturnValue", "To": "Select:Index"}]}]})
        sequence = context.primitive_sequence()
        connect = sequence.index("graph.pin.connect.v1")
        # The connect reconstructed Select; the executor re-queried before resolving "True"/"False".
        self.assertEqual(sequence[connect + 1], "graph.query.v1")
        defaults = context.calls_of("graph.pin.default.set.v1")
        self.assertEqual([(item["pin"]["pin_name"], item["value"]) for item in defaults], [("True", "yes"), ("False", "no")])
        self.assertNotIn("graph.node.dynamic_pin.add.v1", sequence)

    def test_a_member_of_an_existing_pin_adds_no_pins(self):
        # "Option 0_X" names a member of Option 0, not a pin to add: adding pins until one is named so
        # grows the node by every pin it can take (AnimatedMannyPawnExtended's Subtract into B_Z).
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
        with self.assertRaises(AssetDocumentContractError):
            execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
                {"Id": "Rand", "Action": RANDOM_BOOL}, {"Id": "Select", "Action": "K2:Select"}],
                "Links": [{"From": "Rand:ReturnValue", "To": "Select:Option 0_X"}]}]})
        self.assertNotIn("graph.node.dynamic_pin.add.v1", context.primitive_sequence())

    def test_select_index_type_is_set_after_spawn_before_defaults(self):
        # RenderToTextureMacros: an unlinked enum Select; its option pins exist only once the Index type is picked.
        enum = "/Script/Engine.ECollisionChannel"
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
        execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Select", "Action": "K2:Select", "PinTypes": {"Index": f"byte:{enum}"},
             "PinOverrides": [{"Pin": "NewEnumerator0", "DefaultValue": "a"}]}], "Links": []}]})
        self.assertEqual(mutations(context), ["graph.node.spawn.v1", "graph.node.pin_type.set.v1",
                                              "graph.pin.default.set.v1", "graph.node.layout.set.v1",
                                              "graph.finalize.v1"])
        call = context.calls_of("graph.node.pin_type.set.v1")[0]
        self.assertEqual((call["pin_name"], call["type"]["category"], call["type"]["sub_category_object"]),
                         ("Index", "byte", enum))
        live = read_live(context)
        select = next(node for graph in live.graphs for node in graph.nodes if node.action_key == "K2:Select")
        self.assertEqual(k2.K2_ADAPTER.node_pin_types(select)[0][1].sub_category_object, enum)
        # Applying the same document again changes nothing; another index type is a new Select node.
        before = len(context.calls)
        execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Select", "Action": "K2:Select", "PinTypes": {"Index": f"byte:{enum}"},
             "PinOverrides": [{"Pin": "NewEnumerator0", "DefaultValue": "a"}]}], "Links": []}]})
        self.assertNotIn("graph.node.pin_type.set.v1", [call[0] for call in context.calls[before:]])
        before = len(context.calls)
        execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Select", "Action": "K2:Select", "PinTypes": {"Index": "int"}}], "Links": []}]})
        self.assertEqual([call[0] for call in context.calls[before:] if call[0] not in ("graph.list.v1", "graph.query.v1")],
                         ["graph.node.remove.v1", "graph.node.spawn.v1", "graph.node.pin_type.set.v1",
                          "graph.node.layout.set.v1", "graph.finalize.v1"])

    def test_connect_disallowed_is_mapped_to_the_authored_link(self):
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
        with self.assertRaises(AssetDocumentContractError) as caught:
            execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
                {"Id": "BeginPlay", "Action": "K2:BeginPlay"},
                {"Id": "Print", "Action": "K2:PrintString"}],
                "Links": [{"From": "BeginPlay:then", "To": "Print:execute"},
                          {"From": "BeginPlay:then", "To": "Print:InString"}]}]})
        diagnostic = caught.exception.diagnostic
        self.assertEqual(diagnostic.code, "Graph.ConnectDisallowed")
        self.assertEqual(diagnostic.path, "/Body/UbergraphPages/0/Links/1")
        # The pins are named as the document names them.
        self.assertEqual(diagnostic.message, "Cannot connect BeginPlay:then to Print:InString: exec is not compatible with string")
        context_values = {item.key: item.value for item in diagnostic.context}
        self.assertEqual(context_values["primitive"], "graph.pin.connect.v1")
        self.assertEqual(context_values["primitive_path"], "/value_arguments/to")
        self.assertNotIn("graph.finalize.v1", context.primitive_sequence())

    def test_missing_pin_names_available_pins(self):
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
        with self.assertRaises(AssetDocumentContractError) as caught:
            execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
                {"Id": "BeginPlay", "Action": "K2:BeginPlay"}, {"Id": "Print", "Action": "K2:PrintString"}],
                "Links": [{"From": "BeginPlay:then", "To": "Print:Exec"}]}]})
        diagnostic = caught.exception.diagnostic
        self.assertEqual((diagnostic.code, diagnostic.path), ("Graph.PinNotFound", "/Body/UbergraphPages/0/Links/0"))
        self.assertIn("execute", diagnostic.message)

    def test_dynamic_pins_are_added_until_the_link_resolves(self):
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
        execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "BeginPlay", "Action": "K2:BeginPlay"}, {"Id": "Seq", "Action": "K2:Sequence"},
            {"Id": "Print", "Action": "K2:PrintString"}],
            "Links": [{"From": "BeginPlay:then", "To": "Seq:execute"}, {"From": "Seq:then_3", "To": "Print:execute"}]}]})
        self.assertEqual(context.primitive_sequence().count("graph.node.dynamic_pin.add.v1"), 2)
        self.assertEqual(context.calls_of("graph.pin.connect.v1")[1]["from"]["pin_name"], "then_3")

    def test_a_new_function_takes_the_document_flags_not_the_blueprint_types(self):
        # An editor utility Blueprint's new functions are Call In Editor (ErosionComponent): a function whose
        # document leaves CallInEditor out is not.
        for signature in (None, {"Inputs": [{"Name": "Amount", "Type": "float"}]}):
            context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
            context.new_function_flags = {"call_in_editor": True}
            graph = {"Name": "Run", "Nodes": [], "Links": []}
            if signature is not None:
                graph["Signature"] = signature
            execute(context, {"FunctionGraphs": [graph]})
            self.assertEqual([call["flags"] for call in context.calls_of("graph.function.flags.set.v1")],
                             [{"call_in_editor": False}])

    def test_a_link_sends_the_stated_pin_types_of_its_end(self):
        # graph.pin.connect.v1 takes keep_to_types as {pin: PinType}; the native reader refuses anything else.
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
        execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "BeginPlay", "Action": "K2:BeginPlay"},
            {"Id": "Branch", "Action": "K2:Branch", "PinTypes": {"Condition": "bool"}}],
            "Links": [{"From": "BeginPlay:Then", "To": "Branch:execute"}]}]})
        connect = context.calls_of("graph.pin.connect.v1")[0]
        self.assertNotIn("keep_from_types", connect)
        self.assertEqual(connect["keep_to_types"], {"Condition": {
            "category": "bool", "sub_category": "", "sub_category_object": "", "container": "none",
            "is_ref": False, "is_const": False}})

    def test_a_new_override_holds_only_the_document_nodes(self):
        # Overriding a function the parent implements places "Parent: <function>"; the author of
        # AnimatedMannyPawnExtended's ApplyIMC removed it.
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
        context.new_function_placed = ["K2:CallParentFunction:/Game/Base.Base_C:ApplyIMC"]
        report = execute(context, {"FunctionGraphs": [{"Name": "ApplyIMC", "Nodes": [], "Links": []}]})
        graph = context.graphs[report.graph_bindings["FunctionGraphs/ApplyIMC"]]
        self.assertEqual([node["implicit_role"] for node in graph["nodes"]], ["entry"])
        self.assertEqual(len(context.calls_of("graph.node.remove.v1")), 1)

    def test_new_function_graph_with_signature_spawns_missing_result(self):
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
        report = execute(context, {"FunctionGraphs": [{"Name": "AddHealth", "Signature": {
            "Inputs": [{"Name": "Amount", "Type": "float"}], "Outputs": [{"Name": "NewHealth", "Type": "float"}]},
            "Nodes": [{"Id": "Result", "Implicit": "result", "Position": {"X": 400, "Y": 0}}],
            "Links": [{"From": "Entry:then", "To": "Result:execute"}]}]})
        self.assertEqual(mutations(context), [
            "graph.create.v1", "graph.node.user_pins.set.v1", "graph.node.spawn.v1", "graph.node.user_pins.set.v1",
            "graph.pin.connect.v1", "graph.node.layout.set.v1", "graph.node.layout.set.v1", "graph.finalize.v1"])
        self.assertEqual(context.calls_of("graph.node.spawn.v1")[0]["action_key"], "K2:FunctionResult")
        graph_guid = report.graph_bindings["FunctionGraphs/AddHealth"]
        self.assertEqual(snapshot_links(context, graph_guid), [("entry", "then", "K2:FunctionResult", "execute")])
        # Second run against the resulting live state is a no-op.
        context.calls.clear()
        execute(context, {"FunctionGraphs": [{"Name": "AddHealth", "Signature": {
            "Inputs": [{"Name": "Amount", "Type": "float"}], "Outputs": [{"Name": "NewHealth", "Type": "float"}]},
            "Nodes": [{"Id": "Result", "Implicit": "result", "Position": {"X": 400, "Y": 0}}],
            "Links": [{"From": "Entry:then", "To": "Result:execute"}]}]})
        self.assertEqual(mutations(context), [])

    def test_finalize_errors_map_node_guid_to_body_path(self):
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_branch.json")
        context.finalize_value = {"status": "errors", "messages": [
            {"severity": "warning", "message": "Unused", "node_guid": N_BEGIN},
            {"severity": "error", "message": "Accessed None", "node_guid": N_PRINT}]}
        with self.assertRaises(AssetDocumentContractError) as caught:
            execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
                {"Id": "BeginPlay", "Action": "K2:BeginPlay"},
                {"Id": "Print", "Action": "K2:PrintString", "PinOverrides": [{"Pin": "InString", "DefaultValue": "x"}]}],
                "Links": [{"From": "BeginPlay:then", "To": "Print:execute"}]}]})
        diagnostic = caught.exception.diagnostic
        self.assertEqual((diagnostic.code, diagnostic.path, diagnostic.severity),
                         ("Graph.FinalizeFailed", "/Body/UbergraphPages/0/Nodes/1", DiagnosticSeverity.ERROR))

    BRANCH_BODY = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
        {"Id": "BeginPlay", "Action": "K2:BeginPlay"},
        {"Id": "Print", "Action": "K2:PrintString", "PinOverrides": [{"Pin": "InString", "DefaultValue": "x"}]}],
        "Links": [{"From": "BeginPlay:then", "To": "Print:execute"}]}]}

    def test_linked_orphan_pin_error_names_node_and_pin(self):
        for failing in (False, True):
            context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_branch.json")
            messages = [{"severity": "error", "message": "In use pin Amount no longer exists", "node_guid": N_PRINT,
                         "pin": "Amount", "orphaned": True}]
            if failing:
                context.finalize_failure = messages
            else:
                context.finalize_value = {"status": "errors", "messages": messages}
            with self.assertRaises(AssetDocumentContractError) as caught:
                execute(context, self.BRANCH_BODY)
            diagnostic = caught.exception.diagnostic
            self.assertEqual((diagnostic.code, diagnostic.path, diagnostic.severity),
                             ("Graph.OrphanedPinLinked", "/Body/UbergraphPages/0/Nodes/1", DiagnosticSeverity.ERROR))
            self.assertIn("'Amount'", diagnostic.message)
            self.assertIn("'Print'", diagnostic.message)

    def test_valued_orphan_pin_is_a_warning_in_the_report(self):
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_branch.json")
        context.finalize_value = {"status": "warnings", "messages": [
            {"severity": "warning", "message": "Pin Amount was saved", "node_guid": N_PRINT, "pin": "Amount",
             "orphaned": True},
            {"severity": "warning", "message": "Unused", "node_guid": N_BEGIN}]}
        report = execute(context, self.BRANCH_BODY)
        self.assertEqual([(item.code, item.path, item.severity) for item in report.diagnostics], [
            ("Graph.OrphanedPinDefault", "/Body/UbergraphPages/0/Nodes/1", DiagnosticSeverity.WARNING),
            ("Graph.FinalizeMessage", "/Body/UbergraphPages/0/Nodes/0", DiagnosticSeverity.WARNING)])
        self.assertIn("'Amount'", report.diagnostics[0].message)

    def test_pruned_exec_node_warning_names_the_node(self):
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_branch.json")
        context.finalize_value = {"status": "warnings", "messages": [
            {"severity": "warning", "id": "PrunedExecInUse", "node_guid": N_PRINT,
             "message": "Print String was pruned because its Exec pin is not connected"}]}
        report = execute(context, self.BRANCH_BODY)
        self.assertEqual([(item.code, item.path, item.severity) for item in report.diagnostics], [
            ("Graph.PrunedNodeValueRead", "/Body/UbergraphPages/0/Nodes/1", DiagnosticSeverity.WARNING)])
        self.assertIn("'Print'", report.diagnostics[0].message)
        self.assertIn("was pruned because", report.diagnostics[0].message)

    def test_bare_compile_binds_document_nodes_on_demand(self):
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_branch.json")
        context.finalize_failure = [{"severity": "error", "message": "In use pin Amount", "node_guid": N_PRINT,
                                     "pin": "Amount", "orphaned": True}]
        graphs, roles = k2.decode_body(self.BRANCH_BODY, self_class=ACTOR_CLASS)
        executor = GraphExecutor(context, read_handle=object(), write_handle=object(), route_id="route.blueprint.v1",
                                 adapter=k2.K2_ADAPTER, self_class=ACTOR_CLASS)
        executor.binder = lambda bound: bound.bind_document(graphs, managed_roles=roles, role_paths=ROLE_PATHS)
        with self.assertRaises(AssetDocumentContractError) as caught:
            executor._op_graph_finalize(PlanOp("graph.finalize", "", "/Body", {}))
        self.assertEqual(caught.exception.diagnostic.path, "/Body/UbergraphPages/0/Nodes/1")
        self.assertEqual(mutations(context), ["graph.finalize.v1"])

    def test_bare_compile_binds_graphs_the_document_omits_to_their_extraction(self):
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_branch.json")
        context.finalize_failure = [{"severity": "error", "message": "In use pin Amount", "node_guid": N_BEGIN,
                                     "pin": "Amount", "orphaned": True}]
        executor = GraphExecutor(context, read_handle=object(), write_handle=object(), route_id="route.blueprint.v1",
                                 adapter=k2.K2_ADAPTER, self_class=ACTOR_CLASS)
        executor.binder = lambda bound: bound.bind_document(
            (), managed_roles=frozenset(), sections={role: section for section, role in k2.SECTIONS})
        with self.assertRaises(AssetDocumentContractError) as caught:
            executor._op_graph_finalize(PlanOp("graph.finalize", "", "/Body", {}))
        self.assertEqual(caught.exception.diagnostic.code, "Graph.OrphanedPinLinked")
        self.assertRegex(caught.exception.diagnostic.path, r"^/Body/UbergraphPages/0/Nodes/\d+$")
        self.assertNotIn(N_BEGIN, caught.exception.diagnostic.message)

    def test_extract_then_apply_is_a_no_op(self):
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_branch.json")
        executor = GraphExecutor(context, read_handle=object(), route_id="route.blueprint.v1", adapter=k2.K2_ADAPTER)
        live = executor.read_live()
        body = k2.encode_body(extract_graphs(live, sections=k2.SECTION_BY_ROLE, adapter=k2.K2_ADAPTER))
        context.calls.clear()
        report = execute(context, body)
        self.assertEqual(mutations(context), [])
        self.assertIsNone(report.finalize_status)

    def test_unlinked_ghost_event_is_reused_and_survives_extraction(self):
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
        report = execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Tick", "Action": "K2:Tick"}]}]})
        self.assertEqual(mutations(context), ["graph.node.spawn.v1", "graph.finalize.v1"])
        self.assertEqual(report.node_bindings[("UbergraphPages/EventGraph", "Tick")], "B0000000000000000000000000000003")
        body = k2.encode_body(extract_graphs(read_live(context), sections=k2.SECTION_BY_ROLE, adapter=k2.K2_ADAPTER))
        self.assertEqual([(node["Id"], node["Action"]) for node in body["UbergraphPages"][0]["Nodes"]],
                         [("Tick", "K2:Tick")])
        context.calls.clear()
        execute(context, body)
        self.assertEqual(mutations(context), [])

    def test_authored_ids_and_action_spellings_survive_extraction(self):
        # Extract wrote the editor's names (IfActive -> Branch) and the canonical keys, so the
        # next edit had to switch Ids. The executor records each node's Id and authored Action with the node.
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
        authored = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "IfActive", "Action": "K2:Branch"},
            {"Id": "SayHi", "Action": "K2:PrintString", "PinOverrides": [{"Pin": "InString", "DefaultValue": "hi"}]}],
            "Links": [{"From": "IfActive:then", "To": "SayHi:execute"}]}]}
        execute(context, authored)
        finalize = context.calls_of("graph.finalize.v1")[0]
        self.assertEqual(sorted((item["id"], item.get("action")) for item in finalize["node_meta"]),
                         [("IfActive", "K2:Branch"), ("SayHi", "K2:PrintString")])
        body = k2.encode_body(extract_graphs(read_live(context), sections=k2.SECTION_BY_ROLE, adapter=k2.K2_ADAPTER))
        graph = body["UbergraphPages"][0]
        self.assertEqual([(node["Id"], node["Action"]) for node in graph["Nodes"]],
                         [("IfActive", "K2:Branch"), ("SayHi", "K2:PrintString")])
        self.assertEqual(graph["Links"], [{"From": "IfActive:then", "To": "SayHi:execute"}])
        # Re-applying the extract is a no-op (nothing to record again); an edit by the same Ids changes that node.
        context.calls.clear()
        execute(context, body)
        self.assertEqual(mutations(context), [])
        graph["Nodes"][1]["PinOverrides"] = [{"Pin": "InString", "DefaultValue": "bye"}]
        execute(context, body)
        self.assertEqual(mutations(context), ["graph.pin.default.set.v1", "graph.finalize.v1"])
        self.assertNotIn("node_meta", context.calls_of("graph.finalize.v1")[0])

    def test_node_meta_of_a_large_graph_is_sent_in_batches(self):
        # One invocation carries at most 4096 values: a large Blueprint's node_meta failed the Create
        # (FS_BombField_Prototype round trip). The leading batches only record; the last one compiles.
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
        authored = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "IfActive", "Action": "K2:Branch"}, {"Id": "SayHi", "Action": "K2:PrintString"},
            {"Id": "SayBye", "Action": "K2:PrintString"}]}]}
        with mock.patch.object(executor_module, "NODE_META_BATCH", 2):
            report = execute(context, authored)
        calls = context.calls_of("graph.finalize.v1")
        self.assertEqual([(len(call["node_meta"]), call.get("compile")) for call in calls], [(2, False), (1, None)])
        self.assertEqual(report.finalize_status, context.finalize_value["status"])
        body = k2.encode_body(extract_graphs(read_live(context), sections=k2.SECTION_BY_ROLE, adapter=k2.K2_ADAPTER))
        self.assertEqual(sorted(node["Id"] for node in body["UbergraphPages"][0]["Nodes"]), ["IfActive", "SayBye", "SayHi"])

    def test_an_id_recorded_twice_or_for_another_action_is_not_used(self):
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
        execute(context, {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "A", "Action": "K2:Branch"}, {"Id": "B", "Action": "K2:Branch"}]}]})
        nodes = [node for graph in context.graphs.values() for node in graph["nodes"] if node.get("doc_id")]
        nodes[1]["doc_id"] = "A"  # a duplicated node (copy / paste) carries the same metadata
        nodes[0]["doc_action"] = "K2:PrintString"  # no longer names the node's action
        body = k2.encode_body(extract_graphs(read_live(context), sections=k2.SECTION_BY_ROLE, adapter=k2.K2_ADAPTER))
        ids = [node["Id"] for node in body["UbergraphPages"][0]["Nodes"]]
        self.assertEqual(len(set(ids)), len(ids))
        self.assertEqual(ids.count("A"), 1)
        self.assertNotIn("K2:PrintString", [node["Action"] for node in body["UbergraphPages"][0]["Nodes"]])

    def test_function_flags_are_set_through_the_flags_primitive(self):
        context = FakeGraphContext("graph.list.v1.k2_function.json", "graph.query.v1.k2_function_signature.json")
        body = {"FunctionGraphs": [{"Name": "AddHealth", "Signature": {
            "Inputs": [{"Name": "Amount", "Type": "float"}], "Outputs": [{"Name": "NewHealth", "Type": "float"}],
            "Pure": True, "Keywords": "heal"}, "Links": [{"From": "Entry:then", "To": "Result:execute"}]}]}
        execute(context, body)
        self.assertEqual(mutations(context), ["graph.function.flags.set.v1", "graph.finalize.v1"])
        self.assertEqual(context.calls_of("graph.function.flags.set.v1"), [{
            "schema_version": 1, "graph": {"graph_guid": "A0000000000000000000000000000003"},
            "flags": {"pure": True, "keywords": "heal"}}])
        context.calls.clear()
        execute(context, body)
        self.assertEqual(mutations(context), [])

    def test_paginated_query_results_are_merged(self):
        whole = read_live(FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_branch.json"))
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_branch.json", page_size=2)
        paged = read_live(context)
        self.assertEqual(paged, whole)
        self.assertEqual(len(paged.graph(G_EVENT).nodes), 5)
        offsets = [args.get("node_offset") for args in context.calls_of("graph.query.v1")]
        self.assertEqual(offsets, [None, 2, 4])
        # Re-queries after a mutation page as well, and extract -> apply stays a no-op.
        body = k2.encode_body(extract_graphs(paged, sections=k2.SECTION_BY_ROLE, adapter=k2.K2_ADAPTER))
        body["UbergraphPages"][0]["Nodes"][2]["PinOverrides"][0]["DefaultValue"] = "Paged"
        context.calls.clear()
        execute(context, body)
        self.assertEqual(mutations(context), ["graph.pin.default.set.v1", "graph.finalize.v1"])

    def test_result_too_large_falls_back_to_node_limit(self):
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_branch.json",
                                   too_large_above=64)
        live = read_live(context)
        self.assertEqual(len(live.graph(G_EVENT).nodes), 5)
        self.assertEqual([args.get("node_limit") for args in context.calls_of("graph.query.v1")], [None, 256, 128, 64])
        refused = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_branch.json",
                                   too_large_above=0)
        with self.assertRaises(AssetDocumentContractError) as caught:
            read_live(refused)
        self.assertEqual(caught.exception.diagnostic.code, "Graph.ResultTooLarge")

    def test_mutation_without_write_handle_is_refused(self):
        context = FakeGraphContext("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")
        graphs, roles = k2.decode_body({"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Branch", "Action": "K2:Branch"}]}]})
        executor = GraphExecutor(context, read_handle=object(), route_id="route.blueprint.v1", adapter=k2.K2_ADAPTER)
        with self.assertRaises(AssetDocumentContractError) as caught:
            executor.run(graphs, managed_roles=roles)
        self.assertEqual(caught.exception.diagnostic.code, "Graph.HandleUnavailable")
        self.assertEqual(caught.exception.diagnostic.path, "/Body/UbergraphPages/0/Nodes/0")


if __name__ == "__main__":
    unittest.main()
