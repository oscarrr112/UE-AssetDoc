# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from dataclasses import dataclass
import re

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.diagnostics import AssetDocumentContractError, contract_error
from asset_document.dto import JsonValue, RuntimePhase
from asset_document.regions.primitive_catalog import (
    GenericPrimitiveContext,
    NamedOpaqueHandleResult,
    PrimitiveOutcome,
    PrimitiveOutcomeStatus,
    requirement,
)


FRAGMENT_KINDS: tuple[str, ...] = (
    "AssetRef",
    "ClassRef",
    "StructValue",
    "EmbeddedObject",
    "DefinitionRef",
)

_KIND_REQUIREMENTS = {
    "AssetRef": "editor.reference.resolve_asset.v1",
    "ClassRef": "editor.class.resolve_canonical.v1",
    "StructValue": "editor.fragment.struct_codec.v1",
}


def _escape_pointer(value: object) -> str:
    return str(value).replace("~", "~0").replace("/", "~1")


def _definition_path(definition_id: str) -> str:
    return f"/Definitions/{_escape_pointer(definition_id)}"


def _snapshot(value: JsonValue) -> JsonValue:
    codec = DocumentCodec()
    return codec.decode(codec.encode(value))


def _is_script_path(value: str) -> bool:
    return re.fullmatch(r"/Script/[^/.]+\.[^/.]+", value) is not None


def _is_generated_class_path(value: str) -> bool:
    return (
        re.fullmatch(r"/Game/.+\.[^/.]+_C", value) is not None
        and value.split(".")[-1].endswith("_C")
    )


def _is_class_path(value: str) -> bool:
    return _is_script_path(value) or _is_generated_class_path(value)


def _is_asset_path(value: str) -> bool:
    return re.fullmatch(r"/Game/.+\.[^/.]+", value) is not None


def _invalid_reference(path: str, message: str) -> None:
    raise contract_error(path, "Generic.NonCanonicalReference", message)


def _require_canonical_reference(
    value: object,
    predicate,
    path: str,
    message: str,
) -> None:
    if type(value) is not str:
        _invalid_reference(path, message)
    if not predicate(value):
        _invalid_reference(path, message)


def _require_fragment(fragment: JsonValue, path: str) -> tuple[str, dict[str, JsonValue]]:
    if type(fragment) is not dict:
        raise contract_error(path, "Generic.InvalidFragmentType", "fragment must be an exact object")
    kind = fragment.get("Kind")
    if type(kind) is not str or not kind:
        raise contract_error(f"{path}/Kind", "Generic.MissingFragmentKind", "Kind is required")
    if kind not in FRAGMENT_KINDS:
        raise contract_error(f"{path}/Kind", "Generic.UnknownFragmentKind", "Kind is not supported")
    return kind, fragment


def _optional_class(fragment: dict[str, JsonValue], field: str, path: str) -> str | None:
    value = fragment.get(field)
    if value is None:
        return None
    if type(value) is not str or not _is_class_path(value):
        _invalid_reference(f"{path}/{field}", f"{field} must be a canonical class path")
    return value


def _validate_terminal_shape(
    kind: str,
    fragment: dict[str, JsonValue],
    path: str,
) -> None:
    validators = {
        "AssetRef": _validate_asset_ref_shape,
        "ClassRef": _validate_class_ref_shape,
        "StructValue": _validate_struct_value_shape,
    }
    validator = validators.get(kind)
    if validator is not None:
        validator(fragment, path)


def _validate_asset_ref_shape(fragment: dict[str, JsonValue], path: str) -> None:
    _require_canonical_reference(
        fragment.get("Path"),
        _is_asset_path,
        f"{path}/Path",
        "Path must be a canonical asset object path",
    )
    _optional_class(fragment, "Class", path)


def _validate_class_ref_shape(fragment: dict[str, JsonValue], path: str) -> None:
    _require_canonical_reference(
        fragment.get("Class"),
        _is_class_path,
        f"{path}/Class",
        "Class must be a canonical class path",
    )
    _optional_class(fragment, "ExpectedBase", path)


def _validate_struct_value_shape(fragment: dict[str, JsonValue], path: str) -> None:
    _require_canonical_reference(
        fragment.get("Struct"),
        _is_script_path,
        f"{path}/Struct",
        "Struct must be a canonical script path",
    )
    if type(fragment.get("Properties")) is not dict:
        raise contract_error(
            path,
            "Generic.InvalidFragmentType",
            "StructValue.Properties must be an exact object",
        )


def _definition_ref_id(fragment: dict[str, JsonValue], path: str) -> str:
    definition_id = fragment.get("Id")
    if type(definition_id) is not str or not definition_id:
        raise contract_error(
            f"{path}/Id",
            "Generic.MissingDefinitionRefId",
            "DefinitionRef.Id must be a non-empty exact string",
        )
    return definition_id


def _resolve_definition(
    definition_id: str,
    definitions: dict[str, JsonValue],
    stack: tuple[str, ...],
) -> ResolvedFragment:
    path = _definition_path(definition_id)
    if definition_id not in definitions:
        raise contract_error(path, "Generic.DefinitionNotFound", "definition was not found")
    kind, fragment = _require_fragment(definitions[definition_id], path)
    if kind != "DefinitionRef":
        _validate_terminal_shape(kind, fragment, path)
        return ResolvedFragment(kind, fragment)
    target = _definition_ref_id(fragment, path)
    chain = stack + (definition_id,)
    if target in chain:
        display = " -> ".join(_definition_path(item) for item in chain + (target,))
        raise contract_error(f"{path}/Id", "Generic.DefinitionCycle", display)
    return _resolve_definition(target, definitions, chain)


@dataclass(frozen=True, slots=True)
class ResolvedFragment:
    kind: str
    value: JsonValue
    handle_results: tuple[NamedOpaqueHandleResult, ...] = ()

    def __post_init__(self) -> None:
        if self.kind not in FRAGMENT_KINDS:
            raise contract_error("/Fragment/Kind", "Generic.UnknownFragmentKind", "invalid resolved kind")
        if type(self.handle_results) is not tuple or any(
            type(item) is not NamedOpaqueHandleResult for item in self.handle_results
        ):
            raise contract_error("/Fragment", "Generic.InvalidPrimitiveResult", "invalid handle results")
        object.__setattr__(self, "value", _snapshot(self.value))


def _value_arguments(kind: str, fragment: dict[str, JsonValue]) -> dict[str, JsonValue]:
    if kind == "AssetRef":
        return {"path": fragment["Path"], "expected_class": fragment.get("Class")}
    if kind == "ClassRef":
        return {"class": fragment["Class"], "expected_base": fragment.get("ExpectedBase")}
    return {
        "operation": "validate",
        "struct": fragment["Struct"],
        "properties": fragment["Properties"],
    }


def _successful_outcome(outcome: object, path: str) -> PrimitiveOutcome:
    if type(outcome) is not PrimitiveOutcome:
        raise contract_error(path, "Generic.InvalidPrimitiveResult", "primitive returned an invalid outcome")
    if outcome.status is PrimitiveOutcomeStatus.SUCCEEDED:
        return outcome
    if not outcome.diagnostics:
        raise contract_error(path, "Generic.InvalidPrimitiveResult", "failure omitted diagnostics")
    raise AssetDocumentContractError(outcome.diagnostics[0])


def _require_empty_common_lanes(outcome: PrimitiveOutcome, path: str) -> None:
    if outcome.changed_set:
        raise contract_error(path, "Generic.InvalidPrimitiveResult", "fragment primitive returned changes")


def _decode_fragment_outcome(
    kind: str,
    outcome_value: object,
    path: str,
) -> ResolvedFragment:
    outcome = _successful_outcome(outcome_value, path)
    _require_empty_common_lanes(outcome, path)
    decoders = {
        "AssetRef": _decode_asset_ref,
        "ClassRef": _decode_class_ref,
        "StructValue": _decode_struct_value,
    }
    return decoders[kind](outcome, path)


def _require_value_key(
    outcome: PrimitiveOutcome,
    key: str,
    path: str,
) -> JsonValue:
    if outcome.handle_results:
        raise contract_error(path, "Generic.InvalidPrimitiveResult", "unexpected handle result")
    if type(outcome.value) is not dict or set(outcome.value) != {key}:
        raise contract_error(path, "Generic.InvalidPrimitiveResult", "invalid primitive value shape")
    return outcome.value[key]


def _decode_class_ref(outcome: PrimitiveOutcome, path: str) -> ResolvedFragment:
    value = _require_value_key(outcome, "canonical_class", path)
    if type(value) is not str:
        raise contract_error(path, "Generic.InvalidPrimitiveResult", "canonical class must be a string")
    return ResolvedFragment("ClassRef", outcome.value)


def _decode_struct_value(outcome: PrimitiveOutcome, path: str) -> ResolvedFragment:
    value = _require_value_key(outcome, "canonical_value", path)
    try:
        canonical_value = _snapshot(value)
    except AssetDocumentContractError:
        raise contract_error(
            path,
            "Generic.InvalidPrimitiveResult",
            "canonical value must be an exact JSON value",
        )
    return ResolvedFragment("StructValue", {"canonical_value": canonical_value})


def _decode_asset_ref(outcome: PrimitiveOutcome, path: str) -> ResolvedFragment:
    valid_value = (
        type(outcome.value) is dict
        and set(outcome.value) == {"canonical_path"}
        and type(outcome.value["canonical_path"]) is str
    )
    valid_handles = (
        len(outcome.handle_results) == 1
        and outcome.handle_results[0].name == "asset"
        and len(outcome.handle_results[0].handles) == 1
    )
    if not valid_value or not valid_handles:
        raise contract_error(path, "Generic.InvalidPrimitiveResult", "invalid AssetRef result lanes")
    return ResolvedFragment("AssetRef", outcome.value, outcome.handle_results)


class FragmentSemanticAdapter:
    def validate_definitions(self, definitions: JsonValue) -> None:
        if type(definitions) is not dict:
            raise contract_error(
                "/Definitions",
                "Generic.InvalidDefinitionsType",
                "Definitions must be an exact object",
            )
        for definition_id in definitions:
            if type(definition_id) is not str or not definition_id:
                raise contract_error(
                    "/Definitions",
                    "Generic.InvalidDefinitionId",
                    "Definition IDs must be non-empty exact strings",
                )
        for definition_id in sorted(definitions):
            _resolve_definition(definition_id, definitions, ())

    def resolve_definition(
        self,
        definition_id: str,
        definitions: dict[str, JsonValue],
    ) -> ResolvedFragment:
        if type(definition_id) is not str or not definition_id:
            raise contract_error(
                "/Definitions",
                "Generic.InvalidDefinitionId",
                "Definition ID must be a non-empty exact string",
            )
        if type(definitions) is not dict:
            raise contract_error(
                "/Definitions",
                "Generic.InvalidDefinitionsType",
                "Definitions must be an exact object",
            )
        return _resolve_definition(definition_id, definitions, ())

    def preflight(
        self,
        fragment: JsonValue,
        definitions: dict[str, JsonValue],
        context: GenericPrimitiveContext,
        *,
        path: str,
    ) -> ResolvedFragment:
        if context.phase is not RuntimePhase.PREFLIGHT:
            raise contract_error(path, "Generic.InvalidPrimitiveResult", "fragment preflight requires PREFLIGHT")
        kind, parsed = _require_fragment(fragment, path)
        if kind == "EmbeddedObject":
            raise contract_error(
                path,
                "Generic.EmbeddedObjectBlocked",
                "EmbeddedObject authority is not admitted",
            )
        if kind == "DefinitionRef":
            definition_id = _definition_ref_id(parsed, path)
            resolved = self.resolve_definition(definition_id, definitions)
            return self.preflight(resolved.value, definitions, context, path=path)
        _validate_terminal_shape(kind, parsed, path)
        outcome = context.invoke(
            requirement=requirement(_KIND_REQUIREMENTS[kind]),
            value_arguments=_value_arguments(kind, parsed),
            handle_arguments=(),
        )
        return _decode_fragment_outcome(kind, outcome, path)
