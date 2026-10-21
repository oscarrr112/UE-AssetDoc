"""Task6 binding-policy regressions with explicit evidence; no native UE claims."""
from copy import deepcopy
import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.dto import RuntimePhase
from asset_document.profiles.behavior_tree_codec import lower_authored_properties, tree_to_semantic_projection
from asset_document.profiles.behavior_tree_production import BehaviorTreeProfile
from asset_document.profiles.behavior_tree_validation import validate_tree_bindings
from asset_document.regions.primitive_catalog import PrimitiveOutcome, PrimitiveOutcomeStatus


TARGET = "/Game/Tests/BT_BindingPolicy"
BLACKBOARD = "/Game/Tests/BB_Child.BB_Child"
ROOT_CLASS = "/Script/AIModule.BTComposite_Sequence"
TASK_CLASS = "/Script/AssetDocumentTests.AssetDocumentSelectorTaskTestNode"
DECORATOR_CLASS = "/Script/AIModule.BTDecorator_Blackboard"
TASK_ID = "60606060DDDDDDDDEEEEEEEEFFFFFFFF"
TASK_PATH = "/Body/Tree/Root/Children/0"
PAYLOAD = "/Script/Test.BindingPayload"
SELECTOR = {"kind": "selector"}
VALUE = {"kind": "value"}


class EvidenceCatalog:
    """Declared schemas/defaults only; never implement UE filter or ancestry rules."""
    def __init__(self, defaults, schema, *, structs=None):
        self.classes = {}
        self.requests = []
        self.add(ROOT_CLASS, {}, {}, family="composite")
        self.add(TASK_CLASS, defaults, schema, family="task", structs=structs)

    def add(self, name, defaults, schema, *, family=None, structs=None):
        self.classes[name] = {"node_class_path": name, "authored_defaults": defaults,
                              "property_schema": schema, "struct_schemas": structs or {}}
        if family is not None:
            self.classes[name]["node_family"] = family

    def __call__(self, name, base=None):
        self.requests.append((name, base))
        return deepcopy(self.classes[name])


class BindingContext:
    """Supply phase-local observations without simulating any native algorithm."""
    phase = RuntimePhase.VALIDATE

    def __init__(self, observations, blackboard_keys=None):
        self.observations = observations
        self.blackboard_keys = blackboard_keys
        self.calls = []
        self.dependency_calls = []
        self.handoffs = []
        self.read_handle = object()
        self.ai_handle = object()

    def handoff(self, *, source_handle, target_provider_id, access):
        if source_handle is not self.read_handle or target_provider_id != "ai" or access != "read_only":
            raise AssertionError("Retained validation requires this phase's read-only AI lease")
        self.handoffs.append((source_handle, target_provider_id, access))
        return self.ai_handle

    def invoke(self, *, requirement, value_arguments, handle_arguments):
        if requirement.primitive_id == "editor.reference.resolve_asset.v1":
            if requirement.version != 1 or handle_arguments:
                raise AssertionError("Dependency lookup must use the admitted read-only loader")
            self.dependency_calls.append(deepcopy(value_arguments))
            return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED, None, (), (), ())
        if (requirement.primitive_id, requirement.version, requirement.required_by_route_id) != (
                "ai.behavior_tree.binding.inspect.v1", 1, "ai_profile"):
            raise AssertionError("Binding policy invoked an unexpected primitive")
        self.calls.append((deepcopy(value_arguments), handle_arguments))
        result = {"evidence": deepcopy(self.observations)}
        if self.blackboard_keys is not None:
            result["blackboard_keys"] = list(self.blackboard_keys)
        return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED, result, (), (), ())


def selector_observation(*, none_allowed=False, key_exists=True, filter_passes=True,
                         enum_values=None, decorator=False):
    return {"none_allowed": none_allowed, "key_exists": key_exists, "filter_passes": filter_passes,
            "enum_values_or_null": enum_values, "blackboard_decorator_selector": decorator}


def authored_tree(properties, *, decorator=False):
    task = {"Id": TASK_ID, "Class": TASK_CLASS, "Properties": properties}
    if decorator:
        task["Properties"] = {}
        task["Decorators"] = [{"Id": "ABCDABCDABCDABCDABCDABCDABCDABCD",
                               "Class": DECORATOR_CLASS, "Properties": properties}]
    return {"GraphGuid": "ABCDEFABCDEFABCDEFABCDEFABCDEFAB", "Root": {
        "Id": "11111111222222223333333344444444", "Class": ROOT_CLASS, "Children": [task]}}


def projection(properties, catalog, *, decorator=False):
    return tree_to_semantic_projection(authored_tree(properties, decorator=decorator),
        property_mapper=lambda name, value: lower_authored_properties(
            name, value, evidence=catalog, object_evidence=catalog))


def value_or_fixture():
    """Stock eleven-family authored parameters, supplied as inspected type evidence."""
    actor = {"Kind": "ClassRef", "Path": "/Script/Engine.Actor"}
    enum = {"Kind": "AssetRef", "Path": "/Script/Test.EBindingMode"}
    payload = {"Struct": PAYLOAD, "Properties": {"Count": 37, "Label": "isolated"}}
    vectors = {"Vector": ("X", "Y", "Z"), "Rotator": ("Pitch", "Yaw", "Roll")}
    literals = {"Bool": True, "Class": actor, "Enum": 3, "Float": 0.1, "Int32": 37,
                "Name": "LiteralName", "String": "literal text", "Object": None,
                "Rotator": {"Pitch": 1.0, "Yaw": 2.0, "Roll": 3.0},
                "Struct": payload, "Vector": {"X": 4.0, "Y": 5.0, "Z": 6.0}}
    defaults, schema, authored = {}, {}, {}
    for family, literal in literals.items():
        fields = {"Key": {"kind": "name"}, "DefaultValue": VALUE}
        if family == "Float":
            fields["DefaultValue"] = {"kind": "number", "precision": "float32"}
        elif family == "Name":
            fields["DefaultValue"] = {"kind": "name"}
        elif family in vectors:
            fields["DefaultValue"] = {"kind": "struct", "fields": {
                name: {"kind": "number", "precision": "float64"} for name in vectors[family]}}
        elif family == "Struct":
            fields["DefaultValue"] = {"kind": "instanced_struct"}
        item = {"Key": "None", "DefaultValue": deepcopy(literal)}
        if family in ("Class", "Object"):
            fields["BaseClass"] = VALUE
            item["BaseClass"] = deepcopy(actor)
        elif family == "Enum":
            fields.update(EnumType=VALUE, NativeEnumTypeName=VALUE)
            item.update(EnumType=enum, NativeEnumTypeName="EBindingMode")
        elif family == "Struct":
            fields["bCanEditDefaultValueType"] = VALUE
            item["bCanEditDefaultValueType"] = True
        name = family + "Value"
        defaults[name] = item
        schema[name] = {"kind": "struct", "struct_path": "/Script/AIModule.ValueOrBBKey_" + family,
                        "fields": fields}
        authored[name] = dict(deepcopy(item), Key=family)
    catalog = EvidenceCatalog(defaults, schema, structs={PAYLOAD: {"Count": VALUE, "Label": VALUE}})
    return catalog, authored


class BehaviorTreeBindingPolicyTests(unittest.TestCase):
    def assert_diagnostic(self, operation, code, path):
        with self.assertRaises(AssetDocumentContractError) as raised:
            operation()
        self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path), (code, path))

    def validate(self, tree, catalog, context, *, blackboard=BLACKBOARD, retained=False):
        return validate_tree_bindings(tree, context=context, blackboard=blackboard, class_evidence=catalog,
                                      target=TARGET, read_handle=context.read_handle, retained=retained)

    def test_authored_omitted_required_none_rejected_but_retained_none_is_preserved(self):
        # Task6.Selectors.OmittedRequiredKeyParity and sparse Blackboard overlay.
        catalog = EvidenceCatalog({"DirectSelector": {"Key": "None"}}, {"DirectSelector": SELECTOR})
        current = projection({}, catalog)
        before = deepcopy(current)
        profile = BehaviorTreeProfile()
        authored = {"Target": TARGET, "Body": {"Tree": authored_tree({}), "BlackboardAsset": BLACKBOARD}}
        context = BindingContext([selector_observation(none_allowed=False)])
        self.assert_diagnostic(lambda: profile._validate_regions(authored, context=context, exists=True,
            read=context.read_handle, current=current, evidence=catalog), "MissingBlackboardKey",
            TASK_PATH + "/Properties/DirectSelector/Key")
        self.assertEqual(context.calls[0][0]["source"], {"kind": "class_defaults", "node_class_path": TASK_CLASS})
        self.assertEqual(context.calls[0][1], ())
        self.assertEqual(context.handoffs, [])
        self.assertEqual(context.dependency_calls, [{"path": BLACKBOARD, "expected_class": "/Script/AIModule.BlackboardData"}])
        for blackboard in (BLACKBOARD, None):
            context = BindingContext([selector_observation(none_allowed=False)])
            overlay = {"Target": TARGET, "Body": {"BlackboardAsset": blackboard}}
            profile._validate_regions(overlay, context=context, exists=True, read=context.read_handle,
                                      current=current, evidence=catalog)
            values, handles = context.calls[0]
            self.assertEqual(values["source"], {"kind": "live_node", "behavior_tree_path": TARGET + ".BT_BindingPolicy",
                                                "stable_node_id": TASK_ID})
            self.assertEqual([(item.name, item.handle) for item in handles], [("asset", context.ai_handle)])
        self.assertEqual(current, before)

    def test_selector_missing_blackboard_unknown_key_and_wrong_type_keep_task6_paths(self):
        catalog = EvidenceCatalog({"DirectSelector": {"Key": "None"}}, {"DirectSelector": SELECTOR})
        for label, blackboard, observed, code, path in (
            ("missing-blackboard", None, selector_observation(), "MissingBehaviorTreeBlackboard", "/Body/BlackboardAsset"),
            ("unknown", BLACKBOARD, selector_observation(key_exists=False), "UnknownBlackboardKey", TASK_PATH + "/Properties/DirectSelector/Key"),
            ("wrong-type", BLACKBOARD, selector_observation(filter_passes=False), "IncompatibleBlackboardKeyType", TASK_PATH + "/Properties/DirectSelector/Key"),
        ):
            for retained in (False, True):
                with self.subTest(label=label, retained=retained):
                    tree = projection({"DirectSelector": {"Key": "WrongType"}}, catalog)
                    self.assert_diagnostic(lambda: self.validate(tree, catalog, BindingContext([observed]),
                        blackboard=blackboard, retained=retained), code, path)
        # The native evidence lists the Blackboard's keys when one is unknown: the message names the close ones.
        tree = projection({"DirectSelector": {"Key": "TargetActr"}}, catalog)
        with self.assertRaises(AssetDocumentContractError) as raised:
            self.validate(tree, catalog, BindingContext([selector_observation(key_exists=False)],
                                                        ["SelfActor", "TargetActor", "TargetLocation"]))
        self.assertEqual(raised.exception.diagnostic.code, "UnknownBlackboardKey")
        self.assertIn("'TargetActr'", raised.exception.diagnostic.message)
        self.assertIn("Close: TargetActor, TargetLocation.", raised.exception.diagnostic.message)
        with self.assertRaises(AssetDocumentContractError) as raised:
            self.validate(tree, catalog, BindingContext([selector_observation(key_exists=False)], ["Health", "Ammo"]))
        self.assertIn("Keys: Ammo, Health.", raised.exception.diagnostic.message)
        catalog = EvidenceCatalog({"OptionalSelector": {"Key": "None"}}, {"OptionalSelector": SELECTOR})
        tree = projection({}, catalog)
        self.validate(tree, catalog, BindingContext([selector_observation(none_allowed=True)]), blackboard=None)

    def test_eleven_value_or_families_send_complete_typed_authored_parameters(self):
        catalog, authored = value_or_fixture()
        tree = projection(authored, catalog)
        before = deepcopy((authored, tree))
        observed = [{"key_exists": True, "compatible": True} for _ in authored]
        context = BindingContext(observed)
        self.validate(tree, catalog, context)
        queries = context.calls[0][0]["queries"]
        self.assertEqual(len(queries), 11)
        by_name = {row["locator"][0]["name"]: row for row in queries}
        for name, parameters in tree["root"]["children"][0]["authored_properties"].items():
            self.assertEqual(by_name[name], {"kind": "value_or", "locator": [{"kind": "field", "name": name}],
                "selected_key": name.removesuffix("Value"), "authored_value_or_null": parameters})
        self.assertEqual(by_name["FloatValue"]["authored_value_or_null"]["DefaultValue"], 0.10000000149011612)
        self.assertEqual(by_name["EnumValue"]["authored_value_or_null"]["EnumType"]["Path"], "/Script/Test.EBindingMode")
        self.assertEqual(by_name["StructValue"]["authored_value_or_null"]["DefaultValue"]["Properties"]["Count"], 37)
        retained = BindingContext(observed)
        self.validate(tree, catalog, retained, retained=True)
        self.assertTrue(all(row["authored_value_or_null"] is None for row in retained.calls[0][0]["queries"]))
        self.assertEqual((authored, tree), before)

    def test_value_or_literal_none_and_retained_null_metadata_are_allowed(self):
        catalog, authored = value_or_fixture()
        for item in authored.values():
            item["Key"] = ""
        authored["EnumValue"]["EnumType"] = None
        authored["StructValue"]["DefaultValue"] = None
        tree = projection(authored, catalog)
        for retained in (False, True):
            with self.subTest(retained=retained):
                self.validate(tree, catalog, BindingContext([
                    {"key_exists": False, "compatible": False} for _ in authored]), blackboard=None, retained=retained)

    def test_value_or_bound_keys_reject_missing_or_incompatible_observations(self):
        catalog, authored = value_or_fixture()
        for name in authored:
            authored[name]["Key"] = "None"
        authored["FloatValue"]["Key"] = "GoodDuration"
        tree = projection(authored, catalog)
        names = list(authored)
        for blackboard, observed, code, path in (
            (None, {"key_exists": True, "compatible": True}, "MissingBehaviorTreeBlackboard", "/Body/BlackboardAsset"),
            (BLACKBOARD, {"key_exists": False, "compatible": False}, "UnknownValueOrBlackboardKey", TASK_PATH + "/Properties/FloatValue/Key"),
            (BLACKBOARD, {"key_exists": True, "compatible": False}, "IncompatibleValueOrBlackboardKeyType", TASK_PATH + "/Properties/FloatValue/Key"),
        ):
            for retained in (False, True):
                with self.subTest(code=code, retained=retained):
                    rows = [{"key_exists": False, "compatible": False} for _ in authored]
                    rows[names.index("FloatValue")] = observed
                    self.assert_diagnostic(lambda: self.validate(tree, catalog, BindingContext(rows),
                        blackboard=blackboard, retained=retained), code, path)
        # Live Enum/Struct with null type metadata must not inherit authored DTOs.
        for name, parameter in (("EnumValue", "EnumType"), ("StructValue", "DefaultValue")):
            with self.subTest(name=name):
                current = deepcopy(tree)
                properties = current["root"]["children"][0]["authored_properties"]
                properties["FloatValue"]["Key"] = "None"
                properties[name]["Key"] = name.removesuffix("Value")
                properties[name][parameter] = None
                rows = [{"key_exists": True, "compatible": False} for _ in authored]
                context = BindingContext(rows)
                self.assert_diagnostic(lambda: self.validate(current, catalog, context, retained=True),
                    "IncompatibleValueOrBlackboardKeyType", TASK_PATH + "/Properties/" + name + "/Key")
                self.assertTrue(all(row["authored_value_or_null"] is None for row in context.calls[0][0]["queries"]))

    def test_dynamic_struct_direct_value_or_families_keep_binding_type_and_parameters(self):
        # The unrestricted dynamic field can hold a ValueOr itself, not only a
        # payload containing one. Dropping that exact type silently skips policy.
        families, authored = value_or_fixture()
        observed_class = families(TASK_CLASS)
        structs = dict(observed_class["struct_schemas"])
        structs.update({shape["struct_path"]: shape["fields"]
                        for shape in observed_class["property_schema"].values()})
        catalog = EvidenceCatalog({"DynamicValue": None}, {"DynamicValue": {"kind": "instanced_struct"}},
                                  structs=structs)
        for name, parameters in authored.items():
            struct_path = observed_class["property_schema"][name]["struct_path"]
            for retained in (False, True):
                for key, observation, code in (
                    ("Missing", {"key_exists": False, "compatible": False}, "UnknownValueOrBlackboardKey"),
                    ("WrongType", {"key_exists": True, "compatible": False}, "IncompatibleValueOrBlackboardKeyType"),
                ):
                    with self.subTest(family=name, retained=retained, key=key):
                        tree = projection({"DynamicValue": {"Struct": struct_path,
                            "Properties": dict(deepcopy(parameters), Key=key)}}, catalog)
                        complete = tree["root"]["children"][0]["authored_properties"]["DynamicValue"]["Properties"]
                        context = BindingContext([observation])
                        self.assert_diagnostic(lambda: self.validate(tree, catalog, context, retained=retained), code,
                            TASK_PATH + "/Properties/DynamicValue/Properties/Key")
                        self.assertEqual(len(context.calls), 1)
                        self.assertEqual(context.calls[0][0]["queries"], [{"kind": "value_or",
                            "locator": [{"kind": "field", "name": "DynamicValue"},
                                        {"kind": "instanced_struct", "struct_path": struct_path}],
                            "selected_key": key, "authored_value_or_null": None if retained else complete}])

    def test_subtree_forward_compatibility_uses_native_decision_without_structural_guessing(self):
        schema = {"BehaviorAsset": {"kind": "value", "binding_kind": "static_subtree"}}
        catalog = EvidenceCatalog({"BehaviorAsset": None}, schema)
        tree = projection({"BehaviorAsset": {"Kind": "AssetRef", "Path": "/Game/Tests/ParentTree.ParentTree"}}, catalog)
        for label, exists, compatible in (("same", True, True), ("desired-descends-from-subtree", True, True),
                ("reverse-ancestry", True, False), ("siblings", True, False), ("structural-only", True, False),
                ("missing-subtree", False, False), ("static-null-blackboard", True, False)):
            with self.subTest(label=label):
                context = BindingContext([{"exists": exists, "effective_blackboard_compatible": compatible,
                    "has_root_decorators": True, "blackboard_path_or_null": "/Game/Tests/Parent.Parent"}])
                if compatible:
                    self.validate(tree, catalog, context)
                else:
                    self.assert_diagnostic(lambda: self.validate(tree, catalog, context),
                        "IncompatibleBehaviorTreeBlackboard", TASK_PATH + "/Properties/BehaviorAsset")
        self.assert_diagnostic(lambda: self.validate(tree, catalog, BindingContext([
            {"exists": True, "effective_blackboard_compatible": True}]), blackboard=None),
            "MissingBehaviorTreeBlackboard", "/Body/BlackboardAsset")

    def test_subtree_static_null_rejected_dynamic_null_allowed_and_root_decorators_rejected(self):
        for kind, name in (("static_subtree", "BehaviorAsset"), ("dynamic_subtree", "DefaultBehaviorAsset")):
            catalog = EvidenceCatalog({name: None}, {name: {"kind": "value", "binding_kind": kind}})
            tree = projection({name: None}, catalog)
            context = BindingContext([{"exists": False, "effective_blackboard_compatible": False,
                                       "has_root_decorators": False, "blackboard_path_or_null": None}])
            if kind == "static_subtree":
                self.assert_diagnostic(lambda: self.validate(tree, catalog, context, blackboard=None),
                    "IncompatibleBehaviorTreeBlackboard", TASK_PATH + "/Properties/" + name)
            else:
                self.validate(tree, catalog, context, blackboard=None)
                self.assertIsNone(context.calls[0][0]["queries"][0]["subtree_path_or_null"])
                tree = projection({name: {"Kind": "AssetRef", "Path": "/Game/Tests/Dynamic.Dynamic"}}, catalog)
                self.assert_diagnostic(lambda: self.validate(tree, catalog, BindingContext([
                    {"exists": True, "effective_blackboard_compatible": True, "has_root_decorators": True}])),
                    "DynamicSubtreeRootDecoratorsUnsupported", TASK_PATH + "/Properties/DefaultBehaviorAsset")

    def test_nested_typed_locators_include_container_identity_and_inline_declared_base(self):
        nested = {"kind": "struct", "fields": {"Selector": SELECTOR, "Identity": VALUE}}
        inline_class, inline_base = "/Script/Test.InlineSelector", "/Script/Test.InlineBase"
        entry = lambda name: {"Selector": {"Key": "InheritedActor"}, "Identity": name}
        properties = {"NestedValue": entry("Nested"), "ArrayValues": [entry("Array")],
            "MapValues": {"NamedEntry": entry("Map")}, "SetValues": [entry("Set")],
            "DynamicValue": {"Struct": PAYLOAD, "Properties": entry("Dynamic")},
            "InlineValue": {"Class": inline_class, "Properties": entry("Inline")},
            "ScalarValue": {"Key": "MissingActor"},
            "BehaviorAsset": {"Kind": "AssetRef", "Path": "/Game/Tests/Scalar.Scalar"},
            "OrdinaryStruct": {"Key": "WrongType", "DefaultValue": 12}}
        schema = {"NestedValue": nested, "ArrayValues": {"kind": "array", "element": nested},
            "MapValues": {"kind": "map", "value": nested}, "SetValues": {"kind": "set", "element": nested},
            "DynamicValue": {"kind": "instanced_struct"},
            "InlineValue": {"kind": "instanced_object", "declared_class_path": inline_base},
            "ScalarValue": VALUE, "BehaviorAsset": VALUE,
            "OrdinaryStruct": {"kind": "struct", "struct_path": "/Script/Test.NotValueOr",
                "fields": {"Key": {"kind": "name"}, "DefaultValue": VALUE}}}
        defaults = {"NestedValue": entry("Default"), "ArrayValues": [], "MapValues": {}, "SetValues": [],
                    "DynamicValue": None, "InlineValue": None, "ScalarValue": None, "BehaviorAsset": None,
                    "OrdinaryStruct": {"Key": "None", "DefaultValue": 0}}
        catalog = EvidenceCatalog(defaults, schema, structs={PAYLOAD: nested["fields"]})
        catalog.add(inline_class, entry("Default"), nested["fields"])
        tree = projection(properties, catalog)
        context = BindingContext([selector_observation() for _ in range(6)])
        self.validate(tree, catalog, context)
        queries = context.calls[0][0]["queries"]
        locators = {row["locator"][0]["name"]: row["locator"][1:] for row in queries}
        member = {"kind": "field", "name": "Selector"}
        self.assertEqual(locators, {
            "NestedValue": [member], "ArrayValues": [{"kind": "array_element", "index": 0}, member],
            "MapValues": [{"kind": "map_value", "key": "NamedEntry"}, member],
            "SetValues": [{"kind": "set_element", "value": entry("Set")}, member],
            "DynamicValue": [{"kind": "instanced_struct", "struct_path": PAYLOAD}, member],
            "InlineValue": [{"kind": "instanced_object", "class_path": inline_class}, member]})
        self.assertTrue(all(row["kind"] == "selector" and row["selected_key"] == "InheritedActor" for row in queries))
        self.assertIn((inline_class, inline_base), catalog.requests)
        self.assertEqual(properties["ScalarValue"], {"Key": "MissingActor"})

    def test_recursive_map_key_failure_uses_exact_escaped_json_pointer(self):
        nested = {"kind": "struct", "fields": {"Selector": SELECTOR}}
        catalog = EvidenceCatalog({"MapValues": {}}, {"MapValues": {"kind": "map", "value": nested}})
        for key, token in (("NamedEntry", "NamedEntry"), ("bad/~", "bad~1~0")):
            with self.subTest(key=key):
                tree = projection({"MapValues": {key: {"Selector": {"Key": "MissingActor"}}}}, catalog)
                self.assert_diagnostic(lambda: self.validate(tree, catalog, BindingContext([
                    selector_observation(key_exists=False)])), "UnknownBlackboardKey",
                    TASK_PATH + "/Properties/MapValues/" + token + "/Selector/Key")

    def test_blackboard_decorator_enum_token_and_integer_staleness_rejected(self):
        catalog = EvidenceCatalog({}, {})
        catalog.add(DECORATOR_CLASS, {"BlackboardKey": {"Key": "None"}, "StringValue": "", "IntValue": 0},
                    {"BlackboardKey": SELECTOR, "StringValue": VALUE, "IntValue": VALUE}, family="decorator")
        path = TASK_PATH + "/Decorators/0/Properties/"
        for retained in (False, True):
            for tokens, number, code, leaf in (({"SharedValue": 7}, 1, "BehaviorTreeEnumDecoratorValueOutOfSync", "IntValue"),
                ({"ReplacementValue": 9}, 1, "UnknownBehaviorTreeEnumDecoratorValue", "StringValue"),
                ({"SharedValue": 7, "EBindingMode::SharedValue": 7}, 7, None, None)):
                with self.subTest(retained=retained, code=code):
                    tree = projection({"BlackboardKey": {"Key": "Mode"}, "StringValue": "SharedValue", "IntValue": number},
                                      catalog, decorator=True)
                    before = deepcopy(tree)
                    context = BindingContext([selector_observation(enum_values=tokens, decorator=True)])
                    if code:
                        self.assert_diagnostic(lambda: self.validate(tree, catalog, context, retained=retained), code, path + leaf)
                    else:
                        self.validate(tree, catalog, context, retained=retained)
                    self.assertEqual(tree, before)

    def test_bound_node_diagnostics_follow_authored_order_while_retained_uses_readback_order(self):
        catalog = EvidenceCatalog({}, {})
        catalog.add(DECORATOR_CLASS, {"BlackboardKey": {"Key": "None"}, "StringValue": "", "IntValue": 0},
                    {"BlackboardKey": SELECTOR, "StringValue": VALUE, "IntValue": VALUE}, family="decorator")
        sink_id, test_id = "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF", "11111111aaaaaaaaaaaaaaaaaaaaaaaa"
        bound_path = TASK_PATH + "/Decorators/0/BoundGraph/Nodes/"

        def source_tree(*, test_first=False, class_role=False):
            sink = {"Id": sink_id, "Kind": "Sink"}
            test = {"Id": test_id, "Kind": "Test", "Class": TASK_CLASS if class_role else DECORATOR_CLASS,
                    "Properties": {} if class_role else {"BlackboardKey": {"Key": "Mode"},
                                                         "StringValue": "SharedValue", "IntValue": 1}}
            source = authored_tree({})
            source["Root"]["Children"][0]["Decorators"] = [{
                "Id": "ABCDABCDABCDABCDABCDABCDABCDABCD", "Kind": "Composite", "BoundGraph": {
                    "GraphGuid": "EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE", "Nodes": [test, sink] if test_first else [sink, test],
                    "Links": [{"From": test_id, "To": sink_id, "ToInput": 0}]}}]
            return source

        def lower(source):
            return tree_to_semantic_projection(source, property_mapper=lambda name, value:
                lower_authored_properties(name, value, evidence=catalog))

        for test_first in (False, True):
            for label, observed, code, leaf in (
                ("unknown", selector_observation(key_exists=False), "UnknownBlackboardKey", "Properties/BlackboardKey/Key"),
                ("filter", selector_observation(filter_passes=False), "IncompatibleBlackboardKeyType", "Properties/BlackboardKey/Key"),
                ("enum-integer", selector_observation(enum_values={"SharedValue": 7}, decorator=True),
                 "BehaviorTreeEnumDecoratorValueOutOfSync", "Properties/IntValue"),
                ("enum-token", selector_observation(enum_values={"ReplacementValue": 9}, decorator=True),
                 "UnknownBehaviorTreeEnumDecoratorValue", "Properties/StringValue"),
                ("class-role", selector_observation(), "InvalidBehaviorTreeNodeClass", "Class"),
            ):
                with self.subTest(test_first=test_first, label=label):
                    source = source_tree(test_first=test_first, class_role=label == "class-role")
                    tree = lower(source)
                    self.assertEqual(tree["root"]["children"][0]["decorators"][0]["bound_graph"]["nodes"][0]["stable_node_id"],
                                     test_id.upper())
                    context = BindingContext([observed])
                    self.assert_diagnostic(lambda: validate_tree_bindings(tree, context=context, blackboard=BLACKBOARD,
                        class_evidence=catalog, authored_tree=source), code,
                        bound_path + ("0/" if test_first else "1/") + leaf)
        source = source_tree()
        tree = lower(source)
        # The production branch must forward canonical intent, not sorted readback.
        context = BindingContext([selector_observation(key_exists=False)])
        self.assert_diagnostic(lambda: BehaviorTreeProfile()._validate_regions(
            {"Target": TARGET, "Body": {"Tree": source, "BlackboardAsset": BLACKBOARD}},
            context=context, exists=False, read=context.read_handle, evidence=catalog),
            "UnknownBlackboardKey", bound_path + "1/Properties/BlackboardKey/Key")
        self.assert_diagnostic(lambda: self.validate(tree, catalog, BindingContext([
            selector_observation(key_exists=False)]), retained=True),
            "UnknownBlackboardKey", bound_path + "0/Properties/BlackboardKey/Key")

    def test_enum_policy_does_not_follow_an_unrelated_selector_field_spelling(self):
        catalog = EvidenceCatalog({"BlackboardKey": {"Key": "None"}, "StringValue": "SharedValue", "IntValue": 1},
                                 {"BlackboardKey": SELECTOR, "StringValue": VALUE, "IntValue": VALUE})
        self.validate(projection({"BlackboardKey": {"Key": "Mode"}}, catalog), catalog,
                      BindingContext([selector_observation(enum_values={"ReplacementValue": 9}, decorator=False)]))


if __name__ == "__main__":
    unittest.main()
