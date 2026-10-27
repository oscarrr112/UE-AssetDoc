# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Per-region evidence and sidecar bytes for sidecar mirroring.

A region is the unit of change detection between a sidecar and its asset: ``/Class``, ``/Properties`` and each
top-level ``/Body/<Key>``. Its evidence is a semantic hash with region scope over ``{"Kind": "Present", "Value": ...}``
or ``{"Kind": "Absent"}``. A sidecar region that uses ``DefinitionRef`` fragments also hashes the definitions it reaches,
so editing a definition changes exactly the regions that use it. ``Target``, ``SchemaVersion``, ``Action``,
``BaseRevision`` and ``_meta`` are not regions.

The native side asks through the read-only ``Sync`` operation with ``canonical_document``:

- ``{"Mode": "evidence", "Text": <document text>}``: the region hashes of a sidecar or of an extract;
- ``{"Mode": "mirror", "Text": <extract text>, "Package": <saved hash>}``: the mirror file (the extract as
  CreateOrUpdate, ``Body._Skipped`` moved to ``_meta.Skipped``, ``_meta.sync`` stamped with its own region hashes);
- ``{"Mode": "merge", "Text": <sidecar text>, "From": <extract text>, "Regions": [...]}``: the sidecar with those
  regions taken from the extract, everything else (``_meta`` included) as it was;
- ``{"Mode": "conflict", "Text": <extract text>, "Regions": [...]}``: the asset side of a conflict, with
  ``_meta.conflict.Regions``.

Every mode returns ``{"Regions": {pointer: hash}, "AbsentHash": hash}`` (of the resulting document for the writing
modes) and, for the writing modes, the file bytes.
"""

from __future__ import annotations

import copy

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.canonical.semantic_hash import SemanticHashScope, compute_semantic_hash
from asset_document.diagnostics import contract_error
from asset_document.dto import (
    OptionalJsonValue,
    RuntimePhase,
    RuntimeRequestDto,
    RuntimeResultDto,
    TerminalStatus,
)

META_FIELD = "_meta"
SYNC_META_VERSION = 1
_NOT_REGIONS = ("SchemaVersion", "Target", "Action", "BaseRevision", "AssetType", "FactoryContext", META_FIELD)
_SKIPPED = "_Skipped"


def _error(path: str, message: str):
    return contract_error(path, "Sync.InvalidSidecar", message)


def escape_pointer_segment(segment: str) -> str:
    return segment.replace("~", "~0").replace("/", "~1")


def _unescape_pointer_segment(segment: str) -> str:
    return segment.replace("~1", "/").replace("~0", "~")


def decode_document(text: object, path: str = "/Text") -> dict:
    if type(text) is not str:
        raise _error(path, "The document text must be a string.")
    try:
        value = DocumentCodec().decode(text.encode("utf-8"))
    except Exception as failure:
        detail = getattr(getattr(failure, "diagnostic", None), "message", None) or str(failure)
        raise _error(path, f"The file is not a valid JSON document ({detail[:200]}).") from None
    if type(value) is not dict:
        raise _error(path, "The document must be a JSON object.")
    return value


def _body(document: dict) -> dict:
    body = document.get("Body")
    return body if type(body) is dict else {}


def region_pointers(document: dict) -> list[str]:
    pointers = ["/Class", "/Properties"]
    pointers.extend("/Body/" + escape_pointer_segment(key) for key in _body(document) if key != _SKIPPED)
    return pointers


def _region_value(document: dict, pointer: str) -> tuple[bool, object]:
    if pointer == "/Class":
        return ("Class" in document, document.get("Class"))
    if pointer == "/Properties":
        return ("Properties" in document, document.get("Properties"))
    if pointer.startswith("/Body/"):
        key = _unescape_pointer_segment(pointer[len("/Body/"):])
        body = document.get("Body")
        if type(body) is dict and key in body:
            return (True, body[key])
        return (False, None)
    raise _error("/Regions", f"Unknown region '{pointer}'.")


def _definition_ids(value: object, out: list[str]) -> None:
    if type(value) is dict:
        if value.get("Kind") == "DefinitionRef" and type(value.get("Id")) is str:
            out.append(value["Id"])
        for item in value.values():
            _definition_ids(item, out)
    elif type(value) is list:
        for item in value:
            _definition_ids(item, out)


def _reached_definitions(value: object, definitions: dict) -> dict:
    reached: dict = {}
    pending: list[str] = []
    _definition_ids(value, pending)
    while pending:
        identifier = pending.pop()
        if identifier in reached or identifier not in definitions:
            continue
        reached[identifier] = definitions[identifier]
        _definition_ids(definitions[identifier], pending)
    return reached


def _hash(evidence: dict, codec: DocumentCodec) -> str:
    return compute_semantic_hash(evidence, scope=SemanticHashScope.REGION, codec=codec)


def absent_hash(codec: DocumentCodec | None = None) -> str:
    return _hash({"Kind": "Absent"}, codec or DocumentCodec())


def region_hashes(document: dict, codec: DocumentCodec | None = None) -> dict[str, str]:
    codec = codec or DocumentCodec()
    definitions = document.get("Definitions")
    definitions = definitions if type(definitions) is dict else {}
    hashes: dict[str, str] = {}
    for pointer in region_pointers(document):
        present, value = _region_value(document, pointer)
        if not present:
            hashes[pointer] = absent_hash(codec)
            continue
        evidence: dict = {"Kind": "Present", "Value": value}
        reached = _reached_definitions(value, definitions) if definitions else {}
        if reached:
            evidence["Definitions"] = reached
        hashes[pointer] = _hash(evidence, codec)
    return hashes


def content_of(document: dict) -> dict:
    """The document without the fields that are not authored content (``_meta``, ``BaseRevision``)."""
    return {key: value for key, value in document.items() if key not in (META_FIELD, "BaseRevision")}


def _without_skipped(document: dict) -> tuple[dict, object]:
    body = document.get("Body")
    if type(body) is not dict or _SKIPPED not in body:
        return (document, None)
    result = dict(document)
    result["Body"] = {key: value for key, value in body.items() if key != _SKIPPED}
    return (result, body[_SKIPPED])


def mirror_document(extract: dict, package_hash: str, codec: DocumentCodec | None = None) -> dict:
    codec = codec or DocumentCodec()
    document, skipped = _without_skipped(content_of(extract))
    document = dict(document)
    if document.get("Action") == "Create":
        document["Action"] = "CreateOrUpdate"
    hashes = region_hashes(document, codec)
    meta: dict = {
        "sync": {
            "Version": SYNC_META_VERSION,
            "Package": package_hash,
            "Regions": {pointer: {"Sidecar": value, "Asset": value} for pointer, value in hashes.items()},
        }
    }
    if skipped is not None:
        meta["Skipped"] = skipped
    document[META_FIELD] = meta
    return document


def _set_region(document: dict, pointer: str, present: bool, value: object) -> None:
    if pointer in ("/Class", "/Properties"):
        key = pointer[1:]
        if present:
            document[key] = copy.deepcopy(value)
        else:
            document.pop(key, None)
        return
    key = _unescape_pointer_segment(pointer[len("/Body/"):])
    body = document.get("Body")
    body = dict(body) if type(body) is dict else {}
    if present:
        body[key] = copy.deepcopy(value)
    else:
        body.pop(key, None)
    document["Body"] = body


def merge_document(sidecar: dict, extract: dict, pointers: list[str]) -> dict:
    result = dict(sidecar)
    source, _ = _without_skipped(content_of(extract))
    for pointer in pointers:
        present, value = _region_value(source, pointer)
        _set_region(result, pointer, present, value)
    return result


def conflict_document(extract: dict, pointers: list[str]) -> dict:
    document, skipped = _without_skipped(content_of(extract))
    document = dict(document)
    meta: dict = {"conflict": {"Regions": list(pointers)}}
    if skipped is not None:
        meta["Skipped"] = skipped
    document[META_FIELD] = meta
    return document


def _pointers(value: object) -> list[str]:
    if type(value) is not list or any(type(item) is not str for item in value):
        raise _error("/Regions", "Regions must be a list of JSON Pointers.")
    for item in value:
        if item not in ("/Class", "/Properties") and not item.startswith("/Body/"):
            raise _error("/Regions", f"Unknown region '{item}'.")
    return list(value)


def evaluate(options: object) -> tuple[dict, bytes]:
    if type(options) is not dict:
        raise _error("/", "The Sync request must be an object.")
    codec = DocumentCodec()
    mode = options.get("Mode")
    document = decode_document(options.get("Text"))
    written: dict | None = None
    if mode == "evidence":
        hashed = content_of(document)
    elif mode == "mirror":
        package = options.get("Package", "")
        if type(package) is not str:
            raise _error("/Package", "Package must be a string.")
        written = mirror_document(document, package, codec)
        hashed = content_of(written)
    elif mode == "merge":
        written = merge_document(document, decode_document(options.get("From"), "/From"), _pointers(options.get("Regions")))
        hashed = content_of(written)
    elif mode == "conflict":
        written = conflict_document(document, _pointers(options.get("Regions")))
        hashed = content_of(written)
    else:
        raise _error("/Mode", "Mode must be evidence, mirror, merge or conflict.")
    value = {"Regions": region_hashes(_without_skipped(hashed)[0], codec), "AbsentHash": absent_hash(codec)}
    return (value, codec.encode_sidecar(written) if written is not None else b"")


def dispatch_sync(request: RuntimeRequestDto, revision: int) -> RuntimeResultDto:
    value, encoded = evaluate(request.canonical_document)
    codec = DocumentCodec()
    # Byte evidence is complete or absent: the written bytes come with their document hash and codec.
    semantic_hash = compute_semantic_hash(codec.decode(encoded), scope=SemanticHashScope.DOCUMENT, codec=codec) if encoded else ""
    return RuntimeResultDto(
        request_id=request.request_id,
        terminal_status=TerminalStatus.SUCCEEDED,
        final_phase=RuntimePhase.COMPLETE,
        runtime_revision=revision,
        diagnostics=(),
        canonical_value=OptionalJsonValue(True, value),
        canonical_bytes=encoded,
        semantic_hash=semantic_hash,
        codec_version=codec.version if encoded else 0,
        changed_set=(),
    )
