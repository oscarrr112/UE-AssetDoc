# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Registered MaterialInstanceConstant Profile (profile.material_instance_constant.v1).

Document (docs/architecture/asset-document-material-instance-matrix.md)::

    "Body": {
      "Parent": "/Game/Materials/M_Base.M_Base",          # null: no parent
      "Parameters": {                                       # overridden parameters, by name
        "Roughness": 0.4,                                   # scalar
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
* Other value kinds: double vector [X, Y, Z, W]; texture collection / runtime virtual texture /
  sparse volume texture / material parameter collection: object path; font {"Font": path, "Page": n}.
* ``Parent`` change is the editor's: overrides whose parameter (name and type) the new parent also
  has are kept, the others are dropped (as the Material Instance Editor's next edit does).
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


def _number(value):
    return type(value) in (int, float)


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
        if "Font" in value and set(value) <= {"Font", "Page"} and type(value["Font"]) is str \
                and type(value.get("Page", 0)) is int and value.get("Page", 0) >= 0:
            return {"Font": value["Font"], "Page": value.get("Page", 0)}
        raise common.error(path, "An object value is a static component mask {\"R\": bool, \"G\": bool, \"B\": bool, "
                           "\"A\": bool}, a color {\"R\", \"G\", \"B\", \"A\"} or a font {\"Font\": path, \"Page\": n}.",
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
    """(association, index, name, value, document path) of every authored parameter."""
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
    return {_key(row["association"], row["index"], row["name"]): row["value"]
            for row in live["parameters"] if row["overridden"]}


def _layers_document(layers):
    return {"Layers": list(layers.get("layers", [])), "Blends": list(layers.get("blends", [])),
            "Names": list(layers.get("names", [])), "Visible": list(layers.get("visible", []))}


def _body(live):
    body = {"Parent": live["parent"], "Parameters": {}}
    maps = {"LayerParameters": {}, "BlendParameters": {}}
    for (association, index, name), value in sorted(overrides(live).items(), key=lambda item: (
            ("global", "layer", "blend").index(item[0][0]), item[0][1], item[0][2])):
        if association == "global":
            body["Parameters"][name] = value
        else:
            maps[_ASSOCIATION_KEY[association]].setdefault(str(index), {})[name] = value
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
    ops = []
    for association, index, name, value, path in authored_entries(body):
        key = _key(association, index, name)
        op = {"name": name}
        if association != "global":
            op.update(association=association, index=index)
        if value is None:
            if key in current or parent_changed:
                ops.append(({"op": "clear", **op}, path))
        elif parent_changed or key not in current or not common.same(value, current[key], exact=False):
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
    if parent_changed or ops or refraction:
        values = {"schema_version": 1, "ops": [op for op, _ in ops]}
        if parent_changed:
            values["parent"] = body["Parent"] or ""
        if refraction:
            values["refraction_depth_bias"] = body["RefractionDepthBias"]
        outcome = common.invoke(context, BY_ID, EDIT, values, (NamedOpaqueHandleArgument("asset", calls.write),),
                                paths=_paths([path for _, path in ops]), prefix=PREFIX)
        changes.extend(outcome.changed_set)
        _check_applied(body, read_live(calls))
    changes.extend(region.apply(properties, read, mutable, context))
    return tuple(changes)


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
    for association, index, name, value, path in authored_entries(body):
        key = _key(association, index, name)
        if value is not None and (key not in current or not common.same(value, current[key], exact=False)):
            raise common.error(path, f"Parameter '{name}' did not take the value (the parent {live['parent']} exposes "
                               "it with another type?).", "MaterialInstance.InvalidValue")


def _properties():
    return common.PropertiesRegion(MATERIAL_INSTANCE_CONSTANT_CLASS, BODY_OWNED, PREFIX, ("ThumbnailInfo",))


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
    for association, index, name, value, path in authored_entries(body):
        current = live.get(_ASSOCIATION_KEY[association], {})
        if association != "global":
            current = current.get(str(index), {})
        if value is None:
            if name in current:
                changes.append(Difference(path, DiffKind.REMOVE, True, None, True, current[name]))
        elif name not in current or not common.same(value, current[name], exact):
            changes.append(Difference(path, DiffKind.REPLACE, True, value, name in current, current.get(name)))
    changes.extend(common.properties_diff(authored.get("Properties", {}), extracted.get("Properties", {}), exact))
    return tuple(changes)


def parameter_schema(live):
    rows = []
    for row in live["parameters"]:
        association = row["association"]
        if association == "global":
            path = f"/Body/Parameters/{common.escape(row['name'])}"
        else:
            path = f"/Body/{_ASSOCIATION_KEY[association]}/{row['index']}/{common.escape(row['name'])}"
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
