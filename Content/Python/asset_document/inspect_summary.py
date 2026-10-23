# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""The summarized inspect: one shape for every profile with large inspect details.

``inspect_asset_document_target`` over HTTP asks for a summary unless the request passes ``detail: true``
(``runtime.summarized`` calls the profile's optional ``summarize_inspect``). A summary keeps the facts an AI builds on,
gives the document's ``Body`` as an outline (``extract_asset_document`` returns it whole), gives large reflected
``properties`` as their names, types and small values (``skipped`` grouped by reason) and says what it left out in one
top-level ``Summary`` line.
"""
from __future__ import annotations

import json

SUMMARY_KEY = "Summary"
# A value at most this long (compact JSON) stays whole in an outline.
_SMALL = 160
# Names listed per collection in an outline.
_NAMES = 40
# A Body at most this long (compact JSON) stays whole in a summary; so do properties and skipped together.
_SMALL_BODY = 4096
# A property value at most this long (compact JSON) stays in a summarized property row.
_SMALL_VALUE = 120


def _size(value):
    return len(json.dumps(value, separators=(",", ":"), ensure_ascii=False))


def _count(items):
    return f"{len(items)} entries" if len(items) != 1 else "1 entry"


def _name(entry):
    """An entry's identity for an outline: Name (or Key / Id), with its Type when it has one ("Bone(root)")."""
    for key in ("Name", "Key", "Id"):
        if type(entry.get(key)) is str:
            kind = entry.get("Type") if key == "Name" and type(entry.get("Type")) is str else None
            return f"{kind}({entry[key]})" if kind and len(kind) < 24 else entry[key]
    return None


def _listed(names):
    listed = names[:_NAMES]
    return listed + [f"+{len(names) - len(listed)} more"] if len(names) > len(listed) else listed


def outline(value, depth=2):
    """A value as an outline: small values whole; a list of named entries as their names; down to ``depth`` levels an
    object by key (one of many entries as its keys); below that, a collection as its entry count."""
    if type(value) not in (dict, list) or _size(value) <= _SMALL:
        return value
    if type(value) is list:
        names = [_name(item) if type(item) is dict else None for item in value]
        return _listed(names) if value and all(type(name) is str for name in names) else _count(value)
    if depth <= 0:
        return _count(value)
    if len(value) > _NAMES:
        return _listed(list(value))
    return {key: outline(item, depth - 1) for key, item in value.items()}


def _value_size(value):
    if type(value) is dict:
        return f"{len(value)} fields" if len(value) != 1 else "1 field"
    if type(value) is list:
        return _count(value)
    return f"{_size(value)} chars"


def _property_row(row, live):
    """A property row as its name, type and small value. A class's defaults: ``default_value``. A live asset:
    ``current_value``, and ``overridden`` with the class's ``default_value`` when they differ. A large value is left
    out and its ``size`` given (an instanced object's ``class`` too); ``writable`` is given only when false."""
    if type(row) is not dict or not {"name", "type_token", "current_value", "default_value"} <= set(row):
        return row
    out = {"name": row["name"], "type_token": row["type_token"]}
    key = "current_value" if live else "default_value"
    value = row[key]
    if _size(value) <= _SMALL_VALUE:
        out[key] = value
    elif type(value) is dict and type(value.get("Class")) is str and type(value.get("Properties")) is dict:
        # An instanced object: its class, and how many of its properties the value sets.
        out["class"] = value["Class"]
        count = len(value["Properties"])
        out["size"] = f"{count} properties" if count != 1 else "1 property"
    else:
        out["size"] = _value_size(value)
    if live and row["current_value"] != row["default_value"]:
        out["overridden"] = True
        if _size(row["default_value"]) <= _SMALL_VALUE:
            out["default_value"] = row["default_value"]
    if row.get("writable") is False:
        out["writable"] = False
    return out


def _skipped_by_reason(rows):
    grouped = {}
    for row in rows:
        if type(row) is not dict or type(row.get("name")) is not str or type(row.get("reason")) is not str:
            return rows
        grouped.setdefault(row["reason"], []).append(row["name"])
    return {reason: grouped[reason] for reason in sorted(grouped)}


def summarize_properties(inspected):
    """``properties`` as names, types and small values and ``skipped`` grouped by reason, when together they are
    large; the words for the Summary line, or None when they stay whole."""
    properties, skipped = inspected.get("properties"), inspected.get("skipped")
    if type(properties) is not list or _size(properties) + _size(skipped) <= _SMALL_BODY:
        return None
    live = "asset_path" in inspected
    inspected["properties"] = [_property_row(row, live) for row in properties]
    if type(skipped) is list:
        inspected["skipped"] = _skipped_by_reason(skipped)
    if live:
        return ("properties as name, type_token and current_value (size instead of a large value such as a nested "
                "struct; overridden and default_value when it differs from the class default), skipped grouped by reason")
    return ("properties as name, type_token and default_value (size instead of a large value such as a nested struct), "
            "skipped grouped by reason")


def note(left_out):
    """The Summary line: what the summary left out, and how to get the whole."""
    return f"Summarized: {left_out}. Inspect with detail true for the whole."


def summarize(inspected, left_out="", *, facts=None, drop=()):
    """The inspect summarized: a large ``Body`` as an outline, large ``properties`` as names, types and small values
    (``summarize_properties``), each ``facts`` key replaced by its function's result, the ``drop`` keys left out, and
    the Summary line naming what was summarized (``left_out`` adds a profile's own
    words). An inspect with nothing to summarize is returned whole, without a Summary."""
    if type(inspected) is not dict:
        return inspected
    out = dict(inspected)
    parts = []
    dropped = [key for key in drop if out.pop(key, None) is not None]
    if dropped:
        parts.append(f"{', '.join(dropped)} left out")
    if type(out.get("Body")) is dict and _size(out["Body"]) > _SMALL_BODY:
        out["Body"] = {key: outline(item) for key, item in out["Body"].items()}
        parts.append("Body as an outline (extract_asset_document returns the document)")
    properties = summarize_properties(out)
    if properties:
        parts.append(properties)
    for key, summarize_fact in (facts or {}).items():
        if key in out:
            out[key] = summarize_fact(out[key])
    if left_out:
        parts.append(left_out)
    if not parts:
        return inspected
    out[SUMMARY_KEY] = note("; ".join(parts))
    return out
