"""Script graphs of profile.niagara_script.v1 and of system / emitter ``ScratchPads`` (N4): Body checks, the
planner that turns a script body into ``fx.niagara.edit.v1`` script ops plus graph edits (graph.* primitives,
``graphs.schemas.niagara``), and the semantic Diff that reports the same differences.

Script body (MCP/schemas/assetdocument/Niagara/scripts.md)::

    {"Usage": "Module" | "Function" | "DynamicInput",          # immutable once the script exists
     "Settings": {...},                                          # the script Details (complete when present)
     "Versions": [{"Number": "M.m", "Guid"?, "Exposed"?, "ChangeDescription"?, "Deprecated"?,
                   "DeprecationMessage"?, "Visible"?}],          # the version menu (complete when present)
     "Parameters": [{"Name", "Type"?, "Default"?, "DefaultMode"?, "DefaultBinding"?, "StaticSwitchDefault"?,
                     "Meta"?}],                                  # the parameter panel's Details
     "Graph": {"Nodes", "Links", "PinDefaults"},                 # complete when present
     "EditExposedVersion"?: true}                                # opt-in: edit a versioned script's exposed version

* ``Settings`` omitted keeps them; present, a value it leaves out is reset to the default.
* ``Parameters`` lists the parameters whose Details the document states; the parameters themselves are the ones
  the graph uses (Map Get / Map Set pins, static switches), as the editor keeps them (an unlisted one keeps its
  Details; a listed one the graph does not use is refused, ``Niagara.ParameterNotReferenced``).  Per entry
  ``DefaultMode`` omitted = ``Value``, ``DefaultBinding`` is the bound parameter of mode ``Binding``, ``Default``
  and ``StaticSwitchDefault`` omitted keep the value, ``Meta`` is complete (omitted = the default metadata).
* ``Versions``: versions it leaves out are deleted (never the exposed version or 1.0), a new ``Number`` adds a
  version (a copy of the nearest lower one, as the version menu), ``Exposed`` exposes one; with versioning enabled
  the other keys describe the exposed version, and editing the version that is exposed before the apply is refused
  (``Niagara.ExposedVersionEdit``, decision §8.3) unless ``EditExposedVersion`` is true.

Scratch pad entries are ``{"Name", "RenamedFrom"?, "Usage", "Settings", "Parameters", "Graph"}`` (no versions).
"""
from __future__ import annotations

import contextlib
import contextvars

from asset_document.diff.model import Difference, DiffKind
from asset_document.graphs.schemas.niagara import check_graph, has_version_setting, pins_hold
from asset_document.profiles import data_table_common as common

PREFIX = "Niagara"
USAGES = ("Module", "Function", "DynamicInput")
PAD_USAGES = ("Module", "DynamicInput")
SCRIPT_KEYS = ("Usage", "Settings", "Versions", "Parameters", "Graph", "EditExposedVersion")
PAD_KEYS = ("Name", "RenamedFrom", "Usage", "Settings", "Parameters", "Graph")
PARAMETER_KEYS = ("Name", "Type", "Default", "DefaultMode", "DefaultBinding", "StaticSwitchDefault", "Meta")
VERSION_KEYS = ("Number", "Guid", "Exposed", "ChangeDescription", "Deprecated", "DeprecationMessage", "Visible")
DEFAULT_MODES = ("Value", "Binding", "Custom", "FailIfPreviouslyNotSet")
CONTENT_KEYS = ("Settings", "Parameters", "Graph")
_VERSION_DEFAULTS = {"ChangeDescription": "", "Deprecated": False, "DeprecationMessage": "", "Visible": True}
EMPTY_GRAPH = {"Nodes": [], "Links": [], "PinDefaults": {}}
_MISSING = object()
# The read is sparse: Settings and node Properties list the values that differ from a new script's / node's. A value
# the document lists and the read leaves out is compared with that default; where the default is not known the
# value counts as different (the apply writes it, an apply never skips it as "unchanged").
_SETTINGS_DEFAULTS = {}
# {node id: the Details of a new node of that kind} (fx.niagara.read.v1 node_defaults), or None (not read).
_NODE_DEFAULTS = contextvars.ContextVar("niagara_node_defaults", default=None)


def _fail(path, message, code="InvalidBody"):
    raise common.error(path, message, f"{PREFIX}.{code}")


def _escape(name):
    return common.escape(str(name))


def _same(left, right):
    return common.same(left, right, exact=False)


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------
def _check_parameter(entry, path):
    if type(entry) is not dict:
        _fail(path, "A parameter is an object {Name, Type?, Default?, DefaultMode?, DefaultBinding?, "
              "StaticSwitchDefault?, Meta?}.")
    for key in entry:
        if key not in PARAMETER_KEYS:
            _fail(f"{path}/{_escape(key)}", f"Unknown parameter key '{key}'. Keys: {', '.join(PARAMETER_KEYS)}.")
    if type(entry.get("Name")) is not str or not entry["Name"]:
        _fail(f"{path}/Name", "A parameter needs its full Name (e.g. \"Module.Strength\").")
    for key in ("Type", "DefaultBinding"):
        if key in entry and (type(entry[key]) is not str or not entry[key]):
            _fail(f"{path}/{key}", f"{key} is a non-empty string.")
    mode = entry.get("DefaultMode", "Value")
    if mode not in DEFAULT_MODES:
        _fail(f"{path}/DefaultMode", f"DefaultMode is one of {', '.join(DEFAULT_MODES)}.")
    if "DefaultBinding" in entry and mode != "Binding":
        _fail(f"{path}/DefaultBinding", "DefaultBinding (the bound parameter) goes with DefaultMode \"Binding\".")
    if "Default" in entry and mode != "Value":
        _fail(f"{path}/Default", "Default is the value of DefaultMode \"Value\".")
    switch = entry.get("StaticSwitchDefault")
    # JSON numbers may arrive as floats (1.0): an integral value is the integer.
    if "StaticSwitchDefault" in entry and not (type(switch) is int or (type(switch) is float and switch.is_integer())):
        _fail(f"{path}/StaticSwitchDefault", "StaticSwitchDefault is an integer (bool switches: 0 / 1, enums: the "
              "value's index).")
    if "Meta" in entry and type(entry["Meta"]) is not dict:
        _fail(f"{path}/Meta", "Meta is an object (FNiagaraVariableMetaData values).")


def _check_version(entry, path):
    if type(entry) is not dict:
        _fail(path, "A version is an object {Number, Guid?, Exposed?, ChangeDescription?, Deprecated?, "
              "DeprecationMessage?, Visible?}.")
    for key in entry:
        if key not in VERSION_KEYS:
            _fail(f"{path}/{_escape(key)}", f"Unknown version key '{key}'. Keys: {', '.join(VERSION_KEYS)}.")
    number = entry.get("Number")
    parts = number.split(".") if type(number) is str else []
    if len(parts) != 2 or not all(part.isdigit() for part in parts):
        _fail(f"{path}/Number", "Number is \"Major.Minor\" (e.g. \"1.0\").")
    for key, kind in (("Guid", str), ("ChangeDescription", str), ("DeprecationMessage", str), ("Exposed", bool),
                      ("Deprecated", bool), ("Visible", bool)):
        if key in entry and type(entry[key]) is not kind:
            _fail(f"{path}/{key}", f"{key} is a {'string' if kind is str else 'boolean'}.")


def _unique(entries, path, key, what):
    seen = set()
    for index, entry in enumerate(entries):
        value = entry.get(key)
        if value in seen:
            _fail(f"{path}/{index}/{key}", f"{what} '{value}' is listed twice.", "DuplicateName")
        seen.add(value)


def check_script_body(body, path="/Body", *, pad=False):
    """Structural checks of a script body (a script asset's Body, or one scratch pad entry)."""
    if type(body) is not dict:
        _fail(path, "A script body is an object.")
    allowed = PAD_KEYS if pad else SCRIPT_KEYS
    for key in body:
        if key not in allowed:
            hint = " (scratch pads have no versions)" if pad and key in ("Versions", "EditExposedVersion") else ""
            _fail(f"{path}/{_escape(key)}", f"Unknown key '{key}'{hint}. Keys: {', '.join(allowed)}.")
    if pad:
        if type(body.get("Name")) is not str or not body["Name"]:
            _fail(f"{path}/Name", "A scratch pad needs a Name.")
        if "RenamedFrom" in body and (type(body["RenamedFrom"]) is not str or not body["RenamedFrom"]):
            _fail(f"{path}/RenamedFrom", "RenamedFrom is the scratch pad's current name.")
    usages = PAD_USAGES if pad else USAGES
    if "Usage" in body and body["Usage"] not in usages:
        _fail(f"{path}/Usage", f"Usage is one of {', '.join(usages)}.")
    if "Settings" in body and type(body["Settings"]) is not dict:
        _fail(f"{path}/Settings", "Settings is an object (the script Details).")
    if "EditExposedVersion" in body and type(body["EditExposedVersion"]) is not bool:
        _fail(f"{path}/EditExposedVersion", "EditExposedVersion is a boolean.")
    for key, check, ident, what in (("Parameters", _check_parameter, "Name", "Parameter"),
                                    ("Versions", _check_version, "Number", "Version")):
        if key not in body:
            continue
        if type(body[key]) is not list:
            _fail(f"{path}/{key}", f"{key} is a list.")
        for index, entry in enumerate(body[key]):
            check(entry, f"{path}/{key}/{index}")
        _unique(body[key], f"{path}/{key}", ident, what)
    if len([entry for entry in body.get("Versions", []) if entry.get("Exposed") is True]) > 1:
        _fail(f"{path}/Versions", "At most one version is Exposed.")
    if "Graph" in body:
        check_graph(body["Graph"], f"{path}/Graph")
    return body


def check_pads(pads, path):
    if type(pads) is not list:
        _fail(path, "ScratchPads is a list.")
    for index, entry in enumerate(pads):
        check_script_body(entry, f"{path}/{index}", pad=True)
    _unique(pads, path, "Name", "Scratch pad")


# ---------------------------------------------------------------------------
# Semantic comparison (Diff and the apply's "anything to do?")
# ---------------------------------------------------------------------------
def _same_text(left, right):
    """Pin default text: equal, or the same numbers ("1" / "1.0", "1,2,3" / "1.0,2.0,3.0")."""
    if left == right:
        return True
    if type(left) is not str or type(right) is not str:
        return False
    a, b = left.split(","), right.split(",")
    if len(a) != len(b):
        return False
    try:
        return all(_same(float(x), float(y)) for x, y in zip(a, b))
    except ValueError:
        return False


def _action_base(action):
    return action.partition("#")[0] if type(action) is str else action


def _same_action(want, have):
    return want == have or ("#" not in str(want) and _action_base(have) == want)


def set_settings_defaults(values):
    """The Settings of a new script (fx.niagara.read.v1 defaults "script"), constant for the session."""
    _SETTINGS_DEFAULTS.clear()
    _SETTINGS_DEFAULTS.update(values or {})


@contextlib.contextmanager
def node_defaults(values):
    """Within the block, node Properties the read leaves out compare with these defaults ({node id: Details of a new
    node of that kind}; a dict a reader may still fill)."""
    token = _NODE_DEFAULTS.set(values)
    try:
        yield values
    finally:
        _NODE_DEFAULTS.reset(token)


def collecting_node_defaults():
    """The dict the reads of the current node_defaults block fill, or None (reads then leave the defaults out)."""
    return _NODE_DEFAULTS.get()


def _listed_differs(value, live, key, defaults):
    """A value the document lists against the sparse read: the read's value, else the default (unknown: differs)."""
    if key in live:
        return not _same(value, live[key])
    default = defaults.get(key, _MISSING) if defaults is not None else _MISSING
    return default is _MISSING or not _same(value, default)


def graph_differences(authored, live, path, strict=False):
    """strict (planning): a node property the read leaves out (its default) counts as different when listed."""
    node_defaults_by_id = _NODE_DEFAULTS.get()
    out = []
    live_nodes = {node["Id"]: node for node in live.get("Nodes", [])}
    doc_ids = set()
    for index, node in enumerate(authored.get("Nodes", [])):
        where = f"{path}/Nodes/{index}"
        doc_ids.add(node["Id"])
        current = live_nodes.get(node["Id"])
        if current is None or not _same_action(node["Action"], current.get("Action")):
            out.append(Difference(where, DiffKind.ADD if current is None else DiffKind.REPLACE, True, node,
                                  current is not None, current))
            continue
        if "Position" in node and not _same(list(node["Position"]), list(current.get("Position", [0, 0]))):
            out.append(Difference(f"{where}/Position", DiffKind.REPLACE, True, node["Position"], True,
                                  current.get("Position")))
        if node.get("Comment", "") != current.get("Comment", ""):
            out.append(Difference(f"{where}/Comment", DiffKind.REPLACE, True, node.get("Comment", ""),
                                  "Comment" in current, current.get("Comment")))
        if has_version_setting(node["Action"]):
            want, have = node.get("Version", ""), current.get("Version", "")
            # "" is the exposed version, which the read does not name: a version the document names that the
            # read leaves out is the exposed one.
            if want != have and have != "":
                out.append(Difference(f"{where}/Version", DiffKind.REPLACE, "Version" in node, want,
                                      "Version" in current, have))
        live_properties = current.get("Properties", {})
        defaults = None if strict or node_defaults_by_id is None else node_defaults_by_id.get(node["Id"])
        for key, value in node.get("Properties", {}).items():
            # Sparse: a value the read leaves out is the new node's (compared when known; strict planning writes it).
            if _listed_differs(value, live_properties, key, defaults):
                out.append(Difference(f"{where}/Properties/{_escape(key)}", DiffKind.REPLACE, True, value,
                                      key in live_properties, live_properties.get(key)))
        if "Pins" in node and not pins_hold(current.get("Pins", {}), node["Pins"]):
            out.append(Difference(f"{where}/Pins", DiffKind.REPLACE, True, node["Pins"], "Pins" in current,
                                  current.get("Pins")))
    for node_id, node in live_nodes.items():
        if node_id not in doc_ids:
            out.append(Difference(f"{path}/Nodes", DiffKind.REMOVE, False, None, True, node))
    want_links = {(link["From"], link["To"]) for link in authored.get("Links", [])}
    have_links = {(link["From"], link["To"]) for link in live.get("Links", [])}
    for link in sorted(want_links - have_links):
        out.append(Difference(f"{path}/Links", DiffKind.ADD, True, {"From": link[0], "To": link[1]}, False, None))
    for link in sorted(have_links - want_links):
        out.append(Difference(f"{path}/Links", DiffKind.REMOVE, False, None, True, {"From": link[0], "To": link[1]}))
    want_defaults = authored.get("PinDefaults", {})
    have_defaults = live.get("PinDefaults", {})
    for pin in sorted(set(want_defaults) | set(have_defaults)):
        if not _same_text(want_defaults.get(pin), have_defaults.get(pin)):
            out.append(Difference(f"{path}/PinDefaults/{_escape(pin)}", DiffKind.REPLACE, pin in want_defaults,
                                  want_defaults.get(pin), pin in have_defaults, have_defaults.get(pin)))
    return out


def settings_differences(authored, live, path):
    out = []
    for key, value in authored.items():
        # A value the read leaves out is the new script's: a listed value differs from it unless equal (a
        # Description the module did not have was "unchanged" and the apply skipped it).
        if _listed_differs(value, live, key, _SETTINGS_DEFAULTS):
            out.append(Difference(f"{path}/{_escape(key)}", DiffKind.REPLACE, True, value, key in live, live.get(key)))
    for key in live:
        if key not in authored:
            out.append(Difference(f"{path}/{_escape(key)}", DiffKind.REMOVE, False, None, True, live[key]))
    return out


def _parameter_fields(entry, current):
    """[(key, authored, live)] of the Details the entry states that the live parameter does not hold."""
    out = []
    mode = entry.get("DefaultMode", "Value")
    if mode != current.get("DefaultMode", "Value"):
        out.append(("DefaultMode", mode, current.get("DefaultMode", "Value")))
    if mode == "Binding" and "DefaultBinding" in entry and entry["DefaultBinding"] != current.get("DefaultBinding"):
        out.append(("DefaultBinding", entry.get("DefaultBinding"), current.get("DefaultBinding")))
    if "Default" in entry and mode == "Value" and not _same(entry["Default"], current.get("Default")):
        out.append(("Default", entry["Default"], current.get("Default")))
    if "StaticSwitchDefault" in entry and entry["StaticSwitchDefault"] != current.get("StaticSwitchDefault"):
        out.append(("StaticSwitchDefault", entry["StaticSwitchDefault"], current.get("StaticSwitchDefault")))
    if not _same(entry.get("Meta", {}), current.get("Meta", {})):
        out.append(("Meta", entry.get("Meta", {}), current.get("Meta", {})))
    return out


def parameter_differences(authored, live, path):
    out = []
    by_name = {entry["Name"]: entry for entry in live}
    for index, entry in enumerate(authored):
        where = f"{path}/{index}"
        current = by_name.get(entry["Name"])
        if current is None:
            out.append(Difference(where, DiffKind.ADD, True, entry, False, None))
            continue
        if "Type" in entry and entry["Type"] != current.get("Type"):
            out.append(Difference(f"{where}/Type", DiffKind.REPLACE, True, entry["Type"], True, current.get("Type")))
        for key, want, have in _parameter_fields(entry, current):
            out.append(Difference(f"{where}/{key}", DiffKind.REPLACE, True, want, have is not None, have))
    return out


def _version_row(entry):
    return {key: entry.get(key, default) for key, default in _VERSION_DEFAULTS.items()}


def _match_versions(authored, live):
    """[(entry, live entry or None)]: by Guid when the entry names one the asset has, else by Number (a document
    copied from another asset names that asset's version guids; new versions get new ones)."""
    by_guid = {entry.get("Guid"): entry for entry in live}
    by_number = {entry.get("Number"): entry for entry in live}
    return [(entry, by_guid[entry["Guid"]] if entry.get("Guid") in by_guid else by_number.get(entry["Number"]))
            for entry in authored]


def version_differences(authored, live, path):
    out = []
    matches = _match_versions(authored, live)
    expose = any(entry.get("Exposed") is True for entry in authored)
    matched = set()
    for index, (entry, current) in enumerate(matches):
        where = f"{path}/{index}"
        if current is None:
            out.append(Difference(where, DiffKind.ADD, True, entry, False, None))
            continue
        matched.add(current.get("Guid"))
        if entry["Number"] != current.get("Number"):
            out.append(Difference(f"{where}/Number", DiffKind.REPLACE, True, entry["Number"], True, current.get("Number")))
        if _version_row(entry) != _version_row(current):
            out.append(Difference(where, DiffKind.REPLACE, True, entry, True, current))
        if expose and entry.get("Exposed", False) != current.get("Exposed", False):
            out.append(Difference(f"{where}/Exposed", DiffKind.REPLACE, True, entry.get("Exposed", False), True,
                                  current.get("Exposed", False)))
    for current in live:
        if current.get("Guid") not in matched:
            out.append(Difference(path, DiffKind.REMOVE, False, None, True, current))
    return out


def content_differences(authored, live, path):
    """Differences of Settings, Parameters and Graph (the content of the edited version)."""
    out = []
    if "Settings" in authored:
        out += settings_differences(authored["Settings"], live.get("Settings", {}), f"{path}/Settings")
    if "Graph" in authored:
        out += graph_differences(authored["Graph"], live.get("Graph", EMPTY_GRAPH), f"{path}/Graph")
    if "Parameters" in authored:
        out += parameter_differences(authored["Parameters"], live.get("Parameters", []), f"{path}/Parameters")
    return out


def script_differences(authored, live, path="/Body"):
    out = []
    if "Usage" in authored and authored["Usage"] != live.get("Usage"):
        out.append(Difference(f"{path}/Usage", DiffKind.REPLACE, True, authored["Usage"], "Usage" in live,
                              live.get("Usage")))
    if "Versions" in authored:
        out += version_differences(authored["Versions"], live.get("Versions", []), f"{path}/Versions")
    return out + content_differences(authored, live, path)


def _pad_matches(authored, live_names):
    """[(entry, live name or None)]: RenamedFrom claims the old name while the new one does not exist yet."""
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


def pad_differences(authored, live, path):
    """Differences of one owner's ScratchPads list (complete; order not authored)."""
    out = []
    live_by_name = {entry["Name"]: entry for entry in live}
    matches = _pad_matches(authored, list(live_by_name))
    kept = {source for _, source in matches if source is not None}
    for name, entry in live_by_name.items():
        if name not in kept:
            out.append(Difference(path, DiffKind.REMOVE, False, None, True, name))
    for index, (entry, source) in enumerate(matches):
        where = f"{path}/{index}"
        if source is None:
            out.append(Difference(where, DiffKind.ADD, True, entry, False, None))
            continue
        if source != entry["Name"]:
            out.append(Difference(f"{where}/Name", DiffKind.REPLACE, True, entry["Name"], True, source))
        out += script_differences(entry, live_by_name[source], where)
    return out


# ---------------------------------------------------------------------------
# Planning
# ---------------------------------------------------------------------------
def sparse_changes(authored, live):
    """(values to write, names to reset) for a complete Details object against the sparse live one."""
    write = {name: value for name, value in authored.items() if name not in live or not _same(value, live[name])}
    reset = sorted(name for name in live if name not in authored)
    return write, reset


def _scoped(op, scope):
    if scope is not None:
        op["scope"] = scope
    return op


def settings_op(authored, live, scope=None):
    write, reset = sparse_changes(authored, live)
    if not write and not reset:
        return None
    op = {"op": "script_settings", "properties": write}
    if reset:
        op["reset"] = reset
    return _scoped(op, scope)


def bound_targets(authored):
    """Names the listed parameters default-bind to (DefaultMode Binding): the graph's parameter list gains them once
    the binding is set (the load-time rule keeps the parameters bindings use)."""
    return {entry["DefaultBinding"] for entry in authored
            if entry.get("DefaultMode") == "Binding" and type(entry.get("DefaultBinding")) is str}


def parameter_ops(authored, live, path, scope=None, defer=frozenset(), only=None):
    """[(script_parameter op, document path)] for the listed parameters (live: after the graph edits).  Entries named
    in ``defer`` that the graph does not list yet are left out (a later round applies them); ``only`` limits the
    round to those names."""
    ops = []
    by_name = {entry["Name"]: entry for entry in live}
    for index, entry in enumerate(authored):
        if only is not None and entry["Name"] not in only:
            continue
        where = f"{path}/{index}"
        current = by_name.get(entry["Name"])
        if current is None and entry["Name"] in defer:
            continue
        if current is None:
            _fail(f"{where}/Name", f"The graph does not use a parameter '{entry['Name']}': a script's parameters are "
                  "the ones its Map Get / Map Set pins and static switches use (add it to a node's Pins).",
                  "ParameterNotReferenced")
        if "Type" in entry and entry["Type"] != current.get("Type"):
            _fail(f"{where}/Type", f"'{entry['Name']}' is a {current.get('Type')} in the graph: a parameter's type is "
                  "its pins' type (change the pin).", "ParameterType")
        fields = _parameter_fields(entry, current)
        if not fields:
            continue
        op = {"op": "script_parameter", "name": entry["Name"]}
        for key, want, have in fields:
            if key == "DefaultMode":
                op["default_mode"] = want
            elif key == "DefaultBinding":
                op["default_binding"] = want
            elif key == "Default":
                op["default"] = want
            elif key == "StaticSwitchDefault":
                op["static_switch_default"] = int(want)
            else:
                write, reset = sparse_changes(want, have or {})
                op["meta"] = {"properties": write, "reset": reset}
        ops.append((_scoped(op, scope), where))
    return ops


def versions_op(authored, live):
    if not version_differences(authored, live, "/Body/Versions"):
        return None
    entries = []
    for entry in authored:
        row = {"number": entry["Number"], "change_description": entry.get("ChangeDescription", ""),
               "deprecated": entry.get("Deprecated", False), "deprecation_message": entry.get("DeprecationMessage", ""),
               "visible": entry.get("Visible", True)}
        if "Guid" in entry:
            row["guid"] = entry["Guid"]
        if entry.get("Exposed") is True:
            row["exposed"] = True
        entries.append(row)
    return {"op": "versions", "versions": entries}


def check_versions(entries, path):
    """A Versions list (versioned scripts and emitter assets)."""
    if type(entries) is not list:
        _fail(path, "Versions is a list.")
    for index, entry in enumerate(entries):
        _check_version(entry, f"{path}/{index}")
    _unique(entries, path, "Number", "Version")
    if len([entry for entry in entries if entry.get("Exposed") is True]) > 1:
        _fail(path, "At most one version is Exposed.")


def edits_exposed_version(authored, live, changed=None):
    """Does the apply edit the version that is exposed now (versioned scripts and emitters)?  changed: whether the
    apply changes the version's content (default: the script content differs)."""
    if not live.get("Versions"):
        return False
    exposed = next((entry for entry in live["Versions"] if entry.get("Exposed")), None)
    target = exposed
    for entry, current in _match_versions(authored.get("Versions", []), live["Versions"]):
        if entry.get("Exposed") is True:
            target = current
    if changed is None:
        changed = bool(content_differences(authored, live, "/Body"))
    return target is not None and target is exposed and changed


def pad_structure(authored, live, path):
    """(add / rename ops, names to delete) of the system's own scratch pads."""
    live_names = [entry["Name"] for entry in live]
    matches = _pad_matches(authored, live_names)
    kept = {source for _, source in matches if source is not None}
    ops = []
    for index, (entry, source) in enumerate(matches):
        if source is not None and source != entry["Name"]:
            ops.append(({"op": "rename_scratch_pad", "name": source, "to": entry["Name"]}, f"{path}/{index}/Name"))
    for index, (entry, source) in enumerate(matches):
        if source is None:
            ops.append(({"op": "add_scratch_pad", "name": entry["Name"], "usage": entry.get("Usage", "Module")},
                        f"{path}/{index}"))
    return ops, [name for name in live_names if name not in kept]


def check_emitter_pads(authored, live, path):
    """An emitter's scratch pads are edited in place: the list names the pads the emitter has."""
    want = sorted(entry["Name"] for entry in authored)
    have = sorted(entry["Name"] for entry in live)
    if want != have or any("RenamedFrom" in entry and entry["RenamedFrom"] != entry["Name"] for entry in authored
                           if entry["Name"] not in have):
        _fail(path, "An emitter's scratch pads can be edited but not added, removed or renamed (the system's Scratch "
              "Pad panel makes system scratch pads): list exactly " + (", ".join(have) or "none") + ".",
              "ScratchPadScope")


def compile_failure(before_errors, result, nodes, path="/Body"):
    """(new error messages with their document path) of a script compile: nodes = [document node ids]."""
    failures = []
    for message in result.get("messages", []):
        if message.get("severity") != "Error" or message.get("message") in before_errors:
            continue
        node = message.get("node")
        where = f"{path}/Graph/Nodes/{nodes.index(node)}" if node in nodes else path
        failures.append((where, message.get("message", "")))
    if not failures and any(error not in before_errors for error in result.get("errors", [])):
        failures.append((path, next(error for error in result["errors"] if error not in before_errors)))
    return failures


__all__ = ("set_settings_defaults", "node_defaults", "collecting_node_defaults", "SCRIPT_KEYS", "PAD_KEYS", "USAGES", "check_script_body", "check_pads", "script_differences",
           "content_differences", "pad_differences", "settings_op", "parameter_ops", "bound_targets", "versions_op",
           "edits_exposed_version", "check_versions", "pad_structure", "check_emitter_pads", "compile_failure", "sparse_changes")
