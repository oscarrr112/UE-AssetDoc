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
             {"Widget": "Ok", "Property": "Visibility", "Type": "Enum", "Keys": [{"Frame": 15, "Value": "Visible"}]},
             {"Target": "Root", "Property": "RenderOpacity", "Type": "Float", "Keys": [{"Frame": 0, "Value": 1.0}]},
             {"Widget": "Glow", "Property": "Brush", "Type": "Material", "Parameters": {"Pulse": "Scalar", "Tint": "Color"},
              "Channels": [{"Name": "Pulse", "Keys": [...]}, {"Name": "TintR", "Keys": [...]}]}]}]

Frames are display frames of FrameRate (the Sequencer's frame numbers; fractions allowed).
Animations are matched by Name, tracks by (Widget, Target, Property, Type).  A listed animation
is the whole animation (unlisted tracks are removed); an omitted FrameRate / PlaybackRange keeps
the current value (a new animation gets 30 fps and frames 0..30).  Key Interp defaults to Linear
(Cubic, Constant); a Cubic key may carry Tangent {Mode: SmartAuto|User|Break, Arrive, Leave}.
``Target: "Root"`` (no Widget) animates the user widget itself.  A ``Material`` track keys the
material parameters of a widget's brush (``Property`` is the brush's property path); its
``Parameters`` are the complete {name: Scalar|Bool|Vector2D|Vector|Color} set and its channels are
named as the Sequencer names them (``Tint`` gives ``TintR`` ... ``TintA``, ``Offset`` gives
``Offset.X`` ...).  ``"Deactivated": true`` is a track deactivated in the Sequencer.  ``Section`` is the track section's
Details: {StartFrame?, EndFrame? (a missing bound is infinite), Active?, WhenFinished?:
KeepState|RestoreState|ProjectDefault, PreRoll?, PostRoll?}; a stated Section is complete (missing
settings are a new section's), an omitted one is kept and grows to cover the track's keys.
Tracks the profile cannot represent are extracted as {"Widget", "Property", "Type",
"Unsupported": reason, "Sections": [...]}: the sections and their keys are read-only.  They must be
listed; a listed track keeps its content (``Sections``, when listed, must be unchanged).  A track
listed as ``Unsupported`` that the profile now represents is kept as it is.
"""
from __future__ import annotations

import math

from asset_document.diagnostics import contract_error
from asset_document.diff.model import Difference, DiffKind
from asset_document.profiles import ublueprint_members as members

TRACK_TYPES = ("Float", "Double", "Bool", "Byte", "Enum", "Integer", "Color", "Transform", "Margin", "Material")
TARGETS = ("Widget", "Slot", "Root")
PARAMETER_KINDS = ("Scalar", "Bool", "Vector2D", "Vector", "Color")
COMPLETION_MODES = ("KeepState", "RestoreState", "ProjectDefault")
SECTION_DEFAULTS = {"Active": True, "WhenFinished": "ProjectDefault", "PreRoll": 0, "PostRoll": 0}


def _canonical_section(value, path) -> dict:
    if type(value) is not dict or set(value) - {"StartFrame", "EndFrame", *SECTION_DEFAULTS}:
        raise _error(path, "Section is {StartFrame?, EndFrame?, Active?, WhenFinished?, PreRoll?, PostRoll?}.",
                     "Widget.InvalidTrack")
    out = {}
    for key in ("StartFrame", "EndFrame", "PreRoll", "PostRoll"):
        if key in value:
            out[key] = _number(value[key], f"{path}/{key}", key)
    if "StartFrame" in out and "EndFrame" in out and out["EndFrame"] < out["StartFrame"]:
        raise _error(f"{path}/EndFrame", "EndFrame must not be before StartFrame.", "Widget.InvalidTrack")
    if any(out.get(key, 0) < 0 for key in ("PreRoll", "PostRoll")):
        raise _error(path, "PreRoll and PostRoll are not negative.", "Widget.InvalidTrack")
    if "Active" in value:
        if type(value["Active"]) is not bool:
            raise _error(f"{path}/Active", "Active is true or false.", "Widget.InvalidTrack")
        out["Active"] = value["Active"]
    if "WhenFinished" in value:
        if value["WhenFinished"] not in COMPLETION_MODES:
            raise _error(f"{path}/WhenFinished", f"WhenFinished is one of {', '.join(COMPLETION_MODES)}.",
                         "Widget.InvalidTrack")
        out["WhenFinished"] = value["WhenFinished"]
    return {key: item for key, item in out.items() if SECTION_DEFAULTS.get(key, None) != item}


def _section_state(section: dict | None) -> dict:
    return {**SECTION_DEFAULTS, **(section or {})}
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


# Rows an earlier extract wrote as Unsupported under another identity: a track on the user widget itself was
# listed under the root binding's name with this reason, and a material parameter track by its class path with no
# Property.
_OLD_ROOT_REASON = "the track animates the user widget itself"
_OLD_MATERIAL_TYPE = "/Script/UMG.MovieSceneWidgetMaterialTrack"


def _old_row_alias(track: dict, live_tracks: list) -> dict | None:
    """The live track an Unsupported row of an earlier extract names, or None."""
    if track.get("Target", "Widget") != "Widget":
        return None
    widget, material = track.get("Widget", ""), track.get("Type") == _OLD_MATERIAL_TYPE and not track.get("Property")
    if material:
        for owner in (lambda row: row.get("Target", "Widget") == "Widget" and row.get("Widget", "") == widget,
                      lambda row: row.get("Target") == "Root"):
            found = [row for row in live_tracks if row.get("Type") == "Material" and owner(row)]
            if len(found) == 1:
                return found[0]
            if found:
                return None
        return None
    if track.get("Unsupported") == _OLD_ROOT_REASON:
        found = [row for row in live_tracks if row.get("Target") == "Root"
                 and row.get("Property", "") == track.get("Property", "") and row.get("Type") == track.get("Type")]
        return found[0] if len(found) == 1 else None
    return None


def _document_tracks(desired: list, live_tracks: list) -> list:
    """The document's tracks with an earlier extract's Unsupported rows read under the identity the live track has
    now (kept as listed)."""
    live_ids = {track_identity(row) for row in live_tracks}
    out = []
    for track in desired:
        alias = _old_row_alias(track, live_tracks) if "Unsupported" in track and track_identity(track) not in live_ids \
            else None
        if alias is not None:
            track = {**{key: value for key, value in track.items() if key not in ("Widget", "Target", "Property",
                                                                                     "Type", "Sections")},
                     **{key: alias[key] for key in ("Widget", "Target", "Property", "Type") if key in alias}}
        out.append(track)
    return out


def _sections_from_live(rows: list) -> list:
    out = []
    for row in rows:
        item = {}
        if "start" in row:
            item["StartFrame"] = row["start"]
        if "end" in row:
            item["EndFrame"] = row["end"]
        channels = []
        for channel in row.get("channels", []):
            entry = {"Name": channel["name"], "Kind": channel.get("kind", ""),
                     "Keys": [_key_from_live(key) if "value" in key else {"Frame": key["frame"]}
                              for key in channel.get("keys", [])]}
            if "default" in channel:
                entry["Default"] = channel["default"]
            channels.append(entry)
        item["Channels"] = channels
        out.append(item)
    return out


def _canonical_track(value, path) -> dict:
    allowed = {"Widget", "Target", "Property", "Type", "Keys", "Channels", "Unsupported", "Sections", "Deactivated",
               "Parameters", "Section"}
    if type(value) is not dict or set(value) - allowed:
        raise _error(path, "A track is {Widget, Target?, Property, Type, Keys | Channels}.", "Widget.InvalidTrack")
    if "Unsupported" in value:
        return {key: value[key] for key in ("Widget", "Target", "Property", "Type", "Unsupported", "Sections", "Deactivated")
                if key in value}
    widget, prop, kind = value.get("Widget", ""), value.get("Property"), value.get("Type")
    target = value.get("Target", "Widget")
    if target not in TARGETS:
        raise _error(f"{path}/Target", "Target is Widget (default), Slot or Root.", "Widget.InvalidTrack")
    if type(widget) is not str or (target == "Root") == bool(widget) or type(prop) is not str or not prop \
            or type(kind) is not str:
        raise _error(path, "A track needs Widget (none for Target Root), Property and Type.", "Widget.InvalidTrack")
    canonical_kind = next((item for item in TRACK_TYPES if item.casefold() == kind.casefold()), kind)
    if canonical_kind not in TRACK_TYPES and not kind.startswith("/Script/"):
        raise _error(f"{path}/Type", f"Type is one of {', '.join(TRACK_TYPES)} or a property track class path.",
                     "Widget.InvalidTrack")
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
    if target != "Widget":
        out["Target"] = target
    if target == "Root":
        del out["Widget"]
    deactivated = value.get("Deactivated", False)
    if type(deactivated) is not bool:
        raise _error(f"{path}/Deactivated", "Deactivated is true or false.", "Widget.InvalidTrack")
    if deactivated:
        out["Deactivated"] = True
    parameters = value.get("Parameters")
    if canonical_kind == "Material":
        if target == "Slot":
            raise _error(f"{path}/Target", "A Material track animates a brush of the widget, not of its slot.",
                         "Widget.InvalidTrack")
        parameters = {} if parameters is None else parameters
        if type(parameters) is not dict or any(type(name) is not str or not name or kind not in PARAMETER_KINDS
                                               for name, kind in parameters.items()):
            raise _error(f"{path}/Parameters", "Parameters is {\"Name\": Scalar|Bool|Vector2D|Vector|Color}.",
                         "Widget.InvalidTrack")
        out["Parameters"] = dict(sorted(parameters.items()))
    elif parameters is not None:
        raise _error(f"{path}/Parameters", "Only a Material track has Parameters.", "Widget.InvalidTrack")
    if "Section" in value:
        out["Section"] = _canonical_section(value["Section"], f"{path}/Section")
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
    elif row.get("target") == "root":
        out["Target"] = "Root"
        del out["Widget"]
    if row.get("deactivated"):
        out["Deactivated"] = True
    if not row.get("supported", True):
        out["Unsupported"] = row.get("reason", "unsupported track")
        if row.get("sections"):
            out["Sections"] = _sections_from_live(row["sections"])
        return out
    if "parameters" in row:
        out["Parameters"] = {item["name"]: item["kind"] for item in row["parameters"]}
    if row.get("section"):
        names = {"start": "StartFrame", "end": "EndFrame", "active": "Active", "when_finished": "WhenFinished",
                 "pre_roll": "PreRoll", "post_roll": "PostRoll"}
        out["Section"] = {names[key]: value for key, value in row["section"].items() if key in names}
    channels = []
    for channel in row.get("channels", []):
        item = {"Name": channel["name"], "Keys": [_key_from_live(key) for key in channel.get("keys", [])]}
        if "default" in channel:
            item["Default"] = channel["default"]
        channels.append(item)
    if len(channels) == 1 and channels[0]["Name"] == "Value" and "Default" not in channels[0] and "Parameters" not in out:
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
    return {"animation": animation, "widget": track.get("Widget", ""),
            "target": track.get("Target", "Widget").lower(),
            "property": track["Property"], "type": track["Type"]}


def _section_equal(want: dict, have: dict) -> bool:
    if "Section" not in want:
        return True  # an omitted Section is kept
    a, b = _section_state(want["Section"]), _section_state(have.get("Section"))
    if set(a) != set(b):
        return False
    return all(a[key] == b[key] if type(a[key]) in (bool, str) else _close(a[key], b[key]) for key in a)


def _content_equal(want: dict, have: dict) -> bool:
    return (want.get("Deactivated", False) == have.get("Deactivated", False)
            and _section_equal(want, have)
            and want.get("Parameters") == have.get("Parameters")
            and channels_equal(want["Channels"], have.get("Channels", [])))


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
    desired = {**desired, "Tracks": _document_tracks(desired["Tracks"], live_tracks)}
    unsupported_live = sorted(track_identity(t) for t in live_tracks if "Unsupported" in t)
    # A track listed as Unsupported is kept as it is, also once the profile represents it.
    kept = {track_identity(t) for t in desired["Tracks"] if "Unsupported" in t}
    unsupported_want = sorted(i for i in kept if i in unsupported_live or i not in
                              {track_identity(t) for t in live_tracks})
    if unsupported_live != unsupported_want:
        missing = [i for i in unsupported_live if i not in unsupported_want] or unsupported_want
        reasons = {track_identity(t): t["Unsupported"] for t in live_tracks if "Unsupported" in t}
        raise _error(f"{path}/Tracks",
                     f"{name} has tracks this profile cannot edit; list them unchanged as extracted: "
                     + "; ".join(f"{w or '?'}.{p or '?'} ({k}): {reasons.get((w, t, p, k), 'not on the asset')}"
                                 for w, t, p, k in missing),
                     "Widget.UnsupportedTrack")
    by_identity = {track_identity(t): t for t in live_tracks}
    for position, track in enumerate(desired["Tracks"]):
        current = by_identity.get(track_identity(track))
        if "Unsupported" in track and "Sections" in track and current is not None and "Unsupported" in current \
                and track["Sections"] != current.get("Sections"):
            raise _error(f"{path}/Tracks/{position}/Sections",
                         f"{name}: the keys of a track this profile cannot edit are read-only; edit them in the "
                         f"Sequencer ({current['Unsupported']}).", "Widget.UnsupportedTrack")
    have = {track_identity(t): t for t in live_tracks if "Unsupported" not in t}
    want = {track_identity(t): t for t in desired["Tracks"] if "Unsupported" not in t}
    ops = []
    for identity, track in sorted(have.items()):
        if identity not in want and identity not in kept:
            ops.append(("umg.anim.track.remove.v1", _track_address(name, track), f"{path}/Tracks"))
    for position, track in enumerate(desired["Tracks"]):
        if "Unsupported" in track:
            continue
        current = have.get(track_identity(track))
        if current is not None and _content_equal(track, current):
            continue
        values = {**_track_address(name, track), "channels": [_wire_channel(row) for row in track["Channels"]]}
        if track.get("Deactivated", False) or (current or {}).get("Deactivated", False):
            values["deactivated"] = track.get("Deactivated", False)
        if "Parameters" in track:
            values["parameters"] = [{"name": key, "kind": kind} for key, kind in track["Parameters"].items()]
        if "Section" in track:
            names = {"StartFrame": "start", "EndFrame": "end", "Active": "active", "WhenFinished": "when_finished",
                     "PreRoll": "pre_roll", "PostRoll": "post_roll"}
            values["section"] = {names[key]: value for key, value in track["Section"].items()}
        ops.append(("umg.anim.track.set.v1", values, f"{path}/Tracks/{position}"))
    return ops


def animation_mismatch(desired: dict, live: dict | None) -> bool:
    if live is None:
        return True
    if header_values(desired, live) is not None:
        return True
    have = {track_identity(t): t for t in live["Tracks"]}
    want = {track_identity(t): t for t in _document_tracks(desired["Tracks"], live["Tracks"])}
    if set(have) != set(want):
        return True
    return any(("Unsupported" not in t and ("Unsupported" in have[i] or not _content_equal(t, have[i])))
               or ("Unsupported" in t and "Sections" in t and "Unsupported" in have[i] and t["Sections"] != have[i].get("Sections"))
               for i, t in want.items())


def diff_animations(authored: list, extracted: list) -> list:
    live = {row["Name"]: row for row in extracted}
    names = sorted({row["Name"] for row in authored} ^ set(live)
                   | {row["Name"] for row in authored if animation_mismatch(row, live.get(row["Name"]))})
    if not names:
        return []
    return [Difference("/Body/Animations", DiffKind.REPLACE, True, {"Animations": names}, True,
                       {"Animations": sorted(live)})]
