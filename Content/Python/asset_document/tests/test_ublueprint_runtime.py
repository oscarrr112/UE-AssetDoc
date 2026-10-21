import unittest

from asset_document import bootstrap
from asset_document.diagnostics import AssetDocumentContractError
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profiles import ublueprint_members as members
from asset_document.profiles.ublueprint_graph_compare import section_mismatch_reasons, section_mismatches
from asset_document.profiles.ublueprint_runtime import (
    UBLUEPRINT_CLASS, UBlueprintRuntimeProfile, canonicalize, check_blueprint_type, diff, query_member_pages,
)
from asset_document.regions.primitive_catalog import NamedOpaqueHandleResult, PrimitiveOutcome, PrimitiveOutcomeStatus

TARGET = "/Game/Tests/BP_Door"
SELF = "/Game/Tests/BP_Door.BP_Door_C"


def document(body, properties=None):
    return {"SchemaVersion": 1, "Target": TARGET, "Class": UBLUEPRINT_CLASS, "Action": "CreateOrUpdate",
            "Properties": properties or {}, "Body": body}


def live_variable(name, category="real", sub="double", default=None, **flags):
    row = {"name": name, "guid": "0", "type": {"category": category, "sub_category": sub, "sub_category_object": "",
                                               "container": "none", "is_ref": False, "is_const": False},
           "category": "Default", "tooltip": "", "instance_editable": False, "blueprint_read_only": False,
           "expose_on_spawn": False, "private": False, "default": default}
    row.update(flags)
    return row


class CanonicalTests(unittest.TestCase):
    def test_legacy_shapes_are_converted(self):
        body = canonicalize(document({
            "ParentClass": {"Kind": "ClassRef", "Class": "/Script/Engine.Actor"},
            "ImplementedInterfaces": [{"Interface": {"Kind": "ClassRef", "Class": "/Game/BPI_Use"}}],
            "Variables": [{"Name": "Speed", "Type": {"PinCategory": "real", "PinSubCategory": "double"},
                           "DefaultValue": "0.5", "VarGuid": "x"},
                          {"Name": "Alive", "Type": {"PinCategory": "bool"}, "DefaultValue": "true"},
                          {"Name": "Target", "Type": {"PinCategory": "object",
                                                      "PinSubCategoryObject": "/Script/Engine.Actor"},
                           "DefaultValue": ""}],
            "Components": [{"Key": {"Name": "Mesh", "OwnerClass": SELF}, "Scope": "OwnedSCS",
                            "Class": "/Script/Engine.StaticMeshComponent",
                            "AttachTo": {"Name": "Root", "OwnerClass": SELF}, "Root": False, "Properties": {}}],
        }))["Body"]
        self.assertEqual(body["ParentClass"], "/Script/Engine.Actor")
        self.assertEqual(body["ImplementedInterfaces"], ["/Game/BPI_Use.BPI_Use_C"])
        self.assertEqual(body["Variables"][0], {"Name": "Speed", "Type": "float", "DefaultValue": 0.5})
        self.assertEqual(body["Variables"][1]["DefaultValue"], True)
        self.assertEqual(body["Variables"][2], {"Name": "Target", "Type": "object:/Script/Engine.Actor"})
        self.assertEqual(body["Components"], [{"Name": "Mesh", "Class": "/Script/Engine.StaticMeshComponent",
                                               "AttachTo": "Root", "Properties": {}}])

    def test_canonicalize_is_idempotent_and_keeps_graph_spelling(self):
        graph = {"Name": "EventGraph", "Nodes": [{"Id": "B", "Action": "K2:Branch"}], "Links": []}
        once = canonicalize(document({"ParentClass": "/Game/Base/BP_Base", "UbergraphPages": [graph]}))
        self.assertEqual(once["Body"]["ParentClass"], "/Game/Base/BP_Base.BP_Base_C")
        self.assertEqual(once["Body"]["UbergraphPages"], [graph])
        self.assertEqual(canonicalize(once), once)

    def test_unsupported_and_unknown_regions_fail_with_stable_codes(self):
        for body, code in (({"Timelines": [{"Name": "T", "Tracks": [{"Name": "A", "Type": "Curve"}]}]},
                            "UBlueprint.InvalidValue"),
                           ({"Graphs": []}, "UBlueprint.UnknownBodyKey"),
                           ({"Variables": [{"Name": "A"}]}, "UBlueprint.InvalidBodyRegion"),
                           ({"Variables": [{"Name": "A", "Type": "int"}, {"Name": "a", "Type": "int"}]},
                            "UBlueprint.DuplicateName")):
            with self.assertRaises(AssetDocumentContractError) as caught:
                canonicalize(document(body))
            self.assertEqual(caught.exception.diagnostic.code, code)
        self.assertEqual(canonicalize(document({"Timelines": []}))["Body"].get("Timelines"), [])

    def test_preflight_is_snapshot_covered(self):
        self.assertIs(UBlueprintRuntimeProfile().preflight(document({"Variables": []}), None, None), SNAPSHOT_COVERED)

    def test_registered_in_bootstrap(self):
        binding = bootstrap.PROFILE_REGISTRY.resolve_runtime_exact(UBLUEPRINT_CLASS)
        self.assertIsNotNone(binding)
        self.assertEqual(binding.target_lifecycle.create_context_handle_names, ("parent_class", "factory_class"))
        self.assertEqual(binding.target_lifecycle.optional_create_context_handle_names, ("factory_class",))


def failure_code(call):
    with unittest.TestCase().assertRaises(AssetDocumentContractError) as caught:
        call()
    return caught.exception.diagnostic.code


class _ClassResolveContext:
    """editor.class.resolve_canonical.v1 only: the class handle is the resolved path."""

    def __init__(self):
        self.calls = []

    def invoke(self, *, requirement, value_arguments, handle_arguments):
        self.calls.append(value_arguments["class"])
        return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED, {},
                                (NamedOpaqueHandleResult("class", (value_arguments["class"],)),), (), ())


class BlueprintTypeTests(unittest.TestCase):
    """Macro Library / Interface / Function Library are Body.BlueprintType (the creating factory)."""

    def create(self, body):
        return {**document(body), "Action": "Create"}

    def test_type_is_validated_and_kept(self):
        self.assertEqual(canonicalize(document({"BlueprintType": "MacroLibrary"}))["Body"]["BlueprintType"],
                         "MacroLibrary")
        self.assertNotIn("BlueprintType", canonicalize(document({}))["Body"])
        self.assertEqual(failure_code(lambda: canonicalize(document({"BlueprintType": "Const"}))),
                         "UBlueprint.InvalidBlueprintType")

    def test_interface_and_function_library_default_their_only_parent(self):
        self.assertEqual(canonicalize(self.create({"BlueprintType": "Interface"}))["Body"]["ParentClass"],
                         "/Script/CoreUObject.Interface")
        self.assertEqual(canonicalize(self.create({"BlueprintType": "FunctionLibrary"}))["Body"]["ParentClass"],
                         "/Script/Engine.BlueprintFunctionLibrary")
        # A Macro Library's parent is picked like a Blueprint Class's.
        self.assertEqual(failure_code(lambda: canonicalize(self.create({"BlueprintType": "MacroLibrary"}))),
                         "UBlueprint.ParentClassRequired")

    def test_regions_the_type_does_not_have_are_refused(self):
        graph = {"Name": "EventGraph", "Nodes": [], "Links": []}
        for kind, body in (("MacroLibrary", {"UbergraphPages": [graph]}),
                           ("MacroLibrary", {"FunctionGraphs": [{**graph, "Name": "F"}]}),
                           ("MacroLibrary", {"Variables": [{"Name": "A", "Type": "int"}]}),
                           ("Interface", {"MacroGraphs": [{**graph, "Name": "M"}]}),
                           ("Interface", {"Components": [{"Name": "C", "Class": "/Script/Engine.SceneComponent"}]}),
                           ("FunctionLibrary", {"ClassDefaults": {"bCanBeDamaged": False}}),
                           ("FunctionLibrary", {"ImplementedInterfaces": ["/Game/BPI_Use"]})):
            with self.subTest(kind=kind, body=body):
                self.assertEqual(failure_code(lambda: canonicalize(document({"BlueprintType": kind, **body}))),
                                 "UBlueprint.BlueprintTypeRegionUnsupported")
        # The extraction's empty regions are fine.
        canonicalize(document({"BlueprintType": "Interface", "Variables": [], "UbergraphPages": [], "MacroGraphs": [],
                               "FunctionGraphs": [{**graph, "Name": "Use"}], "EventDispatchers": [], "Timelines": []}))

    def test_create_context_passes_the_type_factory(self):
        context = _ClassResolveContext()
        handles = UBlueprintRuntimeProfile().create_target_context(self.create({"BlueprintType": "Interface"}), context)
        self.assertEqual([item.name for item in handles], ["parent_class", "factory_class"])
        self.assertEqual(context.calls, ["/Script/CoreUObject.Interface", "/Script/UnrealEd.BlueprintInterfaceFactory"])
        context = _ClassResolveContext()
        handles = UBlueprintRuntimeProfile().create_target_context(
            self.create({"ParentClass": "/Script/Engine.Actor"}), context)
        self.assertEqual([item.name for item in handles], ["parent_class"])

    def test_update_cannot_change_the_type(self):
        check_blueprint_type({"BlueprintType": "MacroLibrary"}, {"blueprint_type": "macro_library"})
        check_blueprint_type({}, {"blueprint_type": "interface"})
        check_blueprint_type({"BlueprintType": "Normal"}, {"blueprint_type": "const"})
        self.assertEqual(failure_code(lambda: check_blueprint_type({"BlueprintType": "Normal"},
                                                                   {"blueprint_type": "function_library"})),
                         "UBlueprint.BlueprintTypeImmutable")

    def test_diff_reports_a_different_type(self):
        authored = canonicalize(document({"BlueprintType": "Interface"}))
        self.assertEqual([item.path for item in diff(authored, canonicalize(document({})))], ["/Body/BlueprintType"])
        self.assertEqual(diff(authored, canonicalize(document({"BlueprintType": "Interface"}))), ())


class MemberPagingTests(unittest.TestCase):
    def test_pages_merge_into_one_result(self):
        pages = {
            0: {"parent_class": "/Script/Engine.Actor", "blueprint_type": "normal", "interfaces": [],
                "variables": [live_variable("A")], "dispatchers": [], "timelines": [], "components": [],
                "class_defaults": {"X": 1}, "properties": {}, "total": 5, "next_offset": 2},
            2: {"parent_class": "/Script/Engine.Actor", "blueprint_type": "normal", "interfaces": [],
                "variables": [live_variable("B")], "dispatchers": [], "timelines": [], "components": [],
                "class_defaults": {"Y": 2}, "properties": {"P": True}, "total": 5, "next_offset": None},
        }
        seen = []

        def page(offset):
            seen.append(offset)
            return pages[offset]
        merged = query_member_pages(page)
        self.assertEqual(seen, [0, 2])
        self.assertEqual([row["name"] for row in merged["variables"]], ["A", "B"])
        self.assertEqual(merged["class_defaults"], {"X": 1, "Y": 2})
        self.assertEqual(merged["properties"], {"P": True})
        self.assertNotIn("next_offset", merged)

    def test_paging_that_does_not_advance_fails(self):
        self.assertEqual(failure_code(lambda: query_member_pages(lambda offset: {"next_offset": 0})),
                         "UBlueprint.InvalidPrimitiveResult")


class PlanTests(unittest.TestCase):
    def test_variables_remove_add_and_skip_unchanged(self):
        body = members.canonical_members({"Variables": [
            {"Name": "Health", "Type": "float", "DefaultValue": 100.0},
            {"Name": "Ammo", "Type": "int", "DefaultValue": 3, "InstanceEditable": True}]})
        query = {"variables": [live_variable("Health", default=100.0), live_variable("Old", "int", "", 0)]}
        ops = members.plan_variables(body, query)
        self.assertEqual([(op[0], op[1]["name"]) for op in ops],
                         [("blueprint.member.variable.remove.v1", "Old"), ("blueprint.member.variable.set.v1", "Ammo")])
        self.assertEqual(ops[1][1]["instance_editable"], True)
        self.assertEqual(ops[1][1]["default"], 3)
        query["variables"][0]["default"] = 50.0
        ops = members.plan_variables(body, {"variables": query["variables"][:1]})
        self.assertEqual(ops[0][1], {"name": "Health", "type": members.type_dto("float", "/"), "default": 100.0})

    def test_omitted_variable_metadata_resets_to_default(self):
        body = members.canonical_members({"Variables": [{"Name": "Health", "Type": "float", "DefaultValue": 100.0}]})
        query = {"variables": [live_variable("Health", default=100.0, category="Stats", instance_editable=True)]}
        query["variables"][0]["category"], query["variables"][0]["tooltip"] = "Stats", "Hit points"
        ops = members.plan_variables(body, query)
        self.assertEqual(len(ops), 1)
        self.assertEqual((ops[0][1]["category"], ops[0][1]["tooltip"], ops[0][1]["instance_editable"]),
                         ("Default", "", False))
        extracted = members.members_from_query(query)
        self.assertEqual([item.path for item in members.diff_members(body, extracted, {}, {})], ["/Body/Variables"])

    def test_components_are_added_parents_first_and_class_change_recreates(self):
        body = members.canonical_members({"Components": [
            {"Name": "Light", "Class": "/Script/Engine.PointLightComponent", "AttachTo": "Mesh"},
            {"Name": "Mesh", "Class": "/Script/Engine.StaticMeshComponent", "AttachTo": "Root"},
            {"Name": "Root", "Class": "/Script/Engine.SceneComponent"}]})
        query = {"components": [{"name": "Root", "scope": "owned", "class": "/Script/Engine.BoxComponent",
                                 "attach_to": "", "properties": {}}]}
        ops = members.plan_component_structure(body, query)
        self.assertEqual([(op[0].rsplit(".", 2)[-2], op[1]["name"]) for op in ops],
                         [("remove", "Root"), ("add", "Root"), ("add", "Mesh"), ("add", "Light")])

    def test_component_cycle_is_rejected(self):
        body = members.canonical_members({"Components": [
            {"Name": "A", "Class": "/Script/Engine.SceneComponent", "AttachTo": "B"},
            {"Name": "B", "Class": "/Script/Engine.SceneComponent", "AttachTo": "A"}]})
        with self.assertRaises(AssetDocumentContractError):
            members.plan_component_structure(body, {"components": []})

    def test_values_reset_unlisted_overrides(self):
        body = members.canonical_members({
            "ClassDefaults": {"InitialLifeSpan": 5.0},
            "Components": [{"Name": "CharacterMovement", "Scope": "Native", "Properties": {"MaxWalkSpeed": 300}}]})
        query = {"class_defaults": {"InitialLifeSpan": 5.0, "bCanBeDamaged": False},
                 "components": [{"name": "CharacterMovement", "subobject": "CharMoveComp", "scope": "native",
                                 "properties": {"MaxWalkSpeed": 600.0, "JumpZVelocity": 500.0}},
                                {"name": "Base", "scope": "inherited", "properties": {"bVisible": False}}]}
        ops = [(op[1].get("property", op[1].get("name")), op[1].get("value", "reset" if op[1].get("reset") else op[0]))
               for op in members.plan_values(body, query)]
        self.assertEqual(ops, [("MaxWalkSpeed", 300), ("JumpZVelocity", "reset"),
                               ("Base", "blueprint.member.component.remove.v1"), ("bCanBeDamaged", "reset")])


class DiffTests(unittest.TestCase):
    def test_members_round_trip_and_mismatch(self):
        query = {"parent_class": "/Script/Engine.Actor", "interfaces": [],
                 "variables": [live_variable("Health", default=100.0, instance_editable=True)],
                 "components": [{"name": "Mesh", "scope": "owned", "class": "/Script/Engine.StaticMeshComponent",
                                 "attach_to": "DefaultSceneRoot", "properties": {}}],
                 "class_defaults": {"InitialLifeSpan": 5.0}}
        extracted = members.members_from_query(query)
        authored = members.canonical_members({
            "ParentClass": "/Script/Engine.Actor",
            "Variables": [{"Name": "Health", "Type": "float", "DefaultValue": "100", "InstanceEditable": True}],
            "Components": [{"Name": "Mesh", "Class": "/Script/Engine.StaticMeshComponent"}],
            "ClassDefaults": {"InitialLifeSpan": 5}})
        self.assertEqual(members.diff_members(authored, extracted, {}, {}), [])
        authored["Variables"][0]["DefaultValue"] = 50
        self.assertEqual([item.path for item in members.diff_members(authored, extracted, {}, {})], ["/Body/Variables"])

    def test_a_variable_whose_class_is_gone_is_an_object_variable(self):
        # The editor adds such a variable as Object (FBlueprintEditorUtils::PostSetupObjectPinType):
        # AITesting_MoveGoal's ObservedPawn reads back object:/Script/CoreUObject.Object.
        query = {"variables": [live_variable("Pawn", category="object", sub="")]}
        query["variables"][0]["type"]["sub_category_object"] = "/Script/CoreUObject.Object"
        extracted = members.members_from_query(query)
        authored = members.canonical_members({"Variables": [{"Name": "Pawn", "Type": {"Category": "object"}}]})
        self.assertEqual(authored["Variables"][0]["Type"], "object:/Script/CoreUObject.Object")
        self.assertEqual(members.diff_members(authored, extracted, {}, {}), [])
        # The source's class-less variable extracts as the Object variable the editor makes of it.
        query["variables"][0]["type"]["sub_category_object"] = ""
        self.assertEqual(members.members_from_query(query)["Variables"][0]["Type"], "object:/Script/CoreUObject.Object")
        self.assertEqual(members.plan_variables(authored, query), [])
        authored = members.canonical_members({"Variables": [{"Name": "Pawn", "Type": "object:/Script/Engine.Pawn"}]})
        self.assertEqual([item.path for item in members.diff_members(authored, extracted, {}, {})], ["/Body/Variables"])

    def test_a_map_in_another_order_is_the_same_map(self):
        # Compiling a Blueprint reorders a map whose struct default has entries (BP_Dynamic_Force_SkelMesh).
        authored = '(("middle_01_rSocket", "spine_03Socket"),("foot_rSocket", "calf_rSocket"))'
        compiled = '(("foot_rSocket", "calf_rSocket"),("middle_01_rSocket", "spine_03Socket"))'
        self.assertTrue(members.values_equal({"Setup": {"Sockets": authored}}, {"Setup": {"Sockets": compiled}}))
        self.assertFalse(members.values_equal(authored, '(("foot_rSocket", "calf_rSocket"))'))
        self.assertFalse(members.values_equal(authored, '(("foot_rSocket", "x"),("middle_01_rSocket", "spine_03Socket"))'))
        # Struct and array text keeps its order.
        self.assertIsNone(members.map_text_entries("((X=1.0,Y=2.0),(X=3.0,Y=4.0))"))
        self.assertFalse(members.values_equal("((X=1.0,Y=2.0),(X=3.0,Y=4.0))", "((X=3.0,Y=4.0),(X=1.0,Y=2.0))"))
        self.assertIsNone(members.map_text_entries('("a, b")'))

    def test_a_struct_named_by_text_compares_by_its_name(self):
        # An FKey written "Spacbar" against the read-back {"KeyName": "SpaceBar"} is a change (it was taken as
        # equal, so Apply skipped the write and its check); case alone is not (FName).
        self.assertFalse(members.values_equal("Spacbar", {"KeyName": "SpaceBar"}))
        self.assertTrue(members.values_equal("spacebar", {"KeyName": "SpaceBar"}))
        self.assertTrue(members.values_equal('(KeyName="SpaceBar")', {"KeyName": "SpaceBar"}))
        self.assertTrue(members.values_equal("(X=1,Y=2)", {"X": 1.0, "Y": 2.0}))

    def test_strict_diff_reports_authored_values_the_extraction_omits(self):
        query = {"parent_class": "/Script/Engine.Actor", "interfaces": [],
                 "variables": [live_variable("Health")],
                 "components": [{"name": "Mesh", "scope": "owned", "class": "/Script/Engine.StaticMeshComponent",
                                 "attach_to": "DefaultSceneRoot", "properties": {}}],
                 "class_defaults": {}}
        extracted = members.members_from_query(query)
        authored = members.canonical_members({
            "Variables": [{"Name": "Health", "Type": "float", "DefaultValue": 7}],
            "Components": [{"Name": "Mesh", "Class": "/Script/Engine.StaticMeshComponent",
                            "Properties": {"bVisible": False}}],
            "ClassDefaults": {"InitialLifeSpan": 7.5}})
        self.assertEqual(members.diff_members(authored, extracted, {"Note": "x"}, {}), [])
        self.assertEqual([item.path for item in members.diff_members(authored, extracted, {"Note": "x"}, {}, True)],
                         ["/Properties/Note", "/Body/Variables", "/Body/Components/0/Properties/bVisible",
                          "/Body/ClassDefaults"])
        # An extraction re-applied as is stays an empty strict Diff.
        same = members.canonical_members({"Variables": [{"Name": "Health", "Type": "float"}],
                                          "Components": [{"Name": "Mesh", "Class": "/Script/Engine.StaticMeshComponent"}],
                                          "ClassDefaults": {}})
        self.assertEqual(members.diff_members(same, extracted, {}, {}, True), [])


class GraphCompareTests(unittest.TestCase):
    AUTHORED = [{"Name": "Compute", "Signature": {"Inputs": [{"Name": "X", "Type": "float"}]},
                 "Nodes": [{"Id": "If", "Action": "K2:Branch"},
                           {"Id": "Say", "Action": "K2:PrintString",
                            "PinOverrides": [{"Pin": "InString", "DefaultValue": "hi"}]}],
                 "Links": [{"From": "Entry:then", "To": "If:execute"}, {"From": "If:then", "To": "Say:execute"}]}]

    def extracted(self, **changes):
        graph = {"Name": "Compute", "Signature": {"Inputs": [{"Name": "X", "Type": "float"}]},
                 "Nodes": [{"Id": "Entry", "Implicit": "entry", "NodeGuid": "C" * 32},
                           {"Id": "Branch", "Action": "K2:Branch", "Position": {"X": 0, "Y": 0}},
                           {"Id": "PrintString", "Action": "K2:PrintString",
                            "PinOverrides": [{"Pin": "InString", "DefaultValue": changes.get("text", "hi")}]}],
                 "Links": [{"From": "Entry:then", "To": "Branch:execute"},
                           {"From": f"Branch:{changes.get('pin', 'then')}", "To": "PrintString:execute"}]}
        stub = {"Name": "UserConstructionScript", "Nodes": [{"Id": "Entry", "Implicit": "entry", "NodeGuid": "D" * 32}],
                "Links": []}
        return [graph, stub]

    def test_renamed_ids_match_and_stub_graphs_are_tolerated(self):
        self.assertEqual(section_mismatches("FunctionGraphs", self.AUTHORED, self.extracted(), self_class=SELF), [])

    def test_child_blueprint_parent_call_stub_is_tolerated(self):
        def ucs(called):
            return [{"Name": "UserConstructionScript",
                     "Nodes": [{"Id": "Entry"},
                               {"Id": "Parent", "Action": f"K2:CallParentFunction:/Game/B/BP_Base.BP_Base_C:{called}"}],
                     "Links": [{"From": "Entry:then", "To": "Parent:execute"}]}]
        self.assertEqual(section_mismatches("FunctionGraphs", [], ucs("UserConstructionScript"), self_class=SELF), [])
        self.assertEqual(section_mismatches("FunctionGraphs", [], ucs("Other"), self_class=SELF),
                         ["UserConstructionScript"])

    def test_link_override_and_signature_changes_are_reported(self):
        self.assertEqual(section_mismatches("FunctionGraphs", self.AUTHORED, self.extracted(pin="else"),
                                            self_class=SELF), ["Compute"])
        self.assertEqual(section_mismatches("FunctionGraphs", self.AUTHORED, self.extracted(text="bye"),
                                            self_class=SELF), ["Compute"])
        authored = [dict(self.AUTHORED[0], Signature={"Inputs": [{"Name": "X", "Type": "int"}]})]
        self.assertEqual(section_mismatches("FunctionGraphs", authored, self.extracted(), self_class=SELF), ["Compute"])

    def test_mismatches_say_what_differs(self):
        def why(extracted, authored=None):
            return section_mismatch_reasons("FunctionGraphs", authored or self.AUTHORED, extracted, self_class=SELF)
        self.assertEqual(why(self.extracted()), {})
        self.assertEqual(why(self.extracted(pin="else"))["Compute"],
                         "links: missing [K2:Branch.then -> K2:PrintString.execute], "
                         "unexpected [K2:Branch.else -> K2:PrintString.execute]")
        self.assertRegex(why(self.extracted(text="bye"))["Compute"],
                         r"^pin values of Say \(K2:PrintString\): \[.*'hi'.*\] not found; read back \[.*'bye'.*\]$")
        authored = [dict(self.AUTHORED[0], Signature={"Inputs": [{"Name": "X", "Type": "int"}]})]
        self.assertEqual(why(self.extracted(), authored)["Compute"],
                         "signature differs: inputs [('x', 'int')], read back [('x', 'float')]")
        self.assertEqual(why(self.extracted()[1:]), {"Compute": "missing"})
        extra = self.extracted() + [{"Name": "Other", "Nodes": [{"Id": "Say", "Action": "K2:PrintString"}], "Links": []}]
        self.assertEqual(why(extra), {"Other": "unexpected"})

    def test_strict_diff_compares_stated_layout_and_metadata(self):
        def moved(x=0, comment=None, guid=None):
            graph = self.extracted()[0]
            graph["Nodes"][1] = dict(graph["Nodes"][1], Position={"X": x, "Y": 0})
            if comment is not None:
                graph["Nodes"][1]["Comment"] = comment
            return [graph]
        authored = [dict(self.AUTHORED[0], Nodes=[dict(self.AUTHORED[0]["Nodes"][0], Position={"X": 0, "Y": 0},
                                                       Comment="gate"),
                                                  self.AUTHORED[0]["Nodes"][1]])]
        live = moved(64, "gate")
        self.assertEqual(section_mismatches("FunctionGraphs", authored, live, self_class=SELF), [])
        self.assertEqual(section_mismatches("FunctionGraphs", authored, live, self_class=SELF, strict=True), ["Compute"])
        self.assertEqual(section_mismatches("FunctionGraphs", authored, moved(0), self_class=SELF, strict=True),
                         ["Compute"])
        self.assertEqual(section_mismatches("FunctionGraphs", authored, moved(0, "gate"), self_class=SELF, strict=True),
                         [])
        # An implicit node's stated metadata counts even when it is not linked.
        entry = [dict(authored[0], Nodes=authored[0]["Nodes"] + [{"Id": "Entry", "Implicit": "entry",
                                                                   "NodeGuid": "E" * 32}])]
        self.assertEqual(section_mismatches("FunctionGraphs", entry, moved(0, "gate"), self_class=SELF, strict=True),
                         ["Compute"])
        # The whole extraction compared strictly with itself is empty.
        self.assertEqual(section_mismatches("FunctionGraphs", self.extracted(), self.extracted(), self_class=SELF,
                                            strict=True), [])


if __name__ == "__main__":
    unittest.main()


class DifferenceTextTests(unittest.TestCase):
    def test_a_difference_names_the_innermost_value_that_differs(self):
        # BP_Dynamic_Force_SkelMesh: a struct default deep in Variables was cut off before the part that differed.
        from types import SimpleNamespace
        from asset_document.registered_runtime import _describe_differences
        before = [{"Name": "A"}, {"Name": "B", "DefaultValue": {"X": 1, "Map": "((a, b))"}}]
        after = [{"Name": "A"}, {"Name": "B", "DefaultValue": {"X": 1, "Map": "((a,b))"}}]
        item = SimpleNamespace(path="/Body/Variables", before=before, after=after, before_present=True, after_present=True)
        self.assertEqual(_describe_differences([item]),
                         '/Body/Variables/1/DefaultValue/Map: document "((a, b))", read back "((a,b))"')


class GraphRenameDiffTests(unittest.TestCase):
    """A function renamed in the document, keeping its GraphGuid, is a rename."""
    GUID = "A" * 32

    def graph(self, name, guid=GUID, text="hi"):
        return {"Name": name, "GraphGuid": guid,
                "Nodes": [{"Id": "Say", "Action": "K2:PrintString",
                           "PinOverrides": [{"Pin": "InString", "DefaultValue": text}]}],
                "Links": [{"From": "Entry:then", "To": "Say:execute"}]}

    def test_a_renamed_graph_is_reported_as_the_rename(self):
        why = section_mismatch_reasons("FunctionGraphs", [self.graph("SwingDoor")], [self.graph("OpenDoor")],
                                       self_class=SELF)
        self.assertEqual(why, {"SwingDoor": "renamed from OpenDoor"})
        changed = section_mismatch_reasons("FunctionGraphs", [self.graph("SwingDoor", text="bye")],
                                           [self.graph("OpenDoor")], self_class=SELF)
        self.assertRegex(changed["SwingDoor"], r"^renamed from OpenDoor; pin values of Say")

    def test_without_the_identity_it_is_a_new_graph(self):
        why = section_mismatch_reasons("FunctionGraphs", [self.graph("SwingDoor", guid=None)],
                                       [self.graph("OpenDoor")], self_class=SELF)
        self.assertEqual(why, {"SwingDoor": "missing", "OpenDoor": "unexpected"})


class InstancedDefaultsTests(unittest.TestCase):
    """Values of an instanced object compare sparse over its class defaults."""
    PATTERN = "/Script/TemplateSequence.SequenceCameraShakePattern"
    DEFAULTS = {PATTERN: {"Scale": 1.0, "PlayRate": 1.0}}

    def test_a_listed_default_equals_its_omission(self):
        authored = {"Class": self.PATTERN, "Properties": {"Scale": 1.0, "PlayRate": 2.0}}
        observed = {"Class": self.PATTERN, "Properties": {"PlayRate": 2.0}}
        self.assertFalse(members.values_equal(authored, observed))
        self.assertTrue(members.values_equal(authored, observed, self.DEFAULTS))
        self.assertFalse(members.values_equal({"Class": self.PATTERN, "Properties": {"Scale": 2.0}}, observed,
                                              self.DEFAULTS))
        self.assertEqual(members.instance_classes({"RootShakePattern": authored}), {self.PATTERN})

    def test_class_defaults_plan_and_diff_use_the_defaults(self):
        body = members.canonical_members({"ClassDefaults": {"RootShakePattern": {
            "Class": self.PATTERN, "Properties": {"Scale": 1.0, "PlayRate": 2.0}}}})
        query = {"parent_class": "/Script/Engine.CameraShakeBase", "interfaces": [], "variables": [],
                 "components": [], "class_defaults": {"RootShakePattern": {
                     "Class": self.PATTERN, "Properties": {"PlayRate": 2.0}}}}
        self.assertEqual(members.plan_class_defaults(body, query, self.DEFAULTS), [])
        live = members.members_from_query(query)
        self.assertEqual(members.diff_members(body, live, {}, {}, defaults=self.DEFAULTS), [])
        self.assertNotEqual(members.diff_members(body, live, {}, {}), [])


class ConversionLinkTests(unittest.TestCase):
    """Links the editor makes only through a conversion node, in document terms."""
    FACTS = {"from_type": "Integer", "to_type": "Boolean", "reason": "Convert Integer to Boolean",
             "conversion_action": "K2:CallFunction:/Script/Engine.KismetMathLibrary:Conv_IntToBool",
             "conversion_input": "InInt", "conversion_output": "ReturnValue"}

    def test_the_message_names_the_node_and_the_links_through_it(self):
        from asset_document.graphs.executor import conversion_message
        text = conversion_message("Rand:ReturnValue", "Branch:Condition", self.FACTS)
        self.assertIn('{"Id": "Conv_IntToBool", "Action": '
                      '"K2:CallFunction:/Script/Engine.KismetMathLibrary:Conv_IntToBool"}', text)
        self.assertIn('{"From": "Rand:ReturnValue", "To": "Conv_IntToBool:InInt"} and '
                      '{"From": "Conv_IntToBool:ReturnValue", "To": "Branch:Condition"}', text)
        self.assertNotIn("allow_conversion_node", text)
        self.assertIn("link a pin of type Boolean", conversion_message("A:B", "C:D", {"to_type": "Boolean"}))

    def body(self, **rand):
        return {"FunctionGraphs": [{"Name": "Check", "Nodes": [
            {"Id": "Branch", "Action": "K2:Branch"},
            dict({"Id": "Rand", "Action": "K2:CallFunction:/Script/Engine.KismetMathLibrary:RandomInteger"}, **rand),
            {"Id": "Health", "Action": "K2:Get:Health"}],
            "Links": [{"From": "Entry:then", "To": "Branch:execute"},
                      {"From": "Rand:ReturnValue", "To": "Branch:Condition"},
                      {"From": "Health:Health", "To": "Branch:Condition"}]}]}

    def test_only_links_whose_pin_types_the_document_cannot_change_are_checked(self):
        from asset_document.graphs.schemas.k2 import decode_body
        from asset_document.profiles.ublueprint_link_check import check_links, checked_links
        graphs, _ = decode_body(self.body(), self_class=SELF)
        links = checked_links(graphs, self_class=SELF, target=TARGET)
        # Entry is implicit and Health is a member of the Blueprint itself (its type may change in the document).
        self.assertEqual([(path, request["from"]["action_key"], request["to"]["action_key"])
                          for path, _, _, request in links],
                         [("/Body/FunctionGraphs/0/Links/1",
                           "K2:CallFunction:/Script/Engine.KismetMathLibrary:RandomInteger", "K2:IfThenElse")])
        shaped, _ = decode_body(self.body(PinOverrides=[{"Pin": "Max", "DefaultObject": "/Game/X"}]), self_class=SELF)
        self.assertEqual(checked_links(shaped, self_class=SELF, target=TARGET), [])
        calls = []

        def invoke(primitive, values):
            calls.append((primitive, values))
            return {"conversions": [dict(self.FACTS, index=0, from_pin="ReturnValue", to_pin="Condition")],
                    "checked": 1}
        with self.assertRaises(AssetDocumentContractError) as raised:
            check_links(invoke, graphs, self_class=SELF, target=TARGET)
        self.assertEqual(calls[0][0], "graph.link.check.v1")
        self.assertEqual(raised.exception.diagnostic.code, "Graph.ConnectNeedsConversion")
        self.assertEqual(raised.exception.diagnostic.path, "/Body/FunctionGraphs/0/Links/1")
        self.assertIn('"To": "Conv_IntToBool:InInt"', raised.exception.diagnostic.message)
        check_links(lambda primitive, values: {"conversions": [], "checked": 1}, graphs, self_class=SELF, target=TARGET)
        check_links(lambda primitive, values: None, graphs, self_class=SELF, target=TARGET)
