# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import re

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.diff.canonical_diff import canonical_diff
from asset_document.diff.model import Difference
from asset_document.diagnostics import contract_error
from asset_document.dto import ChangedRecord, JsonValue, PrimitiveRequirement, RuntimePhase
from asset_document.meta import DocumentAction
from asset_document.regions.fragment_semantics import (
    FRAGMENT_KINDS,
    FragmentSemanticAdapter,
)
from asset_document.regions.primitive_catalog import (
    GENERIC_REQUIRED_PRIMITIVES,
    GenericPrimitiveContext,
    OpaqueHandleProxy,
)
from asset_document.profiles.generic_components import generic_property_component
from asset_document.regions.property_semantics import (
    GenericPropertyComponent,
    PropertySemanticAdapter,
)
from asset_document.regions.enhanced_input_checks import enhanced_input_warnings
from asset_document.regions.inspection_semantics import InspectionSemanticAdapter
from asset_document.read_only_extract import admits_read_only


GENERIC_PROFILE_ID: str = "profile.generic-candidate.v1"


def _snapshot(value: JsonValue) -> JsonValue:
    codec = DocumentCodec()
    return codec.decode(codec.encode(value))


def _is_script_class_path(value: str) -> bool:
    return re.fullmatch(r"/Script/[^/.]+\.[^/.]+", value) is not None


def _is_generated_class_path(value: str) -> bool:
    return re.fullmatch(r"/Game/.+\.[^/.]+_C", value) is not None


def _is_exact_class_path(value: str) -> bool:
    return _is_script_class_path(value) or _is_generated_class_path(value)


def _is_target(value: str) -> bool:
    return re.fullmatch(r"/Game/[A-Za-z0-9_/-]+", value) is not None


def _require_target(value: object) -> str:
    if admits_read_only(value):
        return value
    if type(value) is not str or not value:
        raise contract_error("/Target", "Meta.EmptyTarget", "Target must not be empty")
    if not _is_target(value):
        raise contract_error(
            "/Target",
            "Generic.NonCanonicalTarget",
            "Target must be a canonical /Game package path",
        )
    return value


def _body_truth_table(document: dict[str, JsonValue]) -> None:
    if "Body" not in document:
        return
    body = document["Body"]
    if type(body) is not dict:
        raise contract_error("/Body", "InvalidBodyType", "Body must be an exact object")
    if body:
        raise contract_error("/Body", "GenericBodyNotEmpty", "Generic Body must be empty")


def _factory_context(document: dict[str, JsonValue]) -> str | None:
    """Optional create-time FactoryContext: the existing asset the class's
    New-asset dialog would ask for (a Skeleton, a Skeletal Mesh, ...), or the
    class its class picker would choose (/Script/Module.Class). It only
    configures the editor factory when the document creates the asset; it is
    not a property and never appears in Extract output or Diff."""
    if "FactoryContext" not in document:
        return None
    value = document["FactoryContext"]
    if type(value) is not str or re.fullmatch(r"/[A-Za-z0-9_]+(?:/[^/.\s]+)+\.[^/.\s:]+", value) is None:
        raise contract_error(
            "/FactoryContext",
            "Generic.InvalidFactoryContext",
            "FactoryContext must be the object path of one existing asset, e.g. /Game/Characters/SK_Hero.SK_Hero, "
            "or a class path such as /Script/Slate.ButtonWidgetStyle",
        )
    return value


def _properties(document: dict[str, JsonValue]) -> dict[str, JsonValue]:
    value = document.get("Properties", {})
    if type(value) is not dict:
        raise contract_error(
            "/Properties",
            "Generic.InvalidPropertiesType",
            "Properties must be an exact object",
        )
    return value


def _definitions(document: dict[str, JsonValue]) -> dict[str, JsonValue]:
    value = document.get("Definitions", {})
    if type(value) is not dict:
        raise contract_error(
            "/Definitions",
            "Generic.InvalidDefinitionsType",
            "Definitions must be an exact object",
        )
    return value


def omit_inspected_properties(payload: dict, body_owned: tuple[str, ...]) -> dict:
    """Drop Body-owned property rows from an Inspect payload's properties/skipped lists."""
    owned = {name.casefold() for name in body_owned}
    result = dict(payload)
    for field in ("properties", "skipped"):
        rows = result.get(field)
        if type(rows) is list:
            result[field] = [row for row in rows
                             if not (type(row) is dict and type(row.get("name")) is str
                                     and row["name"].casefold() in owned)]
    return result


class GenericProfileCandidate:
    __slots__ = ("_exact_asset_class_path", "_fragments", "_properties")

    def __init__(
        self,
        exact_asset_class_path: str,
        component: GenericPropertyComponent | None = None,
    ) -> None:
        if type(exact_asset_class_path) is not str or not _is_exact_class_path(exact_asset_class_path):
            raise contract_error(
                "/Class",
                "Generic.NonCanonicalClassPath",
                "Class must be a canonical /Script path or exact generated-class object path",
            )
        self._exact_asset_class_path = exact_asset_class_path
        self._fragments = FragmentSemanticAdapter()
        if component is None:
            component = generic_property_component(exact_asset_class_path)
        if component is not None and component.exact_class_path != exact_asset_class_path:
            raise contract_error(
                "/Class",
                "Generic.ClassMismatch",
                "property component belongs to another exact class",
            )
        self._properties = PropertySemanticAdapter(component)

    @property
    def runtime_entry_module(self) -> str:
        return "asset_document/bootstrap.py"

    def required_primitives(self) -> tuple[PrimitiveRequirement, ...]:
        return GENERIC_REQUIRED_PRIMITIVES

    def inspect(
        self,
        *,
        mode: str,
        exact_class_path: str,
        exact_asset_object_path: str | None,
        editor_asset_handle: OpaqueHandleProxy | None,
        context: GenericPrimitiveContext,
    ) -> dict[str, JsonValue]:
        return InspectionSemanticAdapter(self._properties).inspect(
            mode=mode,
            exact_class_path=exact_class_path,
            exact_asset_object_path=exact_asset_object_path,
            editor_asset_handle=editor_asset_handle,
            context=context,
        )

    def inspect_profile(self) -> dict[str, JsonValue]:
        return _snapshot(
            {
                "Class": self._exact_asset_class_path,
                "DocumentShape": {
                    "Definitions": "map<string, Fragment>",
                    "Properties": "reflected CDO-diff properties",
                    "Body": {},
                },
                "BodySections": [],
                "FragmentKinds": list(FRAGMENT_KINDS),
                "InternalAdapters": [],
                "RegionPolicies": [],
                "BlockedFragments": [
                    {
                        "kind": "EmbeddedObject",
                        "code": "AD.BLOCK.EDITOR.EMBEDDED_OBJECT_AUTHORITY",
                    }
                ],
            }
        )

    def schema(self) -> dict[str, JsonValue]:
        return _snapshot(
            {
                "SchemaVersion": 1,
                "Class": self._exact_asset_class_path,
                "Type": "object",
                "Required": ["SchemaVersion", "Target", "Class", "Action"],
                "Properties": {
                    "Definitions": {"type": "object", "optional": True},
                    "Properties": {"type": "object", "optional": True},
                    "FactoryContext": {"type": "string", "optional": True, "create_only": True},
                    "Body": {"accepts": ["missing", "empty object"]},
                },
            }
        )

    def create_template(self, target: str) -> dict[str, JsonValue]:
        canonical_target = _require_target(target)
        return _snapshot(
            {
                "SchemaVersion": 1,
                "Target": canonical_target,
                "Class": self._exact_asset_class_path,
                "Action": DocumentAction.CREATE_OR_UPDATE.value,
                "Definitions": {},
                "Properties": {},
            }
        )

    def validate(self, document: JsonValue) -> None:
        if type(document) is not dict:
            raise contract_error(
                "/",
                "Generic.InvalidDocumentType",
                "Generic document must be an exact object",
            )
        schema = document.get("SchemaVersion")
        if type(schema) is not int or schema != 1:
            raise contract_error(
                "/SchemaVersion",
                "Meta.InvalidSchemaVersion",
                "SchemaVersion must be exact integer 1",
            )
        _require_target(document.get("Target"))
        self._validate_class(document.get("Class"))
        self._validate_action(document.get("Action"))
        self._validate_asset_type(document)
        definitions = _definitions(document)
        properties = _properties(document)
        _body_truth_table(document)
        _factory_context(document)
        self._fragments.validate_definitions(definitions)
        DocumentCodec().validate(properties)
        DocumentCodec().validate(document)

    def factory_context(self, document: JsonValue) -> str | None:
        """The FactoryContext asset path a create passes to the editor factory."""
        self.validate(document)
        return _factory_context(document)

    def canonicalize(self, document: JsonValue) -> dict[str, JsonValue]:
        self.validate(document)
        canonical = _snapshot(document)
        if canonical.get("Body") == {}:
            del canonical["Body"]
        if "Properties" in canonical:
            canonical["Properties"] = _snapshot(self._properties.canonicalize(canonical["Properties"]))
        return canonical

    def preflight(
        self,
        document: JsonValue,
        scratch_handle: OpaqueHandleProxy,
        context: GenericPrimitiveContext,
    ) -> tuple:
        """The domain warnings of the document's final state (the scratch holds it after the writes)."""
        self.validate(document)
        parsed = document
        admitted = self._properties.enumerate(self._exact_asset_class_path, context)
        properties = _properties(parsed)
        self._properties.validate(properties, admitted)
        self._properties.preflight(scratch_handle, properties, admitted, context)
        return enhanced_input_warnings(self._exact_asset_class_path, parsed["Target"], scratch_handle, context)

    def apply(
        self,
        document: JsonValue,
        read_asset_handle: OpaqueHandleProxy,
        mutable_asset_handle: OpaqueHandleProxy,
        context: GenericPrimitiveContext,
    ) -> tuple[ChangedRecord, ...]:
        return self._mutate(
            document,
            read_asset_handle,
            mutable_asset_handle,
            context,
            RuntimePhase.APPLY,
        )

    def repair(
        self,
        document: JsonValue,
        asset_handle: OpaqueHandleProxy,
        context: GenericPrimitiveContext,
    ) -> tuple[ChangedRecord, ...]:
        del asset_handle
        self.validate(document)
        if context.phase is not RuntimePhase.REPAIR:
            raise contract_error(
                "/Phase",
                "Generic.InvalidMutationPhase",
                "repair requires the bound REPAIR context",
            )
        return ()

    def extract(
        self,
        *,
        target: str,
        asset_handle: OpaqueHandleProxy,
        context: GenericPrimitiveContext,
        include_defaults: bool = False,
        authored_properties: dict[str, JsonValue] | None = None,
    ) -> dict[str, JsonValue]:
        """``authored_properties``: read structured values in the shape the
        document authored them (omitted struct members / instanced Properties
        keep their current value on Apply, so Diff and Verify skip them)."""
        canonical_target = _require_target(target)
        admitted = self._properties.enumerate(self._exact_asset_class_path, context)
        properties = self._properties.extract(
            asset_handle,
            admitted,
            context,
            include_defaults=include_defaults,
            authored=authored_properties,
        )
        return _snapshot(
            {
                "SchemaVersion": 1,
                "AssetType": "GenericAsset",
                "Target": canonical_target,
                "Class": self._exact_asset_class_path,
                "Action": DocumentAction.CREATE_OR_UPDATE.value,
                "Properties": properties,
            }
        )

    def validate_authored_properties(
        self,
        document: JsonValue,
        context: GenericPrimitiveContext,
    ) -> None:
        """Reject the property names Apply would reject, so Diff never reports a
        change to a property that is unknown or not writable."""
        self.validate(document)
        admitted = self._properties.enumerate(self._exact_asset_class_path, context)
        self._properties.validate(_properties(document), admitted)

    def diff(
        self,
        authored: JsonValue,
        extracted: JsonValue,
    ) -> tuple[Difference, ...]:
        authored_projection = self._comparison_projection(self.canonicalize(authored))
        extracted_projection = self._comparison_projection(self.canonicalize(extracted))
        extracted_properties = extracted_projection["Properties"]
        extracted_projection["Properties"] = {
            name: extracted_properties[name]
            for name in authored_projection["Properties"]
            if name in extracted_properties
        }
        return canonical_diff(authored_projection, extracted_projection)

    def _mutate(
        self,
        document: JsonValue,
        read_asset_handle: OpaqueHandleProxy,
        mutable_asset_handle: OpaqueHandleProxy,
        context: GenericPrimitiveContext,
        phase: RuntimePhase,
    ) -> tuple[ChangedRecord, ...]:
        self.validate(document)
        if context.phase is not phase:
            raise contract_error(
                "/Phase",
                "Generic.InvalidMutationPhase",
                "mutation phase must match the bound context",
            )
        parsed = document
        admitted = self._properties.enumerate(self._exact_asset_class_path, context)
        properties = _properties(parsed)
        self._properties.validate(properties, admitted)
        return self._properties.apply(
            read_asset_handle,
            mutable_asset_handle,
            properties,
            admitted,
            context,
            phase=phase,
        )

    def _comparison_projection(
        self,
        document: dict[str, JsonValue],
    ) -> dict[str, JsonValue]:
        return _snapshot(
            {
                "Target": document["Target"],
                "Class": document["Class"],
                "Properties": document.get("Properties", {}),
            }
        )

    def _validate_class(self, value: object) -> None:
        if type(value) is not str or not value:
            raise contract_error(
                "/Class",
                "Meta.EmptyExactClassPath",
                "Class must not be empty",
            )
        if not _is_exact_class_path(value):
            raise contract_error(
                "/Class",
                "Generic.NonCanonicalClassPath",
                "Class must use exact canonical syntax",
            )
        if value != self._exact_asset_class_path:
            raise contract_error(
                "/Class",
                "Generic.ClassMismatch",
                "document Class does not match this candidate binding",
            )

    @staticmethod
    def _validate_action(value: object) -> None:
        if type(value) is not str or value not in {item.value for item in DocumentAction}:
            raise contract_error(
                "/Action",
                "Generic.InvalidAction",
                "Action must be an existing DocumentAction value",
            )

    @staticmethod
    def _validate_asset_type(document: dict[str, JsonValue]) -> None:
        if "AssetType" not in document:
            return
        if document["AssetType"] != "GenericAsset" or type(document["AssetType"]) is not str:
            raise contract_error(
                "/AssetType",
                "Generic.InvalidAssetType",
                "AssetType must be GenericAsset when present",
            )
