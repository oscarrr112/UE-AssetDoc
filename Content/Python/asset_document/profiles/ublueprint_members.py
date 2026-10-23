# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""UBlueprint member regions (pure): canonical shapes, legacy conversion, planning and diff.

Body member regions (AI-friendly canonical shapes)::

    "ParentClass": "/Script/Engine.Actor"                       (or "/Game/BP_Base.BP_Base_C")
    "ImplementedInterfaces": ["/Game/BPI_Use.BPI_Use_C"]
    "Variables": [{"Name": "Health", "Type": "float", "DefaultValue": 100.0, "Category": "Stats",
                   "Tooltip": "...", "InstanceEditable": true, "ReadOnly": true,
                   "ExposeOnSpawn": true, "Private": true}]
    "Components": [{"Name": "Mesh", "Class": "/Script/Engine.StaticMeshComponent", "AttachTo": "Root",
                    "Properties": {"bVisible": false}},
                   {"Name": "CharacterMovement", "Scope": "Native", "Properties": {"MaxWalkSpeed": 300.0}},
                   {"Name": "BaseMesh", "Scope": "Inherited", "Properties": {...}}]
    "ClassDefaults": {"InitialLifeSpan": 5.0}

Omission: an omitted region is preserved; a present region is the complete desired state
(unlisted variables / own components / interfaces are removed, unlisted ClassDefaults and
component Properties are reset to the parent value).  A listed Variables row is the whole
variable: omitted Category / Tooltip / flags mean their defaults (the extract omits defaults);
an omitted DefaultValue keeps the current value.  Top-level document ``Properties`` (UBlueprint class settings)
merge: only listed properties are written.

The legacy C++ Profile shapes (``{"Kind":"ClassRef"}``, ``{"Interface":...}``,
``{"PinCategory":...}`` types, string DefaultValue, ``Key``/``Scope:"OwnedSCS"`` components)
are accepted and converted by :func:`canonical_members`.
"""
from __future__ import annotations

import math
import re
import struct

from asset_document.diagnostics import contract_error
from asset_document.diff.semantic_compare import number_text
from asset_document.diff.model import Difference, DiffKind
from asset_document.graphs.model import PinType
from asset_document.graphs.schemas.k2 import decode_pin_type, encode_pin_type

MEMBER_KEYS = ("ParentClass", "ImplementedInterfaces", "Variables", "Components", "ClassDefaults")
_VARIABLE_FIELDS = ("Name", "Type", "DefaultValue", "Category", "Tooltip", "InstanceEditable", "ReadOnly",
                    "ExposeOnSpawn", "Private")
_VARIABLE_FLAGS = {"InstanceEditable": "instance_editable", "ReadOnly": "blueprint_read_only",
                   "ExposeOnSpawn": "expose_on_spawn", "Private": "private"}
_COMPONENT_FIELDS = ("Name", "Scope", "Class", "AttachTo", "AttachSocket", "Properties")
_SCOPES = {"owned": "Owned", "ownedscs": "Owned", "inherited": "Inherited", "native": "Native"}
DEFAULT_CATEGORY = "Default"


def _error(path, message, code="UBlueprint.InvalidBodyRegion"):
    return contract_error(path, code, message)


# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
def class_path(value: object, path: str) -> str:
    """A canonical class path; Blueprint asset paths become their generated class."""
    if type(value) is dict:
        if value.get("Kind") == "ClassRef" and type(value.get("Class")) is str:
            value = value["Class"]
        elif type(value.get("Interface")) in (dict, str):
            return class_path(value["Interface"], path)
    if type(value) is not str or not value.startswith("/") or " " in value:
        raise _error(path, "A class is a path such as \"/Script/Engine.Actor\" or \"/Game/BP_Base\".",
                     "UBlueprint.InvalidClassPath")
    if value.startswith("/Script/"):
        return value
    package, _, name = value.partition(".")
    short = package.rsplit("/", 1)[-1]
    if not name or name == short:
        return f"{package}.{short}_C"
    return value


def object_path(value: str) -> str:
    """'/Game/A/B' and '/Game/A/B.B' name the same asset object."""
    if value.startswith("/") and "." not in value.rsplit("/", 1)[-1] and ":" not in value and "'" not in value:
        return f"{value}.{value.rsplit('/', 1)[-1]}"
    return value


# ---------------------------------------------------------------------------
# Variable types
# ---------------------------------------------------------------------------
def _legacy_type(value: dict, path: str) -> dict:
    category = value.get("PinCategory")
    if type(category) is not str or not category:
        raise _error(f"{path}/PinCategory", "Type.PinCategory is required.")
    sub = value.get("PinSubCategory", "") or ""
    obj = value.get("PinSubCategoryObject", "") or ""
    if category in ("float", "double"):
        category, sub = "real", category
    return {"category": category, "sub_category": sub, "sub_category_object": obj, "container": "none",
            "is_ref": False, "is_const": False}


_DTO_KEYS = {"Category": "category", "SubCategory": "sub_category", "SubCategoryObject": "sub_category_object",
             "Container": "container", "IsRef": "is_ref", "IsConst": "is_const"}


def type_dto(value: object, path: str) -> dict:
    """Body Type (shorthand, full object, legacy PinCategory object, or map) -> primitive DTO."""
    if type(value) is dict and "PinCategory" in value:
        return _legacy_type(value, path)
    if type(value) is dict and "ValueType" in value:
        row = {key: item for key, item in value.items() if key != "ValueType"}
        dto = decode_pin_type(row, path).to_dto()
        inner = decode_pin_type(value["ValueType"], f"{path}/ValueType").to_dto()
        dto["value_type"] = {key: inner[key] for key in ("category", "sub_category", "sub_category_object")}
        return dto
    return decode_pin_type(value, path).to_dto()


_UOBJECT = "/Script/CoreUObject.Object"


def variable_type_dto(dto: dict) -> dict:
    """A variable's type as the editor holds it: an object variable whose class is gone (deleted along with
    a parent's variable) is an Object variable (FBlueprintEditorUtils::PostSetupObjectPinType)."""
    if dto["category"] in ("object", "interface") and not dto.get("sub_category_object") \
            and dto.get("sub_category") != "self":
        return {**dto, "sub_category": "", "sub_category_object": _UOBJECT}
    return dto


def type_body(dto: dict) -> object:
    """Primitive DTO -> canonical Body Type."""
    pin = PinType(dto["category"], dto.get("sub_category", ""), dto.get("sub_category_object", ""),
                  dto.get("container", "none"), dto.get("is_ref", False), dto.get("is_const", False))
    if "value_type" not in dto:
        return encode_pin_type(pin)
    output = {"Category": pin.category, "Container": "map"}
    for label, key in _DTO_KEYS.items():
        if label not in ("Category", "Container") and dto.get(key) not in ("", False, None):
            output[label] = dto[key]
    inner = dto["value_type"]
    output["ValueType"] = encode_pin_type(PinType(inner["category"], inner.get("sub_category", ""),
                                                  inner.get("sub_category_object", "")))
    return output


def _typed_default(value: object, dto: dict) -> object:
    """Legacy string defaults of scalar variables become typed JSON."""
    if type(value) is not str or dto.get("container", "none") != "none":
        return value
    category = dto["category"]
    try:
        if category == "bool" and value.lower() in ("true", "false"):
            return value.lower() == "true"
        if category in ("int", "int64") or (category == "byte" and not dto.get("sub_category_object")):
            return int(value)
        if category == "real":
            number = float(value)
            return int(number) if number.is_integer() and "." not in value and "e" not in value.lower() else number
    except ValueError:
        return value
    return value


# ---------------------------------------------------------------------------
# Canonical member regions
# ---------------------------------------------------------------------------
def _canonical_variable(row: object, path: str) -> dict:
    if type(row) is not dict:
        raise _error(path, "A variable is an object with Name and Type.")
    unknown = sorted(set(row) - set(_VARIABLE_FIELDS) - {"VarGuid", "Guid"})
    if unknown:
        raise _error(f"{path}/{unknown[0]}", f"Unknown variable field '{unknown[0]}'. Fields: {', '.join(_VARIABLE_FIELDS)}.",
                     "UBlueprint.UnknownField")
    name = row.get("Name")
    if type(name) is not str or not name.strip():
        raise _error(f"{path}/Name", "Variable Name must be a non-empty string.")
    if "Type" not in row:
        raise _error(f"{path}/Type", "Variable Type is required (for example \"float\" or \"object:/Script/Engine.Actor\").")
    dto = variable_type_dto(type_dto(row["Type"], f"{path}/Type"))
    output = {"Name": name, "Type": type_body(dto)}
    if "DefaultValue" in row and not (row["DefaultValue"] == "" and dto["category"] not in ("string", "name", "text")):
        output["DefaultValue"] = _typed_default(row["DefaultValue"], dto)
    for key in ("Category", "Tooltip"):
        if key in row:
            if type(row[key]) is not str:
                raise _error(f"{path}/{key}", f"{key} must be a string.")
            output[key] = row[key]
    for key in _VARIABLE_FLAGS:
        if key in row:
            if type(row[key]) is not bool:
                raise _error(f"{path}/{key}", f"{key} must be true or false.")
            output[key] = row[key]
    return output


def _component_name(value: object, path: str) -> str:
    if type(value) is dict:
        value = value.get("Name")
    if type(value) is not str or not value.strip():
        raise _error(path, "Component Name must be a non-empty string.")
    return value


def _canonical_component(row: object, path: str) -> dict:
    if type(row) is not dict:
        raise _error(path, "A component is an object with Name and Class.")
    unknown = sorted(set(row) - set(_COMPONENT_FIELDS) - {"Key", "Root"})
    if unknown:
        raise _error(f"{path}/{unknown[0]}", f"Unknown component field '{unknown[0]}'. Fields: {', '.join(_COMPONENT_FIELDS)}.",
                     "UBlueprint.UnknownField")
    name = _component_name(row.get("Name", row.get("Key")), f"{path}/Name")
    scope_text = row.get("Scope", "Owned")
    scope = _SCOPES.get(scope_text.casefold()) if type(scope_text) is str else None
    if scope is None:
        raise _error(f"{path}/Scope", "Scope is Owned (default), Inherited or Native.")
    output = {"Name": name}
    if scope != "Owned":
        output["Scope"] = scope
    if scope == "Owned":
        output["Class"] = class_path(row.get("Class"), f"{path}/Class")
        attach = row.get("AttachTo")
        if attach not in (None, ""):
            output["AttachTo"] = _component_name(attach, f"{path}/AttachTo")
        socket = row.get("AttachSocket")
        if socket not in (None, ""):
            if type(socket) is not str or not socket.strip():
                raise _error(f"{path}/AttachSocket", "AttachSocket is a socket or bone name of the AttachTo component.",
                             "UBlueprint.InvalidAttach")
            if "AttachTo" not in output:
                raise _error(f"{path}/AttachSocket", "AttachSocket names a socket of the AttachTo component: give AttachTo too.",
                             "UBlueprint.InvalidAttach")
            output["AttachSocket"] = socket
    elif row.get("AttachTo") not in (None, "") or row.get("AttachSocket") not in (None, ""):
        field = "AttachTo" if row.get("AttachTo") not in (None, "") else "AttachSocket"
        raise _error(f"{path}/{field}", f"{scope} components keep their parent's attachment; {field} is for own components.",
                     "UBlueprint.InvalidAttach")
    # Legacy "Root": true names the scene root; it is the row without AttachTo.
    if row.get("Root", False) not in (True, False):
        raise _error(f"{path}/Root", "Root must be true or false.")
    if row.get("Root") and "AttachTo" in output:
        raise _error(f"{path}/Root", "A Root component cannot have AttachTo.", "UBlueprint.InvalidAttach")
    if "Properties" in row:
        if type(row["Properties"]) is not dict:
            raise _error(f"{path}/Properties", "Properties must be an object of property name -> value.")
        output["Properties"] = dict(row["Properties"])
    return output


def canonical_members(body: dict) -> dict:
    """Validate and convert every present member region to its canonical shape (in place copy)."""
    output = {}
    if "ParentClass" in body:
        value = body["ParentClass"]
        if value not in (None, {}):
            output["ParentClass"] = class_path(value, "/Body/ParentClass")
    if "ImplementedInterfaces" in body:
        rows = _array(body["ImplementedInterfaces"], "/Body/ImplementedInterfaces")
        output["ImplementedInterfaces"] = sorted({class_path(item, f"/Body/ImplementedInterfaces/{index}")
                                                  for index, item in enumerate(rows)})
    if "Variables" in body:
        rows = [_canonical_variable(item, f"/Body/Variables/{index}")
                for index, item in enumerate(_array(body["Variables"], "/Body/Variables"))]
        _unique(rows, "/Body/Variables", lambda row: row["Name"].casefold())
        output["Variables"] = rows
    if "Components" in body:
        rows = [_canonical_component(item, f"/Body/Components/{index}")
                for index, item in enumerate(_array(body["Components"], "/Body/Components"))]
        _unique(rows, "/Body/Components", lambda row: (row.get("Scope", "Owned"), row["Name"].casefold()))
        roots = [index for index, item in enumerate(body["Components"]) if type(item) is dict and item.get("Root")]
        if len(roots) > 1:
            raise _error(f"/Body/Components/{roots[1]}/Root", "Only one component can be the Root.",
                         "UBlueprint.InvalidAttach")
        output["Components"] = rows
    if "ClassDefaults" in body:
        if type(body["ClassDefaults"]) is not dict:
            raise _error("/Body/ClassDefaults", "ClassDefaults must be an object of property name -> value.")
        output["ClassDefaults"] = dict(body["ClassDefaults"])
    return output


def _array(value, path):
    if type(value) is not list:
        raise _error(path, "This region must be an array.")
    return value


def _unique(rows, path, key):
    seen = {}
    for index, row in enumerate(rows):
        token = key(row)
        if token in seen:
            raise _error(f"{path}/{index}/Name", f"'{row['Name']}' is listed twice (also at index {seen[token]}).",
                         "UBlueprint.DuplicateName")
        seen[token] = index


# ---------------------------------------------------------------------------
# Live query -> Body
# ---------------------------------------------------------------------------
def members_from_query(query: dict) -> dict:
    """blueprint.member.query.v1 result -> canonical Body member regions (extract)."""
    body = {}
    if query.get("parent_class"):
        body["ParentClass"] = query["parent_class"]
    body["ImplementedInterfaces"] = sorted(query.get("interfaces", []))
    body["Variables"] = [_variable_from_live(row) for row in query.get("variables", [])]
    components = []
    for row in query.get("components", []):
        item = {"Name": row["name"]}
        if row["scope"] == "owned":
            item["Class"] = row["class"]
            if row.get("attach_to"):
                item["AttachTo"] = row["attach_to"]
            if row.get("attach_socket"):
                item["AttachSocket"] = row["attach_socket"]
        else:
            item["Scope"] = _SCOPES[row["scope"]]
        if row.get("properties"):
            item["Properties"] = dict(row["properties"])
        components.append(item)
    body["Components"] = components
    body["ClassDefaults"] = dict(query.get("class_defaults", {}))
    return body


def _variable_from_live(row: dict) -> dict:
    output = {"Name": row["name"], "Type": type_body(variable_type_dto(row["type"]))}
    if row.get("default") is not None:
        output["DefaultValue"] = row["default"]
    if row.get("category") and row["category"] != DEFAULT_CATEGORY:
        output["Category"] = row["category"]
    if row.get("tooltip"):
        output["Tooltip"] = row["tooltip"]
    for key, live in _VARIABLE_FLAGS.items():
        if row.get(live):
            output[key] = True
    return output


# ---------------------------------------------------------------------------
# Value comparison (authored vs observed)
# ---------------------------------------------------------------------------
def _top_level_items(text: str) -> list[str] | None:
    """The comma-separated items of a parenthesized UE text value, or None (quotes respected)."""
    if len(text) < 2 or text[0] != "(" or text[-1] != ")":
        return None
    items, depth, quoted, start, index = [], 0, False, 1, 1
    while index < len(text) - 1:
        char = text[index]
        if quoted:
            if char == "\\":
                index += 1
            elif char == '"':
                quoted = False
        elif char == '"':
            quoted = True
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth < 0:
                return None
        elif char == "," and depth == 0:
            items.append(text[start:index])
            start = index + 1
        index += 1
    if quoted or depth != 0:
        return None
    items.append(text[start:-1])
    return items


def map_text_entries(text: str) -> list[str] | None:
    """The entries of a map's UE text, ``(("k", "v"),("k2", "v2"))``, or None for any other text.

    A map's export writes each pair as ``(key, value)`` (comma and space); a struct writes ``(A=1,B=2)``.
    """
    entries = _top_level_items(text)
    if not entries or entries == [""]:
        return None
    for entry in entries:
        parts = _top_level_items(entry)
        if parts is None or len(parts) != 2 or not parts[1].startswith(" "):
            return None
    return entries


_WRAPPED_PATH = re.compile(r"^[\w/.]+'(.*)'$")


def map_token(value: object) -> str:
    """One key or value of a map, as its text and its typed JSON forms compare: quotes and a ``Class'path'``
    wrapper left out, a number as its float32, an asset path with its object name (``"/Game/A"`` and
    ``StaticMesh'/Game/A.A'`` are one key; ``0.8`` and ``0.800000`` one value)."""
    if type(value) is bool:
        return "true" if value else "false"
    if type(value) in (int, float):
        return repr(_float32(float(value)))
    if value is None:
        return "None"
    text = str(value).strip()
    if len(text) >= 2 and text[0] == '"' and text[-1] == '"':
        text = text[1:-1].replace('\\"', '"')
    wrapped = _WRAPPED_PATH.match(text)
    if wrapped:
        text = wrapped.group(1)
    if text.lower() in ("true", "false"):
        return text.lower()
    number = number_text(text)
    if number is not None:
        return repr(_float32(number))
    return object_path(text) if text.startswith("/") else text


def _float32(value: float) -> float:
    if not math.isfinite(value) or abs(value) > 3.4028235e38:
        return value
    return struct.unpack("<f", struct.pack("<f", value))[0]


def map_pairs(value: object) -> list[tuple[str, str]] | None:
    """A map value's (key, value) tokens, sorted, from its UE text, a JSON object keyed by its keys, or a list of
    {"Key", "Value"}; None when it is not a map of scalars (a struct value: only the editor reads it)."""
    if type(value) is str:
        entries = map_text_entries(value)
        if entries is None:
            return [] if value.strip() in ("", "()") else None
        pairs = []
        for entry in entries:
            key, item = _top_level_items(entry)
            if item.strip().startswith("("):
                return None
            pairs.append((map_token(key), map_token(item)))
        return sorted(pairs)
    if type(value) is dict:
        items = list(value.items())
    elif type(value) is list and all(type(row) is dict and set(row) == {"Key", "Value"} for row in value):
        items = [(row["Key"], row["Value"]) for row in value]
    else:
        return None
    if any(type(item) in (dict, list) for _, item in items):
        return None
    return sorted((map_token(key), map_token(item)) for key, item in items)


def _is_instance(value: object) -> bool:
    """An instanced object value {"Class", "Properties"} (blueprint.member.query.v1)."""
    return (type(value) is dict and type(value.get("Class")) is str and set(value) <= {"Class", "Properties"}
            and type(value.get("Properties", {})) is dict)


def instance_classes(value: object, out: set | None = None) -> set:
    """Class paths of the instanced object values inside value (any depth)."""
    out = set() if out is None else out
    if type(value) is dict:
        if _is_instance(value):
            out.add(value["Class"])
        for item in value.values():
            instance_classes(item, out)
    elif type(value) is list:
        for item in value:
            instance_classes(item, out)
    return out


def _instances_equal(authored: dict, observed: dict, defaults: dict) -> bool:
    """An instanced value's Properties are read sparse over its class's defaults: a listed property the read
    leaves out matches when it equals that default (``defaults``: class path -> its default object's values)."""
    if not values_equal(authored["Class"], observed["Class"]):
        return False
    have, known = observed.get("Properties", {}), defaults.get(authored["Class"], {})
    for key, value in authored.get("Properties", {}).items():
        if key in have:
            if not values_equal(value, have[key], defaults):
                return False
        elif key not in known or not values_equal(value, known[key], defaults):
            return False
    return True


def values_equal(authored: object, observed: object, defaults: dict | None = None) -> bool:
    """Authored value matches the observed typed value (authored keys only for objects). ``defaults`` (class
    path -> default values, from blueprint.member.query.v1 defaults_of) lets an instanced value list defaults."""
    if type(authored) is bool or type(observed) is bool:
        if type(authored) is str:
            return authored.lower() == str(observed).lower()
        return authored == observed
    if type(authored) in (int, float) and type(observed) in (int, float):
        return math.isclose(authored, observed, rel_tol=1e-5, abs_tol=1e-5)
    if type(authored) is str and type(observed) in (int, float):
        try:
            return math.isclose(float(authored), observed, rel_tol=1e-5, abs_tol=1e-5)
        except ValueError:
            return False
    if type(authored) is str and type(observed) is str:
        if authored == observed or object_path(authored) == object_path(observed):
            return True
        # A map's entries in another order are the same map (FMapProperty::Identical). Compiling a
        # Blueprint reorders a map whose struct default has entries (the class default object is
        # copied by delta over that default: BP_Dynamic_Force_SkelMesh's SocketsandEndpoints). Its keys and
        # values compare as values: quoted or not, an object path with or without its class, numbers as float32.
        entries = map_text_entries(authored)
        if entries is not None:
            others = map_text_entries(observed)
            if others is not None and sorted(entries) == sorted(others):
                return True
            mine, theirs = map_pairs(authored), map_pairs(observed)
            return mine is not None and mine == theirs
        # Enum values: "EFoo::Bar" and "Bar" name the same entry.
        return authored.rsplit("::", 1)[-1] == observed.rsplit("::", 1)[-1] and ("::" in authored) != ("::" in observed)
    if type(authored) is str and type(observed) is dict and len(observed) == 1 and not authored.startswith("("):
        # A struct written by its native text form, read back as its one name field (FKey "SpaceBar" against
        # {"KeyName": "SpaceBar"}, a gameplay tag against {"TagName": ...}): the names compare (FName: any case).
        (value,) = observed.values()
        if type(value) is str:
            return authored.casefold() == value.casefold()
    if type(authored) is str and type(observed) in (dict, list):
        return True  # UE import text of a struct/container; only the editor can parse it.
    if type(authored) in (dict, list) and type(observed) is str and map_text_entries(observed) is not None:
        # A map written as a JSON object (or {"Key", "Value"} rows) against the map's UE text read back.
        mine, theirs = map_pairs(authored), map_pairs(observed)
        return mine is None or mine == theirs
    if type(authored) is dict and type(observed) is dict:
        if defaults and _is_instance(authored) and _is_instance(observed):
            return _instances_equal(authored, observed, defaults)
        return all(key in observed and values_equal(value, observed[key], defaults) for key, value in authored.items())
    if type(authored) is list and type(observed) is list:
        return len(authored) == len(observed) and all(values_equal(a, b, defaults) for a, b in zip(authored, observed))
    return authored == observed


def _type_key(value: object) -> tuple:
    dto = variable_type_dto(type_dto(value, "/Type"))
    inner = dto.get("value_type", {})
    return (dto["category"], dto["sub_category"], dto["sub_category_object"], dto["container"], dto["is_ref"],
            dto["is_const"], tuple(sorted(inner.items())))


# ---------------------------------------------------------------------------
# Planning: desired Body + live query -> ordered primitive calls (primitive, values)
# ---------------------------------------------------------------------------
def plan_header(properties: dict, body: dict, query: dict) -> list:
    ops = []
    live = query.get("properties", {})
    for key in sorted(properties):
        if key not in live or not values_equal(properties[key], live[key]):
            ops.append(("blueprint.member.property.set.v1",
                        {"target": "blueprint", "property": key, "value": properties[key]}, f"/Properties/{key}"))
    if "ParentClass" in body and body["ParentClass"] != query.get("parent_class"):
        ops.append(("blueprint.member.parent.set.v1", {"parent_class": body["ParentClass"]}, "/Body/ParentClass"))
    return ops


def plan_interfaces(body: dict, query: dict) -> list:
    if "ImplementedInterfaces" not in body:
        return []
    desired, live = body["ImplementedInterfaces"], query.get("interfaces", [])
    ops = [("blueprint.member.interface.remove.v1", {"interface": item}, "/Body/ImplementedInterfaces")
           for item in live if item not in desired]
    ops += [("blueprint.member.interface.add.v1", {"interface": item},
             f"/Body/ImplementedInterfaces/{desired.index(item)}") for item in desired if item not in live]
    return ops


def plan_variables(body: dict, query: dict) -> list:
    if "Variables" not in body:
        return []
    desired = body["Variables"]
    live = {row["name"].casefold(): row for row in query.get("variables", [])}
    wanted = {row["Name"].casefold() for row in desired}
    ops = [("blueprint.member.variable.remove.v1", {"name": row["name"]}, "/Body/Variables")
           for key, row in live.items() if key not in wanted]
    for index, row in enumerate(desired):
        current = live.get(row["Name"].casefold())
        values = _variable_values(row, current)
        if values is not None:
            ops.append(("blueprint.member.variable.set.v1", values, f"/Body/Variables/{index}"))
    return ops


def _variable_values(row: dict, current: dict | None) -> dict | None:
    dto = type_dto(row["Type"], "/Type")
    values = {"name": row["Name"], "type": dto}
    changed = current is None or _type_key(row["Type"]) != _type_key(type_body(current["type"]))
    # A listed row is the whole variable: omitted metadata means its default (the extract shape).
    category = row.get("Category") or DEFAULT_CATEGORY
    if current is None and "Category" in row or current is not None and (
            current.get("category") or DEFAULT_CATEGORY) != category:
        values["category"], changed = category, True
    tooltip = row.get("Tooltip", "")
    if current is None and tooltip or current is not None and (current.get("tooltip") or "") != tooltip:
        values["tooltip"], changed = tooltip, True
    for key, live_key in _VARIABLE_FLAGS.items():
        flag = row.get(key, False)
        if current is None and key in row or current is not None and bool(current.get(live_key)) != flag:
            values[live_key], changed = flag, True
    if "DefaultValue" in row and row["DefaultValue"] is not None and (
            changed or current is None
            or not _same_default(row["DefaultValue"], current.get("default"), current.get("default_name"))):
        values["default"], changed = row["DefaultValue"], True
    return values if changed else None


def _live_components(query: dict) -> dict:
    return {(row["scope"], row["name"].casefold()): row for row in query.get("components", [])}


def _owned(body: dict) -> list:
    return [row for row in body.get("Components", []) if "Scope" not in row]


def plan_component_structure(body: dict, query: dict) -> list:
    """Remove surplus/changed own components, then add new ones parents-first."""
    if "Components" not in body:
        return []
    live = _live_components(query)
    desired = _owned(body)
    by_name = {row["Name"].casefold(): row for row in desired}
    ops, kept = [], set()
    for (scope, key), row in live.items():
        if scope != "owned":
            continue
        want = by_name.get(key)
        if want is None or want["Class"] != row["class"]:
            ops.append(("blueprint.member.component.remove.v1", {"name": row["name"], "scope": "owned"}, "/Body/Components"))
        else:
            kept.add(key)
    added: set[str] = set()
    pending = [row for row in desired if row["Name"].casefold() not in kept]
    while pending:
        progressed = False
        for row in list(pending):
            parent = row.get("AttachTo", "").casefold()
            if parent in by_name and parent not in kept and parent not in added:
                continue
            values = {"name": row["Name"], "class": row["Class"]}
            if row.get("AttachTo"):
                values["attach_to"] = row["AttachTo"]
            index = body["Components"].index(row)
            ops.append(("blueprint.member.component.add.v1", values, f"/Body/Components/{index}"))
            added.add(row["Name"].casefold())
            pending.remove(row)
            progressed = True
        if not progressed:
            row = pending[0]
            raise _error(f"/Body/Components/{body['Components'].index(row)}/AttachTo",
                         "Components are attached in a cycle.", "UBlueprint.InvalidAttach")
    return ops


def plan_component_attach(body: dict, query: dict) -> list:
    """Reattachments (a reattach clears the socket; plan_component_sockets sets it after the component values)."""
    if "Components" not in body:
        return []
    live = _live_components(query)
    names = native_component_names(query)
    ops = []
    for index, row in enumerate(body["Components"]):
        current = live.get(("owned", row["Name"].casefold())) if "Scope" not in row else None
        if current is None:
            continue
        want = row.get("AttachTo", "")
        if _attach_differs(want, current.get("attach_to", ""), names):
            values = {"name": row["Name"]}
            if want:
                values["attach_to"] = want
            ops.append(("blueprint.member.component.attach.v1", values, f"/Body/Components/{index}/AttachTo"))
    return ops


def plan_component_sockets(body: dict, query: dict) -> list:
    """AttachSocket (SCS AttachToName), after the component values: the editor's socket picker lists the sockets of
    the parent's mesh, which the same document may set."""
    if "Components" not in body:
        return []
    live = _live_components(query)
    ops = []
    for index, row in enumerate(body["Components"]):
        current = live.get(("owned", row["Name"].casefold())) if "Scope" not in row else None
        if current is None or not _socket_differs(row.get("AttachSocket", ""), current.get("attach_socket", "")):
            continue
        values = {"name": row["Name"], "attach_socket": row.get("AttachSocket", "")}
        if row.get("AttachTo"):
            values["attach_to"] = row["AttachTo"]
        ops.append(("blueprint.member.component.attach.v1", values, f"/Body/Components/{index}/AttachSocket"))
    return ops


def _property_ops(target: dict, desired: dict, live: dict, path: str, defaults: dict | None = None) -> list:
    ops = []
    for key in desired:
        if key not in live or not values_equal(desired[key], live[key], defaults):
            ops.append(("blueprint.member.property.set.v1", {**target, "property": key, "value": desired[key]},
                        f"{path}/{key}"))
    for key in live:
        if key not in desired:
            ops.append(("blueprint.member.property.set.v1", {**target, "property": key, "reset": True}, path))
    return ops


def plan_values(body: dict, query: dict) -> list:
    """Component property overrides and ClassDefaults (after structure and graphs)."""
    return plan_component_values(body, query) + plan_class_defaults(body, query)


def plan_component_values(body: dict, query: dict, defaults: dict | None = None) -> list:
    """Component property overrides (after the component structure). ``defaults``: see ``values_equal``."""
    ops = []
    if "Components" in body:
        live = _live_components(query)
        native_alias = {row.get("subobject", "").casefold(): key for key, row in live.items() if row["scope"] == "native"}
        listed = set()
        for index, row in enumerate(body["Components"]):
            scope = row.get("Scope", "Owned").lower()
            key = (scope, row["Name"].casefold())
            if scope == "native" and key not in live and row["Name"].casefold() in native_alias:
                key = native_alias[row["Name"].casefold()]
            listed.add(key)
            if "Properties" not in row:
                continue
            current = live.get(key, {}).get("properties", {})
            target = {"target": "component", "component": {"name": row["Name"], "scope": scope}}
            ops += _property_ops(target, row["Properties"], current, f"/Body/Components/{index}/Properties", defaults)
        for key, row in live.items():
            if key in listed or row["scope"] == "owned":
                continue
            if row["scope"] == "inherited":
                ops.append(("blueprint.member.component.remove.v1", {"name": row["name"], "scope": "inherited"},
                            "/Body/Components"))
            else:
                target = {"target": "component", "component": {"name": row["name"], "scope": "native"}}
                ops += _property_ops(target, {}, row.get("properties", {}), "/Body/Components")
    return ops


def plan_class_defaults(body: dict, query: dict, defaults: dict | None = None) -> list:
    """ClassDefaults (after the graphs: the defaults of the compiled class). ``defaults``: see ``values_equal``."""
    ops = []
    if "ClassDefaults" in body:
        ops += _property_ops({"target": "class_defaults"}, body["ClassDefaults"], query.get("class_defaults", {}),
                             "/Body/ClassDefaults", defaults)
    return ops


# ---------------------------------------------------------------------------
# Diff: authored canonical members vs extracted canonical members
# ---------------------------------------------------------------------------
def _replace(path, before, after_present, after):
    return Difference(path, DiffKind.REPLACE, True, before, after_present, after if after_present else None)


def _props_differ(desired: dict, live: dict, strict: bool = False, defaults: dict | None = None) -> bool:
    # A property absent from the extraction equals the parent value; authored defaults are accepted
    # (strict Diff cannot tell an authored default from an edit, so it reports it).
    return any((key in live or strict) and not (key in live and values_equal(value, live[key], defaults))
               for key, value in desired.items()) or any(key not in desired for key in live)


def diff_members(authored: dict, extracted: dict, properties: dict, extracted_properties: dict,
                 strict: bool = False, defaults: dict | None = None) -> list:
    """Member differences. ``strict`` (the Diff operation) also reports authored values the
    extraction omits, so an empty strict Diff means applying the document changes nothing.
    ``defaults``: the default values of the instanced objects' classes (see ``values_equal``)."""
    out = []
    for key, value in properties.items():
        if key not in extracted_properties:
            if strict:
                out.append(_replace(f"/Properties/{key}", value, False, None))
        elif not values_equal(value, extracted_properties[key]):
            out.append(_replace(f"/Properties/{key}", value, True, extracted_properties[key]))
    if "ParentClass" in authored and authored["ParentClass"] != extracted.get("ParentClass"):
        out.append(_replace("/Body/ParentClass", authored["ParentClass"], "ParentClass" in extracted,
                            extracted.get("ParentClass")))
    if "ImplementedInterfaces" in authored and authored["ImplementedInterfaces"] != extracted["ImplementedInterfaces"]:
        out.append(_replace("/Body/ImplementedInterfaces", authored["ImplementedInterfaces"], True,
                            extracted["ImplementedInterfaces"]))
    if "Variables" in authored and not _variables_match(authored["Variables"], extracted["Variables"], strict,
                                                        (defaults or {}).get(ENUM_NAMES)):
        out.append(_replace("/Body/Variables", authored["Variables"], True, extracted["Variables"]))
    if "Components" in authored:
        out += _component_differences(authored["Components"], extracted["Components"], strict, defaults)
    if "ClassDefaults" in authored and _props_differ(authored["ClassDefaults"], extracted["ClassDefaults"], strict,
                                                     defaults):
        out.append(_replace("/Body/ClassDefaults", authored["ClassDefaults"], True, extracted["ClassDefaults"]))
    return out


def _variables_match(desired: list, live: list, strict: bool = False, enum_names: dict | None = None) -> bool:
    by_name = {row["Name"].casefold(): row for row in live}
    enum_names = enum_names or {}
    if set(by_name) != {row["Name"].casefold() for row in desired}:
        return False
    for row in desired:
        current = by_name[row["Name"].casefold()]
        if current["Name"] != row["Name"]:  # a case-only rename is a change (blueprint.member.rename.v1)
            return False
        if _type_key(row["Type"]) != _type_key(current["Type"]):
            return False
        if "DefaultValue" in row and row["DefaultValue"] is not None:
            if "DefaultValue" in current and not _same_default(row["DefaultValue"], current["DefaultValue"],
                                                               enum_names.get(row["Name"].casefold())):
                return False
            if strict and "DefaultValue" not in current:
                return False
        if (row.get("Category") or DEFAULT_CATEGORY) != current.get("Category", DEFAULT_CATEGORY):
            return False
        if row.get("Tooltip", "") != current.get("Tooltip", ""):
            return False
        if any(row.get(key, False) != current.get(key, False) for key in _VARIABLE_FLAGS):
            return False
    return True


NATIVE_NAMES = "@NativeComponentNames"
ENUM_NAMES = "@VariableEnumNames"


def variable_enum_names(query: dict) -> dict:
    """{variable name casefolded: enumerator name} of variables holding a User Defined Enum value, which
    reads as its display name (``default``) while its enumerator name (``default_name``) is the same value."""
    return {row["name"].casefold(): row["default_name"] for row in query.get("variables", [])
            if type(row.get("default_name")) is str}


def _same_default(authored, row_default, enum_name) -> bool:
    return values_equal(authored, row_default) or (
        type(authored) is str and type(enum_name) is str and authored.casefold() == enum_name.casefold())


def native_component_names(query: dict) -> dict:
    """How AttachTo names the parent's native scene components (blueprint.member.query.v1 header):
    {"aliases": {subobject name casefolded: component name}, "root": the native root's component name, else the
    owned scene root's}. A Character's capsule is the subobject CollisionCylinder of the component CapsuleComponent
    (either names it), and an owned component without a parent attaches to the capsule (an omitted AttachTo). Without
    a native root, a scene component without a parent attaches to the owned scene root, as the Components panel's
    Add does (an SCS has one root)."""
    names = query.get("native_component_names")
    aliases = {key.casefold(): value for key, value in names.items() if type(value) is str} if type(names) is dict else {}
    root = query.get("scene_root") if type(query.get("scene_root")) is str else ""
    return {"aliases": aliases, "root": root} if aliases or root else {}


def _socket_differs(want: str, have: str) -> bool:
    """Socket names are names (case-insensitive); none is no socket."""
    return (want or "").casefold() != (have or "").casefold()


def _attach_key(name: str, aliases: dict) -> str:
    folded = (name or "").casefold()
    return aliases.get(folded, name or "").casefold()


def _attach_differs(want: str, have: str, names: dict) -> bool:
    """AttachTo values naming different parents. An omitted AttachTo is the scene root: the editor's
    DefaultSceneRoot, the parent class's native root component or the owned scene root."""
    aliases = names.get("aliases", {})
    if _attach_key(want, aliases) == _attach_key(have, aliases):
        return False
    if want:
        return True
    roots = {"defaultsceneroot", _attach_key(names.get("root", ""), aliases)} - {""}
    return _attach_key(have, aliases) not in roots


def _component_differences(desired: list, live: list, strict: bool = False, defaults: dict | None = None) -> list:
    """Component differences, matched by (Scope, Name) whatever the order: one per component, at the first field
    that differs (AttachTo, a property), with the read-back value of that field."""
    names = (defaults or {}).get(NATIVE_NAMES, {})
    live_by = {(row.get("Scope", "Owned"), row["Name"].casefold()): row for row in live}
    out, listed = [], set()
    for index, row in enumerate(desired):
        path = f"/Body/Components/{index}"
        key = (row.get("Scope", "Owned"), row["Name"].casefold())
        listed.add(key)
        current = live_by.get(key)
        if current is None:
            if "Scope" not in row:
                out.append(_replace(path, row, False, None))
            elif "Properties" in row and _props_differ(row["Properties"], {}, strict, defaults):
                out.append(_replace(f"{path}/Properties", row["Properties"], False, None))
            continue
        if "Scope" not in row:
            if current.get("Class") != row["Class"] or current.get("Name") != row["Name"]:
                out.append(_replace(path, row, True, current))
                continue
            if _attach_differs(row.get("AttachTo", ""), current.get("AttachTo", ""), names):
                out.append(_replace(f"{path}/AttachTo", row.get("AttachTo", ""), True, current.get("AttachTo", "")))
                continue
            if _socket_differs(row.get("AttachSocket", ""), current.get("AttachSocket", "")):
                out.append(_replace(f"{path}/AttachSocket", row.get("AttachSocket", ""), True,
                                    current.get("AttachSocket", "")))
                continue
        if "Properties" in row:
            have = current.get("Properties", {})
            for name, value in row["Properties"].items():
                if (name in have or strict) and not (name in have and values_equal(value, have[name], defaults)):
                    out.append(_replace(f"{path}/Properties/{name}", value, name in have, have.get(name)))
                    break
            else:
                extra = sorted(name for name in have if name not in row["Properties"])
                if extra:
                    out.append(Difference(f"{path}/Properties/{extra[0]}", DiffKind.ADD, False, None, True, have[extra[0]]))
    for key, row in live_by.items():
        if key in listed:
            continue
        # A component the document does not list: an own one is surplus; overrides of an inherited / native one
        # are reset by apply.
        if key[0] == "Owned" or row.get("Properties"):
            out.append(Difference("/Body/Components/-", DiffKind.ADD, False, None, True, row))
    return out


def _components_match(desired: list, live: list, strict: bool = False, defaults: dict | None = None) -> bool:
    return not _component_differences(desired, live, strict, defaults)
