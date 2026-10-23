# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.dto import JsonValue
from asset_document.diff.model import Difference, DiffKind


def _escape_pointer(segment: object) -> str:
    return str(segment).replace("~", "~0").replace("/", "~1")


def _child_path(parent: str, segment: object) -> str:
    child = _escape_pointer(segment)
    return f"{parent}/{child}" if parent else f"/{child}"


def _same_scalar_type_and_value(left: JsonValue, right: JsonValue) -> bool:
    if type(left) is not type(right):
        return False
    if type(left) is float:
        return left.hex() == right.hex()
    return left == right


def _canonical_equal_validated(left: JsonValue, right: JsonValue) -> bool:
    if type(left) is not type(right):
        return False
    if type(left) is list:
        if len(left) != len(right):
            return False
        return all(
            _canonical_equal_validated(left_item, right_item)
            for left_item, right_item in zip(left, right)
        )
    if type(left) is dict:
        if left.keys() != right.keys():
            return False
        return all(
            _canonical_equal_validated(left[key], right[key]) for key in left
        )
    return _same_scalar_type_and_value(left, right)


def canonical_equal(left: JsonValue, right: JsonValue) -> bool:
    codec = DocumentCodec()
    codec.validate(left)
    codec.validate(right)
    return _canonical_equal_validated(left, right)


def _append_difference(
    output: list[Difference],
    path: str,
    kind: DiffKind,
    before_present: bool,
    before: JsonValue | None,
    after_present: bool,
    after: JsonValue | None,
) -> None:
    output.append(
        Difference(path, kind, before_present, before, after_present, after)
    )


def _diff_mapping(
    before: dict[str, JsonValue],
    after: dict[str, JsonValue],
    path: str,
    output: list[Difference],
) -> None:
    for key in sorted(before.keys() | after.keys()):
        before_present = key in before
        after_present = key in after
        _diff_node(
            before_present,
            before.get(key),
            after_present,
            after.get(key),
            _child_path(path, key),
            output,
        )


def _diff_array_common(
    before: list[JsonValue],
    after: list[JsonValue],
    path: str,
    output: list[Difference],
) -> int:
    common = min(len(before), len(after))
    for index in range(common):
        _diff_node(
            True, before[index], True, after[index], _child_path(path, index), output
        )
    return common


def _diff_array(
    before: list[JsonValue],
    after: list[JsonValue],
    path: str,
    output: list[Difference],
) -> None:
    common = _diff_array_common(before, after, path, output)
    for index in range(common, len(before)):
        _append_difference(
            output,
            _child_path(path, index),
            DiffKind.REMOVE,
            True,
            before[index],
            False,
            None,
        )
    for index in range(common, len(after)):
        _append_difference(
            output,
            _child_path(path, index),
            DiffKind.ADD,
            False,
            None,
            True,
            after[index],
        )


def _diff_node(
    before_present: bool,
    before: JsonValue | None,
    after_present: bool,
    after: JsonValue | None,
    path: str,
    output: list[Difference],
) -> None:
    if not before_present:
        _append_difference(output, path, DiffKind.ADD, False, None, True, after)
        return
    if not after_present:
        _append_difference(output, path, DiffKind.REMOVE, True, before, False, None)
        return
    if _canonical_equal_validated(before, after):
        return
    if type(before) is dict and type(after) is dict:
        _diff_mapping(before, after, path, output)
        return
    if type(before) is list and type(after) is list:
        _diff_array(before, after, path, output)
        return
    _append_difference(output, path, DiffKind.REPLACE, True, before, True, after)


def canonical_diff(before: JsonValue, after: JsonValue) -> tuple[Difference, ...]:
    codec = DocumentCodec()
    codec.validate(before)
    codec.validate(after)
    output: list[Difference] = []
    _diff_node(True, before, True, after, "", output)
    return tuple(output)
