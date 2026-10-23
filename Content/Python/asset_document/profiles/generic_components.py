# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Per-class property components for the generic (unregistered-class) Profile.

A component declares the non-edit properties a class's document authors and the
value codecs that give them an AI-friendly document form. Adding a class here is
Python-only: the Editor provider admits declared names generically.
"""

from __future__ import annotations

from types import MappingProxyType
from typing import Mapping

from asset_document.diagnostics import contract_error
from asset_document.profiles.curve_float import CURVE_FLOAT_PROPERTIES
from asset_document.regions.property_semantics import GenericPropertyComponent


GENERIC_PROPERTY_COMPONENTS: tuple[GenericPropertyComponent, ...] = (CURVE_FLOAT_PROPERTIES,)


def _index(components: tuple[GenericPropertyComponent, ...]) -> Mapping[str, GenericPropertyComponent]:
    index: dict[str, GenericPropertyComponent] = {}
    for component in components:
        if type(component) is not GenericPropertyComponent or component.exact_class_path in index:
            raise contract_error(
                "/Class",
                "Generic.InvalidPropertyAdmission",
                "generic property components must be unique per exact class",
            )
        index[component.exact_class_path] = component
    return MappingProxyType(index)


_BY_CLASS = _index(GENERIC_PROPERTY_COMPONENTS)


def generic_property_component(exact_class_path: str) -> GenericPropertyComponent | None:
    return _BY_CLASS.get(exact_class_path)
