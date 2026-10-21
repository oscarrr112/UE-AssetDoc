import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.diff.model import DiffKind
from asset_document.dto import RuntimePhase
import asset_document.profiles.generic as generic_module
from asset_document.profiles.generic import (
    GENERIC_PROFILE_ID,
    GenericProfileCandidate,
)
from asset_document.regions.fragment_semantics import FRAGMENT_KINDS
from asset_document.regions.primitive_catalog import (
    GENERIC_REQUIRED_PRIMITIVES,
    GENERIC_ROUTE_ID,
    GENERIC_RUNTIME_ENTRY_MODULE,
)
from asset_document.tests.fixtures.generic_cases import (
    ENUMERATE_BATCH_ID,
    ENUMERATE_ID,
    EXACT_CLASS,
    GENERATED_CLASS,
    PREFLIGHT_ID,
    READ_ID,
    TARGET,
    WRITE_ID,
    WRITE_BATCH_ID,
    MockPrimitiveContext,
    changed_record,
    empty_body_document,
    missing_body_document,
    non_empty_body_document,
    property_batch_enumeration_outcome,
    property_enumeration_outcome,
    rich_document,
    success_outcome,
)


def assert_error(test, code: str, path: str, callback) -> AssetDocumentContractError:
    with test.assertRaises(AssetDocumentContractError) as caught:
        callback()
    actual = caught.exception.diagnostic
    test.assertEqual((actual.code, actual.path), (code, path))
    return caught.exception


def single_property_catalog(context: MockPrimitiveContext):
    rows = provider_property_rows(
        [{"name": "Value", "readable": True, "writable": True, "default": 0}]
    )
    outcome, handles = property_enumeration_outcome(context, rows)
    context.queue_outcomes(ENUMERATE_ID, outcome)
    return handles


def provider_property_rows(rows):
    return [
        {
            **row,
            "declaring_class": EXACT_CLASS,
            "codec_id": "property.test.v1",
        }
        for row in rows
    ]


def operation_document() -> dict:
    document = missing_body_document()
    document["Properties"] = {"Value": 7}
    return document


def assert_profile_apply(test, profile) -> None:
    context = MockPrimitiveContext(RuntimePhase.APPLY)
    single_property_catalog(context)
    read_asset = context.issue_handle()
    mutable_asset = context.issue_handle()
    expected = (changed_record("Value"),)
    batch_outcome, _, property_batch = property_batch_enumeration_outcome(
        context,
        provider_property_rows(
            [{"name": "Value", "readable": True, "writable": True, "default": 0}]
        ),
    )
    context.queue_outcomes(ENUMERATE_BATCH_ID, batch_outcome)
    context.queue_outcomes(WRITE_BATCH_ID, success_outcome(changed_set=expected))
    result = profile.apply(
        operation_document(),
        read_asset,
        mutable_asset,
        context,
    )
    test.assertIs(result, expected)
    test.assertIs(context.calls[-2].handle_arguments[0].handle, read_asset)
    test.assertIs(context.calls[-1].handle_arguments[0].handle, mutable_asset)
    test.assertIs(context.calls[-1].handle_arguments[1].handle, property_batch)
    test.assertEqual(
        [call.requirement.primitive_id for call in context.calls],
        [ENUMERATE_ID, ENUMERATE_BATCH_ID, WRITE_BATCH_ID],
    )
    test.assertNotIn(WRITE_ID, [call.requirement.primitive_id for call in context.calls])


class GenericProfileTests(unittest.TestCase):
    def setUp(self) -> None:
        self.profile = GenericProfileCandidate(EXACT_CLASS)

    def test_missing_body(self) -> None:
        document = missing_body_document()
        self.profile.validate(document)
        canonical = self.profile.canonicalize(document)
        self.assertNotIn("Body", canonical)

    def test_empty_body(self) -> None:
        document = empty_body_document()
        self.profile.validate(document)
        canonical = self.profile.canonicalize(document)
        self.assertNotIn("Body", canonical)
        self.assertEqual(canonical, self.profile.canonicalize(missing_body_document()))

    def test_non_empty_body_fails_closed(self) -> None:
        assert_error(
            self,
            "GenericBodyNotEmpty",
            "/Body",
            lambda: self.profile.validate(non_empty_body_document()),
        )

    def test_template_schema_extract_diff(self) -> None:
        template = self.profile.create_template(TARGET)
        self.assertEqual(
            template,
            {
                "SchemaVersion": 1,
                "Target": TARGET,
                "Class": EXACT_CLASS,
                "Action": "CreateOrUpdate",
                "Definitions": {},
                "Properties": {},
            },
        )
        schema = self.profile.schema()
        self.assertEqual((schema["SchemaVersion"], schema["Class"]), (1, EXACT_CLASS))
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        single_property_catalog(context)
        asset = context.issue_handle()
        context.queue_outcomes(READ_ID, success_outcome(value={"value": 3}))
        extracted = self.profile.extract(target=TARGET, asset_handle=asset, context=context)
        self.assertEqual(
            extracted,
            {
                "SchemaVersion": 1,
                "AssetType": "GenericAsset",
                "Target": TARGET,
                "Class": EXACT_CLASS,
                "Action": "CreateOrUpdate",
                "Properties": {"Value": 3},
            },
        )
        authored = self.profile.create_template(TARGET)
        authored["Properties"] = {"Value": 4}
        self.assertEqual([(item.path, item.kind) for item in self.profile.diff(authored, extracted)], [("/Properties/Value", DiffKind.REPLACE)])

    def test_constructor_requires_canonical_exact_class_syntax(self) -> None:
        for value in ("", "DataAsset", "/Game/Generic/BP_Generic", "/Game/Generic/BP_Generic.BP_Generic"):
            with self.subTest(value=value):
                assert_error(
                    self,
                    "Generic.NonCanonicalClassPath",
                    "/Class",
                    lambda value=value: GenericProfileCandidate(value),
                )
        generated = GenericProfileCandidate(GENERATED_CLASS)
        self.assertEqual(generated.inspect_profile()["Class"], GENERATED_CLASS)
        self.assertFalse(hasattr(generated, "exact_asset_class_path"))

    def test_validate_rejects_header_and_optional_field_contract_violations(self) -> None:
        cases = (
            ([], "Generic.InvalidDocumentType", "/"),
            ({**missing_body_document(), "SchemaVersion": True}, "Meta.InvalidSchemaVersion", "/SchemaVersion"),
            ({**missing_body_document(), "SchemaVersion": 2}, "Meta.InvalidSchemaVersion", "/SchemaVersion"),
            ({**missing_body_document(), "Target": ""}, "Meta.EmptyTarget", "/Target"),
            ({**missing_body_document(), "Target": "Generic"}, "Generic.NonCanonicalTarget", "/Target"),
            ({**missing_body_document(), "Class": GENERATED_CLASS}, "Generic.ClassMismatch", "/Class"),
            ({**missing_body_document(), "Action": "Delete"}, "Generic.InvalidAction", "/Action"),
            ({**missing_body_document(), "AssetType": "DataAsset"}, "Generic.InvalidAssetType", "/AssetType"),
            ({**missing_body_document(), "Definitions": []}, "Generic.InvalidDefinitionsType", "/Definitions"),
            ({**missing_body_document(), "Properties": []}, "Generic.InvalidPropertiesType", "/Properties"),
            ({**missing_body_document(), "Body": []}, "InvalidBodyType", "/Body"),
        )
        for document, code, path in cases:
            with self.subTest(code=code):
                assert_error(self, code, path, lambda document=document: self.profile.validate(document))

    def test_validate_accepts_optional_asset_type_and_all_existing_actions(self) -> None:
        for action in ("Create", "Update", "CreateOrUpdate"):
            document = missing_body_document()
            document["Action"] = action
            document["AssetType"] = "GenericAsset"
            with self.subTest(action=action):
                self.profile.validate(document)

    def test_required_primitives_are_exact_sixteen_lexical_unadmitted_rows(self) -> None:
        self.assertEqual(
            self.profile.runtime_entry_module,
            GENERIC_RUNTIME_ENTRY_MODULE,
        )
        expected_ids = (
            "editor.class.resolve_canonical.v1",
            "editor.fragment.struct_codec.v1",
            ENUMERATE_ID,
            ENUMERATE_BATCH_ID,
            "editor.property.preflight_admitted.v1",
            "editor.property.read_admitted.v1",
            WRITE_BATCH_ID,
            "editor.reference.resolve_asset.v1",
            "editor.asset.resolve_existing.v1",
            "editor.asset.create_by_factory.v1",
            "editor.scratch.clone_asset.v2",
            "editor.scratch.create_by_factory.v1",
            "editor.asset.create_by_factory.v2",
            "editor.property.inspect_evidence.v1",
            "ai.behavior_tree.selector_inspect_evidence.v1",
            "editor.enhanced_input.check_domain.v1",
        )
        self.assertEqual(self.profile.required_primitives(), GENERIC_REQUIRED_PRIMITIVES)
        self.assertEqual(tuple(item.primitive_id for item in GENERIC_REQUIRED_PRIMITIVES), expected_ids)
        self.assertTrue(all(item.required_by_route_id == GENERIC_ROUTE_ID for item in GENERIC_REQUIRED_PRIMITIVES))
        self.assertEqual(
            {item.primitive_id for item in GENERIC_REQUIRED_PRIMITIVES if item.version == 2},
            {"editor.scratch.clone_asset.v2", "editor.asset.create_by_factory.v2"},
        )
        self.assertNotIn("editor.fragment.embedded_object_create.v1", expected_ids)

    def test_inspection_preserves_generic_shape_and_reports_blocker_separately(self) -> None:
        self.assertEqual(
            self.profile.inspect_profile(),
            {
                "Class": EXACT_CLASS,
                "DocumentShape": {
                    "Definitions": "map<string, Fragment>",
                    "Properties": "reflected CDO-diff properties",
                    "Body": {},
                },
                "BodySections": [],
                "FragmentKinds": list(FRAGMENT_KINDS),
                "InternalAdapters": [],
                "RegionPolicies": [],
                "BlockedFragments": [
                    {
                        "kind": "EmbeddedObject",
                        "code": "AD.G1.BLOCK.EDITOR.EMBEDDED_OBJECT_AUTHORITY",
                    }
                ],
            },
        )

    def test_public_json_results_are_fresh_and_alias_isolated(self) -> None:
        source = rich_document()
        canonical = self.profile.canonicalize(source)
        source["Properties"]["Count"] = 99
        self.assertEqual(canonical["Properties"]["Count"], 7)
        canonical["Properties"]["Count"] = 42
        self.assertEqual(self.profile.canonicalize(rich_document())["Properties"]["Count"], 7)
        first = self.profile.schema()
        first["Properties"]["Body"]["accepts"] = ["mutated"]
        self.assertNotEqual(first, self.profile.schema())

    def test_preflight_forwards_scratch_only_through_named_handle_lane(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        single_property_catalog(context)
        scratch = context.issue_handle()
        context.queue_outcomes(
            PREFLIGHT_ID,
            success_outcome(value={"canonical_value": 7, "feasible": True}),
        )
        self.profile.preflight(operation_document(), scratch, context)
        call = context.calls[-1]
        self.assertEqual(call.value_arguments, {"value": 7})
        self.assertEqual([argument.name for argument in call.handle_arguments], ["scratch", "property"])
        self.assertIs(call.handle_arguments[0].handle, scratch)

    def test_apply_writes_once_and_repair_is_an_explicit_no_op(self) -> None:
        assert_profile_apply(self, self.profile)
        repair_context = MockPrimitiveContext(RuntimePhase.REPAIR)
        repair_result = self.profile.repair(
            operation_document(),
            repair_context.issue_handle(),
            repair_context,
        )
        self.assertEqual(repair_result, ())
        self.assertEqual(
            (
                repair_context.handler_calls,
                repair_context.calls,
                repair_context.successful_changes,
            ),
            (0, (), ()),
        )
        for operation in ("apply", "repair"):
            context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
            single_property_catalog(context)
            asset = context.issue_handle()
            with self.subTest(operation=operation):
                assert_error(
                    self,
                    "Generic.InvalidMutationPhase",
                    "/Phase",
                    lambda operation=operation: (
                        self.profile.apply(
                            operation_document(),
                            asset,
                            asset,
                            context,
                        )
                        if operation == "apply"
                        else self.profile.repair(
                            operation_document(),
                            asset,
                            context,
                        )
                    ),
                )
                self.assertEqual(
                    (context.handler_calls, context.calls, context.successful_changes),
                    (0, (), ()),
                )

    def test_extract_forwards_asset_only_through_named_handle_lane(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        single_property_catalog(context)
        asset = context.issue_handle()
        context.queue_outcomes(READ_ID, success_outcome(value={"value": 9}))
        extracted = self.profile.extract(target=TARGET, asset_handle=asset, context=context)
        call = context.calls[-1]
        self.assertEqual(call.value_arguments, {})
        self.assertEqual([argument.name for argument in call.handle_arguments], ["asset", "property"])
        self.assertIs(call.handle_arguments[0].handle, asset)
        self.assertEqual(extracted["Properties"], {"Value": 9})

    def test_extract_reads_structured_authored_values_in_their_shape(self) -> None:
        # Diff and Verify compare a partial struct / instanced value on what it
        # states: the provider reads it shaped like the authored value, and the
        # authored property is kept even when the shaped read equals the default.
        for authored, expected_arguments in (
            ({"Value": {"Member": 1}}, {"as_authored": {"Member": 1}}),
            ({"Value": [{"Member": 1}]}, {"as_authored": [{"Member": 1}]}),
            ({"Value": 9}, {}),
            (None, {}),
        ):
            with self.subTest(authored=authored):
                context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
                single_property_catalog(context)
                asset = context.issue_handle()
                context.queue_outcomes(READ_ID, success_outcome(value={"value": 9}))
                extracted = self.profile.extract(target=TARGET, asset_handle=asset, context=context,
                                                 authored_properties=authored)
                self.assertEqual(context.calls[-1].value_arguments, expected_arguments)
                self.assertEqual(extracted["Properties"], {"Value": 9})

    def test_extract_omits_editor_derived_properties(self) -> None:
        # A property holding a subobject the asset's own code creates (a
        # non-instanced reference inside the asset) is not an authored value.
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        single_property_catalog(context)
        asset = context.issue_handle()
        context.queue_outcomes(READ_ID, success_outcome(value={"editor_derived": True}))
        extracted = self.profile.extract(target=TARGET, asset_handle=asset, context=context)
        self.assertEqual(extracted["Properties"], {})

    def test_diff_excludes_authoring_and_compatibility_fields_and_normalizes_body(self) -> None:
        authored = rich_document()
        extracted = missing_body_document()
        extracted["AssetType"] = "GenericAsset"
        extracted["Definitions"] = {"ignored": {"Kind": "EmbeddedObject"}}
        extracted["Action"] = "Update"
        extracted["Properties"] = {"Count": 8, "Enabled": True, "Title": "authored"}
        differences = self.profile.diff(authored, extracted)
        self.assertEqual([(item.path, item.kind) for item in differences], [("/Properties/Count", DiffKind.REPLACE)])

    def test_factory_context_is_an_optional_create_time_asset_path(self) -> None:
        document = missing_body_document()
        document["FactoryContext"] = "/Game/Characters/SK_Hero.SK_Hero"
        self.profile.validate(document)
        self.assertEqual(self.profile.factory_context(document), "/Game/Characters/SK_Hero.SK_Hero")
        self.assertIsNone(self.profile.factory_context(missing_body_document()))
        self.assertTrue(self.profile.schema()["Properties"]["FactoryContext"]["create_only"])
        for value in (None, 7, "", "SK_Hero", "/Game/Characters/SK_Hero", "Game/SK.SK"):
            with self.subTest(value=value):
                invalid = {**missing_body_document(), "FactoryContext": value}
                assert_error(self, "Generic.InvalidFactoryContext", "/FactoryContext",
                             lambda invalid=invalid: self.profile.validate(invalid))
        extracted = {**missing_body_document(), "AssetType": "GenericAsset"}
        self.assertEqual(self.profile.diff(document, extracted), ())

    def test_diff_compares_only_authored_property_surface(self) -> None:
        for authored_properties, extracted_properties, expected in (
            ({}, {"DefaultInt": 0, "DefaultString": ""}, ()),
            (
                {"Value": 7},
                {"DefaultInt": 0, "Value": 0},
                (("/Properties/Value", DiffKind.REPLACE, 7, 0),),
            ),
            # Extract output passed through UE JSON (1.0 written as 1) is unchanged.
            ({"Ratio": 1, "Pair": [2, 0.5]}, {"Ratio": 1.0, "Pair": [2.0, 0.5]}, ()),
            ({"Ratio": 1.0}, {"Ratio": 1}, ()),
            ({"Ratio": 0.5}, {"Ratio": 1.0}, (("/Properties/Ratio", DiffKind.REPLACE, 0.5, 1),)),
        ):
            with self.subTest(authored=authored_properties):
                authored = self.profile.create_template(TARGET)
                authored["Properties"] = authored_properties
                extracted = self.profile.create_template(TARGET)
                extracted["Properties"] = extracted_properties
                self.assertEqual(
                    tuple(
                        (item.path, item.kind, item.before, item.after)
                        for item in self.profile.diff(authored, extracted)
                    ),
                    expected,
                )

    def test_candidate_is_not_a_profile_meta_and_imports_no_registry_surface(self) -> None:
        self.assertEqual(GENERIC_PROFILE_ID, "profile.generic-candidate.v1")
        self.assertNotIn("ProfileRegistryBuilder", generic_module.__dict__)
        self.assertNotIn("FrozenProfileRegistry", generic_module.__dict__)
        self.assertNotIn("asset_document.registry", generic_module.__dict__)

    def test_mock_catalog_matches_frozen_g1_unadmitted_rows(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        expected = {item.primitive_id: "unadmitted" for item in GENERIC_REQUIRED_PRIMITIVES}
        self.assertEqual(context.runtime_dispositions, expected)
        self.assertEqual(len(expected), 16)
        self.assertFalse(any("alias" in key or "internal" in key or "BLOCK" in key for key in expected))


class InspectBodyOwnedFilterTests(unittest.TestCase):
    def test_body_owned_rows_are_omitted_from_properties_and_skipped(self) -> None:
        payload = {
            "class": "/Script/AIModule.BlackboardData",
            "properties": [{"name": "Parent"}, {"name": "Visible"}],
            "skipped": [{"name": "keys", "reason": "non-editable"}, {"name": "Other", "reason": "transient"}],
            "selector_policies": [],
        }
        filtered = generic_module.omit_inspected_properties(payload, ("Parent", "Keys"))
        self.assertEqual(filtered["properties"], [{"name": "Visible"}])
        self.assertEqual(filtered["skipped"], [{"name": "Other", "reason": "transient"}])
        self.assertEqual(filtered["selector_policies"], [])
        self.assertEqual(len(payload["properties"]), 2)



if __name__ == "__main__":
    unittest.main()
