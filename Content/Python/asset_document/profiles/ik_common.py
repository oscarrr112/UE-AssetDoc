"""Shared pieces of the IK Rig, IK Retargeter and Control Rig Profiles (route "rig_profile", provider "rig", AssetDocumentRig).

Transport over the ``rig.*`` primitives, paged record reads, the Properties region (viewport / preview Details
through the generic Editor property admission, Body-owned names refused), value comparison and a generic
document diff for the sparse, name-keyed regions of both documents.
"""
from __future__ import annotations

from asset_document.diff.model import Difference, DiffKind
from asset_document.dto import PrimitiveRequirement
from asset_document.profiles import data_table_common as common
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements

ROUTE = "rig_profile"
PROVIDER = "rig"
IKRIG_CLASS = "/Script/IKRig.IKRigDefinition"
RETARGETER_CLASS = "/Script/IKRig.IKRetargeter"
CONTROLRIG_CLASS = "/Script/ControlRigDeveloper.ControlRigBlueprint"
SKELETAL_MESH_CLASS = "/Script/Engine.SkeletalMesh"
IKRIG_READ = "rig.ikrig.read.v1"
IKRIG_EDIT = "rig.ikrig.edit.v1"
RETARGETER_READ = "rig.ikretargeter.read.v1"
RETARGETER_EDIT = "rig.ikretargeter.edit.v1"
SUGGEST = "rig.suggest.v1"
STATUS = "rig.status.v1"
CATALOG = "rig.catalog.v1"
CONTROLRIG_READ = "rig.controlrig.read.v1"
CONTROLRIG_CATALOG = "rig.controlrig.catalog.v1"
CONTROLRIG_EDIT = "rig.controlrig.edit.v1"
CONTROLRIG_COMPILE = "rig.controlrig.compile.v1"


def _requirements(*primitives):
    return tuple(PrimitiveRequirement(item, 1, ROUTE) for item in primitives)


LIFECYCLE = generic_factory_lifecycle(ROUTE)
# One requirement set for the IK Rig, IK Retargeter and Control Rig Profiles (one route).
REQUIREMENTS = _requirements(common.CLASS_RESOLVE, common.RESOLVE, common.REFERENCE_RESOLVE, IKRIG_READ, IKRIG_EDIT, RETARGETER_READ,
                             RETARGETER_EDIT, STATUS, CATALOG, SUGGEST, CONTROLRIG_READ,
                             CONTROLRIG_CATALOG, CONTROLRIG_EDIT,
                             CONTROLRIG_COMPILE) + lifecycle_requirements(LIFECYCLE)
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}


def error(path, message, code):
    return common.error(path, message, code)


escape = common.escape


class RigCalls(common.DataCalls):
    """Calls to the "rig" provider against one IK Rig or retargeter (editor handles handed off)."""

    def __init__(self, context, read, mutable=None, prefix="IKRig"):
        self.context = context
        self.by_id = BY_ID
        self.prefix = prefix
        self.read = context.handoff(source_handle=read, target_provider_id=PROVIDER, access="read_only")
        self.write = (context.handoff(source_handle=mutable, target_provider_id=PROVIDER, access="mutable")
                      if mutable is not None else None)


# Not-found name candidates (data_table_common, shared by every profile).
close_names = common.close_names
describe_candidates = common.describe_candidates


def read_records(calls, primitive, detail="document"):
    """Every record of a paged rig read (header first)."""
    records = []
    offset = 0
    while True:
        values = {"offset": offset}
        if detail != "document":
            values["detail"] = detail
        page = calls.query(primitive, values)
        records += page.get("records", [])
        offset = page.get("next_offset", -1)
        if type(offset) is not int or offset < 0:
            break
    if not records or records[0].get("kind") != "header":
        raise error("/Body", f"{primitive} returned no header.", f"{calls.prefix}.InvalidPrimitiveResult")
    return records


def object_value(value, path, what, prefix):
    if type(value) is not dict:
        raise error(path, f"{what} must be an object.", f"{prefix}.InvalidBody")
    return value


def check_keys(value, allowed, path, what, prefix):
    for key in value:
        if key not in allowed:
            raise error(f"{path}/{escape(key)}", f"Unknown {what} key '{key}'. Keys: {', '.join(allowed)}.",
                        f"{prefix}.UnknownKey")


def name_value(value, path, what, prefix, allow_empty=False):
    if type(value) is not str or (not value and not allow_empty):
        raise error(path, f"{what} is a name string.", f"{prefix}.InvalidBody")
    return value


def path_or_null(value, path, what, prefix):
    if value is not None and (type(value) is not str or not value.startswith("/")):
        raise error(path, f"{what} is an object path (/Game/...) or null.", f"{prefix}.InvalidBody")
    return value


# ---------------------------------------------------------------------------
# Diff: sparse, name-keyed regions (omitted field = its default, omitted entry = absent)
# ---------------------------------------------------------------------------
def value_diff(path, authored, live, exact, changes, defaults=None):
    """Differences of one authored value from the live one. Objects compare key by key (a key missing on one side
    takes its value from ``defaults`` when it has one there, else it is an add / remove); everything else compares
    as a value (numbers as float32 unless exact)."""
    if type(authored) is dict and type(live) is dict:
        defaults = defaults or {}
        for key in list(authored) + [key for key in live if key not in authored]:
            where = f"{path}/{escape(key)}"
            if key in authored and key in live:
                value_diff(where, authored[key], live[key], exact, changes)
            elif key in defaults:
                value_diff(where, authored.get(key, defaults[key]), live.get(key, defaults[key]), exact, changes)
            elif key in authored:
                changes.append(Difference(where, DiffKind.ADD, True, authored[key], False, None))
            else:
                changes.append(Difference(where, DiffKind.REMOVE, False, None, True, live[key]))
        return
    if not common.same(authored, live, exact):
        changes.append(Difference(path, DiffKind.REPLACE, True, authored, True, live))


_ABSENT = object()


def sparse_diff(path, authored, live, defaults, exact, changes):
    """Differences of a settings object written sparse over ``defaults`` from the live one, read sparse over the
    same defaults: a field missing on a side has its default there, so an authored value equal to its default
    matches a live read that omits it (nested objects compare field by field the same way). Without a default a
    missing field is an add / remove, as in ``value_diff``. Strings compare ignoring case: settings strings are
    names (a bone written "root" reads back as the name table's "Root"), enum values and object paths, all
    case-insensitive in the engine."""
    defaults = defaults if type(defaults) is dict else {}
    for key in list(authored) + [key for key in live if key not in authored]:
        where = f"{path}/{escape(key)}"
        mine = authored[key] if key in authored else defaults.get(key, _ABSENT)
        theirs = live[key] if key in live else defaults.get(key, _ABSENT)
        if mine is _ABSENT:
            changes.append(Difference(where, DiffKind.REMOVE, False, None, True, theirs))
        elif theirs is _ABSENT:
            changes.append(Difference(where, DiffKind.ADD, True, mine, False, None))
        elif type(mine) is dict and type(theirs) is dict:
            sparse_diff(where, mine, theirs, defaults.get(key), exact, changes)
        elif type(mine) is str and type(theirs) is str:
            if mine.casefold() != theirs.casefold():
                changes.append(Difference(where, DiffKind.REPLACE, True, mine, True, theirs))
        elif not common.same(mine, theirs, exact):
            changes.append(Difference(where, DiffKind.REPLACE, True, mine, True, theirs))


def sparse_map_diff(path, authored, live, row_defaults, exact, changes, missing_is_default=True):
    """Name-keyed rows of sparse settings, each row as ``sparse_diff`` over ``row_defaults``. A row missing on a
    side is all defaults there (a read omits rows at their defaults) unless ``missing_is_default`` is False (the
    rows themselves are state, e.g. a solver's connected goals): then it is an add / remove."""
    for name in list(authored) + [name for name in live if name not in authored]:
        if not missing_is_default and (name not in authored or name not in live):
            changes.append(Difference(f"{path}/{escape(name)}", DiffKind.ADD, True, authored[name], False, None)
                           if name in authored else
                           Difference(f"{path}/{escape(name)}", DiffKind.REMOVE, False, None, True, live[name]))
            continue
        mine, theirs = authored.get(name, {}), live.get(name, {})
        if type(mine) is dict and type(theirs) is dict:
            sparse_diff(f"{path}/{escape(name)}", mine, theirs, row_defaults, exact, changes)
        else:
            value_diff(f"{path}/{escape(name)}", mine, theirs, exact, changes)


def map_diff(path, authored, live, exact, changes, entry_defaults, renamed_key="RenamedFrom"):
    """A name-keyed region (complete state): entries by name, each a sparse object with ``entry_defaults``.
    ``RenamedFrom`` is an instruction, not state: it is not compared."""
    for name in list(authored) + [name for name in live if name not in authored]:
        where = f"{path}/{escape(name)}"
        if name not in live:
            changes.append(Difference(where, DiffKind.ADD, True, authored[name], False, None))
        elif name not in authored:
            changes.append(Difference(where, DiffKind.REMOVE, False, None, True, live[name]))
        else:
            entry = {key: value for key, value in authored[name].items() if key != renamed_key}
            value_diff(where, entry, live[name], exact, changes, entry_defaults)


# ---------------------------------------------------------------------------
# Properties (viewport / preview Details)
# ---------------------------------------------------------------------------
class RigProperties(common.PropertiesRegion):
    """Top-level Properties of an IK asset: the Details the editors show outside the Body regions. Body-owned and
    stored-but-not-authored names are refused with the region to use."""

    def __init__(self, class_path, body_owned, not_authored, prefix, refused=None):
        super().__init__(class_path, body_owned, prefix)
        self.not_authored = tuple(not_authored)
        self.refused = dict(refused or {})  # name -> (code, message): not authored, with its own code

    def check_owned(self, properties):
        for name in sorted(properties):
            if name in self.refused:
                code, message = self.refused[name]
                raise error(f"/Properties/{escape(name)}", message, code)
            if name in self.not_authored or name.endswith("_DEPRECATED"):
                raise error(f"/Properties/{escape(name)}", f"'{name}' is not authored by this Profile "
                            "(editor state, derived data or a deprecated property); leave it out.",
                            f"{self.prefix}.NotAuthoredProperty")
            if name in self.body_owned:
                raise error(f"/Properties/{escape(name)}", f"'{name}' is authored as {self.body_owned[name]}.",
                            f"{self.prefix}.NotAuthoredProperty")
        super().check_owned(properties)

    def extract(self, read, context, include_defaults=True):
        return {name: value for name, value in super().extract(read, context, include_defaults).items()
                if name not in self.not_authored and not name.endswith("_DEPRECATED")}


def existing(context, document, class_path, prefix):
    """Read handle of the document's target, or None when it does not exist yet."""
    target = document["Target"]
    result = common.invoke(context, BY_ID, common.RESOLVE,
                           {"path": f"{target}.{target.rsplit('/', 1)[-1]}", "expected_class": class_path},
                           prefix=prefix)
    value = result.value if type(result.value) is dict else {}
    if value.get("exists") is not True:
        return None
    return next((group.handles[0] for group in result.handle_results
                 if group.name == "asset_read" and group.handles), None)


def status(calls):
    """The editors' processor initialization of the asset ({Status, Messages})."""
    return calls.query(STATUS).get("compile", {})


def handle(read):
    return (NamedOpaqueHandleArgument("asset", read),)


__all__ = ("BY_ID", "CATALOG", "CONTROLRIG_CATALOG", "CONTROLRIG_CLASS", "CONTROLRIG_COMPILE", "CONTROLRIG_EDIT",
           "CONTROLRIG_READ", "IKRIG_CLASS", "IKRIG_EDIT", "IKRIG_READ", "LIFECYCLE", "PROVIDER", "REQUIREMENTS",
           "RETARGETER_CLASS", "RETARGETER_READ", "ROUTE", "RigCalls", "RigProperties", "SKELETAL_MESH_CLASS",
           "STATUS", "check_keys", "close_names", "describe_candidates", "error", "escape", "existing", "map_diff", "name_value",
           "object_value", "path_or_null", "read_records", "sparse_diff", "sparse_map_diff", "status", "value_diff")
