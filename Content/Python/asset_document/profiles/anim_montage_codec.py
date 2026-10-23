# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Pure, lossless canonical/native projection for the structural Montage keys.

Settings regions (Blend, Sync, RootMotion, ...) live in anim_montage_settings.

Native correspondence is request-local evidence, never a canonical persistent ID.
UE dependency, durable-link and safe property admission checks remain native-owned.
"""
from __future__ import annotations

from copy import deepcopy
import math
import struct

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.diagnostics import contract_error
from asset_document.profiles.anim_montage_settings import settings_snapshot_to_body


def _fail(path, message, code="AnimMontage.InvalidCanonicalShape"):
    raise contract_error(path, code, message)


def _text(value, path):
    if type(value) is not str:
        _fail(path, "Expected a string")
    return value


def _name(value, path):
    value = _text(value, path)
    if not value or value.casefold() == "none":
        _fail(path, "Expected a nonempty, non-None name")
    return value


def _reference(value, path):
    if value is None:
        return None
    value = _text(value, path)
    if not value.startswith("/") or ":" in value:
        _fail(path, "Expected an absolute asset path or null")
    leaf = value.rsplit("/", 1)[-1]
    if not leaf:
        _fail(path, "Asset path must name an object or package")
    return value if "." in leaf else value + "." + leaf


def _required_reference(value, path):
    if value is None:
        _fail(path, "An explicit reference is required")
    return _reference(value, path)


def _class_reference(value, path):
    value = _text(value, path)
    if not value.startswith("/") or ":" in value or "." not in value.rsplit("/", 1)[-1]:
        _fail(path, "Notify Class requires an explicit object path")
    return value


def _integer(low, high):
    def validate(value, path):
        if type(value) is not int or not low <= value <= high:
            _fail(path, f"Expected an integer in [{low}, {high}]")
        return value
    return validate


def _float(value, path):
    if type(value) not in (int, float):
        _fail(path, "Expected a finite float32 number")
    try:
        result = struct.unpack("!f", struct.pack("!f", value))[0]
    except (OverflowError, struct.error):
        _fail(path, "Number is outside finite float32 range")
    if not math.isfinite(result):
        _fail(path, "Expected a finite float32 number")
    return result


def _nonnegative(value, path):
    result = _float(value, path)
    if result < 0:
        _fail(path, "Expected a nonnegative time")
    return result


def _positive(value, path):
    result = _float(value, path)
    if result <= 0:
        _fail(path, "Expected a positive float32 duration")
    return result


def _rate(value, path):
    result = _float(value, path)
    if result == 0:
        _fail(path, "Animation play rate must be nonzero")
    return result


def _boolean(value, path):
    if type(value) is not bool:
        _fail(path, "Expected a boolean")
    return value


def _optional_bits(value, path):
    return None if value is None else _integer(0, 0xffffffff)(value, path)


def _properties(value, path):
    if type(value) is not dict:
        _fail(path, "Expected an explicit Properties object")
    DocumentCodec().validate(value)
    return deepcopy(value)


_BITS = (("Bits", "bits", _integer(0, 0xffffffff)),)
_DEFAULT = (("HasValue", "has_value", _boolean),
            ("Float32Bits", "float32_bits", _optional_bits))
_COLOR = tuple((c, c.lower(), _BITS) for c in ("R", "G", "B", "A"))
_KEY = (
    ("Time", "time", _BITS), ("Value", "value", _BITS),
    ("ArriveTangent", "arrive_tangent", _BITS),
    ("LeaveTangent", "leave_tangent", _BITS),
    ("InterpMode", "interp_mode", _integer(0, 3)),
    ("TangentMode", "tangent_mode", _integer(0, 4)),
    ("TangentWeightMode", "tangent_weight_mode", _integer(0, 3)),
    ("ArriveTangentWeight", "arrive_tangent_weight", _BITS),
    ("LeaveTangentWeight", "leave_tangent_weight", _BITS),
)
_CURVE = (
    ("Name", "name", _name), ("Flags", "flags", _integer(-0x80000000, 0x7fffffff)),
    ("Color", "color", _COLOR), ("Comment", "comment", _text),
    ("DefaultValue", "default_value", _DEFAULT),
    ("PreExtrapolation", "pre_extrapolation", _integer(0, 5)),
    ("PostExtrapolation", "post_extrapolation", _integer(0, 5)),
    ("Keys", "keys", [_KEY]),
)
_SEGMENT = (
    ("AnimReference", "animation", _required_reference),
    ("StartPos", "start", _nonnegative),
    ("AnimStartTime", "source_start", _nonnegative),
    ("AnimEndTime", "source_end", _nonnegative),
    ("AnimPlayRate", "rate", _rate),
    ("LoopingCount", "loops", _integer(1, 0x7fffffff)),
)
_SLOT = (("SlotName", "name", _name), ("Segments", "segments", [_SEGMENT]))
_SECTION = (
    ("Name", "name", _name), ("NextSectionName", "next", "optional_name"),
    ("LinkableTime", "time", _nonnegative),
    ("SlotName", "slot_name", "optional_name"),
    ("LinkMethod", "method", _integer(0, 2)),
)
_OBJECT = (("Class", "class", _class_reference),
           ("Properties", "properties", _properties))
_NOTIFY = (("Time", "time", _nonnegative),
           ("TrackIndex", "track_index", _integer(0, 0x7fffffff)),
           ("Object", "object", _OBJECT))
_STATE = (_NOTIFY[0], ("Duration", "duration", _positive), *_NOTIFY[1:])
# A notify track is a label (the editor's Rename Notify Track takes any text); TrackIndex is its position.
_TRACK = (("Name", "name", _text),)
_BODY = (
    ("Skeleton", "skeleton", _reference), ("PreviewMesh", "preview_mesh", _reference),
    ("Curves", "curves", [_CURVE]), ("SlotAnimTracks", "slots", [_SLOT]),
    ("CompositeSections", "sections", [_SECTION]),
    ("Notifies", "notifies", [_NOTIFY]), ("NotifyStates", "notify_states", [_STATE]),
    ("NotifyTracks", "notify_tracks", [_TRACK]),
)
SUPPORTED_BODY_KEYS = tuple(row[0] for row in _BODY)


def _translate(value, schema, reverse, path, *, sparse=False):
    if schema == "optional_name":
        if reverse:
            return None if value == "None" else _name(value, path)
        return "None" if value is None else _name(value, path)
    if callable(schema):
        return schema(value, path)
    if type(schema) is list:
        if type(value) is not list:
            _fail(path, "Expected an array; omission preserves and [] explicitly clears")
        return [_translate(row, schema[0], reverse, f"{path}/{i}")
                for i, row in enumerate(value)]
    if type(value) is not dict:
        _fail(path, "Expected an object")
    if any(type(key) is not str for key in value):
        _fail(path, "Object field names must be strings")
    source_names = {row[1 if reverse else 0] for row in schema}
    unknown = set(value) - source_names
    missing = source_names - set(value)
    if unknown or (missing and not sparse):
        _fail(path, f"Exact fields required; missing={sorted(missing)}, unknown={sorted(unknown)}")
    result = {}
    for canonical, native, child in schema:
        source, target = (native, canonical) if reverse else (canonical, native)
        if source in value:
            result[target] = _translate(value[source], child, reverse, f"{path}/{source}")
    return result


def _validate_native_semantics(native):
    for key in ("curves", "slots", "sections"):
        seen = set()
        for i, row in enumerate(native.get(key, [])):
            identity = row["name"].casefold()
            if identity in seen:
                _fail(f"/Body/{key}/{i}", "Duplicate native name identity")
            seen.add(identity)
    for i, curve in enumerate(native.get("curves", [])):
        path = f"/Body/Curves/{i}"
        default = curve["default_value"]
        bits = default["float32_bits"]
        if (default["has_value"] and (bits is None or bits == 0x7f7fffff)
                or not default["has_value"] and bits is not None):
            _fail(path + "/DefaultValue", "Set requires non-sentinel bits; unset requires null bits")
        previous = -math.inf
        for j, key in enumerate(curve["keys"]):
            time = struct.unpack("!f", struct.pack("!I", key["time"]["bits"]))[0]
            if not math.isfinite(time) or time <= previous:
                _fail(f"{path}/Keys/{j}/Time", "Curve times must be finite and strictly increasing")
            previous = time
    for i, slot in enumerate(native.get("slots", [])):
        for j, segment in enumerate(slot["segments"]):
            if segment["source_end"] <= segment["source_start"]:
                _fail(f"/Body/SlotAnimTracks/{i}/Segments/{j}", "Source end must exceed source start")
    if "notify_tracks" in native and not native["notify_tracks"]:
        _fail("/Body/NotifyTracks", "List at least one notify track: the editor always keeps one (a Montage without "
              "tracks gets track \"1\")", "AnimMontage.NotifyTrackRequired")
    if "sections" in native:
        names = {row["name"].casefold() for row in native["sections"]}
        for i, row in enumerate(native["sections"]):
            if row["next"] != "None" and row["next"].casefold() not in names:
                _fail(f"/Body/CompositeSections/{i}/NextSectionName", "Next section is absent")


def validate_notify_times(body: dict) -> None:
    """When the document lists SlotAnimTracks, a notify must end within the Montage length they give
    (FAnimSegment::GetEndPos, longest track): apply rejects it (AnimationProvider.Montage.NotifyBeyondLength)."""
    if "SlotAnimTracks" not in body or not any(key in body for key in ("Notifies", "NotifyStates")):
        return
    native = _translate(body, _BODY, False, "/Body", sparse=True)
    length = max((segment["start"] + segment["loops"] * (segment["source_end"] - segment["source_start"]) / abs(segment["rate"])
                  for slot in native["slots"] for segment in slot["segments"]), default=0.0)
    for key, label in (("notifies", "Notifies"), ("notify_states", "NotifyStates")):
        for i, row in enumerate(native.get(key, [])):
            end = row["time"] + row.get("duration", 0.0)
            if end > length + 1e-4:
                _fail(f"/Body/{label}/{i}/Time", f"{label}[{i}] ends at {end:g} s, after the Montage length {length:g} s that SlotAnimTracks give",
                      "AnimMontage.NotifyBeyondLength")


def validate_notify_tracks(body: dict, created: bool = False) -> None:
    """NotifyTracks replaces the notify panel's tracks with the notifies that use them, so it travels with Notifies or
    NotifyStates; every authored TrackIndex names one of its tracks. A new Montage without NotifyTracks has one track."""
    keys = ("Notifies", "NotifyStates", "NotifyTracks")
    if not any(key in body for key in keys):
        return
    native = _translate({key: body[key] for key in keys if key in body}, _BODY, False, "/Body", sparse=True)
    tracks = native.get("notify_tracks")
    if tracks == []:
        _validate_native_semantics({"notify_tracks": tracks})
    if tracks is not None and "notifies" not in native and "notify_states" not in native:
        _fail("/Body/NotifyTracks", "NotifyTracks is applied with the notifies on those tracks: list Notifies or NotifyStates "
              "(as Extract gives them) in the same document", "AnimMontage.NotifyTracksNeedNotifies")
    if tracks is None and not created:
        return  # apply checks TrackIndex against the Montage's current tracks
    count = len(tracks) if tracks is not None else 1
    names = ", ".join(f"{i} '{row['name']}'" for i, row in enumerate(tracks or [{"name": "1"}]))
    for key, label in (("notifies", "Notifies"), ("notify_states", "NotifyStates")):
        for i, row in enumerate(native.get(key, [])):
            if row["track_index"] >= count:
                where = "NotifyTracks lists" if tracks is not None else "a new Montage has (add more with NotifyTracks)"
                _fail(f"/Body/{label}/{i}/TrackIndex", f"{label}[{i}] TrackIndex {row['track_index']} does not exist: "
                      f"{where} {count} notify track(s): {names}", "AnimMontage.NotifyTrackMissing")


def normalize_body(body: object) -> dict:
    """Validate complete rows, normalize numeric float32s, preserve sparse omission.

    Callers resolve aliases and select the seven admitted Body keys first. Native
    legality and class/property admission are deliberately not simulated here.
    """
    native = _translate(body, _BODY, False, "/Body", sparse=True)
    _validate_native_semantics(native)
    return _translate(native, _BODY, True, "/Body", sparse=True)


def _snapshot_without_correspondence(snapshot):
    result = deepcopy(snapshot)
    if type(result) is not dict:
        _fail("/Snapshot", "Expected the full native snapshot object")
    # Validate structural nesting before removing the two observation-only fields.
    slots = result.get("slots")
    if type(slots) is not list:
        _fail("/Snapshot/slots", "Expected slots array")
    for i, slot in enumerate(slots):
        if type(slot) is not dict or type(slot.get("segments")) is not list:
            _fail(f"/Snapshot/slots/{i}", "Expected slot with segments array")
        for j, segment in enumerate(slot["segments"]):
            path = f"/Snapshot/slots/{i}/segments/{j}"
            if type(segment) is not dict or not {"previous_slot", "previous_segment"} <= set(segment):
                _fail(path, "Snapshot segment requires observed correspondence")
            previous_slot = segment.pop("previous_slot")
            previous_index = segment.pop("previous_segment")
            if (previous_slot != slot.get("name") or type(previous_index) is not int
                    or previous_index != j):
                _fail(path, "Snapshot correspondence must identify its actual current slot/index")
    return result


def _shortest(value):
    """The shortest decimal that reads back as the same float32 (0.1, not 0.10000000149011612)."""
    stored = struct.unpack("!f", struct.pack("!f", value))[0]
    for digits in range(1, 10):
        candidate = float(f"{stored:.{digits}g}")
        if struct.pack("!f", candidate) == struct.pack("!f", stored):
            return candidate
    return stored


# Body fields stored as float32 (the core rule: Extract writes the shortest decimal of the stored float).
_FLOAT32_ROWS = (("SlotAnimTracks", "Segments", ("StartPos", "AnimStartTime", "AnimEndTime", "AnimPlayRate")),
                 ("CompositeSections", None, ("LinkableTime",)),
                 ("Notifies", None, ("Time",)), ("NotifyStates", None, ("Time", "Duration")))
_FLOAT32_REGIONS = (("Blend", ("BlendInTime", "BlendOutTime", "BlendOutTriggerTime")),
                    ("TimeStretch", ("SamplingRate", "CurveValueMinPrecision")))


def _f32(value: float) -> float:
    return struct.unpack("!f", struct.pack("!f", value))[0]


def _shortest_duration(time: float, duration: float) -> float:
    """A notify state stores its end time; Duration reads back as end - start in float32, so 0.3 + 0.35
    reads back as 0.34999996. Emit the shortest decimal whose float32 sum with the start gives the same
    end (what apply writes): 0.35."""
    start = _f32(time)
    end = _f32(start + _f32(duration))
    for digits in range(1, 10):
        candidate = float(f"{_f32(duration):.{digits}g}")
        if candidate > 0 and _f32(start + _f32(candidate)) == end:
            return candidate
    return _shortest(duration)


def _shortest_float32_fields(body: dict) -> dict:
    for key, nested, names in _FLOAT32_ROWS:
        rows = body.get(key, [])
        if nested is not None:
            rows = [row for parent in rows for row in parent.get(nested, [])]
        for row in rows:
            for name in names:
                if name == "Duration" and type(row.get(name)) is float and type(row.get("Time")) in (int, float):
                    row[name] = _shortest_duration(row["Time"], row[name])
                elif type(row.get(name)) is float:
                    row[name] = _shortest(row[name])
    for key, names in _FLOAT32_REGIONS:
        region = body.get(key)
        for name in names:
            if type(region) is dict and type(region.get(name)) is float:
                region[name] = _shortest(region[name])
    return body


def snapshot_to_document_parts(snapshot: object) -> tuple[dict, dict]:
    """Project a full native snapshot to (canonical Body, Properties)."""
    native = _snapshot_without_correspondence(snapshot)
    settings = native.pop("settings", None)
    if native.get("notify_tracks") == []:
        # A Montage without notify tracks shows (and gets, on load or its first notify edit) the editor's track "1".
        native["notify_tracks"] = [{"name": "1"}]
    body = normalize_body(_translate(native, _BODY, True, "/Snapshot"))
    properties = {}
    if settings is not None:
        regions, properties = settings_snapshot_to_body(settings)
        body.update(regions)
    return _shortest_float32_fields(body), properties


def snapshot_to_body(snapshot: object) -> dict:
    """Project an independently read full native snapshot to canonical Body."""
    return snapshot_to_document_parts(snapshot)[0]


def _bind_correspondence(slots, baseline):
    old = [(slot["name"], i, segment) for slot in baseline
           for i, segment in enumerate(slot["segments"])]
    used = set()
    for slot in slots:
        for segment in slot["segments"]:
            matches = [(name, i) for name, i, previous in old if previous == segment]
            if len(matches) > 1 or (matches and matches[0] in used):
                _fail("/Body/SlotAnimTracks", "Ambiguous baseline segment correspondence",
                      "AnimMontage.AmbiguousSegmentCorrespondence")
            match = matches[0] if matches else ("None", -1)
            if matches:
                used.add(match)
            segment["previous_slot"], segment["previous_segment"] = match


def lower_body(body: object, baseline_snapshot: object | None = None) -> dict:
    """Return sparse native values in fixed parser order, without transport/steps.

    Only a unique complete semantic match retains a baseline segment identity.
    Modified/new segments have no inferred identity; native retained-link guards
    must reject any dependent links that cannot safely follow that replacement.
    """
    native = _translate(normalize_body(body), _BODY, False, "/Body", sparse=True)
    if "slots" in native:
        baseline = []
        if baseline_snapshot is not None:
            baseline_body = snapshot_to_body(baseline_snapshot)
            baseline_body = {key: baseline_body[key] for key in SUPPORTED_BODY_KEYS}
            baseline = _translate(baseline_body, _BODY, False, "/Baseline")["slots"]
        _bind_correspondence(native["slots"], baseline)
    return native
