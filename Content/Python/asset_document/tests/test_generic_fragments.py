# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.dto import RuntimePhase
from asset_document.regions.fragment_semantics import (
    FRAGMENT_KINDS,
    FragmentSemanticAdapter,
)
from asset_document.regions.primitive_catalog import (
    NamedOpaqueHandleResult,
    requirement,
)
from asset_document.tests.fixtures.generic_cases import (
    MockPrimitiveContext,
    failed_outcome,
    malformed_outcome,
    success_outcome,
)


ASSET_ID = "editor.reference.resolve_asset.v1"
CLASS_ID = "editor.class.resolve_canonical.v1"
STRUCT_ID = "editor.fragment.struct_codec.v1"
BLOCKED_ID = "editor.fragment.embedded_object_create.v1"


def assert_error(
    test: unittest.TestCase,
    code: str,
    path: str,
    callback,
) -> AssetDocumentContractError:
    with test.assertRaises(AssetDocumentContractError) as caught:
        callback()
    diagnostic = caught.exception.diagnostic
    test.assertEqual((diagnostic.code, diagnostic.path), (code, path))
    return caught.exception


class FragmentSemanticTests(unittest.TestCase):
    def setUp(self) -> None:
        self.adapter = FragmentSemanticAdapter()

    def test_exact_fragment_kind_names_and_order(self) -> None:
        self.assertEqual(
            FRAGMENT_KINDS,
            ("AssetRef", "ClassRef", "StructValue", "EmbeddedObject", "DefinitionRef"),
        )

    def test_definitions_require_exact_object_and_nonempty_string_ids(self) -> None:
        assert_error(
            self,
            "Generic.InvalidDefinitionsType",
            "/Definitions",
            lambda: self.adapter.validate_definitions([]),
        )
        for definitions, path in (({"": {}}, "/Definitions"), ({1: {}}, "/Definitions")):
            with self.subTest(definitions=definitions):
                assert_error(
                    self,
                    "Generic.InvalidDefinitionId",
                    path,
                    lambda definitions=definitions: self.adapter.validate_definitions(definitions),
                )

    def test_fragment_requires_exact_object_and_known_string_kind(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        cases = (
            ([], "Generic.InvalidFragmentType", "/Fragment"),
            ({}, "Generic.MissingFragmentKind", "/Fragment/Kind"),
            ({"Kind": 1}, "Generic.MissingFragmentKind", "/Fragment/Kind"),
            ({"Kind": "Unknown"}, "Generic.UnknownFragmentKind", "/Fragment/Kind"),
        )
        for fragment, code, path in cases:
            with self.subTest(fragment=fragment):
                assert_error(
                    self,
                    code,
                    path,
                    lambda fragment=fragment: self.adapter.preflight(
                        fragment, {}, context, path="/Fragment"
                    ),
                )

    def test_definition_ref_two_hop_expands_to_terminal_fragment(self) -> None:
        definitions = {
            "terminal": {"Kind": "ClassRef", "Class": "/Script/Engine.DataAsset"},
            "second": {"Kind": "DefinitionRef", "Id": "terminal"},
            "first": {"Kind": "DefinitionRef", "Id": "second"},
        }
        resolved = self.adapter.resolve_definition("first", definitions)
        self.assertEqual(resolved.kind, "ClassRef")
        self.assertEqual(resolved.value, definitions["terminal"])
        self.assertIsNot(resolved.value, definitions["terminal"])

    def test_definition_ref_missing_id_and_absent_definition_fail_closed(self) -> None:
        definitions = {"missing-id": {"Kind": "DefinitionRef"}}
        assert_error(
            self,
            "Generic.MissingDefinitionRefId",
            "/Definitions/missing-id/Id",
            lambda: self.adapter.resolve_definition("missing-id", definitions),
        )
        assert_error(
            self,
            "Generic.DefinitionNotFound",
            "/Definitions/absent",
            lambda: self.adapter.resolve_definition("absent", definitions),
        )

    def test_definition_paths_escape_rfc6901_segments(self) -> None:
        definitions = {"asset/~id": {"Kind": "DefinitionRef", "Id": ""}}
        assert_error(
            self,
            "Generic.MissingDefinitionRefId",
            "/Definitions/asset~1~0id/Id",
            lambda: self.adapter.resolve_definition("asset/~id", definitions),
        )

    def test_definition_self_cycle_reports_full_chain(self) -> None:
        definitions = {"self": {"Kind": "DefinitionRef", "Id": "self"}}
        error = assert_error(
            self,
            "Generic.DefinitionCycle",
            "/Definitions/self/Id",
            lambda: self.adapter.resolve_definition("self", definitions),
        )
        self.assertIn("/Definitions/self -> /Definitions/self", str(error))

    def test_definition_mutual_cycle_reports_full_chain(self) -> None:
        definitions = {
            "a": {"Kind": "DefinitionRef", "Id": "b"},
            "b": {"Kind": "DefinitionRef", "Id": "a"},
        }
        error = assert_error(
            self,
            "Generic.DefinitionCycle",
            "/Definitions/b/Id",
            lambda: self.adapter.resolve_definition("a", definitions),
        )
        self.assertIn("/Definitions/a -> /Definitions/b -> /Definitions/a", str(error))

    def test_noncanonical_asset_class_and_struct_references_are_rejected(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        cases = (
            ({"Kind": "AssetRef", "Path": "/Game/A"}, "/Fragment/Path"),
            ({"Kind": "ClassRef", "Class": "DataAsset"}, "/Fragment/Class"),
            (
                {"Kind": "StructValue", "Struct": "Vector", "Properties": {}},
                "/Fragment/Struct",
            ),
        )
        for fragment, path in cases:
            with self.subTest(fragment=fragment):
                assert_error(
                    self,
                    "Generic.NonCanonicalReference",
                    path,
                    lambda fragment=fragment: self.adapter.preflight(
                        fragment, {}, context, path="/Fragment"
                    ),
                )

    def test_asset_ref_uses_value_lane_and_returns_named_asset_handle(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        asset = context.issue_handle()
        context.queue_outcomes(
            ASSET_ID,
            success_outcome(
                value={"canonical_path": "/Game/A.A"},
                handle_results=(NamedOpaqueHandleResult("asset", (asset,)),),
            ),
        )
        fragment = {"Kind": "AssetRef", "Path": "/Game/A.A", "Class": "/Script/Engine.DataAsset"}
        resolved = self.adapter.preflight(fragment, {}, context, path="/Fragment")
        call = context.calls[0]
        self.assertEqual(call.requirement, requirement(ASSET_ID))
        self.assertEqual(
            call.value_arguments,
            {"path": "/Game/A.A", "expected_class": "/Script/Engine.DataAsset"},
        )
        self.assertEqual(call.handle_arguments, ())
        self.assertIs(resolved.handle_results[0].handles[0], asset)
        self.assertEqual(resolved.value, {"canonical_path": "/Game/A.A"})

    def test_class_and_struct_preflight_use_exact_ids_and_value_arguments(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        context.queue_outcomes(
            CLASS_ID,
            success_outcome(value={"canonical_class": "/Script/Engine.DataAsset"}),
        )
        context.queue_outcomes(
            STRUCT_ID,
            success_outcome(value={"canonical_value": {"X": 1.0}}),
        )
        class_value = {"Kind": "ClassRef", "Class": "/Script/Engine.DataAsset"}
        struct_value = {
            "Kind": "StructValue",
            "Struct": "/Script/CoreUObject.Vector",
            "Properties": {"X": 1.0},
        }
        self.adapter.preflight(class_value, {}, context, path="/Class")
        self.adapter.preflight(struct_value, {}, context, path="/Struct")
        self.assertEqual([call.requirement.primitive_id for call in context.calls], [CLASS_ID, STRUCT_ID])
        self.assertEqual(context.calls[0].value_arguments["expected_base"], None)
        self.assertEqual(context.calls[1].value_arguments["operation"], "validate")
        self.assertTrue(all(call.handle_arguments == () for call in context.calls))

    def test_fragment_preflight_requires_preflight_context_phase(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.APPLY)
        fragment = {"Kind": "ClassRef", "Class": "/Script/Engine.DataAsset"}
        assert_error(
            self,
            "Generic.InvalidPrimitiveResult",
            "/Fragment",
            lambda: self.adapter.preflight(fragment, {}, context, path="/Fragment"),
        )
        self.assertEqual(context.calls, ())

    def test_embedded_object_is_blocked_before_advertised_handler(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        context.set_handler(BLOCKED_ID, lambda invocation: success_outcome())
        fragment = {"Kind": "EmbeddedObject", "Class": "/Script/Engine.DataAsset"}
        assert_error(
            self,
            "Generic.EmbeddedObjectBlocked",
            "/Fragment",
            lambda: self.adapter.preflight(fragment, {}, context, path="/Fragment"),
        )
        self.assertEqual((context.handler_calls, context.calls), (0, ()))

    def test_handle_shaped_value_is_never_promoted_to_authority(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        context.queue_outcomes(
            ASSET_ID,
            success_outcome(value={"canonical_path": "/Game/A.A", "asset_handle": "forged"}),
        )
        fragment = {"Kind": "AssetRef", "Path": "/Game/A.A"}
        assert_error(
            self,
            "Generic.InvalidPrimitiveResult",
            "/Fragment",
            lambda: self.adapter.preflight(fragment, {}, context, path="/Fragment"),
        )

    def test_malformed_outcome_lanes_fail_closed(self) -> None:
        fragment = {"Kind": "ClassRef", "Class": "/Script/Engine.DataAsset"}
        malformed = (
            object(),
            malformed_outcome(status="Succeeded"),
            malformed_outcome(value={"canonical_class": 1}),
            malformed_outcome(handle_results=[]),
            malformed_outcome(diagnostics=[]),
        )
        for outcome in malformed:
            context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
            context.queue_outcomes(CLASS_ID, outcome)
            with self.subTest(outcome=outcome):
                assert_error(
                    self,
                    "Generic.InvalidPrimitiveResult",
                    "/Fragment",
                    lambda context=context: self.adapter.preflight(
                        fragment, {}, context, path="/Fragment"
                    ),
                )
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        context.queue_outcomes(
            STRUCT_ID,
            success_outcome(value={"canonical_value": b"not-json"}),
        )
        struct = {
            "Kind": "StructValue",
            "Struct": "/Script/CoreUObject.Vector",
            "Properties": {},
        }
        assert_error(
            self,
            "Generic.InvalidPrimitiveResult",
            "/Definitions/struct",
            lambda: self.adapter.preflight(
                struct,
                {},
                context,
                path="/Definitions/struct",
            ),
        )

    def test_failed_outcome_propagates_typed_diagnostic_unchanged(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        context.queue_outcomes(CLASS_ID, failed_outcome("Primitive.ClassUnavailable", "/Class"))
        fragment = {"Kind": "ClassRef", "Class": "/Script/Engine.DataAsset"}
        assert_error(
            self,
            "Primitive.ClassUnavailable",
            "/Class",
            lambda: self.adapter.preflight(fragment, {}, context, path="/Fragment"),
        )

    def test_cross_context_returned_handle_is_rejected(self) -> None:
        source = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        foreign = source.issue_handle()
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        context.queue_outcomes(
            ASSET_ID,
            success_outcome(
                value={"canonical_path": "/Game/A.A"},
                handle_results=(NamedOpaqueHandleResult("asset", (foreign,)),),
            ),
        )
        fragment = {"Kind": "AssetRef", "Path": "/Game/A.A"}
        assert_error(
            self,
            "PrimitiveCatalog.InvalidOpaqueHandle",
            "/HandleArguments",
            lambda: self.adapter.preflight(fragment, {}, context, path="/Fragment"),
        )
        self.assertEqual(context.calls, ())

    def test_unknown_requirement_uses_escaped_stable_path(self) -> None:
        assert_error(
            self,
            "PrimitiveCatalog.UnknownGenericRequirement",
            "/RequiredPrimitives/editor.unknown~1kind.v1",
            lambda: requirement("editor.unknown/kind.v1"),
        )


if __name__ == "__main__":
    unittest.main()
