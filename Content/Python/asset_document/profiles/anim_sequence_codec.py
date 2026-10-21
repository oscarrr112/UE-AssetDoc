"""AnimSequence document <-> native authoring shapes (pure functions, no engine calls).

Body regions (see docs/architecture/asset-document-anim-sequence-matrix.md)::

    References  {Skeleton, RetargetSource, RetargetSourceAsset}
    Preview     {PreviewMesh, PreviewPoseAsset}
    Playback    {RateScale, bLoop, Interpolation, NumberOfFrames, FrameRate}
    Additive    {AdditiveAnimType, RefPoseType, RefFrameIndex, RefPoseSeq}
    RootMotion  {bEnableRootMotion, RootMotionRootLock, bForceRootLock, bUseNormalizedRootMotionScale}
    Compression {CompressionErrorThresholdScale, bDoNotOverrideCompression, bAllowFrameStripping,
                 BoneCompressionSettings, CurveCompressionSettings, VariableFrameStrippingSettings}
    Curves        [{Name, Keys, Flags, Color, Comment}]
    NotifyTracks  [{Name}]
    Notifies      [{Time, Track, Class | Name, Properties, <trigger settings>}]
    NotifyStates  [{Time, Duration, Track, Class, Properties, <trigger settings>}]
    SyncMarkers   [{Name, Time, Track}]
    Metadata      [{Class, Properties}]
    AssetUserData [{Class, Properties}]
    BoneTracks    [bone names]  (read-only)

Settings regions are sparse per field. A list region is its full desired state;
inside a row, omitted optional fields (Track excepted: it defaults to the first
notify track) keep the reused row's value or take the UE default, and are not
compared.
"""
from __future__ import annotations

import math
import struct

from asset_document.diagnostics import contract_error
from asset_document.profiles.curve_float import _key_from_document, _key_to_document

CLASS = "/Script/Engine.AnimSequence"

# Plain reflected fields: written by the generic Editor property batch.
PLAIN_FIELDS = {
    "References": ("RetargetSource", "RetargetSourceAsset"),
    "Playback": ("RateScale", "bLoop", "Interpolation"),
    "Additive": ("AdditiveAnimType", "RefPoseType", "RefFrameIndex"),
    "RootMotion": ("bEnableRootMotion", "RootMotionRootLock", "bForceRootLock", "bUseNormalizedRootMotionScale"),
    "Compression": ("CompressionErrorThresholdScale", "bDoNotOverrideCompression", "bAllowFrameStripping"),
}
# Hard object references: written by animation.asset.authoring.edit.v1.
REFERENCE_FIELDS = {
    "Preview": ("PreviewPoseAsset",),
    "Additive": ("RefPoseSeq",),
    "Compression": ("BoneCompressionSettings", "CurveCompressionSettings", "VariableFrameStrippingSettings"),
}
# Fields with a dedicated authoring section.
SPECIAL_FIELDS = {
    "References": ("Skeleton",),
    "Preview": ("PreviewMesh",),
    "Playback": ("NumberOfFrames", "FrameRate"),
}
SETTINGS_REGIONS = ("References", "Preview", "Playback", "Additive", "RootMotion", "Compression")
LIST_REGIONS = ("Curves", "NotifyTracks", "Notifies", "NotifyStates", "SyncMarkers", "Metadata", "AssetUserData", "BoneTracks")
BODY_KEYS = SETTINGS_REGIONS + LIST_REGIONS
REGION_FIELDS = {region: PLAIN_FIELDS.get(region, ()) + REFERENCE_FIELDS.get(region, ()) + SPECIAL_FIELDS.get(region, ())
                 for region in SETTINGS_REGIONS}
BODY_OWNED_PROPERTIES = frozenset(name for fields in REGION_FIELDS.values() for name in fields)
_NUMBER_FIELDS = frozenset(("RateScale", "CompressionErrorThresholdScale"))
_INTEGER_FIELDS = frozenset(("RefFrameIndex", "NumberOfFrames"))
_BOOL_FIELDS = frozenset(name for name in BODY_OWNED_PROPERTIES if name.startswith("b"))
_PATH_FIELDS = frozenset(("Skeleton", "PreviewMesh", "RetargetSourceAsset")) | frozenset(
    name for fields in REFERENCE_FIELDS.values() for name in fields)
_STRING_FIELDS = frozenset(("RetargetSource", "Interpolation", "AdditiveAnimType", "RefPoseType", "RootMotionRootLock"))

CURVE_FLAGS = {"Editable": 0x04, "Metadata": 0x10, "DriveTrack": 0x20, "Disabled": 0x40}
DEFAULT_CURVE_FLAGS = ("Editable",)
TRIGGER_DEFAULTS = {
    "TriggerWeightThreshold": struct.unpack("<f", struct.pack("<f", 0.00001))[0],
    "NotifyTriggerChance": 1.0,
    "NotifyFilterType": "NoFiltering",
    "NotifyFilterLOD": 0,
    "bCanBeFilteredViaRequest": True,
    "bTriggerOnDedicatedServer": True,
    "bTriggerOnFollower": False,
    "MontageTickType": "Queued",
}
_NOTIFY_FIELDS = frozenset(("Time", "Track", "Class", "Name", "Properties", *TRIGGER_DEFAULTS))
_STATE_FIELDS = (_NOTIFY_FIELDS - {"Name"}) | {"Duration"}
_LEGACY_TRACK = "TrackName"


def error(path, message, code="AnimSequence.InvalidDocument"):
    return contract_error(path, code, message)


def f32(value):
    return struct.unpack("<f", struct.pack("<f", float(value)))[0]


def shortest(value):
    """The shortest decimal that round-trips to the same float32."""
    stored = f32(value)
    for digits in range(1, 10):
        candidate = float(f"{stored:.{digits}g}")
        if f32(candidate) == stored:
            return candidate
    return stored


def _number(value, path):
    if type(value) not in (int, float) or not math.isfinite(value) or abs(value) > 3.4e38:
        raise error(path, "Expected a finite number.")
    return f32(value)


def _integer(value, path):
    # JSON transports may carry integral numbers as floats (30.0).
    if type(value) is float and value.is_integer() and abs(value) < 2 ** 53:
        value = int(value)
    if type(value) is not int:
        raise error(path, "Expected an integer.")
    return value


def _string(value, path, nonempty=True):
    if type(value) is not str or (nonempty and not value):
        raise error(path, "Expected a nonempty string." if nonempty else "Expected a string.")
    return value


def _object(value, path, fields=None, code="AnimSequence.InvalidDocument"):
    if type(value) is not dict:
        raise error(path, "Expected an object.", code)
    if fields is not None:
        for key in value:
            if key not in fields:
                raise error(f"{path}/{key}", f"Unknown field '{key}'. Fields: {', '.join(sorted(fields))}.",
                            "AnimSequence.UnknownField")
    return value


def _list(value, path):
    if type(value) is not list:
        raise error(path, "Expected an array.")
    return value


def object_path(value, path, nullable=True):
    """A canonical object path ("/Game/A/B" -> "/Game/A/B.B"); legacy {Kind: AssetRef|ClassRef, Path} accepted."""
    if type(value) is dict and value.get("Kind") in ("AssetRef", "ClassRef"):
        value = value.get("Path", value.get("Class"))
    if value is None and nullable:
        return None
    if type(value) is not str or not value.startswith("/") or ":" in value or " " in value:
        raise error(path, f"Expected an object path such as /Game/Folder/Asset (or null), got {value!r}.",
                    "AnimSequence.InvalidReference")
    leaf = value.rsplit("/", 1)[-1]
    return value if "." in leaf else f"{value}.{leaf}"


def frame_rate(value, path):
    """30, "30", "30000/1001" -> (numerator, denominator)."""
    if type(value) is float and value.is_integer():
        value = int(value)
    if type(value) is int and value > 0:
        return value, 1
    if type(value) is str:
        parts = value.split("/")
        if len(parts) in (1, 2) and all(part.isdigit() and int(part) > 0 for part in parts):
            return int(parts[0]), int(parts[1]) if len(parts) == 2 else 1
    raise error(path, "FrameRate is a positive integer (30) or a ratio string (\"30000/1001\").")


def frame_rate_to_document(numerator, denominator):
    return numerator if denominator == 1 else f"{numerator}/{denominator}"


# ---------------------------------------------------------------------------
# Canonical (comparable) forms. Authored and extracted rows normalize alike.
# ---------------------------------------------------------------------------
def _setting(name, value, path):
    if name in _PATH_FIELDS:
        if name == "Skeleton" and value is None:
            raise error(path, "Skeleton cannot be cleared.")
        return object_path(value, path)
    if name in _NUMBER_FIELDS:
        return _number(value, path)
    if name in _INTEGER_FIELDS:
        value = _integer(value, path)
        if name == "NumberOfFrames" and value < 1:
            raise error(path, "NumberOfFrames must be at least 1.")
        return value
    if name in _BOOL_FIELDS:
        if type(value) is not bool:
            raise error(path, "Expected true or false.")
        return value
    if name == "FrameRate":
        return frame_rate(value, path)
    if name == "RetargetSource":
        # FName: "" and "None" both mean the default (no retarget source name).
        return "None" if value == "" else _string(value, path)
    return _string(value, path)


def settings(body):
    """{region: {field: canonical value}} for the authored settings regions."""
    out = {}
    for region in SETTINGS_REGIONS:
        if region not in body:
            continue
        values = _object(body[region], f"/Body/{region}", REGION_FIELDS[region])
        out[region] = {name: _setting(name, value, f"/Body/{region}/{name}") for name, value in values.items()}
    additive = out.get("Additive", {})
    # UAnimSequence::PostEditChangeProperty clears RefPoseSeq whenever these settings hide it.
    if additive.get("RefPoseSeq") is not None and (
            additive.get("AdditiveAnimType") == "AAT_None"
            or additive.get("RefPoseType") in ("ABPT_RefPose", "ABPT_LocalAnimFrame")):
        raise error("/Body/Additive/RefPoseSeq", "RefPoseSeq is only kept with an additive AdditiveAnimType and "
                    "RefPoseType ABPT_AnimScaled or ABPT_AnimFrame (UE clears it otherwise).",
                    "AnimSequence.RefPoseSeqHidden")
    return out


def curve_flags(value, path):
    names = _list(value, path)
    bits = 0
    for index, name in enumerate(names):
        if name not in CURVE_FLAGS:
            raise error(f"{path}/{index}", f"Curve flags are {', '.join(CURVE_FLAGS)}.", "AnimSequence.UnknownCurveFlag")
        bits |= CURVE_FLAGS[name]
    return bits


def curve_flags_to_document(bits):
    return [name for name, bit in CURVE_FLAGS.items() if bits & bit]


def _color(value, path):
    color = _object(value, path, {"R", "G", "B", "A"})
    if set(color) != {"R", "G", "B", "A"}:
        raise error(path, "Color requires R, G, B and A.")
    return {key: _number(color[key], f"{path}/{key}") for key in ("R", "G", "B", "A")}


def curves(value, path="/Body/Curves"):
    """Canonical curve rows in authored order; keys in native form (float32, engine enum names)."""
    rows, names = [], set()
    for index, row in enumerate(_list(value, path)):
        row_path = f"{path}/{index}"
        row = _object(row, row_path, {"Name", "Keys", "Flags", "Color", "Comment", "CurveType"})
        name = _string(row.get("Name"), f"{row_path}/Name")
        if name in names:
            raise error(f"{row_path}/Name", f"Curve '{name}' is listed twice.", "AnimSequence.DuplicateCurve")
        names.add(name)
        if row.get("CurveType", "Float") != "Float":
            raise error(f"{row_path}/CurveType", "Only float curves are authored.", "AnimSequence.UnsupportedCurveType")
        keys = [_key_from_document(key, f"{row_path}/Keys/{key_index}")
                for key_index, key in enumerate(_list(row.get("Keys", []), f"{row_path}/Keys"))]
        for key_index in range(1, len(keys)):
            if keys[key_index]["Time"] <= keys[key_index - 1]["Time"]:
                raise error(f"{row_path}/Keys/{key_index}/Time", "Key times must be strictly ascending.",
                            "AnimSequence.KeysNotAscending")
        canonical = {"Name": name, "Keys": keys}
        if "Flags" in row:
            canonical["Flags"] = curve_flags(row["Flags"], f"{row_path}/Flags")
        if "Color" in row:
            canonical["Color"] = _color(row["Color"], f"{row_path}/Color")
        if "Comment" in row:
            canonical["Comment"] = _string(row["Comment"], f"{row_path}/Comment", nonempty=False)
        rows.append(canonical)
    return rows


def _track(row, path):
    if "Track" in row and _LEGACY_TRACK in row:
        raise error(path, "Give Track or the legacy TrackName, not both.")
    track = row.get("Track", row.get(_LEGACY_TRACK))
    return None if track is None else _string(track, f"{path}/Track")


def notify_tracks(value, path="/Body/NotifyTracks"):
    names = []
    for index, row in enumerate(_list(value, path)):
        row_path = f"{path}/{index}"
        if type(row) is str:
            name = _string(row, row_path)
        else:
            row = _object(row, row_path, {"Name", _LEGACY_TRACK})
            name = _string(row.get("Name", row.get(_LEGACY_TRACK)), f"{row_path}/Name")
        if name in names:
            raise error(row_path, f"Notify track '{name}' is listed twice.", "AnimSequence.DuplicateNotifyTrack")
        names.append(name)
    if not names:
        raise error(path, "List at least one notify track: UE always keeps one (it recreates track \"1\").",
                    "AnimSequence.NotifyTrackRequired")
    return names


def _trigger(row, path):
    out = {}
    for name, default in TRIGGER_DEFAULTS.items():
        if name not in row:
            continue
        value = row[name]
        if type(default) is bool:
            if type(value) is not bool:
                raise error(f"{path}/{name}", "Expected true or false.")
        elif type(default) is int:
            value = _integer(value, f"{path}/{name}")
            if value < 0:
                raise error(f"{path}/{name}", "Expected a nonnegative integer.")
        elif type(default) is float:
            value = _number(value, f"{path}/{name}")
        else:
            value = _string(value, f"{path}/{name}")
        out[name] = value
    return out


def _properties(value, path):
    return dict(_object(value, path))


def notifies(value, states, path):
    rows = []
    for index, row in enumerate(_list(value, path)):
        row_path = f"{path}/{index}"
        row = _object(row, row_path, (_STATE_FIELDS if states else _NOTIFY_FIELDS) | {_LEGACY_TRACK})
        time = _number(row.get("Time"), f"{row_path}/Time")
        if time < 0:
            raise error(f"{row_path}/Time", "Time must not be negative.", "AnimSequence.TimeOutOfRange")
        canonical = {"Time": time, "Track": _track(row, row_path)}
        cls = row.get("Class")
        if cls is not None:
            canonical["Class"] = object_path(cls, f"{row_path}/Class", nullable=False)
            canonical["Properties"] = _properties(row.get("Properties", {}), f"{row_path}/Properties")
        elif states:
            raise error(f"{row_path}/Class", "A notify state requires its Class.", "AnimSequence.NotifyClassRequired")
        else:
            if "Properties" in row and row["Properties"]:
                raise error(f"{row_path}/Properties", "A named notify (no Class) has no Properties.")
            canonical["Name"] = _string(row.get("Name"), f"{row_path}/Name")
        if states:
            duration = _number(row.get("Duration"), f"{row_path}/Duration")
            if duration <= 0:
                raise error(f"{row_path}/Duration", "Duration must be positive.")
            canonical["Duration"] = duration
        canonical["Trigger"] = _trigger(row, row_path)
        rows.append(canonical)
    return rows


def sync_markers(value, path="/Body/SyncMarkers"):
    rows = []
    for index, row in enumerate(_list(value, path)):
        row_path = f"{path}/{index}"
        row = _object(row, row_path, {"Name", "Time", "Track", _LEGACY_TRACK})
        time = _number(row.get("Time"), f"{row_path}/Time")
        if time < 0:
            raise error(f"{row_path}/Time", "Time must not be negative.", "AnimSequence.TimeOutOfRange")
        rows.append({"Name": _string(row.get("Name"), f"{row_path}/Name"), "Time": time, "Track": _track(row, row_path)})
    return rows


def object_rows(value, path, unique_class=False):
    rows, classes = [], set()
    for index, row in enumerate(_list(value, path)):
        row_path = f"{path}/{index}"
        if type(row) is dict and row.get("Kind") == "EmbeddedObject":
            row = {key: item for key, item in row.items() if key != "Kind"}
        row = _object(row, row_path, {"Class", "Properties"})
        cls = object_path(row.get("Class"), f"{row_path}/Class", nullable=False)
        if unique_class and cls in classes:
            raise error(f"{row_path}/Class", "Asset user data holds one object per class.", "AnimSequence.DuplicateUserDataClass")
        classes.add(cls)
        rows.append({"Class": cls, "Properties": _properties(row.get("Properties", {}), f"{row_path}/Properties")})
    return rows


def bone_tracks(value, path="/Body/BoneTracks"):
    return [_string(item, f"{path}/{index}") for index, item in enumerate(_list(value, path))]


def canonical_body(body):
    """Validate every authored region; returns {region: canonical value}."""
    _object(body, "/Body")
    for key in body:
        if key not in BODY_KEYS:
            raise error(f"/Body/{key}", f"Unknown Body key '{key}'. Keys: {', '.join(BODY_KEYS)}.", "AnimSequence.UnknownBodyKey")
    out = settings(body)
    if "Curves" in body:
        out["Curves"] = curves(body["Curves"])
    if "NotifyTracks" in body:
        out["NotifyTracks"] = notify_tracks(body["NotifyTracks"])
    if "Notifies" in body:
        out["Notifies"] = notifies(body["Notifies"], False, "/Body/Notifies")
    if "NotifyStates" in body:
        out["NotifyStates"] = notifies(body["NotifyStates"], True, "/Body/NotifyStates")
    if "SyncMarkers" in body:
        out["SyncMarkers"] = sync_markers(body["SyncMarkers"])
    if "Metadata" in body:
        out["Metadata"] = object_rows(body["Metadata"], "/Body/Metadata")
    if "AssetUserData" in body:
        out["AssetUserData"] = object_rows(body["AssetUserData"], "/Body/AssetUserData", unique_class=True)
    if "BoneTracks" in body:
        out["BoneTracks"] = bone_tracks(body["BoneTracks"])
    _check_tracks(out)
    return out


def _check_tracks(canonical):
    """Authored rows must name authored tracks; two notifies may not share a track and time."""
    tracks = canonical.get("NotifyTracks")
    first = tracks[0] if tracks else None
    seen = {}
    for region in ("Notifies", "NotifyStates", "SyncMarkers"):
        for index, row in enumerate(canonical.get(region, ())):
            if tracks is not None and row["Track"] is not None and row["Track"] not in tracks:
                raise error(f"/Body/{region}/{index}/Track", f"Notify track '{row['Track']}' is not in NotifyTracks.",
                            "AnimSequence.UnknownNotifyTrack")
            if region == "SyncMarkers":
                continue
            key = (row["Track"] or first, row["Time"])
            if key in seen:
                raise error(f"/Body/{region}/{index}/Time",
                            f"Two notifies share track '{key[0] or '(first)'}' at time {shortest(key[1])}; "
                            "UE would move one to another track. Use another track or time.",
                            "AnimSequence.OverlappingNotifies")
            seen[key] = True


def validate_properties(properties):
    _object(properties, "/Properties")
    for name in properties:
        if name in BODY_OWNED_PROPERTIES:
            raise error(f"/Properties/{name}", f"'{name}' is authored in its Body region, not in Properties.",
                        "AnimSequence.ManagedPropertyOverlap")
    return dict(properties)


# ---------------------------------------------------------------------------
# Lowering to the authoring intent and the Editor batch.
# ---------------------------------------------------------------------------
def _intent_key(key):
    return {"Time": key["Time"], "Value": key["Value"], "InterpMode": key["InterpMode"], "TangentMode": key["TangentMode"],
            "TangentWeightMode": key["TangentWeightMode"], "ArriveTangent": key["ArriveTangent"],
            "ArriveTangentWeight": key["ArriveTangentWeight"], "LeaveTangent": key["LeaveTangent"],
            "LeaveTangentWeight": key["LeaveTangentWeight"]}


def _intent_notify(row, states):
    out = {"time": row["Time"]}
    if states:
        out["duration"] = row["Duration"]
    if row["Track"] is not None:
        out["track"] = row["Track"]
    if "Class" in row:
        out["class"] = row["Class"]
        out["properties"] = row["Properties"]
    else:
        out["class"] = None
        out["name"] = row["Name"]
    if row["Trigger"]:
        out["trigger"] = dict(row["Trigger"])
    return out


def lower(canonical, properties, admitted_writable=None):
    """(authoring intent, editor batch fields) for one canonical Body + Properties.

    ``admitted_writable``: names the Editor batch can write on the target; other
    Properties keys go to the authoring primitive as object references (it
    rejects names that are not editable object references).
    """
    intent, fields, references = {}, {}, {}
    for region, values in canonical.items():
        if region not in SETTINGS_REGIONS:
            continue
        for name, value in values.items():
            if name in PLAIN_FIELDS.get(region, ()):
                fields[name] = _soft_to_native(value) if name in SOFT_PATH_FIELDS else value
            elif name in REFERENCE_FIELDS.get(region, ()):
                references[name] = value
            elif name == "Skeleton":
                intent["skeleton"] = value
            elif name == "PreviewMesh":
                intent["preview_mesh"] = value
            elif name == "NumberOfFrames":
                intent["number_of_frames"] = value
            elif name == "FrameRate":
                intent["frame_rate"] = {"numerator": value[0], "denominator": value[1]}
    for name, value in properties.items():
        if admitted_writable is None or name in admitted_writable:
            fields[name] = value
        else:
            references[name] = value
    if references:
        intent["object_properties"] = references
    if "Curves" in canonical:
        intent["curves"] = []
        for row in canonical["Curves"]:
            curve = {"name": row["Name"], "keys": [_intent_key(key) for key in row["Keys"]]}
            if "Flags" in row:
                curve["flags"] = row["Flags"]
            if "Color" in row:
                curve["color"] = dict(row["Color"])
            if "Comment" in row:
                curve["comment"] = row["Comment"]
            intent["curves"].append(curve)
    if "NotifyTracks" in canonical:
        intent["notify_tracks"] = list(canonical["NotifyTracks"])
    if "Notifies" in canonical:
        intent["notifies"] = [_intent_notify(row, False) for row in canonical["Notifies"]]
    if "NotifyStates" in canonical:
        intent["notify_states"] = [_intent_notify(row, True) for row in canonical["NotifyStates"]]
    if "SyncMarkers" in canonical:
        intent["sync_markers"] = [{key: value for key, value in (("name", row["Name"]), ("time", row["Time"]),
                                                                  ("track", row["Track"])) if value is not None}
                                  for row in canonical["SyncMarkers"]]
    for region, key in (("Metadata", "metadata"), ("AssetUserData", "asset_user_data")):
        if region in canonical:
            intent[key] = [{"class": row["Class"], "properties": row["Properties"]} for row in canonical[region]]
    return intent, fields


# ---------------------------------------------------------------------------
# Snapshot -> document
# ---------------------------------------------------------------------------
def _native_float(value, path):
    if type(value) not in (int, float):
        raise error(path, "Native snapshot number expected.", "AnimSequence.InvalidSnapshot")
    return shortest(value)


def _document_value(value):
    return shortest(value) if type(value) is float else value


# Soft object paths travel through the Editor codec as export text ("None" = null).
SOFT_PATH_FIELDS = frozenset(("RetargetSourceAsset",))


def _soft_from_native(value):
    return None if value in ("None", "") else value


def _soft_to_native(value):
    return "None" if value is None else value


def snapshot_to_document_parts(snapshot):
    """(Body, Properties) from the native authoring snapshot."""
    if type(snapshot) is not dict:
        raise error("/PrimitiveOutcome", "The authoring snapshot must be an object.", "AnimSequence.InvalidSnapshot")
    native = snapshot["properties"]
    references = snapshot["object_properties"]
    body = {}
    for region in SETTINGS_REGIONS:
        values = {}
        for name in PLAIN_FIELDS.get(region, ()):
            if name in native:
                values[name] = (_soft_from_native(native[name]) if name in SOFT_PATH_FIELDS
                                else _document_value(native[name]))
        for name in REFERENCE_FIELDS.get(region, ()):
            if name in references:
                values[name] = references[name]
        body[region] = values
    body["References"]["Skeleton"] = snapshot["skeleton"]
    body["Preview"]["PreviewMesh"] = snapshot["preview_mesh"]
    rate = snapshot["frame_rate"]
    body["Playback"]["NumberOfFrames"] = snapshot["number_of_frames"]
    body["Playback"]["FrameRate"] = frame_rate_to_document(rate["numerator"], rate["denominator"])
    body["Curves"] = []
    for curve in snapshot["curves"]:
        row = {"Name": curve["name"],
               "Keys": [_key_to_document({**key, "Time": float(key["Time"]), "Value": float(key["Value"]),
                                          **{name: float(key[name]) for name in ("ArriveTangent", "ArriveTangentWeight",
                                                                                 "LeaveTangent", "LeaveTangentWeight")}},
                                         f"/Body/Curves/{curve['name']}") for key in curve["keys"]],
               "Flags": curve_flags_to_document(curve["flags"]),
               "Color": {key: _native_float(curve["color"][key], "/Body/Curves") for key in ("R", "G", "B", "A")}}
        if curve["comment"]:
            row["Comment"] = curve["comment"]
        body["Curves"].append(row)
    body["Curves"].sort(key=lambda item: item["Name"])
    body["NotifyTracks"] = [{"Name": name} for name in snapshot["notify_tracks"]]
    for region, key in (("Notifies", "notifies"), ("NotifyStates", "notify_states")):
        rows = []
        for item in snapshot[key]:
            row = {"Time": _native_float(item["time"], f"/Body/{region}")}
            if "duration" in item:
                row["Duration"] = _native_float(item["duration"], f"/Body/{region}")
            row["Track"] = item["track"]
            if item["class"] is not None:
                row["Class"] = item["class"]
                row["Properties"] = {name: _document_value(value) for name, value in item["properties"].items()}
            else:
                row["Name"] = item["name"]
            for name, default in TRIGGER_DEFAULTS.items():
                value = item["trigger"][name]
                if type(default) is float:
                    value = f32(value)
                if value != default:
                    row[name] = shortest(value) if type(default) is float else value
            rows.append(row)
        body[region] = rows
    body["SyncMarkers"] = [{"Name": item["name"], "Time": _native_float(item["time"], "/Body/SyncMarkers"),
                            "Track": item["track"]} for item in snapshot["sync_markers"]]
    for region, key in (("Metadata", "metadata"), ("AssetUserData", "asset_user_data")):
        body[region] = [{"Class": item["class"],
                         "Properties": {name: _document_value(value) for name, value in item["properties"].items()}}
                        for item in snapshot[key]]
    body["BoneTracks"] = list(snapshot["bone_tracks"])
    properties = {}
    for name, value in native.items():
        if name not in BODY_OWNED_PROPERTIES and type(value) not in (list, dict):
            properties[name] = value
    for name, value in references.items():
        if name not in BODY_OWNED_PROPERTIES:
            properties[name] = value
    return body, properties


# ---------------------------------------------------------------------------
# Comparison: authored (canonical) vs extracted (canonical); per authored field.
# ---------------------------------------------------------------------------
def _same(expected, actual):
    if type(expected) in (int, float) and type(actual) in (int, float) and type(expected) is not bool and type(actual) is not bool:
        return f32(expected) == f32(actual)
    if type(expected) is dict and type(actual) is dict:
        return set(expected) == set(actual) and all(_same(expected[key], actual[key]) for key in expected)
    if type(expected) in (list, tuple) and type(actual) in (list, tuple):
        return len(expected) == len(actual) and all(_same(a, b) for a, b in zip(expected, actual))
    return expected == actual


def _subset(expected, actual):
    """Every authored key holds in actual (values compared at float32)."""
    return all(key in actual and _same(value, actual[key]) for key, value in expected.items())


def _tangents_authored(keys, index):
    key = keys[index]
    return (key["TangentMode"] in ("RCTM_User", "RCTM_Break") and key["InterpMode"] != "RCIM_Constant"
            and (index == 0 or keys[index - 1]["InterpMode"] != "RCIM_Constant"))


def _keys_match(expected, actual):
    if len(expected) != len(actual):
        return False
    for index, (want, have) in enumerate(zip(expected, actual)):
        fields = ["Time", "Value", "InterpMode", "TangentMode", "TangentWeightMode"]
        if _tangents_authored(expected, index):
            fields += ["ArriveTangent", "LeaveTangent", "ArriveTangentWeight", "LeaveTangentWeight"]
        if not all(_same(want[name], have[name]) for name in fields):
            return False
    return True


def _curves_match(expected, actual):
    expected = sorted(expected, key=lambda row: row["Name"])
    actual = sorted(actual, key=lambda row: row["Name"])
    if [row["Name"] for row in expected] != [row["Name"] for row in actual]:
        return False
    for want, have in zip(expected, actual):
        if not _keys_match(want["Keys"], have["Keys"]):
            return False
        if "Flags" in want and want["Flags"] != have.get("Flags", curve_flags(list(DEFAULT_CURVE_FLAGS), "/")):
            return False
        if "Color" in want and ("Color" not in have or not _same(want["Color"], have["Color"])):
            return False
        if "Comment" in want and want["Comment"] != have.get("Comment", ""):
            return False
    return True


def _resolve_track(row, first):
    return row["Track"] if row["Track"] is not None else first


def _notify_key(row, first):
    return (row["Time"], _resolve_track(row, first), row.get("Class") or "", row.get("Name") or "")


def _notifies_match(expected, actual, first_expected, first_actual):
    if len(expected) != len(actual):
        return False
    want_rows = sorted(expected, key=lambda row: _notify_key(row, first_expected))
    have_rows = sorted(actual, key=lambda row: _notify_key(row, first_actual))
    for want, have in zip(want_rows, have_rows):
        if _notify_key(want, first_expected) != _notify_key(have, first_actual):
            return False
        if "Duration" in want and not _same(want["Duration"], have.get("Duration")):
            return False
        if not _subset(want["Trigger"], {**TRIGGER_DEFAULTS, **have["Trigger"]}):
            return False
        if not _subset(want.get("Properties", {}), have.get("Properties", {})):
            return False
    return True


def _markers_match(expected, actual, first_expected, first_actual):
    def key(row, first):
        return (row["Time"], row["Name"], _resolve_track(row, first))
    return (sorted(key(row, first_expected) for row in expected) == sorted(key(row, first_actual) for row in actual))


def _objects_match(expected, actual):
    return len(expected) == len(actual) and all(
        want["Class"] == have["Class"] and _subset(want["Properties"], have["Properties"])
        for want, have in zip(expected, actual))


def region_matches(region, expected, actual, authored, extracted):
    """True when the authored region holds on the extracted asset."""
    if region in SETTINGS_REGIONS:
        return _subset(expected, actual)
    if region == "Curves":
        return _curves_match(expected, actual)
    if region in ("NotifyTracks", "BoneTracks"):
        return expected == actual
    first_expected = (authored.get("NotifyTracks") or extracted.get("NotifyTracks") or [None])[0]
    first_actual = (extracted.get("NotifyTracks") or [None])[0]
    if region in ("Notifies", "NotifyStates"):
        return _notifies_match(expected, actual, first_expected, first_actual)
    if region == "SyncMarkers":
        return _markers_match(expected, actual, first_expected, first_actual)
    return _objects_match(expected, actual)


def properties_match(expected, actual):
    return _subset(expected, actual)
