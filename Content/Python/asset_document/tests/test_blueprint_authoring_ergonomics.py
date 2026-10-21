"""Blueprint authoring ergonomics: pin spellings, short keys, the graph check and its diagnostics."""
import unittest

from asset_document.diagnostics import AssetDocumentContractError, RuntimeDiagnostic, contract_errors
from asset_document.graphs.actions import ALIASES, normalize_action as normalize_action_key
from asset_document.graphs.model import GraphDoc, NodeDoc, PinOverride
from asset_document.graphs.pin_names import node_pin_key, pin_key, resolve_pin_name, strip_member_guids
from asset_document.graphs.schemas.k2 import decode_body, localize_action
from asset_document.profiles import ublueprint_members as members
from asset_document.profiles.ublueprint_graph_compare import display_enum_values
from asset_document.profiles.ublueprint_link_check import check_graphs, check_links

TARGET = "/Game/Tests/BP_Door"
SELF = "/Game/Tests/BP_Door.BP_Door_C"
GUID = "D0706D44" + "0" * 24


class _Pin:
    def __init__(self, name, direction="input", display_name="", category="real", hidden=False):
        self.name, self.direction, self.display_name, self.category, self.hidden = name, direction, display_name, category, hidden


class PinNameTests(unittest.TestCase):
    def test_a_user_struct_member_is_named_by_its_friendly_name(self):
        self.assertEqual(strip_member_guids(f"LaunchVelocity_16_{GUID}"), "LaunchVelocity")
        self.assertEqual(pin_key(f"Hit_LaunchVelocity_16_{GUID}_X"), "hitlaunchvelocityx")
        pins = [_Pin(f"LaunchVelocity_16_{GUID}", "output"), _Pin(f"Damage_2_{GUID}", "output")]
        self.assertIs(resolve_pin_name("LaunchVelocity", pins), pins[0])
        self.assertIs(resolve_pin_name("damage", pins), pins[1])
        self.assertIsNone(resolve_pin_name("Kind", pins))

    def test_a_cast_output_answers_to_result_and_as_class_with_or_without_spaces(self):
        cast = "K2:DynamicCast:/Game/BP_SampleCharacterBase.BP_SampleCharacterBase_C"
        pins = [_Pin("Object"), _Pin("then", "output"), _Pin("AsBP Sample Character Base", "output", category="object"),
                _Pin("bSuccess", "output", category="bool")]
        for spelling in ("Result", "AsBP_SampleCharacterBase", "AsBP Sample Character Base", "AsBPSampleCharacterBase"):
            self.assertIs(resolve_pin_name(spelling, pins, action=cast), pins[2], spelling)
        self.assertEqual(node_pin_key("Result", action=cast), node_pin_key("AsBP Sample Character Base", action=cast))
        self.assertNotEqual(node_pin_key("Result", action="K2:CallFunction:/Script/Engine.Actor:K2_GetActorLocation"),
                            "#castresult")

    def test_get_data_table_row_pins_answer_to_their_display_names(self):
        pins = [_Pin("execute"), _Pin("then", "output", "Row Found"), _Pin("RowNotFound", "output", "Row Not Found"),
                _Pin("ReturnValue", "output", "Out Row", category="wildcard")]
        self.assertIs(resolve_pin_name("RowFound", pins, action="K2:GetDataTableRow"), pins[1])
        self.assertIs(resolve_pin_name("OutRow", pins, action="K2:GetDataTableRow"), pins[3])
        self.assertIs(resolve_pin_name("Row Not Found", pins, action="K2:GetDataTableRow"), pins[2])


class ShortKeyTests(unittest.TestCase):
    def test_operator_aliases_mirror_the_palette_keys(self):
        for alias, key in (("K2:EqualEqual", "K2:PromotableOperator:EqualEqual"), ("K2:EqualEnum", "K2:EnumEquality"),
                           ("K2:NotEqualEnum", "K2:EnumInequality"),
                           ("K2:SelectFloat", "K2:CallFunction:/Script/Engine.KismetMathLibrary:SelectFloat"),
                           ("K2:And", "K2:CallFunction:/Script/Engine.KismetMathLibrary:BooleanAND"),
                           ("K2:Min", "K2:CallFunction:/Script/Engine.KismetMathLibrary:FMin"),
                           ("K2:MinInt", "K2:CallFunction:/Script/Engine.KismetMathLibrary:Min")):
            self.assertEqual(normalize_action_key(alias), key, alias)
        self.assertIn("K2:SelectClass", ALIASES)

    def test_a_short_variable_key_in_a_function_names_its_local_first(self):
        local = localize_action("K2:VariableGet:" + SELF + ":Tmp", ("Tmp",), graph="Probe", self_class=SELF)
        self.assertEqual(local, "K2:VariableGet:" + SELF + ":Probe:Tmp")
        self.assertEqual(localize_action("K2:VariableSet:" + SELF + ":Health", ("Tmp",), graph="Probe", self_class=SELF),
                         "K2:VariableSet:" + SELF + ":Health")
        graphs, _ = decode_body({"FunctionGraphs": [{"Name": "Probe", "LocalVariables": [{"Name": "Tmp", "Type": "int"}],
                                                     "Nodes": [{"Id": "Get", "Action": "K2:Get:Tmp"}]}]}, self_class=SELF)
        self.assertEqual(graphs[0].nodes[0].action, "K2:VariableGet:" + SELF + ":Probe:Tmp")


class GraphCheckTests(unittest.TestCase):
    BODY = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
        {"Id": "Begin", "Action": "K2:BeginPlay"}, {"Id": "B", "Action": "K2:Branch"},
        {"Id": "Print", "Action": "K2:CallFunction:/Script/Engine.KismetSystemLibrary:PrintStrin"},
        {"Id": "Health", "Action": "K2:Get:Health"},
        {"Id": "Cast", "Action": "K2:DynamicCast:/Script/Engine.Character"}],
        "Links": [{"From": "Begin:then", "To": "B:Bogus"}, {"From": "Health:Health", "To": "B:Condition"},
                  {"From": "Cast:Result", "To": "B:Condition"}]}]}

    def invoke(self, calls):
        branch = [{"name": "execute", "direction": "input", "category": "exec"},
                  {"name": "Condition", "direction": "input", "category": "bool"},
                  {"name": "then", "direction": "output", "category": "exec", "display_name": "True"},
                  {"name": "else", "direction": "output", "category": "exec", "display_name": "False"}]
        begin = [{"name": "then", "direction": "output", "category": "exec"}]
        cast = [{"name": "Object", "direction": "input", "category": "object"},
                {"name": "AsCharacter", "direction": "output", "category": "object", "display_name": "As Character"}]

        def call(primitive, values):
            calls.append(values)
            if "describe" in values:
                return {"unresolved": [{"key": "K2:CallFunction:/Script/Engine.KismetSystemLibrary:PrintStrin",
                                        "code": "Graph.ActionNotFound", "message": "No K2 action has that key.",
                                        "suggestions": ["K2:PrintString"]}],
                        "templates": [{"key": "K2:Event:/Script/Engine.Actor:ReceiveBeginPlay", "can_add_pin": False, "pins": begin},
                                      {"key": "K2:IfThenElse", "can_add_pin": False, "pins": branch},
                                      {"key": "K2:DynamicCast:/Script/Engine.Character", "can_add_pin": False, "pins": cast}]}
            return {"conversions": [], "checked": len(values["links"])}
        return call

    def test_every_independent_problem_is_reported_in_one_pass(self):
        graphs, _ = decode_body(self.BODY, self_class=SELF)
        calls = []
        found = check_graphs(self.invoke(calls), graphs, self_class=SELF, target=TARGET)
        by_code = {item.code: item for item in found}
        self.assertEqual(set(by_code), {"Graph.ActionNotFound", "Graph.PinNotFound"})
        self.assertEqual(by_code["Graph.ActionNotFound"].path, "/Body/UbergraphPages/0/Nodes/2/Action")
        self.assertIn("Did you mean: K2:PrintString", by_code["Graph.ActionNotFound"].message)
        self.assertEqual(by_code["Graph.PinNotFound"].path, "/Body/UbergraphPages/0/Links/0")
        self.assertIn("'Bogus'", by_code["Graph.PinNotFound"].message)
        self.assertIn("Pins: execute, Condition.", by_code["Graph.PinNotFound"].message)
        # The Blueprint's own member (Health) is not described: the same document may make it.
        self.assertFalse(any(SELF in key for key in calls[0]["describe"]))
        # The links go out with the template's pin names (Cast:Result -> AsCharacter).
        sent = [link for values in calls[1:] for link in values["links"]]
        self.assertIn({"from": {"action_key": "K2:DynamicCast:/Script/Engine.Character", "pin_name": "AsCharacter"},
                       "to": {"action_key": "K2:IfThenElse", "pin_name": "Condition"}}, sent)

    def test_check_links_raises_them_all(self):
        graphs, _ = decode_body(self.BODY, self_class=SELF)
        with self.assertRaises(AssetDocumentContractError) as raised:
            check_links(self.invoke([]), graphs, self_class=SELF, target=TARGET)
        self.assertEqual(len(raised.exception.diagnostics), 2)
        self.assertEqual(raised.exception.diagnostic.code, "Graph.ActionNotFound")
        check_links(lambda primitive, values: None, graphs, self_class=SELF, target=TARGET)

    def test_skipped_keys_are_left_to_apply(self):
        """A Widget Blueprint's bound event (its widget may be one the same document adds) is neither described nor
        linked by Validate's check."""
        from asset_document.profiles.widget_blueprint_runtime import _bound_event
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Click", "Action": "K2:ComponentBoundEvent:/Script/UMG.Button:OnClicked@OkButton"},
            {"Id": "B", "Action": "K2:Branch"}],
            "Links": [{"From": "Click:then", "To": "B:execute"}]}]}
        graphs, _ = decode_body(body, self_class=SELF)
        calls = []
        self.assertEqual(check_graphs(self.invoke(calls), graphs, self_class=SELF, target=TARGET, skip=_bound_event), [])
        self.assertTrue(calls)
        self.assertFalse(any("ComponentBoundEvent" in key for key in calls[0]["describe"]))
        self.assertFalse(any(values.get("links") for values in calls[1:]))
        calls.clear()
        check_graphs(self.invoke(calls), graphs, self_class=SELF, target=TARGET)
        self.assertTrue(any("ComponentBoundEvent" in key for key in calls[0]["describe"]))

    def test_actor_blueprint_leaves_bound_events_of_its_listed_components_to_apply(self):
        """An Actor Blueprint's bound event of a component the document lists as its own (it may add it) is left
        to Apply; inherited / native components and other keys are still checked."""
        from asset_document.profiles.ublueprint_runtime import listed_component_events
        skip = listed_component_events({"Components": [{"Name": "Trigger", "Class": "/Script/Engine.BoxComponent"},
                                                       {"Name": "Mesh", "Scope": "Inherited", "Properties": {}}]})
        self.assertTrue(skip("K2:ComponentBoundEvent:/Script/Engine.PrimitiveComponent:OnComponentHit@trigger"))
        self.assertFalse(skip("K2:ComponentBoundEvent:/Script/Engine.PrimitiveComponent:OnComponentHit@Mesh"))
        self.assertFalse(skip("K2:Branch"))
        self.assertIsNone(listed_component_events({}))

    def test_a_pin_default_the_pin_refuses_fails_validate_as_apply_words_it(self):
        """A literal on a by-ref input: the override is sent with the template's pin name, and the refusal names the
        Make Literal node to link instead (Apply rewrites the same primitive facts the same way)."""
        set_key = "K2:CallFunction:/Script/AIModule.BlackboardComponent:SetValueAsObject"
        literal = "K2:CallFunction:/Script/Engine.KismetSystemLibrary:MakeLiteralName"
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Set", "Action": set_key, "PinOverrides": [{"Pin": "Key Name", "DefaultValue": "TargetActor"}]},
            {"Id": "Lit", "Action": literal, "PinOverrides": [{"Pin": "Value", "DefaultValue": "Other"}]}]}]}
        pins = {set_key: [{"name": "execute", "direction": "input", "category": "exec"},
                          {"name": "KeyName", "direction": "input", "category": "name"},
                          {"name": "ObjectValue", "direction": "input", "category": "object"}],
                literal: [{"name": "Value", "direction": "input", "category": "name"},
                          {"name": "ReturnValue", "direction": "output", "category": "name"}]}
        facts = {"index": 0, "pin_name": "KeyName", "code": "Graph.PinDefaultInvalid",
                 "message": "Pin 'KeyName': 'Key Name' must have an input wired into it.", "needs_link": "by_ref",
                 "pin_type": "Name", "make_action": literal, "make_input": "Value", "make_output": "ReturnValue"}
        calls = []

        def call(primitive, values):
            calls.append(values)
            if "describe" in values:
                return {"unresolved": [], "templates": [{"key": key, "can_add_pin": False, "pins": rows}
                                                        for key, rows in pins.items()]}
            return {"invalid_defaults": [facts]}
        graphs, _ = decode_body(body, self_class=SELF)
        found = check_graphs(call, graphs, self_class=SELF, target=TARGET)
        self.assertEqual([(item.code, item.path) for item in found],
                         [("Graph.PinDefaultInvalid", "/Body/UbergraphPages/0/Nodes/0/PinOverrides/0")])
        self.assertEqual(calls[-1]["defaults"], [
            {"action_key": set_key, "pin_name": "KeyName", "kind": "value", "value": "TargetActor"},
            {"action_key": literal, "pin_name": "Value", "kind": "value", "value": "Other"}])
        message = found[0].message
        self.assertTrue(message.startswith(facts["message"]))
        self.assertIn("Set:Key Name (Name) is a by-ref input", message)
        self.assertIn(f'{{"Id": "Set_KeyName", "Action": "{literal}", '
                      f'"PinOverrides": [{{"Pin": "Value", "DefaultValue": "TargetActor"}}]}}', message)
        self.assertIn('{"From": "Set_KeyName:ReturnValue", "To": "Set:Key Name"}', message)

    def test_a_link_the_editor_refuses_fails_validate(self):
        location = "K2:CallFunction:/Script/Engine.Actor:K2_GetActorLocation"
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [
            {"Id": "Where", "Action": location}, {"Id": "B", "Action": "K2:Branch"}],
            "Links": [{"From": "B:Condition", "To": "Where:ReturnValue"}]}]}
        pins = {location: [{"name": "ReturnValue", "direction": "output", "category": "struct"}],
                "K2:IfThenElse": [{"name": "Condition", "direction": "input", "category": "bool"}]}

        def call(primitive, values):
            if "describe" in values:
                return {"unresolved": [], "templates": [{"key": key, "can_add_pin": False, "pins": rows}
                                                        for key, rows in pins.items()]}
            return {"conversions": [], "refused": [{"index": 0, "from_pin": "ReturnValue", "to_pin": "Condition",
                                                    "from_type": "Vector", "to_type": "Boolean",
                                                    "reason": "Vector is not compatible with Boolean."}]}
        graphs, _ = decode_body(body, self_class=SELF)
        found = check_graphs(call, graphs, self_class=SELF, target=TARGET)
        self.assertEqual([(item.code, item.path) for item in found],
                         [("Graph.ConnectDisallowed", "/Body/UbergraphPages/0/Links/0")])
        self.assertEqual(found[0].message, "Cannot connect Where:ReturnValue (Vector) to B:Condition (Boolean): "
                                           "Vector is not compatible with Boolean.")

    def test_pin_default_message_for_other_refusals_and_containers(self):
        from asset_document.graphs.executor import pin_default_message
        self.assertEqual(pin_default_message("N", "P", "value", "x", "Pin 'P': bad.", {}), "Pin 'P': bad.")
        text = pin_default_message("N", "Items", "value", "x", "Pin 'Items': wire it.",
                                   {"needs_link": "container", "pin_type": "Array of Strings",
                                    "make_action": "K2:MakeArray", "make_output": "Array"})
        self.assertIn('{"Id": "N_Items", "Action": "K2:MakeArray"}', text)
        self.assertIn('{"From": "N_Items:Array", "To": "N:Items"} and link the values into it', text)
        text = pin_default_message("N", "Hit", "value", "x", "m", {"needs_link": "by_ref", "pin_type": "Hit Result"})
        self.assertIn("Link a variable or another node's output of type Hit Result into it.", text)
        text = pin_default_message("N", "Label", "text", "Hi", "m", {
            "needs_link": "by_ref", "pin_type": "Text", "make_action": "K2:CallFunction:/Script/Engine.KismetSystemLibrary:MakeLiteralText",
            "make_input": "Value", "make_output": "ReturnValue"})
        self.assertIn('"DefaultTextValue": "Hi"', text)

    def test_contract_errors_carry_every_diagnostic(self):
        error = contract_errors([RuntimeDiagnostic("/A", "X.One", "one"), RuntimeDiagnostic("/B", "X.Two", "two")])
        self.assertEqual([item.code for item in error.diagnostics], ["X.One", "X.Two"])
        self.assertIn("(+1 more)", str(error))


class EnumAndSocketTests(unittest.TestCase):
    def test_stored_enumerator_names_compare_as_display_names(self):
        graph = GraphDoc("G", "function", "G", "K2", nodes=(NodeDoc("Set", action="K2:Self", pin_overrides=(
            PinOverride("State", value="NewEnumerator1"), PinOverride("Count", value="3"))),))
        shown = display_enum_values(graph, {"newenumerator1": "Hit"})
        self.assertEqual([item.value for item in shown.nodes[0].pin_overrides], ["Hit", "3"])
        self.assertIs(display_enum_values(graph, {"newenumerator1": None}), graph)
        self.assertIs(display_enum_values(graph, None), graph)

    def test_the_socket_is_set_after_the_component_values(self):
        body = {"Components": [{"Name": "Body", "Class": "/Script/Engine.StaticMeshComponent"},
                               {"Name": "Spear", "Class": "/Script/Engine.StaticMeshComponent", "AttachTo": "Body",
                                "AttachSocket": "hand_r"}]}
        query = {"components": [
            {"name": "Body", "class": "/Script/Engine.StaticMeshComponent", "scope": "owned", "attach_to": ""},
            {"name": "Spear", "class": "/Script/Engine.StaticMeshComponent", "scope": "owned", "attach_to": "Body"}]}
        self.assertEqual(members.plan_component_attach(body, query), [])
        self.assertEqual(members.plan_component_sockets(body, query), [(
            "blueprint.member.component.attach.v1", {"name": "Spear", "attach_socket": "hand_r", "attach_to": "Body"},
            "/Body/Components/1/AttachSocket")])
        query["components"][1]["attach_socket"] = "HAND_R"
        self.assertEqual(members.plan_component_sockets(body, query), [])


if __name__ == "__main__":
    unittest.main()
