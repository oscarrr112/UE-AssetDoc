# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from copy import deepcopy
import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.graphs.executor import GraphExecutor
from asset_document.graphs.schemas import k2
from asset_document.profiles import blueprint_dispatchers as dispatchers
from asset_document.profiles.ublueprint_graph_compare import section_mismatches
from asset_document.tests.graph_support import (
    ACTOR_CLASS, BEGIN_PLAY, EXEC, G_EVENT, FakeGraphContext, TEMPLATES, ok, pin, ptype,
)

ROLE_PATHS = {role: f"/Body/{section}" for section, role in k2.SECTIONS}
DELEGATE = ptype("delegate")
ASSIGN = f"K2:AssignDelegate:{ACTOR_CLASS}:OnDied"
DISPATCHER_EVENT = f"K2:CustomEvent:{ACTOR_CLASS}:OnDied"
ACTOR = ptype("object", "", "/Script/Engine.Actor")


def code_of(call):
    try:
        call()
    except AssetDocumentContractError as error:
        return error.diagnostic.code, error.diagnostic.path
    return None


def custom_event(name, *inputs):
    return {"action_key": "K2:CustomEvent", "implicit_role": None, "can_add_pin": False, "title": name,
            "extras": {"name": name, "user_pins": [item["name"] for item in inputs]},
            "pins": [pin("OutputDelegate", "output", DELEGATE), pin("then", "output", EXEC), *inputs]}


class DispatcherGraphContext(FakeGraphContext):
    """Adds the editor's dispatcher nodes: Assign places a linked "<Dispatcher>_Event" custom event."""

    def __init__(self):
        super().__init__("graph.list.v1.k2_empty_actor.json", "graph.query.v1.k2_empty_actor.json")

    def _graph_node_spawn_v1(self, args):
        key, graph = args["action_key"], self.graphs[args["graph"]["graph_guid"]]
        if key == ASSIGN:
            assign = self._make_node({"action_key": key, "implicit_role": None, "can_add_pin": False,
                                      "pins": [pin("execute", "input", EXEC), pin("then", "output", EXEC),
                                               pin("Delegate", "input", DELEGATE)]}, args.get("position"))
            event = self._make_node(custom_event(self._unique("OnDied_Event"), pin("Killer", "output", ACTOR)), None)
            graph["nodes"] += [assign, event]
            graph["links"].append((event["node_guid"], "OutputDelegate", assign["node_guid"], "Delegate"))
            return ok({"schema_version": 1, "node_guid": assign["node_guid"], "created_node_guids": [event["node_guid"]],
                       "created_graph_guids": [], "reused": False, "node": deepcopy(assign)})
        if key in ("K2:CustomEvent", DISPATCHER_EVENT):
            name = self._unique("OnDied_Event" if key == DISPATCHER_EVENT else "CustomEvent")
            inputs = (pin("Killer", "output", ACTOR),) if key == DISPATCHER_EVENT else ()
            node = self._make_node(custom_event(name, *inputs), args.get("position"))
            graph["nodes"].append(node)
            return ok({"schema_version": 1, "node_guid": node["node_guid"], "created_node_guids": [],
                       "created_graph_guids": [], "reused": False, "node": deepcopy(node)})
        return super()._graph_node_spawn_v1(args)

    def _unique(self, base):
        names = {node["extras"].get("name") for graph in self.graphs.values() for node in graph["nodes"]}
        name, index = base, 0
        while name in names:
            name, index = f"{base}_{index}", index + 1
        return name

    def _graph_node_rename_v1(self, args):
        _, node = self._find(args["node_guid"])
        node["extras"]["name"] = node["title"] = args["name"]
        return ok({"schema_version": 1, "name": args["name"]})

    def custom_events(self):
        return sorted(node["extras"]["name"] for node in self.graphs[G_EVENT]["nodes"]
                      if node["action_key"] == "K2:CustomEvent")


def execute(context, nodes, links):
    graphs, roles = k2.decode_body({"UbergraphPages": [{"Name": "EventGraph", "Nodes": nodes, "Links": links}]},
                                   self_class=ACTOR_CLASS)
    executor = GraphExecutor(context, read_handle=object(), write_handle=object(), route_id="route.blueprint.v1",
                             adapter=k2.K2_ADAPTER, self_class=ACTOR_CLASS)
    return executor.run(graphs, managed_roles=roles, role_paths=ROLE_PATHS)


def mutations(context):
    return [name for name in context.primitive_sequence() if name not in ("graph.list.v1", "graph.query.v1")]


class CanonicalTests(unittest.TestCase):
    def test_rows_omit_defaults_and_normalise_types(self):
        body = {"EventDispatchers": [
            {"Name": "OnDied", "Inputs": [{"Name": "Killer", "Type": "object:/Script/Engine.Actor"},
                                          {"Name": "Amount", "Type": "double"}],
             "Category": "Default", "Tooltip": ""},
            {"Name": "OnHit", "Inputs": [], "Category": "Combat", "Tooltip": "Hit."}]}
        self.assertEqual(dispatchers.canonical(body), [
            {"Name": "OnDied", "Inputs": [{"Name": "Killer", "Type": "object:/Script/Engine.Actor"},
                                          {"Name": "Amount", "Type": "float"}]},
            {"Name": "OnHit", "Category": "Combat", "Tooltip": "Hit."}])
        self.assertIsNone(dispatchers.canonical({}))
        self.assertEqual(dispatchers.canonical({"EventDispatchers": []}), [])

    def test_legacy_anim_key_is_an_alias(self):
        self.assertEqual(dispatchers.canonical({"DelegateSignatureGraphs": [{"Name": "OnLanded"}]}), [{"Name": "OnLanded"}])
        graph_shape = {"Name": "OnLanded", "GraphGuid": "0" * 32, "Schema": "K2",
                       "Signature": {"Inputs": [{"Name": "Speed", "Type": "float"}], "Outputs": []},
                       "Nodes": [{"Id": "Entry", "Implicit": "entry"}, {"Id": "Result"}], "Links": []}
        self.assertEqual(dispatchers.canonical({"DelegateSignatureGraphs": [graph_shape]}),
                         [{"Name": "OnLanded", "Inputs": [{"Name": "Speed", "Type": "float"}]}])

    def test_invalid_rows_fail_with_stable_codes(self):
        for body, code in (
                ({"EventDispatchers": [{"Name": "A"}, {"Name": "a"}]}, "UBlueprint.DuplicateName"),
                ({"EventDispatchers": [{"Name": "A", "Replicated": True}]}, "UBlueprint.UnknownField"),
                ({"EventDispatchers": [{"Name": "A", "Inputs": [{"Name": "X", "Type": "int", "Default": "1"}]}]},
                 "UBlueprint.UnknownField"),
                ({"EventDispatchers": [{"Name": "A", "Inputs": [{"Name": "X", "Type": "int"},
                                                                {"Name": "x", "Type": "int"}]}]},
                 "UBlueprint.DuplicateName"),
                ({"EventDispatchers": [{"Name": ""}]}, "UBlueprint.InvalidBodyRegion"),
                ({"EventDispatchers": [], "DelegateSignatureGraphs": []}, "UBlueprint.DuplicateRegion"),
                ({"DelegateSignatureGraphs": [{"Name": "A", "Nodes": [{"Id": "C", "Action": "K2:Branch"}]}]},
                 "UBlueprint.InvalidBodyRegion")):
            with self.subTest(body=body):
                self.assertEqual(code_of(lambda: dispatchers.canonical(body))[0], code)

    def test_a_dispatcher_cannot_share_a_variable_name(self):
        body = {"Variables": [{"Name": "OnDied", "Type": "int"}], "EventDispatchers": [{"Name": "ondied"}]}
        self.assertEqual(code_of(lambda: dispatchers.check_names(body)),
                         ("UBlueprint.DuplicateName", "/Body/EventDispatchers/0/Name"))


class PlanTests(unittest.TestCase):
    QUERY = {"dispatchers": [
        {"name": "OnDied", "guid": "1" * 32, "has_signature_graph": True, "category": "Default", "tooltip": "",
         "inputs": [{"name": "Killer", "type": ptype("object", "", "/Script/Engine.Actor")}]},
        {"name": "OnOld", "guid": "2" * 32, "has_signature_graph": True, "category": "Default", "tooltip": "",
         "inputs": []}]}

    def test_extract_shape_and_unchanged_rows_plan_nothing(self):
        rows = dispatchers.from_query(self.QUERY)
        self.assertEqual(rows, [{"Name": "OnDied", "Inputs": [{"Name": "Killer", "Type": "object:/Script/Engine.Actor"}]},
                                {"Name": "OnOld"}])
        body = {"EventDispatchers": dispatchers.canonical({"EventDispatchers": rows})}
        self.assertEqual(dispatchers.plan_removes(body, self.QUERY) + dispatchers.plan_sets(body, self.QUERY), [])
        self.assertEqual(dispatchers.diff(body, {"EventDispatchers": rows}), [])
        self.assertEqual(dispatchers.plan_sets({}, self.QUERY), [])

    def test_unlisted_are_removed_new_are_created_and_changes_are_minimal(self):
        body = {"EventDispatchers": [{"Name": "OnDied", "Category": "Combat"}, {"Name": "OnNew", "Tooltip": "T"}]}
        self.assertEqual(dispatchers.plan_removes(body, self.QUERY),
                         [("blueprint.member.dispatcher.remove.v1", {"name": "OnOld"}, "/Body/EventDispatchers")])
        self.assertEqual(dispatchers.plan_sets(body, self.QUERY), [
            ("blueprint.member.dispatcher.set.v1", {"name": "OnDied", "inputs": [], "category": "Combat"},
             "/Body/EventDispatchers/0"),
            ("blueprint.member.dispatcher.set.v1", {"name": "OnNew", "tooltip": "T"}, "/Body/EventDispatchers/1")])
        live = {"EventDispatchers": dispatchers.from_query(self.QUERY)}
        self.assertEqual([item.path for item in dispatchers.diff(body, live)], ["/Body/EventDispatchers"])


class GraphNodeTests(unittest.TestCase):
    """Dispatcher nodes in event graphs: the Assign node's own event is adopted, not duplicated."""

    ASSIGN_NODE = {"Id": "Assign", "Action": "K2:AssignDelegate:OnDied"}
    EVENT_NODE = {"Id": "Ev", "Action": "K2:CustomEvent", "Name": "OnDied_Event",
                  "Inputs": [{"Name": "Killer", "Type": "object:/Script/Engine.Actor"}]}
    LINKS = [{"From": "Begin:then", "To": "Assign:execute"}, {"From": "Ev:OutputDelegate", "To": "Assign:Delegate"}]

    def test_assign_companion_event_is_adopted(self):
        context = DispatcherGraphContext()
        report = execute(context, [{"Id": "Begin", "Action": "K2:BeginPlay"}, self.ASSIGN_NODE, self.EVENT_NODE],
                         self.LINKS)
        self.assertEqual(context.custom_events(), ["OnDied_Event"])
        # BeginPlay reuses the ghost event; the adopted event is only placed (laid out with the others once
        # the graph holds them all).
        self.assertEqual(mutations(context), ["graph.node.spawn.v1", "graph.pin.connect.v1"]
                         + ["graph.node.layout.set.v1"] * 3 + ["graph.finalize.v1"])
        self.assertEqual(context.calls_of("graph.node.spawn.v1")[0]["action_key"], ASSIGN)
        self.assertIn(("UbergraphPages/EventGraph", "Ev"), report.node_bindings)

    def test_event_listed_first_keeps_the_document_node(self):
        context = DispatcherGraphContext()
        execute(context, [{"Id": "Begin", "Action": "K2:BeginPlay"}, self.EVENT_NODE, self.ASSIGN_NODE], self.LINKS)
        # Spawned on its own (and given its Inputs), the companion of the later Assign is removed.
        self.assertEqual(context.custom_events(), ["OnDied_Event"])
        self.assertIn("graph.node.remove.v1", mutations(context))
        self.assertIn("graph.node.user_pins.set.v1", mutations(context))

    def test_companion_the_document_does_not_list_is_removed(self):
        context = DispatcherGraphContext()
        execute(context, [{"Id": "Begin", "Action": "K2:BeginPlay"}, self.ASSIGN_NODE],
                [{"From": "Begin:then", "To": "Assign:execute"}])
        self.assertEqual(context.custom_events(), [])

    def test_dispatcher_event_action_spawns_a_named_event(self):
        context = DispatcherGraphContext()
        execute(context, [{"Id": "Ev", "Action": "K2:CustomEvent:OnDied"}], [])
        self.assertEqual(context.calls_of("graph.node.spawn.v1")[0]["action_key"], DISPATCHER_EVENT)
        self.assertEqual(context.custom_events(), ["OnDied_Event"])
        self.assertEqual(context.calls_of("graph.node.rename.v1"), [])


class CompareTests(unittest.TestCase):
    def graph(self, *nodes):
        return [{"Name": "EventGraph", "Nodes": list(nodes), "Links": []}]

    def test_dispatcher_event_compares_as_the_custom_event_it_spawns(self):
        authored = self.graph({"Id": "E", "Action": "K2:CustomEvent:OnDied"})
        extracted = self.graph({"Id": "N1", "Action": "K2:CustomEvent", "Name": "OnDied_Event",
                                "Inputs": [{"Name": "Killer", "Type": "int"}]})
        self.assertEqual(section_mismatches("UbergraphPages", authored, extracted, self_class=ACTOR_CLASS), [])

    def test_stated_custom_event_inputs_are_compared(self):
        extracted = self.graph({"Id": "N1", "Action": "K2:CustomEvent", "Name": "Ping",
                                "Inputs": [{"Name": "Count", "Type": "int"}]})
        same = self.graph({"Id": "E", "Action": "K2:CustomEvent", "Name": "Ping", "Inputs": [{"Name": "Count", "Type": "int"}]})
        other = self.graph({"Id": "E", "Action": "K2:CustomEvent", "Name": "Ping", "Inputs": [{"Name": "Count", "Type": "bool"}]})
        self.assertEqual(section_mismatches("UbergraphPages", same, extracted, self_class=ACTOR_CLASS), [])
        self.assertEqual(section_mismatches("UbergraphPages", other, extracted, self_class=ACTOR_CLASS), ["EventGraph"])

    def test_inputs_only_on_custom_events(self):
        body = {"UbergraphPages": self.graph({"Id": "B", "Action": "K2:Branch", "Inputs": []})}
        self.assertEqual(code_of(lambda: k2.decode_body(body, self_class=ACTOR_CLASS))[0], "Graph.UnknownField")


if __name__ == "__main__":
    unittest.main()
