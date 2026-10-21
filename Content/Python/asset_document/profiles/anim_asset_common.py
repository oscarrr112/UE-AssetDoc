"""Shared pieces of the standalone animation asset Profiles (Skeleton, the four blend space classes,
PoseAsset, AnimComposite).

Transport over the ``animation_asset.*`` primitives (provider "animation_asset",
AssetDocumentAnimation/Private/AnimAssets), object path checks, the create-time context (the asset
the class's New-asset dialog asks for, resolved through the Editor and set on its factory) and the
generic Properties region.  See docs/architecture/asset-document-animation-assets-matrix.md.
"""
from __future__ import annotations

import re

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.diff.model import Difference, DiffKind
from asset_document.dto import PrimitiveRequirement
from asset_document.profiles import data_table_common as common
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument, _single_handle

ROUTE = "animation_asset_profile"
PROVIDER = "animation_asset"
CLASS_RESOLVE = common.CLASS_RESOLVE
RESOLVE = common.RESOLVE
REFERENCE_RESOLVE = common.REFERENCE_RESOLVE
SKELETON_READ = "animation_asset.skeleton.read.v1"
SKELETON_EDIT = "animation_asset.skeleton.edit.v1"
BLEND_SPACE_READ = "animation_asset.blend_space.read.v1"
BLEND_SPACE_EDIT = "animation_asset.blend_space.edit.v1"
POSE_ASSET_READ = "animation_asset.pose_asset.read.v1"
POSE_ASSET_EDIT = "animation_asset.pose_asset.edit.v1"
COMPOSITE_READ = "animation_asset.composite.read.v1"
COMPOSITE_EDIT = "animation_asset.composite.edit.v1"

SKELETON_CLASS = "/Script/Engine.Skeleton"
SKELETAL_MESH_CLASS = "/Script/Engine.SkeletalMesh"
ANIM_SEQUENCE_CLASS = "/Script/Engine.AnimSequence"
_OBJECT_PATH = re.compile(r"/[A-Za-z0-9_]+(?:/[^/.\s]+)+\.[^/.\s:]+\Z")


def requirements(*primitives):
    return tuple(PrimitiveRequirement(item, 1, ROUTE) for item in primitives)


error = common.error
escape = common.escape


def object_path(value, path, code, what):
    """An asset object path such as /Game/Anims/Walk.Walk."""
    if type(value) is not str or _OBJECT_PATH.fullmatch(value) is None:
        raise error(path, f"{what} is the object path of an asset, e.g. /Game/Characters/SK_Hero.SK_Hero "
                          f"(got {value!r}).", code)
    return value


def optional_path(value, path, code, what):
    """An object path or "" (none)."""
    return "" if value == "" else object_path(value, path, code, what)


def number(value, path, code, what):
    if type(value) not in (int, float) or value != value or value in (float("inf"), float("-inf")):
        raise error(path, f"{what} is a number.", code)
    return value


def integer(value, low=None, high=None):
    """The int a JSON integer decodes to (a float with an integral value counts: Diff hands numbers over as
    doubles), or None when it is not one or lies outside low..high."""
    if type(value) is float and value == value and value not in (float("inf"), float("-inf")) and value.is_integer():
        value = int(value)
    if type(value) is not int or (low is not None and value < low) or (high is not None and value > high):
        return None
    return value


def same_number(left, right):
    return common.same(left, right, exact=False)


class AnimCalls(common.DataCalls):
    """Calls to the "animation_asset" provider against one asset (editor handles handed off)."""

    def __init__(self, context, by_id, read, mutable=None, prefix="AnimationAsset"):
        self.context = context
        self.by_id = by_id
        self.prefix = prefix
        self.read = context.handoff(source_handle=read, target_provider_id=PROVIDER, access="read_only")
        self.write = (context.handoff(source_handle=mutable, target_provider_id=PROVIDER, access="mutable")
                      if mutable is not None else None)


def resolve_context(context, by_id, path, document_path, expected_class, prefix, what):
    """Read handle of the create-time context asset (the factory field the New-asset dialog sets)."""
    try:
        outcome = common.invoke(context, by_id, REFERENCE_RESOLVE, {"expected_class": expected_class, "path": path},
                                path=document_path, prefix=prefix)
    except AssetDocumentContractError as failure:
        raise error(document_path, f"{what} '{path}' is not a loadable {expected_class.rsplit('.', 1)[-1]} "
                    f"({failure.diagnostic.message}).", f"{prefix}.ContextNotFound") from None
    return (NamedOpaqueHandleArgument("factory_context", _single_handle(outcome, "asset")),)


def require_unchanged(body, key, live_value, prefix, message):
    """A create-time field (the asset's Skeleton, ...) that the editor does not change afterwards."""
    if key in body and body[key] != live_value:
        raise error(f"/Body/{key}", f"{message} (it is {live_value or 'none'}).", f"{prefix}.{key}Immutable")


def value_diff(changes, path, authored, live, exact):
    if not common.same(authored, live, exact):
        changes.append(Difference(path, DiffKind.REPLACE, True, authored, live is not None, live))


__all__ = ("ANIM_SEQUENCE_CLASS", "AnimCalls", "BLEND_SPACE_EDIT", "BLEND_SPACE_READ", "COMPOSITE_EDIT",
           "COMPOSITE_READ", "POSE_ASSET_EDIT", "POSE_ASSET_READ", "PROVIDER", "ROUTE", "SKELETAL_MESH_CLASS",
           "SKELETON_CLASS", "SKELETON_EDIT", "SKELETON_READ", "object_path", "optional_path", "requirements",
           "resolve_context", "value_diff")
