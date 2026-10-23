# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from copy import deepcopy
import unittest

from asset_document.graphs.extract import extract_graphs
from asset_document.graphs.live import decode_live_asset
from asset_document.graphs.reconcile import reconcile
from asset_document.graphs.schemas import k2
from asset_document.tests.graph_support import (
    ACTOR_CLASS, N_BEGIN, N_BRANCH, N_ENTRY, N_PRINT, fixture, live_asset,
)


ALL_ROLES = frozenset(role for _, role in k2.SECTIONS)
CASES = (
    ("graph.query.v1.k2_empty_actor.json", "graph.list.v1.k2_empty_actor.json"),
    ("graph.query.v1.k2_branch.json", "graph.list.v1.k2_empty_actor.json"),
    ("graph.query.v1.k2_function_signature.json", "graph.list.v1.k2_function.json"),
)


def extract_body(live, hints=(), self_class=None):
    return k2.encode_body(extract_graphs(live, sections=k2.SECTION_BY_ROLE, hints=hints, adapter=k2.K2_ADAPTER,
                                         self_class=self_class))


def edited_live(query, listing, edit):
    """Decode a captured fixture after ``edit(query_value)`` changed it in place."""
    value = deepcopy(fixture(query))
    edit(value)
    return decode_live_asset(fixture(listing), value)


def find_node(value, guid):
    return next(node for graph in value["graphs"] for node in graph["nodes"] if node["node_guid"] == guid)


def assert_round_trip(test, live, self_class=None):
    graphs = extract_graphs(live, sections=k2.SECTION_BY_ROLE, adapter=k2.K2_ADAPTER, self_class=self_class)
    test.assertEqual(reconcile(graphs, live, managed_roles=ALL_ROLES, adapter=k2.K2_ADAPTER,
                               self_class=self_class).briefs(), [])
    decoded, roles = k2.decode_body(k2.encode_body(graphs), self_class=self_class)
    test.assertEqual(reconcile(decoded, live, managed_roles=roles, adapter=k2.K2_ADAPTER,
                               self_class=self_class).briefs(), [])


class GraphExtractTests(unittest.TestCase):
    def test_pin_overrides_restating_pin_defaults_compare_as_present_with_defaults(self):
        # PinOverrides equal to the pin default ("true" on PrintString bPrintToScreen, "2.0"
        # on Duration) failed the post-save check: extract omits default-valued pins.
        from asset_document.profiles.ublueprint_graph_compare import section_mismatch_reasons
        live = live_asset("graph.query.v1.k2_branch.json")
        authored = deepcopy(extract_body(live))
        graph = authored["UbergraphPages"][0]
        node = next(item for item in graph["Nodes"] if item.get("Action") == "K2:PrintString")
        node.setdefault("PinOverrides", []).extend([{"Pin": "bPrintToScreen", "DefaultValue": "true"},
                                                     {"Pin": "Duration", "DefaultValue": "2.0"}])
        sparse = extract_body(live)
        with_defaults = k2.encode_body(extract_graphs(live, sections=k2.SECTION_BY_ROLE, adapter=k2.K2_ADAPTER,
                                                      include_defaults=True))
        self.assertNotEqual(section_mismatch_reasons("UbergraphPages", authored["UbergraphPages"],
                                                     sparse["UbergraphPages"], self_class=None), {})
        self.assertEqual(section_mismatch_reasons("UbergraphPages", authored["UbergraphPages"],
                                                  with_defaults["UbergraphPages"], self_class=None), {})
        changed = deepcopy(authored)
        next(item for item in changed["UbergraphPages"][0]["Nodes"] if item.get("Action") == "K2:PrintString")[
            "PinOverrides"][-1]["DefaultValue"] = "3.0"
        self.assertNotEqual(section_mismatch_reasons("UbergraphPages", changed["UbergraphPages"],
                                                     with_defaults["UbergraphPages"], self_class=None), {})

    def test_defaults_on_unlinked_implicit_nodes_do_not_enter_the_compared_shape(self):
        # PC suite (AnimBlueprint): an unlinked implicit node (a function Result, an anim Output pose, a
        # transition result) holding only pin defaults must compare as it does sparse: a verify read with
        # defaults made it part of the graph shape and every re-applied extract mismatched.
        from asset_document.profiles.ublueprint_graph_compare import section_mismatch_reasons
        live = live_asset("graph.query.v1.k2_function_signature.json")
        sparse = extract_body(live)
        with_defaults = k2.encode_body(extract_graphs(live, sections=k2.SECTION_BY_ROLE, adapter=k2.K2_ADAPTER,
                                                      include_defaults=True))
        for body in (sparse, with_defaults):
            graph = next(item for item in body["FunctionGraphs"] if item["Name"] == "AddHealth")
            graph["Links"] = []
        self.assertEqual(section_mismatch_reasons("FunctionGraphs", sparse["FunctionGraphs"],
                                                  with_defaults["FunctionGraphs"], self_class=None), {})

    def test_extract_then_reconcile_against_same_live_is_empty(self):
        for query, listing in CASES:
            with self.subTest(query=query):
                # Directly and through the Body codec (what the sidecar stores and the AI edits).
                assert_round_trip(self, live_asset(query, listing))

    def test_extracted_body_shape_uses_alias_ids_and_skips_ghost_nodes(self):
        body = extract_body(live_asset("graph.query.v1.k2_branch.json"))
        graph = body["UbergraphPages"][0]
        # Ghost ActorBeginOverlap/Tick (extras.ghost) are not extracted; actions use their alias form.
        self.assertEqual([(node["Id"], node["Action"]) for node in graph["Nodes"]],
                         [("BeginPlay", "K2:BeginPlay"), ("Branch", "K2:Branch"), ("PrintString", "K2:PrintString")])
        self.assertEqual(graph["Links"], [{"From": "BeginPlay:then", "To": "Branch:execute"},
                                          {"From": "Branch:then", "To": "PrintString:execute"}])
        printer = graph["Nodes"][2]
        self.assertEqual(printer, {
            "Id": "PrintString", "Action": "K2:PrintString", "NodeGuid": N_PRINT, "Position": {"X": 640, "Y": 0},
            "PinOverrides": [{"Pin": "InString", "DefaultValue": "Hello from AssetDocument"}]})
        self.assertEqual([graph["Name"] for graph in body["FunctionGraphs"]], ["UserConstructionScript"])
        # The reserved id "Entry" implies the implicit role, so "Implicit" is not repeated.
        self.assertEqual(body["FunctionGraphs"][0]["Nodes"], [{"Id": "Entry",
                                                                "NodeGuid": "C0000000000000000000000000000001",
                                                                "Position": {"X": 0, "Y": 0}}])

    def test_sidecar_ids_are_reused_by_guid(self):
        hints, _ = k2.decode_body({"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Start", "Action": "K2:BeginPlay", "NodeGuid": N_BEGIN},
            {"Id": "Check", "Action": "K2:Branch", "NodeGuid": N_BRANCH}]}]})
        graph = extract_body(live_asset("graph.query.v1.k2_branch.json"), hints)["UbergraphPages"][0]
        self.assertEqual([node["Id"] for node in graph["Nodes"]], ["Start", "Check", "PrintString"])
        self.assertEqual(graph["Nodes"][1]["Action"], "K2:Branch")
        self.assertEqual(graph["Links"][0], {"From": "Start:then", "To": "Check:execute"})

    def test_function_signature_is_extracted_from_entry_and_result(self):
        body = extract_body(live_asset("graph.query.v1.k2_function_signature.json", "graph.list.v1.k2_function.json"))
        function = next(graph for graph in body["FunctionGraphs"] if graph["Name"] == "AddHealth")
        # Blueprint "Float" is real/double -> "float"; the autogenerated "0.0" default and the
        # default signature_flags are not written.
        self.assertEqual(function["Signature"], {"Inputs": [{"Name": "Amount", "Type": "float"}],
                                                 "Outputs": [{"Name": "NewHealth", "Type": "float"}]})
        self.assertEqual(function["Links"], [{"From": "Entry:then", "To": "Result:execute"}])
        construction = next(graph for graph in body["FunctionGraphs"] if graph["Name"] == "UserConstructionScript")
        self.assertNotIn("Signature", construction)

    def test_non_default_flags_and_authored_defaults_are_extracted(self):
        def edit(value):
            entry = find_node(value, N_ENTRY)
            entry["extras"]["signature_flags"].update(pure=True, access="private", category="Health")
            amount = next(pin for pin in entry["pins"] if pin["name"] == "Amount")
            amount.update(default_value="5.0", is_autogenerated_default=False)
        live = edited_live("graph.query.v1.k2_function_signature.json", "graph.list.v1.k2_function.json", edit)
        function = next(graph for graph in extract_body(live)["FunctionGraphs"] if graph["Name"] == "AddHealth")
        self.assertEqual(function["Signature"], {
            "Inputs": [{"Name": "Amount", "Type": "float", "Default": "5.0"}],
            "Outputs": [{"Name": "NewHealth", "Type": "float"}],
            "Pure": True, "Access": "private", "Category": "Health"})
        assert_round_trip(self, live)

    def test_single_precision_pins_promotable_operators_and_field_owners(self):
        health = f"K2:VariableGet:{ACTOR_CLASS}:Health"

        def edit(value):
            entry = find_node(value, N_ENTRY)
            next(pin for pin in entry["pins"] if pin["name"] == "Amount")["type"]["sub_category"] = "float"
        live = edited_live("graph.query.v1.k2_function_signature.json", "graph.list.v1.k2_function.json", edit)
        body = extract_body(live, self_class=ACTOR_CLASS)
        function = next(graph for graph in body["FunctionGraphs"] if graph["Name"] == "AddHealth")
        self.assertEqual(function["Signature"]["Inputs"], [{"Name": "Amount", "Type": "float32"}])
        assert_round_trip(self, live)

        def edit_branch(value):
            find_node(value, N_BRANCH)["action_key"] = "K2:PromotableOperator:Multiply"
            find_node(value, N_PRINT)["action_key"] = "K2:ForEachElementInEnum:/Script/Engine.EMovementMode"
            find_node(value, N_BEGIN)["action_key"] = health
        live = edited_live("graph.query.v1.k2_branch.json", "graph.list.v1.k2_empty_actor.json", edit_branch)
        nodes = extract_body(live, self_class=ACTOR_CLASS)["UbergraphPages"][0]["Nodes"]
        self.assertEqual([(node["Id"], node["Action"]) for node in nodes], [
            ("Get_Health", "K2:Get:Health"), ("Multiply", "K2:Multiply"),
            ("ForEachElementInEnum_EMovementMode", "K2:ForEachElementInEnum:/Script/Engine.EMovementMode")])
        assert_round_trip(self, live, ACTOR_CLASS)
        # Without the asset class the canonical owner is kept.
        self.assertEqual(extract_body(live)["UbergraphPages"][0]["Nodes"][0]["Action"], health)


if __name__ == "__main__":
    unittest.main()
