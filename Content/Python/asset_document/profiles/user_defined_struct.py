"""Registered UserDefinedStruct Profile (profile.user_defined_struct.v1): create, read and edit the
members of a User Defined Struct as the Struct editor does (FStructureEditorUtils).

Document (docs/architecture/asset-document-user-defined-types-matrix.md)::

    "Body": {
      "Tooltip": "One inventory item.",
      "Members": [                                        # the full ordered member list (omit = keep)
        {"Name": "Label", "Type": "text", "DefaultValue": "Sword", "Tooltip": "Shown in the UI"},
        {"Name": "Damage", "Type": "float", "DefaultValue": 12.5, "ClampMin": "0", "UIMax": "100"},
        {"Name": "Kind", "Type": "byte:/Game/Data/E_Kind.E_Kind", "DefaultValue": "Melee"},
        {"Name": "Tags", "Type": "array:name", "InstanceEditable": false},
        {"Name": "Power", "RenamedFrom": "Strength", "Type": "int", "SaveGame": true}
      ]
    }

* ``Name`` is the member name the Struct editor shows.  A member's identity is its GUID (the
  internal ``Name_N_GUID`` property name is never authored): ``RenamedFrom`` renames a member and
  keeps its data in every data table / Blueprint / struct that uses the struct.
* ``Type`` uses the Blueprint variable type form: ``"bool"``, ``"int"``, ``"int64"``, ``"byte"``,
  ``"float"`` (double, the editor's Float), ``"float32"``, ``"name"``, ``"string"``, ``"text"``,
  ``"struct:/Script/CoreUObject.Vector"``, ``"byte:/Game/E_Kind.E_Kind"`` (a User Defined Enum),
  ``"object:/Script/Engine.Actor"``, ``"class:..."``, ``"softobject:..."``, ``"softclass:..."``,
  ``"interface:..."``, ``"array:<type>"``, ``"set:<type>"`` and maps as
  ``{"Category": "name", "Container": "map", "ValueType": "int"}``.  A type change resets the
  member's default (as the editor does).
* ``DefaultValue`` uses the typed value codec of DataTable row values (omit = keep; a struct value
  may name a subset of fields).  ``Tooltip``, ``InstanceEditable`` (default true), ``SaveGame``,
  ``MultiLine`` (string / text members), ``Widget3D`` (Vector / Transform members) and the value
  range ``ClampMin`` / ``ClampMax`` / ``UIMin`` / ``UIMax`` (numeric members) are the editor's
  per-member settings; omitted ones mean their defaults.
* A struct keeps at least one member (the editor cannot remove the last one).

Changing a struct makes the editor update the loaded assets that use it (data table rows are
re-read by member GUID, Blueprints recompile, containing structs recompile).  An apply that would
do so is refused while one of them has unsaved changes (``UserDefinedStruct.DependentHasUnsavedChanges``);
on success every updated package is reported as a warning; on failure the struct and the updated
dependents are restored.  Writes are covered by Core's package snapshot; preflight returns
``SNAPSHOT_COVERED``.
"""
from __future__ import annotations

from asset_document.diff.model import Difference, DiffKind
from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, IdentityPolicy, OmissionPolicy, OrderPolicy, ProfileMeta,
    RegionMeta, RegionOwnership,
)
from asset_document.diagnostics import AssetDocumentContractError, with_diagnostics
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document.profiles import data_table_common as common
from asset_document.profiles import user_defined_types_common as types
from asset_document.profiles.ublueprint_members import type_body, type_dto
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements

USER_STRUCT_CLASS = "/Script/CoreUObject.UserDefinedStruct"
PROFILE_ID = "profile.user_defined_struct.v1"
PREFIX = "UserDefinedStruct"
BODY_KEYS = ("Tooltip", "Members")
FLAGS = {"InstanceEditable": ("instance_editable", True), "SaveGame": ("save_game", False),
         "MultiLine": ("multi_line", False), "Widget3D": ("widget_3d", False)}
RANGE_KEYS = ("ClampMin", "ClampMax", "UIMin", "UIMax")
MEMBER_KEYS = ("Name", "RenamedFrom", "Type", "DefaultValue", "Tooltip", *FLAGS, *RANGE_KEYS)
LIFECYCLE = generic_factory_lifecycle(common.ROUTE)
REQUIREMENTS = (common.requirements(common.CLASS_RESOLVE, common.RESOLVE, types.USER_STRUCT_READ, types.USER_STRUCT_EDIT)
                + lifecycle_requirements(LIFECYCLE))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}
# Primitive op fields -> document member fields (error paths).
OP_FIELDS = {"type": "/Type", "value": "/DefaultValue", "to": "/Name", "name": "/Name", "tooltip": "/Tooltip",
             "instance_editable": "/InstanceEditable", "save_game": "/SaveGame", "multi_line": "/MultiLine",
             "widget_3d": "/Widget3D", "metadata": "", "names": ""}
TYPE_FORM = {
    "Scalars": ["bool", "byte", "int", "int64", "float", "float32", "name", "string", "text"],
    "Struct": "struct:/Script/CoreUObject.Vector",
    "Enum": "byte:/Game/Data/E_Kind.E_Kind",
    "References": ["object:/Script/Engine.Texture2D", "class:/Script/Engine.Actor", "softobject:/Script/Engine.StaticMesh",
                   "softclass:/Script/Engine.Pawn", "interface:/Script/Engine.BlendableInterface"],
    "Containers": ["array:int", "set:name", {"Category": "name", "Container": "map", "ValueType": "float"}],
    "Notes": "\"float\" is the editor's (double precision) Float; \"double\" is accepted as the same type. "
             "DefaultValue uses the DataTable row value form (numbers, strings, enum display names, "
             "{field: value} structs, lists, object paths).",
}


def _error(path, message, code="UserDefinedStruct.InvalidMember"):
    return common.error(path, message, code)


# ---------------------------------------------------------------------------
# Document shape
# ---------------------------------------------------------------------------
def _range_value(value, path):
    if type(value) is bool or type(value) not in (str, int, float):
        raise _error(path, "A value range bound is a number (or numeric string); \"\" clears it.")
    text = value if type(value) is str else repr(value) if type(value) is float else str(value)
    if text:
        try:
            float(text)
        except ValueError:
            raise _error(path, f"A value range bound is a number (got {value!r}).") from None
    return text


def type_key(dto):
    value = dto.get("value_type") or {}
    return (dto.get("category", ""), dto.get("sub_category", ""), dto.get("sub_category_object", ""),
            dto.get("container", "none"), value.get("category", ""), value.get("sub_category", ""),
            value.get("sub_category_object", ""))


def canonical_member(row, path):
    if type(row) is not dict:
        raise _error(path, "A member is an object with Name and Type.")
    for key in row:
        if key not in MEMBER_KEYS:
            raise _error(f"{path}/{common.escape(key)}", f"Unknown member key '{key}'. Keys: {', '.join(MEMBER_KEYS)}.",
                         "UserDefinedStruct.UnknownMemberKey")
    out = {"Name": types.validate_label(row.get("Name"), f"{path}/Name", "UserDefinedStruct.InvalidMemberName", "member")}
    if "RenamedFrom" in row:
        out["RenamedFrom"] = types.validate_label(row["RenamedFrom"], f"{path}/RenamedFrom",
                                                  "UserDefinedStruct.InvalidMemberName", "member")
    if "Type" not in row:
        raise _error(f"{path}/Type", "Type is required, e.g. \"float\", \"struct:/Script/CoreUObject.Vector\" or "
                     "\"array:name\".", "UserDefinedStruct.InvalidMemberType")
    try:
        dto = type_dto(row["Type"], f"{path}/Type")
    except AssetDocumentContractError as failure:
        raise _error(f"{path}/Type", failure.diagnostic.message, "UserDefinedStruct.InvalidMemberType") from None
    if dto.get("is_ref") or dto.get("is_const") or dto["category"] in ("exec", "wildcard", "delegate", "mcdelegate"):
        raise _error(f"{path}/Type", "A struct member is a value type (no exec, wildcard, delegate, reference or const).",
                     "UserDefinedStruct.InvalidMemberType")
    out["Type"] = type_body(dto)
    if "DefaultValue" in row:
        out["DefaultValue"] = row["DefaultValue"]
    if "Tooltip" in row:
        if type(row["Tooltip"]) is not str:
            raise _error(f"{path}/Tooltip", "Tooltip is a string.")
        out["Tooltip"] = row["Tooltip"]
    for key in FLAGS:
        if key in row:
            if type(row[key]) is not bool:
                raise _error(f"{path}/{key}", f"{key} is true or false.")
            out[key] = row[key]
    for key in RANGE_KEYS:
        if key in row:
            out[key] = _range_value(row[key], f"{path}/{key}")
    return out


def canonical_body(body):
    out = {}
    if "Tooltip" in body:
        if type(body["Tooltip"]) is not str:
            raise _error("/Body/Tooltip", "Tooltip is a string.", "UserDefinedStruct.InvalidTooltip")
        out["Tooltip"] = body["Tooltip"]
    if "Members" in body:
        members = body["Members"]
        if type(members) is not list or not members:
            raise _error("/Body/Members", "Members is a non-empty ordered list of members (a User Defined Struct keeps "
                         "at least one member).", "UserDefinedStruct.InvalidMembers")
        names, sources, canonical = set(), set(), []
        for index, row in enumerate(members):
            path = f"/Body/Members/{index}"
            member = canonical_member(row, path)
            if member["Name"].lower() in names:
                raise _error(f"{path}/Name", f"Member '{member['Name']}' appears more than once (member names are "
                             "unique ignoring case).", "UserDefinedStruct.DuplicateMember")
            names.add(member["Name"].lower())
            source = member.get("RenamedFrom")
            if source is not None:
                if source.lower() == member["Name"].lower() or source.lower() in sources:
                    raise _error(f"{path}/RenamedFrom", "RenamedFrom names another member, once.",
                                 "UserDefinedStruct.InvalidRename")
                sources.add(source.lower())
            canonical.append(member)
        out["Members"] = canonical
    return out


def canonicalize(document):
    body = common.validate_header(document, (USER_STRUCT_CLASS,), PREFIX, BODY_KEYS)
    canonical_body(body)
    if document.get("Properties"):
        raise _error("/Properties", "A User Defined Struct has no Details-panel properties; author Body.Tooltip and "
                     "Body.Members.", "UserDefinedStruct.UnsupportedProperties")
    return common.snapshot(document)


# ---------------------------------------------------------------------------
# Live state
# ---------------------------------------------------------------------------
def read_live(calls, dependents=False):
    return calls.query(types.USER_STRUCT_READ, {"dependents": True} if dependents else {})


def member_from_live(row):
    member = {"Name": row["name"], "Type": type_body(row["type"])}
    if row.get("supported") and row.get("default") is not None:
        member["DefaultValue"] = row["default"]
    if row.get("tooltip"):
        member["Tooltip"] = row["tooltip"]
    for key, (field, default) in FLAGS.items():
        if bool(row.get(field, default)) != default:
            member[key] = bool(row.get(field))
    for key in RANGE_KEYS:
        if row.get("metadata", {}).get(key):
            member[key] = row["metadata"][key]
    return member


def body_from_live(live):
    return {"Tooltip": live.get("tooltip", ""), "Members": [member_from_live(row) for row in live.get("members", [])]}


def _settings(member):
    """Per-member settings with defaults filled in (document form)."""
    out = {"Tooltip": member.get("Tooltip", "")}
    for key, (_, default) in FLAGS.items():
        out[key] = member.get(key, default)
    for key in RANGE_KEYS:
        out[key] = member.get(key, "")
    return out


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------
def _match(members, live_rows):
    """desired index -> live row (by name ignoring case, then RenamedFrom)."""
    by_lower = {row["name"].lower(): row for row in live_rows}
    claimed, used = {}, set()
    for index, member in enumerate(members):
        row = by_lower.get(member["Name"].lower())
        if row is not None:
            claimed[index] = row
            used.add(row["name"])
    for index, member in enumerate(members):
        source = member.get("RenamedFrom")
        if index in claimed or source is None:
            continue
        row = by_lower.get(source.lower())
        if row is not None and row["name"] not in used:
            claimed[index] = row
            used.add(row["name"])
    return claimed, used


def member_ops(body, live):
    """Edit ops (with document paths) that turn the live struct into the authored body, and the plan
    facts the warnings need ({"retyped": [...], "removed": [...]})."""
    ops, facts = [], {"retyped": [], "removed": []}
    live_rows = list(live.get("members", []))
    if "Members" in body:
        members = body["Members"]
        claimed, used = _match(members, live_rows)
        # Names match first (ignoring case), so a rename target is free once the removes ran.
        renames = [index for index, row in claimed.items() if row["name"] != members[index]["Name"]]
        removed = [row["name"] for row in live_rows if row["name"] not in used]
        facts["removed"] = removed
        # The editor cannot remove the last member: when every member goes, the last one goes after the adds.
        deferred = removed[-1:] if len(removed) == len(live_rows) else []
        ops += [({"op": "remove", "name": name}, "/Body/Members") for name in removed if name not in deferred]
        for index in renames:
            ops.append(({"op": "rename", "name": claimed[index]["name"], "to": members[index]["Name"]}, f"/Body/Members/{index}"))
        for index, member in enumerate(members):
            if index not in claimed:
                ops.append(({"op": "add", "name": member["Name"], "type": type_dto(member["Type"], "/Type")},
                            f"/Body/Members/{index}"))
        ops += [({"op": "remove", "name": name}, "/Body/Members") for name in deferred]
        retyped = set()
        for index, row in claimed.items():
            dto = type_dto(members[index]["Type"], "/Type")
            if type_key(dto) != type_key(row["type"]):
                retyped.add(index)
                facts["retyped"].append(members[index]["Name"])
                ops.append(({"op": "retype", "name": members[index]["Name"], "type": dto}, f"/Body/Members/{index}"))
        order_now = [members[index]["Name"] for index in sorted(claimed, key=lambda item: live_rows.index(claimed[item]))]
        order_now += [member["Name"] for index, member in enumerate(members) if index not in claimed]
        desired_order = [member["Name"] for member in members]
        if order_now != desired_order:
            ops.append(({"op": "order", "names": desired_order}, "/Body/Members"))
        for index, member in enumerate(members):
            path = f"/Body/Members/{index}"
            row = claimed.get(index)
            have = _settings(member_from_live(row)) if row is not None else _settings({})
            want = _settings(member)
            op = {"op": "set", "name": member["Name"]}
            if want["Tooltip"] != have["Tooltip"]:
                op["tooltip"] = want["Tooltip"]
            for key, (field, _) in FLAGS.items():
                if want[key] != have[key]:
                    op[field] = want[key]
            metadata = {key: want[key] for key in RANGE_KEYS if want[key] != have[key]}
            if metadata:
                op["metadata"] = metadata
            if len(op) > 2:
                ops.append((op, path))
            if "DefaultValue" in member:
                fresh = row is None or index in retyped
                live_default = None if fresh else row.get("default")
                if fresh or not row.get("supported") or not types.covers(member["DefaultValue"], live_default):
                    ops.append(({"op": "default", "name": member["Name"], "value": member["DefaultValue"]}, path))
    if "Tooltip" in body and body["Tooltip"] != live.get("tooltip", ""):
        ops.append(({"op": "tooltip", "tooltip": body["Tooltip"]}, "/Body/Tooltip"))
    return ops, facts


def apply_warnings(facts, dependents):
    diagnostics = types.dependents_warnings(dependents, "/Body/Members", PREFIX)
    if dependents and facts["retyped"]:
        diagnostics.append(types.warning("/Body/Members", "UserDefinedStruct.MemberValuesConverted",
                                         "Stored values of the retyped member(s) " + ", ".join(facts["retyped"])
                                         + " in the assets above were converted where the old value fits the new type "
                                         "and otherwise reset to the type default (as in the editor)."))
    if dependents and facts["removed"]:
        diagnostics.append(types.warning("/Body/Members", "UserDefinedStruct.MemberValuesRemoved",
                                         "Stored values of the removed member(s) " + ", ".join(facts["removed"])
                                         + " were dropped from the assets above (as in the editor)."))
    return diagnostics


def apply(document, read, mutable, context):
    body = canonical_body(document.get("Body", {}))
    calls = types.TypeCalls(context, BY_ID, read, mutable, PREFIX)
    live = read_live(calls)
    ops, facts = member_ops(body, live)
    if not ops:
        return ()
    changed, dependents = calls.edit_all(types.USER_STRUCT_EDIT, ops, OP_FIELDS, "/Body/Members")
    return with_diagnostics(changed, apply_warnings(facts, dependents))


# ---------------------------------------------------------------------------
# Extract / Diff / Inspect
# ---------------------------------------------------------------------------
def extract(target, read, context):
    live = read_live(types.TypeCalls(context, BY_ID, read, prefix=PREFIX))
    return common.document(target, USER_STRUCT_CLASS, {}, body_from_live(live))


def diff(authored, extracted, exact):
    changes = common.header_diff(authored, extracted)
    body = canonical_body(authored.get("Body", {}))
    live = canonical_body(extracted.get("Body", {}))
    if "Tooltip" in body and body["Tooltip"] != live.get("Tooltip", ""):
        changes.append(Difference("/Body/Tooltip", DiffKind.REPLACE, True, body["Tooltip"], "Tooltip" in live, live.get("Tooltip")))
    if "Members" in body:
        live_members = live.get("Members", [])
        if [member["Name"] for member in body["Members"]] != [member["Name"] for member in live_members]:
            changes.append(Difference("/Body/Members", DiffKind.REPLACE, True, body["Members"], True, live_members))
            return tuple(changes)
        for index, (member, observed) in enumerate(zip(body["Members"], live_members)):
            path = f"/Body/Members/{index}"
            if type_key(type_dto(member["Type"], "/Type")) != type_key(type_dto(observed["Type"], "/Type")):
                changes.append(Difference(f"{path}/Type", DiffKind.REPLACE, True, member["Type"], True, observed["Type"]))
            want, have = _settings(member), _settings(observed)
            for key in want:
                if want[key] != have[key]:
                    changes.append(Difference(f"{path}/{key}", DiffKind.REPLACE, True, want[key], key in observed, observed.get(key)))
            if "DefaultValue" in member and ("DefaultValue" not in observed
                                             or not types.covers(member["DefaultValue"], observed["DefaultValue"], exact)):
                changes.append(Difference(f"{path}/DefaultValue", DiffKind.REPLACE, True, member["DefaultValue"],
                                          "DefaultValue" in observed, observed.get("DefaultValue")))
    return tuple(changes)


def inspect_details(live, object_path=""):
    """Compact: what an author needs beyond the document (settings that only exist when set)."""
    members = []
    for row in live.get("members", []):
        member = {"Name": row["name"], "CppType": row.get("cpp_type", ""), "Codec": row.get("codec", "")}
        allows = [key for key, field in (("MultiLine", "multi_line_allowed"), ("Widget3D", "widget_3d_allowed"),
                                         ("ValueRange", "value_range_allowed")) if row.get(field)]
        if allows:
            member["Allows"] = allows
        if not row.get("supported"):
            member["DefaultSupported"] = False
        if row.get("invalid"):
            member["Invalid"] = True
        members.append(member)
    details = {"Status": live.get("status", ""), "Members": members, "TypeForm": TYPE_FORM}
    if live.get("error_message"):
        details["ErrorMessage"] = live["error_message"]
    if object_path:
        details["UseAs"] = {"RowStruct": object_path, "Type": f"struct:{object_path}"}
    dependents = types.describe_dependents(live.get("dependents", []))
    if dependents:
        details["Dependents"] = dependents
    return details


class UserDefinedStructRuntimeProfile:
    profile_id = PROFILE_ID
    exact_asset_class_path = USER_STRUCT_CLASS
    runtime_entry_module = "asset_document/bootstrap.py"
    preflight_scratch = False

    def required_primitives(self):
        return REQUIREMENTS

    def inspect_profile(self):
        return {"Class": USER_STRUCT_CLASS, "BodySections": list(BODY_KEYS), "ReadOnlySections": [],
                "TypeForm": TYPE_FORM}

    def create_template(self, target):
        return common.document(common.require_target(target, PREFIX), USER_STRUCT_CLASS, {},
                               {"Tooltip": "", "Members": [{"Name": "Value", "Type": "float", "DefaultValue": 0.0}]})

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
        if mode != "live_asset" or exact_class_path != USER_STRUCT_CLASS or editor_asset_handle is None:
            raise _error("/Inspect", "UserDefinedStruct inspection reads a live struct.", "UserDefinedStruct.InspectNotAdmitted")
        live = read_live(types.TypeCalls(context, BY_ID, editor_asset_handle, prefix=PREFIX), dependents=True)
        document = common.document(exact_asset_object_path.rsplit(".", 1)[0], USER_STRUCT_CLASS, {}, body_from_live(live))
        return {**canonicalize(document), "MemberDetails": inspect_details(live, exact_asset_object_path)}

    def diff(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=False)

    def diff_exact(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=True)


def create_user_defined_struct_runtime_handler():
    return UserDefinedStructRuntimeProfile()


USER_DEFINED_STRUCT_PROFILE_META = ProfileMeta(
    PROFILE_ID, common.ROUTE, USER_STRUCT_CLASS,
    (RegionMeta("user_defined_struct.tooltip.v1", "/Body/Tooltip", RegionOwnership.MANAGED, IdentityPolicy.POSITIONAL,
                OrderPolicy.CANONICAL, DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE, DeletePolicy.EXPLICIT_ONLY,
                ComparisonPolicy.CANONICAL),
     RegionMeta("user_defined_struct.members.v1", "/Body/Members", RegionOwnership.MANAGED, IdentityPolicy.STABLE_KEY,
                OrderPolicy.PRESERVE, DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE, DeletePolicy.EXPLICIT_ONLY,
                ComparisonPolicy.ORDERED)),
    REQUIREMENTS)
USER_DEFINED_STRUCT_RUNTIME_REGISTRATION = RuntimeProfileRegistration(
    USER_DEFINED_STRUCT_PROFILE_META, create_user_defined_struct_runtime_handler, RuntimeTargetPolicy.CREATE_OR_UPDATE,
    LIFECYCLE)

__all__ = ("USER_DEFINED_STRUCT_RUNTIME_REGISTRATION", "USER_STRUCT_CLASS", "UserDefinedStructRuntimeProfile",
           "member_ops")
