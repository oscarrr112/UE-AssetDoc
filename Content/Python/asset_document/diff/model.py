# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum

from asset_document.dto import JsonValue, _representation_error, _snapshot_json_value


class DiffKind(str, Enum):
    ADD = "Add"
    REMOVE = "Remove"
    REPLACE = "Replace"


def _snapshot_present(value: JsonValue | None, present: bool, path: str) -> JsonValue | None:
    if type(present) is not bool:
        raise _representation_error(path, "Presence flags must be exact bool values.")
    if not present:
        if value is not None:
            raise _representation_error(path, "Absent audit values must contain JSON null.")
        return None
    return _snapshot_json_value(value, path)


@dataclass(frozen=True, slots=True)
class Difference:
    path: str
    kind: DiffKind
    before_present: bool
    before: JsonValue | None
    after_present: bool
    after: JsonValue | None
    # The row a table difference is in (DataTable / CurveTable rows), named in the public entry ("row") and in
    # Validate / Diff summaries; None elsewhere.
    row: str | None = None

    def __post_init__(self) -> None:
        if type(self.path) is not str:
            raise _representation_error("/Diff/Path", "Diff path must be an exact string.")
        if self.row is not None and type(self.row) is not str:
            raise _representation_error("/Diff/Row", "Diff row must be an exact string.")
        if type(self.kind) is not DiffKind:
            raise _representation_error("/Diff/Kind", "Diff kind must be exact DiffKind.")
        object.__setattr__(
            self,
            "before",
            _snapshot_present(self.before, self.before_present, f"{self.path}/Before"),
        )
        object.__setattr__(
            self,
            "after",
            _snapshot_present(self.after, self.after_present, f"{self.path}/After"),
        )


class ReconciliationPolicy(str, Enum):
    FAIL_ON_CONFLICT = "FailOnConflict"
    PREFER_ASSET = "PreferAsset"
    PREFER_SIDECAR = "PreferSidecar"


@dataclass(frozen=True, slots=True)
class ReconciliationConflict:
    path: str
    base_present: bool
    base: JsonValue | None
    asset_present: bool
    asset: JsonValue | None
    sidecar_present: bool
    sidecar: JsonValue | None

    def __post_init__(self) -> None:
        if type(self.path) is not str:
            raise _representation_error(
                "/Reconciliation/Conflict/Path", "Conflict path must be an exact string."
            )
        object.__setattr__(
            self,
            "base",
            _snapshot_present(self.base, self.base_present, f"{self.path}/Base"),
        )
        object.__setattr__(
            self,
            "asset",
            _snapshot_present(self.asset, self.asset_present, f"{self.path}/Asset"),
        )
        object.__setattr__(
            self,
            "sidecar",
            _snapshot_present(self.sidecar, self.sidecar_present, f"{self.path}/Sidecar"),
        )


@dataclass(frozen=True, slots=True)
class ReconciliationResult:
    resolved: bool
    value: JsonValue | None
    conflicts: tuple[ReconciliationConflict, ...]
    asset_changes: tuple[Difference, ...]
    sidecar_changes: tuple[Difference, ...]
    diagnostic_code: str | None

    def __post_init__(self) -> None:
        if type(self.resolved) is not bool:
            raise _representation_error(
                "/Reconciliation/Resolved", "Resolved must be an exact bool."
            )
        if self.resolved:
            object.__setattr__(
                self, "value", _snapshot_json_value(self.value, "/Reconciliation/Value")
            )
        elif self.value is not None:
            raise _representation_error(
                "/Reconciliation/Value", "An unresolved result cannot expose a partial value."
            )
        _require_tuple(
            self.conflicts,
            ReconciliationConflict,
            "/Reconciliation/Conflicts",
            "conflicts",
        )
        _require_tuple(self.asset_changes, Difference, "/Reconciliation/AssetChanges", "asset changes")
        _require_tuple(
            self.sidecar_changes,
            Difference,
            "/Reconciliation/SidecarChanges",
            "sidecar changes",
        )
        if self.diagnostic_code is not None and type(self.diagnostic_code) is not str:
            raise _representation_error(
                "/Reconciliation/DiagnosticCode",
                "Diagnostic code must be an exact string or JSON null.",
            )


def _require_tuple(value: object, expected: type, path: str, label: str) -> None:
    if type(value) is not tuple or any(type(item) is not expected for item in value):
        raise _representation_error(
            path, f"{label} must be an exact tuple of {expected.__name__}."
        )
