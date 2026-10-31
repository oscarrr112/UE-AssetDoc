# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Focused pure codec tests for the production recursive Tree contract."""
from copy import deepcopy
import unittest

from asset_document.diagnostics import AssetDocumentContractError, collecting_read_warnings
from asset_document.tolerance import authoring, reading_live, settle
from asset_document.profiles.behavior_tree_codec import (
    authored_digest_v2, canonicalize_body, encode_runtime_value,
    readback_v2_to_tree, tree_to_semantic_projection,
    lower_authored_properties, extract_authored_properties, authored_properties_match,
    selector_inventory,
)


def guid(index):
    return f"{index:032X}"


def tree_fixture():
    return {"GraphGuid": guid(100), "Root": {
        "Id": guid(1), "Class": "/Script/AIModule.BTComposite_Sequence",
        "Properties": {"NodeName": "根🌲", "bApplyDecoratorScope": True},
        "Decorators": [{"Id": guid(2), "Kind": "Composite",
                        "Properties": {"CompositeName": "Gate", "bShowOperations": False},
                        "BoundGraph": {"GraphGuid": guid(101), "Nodes": [
                            {"Id": guid(3), "Kind": "Sink"},
                            {"Id": guid(4), "Kind": "And"},
                            {"Id": guid(5), "Kind": "Test", "Class": "/Script/AIModule.BTDecorator_Blackboard",
                             "Properties": {"BlackboardKey": {"Key": "Target"}}},
                            {"Id": guid(6), "Kind": "Test", "Class": "/Script/AIModule.BTDecorator_Blackboard"}],
                         "Links": [{"From": guid(4), "To": guid(3), "ToInput": 0},
                                   {"From": guid(5), "To": guid(4), "ToInput": 0},
                                   {"From": guid(6), "To": guid(4), "ToInput": 1}]}}],
        "Services": [{"Id": guid(7), "Class": "/Script/AIModule.BTService_DefaultFocus"}],
        "Children": [{"Id": guid(8), "Class": "/Script/AIModule.BTTask_Wait",
                      "Properties": {"WaitTime": {"DefaultValue": 2.0, "Key": "None"},
                                     "Typed": {"Struct": "/Script/Test.Payload", "Properties": {"Count": 9}},
                                     "Reference": {"Kind": "AssetRef", "Path": "/Game/A.A"},
                                     "Values": [False, 3, 4.5, None], "EnumValue": 2}}]},
        "Comments": [{"Id": guid(9), "Text": "Note", "Color": {"R": 0.25}, "Size": {"Width": 450}}]}


class BehaviorTreeCodecTests(unittest.TestCase):
    def test_guid_case_matches_stock_readback_without_rewriting_authored_bytes(self):
        source = tree_fixture()
        source["GraphGuid"] = "abcdefabcdefabcdefabcdefabcdefab"
        source["Root"]["Id"] = "60606060ddddddddeeeeeeeeffffffff"
        source["Comments"][0]["Id"] = "abcdefabcdefabcdefabcdefabcdefac"
        source["Root"]["Decorators"][0]["BoundGraph"]["GraphGuid"] = "abcdefabcdefabcdefabcdefabcdefad"
        original = deepcopy(source)
        projected = tree_to_semantic_projection(source)
        self.assertEqual(projected["root"]["stable_node_id"], "60606060DDDDDDDDEEEEEEEEFFFFFFFF")
        self.assertEqual(projected["graph_guid"], source["GraphGuid"].upper())
        self.assertEqual(projected["comments"][0]["stable_node_id"], source["Comments"][0]["Id"].upper())
        extracted = readback_v2_to_tree(projected)
        self.assertEqual(tree_to_semantic_projection(extracted), projected)
        self.assertEqual(authored_digest_v2("/Game/Tree.Tree", tree_to_semantic_projection(extracted)),
                         authored_digest_v2("/Game/Tree.Tree", projected))
        self.assertEqual(canonicalize_body({"Tree": source}), {"Tree": original})
        self.assertEqual(source, original)
        sparse = {"GraphGuid": source["GraphGuid"].upper(), "Root": {},
                  "Comments": [{"Id": source["Comments"][0]["Id"].upper(), "Text": "New"}]}
        retained = tree_to_semantic_projection(sparse, existing_tree=source)
        self.assertEqual(retained["comments"][0]["color"]["r"], 0.25)

    def test_recursive_full_round_trip_and_authored_types(self):
        source = tree_fixture()
        lowered = tree_to_semantic_projection(source)
        extracted = readback_v2_to_tree(lowered)
        self.assertEqual(tree_to_semantic_projection(extracted), lowered)
        self.assertEqual(extracted["Root"]["Children"][0]["Properties"], source["Root"]["Children"][0]["Properties"])
        self.assertEqual(extracted["Root"]["Decorators"][0]["Properties"], {"CompositeName": "Gate", "bShowOperations": False})
        self.assertEqual(lowered["root"]["decorators"][0]["bound_graph"]["nodes"][1]["input_count"], 2)

    def test_omitted_null_and_empty_are_distinct(self):
        self.assertEqual(canonicalize_body({}), {})
        self.assertEqual(canonicalize_body({"BlackboardAsset": None}), {"BlackboardAsset": None})
        self.assertEqual(canonicalize_body({"Tree": {"Root": {}}}), {"Tree": {"Root": {}}})
        with self.assertRaises(AssetDocumentContractError):
            canonicalize_body({"Tree": None})
        body = {"BlackboardAsset": {"Kind": "AssetRef", "Path": "/Game/BB.BB"}}
        self.assertEqual(canonicalize_body(body), body)

    def test_layout_defaults_follow_legacy_parse_order(self):
        value = tree_to_semantic_projection(tree_fixture())["root"]
        self.assertEqual(value["editor"]["position"], {"x": 0, "y": 0})
        self.assertEqual(value["decorators"][0]["editor"]["position"], {"x": 300, "y": 0})
        self.assertEqual(value["services"][0]["editor"]["position"], {"x": 1800, "y": 0})
        self.assertEqual(value["children"][0]["editor"]["position"], {"x": 2100, "y": 300})

    def test_existing_comment_omission_retains_and_guid_is_not_replaced(self):
        existing = readback_v2_to_tree(tree_to_semantic_projection(tree_fixture()))
        existing["Comments"][0]["Text"] = "Retain me"
        desired = {"Root": {}, "Comments": [{"Id": guid(9), "Color": {"G": 0.5}}]}
        result = tree_to_semantic_projection(desired, existing_tree=existing)
        self.assertEqual(result["graph_guid"], guid(100))
        self.assertEqual(result["comments"][0]["text"], "Retain me")
        self.assertEqual(result["comments"][0]["color"], {"r": 0.25, "g": 0.5, "b": 1.0, "a": 1.0})
        self.assertNotIn("GraphGuid", desired)
        for replacement in ({"Root": {}, "Comments": []}, {"Root": {}}):
            self.assertEqual(tree_to_semantic_projection(replacement, existing_tree=existing)["comments"], [])
        desired["GraphGuid"] = guid(999)
        with self.assertRaises(AssetDocumentContractError) as raised:
            tree_to_semantic_projection(desired, existing_tree=existing)
        self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                         ("BehaviorTreeGraphGuidReplacementRejected", "/Body/Tree/GraphGuid"))

    def test_legacy_shape_identity_editor_and_comment_diagnostics_have_leaf_paths(self):
        cases = [
            (("GraphGuid",), None, "InvalidBehaviorTreeGraphGuid"),
            (("Root",), [], "InvalidBehaviorTreeObject"),
            (("Root", "Id"), "invalid", "InvalidBehaviorTreeNodeGuid"),
            (("Root", "Class"), " ", "InvalidBehaviorTreeNodeClass"),
            (("Root", "Properties"), [], "InvalidTreeProperties"),
            (("Root", "unknown/~"), True, "UnknownField"),
            (("Root", "Editor", "Position", "X"), True, "InvalidBehaviorTreeCoordinate"),
            (("Root", "Editor", "bCommentBubblePinned"), 1, "InvalidBehaviorTreeEditorField"),
            (("Root", "Children", 0, "Id"), guid(1), "DuplicateBehaviorTreeNodeGuid"),
            (("Root", "Children", 0), None, "InvalidBehaviorTreeNode"),
            (("Root", "Services", 0), [], "InvalidBehaviorTreeService"),
            (("Root", "Decorators", 0), None, "InvalidBehaviorTreeDecorator"),
            (("Root", "Decorators", 0, "Kind"), "Simple", "InvalidBehaviorTreeDecoratorKind"),
            (("Root", "Decorators", 0, "Properties", "bShowOperations"), 1, "InvalidBehaviorTreeDecoratorProperty"),
            (("Comments",), {}, "InvalidBehaviorTreeArray"),
            (("Comments", 0), False, "InvalidBehaviorTreeComment"),
            (("Comments", 0, "Position", "X"), 0.5, "InvalidBehaviorTreeCoordinate"),
            (("Comments", 0, "Color", "R"), "red", "InvalidBehaviorTreeNumber"),
            (("Comments", 0, "Text"), 1, "InvalidBehaviorTreeCommentField"),
            (("Comments", 0, "FontSize"), 0, "InvalidBehaviorTreeCommentField"),
            (("Comments", 0, "MoveMode"), "Unknown", "InvalidBehaviorTreeCommentField"),
        ]
        for fields, value, code in cases:
            with self.subTest(fields=fields):
                source = tree_fixture()
                target = source
                for field in fields[:-1]:
                    target = target.setdefault(field, {}) if type(target) is dict else target[field]
                target[fields[-1]] = value
                pointer = "/Body/Tree/" + "/".join(str(field).replace("~", "~0").replace("/", "~1") for field in fields)
                with self.assertRaises(AssetDocumentContractError) as raised:
                    tree_to_semantic_projection(source)
                self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path), (code, pointer))
        source = tree_fixture()
        source["Comments"][0]["Size"] = {"Width": 0}
        with self.assertRaises(AssetDocumentContractError) as raised:
            tree_to_semantic_projection(source)
        self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                         ("InvalidBehaviorTreeCommentSize", "/Body/Tree/Comments/0/Size"))
        source = tree_fixture()
        source["Root"]["Id"] = "60606060ddddddddeeeeeeeeffffffff"
        source["Root"]["Children"][0]["Id"] = source["Root"]["Id"].upper()
        with self.assertRaises(AssetDocumentContractError) as raised:
            tree_to_semantic_projection(source)
        self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                         ("DuplicateBehaviorTreeNodeGuid", "/Body/Tree/Root/Children/0/Id"))
        source = tree_fixture()
        source["Root"]["Children"][0]["Editor"] = {"Position": {"X": 0, "Y": 0}}
        # Nodes on top of each other are kept as they are.
        self.assertEqual(tree_to_semantic_projection(source)["root"]["children"][0]["editor"]["position"], {"x": 0, "y": 0})

    def test_bound_graph_diagnostics_preserve_old_cycle_before_arity_order(self):
        base = "/Body/Tree/Root/Decorators/0/BoundGraph"
        cases = [
            (("GraphGuid",), None, "InvalidBehaviorTreeGraphGuid"),
            (("Nodes", 1, "Kind"), "Unknown", "InvalidBehaviorTreeDecoratorKind"),
            (("Nodes", 1, "Class"), "C", "InvalidBehaviorTreeDecoratorNode"),
            (("Nodes", 2, "Id"), guid(1), "DuplicateBehaviorTreeNodeGuid"),
            (("Links", 0, "From"), guid(999), "DanglingBehaviorTreeDecoratorLink"),
            (("Links", 0, "ToInput"), -1, "InvalidBehaviorTreeDecoratorInput"),
            (("Links", 2, "ToInput"), 0, "MultipleBehaviorTreeDecoratorLinks"),
        ]
        for fields, value, code in cases:
            with self.subTest(fields=fields):
                source = tree_fixture()
                target = source["Root"]["Decorators"][0]["BoundGraph"]
                for field in fields[:-1]:
                    target = target[field]
                target[fields[-1]] = value
                with self.assertRaises(AssetDocumentContractError) as raised:
                    tree_to_semantic_projection(source)
                self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                                 (code, base + "/" + "/".join(map(str, fields))))
        for change, code, suffix in (
            (lambda graph: graph["Links"][0].update(To=guid(5)), "BehaviorTreeDecoratorGraphCycle", "/Links/1"),
            (lambda graph: graph["Nodes"][1].update(Kind="Not"), "InvalidBehaviorTreeDecoratorArity", "/Nodes/1"),
            (lambda graph: graph["Links"].pop(), "InvalidBehaviorTreeDecoratorArity", "/Nodes/1"),
            (lambda graph: graph["Links"].append(deepcopy(graph["Links"][1])), "MultipleBehaviorTreeDecoratorLinks", "/Links/3/From"),
            (lambda graph: graph["Nodes"].append({"Id": guid(10), "Kind": "Test", "Class": "C"}),
             "UnreachableBehaviorTreeDecoratorNode", "/Nodes/4"),
        ):
            with self.subTest(code=code, suffix=suffix):
                source = tree_fixture()
                change(source["Root"]["Decorators"][0]["BoundGraph"])
                with self.assertRaises(AssetDocumentContractError) as raised:
                    tree_to_semantic_projection(source)
                self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path), (code, base + suffix))

    def test_body_and_blackboard_reference_shape_diagnostics(self):
        for body, code, path in (
            ({"BlackboardInline": {}}, "UnknownBodyKey", "/Body/BlackboardInline"),
            ({"BlackboardAsset": {"Kind": "ClassRef", "Path": "/Game/A.A"}}, "InvalidBehaviorTreeBlackboardRef", "/Body/BlackboardAsset/Kind"),
            ({"BlackboardAsset": {"Kind": "AssetRef"}}, "MissingBehaviorTreeBlackboardPath", "/Body/BlackboardAsset/Path"),
            ({"BlackboardAsset": ""}, "MissingBehaviorTreeBlackboardPath", "/Body/BlackboardAsset"),
        ):
            with self.subTest(body=body):
                with self.assertRaises(AssetDocumentContractError) as raised:
                    canonicalize_body(body)
                self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path), (code, path))

    def test_children_and_attachment_order_are_semantic(self):
        source = tree_fixture()
        child = deepcopy(source["Root"]["Children"][0]); child["Id"] = guid(10)
        source["Root"]["Children"].append(child)
        before = tree_to_semantic_projection(source)
        source["Root"]["Children"].reverse()
        after = tree_to_semantic_projection(source)
        self.assertEqual([node["stable_node_id"] for node in after["root"]["children"]], [guid(10), guid(8)])
        self.assertNotEqual(before, after)

    def test_guid_collision_and_unknown_control_keys_rejected(self):
        source = tree_fixture(); source["Root"]["Children"][0]["Id"] = guid(2)
        with self.assertRaises(AssetDocumentContractError): tree_to_semantic_projection(source)
        for name in ("InstallGraph", "EnsureDecoratorInputCount", "Nodes", "RefreshValueOrKey"):
            source = tree_fixture(); source[name] = True
            with self.assertRaises(AssetDocumentContractError): tree_to_semantic_projection(source)

    def test_bound_graph_cycles_dangling_and_missing_input_rejected(self):
        for replacement in (guid(99), guid(5)):
            source = tree_fixture()
            source["Root"]["Decorators"][0]["BoundGraph"]["Links"][0]["To"] = replacement
            with self.assertRaises(AssetDocumentContractError): tree_to_semantic_projection(source)
        source = tree_fixture()
        source["Root"]["Decorators"][0]["BoundGraph"]["Links"][2]["ToInput"] = 2
        with self.assertRaises(AssetDocumentContractError): tree_to_semantic_projection(source)

    def test_bound_comments_and_open_inputs_read_as_kept(self):
        value = tree_to_semantic_projection(tree_fixture())
        bound = value["root"]["decorators"][0]["bound_graph"]
        bound["comments"] = [dict(deepcopy(value["comments"][0]), stable_node_id=guid(50))]
        bound["nodes"][1]["input_count"] = 3
        with collecting_read_warnings() as warnings, reading_live():
            extracted = readback_v2_to_tree(value)
        graph = extracted["Root"]["Decorators"][0]["BoundGraph"]
        self.assertEqual(graph["Comments"][0]["Id"], guid(50))
        self.assertEqual(graph["Nodes"][1]["InputCount"], 3)
        self.assertNotIn("InputCount", graph["Nodes"][0])
        self.assertEqual([(item.code, item.path) for item in warnings],
                         [("InvalidBehaviorTreeDecoratorArity", "/Body/Tree/Root/Decorators/0/BoundGraph/Nodes/1")])
        # The extract re-applies unchanged: the finding is waived because the asset holds the same node.
        with authoring() as findings:
            lowered = tree_to_semantic_projection(extracted)
        self.assertEqual(lowered, value)
        settle(findings, {"Body": {"Tree": extracted}}, {"Body": {"Tree": deepcopy(extracted)}})
        # A new open input is refused.
        edited = deepcopy(extracted)
        edited["Root"]["Decorators"][0]["BoundGraph"]["Nodes"][1]["InputCount"] = 4
        with authoring() as findings:
            tree_to_semantic_projection(edited)
        with self.assertRaises(AssetDocumentContractError) as raised:
            settle(findings, {"Body": {"Tree": edited}}, {"Body": {"Tree": extracted}})
        self.assertEqual(raised.exception.diagnostic.code, "InvalidBehaviorTreeDecoratorArity")

    def test_an_apply_names_its_document_after_preflight_settled_its_rules(self):
        from types import SimpleNamespace
        from asset_document import runtime
        from asset_document.dto import RuntimeOperation
        value = tree_to_semantic_projection(tree_fixture())
        value["root"]["decorators"][0]["bound_graph"]["nodes"][1]["input_count"] = 3
        with collecting_read_warnings(), reading_live():
            extracted = readback_v2_to_tree(value)
        request = SimpleNamespace(request_id="apply", expected_runtime_revision=1, operation=RuntimeOperation.APPLY,
            requested_asset_class_path="/Script/AIModule.BehaviorTree", target_asset_path="/Game/BT_Open.BT_Open",
            canonical_document={"SchemaVersion": 1, "Target": "/Game/BT_Open", "Class": "/Script/AIModule.BehaviorTree",
                                "Action": "CreateOrUpdate", "Definitions": {}, "Properties": {},
                                "Body": {"BlackboardAsset": None, "Tree": extracted}})
        identity = runtime._behavior_tree_expected_identity(request)
        self.assertEqual(identity[:5], ("apply", 1, "/Script/AIModule.BehaviorTree", "/Game/BT_Open.BT_Open", "Apply"))

    def test_bound_node_not_wired_to_the_sink_reads_with_a_warning(self):
        source = tree_fixture()
        graph = source["Root"]["Decorators"][0]["BoundGraph"]
        graph["Nodes"].append({"Id": guid(60), "Kind": "Not"})
        graph["Links"].append({"From": guid(60), "To": guid(4), "ToInput": 2})
        graph["Nodes"].append({"Id": guid(61), "Kind": "Test", "Class": "/Script/AIModule.BTDecorator_Blackboard"})
        with self.assertRaises(AssetDocumentContractError) as raised:
            tree_to_semantic_projection(source)
        self.assertEqual(raised.exception.diagnostic.code, "InvalidBehaviorTreeDecoratorArity")
        graph["Nodes"][1]["InputCount"] = 3
        with collecting_read_warnings() as warnings, reading_live():
            lowered = tree_to_semantic_projection(source)
        self.assertEqual(sorted((item.code, item.path.rsplit("/", 1)[-1]) for item in warnings),
                         [("InvalidBehaviorTreeDecoratorArity", "4"), ("UnreachableBehaviorTreeDecoratorNode", "5")])
        with collecting_read_warnings(), reading_live():
            extracted = readback_v2_to_tree(lowered)
        nodes = extracted["Root"]["Decorators"][0]["BoundGraph"]["Nodes"]
        # The And has all three inputs wired; the Not's input is open.
        self.assertEqual([node.get("InputCount") for node in nodes], [None, None, None, None, 1, None])

    def test_an_input_count_must_fit_the_node_kind_and_its_links(self):
        for kind, count in (("Not", 2), ("And", 1)):
            source = tree_fixture()
            nodes = source["Root"]["Decorators"][0]["BoundGraph"]["Nodes"]
            nodes[1] = dict(nodes[1], Kind=kind, InputCount=count)
            with self.assertRaises(AssetDocumentContractError) as raised:
                tree_to_semantic_projection(source)
            self.assertEqual(raised.exception.diagnostic.path, "/Body/Tree/Root/Decorators/0/BoundGraph/Nodes/1/InputCount")

    def test_missing_wrapper_authored_fields_rejected(self):
        value = tree_to_semantic_projection(tree_fixture())
        value["root"]["decorators"][0]["authored_properties"] = {}
        with self.assertRaises(AssetDocumentContractError): readback_v2_to_tree(value)

    def test_property_mapping_is_explicit_and_does_not_guess_int(self):
        calls = []
        def mapper(class_path, properties):
            calls.append(class_path)
            if class_path.endswith("BTTask_Wait"):
                properties["EnumValue"] = "DeclaredLabel"
            return properties
        value = tree_to_semantic_projection(tree_fixture())
        unchanged = readback_v2_to_tree(value)
        mapped = readback_v2_to_tree(value, property_mapper=mapper)
        self.assertEqual(unchanged["Root"]["Children"][0]["Properties"]["EnumValue"], 2)
        self.assertEqual(mapped["Root"]["Children"][0]["Properties"]["EnumValue"], "DeclaredLabel")
        self.assertIn("/Script/AIModule.BTTask_Wait", calls)

    def test_runtime_byte_golden_and_complete_v2_digest(self):
        self.assertEqual(encode_runtime_value([False, 1, -0.0, "中"]).hex(),
                         "0600000000000000040100020000000000000001030000000000000000040000000000000003e4b8ad")
        absent = {"comments": [], "graph_guid": None, "graph_state": "absent", "root": None}
        self.assertEqual(authored_digest_v2("/Game/Tests/Tree.Tree", absent),
                         "a30a0bcf01c2c0c3f344332076f1d78d1a1ff77f0c658307794513c132fa3a54")
        self.assertEqual(readback_v2_to_tree(absent), {"Root": {}, "Comments": []})
        self.assertNotEqual(encode_runtime_value(1), encode_runtime_value(1.0))
        self.assertEqual(encode_runtime_value(-0.0), encode_runtime_value(0.0))
        self.assertEqual(encode_runtime_value({"z": 1, "a": 2}), encode_runtime_value({"a": 2, "z": 1}))

    def test_detached_branches_round_trip_in_identity_order_and_only_when_present(self):
        source = tree_fixture()
        source["Detached"] = [
            {"Id": guid(31), "Class": "/Script/AIModule.BTTask_Wait", "Editor": {"Position": {"X": 900, "Y": 40}}},
            {"Id": guid(30), "Class": "/Script/AIModule.BTComposite_Selector",
             "Children": [{"Id": guid(32), "Class": "/Script/AIModule.BTTask_Wait"}]}]
        lowered = tree_to_semantic_projection(source)
        self.assertEqual([item["stable_node_id"] for item in lowered["detached"]], [guid(30), guid(31)])
        self.assertEqual(lowered["detached"][0]["children"][0]["stable_node_id"], guid(32))
        self.assertEqual(lowered["detached"][1]["editor"]["position"], {"x": 900, "y": 40})
        extracted = readback_v2_to_tree(lowered)
        self.assertEqual([item["Id"] for item in extracted["Detached"]], [guid(30), guid(31)])
        self.assertEqual(tree_to_semantic_projection(extracted), lowered)
        # A Root-less tree may keep detached branches.
        rootless = tree_to_semantic_projection({"Root": {}, "Detached": source["Detached"]})
        self.assertIsNone(rootless["root"])
        self.assertEqual(readback_v2_to_tree(dict(rootless, graph_guid=guid(100)))["Root"], {})
        # No branch, no key: a connected tree lowers and reads exactly as before.
        plain = tree_to_semantic_projection(tree_fixture())
        self.assertNotIn("detached", plain)
        self.assertNotIn("Detached", readback_v2_to_tree(plain))
        self.assertNotIn("detached", tree_to_semantic_projection(dict(tree_fixture(), Detached=[])))
        self.assertEqual(authored_digest_v2("/Game/Tree.Tree", tree_to_semantic_projection(readback_v2_to_tree(plain))),
                         authored_digest_v2("/Game/Tree.Tree", plain))
        for branch, code, path in (
                ({"Id": guid(1), "Class": "/Script/AIModule.BTTask_Wait"}, "DuplicateBehaviorTreeNodeGuid", "/Body/Tree/Detached/0/Id"),
                ({"Id": guid(40), "Class": "/Script/AIModule.BTTask_Wait", "Kind": "Composite"}, "UnknownField", "/Body/Tree/Detached/0/Kind"),
                ([], "InvalidBehaviorTreeNode", "/Body/Tree/Detached/0")):
            with self.subTest(path=path):
                with self.assertRaises(AssetDocumentContractError) as raised:
                    tree_to_semantic_projection(dict(tree_fixture(), Detached=[branch]))
                self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path), (code, path))
        with self.assertRaises(AssetDocumentContractError) as raised:
            tree_to_semantic_projection(dict(tree_fixture(), Detached={}))
        self.assertEqual(raised.exception.diagnostic.path, "/Body/Tree/Detached")

    def test_detached_nodes_join_the_selector_inventory(self):
        props = {"Choice": {"Key": "Target"}}
        source = dict(tree_fixture(), Detached=[{"Id": guid(30), "Class": "/Script/AIModule.BTTask_Wait"}])
        value = tree_to_semantic_projection(source, property_mapper=lambda _class, _props: deepcopy(props))
        evidence = lambda name: {"node_class_path": name, "authored_defaults": props,
                                "property_schema": {"Choice": {"kind": "selector"}}}
        self.assertIn(guid(30), [row["stable_node_id"] for row in selector_inventory(value, evidence=evidence)])

    def test_unsupported_property_marker_passes_through_and_refuses_a_value(self):
        reason = "A TMap<int32,float> property value has no document form; AssetDoc keeps the value."
        default, live = {"Text": "()", "Unsupported": reason}, {"Text": "((1, 2.000000))", "Unsupported": reason}
        evidence = lambda name: {"node_class_path": name, "authored_defaults": {"Table": default, "Duration": 0.0},
                                 "property_schema": {"Table": {"kind": "unsupported", "default": default},
                                                     "Duration": {"kind": "number", "precision": "float32"}}}
        observed = {"Table": live, "Duration": 1.5}
        self.assertEqual(extract_authored_properties("/Script/Test.Node", observed, evidence=evidence), observed)
        self.assertEqual(extract_authored_properties("/Script/Test.Node", {"Table": default, "Duration": 0.0},
                                                     evidence=evidence, omit_defaults=True), {})
        self.assertEqual(lower_authored_properties("/Script/Test.Node", {"Table": live}, evidence=evidence),
                         {"Table": live, "Duration": 0.0})
        # Omitted, the property lowers to the class default's marker.
        self.assertEqual(lower_authored_properties("/Script/Test.Node", {}, evidence=evidence)["Table"], default)
        for value in ({}, {"1": 2.0}, None, "((1, 2.0))"):
            with self.subTest(value=value), self.assertRaises(AssetDocumentContractError) as raised:
                lower_authored_properties("/Script/Test.Node", {"Table": value}, evidence=evidence)
            self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                             ("BehaviorTree.UnrepresentedValue", "/Properties/Table"))

    def test_a_non_finite_number_reads_as_a_marker_of_a_typed_property(self):
        nan = {"Text": "nan", "Unsupported": "A non-finite number has no document form; AssetDoc keeps the value."}
        evidence = lambda name: {"node_class_path": name, "authored_defaults": {"Duration": 0.0},
                                 "property_schema": {"Duration": {"kind": "number", "precision": "float32"}}}
        self.assertEqual(extract_authored_properties("/Script/Test.Node", {"Duration": nan}, evidence=evidence),
                         {"Duration": nan})
        self.assertEqual(lower_authored_properties("/Script/Test.Node", {"Duration": nan}, evidence=evidence),
                         {"Duration": nan})
        with self.assertRaises(AssetDocumentContractError):
            lower_authored_properties("/Script/Test.Node", {"Duration": float("nan")}, evidence=evidence)

    def test_a_marked_property_holds_no_selector(self):
        marker = {"Text": "(SelectedKeyName=\"Target\")", "Unsupported": "reason"}
        value = tree_to_semantic_projection({"Root": {"Id": guid(1), "Class": "/Script/Test.Node"}},
                                            property_mapper=lambda _class, _props: {"Choice": deepcopy(marker)})
        evidence = lambda name: {"node_class_path": name, "authored_defaults": {"Choice": {"Key": "None"}},
                                 "property_schema": {"Choice": {"kind": "selector"}}}
        self.assertEqual(selector_inventory(value, evidence=evidence), [])

    def test_a_node_whose_class_is_missing_reads_and_lowers_as_a_marked_node(self):
        gone = "/Game/Gone/BTT_Gone.BTT_Gone_C"
        reason = f"The node's class {gone} is missing; AssetDoc keeps the node as it is."
        tree = {"Root": {"Id": guid(1), "Class": "/Script/AIModule.BTComposite_Sequence", "Children": [
            {"Id": guid(2), "Class": gone, "Unsupported": reason,
             "Decorators": [{"Id": guid(3), "Class": gone, "Unsupported": reason}],
             "Services": [{"Id": guid(4), "Class": gone, "Unsupported": reason}]}]}}

        def mapper(class_path, properties):
            self.assertNotEqual(class_path, gone, "a marked node has no properties to map")
            return properties

        value = tree_to_semantic_projection(tree, property_mapper=mapper)
        value["graph_guid"] = guid(100)
        marked = value["root"]["children"][0]
        for node in (marked, marked["decorators"][0], marked["services"][0]):
            self.assertEqual((node["node_class_path"], node["authored_properties"], node["unsupported"]),
                             (gone, {}, reason))
        raised = readback_v2_to_tree(value, property_mapper=mapper)
        node = raised["Root"]["Children"][0]
        self.assertEqual((node["Class"], node["Unsupported"], "Properties" in node), (gone, reason, False))
        self.assertEqual(node["Decorators"][0]["Unsupported"], reason)
        self.assertEqual(node["Services"][0]["Unsupported"], reason)
        self.assertEqual(selector_inventory(value, evidence=lambda name: self.fail("no evidence for " + name)
                                            if name == gone else {"node_class_path": name, "authored_defaults": {},
                                                                  "property_schema": {}}), [])
        # A marked node has no properties to write, and its reason is a string.
        with_properties = deepcopy(tree); with_properties["Root"]["Children"][0]["Properties"] = {}
        bad_reason = deepcopy(tree); bad_reason["Root"]["Children"][0]["Services"][0]["Unsupported"] = 3
        for source, path in ((with_properties, "/Body/Tree/Root/Children/0/Properties"),
                             (bad_reason, "/Body/Tree/Root/Children/0/Services/0/Unsupported")):
            with self.subTest(path=path), self.assertRaises(AssetDocumentContractError) as error:
                tree_to_semantic_projection(source)
            self.assertEqual((error.exception.diagnostic.code, error.exception.diagnostic.path),
                             ("BehaviorTree.UnrepresentedValue", path))

    def test_nonfinite_boolean_coordinate_and_invalid_comment_rejected(self):
        for editor in ({"Position": {"X": True, "Y": 0}}, {"Position": {"X": float("nan"), "Y": 0}}):
            source = tree_fixture(); source["Root"]["Editor"] = editor
            with self.assertRaises(AssetDocumentContractError): tree_to_semantic_projection(source)
        source = tree_fixture(); source["Comments"][0]["Size"] = {"Width": 0}
        with self.assertRaises(AssetDocumentContractError): tree_to_semantic_projection(source)


def class_evidence(class_path):
    enum = {"kind": "enum", "values": {"First": 1, "Second": 4}}
    fields = {"Mode": dict(enum, default=1), "Count": {"kind": "value", "default": 8}}
    return {"node_class_path": class_path,
            "authored_defaults": {"Mode": 1, "Duration": 0.0, "Payload": {"Mode": 1, "Count": 8},
                                  "Array": [], "Set": [], "Map": {}, "Selector": {"Key": "None"},
                                  "Reference": None, "Dynamic": None},
            "property_schema": {"Mode": enum, "Duration": {"kind": "number", "precision": "float32"},
                                "Payload": {"kind": "struct", "fields": fields},
                                "Array": {"kind": "array", "element": {"kind": "struct", "fields": fields}},
                                "Set": {"kind": "set", "element": enum},
                                "Map": {"kind": "map", "key_enum": enum["values"], "value": enum},
                                "Selector": {"kind": "selector"}, "Reference": {"kind": "value"},
                                "Dynamic": {"kind": "instanced_struct"}},
            "struct_schemas": {"/Script/Test.Payload": fields}}


class BehaviorTreePropertyCodecTests(unittest.TestCase):
    def test_value_metadata_enforces_scalars_and_real_integer_boundaries(self):
        for bits, signed, minimum, maximum in ((8, False, 0, 255), (16, True, -(2**15), 2**15 - 1),
                                              (32, True, -(2**31), 2**31 - 1), (64, True, -(2**63), 2**63 - 1)):
            def evidence(name):
                data = class_evidence(name)
                data["authored_defaults"]["Count"] = 8
                data["property_schema"]["Count"] = {"kind": "value", "value_type": "integer",
                                                    "integer_bits": bits, "integer_signed": signed}
                return data
            for value in (minimum, maximum):
                with self.subTest(bits=bits, signed=signed, boundary=value):
                    self.assertEqual(lower_authored_properties("C", {"Count": value}, evidence=evidence)["Count"], value)
            invalid = [True, 1.25]
            if bits < 64:
                invalid.extend((minimum - 1, maximum + 1))
            for value in invalid:
                with self.subTest(bits=bits, invalid=value):
                    with self.assertRaises(AssetDocumentContractError) as raised:
                        lower_authored_properties("C", {"Count": value}, evidence=evidence)
                    self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                                     ("InvalidPropertyValue", "/Properties/Count"))
        authored = {"Count": 1.0}
        before = encode_runtime_value(authored)
        native = lower_authored_properties("C", authored, evidence=evidence)
        self.assertEqual(native["Count"], 1)
        self.assertIs(type(native["Count"]), int)
        self.assertEqual(encode_runtime_value(authored), before)
        def scalar_evidence(name):
            return {"node_class_path": name, "authored_defaults": {"Flag": False, "String": "", "Text": ""},
                    "property_schema": {name: {"kind": "value", "value_type": kind}
                                        for name, kind in (("Flag", "bool"), ("String", "string"), ("Text", "text"))}}
        for field, value in (("Flag", "not-a-boolean"), ("String", 9), ("Text", {"Text": "wrong wrapper"})):
            with self.subTest(field=field):
                with self.assertRaises(AssetDocumentContractError) as raised:
                    lower_authored_properties("C", {field: value}, evidence=scalar_evidence)
                self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                                 ("InvalidPropertyValue", "/Properties/" + field))
        self.assertEqual(lower_authored_properties("C", {"Flag": True, "String": "字符串", "Text": "文本"},
                                                  evidence=scalar_evidence), {"Flag": True, "String": "字符串", "Text": "文本"})
        # Opaque legacy mocks remain opaque when the provider supplies no metadata.
        self.assertEqual(lower_authored_properties("C", {"Reference": "legacy mock"}, evidence=class_evidence)["Reference"], "legacy mock")

    def test_reference_constraints_recurse_without_extract_loading(self):
        asset = {"kind": "value", "value_type": "reference", "reference_kind": "AssetRef",
                 "declared_class_path": "/Script/AIModule.BehaviorTree"}
        represented = {"kind": "value", "value_type": "reference", "reference_kind": "ClassRef",
                       "declared_class_path": "/Script/Engine.Actor"}
        class_object = dict(represented, reference_constraint="object_class", declared_class_path="/Script/CoreUObject.Class")
        inline_class, inline_base, payload = "/Script/Test.RefInline", "/Script/Test.RefBase", "/Script/Test.RefPayload"
        def evidence(name):
            data = class_evidence(name)
            data["authored_defaults"].update(Nested={"Refs": []}, Map={}, Set=[], Inline=None, ActorClass=None, ClassObject=None)
            data["property_schema"].update(
                Nested={"kind": "struct", "fields": {"Refs": {"kind": "array", "element": asset}}},
                Map={"kind": "map", "value": asset}, Set={"kind": "set", "element": asset},
                Inline={"kind": "instanced_object", "declared_class_path": inline_base},
                ActorClass=represented, ClassObject=class_object)
            data["struct_schemas"][payload] = {"Reference": asset}
            return data
        def inline(name, base):
            self.assertEqual((name, base), (inline_class, inline_base))
            return {"node_class_path": name, "authored_defaults": {"Reference": None}, "property_schema": {"Reference": asset}}
        path = "/Game/Tests/Referenced.Referenced"
        canonical = {"Kind": "AssetRef", "Path": path}
        authored = {"Nested": {"Refs": [path]}, "Map": {"slot/0~": canonical}, "Set": [path],
                    "Dynamic": {"Struct": payload, "Properties": {"Reference": path}},
                    "Inline": {"Class": inline_class, "Properties": {"Reference": path}},
                    "ActorClass": "/Script/Engine.Pawn", "ClassObject": "/Script/Engine.Actor"}
        before, calls = deepcopy(authored), []
        def resolve(constraint, reference, declared, wire_path):
            calls.append((constraint, reference, declared, wire_path))
        native = lower_authored_properties("C", authored, evidence=evidence, object_evidence=inline, reference_resolver=resolve)
        expected = {("object_class", path, asset["declared_class_path"], field) for field in (
            "/Properties/Nested/Refs/0", "/Properties/Map/slot~10~0", "/Properties/Set/0",
            "/Properties/Dynamic/Properties/Reference", "/Properties/Inline/Properties/Reference")}
        expected.update({("represented_class", "/Script/Engine.Pawn", "/Script/Engine.Actor", "/Properties/ActorClass"),
                         ("object_class", "/Script/Engine.Actor", "/Script/CoreUObject.Class", "/Properties/ClassObject")})
        self.assertEqual(set(calls), expected)
        self.assertEqual(len(calls), 7)
        self.assertEqual(native["Nested"]["Refs"], [canonical])
        self.assertEqual(native["ClassObject"], {"Kind": "ClassRef", "Path": "/Script/Engine.Actor"})
        self.assertEqual(authored, before)
        extracted = extract_authored_properties("C", native, evidence=evidence, object_evidence=inline, reference_resolver=resolve)
        self.assertEqual(len(calls), 7)
        self.assertEqual(extracted["Map"], {"slot/0~": canonical})
        self.assertTrue(authored_properties_match("C", authored, native, evidence=evidence,
                                                 object_evidence=inline, reference_resolver=resolve))
        self.assertEqual(calls[7:], calls[:7])
        calls.clear()
        empty = lower_authored_properties("C", {"ActorClass": "", "ClassObject": None}, evidence=evidence,
                                          object_evidence=inline, reference_resolver=resolve)
        self.assertIsNone(empty["ActorClass"])
        self.assertIsNone(empty["ClassObject"])
        self.assertEqual(calls, [])

    def test_soft_reference_normalization_preserves_future_paths_without_resolution(self):
        asset_path, class_path = "/Game/Future/Tree.Tree", "/Game/Future/BP_Actor.BP_Actor_C"
        def evidence(name):
            return {"node_class_path": name, "authored_defaults": {"Asset": None, "Class": None},
                    "property_schema": {
                        "Asset": {"kind": "value", "value_type": "reference", "reference_kind": "AssetRef",
                                  "reference_soft": True, "declared_class_path": "/Script/AIModule.BehaviorTree"},
                        "Class": {"kind": "value", "value_type": "reference", "reference_kind": "ClassRef",
                                  "reference_soft": True, "declared_class_path": "/Script/Engine.Actor"}}}
        def forbidden(*args):
            self.fail("Soft references must not invoke the hard resolver")
        authored = {"Asset": asset_path, "Class": {"Kind": "ClassRef", "Path": " " + class_path + " "}}
        before = encode_runtime_value(authored)
        native = lower_authored_properties("C", authored, evidence=evidence, reference_resolver=forbidden)
        self.assertEqual(native, {"Asset": {"Kind": "AssetRef", "Path": asset_path},
                                  "Class": {"Kind": "ClassRef", "Path": class_path}})
        self.assertEqual(encode_runtime_value(authored), before)
        self.assertEqual(lower_authored_properties("C", {"Asset": "", "Class": None}, evidence=evidence,
                                                   reference_resolver=forbidden), {"Asset": None, "Class": None})

    def test_reference_metadata_shape_errors_keep_reference_leaf_paths(self):
        def evidence(name):
            data = class_evidence(name)
            data["property_schema"]["Reference"] = {"kind": "value", "value_type": "reference", "reference_kind": "AssetRef",
                                                     "declared_class_path": "/Script/AIModule.BehaviorTree"}
            return data
        for value, code, leaf in (
            ({"Kind": "ClassRef", "Path": "/Script/Engine.Actor"}, "InvalidPropertyValue", "Kind"),
            ({"Kind": "AssetRef", "Path": 7}, "InvalidPropertyValue", "Path"),
            ({"Kind": "AssetRef", "Path": "/Game/A.A", "Extra": True}, "UnknownField", "Extra"),
        ):
            with self.subTest(value=value):
                with self.assertRaises(AssetDocumentContractError) as raised:
                    lower_authored_properties("C", {"Reference": value}, evidence=evidence)
                self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                                 (code, "/Properties/Reference/" + leaf))

    def test_selector_derived_field_diagnostics_rebase_to_every_node_owner(self):
        selector = {"kind": "selector"}
        def evidence(name):
            return {"node_class_path": name, "authored_defaults": {
                "Choice": {"Key": "None"}, "Nested": {"Choice": {"Key": "None"}},
                "Rows": [], "ByName": {}, "Dynamic": None},
                "property_schema": {"Choice": selector,
                    "Nested": {"kind": "struct", "fields": {"Choice": selector}},
                    "Rows": {"kind": "array", "element": {"kind": "struct", "fields": {"Choice": selector}}},
                    "ByName": {"kind": "map", "value": selector}, "Dynamic": {"kind": "instanced_struct"}},
                "struct_schemas": {"/Script/Test.Payload": {"Choice": selector}}}
        mapper = lambda name, properties: lower_authored_properties(name, properties, evidence=evidence)

        def at_owner(owner, properties):
            node = {"Id": guid(2), "Class": "C", "Properties": properties}
            root = {"Id": guid(1), "Class": "C"}
            if owner == "Root":
                root["Properties"] = properties
            elif owner == "Root/Decorators/0/BoundGraph/Nodes/1":
                node["Kind"] = "Test"
                root["Decorators"] = [{"Id": guid(3), "Kind": "Composite", "BoundGraph": {
                    "GraphGuid": guid(101), "Nodes": [{"Id": guid(4), "Kind": "Sink"}, node],
                    "Links": [{"From": guid(2), "To": guid(4), "ToInput": 0}]}}]
            else:
                root[owner.split("/")[1]] = [node]
            return {"GraphGuid": guid(100), "Root": root}

        derived_fields = ("SelectedKeyName", "AllowedTypes", "SelectedKeyType", "SelectedKeyID", "bNoneIsAllowedValue")
        for owner in ("Root", "Root/Children/0", "Root/Services/0", "Root/Decorators/0", "Root/Decorators/0/BoundGraph/Nodes/1"):
            for field in derived_fields:
                with self.subTest(owner=owner, field=field):
                    source = at_owner(owner, {"Choice": {"Key": "Target", field: False}})
                    with self.assertRaises(AssetDocumentContractError) as raised:
                        tree_to_semantic_projection(source, property_mapper=mapper)
                    self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                                     ("NonAuthoredProperty", f"/Body/Tree/{owner}/Properties/Choice/{field}"))
        for field in derived_fields:
            invalid = {"Key": "Target", field: False}
            for properties, suffix in (
                ({"Nested": {"Choice": invalid}}, "Nested/Choice"),
                ({"Rows": [{"Choice": invalid}]}, "Rows/0/Choice"),
                ({"ByName": {"a/~": invalid}}, "ByName/a~1~0"),
                ({"Dynamic": {"Struct": "/Script/Test.Payload", "Properties": {"Choice": invalid}}}, "Dynamic/Properties/Choice"),
            ):
                with self.subTest(field=field, suffix=suffix):
                    with self.assertRaises(AssetDocumentContractError) as raised:
                        tree_to_semantic_projection(at_owner("Root", properties), property_mapper=mapper)
                    self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                                     ("NonAuthoredProperty", f"/Body/Tree/Root/Properties/{suffix}/{field}"))

    def test_property_shape_diagnostics_keep_specific_fields(self):
        for authored, code, suffix in (
            ({"unknown/~": 1}, "UnknownProperty", "unknown~1~0"),
            ({"Selector": "Target"}, "InvalidPropertyValue", "Selector"),
            ({"Selector": {}}, "MissingProperty", "Selector/Key"),
            ({"Selector": {"Key": 1}}, "InvalidPropertyValue", "Selector/Key"),
            ({"Selector": {"Key": "None", "unknown/~": 0}}, "UnknownProperty", "Selector/unknown~1~0"),
            ({"Payload": {"Unknown": 0}}, "UnknownProperty", "Payload/Unknown"),
            ({"Array": {}}, "InvalidPropertyValue", "Array"),
            ({"Map": []}, "InvalidPropertyValue", "Map"),
            ({"Mode": "Undeclared"}, "InvalidPropertyValue", "Mode"),
            ({"Dynamic": {"Struct": "", "Properties": {}}}, "InvalidStructRef", "Dynamic/Struct"),
            ({"Dynamic": {"Struct": "/Script/Test.Payload", "Properties": []}}, "InvalidPropertyValue", "Dynamic/Properties"),
        ):
            with self.subTest(authored=authored):
                source = {"Root": {"Id": guid(1), "Class": "C", "Properties": authored}}
                with self.assertRaises(AssetDocumentContractError) as raised:
                    tree_to_semantic_projection(source, property_mapper=lambda name, properties:
                        lower_authored_properties(name, properties, evidence=class_evidence))
                self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                                 (code, "/Body/Tree/Root/Properties/" + suffix))
        source = {"Root": {"Id": guid(1), "Class": "C"}}
        with self.assertRaises(AssetDocumentContractError) as raised:
            tree_to_semantic_projection(source, property_mapper=lambda name, properties:
                lower_authored_properties(name, properties, evidence=lambda _: class_evidence("Other")))
        self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                         ("BehaviorTree.InvalidClassEvidence", "/Body/Tree/Root/Class"))

    def test_none_names_are_normalized_only_with_name_or_selector_evidence(self):
        def evidence(name):
            return {"node_class_path": name,
                    "authored_defaults": {"Name": "None", "Choice": {"Key": "None"}, "Text": ""},
                    "property_schema": {"Name": {"kind": "name"}, "Choice": {"kind": "selector"}, "Text": {"kind": "value"}}}
        value = lower_authored_properties("/Script/Test.Node",
            {"Name": "", "Choice": {"Key": ""}, "Text": ""}, evidence=evidence)
        self.assertEqual(value, {"Name": "None", "Choice": {"Key": "None"}, "Text": ""})

    def test_enum_aliases_are_inputs_but_extract_uses_canonical_labels(self):
        def evidence(name):
            data = class_evidence(name)
            data["property_schema"]["Mode"]["aliases"] = {"OldSecond": 4}
            data["property_schema"]["Map"]["key_enum_aliases"] = {"OldSecond": 4}
            return data
        value = lower_authored_properties("C", {"Mode": "OldSecond", "Map": {"OldSecond": "First"}}, evidence=evidence)
        extracted = extract_authored_properties("C", value, evidence=evidence)
        self.assertEqual(extracted["Mode"], "Second")
        self.assertEqual(extracted["Map"], {"Second": "First"})

    def test_inline_object_uses_explicit_class_and_declared_base_evidence(self):
        calls = []
        def parent(name):
            return {"node_class_path": name, "authored_defaults": {"Inline": None},
                    "property_schema": {"Inline": {"kind": "instanced_object", "declared_class_path": "/Script/Test.Base"}}}
        def inline(name, base):
            calls.append((name, base))
            return {"node_class_path": name, "authored_defaults": {"Mode": 1, "Count": 5},
                    "property_schema": {"Mode": {"kind": "enum", "values": {"First": 1, "Second": 2}}, "Count": {"kind": "value"}}}
        authored = {"Inline": {"Class": "/Script/Test.Child", "Properties": {"Mode": "Second"}}}
        value = lower_authored_properties("C", authored, evidence=parent, object_evidence=inline)
        self.assertEqual(value["Inline"]["Properties"], {"Mode": 2, "Count": 5})
        self.assertEqual(extract_authored_properties("C", value, evidence=parent, object_evidence=inline)["Inline"]["Properties"]["Mode"], "Second")
        self.assertEqual(calls, [("/Script/Test.Child", "/Script/Test.Base")] * 2)
        with self.assertRaises(AssetDocumentContractError): lower_authored_properties("C", authored, evidence=parent)
        self.assertEqual(lower_authored_properties("C", {"Inline": None}, evidence=parent), {"Inline": None})

    def test_enum_container_and_precision_round_trip_from_public_evidence(self):
        authored = {"Mode": "Second", "Duration": 0.1, "Payload": {"Mode": "Second"},
                    "Array": [{"Mode": "Second"}], "Set": ["Second", "First"], "Map": {"Second": "First"},
                    "Selector": {"Key": "Target"}, "Reference": {"Kind": "AssetRef", "Path": "/Game/A.A"}}
        native = lower_authored_properties("C", authored, evidence=class_evidence)
        self.assertEqual(native["Mode"], 4)
        self.assertEqual(native["Duration"], 0.10000000149011612)
        self.assertEqual(native["Payload"], {"Mode": 4, "Count": 8})
        self.assertEqual(native["Array"], [{"Mode": 4, "Count": 8}])
        self.assertEqual(native["Set"], [1, 4])
        self.assertEqual(native["Map"], {"4": 1})
        extracted = extract_authored_properties("C", native, evidence=class_evidence)
        self.assertEqual(extracted["Map"], {"Second": "First"})
        self.assertEqual(extracted["Set"], ["First", "Second"])
        self.assertEqual(lower_authored_properties("C", extracted, evidence=class_evidence), native)
        self.assertTrue(authored_properties_match("C", authored, native, evidence=class_evidence))

    def test_default_completion_detects_authored_and_omitted_drift(self):
        authored = {"Mode": "Second"}
        native = lower_authored_properties("C", authored, evidence=class_evidence)
        self.assertEqual(set(native), set(class_evidence("C")["authored_defaults"]))
        for name, value in (("Mode", 1), ("Duration", 5.0), ("Payload", {"Mode": 1, "Count": 99})):
            changed = deepcopy(native); changed[name] = value
            self.assertFalse(authored_properties_match("C", authored, changed, evidence=class_evidence))
        self.assertEqual(extract_authored_properties("C", native, evidence=class_evidence, omit_defaults=True), {"Mode": "Second"})
        self.assertEqual(len(extract_authored_properties("C", native, evidence=class_evidence)), len(native))

    def test_dynamic_struct_requires_explicit_type_and_default_evidence(self):
        authored = {"Dynamic": {"Struct": "/Script/Test.Payload", "Properties": {"Mode": "Second"}}}
        native = lower_authored_properties("C", authored, evidence=class_evidence)
        self.assertEqual(native["Dynamic"]["Properties"], {"Mode": 4, "Count": 8})
        self.assertEqual(extract_authored_properties("C", native, evidence=class_evidence)["Dynamic"]["Properties"]["Mode"], "Second")
        authored["Dynamic"]["Struct"] = "/Script/Test.Unrequested"
        with self.assertRaises(AssetDocumentContractError): lower_authored_properties("C", authored, evidence=class_evidence)

    def test_wrong_class_unknown_fields_and_untyped_enum_are_rejected(self):
        with self.assertRaises(AssetDocumentContractError): lower_authored_properties("C", {}, evidence=lambda _: class_evidence("Other"))
        for authored in ({"Missing": 0}, {"Mode": "Missing"}, {"Mode": True}, {"Set": ["First", 1]}, {"Selector": {"Key": "None", "SelectedKeyID": 0}}):
            with self.assertRaises(AssetDocumentContractError): lower_authored_properties("C", authored, evidence=class_evidence)
        observed = lower_authored_properties("C", {}, evidence=class_evidence); del observed["Payload"]["Count"]
        with self.assertRaises(AssetDocumentContractError): extract_authored_properties("C", observed, evidence=class_evidence)

    def test_empty_string_map_key_uses_private_entries_and_round_trips(self):
        def evidence(name):
            return {"node_class_path": name, "authored_defaults": {"Map": {}},
                    "property_schema": {"Map": {"kind": "map", "value": {"kind": "value"}}}}
        authored = {"Map": {"Later": 2, "": 7}}
        native = lower_authored_properties("C", authored, evidence=evidence)
        self.assertEqual(native["Map"], [{"key": "", "value": 7}, {"key": "Later", "value": 2}])
        self.assertEqual(extract_authored_properties("C", native, evidence=evidence), authored)
        unicode_map = {"Map": {"": 7, "\ue000": 8, "\U00010000": 9}}
        unicode_native = lower_authored_properties("C", unicode_map, evidence=evidence)
        self.assertEqual([row["key"] for row in unicode_native["Map"]], ["", "\U00010000", "\ue000"])
        self.assertEqual(extract_authored_properties("C", unicode_native, evidence=evidence), unicode_map)
        self.assertEqual(lower_authored_properties("C", {"Map": {"Later": 2}}, evidence=evidence),
                         {"Map": {"Later": 2}})
        for rows in ([{"key": "A", "value": 1}], [{"key": "", "value": 1}, {"key": "", "value": 2}]):
            with self.assertRaises(AssetDocumentContractError):
                extract_authored_properties("C", {"Map": rows}, evidence=evidence)
        def name_evidence(name):
            row = evidence(name)
            row["property_schema"]["Map"]["key_kind"] = "name"
            return row
        self.assertEqual(lower_authored_properties("C", {"Map": {"": 7}}, evidence=name_evidence),
                         {"Map": {"None": 7}})
        with self.assertRaises(AssetDocumentContractError):
            lower_authored_properties("C", {"Map": {"": 7, "None": 8}}, evidence=name_evidence)

    def test_empty_map_and_array_replace_defaults_instead_of_merging(self):
        def evidence(name):
            value = class_evidence(name)
            value["authored_defaults"]["Map"] = {"1": 4}
            value["authored_defaults"]["Array"] = [{"Mode": 1, "Count": 8}]
            return value
        native = lower_authored_properties("C", {"Map": {}, "Array": []}, evidence=evidence)
        self.assertEqual(native["Map"], {})
        self.assertEqual(native["Array"], [])
        self.assertFalse(authored_properties_match("C", {}, native, evidence=evidence))


class BehaviorTreeSelectorInventoryTests(unittest.TestCase):
    def projection(self, properties):
        return tree_to_semantic_projection({"GraphGuid": guid(100), "Root": {
            "Id": guid(1), "Class": "/Script/Test.Task", "Properties": properties}})

    def test_types_control_discovery_and_property_paths(self):
        selector = {"kind": "selector"}
        fields = {"Direct": selector, "Unrelated": {"kind": "value"},
                  "Nested": {"kind": "struct", "fields": {"Choice": selector}},
                  "Rows": {"kind": "array", "element": {"kind": "struct", "fields": {"Choice": selector}}}}
        properties = {"Direct": {"Key": "None"}, "Unrelated": {"Key": "DoNotGuess"},
                      "Nested": {"Choice": {"Key": "Target"}}, "Rows": [{"Choice": {"Key": "Other"}}]}
        evidence = lambda name: {"node_class_path": name, "authored_defaults": properties, "property_schema": fields}
        self.assertEqual(selector_inventory(self.projection(properties), evidence=evidence), [
            {"stable_node_id": guid(1), "property_path": "Direct", "selected_key_name": "None"},
            {"stable_node_id": guid(1), "property_path": "Nested.Choice", "selected_key_name": "Target"},
            {"stable_node_id": guid(1), "property_path": "Rows.Rows[0].Choice", "selected_key_name": "Other"}])

    def test_map_set_and_direct_array_selector_require_native_locator_evidence(self):
        selector = {"kind": "selector"}
        for shape, value in (({"kind": "map", "value": selector}, {"A": {"Key": "Target"}}),
                             ({"kind": "set", "element": selector}, [{"Key": "Target"}]),
                             ({"kind": "array", "element": selector}, [{"Key": "Target"}])):
            evidence = lambda name: {"node_class_path": name, "authored_defaults": {"Entries": value}, "property_schema": {"Entries": shape}}
            with self.assertRaises(AssetDocumentContractError) as raised:
                selector_inventory(self.projection({"Entries": value}), evidence=evidence)
            self.assertEqual(raised.exception.diagnostic.code, "BehaviorTree.SelectorLocatorEvidenceRequired")

    def test_empty_collections_still_produce_exact_zero_inventory(self):
        properties = {"Items": {}}
        evidence = lambda name: {"node_class_path": name, "authored_defaults": properties,
                                "property_schema": {"Items": {"kind": "map", "value": {"kind": "selector"}}}}
        self.assertEqual(selector_inventory(self.projection(properties), evidence=evidence), [])

    def test_main_service_and_bound_test_owners_share_exact_sorted_inventory(self):
        props = {"Choice": {"Key": "Target"}}
        value = tree_to_semantic_projection(tree_fixture(), property_mapper=lambda _class, _props: deepcopy(props))
        evidence = lambda name: {"node_class_path": name, "authored_defaults": props,
                                "property_schema": {"Choice": {"kind": "selector"}}}
        rows = selector_inventory(value, evidence=evidence)
        self.assertEqual([row["stable_node_id"] for row in rows], [guid(i) for i in (1, 5, 6, 7, 8)])
        self.assertTrue(all(row["property_path"] == "Choice" and row["selected_key_name"] == "Target" for row in rows))


if __name__ == "__main__":
    unittest.main()
