# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Registered StateTree Profile (UStateTree, profile.state_tree.v1).

Document (docs/architecture/asset-document-statetree-matrix.md)::

    "Body": {
      "Schema": {"Class": "/Script/GameplayStateTreeModule.StateTreeComponentSchema",
                 "Properties": {"ContextActorClass": "/Script/Engine.Pawn"}},
      "Parameters": [{"Name": "WaitTime", "Type": "float", "Value": 2.0}],
      "GlobalTasksCompletion": "Any",
      "Evaluators": [{"Type": "/Script/MyGame.MyEvaluator", "Name": "Sensor"}],
      "GlobalTasks": [],
      "States": [
        {"Name": "Root", "Children": [
          {"Name": "Idle",
           "Tasks": [{"Type": "/Script/StateTreeModule.StateTreeDelayTask", "Name": "Wait",
                      "Instance": {"RandomDeviation": 0.5},
                      "Bindings": {"Duration": "Parameters:WaitTime"}}],
           "Transitions": [{"Trigger": "OnStateCompleted", "To": "Root/Walk"}]},
          {"Name": "Walk", "Selection": "TryEnterState",
           "Transitions": [{"Trigger": "OnEvent", "Event": {"Tag": "Event.Stop"}, "To": "Root/Idle",
                            "Priority": "High", "Delay": {"Duration": 0.5},
                            "Conditions": [{"Type": "/Script/StateTreeModule.StateTreeCompareIntCondition",
                                            "Instance": {"Right": 3}}]}]}]}]}

* Omitted Body keys, state fields, node fields and node values keep what the asset has; a present list
  (``Parameters``, ``States``, ``Children``, node lists, ``Transitions``) is the complete list.  A state
  without ``Children`` keeps its children.  Transitions are complete values (omitted fields take the
  editor defaults) except ``Conditions`` (omitted = keep).
* Identity: states by path (``Root/Idle``; names may not contain ``/`` or ``:``) or ``RenamedFrom`` (old
  name under the same parent, or an old full path to move a state); state GUIDs are kept.  Nodes by
  ``Name``, otherwise the k-th unnamed node of the same ``Type`` in the same list; transitions by
  position.  Matched nodes keep their instance data and bindings.
* Bindings (node ``Bindings``, linked / subtree state ``ParameterBindings``): ``{"<target property
  path>": "<source>:<property path>"}`` (``"Node:<path>"`` targets a node struct property; a source
  without ``:<path>`` binds the whole struct, e.g. ``"Context/Actor"``) or ``{"<target>": {"Function":
  "<property function struct>", "Instance": {...}, "Bindings": {...}}}``.  Sources: ``Parameters``,
  ``Context/<Name>``, ``Evaluators/<key>``, ``GlobalTasks/<key>``, ``<StatePath>/Parameters``,
  ``<StatePath>/EnterEvent``, ``<StatePath>/{Tasks|EnterConditions|Considerations}/<key>``,
  ``<StatePath>/Transitions/<i>/{Event|Conditions/<key>}`` (key = node Name or list index).
* Apply edits the editor data through the StateTree editor paths (ai.state_tree.edit.v1), then compiles as
  the editor's Compile button does; compile errors fail the apply (Core rolls the package back),
  warnings are reported with the success result.

Writes are covered by Core's package snapshot (existing target) or the factory rollback (created
target); preflight therefore returns ``SNAPSHOT_COVERED``.
"""
from __future__ import annotations

import re
import uuid

from asset_document.diagnostics import (
    AssetDocumentContractError, DiagnosticContext, DiagnosticSeverity, RuntimeDiagnostic, with_diagnostics,
)
from asset_document.diff.model import Difference, DiffKind
from asset_document.dto import PrimitiveRequirement
from asset_document.meta import (
    ComparisonPolicy, DefaultPolicy, DeletePolicy, IdentityPolicy, OmissionPolicy, OrderPolicy, ProfileMeta,
    RegionMeta, RegionOwnership,
)
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
from asset_document.profiles import data_table_common as common
from asset_document.profiles.ublueprint_members import type_body, type_dto
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument, _single_handle
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements

STATE_TREE_CLASS = "/Script/StateTreeModule.StateTree"
SCHEMA_BASE = "/Script/StateTreeModule.StateTreeSchema"
CLASS_CLASS = "/Script/CoreUObject.Class"
ROUTE = "state_tree_profile"
PROVIDER = "ai"
READ = "ai.state_tree.read.v1"
EDIT = "ai.state_tree.edit.v1"
COMPILE = "ai.state_tree.compile.v1"
PREFIX = "StateTree"
# The New State Tree dialog's schema class is the factory context (UStateTreeFactory::StateTreeSchemaClass).
LIFECYCLE = generic_factory_lifecycle(ROUTE, ("factory_context",))
REQUIREMENTS = (tuple(PrimitiveRequirement(item, 1, ROUTE) for item in (
    READ, EDIT, COMPILE, common.CLASS_RESOLVE, common.RESOLVE, common.REFERENCE_RESOLVE)) + lifecycle_requirements(LIFECYCLE))
BY_ID = {item.primitive_id: item for item in REQUIREMENTS}

BODY_KEYS = ("Schema", "Parameters", "GlobalTasksCompletion", "Evaluators", "GlobalTasks", "States")
SCHEMA_KEYS = ("Class", "Properties")
PARAMETER_KEYS = ("Name", "RenamedFrom", "Type", "Value")
STATE_FIELDS = ("Description", "Tag", "Type", "Selection", "TasksCompletion", "Enabled", "Weight", "TickRate",
                "CheckPrerequisites", "EnterEvent", "LinkedSubtree", "LinkedAsset")
STATE_LISTS = ("EnterConditions", "Tasks", "Considerations")
STATE_KEYS = (("Name", "RenamedFrom") + STATE_FIELDS
              + ("Parameters", "ParameterValues", "ParameterBindings") + STATE_LISTS + ("Transitions", "Children"))
NODE_KEYS = ("Type", "Name", "Enabled", "ConsideredForCompletion", "EvaluationMode", "Indent", "Operand", "Node",
             "Instance", "Bindings")
NODE_FLAGS = (("Enabled", "enabled"), ("ConsideredForCompletion", "considered"), ("EvaluationMode", "evaluation_mode"),
              ("Indent", "indent"), ("Operand", "operand"))
TRANSITION_KEYS = ("Trigger", "Event", "To", "Priority", "Delay", "Enabled", "Conditions")
EVENT_KEYS = ("Tag", "Payload", "Consume")
STATE_TYPES = ("State", "Group", "Linked", "LinkedAsset", "Subtree")
LINKED_TYPES = ("Linked", "LinkedAsset")
TARGET_KINDS = ("None", "Succeeded", "Failed", "NextState", "NextSelectableState")
COMPLETION_TRIGGERS = ("OnStateCompleted", "OnStateSucceeded", "OnStateFailed")
STATE_DEFAULTS = {"Description": "", "Tag": "", "Type": "State", "Selection": "TrySelectChildrenInOrder",
                  "TasksCompletion": "Any", "Enabled": True, "Weight": 1, "TickRate": None, "CheckPrerequisites": True,
                  "EnterEvent": None, "LinkedSubtree": "", "LinkedAsset": ""}
# A listed state's omitted field is a new state's, except its kind: Type and its link (kept when omitted, like
# its lists; Diff notes it).
STATE_KEPT = ("Type", "LinkedSubtree", "LinkedAsset")


def state_default(key, state_type):
    """A new state's value of a state field; a linked state always selects TryEnterState (the editor resets it)."""
    if key == "Selection" and state_type in LINKED_TYPES:
        return "TryEnterState"
    return STATE_DEFAULTS[key]


NODE_DEFAULTS = {"Enabled": True, "ConsideredForCompletion": True, "EvaluationMode": "Evaluated", "Indent": 0,
                 "Operand": "And"}
EVENT_DEFAULT = {"Tag": "", "Payload": "", "Consume": True}
# Document node list -> (primitive list, owner kind).
TREE_LISTS = (("Evaluators", "evaluators"), ("GlobalTasks", "global_tasks"))
STATE_LIST_NAMES = {"EnterConditions": "enter_conditions", "Tasks": "tasks", "Considerations": "considerations"}
# list_types bases for each node list (struct nodes, Blueprint nodes).
NODE_BASES = {
    "Evaluators": ("/Script/StateTreeModule.StateTreeEvaluatorBase", "/Script/StateTreeModule.StateTreeEvaluatorBlueprintBase"),
    "GlobalTasks": ("/Script/StateTreeModule.StateTreeTaskBase", "/Script/StateTreeModule.StateTreeTaskBlueprintBase"),
    "Tasks": ("/Script/StateTreeModule.StateTreeTaskBase", "/Script/StateTreeModule.StateTreeTaskBlueprintBase"),
    "EnterConditions": ("/Script/StateTreeModule.StateTreeConditionBase", "/Script/StateTreeModule.StateTreeConditionBlueprintBase"),
    "Conditions": ("/Script/StateTreeModule.StateTreeConditionBase", "/Script/StateTreeModule.StateTreeConditionBlueprintBase"),
    "Considerations": ("/Script/StateTreeModule.StateTreeConsiderationBase",
                       "/Script/StateTreeModule.StateTreeConsiderationBlueprintBase"),
    "Functions": ("/Script/StateTreeModule.StateTreePropertyFunctionBase", ""),
}
ZERO_ID = "0" * 32
_GUID = re.compile(r"[0-9A-Fa-f]{32}\Z")


def _error(path, message, code):
    return common.error(path, message, f"{PREFIX}.{code}")


def new_id():
    return uuid.uuid4().hex.upper()


def _id(value):
    return "" if value in (None, "", ZERO_ID) else value


def _escape(key):
    return common.escape(key)


# ---------------------------------------------------------------------------
# Document shape
# ---------------------------------------------------------------------------
def _object(value, path, what, keys=None):
    if type(value) is not dict:
        raise _error(path, f"{what} must be an object.", "InvalidDocument")
    if keys is not None:
        for key in value:
            if key not in keys:
                raise _error(f"{path}/{_escape(key)}", f"Unknown {what} key '{key}'. Keys: {', '.join(keys)}.",
                             "UnknownKey")
    return value


def _list(value, path, what):
    if type(value) is not list:
        raise _error(path, f"{what} must be a list.", "InvalidDocument")
    return value


def _string(value, path, what, empty=True):
    if type(value) is not str or (not empty and not value):
        raise _error(path, f"{what} must be a{'' if empty else ' non-empty'} string.", "InvalidDocument")
    return value


def _bool(value, path, what):
    if type(value) is not bool:
        raise _error(path, f"{what} must be true or false.", "InvalidDocument")
    return value


def _number(value, path, what):
    if type(value) not in (int, float):
        raise _error(path, f"{what} must be a number.", "InvalidDocument")
    return value


def _state_name(value, path):
    _string(value, path, "A state Name", empty=False)
    if "/" in value or ":" in value or value.strip() != value or len(value) >= 1024:
        raise _error(path, f"State name '{value}' may not contain '/' or ':' or surrounding spaces (state paths "
                     "join names with '/').", "InvalidStateName")
    return value


def _validate_event(value, path):
    _object(value, path, "An event", EVENT_KEYS)
    _string(value.get("Tag", ""), f"{path}/Tag", "Tag")
    _string(value.get("Payload", ""), f"{path}/Payload", "Payload")
    if "Consume" in value:
        _bool(value["Consume"], f"{path}/Consume", "Consume")


def _validate_bindings(value, path):
    _object(value, path, "Bindings")
    for target, source in value.items():
        where = f"{path}/{_escape(target)}"
        if not target or target == "Node:":
            raise _error(where, "A binding target is a property path of the node (\"Node:<path>\" for a node struct "
                         "property).", "InvalidBinding")
        if type(source) is str:
            if not source or source.startswith(":"):
                raise _error(where, "A binding source is \"<source>:<property path>\", e.g. \"Parameters:Speed\".",
                             "InvalidBinding")
            continue
        _object(source, where, "A property function binding", ("Function", "Instance", "Bindings"))
        _string(source.get("Function"), f"{where}/Function", "Function", empty=False)
        if "Instance" in source:
            _object(source["Instance"], f"{where}/Instance", "Instance")
        if "Bindings" in source:
            _validate_bindings(source["Bindings"], f"{where}/Bindings")


def _validate_node(value, path):
    _object(value, path, "A node", NODE_KEYS)
    _string(value.get("Type"), f"{path}/Type", "Type", empty=False)
    if "Name" in value:
        _string(value["Name"], f"{path}/Name", "Name")
    for key in ("Enabled", "ConsideredForCompletion"):
        if key in value:
            _bool(value[key], f"{path}/{key}", key)
    for key in ("EvaluationMode", "Operand"):
        if key in value:
            _string(value[key], f"{path}/{key}", key, empty=False)
    if "Indent" in value and (type(value["Indent"]) is not int or not 0 <= value["Indent"] <= 4):
        raise _error(f"{path}/Indent", "Indent is an integer 0..4 (expression indent of a condition / consideration).",
                     "InvalidValue")
    for key in ("Node", "Instance"):
        if key in value:
            _object(value[key], f"{path}/{key}", key)
    if "Bindings" in value:
        _validate_bindings(value["Bindings"], f"{path}/Bindings")


def _validate_nodes(value, path):
    names = set()
    for index, node in enumerate(_list(value, path, "A node list")):
        _validate_node(node, f"{path}/{index}")
        name = node.get("Name", "")
        if name:
            if name in names:
                raise _error(f"{path}/{index}/Name", f"Node name '{name}' appears twice in this list.", "DuplicateNodeName")
            names.add(name)


def _validate_parameters(value, path):
    names, sources = set(), set()
    for index, row in enumerate(_list(value, path, "Parameters")):
        where = f"{path}/{index}"
        _object(row, where, "A parameter", PARAMETER_KEYS)
        name = _string(row.get("Name"), f"{where}/Name", "Name", empty=False)
        if name in names:
            raise _error(f"{where}/Name", f"Parameter '{name}' appears twice.", "DuplicateParameter")
        names.add(name)
        if "Type" not in row:
            raise _error(f"{where}/Type", "Type is required, e.g. \"float\", \"int\", \"bool\" or "
                         "\"object:/Script/Engine.Actor\".", "InvalidParameter")
        type_dto(row["Type"], f"{where}/Type")
        if "RenamedFrom" in row:
            source = _string(row["RenamedFrom"], f"{where}/RenamedFrom", "RenamedFrom", empty=False)
            if source == name or source in sources:
                raise _error(f"{where}/RenamedFrom", "RenamedFrom names another parameter, once.", "InvalidRename")
            sources.add(source)


def _validate_transition(value, path):
    _object(value, path, "A transition", TRANSITION_KEYS)
    trigger = _string(value.get("Trigger"), f"{path}/Trigger", "Trigger", empty=False)
    _string(value.get("To"), f"{path}/To", "To", empty=False)
    if "Event" in value and value["Event"] is not None:
        _validate_event(value["Event"], f"{path}/Event")
    if "Priority" in value:
        _string(value["Priority"], f"{path}/Priority", "Priority", empty=False)
    if "Enabled" in value:
        _bool(value["Enabled"], f"{path}/Enabled", "Enabled")
    delay = value.get("Delay")
    if delay is not None:
        _object(delay, f"{path}/Delay", "Delay", ("Duration", "Variance"))
        _number(delay.get("Duration"), f"{path}/Delay/Duration", "Duration")
        if "Variance" in delay:
            _number(delay["Variance"], f"{path}/Delay/Variance", "Variance")
        if trigger in COMPLETION_TRIGGERS:
            raise _error(f"{path}/Delay", "Completion transitions (OnStateCompleted / OnStateSucceeded / OnStateFailed) "
                         "cannot be delayed (the editor clears their delay).", "InvalidDelay")
    if "Conditions" in value:
        _validate_nodes(value["Conditions"], f"{path}/Conditions")


def _validate_state(value, path):
    _object(value, path, "A state", STATE_KEYS)
    _state_name(value.get("Name"), f"{path}/Name")
    if "RenamedFrom" in value:
        source = _string(value["RenamedFrom"], f"{path}/RenamedFrom", "RenamedFrom", empty=False)
        if source.strip("/") != source or "//" in source or ":" in source:
            raise _error(f"{path}/RenamedFrom", "RenamedFrom is the old state name or the old state path "
                         "(e.g. Root/Old).", "InvalidRename")
    for key in ("Description", "Tag", "LinkedSubtree", "LinkedAsset"):
        if key in value:
            _string(value[key], f"{path}/{key}", key)
    for key in ("Type", "Selection", "TasksCompletion"):
        if key in value:
            _string(value[key], f"{path}/{key}", key, empty=False)
    if "Type" in value and value["Type"] not in STATE_TYPES:
        raise _error(f"{path}/Type", f"Type is one of {', '.join(STATE_TYPES)}.", "InvalidValue")
    for key in ("Enabled", "CheckPrerequisites"):
        if key in value:
            _bool(value[key], f"{path}/{key}", key)
    if "Weight" in value:
        _number(value["Weight"], f"{path}/Weight", "Weight")
    if "TickRate" in value and value["TickRate"] is not None:
        _number(value["TickRate"], f"{path}/TickRate", "TickRate")
    if "EnterEvent" in value and value["EnterEvent"] is not None:
        _validate_event(value["EnterEvent"], f"{path}/EnterEvent")
    state_type = value.get("Type")
    linked = state_type in LINKED_TYPES
    if "Parameters" in value:
        if linked:
            raise _error(f"{path}/Parameters", "A linked state's parameters take the linked subtree's layout; author "
                         "ParameterValues instead.", "ParameterLayoutFixed")
        _validate_parameters(value["Parameters"], f"{path}/Parameters")
    if "ParameterValues" in value:
        if state_type is not None and not linked:
            raise _error(f"{path}/ParameterValues", "ParameterValues override the parameters of a Linked / LinkedAsset "
                         "state; other states define Parameters.", "InvalidValue")
        _object(value["ParameterValues"], f"{path}/ParameterValues", "ParameterValues")
    if "ParameterBindings" in value:
        _validate_bindings(value["ParameterBindings"], f"{path}/ParameterBindings")
    if "LinkedSubtree" in value and value["LinkedSubtree"] and state_type not in (None, "Linked"):
        raise _error(f"{path}/LinkedSubtree", "LinkedSubtree is set on a state of Type Linked.", "InvalidValue")
    if "LinkedAsset" in value and value["LinkedAsset"] and state_type not in (None, "LinkedAsset"):
        raise _error(f"{path}/LinkedAsset", "LinkedAsset is set on a state of Type LinkedAsset.", "InvalidValue")
    if linked and value.get("Selection", "TryEnterState") != "TryEnterState":
        raise _error(f"{path}/Selection", "Linked states always use TryEnterState (the editor resets it).", "InvalidValue")
    for key in STATE_LISTS:
        if key in value:
            _validate_nodes(value[key], f"{path}/{key}")
    if value.get("Tasks") and state_type in ("Group",) + LINKED_TYPES:
        raise _error(f"{path}/Tasks", f"A {state_type} state has no tasks (the editor removes them).", "InvalidValue")
    for index, transition in enumerate(_list(value.get("Transitions", []), f"{path}/Transitions", "Transitions")):
        _validate_transition(transition, f"{path}/Transitions/{index}")
    names = set()
    for index, child in enumerate(_list(value.get("Children", []), f"{path}/Children", "Children")):
        _validate_state(child, f"{path}/Children/{index}")
        if child["Name"] in names:
            raise _error(f"{path}/Children/{index}/Name", f"Two children are named '{child['Name']}' (state paths must "
                         "be unique).", "DuplicateStateName")
        names.add(child["Name"])


def validate_body(body):
    if "Schema" in body:
        schema = _object(body["Schema"], "/Body/Schema", "Schema", SCHEMA_KEYS)
        if "Class" in schema:
            _string(schema["Class"], "/Body/Schema/Class", "Class", empty=False)
        if "Properties" in schema:
            _object(schema["Properties"], "/Body/Schema/Properties", "Properties")
    if "Parameters" in body:
        _validate_parameters(body["Parameters"], "/Body/Parameters")
    if "GlobalTasksCompletion" in body:
        _string(body["GlobalTasksCompletion"], "/Body/GlobalTasksCompletion", "GlobalTasksCompletion", empty=False)
    for key, _ in TREE_LISTS:
        if key in body:
            _validate_nodes(body[key], f"/Body/{key}")
    if "States" in body:
        names = set()
        for index, state in enumerate(_list(body["States"], "/Body/States", "States")):
            _validate_state(state, f"/Body/States/{index}")
            if state["Name"] in names:
                raise _error(f"/Body/States/{index}/Name", f"Two subtrees are named '{state['Name']}'.",
                             "DuplicateStateName")
            names.add(state["Name"])


def canonicalize(document):
    body = common.validate_header(document, (STATE_TREE_CLASS,), PREFIX, BODY_KEYS)
    if document.get("Properties", {}):
        raise _error("/Properties", "A StateTree has no Details properties outside its editor data; author the Body "
                     "(Schema.Properties holds the schema's settings).", "UnsupportedProperties")
    validate_body(body)
    return common.snapshot(document)


# ---------------------------------------------------------------------------
# Live state (ai.state_tree.read.v1 records)
# ---------------------------------------------------------------------------
class Live:
    """The editor data of one tree, from the paged read records."""

    def __init__(self, records):
        self.tree = {}
        self.root_params = {"id": "", "props": [], "overrides": []}
        self.states = {}
        self.roots = []
        self.state_params = {}
        self.nodes = {}
        self.bindings = []
        for record in records:
            kind = record.get("kind")
            if kind == "tree":
                self.tree = record
            elif kind == "parameters":
                if record["owner"]:
                    self.state_params[record["owner"]] = record
                else:
                    self.root_params = record
            elif kind == "state":
                self.states[record["id"]] = dict(record, children=[])
                parent = record["parent"]
                if parent:
                    self.states[parent]["children"].append(record["id"])
                else:
                    self.roots.append(record["id"])
            elif kind == "node":
                self.nodes.setdefault((record["list"], record["state"], record["transition"]), []).append(record)
            elif kind == "binding":
                self.bindings.append(record)
        self.paths = {}
        self.by_path = {}

        def walk(ids, prefix):
            for state_id in ids:
                path = f"{prefix}{self.states[state_id]['name']}"
                self.paths[state_id] = path
                self.by_path.setdefault(path, state_id)
                walk(self.states[state_id]["children"], path + "/")
        walk(self.roots, "")

    @property
    def allows(self):
        return self.tree.get("allows", {})

    def task_list(self):
        return "tasks" if self.allows.get("multiple_tasks", True) else "single_task"

    def node_list(self, list_name, state="", transition=""):
        return self.nodes.get((list_name, state, transition), [])


class Calls:
    """ai.state_tree.* calls against one tree (editor handles handed off to the AI provider)."""

    def __init__(self, context, read, mutable=None):
        self.context = context
        self.read = context.handoff(source_handle=read, target_provider_id=PROVIDER, access="read_only")
        self.write = (context.handoff(source_handle=mutable, target_provider_id=PROVIDER, access="mutable")
                      if mutable is not None else None)

    def live(self):
        records, offset = [], 0
        while True:
            outcome = common.invoke(self.context, BY_ID, READ, {"schema_version": 1, "offset": offset},
                                    (NamedOpaqueHandleArgument("asset", self.read),), "/Body", prefix=PREFIX)
            page = outcome.value
            if type(page) is not dict:
                raise _error("/PrimitiveOutcome", "ai.state_tree.read.v1 returned no object.", "InvalidPrimitiveResult")
            records += page.get("records", [])
            offset = page.get("next_offset", -1)
            if type(offset) is not int or offset < 0:
                return Live(records)

    def edit(self, ops):
        """ops: [(op, PathMap)] -> changed records."""
        changed = []
        for chunk in common.chunk_ops(ops):
            maps = [where for _, where in chunk]
            outcome = common.invoke(self.context, BY_ID, EDIT, {"schema_version": 1, "ops": [op for op, _ in chunk]},
                                    (NamedOpaqueHandleArgument("asset", self.write),),
                                    paths=lambda primitive_path, maps=maps: map_op_path(primitive_path, maps),
                                    prefix=PREFIX)
            changed.extend(outcome.changed_set)
        return changed

    def compile(self):
        outcome = common.invoke(self.context, BY_ID, COMPILE, {"schema_version": 1},
                                (NamedOpaqueHandleArgument("asset", self.write),), "/Body", prefix=PREFIX)
        return outcome.value, tuple(outcome.changed_set)


class PathMap:
    """Primitive op sub-path -> document path (longest matching prefix wins; the rest is appended)."""

    def __init__(self, base, entries=None):
        self.base = base
        self.entries = dict(entries or {})

    def add(self, prefix, path):
        self.entries[prefix] = path
        return self

    def resolve(self, parts):
        for size in range(len(parts), 0, -1):
            key = "/".join(parts[:size])
            if key in self.entries:
                return self.entries[key] + "".join(f"/{part}" for part in parts[size:])
        return self.base


def map_op_path(primitive_path, maps):
    parts = [part for part in (primitive_path or "").split("/") if part]
    if len(parts) >= 2 and parts[0] == "ops" and parts[1].isdigit() and int(parts[1]) < len(maps):
        return maps[int(parts[1])].resolve(parts[2:])
    return "/Body"


# ---------------------------------------------------------------------------
# Value comparison
# ---------------------------------------------------------------------------
def _same(left, right, exact=False):
    return common.same(left, right, exact)


def _type_key(dto):
    inner = dto.get("value_type") or {}
    return (dto.get("category", ""), dto.get("sub_category", ""), dto.get("sub_category_object", ""),
            dto.get("container", "none") or "none",
            tuple(sorted((key, value) for key, value in inner.items() if value)))


def _type_matches(authored, live_path):
    """A node Type as written (object path, or struct / class name with or without the F prefix) vs the live path."""
    if not live_path:
        return False
    if "/" in authored:
        return authored == live_path
    name = live_path.rsplit(".", 1)[-1]
    return authored in (name, f"F{name}")


def _event_value(record):
    return {"Tag": record.get("tag", ""), "Payload": record.get("payload", ""), "Consume": record.get("consume", True)}


def _event_dto(value):
    value = {**EVENT_DEFAULT, **(value or {})}
    return {"tag": value["Tag"], "payload": value["Payload"], "consume": value["Consume"]}


def _event_same(authored, live):
    authored = {**EVENT_DEFAULT, **(authored or {})}
    live = {**EVENT_DEFAULT, **(live or {})}
    return authored == live


# ---------------------------------------------------------------------------
# Planning
# ---------------------------------------------------------------------------
class StateEntry:
    def __init__(self, state_id, name, doc=None, doc_path=None, live=None, parent=None):
        self.id = state_id
        self.name = name
        self.doc = doc
        self.doc_path = doc_path
        self.live = live
        self.parent = parent
        self.children = []
        self.path = ""


class Planner:
    """Desired document Body + live editor data -> ai.state_tree.edit.v1 ops (with document paths)."""

    def __init__(self, body, live, exact=False):
        self.body = body
        self.live = live
        self.exact = exact
        self.entries = {}          # state id -> StateEntry (desired hierarchy, documented and kept states)
        self.paths = {}            # desired state path -> id
        self.roots = None          # desired subtrees (None: hierarchy kept)
        self.node_ids = {}         # (list, state id, transition id, index) -> node id (documented lists)
        self.doc_paths = {}        # id (state / node) -> document path
        self.transition_ids = {}   # (state id, index) -> transition id

    # -- hierarchy ------------------------------------------------------------
    def plan_hierarchy(self):
        live = self.live
        if "States" not in self.body:
            self._keep_all()
            return []
        claimed = {}

        def assign(states, parent, doc_base):
            entries = []
            parent_live_path = live.paths.get(parent.id) if parent is not None and parent.live is not None else (
                "" if parent is None else None)
            for index, state in enumerate(states):
                doc_path = f"{doc_base}/{index}"
                match = None
                source = state.get("RenamedFrom")
                if source is not None:
                    old_path = source if "/" in source else _join(parent_live_path, source)
                    match = live.by_path.get(old_path) if old_path is not None else None
                    if match in claimed:
                        match = None
                if match is None:
                    candidate = _join(parent_live_path, state["Name"])
                    match = live.by_path.get(candidate) if candidate is not None else None
                if match is not None and match in claimed:
                    raise _error(f"{doc_path}/RenamedFrom" if source is not None else f"{doc_path}/Name",
                                 f"State {live.paths[match]} is claimed twice (by {claimed[match]} and {doc_path}).",
                                 "DuplicateState")
                entry = StateEntry(match or new_id(), state["Name"], state, doc_path,
                                   live.states.get(match) if match else None, parent)
                if match:
                    claimed[match] = doc_path
                self.entries[entry.id] = entry
                self.doc_paths[entry.id] = doc_path
                entry.children = assign(state["Children"], entry, f"{doc_path}/Children") if "Children" in state else None
                entries.append(entry)
            return entries

        self.roots = assign(self.body["States"], None, "/Body/States")

        def keep(entry):
            if entry.children is None:
                entry.children = []
                if entry.live is not None:
                    for child in entry.live["children"]:
                        if child not in claimed:
                            entry.children.append(self._kept(child, entry, claimed))
            for child in entry.children:
                if child.doc is not None:
                    keep(child)
        for root in self.roots:
            keep(root)
        self._index_paths()
        desired = [_tree(entry) for entry in self.roots]
        current = [_live_tree(live, state_id) for state_id in live.roots]
        if _shape(desired) == _shape(current):
            return []
        return [({"op": "hierarchy", "subtrees": desired}, PathMap("/Body/States"))]

    def _kept(self, state_id, parent, claimed):
        record = self.live.states[state_id]
        entry = StateEntry(state_id, record["name"], live=record, parent=parent)
        self.entries[state_id] = entry
        entry.children = [self._kept(child, entry, claimed) for child in record["children"] if child not in claimed]
        return entry

    def _keep_all(self):
        self.roots = [self._kept(state_id, None, {}) for state_id in self.live.roots]
        self._index_paths()

    def _index_paths(self):
        def walk(entries, prefix):
            names = {}
            for entry in entries:
                entry.path = f"{prefix}{entry.name}"
                if entry.path in self.paths:
                    where = entry.doc_path or names.get(entry.name)
                    raise _error(f"{where}/Name" if where else "/Body/States",
                                 f"Two states have the path {entry.path} (a state kept from the asset has this name; "
                                 "rename one of them).", "DuplicateStateName")
                names[entry.name] = entry.doc_path
                self.paths[entry.path] = entry.id
                walk(entry.children, entry.path + "/")
        walk(self.roots, "")

    def documented(self):
        """Documented states in hierarchy order."""
        out = []

        def walk(entries):
            for entry in entries:
                if entry.doc is not None:
                    out.append(entry)
                walk(entry.children)
        walk(self.roots or [])
        return out

    def state_type(self, entry):
        if entry.doc is not None and "Type" in entry.doc:
            return entry.doc["Type"]
        return entry.live["type"] if entry.live is not None else "State"

    def resolve_state(self, path, where, code="StateNotFound"):
        state_id = self.paths.get(path)
        if state_id is None:
            known = ", ".join(sorted(self.paths)[:20])
            raise _error(where, f"There is no state '{path}'. States: {known or '(none)'}.", code)
        return state_id

    # -- tree ------------------------------------------------------------------
    def plan_schema(self):
        if "Schema" not in self.body:
            return []
        schema = self.body["Schema"]
        live_class = self.live.tree.get("schema_class", "")
        if "Class" in schema and schema["Class"] != live_class:
            raise _error("/Body/Schema/Class", f"The tree's schema is {live_class}; the editor offers no schema change on an "
                         "existing tree (create a new tree with the other schema).", "SchemaClassImmutable")
        values = self.live.tree.get("schema", {})
        unsupported = set(self.live.tree.get("schema_unsupported", []))
        changed = {}
        for key, value in schema.get("Properties", {}).items():
            if key in unsupported:
                raise _error(f"/Body/Schema/Properties/{_escape(key)}", f"Schema property '{key}' has a type the "
                             "document codec cannot write.", "UnsupportedProperty")
            if key not in values or not _same(value, values[key], self.exact):
                changed[key] = value
        if not changed:
            return []
        return [({"op": "schema", "values": changed}, PathMap("/Body/Schema", {"values": "/Body/Schema/Properties"}))]

    def plan_tree(self):
        value = self.body.get("GlobalTasksCompletion")
        if value is None or value == self.live.tree.get("global_tasks_completion"):
            return []
        return [({"op": "tree", "global_tasks_completion": value},
                 PathMap("/Body/GlobalTasksCompletion"))]

    # -- parameters ------------------------------------------------------------
    def parameters_op(self, rows, record, doc_path, state_id=""):
        live_props = record.get("props", []) if record else []
        by_name = {prop["name"]: prop for prop in live_props}
        props, where = [], PathMap(doc_path)
        claimed = set()
        for index, row in enumerate(rows):
            match = by_name.get(row["Name"])
            if match is None and "RenamedFrom" in row:
                match = by_name.get(row["RenamedFrom"])
            if match is not None and match["id"] in claimed:
                match = None
            prop = {"id": match["id"] if match else new_id(), "name": row["Name"], "type": type_dto(row["Type"], f"{doc_path}/{index}/Type")}
            claimed.add(prop["id"])
            changed = match is None or _type_key(prop["type"]) != _type_key(match["type"]) or match["name"] != row["Name"]
            if "Value" in row and (changed or not match.get("supported", True) or "value" not in match
                                   or not _same(row["Value"], match["value"], self.exact)):
                prop["value"] = row["Value"]
            props.append((prop, changed))
            where.add(f"props/{index}", f"{doc_path}/{index}").add(f"props/{index}/value", f"{doc_path}/{index}/Value") \
                 .add(f"props/{index}/type", f"{doc_path}/{index}/Type").add(f"props/{index}/name", f"{doc_path}/{index}/Name")
        layout_same = [prop["id"] for prop, _ in props] == [prop["id"] for prop in live_props] \
            and not any(changed for _, changed in props)
        if layout_same and not any("value" in prop for prop, _ in props):
            return []
        op = {"op": "parameters", "props": [prop for prop, _ in props]}
        if state_id:
            op["state"] = state_id
        return [(op, where)]

    def plan_parameters(self):
        ops = []
        if "Parameters" in self.body:
            if self.body["Parameters"] and not self.live.allows.get("global_parameters", True):
                raise _error("/Body/Parameters", f"The schema {self.live.tree.get('schema_class')} does not allow tree "
                             "parameters.", "ListNotAllowed")
            ops += self.parameters_op(self.body["Parameters"], self.live.root_params, "/Body/Parameters")
        for entry in self.documented():
            if "Parameters" in entry.doc:
                if self.state_type(entry) in LINKED_TYPES:
                    raise _error(f"{entry.doc_path}/Parameters", "A linked state's parameters take the linked subtree's "
                                 "layout; author ParameterValues instead.", "ParameterLayoutFixed")
                record = self.live.state_params.get(entry.id) if entry.live is not None else None
                ops += self.parameters_op(entry.doc["Parameters"], record, f"{entry.doc_path}/Parameters", entry.id)
        return ops

    def plan_parameter_values(self, live=None):
        """Linked state overrides; planned against the live data after the layout edits (live)."""
        live = live or self.live
        ops = []
        for entry in self.documented():
            if "ParameterValues" not in entry.doc:
                continue
            where = f"{entry.doc_path}/ParameterValues"
            if self.state_type(entry) not in LINKED_TYPES:
                raise _error(where, "ParameterValues override the parameters of a Linked / LinkedAsset state.", "InvalidValue")
            values = entry.doc["ParameterValues"]
            record = live.state_params.get(entry.id, {"props": [], "overrides": []})
            props = {prop["name"]: prop for prop in record.get("props", [])}
            overridden = {prop["name"] for prop in record.get("props", []) if prop["id"] in record.get("overrides", [])}
            same = set(values) == overridden and all(
                name in props and "value" in props[name] and _same(value, props[name]["value"], self.exact)
                for name, value in values.items())
            if not same:
                ops.append(({"op": "parameter_values", "state": entry.id, "values": dict(values)},
                            PathMap(where, {"values": where})))
        return ops

    # -- states ----------------------------------------------------------------
    def plan_states(self):
        ops = []
        for entry in self.documented():
            doc, record = entry.doc, entry.live
            fields = {}
            if record is not None and doc["Name"] != record["name"]:
                fields["name"] = doc["Name"]
            state_type = doc.get("Type", record["type"] if record is not None else "State")
            for key in STATE_FIELDS:
                if key not in doc and (record is None or key in STATE_KEPT):
                    continue
                # An existing state's field the document leaves out is a new state's (STATE_KEPT: kept).
                desired, current = self._state_field(entry, key, doc.get(key, state_default(key, state_type)))
                if record is None or not _same(desired, current, self.exact):
                    fields[_STATE_FIELD_DTO[key]] = desired
            if fields:
                where = PathMap(entry.doc_path, {dto: f"{entry.doc_path}/{key}" for key, dto in _STATE_FIELD_DTO.items()})
                where.add("name", f"{entry.doc_path}/Name")
                ops.append(({"op": "state", "id": entry.id, **fields}, where))
        return ops

    def _state_field(self, entry, key, value):
        record = entry.live or {}
        if key == "TickRate":
            return value, (record.get("tick_rate") if record.get("has_tick_rate") else None)
        if key == "EnterEvent":
            desired = None if value is None else _event_dto(value)
            current = None
            if record.get("has_enter_event"):
                current = {k: record["enter_event"][k] for k in ("tag", "payload", "consume")}
            return desired, current
        if key == "LinkedSubtree":
            desired = self.resolve_state(value, f"{entry.doc_path}/LinkedSubtree", "LinkedSubtreeNotFound") if value else ""
            return desired, _id(record.get("linked_subtree", ""))
        return value, record.get(_STATE_FIELD_DTO[key])

    # -- transitions -----------------------------------------------------------
    def transition_target(self, value, where):
        if value in TARGET_KINDS:
            return {"type": value}
        return {"type": "GotoState", "state": self.resolve_state(value, where, "TransitionTargetNotFound")}

    def plan_transitions(self):
        ops = []
        for entry in self.documented():
            live_transitions = entry.live["transitions"] if entry.live is not None else []
            if "Transitions" not in entry.doc:
                for index, transition in enumerate(live_transitions):
                    self.transition_ids[(entry.id, index)] = transition["id"]
                continue
            desired, differs = [], len(entry.doc["Transitions"]) != len(live_transitions)
            where = PathMap(f"{entry.doc_path}/Transitions")
            for index, transition in enumerate(entry.doc["Transitions"]):
                doc_path = f"{entry.doc_path}/Transitions/{index}"
                current = live_transitions[index] if index < len(live_transitions) else None
                dto = {"id": current["id"] if current else new_id(), "trigger": transition["Trigger"],
                       "event": _event_dto(transition.get("Event")),
                       "to": self.transition_target(transition["To"], f"{doc_path}/To"),
                       "priority": transition.get("Priority", "Normal"), "enabled": transition.get("Enabled", True)}
                delay = transition.get("Delay")
                dto["delay"] = None if delay is None else {"duration": delay["Duration"], "variance": delay.get("Variance", 0)}
                self.transition_ids[(entry.id, index)] = dto["id"]
                self.doc_paths[dto["id"]] = doc_path
                differs = differs or not self._transition_same(dto, current)
                desired.append(dto)
                for key, label in (("trigger", "Trigger"), ("event", "Event"), ("to", "To"), ("priority", "Priority"),
                                   ("delay", "Delay"), ("enabled", "Enabled")):
                    where.add(f"transitions/{index}/{key}", f"{doc_path}/{label}")
                where.add(f"transitions/{index}", doc_path)
            if differs:
                ops.append(({"op": "transitions", "state": entry.id, "transitions": desired}, where))
        return ops

    def _transition_same(self, dto, current):
        if current is None:
            return False
        to = current["to"]
        if dto["trigger"] != current["trigger"] or dto["priority"] != current["priority"] \
                or dto["enabled"] != current["enabled"] or dto["to"]["type"] != to["type"] \
                or (to["type"] == "GotoState" and dto["to"].get("state") != to["state"]):
            return False
        if not _event_same({"Tag": dto["event"]["tag"], "Payload": dto["event"]["payload"], "Consume": dto["event"]["consume"]},
                           _event_value(current["event"])):
            return False
        if (dto["delay"] is not None) != bool(current["delay"]):
            return False
        return dto["delay"] is None or (_same(dto["delay"]["duration"], current["delay_duration"], self.exact)
                                        and _same(dto["delay"]["variance"], current["delay_variance"], self.exact))

    # -- nodes -----------------------------------------------------------------
    def node_lists(self):
        """(document nodes, document path, primitive list, state id, transition id) of every authored list."""
        out = []
        for key, list_name in TREE_LISTS:
            if key in self.body:
                out.append((self.body[key], f"/Body/{key}", list_name, "", ""))
        for entry in self.documented():
            for key in STATE_LISTS:
                if key in entry.doc:
                    list_name = self.live.task_list() if key == "Tasks" else STATE_LIST_NAMES[key]
                    out.append((entry.doc[key], f"{entry.doc_path}/{key}", list_name, entry.id, ""))
            for index, transition in enumerate(entry.doc.get("Transitions", [])):
                if "Conditions" in transition:
                    out.append((transition["Conditions"], f"{entry.doc_path}/Transitions/{index}/Conditions",
                                "transition_conditions", entry.id, self.transition_ids[(entry.id, index)]))
        return out

    def plan_nodes(self):
        ops = []
        for nodes, doc_path, list_name, state_id, transition_id in self.node_lists():
            if list_name == "single_task" and len(nodes) > 1:
                raise _error(doc_path, f"The schema {self.live.tree.get('schema_class')} allows one task per state.",
                             "ListNotAllowed")
            current = self.live.node_list(list_name, state_id, transition_id)
            matches = match_nodes(nodes, current)
            desired, differs = [], [m["id"] if m else None for m in matches] != [n["id"] for n in current]
            where = PathMap(doc_path)
            for index, (node, match) in enumerate(zip(nodes, matches)):
                node_path = f"{doc_path}/{index}"
                dto, changed = self._node_dto(node, match)
                differs = differs or changed
                desired.append(dto)
                self.node_ids[(list_name, state_id, transition_id, index)] = dto["id"]
                self.doc_paths[dto["id"]] = node_path
                where.add(f"nodes/{index}", node_path)
                for key, label in (("type", "Type"), ("name", "Name"), ("node", "Node"), ("instance", "Instance"),
                                   ("enabled", "Enabled"), ("considered", "ConsideredForCompletion"),
                                   ("evaluation_mode", "EvaluationMode"), ("indent", "Indent"), ("operand", "Operand")):
                    where.add(f"nodes/{index}/{key}", f"{node_path}/{label}")
            if differs:
                op = {"op": "nodes", "list": list_name, "nodes": desired}
                if state_id:
                    op["state"] = state_id
                if transition_id:
                    op["transition"] = transition_id
                ops.append((op, where))
        return ops

    def _node_dto(self, node, match):
        dto = {"id": match["id"] if match else new_id(), "type": node["Type"]}
        changed = match is None
        record = match or {}
        if "Name" in node and (match is None or node["Name"] != record.get("name", "")):
            dto["name"] = node["Name"]
            changed = True
        for key, field in NODE_FLAGS:
            if key in node and (match is None or field not in record or not _same(node[key], record[field], True)):
                dto[field] = node[key]
                changed = True
            elif key not in node and match is not None and _resets_flag(record, key, field):
                # A flag the document leaves out is a new node's.
                dto[field] = NODE_DEFAULTS[key]
                changed = True
        for key, field in (("Node", "node"), ("Instance", "instance")):
            values = node.get(key, {})
            live_values = record.get(field, {})
            delta = {name: value for name, value in values.items()
                     if match is None or name not in live_values or not _same(value, live_values[name], self.exact)}
            # A value the document leaves out is the node type's default (record "<field>_reset": the default of
            # each value that differs from it).
            delta.update({name: value for name, value in (record.get(field + "_reset") or {}).items()
                          if name not in values})
            if delta:
                dto[field] = delta
                changed = True
        return dto, changed

    # -- bindings (planned against the live data after the structural edits) --
    def plan_bindings(self, live):
        refs = SourceRefs(self, live)
        ops = []
        for nodes, doc_path, list_name, state_id, transition_id in self.node_lists():
            for index, node in enumerate(nodes):
                if "Bindings" not in node:
                    continue
                node_id = self.node_ids[(list_name, state_id, transition_id, index)]
                record = next((item for item in live.node_list(list_name, state_id, transition_id)
                               if item["id"] == node_id), None)
                if record is None:
                    continue
                targets = (node_id, record["node_id"])
                ops += self._bindings_op(node["Bindings"], targets, refs, live, f"{doc_path}/{index}/Bindings")
        for entry in self.documented():
            if "ParameterBindings" not in entry.doc:
                continue
            record = live.state_params.get(entry.id)
            if record is None:
                continue
            ops += self._bindings_op(entry.doc["ParameterBindings"], (record["id"],), refs, live,
                                     f"{entry.doc_path}/ParameterBindings")
        return ops

    def _bindings_op(self, bindings, targets, refs, live, doc_path):
        desired = binding_dtos(bindings, targets, refs, doc_path)
        current = [binding for binding in live.bindings
                   if not binding.get("output") and binding["target"]["struct"] in targets]
        if bindings_same(desired, current, live, self.exact):
            return []
        where = PathMap(doc_path)
        for index, (target, _) in enumerate(bindings.items()):
            # A binding's source / target failures name the authored binding entry.
            entry_path = f"{doc_path}/{_escape(target)}"
            where.add(f"bindings/{index}", entry_path).add(f"bindings/{index}/source", entry_path) \
                 .add(f"bindings/{index}/target", entry_path)
        return [({"op": "bindings", "targets": list(targets), "bindings": [item for item, _ in desired]}, where)]


_STATE_FIELD_DTO = {"Description": "description", "Tag": "tag", "Type": "type", "Selection": "selection",
                    "TasksCompletion": "tasks_completion", "Enabled": "enabled", "Weight": "weight",
                    "TickRate": "tick_rate", "CheckPrerequisites": "check_prerequisites", "EnterEvent": "enter_event",
                    "LinkedSubtree": "linked_subtree", "LinkedAsset": "linked_asset"}


def _resets_flag(record, key, field):
    """An existing node's flag that a document leaving it out resets (the live value is not the default; a task's
    Considered For Completion only where the editor lets it be edited)."""
    if field not in record or _same(record[field], NODE_DEFAULTS[key], True):
        return False
    return key != "ConsideredForCompletion" or record.get("considered_editable", True)


def _join(parent_path, name):
    if parent_path is None:
        return None
    return f"{parent_path}/{name}" if parent_path else name


def _tree(entry):
    return {"id": entry.id, "name": entry.name, "children": [_tree(child) for child in entry.children]}


def _live_tree(live, state_id):
    record = live.states[state_id]
    return {"id": state_id, "name": record["name"], "children": [_live_tree(live, child) for child in record["children"]]}


def _shape(trees):
    return [(tree["id"], _shape(tree["children"])) for tree in trees]


def match_nodes(nodes, current):
    """Live node record (or None) for each document node: by Name, else the k-th unnamed node of the same Type."""
    used = set()
    out = []
    for node in nodes:
        match = None
        name = node.get("Name", "")
        if name:
            match = next((item for item in current if item.get("name") == name and item["id"] not in used), None)
        else:
            match = next((item for item in current if not item.get("name") and item["id"] not in used
                          and _type_matches(node["Type"], item.get("type", ""))), None)
        if match is not None and not _type_matches(node["Type"], match.get("type", "")):
            match = None  # Another type: a new node (the node picker replaces the node).
        if match is not None:
            used.add(match["id"])
        out.append(match)
    return out


# ---------------------------------------------------------------------------
# Binding sources
# ---------------------------------------------------------------------------
class SourceRefs:
    """Binding source references <-> bindable struct ids."""

    def __init__(self, planner, live):
        self.planner = planner
        self.live = live
        self.paths = planner.paths if planner is not None else {}
        self.ids = {}   # struct id -> reference
        self.ids[live.root_params.get("id", "")] = "Parameters"
        for item in live.tree.get("context", []):
            self.ids[item["id"]] = f"Context/{item['name']}"
        for key, list_name in TREE_LISTS:
            for index, record in enumerate(live.node_list(list_name)):
                self.ids[record["id"]] = f"{key}/{_node_key(live.node_list(list_name), index)}"
        for state_id, path in self._state_paths().items():
            record = live.states[state_id]
            params = live.state_params.get(state_id)
            if params is not None:
                self.ids[params["id"]] = f"{path}/Parameters"
            self.ids[record["enter_event_id"]] = f"{path}/EnterEvent"
            for key, list_name in (("EnterConditions", "enter_conditions"), ("Tasks", "tasks"), ("Tasks", "single_task"),
                                   ("Considerations", "considerations")):
                records = live.node_list(list_name, state_id)
                for index in range(len(records)):
                    self.ids[records[index]["id"]] = f"{path}/{key}/{_node_key(records, index)}"
            for index, transition in enumerate(record["transitions"]):
                self.ids[transition["event_id"]] = f"{path}/Transitions/{index}/Event"
                records = live.node_list("transition_conditions", state_id, transition["id"])
                for position in range(len(records)):
                    self.ids[records[position]["id"]] = (f"{path}/Transitions/{index}/Conditions/"
                                                         f"{_node_key(records, position)}")
        self.ids.pop("", None)

    def _state_paths(self):
        if self.planner is not None and self.planner.paths:
            return {state_id: path for path, state_id in self.planner.paths.items() if state_id in self.live.states}
        return dict(self.live.paths)

    def name(self, struct_id):
        return self.ids.get(struct_id, f"Id/{struct_id}")

    def resolve(self, ref, where):
        live = self.live
        if ref.startswith("Id/") and _GUID.fullmatch(ref[3:]):
            return ref[3:].upper()
        if ref == "Parameters":
            return live.root_params["id"]
        parts = ref.split("/")
        if parts[0] == "Context" and len(parts) == 2:
            match = next((item["id"] for item in live.tree.get("context", []) if item["name"] == parts[1]), None)
            if match is not None:
                return match
        for key, list_name in TREE_LISTS:
            if parts[0] == key and len(parts) == 2:
                found = _find_node(live.node_list(list_name), parts[1])
                if found is not None:
                    return found
        for size in range(len(parts) - 1, 0, -1):
            state_id = self.paths.get("/".join(parts[:size])) if self.paths else live.by_path.get("/".join(parts[:size]))
            if state_id is None or state_id not in live.states:
                continue
            found = self._state_source(state_id, parts[size:])
            if found is not None:
                return found
        raise _error(where, f"Binding source '{ref}' does not name a bindable struct. Sources: Parameters, "
                     "Context/<Name>, Evaluators/<key>, GlobalTasks/<key>, <StatePath>/Parameters, <StatePath>/EnterEvent, "
                     "<StatePath>/Tasks/<key>, <StatePath>/EnterConditions/<key>, <StatePath>/Considerations/<key>, "
                     "<StatePath>/Transitions/<i>/Event, <StatePath>/Transitions/<i>/Conditions/<key> (key = node Name "
                     "or index).", "BindingSourceNotFound")

    def _state_source(self, state_id, rest):
        live = self.live
        record = live.states[state_id]
        if rest == ["Parameters"]:
            params = live.state_params.get(state_id)
            return params["id"] if params else None
        if rest == ["EnterEvent"]:
            return record["enter_event_id"]
        if len(rest) == 2 and rest[0] in ("Tasks", "EnterConditions", "Considerations"):
            lists = {"Tasks": (live.task_list(),), "EnterConditions": ("enter_conditions",),
                     "Considerations": ("considerations",)}[rest[0]]
            for list_name in lists:
                found = _find_node(live.node_list(list_name, state_id), rest[1])
                if found is not None:
                    return found
            return None
        if len(rest) >= 3 and rest[0] == "Transitions" and rest[1].isdigit():
            index = int(rest[1])
            if index >= len(record["transitions"]):
                return None
            transition = record["transitions"][index]
            if rest[2:] == ["Event"]:
                return transition["event_id"]
            if len(rest) == 4 and rest[2] == "Conditions":
                return _find_node(live.node_list("transition_conditions", state_id, transition["id"]), rest[3])
        return None


def _node_key(records, index):
    name = records[index].get("name", "")
    if name and sum(1 for item in records if item.get("name") == name) == 1:
        return name
    return str(index)


def _find_node(records, key):
    named = [item for item in records if item.get("name") == key]
    if len(named) == 1:
        return named[0]["id"]
    if key.isdigit() and int(key) < len(records):
        return records[int(key)]["id"]
    return None


def _split_source(text):
    ref, sep, path = text.partition(":")
    return ref, path if sep else ""


def _target_dto(target, targets):
    """Document binding key -> {struct, path}: "Node:<path>" targets the node struct (targets[1])."""
    if target.startswith("Node:"):
        if len(targets) < 2:
            return None
        return {"struct": targets[1], "path": target[5:]}
    return {"struct": targets[0], "path": target}


def binding_dtos(bindings, targets, refs, doc_path):
    """[(binding dto, comparable key)] for one target node."""
    out = []
    for target, source in bindings.items():
        where = f"{doc_path}/{_escape(target)}"
        target_dto = _target_dto(target, targets)
        if target_dto is None:
            raise _error(where, "This target has no node struct (\"Node:\" targets are node struct properties).",
                         "InvalidBinding")
        if type(source) is str:
            ref, path = _split_source(source)
            out.append(({"target": target_dto, "source": {"struct": refs.resolve(ref, where), "path": path}}, None))
            continue
        function = {"type": source["Function"]}
        if "Instance" in source:
            function["instance"] = dict(source["Instance"])
        nested = []
        for nested_target, nested_source in source.get("Bindings", {}).items():
            nested_where = f"{where}/Bindings/{_escape(nested_target)}"
            if type(nested_source) is not str:
                raise _error(nested_where, "A property function input binds to a source (nested property functions are "
                             "not supported).", "InvalidBinding")
            ref, path = _split_source(nested_source)
            nested.append({"target": {"path": nested_target},
                           "source": {"struct": refs.resolve(ref, nested_where), "path": path}})
        if nested:
            function["bindings"] = nested
        out.append(({"target": target_dto, "function": function}, None))
    return out


def bindings_same(desired, current, live, exact=False):
    """Whether the live bindings of the targets already are the desired ones."""
    if len(desired) != len(current):
        return False
    by_target = {(item["target"]["struct"], item["target"]["path"]): item for item in current}
    for dto, _ in desired:
        live_binding = by_target.get((dto["target"]["struct"], dto["target"]["path"]))
        if live_binding is None:
            return False
        if "source" in dto:
            if "function" in live_binding or live_binding["source"] != dto["source"]:
                return False
            continue
        function = live_binding.get("function")
        if function is None or not _type_matches(dto["function"]["type"], function["type"]):
            return False
        instance = function.get("instance", {})
        if any(name not in instance or not _same(value, instance[name], exact)
               for name, value in dto["function"].get("instance", {}).items()):
            return False
        nested = [binding for binding in live.bindings
                  if not binding.get("output") and binding["target"]["struct"] == function["id"]]
        wanted = dto["function"].get("bindings", [])
        if len(nested) != len(wanted):
            return False
        nested_by_target = {item["target"]["path"]: item for item in nested}
        for item in wanted:
            match = nested_by_target.get(item["target"]["path"])
            if match is None or "function" in match or match["source"] != item["source"]:
                return False
    return True


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------
def plan_structure(planner):
    """Ordered structural ops: schema, tree, hierarchy, state fields, parameters, transitions, nodes."""
    ops = planner.plan_schema() + planner.plan_tree()
    ops += planner.plan_hierarchy()
    ops += planner.plan_states()
    ops += planner.plan_parameters()
    ops += planner.plan_transitions()
    ops += planner.plan_nodes()
    return ops


def _needs_second_pass(body):
    """ParameterValues / bindings are planned against the layout the structural edits produce."""
    def node_bindings(nodes):
        return any("Bindings" in node for node in nodes)

    def state(value):
        return ("ParameterValues" in value or "ParameterBindings" in value
                or any(node_bindings(value.get(key, [])) for key in STATE_LISTS)
                or any(node_bindings(item.get("Conditions", [])) for item in value.get("Transitions", []))
                or any(state(child) for child in value.get("Children", [])))
    return any(node_bindings(body.get(key, [])) for key, _ in TREE_LISTS) or any(state(item) for item in body.get("States", []))


def apply(document, read, mutable, context):
    body = document.get("Body", {})
    calls = Calls(context, read, mutable)
    live = calls.live()
    planner = Planner(body, live)
    ops = plan_structure(planner)
    changes = list(calls.edit(ops)) if ops else []
    edited = bool(ops)
    if _needs_second_pass(body):
        after = calls.live() if edited else live
        second = planner.plan_parameter_values(after) + planner.plan_bindings(after)
        if second:
            changes += calls.edit(second)
            edited = True
    if not edited:
        return ()
    result, compiled = calls.compile()
    changes += compiled
    return with_diagnostics(changes, compile_diagnostics(result, planner))


def compile_diagnostics(result, planner):
    """Compile errors fail the apply (Core rolls back); warnings go with the success result."""
    messages = result.get("messages", []) if type(result) is dict else []
    warnings = []
    for message in messages:
        where = planner.doc_paths.get(message.get("item_id", "")) or planner.doc_paths.get(message.get("state_id", "")) \
            or _state_doc_path(planner, message.get("state_path", "")) or "/Body"
        text = message.get("message", "")
        if message.get("state_path"):
            text = f"{message['state_path']}: {text}"
        if message.get("severity") == "Error":
            raise AssetDocumentContractError(RuntimeDiagnostic(
                path=where, code=f"{PREFIX}.CompileError",
                message=f"The StateTree does not compile: {text}",
                context=(DiagnosticContext("primitive", COMPILE),)))
        if message.get("severity") == "Warning":
            warnings.append(RuntimeDiagnostic(path=where, code=f"{PREFIX}.CompileWarning", message=text,
                                              severity=DiagnosticSeverity.WARNING))
    if type(result) is dict and result.get("succeeded") is False:
        raise _error("/Body", "The StateTree does not compile (the compiler reported no message).", "CompileError")
    return warnings


def _state_doc_path(planner, state_path):
    state_id = planner.paths.get(state_path)
    return planner.doc_paths.get(state_id) if state_id else None


def create_target_context(document, context):
    schema = document.get("Body", {}).get("Schema", {})
    if "Class" not in schema:
        raise _error("/Body/Schema/Class", "Creating a StateTree requires Body.Schema.Class (the schema the New State Tree "
                     "dialog asks for), e.g. /Script/GameplayStateTreeModule.StateTreeComponentSchema. list_types (kind "
                     f"class, base {SCHEMA_BASE}) lists the schemas.", "SchemaRequired")
    class_path = schema["Class"]
    try:
        common.invoke(context, BY_ID, common.CLASS_RESOLVE, {"class": class_path, "expected_base": SCHEMA_BASE},
                      path="/Body/Schema/Class", prefix=PREFIX)
        outcome = common.invoke(context, BY_ID, common.REFERENCE_RESOLVE, {"expected_class": CLASS_CLASS, "path": class_path},
                                path="/Body/Schema/Class", prefix=PREFIX)
    except AssetDocumentContractError as failure:
        raise _error("/Body/Schema/Class", f"Schema class '{class_path}' is not a loadable StateTree schema: "
                     f"{failure.diagnostic.message} list_types (kind class, base {SCHEMA_BASE}) lists the schemas.",
                     "SchemaNotFound") from None
    return (NamedOpaqueHandleArgument("factory_context", _single_handle(outcome, "asset")),)


# ---------------------------------------------------------------------------
# Extract
# ---------------------------------------------------------------------------
def body_from_live(live, include_defaults=False):
    refs = SourceRefs(None, live)
    bindings = _bindings_by_target(live, refs)
    tree = live.tree
    body = {"Schema": {"Class": tree.get("schema_class", ""),
                       "Properties": _without_defaults(tree.get("schema", {}), tree.get("schema_defaults"), include_defaults)}}
    body["Parameters"] = _parameters_doc(live.root_params)
    body["GlobalTasksCompletion"] = tree.get("global_tasks_completion", "Any")
    for key, list_name in TREE_LISTS:
        nodes = [_node_doc(record, bindings, include_defaults) for record in live.node_list(list_name)]
        if nodes or include_defaults:
            body[key] = nodes
    body["States"] = [_state_doc(live, state_id, refs, bindings, include_defaults) for state_id in live.roots]
    return body


def _parameters_doc(record):
    rows = []
    for prop in record.get("props", []):
        row = {"Name": prop["name"], "Type": type_body(prop["type"])}
        if prop.get("supported") and "value" in prop:
            row["Value"] = prop["value"]
        rows.append(row)
    return rows


def _without_defaults(values, defaults, include_defaults):
    """diff_only extract: drop values the read reports equal to the struct / class default."""
    if include_defaults or not defaults:
        return dict(values)
    return {name: value for name, value in values.items() if name not in set(defaults)}


# Within a node_facts block, _node_doc stores each node's comparison facts under this key (popped by node_facts).
_FACTS = "_NodeFacts"
_COLLECT_FACTS = []


def _node_facts_of(record):
    """What Diff needs to compare a node the document lists with omissions: the values not at their default (name ->
    default), and whether an omitted Considered For Completion is reset."""
    return {"Node": dict(record.get("node_reset") or {}), "Instance": dict(record.get("instance_reset") or {}),
            "ConsideredEditable": record.get("considered_editable", True)}


def _node_doc(record, bindings, include_defaults):
    node = {"Type": record["type"]}
    if _COLLECT_FACTS:
        node[_FACTS] = _node_facts_of(record)
    if record.get("name"):
        node["Name"] = record["name"]
    for key, field in NODE_FLAGS:
        if field in record and (include_defaults or record[field] != NODE_DEFAULTS[key]):
            if key == "ConsideredForCompletion" and not record.get("considered_editable", True) and not include_defaults:
                continue
            node[key] = record[field]
    for key, field in (("Node", "node"), ("Instance", "instance")):
        values = _without_defaults(record.get(field) or {}, record.get(field + "_defaults"), include_defaults)
        if values:
            node[key] = values
    authored = {**bindings.get(record["id"], {}),
                **{f"Node:{path}": value for path, value in bindings.get(record["node_id"], {}).items()}}
    if authored:
        node["Bindings"] = authored
    return node


def _bindings_by_target(live, refs):
    """struct id -> {target path: document source} of the authored (non-output) bindings."""
    out = {}
    nested_of = {}
    for binding in live.bindings:
        if binding.get("output"):
            continue
        nested_of.setdefault(binding["target"]["struct"], []).append(binding)
    for binding in live.bindings:
        if binding.get("output"):
            continue
        function = binding.get("function")
        if function is not None:
            value = {"Function": function["type"]}
            if function.get("instance"):
                value["Instance"] = dict(function["instance"])
            nested = {item["target"]["path"]: _source_text(item, refs) for item in nested_of.get(function["id"], [])
                      if "function" not in item}
            if nested:
                value["Bindings"] = nested
        else:
            value = _source_text(binding, refs)
        out.setdefault(binding["target"]["struct"], {})[binding["target"]["path"]] = value
    return out


def _source_text(binding, refs):
    source = binding["source"]
    ref = refs.name(source["struct"])
    return f"{ref}:{source['path']}" if source["path"] else ref


def _state_doc(live, state_id, refs, bindings, include_defaults):
    record = live.states[state_id]
    state = {"Name": record["name"]}
    values = {"Description": record["description"], "Tag": record["tag"], "Type": record["type"],
              "Selection": record["selection"], "TasksCompletion": record["tasks_completion"],
              "Enabled": record["enabled"], "Weight": record["weight"],
              "TickRate": record["tick_rate"] if record["has_tick_rate"] else None,
              "CheckPrerequisites": record["check_prerequisites"],
              "EnterEvent": _event_value(record["enter_event"]) if record["has_enter_event"] else None,
              "LinkedSubtree": live.paths.get(_id(record["linked_subtree"]), ""),
              "LinkedAsset": record["linked_asset"]}
    for key in STATE_FIELDS:
        if include_defaults or not _same(values[key], STATE_DEFAULTS[key], True):
            state[key] = values[key]
    params = live.state_params.get(state_id)
    if params is not None:
        if record["type"] in LINKED_TYPES:
            overridden = [prop for prop in params["props"] if prop["id"] in params.get("overrides", [])]
            if overridden or include_defaults:
                state["ParameterValues"] = {prop["name"]: prop["value"] for prop in overridden if "value" in prop}
        elif params["props"] or include_defaults:
            state["Parameters"] = _parameters_doc(params)
        if bindings.get(params["id"]):
            state["ParameterBindings"] = dict(bindings[params["id"]])
    for key, lists in (("EnterConditions", ("enter_conditions",)), ("Tasks", ("tasks", "single_task")),
                       ("Considerations", ("considerations",))):
        nodes = [_node_doc(item, bindings, include_defaults) for list_name in lists
                 for item in live.node_list(list_name, state_id)]
        if nodes or include_defaults:
            state[key] = nodes
    transitions = [_transition_doc(live, state_id, item, bindings, include_defaults) for item in record["transitions"]]
    if transitions or include_defaults:
        state["Transitions"] = transitions
    children = [_state_doc(live, child, refs, bindings, include_defaults) for child in record["children"]]
    if children or include_defaults:
        state["Children"] = children
    return state


def _transition_doc(live, state_id, record, bindings, include_defaults):
    to = record["to"]
    target = live.paths.get(_id(to["state"]), to.get("name", "")) if to["type"] == "GotoState" else to["type"]
    transition = {"Trigger": record["trigger"], "To": target}
    event = _event_value(record["event"])
    if include_defaults or event != EVENT_DEFAULT:
        transition["Event"] = event
    if include_defaults or record["priority"] != "Normal":
        transition["Priority"] = record["priority"]
    if record["delay"]:
        transition["Delay"] = {"Duration": record["delay_duration"], "Variance": record["delay_variance"]}
    elif include_defaults:
        transition["Delay"] = None
    if include_defaults or not record["enabled"]:
        transition["Enabled"] = record["enabled"]
    conditions = [_node_doc(item, bindings, include_defaults)
                  for item in live.node_list("transition_conditions", state_id, record["id"])]
    if conditions or include_defaults:
        transition["Conditions"] = conditions
    return transition


def extract(target, read, context, include_defaults=False):
    live = Calls(context, read).live()
    return common.document(target, STATE_TREE_CLASS, {}, body_from_live(live, include_defaults))


def _pop_facts(value, path, out):
    """Moves the _FACTS of every node under value to out ({node document path: facts})."""
    if type(value) is dict:
        if _FACTS in value:
            out[path] = value.pop(_FACTS)
        for key, item in value.items():
            _pop_facts(item, f"{path}/{key}", out)
    elif type(value) is list:
        for index, item in enumerate(value):
            _pop_facts(item, f"{path}/{index}", out)


def body_and_facts(live):
    """Every live value of the Body, and per node the facts an omitted value compares with (_node_facts_of)."""
    _COLLECT_FACTS.append(True)
    try:
        body = body_from_live(live, include_defaults=True)
    finally:
        _COLLECT_FACTS.pop()
    facts = {}
    _pop_facts(body, "/Body", facts)
    return body, facts


def extract_with_defaults(target, read, context):
    """Diff / Verify: the document with every live value and the node facts (body_and_facts)."""
    body, facts = body_and_facts(Calls(context, read).live())
    return common.document(target, STATE_TREE_CLASS, {}, body), facts


# ---------------------------------------------------------------------------
# Diff: does the asset hold what the document authors (omitted = keep)?
# ---------------------------------------------------------------------------
class _Mismatch(Exception):
    """The first place the asset does not hold what the document authors, with the two values compared there when
    they are not the document's and the asset's own values at that path (a default stands in for an omitted one)."""

    def __init__(self, path, values=None):
        super().__init__(path)
        self.path = path
        self.values = values


def diff(authored, extracted, exact, facts=None):
    """facts (extract_with_defaults: every live value is read): a node's omitted flags and values compare with the
    defaults; without them only the listed ones compare."""
    changes = common.header_diff(authored, extracted)
    body = authored.get("Body", {})
    live = extracted.get("Body", {})
    for key in BODY_KEYS:
        if key not in body:
            continue
        try:
            _covers_body_key(key, body[key], live.get(key), exact, live, facts)
        except _Mismatch as mismatch:
            changes.append(_mismatch_difference(mismatch, f"/Body/{key}", body[key], key in live, live.get(key)))
    return with_diagnostics(changes, omitted_kept(body.get("States"), live.get("States")))


def omitted_kept(states, live, path="/Body/States"):
    """Info diagnostics of the listed states that leave out the Type of a live state of another kind: kept."""
    notes = []
    if type(states) is not list or type(live) is not list:
        return notes
    by_name = {state.get("Name"): state for state in live if type(state) is dict}
    for index, state in enumerate(states):
        if type(state) is not dict:
            continue
        current = by_name.get(str(state.get("RenamedFrom", "")).rsplit("/", 1)[-1]) or by_name.get(state.get("Name"))
        if current is None:
            continue
        where = f"{path}/{index}"
        if "Type" not in state and current.get("Type", "State") != "State":
            notes.append(RuntimeDiagnostic(path=where, code="StateTree.OmittedKept", severity=DiagnosticSeverity.INFO,
                                           message=f"Type omitted: the state keeps its Type {current['Type']} and its "
                                                   "link (the other state fields omitted take a new state's value)."))
        notes.extend(omitted_kept(state.get("Children"), current.get("Children"), f"{where}/Children"))
    return notes


def _mismatch_difference(mismatch, root, authored, live_present, live):
    """The difference at the mismatch path, with the values there (the region's values under a deeper path
    made the VerificationMismatch message walk the region from that path and name places that do not exist)."""
    if mismatch.values is not None:
        expected, actual = mismatch.values
        return Difference(mismatch.path, DiffKind.REPLACE, True, expected, True, actual)
    present, expected = common.at_pointer(authored, root, mismatch.path)
    actual_present, actual = common.at_pointer(live, root, mismatch.path) if live_present else (False, None)
    if not present:
        return Difference(root, DiffKind.REPLACE, True, authored, live_present, live)
    return Difference(mismatch.path, DiffKind.REPLACE, True, expected, actual_present, actual)


def _check(condition, path, *values):
    """``values``: (expected, actual) when they are not the document's and the asset's values at ``path``."""
    if not condition:
        raise _Mismatch(path, values or None)


def _covers_body_key(key, authored, live, exact, live_body, facts=None):
    path = f"/Body/{key}"
    if key == "Schema":
        live = live or {}
        _check("Class" not in authored or authored["Class"] == live.get("Class"), f"{path}/Class")
        _covers_values(authored.get("Properties", {}), live.get("Properties", {}), f"{path}/Properties", exact)
    elif key == "Parameters":
        _covers_parameters(authored, live or [], path, exact)
    elif key == "GlobalTasksCompletion":
        _check(authored == live, path)
    elif key in ("Evaluators", "GlobalTasks"):
        _covers_nodes(authored, live or [], path, exact, facts)
    elif key == "States":
        _covers_states(authored, live or [], path, exact, facts)


def _covers_values(authored, live, path, exact):
    for name, value in authored.items():
        _check(name in live and _same(value, live[name], exact), f"{path}/{_escape(name)}")


def _covers_parameters(authored, live, path, exact):
    _check(len(authored) == len(live), path)
    for index, (row, current) in enumerate(zip(authored, live)):
        where = f"{path}/{index}"
        _check(row["Name"] == current["Name"], f"{where}/Name")
        _check(_type_key(type_dto(row["Type"], where)) == _type_key(type_dto(current["Type"], where)), f"{where}/Type")
        if "Value" in row:
            _check("Value" in current and _same(row["Value"], current["Value"], exact), f"{where}/Value")


def _covers_nodes(authored, live, path, exact, facts=None):
    _check(len(authored) == len(live), path)
    for index, (node, current) in enumerate(zip(authored, live)):
        where = f"{path}/{index}"
        _check(_type_matches(node["Type"], current["Type"]), f"{where}/Type")
        _check(node.get("Name", "") == current.get("Name", ""), f"{where}/Name")
        known = facts.get(where) if facts is not None else None
        for key, _ in NODE_FLAGS:
            if key in node:
                _check(_same(node[key], current.get(key, NODE_DEFAULTS[key]), True), f"{where}/{key}", node[key],
                       current.get(key, NODE_DEFAULTS[key]))
            elif known is not None and key in current and (key != "ConsideredForCompletion"
                                                           or known["ConsideredEditable"]):
                # An omitted flag is a new node's.
                _check(_same(NODE_DEFAULTS[key], current[key], True), f"{where}/{key}", NODE_DEFAULTS[key], current[key])
        _covers_values(node.get("Node", {}), current.get("Node", {}), f"{where}/Node", exact)
        _covers_values(node.get("Instance", {}), current.get("Instance", {}), f"{where}/Instance", exact)
        if known is not None:
            # An omitted value is the node type's default: a live value not at its default differs.
            for key in ("Node", "Instance"):
                for name in sorted(set(known[key]) - set(node.get(key, {}))):
                    _check(False, f"{where}/{key}/{_escape(name)}", known[key][name], current.get(key, {}).get(name))
        if "Bindings" in node:
            _covers_bindings(node["Bindings"], current.get("Bindings", {}), f"{where}/Bindings", exact)


def _covers_bindings(authored, live, path, exact):
    _check(set(authored) == set(live), path)
    for target, source in authored.items():
        where = f"{path}/{_escape(target)}"
        current = live[target]
        if type(source) is str:
            # Sources compare by property path; the reference may be spelled by name or index.
            _check(type(current) is str and _split_source(source)[1] == _split_source(current)[1], where)
            continue
        _check(type(current) is dict and _type_matches(source["Function"], current.get("Function", "")), where)
        _covers_values(source.get("Instance", {}), current.get("Instance", {}), f"{where}/Instance", exact)
        if "Bindings" in source:
            _covers_bindings(source["Bindings"], current.get("Bindings", {}), f"{where}/Bindings", exact)


def _covers_states(authored, live, path, exact, facts=None):
    _check(len(authored) == len(live), path)
    for index, (state, current) in enumerate(zip(authored, live)):
        where = f"{path}/{index}"
        _check(state["Name"] == current["Name"], f"{where}/Name")
        live_type = current.get("Type", "State")
        state_type = state.get("Type", live_type)
        for key in STATE_FIELDS:
            if key in STATE_KEPT and key not in state:
                continue
            # An omitted field is a new state's (a sparse read leaves the defaults out).
            expected, actual = state.get(key, state_default(key, state_type)), current.get(key, state_default(key, live_type))
            _check(_same(expected, actual, exact), f"{where}/{key}", expected, actual)
        if "Parameters" in state:
            _covers_parameters(state["Parameters"], current.get("Parameters", []), f"{where}/Parameters", exact)
        if "ParameterValues" in state:
            values = current.get("ParameterValues", {})
            _check(set(state["ParameterValues"]) == set(values), f"{where}/ParameterValues")
            _covers_values(state["ParameterValues"], values, f"{where}/ParameterValues", exact)
        if "ParameterBindings" in state:
            _covers_bindings(state["ParameterBindings"], current.get("ParameterBindings", {}),
                             f"{where}/ParameterBindings", exact)
        for key in STATE_LISTS:
            if key in state:
                _covers_nodes(state[key], current.get(key, []), f"{where}/{key}", exact, facts)
        if "Transitions" in state:
            _covers_transitions(state["Transitions"], current.get("Transitions", []), f"{where}/Transitions", exact,
                                facts)
        if "Children" in state:
            _covers_states(state["Children"], current.get("Children", []), f"{where}/Children", exact, facts)


def _covers_transitions(authored, live, path, exact, facts=None):
    _check(len(authored) == len(live), path)
    for index, (transition, current) in enumerate(zip(authored, live)):
        where = f"{path}/{index}"
        _check(transition["Trigger"] == current["Trigger"], f"{where}/Trigger")
        _check(transition["To"] == current["To"], f"{where}/To")
        _check(_event_same(transition.get("Event"), current.get("Event")), f"{where}/Event")
        _check(transition.get("Priority", "Normal") == current.get("Priority", "Normal"), f"{where}/Priority",
               transition.get("Priority", "Normal"), current.get("Priority", "Normal"))
        _check(transition.get("Enabled", True) == current.get("Enabled", True), f"{where}/Enabled",
               transition.get("Enabled", True), current.get("Enabled", True))
        delay, live_delay = transition.get("Delay"), current.get("Delay")
        _check((delay is None) == (live_delay is None), f"{where}/Delay")
        if delay is not None:
            _check(_same(delay["Duration"], live_delay["Duration"], exact)
                   and _same(delay.get("Variance", 0), live_delay.get("Variance", 0), exact), f"{where}/Delay")
        if "Conditions" in transition:
            _covers_nodes(transition["Conditions"], current.get("Conditions", []), f"{where}/Conditions", exact, facts)


# ---------------------------------------------------------------------------
# Inspect
# ---------------------------------------------------------------------------
def inspect_live(live):
    tree = live.tree
    return {
        "Schema": {"Class": tree.get("schema_class", ""), "EditorData": tree.get("editor_data_class", ""),
                   "UnsupportedProperties": list(tree.get("schema_unsupported", []))},
        "Context": [{"Name": item["name"], "Struct": item["struct"], "Source": f"Context/{item['name']}"}
                    for item in tree.get("context", [])],
        "Allows": {"Evaluators": tree.get("allows", {}).get("evaluators", True),
                   "EnterConditions": tree.get("allows", {}).get("enter_conditions", True),
                   "Considerations": tree.get("allows", {}).get("considerations", True),
                   "MultipleTasks": tree.get("allows", {}).get("multiple_tasks", True),
                   "Parameters": tree.get("allows", {}).get("global_parameters", True),
                   "TasksCompletion": tree.get("allows", {}).get("tasks_completion", True)},
        "Compiled": bool(tree.get("compiled_hash")) and tree.get("compiled_hash") == tree.get("editor_hash"),
        "NodeTypes": _node_types(),
    }


def _node_types():
    return {key: {"Struct": struct, **({"Class": cls} if cls else {})} for key, (struct, cls) in NODE_BASES.items()}


# ---------------------------------------------------------------------------
# Handler
# ---------------------------------------------------------------------------
class StateTreeRuntimeProfile:
    runtime_entry_module = "asset_document/bootstrap.py"
    # Snapshot-covered: preflight never reads a preview.
    preflight_scratch = False
    exact_asset_class_path = STATE_TREE_CLASS
    profile_id = "profile.state_tree.v1"

    def required_primitives(self):
        return REQUIREMENTS

    def inspect_profile(self):
        return {"Class": STATE_TREE_CLASS, "BodySections": list(BODY_KEYS), "ReadOnlySections": [],
                "CreateContext": "Body.Schema.Class", "SchemaBase": SCHEMA_BASE, "NodeTypes": _node_types(),
                "StateTypes": list(STATE_TYPES), "TransitionTargets": list(TARGET_KINDS) + ["<state path>"]}

    def create_template(self, target):
        body = {"Schema": {"Class": "/Script/GameplayStateTreeModule.StateTreeComponentSchema", "Properties": {}},
                "Parameters": [],
                "States": [{"Name": "Root", "Children": [
                    {"Name": "Idle", "Tasks": [{"Type": "/Script/StateTreeModule.StateTreeDelayTask",
                                                "Instance": {"Duration": 1.0}}],
                     "Transitions": [{"Trigger": "OnStateCompleted", "To": "Root/Idle"}]}]}]}
        return common.document(common.require_target(target, PREFIX), STATE_TREE_CLASS, {}, body)

    def validate(self, document):
        canonicalize(document)

    def canonicalize(self, document):
        return canonicalize(document)

    def create_target_context(self, document, context):
        return create_target_context(canonicalize(document), context)

    def preflight(self, document, scratch_handle, context):
        canonicalize(document)
        return SNAPSHOT_COVERED

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        return apply(canonicalize(document), read_asset_handle, mutable_asset_handle, context)

    def repair(self, document, asset_handle, context):
        canonicalize(document)
        return ()

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        return extract(common.require_target(target, PREFIX), asset_handle, context, include_defaults)

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        if mode != "live_asset" or exact_class_path != STATE_TREE_CLASS or editor_asset_handle is None:
            raise _error("/Inspect", "StateTree inspection reads a live StateTree asset.", "InspectNotAdmitted")
        live = Calls(context, editor_asset_handle).live()
        document = common.document(exact_asset_object_path.rsplit(".", 1)[0], STATE_TREE_CLASS, {}, body_from_live(live))
        return {**canonicalize(document), "TreeDetails": inspect_live(live)}

    def diff(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=False)

    def diff_exact(self, authored, extracted):
        return diff(canonicalize(authored), canonicalize(extracted), exact=True)

    def extract_with_defaults(self, *, target, asset_handle, context):
        return extract_with_defaults(common.require_target(target, PREFIX), asset_handle, context)

    def diff_with_defaults(self, authored, extracted, live_defaults, exact):
        return diff(canonicalize(authored), canonicalize(extracted), exact=exact, facts=live_defaults)


def _meta():
    rows = (("schema", "/Body/Schema", IdentityPolicy.POSITIONAL, OrderPolicy.CANONICAL, ComparisonPolicy.CANONICAL),
            ("parameters", "/Body/Parameters", IdentityPolicy.STABLE_KEY, OrderPolicy.PRESERVE, ComparisonPolicy.ORDERED),
            ("evaluators", "/Body/Evaluators", IdentityPolicy.STABLE_KEY, OrderPolicy.PRESERVE, ComparisonPolicy.ORDERED),
            ("global_tasks", "/Body/GlobalTasks", IdentityPolicy.STABLE_KEY, OrderPolicy.PRESERVE, ComparisonPolicy.ORDERED),
            ("states", "/Body/States", IdentityPolicy.STABLE_KEY, OrderPolicy.PRESERVE, ComparisonPolicy.ORDERED))
    regions = tuple(RegionMeta(f"state_tree.{name}.v1", path, RegionOwnership.MANAGED, identity, order,
                               DefaultPolicy.EXPLICIT, OmissionPolicy.PRESERVE, DeletePolicy.EXPLICIT_ONLY, comparison)
                    for name, path, identity, order, comparison in rows)
    return ProfileMeta(StateTreeRuntimeProfile.profile_id, ROUTE, STATE_TREE_CLASS, regions, REQUIREMENTS)


STATE_TREE_RUNTIME_REGISTRATION = RuntimeProfileRegistration(_meta(), StateTreeRuntimeProfile,
                                                             RuntimeTargetPolicy.CREATE_OR_UPDATE, LIFECYCLE)

__all__ = ("STATE_TREE_RUNTIME_REGISTRATION", "StateTreeRuntimeProfile", "Planner", "Live", "canonicalize",
           "body_from_live", "body_and_facts", "match_nodes")
