# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""CurveFloat through the class-neutral generic factory lifecycle.

No CurveFloat-specific C++ exists: the generic candidate creates it with
``editor.scratch.create_by_factory.v1`` / ``editor.asset.create_by_factory.v2``
(the editor selects ``UCurveFloatFactory``) in the same apply that writes its
admitted properties; the Python-only ``profiles.curve_float`` component declares
``FloatCurve``/``bIsEventCurve`` and maps the curve to a readable key list.
"""
from __future__ import annotations

import unittest

import asset_document.bootstrap as bootstrap_module
from asset_document.diagnostics import AssetDocumentContractError
from asset_document.diff.canonical_diff import canonical_equal
from asset_document.profiles.curve_float import (
    CURVE_FLOAT_PROPERTIES,
    CURVE_LINEAR_COLOR_PROPERTIES,
    CURVE_VECTOR_PROPERTIES,
    RichCurveChannelsCodec,
    RichCurveCodec,
)
from asset_document.profiles.generic_components import generic_property_component
from asset_document.tests.fixtures.curve_float_cases import (
    CURVE_FLOAT_CANONICAL_KEYS,
    CURVE_FLOAT_CLASS,
    CURVE_FLOAT_KEYS,
    CURVE_FLOAT_NATIVE_CURVE,
    CURVE_FLOAT_OBJECT_PATH,
    CURVE_FLOAT_TARGET,
    curve_float_document,
)
from asset_document.tests.test_generic_runtime import (
    NativeContext,
    NativeHandle,
    diagnostic,
    handle_group,
    outcome,
)


def _request(document: dict, operation: int = 5) -> dict:
    return {
        "abi_version": 1,
        "request_id": "00000000-0000-0000-0000-0000000000cf",
        "operation": operation,
        "target_asset_path": CURVE_FLOAT_OBJECT_PATH,
        "requested_profile_class_path": CURVE_FLOAT_CLASS,
        "requested_asset_class_path": CURVE_FLOAT_CLASS,
        "canonical_document": document,
        "deadline_unix_milliseconds": 0,
        "cancellation": {"sequence": 0, "requested": False},
        "expected_runtime_revision": 17,
    }


def _plain(value: object) -> object:
    """Native projection arrays are tuples; compare as plain JSON lists."""
    if type(value) in (list, tuple):
        return [_plain(item) for item in value]
    if type(value) is dict:
        return {key: _plain(item) for key, item in value.items()}
    return value


def _context(phase: int) -> NativeContext:
    context = NativeContext(phase, request_id="00000000-0000-0000-0000-0000000000cf")
    context.queue(
        "editor.class.resolve_canonical.v1",
        outcome(value={"canonical_class": CURVE_FLOAT_CLASS},
                handle_results=(handle_group("class", NativeHandle()),)),
    )
    context.queue(
        "editor.asset.resolve_existing.v1",
        outcome(
            value={"exists": False, "canonical_path": None, "canonical_class": None,
                   "package_name": None, "package_dirty": False},
            handle_results=(handle_group("asset_read"), handle_group("asset_mutable"), handle_group("package")),
        ),
    )
    return context


def _queue_created(context: NativeContext) -> None:
    context.queue(
        "editor.asset.create_by_factory.v2",
        outcome(handle_results=(handle_group("asset_read", NativeHandle()),
                                handle_group("asset_mutable", NativeHandle()),
                                handle_group("package", NativeHandle()))),
    )
    context.queue("editor.property.write_admitted_batch.v1", outcome(changed_set=()))


def _queue_no_admitted_properties(context: NativeContext) -> None:
    # CurveFloat's curve data is serialized but not Details-editable (no CPF_Edit).
    context.queue(
        "editor.property.enumerate_admitted.v1",
        outcome(value={"properties": ()}, handle_results=(handle_group("properties"),)),
    )
    context.queue(
        "editor.property.enumerate_admitted_batch.v1",
        outcome(value={"properties": ()},
                handle_results=(handle_group("properties"), handle_group("property_batch", NativeHandle()))),
    )


class CurveFloatGenericFactoryTests(unittest.TestCase):
    def test_create_preflight_uses_the_generic_factory_scratch(self) -> None:
        context = _context(1)
        context.queue(
            "editor.scratch.create_by_factory.v1",
            outcome(value={"canonical_class": CURVE_FLOAT_CLASS, "lifecycle_id": "factory_generic.v1"},
                    handle_results=(handle_group("scratch", NativeHandle()),)),
        )
        _queue_no_admitted_properties(context)
        result = bootstrap_module.dispatch(_request(curve_float_document()), context)
        self.assertEqual(result["terminal_status"], 0, result["diagnostics"])
        scratch = next(call for call in context.calls if call[0] == "editor.scratch.create_by_factory.v1")
        self.assertEqual((scratch[1], scratch[2], [item["name"] for item in scratch[3]]), (1, {}, ["asset_class"]))
        self.assertFalse(any(call[0].startswith("editor.asset.create_by_factory") for call in context.calls))

    def test_one_apply_creates_through_the_generic_factory(self) -> None:
        context = _context(2)
        _queue_created(context)
        _queue_no_admitted_properties(context)
        result = bootstrap_module.dispatch(_request(curve_float_document()), context)
        self.assertEqual(result["terminal_status"], 0, result["diagnostics"])
        create = next(call for call in context.calls if call[0] == "editor.asset.create_by_factory.v2")
        self.assertEqual(create[1], 2)
        self.assertEqual(create[2], {"package_path": "/Game/Curves", "asset_name": "Curve_Generic"})
        self.assertEqual([item["name"] for item in create[3]], ["asset_class"])
        self.assertFalse(any(call[0] == "editor.asset.create_by_factory.v1" for call in context.calls))

    def test_factory_rejection_is_reported_before_creation(self) -> None:
        context = _context(1)
        context.queue(
            "editor.scratch.create_by_factory.v1",
            outcome(status=2, diagnostics=(diagnostic("FactoryCreate.NoFactory"),)),
        )
        result = bootstrap_module.dispatch(_request(curve_float_document()), context)
        self.assertEqual(result["diagnostics"][0]["code"], "FactoryCreate.NoFactory")

    def test_curve_float_component_is_registered(self) -> None:
        component = generic_property_component(CURVE_FLOAT_CLASS)
        self.assertIs(component, CURVE_FLOAT_PROPERTIES)
        self.assertEqual(component.admission.names, ("FloatCurve", "bIsEventCurve"))

    def test_rich_curve_codec_round_trips_readable_keys(self) -> None:
        codec = RichCurveCodec()
        native = codec.from_document(CURVE_FLOAT_KEYS["FloatCurve"], "/Properties/FloatCurve")
        self.assertTrue(canonical_equal(native, CURVE_FLOAT_NATIVE_CURVE), native)
        document = codec.to_document(native, "/Properties/FloatCurve")
        self.assertTrue(canonical_equal(document, CURVE_FLOAT_CANONICAL_KEYS["FloatCurve"]), document)
        self.assertTrue(canonical_equal(codec.from_document(document, "/Properties/FloatCurve"), native))

    def test_unset_default_value_is_omitted(self) -> None:
        codec = RichCurveCodec()
        native = codec.from_document({"Keys": []}, "/Properties/FloatCurve")
        self.assertEqual(
            codec.to_document(native, "/Properties/FloatCurve"),
            {"Keys": [], "PreInfinityExtrap": "Constant", "PostInfinityExtrap": "Constant"},
        )

    def test_invalid_curves_have_stable_codes(self) -> None:
        cases = {
            "CurveFloat.KeysNotAscending": {"Keys": [{"Time": 1, "Value": 0}, {"Time": 1, "Value": 1}]},
            "CurveFloat.UnknownEnumName": {"Keys": [{"Time": 0, "Value": 0, "InterpMode": "RCIM_Cubic"}]},
            "CurveFloat.InvalidKey": {"Keys": [{"Time": 0}]},
            "CurveFloat.UnknownField": {"Keys": [], "Extrapolation": "Cycle"},
            "CurveFloat.InvalidNumber": {"Keys": [{"Time": 0, "Value": 1e39}]},
            "CurveFloat.InvalidCurve": {"Keys": {"Time": 0, "Value": 0}},
        }
        for code, curve in cases.items():
            with self.subTest(code=code):
                with self.assertRaises(AssetDocumentContractError) as raised:
                    RichCurveCodec().from_document(curve, "/Properties/FloatCurve")
                self.assertEqual(raised.exception.diagnostic.code, code)
        with self.assertRaises(AssetDocumentContractError) as raised:
            RichCurveCodec().from_document({"Keys": [{"Time": float("nan"), "Value": 0}]}, "/Properties/FloatCurve")
        self.assertEqual(raised.exception.diagnostic.code, "CurveFloat.InvalidNumber")

    def test_vector_and_color_curves_are_registered(self) -> None:
        self.assertIs(generic_property_component("/Script/Engine.CurveVector"), CURVE_VECTOR_PROPERTIES)
        self.assertIs(generic_property_component("/Script/Engine.CurveLinearColor"), CURVE_LINEAR_COLOR_PROPERTIES)
        self.assertEqual(CURVE_LINEAR_COLOR_PROPERTIES.admission.names, ("FloatCurves",))

    def test_channel_curves_are_keyed_by_channel_and_omitted_channels_keep_their_curve(self) -> None:
        codec = RichCurveChannelsCodec(("R", "G", "B", "A"))
        native = codec.from_document({"G": CURVE_FLOAT_KEYS["FloatCurve"]}, "/Properties/FloatCurves")
        self.assertEqual(len(native), 4)
        self.assertTrue(canonical_equal(native[1], CURVE_FLOAT_NATIVE_CURVE), native[1])
        # R, B and A: a struct edit with every member omitted keeps the channel's curve.
        self.assertEqual((native[0], native[2], native[3]), ({}, {}, {}))
        document = codec.to_document(native, "/Properties/FloatCurves")
        self.assertEqual(list(document), ["G"])
        self.assertTrue(canonical_equal(document["G"], CURVE_FLOAT_CANONICAL_KEYS["FloatCurve"]), document)
        self.assertEqual(codec.to_document(codec.from_document({}, "/P"), "/P"), {})

    def test_channel_curves_clear_an_explicit_empty_channel_and_read_every_channel(self) -> None:
        codec = RichCurveChannelsCodec(("X", "Y", "Z"))
        empty = {"Keys": [], "PreInfinityExtrap": "Constant", "PostInfinityExtrap": "Constant"}
        for cleared in ({}, {"Keys": []}):
            with self.subTest(cleared=cleared):
                native = codec.from_document({"X": cleared}, "/Properties/FloatCurves")
                self.assertEqual(native[0]["Keys"], [])
                self.assertEqual((native[1], native[2]), ({}, {}))
                self.assertEqual(codec.to_document(native, "/Properties/FloatCurves"), {"X": empty})
        live = [RichCurveCodec().from_document({"Keys": []}, "/P"), CURVE_FLOAT_NATIVE_CURVE,
                RichCurveCodec().from_document({"Keys": []}, "/P")]
        self.assertEqual(list(codec.to_document(live, "/Properties/FloatCurves")), ["X", "Y", "Z"])

    def test_observed_channels_are_compared_only_where_the_document_names_them(self) -> None:
        from asset_document.regions.property_semantics import _require_preflight_result
        codec = RichCurveChannelsCodec(("R", "G", "B", "A"))
        authored = codec.from_document({"G": CURVE_FLOAT_KEYS["FloatCurve"]}, "/Properties/FloatCurves")
        kept = RichCurveCodec().from_document({"Keys": [{"Time": 0, "Value": 1}]}, "/P")
        observed = [kept, CURVE_FLOAT_NATIVE_CURVE, kept, kept]
        self.assertEqual(codec.shape_like(authored, observed), [{}, CURVE_FLOAT_NATIVE_CURVE, {}, {}])
        _require_preflight_result({"canonical_value": observed, "feasible": True}, authored,
                                  "/Properties/FloatCurves", codec)
        with self.assertRaises(AssetDocumentContractError) as raised:
            _require_preflight_result({"canonical_value": observed, "feasible": True}, authored,
                                      "/Properties/FloatCurves")
        self.assertEqual(raised.exception.diagnostic.code, "Generic.PropertyValueNotStoredExactly")
        changed = [kept, kept, kept, kept]
        with self.assertRaises(AssetDocumentContractError):
            _require_preflight_result({"canonical_value": changed, "feasible": True}, authored,
                                      "/Properties/FloatCurves", codec)

    def test_saved_keys_at_one_time_read_with_a_warning_and_new_ones_are_refused(self) -> None:
        from asset_document import tolerance
        from asset_document.diagnostics import collecting_read_warnings
        curve = {"Keys": [{"Time": 0, "Value": 0}, {"Time": 0, "Value": 0.25}, {"Time": 1, "Value": 1}]}
        with collecting_read_warnings() as warnings, tolerance.reading_live():
            native = RichCurveCodec().from_document(curve, "/Properties/FloatCurves/R")
        self.assertEqual(len(native["Keys"]), 3)
        self.assertEqual([(item.code, item.path) for item in warnings],
                         [("CurveFloat.KeysNotAscending", "/Properties/FloatCurves/R/Keys/1/Time")])
        live = {"Properties": {"FloatCurves": {"R": curve}}}
        with tolerance.authoring() as findings:
            RichCurveCodec().from_document(curve, "/Properties/FloatCurves/R")
        tolerance.settle(findings, live, live)
        added = {"Keys": [{"Time": 0, "Value": 0}, {"Time": 1, "Value": 1}, {"Time": 0.5, "Value": 0.5}]}
        sorted_live = {"Properties": {"FloatCurves": {"R": {"Keys": [{"Time": 0, "Value": 0}, {"Time": 1, "Value": 1}]}}}}
        with tolerance.authoring() as findings:
            RichCurveCodec().from_document(added, "/Properties/FloatCurves/R")
        with self.assertRaises(AssetDocumentContractError) as raised:
            tolerance.settle(findings, {"Properties": {"FloatCurves": {"R": added}}}, sorted_live)
        self.assertEqual(raised.exception.diagnostic.code, "CurveFloat.KeysNotAscending")

    def test_channel_curves_refuse_unknown_channels_and_bad_curves(self) -> None:
        codec = RichCurveChannelsCodec(("X", "Y", "Z"))
        with self.assertRaises(AssetDocumentContractError) as raised:
            codec.from_document({"W": {"Keys": []}}, "/Properties/FloatCurves")
        self.assertEqual(raised.exception.diagnostic.code, "CurveFloat.UnknownField")
        with self.assertRaises(AssetDocumentContractError) as raised:
            codec.from_document({"Y": {"Keys": [{"Time": 1, "Value": 0}, {"Time": 0, "Value": 0}]}}, "/Properties/FloatCurves")
        self.assertEqual(raised.exception.diagnostic.path, "/Properties/FloatCurves/Y/Keys/1/Time")
        with self.assertRaises(AssetDocumentContractError):
            codec.to_document([], "/Properties/FloatCurves")

    def test_invalid_curve_is_rejected_before_any_creation(self) -> None:
        context = _context(1)
        document = curve_float_document({"FloatCurve": {"Keys": ({"Time": 2, "Value": 0}, {"Time": 1, "Value": 0})}})
        result = bootstrap_module.dispatch(_request(document), context)
        self.assertEqual(result["diagnostics"][0]["code"], "CurveFloat.KeysNotAscending")
        self.assertFalse(any("create_by_factory" in call[0] for call in context.calls))

    def test_one_apply_creates_and_writes_the_declared_curve(self) -> None:
        context = _context(2)
        context.queue(
            "editor.asset.create_by_factory.v2",
            outcome(handle_results=(handle_group("asset_read", NativeHandle()),
                                    handle_group("asset_mutable", NativeHandle()),
                                    handle_group("package", NativeHandle()))),
        )
        rows = (
            {"name": "FloatCurve", "declaring_class": CURVE_FLOAT_CLASS, "codec_id": "struct.v1",
             "readable": True, "writable": True},
            {"name": "bIsEventCurve", "declaring_class": CURVE_FLOAT_CLASS, "codec_id": "bool.v1",
             "readable": True, "writable": True, "default": False},
        )
        context.queue(
            "editor.property.enumerate_admitted.v1",
            outcome(value={"properties": rows},
                    handle_results=(handle_group("properties", NativeHandle(), NativeHandle()),)),
        )
        context.queue(
            "editor.property.enumerate_admitted_batch.v1",
            outcome(value={"properties": rows},
                    handle_results=(handle_group("properties", NativeHandle(), NativeHandle()),
                                    handle_group("property_batch", NativeHandle()))),
        )
        context.queue("editor.property.write_admitted_batch.v1", outcome(changed_set=()))
        result = bootstrap_module.dispatch(_request(curve_float_document(CURVE_FLOAT_KEYS)), context)
        self.assertEqual(result["terminal_status"], 0, result["diagnostics"])
        declared = ["FloatCurve", "bIsEventCurve"]
        for primitive in ("editor.property.enumerate_admitted.v1", "editor.property.enumerate_admitted_batch.v1"):
            call = next(call for call in context.calls if call[0] == primitive)
            self.assertEqual(list(call[2]["admit_properties"]), declared)
        write = next(call for call in context.calls if call[0] == "editor.property.write_admitted_batch.v1")
        self.assertEqual(list(write[2]["admit_properties"]), declared)
        values = {row["name"]: row["value"] for row in write[2]["properties"]}
        self.assertTrue(canonical_equal(_plain(values["FloatCurve"]), CURVE_FLOAT_NATIVE_CURVE), values["FloatCurve"])
        self.assertIs(values["bIsEventCurve"], True)


if __name__ == "__main__":
    unittest.main()
