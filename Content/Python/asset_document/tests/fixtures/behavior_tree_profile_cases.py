# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from asset_document.dto import RuntimePhase
from asset_document.regions.primitive_catalog import (
    NamedOpaqueHandleArgument,
    NamedOpaqueHandleResult,
    PrimitiveOutcome,
    PrimitiveOutcomeStatus,
)
from asset_document.diagnostics import RuntimeDiagnostic


EXACT_CLASS = "/Script/AIModule.BehaviorTree"
TARGET = "/Game/AI/BT_ProfileCandidate"
ROUTE_ID = "ai_profile"
GRAPH_BLOCKER = "AD.BLOCK.AI.GRAPH_INSTALL_AUTHORITY"
DECORATOR_INPUT_BLOCKER = "AD.BLOCK.AI.DECORATOR_INPUT_AUTHORITY"
VALUE_OR_KEY_BLOCKER = "AD.BLOCK.AI.VALUE_OR_KEY_REFRESH_AUTHORITY"
DECORATOR_REFRESH_BLOCKER = "AD.BLOCK.AI.DECORATOR_REFRESH_AUTHORITY"


def body(tree: dict | None = None, blackboard: str | None = "/Game/AI/BB_Profile") -> dict:
    return {
        "BlackboardAsset": blackboard,
        "Tree": tree if tree is not None else tree_spec(),
    }


def document(
    body_value: dict | None = None,
    action: str = "CreateOrUpdate",
) -> dict:
    return {
        "SchemaVersion": 1,
        "Target": TARGET,
        "Class": EXACT_CLASS,
        "Action": action,
        "Definitions": {},
        "Properties": {},
        "Body": body() if body_value is None else body_value,
    }


def tree_spec() -> dict:
    return {
        "Nodes": [
            {
                "Id": "selector",
                "Kind": "Main",
                "Class": "/Script/AIModule.BTComposite_Selector",
                "Position": {"x": 0, "y": 0},
                "Children": ["wait"],
                "Decorators": [
                    {
                        "Id": "cooldown",
                        "Kind": "Decorator",
                        "Class": "/Script/AIModule.BTDecorator_Cooldown",
                    }
                ],
                "Services": [],
            },
            {
                "Id": "wait",
                "Kind": "Main",
                "Class": "/Script/AIModule.BTTask_Wait",
                "Position": {"x": 240, "y": 0},
                "Children": [],
                "Decorators": [],
                "Services": [
                    {
                        "Id": "tick",
                        "Kind": "Service",
                        "Class": "/Script/AIModule.BTService_DefaultFocus",
                    }
                ],
            },
        ],
        "Comments": [
            {
                "Id": "note",
                "Bounds": {"x": -20, "y": -20, "w": 160, "h": 80},
                "Text": "candidate only",
            }
        ],
    }


class PrimitiveContext:
    def __init__(self, phase: RuntimePhase = RuntimePhase.APPLY) -> None:
        self._phase = phase
        self.calls: list[tuple[str, dict, tuple[NamedOpaqueHandleArgument, ...]]] = []
        self._index = 0
        self.scratch_graph = object()

    @property
    def phase(self) -> RuntimePhase:
        return self._phase

    def invoke(self, *, requirement, value_arguments, handle_arguments) -> PrimitiveOutcome:
        self.calls.append((requirement.primitive_id, value_arguments, handle_arguments))
        self._index += 1
        if requirement.primitive_id == "ai.behavior_tree.runtime_mirror.rebuild.v1":
            graph = next(argument.handle for argument in handle_arguments if argument.name == "graph")
            if graph is self.scratch_graph:
                return PrimitiveOutcome(
                    PrimitiveOutcomeStatus.FAILED,
                    None,
                    (),
                    (),
                    (RuntimeDiagnostic("/HandleArguments/graph", "AIProvider.InvalidHandle", "scratch graph is not installed"),),
                )
        handle_name = {
            "ai.behavior_tree.graph.create.v1": "graph",
            "ai.behavior_tree.graph.create_defaults.v1": "root_node",
            "ai.behavior_tree.node.create_action.v1": "node",
            "ai.behavior_tree.subnode.create_action.v1": "node",
            "ai.behavior_tree.comment.create_action.v1": "node",
        }.get(requirement.primitive_id)
        handles = (
            (NamedOpaqueHandleResult(handle_name, (self.scratch_graph if handle_name == "graph" else object(),)),)
            if handle_name is not None
            else ()
        )
        return PrimitiveOutcome(
            PrimitiveOutcomeStatus.SUCCEEDED,
            {"stable_node_id": f"native-{self._index}"},
            handles,
            (),
            (),
        )


class RejectingPrimitiveContext(PrimitiveContext):
    def invoke(self, *, requirement, value_arguments, handle_arguments) -> PrimitiveOutcome:
        self.calls.append((requirement.primitive_id, value_arguments, handle_arguments))
        return PrimitiveOutcome(
            PrimitiveOutcomeStatus.FAILED,
            None,
            (),
            (),
            (RuntimeDiagnostic("/Handle", "AIProvider.InvalidHandle", "stale handle"),),
        )


class RuntimeMirrorContext(PrimitiveContext):
    def __init__(self, *, fail_runtime_mirror: bool = False) -> None:
        super().__init__(RuntimePhase.REPAIR)
        self.live_asset = object()
        self.live_graph = object()
        self.fail_runtime_mirror = fail_runtime_mirror

    def invoke(self, *, requirement, value_arguments, handle_arguments) -> PrimitiveOutcome:
        self.calls.append((requirement.primitive_id, value_arguments, handle_arguments))
        if requirement.primitive_id != "ai.behavior_tree.runtime_mirror.rebuild.v1":
            raise AssertionError("Repair context only admits runtime-mirror")
        arguments = {argument.name: argument.handle for argument in handle_arguments}
        if arguments != {"asset": self.live_asset, "graph": self.live_graph}:
            return PrimitiveOutcome(
                PrimitiveOutcomeStatus.FAILED,
                None,
                (),
                (),
                (RuntimeDiagnostic("/HandleArguments/graph", "AIProvider.InvalidHandle", "live HA/HG identity mismatch"),),
            )
        if self.fail_runtime_mirror:
            return PrimitiveOutcome(
                PrimitiveOutcomeStatus.FAILED,
                None,
                (),
                (),
                (RuntimeDiagnostic("/RuntimeMirror", "AIProvider.BehaviorTree.RuntimeMirrorRebuildFailed", "injected runtime-mirror failure"),),
            )
        return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED, {"node_count": 2}, (), (), ())
