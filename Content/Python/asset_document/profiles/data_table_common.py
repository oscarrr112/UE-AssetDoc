# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Shared pieces of the data table family Profiles (DataTable, CurveTable, StringTable).

Transport over the ``data.*`` primitives (provider "data", AssetDocumentData), document header
validation, the Properties region over the generic Editor property admission, value comparison
and op chunking.  See docs/architecture/asset-document-datatable-matrix.md.
"""
from __future__ import annotations

import math
import re
import struct

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.diagnostics import contract_error, primitive_error
from asset_document.diff.model import Difference, DiffKind
from asset_document.dto import PrimitiveRequirement, RuntimePhase
from asset_document.meta import DocumentAction
from asset_document.regions.primitive_catalog import (
    NamedOpaqueHandleArgument, PrimitiveOutcome, PrimitiveOutcomeStatus,
)
from asset_document.regions.property_semantics import PropertySemanticAdapter
from asset_document.read_only_extract import admits_read_only


ROUTE = "data_profile"
CLASS_RESOLVE = "editor.class.resolve_canonical.v1"
RESOLVE = "editor.asset.resolve_existing.v1"
REFERENCE_RESOLVE = "editor.reference.resolve_asset.v1"
DATA_TABLE_READ = "data.data_table.read.v1"
DATA_TABLE_EDIT = "data.data_table.edit.v1"
CURVE_TABLE_READ = "data.curve_table.read.v1"
CURVE_TABLE_EDIT = "data.curve_table.edit.v1"
STRING_TABLE_READ = "data.string_table.read.v1"
STRING_TABLE_EDIT = "data.string_table.edit.v1"
PROVIDER = "data"
DOCUMENT_KEYS = ("SchemaVersion", "Target", "Class", "Action", "Definitions", "Properties", "Body")
# Ops per edit call stay well below the 4096-node runtime value budget.
OP_NODE_BUDGET = 3000
_TARGET = re.compile(r"/Game/[A-Za-z0-9_/-]+\Z")


def requirements(*primitives):
    return tuple(PrimitiveRequirement(item, 1, ROUTE) for item in primitives)


def error(path, message, code):
    return contract_error(path, code, message)


def escape(name):
    return name.replace("~", "~0").replace("/", "~1")


def row_differences(authored_rows, live_rows, matched, ordered=True):
    """Every difference of a complete authored ``Rows`` list (rows it leaves out are removed, ``RenamedFrom``
    renames a live row) from the live rows, each naming its row: a Remove per removed row, an Add per new row, a
    Replace of ``Name`` per rename, ``matched(index, row, live_row)``'s differences for each row both have, and,
    when ``ordered``, one Replace of ``/Body/Rows`` (the names of the rows both have, in both orders) when only
    their order differs. Validate and Diff count and name each changed row (two changed rows
    read as one)."""
    live = {row["Name"]: row for row in live_rows}
    names = [row["Name"] for row in authored_rows]
    authored = set(names)
    renames = {row["Name"]: row["RenamedFrom"] for row in authored_rows
               if row.get("RenamedFrom") in live and row["Name"] not in live and row["RenamedFrom"] not in authored}
    sources = set(renames.values())
    changes = [Difference("/Body/Rows", DiffKind.REMOVE, False, None, True, row, row=row["Name"])
               for row in live_rows if row["Name"] not in authored and row["Name"] not in sources]
    kept = []
    for index, row in enumerate(authored_rows):
        name = row["Name"]
        source = renames.get(name, name)
        if source not in live:
            changes.append(Difference(f"/Body/Rows/{index}", DiffKind.ADD, True, row, False, None, row=name))
            continue
        kept.append(source)
        if name != source:
            changes.append(Difference(f"/Body/Rows/{index}/Name", DiffKind.REPLACE, True, name, True, source, row=name))
        changes.extend(matched(index, row, live[source]))
    kept_set = set(kept)
    live_order = [row["Name"] for row in live_rows if row["Name"] in kept_set]
    if ordered and kept != live_order:
        changes.append(Difference("/Body/Rows", DiffKind.REPLACE, True, kept, True, live_order))
    return changes


def at_pointer(value, root, path):
    """(present, value) at the JSON Pointer ``path`` of a value whose own path is ``root`` (``path`` starts with it):
    a diff that names the place a region differs pairs that place with its own values, not the region's (the
    VerificationMismatch message and the removal count read a difference's values at its path)."""
    if path != root and not path.startswith(root + "/"):
        return False, None
    for token in path[len(root) + 1:].split("/") if path != root else ():
        token = token.replace("~1", "/").replace("~0", "~")
        if type(value) is dict and token in value:
            value = value[token]
        elif type(value) is list and token.isdigit() and int(token) < len(value):
            value = value[int(token)]
        else:
            return False, None
    return True, value


def snapshot(value):
    codec = DocumentCodec()
    return codec.decode(codec.encode(value))


def require_target(value, prefix):
    if admits_read_only(value):
        return value
    if type(value) is not str or _TARGET.fullmatch(value) is None:
        raise error("/Target", "Target must be a canonical /Game package path such as /Game/Data/DT_Items.",
                    f"{prefix}.NonCanonicalTarget")
    return value


def validate_header(document, class_paths, prefix, body_keys, extra_keys=()):
    """Header of a data table family document; returns the Body object.

    ``extra_keys``: further top-level keys the class admits (MirrorDataTable: ``FactoryContext``)."""
    if type(document) is not dict:
        raise error("/", f"A {prefix} document must be a JSON object.", f"{prefix}.InvalidDocumentType")
    for key in document:
        if key not in DOCUMENT_KEYS and key not in extra_keys:
            raise error(f"/{escape(key)}", f"Unknown document key '{key}'.", f"{prefix}.UnknownDocumentKey")
    if type(document.get("SchemaVersion")) is not int or document["SchemaVersion"] != 1:
        raise error("/SchemaVersion", "SchemaVersion must be 1.", "Meta.InvalidSchemaVersion")
    require_target(document.get("Target"), prefix)
    if document.get("Class") not in class_paths:
        raise error("/Class", f"Class must be {' or '.join(class_paths)}.", f"{prefix}.ClassMismatch")
    if document.get("Action") not in {item.value for item in DocumentAction}:
        raise error("/Action", "Action must be Create, Update or CreateOrUpdate.", f"{prefix}.InvalidAction")
    if document.get("Definitions", {}) != {}:
        raise error("/Definitions", f"Definitions are not authored by the {prefix} Profile.", f"{prefix}.UnsupportedDefinitions")
    for key in ("Properties", "Body"):
        if type(document.get(key, {})) is not dict:
            raise error(f"/{key}", f"{key} must be an object.", f"{prefix}.Invalid{key}Type")
    body = document.get("Body", {})
    for key in body:
        if key not in body_keys:
            raise error(f"/Body/{escape(key)}", f"Unknown Body key '{key}'. Keys: {', '.join(body_keys)}.",
                        f"{prefix}.UnknownBodyKey")
    DocumentCodec().validate(document)
    return body


def validate_name(value, path, prefix, what="row"):
    if type(value) is not str or not value or len(value) >= 1024 or value.strip() != value \
            or any(char in value for char in "\"' ,\n\r\t") or value.lower() == "none":
        raise error(path, f"A {what} name is a non-empty name without spaces, quotes, commas or line breaks "
                          f"(got {value!r}).", f"{prefix}.InvalidRowName")
    return value


# ---------------------------------------------------------------------------
# Values
# ---------------------------------------------------------------------------
def _float32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def _is_number(value):
    return type(value) in (int, float)


def same(left, right, exact=True):
    """Value equality for row values: numbers compare numerically (1 == 1.0); the lenient
    form also accepts values that store as the same float32."""
    if _is_number(left) and _is_number(right):
        if left == right:
            return True
        if exact:
            return False
        try:
            return (math.isfinite(left) and math.isfinite(right)
                    and abs(left) < 3.5e38 and abs(right) < 3.5e38 and _float32(float(left)) == _float32(float(right)))
        except OverflowError:
            return False
    if type(left) is dict and type(right) is dict:
        return left.keys() == right.keys() and all(same(left[key], right[key], exact) for key in left)
    if type(left) in (list, tuple) and type(right) in (list, tuple):
        return len(left) == len(right) and all(same(a, b, exact) for a, b in zip(left, right))
    return type(left) is type(right) and left == right


def count_nodes(value):
    if type(value) is dict:
        return 1 + sum(count_nodes(item) for item in value.values())
    if type(value) in (list, tuple):
        return 1 + sum(count_nodes(item) for item in value)
    return 1


def chunk_ops(ops, budget=OP_NODE_BUDGET):
    """[(op, document_path)] -> chunks whose op values stay under the node budget."""
    chunks, current, size = [], [], 0
    for op, path in ops:
        nodes = count_nodes(op)
        if current and size + nodes > budget:
            chunks.append(current)
            current, size = [], 0
        current.append((op, path))
        size += nodes
    if current:
        chunks.append(current)
    return chunks


# ---------------------------------------------------------------------------
# Transport
# ---------------------------------------------------------------------------
def invoke(context, by_id, primitive, values, handles=(), path="/Body", paths=None, prefix="Data"):
    outcome = context.invoke(requirement=by_id[primitive], value_arguments=values, handle_arguments=handles)
    if type(outcome) is not PrimitiveOutcome:
        raise error("/PrimitiveOutcome", "A typed primitive outcome is required.", f"{prefix}.InvalidPrimitiveResult")
    if outcome.status is not PrimitiveOutcomeStatus.SUCCEEDED:
        if outcome.diagnostics:
            source = outcome.diagnostics[0]
            where = paths(source.path) if paths is not None else path
            # Report the document location the AI wrote; the primitive detail goes to the diagnostic context.
            raise primitive_error(where, source, primitive)
        raise error(path, f"{primitive} did not succeed.", f"{prefix}.PrimitiveFailed")
    return outcome


class DataCalls:
    """Calls to the "data" provider against one table asset (editor handles handed off)."""

    def __init__(self, context, by_id, read, mutable=None, prefix="Data"):
        self.context = context
        self.by_id = by_id
        self.prefix = prefix
        self.read = context.handoff(source_handle=read, target_provider_id=PROVIDER, access="read_only")
        self.write = (context.handoff(source_handle=mutable, target_provider_id=PROVIDER, access="mutable")
                      if mutable is not None else None)

    def query(self, primitive, values=None, path="/Body"):
        outcome = invoke(self.context, self.by_id, primitive, {"schema_version": 1, **(values or {})},
                         (NamedOpaqueHandleArgument("asset", self.read),), path, prefix=self.prefix)
        if type(outcome.value) is not dict:
            raise error("/PrimitiveOutcome", f"{primitive} returned no object.", f"{self.prefix}.InvalidPrimitiveResult")
        return outcome.value

    def query_all(self, primitive, list_key):
        """First page (with its header fields) and every row of a paged read."""
        first = self.query(primitive, {"offset": 0})
        rows = list(first.get(list_key, []))
        offset = first.get("next_offset", -1)
        while type(offset) is int and offset >= 0:
            page = self.query(primitive, {"offset": offset})
            rows += page.get(list_key, [])
            offset = page.get("next_offset", -1)
        return first, rows

    def edit(self, primitive, fixed, ops, base_paths):
        """Runs ops (with their document paths) in node-bounded chunks; returns the changed records."""
        changed = []
        for chunk in chunk_ops(ops):
            op_paths = [path for _, path in chunk]

            def where(primitive_path, op_paths=op_paths):
                return map_path(primitive_path, op_paths, base_paths)
            outcome = invoke(self.context, self.by_id, primitive,
                             {"schema_version": 1, **fixed, "ops": [op for op, _ in chunk]},
                             (NamedOpaqueHandleArgument("asset", self.write),), paths=where, prefix=self.prefix)
            changed.extend(outcome.changed_set)
        return changed


def map_path(primitive_path, op_paths, base_paths):
    """/ops/<n>/<field>/... -> the document path of op n; top-level fields via base_paths."""
    parts = [part for part in (primitive_path or "").split("/") if part]
    if len(parts) >= 2 and parts[0] == "ops" and parts[1].isdigit() and int(parts[1]) < len(op_paths):
        base = op_paths[int(parts[1])]
        rest = parts[2:]
        if not rest:
            return base
        field = rest[0]
        if field in ("values", "curve") and len(rest) > 1:
            return base + {"values": "/Values", "curve": ""}[field] + "".join(f"/{part}" for part in rest[1:])
        if field == "tables" and len(rest) > 1:
            return f"{base}/{rest[1]}"
        return base
    if parts and parts[0] in base_paths:
        return base_paths[parts[0]]
    return "/Body"


# ---------------------------------------------------------------------------
# Properties (Details panel), through the generic Editor property admission
# ---------------------------------------------------------------------------
# The Content Browser's thumbnail state (created on demand when a thumbnail is shown or edited), not
# asset content: a new asset has none, so an extract carrying it could not be applied as Create.
# Not extracted; a document that sets it still applies to an asset that has one.
NOT_EXTRACTED = ("ThumbnailInfo",)


class PropertiesRegion:
    """Top-level Properties of a table asset; Body-owned names are refused."""

    def __init__(self, class_path, body_owned, prefix, editor_state=()):
        self.class_path = class_path
        self.body_owned = dict(body_owned)
        self.prefix = prefix
        # Editor state (for example the asset's ThumbnailInfo) is neither extracted nor authored.
        self.editor_state = tuple(editor_state)
        self.adapter = PropertySemanticAdapter()

    def check_owned(self, properties):
        for name in sorted(properties):
            if name in self.editor_state:
                raise error(f"/Properties/{escape(name)}", f"'{name}' is editor state (the Content Browser thumbnail), "
                            "not asset content; leave it out.", f"{self.prefix}.NotAuthoredProperty")
            if name in self.body_owned:
                raise error(f"/Properties/{escape(name)}", f"'{name}' is authored as {self.body_owned[name]}.",
                            f"{self.prefix}.BodyOwnedProperty")

    def validate(self, properties, context):
        self.check_owned(properties)
        if properties:
            self.adapter.validate(properties, self.adapter.enumerate(self.class_path, context))

    def apply(self, properties, read, mutable, context):
        self.check_owned(properties)
        if not properties:
            return ()
        admitted = self.adapter.enumerate(self.class_path, context)
        return tuple(self.adapter.apply(read, mutable, properties, admitted, context, phase=RuntimePhase.APPLY))

    def skipped(self):
        """Names never read as Properties: the Body-owned ones, editor state and what is not extracted."""
        return set(self.body_owned) | set(NOT_EXTRACTED) | set(self.editor_state)

    def extract(self, read, context, include_defaults=True):
        """include_defaults=False (extract diff_only) lists only values that differ from the class default.

        Names the profile owns are left out before reading, not after: the generic property codec need not
        encode them (a Body-owned value can be far larger than one runtime value, e.g. a MetaSound's frontend
        document or a Niagara script's version data), and Extract, Diff and the save's fresh verification
        read the same set."""
        skipped = self.skipped()
        admitted = tuple(item for item in self.adapter.enumerate(self.class_path, context) if item.name not in skipped)
        values = self.adapter.extract(read, admitted, context, include_defaults=include_defaults)
        return {name: value for name, value in values.items() if name not in skipped}


def properties_diff(authored, live, exact):
    changes = []
    for name, value in authored.items():
        if name not in live or not same(value, live[name], exact):
            changes.append(Difference(f"/Properties/{escape(name)}", DiffKind.REPLACE, True, value,
                                      name in live, live.get(name)))
    return changes


def struct_as_authored(authored, live):
    """``live`` in the shape of ``authored`` for a value codec whose objects are always structs (the graph property
    codec: a map or set is export text): struct members the authored value omits are left out (an edit keeps them),
    arrays of the same length compare element by element; anything else is returned as it is."""
    if type(authored) is dict and type(live) is dict:
        return {key: struct_as_authored(value, live[key]) for key, value in authored.items() if key in live}
    if type(authored) is list and type(live) is list and len(authored) == len(live):
        return [struct_as_authored(mine, theirs) for mine, theirs in zip(authored, live)]
    return live


def structured_property_differences(differences, document):
    """{"/Properties/<name>": name} of the differences at or under a Properties value authored as an object or
    array."""
    properties = document.get("Properties") if type(document) is dict else None
    if type(properties) is not dict or not differences or type(document.get("Class")) is not str:
        return {}
    structured = {f"/Properties/{escape(name)}": name for name, value in properties.items()
                  if type(value) in (dict, list)}
    flagged = {}
    for item in differences:
        path = getattr(item, "path", None)
        for prefix, name in structured.items():
            if type(path) is str and (path == prefix or path.startswith(prefix + "/")):
                flagged[prefix] = name
    return flagged


def properties_as_authored(differences, document, read, context, exact):
    """``differences`` without those a partial structured value explains.

    Properties are read in full, while Apply writes a structured value as an edit of the current one (struct members
    and instanced ``Properties`` it omits keep their value, as on the generic route). A difference at
    ``/Properties/<name>`` (or under it) whose authored value is an object or array is compared again with the live
    value read in the authored shape (the generic property codec's as-authored read: struct members it omits left
    out, map keys and array lengths kept); when they match, its differences are dropped."""
    flagged = structured_property_differences(differences, document)
    if not flagged:
        return differences
    properties = document["Properties"]
    adapter = PropertySemanticAdapter()
    admitted = {item.name: item for item in adapter.enumerate(document["Class"], context)}
    explained = set()
    for prefix, name in flagged.items():
        item = admitted.get(name)
        if item is None or not item.readable or not item.writable:
            continue
        live = adapter.extract(read, (item,), context, include_defaults=True, authored={name: properties[name]})
        if name in live and same(properties[name], live[name], exact):
            explained.add(prefix)
    if not explained:
        return differences
    return tuple(item for item in differences
                 if not any(item.path == prefix or item.path.startswith(prefix + "/") for prefix in explained))


def header_diff(authored, extracted):
    changes = []
    for key in ("SchemaVersion", "Target", "Class"):
        if authored.get(key) != extracted.get(key):
            changes.append(Difference(f"/{key}", DiffKind.REPLACE, True, authored.get(key),
                                      key in extracted, extracted.get(key)))
    return changes


def document(target, class_path, properties, body):
    return {"SchemaVersion": 1, "Target": target, "Class": class_path,
            "Action": DocumentAction.CREATE_OR_UPDATE.value, "Definitions": {}, "Properties": properties,
            "Body": body}


def _name_words(text):
    """The words of a name (as the native NameSuggestions): split at non-alphanumerics, digit / letter changes and
    lower-to-upper changes, lower-cased."""
    words, current = [], ""
    for char in text:
        if not char.isalnum():
            if current:
                words.append(current.lower())
            current = ""
            continue
        if current and (char.isdigit() != current[-1].isdigit() or (char.isupper() and current[-1].islower())):
            words.append(current.lower())
            current = ""
        current += char
    if current:
        words.append(current.lower())
    return words


def _edit_distance(a, b):
    """Edits from a to b; swapping two neighbours is one edit ("Spien" / "Spine")."""
    before, previous = None, list(range(len(b) + 1))
    for i in range(1, len(a) + 1):
        current = [i] + [0] * len(b)
        for j in range(1, len(b) + 1):
            current[j] = min(previous[j] + 1, current[j - 1] + 1, previous[j - 1] + (a[i - 1] != b[j - 1]))
            if i > 1 and j > 1 and a[i - 1] == b[j - 2] and a[i - 2] == b[j - 1]:
                current[j] = min(current[j], before[j - 2] + 1)
        before, previous = previous, current
    return previous[len(b)]


def _words_match(wanted, word):
    """Equal, one the start of the other ("pluck" / "plucky") or one typo apart."""
    if wanted == word:
        return True
    shorter = min(len(wanted), len(word))
    if shorter >= 5 and (wanted.startswith(word) or word.startswith(wanted)):
        return True
    return shorter >= 5 and abs(len(wanted) - len(word)) <= 1 and _edit_distance(wanted, word) <= 1


def _closeness(wanted, wanted_words, candidate, words, common):
    """0 when not close; higher is closer (as the native NameSuggestions)."""
    lower, wanted_lower = candidate.lower(), wanted.lower()
    if not lower or not wanted_lower:
        return 0
    if lower == wanted_lower:
        return 1000
    score = 0
    if min(len(lower), len(wanted_lower)) >= 3 and (wanted_lower in lower or lower in wanted_lower):
        score = 800 - abs(len(lower) - len(wanted_lower))
    distance = _edit_distance(wanted_lower, lower)
    if distance <= max(1, len(wanted_lower) // 4):
        score = max(score, 700 - 10 * distance)
    shared = 0
    for item in wanted_words:
        if len(item) < 3 or item[0].isdigit() or item in common:
            continue
        if item in words:
            shared += 2
        elif any(_words_match(item, word) for word in words):
            shared += 1
    if shared:
        score = max(score, 400 + 25 * shared - min(distance, 50))
    return score


def close_names(wanted, candidates, limit=5):
    """"Did you mean" candidates for a not-found name, closest first, as the native NameSuggestions: equal ignoring
    case, then one containing the other, then a small edit distance, then a shared word (a word most candidates share,
    a naming prefix like "MSS_" or "Bone(", does not count)."""
    unique = list(dict.fromkeys(item for item in candidates if type(item) is str and item != wanted))
    words = [_name_words(item) for item in unique]
    counts = {}
    for item in words:
        for word in set(item):
            counts[word] = counts.get(word, 0) + 1
    common = {word for word, count in counts.items() if count >= 3 and count * 4 >= len(unique)}
    wanted_words = _name_words(wanted)
    scored = [(_closeness(wanted, wanted_words, item, item_words, common), item)
              for item, item_words in zip(unique, words)]
    scored = [entry for entry in scored if entry[0] > 0]
    scored.sort(key=lambda entry: (-entry[0], len(entry[1]), entry[1].casefold(), entry[1]))
    return [item for _, item in scored[:limit]]


def describe_candidates(wanted, candidates, noun, listed=12):
    """A sentence for a not-found message: " Close: a, b." when some are close, else the candidates (sorted, at
    most ``listed``), else ""."""
    close = close_names(wanted, candidates)
    if close:
        return f" Close: {', '.join(close)}."
    names = sorted(set(item for item in candidates if type(item) is str))
    if not names:
        return ""
    more = f" (+{len(names) - listed} more)" if len(names) > listed else ""
    return f" {noun}: {', '.join(names[:listed])}{more}."
