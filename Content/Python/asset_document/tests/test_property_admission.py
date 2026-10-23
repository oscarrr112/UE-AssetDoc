# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Profile-declared property admission and value codecs (class-neutral).

A ``GenericPropertyComponent`` declares serialized non-edit properties of one
exact class; the names travel to the Editor provider as ``admit_properties`` on
every property primitive, and value codecs map document values to native typed
values and back.
"""
from __future__ import annotations

import unittest
from unittest import mock

import asset_document.bootstrap as bootstrap_module
from asset_document.diagnostics import AssetDocumentContractError, contract_error
from asset_document.profiles import generic as generic_module
from asset_document.profiles.generic_components import _index
from asset_document.regions.property_semantics import (
    GenericPropertyComponent,
    PropertySemanticAdapter,
    admit_properties,
)
from asset_document.tests.test_generic_runtime import (
    NativeContext,
    NativeHandle,
    handle_group,
    outcome,
)

CLASS = "/Script/AssetDocumentTests.TestDataAsset"
TARGET = "/Game/Admission/DA_Declared"
OBJECT_PATH = f"{TARGET}.DA_Declared"
REQUEST_ID = "00000000-0000-0000-0000-0000000000ad"


class WrappedCodec:
    """Document {"Wrapped": n} <-> native n."""

    def from_document(self, value, path):
        if type(value) is not dict or set(value) != {"Wrapped"} or type(value["Wrapped"]) is not int:
            raise contract_error(path, "Test.InvalidWrapped", "expected {\"Wrapped\": int}")
        return value["Wrapped"]

    def to_document(self, value, path):
        return {"Wrapped": value}


COMPONENT = GenericPropertyComponent(
    admit_properties(CLASS, ("Hidden",)),
    {"Hidden": WrappedCodec()},
)


def _request(properties: dict) -> dict:
    return {
        "abi_version": 1,
        "request_id": REQUEST_ID,
        "operation": 5,
        "target_asset_path": OBJECT_PATH,
        "requested_profile_class_path": CLASS,
        "requested_asset_class_path": CLASS,
        "canonical_document": {
            "SchemaVersion": 1,
            "AssetType": "GenericAsset",
            "Target": TARGET,
            "Class": CLASS,
            "Action": "Update",
            "Properties": properties,
        },
        "deadline_unix_milliseconds": 0,
        "cancellation": {"sequence": 0, "requested": False},
        "expected_runtime_revision": 17,
    }


def _rows() -> tuple[dict, ...]:
    return (
        {"name": "Hidden", "declaring_class": CLASS, "codec_id": "integer.v1",
         "readable": True, "writable": True, "default": 0},
    )


def _apply_context() -> NativeContext:
    context = NativeContext(2, request_id=REQUEST_ID)
    context.queue(
        "editor.class.resolve_canonical.v1",
        outcome(value={"canonical_class": CLASS}, handle_results=(handle_group("class", NativeHandle()),)),
    )
    context.queue(
        "editor.asset.resolve_existing.v1",
        outcome(
            value={"exists": True, "canonical_path": OBJECT_PATH, "canonical_class": CLASS,
                   "package_name": TARGET, "package_dirty": False},
            handle_results=(handle_group("asset_read", NativeHandle()),
                            handle_group("asset_mutable", NativeHandle()),
                            handle_group("package", NativeHandle())),
        ),
    )
    context.queue(
        "editor.property.enumerate_admitted.v1",
        outcome(value={"properties": _rows()}, handle_results=(handle_group("properties", NativeHandle()),)),
    )
    context.queue(
        "editor.property.enumerate_admitted_batch.v1",
        outcome(value={"properties": _rows()},
                handle_results=(handle_group("properties", NativeHandle()),
                                handle_group("property_batch", NativeHandle()))),
    )
    context.queue("editor.property.write_admitted_batch.v1", outcome(changed_set=()))
    return context


class PropertyAdmissionContractTests(unittest.TestCase):
    def test_admission_requires_unique_names_of_an_exact_class(self) -> None:
        for cls, names in (("CurveFloat", ("A",)), (CLASS, ()), (CLASS, ("A", "A")), (CLASS, ("",))):
            with self.subTest(cls=cls, names=names):
                with self.assertRaises(AssetDocumentContractError) as raised:
                    admit_properties(cls, names)
                self.assertEqual(raised.exception.diagnostic.code, "Generic.InvalidPropertyAdmission")

    def test_codecs_may_only_cover_declared_names(self) -> None:
        with self.assertRaises(AssetDocumentContractError) as raised:
            GenericPropertyComponent(admit_properties(CLASS, ("Hidden",)), {"Other": WrappedCodec()})
        self.assertEqual(raised.exception.diagnostic.code, "Generic.InvalidPropertyAdmission")

    def test_components_are_unique_per_class(self) -> None:
        with self.assertRaises(AssetDocumentContractError):
            _index((COMPONENT, COMPONENT))
        self.assertIs(_index((COMPONENT,))[CLASS], COMPONENT)

    def test_adapter_canonicalizes_through_its_codecs(self) -> None:
        adapter = PropertySemanticAdapter(COMPONENT)
        self.assertEqual(adapter.declared_names, ("Hidden",))
        self.assertEqual(adapter.to_native({"Hidden": {"Wrapped": 4}, "Plain": 1}), {"Hidden": 4, "Plain": 1})
        self.assertEqual(adapter.to_document("Hidden", 4), {"Wrapped": 4})
        self.assertEqual(adapter.to_document("Plain", 4), 4)
        self.assertEqual(PropertySemanticAdapter().declared_names, ())

    def test_component_mismatch_with_the_profile_class_is_rejected(self) -> None:
        with self.assertRaises(AssetDocumentContractError) as raised:
            generic_module.GenericProfileCandidate("/Script/Engine.CurveFloat", COMPONENT)
        self.assertEqual(raised.exception.diagnostic.code, "Generic.ClassMismatch")


class PropertyAdmissionRuntimeTests(unittest.TestCase):
    def test_declared_names_travel_on_every_property_primitive(self) -> None:
        context = _apply_context()
        with mock.patch.object(generic_module, "generic_property_component", return_value=COMPONENT):
            result = bootstrap_module.dispatch(_request({"Hidden": {"Wrapped": 7}}), context)
        self.assertEqual(result["terminal_status"], 0, result["diagnostics"])
        for primitive in (
            "editor.property.enumerate_admitted.v1",
            "editor.property.enumerate_admitted_batch.v1",
            "editor.property.write_admitted_batch.v1",
        ):
            call = next(call for call in context.calls if call[0] == primitive)
            self.assertEqual(list(call[2]["admit_properties"]), ["Hidden"], primitive)
        write = next(call for call in context.calls if call[0] == "editor.property.write_admitted_batch.v1")
        self.assertEqual([(row["name"], row["value"]) for row in write[2]["properties"]], [("Hidden", 7)])

    def test_without_a_component_no_declaration_is_sent(self) -> None:
        context = _apply_context()
        with mock.patch.object(generic_module, "generic_property_component", return_value=None):
            bootstrap_module.dispatch(_request({"Hidden": 7}), context)
        for call in context.calls:
            if call[0].startswith("editor.property."):
                self.assertNotIn("admit_properties", call[2], call[0])


if __name__ == "__main__":
    unittest.main()
