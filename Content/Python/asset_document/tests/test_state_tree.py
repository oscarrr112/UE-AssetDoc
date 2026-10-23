# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""StateTree Profile: document validation, identity matching, op planning, binding references, extract
and diff over synthetic ai.state_tree.read.v1 records.  The editor-side behaviour is covered end to end by
UEAssetDoc.StateTree.* in the editor."""
from __future__ import annotations

import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.profiles import state_tree as st

TARGET = "/Game/AI/ST_Test"
SCHEMA = "/Script/GameplayStateTreeModule.StateTreeComponentSchema"
DELAY = "/Script/StateTreeModule.StateTreeDelayTask"
COMPARE = "/Script/StateTreeModule.StateTreeCompareIntCondition"
ROOT_PARAMS = "A" * 32
CONTEXT_ACTOR = "C" * 32


def _id(number):
    return f"{number:032X}"


def _document(body, action="CreateOrUpdate"):
    return {"SchemaVersion": 1, "Target": TARGET, "Class": st.STATE_TREE_CLASS, "Action": action,
            "Properties": {}, "Body": body}


def _tree(**overrides):
    record = {"kind": "tree", "schema_class": SCHEMA, "schema": {"ContextActorClass": "/Script/Engine.Actor"},
              "schema_defaults": [], "schema_unsupported": [], "editor_data_class": "/Script/StateTreeEditorModule.StateTreeEditorData",
              "global_tasks_completion": "Any", "context": [{"name": "Actor", "id": CONTEXT_ACTOR, "struct": "/Script/Engine.Actor"}],
              "allows": {"evaluators": True, "enter_conditions": True, "considerations": True, "multiple_tasks": True,
                         "global_parameters": True, "tasks_completion": True},
              "compiled_hash": 7, "editor_hash": 7, "ready": True}
    record.update(overrides)
    return record


def _params(owner="", ident=ROOT_PARAMS, props=()):
    return {"kind": "parameters", "owner": owner, "id": ident, "fixed": False, "props": list(props), "overrides": []}


def _prop(name, ident, category="real", sub="double", value=None):
    prop = {"name": name, "id": ident, "type": {"category": category, "sub_category": sub, "sub_category_object": "",
                                                 "container": "none", "is_ref": False, "is_const": False},
            "supported": True}
    if value is not None:
        prop["value"] = value
    return prop


def _state(ident, name, parent="", index=0, **overrides):
    record = {"kind": "state", "id": ident, "parent": parent, "index": index, "name": name, "description": "", "tag": "",
              "type": "State", "selection": "TrySelectChildrenInOrder", "tasks_completion": "Any", "enabled": True,
              "weight": 1, "has_tick_rate": False, "tick_rate": 0, "check_prerequisites": True,
              "has_enter_event": False, "enter_event": {"tag": "", "payload": "", "consume": True},
              "enter_event_id": f"E{ident[1:]}", "linked_subtree": st.ZERO_ID, "linked_asset": "", "transitions": []}
    record.update(overrides)
    return record


def _transition(ident, to_type="GotoState", to_state="", trigger="OnStateCompleted", **overrides):
    record = {"id": ident, "trigger": trigger, "event": {"tag": "", "payload": "", "consume": True},
              "event_id": f"F{ident[1:]}", "to": {"type": to_type, "state": to_state or st.ZERO_ID, "name": ""},
              "priority": "Normal", "delay": False, "delay_duration": 0, "delay_variance": 0, "enabled": True,
              "delegate": False}
    record.update(overrides)
    return record


def _node(ident, list_name, state="", index=0, node_type=DELAY, name="", instance=None, transition="", **overrides):
    record = {"kind": "node", "list": list_name, "state": state, "transition": transition, "index": index, "id": ident,
              "node_id": f"9{ident[1:]}", "type": node_type, "name": name, "indent": 0, "operand": "And",
              "instance_type": "", "node": {}, "node_defaults": [], "instance": instance or {}, "instance_defaults": [],
              "unsupported": []}
    if list_name in ("tasks", "global_tasks", "single_task"):
        record.update(enabled=True, considered=True, considered_editable=True)
    if list_name in ("enter_conditions", "transition_conditions"):
        record["evaluation_mode"] = "Evaluated"
    record.update(overrides)
    return record


ROOT, IDLE, WALK = _id(1), _id(2), _id(3)
WAIT = _id(10)


def _live(extra=(), transitions=None):
    records = [_tree(), _params(props=[_prop("WaitTime", _id(50), value=2.0)]),
               _state(ROOT, "Root"), _params(ROOT, _id(60)),
               _state(IDLE, "Idle", ROOT, 0, transitions=transitions if transitions is not None
                      else [_transition(_id(20), to_state=WALK)]),
               _params(IDLE, _id(61)),
               _node(WAIT, "tasks", IDLE, name="Wait", instance={"Duration": 1, "RandomDeviation": 0, "bRunForever": False}),
               _state(WALK, "Walk", ROOT, 1), _params(WALK, _id(62))]
    return st.Live(records + list(extra))


def _body():
    return {"States": [{"Name": "Root", "Children": [
        {"Name": "Idle", "Tasks": [{"Type": DELAY, "Name": "Wait", "Instance": {"Duration": 1}}],
         "Transitions": [{"Trigger": "OnStateCompleted", "To": "Root/Walk"}]},
        {"Name": "Walk"}]}]}


def _plan(body, live=None):
    planner = st.Planner(body, live or _live())
    return planner, st.plan_structure(planner)


class ValidationTests(unittest.TestCase):
    def _code(self, body):
        with self.assertRaises(AssetDocumentContractError) as raised:
            st.canonicalize(_document(body))
        return raised.exception.diagnostic

    def test_paths_name_the_document_location(self):
        self.assertEqual(self._code({"States": [{"Name": "A/B"}]}).code, "StateTree.InvalidStateName")
        self.assertEqual(self._code({"States": [{"Name": "A"}, {"Name": "A"}]}).code, "StateTree.DuplicateStateName")
        diagnostic = self._code({"States": [{"Name": "A", "Tasks": [{"Name": "x"}]}]})
        self.assertEqual(diagnostic.path, "/Body/States/0/Tasks/0/Type")
        self.assertEqual(self._code({"States": [{"Name": "A", "Bogus": 1}]}).code, "StateTree.UnknownKey")
        self.assertEqual(self._code({"Extra": 1}).code, "StateTree.UnknownBodyKey")

    def test_semantic_rules_follow_the_editor(self):
        delayed = {"States": [{"Name": "A", "Transitions": [{"Trigger": "OnStateCompleted", "To": "A",
                                                              "Delay": {"Duration": 1}}]}]}
        self.assertEqual(self._code(delayed).code, "StateTree.InvalidDelay")
        group = {"States": [{"Name": "A", "Type": "Group", "Tasks": [{"Type": DELAY}]}]}
        self.assertEqual(self._code(group).code, "StateTree.InvalidValue")
        linked = {"States": [{"Name": "A", "Type": "Linked", "Parameters": []}]}
        self.assertEqual(self._code(linked).code, "StateTree.ParameterLayoutFixed")
        selection = {"States": [{"Name": "A", "Type": "Linked", "Selection": "TrySelectChildrenInOrder"}]}
        self.assertEqual(self._code(selection).path, "/Body/States/0/Selection")
        self.assertEqual(self._code({"Parameters": [{"Name": "P"}]}).code, "StateTree.InvalidParameter")
        self.assertEqual(self._code({"Evaluators": [{"Type": "X", "Indent": 9}]}).code, "StateTree.InvalidValue")
        self.assertEqual(self._code({"Evaluators": [{"Type": "X", "Bindings": {"A": ":B"}}]}).code,
                         "StateTree.InvalidBinding")

    def test_properties_are_refused_and_a_valid_document_passes(self):
        with self.assertRaises(AssetDocumentContractError) as raised:
            st.canonicalize({**_document({}), "Properties": {"X": 1}})
        self.assertEqual(raised.exception.diagnostic.code, "StateTree.UnsupportedProperties")
        self.assertEqual(st.canonicalize(_document(_body()))["Body"], _body())


class PlanningTests(unittest.TestCase):
    def test_extracted_body_reapplies_as_a_no_op(self):
        live = _live()
        body = st.body_from_live(live)
        _, ops = _plan(body, live)
        self.assertEqual(ops, [])
        planner, ops = _plan(_body(), live)
        self.assertEqual(ops, [])
        self.assertEqual(planner.plan_bindings(live), [])
        self.assertEqual(planner.plan_parameter_values(live), [])

    def test_rename_keeps_the_state_id_and_transitions_follow_the_path(self):
        body = _body()
        body["States"][0]["Children"][1] = {"Name": "Run", "RenamedFrom": "Walk"}
        body["States"][0]["Children"][0]["Transitions"][0]["To"] = "Root/Run"
        planner, ops = _plan(body)
        self.assertEqual(planner.paths["Root/Run"], WALK)
        self.assertEqual([op["op"] for op, _ in ops], ["state"])
        self.assertEqual(ops[0][0], {"op": "state", "id": WALK, "name": "Run"})

    def test_move_with_full_path_rename_and_remove(self):
        body = {"States": [{"Name": "Root", "Children": [{"Name": "Walk", "Children": [
            {"Name": "Idle", "RenamedFrom": "Root/Idle"}]}]}]}
        planner, ops = _plan(body)
        hierarchy = ops[0][0]
        self.assertEqual(hierarchy["op"], "hierarchy")
        self.assertEqual(hierarchy["subtrees"], [{"id": ROOT, "name": "Root", "children": [
            {"id": WALK, "name": "Walk", "children": [{"id": IDLE, "name": "Idle", "children": []}]}]}])
        removed = {"States": [{"Name": "Root", "Children": [{"Name": "Walk"}]}]}
        _, ops = _plan(removed)
        self.assertEqual(ops[0][0]["subtrees"][0]["children"], [{"id": WALK, "name": "Walk", "children": []}])

    def test_omitted_children_keep_the_live_subtree(self):
        planner, ops = _plan({"States": [{"Name": "Root", "Description": "top"}]})
        self.assertEqual([op["op"] for op, _ in ops], ["state"])
        self.assertEqual(planner.paths, {"Root": ROOT, "Root/Idle": IDLE, "Root/Walk": WALK})

    def test_new_states_get_ids_and_fields(self):
        body = _body()
        body["States"][0]["Children"].append({"Name": "Jump", "Type": "Group", "Tag": "AI.Jump"})
        planner, ops = _plan(body)
        jump = planner.paths["Root/Jump"]
        self.assertNotIn(jump, (ROOT, IDLE, WALK))
        self.assertEqual([op["op"] for op, _ in ops], ["hierarchy", "state"])
        self.assertEqual(ops[1][0], {"op": "state", "id": jump, "tag": "AI.Jump", "type": "Group"})

    def test_transition_targets_and_missing_states(self):
        body = _body()
        body["States"][0]["Children"][0]["Transitions"] = [
            {"Trigger": "OnEvent", "Event": {"Tag": "Ev.Go"}, "To": "Succeeded", "Priority": "High",
             "Delay": {"Duration": 0.5}},
            {"Trigger": "OnTick", "To": "Root/Missing"}]
        with self.assertRaises(AssetDocumentContractError) as raised:
            _plan(body)
        self.assertEqual(raised.exception.diagnostic.path, "/Body/States/0/Children/0/Transitions/1/To")
        self.assertEqual(raised.exception.diagnostic.code, "StateTree.TransitionTargetNotFound")
        body["States"][0]["Children"][0]["Transitions"].pop()
        planner, ops = _plan(body)
        op, where = ops[0]
        self.assertEqual(op["transitions"][0]["id"], _id(20))
        self.assertEqual(op["transitions"][0]["to"], {"type": "Succeeded"})
        self.assertEqual(op["transitions"][0]["delay"], {"duration": 0.5, "variance": 0})
        self.assertEqual(st.map_op_path("/ops/0/transitions/0/to/type", [where]),
                         "/Body/States/0/Children/0/Transitions/0/To/type")

    def test_node_identity_by_name_then_type_order(self):
        current = [_node(_id(30), "tasks", name="A"), _node(_id(31), "tasks"), _node(_id(32), "tasks")]
        matches = st.match_nodes([{"Type": DELAY}, {"Type": "StateTreeDelayTask", "Name": "A"}, {"Type": "FStateTreeDelayTask"},
                                  {"Type": COMPARE}], current)
        self.assertEqual([item["id"] if item else None for item in matches], [_id(31), _id(30), _id(32), None])
        retyped = st.match_nodes([{"Type": COMPARE, "Name": "A"}], current)
        self.assertEqual(retyped, [None])

    def test_node_value_edit_sends_only_the_change(self):
        body = _body()
        body["States"][0]["Children"][0]["Tasks"][0]["Instance"] = {"Duration": 3, "bRunForever": False}
        planner, ops = _plan(body)
        op, where = ops[0]
        self.assertEqual(op, {"op": "nodes", "list": "tasks", "nodes": [{"id": WAIT, "type": DELAY, "instance": {"Duration": 3}}],
                              "state": IDLE})
        self.assertEqual(st.map_op_path("/ops/0/nodes/0/instance/Duration", [where]),
                         "/Body/States/0/Children/0/Tasks/0/Instance/Duration")

    def test_omitted_fields_and_values_are_reset(self):
        # An existing state's / node's field or value the document leaves out is a new one's.
        live = _live()
        for record in live.node_list("tasks", IDLE):
            record.update(enabled=False, operand="Or", considered=False, considered_editable=False,
                          instance={"Duration": 1, "RandomDeviation": 0.5, "bRunForever": False},
                          instance_reset={"RandomDeviation": 0.0})
        live.states[IDLE].update(description="Old", has_tick_rate=True, tick_rate=5)
        _, ops = _plan(_body(), live)
        self.assertEqual([op for op, _ in ops], [
            {"op": "state", "id": IDLE, "description": "", "tick_rate": None},
            {"op": "nodes", "list": "tasks", "state": IDLE, "nodes": [
                {"id": WAIT, "type": DELAY, "enabled": True, "operand": "And", "instance": {"RandomDeviation": 0.0}}]}])

    def test_omitted_state_kind_is_kept(self):
        # An omitted Type (and link) keeps the state's kind; a linked state's omitted Selection is TryEnterState.
        live = _live()
        live.states[WALK].update(type="Linked", selection="TryEnterState", linked_subtree=ROOT, description="Old")
        _, ops = _plan(_body(), live)
        self.assertEqual([op for op, _ in ops if op.get("id") == WALK], [{"op": "state", "id": WALK, "description": ""}])
        extracted = st.canonicalize(_document(st.body_from_live(live)))
        walk = {"Name": "Walk", "Description": "Old"}
        body = {"States": [{"Name": "Root", "Children": [_body()["States"][0]["Children"][0], walk]}]}
        result = st.diff(st.canonicalize(_document(body)), extracted, False)
        self.assertEqual(result, ())
        self.assertEqual([(note.path, note.code) for note in result.diagnostics],
                         [("/Body/States/0/Children/1", "StateTree.OmittedKept")])

    def test_single_task_schema_maps_tasks(self):
        live = st.Live([_tree(allows={"multiple_tasks": False}), _params(), _state(ROOT, "Root")])
        body = {"States": [{"Name": "Root", "Tasks": [{"Type": DELAY}, {"Type": DELAY}]}]}
        with self.assertRaises(AssetDocumentContractError) as raised:
            _plan(body, live)
        self.assertEqual(raised.exception.diagnostic.code, "StateTree.ListNotAllowed")
        _, ops = _plan({"States": [{"Name": "Root", "Tasks": [{"Type": DELAY}]}]}, live)
        self.assertEqual(ops[0][0]["list"], "single_task")

    def test_parameters_match_by_name_and_rename(self):
        planner, ops = _plan({"Parameters": [{"Name": "WaitTime", "Type": "float", "Value": 2.0}]})
        self.assertEqual(ops, [])
        _, ops = _plan({"Parameters": [{"Name": "Delay", "RenamedFrom": "WaitTime", "Type": "float"},
                                       {"Name": "Count", "Type": "int", "Value": 3}]})
        op = ops[0][0]
        self.assertEqual(op["props"][0]["id"], _id(50))
        self.assertEqual(op["props"][1]["type"]["category"], "int")
        self.assertEqual(op["props"][1]["value"], 3)
        self.assertNotIn("state", op)

    def test_schema_class_is_immutable_and_properties_are_sparse(self):
        with self.assertRaises(AssetDocumentContractError) as raised:
            _plan({"Schema": {"Class": "/Script/StateTreeModule.StateTreeSchema"}})
        self.assertEqual(raised.exception.diagnostic.code, "StateTree.SchemaClassImmutable")
        _, ops = _plan({"Schema": {"Class": SCHEMA, "Properties": {"ContextActorClass": "/Script/Engine.Pawn"}}})
        self.assertEqual(ops[0][0], {"op": "schema", "values": {"ContextActorClass": "/Script/Engine.Pawn"}})

    def test_kept_state_name_clash_is_reported(self):
        body = {"States": [{"Name": "Root", "Children": [{"Name": "Idle", "RenamedFrom": "Walk"}]}]}
        planner, ops = _plan({"States": [{"Name": "Root", "Children": [{"Name": "Idle"}]}]})
        self.assertEqual(planner.paths["Root/Idle"], IDLE)
        with self.assertRaises(AssetDocumentContractError):
            # Walk renamed to Idle while the live Idle is not listed (removed) is fine; listed twice is not.
            _plan({"States": [{"Name": "Root", "Children": [{"Name": "Idle"}, {"Name": "Idle", "RenamedFrom": "Walk"}]}]})
        _, ops = _plan(body)
        self.assertEqual(ops[0][0]["op"], "hierarchy")


class BindingTests(unittest.TestCase):
    def _live(self, bindings=()):
        return _live(list(bindings) + [_node(_id(40), "evaluators", name="Sensor", node_type="/Script/M.Eval")])

    def test_sources_resolve_to_struct_ids(self):
        live = self.live = self._live()
        planner, _ = _plan(_body(), live)
        refs = st.SourceRefs(planner, live)
        self.assertEqual(refs.resolve("Parameters", "/x"), ROOT_PARAMS)
        self.assertEqual(refs.resolve("Context/Actor", "/x"), CONTEXT_ACTOR)
        self.assertEqual(refs.resolve("Evaluators/Sensor", "/x"), _id(40))
        self.assertEqual(refs.resolve("Evaluators/0", "/x"), _id(40))
        self.assertEqual(refs.resolve("Root/Idle/Tasks/Wait", "/x"), WAIT)
        self.assertEqual(refs.resolve("Root/Idle/Parameters", "/x"), _id(61))
        self.assertEqual(refs.resolve("Root/Idle/EnterEvent", "/x"), f"E{IDLE[1:]}")
        self.assertEqual(refs.resolve("Root/Idle/Transitions/0/Event", "/x"), f"F{_id(20)[1:]}")
        with self.assertRaises(AssetDocumentContractError) as raised:
            refs.resolve("Root/Nope/Tasks/0", "/Body/X")
        self.assertEqual(raised.exception.diagnostic.code, "StateTree.BindingSourceNotFound")
        self.assertEqual(refs.name(WAIT), "Root/Idle/Tasks/Wait")

    def test_binding_ops_and_no_op(self):
        live = self._live()
        body = _body()
        body["States"][0]["Children"][0]["Tasks"][0]["Bindings"] = {
            "Duration": "Parameters:WaitTime", "Node:Text": "Evaluators/Sensor:Out",
            "RandomDeviation": {"Function": "StateTreeAddFloatPropertyFunction", "Bindings": {"Left": "Parameters:WaitTime"}}}
        planner, _ = _plan(body, live)
        ops = planner.plan_bindings(live)
        op = ops[0][0]
        self.assertEqual(op["targets"], [WAIT, f"9{WAIT[1:]}"])
        self.assertEqual(op["bindings"][0], {"target": {"struct": WAIT, "path": "Duration"},
                                             "source": {"struct": ROOT_PARAMS, "path": "WaitTime"}})
        self.assertEqual(op["bindings"][1]["target"], {"struct": f"9{WAIT[1:]}", "path": "Text"})
        where = ops[0][1]
        self.assertEqual(st.map_op_path("/ops/0/bindings/0/source", [where]),
                         "/Body/States/0/Children/0/Tasks/0/Bindings/Duration")
        self.assertEqual(st.map_op_path("/ops/0/bindings/1/target", [where]),
                         "/Body/States/0/Children/0/Tasks/0/Bindings/Node:Text")
        self.assertEqual(op["bindings"][2]["function"]["bindings"],
                         [{"target": {"path": "Left"}, "source": {"struct": ROOT_PARAMS, "path": "WaitTime"}}])
        function_id = _id(70)
        bound = self._live([
            {"kind": "binding", "source": {"struct": ROOT_PARAMS, "path": "WaitTime"}, "target": {"struct": WAIT, "path": "Duration"},
             "output": False},
            {"kind": "binding", "source": {"struct": _id(40), "path": "Out"}, "target": {"struct": f"9{WAIT[1:]}", "path": "Text"},
             "output": False},
            {"kind": "binding", "source": {"struct": function_id, "path": "Result"}, "target": {"struct": WAIT, "path": "RandomDeviation"},
             "output": False, "function": {"id": function_id, "node_id": _id(71),
                                            "type": "/Script/StateTreeModule.StateTreeAddFloatPropertyFunction",
                                            "instance": {"Right": 0}, "instance_defaults": [], "unsupported": []}},
            {"kind": "binding", "source": {"struct": ROOT_PARAMS, "path": "WaitTime"}, "target": {"struct": function_id, "path": "Left"},
             "output": False}])
        planner, _ = _plan(body, bound)
        self.assertEqual(planner.plan_bindings(bound), [])
        extracted = st.body_from_live(bound)
        task = extracted["States"][0]["Children"][0]["Tasks"][0]
        self.assertEqual(task["Bindings"]["Duration"], "Parameters:WaitTime")
        self.assertEqual(task["Bindings"]["Node:Text"], "Evaluators/Sensor:Out")
        self.assertEqual(task["Bindings"]["RandomDeviation"]["Bindings"], {"Left": "Parameters:WaitTime"})
        self.assertEqual(st.diff(st.canonicalize(_document(body)), st.canonicalize(_document(extracted)), False), ())


class ExtractDiffTests(unittest.TestCase):
    def test_extract_is_compact_and_diff_detects_changes(self):
        live = _live()
        body = st.body_from_live(live)
        self.assertEqual(body["States"][0]["Children"][0]["Transitions"], [{"Trigger": "OnStateCompleted", "To": "Root/Walk"}])
        self.assertNotIn("Description", body["States"][0])
        self.assertEqual(body["Parameters"], [{"Name": "WaitTime", "Type": "float", "Value": 2.0}])
        full = st.body_from_live(live, include_defaults=True)
        self.assertEqual(full["States"][0]["Enabled"], True)
        extracted = st.canonicalize(_document(body))
        authored = st.canonicalize(_document(_body()))
        self.assertEqual(st.diff(authored, extracted, False), ())
        changed = _body()
        changed["States"][0]["Children"][0]["Tasks"][0]["Instance"]["Duration"] = 5
        differences = st.diff(st.canonicalize(_document(changed)), extracted, False)
        self.assertEqual([item.path for item in differences], ["/Body/States/0/Children/0/Tasks/0/Instance/Duration"])
        defaults = _body()
        defaults["States"][0]["Enabled"] = True
        defaults["States"][0]["Children"][0]["Transitions"][0]["Priority"] = "Normal"
        self.assertEqual(st.diff(st.canonicalize(_document(defaults)), extracted, False), ())

    def test_omitted_state_and_node_fields_are_the_defaults(self):
        # Deleting a state field, a node flag or a node value from an extract was kept and Diff said nothing.
        live = _live()
        for record in live.node_list("tasks", IDLE):
            record.update(enabled=False, indent=1, considered=False, considered_editable=False,
                          instance={"Duration": 1, "RandomDeviation": 0.5, "bRunForever": False},
                          instance_reset={"RandomDeviation": 0.0})
        live.states[IDLE].update(description="Old", weight=2)
        full, facts = st.body_and_facts(live)
        self.assertEqual(facts["/Body/States/0/Children/0/Tasks/0"],
                         {"Node": {}, "Instance": {"RandomDeviation": 0.0}, "ConsideredEditable": False})
        self.assertNotIn(st._FACTS, full["States"][0]["Children"][0]["Tasks"][0])
        extracted = st.canonicalize(_document(full))
        paths = [item.path for item in st.diff(st.canonicalize(_document(_body())), extracted, False, facts)]
        self.assertEqual(paths, ["/Body/States/0/Children/0/Description"])   # the first difference per region
        idle = _body()["States"][0]["Children"][0]
        idle.update(Description="Old", Weight=2)
        body = {"States": [{"Name": "Root", "Children": [idle, {"Name": "Walk"}]}]}
        paths = [item.path for item in st.diff(st.canonicalize(_document(body)), extracted, False, facts)]
        self.assertEqual(paths, ["/Body/States/0/Children/0/Tasks/0/Enabled"])
        idle["Tasks"][0].update(Enabled=False, Indent=1)
        paths = [item.path for item in st.diff(st.canonicalize(_document(body)), extracted, False, facts)]
        self.assertEqual(paths, ["/Body/States/0/Children/0/Tasks/0/Instance/RandomDeviation"])
        # The difference holds the values compared there (the default, the live value), not the region's.
        (item,) = st.diff(st.canonicalize(_document(body)), extracted, False, facts)
        self.assertEqual((item.before, item.after), (0.0, 0.5))
        idle["Tasks"][0]["Instance"]["RandomDeviation"] = 0.5
        # A Considered For Completion the editor does not let one edit is not reset.
        self.assertEqual(st.diff(st.canonicalize(_document(body)), extracted, False, facts), ())
        # Without the facts (a sparse read) only the listed values compare.
        self.assertEqual(st.diff(st.canonicalize(_document(body)), extracted, False), ())

    def test_verification_message_names_the_value_that_differs(self):
        # The difference carried the whole States region under the mismatch path, so the VerificationMismatch
        # text walked the region from there and named places like .../TickRate/0/Children/... .
        from asset_document.registered_runtime import _describe_differences
        extracted = st.canonicalize(_document(st.body_and_facts(_live())[0]))
        changed = _body()
        changed["States"][0]["Children"][0]["TickRate"] = 0.5
        differences = st.diff(st.canonicalize(_document(changed)), extracted, False)
        self.assertEqual([(item.path, item.before, item.after) for item in differences],
                         [("/Body/States/0/Children/0/TickRate", 0.5, None)])
        self.assertEqual(_describe_differences(differences),
                         "/Body/States/0/Children/0/TickRate: document 0.5, read back null")
        changed = _body()
        changed["States"][0]["Children"][0]["Tasks"][0]["Instance"]["Duration"] = 5
        self.assertEqual(_describe_differences(st.diff(st.canonicalize(_document(changed)), extracted, False)),
                         "/Body/States/0/Children/0/Tasks/0/Instance/Duration: document 5, read back 1")

    def test_compile_errors_fail_with_the_state_path(self):
        planner, _ = _plan(_body())
        result = {"succeeded": False, "messages": [{"severity": "Error", "message": "No next state.", "state_id": IDLE,
                                                    "state_path": "Root/Idle", "item_id": "", "item_name": ""}]}
        with self.assertRaises(AssetDocumentContractError) as raised:
            st.compile_diagnostics(result, planner)
        self.assertEqual(raised.exception.diagnostic.path, "/Body/States/0/Children/0")
        self.assertEqual(raised.exception.diagnostic.code, "StateTree.CompileError")
        warnings = st.compile_diagnostics({"succeeded": True, "messages": [
            {"severity": "Warning", "message": "w", "state_id": "", "state_path": "", "item_id": WAIT, "item_name": "Wait"}]},
            planner)
        self.assertEqual(warnings[0].path, "/Body/States/0/Children/0/Tasks/0")


if __name__ == "__main__":
    unittest.main()
