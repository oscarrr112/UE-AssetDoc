"""Extract of engine and plugin content (outside /Game), which is read-only.

Such an asset extracts as the same document a /Game asset of its class would give, with
its own path as Target, so an AI can study it or re-target it to a /Game path and apply it
with Action Create. Profiles admit only /Game targets; while an Extract reads a read-only
asset, its exact path is admitted too (``admits_read_only``), so the profile extracts and
canonicalizes it as it would a /Game asset (self references keep the real path). Apply,
Validate and Diff never admit it: the editor refuses them with Meta.ReadOnlyTarget.
Inspect of such an asset reads it the same way (``inspect_live``), so an AI can study
engine templates and plugin types through the profile's details.
"""
from __future__ import annotations

from contextlib import contextmanager
import re
from contextvars import ContextVar
from typing import Callable, Iterator

GAME_ROOT = "/Game/"
_ADMITTED: ContextVar[str | None] = ContextVar("asset_document_read_only_target", default=None)


_LIVE_OBJECT_PATH = re.compile(r"/[^/.]+/[^.]+\.[^/.]+")


def is_read_only_target(target: object) -> bool:
    return type(target) is str and target.startswith("/") and not target.startswith(GAME_ROOT)


def admits_read_only(value: object) -> bool:
    """True for the read-only target an Extract is reading right now (profile target checks)."""
    admitted = _ADMITTED.get()
    return admitted is not None and type(value) is str and value == admitted


@contextmanager
def _admitting(target: str | None) -> Iterator[None]:
    if not is_read_only_target(target):
        yield
        return
    token = _ADMITTED.set(target)
    try:
        yield
    finally:
        _ADMITTED.reset(token)


def extract_document(
    target: str,
    extract: Callable[[], dict],
    canonicalize: Callable[[dict], dict],
    *,
    read_only: bool,
) -> dict:
    """The canonical extract of ``target``; ``read_only`` (the Extract operation) admits an
    engine or plugin target while it is read."""
    with _admitting(target if read_only else None):
        return canonicalize(extract())


def is_live_inspect_path(value: object) -> bool:
    """A canonical asset object path Inspect may read: /Game or engine/plugin content."""
    return (type(value) is str and _LIVE_OBJECT_PATH.fullmatch(value) is not None
            and not value.startswith("/Script/"))


def inspect_live(object_path: str, inspect: Callable[[], dict]) -> dict:
    """Inspect of a live asset; an engine or plugin asset is admitted while it is read."""
    with _admitting(object_path.rsplit(".", 1)[0]):
        return inspect()
