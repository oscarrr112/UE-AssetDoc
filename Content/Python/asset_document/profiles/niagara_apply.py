"""System-level Apply of profile.niagara_system.v1 (N2): the planner that turns a system document and the live
system (its extract) into ``fx.niagara.edit.v1`` ops, and the Diff that reports the same differences.

Body semantics (MCP/schemas/assetdocument/Niagara.md):

* ``UserParameters`` is complete when present: a parameter that is not listed is deleted (the editor's delete:
  inputs linked to it keep its value as a local value), a new name adds one, ``RenamedFrom`` renames one (links
  follow; it applies while the system does not have the new name yet, so a re-apply is a no-op, and swapping
  two names takes two applies).  ``Type`` cannot change (delete and add instead).  ``Value`` omitted keeps the value (a new
  parameter: the type's default); ``Description`` and ``Category`` omitted are none.  A data interface's
  ``Properties`` are complete (omitted ones are the class defaults).  Order is not authored (the extract lists
  parameters by name).
* ``Emitters`` is complete when present: not listed = deleted, a new name = added (``From``: an emitter asset path,
  ``{"Emitter": path, "Version": guid}``, ``"Empty"`` or ``"Minimal"``; omitted: the ``Parent`` asset, else
  ``"Empty"``), ``RenamedFrom`` renames.  New emitters are added at the end; the editor cannot reorder emitters,
  so the listed order must be the system's.  Per emitter: ``Enabled`` (omitted: enabled), ``Parent`` (omitted or
  null: none; the parent can be removed, not changed), ``Properties`` (emitter Details; complete when present,
  omitted ones are the defaults), ``Renderers`` (complete when present: by ``Name``, ``Class``, ``Enabled``
  omitted = enabled, ``Properties`` kept when omitted and complete when present; a different Class replaces the
  renderer; listed order).
* Module stacks (``SystemSpawn``, ``SystemUpdate``; per emitter ``EmitterSpawn``, ``EmitterUpdate``,
  ``ParticleSpawn``, ``ParticleUpdate``, ``EventHandlers``, ``SimulationStages``), the declared ``Parameters`` and
  lightweight emitters (``Mode: "Stateless"``, their ``Stateless`` value): ``niagara_stack`` (N3).
* ``ScratchPads`` (N4, ``niagara_script``): the system's list is complete (not listed = deleted after the stacks no
  longer use it, a new name = added, ``RenamedFrom`` renames); an emitter's pads are edited in place (the list names
  the pads it has, ``Niagara.ScratchPadScope``).  Each pad is a script body (``Usage`` fixed once added).  The
  scratch pad edits run before each pass's stack edits (``niagara.apply_scratch_pads``).

The apply runs in passes: each pass plans against a fresh read and edits what exists (a new emitter's renderers
and a new parameter's value are planned in the pass after it is added), until a pass plans nothing.
"""
from __future__ import annotations

from asset_document.diff.model import Difference, DiffKind
from asset_document.profiles import data_table_common as common
from asset_document.profiles import niagara_script as scripts
from asset_document.profiles import niagara_stack as stacks

PREFIX = "Niagara"
SYSTEM_STACK_KEYS = ("SystemSpawn", "SystemUpdate")
EMITTER_STACK_KEYS = ("EmitterSpawn", "EmitterUpdate", "ParticleSpawn", "ParticleUpdate", "EventHandlers",
                      "SimulationStages")
SCRATCH_PAD_KEYS = ("ScratchPads",)
EMITTER_KEYS = ("Name", "RenamedFrom", "From", "Enabled", "Mode", "Parent", "Properties", "Renderers", "Stateless") \
    + EMITTER_STACK_KEYS + SCRATCH_PAD_KEYS
RENDERER_KEYS = ("Name", "Class", "Enabled", "Properties")
USER_PARAMETER_KEYS = ("Name", "RenamedFrom", "Type", "Value", "Description", "Category")
_MISSING = object()


def _escape(name):
    return common.escape(str(name))


def _fail(path, message, code):
    raise common.error(path, message, f"{PREFIX}.{code}")


# ---------------------------------------------------------------------------
# Body validation
# ---------------------------------------------------------------------------
def _object(value, path, keys, what):
    if type(value) is not dict:
        _fail(path, f"A {what} is an object.", "InvalidBody")
    for key in value:
        if key not in keys:
            _fail(f"{path}/{_escape(key)}", f"Unknown {what} key '{key}'. Keys: {', '.join(keys)}.", "UnknownKey")
    return value


def _string(value, path, what, empty=False):
    if type(value) is not str or (not empty and not value):
        _fail(path, f"{what} is a {'' if empty else 'non-empty '}string.", "InvalidBody")
    return value


def _unique(entries, path, what, key="Name"):
    seen = set()
    for index, entry in enumerate(entries):
        if entry[key] in seen:
            _fail(f"{path}/{index}/{key}", f"Two {what}s are named '{entry[key]}'.", "DuplicateName")
        seen.add(entry[key])


def _check_parameter(entry, path):
    _object(entry, path, USER_PARAMETER_KEYS, "user parameter")
    for key in ("Name", "Type"):
        if key not in entry:
            _fail(f"{path}/{key}", f"A user parameter has a {key}.", "InvalidBody")
    for key in ("Name", "RenamedFrom"):
        if key in entry and (type(entry[key]) is not str or not entry[key].startswith("User.") or len(entry[key]) <= 5):
            _fail(f"{path}/{key}", f"A user parameter name starts with 'User.' (got {entry[key]!r}).",
                  "InvalidUserParameterName")
    _string(entry["Type"], f"{path}/Type", "Type")
    for key in ("Description", "Category"):
        if key in entry:
            _string(entry[key], f"{path}/{key}", key, empty=True)


def _emitter_reference(value, path, what):
    """An emitter asset reference: a path or {"Emitter": path, "Version"?: guid} -> {"Emitter", "Version"?}."""
    if type(value) is str:
        value = {"Emitter": value}
    _object(value, path, ("Emitter", "Version"), what)
    _string(value.get("Emitter"), f"{path}/Emitter", "Emitter")
    if "Version" in value:
        _string(value["Version"], f"{path}/Version", "Version")
    return value


def _check_emitter(entry, path):
    _object(entry, path, EMITTER_KEYS, "emitter")
    _string(entry.get("Name"), f"{path}/Name", "Name")
    if "RenamedFrom" in entry:
        _string(entry["RenamedFrom"], f"{path}/RenamedFrom", "RenamedFrom")
    if "Enabled" in entry and type(entry["Enabled"]) is not bool:
        _fail(f"{path}/Enabled", "Enabled is true or false.", "InvalidBody")
    if entry.get("Mode", "Standard") not in ("Standard", "Stateless"):
        _fail(f"{path}/Mode", "Mode is \"Standard\" or \"Stateless\".", "InvalidBody")
    if entry.get("Parent") is not None:
        _emitter_reference(entry["Parent"], f"{path}/Parent", "parent")
    if "From" in entry and entry["From"] not in ("Empty", "Minimal"):
        _emitter_reference(entry["From"], f"{path}/From", "From")
    if "Properties" in entry and type(entry["Properties"]) is not dict:
        _fail(f"{path}/Properties", "Properties is an object.", "InvalidBody")
    if "Renderers" in entry:
        if type(entry["Renderers"]) is not list:
            _fail(f"{path}/Renderers", "Renderers is a list.", "InvalidBody")
        for index, renderer in enumerate(entry["Renderers"]):
            where = f"{path}/Renderers/{index}"
            _object(renderer, where, RENDERER_KEYS, "renderer")
            _string(renderer.get("Name"), f"{where}/Name", "Name")
            _string(renderer.get("Class"), f"{where}/Class", "Class")
            if "Enabled" in renderer and type(renderer["Enabled"]) is not bool:
                _fail(f"{where}/Enabled", "Enabled is true or false.", "InvalidBody")
            if "Properties" in renderer and type(renderer["Properties"]) is not dict:
                _fail(f"{where}/Properties", "Properties is an object.", "InvalidBody")
        _unique(entry["Renderers"], f"{path}/Renderers", "renderer")
    stacks.check_emitter_stacks(entry, path)
    if "ScratchPads" in entry:
        scripts.check_pads(entry["ScratchPads"], f"{path}/ScratchPads")
    if "Stateless" in entry and "Renderers" in entry["Stateless"]:
        _check_emitter({"Name": entry["Name"], "Renderers": entry["Stateless"]["Renderers"]}, f"{path}/Stateless")


def check_body(body):
    """Structural checks of a system Body (the planner relies on them)."""
    for key in ("UserParameters", "Parameters", "Emitters", "SystemSpawn", "SystemUpdate", "ScratchPads"):
        if key in body and type(body[key]) is not list:
            _fail(f"/Body/{key}", f"{key} is a list.", "InvalidBody")
    for index, entry in enumerate(body.get("UserParameters", [])):
        _check_parameter(entry, f"/Body/UserParameters/{index}")
    _unique(body.get("UserParameters", []), "/Body/UserParameters", "user parameter")
    for index, entry in enumerate(body.get("Emitters", [])):
        _check_emitter(entry, f"/Body/Emitters/{index}")
    _unique(body.get("Emitters", []), "/Body/Emitters", "emitter")
    if "ScratchPads" in body:
        scripts.check_pads(body["ScratchPads"], "/Body/ScratchPads")
    stacks.check_system_stacks(body)
    if "UserParameters" in body:
        # Linking an input to a user parameter the system lacks adds it (the editor's link): with a complete
        # UserParameters list it must be listed there.
        declared = {entry["Name"] for entry in body["UserParameters"]}
        for path, link in stacks.links(body):
            if link.startswith("User.") and link not in declared:
                _fail(path, f"'{link}' is not in UserParameters: add it there (linking an input to a user parameter "
                      "the system lacks would add it).", "UndeclaredUserParameter")
    return body


# ---------------------------------------------------------------------------
# Planner
# ---------------------------------------------------------------------------
class Plan:
    """ops: [(op, document path)]; refusals: differences no op can make (raised before editing); differences:
    every difference (Diff; scratch pad differences have no op here: ``niagara.apply_scratch_pads`` makes them)."""

    def __init__(self):
        self.ops = []
        self.refusals = []
        self.differences = []

    def change(self, path, authored, live, op=None, kind=DiffKind.REPLACE, authored_present=True, live_present=True):
        self.differences.append(Difference(path, kind, authored_present, authored, live_present, live))
        if op is not None:
            self.ops.append((op, path))

    def refuse(self, path, code, message, authored=None, live=None):
        self.differences.append(Difference(path, DiffKind.REPLACE, True, authored, True, live))
        self.refusals.append(common.error(path, message, f"{PREFIX}.{code}"))


def _same(left, right):
    return common.same(left, right, exact=False)


def _match(authored, live_names):
    """[(entry, live name or None)]: an entry whose Name the system does not have yet claims its RenamedFrom (a
    rename); the others match by Name. (A re-apply after the rename matches by Name: renames are idempotent; swaps
    and chains of names take two applies.)"""
    sources = [None] * len(authored)
    taken = set()
    for index, entry in enumerate(authored):
        source = entry.get("RenamedFrom")
        if source is not None and entry["Name"] not in live_names and source in live_names and source not in taken:
            sources[index] = source
            taken.add(source)
    for index, entry in enumerate(authored):
        if sources[index] is None and entry["Name"] in live_names and entry["Name"] not in taken:
            sources[index] = entry["Name"]
            taken.add(entry["Name"])
    return list(zip(authored, sources))


def _structure(plan, matches, live_names, base, rename_op, delete_op):
    """Deletes (freeing names), then renames (to names the system does not have)."""
    kept = {source for _, source in matches if source is not None}
    for name in live_names:
        if name not in kept:
            plan.change(base, None, name, delete_op(name), DiffKind.REMOVE, False, True)
    for index, (entry, source) in enumerate(matches):
        if source is not None and source != entry["Name"]:
            plan.change(f"{base}/{index}/Name", entry["Name"], source, rename_op(source, entry["Name"]))


def _struct_as_authored(authored, live, default):
    """``live`` in the shape of ``authored`` where it is a struct (the edit decodes a struct value as an edit: members
    it omits keep their value). The typed codec writes structs and maps as objects alike: an object is taken as a
    struct when its keys are the default value's (a struct's members are fixed, a map's keys are its entries)."""
    if type(authored) is dict and type(live) is dict and type(default) is dict and set(live) == set(default):
        return {key: _struct_as_authored(value, live[key], default[key]) for key, value in authored.items()
                if key in live}
    if type(authored) is list and type(live) is list and len(authored) == len(live):
        # An array of structs (a mesh renderer's Meshes, a curve's Keys): each element lists the members it sets.
        # The array's default has no elements; its elements are structs when they all have the same members.
        members = {frozenset(item) for item in live if type(item) is dict}
        shape = next(iter(members)) if len(members) == 1 and all(type(item) is dict for item in live) else None
        return [_struct_as_authored(mine, theirs, {key: theirs[key] for key in shape}
                                    if shape is not None and type(mine) is dict and set(mine) <= shape else None)
                for mine, theirs in zip(authored, live)]
    return live


def property_changes(authored, live, defaults):
    """(values to write, names to reset) for complete sparse Properties: live and authored list values that
    differ from the defaults; defaults may be partial (unknown defaults are written; the edit skips equal ones). A
    struct value lists the members it sets (``_struct_as_authored``)."""
    write = {}
    for name, value in authored.items():
        current = live[name] if name in live else defaults.get(name, _MISSING)
        if current is not _MISSING:
            current = _struct_as_authored(value, current, defaults.get(name))
        if current is _MISSING or not _same(value, current):
            write[name] = value
    reset = sorted(name for name in live if name not in authored)
    return write, reset


def _plan_parameters(plan, body, live_body, defaults):
    if "UserParameters" not in body:
        return
    authored = body["UserParameters"]
    live = {entry["Name"]: entry for entry in live_body.get("UserParameters", [])}
    matches = _match(authored, list(live))
    base = "/Body/UserParameters"
    _structure(plan, matches, list(live), base, lambda old, new: {"op": "rename_user_parameter", "name": old, "to": new},
               lambda name: {"op": "delete_user_parameter", "name": name})
    for index, (entry, source) in enumerate(matches):
        path = f"{base}/{index}"
        name = entry["Name"]
        if source is None:
            plan.change(path, entry, None, {"op": "add_user_parameter", "name": name, "type": entry["Type"]},
                        DiffKind.ADD, True, False)
            continue
        current = live[source]
        if entry["Type"] != current["Type"]:
            plan.refuse(f"{path}/Type", "UserParameterRetype", f"'{name}' is a {current['Type']}; a user parameter's "
                        "type cannot change (delete it and add a new one).", entry["Type"], current["Type"])
            continue
        if "Value" in entry:
            _plan_parameter_value(plan, f"{path}/Value", name, entry["Value"], current.get("Value", _MISSING), defaults)
        for key, field in (("Description", "description"), ("Category", "category")):
            want = entry.get(key, "")
            have = current.get(key, "")
            if want != have:
                plan.change(f"{path}/{key}", want, have, {"op": "user_parameter", "name": name, field: want})


def _plan_parameter_value(plan, path, name, want, have, defaults):
    if type(want) is dict and "DataInterface" in want and type(have) is dict and "DataInterface" in have:
        spec = want["DataInterface"]
        if type(spec) is not dict:
            _fail(path, "A data interface value is {\"DataInterface\": {\"Class\": ..., \"Properties\": {...}}}.",
                  "InvalidUserParameterValue")
        current = have["DataInterface"]
        label = current.get("Class", "")
        write, reset = property_changes(spec.get("Properties", {}), current.get("Properties", {}),
                                        defaults.get("data_interfaces", {}).get(label, {}))
        if write or reset or ("Class" in spec and not _same_class(spec["Class"], label)):
            value = {"DataInterface": {"Properties": write}}
            if "Class" in spec:
                value["DataInterface"]["Class"] = spec["Class"]
            op = {"op": "user_parameter", "name": name, "value": value}
            if reset:
                op["reset"] = reset
            plan.change(path, want, have, op)
        return
    if have is _MISSING or not _same(want, have):
        plan.change(path, want, None if have is _MISSING else have, {"op": "user_parameter", "name": name, "value": want},
                    live_present=have is not _MISSING)


def _same_class(left, right):
    """A data interface class by label (``Curve``) or path (``/Script/Niagara.NiagaraDataInterfaceCurve``)."""
    def full(name):
        return name if name.startswith("/") else f"/Script/Niagara.NiagaraDataInterface{name}"
    return type(left) is str and type(right) is str and full(left) == full(right)


def _from(entry, path, plan):
    """The add_emitter source of a new emitter (From, else its Parent, else an empty emitter)."""
    source = entry.get("From")
    parent = entry.get("Parent")
    if source is None:
        source = parent if parent is not None else "Empty"
    elif parent is not None and (source in ("Empty", "Minimal")
                                 or not _same_parent(_emitter_reference(parent, f"{path}/Parent", "parent"),
                                                     _emitter_reference(source, f"{path}/From", "From"))):
        plan.refuse(f"{path}/Parent", "ParentImmutable", "A new emitter's parent is the emitter asset it is added from "
                    "(only an emitter asset whose Is Inheritable option is on can be one): leave From out (it defaults to "
                    "the Parent) or make them the same.", parent, source)
    if source == "Empty":
        return {"kind": "empty"}
    if source == "Minimal":
        return {"kind": "minimal"}
    reference = _emitter_reference(source, f"{path}/From", "From")
    result = {"kind": "asset", "emitter": _object_path(reference["Emitter"])}
    if "Version" in reference:
        result["version"] = reference["Version"]
    if parent is not None:
        # The editor makes it the parent only when the asset is inheritable (Niagara.NotInheritable at .../Parent).
        result["inherit"] = True
    return result


def _object_path(path):
    """/Game/FX/NE_A -> /Game/FX/NE_A.NE_A (an asset's object path)."""
    leaf = path.rsplit("/", 1)[-1]
    return path if "." in leaf else f"{path}.{leaf}"


def _same_parent(want, have):
    if want is None or have is None:
        return want is have
    return (_object_path(want["Emitter"]) == _object_path(have["Emitter"])
            and want.get("Version") == have.get("Version"))


def _plan_emitters(plan, body, live_body, defaults):
    if "Emitters" not in body:
        return
    authored = body["Emitters"]
    live_list = live_body.get("Emitters", [])
    live = {entry["Name"]: entry for entry in live_list}
    live_names = [entry["Name"] for entry in live_list]
    matches = _match(authored, live_names)
    base = "/Body/Emitters"
    _structure(plan, matches, live_names, base, lambda old, new: {"op": "rename_emitter", "name": old, "to": new},
               lambda name: {"op": "delete_emitter", "name": name})
    # Order: kept emitters stay where they are, new ones are added at the end.
    kept = {source: entry["Name"] for entry, source in matches if source is not None}
    order = [kept[name] for name in live_names if name in kept]
    order += [entry["Name"] for entry, source in matches if source is None]
    if order != [entry["Name"] for entry in authored]:
        plan.refuse(base, "EmitterOrder", "The editor cannot reorder emitters: existing emitters keep their order and new "
                    "ones are added at the end. List them in this order: " + ", ".join(order) + ".",
                    [entry["Name"] for entry in authored], order)
    for index, (entry, source) in enumerate(matches):
        path = f"{base}/{index}"
        if source is None:
            if entry.get("Mode", "Standard") == "Stateless":
                for key in ("From", "Parent", "Properties", "Renderers") + EMITTER_STACK_KEYS + SCRATCH_PAD_KEYS:
                    if key in entry:
                        plan.refuse(f"{path}/{key}", "StatelessEmitter", "A lightweight emitter is authored by its "
                                    "Stateless value ({Properties, Modules, Renderers}).", entry[key], None)
                plan.change(path, entry, None, {"op": "add_emitter", "name": entry["Name"], "from": {"kind": "stateless"}},
                            DiffKind.ADD, True, False)
                continue
            plan.change(path, entry, None, {"op": "add_emitter", "name": entry["Name"], "from": _from(entry, path, plan)},
                        DiffKind.ADD, True, False)
            continue
        _plan_emitter(plan, path, entry, live[source], defaults)


def _plan_emitter(plan, path, entry, current, defaults):
    name = entry["Name"]
    stateless = current.get("Mode") == "Stateless"
    if (entry.get("Mode", "Standard") == "Stateless") != stateless:
        plan.refuse(f"{path}/Mode", "ModeChange", "An emitter's mode (standard / lightweight) cannot change: delete the "
                    "emitter and add a new one.", entry.get("Mode", "Standard"), current.get("Mode", "Standard"))
        return
    want = entry.get("Enabled", True)
    have = current.get("Enabled", True)
    # Enabled first, disabled last: the editor refuses renderer deletes on a disabled emitter.
    if want and not have:
        plan.change(f"{path}/Enabled", want, have, {"op": "emitter", "name": name, "enabled": True})
    _plan_emitter_content(plan, path, entry, current, stateless, defaults)
    if have and not want:
        plan.change(f"{path}/Enabled", want, have, {"op": "emitter", "name": name, "enabled": False})


def _plan_emitter_content(plan, path, entry, current, stateless, defaults):
    name = entry["Name"]
    if stateless:
        for key in ("Parent", "Properties", "Renderers") + EMITTER_STACK_KEYS + SCRATCH_PAD_KEYS:
            if key in entry:
                plan.refuse(f"{path}/{key}", "StatelessEmitter", "A lightweight emitter is authored by its Stateless "
                            "value ({Properties, Modules, Renderers}).", entry[key], None)
        if "Stateless" in entry:
            stacks.plan_stateless(plan, path, name, entry["Stateless"], current.get("Stateless", {}), defaults,
                                  property_changes, _plan_renderers)
        return
    if "Stateless" in entry:
        plan.refuse(f"{path}/Stateless", "ModeChange", "Stateless is the value of a lightweight emitter (Mode: "
                    "\"Stateless\").", entry["Stateless"], None)
    want_parent = entry.get("Parent")
    have_parent = current.get("Parent")
    if want_parent is None and have_parent is not None:
        plan.change(f"{path}/Parent", None, have_parent, {"op": "emitter", "name": name, "remove_parent": True},
                    authored_present="Parent" in entry)
    elif want_parent is not None and not _same_parent(_emitter_reference(want_parent, f"{path}/Parent", "parent"),
                                                      have_parent):
        reference = _emitter_reference(want_parent, f"{path}/Parent", "parent")
        if have_parent is not None and _object_path(reference["Emitter"]) == _object_path(have_parent["Emitter"]):
            # The emitter header's parent version menu.
            plan.change(f"{path}/Parent/Version", reference.get("Version"), have_parent.get("Version"),
                        {"op": "parent_version", "emitter": name, "version": reference.get("Version", "")},
                        authored_present="Version" in reference, live_present="Version" in have_parent)
        else:
            plan.refuse(f"{path}/Parent", "ParentImmutable", "An existing emitter's parent can be removed (Parent null) "
                        "or switched to another version of the same emitter, but not set or changed to another emitter: "
                        "the editor sets a parent only when it adds an emitter from an inheritable emitter asset (Is "
                        "Inheritable on; engine templates such as Fountain are not, and an emitter added from one has no "
                        "parent). Add a new emitter whose Parent is an inheritable emitter asset instead, or leave Parent "
                        "out.", want_parent, have_parent)
    if "Properties" in entry:
        write, reset = property_changes(entry["Properties"], current.get("Properties", {}), defaults.get("emitter", {}))
        if write or reset:
            op = {"op": "emitter", "name": name, "properties": write}
            if reset:
                op["reset"] = reset
            plan.change(f"{path}/Properties", entry["Properties"], current.get("Properties", {}), op)
    if "Renderers" in entry:
        _plan_renderers(plan, f"{path}/Renderers", name, entry["Renderers"], current.get("Renderers", []), defaults)
    stacks.plan_emitter_stacks(plan, path, name, entry, current, defaults, property_changes)
    if "ScratchPads" in entry:
        plan.differences += scripts.pad_differences(entry["ScratchPads"], current.get("ScratchPads", []),
                                                    f"{path}/ScratchPads")


def _plan_renderers(plan, base, emitter, authored, live_list, defaults):
    live = {entry["Name"]: entry for entry in live_list}
    replaced = {entry["Name"] for entry in authored if entry["Name"] in live and live[entry["Name"]]["Class"] != entry["Class"]}
    names = {entry["Name"] for entry in authored}
    for entry in live_list:
        if entry["Name"] not in names or entry["Name"] in replaced:
            plan.change(base, None, entry, {"op": "delete_renderer", "emitter": emitter, "name": entry["Name"]},
                        DiffKind.REMOVE, False, True)
    order = [entry["Name"] for entry in live_list if entry["Name"] in names and entry["Name"] not in replaced]
    for index, entry in enumerate(authored):
        if entry["Name"] not in live or entry["Name"] in replaced:
            plan.change(f"{base}/{index}", entry, live.get(entry["Name"]),
                        {"op": "add_renderer", "emitter": emitter, "name": entry["Name"], "class": entry["Class"]},
                        DiffKind.ADD, True, entry["Name"] in live)
            order.append(entry["Name"])
    wanted = [entry["Name"] for entry in authored]
    if order != wanted:
        plan.change(base, wanted, [entry["Name"] for entry in live_list],
                    {"op": "order_renderers", "emitter": emitter, "names": wanted})
    for index, entry in enumerate(authored):
        current = live.get(entry["Name"])
        if current is None or entry["Name"] in replaced:
            continue
        path = f"{base}/{index}"
        want = entry.get("Enabled", True)
        have = current.get("Enabled", True)
        if want != have:
            plan.change(f"{path}/Enabled", want, have, {"op": "renderer", "emitter": emitter, "name": entry["Name"],
                                                         "enabled": want})
        if "Properties" not in entry:
            continue
        write, reset = property_changes(entry["Properties"], current.get("Properties", {}),
                                        defaults.get("renderers", {}).get(entry["Class"], {}))
        if write or reset:
            op = {"op": "renderer", "emitter": emitter, "name": entry["Name"], "properties": write}
            if reset:
                op["reset"] = reset
            plan.change(f"{path}/Properties", entry.get("Properties", {}), current.get("Properties", {}), op)


def plan_system(body, live_body, defaults):
    """The edits that move the live system (its extracted Body) towards the document's Body."""
    plan = Plan()
    # User parameters first: an input linked to a renamed or new parameter is set after the parameter exists.
    _plan_parameters(plan, body, live_body, defaults)
    _plan_emitters(plan, body, live_body, defaults)
    stacks.plan_declared_parameters(plan, body, live_body)
    stacks.plan_system_stacks(plan, body, live_body)
    if "ScratchPads" in body:
        plan.differences += scripts.pad_differences(body["ScratchPads"], live_body.get("ScratchPads", []),
                                                    "/Body/ScratchPads")
    return plan


def defaults_wanted(body, live_body):
    """(renderer classes, data interface classes, simulation stage classes, lightweight module classes) whose
    defaults the planner compares against."""
    renderers = set()
    interfaces = set()
    for source in (body, live_body):
        for emitter in source.get("Emitters", []) if type(source.get("Emitters")) is list else []:
            if type(emitter) is not dict:
                continue
            stateless = emitter.get("Stateless") if type(emitter.get("Stateless")) is dict else {}
            for listed in (emitter.get("Renderers"), stateless.get("Renderers")):
                for renderer in listed if type(listed) is list else []:
                    if type(renderer) is dict and type(renderer.get("Class")) is str:
                        renderers.add(renderer["Class"])
        for parameter in source.get("UserParameters", []) if type(source.get("UserParameters")) is list else []:
            value = parameter.get("Value") if type(parameter) is dict else None
            if type(value) is dict and type(value.get("DataInterface")) is dict \
                    and type(value["DataInterface"].get("Class")) is str:
                interfaces.add(value["DataInterface"]["Class"])
    return (sorted(renderers), sorted(interfaces)) + stacks.classes_wanted(body, live_body)


def op_path(primitive_path, op_paths):
    """/ops/<n>/... of an edit call -> the document path of op n (a property: .../Properties/<name>)."""
    parts = [part for part in (primitive_path or "").split("/") if part]
    if len(parts) >= 2 and parts[0] == "ops" and parts[1].isdigit() and int(parts[1]) < len(op_paths):
        base = op_paths[int(parts[1])]
        if len(parts) >= 4 and parts[2] == "properties" and base.endswith("/Properties"):
            return f"{base}/{parts[3]}"
        if len(parts) >= 4 and parts[2] == "from" and parts[3] == "inherit":
            # add_emitter's inherit: the new emitter's Parent (Niagara.NotInheritable).
            return f"{base}/Parent"
        if len(parts) >= 4 and parts[2] == "set" and base.endswith("/Inputs"):
            # An input path mirrors the document: <input>/Dynamic/Inputs/<nested input>/...
            return "/".join([base] + parts[3:])
        return base
    return "/Body"


__all__ = ("check_body", "plan_system", "property_changes", "defaults_wanted", "op_path", "Plan")
