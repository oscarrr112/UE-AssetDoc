# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Authored rules: checks that judge new input, never the asset's own state.

A profile's ``canonicalize`` mixes two kinds of checks. A *shape* check (a wrong JSON type, an unknown key) raises
as before. An *authored rule* judges a value the engine can hold but that is not accepted as new input (a name with
spaces, a missing reference, an out-of-range number, a duplicate): it is written ``authored_rule(path, code,
message)`` instead of ``raise``. What the call does depends on the read or write around it:

* while the current asset is read (Extract, Inspect, sidecar mirroring, the asset side of Diff and Verify), the value
  is kept as read and the rule becomes a read warning;
* while a request document is canonicalized (Validate, Diff, Apply), the finding is recorded and canonicalize goes
  on; ``settle`` then waives every finding whose judged values the current asset already holds at the same place
  (an unchanged extract re-applies), and fails with the rest;
* after settlement nothing is judged again (the profile's own nested canonicalize calls);
* outside all of these (direct calls) it raises, as a shape check does.

``judged`` names the values a rule judges (JSON pointers into the document); by default its own path. A relational
rule names both sides, e.g. the two notifies that overlap. A rule that judges a name used as an object key (a String
Table key, a metadata id) judges ``key(pointer)``: the member is unchanged when the current asset has a member of that
name, whatever its value.

Identity matching (``locate``) finds an authored value's place in the current document: object members by key;
lists of objects by their identity field (``Name``, ``Id``, ``Key``, ``NodeGuid``, ``Guid``; duplicates pair in
order, an equal element first), other lists by an equal element first and then by index.
"""
from __future__ import annotations

from contextlib import contextmanager
from contextvars import ContextVar
from dataclasses import dataclass, field
from typing import Iterator

from asset_document.diagnostics import RuntimeDiagnostic, contract_error, contract_errors, read_warning
from asset_document.diff.semantic_compare import as_authored, same_value

__all__ = (
    "authored_rule", "reading_live", "authoring", "settled", "settle", "locate", "changed_indices",
    "is_reading_live", "Finding", "key",
)

_LIVE, _AUTHORED, _SETTLED = "live", "authored", "settled"
_IDENTITY_FIELDS = ("Name", "Id", "Key", "NodeGuid", "Guid")


@dataclass(frozen=True)
class _Key:
    pointer: str


def key(pointer: str) -> _Key:
    """A judged object-member name (not its value): see the module docstring."""
    return _Key(pointer)


@dataclass(frozen=True)
class Finding:
    path: str
    code: str
    message: str
    judged: tuple

    def diagnostic(self) -> RuntimeDiagnostic:
        return RuntimeDiagnostic(path=self.path, code=self.code, message=self.message)


@dataclass
class _Mode:
    kind: str
    findings: list = field(default_factory=list)


_MODE: ContextVar[_Mode | None] = ContextVar("asset_document_authored_rule_mode", default=None)


def authored_rule(path: str, code: str, message: str, judged=None) -> None:
    """A rule for new input; see the module docstring."""
    mode = _MODE.get()
    if mode is None:
        raise contract_error(path, code, message)
    if mode.kind == _LIVE:
        read_warning(path, code, message)
    elif mode.kind == _AUTHORED:
        finding = Finding(path, code, message, tuple(judged) if judged else (path,))
        if finding not in mode.findings:
            mode.findings.append(finding)


def is_reading_live() -> bool:
    mode = _MODE.get()
    return mode is not None and mode.kind == _LIVE


@contextmanager
def _entering(mode: _Mode) -> Iterator[_Mode]:
    token = _MODE.set(mode)
    try:
        yield mode
    finally:
        _MODE.reset(token)


@contextmanager
def reading_live() -> Iterator[None]:
    """The current asset is read: rules become read warnings, values stay as read."""
    with _entering(_Mode(_LIVE)):
        yield


@contextmanager
def authoring() -> Iterator[list]:
    """A request document is canonicalized: findings are recorded (the list it yields) for ``settle``. A failure
    that the recorded findings explain (a later check tripping over a value a rule already refused) is reported as
    those findings."""
    with _entering(_Mode(_AUTHORED)) as mode:
        try:
            yield mode.findings
        except Exception as failure:
            if mode.findings and not hasattr(failure, "diagnostic"):
                raise contract_errors(item.diagnostic() for item in mode.findings) from None
            raise


@contextmanager
def settled() -> Iterator[None]:
    """After settlement: rules are no longer judged (they were, on the same document)."""
    with _entering(_Mode(_SETTLED)):
        yield


# ---------------------------------------------------------------------------
# Settlement
# ---------------------------------------------------------------------------
def _tokens(pointer: str) -> list[str]:
    if pointer in ("", "/"):
        return []
    return [token.replace("~1", "/").replace("~0", "~") for token in pointer.lstrip("/").split("/")]


def _identity_field(left: list, right: list) -> str | None:
    items = list(left) + list(right)
    if not items or not all(type(item) is dict for item in items):
        return None
    for name in _IDENTITY_FIELDS:
        if all(type(item.get(name)) is str and item.get(name) for item in items):
            return name
    return None


def _covers(authored: object, live: object) -> bool:
    return same_value(authored, as_authored(authored, live))


def pair_lists(authored: list, live: list) -> list[int | None]:
    """For each authored element, the index of its current counterpart (None: new)."""
    key = _identity_field(authored, live)
    taken: set[int] = set()
    result: list[int | None] = [None] * len(authored)
    if key is not None:
        for index, item in enumerate(authored):
            candidates = [other for other, value in enumerate(live)
                          if other not in taken and value[key] == item[key]]
            chosen = next((other for other in candidates if _covers(item, live[other])), None)
            if chosen is None and candidates:
                chosen = candidates[0]
            if chosen is not None:
                taken.add(chosen)
                result[index] = chosen
        return result
    for index, item in enumerate(authored):
        chosen = next((other for other, value in enumerate(live) if other not in taken and _covers(item, value)), None)
        if chosen is not None:
            taken.add(chosen)
            result[index] = chosen
    for index in range(len(authored)):
        if result[index] is None and index < len(live) and index not in taken:
            taken.add(index)
            result[index] = index
    return result


_MISSING = object()


def locate(authored_root: object, live_root: object, pointer: str) -> tuple[object, object]:
    """(authored value, current value) at ``pointer`` of the authored document; either is ``_MISSING`` when the
    authored document has no such place or the current one has no counterpart for it."""
    authored, live = authored_root, live_root
    for token in _tokens(pointer):
        if type(authored) is dict:
            if token not in authored:
                return _MISSING, _MISSING
            authored = authored[token]
            live = live.get(token, _MISSING) if type(live) is dict else _MISSING
        elif type(authored) is list:
            try:
                index = int(token)
            except ValueError:
                return _MISSING, _MISSING
            if not 0 <= index < len(authored):
                return _MISSING, _MISSING
            if type(live) is list:
                partner = pair_lists(authored, live)[index]
                live = live[partner] if partner is not None else _MISSING
            else:
                live = _MISSING
            authored = authored[index]
        else:
            return _MISSING, _MISSING
        if live is _MISSING:
            return authored, _MISSING
    return authored, live


def unchanged(authored_root: object, live_root: object, pointer) -> bool:
    """True when the current document holds the authored value at ``pointer``'s identity-matched place (for
    ``key(pointer)``: has a member there)."""
    if live_root is None:
        return False
    if type(pointer) is _Key:
        authored, live = locate(authored_root, live_root, pointer.pointer)
        return authored is not _MISSING and live is not _MISSING
    authored, live = locate(authored_root, live_root, pointer)
    return authored is not _MISSING and live is not _MISSING and _covers(authored, live)


def settle(findings, authored_root: object, live_root: object | None) -> None:
    """Fails with every finding whose judged values are not all unchanged (``live_root`` None: no current asset,
    every finding counts)."""
    remaining = [item for item in findings
                 if not all(unchanged(authored_root, live_root, pointer) for pointer in item.judged)]
    if remaining:
        raise contract_errors(item.diagnostic() for item in remaining)


def changed_indices(authored: list, live: list) -> list[int]:
    """Indices of authored elements that are new or differ from their current counterpart (apply-time checks judge
    only these)."""
    pairs = pair_lists(authored, live)
    return [index for index, partner in enumerate(pairs)
            if partner is None or not _covers(authored[index], live[partner])]
