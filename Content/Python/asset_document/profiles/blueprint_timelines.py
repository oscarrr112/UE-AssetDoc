# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Timelines of an actor-based Blueprint (UBlueprint), pure.

Body region::

    "Timelines": [
      {"Name": "Fade", "Length": 2.0, "AutoPlay": true, "Loop": true, "TickGroup": "PostPhysics",
       "Tracks": [
         {"Name": "Alpha", "Type": "Float", "Keys": [{"Time": 0.0, "Value": 0.0}, {"Time": 2.0, "Value": 1.0}]},
         {"Name": "Offset", "Type": "Vector", "Keys": {"Z": [{"Time": 0.0}, {"Time": 2.0, "Value": 50.0}]}},
         {"Name": "Tint", "Type": "LinearColor", "Curve": "/Game/Curves/C_Tint"},
         {"Name": "Halfway", "Type": "Event", "Keys": [{"Time": 1.0}]}]}
    ]

A timeline is the editor's timeline node plus its UTimelineTemplate;
``blueprint.member.timeline.set.v1`` / ``.remove.v1`` create and remove both, like the editor.
A listed row is the whole timeline: omitted settings take the editor defaults (Length 5,
UseLastKeyframe / AutoPlay / Loop / Replicated / IgnoreTimeDilation false, TickGroup
PrePhysics), omitted Tracks mean none, and the tracks are listed in display order.  Track
Type is Float, Vector, LinearColor or Event.  A track holds its own keys (``Keys``: one key
list, or per channel X/Y/Z resp. R/G/B/A; an omitted channel has no keys) or uses an external
curve asset (``Curve``).  Keys use the CurveFloat key fields (Time, Value, InterpMode,
TangentMode, TangentWeightMode, Arrive/LeaveTangent, Arrive/LeaveTangentWeight; only Time is
required).  The tangents of Cubic keys in Auto / SmartAuto tangent mode are derived by the
curve editor and are neither extracted nor compared; the editor's tangent clean-up around
Constant keys (FRichCurve::AutoSetTangents) is applied to the authored keys.  Numbers are
float32 in the asset.

An omitted region is preserved; a present region is the complete list.  The timeline node of a
new timeline is placed where the document's UbergraphPages put the ``K2:Timeline`` node of
that Name (else to the right of the first event graph); when both regions are given they must
name the same timelines.  Timelines exist only in actor-based Blueprints with an event graph
(FBlueprintEditorUtils::DoesSupportTimelines).
"""
from __future__ import annotations

import math
import struct

from asset_document.diagnostics import contract_error
from asset_document.diff.model import Difference, DiffKind
from asset_document.graphs.actions import action_kind

KEY = "Timelines"
PRIMITIVES = ("timeline.set", "timeline.remove")
DEFAULT_LENGTH = 5.0
DEFAULT_TICK_GROUP = "PrePhysics"
TICK_GROUPS = ("PrePhysics", "DuringPhysics", "PostPhysics", "PostUpdateWork")
# Document track type -> (primitive type, channel names; "" is the single channel).
TRACK_TYPES = {"Float": ("float", ("",)), "Vector": ("vector", ("X", "Y", "Z")),
               "LinearColor": ("linear_color", ("R", "G", "B", "A")), "Event": ("event", ("",))}
_TRACK_BY_DTO = {dto: name for name, (dto, _) in TRACK_TYPES.items()}
_FLAGS = {"UseLastKeyframe": "use_last_keyframe", "AutoPlay": "auto_play", "Loop": "loop",
          "Replicated": "replicated", "IgnoreTimeDilation": "ignore_time_dilation"}
_FIELDS = ("Name", "Length", *_FLAGS, "TickGroup", "Tracks")
_TRACK_FIELDS = ("Name", "Type", "Keys", "Curve")
# Document key enum -> (primitive field, engine prefix, document values, default).
_KEY_ENUMS = (
    ("InterpMode", "interp_mode", "RCIM_", ("Linear", "Constant", "Cubic"), "Linear"),
    ("TangentMode", "tangent_mode", "RCTM_", ("Auto", "User", "Break", "SmartAuto"), "Auto"),
    ("TangentWeightMode", "tangent_weight_mode", "RCTWM_Weighted", ("None", "Arrive", "Leave", "Both"), "None"),
)
_KEY_NUMBERS = {"Value": "value", "ArriveTangent": "arrive_tangent", "ArriveTangentWeight": "arrive_tangent_weight",
                "LeaveTangent": "leave_tangent", "LeaveTangentWeight": "leave_tangent_weight"}
_TANGENTS = ("ArriveTangent", "LeaveTangent")
_KEY_FIELDS = ("Time", *(name for name, *_ in _KEY_ENUMS), *_KEY_NUMBERS)
_FLOAT32_MAX = 3.4028234663852886e38


def _error(path, message, code="UBlueprint.InvalidBodyRegion"):
    return contract_error(path, code, message)


# ---------------------------------------------------------------------------
# float32 numbers
# ---------------------------------------------------------------------------
def _float32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def _shortest(value: float) -> float:
    """The shortest decimal that round-trips to the same float32 (as the extract writes it)."""
    stored = _float32(value)
    for digits in range(1, 10):
        candidate = float(f"{stored:.{digits}g}")
        if _float32(candidate) == stored:
            return candidate
    return stored


def _number(value: object, path: str) -> float:
    if type(value) not in (int, float) or not math.isfinite(value) or abs(value) > _FLOAT32_MAX:
        raise _error(path, "Expected a finite number that fits float32.", "UBlueprint.InvalidValue")
    return _shortest(float(value))


def _enum(value: object, prefix: str, names: tuple, path: str) -> str:
    if type(value) is str and value.startswith(prefix):
        value = value[len(prefix):]
    if value not in names:
        raise _error(path, f"Expected one of {', '.join(names)}.", "UBlueprint.InvalidValue")
    return value


# ---------------------------------------------------------------------------
# Keys
# ---------------------------------------------------------------------------
def _derived_tangents(key: dict) -> bool:
    """Cubic keys in an auto tangent mode: the curve editor computes their tangents."""
    return key.get("InterpMode") == "Cubic" and key.get("TangentMode", "Auto") in ("Auto", "SmartAuto")


def _normalize_keys(keys: list) -> list:
    """FRichCurve::AutoSetTangents on non-derived keys, then drop derived tangents and defaults."""
    full = [{"InterpMode": "Linear", "TangentMode": "Auto", **key} for key in keys]
    last = len(full) - 1
    for index, key in enumerate(full):
        if _derived_tangents(key):
            continue
        if index == 0:
            if last > 0 and key["TangentMode"] == "Auto":
                key["LeaveTangent"] = 0.0
        elif index < last and ("Constant" in (full[index - 1]["InterpMode"], key["InterpMode"])):
            key["LeaveTangent"] = 0.0
            if full[index - 1]["InterpMode"] != "Cubic":
                key["ArriveTangent"] = 0.0
    out = []
    for key in full:
        row = {"Time": key["Time"]}
        for name, _, _, _, default in _KEY_ENUMS:
            if key.get(name, default) != default:
                row[name] = key[name]
        for name in _KEY_NUMBERS:
            if name in _TANGENTS and _derived_tangents(key):
                continue
            if key.get(name, 0.0) != 0.0:
                row[name] = key[name]
        out.append(row)
    return out


def _canonical_keys(value: object, path: str) -> list:
    if type(value) is not list:
        raise _error(path, "Keys is an array of {\"Time\", \"Value\", ...}.")
    keys = []
    for index, item in enumerate(value):
        item_path = f"{path}/{index}"
        if type(item) is not dict:
            raise _error(item_path, "A key is an object with Time (and Value, InterpMode, ...).")
        unknown = sorted(set(item) - set(_KEY_FIELDS))
        if unknown:
            raise _error(f"{item_path}/{unknown[0]}", f"Unknown key field '{unknown[0]}'. Fields: {', '.join(_KEY_FIELDS)}.",
                         "UBlueprint.UnknownField")
        if "Time" not in item:
            raise _error(f"{item_path}/Time", "Every key has a Time.")
        key = {"Time": _number(item["Time"], f"{item_path}/Time")}
        if keys and key["Time"] <= keys[-1]["Time"]:
            raise _error(f"{item_path}/Time", "Key times must be strictly ascending.", "UBlueprint.InvalidValue")
        for name, _, prefix, names, _ in _KEY_ENUMS:
            if name in item:
                key[name] = _enum(item[name], prefix, names, f"{item_path}/{name}")
        for name in _KEY_NUMBERS:
            if name in item:
                key[name] = _number(item[name], f"{item_path}/{name}")
        keys.append(key)
    return _normalize_keys(keys)


def _key_dto(key: dict) -> dict:
    dto = {"time": key["Time"]}
    for name, field, prefix, _, default in _KEY_ENUMS:
        dto[field] = prefix + key.get(name, default)
    for name, field in _KEY_NUMBERS.items():
        dto[field] = key.get(name, 0.0)
    return dto


def _key_from_dto(dto: dict) -> dict:
    key = {"Time": _shortest(dto["time"])}
    for name, field, prefix, _, default in _KEY_ENUMS:
        value = dto.get(field, prefix + default)
        key[name] = value[len(prefix):] if value.startswith(prefix) else value
    for name, field in _KEY_NUMBERS.items():
        key[name] = _shortest(dto.get(field, 0.0))
    return key


# ---------------------------------------------------------------------------
# Canonical rows
# ---------------------------------------------------------------------------
def _canonical_track(value: object, path: str) -> dict:
    if type(value) is not dict:
        raise _error(path, "A track is an object with Name and Type (and Keys or Curve).")
    unknown = sorted(set(value) - set(_TRACK_FIELDS))
    if unknown:
        raise _error(f"{path}/{unknown[0]}", f"Unknown track field '{unknown[0]}'. Fields: {', '.join(_TRACK_FIELDS)}.",
                     "UBlueprint.UnknownField")
    name = value.get("Name")
    if type(name) is not str or not name.strip():
        raise _error(f"{path}/Name", "Track Name must be a non-empty string.")
    kind = value.get("Type")
    if kind not in TRACK_TYPES:
        raise _error(f"{path}/Type", f"Track Type is one of {', '.join(TRACK_TYPES)}.", "UBlueprint.InvalidValue")
    out = {"Name": name, "Type": kind}
    if "Curve" in value:
        if "Keys" in value:
            raise _error(f"{path}/Keys", "A track uses an external Curve or its own Keys, not both.",
                         "BlueprintMember.InvalidTrack")
        curve = value["Curve"]
        if type(curve) is not str or not curve.startswith("/"):
            raise _error(f"{path}/Curve", "Curve is a curve asset path such as \"/Game/Curves/C_Fade\".")
        out["Curve"] = _object_path(curve)
        return out
    channels = TRACK_TYPES[kind][1]
    if "Keys" not in value:
        return out
    keys = value["Keys"]
    if channels == ("",):
        canonical = _canonical_keys(keys, f"{path}/Keys")
        if canonical:
            out["Keys"] = canonical
        return out
    if type(keys) is not dict:
        raise _error(f"{path}/Keys", f"{kind} keys are given per channel: {{{', '.join(channels)}}}.")
    unknown = sorted(set(keys) - set(channels))
    if unknown:
        raise _error(f"{path}/Keys/{unknown[0]}", f"{kind} channels are {', '.join(channels)}.", "UBlueprint.UnknownField")
    per_channel = {channel: _canonical_keys(keys[channel], f"{path}/Keys/{channel}")
                   for channel in channels if channel in keys}
    per_channel = {channel: rows for channel, rows in per_channel.items() if rows}
    if per_channel:
        out["Keys"] = per_channel
    return out


def _object_path(value: str) -> str:
    leaf = value.rsplit("/", 1)[-1]
    return value if "." in leaf else f"{value}.{leaf}"


def _canonical_row(value: object, path: str) -> dict:
    if type(value) is not dict:
        raise _error(path, "A timeline is an object with Name (and Length, flags, TickGroup, Tracks).")
    unknown = sorted(set(value) - set(_FIELDS))
    if unknown:
        raise _error(f"{path}/{unknown[0]}", f"Unknown timeline field '{unknown[0]}'. Fields: {', '.join(_FIELDS)}.",
                     "UBlueprint.UnknownField")
    name = value.get("Name")
    if type(name) is not str or not name.strip():
        raise _error(f"{path}/Name", "Timeline Name must be a non-empty string.")
    out = {"Name": name}
    if "Length" in value:
        length = _number(value["Length"], f"{path}/Length")
        if not length > 1e-4:
            raise _error(f"{path}/Length", "Length must be greater than 0.", "UBlueprint.InvalidValue")
        if length != DEFAULT_LENGTH:
            out["Length"] = length
    for key in _FLAGS:
        if key in value:
            if type(value[key]) is not bool:
                raise _error(f"{path}/{key}", f"{key} must be true or false.")
            if value[key]:
                out[key] = True
    if "TickGroup" in value:
        group = _enum(value["TickGroup"], "TG_", TICK_GROUPS, f"{path}/TickGroup")
        if group != DEFAULT_TICK_GROUP:
            out["TickGroup"] = group
    if "Tracks" in value:
        if type(value["Tracks"]) is not list:
            raise _error(f"{path}/Tracks", "Tracks must be an array.")
        tracks = [_canonical_track(item, f"{path}/Tracks/{index}") for index, item in enumerate(value["Tracks"])]
        _unique(tracks, f"{path}/Tracks")
        if tracks:
            out["Tracks"] = tracks
    return out


def _unique(rows: list, path: str) -> None:
    seen = {}
    for index, row in enumerate(rows):
        token = row["Name"].casefold()
        if token in seen:
            raise _error(f"{path}/{index}/Name", f"'{row['Name']}' is listed twice (also at index {seen[token]}).",
                         "UBlueprint.DuplicateName")
        seen[token] = index


def canonical(body: dict) -> list | None:
    """The canonical Timelines rows of a Body (``None`` when the region is omitted)."""
    if body.get(KEY) is None:
        return None
    if type(body[KEY]) is not list:
        raise _error(f"/Body/{KEY}", "This region must be an array.")
    rows = [_canonical_row(item, f"/Body/{KEY}/{index}") for index, item in enumerate(body[KEY])]
    _unique(rows, f"/Body/{KEY}")
    return rows


def timeline_nodes(graphs) -> dict:
    """``{name.casefold(): (graph name, position, json path)}`` of the K2:Timeline nodes in decoded event graphs."""
    out = {}
    for graph in graphs:
        if graph.role != "ubergraph":
            continue
        for sub in graph.walk():
            for node in sub.nodes:
                if node.action and action_kind(node.action) == "Timeline" and node.name:
                    out[node.name.casefold()] = (graph.name, node.position, sub.node_path(node.id))
    return out


def check_nodes(rows: list, graphs, ubergraph_listed: bool) -> None:
    """With both regions given, Timelines and the event graphs' timeline nodes name the same timelines."""
    if rows is None or not ubergraph_listed:
        return
    nodes = timeline_nodes(graphs)
    names = {row["Name"].casefold() for row in rows}
    for index, row in enumerate(rows):
        if row["Name"].casefold() not in nodes:
            raise _error(f"/Body/{KEY}/{index}/Name",
                         f"Timeline '{row['Name']}' has no K2:Timeline node named '{row['Name']}' in UbergraphPages "
                         "(a timeline and its node exist together).", "UBlueprint.TimelineNodeMismatch")
    for key, (_, _, path) in nodes.items():
        if key not in names:
            raise _error(path, f"This K2:Timeline node names a timeline that Body.{KEY} does not list.",
                         "UBlueprint.TimelineNodeMismatch")


# ---------------------------------------------------------------------------
# Live query -> Body
# ---------------------------------------------------------------------------
def _row_from_live(item: dict) -> dict:
    row = {"Name": item["name"]}
    length = _shortest(item.get("length", DEFAULT_LENGTH))
    if length != DEFAULT_LENGTH:
        row["Length"] = length
    for key, field in _FLAGS.items():
        if item.get(field):
            row[key] = True
    group = item.get("tick_group", "TG_" + DEFAULT_TICK_GROUP)
    group = group[3:] if group.startswith("TG_") else group
    if group != DEFAULT_TICK_GROUP:
        row["TickGroup"] = group
    tracks = []
    for track in item.get("tracks", []):
        kind = _TRACK_BY_DTO.get(track["type"], track["type"])
        out = {"Name": track["name"], "Type": kind}
        if track.get("curve"):
            out["Curve"] = track["curve"]
        else:
            channels = TRACK_TYPES.get(kind, ("float", ("",)))[1]
            keys = [_normalize_keys([_key_from_dto(key) for key in channel]) for channel in track.get("channels", [])]
            if channels == ("",):
                if keys and keys[0]:
                    out["Keys"] = keys[0]
            else:
                per_channel = {channel: rows for channel, rows in zip(channels, keys) if rows}
                if per_channel:
                    out["Keys"] = per_channel
        tracks.append(out)
    if tracks:
        row["Tracks"] = tracks
    return row


def from_query(query: dict) -> list:
    return [_row_from_live(item) for item in query.get("timelines", [])]


# ---------------------------------------------------------------------------
# Planning and comparison
# ---------------------------------------------------------------------------
def _tracks_dto(row: dict) -> list:
    out = []
    for track in row.get("Tracks", []):
        kind, channels = TRACK_TYPES[track["Type"]]
        dto = {"name": track["Name"], "type": kind}
        if "Curve" in track:
            dto["curve"] = track["Curve"]
        else:
            keys = track.get("Keys", [] if channels == ("",) else {})
            per_channel = [keys] if channels == ("",) else [keys.get(channel, []) for channel in channels]
            dto["channels"] = [[_key_dto(key) for key in channel] for channel in per_channel]
        out.append(dto)
    return out


def _settings(row: dict) -> dict:
    values = {"length": row.get("Length", DEFAULT_LENGTH), "tick_group": "TG_" + row.get("TickGroup", DEFAULT_TICK_GROUP)}
    for key, field in _FLAGS.items():
        values[field] = row.get(key, False)
    return values


def plan_removes(body: dict, query: dict) -> list:
    if body.get(KEY) is None:
        return []
    wanted = {row["Name"].casefold() for row in body[KEY]}
    return [("blueprint.member.timeline.remove.v1", {"name": item["name"]}, f"/Body/{KEY}")
            for item in query.get("timelines", []) if item["name"].casefold() not in wanted]


def plan_sets(body: dict, query: dict, nodes: dict | None = None) -> list:
    """Create / update listed timelines; ``nodes`` (:func:`timeline_nodes`) places new timeline nodes."""
    if body.get(KEY) is None:
        return []
    live = {row["Name"].casefold(): row for row in from_query(query)}
    ops = []
    for index, row in enumerate(body[KEY]):
        current = live.get(row["Name"].casefold())
        if current == row or current is not None and _rows_equal(row, current):
            continue
        values = {"name": row["Name"]}
        wanted = _settings(row)
        have = _settings(current) if current is not None else {
            "length": DEFAULT_LENGTH, "tick_group": "TG_" + DEFAULT_TICK_GROUP, **{field: False for field in _FLAGS.values()}}
        # Only changed settings: a live value the editor hides (e.g. a hidden tick group) stays untouched.
        values.update({field: value for field, value in wanted.items() if have[field] != value})
        if current is None or row.get("Tracks", []) != current.get("Tracks", []):
            values["tracks"] = _tracks_dto(row)
        if current is None and nodes and row["Name"].casefold() in nodes:
            graph, position, _ = nodes[row["Name"].casefold()]
            values["graph"] = graph
            if position is not None:
                values["position"] = {"x": position.x, "y": position.y}
        ops.append(("blueprint.member.timeline.set.v1", values, f"/Body/{KEY}/{index}"))
    return ops


def _rows_equal(authored: dict, live: dict) -> bool:
    return {**authored, "Name": authored["Name"].casefold()} == {**live, "Name": live["Name"].casefold()}


def rows_match(authored: list, live: list) -> bool:
    have = {row["Name"].casefold(): row for row in live}
    if set(have) != {row["Name"].casefold() for row in authored}:
        return False
    # A case-only rename is a change (blueprint.member.rename.v1).
    return all(row["Name"] == have[row["Name"].casefold()]["Name"] and _rows_equal(row, have[row["Name"].casefold()])
               for row in authored)


def diff(body: dict, live: dict) -> list:
    if body.get(KEY) is None or rows_match(body[KEY], live.get(KEY, [])):
        return []
    return [Difference(f"/Body/{KEY}", DiffKind.REPLACE, True, body[KEY], True, live.get(KEY, []))]
