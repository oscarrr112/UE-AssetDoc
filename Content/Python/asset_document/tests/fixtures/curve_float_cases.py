"""CurveFloat documents for the generic factory lifecycle.

``/Script/Engine.CurveFloat`` is created by the editor's ``UCurveFloatFactory``
through ``factory_generic.v1``. Its curve data (``FloatCurve``,
``bIsEventCurve``) is ``UPROPERTY()`` without ``CPF_Edit``; the Python-only
``profiles.curve_float`` component declares both through the generic property
admission and maps ``FloatCurve`` to a readable key list.
"""
from __future__ import annotations

CURVE_FLOAT_CLASS = "/Script/Engine.CurveFloat"
CURVE_FLOAT_TARGET = "/Game/Curves/Curve_Generic"
CURVE_FLOAT_OBJECT_PATH = f"{CURVE_FLOAT_TARGET}.Curve_Generic"


def curve_float_document(properties: dict | None = None, action: str = "Create") -> dict:
    return {
        "SchemaVersion": 1,
        "AssetType": "GenericAsset",
        "Target": CURVE_FLOAT_TARGET,
        "Class": CURVE_FLOAT_CLASS,
        "Action": action,
        "Properties": {} if properties is None else properties,
    }


# Authored document form (native request projection: JSON arrays arrive as
# tuples): readable keys and enum names; omitted key fields take defaults.
CURVE_FLOAT_KEYS = {
    "FloatCurve": {
        "Keys": (
            {"Time": 0, "Value": 0.1, "InterpMode": "Cubic", "TangentMode": "User", "LeaveTangent": 2},
            {"Time": 1.5, "Value": 2.5},
        ),
        "PreInfinityExtrap": "Cycle",
        "DefaultValue": 1,
    },
    "bIsEventCurve": True,
}

# The same curve in canonical document form.
CURVE_FLOAT_CANONICAL_KEYS = {
    "FloatCurve": {
        "Keys": [
            {"Time": 0.0, "Value": 0.1, "InterpMode": "Cubic", "TangentMode": "User",
             "TangentWeightMode": "None", "ArriveTangent": 0.0, "ArriveTangentWeight": 0.0,
             "LeaveTangent": 2.0, "LeaveTangentWeight": 0.0},
            {"Time": 1.5, "Value": 2.5, "InterpMode": "Linear", "TangentMode": "Auto",
             "TangentWeightMode": "None", "ArriveTangent": 0.0, "ArriveTangentWeight": 0.0,
             "LeaveTangent": 0.0, "LeaveTangentWeight": 0.0},
        ],
        "PreInfinityExtrap": "Cycle",
        "PostInfinityExtrap": "Constant",
        "DefaultValue": 1.0,
    },
    "bIsEventCurve": True,
}

# The typed native value the Editor provider writes and reads back (each float as the
# shortest decimal of the stored float32, as the Editor codec exports floats).
CURVE_FLOAT_NATIVE_CURVE = {
    "DefaultValue": 1.0,
    "PreInfinityExtrap": "RCCE_Cycle",
    "PostInfinityExtrap": "RCCE_Constant",
    "Keys": [
        {"InterpMode": "RCIM_Cubic", "TangentMode": "RCTM_User", "TangentWeightMode": "RCTWM_WeightedNone",
         "Time": 0.0, "Value": 0.1, "ArriveTangent": 0.0, "ArriveTangentWeight": 0.0,
         "LeaveTangent": 2.0, "LeaveTangentWeight": 0.0},
        {"InterpMode": "RCIM_Linear", "TangentMode": "RCTM_Auto", "TangentWeightMode": "RCTWM_WeightedNone",
         "Time": 1.5, "Value": 2.5, "ArriveTangent": 0.0, "ArriveTangentWeight": 0.0,
         "LeaveTangent": 0.0, "LeaveTangentWeight": 0.0},
    ],
}
