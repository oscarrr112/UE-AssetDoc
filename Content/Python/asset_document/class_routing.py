# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""A request for an asset class that has no profile of its own, read by the profile of an ancestor class.

A Blueprint subclass without its own profile (an Editor Utility Blueprint, a Gameplay Ability Blueprint, ...) is read
and edited by the profile of its nearest Blueprint ancestor that has one. The native route names that ancestor in the
request's ``requested_profile_class_path``; the request keeps the asset's exact class. While such a request runs, the
profile's documents carry the asset's exact class as ``Class`` (``document_class``) and accept it (``accepts``).
"""
from __future__ import annotations

from contextlib import contextmanager
from contextvars import ContextVar
from typing import Iterator

__all__ = ("routed", "routed_class", "document_class", "accepts")

# (asset class, profile class) of the running request, or None when the request's class has its own profile.
_ROUTED: ContextVar[tuple[str, str] | None] = ContextVar("asset_document_routed_class", default=None)


@contextmanager
def routed(asset_class: str, profile_class: str) -> Iterator[None]:
    token = _ROUTED.set((asset_class, profile_class))
    try:
        yield
    finally:
        _ROUTED.reset(token)


def routed_class(profile_class: str) -> str | None:
    """The asset class of the running request when it is routed to ``profile_class``'s profile, else None."""
    value = _ROUTED.get()
    return value[0] if value is not None and value[1] == profile_class else None


def document_class(profile_class: str) -> str:
    """The ``Class`` a document of ``profile_class``'s profile carries in the running request."""
    return routed_class(profile_class) or profile_class


def accepts(profile_class: str, value) -> bool:
    """``value`` is a ``Class`` the profile of ``profile_class`` reads in the running request."""
    return type(value) is str and value == document_class(profile_class)
