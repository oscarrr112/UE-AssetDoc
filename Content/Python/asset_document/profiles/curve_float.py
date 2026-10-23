# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""CurveFloat authored from Python only.

``UCurveFloat`` keeps its curve in ``FloatCurve`` (``FRichCurve``) and
``bIsEventCurve``; both are serialized ``UPROPERTY()`` without ``CPF_Edit``, so
this component declares them through the generic property admission. The
Editor provider writes them with the editor-equivalent Modify/PreEditChange/
PostEditChange sequence; nothing here is CurveFloat-specific in C++.

Document form of ``FloatCurve`` (what Extract returns and Apply accepts)::

    {
      "Keys": [
        {"Time": 0.0, "Value": 1.0, "InterpMode": "Cubic", "TangentMode": "User",
         "TangentWeightMode": "None", "ArriveTangent": 0.0, "ArriveTangentWeight": 0.0,
         "LeaveTangent": 0.5, "LeaveTangentWeight": 0.0}
      ],
      "PreInfinityExtrap": "Constant",
      "PostInfinityExtrap": "Cycle",
      "DefaultValue": 2.0
    }

Only ``Time`` and ``Value`` are required per key; omitted key fields take the
engine defaults (Linear, Auto, None, zero tangents/weights). Enum values are the
engine names without their ``RCIM_``/``RCTM_``/``RCTWM_Weighted``/``RCCE_``
prefix. ``DefaultValue`` is omitted while the curve has none. ``FloatCurve`` is
authored whole: omitted curve fields reset to their defaults. Numbers are
float32 in the asset; extracted numbers are the shortest decimals that round-trip
to the stored float32.
"""
from __future__ import annotations

import math
import struct

from asset_document.diagnostics import contract_error
from asset_document.dto import JsonValue
from asset_document.regions.property_semantics import (
    GenericPropertyComponent,
    admit_properties,
)

CURVE_FLOAT_CLASS = "/Script/Engine.CurveFloat"

_FLOAT32_MAX = struct.unpack("<f", struct.pack("<f", 3.4028234663852886e38))[0]

# Document name -> native (authored) enum name.
_INTERP_MODES = {"Linear": "RCIM_Linear", "Constant": "RCIM_Constant", "Cubic": "RCIM_Cubic", "None": "RCIM_None"}
_TANGENT_MODES = {
    "Auto": "RCTM_Auto",
    "User": "RCTM_User",
    "Break": "RCTM_Break",
    "None": "RCTM_None",
    "SmartAuto": "RCTM_SmartAuto",
}
_TANGENT_WEIGHT_MODES = {
    "None": "RCTWM_WeightedNone",
    "Arrive": "RCTWM_WeightedArrive",
    "Leave": "RCTWM_WeightedLeave",
    "Both": "RCTWM_WeightedBoth",
}
_EXTRAPOLATIONS = {
    "Cycle": "RCCE_Cycle",
    "CycleWithOffset": "RCCE_CycleWithOffset",
    "Oscillate": "RCCE_Oscillate",
    "Linear": "RCCE_Linear",
    "Constant": "RCCE_Constant",
    "None": "RCCE_None",
}

_KEY_ENUMS = (
    ("InterpMode", _INTERP_MODES, "Linear"),
    ("TangentMode", _TANGENT_MODES, "Auto"),
    ("TangentWeightMode", _TANGENT_WEIGHT_MODES, "None"),
)
_KEY_NUMBERS = ("ArriveTangent", "ArriveTangentWeight", "LeaveTangent", "LeaveTangentWeight")
_KEY_FIELDS = frozenset(("Time", "Value", *(name for name, _, _ in _KEY_ENUMS), *_KEY_NUMBERS))
_CURVE_FIELDS = frozenset(("Keys", "PreInfinityExtrap", "PostInfinityExtrap", "DefaultValue"))
_NATIVE_CURVE_FIELDS = _CURVE_FIELDS


def _fail(path: str, code: str, message: str) -> None:
    raise contract_error(path, code, message)


def _float32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def _number(value: object, path: str) -> float:
    """A finite JSON number as the float32 it is stored as, written as the shortest
    decimal of that float (the form the Editor codec reads floats back in)."""
    if type(value) not in (int, float) or not math.isfinite(value):
        _fail(path, "CurveFloat.InvalidNumber", "curve numbers must be finite JSON numbers")
    try:
        stored = _float32(float(value))
    except OverflowError:
        _fail(path, "CurveFloat.InvalidNumber", "curve numbers must fit float32")
    return _shortest(stored, path)


def _shortest(value: object, path: str) -> float:
    """The shortest decimal that round-trips to the same float32."""
    if type(value) is not float or not math.isfinite(value):
        _fail(path, "CurveFloat.InvalidNativeValue", "native curve numbers must be finite floats")
    stored = _float32(value)
    for digits in range(1, 10):
        candidate = float(f"{stored:.{digits}g}")
        try:
            if _float32(candidate) == stored:
                return candidate
        except OverflowError:  # a short spelling of a value near float max rounds up past it
            continue
    return stored


def _enum_from_document(value: object, names: dict[str, str], path: str) -> str:
    if type(value) is not str or value not in names:
        _fail(path, "CurveFloat.UnknownEnumName", f"expected one of {', '.join(names)}")
    return names[value]


def _enum_to_document(value: object, names: dict[str, str], path: str) -> str:
    for document, native in names.items():
        if value == native:
            return document
    _fail(path, "CurveFloat.InvalidNativeValue", "native curve enum is not a known name")


def _object(value: object, fields: frozenset[str], path: str, code: str) -> dict:
    if type(value) is not dict:
        _fail(path, code, "expected a JSON object")
    unknown = sorted(set(value) - fields)
    if unknown:
        _fail(f"{path}/{unknown[0]}", "CurveFloat.UnknownField", f"unknown field; allowed: {', '.join(sorted(fields))}")
    return value


def _key_from_document(value: object, path: str) -> dict[str, JsonValue]:
    key = _object(value, _KEY_FIELDS, path, "CurveFloat.InvalidKey")
    for required in ("Time", "Value"):
        if required not in key:
            _fail(f"{path}/{required}", "CurveFloat.InvalidKey", "every key requires Time and Value")
    native: dict[str, JsonValue] = {
        name: _enum_from_document(key.get(name, default), names, f"{path}/{name}")
        for name, names, default in _KEY_ENUMS
    }
    native["Time"] = _number(key["Time"], f"{path}/Time")
    native["Value"] = _number(key["Value"], f"{path}/Value")
    for name in _KEY_NUMBERS:
        native[name] = _number(key.get(name, 0.0), f"{path}/{name}")
    return native


def _key_to_document(value: object, path: str) -> dict[str, JsonValue]:
    if type(value) is not dict:
        _fail(path, "CurveFloat.InvalidNativeValue", "native curve key must be an object")
    document: dict[str, JsonValue] = {
        "Time": _shortest(value.get("Time"), f"{path}/Time"),
        "Value": _shortest(value.get("Value"), f"{path}/Value"),
    }
    for name, names, _ in _KEY_ENUMS:
        document[name] = _enum_to_document(value.get(name), names, f"{path}/{name}")
    for name in _KEY_NUMBERS:
        document[name] = _shortest(value.get(name), f"{path}/{name}")
    return document


class RichCurveCodec:
    """``FRichCurve`` <-> readable key list (see the module docstring)."""

    def from_document(self, value: JsonValue, path: str) -> JsonValue:
        curve = _object(value, _CURVE_FIELDS, path, "CurveFloat.InvalidCurve")
        keys = curve.get("Keys", [])
        if type(keys) not in (list, tuple):
            _fail(f"{path}/Keys", "CurveFloat.InvalidCurve", "Keys must be an array of keys")
        native_keys = [_key_from_document(key, f"{path}/Keys/{index}") for index, key in enumerate(keys)]
        for index in range(1, len(native_keys)):
            if native_keys[index]["Time"] <= native_keys[index - 1]["Time"]:
                _fail(
                    f"{path}/Keys/{index}/Time",
                    "CurveFloat.KeysNotAscending",
                    "key times must be strictly ascending (after float32 rounding)",
                )
        default_value = curve.get("DefaultValue")
        return {
            "DefaultValue": _number(_FLOAT32_MAX if default_value is None else default_value, f"{path}/DefaultValue"),
            "PreInfinityExtrap": _enum_from_document(
                curve.get("PreInfinityExtrap", "Constant"), _EXTRAPOLATIONS, f"{path}/PreInfinityExtrap"),
            "PostInfinityExtrap": _enum_from_document(
                curve.get("PostInfinityExtrap", "Constant"), _EXTRAPOLATIONS, f"{path}/PostInfinityExtrap"),
            "Keys": native_keys,
        }

    def to_document(self, value: JsonValue, path: str) -> JsonValue:
        if type(value) is not dict or set(value) != _NATIVE_CURVE_FIELDS:
            _fail(path, "CurveFloat.InvalidNativeValue", "native FRichCurve has an unexpected shape")
        keys = value["Keys"]
        if type(keys) not in (list, tuple):
            _fail(f"{path}/Keys", "CurveFloat.InvalidNativeValue", "native keys must be an array")
        document: dict[str, JsonValue] = {
            "Keys": [_key_to_document(key, f"{path}/Keys/{index}") for index, key in enumerate(keys)],
            "PreInfinityExtrap": _enum_to_document(value["PreInfinityExtrap"], _EXTRAPOLATIONS, f"{path}/PreInfinityExtrap"),
            "PostInfinityExtrap": _enum_to_document(value["PostInfinityExtrap"], _EXTRAPOLATIONS, f"{path}/PostInfinityExtrap"),
        }
        default_value = value["DefaultValue"]
        if type(default_value) is not float or _float32(default_value) != _FLOAT32_MAX:
            document["DefaultValue"] = _shortest(default_value, f"{path}/DefaultValue")
        return document


CURVE_FLOAT_PROPERTIES = GenericPropertyComponent(
    admit_properties(CURVE_FLOAT_CLASS, ("FloatCurve", "bIsEventCurve")),
    {"FloatCurve": RichCurveCodec()},
)
