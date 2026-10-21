"""Shared pieces of the UserDefinedStruct / UserDefinedEnum Profiles.

Transport over ``data.user_struct.*`` / ``data.user_enum.*`` (provider "data", AssetDocumentData),
op chunking with the primitive result kept (it lists the dependents the editor updated), and the
warnings an apply reports for them.  See docs/architecture/asset-document-user-defined-types-matrix.md.
"""
from __future__ import annotations

from asset_document.diagnostics import DiagnosticSeverity, RuntimeDiagnostic
from asset_document.profiles import data_table_common as common
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument

USER_STRUCT_READ = "data.user_struct.read.v1"
USER_STRUCT_EDIT = "data.user_struct.edit.v1"
USER_ENUM_READ = "data.user_enum.read.v1"
USER_ENUM_EDIT = "data.user_enum.edit.v1"
_NAME_LIMIT = 100


def validate_label(value, path, code, what):
    """A member / enumerator name as the editors accept it: one line, no outer spaces."""
    if type(value) is not str or not value or value.strip() != value or len(value) > _NAME_LIMIT \
            or "\n" in value or "\r" in value:
        raise common.error(path, f"A {what} name is 1 to {_NAME_LIMIT} characters on one line without leading or "
                                 f"trailing spaces (got {value!r}).", code)
    return value


def covers(authored, live, exact=False):
    """Authored value equality where an authored struct value may name a subset of the fields."""
    if type(authored) is dict and type(live) is dict:
        return all(key in live and covers(value, live[key], exact) for key, value in authored.items())
    return common.same(authored, live, exact)


def map_op_path(primitive_path, op_paths, fields, base="/Body"):
    """/ops/<n>/<field>/... -> the document path of op n (+ the document field of a primitive field)."""
    parts = [part for part in (primitive_path or "").split("/") if part]
    if len(parts) >= 2 and parts[0] == "ops" and parts[1].isdigit() and int(parts[1]) < len(op_paths):
        path = op_paths[int(parts[1])]
        rest = parts[2:]
        if rest and rest[0] in fields:
            path += fields[rest[0]]
            if rest[0] == "metadata" and len(rest) > 1:
                path += f"/{rest[1]}"
            if rest[0] == "names" and len(rest) > 1 and rest[1].isdigit():
                path += f"/{rest[1]}"
        return path
    return base


class TypeCalls(common.DataCalls):
    """Calls against one User Defined Struct / Enum; edits keep each call's result."""

    def edit_all(self, primitive, ops, fields, base):
        """Runs [(op, document path)] in node-bounded chunks; returns (changed records, dependents rows)."""
        changed, dependents = [], []
        for chunk in common.chunk_ops(ops):
            op_paths = [path for _, path in chunk]

            def where(primitive_path, op_paths=op_paths):
                return map_op_path(primitive_path, op_paths, fields, base)
            outcome = common.invoke(self.context, self.by_id, primitive, {"schema_version": 1, "ops": [op for op, _ in chunk]},
                                    (NamedOpaqueHandleArgument("asset", self.write),), paths=where, prefix=self.prefix)
            changed.extend(outcome.changed_set)
            value = outcome.value if type(outcome.value) is dict else {}
            dependents.extend(row for row in value.get("dependents", []) if type(row) is dict)
        return changed, dependents


def warning(path, code, message):
    return RuntimeDiagnostic(path=path, code=code, message=message, severity=DiagnosticSeverity.WARNING)


def dependents_warnings(rows, path, prefix):
    """The rule's report: every package the editor updated for the change (see the matrix doc)."""
    unique = {}
    for row in rows:
        if type(row.get("package")) is str:
            unique[row["package"]] = row
    if not unique:
        return []
    kept = sorted(name for name, row in unique.items() if row.get("kept_clean"))
    dirty = sorted(name for name, row in unique.items() if not row.get("kept_clean"))
    parts = []
    if kept:
        parts.append("updated in memory and left saved (their saved files load to the same state): "
                     + ", ".join(f"{name} ({unique[name].get('kind', 'Other')})" for name in kept))
    if dirty:
        parts.append("updated in memory and now unsaved, as in the editor (save them to keep the update): "
                     + ", ".join(f"{name} ({unique[name].get('kind', 'Other')})" for name in dirty))
    warnings = [warning(path, f"{prefix}.DependentsUpdated",
                        "The editor updated the loaded assets that use this type: " + "; ".join(parts) + ".")]
    failed = sorted(name for name, row in unique.items() if row.get("compile_failed"))
    if failed:
        warnings.append(warning(path, f"{prefix}.DependentCompileFailed",
                                "These Blueprints use this type and did not compile after the change (fix them, e.g. the "
                                "nodes that use a removed member, then compile): " + ", ".join(failed) + "."))
    return warnings


def describe_dependents(rows):
    return [{"Package": row.get("package", ""), "Kind": row.get("kind", ""), "Unsaved": bool(row.get("dirty"))}
            for row in rows if type(row) is dict]


__all__ = ("TypeCalls", "USER_ENUM_EDIT", "USER_ENUM_READ", "USER_STRUCT_EDIT", "USER_STRUCT_READ", "covers",
           "dependents_warnings", "describe_dependents", "map_op_path", "validate_label", "warning")
