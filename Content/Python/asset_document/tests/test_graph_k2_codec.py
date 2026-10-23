# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.graphs.actions import actions_equal, normalize_action, preferred_alias
from asset_document.graphs.model import PinType
from asset_document.graphs.schemas import k2
from asset_document.tests.graph_support import ACTOR_CLASS, PRINT


def error_of(test, call):
    with test.assertRaises(AssetDocumentContractError) as caught:
        call()
    return caught.exception.diagnostic.code, caught.exception.diagnostic.path


class ActionAliasTests(unittest.TestCase):
    def test_aliases_and_canonical_keys(self):
        cases = {
            "K2:Branch": "K2:IfThenElse",
            "k2:branch": "K2:IfThenElse",
            "K2:Sequence": "K2:ExecutionSequence",
            "K2:PrintString": PRINT,
            "K2:BeginPlay": "K2:Event:/Script/Engine.Actor:ReceiveBeginPlay",
            "K2:Self": "K2:Self",
            "K2:K2Node_IfThenElse": "K2:IfThenElse",
            "K2:ForEachLoop": "K2:MacroInstance:/Engine/EditorBlueprintResources/StandardMacros.StandardMacros:ForEachLoop",
            "K2:Get:Health": f"K2:VariableGet:{ACTOR_CLASS}:Health",
            "K2:VariableSet:Self:Health": f"K2:VariableSet:{ACTOR_CLASS}:Health",
            "K2:Cast:/Script/Engine.Pawn": "K2:DynamicCast:/Script/Engine.Pawn",
            "K2:Multiply": "K2:PromotableOperator:Multiply",
            "K2:Add": "K2:PromotableOperator:Add",
            "K2:Subtract": "K2:PromotableOperator:Subtract",
            "K2:Divide": "K2:PromotableOperator:Divide",
            "K2:Equal": "K2:PromotableOperator:EqualEqual",
            "K2:ForEachElementInEnum:/Script/Engine.EMovementMode": "K2:ForEachElementInEnum:/Script/Engine.EMovementMode",
            "K2:ComponentBoundEvent:/Script/Engine.PrimitiveComponent:OnComponentBeginOverlap@CollisionComp":
                "K2:ComponentBoundEvent:/Script/Engine.PrimitiveComponent:OnComponentBeginOverlap@CollisionComp",
            "BT:Task:/Script/AIModule.BTTask_MoveTo": "BT:Task:/Script/AIModule.BTTask_MoveTo",
            "SM:State": "SM:State",
        }
        for alias, expected in cases.items():
            with self.subTest(alias=alias):
                self.assertEqual(normalize_action(alias, self_class=ACTOR_CLASS), expected)
                self.assertEqual(normalize_action(expected, self_class=ACTOR_CLASS), expected)

    def test_preferred_alias_is_the_inverse_of_normalization(self):
        cases = {
            "K2:IfThenElse": "K2:Branch",
            PRINT: "K2:PrintString",
            "K2:ExecutionSequence": "K2:Sequence",
            "K2:Event:/Script/Engine.Actor:ReceiveBeginPlay": "K2:BeginPlay",
            "K2:PromotableOperator:Multiply": "K2:Multiply",
            "K2:PromotableOperator:EqualEqual": "K2:Equal",
            f"K2:VariableGet:{ACTOR_CLASS}:Health": "K2:Get:Health",
            f"K2:CallFunction:{ACTOR_CLASS}:Heal": "K2:CallFunction:Self:Heal",
            "K2:DynamicCast:/Script/Engine.Pawn": "K2:DynamicCast:/Script/Engine.Pawn",
            f"{PRINT}#0A1B2C3D": f"{PRINT}#0A1B2C3D",
        }
        for canonical, alias in cases.items():
            with self.subTest(canonical=canonical):
                self.assertEqual(preferred_alias(canonical, self_class=ACTOR_CLASS), alias)
                self.assertEqual(normalize_action(alias, self_class=ACTOR_CLASS), canonical)
        self.assertEqual(preferred_alias(f"K2:VariableGet:{ACTOR_CLASS}:Health"), f"K2:VariableGet:{ACTOR_CLASS}:Health")

    def test_a_member_name_keeps_its_trailing_space(self):
        # A variable named "Follow Player " (BP_FluidSim_01); white space around a key without a member goes.
        self.assertEqual(normalize_action("K2:Get:Follow Player ", self_class=ACTOR_CLASS),
                         f"K2:VariableGet:{ACTOR_CLASS}:Follow Player ")
        self.assertEqual(normalize_action(" K2:Branch "), "K2:IfThenElse")

    def test_own_class_spelled_with_other_case_is_self(self):
        # Package "DatasmithArealightMesh", class "DatasmithAreaLightMesh_C" (DatasmithContent).
        own = "/DatasmithContent/Datasmith/DatasmithArealightMesh.DatasmithArealightMesh_C"
        live = "/DatasmithContent/Datasmith/DatasmithArealightMesh.DatasmithAreaLightMesh_C"
        self.assertEqual(preferred_alias(f"K2:CallFunction:{live}:LightColor", self_class=own),
                         "K2:CallFunction:Self:LightColor")
        self.assertEqual(preferred_alias(f"K2:VariableGet:{live}:Color", self_class=own), "K2:Get:Color")

    def test_invalid_keys_and_self_without_class(self):
        for value in ("", "IfThenElse", "K2:", 3):
            with self.subTest(value=value):
                self.assertEqual(error_of(self, lambda: normalize_action(value))[0], "Graph.InvalidAction")
        self.assertEqual(error_of(self, lambda: normalize_action("K2:Get:Health"))[0], "Graph.InvalidAction")

    def test_disambiguated_live_keys_match_plain_desired_keys(self):
        self.assertTrue(actions_equal("K2:CallFunction:/X.Y:F", "K2:CallFunction:/X.Y:F#0A1B2C3D"))
        self.assertFalse(actions_equal("K2:CallFunction:/X.Y:F#FFFFFFFF", "K2:CallFunction:/X.Y:F#0A1B2C3D"))


class K2CodecTests(unittest.TestCase):
    def test_decode_compact_and_object_links_with_reserved_endpoints(self):
        graphs, roles = k2.decode_body({"FunctionGraphs": [{"Name": "F", "Nodes": [{"Id": "Print", "Action": "K2:PrintString"}],
            "Links": [{"From": "Entry:then", "To": {"Node": "Print", "Pin": "execute"}},
                      {"From": "Print:then", "To": "Result:execute"}]}]})
        graph = graphs[0]
        self.assertEqual(roles, frozenset({"function"}))
        self.assertEqual((graph.id, graph.role, graph.path), ("FunctionGraphs/F", "function", "/Body/FunctionGraphs/0"))
        self.assertEqual([(node.id, node.implicit) for node in graph.nodes],
                         [("Print", None), ("Entry", "entry"), ("Result", "result")])
        self.assertEqual(graph.nodes[0].action, PRINT)

    def test_round_trip_encode_decode(self):
        source = [{"Name": "AddHealth", "GraphGuid": "a" * 32,
                   "Signature": {"Pure": True, "Access": "Protected", "Keywords": "hp",
                                 "Inputs": [{"Name": "Amount", "Type": "float", "Default": "1.0"},
                                            {"Name": "Target", "Type": "object:/Script/Engine.Actor"},
                                            {"Name": "Tags", "Type": "array:name"},
                                            {"Name": "Hit", "Type": {"Category": "struct", "SubCategoryObject":
                                                                     "/Script/Engine.HitResult", "IsRef": True}}],
                                 "Outputs": [{"Name": "Ok", "Type": "bool"}]},
                   "Nodes": [{"Id": "Branch", "Action": "K2:IfThenElse", "NodeGuid": "B" * 32,
                              "Position": {"X": 10, "Y": -5.5}, "Comment": "c",
                              "PinOverrides": [{"Pin": "Condition", "DefaultValue": "false"}],
                              "Properties": {"bFoo": True}, "TemplateProperties": {"Bar": [1, 2]}},
                             {"Id": "Ev", "Action": "K2:CustomEvent", "Name": "OnPing", "Extras": {"x": 1}},
                             {"Id": "Sel", "Action": "K2:Select", "PinTypes": {"Index": "byte:/Script/Engine.EAxis"}}],
                   "Links": [{"From": "Entry:then", "To": "Branch:execute"}],
                   "Subgraphs": [{"Name": "Inner", "Owner": "Branch", "Role": "state", "Nodes": [], "Links": []}]}]
        graphs = k2.decode_section("FunctionGraphs", source)
        encoded = k2.encode_body(graphs)["FunctionGraphs"]
        self.assertEqual(k2.decode_section("FunctionGraphs", encoded), graphs)
        self.assertEqual(encoded[0]["GraphGuid"], "A" * 32)
        self.assertEqual(encoded[0]["Signature"]["Inputs"][3]["Type"],
                         {"Category": "struct", "SubCategoryObject": "/Script/Engine.HitResult", "IsRef": True})
        self.assertEqual(encoded[0]["Signature"]["Inputs"][2]["Type"], "array:name")
        self.assertEqual(graphs[0].signature.flags, {"pure": True, "access": "protected", "keywords": "hp"})
        self.assertEqual({key: encoded[0]["Signature"][key] for key in ("Pure", "Access", "Keywords")},
                         {"Pure": True, "Access": "protected", "Keywords": "hp"})
        self.assertEqual(encoded[0]["Nodes"][0]["Position"], {"X": 10, "Y": -5.5})
        self.assertEqual(encoded[0]["Nodes"][2]["PinTypes"], {"Index": "byte:/Script/Engine.EAxis"})
        self.assertEqual(graphs[0].nodes[2].pin_type("index"), PinType("byte", "", "/Script/Engine.EAxis"))

    def test_pin_type_shorthands(self):
        # UE5 Blueprint "Float" is real/double; single precision is float32.
        self.assertEqual(k2.decode_pin_type("float", "/T"), PinType("real", "double"))
        self.assertEqual(k2.decode_pin_type("double", "/T"), PinType("real", "double"))
        self.assertEqual(k2.decode_pin_type("float32", "/T"), PinType("real", "float"))
        self.assertEqual(k2.decode_pin_type("array:float32", "/T"), PinType("real", "float", container="array"))
        self.assertEqual(k2.encode_pin_type(PinType("real", "double")), "float")
        self.assertEqual(k2.encode_pin_type(PinType("real", "float")), "float32")
        self.assertEqual(k2.encode_pin_type(PinType("real", "double", container="set")), "set:float")
        self.assertEqual(k2.encode_pin_type(PinType("int")), "int")
        self.assertEqual(k2.decode_pin_type("set:int", "/T"), PinType("int", container="set"))
        self.assertEqual(k2.decode_pin_type("byte:/Script/Engine.ECollisionChannel", "/T"),
                         PinType("byte", "", "/Script/Engine.ECollisionChannel"))
        self.assertEqual(error_of(self, lambda: k2.decode_pin_type("vector", "/T")), ("Graph.InvalidPinType", "/T"))

    def test_map_pin_types_carry_their_value_type(self):
        # A function's map input (Landmass_FunctionLibrary): the key type plus ValueType, both ways.
        value = {"Category": "name", "Container": "map", "ValueType": "float"}
        decoded = k2.decode_pin_type(value, "/T")
        self.assertEqual(decoded, PinType("name", container="map", value_type=PinType("real", "double")))
        self.assertEqual(k2.encode_pin_type(decoded), value)
        self.assertEqual(decoded.to_dto()["value_type"],
                         {"category": "real", "sub_category": "double", "sub_category_object": ""})
        from asset_document.graphs.live import decode_pin_type_dto
        self.assertEqual(decode_pin_type_dto(dict(decoded.to_dto(), is_ref=False, is_const=False), "/T"), decoded)
        # Only a map has a value type, and the value type is a plain type.
        self.assertEqual(error_of(self, lambda: k2.decode_pin_type({"Category": "name", "ValueType": "float"}, "/T"))[0],
                         "Graph.InvalidPinType")
        self.assertEqual(error_of(self, lambda: k2.decode_pin_type(
            {"Category": "name", "Container": "map", "ValueType": "array:float"}, "/T"))[0], "Graph.InvalidPinType")

    def test_diagnostics_point_at_the_authored_location(self):
        cases = (
            ({"Name": "G", "Nodes": [{"Id": "A", "Action": "K2:Branch", "Bogus": 1}]},
             ("Graph.UnknownField", "/Body/UbergraphPages/0/Nodes/0/Bogus")),
            ({"Name": "G", "Nodes": [{"Id": "A"}]}, ("Graph.NodeActionMissing", "/Body/UbergraphPages/0/Nodes/0/Action")),
            ({"Name": "G", "Nodes": [{"Id": "A", "Action": "Branch"}]},
             ("Graph.InvalidAction", "/Body/UbergraphPages/0/Nodes/0/Action")),
            ({"Name": "G", "Nodes": [{"Id": "A", "Action": "K2:Branch"}], "Links": [{"From": "A:then", "To": "B:execute"}]},
             ("Graph.UnknownNode", "/Body/UbergraphPages/0/Links/0/To")),
            ({"Name": "G", "Nodes": [{"Id": "A", "Action": "K2:Branch"}], "Links": [{"From": "Athen", "To": "A:x"}]},
             ("Graph.InvalidLink", "/Body/UbergraphPages/0/Links/0/From")),
            ({"Name": "G", "Nodes": [{"Id": "A", "Action": "K2:Branch"}, {"Id": "A", "Action": "K2:Branch"}]},
             ("Graph.DuplicateNodeId", "/Body/UbergraphPages/0/Nodes/1/Id")),
            ({"Name": "G", "Nodes": [{"Id": "A", "Action": "K2:Branch", "NodeGuid": "xyz"}]},
             ("Graph.InvalidNodeGuid", "/Body/UbergraphPages/0/Nodes/0/NodeGuid")),
            ({"Name": "G", "Nodes": [{"Id": "A", "Action": "K2:Branch", "PinOverrides": [{"Pin": "C"}]}]},
             ("Graph.InvalidPinOverride", "/Body/UbergraphPages/0/Nodes/0/PinOverrides/0")),
            ({"Name": "G", "Nodes": [{"Id": "A", "Action": "K2:Branch", "Class": "/Script/BlueprintGraph.K2Node_Self"}]},
             ("Graph.ConflictingAction", "/Body/UbergraphPages/0/Nodes/0/Class")),
            ({"Name": "G", "Nodes": [{"Id": "A", "Class": "/Script/BlueprintGraph.K2Node_Event",
                                      "Member": {"Kind": "DefinitionRef", "Id": "X"}}]},
             ("Graph.LegacyNodeUnsupported", "/Body/UbergraphPages/0/Nodes/0/Member")),
            ({"Name": "G", "Kind": "event"}, ("Graph.UnknownField", "/Body/UbergraphPages/0/Kind")),
            ({"Name": "G", "Signature": {"Inputs": [{"Name": "A"}]}},
             ("Graph.MissingField", "/Body/UbergraphPages/0/Signature/Inputs/0/Type")),
        )
        for graph, expected in cases:
            with self.subTest(expected=expected):
                self.assertEqual(error_of(self, lambda graph=graph: k2.decode_section("UbergraphPages", [graph])), expected)
        self.assertEqual(error_of(self, lambda: k2.decode_section("UbergraphPages", [{"Name": "G"}, {"Name": "g"}])),
                         ("Graph.DuplicateGraphName", "/Body/UbergraphPages/1/Name"))

    def test_canonical_section_is_order_insensitive(self):
        graph = {"Name": "G", "Nodes": [{"Id": "B", "Action": "K2:Branch"}, {"Id": "A", "Action": "K2:Self"}],
                 "Links": []}
        reordered = {"Name": "G", "Nodes": [{"Id": "A", "Action": "K2:Self"}, {"Id": "B", "Action": "K2:IfThenElse"}],
                     "Links": []}
        self.assertEqual(k2.canonical_section("UbergraphPages", [graph, {"Name": "A"}]),
                         k2.canonical_section("UbergraphPages", [{"Name": "A"}, reordered]))


if __name__ == "__main__":
    unittest.main()
