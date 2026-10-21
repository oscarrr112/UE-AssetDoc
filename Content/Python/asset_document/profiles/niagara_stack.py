"""Module stack planning of profile.niagara_system.v1 (N3): module lists, Set Parameters modules, inputs, event
handlers, simulation stages and lightweight emitters, turned into ``fx.niagara.edit.v1`` stack ops.

Semantics (MCP/schemas/assetdocument/Niagara.md):

* A module list present in the document is complete and ordered.  A module's identity is its ``Name`` (the
  function call name, unique in its graph: the system graph, or the emitter graph holding all of an emitter's
  stacks); a name listed in another stack of the same graph is a move.  A new module is added under its ``Name``
  (the script name, or the script name and digits).  A different ``Script`` (or a Set Parameters entry in place of
  a module) replaces the module.  ``Enabled`` omitted = enabled, ``Version`` omitted = the exposed version,
  ``DisplayName`` / ``Note`` omitted = none, ``Inputs`` complete (omitted = reset to default).
* Set Parameters modules (``Assign``) get editor-made names (``SetVariables_<guid>``): an entry whose ``Name`` the
  graph does not have pairs with the stack's next unpaired Set Parameters module, in order.
* ``EventHandlers`` match by position (deleted from the end, added at the end), ``SimulationStages`` by ``Name``
  (a different ``Class`` replaces the stage).
* Scratch pad modules and dynamic inputs (``Script: "ScratchPad:<name>"``: the system's scratch pad of that name,
  else the emitter's) are added, moved, deleted and set like any other module (they have no ``Version``).

Each pass plans against a fresh read: new event handlers / stages / emitters get their modules, and new modules
their settings and inputs, in the next pass.
"""
from __future__ import annotations

from asset_document.diff.model import DiffKind
from asset_document.diff.semantic_compare import as_authored
from asset_document.profiles import data_table_common as common

PREFIX = "Niagara"
SCRATCH = "ScratchPad:"
SYSTEM_GROUPS = ("SystemSpawn", "SystemUpdate")
EMITTER_GROUPS = ("EmitterSpawn", "EmitterUpdate", "ParticleSpawn", "ParticleUpdate")
MODULE_KEYS = ("Name", "Script", "Assign", "Version", "Enabled", "DisplayName", "Note", "Inputs")
HANDLER_KEYS = ("Properties", "Modules")
STAGE_KEYS = ("Name", "Class", "Enabled", "Properties", "Modules")
STATELESS_KEYS = ("Properties", "Modules", "Renderers")
STATELESS_MODULE_KEYS = ("Class", "Properties")
_MISSING = object()


def _escape(name):
    return common.escape(str(name))


def _fail(path, message, code):
    raise common.error(path, message, f"{PREFIX}.{code}")


def _same(left, right):
    return common.same(left, right, exact=False)


def object_path(path):
    """/Game/FX/X -> /Game/FX/X.X (an asset's object path); scratch pad references unchanged."""
    if type(path) is not str or path.startswith(SCRATCH) or not path.startswith("/"):
        return path
    leaf = path.rsplit("/", 1)[-1]
    return path if "." in leaf else f"{path}.{leaf}"


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------
def _object(value, path, keys, what):
    if type(value) is not dict:
        _fail(path, f"A {what} is an object.", "InvalidBody")
    for key in value:
        if key not in keys:
            _fail(f"{path}/{_escape(key)}", f"Unknown {what} key '{key}'. Keys: {', '.join(keys)}.", "UnknownKey")
    return value


def _list(value, path, what):
    if type(value) is not list:
        _fail(path, f"{what} is a list.", "InvalidBody")
    return value


def _check_value(value, path):
    if type(value) is not dict or len(value) != 1:
        return
    (tag, inner), = value.items()
    if tag == "Dynamic":
        _object(inner, f"{path}/Dynamic", ("Script", "Version", "Inputs"), "dynamic input")
        if type(inner.get("Script")) is not str or not inner["Script"]:
            _fail(f"{path}/Dynamic/Script", "A dynamic input names its Script.", "InvalidBody")
        inputs = inner.get("Inputs", {})
        if type(inputs) is not dict:
            _fail(f"{path}/Dynamic/Inputs", "Inputs is an object.", "InvalidBody")
        for name, nested in inputs.items():
            _check_value(nested, f"{path}/Dynamic/Inputs/{_escape(name)}")
    elif tag == "Link" and type(inner) is not str:
        _fail(f"{path}/Link", "A link names a parameter.", "InvalidBody")


def check_module(entry, path):
    _object(entry, path, MODULE_KEYS, "module")
    if type(entry.get("Name")) is not str or not entry["Name"]:
        _fail(f"{path}/Name", "A module has a Name (its function name).", "InvalidBody")
    if ("Script" in entry) == ("Assign" in entry):
        _fail(path, "A module has a Script, or (Set Parameters) an Assign list.", "InvalidBody")
    if "Script" in entry and (type(entry["Script"]) is not str or not entry["Script"]):
        _fail(f"{path}/Script", "Script is a module script path.", "InvalidBody")
    if "Assign" in entry:
        for index, target in enumerate(_list(entry["Assign"], f"{path}/Assign", "Assign")):
            _object(target, f"{path}/Assign/{index}", ("Target", "Type"), "Set Parameters target")
            for key in ("Target", "Type"):
                if type(target.get(key)) is not str or not target[key]:
                    _fail(f"{path}/Assign/{index}/{key}", f"A target has a {key}.", "InvalidBody")
    for key, kind in (("Enabled", bool), ("Version", str), ("DisplayName", str)):
        if key in entry and type(entry[key]) is not kind:
            _fail(f"{path}/{key}", f"{key} is a {'boolean' if kind is bool else 'string'}.", "InvalidBody")
    if "Note" in entry:
        note = _object(entry["Note"], f"{path}/Note", ("Header", "Body"), "note")
        for key, value in note.items():
            if type(value) is not str:
                _fail(f"{path}/Note/{key}", f"{key} is a string.", "InvalidBody")
    if "Inputs" in entry:
        if type(entry["Inputs"]) is not dict:
            _fail(f"{path}/Inputs", "Inputs is an object.", "InvalidBody")
        for name, value in entry["Inputs"].items():
            _check_value(value, f"{path}/Inputs/{_escape(name)}")


def _check_modules(modules, path, names, graph_path):
    for index, entry in enumerate(_list(modules, path, "A module list")):
        where = f"{path}/{index}"
        check_module(entry, where)
        if entry["Name"] in names:
            _fail(f"{where}/Name", f"Two modules of {graph_path} are named '{entry['Name']}' (names are unique in a graph).",
                  "DuplicateName")
        names.add(entry["Name"])


def check_system_stacks(body):
    names = set()
    for group in SYSTEM_GROUPS:
        if group in body:
            _check_modules(body[group], f"/Body/{group}", names, "the system")
    if "Parameters" in body:
        seen = set()
        for index, entry in enumerate(_list(body["Parameters"], "/Body/Parameters", "Parameters")):
            where = f"/Body/Parameters/{index}"
            _object(entry, where, ("Name", "Type", "Description"), "declared parameter")
            for key in ("Name", "Type"):
                if type(entry.get(key)) is not str or not entry[key]:
                    _fail(f"{where}/{key}", f"A declared parameter has a {key}.", "InvalidBody")
            if "Description" in entry and type(entry["Description"]) is not str:
                _fail(f"{where}/Description", "Description is a string.", "InvalidBody")
            if entry["Name"] in seen:
                _fail(f"{where}/Name", f"Two declared parameters are named '{entry['Name']}'.", "DuplicateName")
            seen.add(entry["Name"])


def _value_links(value, path):
    if type(value) is not dict or len(value) != 1:
        return
    (tag, inner), = value.items()
    if tag == "Link" and type(inner) is str:
        yield f"{path}/Link", inner
    elif tag == "Dynamic" and type(inner) is dict and type(inner.get("Inputs")) is dict:
        for name, nested in inner["Inputs"].items():
            yield from _value_links(nested, f"{path}/Dynamic/Inputs/{_escape(name)}")


def _module_links(modules, path):
    for index, module in enumerate(modules if type(modules) is list else []):
        inputs = module.get("Inputs") if type(module) is dict else None
        for name, value in (inputs.items() if type(inputs) is dict else ()):
            yield from _value_links(value, f"{path}/{index}/Inputs/{_escape(name)}")


def links(body):
    """(document path, parameter) of every linked input of a system Body (nested dynamic inputs included)."""
    for group in SYSTEM_GROUPS:
        yield from _module_links(body.get(group), f"/Body/{group}")
    for index, entry in enumerate(body.get("Emitters", [])):
        base = f"/Body/Emitters/{index}"
        for group in EMITTER_GROUPS:
            yield from _module_links(entry.get(group), f"{base}/{group}")
        for key in ("EventHandlers", "SimulationStages"):
            for position, item in enumerate(entry.get(key, []) if type(entry.get(key)) is list else []):
                if type(item) is dict:
                    yield from _module_links(item.get("Modules"), f"{base}/{key}/{position}/Modules")


def check_emitter_stacks(entry, path):
    names = set()
    for group in EMITTER_GROUPS:
        if group in entry:
            _check_modules(entry[group], f"{path}/{group}", names, f"the emitter '{entry.get('Name')}'")
    if "EventHandlers" in entry:
        for index, handler in enumerate(_list(entry["EventHandlers"], f"{path}/EventHandlers", "EventHandlers")):
            where = f"{path}/EventHandlers/{index}"
            _object(handler, where, HANDLER_KEYS, "event handler")
            if "Properties" in handler and type(handler["Properties"]) is not dict:
                _fail(f"{where}/Properties", "Properties is an object.", "InvalidBody")
            if "Modules" in handler:
                _check_modules(handler["Modules"], f"{where}/Modules", names, f"the emitter '{entry.get('Name')}'")
    if "SimulationStages" in entry:
        stages = set()
        for index, stage in enumerate(_list(entry["SimulationStages"], f"{path}/SimulationStages", "SimulationStages")):
            where = f"{path}/SimulationStages/{index}"
            _object(stage, where, STAGE_KEYS, "simulation stage")
            if type(stage.get("Name")) is not str or not stage["Name"]:
                _fail(f"{where}/Name", "A simulation stage has a Name.", "InvalidBody")
            if stage["Name"] in stages:
                _fail(f"{where}/Name", f"Two simulation stages are named '{stage['Name']}'.", "DuplicateName")
            stages.add(stage["Name"])
            if "Class" in stage and type(stage["Class"]) is not str:
                _fail(f"{where}/Class", "Class is a simulation stage class.", "InvalidBody")
            if "Enabled" in stage and type(stage["Enabled"]) is not bool:
                _fail(f"{where}/Enabled", "Enabled is true or false.", "InvalidBody")
            if "Properties" in stage and type(stage["Properties"]) is not dict:
                _fail(f"{where}/Properties", "Properties is an object.", "InvalidBody")
            if "Modules" in stage:
                _check_modules(stage["Modules"], f"{where}/Modules", names, f"the emitter '{entry.get('Name')}'")
    if "Stateless" in entry:
        stateless = _object(entry["Stateless"], f"{path}/Stateless", STATELESS_KEYS, "Stateless value")
        if "Properties" in stateless and type(stateless["Properties"]) is not dict:
            _fail(f"{path}/Stateless/Properties", "Properties is an object.", "InvalidBody")
        classes = set()
        for index, module in enumerate(_list(stateless.get("Modules", []), f"{path}/Stateless/Modules", "Modules")):
            where = f"{path}/Stateless/Modules/{index}"
            _object(module, where, STATELESS_MODULE_KEYS, "lightweight module")
            if type(module.get("Class")) is not str or not module["Class"]:
                _fail(f"{where}/Class", "A lightweight module has a Class.", "InvalidBody")
            if module["Class"] in classes:
                _fail(f"{where}/Class", f"Two lightweight modules are '{module['Class']}'.", "DuplicateName")
            classes.add(module["Class"])
            if "Properties" in module and type(module["Properties"]) is not dict:
                _fail(f"{where}/Properties", "Properties is an object.", "InvalidBody")


# ---------------------------------------------------------------------------
# Values
# ---------------------------------------------------------------------------
def normalize_value(value):
    """An input value with its script references as object paths (as the extract writes them)."""
    if type(value) is dict and len(value) == 1 and type(value.get("Dynamic")) is dict:
        dynamic = dict(value["Dynamic"])
        dynamic["Script"] = object_path(dynamic.get("Script"))
        dynamic["Inputs"] = {name: normalize_value(item) for name, item in dynamic.get("Inputs", {}).items()}
        return {"Dynamic": dynamic}
    return value


def enum_short_name(name):
    """An enum value's name without a trailing parenthesized explanation: "Kill (Emitter and Particles Die
    Immediately)" -> "Kill" (the editor accepts it when exactly one value of the enum has it)."""
    open_at = name.find(" (")
    return name[:open_at].rstrip() if open_at > 0 and name.endswith(")") else name


def _same_enum_name(want, have):
    """An enum input written by its short name reads back by its full name (names compare without case)."""
    if type(want) is not str or type(have) is not str or "(" in want:
        return False
    short = enum_short_name(have)
    return short != have and short.lower() == want.lower()


def _same_input(want, have):
    if have is _MISSING:
        return False
    if type(want) is dict and type(have) is dict and "DataInterface" in want and "DataInterface" in have \
            and type(want["DataInterface"]) is dict and type(have["DataInterface"]) is dict:
        spec = dict(want["DataInterface"])
        current = have["DataInterface"]
        if "Class" not in spec:
            spec["Class"] = current.get("Class")
        want_properties, have_properties = spec.get("Properties", {}), current.get("Properties", {})
        if type(want_properties) is dict and type(have_properties) is dict:
            # A struct value (a curve) and the elements of an array of structs (its Keys) list the members they
            # set; the others hold their defaults (the read-back lists them all).
            have_properties = {name: as_authored(want_properties[name], value) if name in want_properties else value
                               for name, value in have_properties.items()}
        return _same(spec.get("Class"), current.get("Class")) and _same(want_properties, have_properties)
    if type(want) is dict and type(have) is dict and "Dynamic" in want and "Dynamic" in have:
        left, right = want["Dynamic"], have["Dynamic"]
        if left.get("Script") != right.get("Script") or left.get("Version") != right.get("Version"):
            return False
        inputs_left, inputs_right = left.get("Inputs", {}), right.get("Inputs", {})
        return set(inputs_left) == set(inputs_right) and all(
            _same_input(inputs_left[name], inputs_right[name]) for name in inputs_left)
    return _same(want, have) or _same_enum_name(want, have)


def _module_signature(entry):
    """What makes a module the same module: its script (or Set Parameters)."""
    return "Assign" if "Assign" in entry else object_path(entry.get("Script"))


# ---------------------------------------------------------------------------
# Module stacks of one graph
# ---------------------------------------------------------------------------
class Group:
    """One stack group of a graph: its op location, its document path and its authored / live module lists."""

    def __init__(self, location, path, authored, live):
        self.location = location
        self.path = path
        self.authored = authored
        self.live = live
        self.claims = [None] * len(authored)


def _with(location, **fields):
    op = dict(fields)
    op.update(location)
    return op


def plan_graph(plan, groups, other_live):
    """Plan the module ops of one graph. groups: the authored groups (Group, present in the document and existing in
    the system); other_live: {name: (group location, live entry)} of the modules in the graph's groups the document
    leaves out (kept, but movable into an authored group)."""
    live_by_name = {}
    for group in groups:
        for entry in group.live:
            live_by_name[entry["Name"]] = (group, entry)
    claimed = set()
    # 1. Claims by Name (same kind of module), then Set Parameters entries pair with unclaimed Set Parameters
    #    modules of their group, in order.
    for group in groups:
        for index, entry in enumerate(group.authored):
            source = live_by_name.get(entry["Name"])
            located = source[1] if source is not None else (other_live.get(entry["Name"], (None, None))[1])
            if located is not None and _module_signature(located) == _module_signature(entry) and entry["Name"] not in claimed:
                group.claims[index] = entry["Name"]
                claimed.add(entry["Name"])
    for group in groups:
        free = [entry["Name"] for entry in group.live if "Assign" in entry and entry["Name"] not in claimed
                and entry["Name"] not in {item["Name"] for item in _all_authored(groups)}]
        for index, entry in enumerate(group.authored):
            if group.claims[index] is None and "Assign" in entry and free:
                group.claims[index] = free.pop(0)
                claimed.add(group.claims[index])
    # 2. Deletes (live modules of authored groups that no entry claims), freeing names.
    for group in groups:
        for entry in group.live:
            if entry["Name"] not in claimed:
                plan.change(group.path, None, entry, _with(group.location, op="delete_module", name=entry["Name"]),
                            DiffKind.REMOVE, False, True)
    # 3. Moves: the claimed modules of each group in document order (each placed after its claimed predecessor).
    where = {}
    lists = {}
    for group in groups:
        lists[id(group)] = [entry["Name"] for entry in group.live if entry["Name"] in claimed]
        for name in lists[id(group)]:
            where[name] = id(group)
    for name, (_, _) in other_live.items():
        if name in claimed:
            where[name] = None
    for group in groups:
        kept = [(index, name) for index, name in enumerate(group.claims) if name is not None]
        previous = None
        for index, name in kept:
            current = lists[id(group)]
            predecessor = _predecessor(current, name, set(group.claims))
            if where.get(name) != id(group) or predecessor != previous:
                entry = group.authored[index]
                plan.change(f"{group.path}/{index}", entry, name,
                            _with(group.location, op="move_module", name=name, after=previous), DiffKind.REPLACE)
                old = where.get(name)
                if old is not None:
                    lists[old].remove(name)
                position = 0 if previous is None else lists[id(group)].index(previous) + 1
                lists[id(group)].insert(position, name)
                where[name] = id(group)
            previous = name
    # 4. Adds, last entry first, each right after the nearest kept module before it.
    for group in groups:
        for index in reversed(range(len(group.authored))):
            if group.claims[index] is not None:
                continue
            entry = group.authored[index]
            anchor = next((group.claims[at] for at in reversed(range(index)) if group.claims[at] is not None), None)
            path = f"{group.path}/{index}"
            if "Assign" in entry:
                op = _with(group.location, op="add_assignment", after=anchor,
                           targets=[{"name": target["Target"], "type": target["Type"]} for target in entry["Assign"]])
            else:
                op = _with(group.location, op="add_module", name=entry["Name"], script=object_path(entry["Script"]),
                           after=anchor)
                if "Version" in entry:
                    op["version"] = entry["Version"]
            plan.change(path, entry, None, op, DiffKind.ADD, True, False)
    # 5. Settings and inputs of the kept modules.
    for group in groups:
        for index, name in enumerate(group.claims):
            if name is None:
                continue
            current = live_by_name.get(name, other_live.get(name))[1]
            _plan_module(plan, group.location, f"{group.path}/{index}", group.authored[index], current, name)


def _all_authored(groups):
    for group in groups:
        yield from group.authored


def _predecessor(names, name, relevant):
    """The module before name in names, among the relevant ones (None: first)."""
    if name not in names:
        return _MISSING
    previous = None
    for item in names:
        if item == name:
            return previous
        if item in relevant:
            previous = item
    return previous


def _plan_module(plan, location, path, entry, current, name):
    settings = {}
    want_version = entry.get("Version", "")
    if "Assign" not in entry and want_version != current.get("Version", ""):
        settings["version"] = want_version
        plan.change(f"{path}/Version", entry.get("Version"), current.get("Version"),
                    authored_present="Version" in entry, live_present="Version" in current)
    want_enabled = entry.get("Enabled", True)
    if want_enabled != current.get("Enabled", True):
        settings["enabled"] = want_enabled
        plan.change(f"{path}/Enabled", want_enabled, current.get("Enabled", True))
    if entry.get("DisplayName", "") != current.get("DisplayName", ""):
        settings["display_name"] = entry.get("DisplayName", "")
        plan.change(f"{path}/DisplayName", entry.get("DisplayName"), current.get("DisplayName"),
                    authored_present="DisplayName" in entry, live_present="DisplayName" in current)
    if not _same(entry.get("Note"), current.get("Note")):
        note = entry.get("Note")
        settings["note"] = None if note is None else {"Header": note.get("Header", ""), "Body": note.get("Body", "")}
        plan.change(f"{path}/Note", note, current.get("Note"), authored_present="Note" in entry,
                    live_present="Note" in current)
    if settings:
        plan.ops.append((_with(location, op="module", name=name, **settings), path))
    if "Assign" in entry:
        want = [(target["Target"], target["Type"]) for target in entry["Assign"]]
        have = [(target["Target"], target["Type"]) for target in current.get("Assign", [])]
        if want != have:
            keep = 0
            while keep < min(len(want), len(have)) and want[keep] == have[keep]:
                keep += 1
            op = _with(location, op="assignment", name=name)
            if have[keep:]:
                op["remove"] = [target for target, _ in have[keep:]]
            if want[keep:]:
                op["add"] = [{"name": target, "type": kind} for target, kind in want[keep:]]
            plan.change(f"{path}/Assign", entry["Assign"], current.get("Assign", []), op)
            # The changed targets' inputs are planned against the new targets in the next pass.
            return
    if "version" in settings:
        # A version change converts the inputs: they are planned against the new version in the next pass.
        return
    _plan_inputs(plan, location, path, name, entry.get("Inputs", {}), current.get("Inputs", {}))


def _plan_inputs(plan, location, path, name, authored, live):
    write = {}
    for input_name, value in authored.items():
        want = normalize_value(value)
        have = live.get(input_name, _MISSING)
        if _same_input(want, have):
            continue
        where = f"{path}/Inputs/{_escape(input_name)}"
        if type(want) is dict and len(want) == 1 and "Unsupported" in want:
            plan.refuse(where, "UnsupportedInputValue", f"'{input_name}' holds a value the document cannot write "
                        f"({want['Unsupported']}); keep it as extracted or give it a value.", want,
                        None if have is _MISSING else have)
            continue
        write[input_name] = want
        plan.change(where, value, None if have is _MISSING else have, live_present=have is not _MISSING)
    reset = sorted(input_name for input_name in live if input_name not in authored)
    for input_name in reset:
        plan.change(f"{path}/Inputs/{_escape(input_name)}", None, live[input_name], kind=DiffKind.REMOVE,
                    authored_present=False)
    if write or reset:
        op = _with(location, op="inputs", module=name)
        if write:
            op["set"] = write
        if reset:
            op["reset"] = reset
        plan.ops.append((op, f"{path}/Inputs"))


# ---------------------------------------------------------------------------
# System and emitter stacks
# ---------------------------------------------------------------------------
def plan_system_stacks(plan, body, live_body):
    groups = []
    other = {}
    for group in SYSTEM_GROUPS:
        live = live_body.get(group, [])
        if group in body:
            groups.append(Group({"group": group}, f"/Body/{group}", body[group], live))
        else:
            for entry in live:
                other[entry["Name"]] = ({"group": group}, entry)
    if groups:
        plan_graph(plan, groups, other)


def plan_declared_parameters(plan, body, live_body):
    if "Parameters" not in body:
        return
    base = "/Body/Parameters"
    live = {entry["Name"]: entry for entry in live_body.get("Parameters", [])}
    authored = {entry["Name"]: entry for entry in body["Parameters"]}
    for name, entry in live.items():
        if name not in authored or authored[name]["Type"] != entry["Type"]:
            plan.change(base, None, entry, {"op": "delete_parameter", "name": name}, DiffKind.REMOVE, False, True)
    for index, entry in enumerate(body["Parameters"]):
        current = live.get(entry["Name"])
        path = f"{base}/{index}"
        if current is None or current["Type"] != entry["Type"]:
            op = {"op": "add_parameter", "name": entry["Name"], "type": entry["Type"]}
            if entry.get("Description"):
                op["description"] = entry["Description"]
            plan.change(path, entry, current, op, DiffKind.ADD, True, current is not None)
        elif entry.get("Description", "") != current.get("Description", ""):
            plan.change(f"{path}/Description", entry.get("Description"), current.get("Description"),
                        {"op": "parameter", "name": entry["Name"], "description": entry.get("Description", "")})


def plan_emitter_stacks(plan, path, name, entry, current, defaults, property_changes):
    """Event handlers, simulation stages and module lists of one existing standard emitter."""
    location = {"emitter": name}
    groups = []
    other = {}
    for group in EMITTER_GROUPS:
        live = current.get(group, [])
        if group in entry:
            groups.append(Group(dict(location, group=group), f"{path}/{group}", entry[group], live))
        else:
            for module in live:
                other[module["Name"]] = (dict(location, group=group), module)
    # Event handlers: by position.
    live_handlers = current.get("EventHandlers", [])
    if "EventHandlers" in entry:
        authored = entry["EventHandlers"]
        base = f"{path}/EventHandlers"
        for index in reversed(range(len(authored), len(live_handlers))):
            plan.change(base, None, live_handlers[index], {"op": "delete_event_handler", "emitter": name, "event": index},
                        DiffKind.REMOVE, False, True)
        for index in range(len(live_handlers), len(authored)):
            plan.change(f"{base}/{index}", authored[index], None, {"op": "add_event_handler", "emitter": name},
                        DiffKind.ADD, True, False)
        for index in range(min(len(authored), len(live_handlers))):
            handler = authored[index]
            have = live_handlers[index]
            where = f"{base}/{index}"
            if "Properties" in handler:
                write, reset = property_changes(handler["Properties"], have.get("Properties", {}),
                                                defaults.get("event_handler", {}))
                if write or reset:
                    op = {"op": "event_handler", "emitter": name, "event": index, "properties": write}
                    if reset:
                        op["reset"] = reset
                    plan.change(f"{where}/Properties", handler["Properties"], have.get("Properties", {}), op)
            location_event = dict(location, group="EventHandler", event=index)
            if "Modules" in handler:
                groups.append(Group(location_event, f"{where}/Modules", handler["Modules"], have.get("Modules", [])))
            else:
                for module in have.get("Modules", []):
                    other[module["Name"]] = (location_event, module)
    else:
        for index, have in enumerate(live_handlers):
            for module in have.get("Modules", []):
                other[module["Name"]] = (dict(location, group="EventHandler", event=index), module)
    # Simulation stages: by Name.
    live_stages = current.get("SimulationStages", [])
    if "SimulationStages" in entry:
        _plan_stages(plan, f"{path}/SimulationStages", name, entry["SimulationStages"], live_stages, defaults,
                     property_changes, groups, other, location)
    else:
        for stage in live_stages:
            for module in stage.get("Modules", []):
                other[module["Name"]] = (dict(location, group="SimulationStage", stage=stage["Name"]), module)
    if groups:
        plan_graph(plan, groups, other)


def _stage_class(entry, live):
    return entry.get("Class", live.get("Class") if live else None)


def _plan_stages(plan, base, emitter, authored, live_list, defaults, property_changes, groups, other, location):
    live = {stage["Name"]: stage for stage in live_list}
    kept = {}
    for stage in authored:
        current = live.get(stage["Name"])
        if current is not None and _stage_class(stage, current) == current["Class"]:
            kept[stage["Name"]] = current
    for stage in live_list:
        if stage["Name"] not in kept:
            plan.change(base, None, stage, {"op": "delete_stage", "emitter": emitter, "stage": stage["Name"]},
                        DiffKind.REMOVE, False, True)
    order = [stage["Name"] for stage in live_list if stage["Name"] in kept]
    previous = None
    for index, stage in enumerate(authored):
        if stage["Name"] not in kept:
            continue
        position = order.index(stage["Name"])
        if (order[position - 1] if position > 0 else None) != previous:
            plan.change(f"{base}/{index}", stage["Name"], stage["Name"],
                        {"op": "move_stage", "emitter": emitter, "stage": stage["Name"], "after": previous}, DiffKind.REPLACE)
            order.remove(stage["Name"])
            order.insert(0 if previous is None else order.index(previous) + 1, stage["Name"])
        previous = stage["Name"]
    for index in reversed(range(len(authored))):
        stage = authored[index]
        if stage["Name"] in kept:
            continue
        if "Class" not in stage:
            _fail(f"{base}/{index}/Class", "A new simulation stage has a Class.", "InvalidBody")
        anchor = next((authored[at]["Name"] for at in reversed(range(index)) if authored[at]["Name"] in kept), None)
        plan.change(f"{base}/{index}", stage, live.get(stage["Name"]),
                    {"op": "add_stage", "emitter": emitter, "name": stage["Name"], "class": stage["Class"], "after": anchor},
                    DiffKind.ADD, True, stage["Name"] in live)
    for index, stage in enumerate(authored):
        current = kept.get(stage["Name"])
        if current is None:
            continue
        where = f"{base}/{index}"
        op = {"op": "stage", "emitter": emitter, "stage": stage["Name"]}
        want = stage.get("Enabled", True)
        if want != current.get("Enabled", True):
            op["enabled"] = want
            plan.change(f"{where}/Enabled", want, current.get("Enabled", True))
        if "Properties" in stage:
            write, reset = property_changes(stage["Properties"], current.get("Properties", {}),
                                            defaults.get("stages", {}).get(current["Class"], {}))
            if write or reset:
                op["properties"] = write
                if reset:
                    op["reset"] = reset
                plan.change(f"{where}/Properties", stage["Properties"], current.get("Properties", {}))
        if len(op) > 3:
            plan.ops.append((op, where))
        stage_location = dict(location, group="SimulationStage", stage=stage["Name"])
        if "Modules" in stage:
            groups.append(Group(stage_location, f"{where}/Modules", stage["Modules"], current.get("Modules", [])))
        else:
            for module in current.get("Modules", []):
                other[module["Name"]] = (stage_location, module)


# ---------------------------------------------------------------------------
# Lightweight emitters
# ---------------------------------------------------------------------------
def plan_stateless(plan, path, name, authored, current, defaults, property_changes, plan_renderers):
    base = f"{path}/Stateless"
    template_changed = False
    if "Properties" in authored:
        write, reset = property_changes(authored["Properties"], current.get("Properties", {}),
                                        defaults.get("stateless", {}))
        if write or reset:
            op = {"op": "stateless", "emitter": name, "properties": write}
            if reset:
                op["reset"] = reset
            plan.change(f"{base}/Properties", authored["Properties"], current.get("Properties", {}), op)
            template_changed = "EmitterTemplate" in write or "EmitterTemplate" in reset
    if template_changed:
        # A new template rebuilds the module list: modules are planned against it in the next pass.
        return
    if "Modules" in authored:
        live = {module["Class"]: module for module in current.get("Modules", [])}
        listed = set()
        for index, module in enumerate(authored["Modules"]):
            where = f"{base}/Modules/{index}"
            listed.add(module["Class"])
            have = live.get(module["Class"])
            if have is None:
                plan.refuse(f"{where}/Class", "StatelessModuleNotFound", f"The template of '{name}' has no module "
                            f"'{module['Class']}' (a lightweight emitter's modules come from its EmitterTemplate): "
                            + ", ".join(sorted(live)) + ".", module["Class"], None)
                continue
            _plan_stateless_module(plan, where, name, module["Class"], module.get("Properties", {}),
                                   have.get("Properties", {}), defaults, property_changes)
        for module_class, have in live.items():
            if module_class not in listed and have.get("Properties"):
                _plan_stateless_module(plan, f"{base}/Modules", name, module_class, {}, have.get("Properties", {}),
                                       defaults, property_changes)
    if "Renderers" in authored:
        plan_renderers(plan, f"{base}/Renderers", name, authored["Renderers"], current.get("Renderers", []), defaults)


def _plan_stateless_module(plan, where, emitter, module_class, authored, live, defaults, property_changes):
    write, reset = property_changes(authored, live, defaults.get("stateless_modules", {}).get(module_class, {}))
    if write or reset:
        op = {"op": "stateless_module", "emitter": emitter, "class": module_class, "properties": write}
        if reset:
            op["reset"] = reset
        plan.change(f"{where}/Properties", authored, live, op)


def classes_wanted(body, live_body):
    """(simulation stage classes, lightweight module classes) whose defaults the planner compares against."""
    stages = set()
    modules = set()
    for source in (body, live_body):
        for emitter in source.get("Emitters", []) if type(source.get("Emitters")) is list else []:
            if type(emitter) is not dict:
                continue
            for stage in emitter.get("SimulationStages", []) if type(emitter.get("SimulationStages")) is list else []:
                if type(stage) is dict and type(stage.get("Class")) is str:
                    stages.add(stage["Class"])
            stateless = emitter.get("Stateless")
            if type(stateless) is dict and type(stateless.get("Modules")) is list:
                for module in stateless["Modules"]:
                    if type(module) is dict and type(module.get("Class")) is str:
                        modules.add(module["Class"])
    return sorted(stages), sorted(modules)


__all__ = ("check_module", "check_system_stacks", "check_emitter_stacks", "plan_graph", "plan_system_stacks",
           "plan_declared_parameters", "plan_emitter_stacks", "plan_stateless", "classes_wanted", "normalize_value",
           "object_path")
