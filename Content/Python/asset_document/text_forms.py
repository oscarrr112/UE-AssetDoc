# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""FText in documents: the source string, or a localization form that keeps what the string alone loses.

A text reads as its source string (``"Start"``), or in the engine's text form when the string alone would lose its
identity: ``LOCTABLE("UI", "Start")`` for a String Table reference, ``INVTEXT("x")`` for a culture-invariant text,
``NSLOCTEXT("ns", "key", "source")`` for a text whose source looks like one of these forms. A written source string
edits the text as the Details text box does: an unchanged string keeps the text, a changed one keeps its key.

A document written before a text was read in its localization form holds the text's display string instead. That
string still names the same text: ``same_text`` compares it with the form, and the editor keeps the text when it is
written. The display string of a String Table reference is only known to the editor; reads hand it over
(``learn``) when they write such a form.
"""
from __future__ import annotations

import re

__all__ = ("is_form", "learn", "display", "same_text", "adopt")

_FORM = re.compile(r"^\s*(NSLOCTEXT|LOCTEXT|INVTEXT|LOCTABLE|LOCGEN_[A-Z_]+)\s*\(")
_LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')
_DISPLAYS: dict[str, str] = {}
_MAXIMUM = 16384


def is_form(value: object) -> bool:
    """A string in one of the engine's text forms (not a plain source string)."""
    return type(value) is str and _FORM.match(value) is not None


def learn(displays) -> None:
    """Records ``{form: display string}`` as a read reported it."""
    if type(displays) is not dict:
        return
    if len(_DISPLAYS) + len(displays) > _MAXIMUM:
        _DISPLAYS.clear()
    for form, shown in displays.items():
        if type(form) is str and type(shown) is str:
            _DISPLAYS[form] = shown


def _unescape(literal: str) -> str:
    return re.sub(r"\\(.)", lambda match: match.group(1), literal)


def display(form: str) -> str | None:
    """The display string of a text form: as a read reported it, else the source string the form states
    (``INVTEXT``, ``NSLOCTEXT``, ``LOCTEXT``). None when unknown."""
    if form in _DISPLAYS:
        return _DISPLAYS[form]
    match = _FORM.match(form)
    if match is None:
        return None
    literals = [_unescape(item) for item in _LITERAL.findall(form[match.end():])]
    kind = match.group(1)
    if kind == "INVTEXT" and len(literals) == 1:
        return literals[0]
    if kind == "NSLOCTEXT" and len(literals) == 3:
        return literals[2]
    if kind == "LOCTEXT" and len(literals) == 2:
        return literals[1]
    return None


def same_text(authored: object, live: object) -> bool:
    """``authored`` names the text ``live`` reads as: the same string, or a plain string equal to the display
    string of the form ``live`` is written in."""
    if authored == live:
        return True
    if type(authored) is not str or not is_form(live) or is_form(authored):
        return False
    shown = display(live)
    return shown is not None and shown == authored


def adopt(authored, live):
    """``authored`` with every string that names the text ``live`` holds at the same place (``same_text``) replaced by
    ``live``'s form, for a profile that compares values exactly. Objects pair by key, lists by index."""
    if type(authored) is str:
        return live if type(live) is str and authored != live and same_text(authored, live) else authored
    if type(authored) is dict and type(live) is dict:
        return {key: adopt(value, live[key]) if key in live else value for key, value in authored.items()}
    if type(authored) in (list, tuple) and type(live) in (list, tuple):
        items = [adopt(item, live[index]) if index < len(live) else item for index, item in enumerate(authored)]
        return tuple(items) if type(authored) is tuple else items
    return authored
