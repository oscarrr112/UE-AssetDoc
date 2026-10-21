"""Registered UserDefinedEnum Profile (profile.user_defined_enum.v1): create, read and edit the
enumerators of a User Defined Enum as the Enum editor does (FEnumEditorUtils).

Document (docs/architecture/asset-document-user-defined-types-matrix.md)::

    "Body": {
      "Tooltip": "How an item is used.",                # the enum's Description
      "Bitflags": false,                                # the editor's Bitmask Flags option
      "Enumerators": [                                  # the full ordered list (omit = keep)
        {"Name": "Melee", "Tooltip": "Close range"},
        {"Name": "Ranged", "RenamedFrom": "Bow"},
        {"Name": "Magic"}
      ]
    }

* ``Name`` is the enumerator's display name: the name the Enum editor, Blueprints and data table
  values use.  The internal names (``NewEnumerator0`` ...) never change, so ``RenamedFrom`` (or a
  case change) keeps every stored value.  Display names are unique ignoring case.
* ``Tooltip`` on an enumerator is its Description (omitted = none).
* Removing an enumerator invalidates values that used it in the assets that use the enum (as in
  the editor; the apply warns).  Reordering keeps stored values: the editor re-resolves them by
  enumerator.

Editing an enum makes the editor update the loaded assets that use it; the dependents rule of the
UserDefinedStruct Profile applies (``UserDefinedEnum.DependentHasUnsavedChanges``, warnings, restore
on failure).  Writes are covered by Core's package snapshot; preflight returns ``SNAPSHOT_COVERED``.
"""
from __future__ import annotations

from asset_document.diagnostics import with_diagnostics
from asset_document.diff.model import Difference, DiffKind
from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, IdentityPolicy, OmissionPolicy, OrderPolicy, ProfileMeta,
    RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document.profiles import data_table_common as common
from asset_document.profiles import user_defined_types_common as types
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements

USER_ENUM_CLASS = "/Script/Engine.UserDefinedEnum"
PROFILE_ID = "profile.user_defined_enum.v1"
PREFIX = "UserDefinedEnum"
BODY_KEYS = ("Tooltip", "Bitflags", "Enumerators")
ENUMERATOR_KEYS = ("Name", "RenamedFrom", "Tooltip")
LIFECYCLE = generic_factory_lifecycle(common.ROUTE)
REQUIREMENTS = (common.requirements(common.CLASS_RESOLVE, common.RESOLVE, types.USER_ENUM_READ, types.USER_ENUM_EDIT)
                + lifecycle_requirements(LIFECYCLE))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}
OP_FIELDS = {"name": "/Name", "to": "/Name", "tooltip": "/Tooltip", "names": "", "value": ""}


def _error(path, message, code="UserDefinedEnum.InvalidEnumerator"):
    return common.error(path, message, code)


# ---------------------------------------------------------------------------
# Document shape
# ---------------------------------------------------------------------------
def canonical_body(body):
    out = {}
    if "Tooltip" in body:
        if type(body["Tooltip"]) is not str:
            raise _error("/Body/Tooltip", "Tooltip (the enum's Description) is a string.", "UserDefinedEnum.InvalidTooltip")
        out["Tooltip"] = body["Tooltip"]
    if "Bitflags" in body:
        if type(body["Bitflags"]) is not bool:
            raise _error("/Body/Bitflags", "Bitflags is true or false.", "UserDefinedEnum.InvalidBitflags")
        out["Bitflags"] = body["Bitflags"]
    if "Enumerators" in body:
        rows = body["Enumerators"]
        if type(rows) is not list:
            raise _error("/Body/Enumerators", "Enumerators is an ordered list of {\"Name\"} objects.",
                         "UserDefinedEnum.InvalidEnumerators")
        names, sources, canonical = set(), set(), []
        for index, row in enumerate(rows):
            path = f"/Body/Enumerators/{index}"
            if type(row) is not dict:
                raise _error(path, "An enumerator is an object with Name and optional Tooltip / RenamedFrom.")
            for key in row:
                if key not in ENUMERATOR_KEYS:
                    raise _error(f"{path}/{common.escape(key)}", f"Unknown enumerator key '{key}'. Keys: "
                                 f"{', '.join(ENUMERATOR_KEYS)}.", "UserDefinedEnum.UnknownEnumeratorKey")
            item = {"Name": types.validate_label(row.get("Name"), f"{path}/Name", "UserDefinedEnum.InvalidEnumeratorName",
                                                 "enumerator")}
            if item["Name"].lower() in names:
                raise _error(f"{path}/Name", f"Enumerator '{item['Name']}' appears more than once (display names are "
                             "unique ignoring case).", "UserDefinedEnum.DuplicateEnumerator")
            names.add(item["Name"].lower())
            if "RenamedFrom" in row:
                source = types.validate_label(row["RenamedFrom"], f"{path}/RenamedFrom",
                                              "UserDefinedEnum.InvalidEnumeratorName", "enumerator")
                if source.lower() == item["Name"].lower() or source.lower() in sources:
                    raise _error(f"{path}/RenamedFrom", "RenamedFrom names another enumerator, once.",
                                 "UserDefinedEnum.InvalidRename")
                sources.add(source.lower())
                item["RenamedFrom"] = source
            if "Tooltip" in row:
                if type(row["Tooltip"]) is not str:
                    raise _error(f"{path}/Tooltip", "Tooltip (the enumerator's Description) is a string.")
                item["Tooltip"] = row["Tooltip"]
            canonical.append(item)
        out["Enumerators"] = canonical
    return out


def canonicalize(document):
    body = common.validate_header(document, (USER_ENUM_CLASS,), PREFIX, BODY_KEYS)
    canonical_body(body)
    if document.get("Properties"):
        raise _error("/Properties", "Author the enum's Description as Body.Tooltip; a User Defined Enum has no other "
                     "Details-panel properties.", "UserDefinedEnum.UnsupportedProperties")
    return common.snapshot(document)


# ---------------------------------------------------------------------------
# Live state
# ---------------------------------------------------------------------------
def read_live(calls, dependents=False):
    return calls.query(types.USER_ENUM_READ, {"dependents": True} if dependents else {})


def body_from_live(live):
    enumerators = []
    for row in live.get("enumerators", []):
        item = {"Name": row["display_name"]}
        if row.get("tooltip"):
            item["Tooltip"] = row["tooltip"]
        enumerators.append(item)
    return {"Tooltip": live.get("tooltip", ""), "Bitflags": bool(live.get("bitflags")), "Enumerators": enumerators}


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------
def enumerator_ops(body, live):
    """Edit ops (with document paths) that turn the live enum into the authored body, and the plan
    facts the warnings need ({"removed": [...]})."""
    ops, facts = [], {"removed": []}
    live_rows = list(live.get("enumerators", []))
    if "Enumerators" in body:
        rows = body["Enumerators"]
        by_lower = {row["display_name"].lower(): row for row in live_rows}
        claimed, used = {}, set()
        for index, row in enumerate(rows):
            match = by_lower.get(row["Name"].lower())
            if match is not None:
                claimed[index] = match
                used.add(match["name"])
        for index, row in enumerate(rows):
            source = row.get("RenamedFrom")
            match = by_lower.get(source.lower()) if source is not None and index not in claimed else None
            if match is not None and match["name"] not in used:
                claimed[index] = match
                used.add(match["name"])
        # Names match first (ignoring case), so a rename target is free once the removes ran.
        renames = [index for index, match in claimed.items() if match["display_name"] != rows[index]["Name"]]
        removed = [row["display_name"] for row in live_rows if row["name"] not in used]
        facts["removed"] = removed
        ops += [({"op": "remove", "name": name}, "/Body/Enumerators") for name in removed]
        for index in renames:
            ops.append(({"op": "rename", "name": claimed[index]["display_name"], "to": rows[index]["Name"]}, f"/Body/Enumerators/{index}"))
        for index, row in enumerate(rows):
            if index not in claimed:
                ops.append(({"op": "add", "name": row["Name"]}, f"/Body/Enumerators/{index}"))
        order_now = [rows[index]["Name"] for index in sorted(claimed, key=lambda item: live_rows.index(claimed[item]))]
        order_now += [row["Name"] for index, row in enumerate(rows) if index not in claimed]
        if order_now != [row["Name"] for row in rows]:
            ops.append(({"op": "order", "names": [row["Name"] for row in rows]}, "/Body/Enumerators"))
        for index, row in enumerate(rows):
            have = claimed[index].get("tooltip", "") if index in claimed else ""
            if row.get("Tooltip", "") != have:
                ops.append(({"op": "tooltip", "name": row["Name"], "tooltip": row.get("Tooltip", "")},
                            f"/Body/Enumerators/{index}"))
    if "Tooltip" in body and body["Tooltip"] != live.get("tooltip", ""):
        ops.append(({"op": "description", "tooltip": body["Tooltip"]}, "/Body/Tooltip"))
    if "Bitflags" in body and body["Bitflags"] != bool(live.get("bitflags")):
        ops.append(({"op": "bitflags", "value": body["Bitflags"]}, "/Body/Bitflags"))
    return ops, facts


def apply_warnings(facts, dependents):
    diagnostics = types.dependents_warnings(dependents, "/Body/Enumerators", PREFIX)
    if dependents and facts["removed"]:
        diagnostics.append(types.warning("/Body/Enumerators", "UserDefinedEnum.EnumeratorValuesInvalidated",
                                         "Values that used the removed enumerator(s) " + ", ".join(facts["removed"])
                                         + " in the assets above no longer name an enumerator (as in the editor: "
                                         "Blueprint pins show (INVALID), stored values fall back)."))
    return diagnostics


def apply(document, read, mutable, context):
    body = canonical_body(document.get("Body", {}))
    calls = types.TypeCalls(context, BY_ID, read, mutable, PREFIX)
    live = read_live(calls)
    ops, facts = enumerator_ops(body, live)
    if not ops:
        return ()
    changed, dependents = calls.edit_all(types.USER_ENUM_EDIT, ops, OP_FIELDS, "/Body/Enumerators")
    return with_diagnostics(changed, apply_warnings(facts, dependents))


# ---------------------------------------------------------------------------
# Extract / Diff / Inspect
# ---------------------------------------------------------------------------
def extract(target, read, context):
    live = read_live(types.TypeCalls(context, BY_ID, read, prefix=PREFIX))
    return common.document(target, USER_ENUM_CLASS, {}, body_from_live(live))


def diff(authored, extracted):
    changes = common.header_diff(authored, extracted)
    body = canonical_body(authored.get("Body", {}))
    live = canonical_body(extracted.get("Body", {}))
    for key, default in (("Tooltip", ""), ("Bitflags", False)):
        if key in body and body[key] != live.get(key, default):
            changes.append(Difference(f"/Body/{key}", DiffKind.REPLACE, True, body[key], key in live, live.get(key)))
    if "Enumerators" in body:
        observed = live.get("Enumerators", [])
        if [row["Name"] for row in body["Enumerators"]] != [row["Name"] for row in observed]:
            changes.append(Difference("/Body/Enumerators", DiffKind.REPLACE, True, body["Enumerators"], True, observed))
        else:
            for index, (row, have) in enumerate(zip(body["Enumerators"], observed)):
                if row.get("Tooltip", "") != have.get("Tooltip", ""):
                    changes.append(Difference(f"/Body/Enumerators/{index}/Tooltip", DiffKind.REPLACE, True,
                                              row.get("Tooltip", ""), "Tooltip" in have, have.get("Tooltip")))
    return tuple(changes)


def inspect_details(live, object_path=""):
    details = {"Enumerators": [{"Name": row["display_name"], "InternalName": row["name"], "Value": row["value"]}
                               for row in live.get("enumerators", [])],
               "ValueForm": "Data table rows, struct defaults and Blueprint defaults name an enumerator by its display "
                            "Name; InternalName and Value are the editor's storage (Value is the index, reassigned on "
                            "reorder and remove)."}
    if object_path:
        details["UseAs"] = {"Type": f"byte:{object_path}"}
    dependents = types.describe_dependents(live.get("dependents", []))
    if dependents:
        details["Dependents"] = dependents
    return details


class UserDefinedEnumRuntimeProfile:
    profile_id = PROFILE_ID
    exact_asset_class_path = USER_ENUM_CLASS
    runtime_entry_module = "asset_document/bootstrap.py"
    preflight_scratch = False

    def required_primitives(self):
        return REQUIREMENTS

    def inspect_profile(self):
        return {"Class": USER_ENUM_CLASS, "BodySections": list(BODY_KEYS), "ReadOnlySections": []}

    def create_template(self, target):
        return common.document(common.require_target(target, PREFIX), USER_ENUM_CLASS, {},
                               {"Tooltip": "", "Bitflags": False, "Enumerators": [{"Name": "First"}, {"Name": "Second"}]})

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
        if mode != "live_asset" or exact_class_path != USER_ENUM_CLASS or editor_asset_handle is None:
            raise _error("/Inspect", "UserDefinedEnum inspection reads a live enum.", "UserDefinedEnum.InspectNotAdmitted")
        live = read_live(types.TypeCalls(context, BY_ID, editor_asset_handle, prefix=PREFIX), dependents=True)
        document = common.document(exact_asset_object_path.rsplit(".", 1)[0], USER_ENUM_CLASS, {}, body_from_live(live))
        return {**canonicalize(document), "EnumeratorDetails": inspect_details(live, exact_asset_object_path)}

    def diff(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted))

    def diff_exact(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted))


def create_user_defined_enum_runtime_handler():
    return UserDefinedEnumRuntimeProfile()


USER_DEFINED_ENUM_PROFILE_META = ProfileMeta(
    PROFILE_ID, common.ROUTE, USER_ENUM_CLASS,
    tuple(RegionMeta(f"user_defined_enum.{name}.v1", f"/Body/{key}", RegionOwnership.MANAGED, identity, order,
                     DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE, DeletePolicy.EXPLICIT_ONLY, comparison)
          for name, key, identity, order, comparison in (
              ("tooltip", "Tooltip", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
              ("bitflags", "Bitflags", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
              ("enumerators", "Enumerators", IdentityPolicy.STABLE_KEY, OrderPolicy.PRESERVE, ComparisonPolicy.ORDERED))),
    REQUIREMENTS)
USER_DEFINED_ENUM_RUNTIME_REGISTRATION = RuntimeProfileRegistration(
    USER_DEFINED_ENUM_PROFILE_META, create_user_defined_enum_runtime_handler, RuntimeTargetPolicy.CREATE_OR_UPDATE,
    LIFECYCLE)

__all__ = ("USER_DEFINED_ENUM_RUNTIME_REGISTRATION", "USER_ENUM_CLASS", "UserDefinedEnumRuntimeProfile",
           "enumerator_ops")
