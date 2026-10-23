# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Apply planner of profile.niagara_parameter_collection.v1: the collection editor's parameter list.

Body ``Parameters`` is complete when present: ``[{Name, Type, Value?, RenamedFrom?}]`` with the names the collection
editor shows (without the ``NPC.<Namespace>.`` prefix; systems link them as ``NPC.<Namespace>.<Name>``).  A parameter
that is not listed is deleted, a new name adds one (its value: the type's default unless ``Value`` is given),
``RenamedFrom`` renames one (applies while the collection does not have the new name yet), a different ``Type``
changes the type as the type menu does (the value goes back to the new type's default unless ``Value`` is given).
``Value`` omitted keeps the value.  Values are written with the Niagara value codec (as user parameters): plain
values, ``{"DataInterface": {"Class"?, "Properties"}}`` (Properties complete: omitted ones go back to the class
defaults) and ``{"Object": path}``.  Parameters the source material parameter collection drives
(``MaterialCollection: true`` in the extract) belong to it: they are never deleted, and changing them is refused
(``Niagara.CollectionParameterOwned``).  Order is not authored (the extract lists parameters by name).
"""
from __future__ import annotations

from asset_document.diff.model import Difference, DiffKind
from asset_document.profiles import data_table_common as common

PREFIX = "Niagara"
PARAMETER_KEYS = ("Name", "RenamedFrom", "Type", "Value", "MaterialCollection")
_MISSING = object()
_ORDER = {"delete_parameter": 0, "rename_parameter": 1, "add_parameter": 2, "retype_parameter": 3, "parameter": 4}


def _fail(path, message, code):
    raise common.error(path, message, f"{PREFIX}.{code}")


def check_body(body):
    if type(body) is not dict:
        _fail("/Body", "The Body is an object.", "InvalidBody")
    if "Parameters" not in body:
        return body
    if type(body["Parameters"]) is not list:
        _fail("/Body/Parameters", "Parameters is a list.", "InvalidBody")
    seen = set()
    for index, entry in enumerate(body["Parameters"]):
        path = f"/Body/Parameters/{index}"
        if type(entry) is not dict:
            _fail(path, "A parameter is an object {Name, Type, Value?, RenamedFrom?}.", "InvalidBody")
        for key in entry:
            if key not in PARAMETER_KEYS:
                _fail(f"{path}/{common.escape(key)}", f"Unknown parameter key '{key}'. Keys: {', '.join(PARAMETER_KEYS)}.",
                      "UnknownKey")
        for key in ("Name", "Type"):
            if type(entry.get(key)) is not str or not entry[key]:
                _fail(f"{path}/{key}", f"A parameter has a non-empty {key}.", "InvalidBody")
        if "RenamedFrom" in entry and (type(entry["RenamedFrom"]) is not str or not entry["RenamedFrom"]):
            _fail(f"{path}/RenamedFrom", "RenamedFrom is the parameter's current name.", "InvalidBody")
        if entry["Name"].startswith("NPC."):
            _fail(f"{path}/Name", "Collection parameters are named without the \"NPC.<Namespace>.\" prefix (the name the "
                  "collection editor shows).", "InvalidParameterName")
        if entry["Name"] in seen:
            _fail(f"{path}/Name", f"Two parameters are named '{entry['Name']}'.", "DuplicateName")
        seen.add(entry["Name"])
    return body


def _same(left, right):
    return common.same(left, right, exact=False)


def _match(authored, live_names):
    sources = [None] * len(authored)
    taken = set()
    for index, entry in enumerate(authored):
        source = entry.get("RenamedFrom")
        if source is not None and entry["Name"] not in live_names and source in live_names and source not in taken:
            sources[index] = source
            taken.add(source)
    for index, entry in enumerate(authored):
        if sources[index] is None and entry["Name"] in live_names and entry["Name"] not in taken:
            sources[index] = entry["Name"]
            taken.add(entry["Name"])
    return list(zip(authored, sources))


def _sparse_changes(authored, live, defaults):
    write = {}
    for name, value in authored.items():
        current = live[name] if name in live else defaults.get(name, _MISSING)
        if current is _MISSING or not _same(value, current):
            write[name] = value
    return write, sorted(name for name in live if name not in authored)


class Plan:
    def __init__(self):
        self.ops = []
        self.refusals = []
        self.differences = []

    def change(self, path, authored, live, op=None, kind=DiffKind.REPLACE, authored_present=True, live_present=True):
        self.differences.append(Difference(path, kind, authored_present, authored, live_present, live))
        if op is not None:
            self.ops.append((op, path))

    def refuse(self, path, code, message, authored=None, live=None):
        self.differences.append(Difference(path, DiffKind.REPLACE, True, authored, True, live))
        self.refusals.append(common.error(path, message, f"{PREFIX}.{code}"))


def _value(plan, path, name, want, have, interface_defaults):
    if type(want) is dict and "DataInterface" in want and type(have) is dict and "DataInterface" in have:
        spec = want["DataInterface"]
        if type(spec) is not dict:
            _fail(path, "A data interface value is {\"DataInterface\": {\"Class\": ..., \"Properties\": {...}}}.",
                  "InvalidCollectionValue")
        current = have["DataInterface"]
        write, reset = _sparse_changes(spec.get("Properties", {}), current.get("Properties", {}),
                                       interface_defaults.get(current.get("Class", ""), {}))
        if write or reset:
            value = {"DataInterface": {"Properties": write}}
            if "Class" in spec:
                value["DataInterface"]["Class"] = spec["Class"]
            op = {"op": "parameter", "name": name, "value": value}
            if reset:
                op["reset"] = reset
            plan.change(path, want, have, op)
        return
    if have is _MISSING or not _same(want, have):
        plan.change(path, want, None if have is _MISSING else have, {"op": "parameter", "name": name, "value": want},
                    live_present=have is not _MISSING)


def plan_collection(body, live_body, interface_defaults=None):
    """The edits that move the live collection (its extracted Body) towards the document's Body."""
    plan = Plan()
    if "Parameters" not in body:
        return plan
    interface_defaults = interface_defaults or {}
    authored = body["Parameters"]
    live = {entry["Name"]: entry for entry in live_body.get("Parameters", [])}
    owned = {name for name, entry in live.items() if entry.get("MaterialCollection") is True}
    matches = _match(authored, list(live))
    base = "/Body/Parameters"
    kept = {source for _, source in matches if source is not None}
    for name in live:
        if name not in kept and name not in owned:
            plan.change(base, None, live[name], {"op": "delete_parameter", "name": name}, DiffKind.REMOVE, False, True)
    for index, (entry, source) in enumerate(matches):
        path = f"{base}/{index}"
        name = entry["Name"]
        if source is None:
            plan.change(path, entry, None, {"op": "add_parameter", "name": name, "type": entry["Type"]}, DiffKind.ADD, True,
                        False)
            continue
        current = live[source]
        if source in owned:
            if source != name or entry["Type"] != current["Type"] or ("Value" in entry and not _same(entry["Value"],
                                                                                                    current.get("Value"))):
                plan.refuse(path, "CollectionParameterOwned", f"'{source}' comes from the source material parameter "
                            "collection, which owns its name, type and value.", entry, current)
            continue
        if source != name:
            plan.change(f"{path}/Name", name, source, {"op": "rename_parameter", "name": source, "to": name})
        if entry["Type"] != current["Type"]:
            plan.change(f"{path}/Type", entry["Type"], current["Type"],
                        {"op": "retype_parameter", "name": name, "type": entry["Type"]})
            # The value of the new type is planned once the type has changed (a later pass).
            continue
        if "Value" in entry:
            _value(plan, f"{path}/Value", name, entry["Value"], current.get("Value", _MISSING), interface_defaults)
    # Deletes and renames free names before adds take them; retypes and values use the new names.
    plan.ops.sort(key=lambda item: _ORDER[item[0]["op"]])
    return plan


__all__ = ("check_body", "plan_collection", "Plan")
