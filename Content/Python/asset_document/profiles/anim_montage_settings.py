"""Montage settings regions: validation, snapshot projection and lowering.

Plain reflected fields lower to one generic Editor property batch step. Fields
that need specific UE calls (FAlphaBlend setters, blend profiles, root motion,
preview base pose, time stretch configuration, metadata objects) lower to one
``animation.montage.settings.set.v1`` step. Both are rolled back by Core's
whole-package snapshot.
"""
from __future__ import annotations

from copy import deepcopy
import math
import struct

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.diagnostics import contract_error

SETTINGS_BODY_KEYS = (
    "Preview", "Blend", "Sync", "RootMotion", "TimeStretch", "Metadata", "SectionMetadata",
)
EDITOR_WRITE = "editor.property.write_admitted_batch.v1"
EDITOR_ENUMERATE = "editor.property.enumerate_admitted_batch.v1"
SETTINGS = "animation.montage.settings.set.v1"

ALPHA_BLEND_OPTIONS = frozenset((
    "Linear", "Cubic", "HermiteCubic", "Sinusoidal", "QuadraticInOut", "CubicInOut",
    "QuarticInOut", "QuinticInOut", "CircularIn", "CircularOut", "CircularInOut",
    "ExpIn", "ExpOut", "ExpInOut", "Custom",
))
BLEND_MODES = frozenset(("Standard", "Inertialization"))
ROOT_LOCKS = frozenset(("RefPose", "AnimFirstFrame", "Zero"))


def _fail(path, message, code="AnimMontage.InvalidSettings"):
    raise contract_error(path, code, message)


def _escape(value: str) -> str:
    return value.replace("~", "~0").replace("/", "~1")


def _float32(value, path, *, minimum=None, exclusive=False):
    if type(value) not in (int, float):
        _fail(path, "Expected a finite number")
    try:
        result = struct.unpack("!f", struct.pack("!f", value))[0]
    except (OverflowError, struct.error):
        _fail(path, "Number is outside finite float32 range")
    if not math.isfinite(result):
        _fail(path, "Expected a finite float32 number")
    if minimum is not None and (result < minimum or (exclusive and result == minimum)):
        _fail(path, f"Expected a number {'>' if exclusive else '>='} {minimum}")
    return result


def _bool(value, path):
    if type(value) is not bool:
        _fail(path, "Expected a boolean")
    return value


def _choice(options):
    def validate(value, path):
        if type(value) is not str or value not in options:
            _fail(path, f"Expected one of {sorted(options)}")
        return value
    return validate


def _fname(value, path):
    if type(value) is not str:
        _fail(path, "Expected a name string")
    # FName("") reads back as None.
    return "None" if value == "" else value


def _optional_name(value, path):
    if value is None:
        return None
    if type(value) is not str or not value or value.casefold() == "none":
        _fail(path, "Expected a nonempty name or null")
    return value


def _optional_reference(value, path):
    if value is None:
        return None
    if type(value) is not str or not value.startswith("/") or ":" in value:
        _fail(path, "Expected an absolute asset path or null")
    leaf = value.rsplit("/", 1)[-1]
    if not leaf:
        _fail(path, "Asset path must name an object or package")
    return value if "." in leaf else value + "." + leaf


def _slot_index(value, path):
    # INDEX_NONE (-1) is a valid native "no sync slot" value and must round-trip.
    if type(value) is not int or not -1 <= value <= 0x7fffffff:
        _fail(path, "Expected -1 (none) or a nonnegative integer")
    return value


def _trigger_time(value, path):
    result = _float32(value, path)
    if result < 0 and result != -1:
        _fail(path, "BlendOutTriggerTime must be -1 (default) or nonnegative")
    return result


_REGIONS = {
    "Preview": {"PreviewMesh": _optional_reference, "PreviewBasePose": _optional_reference},
    "Blend": {
        "BlendInTime": lambda v, p: _float32(v, p, minimum=0),
        "BlendOutTime": lambda v, p: _float32(v, p, minimum=0),
        "BlendInOption": _choice(ALPHA_BLEND_OPTIONS),
        "BlendOutOption": _choice(ALPHA_BLEND_OPTIONS),
        "BlendInCustomCurve": _optional_reference,
        "BlendOutCustomCurve": _optional_reference,
        "BlendModeIn": _choice(BLEND_MODES),
        "BlendModeOut": _choice(BLEND_MODES),
        "BlendOutTriggerTime": _trigger_time,
        "bEnableAutoBlendOut": _bool,
        "BlendProfileIn": _optional_name,
        "BlendProfileOut": _optional_name,
    },
    "Sync": {"SyncGroup": _fname, "SyncSlotIndex": _slot_index},
    "RootMotion": {
        "bEnableRootMotionTranslation": _bool,
        "bEnableRootMotionRotation": _bool,
        "RootMotionRootLock": _choice(ROOT_LOCKS),
    },
    "TimeStretch": {
        "TimeStretchCurveName": _fname,
        "SamplingRate": lambda v, p: _float32(v, p, minimum=0, exclusive=True),
        "CurveValueMinPrecision": lambda v, p: _float32(v, p, minimum=0),
    },
}


def _metadata_object(value, path):
    if type(value) is not dict or set(value) != {"Class", "Properties"}:
        _fail(path, "Metadata rows require exactly Class and Properties")
    cls = value["Class"]
    if type(cls) is not str or not cls.startswith("/") or ":" in cls or "." not in cls.rsplit("/", 1)[-1]:
        _fail(path + "/Class", "Metadata Class requires an explicit class object path")
    if type(value["Properties"]) is not dict:
        _fail(path + "/Properties", "Metadata Properties must be an object")
    DocumentCodec().validate(value["Properties"])
    return {"Class": cls, "Properties": deepcopy(value["Properties"])}


def _metadata_array(value, path):
    if type(value) is not list:
        _fail(path, "Metadata must be an array; [] explicitly clears")
    return [_metadata_object(row, f"{path}/{i}") for i, row in enumerate(value)]


def normalize_settings(body: dict) -> dict:
    """Validate and normalize the authored settings regions (sparse fields)."""
    result = {}
    for region in SETTINGS_BODY_KEYS:
        if region not in body:
            continue
        value, path = body[region], "/Body/" + region
        if region == "Metadata":
            result[region] = _metadata_array(value, path)
            continue
        if region == "SectionMetadata":
            if type(value) is not dict:
                _fail(path, "SectionMetadata must map section names to metadata arrays")
            result[region] = {}
            for name in value:
                if type(name) is not str or not name or name.casefold() == "none":
                    _fail(path, "SectionMetadata keys must be section names")
                result[region][name] = _metadata_array(value[name], f"{path}/{_escape(name)}")
            continue
        if type(value) is not dict:
            _fail(path, f"Body.{region} must be an object")
        fields = _REGIONS[region]
        normalized = {}
        for name in value:
            if name not in fields:
                _fail(f"{path}/{_escape(str(name))}", f"Body.{region}.{name} is not a supported field",
                      "AnimMontage.UnsupportedAuthoredField")
            normalized[name] = fields[name](value[name], f"{path}/{_escape(name)}")
        result[region] = normalized
    return result


def validate_settings_against_body(settings: dict, body: dict) -> None:
    """Cross-region checks that only need the authored document."""
    slots = body.get("SlotAnimTracks")
    sync = settings.get("Sync", {})
    if type(slots) is list and "SyncSlotIndex" in sync and slots and sync["SyncSlotIndex"] >= len(slots):
        _fail("/Body/Sync/SyncSlotIndex", "SyncSlotIndex must name an authored slot track")


def validate_time_stretch_curve(settings: dict, body: dict, native_name=None) -> None:
    """A changed TimeStretchCurveName must name an authored float curve.

    Keeping the current native name is always allowed: UE's default name
    (MontageTimeStretchCurve) usually names no curve, which natively just
    means "no time stretch", and extract -> apply must round-trip it.
    """
    curves = body.get("Curves")
    name = settings.get("TimeStretch", {}).get("TimeStretchCurveName")
    if type(curves) is list and name not in (None, "None", native_name):
        if not any(type(row) is dict and row.get("Name") == name for row in curves):
            _fail("/Body/TimeStretch/TimeStretchCurveName", "TimeStretchCurveName must name an authored float curve",
                  "AnimMontage.MissingTimeStretchCurve")


def settings_snapshot_to_body(settings: object) -> tuple[dict, dict]:
    """Project the native settings observation to (complete canonical regions, Properties).

    Properties holds every other plain admitted reflected field of the Montage
    (e.g. RateScale); fields owned by a Body region are not repeated there.
    """
    path = "/Snapshot/settings"
    expected = {"blend_in", "blend_out", "blend_mode_in", "blend_mode_out", "blend_out_trigger_time",
                "enable_auto_blend_out", "blend_profile_in", "blend_profile_out", "sync_group",
                "sync_slot_index", "root_motion", "preview_base_pose", "time_stretch",
                "metadata", "section_metadata", "properties"}
    if type(settings) is not dict or set(settings) != expected:
        _fail(path, "Expected the exact native settings object", "AnimMontage.InvalidCanonicalShape")
    for key, fields in (("blend_in", {"time", "option", "custom_curve"}), ("blend_out", {"time", "option", "custom_curve"}),
                        ("root_motion", {"translation", "rotation", "root_lock"}),
                        ("time_stretch", {"curve_name", "sampling_rate", "precision"})):
        if type(settings[key]) is not dict or set(settings[key]) != fields:
            _fail(f"{path}/{key}", "Unexpected native settings shape", "AnimMontage.InvalidCanonicalShape")
    blend_in, blend_out = settings["blend_in"], settings["blend_out"]

    def metadata(rows, row_path):
        if type(rows) is not list:
            _fail(row_path, "Expected metadata rows", "AnimMontage.InvalidCanonicalShape")
        out = []
        for i, row in enumerate(rows):
            if type(row) is not dict or set(row) != {"class", "properties"}:
                _fail(f"{row_path}/{i}", "Expected class/properties", "AnimMontage.InvalidCanonicalShape")
            out.append({"Class": row["class"], "Properties": deepcopy(row["properties"])})
        return out

    sections = {}
    if type(settings["section_metadata"]) is not list:
        _fail(path + "/section_metadata", "Expected section rows", "AnimMontage.InvalidCanonicalShape")
    for i, row in enumerate(settings["section_metadata"]):
        if type(row) is not dict or set(row) != {"section", "metadata"}:
            _fail(f"{path}/section_metadata/{i}", "Expected section/metadata", "AnimMontage.InvalidCanonicalShape")
        sections[row["section"]] = metadata(row["metadata"], f"{path}/section_metadata/{i}/metadata")
    body = {
        "Preview": {"PreviewBasePose": settings["preview_base_pose"]},
        "Blend": {
            "BlendInTime": blend_in["time"], "BlendOutTime": blend_out["time"],
            "BlendInOption": blend_in["option"], "BlendOutOption": blend_out["option"],
            "BlendInCustomCurve": blend_in["custom_curve"], "BlendOutCustomCurve": blend_out["custom_curve"],
            "BlendModeIn": settings["blend_mode_in"], "BlendModeOut": settings["blend_mode_out"],
            "BlendOutTriggerTime": settings["blend_out_trigger_time"],
            "bEnableAutoBlendOut": settings["enable_auto_blend_out"],
            "BlendProfileIn": settings["blend_profile_in"], "BlendProfileOut": settings["blend_profile_out"],
        },
        "Sync": {"SyncGroup": settings["sync_group"], "SyncSlotIndex": settings["sync_slot_index"]},
        "RootMotion": {
            "bEnableRootMotionTranslation": settings["root_motion"]["translation"],
            "bEnableRootMotionRotation": settings["root_motion"]["rotation"],
            "RootMotionRootLock": settings["root_motion"]["root_lock"],
        },
        "TimeStretch": {
            "TimeStretchCurveName": settings["time_stretch"]["curve_name"],
            "SamplingRate": settings["time_stretch"]["sampling_rate"],
            "CurveValueMinPrecision": settings["time_stretch"]["precision"],
        },
        "Metadata": metadata(settings["metadata"], path + "/metadata"),
        "SectionMetadata": sections,
    }
    properties = settings["properties"]
    if type(properties) is not dict:
        _fail(path + "/properties", "Expected admitted property values", "AnimMontage.InvalidCanonicalShape")
    properties = {name: deepcopy(value) for name, value in properties.items() if name not in MANAGED_PROPERTY_NAMES}
    return normalize_settings(body), properties


# Plain reflected UAnimMontage fields owned by a Body region and written by the
# generic Editor property batch: (Body region, field) -> reflected property name.
_EDITOR_FIELDS = {
    ("Blend", "BlendModeIn"): "BlendModeIn",
    ("Blend", "BlendModeOut"): "BlendModeOut",
    ("Blend", "BlendOutTriggerTime"): "BlendOutTriggerTime",
    ("Blend", "bEnableAutoBlendOut"): "bEnableAutoBlendOut",
    ("Sync", "SyncGroup"): "SyncGroup",
    ("Sync", "SyncSlotIndex"): "SyncSlotIndex",
    ("TimeStretch", "TimeStretchCurveName"): "TimeStretchCurveName",
}
MANAGED_PROPERTY_NAMES = frozenset(_EDITOR_FIELDS.values())


def validate_properties(properties: object) -> dict:
    """Properties author other admitted reflected fields; Body-owned ones are rejected."""
    if type(properties) is not dict:
        _fail("/Properties", "Properties must be an exact object", "AnimMontage.InvalidPropertiesType")
    DocumentCodec().validate(properties)
    for name in sorted(properties):
        if name in MANAGED_PROPERTY_NAMES:
            _fail(f"/Properties/{_escape(name)}", f"{name} is owned by its AnimMontage Body region",
                  "AnimMontage.ManagedPropertyOverlap")
    return deepcopy(properties)


def lower_settings(settings: dict) -> tuple[dict, dict | None]:
    """Return (Body-owned editor property values by name, sparse settings intent or None)."""
    fields = {}
    for (region, field), name in _EDITOR_FIELDS.items():
        if field in settings.get(region, {}):
            fields[name] = settings[region][field]
    intent = {}
    blend = settings.get("Blend", {})
    for side in ("In", "Out"):
        sparse = {native: blend[f"Blend{side}{field}"] for field, native in
                  (("Time", "time"), ("Option", "option"), ("CustomCurve", "custom_curve"))
                  if f"Blend{side}{field}" in blend}
        if sparse:
            if "time" in sparse:
                sparse["time"] = float(sparse["time"])
            intent["blend_" + side.lower()] = sparse
        if f"BlendProfile{side}" in blend:
            intent["blend_profile_" + side.lower()] = blend[f"BlendProfile{side}"]
    root = settings.get("RootMotion", {})
    sparse = {native: root[field] for field, native in (("bEnableRootMotionTranslation", "translation"),
              ("bEnableRootMotionRotation", "rotation"), ("RootMotionRootLock", "root_lock")) if field in root}
    if sparse:
        intent["root_motion"] = sparse
    if "PreviewBasePose" in settings.get("Preview", {}):
        intent["preview_base_pose"] = settings["Preview"]["PreviewBasePose"]
    stretch = settings.get("TimeStretch", {})
    sparse = {native: float(stretch[field]) for field, native in (("SamplingRate", "sampling_rate"),
              ("CurveValueMinPrecision", "precision")) if field in stretch}
    if sparse:
        intent["time_stretch"] = sparse
    if "Metadata" in settings:
        intent["metadata"] = [{"class": row["Class"], "properties": deepcopy(row["Properties"])}
                              for row in settings["Metadata"]]
    if "SectionMetadata" in settings:
        intent["section_metadata"] = [
            {"section": name, "metadata": [{"class": row["Class"], "properties": deepcopy(row["Properties"])}
                                           for row in settings["SectionMetadata"][name]]}
            for name in sorted(settings["SectionMetadata"])]
    return fields, intent or None


def references(settings: dict) -> list[tuple[str, str]]:
    """Authored asset references with their expected classes, for dependency checks."""
    result = []
    preview = settings.get("Preview", {})
    if preview.get("PreviewBasePose") is not None:
        result.append((preview["PreviewBasePose"], "/Script/Engine.AnimSequence"))
    blend = settings.get("Blend", {})
    for side in ("In", "Out"):
        curve = blend.get(f"Blend{side}CustomCurve")
        if curve is not None:
            result.append((curve, "/Script/Engine.CurveFloat"))
    return result


def _metadata_differs(expected: list, actual: object) -> bool:
    if type(actual) is not list or len(actual) != len(expected):
        return True
    for wanted, observed in zip(expected, actual):
        if type(observed) is not dict or observed.get("Class") != wanted["Class"]:
            return True
        props = observed.get("Properties", {})
        if any(name not in props or props[name] != value for name, value in wanted["Properties"].items()):
            return True
    return False


def changed_settings_regions(expected: dict, actual: dict) -> list[str]:
    """Regions whose authored (sparse) content differs from the observation.

    Metadata objects compare only authored properties, like notify objects.
    """
    changed = []
    for region in SETTINGS_BODY_KEYS:
        if region not in expected:
            continue
        wanted, observed = expected[region], actual.get(region)
        if region == "Preview":
            # Preview.PreviewMesh is the PreviewMesh alias, compared on its own key.
            wanted = {name: value for name, value in wanted.items() if name != "PreviewMesh"}
        if region == "Metadata":
            if _metadata_differs(wanted, observed):
                changed.append(region)
        elif region == "SectionMetadata":
            if type(observed) is not dict or any(
                    _metadata_differs(rows, observed.get(name)) for name, rows in wanted.items()):
                changed.append(region)
        elif type(observed) is not dict or any(
                name not in observed or observed[name] != value for name, value in wanted.items()):
            changed.append(region)
    return changed


def _values_equal(expected, actual) -> bool:
    numeric = (int, float)
    if type(expected) in numeric and type(actual) in numeric and float in (type(expected), type(actual)):
        try:
            return struct.pack("!f", expected) == struct.pack("!f", actual)
        except (OverflowError, struct.error):
            return expected == actual
    if type(expected) is dict:
        return type(actual) is dict and set(expected) == set(actual) and all(
            _values_equal(value, actual[name]) for name, value in expected.items())
    if type(expected) is list:
        return type(actual) is list and len(expected) == len(actual) and all(
            _values_equal(a, b) for a, b in zip(expected, actual))
    return type(expected) is type(actual) and expected == actual


def properties_differ(expected: dict, actual: object) -> bool:
    """Authored Properties are sparse; stored floats compare at float32 precision."""
    if type(actual) is not dict:
        return bool(expected)
    return any(name not in actual or not _values_equal(value, actual[name]) for name, value in expected.items())
