# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""SoundCue document model: shape checks, the live cue (graph.* snapshot + audio.soundcue.read.v1), Extract and Diff.

Document Body (profile.sound_cue.v1)::

    "Body": {
      "Root": {"Input": "Mod", "Position": [200, 0]},            # the Output node: its link (a key or null)
      "Nodes": {                                                  # every sound node, by key (object name)
        "Mod":  {"Class": "Modulator", "Position": [-50, 0], "Properties": {"PitchMin": 0.9}, "Inputs": ["Rand"]},
        "Rand": {"Class": "Random", "Position": [-300, 0], "Properties": {"Weights": [1.0, 0.5]},
                 "Inputs": ["Step1", "Step2"]},
        "Step1": {"Class": "WavePlayer", "Position": [-600, -100],
                  "Properties": {"SoundWaveAssetPtr": "/Game/Audio/SW_Step1.SW_Step1"}},
        ...},
      "Comments": {"Variations": {"Text": "Variations", "Position": [-650, -220], "Size": [400, 520]}}
    }

* Regions: omitted = keep, present = complete (a node or comment not listed is deleted).
* ``Inputs`` is positional: entry *i* is input pin *i* (child slot *i*), a node key or null; its length is the
  number of input pins.  Omitted: the class's starting connectors, nothing connected.
* ``Properties`` of a node: the Details values of its sound node, complete (omitted = the value a new node of the
  class with that many inputs has); extract lists the differences.  Per-child arrays (Random ``Weights``, Mixer /
  Concatenator ``InputVolume``, crossfade ``CrossFadeInput``) have one entry per input, Group Control
  ``GroupSizes`` one less.
* ``Position`` omitted keeps the node (a new node goes left of its first parent); ``Comment`` / ``CommentVisible``
  omitted: none / false.
* Comment fields ``Color``, ``FontSize``, ``BubbleVisible``, ``ColorBubble``, ``MoveMode``, ``Details`` omitted: a
  new comment's values.
"""
from __future__ import annotations

import math
from types import MappingProxyType

from asset_document.diagnostics import AssetDocumentContractError, DiagnosticSeverity, RuntimeDiagnostic
from asset_document.diff.model import Difference, DiffKind
from asset_document.graphs.executor import graph_requirements
from asset_document.graphs.live import list_graphs, query_graphs
from asset_document.dto import PrimitiveRequirement
from asset_document.profiles import data_table_common as common
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements

ROUTE = "audio_profile"
PREFIX = "SoundCue"
CLASS = "/Script/Engine.SoundCue"
PROFILE_ID = "profile.sound_cue.v1"
READ = "audio.soundcue.read.v1"
LIFECYCLE = generic_factory_lifecycle(ROUTE)
REQUIREMENTS = (tuple(PrimitiveRequirement(item, 1, ROUTE) for item in (common.CLASS_RESOLVE, common.RESOLVE, READ))
                + lifecycle_requirements(LIFECYCLE) + graph_requirements(ROUTE))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}

BODY_KEYS = ("Root", "Nodes", "Comments")
ROOT_FIELDS = ("Input", "Position")
NODE_FIELDS = ("Class", "Position", "Properties", "Inputs", "Comment", "CommentVisible")
COMMENT_FIELDS = ("Text", "Position", "Size", "Color", "FontSize", "BubbleVisible", "ColorBubble", "MoveMode", "Details")
# Comment fields that are the comment node's Details properties.
COMMENT_PROPERTIES = {"Color": "CommentColor", "FontSize": "FontSize",
                      "BubbleVisible": "bCommentBubbleVisible_InDetailsPanel", "ColorBubble": "bColorCommentBubble",
                      "MoveMode": "MoveMode", "Details": "NodeDetails"}
# Per-child arrays: property and its length relative to the number of inputs.
PER_CHILD = {"Random": ("Weights", 0), "Mixer": ("InputVolume", 0), "Concatenator": ("InputVolume", 0),
             "DistanceCrossFade": ("CrossFadeInput", 0), "ParamCrossFade": ("CrossFadeInput", 0),
             "GroupControl": ("GroupSizes", -1)}
MAX_INPUTS = 32
ENGINE_PREFIX = "/Script/Engine.SoundNode"
NODE_ACTION_PREFIX = "SoundCue:Node:"
COMMENT_ACTION = "SoundCue:Comment"
# Cue Details the Body owns or the editor derives: never Properties.
NOT_AUTHORED = {
    "FirstNode": "Body.Root.Input (the Output node's link)", "AllNodes": "Body.Nodes",
    "SoundCueGraph": "Body", "Duration": None, "MaxDistance": None, "TotalSamples": None, "bHasDelayNode": None,
    "bHasConcatenatorNode": None, "bHasPlayWhenSilent": None, "CookedQualityIndex": None,
}
_INVALID_KEY_CHARACTERS = set("\"' ,/.:|&!~\n\r\t@#(){}[]=;^%$`*?<>\\+")
MAX_DEFAULT_ROWS = 16


def error(path, message, code):
    return common.error(path, message, code)


def escape(name):
    return common.escape(name)


# ---------------------------------------------------------------------------
# Shape
# ---------------------------------------------------------------------------
def class_label(value):
    """Canonical class label: engine nodes by short name without the SoundNode prefix, others by class path."""
    if value.startswith(ENGINE_PREFIX) and len(value) > len(ENGINE_PREFIX):
        return value[len(ENGINE_PREFIX):]
    if "/" not in value and "." not in value and value.startswith("SoundNode") and len(value) > len("SoundNode"):
        return value[len("SoundNode"):]
    return value


def check_key(key, path, what="node"):
    if type(key) is not str or not key or len(key) >= 1024 or key.lower() == "none" \
            or any(char in _INVALID_KEY_CHARACTERS for char in key):
        raise error(path, f"A {what} key is an object name: letters, digits, '_' and '-' (got {key!r}).",
                    f"{PREFIX}.InvalidKey")
    return key


def _number(value):
    return type(value) in (int, float)


def _pair(value, path, what, positive=False):
    if type(value) not in (list, tuple) or len(value) != 2 or not all(_number(item) for item in value) \
            or (positive and not all(item > 0 for item in value)):
        raise error(path, f"{what} is [{'width, height' if positive else 'x, y'}]"
                          f"{' (positive numbers)' if positive else ' (numbers)'}.", f"{PREFIX}.Invalid{what}")
    return [int(round(item)) for item in value]


def _fields(entry, path, allowed, what):
    if type(entry) is not dict:
        raise error(path, f"A {what} is an object.", f"{PREFIX}.Invalid{what.title()}")
    for field in entry:
        if field not in allowed:
            raise error(f"{path}/{escape(field)}", f"Unknown {what} field '{field}'. Fields: {', '.join(allowed)}.",
                        f"{PREFIX}.UnknownField")


def check_body(body):
    """Shape of the Body; returns the canonical Body (class labels, integer positions)."""
    out = {}
    if "Root" in body:
        root = body["Root"]
        _fields(root, "/Body/Root", ROOT_FIELDS, "root")
        if "Input" not in root:
            raise error("/Body/Root/Input", "Root.Input is required: the key of the node the Output plays, or null.",
                        f"{PREFIX}.MissingField")
        if root["Input"] is not None and type(root["Input"]) is not str:
            raise error("/Body/Root/Input", "Root.Input is a node key or null.", f"{PREFIX}.InvalidInput")
        out["Root"] = {"Input": root["Input"]}
        if "Position" in root:
            out["Root"]["Position"] = _pair(root["Position"], "/Body/Root/Position", "Position")
    keys = set()
    if "Nodes" in body:
        nodes = body["Nodes"]
        if type(nodes) is not dict:
            raise error("/Body/Nodes", "Nodes is an object of node key -> node.", f"{PREFIX}.InvalidNodes")
        out["Nodes"] = {}
        for key, entry in nodes.items():
            path = f"/Body/Nodes/{escape(key)}"
            check_key(key, path)
            keys.add(key)
            _fields(entry, path, NODE_FIELDS, "node")
            if type(entry.get("Class")) is not str or not entry["Class"]:
                raise error(f"{path}/Class", "Class is required: a sound node class (WavePlayer, Random, Mixer, ... or "
                            "a class path).", f"{PREFIX}.MissingField")
            node = {"Class": class_label(entry["Class"])}
            if "Position" in entry:
                node["Position"] = _pair(entry["Position"], f"{path}/Position", "Position")
            if "Properties" in entry:
                if type(entry["Properties"]) is not dict:
                    raise error(f"{path}/Properties", "Properties is an object of Details values.",
                                f"{PREFIX}.InvalidProperties")
                # A number that stores as a float32 reads back as its shortest decimal (as Extract writes it).
                node["Properties"] = tidy(thaw(entry["Properties"]))
            if "Inputs" in entry:
                inputs = entry["Inputs"]
                if type(inputs) not in (list, tuple) or any(item is not None and type(item) is not str for item in inputs):
                    raise error(f"{path}/Inputs", "Inputs is an array with one entry per input pin: a node key or null.",
                                f"{PREFIX}.InvalidInput")
                if len(inputs) > MAX_INPUTS:
                    raise error(f"{path}/Inputs", f"A sound node takes at most {MAX_INPUTS} inputs (got {len(inputs)}).",
                                f"{PREFIX}.ChildCount")
                node["Inputs"] = list(inputs)
            if "Comment" in entry:
                if type(entry["Comment"]) is not str:
                    raise error(f"{path}/Comment", "Comment is a string.", f"{PREFIX}.InvalidComment")
                node["Comment"] = entry["Comment"]
            if "CommentVisible" in entry:
                if type(entry["CommentVisible"]) is not bool:
                    raise error(f"{path}/CommentVisible", "CommentVisible is a boolean.", f"{PREFIX}.InvalidComment")
                node["CommentVisible"] = entry["CommentVisible"]
            out["Nodes"][key] = node
    if "Comments" in body:
        comments = body["Comments"]
        if type(comments) is not dict:
            raise error("/Body/Comments", "Comments is an object of comment key -> comment box.",
                        f"{PREFIX}.InvalidComments")
        out["Comments"] = {}
        for key, entry in comments.items():
            path = f"/Body/Comments/{escape(key)}"
            check_key(key, path, "comment")
            if key in keys:
                raise error(path, f"'{key}' is a node key too; keys name one object.", f"{PREFIX}.DuplicateKey")
            _fields(entry, path, COMMENT_FIELDS, "comment")
            if type(entry.get("Text")) is not str:
                raise error(f"{path}/Text", "Text is required (a string).", f"{PREFIX}.MissingField")
            comment = {field: (value if field in ("Text", "Position", "Size") else tidy(thaw(value)))
                       for field, value in entry.items()}
            if "Position" in entry:
                comment["Position"] = _pair(entry["Position"], f"{path}/Position", "Position")
            if "Size" in entry:
                comment["Size"] = _pair(entry["Size"], f"{path}/Size", "Size", positive=True)
            out["Comments"][key] = comment
    return out


def check_references(body, live_keys=()):
    """Inputs / Root.Input name nodes of the document (or, Nodes omitted, of the cue)."""
    keys = set(body["Nodes"]) if "Nodes" in body else set(live_keys)
    if "Root" in body and body["Root"]["Input"] is not None and body["Root"]["Input"] not in keys:
        raise error("/Body/Root/Input", f"Unknown node key '{body['Root']['Input']}'.", f"{PREFIX}.UnknownKey")
    for key, node in body.get("Nodes", {}).items():
        for index, child in enumerate(node.get("Inputs", ())):
            if child is not None and child not in keys:
                raise error(f"/Body/Nodes/{escape(key)}/Inputs/{index}", f"Unknown node key '{child}'.",
                            f"{PREFIX}.UnknownKey")


# ---------------------------------------------------------------------------
# Live cue
# ---------------------------------------------------------------------------
class Calls:
    """graph.* and audio.soundcue.read.v1 against one cue (editor handles handed off)."""

    def __init__(self, context, read, mutable=None):
        self.context = context
        self.graph_read = context.handoff(source_handle=read, target_provider_id="graph", access="read_only")
        self.graph_write = (context.handoff(source_handle=mutable, target_provider_id="graph", access="mutable")
                            if mutable is not None else None)
        self.audio = context.handoff(source_handle=read, target_provider_id="audio", access="read_only")
        self.changes = []
        self.writes = 0
        self._graph = None

    def call(self, primitive, values, path="/Body", write=False, paths=None):
        handle = self.graph_write if write else (self.audio if primitive == READ else self.graph_read)
        outcome = common.invoke(self.context, BY_ID, primitive, {"schema_version": 1, **values},
                                (NamedOpaqueHandleArgument("asset", handle),), path=path, paths=paths, prefix=PREFIX)
        self.changes.extend(outcome.changed_set)
        if write:
            self.writes += 1
        return outcome.value if type(outcome.value) is dict else {}

    def graph_info(self):
        if self._graph is None:
            infos = list_graphs(lambda values: self.call("graph.list.v1", values))
            if not infos:
                raise error("/Body", "The cue has no Sound Cue graph.", f"{PREFIX}.UnsupportedAsset")
            self._graph = infos[0]
        return self._graph

    def live(self):
        info = self.graph_info()
        graphs = query_graphs(lambda values: self.call("graph.query.v1", values), graphs=[{"graph_guid": info.guid}],
                              infos=(info,))
        return LiveCue(graphs[0])

    def read(self, defaults=None):
        values = {} if defaults is None else {"defaults": defaults}
        return self.call(READ, values)


def _input_pins(node):
    return [pin for pin in node.pins if pin.direction == "input"]


def _output_pin(node):
    return next((pin for pin in node.pins if pin.direction == "output"), None)


class LiveCue:
    """One graph.query snapshot of the cue: Root, sound nodes and comments by key."""

    def __init__(self, graph):
        self.graph = graph
        self.root = None
        self.nodes = {}
        self.comments = {}
        self.keys = {}
        for node in graph.nodes:
            name = node.extras.get("name")
            if node.implicit_role == "root":
                self.root = node
                self.keys[node.guid] = None
            elif node.action_key == COMMENT_ACTION and type(name) is str:
                self.comments[name] = node
                self.keys[node.guid] = name
            elif (node.action_key or "").startswith(NODE_ACTION_PREFIX) and type(name) is str:
                self.nodes[name] = node
                self.keys[node.guid] = name

    def key_of(self, guid):
        return self.keys.get(guid)

    def inputs(self, key):
        return self._links(self.nodes[key])

    def root_input(self):
        if self.root is None:
            return None
        links = self._links(self.root)
        return links[0] if links else None

    def _links(self, node):
        out = []
        for pin in _input_pins(node):
            out.append(self.keys.get(pin.linked_to[0].node_guid) if pin.linked_to else None)
        return out

    def path_of(self, guid):
        if self.root is not None and guid == self.root.guid:
            return "/Body/Root"
        key = self.keys.get(guid)
        if key in self.nodes:
            return f"/Body/Nodes/{escape(key)}"
        if key in self.comments:
            return f"/Body/Comments/{escape(key)}"
        return "/Body"


def position(node):
    return None if node.position is None else [int(round(node.position.x)), int(round(node.position.y))]


def thaw(value):
    if isinstance(value, MappingProxyType) or isinstance(value, dict):
        return {key: thaw(item) for key, item in value.items()}
    if type(value) in (list, tuple):
        return [thaw(item) for item in value]
    return value


# ---------------------------------------------------------------------------
# Class facts (defaults of a new node), cached for the session: constant for a class
# ---------------------------------------------------------------------------
_DEFAULTS = {}      # (class label, inputs) -> {properties}
_CLASSES = {}       # class label -> {"starting": n, "min": n, "max": n, "class_path": str}
_COMMENT_DEFAULTS = {}
_QUALITY_LEVELS = []


def ensure_defaults(calls, wanted, paths=None):
    """Reads the defaults of the (class label, inputs) pairs not cached yet.  ``paths`` maps a pair to the document
    path its class came from (an unknown class fails there)."""
    missing = []
    for item in wanted:
        if item not in _DEFAULTS and item not in missing:
            missing.append(item)
    if not missing and _COMMENT_DEFAULTS:
        return
    batches = [missing[start:start + MAX_DEFAULT_ROWS] for start in range(0, len(missing), MAX_DEFAULT_ROWS)] or [[]]
    for batch in batches:
        rows = [{"class": label} if inputs is None else {"class": label, "inputs": inputs} for label, inputs in batch]

        def where(primitive_path, batch=batch):
            parts = [part for part in (primitive_path or "").split("/") if part]
            if len(parts) >= 2 and parts[0] == "defaults" and parts[1].isdigit() and int(parts[1]) < len(batch):
                return (paths or {}).get(batch[int(parts[1])], "/Body/Nodes")
            return "/Body"
        value = calls.call(READ, {"defaults": rows}, paths=where)
        for (label, inputs), row in zip(batch, value.get("defaults", [])):
            _CLASSES[label] = {"starting": row.get("starting_inputs", 0), "min": row.get("min", 0),
                               "max": row.get("max", 0), "class_path": row.get("class_path", "")}
            properties = tidy(thaw(row.get("properties", {})))
            _DEFAULTS[(label, row.get("inputs", inputs))] = dict(properties)
            if inputs is None:
                _DEFAULTS[(label, None)] = dict(properties)
        if isinstance(value.get("comment_defaults"), dict):
            _COMMENT_DEFAULTS.clear()
            _COMMENT_DEFAULTS.update(tidy(thaw(value["comment_defaults"])))
        if isinstance(value.get("quality_levels"), list):
            _QUALITY_LEVELS[:] = value["quality_levels"]


def class_facts(label):
    return _CLASSES.get(label)


def node_defaults(label, inputs):
    return _DEFAULTS.get((label, inputs))


def comment_defaults():
    return dict(_COMMENT_DEFAULTS)


def quality_levels():
    return list(_QUALITY_LEVELS)


def starting_inputs(label):
    facts = _CLASSES.get(label)
    return None if facts is None else facts["starting"]


def desired_inputs(node):
    """The node's input list (Inputs, or the class's starting connectors with nothing connected); None when the
    starting count is not known."""
    if "Inputs" in node:
        return list(node["Inputs"])
    count = starting_inputs(node["Class"])
    return None if count is None else [None] * count


# ---------------------------------------------------------------------------
# Extract
# ---------------------------------------------------------------------------
def tidy(value):
    """Numbers stored as float32 (every float a sound node or comment box holds) as the shortest decimal that
    round-trips to the same float32: 0.6, not 0.6000000238418579. Other values unchanged."""
    if type(value) is float and math.isfinite(value) and abs(value) < 3.4e38:
        stored = common._float32(value)
        if stored == value:
            for digits in range(1, 10):
                candidate = float(f"{stored:.{digits}g}")
                if common._float32(candidate) == stored:
                    return candidate
        return value
    if type(value) is dict:
        return {key: tidy(item) for key, item in value.items()}
    if type(value) is list:
        return [tidy(item) for item in value]
    return value


def node_entry(live, key):
    node = live.nodes[key]
    entry = {"Class": node.extras.get("class", "")}
    where = position(node)
    if where is not None:
        entry["Position"] = where
    properties = tidy(thaw(node.extras.get("properties", {})))
    if properties:
        entry["Properties"] = properties
    limits = node.extras.get("child_limits", {})
    if limits.get("max", 0) > 0 or _input_pins(node):
        entry["Inputs"] = live.inputs(key)
    if node.comment:
        entry["Comment"] = node.comment
    if node.extras.get("comment_bubble_visible") is True:
        entry["CommentVisible"] = True
    return entry


def comment_entry(node):
    entry = {"Text": node.comment, "Position": position(node) or [0, 0]}
    size = node.extras.get("size")
    if type(size) in (list, tuple) and len(size) == 2:
        entry["Size"] = [int(size[0]), int(size[1])]
    properties = node.extras.get("properties", {})
    for field, name in COMMENT_PROPERTIES.items():
        if name in properties:
            entry[field] = tidy(thaw(properties[name]))
    return entry


def extract_body(live):
    root = {"Input": live.root_input()}
    if live.root is not None and position(live.root) is not None:
        root["Position"] = position(live.root)
    return {"Root": root, "Nodes": {key: node_entry(live, key) for key in live.nodes},
            "Comments": {key: comment_entry(node) for key, node in live.comments.items()}}


def wanted_defaults(body):
    """(class, inputs) pairs whose defaults Diff and Apply need for this Body."""
    wanted = []
    for node in body.get("Nodes", {}).values():
        if "Inputs" in node:
            wanted.append((node["Class"], len(node["Inputs"])))
        else:
            wanted.append((node["Class"], None))
    return wanted


def read_live(calls):
    """(live cue, Body, read result) of the cue; the class defaults of its nodes are cached on the way."""
    live = calls.live()
    body = extract_body(live)
    ensure_defaults(calls, wanted_defaults(body))
    return live, body


# ---------------------------------------------------------------------------
# Diff
# ---------------------------------------------------------------------------
_MISSING = object()


def _difference(changes, path, authored, live, authored_present=True, live_present=True):
    changes.append(Difference(path, DiffKind.REPLACE, authored_present, authored if authored_present else None,
                              live_present, live if live_present else None))


def _same(left, right, exact):
    return common.same(left, right, exact)


def _properties_diff(changes, path, authored, live, defaults, exact):
    """Properties sparse over ``defaults`` (an omitted property is its default); a struct value is an edit of the
    current one (graph.node.property.set.v1 decodes only the members it lists), so it compares with the live value in
    its shape."""
    for name in list(authored) + [name for name in live if name not in authored]:
        default = defaults.get(name, _MISSING) if defaults is not None else _MISSING
        left = authored.get(name, default)
        right = live.get(name, default)
        if name in authored and right is not _MISSING:
            right = common.struct_as_authored(left, right)
        if left is _MISSING or right is _MISSING or not _same(left, right, exact):
            _difference(changes, f"{path}/{escape(name)}", None if left is _MISSING else left,
                        None if right is _MISSING else right, left is not _MISSING, right is not _MISSING)


def node_diff(changes, path, authored, live, exact):
    if authored["Class"] != live.get("Class"):
        _difference(changes, f"{path}/Class", authored["Class"], live.get("Class"))
        return
    wanted = desired_inputs(authored)
    current = live.get("Inputs", desired_inputs({"Class": live["Class"]}) or [])
    if wanted is None or wanted != current:
        _difference(changes, f"{path}/Inputs", wanted, current)
    count = len(wanted) if wanted is not None else len(current)
    defaults = node_defaults(authored["Class"], count)
    _properties_diff(changes, f"{path}/Properties", authored.get("Properties", {}), live.get("Properties", {}),
                     defaults, exact)
    if "Position" in authored and authored["Position"] != live.get("Position"):
        _difference(changes, f"{path}/Position", authored["Position"], live.get("Position"))
    if authored.get("Comment", "") != live.get("Comment", ""):
        _difference(changes, f"{path}/Comment", authored.get("Comment", ""), live.get("Comment", ""))
    if authored.get("CommentVisible", False) != live.get("CommentVisible", False):
        _difference(changes, f"{path}/CommentVisible", authored.get("CommentVisible", False),
                    live.get("CommentVisible", False))


def comment_diff(changes, path, authored, live, exact):
    if authored["Text"] != live.get("Text"):
        _difference(changes, f"{path}/Text", authored["Text"], live.get("Text"))
    for field in ("Position", "Size"):
        if field in authored and authored[field] != live.get(field):
            _difference(changes, f"{path}/{field}", authored[field], live.get(field))
    defaults = {field: _COMMENT_DEFAULTS[name] for field, name in COMMENT_PROPERTIES.items() if name in _COMMENT_DEFAULTS} \
        if _COMMENT_DEFAULTS else None
    _properties_diff(changes, path, {field: authored[field] for field in COMMENT_PROPERTIES if field in authored},
                     {field: live[field] for field in COMMENT_PROPERTIES if field in live}, defaults, exact)


def body_diff(authored, live, exact):
    """Does the cue (its extracted Body) hold what the document Body authors?"""
    changes = []
    if "Root" in authored:
        if authored["Root"]["Input"] != live.get("Root", {}).get("Input"):
            _difference(changes, "/Body/Root/Input", authored["Root"]["Input"], live.get("Root", {}).get("Input"))
        if "Position" in authored["Root"] and authored["Root"]["Position"] != live.get("Root", {}).get("Position"):
            _difference(changes, "/Body/Root/Position", authored["Root"]["Position"],
                        live.get("Root", {}).get("Position"))
    for region, compare in (("Nodes", node_diff), ("Comments", comment_diff)):
        if region not in authored:
            continue
        wanted = authored[region]
        current = live.get(region, {})
        for key, entry in wanted.items():
            path = f"/Body/{region}/{escape(key)}"
            if key not in current:
                changes.append(Difference(path, DiffKind.ADD, True, entry, False, None))
            else:
                compare(changes, path, entry, current[key], exact)
        for key in current:
            if key not in wanted:
                changes.append(Difference(f"/Body/{region}/{escape(key)}", DiffKind.REMOVE, True, None, True,
                                          current[key]))
    return changes


# ---------------------------------------------------------------------------
# Validation messages (audio.soundcue.read.v1 / graph.finalize.v1)
# ---------------------------------------------------------------------------
def message_diagnostics(messages, code_by_severity, path_of):
    out = []
    for message in messages:
        severity = str(message.get("severity", "")).casefold()
        code = code_by_severity.get(severity)
        if code is None:
            continue
        level = {"error": DiagnosticSeverity.WARNING, "warning": DiagnosticSeverity.WARNING}.get(
            severity, DiagnosticSeverity.INFO)
        out.append(RuntimeDiagnostic(path=path_of(message), code=code, message=str(message.get("message", "")),
                                     severity=level))
    return out


__all__ = ("AssetDocumentContractError", "BODY_KEYS", "BY_ID", "CLASS", "Calls", "LiveCue", "PREFIX", "PROFILE_ID",
           "READ", "REQUIREMENTS", "ROUTE", "LIFECYCLE", "body_diff", "check_body", "check_references", "class_label",
           "ensure_defaults", "extract_body", "read_live")
