# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.dto import PrimitiveRequirement
from asset_document.profiles.behavior_tree import BehaviorTreeProfileCandidate
from asset_document.tests.fixtures.behavior_tree_profile_cases import (
    DECORATOR_INPUT_BLOCKER,
    DECORATOR_REFRESH_BLOCKER,
    EXACT_CLASS,
    GRAPH_BLOCKER,
    PrimitiveContext,
    RejectingPrimitiveContext,
    ROUTE_ID,
    RuntimeMirrorContext,
    TARGET,
    VALUE_OR_KEY_BLOCKER,
    body,
    document,
    tree_spec,
)


def assert_error(test: unittest.TestCase, code: str, path: str, callback) -> None:
    with test.assertRaises(AssetDocumentContractError) as caught:
        callback()
    diagnostic = caught.exception.diagnostic
    test.assertEqual((diagnostic.code, diagnostic.path), (code, path))


class BehaviorTreeProfileTests(unittest.TestCase):
    def test_profile_has_the_exact_unregistered_class_body_and_requirements(self) -> None:
        profile = BehaviorTreeProfileCandidate()

        self.assertEqual(
            profile.inspect_profile(),
            {"Class": EXACT_CLASS, "BodySections": ["BlackboardAsset", "Tree"]},
        )
        self.assertEqual(
            profile.required_primitives(),
            (
                PrimitiveRequirement("ai.behavior_tree.graph.create.v1", 1, ROUTE_ID),
                PrimitiveRequirement("ai.behavior_tree.graph.create_defaults.v1", 1, ROUTE_ID),
                PrimitiveRequirement("ai.behavior_tree.node.create_action.v1", 1, ROUTE_ID),
                PrimitiveRequirement("ai.behavior_tree.subnode.create_action.v1", 1, ROUTE_ID),
                PrimitiveRequirement("ai.behavior_tree.comment.create_action.v1", 1, ROUTE_ID),
                PrimitiveRequirement("ai.behavior_tree.graph.connect.v1", 1, ROUTE_ID),
                PrimitiveRequirement("ai.behavior_tree.blackboard.set.v1", 1, ROUTE_ID),
                PrimitiveRequirement("ai.behavior_tree.runtime_mirror.rebuild.v1", 1, ROUTE_ID),
            ),
        )
        self.assertEqual(
            profile.create_template(TARGET)["Body"],
            {"BlackboardAsset": None, "Tree": {"Nodes": [], "Comments": []}},
        )

    def test_graph_install_is_a_terminal_blocker_before_any_primitive(self) -> None:
        profile = BehaviorTreeProfileCandidate()
        context = PrimitiveContext()
        blocked = tree_spec()
        blocked["InstallGraph"] = True

        assert_error(
            self,
            GRAPH_BLOCKER,
            "/Body/Tree/InstallGraph",
            lambda: profile.apply(document(body(blocked)), context=context, asset_handle=object()),
        )
        self.assertEqual(context.calls, [])

    def test_each_other_frozen_refresh_or_decorator_gap_fails_closed(self) -> None:
        profile = BehaviorTreeProfileCandidate()
        cases = (
            ("EnsureDecoratorInputCount", DECORATOR_INPUT_BLOCKER),
            ("RefreshValueOrKey", VALUE_OR_KEY_BLOCKER),
            ("RefreshDecorators", DECORATOR_REFRESH_BLOCKER),
        )
        for field, code in cases:
            with self.subTest(field=field):
                context = PrimitiveContext()
                blocked = tree_spec()
                blocked[field] = True
                assert_error(
                    self,
                    code,
                    f"/Body/Tree/{field}",
                    lambda blocked=blocked, context=context: profile.apply(
                        document(body(blocked)), context=context, asset_handle=object()
                    ),
                )
                self.assertEqual(context.calls, [])

    def test_apply_orders_the_admitted_graph_actions_and_opaque_handles(self) -> None:
        context = PrimitiveContext()

        BehaviorTreeProfileCandidate().apply(
            document(), context=context, asset_handle=object()
        )

        self.assertEqual(
            [call[0] for call in context.calls],
            [
                "ai.behavior_tree.blackboard.set.v1",
                "ai.behavior_tree.graph.create.v1",
                "ai.behavior_tree.graph.create_defaults.v1",
                "ai.behavior_tree.node.create_action.v1",
                "ai.behavior_tree.subnode.create_action.v1",
                "ai.behavior_tree.node.create_action.v1",
                "ai.behavior_tree.subnode.create_action.v1",
                "ai.behavior_tree.comment.create_action.v1",
                "ai.behavior_tree.graph.connect.v1",
            ],
        )
        self.assertEqual(
            context.calls[4][1],
            {"node_class_path": "/Script/AIModule.BTDecorator_Cooldown", "subnode_kind": "Decorator"},
        )
        self.assertEqual(context.calls[8][1], {"from_pin_name": "Out", "to_pin_name": "In"})

    def test_apply_never_sends_a_scratch_graph_to_runtime_mirror(self) -> None:
        context = PrimitiveContext()

        BehaviorTreeProfileCandidate().apply(
            document(), context=context, asset_handle=object()
        )

        self.assertNotIn(
            "ai.behavior_tree.runtime_mirror.rebuild.v1",
            [call[0] for call in context.calls],
        )

    def test_runtime_mirror_rejects_a_scratch_graph_before_any_helper(self) -> None:
        context = RuntimeMirrorContext()

        assert_error(
            self,
            "AIProvider.InvalidHandle",
            "/HandleArguments/graph",
            lambda: BehaviorTreeProfileCandidate().rebuild_runtime_mirror(
                document(),
                context=context,
                asset_handle=context.live_asset,
                live_graph_handle=context.scratch_graph,
            ),
        )
        self.assertEqual(
            [call[0] for call in context.calls],
            ["ai.behavior_tree.runtime_mirror.rebuild.v1"],
        )

    def test_runtime_mirror_failure_after_valid_live_identity_does_not_fallback_to_blackboard(self) -> None:
        context = RuntimeMirrorContext(fail_runtime_mirror=True)

        assert_error(
            self,
            "AIProvider.BehaviorTree.RuntimeMirrorRebuildFailed",
            "/RuntimeMirror",
            lambda: BehaviorTreeProfileCandidate().rebuild_runtime_mirror(
                document(),
                context=context,
                asset_handle=context.live_asset,
                live_graph_handle=context.live_graph,
            ),
        )
        self.assertEqual(context.phase.value, "Repair")
        self.assertEqual(
            [call[0] for call in context.calls],
            ["ai.behavior_tree.runtime_mirror.rebuild.v1"],
        )

    def test_rejected_handle_or_transaction_outcome_is_preserved_without_retry(self) -> None:
        context = RejectingPrimitiveContext()

        assert_error(
            self,
            "AIProvider.InvalidHandle",
            "/Handle",
            lambda: BehaviorTreeProfileCandidate().apply(
                document(), context=context, asset_handle=object()
            ),
        )
        self.assertEqual([call[0] for call in context.calls], ["ai.behavior_tree.blackboard.set.v1"])

    def test_canonicalization_and_diff_are_deterministic_and_preserve_empty_tree(self) -> None:
        profile = BehaviorTreeProfileCandidate()
        authored = document(body({"Nodes": [], "Comments": []}, None))
        equivalent = profile.canonicalize(authored)
        authored["Body"]["Tree"]["Nodes"].append({"Id": "later"})

        self.assertEqual(equivalent["Body"]["Tree"], {"Nodes": [], "Comments": []})
        self.assertEqual(profile.diff(equivalent, document(body({"Nodes": [], "Comments": []}, None))), ())

    def test_validate_rejects_unknown_body_and_noncanonical_tree_shape(self) -> None:
        profile = BehaviorTreeProfileCandidate()

        assert_error(
            self,
            "BehaviorTree.UnknownBodyKey",
            "/Body/RuntimeCache",
            lambda: profile.validate(document({"RuntimeCache": {}})),
        )
        assert_error(
            self,
            "BehaviorTree.InvalidTreeNode",
            "/Body/Tree/Nodes/0",
            lambda: profile.validate(document(body({"Nodes": [{"Id": "only"}], "Comments": []}))),
        )


if __name__ == "__main__":
    unittest.main()
