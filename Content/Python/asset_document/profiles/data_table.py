"""Registered DataTable Profiles: UDataTable, UCompositeDataTable and UMirrorDataTable.

Document (docs/architecture/asset-document-datatable-matrix.md)::

    "Body": {
      "RowStruct": "/Script/MyGame.ItemRow",          # create: the New Data Table dialog's row struct
      "Rows": [                                       # the full ordered row list (omit = keep)
        {"Name": "Sword", "Values": {"Damage": 12, "Label": "Sword"}},
        {"Name": "Bow", "RenamedFrom": "Bow_Old", "Values": {"Range": 900.5}}
      ]
    },
    "Properties": {"bStripFromClientBuilds": false, ...}

* ``RowStruct`` is required to create (native USTRUCT or User Defined Struct path; resolved and set
  on UDataTableFactory as the New Asset dialog does) and immutable afterwards (the editor offers no
  row struct change): a different value fails with ``DataTable.RowStructImmutable``.
* ``Rows`` is ordered (the editor's row order); a row's ``Values`` is the complete row: a field it
  leaves out is the row struct's default (the value a new row gets), so applying a row rewrites the
  whole row (fields the codec cannot write keep their value).  ``RenamedFrom`` asks for a rename (FDataTableEditorUtils::
  RenameRow keeps the row data) instead of remove + add; it is ignored once the rename happened.
  Field names are the row struct's authored names (Inspect lists them under ``RowSchema``); values
  use the generic route's typed value codec (hard/soft references, FText, structs, containers,
  FInstancedStruct).  Instanced subobject fields (the row editor cannot create them: no owning
  object) are not extracted and are refused when authored (``DataTable.UnsupportedField``).
* CompositeDataTable: ``ParentTables`` (ordered, later parents override earlier ones) is authored;
  ``Rows`` are derived from the parents and read-only (given Rows must equal the derived rows).
* MirrorDataTable: rows and Properties update like a DataTable.  Creating one is the New Mirror Data
  Table dialog: top-level ``FactoryContext`` is the Skeleton it asks for and ``Body.RowStruct`` the
  "Row Structure" (default ``/Script/Engine.MirrorTableRow``, the dialog's default; it must have a
  ``MirroredName`` field, the dialog's struct filter).  Both go to UMirrorDataTableFactory as the two
  factory context objects (``Skeleton``, ``Struct``); the factory fills the rows from the skeleton
  with the project's find/replace expressions (Animation settings), as the dialog's defaults do.

Writes are covered by Core's package snapshot (existing target) or the factory rollback (created
target); preflight therefore returns ``SNAPSHOT_COVERED``.
"""
from __future__ import annotations

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.diff.model import Difference, DiffKind
from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, IdentityPolicy, OmissionPolicy, OrderPolicy, ProfileMeta,
    RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document.profiles import data_table_common as common
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument, _single_handle
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements

DATA_TABLE_CLASS = "/Script/Engine.DataTable"
COMPOSITE_DATA_TABLE_CLASS = "/Script/Engine.CompositeDataTable"
MIRROR_DATA_TABLE_CLASS = "/Script/Engine.MirrorDataTable"
SCRIPT_STRUCT_CLASS = "/Script/CoreUObject.ScriptStruct"
ROW_KEYS = ("Name", "Values", "RenamedFrom")
# The New Data Table dialog's row struct is the factory context (UDataTableFactory::Struct).
LIFECYCLE = generic_factory_lifecycle(common.ROUTE, ("factory_context",))
# The New Mirror Data Table dialog sets two factory objects: Skeleton and Struct (the row struct).
MIRROR_LIFECYCLE = generic_factory_lifecycle(common.ROUTE, ("factory_context", "factory_context_2"))
MIRROR_ROW_STRUCT = "/Script/Engine.MirrorTableRow"
SKELETON_CLASS = "/Script/Engine.Skeleton"
REQUIREMENTS = (common.requirements(common.CLASS_RESOLVE, common.RESOLVE, common.REFERENCE_RESOLVE,
                                    common.DATA_TABLE_READ, common.DATA_TABLE_EDIT)
                + lifecycle_requirements(LIFECYCLE))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}
PREFIX = "DataTable"


def _composite(class_path):
    return class_path == COMPOSITE_DATA_TABLE_CLASS


def _body_keys(class_path):
    return ("RowStruct", "ParentTables", "Rows") if _composite(class_path) else ("RowStruct", "Rows")


# ---------------------------------------------------------------------------
# Document shape
# ---------------------------------------------------------------------------
def canonical_body(body, class_path):
    out = {}
    if "RowStruct" in body:
        value = body["RowStruct"]
        if type(value) is not str or not value.startswith("/") or "." not in value:
            raise common.error("/Body/RowStruct", "RowStruct is the object path of a struct, e.g. "
                               "/Script/MyGame.ItemRow or /Game/Data/S_Item.S_Item.", "DataTable.InvalidRowStruct")
        out["RowStruct"] = value
    if "ParentTables" in body:
        tables = body["ParentTables"]
        if type(tables) is not list or any(type(item) is not str or not item for item in tables):
            raise common.error("/Body/ParentTables", "ParentTables is an ordered list of DataTable object paths.",
                               "DataTable.InvalidParentTables")
        out["ParentTables"] = list(tables)
    if "Rows" in body:
        rows = body["Rows"]
        if type(rows) is not list:
            raise common.error("/Body/Rows", "Rows is an ordered list of {\"Name\", \"Values\"} objects.",
                               "DataTable.InvalidRows")
        names, sources, canonical = set(), set(), []
        for index, row in enumerate(rows):
            path = f"/Body/Rows/{index}"
            if type(row) is not dict:
                raise common.error(path, "A row is an object with Name and optional Values / RenamedFrom.",
                                   "DataTable.InvalidRow")
            for key in row:
                if key not in ROW_KEYS:
                    raise common.error(f"{path}/{common.escape(key)}", f"Unknown row key '{key}'. Keys: Name, Values, "
                                       "RenamedFrom (field values go inside Values).", "DataTable.UnknownRowKey")
            name = common.validate_name(row.get("Name"), f"{path}/Name", PREFIX)
            if name.lower() in names:
                raise common.error(f"{path}/Name", f"Row '{name}' appears more than once (row names are "
                                   "case-insensitive).", "DataTable.DuplicateRow")
            names.add(name.lower())
            item = {"Name": name}
            if "Values" in row:
                if type(row["Values"]) is not dict:
                    raise common.error(f"{path}/Values", "Values is an object of field name -> value.",
                                       "DataTable.InvalidValues")
                item["Values"] = dict(row["Values"])
            if "RenamedFrom" in row:
                source = common.validate_name(row["RenamedFrom"], f"{path}/RenamedFrom", PREFIX)
                if source.lower() == name.lower() or source.lower() in sources:
                    raise common.error(f"{path}/RenamedFrom", "RenamedFrom names another row, once.",
                                       "DataTable.InvalidRename")
                sources.add(source.lower())
                item["RenamedFrom"] = source
            canonical.append(item)
        out["Rows"] = canonical
    return out


def canonicalize(document):
    class_paths = (DATA_TABLE_CLASS, COMPOSITE_DATA_TABLE_CLASS, MIRROR_DATA_TABLE_CLASS)
    body = common.validate_header(document, class_paths, PREFIX, ("RowStruct", "ParentTables", "Rows"),
                                  ("FactoryContext",))
    class_path = document["Class"]
    if "FactoryContext" in document:
        value = document["FactoryContext"]
        if class_path != MIRROR_DATA_TABLE_CLASS:
            raise common.error("/FactoryContext", "FactoryContext is only used to create a MirrorDataTable (its "
                               "Skeleton); a data table's create context is Body.RowStruct.",
                               "DataTable.UnknownDocumentKey")
        if type(value) is not str or not value.startswith("/") or "." not in value:
            raise common.error("/FactoryContext", "FactoryContext is the object path of the Skeleton the New Mirror "
                               "Data Table dialog asks for, e.g. /Game/Characters/SK_Hero.SK_Hero.",
                               "MirrorDataTable.InvalidFactoryContext")
    for key in body:
        if key not in _body_keys(class_path):
            raise common.error(f"/Body/{key}", f"{key} is only authored on a CompositeDataTable.",
                               "DataTable.UnknownBodyKey")
    canonical_body(body, class_path)
    return common.snapshot(document)


# ---------------------------------------------------------------------------
# Live state
# ---------------------------------------------------------------------------
def read_live(calls):
    first, rows = calls.query_all(common.DATA_TABLE_READ, "rows")
    return first, [{"Name": row["name"], "Values": row["values"]} for row in rows]


def _schema(first):
    fields = []
    for row in first.get("fields", []):
        field = {"Name": row["name"], "Type": row["type"], "Codec": row["codec"], "Supported": row["supported"]}
        if "enum_values" in row:
            field["EnumValues"] = row["enum_values"]
        fields.append(field)
    return {"RowStruct": first.get("row_struct", ""), "Fields": fields}


def row_defaults(first):
    """The values of a new row (``row_defaults`` of the read's first page), or None from an older provider."""
    found = first.get("row_defaults")
    values = found.get("values") if type(found) is dict else None
    return dict(values) if type(values) is dict else None


def _body(first, rows):
    body = {"RowStruct": first.get("row_struct", "")}
    if first.get("composite"):
        body["ParentTables"] = list(first.get("parent_tables", []))
    body["Rows"] = rows
    return body


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------
# Row names per order op: a long order goes as consecutive {"op": "order", "rows", "at"} segments,
# so no single op outgrows the per-call value budget (chunk_ops only splits between ops).
ORDER_SEGMENT = 1000


def _row_differs(values, current, defaults):
    """Whether a live row differs from the authored row (``defaults``: unwritten fields are these)."""
    if any(key not in current or not common.same(value, current[key]) for key, value in values.items()):
        return True
    return defaults is not None and any(key not in values and key in defaults and not common.same(defaults[key], value)
                                        for key, value in current.items())


def row_ops(authored, live, defaults=None):
    """Edit ops (with document paths) that turn the live rows into the authored rows. ``defaults`` (the
    row struct's defaults): a row is the complete row, so a live field the row leaves out goes back to
    its default (a "replace" set); None keeps the fields a row leaves out."""
    live_names = [row["Name"] for row in live]
    live_values = {row["Name"]: row["Values"] for row in live}
    authored_names = [row["Name"] for row in authored]
    authored_set = set(authored_names)
    renames = {}
    for row in authored:
        source = row.get("RenamedFrom")
        if source is not None and row["Name"] not in live_values and source in live_values \
                and source not in authored_set:
            renames[row["Name"]] = source
    sources = set(renames.values())
    ops = [({"op": "remove", "row": name}, "/Body/Rows") for name in live_names
           if name not in authored_set and name not in sources]
    for index, row in enumerate(authored):
        name = row["Name"]
        if name in renames:
            ops.append(({"op": "rename", "row": renames[name], "to": name}, f"/Body/Rows/{index}/RenamedFrom"))
        elif name not in live_values:
            ops.append(({"op": "add", "row": name}, f"/Body/Rows/{index}/Name"))
    structural = bool(ops)
    for index, row in enumerate(authored):
        values = row.get("Values") or {}
        current = live_values.get(renames.get(row["Name"], row["Name"]))
        if current is None:
            if values:  # a new row starts at the defaults
                ops.append(({"op": "set", "row": row["Name"], "values": values}, f"/Body/Rows/{index}"))
        elif _row_differs(values, current, defaults):
            op = {"op": "set", "row": row["Name"], "values": values}
            if defaults is not None:
                op["replace"] = True
            ops.append((op, f"/Body/Rows/{index}"))
    # Removes, adds and renames can leave rows out of place; value edits cannot. The provider only
    # moves rows that are out of place, so an order that already holds costs no edit.
    if structural or [name for name in live_names if name in authored_set] != authored_names:
        for at in range(0, len(authored_names), ORDER_SEGMENT):
            ops.append(({"op": "order", "rows": authored_names[at:at + ORDER_SEGMENT], "at": at}, "/Body/Rows"))
    return ops


_BASE_PATHS = {"row_struct": "/Body/RowStruct", "ops": "/Body/Rows"}


def _rows_mismatch(authored_rows, live_rows, exact, defaults=None):
    """The first authored path the live rows do not hold. ``defaults``: a field a row leaves out must hold
    the row struct's default (None: unwritten fields are not compared)."""
    if [row["Name"] for row in authored_rows] != [row["Name"] for row in live_rows]:
        return "/Body/Rows"
    live = {row["Name"]: row["Values"] for row in live_rows}
    for index, row in enumerate(authored_rows):
        values = row.get("Values", {})
        for key, value in values.items():
            if key not in live[row["Name"]] or not common.same(value, live[row["Name"]][key], exact):
                return f"/Body/Rows/{index}/Values/{common.escape(key)}"
        if defaults is not None:
            for key, value in live[row["Name"]].items():
                if key not in values and key in defaults and not common.same(defaults[key], value, exact):
                    return f"/Body/Rows/{index}/Values/{common.escape(key)}"
    return None


def apply(document, read, mutable, context):
    class_path = document["Class"]
    body = canonical_body(document.get("Body", {}), class_path)
    properties = document.get("Properties", {})
    region = _properties(class_path)
    region.check_owned(properties)
    calls = common.DataCalls(context, BY_ID, read, mutable, PREFIX)
    first, live = read_live(calls)
    row_struct = first.get("row_struct", "")
    if "RowStruct" in body and body["RowStruct"] != row_struct:
        raise common.error("/Body/RowStruct", f"The table's row struct is {row_struct}; the editor offers no row "
                           "struct change on an existing table (create a new table instead).",
                           "DataTable.RowStructImmutable")
    if ("RowStruct" in body or "Rows" in body) and first.get("row_struct_valid") is not True:
        # A create with a struct that cannot be a row struct fails here and is rolled back.
        raise common.error("/Body/RowStruct", f"'{row_struct or body.get('RowStruct')}' cannot be a data table row "
                           "struct: it must derive from FTableRowBase or be a User Defined Struct "
                           "(FDataTableEditorUtils::IsValidTableStruct).", "DataTable.InvalidRowStruct")
    if class_path == MIRROR_DATA_TABLE_CLASS and "RowStruct" in body \
            and not any(field.get("name") == "MirroredName" for field in first.get("fields", [])):
        # The New Mirror Data Table dialog's struct picker lists only row structs with a MirroredName field.
        raise common.error("/Body/RowStruct", f"'{row_struct}' cannot be a mirror table row struct: it needs a "
                           "MirroredName field (derive it from FMirrorTableRow).", "MirrorDataTable.InvalidRowStruct")
    changes = []
    ops = []
    if _composite(class_path):
        if "ParentTables" in body and body["ParentTables"] != first.get("parent_tables", []):
            ops.append(({"op": "parent_tables", "tables": body["ParentTables"]}, "/Body/ParentTables"))
    elif "Rows" in body:
        ops.extend(row_ops(body["Rows"], live, row_defaults(first)))
    if ops:
        changes.extend(calls.edit(common.DATA_TABLE_EDIT, {"row_struct": row_struct}, ops, _BASE_PATHS))
    if _composite(class_path) and "Rows" in body:
        _, derived = read_live(calls)
        where = _rows_mismatch(body["Rows"], derived, exact=False)
        if where is not None:
            raise common.error(where, "A CompositeDataTable's rows come from its ParentTables and are read-only; "
                               "omit Rows or give the derived rows.", "CompositeDataTable.RowsReadOnly")
    changes.extend(region.apply(properties, read, mutable, context))
    return tuple(changes)


def _properties(class_path):
    owned = {"RowStruct": "Body.RowStruct", "ParentTables": "Body.ParentTables"}
    return common.PropertiesRegion(class_path, owned, PREFIX)


def _resolve_row_struct(path, context):
    try:
        outcome = common.invoke(context, BY_ID, common.REFERENCE_RESOLVE,
                                {"expected_class": SCRIPT_STRUCT_CLASS, "path": path},
                                path="/Body/RowStruct", prefix=PREFIX)
    except AssetDocumentContractError as failure:
        raise common.error("/Body/RowStruct", f"RowStruct '{path}' is not a loadable struct: "
                           f"{failure.diagnostic.message} list_types (kind struct, base "
                           "/Script/Engine.TableRowBase) lists the row structs.",
                           "DataTable.RowStructNotFound") from None
    return _single_handle(outcome, "asset")


def _mirror_create_context(document, body, context):
    """The New Mirror Data Table dialog: the Skeleton (FactoryContext) and the Row Structure (Body.RowStruct,
    default FMirrorTableRow), set on UMirrorDataTableFactory::Skeleton / ::Struct."""
    skeleton = document.get("FactoryContext")
    if skeleton is None:
        raise common.error("/FactoryContext", "Creating a MirrorDataTable requires FactoryContext: the object path of "
                           "the Skeleton the New Mirror Data Table dialog asks for (its bones, sockets, notifies and "
                           "curves become the rows).", "MirrorDataTable.SkeletonRequired")
    try:
        outcome = common.invoke(context, BY_ID, common.REFERENCE_RESOLVE,
                                {"expected_class": SKELETON_CLASS, "path": skeleton},
                                path="/FactoryContext", prefix=PREFIX)
    except AssetDocumentContractError as failure:
        raise common.error("/FactoryContext", f"FactoryContext '{skeleton}' is not a loadable Skeleton: "
                           f"{failure.diagnostic.message}", "MirrorDataTable.SkeletonNotFound") from None
    return (NamedOpaqueHandleArgument("factory_context", _single_handle(outcome, "asset")),
            NamedOpaqueHandleArgument("factory_context_2",
                                      _resolve_row_struct(body.get("RowStruct", MIRROR_ROW_STRUCT), context)))


def create_target_context(document, context):
    body = canonical_body(document.get("Body", {}), document["Class"])
    if document["Class"] == MIRROR_DATA_TABLE_CLASS:
        return _mirror_create_context(document, body, context)
    if "RowStruct" not in body:
        raise common.error("/Body/RowStruct", "Creating a data table requires Body.RowStruct (the row struct the "
                           "New Data Table dialog asks for).", "DataTable.RowStructRequired")
    return (NamedOpaqueHandleArgument("factory_context", _resolve_row_struct(body["RowStruct"], context)),)


# ---------------------------------------------------------------------------
# Extract / Diff
# ---------------------------------------------------------------------------
def extract(target, class_path, read, context, include_defaults=True):
    calls = common.DataCalls(context, BY_ID, read, prefix=PREFIX)
    first, rows = read_live(calls)
    return common.document(target, class_path, _properties(class_path).extract(read, context, include_defaults), _body(first, rows))


def extract_with_defaults(target, class_path, read, context):
    """The extraction and the row struct's defaults (a row's unwritten fields compare against them)."""
    calls = common.DataCalls(context, BY_ID, read, prefix=PREFIX)
    first, rows = read_live(calls)
    document = common.document(target, class_path, _properties(class_path).extract(read, context, True), _body(first, rows))
    return document, (None if _composite(class_path) else row_defaults(first))


def _value_differences(index, row, current, exact, defaults=None):
    """The values of one row (the live row of the same name) that differ: each authored value, and with
    ``defaults`` each field the row leaves out that is off the row struct's default."""
    values, live = row.get("Values", {}), current["Values"]
    where = f"/Body/Rows/{index}/Values/"
    changes = [Difference(where + common.escape(key), DiffKind.REPLACE, True, value, key in live, live.get(key), row=row["Name"])
               for key, value in values.items() if key not in live or not common.same(value, live[key], exact)]
    if defaults is not None:
        changes.extend(Difference(where + common.escape(key), DiffKind.REPLACE, True, defaults[key], True, value, row=row["Name"])
                       for key, value in live.items()
                       if key not in values and key in defaults and not common.same(defaults[key], value, exact))
    return changes


def diff(authored, extracted, exact, defaults=None):
    changes = common.header_diff(authored, extracted)
    class_path = authored["Class"]
    body = canonical_body(authored.get("Body", {}), class_path)
    live = extracted.get("Body", {})
    for key in ("RowStruct", "ParentTables"):
        if key in body and body[key] != live.get(key):
            changes.append(Difference(f"/Body/{key}", DiffKind.REPLACE, True, body[key], key in live, live.get(key)))
    if "Rows" in body:
        field_defaults = None if _composite(class_path) else defaults
        changes.extend(common.row_differences(body["Rows"], live.get("Rows", []),
                                              lambda index, row, current: _value_differences(index, row, current,
                                                                                             exact, field_defaults)))
    changes.extend(common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact))
    return tuple(changes)


# ---------------------------------------------------------------------------
# Handler
# ---------------------------------------------------------------------------
class DataTableRuntimeProfile:
    runtime_entry_module = "asset_document/bootstrap.py"
    # Snapshot-covered: preflight never reads a preview.
    preflight_scratch = False

    def __init__(self, class_path, profile_id):
        self.exact_asset_class_path = class_path
        self.profile_id = profile_id

    def required_primitives(self):
        return REQUIREMENTS

    def inspect_profile(self):
        return {"Class": self.exact_asset_class_path, "BodySections": list(_body_keys(self.exact_asset_class_path)),
                "ReadOnlySections": ["Rows"] if _composite(self.exact_asset_class_path) else [],
                "CreateContext": ("FactoryContext (Skeleton) + Body.RowStruct (default /Script/Engine.MirrorTableRow)"
                                  if self.exact_asset_class_path == MIRROR_DATA_TABLE_CLASS else "Body.RowStruct")}

    def create_template(self, target):
        body = {"RowStruct": "/Script/MyModule.MyTableRow", "Rows": []}
        if _composite(self.exact_asset_class_path):
            body = {"RowStruct": "/Script/MyModule.MyTableRow", "ParentTables": []}
        if self.exact_asset_class_path == MIRROR_DATA_TABLE_CLASS:
            # Rows omitted: the factory fills them from the skeleton.
            document = common.document(common.require_target(target, PREFIX), self.exact_asset_class_path, {},
                                       {"RowStruct": MIRROR_ROW_STRUCT})
            return {**document, "FactoryContext": "/Game/Characters/SK_Hero.SK_Hero"}
        return common.document(common.require_target(target, PREFIX), self.exact_asset_class_path, {}, body)

    def _canonical(self, document):
        canonical = canonicalize(document)
        if canonical["Class"] != self.exact_asset_class_path:
            raise common.error("/Class", f"Class must be {self.exact_asset_class_path}.", "DataTable.ClassMismatch")
        return canonical

    def validate(self, document):
        self._canonical(document)

    def canonicalize(self, document):
        return self._canonical(document)

    def create_target_context(self, document, context):
        return create_target_context(self._canonical(document), context)

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
            raise common.error("/Inspect", "DataTable inspection reads a live table asset.", "DataTable.InspectNotAdmitted")
        document = self.canonicalize(self.extract(target=exact_asset_object_path.rsplit(".", 1)[0],
                                                  asset_handle=editor_asset_handle, context=context, include_defaults=True))
        first = common.DataCalls(context, BY_ID, editor_asset_handle, prefix=PREFIX).query(common.DATA_TABLE_READ,
                                                                                           {"offset": 0, "limit": 1})
        return {**document, "RowSchema": _schema(first)}

    def diff(self, authored, extracted):
        return diff(self._canonical(authored), self._canonical(extracted), exact=False)

    def diff_exact(self, authored, extracted):
        return diff(self._canonical(authored), self._canonical(extracted), exact=True)

    def extract_with_defaults(self, *, target, asset_handle, context):
        return extract_with_defaults(common.require_target(target, PREFIX), self.exact_asset_class_path, asset_handle,
                                     context)

    def diff_with_defaults(self, authored, extracted, live_defaults, exact):
        """Verify / Diff: a row's unwritten fields must hold the row struct's defaults."""
        return diff(self._canonical(authored), self._canonical(extracted), exact=exact, defaults=live_defaults)


def _meta(class_path, profile_id):
    rows = [("row_struct", "/Body/RowStruct", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL)]
    if _composite(class_path):
        rows.append(("parent_tables", "/Body/ParentTables", IdentityPolicy.POSITIONAL, OrderPolicy.PRESERVE,
                     ComparisonPolicy.ORDERED))
    rows.append(("rows", "/Body/Rows", IdentityPolicy.STABLE_KEY, OrderPolicy.PRESERVE, ComparisonPolicy.ORDERED))
    regions = tuple(RegionMeta(f"data_table.{name}.v1", path, RegionOwnership.MANAGED, identity, order, DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE,
                               DeletePolicy.EXPLICIT_ONLY, comparison)
                    for name, path, identity, order, comparison in rows)
    return ProfileMeta(profile_id, common.ROUTE, class_path, regions, REQUIREMENTS)


def _registration(class_path, profile_id):
    lifecycle = MIRROR_LIFECYCLE if class_path == MIRROR_DATA_TABLE_CLASS else LIFECYCLE
    return RuntimeProfileRegistration(_meta(class_path, profile_id), lambda: DataTableRuntimeProfile(class_path, profile_id),
                                      RuntimeTargetPolicy.CREATE_OR_UPDATE, lifecycle)


DATA_TABLE_RUNTIME_REGISTRATION = _registration(DATA_TABLE_CLASS, "profile.data_table.v1")
COMPOSITE_DATA_TABLE_RUNTIME_REGISTRATION = _registration(COMPOSITE_DATA_TABLE_CLASS, "profile.composite_data_table.v1")
MIRROR_DATA_TABLE_RUNTIME_REGISTRATION = _registration(MIRROR_DATA_TABLE_CLASS, "profile.mirror_data_table.v1")

__all__ = ("DATA_TABLE_RUNTIME_REGISTRATION", "COMPOSITE_DATA_TABLE_RUNTIME_REGISTRATION",
           "MIRROR_DATA_TABLE_RUNTIME_REGISTRATION", "DataTableRuntimeProfile", "row_ops")
