"""Widget Blueprint Designer regions (pure): widget tree, slots, navigation and property bindings.

Body regions (AI-friendly canonical shapes)::

    "WidgetTree": {
        "RootWidget": {"Name": "Root", "Class": "/Script/UMG.CanvasPanel", "IsVariable": false,
                       "Children": [
                           {"Name": "Title", "Class": "/Script/UMG.TextBlock",
                            "Properties": {"Text": "Hello", "ColorAndOpacity": {...}},
                            "Slot": {"LayoutData": {...}, "ZOrder": 2}},
                           {"Name": "Ok", "Class": "/Script/UMG.Button",
                            "Navigation": {"Down": {"Rule": "Explicit", "WidgetToFocus": "Cancel"}},
                            "Children": [{"Name": "OkLabel", "Class": "/Script/UMG.TextBlock"}]},
                           {"Name": "Card", "Class": "/Game/UI/WBP_Card.WBP_Card_C",
                            "NamedSlots": {"Body": {"Name": "CardBody", "Class": "/Script/UMG.Image"}}}]},
        "NamedSlotBindings": {"HeaderSlot": {"Name": "Header", "Class": "/Script/UMG.TextBlock"}}}
    "Bindings": [{"Widget": "Title", "Property": "Text", "Kind": "Function", "Function": "GetTitle"},
                 {"Widget": "Bar", "Property": "Percent", "Kind": "Property", "SourcePath": ["Health"]}]

Omission: an omitted region (or RootWidget / NamedSlotBindings sub-key) is preserved; a present
one is the complete desired state (``null`` / ``{}`` / ``[]`` clear it).  A listed node is the
whole widget: omitted Properties / Slot / Navigation / Children / NamedSlots mean "none" (the
class / slot defaults), an omitted IsVariable keeps the current flag (a new widget gets the
palette default of its class).  Properties and Slot list the values that differ from the class
default; unlisted values are reset.  Widgets are matched by Name; a Class change replaces the
widget (graph references to the name are kept).  ``NamedSlotBindings`` fills the named slots
the parent class exposes (inherited named slots).

Renames: ``"RenamedFrom": "OldName"`` on a widget is the Hierarchy / Details name box
(FWidgetBlueprintEditorUtils::RenameWidget): the same widget takes the new name and its variable
GUID, bindings, animation bindings, desired focus, navigation targets and graph references follow,
including the references in the loaded Blueprints that use this one (left unsaved, as in the editor;
reported as ``Widget.DependentsUpdated``).  It is ignored once the rename happened.  A name that
differs from a live widget only by case renames it without ``RenamedFrom``; any other new name
without it is remove + add.
"""
from __future__ import annotations

from asset_document.diagnostics import contract_error
from asset_document.diff.model import Difference, DiffKind
from asset_document.profiles import ublueprint_members as members

NODE_KEYS = ("Name", "Class", "IsVariable", "VariableName", "Properties", "Slot", "Navigation", "Children",
             "NamedSlots", "RenamedFrom")
DIRECTIONS = ("Up", "Down", "Left", "Right", "Next", "Previous")
NAV_RULES = ("Escape", "Explicit", "Wrap", "Stop", "Custom", "CustomBoundary", "Invalid")
TREE = "umg.tree"


def _error(path, message, code="Widget.InvalidWidgetTree"):
    return contract_error(path, code, message)


# ---------------------------------------------------------------------------
# Canonical shapes
# ---------------------------------------------------------------------------
def _canonical_navigation(value, path) -> dict:
    if type(value) is not dict:
        raise _error(path, "Navigation is an object of direction -> {Rule, WidgetToFocus}.", "Widget.InvalidNavigation")
    out = {}
    for key, entry in value.items():
        if key == "RoutingPolicy":
            if type(entry) is not str:
                raise _error(f"{path}/RoutingPolicy", "RoutingPolicy is an EWidgetNavigationRoutingPolicy name.",
                             "Widget.InvalidNavigation")
            entry = entry.rsplit("::", 1)[-1]
            if entry != "Default":
                out["RoutingPolicy"] = entry
            continue
        if key not in DIRECTIONS:
            raise _error(f"{path}/{key}", f"Unknown navigation direction '{key}'. Directions: {', '.join(DIRECTIONS)}"
                         ", RoutingPolicy.", "Widget.InvalidNavigation")
        if type(entry) is str:
            entry = {"Rule": entry}
        if type(entry) is not dict or set(entry) - {"Rule", "WidgetToFocus"} or type(entry.get("Rule")) is not str:
            raise _error(f"{path}/{key}", "A direction is {\"Rule\": \"Explicit\", \"WidgetToFocus\": \"Name\"}.",
                         "Widget.InvalidNavigation")
        rule = entry["Rule"].rsplit("::", 1)[-1]
        if rule not in NAV_RULES:
            raise _error(f"{path}/{key}/Rule", f"Rule is one of {', '.join(NAV_RULES)}.", "Widget.InvalidNavigation")
        focus = entry.get("WidgetToFocus", "")
        if type(focus) is not str:
            raise _error(f"{path}/{key}/WidgetToFocus", "WidgetToFocus is a widget or function name.",
                         "Widget.InvalidNavigation")
        if rule == "Escape" and not focus:
            continue  # the default
        out[key] = {"Rule": rule, **({"WidgetToFocus": focus} if focus else {})}
    return out


def _object(value, path, label):
    if type(value) is not dict:
        raise _error(path, f"{label} must be an object of property name -> value.")
    return dict(value)


def canonical_node(value, path: str, names: dict, paths: dict) -> dict:
    if type(value) is not dict:
        raise _error(path, "A widget is {\"Name\": ..., \"Class\": ...}.")
    for key in value:
        if key not in NODE_KEYS:
            raise _error(f"{path}/{key}", f"Unknown widget key '{key}'. Keys: {', '.join(NODE_KEYS[:3] + NODE_KEYS[4:])}.",
                         "Widget.UnknownField")
    name = value.get("Name")
    if type(name) is not str or not name or any(ch in name for ch in " ./:\"'"):
        raise _error(f"{path}/Name", "Name is a widget name without spaces, dots or quotes.", "Widget.InvalidName")
    folded = name.casefold()
    if folded in names:
        raise _error(f"{path}/Name", f"Widget '{name}' is listed twice (also at {names[folded]}).", "Widget.DuplicateName")
    names[folded] = path
    paths[name] = path
    if "RenamedFrom" in value:
        source = value["RenamedFrom"]
        if type(source) is not str or not source or any(ch in source for ch in " ./:\"'") or source == name:
            raise _error(f"{path}/RenamedFrom", "RenamedFrom is the widget's previous name (another name, without "
                         "spaces, dots or quotes).", "Widget.InvalidRename")
    if "VariableName" in value and value["VariableName"] != name:
        raise _error(f"{path}/VariableName", "VariableName must equal Name (a widget's variable is its name).",
                     "Widget.InvalidName")
    out = {"Name": name, "Class": members.class_path(value.get("Class"), f"{path}/Class")}
    if "RenamedFrom" in value:
        out["RenamedFrom"] = value["RenamedFrom"]
    if "IsVariable" in value:
        if type(value["IsVariable"]) is not bool:
            raise _error(f"{path}/IsVariable", "IsVariable is true or false.")
        out["IsVariable"] = value["IsVariable"]
    for key, label in (("Properties", "Properties"), ("Slot", "Slot")):
        if value.get(key):
            out[key] = _object(value[key], f"{path}/{key}", label)
    if value.get("Navigation"):
        navigation = _canonical_navigation(value["Navigation"], f"{path}/Navigation")
        if navigation:
            out["Navigation"] = navigation
    children = value.get("Children", [])
    if type(children) is not list:
        raise _error(f"{path}/Children", "Children is an array of widgets.")
    if children:
        out["Children"] = [canonical_node(child, f"{path}/Children/{index}", names, paths)
                           for index, child in enumerate(children)]
    slots = value.get("NamedSlots", {})
    if type(slots) is not dict:
        raise _error(f"{path}/NamedSlots", "NamedSlots is an object of slot name -> widget.")
    if slots:
        out["NamedSlots"] = {slot: canonical_node(child, f"{path}/NamedSlots/{slot}", names, paths)
                             for slot, child in sorted(slots.items()) if child is not None}
        if not out["NamedSlots"]:
            del out["NamedSlots"]
    return out


def canonical_tree(value, path: str = "/Body/WidgetTree") -> tuple[dict, dict]:
    """Canonical WidgetTree region and the document path of every listed widget."""
    if type(value) is not dict:
        raise _error(path, "WidgetTree is {\"RootWidget\": widget|null, \"NamedSlotBindings\": {slot: widget}}.")
    for key in value:
        if key not in ("RootWidget", "NamedSlotBindings"):
            raise _error(f"{path}/{key}", f"Unknown WidgetTree key '{key}'. Keys: RootWidget, NamedSlotBindings.",
                         "Widget.UnknownField")
    names, paths, out = {}, {}, {}
    if "RootWidget" in value:
        root = value["RootWidget"]
        out["RootWidget"] = None if root is None else canonical_node(root, f"{path}/RootWidget", names, paths)
    if "NamedSlotBindings" in value:
        slots = value["NamedSlotBindings"]
        if type(slots) is not dict:
            raise _error(f"{path}/NamedSlotBindings", "NamedSlotBindings is an object of slot name -> widget.")
        out["NamedSlotBindings"] = {slot: canonical_node(child, f"{path}/NamedSlotBindings/{slot}", names, paths)
                                    for slot, child in sorted(slots.items()) if child is not None}
    sources = {}
    for node, *_ in walk(out):
        source = node.get("RenamedFrom")
        if source is None or source.casefold() == node["Name"].casefold():
            continue  # a case-only change needs no RenamedFrom
        here = paths[node["Name"]]
        if source.casefold() in names or source.casefold() in sources:
            raise _error(f"{here}/RenamedFrom", f"RenamedFrom '{source}' names a listed widget or another rename's "
                         "source; each widget is renamed from a name that is no longer listed, once.",
                         "Widget.InvalidRename")
        sources[source.casefold()] = here
    return out, paths


def canonical_bindings(value, path: str = "/Body/Bindings") -> list:
    if type(value) is not list:
        raise _error(path, "Bindings is an array of {Widget, Property, Kind, Function | SourcePath}.",
                     "Widget.InvalidBinding")
    out, seen = [], {}
    for index, row in enumerate(value):
        here = f"{path}/{index}"
        if type(row) is not dict:
            raise _error(here, "A binding is an object.", "Widget.InvalidBinding")
        unknown = set(row) - {"Widget", "Property", "Kind", "Function", "SourcePath", "SourceProperty", "PropertyPath",
                              "MemberGuid"}
        if unknown:
            raise _error(f"{here}/{sorted(unknown)[0]}", "Binding keys: Widget, Property, Kind, Function, SourcePath.",
                         "Widget.UnknownField")
        widget, prop = row.get("Widget"), row.get("Property")
        if type(widget) is not str or not widget or type(prop) is not str or not prop:
            raise _error(here, "A binding needs Widget and Property names.", "Widget.InvalidBinding")
        source = row.get("SourcePath", row.get("PropertyPath", row.get("SourceProperty")))
        if type(source) is str:
            source = [part for part in source.split(".") if part]
        kind = row.get("Kind") or ("Function" if "Function" in row else "Property")
        if kind not in ("Function", "Property"):
            raise _error(f"{here}/Kind", "Kind is Function or Property.", "Widget.InvalidBinding")
        if "MemberGuid" in row and type(row["MemberGuid"]) is not str:
            raise _error(f"{here}/MemberGuid", "MemberGuid is a GUID string (ignored; the editor keeps it).",
                         "Widget.InvalidBinding")
        item = {"Widget": widget, "Property": prop, "Kind": kind}
        if kind == "Function":
            if type(row.get("Function")) is not str or not row["Function"]:
                raise _error(f"{here}/Function", "A Function binding names a function of this Blueprint.",
                             "Widget.InvalidBinding")
            item["Function"] = row["Function"]
        else:
            if type(source) is not list or not source or any(type(part) is not str or not part for part in source):
                raise _error(f"{here}/SourcePath", "A Property binding has SourcePath, e.g. [\"Health\"].",
                             "Widget.InvalidBinding")
            item["SourcePath"] = list(source)
        key = (widget.casefold(), prop.casefold())
        if key in seen:
            raise _error(here, f"{widget}.{prop} is bound twice (also at index {seen[key]}).", "Widget.DuplicateBinding")
        seen[key] = index
        out.append(item)
    return sorted(out, key=lambda item: (item["Widget"], item["Property"]))


# ---------------------------------------------------------------------------
# Live query -> Body
# ---------------------------------------------------------------------------
def navigation_from_live(value: dict) -> dict:
    out = {}
    for key in DIRECTIONS:
        entry = value.get(key)
        if entry:
            out[key] = {"Rule": entry["rule"], **({"WidgetToFocus": entry["widget_to_focus"]}
                                                  if entry.get("widget_to_focus") else {})}
    if value.get("routing_policy"):
        out["RoutingPolicy"] = value["routing_policy"]
    return out


def tree_from_rows(rows: list) -> dict:
    """umg.tree.query.v1 widget rows (depth-first) -> canonical WidgetTree."""
    nodes, tree = {}, {"RootWidget": None, "NamedSlotBindings": {}}
    for row in rows:
        node = {"Name": row["name"], "Class": row["class"], "IsVariable": row["is_variable"]}
        if row.get("properties"):
            node["Properties"] = dict(row["properties"])
        if row.get("slot"):
            node["Slot"] = dict(row["slot"])
        navigation = navigation_from_live(row.get("navigation") or {})
        if navigation:
            node["Navigation"] = navigation
        nodes[row["name"]] = node
        parent, slot = row["parent"], row["slot_name"]
        if parent:
            if slot:
                nodes[parent].setdefault("NamedSlots", {})[slot] = node
            else:
                nodes[parent].setdefault("Children", []).append(node)
        elif slot:
            tree["NamedSlotBindings"][slot] = node
        else:
            tree["RootWidget"] = node
    for node in nodes.values():
        if "NamedSlots" in node:
            node["NamedSlots"] = dict(sorted(node["NamedSlots"].items()))
    tree["NamedSlotBindings"] = dict(sorted(tree["NamedSlotBindings"].items()))
    return tree


def bindings_from_live(rows: list) -> list:
    out = []
    for row in rows:
        item = {"Widget": row["widget"], "Property": row["property"], "Kind": row["kind"]}
        if row["kind"] == "Function":
            item["Function"] = row["function"]
        else:
            item["SourcePath"] = list(row.get("source_path", []))
        out.append(item)
    return sorted(out, key=lambda item: (item["Widget"], item["Property"]))


# ---------------------------------------------------------------------------
# Structure planning (simulated so an unchanged tree issues no calls)
# ---------------------------------------------------------------------------
def walk(tree: dict):
    """(node, parent name, slot name or None, panel index) in pre-order; root/inherited slots have parent ''."""
    def visit(node, parent, slot, index):
        yield node, parent, slot, index
        for name, child in node.get("NamedSlots", {}).items():
            yield from visit(child, node["Name"], name, 0)
        for position, child in enumerate(node.get("Children", [])):
            yield from visit(child, node["Name"], None, position)
    if tree.get("RootWidget"):
        yield from visit(tree["RootWidget"], "", "", 0)
    for slot, node in tree.get("NamedSlotBindings", {}).items():
        yield from visit(node, "", slot, 0)


def effective_tree(desired: dict, live: dict) -> dict:
    """Fill the omitted RootWidget / NamedSlotBindings sub-keys from the live tree."""
    return {"RootWidget": desired["RootWidget"] if "RootWidget" in desired else live["RootWidget"],
            "NamedSlotBindings": (desired["NamedSlotBindings"] if "NamedSlotBindings" in desired
                                  else live["NamedSlotBindings"])}


class _Model:
    """Where every widget sits, mirroring UPanelWidget / named slot semantics."""

    def __init__(self, rows):
        self.where = {}      # name -> (parent, slot) ; slot None = panel child
        self.panels = {}     # parent -> [names]
        self.slots = {}      # (parent, slot) -> name
        self.capacity = {row["name"]: row.get("capacity", "many") for row in rows}
        self.classes = {row["name"]: row["class"] for row in rows}
        for row in rows:
            self._attach(row["name"], row["parent"], row["slot_name"] if (row["slot_name"] or not row["parent"])
                         else None, None)

    def _attach(self, name, parent, slot, index):
        self.where[name] = (parent, slot)
        if slot is None:
            children = self.panels.setdefault(parent, [])
            children.insert(len(children) if index is None or index < 0 else min(index, len(children)), name)
        else:
            self.slots[(parent, slot)] = name

    def detach(self, name):
        location = self.where.pop(name, None)
        if location is None:
            return False
        parent, slot = location
        if slot is None:
            self.panels[parent].remove(name)
        else:
            del self.slots[(parent, slot)]
        return True

    def subtree(self, name):
        out = [name]
        for (parent, _), child in list(self.slots.items()):
            if parent == name:
                out += self.subtree(child)
        for child in list(self.panels.get(name, [])):
            out += self.subtree(child)
        return out

    def remove(self, name):
        for item in self.subtree(name):
            self.detach(item)
            self.classes.pop(item, None)

    def place(self, name, parent, slot, index):
        self.detach(name)
        self._attach(name, parent, slot, index)

    def index_of(self, name):
        parent, slot = self.where[name]
        return self.panels[parent].index(name) if slot is None else 0

    def occupant(self, parent, slot):
        if slot is not None:
            return self.slots.get((parent, slot))
        if self.capacity.get(parent) == "one" and self.panels.get(parent):
            return self.panels[parent][0]
        return None


def plan_renames(desired: dict, rows: list, paths: dict) -> list:
    """umg.widget.rename.v1 for a widget listed with RenamedFrom (or whose name differs from a live one only by case).

    The editor's rename keeps the widget (variable GUID, bindings, animation bindings, graph references); a new
    name without RenamedFrom is a remove + add instead. RenamedFrom is ignored once the rename happened (a live
    widget already has the name) or when no live widget has the old name. Runs before plan_structure, on the live
    rows.
    """
    by_fold = {row["name"].casefold(): row["name"] for row in rows}
    wanted = {node["Name"].casefold() for node, *_ in walk(desired)}
    ops = []
    for node, *_ in walk(desired):
        name = node["Name"]
        path = paths.get(name, "/Body/WidgetTree")
        current = by_fold.get(name.casefold())
        if current is not None:
            if current != name:
                ops.append(("umg.widget.rename.v1", {"name": current, "new_name": name}, f"{path}/Name"))
            continue
        source = node.get("RenamedFrom")
        old = by_fold.get(source.casefold()) if source is not None else None
        if old is not None and old.casefold() not in wanted:
            ops.append(("umg.widget.rename.v1", {"name": old, "new_name": name}, f"{path}/RenamedFrom"))
    return ops


def plan_structure(desired: dict, rows: list, paths: dict) -> list:
    """Ordered umg.tree.* calls that turn the live tree (rows) into the effective desired tree."""
    live_parent = {row["name"]: row["parent"] for row in rows}
    wanted = {node["Name"]: node for node, *_ in walk(desired)}
    model = _Model(rows)
    doomed = {name for name, cls in model.classes.items() if name not in wanted or wanted[name]["Class"] != cls}
    ops = []
    # Keep widgets whose parent goes away: detach them first (their own children come along).
    for row in rows:
        name = row["name"]
        if name not in doomed and live_parent[name] in doomed:
            ops.append(("umg.tree.move.v1", {"name": name, "detach": True}, paths.get(name, "/Body/WidgetTree")))
            model.detach(name)
    # Remove the topmost doomed widgets (descendants go with them).
    for row in rows:
        name = row["name"]
        if name in doomed and live_parent[name] not in doomed and name in model.classes:
            values = {"name": name}
            if name in wanted:
                values["keep_references"] = True  # replaced by a widget of another class under the same name
            ops.append(("umg.tree.remove.v1", values, paths.get(name, "/Body/WidgetTree")))
            model.remove(name)
    for node, parent, slot, index in walk(desired):
        name = node["Name"]
        path = paths.get(name, "/Body/WidgetTree")
        placement = {}
        if parent:
            placement["parent"] = parent
        if slot:
            placement["slot_name"] = slot
        if slot is None:
            placement["index"] = index
        occupant = model.occupant(parent, slot)
        if occupant is not None and occupant != name:
            ops.append(("umg.tree.move.v1", {"name": occupant, "detach": True}, path))
            model.detach(occupant)
        if name not in model.classes:
            values = {"name": name, "class": node["Class"], **placement}
            if "IsVariable" in node:
                values["is_variable"] = node["IsVariable"]
            ops.append(("umg.tree.add.v1", values, path))
            model.classes[name] = node["Class"]
            model.capacity[name] = "unknown"
            model.place(name, parent, slot, index)
            continue
        here = model.where.get(name)
        if here != (parent, slot) or (slot is None and model.index_of(name) != index):
            ops.append(("umg.tree.move.v1", {"name": name, **placement}, path))
            model.place(name, parent, slot, index)
    return ops


def _changed(desired: dict, live: dict) -> list:
    return [key for key in desired if key not in live or not members.values_equal(desired[key], live[key])]


def plan_values(desired: dict, rows: list, paths: dict) -> list:
    """Variable flag, widget / slot properties and navigation of every desired widget."""
    live = {row["name"]: row for row in rows}
    ops = []
    for node, *_ in walk(desired):
        name = node["Name"]
        row = live.get(name)
        path = paths.get(name, "/Body/WidgetTree")
        if row is None:
            continue
        if "IsVariable" in node and node["IsVariable"] != row["is_variable"]:
            ops.append(("umg.widget.variable.set.v1", {"name": name, "is_variable": node["IsVariable"]},
                        f"{path}/IsVariable"))
        for key, target, current in (("Properties", "widget", row.get("properties", {})),
                                     ("Slot", "slot", row.get("slot", {}))):
            wanted = node.get(key, {})
            for prop in _changed(wanted, current):
                ops.append(("umg.widget.property.set.v1",
                            {"name": name, "target": target, "property": prop, "value": wanted[prop]},
                            f"{path}/{key}/{prop}"))
            for prop in current:
                if prop not in wanted:
                    ops.append(("umg.widget.property.set.v1",
                                {"name": name, "target": target, "property": prop, "reset": True}, f"{path}/{key}"))
        navigation = node.get("Navigation", {})
        if navigation != navigation_from_live(row.get("navigation") or {}):
            wire = {key: {"rule": entry["Rule"], **({"widget_to_focus": entry["WidgetToFocus"]}
                                                     if "WidgetToFocus" in entry else {})}
                    for key, entry in navigation.items() if key in DIRECTIONS}
            if "RoutingPolicy" in navigation:
                wire["routing_policy"] = navigation["RoutingPolicy"]
            ops.append(("umg.widget.navigation.set.v1", {"name": name, "navigation": wire}, f"{path}/Navigation"))
    return ops


def plan_bindings(desired: list, live_rows: list) -> list:
    live = {(row["Widget"].casefold(), row["Property"].casefold()): row for row in bindings_from_live(live_rows)}
    wanted = {(row["Widget"].casefold(), row["Property"].casefold()) for row in desired}
    ops = [("umg.binding.remove.v1", {"widget": row["Widget"], "property": row["Property"]}, "/Body/Bindings")
           for key, row in sorted(live.items()) if key not in wanted]
    for index, row in enumerate(desired):
        if live.get((row["Widget"].casefold(), row["Property"].casefold())) == row:
            continue
        values = {"widget": row["Widget"], "property": row["Property"], "kind": row["Kind"]}
        if row["Kind"] == "Function":
            values["function"] = row["Function"]
        else:
            values["source_path"] = row["SourcePath"]
        ops.append(("umg.binding.set.v1", values, f"/Body/Bindings/{index}"))
    return ops


# ---------------------------------------------------------------------------
# Diff
# ---------------------------------------------------------------------------
def _node_mismatch(want: dict, have: dict, strict: bool) -> bool:
    if want["Class"] != have["Class"]:
        return True
    if "IsVariable" in want and want["IsVariable"] != have.get("IsVariable"):
        return True
    for key in ("Properties", "Slot"):
        if members._props_differ(want.get(key, {}), have.get(key, {}), strict):
            return True
    return want.get("Navigation", {}) != have.get("Navigation", {})


def _placements(tree: dict) -> dict:
    return {node["Name"]: (parent, slot, index) for node, parent, slot, index in walk(tree)}


def tree_mismatches(desired: dict, live: dict, strict: bool = False) -> list:
    """Names of widgets whose class, placement or values differ (and widgets that should not exist)."""
    effective = effective_tree(desired, live)
    want, have = _placements(effective), _placements(live)
    nodes_want = {node["Name"]: node for node, *_ in walk(effective)}
    nodes_have = {node["Name"]: node for node, *_ in walk(live)}
    out = sorted(name for name in have if name not in want)
    for name, placement in want.items():
        if name not in have or have[name] != placement or _node_mismatch(nodes_want[name], nodes_have[name], strict):
            out.append(name)
    return out


def _escape(segment: str) -> str:
    return str(segment).replace("~", "~0").replace("/", "~1")


def _pointers(tree: dict, base: str = "/Body/WidgetTree") -> dict:
    """Widget name -> JSON pointer of its node in ``tree``."""
    out = {}

    def visit(node, pointer):
        out[node["Name"]] = pointer
        for name, child in node.get("NamedSlots", {}).items():
            visit(child, f"{pointer}/NamedSlots/{_escape(name)}")
        for position, child in enumerate(node.get("Children", [])):
            visit(child, f"{pointer}/Children/{position}")
    if tree.get("RootWidget"):
        visit(tree["RootWidget"], f"{base}/RootWidget")
    for slot, node in tree.get("NamedSlotBindings", {}).items():
        visit(node, f"{base}/NamedSlotBindings/{_escape(slot)}")
    return out


def _shallow(node: dict) -> dict:
    return {key: value for key, value in node.items() if key not in ("Children", "NamedSlots")}


def _placement(placement: tuple) -> dict:
    parent, slot, index = placement
    return {"Parent": parent, "NamedSlot": slot} if slot is not None else {"Parent": parent, "Index": index}


def _value_differences(path: str, want, have) -> list:
    """The smallest authored paths at which ``want`` differs from ``have`` (authored keys only)."""
    if type(want) is dict and type(have) is dict:
        out = []
        for key, value in want.items():
            child = f"{path}/{_escape(key)}"
            if key not in have:
                out.append(Difference(child, DiffKind.REMOVE, True, value, False, None))
            elif not members.values_equal(value, have[key]):
                out.extend(_value_differences(child, value, have[key]))
        if out:
            return out
    return [Difference(path, DiffKind.REPLACE, True, want, True, have)]


def _property_differences(path: str, want: dict, have: dict, strict: bool) -> list:
    """Per-property form of ``ublueprint_members._props_differ`` (same verdict)."""
    out = []
    for key, value in want.items():
        child = f"{path}/{_escape(key)}"
        if key in have:
            if not members.values_equal(value, have[key]):
                out.extend(_value_differences(child, value, have[key]))
        elif strict:
            out.append(Difference(child, DiffKind.REMOVE, True, value, False, None))
    for key in have:
        if key not in want:
            out.append(Difference(f"{path}/{_escape(key)}", DiffKind.ADD, False, None, True, have[key]))
    return out


def _widget_differences(name: str, effective: dict, live: dict, strict: bool) -> list:
    """Per-widget Differences for one name ``tree_mismatches`` reports; never empty."""
    want_at, have_at = _pointers(effective), _pointers(live)
    nodes_want = {node["Name"]: node for node, *_ in walk(effective)}
    nodes_have = {node["Name"]: node for node, *_ in walk(live)}
    if name not in nodes_want:
        return [Difference(have_at[name], DiffKind.ADD, False, None, True,
                           {"Name": name, "Class": nodes_have[name]["Class"]})]
    if name not in nodes_have:
        return [Difference(want_at[name], DiffKind.REMOVE, True, _shallow(nodes_want[name]), False, None)]
    want, have, pointer = nodes_want[name], nodes_have[name], want_at[name]
    out = []
    want_place, have_place = _placements(effective)[name], _placements(live)[name]
    if want_place != have_place:
        out.append(Difference(pointer, DiffKind.REPLACE, True, _placement(want_place), True, _placement(have_place)))
    if want["Class"] != have["Class"]:
        out.append(Difference(f"{pointer}/Class", DiffKind.REPLACE, True, want["Class"], True, have["Class"]))
    if "IsVariable" in want and want["IsVariable"] != have.get("IsVariable"):
        out.append(Difference(f"{pointer}/IsVariable", DiffKind.REPLACE, True, want["IsVariable"],
                              "IsVariable" in have, have.get("IsVariable")))
    for key in ("Properties", "Slot"):
        out.extend(_property_differences(f"{pointer}/{key}", want.get(key, {}), have.get(key, {}), strict))
    if want.get("Navigation", {}) != have.get("Navigation", {}):
        out.append(Difference(f"{pointer}/Navigation", DiffKind.REPLACE, True, want.get("Navigation", {}), True,
                              have.get("Navigation", {})))
    return out or [Difference(pointer, DiffKind.REPLACE, True, _shallow(want), True, _shallow(have))]


def diff_designer(authored: dict, extracted: dict, strict: bool = False) -> list:
    out = []
    if "WidgetTree" in authored:
        names = tree_mismatches(authored["WidgetTree"], extracted["WidgetTree"], strict)
        effective = effective_tree(authored["WidgetTree"], extracted["WidgetTree"])
        for name in names:
            out.extend(_widget_differences(name, effective, extracted["WidgetTree"], strict))
    if "Bindings" in authored and authored["Bindings"] != extracted.get("Bindings", []):
        out.append(Difference("/Body/Bindings", DiffKind.REPLACE, True, authored["Bindings"], True,
                              extracted.get("Bindings", [])))
    return out
