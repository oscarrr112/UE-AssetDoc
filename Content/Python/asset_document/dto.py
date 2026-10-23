# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import TypeAlias

from asset_document.diagnostics import DiagnosticSeverity, RuntimeDiagnostic, contract_error


RUNTIME_CONTRACT_VERSION: int = 1


class RuntimeOperation(str, Enum):
    INSPECT = "Inspect"
    SCHEMA = "Schema"
    TEMPLATE = "Template"
    VALIDATE = "Validate"
    PREFLIGHT = "Preflight"
    APPLY = "Apply"
    EXTRACT = "Extract"
    DIFF = "Diff"
    REGENERATE_SIDECAR = "RegenerateSidecar"
    SYNC = "Sync"


class RuntimePhase(str, Enum):
    VALIDATE = "Validate"
    PREFLIGHT = "Preflight"
    APPLY = "Apply"
    REPAIR = "Repair"
    PERSIST = "Persist"
    VERIFY = "Verify"
    COMPLETE = "Complete"
    FAILED = "Failed"


class TerminalStatus(str, Enum):
    SUCCEEDED = "Succeeded"
    FAILED = "Failed"
    PARTIAL = "Partial"
    CANCELLED = "Cancelled"
    DEADLINE_EXCEEDED = "DeadlineExceeded"


_NATIVE_PHASES = (
    RuntimePhase.VALIDATE,
    RuntimePhase.PREFLIGHT,
    RuntimePhase.APPLY,
    RuntimePhase.REPAIR,
    RuntimePhase.PERSIST,
    RuntimePhase.VERIFY,
    RuntimePhase.COMPLETE,
    RuntimePhase.FAILED,
)
_NATIVE_OPERATIONS = tuple(RuntimeOperation)
_NATIVE_SEVERITIES = (
    DiagnosticSeverity.INFO,
    DiagnosticSeverity.WARNING,
    DiagnosticSeverity.ERROR,
)


JsonScalar: TypeAlias = None | bool | int | float | str
JsonValue: TypeAlias = JsonScalar | list["JsonValue"] | dict[str, "JsonValue"]
RuntimeScalar: TypeAlias = JsonScalar | bytes
RuntimeValue: TypeAlias = RuntimeScalar | list["RuntimeValue"] | dict[str, "RuntimeValue"]


def _require_native_dict(value: object, keys: set[str], path: str) -> dict:
    if type(value) is not dict or set(value) != keys:
        raise contract_error(
            path,
            "GenericRuntime.InvalidNativeProjection",
            "native projection has the wrong exact key set",
        )
    return value


def _json_from_native(value: object, path: str = "/CanonicalDocument"):
    if value is None or type(value) in (bool, int, float, str):
        return value
    if type(value) is tuple:
        return [
            _json_from_native(item, f"{path}/{index}")
            for index, item in enumerate(value)
        ]
    if type(value) is dict:
        if any(type(key) is not str for key in value):
            raise contract_error(
                path,
                "GenericRuntime.InvalidNativeProjection",
                "native object keys must be exact strings",
            )
        return {
            key: _json_from_native(item, f"{path}/{key}")
            for key, item in value.items()
        }
    raise contract_error(
        path,
        "GenericRuntime.InvalidNativeProjection",
        "native canonical document is not an exact JSON projection",
    )


def _runtime_to_native(value):
    if type(value) is list:
        return tuple(_runtime_to_native(item) for item in value)
    if type(value) is dict:
        return {key: _runtime_to_native(item) for key, item in value.items()}
    return value


def _enum_from_native(value: object, values: tuple, path: str, code: str):
    if type(value) is not int or not 0 <= value < len(values):
        raise contract_error(path, code, "native enum value is outside the frozen range")
    return values[value]


def _decode_native_request(value: object) -> RuntimeRequestDto:
    row = _require_native_dict(
        value,
        {
            "abi_version",
            "request_id",
            "operation",
            "target_asset_path",
            "requested_profile_class_path",
            "requested_asset_class_path",
            "canonical_document",
            "deadline_unix_milliseconds",
            "cancellation",
            "expected_runtime_revision",
        },
        "/Request",
    )
    cancellation = _require_native_dict(
        row["cancellation"], {"sequence", "requested"}, "/Cancellation"
    )
    return RuntimeRequestDto(
        abi_version=row["abi_version"],
        request_id=row["request_id"],
        operation=_enum_from_native(
            row["operation"],
            _NATIVE_OPERATIONS,
            "/Operation",
            "GenericRuntime.InvalidNativeOperation",
        ),
        target_asset_path=row["target_asset_path"],
        requested_profile_class_path=row["requested_profile_class_path"],
        requested_asset_class_path=row["requested_asset_class_path"],
        canonical_document=_json_from_native(row["canonical_document"]),
        deadline_unix_milliseconds=row["deadline_unix_milliseconds"],
        cancellation=CancellationEvidence(
            cancellation["sequence"], cancellation["requested"]
        ),
        expected_runtime_revision=row["expected_runtime_revision"],
    )


def _validate_native_context(request: RuntimeRequestDto, context: object) -> RuntimePhase:
    try:
        context_request_id = context.request_id
        revision = context.runtime_revision
        native_phase = context.phase
    except AttributeError:
        raise contract_error(
            "/RequestContext",
            "GenericRuntime.InvalidNativeProjection",
            "native context omitted a frozen public property",
        ) from None
    if context_request_id != request.request_id:
        raise contract_error(
            "/RequestId",
            "GenericRuntime.ContextIdentityMismatch",
            "native context request identity does not match the DTO",
        )
    if type(revision) is not int or revision != request.expected_runtime_revision:
        raise contract_error(
            "/RuntimeRevision",
            "GenericRuntime.ContextRevisionMismatch",
            "native context revision does not match the DTO",
        )
    return _enum_from_native(
        native_phase,
        _NATIVE_PHASES,
        "/Phase",
        "GenericRuntime.InvalidNativePhase",
    )


def _representation_error(path: str, message: str):
    from asset_document.diagnostics import contract_error

    return contract_error(path, "DTO.InvalidRepresentationType", message)


def _child_path(path: str, segment: object) -> str:
    return f"{path}/{segment}" if path else f"/{segment}"


def _snapshot_value(
    value: RuntimeValue,
    path: str,
    allow_bytes: bool,
    active: set[int],
) -> RuntimeValue:
    if value is None or type(value) in (bool, int, float, str):
        return value
    if type(value) is bytes:
        if allow_bytes:
            return bytes(value)
        raise _representation_error(path, "Canonical JSON values cannot contain bytes.")
    return _snapshot_container(value, path, allow_bytes, active)


def _snapshot_container(
    value: RuntimeValue,
    path: str,
    allow_bytes: bool,
    active: set[int],
) -> RuntimeValue:
    if type(value) is list:
        return _snapshot_list(value, path, allow_bytes, active)
    if type(value) is dict:
        return _snapshot_dict(value, path, allow_bytes, active)
    raise _representation_error(path, "Unsupported runtime value type.")


def _activate_container(value: RuntimeValue, path: str, active: set[int]) -> int:
    identity = id(value)
    if identity in active:
        raise _representation_error(path, "Runtime values cannot contain cycles.")
    active.add(identity)
    return identity


def _snapshot_list(
    value: list[RuntimeValue],
    path: str,
    allow_bytes: bool,
    active: set[int],
) -> list[RuntimeValue]:
    identity = _activate_container(value, path, active)
    try:
        return [
            _snapshot_value(item, _child_path(path, index), allow_bytes, active)
            for index, item in enumerate(value)
        ]
    finally:
        active.remove(identity)


def _snapshot_dict(
    value: dict[str, RuntimeValue],
    path: str,
    allow_bytes: bool,
    active: set[int],
) -> dict[str, RuntimeValue]:
    identity = _activate_container(value, path, active)
    try:
        result: dict[str, RuntimeValue] = {}
        for key, item in value.items():
            if type(key) is not str:
                raise _representation_error(path, "Runtime object keys must be exact strings.")
            result[key] = _snapshot_value(
                item, _child_path(path, key), allow_bytes, active
            )
        return result
    finally:
        active.remove(identity)


def _snapshot_runtime_value(value: RuntimeValue, path: str) -> RuntimeValue:
    return _snapshot_value(value, path, True, set())


def _snapshot_json_value(value: JsonValue, path: str) -> JsonValue:
    return _snapshot_value(value, path, False, set())


def _require_exact(value: object, expected: type, path: str, label: str) -> None:
    if type(value) is not expected:
        raise _representation_error(path, f"{label} must be an exact {expected.__name__}.")


def _require_exact_tuple(value: object, expected: type, path: str, label: str) -> None:
    if type(value) is not tuple:
        raise _representation_error(path, f"{label} must be an exact tuple.")
    if any(type(item) is not expected for item in value):
        raise _representation_error(
            path, f"Every {label} entry must be exact {expected.__name__}."
        )


def _valid_requirement(requirement: PrimitiveRequirement) -> bool:
    if type(requirement.version) is not int or requirement.version <= 0:
        return False
    if type(requirement.primitive_id) is not str:
        return False
    if not requirement.primitive_id or requirement.primitive_id != requirement.primitive_id.strip():
        return False
    if "." not in requirement.primitive_id:
        return False
    if not requirement.primitive_id.endswith(f".v{requirement.version}"):
        return False
    if type(requirement.required_by_route_id) is not str:
        return False
    return bool(requirement.required_by_route_id) and (
        requirement.required_by_route_id == requirement.required_by_route_id.strip()
    )


@dataclass(frozen=True, slots=True)
class OptionalJsonValue:
    is_set: bool = False
    value: JsonValue = None

    def __post_init__(self) -> None:
        _require_exact(self.is_set, bool, "/CanonicalValue", "is_set")
        if not self.is_set and self.value is not None:
            from asset_document.diagnostics import contract_error

            raise contract_error(
                "/CanonicalValue",
                "DTO.InvalidOptionalValue",
                "An unset optional value must contain JSON null.",
            )
        if self.is_set:
            object.__setattr__(
                self, "value", _snapshot_json_value(self.value, "/CanonicalValue")
            )


@dataclass(frozen=True, slots=True, order=True)
class PrimitiveRequirement:
    primitive_id: str
    version: int
    required_by_route_id: str

    def __post_init__(self) -> None:
        if not _valid_requirement(self):
            from asset_document.diagnostics import contract_error

            raise contract_error(
                "/RequiredPrimitives",
                "DTO.InvalidPrimitiveRequirement",
                "Primitive requirements must contain a normalized versioned ID and route.",
            )


@dataclass(frozen=True, slots=True)
class ChangedRecord:
    authority_id: str
    stable_object_id: str
    path: str
    change_kind: str

    def __post_init__(self) -> None:
        for value, path, label in (
            (self.authority_id, "/ChangedSet/AuthorityId", "authority_id"),
            (self.stable_object_id, "/ChangedSet/StableObjectId", "stable_object_id"),
            (self.path, "/ChangedSet/Path", "path"),
            (self.change_kind, "/ChangedSet/ChangeKind", "change_kind"),
        ):
            _require_exact(value, str, path, label)


@dataclass(frozen=True, slots=True)
class CancellationEvidence:
    sequence: int = 0
    requested: bool = False

    def __post_init__(self) -> None:
        valid = (
            type(self.sequence) is int
            and 0 <= self.sequence <= 2**64 - 1
            and type(self.requested) is bool
            and (not self.requested or self.sequence != 0)
        )
        if not valid:
            from asset_document.diagnostics import contract_error

            raise contract_error(
                "/Cancellation",
                "DTO.InvalidCancellationEvidence",
                "Cancellation evidence must contain a uint64 sequence and a valid request marker.",
            )


@dataclass(frozen=True, slots=True)
class RuntimeRequestDto:
    request_id: str
    operation: RuntimeOperation
    target_asset_path: str
    requested_profile_class_path: str
    requested_asset_class_path: str
    canonical_document: JsonValue
    deadline_unix_milliseconds: int = 0
    cancellation: CancellationEvidence = CancellationEvidence()
    expected_runtime_revision: int = 0
    abi_version: int = RUNTIME_CONTRACT_VERSION

    def __post_init__(self) -> None:
        if type(self.abi_version) is not int or self.abi_version != RUNTIME_CONTRACT_VERSION:
            from asset_document.diagnostics import contract_error

            raise contract_error(
                "/AbiVersion", "DTO.AbiVersionMismatch", "Only runtime ABI v1 is supported."
            )
        for value, path, label in (
            (self.request_id, "/RequestId", "request_id"),
            (self.target_asset_path, "/TargetAssetPath", "target_asset_path"),
            (
                self.requested_profile_class_path,
                "/RequestedProfileClassPath",
                "requested_profile_class_path",
            ),
            (
                self.requested_asset_class_path,
                "/RequestedAssetClassPath",
                "requested_asset_class_path",
            ),
        ):
            _require_exact(value, str, path, label)
        if type(self.operation) is not RuntimeOperation:
            raise _representation_error(
                "/Operation", "operation must be an exact RuntimeOperation."
            )
        _require_exact(
            self.deadline_unix_milliseconds,
            int,
            "/DeadlineUnixMilliseconds",
            "deadline_unix_milliseconds",
        )
        _require_exact(
            self.expected_runtime_revision,
            int,
            "/ExpectedRuntimeRevision",
            "expected_runtime_revision",
        )
        if type(self.cancellation) is not CancellationEvidence:
            raise _representation_error(
                "/Cancellation", "cancellation must be exact CancellationEvidence."
            )
        object.__setattr__(
            self,
            "canonical_document",
            _snapshot_json_value(self.canonical_document, "/CanonicalDocument"),
        )


@dataclass(frozen=True, slots=True)
class RuntimeResultDto:
    request_id: str
    terminal_status: TerminalStatus
    final_phase: RuntimePhase
    runtime_revision: int
    diagnostics: tuple[RuntimeDiagnostic, ...] = ()
    canonical_value: OptionalJsonValue = OptionalJsonValue()
    canonical_bytes: bytes = b""
    semantic_hash: str = ""
    codec_version: int = 0
    changed_set: tuple[ChangedRecord, ...] = ()
    abi_version: int = RUNTIME_CONTRACT_VERSION

    def __post_init__(self) -> None:
        if type(self.abi_version) is not int or self.abi_version != RUNTIME_CONTRACT_VERSION:
            from asset_document.diagnostics import contract_error

            raise contract_error(
                "/AbiVersion", "DTO.AbiVersionMismatch", "Only runtime ABI v1 is supported."
            )
        _require_exact(self.request_id, str, "/RequestId", "request_id")
        if type(self.terminal_status) is not TerminalStatus:
            raise _representation_error(
                "/TerminalStatus", "terminal_status must be an exact TerminalStatus."
            )
        if type(self.final_phase) is not RuntimePhase:
            raise _representation_error(
                "/FinalPhase", "final_phase must be an exact RuntimePhase."
            )
        _require_exact(self.runtime_revision, int, "/RuntimeRevision", "runtime_revision")
        # Module-level RuntimeDiagnostic on purpose: the native runtime restores sys.modules between phase calls, so
        # an in-call import would be a second copy of the class and reject the diagnostics a handler produced.
        _require_exact_tuple(
            self.diagnostics, RuntimeDiagnostic, "/Diagnostics", "diagnostics"
        )
        if type(self.canonical_value) is not OptionalJsonValue:
            raise _representation_error(
                "/CanonicalValue", "canonical_value must be exact OptionalJsonValue."
            )
        _require_exact(self.canonical_bytes, bytes, "/CanonicalBytes", "canonical_bytes")
        _require_exact(self.semantic_hash, str, "/SemanticHash", "semantic_hash")
        _require_exact(self.codec_version, int, "/CodecVersion", "codec_version")
        _require_exact_tuple(
            self.changed_set, ChangedRecord, "/ChangedSet", "changed_set"
        )


def _diagnostic_native_projection(value: RuntimeDiagnostic) -> dict:
    return {
        "path": value.path,
        "code": value.code,
        "message": value.message,
        "severity": _NATIVE_SEVERITIES.index(value.severity),
        "context": tuple(
            {"key": item.key, "value": _runtime_to_native(item.value)}
            for item in value.context
        ),
    }


def _changed_native_projection(value: ChangedRecord) -> dict:
    return {
        "authority_id": value.authority_id,
        "stable_object_id": value.stable_object_id,
        "path": value.path,
        "change_kind": value.change_kind,
    }


def _project_native_result(value: RuntimeResultDto) -> dict:
    canonical = (
        _runtime_to_native(value.canonical_value.value)
        if value.canonical_value.is_set
        else None
    )
    return {
        "abi_version": value.abi_version,
        "request_id": value.request_id,
        "terminal_status": tuple(TerminalStatus).index(value.terminal_status),
        "final_phase": _NATIVE_PHASES.index(value.final_phase),
        "runtime_revision": value.runtime_revision,
        "diagnostics": tuple(
            _diagnostic_native_projection(item)
            for item in value.diagnostics
        ),
        "canonical_value": canonical,
        "canonical_bytes": value.canonical_bytes,
        "semantic_hash": value.semantic_hash,
        "codec_version": value.codec_version,
        "changed_set": tuple(
            _changed_native_projection(item)
            for item in value.changed_set
        ),
    }


def _project_native_failure(
    request_id: str,
    revision: int,
    diagnostic: RuntimeDiagnostic,
    additional: tuple = (),
) -> dict:
    return {
        "abi_version": 1,
        "request_id": request_id,
        "terminal_status": tuple(TerminalStatus).index(TerminalStatus.FAILED),
        "final_phase": _NATIVE_PHASES.index(RuntimePhase.FAILED),
        "runtime_revision": revision,
        "diagnostics": tuple(_diagnostic_native_projection(item) for item in (diagnostic, *additional)),
        "canonical_value": None,
        "canonical_bytes": b"",
        "semantic_hash": "",
        "codec_version": 0,
        "changed_set": (),
    }
