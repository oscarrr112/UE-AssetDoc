"""Frozen, class-neutral runtime handler registration contracts."""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from inspect import iscoroutinefunction, isgeneratorfunction, signature
from typing import Callable, Protocol

from asset_document.diagnostics import contract_error
from asset_document.diff.model import Difference
from asset_document.dto import ChangedRecord, JsonValue, PrimitiveRequirement
from asset_document.meta import ProfileMeta
from asset_document.regions.primitive_catalog import (
    GenericPrimitiveContext, NamedOpaqueHandleArgument, OpaqueHandleProxy,
)


class RuntimeTargetPolicy(str, Enum):
    CREATE_OR_UPDATE = "CreateOrUpdate"
    UPDATE_ONLY = "UpdateOnly"


class RuntimeProfileHandler(Protocol):
    @property
    def profile_id(self) -> str: ...
    @property
    def exact_asset_class_path(self) -> str: ...
    @property
    def runtime_entry_module(self) -> str: ...
    def required_primitives(self) -> tuple[PrimitiveRequirement, ...]: ...
    def inspect(self, *, mode: str, exact_class_path: str,
                exact_asset_object_path: str | None,
                editor_asset_handle: OpaqueHandleProxy | None,
                context: GenericPrimitiveContext) -> dict[str, JsonValue]: ...
    def inspect_profile(self) -> dict[str, JsonValue]: ...
    def create_template(self, target: str) -> dict[str, JsonValue]: ...
    def validate(self, document: JsonValue) -> None: ...
    def canonicalize(self, document: JsonValue) -> dict[str, JsonValue]: ...
    def create_target_context(self, document: JsonValue, context: GenericPrimitiveContext) -> tuple[NamedOpaqueHandleArgument, ...]: ...
    def preflight(self, document: JsonValue, scratch_handle: OpaqueHandleProxy, context: GenericPrimitiveContext) -> list[dict[str, JsonValue]] | None: ...
    def apply(self, document: JsonValue, read_asset_handle: OpaqueHandleProxy, mutable_asset_handle: OpaqueHandleProxy, context: GenericPrimitiveContext) -> tuple[ChangedRecord, ...]: ...
    def repair(self, document: JsonValue, asset_handle: OpaqueHandleProxy, context: GenericPrimitiveContext) -> tuple[ChangedRecord, ...]: ...
    def extract(self, *, target: str, asset_handle: OpaqueHandleProxy, context: GenericPrimitiveContext, include_defaults: bool = False) -> dict[str, JsonValue]: ...
    def diff(self, authored: JsonValue, extracted: JsonValue) -> tuple[Difference, ...]: ...


def _error(code: str, message: str):
    return contract_error("/Registry", "Registry." + code, message)


@dataclass(frozen=True, slots=True)
class RuntimeTargetLifecycleSpec:
    lifecycle_id: str
    scratch_category_id: str
    factory_id: str
    clone_scratch: PrimitiveRequirement
    create_scratch: PrimitiveRequirement
    create_asset: PrimitiveRequirement
    rollback_created: PrimitiveRequirement
    create_context_handle_names: tuple[str, ...]
    # Names in ``create_context_handle_names`` that create_target_context may leave out
    # (the catalog declares them not required, e.g. an Animation Blueprint template has no skeleton).
    optional_create_context_handle_names: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        identities = (self.lifecycle_id, self.scratch_category_id, self.factory_id)
        names = self.create_context_handle_names
        optional = self.optional_create_context_handle_names
        if (any(type(value) is not str or not value.strip() for value in identities)
                or type(optional) is not tuple or any(name not in names for name in optional)
                or any(type(item) is not PrimitiveRequirement for item in lifecycle_requirements(self))
                or type(names) is not tuple
                or any(type(name) is not str or not name.strip() for name in names)
                or len(set(names)) != len(names)):
            raise _error("RuntimeTargetLifecycleInvalid", "Lifecycle requires exact nonempty identities, primitive requirements and unique handle names.")


def lifecycle_requirements(lifecycle: RuntimeTargetLifecycleSpec) -> tuple[PrimitiveRequirement, ...]:
    return (lifecycle.clone_scratch, lifecycle.create_scratch, lifecycle.create_asset, lifecycle.rollback_created)


def _validate_registration(profile, policy, lifecycle) -> None:
    if type(profile) is not ProfileMeta or type(policy) is not RuntimeTargetPolicy:
        raise _error("RuntimeRegistrationInvalid", "Runtime registration requires exact ProfileMeta and target policy.")
    if lifecycle is None:
        # No lifecycle: the Profile can only update existing assets.
        if policy is not RuntimeTargetPolicy.UPDATE_ONLY:
            raise _error("RuntimeTargetLifecycleRequired", "Creating targets requires a target lifecycle; omit it only for UpdateOnly Profiles.")
        return
    if type(lifecycle) is not RuntimeTargetLifecycleSpec:
        raise _error("RuntimeTargetLifecycleInvalid", "Runtime registration requires an exact lifecycle spec.")
    if any(requirement not in profile.required_primitives or requirement.required_by_route_id != profile.route_id
           for requirement in lifecycle_requirements(lifecycle)):
        raise _error("RuntimeTargetLifecyclePrimitiveMismatch", "All lifecycle requirements must belong to the ordered Profile tuple and route.")


@dataclass(frozen=True, slots=True)
class RuntimeProfileRegistration:
    profile_meta: ProfileMeta
    handler_factory: Callable[[], RuntimeProfileHandler]
    target_policy: RuntimeTargetPolicy
    target_lifecycle: RuntimeTargetLifecycleSpec | None = None

    def __post_init__(self) -> None:
        _validate_registration(self.profile_meta, self.target_policy, self.target_lifecycle)
        if self.handler_factory is None:
            raise _error("RuntimeFactoryMissing", "Runtime handler factory is required.")
        if not callable(self.handler_factory):
            raise _error("RuntimeFactoryNotCallable", "Runtime handler factory must be callable.")
        try:
            signature(self.handler_factory).bind()
        except (TypeError, ValueError):
            raise _error("RuntimeFactoryNotCallable", "Runtime handler factory must accept zero arguments.") from None


@dataclass(frozen=True, slots=True)
class FrozenRuntimeProfileRegistration:
    profile_meta: ProfileMeta
    handler: RuntimeProfileHandler
    target_policy: RuntimeTargetPolicy
    target_lifecycle: RuntimeTargetLifecycleSpec | None = None

    def __post_init__(self) -> None:
        _validate_registration(self.profile_meta, self.target_policy, self.target_lifecycle)
        validate_handler(self.handler, self.profile_meta)


def validate_handler(handler: RuntimeProfileHandler, profile: ProfileMeta) -> None:
    """Validate structure without invoking any operation or retaining request state."""
    methods = ("required_primitives", "inspect", "inspect_profile", "create_template", "validate",
               "canonicalize", "create_target_context", "preflight", "apply", "repair", "extract", "diff")
    try:
        for name in methods:
            method = getattr(handler, name)
            if (not callable(method) or iscoroutinefunction(method) or isgeneratorfunction(method)
                    or getattr(method, "__func__", None) is getattr(RuntimeProfileHandler, name)):
                raise ValueError(name)
            expected = tuple(signature(getattr(RuntimeProfileHandler, name)).parameters.values())[1:]
            actual = tuple(signature(method).parameters.values())
            shape = lambda parameters: tuple((p.name, p.kind, p.default) for p in parameters)
            if shape(actual) != shape(expected):
                raise ValueError(name)
        identities = (handler.profile_id, handler.exact_asset_class_path, handler.runtime_entry_module)
        if any(type(value) is not str or not value.strip() for value in identities):
            raise ValueError("identity")
    except Exception:
        raise _error("RuntimeHandlerProtocolMismatch", "Handler must implement the complete exact runtime protocol.") from None
    if identities[0] != profile.profile_id:
        raise _error("RuntimeHandlerProfileMismatch", "Handler Profile ID differs from registration.")
    if identities[1] != profile.exact_asset_class_path:
        raise _error("RuntimeHandlerClassMismatch", "Handler exact class differs from registration.")
    try:
        requirements = handler.required_primitives()
    except Exception:
        raise _error("RuntimeHandlerPrimitiveMismatch", "Handler primitive declaration failed.") from None
    if (type(requirements) is not tuple
            or any(type(item) is not PrimitiveRequirement for item in requirements)
            or requirements != profile.required_primitives):
        raise _error("RuntimeHandlerPrimitiveMismatch", "Handler must declare the exact ordered Profile primitive tuple.")
