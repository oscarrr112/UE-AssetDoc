# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Authored rules: read as warnings, settled against the current asset on authored requests."""
from __future__ import annotations

import unittest

from asset_document import tolerance
from asset_document.diagnostics import AssetDocumentContractError, collecting_read_warnings
from asset_document.read_only_extract import extract_document


def _rule(value, path="/Body/Name"):
    if " " in value:
        tolerance.authored_rule(path, "Test.Spaced", f"{value!r} has a space.")
    return value


class AuthoredRuleModeTests(unittest.TestCase):
    def test_direct_call_raises(self):
        with self.assertRaises(AssetDocumentContractError) as raised:
            _rule("a b")
        self.assertEqual(raised.exception.diagnostic.code, "Test.Spaced")

    def test_live_read_keeps_the_value_and_warns(self):
        with collecting_read_warnings() as warnings:
            value = extract_document("/Game/X", lambda: {"Name": "a b"}, lambda doc: {"Name": _rule(doc["Name"])},
                                     read_only=True)
        self.assertEqual(value, {"Name": "a b"})
        self.assertEqual([(item.path, item.code, item.severity.value) for item in warnings],
                         [("/Body/Name", "Test.Spaced", "Warning")])

    def test_authoring_records_and_goes_on(self):
        with tolerance.authoring() as findings:
            _rule("a b", "/Body/A")
            _rule("c d", "/Body/B")
        self.assertEqual([item.path for item in findings], ["/Body/A", "/Body/B"])

    def test_settled_ignores(self):
        with tolerance.settled():
            self.assertEqual(_rule("a b"), "a b")

    def test_authoring_reports_findings_for_a_failure_they_explain(self):
        with self.assertRaises(AssetDocumentContractError) as raised:
            with tolerance.authoring():
                _rule("a b")
                raise TypeError("later check tripped")
        self.assertEqual(raised.exception.diagnostic.code, "Test.Spaced")


class SettleTests(unittest.TestCase):
    def _settle(self, authored, live, *pointers):
        findings = [tolerance.Finding(pointer, "Test.Rule", "bad", (pointer,)) for pointer in pointers]
        tolerance.settle(findings, authored, live)

    def test_unchanged_object_member_is_waived(self):
        doc = {"Body": {"Entries": {"Bad Key": "x"}}}
        self._settle(doc, {"Body": {"Entries": {"Bad Key": "x", "Other": "y"}}}, "/Body/Entries/Bad Key")

    def test_new_or_changed_value_fails(self):
        doc = {"Body": {"Entries": {"Bad Key": "x"}}}
        with self.assertRaises(AssetDocumentContractError):
            self._settle(doc, {"Body": {"Entries": {}}}, "/Body/Entries/Bad Key")
        with self.assertRaises(AssetDocumentContractError):
            self._settle(doc, {"Body": {"Entries": {"Bad Key": "z"}}}, "/Body/Entries/Bad Key")
        with self.assertRaises(AssetDocumentContractError):
            self._settle(doc, None, "/Body/Entries/Bad Key")

    def test_keyed_list_matches_by_name_whatever_the_order(self):
        doc = {"L": [{"Name": "b", "V": " x"}, {"Name": "a", "V": 1}]}
        live = {"L": [{"Name": "a", "V": 1}, {"Name": "b", "V": " x", "Extra": 2}]}
        self._settle(doc, live, "/L/0/V")
        with self.assertRaises(AssetDocumentContractError):
            self._settle({"L": [{"Name": "c", "V": " x"}]}, live, "/L/0/V")

    def test_duplicate_keys_pair_in_order_preferring_equal(self):
        live = {"L": [{"Name": "FX", "T": 1}, {"Name": "FX", "T": 2}]}
        self._settle({"L": [{"Name": "FX", "T": 2}]}, live, "/L/0/Name")
        self.assertEqual(tolerance.pair_lists([{"Name": "FX", "T": 2}, {"Name": "FX", "T": 9}], live["L"]), [1, 0])

    def test_unkeyed_list_equal_element_first_then_index(self):
        live = {"S": [{"A": None, "X": 0}, {"A": "/Game/W.W", "X": 1}]}
        # Inserted before: the unchanged element still finds its counterpart.
        self._settle({"S": [{"A": "/Game/N.N", "X": 5}, {"A": None, "X": 0}]}, live, "/S/1/A")
        # Changed elsewhere in the element, same judged value at the same index: waived per value.
        self._settle({"S": [{"A": None, "X": 3}]}, live, "/S/0/A")

    def test_relational_rule_judges_every_named_value(self):
        live = {"N": [{"T": 1}, {"T": 1}]}
        finding = tolerance.Finding("/N/1", "Test.Overlap", "overlap", ("/N/0/T", "/N/1/T"))
        tolerance.settle([finding], {"N": [{"T": 1}, {"T": 1}]}, live)
        with self.assertRaises(AssetDocumentContractError):
            tolerance.settle([finding], {"N": [{"T": 1}, {"T": 1}, {"T": 1}]}, {"N": [{"T": 1}]})

    def test_all_remaining_findings_fail_together(self):
        with self.assertRaises(AssetDocumentContractError) as raised:
            self._settle({"A": "x", "B": "y"}, {}, "/A", "/B")
        self.assertEqual([item.path for item in raised.exception.diagnostics], ["/A", "/B"])

    def test_a_judged_key_needs_the_member_not_its_value(self):
        finding = tolerance.Finding("/E/a b", "Test.Key", "bad", (tolerance.key("/E/a b"),))
        tolerance.settle([finding], {"E": {"a b": "new"}}, {"E": {"a b": "old"}})
        with self.assertRaises(AssetDocumentContractError):
            tolerance.settle([finding], {"E": {"a b": "new"}}, {"E": {}})

    def test_changed_indices(self):
        live = [{"Name": "a", "V": 1}, {"Name": "b", "V": 2}]
        self.assertEqual(tolerance.changed_indices([{"Name": "b", "V": 2}, {"Name": "a", "V": 3},
                                                    {"Name": "c", "V": 0}], live), [1, 2])


class RegisteredSettlementPhaseTests(unittest.TestCase):
    """A registered Profile's findings are settled in Preflight; the phases that change the asset do not read it
    again (a Profile's read may be admitted only in Preflight and Verify)."""

    PREFLIGHT, APPLY, REPAIR = 1, 2, 3
    APPLY_OPERATION = 5

    def dispatch(self, phase, value):
        from asset_document.registered_runtime import build_dispatch
        from asset_document.tests.runtime_registration_fixtures import Context, Handler, registry
        from asset_document.tests.test_generic_runtime import document as make_document, request

        class RuledHandler(Handler):
            def canonicalize(self, document):
                tolerance.authored_rule("/Properties/Value", "Test.Rule", "Value breaks a rule for new input.")
                return super().canonicalize(document)

            def extract(self, *, target, asset_handle, context, include_defaults=False):
                if context.phase.value not in ("Preflight", "Verify"):
                    raise AssertionError(f"the asset is read in {context.phase.value}")
                return super().extract(target=target, asset_handle=asset_handle, context=context,
                                       include_defaults=include_defaults)

        handler = RuledHandler()
        frozen, _ = registry(handler)
        authored = make_document()
        authored["Properties"]["Value"] = value
        result = build_dispatch(frozen)(request(authored, operation=self.APPLY_OPERATION), Context(phase))
        return result, handler.calls

    def test_preflight_waives_the_value_the_asset_holds(self):
        result, calls = self.dispatch(self.PREFLIGHT, 7)
        self.assertEqual(result["terminal_status"], 0, result)
        self.assertIn("extract", calls)

    def test_preflight_refuses_a_new_value(self):
        result, _ = self.dispatch(self.PREFLIGHT, 8)
        self.assertNotEqual(result["terminal_status"], 0)
        self.assertEqual([item["code"] for item in result["diagnostics"]], ["Test.Rule"])

    def test_apply_and_repair_do_not_read_the_asset_again(self):
        for phase in (self.APPLY, self.REPAIR):
            with self.subTest(phase=phase):
                result, calls = self.dispatch(phase, 7)
                self.assertEqual(result["terminal_status"], 0, result)
                self.assertNotIn("extract", calls)


class LegacySettlementPhaseTests(unittest.TestCase):
    """The non-registered route settles authored-rule findings in Preflight and Verify only, as the registered one
    does: Apply and Repair never read the asset."""

    def run_phase(self, phase):
        from types import SimpleNamespace
        from unittest import mock

        from asset_document import runtime
        from asset_document.dto import RuntimePhase

        class Profile:
            def canonicalize(self, document):
                tolerance.authored_rule("/Properties/Value", "Test.Rule", "Value breaks a rule for new input.")
                return document

        reads = []
        request = SimpleNamespace(canonical_document={"Properties": {"Value": 7}})
        with mock.patch.object(runtime, "_current_document",
                               side_effect=lambda *args: reads.append(phase) or {"Properties": {"Value": 7}}), \
                mock.patch.object(runtime, "_mutating_phase_result", return_value="done"):
            result = runtime._mutating_operation_result(request, 1, getattr(RuntimePhase, phase), Profile(), None)
        return result, reads

    def test_preflight_and_verify_read_the_asset(self):
        for phase in ("PREFLIGHT", "VERIFY"):
            with self.subTest(phase=phase):
                self.assertEqual(self.run_phase(phase), ("done", [phase]))

    def test_apply_and_repair_do_not_read_the_asset(self):
        for phase in ("APPLY", "REPAIR"):
            with self.subTest(phase=phase):
                self.assertEqual(self.run_phase(phase), ("done", []))


if __name__ == "__main__":
    unittest.main()
