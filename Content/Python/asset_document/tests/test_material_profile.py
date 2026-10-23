# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Material / MaterialFunction Profiles: document validation, class labels, cycle detection, op planning,
compile-state rule, Extract shape, Diff and primitive-path mapping.  The editor side (expression creation,
connections, Details edits, compile, rollback) is covered end to end by UEAssetDoc.Material.*."""
from __future__ import annotations

import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.profiles import material

TARGET = "/Game/Materials/M_Test"


def _document(body, class_path=material.MATERIAL_CLASS, properties=None):
    return {"SchemaVersion": 1, "Target": TARGET, "Class": class_path, "Action": "CreateOrUpdate",
            "Properties": properties or {}, "Body": body}


def _row(cls, position=(0, 0), properties=None, inputs=None):
    return {"Class": cls, "Position": list(position), "Properties": dict(properties or {}), "Inputs": dict(inputs or {}),
            "outputs": [""]}


LIVE = {
    "host": "material",
    "expressions": {
        "Rough": _row("ScalarParameter", (-400, 0), {"ParameterName": "Roughness", "DefaultValue": 0.5}),
        "Mul": _row("Multiply", (-200, 0), {}, {"A": "Rough"}),
        "Note": _row("Comment", (-500, -100), {"Text": "x"}),
    },
    "outputs": {"Roughness": "Mul"},
    "parameter_groups": {},
    "compile_errors": [],
}

DEFAULTS = {"ScalarParameter": {"properties": {"ParameterName": "Param", "DefaultValue": 0.0, "Group": "None"}},
            "Multiply": {"properties": {"ConstA": 0.0, "ConstB": 1.0}},
            "Comment": {"properties": {"Text": "Comment", "SizeX": 400}}}


def _body(**changes):
    body = {"Expressions": {
        "Rough": {"Class": "ScalarParameter", "Position": [-400, 0],
                  "Properties": {"ParameterName": "Roughness", "DefaultValue": 0.5}},
        "Mul": {"Class": "Multiply", "Position": [-200, 0], "Inputs": {"A": "Rough"}},
        "Note": {"Class": "Comment", "Position": [-500, -100], "Properties": {"Text": "x"}},
    }, "Outputs": {"Roughness": "Mul"}, "ParameterGroups": {}}
    body.update(changes)
    return body


class DocumentTests(unittest.TestCase):
    def _error(self, body, class_path=material.MATERIAL_CLASS):
        with self.assertRaises(AssetDocumentContractError) as raised:
            material.canonicalize(_document(body, class_path))
        return raised.exception.diagnostic

    def test_class_labels(self):
        self.assertEqual(material.class_label("/Script/Engine.MaterialExpressionMultiply"), "Multiply")
        self.assertEqual(material.class_label("MaterialExpressionMultiply"), "Multiply")
        self.assertEqual(material.class_label("Multiply"), "Multiply")
        self.assertEqual(material.class_label("/Script/Landscape.MaterialExpressionLandscapeLayerBlend"), "LandscapeLayerBlend")
        self.assertEqual(material.class_label("/Game/Custom.Custom_C"), "/Game/Custom.Custom_C")

    def test_canonical_body_fills_defaults_and_drops_null_links(self):
        body = material.canonical_body({"Expressions": {"A": {"Class": "/Script/Engine.MaterialExpressionAdd",
                                                              "Inputs": {"A": None}}},
                                        "Outputs": {"BaseColor": None}}, material.MATERIAL_CLASS)
        self.assertEqual(body, {"Expressions": {"A": {"Class": "Add", "Properties": {}, "Inputs": {}}}, "Outputs": {}})

    def test_integral_float_positions_are_integers(self):
        body = material.canonical_body({"Expressions": {"A": {"Class": "Add", "Position": [-1300.0, 20.0]}}},
                                       material.MATERIAL_CLASS)
        self.assertEqual(body["Expressions"]["A"]["Position"], [-1300, 20])
        self.assertIs(type(body["Expressions"]["A"]["Position"][0]), int)

    def test_bare_key_and_new_node_output_zero_name_are_one_link(self):
        defaults = {"VectorParameter": {"properties": {}, "pins": {"outputs": [{"name": "RGB", "type": "float3"},
                                                                               {"name": "R", "type": "float"}]}}}
        classes = {"Color": "VectorParameter", "Mul": "Multiply"}
        self.assertTrue(material.same_source("Color", "Color.RGB", classes, defaults))
        self.assertTrue(material.same_source("Color.RGB", "Color", classes, defaults))
        self.assertFalse(material.same_source("Color", "Color.R", classes, defaults))
        self.assertFalse(material.same_source("Mul", "Mul.RGB", classes, defaults))
        self.assertFalse(material.same_source("Color", None, classes, defaults))
        live = {**LIVE, "expressions": {**LIVE["expressions"], "Color": _row("VectorParameter")},
                "outputs": {"Roughness": "Mul", "BaseColor": "Color"}}
        body = _body(Outputs={"Roughness": "Mul", "BaseColor": "Color.RGB"})
        body["Expressions"]["Color"] = {"Class": "VectorParameter"}
        ops = material.graph_ops(material.canonical_body(body, material.MATERIAL_CLASS), live, {**DEFAULTS, **defaults})
        self.assertEqual([op["op"] for op, _ in ops], [])

    def test_connections_are_made_from_the_sources_forward(self):
        body = {"Expressions": {
            "Out": {"Class": "Multiply", "Inputs": {"A": "Switch"}},
            "Switch": {"Class": "StaticSwitch", "Inputs": {"True": "Value", "False": "Value"}},
            "Value": {"Class": "Constant"}}}
        live = {**LIVE, "expressions": {}, "outputs": {}}
        ops = material.graph_ops(material.canonical_body(body, material.MATERIAL_CLASS), live, DEFAULTS)
        self.assertEqual([op["key"] for op, _ in ops if op["op"] == "connect"], ["Switch", "Switch", "Out"])

    def test_invalid_documents_name_the_path(self):
        self.assertEqual((self._error({"Expressions": {"a.b": {"Class": "Add"}}}).code), "Material.InvalidKey")
        self.assertEqual(self._error({"Expressions": {"A": {"Class": "Add", "Pins": {}}}}).path, "/Body/Expressions/A/Pins")
        self.assertEqual(self._error({"Expressions": {"A": {}}}).path, "/Body/Expressions/A/Class")
        self.assertEqual(self._error({"Expressions": {"A": {"Class": "Add", "Position": [1.5, 0]}}}).code,
                         "Material.InvalidPosition")
        missing = self._error({"Expressions": {"A": {"Class": "Add", "Inputs": {"A": "B.RGB"}}}})
        self.assertEqual((missing.code, missing.path), ("Material.ExpressionNotFound", "/Body/Expressions/A/Inputs/A"))
        # The message names the keys that are close (or lists them).
        close = self._error({"Expressions": {"Mul": {"Class": "Multiply"}, "A": {"Class": "Add", "Inputs": {"A": "mul"}}}})
        self.assertIn("Close: Mul.", close.message)
        self.assertEqual(self._error({"Expressions": {}, "Outputs": {"BaseColor": "Nope"}}).path, "/Body/Outputs/BaseColor")
        self.assertEqual(self._error({"ParameterGroups": {"A": "1"}}).code, "Material.InvalidParameterGroups")
        self.assertEqual(self._error({"Outputs": {}}, material.MATERIAL_FUNCTION_CLASS).code, "Material.UnknownBodyKey")

    def test_cycles_are_refused_at_an_input(self):
        diagnostic = self._error({"Expressions": {
            "A": {"Class": "Add", "Inputs": {"A": "C"}},
            "B": {"Class": "Add", "Inputs": {"A": "A"}},
            "C": {"Class": "Add", "Inputs": {"B": "B.#0"}}}})
        self.assertEqual(diagnostic.code, "Material.ConnectionCycle")
        self.assertTrue(diagnostic.path.startswith("/Body/Expressions/"))
        self.assertIn("->", diagnostic.message)
        self.assertEqual(self._error({"Expressions": {"A": {"Class": "Add", "Inputs": {"A": "A"}}}}).code,
                         "Material.ConnectionCycle")

    def test_body_owned_graph_properties(self):
        region = material._properties(material.MATERIAL_CLASS)
        for name in ("Expressions", "ExpressionCollection", "ParameterGroupData", "FunctionExpressions"):
            with self.assertRaises(AssetDocumentContractError) as raised:
                region.check_owned({name: []})
            self.assertEqual(raised.exception.diagnostic.code, "Material.BodyOwnedProperty")
        region.check_owned({"TwoSided": True, "BlendMode": "BLEND_Masked"})


class PlanningTests(unittest.TestCase):
    def _ops(self, body):
        return [op for op, _ in material.graph_ops(material.canonical_body(body, material.MATERIAL_CLASS), LIVE, DEFAULTS)]

    def test_unchanged_graph_sends_nothing(self):
        self.assertEqual(self._ops(_body()), [])

    def test_explicit_default_value_is_not_a_change(self):
        body = _body()
        body["Expressions"]["Mul"]["Properties"] = {"ConstB": 1}
        self.assertEqual(self._ops(body), [])

    def test_omitted_sections_are_preserved(self):
        self.assertEqual(self._ops({}), [])
        self.assertEqual(self._ops({"Outputs": {"Roughness": "Mul"}}), [])

    def test_delete_create_set_connect_order(self):
        body = _body()
        del body["Expressions"]["Note"]
        body["Expressions"]["Half"] = {"Class": "Multiply", "Properties": {"ConstB": 0.5}, "Inputs": {"A": "Rough"}}
        body["Expressions"]["Rough"]["Properties"]["ParameterName"] = "R2"
        body["Expressions"]["Mul"]["Inputs"] = {"A": "Half"}
        body["Outputs"] = {"Roughness": "Mul", "Specular": "Half"}
        ops = self._ops(body)
        self.assertEqual([op["op"] for op in ops], ["delete", "create", "set", "set", "connect", "connect", "output"])
        self.assertEqual(ops[0], {"op": "delete", "key": "Note"})
        self.assertEqual(ops[1]["class"], "Multiply")
        self.assertEqual(ops[1]["position"], [-400, 0])  # a new node without Position goes left of the material node
        self.assertEqual(ops[2], {"op": "set", "key": "Rough", "properties": {"ParameterName": "R2", "DefaultValue": 0.5}})
        self.assertEqual(ops[3], {"op": "set", "key": "Half", "properties": {"ConstB": 0.5}})
        # (Sources forward: Half's input before Mul's.)
        self.assertEqual(ops[4], {"op": "connect", "key": "Half", "input": "A", "from": "Rough"})
        self.assertEqual(ops[5], {"op": "connect", "key": "Mul", "input": "A", "from": "Half"})
        self.assertEqual(ops[6], {"op": "output", "name": "Specular", "from": "Half"})

    def test_class_change_replaces_the_expression(self):
        body = _body()
        body["Expressions"]["Mul"] = {"Class": "Add", "Inputs": {"A": "Rough"}}
        ops = self._ops(body)
        # (The Roughness output, wired to the old Mul, is wired to the new one.)
        self.assertEqual([op["op"] for op in ops], ["delete", "create", "set", "connect", "output"])
        self.assertEqual(ops[1]["position"], [-400, 0])

    def test_class_change_wires_the_new_expression_again(self):
        # The links to a replaced expression went with it; the ones the document keeps
        # are made again to the new expression.
        body = _body()
        body["Expressions"]["Rough"] = {"Class": "Constant", "Position": [-400, 0], "Properties": {"R": 0.5}}
        ops = self._ops(body)
        self.assertEqual([op["op"] for op in ops], ["delete", "create", "set", "connect"])
        self.assertEqual(ops[3], {"op": "connect", "key": "Mul", "input": "A", "from": "Rough"})
        body["Expressions"]["Mul"]["Class"] = "Add"
        body["Outputs"] = {"Roughness": "Mul", "Metallic": "Rough"}
        ops = self._ops(body)
        self.assertEqual([op["op"] for op in ops], ["delete", "delete", "create", "create", "set", "set", "connect",
                                                    "output", "output"])
        self.assertEqual({op["name"] for op in ops if op["op"] == "output"}, {"Roughness", "Metallic"})

    def test_links_break_before_a_property_set_that_removes_the_input(self):
        # A Custom node's Inputs change removes input N; its link is broken first.
        live = {**LIVE, "expressions": {
            "C": _row("Constant", (-600, 0), {"R": 1.0}),
            "Arc": _row("Custom", (-300, 0), {"Code": "return A + N;", "Inputs": [{"InputName": "A"}, {"InputName": "N"}]},
                        {"A": "C", "N": "C"})}, "outputs": {}}
        body = {"Expressions": {
            "C": {"Class": "Constant", "Position": [-600, 0], "Properties": {"R": 1.0}},
            "Arc": {"Class": "Custom", "Position": [-300, 0],
                    "Properties": {"Code": "return A * M;", "Inputs": [{"InputName": "A"}, {"InputName": "M"}]},
                    "Inputs": {"A": "C", "M": "C"}}}}
        ops = [op for op, _ in material.graph_ops(material.canonical_body(body, material.MATERIAL_CLASS), live, DEFAULTS)]
        self.assertEqual([(op["op"], op.get("input")) for op in ops], [("connect", "N"), ("set", None), ("connect", "M")])
        self.assertIsNone(ops[0]["from"])

    def test_removed_property_resets_and_disconnect_sends_null(self):
        body = _body()
        body["Expressions"]["Rough"]["Properties"] = {"ParameterName": "Roughness"}
        body["Expressions"]["Mul"]["Inputs"] = {}
        body["Outputs"] = {}
        ops = self._ops(body)
        # The link is broken first, while its input exists.
        self.assertEqual(ops[0], {"op": "connect", "key": "Mul", "input": "A", "from": None})
        self.assertEqual(ops[1], {"op": "set", "key": "Rough", "properties": {"ParameterName": "Roughness"}})
        self.assertIn({"op": "output", "name": "Roughness", "from": None}, ops)

    def test_position_only_change(self):
        body = _body()
        body["Expressions"]["Mul"]["Position"] = [0, 64]
        self.assertEqual(self._ops(body), [{"op": "set", "key": "Mul", "position": [0, 64]}])

    def test_parameter_groups_change(self):
        self.assertEqual(self._ops(_body(ParameterGroups={"Surface": 1})),
                         [{"op": "parameter_groups", "groups": {"Surface": 1}}])

    def test_primitive_paths_map_to_document_paths(self):
        paths = ["/Body/Expressions/Rough/Properties", "/Body/Expressions/Mul/Inputs/A", "/Body/Expressions/X/Class"]
        self.assertEqual(material._op_path("/ops/0/properties/DefaultValue", paths),
                         "/Body/Expressions/Rough/Properties/DefaultValue")
        self.assertEqual(material._op_path("/ops/1/from", paths), "/Body/Expressions/Mul/Inputs/A")
        self.assertEqual(material._op_path("/ops/2/class", paths), "/Body/Expressions/X/Class")
        self.assertEqual(material._op_path("/ops/9", paths), "/Body")


class CompileRuleTests(unittest.TestCase):
    def test_new_errors_fail(self):
        failure, warnings = material.compile_outcome([], {"errors": ["Missing input texture"], "error_expressions": ["T"]})
        self.assertIsNotNone(failure)
        self.assertEqual(failure.diagnostic.code, "Material.CompileFailed")
        self.assertEqual(failure.diagnostic.path, "/Body/Expressions/T")

    def test_existing_errors_are_warnings(self):
        failure, warnings = material.compile_outcome(["Old"], {"errors": ["Old"]})
        self.assertIsNone(failure)
        self.assertEqual([item.code for item in warnings], ["Material.CompileError"])

    def test_clean_compile(self):
        self.assertEqual(material.compile_outcome([], {"errors": []}), (None, ()))

    def test_keys_that_differ_only_by_case_are_one_object_name(self):
        # 'DropS' and 'Drops' would be one UE object; refused up front, naming both keys.
        with self.assertRaises(AssetDocumentContractError) as raised:
            material.canonicalize(_document({"Expressions": {"DropS": {"Class": "Constant"}, "Drops": {"Class": "Constant"}}}))
        diagnostic = raised.exception.diagnostic
        self.assertEqual((diagnostic.code, diagnostic.path), ("Material.DuplicateKey", "/Body/Expressions/Drops"))
        self.assertIn("'DropS'", diagnostic.message)
        self.assertIn("'Drops'", diagnostic.message)


class OutputConstantTests(unittest.TestCase):
    """A number in Outputs is the Constant expression Constant_<pin> wired to the pin."""

    def _canonical(self, body):
        return material.canonical_body(body, material.MATERIAL_CLASS)

    def test_numbers_are_accepted_and_bad_values_refused(self):
        self.assertEqual(self._canonical({"Outputs": {"Roughness": 0.15, "Metallic": 1}})["Outputs"],
                         {"Roughness": 0.15, "Metallic": 1.0})
        for bad in (True, float("nan")):
            with self.assertRaises(AssetDocumentContractError) as raised:
                self._canonical({"Outputs": {"Roughness": bad}})
            self.assertEqual(raised.exception.diagnostic.code, "Material.InvalidSource")
        with self.assertRaises(AssetDocumentContractError) as raised:
            self._canonical({"Expressions": {"constant_roughness": {"Class": "Add"}}, "Outputs": {"Roughness": 0.5}})
        self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                         ("Material.DuplicateKey", "/Body/Outputs/Roughness"))

    def test_apply_creates_and_wires_the_constant(self):
        body = material.expand_constants(self._canonical(_body(Outputs={"Roughness": 0.15})), LIVE)
        self.assertEqual(body["Outputs"], {"Roughness": "Constant_Roughness"})
        ops = material.graph_ops(body, LIVE, DEFAULTS)
        self.assertEqual([(op["op"], op.get("key") or op.get("name")) for op, _ in ops],
                         [("create", "Constant_Roughness"), ("set", "Constant_Roughness"), ("output", "Roughness")])
        self.assertEqual(ops[1][0]["properties"], {"R": 0.15})

    def _live_with_constant(self, value=0.15):
        properties = {} if value == 0 else {"R": value}
        return {**LIVE, "expressions": {**LIVE["expressions"], "Constant_Roughness": _row("Constant", (-250, 0), properties)},
                "outputs": {"Roughness": "Constant_Roughness"}}

    def test_extract_writes_the_number_back(self):
        body = material._body(self._live_with_constant(), material.MATERIAL_CLASS)
        self.assertEqual(body["Outputs"], {"Roughness": 0.15})
        self.assertNotIn("Constant_Roughness", body["Expressions"])
        self.assertEqual(material._body(self._live_with_constant(0), material.MATERIAL_CLASS)["Outputs"],
                         {"Roughness": 0.0})
        # A constant that feeds something else, or carries more than its value, is an ordinary expression.
        shared = self._live_with_constant()
        shared["expressions"]["Mul"] = _row("Multiply", (-200, 0), {}, {"A": "Constant_Roughness"})
        self.assertEqual(material._body(shared, material.MATERIAL_CLASS)["Outputs"], {"Roughness": "Constant_Roughness"})
        described = self._live_with_constant()
        described["expressions"]["Constant_Roughness"]["Properties"]["Desc"] = "note"
        self.assertIn("Constant_Roughness", material._body(described, material.MATERIAL_CLASS)["Expressions"])

    def test_reapply_and_explicit_form_are_unchanged(self):
        live = self._live_with_constant()
        extracted = material._body(live, material.MATERIAL_CLASS)
        defaults = {**DEFAULTS, "Constant": {"properties": {"R": 0.0}}}
        self.assertEqual(material.graph_ops(material.expand_constants(self._canonical(extracted), live), live, defaults), [])
        explicit = _body(Outputs={"Roughness": "Constant_Roughness"})
        explicit["Expressions"]["Constant_Roughness"] = {"Class": "Constant", "Properties": {"R": 0.15}}
        self.assertEqual(material.diff(material.canonicalize(_document(explicit)),
                                       material.canonicalize(_document(extracted)), False, defaults=defaults), ())
        changed = _body(Outputs={"Roughness": 0.3})
        self.assertEqual([item.path for item in material.diff(material.canonicalize(_document(changed)),
                                                              material.canonicalize(_document(extracted)), False,
                                                              defaults=defaults)], ["/Body/Outputs"])

    def test_without_expressions_the_graph_is_kept_and_a_stale_constant_goes(self):
        live = self._live_with_constant()
        defaults = {**DEFAULTS, "Constant": {"properties": {"R": 0.0}}}
        body = material.expand_constants(self._canonical({"Outputs": {"Roughness": 0.4}}), live)
        ops = material.graph_ops(body, live, defaults)
        self.assertEqual([(op["op"], op["key"]) for op, _ in ops], [("set", "Constant_Roughness")])
        body = material.expand_constants(self._canonical({"Outputs": {"Roughness": "Mul"}}), live)
        ops = material.graph_ops(body, live, defaults)
        self.assertEqual([(op["op"], op.get("key") or op.get("name")) for op, _ in ops],
                         [("delete", "Constant_Roughness"), ("output", "Roughness")])


class ExtractDiffTests(unittest.TestCase):
    def test_extract_shape(self):
        body = material._body(LIVE, material.MATERIAL_CLASS)
        self.assertEqual(sorted(body), ["Expressions", "Outputs", "ParameterGroups"])
        self.assertNotIn("Inputs", body["Expressions"]["Rough"])
        self.assertNotIn("Properties", body["Expressions"]["Mul"])
        self.assertEqual(sorted(material._body(LIVE, material.MATERIAL_FUNCTION_CLASS)), ["Expressions"])

    def _diff(self, authored_body, exact=False):
        extracted = _document(material._body(LIVE, material.MATERIAL_CLASS))
        return material.diff(material.canonicalize(_document(authored_body)), material.canonicalize(extracted), exact,
                             defaults=DEFAULTS)

    def test_extraction_has_no_diff(self):
        self.assertEqual(self._diff(_body()), ())
        self.assertEqual(self._diff(_body(), exact=True), ())

    def test_explicit_default_and_float32_are_lenient(self):
        body = _body()
        body["Expressions"]["Mul"]["Properties"] = {"ConstB": 1.0}
        body["Expressions"]["Rough"]["Properties"]["DefaultValue"] = 0.5000000001
        self.assertEqual(self._diff(body), ())
        self.assertEqual([item.path for item in self._diff(body, exact=True)],
                         ["/Body/Expressions/Rough/Properties/DefaultValue"])

    def test_unknown_default_is_a_difference(self):
        body = _body()
        body["Expressions"]["Mul"]["Properties"] = {"Unknown": 1}
        self.assertEqual([item.path for item in self._diff(body)], ["/Body/Expressions/Mul/Properties/Unknown"])

    def test_graph_differences(self):
        body = _body()
        del body["Expressions"]["Note"]
        body["Expressions"]["New"] = {"Class": "Add"}
        body["Expressions"]["Mul"]["Inputs"] = {}
        body["Outputs"] = {"Roughness": "Rough"}
        paths = sorted(item.path for item in self._diff(body))
        self.assertEqual(paths, ["/Body/Expressions/Mul/Inputs/A", "/Body/Expressions/New", "/Body/Expressions/Note",
                                 "/Body/Outputs"])


class ChangedParameterDefaultsTest(unittest.TestCase):
    """The parameters whose default a material apply changes (instances overriding them are named)."""

    def _changed(self, expressions):
        return material.changed_parameter_defaults({"Expressions": expressions}, LIVE, DEFAULTS)

    def test_changed_default_is_named_by_its_live_name(self):
        self.assertEqual(self._changed({"Rough": {"Class": "ScalarParameter",
                                                  "Properties": {"ParameterName": "Rough2", "DefaultValue": 0.7}}}),
                         ["Roughness"])

    def test_unchanged_default_new_and_reclassed_expressions_are_not(self):
        self.assertEqual(self._changed({"Rough": {"Class": "ScalarParameter",
                                                  "Properties": {"ParameterName": "Roughness", "DefaultValue": 0.5,
                                                                 "Group": "Surface"}},
                                        "New": {"Class": "ScalarParameter",
                                                "Properties": {"ParameterName": "Metallic", "DefaultValue": 1}}}), [])
        self.assertEqual(self._changed({"Rough": {"Class": "VectorParameter",
                                                  "Properties": {"ParameterName": "Roughness", "DefaultValue": 1}}}), [])

    def test_omitted_default_means_the_new_node_value(self):
        self.assertEqual(self._changed({"Rough": {"Class": "ScalarParameter", "Properties": {"ParameterName": "Roughness"}}}),
                         ["Roughness"])


if __name__ == "__main__":
    unittest.main()
