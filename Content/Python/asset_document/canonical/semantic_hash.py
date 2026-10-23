# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from enum import Enum
import hashlib

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.diagnostics import contract_error
from asset_document.dto import JsonValue


class SemanticHashScope(str, Enum):
    DOCUMENT = "document"
    REGION = "region"


def compute_semantic_hash(
    value: JsonValue,
    *,
    scope: SemanticHashScope,
    codec: DocumentCodec = DocumentCodec(),
) -> str:
    if type(scope) is not SemanticHashScope:
        raise contract_error(
            "/SemanticHash/Scope",
            "Hash.InvalidScope",
            "Semantic hash scope must be an exact SemanticHashScope.",
        )
    digest_input = (
        b"AssetDocumentSemanticHash\0"
        + f"v{codec.version}".encode("ascii")
        + b"\0"
        + scope.value.encode("ascii")
        + b"\0"
        + codec.encode(value)
    )
    digest = hashlib.sha256(digest_input).hexdigest()
    return f"adsh:v{codec.version}:{scope.value}:{digest}"
