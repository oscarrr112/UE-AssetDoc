from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
from typing import Literal, Protocol

from asset_document.diagnostics import (
    AssetDocumentContractError,
    DiagnosticContext,
    DiagnosticSeverity,
    RuntimeDiagnostic,
    contract_error,
)
from asset_document.dto import (
    ChangedRecord,
    PrimitiveRequirement,
    RuntimePhase,
    RuntimeValue,
    _snapshot_runtime_value,
)


GENERIC_ROUTE_ID: str = "route.generic.v1"
GENERIC_RUNTIME_ENTRY_MODULE: str = "asset_document/bootstrap.py"
GENERIC_REQUIRED_PRIMITIVES: tuple[PrimitiveRequirement, ...] = (
    PrimitiveRequirement("editor.class.resolve_canonical.v1", 1, GENERIC_ROUTE_ID),
    PrimitiveRequirement("editor.fragment.struct_codec.v1", 1, GENERIC_ROUTE_ID),
    PrimitiveRequirement("editor.property.enumerate_admitted.v1", 1, GENERIC_ROUTE_ID),
    PrimitiveRequirement("editor.property.enumerate_admitted_batch.v1", 1, GENERIC_ROUTE_ID),
    PrimitiveRequirement("editor.property.preflight_admitted.v1", 1, GENERIC_ROUTE_ID),
    PrimitiveRequirement("editor.property.read_admitted.v1", 1, GENERIC_ROUTE_ID),
    PrimitiveRequirement("editor.property.write_admitted_batch.v1", 1, GENERIC_ROUTE_ID),
    PrimitiveRequirement("editor.reference.resolve_asset.v1", 1, GENERIC_ROUTE_ID),
    PrimitiveRequirement("editor.asset.resolve_existing.v1", 1, GENERIC_ROUTE_ID),
    # Blackboard creation keeps the exact data-asset factory.
    PrimitiveRequirement("editor.asset.create_by_factory.v1", 1, GENERIC_ROUTE_ID),
    # Generic candidates: regions/target_lifecycle.py GENERIC_FACTORY_LIFECYCLE
    # (rollback_created runs from native rollback seeds, not from Python).
    PrimitiveRequirement("editor.scratch.clone_asset.v2", 2, GENERIC_ROUTE_ID),
    PrimitiveRequirement("editor.scratch.create_by_factory.v1", 1, GENERIC_ROUTE_ID),
    PrimitiveRequirement("editor.asset.create_by_factory.v2", 2, GENERIC_ROUTE_ID),
    PrimitiveRequirement("editor.property.inspect_evidence.v1", 1, GENERIC_ROUTE_ID),
    PrimitiveRequirement("ai.behavior_tree.selector_inspect_evidence.v1", 1, GENERIC_ROUTE_ID),
    # Enhanced Input warnings on the preflight scratch (regions/enhanced_input_checks.py).
    PrimitiveRequirement("editor.enhanced_input.check_domain.v1", 1, GENERIC_ROUTE_ID),
)


class OpaqueHandleProxy(Protocol):
    """Memberless structural seam; only the bound port attests identity."""


def _invalid_outcome(message: str):
    return contract_error(
        "/PrimitiveOutcome",
        "PrimitiveCatalog.InvalidOutcome",
        message,
    )


def _require_name(value: object, path: str) -> None:
    if type(value) is not str or not value:
        raise contract_error(
            path,
            "PrimitiveCatalog.InvalidOutcome",
            "opaque handle lane names must be non-empty exact strings",
        )


def _require_exact_tuple(
    value: object,
    expected: type,
    message: str,
) -> None:
    if type(value) is not tuple:
        raise _invalid_outcome(message)
    if any(type(item) is not expected for item in value):
        raise _invalid_outcome(message)


@dataclass(frozen=True, slots=True)
class NamedOpaqueHandleArgument:
    name: str
    handle: OpaqueHandleProxy

    def __post_init__(self) -> None:
        _require_name(self.name, "/HandleArguments/Name")


@dataclass(frozen=True, slots=True)
class NamedOpaqueHandleResult:
    name: str
    handles: tuple[OpaqueHandleProxy, ...]

    def __post_init__(self) -> None:
        _require_name(self.name, "/HandleResults/Name")
        if type(self.handles) is not tuple:
            raise _invalid_outcome("handle results must contain an exact tuple")


class PrimitiveOutcomeStatus(IntEnum):
    SUCCEEDED = 0
    FAILED = 1
    UNAVAILABLE = 2


@dataclass(frozen=True, slots=True)
class PrimitiveOutcome:
    status: PrimitiveOutcomeStatus
    value: RuntimeValue
    handle_results: tuple[NamedOpaqueHandleResult, ...]
    changed_set: tuple[ChangedRecord, ...]
    diagnostics: tuple[RuntimeDiagnostic, ...]

    def __post_init__(self) -> None:
        if type(self.status) is not PrimitiveOutcomeStatus:
            raise _invalid_outcome("status must be an exact PrimitiveOutcomeStatus")
        _require_exact_tuple(
            self.handle_results,
            NamedOpaqueHandleResult,
            "handle results must be exact named groups",
        )
        _require_exact_tuple(
            self.changed_set,
            ChangedRecord,
            "changed set must contain exact ChangedRecord values",
        )
        _require_exact_tuple(
            self.diagnostics,
            RuntimeDiagnostic,
            "diagnostics must contain exact RuntimeDiagnostic values",
        )
        try:
            value = _snapshot_runtime_value(self.value, "/PrimitiveOutcome/Value")
        except AssetDocumentContractError:
            raise _invalid_outcome("value must be an exact RuntimeValue")
        object.__setattr__(self, "value", value)
        self._validate_terminal_lanes()

    def _validate_terminal_lanes(self) -> None:
        if self.status is PrimitiveOutcomeStatus.SUCCEEDED:
            return
        if self.value is not None or self.handle_results or self.changed_set:
            raise _invalid_outcome("failed outcomes cannot expose value, handles, or changes")
        if not self.diagnostics:
            raise _invalid_outcome("failed outcomes must carry a typed diagnostic")


class GenericPrimitiveContext(Protocol):
    def handoff(
        self,
        *,
        source_handle: OpaqueHandleProxy,
        target_provider_id: str,
        access: Literal["read_only", "mutable"],
    ) -> OpaqueHandleProxy: ...

    @property
    def phase(self) -> RuntimePhase: ...

    def invoke(
        self,
        *,
        requirement: PrimitiveRequirement,
        value_arguments: RuntimeValue,
        handle_arguments: tuple[NamedOpaqueHandleArgument, ...],
    ) -> PrimitiveOutcome: ...


_REQUIREMENTS_BY_ID = {
    item.primitive_id: item for item in GENERIC_REQUIRED_PRIMITIVES
}


def _escape_pointer(value: str) -> str:
    return value.replace("~", "~0").replace("/", "~1")


def requirement(primitive_id: str) -> PrimitiveRequirement:
    if type(primitive_id) is str:
        found = _REQUIREMENTS_BY_ID.get(primitive_id)
        if found is not None:
            return found
        segment = _escape_pointer(primitive_id)
    else:
        segment = "<invalid>"
    raise contract_error(
        f"/RequiredPrimitives/{segment}",
        "PrimitiveCatalog.UnknownGenericRequirement",
        "primitive ID is not a frozen Generic requirement",
    )


_NATIVE_SEVERITIES = (
    DiagnosticSeverity.INFO,
    DiagnosticSeverity.WARNING,
    DiagnosticSeverity.ERROR,
)
_NATIVE_OUTCOME_STATUSES = (
    PrimitiveOutcomeStatus.SUCCEEDED,
    PrimitiveOutcomeStatus.FAILED,
    PrimitiveOutcomeStatus.UNAVAILABLE,
)


def _native_projection_error(path: str, message: str):
    return contract_error(
        path,
        "GenericRuntime.InvalidNativeProjection",
        message,
    )


def _require_native_dict(value: object, keys: set[str], path: str) -> dict:
    if type(value) is not dict or set(value) != keys:
        raise _native_projection_error(
            path,
            "native projection has the wrong exact key set",
        )
    return value


def _native_enum(value: object, values: tuple, path: str):
    if type(value) is not int or not 0 <= value < len(values):
        raise _native_projection_error(
            path,
            "native enum value is outside the frozen range",
        )
    return values[value]


def _runtime_from_native(value: object, path: str) -> RuntimeValue:
    if value is None or type(value) in (bool, int, float, str, bytes):
        return value
    if type(value) is tuple:
        return [
            _runtime_from_native(item, f"{path}/{index}")
            for index, item in enumerate(value)
        ]
    if type(value) is dict and all(type(key) is str for key in value):
        return {
            key: _runtime_from_native(item, f"{path}/{key}")
            for key, item in value.items()
        }
    raise _native_projection_error(
        path,
        "native value is not an exact RuntimeValue projection",
    )


def _runtime_to_native(value: RuntimeValue):
    if type(value) is list:
        return tuple(_runtime_to_native(item) for item in value)
    if type(value) is dict:
        return {
            key: _runtime_to_native(item)
            for key, item in value.items()
        }
    return value


def _decode_native_diagnostic_context(value: object) -> DiagnosticContext:
    row = _require_native_dict(
        value,
        {"key", "value"},
        "/PrimitiveOutcome/Diagnostics/Context",
    )
    return DiagnosticContext(
        row["key"],
        _runtime_from_native(
            row["value"],
            "/PrimitiveOutcome/Diagnostics/Context/Value",
        ),
    )


def _decode_native_diagnostic(value: object) -> RuntimeDiagnostic:
    row = _require_native_dict(
        value,
        {"path", "code", "message", "severity", "context"},
        "/PrimitiveOutcome/Diagnostics",
    )
    if type(row["context"]) is not tuple:
        raise _native_projection_error(
            "/PrimitiveOutcome/Diagnostics/Context",
            "diagnostic context must be an exact tuple",
        )
    return RuntimeDiagnostic(
        row["path"],
        row["code"],
        row["message"],
        _native_enum(
            row["severity"],
            _NATIVE_SEVERITIES,
            "/PrimitiveOutcome/Diagnostics/Severity",
        ),
        tuple(
            _decode_native_diagnostic_context(item)
            for item in row["context"]
        ),
    )


def _decode_native_changed(value: object) -> ChangedRecord:
    row = _require_native_dict(
        value,
        {"authority_id", "stable_object_id", "path", "change_kind"},
        "/PrimitiveOutcome/ChangedSet",
    )
    return ChangedRecord(
        row["authority_id"],
        row["stable_object_id"],
        row["path"],
        row["change_kind"],
    )


def _decode_native_handle_result(value: object) -> NamedOpaqueHandleResult:
    row = _require_native_dict(
        value,
        {"name", "handles"},
        "/PrimitiveOutcome/HandleResults",
    )
    if type(row["handles"]) is not tuple:
        raise _native_projection_error(
            "/PrimitiveOutcome/HandleResults",
            "native handle results must be exact tuples",
        )
    return NamedOpaqueHandleResult(row["name"], row["handles"])


def _decode_native_outcome(value: object) -> PrimitiveOutcome:
    row = _require_native_dict(
        value,
        {"status", "value", "handle_results", "changed_set", "diagnostics"},
        "/PrimitiveOutcome",
    )
    for key in ("handle_results", "changed_set", "diagnostics"):
        if type(row[key]) is not tuple:
            raise _native_projection_error(
                f"/PrimitiveOutcome/{key}",
                "native outcome lanes must be exact tuples",
            )
    status = _native_enum(
        row["status"],
        _NATIVE_OUTCOME_STATUSES,
        "/PrimitiveOutcome/Status",
    )
    runtime_value = _runtime_from_native(row["value"], "/PrimitiveOutcome/Value")
    if status is not PrimitiveOutcomeStatus.SUCCEEDED and runtime_value == {}:
        runtime_value = None
    return PrimitiveOutcome(
        status,
        runtime_value,
        tuple(
            _decode_native_handle_result(item)
            for item in row["handle_results"]
        ),
        tuple(_decode_native_changed(item) for item in row["changed_set"]),
        tuple(
            _decode_native_diagnostic(item)
            for item in row["diagnostics"]
        ),
    )


_HANDOFF_DIAGNOSTIC_CODES = frozenset({
    'AD.RUNTIME.HANDOFF_UNAVAILABLE',
    'AD.RUNTIME.HANDOFF_INVALID_CONTEXT',
    'AD.RUNTIME.HANDOFF_TARGET_UNAVAILABLE',
    'AD.RUNTIME.HANDOFF_KIND_UNSUPPORTED',
    'AD.RUNTIME.HANDOFF_ACCESS_ESCALATION',
    'AD.RUNTIME.HANDOFF_TRANSACTION_REQUIRED',
    'AD.RUNTIME.HANDOFF_STORAGE_REJECTED',
    'HandleMetadata.RequestMismatch',
    'HandleMetadata.TransactionMismatch',
    'HandleMetadata.RuntimeRevisionMismatch',
    'HandleMetadata.ProviderMismatch',
    'HandleMetadata.InvalidPhase',
    'HandleMetadata.InvalidKind',
    'HandleMetadata.InvalidAccess',
    'HandleMetadata.KindMismatch',
    'HandleMetadata.AccessMismatch',
    'HandleMetadata.InvalidSlot',
    'HandleMetadata.InvalidGeneration',
    'AD.RUNTIME.STALE_HANDLE',
})


class _NativePrimitiveContext:
    __slots__ = ("_native", "_phase")

    def __init__(self, native: object, phase: RuntimePhase) -> None:
        self._native = native
        self._phase = phase

    @property
    def phase(self) -> RuntimePhase:
        return self._phase

    def handoff(
        self,
        *,
        source_handle: OpaqueHandleProxy,
        target_provider_id: str,
        access: Literal["read_only", "mutable"],
    ) -> OpaqueHandleProxy:
        try:
            return self._native.handoff(source_handle, target_provider_id, access)
        except RuntimeError as error:
            code, separator, message = str(error).partition(": ")
            if separator and code in _HANDOFF_DIAGNOSTIC_CODES and message:
                raise AssetDocumentContractError(
                    RuntimeDiagnostic("/HandleHandoff", code, message)
                ) from error
            raise

    def invoke(
        self,
        *,
        requirement: PrimitiveRequirement,
        value_arguments: RuntimeValue,
        handle_arguments: tuple[NamedOpaqueHandleArgument, ...],
    ) -> PrimitiveOutcome:
        raw = self._native.invoke(
            requirement.primitive_id,
            requirement.version,
            _runtime_to_native(value_arguments),
            tuple(
                {"name": item.name, "handle": item.handle}
                for item in handle_arguments
            ),
        )
        return _decode_native_outcome(raw)


def _missing_native_requirement() -> None:
    raise contract_error(
        "/RequiredPrimitives",
        "GenericRuntime.RequiredPrimitiveUnavailable",
        "the frozen Generic primitive closure is unavailable",
    )


def _validate_native_catalog(context: object, revision: int) -> None:
    catalog = _require_native_dict(
        context.inspect_catalog(),
        {"runtime_revision", "frozen", "descriptors", "providers"},
        "/PrimitiveCatalog",
    )
    valid_header = (
        type(catalog["runtime_revision"]) is int
        and catalog["runtime_revision"] == revision
        and revision > 0
        and catalog["frozen"] is True
        and type(catalog["descriptors"]) is tuple
        and type(catalog["providers"]) is tuple
    )
    if not valid_header:
        _missing_native_requirement()
    providers = {
        row.get("provider_id")
        for row in catalog["providers"]
        if type(row) is dict and row.get("state") == 0
    }
    available = {
        (row.get("primitive_id"), row.get("version"))
        for row in catalog["descriptors"]
        if type(row) is dict
        and row.get("provider_id") in providers
        and type(row.get("required_consumer_ids")) is tuple
        and GENERIC_ROUTE_ID in row["required_consumer_ids"]
    }
    expected = {
        (item.primitive_id, item.version)
        for item in GENERIC_REQUIRED_PRIMITIVES
    }
    if not expected.issubset(available):
        _missing_native_requirement()


def _invoke_required(
    context: GenericPrimitiveContext,
    primitive_id: str,
    *,
    values: RuntimeValue,
    handles: tuple[NamedOpaqueHandleArgument, ...] = (),
) -> PrimitiveOutcome:
    outcome = context.invoke(
        requirement=requirement(primitive_id),
        value_arguments=values,
        handle_arguments=handles,
    )
    if outcome.status is PrimitiveOutcomeStatus.SUCCEEDED:
        return outcome
    if outcome.diagnostics:
        raise AssetDocumentContractError(outcome.diagnostics[0])
    raise _native_projection_error(
        "/PrimitiveOutcome",
        "failed primitive omitted its diagnostic",
    )


def _single_handle(outcome: PrimitiveOutcome, name: str):
    groups = tuple(
        group
        for group in outcome.handle_results
        if group.name == name
    )
    if len(groups) != 1 or len(groups[0].handles) != 1:
        raise _native_projection_error(
            "/PrimitiveOutcome/HandleResults",
            f"primitive must return exactly one {name} handle",
        )
    return groups[0].handles[0]


def _optional_handle(outcome: PrimitiveOutcome, name: str):
    groups = tuple(
        group
        for group in outcome.handle_results
        if group.name == name
    )
    if len(groups) != 1 or len(groups[0].handles) > 1:
        raise _native_projection_error(
            "/PrimitiveOutcome/HandleResults",
            f"primitive returned an invalid {name} handle group",
        )
    return groups[0].handles[0] if groups[0].handles else None
