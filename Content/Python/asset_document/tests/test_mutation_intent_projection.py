# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Focused checks for the dormant A0 pure-value protocol."""
import copy
import unittest
from asset_document.mutation_intent_projection import build_mutation_intent_projection, validate_mutation_intent_projection


class MutationIntentProjectionTests(unittest.TestCase):
    def setUp(self):
        self.identity = dict(request_id="12345678-1234-1234-1234-123456789abc", runtime_revision=7,
                             exact_asset_class_path="/Script/Engine.AnimMontage",
                             target_asset_path="/Game/Test.Test", semantic_hash="validated-hash")
        self.steps = [dict(Index=0, PrimitiveId="editor.test.v1", Version=1, Phase="Apply",
                           InputSchemaId="TestIntentV1", Intent={"TargetAssetPath": "/Game/Test.Test",
                                                                  "Desired": [None, True, 4, 1.5, "value"]})]

    def build(self):
        return build_mutation_intent_projection(**self.identity, steps=self.steps)

    def validate(self, value):
        return validate_mutation_intent_projection(value, **self.identity)

    def test_exact_projection_and_detached_intent(self):
        value = self.build()
        self.assertEqual(value["Kind"], "MutationIntentProjectionV1")
        self.assertEqual(value["Steps"], self.steps)
        self.steps[0]["Intent"]["Desired"].append("later")
        self.assertEqual(len(value["Steps"][0]["Intent"]["Desired"]), 5)
        result = self.validate(value)
        result["Steps"][0]["Intent"]["Desired"].clear()
        self.assertEqual(len(value["Steps"][0]["Intent"]["Desired"]), 5)

    def test_root_and_step_are_closed(self):
        for level in ("root", "step"):
            for change in ("extra", "missing"):
                with self.subTest(level=level, change=change):
                    value = self.build()
                    node = value if level == "root" else value["Steps"][0]
                    if change == "extra":
                        node["RequiredByRouteId"] = "not-protocol"
                    else:
                        node.pop("Kind" if level == "root" else "Intent")
                    with self.assertRaises(ValueError):
                        self.validate(value)

    def test_every_identity_binding_is_checked(self):
        for key, wrong in (("RequestId", "22345678-1234-1234-1234-123456789abc"),
                           ("RuntimeRevision", 8), ("ExactAssetClassPath", "/Script/Other.Class"),
                           ("TargetAssetPath", "/Game/Other.Other"), ("SemanticHash", "other"),
                           ("Kind", "Other"), ("Operation", "Validate")):
            with self.subTest(key=key):
                value = self.build()
                value[key] = wrong
                with self.assertRaises(ValueError):
                    self.validate(value)

    def test_apply_repair_sequence_and_empty_steps(self):
        self.steps.append({**copy.deepcopy(self.steps[0]), "Index": 1, "Phase": "Repair"})
        self.assertEqual(self.build()["Steps"][1]["Phase"], "Repair")
        self.steps.clear()
        self.assertEqual(self.build()["Steps"], [])

    def test_invalid_step_types_indices_and_phases(self):
        for key, bad in (("Index", True), ("Index", 1), ("Version", 0), ("Version", True),
                         ("Version", 1 << 32), ("Phase", "Preflight"), ("Phase", "Persist"),
                         ("InputSchemaId", ""), ("PrimitiveId", ""), ("Intent", [])):
            with self.subTest(key=key, bad=bad):
                value = self.build()
                value["Steps"][0][key] = bad
                with self.assertRaises(ValueError):
                    self.validate(value)

    def test_no_body_handles_addresses_or_rollback_envelopes(self):
        for key in ("Body", "Handle", "Handles", "HandleArguments", "OpaqueHandle", "Pointer",
                    "Address", "Rollback", "RollbackCode", "TransactionId", "Slot", "Generation"):
            with self.subTest(key=key):
                self.steps[0]["Intent"] = {"Nested": [{key: "invalid"}]}
                with self.assertRaises(ValueError):
                    self.build()

    def test_invalid_native_values_and_nonfinite_numbers(self):
        for bad in (object(), b"bytes", float("nan"), float("inf"), 1 << 63, {1: "key"}, (1, 2)):
            with self.subTest(bad=type(bad)):
                self.steps[0]["Intent"] = {"Desired": bad}
                with self.assertRaises(ValueError):
                    self.build()

    def test_invalid_request_identity(self):
        for key, bad in (("request_id", "not-guid"), ("request_id", "00000000-0000-0000-0000-000000000000"),
                         ("runtime_revision", True), ("runtime_revision", 0), ("runtime_revision", 1 << 63)):
            with self.subTest(key=key, bad=bad):
                arguments = {**self.identity, key: bad}
                with self.assertRaises(ValueError):
                    build_mutation_intent_projection(**arguments, steps=self.steps)


if __name__ == "__main__":
    unittest.main()
