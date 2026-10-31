# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Markers: unrepresented content shown where it stands, warned on read, never written."""
from __future__ import annotations

import unittest

from asset_document import markers
from asset_document.diagnostics import AssetDocumentContractError, collecting_read_warnings

MARK = {"Unsupported": "the type does not load"}


class MarkerTests(unittest.TestCase):
    def test_recognition(self):
        self.assertTrue(markers.is_marker(MARK))
        self.assertTrue(markers.is_marker({"Name": "a", "Unsupported": "x", "Text": "t"}))
        self.assertFalse(markers.is_marker({"Unsupported": True}))
        self.assertFalse(markers.is_marker([MARK]))
        self.assertEqual(markers.marker("r", "t", Name="a"), {"Name": "a", "Unsupported": "r", "Text": "t"})

    def test_holds_marker_finds_markers_at_any_depth(self):
        self.assertTrue(markers.holds_marker(MARK))
        self.assertTrue(markers.holds_marker({"X": 1, "P": MARK}))
        self.assertTrue(markers.holds_marker([{"X": 1}, {"X": 2, "Inner": [MARK]}]))
        self.assertFalse(markers.holds_marker({"X": 1, "Unsupported": 2}))
        self.assertFalse(markers.holds_marker([1, "Unsupported", {"Y": [None]}]))

    def test_every_marker_warns_once_at_its_pointer(self):
        with collecting_read_warnings() as warnings:
            markers.warn_all({"a/b": [1, MARK], "c": {"d": dict(MARK, Unsupported="other")}}, "Profile", "/Body")
        self.assertEqual([(item.code, item.path) for item in warnings],
                         [("Profile.Unrepresented", "/Body/a~1b/1"), ("Profile.Unrepresented", "/Body/c/d")])

    def test_only_the_assets_own_marker_is_accepted(self):
        markers.require_unchanged("/x", dict(MARK), dict(MARK), "Profile")
        markers.require_unchanged("/x", 3, None, "Profile")
        for live in (None, 3, dict(MARK, Unsupported="other"), dict(MARK, Text="t")):
            with self.assertRaises(AssetDocumentContractError) as caught:
                markers.require_unchanged("/x", dict(MARK), live, "Profile")
            self.assertEqual((caught.exception.diagnostic.code, caught.exception.diagnostic.path),
                             ("Profile.UnrepresentedValue", "/x"))

    def test_text_naming_an_object_inside_the_asset_matches_in_a_copy_or_after_a_move(self):
        authored = dict(MARK, Text="/Script/Engine.Obj'/Game/Data/DT_Items.DT_Items:Inline_0'")
        with markers.target("/Game/Data/DT_Items.DT_Items"):
            for path in ("/Engine/Transient.DT_Items", "/Engine/Transient.DT_Items_1", "/Game/Data/DT_Items.DT_Items",
                         "/Temp/AssetDocument/ValidateTrial/DataTable_0123ABCD.DT_Items"):
                markers.require_unchanged("/x", authored, dict(MARK, Text=f"/Script/Engine.Obj'{path}:Inline_0'"),
                                          "Profile")
            with self.assertRaises(AssetDocumentContractError):
                markers.require_unchanged("/x", authored, dict(
                    MARK, Text="/Script/Engine.Obj'/Game/Data/DT_Items.DT_Items:Inline_1'"), "Profile")
        # The asset moved: the document (extracted before the move) targets the new path.
        with markers.target("/Game/Moved/DT_Items.DT_Items"):
            for path in ("/Game/Moved/DT_Items.DT_Items", "/Engine/Transient.DT_Items_2"):
                markers.require_unchanged("/x", authored, dict(MARK, Text=f"/Script/Engine.Obj'{path}:Inline_0'"),
                                          "Profile")

    def test_text_naming_another_object_compares_exactly(self):
        with markers.target("/Game/Data/DT_Items.DT_Items"):
            for authored, live in (
                    # Field paths of different owners.
                    ("/Script/Engine.Actor:Tags", "/Script/Engine.Pawn:Tags"),
                    # Another asset's objects, a copy of another asset, another asset's staged copy.
                    ("/Game/Data/DT_Other.DT_Other:Inline_0", "/Game/Data/DT_Else.DT_Else:Inline_0"),
                    ("/Game/Data/DT_Items.DT_Items:Inline_0", "/Engine/Transient.DT_Other_1:Inline_0"),
                    ("/Game/Data/DT_Items.DT_Items:Inline_0", "/Temp/AssetDocument/ValidateTrial/X_1.DT_Other:Inline_0"),
                    # A transient object whose name only starts like the asset's.
                    ("/Game/Data/DT_Items.DT_Items:Inline_0", "/Engine/Transient.DT_ItemsX:Inline_0")):
                with self.subTest(live=live), self.assertRaises(AssetDocumentContractError):
                    markers.require_unchanged("/x", dict(MARK, Text=authored), dict(MARK, Text=live), "Profile")
        # Without a target, text compares exactly.
        with self.assertRaises(AssetDocumentContractError):
            markers.require_unchanged("/x", dict(MARK, Text="/Game/Data/DT_Items.DT_Items:Inline_0"),
                                      dict(MARK, Text="/Engine/Transient.DT_Items_1:Inline_0"), "Profile")

    def test_rebase_text_names_paths_into_the_asset_as_the_target_does(self):
        target = "/Game/New/DT_Items_3.DT_Items_3"
        cases = {
            "/Game/Old/DT_Items_3.DT_Items_3:Inline_0.Deeper": "/Game/New/DT_Items_3.DT_Items_3:Inline_0.Deeper",
            "/Engine/Transient.DT_Items_7:Inline_0": "/Game/New/DT_Items_3.DT_Items_3:Inline_0",
            "/Script/Engine.Obj'/Game/Old/DT_Items_3.DT_Items_3'": "/Script/Engine.Obj'/Game/New/DT_Items_3.DT_Items_3'",
            "/Game/Old/DT_Items_3.Default__DT_Items_3_C:Comp": "/Game/New/DT_Items_3.Default__DT_Items_3_C:Comp",
            "(A=/Script/Engine.Actor:Tags,B=/Game/Other/DT.DT:X)": "(A=/Script/Engine.Actor:Tags,B=/Game/Other/DT.DT:X)",
            "/Game/Old/DT_Items_2.DT_Items_2:Inline_0": "/Game/Old/DT_Items_2.DT_Items_2:Inline_0",
        }
        for text, expected in cases.items():
            with self.subTest(text=text):
                self.assertEqual(markers.rebase_text(text, target), expected)
        self.assertEqual(markers.rebase_text(3, target), 3)
        self.assertEqual(markers.rebase_text("/Engine/Transient.DT_Items_7:X", None), "/Engine/Transient.DT_Items_7:X")

    def test_rebase_changes_only_marker_text(self):
        target = "/Game/New/DT.DT"
        document = {"Text": "/Game/Old/DT.DT:X", "Rows": [{"Name": "a", "Values": {
            "V": dict(MARK, Text="/Game/Old/DT.DT:Inline_0"), "W": 1}}], "Same": [dict(MARK, Text="kept")]}
        rebased = markers.rebase(document, target)
        self.assertEqual(rebased["Rows"][0]["Values"]["V"]["Text"], "/Game/New/DT.DT:Inline_0")
        self.assertEqual(rebased["Text"], "/Game/Old/DT.DT:X")
        self.assertIs(rebased["Same"], document["Same"])
        self.assertEqual(document["Rows"][0]["Values"]["V"]["Text"], "/Game/Old/DT.DT:Inline_0")
        self.assertIs(markers.rebase(document, None), document)

    def test_keep_puts_the_current_value_in_place_of_markers(self):
        authored = {"A": 1, "B": dict(MARK), "C": [dict(MARK), 5]}
        live = {"A": 0, "B": {"Deep": 2}, "C": [{"Raw": 1}, 4]}
        self.assertEqual(markers.keep(authored, live), {"A": 1, "B": {"Deep": 2}, "C": [{"Raw": 1}, 5]})

    def test_keep_pairs_list_items_by_identity(self):
        marked = markers.marker("x")
        live = [{"Id": "A", "Value": 1}, {"Id": "B", "Value": 2}, {"Id": "C", "Value": 3}]
        # Reordered, one removed, one added: each marker keeps its own item's value.
        authored = [{"Id": "C", "Value": marked}, {"Id": "N", "Value": marked}, {"Id": "A", "Value": marked}]
        self.assertEqual(markers.keep(authored, live),
                         [{"Id": "C", "Value": 3}, {"Id": "N", "Value": marked}, {"Id": "A", "Value": 1}])
        # Without an identity field items pair by index.
        self.assertEqual(markers.keep([marked, 7], [5, 6]), [5, 7])

    def test_a_former_marker_keeps_the_value_its_type_now_reads_as(self):
        former = {"Unsupported": "TMap has no document form; AssetDoc keeps the value.", "Text": "((\"/Game/A.A\", 3))"}
        optional = {"Unsupported": "TOptional has no document form; AssetDoc keeps the value.", "Text": "5"}
        self.assertTrue(markers.is_former(former, {"/Game/A.A": 3}))
        self.assertTrue(markers.is_former(optional, 5))
        self.assertFalse(markers.is_former(optional, None))
        self.assertFalse(markers.is_former(former, dict(former)))
        self.assertFalse(markers.is_former(dict(MARK), {"/Game/A.A": 3}))
        self.assertFalse(markers.is_former(dict(former, Name="x"), {"/Game/A.A": 3}))
        markers.require_unchanged("/x", former, {"/Game/A.A": 3}, "Profile")
        with self.assertRaises(AssetDocumentContractError):
            markers.require_unchanged("/x", dict(MARK), {"/Game/A.A": 3}, "Profile")
        authored = {"Counts": former, "Settings": {"Label": "Main", "Scale": optional}, "Other": dict(MARK)}
        live = {"Counts": {"/Game/A.A": 3}, "Settings": {"Label": "Main", "Scale": 0.5}, "Other": {"Raw": 1}}
        self.assertEqual(markers.adopt_former(authored, live),
                         {"Counts": {"/Game/A.A": 3}, "Settings": {"Label": "Main", "Scale": 0.5}, "Other": dict(MARK)})

    def test_value_comparisons_read_a_former_marker_as_the_live_value(self):
        from asset_document.diff import semantic_compare
        from asset_document.profiles import data_table_common as common
        former = {"Unsupported": "TOptional has no document form; AssetDoc keeps the value.", "Text": "5"}
        self.assertTrue(semantic_compare.same_value({"Count": former}, {"Count": 5}))
        self.assertTrue(common.same({"Count": former}, {"Count": 5}))
        self.assertFalse(common.same({"Count": dict(MARK)}, {"Count": 5}))


class MarkerComparisonInDiffAndVerifyTests(unittest.TestCase):
    """Diff and post-apply Verify compare marker text by the rule ``require_unchanged`` uses."""

    VERIFY_PHASE, APPLY_OPERATION, DIFF_OPERATION = 5, 5, 7

    def dispatch(self, operation, authored_text, live_text):
        from asset_document.diff.canonical_diff import canonical_diff
        from asset_document.registered_runtime import build_dispatch
        from asset_document.tests.runtime_registration_fixtures import Context, Handler, registry
        from asset_document.tests.test_generic_runtime import document as make_document, request

        class MarkedHandler(Handler):
            def extract(self, *, target, asset_handle, context, include_defaults=False):
                live = make_document()
                live["Body"] = {"Item": dict(MARK, Text=live_text)}
                return live

            def diff(self, authored, extracted):
                return canonical_diff(authored, extracted)

        frozen, _ = registry(MarkedHandler())
        authored = make_document()
        authored["Body"] = {"Item": dict(MARK, Text=authored_text)}
        return build_dispatch(frozen)(request(authored, operation=operation), Context(self.VERIFY_PHASE))

    def test_an_unchanged_marker_extracted_before_a_move_verifies_and_diffs_empty(self):
        # The fixture's target is /Game/Generic/DA_Generic.DA_Generic; the document was extracted at /Game/Old.
        before = "/Script/Engine.Obj'/Game/Old/DA_Generic.DA_Generic:Inline_0'"
        live = "/Script/Engine.Obj'/Game/Generic/DA_Generic.DA_Generic:Inline_0'"
        verified = self.dispatch(self.APPLY_OPERATION, before, live)
        self.assertEqual(verified["terminal_status"], 0, verified)
        self.assertEqual(verified["canonical_value"]["Body"]["Item"]["Text"], before)
        diffed = self.dispatch(self.DIFF_OPERATION, before, live)
        self.assertEqual(diffed["terminal_status"], 0, diffed)
        self.assertEqual(diffed["canonical_value"]["changed"], ())

    def test_a_marker_naming_another_field_owner_is_a_difference(self):
        verified = self.dispatch(self.APPLY_OPERATION, "/Script/Engine.Actor:Tags", "/Script/Engine.Pawn:Tags")
        self.assertNotEqual(verified["terminal_status"], 0)
        self.assertEqual([item["code"] for item in verified["diagnostics"]], ["GenericRuntime.VerificationMismatch"])
        diffed = self.dispatch(self.DIFF_OPERATION, "/Script/Engine.Actor:Tags", "/Script/Engine.Pawn:Tags")
        self.assertEqual([item["path"] for item in diffed["canonical_value"]["changed"]], ["/Body/Item/Text"])


if __name__ == "__main__":
    unittest.main()
