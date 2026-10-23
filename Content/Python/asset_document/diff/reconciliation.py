# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from dataclasses import dataclass

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.diagnostics import contract_error
from asset_document.dto import JsonValue
from asset_document.diff.canonical_diff import (
    _canonical_equal_validated,
    _child_path,
    canonical_diff,
)
from asset_document.diff.model import (
    ReconciliationConflict,
    ReconciliationPolicy,
    ReconciliationResult,
)


_MISSING = object()


@dataclass(frozen=True, slots=True)
class _NodeResult:
    present: bool
    value: JsonValue | None
    conflicts: tuple[ReconciliationConflict, ...]


def _is_equal(
    present_left: bool,
    left: JsonValue | None,
    present_right: bool,
    right: JsonValue | None,
) -> bool:
    if present_left != present_right:
        return False
    if not present_left:
        return True
    return _canonical_equal_validated(left, right)


def _make_conflict(
    path: str,
    base_present: bool,
    base: JsonValue | None,
    asset_present: bool,
    asset: JsonValue | None,
    sidecar_present: bool,
    sidecar: JsonValue | None,
) -> ReconciliationConflict:
    return ReconciliationConflict(
        path,
        base_present,
        base,
        asset_present,
        asset,
        sidecar_present,
        sidecar,
    )


def _resolve_conflict(
    path: str,
    base_present: bool,
    base: JsonValue | None,
    asset_present: bool,
    asset: JsonValue | None,
    sidecar_present: bool,
    sidecar: JsonValue | None,
    policy: ReconciliationPolicy,
) -> _NodeResult:
    conflict = _make_conflict(
        path,
        base_present,
        base,
        asset_present,
        asset,
        sidecar_present,
        sidecar,
    )
    if policy is ReconciliationPolicy.PREFER_ASSET:
        return _NodeResult(asset_present, asset, (conflict,))
    if policy is ReconciliationPolicy.PREFER_SIDECAR:
        return _NodeResult(sidecar_present, sidecar, (conflict,))
    return _NodeResult(False, None, (conflict,))


def _mapping_value(mapping: dict[str, JsonValue], key: str) -> JsonValue | None:
    value = mapping.get(key, _MISSING)
    return None if value is _MISSING else value


def _reconcile_mapping(
    base: dict[str, JsonValue],
    asset: dict[str, JsonValue],
    sidecar: dict[str, JsonValue],
    path: str,
    policy: ReconciliationPolicy,
) -> _NodeResult:
    merged: dict[str, JsonValue] = {}
    conflicts: list[ReconciliationConflict] = []
    for key in sorted(base.keys() | asset.keys() | sidecar.keys()):
        node = _reconcile_node(
            key in base,
            _mapping_value(base, key),
            key in asset,
            _mapping_value(asset, key),
            key in sidecar,
            _mapping_value(sidecar, key),
            _child_path(path, key),
            policy,
        )
        if node.present:
            merged[key] = node.value
        conflicts.extend(node.conflicts)
    return _NodeResult(True, merged, tuple(conflicts))


def _reconcile_node(
    base_present: bool,
    base: JsonValue | None,
    asset_present: bool,
    asset: JsonValue | None,
    sidecar_present: bool,
    sidecar: JsonValue | None,
    path: str,
    policy: ReconciliationPolicy,
) -> _NodeResult:
    if _is_equal(asset_present, asset, sidecar_present, sidecar):
        return _NodeResult(asset_present, asset, ())
    if _is_equal(base_present, base, asset_present, asset):
        return _NodeResult(sidecar_present, sidecar, ())
    if _is_equal(base_present, base, sidecar_present, sidecar):
        return _NodeResult(asset_present, asset, ())
    if (
        base_present
        and asset_present
        and sidecar_present
        and type(base) is dict
        and type(asset) is dict
        and type(sidecar) is dict
    ):
        return _reconcile_mapping(base, asset, sidecar, path, policy)
    return _resolve_conflict(
        path,
        base_present,
        base,
        asset_present,
        asset,
        sidecar_present,
        sidecar,
        policy,
    )


def reconcile_three_way(
    base: JsonValue,
    asset: JsonValue,
    sidecar: JsonValue,
    *,
    policy: ReconciliationPolicy = ReconciliationPolicy.FAIL_ON_CONFLICT,
) -> ReconciliationResult:
    if type(policy) is not ReconciliationPolicy:
        raise contract_error(
            "/Reconciliation/Policy",
            "Reconciliation.InvalidPolicy",
            "Reconciliation policy must be an exact ReconciliationPolicy.",
        )
    codec = DocumentCodec()
    codec.validate(base)
    codec.validate(asset)
    codec.validate(sidecar)
    asset_changes = canonical_diff(base, asset)
    sidecar_changes = canonical_diff(base, sidecar)
    node = _reconcile_node(True, base, True, asset, True, sidecar, "", policy)
    conflicts = tuple(sorted(node.conflicts, key=lambda item: item.path))
    if conflicts and policy is ReconciliationPolicy.FAIL_ON_CONFLICT:
        return ReconciliationResult(
            False,
            None,
            conflicts,
            asset_changes,
            sidecar_changes,
            "Reconciliation.Conflict",
        )
    return ReconciliationResult(
        True,
        node.value,
        conflicts,
        asset_changes,
        sidecar_changes,
        None,
    )
