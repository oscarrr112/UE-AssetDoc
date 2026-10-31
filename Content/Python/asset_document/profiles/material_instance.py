# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Registered MaterialInstanceConstant Profile (profile.material_instance_constant.v1).

Document (docs/architecture/asset-document-material-instance-matrix.md)::

    "Body": {
      "Parent": "/Game/Materials/M_Base.M_Base",          # null: no parent
      "Parameters": {                                       # overridden parameters, by name
        "Roughness": 0.4,                                   # scalar
        "Gradient": {"Curve": "/Game/FX/C_Fire.C_Fire", "Row": 3},   # curve atlas scalar (the curve, its row)
        "BaseColor": [1, 0, 0, 1],                          # vector [R, G, B, A] (also {"R","G","B","A"})
        "Albedo": "/Game/Textures/T_Rock.T_Rock",           # texture (or "" = override with None)
        "UseDetail": true,                                  # static switch
        "Channels": {"R": true, "G": false, "B": false, "A": false},   # static component mask
        "Metallic": null                                    # clear the override (revert to the parent)
      },
      "LayerParameters": {"1": {"Tiling": 4}},              # layered materials: by layer index
      "BlendParameters": {"1": {"Height": 0.5}},
      "RefractionDepthBias": 0.0,                           # the Details panel's Refraction Depth Bias
      "MaterialLayers": {...}                               # read-only: the instance's own layer stack
    },
    "Properties": {"PhysMaterial": ..., "BasePropertyOverrides": {...}, ...}

* ``Parameters`` (and the layer / blend maps) are sparse: a parameter that is not listed keeps its
  state; a value overrides it (the Material Instance Editor's override checkbox + value); ``null``
  clears the override.  ``null`` on a parameter that is not overridden is a no-op.  Extract lists
  the overridden parameters only; Inspect lists every parameter the parent chain exposes
  (``ParameterSchema``: kind, group, default, current value, overridden).
* A parameter is keyed by its name.  Where the parent chain exposes one name with several kinds (the
  editor lists each), each of them is keyed ``<name>@<kind>`` (``"Mask@texture"``).  A plain name that
  stands for several kinds is the one kind its value's form fits (a number: scalar, a bool: static
  switch, a path: texture, ...); where the form still fits several of them it fails with
  ``MaterialInstance.AmbiguousParameter``.  ``<name>@<kind>`` is also accepted where the name is not shared.
* A Curve Atlas Row Parameter's value is ``{"Curve": <curve path>, "Row": <its row in the atlas>}`` (the curve the
  Material Instance Editor's picker chose; the row is derived from it). Set it by ``Curve``, or by its row
  (``{"Row": n}`` or a bare number, the older form; a row the atlas does not have is stored with no curve, as
  ``{"Curve": "", "Row": n}`` is); a curve outside the parameter's atlas is refused.
* Other value kinds: double vector [X, Y, Z, W]; texture collection / runtime virtual texture /
  sparse volume texture / material parameter collection: object path; font {"Font": path, "Page": n}.
* ``Parent`` change is the editor's: overrides whose parameter (name and type) the new parent also
  has are kept, the others are dropped (as the Material Instance Editor's next edit does).  Overrides
  of parameters the parent no longer exposes (stale) are not in the document: Extract warns
  ``MaterialInstance.StaleOverrides`` naming them, and an apply that writes overrides drops them as
  the editor's next edit does, with the warning ``MaterialInstance.OverridesDropped``.
* Create: ``Parent`` is the New Material Instance factory's InitialParent (factory context).
* Static switches, component masks and the layer stack change the shader permutation: the edit
  runs UMaterialInstance::UpdateStaticPermutation, which starts an asynchronous shader compile;
  the apply does not wait for it (as the editor does not).
* ``RefractionDepthBias`` is the instance's Refraction Depth Bias (the Material Instance Editor stores
  it as a scalar override entry that no parent parameter exposes); ``null`` removes it (the parent's
  value).  Extract lists it when the instance has one.
* Details-panel properties go through the generic property admission (``Properties``); the raw
  parameter arrays and ``Parent`` are Body-owned.

Writes are covered by Core's package snapshot (existing target) or the factory rollback (created
target); preflight therefore returns ``SNAPSHOT_COVERED``.
"""
from __future__ import annotations

import dataclasses

from asset_document.diagnostics import (
    AssetDocumentContractError, DiagnosticSeverity, RuntimeDiagnostic, read_warning, with_diagnostics,
)
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

MATERIAL_INSTANCE_CONSTANT_CLASS = "/Script/Engine.MaterialInstanceConstant"
MATERIAL_INTERFACE_CLASS = "/Script/Engine.MaterialInterface"
PROFILE_ID = "profile.material_instance_constant.v1"
READ = "data.material_instance.read.v1"
EDIT = "data.material_instance.edit.v1"
PREFIX = "MaterialInstance"
BODY_KEYS = ("Parent", "Parameters", "LayerParameters", "BlendParameters", "RefractionDepthBias", "MaterialLayers")
# The New Material Instance factory's InitialParent is the factory context; a create without Parent
# makes a parentless instance, as the editor's New menu does.
LIFECYCLE = dataclasses.replace(generic_factory_lifecycle(common.ROUTE, ("factory_context",)),
                                optional_create_context_handle_names=("factory_context",))
REQUIREMENTS = (common.requirements(common.CLASS_RESOLVE, common.RESOLVE, common.REFERENCE_RESOLVE, READ, EDIT)
                + lifecycle_requirements(LIFECYCLE))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}
# Raw override arrays and the parent are authored in the Body, never as generic Properties.
BODY_OWNED = {
    "Parent": "Body.Parent",
    **{name: "Body.Parameters" for name in (
        "ScalarParameterValues", "VectorParameterValues", "DoubleVectorParameterValues", "TextureParameterValues",
        "TextureCollectionParameterValues", "ParameterCollectionParameterValues",
        "RuntimeVirtualTextureParameterValues", "SparseVolumeTextureParameterValues", "FontParameterValues")},
}
# Association of each parameter map: (Body key, primitive association).
MAPS = (("Parameters", "global"), ("LayerParameters", "layer"), ("BlendParameters", "blend"))
_ASSOCIATION_KEY = {association: key for key, association in MAPS}
_CHANNELS = ("R", "G", "B", "A")
# Parameter kinds of the read and edit primitives (EMaterialParameterType).
KINDS = ("scalar", "vector", "double_vector", "texture", "texture_collection", "font", "runtime_virtual_texture",
         "sparse_volume_texture", "static_switch", "parameter_collection", "static_component_mask")
_MISSING = object()
# A curve atlas scalar's value keys.
ATLAS_KEYS = frozenset(("Curve", "Row"))


def _number(value):
    return type(value) in (int, float)


def value_kinds(value):
    """The parameter kinds a canonical value's form can set (every kind for null: a clear)."""
    if value is None:
        return KINDS
    if type(value) is bool:
        return ("static_switch",)
    if _number(value):
        return ("scalar",)
    if type(value) is str:
        return ("texture", "texture_collection", "runtime_virtual_texture", "sparse_volume_texture", "parameter_collection")
    if type(value) is list:
        return ("vector", "double_vector")
    if type(value) is dict:
        if value and set(value) <= ATLAS_KEYS:
            return ("scalar",)
        return ("font",) if "Font" in value else ("static_component_mask",)
    return ()


def _is_atlas(value):
    return type(value) is dict and bool(value) and set(value) <= ATLAS_KEYS


def same_value(value, have, exact=False):
    """Authored against live: a curve atlas value compares the fields the authored one lists (a bare number is its
    row); other values as numbers / structures."""
    if _is_atlas(have):
        if _number(value):
            return common.same(value, have.get("Row"), exact)
        if _is_atlas(value):
            return all(common.same(item, have.get(key), exact) for key, item in value.items())
        return False
    return common.same(value, have, exact)


# ---------------------------------------------------------------------------
# Document shape
# ---------------------------------------------------------------------------
def canonical_value(value, path):
    """Document value -> canonical form (vector objects become [R, G, B, A]); the kind is checked
    against the parameter by the edit primitive."""
    if value is None or type(value) in (bool, int, float, str):
        return value
    if type(value) is list:
        if len(value) != 4 or not all(_number(item) for item in value):
            raise common.error(path, "A vector value is [R, G, B, A] (or [X, Y, Z, W] for a double vector): four numbers.",
                               "MaterialInstance.InvalidValue")
        return list(value)
    if type(value) is dict:
        if set(value) == set(_CHANNELS) and all(_number(value[key]) for key in _CHANNELS):
            return [value[key] for key in _CHANNELS]
        if set(value) == set(_CHANNELS) and all(type(value[key]) is bool for key in _CHANNELS):
            return {key: value[key] for key in _CHANNELS}
        if _is_atlas(value) and type(value.get("Curve", "")) is str and (
                "Row" not in value or (_number(value["Row"]) and value["Row"] >= 0 and float(value["Row"]).is_integer())):
            return dict(value)
        if "Font" in value and set(value) <= {"Font", "Page"} and type(value["Font"]) is str \
                and type(value.get("Page", 0)) is int and value.get("Page", 0) >= 0:
            return {"Font": value["Font"], "Page": value.get("Page", 0)}
        raise common.error(path, "An object value is a static component mask {\"R\": bool, \"G\": bool, \"B\": bool, "
                           "\"A\": bool}, a color {\"R\", \"G\", \"B\", \"A\"}, a font {\"Font\": path, \"Page\": n} or a "
                           "curve atlas value {\"Curve\": path} / {\"Row\": n}.",
                           "MaterialInstance.InvalidValue")
    raise common.error(path, "A parameter value is a number, bool, object path, [four numbers], an object, or null "
                       "(clear the override).", "MaterialInstance.InvalidValue")


def _parameter_map(value, path):
    if type(value) is not dict:
        raise common.error(path, "Parameters is an object of parameter name -> value (null clears the override).",
                           "MaterialInstance.InvalidParameters")
    out = {}
    for name, item in value.items():
        if not name.strip() or name.strip() != name:
            raise common.error(f"{path}/{common.escape(name)}", f"'{name}' is not a parameter name.",
                               "MaterialInstance.UnknownParameter")
        out[name] = canonical_value(item, f"{path}/{common.escape(name)}")
    return out


def canonical_body(body):
    out = {}
    if "Parent" in body:
        parent = body["Parent"]
        if parent == "":
            parent = None
        if parent is not None and (type(parent) is not str or not parent.startswith("/") or "." not in parent):
            raise common.error("/Body/Parent", "Parent is the object path of a Material or Material Instance, e.g. "
                               "/Game/Materials/M_Base.M_Base, or null.", "MaterialInstance.InvalidParent")
        out["Parent"] = parent
    if "Parameters" in body:
        out["Parameters"] = _parameter_map(body["Parameters"], "/Body/Parameters")
    for key in ("LayerParameters", "BlendParameters"):
        if key in body:
            layers = body[key]
            if type(layers) is not dict:
                raise common.error(f"/Body/{key}", f"{key} is an object of layer index (\"0\", \"1\", ...) -> "
                                   "{parameter name -> value}.", "MaterialInstance.InvalidParameters")
            canonical = {}
            for index, values in layers.items():
                if not index.isdigit() or str(int(index)) != index:
                    raise common.error(f"/Body/{key}/{common.escape(index)}", f"'{index}' is not a layer index "
                                       "(\"0\", \"1\", ...).", "MaterialInstance.InvalidParameters")
                canonical[index] = _parameter_map(values, f"/Body/{key}/{index}")
            out[key] = canonical
    if "RefractionDepthBias" in body:
        value = body["RefractionDepthBias"]
        if value is not None and not _number(value):
            raise common.error("/Body/RefractionDepthBias", "RefractionDepthBias is a number, or null for the parent's value.",
                               "MaterialInstance.InvalidValue")
        out["RefractionDepthBias"] = value
    if "MaterialLayers" in body:
        if type(body["MaterialLayers"]) is not dict:
            raise common.error("/Body/MaterialLayers", "MaterialLayers is read-only; give the extracted object or omit it.",
                               "MaterialInstance.MaterialLayersReadOnly")
        out["MaterialLayers"] = body["MaterialLayers"]
    return out


def canonicalize(document):
    common.validate_header(document, (MATERIAL_INSTANCE_CONSTANT_CLASS,), PREFIX, BODY_KEYS)
    canonical_body(document.get("Body", {}))
    return common.snapshot(document)


def authored_entries(body):
    """(association, index, key, value, document path) of every authored parameter (key: see resolve)."""
    entries = []
    for key, association in MAPS:
        if key not in body:
            continue
        if association == "global":
            groups = ((-1, body[key], "/Body/Parameters"),)
        else:
            groups = tuple((int(index), values, f"/Body/{key}/{index}") for index, values in sorted(
                body[key].items(), key=lambda item: int(item[0])))
        for index, values, base in groups:
            for name in sorted(values):
                entries.append((association, index, name, values[name], f"{base}/{common.escape(name)}"))
    return entries


# ---------------------------------------------------------------------------
# Live state
# ---------------------------------------------------------------------------
def read_live(calls):
    first, parameters = calls.query_all(READ, "parameters")
    return {"parent": first.get("parent", "") or None, "base_material": first.get("base_material", ""),
            "has_static_permutation": first.get("has_static_permutation", False),
            "stale": list(first.get("stale_overrides", [])), "layers": first.get("layers"),
            "refraction_depth_bias": first.get("refraction_depth_bias"),
            "parameters": parameters}


def _key(association, index, name):
    return (association, index if association != "global" else -1, name)


def overrides(live):
    """{(association, index, name, kind): value} of the overridden parameters."""
    return {(*_key(row["association"], row["index"], row["name"]), row["kind"]): row["value"]
            for row in live["parameters"] if row["overridden"]}


def exposed_kinds(live):
    """{(association, index, name): [kinds]} of the parameters the parent chain exposes."""
    kinds = {}
    for row in live["parameters"]:
        listed = kinds.setdefault(_key(row["association"], row["index"], row["name"]), [])
        if row["kind"] not in listed:
            listed.append(row["kind"])
    return kinds


def document_key(name, kind, kinds):
    """The key of a parameter in its map: its name, or ``<name>@<kind>`` when the name has several kinds."""
    return f"{name}@{kind}" if len(kinds) > 1 else name


def split_key(key):
    """(name, kind) of a ``<name>@<kind>`` key, else (key, None)."""
    name, separator, kind = key.rpartition("@")
    return (name, kind) if separator and name and kind in KINDS else (key, None)


def resolve(association, index, key, exposed, path, strict=True, value=_MISSING):
    """(parameter name, kind or None) of an authored key: a name the parent exposes (kind None: its one kind), or a
    ``<name>@<kind>`` key. A plain name the parent exposes with several kinds is the one of them the authored
    ``value``'s form fits; ``strict``: one that is still ambiguous fails."""
    kinds = exposed.get(_key(association, index, key))
    if kinds is not None:
        if len(kinds) > 1 and value is not _MISSING:
            fitting = [kind for kind in kinds if kind in value_kinds(value)]
            if len(fitting) == 1:
                return key, fitting[0]
        if strict and len(kinds) > 1:
            choices = ", ".join(f"'{key}@{kind}'" for kind in kinds)
            raise common.error(path, f"The parent exposes several parameters named '{key}' ({', '.join(kinds)}), as the "
                               f"Material Instance Editor lists them; key the one to author as {choices}.",
                               "MaterialInstance.AmbiguousParameter")
        return key, None
    return split_key(key)


def _override(current, association, index, name, kind):
    """The current override of a parameter (kind None: of its one kind), or _MISSING."""
    base = _key(association, index, name)
    for (*where, have_kind), value in current.items():
        if tuple(where) == base and (kind is None or have_kind == kind):
            return value
    return _MISSING


def _layers_document(layers):
    return {"Layers": list(layers.get("layers", [])), "Blends": list(layers.get("blends", [])),
            "Names": list(layers.get("names", [])), "Visible": list(layers.get("visible", []))}


def _body(live):
    body = {"Parent": live["parent"], "Parameters": {}}
    maps = {"LayerParameters": {}, "BlendParameters": {}}
    exposed = exposed_kinds(live)
    for (association, index, name, kind), value in sorted(overrides(live).items(), key=lambda item: (
            ("global", "layer", "blend").index(item[0][0]), item[0][1], item[0][2], item[0][3])):
        key = document_key(name, kind, exposed.get(_key(association, index, name), [kind]))
        if association == "global":
            body["Parameters"][key] = value
        else:
            maps[_ASSOCIATION_KEY[association]].setdefault(str(index), {})[key] = value
    for key, value in maps.items():
        if value:
            body[key] = value
    if live.get("refraction_depth_bias") is not None:
        body["RefractionDepthBias"] = live["refraction_depth_bias"]
    if live["layers"] is not None:
        body["MaterialLayers"] = _layers_document(live["layers"])
    return body


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------
def parameter_ops(body, live):
    """Edit ops (with document paths) for the authored parameters; ``parent`` is the new parent path
    when it changes (then every authored value is sent: the new parent decides what is kept)."""
    parent_changed = "Parent" in body and body["Parent"] != live["parent"]
    current = overrides(live)
    exposed = exposed_kinds(live)
    ops = []
    for association, index, key, value, path in authored_entries(body):
        # After a parent change the new parent decides what a plain name stands for.
        name, kind = resolve(association, index, key, exposed, path, strict=not parent_changed,
                             value=_MISSING if parent_changed else value)
        have = _override(current, association, index, name, kind)
        op = {"name": name}
        if kind is not None:
            op["kind"] = kind
        if association != "global":
            op.update(association=association, index=index)
        if value is None:
            if have is not _MISSING or parent_changed:
                ops.append(({"op": "clear", **op}, path))
        elif parent_changed or have is _MISSING or not same_value(value, have, exact=False):
            ops.append(({"op": "set", **op, "value": value}, path))
    return parent_changed, ops


def refraction_changed(body, live):
    if "RefractionDepthBias" not in body:
        return False
    authored, current = body["RefractionDepthBias"], live.get("refraction_depth_bias")
    if authored is None or current is None:
        return authored is not current
    return not common.same(authored, current, exact=False)


def _paths(op_paths):
    def where(primitive_path):
        parts = [part for part in (primitive_path or "").split("/") if part]
        if len(parts) >= 2 and parts[0] == "ops" and parts[1].isdigit() and int(parts[1]) < len(op_paths):
            return op_paths[int(parts[1])]
        if parts and parts[0] == "parent":
            return "/Body/Parent"
        if parts and parts[0] == "refraction_depth_bias":
            return "/Body/RefractionDepthBias"
        return "/Body"
    return where


def apply(document, read, mutable, context):
    body = canonical_body(document.get("Body", {}))
    properties = document.get("Properties", {})
    region = _properties()
    region.check_owned(properties)
    calls = common.DataCalls(context, BY_ID, read, mutable, PREFIX)
    live = read_live(calls)
    if "MaterialLayers" in body:
        _check_layers(body["MaterialLayers"], live)
    parent_changed, ops = parameter_ops(body, live)
    refraction = refraction_changed(body, live)
    changes = []
    warnings = []
    if parent_changed or ops or refraction:
        values = {"schema_version": 1, "ops": [op for op, _ in ops]}
        if parent_changed:
            values["parent"] = body["Parent"] or ""
        if refraction:
            values["refraction_depth_bias"] = body["RefractionDepthBias"]
        outcome = common.invoke(context, BY_ID, EDIT, values, (NamedOpaqueHandleArgument("asset", calls.write),),
                                paths=_paths([path for _, path in ops]), prefix=PREFIX)
        changes.extend(outcome.changed_set)
        dropped = [item for item in ((outcome.value or {}).get("dropped") or []) if type(item) is str]
        if dropped:
            warnings.append(RuntimeDiagnostic(
                path="/Body/Parameters", code=f"{PREFIX}.OverridesDropped", severity=DiagnosticSeverity.WARNING,
                message=f"Overrides of parameters the parent does not expose were dropped, as the Material Instance "
                        f"Editor's next edit drops them: {', '.join(dropped)}."))
        _check_applied(body, read_live(calls))
    changes.extend(region.apply(properties, read, mutable, context))
    return with_diagnostics(tuple(changes), warnings)


def _stale_names(live):
    return [f"{row.get('name')} ({row.get('kind')})" for row in live.get("stale", []) if type(row) is dict]


def _check_layers(authored, live):
    current = _layers_document(live["layers"]) if live["layers"] is not None else None
    if authored != current:
        raise common.error("/Body/MaterialLayers", "The material layer stack is read-only in documents (edit it in the "
                           "Material Instance Editor's Layer Parameters panel); omit MaterialLayers or give the "
                           "extracted value. Layer and blend parameter values are authored in LayerParameters / "
                           "BlendParameters.", "MaterialInstance.MaterialLayersReadOnly")


def _check_applied(body, live):
    """After a parent change an authored override may not survive (the new parent's parameter has
    another type): fail at its path rather than report success."""
    current = overrides(live)
    exposed = exposed_kinds(live)
    for association, index, key, value, path in authored_entries(body):
        name, kind = resolve(association, index, key, exposed, path, strict=False, value=value)
        have = _override(current, association, index, name, kind)
        if value is not None and (have is _MISSING or not same_value(value, have, exact=False)):
            raise common.error(path, f"Parameter '{key}' did not take the value (the parent {live['parent']} exposes "
                               "it with another type?).", "MaterialInstance.InvalidValue")


def _properties():
    # The specular profile override has no CPF_Edit; the Material Instance Editor shows it next to its override flag.
    return common.PropertiesRegion(MATERIAL_INSTANCE_CONSTANT_CLASS, BODY_OWNED, PREFIX, ("ThumbnailInfo",),
                                   admitted=("SpecularProfileOverride",))


def create_target_context(document, context):
    body = canonical_body(document.get("Body", {}))
    if body.get("Parent") is None:
        return ()
    try:
        outcome = common.invoke(context, BY_ID, common.REFERENCE_RESOLVE,
                                {"expected_class": MATERIAL_INTERFACE_CLASS, "path": body["Parent"]},
                                path="/Body/Parent", prefix=PREFIX)
    except AssetDocumentContractError as failure:
        raise common.error("/Body/Parent", f"Parent '{body['Parent']}' is not a loadable Material or Material Instance "
                           f"({failure.diagnostic.message}).", "MaterialInstance.ParentNotFound") from None
    return (NamedOpaqueHandleArgument("factory_context", _single_handle(outcome, "asset")),)


# ---------------------------------------------------------------------------
# Extract / Diff / Inspect
# ---------------------------------------------------------------------------
def extract(target, read, context, include_defaults=True):
    live = read_live(common.DataCalls(context, BY_ID, read, prefix=PREFIX))
    if live["stale"]:
        read_warning("/Body/Parameters", f"{PREFIX}.StaleOverrides",
                     f"Overrides of parameters the parent no longer exposes are not in Parameters: "
                     f"{', '.join(_stale_names(live))}. The next apply that writes overrides drops them, as the Material "
                     "Instance Editor's next edit does (MaterialInstance.OverridesDropped).")
    return common.document(target, MATERIAL_INSTANCE_CONSTANT_CLASS, _properties().extract(read, context, include_defaults), _body(live))


def diff(authored, extracted, exact):
    changes = common.header_diff(authored, extracted)
    body = canonical_body(authored.get("Body", {}))
    live = extracted.get("Body", {})
    for key in ("Parent", "MaterialLayers"):
        if key in body and body[key] != live.get(key):
            changes.append(Difference(f"/Body/{key}", DiffKind.REPLACE, True, body[key], key in live, live.get(key)))
    if "RefractionDepthBias" in body:
        bias, current = body["RefractionDepthBias"], live.get("RefractionDepthBias")
        if (bias is None) != (current is None) or (bias is not None and not common.same(bias, current, exact)):
            changes.append(Difference("/Body/RefractionDepthBias", DiffKind.REPLACE, True, bias,
                                      "RefractionDepthBias" in live, current))
    for association, index, key, value, path in authored_entries(body):
        current = live.get(_ASSOCIATION_KEY[association], {})
        if association != "global":
            current = current.get(str(index), {})
        # "<name>@<kind>" for a name the extract lists plainly (it has one kind) is that parameter; a plain name the
        # extract lists as "<name>@<kind>" is the one listed kind its value's form fits.
        listed = key if key in current else _listed_key(key, value, current)
        present = listed in current
        if value is None:
            if present:
                changes.append(Difference(path, DiffKind.REMOVE, True, None, True, current[listed]))
            elif listed == key and any(split_key(other)[0] == key and split_key(other)[1] for other in current):
                # A plain clear of a name listed with several kinds: the apply cannot tell which one it clears.
                changes.append(Difference(path, DiffKind.REMOVE, True, None, False, None))
        elif not present or not same_value(value, current[listed], exact):
            changes.append(Difference(path, DiffKind.REPLACE, True, value, present, current.get(listed)))
    changes.extend(common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact))
    return tuple(changes)


def _listed_key(key, value, current):
    """The key under which an extracted parameter map lists an authored key that it does not list as is."""
    name, kind = split_key(key)
    if kind is not None:
        return name
    fitting = [listed for listed in current if split_key(listed)[0] == key and split_key(listed)[1] in value_kinds(value)]
    return fitting[0] if len(fitting) == 1 else key


def parameter_schema(live):
    rows = []
    exposed = exposed_kinds(live)
    for row in live["parameters"]:
        association = row["association"]
        key = common.escape(document_key(row["name"], row["kind"], exposed[_key(association, row["index"], row["name"])]))
        if association == "global":
            path = f"/Body/Parameters/{key}"
        else:
            path = f"/Body/{_ASSOCIATION_KEY[association]}/{row['index']}/{key}"
        # Compact: Value only when overridden (otherwise it is Default), empty fields left out,
        # DeclaredBy only for parameters declared outside the base material (material functions).
        item = {"Name": row["name"], "Kind": row["kind"], "Path": path, "Overridden": row["overridden"]}
        if row["overridden"]:
            item["Value"] = row["value"]
        item["Default"] = row["default"]
        if row.get("group"):
            item["Group"] = row["group"]
        if association != "global":
            item.update(Association=association, LayerIndex=row["index"])
        if row.get("description"):
            item["Description"] = row["description"]
        if row.get("asset") and row["asset"] != live.get("base_material"):
            item["DeclaredBy"] = row["asset"]
        if "ui_min" in row:
            item.update(UiMin=row["ui_min"], UiMax=row["ui_max"])
        rows.append(item)
    return rows


# ---------------------------------------------------------------------------
# Handler
# ---------------------------------------------------------------------------
class MaterialInstanceRuntimeProfile:
    profile_id = PROFILE_ID
    exact_asset_class_path = MATERIAL_INSTANCE_CONSTANT_CLASS
    runtime_entry_module = "asset_document/bootstrap.py"
    preflight_scratch = False

    def required_primitives(self):
        return REQUIREMENTS

    def inspect_profile(self):
        return {"Class": MATERIAL_INSTANCE_CONSTANT_CLASS, "BodySections": list(BODY_KEYS),
                "ReadOnlySections": ["MaterialLayers"], "CreateContext": "Body.Parent"}

    def create_template(self, target):
        return common.document(common.require_target(target, PREFIX), MATERIAL_INSTANCE_CONSTANT_CLASS, {},
                               {"Parent": "/Game/Materials/M_Base.M_Base", "Parameters": {}})

    def validate(self, document):
        canonicalize(document)

    def canonicalize(self, document):
        return canonicalize(document)

    def create_target_context(self, document, context):
        return create_target_context(canonicalize(document), context)

    def preflight(self, document, scratch_handle, context):
        canonical = canonicalize(document)
        _properties().validate(canonical.get("Properties", {}), context)
        return SNAPSHOT_COVERED

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        return apply(canonicalize(document), read_asset_handle, mutable_asset_handle, context)

    def repair(self, document, asset_handle, context):
        canonicalize(document)
        return ()

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        return extract(common.require_target(target, PREFIX), asset_handle, context, include_defaults)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != MATERIAL_INSTANCE_CONSTANT_CLASS or editor_asset_handle is None:
            raise common.error("/Inspect", "MaterialInstance inspection reads a live material instance.",
                               "MaterialInstance.InspectNotAdmitted")
        # The document as extract writes it (Properties that differ from the defaults, structs with only their
        # changed members): ParameterSchema below lists every parameter.
        document = canonicalize(self.extract(target=exact_asset_object_path.rsplit(".", 1)[0],
                                             asset_handle=editor_asset_handle, context=context, include_defaults=False))
        live = read_live(common.DataCalls(context, BY_ID, editor_asset_handle, prefix=PREFIX))
        inspected = {**document, "BaseMaterial": live["base_material"] or None,
                     "HasStaticPermutation": live["has_static_permutation"], "ParameterSchema": parameter_schema(live)}
        if live["stale"]:
            inspected["StaleOverrides"] = [
                {"Name": row["name"], "Kind": row["kind"], "Association": row["association"], "LayerIndex": row["index"]}
                for row in live["stale"]]
        return inspected

    def diff(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=False)

    def diff_exact(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=True)


def create_material_instance_runtime_handler():
    return MaterialInstanceRuntimeProfile()


def _meta():
    regions = tuple(
        RegionMeta(f"material_instance.{name}.v1", f"/Body/{key}", RegionOwnership.MANAGED, identity, OrderPolicy.CANONICAL,
                   DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE, DeletePolicy.EXPLICIT_ONLY, ComparisonPolicy.CANONICAL)
        for name, key, identity in (("parent", "Parent", IdentityPolicy.POSITIONAL),
                                    ("parameters", "Parameters", IdentityPolicy.STABLE_KEY),
                                    ("layer_parameters", "LayerParameters", IdentityPolicy.STABLE_KEY),
                                    ("blend_parameters", "BlendParameters", IdentityPolicy.STABLE_KEY),
                                    ("refraction_depth_bias", "RefractionDepthBias", IdentityPolicy.POSITIONAL),
                                    ("material_layers", "MaterialLayers", IdentityPolicy.POSITIONAL)))
    return ProfileMeta(PROFILE_ID, common.ROUTE, MATERIAL_INSTANCE_CONSTANT_CLASS, regions, REQUIREMENTS)


MATERIAL_INSTANCE_RUNTIME_REGISTRATION = RuntimeProfileRegistration(
    _meta(), create_material_instance_runtime_handler, RuntimeTargetPolicy.CREATE_OR_UPDATE, LIFECYCLE)

__all__ = ("MATERIAL_INSTANCE_RUNTIME_REGISTRATION", "MaterialInstanceRuntimeProfile", "MATERIAL_INSTANCE_CONSTANT_CLASS",
           "canonical_body", "parameter_ops", "diff")
