# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Registered Animation Blueprint Profile (profile.anim_blueprint.v1): read, create and update UAnimBlueprint.

Body (every key optional; an omitted key keeps the live state, a present key is its complete
desired state)::

    "ParentClass": "/Script/Engine.AnimInstance",
    "TargetSkeleton": "/Game/Char/SK_Mannequin_Skeleton.SK_Mannequin_Skeleton",   (null for a template)
    "Template": false,                     (fixed at creation; true = template Animation Blueprint)
    "ImplementedInterfaces": [...], "Variables": [...], "ClassDefaults": {...},      (as UBlueprint)
    "EventDispatchers": [{"Name": "OnLanded", "Inputs": [...]}],  (as UBlueprint; legacy key DelegateSignatureGraphs)
    "AnimGraph": {"Nodes": [...], "Links": [...]},                                   (graphs.schemas.anim)
    "AnimLayers": [{"Name": "UpperBody", "Nodes": [...], "Links": [...]}],
    "UbergraphPages": [...], "FunctionGraphs": [...], "MacroGraphs": [...],          (K2, as UBlueprint)
    "Preview": {"Mesh": "/Game/...", "AnimationBlueprint": "/Game/...",
                "ApplicationMethod": "LinkedLayers" | "LinkedAnimGraph", "Tag": "Name"},
    "SyncGroups": ["Locomotion"],
    "ParentAssetOverrides": [{"ParentNodeGuid": "<32 hex>", "Asset": "/Game/..."}],  (child Animation Blueprints)
    "Thumbnail": {"OrbitPitch": -11.25, "OrbitYaw": -157.5, "OrbitZoom": 0}

An Anim Layer Interface (Content Browser > Animation > Animation Layer Interface; an Animation
Blueprint of type Interface) is the document with ``"ParentClass": "/Script/Engine.AnimLayerInterface"``:
no TargetSkeleton, Template, variables, event dispatchers, interfaces, event graphs or functions; its ``AnimLayers`` hold
only each layer's ``Output`` (``Properties.LayerGroup``) and its input poses
(``Anim:LinkedInputPose``), unlinked.  The type is fixed at creation.

Top-level ``Properties`` are the Class Settings (reflected UAnimBlueprint/UBlueprint properties such
as ``bUseMultiThreadedAnimationUpdate``, ``DefaultBindingClass``, ``BlueprintDescription``); they merge.

Members go through ``blueprint.member.*`` (provider "blueprint", Animation Blueprint settings through
``blueprint.member.setting.set.v1``), graphs through the graph executor (provider "graph", Anim / SM
adapters registered by the AssetDocumentAnimation module).  Writes are covered by the package snapshot
(existing target) or the factory rollback (created target).
See docs/architecture/asset-document-anim-blueprint-matrix.md for the support matrix.
"""
from __future__ import annotations

import re

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.diagnostics import contract_error, with_diagnostics
from asset_document.diff.model import Difference, DiffKind
from asset_document.dto import PrimitiveRequirement
from asset_document.graphs.extract import extract_graphs
from asset_document.graphs.reconcile import PlanOp
from asset_document.graphs.schemas.anim import (
    ANIM_ADAPTER, anim_label, decode_anim_graph, decode_anim_layers, encode_anim_graph,
)
from asset_document.graphs.schemas.k2 import SECTIONS, SECTION_BY_ROLE, decode_body, encode_graph
from asset_document.graphs.executor import LINK_CHECK, GraphExecutor, graph_requirements
from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, DocumentAction, IdentityPolicy, OmissionPolicy, OrderPolicy,
    ProfileMeta, RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetLifecycleSpec, RuntimeTargetPolicy
from asset_document.profiles import blueprint_dispatchers as dispatchers
from asset_document.profiles import blueprint_renames as renames
from asset_document.profiles import ublueprint_members as members
from asset_document.profiles.ublueprint_graph_compare import _refine, _shape, graphs_match, section_mismatches, _is_stub
from asset_document.profiles.ublueprint_runtime import (
    PENDING_RELAYOUT, _Members as _BlueprintMembers, _invoke as _blueprint_invoke, read_live_with_relayout,
    relayout_differences, trial_parent_context, validate_links,
)
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument, _optional_handle, _single_handle
from asset_document.read_only_extract import admits_read_only

ANIM_BLUEPRINT_CLASS = "/Script/Engine.AnimBlueprint"
ROUTE = "blueprint_profile"
PROFILE_ID = "profile.anim_blueprint.v1"
K2_SECTIONS = tuple(section for section, _ in SECTIONS)
ANIM_MEMBER_KEYS = ("ParentClass", "ImplementedInterfaces", "Variables", "ClassDefaults")
SETTING_KEYS = ("Preview", "SyncGroups", "ParentAssetOverrides", "Thumbnail")
# Accepted only as [] (or omitted), like the legacy Profile: no components on an Animation
# Blueprint, and the editor offers timelines only in actor-based Blueprints.
EMPTY_ONLY_KEYS = {"Components": "Animation Blueprints have no components",
                   "Timelines": "The Blueprint editor offers Timelines only in actor-based Blueprints "
                                "(FBlueprintEditorUtils::DoesSupportTimelines)"}
BODY_KEYS = (("ParentClass", "TargetSkeleton", "Template") + ANIM_MEMBER_KEYS[1:] + (dispatchers.KEY, dispatchers.ALIAS)
             + ("AnimGraph", "AnimLayers")
             + K2_SECTIONS + SETTING_KEYS + tuple(EMPTY_ONLY_KEYS))
CLASS_RESOLVE = "editor.class.resolve_canonical.v1"
RESOLVE = "editor.asset.resolve_existing.v1"
LIFECYCLE = ("editor.blueprint.scratch.clone.v1", "editor.blueprint.scratch.create.v1",
             "editor.blueprint.asset.create.v1", "editor.asset.rollback_created.v1")
MEMBER_PRIMITIVES = tuple(f"blueprint.member.{name}.v1" for name in (
    "query", "parent.set", "interface.add", "interface.remove", "variable.set", "variable.remove",
    "component.add", "component.attach", "component.remove", "property.set", "setting.set", *dispatchers.PRIMITIVES,
    *renames.PRIMITIVES))
REQUIREMENTS = (tuple(PrimitiveRequirement(item, 1, ROUTE) for item in (CLASS_RESOLVE, RESOLVE, *LIFECYCLE))
                + graph_requirements(ROUTE)
                + tuple(PrimitiveRequirement(item, 1, ROUTE) for item in MEMBER_PRIMITIVES + (LINK_CHECK,)))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}
DEFAULT_PARENT = "/Script/Engine.AnimInstance"
LAYER_INTERFACE = "/Script/Engine.AnimLayerInterface"
# Nodes an Anim Layer Interface's layer graphs hold (the layer result and its input poses).
INTERFACE_LAYER_ACTIONS = frozenset({"Anim:LinkedInputPose"})
_TARGET = re.compile(r"/Game/[A-Za-z0-9_/-]+\Z")
_GUID = re.compile(r"[0-9A-Fa-f]{32}\Z")
# Body.Preview field -> (setting, default value).
PREVIEW_FIELDS = {
    "Mesh": ("preview_mesh", None),
    "AnimationBlueprint": ("preview_animation_blueprint", None),
    "ApplicationMethod": ("preview_animation_blueprint_application_method", "LinkedLayers"),
    "Tag": ("preview_animation_blueprint_tag", ""),
}
# Graph roles -> Body sections extracted for an Animation Blueprint.
EXTRACT_SECTIONS = {"anim_graph": "AnimGraph", "anim_layer": "AnimLayers", **{role: section for section, role in SECTIONS}}
_LEGACY_KEYS = {
    "BlueprintClassSettings": "top-level Properties (BlueprintDisplayName, BlueprintDescription, ...)",
    "Optimization": "top-level Properties (bUseMultiThreadedAnimationUpdate, bWarnAboutBlueprintUsage, ...)",
    "DefaultBindingClass": "top-level Properties.DefaultBindingClass",
    "StateMachines": "the owning Anim:StateMachine node's nested Graph in AnimGraph / AnimLayers",
}


def _error(path, message, code="AnimBlueprint.InvalidDocument"):
    return contract_error(path, code, message)


def _require_target(value: object) -> str:
    if admits_read_only(value):
        return value
    if type(value) is not str or _TARGET.fullmatch(value) is None:
        raise _error("/Target", "Target must be a canonical /Game package path such as /Game/Anim/ABP_Hero.",
                     "AnimBlueprint.NonCanonicalTarget")
    return value


def self_class(target: str) -> str:
    return f"{target}.{target.rsplit('/', 1)[-1]}_C"


def _snapshot(value):
    codec = DocumentCodec()
    return codec.decode(codec.encode(value))


def _object_path(value: object, path: str) -> str | None:
    if value is None:
        return None
    if type(value) is dict and set(value) <= {"Kind", "Asset", "Path", "Class"}:  # legacy {"Kind":"AssetRef",...}
        value = value.get("Asset", value.get("Path"))
    if type(value) is not str or not value.startswith("/"):
        raise _error(path, "Expected an object path such as /Game/Folder/Asset.Asset (or null).")
    return members.object_path(value)


def _invoke(context, primitive, values, handles=(), path="/Body"):
    return _blueprint_invoke(context, primitive, values, handles, path, BY_ID)


def _Members(context, read, mutable=None):
    return _BlueprintMembers(context, read, mutable, BY_ID)


def _graph_executor(context, read, mutable, target):
    read_graph = context.handoff(source_handle=read, target_provider_id="graph", access="read_only")
    write_graph = (context.handoff(source_handle=mutable, target_provider_id="graph", access="mutable")
                   if mutable is not None else None)
    return GraphExecutor(context, read_handle=read_graph, write_handle=write_graph, route_id=ROUTE,
                         adapter=ANIM_ADAPTER, self_class=self_class(target))


# ---------------------------------------------------------------------------
# Document shape
# ---------------------------------------------------------------------------
def _validate_header(document) -> None:
    if type(document) is not dict:
        raise _error("/", "An AnimBlueprint document is a JSON object.")
    if type(document.get("SchemaVersion")) is not int or document["SchemaVersion"] != 1:
        raise _error("/SchemaVersion", "SchemaVersion must be 1.", "Meta.InvalidSchemaVersion")
    _require_target(document.get("Target"))
    if document.get("Class") != ANIM_BLUEPRINT_CLASS:
        raise _error("/Class", "Class must be /Script/Engine.AnimBlueprint.", "AnimBlueprint.ClassMismatch")
    if document.get("Action") not in {item.value for item in DocumentAction}:
        raise _error("/Action", "Action is Create, Update or CreateOrUpdate.", "AnimBlueprint.InvalidAction")
    for key in ("Definitions", "Properties"):
        if type(document.get(key, {})) is not dict:
            raise _error(f"/{key}", f"{key} must be an object.")
    if document.get("Definitions"):
        raise _error("/Definitions", "AnimBlueprint documents do not use Definitions.", "AnimBlueprint.UnsupportedRegion")
    body = document.get("Body", {})
    if type(body) is not dict:
        raise _error("/Body", "Body must be an object.")
    for key in body:
        if key in _LEGACY_KEYS:
            raise _error(f"/Body/{key}", f"Body.{key} is not a region of this Profile; use {_LEGACY_KEYS[key]}.",
                         "AnimBlueprint.UnknownBodyKey")
        if key not in BODY_KEYS:
            raise _error(f"/Body/{key}", f"Unknown Body key '{key}'. Keys: {', '.join(BODY_KEYS)}.",
                         "AnimBlueprint.UnknownBodyKey")
    for key, what in EMPTY_ONLY_KEYS.items():
        if body.get(key) not in (None, []):
            raise _error(f"/Body/{key}", f"{what}; give [] or omit the key.", "AnimBlueprint.UnsupportedRegion")


def _canonical_template(value: object) -> bool:
    if type(value) is dict:  # legacy {"bIsTemplate": bool, "BlueprintType": "Normal"}
        if value.get("BlueprintType", "Normal") != "Normal" or set(value) - {"bIsTemplate", "BlueprintType"}:
            raise _error("/Body/Template", "Only Normal Animation Blueprints are supported.",
                         "AnimBlueprint.UnsupportedRegion")
        value = value.get("bIsTemplate", False)
    if type(value) is not bool:
        raise _error("/Body/Template", "Template is true (a template Animation Blueprint) or false.")
    return value


def _canonical_preview(value: object) -> dict:
    if type(value) is not dict:
        raise _error("/Body/Preview", "Preview is an object {Mesh, AnimationBlueprint, ApplicationMethod, Tag}.")
    legacy = {"PreviewSkeletalMesh": "Mesh", "PreviewAnimationBlueprint": "AnimationBlueprint",
              "PreviewAnimationBlueprintApplicationMethod": "ApplicationMethod", "PreviewAnimationBlueprintTag": "Tag"}
    output = {}
    for key, item in value.items():
        field = legacy.get(key, key)
        if field not in PREVIEW_FIELDS:
            raise _error(f"/Body/Preview/{key}", f"Unknown Preview field. Fields: {', '.join(PREVIEW_FIELDS)}.")
        path = f"/Body/Preview/{key}"
        if field in ("Mesh", "AnimationBlueprint"):
            item = _object_path(item, path)
        elif field == "ApplicationMethod" and item not in ("LinkedLayers", "LinkedAnimGraph"):
            raise _error(path, "ApplicationMethod is LinkedLayers or LinkedAnimGraph.")
        elif field == "Tag" and type(item) is not str:
            raise _error(path, "Tag is a name string.")
        if item != PREVIEW_FIELDS[field][1]:
            output[field] = item
    return output


def _canonical_sync_groups(value: object) -> list:
    if type(value) is not list:
        raise _error("/Body/SyncGroups", "SyncGroups is an array of group names.")
    names, seen = [], set()
    for index, item in enumerate(value):
        if type(item) is dict and set(item) <= {"Name", "Color"}:  # legacy {"Name": ...}
            if "Color" in item:
                raise _error(f"/Body/SyncGroups/{index}/Color", "Sync group colors are not authored.",
                             "AnimBlueprint.UnsupportedRegion")
            item = item.get("Name")
        if type(item) is not str or not item or item in seen:
            raise _error(f"/Body/SyncGroups/{index}", "Sync group names are unique non-empty strings.")
        seen.add(item)
        names.append(item)
    return names


def _canonical_overrides(value: object) -> list:
    if type(value) is not list:
        raise _error("/Body/ParentAssetOverrides", "ParentAssetOverrides is an array of {ParentNodeGuid, Asset}.")
    rows, seen = [], set()
    for index, item in enumerate(value):
        path = f"/Body/ParentAssetOverrides/{index}"
        if type(item) is not dict or set(item) - {"ParentNodeGuid", "Asset"} or "ParentNodeGuid" not in item:
            raise _error(path, "A parent asset override is {\"ParentNodeGuid\": \"<32 hex>\", \"Asset\": path|null}.")
        guid = item["ParentNodeGuid"]
        if type(guid) is not str or _GUID.fullmatch(guid.replace("-", "")) is None:
            raise _error(f"{path}/ParentNodeGuid", "ParentNodeGuid is the parent node's 32-hex-digit NodeGuid.")
        guid = guid.replace("-", "").upper()
        if guid in seen:
            raise _error(f"{path}/ParentNodeGuid", "Each parent node is overridden once.")
        seen.add(guid)
        rows.append({"ParentNodeGuid": guid, "Asset": _object_path(item.get("Asset"), f"{path}/Asset")})
    return sorted(rows, key=lambda row: row["ParentNodeGuid"])


def _canonical_thumbnail(value: object):
    if value is None:
        return None
    if type(value) is not dict or set(value) != {"OrbitPitch", "OrbitYaw", "OrbitZoom"} or any(
            type(item) not in (int, float) or type(item) is bool for item in value.values()):
        raise _error("/Body/Thumbnail", "Thumbnail is {\"OrbitPitch\", \"OrbitYaw\", \"OrbitZoom\"} numbers (or null).")
    return {key: float(value[key]) for key in ("OrbitPitch", "OrbitYaw", "OrbitZoom")}


def canonicalize(document) -> dict:
    _validate_header(document)
    DocumentCodec().validate(document)
    body = dict(document.get("Body", {}))
    canonical = {key: value for key, value in document.items() if key != "Body"}
    canonical.setdefault("Definitions", {})
    properties = dict(canonical.get("Properties", {}))
    if "TargetSkeleton" in properties:
        # The Class Settings spelling of Body.TargetSkeleton.
        moved = properties.pop("TargetSkeleton")
        if "TargetSkeleton" in body and _object_path(body["TargetSkeleton"], "/Body/TargetSkeleton") != \
                _object_path(moved, "/Properties/TargetSkeleton"):
            raise _error("/Properties/TargetSkeleton", "TargetSkeleton is given twice with different values.")
        body.setdefault("TargetSkeleton", moved)
    canonical["Properties"] = properties
    out = members.canonical_members({key: body[key] for key in ANIM_MEMBER_KEYS if key in body})
    rows = dispatchers.canonical(body)
    if rows is not None:
        out[dispatchers.KEY] = rows
        dispatchers.check_names(out)
    if "TargetSkeleton" in body:
        out["TargetSkeleton"] = _object_path(body["TargetSkeleton"], "/Body/TargetSkeleton")
    if "Template" in body:
        out["Template"] = _canonical_template(body["Template"])
    if out.get("Template") and out.get("TargetSkeleton"):
        raise _error("/Body/TargetSkeleton", "Template Animation Blueprints have no TargetSkeleton.",
                     "AnimBlueprint.InvalidTemplateSkeleton")
    if document.get("Action") == DocumentAction.CREATE.value and "ParentClass" not in out:
        raise _error("/Body/ParentClass" if "Body" in document else "/Body",
                     "AnimBlueprint creation requires Body.ParentClass", "AnimBlueprint.ParentClassRequired")
    if document.get("Action") == DocumentAction.CREATE.value and not out.get("Template") \
            and not out.get("TargetSkeleton") and out.get("ParentClass") != LAYER_INTERFACE:
        raise _error("/Body/TargetSkeleton", "Creating a (non-template) Animation Blueprint requires Body.TargetSkeleton.",
                     "AnimBlueprint.MissingTargetSkeleton")
    target = document["Target"]
    if out.get("ParentClass") == LAYER_INTERFACE:
        validate_interface_body(body, target)
    for key, decode in (("AnimGraph", decode_anim_graph), ("AnimLayers", decode_anim_layers)):
        if body.get(key) is not None:
            decode(body[key], self_class=self_class(target))
            out[key] = body[key]
    for section in K2_SECTIONS:
        if body.get(section) is not None:
            decode_body({section: body[section]}, self_class=self_class(target))
            out[section] = body[section]
    for key, canonical_value in (("Preview", _canonical_preview), ("SyncGroups", _canonical_sync_groups),
                                 ("ParentAssetOverrides", _canonical_overrides), ("Thumbnail", _canonical_thumbnail)):
        if key in body:
            out[key] = canonical_value(body[key])
    canonical["Body"] = out
    return _snapshot(canonical)


def validate_interface_body(body: dict, target: str) -> None:
    """What an Anim Layer Interface can hold (the Animation Blueprint editor of an interface)."""
    if body.get("Template"):
        raise _error("/Body/Template", "An Anim Layer Interface is not a template.", "AnimBlueprint.InvalidLayerInterface")
    if body.get("TargetSkeleton") is not None:
        raise _error("/Body/TargetSkeleton", "An Anim Layer Interface has no TargetSkeleton.",
                     "AnimBlueprint.InvalidLayerInterface")
    for key, what in (("Variables", "variables"), ("ImplementedInterfaces", "implemented interfaces"),
                      ("ParentAssetOverrides", "asset overrides"), (dispatchers.KEY, "event dispatchers"),
                      (dispatchers.ALIAS, "event dispatchers"),
                      *((section, "event graphs, functions or macros") for section in K2_SECTIONS)):
        if body.get(key) not in (None, []):
            raise _error(f"/Body/{key}", f"An Anim Layer Interface has no {what}; give [] or omit the key.",
                         "AnimBlueprint.InvalidLayerInterface")
    anim_graph = body.get("AnimGraph")
    if anim_graph is not None and (type(anim_graph) is not dict or anim_graph.get("Nodes") or anim_graph.get("Links")):
        raise _error("/Body/AnimGraph", "An Anim Layer Interface has no AnimGraph; omit the key.",
                     "AnimBlueprint.InvalidLayerInterface")
    if body.get("AnimLayers") is None:
        return
    for graph in decode_anim_layers(body["AnimLayers"], self_class=self_class(target)):
        for node in graph.nodes:
            if node.implicit != "root" and (node.action or "").partition("#")[0] not in INTERFACE_LAYER_ACTIONS:
                raise _error(graph.node_path(node.id), f"Layer '{graph.name}' of an Anim Layer Interface holds only its "
                             "Output and input poses (Anim:LinkedInputPose); the layer's graph is implemented by the "
                             "Animation Blueprints that implement the interface.", "AnimBlueprint.InvalidLayerInterface")
        if graph.links:
            raise _error(f"{graph.path}/Links", f"Layer '{graph.name}' of an Anim Layer Interface has no links.",
                         "AnimBlueprint.InvalidLayerInterface")


def _decode_graphs(body: dict, target: str):
    """(anim layer graphs, other graphs, managed roles of the second pass)."""
    owner = self_class(target)
    layers = decode_anim_layers(body["AnimLayers"], self_class=owner) if "AnimLayers" in body else None
    graphs, roles = decode_body({key: body[key] for key in K2_SECTIONS if key in body}, self_class=owner)
    if "AnimGraph" in body:
        graphs = (decode_anim_graph(body["AnimGraph"], self_class=owner),) + graphs
        roles = roles | {"anim_graph"}
    return layers, graphs, roles


# ---------------------------------------------------------------------------
# Settings (blueprint.member.setting.set.v1)
# ---------------------------------------------------------------------------
def _settings_from_query(settings: dict) -> dict:
    """Live settings -> canonical Body keys (defaults omitted inside Preview)."""
    preview = {}
    for field, (name, default) in PREVIEW_FIELDS.items():
        value = settings.get(name)
        if field in ("Mesh", "AnimationBlueprint") and value is not None:
            value = members.object_path(value) if value else None
        if value != default:
            preview[field] = value
    overrides = sorted(({"ParentNodeGuid": row["parent_node_guid"].upper(),
                         "Asset": members.object_path(row["asset"]) if row.get("asset") else None}
                        for row in settings.get("parent_asset_overrides", [])), key=lambda row: row["ParentNodeGuid"])
    thumbnail = settings.get("thumbnail")
    return {"Preview": preview, "SyncGroups": list(settings.get("sync_groups", [])),
            "ParentAssetOverrides": overrides,
            "Thumbnail": {key: float(thumbnail[key]) for key in ("OrbitPitch", "OrbitYaw", "OrbitZoom")}
            if thumbnail else None}


def _thumbnail_equal(desired, live) -> bool:
    if desired is None:
        return True  # null keeps the current camera (the thumbnail info cannot be removed in the editor)
    return live is not None and all(abs(desired[key] - live[key]) <= 1e-3 for key in desired)


def plan_settings(body: dict, settings: dict) -> list:
    live = _settings_from_query(settings)
    ops = []
    if "Preview" in body:
        for field, (name, default) in PREVIEW_FIELDS.items():
            wanted = body["Preview"].get(field, default)
            if wanted != live["Preview"].get(field, default):
                ops.append(("blueprint.member.setting.set.v1", {"name": name, "value": wanted}, f"/Body/Preview/{field}"))
    if "SyncGroups" in body and body["SyncGroups"] != live["SyncGroups"]:
        ops.append(("blueprint.member.setting.set.v1", {"name": "sync_groups", "value": body["SyncGroups"]},
                    "/Body/SyncGroups"))
    if "ParentAssetOverrides" in body and body["ParentAssetOverrides"] != live["ParentAssetOverrides"]:
        value = [{"parent_node_guid": row["ParentNodeGuid"], "asset": row["Asset"]} for row in body["ParentAssetOverrides"]]
        ops.append(("blueprint.member.setting.set.v1", {"name": "parent_asset_overrides", "value": value},
                    "/Body/ParentAssetOverrides"))
    if "Thumbnail" in body and not _thumbnail_equal(body["Thumbnail"], live["Thumbnail"]):
        ops.append(("blueprint.member.setting.set.v1", {"name": "thumbnail", "value": body["Thumbnail"]},
                    "/Body/Thumbnail"))
    return ops


def _live_skeleton(query: dict) -> str | None:
    value = query.get("properties", {}).get("TargetSkeleton")
    return members.object_path(value) if type(value) is str and value else None


def plan_header(document: dict, query: dict) -> list:
    body = document["Body"]
    settings = query.get("settings", {})
    kind = str(settings.get("blueprint_type") or "Normal")
    interface = kind.endswith("Interface")
    if not interface and not kind.endswith("Normal"):
        raise _error("/Class", f"This Animation Blueprint is neither a Normal one nor an Anim Layer Interface ({kind}).",
                     "AnimBlueprint.UnsupportedBlueprintType")
    if "ParentClass" in body and (body["ParentClass"] == LAYER_INTERFACE) != interface:
        raise _error("/Body/ParentClass", "An Animation Blueprint and an Anim Layer Interface are different assets "
                     f"(the type is fixed at creation; this one is {'an Anim Layer Interface' if interface else 'Normal'}); "
                     "create a new asset instead.", "AnimBlueprint.BlueprintTypeImmutable")
    if interface:
        validate_interface_body(body, document["Target"])
    if "Template" in body and body["Template"] != bool(settings.get("is_template")):
        raise _error("/Body/Template", "Template is fixed when the Animation Blueprint is created; "
                     "create a new asset instead.", "AnimBlueprint.TemplateImmutable")
    ops = members.plan_header(document.get("Properties", {}), body, query)
    if "TargetSkeleton" in body and body["TargetSkeleton"] != _live_skeleton(query):
        if settings.get("is_template") and body["TargetSkeleton"] is not None:
            raise _error("/Body/TargetSkeleton", "Template Animation Blueprints have no TargetSkeleton.",
                         "AnimBlueprint.InvalidTemplateSkeleton")
        if body["TargetSkeleton"] is None:
            raise _error("/Body/TargetSkeleton", "A non-template Animation Blueprint needs a TargetSkeleton.",
                         "AnimBlueprint.MissingTargetSkeleton")
        # Class Settings > Target Skeleton.
        ops.append(("blueprint.member.property.set.v1",
                    {"target": "blueprint", "property": "TargetSkeleton", "value": body["TargetSkeleton"]},
                    "/Body/TargetSkeleton"))
    return ops


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------
def apply(document, read, mutable, context) -> tuple:
    body = document["Body"]
    target = document["Target"]
    blueprint = _Members(context, read, mutable)
    blueprint.run(plan_header(document, blueprint.query()))
    query = blueprint.query()
    blueprint.run(renames.plan_renames(body, query) + dispatchers.plan_removes(body, query) + members.plan_interfaces(body, query)
                  + members.plan_variables(body, query) + dispatchers.plan_sets(body, query))
    changed = list(blueprint.changed)
    layers, graphs, roles = _decode_graphs(body, target)
    diagnostics = []
    if layers is not None:
        # Anim layers first and compiled: Linked Anim Layer nodes of the anim graph resolve the
        # layer signatures of the compiled class (the editor offers them only after a compile).
        executor = _graph_executor(context, read, mutable, target)
        report = executor.run(layers, managed_roles=frozenset({"anim_layer"}),
                              role_paths={"anim_layer": "/Body/AnimLayers"})
        changed.extend(report.changed_set)
        diagnostics.extend(report.diagnostics)
    if roles:
        executor = _graph_executor(context, read, mutable, target)
        role_paths = {role: f"/Body/{SECTION_BY_ROLE[role]}" for role in roles if role in SECTION_BY_ROLE}
        role_paths["anim_graph"] = "/Body/AnimGraph"
        report = executor.run(graphs, managed_roles=roles, role_paths=role_paths)
        changed.extend(report.changed_set)
        diagnostics.extend(report.diagnostics)
    values = _Members(context, read, mutable)
    query = values.query()
    values.run(plan_settings(body, query.get("settings", {})) + members.plan_values(body, query))
    changed.extend(values.changed)
    return with_diagnostics(changed, diagnostics)


def repair(document, mutable, context) -> tuple:
    """Compile once if anything is left uncompiled; compiler errors fail the request (Core rolls back)."""
    blueprint = _Members(context, mutable)
    if blueprint.query().get("status") in ("up_to_date", "up_to_date_with_warnings"):
        return ()
    executor = _graph_executor(context, mutable, mutable, document["Target"])
    executor._op_graph_finalize(PlanOp("graph.finalize", "", "/Body", {}))
    return with_diagnostics(executor.changed, executor.diagnostics)


# ---------------------------------------------------------------------------
# Extract and diff
# ---------------------------------------------------------------------------
def extract(target, read, context, include_defaults: bool = False) -> dict:
    return _extract(target, read, context, include_defaults)[0]


def extract_with_defaults(target, read, context, authored=None):
    """Verify / Diff's read (include_defaults) and the relayout the authored graphs' "Layout": "Auto" would make
    (PENDING_RELAYOUT, for diff's Diff-operation differences)."""
    body = authored.get("Body") if type(authored) is dict else None
    document, pending = _extract(target, read, context, True, body)
    return document, ({PENDING_RELAYOUT: pending} if pending else {})


def _authored_graphs(body: dict | None, target: str):
    """decode_authored for read_live_with_relayout: every graph section, anim layers included (as apply's two
    passes manage them)."""
    if type(body) is not dict:
        return lambda: None

    def decode():
        layers, graphs, roles = _decode_graphs(body, target)
        return (tuple(layers or ()) + graphs, roles | ({"anim_layer"} if layers is not None else set()))
    return decode


def _extract(target, read, context, include_defaults: bool = False, authored_body: dict | None = None) -> tuple:
    blueprint = _Members(context, read)
    query = blueprint.query()
    member_body = members.members_from_query(query)
    settings = query.get("settings", {})
    body = {key: member_body[key] for key in ("ParentClass",) if key in member_body}
    body["TargetSkeleton"] = _live_skeleton(query)
    if settings.get("is_template"):
        body["Template"] = True
    for key in ANIM_MEMBER_KEYS[1:]:
        body[key] = member_body[key]
    body[dispatchers.KEY] = dispatchers.from_query(query)
    executor = _graph_executor(context, read, None, target)
    live, pending = read_live_with_relayout(executor, _authored_graphs(authored_body, target), adapter=ANIM_ADAPTER,
                                            self_class=self_class(target))
    graphs = extract_graphs(live, sections=EXTRACT_SECTIONS, adapter=ANIM_ADAPTER, self_class=self_class(target),
                            include_defaults=include_defaults)
    anim = [graph for graph in graphs if graph.role == "anim_graph"]
    if anim:
        body["AnimGraph"] = encode_anim_graph(anim[0])
    body["AnimLayers"] = [encode_anim_graph(graph) for graph in graphs if graph.role == "anim_layer"]
    for section in K2_SECTIONS:
        body[section] = [encode_graph(graph) for graph in graphs if graph.id.startswith(section + "/")]
    body.update(_settings_from_query(settings))
    properties = {key: value for key, value in query.get("properties", {}).items() if key != "TargetSkeleton"}
    return {"SchemaVersion": 1, "Target": target, "Class": ANIM_BLUEPRINT_CLASS,
            "Action": DocumentAction.CREATE_OR_UPDATE.value, "Definitions": {},
            "Properties": properties, "Body": body}, pending


# Node settings extraction omits at their default value; Diff compares an authored default with them.
KNOWN_DEFAULTS = (
    (lambda node: (node.get("Action") or "").startswith("Anim:"), {"Bindings": {}}),
    (lambda node: (node.get("Action") or "").partition("#")[0] == "SM:Transition" and "BlendGraph" not in node,
     {"LogicType": "TLT_StandardBlend"}),
    (lambda node: (node.get("Action") or "").partition("#")[0] == "SM:Transition", {"PriorityOrder": 1}),
)


def with_known_defaults(value):
    """An extracted graph section with the KNOWN_DEFAULTS of its nodes stated (for Diff)."""
    if type(value) is list:
        return [with_known_defaults(item) for item in value]
    if type(value) is not dict:
        return value
    out = {key: with_known_defaults(item) for key, item in value.items()}
    if type(out.get("Action")) is str:
        for applies, defaults in KNOWN_DEFAULTS:
            if applies(out):
                properties = dict(out.get("Properties") or {})
                for key, default in defaults.items():
                    properties.setdefault(key, default)
                out["Properties"] = properties
    return out


def _graph_list_mismatches(key: str, wanted, have, strict: bool) -> list[str]:
    remaining = {graph.name.casefold(): graph for graph in have}
    out = []
    for graph in wanted:
        other = remaining.pop(graph.name.casefold(), None)
        if other is None or not graphs_match(graph, other, strict, anim_label):
            out.append(graph.name)
    return out + [graph.name for graph in remaining.values() if not _is_stub(graph)]


_STRUCTURAL_SETTINGS = frozenset(("Samples", "LayerGroup"))


def _leftover_settings(live_node, authored_node, authored_graph) -> set:
    """Settings an extracted animation or state machine node holds (extraction reports the non-default
    ones) that the authored node leaves out: an omitted setting is the default, so these differ."""
    if not (live_node.action or "").startswith(("Anim:", "SM:")):
        return set()
    linked = {end.pin.casefold() for link in authored_graph.links for end in (link.from_, link.to)
              if end.node == authored_node.id}
    bindings = authored_node.properties.node.get("Bindings")
    linked |= {str(name).casefold() for name in (bindings if hasattr(bindings, "keys") else ())}
    out = set()
    for key in live_node.properties.node:
        if key in authored_node.properties.node or key in _STRUCTURAL_SETTINGS or key.startswith("BlendSpace."):
            continue
        if key.startswith("ShowPin.") and key[len("ShowPin."):].casefold() in linked:
            continue
        out.add(key)
    return out


def omitted_settings(authored, extracted) -> list[str]:
    """``"<node id>: <settings>"`` for each authored animation node whose live node keeps non-default
    settings the document leaves out (nodes paired by the structural comparison's colours)."""
    sides = (_shape(authored), _shape(extracted))
    table: dict = {}
    colours = _refine(sides, lambda key: table.setdefault(key, len(table)), anim_label)
    out = []
    for colour in sorted(set(colours[1].values())):
        remaining = [sides[0][0][node_id] for node_id, value in colours[0].items() if value == colour]
        have = [sides[1][0][node_id] for node_id, value in colours[1].items() if value == colour]
        for live_node in sorted(have, key=lambda node: len(node.properties.node), reverse=True):
            if not remaining:
                break
            best = min(remaining, key=lambda node: len(_leftover_settings(live_node, node, authored)))
            remaining.remove(best)
            extra = _leftover_settings(live_node, best, authored)
            if extra:
                out.append(f"{best.id}: {', '.join(sorted(extra))}")
    have_subgraphs = {graph.name.casefold(): graph for graph in extracted.subgraphs}
    for graph in authored.subgraphs:
        other = have_subgraphs.get(graph.name.casefold())
        if other is not None:
            out += omitted_settings(graph, other)
    return out


def _omitted_settings_difference(key: str, wanted, have) -> Difference | None:
    remaining = {graph.name.casefold(): graph for graph in have}
    found = []
    for graph in wanted:
        other = remaining.get(graph.name.casefold())
        if other is not None:
            found += [f"{graph.name}/{item}" for item in omitted_settings(graph, other)]
    if not found:
        return None
    return Difference(f"/Body/{key}", DiffKind.REPLACE, True, {"OmittedSettingsAreDefaults": found}, True,
                      {"NonDefaultSettings": found})


def diff(authored: dict, extracted: dict, strict: bool = False, defaults: dict | None = None) -> tuple:
    body, live = authored["Body"], extracted["Body"]
    out = members.diff_members({key: body[key] for key in ANIM_MEMBER_KEYS if key in body},
                               {key: live.get(key) for key in ANIM_MEMBER_KEYS},
                               authored.get("Properties", {}), extracted.get("Properties", {}), strict)
    out += dispatchers.diff(body, live)
    for key in ("TargetSkeleton", "Template"):
        if key in body and body[key] != live.get(key, False if key == "Template" else None):
            out.append(Difference(f"/Body/{key}", DiffKind.REPLACE, True, body[key], True, live.get(key)))
    target = authored["Target"]
    owner = self_class(target)
    if "AnimGraph" in body:
        names = _graph_list_mismatches("AnimGraph", (decode_anim_graph(body["AnimGraph"], self_class=owner),),
                                       (decode_anim_graph(with_known_defaults(live["AnimGraph"]), self_class=owner),)
                                       if live.get("AnimGraph") else (), strict)
        if names:
            out.append(Difference("/Body/AnimGraph", DiffKind.REPLACE, True, {"Graphs": names}, True,
                                  {"Graphs": ["AnimGraph"] if live.get("AnimGraph") else []}))
        elif live.get("AnimGraph"):
            leftover = _omitted_settings_difference("AnimGraph", (decode_anim_graph(body["AnimGraph"], self_class=owner),),
                                                    (decode_anim_graph(live["AnimGraph"], self_class=owner),))
            if leftover is not None:
                out.append(leftover)
    if "AnimLayers" in body:
        names = _graph_list_mismatches("AnimLayers", decode_anim_layers(body["AnimLayers"], self_class=owner),
                                       decode_anim_layers(with_known_defaults(live.get("AnimLayers", [])),
                                                          self_class=owner), strict)
        if names:
            out.append(Difference("/Body/AnimLayers", DiffKind.REPLACE, True, {"Graphs": names}, True,
                                  {"Graphs": [graph["Name"] for graph in live.get("AnimLayers", [])]}))
        else:
            leftover = _omitted_settings_difference(
                "AnimLayers", decode_anim_layers(body["AnimLayers"], self_class=owner),
                decode_anim_layers(live.get("AnimLayers", []), self_class=owner))
            if leftover is not None:
                out.append(leftover)
    for section in K2_SECTIONS:
        if section not in body:
            continue
        names = section_mismatches(section, body[section], live.get(section, []), self_class=owner, strict=strict)
        if names:
            out.append(Difference(f"/Body/{section}", DiffKind.REPLACE, True, {"Graphs": names}, True,
                                  {"Graphs": [graph["Name"] for graph in live.get(section, [])]}))
    if "Preview" in body:
        for field, (_, default) in PREVIEW_FIELDS.items():
            wanted, have = body["Preview"].get(field, default), live["Preview"].get(field, default)
            if wanted != have:
                out.append(Difference(f"/Body/Preview/{field}", DiffKind.REPLACE, True, wanted, True, have))
    for key in ("SyncGroups", "ParentAssetOverrides"):
        if key in body and body[key] != live.get(key):
            out.append(Difference(f"/Body/{key}", DiffKind.REPLACE, True, body[key], True, live.get(key)))
    if "Thumbnail" in body and not _thumbnail_equal(body["Thumbnail"], live.get("Thumbnail")):
        out.append(Difference("/Body/Thumbnail", DiffKind.REPLACE, True, body["Thumbnail"], True, live.get("Thumbnail")))
    if strict:
        out += relayout_differences(defaults)
    return tuple(out)


# ---------------------------------------------------------------------------
# Handler
# ---------------------------------------------------------------------------
class AnimBlueprintRuntimeProfile:
    profile_id = PROFILE_ID
    exact_asset_class_path = ANIM_BLUEPRINT_CLASS
    runtime_entry_module = "asset_document/bootstrap.py"

    def required_primitives(self) -> tuple[PrimitiveRequirement, ...]:
        return REQUIREMENTS

    def inspect_profile(self) -> dict:
        return {"Class": ANIM_BLUEPRINT_CLASS, "BodySections": [key for key in BODY_KEYS
                                                                 if key not in EMPTY_ONLY_KEYS and key != dispatchers.ALIAS]}

    def create_template(self, target: str) -> dict:
        return {"SchemaVersion": 1, "Target": _require_target(target), "Class": ANIM_BLUEPRINT_CLASS,
                "Action": DocumentAction.CREATE_OR_UPDATE.value, "Definitions": {}, "Properties": {},
                "Body": {"ParentClass": DEFAULT_PARENT, "TargetSkeleton": None, "ImplementedInterfaces": [],
                         "Variables": [], "ClassDefaults": {}, "AnimGraph": {"Nodes": [], "Links": []},
                         "AnimLayers": [], "UbergraphPages": [], "FunctionGraphs": [], "MacroGraphs": [],
                         "Preview": {}, "SyncGroups": []}}

    def validate(self, document) -> None:
        canonicalize(document)

    def canonicalize(self, document) -> dict:
        return canonicalize(document)

    def create_target_context(self, document, context):
        document = canonicalize(document)
        body = document["Body"]
        parent = body.get("ParentClass", DEFAULT_PARENT)
        outcome = _invoke(context, CLASS_RESOLVE, {"class": parent, "expected_base": None}, (),
                          "/Body/ParentClass" if "ParentClass" in body else "/Body")
        handles = (NamedOpaqueHandleArgument("parent_class", _single_handle(outcome, "class")),)
        if body.get("Template") or parent == LAYER_INTERFACE:
            return handles  # a template Animation Blueprint or an Anim Layer Interface has no skeleton
        resolved = _invoke(context, RESOLVE, {"path": body["TargetSkeleton"], "expected_class": "/Script/Engine.Skeleton"},
                           (), "/Body/TargetSkeleton")
        skeleton = _optional_handle(resolved, "asset_read") if type(resolved.value) is dict and \
            resolved.value.get("exists") is True else None
        if skeleton is None:
            raise _error("/Body/TargetSkeleton", f"Skeleton {body['TargetSkeleton']} does not exist.",
                         "AnimBlueprint.InvalidSkeleton")
        return handles + (NamedOpaqueHandleArgument("factory_context", skeleton),)

    def preflight(self, document, scratch_handle, context):
        document = canonicalize(document)
        _decode_graphs(document["Body"], document["Target"])
        return SNAPSHOT_COVERED

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        return apply(canonicalize(document), read_asset_handle, mutable_asset_handle, context)

    def validate_checks(self, document, read_asset_handle, context):
        """Validate's graph check of the K2 graph sections (UbergraphPages, FunctionGraphs, MacroGraphs; the UBlueprint
        Profile's ublueprint_link_check): node keys, pin names and links, read on the existing target, or on a
        transient template Animation Blueprint of the parent class for a new one (validate_trial_context). AnimGraph
        and AnimLayers are not checked this way: Apply reports their nodes."""
        validate_links(canonicalize(document), read_asset_handle, context, requirements=BY_ID)

    def validate_trial_context(self, document, context):
        return trial_parent_context(context, canonicalize(document)["Body"])

    def repair(self, document, asset_handle, context):
        return repair(canonicalize(document), asset_handle, context)

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        return extract(_require_target(target), asset_handle, context, include_defaults)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != ANIM_BLUEPRINT_CLASS or editor_asset_handle is None:
            raise _error("/Inspect", "AnimBlueprint inspection reads a live Animation Blueprint asset.",
                         "AnimBlueprint.InspectNotAdmitted")
        return self.canonicalize(self.extract(target=exact_asset_object_path.rsplit(".", 1)[0],
                                              asset_handle=editor_asset_handle, context=context))

    def diff(self, authored, extracted):
        """Post-apply verification: lenient about values the editor normalises or omits."""
        return diff(canonicalize(authored), canonicalize(extracted))

    def diff_exact(self, authored, extracted):
        """The Diff operation: every value the document states must hold on the live asset."""
        return diff(canonicalize(authored), canonicalize(extracted), strict=True)

    def extract_with_defaults(self, *, target, asset_handle, context, authored=None):
        return extract_with_defaults(_require_target(target), asset_handle, context, authored)

    def diff_with_defaults(self, authored, extracted, live_defaults, exact):
        """Verify / Diff; the Diff operation (exact) also reports a pending "Layout": "Auto" relayout."""
        return diff(canonicalize(authored), canonicalize(extracted), strict=exact, defaults=live_defaults)


def create_anim_blueprint_runtime_handler():
    return AnimBlueprintRuntimeProfile()


_REGIONS = tuple(
    RegionMeta(f"anim_blueprint.{name}.v1", f"/Body/{key}", RegionOwnership.MANAGED, identity, OrderPolicy.CANONICAL,
               DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE, DeletePolicy.EXPLICIT_ONLY, ComparisonPolicy.CANONICAL)
    for name, key, identity in (
        ("parent_class", "ParentClass", IdentityPolicy.POSITIONAL),
        ("target_skeleton", "TargetSkeleton", IdentityPolicy.POSITIONAL),
        ("template", "Template", IdentityPolicy.POSITIONAL),
        ("implemented_interfaces", "ImplementedInterfaces", IdentityPolicy.STABLE_KEY),
        ("variables", "Variables", IdentityPolicy.STABLE_KEY),
        ("class_defaults", "ClassDefaults", IdentityPolicy.STABLE_KEY),
        ("event_dispatchers", "EventDispatchers", IdentityPolicy.STABLE_KEY),
        ("anim_graph", "AnimGraph", IdentityPolicy.STABLE_KEY),
        ("anim_layers", "AnimLayers", IdentityPolicy.STABLE_KEY),
        ("ubergraph_pages", "UbergraphPages", IdentityPolicy.STABLE_KEY),
        ("function_graphs", "FunctionGraphs", IdentityPolicy.STABLE_KEY),
        ("macro_graphs", "MacroGraphs", IdentityPolicy.STABLE_KEY),
        ("preview", "Preview", IdentityPolicy.POSITIONAL),
        ("sync_groups", "SyncGroups", IdentityPolicy.STABLE_KEY),
        ("parent_asset_overrides", "ParentAssetOverrides", IdentityPolicy.STABLE_KEY),
        ("thumbnail", "Thumbnail", IdentityPolicy.POSITIONAL),
    ))
ANIM_BLUEPRINT_PROFILE_META = ProfileMeta(PROFILE_ID, ROUTE, ANIM_BLUEPRINT_CLASS, _REGIONS, REQUIREMENTS)
ANIM_BLUEPRINT_RUNTIME_REGISTRATION = RuntimeProfileRegistration(
    ANIM_BLUEPRINT_PROFILE_META, create_anim_blueprint_runtime_handler, RuntimeTargetPolicy.CREATE_OR_UPDATE,
    RuntimeTargetLifecycleSpec("anim_blueprint_exact.v1", "anim_blueprint_exact.v1", "anim_blueprint_factory.v1",
                               *(BY_ID[item] for item in LIFECYCLE), ("parent_class", "factory_context"),
                               ("factory_context",)))
