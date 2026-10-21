import copy
import json
import pickle
import unittest

from asset_document.diagnostics import (
    AssetDocumentContractError,
    DiagnosticContext,
    RuntimeDiagnostic,
)
from asset_document.dto import OptionalJsonValue, RuntimePhase
from asset_document.regions.primitive_catalog import (
    NamedOpaqueHandleResult,
    PrimitiveOutcome,
    PrimitiveOutcomeStatus,
)
from asset_document.regions.property_semantics import (
    AdmittedProperty,
    GENERIC_RUNTIME_MUTATION_PHASES,
    PropertySemanticAdapter,
)
from asset_document.tests.fixtures.generic_cases import (
    ENUMERATE_BATCH_ID,
    ENUMERATE_ID,
    EXACT_CLASS,
    PREFLIGHT_ID,
    READ_ID,
    WRITE_ID,
    WRITE_BATCH_ID,
    MockPrimitiveContext,
    admitted_property_rows,
    changed_record,
    failed_outcome,
    malformed_outcome,
    property_batch_enumeration_outcome,
    property_enumeration_outcome,
    success_outcome,
)


def assert_error(test, code: str, path: str, callback) -> AssetDocumentContractError:
    with test.assertRaises(AssetDocumentContractError) as caught:
        callback()
    actual = caught.exception.diagnostic
    test.assertEqual((actual.code, actual.path), (code, path))
    return caught.exception


def enumerate_properties(
    context: MockPrimitiveContext,
    rows=None,
) -> tuple[PropertySemanticAdapter, tuple[AdmittedProperty, ...], tuple[object, ...]]:
    adapter = PropertySemanticAdapter()
    source_rows = admitted_property_rows() if rows is None else rows
    outcome, handles = property_enumeration_outcome(
        context,
        provider_property_rows(source_rows),
    )
    context.queue_outcomes(ENUMERATE_ID, outcome)
    admitted = adapter.enumerate(EXACT_CLASS, context)
    return adapter, admitted, handles


def provider_property_rows(rows):
    projected = copy.deepcopy(rows)
    for row in projected:
        row.setdefault("declaring_class", EXACT_CLASS)
        row.setdefault("codec_id", "property.test.v1")
    return projected


def single_admitted(context: MockPrimitiveContext, *, writable: bool = True):
    return (
        AdmittedProperty(
            "Value",
            context.issue_handle(),
            True,
            writable,
            OptionalJsonValue(True, 0),
        ),
    )


def single_batch_outcome(context: MockPrimitiveContext):
    return property_batch_enumeration_outcome(
        context,
        provider_property_rows(
            [{"name": "Value", "readable": True, "writable": True, "default": 0}]
        ),
    )


def malformed_batch_groups_outcome(
    context: MockPrimitiveContext,
    variant: str,
) -> PrimitiveOutcome:
    property_handle = context.issue_handle()
    batch_handle = context.issue_handle()
    properties = NamedOpaqueHandleResult("properties", (property_handle,))
    batch = NamedOpaqueHandleResult("property_batch", (batch_handle,))
    groups_by_variant = {
        "missing_batch": (properties,),
        "extra_group": (properties, batch, NamedOpaqueHandleResult("extra", ())),
        "wrong_name": (
            properties,
            NamedOpaqueHandleResult("batch", (batch_handle,)),
        ),
        "wrong_order": (batch, properties),
        "empty_batch": (
            properties,
            NamedOpaqueHandleResult("property_batch", ()),
        ),
        "multiple_batch": (
            properties,
            NamedOpaqueHandleResult(
                "property_batch",
                (batch_handle, context.issue_handle()),
            ),
        ),
        "row_handle_mismatch": (
            NamedOpaqueHandleResult("properties", ()),
            batch,
        ),
    }
    return success_outcome(
        value={
            "properties": provider_property_rows(
                [{"name": "Value", "readable": True, "writable": True}]
            )
        },
        handle_results=groups_by_variant[variant],
    )


def run_stale_operation(operation, adapter, asset, admitted, context):
    if operation == "preflight":
        return adapter.preflight(asset, {"Value": 1}, admitted, context)
    if operation == "read":
        return adapter.extract(asset, admitted, context, include_defaults=False)
    return adapter.apply(
        asset,
        asset,
        {"Value": 1},
        admitted,
        context,
        phase=RuntimePhase.APPLY,
    )


def run_malformed_operation(operation, adapter, asset, admitted, context):
    if operation == "enumerate":
        return adapter.enumerate(EXACT_CLASS, context)
    if operation == "preflight":
        return adapter.preflight(asset, {"Value": 1}, admitted, context)
    if operation == "read":
        return adapter.extract(asset, admitted, context, include_defaults=False)
    return adapter.apply(
        asset,
        asset,
        {"Value": 1},
        admitted,
        context,
        phase=RuntimePhase.APPLY,
    )


def assert_type_error(test: unittest.TestCase, callback) -> None:
    with test.assertRaises(TypeError):
        callback()


def assert_apply_batch_contract(test: unittest.TestCase) -> None:
    context = MockPrimitiveContext(RuntimePhase.APPLY)
    adapter, admitted, _ = enumerate_properties(context)
    read_asset = context.issue_handle()
    mutable_asset = context.issue_handle()
    expected = (changed_record("Count"), changed_record("Title"))
    batch_outcome, _, property_batch = property_batch_enumeration_outcome(
        context,
        provider_property_rows(sorted(admitted_property_rows(), key=lambda row: row["name"])),
    )
    context.queue_outcomes(ENUMERATE_BATCH_ID, batch_outcome)
    context.queue_outcomes(
        WRITE_BATCH_ID,
        success_outcome(value={}, changed_set=expected),
    )
    actual = adapter.apply(
        read_asset,
        mutable_asset,
        {"Title": "z", "Count": 7},
        admitted,
        context,
        phase=RuntimePhase.APPLY,
    )
    test.assertIs(actual, expected)
    calls = context.calls[1:]
    test.assertEqual(
        [call.requirement.primitive_id for call in calls],
        [ENUMERATE_BATCH_ID, WRITE_BATCH_ID],
    )
    test.assertEqual(calls[0].value_arguments, {})
    test.assertEqual(
        [argument.name for argument in calls[0].handle_arguments],
        ["asset"],
    )
    test.assertIs(calls[0].handle_arguments[0].handle, read_asset)
    test.assertEqual(
        calls[1].value_arguments,
        {
            "properties": [
                {"name": "Count", "value": 7},
                {"name": "Title", "value": "z"},
            ]
        },
    )
    test.assertEqual(
        [argument.name for argument in calls[1].handle_arguments],
        ["asset", "property_batch"],
    )
    test.assertIs(calls[1].handle_arguments[0].handle, mutable_asset)
    test.assertIs(calls[1].handle_arguments[1].handle, property_batch)


def assert_repair_adapter_rejected(test: unittest.TestCase) -> None:
    context = MockPrimitiveContext(RuntimePhase.REPAIR)
    adapter = PropertySemanticAdapter()
    admitted = single_admitted(context)
    asset = context.issue_handle()
    assert_error(
        test,
        "Generic.InvalidMutationPhase",
        "/Phase",
        lambda: adapter.apply(
            asset,
            asset,
            {"Value": 1},
            admitted,
            context,
            phase=RuntimePhase.REPAIR,
        ),
    )
    test.assertEqual((context.handler_calls, context.calls), (0, ()))


class PropertySemanticTests(unittest.TestCase):
    def test_enumerate_accepts_frozen_provider_metadata_keys(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        rows = [
            {
                "name": "Value",
                "declaring_class": EXACT_CLASS,
                "codec_id": "property.scalar.v1",
                "readable": True,
                "writable": True,
            }
        ]
        _, admitted, _ = enumerate_properties(context, rows)
        self.assertEqual([item.name for item in admitted], ["Value"])

    def test_enumerate_decodes_exact_rows_and_sorts_with_handle_association(self) -> None:
        rows = admitted_property_rows()
        rows.reverse()
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        _, admitted, handles = enumerate_properties(context, rows)
        self.assertEqual([item.name for item in admitted], sorted(row["name"] for row in rows))
        source_by_name = {row["name"]: handle for row, handle in zip(rows, handles)}
        for item in admitted:
            self.assertIs(item.property_handle, source_by_name[item.name])
        self.assertEqual(
            context.calls[0].value_arguments,
            {"exact_asset_class_path": EXACT_CLASS},
        )
        self.assertEqual(context.calls[0].handle_arguments, ())

    def test_enumerate_rejects_invalid_descriptor_value_types_and_keys(self) -> None:
        cases = (
            (None, "Generic.InvalidPropertyDescriptor"),
            ({}, "Generic.InvalidPropertyDescriptor"),
            ({"properties": []}, "Generic.InvalidPropertyDescriptor"),
            ({"properties": [1]}, "Generic.InvalidPropertyDescriptor"),
            (
                {"properties": [{"name": "A", "readable": 1, "writable": True}]},
                "Generic.InvalidPropertyDescriptor",
            ),
            (
                {"properties": [{"name": "A", "readable": True, "writable": True, "extra": 1}]},
                "Generic.InvalidPropertyDescriptor",
            ),
        )
        for value, code in cases:
            context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
            context.queue_outcomes(ENUMERATE_ID, success_outcome(value=value))
            with self.subTest(value=value):
                assert_error(
                    self,
                    code,
                    "/Properties",
                    lambda context=context: PropertySemanticAdapter().enumerate(EXACT_CLASS, context),
                )

    def test_enumerate_rejects_duplicate_names_and_row_handle_count_mismatch(self) -> None:
        duplicate = [
            {"name": "A", "readable": True, "writable": True},
            {"name": "A", "readable": True, "writable": True},
        ]
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        outcome, _ = property_enumeration_outcome(
            context,
            provider_property_rows(duplicate),
        )
        context.queue_outcomes(ENUMERATE_ID, outcome)
        assert_error(
            self,
            "Generic.DuplicatePropertyName",
            "/Properties/A",
            lambda: PropertySemanticAdapter().enumerate(EXACT_CLASS, context),
        )
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        handle = context.issue_handle()
        bad = success_outcome(
            value={"properties": provider_property_rows(admitted_property_rows())},
            handle_results=(NamedOpaqueHandleResult("properties", (handle,)),),
        )
        context.queue_outcomes(ENUMERATE_ID, bad)
        assert_error(
            self,
            "Generic.InvalidPropertyDescriptor",
            "/Properties",
            lambda: PropertySemanticAdapter().enumerate(EXACT_CLASS, context),
        )

    def test_validate_rejects_nonobject_unknown_and_nonwritable_properties(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        adapter, admitted, _ = enumerate_properties(context)
        assert_error(
            self,
            "Generic.InvalidPropertiesType",
            "/Properties",
            lambda: adapter.validate([], admitted),
        )
        for properties, code, path in (
            ({"Unknown/~": 1}, "Generic.UnknownProperty", "/Properties/Unknown~1~0"),
            ({"ReadOnly": "change"}, "Generic.NonWritableProperty", "/Properties/ReadOnly"),
        ):
            with self.subTest(properties=properties):
                assert_error(
                    self,
                    code,
                    path,
                    lambda properties=properties: adapter.validate(properties, admitted),
                )

    def test_preflight_is_lexical_and_keeps_value_and_handle_lanes_separate(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        adapter, admitted, _ = enumerate_properties(context)
        scratch = context.issue_handle()
        context.queue_outcomes(
            PREFLIGHT_ID,
            success_outcome(value={"canonical_value": 7, "feasible": True}),
            success_outcome(value={"canonical_value": "z", "feasible": True}),
        )
        adapter.preflight(scratch, {"Title": "z", "Count": 7}, admitted, context)
        calls = context.calls[1:]
        self.assertEqual([call.value_arguments for call in calls], [{"value": 7}, {"value": "z"}])
        self.assertEqual([call.requirement.primitive_id for call in calls], [PREFLIGHT_ID, PREFLIGHT_ID])
        for call in calls:
            self.assertEqual([argument.name for argument in call.handle_arguments], ["scratch", "property"])
            self.assertIs(call.handle_arguments[0].handle, scratch)
        count_handle = next(item.property_handle for item in admitted if item.name == "Count")
        self.assertIs(calls[0].handle_arguments[1].handle, count_handle)

    def test_apply_preserves_typed_changed_record_order_and_repair_is_rejected(self) -> None:
        self.assertEqual(
            GENERIC_RUNTIME_MUTATION_PHASES,
            (RuntimePhase.APPLY,),
        )
        assert_apply_batch_contract(self)
        assert_repair_adapter_rejected(self)

    def test_batch_write_preserves_provider_snapshot_order_across_case_boundaries(self) -> None:
        rows = provider_property_rows(
            [
                {"name": "bTestBool", "readable": True, "writable": True},
                {"name": "TestFloat", "readable": True, "writable": True},
                {"name": "TestInt", "readable": True, "writable": True},
                {"name": "TestName", "readable": True, "writable": True},
                {"name": "TestString", "readable": True, "writable": True},
            ]
        )
        context = MockPrimitiveContext(RuntimePhase.APPLY)
        adapter, admitted, _ = enumerate_properties(context, rows)
        asset = context.issue_handle()
        batch_outcome, _, _ = property_batch_enumeration_outcome(context, rows)
        context.queue_outcomes(ENUMERATE_BATCH_ID, batch_outcome)
        context.queue_outcomes(
            WRITE_BATCH_ID,
            success_outcome(
                changed_set=(
                    changed_record("bTestBool"),
                    changed_record("TestFloat"),
                    changed_record("TestString"),
                )
            ),
        )

        adapter.apply(
            asset,
            asset,
            {
                "TestString": "hello",
                "bTestBool": True,
                "TestFloat": 3.5,
            },
            admitted,
            context,
            phase=RuntimePhase.APPLY,
        )

        self.assertEqual(
            context.calls[-1].value_arguments,
            {
                "properties": [
                    {"name": "bTestBool", "value": True},
                    {"name": "TestFloat", "value": 3.5},
                    {"name": "TestString", "value": "hello"},
                ]
            },
        )

    def test_batch_write_accepts_exact_empty_and_single_sparse_rows(self) -> None:
        for properties in ({}, {"Title": "single"}):
            context = MockPrimitiveContext(RuntimePhase.APPLY)
            adapter, admitted, _ = enumerate_properties(context)
            asset = context.issue_handle()
            batch_outcome, _, _ = property_batch_enumeration_outcome(
                context,
                provider_property_rows(admitted_property_rows()),
            )
            context.queue_outcomes(ENUMERATE_BATCH_ID, batch_outcome)
            expected = () if not properties else (changed_record("Title"),)
            context.queue_outcomes(
                WRITE_BATCH_ID,
                success_outcome(changed_set=expected),
            )
            with self.subTest(properties=properties):
                actual = adapter.apply(
                    asset,
                    asset,
                    properties,
                    admitted,
                    context,
                    phase=RuntimePhase.APPLY,
                )
                self.assertIs(actual, expected)
                self.assertEqual(
                    context.calls[-1].value_arguments,
                    {
                        "properties": [
                            {"name": name, "value": properties[name]}
                            for name in sorted(properties)
                        ]
                    },
                )
                self.assertEqual(
                    [
                        call.requirement.primitive_id
                        for call in context.calls
                        if call.requirement.primitive_id == WRITE_BATCH_ID
                    ],
                    [WRITE_BATCH_ID],
                )

    def test_invalid_and_duplicate_batch_rows_fail_before_write(self) -> None:
        cases = (
            (
                [{"name": "Value", "readable": True}],
                "Generic.InvalidPropertyDescriptor",
                "/Properties",
            ),
            (
                [
                    {"name": "Value", "readable": True, "writable": True},
                    {"name": "Value", "readable": True, "writable": True},
                ],
                "Generic.DuplicatePropertyName",
                "/Properties/Value",
            ),
        )
        for rows, code, path in cases:
            context = MockPrimitiveContext(RuntimePhase.APPLY)
            adapter = PropertySemanticAdapter()
            admitted = single_admitted(context)
            asset = context.issue_handle()
            batch_outcome, _, _ = property_batch_enumeration_outcome(
                context,
                provider_property_rows(rows),
            )
            context.queue_outcomes(ENUMERATE_BATCH_ID, batch_outcome)
            with self.subTest(code=code):
                assert_error(
                    self,
                    code,
                    path,
                    lambda: adapter.apply(
                        asset,
                        asset,
                        {"Value": 1},
                        admitted,
                        context,
                        phase=RuntimePhase.APPLY,
                    ),
                )
                self.assertNotIn(
                    WRITE_BATCH_ID,
                    [call.requirement.primitive_id for call in context.calls],
                )
                self.assertEqual(context.successful_changes, ())

    def test_batch_handle_groups_and_row_handle_count_are_exact(self) -> None:
        for variant in (
            "missing_batch",
            "extra_group",
            "wrong_name",
            "wrong_order",
            "empty_batch",
            "multiple_batch",
            "row_handle_mismatch",
        ):
            context = MockPrimitiveContext(RuntimePhase.APPLY)
            adapter = PropertySemanticAdapter()
            admitted = single_admitted(context)
            asset = context.issue_handle()
            context.queue_outcomes(
                ENUMERATE_BATCH_ID,
                malformed_batch_groups_outcome(context, variant),
            )
            with self.subTest(variant=variant):
                assert_error(
                    self,
                    "Generic.InvalidPropertyDescriptor",
                    "/Properties",
                    lambda: adapter.apply(
                        asset,
                        asset,
                        {"Value": 1},
                        admitted,
                        context,
                        phase=RuntimePhase.APPLY,
                    ),
                )
                self.assertNotIn(
                    WRITE_BATCH_ID,
                    [call.requirement.primitive_id for call in context.calls],
                )

    def test_cross_context_asset_fails_before_batch_handler(self) -> None:
        source = MockPrimitiveContext(RuntimePhase.APPLY)
        foreign_asset = source.issue_handle()
        context = MockPrimitiveContext(RuntimePhase.APPLY)
        adapter = PropertySemanticAdapter()
        admitted = single_admitted(context)
        local_batch_outcome, _, _ = single_batch_outcome(context)
        context.queue_outcomes(ENUMERATE_BATCH_ID, local_batch_outcome)
        assert_error(
            self,
            "PrimitiveCatalog.InvalidOpaqueHandle",
            "/HandleArguments",
            lambda: adapter.apply(
                foreign_asset,
                foreign_asset,
                {"Value": 1},
                admitted,
                context,
                phase=RuntimePhase.APPLY,
            ),
        )
        self.assertEqual((context.handler_calls, context.calls), (0, ()))

    def test_cross_context_batch_proxy_fails_before_success(self) -> None:
        source = MockPrimitiveContext(RuntimePhase.APPLY)
        foreign_batch = source.issue_handle()
        context = MockPrimitiveContext(RuntimePhase.APPLY)
        adapter = PropertySemanticAdapter()
        admitted = single_admitted(context)
        asset = context.issue_handle()
        property_handle = context.issue_handle()
        context.queue_outcomes(
            ENUMERATE_BATCH_ID,
            success_outcome(
                value={
                    "properties": [
                        {"name": "Value", "readable": True, "writable": True}
                    ]
                },
                handle_results=(
                    NamedOpaqueHandleResult("properties", (property_handle,)),
                    NamedOpaqueHandleResult("property_batch", (foreign_batch,)),
                ),
            ),
        )
        assert_error(
            self,
            "PrimitiveCatalog.InvalidOpaqueHandle",
            "/HandleArguments",
            lambda: adapter.apply(
                asset,
                asset,
                {"Value": 1},
                admitted,
                context,
                phase=RuntimePhase.APPLY,
            ),
        )
        self.assertEqual((context.calls, context.successful_changes), ((), ()))

    def test_stale_batch_proxy_fails_before_success(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.APPLY)
        adapter = PropertySemanticAdapter()
        admitted = single_admitted(context)
        asset = context.issue_handle()
        stale_outcome, _, _ = single_batch_outcome(context)

        def expire_before_return(invocation):
            context.expire()
            return stale_outcome

        context.set_handler(ENUMERATE_BATCH_ID, expire_before_return)
        assert_error(
            self,
            "PrimitiveCatalog.InvalidOpaqueHandle",
            "/HandleArguments",
            lambda: adapter.apply(
                asset,
                asset,
                {"Value": 1},
                admitted,
                context,
                phase=RuntimePhase.APPLY,
            ),
        )
        self.assertEqual((context.calls, context.successful_changes), ((), ()))

    def test_failed_batch_write_propagates_once_without_changed_progress(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.APPLY)
        adapter = PropertySemanticAdapter()
        admitted = single_admitted(context)
        asset = context.issue_handle()
        batch_outcome, _, _ = single_batch_outcome(context)
        context.queue_outcomes(ENUMERATE_BATCH_ID, batch_outcome)
        context.queue_outcomes(
            WRITE_BATCH_ID,
            failed_outcome("Primitive.BatchFailure", "/Properties"),
        )
        assert_error(
            self,
            "Primitive.BatchFailure",
            "/Properties",
            lambda: adapter.apply(
                asset,
                asset,
                {"Value": 1},
                admitted,
                context,
                phase=RuntimePhase.APPLY,
            ),
        )
        self.assertEqual(
            [call.requirement.primitive_id for call in context.calls],
            [ENUMERATE_BATCH_ID, WRITE_BATCH_ID],
        )
        self.assertEqual((context.handler_calls, context.successful_changes), (2, ()))

    def test_codec_value_failure_is_located_inside_the_authored_property(self) -> None:
        def located_failure(name, value_path):
            return PrimitiveOutcome(
                PrimitiveOutcomeStatus.FAILED,
                None,
                (),
                (),
                (
                    RuntimeDiagnostic(
                        path="/Primitive",
                        code="PropertyWrite.DecodeFailed",
                        message="'/Game/Missing.Missing' does not resolve to an existing object",
                        context=(
                            DiagnosticContext("property", name),
                            DiagnosticContext("value_path", value_path),
                        ),
                    ),
                ),
            )

        for name, value_path, expected in (
            ("Value", "/Properties/Sound", "/Properties/Value/Properties/Sound"),
            ("Value", "", "/Properties/Value"),
            ("A/B", "/0", "/Properties/A~1B/0"),
        ):
            with self.subTest(name=name, value_path=value_path):
                context = MockPrimitiveContext(RuntimePhase.APPLY)
                adapter = PropertySemanticAdapter()
                admitted = single_admitted(context)
                asset = context.issue_handle()
                batch_outcome, _, _ = single_batch_outcome(context)
                context.queue_outcomes(ENUMERATE_BATCH_ID, batch_outcome)
                context.queue_outcomes(WRITE_BATCH_ID, located_failure(name, value_path))
                error = assert_error(
                    self,
                    "PropertyWrite.DecodeFailed",
                    expected,
                    lambda: adapter.apply(
                        asset, asset, {"Value": 1}, admitted, context, phase=RuntimePhase.APPLY
                    ),
                )
                self.assertIn("does not resolve", error.diagnostic.message)

    def test_unlocated_or_malformed_value_context_keeps_the_primitive_path(self) -> None:
        for extra in (
            (),
            (DiagnosticContext("property", "Value"),),
            (DiagnosticContext("property", ""), DiagnosticContext("value_path", "/0")),
            (DiagnosticContext("property", "Value"), DiagnosticContext("value_path", "0")),
            (DiagnosticContext("property", "Value"), DiagnosticContext("value_path", 3)),
        ):
            with self.subTest(extra=extra):
                context = MockPrimitiveContext(RuntimePhase.APPLY)
                adapter = PropertySemanticAdapter()
                admitted = single_admitted(context)
                asset = context.issue_handle()
                batch_outcome, _, _ = single_batch_outcome(context)
                context.queue_outcomes(ENUMERATE_BATCH_ID, batch_outcome)
                context.queue_outcomes(
                    WRITE_BATCH_ID,
                    PrimitiveOutcome(
                        PrimitiveOutcomeStatus.FAILED,
                        None,
                        (),
                        (),
                        (RuntimeDiagnostic("/Primitive", "PropertyWrite.DecodeFailed", "bad", context=extra),),
                    ),
                )
                assert_error(
                    self,
                    "PropertyWrite.DecodeFailed",
                    "/Primitive",
                    lambda: adapter.apply(
                        asset, asset, {"Value": 1}, admitted, context, phase=RuntimePhase.APPLY
                    ),
                )

    def test_batch_write_rejects_nonempty_value_or_returned_handles(self) -> None:
        for lane in ("value", "handles"):
            context = MockPrimitiveContext(RuntimePhase.APPLY)
            adapter = PropertySemanticAdapter()
            admitted = single_admitted(context)
            asset = context.issue_handle()
            batch_outcome, _, _ = single_batch_outcome(context)
            context.queue_outcomes(ENUMERATE_BATCH_ID, batch_outcome)
            returned = context.issue_handle()
            outcome = (
                success_outcome(value={"changed": True})
                if lane == "value"
                else success_outcome(
                    handle_results=(
                        NamedOpaqueHandleResult("properties", (returned,)),
                    )
                )
            )
            context.queue_outcomes(WRITE_BATCH_ID, outcome)
            with self.subTest(lane=lane):
                assert_error(
                    self,
                    "Generic.InvalidPrimitiveResult",
                    "/Properties",
                    lambda: adapter.apply(
                        asset,
                        asset,
                        {"Value": 1},
                        admitted,
                        context,
                        phase=RuntimePhase.APPLY,
                    ),
                )
                self.assertEqual(context.successful_changes, ())

    def test_mutation_rejects_phase_mismatch_and_inactive_transaction(self) -> None:
        mismatch = MockPrimitiveContext(RuntimePhase.APPLY)
        adapter, admitted, _ = enumerate_properties(mismatch)
        asset = mismatch.issue_handle()
        assert_error(
            self,
            "Generic.InvalidMutationPhase",
            "/Phase",
            lambda: adapter.apply(
                asset,
                asset,
                {"Count": 1},
                admitted,
                mismatch,
                phase=RuntimePhase.REPAIR,
            ),
        )
        inactive = MockPrimitiveContext(RuntimePhase.APPLY, active_transaction=False)
        adapter, admitted, _ = enumerate_properties(inactive)
        asset = inactive.issue_handle()
        batch_outcome, _, _ = property_batch_enumeration_outcome(
            inactive,
            provider_property_rows(admitted_property_rows()),
        )
        inactive.queue_outcomes(ENUMERATE_BATCH_ID, batch_outcome)
        inactive.queue_outcomes(WRITE_BATCH_ID, success_outcome())
        assert_error(
            self,
            "Generic.InvalidMutationPhase",
            "/Phase",
            lambda: adapter.apply(
                asset,
                asset,
                {"Count": 1},
                admitted,
                inactive,
                phase=RuntimePhase.APPLY,
            ),
        )
        self.assertEqual(
            [call.requirement.primitive_id for call in inactive.calls],
            [ENUMERATE_ID, ENUMERATE_BATCH_ID],
        )
        self.assertEqual(inactive.successful_changes, ())

    def test_extract_reads_lexically_and_omits_exact_defaults(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        adapter, admitted, _ = enumerate_properties(context)
        asset = context.issue_handle()
        values = (0, False, 1.0, "visible")
        context.queue_outcomes(READ_ID, *(success_outcome(value={"value": value}) for value in values))
        extracted = adapter.extract(asset, admitted, context, include_defaults=False)
        self.assertEqual(extracted, {"Title": "visible"})
        calls = context.calls[1:]
        self.assertEqual([call.requirement.primitive_id for call in calls], [READ_ID] * 4)
        expected_handles = [item.property_handle for item in admitted if item.writable]
        self.assertEqual([call.handle_arguments[1].handle for call in calls], expected_handles)

    def test_extract_include_defaults_and_scalar_type_distinction(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        adapter, admitted, _ = enumerate_properties(context)
        asset = context.issue_handle()
        values = (False, 0, 1, "visible")
        context.queue_outcomes(READ_ID, *(success_outcome(value={"value": value}) for value in values))
        extracted = adapter.extract(asset, admitted, context, include_defaults=False)
        self.assertEqual(extracted, {"Count": False, "Enabled": 0, "Ratio": 1, "Title": "visible"})
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        adapter, admitted, _ = enumerate_properties(context)
        asset = context.issue_handle()
        context.queue_outcomes(READ_ID, *(success_outcome(value={"value": value}) for value in values))
        self.assertEqual(len(adapter.extract(asset, admitted, context, include_defaults=True)), 4)

    def test_diff_only_extract_reads_a_struct_sparse(self) -> None:
        """A struct (or an array of structs) that differs from its default is read again with
        sparse: true (members equal to the class default's left out); Verify / Diff (include_defaults) read it whole."""
        for include_defaults, expected in ((False, [{}, {"sparse": True}]), (True, [{}])):
            context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
            adapter = PropertySemanticAdapter()
            asset = context.issue_handle()
            admitted = (AdmittedProperty("Overrides", context.issue_handle(), True, True,
                                         OptionalJsonValue(True, {"bOverride_Blend": False, "Clip": 0.3333})),)
            context.queue_outcomes(READ_ID, success_outcome(value={"value": {"bOverride_Blend": True, "Clip": 0.3333}}),
                                   success_outcome(value={"value": {"bOverride_Blend": True}}))
            extracted = adapter.extract(asset, admitted, context, include_defaults=include_defaults)
            with self.subTest(include_defaults=include_defaults):
                self.assertEqual([call.value_arguments for call in context.calls], expected)
                self.assertEqual(extracted["Overrides"], {"bOverride_Blend": True} if not include_defaults
                                 else {"bOverride_Blend": True, "Clip": 0.3333})

    def test_extract_leaves_out_an_empty_import_record(self) -> None:
        """diff_only leaves out an AssetImportData without a source file (the asset makes one on creation); Verify
        and Diff (include_defaults) still read it, and one with a source file or another class is kept."""
        from types import SimpleNamespace
        from asset_document.regions import property_semantics

        item = SimpleNamespace(name="AssetImportData", default_value=SimpleNamespace(is_set=False, value=None))
        empty = {"Class": "/Script/Engine.AssetImportData", "Properties": {}}
        include = property_semantics._include_extracted_value
        self.assertFalse(include(empty, item, False))
        self.assertTrue(include(empty, item, True))
        self.assertTrue(include({**empty, "Properties": {"SourceData": {"SourceFiles": [{"RelativeFilename": "a.wav"}]}}}, item, False))
        self.assertTrue(include({**empty, "Class": "/Script/UnrealEd.FbxStaticMeshImportData"}, item, False))
        self.assertTrue(include(empty, SimpleNamespace(name="Other", default_value=item.default_value), False))

    def test_extract_omits_non_writable_properties_even_when_non_default(self) -> None:
        # Apply rejects non-writable properties, so Extract must not emit them.
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        adapter, admitted, _ = enumerate_properties(context)
        self.assertIn("ReadOnly", [item.name for item in admitted if not item.writable])
        asset = context.issue_handle()
        values = (0, False, 1.0, "visible")
        context.queue_outcomes(READ_ID, *(success_outcome(value={"value": value}) for value in values))
        extracted = adapter.extract(asset, admitted, context, include_defaults=True)
        self.assertNotIn("ReadOnly", extracted)
        read_handles = [call.handle_arguments[1].handle for call in context.calls[1:]]
        read_only = next(item for item in admitted if item.name == "ReadOnly")
        self.assertNotIn(read_only.property_handle, read_handles)

    def test_raw_asset_and_property_handle_substitutes_are_rejected(self) -> None:
        substitutes = ("asset", b"asset", object(), None, True, 1, 1.0, [], {})
        for substitute in substitutes:
            context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
            adapter = PropertySemanticAdapter()
            valid_asset = context.issue_handle()
            valid = single_admitted(context)
            forged = (AdmittedProperty("Value", substitute, True, True, OptionalJsonValue()),)
            with self.subTest(substitute=type(substitute).__name__, lane="asset"):
                assert_error(
                    self,
                    "PrimitiveCatalog.InvalidOpaqueHandle",
                    "/HandleArguments",
                    lambda substitute=substitute: adapter.preflight(substitute, {"Value": 1}, valid, context),
                )
            with self.subTest(substitute=type(substitute).__name__, lane="property"):
                assert_error(
                    self,
                    "PrimitiveCatalog.InvalidOpaqueHandle",
                    "/HandleArguments",
                    lambda: adapter.preflight(valid_asset, {"Value": 1}, forged, context),
                )

    def test_cross_context_asset_and_property_handles_are_rejected(self) -> None:
        source = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        foreign_asset = source.issue_handle()
        foreign_properties = single_admitted(source)
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        adapter = PropertySemanticAdapter()
        local_asset = context.issue_handle()
        for asset, admitted in ((foreign_asset, single_admitted(context)), (local_asset, foreign_properties)):
            with self.subTest(asset_is_foreign=asset is foreign_asset):
                assert_error(
                    self,
                    "PrimitiveCatalog.InvalidOpaqueHandle",
                    "/HandleArguments",
                    lambda asset=asset, admitted=admitted: adapter.preflight(
                        asset, {"Value": 1}, admitted, context
                    ),
                )

    def test_test_proxy_is_noncopyable_nonserializable_and_not_json(self) -> None:
        handle = MockPrimitiveContext(RuntimePhase.PREFLIGHT).issue_handle()
        for callback in (
            lambda: copy.copy(handle),
            lambda: copy.deepcopy(handle),
            lambda: pickle.dumps(handle),
            lambda: json.dumps(handle),
        ):
            with self.subTest(callback=callback):
                assert_type_error(self, callback)

    def test_same_context_expired_proxy_fails_before_any_success_progress(self) -> None:
        cases = (
            ("preflight", RuntimePhase.PREFLIGHT),
            ("read", RuntimePhase.PREFLIGHT),
            ("write", RuntimePhase.APPLY),
        )
        for operation, phase in cases:
            context = MockPrimitiveContext(phase)
            adapter = PropertySemanticAdapter()
            admitted = single_admitted(context)
            asset = context.issue_handle()
            primitive_id = {"preflight": PREFLIGHT_ID, "read": READ_ID, "write": WRITE_ID}[operation]
            context.queue_outcomes(primitive_id, success_outcome(changed_set=(changed_record("Value"),)))
            context.expire()
            with self.subTest(operation=operation):
                assert_error(
                    self,
                    "PrimitiveCatalog.InvalidOpaqueHandle",
                    "/HandleArguments",
                    lambda operation=operation: run_stale_operation(
                        operation, adapter, asset, admitted, context
                    ),
                )
                self.assertEqual((context.handler_calls, context.calls, context.successful_changes), (0, (), ()))

    def test_changed_set_in_value_is_rejected_and_never_decoded(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.APPLY)
        adapter = PropertySemanticAdapter()
        admitted = single_admitted(context)
        asset = context.issue_handle()
        batch_outcome, _, _ = single_batch_outcome(context)
        context.queue_outcomes(ENUMERATE_BATCH_ID, batch_outcome)
        context.queue_outcomes(
            WRITE_BATCH_ID,
            success_outcome(value={"changed_set": [{"path": "/forged"}]}),
        )
        assert_error(
            self,
            "Generic.InvalidPrimitiveResult",
            "/Properties",
            lambda: adapter.apply(
                asset,
                asset,
                {"Value": 1},
                admitted,
                context,
                phase=RuntimePhase.APPLY,
            ),
        )
        self.assertEqual(context.successful_changes, ())

    def test_malformed_enumerate_preflight_read_and_write_outcomes_fail_closed(self) -> None:
        operations = ("enumerate", "preflight", "read", "write")
        for operation in operations:
            phase = RuntimePhase.APPLY if operation == "write" else RuntimePhase.PREFLIGHT
            context = MockPrimitiveContext(phase)
            adapter = PropertySemanticAdapter()
            admitted = single_admitted(context)
            asset = context.issue_handle()
            primitive_id = {
                "enumerate": ENUMERATE_ID,
                "preflight": PREFLIGHT_ID,
                "read": READ_ID,
                "write": WRITE_BATCH_ID,
            }[operation]
            if operation == "write":
                batch_outcome, _, _ = single_batch_outcome(context)
                context.queue_outcomes(ENUMERATE_BATCH_ID, batch_outcome)
            context.queue_outcomes(primitive_id, malformed_outcome(diagnostics=[]))
            with self.subTest(operation=operation):
                assert_error(
                    self,
                    "Generic.InvalidPrimitiveResult",
                    "/Properties" if operation in ("enumerate", "write") else "/Properties/Value",
                    lambda operation=operation: run_malformed_operation(
                        operation, adapter, asset, admitted, context
                    ),
                )

    def test_outcome_snapshots_value_and_diagnostic_context_sources(self) -> None:
        value_source = {"value": {"Nested": [1]}}
        diagnostic_source = {"Nested": [1]}
        typed_diagnostic = RuntimeDiagnostic(
            "/Properties/Value",
            "Primitive.Warning",
            "warning",
            context=(DiagnosticContext("payload", diagnostic_source),),
        )
        outcome = success_outcome(value=value_source, diagnostics=(typed_diagnostic,))
        value_source["value"]["Nested"].append(2)
        diagnostic_source["Nested"].append(2)
        self.assertEqual(outcome.value, {"value": {"Nested": [1]}})
        self.assertEqual(outcome.diagnostics[0].context[0].value, {"Nested": [1]})
        assert_error(
            self,
            "PrimitiveCatalog.InvalidOutcome",
            "/PrimitiveOutcome",
            lambda: PrimitiveOutcome(
                PrimitiveOutcomeStatus.SUCCEEDED,
                object(),
                (),
                (),
                (),
            ),
        )


class IntegralNumberPropertyTests(unittest.TestCase):
    """Extract output re-applies: UE JSON writes 1.0 as 1, so 1 and 1.0 are one value."""

    def test_extract_writes_integral_floats_as_integers(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        adapter, admitted, _ = enumerate_properties(context)
        asset = context.issue_handle()
        values = (1.0, False, 0.0, {"X": 2.0, "Y": 0.5})
        context.queue_outcomes(READ_ID, *(success_outcome(value={"value": value}) for value in values))
        extracted = adapter.extract(asset, admitted, context, include_defaults=True)
        self.assertEqual(extracted["Count"], 1)
        self.assertIs(type(extracted["Count"]), int)
        self.assertIs(type(extracted["Ratio"]), int)
        self.assertEqual(extracted["Title"], {"X": 2, "Y": 0.5})
        self.assertIs(type(extracted["Title"]["Y"]), float)

    def test_canonicalize_accepts_either_spelling(self) -> None:
        adapter = PropertySemanticAdapter()
        self.assertEqual(
            json.dumps(adapter.canonicalize({"Ratio": 1.0, "Count": 3, "Nested": [2.0, 0.25]})),
            json.dumps({"Ratio": 1, "Count": 3, "Nested": [2, 0.25]}),
        )

    def test_preflight_accepts_integer_literal_for_float_property(self) -> None:
        for authored, stored in ((1, 1.0), (1.0, 1.0), ({"X": 0}, {"X": 0.0}), ([3, 4.5], [3.0, 4.5])):
            with self.subTest(authored=authored):
                context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
                adapter, admitted, _ = enumerate_properties(context)
                context.queue_outcomes(
                    PREFLIGHT_ID,
                    success_outcome(value={"canonical_value": stored, "feasible": True}),
                )
                adapter.preflight(context.issue_handle(), {"Ratio": authored}, admitted, context)
                self.assertEqual(context.calls[1].value_arguments, {"value": authored})

    def test_preflight_rejects_value_the_editor_rewrites(self) -> None:
        # e.g. a PostEditChange clamp; float precision itself is normalised in C++
        # before Validate (AssetDocumentEditorFloatPrecision), so it never reaches here.
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        adapter, admitted, _ = enumerate_properties(context)
        context.queue_outcomes(
            PREFLIGHT_ID,
            success_outcome(value={"canonical_value": 1.0, "feasible": True}),
        )
        error = assert_error(
            self,
            "Generic.PropertyValueNotStoredExactly",
            "/Properties/Ratio",
            lambda: adapter.preflight(context.issue_handle(), {"Ratio": 5.5}, admitted, context),
        )
        self.assertIn("the asset would hold 1 instead of 5.5", error.diagnostic.message)

    def test_preflight_rejection_names_first_differing_sub_path_and_count(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.PREFLIGHT)
        adapter, admitted, _ = enumerate_properties(context)
        authored = {"Keys": [{"Time": 0, "Value": 1}] * 3 + [{"Time": 3, "Value": 2}, {"Time": 4, "Value": 5}],
                    "Note": "x" * 400}
        stored = {"Keys": [{"Time": 0, "Value": 1}] * 3 + [{"Time": 3, "Value": 3.4e38}, {"Time": 4, "Value": 3.4e38}],
                  "Note": "x" * 400}
        context.queue_outcomes(
            PREFLIGHT_ID,
            success_outcome(value={"canonical_value": stored, "feasible": True}),
        )
        error = assert_error(
            self,
            "Generic.PropertyValueNotStoredExactly",
            "/Properties/Ratio/Keys/3/Value",
            lambda: adapter.preflight(context.issue_handle(), {"Ratio": authored}, admitted, context),
        )
        message = error.diagnostic.message
        self.assertIn("at /Properties/Ratio/Keys/3/Value the asset would hold 3.4e+38 instead of 2", message)
        self.assertIn("(2 values differ; the first is shown)", message)
        self.assertNotIn("x" * 50, message)


if __name__ == "__main__":
    unittest.main()
