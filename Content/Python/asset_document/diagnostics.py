from __future__ import annotations

from contextlib import contextmanager
from contextvars import ContextVar
from dataclasses import dataclass
from enum import Enum
from typing import TYPE_CHECKING, Iterator

if TYPE_CHECKING:
    from asset_document.dto import RuntimeValue


class DiagnosticSeverity(str, Enum):
    INFO = "Info"
    WARNING = "Warning"
    ERROR = "Error"


@dataclass(frozen=True, slots=True)
class DiagnosticContext:
    key: str
    value: RuntimeValue

    def __post_init__(self) -> None:
        if type(self.key) is not str:
            raise contract_error(
                "/Diagnostics/Context/Key",
                "DTO.InvalidRepresentationType",
                "Diagnostic context keys must be exact strings.",
            )
        from asset_document.dto import _snapshot_runtime_value

        object.__setattr__(
            self,
            "value",
            _snapshot_runtime_value(self.value, "/Diagnostics/Context/Value"),
        )


@dataclass(frozen=True, slots=True)
class RuntimeDiagnostic:
    path: str
    code: str
    message: str
    severity: DiagnosticSeverity = DiagnosticSeverity.ERROR
    context: tuple[DiagnosticContext, ...] = ()

    def __post_init__(self) -> None:
        for value, path, label in (
            (self.path, "/Diagnostics/Path", "path"),
            (self.code, "/Diagnostics/Code", "code"),
            (self.message, "/Diagnostics/Message", "message"),
        ):
            if type(value) is not str:
                raise contract_error(
                    path,
                    "DTO.InvalidRepresentationType",
                    f"Diagnostic {label} must be an exact string.",
                )
        if type(self.severity) is not DiagnosticSeverity:
            raise contract_error(
                "/Diagnostics/Severity",
                "DTO.InvalidRepresentationType",
                "Diagnostic severity must be a DiagnosticSeverity value.",
            )
        if type(self.context) is not tuple or any(
            type(item) is not DiagnosticContext for item in self.context
        ):
            raise contract_error(
                "/Diagnostics/Context",
                "DTO.InvalidRepresentationType",
                "Diagnostic context must be an exact tuple of DiagnosticContext values.",
            )


class AssetDocumentContractError(ValueError):
    diagnostic: RuntimeDiagnostic

    def __init__(self, diagnostic: RuntimeDiagnostic, additional: tuple = ()) -> None:
        self.diagnostic = diagnostic
        # Further independent failures found in the same pass (reported after the first).
        self.additional = tuple(additional)
        super().__init__(
            f"{diagnostic.code} at {diagnostic.path}: {diagnostic.message}"
            + (f" (+{len(self.additional)} more)" if self.additional else "")
        )

    @property
    def diagnostics(self) -> tuple:
        return (self.diagnostic,) + self.additional


def contract_errors(diagnostics) -> AssetDocumentContractError:
    """One failure carrying every diagnostic of a check that reports all it finds (the first leads)."""
    items = tuple(diagnostics)
    if not items:
        raise ValueError("contract_errors needs at least one diagnostic")
    return AssetDocumentContractError(items[0], items[1:])


def contract_error(path: str, code: str, message: str) -> AssetDocumentContractError:
    return AssetDocumentContractError(
        RuntimeDiagnostic(path=path, code=code, message=message)
    )


def primitive_error(path: str, source: RuntimeDiagnostic, primitive: str) -> AssetDocumentContractError:
    """A primitive failure reported at the document location the AI wrote.

    The message stays the primitive's own; which primitive failed, and where in its arguments,
    goes to the diagnostic context (public result: ``details``), not into the message.
    """
    context = (
        DiagnosticContext("primitive", primitive),
        DiagnosticContext("primitive_path", source.path),
    ) + tuple(item for item in source.context if item.key not in ("primitive", "primitive_path"))
    return AssetDocumentContractError(
        RuntimeDiagnostic(path=path, code=source.code, message=source.message, context=context)
    )


class DiagnosedChanges(tuple):
    """A handler's changed set that also carries non-fatal diagnostics for the success Result."""

    diagnostics: tuple[RuntimeDiagnostic, ...] = ()


_READ_WARNINGS: ContextVar[list | None] = ContextVar("asset_document_read_warnings", default=None)


def read_warning(path: str, code: str, message: str) -> None:
    """A warning of a read (Extract, Inspect): the read goes on and its result carries the warning. Outside a read
    (Validate, Apply, Diff) nothing is recorded."""
    sink = _READ_WARNINGS.get()
    if sink is not None:
        diagnostic = RuntimeDiagnostic(path=path, code=code, message=message, severity=DiagnosticSeverity.WARNING)
        if diagnostic not in sink:
            sink.append(diagnostic)


@contextmanager
def collecting_read_warnings() -> Iterator[list]:
    """Collects the read_warning calls of one read."""
    sink: list = []
    token = _READ_WARNINGS.set(sink)
    try:
        yield sink
    finally:
        _READ_WARNINGS.reset(token)


def with_diagnostics(changes, diagnostics) -> tuple:
    diagnostics = tuple(diagnostics)
    if not diagnostics:
        return tuple(changes)
    result = DiagnosedChanges(changes)
    result.diagnostics = diagnostics
    return result


def describe_json(value: object, limit: int = 400) -> str:
    """Compact JSON of ``value`` for a diagnostic message, never cut inside a value: when it is longer than
    ``limit``, whole object entries / array items are left out and counted ("+N more")."""
    import json

    def text(item: object) -> str:
        try:
            return json.dumps(item, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
        except (TypeError, ValueError):
            return repr(item)

    def bounded(item: object, budget: int) -> str:
        full = text(item)
        if len(full) <= budget or not isinstance(item, (dict, list)) or not item:
            return full
        if isinstance(item, dict):
            entries = [(text(str(key)) + ":", item[key]) for key in sorted(item, key=str)]
            opener, closer = "{", "}"
        else:
            entries = [("", element) for element in item]
            opener, closer = "[", "]"
        parts: list[str] = []
        used = 2
        for index, (prefix, element) in enumerate(entries):
            remaining = len(entries) - index
            reserve = 0 if remaining == 1 else len(f',"+{remaining} more"')
            room = budget - used - len(prefix) - reserve - (1 if parts else 0)
            # The first entry is always shown (shortened only by leaving out its own inner entries).
            piece = bounded(element, max(room, 0) if parts else max(room, budget // 2))
            if parts and len(piece) > room:
                parts.append(text(f"+{remaining} more"))
                break
            parts.append(prefix + piece)
            used += len(prefix) + len(piece) + 1
        return opener + ",".join(parts) + closer

    return bounded(value, limit)
