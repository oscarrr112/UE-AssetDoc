import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.graphs.extract import extract_graphs
from asset_document.graphs.live import LiveAsset, LiveGraph, LiveLink, LiveNode, LivePinRef
from asset_document.graphs.reconcile import reconcile
from asset_document.graphs.schemas.anim import (
    ANIM_ADAPTER, anim_label, decode_anim_graph, decode_anim_layers, encode_anim_graph, omitted_anim_settings,
    sample_name,
)
from asset_document.profiles import anim_blueprint_runtime as runtime
from asset_document.profiles.anim_blueprint_runtime import (
    ANIM_BLUEPRINT_CLASS, ANIM_BLUEPRINT_RUNTIME_REGISTRATION, canonicalize, plan_header, plan_settings,
)
from asset_document.registered_runtime import _context_names_match

TARGET = "/Game/Tests/ABP_Hero"
SELF = "/Game/Tests/ABP_Hero.ABP_Hero_C"
SKELETON = "/Game/Characters/SK_Hero.SK_Hero"
SETTING = "blueprint.member.setting.set.v1"


def document(body, action="CreateOrUpdate", properties=None):
    return {"SchemaVersion": 1, "Target": TARGET, "Class": ANIM_BLUEPRINT_CLASS, "Action": action,
            "Properties": properties or {}, "Body": body}


def code_of(call):
    try:
        call()
    except AssetDocumentContractError as error:
        return error.diagnostic.code, error.diagnostic.path, error.diagnostic.message
    raise AssertionError("expected a contract error")


LOCOMOTION = {"Nodes": [
    {"Id": "Output"},
    {"Id": "Loco", "Action": "Anim:StateMachine", "Graph": {"Nodes": [
        {"Id": "Entry"},
        {"Id": "Idle", "Action": "SM:State", "Graph": {
            "Nodes": [{"Id": "Output"}, {"Id": "Play", "Action": "Anim:SequencePlayer",
                                         "Properties": {"Node.PlayRate": 1.5}}],
            "Links": [{"From": "Play:Pose", "To": "Output:Result"}]}},
        {"Id": "Walk", "Action": "SM:State", "Name": "Walking"},
        {"Id": "Go", "Action": "SM:Transition", "From": "Idle", "To": "Walk", "Graph": {
            "Nodes": [{"Id": "Result"}, {"Id": "Moving", "Action": f"K2:VariableGet:{SELF}:bMoving"}],
            "Links": [{"From": "Moving:bMoving", "To": "Result:bCanEnterTransition"}]}}],
        "Links": [{"From": "Entry:Entry", "To": "Idle:In"}]}}],
    "Links": [{"From": "Loco:Pose", "To": "Output:Result"}]}


class AnimGraphCodecTests(unittest.TestCase):
    def test_nested_graphs_and_transition_sugar(self):
        graph = decode_anim_graph(LOCOMOTION, self_class=SELF)
        self.assertEqual((graph.id, graph.role, graph.name), ("AnimGraph/AnimGraph", "anim_graph", "AnimGraph"))
        machine = graph.subgraphs[0]
        self.assertEqual((machine.role, machine.name, machine.owner, machine.schema_tag),
                         ("state_machine", "Loco", "Loco", "SM"))
        self.assertEqual([f"{link.from_}->{link.to}" for link in machine.links],
                         ["Entry:Entry->Idle:In", "Idle:Out->Go:In", "Go:Out->Walk:In"])
        transition = next(node for node in machine.nodes if node.id == "Go")
        self.assertEqual(dict(transition.extras), {"from": "Idle", "to": "Walking"})
        self.assertEqual(next(node for node in machine.nodes if node.id == "Idle").name, "Idle")
        self.assertEqual([(sub.name, sub.role, sub.owner) for sub in machine.subgraphs],
                         [("Idle", "state", "Idle"), ("Idle->Walking", "transition", "Go")])
        self.assertEqual(next(node for node in machine.nodes if node.id == "Entry").implicit, "sm_entry")

    def test_encode_is_the_inverse_of_decode(self):
        self.assertEqual(encode_anim_graph(decode_anim_graph(LOCOMOTION, self_class=SELF)), LOCOMOTION)

    def test_reserved_nodes_used_only_in_links_are_implicit(self):
        graph = decode_anim_graph({"Nodes": [{"Id": "Play", "Action": "Anim:SequencePlayer"}],
                                   "Links": [{"From": "Play:Pose", "To": "Output:Result"}]})
        self.assertEqual(next(node for node in graph.nodes if node.id == "Output").implicit, "root")

    def test_duplicate_transitions_get_distinct_subgraph_names(self):
        value = {"Nodes": [{"Id": "M", "Action": "Anim:StateMachine", "Graph": {"Nodes": [
            {"Id": "A", "Action": "SM:State"}, {"Id": "B", "Action": "SM:State"},
            {"Id": "T1", "Action": "SM:Transition", "From": "A", "To": "B", "Graph": {"Nodes": [], "Links": []}},
            {"Id": "T2", "Action": "SM:Transition", "From": "A", "To": "B", "Graph": {"Nodes": [], "Links": []}}],
            "Links": []}}], "Links": []}
        machine = decode_anim_graph(value).subgraphs[0]
        self.assertEqual([sub.name for sub in machine.subgraphs], ["A->B", "A->B #2"])

    def test_anim_label_identifies_named_nodes_and_transitions(self):
        machine = decode_anim_graph(LOCOMOTION, self_class=SELF).subgraphs[0]
        labels = {node.id: anim_label(node) for node in machine.nodes}
        self.assertEqual(labels["Walk"], ("", "SM:State", "walking"))
        self.assertEqual(labels["Go"], ("", "SM:Transition", "idle->walking"))

    def test_invalid_shapes_fail_with_stable_codes(self):
        machine = lambda nodes: {"Nodes": [{"Id": "M", "Action": "Anim:StateMachine",  # noqa: E731
                                            "Graph": {"Nodes": nodes, "Links": []}}], "Links": []}
        cases = (
            (machine([{"Id": "A", "Action": "SM:State", "From": "A"}]), "Graph.UnknownField"),
            (machine([{"Id": "A", "Action": "SM:State"}, {"Id": "T", "Action": "SM:Transition", "From": "A"}]),
             "Graph.MissingField"),
            (machine([{"Id": "A", "Action": "SM:State"},
                      {"Id": "T", "Action": "SM:Transition", "From": "A", "To": "Nowhere"}]), "Graph.UnknownNode"),
            ({"Nodes": [{"Id": "B", "Action": "Anim:TwoWayBlend", "Graph": {"Nodes": []}}], "Links": []},
             "Graph.UnknownField"),
            ({"Name": "Other", "Nodes": [], "Links": []}, "Graph.InvalidGraphName"),
        )
        for value, code in cases:
            with self.subTest(code=code):
                self.assertEqual(code_of(lambda: decode_anim_graph(value))[0], code)

    def test_layers_are_named_and_unique(self):
        layers = decode_anim_layers([{"Name": "Upper", "Nodes": [], "Links": []}])
        self.assertEqual([(layer.id, layer.role, layer.name) for layer in layers],
                         [("AnimLayers/Upper", "anim_layer", "Upper")])
        self.assertEqual(encode_anim_graph(layers[0]), {"Name": "Upper", "Nodes": [], "Links": []})
        duplicate = [{"Name": "Upper", "Nodes": []}, {"Name": "upper", "Nodes": []}]
        self.assertEqual(code_of(lambda: decode_anim_layers(duplicate))[0], "Graph.DuplicateGraphName")


BLEND = {"Nodes": [
    {"Id": "Output"},
    {"Id": "Loco", "Action": "Anim:BlendSpaceGraph:BlendSpace",
     "Properties": {"BlendSpace.BlendParameters.X.Max": 300.0},
     "Samples": [{"Name": "Idle", "X": 0, "Y": 0, "Graph": {
                     "Nodes": [{"Id": "Output"}, {"Id": "Play", "Action": "Anim:SequencePlayer"}],
                     "Links": [{"From": "Play:Pose", "To": "Output:Result"}]}},
                 {"Name": "Walk", "X": 150, "Y": 0}]}],
    "Links": [{"From": "Loco:Pose", "To": "Output:Result"}]}
CUSTOM = {"Nodes": [{"Id": "M", "Action": "Anim:StateMachine", "Graph": {"Nodes": [
    {"Id": "A", "Action": "SM:State"}, {"Id": "B", "Action": "SM:State"},
    {"Id": "T", "Action": "SM:Transition", "From": "A", "To": "B", "Properties": {"CrossfadeDuration": 0.5},
     "BlendGraph": {"Nodes": [{"Id": "Output"}, {"Id": "SourcePose"}, {"Id": "DestinationPose"},
                              {"Id": "Blend", "Action": "Anim:TwoWayBlend"}],
                    "Links": [{"From": "SourcePose:Pose", "To": "Blend:A"},
                              {"From": "DestinationPose:Pose", "To": "Blend:B"},
                              {"From": "Blend:Pose", "To": "Output:Result"}]}}],
    "Links": []}}], "Links": []}


class BlendGraphCodecTests(unittest.TestCase):
    def test_custom_blend_graph_is_an_owned_subgraph_implying_custom_logic(self):
        machine = decode_anim_graph(CUSTOM).subgraphs[0]
        transition = next(node for node in machine.nodes if node.id == "T")
        self.assertEqual(dict(transition.properties.node), {"CrossfadeDuration": 0.5, "LogicType": "TLT_Custom"})
        blend = machine.subgraphs[0]
        self.assertEqual((blend.name, blend.role, blend.owner), ("A->B (custom_transition)", "custom_transition", "T"))
        self.assertEqual({node.id: node.implicit for node in blend.nodes if node.implicit},
                         {"Output": "root", "SourcePose": "source_pose", "DestinationPose": "destination_pose"})
        self.assertEqual(encode_anim_graph(decode_anim_graph(CUSTOM)), CUSTOM)

    def test_custom_blend_graph_rules(self):
        def with_transition(**fields):
            row = {"Id": "T", "Action": "SM:Transition", "From": "A", "To": "B", **fields}
            return {"Nodes": [{"Id": "M", "Action": "Anim:StateMachine", "Graph": {"Nodes": [
                {"Id": "A", "Action": "SM:State"}, {"Id": "B", "Action": "SM:State"}, row], "Links": []}}],
                "Links": []}
        self.assertEqual(code_of(lambda: decode_anim_graph(with_transition(
            BlendGraph={"Nodes": []}, Properties={"LogicType": "TLT_StandardBlend"})))[0], "Graph.ConflictingSetting")
        self.assertEqual(code_of(lambda: decode_anim_graph(
            {"Nodes": [{"Id": "S", "Action": "Anim:StateMachine", "BlendGraph": {"Nodes": []}}]}))[0],
            "Graph.UnknownField")
        # An explicit TLT_Custom is accepted and folded away on encode.
        value = with_transition(BlendGraph={"Nodes": [], "Links": []}, Properties={"LogicType": "TLT_Custom"})
        encoded = encode_anim_graph(decode_anim_graph(value))
        self.assertNotIn("Properties", encoded["Nodes"][0]["Graph"]["Nodes"][2])

    def test_blend_space_samples_and_sample_graphs(self):
        graph = decode_anim_graph(BLEND)
        node = next(node for node in graph.nodes if node.id == "Loco")
        self.assertEqual(node.name, "Loco")
        self.assertEqual(node.properties.node["Samples"],
                         ({"Name": "Idle", "X": 0, "Y": 0}, {"Name": "Walk", "X": 150, "Y": 0}))
        self.assertEqual([(sub.name, sub.role, sub.owner) for sub in graph.subgraphs],
                         [("Loco/Idle", "blend_space_sample", "Loco")])
        self.assertEqual(anim_label(node), ("", "Anim:BlendSpaceGraph:BlendSpace", "loco"))
        self.assertEqual(encode_anim_graph(graph), BLEND)
        # Samples are a set keyed by name: listed order does not matter.
        reordered = {**BLEND, "Nodes": [BLEND["Nodes"][0], {**BLEND["Nodes"][1],
                                                            "Samples": BLEND["Nodes"][1]["Samples"][::-1]}]}
        self.assertEqual(encode_anim_graph(decode_anim_graph(reordered)), BLEND)

    def test_sample_rules(self):
        def blend(samples, action="Anim:BlendSpaceGraph:BlendSpace", **fields):
            return {"Nodes": [{"Id": "B", "Action": action, "Samples": samples, **fields}], "Links": []}
        cases = (
            (blend([{"Name": "Idle", "X": 0}, {"Name": "idle", "X": 1}]), "Graph.DuplicateGraphName"),
            (blend([{"Name": "Idle Walk", "X": 0}]), "Graph.InvalidGraphName"),
            (blend([{"Name": "A/B", "X": 0}]), "Graph.InvalidGraphName"),
            (blend([{"Name": "Idle"}]), "Graph.InvalidType"),
            (blend([{"Name": "Idle", "X": 0, "Z": 1}]), "Graph.UnknownField"),
            (blend([], action="Anim:SequencePlayer"), "Graph.UnknownField"),
            (blend([], Properties={"Samples": []}), "Graph.ConflictingSetting"),
        )
        for value, code in cases:
            with self.subTest(code=code, value=value):
                self.assertEqual(code_of(lambda: decode_anim_graph(value))[0], code)

    def test_sample_identity(self):
        self.assertEqual(sample_name("Loco/Idle"), "Idle")
        self.assertEqual(sample_name("Loco/Idle #2"), "Idle")
        self.assertEqual(ANIM_ADAPTER.subgraph_identity("blend_space_sample", "Loco/Walk"), "Walk")
        self.assertIsNone(ANIM_ADAPTER.subgraph_identity("transition", "A->B"))


def _live_node(guid, action=None, implicit=None, extras=None, subgraphs=()):
    return LiveNode(guid=guid, class_path="/Script/AnimGraph.AnimGraphNode_Base", action_key=action, title=guid,
                    implicit_role=implicit, position=None, comment="", enabled_state="Enabled", parent_guid=None,
                    subnode_index=None, subgraph_guids=tuple(subgraphs), can_add_pin=False, extras=extras or {},
                    pins=())


def _guid(index):
    return f"{index:032X}"


class LiveBlendSpaceTests(unittest.TestCase):
    """Extract / reconcile of a live blend space and of duplicate transitions (no editor)."""

    def live(self, samples=("Walk", "Idle")):
        rows = [{"Name": name, "X": float(index), "Y": 0.0} for index, name in enumerate(samples)]
        sample_graphs = tuple(
            LiveGraph(guid=_guid(100 + index), path=f"AnimGraph/BlendSpaceGraph/{name}", role="blend_space_sample",
                      schema_tag="Anim", nodes=(_live_node(_guid(200 + index), implicit="root"),), links=(),
                      name=name, parent_guid=_guid(1), owner_node_guid=_guid(3))
            for index, name in enumerate(samples))
        owner = _live_node(_guid(3), "Anim:BlendSpaceGraph:BlendSpace",
                           extras={"name": "Loco", "properties": {"Samples": rows}},
                           subgraphs=[graph.guid for graph in sample_graphs])
        anim = LiveGraph(guid=_guid(1), path="AnimGraph", role="anim_graph", schema_tag="Anim",
                         nodes=(_live_node(_guid(2), implicit="root"), owner), links=(), name="AnimGraph")
        return LiveAsset((anim,) + sample_graphs)

    def extract(self, live):
        return extract_graphs(live, sections={"anim_graph": "AnimGraph"}, adapter=ANIM_ADAPTER)

    def test_extract_nests_sample_graphs_under_their_samples(self):
        body = encode_anim_graph(self.extract(self.live())[0])
        node = body["Nodes"][1]
        self.assertEqual(node["Action"], "Anim:BlendSpaceGraph:BlendSpace")
        self.assertNotIn("Name", node)  # named after its id
        self.assertEqual([(row["Name"], row["X"], "Graph" in row) for row in node["Samples"]],
                         [("Idle", 1.0, True), ("Walk", 0.0, True)])

    def test_extract_then_reconcile_is_empty(self):
        live = self.live()
        graphs = self.extract(live)
        roles = frozenset({"anim_graph"})
        self.assertEqual(reconcile(graphs, live, managed_roles=roles, adapter=ANIM_ADAPTER).briefs(), [])
        decoded = (decode_anim_graph(encode_anim_graph(graphs[0])),)
        self.assertEqual(reconcile(decoded, live, managed_roles=roles, adapter=ANIM_ADAPTER).briefs(), [])

    def test_sample_graphs_match_by_sample_name(self):
        # A renamed sample (Walk -> Run) is not matched to the Walk graph: its graph is synced after the
        # Samples setting made it, and the Walk graph is left to the Samples setting to remove.
        live = self.live()
        body = encode_anim_graph(self.extract(live)[0])
        body["Nodes"][1]["Samples"][1]["Name"] = "Run"
        body["Nodes"][1]["Samples"][1]["Graph"]["Nodes"].append({"Id": "Play", "Action": "Anim:SequencePlayer"})
        ops = reconcile((decode_anim_graph(body),), live, managed_roles=frozenset({"anim_graph"}),
                        adapter=ANIM_ADAPTER).briefs()
        names = [op[0] for op in ops]
        self.assertIn("graph.subgraph.sync", names)
        self.assertLess(names.index("graph.node.property.set"), names.index("graph.subgraph.sync"))
        sync = next(op for op in ops if op[0] == "graph.subgraph.sync")
        self.assertEqual(sync[2]["name"], "Loco/Run")
        self.assertNotIn("graph.remove", names)

    def duplicate_transitions(self):
        transitions = tuple(
            _live_node(_guid(10 + index), "SM:Transition", extras={"from": "A", "to": "B"},
                       subgraphs=[_guid(20 + index)]) for index in range(2))
        states = tuple(_live_node(_guid(30 + index), "SM:State", extras={"name": name}) for index, name in
                       enumerate("AB"))
        links = tuple(link for index in range(2) for link in (
            LiveLink(from_=LivePinRef(_guid(30), "Out"), to=LivePinRef(_guid(10 + index), "In")),
            LiveLink(from_=LivePinRef(_guid(10 + index), "Out"), to=LivePinRef(_guid(31), "In"))))
        machine = LiveGraph(guid=_guid(5), path="AnimGraph/M", role="state_machine", schema_tag="SM",
                            nodes=states + transitions, links=links, name="M", parent_guid=_guid(1),
                            owner_node_guid=_guid(4))
        rules = tuple(LiveGraph(guid=_guid(20 + index), path=f"AnimGraph/M/Rule{index}", role="transition",
                                schema_tag="Anim", nodes=(), links=(), name=f"Rule{index}", parent_guid=_guid(5),
                                owner_node_guid=_guid(10 + index)) for index in range(2))
        anim = LiveGraph(guid=_guid(1), path="AnimGraph", role="anim_graph", schema_tag="Anim",
                         nodes=(_live_node(_guid(4), "Anim:StateMachine", extras={"name": "M"},
                                           subgraphs=[_guid(5)]),), links=(), name="AnimGraph")
        return LiveAsset((anim, machine) + rules)

    @staticmethod
    def _without_guids(value):
        if isinstance(value, dict):
            return {key: LiveBlendSpaceTests._without_guids(item) for key, item in value.items()
                    if key not in ("NodeGuid", "GraphGuid")}
        if isinstance(value, list):
            return [LiveBlendSpaceTests._without_guids(item) for item in value]
        return value

    def test_duplicate_transitions_without_guids_match_in_order(self):
        # An authored document (no NodeGuid) with two A->B transitions keeps both live transitions.
        live = self.duplicate_transitions()
        body = self._without_guids(encode_anim_graph(self.extract(live)[0]))
        roles = frozenset({"anim_graph"})
        ops = reconcile((decode_anim_graph(body),), live, managed_roles=roles, adapter=ANIM_ADAPTER).briefs()
        self.assertEqual([op for op in ops if op[0] in ("graph.node.remove", "graph.node.spawn")], [])
        # Dropping the second one removes the second live transition and keeps the first (and its rule graph).
        machine = body["Nodes"][0]["Graph"]
        second = [index for index, node in enumerate(machine["Nodes"]) if node.get("Action") == "SM:Transition"][1]
        del machine["Nodes"][second]
        plan = reconcile((decode_anim_graph(body),), live, managed_roles=roles, adapter=ANIM_ADAPTER)
        removed = [op for op in plan.briefs() if op[0] == "graph.node.remove"]
        self.assertEqual([op[2]["node_guid"] for op in removed], [_guid(11)])
        self.assertNotIn("graph.node.spawn", [op[0] for op in plan.briefs()])
        self.assertIn(_guid(10), plan.node_bindings.values())

    def test_duplicate_sibling_subgraph_names_are_suffixed(self):
        transitions = tuple(
            _live_node(_guid(10 + index), "SM:Transition", extras={"from": "A", "to": "B"},
                       subgraphs=[_guid(20 + index)]) for index in range(2))
        states = tuple(_live_node(_guid(30 + index), "SM:State", extras={"name": name}) for index, name in
                       enumerate("AB"))
        machine = LiveGraph(guid=_guid(5), path="AnimGraph/M", role="state_machine", schema_tag="SM",
                            nodes=states + transitions, links=(), name="M", parent_guid=_guid(1),
                            owner_node_guid=_guid(4))
        rules = tuple(LiveGraph(guid=_guid(20 + index), path=f"AnimGraph/M/Rule{index}", role="transition",
                                schema_tag="Anim", nodes=(), links=(), name=f"Rule{index}", parent_guid=_guid(5),
                                owner_node_guid=_guid(10 + index)) for index in range(2))
        anim = LiveGraph(guid=_guid(1), path="AnimGraph", role="anim_graph", schema_tag="Anim",
                         nodes=(_live_node(_guid(4), "Anim:StateMachine", extras={"name": "M"},
                                           subgraphs=[_guid(5)]),), links=(), name="AnimGraph")
        graphs = self.extract(LiveAsset((anim, machine) + rules))
        self.assertEqual([sub.name for sub in graphs[0].subgraphs[0].subgraphs], ["A->B", "A->B #2"])


class CanonicalTests(unittest.TestCase):
    def test_legacy_body_keys_point_to_their_replacement(self):
        for key, hint in (("BlueprintClassSettings", "Properties"), ("StateMachines", "nested Graph"),
                          ("DefaultBindingClass", "Properties.DefaultBindingClass")):
            with self.subTest(key=key):
                code, path, message = code_of(lambda: canonicalize(document({key: {}})))
                self.assertEqual((code, path), ("AnimBlueprint.UnknownBodyKey", f"/Body/{key}"))
                self.assertIn(hint, message)

    def test_create_requires_parent_class_then_skeleton(self):
        self.assertEqual(code_of(lambda: canonicalize(document({"TargetSkeleton": SKELETON}, "Create")))[:2],
                         ("AnimBlueprint.ParentClassRequired", "/Body/ParentClass"))
        self.assertEqual(code_of(lambda: canonicalize(document({"ParentClass": "/Script/Engine.AnimInstance"},
                                                               "Create")))[0],
                         "AnimBlueprint.MissingTargetSkeleton")
        template = canonicalize(document({"ParentClass": "/Script/Engine.AnimInstance", "Template": True}, "Create"))
        self.assertTrue(template["Body"]["Template"])
        self.assertEqual(code_of(lambda: canonicalize(document({"Template": True, "TargetSkeleton": SKELETON})))[0],
                         "AnimBlueprint.InvalidTemplateSkeleton")

    def test_settings_and_legacy_spellings_are_canonical(self):
        body = canonicalize(document({
            "Template": {"bIsTemplate": False, "BlueprintType": "Normal"},
            "Preview": {"PreviewSkeletalMesh": {"Kind": "AssetRef", "Asset": "/Game/Characters/SKM_Hero"},
                        "ApplicationMethod": "LinkedLayers", "Tag": ""},
            "SyncGroups": [{"Name": "Feet"}, "Hands"],
            "ParentAssetOverrides": [{"ParentNodeGuid": "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb", "Asset": None},
                                     {"ParentNodeGuid": "A" * 32, "Asset": "/Game/Anims/Run"}],
            "Thumbnail": {"OrbitPitch": -15, "OrbitYaw": 45, "OrbitZoom": 10},
        }, properties={"TargetSkeleton": SKELETON}))
        self.assertNotIn("TargetSkeleton", body["Properties"])
        body = body["Body"]
        self.assertEqual(body["TargetSkeleton"], SKELETON)
        self.assertIs(body["Template"], False)
        self.assertEqual(body["Preview"], {"Mesh": "/Game/Characters/SKM_Hero.SKM_Hero"})
        self.assertEqual(body["SyncGroups"], ["Feet", "Hands"])
        self.assertEqual(body["ParentAssetOverrides"], [
            {"ParentNodeGuid": "A" * 32, "Asset": "/Game/Anims/Run.Run"},
            {"ParentNodeGuid": "B" * 32, "Asset": None}])
        self.assertEqual(body["Thumbnail"], {"OrbitPitch": -15.0, "OrbitYaw": 45.0, "OrbitZoom": 10.0})
        self.assertEqual(canonicalize(document(body)), canonicalize(document(body)))

    def test_unsupported_regions_and_values_are_rejected(self):
        for body, code in (({"Components": [{"Name": "Mesh"}]}, "AnimBlueprint.UnsupportedRegion"),
                           ({"Timelines": [{"Name": "T"}]}, "AnimBlueprint.UnsupportedRegion"),
                           ({"DelegateSignatureGraphs": [{"Name": "OnHit", "Nodes": [{"Id": "Call",
                                                                                      "Action": "K2:CallFunction"}]}]},
                            "UBlueprint.InvalidBodyRegion"),
                           ({"Graphs": []}, "AnimBlueprint.UnknownBodyKey"),
                           ({"SyncGroups": [{"Name": "Feet", "Color": 1}]}, "AnimBlueprint.UnsupportedRegion"),
                           ({"Preview": {"ApplicationMethod": "Copy"}}, "AnimBlueprint.InvalidDocument"),
                           ({"Thumbnail": {"OrbitYaw": 1}}, "AnimBlueprint.InvalidDocument")):
            with self.subTest(body=body):
                self.assertEqual(code_of(lambda: canonicalize(document(body)))[0], code)
        self.assertEqual(canonicalize(document({"Components": [], "Timelines": [], "DelegateSignatureGraphs": []}))["Body"],
                         {"EventDispatchers": []})


INTERFACE = "/Script/Engine.AnimLayerInterface"


class DiffDefaultsTests(unittest.TestCase):
    def test_extracted_nodes_state_known_defaults(self):
        extracted = {"Nodes": [
            {"Id": "Blend", "Action": "Anim:TwoWayBlend"},
            {"Id": "M", "Action": "Anim:StateMachine", "Graph": {"Nodes": [
                {"Id": "T", "Action": "SM:Transition", "From": "A", "To": "B"},
                {"Id": "C", "Action": "SM:Transition", "From": "B", "To": "A", "BlendGraph": {"Nodes": []}}]}}]}
        filled = runtime.with_known_defaults(extracted)
        self.assertEqual(filled["Nodes"][0]["Properties"], {"Bindings": {}})
        machine = filled["Nodes"][1]["Graph"]["Nodes"]
        self.assertEqual(machine[0]["Properties"], {"LogicType": "TLT_StandardBlend", "PriorityOrder": 1})
        self.assertEqual(machine[1]["Properties"], {"PriorityOrder": 1})  # a custom blend graph implies TLT_Custom
        self.assertNotIn("Properties", extracted["Nodes"][0])  # the input is not changed

    def test_input_poses_are_matched_by_pose_name(self):
        layer = decode_anim_layers([{"Name": "L", "Nodes": [
            {"Id": "A", "Action": "Anim:LinkedInputPose", "Properties": {"Node.Name": "BasePose"}},
            {"Id": "B", "Action": "Anim:LinkedInputPose"}]}])[0]
        keys = [ANIM_ADAPTER.semantic_key(node, node.action) for node in layer.nodes if node.action]
        self.assertEqual(keys, [("Anim:LinkedInputPose", "basepose"), ("Anim:LinkedInputPose", "inpose")])


class OmittedSettingsTests(unittest.TestCase):
    """An animation node setting the document leaves out is the node's default."""

    def live(self, properties, defaults):
        player = _live_node(_guid(3), "Anim:SequencePlayer",
                            extras={"properties": properties, "property_defaults": defaults})
        anim = LiveGraph(guid=_guid(1), path="AnimGraph", role="anim_graph", schema_tag="Anim",
                         nodes=(_live_node(_guid(2), implicit="root"), player), links=(), name="AnimGraph")
        return LiveAsset((anim,))

    def plan(self, live, nodes, links=()):
        body = {"Nodes": [{"Id": "Output"}, *nodes], "Links": list(links)}
        return reconcile((decode_anim_graph(body),), live, managed_roles=frozenset({"anim_graph"}),
                         adapter=ANIM_ADAPTER).briefs()

    def test_an_omitted_setting_goes_back_to_its_default(self):
        live = self.live({"Node.PlayRate": 2.0, "Node.bLoopAnimation": False, "ShowPin.StartPosition": True},
                         {"Node.PlayRate": 1.0, "Node.bLoopAnimation": True, "ShowPin.StartPosition": False})
        ops = self.plan(live, [{"Id": "Play", "Action": "Anim:SequencePlayer", "Properties": {"Node.PlayRate": 2.0}}])
        sets = sorted((op[2]["property_path"], op[2]["value"]) for op in ops if op[0] == "graph.node.property.set")
        self.assertEqual(sets, [("Node.bLoopAnimation", True), ("ShowPin.StartPosition", False)])
        # The complete extraction plans nothing; a pin the document links stays shown.
        stated = {"Node.PlayRate": 2.0, "Node.bLoopAnimation": False, "ShowPin.StartPosition": True}
        self.assertEqual(self.plan(live, [{"Id": "Play", "Action": "Anim:SequencePlayer", "Properties": stated}]), [])
        self.assertEqual(omitted_anim_settings({"ShowPin.StartPosition": True}, {"ShowPin.StartPosition": False}, {},
                                               frozenset({"startposition"})), {})
        self.assertEqual(omitted_anim_settings({"ShowPin.StartPosition": True}, {"ShowPin.StartPosition": False},
                                               {"Bindings": {"StartPosition": {"Path": "Start"}}}), {})

    def test_hiding_a_pin_follows_the_links(self):
        # A setting that hides a pin is planned after the graph's links, so the links the pin had are broken first.
        live = self.live({"ShowPin.StartPosition": True, "Node.bLoopAnimation": False},
                         {"ShowPin.StartPosition": False, "Node.bLoopAnimation": True})
        ops = self.plan(live, [{"Id": "Play", "Action": "Anim:SequencePlayer"}])
        paths = [op[2]["property_path"] for op in ops if op[0] == "graph.node.property.set"]
        self.assertEqual(paths, ["Node.bLoopAnimation", "ShowPin.StartPosition"])
        self.assertTrue(ANIM_ADAPTER.hides_pin("Anim:ControlRig", "node", "ShowPin.Gain", False))
        self.assertFalse(ANIM_ADAPTER.hides_pin("Anim:ControlRig", "node", "ShowPin.Gain", True))
        self.assertFalse(ANIM_ADAPTER.hides_pin("K2:Get:Speed", "node", "ShowPin.Gain", False))
        # A missing pin on an animation node points at ShowPin.
        self.assertIn('"ShowPin.Gain": true', ANIM_ADAPTER.missing_pin_hint(_live_node(_guid(4), "Anim:ControlRig"), "Gain"))
        self.assertEqual(ANIM_ADAPTER.missing_pin_hint(_live_node(_guid(5), "K2:Get:Speed"), "Gain"), "")

    def test_settings_without_a_default_are_kept(self):
        live = self.live({"Node.PlayRate": 2.0, "Samples": []}, {})
        self.assertEqual([op for op in self.plan(live, [{"Id": "Play", "Action": "Anim:SequencePlayer"}])
                          if op[0] == "graph.node.property.set"], [])

    def test_diff_reports_non_default_settings_the_document_leaves_out(self):
        authored = {"Nodes": [{"Id": "Output"}, {"Id": "Play", "Action": "Anim:SequencePlayer"}],
                    "Links": [{"From": "Play:Pose", "To": "Output:Result"}]}
        extracted = {"Nodes": [{"Id": "Output"}, {"Id": "Play", "Action": "Anim:SequencePlayer",
                                                  "Properties": {"Node.PlayRate": 2.0}}],
                     "Links": [{"From": "Play:Pose", "To": "Output:Result"}]}
        found = runtime.omitted_settings(decode_anim_graph(authored), decode_anim_graph(extracted))
        self.assertEqual(found, ["Play: Node.PlayRate"])
        self.assertEqual(runtime.omitted_settings(decode_anim_graph(extracted), decode_anim_graph(extracted)), [])


class ControlRigSpellingTests(unittest.TestCase):
    """A Control Rig node's rig is "Node.ControlRigClass" on some engines and "Node.ControlRigAssetReference" on
    others: either spelling applies to either node, and extraction keeps the node's own."""

    RIG = "/Game/Rigs/CR_Arm.CR_Arm_C"
    REFERENCE = {"BlueprintRigClass": RIG, "ControlRigAsset": None}

    def live(self, properties, defaults):
        rig = _live_node(_guid(3), "Anim:ControlRig", extras={"properties": properties, "property_defaults": defaults})
        anim = LiveGraph(guid=_guid(1), path="AnimGraph", role="anim_graph", schema_tag="Anim",
                         nodes=(_live_node(_guid(2), implicit="root"), rig), links=(), name="AnimGraph")
        return LiveAsset((anim,))

    def plan(self, live, properties):
        body = {"Nodes": [{"Id": "Output"}, {"Id": "Rig", "Action": "Anim:ControlRig", "Properties": properties}],
                "Links": []}
        return [op for op in reconcile((decode_anim_graph(body),), live, managed_roles=frozenset({"anim_graph"}),
                                       adapter=ANIM_ADAPTER).briefs() if op[0] == "graph.node.property.set"]

    def test_either_spelling_matches_either_node(self):
        by_class = self.live({"Node.ControlRigClass": self.RIG}, {"Node.ControlRigClass": None})
        by_reference = self.live({"Node.ControlRigAssetReference": self.REFERENCE},
                                 {"Node.ControlRigAssetReference": {"BlueprintRigClass": None, "ControlRigAsset": None}})
        for live in (by_class, by_reference):
            self.assertEqual(self.plan(live, {"Node.ControlRigClass": self.RIG}), [])
            self.assertEqual(self.plan(live, {"Node.ControlRigAssetReference": self.REFERENCE}), [])
            # Another rig is written in the reference spelling (the editor sets the property its node has); an
            # omitted rig goes back to none.
            other = self.plan(live, {"Node.ControlRigClass": "/Game/Rigs/CR_Leg.CR_Leg_C"})
            self.assertEqual([(op[2]["property_path"], op[2]["value"]) for op in other],
                             [("Node.ControlRigAssetReference",
                               {"BlueprintRigClass": "/Game/Rigs/CR_Leg.CR_Leg_C", "ControlRigAsset": None})])
            omitted = self.plan(live, {})
            self.assertEqual([(op[2]["property_path"], op[2]["value"]) for op in omitted],
                             [("Node.ControlRigAssetReference", {"BlueprintRigClass": None, "ControlRigAsset": None})])
        # A node that reports its retired class field (empty) next to the reference is read by the reference.
        both = self.live({"Node.ControlRigAssetReference": self.REFERENCE},
                         {"Node.ControlRigAssetReference": {"BlueprintRigClass": None, "ControlRigAsset": None}})
        both.graphs[0].nodes[1].extras["properties"]["Node.ControlRigClass"] = None
        self.assertEqual(self.plan(both, {"Node.ControlRigClass": self.RIG}), [])
        # Extraction reports the node's own spelling.
        extracted = encode_anim_graph(extract_graphs(by_class, sections={"anim_graph": "AnimGraph"},
                                                     adapter=ANIM_ADAPTER)[0])
        self.assertEqual(extracted["Nodes"][1]["Properties"], {"Node.ControlRigClass": self.RIG})

    def test_both_spellings_must_agree(self):
        body = {"Nodes": [{"Id": "Rig", "Action": "Anim:ControlRig", "Properties": {
            "Node.ControlRigClass": self.RIG, "Node.ControlRigAssetReference": self.REFERENCE}}]}
        node = decode_anim_graph(body).nodes[0]
        self.assertEqual(dict(node.properties.node), {"Node.ControlRigAssetReference": node.properties.node[
            "Node.ControlRigAssetReference"]})
        body["Nodes"][0]["Properties"]["Node.ControlRigClass"] = "/Game/Rigs/CR_Leg.CR_Leg_C"
        code, path, _ = code_of(lambda: decode_anim_graph(body))
        self.assertEqual((code, path), ("Graph.ConflictingSetting", "/Body/AnimGraph/Nodes/0/Properties/Node.ControlRigClass"))


class StateMachineOmittedSettingsTests(unittest.TestCase):
    """A state machine node setting the document leaves out is a newly placed node's (as Anim: nodes)."""

    @staticmethod
    def machine(transition_properties=None, alias=None):
        row = {"Id": "T", "Action": "SM:Transition", "From": "A", "To": "B"}
        if transition_properties is not None:
            row["Properties"] = transition_properties
        nodes = [{"Id": "A", "Action": "SM:State"}, {"Id": "B", "Action": "SM:State"}, row]
        if alias is not None:
            nodes.append(alias)
        return {"Nodes": [{"Id": "Output"}, {"Id": "M", "Action": "Anim:StateMachine", "Graph": {"Nodes": nodes,
                                                                                               "Links": []}}],
                "Links": [{"From": "M:Pose", "To": "Output:Result"}]}

    def test_an_omitted_setting_goes_back_to_the_new_nodes(self):
        machine = decode_anim_graph(self.machine({"CrossfadeDuration": 0.5},
                                                 {"Id": "Any", "Action": "SM:StateAlias"})).subgraphs[0]
        transition = next(node for node in machine.nodes if node.id == "T")
        alias = next(node for node in machine.nodes if node.id == "Any")
        live = _live_node(_guid(7), "SM:Transition", extras={
            "from": "A", "to": "B", "properties": {"CrossfadeDuration": 0.5, "Bidirectional": True},
            "property_defaults": {"CrossfadeDuration": 0.2, "Bidirectional": False}})
        self.assertEqual(ANIM_ADAPTER.omitted_property_resets(live, transition, set()), {"Bidirectional": False})
        live_alias = _live_node(_guid(8), "SM:StateAlias", extras={
            "name": "Any", "properties": {"AliasedStates": ["A"], "bGlobalAlias": True},
            "property_defaults": {"AliasedStates": [], "bGlobalAlias": False}})
        self.assertEqual(ANIM_ADAPTER.omitted_property_resets(live_alias, alias, set()),
                         {"AliasedStates": [], "bGlobalAlias": False})

    def test_diff_reports_state_machine_settings_the_document_leaves_out(self):
        authored = decode_anim_graph(self.machine())
        extracted = decode_anim_graph(self.machine({"Bidirectional": True}))
        self.assertEqual(runtime.omitted_settings(authored, extracted), ["T: Bidirectional"])
        self.assertEqual(runtime.omitted_settings(extracted, extracted), [])


class LayerInterfaceTests(unittest.TestCase):
    LAYERS = [{"Name": "FullBody", "Nodes": [
        {"Id": "Output", "Properties": {"LayerGroup": "Locomotion"}},
        {"Id": "Base", "Action": "Anim:LinkedInputPose", "Properties": {"Node.Name": "BasePose"}}], "Links": []}]

    def test_create_needs_no_skeleton(self):
        body = canonicalize(document({"ParentClass": INTERFACE, "AnimLayers": self.LAYERS}, "Create"))["Body"]
        self.assertEqual(body["ParentClass"], INTERFACE)
        self.assertEqual(body["AnimLayers"], self.LAYERS)

    def test_interface_body_holds_layers_only(self):
        cases = (
            ({"TargetSkeleton": SKELETON}, "/Body/TargetSkeleton"),
            ({"Template": True}, "/Body/Template"),
            ({"Variables": [{"Name": "W", "Type": "float"}]}, "/Body/Variables"),
            ({"EventDispatchers": [{"Name": "OnLanded"}]}, "/Body/EventDispatchers"),
            ({"UbergraphPages": [{"Name": "EventGraph", "Nodes": []}]}, "/Body/UbergraphPages"),
            ({"AnimGraph": {"Nodes": [{"Id": "P", "Action": "Anim:SequencePlayer"}]}}, "/Body/AnimGraph"),
            ({"AnimLayers": [{"Name": "L", "Nodes": [{"Id": "P", "Action": "Anim:SequencePlayer"}]}]},
             "/Body/AnimLayers/0/Nodes/0"),
            ({"AnimLayers": [{"Name": "L", "Nodes": [{"Id": "In", "Action": "Anim:LinkedInputPose"}],
                              "Links": [{"From": "In:Pose", "To": "Output:Result"}]}]}, "/Body/AnimLayers/0/Links"),
        )
        for extra, path in cases:
            with self.subTest(path=path):
                code, where, _ = code_of(lambda: canonicalize(document({"ParentClass": INTERFACE, **extra})))
                self.assertEqual((code, where), ("AnimBlueprint.InvalidLayerInterface", path))
        canonicalize(document({"ParentClass": INTERFACE, "Variables": [], "UbergraphPages": [],
                               "AnimGraph": {"Nodes": [], "Links": []}, "TargetSkeleton": None}))

    def test_blueprint_type_is_fixed(self):
        normal = {"settings": {"blueprint_type": "BPTYPE_Normal"}, "properties": {}}
        interface = {"settings": {"blueprint_type": "BPTYPE_Interface"}, "properties": {}}
        self.assertEqual(code_of(lambda: plan_header({"Properties": {}, "Body": {"ParentClass": INTERFACE}},
                                                     normal))[0], "AnimBlueprint.BlueprintTypeImmutable")
        self.assertEqual(code_of(lambda: plan_header(
            {"Target": TARGET, "Properties": {}, "Body": {"ParentClass": "/Script/Engine.AnimInstance"}},
            interface))[0], "AnimBlueprint.BlueprintTypeImmutable")
        # An interface edited without ParentClass is still held to the interface rules.
        self.assertEqual(code_of(lambda: plan_header(
            {"Target": TARGET, "Properties": {}, "Body": {"Variables": [{"Name": "W"}]}}, interface))[0],
            "AnimBlueprint.InvalidLayerInterface")
        self.assertEqual(code_of(lambda: plan_header({"Properties": {}, "Body": {}},
                                                     {"settings": {"blueprint_type": "BPTYPE_MacroLibrary"}}))[0],
                         "AnimBlueprint.UnsupportedBlueprintType")


class PlanTests(unittest.TestCase):
    LIVE = {"preview_mesh": "/Game/Characters/SKM_Hero.SKM_Hero", "preview_animation_blueprint": None,
            "preview_animation_blueprint_application_method": "LinkedLayers", "preview_animation_blueprint_tag": "",
            "sync_groups": ["Feet"], "parent_asset_overrides": [],
            "thumbnail": {"OrbitPitch": -15.0, "OrbitYaw": 45.0, "OrbitZoom": 10.0}, "is_template": False}

    def test_equal_settings_plan_nothing(self):
        body = {"Preview": {"Mesh": "/Game/Characters/SKM_Hero.SKM_Hero"}, "SyncGroups": ["Feet"],
                "ParentAssetOverrides": [], "Thumbnail": {"OrbitPitch": -15.0, "OrbitYaw": 45.0, "OrbitZoom": 10.0}}
        self.assertEqual(plan_settings(body, self.LIVE), [])
        self.assertEqual(plan_settings({}, self.LIVE), [])  # omitted regions are kept

    def test_changed_settings_plan_one_setting_each(self):
        body = {"Preview": {}, "SyncGroups": [], "Thumbnail": None,
                "ParentAssetOverrides": [{"ParentNodeGuid": "A" * 32, "Asset": "/Game/Anims/Run.Run"}]}
        self.assertEqual(plan_settings(body, self.LIVE), [
            (SETTING, {"name": "preview_mesh", "value": None}, "/Body/Preview/Mesh"),
            (SETTING, {"name": "sync_groups", "value": []}, "/Body/SyncGroups"),
            (SETTING, {"name": "parent_asset_overrides",
                       "value": [{"parent_node_guid": "A" * 32, "asset": "/Game/Anims/Run.Run"}]},
             "/Body/ParentAssetOverrides"),
        ])

    def test_header_changes_skeleton_and_keeps_template_fixed(self):
        query = {"settings": {"is_template": False}, "properties": {"TargetSkeleton": SKELETON},
                 "parent_class": "/Script/Engine.AnimInstance", "interfaces": [], "variables": []}
        other = "/Game/Characters/SK_Other.SK_Other"
        ops = plan_header({"Properties": {}, "Body": {"TargetSkeleton": other}}, query)
        self.assertIn(("blueprint.member.property.set.v1",
                       {"target": "blueprint", "property": "TargetSkeleton", "value": other}, "/Body/TargetSkeleton"),
                      ops)
        self.assertEqual(plan_header({"Properties": {}, "Body": {"TargetSkeleton": SKELETON}}, query), [])
        self.assertEqual(code_of(lambda: plan_header({"Properties": {}, "Body": {"Template": True}}, query))[0],
                         "AnimBlueprint.TemplateImmutable")


class RegistrationTests(unittest.TestCase):
    def test_lifecycle_declares_an_optional_factory_context(self):
        lifecycle = ANIM_BLUEPRINT_RUNTIME_REGISTRATION.target_lifecycle
        self.assertEqual(lifecycle.create_context_handle_names, ("parent_class", "factory_context"))
        self.assertEqual(lifecycle.optional_create_context_handle_names, ("factory_context",))
        self.assertTrue(_context_names_match(("parent_class", "factory_context"), lifecycle))
        self.assertTrue(_context_names_match(("parent_class",), lifecycle))  # template Animation Blueprint
        self.assertFalse(_context_names_match(("factory_context",), lifecycle))
        self.assertFalse(_context_names_match(("factory_context", "parent_class"), lifecycle))

    def test_profile_declares_the_setting_primitive(self):
        ids = {item.primitive_id for item in runtime.REQUIREMENTS}
        self.assertIn(SETTING, ids)
        self.assertIn("editor.asset.resolve_existing.v1", ids)


if __name__ == "__main__":
    unittest.main()
