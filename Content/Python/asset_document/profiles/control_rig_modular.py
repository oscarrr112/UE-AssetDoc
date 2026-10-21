"""Modular rigs of profile.control_rig.v1 (phase CR4, design §3.7, §4.4 step 9): Modules, Connections and
ModularSettings as the Module Assembly panel edits them (UModularRigController through rig.controlrig.edit.v1).

- ``Modules`` is the module tree: entries by ``Name`` (``RenamedFrom`` renames), ``Class`` (a changed class is "Swap
  Module Class"), ``Parent`` (omitted = a root module), ``Config`` (the module's config overrides, complete: an
  override the entry does not list is reset) and ``Bindings`` (module variable -> host variable or
  ``<module>/<variable>``, complete). The order is the order of siblings (the tree's order under each parent).
- ``Connections`` maps ``<module>/<connector>`` to an element key (``"Bone(pelvis)"``) or a list of keys (array
  connectors); complete. Connections are made without auto-resolve, module by module down the tree (a module's
  primary connector before its others: the sockets its children target appear as its connectors resolve), so the
  result is exactly the document. Connecting a primary connector moves its module under
  the module that owns the target, as the editor does; the planned ``Parent`` is written again afterwards.
- ``ModularSettings`` ``{AutoResolve}`` (default true) is the Module Assembly toggle.

Order of one apply: renames, adds (in document order, parents first), class swaps, reparents, removals of unlisted
modules (children first), sibling order, config, bindings, then connections (disconnects, then connects), the
parents again (primary connections reparent), settings.
"""
from __future__ import annotations

from asset_document.profiles import data_table_common as common
from asset_document.profiles import ik_common as ik

escape = common.escape
MODULE_DEFAULTS = {"Parent": "", "Config": {}, "Bindings": {}}
SETTINGS_DEFAULTS = {"AutoResolve": True}


def error(path, message, code):
    return common.error(path, message, code)


def _renamed(entry):
    old = entry.get("RenamedFrom")
    return old if type(old) is str and old and old != entry.get("Name") else None


def normalize_connections(connections):
    """A one-target list is the single target (the read writes array connectors as lists only when they hold
    several)."""
    return {key: value[0] if type(value) is list and len(value) == 1 else value for key, value in connections.items()}


def validate(body):
    """Shape of Modules / Connections / ModularSettings."""
    for index, module in enumerate(body.get("Modules", [])):
        where = f"/Body/Modules/{index}"
        if type(module) is not dict or type(module.get("Name")) is not str or type(module.get("Class")) is not str:
            raise error(where, "A module is {Name, Class, Parent?, Config?, Bindings?}.", "ControlRig.InvalidBody")
        if not module["Name"] or "/" in module["Name"] or ":" in module["Name"]:
            raise error(f"{where}/Name", "A module name is not empty and has no '/' or ':'.", "ControlRig.InvalidName")
        for key in module:
            if key not in ("Name", "Class", "Parent", "Config", "Bindings", "RenamedFrom"):
                raise error(f"{where}/{escape(key)}", f"Unknown module field '{key}' (Name, Class, Parent, Config, "
                            "Bindings, RenamedFrom).", "ControlRig.InvalidBody")
        if "Parent" in module and type(module["Parent"]) is not str:
            raise error(f"{where}/Parent", "Parent is a module name.", "ControlRig.InvalidBody")
        for key in ("Config", "Bindings"):
            if key in module and type(module[key]) is not dict:
                raise error(f"{where}/{key}", f"{key} is an object.", "ControlRig.InvalidBody")
        for variable, source in module.get("Bindings", {}).items():
            if type(source) is not str:
                raise error(f"{where}/Bindings/{escape(variable)}", "A binding is the source variable's name "
                            "(\"<variable>\" or \"<module>/<variable>\").", "ControlRig.InvalidBody")
        if "RenamedFrom" in module and (type(module["RenamedFrom"]) is not str or not module["RenamedFrom"]):
            raise error(f"{where}/RenamedFrom", "RenamedFrom is the module's current name.", "ControlRig.InvalidBody")
    for connector, target in body.get("Connections", {}).items():
        where = f"/Body/Connections/{escape(connector)}"
        if "/" not in connector:
            raise error(where, "A connection key is \"<module>/<connector>\" (\"Arm_l/Root\").", "ControlRig.InvalidBody")
        targets = target if type(target) is list else [target]
        if not targets or any(type(item) is not str or "(" not in item for item in targets):
            raise error(where, "A connection is an element key (\"Bone(pelvis)\") or a list of keys.",
                        "ControlRig.InvalidBody")
    settings = body.get("ModularSettings", {})
    for key, value in settings.items():
        if key != "AutoResolve" or type(value) is not bool:
            raise error(f"/Body/ModularSettings/{escape(key)}", "ModularSettings is {AutoResolve: bool}.",
                        "ControlRig.InvalidBody")


def check(body, live):
    """Refusals that need no write: duplicate names, parents the document does not list (or lists after the child),
    renames of modules the rig does not have, connections of modules the document does not list."""
    if "Modules" not in body:
        names = [entry.get("Name") for entry in live.get("Modules", [])]
        for connector in body.get("Connections", {}):
            module = connector.split("/", 1)[0]
            if module not in names:
                raise error(f"/Body/Connections/{escape(connector)}", f"The rig has no module '{module}'."
                            f"{ik.describe_candidates(module, names, 'Modules')}", "ControlRig.ModuleNotFound")
        return
    modules = body["Modules"]
    have = {entry.get("Name") for entry in live.get("Modules", [])}
    seen = {}
    for index, entry in enumerate(modules):
        where = f"/Body/Modules/{index}"
        if entry["Name"] in seen:
            raise error(f"{where}/Name", f"Module '{entry['Name']}' is listed twice (also at /Body/Modules/"
                        f"{seen[entry['Name']]}).", "ControlRig.DuplicateModule")
        seen[entry["Name"]] = index
        old = _renamed(entry)
        if old is not None and old not in have and entry["Name"] not in have:
            raise error(f"{where}/RenamedFrom", f"RenamedFrom names module '{old}', which the rig does not have."
                        f"{ik.describe_candidates(old, sorted(have), 'Modules')}", "ControlRig.ModuleNotFound")
    # A Parent cycle is reported as one (no order of its modules lists every parent first).
    cycle = module_cycle(modules)
    on_cycle = set(cycle[1]) if cycle is not None else set()
    for index, entry in enumerate(modules):
        parent = entry.get("Parent", "")
        if not parent:
            continue
        where = f"/Body/Modules/{index}/Parent"
        if parent not in seen:
            raise error(where, f"'{parent}' is not a module of Modules (the region lists every module)."
                        f"{ik.describe_candidates(parent, list(seen), 'Modules')}", "ControlRig.ModuleNotFound")
        if entry["Name"] in on_cycle:
            first, chain = cycle
            message = ("A module cannot be its own parent." if len(chain) == 2 else
                       f"The module Parent chain is a cycle: {' -> '.join(chain)} (each name is the previous one's "
                       "Parent). Give one of them another Parent (or none: a root module).")
            raise error(f"/Body/Modules/{first}/Parent", message, "ControlRig.ModuleCycle")
        if seen[parent] > index:
            raise error(where, f"Module '{parent}' is listed after its child '{entry['Name']}'; list parents first.",
                        "ControlRig.ParentAfterChild")
    for connector in body.get("Connections", {}):
        module = connector.split("/", 1)[0]
        if module not in seen:
            raise error(f"/Body/Connections/{escape(connector)}", f"Connection of module '{module}', which Modules "
                        f"does not list.{ik.describe_candidates(module, list(seen), 'Modules')}",
                        "ControlRig.ModuleNotFound")


def module_cycle(modules):
    """(index of the first listed module on a Parent cycle, the cycle's names from it back to it) or None."""
    parents = {entry["Name"]: entry.get("Parent", "") for entry in modules}
    index_of = {entry["Name"]: index for index, entry in enumerate(modules)}
    for entry in modules:
        chain = [entry["Name"]]
        name = parents.get(entry["Name"], "")
        while name and name in parents and name not in chain:
            chain.append(name)
            name = parents[name]
        if name and name in chain:
            loop = chain[chain.index(name):]
            first = min(loop, key=lambda item: index_of[item])
            start = loop.index(first)
            ordered = loop[start:] + loop[:start]
            return index_of[first], ordered + [first]
    return None


def _live_names(modules, live):
    """Document module name -> the rig's current name of it (renames applied), None for a new module."""
    have = {entry.get("Name") for entry in live}
    result = {}
    for entry in modules:
        old = _renamed(entry)
        result[entry["Name"]] = old if old is not None and old in have else (entry["Name"] if entry["Name"] in have
                                                                              else None)
    return result


def _op(make, op, path, fields=None):
    return make(op, path, fields or {})


def phase_renames(body, live, make):
    if "Modules" not in body:
        return []
    have = {entry.get("Name") for entry in live.get("Modules", [])}
    ops = []
    for index, entry in enumerate(body["Modules"]):
        old = _renamed(entry)
        if old is None or old not in have:
            continue
        where = f"/Body/Modules/{index}"
        if entry["Name"] in have:
            ops.append(make({"op": "module.remove", "name": entry["Name"]}, where, {}))
        ops.append(make({"op": "module.rename", "name": old, "to": entry["Name"]}, where,
                        {"to": f"{where}/Name", "name": f"{where}/RenamedFrom"}))
    return ops


def phase_adds(body, live, make):
    if "Modules" not in body:
        return []
    have = {entry.get("Name") for entry in live.get("Modules", [])}
    ops = []
    for index, entry in enumerate(body["Modules"]):
        if entry["Name"] in have:
            continue
        where = f"/Body/Modules/{index}"
        ops.append(make({"op": "module.add", "name": entry["Name"], "class": entry["Class"],
                         "parent": entry.get("Parent", "")}, where,
                        {"name": f"{where}/Name", "class": f"{where}/Class", "parent": f"{where}/Parent"}))
    return ops


def phase_swaps(body, live, make):
    if "Modules" not in body:
        return []
    have = {entry.get("Name"): entry for entry in live.get("Modules", [])}
    ops = []
    for index, entry in enumerate(body["Modules"]):
        current = have.get(entry["Name"])
        if current is not None and current.get("Class") != entry["Class"]:
            where = f"/Body/Modules/{index}/Class"
            ops.append(make({"op": "module.swap", "name": entry["Name"], "class": entry["Class"]}, where,
                            {"class": where}))
    return ops


def phase_reparents(body, live, make):
    if "Modules" not in body:
        return []
    have = {entry.get("Name"): entry for entry in live.get("Modules", [])}
    ops = []
    for index, entry in enumerate(body["Modules"]):
        current = have.get(entry["Name"])
        if current is not None and current.get("Parent", "") != entry.get("Parent", ""):
            where = f"/Body/Modules/{index}/Parent"
            ops.append(make({"op": "module.reparent", "name": entry["Name"], "parent": entry.get("Parent", "")}, where,
                            {"parent": where}))
    return ops


def phase_removes(body, live, make):
    """Unlisted modules, children first (the editor's delete also removes the children it still has)."""
    if "Modules" not in body:
        return []
    wanted = {entry["Name"] for entry in body["Modules"]}
    return [make({"op": "module.remove", "name": entry["Name"]}, "/Body/Modules", {})
            for entry in reversed(live.get("Modules", [])) if entry.get("Name") not in wanted]


def phase_order(body, live, make):
    """The order of siblings: each listed module moved to its index under its parent."""
    if "Modules" not in body:
        return []
    groups = {}
    for entry in live.get("Modules", []):
        groups.setdefault(entry.get("Parent", ""), []).append(entry["Name"])
    wanted = {}
    for entry in body["Modules"]:
        wanted.setdefault(entry.get("Parent", ""), []).append(entry["Name"])
    ops = []
    for parent, names in wanted.items():
        current = list(groups.get(parent, []))
        for index, name in enumerate(names):
            if name not in current or current.index(name) == index:
                continue
            current.remove(name)
            current.insert(index, name)
            ops.append(make({"op": "module.reorder", "name": name, "index": index}, "/Body/Modules", {}))
    return ops


def phase_config(body, live, make):
    if "Modules" not in body:
        return []
    have = {entry.get("Name"): entry for entry in live.get("Modules", [])}
    resets, sets = [], []
    for index, entry in enumerate(body["Modules"]):
        where = f"/Body/Modules/{index}/Config"
        mine = entry.get("Config", {})
        theirs = (have.get(entry["Name"]) or {}).get("Config", {})
        for path in theirs:
            if path not in mine:
                resets.append(make({"op": "module.config", "name": entry["Name"], "path": path, "value": None}, where,
                                   {}))
        for path, value in mine.items():
            if path not in theirs or not common.same(value, theirs[path], False):
                sets.append(make({"op": "module.config", "name": entry["Name"], "path": path, "value": value},
                                 f"{where}/{escape(path)}", {"path": f"{where}/{escape(path)}",
                                                             "value": f"{where}/{escape(path)}"}))
    return resets + sets


def phase_bindings(body, live, make):
    if "Modules" not in body:
        return []
    have = {entry.get("Name"): entry for entry in live.get("Modules", [])}
    unbinds, binds = [], []
    for index, entry in enumerate(body["Modules"]):
        where = f"/Body/Modules/{index}/Bindings"
        mine = entry.get("Bindings", {})
        theirs = (have.get(entry["Name"]) or {}).get("Bindings", {})
        for variable in theirs:
            if variable not in mine:
                unbinds.append(make({"op": "module.bind", "name": entry["Name"], "variable": variable, "source": ""},
                                    where, {}))
        for variable, source in mine.items():
            if theirs.get(variable) != source:
                path = f"{where}/{escape(variable)}"
                binds.append(make({"op": "module.bind", "name": entry["Name"], "variable": variable, "source": source},
                                  path, {"variable": path, "source": path}))
    return unbinds + binds


def _targets(value):
    return value if type(value) is list else [value]


def phase_disconnects(body, live, make):
    if "Connections" not in body:
        return []
    mine = normalize_connections(body["Connections"])
    return [make({"op": "disconnect", "connector": connector}, "/Body/Connections", {})
            for connector in live.get("Connections", {}) if connector not in mine]


# The rig's primary connectors ("<module>/<connector>"), which control_rig_apply.run_modular adds to the live body
# it reads for the connects.
PRIMARIES = "__PrimaryConnectors"


def connect_order(connections, modules, primaries):
    """Connection keys in the order the apply makes them: module by module down the module tree (parents first, as
    Modules lists them), each module's primary connector first. A module's sockets, which its children's primary
    connectors target, and the matches of its secondary connectors exist once its own connectors resolve."""
    parents = {entry["Name"]: entry.get("Parent", "") for entry in modules if type(entry) is dict and "Name" in entry}
    rank = {}

    def place(name, seen=()):
        if name in rank or name in seen:
            return
        parent = parents.get(name, "")
        if parent in parents:
            place(parent, seen + (name,))
        rank[name] = len(rank)
    for name in parents:
        place(name)
    primary = set(primaries)
    keys = list(connections)
    return sorted(keys, key=lambda key: (rank.get(key.split("/", 1)[0], len(rank)), key not in primary, keys.index(key)))


def phase_connects(body, live, make):
    """Module by module down the tree, each module's primary connector first: a module's sockets (its children's
    targets) appear once its connectors resolve, so one apply connects every level of an explicit Connections map."""
    if "Connections" not in body:
        return []
    theirs = normalize_connections(live.get("Connections", {}))
    mine = normalize_connections(body["Connections"])
    ops = []
    for connector in connect_order(mine, body.get("Modules", live.get("Modules", [])), live.get(PRIMARIES, ())):
        target = mine[connector]
        if theirs.get(connector) != target:
            where = f"/Body/Connections/{escape(connector)}"
            ops.append(make({"op": "connect", "connector": connector, "targets": _targets(target)}, where,
                            {"connector": where, "targets": where}))
    return ops


def phase_settings(body, live, make):
    if "ModularSettings" not in body:
        return []
    wanted = body["ModularSettings"].get("AutoResolve", True)
    if wanted == live.get("ModularSettings", {}).get("AutoResolve", True):
        return []
    return [make({"op": "modular.settings", "auto_resolve": wanted}, "/Body/ModularSettings/AutoResolve", {})]


# (name, planner) in apply order; the apply re-reads the rig after each batch that changed it.
PHASES = (("renames", phase_renames), ("adds", phase_adds), ("swaps", phase_swaps), ("reparents", phase_reparents),
          ("removes", phase_removes), ("order", phase_order), ("config", phase_config), ("bindings", phase_bindings),
          ("disconnects", phase_disconnects), ("connects", phase_connects), ("reparents", phase_reparents),
          ("order", phase_order), ("settings", phase_settings))


__all__ = ("MODULE_DEFAULTS", "PHASES", "PRIMARIES", "connect_order", "SETTINGS_DEFAULTS", "check", "module_cycle",
           "normalize_connections", "validate")
