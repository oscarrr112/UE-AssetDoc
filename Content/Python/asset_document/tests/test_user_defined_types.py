# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""UserDefinedStruct / UserDefinedEnum Profiles: document validation, op planning, diff and the
dependents warnings.  The editor-side behaviour is covered end to end by
UEAssetDoc.UserDefinedTypes.* in the editor."""
from __future__ import annotations

import unittest

from asset_document.diagnostics import AssetDocumentContractError, DiagnosticSeverity
from asset_document.profiles import user_defined_enum as uenum
from asset_document.profiles import user_defined_struct as ustruct
from asset_document.profiles import user_defined_types_common as types

STRUCT_TARGET = "/Game/Data/S_Item"
ENUM_TARGET = "/Game/Data/E_Kind"
BOOL = {"category": "bool", "sub_category": "", "sub_category_object": "", "container": "none", "is_ref": False, "is_const": False}
FLOAT = {**BOOL, "category": "real", "sub_category": "double"}
INT = {**BOOL, "category": "int"}
TEXT = {**BOOL, "category": "text"}
VECTOR = {**BOOL, "category": "struct", "sub_category_object": "/Script/CoreUObject.Vector"}


def _struct(body, properties=None):
    return {"SchemaVersion": 1, "Target": STRUCT_TARGET, "Class": ustruct.USER_STRUCT_CLASS, "Action": "CreateOrUpdate",
            "Properties": properties or {}, "Body": body}


def _enum(body):
    return {"SchemaVersion": 1, "Target": ENUM_TARGET, "Class": uenum.USER_ENUM_CLASS, "Action": "CreateOrUpdate",
            "Properties": {}, "Body": body}


def _member(name, dto, default=None, **fields):
    row = {"guid": f"guid-{name}", "name": name, "type": dto, "tooltip": "", "instance_editable": True, "save_game": False,
           "multi_line": False, "widget_3d": False, "metadata": {}, "supported": default is not None, "default": default}
    row.update(fields)
    return row


def _live_struct(*members, tooltip=""):
    return {"status": "up_to_date", "tooltip": tooltip, "members": list(members)}


def _ops(ops):
    return [op for op, _ in ops]


class StructDocumentTests(unittest.TestCase):
    def _code(self, body, properties=None):
        with self.assertRaises(AssetDocumentContractError) as raised:
            ustruct.canonicalize(_struct(body, properties))
        return raised.exception.diagnostic

    def test_valid_document_canonicalizes(self):
        body = {"Tooltip": "Item", "Members": [
            {"Name": "Label", "Type": "text", "DefaultValue": "Sword", "MultiLine": True},
            {"Name": "Damage", "Type": "double", "ClampMin": 0, "UIMax": 100.5},
            {"Name": "Lookup", "Type": {"Category": "name", "Container": "map", "ValueType": "int"}},
            {"Name": "Power", "RenamedFrom": "Strength", "Type": "array:int", "SaveGame": True}]}
        self.assertEqual(ustruct.canonicalize(_struct(body))["Body"], body)
        canonical = ustruct.canonical_body(body)["Members"]
        self.assertEqual(canonical[1]["Type"], "float")
        self.assertEqual((canonical[1]["ClampMin"], canonical[1]["UIMax"]), ("0", "100.5"))

    def test_invalid_documents_name_the_path(self):
        self.assertEqual(self._code({"Members": []}).code, "UserDefinedStruct.InvalidMembers")
        diagnostic = self._code({"Members": [{"Name": "A", "Type": "int"}, {"Name": "a", "Type": "int"}]})
        self.assertEqual((diagnostic.code, diagnostic.path), ("UserDefinedStruct.DuplicateMember", "/Body/Members/1/Name"))
        diagnostic = self._code({"Members": [{"Name": "A", "Type": "nope"}]})
        self.assertEqual((diagnostic.code, diagnostic.path), ("UserDefinedStruct.InvalidMemberType", "/Body/Members/0/Type"))
        diagnostic = self._code({"Members": [{"Name": "A", "Type": "exec"}]})
        self.assertEqual(diagnostic.code, "UserDefinedStruct.InvalidMemberType")
        diagnostic = self._code({"Members": [{"Name": "A", "Type": {"Category": "int", "IsRef": True}}]})
        self.assertEqual(diagnostic.code, "UserDefinedStruct.InvalidMemberType")
        diagnostic = self._code({"Members": [{"Name": "A"}]})
        self.assertEqual((diagnostic.code, diagnostic.path), ("UserDefinedStruct.InvalidMemberType", "/Body/Members/0/Type"))
        diagnostic = self._code({"Members": [{"Name": " A", "Type": "int"}]})
        self.assertEqual(diagnostic.code, "UserDefinedStruct.InvalidMemberName")
        diagnostic = self._code({"Members": [{"Name": "A", "Type": "int", "ClampMin": "low"}]})
        self.assertEqual(diagnostic.path, "/Body/Members/0/ClampMin")
        diagnostic = self._code({"Members": [{"Name": "A", "Type": "int", "Hidden": True}]})
        self.assertEqual(diagnostic.code, "UserDefinedStruct.UnknownMemberKey")
        self.assertEqual(self._code({"Members": [{"Name": "A", "Type": "int"}]}, {"Guid": "x"}).code,
                         "UserDefinedStruct.UnsupportedProperties")
        self.assertEqual(self._code({"Rows": []}).code, "UserDefinedStruct.UnknownBodyKey")


class StructPlanTests(unittest.TestCase):
    def test_create_replaces_the_factory_member_after_the_adds(self):
        live = _live_struct(_member("MemberVar_0", BOOL, False))
        body = ustruct.canonical_body({"Tooltip": "Item", "Members": [
            {"Name": "Label", "Type": "text", "DefaultValue": "Sword"}, {"Name": "Damage", "Type": "float", "ClampMin": 0}]})
        ops, facts = ustruct.member_ops(body, live)
        self.assertEqual([op["op"] for op in _ops(ops)], ["add", "add", "remove", "default", "set", "tooltip"])
        self.assertEqual(_ops(ops)[0], {"op": "add", "name": "Label", "type": TEXT})
        self.assertEqual(_ops(ops)[2], {"op": "remove", "name": "MemberVar_0"})
        self.assertEqual(_ops(ops)[3], {"op": "default", "name": "Label", "value": "Sword"})
        self.assertEqual(_ops(ops)[4], {"op": "set", "name": "Damage", "metadata": {"ClampMin": "0"}})
        self.assertEqual(facts["removed"], ["MemberVar_0"])

    def test_unchanged_body_plans_nothing(self):
        live = _live_struct(_member("Label", TEXT, "Sword", tooltip="UI", multi_line=True),
                            _member("Damage", FLOAT, 12.5, metadata={"ClampMin": "0"}), tooltip="Item")
        body = ustruct.body_from_live(live)
        self.assertEqual(body["Members"][0], {"Name": "Label", "Type": "text", "DefaultValue": "Sword", "Tooltip": "UI",
                                              "MultiLine": True})
        self.assertEqual(ustruct.member_ops(ustruct.canonical_body(body), live), ([], {"retyped": [], "removed": []}))
        # float32-equal defaults and a struct default naming a subset of fields are unchanged too.
        live = _live_struct(_member("Ratio", FLOAT, 0.30000001192092896), _member("At", VECTOR, {"X": 1.0, "Y": 0.0, "Z": 0.0}))
        body = ustruct.canonical_body({"Members": [{"Name": "Ratio", "Type": "float", "DefaultValue": 0.3},
                                                   {"Name": "At", "Type": "struct:/Script/CoreUObject.Vector",
                                                    "DefaultValue": {"X": 1}}]})
        self.assertEqual(ustruct.member_ops(body, live)[0], [])

    def test_rename_retype_reorder_and_remove(self):
        live = _live_struct(_member("A", INT, 1), _member("B", INT, 2), _member("C", INT, 3))
        body = ustruct.canonical_body({"Members": [
            {"Name": "C", "Type": "int"}, {"Name": "Alpha", "RenamedFrom": "A", "Type": "float", "DefaultValue": 1.5}]})
        ops, facts = ustruct.member_ops(body, live)
        self.assertEqual(_ops(ops), [
            {"op": "remove", "name": "B"},
            {"op": "rename", "name": "A", "to": "Alpha"},
            {"op": "retype", "name": "Alpha", "type": FLOAT},
            {"op": "order", "names": ["C", "Alpha"]},
            {"op": "default", "name": "Alpha", "value": 1.5}])
        self.assertEqual(facts, {"retyped": ["Alpha"], "removed": ["B"]})
        self.assertEqual(ops[1][1], "/Body/Members/1")

    def test_names_match_before_renames_and_case_changes_rename(self):
        live = _live_struct(_member("A", INT, 1), _member("B", INT, 2))
        body = ustruct.canonical_body({"Members": [{"Name": "B", "RenamedFrom": "A", "Type": "int"},
                                                   {"Name": "A", "RenamedFrom": "B", "Type": "int"}]})
        # A listed name keeps its member, so this is a reorder, not a swap.
        self.assertEqual(_ops(ustruct.member_ops(body, live)[0]), [{"op": "order", "names": ["B", "A"]}])
        live = _live_struct(_member("A", INT, 1), _member("B", INT, 2), _member("C", INT, 3))
        body = ustruct.canonical_body({"Members": [{"Name": "a", "Type": "int"}, {"Name": "C", "Type": "int"},
                                                   {"Name": "B", "RenamedFrom": "D", "Type": "int"}]})
        self.assertEqual(_ops(ustruct.member_ops(body, live)[0]), [
            {"op": "rename", "name": "A", "to": "a"}, {"op": "order", "names": ["a", "C", "B"]}])
        # The rename target of a renamed member is freed by the removes that run first.
        body = ustruct.canonical_body({"Members": [{"Name": "C2", "RenamedFrom": "C", "Type": "int"},
                                                   {"Name": "B", "Type": "int"}]})
        live = _live_struct(_member("C2_Old", INT, 0), _member("B", INT, 2), _member("C", INT, 3))
        ops = _ops(ustruct.member_ops(body, live)[0])
        self.assertEqual(ops[:2], [{"op": "remove", "name": "C2_Old"}, {"op": "rename", "name": "C", "to": "C2"}])

    def test_settings_default_and_reset(self):
        live = _live_struct(_member("Label", TEXT, "x", tooltip="Old", instance_editable=False, metadata={"UIMax": "5"}))
        body = ustruct.canonical_body({"Members": [{"Name": "Label", "Type": "text", "SaveGame": True}]})
        ops = _ops(ustruct.member_ops(body, live)[0])
        self.assertEqual(ops, [{"op": "set", "name": "Label", "tooltip": "", "instance_editable": True, "save_game": True,
                                "metadata": {"UIMax": ""}}])

    def test_error_paths_map_to_the_document(self):
        paths = ["/Body/Members", "/Body/Members/2"]
        self.assertEqual(types.map_op_path("/ops/1/type/sub_category_object", paths, ustruct.OP_FIELDS), "/Body/Members/2/Type")
        self.assertEqual(types.map_op_path("/ops/1/value", paths, ustruct.OP_FIELDS), "/Body/Members/2/DefaultValue")
        self.assertEqual(types.map_op_path("/ops/1/metadata/ClampMin", paths, ustruct.OP_FIELDS), "/Body/Members/2/ClampMin")
        self.assertEqual(types.map_op_path("/ops/0/names/3", paths, ustruct.OP_FIELDS), "/Body/Members/3")
        self.assertEqual(types.map_op_path("/ops", paths, ustruct.OP_FIELDS, "/Body/Members"), "/Body/Members")


class StructDiffTests(unittest.TestCase):
    def test_diff_reports_fields_and_order(self):
        live = _struct(ustruct.body_from_live(_live_struct(_member("A", FLOAT, 0.30000001192092896), _member("B", INT, 2))))
        authored = _struct({"Members": [{"Name": "A", "Type": "float", "DefaultValue": 0.3}, {"Name": "B", "Type": "int"}]})
        self.assertEqual(ustruct.diff(authored, live, exact=False), ())
        self.assertEqual([item.path for item in ustruct.diff(authored, live, exact=True)], ["/Body/Members/0/DefaultValue"])
        authored = _struct({"Tooltip": "T", "Members": [{"Name": "A", "Type": "int", "SaveGame": True}, {"Name": "B", "Type": "int"}]})
        self.assertEqual([item.path for item in ustruct.diff(authored, live, exact=True)],
                         ["/Body/Tooltip", "/Body/Members/0/Type", "/Body/Members/0/SaveGame"])
        authored = _struct({"Members": [{"Name": "B", "Type": "int"}, {"Name": "A", "Type": "float"}]})
        self.assertEqual([item.path for item in ustruct.diff(authored, live, exact=True)], ["/Body/Members"])


class EnumTests(unittest.TestCase):
    def _live(self, *names, tooltips=None, bitflags=False, tooltip=""):
        tooltips = tooltips or {}
        return {"tooltip": tooltip, "bitflags": bitflags,
                "enumerators": [{"name": f"NewEnumerator{index}", "display_name": name, "tooltip": tooltips.get(name, ""),
                                 "value": index} for index, name in enumerate(names)]}

    def test_document_validation(self):
        body = {"Tooltip": "Kinds", "Bitflags": True, "Enumerators": [{"Name": "Melee", "Tooltip": "Close"},
                                                                        {"Name": "Ranged", "RenamedFrom": "Bow"}]}
        self.assertEqual(uenum.canonicalize(_enum(body))["Body"], body)
        for bad, code in (({"Enumerators": [{"Name": "A"}, {"Name": "a"}]}, "UserDefinedEnum.DuplicateEnumerator"),
                          ({"Enumerators": [{"Name": ""}]}, "UserDefinedEnum.InvalidEnumeratorName"),
                          ({"Enumerators": [{"Name": "A", "Value": 1}]}, "UserDefinedEnum.UnknownEnumeratorKey"),
                          ({"Bitflags": "yes"}, "UserDefinedEnum.InvalidBitflags")):
            with self.assertRaises(AssetDocumentContractError) as raised:
                uenum.canonicalize(_enum(bad))
            self.assertEqual(raised.exception.diagnostic.code, code)

    def test_plan_and_no_op(self):
        live = self._live("Melee", "Bow", "Magic", tooltips={"Melee": "Close"})
        body = uenum.canonical_body({"Tooltip": "Kinds", "Bitflags": True, "Enumerators": [
            {"Name": "Ranged", "RenamedFrom": "Bow"}, {"Name": "melee"}, {"Name": "Thrown", "Tooltip": "Far"}]})
        ops, facts = uenum.enumerator_ops(body, live)
        self.assertEqual(_ops(ops), [
            {"op": "remove", "name": "Magic"},
            {"op": "rename", "name": "Melee", "to": "melee"},
            {"op": "rename", "name": "Bow", "to": "Ranged"},
            {"op": "add", "name": "Thrown"},
            {"op": "order", "names": ["Ranged", "melee", "Thrown"]},
            {"op": "tooltip", "name": "melee", "tooltip": ""},
            {"op": "tooltip", "name": "Thrown", "tooltip": "Far"},
            {"op": "description", "tooltip": "Kinds"},
            {"op": "bitflags", "value": True}])
        self.assertEqual(facts, {"removed": ["Magic"]})
        unchanged = uenum.canonical_body(uenum.body_from_live(live))
        self.assertEqual(uenum.enumerator_ops(unchanged, live), ([], {"removed": []}))

    def test_diff(self):
        live = _enum(uenum.body_from_live(self._live("A", "B", tooltips={"B": "b"})))
        self.assertEqual(uenum.diff(_enum({"Enumerators": [{"Name": "A"}, {"Name": "B", "Tooltip": "b"}]}), live), ())
        self.assertEqual([item.path for item in uenum.diff(_enum({"Bitflags": True, "Enumerators": [{"Name": "A"}, {"Name": "B"}]}), live)],
                         ["/Body/Bitflags", "/Body/Enumerators/1/Tooltip"])


class InspectTests(unittest.TestCase):
    def test_details_are_compact(self):
        live = {"status": "UDSS_UpToDate", "error_message": "", "members": [
            {"name": "Label", "guid": "g1", "cpp_type": "FText", "codec": "text", "supported": True,
             "multi_line_allowed": True, "widget_3d_allowed": False, "value_range_allowed": False, "invalid": False},
            {"name": "Damage", "guid": "g2", "cpp_type": "double", "codec": "double", "supported": False,
             "multi_line_allowed": False, "widget_3d_allowed": False, "value_range_allowed": True, "invalid": True}]}
        details = ustruct.inspect_details(live, "/Game/Data/S_Item.S_Item")
        self.assertEqual(details["Members"], [
            {"Name": "Label", "CppType": "FText", "Codec": "text", "Allows": ["MultiLine"]},
            {"Name": "Damage", "CppType": "double", "Codec": "double", "Allows": ["ValueRange"],
             "DefaultSupported": False, "Invalid": True}])
        self.assertEqual(details["UseAs"], {"RowStruct": "/Game/Data/S_Item.S_Item", "Type": "struct:/Game/Data/S_Item.S_Item"})
        self.assertNotIn("Dependents", details)
        self.assertNotIn("ErrorMessage", details)
        enum = uenum.inspect_details({"enumerators": [{"display_name": "Melee", "name": "NewEnumerator0", "value": 0}],
                                      "dependents": [{"package": "/Game/DT", "kind": "DataTable", "dirty": False}]},
                                     "/Game/Data/E_Kind.E_Kind")
        self.assertEqual(enum["UseAs"], {"Type": "byte:/Game/Data/E_Kind.E_Kind"})
        self.assertEqual(enum["Dependents"], [{"Package": "/Game/DT", "Kind": "DataTable", "Unsaved": False}])


class WarningTests(unittest.TestCase):
    def test_dependents_are_reported_as_warnings(self):
        rows = [{"package": "/Game/DT_Items", "kind": "DataTable", "kept_clean": True},
                {"package": "/Game/Other", "kind": "Other", "kept_clean": False},
                {"package": "/Game/DT_Items", "kind": "DataTable", "kept_clean": True}]
        warnings = ustruct.apply_warnings({"retyped": ["Damage"], "removed": ["Old"]}, rows)
        self.assertEqual([item.code for item in warnings], ["UserDefinedStruct.DependentsUpdated",
                                                            "UserDefinedStruct.MemberValuesConverted",
                                                            "UserDefinedStruct.MemberValuesRemoved"])
        self.assertTrue(all(item.severity is DiagnosticSeverity.WARNING for item in warnings))
        self.assertIn("/Game/DT_Items (DataTable)", warnings[0].message)
        self.assertIn("/Game/Other (Other)", warnings[0].message)
        self.assertEqual(ustruct.apply_warnings({"retyped": ["Damage"], "removed": []}, []), [])
        self.assertEqual([item.code for item in uenum.apply_warnings({"removed": ["A"]}, rows[:1])],
                         ["UserDefinedEnum.DependentsUpdated", "UserDefinedEnum.EnumeratorValuesInvalidated"])

    def test_a_dependent_blueprint_that_did_not_compile_is_reported(self):
        rows = [{"package": "/Game/BP_User", "kind": "Blueprint", "kept_clean": True, "compile_failed": True},
                {"package": "/Game/DT_Items", "kind": "DataTable", "kept_clean": True, "compile_failed": False}]
        warnings = ustruct.apply_warnings({"retyped": [], "removed": []}, rows)
        self.assertEqual([item.code for item in warnings], ["UserDefinedStruct.DependentsUpdated",
                                                            "UserDefinedStruct.DependentCompileFailed"])
        self.assertIn("/Game/BP_User", warnings[1].message)
        self.assertNotIn("/Game/DT_Items", warnings[1].message)
        self.assertEqual([item.code for item in uenum.apply_warnings({"removed": []}, rows)],
                         ["UserDefinedEnum.DependentsUpdated", "UserDefinedEnum.DependentCompileFailed"])


if __name__ == "__main__":
    unittest.main()
