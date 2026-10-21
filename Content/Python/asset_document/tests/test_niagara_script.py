"""Niagara scripts and scratch pads (N4): Body checks, the semantic Diff, the script op planner, the graph codec and
the script Apply's call sequence against a fake runtime (dependents, exposed version rule, compile failure path).
The editor side is covered by UEAssetDoc.Niagara.Script* in the editor."""
from __future__ import annotations

import copy
import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.diff.model import DiffKind
from asset_document.graphs.live import LiveGraph, decode_node
from asset_document.graphs.model import thaw_json
from asset_document.graphs.schemas.niagara import (
    NIAGARA_ADAPTER, bind_by_action, decode_graph, has_version_setting, pins_hold,
)
from asset_document.profiles import niagara
from asset_document.profiles import niagara_script as scripts
from asset_document.profiles import niagara_shape as shape
from asset_document.regions.primitive_catalog import PrimitiveOutcome, PrimitiveOutcomeStatus

TARGET = "/Game/FX/NMS_Test"
GUID_1 = "11111111-1111-1111-1111-111111111111"
GUID_2 = "22222222-2222-2222-2222-222222222222"

SCRIPT_ROWS = [
    {"kind": "script", "scope": "", "usage": "Module", "settings": {"Category": "Forces"}},
    {"kind": "parameter", "scope": "", "name": "Module.Strength", "type": "float", "default": 1.0,
     "meta": {"Description": "How strong"}},
    {"kind": "parameter", "scope": "", "name": "Particles.Velocity", "type": "Vector3f",
     "default_mode": "FailIfPreviouslyNotSet"},
    {"kind": "node", "scope": "", "id": "Get", "action": "Niagara:MapGet", "x": 0, "y": 0,
     "pins": {"Inputs": [], "Outputs": [{"Name": "Module.Strength", "Type": "float"}]}},
    {"kind": "node", "scope": "", "id": "Out", "action": "Niagara:Output", "x": 300, "y": 0, "comment": "Done"},
    {"kind": "link", "scope": "", "from": "Get:Dest", "to": "Out:Output"},
    {"kind": "pin_default", "scope": "", "pin": "Out:Output", "value": "1.0"},
]


def _ok(value):
    return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED, value, (), (), ())


class _Runtime:
    """Serves fx.niagara.read / edit / compile for one script: reads page the rows, edits are recorded."""

    def __init__(self, rows, *, dependents=(), compile_result=None, before_errors=()):
        self.rows = rows
        self.dependents = list(dependents)
        self.compile_result = compile_result or {"status": "UpToDate", "errors": [], "warnings": [], "messages": []}
        self.before_errors = list(before_errors)
        self.edits = []
        self.compiled = 0

    def handoff(self, *, source_handle, target_provider_id, access):
        return (target_provider_id, access)

    def invoke(self, *, requirement, value_arguments, handle_arguments):
        primitive = requirement.primitive_id
        if primitive == niagara.READ:
            if value_arguments.get("detail") == "compile":
                return _ok({"class": "script", "errors": self.before_errors, "warnings": [], "rows": [],
                            "next_offset": -1})
            return _ok({"class": "script", "rows": self.rows, "total": len(self.rows), "next_offset": -1})
        if primitive == niagara.EDIT:
            self.edits.extend(value_arguments["ops"])
            return _ok({"changed": True, "created": [], "dependents": self.dependents})
        if primitive == niagara.COMPILE:
            self.compiled += 1
            return _ok(dict(self.compile_result, timed_out=False))
        raise AssertionError(f"unexpected primitive {primitive}")

    def kinds(self):
        return [op["op"] for op in self.edits]


def _document(body):
    return {"SchemaVersion": 1, "Target": TARGET, "Class": niagara.SCRIPT_CLASS, "Action": "CreateOrUpdate",
            "Definitions": {}, "Properties": {}, "Body": body}


def _body(rows=SCRIPT_ROWS):
    return copy.deepcopy(shape.script_asset_body(rows))


def _apply(runtime, body):
    return niagara.NiagaraScriptProfile().apply(_document(body), object(), object(), runtime)


def _versioned_rows():
    rows = copy.deepcopy(SCRIPT_ROWS)
    rows[0]["versions"] = [{"Number": "1.0", "Guid": GUID_1, "Exposed": True}]
    return rows


class ScriptBodyTests(unittest.TestCase):
    def test_extract_shape_includes_comment_and_binding(self) -> None:
        rows = copy.deepcopy(SCRIPT_ROWS)
        rows[2]["default_mode"] = "Binding"
        rows[2]["default_binding"] = "Particles.Position"
        body = shape.script_asset_body(rows)
        self.assertEqual(body["Graph"]["Nodes"][1]["Comment"], "Done")
        self.assertEqual(body["Parameters"][1]["DefaultBinding"], "Particles.Position")
        scripts.check_script_body(body)

    def test_checks(self) -> None:
        bad = [({"Usage": "System"}, "/Body/Usage"),
               ({"Parameters": [{"Name": "Module.A", "Default": 1, "DefaultMode": "Binding", "DefaultBinding": "B"}]},
                "/Body/Parameters/0/Default"),
               ({"Parameters": [{"Name": "Module.A", "DefaultBinding": "B"}]}, "/Body/Parameters/0/DefaultBinding"),
               ({"Parameters": [{"Name": "Module.A"}, {"Name": "Module.A"}]}, "/Body/Parameters/1/Name"),
               ({"Versions": [{"Number": "1"}]}, "/Body/Versions/0/Number"),
               ({"Versions": [{"Number": "1.0", "Exposed": True}, {"Number": "2.0", "Exposed": True}]}, "/Body/Versions"),
               ({"Graph": {"Nodes": [{"Id": "A"}]}}, "/Body/Graph/Nodes/0/Action"),
               ({"Graph": {"PinDefaults": {"Missing:Pin": "1"}}}, "/Body/Graph/PinDefaults"),
               ({"Other": 1}, "/Body/Other")]
        for body, path in bad:
            with self.assertRaises(AssetDocumentContractError, msg=path) as raised:
                scripts.check_script_body(body)
            self.assertEqual(raised.exception.diagnostic.path, path)
        with self.assertRaises(AssetDocumentContractError) as raised:
            scripts.check_pads([{"Name": "P", "Versions": []}], "/Body/ScratchPads")
        self.assertEqual(raised.exception.diagnostic.path, "/Body/ScratchPads/0/Versions")

    def test_profile_applies_and_lists_no_read_only_sections(self) -> None:
        profile = niagara.NiagaraScriptProfile()
        self.assertEqual(profile.inspect_profile()["ApplyAvailable"], True)
        template = profile.create_template(TARGET)
        self.assertEqual(template["Body"], {"Usage": "Module"})
        profile.validate(template)
        document = _document({"Usage": "Module"})
        # The script's other Details (validation rules, preview movie) are generic Properties; the state the Body
        # authors is not.
        document["Properties"] = {"VersionData": []}
        with self.assertRaises(AssetDocumentContractError) as raised:
            profile.validate(document)
        self.assertEqual(raised.exception.diagnostic.code, "Niagara.BodyOwnedProperty")


class ScriptDiffTests(unittest.TestCase):
    def tearDown(self) -> None:
        scripts.set_settings_defaults({})

    def test_extract_equals_itself(self) -> None:
        self.assertEqual(scripts.script_differences(_body(), _body()), [])

    def test_lenient_where_the_read_is_sparse(self) -> None:
        body = _body()
        body["Graph"]["Nodes"][0].pop("Position")
        body["Graph"]["Nodes"][0]["Properties"] = {"bSomething": True}   # not read: the default
        body["Graph"]["PinDefaults"]["Out:Output"] = "1"
        body["Graph"]["Nodes"][0]["Pins"] = {"Outputs": ["Module.Strength"]}
        body["Settings"]["LibraryVisibility"] = "Unexposed"              # not read: the default
        body["Parameters"] = body["Parameters"][:1]                       # unlisted: kept
        scripts.set_settings_defaults({"LibraryVisibility": "Unexposed", "Description": ""})
        with scripts.node_defaults({"Get": {"bSomething": True}}):
            self.assertEqual(scripts.script_differences(body, _body()), [])

    def test_a_value_the_read_leaves_out_is_compared_with_the_default(self) -> None:
        # A Description the module did not have was "unchanged; nothing to apply" and the edit was lost.
        body = _body()
        body["Settings"]["Description"] = "d"
        self.assertEqual([(item.path, item.kind, item.after_present)
                          for item in scripts.script_differences(body, _body())],
                         [("/Body/Settings/Description", DiffKind.REPLACE, False)])
        scripts.set_settings_defaults({"Description": ""})
        self.assertEqual([item.path for item in scripts.script_differences(body, _body())],
                         ["/Body/Settings/Description"])
        body["Settings"]["Description"] = ""                              # the default: nothing to apply
        self.assertEqual(scripts.script_differences(body, _body()), [])

    def test_a_node_property_the_read_leaves_out_is_compared_with_the_default(self) -> None:
        body = _body()
        body["Graph"]["Nodes"][0]["Properties"] = {"bSomething": True}
        # Default unknown (not read): the value differs, as the planner writes it.
        self.assertEqual([item.path for item in scripts.script_differences(body, _body())],
                         ["/Body/Graph/Nodes/0/Properties/bSomething"])
        with scripts.node_defaults({"Get": {"bSomething": False}}):
            self.assertEqual([item.path for item in scripts.script_differences(body, _body())],
                             ["/Body/Graph/Nodes/0/Properties/bSomething"])
        with scripts.node_defaults({"Get": {"bSomething": True}}):
            self.assertEqual(scripts.script_differences(body, _body()), [])

    def test_reports_content_differences(self) -> None:
        body = _body()
        body["Settings"] = {}
        body["Graph"]["Nodes"][1]["Comment"] = ""
        body["Graph"]["Links"] = []
        body["Parameters"][0]["Default"] = 2.0
        body["Parameters"][0].pop("Meta")
        paths = [(item.path, item.kind) for item in scripts.script_differences(body, _body())]
        self.assertEqual(paths, [("/Body/Settings/Category", DiffKind.REMOVE),
                                 ("/Body/Graph/Nodes/1/Comment", DiffKind.REPLACE),
                                 ("/Body/Graph/Links", DiffKind.REMOVE),
                                 ("/Body/Parameters/0/Default", DiffKind.REPLACE),
                                 ("/Body/Parameters/0/Meta", DiffKind.REPLACE)])

    def test_versions(self) -> None:
        live = [{"Number": "1.0", "Guid": GUID_1}, {"Number": "2.0", "Guid": GUID_2, "Exposed": True}]
        self.assertEqual(scripts.version_differences([{"Number": "1.0"}, {"Number": "2.0"}], live, "/V"), [])
        changes = scripts.version_differences([{"Number": "1.0", "Exposed": True}], live, "/V")
        self.assertEqual([(item.path, item.kind) for item in changes],
                         [("/V/0/Exposed", DiffKind.REPLACE), ("/V", DiffKind.REMOVE)])
        op = scripts.versions_op([{"Number": "1.0", "Guid": GUID_1, "Exposed": True}, {"Number": "1.1"}], live)
        self.assertEqual(op["versions"][0], {"number": "1.0", "guid": GUID_1, "exposed": True, "change_description": "",
                                             "deprecated": False, "deprecation_message": "", "visible": True})
        self.assertEqual(op["versions"][1]["number"], "1.1")

    def test_function_call_version(self) -> None:
        live = {"Nodes": [{"Id": "Call", "Action": "Niagara:Function:/Game/S.S", "Position": [0, 0]}]}
        doc = copy.deepcopy(live)
        doc["Nodes"][0]["Version"] = "1.0"   # the exposed one: the read leaves it out
        self.assertEqual(scripts.graph_differences(doc, live, "/G"), [])
        live["Nodes"][0]["Version"] = GUID_1
        self.assertEqual([item.path for item in scripts.graph_differences(doc, live, "/G")], ["/G/Nodes/0/Version"])


class ScriptPlannerTests(unittest.TestCase):
    def test_settings_are_complete(self) -> None:
        op = scripts.settings_op({"Description": "d"}, {"Category": "Forces"}, "/Pad")
        self.assertEqual(op, {"op": "script_settings", "properties": {"Description": "d"}, "reset": ["Category"],
                              "scope": "/Pad"})
        self.assertIsNone(scripts.settings_op({"Category": "Forces"}, {"Category": "Forces"}))

    def test_parameter_ops(self) -> None:
        live = _body()["Parameters"]
        ops = scripts.parameter_ops([{"Name": "Module.Strength", "Default": 2.0},
                                     {"Name": "Particles.Velocity", "DefaultMode": "Binding",
                                      "DefaultBinding": "Particles.Position"}], live, "/Body/Parameters")
        self.assertEqual(ops, [
            ({"op": "script_parameter", "name": "Module.Strength", "default": 2.0,
              "meta": {"properties": {}, "reset": ["Description"]}}, "/Body/Parameters/0"),
            ({"op": "script_parameter", "name": "Particles.Velocity", "default_mode": "Binding",
              "default_binding": "Particles.Position"}, "/Body/Parameters/1")])
        for entry, code in (({"Name": "Module.Missing"}, "Niagara.ParameterNotReferenced"),
                            ({"Name": "Module.Strength", "Type": "int32"}, "Niagara.ParameterType")):
            with self.assertRaises(AssetDocumentContractError) as raised:
                scripts.parameter_ops([entry], live, "/Body/Parameters")
            self.assertEqual(raised.exception.diagnostic.code, code)

    def test_a_parameter_only_a_binding_uses_waits_for_the_binding(self) -> None:
        live = _body()["Parameters"]
        authored = [{"Name": "Particles.Velocity", "DefaultMode": "Binding", "DefaultBinding": "Engine.Owner.Velocity"},
                    {"Name": "Engine.Owner.Velocity", "Meta": {"Description": "bound"}}]
        deferred = scripts.bound_targets(authored)
        self.assertEqual(deferred, {"Engine.Owner.Velocity"})
        first = scripts.parameter_ops(authored, live, "/Body/Parameters", defer=deferred)
        self.assertEqual([where for _, where in first], ["/Body/Parameters/0"])
        later = live + [{"Name": "Engine.Owner.Velocity", "Type": "Vector"}]
        second = scripts.parameter_ops(authored, later, "/Body/Parameters", only=deferred)
        self.assertEqual([where for _, where in second], ["/Body/Parameters/1"])

    def test_exposed_version_rule(self) -> None:
        live = _body(_versioned_rows())
        body = copy.deepcopy(live)
        self.assertFalse(scripts.edits_exposed_version(body, live))
        body["Settings"]["Category"] = "Other"
        self.assertTrue(scripts.edits_exposed_version(body, live))
        body["Versions"] = [{"Number": "1.0", "Guid": GUID_1}, {"Number": "1.1", "Exposed": True}]
        self.assertFalse(scripts.edits_exposed_version(body, live))
        unversioned = _body()
        changed = copy.deepcopy(unversioned)
        changed["Settings"]["Category"] = "Other"
        self.assertFalse(scripts.edits_exposed_version(changed, unversioned))

    def test_scratch_pad_structure(self) -> None:
        live = [{"Name": "A"}, {"Name": "B"}, {"Name": "C"}]
        ops, deletes = scripts.pad_structure([{"Name": "A"}, {"Name": "B2", "RenamedFrom": "B"},
                                              {"Name": "N", "Usage": "DynamicInput"}], live, "/Body/ScratchPads")
        self.assertEqual(ops, [({"op": "rename_scratch_pad", "name": "B", "to": "B2"}, "/Body/ScratchPads/1/Name"),
                               ({"op": "add_scratch_pad", "name": "N", "usage": "DynamicInput"}, "/Body/ScratchPads/2")])
        self.assertEqual(deletes, ["C"])
        scripts.check_emitter_pads([{"Name": "B"}, {"Name": "A"}], live[:2], "/E")
        with self.assertRaises(AssetDocumentContractError) as raised:
            scripts.check_emitter_pads([{"Name": "A"}], live[:2], "/E")
        self.assertEqual(raised.exception.diagnostic.code, "Niagara.ScratchPadScope")

    def test_compile_failure_paths(self) -> None:
        result = {"errors": ["old", "new"], "messages": [
            {"severity": "Error", "message": "old"}, {"severity": "Error", "message": "new", "node": "Out"},
            {"severity": "Warning", "message": "w", "node": "Get"}]}
        self.assertEqual(scripts.compile_failure(["old"], result, ["Get", "Out"]), [("/Body/Graph/Nodes/1", "new")])
        self.assertEqual(scripts.compile_failure(["old", "new"], result, ["Get", "Out"]), [])


class GraphCodecTests(unittest.TestCase):
    def test_decode(self) -> None:
        graph = _body()["Graph"]
        graph["Nodes"].append({"Id": "Call", "Action": "Niagara:Function:/Game/S.S", "Properties": {"bInheritDebugStatus": True}})
        doc = decode_graph(graph, "/Body/Graph", graph_id="Graph", name="NiagaraGraph", guid=None)
        settings = {node.id: thaw_json(node.properties.node) for node in doc.nodes}
        self.assertEqual(settings["Get"], {"Name": "Get", "Pins": {"Outputs": [{"Name": "Module.Strength", "Type": "float"}],
                                                                   "Inputs": []}})
        self.assertEqual(settings["Call"], {"Name": "Call", "Version": "", "Properties/bInheritDebugStatus": True})
        self.assertEqual([node.comment for node in doc.nodes], ["", "Done", ""])
        self.assertEqual([(override.pin, override.value) for override in doc.nodes[1].pin_overrides],
                         [("Output", "1.0")])
        self.assertEqual((doc.links[0].from_.node, doc.links[0].from_.pin), ("Get", "Dest"))
        self.assertTrue(has_version_setting("Niagara:Function:/Game/S.S"))
        self.assertFalse(has_version_setting("Niagara:Function:ScratchPad:Pad"))

    def test_pins_hold(self) -> None:
        live = {"Inputs": [], "Outputs": [{"Name": "A", "Type": "float"}]}
        self.assertTrue(pins_hold(live, {"Outputs": ["A"]}))
        self.assertTrue(pins_hold(live, {"Outputs": [{"Name": "A", "Type": "float"}]}))
        self.assertFalse(pins_hold(live, {"Outputs": [{"Name": "A", "Type": "int32"}]}))
        self.assertFalse(pins_hold(live, {"Inputs": ["B"]}))

    def test_bind_by_unique_action(self) -> None:
        def node(guid, action, name):
            return decode_node({"node_guid": guid, "class": "/Script/NiagaraEditor.X", "action_key": action,
                                "extras": {"object": name}}, "/n")
        g, n1, n2, n3 = (f"{index:032x}" for index in range(4))
        live = LiveGraph(guid=g, name="NiagaraGraph", role="other", path="Graph", schema_tag="Niagara", nodes=(
            node(n1, "Niagara:Output", "NiagaraNodeOutput_0"), node(n2, "Niagara:MapGet", "NiagaraNodeParameterMapGet_0"),
            node(n3, "Niagara:MapGet", "NiagaraNodeParameterMapGet_1")), links=())
        doc = decode_graph({"Nodes": [{"Id": "Out", "Action": "Niagara:Output"},
                                      {"Id": "Get", "Action": "Niagara:MapGet"}]},
                           "/Body/Graph", graph_id="Graph", name="NiagaraGraph", guid=g)
        bound = {node.id: node.guid for node in bind_by_action(doc, live).nodes}
        self.assertEqual(bound, {"Out": n1, "Get": None})
        self.assertEqual(NIAGARA_ADAPTER.live_properties(live.nodes[0], "node"), {"Name": "NiagaraNodeOutput_0"})

    def test_map_get_default_pins_read_under_their_output_name(self) -> None:
        from asset_document.graphs.schemas.niagara import alias_default_pins
        raw = {"graphs": [{"nodes": [{
            "node_guid": "A", "extras": {"default_pins": ["Module.X", "Module.Y"], "custom_default_pins": ["Module.Y"]},
            "pins": [{"name": "Source", "direction": "input"},
                     {"name": "None", "direction": "input", "is_autogenerated_default": False},
                     {"name": "None", "direction": "input", "linked_to": [{"node_guid": "B", "pin_name": "Out"}]},
                     {"name": "Module.X", "direction": "output"}]}],
            "links": [{"from": {"node_guid": "B", "pin_name": "Out"}, "to": {"node_guid": "A", "pin_name": "None"}}]}]}
        graph = alias_default_pins(raw)["graphs"][0]
        pins = graph["nodes"][0]["pins"]
        self.assertEqual([pin["name"] for pin in pins], ["Source", "Module.X", "Module.Y", "Module.X"])
        # The parameter's Default shows on Module.X (not custom): it is never reset by an apply.
        self.assertTrue(pins[1]["is_autogenerated_default"])
        self.assertEqual(graph["links"][0]["to"], {"node_guid": "A", "pin_name": "Module.Y"})

    def test_type_default_pins_of_older_assets_read_as_generated(self) -> None:
        # A pin saved without an autogenerated default holding its type's default is not an authored value.
        from asset_document.graphs.schemas.niagara import alias_default_pins
        raw = {"graphs": [{"nodes": [{
            "node_guid": "A", "extras": {"type_default_pins": ["X if true"]},
            "pins": [{"name": "X if true", "direction": "input", "is_autogenerated_default": False},
                     {"name": "X if false", "direction": "input", "is_autogenerated_default": False},
                     {"name": "X if true", "direction": "output"}]}], "links": []}]}
        pins = alias_default_pins(raw)["graphs"][0]["nodes"][0]["pins"]
        self.assertEqual([pin.get("is_autogenerated_default") for pin in pins], [True, False, None])

    def test_breaking_a_link_of_a_removed_pin_is_skipped(self) -> None:
        # Node settings (a Pins list that drops a pin) run before link breaks; the removed pin took its links along.
        from asset_document.graphs.executor import GraphExecutor
        from asset_document.graphs.reconcile import PlanOp
        from asset_document.graphs.schemas.niagara import NiagaraGraphExecutor

        pin = {"name": "Module.A", "direction": "output", "type": {"category": "float"}}
        nodes = {"g": decode_node({"node_guid": GUID_1.replace("-", ""), "class": "/Script/NiagaraEditor.X",
                                   "pins": [pin]}, "/n"),
                 "s": decode_node({"node_guid": GUID_2.replace("-", ""), "class": "/Script/NiagaraEditor.X",
                                   "pins": [dict(pin, direction="input")]}, "/n")}
        executor = object.__new__(NiagaraGraphExecutor)
        executor.node_guid = lambda graph, ref, op: ref["node"]
        executor.live_node = lambda graph, guid, op: nodes[guid]
        broken = []
        original = GraphExecutor._op_graph_pin_break
        GraphExecutor._op_graph_pin_break = lambda self, op: broken.append(op)
        try:
            op = PlanOp("graph.pin.break", "Graph", "/Body/Graph/Links", {
                "from": {"node": "g", "pin": "Module.A", "direction": "output"},
                "to": {"node": "s", "pin": "Module.A", "direction": "input"}})
            executor._op_graph_pin_break(op)
            self.assertEqual(len(broken), 1)
            nodes["g"] = decode_node({"node_guid": GUID_1.replace("-", ""), "class": "/Script/NiagaraEditor.X"}, "/n")
            executor._op_graph_pin_break(op)
            self.assertEqual(len(broken), 1)
        finally:
            GraphExecutor._op_graph_pin_break = original


class ScriptApplyTests(unittest.TestCase):
    def test_unchanged_apply_edits_nothing(self) -> None:
        runtime = _Runtime(SCRIPT_ROWS)
        self.assertEqual(_apply(runtime, _body()), ())
        self.assertEqual((runtime.edits, runtime.compiled), ([], 0))

    def test_settings_edit_compiles_and_reports_dependents(self) -> None:
        runtime = _Runtime(SCRIPT_ROWS, dependents=["/Game/FX/NS_User"])
        body = _body()
        body["Settings"] = {"Category": "Motion"}
        result = _apply(runtime, body)
        self.assertEqual(runtime.kinds(), ["check_dependents", "script_settings", "update_dependents"])
        self.assertEqual(runtime.compiled, 1)
        self.assertEqual([item.code for item in result.diagnostics], ["Niagara.DependentsUpdated"])

    def test_usage_change_initializes(self) -> None:
        runtime = _Runtime(SCRIPT_ROWS)
        _apply(runtime, {"Usage": "DynamicInput"})
        self.assertEqual(runtime.edits[1], {"op": "initialize_script", "usage": "DynamicInput"})

    def test_exposed_version_needs_the_opt_in(self) -> None:
        runtime = _Runtime(_versioned_rows(), dependents=["/Game/FX/NS_User"])
        body = _body(_versioned_rows())
        body["Settings"] = {"Category": "Motion"}
        with self.assertRaises(AssetDocumentContractError) as raised:
            _apply(runtime, body)
        self.assertEqual(raised.exception.diagnostic.code, "Niagara.ExposedVersionEdit")
        self.assertIn("/Game/FX/NS_User", raised.exception.diagnostic.message)
        self.assertEqual(runtime.kinds(), ["check_dependents"])
        runtime = _Runtime(_versioned_rows())
        _apply(runtime, dict(body, EditExposedVersion=True))
        self.assertIn("script_settings", runtime.kinds())
        runtime = _Runtime(_versioned_rows())
        _apply(runtime, dict(body, Versions=[{"Number": "1.0", "Guid": GUID_1}, {"Number": "2.0", "Exposed": True}]))
        self.assertEqual(runtime.kinds()[:2], ["check_dependents", "versions"])

    def test_new_compile_errors_fail_at_the_node(self) -> None:
        runtime = _Runtime(SCRIPT_ROWS, compile_result={"status": "Error", "errors": ["bad"], "warnings": [],
                                                        "messages": [{"severity": "Error", "message": "bad",
                                                                      "node": "Out"}]})
        body = _body()
        body["Settings"] = {}
        with self.assertRaises(AssetDocumentContractError) as raised:
            _apply(runtime, body)
        self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path),
                         ("Niagara.CompileFailed", "/Body/Graph/Nodes/1"))
        self.assertNotIn("update_dependents", runtime.kinds())


if __name__ == "__main__":
    unittest.main()


class HoldDefaultPinsTests(unittest.TestCase):
    def test_map_get_default_pins_wait_for_the_parameters(self):
        from asset_document.graphs.model import GraphDoc, NodeDoc
        from asset_document.graphs.reconcile import Plan, PlanOp
        from asset_document.graphs.schemas import niagara as schema

        doc = GraphDoc(id="Graph", role=schema.ROLE, name="Graph", schema_tag=schema.TAG,
                       nodes=(NodeDoc(id="Get", action="Niagara:MapGet"), NodeDoc(id="Call", action="Niagara:Call")))
        ops = (PlanOp("graph.pin.connect", "Graph", "/Graph/Links/0", {
                   "from": {"node": "Call", "pin": "Out", "direction": "output"},
                   "to": {"node": "Get", "pin": "Module.Position", "direction": "input"}}),
               PlanOp("graph.pin.connect", "Graph", "/Graph/Links/1", {
                   "from": {"node": "Call", "pin": "Map", "direction": "output"},
                   "to": {"node": "Get", "pin": "Source", "direction": "input"}}),
               PlanOp("graph.pin.default.set", "Graph", "/Graph/Nodes/0/PinOverrides/0", {
                   "pin": {"node": "Get", "pin": "Module.Scale", "direction": "input"}, "kind": "value", "value": "2"}),
               PlanOp("graph.pin.default.set", "Graph", "/Graph/Nodes/1/PinOverrides/0", {
                   "pin": {"node": "Call", "pin": "Scale", "direction": "input"}, "kind": "value", "value": "2"}))
        held = schema._hold_default_pins(Plan(ops, (doc,), {}, {}))
        self.assertEqual([op.path for op in held.ops], ["/Graph/Links/1", "/Graph/Nodes/1/PinOverrides/0"])

    def test_a_nodes_pins_follow_its_details(self):
        from asset_document.graphs.reconcile import Plan, PlanOp
        from asset_document.graphs.schemas import niagara as schema

        def prop(node, key):
            return PlanOp("graph.node.property.set", "Graph", f"/{node}/{key}", {"node": node, "property_path": key})

        ops = (prop("Switch", "Pins"), prop("Switch", "Properties/SwitchTypeData"), prop("Op", "Pins"),
               PlanOp("graph.pin.connect", "Graph", "/Links/0", {}))
        ordered = schema._pins_last(Plan(ops, (), {}, {}))
        self.assertEqual([op.path for op in ordered.ops],
                         ["/Switch/Properties/SwitchTypeData", "/Switch/Pins", "/Op/Pins", "/Links/0"])
