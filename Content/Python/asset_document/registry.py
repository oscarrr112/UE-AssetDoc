# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from dataclasses import dataclass, field
from types import MappingProxyType
from typing import Mapping

from asset_document.diagnostics import contract_error
from asset_document.dto import PrimitiveRequirement
from asset_document.meta import ProfileMeta
from asset_document.profile_runtime import RuntimeProfileRegistration, FrozenRuntimeProfileRegistration


def _invalid_registry_type(message: str):
    return contract_error("/Registry", "DTO.InvalidRepresentationType", message)


@dataclass(frozen=True, slots=True)
class FrozenProfileRegistry:
    profiles_by_class: Mapping[str, ProfileMeta]
    runtime_by_class: Mapping[str, FrozenRuntimeProfileRegistration] = field(default_factory=dict)

    def __post_init__(self) -> None:
        if not isinstance(self.profiles_by_class, Mapping):
            raise _invalid_registry_type("Registry input must be a mapping.")
        if not self.profiles_by_class:
            raise contract_error(
                "/Registry",
                "Registry.Empty",
                "A frozen registry must contain at least one Profile.",
            )
        copied: dict[str, ProfileMeta] = {}
        profile_ids: set[str] = set()
        for key, profile in self.profiles_by_class.items():
            if type(key) is not str or type(profile) is not ProfileMeta:
                raise _invalid_registry_type(
                    "Registry keys and values must be exact strings and ProfileMeta values."
                )
            if key != profile.exact_asset_class_path:
                raise _invalid_registry_type(
                    "Every registry key must equal its Profile exact asset class path."
                )
            if key in copied:
                raise contract_error(
                    "/Registry",
                    "Registry.DuplicateExactClassPath",
                    "Exact asset class paths must be unique.",
                )
            if profile.profile_id in profile_ids:
                raise contract_error(
                    "/Registry",
                    "Registry.DuplicateProfileId",
                    "Profile IDs must be unique.",
                )
            copied[key] = profile
            profile_ids.add(profile.profile_id)
        ordered = {key: copied[key] for key in sorted(copied)}
        if not isinstance(self.runtime_by_class, Mapping):
            raise _invalid_registry_type("Runtime bindings must be a mapping.")
        runtime = {}
        for key, binding in self.runtime_by_class.items():
            if (type(key) is not str or type(binding) is not FrozenRuntimeProfileRegistration
                    or key not in ordered or binding.profile_meta != ordered[key]):
                raise contract_error("/Registry", "Registry.RuntimeRegistrationInvalid", "Runtime binding must match registered exact metadata.")
            runtime[key] = binding
        object.__setattr__(self, "profiles_by_class", MappingProxyType(ordered))
        object.__setattr__(self, "runtime_by_class", MappingProxyType({key: runtime[key] for key in sorted(runtime)}))

    def resolve_exact(self, exact_asset_class_path: str) -> ProfileMeta:
        if type(exact_asset_class_path) is not str:
            raise _invalid_registry_type("Exact asset class lookup must be a string.")
        try:
            return self.profiles_by_class[exact_asset_class_path]
        except KeyError:
            raise contract_error(
                f"/Registry/{exact_asset_class_path}",
                "Registry.ProfileNotFound",
                "No exact Profile registration exists for the requested class.",
            ) from None

    def resolve_runtime_exact(self, exact_asset_class_path: str) -> FrozenRuntimeProfileRegistration | None:
        if type(exact_asset_class_path) is not str:
            raise _invalid_registry_type("Exact asset class lookup must be a string.")
        return self.runtime_by_class.get(exact_asset_class_path)

    def runtime_registrations(self) -> tuple[FrozenRuntimeProfileRegistration, ...]:
        return tuple(self.runtime_by_class.values())

    def profiles(self) -> tuple[ProfileMeta, ...]:
        return tuple(self.profiles_by_class[key] for key in sorted(self.profiles_by_class))

    def required_primitives(self) -> tuple[PrimitiveRequirement, ...]:
        return tuple(
            sorted(
                {
                    requirement
                    for profile in self.profiles_by_class.values()
                    for requirement in profile.required_primitives
                }
            )
        )


class ProfileRegistryBuilder:
    def __init__(self) -> None:
        self._profiles_by_id: dict[str, ProfileMeta] = {}
        self._profiles_by_class: dict[str, ProfileMeta] = {}
        self._snapshot: FrozenProfileRegistry | None = None
        self._runtime_by_class: dict[str, RuntimeProfileRegistration] = {}
        self._freeze_error: Exception | None = None
        self._freezing = False

    def register(self, profile: ProfileMeta) -> None:
        if self._snapshot is not None or self._freezing or self._freeze_error is not None:
            raise contract_error(
                "/Registry", "Registry.Frozen", "A frozen registry cannot be modified."
            )
        if type(profile) is not ProfileMeta:
            raise _invalid_registry_type("Only exact ProfileMeta values can be registered.")
        if profile.profile_id in self._profiles_by_id:
            raise contract_error(
                "/Registry", "Registry.DuplicateProfileId", "Profile IDs must be unique."
            )
        if profile.exact_asset_class_path in self._profiles_by_class:
            raise contract_error(
                "/Registry",
                "Registry.DuplicateExactClassPath",
                "Exact asset class paths must be unique.",
            )
        self._profiles_by_id[profile.profile_id] = profile
        self._profiles_by_class[profile.exact_asset_class_path] = profile

    def register_runtime(self, registration: RuntimeProfileRegistration) -> None:
        if self._snapshot is not None or self._freezing or self._freeze_error is not None:
            raise contract_error("/Registry", "Registry.Frozen", "A frozen registry cannot be modified.")
        if type(registration) is not RuntimeProfileRegistration:
            raise contract_error("/Registry", "Registry.RuntimeRegistrationInvalid", "An exact RuntimeProfileRegistration is required.")
        # Metadata validation and collision checks complete before either map changes.
        self.register(registration.profile_meta)
        self._runtime_by_class[registration.profile_meta.exact_asset_class_path] = registration

    def freeze(self) -> FrozenProfileRegistry:
        if self._snapshot is not None:
            return self._snapshot
        if self._freeze_error is not None:
            raise self._freeze_error
        if self._freezing:
            raise contract_error("/Registry", "Registry.Frozen", "Registry freeze is already in progress.")
        if not self._profiles_by_class:
            raise contract_error(
                "/Registry", "Registry.Empty", "At least one Profile is required before freeze."
            )
        ordered = {
            key: self._profiles_by_class[key]
            for key in sorted(self._profiles_by_class)
        }
        # A failed attempt is terminal: retrying freeze never repeats a factory.
        self._freezing = True
        try:
            runtime = {}
            for key in sorted(self._runtime_by_class):
                registration = self._runtime_by_class[key]
                try:
                    handler = registration.handler_factory()
                except Exception:
                    raise contract_error("/Registry", "Registry.RuntimeFactoryFailed", "Runtime handler factory failed.") from None
                runtime[key] = FrozenRuntimeProfileRegistration(
                    registration.profile_meta, handler, registration.target_policy,
                    registration.target_lifecycle,
                )
            snapshot = FrozenProfileRegistry(ordered, runtime)
        except Exception as error:
            self._freeze_error = error
            raise
        finally:
            self._freezing = False
        self._snapshot = snapshot
        return snapshot
