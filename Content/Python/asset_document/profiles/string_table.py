# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Registered StringTable Profile (profile.string_table.v1): read, create and update UStringTables.

Document (docs/architecture/asset-document-datatable-matrix.md)::

    "Body": {
      "Namespace": "UI",                                   # new tables default to the asset name
      "Entries": {"Menu.Start": "Start Game", "Menu.Quit": "Quit"},
      "MetaData": {"Menu.Start": {"Comment": "Main menu button"}}
    }

* ``Entries`` is the full key -> source string map (omit = keep; ``{}`` clears).  A key is
  renamed by removing it and adding the new key, as in the String Table editor.  Source
  strings must not be empty (the editor's Add refuses them).
* ``MetaData`` is the full per-key metadata map (omit = keep; ``{}`` clears); keys must be entries.
  The editor sets entry metadata through CSV import only; the Profile uses the same
  FStringTable::SetMetaData / RemoveMetaData.
* Keys and the namespace follow the editor's identity rule (no ``"' ,\\n\\r\\t`` and no
  package-namespace markers).
"""
from __future__ import annotations

from asset_document.diff.model import Difference, DiffKind
from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, IdentityPolicy, OmissionPolicy, OrderPolicy, ProfileMeta,
    RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document.profiles import data_table_common as common
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements

STRING_TABLE_CLASS = "/Script/Engine.StringTable"
PROFILE_ID = "profile.string_table.v1"
BODY_KEYS = ("Namespace", "Entries", "MetaData")
LIFECYCLE = generic_factory_lifecycle(common.ROUTE)
REQUIREMENTS = (common.requirements(common.CLASS_RESOLVE, common.RESOLVE, common.STRING_TABLE_READ,
                                    common.STRING_TABLE_EDIT)
                + lifecycle_requirements(LIFECYCLE))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}
PREFIX = "StringTable"
_INVALID = "\"' ,\n\r\t"


def _identity(value, path, what):
    if type(value) is not str or not value.strip() or any(char in value for char in _INVALID) \
            or any(marker in value for marker in ("[", "]")):
        raise common.error(path, f"A string table {what} is a non-empty string without spaces, quotes, commas, "
                                 f"line breaks or [ ] (got {value!r}).",
                           "StringTable.InvalidNamespace" if what == "namespace" else "StringTable.InvalidKey")
    return value


def canonical_body(body):
    out = {}
    if "Namespace" in body:
        out["Namespace"] = _identity(body["Namespace"], "/Body/Namespace", "namespace")
    if "Entries" in body:
        entries = body["Entries"]
        if type(entries) is not dict:
            raise common.error("/Body/Entries", "Entries is an object of key -> source string.", "StringTable.InvalidEntries")
        for key, source in entries.items():
            path = f"/Body/Entries/{common.escape(key)}"
            _identity(key, path, "key")
            if type(source) is not str or not source:
                raise common.error(path, "A source string is a non-empty string (the String Table editor does not "
                                         "add empty entries).", "StringTable.EmptySourceString")
        out["Entries"] = dict(entries)
    if "MetaData" in body:
        metadata = body["MetaData"]
        if type(metadata) is not dict:
            raise common.error("/Body/MetaData", "MetaData is an object of key -> {metadata id -> value}.",
                               "StringTable.InvalidMetaData")
        canonical = {}
        for key, values in metadata.items():
            path = f"/Body/MetaData/{common.escape(key)}"
            _identity(key, path, "key")
            if type(values) is not dict or any(type(value) is not str for value in values.values()):
                raise common.error(path, "Entry metadata is an object of metadata id -> string.", "StringTable.InvalidMetaData")
            for name in values:
                common.validate_name(name, f"{path}/{common.escape(name)}", PREFIX, "metadata id")
            if values:
                canonical[key] = dict(values)
            if "Entries" in out and key not in out["Entries"]:
                raise common.error(path, f"MetaData names key '{key}', which is not in Entries."
                                   f"{common.describe_candidates(key, sorted(out['Entries']), 'Entries')}", "StringTable.KeyNotFound")
        out["MetaData"] = canonical
    return out


def canonicalize(document):
    body = common.validate_header(document, (STRING_TABLE_CLASS,), PREFIX, BODY_KEYS)
    canonical_body(body)
    if document.get("Properties"):
        raise common.error("/Properties", "A StringTable has no Details-panel properties.", "StringTable.UnsupportedProperties")
    return common.snapshot(document)


def read_live(calls):
    first, entries = calls.query_all(common.STRING_TABLE_READ, "entries")
    return {"Namespace": first.get("namespace", ""),
            "Entries": {entry["key"]: entry["source"] for entry in entries},
            "MetaData": {entry["key"]: dict(entry["metadata"]) for entry in entries if entry.get("metadata")}}


def string_ops(body, live):
    """Edit ops (with document paths) for Entries and MetaData."""
    ops = []
    entries = body.get("Entries", live["Entries"])
    if "Entries" in body:
        ops += [({"op": "remove", "key": key}, "/Body/Entries") for key in sorted(live["Entries"]) if key not in entries]
        ops += [({"op": "set", "key": key, "source": source}, f"/Body/Entries/{common.escape(key)}")
                for key, source in sorted(entries.items()) if live["Entries"].get(key) != source]
    if "MetaData" in body:
        authored = body["MetaData"]
        for key in sorted(authored):
            if key not in entries:
                raise common.error(f"/Body/MetaData/{common.escape(key)}", f"MetaData names key '{key}', which is not "
                                   f"an entry.{common.describe_candidates(key, sorted(entries), 'Entries')}",
                                   "StringTable.KeyNotFound")
        for key in sorted(entries):
            # Removing an entry removes its metadata too.
            current = live["MetaData"].get(key, {}) if key in live["Entries"] else {}
            wanted = authored.get(key, {})
            path = f"/Body/MetaData/{common.escape(key)}"
            ops += [({"op": "meta_remove", "key": key, "id": name}, path) for name in sorted(current) if name not in wanted]
            ops += [({"op": "meta_set", "key": key, "id": name, "value": value}, f"{path}/{common.escape(name)}")
                    for name, value in sorted(wanted.items()) if current.get(name) != value]
    return ops


def apply(document, read, mutable, context):
    body = canonical_body(document.get("Body", {}))
    calls = common.DataCalls(context, BY_ID, read, mutable, PREFIX)
    live = read_live(calls)
    fixed = {"namespace": body["Namespace"]} if body.get("Namespace", live["Namespace"]) != live["Namespace"] else {}
    ops = string_ops(body, live)
    if not ops and not fixed:
        return ()
    if not ops:
        return tuple(_namespace_only(calls, fixed))
    return tuple(calls.edit(common.STRING_TABLE_EDIT, fixed, ops, {"namespace": "/Body/Namespace", "ops": "/Body/Entries"}))


def _namespace_only(calls, fixed):
    outcome = common.invoke(calls.context, BY_ID, common.STRING_TABLE_EDIT, {"schema_version": 1, **fixed, "ops": []},
                            (common.NamedOpaqueHandleArgument("asset", calls.write),), path="/Body/Namespace", prefix=PREFIX)
    return list(outcome.changed_set)


def extract(target, read, context):
    live = read_live(common.DataCalls(context, BY_ID, read, prefix=PREFIX))
    return common.document(target, STRING_TABLE_CLASS, {}, live)


def _map_diff(path, authored, live, changes, keys=None):
    """``Entries`` and ``MetaData`` are complete maps: a key only the asset has is removed (a Remove entry, so
    validate and diff count and name it), a key only the document has is added. ``keys`` limits the keys compared
    (MetaData of an entry the document removes goes with the entry)."""
    for key in sorted(live):
        if key not in authored and (keys is None or key in keys):
            changes.append(Difference(f"{path}/{common.escape(key)}", DiffKind.REMOVE, False, None, True, live[key]))
    for key, value in authored.items():
        where = f"{path}/{common.escape(key)}"
        if key not in live:
            changes.append(Difference(where, DiffKind.ADD, True, value, False, None))
        elif type(value) is dict and type(live[key]) is dict:
            _map_diff(where, value, live[key], changes)
        elif value != live[key]:
            changes.append(Difference(where, DiffKind.REPLACE, True, value, True, live[key]))


def diff(authored, extracted):
    changes = common.header_diff(authored, extracted)
    body = canonical_body(authored.get("Body", {}))
    live = extracted.get("Body", {})
    if "Namespace" in body and body["Namespace"] != live.get("Namespace", ""):
        changes.append(Difference("/Body/Namespace", DiffKind.REPLACE, True, body["Namespace"], "Namespace" in live,
                                  live.get("Namespace")))
    if "Entries" in body:
        _map_diff("/Body/Entries", body["Entries"], live.get("Entries", {}), changes)
    if "MetaData" in body:
        _map_diff("/Body/MetaData", body["MetaData"], live.get("MetaData", {}), changes,
                  set(body.get("Entries", live.get("Entries", {}))))
    return tuple(changes)


class StringTableRuntimeProfile:
    profile_id = PROFILE_ID
    exact_asset_class_path = STRING_TABLE_CLASS
    runtime_entry_module = "asset_document/bootstrap.py"
    preflight_scratch = False

    def required_primitives(self):
        return REQUIREMENTS

    def inspect_profile(self):
        return {"Class": STRING_TABLE_CLASS, "BodySections": list(BODY_KEYS), "ReadOnlySections": []}

    def create_template(self, target):
        return common.document(common.require_target(target, PREFIX), STRING_TABLE_CLASS, {},
                               {"Namespace": target.rsplit("/", 1)[-1], "Entries": {}, "MetaData": {}})

    def validate(self, document):
        canonicalize(document)

    def canonicalize(self, document):
        return canonicalize(document)

    def create_target_context(self, document, context):
        canonicalize(document)
        return ()

    def preflight(self, document, scratch_handle, context):
        canonicalize(document)
        return SNAPSHOT_COVERED

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        return apply(canonicalize(document), read_asset_handle, mutable_asset_handle, context)

    def repair(self, document, asset_handle, context):
        canonicalize(document)
        return ()

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        return extract(common.require_target(target, PREFIX), asset_handle, context)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != STRING_TABLE_CLASS or editor_asset_handle is None:
            raise common.error("/Inspect", "StringTable inspection reads a live string table.", "StringTable.InspectNotAdmitted")
        return canonicalize(self.extract(target=exact_asset_object_path.rsplit(".", 1)[0],
                                         asset_handle=editor_asset_handle, context=context))

    def diff(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted))

    def diff_exact(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted))


def create_string_table_runtime_handler():
    return StringTableRuntimeProfile()


STRING_TABLE_PROFILE_META = ProfileMeta(
    PROFILE_ID, common.ROUTE, STRING_TABLE_CLASS,
    tuple(RegionMeta(f"string_table.{name}.v1", f"/Body/{key}", RegionOwnership.MANAGED, identity,
                     OrderPolicy.CANONICAL, DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE, DeletePolicy.EXPLICIT_ONLY,
                     ComparisonPolicy.CANONICAL)
          for name, key, identity in (("namespace", "Namespace", IdentityPolicy.POSITIONAL),
                                      ("entries", "Entries", IdentityPolicy.STABLE_KEY),
                                      ("metadata", "MetaData", IdentityPolicy.STABLE_KEY))),
    REQUIREMENTS)
STRING_TABLE_RUNTIME_REGISTRATION = RuntimeProfileRegistration(
    STRING_TABLE_PROFILE_META, create_string_table_runtime_handler, RuntimeTargetPolicy.CREATE_OR_UPDATE, LIFECYCLE)

__all__ = ("STRING_TABLE_RUNTIME_REGISTRATION", "StringTableRuntimeProfile", "STRING_TABLE_CLASS", "string_ops")
