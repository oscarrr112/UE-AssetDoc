from __future__ import annotations

import math

from asset_document.diagnostics import contract_error
from asset_document.dto import JsonValue
from asset_document.read_only_extract import admits_read_only
from asset_document.regions.fragment_semantics import _is_asset_path
from asset_document.regions.primitive_catalog import (
    GenericPrimitiveContext,
    NamedOpaqueHandleArgument,
    OpaqueHandleProxy,
    _invoke_required,
)


_EDITOR_INSPECT_ID = "editor.property.inspect_evidence.v1"
_AI_INSPECT_ID = "ai.behavior_tree.selector_inspect_evidence.v1"

NATIVE_REASON_TO_PUBLIC = {
    "NonEditable": "non-editable",
    "Transient": "transient",
    # Flagged transient, but the class saves the value itself (SkeletalMesh.Materials: the
    # mesh's material slots, listed in the inspect facts' MaterialSlots).
    "SavedByAsset": "saved-by-asset",
    "DuplicateTransient": "duplicate-transient",
    "Deprecated": "deprecated",
    "SkipSerialization": "skip-serialization",
    "ReadDenied": "unsupported-serialization",
    "UnsupportedCodec": "unsupported-serialization",
    "CurrentReadFailed": "unsupported-serialization",
    "DefaultReadFailed": "unsupported-serialization",
    "EncodeFailed": "unsupported-serialization",
    # A non-instanced reference to an object inside the asset, created by the
    # asset's own code (e.g. PaperSprite.BodySetup): not authorable.
    "EditorDerivedReference": "editor-derived-reference",
}


_CODEC_TOKENS = {
    "bool.v1": "Bool",
    "integer.v1": "Int",
    "number.v1": "Float",
    "enum.v1": "Enum",
    "string.v1": "String",
    "name.v1": "Name",
    "text.v1": "Text",
    "object_path.v1": "Object",
    "soft_object_path.v1": "Object",
    "class_path.v1": "Class",
    "soft_class_path.v1": "Class",
    "array.v1": "Array",
    "fixed_array.v1": "Array",
    "string_key_map.v1": "Map",
    "map.v1": "Map",
    "set.v1": "Set",
    "instanced_object.v1": "InstancedObject",
    # A plain reference with EditInline metadata: the asset's own object in the
    # instanced form, another asset by its path.
    "edit_inline_object.v1": "EditInlineObject",
    "instanced_struct.v1": "InstancedStruct",
}

# Codecs whose type token is the UE C++ struct type itself.
_STRUCT_CODECS = frozenset({"struct.v1", "struct_text.v1"})

_CODEC_VALUE_TYPES = {
    "bool.v1": (bool,),
    "integer.v1": (int,),
    "number.v1": (int, float),
    "enum.v1": (str,),
    "string.v1": (str,),
    "name.v1": (str,),
    "text.v1": (str,),
    "object_path.v1": (type(None), str),
    "soft_object_path.v1": (type(None), str),
    "class_path.v1": (type(None), str),
    "soft_class_path.v1": (type(None), str),
    "array.v1": (list,),
    "fixed_array.v1": (list,),
    "set.v1": (list,),
    "string_key_map.v1": (dict,),
    "map.v1": (dict,),
    "struct.v1": (dict,),
    "struct_text.v1": (str,),
    "instanced_object.v1": (type(None), dict),
    "edit_inline_object.v1": (type(None), str, dict),
    "instanced_struct.v1": (type(None), dict),
}


def _invalid(path: str, message: str):
    return contract_error(path, "GenericRuntime.InvalidNativeProjection", message)


def _exact_dict(value: object, keys: set[str], path: str) -> dict:
    if type(value) is not dict or set(value) != keys:
        raise _invalid(path, "native inspection DTO has the wrong exact key set")
    return value


def _exact_list(value: object, path: str) -> list:
    if type(value) is not list:
        raise _invalid(path, "native inspection rows must be exact lists")
    return value


def _nonempty_string(value: object, path: str) -> str:
    if type(value) is not str or not value:
        raise _invalid(path, "native inspection string is not a non-empty exact string")
    return value


def _finite_json_number(value: float, path: str) -> float:
    if not math.isfinite(value):
        raise _invalid(path, "inspection values must be finite JSON numbers")
    return value


def _json_list(value: list, path: str) -> list[JsonValue]:
    return [_json(item, f"{path}/{index}") for index, item in enumerate(value)]


def _json_object(value: dict, path: str) -> dict[str, JsonValue]:
    if any(type(key) is not str for key in value):
        raise _invalid(path, "inspection object keys must be exact strings")
    return {key: _json(item, f"{path}/{key}") for key, item in value.items()}


def _json(value: object, path: str) -> JsonValue:
    if value is None or type(value) in (bool, int, str):
        return value
    if type(value) is float:
        return _finite_json_number(value, path)
    if type(value) is list:
        return _json_list(value, path)
    if type(value) is dict:
        return _json_object(value, path)
    raise _invalid(path, "inspection value is not exact JSON")


def _subject(value: object, expected: dict[str, str], path: str) -> None:
    row = _exact_dict(value, set(expected), path)
    for key, expected_value in expected.items():
        if row[key] != expected_value or type(row[key]) is not str:
            raise _invalid(path, "native inspection subject differs from the request")


def _validate_subject_request(mode: str, exact_class_path: str) -> None:
    if type(mode) is not str or type(exact_class_path) is not str or not exact_class_path:
        raise _invalid("/Request", "inspection request has an invalid canonical subject")


def _class_default_subject(mode: str, class_path: str, asset_path: str | None) -> dict[str, str]:
    if asset_path is None:
        return {"kind": mode, "exact_class_path": class_path}
    raise _invalid("/Request", "inspection request mode and canonical subject disagree")


def _live_subject(mode: str, class_path: str, asset_path: str | None) -> dict[str, str]:
    if type(asset_path) is str and (_is_asset_path(asset_path) or admits_read_only(asset_path.rsplit(".", 1)[0])):
        return {"kind": mode, "exact_class_path": class_path, "asset_path": asset_path}
    raise _invalid("/Request", "inspection request mode and canonical subject disagree")


def _subject_for(mode: str, exact_class_path: str, exact_asset_object_path: str | None) -> dict[str, str]:
    _validate_subject_request(mode, exact_class_path)
    if mode == "class_defaults":
        return _class_default_subject(mode, exact_class_path, exact_asset_object_path)
    if mode == "live_asset":
        return _live_subject(mode, exact_class_path, exact_asset_object_path)
    raise _invalid("/Request", "inspection request mode and canonical subject disagree")


def _value_for_codec(codec_id: str, value: object, path: str) -> JsonValue:
    json_value = _json(value, path)
    expected_types = _CODEC_VALUE_TYPES.get(codec_id, ())
    if type(json_value) in expected_types:
        return json_value
    raise _invalid(path, f"inspection codec {codec_id} does not match its value")


def _sort_set(value: JsonValue) -> JsonValue:
    if type(value) is not list:
        return value
    return sorted(value, key=lambda item: _canonical_value(item))


def _canonical_value(value: JsonValue) -> tuple:
    if value is None:
        return (0, "")
    if type(value) is bool:
        return (1, value)
    if type(value) in (int, float):
        return (2, value)
    if type(value) is str:
        return (3, value)
    if type(value) is list:
        return (4, tuple(_canonical_value(item) for item in value))
    return (5, tuple((key, _canonical_value(item)) for key, item in sorted(value.items())))


def _property_row(value: object, path: str) -> dict[str, JsonValue]:
    row = _exact_dict(value, {"name", "declaring_class", "ue_cpp_type", "codec_id", "writable", "current_value", "default_value"}, path)
    name = _nonempty_string(row["name"], f"{path}/name")
    _nonempty_string(row["declaring_class"], f"{path}/declaring_class")
    ue_type = _nonempty_string(row["ue_cpp_type"], f"{path}/ue_cpp_type")
    codec_id = _nonempty_string(row["codec_id"], f"{path}/codec_id")
    if type(row["writable"]) is not bool or codec_id not in set(_CODEC_TOKENS) | _STRUCT_CODECS:
        raise _invalid(path, "native admitted property row is invalid")
    current = _value_for_codec(codec_id, row["current_value"], f"{path}/current_value")
    default = _value_for_codec(codec_id, row["default_value"], f"{path}/default_value")
    if codec_id == "set.v1":
        current, default = _sort_set(current), _sort_set(default)
    return {
        "name": name,
        "ue_type": ue_type,
        "type_token": ue_type if codec_id in _STRUCT_CODECS else _CODEC_TOKENS[codec_id],
        "current_value": current,
        "default_value": default,
        "writable": row["writable"],
    }


def _skipped_row(value: object, path: str) -> dict[str, str]:
    row = _exact_dict(value, {"name", "declaring_class", "ue_cpp_type", "reason_code"}, path)
    name = _nonempty_string(row["name"], f"{path}/name")
    _nonempty_string(row["declaring_class"], f"{path}/declaring_class")
    _nonempty_string(row["ue_cpp_type"], f"{path}/ue_cpp_type")
    reason = _nonempty_string(row["reason_code"], f"{path}/reason_code")
    if reason not in NATIVE_REASON_TO_PUBLIC:
        raise _invalid(f"{path}/reason_code", "native rejection reason is unknown")
    return {"name": name, "reason": NATIVE_REASON_TO_PUBLIC[reason]}


def _optional_exact_dict(value: object, required: set[str], optional: set[str], path: str) -> dict:
    if type(value) is not dict or not required <= set(value) <= required | optional:
        raise _invalid(path, "native inspection DTO has the wrong exact key set")
    return value


def _selector_allowed_type(value: object, path: str) -> dict[str, str | None]:
    row = _optional_exact_dict(value, {"filter_type_path"}, {"base_class_path"}, path)
    base_class = row.get("base_class_path")
    if base_class is not None and (type(base_class) is not str or not base_class):
        raise _invalid(f"{path}/base_class_path", "selector base class is invalid")
    return {
        "filter_type": _nonempty_string(row["filter_type_path"], f"{path}/filter_type_path"),
        "base_class": base_class,
    }


def _selector_allowed_types(value: object, path: str) -> list[dict[str, str | None]]:
    rows = _exact_list(value, path)
    return [_selector_allowed_type(item, f"{path}/{index}") for index, item in enumerate(rows)]


def _selector(value: object, path: str) -> dict[str, JsonValue]:
    row = _exact_dict(value, {"property_path", "authored_field_name", "read_only", "none_allowed", "allowed_types"}, path)
    if type(row["read_only"]) is not bool or type(row["none_allowed"]) is not bool:
        raise _invalid(path, "selector booleans must be exact booleans")
    return {
        "path": _nonempty_string(row["property_path"], f"{path}/property_path"),
        "authored_field": _nonempty_string(row["authored_field_name"], f"{path}/authored_field_name"),
        "read_only": row["read_only"],
        "none_allowed": row["none_allowed"],
        "allowed_types": sorted(_selector_allowed_types(row["allowed_types"], f"{path}/allowed_types"), key=lambda item: (item["filter_type"], item["base_class"] or "")),
    }


def _validate_editor_handle(mode: str, editor_asset_handle: OpaqueHandleProxy | None) -> None:
    if (mode == "class_defaults" and editor_asset_handle is not None) or (mode == "live_asset" and editor_asset_handle is None):
        raise _invalid("/Request", "inspection mode and Editor handle presence disagree")


def _invoke_inspection_evidence(mode: str, class_path: str, asset_path: str | None, editor_asset_handle: OpaqueHandleProxy | None, context: GenericPrimitiveContext, declared: tuple[str, ...] = ()):
    editor_handles = () if editor_asset_handle is None else (NamedOpaqueHandleArgument("asset", editor_asset_handle),)
    editor_values: dict[str, JsonValue] = {"mode": mode, "exact_asset_class_path": class_path}
    if declared:
        editor_values["admit_properties"] = list(declared)
    editor = _invoke_required(context, _EDITOR_INSPECT_ID, values=editor_values, handles=editor_handles)
    ai_values = {"mode": mode, "exact_asset_class_path": class_path}
    if asset_path is not None:
        ai_values["exact_asset_object_path"] = asset_path
    return editor, _invoke_required(context, _AI_INSPECT_ID, values=ai_values)


def _validate_evidence_lanes(value: object, path: str, owner: str) -> None:
    if value.handle_results or value.changed_set or value.diagnostics:
        raise _invalid(path, f"{owner} evidence must not expose side-effect lanes")


def _inspection_rows(editor: object, ai: object, expected: dict[str, str]) -> tuple[list, list, list]:
    _validate_evidence_lanes(editor, "/EditorEvidence", "Editor")
    _validate_evidence_lanes(ai, "/AiEvidence", "Ai")
    editor_value = _exact_dict(editor.value, {"subject", "admitted", "rejected"}, "/EditorEvidence")
    ai_value = _exact_dict(ai.value, {"subject", "selectors"}, "/AiEvidence")
    _subject(editor_value["subject"], expected, "/EditorEvidence/subject")
    _subject(ai_value["subject"], expected, "/AiEvidence/subject")
    return (
        _exact_list(editor_value["admitted"], "/EditorEvidence/admitted"),
        _exact_list(editor_value["rejected"], "/EditorEvidence/rejected"),
        _exact_list(ai_value["selectors"], "/AiEvidence/selectors"),
    )


def _property_rows(rows: list) -> list[tuple[dict[str, JsonValue], object]]:
    return [(_property_row(item, f"/EditorEvidence/admitted/{index}"), item["declaring_class"]) for index, item in enumerate(rows)]


def _class_defaults_match(properties: list[tuple[dict[str, JsonValue], object]]) -> None:
    if any(item[0]["current_value"] != item[0]["default_value"] for item in properties):
        raise _invalid("/EditorEvidence/admitted", "class-default evidence must equal the default value")


def _skipped_rows(rows: list) -> list[tuple[dict[str, str], object]]:
    return [(_skipped_row(item, f"/EditorEvidence/rejected/{index}"), item["declaring_class"]) for index, item in enumerate(rows)]


def _inspection_result(class_path: str, asset_path: str | None, properties: list, skipped: list, selectors: list) -> dict[str, JsonValue]:
    result: dict[str, JsonValue] = {
        "class": class_path,
        "properties": [item[0] for item in sorted(properties, key=lambda item: (item[0]["name"], item[1]))],
        "skipped": [item[0] for item in sorted(skipped, key=lambda item: (item[0]["name"], item[1]))],
        "selector_policies": sorted(selectors, key=lambda item: item["path"]),
    }
    if asset_path is not None:
        result["asset_path"] = asset_path
    return result


class InspectionSemanticAdapter:
    """Inspection evidence; an optional property adapter adds its declared
    properties and shows codec-backed values in their document form."""

    __slots__ = ("_properties",)

    def __init__(self, properties: object | None = None) -> None:
        self._properties = properties

    def _document_values(self, properties: list) -> list:
        if self._properties is None:
            return properties
        converted = []
        for row, declaring_class in properties:
            row = dict(row)
            row["current_value"] = self._properties.to_document(row["name"], row["current_value"])
            row["default_value"] = self._properties.to_document(row["name"], row["default_value"])
            converted.append((row, declaring_class))
        return converted

    def inspect(
        self,
        *,
        mode: str,
        exact_class_path: str,
        exact_asset_object_path: str | None,
        editor_asset_handle: OpaqueHandleProxy | None,
        context: GenericPrimitiveContext,
    ) -> dict[str, JsonValue]:
        expected = _subject_for(mode, exact_class_path, exact_asset_object_path)
        _validate_editor_handle(mode, editor_asset_handle)
        declared = () if self._properties is None else self._properties.declared_names
        editor, ai = _invoke_inspection_evidence(mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context, declared)
        admitted, rejected, selector_rows = _inspection_rows(editor, ai, expected)
        properties = _property_rows(admitted)
        if mode == "class_defaults":
            _class_defaults_match(properties)
        properties = self._document_values(properties)
        skipped = _skipped_rows(rejected)
        selectors = [_selector(item, f"/AiEvidence/selectors/{index}") for index, item in enumerate(selector_rows)]
        return _inspection_result(exact_class_path, exact_asset_object_path, properties, skipped, selectors)
