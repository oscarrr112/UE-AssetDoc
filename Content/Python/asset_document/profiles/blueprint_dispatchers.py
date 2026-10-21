"""Event dispatchers of a Blueprint (UBlueprint / WidgetBlueprint / AnimBlueprint), pure.

Body region::

    "EventDispatchers": [
      {"Name": "OnDied", "Inputs": [{"Name": "Killer", "Type": "object:/Script/Engine.Actor"}],
       "Category": "Combat", "Tooltip": "Broadcast when health reaches zero."}
    ]

An event dispatcher is the editor's "+ Event Dispatcher": a multicast delegate member variable
together with its signature graph; ``blueprint.member.dispatcher.set.v1`` / ``.remove.v1`` create
and remove both, like the editor.  A listed row is the whole dispatcher: omitted Inputs mean
none, omitted Category means "Default", omitted Tooltip means none (the extract omits defaults).
An omitted region is preserved; a present region is the complete list (unlisted dispatchers are
removed).  Dispatchers have no replication setting (the editor disables it).

The AnimBlueprint key ``DelegateSignatureGraphs`` is accepted as an alias: rows in this shape,
or in the legacy graph shape ``{"Name", "Signature": {"Inputs": [...]}, "Nodes": [...]}`` whose
Nodes may only be the signature graph's own entry/result nodes.

Graph nodes reach a dispatcher by action key: ``K2:CallDelegate:OnDied`` (Call),
``K2:AddDelegate:OnDied`` (Bind), ``K2:RemoveDelegate:OnDied`` (Unbind), ``K2:ClearDelegate:OnDied``
(Unbind all), ``K2:AssignDelegate:OnDied`` (Assign, with its linked event) and
``K2:CustomEvent:OnDied`` (an event with the dispatcher's signature, named ``OnDied_Event``).
"""
from __future__ import annotations

from asset_document.diagnostics import contract_error
from asset_document.diff.model import Difference, DiffKind
from asset_document.graphs.model import PinType
from asset_document.graphs.schemas.k2 import decode_pin_type, encode_pin_type

KEY = "EventDispatchers"
ALIAS = "DelegateSignatureGraphs"
DEFAULT_CATEGORY = "Default"
PRIMITIVES = ("dispatcher.set", "dispatcher.remove")
_FIELDS = ("Name", "Inputs", "Category", "Tooltip")
_GRAPH_FIELDS = frozenset(("Name", "Signature", "Nodes", "Links", "GraphGuid", "Schema", "Category", "Tooltip"))
_SIGNATURE_FIELDS = frozenset(("Inputs", "Outputs", "Flags", "Category", "Tooltip"))


def _error(path, message, code="UBlueprint.InvalidBodyRegion"):
    return contract_error(path, code, message)


def _text(row: dict, key: str, path: str) -> str | None:
    if key not in row:
        return None
    if type(row[key]) is not str:
        raise _error(f"{path}/{key}", f"{key} must be a string.")
    return row[key]


def _inputs(value: object, path: str) -> list:
    if type(value) is not list:
        raise _error(path, "Inputs is an array of {\"Name\", \"Type\"}.")
    out, seen = [], {}
    for index, item in enumerate(value):
        item_path = f"{path}/{index}"
        if type(item) is not dict:
            raise _error(item_path, "An input is an object {\"Name\", \"Type\"}.")
        unknown = sorted(set(item) - {"Name", "Type"})
        if unknown:
            raise _error(f"{item_path}/{unknown[0]}", f"Unknown input field '{unknown[0]}'. Fields: Name, Type "
                         "(dispatcher inputs have no default value).", "UBlueprint.UnknownField")
        name = item.get("Name")
        if type(name) is not str or not name.strip():
            raise _error(f"{item_path}/Name", "Input Name must be a non-empty string.")
        if name.casefold() in seen:
            raise _error(f"{item_path}/Name", f"Input '{name}' is listed twice (also at index {seen[name.casefold()]}).",
                         "UBlueprint.DuplicateName")
        seen[name.casefold()] = index
        if "Type" not in item:
            raise _error(f"{item_path}/Type", "Input Type is required (for example \"float\" or \"object:/Script/Engine.Actor\").")
        out.append({"Name": name, "Type": encode_pin_type(decode_pin_type(item["Type"], f"{item_path}/Type"))})
    return out


def _row(value: object, path: str, *, legacy: bool) -> dict:
    if type(value) is not dict:
        raise _error(path, "An event dispatcher is an object with Name (and Inputs, Category, Tooltip).")
    if legacy and ("Signature" in value or "Nodes" in value or "Links" in value):
        return _legacy_row(value, path)
    unknown = sorted(set(value) - set(_FIELDS))
    if unknown:
        raise _error(f"{path}/{unknown[0]}", f"Unknown event dispatcher field '{unknown[0]}'. Fields: {', '.join(_FIELDS)}.",
                     "UBlueprint.UnknownField")
    name = value.get("Name")
    if type(name) is not str or not name.strip():
        raise _error(f"{path}/Name", "Event dispatcher Name must be a non-empty string.")
    out = {"Name": name}
    if "Inputs" in value:
        inputs = _inputs(value["Inputs"], f"{path}/Inputs")
        if inputs:
            out["Inputs"] = inputs
    category = _text(value, "Category", path)
    if category and category != DEFAULT_CATEGORY:
        out["Category"] = category
    tooltip = _text(value, "Tooltip", path)
    if tooltip:
        out["Tooltip"] = tooltip
    return out


def _legacy_row(value: dict, path: str) -> dict:
    """The legacy AnimBlueprint graph shape of a delegate signature graph."""
    unknown = sorted(set(value) - _GRAPH_FIELDS)
    if unknown:
        raise _error(f"{path}/{unknown[0]}", f"Unknown delegate signature graph field '{unknown[0]}'.",
                     "UBlueprint.UnknownField")
    signature = value.get("Signature", {})
    if type(signature) is not dict:
        raise _error(f"{path}/Signature", "Signature must be an object.")
    unknown = sorted(set(signature) - _SIGNATURE_FIELDS)
    if unknown:
        raise _error(f"{path}/Signature/{unknown[0]}", f"Unknown signature field '{unknown[0]}'.", "UBlueprint.UnknownField")
    if signature.get("Outputs") not in (None, []):
        raise _error(f"{path}/Signature/Outputs", "Event dispatchers have no outputs.")
    if value.get("Links") not in (None, []):
        raise _error(f"{path}/Links", "A delegate signature graph holds no links.")
    nodes = value.get("Nodes", [])
    if type(nodes) is not list:
        raise _error(f"{path}/Nodes", "Nodes must be an array.")
    for index, node in enumerate(nodes):
        if type(node) is not dict or not (node.get("Implicit") in ("entry", "result")
                                          or node.get("Id") in ("Entry", "Result") and "Action" not in node):
            raise _error(f"{path}/Nodes/{index}", "A delegate signature graph holds only its entry and result nodes.")
    row = {"Name": value.get("Name")}
    for key in ("Category", "Tooltip"):
        if key in value or key in signature:
            row[key] = value.get(key, signature.get(key))
    if "Inputs" in signature:
        row["Inputs"] = [{"Name": pin.get("Name"), "Type": pin.get("Type")} if type(pin) is dict else pin
                         for pin in signature["Inputs"]] if type(signature["Inputs"]) is list else signature["Inputs"]
    return _row(row, path, legacy=False)


def canonical(body: dict) -> list | None:
    """The canonical EventDispatchers rows of a Body (``None`` when the region is omitted)."""
    if KEY in body and ALIAS in body:
        raise _error(f"/Body/{ALIAS}", f"{ALIAS} is the legacy name of {KEY}; give one of them.",
                     "UBlueprint.DuplicateRegion")
    key = KEY if KEY in body else ALIAS if ALIAS in body else None
    if key is None or body[key] is None:
        return None
    if type(body[key]) is not list:
        raise _error(f"/Body/{key}", "This region must be an array.")
    rows = [_row(item, f"/Body/{key}/{index}", legacy=key == ALIAS) for index, item in enumerate(body[key])]
    seen = {}
    for index, row in enumerate(rows):
        token = row["Name"].casefold()
        if token in seen:
            raise _error(f"/Body/{key}/{index}/Name", f"'{row['Name']}' is listed twice (also at index {seen[token]}).",
                         "UBlueprint.DuplicateName")
        seen[token] = index
    return rows


def check_names(body: dict) -> None:
    """A dispatcher is a member variable: its name cannot be a listed variable's name too."""
    variables = {row["Name"].casefold() for row in body.get("Variables", [])}
    for index, row in enumerate(body.get(KEY) or []):
        if row["Name"].casefold() in variables:
            raise _error(f"/Body/{KEY}/{index}/Name", f"'{row['Name']}' is both a variable and an event dispatcher.",
                         "UBlueprint.DuplicateName")


# ---------------------------------------------------------------------------
# Live query -> Body
# ---------------------------------------------------------------------------
def _pin(dto: dict) -> PinType:
    return PinType(dto["category"], dto.get("sub_category", ""), dto.get("sub_category_object", ""),
                   dto.get("container", "none"), dto.get("is_ref", False), dto.get("is_const", False))


def from_query(query: dict) -> list:
    rows = []
    for item in query.get("dispatchers", []):
        row = {"Name": item["name"]}
        if item.get("inputs"):
            row["Inputs"] = [{"Name": pin["name"], "Type": encode_pin_type(_pin(pin["type"]))} for pin in item["inputs"]]
        if item.get("category") and item["category"] != DEFAULT_CATEGORY:
            row["Category"] = item["category"]
        if item.get("tooltip"):
            row["Tooltip"] = item["tooltip"]
        rows.append(row)
    return rows


# ---------------------------------------------------------------------------
# Planning and comparison
# ---------------------------------------------------------------------------
def _input_key(rows: list) -> tuple:
    return tuple((row["Name"], decode_pin_type(row["Type"], "/Type")) for row in rows)


def _inputs_dto(rows: list) -> list:
    return [{"name": row["Name"], "type": decode_pin_type(row["Type"], "/Type").to_dto()} for row in rows]


def plan_removes(body: dict, query: dict) -> list:
    if body.get(KEY) is None:
        return []
    wanted = {row["Name"].casefold() for row in body[KEY]}
    return [("blueprint.member.dispatcher.remove.v1", {"name": item["name"]}, f"/Body/{KEY}")
            for item in query.get("dispatchers", []) if item["name"].casefold() not in wanted]


def plan_sets(body: dict, query: dict) -> list:
    if body.get(KEY) is None:
        return []
    live = {row["Name"].casefold(): row for row in from_query(query)}
    ops = []
    for index, row in enumerate(body[KEY]):
        current = live.get(row["Name"].casefold())
        values = {"name": row["Name"]}
        if current is None or _input_key(row.get("Inputs", [])) != _input_key(current.get("Inputs", [])):
            if current is not None or row.get("Inputs"):
                values["inputs"] = _inputs_dto(row.get("Inputs", []))
        if (current or {}).get("Category", DEFAULT_CATEGORY) != row.get("Category", DEFAULT_CATEGORY):
            values["category"] = row.get("Category", DEFAULT_CATEGORY)
        if (current or {}).get("Tooltip", "") != row.get("Tooltip", ""):
            values["tooltip"] = row.get("Tooltip", "")
        if current is None or len(values) > 1:
            ops.append(("blueprint.member.dispatcher.set.v1", values, f"/Body/{KEY}/{index}"))
    return ops


def rows_match(authored: list, live: list) -> bool:
    have = {row["Name"].casefold(): row for row in live}
    if set(have) != {row["Name"].casefold() for row in authored}:
        return False
    for row in authored:
        other = have[row["Name"].casefold()]
        if other["Name"] != row["Name"]:  # a case-only rename is a change (blueprint.member.rename.v1)
            return False
        if _input_key(row.get("Inputs", [])) != _input_key(other.get("Inputs", [])):
            return False
        if row.get("Category", DEFAULT_CATEGORY) != other.get("Category", DEFAULT_CATEGORY):
            return False
        if row.get("Tooltip", "") != other.get("Tooltip", ""):
            return False
    return True


def diff(body: dict, live: dict) -> list:
    if body.get(KEY) is None or rows_match(body[KEY], live.get(KEY, [])):
        return []
    return [Difference(f"/Body/{KEY}", DiffKind.REPLACE, True, body[KEY], True, live.get(KEY, []))]
