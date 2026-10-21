from dataclasses import FrozenInstanceError
import unittest

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.canonical.semantic_hash import SemanticHashScope, compute_semantic_hash
from asset_document.diff.canonical_diff import canonical_diff, canonical_equal
from asset_document.diagnostics import AssetDocumentContractError
from asset_document.diff.model import (
    DiffKind,
    Difference,
    ReconciliationConflict,
    ReconciliationPolicy,
    ReconciliationResult,
)
from asset_document.diff.reconciliation import reconcile_three_way
from asset_document.tests.property_cases import generated_json_values


class CanonicalDiffTests(unittest.TestCase):
    def test_diff_golden_paths_kinds_and_values(self) -> None:
        before = {"A/B": 1, "Body": {"Items": [1, True], "Removed": []}, "Tilde~": 0}
        after = {"A/B": 2, "Body": {"Items": [1, 1.0], "Added": {}}, "Tilde~": 0}
        actual = canonical_diff(before, after)
        self.assertEqual(
            tuple((item.path, item.kind) for item in actual),
            (
                ("/A~1B", DiffKind.REPLACE),
                ("/Body/Added", DiffKind.ADD),
                ("/Body/Items/1", DiffKind.REPLACE),
                ("/Body/Removed", DiffKind.REMOVE),
            ),
        )
        self.assertEqual(
            tuple(
                (item.before_present, item.before, item.after_present, item.after)
                for item in actual
            ),
            (
                (True, 1, True, 2),
                (False, None, True, {}),
                (True, True, True, 1.0),
                (True, [], False, None),
            ),
        )

    def test_root_replacement_uses_empty_pointer(self) -> None:
        self.assertEqual(
            canonical_diff(1, {"A": 1}),
            (Difference("", DiffKind.REPLACE, True, 1, True, {"A": 1}),),
        )

    def test_explicit_empty_and_null_are_distinct_from_absent(self) -> None:
        actual = canonical_diff({}, {"Array": [], "Null": None, "Object": {}})
        self.assertEqual(
            tuple((item.path, item.kind, item.after_present, item.after) for item in actual),
            (
                ("/Array", DiffKind.ADD, True, []),
                ("/Null", DiffKind.ADD, True, None),
                ("/Object", DiffKind.ADD, True, {}),
            ),
        )

    def test_object_key_order_is_insensitive(self) -> None:
        left = {"B": 2, "A": {"Y": 2, "X": 1}}
        right = {"A": {"X": 1, "Y": 2}, "B": 2}
        self.assertTrue(canonical_equal(left, right))
        self.assertEqual(canonical_diff(left, right), ())

    def test_array_order_is_sensitive(self) -> None:
        self.assertFalse(canonical_equal([1, 2], [2, 1]))
        self.assertEqual(
            tuple(item.path for item in canonical_diff([1, 2], [2, 1])),
            ("/0", "/1"),
        )

    def test_pointer_escaping_covers_tilde_and_slash(self) -> None:
        actual = canonical_diff({}, {"Tilde~": 1, "Slash/": 2, "Both~/": 3})
        self.assertEqual(
            tuple(item.path for item in actual),
            ("/Both~0~1", "/Slash~1", "/Tilde~0"),
        )

    def test_boolean_integer_and_float_shapes_are_distinct(self) -> None:
        for left, right in ((True, 1), (1, 1.0), (False, 0), (0, 0.0)):
            with self.subTest(left=left, right=right):
                self.assertFalse(canonical_equal(left, right))
                self.assertEqual(canonical_diff(left, right)[0].kind, DiffKind.REPLACE)

    def test_signed_zero_is_a_replacement(self) -> None:
        self.assertFalse(canonical_equal(-0.0, 0.0))
        actual = canonical_diff(-0.0, 0.0)
        self.assertEqual((actual[0].path, actual[0].kind), ("", DiffKind.REPLACE))

    def test_difference_records_snapshot_nested_add_remove_and_root_replace_values(self) -> None:
        before = {"Removed": {"Nested": [1, {"Value": "before"}]}}
        after = {"Added": {"Nested": [2, {"Value": "after"}]}}
        records = canonical_diff(before, after)
        frozen_repr = repr(records)
        before["Removed"]["Nested"][1]["Value"] = "mutated"
        after["Added"]["Nested"][1]["Value"] = "mutated"
        self.assertEqual(repr(records), frozen_repr)
        with self.assertRaises(FrozenInstanceError):
            records[0].path = "/mutated"

        root_before = {"Nested": [1]}
        root_after = [root_before]
        root_record = canonical_diff(root_before, root_after)
        root_before["Nested"].append(2)
        root_after.append(None)
        self.assertEqual(root_record[0].before, {"Nested": [1]})
        self.assertEqual(root_record[0].after, [{"Nested": [1]}])

    def test_array_zero_one_many_and_mutation_sensitivity(self) -> None:
        self.assertEqual(canonical_diff([], []), ())
        self.assertEqual(
            tuple((item.path, item.kind) for item in canonical_diff([], [1])),
            (("/0", DiffKind.ADD),),
        )
        self.assertEqual(
            tuple((item.path, item.kind) for item in canonical_diff([1, 2, 3], [1])),
            (("/1", DiffKind.REMOVE), ("/2", DiffKind.REMOVE)),
        )
        self.assertEqual(canonical_diff({"A": 1}, {"A": 1}), ())
        self.assertEqual(canonical_diff({"A": 1}, {"A": 2})[0].kind, DiffKind.REPLACE)

    def test_generated_equality_diff_and_hash_invariants(self) -> None:
        codec = DocumentCodec()
        values = generated_json_values(seed=0xADD1, count=512)
        for index, left in enumerate(values):
            right = values[(index + 1) % len(values)]
            with self.subTest(index=index):
                self.assertEqual(canonical_diff(left, left), ())
                roundtrip = codec.decode(codec.encode(left))
                self.assertTrue(canonical_equal(left, roundtrip))
                self.assertEqual(canonical_equal(left, right), canonical_equal(right, left))
                self.assertEqual(
                    canonical_equal(left, right), codec.encode(left) == codec.encode(right)
                )
                if canonical_equal(left, right):
                    for scope in SemanticHashScope:
                        self.assertEqual(
                            compute_semantic_hash(left, scope=scope),
                            compute_semantic_hash(right, scope=scope),
                        )
                self.assertEqual(repr(canonical_diff(left, right)), repr(canonical_diff(left, right)))

    def test_difference_public_constructor_rejects_invalid_representations(self) -> None:
        cases = (
            ("path", "/Diff/Path", lambda: Difference(1, DiffKind.ADD, False, None, True, 1)),
            ("kind", "/Diff/Kind", lambda: Difference("/A", "Add", False, None, True, 1)),
            ("before-presence", "/A/Before", lambda: Difference("/A", DiffKind.REPLACE, 1, 1, True, 2)),
            ("after-presence", "/A/After", lambda: Difference("/A", DiffKind.REPLACE, True, 1, 1, 2)),
            ("absent-before-value", "/A/Before", lambda: Difference("/A", DiffKind.ADD, False, 1, True, 2)),
            ("absent-after-value", "/A/After", lambda: Difference("/A", DiffKind.REMOVE, True, 1, False, 2)),
            ("invalid-before-json", "/A/Before", lambda: Difference("/A", DiffKind.REPLACE, True, b"value", True, 2)),
            ("invalid-after-json", "/A/After", lambda: Difference("/A", DiffKind.REPLACE, True, 1, True, bytearray())),
        )
        for name, path, factory in cases:
            with self.subTest(case=name):
                with self.assertRaises(AssetDocumentContractError) as caught:
                    factory()
                self.assertEqual(
                    (caught.exception.diagnostic.code, caught.exception.diagnostic.path),
                    ("DTO.InvalidRepresentationType", path),
                )

    def test_reconciliation_conflict_public_constructor_rejects_invalid_representations(self) -> None:
        def conflict(**overrides) -> ReconciliationConflict:
            values = {
                "path": "/A",
                "base_present": True,
                "base": 1,
                "asset_present": True,
                "asset": 2,
                "sidecar_present": True,
                "sidecar": 3,
            }
            values.update(overrides)
            return ReconciliationConflict(**values)

        cases = (
            ("path", "/Reconciliation/Conflict/Path", lambda: conflict(path=1)),
            ("base-presence", "/A/Base", lambda: conflict(base_present=1)),
            ("asset-presence", "/A/Asset", lambda: conflict(asset_present=1)),
            ("sidecar-presence", "/A/Sidecar", lambda: conflict(sidecar_present=1)),
            ("absent-base-value", "/A/Base", lambda: conflict(base_present=False, base=1)),
            ("absent-asset-value", "/A/Asset", lambda: conflict(asset_present=False, asset=2)),
            ("absent-sidecar-value", "/A/Sidecar", lambda: conflict(sidecar_present=False, sidecar=3)),
            ("invalid-base-json", "/A/Base", lambda: conflict(base=b"value")),
            ("invalid-asset-json", "/A/Asset", lambda: conflict(asset=bytearray())),
            ("invalid-sidecar-json", "/A/Sidecar", lambda: conflict(sidecar=(1,))),
        )
        for name, path, factory in cases:
            with self.subTest(case=name):
                with self.assertRaises(AssetDocumentContractError) as caught:
                    factory()
                self.assertEqual(
                    (caught.exception.diagnostic.code, caught.exception.diagnostic.path),
                    ("DTO.InvalidRepresentationType", path),
                )

    def test_reconciliation_result_public_constructor_rejects_invalid_representations(self) -> None:
        conflict = ReconciliationConflict("/A", True, 1, True, 2, True, 3)
        difference = Difference("/A", DiffKind.REPLACE, True, 1, True, 2)

        def result(**overrides) -> ReconciliationResult:
            values = {
                "resolved": True,
                "value": 2,
                "conflicts": (conflict,),
                "asset_changes": (difference,),
                "sidecar_changes": (difference,),
                "diagnostic_code": None,
            }
            values.update(overrides)
            return ReconciliationResult(**values)

        cases = (
            ("resolved", "/Reconciliation/Resolved", lambda: result(resolved=1)),
            ("invalid-value", "/Reconciliation/Value", lambda: result(value=b"value")),
            ("unresolved-partial-value", "/Reconciliation/Value", lambda: result(resolved=False, value=1)),
            ("conflicts-container", "/Reconciliation/Conflicts", lambda: result(conflicts=[])),
            ("conflicts-element", "/Reconciliation/Conflicts", lambda: result(conflicts=("conflict",))),
            ("asset-container", "/Reconciliation/AssetChanges", lambda: result(asset_changes=[])),
            ("asset-element", "/Reconciliation/AssetChanges", lambda: result(asset_changes=("change",))),
            ("sidecar-container", "/Reconciliation/SidecarChanges", lambda: result(sidecar_changes=[])),
            ("sidecar-element", "/Reconciliation/SidecarChanges", lambda: result(sidecar_changes=("change",))),
            ("diagnostic-code", "/Reconciliation/DiagnosticCode", lambda: result(diagnostic_code=1)),
        )
        for name, path, factory in cases:
            with self.subTest(case=name):
                with self.assertRaises(AssetDocumentContractError) as caught:
                    factory()
                self.assertEqual(
                    (caught.exception.diagnostic.code, caught.exception.diagnostic.path),
                    ("DTO.InvalidRepresentationType", path),
                )


class ReconciliationTests(unittest.TestCase):
    def test_no_change_returns_base(self) -> None:
        base = {"A": 1, "Nested": [True]}
        result = reconcile_three_way(base, base, base)
        self.assertTrue(result.resolved)
        self.assertTrue(canonical_equal(result.value, base))
        self.assertEqual((result.conflicts, result.asset_changes, result.sidecar_changes), ((), (), ()))

    def test_asset_only_change_returns_asset(self) -> None:
        base = {"A": 1}
        asset = {"A": 2}
        result = reconcile_three_way(base, asset, base)
        self.assertTrue(result.resolved)
        self.assertEqual(result.value, asset)
        self.assertEqual(tuple(item.path for item in result.asset_changes), ("/A",))

    def test_sidecar_only_change_returns_sidecar(self) -> None:
        base = {"A": 1}
        sidecar = {"A": 2}
        result = reconcile_three_way(base, base, sidecar)
        self.assertTrue(result.resolved)
        self.assertEqual(result.value, sidecar)
        self.assertEqual(tuple(item.path for item in result.sidecar_changes), ("/A",))

    def test_equal_dual_change_converges(self) -> None:
        result = reconcile_three_way({"A": 1}, {"A": 2}, {"A": 2})
        self.assertTrue(result.resolved)
        self.assertEqual(result.value, {"A": 2})
        self.assertEqual(result.conflicts, ())

    def test_non_overlapping_object_changes_merge(self) -> None:
        result = reconcile_three_way(
            {"A": 1, "B": 1},
            {"A": 2, "B": 1},
            {"A": 1, "B": 2},
        )
        self.assertTrue(result.resolved)
        self.assertEqual(result.value, {"A": 2, "B": 2})
        self.assertEqual(result.conflicts, ())

    def test_delete_and_unrelated_update_merge(self) -> None:
        result = reconcile_three_way(
            {"A": 1, "B": 1}, {"B": 1}, {"A": 1, "B": 2}
        )
        self.assertTrue(result.resolved)
        self.assertEqual(result.value, {"B": 2})

    def test_same_path_divergence_fails_closed(self) -> None:
        result = reconcile_three_way({"A": 1}, {"A": 2}, {"A": 3})
        self.assertFalse(result.resolved)
        self.assertIsNone(result.value)
        self.assertEqual(result.diagnostic_code, "Reconciliation.Conflict")
        self.assertEqual(tuple(item.path for item in result.conflicts), ("/A",))

    def test_divergent_array_edits_conflict(self) -> None:
        result = reconcile_three_way([1, 2], [1, 3], [1, 4])
        self.assertFalse(result.resolved)
        self.assertEqual(tuple(item.path for item in result.conflicts), ("",))

    def test_add_delete_collision_conflicts(self) -> None:
        result = reconcile_three_way({"A": 1}, {}, {"A": 2})
        self.assertFalse(result.resolved)
        conflict = result.conflicts[0]
        self.assertEqual(conflict.path, "/A")
        self.assertFalse(conflict.asset_present)
        self.assertTrue(conflict.sidecar_present)

    def test_signed_zero_divergence_uses_codec_equality_and_conflicts(self) -> None:
        result = reconcile_three_way({"A": 1.0}, {"A": -0.0}, {"A": 0.0})
        self.assertFalse(result.resolved)
        self.assertEqual(tuple(item.path for item in result.conflicts), ("/A",))

    def test_explicit_prefer_asset_resolves_and_preserves_audit_conflict(self) -> None:
        result = reconcile_three_way(
            {"A": 1, "B": 1},
            {"A": 2, "B": 2},
            {"A": 3, "B": 1},
            policy=ReconciliationPolicy.PREFER_ASSET,
        )
        self.assertTrue(result.resolved)
        self.assertEqual(result.value, {"A": 2, "B": 2})
        self.assertEqual(tuple(item.path for item in result.conflicts), ("/A",))
        self.assertIsNone(result.diagnostic_code)

    def test_explicit_prefer_sidecar_resolves_and_preserves_audit_conflict(self) -> None:
        result = reconcile_three_way(
            {"A": 1, "B": 1},
            {"A": 2, "B": 2},
            {"A": 3, "B": 1},
            policy=ReconciliationPolicy.PREFER_SIDECAR,
        )
        self.assertTrue(result.resolved)
        self.assertEqual(result.value, {"A": 3, "B": 2})
        self.assertEqual(tuple(item.path for item in result.conflicts), ("/A",))
        self.assertIsNone(result.diagnostic_code)

    def test_conflict_paths_are_canonical_and_sorted(self) -> None:
        base = {"Z": 0, "A/B": 0, "Tilde~": 0}
        asset = {"Z": 1, "A/B": 1, "Tilde~": 1}
        sidecar = {"Z": 2, "A/B": 2, "Tilde~": 2}
        result = reconcile_three_way(base, asset, sidecar)
        self.assertEqual(
            tuple(item.path for item in result.conflicts),
            ("/A~1B", "/Tilde~0", "/Z"),
        )

    def test_policy_requires_exact_enum(self) -> None:
        for policy in ("FailOnConflict", DiffKind.ADD):
            with self.subTest(policy=policy):
                with self.assertRaises(AssetDocumentContractError) as caught:
                    reconcile_three_way({}, {}, {}, policy=policy)
                self.assertEqual(
                    (caught.exception.diagnostic.code, caught.exception.diagnostic.path),
                    ("Reconciliation.InvalidPolicy", "/Reconciliation/Policy"),
                )

    def test_input_alias_mutation_does_not_change_diff_or_reconciliation_audit(self) -> None:
        base = {"Node": {"Value": 1}, "Stable": {"List": [1]}}
        asset = {"Node": {"Value": 2}, "Stable": {"List": [1, 2]}}
        sidecar = {"Node": {"Value": 3}, "Stable": {"List": [1]}}
        strict = reconcile_three_way(base, asset, sidecar)
        preferred = reconcile_three_way(
            base, asset, sidecar, policy=ReconciliationPolicy.PREFER_SIDECAR
        )
        strict_audit = repr((strict.conflicts, strict.asset_changes, strict.sidecar_changes))
        preferred_audit = repr(
            (preferred.value, preferred.conflicts, preferred.asset_changes, preferred.sidecar_changes)
        )
        base["Node"]["Value"] = 10
        asset["Node"]["Value"] = 20
        asset["Stable"]["List"].append(99)
        sidecar["Node"]["Value"] = 30
        self.assertEqual(
            repr((strict.conflicts, strict.asset_changes, strict.sidecar_changes)),
            strict_audit,
        )
        self.assertEqual(
            repr((preferred.value, preferred.conflicts, preferred.asset_changes, preferred.sidecar_changes)),
            preferred_audit,
        )

    def test_generated_state_invariants_and_disjoint_mapping_commutativity(self) -> None:
        values = generated_json_values(seed=0xAD3, count=256)
        for index, value in enumerate(values):
            base = values[(index + 1) % len(values)]
            with self.subTest(index=index, rule="one-side-and-converged"):
                self.assertTrue(canonical_equal(reconcile_three_way(base, base, value).value, value))
                self.assertTrue(canonical_equal(reconcile_three_way(base, value, base).value, value))
                self.assertTrue(canonical_equal(reconcile_three_way(base, value, value).value, value))
                self.assertEqual(
                    reconcile_three_way(base, value, value),
                    reconcile_three_way(base, value, value),
                )

        for index in range(64):
            base = {"A": values[index], "B": values[index + 64], "Stable": values[index + 128]}
            asset = {**base, "A": {"Asset": values[index + 192]}}
            sidecar = {**base, "B": {"Sidecar": values[index]}}
            asset_paths = {item.path for item in canonical_diff(base, asset)}
            sidecar_paths = {item.path for item in canonical_diff(base, sidecar)}
            self.assertTrue(asset_paths.isdisjoint(sidecar_paths))
            forward = reconcile_three_way(base, asset, sidecar)
            reverse = reconcile_three_way(base, sidecar, asset)
            self.assertEqual(forward.resolved, reverse.resolved)
            self.assertTrue(canonical_equal(forward.value, reverse.value))
            self.assertEqual(
                tuple(item.path for item in forward.conflicts),
                tuple(item.path for item in reverse.conflicts),
            )

        forward_conflict = reconcile_three_way({"A": 1}, {"A": 2}, {"A": 3})
        reverse_conflict = reconcile_three_way({"A": 1}, {"A": 3}, {"A": 2})
        self.assertEqual(forward_conflict.resolved, reverse_conflict.resolved)
        self.assertEqual(
            tuple(item.path for item in forward_conflict.conflicts),
            tuple(item.path for item in reverse_conflict.conflicts),
        )


if __name__ == "__main__":
    unittest.main()
