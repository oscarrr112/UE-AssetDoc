# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Closed, value-only preparation projection; no document interpretation or IO.

An exact handler supplies its already validated typed intents. Native preparation
still validates each intent against the selected provider's admitted schema.
"""
from __future__ import annotations

import math
from uuid import UUID

_KIND = "MutationIntentProjectionV1"
# Snapshot-covered: the request's writes are covered by Core's whole-package
# snapshot (existing target) or the factory rollback (created target), so the
# handler invokes mutating primitives dynamically instead of declaring steps.
SNAPSHOT_COVERED_KIND = "SnapshotCoveredMutationV1"
_KINDS = (_KIND, SNAPSHOT_COVERED_KIND)


class _SnapshotCovered:
    """Preflight return marker: build a SnapshotCoveredMutationV1 projection."""
    __slots__ = ()

    def __repr__(self) -> str:
        return "SNAPSHOT_COVERED"


SNAPSHOT_COVERED = _SnapshotCovered()
_ROOT_KEYS = frozenset(("Kind", "RequestId", "RuntimeRevision", "ExactAssetClassPath",
                        "TargetAssetPath", "Operation", "SemanticHash", "Steps"))
_STEP_KEYS = frozenset(("Index", "PrimitiveId", "Version", "Phase", "InputSchemaId", "Intent"))
_RESERVED = frozenset(("Body", "Handle", "Handles", "HandleArguments", "OpaqueHandle",
                       "Address", "Pointer", "Rollback", "RollbackCode", "TransactionId",
                       "Slot", "Generation"))


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _text(value: object) -> bool:
    return type(value) is str and bool(value)


def _copy_value(value: object, depth: int = 0) -> object:
    _require(depth <= 64, "Intent exceeds value nesting limit")
    if value is None or type(value) in (str, bool):
        return value
    if type(value) is int:
        _require(-(1 << 63) <= value < (1 << 63), "Intent integer exceeds int64")
        return value
    if type(value) is float:
        _require(math.isfinite(value), "Intent number must be finite")
        return value
    if type(value) is list:
        return [_copy_value(child, depth + 1) for child in value]
    if type(value) is dict:
        _require(all(type(key) is str and key not in _RESERVED for key in value),
                 "Intent cannot contain Body, handles, addresses or rollback envelopes")
        return {key: _copy_value(child, depth + 1) for key, child in value.items()}
    raise ValueError("Intent must contain only JSON values; native objects and bytes are forbidden")


def validate_mutation_intent_projection(
    projection: object, *, request_id: str, runtime_revision: int,
    exact_asset_class_path: str, target_asset_path: str, semantic_hash: str,
) -> dict:
    """Validate closed protocol/identity and return an alias-free value copy."""
    _require(type(projection) is dict and set(projection) == _ROOT_KEYS,
             "Projection must have exactly the closed root fields")
    _require(_text(request_id), "RequestId must be a nonempty UUID string")
    try:
        native_id = UUID(request_id)
        projected_id = UUID(projection["RequestId"]) if type(projection["RequestId"]) is str else None
    except (ValueError, AttributeError) as error:
        raise ValueError("RequestId must be a UUID") from error
    _require(native_id.int != 0 and projected_id == native_id, "RequestId identity drift")
    _require(type(runtime_revision) is int and 0 < runtime_revision < (1 << 63),
             "RuntimeRevision must be a positive int64")
    _require(type(projection["RuntimeRevision"]) is int
             and projection["RuntimeRevision"] == runtime_revision, "RuntimeRevision identity drift")
    for key, expected in (("ExactAssetClassPath", exact_asset_class_path),
                          ("TargetAssetPath", target_asset_path), ("SemanticHash", semantic_hash)):
        _require(_text(expected) and type(projection[key]) is str and projection[key] == expected,
                 key + " identity drift")
    _require(type(projection["Kind"]) is str and projection["Kind"] in _KINDS
             and type(projection["Operation"]) is str and projection["Operation"] == "Apply",
             "Projection kind/operation must be MutationIntentProjectionV1/Apply")
    _require(type(projection["Steps"]) is list, "Steps must be an array")
    _require(projection["Kind"] == _KIND or not projection["Steps"],
             "A snapshot-covered projection declares no steps")
    steps = []
    for index, step in enumerate(projection["Steps"]):
        _require(type(step) is dict and set(step) == _STEP_KEYS, "Step fields must be closed")
        _require(type(step["Index"]) is int and step["Index"] == index, "Step indices must be continuous")
        _require(_text(step["PrimitiveId"]) and _text(step["InputSchemaId"]), "Step identity must be nonempty")
        _require(type(step["Version"]) is int and 0 < step["Version"] <= 0xFFFFFFFF, "Version must be uint32")
        _require(type(step["Phase"]) is str and step["Phase"] in ("Apply", "Repair"), "Invalid mutation phase")
        _require(type(step["Intent"]) is dict, "Intent must be a typed value object")
        steps.append({**step, "Intent": _copy_value(step["Intent"])})
    return {**projection, "Steps": steps}


def build_mutation_intent_projection(
    *, request_id: str, runtime_revision: int, exact_asset_class_path: str,
    target_asset_path: str, semantic_hash: str, steps: list, kind: str = _KIND,
) -> dict:
    """Package already interpreted intents without issuing handles or parsing Body."""
    projection = {"Kind": kind, "RequestId": request_id, "RuntimeRevision": runtime_revision,
                  "ExactAssetClassPath": exact_asset_class_path, "TargetAssetPath": target_asset_path,
                  "Operation": "Apply", "SemanticHash": semantic_hash, "Steps": steps}
    return validate_mutation_intent_projection(
        projection, request_id=request_id, runtime_revision=runtime_revision,
        exact_asset_class_path=exact_asset_class_path, target_asset_path=target_asset_path,
        semantic_hash=semantic_hash)
