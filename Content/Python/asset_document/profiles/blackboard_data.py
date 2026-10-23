# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import re
from dataclasses import dataclass

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.diagnostics import AssetDocumentContractError, contract_error
from asset_document.diff.canonical_diff import canonical_diff
from asset_document.dto import ChangedRecord, PrimitiveRequirement, RuntimePhase
from asset_document.meta import (
    ComparisonPolicy,
    DefaultPolicy,
    DeletePolicy,
    DocumentAction,
    IdentityPolicy,
    OmissionPolicy,
    OrderPolicy,
    ProfileMeta,
    RegionMeta,
    RegionOwnership,
)
# Module-level on purpose: the native runtime restores sys.modules between phase
# calls, so an in-call import would load a second copy of the primitive catalog.
from asset_document.profiles.generic import GenericProfileCandidate, omit_inspected_properties
from asset_document.regions.primitive_catalog import GenericPrimitiveContext, NamedOpaqueHandleArgument, OpaqueHandleProxy, PrimitiveOutcome, PrimitiveOutcomeStatus
from asset_document.read_only_extract import admits_read_only


BLACKBOARD_DATA_CLASS = "/Script/AIModule.BlackboardData"
BLACKBOARD_ROUTE_ID = "ai_profile"
_BLACKBOARD_PRIMITIVE_IDS = ("ai.blackboard.entry.readback.v2", "ai.blackboard.parent.readback.v2", "ai.blackboard.local_keys.readback.v2", "ai.blackboard.entry.update_existing.v2", "ai.blackboard.parent.set.v2", "ai.blackboard.entry.add.v2", "ai.blackboard.entry.replace_class.v2", "ai.blackboard.entry.remove.v2", "ai.blackboard.local_keys.reorder.v2")
BLACKBOARD_REQUIRED_PRIMITIVES = tuple(PrimitiveRequirement(primitive_id, 2, BLACKBOARD_ROUTE_ID) for primitive_id in _BLACKBOARD_PRIMITIVE_IDS)
_ENTRY_FIELDS = ("Category", "Description", "KeyTypeClass", "KeyTypeProperties", "Name", "bInstanceSynced")
_ENTRY_ALIASES = frozenset(("Type", "BaseClass", "Enum"))
_LEGACY_KEY_TYPES = frozenset(("Bool", "Class", "Enum", "Float", "Int", "Name", "Object", "Rotator", "String", "Struct", "Vector"))
_REQUIREMENT_BY_ID = {item.primitive_id: item for item in BLACKBOARD_REQUIRED_PRIMITIVES}
_SELF_ACTOR_NAME = "SelfActor"
_SELF_ACTOR_CLASS = "/Script/AIModule.BlackboardKeyType_Object"
_ACTOR_CLASS = "/Script/Engine.Actor"


def _snapshot(value: object):
    codec = DocumentCodec()
    return codec.decode(codec.encode(value))


def _is_target(value: str) -> bool:
    return re.fullmatch(r"/Game/[A-Za-z0-9_/-]+", value) is not None


def _is_object_path(value: object) -> bool:
    if type(value) is not str:
        return False
    return re.fullmatch(r"/Script/[^/.]+\.[^/.]+", value) is not None or re.fullmatch(r"/Game/.+\.[^/.]+", value) is not None


def _is_class_path(value: str) -> bool:
    return type(value) is str and (re.fullmatch(r"/Script/[^/.]+\.[^/.]+", value) is not None or re.fullmatch(r"/Game/.+\.[^/.]+_C", value) is not None)


def _target_object_path(target: str) -> str:
    return f"{target}.{target.rsplit('/', 1)[-1]}"


def _require_target(value: object) -> str:
    if admits_read_only(value):
        return value
    if type(value) is not str or not _is_target(value):
        raise contract_error(
            "/Target",
            "BlackboardData.NonCanonicalTarget",
            "Target must be a canonical /Game package path",
        )
    return value


def _canonical_reference(kind: str, path: str) -> dict:
    return {"Kind": kind, "Path": path}


def _require_reference(value: object, kind: str, path: str, code: str) -> dict:
    if (
        type(value) is not dict
        or set(value) != {"Kind", "Path"}
        or value.get("Kind") != kind
        or type(value.get("Path")) is not str
        or not (_is_class_path(value["Path"]) if kind == "ClassRef" else _is_object_path(value["Path"]))
    ):
        raise contract_error(path, code, f"{kind} must contain an exact canonical Path")
    return _snapshot(value)


def _require_parent(value: object, target: str) -> dict | None:
    if value is None:
        return None
    if (
        type(value) is not dict
        or set(value) != {"Kind", "Path"}
        or value.get("Kind") != "AssetRef"
        or type(value.get("Path")) is not str
        or not _is_object_path(value["Path"])
    ):
        raise contract_error(
            "/Body/Parent",
            "BlackboardData.InvalidParent",
            "Parent must be null or an exact Blackboard AssetRef",
        )
    if value["Path"] == _target_object_path(target):
        raise contract_error(
            "/Body/Parent/Path",
            "BlackboardData.SelfParent",
            "a Blackboard cannot name itself as Parent",
        )
    return _snapshot(value)


def _alias_property(properties: dict, name: str, value: object, path: str) -> None:
    _expect(name not in properties, path, "BlackboardData.AliasConflict", "legacy alias duplicates canonical state")
    properties[name] = value


def _expect(valid: bool, path: str, code: str, message: str) -> None:
    if not valid:
        raise contract_error(path, code, message)


def _legacy_reference(value: object, kind: str, path: str) -> dict:
    if type(value) is str:
        _expect(_is_class_path(value) if kind == "ClassRef" else _is_object_path(value), path, "BlackboardData.InvalidAlias", "legacy reference must be canonical")
        return _canonical_reference(kind, value)
    return _require_reference(value, kind, path, "BlackboardData.InvalidAlias")


def _entry_shape(value: object, path: str) -> dict:
    if type(value) is not dict:
        raise contract_error(path, "BlackboardData.InvalidEntryType", "key entry must be an exact object")
    unknown = sorted(set(value) - (set(_ENTRY_FIELDS) | _ENTRY_ALIASES))
    _expect(not unknown, f"{path}/{unknown[0] if unknown else ''}", "BlackboardData.UnknownEntryField", "entry field is not canonical")
    required = {"Category", "Description", "Name", "bInstanceSynced"}
    missing = sorted(required - set(value))
    _expect(not missing, f"{path}/{missing[0] if missing else ''}", "BlackboardData.MissingEntryField", "entry field is required")
    _expect(all(type(value[field]) is str for field in ("Category", "Description")), path, "BlackboardData.InvalidEntryField", "metadata must be exact strings")
    _expect(all((type(value["Name"]) is str, bool(value["Name"]))), f"{path}/Name", "BlackboardData.InvalidLocalName", "Name must be non-empty")
    _expect(type(value["bInstanceSynced"]) is bool, f"{path}/bInstanceSynced", "BlackboardData.InvalidEntryField", "bInstanceSynced must be exact bool")
    return value


def _entry_class(value: dict, path: str) -> dict:
    has_canonical, has_legacy = "KeyTypeClass" in value, "Type" in value
    canonical_class, legacy_type = value.get("KeyTypeClass"), value.get("Type")
    _expect(any((has_canonical, has_legacy)), f"{path}/KeyTypeClass", "BlackboardData.MissingEntryField", "KeyTypeClass or Type is required")
    _expect(not all((has_canonical, has_legacy)), f"{path}/Type", "BlackboardData.AliasConflict", "Type duplicates KeyTypeClass")
    if has_canonical:
        return _require_reference(canonical_class, "ClassRef", f"{path}/KeyTypeClass", "BlackboardData.InvalidKeyTypeClass")
    _expect(type(legacy_type) is str, f"{path}/Type", "BlackboardData.InvalidKeyTypeClass", "Type must be an exact string")
    class_path = legacy_type if _is_class_path(legacy_type) else f"/Script/AIModule.BlackboardKeyType_{legacy_type}"
    _expect(_is_class_path(legacy_type) or legacy_type in _LEGACY_KEY_TYPES, f"{path}/Type", "BlackboardData.InvalidKeyTypeClass", "Type must be canonical")
    return _canonical_reference("ClassRef", class_path)


def _property_shape(properties: dict, required: tuple[str, ...], path: str) -> None:
    field = next((name for name in required if name not in properties), next(iter(sorted(set(properties) - set(required))), ""))
    _expect(not field, f"{path}/{field}", "BlackboardData.InvalidEntryField", "specialized KeyTypeProperties must be complete and exact")


_DIRECT_VALUE_COMPONENTS = {
    "/Script/AIModule.BlackboardKeyType_Vector": ("X", "Y", "Z"),
    "/Script/AIModule.BlackboardKeyType_Rotator": ("Pitch", "Roll", "Yaw"),
}


def _direct_value_properties(properties: dict, components: tuple[str, ...], path: str) -> dict:
    """Vector/Rotator keys: {"DefaultValue": null} (no default, the engine default) or
    {"DefaultValue": {components}} (default on; omitted components are 0).
    bUseDefaultValue is accepted as input: false means null, true requires/implies a value."""
    unknown = sorted(set(properties) - {"DefaultValue", "bUseDefaultValue"})
    _expect(not unknown, f"{path}/{unknown[0] if unknown else ''}", "BlackboardData.InvalidEntryField", "Vector/Rotator KeyTypeProperties accept only DefaultValue (and the bUseDefaultValue alias)")
    use_default = properties.get("bUseDefaultValue", properties.get("DefaultValue") is not None)
    _expect(type(use_default) is bool, f"{path}/bUseDefaultValue", "BlackboardData.InvalidEntryField", "bUseDefaultValue must be exact bool")
    if not use_default:
        return {"DefaultValue": None}
    default = properties.get("DefaultValue") or {}
    _expect(type(default) is dict, f"{path}/DefaultValue", "BlackboardData.InvalidEntryField", f"DefaultValue must be null or an object with {', '.join(components)}")
    extra = sorted(set(default) - set(components))
    _expect(not extra, f"{path}/DefaultValue/{extra[0] if extra else ''}", "BlackboardData.InvalidEntryField", f"DefaultValue fields are {', '.join(components)}")
    values = {}
    for name in components:
        value = default.get(name, 0.0)
        _expect(type(value) in (int, float) and value == value and value not in (float("inf"), float("-inf")), f"{path}/DefaultValue/{name}", "BlackboardData.InvalidEntryField", "DefaultValue components must be finite numbers")
        values[name] = float(value)
    return {"DefaultValue": values}


# The editable properties of the stock value key types. A key type is created with all of its properties, so
# KeyTypeProperties without one of them (or with another name) cannot be created: the same refusal as the provider's.
_VALUE_KEY_PROPERTIES = {
    "/Script/AIModule.BlackboardKeyType_Bool": ("bDefaultValue",),
    "/Script/AIModule.BlackboardKeyType_Int": ("DefaultValue",),
    "/Script/AIModule.BlackboardKeyType_Float": ("DefaultValue",),
    "/Script/AIModule.BlackboardKeyType_Name": ("DefaultValue",),
    "/Script/AIModule.BlackboardKeyType_String": ("DefaultValue",),
}


def _value_properties(properties: dict, names: tuple[str, ...], class_path: str, path: str) -> dict:
    missing = [name for name in names if name not in properties]
    unknown = sorted(set(properties) - set(names))
    field = (missing or unknown or [""])[0]
    _expect(not field, f"{path}/{field}", "AIProvider.Blackboard.EntryMaterializationUnavailable",
            f"{class_path.rsplit('.', 1)[-1]} KeyTypeProperties must be exactly {{{', '.join(repr(name) for name in names)}}}"
            f" ({'missing' if missing else 'not a property of this key type'}: {field})")
    return properties


def _specialized_properties(properties: dict, key_class: dict, path: str) -> dict:
    class_path = key_class["Path"]
    if class_path in _VALUE_KEY_PROPERTIES:
        return _value_properties(properties, _VALUE_KEY_PROPERTIES[class_path], class_path, path)
    if class_path in _DIRECT_VALUE_COMPONENTS:
        return _direct_value_properties(properties, _DIRECT_VALUE_COMPONENTS[class_path], path)
    if class_path in ("/Script/AIModule.BlackboardKeyType_Object", "/Script/AIModule.BlackboardKeyType_Class"):
        _property_shape(properties, ("BaseClass", "DefaultValue"), path)
        properties["BaseClass"] = _require_reference(properties["BaseClass"], "ClassRef", f"{path}/BaseClass", "BlackboardData.InvalidEntryField")
        if properties["DefaultValue"] is not None: properties["DefaultValue"] = _require_reference(properties["DefaultValue"], "AssetRef" if class_path.endswith("_Object") else "ClassRef", f"{path}/DefaultValue", "BlackboardData.InvalidEntryField")
    elif class_path == "/Script/AIModule.BlackboardKeyType_Enum":
        _property_shape(properties, ("EnumType", "EnumName", "DefaultValue"), path)
        properties["EnumType"] = _require_reference(properties["EnumType"], "AssetRef", f"{path}/EnumType", "BlackboardData.InvalidEntryField")
        _expect(type(properties["DefaultValue"]) is str, f"{path}/DefaultValue", "BlackboardData.InvalidEntryField", "Enum DefaultValue must be an exact string")
        expected_name = "" if properties["EnumType"]["Path"].startswith("/Game/") else properties["EnumType"]["Path"]
        _expect(all((type(properties["EnumName"]) is str, properties["EnumName"] == expected_name)), f"{path}/EnumName", "BlackboardData.InvalidEntryField", "EnumName must match EnumType authority")
    return properties


def _entry_properties(value: dict, key_class: dict, path: str) -> dict:
    raw_properties = value.get("KeyTypeProperties", {})
    _expect(type(raw_properties) is dict, f"{path}/KeyTypeProperties", "BlackboardData.InvalidEntryField", "KeyTypeProperties must be exact object")
    properties = _snapshot(raw_properties)
    if "BaseClass" in value:
        _alias_property(properties, "BaseClass", _legacy_reference(value["BaseClass"], "ClassRef", f"{path}/BaseClass"), f"{path}/BaseClass")
    if "Enum" in value:
        enum = _legacy_reference(value["Enum"], "AssetRef", f"{path}/Enum")
        _alias_property(properties, "EnumName", "" if enum["Path"].startswith("/Game/") else enum["Path"], f"{path}/Enum")
        _alias_property(properties, "EnumType", enum, f"{path}/Enum")
    return _specialized_properties(properties, key_class, f"{path}/KeyTypeProperties")


def _canonical_entry(value: object, index: int) -> dict:
    path = f"/Body/Keys/{index}"
    item = _entry_shape(value, path); key_class = _entry_class(item, path)
    return _snapshot({"Category": item["Category"], "Description": item["Description"],
                      "KeyTypeClass": key_class,
                      "KeyTypeProperties": _entry_properties(item, key_class, path), "Name": item["Name"],
                      "bInstanceSynced": item["bInstanceSynced"]})


@dataclass(frozen=True, slots=True)
class BlackboardSnapshot:
    parent_path_or_null: str | None; entries: tuple[dict, ...]; ordered_names: tuple[str, ...]


@dataclass(frozen=True, slots=True)
class BlackboardTransition:
    kind: str; name: str | None = None; entry: dict | None = None
    index: int | None = None; ordered_names: tuple[str, ...] = ()


def _invalid_snapshot(path: str, message: str):
    return contract_error(path, "BlackboardData.InvalidCurrentSnapshot", message)


def _current_entry(value: object, index: int) -> dict:
    path = f"/Current/Entries/{index}"
    if type(value) is not dict or set(value) != set(_ENTRY_FIELDS):
        raise _invalid_snapshot(path, "entry readback must be exact BlackboardEntryV2")
    try:
        canonical = _canonical_entry(value, index)
    except AssetDocumentContractError:
        raise _invalid_snapshot(path, "entry readback is not canonical") from None
    return canonical


def _snapshot_parent(value: object) -> str | None:
    if type(value) is not dict:
        raise _invalid_snapshot("/Current/Parent", "Parent result is required")
    _expect(set(value) == {"parent_path_or_null"}, "/Current/Parent", "BlackboardData.InvalidCurrentSnapshot", "Parent result is required")
    parent = value["parent_path_or_null"]
    _expect(any((parent is None, all((type(parent) is str, _is_object_path(parent) if type(parent) is str else False)))), "/Current/Parent", "BlackboardData.InvalidCurrentSnapshot", "Parent result is not canonical")
    return parent


def _snapshot_names(value: object) -> list[str]:
    if type(value) is not dict:
        raise _invalid_snapshot("/Current/OrderedLocalKeyNames", "local-key result is required")
    _expect(set(value) == {"ordered_local_key_names"}, "/Current/OrderedLocalKeyNames", "BlackboardData.InvalidCurrentSnapshot", "local-key result is required")
    names = value["ordered_local_key_names"]
    _expect(type(names) is list, "/Current/OrderedLocalKeyNames", "BlackboardData.InvalidCurrentSnapshot", "ordered names must be an array")
    seen: set[str] = set()
    for index, name in enumerate(names):
        _expect(type(name) is str and bool(name) and name.casefold() not in seen, f"/Current/OrderedLocalKeyNames/{index}", "BlackboardData.InvalidCurrentSnapshot", "ordered names must be unique non-empty strings")
        seen.add(name.casefold())
    return names


def _snapshot_entries(values: object, names: list[str]) -> list[dict]:
    _expect(all((type(values) is list, len(values) == len(names) if type(values) is list else False)), "/Current/Entries", "BlackboardData.InvalidCurrentSnapshot", "one entry result is required per ordered name")
    entries: list[dict] = []
    for index, result in enumerate(values):
        _expect(all((type(result) is dict, set(result) == {"entry"} if type(result) is dict else False)), f"/Current/Entries/{index}", "BlackboardData.InvalidCurrentSnapshot", "entry result has the wrong shape")
        canonical = _current_entry(result["entry"], index)
        _expect(canonical["Name"] == names[index], f"/Current/Entries/{index}/Name", "BlackboardData.InvalidCurrentSnapshot", "entry order does not match local names")
        entries.append(canonical)
    return entries


def _is_engine_derived_self_actor(entry: dict) -> bool:
    return (
        entry["Name"] == _SELF_ACTOR_NAME
        and entry["KeyTypeClass"]
        == {"Kind": "ClassRef", "Path": _SELF_ACTOR_CLASS}
        and entry["KeyTypeProperties"]
        == {
            "BaseClass": {"Kind": "ClassRef", "Path": _ACTOR_CLASS},
            "DefaultValue": None,
        }
        and entry["Description"] == ""
        and entry["Category"] == ""
        and entry["bInstanceSynced"] is False
    )


def parse_blackboard_snapshot(
    *,
    parent_result: object,
    local_keys_result: object,
    entry_results: object,
) -> BlackboardSnapshot:
    parent = _snapshot_parent(parent_result)
    names = _snapshot_names(local_keys_result)
    entries = _snapshot_entries(entry_results, names)
    authored_entries: list[dict] = []
    authored_names: list[str] = []
    for entry in entries:
        if _is_engine_derived_self_actor(entry):
            continue
        authored_entries.append(entry)
        authored_names.append(entry["Name"])
    return BlackboardSnapshot(
        parent, tuple(authored_entries), tuple(authored_names)
    )


def _transition_kind(before: dict, after: dict) -> str:
    if before == after:
        return "unchanged"
    if before["KeyTypeClass"] != after["KeyTypeClass"]:
        return "replace"
    # Vector/Rotator key types are never edited in place: any change stages a fresh key type.
    return "replace" if after["KeyTypeClass"]["Path"] in _DIRECT_VALUE_COMPONENTS else "update"


def _existing_transitions(current: list[dict], desired: list[dict], current_map: dict, desired_map: dict) -> list[BlackboardTransition]:
    transitions = [BlackboardTransition("remove", name=item["Name"]) for item in current if item["Name"].casefold() not in desired_map]
    for index, item in enumerate(desired):
        before = current_map.get(item["Name"].casefold())
        if before is not None:
            _expect(before["Name"] == item["Name"], f"/Body/Keys/{index}/Name", "BlackboardData.InvalidLocalName", "authored Name spelling must match current FName identity")
            transitions.append(BlackboardTransition(_transition_kind(before, item), name=item["Name"], entry=_snapshot(item)))
    return transitions


def _addition_transitions(current: list[dict], desired: list[dict], current_map: dict, desired_map: dict) -> tuple[list[BlackboardTransition], list[str]]:
    working = [item["Name"] for item in current if item["Name"].casefold() in desired_map]
    transitions: list[BlackboardTransition] = []
    for index, item in enumerate(desired):
        if item["Name"].casefold() not in current_map:
            transitions.append(BlackboardTransition("add", name=item["Name"], entry=_snapshot(item), index=index))
            working.insert(index, item["Name"])
    return transitions, working


def plan_blackboard_transitions(
    current_entries: object,
    desired_entries: object,
) -> tuple[BlackboardTransition, ...]:
    _expect(all((type(current_entries) is list, type(desired_entries) is list)), "/Transitions", "BlackboardData.InvalidTransitionInput", "entry sets must be exact arrays")
    current = [_current_entry(value, index) for index, value in enumerate(current_entries)]
    desired = [_canonical_entry(value, index) for index, value in enumerate(desired_entries)]
    current_by_name = _unique_entry_map(current, "/Current/Entries")
    desired_by_name = _unique_entry_map(desired, "/Body/Keys")
    desired_names = [item["Name"] for item in desired]
    transitions = _existing_transitions(current, desired, current_by_name, desired_by_name)
    additions, working = _addition_transitions(current, desired, current_by_name, desired_by_name)
    transitions.extend(additions)
    if working != desired_names:
        transitions.append(BlackboardTransition("reorder", ordered_names=tuple(desired_names)))
    return tuple(transitions)


def build_blackboard_footprint_projection(
    *,
    request_id: str,
    runtime_revision: int,
    target_asset_path: str,
    operation: str,
    semantic_hash: str,
    current_parent_path_or_null: str | None,
    desired_parent_path_or_null: str | None,
    transitions: tuple[BlackboardTransition, ...],
) -> dict:
    target = target_asset_path
    keys = f"asset:{target}/Keys[*]"
    steps: list[dict] = []

    def append(primitive_id: str, rows: list[dict]) -> None:
        steps.append({"Index": len(steps), "PrimitiveId": primitive_id, "RawFootprint": rows})

    if current_parent_path_or_null != desired_parent_path_or_null:
        append("ai.blackboard.parent.set.v2", [
            {"Kind": "UObjectState", "StableId": keys, "RollbackComponentId": "assetdocument.ai.blackboard.parent.derived_closure.v2"},
            {"Kind": "UObjectState", "StableId": f"asset:{target}/Parent", "RollbackComponentId": "assetdocument.ai.blackboard.parent.reference.v2"},
        ])
    for transition in transitions:
        if transition.kind == "unchanged":
            continue
        if transition.kind == "update":
            entry = f"asset:{target}/Keys[name={transition.name}]"
            append("ai.blackboard.entry.update_existing.v2", [
                {"Kind": "UObjectState", "StableId": entry + ".LocalKeysMetadataAggregate", "RollbackComponentId": "assetdocument.ai.blackboard.entry_metadata.v2"},
                {"Kind": "UObjectState", "StableId": entry + ".KeyTypeProperties", "RollbackComponentId": "assetdocument.ai.blackboard.key_type_properties.v2"},
            ])
            continue
        primitive_id, component_id = {
            "replace": ("ai.blackboard.entry.replace_class.v2", "assetdocument.ai.blackboard.entry.replace_class.v2"),
            "add": ("ai.blackboard.entry.add.v2", "assetdocument.ai.blackboard.entry.add.v2"),
            "remove": ("ai.blackboard.entry.remove.v2", "assetdocument.ai.blackboard.entry_remove.v2"),
            "reorder": ("ai.blackboard.local_keys.reorder.v2", "assetdocument.ai.blackboard.local_keys_reorder.v2"),
        }[transition.kind]
        append(primitive_id, [{"Kind": "UObjectState", "StableId": keys, "RollbackComponentId": component_id}])
    return _snapshot({
        "Kind": "BlackboardFootprintProjectionV1",
        "RequestId": request_id,
        "RuntimeRevision": runtime_revision,
        "ExactAssetClassPath": BLACKBOARD_DATA_CLASS,
        "TargetAssetPath": target_asset_path,
        "Operation": operation,
        "SemanticHash": semantic_hash,
        "Steps": steps,
    })


def _unique_entry_map(entries: list[dict], path: str) -> dict[str, dict]:
    result: dict[str, dict] = {}
    for index, item in enumerate(entries):
        name = item["Name"].casefold()
        if name in result:
            raise contract_error(f"{path}/{index}/Name", "BlackboardData.DuplicateLocalName", "local Names must be unique")
        result[name] = item
    return result


def _invoke(
    context: GenericPrimitiveContext,
    primitive_id: str,
    value: dict,
    handles: tuple[NamedOpaqueHandleArgument, ...] = (),
) -> PrimitiveOutcome:
    outcome = context.invoke(
        requirement=_REQUIREMENT_BY_ID[primitive_id],
        value_arguments=_snapshot(value),
        handle_arguments=handles,
    )
    _expect(type(outcome) is PrimitiveOutcome, "/PrimitiveOutcome", "BlackboardData.InvalidSuccessEnvelope", "primitive outcome must be exact")
    if outcome.status is not PrimitiveOutcomeStatus.SUCCEEDED:
        if outcome.diagnostics:
            raise AssetDocumentContractError(outcome.diagnostics[0])
        raise contract_error("/PrimitiveOutcome", "BlackboardData.PrimitiveFailure", "provider omitted a diagnostic")
    if outcome.handle_results:
        raise contract_error("/PrimitiveOutcome/HandleResults", "BlackboardData.InvalidSuccessEnvelope", "Blackboard rows return no handles")
    _expect(not outcome.diagnostics, "/PrimitiveOutcome/Diagnostics", "BlackboardData.InvalidSuccessEnvelope", "successful outcome returns no diagnostics")
    return outcome


def _read_value(context: GenericPrimitiveContext, primitive_id: str, value: dict) -> object:
    outcome = _invoke(context, primitive_id, value)
    if outcome.changed_set:
        raise contract_error("/PrimitiveOutcome/ChangedSet", "BlackboardData.InvalidSuccessEnvelope", "readback cannot mutate")
    return outcome.value


def _checked_snapshot(value: object) -> BlackboardSnapshot:
    if type(value) is not BlackboardSnapshot:
        raise _invalid_snapshot("/Current", "exact BlackboardSnapshot is required")
    return parse_blackboard_snapshot(
        parent_result={"parent_path_or_null": value.parent_path_or_null},
        local_keys_result={"ordered_local_key_names": list(value.ordered_names)},
        entry_results=[{"entry": item} for item in value.entries],
    )


def _mutation(
    context: GenericPrimitiveContext,
    primitive_id: str,
    value: dict,
    handle: OpaqueHandleProxy,
    expected_value: dict,
    target: str,
    expected_paths: tuple[str, ...],
) -> tuple[ChangedRecord, ...]:
    outcome = _invoke(context, primitive_id, value, (NamedOpaqueHandleArgument("asset", handle),))
    if outcome.value != _snapshot(expected_value):
        raise contract_error("/PrimitiveOutcome/Value", "BlackboardData.ReadbackMismatch", "full mutation readback differs")
    actual = tuple(item.path for item in outcome.changed_set)
    valid = actual == expected_paths and all(
        item.authority_id == primitive_id
        and item.stable_object_id == target
        and item.change_kind == "Update"
        for item in outcome.changed_set
    )
    if not valid:
        raise contract_error("/PrimitiveOutcome/ChangedSet", "BlackboardData.ChangedSetMismatch", "changed evidence differs")
    return outcome.changed_set


def _entry_change_paths(kind: str, target: str, name: str, before: dict | None, after: dict | None) -> tuple[str, ...]:
    base = f"asset:{target}/Keys[name={name}]"
    order = f"asset:{target}/Keys.@Order"
    if kind in ("add", "remove"):
        return (base, order)
    fields = ("KeyTypeClass", "KeyTypeProperties", "Description", "Category", "bInstanceSynced")
    if kind == "update":
        fields = fields[1:]
    return tuple(
        f"{base}.{field}"
        for field in fields
        if before is None
        or after is None
        or before[field] != after[field]
        or (kind == "replace" and field == "KeyTypeProperties" and before["KeyTypeClass"] != after["KeyTypeClass"])
    )


def _observe_board(context: GenericPrimitiveContext, board_path: str) -> tuple[object, object, str | None, list[str]]:
    request = {"blackboard_path": board_path}
    parent = _read_value(context, "ai.blackboard.parent.readback.v2", request)
    names = _read_value(context, "ai.blackboard.local_keys.readback.v2", request)
    return parent, names, _snapshot_parent(parent), _snapshot_names(names)


def _record_chain(context: GenericPrimitiveContext, start: str, records: dict, forbidden: str | None = None) -> list[str]:
    visited: set[str] = set()
    route: list[str] = []
    current: str | None = start
    while current is not None:
        _expect(current != forbidden, "/Body/Parent/Path", "BlackboardData.ParentCycle", "Parent chain contains the target")
        _expect(current not in visited, "/Body/Parent/Path", "BlackboardData.ParentCycle", "Parent chain contains a cycle")
        visited.add(current)
        if current not in records:
            records[current] = _observe_board(context, current)
        route.append(current)
        current = records[current][2]
    return route


def _recorded_snapshots(context: GenericPrimitiveContext, records: dict, order: list[str]) -> dict[str, BlackboardSnapshot]:
    snapshots: dict[str, BlackboardSnapshot] = {}
    for board_path in order:
        parent_result, local_keys_result, _, names = records[board_path]
        entry_results = [_read_value(context, "ai.blackboard.entry.readback.v2", {"blackboard_path": board_path, "key_name": name}) for name in names]
        snapshots[board_path] = parse_blackboard_snapshot(parent_result=parent_result, local_keys_result=local_keys_result, entry_results=entry_results)
    return snapshots


def _chain_names(records: dict, route: list[str]) -> set[str]:
    inherited: set[str] = set()
    for board_path in route:
        names = records[board_path][3]
        for name in names:
            _expect(name.casefold() not in inherited, "/Current/InheritedNames", "BlackboardData.InheritedShadow", "Parent chain contains a shadowed Name")
            inherited.add(name.casefold())
    return inherited


def _preflight_observation(context: GenericPrimitiveContext, target: str, desired_parent: str | None, desired_entries: list[dict]) -> BlackboardSnapshot:
    records: dict = {}
    current_route = _record_chain(context, target, records)
    desired_route = [] if desired_parent is None else _record_chain(context, desired_parent, records, target)
    _chain_names(records, current_route)
    inherited = _chain_names(records, desired_route)
    _reject_inherited_shadow(desired_entries, inherited)
    join = next((path for path in desired_route if path in current_route), None)
    prefix = len(current_route) if join is None else current_route.index(join)
    order = current_route[:prefix] + desired_route[:len(desired_route) if join is None else desired_route.index(join)] + current_route[prefix:]
    snapshots = _recorded_snapshots(context, records, order)
    return snapshots[target]


def _reject_inherited_shadow(entries: list[dict], inherited_names: set[str]) -> None:
    for index, item in enumerate(entries):
        if item["Name"].casefold() in inherited_names:
            raise contract_error(f"/Body/Keys/{index}/Name", "BlackboardData.InheritedShadow", "local key shadows Parent")


def _parent_change(context: GenericPrimitiveContext, handle: OpaqueHandleProxy, target: str, before: str | None, after: str | None) -> tuple[ChangedRecord, ...]:
    if before == after:
        return ()
    return _mutation(context, "ai.blackboard.parent.set.v2", {"blackboard_path": target, "parent_path_or_null": after}, handle, {"parent_path_or_null": after}, target, (f"asset:{target}/Parent",))


def _transition_payload(transition: BlackboardTransition, working: list[dict], target: str) -> tuple[list[dict], str, dict, dict, dict | None]:
    before = next((item for item in working if item["Name"] == transition.name), None)
    if transition.kind == "remove":
        working = [item for item in working if item["Name"] != transition.name]
        return working, "ai.blackboard.entry.remove.v2", {"blackboard_path": target, "key_name": transition.name}, {"entries": working, "ordered_local_key_names": [item["Name"] for item in working], "removed_name": transition.name}, before
    if transition.kind in ("update", "replace"):
        index = next(index for index, item in enumerate(working) if item["Name"] == transition.name)
        working[index] = _snapshot(transition.entry)
        primitive = {"update": "ai.blackboard.entry.update_existing.v2", "replace": "ai.blackboard.entry.replace_class.v2"}[transition.kind]
        return working, primitive, {"blackboard_path": target, "entry": transition.entry, "key_name": transition.name}, {"entry": transition.entry, "ordered_local_key_names": [item["Name"] for item in working]}, before
    if transition.kind == "add":
        working.insert(transition.index, _snapshot(transition.entry))
        return working, "ai.blackboard.entry.add.v2", {"blackboard_path": target, "entry": transition.entry, "insertion": {"Kind": "AtIndex", "Index": transition.index}}, {"entry": transition.entry, "ordered_local_key_names": [item["Name"] for item in working]}, before
    by_name = {item["Name"]: item for item in working}
    working = [by_name[name] for name in transition.ordered_names]
    return working, "ai.blackboard.local_keys.reorder.v2", {"blackboard_path": target, "ordered_local_key_names": list(transition.ordered_names)}, {"entries": working, "ordered_local_key_names": list(transition.ordered_names)}, before


def _apply_transition(context: GenericPrimitiveContext, handle: OpaqueHandleProxy, target: str, transition: BlackboardTransition, working: list[dict]) -> tuple[list[dict], tuple[ChangedRecord, ...]]:
    working, primitive, request, expected, before = _transition_payload(transition, working, target)
    paths = (f"asset:{target}/Keys.@Order",) if transition.kind == "reorder" else _entry_change_paths(transition.kind, target, transition.name, before, transition.entry)
    return working, _mutation(context, primitive, request, handle, expected, target, paths)


_BODY_OWNED_PROPERTIES = {"parent": "Body.Parent", "keys": "Body.Keys"}


def _reject_top_level_properties(properties: object) -> None:
    """BlackboardData has no top-level Properties: Parent and Keys are owned by Body and
    nothing else is editable, so any authored name would otherwise be silently dropped."""
    if type(properties) is not dict:
        return
    for name in sorted(properties):
        owner = _BODY_OWNED_PROPERTIES.get(name.casefold()) if type(name) is str else None
        if owner is not None:
            raise contract_error(f"/Properties/{name}", "BodyOwnedProperty",
                                 f"Property '{name}' is owned by {owner} and cannot be authored through top-level Properties")
        raise contract_error(f"/Properties/{name}", "Generic.UnknownProperty", "property is not admitted")


def _document_header(document: object) -> tuple[dict, str]:
    if type(document) is not dict:
        raise contract_error("/", "BlackboardData.InvalidDocumentType", "document must be an exact object")
    _reject_top_level_properties(document.get("Properties", {}))
    allowed = {"SchemaVersion", "Target", "Class", "Action", "Definitions", "Properties", "Body"}
    unknown = sorted(set(document) - allowed)
    _expect(not unknown, f"/{unknown[0] if unknown else ''}", "BlackboardData.UnknownDocumentField", "document field is not canonical")
    _expect(all((type(document.get("SchemaVersion")) is int, document.get("SchemaVersion") == 1)), "/SchemaVersion", "Meta.InvalidSchemaVersion", "SchemaVersion must be exact integer 1")
    target = _require_target(document.get("Target"))
    _expect(all((type(document.get("Class")) is str, document.get("Class") == BLACKBOARD_DATA_CLASS)), "/Class", "BlackboardData.ClassMismatch", "Class must be exact BlackboardData")
    action = document.get("Action")
    _expect(type(action) is str, "/Action", "BlackboardData.InvalidAction", "Action must be a DocumentAction")
    _expect(action in {item.value for item in DocumentAction}, "/Action", "BlackboardData.InvalidAction", "Action must be a DocumentAction")
    for field in ("Definitions", "Properties"):
        _expect(type(document.get(field, {})) is dict, f"/{field}", "BlackboardData.InvalidGenericRegion", f"{field} must be an exact object")
    return document, target


def _canonical_keys(body: object) -> list[dict]:
    if type(body) is dict:
        unknown = sorted(name for name in body if name not in {"Parent", "Keys"})
        _expect(not unknown, f"/Body/{unknown[0] if unknown else ''}", "UnknownBodyKey", "Unknown authored Body region")
    _expect(all((type(body) is dict, set(body) == {"Parent", "Keys"} if type(body) is dict else False)), "/Body", "BlackboardData.InvalidBodyShape", "Body requires exact Parent and Keys")
    _expect(type(body["Keys"]) is list, "/Body/Keys", "BlackboardData.InvalidKeys", "Keys must be an exact array")
    keys: list[dict] = []
    names: set[str] = set()
    for index, raw in enumerate(body["Keys"]):
        canonical = _canonical_entry(raw, index)
        _expect(canonical["Name"].casefold() not in names, f"/Body/Keys/{index}/Name", "BlackboardData.DuplicateLocalName", "local Names must be unique")
        names.add(canonical["Name"].casefold())
        keys.append(canonical)
    return keys


class BlackboardDataProfileCandidate:
    def inspect_profile(self) -> dict:
        return {"Class": BLACKBOARD_DATA_CLASS, "BodySections": ["Parent", "Keys"]}

    def inspect(self, **kwargs) -> dict:
        # Reflected Inspect like any generic asset, minus the Body-owned Parent/Keys rows.
        return omit_inspected_properties(
            GenericProfileCandidate(BLACKBOARD_DATA_CLASS).inspect(**kwargs), ("Parent", "Keys"))

    def required_primitives(self) -> tuple[PrimitiveRequirement, ...]:
        return BLACKBOARD_REQUIRED_PRIMITIVES

    def schema(self) -> dict:
        return _snapshot(
            {
                "SchemaVersion": 1,
                "Class": BLACKBOARD_DATA_CLASS,
                "Type": "object",
                "Required": ["SchemaVersion", "Target", "Class", "Action", "Body"],
                "Properties": {
                    "Body": {
                        "type": "object",
                        "required": ["Parent", "Keys"],
                        "order": "authored-local-key-order",
                        "entry": "BlackboardEntryV2",
                    }
                },
            }
        )

    def create_template(self, target: str) -> dict:
        return _snapshot(
            {
                "SchemaVersion": 1,
                "Target": _require_target(target),
                "Class": BLACKBOARD_DATA_CLASS,
                "Action": DocumentAction.CREATE_OR_UPDATE.value,
                "Definitions": {},
                "Properties": {},
                "Body": {"Parent": None, "Keys": []},
            }
        )

    def validate(self, document: object) -> None:
        self._canonical_document(document)

    def canonicalize(self, document: object) -> dict:
        return self._canonical_document(document)

    def diff(self, authored: object, extracted: object):
        # Action describes the requested operation, not the asset's stored state.
        desired, current = self.canonicalize(authored), self.canonicalize(extracted)
        desired.pop("Action", None)
        current.pop("Action", None)
        return canonical_diff(desired, current)

    def preflight(
        self,
        document: object,
        *,
        context: GenericPrimitiveContext,
        current_snapshot: BlackboardSnapshot | None = None,
    ) -> BlackboardSnapshot:
        canonical = self.canonicalize(document)
        _expect(context.phase is RuntimePhase.PREFLIGHT, "/Phase", "BlackboardData.InvalidPreflightPhase", "preflight requires PREFLIGHT phase")
        target = _target_object_path(canonical["Target"])
        desired_parent = canonical["Body"]["Parent"]
        if current_snapshot is not None:
            current = _checked_snapshot(current_snapshot)
            records: dict = {}
            desired_parent_path = None if desired_parent is None else desired_parent["Path"]
            desired_route = [] if desired_parent_path is None else _record_chain(context, desired_parent_path, records, target)
            _reject_inherited_shadow(canonical["Body"]["Keys"], _chain_names(records, desired_route))
            return current
        return _preflight_observation(context, target, None if desired_parent is None else desired_parent["Path"], canonical["Body"]["Keys"])

    def observe_current_snapshot(
        self, *, target: str, context: GenericPrimitiveContext
    ) -> BlackboardSnapshot:
        _expect(
            context.phase is RuntimePhase.VERIFY,
            "/Phase",
            "BlackboardData.InvalidReadOnlyPhase",
            "read-only Blackboard observation requires VERIFY phase",
        )
        return _preflight_observation(
            context, _target_object_path(_require_target(target)), None, []
        )

    def apply(
        self,
        document: object,
        *,
        current_snapshot: BlackboardSnapshot,
        context: GenericPrimitiveContext,
        mutable_asset_handle: OpaqueHandleProxy,
        transitions: tuple[BlackboardTransition, ...] | None = None,
    ) -> tuple[ChangedRecord, ...]:
        canonical = self.canonicalize(document)
        current = _checked_snapshot(current_snapshot)
        _expect(context.phase is RuntimePhase.APPLY, "/Phase", "BlackboardData.InvalidApplyPhase", "apply requires APPLY phase")
        target = _target_object_path(canonical["Target"])
        desired_entries = canonical["Body"]["Keys"]
        transitions = transitions if transitions is not None else plan_blackboard_transitions(list(current.entries), desired_entries)
        desired_parent = canonical["Body"]["Parent"]
        desired_parent_path = None if desired_parent is None else desired_parent["Path"]
        changes = list(_parent_change(context, mutable_asset_handle, target, current.parent_path_or_null, desired_parent_path))
        working = [_snapshot(item) for item in current.entries]
        for transition in transitions:
            if transition.kind == "unchanged":
                continue
            working, emitted = _apply_transition(context, mutable_asset_handle, target, transition, working)
            changes.extend(emitted)
        return tuple(changes)

    def extract(self, *, target: str, current_snapshot: BlackboardSnapshot) -> dict:
        snapshot = _checked_snapshot(current_snapshot)
        result = self.create_template(target)
        # An extract describes an existing asset; re-applying it must update.
        result["Action"] = DocumentAction.CREATE_OR_UPDATE.value
        result["Body"] = {
            "Parent": None if snapshot.parent_path_or_null is None else {
                "Kind": "AssetRef", "Path": snapshot.parent_path_or_null,
            },
            "Keys": list(snapshot.entries),
        }
        return self.canonicalize(result)

    @staticmethod
    def _canonical_document(document: object) -> dict:
        document, target = _document_header(document)
        body = document.get("Body")
        keys = _canonical_keys(body)
        canonical = _snapshot({"SchemaVersion": 1, "Target": target, "Class": BLACKBOARD_DATA_CLASS,
                               "Action": document["Action"], "Definitions": document.get("Definitions", {}),
                               "Properties": document.get("Properties", {}),
                               "Body": {"Parent": _require_parent(body["Parent"], target),
                                        "Keys": keys}})
        DocumentCodec().validate(canonical)
        return canonical

    def repair(self, document: object, *, context: object) -> tuple[()]:
        self.validate(document)
        if context.phase is not RuntimePhase.REPAIR:
            raise contract_error("/Phase", "BlackboardData.InvalidRepairPhase", "repair requires REPAIR phase")
        return ()


BLACKBOARD_DATA_PROFILE_META = ProfileMeta(
    "profile.blackboard_data.v2",
    BLACKBOARD_ROUTE_ID,
    BLACKBOARD_DATA_CLASS,
    (
        RegionMeta(
            "Parent",
            "/Body/Parent",
            RegionOwnership.MANAGED,
            IdentityPolicy.STABLE_ID,
            OrderPolicy.PRESERVE,
            DefaultPolicy.EXPLICIT,
            OmissionPolicy.PRESERVE,
            DeletePolicy.EXPLICIT_ONLY,
            ComparisonPolicy.EXACT,
        ),
        RegionMeta(
            "Keys",
            "/Body/Keys",
            RegionOwnership.MANAGED,
            IdentityPolicy.STABLE_KEY,
            OrderPolicy.IDENTITY_KEYED,
            DefaultPolicy.EXPLICIT,
            OmissionPolicy.PRESERVE,
            DeletePolicy.OMISSION_DELETES,
            ComparisonPolicy.ORDERED,
        ),
    ),
    BLACKBOARD_REQUIRED_PRIMITIVES,
)


def create_blackboard_data_profile() -> BlackboardDataProfileCandidate:
    return BlackboardDataProfileCandidate()
