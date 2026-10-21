"""Pin-name spellings an author may use for a live pin (pure).

A pin is named by its exact name (case-insensitive) first.  These spellings are
the fallback, accepted only when they pick one pin of the node:

- a user-defined struct member's pin (``LaunchVelocity_16_D0706D44...``, the
  member's GUID name) by the member's name (``LaunchVelocity``), as DataTable
  rows and struct defaults name members;
- spaces and underscores are ignored (``AsBP_SampleCharacterBase`` names the cast
  output ``AsBP Sample Character Base``);
- the pin's editor display name (``Row Found`` / ``Out Row`` of Get Data Table
  Row, whose pins are ``then`` / ``ReturnValue``);
- ``Result`` names a cast node's ``As<Class>`` output.
"""
from __future__ import annotations

import re

# "<Member>_<index>_<32 hex>" is a user-defined struct member's GUID name; split pins of such a member
# carry it in the middle ("Hit_LaunchVelocity_16_D0706D44..._X").
_STRUCT_MEMBER_GUID = re.compile(r"_\d+_[0-9a-f]{32}", re.IGNORECASE)
_CAST_KINDS = frozenset({"DynamicCast", "ClassDynamicCast"})
CAST_RESULT_ALIAS = "Result"
# Display names of pins a node kind names apart from their pin names (pin_key form).
_KIND_PIN_ALIASES = {"GetDataTableRow": {"rowfound": "then", "outrow": "returnvalue"}}


def strip_member_guids(name: str) -> str:
    """``LaunchVelocity_16_D0706D44...`` -> ``LaunchVelocity`` (a user struct member's friendly name)."""
    return _STRUCT_MEMBER_GUID.sub("", name)


def pin_key(name: str) -> str:
    """Loose identity of a pin name: GUID suffixes, case, spaces and underscores ignored."""
    return re.sub(r"[\s_]+", "", strip_member_guids(name)).casefold()


def _kind(action: str | None, class_path: str | None) -> str:
    if action:
        parts = action.split(":")
        if len(parts) >= 2:
            return parts[1].partition("#")[0].partition("@")[0]
    if class_path:
        return class_path.rsplit(".", 1)[-1].removeprefix("K2Node_")
    return ""


def node_pin_key(name: str, *, action: str | None = None, class_path: str | None = None) -> str:
    """``pin_key`` plus the node kind's own spellings: a cast's ``Result`` / ``As<Class>`` output,
    Get Data Table Row's ``RowFound`` / ``OutRow``."""
    key = pin_key(name)
    kind = _kind(action, class_path)
    if kind in _CAST_KINDS and (key == CAST_RESULT_ALIAS.casefold() or key.startswith("as")):
        return "#castresult"
    return _KIND_PIN_ALIASES.get(kind, {}).get(key, key)


def resolve_pin_name(wanted: str, pins, *, action: str | None = None, class_path: str | None = None):
    """The single pin of ``pins`` (objects with ``name`` and optional ``display_name``) the loose
    spelling ``wanted`` names, else None."""
    key = node_pin_key(wanted, action=action, class_path=class_path)
    found = [pin for pin in pins
             if node_pin_key(pin.name, action=action, class_path=class_path) == key
             or (getattr(pin, "display_name", "") and pin_key(pin.display_name) == pin_key(wanted))]
    if key == "#castresult":
        # Only the cast's object/class output answers to Result / As<Class>.
        found = [pin for pin in found if getattr(pin, "direction", "output") == "output"]
    return found[0] if len(found) == 1 else None
