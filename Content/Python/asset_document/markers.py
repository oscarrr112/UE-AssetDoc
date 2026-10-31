# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Unrepresented content: what a read cannot put in a document, shown where it stands.

A *marked value* is an object holding ``Unsupported`` (a string with the reason) and optionally ``Text`` (the
engine's export text, read only). It stands where the value would be: a property, a struct member, a map value, a
shape list. A *marked element* is a list or map entry with its identity fields plus ``Unsupported``; its unreadable
content is left out.

Apply never writes a marker:

* a marker left as extracted keeps the current value;
* a marked value whose member is omitted is kept (a codec never resets what it cannot write);
* a marker whose ``Unsupported`` or ``Text`` differs from the current asset's is refused with
  ``<Profile>.UnrepresentedValue``: it is never silently ignored.

Each marker a read emits also gives one warning ``<Profile>.Unrepresented`` at its pointer (``warn``).

``Text`` names objects by path. A path into the asset itself depends on where the asset is: the asset after a move,
Validate's copy of it (``/Engine/Transient.<Name>_<N>``, or a staged package under
``/Temp/AssetDocument/ValidateTrial/``) read the same content with another path. Markers therefore compare with
those paths named as the request's target names them (``rebase_text``): every marker check of an apply
(``require_unchanged``, under ``target``) and the Diff / Verify comparison (``rebase`` of both documents). A path
into any other asset or a native type (``/Script/Engine.Actor:Tags``) is compared as it is. The editor's marker
checks use the same rule (AssetDocumentEditorMarkerText.h).
"""
from __future__ import annotations

from contextlib import contextmanager
from contextvars import ContextVar
import re
from typing import Iterator

from asset_document.diagnostics import contract_error, read_warning

__all__ = ("KEY", "TEXT", "is_marker", "marker", "warn", "warn_all", "require_unchanged", "keep", "target",
           "current_target", "rebase_text", "rebase", "is_former", "adopt_former")

KEY = "Unsupported"
TEXT = "Text"


def is_marker(value) -> bool:
    """An object whose ``Unsupported`` is a string (where the type there has no member of that name)."""
    return type(value) is dict and type(value.get(KEY)) is str


# The marker a read gave a whole optional, map, array or set whose type had no document form then. The editor now reads
# such values; a document written before still holds the marker there.
_FORMER_REASON = re.compile(r"^T(?:Optional|Map|Array|Set) has no document form; AssetDoc keeps the value\.$")


def is_former(authored, live) -> bool:
    """``authored`` is the marker of a value read before its type had a document form, and the asset now holds that
    value in its form (``live``, not a marker): the marker keeps that value, as it did when it was read."""
    return (is_marker(authored) and set(authored) <= {KEY, TEXT} and type(authored.get(TEXT, "")) is str
            and _FORMER_REASON.match(authored[KEY]) is not None
            and type(live) in (dict, list, str, int, float, bool) and not is_marker(live))


def adopt_former(authored, live):
    """``authored`` with every marker that ``is_former`` at the same place of ``live`` replaced by the value it keeps.
    Objects pair by key, lists of the same length by index."""
    if is_former(authored, live):
        return live
    if type(authored) is dict and type(live) is dict:
        return {name: adopt_former(item, live[name]) if name in live else item for name, item in authored.items()}
    if type(authored) is list and type(live) is list and len(authored) == len(live):
        return [adopt_former(item, other) for item, other in zip(authored, live)]
    return authored


def holds_marker(value) -> bool:
    """A marker, or a value with a marker at any depth (a struct with a member that has no document form, a list of
    such structs, ...)."""
    if is_marker(value):
        return True
    if type(value) is dict:
        return any(holds_marker(item) for item in value.values())
    if type(value) is list:
        return any(holds_marker(item) for item in value)
    return False


def marker(reason: str, text: str | None = None, **identity) -> dict:
    value = {**identity, KEY: reason}
    if text is not None:
        value[TEXT] = text
    return value


def warn(path: str, prefix: str, value: dict) -> None:
    """The read warning of one marker."""
    read_warning(path, f"{prefix}.Unrepresented", value[KEY])


def warn_all(value, prefix: str, path: str = "") -> None:
    """Read warnings for every marker under ``value`` (JSON pointer ``path``); a marker's own content is not walked."""
    if is_marker(value):
        warn(path or "/", prefix, value)
    elif type(value) is dict:
        for name, item in value.items():
            warn_all(item, prefix, f"{path}/{_escape(str(name))}")
    elif type(value) is list:
        for index, item in enumerate(value):
            warn_all(item, prefix, f"{path}/{index}")


def require_unchanged(path: str, authored, live, prefix: str) -> None:
    """Refuses an authored marker that is not the current asset's marker at the same place (edited reason or text,
    or a marker where the asset holds a readable value)."""
    if not is_marker(authored) or is_former(authored, live):
        return
    if not is_marker(live):
        raise contract_error(path, f"{prefix}.UnrepresentedValue", "This place holds a marker the asset does not have "
                             "here; a marker cannot be written. Give a value, or omit it to keep the current one.")
    asset = _TARGET.get()
    if authored.get(KEY) != live.get(KEY) \
            or rebase_text(authored.get(TEXT), asset) != rebase_text(live.get(TEXT), asset):
        raise contract_error(path, f"{prefix}.UnrepresentedValue", f"The marker was edited; AssetDoc cannot write this "
                             f"content ({live[KEY]}). Leave the marker as extracted, or omit it to keep the content.")


def keep(authored, live):
    """``authored`` with every marker replaced by the current value it stands for (``live`` is the current asset read
    with the same reader, so a marker there means "keep what is stored").

    List items are paired by identity: by the first of ``Name``, ``Id``, ``Key``, ``NodeGuid``, ``Guid`` that every
    item of both lists holds as a distinct non-empty string, so a reordered list, or one that lost or gained items,
    keeps each marker's own stored value. An item with no current counterpart keeps its markers (a marker there has
    nothing to stand for, and the apply's marker check refuses it). Lists without such a field pair by index."""
    if is_marker(authored):
        return live
    if type(authored) is dict and type(live) is dict:
        return {name: keep(item, live.get(name)) for name, item in authored.items()}
    if type(authored) is list and type(live) is list:
        field = _identity_field(authored, live)
        if field is None:
            return [keep(item, live[index]) if index < len(live) else item for index, item in enumerate(authored)]
        current = {item[field]: item for item in live}
        return [keep(item, current[item[field]]) if item[field] in current else item for item in authored]
    return authored


_IDENTITY_FIELDS = ("Name", "Id", "Key", "NodeGuid", "Guid")


def _identity_field(authored: list, live: list) -> str | None:
    for items in (authored, live):
        if not all(type(item) is dict for item in items):
            return None
    for name in _IDENTITY_FIELDS:
        if all(type(item.get(name)) is str and item.get(name) for item in authored + live) and all(
                len({item[name] for item in items}) == len(items) for items in (authored, live)):
            return name
    return None


_TARGET: ContextVar[str | None] = ContextVar("asset_document_marker_target", default=None)


@contextmanager
def target(asset_path: str | None) -> Iterator[None]:
    """The request's target asset (``/Game/A/B.B``) while a request runs: ``require_unchanged`` compares marker
    text with paths into the asset named as this path names them. Without one, text compares exactly."""
    token = _TARGET.set(asset_path if type(asset_path) is str and asset_path else None)
    try:
        yield
    finally:
        _TARGET.reset(token)


def current_target() -> str | None:
    """The asset path set by the innermost ``target``, or None."""
    return _TARGET.get()


# An object path "<package>.<object>" (a subobject continues with ":"): package and object name.
_OBJECT_PATH = re.compile(r"(/[^\s'\"(),:]+)\.([^\s'\"(),:/.]+)")
_NUMBER_SUFFIX = re.compile(r"(?:_\d+)+$")
_TRANSIENT_PACKAGE = "/Engine/Transient"
_STAGED_TRIAL_ROOT = "/Temp/AssetDocument/ValidateTrial/"


def _own_object(package: str, name: str, asset_name: str) -> str | None:
    """The object name ``<package>.<name>`` has in the asset when it is an object of the asset ``asset_name`` (the
    asset's own package in any folder: the asset where it is, or before a move; Validate's copy of it), else None.

    Validate's transient copy is the top-level object ``<asset name>`` or ``<asset name>_<N>`` in
    ``/Engine/Transient`` (a unique name of the same base); a staged copy keeps the asset's name in a package of its
    own under ``/Temp/AssetDocument/ValidateTrial/``."""
    if package.startswith("/Script/"):
        return None
    if package.rsplit("/", 1)[-1] == asset_name:
        return name
    if package == _TRANSIENT_PACKAGE:
        base = _NUMBER_SUFFIX.sub("", asset_name) or asset_name
        return asset_name if re.fullmatch(re.escape(base) + r"(?:_\d+)*", name) else None
    if package.startswith(_STAGED_TRIAL_ROOT) and name == asset_name:
        return asset_name
    return None


def rebase_text(text, asset_path: str | None):
    """Export text with every path into the asset ``asset_path`` (``/Game/A/B.B``) named as that path names it:
    ``/Game/Old/B.B:Inner`` and ``/Engine/Transient.B_1:Inner`` read ``/Game/A/B.B:Inner``. Paths into other
    assets and native types stay as they are; a non-string, or no ``asset_path``, is returned unchanged."""
    if type(text) is not str or type(asset_path) is not str or "." not in asset_path:
        return text
    package = asset_path.rsplit(".", 1)[0]
    asset_name = asset_path.rsplit(".", 1)[1].split(":", 1)[0]
    if not asset_name:
        return text

    def named(match):
        own = _own_object(match.group(1), match.group(2), asset_name)
        return match.group(0) if own is None else f"{package}.{own}"
    return _OBJECT_PATH.sub(named, text)


def rebase(value, asset_path: str | None):
    """``value`` (a document or part of one) with each marker's ``Text`` rebased to ``asset_path`` (``rebase_text``),
    for comparing an authored document with the asset's read. ``value`` is not changed; parts without a rebased
    marker are shared."""
    if is_marker(value):
        text = value.get(TEXT)
        rebased = rebase_text(text, asset_path)
        return value if rebased == text else {**value, TEXT: rebased}
    if type(value) is dict:
        items = {name: rebase(item, asset_path) for name, item in value.items()}
        return value if all(items[name] is value[name] for name in value) else items
    if type(value) is list:
        items = [rebase(item, asset_path) for item in value]
        return value if all(new is old for new, old in zip(items, value)) else items
    return value


def _escape(name: str) -> str:
    return name.replace("~", "~0").replace("/", "~1")
