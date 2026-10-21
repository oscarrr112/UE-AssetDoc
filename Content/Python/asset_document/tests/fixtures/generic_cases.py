from __future__ import annotations

from collections.abc import Callable
import copy
from dataclasses import dataclass
from types import SimpleNamespace

from asset_document.diagnostics import RuntimeDiagnostic, contract_error
from asset_document.dto import (
    ChangedRecord,
    JsonValue,
    PrimitiveRequirement,
    RuntimePhase,
    RuntimeValue,
    _snapshot_runtime_value,
)
from asset_document.regions.primitive_catalog import (
    GENERIC_REQUIRED_PRIMITIVES,
    NamedOpaqueHandleArgument,
    NamedOpaqueHandleResult,
    PrimitiveOutcome,
    PrimitiveOutcomeStatus,
)


EXACT_CLASS = "/Script/Engine.DataAsset"
GENERATED_CLASS = "/Game/Generic/BP_Generic.BP_Generic_C"
TARGET = "/Game/Generic/DA_Generic"

ENUMERATE_ID = "editor.property.enumerate_admitted.v1"
ENUMERATE_BATCH_ID = "editor.property.enumerate_admitted_batch.v1"
PREFLIGHT_ID = "editor.property.preflight_admitted.v1"
READ_ID = "editor.property.read_admitted.v1"
WRITE_ID = "editor.property.write_admitted.v1"
WRITE_BATCH_ID = "editor.property.write_admitted_batch.v1"


def missing_body_document() -> dict[str, JsonValue]:
    return {
        "SchemaVersion": 1,
        "Target": TARGET,
        "Class": EXACT_CLASS,
        "Action": "CreateOrUpdate",
        "Definitions": {},
        "Properties": {},
    }


def empty_body_document() -> dict[str, JsonValue]:
    document = missing_body_document()
    document["Body"] = {}
    return document


def non_empty_body_document() -> dict[str, JsonValue]:
    document = empty_body_document()
    document["Body"] = {"Unsupported": True}
    return document


def rich_document() -> dict[str, JsonValue]:
    document = empty_body_document()
    document["Definitions"] = {
        "asset/~id": {
            "Kind": "AssetRef",
            "Path": "/Game/Generic/Reference.Reference",
            "Class": EXACT_CLASS,
        },
        "class": {"Kind": "ClassRef", "Class": EXACT_CLASS},
        "struct": {
            "Kind": "StructValue",
            "Struct": "/Script/CoreUObject.Vector",
            "Properties": {"X": 1.0, "Y": 2.0, "Z": 3.0},
        },
        "hop-b": {"Kind": "DefinitionRef", "Id": "class"},
        "hop-a": {"Kind": "DefinitionRef", "Id": "hop-b"},
    }
    document["Properties"] = {"Count": 7, "Enabled": True, "Title": "authored"}
    return document


def admitted_property_rows() -> list[dict[str, JsonValue]]:
    return [
        {"name": "Title", "readable": True, "writable": True},
        {"name": "Ratio", "readable": True, "writable": True, "default": 1.0},
        {"name": "ReadOnly", "readable": True, "writable": False, "default": "locked"},
        {"name": "Enabled", "readable": True, "writable": True, "default": False},
        {"name": "Count", "readable": True, "writable": True, "default": 0},
    ]


def changed_record(name: str = "Title", kind: str = "Replace") -> ChangedRecord:
    return ChangedRecord(
        WRITE_BATCH_ID,
        "asset:generic-test",
        f"/Properties/{name.replace('~', '~0').replace('/', '~1')}",
        kind,
    )


def diagnostic(
    code: str = "Primitive.TestFailure",
    path: str = "/Primitive",
) -> RuntimeDiagnostic:
    return RuntimeDiagnostic(path, code, "test diagnostic")


def success_outcome(
    *,
    value: RuntimeValue = None,
    handle_results: tuple[NamedOpaqueHandleResult, ...] = (),
    changed_set: tuple[ChangedRecord, ...] = (),
    diagnostics: tuple[RuntimeDiagnostic, ...] = (),
) -> PrimitiveOutcome:
    return PrimitiveOutcome(
        PrimitiveOutcomeStatus.SUCCEEDED,
        value,
        handle_results,
        changed_set,
        diagnostics,
    )


def failed_outcome(
    code: str = "Primitive.TestFailure",
    path: str = "/Primitive",
) -> PrimitiveOutcome:
    return PrimitiveOutcome(
        PrimitiveOutcomeStatus.FAILED,
        None,
        (),
        (),
        (diagnostic(code, path),),
    )


def unavailable_outcome() -> PrimitiveOutcome:
    return PrimitiveOutcome(
        PrimitiveOutcomeStatus.UNAVAILABLE,
        None,
        (),
        (),
        (diagnostic("Primitive.Unavailable"),),
    )


def malformed_outcome(**overrides: object) -> object:
    values: dict[str, object] = {
        "status": PrimitiveOutcomeStatus.SUCCEEDED,
        "value": None,
        "handle_results": (),
        "changed_set": (),
        "diagnostics": (),
    }
    values.update(overrides)
    return SimpleNamespace(**values)


_HANDLE_CONSTRUCTION_SEAL = object()


class MockOpaqueHandle:
    __slots__ = ()

    def __new__(cls, seal: object = None):
        if seal is not _HANDLE_CONSTRUCTION_SEAL:
            raise TypeError("opaque handles are issued only by MockPrimitiveContext")
        return super().__new__(cls)

    def __copy__(self):
        raise TypeError("opaque handles cannot be copied")

    def __deepcopy__(self, memo: dict[int, object]):
        raise TypeError("opaque handles cannot be deep-copied")

    def __reduce_ex__(self, protocol: int):
        raise TypeError("opaque handles cannot be serialized")

    def __repr__(self) -> str:
        return "<MockOpaqueHandle>"


@dataclass(frozen=True, slots=True)
class MockInvocation:
    requirement: PrimitiveRequirement
    value_arguments: RuntimeValue
    handle_arguments: tuple[NamedOpaqueHandleArgument, ...]


MockHandler = Callable[[MockInvocation], object]


class MockPrimitiveContext:
    def __init__(
        self,
        phase: RuntimePhase,
        *,
        active_transaction: bool = True,
    ) -> None:
        self._phase = phase
        self._active_transaction = active_transaction
        self._expired = False
        self._issued: list[MockOpaqueHandle] = []
        self._handlers: dict[str, MockHandler] = {}
        self._calls: list[MockInvocation] = []
        self._successful_changes: list[ChangedRecord] = []
        self._handler_calls = 0

    @property
    def phase(self) -> RuntimePhase:
        return self._phase

    @property
    def calls(self) -> tuple[MockInvocation, ...]:
        return tuple(self._calls)

    @property
    def successful_changes(self) -> tuple[ChangedRecord, ...]:
        return tuple(self._successful_changes)

    @property
    def handler_calls(self) -> int:
        return self._handler_calls

    @property
    def runtime_dispositions(self) -> dict[str, str]:
        return {
            item.primitive_id: "unadmitted"
            for item in GENERIC_REQUIRED_PRIMITIVES
        }

    def issue_handle(self) -> MockOpaqueHandle:
        handle = MockOpaqueHandle(_HANDLE_CONSTRUCTION_SEAL)
        self._issued.append(handle)
        return handle

    def expire(self) -> None:
        self._expired = True

    def set_handler(self, primitive_id: str, handler: MockHandler) -> None:
        self._handlers[primitive_id] = handler

    def queue_outcomes(self, primitive_id: str, *outcomes: object) -> None:
        pending = list(outcomes)

        def queued_handler(invocation: MockInvocation) -> object:
            if not pending:
                raise AssertionError(f"unexpected invocation: {primitive_id}")
            return pending.pop(0)

        self.set_handler(primitive_id, queued_handler)

    def invoke(
        self,
        *,
        requirement: PrimitiveRequirement,
        value_arguments: RuntimeValue,
        handle_arguments: tuple[NamedOpaqueHandleArgument, ...],
    ) -> PrimitiveOutcome:
        self._validate_requirement(requirement)
        self._validate_handle_arguments(handle_arguments)
        self._validate_transaction(requirement)
        invocation = MockInvocation(
            requirement,
            _snapshot_runtime_value(value_arguments, "/ValueArguments"),
            handle_arguments,
        )
        handler = self._handlers.get(requirement.primitive_id)
        if handler is None:
            return unavailable_outcome()
        self._handler_calls += 1
        outcome = handler(invocation)
        self._validate_returned_handles(outcome)
        self._calls.append(invocation)
        if type(outcome) is PrimitiveOutcome and outcome.status is PrimitiveOutcomeStatus.SUCCEEDED:
            self._successful_changes.extend(outcome.changed_set)
        return outcome

    def _validate_requirement(self, value: PrimitiveRequirement) -> None:
        if value not in GENERIC_REQUIRED_PRIMITIVES:
            raise contract_error(
                "/RequiredPrimitives",
                "PrimitiveCatalog.UnknownGenericRequirement",
                "mock context accepts only frozen Generic requirements",
            )

    def _validate_handle_arguments(
        self, values: tuple[NamedOpaqueHandleArgument, ...]
    ) -> None:
        if type(values) is not tuple:
            self._invalid_handle()
        for value in values:
            if type(value) is not NamedOpaqueHandleArgument:
                self._invalid_handle()
            if not self._attests(value.handle):
                self._invalid_handle()

    def _validate_returned_handles(self, outcome: object) -> None:
        if type(outcome) is not PrimitiveOutcome:
            return
        for group in outcome.handle_results:
            self._validate_returned_group(group)

    def _validate_returned_group(self, group: NamedOpaqueHandleResult) -> None:
        for handle in group.handles:
            if not self._attests(handle):
                self._invalid_handle()

    def _validate_transaction(self, requirement: PrimitiveRequirement) -> None:
        if requirement.primitive_id != WRITE_BATCH_ID:
            return
        if self._phase not in (RuntimePhase.APPLY, RuntimePhase.REPAIR):
            self._invalid_phase()
        if not self._active_transaction:
            self._invalid_phase()

    def _attests(self, handle: object) -> bool:
        if self._expired or type(handle) is not MockOpaqueHandle:
            return False
        return any(handle is issued for issued in self._issued)

    @staticmethod
    def _invalid_handle() -> None:
        raise contract_error(
            "/HandleArguments",
            "PrimitiveCatalog.InvalidOpaqueHandle",
            "opaque handle was not issued by this bound context",
        )

    @staticmethod
    def _invalid_phase() -> None:
        raise contract_error(
            "/Phase",
            "Generic.InvalidMutationPhase",
            "mutation requires the matching active transaction phase",
        )


def property_enumeration_outcome(
    context: MockPrimitiveContext,
    rows: list[dict[str, JsonValue]] | None = None,
) -> tuple[PrimitiveOutcome, tuple[MockOpaqueHandle, ...]]:
    source_rows = admitted_property_rows() if rows is None else copy.deepcopy(rows)
    handles = tuple(context.issue_handle() for _ in source_rows)
    outcome = success_outcome(
        value={"properties": source_rows},
        handle_results=(NamedOpaqueHandleResult("properties", handles),),
    )
    return outcome, handles


def property_batch_enumeration_outcome(
    context: MockPrimitiveContext,
    rows: list[dict[str, JsonValue]] | None = None,
) -> tuple[PrimitiveOutcome, tuple[MockOpaqueHandle, ...], MockOpaqueHandle]:
    source_rows = admitted_property_rows() if rows is None else copy.deepcopy(rows)
    property_handles = tuple(context.issue_handle() for _ in source_rows)
    property_batch = context.issue_handle()
    outcome = success_outcome(
        value={"properties": source_rows},
        handle_results=(
            NamedOpaqueHandleResult("properties", property_handles),
            NamedOpaqueHandleResult("property_batch", (property_batch,)),
        ),
    )
    return outcome, property_handles, property_batch
