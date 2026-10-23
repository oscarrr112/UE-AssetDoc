# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Reusable target lifecycle components for runtime Profile registrations.

A lifecycle names the four Editor primitives that preview and create a new
target: clone scratch (preview an existing asset), create scratch (preview a
new asset), create asset and roll back a created asset. Profiles opt in by
passing one as ``RuntimeProfileRegistration.target_lifecycle``; without one a
Profile is update-only.
"""
from __future__ import annotations

from asset_document.dto import PrimitiveRequirement
from asset_document.profile_runtime import RuntimeTargetLifecycleSpec, lifecycle_requirements
from asset_document.regions.primitive_catalog import GENERIC_ROUTE_ID


GENERIC_FACTORY_LIFECYCLE_ID = "factory_generic.v1"


def generic_factory_lifecycle(
    route_id: str = GENERIC_ROUTE_ID,
    create_context_handle_names: tuple[str, ...] = (),
) -> RuntimeTargetLifecycleSpec:
    """Create any asset class the editor can create from its "Add New" factory.

    The Editor selects the factory whose SupportedClass is the target class (or
    the nearest New-menu ancestor) and rejects abstract classes, Blueprint asset
    classes and classes without a factory with stable ``FactoryCreate.*``
    diagnostics.

    A Profile whose factory needs one context asset (AnimMontage: its Skeleton)
    passes ``("factory_context",)``; its ``create_target_context`` then returns
    that read handle and the Editor sets it on the single factory property that
    accepts it, as the factory's New-asset dialog would.
    """
    return RuntimeTargetLifecycleSpec(
        GENERIC_FACTORY_LIFECYCLE_ID,
        GENERIC_FACTORY_LIFECYCLE_ID,
        GENERIC_FACTORY_LIFECYCLE_ID,
        clone_scratch=PrimitiveRequirement("editor.scratch.clone_asset.v2", 2, route_id),
        create_scratch=PrimitiveRequirement("editor.scratch.create_by_factory.v1", 1, route_id),
        create_asset=PrimitiveRequirement("editor.asset.create_by_factory.v2", 2, route_id),
        rollback_created=PrimitiveRequirement("editor.asset.rollback_created.v1", 1, route_id),
        create_context_handle_names=create_context_handle_names,
    )


GENERIC_FACTORY_LIFECYCLE = generic_factory_lifecycle()

__all__ = (
    "GENERIC_FACTORY_LIFECYCLE",
    "GENERIC_FACTORY_LIFECYCLE_ID",
    "generic_factory_lifecycle",
    "lifecycle_requirements",
)
