# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Widget animations (pure): canonical shape, live conversion, planning and diff.

Body region::

    "Animations": [
        {"Name": "FadeIn", "FrameRate": {"Numerator": 30, "Denominator": 1},
         "PlaybackRange": {"StartFrame": 0, "EndFrame": 30},
         "Tracks": [
             {"Widget": "Title", "Property": "RenderOpacity", "Type": "Float",
              "Keys": [{"Frame": 0, "Value": 0.0}, {"Frame": 30, "Value": 1.0, "Interp": "Cubic"}]},
             {"Widget": "Title", "Property": "RenderTransform", "Type": "Transform",
              "Channels": [{"Name": "Translation.Y", "Keys": [{"Frame": 0, "Value": 40}, {"Frame": 30, "Value": 0}]},
                           {"Name": "Angle", "Keys": [{"Frame": 0, "Value": -5}]}]},
             {"Widget": "Title", "Target": "Slot", "Property": "LayoutData.Offsets", "Type": "Margin",
              "Channels": [{"Name": "Left", "Keys": [{"Frame": 0, "Value": 10}]}]},
             {"Widget": "Ok", "Property": "Visibility", "Type": "Enum", "Keys": [{"Frame": 15, "Value": "Visible"}]}]}]

Frames are display frames of FrameRate (the Sequencer's frame numbers; fractions allowed).
Animations are matched by Name, tracks by (Widget, Target, Property, Type).  A listed animation
is the whole animation (unlisted tracks are removed); an omitted FrameRate / PlaybackRange keeps
the current value (a new animation gets 30 fps and frames 0..30).  Key Interp defaults to Linear
(Cubic, Constant); a Cubic key may carry Tangent {Mode: SmartAuto|User|Break, Arrive, Leave}.
Tracks the profile cannot represent are extracted as {"Widget", "Property", "Type",
"Unsupported": reason} and must be listed unchanged.
"""
from __future__ import annotations

import math

from asset_document.diagnostics import contract_error
from asset_document.diff.model import Difference, DiffKind
from asset_document.profiles import ublueprint_members as members

TRACK_TYPES = ("Float", "Double", "Bool", "Byte", "Enum", "Integer", "Color", "Transform", "Margin")
CHANNEL_ALIASES = {"Rotation": "Angle"}
DEFAULT_RATE = {"Numerator": 30, "Denominator": 1}
DEFAULT_RANGE = {"StartFrame": 0, "EndFrame": 30}
TANGENT_MODES = ("Auto", "SmartAuto", "User", "Break")


def _error(path, message, code="Widget.InvalidAnimation"):
    return contract_error(path, code, message)


def _whole(value):
    """An int, or an integral float as int; None for anything else (bool included)."""
    if type(value) is int:
        return value
    if type(value) is float and math.isfinite(value) and value.is_integer():
        return int(value)
    return None


def _number(value, path, label):
    if type(value) not in (int, float) or isinstance(value, bool) or not math.isfinite(value):
        raise _error(path, f"{label} must be a number.")
    return int(value) if float(value).is_integer() else value


# ---------------------------------------------------------------------------
# Canonical shapes
# ---------------------------------------------------------------------------
def _canonical_key(value, path) -> dict:
    if type(value) is not dict or set(value) - {"Frame", "Value", "Interp", "Tangent"} or "Value" not in value:
        raise _error(path, "A key is {\"Frame\": 0, \"Value\": 1.0, \"Interp\"?: Linear|Cubic|Constant}.",
                     "Widget.InvalidKey")
    out = {"Frame": _number(value.get("Frame"), f"{path}/Frame", "Frame"), "Value": value["Value"]}
    if type(out["Value"]) in (int, float) and not isinstance(out["Value"], bool):
        out["Value"] = _number(out["Value"], f"{path}/Value", "Value")
    interp = value.get("Interp", "Linear")
    if interp not in ("Linear", "Cubic", "Constant"):
        raise _error(f"{path}/Interp", "Interp is Linear, Cubic or Constant.", "Widget.InvalidKey")
    if interp != "Linear":
        out["Interp"] = interp
    tangent = value.get("Tangent")
    if tangent is not None:
        if interp != "Cubic":
            raise _error(f"{path}/Tangent", "Tangent applies to Cubic keys only.", "Widget.InvalidKey")
        if type(tangent) is not dict or set(tangent) - {"Mode", "Arrive", "Leave"} or tangent.get("Mode") not in TANGENT_MODES:
            raise _error(f"{path}/Tangent", "Tangent is {\"Mode\": SmartAuto|User|Break, \"Arrive\", \"Leave\"}.",
                         "Widget.InvalidKey")
        mode = tangent["Mode"]
        if mode == "SmartAuto":
            out["Tangent"] = {"Mode": mode}
        elif mode in ("User", "Break"):
            out["Tangent"] = {"Mode": mode,
                              "Arrive": _number(tangent.get("Arrive", 0), f"{path}/Tangent/Arrive", "Arrive"),
                              "Leave": _number(tangent.get("Leave", 0), f"{path}/Tangent/Leave", "Leave")}
    return out


def _canonical_channel(value, path) -> dict | None:
    if type(value) is not dict or set(value) - {"Name", "Keys", "Default"} or type(value.get("Name")) is not str:
        raise _error(path, "A channel is {\"Name\": \"Translation.X\", \"Keys\": [...], \"Default\"?: value}.",
                     "Widget.InvalidTrack")
    keys = value.get("Keys", [])
    if type(keys) is not list:
        raise _error(f"{path}/Keys", "Keys is an array.", "Widget.InvalidTrack")
    out = {"Name": CHANNEL_ALIASES.get(value["Name"], value["Name"]),
           "Keys": [_canonical_key(key, f"{path}/Keys/{index}") for index, key in enumerate(keys)]}
    if "Default" in value and value["Default"] is not None:
        out["Default"] = value["Default"]
    if not out["Keys"] and "Default" not in out:
        return None  # an empty channel is the same as an unlisted one
    return out


def track_identity(track: dict) -> tuple:
    return (track.get("Widget", ""), track.get("Target", "Widget"), track.get("Property", ""), track.get("Type", ""))


def _canonical_track(value, path) -> dict:
    allowed = {"Widget", "Target", "Property", "Type", "Keys", "Channels", "Unsupported"}
    if type(value) is not dict or set(value) - allowed:
        raise _error(path, "A track is {Widget, Target?, Property, Type, Keys | Channels}.", "Widget.InvalidTrack")
    if "Unsupported" in value:
        return {key: value[key] for key in ("Widget", "Target", "Property", "Type", "Unsupported") if key in value}
    widget, prop, kind = value.get("Widget"), value.get("Property"), value.get("Type")
    if type(widget) is not str or not widget or type(prop) is not str or not prop or type(kind) is not str:
        raise _error(path, "A track needs Widget, Property and Type.", "Widget.InvalidTrack")
    canonical_kind = next((item for item in TRACK_TYPES if item.casefold() == kind.casefold()), kind)
    if canonical_kind not in TRACK_TYPES and not kind.startswith("/Script/"):
        raise _error(f"{path}/Type", f"Type is one of {', '.join(TRACK_TYPES)} or a property track class path.",
                     "Widget.InvalidTrack")
    target = value.get("Target", "Widget")
    if target not in ("Widget", "Slot"):
        raise _error(f"{path}/Target", "Target is Widget (default) or Slot.", "Widget.InvalidTrack")
    if "Keys" in value and "Channels" in value:
        raise _error(path, "Give Keys (single-channel track) or Channels, not both.", "Widget.InvalidTrack")
    channels = value.get("Channels", [{"Name": "Value", "Keys": value.get("Keys", [])}])
    if type(channels) is not list:
        raise _error(f"{path}/Channels", "Channels is an array.", "Widget.InvalidTrack")
    rows = [_canonical_channel(item, f"{path}/Channels/{index}") for index, item in enumerate(channels)]
    rows = [row for row in rows if row is not None]
    names = [row["Name"] for row in rows]
    if len(set(names)) != len(names):
        raise _error(f"{path}/Channels", "A channel is listed twice.", "Widget.InvalidTrack")
    out = {"Widget": widget, "Property": prop, "Type": canonical_kind, "Channels": sorted(rows, key=lambda r: r["Name"])}
    if target == "Slot":
        out["Target"] = "Slot"
    return out


def canonical_animations(value, path: str = "/Body/Animations") -> list:
    if type(value) is not list:
        raise _error(path, "Animations is an array of {Name, FrameRate, PlaybackRange, Tracks}.")
    out, seen = [], {}
    for index, row in enumerate(value):
        here = f"{path}/{index}"
        if type(row) is not dict or set(row) - {"Name", "FrameRate", "PlaybackRange", "Tracks"}:
            raise _error(here, "An animation is {Name, FrameRate?, PlaybackRange?, Tracks}.")
        name = row.get("Name")
        if type(name) is not str or not name:
            raise _error(f"{here}/Name", "An animation needs a Name.")
        if name.casefold() in seen:
            raise _error(f"{here}/Name", f"Animation '{name}' is listed twice.", "Widget.DuplicateName")
        seen[name.casefold()] = index
        item = {"Name": name}
        if "FrameRate" in row:
            rate = row["FrameRate"]
            if type(rate) is not dict or set(rate) - {"Numerator", "Denominator"}:
                raise _error(f"{here}/FrameRate", "FrameRate is {Numerator, Denominator}.", "Widget.InvalidFrameRate")
            # JSON numbers may arrive as integral floats (e.g. a document that went through C++ JSON).
            numerator, denominator = (_whole(rate.get("Numerator")), _whole(rate.get("Denominator", 1)))
            if numerator is None or denominator is None or numerator <= 0 or denominator <= 0:
                raise _error(f"{here}/FrameRate", "FrameRate needs a positive integer Numerator and Denominator.",
                             "Widget.InvalidFrameRate")
            item["FrameRate"] = {"Numerator": numerator, "Denominator": denominator}
        if "PlaybackRange" in row:
            span = row["PlaybackRange"]
            if type(span) is not dict or set(span) - {"StartFrame", "EndFrame"}:
                raise _error(f"{here}/PlaybackRange", "PlaybackRange is {StartFrame, EndFrame}.")
            start = _number(span.get("StartFrame", 0), f"{here}/PlaybackRange/StartFrame", "StartFrame")
            end = _number(span.get("EndFrame", start), f"{here}/PlaybackRange/EndFrame", "EndFrame")
            if end < start:
                raise _error(f"{here}/PlaybackRange", "EndFrame must not be before StartFrame.")
            item["PlaybackRange"] = {"StartFrame": start, "EndFrame": end}
        tracks = row.get("Tracks", [])
        if type(tracks) is not list:
            raise _error(f"{here}/Tracks", "Tracks is an array.", "Widget.InvalidTrack")
        item["Tracks"] = [_canonical_track(track, f"{here}/Tracks/{position}") for position, track in enumerate(tracks)]
        identities = [track_identity(track) for track in item["Tracks"] if "Unsupported" not in track]
        if len(set(identities)) != len(identities):
            raise _error(f"{here}/Tracks", "Two tracks animate the same Widget, Target, Property and Type.",
                         "Widget.InvalidTrack")
        out.append(item)
    return out  # authored order (paths); animations are compared by Name


# ---------------------------------------------------------------------------
# Live query -> Body
# ---------------------------------------------------------------------------
def _key_from_live(row: dict) -> dict:
    out = {"Frame": row["frame"], "Value": row["value"]}
    if row.get("interp"):
        out["Interp"] = row["interp"]
    if row.get("tangent"):
        tangent = {"Mode": row["tangent"]["mode"]}
        if "arrive" in row["tangent"]:
            tangent["Arrive"], tangent["Leave"] = row["tangent"]["arrive"], row["tangent"]["leave"]
        out["Tangent"] = tangent
    return out


def track_from_live(row: dict) -> dict:
    out = {"Widget": row.get("widget", ""), "Property": row.get("property", ""), "Type": row["type"]}
    if row.get("target") == "slot":
        out["Target"] = "Slot"
    if not row.get("supported", True):
        out["Unsupported"] = row.get("reason", "unsupported track")
        return out
    channels = []
    for channel in row.get("channels", []):
        item = {"Name": channel["name"], "Keys": [_key_from_live(key) for key in channel.get("keys", [])]}
        if "default" in channel:
            item["Default"] = channel["default"]
        channels.append(item)
    if len(channels) == 1 and channels[0]["Name"] == "Value" and "Default" not in channels[0]:
        out["Keys"] = channels[0]["Keys"]  # the single-channel shorthand
    else:
        out["Channels"] = sorted(channels, key=lambda item: item["Name"])
    return out


def animation_from_live(detail: dict) -> dict:
    rate = detail["display_rate"]
    return {"Name": detail["name"],
            "FrameRate": {"Numerator": rate["numerator"], "Denominator": rate["denominator"]},
            "PlaybackRange": {"StartFrame": detail["playback"]["start"], "EndFrame": detail["playback"]["end"]},
            "Tracks": [track_from_live(row) for row in detail.get("tracks", [])]}


# ---------------------------------------------------------------------------
# Comparison and planning
# ---------------------------------------------------------------------------
def _close(a, b, tolerance=1e-3) -> bool:
    return math.isclose(a, b, rel_tol=0.0, abs_tol=tolerance)


def _keys_equal(want: list, have: list) -> bool:
    if len(want) != len(have):
        return False
    for a, b in zip(want, have):
        if not _close(a["Frame"], b["Frame"]) or not members.values_equal(a["Value"], b["Value"]):
            return False
        if a.get("Interp", "Linear") != b.get("Interp", "Linear"):
            return False
        ta, tb = a.get("Tangent", {"Mode": "Auto"}), b.get("Tangent", {"Mode": "Auto"})
        if ta["Mode"] != tb["Mode"] or any(not _close(ta.get(k, 0), tb.get(k, 0), 1e-4) for k in ("Arrive", "Leave")):
            return False
    return True


def channels_equal(want: list, have: list) -> bool:
    have_by = {row["Name"]: row for row in have}
    if set(have_by) != {row["Name"] for row in want}:
        return False
    for row in want:
        other = have_by[row["Name"]]
        if ("Default" in row) != ("Default" in other):
            return False
        if "Default" in row and not members.values_equal(row["Default"], other["Default"]):
            return False
        if not _keys_equal(row["Keys"], other["Keys"]):
            return False
    return True


def _rate(animation: dict) -> dict:
    return animation.get("FrameRate", DEFAULT_RATE)


def header_values(desired: dict, live: dict | None) -> dict | None:
    """umg.anim.set.v1 values for a new animation or a changed header (None when unchanged)."""
    values = {}
    rate = desired.get("FrameRate") if live is not None else _rate(desired)
    span = desired.get("PlaybackRange") if live is not None else desired.get("PlaybackRange", DEFAULT_RANGE)
    if rate is not None and (live is None or rate != live["FrameRate"]):
        values["display_rate"] = {"numerator": rate["Numerator"], "denominator": rate["Denominator"]}
    if span is not None and (live is None or not (_close(span["StartFrame"], live["PlaybackRange"]["StartFrame"])
                                                 and _close(span["EndFrame"], live["PlaybackRange"]["EndFrame"]))):
        values["playback"] = {"start": span["StartFrame"], "end": span["EndFrame"]}
    if live is not None and not values:
        return None
    return {"name": desired["Name"], **values}


def _track_address(animation: str, track: dict) -> dict:
    return {"animation": animation, "widget": track["Widget"],
            "target": "slot" if track.get("Target") == "Slot" else "widget",
            "property": track["Property"], "type": track["Type"]}


def _wire_channel(row: dict) -> dict:
    keys = []
    for key in row["Keys"]:
        item = {"frame": key["Frame"], "value": key["Value"]}
        if "Interp" in key:
            item["interp"] = key["Interp"]
        if "Tangent" in key:
            item["tangent"] = {"mode": key["Tangent"]["Mode"],
                               **({"arrive": key["Tangent"]["Arrive"], "leave": key["Tangent"]["Leave"]}
                                  if "Arrive" in key["Tangent"] else {})}
        keys.append(item)
    out = {"name": row["Name"], "keys": keys}
    if "Default" in row:
        out["default"] = row["Default"]
    return out


def plan_tracks(desired: dict, live: dict | None, path: str) -> list:
    """Track set/remove calls for one animation (live is the canonical live animation or None)."""
    name = desired["Name"]
    live_tracks = [] if live is None else live["Tracks"]
    unsupported_live = sorted(track_identity(t) for t in live_tracks if "Unsupported" in t)
    unsupported_want = sorted(track_identity(t) for t in desired["Tracks"] if "Unsupported" in t)
    if unsupported_live != unsupported_want:
        missing = [i for i in unsupported_live if i not in unsupported_want] or unsupported_want
        reasons = {track_identity(t): t["Unsupported"] for t in live_tracks if "Unsupported" in t}
        raise _error(f"{path}/Tracks",
                     f"{name} has tracks this profile cannot edit; list them unchanged as extracted: "
                     + "; ".join(f"{w or '?'}.{p or '?'} ({k}): {reasons.get((w, t, p, k), 'not on the asset')}"
                                 for w, t, p, k in missing),
                     "Widget.UnsupportedTrack")
    have = {track_identity(t): t for t in live_tracks if "Unsupported" not in t}
    want = {track_identity(t): t for t in desired["Tracks"] if "Unsupported" not in t}
    ops = []
    for identity, track in sorted(have.items()):
        if identity not in want:
            ops.append(("umg.anim.track.remove.v1", _track_address(name, track), f"{path}/Tracks"))
    for position, track in enumerate(desired["Tracks"]):
        if "Unsupported" in track:
            continue
        current = have.get(track_identity(track))
        if current is not None and channels_equal(track["Channels"], current["Channels"]):
            continue
        ops.append(("umg.anim.track.set.v1",
                    {**_track_address(name, track), "channels": [_wire_channel(row) for row in track["Channels"]]},
                    f"{path}/Tracks/{position}"))
    return ops


def animation_mismatch(desired: dict, live: dict | None) -> bool:
    if live is None:
        return True
    if header_values(desired, live) is not None:
        return True
    have = {track_identity(t): t for t in live["Tracks"]}
    want = {track_identity(t): t for t in desired["Tracks"]}
    if set(have) != set(want):
        return True
    return any("Unsupported" not in t and not channels_equal(t["Channels"], have[i].get("Channels", []))
               for i, t in want.items())


def diff_animations(authored: list, extracted: list) -> list:
    live = {row["Name"]: row for row in extracted}
    names = sorted({row["Name"] for row in authored} ^ set(live)
                   | {row["Name"] for row in authored if animation_mismatch(row, live.get(row["Name"]))})
    if not names:
        return []
    return [Difference("/Body/Animations", DiffKind.REPLACE, True, {"Animations": names}, True,
                       {"Animations": sorted(live)})]
