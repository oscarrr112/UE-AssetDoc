# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

import json
import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.dto import RuntimeOperation, RuntimeRequestDto, TerminalStatus
from asset_document.sync_evidence import (
    absent_hash,
    decode_document,
    dispatch_sync,
    evaluate,
    region_hashes,
    region_pointers,
)

BASE = {
    "SchemaVersion": 1,
    "Target": "/Game/Data/BB_Test",
    "Class": "/Script/AIModule.BlackboardData",
    "Action": "CreateOrUpdate",
    "Definitions": {},
    "Properties": {"A": 1},
    "Body": {"Parent": None, "Keys": [{"Name": "Target", "Type": "Object"}]},
}


def text(value) -> str:
    return json.dumps(value)


class RegionHashTests(unittest.TestCase):
    def test_regions_are_class_properties_and_each_body_key(self):
        self.assertEqual(region_pointers(BASE), ["/Class", "/Properties", "/Body/Parent", "/Body/Keys"])

    def test_body_keys_are_escaped_and_skipped_is_not_a_region(self):
        document = {**BASE, "Body": {"a/b~c": 1, "_Skipped": {"x": 1}}}
        self.assertEqual(region_pointers(document), ["/Class", "/Properties", "/Body/a~1b~0c"])

    def test_key_order_does_not_change_a_hash(self):
        reordered = decode_document('{"Body": {"Keys": [{"Type": "Object", "Name": "Target"}], "Parent": null},'
                                    ' "Properties": {"A": 1}, "Class": "/Script/AIModule.BlackboardData"}')
        self.assertEqual(region_hashes(reordered)["/Body/Keys"], region_hashes(BASE)["/Body/Keys"])

    def test_one_region_change_changes_only_that_hash(self):
        changed = json.loads(text(BASE))
        changed["Body"]["Keys"][0]["Name"] = "Other"
        before, after = region_hashes(BASE), region_hashes(changed)
        self.assertNotEqual(before["/Body/Keys"], after["/Body/Keys"])
        for pointer in ("/Class", "/Properties", "/Body/Parent"):
            self.assertEqual(before[pointer], after[pointer])

    def test_absent_empty_and_null_are_distinct(self):
        absent = region_hashes({"Class": "C"})["/Properties"]
        empty = region_hashes({"Class": "C", "Properties": {}})["/Properties"]
        null = region_hashes({"Class": "C", "Properties": None})["/Properties"]
        self.assertEqual(absent, absent_hash())
        self.assertEqual(len({absent, empty, null}), 3)

    def test_integer_and_float_differ(self):
        self.assertNotEqual(region_hashes(decode_document('{"Properties": {"A": 1}}'))["/Properties"],
                            region_hashes(decode_document('{"Properties": {"A": 1.5}}'))["/Properties"])

    def test_target_action_base_revision_and_meta_are_not_hashed(self):
        other = {**BASE, "Target": "/Game/Other/BB", "Action": "Update", "BaseRevision": "r",
                 "_meta": {"sync": {"Version": 1}}}
        value, _ = evaluate({"Mode": "evidence", "Text": text(other)})
        self.assertEqual(value["Regions"], region_hashes(BASE))

    def test_a_definition_change_changes_the_regions_that_reach_it(self):
        document = {
            "Class": "C",
            "Definitions": {"a": {"Kind": "DefinitionRef", "Id": "b"}, "b": {"Kind": "Class", "Path": "/Script/X.Y"},
                            "unused": {"Kind": "Class", "Path": "/Script/X.Z"}},
            "Properties": {"P": {"Kind": "DefinitionRef", "Id": "a"}},
            "Body": {"K": [1]},
        }
        changed = json.loads(text(document))
        changed["Definitions"]["b"]["Path"] = "/Script/X.W"
        unused = json.loads(text(document))
        unused["Definitions"]["unused"]["Path"] = "/Script/X.V"
        self.assertNotEqual(region_hashes(document)["/Properties"], region_hashes(changed)["/Properties"])
        self.assertEqual(region_hashes(document)["/Body/K"], region_hashes(changed)["/Body/K"])
        self.assertEqual(region_hashes(document), region_hashes(unused))

    def test_hashes_carry_the_codec_version(self):
        self.assertTrue(all(value.startswith("adsh:v1:region:") for value in region_hashes(BASE).values()))


class SyncModeTests(unittest.TestCase):
    def test_mirror_stamps_its_own_region_hashes_and_moves_skipped(self):
        extract = {**BASE, "Action": "Create", "Body": {**BASE["Body"], "_Skipped": {"Graph": "unsupported"}}}
        value, encoded = evaluate({"Mode": "mirror", "Text": text(extract), "Package": "abc"})
        mirror = json.loads(encoded)
        self.assertEqual(mirror["Action"], "CreateOrUpdate")
        self.assertNotIn("_Skipped", mirror["Body"])
        self.assertEqual(mirror["_meta"]["Skipped"], {"Graph": "unsupported"})
        sync = mirror["_meta"]["sync"]
        self.assertEqual((sync["Version"], sync["Package"]), (1, "abc"))
        self.assertEqual({pointer: row["Sidecar"] for pointer, row in sync["Regions"].items()}, value["Regions"])
        self.assertEqual({pointer: row["Asset"] for pointer, row in sync["Regions"].items()}, value["Regions"])
        reread, _ = evaluate({"Mode": "evidence", "Text": encoded.decode("utf-8")})
        self.assertEqual(reread["Regions"], value["Regions"])
        self.assertEqual(list(mirror)[-1], "_meta")
        self.assertTrue(encoded.endswith(b"\n"))

    def test_mirror_of_its_own_extract_evidence_is_equal(self):
        extract_evidence, _ = evaluate({"Mode": "evidence", "Text": text(BASE)})
        mirror_evidence, _ = evaluate({"Mode": "mirror", "Text": text(BASE), "Package": ""})
        self.assertEqual(extract_evidence["Regions"], mirror_evidence["Regions"])

    def test_merge_takes_only_the_named_regions_and_keeps_meta(self):
        sidecar = {**BASE, "Properties": {"A": 2}, "Body": {"Parent": None, "Keys": []}, "_meta": {"sync": {"Version": 1}}}
        _, encoded = evaluate({"Mode": "merge", "Text": text(sidecar), "From": text(BASE), "Regions": ["/Body/Keys"]})
        merged = json.loads(encoded)
        self.assertEqual(merged["Body"]["Keys"], BASE["Body"]["Keys"])
        self.assertEqual(merged["Properties"], {"A": 2})
        self.assertEqual(merged["_meta"], {"sync": {"Version": 1}})

    def test_merge_removes_a_region_absent_in_the_asset(self):
        sidecar = {**BASE, "Body": {**BASE["Body"], "Extra": 1}}
        _, encoded = evaluate({"Mode": "merge", "Text": text(sidecar), "From": text(BASE), "Regions": ["/Body/Extra"]})
        self.assertNotIn("Extra", json.loads(encoded)["Body"])

    def test_conflict_names_its_regions(self):
        _, encoded = evaluate({"Mode": "conflict", "Text": text(BASE), "Regions": ["/Properties"]})
        self.assertEqual(json.loads(encoded)["_meta"], {"conflict": {"Regions": ["/Properties"]}})

    def test_invalid_text_is_a_stable_diagnostic(self):
        for bad in ("{", "[]", '{"A": NaN}'):
            with self.assertRaises(AssetDocumentContractError) as caught:
                evaluate({"Mode": "evidence", "Text": bad})
            self.assertEqual(caught.exception.diagnostic.code, "Sync.InvalidSidecar")

    def test_unknown_mode_and_region_are_refused(self):
        with self.assertRaises(AssetDocumentContractError):
            evaluate({"Mode": "other", "Text": text(BASE)})
        with self.assertRaises(AssetDocumentContractError):
            evaluate({"Mode": "merge", "Text": text(BASE), "From": text(BASE), "Regions": ["/Target"]})

    def test_dispatch_returns_value_and_bytes(self):
        request = RuntimeRequestDto(
            request_id="r", operation=RuntimeOperation.SYNC, target_asset_path="/Game/A.A",
            requested_profile_class_path="", requested_asset_class_path="/Script/CoreUObject.Object",
            canonical_document={"Mode": "mirror", "Text": text(BASE), "Package": "p"}, expected_runtime_revision=3)
        result = dispatch_sync(request, 3)
        self.assertIs(result.terminal_status, TerminalStatus.SUCCEEDED)
        self.assertEqual(set(result.canonical_value.value), {"Regions", "AbsentHash"})
        self.assertTrue(result.canonical_bytes.startswith(b"{"))
        self.assertTrue(result.semantic_hash and result.codec_version > 0)

    def test_dispatch_without_bytes_has_no_byte_evidence(self):
        request = RuntimeRequestDto(
            request_id="r", operation=RuntimeOperation.SYNC, target_asset_path="/Game/A.A",
            requested_profile_class_path="", requested_asset_class_path="/Script/CoreUObject.Object",
            canonical_document={"Mode": "evidence", "Text": text(BASE)}, expected_runtime_revision=3)
        result = dispatch_sync(request, 3)
        self.assertEqual((result.canonical_bytes, result.semantic_hash, result.codec_version), (b"", "", 0))


if __name__ == "__main__":
    unittest.main()
