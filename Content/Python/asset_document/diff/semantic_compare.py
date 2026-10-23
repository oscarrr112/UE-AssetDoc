# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Semantic comparison of an authored document value with the value read back from the asset (pure).

The shared rules post-apply verification (and Diff) use when the profile's own comparison says "same":

* numbers compare numerically, within float32 precision where the engine stores floats (``0.8`` authored,
  ``0.800000011920929`` read back; ``"1.0"`` and ``"1.000000"`` as pin / parameter text);
* keyed collections are matched by key, not by index (``match_by_key``);
* a struct value lists the members it sets: members it omits are not compared (``as_authored``), also inside
  the elements of an array of structs;
* a mismatch is described at the first place the values really differ (``first_difference``), with that
  element's complete values (``describe``): never a float that only differs in its last digits, never an
  element paired by index when the list is keyed.
"""
from __future__ import annotations

import json
import math
import struct

__all__ = (
    "same_number", "number_text", "same_scalar_text", "same_value", "as_authored", "match_by_key",
    "first_difference", "describe", "ABSENT",
)

_FLOAT32_MAX = 3.4028235e38


def _float32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def _is_number(value: object) -> bool:
    return type(value) in (int, float)


def same_number(left: float, right: float) -> bool:
    """Equal numbers: exactly, as the same float32, or within float32 precision (relative 1e-6)."""
    if left == right:
        return True
    if not (math.isfinite(left) and math.isfinite(right)):
        return False
    if abs(left) <= _FLOAT32_MAX and abs(right) <= _FLOAT32_MAX and _float32(float(left)) == _float32(float(right)):
        return True
    return math.isclose(left, right, rel_tol=1e-6, abs_tol=1e-6)


def number_text(text: object) -> float | None:
    """The number a value's text form states ("1.0", "1.000000", " 3 "), or None."""
    if type(text) is not str:
        return None
    stripped = text.strip()
    if not stripped or stripped.lower() in ("nan", "inf", "-inf", "infinity", "-infinity", "+inf"):
        return None
    try:
        return float(stripped)
    except ValueError:
        return None


def same_scalar_text(left: object, right: object) -> bool:
    """Two text forms of one scalar value: equal text, the same number, or the same bool (any case)."""
    if left == right:
        return True
    if type(left) is not str or type(right) is not str:
        return False
    if left.strip().lower() in ("true", "false") and right.strip().lower() in ("true", "false"):
        return left.strip().lower() == right.strip().lower()
    a, b = number_text(left), number_text(right)
    return a is not None and b is not None and same_number(a, b)


def same_value(authored: object, observed: object) -> bool:
    """Structural equality with numbers compared by ``same_number`` (bools are not numbers)."""
    if type(authored) is bool or type(observed) is bool:
        return type(authored) is type(observed) and authored == observed
    if _is_number(authored) and _is_number(observed):
        return same_number(authored, observed)
    if type(authored) is dict and type(observed) is dict:
        return authored.keys() == observed.keys() and all(same_value(authored[key], observed[key]) for key in authored)
    if type(authored) in (list, tuple) and type(observed) in (list, tuple):
        return len(authored) == len(observed) and all(same_value(a, b) for a, b in zip(authored, observed))
    return type(authored) is type(observed) and authored == observed


def as_authored(authored: object, observed: object, is_struct=None) -> object:
    """``observed`` in the shape of ``authored``: the members of a struct the authored value omits are left out
    (they have the struct's default, or keep their value on an edit), recursively, also in the elements of arrays
    of the same length. ``is_struct(authored, observed)`` tells a struct from a map (both are objects); the default
    takes every object whose keys the observed value all has as a struct."""
    if type(authored) is dict and type(observed) is dict:
        struct_like = is_struct(authored, observed) if is_struct is not None else set(authored) <= set(observed)
        if not struct_like:
            return observed
        return {key: as_authored(value, observed[key], is_struct) for key, value in authored.items() if key in observed}
    if type(authored) is list and type(observed) is list and len(authored) == len(observed):
        return [as_authored(mine, theirs, is_struct) for mine, theirs in zip(authored, observed)]
    return observed


def match_by_key(expected: list, actual: list, key, equal=None) -> tuple[list[tuple[int, int | None]], list[int]]:
    """Pairs each expected item with an actual item of the same ``key(item)``.

    Returns ([(expected index, actual index or None)], unmatched actual indices). Among actual items of the same
    key, one that is ``equal`` to the expected item is preferred (two notifies at the same time and class are told
    apart by their properties); otherwise they pair in order."""
    pairs: list[tuple[int, int | None]] = []
    taken: set[int] = set()
    keys = [key(item) for item in actual]
    for index, item in enumerate(expected):
        wanted = key(item)
        candidates = [other for other, value in enumerate(keys) if other not in taken and value == wanted]
        chosen = None
        if equal is not None:
            chosen = next((other for other in candidates if equal(item, actual[other])), None)
        if chosen is None and candidates:
            chosen = candidates[0]
        if chosen is not None:
            taken.add(chosen)
        pairs.append((index, chosen))
    return pairs, [index for index in range(len(actual)) if index not in taken]


def _list_key(items: list) -> str | None:
    """The identity field a list of objects is keyed by (Name, Id, Key), when every item has a distinct one."""
    if not items or not all(type(item) is dict for item in items):
        return None
    for field in ("Name", "Id", "Key"):
        values = [item.get(field) for item in items]
        if all(type(value) is str and value for value in values) and len({value.casefold() for value in values}) == len(values):
            return field
    return None


def _escape(segment: object) -> str:
    return str(segment).replace("~", "~0").replace("/", "~1")


class _Absent:
    def __repr__(self) -> str:
        return "ABSENT"


ABSENT = _Absent()


def first_difference(path: str, expected: object, actual: object, equal=same_value) -> tuple[str, object, object]:
    """The innermost place two values really differ: the first member the expected object states that the
    actual one lacks (``ABSENT``) or holds unequal (members only the actual object has are left aside: a document
    states some members); in a list of objects keyed by Name / Id / Key the item of the same key (the path keeps
    the expected item's index); in other lists of the same length the first unequal item. Stops where the values
    differ as a whole."""
    while True:
        if type(expected) is dict and type(actual) is dict:
            member = next((key for key in sorted(expected, key=str)
                           if key not in actual or not equal(expected[key], actual[key])), None)
            if member is None:
                return path, expected, actual
            if member not in actual:
                return f"{path}/{_escape(member)}", expected[member], ABSENT
            path, expected, actual = f"{path}/{_escape(member)}", expected[member], actual[member]
        elif type(expected) is list and type(actual) is list:
            field = _list_key(expected)
            if field is not None and _list_key(actual) == field:
                by_key = {item[field].casefold(): item for item in actual}
                index = next((index for index, item in enumerate(expected)
                              if item[field].casefold() not in by_key or not equal(item, by_key[item[field].casefold()])), None)
                if index is None:
                    return path, expected, actual
                other = by_key.get(expected[index][field].casefold())
                if other is None:
                    return f"{path}/{index}", expected[index], ABSENT
                path, expected, actual = f"{path}/{index}", expected[index], other
            elif len(expected) == len(actual):
                index = next((index for index, (a, b) in enumerate(zip(expected, actual)) if not equal(a, b)), None)
                if index is None:
                    return path, expected, actual
                path, expected, actual = f"{path}/{index}", expected[index], actual[index]
            else:
                return path, expected, actual
        else:
            return path, expected, actual


def describe(value: object, limit: int = 4000) -> str:
    """Compact JSON of a value for a diagnostic: complete up to ``limit`` characters (the relevant element's
    values are what the author needs to copy), then cut with a note of the full length."""
    if value is ABSENT:
        return "(absent)"
    try:
        text = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    except (TypeError, ValueError):
        text = repr(value)
    if len(text) <= limit:
        return text
    return f"{text[:limit]}... ({len(text)} characters)"
