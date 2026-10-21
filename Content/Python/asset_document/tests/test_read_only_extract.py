"""Read-only extract of engine and plugin content: while an Extract reads it, the exact
read-only Target is admitted by the profiles, so it extracts and canonicalizes as the same
document under /Game would. The editor side (extract of engine assets, re-target and
Create) is covered by UEAssetDoc.EngineAssets.*."""
from __future__ import annotations

import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.profiles import anim_montage, data_table, data_table_common, skeleton
from asset_document.read_only_extract import (
    admits_read_only, extract_document, inspect_live, is_live_inspect_path, is_read_only_target)
from asset_document.regions import inspection_semantics

ENGINE_TABLE = "/Engine/Data/DT_Items"


def _table(target):
    return {"SchemaVersion": 1, "Target": target, "Class": data_table.DATA_TABLE_CLASS,
            "Action": "CreateOrUpdate", "Properties": {},
            "Body": {"RowStruct": "/Script/Engine.TableRowBase", "Rows": []}}


class ReadOnlyExtractTests(unittest.TestCase):
    def test_only_targets_outside_game_are_read_only(self):
        self.assertTrue(is_read_only_target("/Engine/EngineMaterials/WorldGridMaterial"))
        self.assertTrue(is_read_only_target("/UEAssetDoc/Data/DT_Items"))
        self.assertFalse(is_read_only_target("/Game/Data/DT_Items"))
        self.assertFalse(is_read_only_target(None))
        self.assertFalse(is_read_only_target("Generic"))

    def test_extract_admits_exactly_its_read_only_target(self):
        seen = []

        def extract():
            seen.append((admits_read_only(ENGINE_TABLE), admits_read_only("/Engine/Data/DT_Other")))
            return data_table_common.require_target(ENGINE_TABLE, "DataTable") and _table(ENGINE_TABLE)

        canonical = extract_document(ENGINE_TABLE, extract, data_table.canonicalize, read_only=True)
        self.assertEqual(seen, [(True, False)])
        self.assertEqual(canonical, {**data_table.canonicalize(_table("/Game/Data/DT_Items")), "Target": ENGINE_TABLE})
        self.assertFalse(admits_read_only(ENGINE_TABLE))
        with self.assertRaises(AssetDocumentContractError) as raised:
            data_table.canonicalize(_table(ENGINE_TABLE))
        self.assertEqual(raised.exception.diagnostic.code, "DataTable.NonCanonicalTarget")

    def test_other_operations_do_not_admit_it(self):
        with self.assertRaises(AssetDocumentContractError) as raised:
            extract_document(ENGINE_TABLE, lambda: _table(ENGINE_TABLE), data_table.canonicalize, read_only=False)
        self.assertEqual(raised.exception.diagnostic.code, "DataTable.NonCanonicalTarget")

    def test_admission_ends_when_the_extract_fails(self):
        def failing():
            raise RuntimeError("read failed")

        with self.assertRaises(RuntimeError):
            extract_document(ENGINE_TABLE, failing, data_table.canonicalize, read_only=True)
        self.assertFalse(admits_read_only(ENGINE_TABLE))

    def test_game_target_is_canonicalized_as_given(self):
        with self.assertRaises(AssetDocumentContractError) as raised:
            extract_document("/Game/Bad Name", lambda: _table("/Game/Bad Name"), data_table.canonicalize, read_only=True)
        self.assertEqual(raised.exception.diagnostic.code, "DataTable.NonCanonicalTarget")

    def test_profile_target_checks_admit_it(self):
        target = "/Engine/Tutorial/AM_Attack"
        self.assertEqual(extract_document(target, lambda: {"Target": anim_montage._require_target(target)},
                                          lambda document: document, read_only=True), {"Target": target})

    def test_inspect_reads_engine_and_plugin_assets(self):
        self.assertTrue(is_live_inspect_path("/Game/Data/DT_Items.DT_Items"))
        self.assertTrue(is_live_inspect_path("/Niagara/Enums/SpriteRenderer/ENiagara_ScaleSpriteSize.ENiagara_ScaleSpriteSize"))
        self.assertFalse(is_live_inspect_path("/Script/Engine.CurveFloat"))
        self.assertFalse(is_live_inspect_path("/Niagara/Enums/ENiagara_ScaleSpriteSize"))
        self.assertFalse(is_live_inspect_path(None))
        engine_enum = "/Niagara/Enums/SpriteRenderer/ENiagara_ScaleSpriteSize"
        seen = []

        def inspect():
            seen.append((admits_read_only(engine_enum), admits_read_only("/Niagara/Enums/Other")))
            return inspection_semantics._live_subject("live_asset", "/Script/Engine.UserDefinedEnum",
                                                      engine_enum + ".ENiagara_ScaleSpriteSize")

        self.assertEqual(inspect_live(engine_enum + ".ENiagara_ScaleSpriteSize", inspect)["asset_path"],
                         engine_enum + ".ENiagara_ScaleSpriteSize")
        self.assertEqual(seen, [(True, False)])
        self.assertFalse(admits_read_only(engine_enum))
        # Outside the Inspect read, the engine path is not a live subject.
        with self.assertRaises(AssetDocumentContractError):
            inspection_semantics._live_subject("live_asset", "/Script/Engine.UserDefinedEnum",
                                               engine_enum + ".ENiagara_ScaleSpriteSize")

    def test_skeleton_without_slots_extracts(self):
        document = {"SchemaVersion": 1, "Target": "/Game/Characters/SK_Hero", "Class": skeleton.CLASS,
                    "Action": "Update", "Properties": {}, "Body": {"SlotGroups": []}}
        self.assertEqual(skeleton.canonicalize(document)["Body"]["SlotGroups"], [])


if __name__ == "__main__":
    unittest.main()
