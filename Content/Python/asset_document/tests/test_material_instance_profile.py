# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""MaterialInstanceConstant Profile: document validation, value canonical forms, op planning,
Extract shape, Diff and primitive-path mapping.  The editor-side behaviour (parameter discovery,
UpdateStaticPermutation, parent change, rollback) is covered end to end by
UEAssetDoc.MaterialInstance.* in the editor."""
from __future__ import annotations

import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.profiles import material_instance as mi

TARGET = "/Game/Materials/MI_Rock"
PARENT = "/Game/Materials/M_Base.M_Base"


def _document(body, properties=None, action="CreateOrUpdate"):
    return {"SchemaVersion": 1, "Target": TARGET, "Class": mi.MATERIAL_INSTANCE_CONSTANT_CLASS, "Action": action,
            "Properties": properties or {}, "Body": body}


def _row(name, kind, value, overridden=True, association="global", index=-1):
    return {"name": name, "kind": kind, "association": association, "index": index, "group": "", "sort_priority": 0,
            "description": "", "asset": "", "overridden": overridden, "value": value, "default": value}


def _live(parameters, parent=PARENT, layers=None):
    return {"parent": parent, "base_material": PARENT, "has_static_permutation": False, "stale": [], "layers": layers,
            "parameters": parameters}


LIVE = _live([
    _row("Roughness", "scalar", 0.4),
    _row("BaseColor", "vector", [1, 0, 0, 1]),
    _row("Albedo", "texture", "/Game/T_A.T_A", overridden=False),
    _row("UseDetail", "static_switch", True),
    _row("Tiling", "scalar", 4, association="layer", index=1),
    _row("Mask", "scalar", 0.5, overridden=False, association="blend", index=1),
])


class DocumentTests(unittest.TestCase):
    def _error(self, body, properties=None):
        with self.assertRaises(AssetDocumentContractError) as raised:
            mi.canonicalize(_document(body, properties))
        return raised.exception.diagnostic

    def test_values_have_canonical_forms(self):
        self.assertEqual(mi.canonical_value({"R": 1, "G": 0.5, "B": 0, "A": 1}, "/p"), [1, 0.5, 0, 1])
        self.assertEqual(mi.canonical_value({"R": True, "G": False, "B": False, "A": True}, "/p"),
                         {"R": True, "G": False, "B": False, "A": True})
        self.assertEqual(mi.canonical_value({"Font": "/Game/F.F"}, "/p"), {"Font": "/Game/F.F", "Page": 0})
        for value in (0.4, 3, True, "", "/Game/T.T", None, [1, 2, 3, 4]):
            self.assertEqual(mi.canonical_value(value, "/p"), value)

    def test_invalid_values_name_the_parameter_path(self):
        diagnostic = self._error({"Parameters": {"BaseColor": [1, 0, 0]}})
        self.assertEqual((diagnostic.code, diagnostic.path), ("MaterialInstance.InvalidValue", "/Body/Parameters/BaseColor"))
        self.assertEqual(self._error({"Parameters": {"M": {"R": True, "G": 1, "B": 0, "A": 0}}}).code,
                         "MaterialInstance.InvalidValue")
        self.assertEqual(self._error({"Parameters": {"B": [True, False, True, False]}}).code, "MaterialInstance.InvalidValue")
        self.assertEqual(self._error({"LayerParameters": {"x": {}}}).path, "/Body/LayerParameters/x")
        self.assertEqual(self._error({"LayerParameters": {"01": {}}}).code, "MaterialInstance.InvalidParameters")
        self.assertEqual(self._error({"Parameters": []}).code, "MaterialInstance.InvalidParameters")
        self.assertEqual(self._error({"Parent": "M_Base"}).code, "MaterialInstance.InvalidParent")
        self.assertEqual(self._error({"Params": {}}).code, "MaterialInstance.UnknownBodyKey")

    def test_parent_empty_string_means_none(self):
        self.assertIsNone(mi.canonical_body({"Parent": ""})["Parent"])
        self.assertIsNone(mi.canonical_body({"Parent": None})["Parent"])

    def test_raw_parameter_arrays_are_body_owned(self):
        region = mi._properties()
        for name in ("ScalarParameterValues", "TextureParameterValues", "FontParameterValues", "Parent"):
            with self.assertRaises(AssetDocumentContractError) as raised:
                region.check_owned({name: []})
            self.assertEqual(raised.exception.diagnostic.code, "MaterialInstance.BodyOwnedProperty")
            self.assertEqual(raised.exception.diagnostic.path, f"/Properties/{name}")
        region.check_owned({"PhysMaterial": None, "BasePropertyOverrides": {}})


class ApplyPlanningTests(unittest.TestCase):
    def test_unchanged_values_send_nothing(self):
        body = mi.canonical_body({"Parent": PARENT, "Parameters": {"Roughness": 0.4, "BaseColor": [1, 0, 0, 1],
                                                                   "Albedo": None},
                                  "LayerParameters": {"1": {"Tiling": 4.0}}})
        self.assertEqual(mi.parameter_ops(body, LIVE), (False, []))

    def test_set_and_clear_only_what_changes(self):
        body = mi.canonical_body({"Parameters": {"Roughness": 0.5, "Albedo": "/Game/T_B.T_B", "UseDetail": None,
                                                 "Missing": None},
                                  "BlendParameters": {"1": {"Mask": 0.25}}})
        changed, ops = mi.parameter_ops(body, LIVE)
        self.assertFalse(changed)
        self.assertEqual(ops, [
            ({"op": "set", "name": "Albedo", "value": "/Game/T_B.T_B"}, "/Body/Parameters/Albedo"),
            ({"op": "set", "name": "Roughness", "value": 0.5}, "/Body/Parameters/Roughness"),
            ({"op": "clear", "name": "UseDetail"}, "/Body/Parameters/UseDetail"),
            ({"op": "set", "name": "Mask", "association": "blend", "index": 1, "value": 0.25},
             "/Body/BlendParameters/1/Mask"),
        ])

    def test_float32_rounding_is_not_a_change(self):
        body = mi.canonical_body({"Parameters": {"Roughness": 0.4000000059604645}})
        self.assertEqual(mi.parameter_ops(body, LIVE), (False, []))

    def test_parent_change_sends_every_authored_parameter(self):
        body = mi.canonical_body({"Parent": "/Game/M_Other.M_Other", "Parameters": {"Roughness": 0.4, "Old": None}})
        changed, ops = mi.parameter_ops(body, LIVE)
        self.assertTrue(changed)
        self.assertEqual([op["op"] for op, _ in ops], ["clear", "set"])

    def test_primitive_paths_map_to_document_paths(self):
        where = mi._paths(["/Body/Parameters/Roughness", "/Body/LayerParameters/1/Tiling"])
        self.assertEqual(where("/ops/1/value"), "/Body/LayerParameters/1/Tiling")
        self.assertEqual(where("/ops/0/name"), "/Body/Parameters/Roughness")
        self.assertEqual(where("/parent"), "/Body/Parent")
        self.assertEqual(where("/ops/7/value"), "/Body")

    def test_refraction_depth_bias_is_sent_only_when_it_changes(self):
        live = {**LIVE, "refraction_depth_bias": 0.0}
        self.assertFalse(mi.refraction_changed(mi.canonical_body({"RefractionDepthBias": 0}), live))
        self.assertTrue(mi.refraction_changed(mi.canonical_body({"RefractionDepthBias": 2}), live))
        self.assertTrue(mi.refraction_changed(mi.canonical_body({"RefractionDepthBias": None}), live))
        self.assertFalse(mi.refraction_changed(mi.canonical_body({"RefractionDepthBias": None}), LIVE))
        self.assertFalse(mi.refraction_changed(mi.canonical_body({}), live))
        self.assertEqual(mi._body(live)["RefractionDepthBias"], 0.0)
        self.assertEqual(mi._paths([])("/refraction_depth_bias"), "/Body/RefractionDepthBias")

    def test_material_layers_are_read_only(self):
        layers = {"layers": ["/Game/L.L"], "blends": [], "names": ["Base"], "visible": [True]}
        live = _live([], layers=layers)
        mi._check_layers({"Layers": ["/Game/L.L"], "Blends": [], "Names": ["Base"], "Visible": [True]}, live)
        with self.assertRaises(AssetDocumentContractError) as raised:
            mi._check_layers({"Layers": [], "Blends": [], "Names": [], "Visible": []}, live)
        self.assertEqual(raised.exception.diagnostic.code, "MaterialInstance.MaterialLayersReadOnly")


class ExtractDiffTests(unittest.TestCase):
    def test_extract_lists_overridden_parameters_only(self):
        body = mi._body(LIVE)
        self.assertEqual(body, {"Parent": PARENT,
                                "Parameters": {"BaseColor": [1, 0, 0, 1], "Roughness": 0.4, "UseDetail": True},
                                "LayerParameters": {"1": {"Tiling": 4}}})
        self.assertEqual(list(body["Parameters"]), ["BaseColor", "Roughness", "UseDetail"])
        self.assertNotIn("BlendParameters", body)
        self.assertEqual(mi._body(_live([], parent=None)), {"Parent": None, "Parameters": {}})

    def test_diff_compares_authored_keys(self):
        extracted = _document(mi._body(LIVE))
        self.assertEqual(mi.diff(mi.canonicalize(extracted), mi.canonicalize(extracted), exact=True), ())
        authored = _document({"Parameters": {"Roughness": 0.4, "UseDetail": None, "Albedo": None}})
        changes = mi.diff(mi.canonicalize(authored), mi.canonicalize(extracted), exact=True)
        self.assertEqual([change.path for change in changes], ["/Body/Parameters/UseDetail"])
        extracted_bias = _document({**mi._body(LIVE), "RefractionDepthBias": 2.5}, properties={"PhysMaterial": None})
        authored = _document({"RefractionDepthBias": 2.5}, properties={"PhysMaterial": None})
        self.assertEqual(mi.diff(mi.canonicalize(authored), mi.canonicalize(extracted_bias), exact=True), ())
        changes = mi.diff(mi.canonicalize(_document({"RefractionDepthBias": None})), mi.canonicalize(extracted_bias), exact=False)
        self.assertEqual([change.path for change in changes], ["/Body/RefractionDepthBias"])
        authored = _document({"Parent": None, "LayerParameters": {"1": {"Tiling": 5}}})
        changes = mi.diff(mi.canonicalize(authored), mi.canonicalize(extracted), exact=False)
        self.assertEqual([change.path for change in changes], ["/Body/Parent", "/Body/LayerParameters/1/Tiling"])

    def test_inspect_schema_gives_the_document_path(self):
        schema = mi.parameter_schema(LIVE)
        self.assertEqual(schema[0]["Path"], "/Body/Parameters/Roughness")
        self.assertEqual(schema[4]["Path"], "/Body/LayerParameters/1/Tiling")
        self.assertEqual((schema[4]["Association"], schema[4]["LayerIndex"]), ("layer", 1))
        self.assertNotIn("Association", schema[0])
        # Compact rows: no Value when not overridden, no empty Group, no DeclaredBy for the base material.
        self.assertEqual(schema[2], {"Name": "Albedo", "Kind": "texture", "Path": "/Body/Parameters/Albedo",
                                     "Overridden": False, "Default": "/Game/T_A.T_A"})
        self.assertEqual(schema[0]["Value"], 0.4)


if __name__ == "__main__":
    unittest.main()
