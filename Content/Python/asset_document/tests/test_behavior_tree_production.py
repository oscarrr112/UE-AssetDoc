"""Pure Profile orchestration tests; fake outcomes are not native acceptance evidence."""
from copy import deepcopy
from dataclasses import dataclass
from types import SimpleNamespace
import unittest

from asset_document.diagnostics import AssetDocumentContractError, RuntimeDiagnostic
from asset_document.dto import RuntimeOperation, RuntimePhase
from asset_document.profiles.behavior_tree_codec import (
    authored_digest_v2, lower_authored_properties, tree_to_semantic_projection,
)
from asset_document.profiles.behavior_tree_production import BehaviorTreeProfile
from asset_document.regions.primitive_catalog import (
    NamedOpaqueHandleResult, PrimitiveOutcome, PrimitiveOutcomeStatus,
)

TARGET = "/Game/Tests/BT_Production"
CLASS = "/Script/AIModule.BehaviorTree"


def guid(index):
    return f"{index:032x}"


def evidence(class_path):
    return {"node_class_path": class_path, "authored_defaults": {"Mode": 1, "Count": 8, "Duration": 0.0},
            "property_schema": {"Mode": {"kind": "enum", "values": {"First": 1, "Second": 4}},
                                "Count": {"kind": "value"}, "Duration": {"kind": "number", "precision": "float32"}},
            "struct_schemas": {}}


def document():
    return {"SchemaVersion": 1, "Class": CLASS, "Target": TARGET, "AssetType": "GenericAsset",
            "Action": "Update", "Properties": {}, "Body": {"BlackboardAsset": None, "Tree": {
                "GraphGuid": guid(100), "Comments": [{"Id": guid(9), "Text": "完整树"}],
                "Root": {"Id": guid(1), "Class": "/Script/AIModule.BTComposite_Sequence",
                         "Properties": {"Mode": "Second"},
                         "Children": [{"Id": guid(2), "Class": "/Script/AIModule.BTTask_Wait", "Properties": {"Duration": 0.1}}],
                         "Services": [{"Id": guid(3), "Class": "/Script/AIModule.BTService_DefaultFocus"}],
                         "Decorators": [{"Id": guid(4), "Class": "/Script/AIModule.BTDecorator_Blackboard"},
                                        {"Id": guid(5), "Kind": "Composite", "Properties": {"CompositeName": "Gate", "bShowOperations": True},
                                         "BoundGraph": {"GraphGuid": guid(101), "Nodes": [
                                             {"Id": guid(6), "Kind": "Sink"},
                                             {"Id": guid(7), "Kind": "Test", "Class": "/Script/AIModule.BTDecorator_Blackboard"}],
                                             "Links": [{"From": guid(7), "To": guid(6), "ToInput": 0}]}}]}}}}


def projection(doc):
    return tree_to_semantic_projection(doc["Body"]["Tree"], property_mapper=lambda name, props:
                                      lower_authored_properties(name, props, evidence=evidence))


@dataclass(frozen=True)
class Handle:
    context: object
    name: str


class State:
    def __init__(self, doc=None, *, exists=True):
        self.exists = exists
        self.tree = projection(doc or document()) if exists else {"graph_state": "absent", "graph_guid": None, "root": None, "comments": []}
        self.blackboard = None
        self.top_properties = {"Description": "same"}
        self.selectors = [{"stable_node_id": guid(2), "property_path": "Native.ExactPath", "selected_key_name": "None"}]


class TypedContext:
    """Exercise actual Python wrappers with explicit phase-owned opaque tokens."""
    def __init__(self, phase, state):
        self.phase, self.state = phase, state
        self.calls, self.handoffs = [], []
        self.identity = object()
        self.fail_id = None
        self.scratch = None
        self.class_inspector = evidence
        self.reference_failures = {}

    def handle(self, name):
        return Handle(self.identity, name)

    def handoff(self, *, source_handle, target_provider_id, access):
        assert source_handle.context is self.identity, "A handle crossed phase dispatches"
        assert target_provider_id == "ai"
        if self.phase is RuntimePhase.VERIFY:
            assert access == "read_only"
        self.handoffs.append((source_handle, access))
        return self.handle("ai:" + access)

    def invoke(self, *, requirement, value_arguments, handle_arguments):
        if requirement.primitive_id == "editor.class.resolve_canonical.v1":
            assert self.phase in (RuntimePhase.VALIDATE, RuntimePhase.PREFLIGHT, RuntimePhase.APPLY)
        if requirement.primitive_id.startswith("ai.behavior_tree."):
            def assert_native_order(value):
                if type(value) is dict:
                    from asset_document.profiles.behavior_tree_execution import _native_field_order
                    assert list(value) == sorted(value, key=_native_field_order), "AI native DTO requires FString-ordered fields"
                    for item in value.values():
                        assert_native_order(item)
                elif type(value) is list:
                    for item in value:
                        assert_native_order(item)
            assert_native_order(value_arguments)
        for argument in handle_arguments:
            assert argument.handle.context is self.identity, "A handle crossed phase dispatches"
        name = requirement.primitive_id
        self.calls.append((name, requirement.version, deepcopy(value_arguments), handle_arguments))
        if name == self.fail_id:
            return PrimitiveOutcome(PrimitiveOutcomeStatus.FAILED, None, (), (),
                                    (RuntimeDiagnostic("/Test", "Test.ProviderRejected", "Focused provider rejection"),))
        if name in ("editor.class.resolve_canonical.v1", "editor.reference.resolve_asset.v1"):
            path_key, expected_key = ("class", "expected_base") if name == "editor.class.resolve_canonical.v1" else ("path", "expected_class")
            diagnostic = self.reference_failures.get((name, value_arguments[path_key], value_arguments[expected_key]))
            if diagnostic is not None:
                return PrimitiveOutcome(PrimitiveOutcomeStatus.FAILED, None, (), (), (diagnostic,))
        value, groups = {}, ()
        if name == "editor.asset.resolve_existing.v1":
            value = {"exists": self.state.exists}
            groups = (NamedOpaqueHandleResult("asset_read", (self.handle("read"),) if self.state.exists else ()),)
            if self.phase is not RuntimePhase.VERIFY:
                groups += (NamedOpaqueHandleResult("asset_mutable", (self.handle("mutable"),) if self.state.exists else ()),)
        elif name == "editor.class.resolve_canonical.v1":
            groups = (NamedOpaqueHandleResult("class", (self.handle("class"),)),)
        elif name == "editor.reference.resolve_asset.v1":
            assert self.phase in (RuntimePhase.VALIDATE, RuntimePhase.PREFLIGHT)
            if type(value_arguments["path"]) is not str:
                return PrimitiveOutcome(PrimitiveOutcomeStatus.FAILED, None, (), (),
                    (RuntimeDiagnostic("/ValueArguments", "ClassReference.InvalidAssetValueShape", "Path must be a string"),))
            value = {"canonical_path": value_arguments["path"], "canonical_class": value_arguments["expected_class"] or CLASS}
            groups = (NamedOpaqueHandleResult("asset", (self.handle("reference"),)),)
        elif name == "editor.asset.create_by_factory.v1":
            self.state.exists = True
            groups = (NamedOpaqueHandleResult("asset_mutable", (self.handle("factory-lifecycle"),)),)
        elif name == "editor.property.enumerate_admitted.v1":
            value = {"properties": [{"name": "Description", "declaring_class": CLASS,
                       "codec_id": "test.string.v1", "readable": True, "writable": True, "default": "same"}]}
            groups = (NamedOpaqueHandleResult("properties", (self.handle("Description"),)),)
        elif name == "editor.property.read_admitted.v1":
            value = {"value": self.state.top_properties["Description"]}
        elif name == "ai.behavior_tree.authored_graph.readback.v2":
            value = deepcopy(self.state.tree)
        elif name == "ai.behavior_tree.blackboard.readback.v1":
            value = {"blackboard_path_or_null": self.state.blackboard}
        elif name == "ai.behavior_tree.node_class.inspect.v2":
            value = self.class_inspector(value_arguments["node_class_path"])
        elif name == "ai.behavior_tree.selector.inventory.v1":
            value = {"selectors": deepcopy(self.state.selectors)}
        elif name == "ai.behavior_tree.graph.create.v1":
            groups = (NamedOpaqueHandleResult("graph", (self.handle("scratch"),)),)
        elif name == "ai.behavior_tree.graph.author_scratch.v1":
            self.scratch = deepcopy(value_arguments["tree"])
            value = deepcopy(self.scratch)
        elif name == "ai.behavior_tree.graph.install.v1":
            self.state.tree = deepcopy(self.scratch)
        elif name == "ai.behavior_tree.blackboard.set.v1":
            self.state.blackboard = value_arguments["blackboard_path_or_null"]
        elif name not in ("ai.behavior_tree.selector.resolve_batch.v2", "ai.behavior_tree.value_or_key.refresh.v3"):
            raise AssertionError("Unexpected primitive " + name)
        return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED, value, groups, (), ())


class BehaviorTreeProductionTests(unittest.TestCase):
    def test_body_owned_top_properties_restore_original_public_diagnostics(self):
        profile = BehaviorTreeProfile()
        owners = {"BlackboardAsset": "Body.BlackboardAsset", "BTGraph": "Body.Tree",
                  "RootNode": "Body.Tree", "RootDecorators": "Body.Tree", "RootDecoratorOps": "Body.Tree"}
        for phase in (RuntimePhase.VALIDATE, RuntimePhase.PREFLIGHT):
            operation = profile.validate if phase is RuntimePhase.VALIDATE else profile.preflight
            for managed, owner in owners.items():
                for authored in (managed, managed.lower(), managed.upper()):
                    with self.subTest(phase=phase, authored=authored):
                        doc = document(); doc["Body"] = {}; doc["Properties"] = {authored: None}
                        context = TypedContext(phase, State())
                        before = deepcopy(doc)
                        with self.assertRaises(AssetDocumentContractError) as raised:
                            operation(doc, context=context)
                        diagnostic = raised.exception.diagnostic
                        self.assertEqual((diagnostic.code, diagnostic.path, diagnostic.message),
                            ("BodyOwnedProperty", "/Properties/" + authored,
                             f"Property '{authored}' is owned by {owner} and cannot be authored through top-level Properties"))
                        self.assertEqual(context.calls, [])
                        self.assertEqual(doc, before)

    def test_top_property_ownership_keeps_generic_shape_omission_and_admission(self):
        profile = BehaviorTreeProfile()
        for properties in (None, [], False, "RootNode"):
            with self.subTest(properties=properties):
                doc = document(); doc["Body"] = {}; doc["Properties"] = properties
                context = TypedContext(RuntimePhase.VALIDATE, State())
                with self.assertRaises(AssetDocumentContractError) as raised:
                    profile.validate(doc, context=context)
                self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                                 ("Generic.InvalidPropertiesType", "/Properties"))
                self.assertEqual(context.calls, [])
        for properties in ({}, {"Description": "changed"}, None):
            doc = document(); doc["Body"] = {}
            if properties is None:
                del doc["Properties"]
            else:
                doc["Properties"] = properties
            before = deepcopy(doc)
            profile.validate(doc, context=TypedContext(RuntimePhase.VALIDATE, State()))
            self.assertEqual(doc, before)
        # Ownership alone uses the original FName ASCII folding. Ordinary
        # admitted fields retain their exact spelling, including near aliases.
        for authored in ("Unknown", "DESCRIPTION", "RootNode_0", "BlackboardA\u017fset", "Blac\u212aboardAsset"):
            doc = document(); doc["Body"] = {}; doc["Properties"] = {authored: None}
            with self.subTest(authored=authored), self.assertRaises(AssetDocumentContractError) as raised:
                profile.validate(doc, context=TypedContext(RuntimePhase.VALIDATE, State()))
            self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                             ("Generic.UnknownProperty", "/Properties/" + authored))

    def test_omitted_dynamic_default_requests_its_type_before_binding_validation(self):
        payload = "/Script/Test.DefaultSelectorPayload"
        task_class = "/Script/Test.DefaultDynamicTask"
        doc = document()
        doc["Action"] = "Create"
        doc["Body"]["Tree"]["Root"] = {"Id": guid(1), "Class": "/Script/AIModule.BTComposite_Sequence",
            "Children": [{"Id": guid(2), "Class": task_class}]}

        class DefaultContext(TypedContext):
            def invoke(self, *, requirement, value_arguments, handle_arguments):
                if requirement.primitive_id == "ai.behavior_tree.node_class.inspect.v2":
                    self.calls.append((requirement.primitive_id, requirement.version,
                                       deepcopy(value_arguments), handle_arguments))
                    defaults, schema, structs = {}, {}, {}
                    if value_arguments["node_class_path"] == task_class:
                        defaults = {"DynamicValue": {"Struct": payload,
                            "Properties": {"Selector": {"Key": "None"}}}}
                        schema = {"DynamicValue": {"kind": "instanced_struct"}}
                        if payload in value_arguments["struct_type_paths"]:
                            structs[payload] = {"Selector": {"kind": "selector"}}
                    return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED,
                        {"node_class_path": value_arguments["node_class_path"],
                         "authored_defaults": defaults, "property_schema": schema, "struct_schemas": structs}, (), (), ())
                if requirement.primitive_id == "ai.behavior_tree.binding.inspect.v1":
                    self.calls.append((requirement.primitive_id, requirement.version,
                                       deepcopy(value_arguments), handle_arguments))
                    return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED,
                        {"evidence": [{"none_allowed": True}]}, (), (), ())
                return super().invoke(requirement=requirement, value_arguments=value_arguments,
                                      handle_arguments=handle_arguments)

        context = DefaultContext(RuntimePhase.VALIDATE, State(exists=False))
        BehaviorTreeProfile().validate(doc, context=context)
        inspections = [values for name, _, values, _ in context.calls
                       if name == "ai.behavior_tree.node_class.inspect.v2" and values["node_class_path"] == task_class]
        self.assertEqual([row["struct_type_paths"] for row in inspections], [[], [payload]])
        binding = next(values for name, _, values, _ in context.calls if name == "ai.behavior_tree.binding.inspect.v1")
        self.assertEqual(binding["queries"][0]["locator"], [{"kind": "field", "name": "DynamicValue"},
            {"kind": "instanced_struct", "struct_path": payload}, {"kind": "field", "name": "Selector"}])
        self.assertEqual(binding["queries"][0]["selected_key"], "None")

    def typed_value_document(self, properties):
        doc = document()
        doc["Body"]["Tree"] = {"GraphGuid": guid(100), "Root": {"Id": guid(1),
            "Class": "/Script/AIModule.BTComposite_Sequence", "Children": [{"Id": guid(2),
            "Class": "/Script/AIModule.BTTask_Wait", "Properties": properties}]}}
        return doc

    def typed_value_context(self, phase):
        context = TypedContext(phase, State())
        def inspected(class_path):
            if class_path == "/Script/AIModule.BTComposite_Sequence":
                return {"node_class_path": class_path, "authored_defaults": {}, "property_schema": {}, "node_family": "composite"}
            return {"node_class_path": class_path, "node_family": "task",
                "authored_defaults": {"bIgnoreRestartSelf": False, "Count": 0, "NodeName": "", "Label": "",
                                      "ActorClass": None, "ReferencedTree": None, "ClassObject": None},
                "property_schema": {
                    "bIgnoreRestartSelf": {"kind": "value", "value_type": "bool"},
                    "Count": {"kind": "value", "value_type": "integer", "integer_bits": 16, "integer_signed": True},
                    "NodeName": {"kind": "value", "value_type": "string"}, "Label": {"kind": "value", "value_type": "text"},
                    "ActorClass": {"kind": "value", "value_type": "reference", "reference_kind": "ClassRef",
                                   "declared_class_path": "/Script/Engine.Actor"},
                    "ReferencedTree": {"kind": "value", "value_type": "reference", "reference_kind": "AssetRef",
                                       "declared_class_path": CLASS},
                    "ClassObject": {"kind": "value", "value_type": "reference", "reference_kind": "ClassRef",
                                    "reference_constraint": "object_class", "declared_class_path": "/Script/CoreUObject.Class"}}}
        context.class_inspector = inspected
        return context

    def test_root_role_error_precedes_properties_from_the_replaced_class(self):
        for phase in (RuntimePhase.VALIDATE, RuntimePhase.PREFLIGHT):
            with self.subTest(phase=phase):
                context = self.typed_value_context(phase)
                doc = self.typed_value_document({"Count": 1})
                root = doc["Body"]["Tree"]["Root"]
                root["Class"] = "/Script/AIModule.BTTask_Wait"
                root["Properties"] = {"CompositeOnlyProperty": True}
                before = deepcopy(context.state.tree)
                operation = BehaviorTreeProfile().validate if phase is RuntimePhase.VALIDATE else BehaviorTreeProfile().preflight
                with self.assertRaises(AssetDocumentContractError) as raised:
                    operation(doc, context=context)
                self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                                 ("InvalidBehaviorTreeNodeClass", "/Body/Tree/Root/Class"))
                self.assertEqual(context.state.tree, before)

    def test_public_validate_and_preflight_reject_typed_scalar_errors_at_node_fields(self):
        profile = BehaviorTreeProfile()
        for phase in (RuntimePhase.VALIDATE, RuntimePhase.PREFLIGHT):
            operation = profile.validate if phase is RuntimePhase.VALIDATE else profile.preflight
            for field, value in (("bIgnoreRestartSelf", "not-a-boolean"), ("Count", 32768), ("Count", True),
                                 ("Count", 1.25), ("NodeName", 9), ("Label", {"Text": "wrong wrapper"})):
                with self.subTest(phase=phase, field=field, value=value):
                    context = self.typed_value_context(phase)
                    before = deepcopy(context.state.tree)
                    with self.assertRaises(AssetDocumentContractError) as raised:
                        operation(self.typed_value_document({field: value}), context=context)
                    self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                                     ("InvalidPropertyValue", "/Body/Tree/Root/Children/0/Properties/" + field))
                    self.assertEqual(context.state.tree, before)
            for boundary in (-32768, 32767):
                operation(self.typed_value_document({"bIgnoreRestartSelf": True, "Count": boundary,
                    "NodeName": "等待", "Label": "文本"}), context=self.typed_value_context(phase))
            doc = self.typed_value_document({"Count": 1.0})
            operation(doc, context=self.typed_value_context(phase))
            self.assertIs(type(doc["Body"]["Tree"]["Root"]["Children"][0]["Properties"]["Count"]), float)
        integer = self.typed_value_document({"Count": 1})
        self.assertEqual(profile.diff(doc, integer, context=self.typed_value_context(RuntimePhase.VERIFY)), ())

    def test_public_soft_references_allow_future_paths_without_hard_loading(self):
        profile = BehaviorTreeProfile()
        asset_path, class_path = "/Game/Future/Tree.Tree", "/Game/Future/BP_Actor.BP_Actor_C"
        def context_for(phase):
            context = self.typed_value_context(phase)
            inspected = context.class_inspector
            def soft_evidence(name):
                data = inspected(name)
                for field in ("ReferencedTree", "ActorClass"):
                    if field in data["property_schema"]:
                        data["property_schema"][field]["reference_soft"] = True
                return data
            context.class_inspector = soft_evidence
            return context
        for phase in (RuntimePhase.VALIDATE, RuntimePhase.PREFLIGHT):
            operation = profile.validate if phase is RuntimePhase.VALIDATE else profile.preflight
            for properties in (
                {"ReferencedTree": asset_path, "ActorClass": {"Kind": "ClassRef", "Path": class_path}},
                {"ReferencedTree": {"Kind": "AssetRef", "Path": asset_path}, "ActorClass": class_path},
            ):
                context = context_for(phase)
                doc = self.typed_value_document(properties)
                operation(doc, context=context)
                self.assertFalse(any(row[0] == "editor.reference.resolve_asset.v1" for row in context.calls))
                self.assertTrue(all(row[2]["class"] in ("/Script/AIModule.BTComposite_Sequence", "/Script/AIModule.BTTask_Wait")
                                    for row in context.calls if row[0] == "editor.class.resolve_canonical.v1"))
            for field, value, code, leaf in (
                ("ActorClass", {"Kind": "AssetRef", "Path": asset_path}, "InvalidPropertyValue", "Kind"),
                ("ReferencedTree", {"Kind": "AssetRef", "Path": 7}, "InvalidPropertyValue", "Path"),
                ("ReferencedTree", {"Kind": "AssetRef", "Path": asset_path, "Extra": True}, "UnknownField", "Extra"),
            ):
                with self.assertRaises(AssetDocumentContractError) as raised:
                    operation(self.typed_value_document({field: value}), context=context_for(phase))
                self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                                 (code, "/Body/Tree/Root/Children/0/Properties/" + field + "/" + leaf))
        verify = context_for(RuntimePhase.VERIFY)
        self.assertEqual(profile.diff(doc, deepcopy(doc), context=verify), ())
        self.assertFalse(any(row[0] in ("editor.class.resolve_canonical.v1", "editor.reference.resolve_asset.v1") for row in verify.calls))

    def test_public_reference_constraints_keep_field_diagnostics_and_verify_does_not_load(self):
        profile = BehaviorTreeProfile()
        base = "/Body/Tree/Root/Children/0/Properties/"
        for phase in (RuntimePhase.VALIDATE, RuntimePhase.PREFLIGHT):
            operation = profile.validate if phase is RuntimePhase.VALIDATE else profile.preflight
            for value, code, suffix in (
                ({"Kind": "ClassRef", "Path": "/Script/Engine.Actor"}, "InvalidPropertyValue", "Kind"),
                ({"Kind": "AssetRef", "Path": 7}, "InvalidPropertyValue", "Path"),
                ({"Kind": "AssetRef", "Path": "/Game/A.A", "Extra": True}, "UnknownField", "Extra"),
            ):
                with self.subTest(phase=phase, value=value):
                    with self.assertRaises(AssetDocumentContractError) as raised:
                        operation(self.typed_value_document({"ReferencedTree": value}), context=self.typed_value_context(phase))
                    self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                                     (code, base + "ReferencedTree/" + suffix))
            for field, path, primitive, expected, code in (
                ("ActorClass", CLASS, "editor.class.resolve_canonical.v1", "/Script/Engine.Actor", "ClassReference.ExpectedBaseMismatch"),
                ("ReferencedTree", "/Game/Tests/BB.BB", "editor.reference.resolve_asset.v1", CLASS, "ClassReference.AssetTypeMismatch"),
            ):
                with self.subTest(phase=phase, mismatch=field):
                    context = self.typed_value_context(phase)
                    context.reference_failures[(primitive, path, expected)] = RuntimeDiagnostic("/ValueArguments/path", code, "Explicit native type mismatch")
                    with self.assertRaises(AssetDocumentContractError) as raised:
                        operation(self.typed_value_document({field: path}), context=context)
                    self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                                     ("InvalidPropertyValue", base + field))
            doc = self.typed_value_document({"ActorClass": "/Script/Engine.Pawn", "ReferencedTree": "/Game/Tests/Tree.Tree",
                                            "ClassObject": "/Script/Engine.Actor"})
            context = self.typed_value_context(phase)
            operation(doc, context=context)
            asset_lookups = [row[2] for row in context.calls if row[0] == "editor.reference.resolve_asset.v1"]
            self.assertIn({"path": "/Script/Engine.Actor", "expected_class": "/Script/CoreUObject.Class"}, asset_lookups)
            self.assertIn({"path": "/Game/Tests/Tree.Tree", "expected_class": CLASS}, asset_lookups)
        verify = self.typed_value_context(RuntimePhase.VERIFY)
        self.assertEqual(profile.diff(doc, deepcopy(doc), context=verify), ())
        self.assertFalse(any(row[0] in ("editor.class.resolve_canonical.v1", "editor.reference.resolve_asset.v1") for row in verify.calls))

    def test_semantic_diff_after_apply_completes_defaults_and_retained_comments(self):
        profile = BehaviorTreeProfile()
        state = State()
        doc = document()
        doc["Action"] = "Create"
        del doc["Body"]["Tree"]["GraphGuid"]
        doc["Body"]["Tree"]["Comments"] = [{"Id": guid(9)}]
        profile.apply(doc, context=TypedContext(RuntimePhase.APPLY, state))
        context = TypedContext(RuntimePhase.VERIFY, state)
        actual = profile.extract(target=TARGET, context=context, include_defaults=True)
        before = deepcopy((doc, actual))
        call_count = len(context.calls)
        self.assertEqual(profile.diff(doc, actual, context=context), ())
        self.assertEqual((doc, actual), before)
        self.assertTrue(all(name in {"ai.behavior_tree.node_class.inspect.v2",
                            "editor.asset.resolve_existing.v1", "ai.behavior_tree.authored_graph.readback.v2",
                            "ai.behavior_tree.blackboard.readback.v1", "ai.behavior_tree.binding.inspect.v1",
                            "editor.property.enumerate_admitted.v1"}
                            for name, *_ in context.calls[call_count:]))

    def test_semantic_diff_omission_preserves_regions_and_compares_declared_properties(self):
        profile = BehaviorTreeProfile()
        state = State(); state.blackboard = "/Game/Tests/BB.BB"
        actual = profile.extract(target=TARGET, context=TypedContext(RuntimePhase.VERIFY, state), include_defaults=True)
        desired = document(); desired["Body"] = {}; desired["Action"] = "Create"
        self.assertEqual(profile.diff(desired, actual), ())
        # An omitted existing Tree is reported as skipped, never as a no-op proof.
        self.assertEqual([row["path"] for row in profile.diff_payload(desired, actual)["skipped"]], ["/Body/Tree"])
        desired["Properties"] = {"Description": "changed"}
        self.assertEqual([item.path for item in profile.diff(desired, actual)], ["/Properties/Description"])
        desired["Properties"] = {}; desired["Body"] = {"BlackboardAsset": "/Game/Tests/BB"}
        self.assertEqual(profile.diff(desired, actual), ())
        desired["Body"]["BlackboardAsset"] = None
        self.assertEqual([item.path for item in profile.diff(desired, actual)], ["/Body/BlackboardAsset"])

    def test_public_diff_identity_paths_values_and_unchanged_receipts(self):
        profile = BehaviorTreeProfile(); doc = document()
        doc["Body"]["BlackboardAsset"] = "/Game/Tests/BB"
        doc["Body"]["Tree"]["Comments"][0]["NodeDetails"] = "retained details"
        state = State(doc); state.blackboard = "/Game/Tests/BB.BB"
        context = TypedContext(RuntimePhase.VERIFY, state)
        actual = profile.extract(target=TARGET, context=context, include_defaults=True)
        desired = deepcopy(actual); root = desired["Body"]["Tree"]["Root"]
        root["Properties"]["Count"] = 9
        root["Editor"]["Position"]["X"] = -50
        root["Decorators"][0]["Properties"]["Mode"] = "Second"
        root["Decorators"][1]["BoundGraph"]["Nodes"][1]["Properties"]["Count"] = 12
        desired["Body"]["Tree"]["Comments"] = [{"Id": guid(9), "Text": "changed"}]
        before = deepcopy((desired, actual))
        changes = {item.path: item for item in profile.diff(desired, actual, context=context)}
        # Diff paths are JSON pointers into the document (not node GUIDs).
        node_path = "/Body/Tree/Root"
        decorator_path = "/Body/Tree/Root/Decorators/0"
        bound_path = "/Body/Tree/Root/Decorators/1/BoundGraph/Nodes/1"
        self.assertEqual(root["Decorators"][1]["BoundGraph"]["Nodes"][1]["Id"], guid(7))
        self.assertEqual(set(changes), {node_path + "/Properties/Count", node_path + "/Editor/Position/X",
            decorator_path + "/Properties/Mode", bound_path + "/Properties/Count",
            "/Body/Tree/Comments/0/Text"})
        self.assertEqual((changes[node_path + "/Properties/Count"].before,
                          changes[node_path + "/Properties/Count"].after), (8, 9))
        self.assertEqual((changes[decorator_path + "/Properties/Mode"].before,
                          changes[decorator_path + "/Properties/Mode"].after), ("First", "Second"))
        call_count = len(context.calls)
        payload = profile.diff_payload(desired, actual, context=context, validated=True)
        unchanged = {row["path"]: row for row in payload["unchanged"]}
        self.assertEqual(unchanged["/Body/BlackboardAsset"]["current"], {"Kind": "AssetRef", "Path": state.blackboard})
        self.assertEqual(unchanged["/Body/Tree/GraphGuid"]["desired"], guid(100))
        changed = {row["path"]: row for row in payload["changed"]}
        self.assertEqual((changed[node_path + "/Properties/Count"]["current"],
                          changed[node_path + "/Properties/Count"]["desired"]), (8, 9))
        self.assertEqual(set(changed), set(changes))
        self.assertTrue(all(row["status"] == "changed" for row in changed.values()))
        self.assertTrue(all(not payload[name] for name in ("added", "removed", "skipped", "failed")))
        self.assertTrue(all(name == "ai.behavior_tree.node_class.inspect.v2" for name, *_ in context.calls[call_count:]))
        self.assertEqual((desired, actual), before)

    def test_public_diff_keeps_order_paths_and_ignores_identity_collection_order(self):
        profile = BehaviorTreeProfile(); doc = document(); root = doc["Body"]["Tree"]["Root"]
        other = deepcopy(root["Children"][0]); other["Id"] = guid(20)
        root["Children"].append(other)
        root["Children"][0]["Decorators"] = [dict(deepcopy(root["Decorators"][0]), Id=guid(21)),
                                             dict(deepcopy(root["Decorators"][0]), Id=guid(22))]
        bound = root["Decorators"][1]["BoundGraph"]
        bound["Nodes"].append({"Id": guid(23), "Kind": "Not"})
        bound["Links"] = [{"From": guid(7), "To": guid(23), "ToInput": 0},
                          {"From": guid(23), "To": guid(6), "ToInput": 0}]
        doc["Body"]["Tree"]["Comments"].append({"Id": guid(24), "Text": "another"})
        state = State(doc); context = TypedContext(RuntimePhase.VERIFY, state)
        actual = profile.extract(target=TARGET, context=context, include_defaults=True)
        reordered = deepcopy(actual); tree = reordered["Body"]["Tree"]
        tree["Comments"].reverse()
        bound = tree["Root"]["Decorators"][1]["BoundGraph"]
        bound["Nodes"].reverse(); bound["Links"].reverse()
        self.assertEqual(profile.diff(reordered, actual, context=context), ())
        desired = deepcopy(doc); root = desired["Body"]["Tree"]["Root"]
        root["Children"].reverse(); root["Decorators"].reverse()
        root["Children"][1]["Decorators"].reverse()
        changes = {item.path: item for item in profile.diff(desired, actual, context=context)}
        root_path = "/Body/Tree/Root"
        child_path = "/Body/Tree/Root/Children/1"  # guid(2), second in the authored (reversed) order
        self.assertEqual((changes[root_path + "/Children"].before, changes[root_path + "/Children"].after),
                         ([guid(2), guid(20)], [guid(20), guid(2)]))
        self.assertEqual(changes[root_path + "/Decorators"].after, [guid(5), guid(4)])
        self.assertEqual(changes[child_path + "/Decorators"].after, [guid(22), guid(21)])

    def test_public_diff_absent_graph_uses_graph_guid_path(self):
        profile = BehaviorTreeProfile(); desired = document()
        desired["Body"]["Tree"] = {"GraphGuid": guid(100), "Root": {}, "Comments": []}
        actual = deepcopy(desired); actual["Body"]["Tree"].pop("GraphGuid")
        context = TypedContext(RuntimePhase.VERIFY, State(exists=False))
        changes = profile.diff(desired, actual, context=context)
        self.assertEqual([item.path for item in changes], ["/Body/Tree/GraphGuid"])
        self.assertEqual((changes[0].before, changes[0].after), (None, guid(100)))
        self.assertEqual((changes[0].before_present, changes[0].after_present), (False, True))
        payload = profile.diff_payload(desired, actual, context=context)
        self.assertEqual(payload["changed"], [{"path": "/Body/Tree/GraphGuid", "status": "changed",
                                              "current": None, "desired": guid(100)}])
        self.assertEqual(profile.diff(desired, deepcopy(desired), context=context), ())

    def test_semantic_diff_keeps_order_links_and_nondefault_properties_visible(self):
        profile = BehaviorTreeProfile(); doc = document()
        other = deepcopy(doc["Body"]["Tree"]["Root"]["Children"][0]); other["Id"] = guid(20)
        doc["Body"]["Tree"]["Root"]["Children"].append(other)
        state = State(doc); context = TypedContext(RuntimePhase.VERIFY, state)
        actual = profile.extract(target=TARGET, context=context, include_defaults=True)
        changed = deepcopy(doc); changed["Body"]["Tree"]["Root"]["Children"].reverse()
        self.assertTrue(profile.diff(changed, actual, context=context))
        changed = deepcopy(doc); changed["Body"]["Tree"]["Root"]["Properties"]["Count"] = 9
        self.assertTrue(profile.diff(changed, actual, context=context))
        changed = deepcopy(doc); bound = changed["Body"]["Tree"]["Root"]["Decorators"][1]["BoundGraph"]
        bound["Nodes"].append({"Id": guid(21), "Kind": "Not"})
        bound["Links"] = [{"From": guid(7), "To": guid(21), "ToInput": 0},
                          {"From": guid(21), "To": guid(6), "ToInput": 0}]
        self.assertTrue(profile.diff(changed, actual, context=context))
        with self.assertRaises(AssetDocumentContractError):
            profile.diff(doc, actual)
        changed = deepcopy(doc); changed["Body"]["Tree"]["Root"]["Properties"]["Unknown"] = 1
        with self.assertRaises(AssetDocumentContractError):
            profile.diff(changed, actual, context=context)
        empty = document(); empty["Body"]["Tree"] = {"Root": {}}
        absent = deepcopy(actual); absent["Body"]["Tree"] = {"Root": {}, "Comments": []}
        self.assertEqual([item.path for item in profile.diff(empty, absent, context=context)],
                         ["/Body/Tree/GraphGuid"])

    def test_native_field_order_preserves_punctuation_and_non_ascii_keys(self):
        from asset_document.profiles.behavior_tree_execution import _canonical_native_value
        value = {"Map": {"A": 1, "_": 2, "ß": 3, "SS": 4, "Ä": 5, "ä": 6,
                         "\U00010000": 7, "\ue000": 8}}
        actual = _canonical_native_value(value)["Map"]
        self.assertEqual(list(actual), ["_", "A", "SS", "Ä", "ß", "ä", "\U00010000", "\ue000"])
        self.assertEqual(actual, value["Map"])

    def test_overlay_expected_rejects_later_unrequested_tree_drift(self):
        doc = document(); state = State(doc)
        del doc["Body"]["Tree"]
        values = []
        profile = BehaviorTreeProfile()
        profile.apply(doc, context=TypedContext(RuntimePhase.APPLY, state), expected_sink=values.append)
        profile.verify(doc, context=TypedContext(RuntimePhase.VERIFY, state), expected=values[0])
        state.tree["root"]["authored_properties"]["Count"] += 1
        with self.assertRaises(AssetDocumentContractError):
            profile.verify(doc, context=TypedContext(RuntimePhase.VERIFY, state), expected=values[0])

    def test_partial_comment_expected_is_from_before_mutation(self):
        doc = document(); state = State(doc)
        del doc["Body"]["Tree"]["Comments"][0]["Text"]
        values = []
        profile = BehaviorTreeProfile()
        profile.apply(doc, context=TypedContext(RuntimePhase.APPLY, state), expected_sink=values.append)
        self.assertEqual(values[0]["tree"]["comments"][0]["text"], "完整树")
        state.tree["comments"][0]["text"] = "unexpected drift"
        with self.assertRaises(AssetDocumentContractError):
            profile.verify(doc, context=TypedContext(RuntimePhase.VERIFY, state), expected=values[0])

    def test_expected_relay_is_pure_copied_request_bound_and_cleared(self):
        from asset_document import runtime
        request = SimpleNamespace(request_id="request-one", expected_runtime_revision=7,
            requested_asset_class_path=CLASS, target_asset_path=TARGET + ".BT_Production",
            operation=RuntimeOperation.APPLY, canonical_document=document())
        runtime._clear_behavior_tree_expected()
        try:
            expected = {"tree": projection(document()), "blackboard": None, "properties": {}}
            runtime._store_behavior_tree_expected(request, expected)
            expected["tree"]["root"] = None
            observed = runtime._get_behavior_tree_expected(request)
            self.assertIsNotNone(observed["tree"]["root"])
            observed["tree"]["root"] = None
            self.assertIsNotNone(runtime._get_behavior_tree_expected(request)["tree"]["root"])
            request.canonical_document["Body"]["BlackboardAsset"] = "/Game/Other.Other"
            with self.assertRaises(AssetDocumentContractError):
                runtime._get_behavior_tree_expected(request)
            self.assertIsNone(runtime._behavior_tree_expected_slot)
        finally:
            runtime._clear_behavior_tree_expected()

    def test_terminal_cleanup_matches_request_and_revision_and_is_idempotent(self):
        from asset_document import runtime
        request = SimpleNamespace(request_id="request-one", expected_runtime_revision=7,
            requested_asset_class_path=CLASS, target_asset_path=TARGET + ".BT_Production",
            operation=RuntimeOperation.APPLY, canonical_document=document())
        runtime._clear_behavior_tree_expected()
        try:
            runtime._store_behavior_tree_expected(request, {"tree": projection(document())})
            cleanup = runtime.dispatch._clear_behavior_tree_expected_for_terminal
            cleanup("other-request", 7)
            cleanup("request-one", 6)
            self.assertIsNotNone(runtime._behavior_tree_expected_slot)
            cleanup("request-one", 7)
            self.assertIsNone(runtime._behavior_tree_expected_slot)
            cleanup("request-one", 7)
            self.assertIsNone(runtime._behavior_tree_expected_slot)
        finally:
            runtime._clear_behavior_tree_expected()

    def test_full_legacy_tree_lowering_and_apply_order(self):
        doc = document(); state = State(doc); context = TypedContext(RuntimePhase.APPLY, state)
        BehaviorTreeProfile().apply(doc, context=context)
        calls = [row[0] for row in context.calls]
        ordered = ["ai.behavior_tree.graph.author_scratch.v1", "ai.behavior_tree.graph.install.v1", "ai.behavior_tree.blackboard.set.v1"]
        self.assertEqual([name for name in calls if name in ordered], ordered)
        self.assertEqual(state.tree, projection(doc))
        self.assertEqual(state.tree["root"]["authored_properties"], {"Mode": 4, "Count": 8, "Duration": 0.0})
        self.assertIsNone(state.blackboard)
        self.assertNotIn("ai.behavior_tree.runtime_mirror.rebuild.v1", calls)

    def test_each_phase_owns_fresh_handles_and_repair_uses_native_inventory(self):
        profile = BehaviorTreeProfile(); doc = document(); state = State(doc)
        apply = TypedContext(RuntimePhase.APPLY, state); profile.apply(doc, context=apply)
        repair = TypedContext(RuntimePhase.REPAIR, state); profile.repair(doc, context=repair)
        verify = TypedContext(RuntimePhase.VERIFY, state); profile.verify(doc, context=verify)
        calls = {row[0]: row for row in repair.calls}
        batch = calls["ai.behavior_tree.selector.resolve_batch.v2"]
        refresh = calls["ai.behavior_tree.value_or_key.refresh.v3"]
        self.assertEqual(batch[1], 2); self.assertEqual(refresh[1], 3)
        self.assertEqual(batch[2]["selectors"], state.selectors)
        self.assertIsNone(batch[2]["blackboard_path"])
        self.assertEqual(refresh[2]["completion_registration"], "installed_replacement")
        self.assertEqual(refresh[2]["expected_authored_inventory_digest"], authored_digest_v2(TARGET + ".BT_Production", state.tree))
        self.assertTrue(all(access == "read_only" for _, access in verify.handoffs))
        self.assertEqual([row[0] for row in repair.calls].count("ai.behavior_tree.selector.resolve_batch.v2"), 1)
        ordered = [row[0] for row in repair.calls]
        self.assertLess(ordered.index("ai.behavior_tree.selector.inventory.v1"), ordered.index("ai.behavior_tree.selector.resolve_batch.v2"))
        self.assertLess(ordered.index("ai.behavior_tree.selector.resolve_batch.v2"), ordered.index("ai.behavior_tree.authored_graph.readback.v2"))
        self.assertLess(ordered.index("ai.behavior_tree.authored_graph.readback.v2"), ordered.index("ai.behavior_tree.value_or_key.refresh.v3"))

    def test_preflight_observes_classes_without_mutating_or_creating(self):
        context = TypedContext(RuntimePhase.PREFLIGHT, State())
        self.assertEqual(BehaviorTreeProfile().preflight(document(), context=context), ())
        names = [row[0] for row in context.calls]
        self.assertIn("ai.behavior_tree.node_class.inspect.v2", names)
        self.assertIn("ai.behavior_tree.authored_graph.readback.v2", names)
        self.assertFalse(any(name.endswith((".create.v1", ".install.v1", ".set.v1", ".author_scratch.v1")) for name in names))

    def test_unknown_blackboard_names_the_authored_asset(self):
        for phase in (RuntimePhase.VALIDATE, RuntimePhase.PREFLIGHT):
            with self.subTest(phase=phase):
                profile = BehaviorTreeProfile()
                operation = profile.validate if phase is RuntimePhase.VALIDATE else profile.preflight
                context = TypedContext(phase, State())
                context.reference_failures[("editor.reference.resolve_asset.v1", "/Game/AI/BB_Missing.BB_Missing",
                                            "/Script/AIModule.BlackboardData")] = RuntimeDiagnostic(
                    "/ValueArguments/path", "ClassReference.NonCanonicalAsset",
                    "The asset path did not resolve to that exact canonical identity.")
                doc = document(); doc["Body"]["BlackboardAsset"] = "/Game/AI/BB_Missing"
                with self.assertRaises(AssetDocumentContractError) as raised:
                    operation(doc, context=context)
                diagnostic = raised.exception.diagnostic
                self.assertEqual((diagnostic.code, diagnostic.path),
                                 ("BehaviorTree.BlackboardNotFound", "/Body/BlackboardAsset"))
                self.assertIn("/Game/AI/BB_Missing", diagnostic.message)
                self.assertNotIn("editor.reference", diagnostic.message)
                self.assertNotIn("/ValueArguments", diagnostic.message)

    def test_zero_selector_null_blackboard_still_runs_both_completion_operations(self):
        state = State(); state.selectors = []
        context = TypedContext(RuntimePhase.REPAIR, state)
        BehaviorTreeProfile().repair(document(), context=context)
        batch = next(row for row in context.calls if row[0] == "ai.behavior_tree.selector.resolve_batch.v2")
        refresh = next(row for row in context.calls if row[0] == "ai.behavior_tree.value_or_key.refresh.v3")
        self.assertEqual(batch[2]["selectors"], [])
        self.assertIsNone(refresh[2]["expected_blackboard_path"])

    def test_overlay_preserves_retained_none_without_resolving_it(self):
        state = State()
        state.selectors = [
            {"stable_node_id": guid(2), "property_path": "First", "selected_key_name": "Target"},
            {"stable_node_id": guid(3), "property_path": "Unbound", "selected_key_name": "None"},
            {"stable_node_id": guid(4), "property_path": "Last", "selected_key_name": "Distance"},
        ]
        for body in ({}, {"BlackboardAsset": None}, document()["Body"]):
            with self.subTest(full_tree="Tree" in body):
                doc = document(); doc["Body"] = deepcopy(body)
                before = deepcopy(state.selectors)
                context = TypedContext(RuntimePhase.REPAIR, state)
                BehaviorTreeProfile().repair(doc, context=context)
                batch = next(row for row in context.calls if row[0] == "ai.behavior_tree.selector.resolve_batch.v2")
                expected = before if "Tree" in body else [before[0], before[2]]
                self.assertEqual(batch[2]["selectors"], expected)
                self.assertEqual(state.selectors, before)

    def test_factory_handle_is_reresolved_before_ai_use(self):
        doc = document(); doc["Action"] = "Create"; state = State(exists=False)
        context = TypedContext(RuntimePhase.APPLY, state)
        BehaviorTreeProfile().apply(doc, context=context)
        calls = [row[0] for row in context.calls]
        created = calls.index("editor.asset.create_by_factory.v1")
        self.assertEqual(calls[created + 1], "editor.asset.resolve_existing.v1")
        self.assertFalse(any(handle.name == "factory-lifecycle" for handle, _ in context.handoffs))

    def test_blackboard_omission_does_not_set_and_overlay_does_not_install(self):
        doc = document(); del doc["Body"]["BlackboardAsset"]
        context = TypedContext(RuntimePhase.APPLY, State(doc)); BehaviorTreeProfile().apply(doc, context=context)
        self.assertNotIn("ai.behavior_tree.blackboard.set.v1", [row[0] for row in context.calls])
        doc["Body"] = {"BlackboardAsset": None}; context = TypedContext(RuntimePhase.APPLY, State())
        BehaviorTreeProfile().apply(doc, context=context)
        self.assertNotIn("ai.behavior_tree.graph.create.v1", [row[0] for row in context.calls])
        repair = TypedContext(RuntimePhase.REPAIR, context.state); BehaviorTreeProfile().repair(doc, context=repair)
        refreshed = next(row for row in repair.calls if row[0] == "ai.behavior_tree.value_or_key.refresh.v3")
        self.assertEqual(refreshed[2]["completion_registration"], "current_graph")

    def test_verify_rejects_authored_default_topology_and_blackboard_drift(self):
        for drift in ("authored", "default", "topology", "blackboard", "top_property", "comment", "node_id", "bound_inputs"):
            with self.subTest(drift=drift):
                doc = document(); doc["Properties"] = {"Description": "same"}; state = State(doc)
                if drift == "authored": state.tree["root"]["authored_properties"]["Mode"] = 1
                elif drift == "default": state.tree["root"]["authored_properties"]["Count"] = 99
                elif drift == "topology": state.tree["root"]["children"] = []
                elif drift == "blackboard": state.blackboard = "/Game/Other.Other"
                elif drift == "comment": state.tree["comments"][0]["text"] = "wrong"
                elif drift == "node_id": state.tree["root"]["children"][0]["stable_node_id"] = guid(123)
                elif drift == "bound_inputs": state.tree["root"]["decorators"][1]["bound_graph"]["nodes"][0]["input_count"] = 2
                else: state.top_properties["Description"] = "changed"
                with self.assertRaises(AssetDocumentContractError) as raised:
                    BehaviorTreeProfile().verify(doc, context=TypedContext(RuntimePhase.VERIFY, state))
                self.assertEqual(raised.exception.diagnostic.code, "BehaviorTree.VerificationMismatch")

    def test_verify_accepts_omitted_top_properties_without_synthesizing_them(self):
        doc = document(); del doc["Properties"]
        result = BehaviorTreeProfile().verify(doc, context=TypedContext(RuntimePhase.VERIFY, State(doc)))
        self.assertNotIn("Properties", result)

    def test_extract_recovers_enum_names_and_omits_node_defaults_unless_asked(self):
        state = State(); context = TypedContext(RuntimePhase.VERIFY, state)
        result = BehaviorTreeProfile().extract(target=TARGET, context=context)
        # diff_only (the default): node properties equal to the class default are omitted
        # (an omitted node property is reset to its default, so re-applying changes nothing).
        self.assertEqual(result["Body"]["Tree"]["Root"]["Properties"], {"Mode": "Second"})
        full = BehaviorTreeProfile().extract(target=TARGET, context=context, include_defaults=True)
        self.assertEqual(full["Body"]["Tree"]["Root"]["Properties"], {"Mode": "Second", "Count": 8, "Duration": 0.0})
        self.assertIsNone(result["Body"]["BlackboardAsset"])
        self.assertEqual(result["Properties"], {})
        self.assertTrue(all(access == "read_only" for _, access in context.handoffs))

    def test_provider_failure_ends_apply_before_install_or_blackboard(self):
        context = TypedContext(RuntimePhase.APPLY, State()); context.fail_id = "ai.behavior_tree.graph.author_scratch.v1"
        with self.assertRaises(AssetDocumentContractError) as raised:
            BehaviorTreeProfile().apply(document(), context=context)
        self.assertEqual(raised.exception.diagnostic.code, "Test.ProviderRejected")
        self.assertEqual(context.calls[-1][0], context.fail_id)


if __name__ == "__main__":
    unittest.main()
