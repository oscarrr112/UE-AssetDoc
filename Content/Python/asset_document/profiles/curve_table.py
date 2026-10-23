# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Registered CurveTable Profiles: UCurveTable and UCompositeCurveTable.

Document (docs/architecture/asset-document-datatable-matrix.md)::

    "Body": {
      "CurveTableMode": "SimpleCurves",                 # or "RichCurves" (the New Curve Table dialog's
                                                        # Linear/Constant vs Cubic choice)
      "Rows": [
        {"Name": "Damage", "InterpMode": "Linear",      # SimpleCurves: one interp mode per curve
         "Keys": [{"Time": 1, "Value": 10}, {"Time": 10, "Value": 55.5}]},
        {"Name": "Speed", "RenamedFrom": "Velocity", "Keys": [...]}
      ]
    }

* SimpleCurves rows: ``InterpMode`` (Linear | Constant), ``Keys`` [{Time, Value}], optional
  ``PreInfinityExtrap`` / ``PostInfinityExtrap`` (Constant, Linear, Cycle, CycleWithOffset,
  Oscillate, None) and ``DefaultValue``.
* RichCurves rows: the CurveFloat curve form (per-key InterpMode, TangentMode, TangentWeightMode,
  tangents and weights; see profiles/curve_float.py).
* ``Rows`` is the full set of curves (omit = keep); row order is not authored (the Curve Table
  editor has no reorder).  ``RenamedFrom`` renames a curve (UCurveTable::RenameRow).  Changing
  ``CurveTableMode`` of a table that has curves rebuilds it from ``Rows`` (as a reimport does).
* CompositeCurveTable: ``ParentTables`` is authored; ``Rows`` are derived and read-only.
"""
from __future__ import annotations

from asset_document.diagnostics import AssetDocumentContractError, contract_error
from asset_document.diff.model import Difference, DiffKind
from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, IdentityPolicy, OmissionPolicy, OrderPolicy, ProfileMeta,
    RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document.profiles import curve_float
from asset_document.profiles import data_table_common as common
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements

CURVE_TABLE_CLASS = "/Script/Engine.CurveTable"
COMPOSITE_CURVE_TABLE_CLASS = "/Script/Engine.CompositeCurveTable"
MODES = ("SimpleCurves", "RichCurves")
SIMPLE_FIELDS = ("Keys", "InterpMode", "PreInfinityExtrap", "PostInfinityExtrap", "DefaultValue")
RICH_FIELDS = ("Keys", "PreInfinityExtrap", "PostInfinityExtrap", "DefaultValue")
SIMPLE_INTERP_MODES = {"Linear": "RCIM_Linear", "Constant": "RCIM_Constant"}
LIFECYCLE = generic_factory_lifecycle(common.ROUTE)
REQUIREMENTS = (common.requirements(common.CLASS_RESOLVE, common.RESOLVE, common.CURVE_TABLE_READ,
                                    common.CURVE_TABLE_EDIT)
                + lifecycle_requirements(LIFECYCLE))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}
PREFIX = "CurveTable"
_RICH = curve_float.RichCurveCodec()


def _composite(class_path):
    return class_path == COMPOSITE_CURVE_TABLE_CLASS


def _body_keys(class_path):
    return ("CurveTableMode", "ParentTables", "Rows") if _composite(class_path) else ("CurveTableMode", "Rows")


def _retag(failure):
    """Curve value errors of the shared CurveFloat codec, reported as CurveTable errors."""
    diagnostic = failure.diagnostic
    return contract_error(diagnostic.path, diagnostic.code.replace("CurveFloat.", "CurveTable."), diagnostic.message)


# ---------------------------------------------------------------------------
# Curves: document form <-> native FSimpleCurve / FRichCurve form
# ---------------------------------------------------------------------------
def simple_from_document(row, path):
    for key in row:
        if key not in SIMPLE_FIELDS:
            raise common.error(f"{path}/{common.escape(key)}", f"Unknown simple curve field '{key}'. Fields: Name, "
                               f"RenamedFrom, {', '.join(SIMPLE_FIELDS)}.", "CurveTable.UnknownField")
    mode = row.get("InterpMode", "Linear")
    if mode not in SIMPLE_INTERP_MODES:
        raise common.error(f"{path}/InterpMode", "A simple curve's InterpMode is Linear or Constant (cubic curves "
                           "need CurveTableMode RichCurves).", "CurveTable.InvalidInterpMode")
    keys = row.get("Keys", [])
    if type(keys) is not list:
        raise common.error(f"{path}/Keys", "Keys is a list of {\"Time\", \"Value\"}.", "CurveTable.InvalidKey")
    native_keys = []
    for index, key in enumerate(keys):
        key_path = f"{path}/Keys/{index}"
        if type(key) is not dict or set(key) != {"Time", "Value"}:
            raise common.error(key_path, "A simple curve key is exactly {\"Time\", \"Value\"}.", "CurveTable.InvalidKey")
        try:
            native_keys.append({"Time": curve_float._number(key["Time"], f"{key_path}/Time"),
                                "Value": curve_float._number(key["Value"], f"{key_path}/Value")})
        except AssetDocumentContractError as failure:
            raise _retag(failure) from None
        if index and native_keys[-1]["Time"] <= native_keys[-2]["Time"]:
            raise common.error(f"{key_path}/Time", "Key times must be strictly ascending (after float32 rounding).",
                               "CurveTable.KeysNotAscending")
    try:
        default = row.get("DefaultValue")
        return {
            "InterpMode": SIMPLE_INTERP_MODES[mode],
            "Keys": native_keys,
            "DefaultValue": curve_float._FLOAT32_MAX if default is None else curve_float._number(default, f"{path}/DefaultValue"),
            "PreInfinityExtrap": curve_float._enum_from_document(row.get("PreInfinityExtrap", "Constant"),
                                                                 curve_float._EXTRAPOLATIONS, f"{path}/PreInfinityExtrap"),
            "PostInfinityExtrap": curve_float._enum_from_document(row.get("PostInfinityExtrap", "Constant"),
                                                                  curve_float._EXTRAPOLATIONS, f"{path}/PostInfinityExtrap"),
        }
    except AssetDocumentContractError as failure:
        raise _retag(failure) from None


def simple_to_document(native, path):
    if type(native) is not dict or set(native) != set(SIMPLE_FIELDS):
        raise common.error(path, "Native FSimpleCurve has an unexpected shape.", "CurveTable.InvalidNativeValue")
    modes = {value: key for key, value in curve_float._INTERP_MODES.items()}
    out = {"InterpMode": modes.get(native["InterpMode"], native["InterpMode"]),
           "Keys": [{"Time": curve_float._shortest(key["Time"], f"{path}/Keys/{index}/Time"),
                     "Value": curve_float._shortest(key["Value"], f"{path}/Keys/{index}/Value")}
                    for index, key in enumerate(native["Keys"])],
           "PreInfinityExtrap": curve_float._enum_to_document(native["PreInfinityExtrap"], curve_float._EXTRAPOLATIONS,
                                                              f"{path}/PreInfinityExtrap"),
           "PostInfinityExtrap": curve_float._enum_to_document(native["PostInfinityExtrap"], curve_float._EXTRAPOLATIONS,
                                                               f"{path}/PostInfinityExtrap")}
    default = native["DefaultValue"]
    if type(default) is not float or curve_float._float32(default) != curve_float._FLOAT32_MAX:
        out["DefaultValue"] = curve_float._shortest(float(default), f"{path}/DefaultValue")
    return out


def rich_from_document(row, path):
    try:
        return _RICH.from_document({key: value for key, value in row.items() if key not in ("Name", "RenamedFrom")}, path)
    except AssetDocumentContractError as failure:
        raise _retag(failure) from None


def curve_from_document(row, mode, path):
    curve = {key: value for key, value in row.items() if key not in ("Name", "RenamedFrom")}
    return simple_from_document(curve, path) if mode == "SimpleCurves" else rich_from_document(curve, path)


def curve_to_document(native, mode, path):
    return simple_to_document(native, path) if mode == "SimpleCurves" else _RICH.to_document(native, path)


def canonical_curve(row, mode, path):
    """Document form with every default spelled out (for comparison)."""
    return curve_to_document(curve_from_document(row, mode, path), mode, path)


# ---------------------------------------------------------------------------
# Document shape
# ---------------------------------------------------------------------------
def canonical_body(body, class_path):
    out = {}
    if "CurveTableMode" in body:
        if body["CurveTableMode"] not in MODES + ("Empty",):
            raise common.error("/Body/CurveTableMode", "CurveTableMode is SimpleCurves or RichCurves.",
                               "CurveTable.InvalidMode")
        out["CurveTableMode"] = body["CurveTableMode"]
    if "ParentTables" in body:
        tables = body["ParentTables"]
        if type(tables) is not list or any(type(item) is not str or not item for item in tables):
            raise common.error("/Body/ParentTables", "ParentTables is an ordered list of CurveTable object paths.",
                               "CurveTable.InvalidParentTables")
        out["ParentTables"] = list(tables)
    if "Rows" in body:
        rows = body["Rows"]
        if type(rows) is not list:
            raise common.error("/Body/Rows", "Rows is a list of curves {\"Name\", \"Keys\", ...}.", "CurveTable.InvalidRows")
        names, sources = set(), set()
        for index, row in enumerate(rows):
            path = f"/Body/Rows/{index}"
            if type(row) is not dict:
                raise common.error(path, "A curve row is an object with Name and its curve fields.", "CurveTable.InvalidRow")
            name = common.validate_name(row.get("Name"), f"{path}/Name", PREFIX, "curve")
            if name.lower() in names:
                raise common.error(f"{path}/Name", f"Curve '{name}' appears more than once.", "CurveTable.DuplicateRow")
            names.add(name.lower())
            if "RenamedFrom" in row:
                source = common.validate_name(row["RenamedFrom"], f"{path}/RenamedFrom", PREFIX, "curve")
                if source.lower() == name.lower() or source.lower() in sources:
                    raise common.error(f"{path}/RenamedFrom", "RenamedFrom names another curve, once.",
                                       "CurveTable.InvalidRename")
                sources.add(source.lower())
        mode = out.get("CurveTableMode")
        if rows and mode in (None, "Empty"):
            raise common.error("/Body/CurveTableMode", "Rows need CurveTableMode (SimpleCurves or RichCurves) to "
                               "read their curves.", "CurveTable.ModeRequired")
        for index, row in enumerate(rows):
            curve_from_document(row, mode, f"/Body/Rows/{index}")
        out["Rows"] = [dict(row) for row in rows]
    return out


def canonicalize(document):
    body = common.validate_header(document, (CURVE_TABLE_CLASS, COMPOSITE_CURVE_TABLE_CLASS), PREFIX,
                                  ("CurveTableMode", "ParentTables", "Rows"))
    for key in body:
        if key not in _body_keys(document["Class"]):
            raise common.error(f"/Body/{key}", f"{key} is only authored on a CompositeCurveTable.", "CurveTable.UnknownBodyKey")
    canonical_body(body, document["Class"])
    return common.snapshot(document)


# ---------------------------------------------------------------------------
# Live state
# ---------------------------------------------------------------------------
def read_live(calls):
    first, rows = calls.query_all(common.CURVE_TABLE_READ, "rows")
    mode = first.get("mode", "Empty")
    live = [{"Name": row["name"], **curve_to_document(row["curve"], mode, f"/Body/Rows/{index}")}
            for index, row in enumerate(rows)]
    return first, mode, live


def _body(first, mode, rows):
    body = {"CurveTableMode": mode}
    if first.get("composite"):
        body["ParentTables"] = list(first.get("parent_tables", []))
    body["Rows"] = rows
    return body


def _curve(row):
    return {key: value for key, value in row.items() if key not in ("Name", "RenamedFrom")}


def _rows_mismatch(authored_rows, mode, live_rows, live_mode, exact):
    live = {row["Name"]: _curve(row) for row in live_rows}
    if {row["Name"] for row in authored_rows} != set(live):
        return "/Body/Rows"
    if authored_rows and mode != live_mode:
        return "/Body/CurveTableMode"
    for index, row in enumerate(authored_rows):
        if not common.same(_curve(canonical_curve(row, mode, f"/Body/Rows/{index}")), live[row["Name"]], exact):
            return f"/Body/Rows/{index}"
    return None


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------
def curve_ops(authored, mode, live, live_mode):
    """Edit ops (with document paths) that turn the live curves into the authored ones."""
    def set_op(index, row):
        return ({"op": "set", "row": row["Name"], "mode": mode,
                 "curve": curve_from_document(row, mode, f"/Body/Rows/{index}")}, f"/Body/Rows/{index}")
    if live and live_mode != mode:
        # A different curve type: rebuild from Rows (as a reimport with the other type does).
        return [({"op": "reset", "mode": mode}, "/Body/CurveTableMode")] + [set_op(index, row) for index, row in enumerate(authored)]
    live_curves = {row["Name"]: _curve(row) for row in live}
    names = {row["Name"] for row in authored}
    renames = {row["Name"]: row["RenamedFrom"] for row in authored
               if row.get("RenamedFrom") in live_curves and row["Name"] not in live_curves and row["RenamedFrom"] not in names}
    sources = set(renames.values())
    ops = [({"op": "remove", "row": name}, "/Body/Rows") for name in live_curves
           if name not in names and name not in sources]
    for index, row in enumerate(authored):
        if row["Name"] in renames:
            ops.append(({"op": "rename", "row": renames[row["Name"]], "to": row["Name"]}, f"/Body/Rows/{index}/RenamedFrom"))
    for index, row in enumerate(authored):
        current = live_curves.get(renames.get(row["Name"], row["Name"]))
        if current is None or not common.same(_curve(canonical_curve(row, mode, f"/Body/Rows/{index}")), current):
            ops.append(set_op(index, row))
    return ops


_BASE_PATHS = {"ops": "/Body/Rows"}


def apply(document, read, mutable, context):
    class_path = document["Class"]
    body = canonical_body(document.get("Body", {}), class_path)
    properties = document.get("Properties", {})
    region = _properties(class_path)
    region.check_owned(properties)
    calls = common.DataCalls(context, BY_ID, read, mutable, PREFIX)
    first, live_mode, live = read_live(calls)
    ops = []
    if _composite(class_path):
        if "ParentTables" in body and body["ParentTables"] != first.get("parent_tables", []):
            ops.append(({"op": "parent_tables", "tables": body["ParentTables"]}, "/Body/ParentTables"))
    elif "Rows" in body:
        mode = body.get("CurveTableMode", live_mode)
        if body["Rows"]:
            ops.extend(curve_ops(body["Rows"], mode, live, live_mode))
        else:
            ops.extend(({"op": "remove", "row": row["Name"]}, "/Body/Rows") for row in live)
    elif body.get("CurveTableMode", live_mode) not in (live_mode, "Empty") and live:
        raise common.error("/Body/CurveTableMode", "Changing the curve type of a table with curves rebuilds it: give "
                           "Rows in the new type as well.", "CurveTable.ModeChangeNeedsRows")
    changes = calls.edit(common.CURVE_TABLE_EDIT, {}, ops, _BASE_PATHS) if ops else []
    if _composite(class_path) and "Rows" in body:
        _, derived_mode, derived = read_live(calls)
        where = _rows_mismatch(body["Rows"], body.get("CurveTableMode", derived_mode), derived, derived_mode, exact=False)
        if where is not None:
            raise common.error(where, "A CompositeCurveTable's curves come from its ParentTables and are read-only; "
                               "omit Rows or give the derived curves.", "CompositeCurveTable.RowsReadOnly")
    changes.extend(region.apply(properties, read, mutable, context))
    return tuple(changes)


def _properties(class_path):
    return common.PropertiesRegion(class_path, {"ParentTables": "Body.ParentTables"}, PREFIX)


# ---------------------------------------------------------------------------
# Extract / Diff
# ---------------------------------------------------------------------------
def extract(target, class_path, read, context, include_defaults=True):
    calls = common.DataCalls(context, BY_ID, read, prefix=PREFIX)
    first, mode, rows = read_live(calls)
    return common.document(target, class_path, _properties(class_path).extract(read, context, include_defaults), _body(first, mode, rows))


def diff(authored, extracted, exact):
    changes = common.header_diff(authored, extracted)
    body = canonical_body(authored.get("Body", {}), authored["Class"])
    live = extracted.get("Body", {})
    live_mode = live.get("CurveTableMode", "Empty")
    if "ParentTables" in body and body["ParentTables"] != live.get("ParentTables"):
        changes.append(Difference("/Body/ParentTables", DiffKind.REPLACE, True, body["ParentTables"],
                                  "ParentTables" in live, live.get("ParentTables")))
    if "Rows" in body:
        mode = body.get("CurveTableMode", live_mode)
        live_rows = live.get("Rows", [])
        # A different curve type rebuilds every curve: the mode is the difference, not each curve.
        retyped = bool(body["Rows"]) and mode != live_mode

        def curve_difference(index, row, current):
            if retyped or common.same(_curve(canonical_curve(row, mode, f"/Body/Rows/{index}")), _curve(current), exact):
                return []
            return [Difference(f"/Body/Rows/{index}", DiffKind.REPLACE, True, row, True, current, row=row["Name"])]
        changes.extend(common.row_differences(body["Rows"], live_rows, curve_difference, ordered=False))
        if retyped:
            changes.append(Difference("/Body/CurveTableMode", DiffKind.REPLACE, True, mode, True, live_mode))
    elif "CurveTableMode" in body and live.get("Rows") and body["CurveTableMode"] != live_mode:
        changes.append(Difference("/Body/CurveTableMode", DiffKind.REPLACE, True, body["CurveTableMode"], True, live_mode))
    changes.extend(common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact))
    return tuple(changes)


# ---------------------------------------------------------------------------
# Handler
# ---------------------------------------------------------------------------
class CurveTableRuntimeProfile:
    runtime_entry_module = "asset_document/bootstrap.py"
    preflight_scratch = False

    def __init__(self, class_path, profile_id):
        self.exact_asset_class_path = class_path
        self.profile_id = profile_id

    def required_primitives(self):
        return REQUIREMENTS

    def inspect_profile(self):
        return {"Class": self.exact_asset_class_path, "BodySections": list(_body_keys(self.exact_asset_class_path)),
                "ReadOnlySections": ["Rows"] if _composite(self.exact_asset_class_path) else [],
                "CurveTableModes": list(MODES)}

    def create_template(self, target):
        body = ({"ParentTables": []} if _composite(self.exact_asset_class_path) else
                {"CurveTableMode": "SimpleCurves",
                 "Rows": [{"Name": "Curve", "InterpMode": "Linear", "Keys": [{"Time": 0, "Value": 0}]}]})
        return common.document(common.require_target(target, PREFIX), self.exact_asset_class_path, {}, body)

    def _canonical(self, document):
        canonical = canonicalize(document)
        if canonical["Class"] != self.exact_asset_class_path:
            raise common.error("/Class", f"Class must be {self.exact_asset_class_path}.", "CurveTable.ClassMismatch")
        return canonical

    def validate(self, document):
        self._canonical(document)

    def canonicalize(self, document):
        return self._canonical(document)

    def create_target_context(self, document, context):
        self._canonical(document)
        return ()

    def preflight(self, document, scratch_handle, context):
        canonical = self._canonical(document)
        _properties(canonical["Class"]).validate(canonical.get("Properties", {}), context)
        return SNAPSHOT_COVERED

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        return apply(self._canonical(document), read_asset_handle, mutable_asset_handle, context)

    def repair(self, document, asset_handle, context):
        self._canonical(document)
        return ()

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        return extract(common.require_target(target, PREFIX), self.exact_asset_class_path, asset_handle, context,
                       include_defaults)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != self.exact_asset_class_path or editor_asset_handle is None:
            raise common.error("/Inspect", "CurveTable inspection reads a live curve table.", "CurveTable.InspectNotAdmitted")
        return self.canonicalize(self.extract(target=exact_asset_object_path.rsplit(".", 1)[0],
                                              asset_handle=editor_asset_handle, context=context, include_defaults=True))

    def diff(self, authored, extracted):
        return diff(self._canonical(authored), self._canonical(extracted), exact=False)

    def diff_exact(self, authored, extracted):
        return diff(self._canonical(authored), self._canonical(extracted), exact=True)


def _meta(class_path, profile_id):
    rows = [("mode", "/Body/CurveTableMode", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL)]
    if _composite(class_path):
        rows.append(("parent_tables", "/Body/ParentTables", IdentityPolicy.POSITIONAL, OrderPolicy.PRESERVE,
                     ComparisonPolicy.ORDERED))
    rows.append(("rows", "/Body/Rows", IdentityPolicy.STABLE_KEY, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL))
    regions = tuple(RegionMeta(f"curve_table.{name}.v1", path, RegionOwnership.MANAGED, identity, order,
                               DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE, DeletePolicy.EXPLICIT_ONLY, comparison)
                    for name, path, identity, order, comparison in rows)
    return ProfileMeta(profile_id, common.ROUTE, class_path, regions, REQUIREMENTS)


def _registration(class_path, profile_id):
    return RuntimeProfileRegistration(_meta(class_path, profile_id), lambda: CurveTableRuntimeProfile(class_path, profile_id),
                                      RuntimeTargetPolicy.CREATE_OR_UPDATE, LIFECYCLE)


CURVE_TABLE_RUNTIME_REGISTRATION = _registration(CURVE_TABLE_CLASS, "profile.curve_table.v1")
COMPOSITE_CURVE_TABLE_RUNTIME_REGISTRATION = _registration(COMPOSITE_CURVE_TABLE_CLASS, "profile.composite_curve_table.v1")

__all__ = ("CURVE_TABLE_RUNTIME_REGISTRATION", "COMPOSITE_CURVE_TABLE_RUNTIME_REGISTRATION", "CurveTableRuntimeProfile",
           "curve_ops")
