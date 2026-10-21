"""Action-key aliases and normalization (contract section 6).

C++ only accepts canonical keys ``<Tag>:<Kind>[:<Owner>][:<Field>][@<Binding>][#<Disambiguator>]``.
Aliases exist purely for AI convenience and are resolved here, before any
primitive call.  Normalization is pure and deterministic.
"""
from __future__ import annotations

import re

from asset_document.diagnostics import contract_error


_KSL = "/Script/Engine.KismetSystemLibrary"
_ACTOR = "/Script/Engine.Actor"
_MACROS = "/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"


def _call(owner: str, name: str) -> str:
    return f"K2:CallFunction:{owner}:{name}"


def _event(name: str, owner: str = _ACTOR) -> str:
    return f"K2:Event:{owner}:{name}"


def _macro(name: str) -> str:
    return f"K2:MacroInstance:{_MACROS}:{name}"


def _operator(name: str) -> str:
    # Type-promoted operators are addressed by operator name only (contract section 6).
    return f"K2:PromotableOperator:{name}"


# Order matters: the first alias of a canonical key is the one extraction prefers.
ALIASES: dict[str, str] = {
    "K2:Branch": "K2:IfThenElse",
    "K2:If": "K2:IfThenElse",
    "K2:Sequence": "K2:ExecutionSequence",
    "K2:PrintString": _call(_KSL, "PrintString"),
    "K2:PrintText": _call(_KSL, "PrintText"),
    "K2:Delay": _call(_KSL, "Delay"),
    "K2:IsValidObject": _call(_KSL, "IsValid"),
    "K2:BeginPlay": _event("ReceiveBeginPlay"),
    "K2:Tick": _event("ReceiveTick"),
    "K2:EndPlay": _event("ReceiveEndPlay"),
    "K2:ActorBeginOverlap": _event("ReceiveActorBeginOverlap"),
    "K2:ActorEndOverlap": _event("ReceiveActorEndOverlap"),
    "K2:AnyDamage": _event("ReceiveAnyDamage"),
    "K2:Destroyed": _event("ReceiveDestroyed"),
    "K2:ForLoop": _macro("ForLoop"),
    "K2:ForEachLoop": _macro("ForEachLoop"),
    "K2:ForEachLoopWithBreak": _macro("ForEachLoopWithBreak"),
    "K2:WhileLoop": _macro("WhileLoop"),
    "K2:DoOnce": _macro("DoOnce"),
    "K2:DoN": _macro("DoN"),
    "K2:FlipFlop": _macro("FlipFlop"),
    "K2:Gate": _macro("Gate"),
    "K2:IsValid": _macro("IsValid"),
    "K2:Add": _operator("Add"),
    "K2:Subtract": _operator("Subtract"),
    "K2:Multiply": _operator("Multiply"),
    "K2:Divide": _operator("Divide"),
    "K2:Greater": _operator("Greater"),
    "K2:GreaterEqual": _operator("GreaterEqual"),
    "K2:Less": _operator("Less"),
    "K2:LessEqual": _operator("LessEqual"),
    "K2:Equal": _operator("EqualEqual"),
    "K2:NotEqual": _operator("NotEqual"),
    "K2:Cast": "K2:DynamicCast",
    "K2:Get": "K2:VariableGet",
    "K2:Set": "K2:VariableSet",
    "K2:Return": "K2:FunctionResult",
    "K2:Reroute": "K2:Knot",
    # A comment box (the editor's "Add Comment"): text in Comment, Position, Size, colour in Properties.
    "K2:Comment": "K2:EdGraphNode_Comment",
}
_ALIASES_FOLDED = {key.casefold(): value for key, value in ALIASES.items()}
# Canonical key -> preferred alias.  Kind-only targets ("K2:DynamicCast", "K2:VariableGet", ...)
# never occur as live keys and are not reverse-mapped.
PREFERRED_ALIASES: dict[str, str] = {}
for _alias, _canonical in ALIASES.items():
    if _canonical.count(":") >= 2 or _canonical in ("K2:IfThenElse", "K2:ExecutionSequence", "K2:Knot",
                                                     "K2:EdGraphNode_Comment"):
        PREFERRED_ALIASES.setdefault(_canonical, _alias)
_KML = "/Script/Engine.KismetMathLibrary"
# Short names of common math / logic nodes, accepted on input only (extraction keeps their canonical
# keys): the operator family is named by its function name ("K2:EqualEqual" as "K2:Equal") and the
# Kismet math functions by their names without the type suffix the library adds.
_INPUT_ALIASES: dict[str, str] = {
    "K2:EqualEqual": _operator("EqualEqual"),
    "K2:EqualEnum": "K2:EnumEquality",
    "K2:NotEqualEnum": "K2:EnumInequality",
    **{f"K2:Select{kind}": _call(_KML, f"Select{kind}") for kind in (
        "Float", "Int", "String", "Name", "Text", "Vector", "Rotator", "Transform", "Color", "Object", "Class")},
    "K2:And": _call(_KML, "BooleanAND"),
    "K2:Or": _call(_KML, "BooleanOR"),
    "K2:Xor": _call(_KML, "BooleanXOR"),
    "K2:Nand": _call(_KML, "BooleanNAND"),
    "K2:Nor": _call(_KML, "BooleanNOR"),
    "K2:Not": _call(_KML, "Not_PreBool"),
    "K2:Abs": _call(_KML, "Abs"),
    "K2:AbsInt": _call(_KML, "Abs_Int"),
    "K2:Min": _call(_KML, "FMin"),
    "K2:Max": _call(_KML, "FMax"),
    "K2:MinInt": _call(_KML, "Min"),
    "K2:MaxInt": _call(_KML, "Max"),
    "K2:Clamp": _call(_KML, "FClamp"),
    "K2:ClampInt": _call(_KML, "Clamp"),
    "K2:Lerp": _call(_KML, "Lerp"),
    "K2:Percent": _call(_KML, "Percent_FloatFloat"),
    "K2:PercentInt": _call(_KML, "Percent_IntInt"),
    "K2:NearlyEqual": _call(_KML, "NearlyEqual_FloatFloat"),
    "K2:InRange": _call(_KML, "InRange_FloatFloat"),
}
ALIASES.update(_INPUT_ALIASES)
_ALIASES_FOLDED = {key.casefold(): value for key, value in ALIASES.items()}
# Prefix aliases keep their tail, e.g. "K2:Get:Health" -> "K2:VariableGet:Self:Health".
_PREFIX_ALIASES = {"k2:get": "K2:VariableGet", "k2:set": "K2:VariableSet", "k2:cast": "K2:DynamicCast"}
_SELF_MEMBER_KINDS = frozenset({"VariableGet", "VariableSet", "CallFunction", "Event", *(
    "CallDelegate", "AddDelegate", "RemoveDelegate", "ClearDelegate", "AssignDelegate", "CustomEvent")})
# Event dispatcher nodes (Call / Bind / Unbind / Unbind all / Assign, and the dispatcher's "Event",
# a custom event keyed "K2:CustomEvent:<Owner>:<Dispatcher>") accept "K2:<Kind>:<Dispatcher>" for
# a dispatcher of the asset's own class.
DISPATCHER_KINDS = frozenset({"CallDelegate", "AddDelegate", "RemoveDelegate", "ClearDelegate", "AssignDelegate",
                              "CustomEvent"})
_KEY = re.compile(r"[A-Za-z][A-Za-z0-9]*:[A-Za-z_][A-Za-z0-9_]*(:[^:@#\s][^:@#]*)*(@[^:@#\s]+)?(#[0-9A-Fa-f]{8})?\Z")

# Legacy Body ``Class`` paths handled by the K2 legacy node form.
LEGACY_K2_CLASS_PREFIX = "/Script/BlueprintGraph.K2Node_"


def _error(path: str, message: str):
    return contract_error(path, "Graph.InvalidAction", message)


def split_disambiguator(key: str) -> tuple[str, str | None]:
    base, _, suffix = key.partition("#")
    return base, (suffix.upper() if suffix else None)


def normalize_action(key: object, *, self_class: str | None = None, path: str = "/Action") -> str:
    """Return the canonical action key for an authored key or alias.

    ``self_class`` (the asset's generated class path, e.g.
    ``/Game/BP_Foo.BP_Foo_C``) replaces a ``Self`` owner segment.
    """
    if type(key) is not str or not key.strip():
        raise _error(path, "Action must be a non-empty string such as \"K2:IfThenElse\".")
    text = key.strip()
    if text.count(":") >= 2 and not text.endswith(":"):
        # Trailing white space past the kind belongs to a member's name ("Follow Player " of BP_FluidSim_01).
        text = key.lstrip()
    tag, sep, rest = text.partition(":")
    if not sep:
        raise _error(path, f"Action '{text}' needs a schema tag prefix, for example \"K2:{text}\".")
    if rest.startswith("K2Node_"):
        rest = rest[len("K2Node_"):]
    text = f"{tag}:{rest}"
    folded = text.casefold()
    if folded in _ALIASES_FOLDED:
        text = _ALIASES_FOLDED[folded]
    else:
        head, sep, tail = text.partition(":")
        kind, sep2, tail2 = tail.partition(":")
        prefix = f"{head}:{kind}".casefold()
        if sep2 and prefix in _PREFIX_ALIASES:
            text = f"{_PREFIX_ALIASES[prefix]}:{tail2}"
    text = _expand_self(text, self_class, path)
    if not _KEY.fullmatch(text):
        raise _error(path, f"Action '{key}' is not a canonical key <Tag>:<Kind>[:<Owner>][:<Field>][@<Binding>].")
    return text


def _expand_self(text: str, self_class: str | None, path: str) -> str:
    parts = text.split(":")
    if len(parts) < 2 or parts[1] not in _SELF_MEMBER_KINDS:
        return text
    if len(parts) == 3 and (parts[1] in ("VariableGet", "VariableSet") or parts[1] in DISPATCHER_KINDS):
        parts.insert(2, "Self")  # "K2:VariableGet:Health", "K2:CallDelegate:OnDied"
    if len(parts) >= 3 and parts[2] == "Self":
        if self_class is None:
            raise _error(path, f"Action '{text}' uses a Self owner but the asset class path is unknown.")
        parts[2] = self_class
    return ":".join(parts)


def preferred_alias(key: str | None, *, self_class: str | None = None) -> str | None:
    """The AI-friendly spelling of a canonical live key; ``normalize_action`` maps it back.

    Keys with a ``#`` disambiguator are returned unchanged.  Members of
    ``self_class`` use the ``Self`` owner (``K2:VariableGet:Self:Health``,
    shortened to ``K2:Get:Health``).
    """
    if not key or "#" in key:
        return key
    alias = PREFERRED_ALIASES.get(key)
    if alias is not None:
        return alias
    parts = key.split(":")
    # Object paths are FNames (case-insensitive): a package "DatasmithArealightMesh" holds the class
    # "DatasmithAreaLightMesh_C", while the asset's own class path is spelled from the package name.
    if (self_class and len(parts) == 4 and parts[1] in _SELF_MEMBER_KINDS
            and parts[2].casefold() == self_class.casefold()):
        if parts[1] in ("VariableGet", "VariableSet") and "@" not in parts[3]:
            return f"{parts[0]}:{parts[1][len('Variable'):]}:{parts[3]}"
        return ":".join(parts[:2] + ["Self"] + parts[3:])
    return key


def actions_equal(desired: str | None, live: str | None) -> bool:
    """Desired keys without a ``#`` disambiguator match any disambiguated live key."""
    if desired is None or live is None:
        return False
    desired_base, desired_suffix = split_disambiguator(desired)
    live_base, live_suffix = split_disambiguator(live)
    if desired_base != live_base:
        return False
    return desired_suffix is None or desired_suffix == live_suffix


def legacy_k2_action(class_path: object, member: object, *, self_class: str | None, path: str) -> str:
    """Convert the legacy ``Class`` + ``Member`` node form into a canonical K2 action key."""
    if type(class_path) is not str or not class_path.startswith(LEGACY_K2_CLASS_PREFIX):
        raise contract_error(f"{path}/Class", "Graph.LegacyNodeUnsupported",
                             "Legacy Class must be a /Script/BlueprintGraph.K2Node_* path; use Action instead.")
    kind = class_path[len(LEGACY_K2_CLASS_PREFIX):]
    if member is None:
        return normalize_action(f"K2:{kind}", self_class=self_class, path=f"{path}/Class")
    if type(member) is not dict or member.get("Kind") != "MemberRef":
        raise contract_error(f"{path}/Member", "Graph.LegacyNodeUnsupported",
                             "Only {\"Kind\":\"MemberRef\",\"OwnerClass\",\"Name\"} members are converted; use Action instead.")
    owner, name = member.get("OwnerClass"), member.get("Name")
    if type(owner) is not str or not owner or type(name) is not str or not name:
        raise contract_error(f"{path}/Member", "Graph.LegacyNodeUnsupported", "MemberRef needs OwnerClass and Name.")
    return normalize_action(f"K2:{kind}:{owner}:{name}", self_class=self_class, path=f"{path}/Member")


def action_field_name(key: str | None) -> str | None:
    """Last ``:``-segment of a key with owner/field, used for semantic ids."""
    if not key:
        return None
    base = split_disambiguator(key)[0].partition("@")[0]
    parts = base.split(":")
    return parts[-1] if len(parts) >= 3 else None


def action_kind(key: str | None) -> str | None:
    if not key:
        return None
    parts = split_disambiguator(key)[0].split(":")
    return parts[1] if len(parts) >= 2 else None
