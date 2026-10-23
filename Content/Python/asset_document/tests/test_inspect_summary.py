# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""The shared summarized inspect: inspect over HTTP outlines a large document Body and leaves out the bulk facts
unless detail is true, with one Summary line."""
from __future__ import annotations

import json
import unittest
from types import SimpleNamespace

from asset_document import inspect_summary as summary
from asset_document import runtime
from asset_document.profiles import control_rig


def _node(index):
    return {"Unit": "RigUnit_SetTransform", "Position": [index * 100.0, 0.0],
            "Defaults": {"Item": f"Bone(bone_{index})", "Weight": 0.5, "bPropagateToChildren": True}}


LARGE_BODY = {
    "Hierarchy": [{"Type": "Bone", "Name": f"bone_{index}", "Imported": True} for index in range(60)],
    "Graphs": {"Rig": {"Nodes": {f"Set_{index}": _node(index) for index in range(30)},
                       "Order": [f"Set_{index}" for index in range(30)]}},
    "Variables": [],
}


class OutlineTests(unittest.TestCase):
    def test_small_values_stay_whole(self):
        self.assertEqual(summary.outline({"Mode": "Custom", "Width": 242}), {"Mode": "Custom", "Width": 242})
        self.assertEqual(summary.outline("text"), "text")

    def test_named_entries_are_listed_by_name(self):
        self.assertEqual(summary.outline(LARGE_BODY["Hierarchy"])[:2], ["Bone(bone_0)", "Bone(bone_1)"])
        self.assertEqual(summary.outline(LARGE_BODY["Hierarchy"])[-1], "+20 more")
        self.assertEqual(len(summary.outline(LARGE_BODY["Hierarchy"])), 41)

    def test_objects_by_key_then_counts(self):
        graphs = summary.outline(LARGE_BODY["Graphs"])
        self.assertEqual(graphs, {"Rig": {"Nodes": "30 entries", "Order": "30 entries"}})
        many = {f"Expression_{index}": _node(index) for index in range(50)}
        self.assertEqual(summary.outline({"Expressions": many})["Expressions"][-1], "+10 more")


class SummarizeTests(unittest.TestCase):
    def test_a_large_body_is_outlined_with_one_summary_line(self):
        inspected = {"Target": "/Game/CR", "Body": LARGE_BODY, "Counts": {"nodes": 30}, "Pins": {"x": [1] * 500}}
        out = summary.summarize(inspected, drop=("Pins", "Missing"))
        self.assertEqual(out["Body"]["Graphs"], {"Rig": {"Nodes": "30 entries", "Order": "30 entries"}})
        self.assertEqual(out["Body"]["Variables"], [])
        self.assertEqual(out["Counts"], {"nodes": 30})
        self.assertNotIn("Pins", out)
        self.assertEqual(out["Summary"], "Summarized: Pins left out; Body as an outline (extract_asset_document returns "
                                         "the document). Inspect with detail true for the whole.")
        self.assertIn("Pins", inspected)  # the input is not changed

    def test_nothing_to_summarize_is_returned_whole(self):
        inspected = {"Target": "/Game/C", "Body": {"Keys": [{"Time": 0.0, "Value": 1.0}]}}
        self.assertIs(summary.summarize(inspected), inspected)

    def test_facts_and_profile_words(self):
        out = summary.summarize({"Details": {"A": [1, 2, 3]}}, "Details as counts",
                                facts={"Details": lambda value: {key: len(item) for key, item in value.items()}})
        self.assertEqual(out["Details"], {"A": 3})
        self.assertEqual(out["Summary"], "Summarized: Details as counts. Inspect with detail true for the whole.")

    def test_runtime_summarizes_every_profile(self):
        """A profile without its own summarize_inspect gets the shared outline; detail (no summary flag) is whole."""
        inspected = {"Target": "/Game/W", "Body": LARGE_BODY}
        summary_request = SimpleNamespace(canonical_document={"mode": "live_asset", "summary": True})
        detail_request = SimpleNamespace(canonical_document={"mode": "live_asset"})
        self.assertIs(runtime.summarized(object(), detail_request, inspected), inspected)
        self.assertEqual(runtime.summarized(object(), summary_request, inspected), summary.summarize(inspected))

    def test_control_rig_leaves_out_pins_and_global_transforms(self):
        inspected = {"Body": LARGE_BODY, "Counts": {}, "Pins": {}, "GlobalTransforms": {}, "Events": ["Forwards Solve"]}
        out = control_rig.ControlRigRuntimeProfile().summarize_inspect(inspected)
        self.assertEqual(sorted(out), ["Body", "Counts", "Events", "Summary"])
        self.assertIn("Pins, GlobalTransforms left out", out["Summary"])


def _row(name, value, default=None, *, token="Float", writable=True):
    return {"name": name, "ue_type": "double", "type_token": token, "current_value": value,
            "default_value": value if default is None else default, "writable": writable}


BODY_INSTANCE = {f"Field{index}": index * 1.5 for index in range(40)}
MESH = {"Class": "/Script/Engine.SkeletalMeshComponent", "Properties": {"BodyInstance": BODY_INSTANCE}}


def _class_properties():
    rows = [_row(f"Value{index:03d}", float(index)) for index in range(80)]
    rows.append(_row("BodyInstance", BODY_INSTANCE, token="FBodyInstance"))
    rows.append(_row("Mesh", MESH, token="InstancedObject"))
    rows.append(_row("Locked", 1.0, writable=False))
    skipped = [{"name": f"Hidden{index}", "reason": "non-editable"} for index in range(30)]
    skipped.append({"name": "Materials", "reason": "saved-by-asset"})
    return {"class": "/Script/Engine.Thing", "properties": rows, "skipped": skipped, "selector_policies": []}


class PropertySummaryTests(unittest.TestCase):
    def test_class_defaults_keep_names_types_and_small_values(self):
        inspected = _class_properties()
        out = summary.summarize(inspected)
        rows = {row["name"]: row for row in out["properties"]}
        self.assertEqual(rows["Value001"], {"name": "Value001", "type_token": "Float", "default_value": 1.0})
        self.assertEqual(rows["BodyInstance"], {"name": "BodyInstance", "type_token": "FBodyInstance", "size": "40 fields"})
        self.assertEqual(rows["Mesh"], {"name": "Mesh", "type_token": "InstancedObject",
                                        "class": "/Script/Engine.SkeletalMeshComponent", "size": "1 property"})
        self.assertEqual(rows["Locked"]["writable"], False)
        self.assertEqual(out["skipped"], {"non-editable": [f"Hidden{index}" for index in range(30)],
                                          "saved-by-asset": ["Materials"]})
        self.assertIn("default_value", out["Summary"])
        self.assertLess(len(json.dumps(out)), len(json.dumps(inspected)) // 2)
        self.assertEqual(inspected["properties"][0]["ue_type"], "double")  # the input is not changed

    def test_live_asset_says_what_is_overridden(self):
        inspected = {**_class_properties(), "asset_path": "/Game/Thing.Thing"}
        inspected["properties"][0] = _row("Value000", 5.0, 0.0)
        inspected["properties"][80] = _row("BodyInstance", {**BODY_INSTANCE, "Field0": 9.0}, BODY_INSTANCE,
                                           token="FBodyInstance")
        rows = {row["name"]: row for row in summary.summarize(inspected)["properties"]}
        self.assertEqual(rows["Value000"], {"name": "Value000", "type_token": "Float", "current_value": 5.0,
                                            "overridden": True, "default_value": 0.0})
        self.assertEqual(rows["Value001"], {"name": "Value001", "type_token": "Float", "current_value": 1.0})
        self.assertEqual(rows["BodyInstance"], {"name": "BodyInstance", "type_token": "FBodyInstance",
                                                "size": "40 fields", "overridden": True})

    def test_small_property_lists_stay_whole(self):
        inspected = {"class": "/Script/Engine.CurveFloat", "properties": [_row("A", 1.0)], "skipped": []}
        self.assertIs(summary.summarize(inspected), inspected)

    def test_runtime_summarizes_class_defaults(self):
        inspected = _class_properties()
        request = SimpleNamespace(canonical_document={"mode": "class_defaults", "summary": True})
        self.assertEqual(runtime.summarized(object(), request, inspected), summary.summarize(inspected))


if __name__ == "__main__":
    unittest.main()
