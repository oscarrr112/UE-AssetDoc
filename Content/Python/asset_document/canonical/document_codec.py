from __future__ import annotations

from dataclasses import dataclass, field
import json
import math
from typing import NoReturn

from asset_document.diagnostics import AssetDocumentContractError, contract_error
from asset_document.dto import JsonValue


CODEC_VERSION: int = 1
MAXIMUM_DEPTH: int = 64
# Whole-document safety bound (memory), not a per-call budget: primitive invocation values
# and outcomes keep the host's 4096-node per-call budget (RuntimeValueMaximumNodes), and
# providers page / chunk their calls under it. Matches RuntimeDocumentMaximumNodes (C++).
MAXIMUM_NODES: int = 1 << 20
MINIMUM_INTEGER: int = -(2**63)
MAXIMUM_INTEGER: int = 2**63 - 1


@dataclass(slots=True)
class _ValidationState:
    nodes: int = 0
    active_container_ids: set[int] = field(default_factory=set)


def _display_path(path: str) -> str:
    return path or "/"


def _escape_pointer(segment: object) -> str:
    return str(segment).replace("~", "~0").replace("/", "~1")


def _child_path(parent: str, segment: object) -> str:
    child = _escape_pointer(segment)
    return f"{parent}/{child}" if parent else f"/{child}"


def _raise_codec(path: str, code: str, message: str) -> NoReturn:
    raise contract_error(_display_path(path), code, message)


def _validate_string(value: str, path: str) -> None:
    try:
        value.encode("utf-8")
    except UnicodeEncodeError:
        _raise_codec(path, "Codec.InvalidUnicode", "Strings must contain valid Unicode scalars.")


def _enter_node(path: str, depth: int, state: _ValidationState) -> None:
    state.nodes += 1
    if state.nodes > MAXIMUM_NODES:
        _raise_codec(path, "Codec.NodeLimitExceeded", f"JSON value exceeds {MAXIMUM_NODES} nodes.")
    if depth > MAXIMUM_DEPTH:
        _raise_codec(path, "Codec.DepthExceeded", "JSON value exceeds depth 64.")


def _activate_container(value: JsonValue, path: str, state: _ValidationState) -> int:
    identity = id(value)
    if identity in state.active_container_ids:
        _raise_codec(path, "Codec.CycleDetected", "JSON value contains an active container cycle.")
    state.active_container_ids.add(identity)
    return identity


def _validate_list(
    value: list[JsonValue], path: str, depth: int, state: _ValidationState
) -> None:
    identity = _activate_container(value, path, state)
    try:
        for index, item in enumerate(value):
            _validate_node(item, _child_path(path, index), depth + 1, state)
    finally:
        state.active_container_ids.remove(identity)


def _validate_key(key: object, path: str) -> str:
    if type(key) is not str:
        _raise_codec(path, "Codec.InvalidType", "JSON object keys must be exact strings.")
    try:
        key.encode("utf-8")
    except UnicodeEncodeError:
        _raise_codec(_child_path(path, "Key"), "Codec.InvalidUnicode", "Object keys must be valid Unicode.")
    return key


def _validate_dict(
    value: dict[str, JsonValue], path: str, depth: int, state: _ValidationState
) -> None:
    identity = _activate_container(value, path, state)
    try:
        for key, item in value.items():
            valid_key = _validate_key(key, path)
            _validate_node(item, _child_path(path, valid_key), depth + 1, state)
    finally:
        state.active_container_ids.remove(identity)


def _validate_scalar(value: JsonValue, path: str) -> bool:
    if value is None or type(value) is bool:
        return True
    if type(value) is int:
        if value < MINIMUM_INTEGER or value > MAXIMUM_INTEGER:
            _raise_codec(path, "Codec.IntegerOutOfRange", "Integers must fit signed int64.")
        return True
    if type(value) is float:
        if not math.isfinite(value):
            _raise_codec(path, "Codec.NonFiniteNumber", "JSON numbers must be finite.")
        return True
    if type(value) is str:
        _validate_string(value, path)
        return True
    return False


def _validate_node(
    value: JsonValue, path: str, depth: int, state: _ValidationState
) -> None:
    _enter_node(path, depth, state)
    if _validate_scalar(value, path):
        return
    if type(value) is list:
        _validate_list(value, path, depth, state)
        return
    if type(value) is dict:
        _validate_dict(value, path, depth, state)
        return
    _raise_codec(path, "Codec.InvalidType", "Value is not an exact builtin JSON type.")


def integral_numbers_as_integers(value: JsonValue) -> JsonValue:
    """Document number form: a float with an integral value is written as an integer.

    JSON has one number type and UE's JSON writer prints the float 1.0 as ``1``,
    so a document that went through any UE serialization (an MCP response, a
    sidecar written by C++) carries ``1`` where the property holds 1.0. Reflected
    property values use this form on both sides of every comparison, so ``1`` and
    ``1.0`` authored for a float property mean the same value. Non-integral floats
    keep their exact binary value (no rounding is introduced here).
    """
    if type(value) is float:
        if math.isfinite(value) and value.is_integer() and MINIMUM_INTEGER <= value < 2**63:
            return int(value)
        return value
    if type(value) is list:
        return [integral_numbers_as_integers(item) for item in value]
    if type(value) is dict:
        return {key: integral_numbers_as_integers(item) for key, item in value.items()}
    return value


def _reject_constant(token: str) -> NoReturn:
    raise contract_error("/", "Codec.NonFiniteNumber", "JSON numbers must be finite.")


def _unique_object(pairs: list[tuple[str, JsonValue]]) -> dict[str, JsonValue]:
    result: dict[str, JsonValue] = {}
    for key, value in pairs:
        if key in result:
            raise contract_error(
                "/", "Codec.DuplicateObjectKey", f"Duplicate object key: {key}"
            )
        result[key] = value
    return result


SIDECAR_INDENT: str = "  "
SIDECAR_INLINE_WIDTH: int = 100


def _scalar_text(value: JsonValue) -> str:
    return json.dumps(value, ensure_ascii=False, allow_nan=False)


def _write_sidecar(value: JsonValue, depth: int, out: list[str], prefix: str) -> None:
    """Appends prefix + the indented form of value (no trailing newline)."""
    if type(value) is dict:
        if not value:
            out.append(prefix + "{}")
            return
        out.append(prefix + "{\n")
        inner = SIDECAR_INDENT * (depth + 1)
        for index, key in enumerate(sorted(value)):
            _write_sidecar(value[key], depth + 1, out, f"{inner}{_scalar_text(key)}: ")
            out.append(",\n" if index + 1 < len(value) else "\n")
        out.append(SIDECAR_INDENT * depth + "}")
        return
    if type(value) in (list, tuple):
        if not value:
            out.append(prefix + "[]")
            return
        if all(type(item) not in (dict, list, tuple) for item in value):
            line = "[" + ", ".join(_scalar_text(item) for item in value) + "]"
            if len(prefix) + len(line) <= SIDECAR_INLINE_WIDTH:
                out.append(prefix + line)
                return
        out.append(prefix + "[\n")
        inner = SIDECAR_INDENT * (depth + 1)
        for index, item in enumerate(value):
            _write_sidecar(item, depth + 1, out, inner)
            out.append(",\n" if index + 1 < len(value) else "\n")
        out.append(SIDECAR_INDENT * depth + "]")
        return
    out.append(prefix + _scalar_text(value))


@dataclass(frozen=True, slots=True)
class DocumentCodec:
    version: int = CODEC_VERSION

    def __post_init__(self) -> None:
        if type(self.version) is not int or self.version != CODEC_VERSION:
            raise contract_error(
                "/CodecVersion",
                "Codec.UnsupportedVersion",
                "Only AssetDocument codec v1 is supported.",
            )

    def validate(self, value: JsonValue) -> None:
        _validate_node(value, "", 0, _ValidationState())

    def encode(self, value: JsonValue) -> bytes:
        self.validate(value)
        encoded = json.dumps(
            value,
            ensure_ascii=False,
            allow_nan=False,
            sort_keys=True,
            separators=(",", ":"),
        )
        return encoded.encode("utf-8")

    def encode_sidecar(self, value: JsonValue) -> bytes:
        """The sidecar file form: the same document as ``encode`` (sorted keys, shortest numbers), indented two
        spaces per level so that a person can read and edit it after an apply wrote it back. An array of scalars
        that fits a line stays on one line ([-400, 0]); a final newline ends the file. The semantic hash is over
        ``encode`` and does not depend on this layout."""
        self.validate(value)
        lines: list[str] = []
        _write_sidecar(value, 0, lines, "")
        return ("".join(lines) + "\n").encode("utf-8")

    def decode(self, payload: bytes) -> JsonValue:
        if type(payload) is not bytes:
            raise contract_error(
                "/", "Codec.InvalidType", "DocumentCodec.decode requires exact bytes."
            )
        try:
            text = payload.decode("utf-8", errors="strict")
        except UnicodeDecodeError:
            raise contract_error(
                "/", "Codec.InvalidUtf8", "Document bytes must be strict UTF-8."
            ) from None
        try:
            value = json.loads(
                text,
                parse_constant=_reject_constant,
                object_pairs_hook=_unique_object,
            )
        except AssetDocumentContractError:
            raise
        except json.JSONDecodeError:
            raise contract_error(
                "/", "Codec.MalformedJson", "Document bytes contain malformed JSON."
            ) from None
        self.validate(value)
        return value
