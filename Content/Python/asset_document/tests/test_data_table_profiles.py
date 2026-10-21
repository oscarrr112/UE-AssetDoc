"""Data table family Profiles (DataTable, CompositeDataTable, MirrorDataTable, CurveTable,
CompositeCurveTable, StringTable): document validation, value comparison, op planning and
primitive-path mapping.  The editor-side behaviour is covered end to end by
UEAssetDoc.DataTable.* / CurveTable.* / StringTable.* in the editor."""
from __future__ import annotations

import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.profiles import curve_table, data_table, string_table
from asset_document.profiles import data_table_common as common

TARGET = "/Game/Data/DT_Items"


def _document(class_path, body, properties=None, action="CreateOrUpdate"):
    return {"SchemaVersion": 1, "Target": TARGET, "Class": class_path, "Action": action,
            "Properties": properties or {}, "Body": body}


class CommonTests(unittest.TestCase):
    def test_same_compares_numbers_numerically_and_float32_leniently(self):
        self.assertTrue(common.same(1, 1.0))
        self.assertFalse(common.same(0.3, 0.30000001192092896))
        self.assertTrue(common.same(0.3, 0.30000001192092896, exact=False))
        self.assertTrue(common.same({"A": [1, 2.0]}, {"A": [1.0, 2]}))
        self.assertFalse(common.same({"A": 1}, {"A": 1, "B": 2}))
        self.assertFalse(common.same(True, 1))
        self.assertFalse(common.same("1", 1))

    def test_map_path_turns_primitive_paths_into_document_paths(self):
        op_paths = ["/Body/Rows", "/Body/Rows/3"]
        base = {"row_struct": "/Body/RowStruct", "ops": "/Body/Rows"}
        self.assertEqual(common.map_path("/ops/1/values/Count", op_paths, base), "/Body/Rows/3/Values/Count")
        self.assertEqual(common.map_path("/ops/1/curve/Keys/0/Time", op_paths, base), "/Body/Rows/3/Keys/0/Time")
        self.assertEqual(common.map_path("/ops/0/tables/2", ["/Body/ParentTables"], base), "/Body/ParentTables/2")
        self.assertEqual(common.map_path("/ops/1/row", op_paths, base), "/Body/Rows/3")
        self.assertEqual(common.map_path("/row_struct", op_paths, base), "/Body/RowStruct")
        self.assertEqual(common.map_path("/ops/9/row", op_paths, base), "/Body/Rows")
        self.assertEqual(common.map_path("", op_paths, base), "/Body")

    def test_chunk_ops_stays_under_the_node_budget(self):
        ops = [({"op": "set", "row": f"R{index}", "values": {"A": 1}}, f"/Body/Rows/{index}") for index in range(10)]
        chunks = common.chunk_ops(ops, budget=12)
        self.assertEqual(sum(len(chunk) for chunk in chunks), 10)
        self.assertTrue(all(sum(common.count_nodes(op) for op, _ in chunk) <= 12 for chunk in chunks))
        self.assertEqual([path for chunk in chunks for _, path in chunk], [path for _, path in ops])

    def test_header_rejects_unknown_keys_and_non_canonical_targets(self):
        with self.assertRaises(AssetDocumentContractError) as raised:
            data_table.canonicalize({**_document(data_table.DATA_TABLE_CLASS, {}), "Extra": 1})
        self.assertEqual(raised.exception.diagnostic.code, "DataTable.UnknownDocumentKey")
        with self.assertRaises(AssetDocumentContractError) as raised:
            data_table.canonicalize({**_document(data_table.DATA_TABLE_CLASS, {}), "Target": "/Game/Bad Name"})
        self.assertEqual(raised.exception.diagnostic.code, "DataTable.NonCanonicalTarget")


class DataTableTests(unittest.TestCase):
    def _error(self, body, class_path=data_table.DATA_TABLE_CLASS):
        with self.assertRaises(AssetDocumentContractError) as raised:
            data_table.canonicalize(_document(class_path, body))
        return raised.exception.diagnostic

    def test_body_validation_names_the_document_path(self):
        self.assertEqual(self._error({"Rows": [{"Name": "A", "Value": {}}]}).path, "/Body/Rows/0/Value")
        self.assertEqual(self._error({"Rows": [{"Name": "A"}, {"Name": "a"}]}).code, "DataTable.DuplicateRow")
        self.assertEqual(self._error({"Rows": [{"Name": "has space"}]}).code, "DataTable.InvalidRowName")
        self.assertEqual(self._error({"Rows": [{"Name": "None"}]}).code, "DataTable.InvalidRowName")
        self.assertEqual(self._error({"RowStruct": "ItemRow"}).code, "DataTable.InvalidRowStruct")
        self.assertEqual(self._error({"ParentTables": []}).code, "DataTable.UnknownBodyKey")
        self.assertEqual(self._error({"Rows": [{"Name": "B", "RenamedFrom": "B"}]}).code, "DataTable.InvalidRename")

    def test_a_row_value_difference_holds_that_value(self):
        # The difference named a row value but held the whole Rows region (the verification text then walked
        # the region from the value's path).
        live = _document(data_table.DATA_TABLE_CLASS, {"RowStruct": "/Script/M.Row", "Rows": [
            {"Name": "A", "Values": {"Damage": 1, "Label": "a"}}, {"Name": "B", "Values": {"Damage": 2, "Label": "b"}}]})
        authored = _document(data_table.DATA_TABLE_CLASS, {"Rows": [
            {"Name": "A", "Values": {"Damage": 1, "Label": "a"}}, {"Name": "B", "Values": {"Damage": 3}}]})
        (item,) = data_table.diff(authored, live, True)
        self.assertEqual((item.path, item.before, item.after), ("/Body/Rows/1/Values/Damage", 3, 2))
        # A field the row leaves out is the row struct's default.
        authored["Body"]["Rows"][1]["Values"]["Damage"] = 2
        (item,) = data_table.diff(authored, live, True, {"Damage": 0, "Label": ""})
        self.assertEqual((item.path, item.before, item.after), ("/Body/Rows/1/Values/Label", "", "b"))

    def test_diff_counts_and_names_every_changed_row(self):
        # Two changed rows read "would change 1 value(s)"; only the first was listed.
        live = _document(data_table.DATA_TABLE_CLASS, {"RowStruct": "/Script/M.Row", "Rows": [
            {"Name": "Small", "Values": {"Heal": 20, "Material": None}},
            {"Name": "Large", "Values": {"Heal": 40, "Material": None}},
            {"Name": "Old", "Values": {"Heal": 1, "Material": None}}]})
        authored = _document(data_table.DATA_TABLE_CLASS, {"Rows": [
            {"Name": "Small", "Values": {"Heal": 25, "Material": "/Game/MI_S.MI_S"}},
            {"Name": "Large", "Values": {"Heal": 40, "Material": "/Game/MI_L.MI_L"}},
            {"Name": "Boost", "Values": {"Heal": 15}}]})
        changes = data_table.diff(authored, live, True)
        self.assertEqual([(item.path, item.kind.value, item.row) for item in changes], [
            ("/Body/Rows", "Remove", "Old"),
            ("/Body/Rows/0/Values/Heal", "Replace", "Small"),
            ("/Body/Rows/0/Values/Material", "Replace", "Small"),
            ("/Body/Rows/1/Values/Material", "Replace", "Large"),
            ("/Body/Rows/2", "Add", "Boost")])
        removed, added = changes[0], changes[-1]
        self.assertEqual((removed.before_present, removed.after["Name"]), (False, "Old"))
        self.assertEqual((added.after_present, added.before["Name"]), (False, "Boost"))
        # A rename is one change of Name; a move of kept rows one change of their order.
        renamed = _document(data_table.DATA_TABLE_CLASS, {"Rows": [
            {"Name": "Large", "Values": {"Heal": 40}}, {"Name": "Tiny", "RenamedFrom": "Small", "Values": {"Heal": 20}},
            {"Name": "Old"}]})
        self.assertEqual([(item.path, item.row) for item in data_table.diff(renamed, live, True)],
                         [("/Body/Rows/1/Name", "Tiny"), ("/Body/Rows", None)])
        self.assertEqual(data_table.diff(live, live, True), ())

    def test_composite_accepts_parent_tables(self):
        document = data_table.canonicalize(_document(data_table.COMPOSITE_DATA_TABLE_CLASS,
                                                     {"RowStruct": "/Script/M.Row", "ParentTables": ["/Game/A.A"]}))
        self.assertEqual(document["Body"]["ParentTables"], ["/Game/A.A"])

    def test_row_ops_rename_remove_add_set_and_order(self):
        live = [{"Name": "Sword", "Values": {"Count": 3, "Label": "Sword"}},
                {"Name": "Bow", "Values": {"Count": 1, "Label": "Bow"}},
                {"Name": "Staff", "Values": {"Count": 0, "Label": ""}}]
        authored = [{"Name": "Axe", "Values": {"Label": "Axe"}},
                    {"Name": "Longbow", "RenamedFrom": "Bow", "Values": {"Count": 2}},
                    {"Name": "Sword", "Values": {"Count": 3.0}}]
        ops = data_table.row_ops(authored, live)
        self.assertEqual([op for op, _ in ops], [
            {"op": "remove", "row": "Staff"},
            {"op": "add", "row": "Axe"},
            {"op": "rename", "row": "Bow", "to": "Longbow"},
            {"op": "set", "row": "Axe", "values": {"Label": "Axe"}},
            {"op": "set", "row": "Longbow", "values": {"Count": 2}},
            {"op": "order", "rows": ["Axe", "Longbow", "Sword"], "at": 0},
        ])
        self.assertEqual(ops[2][1], "/Body/Rows/1/RenamedFrom")
        self.assertEqual(ops[4][1], "/Body/Rows/1")

    def test_row_ops_is_empty_for_the_live_rows(self):
        live = [{"Name": "A", "Values": {"X": 1}}, {"Name": "B", "Values": {"X": 2}}]
        self.assertEqual(data_table.row_ops([{"Name": "A", "Values": {"X": 1.0}}, {"Name": "B"}], live), [])
        # A done rename (source gone, target present) is ignored.
        self.assertEqual(data_table.row_ops([{"Name": "A", "RenamedFrom": "Old"}, {"Name": "B"}], live), [])

    def test_row_ops_reorders_only(self):
        live = [{"Name": "A", "Values": {}}, {"Name": "B", "Values": {}}]
        self.assertEqual([op for op, _ in data_table.row_ops([{"Name": "B"}, {"Name": "A"}], live)],
                         [{"op": "order", "rows": ["B", "A"], "at": 0}])

    def test_row_ops_value_edit_sends_no_order(self):
        live = [{"Name": "A", "Values": {"X": 1}}, {"Name": "B", "Values": {"X": 2}}]
        self.assertEqual([op for op, _ in data_table.row_ops([{"Name": "A"}, {"Name": "B", "Values": {"X": 3}}], live)],
                         [{"op": "set", "row": "B", "values": {"X": 3}}])

    def test_long_order_is_sent_in_budgeted_segments(self):
        names = [f"Row_{index:05d}" for index in range(5000)]
        live = [{"Name": name, "Values": {"X": 1}} for name in names[1:]]
        ops = data_table.row_ops([{"Name": name} for name in names], live)
        orders = [op for op, _ in ops if op["op"] == "order"]
        self.assertEqual([op["at"] for op in orders], list(range(0, 5000, data_table.ORDER_SEGMENT)))
        self.assertEqual([name for op in orders for name in op["rows"]], names)
        self.assertTrue(all(common.count_nodes(op) <= common.OP_NODE_BUDGET for op, _ in ops))

    def test_diff_is_sparse_and_ordered(self):
        extracted = _document(data_table.DATA_TABLE_CLASS, {"RowStruct": "/Script/M.Row", "Rows": [
            {"Name": "A", "Values": {"X": 0.30000001192092896, "Y": 2}}, {"Name": "B", "Values": {"X": 1}}]})
        profile = data_table.DataTableRuntimeProfile(data_table.DATA_TABLE_CLASS, "profile.data_table.v1")
        sparse = _document(data_table.DATA_TABLE_CLASS, {"Rows": [{"Name": "A", "Values": {"X": 0.3}}, {"Name": "B"}]})
        self.assertEqual(profile.diff(sparse, extracted), ())
        self.assertEqual([change.path for change in profile.diff_exact(sparse, extracted)], ["/Body/Rows/0/Values/X"])
        reordered = _document(data_table.DATA_TABLE_CLASS, {"Rows": [{"Name": "B"}, {"Name": "A"}]})
        self.assertEqual([change.path for change in profile.diff(reordered, extracted)], ["/Body/Rows"])

    # A row's Values is the complete row; a field it leaves out is the row struct default.
    def test_row_ops_replaces_a_row_whose_unwritten_field_is_not_the_default(self):
        defaults = {"Count": 0, "Label": "", "Weight": 1.0}
        live = [{"Name": "A", "Values": {"Count": 3, "Label": "A", "Weight": 1.0}},
                {"Name": "B", "Values": {"Count": 2, "Label": "", "Weight": 1.0}}]
        authored = [{"Name": "A", "Values": {"Count": 3}}, {"Name": "B", "Values": {"Count": 2}}]
        self.assertEqual([op for op, _ in data_table.row_ops(authored, live, defaults)],
                         [{"op": "set", "row": "A", "values": {"Count": 3}, "replace": True}])
        # A row without Values is a row at the defaults.
        self.assertEqual([op for op, _ in data_table.row_ops([{"Name": "A"}, {"Name": "B", "Values": {"Count": 2}}], live,
                                                              defaults)],
                         [{"op": "set", "row": "A", "values": {}, "replace": True}])
        # The complete live rows (what Extract returns) are a no-op; a new row is only set when it has values.
        self.assertEqual(data_table.row_ops(live, live, defaults), [])
        self.assertEqual([op for op, _ in data_table.row_ops(live + [{"Name": "C"}], live, defaults)],
                         [{"op": "add", "row": "C"}, {"op": "order", "rows": ["A", "B", "C"], "at": 0}])

    def test_row_defaults_come_from_the_first_page(self):
        self.assertEqual(data_table.row_defaults({"row_defaults": {"name": "", "values": {"X": 1}}}), {"X": 1})
        self.assertIsNone(data_table.row_defaults({}))

    def test_diff_with_defaults_reports_an_unwritten_field_off_its_default(self):
        extracted = _document(data_table.DATA_TABLE_CLASS, {"RowStruct": "/Script/M.Row", "Rows": [
            {"Name": "A", "Values": {"X": 1, "Y": 2}}]})
        profile = data_table.DataTableRuntimeProfile(data_table.DATA_TABLE_CLASS, "profile.data_table.v1")
        sparse = _document(data_table.DATA_TABLE_CLASS, {"Rows": [{"Name": "A", "Values": {"X": 1}}]})
        self.assertEqual([change.path for change in profile.diff_with_defaults(sparse, extracted, {"X": 0, "Y": 0}, False)],
                         ["/Body/Rows/0/Values/Y"])
        self.assertEqual(profile.diff_with_defaults(sparse, extracted, {"X": 0, "Y": 2}, False), ())
        self.assertEqual(profile.diff_with_defaults(sparse, extracted, None, False), ())

    def test_mirror_factory_context(self):
        mirror = data_table.MIRROR_DATA_TABLE_CLASS
        document = data_table.canonicalize({**_document(mirror, {}, action="Create"),
                                            "FactoryContext": "/Game/Characters/SK_Hero.SK_Hero"})
        self.assertEqual(document["FactoryContext"], "/Game/Characters/SK_Hero.SK_Hero")
        with self.assertRaises(AssetDocumentContractError) as raised:
            data_table.canonicalize({**_document(data_table.DATA_TABLE_CLASS, {}), "FactoryContext": "/Game/S.S"})
        self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                         ("DataTable.UnknownDocumentKey", "/FactoryContext"))
        with self.assertRaises(AssetDocumentContractError) as raised:
            data_table.canonicalize({**_document(mirror, {}), "FactoryContext": "SK_Hero"})
        self.assertEqual(raised.exception.diagnostic.code, "MirrorDataTable.InvalidFactoryContext")
        # The Skeleton is required before anything is resolved.
        with self.assertRaises(AssetDocumentContractError) as raised:
            data_table.create_target_context(data_table.canonicalize(_document(mirror, {}, action="Create")), None)
        self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                         ("MirrorDataTable.SkeletonRequired", "/FactoryContext"))
        self.assertEqual(data_table.MIRROR_DATA_TABLE_RUNTIME_REGISTRATION.target_lifecycle.create_context_handle_names,
                         ("factory_context", "factory_context_2"))
        template = data_table.DataTableRuntimeProfile(mirror, "profile.mirror_data_table.v1").create_template(TARGET)
        self.assertEqual(data_table.canonicalize(template)["Body"], {"RowStruct": "/Script/Engine.MirrorTableRow"})

    def test_registrations(self):
        self.assertEqual({registration.profile_meta.profile_id for registration in (
            data_table.DATA_TABLE_RUNTIME_REGISTRATION, data_table.COMPOSITE_DATA_TABLE_RUNTIME_REGISTRATION,
            data_table.MIRROR_DATA_TABLE_RUNTIME_REGISTRATION)},
            {"profile.data_table.v1", "profile.composite_data_table.v1", "profile.mirror_data_table.v1"})


class CurveTableTests(unittest.TestCase):
    def _error(self, body, class_path=curve_table.CURVE_TABLE_CLASS):
        with self.assertRaises(AssetDocumentContractError) as raised:
            curve_table.canonicalize(_document(class_path, body))
        return raised.exception.diagnostic

    def test_body_validation(self):
        self.assertEqual(self._error({"Rows": [{"Name": "A", "Keys": []}]}).code, "CurveTable.ModeRequired")
        self.assertEqual(self._error({"CurveTableMode": "Cubic"}).code, "CurveTable.InvalidMode")
        error = self._error({"CurveTableMode": "SimpleCurves", "Rows": [
            {"Name": "A", "Keys": [{"Time": 2, "Value": 0}, {"Time": 1, "Value": 0}]}]})
        self.assertEqual((error.code, error.path), ("CurveTable.KeysNotAscending", "/Body/Rows/0/Keys/1/Time"))
        error = self._error({"CurveTableMode": "SimpleCurves", "Rows": [{"Name": "A", "InterpMode": "Cubic"}]})
        self.assertEqual(error.code, "CurveTable.InvalidInterpMode")
        error = self._error({"CurveTableMode": "RichCurves", "Rows": [{"Name": "A", "Keys": [{"Time": 0}]}]})
        self.assertEqual(error.code, "CurveTable.InvalidKey")
        self.assertEqual(self._error({"ParentTables": []}).code, "CurveTable.UnknownBodyKey")

    def test_simple_curve_round_trips(self):
        row = {"InterpMode": "Constant", "Keys": [{"Time": 0, "Value": 0.3}], "PostInfinityExtrap": "Linear"}
        native = curve_table.simple_from_document(row, "/Body/Rows/0")
        self.assertEqual(native["InterpMode"], "RCIM_Constant")
        back = curve_table.simple_to_document(native, "/Body/Rows/0")
        self.assertEqual(back, {"InterpMode": "Constant", "Keys": [{"Time": 0.0, "Value": 0.3}],
                                "PreInfinityExtrap": "Constant", "PostInfinityExtrap": "Linear"})

    def test_curve_ops(self):
        live = [{"Name": "Damage", **curve_table.canonical_curve({"Keys": [{"Time": 0, "Value": 1}]}, "SimpleCurves", "/x")},
                {"Name": "Speed", **curve_table.canonical_curve({"Keys": [{"Time": 0, "Value": 2}]}, "SimpleCurves", "/x")}]
        authored = [{"Name": "Velocity", "RenamedFrom": "Speed", "Keys": [{"Time": 0, "Value": 2}]},
                    {"Name": "Range", "Keys": [{"Time": 0, "Value": 3}]}]
        ops = [op for op, _ in curve_table.curve_ops(authored, "SimpleCurves", live, "SimpleCurves")]
        self.assertEqual([(op["op"], op["row"]) for op in ops], [("remove", "Damage"), ("rename", "Speed"), ("set", "Range")])
        unchanged = [{"Name": "Damage", "Keys": [{"Time": 0, "Value": 1}]}, {"Name": "Speed", "Keys": [{"Time": 0, "Value": 2}]}]
        self.assertEqual(curve_table.curve_ops(unchanged, "SimpleCurves", live, "SimpleCurves"), [])
        rebuilt = curve_table.curve_ops([{"Name": "Damage", "Keys": [{"Time": 0, "Value": 1}]}], "RichCurves", live, "SimpleCurves")
        self.assertEqual([op["op"] for op, _ in rebuilt], ["reset", "set"])
        self.assertEqual(rebuilt[1][0]["mode"], "RichCurves")


    def test_diff_lists_every_changed_curve(self):
        def curve(name, value):
            return {"Name": name, **curve_table.canonical_curve({"Keys": [{"Time": 0, "Value": value}]}, "SimpleCurves", "/x")}
        live = _document(curve_table.CURVE_TABLE_CLASS, {"CurveTableMode": "SimpleCurves",
                                                         "Rows": [curve("Damage", 1), curve("Speed", 2), curve("Range", 3)]})
        authored = _document(curve_table.CURVE_TABLE_CLASS, {"CurveTableMode": "SimpleCurves", "Rows": [
            {"Name": "Damage", "Keys": [{"Time": 0, "Value": 5}]}, {"Name": "Speed", "Keys": [{"Time": 0, "Value": 6}]},
            {"Name": "Range", "Keys": [{"Time": 0, "Value": 3}]}, {"Name": "Weight", "Keys": [{"Time": 0, "Value": 1}]}]})
        self.assertEqual([(item.path, item.kind.value, item.row) for item in curve_table.diff(authored, live, True)], [
            ("/Body/Rows/0", "Replace", "Damage"), ("/Body/Rows/1", "Replace", "Speed"), ("/Body/Rows/3", "Add", "Weight")])
        # Curve order is not authored; a new curve type is one change of the mode.
        reordered = {**authored, "Body": {"CurveTableMode": "SimpleCurves", "Rows": [
            {"Name": "Range", "Keys": [{"Time": 0, "Value": 3}]}, {"Name": "Damage", "Keys": [{"Time": 0, "Value": 1}]},
            {"Name": "Speed", "Keys": [{"Time": 0, "Value": 2}]}]}}
        self.assertEqual(curve_table.diff(reordered, live, True), ())
        retyped = {**reordered, "Body": {**reordered["Body"], "CurveTableMode": "RichCurves"}}
        self.assertEqual([item.path for item in curve_table.diff(retyped, live, True)], ["/Body/CurveTableMode"])


class StringTableTests(unittest.TestCase):
    def _error(self, body, properties=None):
        with self.assertRaises(AssetDocumentContractError) as raised:
            string_table.canonicalize(_document(string_table.STRING_TABLE_CLASS, body, properties))
        return raised.exception.diagnostic

    def test_validation(self):
        self.assertEqual(self._error({"Entries": {"a b": "x"}}).path, "/Body/Entries/a b")
        self.assertEqual(self._error({"Entries": {"a": ""}}).code, "StringTable.EmptySourceString")
        self.assertEqual(self._error({"Namespace": "[x]"}).code, "StringTable.InvalidNamespace")
        self.assertEqual(self._error({"Entries": {"a": "x"}, "MetaData": {"b": {"Comment": "c"}}}).code,
                         "StringTable.KeyNotFound")
        self.assertIn("Close: Greeting.", self._error({"Entries": {"Greeting": "x"},
                                                       "MetaData": {"greeting": {"Comment": "c"}}}).message)
        self.assertEqual(self._error({}, {"Foo": 1}).code, "StringTable.UnsupportedProperties")

    def test_string_ops(self):
        live = {"Namespace": "UI", "Entries": {"Start": "Start", "Quit": "Quit"}, "MetaData": {"Start": {"Comment": "c"}}}
        body = {"Entries": {"Start": "Play", "Options": "Options"}, "MetaData": {"Options": {"Comment": "o"}}}
        ops = [op for op, _ in string_table.string_ops(body, live)]
        self.assertEqual(ops, [
            {"op": "remove", "key": "Quit"},
            {"op": "set", "key": "Options", "source": "Options"},
            {"op": "set", "key": "Start", "source": "Play"},
            {"op": "meta_set", "key": "Options", "id": "Comment", "value": "o"},
            {"op": "meta_remove", "key": "Start", "id": "Comment"},
        ])
        self.assertEqual(string_table.string_ops({"Entries": dict(live["Entries"]), "MetaData": dict(live["MetaData"])}, live), [])
        # Omitted sections are kept.
        self.assertEqual(string_table.string_ops({}, live), [])

    def test_diff_is_key_by_key(self):
        # Entries and MetaData are complete maps: removed keys are Remove entries (validate and diff count them);
        # the metadata of a removed entry goes with it.
        live = _document(string_table.STRING_TABLE_CLASS, {"Namespace": "UI", "Entries": {"Start": "Start", "Quit": "Quit"},
                                                            "MetaData": {"Start": {"Comment": "c", "Context": "x"},
                                                                         "Quit": {"Comment": "q"}}})
        authored = _document(string_table.STRING_TABLE_CLASS, {"Entries": {"Start": "Play", "Options": "Options"},
                                                                "MetaData": {"Start": {"Comment": "c"}}})
        changes = [(item.path, item.kind.value, item.before_present, item.after_present)
                   for item in string_table.diff(authored, live)]
        self.assertEqual(changes, [
            ("/Body/Entries/Quit", "Remove", False, True),
            ("/Body/Entries/Start", "Replace", True, True),
            ("/Body/Entries/Options", "Add", True, False),
            ("/Body/MetaData/Start/Context", "Remove", False, True),
        ])
        self.assertEqual(string_table.diff(live, live), ())


if __name__ == "__main__":
    unittest.main()
