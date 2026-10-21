from __future__ import annotations

from dataclasses import dataclass, field, replace
from types import MappingProxyType
from typing import Mapping, Protocol


from asset_document.canonical.document_codec import DocumentCodec, integral_numbers_as_integers
from asset_document.diagnostics import (
    AssetDocumentContractError,
    RuntimeDiagnostic,
    contract_error,
    describe_json,
)
from asset_document.diff.canonical_diff import canonical_diff, canonical_equal
from asset_document.diff.model import DiffKind
from asset_document.dto import (
    ChangedRecord,
    JsonValue,
    OptionalJsonValue,
    RuntimePhase,
)
from asset_document.regions.primitive_catalog import (
    GenericPrimitiveContext,
    NamedOpaqueHandleArgument,
    NamedOpaqueHandleResult,
    OpaqueHandleProxy,
    PrimitiveOutcome,
    PrimitiveOutcomeStatus,
    requirement,
)


_ENUMERATE_ID = "editor.property.enumerate_admitted.v1"
_ENUMERATE_BATCH_ID = "editor.property.enumerate_admitted_batch.v1"
_PREFLIGHT_ID = "editor.property.preflight_admitted.v1"
_READ_ID = "editor.property.read_admitted.v1"
_WRITE_BATCH_ID = "editor.property.write_admitted_batch.v1"
GENERIC_RUNTIME_MUTATION_PHASES = (RuntimePhase.APPLY,)


def _escape_pointer(value: str) -> str:
    return value.replace("~", "~0").replace("/", "~1")


def _property_path(name: str) -> str:
    return f"/Properties/{_escape_pointer(name)}"


def _is_nonempty_exact_string(value: object) -> bool:
    return type(value) is str and bool(value)


def _invalid_result(path: str, message: str) -> None:
    raise contract_error(path, "Generic.InvalidPrimitiveResult", message)


def _json_snapshot(value: object, path: str) -> JsonValue:
    try:
        codec = DocumentCodec()
        return codec.decode(codec.encode(value))
    except AssetDocumentContractError:
        _invalid_result(path, "primitive value must be an exact JSON value")


def _successful_outcome(value: object, path: str) -> PrimitiveOutcome:
    if type(value) is not PrimitiveOutcome:
        _invalid_result(path, "primitive returned an invalid outcome")
    if value.status is PrimitiveOutcomeStatus.SUCCEEDED:
        return value
    if not value.diagnostics:
        _invalid_result(path, "failed primitive omitted diagnostics")
    raise AssetDocumentContractError(_locate_value_diagnostic(value.diagnostics[0]))


def _locate_value_diagnostic(diagnostic: RuntimeDiagnostic) -> RuntimeDiagnostic:
    """Map a typed-value decode failure onto the authored document location.

    The editor codec reports the property name and the JSON pointer inside its
    value as diagnostic context ("property", "value_path").
    """
    context = {item.key: item.value for item in diagnostic.context}
    name = context.get("property")
    value_path = context.get("value_path")
    if not _is_nonempty_exact_string(name) or type(value_path) is not str:
        return diagnostic
    if value_path and not value_path.startswith("/"):
        return diagnostic
    return replace(diagnostic, path=_property_path(name) + value_path)


def _require_no_handles(outcome: PrimitiveOutcome, path: str) -> None:
    if outcome.handle_results:
        _invalid_result(path, "property operation returned unexpected handles")


def _require_read_only_lanes(outcome: PrimitiveOutcome, path: str) -> None:
    _require_no_handles(outcome, path)
    if outcome.changed_set:
        _invalid_result(path, "read-only property operation returned changes")


def _is_no_value(value: object) -> bool:
    return value is None or (type(value) is dict and not value)


def _require_preflight_result(value: object, expected_value: JsonValue, path: str) -> None:
    if type(value) is not dict or set(value) != {"canonical_value", "feasible"}:
        _invalid_result(path, "preflight returned an invalid result shape")
    if value["feasible"] is not True:
        _invalid_result(path, "preflight did not report exact feasibility")
    canonical_value = integral_numbers_as_integers(_json_snapshot(value["canonical_value"], path))
    authored_value = integral_numbers_as_integers(expected_value)
    if not canonical_equal(canonical_value, authored_value):
        differences = canonical_diff(authored_value, canonical_value)
        first = differences[0]
        more = (
            f" ({len(differences)} values differ; the first is shown)"
            if len(differences) > 1 else ""
        )
        raise contract_error(
            path + first.path,
            "Generic.PropertyValueNotStoredExactly",
            "the editor changes this value when it applies it: at "
            f"{path + first.path} {_describe_stored_difference(first.kind, first.before, first.after)}"
            f"{more}. Author the value the asset keeps (PostEditChange rewrites values such as "
            "clamped numbers, generated names or a sprite's pivot; integer properties keep their "
            "range, names and enums their canonical spelling). Float numbers are not rejected: they are stored at float precision.",
        )


def _short_json(value: JsonValue | None) -> str:
    return describe_json(value, 200)


def _describe_stored_difference(kind: DiffKind, authored: JsonValue | None, stored: JsonValue | None) -> str:
    if kind is DiffKind.ADD:
        return f"the asset would also hold {_short_json(stored)}"
    if kind is DiffKind.REMOVE:
        return f"the asset would not hold the authored {_short_json(authored)}"
    return f"the asset would hold {_short_json(stored)} instead of {_short_json(authored)}"


_DECLARED_ARGUMENT = "admit_properties"


@dataclass(frozen=True, slots=True)
class PropertyAdmission:
    """Profile-declared admission of serialized non-edit properties of one exact class.

    The names travel to the Editor provider as the ``admit_properties`` value
    argument of the property primitives; the provider still requires each name
    to exist, be serialized and not be EditConst.
    """

    exact_class_path: str
    names: tuple[str, ...]

    def __post_init__(self) -> None:
        if type(self.exact_class_path) is not str or not self.exact_class_path.startswith("/"):
            raise contract_error(
                "/Class",
                "Generic.InvalidPropertyAdmission",
                "admission class must be an exact class path",
            )
        if (
            type(self.names) is not tuple
            or not self.names
            or any(type(name) is not str or not name for name in self.names)
            or len(set(self.names)) != len(self.names)
        ):
            raise contract_error(
                "/Properties",
                "Generic.InvalidPropertyAdmission",
                "admitted names must be a non-empty tuple of unique non-empty strings",
            )


def admit_properties(cls: str, names: tuple[str, ...]) -> PropertyAdmission:
    """Declares extra (non-edit, serialized) properties a Profile authors on ``cls``."""
    return PropertyAdmission(cls, tuple(names))


class PropertyValueCodec(Protocol):
    """Converts one property between its document form and the native typed codec value.

    ``to_document`` must be the canonical document form of ``from_document``'s
    input, so ``to_document(from_document(v))`` canonicalizes authored values.
    Both raise stable contract errors at ``path`` for invalid values.
    """

    def from_document(self, value: JsonValue, path: str) -> JsonValue: ...

    def to_document(self, value: JsonValue, path: str) -> JsonValue: ...


@dataclass(frozen=True, slots=True)
class GenericPropertyComponent:
    """Property surface additions for one exact class: declared names plus value codecs."""

    admission: PropertyAdmission
    value_codecs: Mapping[str, PropertyValueCodec] = field(default_factory=dict)

    def __post_init__(self) -> None:
        if type(self.admission) is not PropertyAdmission:
            raise contract_error(
                "/Properties",
                "Generic.InvalidPropertyAdmission",
                "component admission must be a PropertyAdmission",
            )
        unknown = set(self.value_codecs) - set(self.admission.names)
        if unknown:
            raise contract_error(
                "/Properties",
                "Generic.InvalidPropertyAdmission",
                "value codecs may only cover declared properties",
            )
        object.__setattr__(self, "value_codecs", MappingProxyType(dict(self.value_codecs)))

    @property
    def exact_class_path(self) -> str:
        return self.admission.exact_class_path


@dataclass(frozen=True, slots=True)
class AdmittedProperty:
    name: str
    property_handle: OpaqueHandleProxy
    readable: bool
    writable: bool
    default_value: OptionalJsonValue

    def __post_init__(self) -> None:
        if type(self.name) is not str or not self.name:
            raise contract_error(
                "/Properties",
                "Generic.InvalidPropertyDescriptor",
                "property name must be a non-empty exact string",
            )
        if type(self.readable) is not bool or type(self.writable) is not bool:
            raise contract_error(
                _property_path(self.name),
                "Generic.InvalidPropertyDescriptor",
                "readable and writable must be exact bool values",
            )
        if type(self.default_value) is not OptionalJsonValue:
            raise contract_error(
                _property_path(self.name),
                "Generic.InvalidPropertyDescriptor",
                "default must be an OptionalJsonValue",
            )


@dataclass(frozen=True, slots=True)
class AdmittedPropertyBatch:
    properties: tuple[AdmittedProperty, ...]
    property_batch_handle: OpaqueHandleProxy

    def __post_init__(self) -> None:
        if type(self.properties) is not tuple or any(
            type(item) is not AdmittedProperty for item in self.properties
        ):
            raise contract_error(
                "/Properties",
                "Generic.InvalidPropertyDescriptor",
                "batch properties must be an exact admitted tuple",
            )


def _decode_row(
    row: object,
    handle: OpaqueHandleProxy,
) -> AdmittedProperty:
    descriptor = _require_descriptor_shape(row)
    name = _require_descriptor_fields(descriptor)
    default = _descriptor_default(descriptor, name)
    return AdmittedProperty(
        name,
        handle,
        descriptor["readable"],
        descriptor["writable"],
        default,
    )


def _require_descriptor_shape(row: object) -> dict[str, JsonValue]:
    if type(row) is not dict:
        raise contract_error(
            "/Properties",
            "Generic.InvalidPropertyDescriptor",
            "every property descriptor must be an exact object",
        )
    allowed = {
        "name",
        "declaring_class",
        "codec_id",
        "readable",
        "writable",
        "default",
    }
    required = {
        "name",
        "declaring_class",
        "codec_id",
        "readable",
        "writable",
    }
    if not required.issubset(row) or not set(row).issubset(allowed):
        raise contract_error(
            "/Properties",
            "Generic.InvalidPropertyDescriptor",
            "property descriptor keys do not match the frozen shape",
        )
    return row


def _require_descriptor_fields(row: dict[str, JsonValue]) -> str:
    name = row["name"]
    if not _is_nonempty_exact_string(name):
        raise contract_error(
            "/Properties",
            "Generic.InvalidPropertyDescriptor",
            "property name must be a non-empty exact string",
        )
    if not all(map(_is_nonempty_exact_string, (row["declaring_class"], row["codec_id"]))):
        raise contract_error(
            _property_path(name),
            "Generic.InvalidPropertyDescriptor",
            "declaring_class and codec_id must be non-empty exact strings",
        )
    if type(row["readable"]) is not bool or type(row["writable"]) is not bool:
        raise contract_error(
            _property_path(name),
            "Generic.InvalidPropertyDescriptor",
            "readable and writable must be exact bool values",
        )
    return name


def _descriptor_default(
    row: dict[str, JsonValue],
    name: str,
) -> OptionalJsonValue:
    if "default" not in row:
        return OptionalJsonValue()
    return OptionalJsonValue(
        True,
        _json_snapshot(row["default"], _property_path(name)),
    )


def _decode_enumeration(
    outcome_value: object,
) -> tuple[AdmittedProperty, ...]:
    outcome = _successful_outcome(outcome_value, "/Properties")
    if outcome.changed_set:
        _invalid_result("/Properties", "enumeration returned changes")
    rows = _enumeration_rows(outcome)
    handles = _enumeration_handles(outcome)
    return _decode_rows(rows, handles)


def _decode_rows(
    rows: list[JsonValue],
    handles: tuple[OpaqueHandleProxy, ...],
) -> tuple[AdmittedProperty, ...]:
    decoded = _decode_rows_in_provider_order(rows, handles)
    return tuple(sorted(decoded, key=lambda item: item.name))


def _decode_rows_in_provider_order(
    rows: list[JsonValue],
    handles: tuple[OpaqueHandleProxy, ...],
) -> tuple[AdmittedProperty, ...]:
    if len(rows) != len(handles):
        raise contract_error(
            "/Properties",
            "Generic.InvalidPropertyDescriptor",
            "enumeration row and handle counts differ",
        )
    decoded = tuple(_decode_row(row, handle) for row, handle in zip(rows, handles))
    _reject_duplicate_names(decoded)
    return decoded


def _decode_batch_enumeration(outcome_value: object) -> AdmittedPropertyBatch:
    outcome = _successful_outcome(outcome_value, "/Properties")
    if outcome.changed_set:
        _invalid_result("/Properties", "batch enumeration returned changes")
    rows = _enumeration_rows(outcome)
    property_handles, property_batch = _batch_enumeration_handles(outcome)
    return AdmittedPropertyBatch(
        _decode_rows_in_provider_order(rows, property_handles),
        property_batch,
    )


def _enumeration_rows(outcome: PrimitiveOutcome) -> list[JsonValue]:
    if type(outcome.value) is not dict or set(outcome.value) != {"properties"}:
        raise contract_error(
            "/Properties",
            "Generic.InvalidPropertyDescriptor",
            "enumeration value must contain only properties",
        )
    rows = outcome.value["properties"]
    if type(rows) is not list:
        raise contract_error(
            "/Properties",
            "Generic.InvalidPropertyDescriptor",
            "enumeration rows must be an exact list",
        )
    return rows


def _enumeration_handles(outcome: PrimitiveOutcome) -> tuple[OpaqueHandleProxy, ...]:
    if len(outcome.handle_results) != 1:
        raise contract_error(
            "/Properties",
            "Generic.InvalidPropertyDescriptor",
            "enumeration requires one property handle group",
        )
    group = outcome.handle_results[0]
    if type(group) is not NamedOpaqueHandleResult or group.name != "properties":
        raise contract_error(
            "/Properties",
            "Generic.InvalidPropertyDescriptor",
            "enumeration property handle group is malformed",
        )
    return group.handles


def _batch_enumeration_handles(
    outcome: PrimitiveOutcome,
) -> tuple[tuple[OpaqueHandleProxy, ...], OpaqueHandleProxy]:
    if len(outcome.handle_results) != 2:
        raise contract_error(
            "/Properties",
            "Generic.InvalidPropertyDescriptor",
            "batch enumeration requires properties and property_batch groups",
        )
    properties, property_batch = outcome.handle_results
    if (
        type(properties) is not NamedOpaqueHandleResult
        or properties.name != "properties"
        or type(property_batch) is not NamedOpaqueHandleResult
        or property_batch.name != "property_batch"
        or len(property_batch.handles) != 1
    ):
        raise contract_error(
            "/Properties",
            "Generic.InvalidPropertyDescriptor",
            "batch enumeration handle groups are malformed",
        )
    return properties.handles, property_batch.handles[0]


def _reject_duplicate_names(admitted: tuple[AdmittedProperty, ...]) -> None:
    seen: set[str] = set()
    for item in admitted:
        if item.name in seen:
            raise contract_error(
                _property_path(item.name),
                "Generic.DuplicatePropertyName",
                "admitted property names must be unique",
            )
        seen.add(item.name)


def _lookup(admitted: tuple[AdmittedProperty, ...]) -> dict[str, AdmittedProperty]:
    if type(admitted) is not tuple or any(type(item) is not AdmittedProperty for item in admitted):
        raise contract_error(
            "/Properties",
            "Generic.InvalidPropertyDescriptor",
            "admitted properties must be an exact tuple",
        )
    _reject_duplicate_names(admitted)
    return {item.name: item for item in admitted}


def _validate_authored(
    properties: JsonValue,
    admitted: tuple[AdmittedProperty, ...],
) -> tuple[dict[str, JsonValue], dict[str, AdmittedProperty]]:
    if type(properties) is not dict:
        raise contract_error(
            "/Properties",
            "Generic.InvalidPropertiesType",
            "Properties must be an exact object",
        )
    DocumentCodec().validate(properties)
    by_name = _lookup(admitted)
    for name in sorted(properties):
        item = by_name.get(name)
        if item is None:
            raise contract_error(
                _property_path(name),
                "Generic.UnknownProperty",
                "property is not admitted",
            )
        if not item.writable:
            raise contract_error(
                _property_path(name),
                "Generic.NonWritableProperty",
                "property is not writable",
            )
    return properties, by_name


def _handle_arguments(
    **handles: OpaqueHandleProxy,
) -> tuple[NamedOpaqueHandleArgument, ...]:
    return tuple(
        NamedOpaqueHandleArgument(name, handle)
        for name, handle in handles.items()
    )


def _declared_arguments(declared: tuple[str, ...]) -> dict[str, JsonValue]:
    return {_DECLARED_ARGUMENT: list(declared)} if declared else {}


def _enumerate_batch(
    asset_handle: OpaqueHandleProxy,
    context: GenericPrimitiveContext,
    declared: tuple[str, ...] = (),
) -> AdmittedPropertyBatch:
    return _decode_batch_enumeration(
        context.invoke(
            requirement=requirement(_ENUMERATE_BATCH_ID),
            value_arguments=_declared_arguments(declared),
            handle_arguments=(NamedOpaqueHandleArgument("asset", asset_handle),),
        )
    )


def _batch_rows(
    properties: dict[str, JsonValue],
    admitted: tuple[AdmittedProperty, ...],
) -> list[dict[str, JsonValue]]:
    return [
        {
            "name": item.name,
            "value": _json_snapshot(
                properties[item.name],
                _property_path(item.name),
            ),
        }
        for item in admitted
        if item.name in properties
    ]


def _invoke_preflight(
    context: GenericPrimitiveContext,
    scratch_handle: OpaqueHandleProxy,
    item: AdmittedProperty,
    value: JsonValue,
) -> PrimitiveOutcome:
    return context.invoke(
        requirement=requirement(_PREFLIGHT_ID),
        value_arguments={"value": _json_snapshot(value, _property_path(item.name))},
        handle_arguments=_handle_arguments(
            scratch=scratch_handle,
            property=item.property_handle,
        ),
    )


# The read result for a property that holds an editor-derived reference: a
# non-instanced reference to an object inside the asset, which the asset's own
# code creates (e.g. UPaperSprite.BodySetup). Documents omit such a property.
_EDITOR_DERIVED = object()


def _read_property_value(
    asset_handle: OpaqueHandleProxy,
    item: AdmittedProperty,
    context: GenericPrimitiveContext,
    as_authored: JsonValue = None,
    sparse: bool = False,
) -> JsonValue:
    """The live value; with ``as_authored`` (a dict or list), shaped like that
    authored value: struct members and instanced ``Properties`` it omits are
    left out (Apply keeps them), so a partial value compares exactly.  With
    ``sparse`` (extract's diff_only), struct members and array elements' members
    equal to the class default's are left out (Apply, an edit of the value, keeps
    them).  ``_EDITOR_DERIVED`` when the property holds an editor-derived reference."""
    path = _property_path(item.name)
    if as_authored is not None:
        arguments = {"as_authored": _json_snapshot(as_authored, path)}
    else:
        arguments = {"sparse": True} if sparse else {}
    outcome = _successful_outcome(
        context.invoke(
            requirement=requirement(_READ_ID),
            value_arguments=arguments,
            handle_arguments=_handle_arguments(
                asset=asset_handle,
                property=item.property_handle,
            ),
        ),
        path,
    )
    _require_read_only_lanes(outcome, path)
    if outcome.value == {"editor_derived": True}:
        return _EDITOR_DERIVED
    if type(outcome.value) is not dict or set(outcome.value) != {"value"}:
        _invalid_result(path, "read must return exactly one value")
    return _json_snapshot(outcome.value["value"], path)


# An asset's import record (``AssetImportData``) is made by the asset itself on creation (PostInitProperties; the class
# default has none): one of the base class without any value set (no source file) is the default an Apply recreates.
_IMPORT_RECORD = ("AssetImportData", "/Script/Engine.AssetImportData")


def _is_empty_import_record(value: JsonValue, item: AdmittedProperty) -> bool:
    return (item.name == _IMPORT_RECORD[0] and type(value) is dict and value.get("Class") == _IMPORT_RECORD[1]
            and not value.get("Properties") and set(value) <= {"Class", "Properties"})


def _include_extracted_value(
    value: JsonValue,
    item: AdmittedProperty,
    include_defaults: bool,
) -> bool:
    """Every value with include_defaults (Verify and Diff read every value); otherwise (extract diff_only) the values
    that differ from the class default, an empty import record left out."""
    if include_defaults:
        return True
    if _is_empty_import_record(value, item):
        return False
    if not item.default_value.is_set:
        return True
    return not canonical_equal(value, item.default_value.value)


class PropertySemanticAdapter:
    """Reflected property surface of one class.

    ``component`` (optional) declares extra non-edit properties and per-property
    value codecs; documents carry the codec's document form while primitives
    carry the native typed value.
    """

    __slots__ = ("_declared", "_codecs")

    def __init__(self, component: GenericPropertyComponent | None = None) -> None:
        if component is not None and type(component) is not GenericPropertyComponent:
            raise contract_error(
                "/Properties",
                "Generic.InvalidPropertyAdmission",
                "property component must be a GenericPropertyComponent",
            )
        self._declared: tuple[str, ...] = () if component is None else component.admission.names
        self._codecs: Mapping[str, PropertyValueCodec] = (
            MappingProxyType({}) if component is None else component.value_codecs
        )

    @property
    def declared_names(self) -> tuple[str, ...]:
        return self._declared

    def to_native(self, properties: dict[str, JsonValue]) -> dict[str, JsonValue]:
        """Document property values -> native typed codec values (validating codec inputs)."""
        return {
            name: (
                self._codecs[name].from_document(value, _property_path(name))
                if name in self._codecs
                else value
            )
            for name, value in properties.items()
        }

    def to_document(self, name: str, value: JsonValue) -> JsonValue:
        codec = self._codecs.get(name)
        if codec is None:
            return integral_numbers_as_integers(value)
        return codec.to_document(value, _property_path(name))

    def canonicalize(self, properties: dict[str, JsonValue]) -> dict[str, JsonValue]:
        """Canonical document form of authored values (value codec, else integral numbers as integers)."""
        return {
            name: (
                self._codecs[name].to_document(
                    self._codecs[name].from_document(value, _property_path(name)),
                    _property_path(name),
                )
                if name in self._codecs
                else integral_numbers_as_integers(value)
            )
            for name, value in properties.items()
        }

    def enumerate(
        self,
        exact_asset_class_path: str,
        context: GenericPrimitiveContext,
    ) -> tuple[AdmittedProperty, ...]:
        if type(exact_asset_class_path) is not str or not exact_asset_class_path:
            raise contract_error(
                "/Class",
                "Generic.InvalidPropertyDescriptor",
                "exact asset class path must be a non-empty string",
            )
        outcome = context.invoke(
            requirement=requirement(_ENUMERATE_ID),
            value_arguments={
                "exact_asset_class_path": exact_asset_class_path,
                **_declared_arguments(self._declared),
            },
            handle_arguments=(),
        )
        return _decode_enumeration(outcome)

    def validate(
        self,
        properties: JsonValue,
        admitted: tuple[AdmittedProperty, ...],
    ) -> None:
        authored, _ = _validate_authored(properties, admitted)
        self.to_native(authored)

    def preflight(
        self,
        scratch_handle: OpaqueHandleProxy,
        properties: dict[str, JsonValue],
        admitted: tuple[AdmittedProperty, ...],
        context: GenericPrimitiveContext,
    ) -> None:
        if context.phase is not RuntimePhase.PREFLIGHT:
            raise contract_error(
                "/Phase",
                "Generic.InvalidMutationPhase",
                "property preflight requires PREFLIGHT",
            )
        authored, by_name = _validate_authored(properties, admitted)
        native = self.to_native(authored)
        for name in sorted(native):
            outcome = _successful_outcome(
                _invoke_preflight(
                    context,
                    scratch_handle,
                    by_name[name],
                    native[name],
                ),
                _property_path(name),
            )
            _require_read_only_lanes(outcome, _property_path(name))
            _require_preflight_result(outcome.value, native[name], _property_path(name))

    def apply(
        self,
        read_asset_handle: OpaqueHandleProxy,
        mutable_asset_handle: OpaqueHandleProxy,
        properties: dict[str, JsonValue],
        admitted: tuple[AdmittedProperty, ...],
        context: GenericPrimitiveContext,
        *,
        phase: RuntimePhase,
    ) -> tuple[ChangedRecord, ...]:
        if phase not in GENERIC_RUNTIME_MUTATION_PHASES or context.phase is not phase:
            raise contract_error(
                "/Phase",
                "Generic.InvalidMutationPhase",
                "mutation phase must match the bound context",
            )
        authored, _ = _validate_authored(properties, admitted)
        native = self.to_native(authored)
        batch = _enumerate_batch(read_asset_handle, context, self._declared)
        _validate_authored(native, batch.properties)
        outcome = _successful_outcome(
            context.invoke(
                requirement=requirement(_WRITE_BATCH_ID),
                value_arguments={
                    "properties": _batch_rows(native, batch.properties),
                    **_declared_arguments(self._declared),
                },
                handle_arguments=_handle_arguments(
                    asset=mutable_asset_handle,
                    property_batch=batch.property_batch_handle,
                ),
            ),
            "/Properties",
        )
        _require_no_handles(outcome, "/Properties")
        if not _is_no_value(outcome.value):
            _invalid_result(
                "/Properties",
                "batch write must return changes only in its typed lane",
            )
        return outcome.changed_set

    def extract(
        self,
        asset_handle: OpaqueHandleProxy,
        admitted: tuple[AdmittedProperty, ...],
        context: GenericPrimitiveContext,
        *,
        include_defaults: bool,
        authored: dict[str, JsonValue] | None = None,
    ) -> dict[str, JsonValue]:
        """``authored`` (document form): read each structured authored value
        in its shape, so Diff and Verify compare partial values exactly."""
        if type(include_defaults) is not bool:
            _invalid_result("/Properties", "include_defaults must be an exact bool")
        native_authored = self.to_native({
            name: value
            for name, value in (authored or {}).items()
            if type(value) in (dict, list)
        })
        # Only writable rows are extracted: Apply rejects non-writable properties
        # (Generic.NonWritableProperty), so an extracted document must stay appliable.
        readable = tuple(sorted((item for item in admitted if item.readable and item.writable), key=lambda item: item.name))
        output: dict[str, JsonValue] = {}
        for item in readable:
            value = _read_property_value(asset_handle, item, context, native_authored.get(item.name))
            if value is _EDITOR_DERIVED:
                continue
            if item.name in native_authored or _include_extracted_value(value, item, include_defaults):
                if not include_defaults and item.name not in native_authored and item.name not in self._codecs \
                        and type(value) in (dict, list):
                    # diff_only: a struct (also inside arrays) lists only its members that differ from the default.
                    value = _read_property_value(asset_handle, item, context, sparse=True)
                output[item.name] = self.to_document(item.name, value)
        return output
