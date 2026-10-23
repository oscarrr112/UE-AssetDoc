# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""K2 (Blueprint) schema adapter and Body codec.

Body shape (``UbergraphPages`` / ``FunctionGraphs`` / ``MacroGraphs`` arrays)::

    {"Name": "AddHealth",
     "Signature": {"Inputs": [{"Name": "Amount", "Type": "float"}], "Outputs": [...], "Pure": true},
     "Nodes": [{"Id": "Branch", "Action": "K2:Branch", "PinOverrides": [{"Pin": "Condition", "DefaultValue": "true"}]},
               {"Id": "Print", "Class": "/Script/BlueprintGraph.K2Node_CallFunction",
                "Member": {"Kind": "MemberRef", "OwnerClass": "/Script/Engine.KismetSystemLibrary", "Name": "PrintString"}}],
     "Links": [{"From": "Entry:then", "To": "Branch:execute"}, {"From": {"Node": "Branch", "Pin": "then"}, "To": "Print:execute"}],
     "Subgraphs": [{"Name": "Collapsed", "Owner": "CompositeNode", "Nodes": [...], "Links": [...]}]}

Pin type shorthands: ``float`` is the UE5 Blueprint Float (real/double), ``float32`` a
single-precision real; ``bool``, ``int``, ``int64``, ``byte``, ``string``, ``name``,
``text``, ``<category>:<object path>`` and ``array:``/``set:`` prefixes as before.
Signature flags (function graphs only): ``Pure``, ``Const``, ``CallInEditor``,
``Access`` (public/protected/private), ``Category``, ``Tooltip``, ``Keywords``.

``Entry`` and ``Result`` are reserved node ids for the implicit function/macro
terminators and may be used as link endpoints without being declared.
``NodeGuid``, ``GraphGuid`` and ``Position`` are generated metadata: extract
writes them, reconcile ignores them when the author omitted them.
"""
from __future__ import annotations

import re
from dataclasses import replace

from asset_document.diagnostics import contract_error
from asset_document.graphs.actions import action_field_name, action_kind, legacy_k2_action
from asset_document.graphs.live import LiveNode
from asset_document.graphs.model import (
    Endpoint, GraphDoc, LinkDoc, LocalVariable, NodeDoc, NodeProperties, PinOverride, PinType, Position, Signature,
    Size, UserPin, thaw_json,
)
from asset_document.graphs.schemas import SchemaAdapter, sanitize_id


SCHEMA_TAG = "K2"
SCHEMA_CLASS = "/Script/BlueprintGraph.EdGraphSchema_K2"
SECTIONS = (("UbergraphPages", "ubergraph"), ("FunctionGraphs", "function"), ("MacroGraphs", "macro"))
SECTION_BY_ROLE = {role: section for section, role in SECTIONS}
RESERVED_IDS = {"Entry": "entry", "Result": "result"}
_GRAPH_FIELDS = frozenset({"Name", "Schema", "GraphGuid", "Nodes", "Links", "Signature", "Subgraphs",
                           "LocalVariables", "Layout"})
_SUBGRAPH_FIELDS = _GRAPH_FIELDS | {"Owner", "Role"}
_NODE_FIELDS = frozenset({"Id", "Action", "Class", "Member", "Implicit", "NodeGuid", "Position", "PinOverrides",
                          "Properties", "TemplateProperties", "Name", "Comment", "Parent", "Extras", "Inputs",
                          "PinTypes", "Size", "CommentBubbleVisible"})
COMMENT_BOX_KIND = "EdGraphNode_Comment"
# Node kinds identified by their Name (custom events, timelines, collapsed graphs); other K2 node names are titles.
NAMED_KINDS = frozenset({"CustomEvent", "Timeline", "Composite"})
_OVERRIDE_FIELDS = frozenset({"Pin", "Direction", "DefaultValue", "DefaultObject", "DefaultTextValue"})
_USER_PIN_FIELDS = frozenset({"Name", "Type", "Default"})
_LOCAL_FIELDS = frozenset({"Name", "Type", "Default", "Category", "Tooltip"})
_TYPE_FIELDS = {"Category": "category", "SubCategory": "sub_category", "SubCategoryObject": "sub_category_object",
                "Container": "container", "IsRef": "is_ref", "IsConst": "is_const"}
# UE5 Blueprint "Float" variables/parameters are double precision (category real, sub-category
# double), so the shorthand ``float`` means that type; single-precision ``real``/``float`` pins
# (C++ ``float`` parameters such as ReceiveTick's DeltaSeconds) are ``float32``.  ``double`` is
# accepted as an input synonym of ``float``.
_SHORT_TYPES = {"bool": ("bool", ""), "int": ("int", ""), "int64": ("int64", ""), "byte": ("byte", ""),
                "float": ("real", "double"), "float32": ("real", "float"), "double": ("real", "double"),
                "string": ("string", ""), "name": ("name", ""), "text": ("text", ""), "exec": ("exec", "")}
_SHORT_BY_TYPE = {}
for _short, _type in _SHORT_TYPES.items():
    _SHORT_BY_TYPE.setdefault(_type, _short)  # first spelling wins: (real, double) -> "float"
# Body Signature flag keys -> graph.function.flags.set.v1 / extras.signature_flags keys.
SIGNATURE_FLAGS = {"Pure": "pure", "Const": "const", "CallInEditor": "call_in_editor", "Access": "access",
                   "Category": "category", "Tooltip": "tooltip", "Keywords": "keywords"}
SIGNATURE_FLAG_DEFAULTS = {"pure": False, "const": False, "call_in_editor": False, "access": "public",
                           "category": "", "tooltip": "", "keywords": ""}
_BOOL_FLAGS = frozenset({"pure", "const", "call_in_editor"})
_ACCESS = ("public", "protected", "private")
_SIGNATURE_FIELDS = frozenset({"Inputs", "Outputs"}) | frozenset(SIGNATURE_FLAGS)
_EXEC_NAMES = frozenset({"execute", "then", "else", "exec", "completed", "loop body", "break", "cast failed"})
_OWNER_ONLY_KINDS = {"DynamicCast": "Cast", "ClassDynamicCast": "ClassCast", "MakeStruct": "MakeStruct",
                     "BreakStruct": "BreakStruct", "SetFieldsInStruct": "SetFieldsInStruct",
                     "EnumLiteral": "EnumLiteral", "SwitchEnum": "SwitchEnum",
                     "ForEachElementInEnum": "ForEachElementInEnum", "GetNumEnumEntries": "GetNumEnumEntries",
                     "CastByteToEnum": "CastByteToEnum"}
_THEN_N = re.compile(r"then_\d+\Z", re.IGNORECASE)
# Pin categories whose value is an object (UEdGraphPin::DefaultObject), never text.
_OBJECT_VALUE_CATEGORIES = frozenset({"object", "class", "interface"})


# --------------------------------------------------------------------------
# Adapter
# --------------------------------------------------------------------------
def declared_call_key(base: str, have) -> str | None:
    """The live key of a call an authored key names by a class that inherits or implements the function
    (``K2:CallFunction:/Script/EnhancedInput.EnhancedInputLocalPlayerSubsystem:AddMappingContext``): Apply
    places the action of the declaring class (``...EnhancedInputSubsystemInterface:AddMappingContext``) and
    reads it back under that key. ``have``: the live keys (no disambiguators); None when ``base`` is live
    itself, names a Blueprint class (its own functions may be new) or no single live call of that function
    exists."""
    parts = base.split(":")
    if len(parts) != 4 or parts[:2] != ["K2", "CallFunction"] or not parts[2].startswith("/Script/") or base in have:
        return None
    found = {key for key in have if key.count(":") == 3 and key.startswith("K2:CallFunction:/")
             and key.rsplit(":", 1)[-1] == parts[3]}
    return found.pop() if len(found) == 1 else None


def node_identity_action(action: str | None) -> str | None:
    """The action a node is identified by: a dispatcher event ("K2:CustomEvent:<Owner>:<Dispatcher>")
    spawns a plain custom event, so it is identified as ``K2:CustomEvent``."""
    if action is not None and action_kind(action) == "CustomEvent":
        return "K2:CustomEvent"
    return action


def dispatcher_event_name(action: str | None) -> str | None:
    """Default Name of a dispatcher event node (``<Dispatcher>_Event``, as the editor names it)."""
    if action is None or action_kind(action) != "CustomEvent":
        return None
    field = action_field_name(action)
    return f"{field}_Event" if field else None


_CONTAINER_LIBRARIES = frozenset(("/Script/Engine.KismetArrayLibrary", "/Script/Engine.BlueprintMapLibrary",
                                  "/Script/Engine.BlueprintSetLibrary"))


class K2SchemaAdapter(SchemaAdapter):
    tag = SCHEMA_TAG
    # A collapsed graph (K2:Composite) is named after its node: renaming the node renames the graph.
    owned_subgraphs_follow_owner = True
    # Its entry / exit tunnel pins make the collapsed node's pins, which the parent graph links to.
    subgraphs_before_links = True
    # "UbergraphPages": [] empties the EventGraph (a user can delete all of its nodes, not the graph).
    clears_kept_graphs = True
    keeps_authored_ids = True

    # Adapter-owned node settings (extras.properties, SetNodeSetting): "Pure", "Fields", "Function" ---
    def live_properties(self, node, container):
        settings = node.extras.get("properties") if container == "node" else None
        if not settings or not hasattr(settings, "items"):
            return super().live_properties(node, container)
        return {**super().live_properties(node, container), **thaw_json(settings)}

    def property_follows_links(self, action, container, key):
        # Create Event's Function: an unlinked node clears the name (UK2Node_CreateDelegate::
        # HandleAnyChangeWithoutNotifying); the author picks it from the combo once OutputDelegate is linked.
        return container == "node" and key == "Function" and action_kind(action) == "CreateDelegate"

    def extract_properties(self, node, hint):
        settings = node.extras.get("properties")
        if not settings or not hasattr(settings, "items"):
            return None
        defaults = node.extras.get("property_defaults") or {}
        authored = hint.properties.node if hint is not None else {}
        output = {}
        for key, value in settings.items():
            if key in authored or defaults.get(key) != value:
                output[key] = thaw_json(value)
        if hint is not None and node.properties is not None:
            live = node.property_container("node")
            for key in authored:
                if key not in output and key in live:
                    output[key] = thaw_json(live[key])
        template = {}
        if hint is not None and node.properties is not None:
            live = node.property_container("template")
            template = {key: thaw_json(live[key]) for key in hint.properties.template if key in live}
        return NodeProperties(output, template)

    # Promotable operators: Pin Conversions ----------------------------------------------------
    def node_pin_types(self, node):
        types = super().node_pin_types(node)
        if node.extras.get("promotable") is True and len({item for _, item in types}) < 2 \
                and node.extras.get("wired_to_operator") is not True:
            # Operands and result of one type come from linking alone; a mixed node (vector * float)
            # states the types its Pin Conversions picked, and one wired to another operator states
            # them too (two wildcard operators take the types the order of their links gives them).
            return ()
        return types

    def live_pin_types(self, node):
        return super().node_pin_types(node)

    def pin_types_fit(self, node, live):
        # Pin Conversions changes a promotable operator's types in place.
        return live.extras.get("promotable") is True or super().pin_types_fit(node, live)

    def semantic_key(self, node, normalized_action):
        key = super().semantic_key(node, normalized_action)
        if key is None:
            return None
        base = node_identity_action(key[0])
        # Custom events and timelines are identified by their name; other K2 node names are titles.
        return (base, key[1]) if action_kind(base) in NAMED_KINDS else (base, None)

    def live_semantic_key(self, node):
        key = super().live_semantic_key(node)
        if key is None:
            return None
        return key if action_kind(key[0]) in NAMED_KINDS else (key[0], None)

    def placed_with_graph(self, node):
        # An override of a function the parent Blueprint implements comes with "Parent: <function>"
        # (UEdGraphSchema_K2::CreateFunctionGraphTerminators); the author may have removed it.
        return node.implicit_role is None and action_kind(node.action_key) == "CallParentFunction"

    def id_rank(self, node):
        # A Create places called custom events before the rest of the graph (reconcile ``definitions``).
        return -1 if action_kind(node.action_key) == "CustomEvent" else 0

    def live_name(self, node):
        name = super().live_name(node)
        if name is None and action_kind(node.action_key) == "CustomEvent":
            return node.title or None
        return name

    def extract_name(self, node):
        return self.live_name(node) if action_kind(node.action_key) in NAMED_KINDS else None

    def node_user_pins(self, node):
        """User pins (outputs) of a custom event, in ``extras.user_pins`` order; None for other nodes."""
        if action_kind(node.action_key) != "CustomEvent" or "user_pins" not in node.extras:
            return None
        output = []
        for name in node.extras.get("user_pins") or ():
            pin = node.find_pin(name, "output") if type(name) is str else None
            if pin is not None and pin not in output:
                output.append(pin)
        return tuple(output)

    def is_exec_pin_name(self, name: str) -> bool:
        folded = name.casefold()
        return folded in _EXEC_NAMES or bool(_THEN_N.fullmatch(name))

    def ignores_default(self, pin) -> bool:
        # An object / class / interface pin's value is its DefaultObject: a text DefaultValue there
        # ("0" on StandardMacros' IsDedicatedServer WorldContextObject, saved by an old editor) is no
        # value (UEdGraphSchema_K2::DefaultValueSimpleValidation refuses it, the compiler reads the
        # object), so it is neither extracted nor reset.
        return (pin.type.category in _OBJECT_VALUE_CATEGORIES and bool(pin.default_value)
                and not pin.default_object and not pin.default_text)

    def user_pins(self, node, graph_role):
        if graph_role == "macro" and "user_pins" not in node.extras:
            direction = "output" if node.implicit_role == "entry" else "input"
            return tuple(pin for pin in node.pins if pin.direction == direction)
        return super().user_pins(node, graph_role)

    def link_waits(self, from_action: str | None, to_pin: str) -> bool:
        # A reroute takes its type from its first link; "self" accepts a single object or an array.
        return to_pin == "self" and from_action is not None and action_kind(from_action) in ("Reroute", "Knot")

    def link_leads(self, to_action: str | None, to_pin: str) -> bool:
        # A container function's wildcard pins take their type from the first link: its element linked
        # first types the container too, which then refuses a container of a base class (Array Add's
        # Texture 2D NewItem before its Object array: LandmassBrush_CopyRegion). The author links the
        # container first.
        if to_action is None or to_pin.casefold() not in ("targetarray", "targetmap", "targetset"):
            return False
        owner = to_action.partition("#")[0].split(":")
        kind = action_kind(to_action)
        return kind == "CallArrayFunction" or (kind == "CallFunction" and len(owner) >= 3 and owner[2] in _CONTAINER_LIBRARIES)

    def types_from_operands(self, action: str | None) -> bool:
        # A promotable operator picks its function from the types linked into it (EvaluatePinsFromChange):
        # an operator whose output is linked before its operands takes the type its consumer offers. A
        # container function (Map Find, Array Get, Set Contains) takes its types from its container: linked
        # output-first, Map Find's Value into a Texture input makes the map Name->Texture before the
        # Name->Texture2D map is linked in (Landmass_FunctionLibrary). (A Make Array linked into a target
        # first is the author's way to type it by that target: it keeps document order.)
        if action is None:
            return False
        if action_kind(action) == "PromotableOperator":
            return True
        owner = action.partition("#")[0].split(":")
        return action_kind(action) == "CallFunction" and len(owner) >= 3 and owner[2] in _CONTAINER_LIBRARIES

    def passes_link_type(self, action: str | None) -> bool:
        return action is not None and action_kind(action) in ("Reroute", "Knot")

    def override_shapes_pins(self, action: str | None, pin: str) -> bool:
        # UK2Node_FormatText makes one input pin per {argument} of its Format text.
        return action is not None and action_kind(action) == "FormatText" and pin.casefold() == "format"

    def implicit_spawn_action(self, implicit_role: str, graph_role: str) -> str | None:
        if implicit_role == "result" and graph_role == "function":
            return "K2:FunctionResult"
        return None

    def signature_flag_defaults(self, graph_role: str) -> dict | None:
        return dict(SIGNATURE_FLAG_DEFAULTS) if graph_role == "function" else None

    def node_base_id(self, node: LiveNode) -> str:
        if node.implicit_role or self.live_name(node):
            return super().node_base_id(node)
        kind = action_kind(node.action_key)
        field = action_field_name(node.action_key)
        if field and kind in ("VariableGet", "VariableSet"):
            return sanitize_id(f"{kind[len('Variable'):]}_{field}")
        if kind == "Self":
            return "Self"
        parts = (node.action_key or "").partition("#")[0].partition("@")[0].split(":")
        if len(parts) == 3 and kind in _OWNER_ONLY_KINDS:
            # Owner-only keys (casts, struct/enum field nodes): "DynamicCast_Pawn", "BreakStruct_HitResult".
            short = parts[2].rsplit("/", 1)[-1].rsplit(".", 1)[-1]
            return sanitize_id(f"{_OWNER_ONLY_KINDS[kind]}_{short}")
        return super().node_base_id(node)


K2_ADAPTER = K2SchemaAdapter()


# --------------------------------------------------------------------------
# Decode (Body -> GraphDoc)
# --------------------------------------------------------------------------
def _object(value: object, path: str) -> dict:
    if type(value) is not dict:
        raise contract_error(path, "Graph.InvalidType", "Expected a JSON object.")
    return value


def _array(value: object, path: str) -> list:
    if type(value) is not list:
        raise contract_error(path, "Graph.InvalidType", "Expected a JSON array.")
    return value


def _known(row: dict, allowed: frozenset, path: str) -> None:
    for key in row:
        if key not in allowed:
            raise contract_error(f"{path}/{key}", "Graph.UnknownField",
                                 f"Unknown field '{key}'. Allowed: {', '.join(sorted(allowed))}.")


def _string(row: dict, key: str, path: str, *, required: bool = False) -> str | None:
    value = row.get(key)
    if value is None:
        if required:
            raise contract_error(f"{path}/{key}", "Graph.MissingField", f"{key} is required.")
        return None
    if type(value) is not str or (required and not value):
        raise contract_error(f"{path}/{key}", "Graph.InvalidType", f"{key} must be a string.")
    return value


def _rebase(error, path: str):
    """Prefix model-level diagnostic paths with the Body location."""
    diagnostic = error.diagnostic
    if diagnostic.path.startswith("/Body") or diagnostic.path.startswith(path):
        return error
    suffix = "" if diagnostic.path == "/" else diagnostic.path
    return contract_error(path + suffix, diagnostic.code, diagnostic.message)


def decode_pin_type(value: object, path: str) -> PinType:
    try:
        if type(value) is str:
            container, _, rest = value.partition(":") if value.split(":", 1)[0] in ("array", "set") else ("none", "", value)
            short = _SHORT_TYPES.get(rest)
            if short is not None:
                return PinType(short[0], short[1], "", container)
            category, sep, obj = rest.partition(":")
            if not sep or not category or not obj:
                raise contract_error(path, "Graph.InvalidPinType",
                                     f"Type '{value}' is not a known shorthand ({', '.join(sorted(_SHORT_TYPES))}, "
                                     "<category>:<object path>, array:<type>) or a full type object.")
            return PinType(category, "", obj, container)
        row = _object(value, path)
        _known(row, frozenset(_TYPE_FIELDS) | {"ValueType"}, path)
        value_type = row.get("ValueType")
        if value_type is not None:
            value_type = decode_pin_type(value_type, f"{path}/ValueType")
        return PinType(**{_TYPE_FIELDS[key]: item for key, item in row.items() if key != "ValueType"},
                       value_type=value_type)
    except Exception as error:
        if hasattr(error, "diagnostic"):
            raise _rebase(error, path) from None
        raise contract_error(path, "Graph.InvalidPinType", "Invalid pin type.") from None


def encode_pin_type(value: PinType) -> object:
    short = _SHORT_BY_TYPE.get((value.category, value.sub_category)) if not value.sub_category_object else None
    simple = not value.is_ref and not value.is_const and value.container in ("none", "array", "set")
    if simple and short is not None:
        text = short
    elif simple and not value.sub_category and value.sub_category_object:
        text = f"{value.category}:{value.sub_category_object}"
    else:
        output = {"Category": value.category}
        for label, attr in _TYPE_FIELDS.items():
            item = getattr(value, attr)
            if label != "Category" and item not in ("", "none", False):
                output[label] = item
        if value.value_type is not None:
            output["ValueType"] = encode_pin_type(value.value_type)
        return output
    return text if value.container == "none" else f"{value.container}:{text}"


def _decode_user_pins(value: object, path: str) -> tuple[UserPin, ...]:
    output = []
    for index, item in enumerate(_array(value, path)):
        row_path = f"{path}/{index}"
        row = _object(item, row_path)
        _known(row, _USER_PIN_FIELDS, row_path)
        if "Type" not in row:
            raise contract_error(f"{row_path}/Type", "Graph.MissingField", "Type is required, e.g. \"float\".")
        try:
            output.append(UserPin(_string(row, "Name", row_path, required=True),
                                  decode_pin_type(row["Type"], f"{row_path}/Type"), _string(row, "Default", row_path)))
        except Exception as error:
            raise _rebase(error, row_path) from None
    return tuple(output)


def _decode_local_variables(value: object, path: str) -> tuple[LocalVariable, ...]:
    output = []
    for index, item in enumerate(_array(value, path)):
        row_path = f"{path}/{index}"
        row = _object(item, row_path)
        _known(row, _LOCAL_FIELDS, row_path)
        if "Type" not in row:
            raise contract_error(f"{row_path}/Type", "Graph.MissingField", "Type is required, e.g. \"float\".")
        try:
            pin_type, value_type = row["Type"], None
            if type(pin_type) is dict and "ValueType" in pin_type:
                value_type = decode_pin_type(pin_type["ValueType"], f"{row_path}/Type/ValueType")
                pin_type = {key: item for key, item in pin_type.items() if key != "ValueType"}
            output.append(LocalVariable(_string(row, "Name", row_path, required=True),
                                        decode_pin_type(pin_type, f"{row_path}/Type"),
                                        _string(row, "Default", row_path) or "", _string(row, "Category", row_path) or "",
                                        _string(row, "Tooltip", row_path) or "", value_type))
        except Exception as error:
            raise _rebase(error, row_path) from None
    return tuple(output)


def _encode_local_variable(value: LocalVariable) -> dict:
    pin_type = encode_pin_type(value.type)
    if value.value_type is not None:
        pin_type = dict(pin_type, ValueType=encode_pin_type(value.value_type))
    output = {"Name": value.name, "Type": pin_type}
    for label, item in (("Default", value.default), ("Category", value.category), ("Tooltip", value.tooltip)):
        if item:
            output[label] = item
    return output


def _decode_signature_flags(row: dict, path: str) -> dict:
    flags = {}
    for label, key in SIGNATURE_FLAGS.items():
        if label not in row:
            continue
        value = row[label]
        if key in _BOOL_FLAGS:
            if type(value) is not bool:
                raise contract_error(f"{path}/{label}", "Graph.InvalidSignature", f"{label} must be true or false.")
        elif key == "access":
            if type(value) is not str or value.casefold() not in _ACCESS:
                raise contract_error(f"{path}/{label}", "Graph.InvalidSignature",
                                     "Access must be \"public\", \"protected\" or \"private\".")
            value = value.casefold()
        elif type(value) is not str:
            raise contract_error(f"{path}/{label}", "Graph.InvalidSignature", f"{label} must be a string.")
        flags[key] = value
    return flags


def _decode_signature(value: object, path: str) -> Signature:
    row = _object(value, path)
    _known(row, _SIGNATURE_FIELDS, path)
    flags = _decode_signature_flags(row, path)
    try:
        return Signature(_decode_user_pins(row.get("Inputs", []), f"{path}/Inputs"),
                         _decode_user_pins(row.get("Outputs", []), f"{path}/Outputs"), flags)
    except Exception as error:
        raise _rebase(error, path) from None


def _decode_override(value: object, path: str) -> PinOverride:
    row = _object(value, path)
    _known(row, _OVERRIDE_FIELDS, path)
    direction = row.get("Direction", "input")
    if type(direction) is not str or direction.casefold() not in ("input", "output"):
        raise contract_error(f"{path}/Direction", "Graph.InvalidPinOverride", "Direction is Input or Output.")
    try:
        return PinOverride(_string(row, "Pin", path, required=True), direction.casefold(),
                           row.get("DefaultValue"), row.get("DefaultObject"), row.get("DefaultTextValue"))
    except Exception as error:
        raise _rebase(error, path) from None


def _decode_position(value: object, path: str) -> Position | None:
    if value is None:
        return None
    row = _object(value, path)
    _known(row, frozenset({"X", "Y"}), path)
    try:
        return Position(row.get("X"), row.get("Y"))
    except Exception as error:
        raise _rebase(error, path) from None


def _decode_size(value: object, path: str) -> Size | None:
    if value is None:
        return None
    row = _object(value, path)
    _known(row, frozenset({"Width", "Height"}), path)

    def whole(number):
        # JSON numbers may arrive as floats (640.0): a whole number is the integer.
        return int(number) if type(number) is float and number.is_integer() else number
    try:
        return Size(whole(row.get("Width")), whole(row.get("Height")))
    except Exception as error:
        raise _rebase(error, path) from None


def _decode_box(row: dict, action: str | None, path: str) -> tuple[Size | None, bool | None]:
    """A comment box's Size and a node's CommentBubbleVisible."""
    box = action is not None and action_kind(action) == COMMENT_BOX_KIND
    size = _decode_size(row.get("Size"), f"{path}/Size")
    if size is not None and action is not None and not box:
        raise contract_error(f"{path}/Size", "Graph.NodeNotResizable",
                             "Only a comment box (Action \"K2:Comment\") has a Size; other nodes size to their pins.")
    bubble = row.get("CommentBubbleVisible")
    if bubble is not None and type(bubble) is not bool:
        raise contract_error(f"{path}/CommentBubbleVisible", "Graph.InvalidType", "CommentBubbleVisible is true or false.")
    if bubble is not None and box:
        raise contract_error(f"{path}/CommentBubbleVisible", "Graph.UnknownField",
                             "A comment box shows its text in the box; its zoomed-out bubble is "
                             "Properties.bCommentBubbleVisible_InDetailsPanel.")
    return size, bubble


def _decode_node(value: object, path: str, self_class: str | None) -> NodeDoc:
    row = _object(value, path)
    _known(row, _NODE_FIELDS, path)
    node_id = _string(row, "Id", path, required=True)
    action, implicit = row.get("Action"), row.get("Implicit")
    if action is not None and "Class" in row:
        raise contract_error(f"{path}/Class", "Graph.ConflictingAction", "Use either Action or the legacy Class+Member form.")
    if "Member" in row and "Class" not in row:
        raise contract_error(f"{path}/Member", "Graph.LegacyNodeUnsupported", "Member requires the legacy Class field.")
    authored_action = None
    if "Class" in row:
        action = legacy_k2_action(row["Class"], row.get("Member"), self_class=self_class, path=path)
    elif action is not None:
        authored_action = action if type(action) is str else None
        action = K2_ADAPTER.normalize_action(action, self_class=self_class, path=f"{path}/Action")
    if node_id in RESERVED_IDS and action is None and implicit is None:
        implicit = RESERVED_IDS[node_id]
    for key in ("Properties", "TemplateProperties", "Extras"):
        if key in row:
            _object(row[key], f"{path}/{key}")
    user_pins = None
    if "Inputs" in row:
        if action is None or action_kind(action) != "CustomEvent":
            raise contract_error(f"{path}/Inputs", "Graph.UnknownField",
                                 "Inputs are the parameters of a custom event (Action K2:CustomEvent).")
        user_pins = _decode_user_pins(row["Inputs"], f"{path}/Inputs")
        if any(pin.default is not None for pin in user_pins):
            raise contract_error(f"{path}/Inputs", "Graph.InvalidSignature", "Custom event inputs have no Default.")
    pin_types = tuple((pin, decode_pin_type(item, f"{path}/PinTypes/{pin}"))
                      for pin, item in _object(row.get("PinTypes", {}), f"{path}/PinTypes").items())
    size, bubble = _decode_box(row, action, path)
    name = row.get("Name")
    if name is None:
        name = dispatcher_event_name(action)
    if name is None and action is not None and action_kind(action) == "Timeline":
        raise contract_error(f"{path}/Name", "Graph.MissingName",
                             "A K2:Timeline node names its timeline (Name), the row listed under Timelines.")
    try:
        return NodeDoc(
            id=node_id, action=action, implicit=implicit, guid=row.get("NodeGuid"), parent=row.get("Parent"),
            position=_decode_position(row.get("Position"), f"{path}/Position"),
            pin_overrides=tuple(_decode_override(item, f"{path}/PinOverrides/{index}") for index, item in
                                enumerate(_array(row.get("PinOverrides", []), f"{path}/PinOverrides"))),
            properties=NodeProperties(row.get("Properties", {}), row.get("TemplateProperties", {})),
            name=name, comment=row.get("Comment"), extras=row.get("Extras", {}), user_pins=user_pins,
            pin_types=pin_types, size=size, comment_bubble_visible=bubble, authored_action=authored_action,
        )
    except Exception as error:
        if hasattr(error, "diagnostic"):
            raise _rebase(error, path) from None
        raise


def _decode_endpoint(value: object, path: str) -> Endpoint:
    if type(value) is str:
        node, sep, pin = value.partition(":")
        if not sep:
            raise contract_error(path, "Graph.InvalidLink", "Compact endpoints are \"NodeId:PinName\".")
    else:
        row = _object(value, path)
        _known(row, frozenset({"Node", "Pin"}), path)
        node, pin = row.get("Node"), row.get("Pin")
    try:
        return Endpoint(node, pin)
    except Exception as error:
        raise _rebase(error, path) from None


def _decode_links(value: object, path: str) -> tuple[LinkDoc, ...]:
    output = []
    for index, item in enumerate(_array(value, path)):
        row_path = f"{path}/{index}"
        row = _object(item, row_path)
        _known(row, frozenset({"From", "To"}), row_path)
        for key in ("From", "To"):
            if key not in row:
                raise contract_error(f"{row_path}/{key}", "Graph.MissingField", f"{key} is required.")
        output.append(LinkDoc(_decode_endpoint(row["From"], f"{row_path}/From"),
                              _decode_endpoint(row["To"], f"{row_path}/To")))
    return tuple(output)


def _decode_graph(value: object, path: str, *, graph_id: str, role: str | None, self_class: str | None,
                  subgraph: bool) -> GraphDoc:
    row = _object(value, path)
    _known(row, _SUBGRAPH_FIELDS if subgraph else _GRAPH_FIELDS, path)
    name = _string(row, "Name", path, required=True)
    schema = row.get("Schema")
    if schema not in (None, SCHEMA_TAG, SCHEMA_CLASS):
        raise contract_error(f"{path}/Schema", "Graph.SchemaMismatch", f"Blueprint graphs use the {SCHEMA_TAG} schema.")
    graph_id = f"{graph_id}/{name}"
    nodes = [_decode_node(item, f"{path}/Nodes/{index}", self_class)
             for index, item in enumerate(_array(row.get("Nodes", []), f"{path}/Nodes"))]
    links = _decode_links(row.get("Links", []), f"{path}/Links")
    declared = {node.id for node in nodes}
    for link in links:
        for endpoint in (link.from_, link.to):
            if endpoint.node in RESERVED_IDS and endpoint.node not in declared:
                nodes.append(NodeDoc(id=endpoint.node, implicit=RESERVED_IDS[endpoint.node]))
                declared.add(endpoint.node)
    subgraphs = tuple(
        _decode_graph(item, f"{path}/Subgraphs/{index}", graph_id=graph_id, role=None, self_class=self_class,
                      subgraph=True)
        for index, item in enumerate(_array(row.get("Subgraphs", []), f"{path}/Subgraphs")))
    signature = _decode_signature(row["Signature"], f"{path}/Signature") if "Signature" in row else None
    local_variables = _decode_local_variables(row["LocalVariables"], f"{path}/LocalVariables") \
        if row.get("LocalVariables") is not None else None
    if local_variables:
        names = frozenset(item.name for item in local_variables)
        nodes = [replace(node, action=localize_action(node.action, names, graph=name, self_class=self_class))
                 if node.action else node for node in nodes]
    layout = row.get("Layout")
    if layout is not None and type(layout) is not str:
        raise contract_error(f"{path}/Layout", "Graph.InvalidLayout",
                             "Layout is \"Auto\" (lay out every node without a Position) or omitted.")
    return GraphDoc(id=graph_id, role=role or _string(row, "Role", path) or "other", name=name,
                    schema_tag=SCHEMA_TAG, guid=row.get("GraphGuid"), nodes=tuple(nodes), links=links,
                    subgraphs=subgraphs, signature=signature, owner=row.get("Owner"), path=path,
                    local_variables=local_variables, layout=layout)


def localize_action(action: str | None, local_names, *, graph: str, self_class: str | None) -> str | None:
    """Inside a function, ``K2:Get:X`` / ``K2:Set:X`` (the asset's own ``X``) name the function's local
    variable ``X`` when it has one, as the editor's My Blueprint panel resolves the name: the key becomes
    the local's ``K2:VariableGet:<Class>:<Function>:<X>``."""
    if not action or not self_class or not local_names:
        return action
    base, sep, suffix = action.partition("#")
    parts = base.split(":")
    if len(parts) != 4 or parts[1] not in ("VariableGet", "VariableSet") or parts[2].casefold() != self_class.casefold():
        return action
    field = parts[3].partition("@")[0]
    folded = {name.casefold(): name for name in local_names}
    if field.casefold() not in folded:
        return action
    parts[3:] = [graph, folded[field.casefold()] + parts[3][len(field):]]
    return ":".join(parts) + sep + suffix


def decode_section(section: str, value: object, *, self_class: str | None = None) -> tuple[GraphDoc, ...]:
    """Decode one Body graph section (``UbergraphPages`` etc.) into GraphDocs."""
    roles = dict(SECTIONS)
    if section not in roles:
        raise contract_error(f"/Body/{section}", "Graph.UnknownSection", "Not a K2 graph section.")
    path = f"/Body/{section}"
    graphs = tuple(_decode_graph(item, f"{path}/{index}", graph_id=section, role=roles[section],
                                 self_class=self_class, subgraph=False)
                   for index, item in enumerate(_array(value, path)))
    names = {}
    for index, graph in enumerate(graphs):
        if graph.name.casefold() in names:
            raise contract_error(f"{path}/{index}/Name", "Graph.DuplicateGraphName",
                                 f"Graph '{graph.name}' already appears at {path}/{names[graph.name.casefold()]}.")
        names[graph.name.casefold()] = index
    return graphs


def decode_body(body: dict, *, self_class: str | None = None) -> tuple[tuple[GraphDoc, ...], frozenset[str]]:
    """Decode every present K2 section; returns the graphs and the managed roles.

    A section that is absent from the Body is not managed (its live graphs are
    left untouched); a present section is the complete desired state.
    """
    graphs, roles = [], set()
    for section, role in SECTIONS:
        if section in body and body[section] is not None:
            graphs.extend(decode_section(section, body[section], self_class=self_class))
            roles.add(role)
    return tuple(graphs), frozenset(roles)


# --------------------------------------------------------------------------
# Encode (GraphDoc -> Body)
# --------------------------------------------------------------------------
def _encode_override(value: PinOverride) -> dict:
    output = {"Pin": value.pin}
    if value.direction != "input":
        output["Direction"] = "Output"
    output[{"value": "DefaultValue", "object": "DefaultObject", "text": "DefaultTextValue"}[value.kind]] = value.payload
    return output


def _number(value: float):
    return int(value) if float(value).is_integer() else value


def encode_node(node: NodeDoc) -> dict:
    output: dict = {"Id": node.id}
    if node.action is not None:
        output["Action"] = node.action
    if node.implicit is not None and RESERVED_IDS.get(node.id) != node.implicit:
        output["Implicit"] = node.implicit
    for key, value in (("NodeGuid", node.guid), ("Parent", node.parent), ("Name", node.name), ("Comment", node.comment)):
        if value is not None:
            output[key] = value
    if node.position is not None:
        output["Position"] = {"X": _number(node.position.x), "Y": _number(node.position.y)}
    if node.size is not None:
        output["Size"] = {"Width": node.size.width, "Height": node.size.height}
    if node.comment_bubble_visible is not None:
        output["CommentBubbleVisible"] = node.comment_bubble_visible
    if node.pin_overrides:
        output["PinOverrides"] = [_encode_override(item) for item in node.pin_overrides]
    if node.properties.node:
        output["Properties"] = thaw_json(node.properties.node)
    if node.properties.template:
        output["TemplateProperties"] = thaw_json(node.properties.template)
    if node.user_pins:
        output["Inputs"] = [_encode_user_pin(pin) for pin in node.user_pins]
    if node.pin_types:
        output["PinTypes"] = {pin: encode_pin_type(item) for pin, item in node.pin_types}
    if node.extras:
        output["Extras"] = thaw_json(node.extras)
    return output


def _encode_user_pin(pin: UserPin) -> dict:
    output = {"Name": pin.name, "Type": encode_pin_type(pin.type)}
    if pin.default is not None:
        output["Default"] = pin.default
    return output


def encode_graph(graph: GraphDoc, *, subgraph: bool = False) -> dict:
    output: dict = {"Name": graph.name}
    if graph.guid is not None:
        output["GraphGuid"] = graph.guid
    if subgraph:
        if graph.owner is not None:
            output["Owner"] = graph.owner
        if graph.role != "other":
            output["Role"] = graph.role
    if graph.signature is not None:
        signature = {}
        if graph.signature.inputs:
            signature["Inputs"] = [_encode_user_pin(pin) for pin in graph.signature.inputs]
        if graph.signature.outputs:
            signature["Outputs"] = [_encode_user_pin(pin) for pin in graph.signature.outputs]
        flags = graph.signature.flags
        signature.update({label: thaw_json(flags[key]) for label, key in SIGNATURE_FLAGS.items() if key in flags})
        output["Signature"] = signature
    if graph.local_variables is not None:
        output["LocalVariables"] = [_encode_local_variable(item) for item in graph.local_variables]
    output["Nodes"] = [encode_node(node) for node in graph.nodes]
    output["Links"] = [{"From": str(link.from_), "To": str(link.to)} for link in graph.links]
    if graph.subgraphs:
        output["Subgraphs"] = [encode_graph(child, subgraph=True) for child in graph.subgraphs]
    return output


def encode_body(graphs: tuple[GraphDoc, ...], roles: frozenset[str] | None = None) -> dict:
    """Encode top-level graphs into Body sections (one key per managed role)."""
    roles = frozenset(role for _, role in SECTIONS) if roles is None else roles
    body = {SECTION_BY_ROLE[role]: [] for _, role in SECTIONS if role in roles}
    for graph in graphs:
        section = SECTION_BY_ROLE.get(graph.role)
        if section in body:
            body[section].append(encode_graph(graph))
    return body


def canonical_graph(value: dict) -> dict:
    """Order-insensitive canonical form for diffs: nodes by Id, links lexically, subgraphs by Name."""
    output = dict(value)
    output["Nodes"] = sorted(value.get("Nodes", []), key=lambda item: item["Id"])
    output["Links"] = sorted(value.get("Links", []), key=lambda item: (item["From"], item["To"]))
    if "Subgraphs" in value:
        output["Subgraphs"] = sorted((canonical_graph(item) for item in value["Subgraphs"]),
                                     key=lambda item: item["Name"].casefold())
    return output


def canonical_section(section: str, value: object, *, self_class: str | None = None) -> list:
    graphs = decode_section(section, value, self_class=self_class)
    return sorted((canonical_graph(encode_graph(graph)) for graph in graphs), key=lambda item: item["Name"].casefold())
