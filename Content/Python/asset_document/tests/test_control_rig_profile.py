"""Control Rig Profile (phases CR0 - CR4): assembling the Body from rig.controlrig.read.v1 records, the paged read
with its token, canonical form and refusals, Diff, and the apply planning (refusals before any write, op batches per
phase including the graph and function phases, error paths, compile outcome). The editor side (read and edit primitives, compile, fidelity,
free edits, rollback, undo) is covered by UEAssetDoc.ControlRig.*."""
from __future__ import annotations

import copy
import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.profiles import control_rig as cr
from asset_document.profiles import control_rig_apply as ap
from asset_document.profiles import control_rig_graph as crg
from asset_document.profiles import control_rig_modular as crm
from asset_document.profiles import ik_common as ik

TARGET = "/Game/Rigs/CR_Test"


def _records():
    rig = ["Graphs", "Rig"]
    function = ["Functions", "Align"]
    collapse = function + ["Nodes", "Inner", "Collapse"]
    return [
        {"kind": "header", "rig_type": "Rig", "parent_class": cr.RIG_PARENT, "counts": {"nodes": 5},
         "facts": {"shapes": ["Circle"], "public_functions": ["Align"], "events": ["Forwards Solve"]}},
        {"kind": "variable", "entry": {"Name": "Weight", "Type": "float", "Default": 1.0, "Public": True}},
        {"kind": "element", "entry": {"Type": "Bone", "Name": "root", "Imported": True}, "global": {}},
        {"kind": "element", "entry": {"Type": "Control", "Name": "root_ctrl", "Parent": "Bone(root)",
                                      "Settings": {"ControlType": "EulerTransform"}}},
        {"kind": "graph", "name": "Rig"},
        {"kind": "node", "scope": rig, "name": "BeginExecution",
         "entry": {"Unit": "RigUnit_BeginExecution", "Position": [0, 0]},
         "pins": [{"Name": "ExecutePin", "Type": "/Script/RigVM.RigVMExecuteContext", "Direction": "Output"}]},
        {"kind": "node", "scope": rig, "name": "Call", "entry": {"Function": "Align", "Position": [300, 0],
                                                                 "Inputs": {"ExecutePin": "BeginExecution.ExecutePin"}}},
        {"kind": "function", "name": "Align", "entry": {
            "Public": True, "Inputs": [{"Name": "ExecuteContext", "Type": "/Script/RigVM.RigVMExecuteContext",
                                        "Direction": "IO"}], "Outputs": []}},
        {"kind": "interface", "scope": function, "name": "Entry", "entry": {"Position": [-400, 0]}},
        {"kind": "node", "scope": function, "name": "Inner", "entry": {"Collapse": {"Inputs": [], "Outputs": []},
                                                                      "Position": [0, 0]}},
        {"kind": "interface", "scope": collapse, "name": "Entry", "entry": {"Position": [-200, 0]}},
        {"kind": "node", "scope": collapse, "name": "Add", "entry": {"Template": "Add::Execute(in A,in B,out Result)",
                                                                    "Position": [0, 0]}},
        {"kind": "interface", "scope": collapse, "name": "Return", "entry": {"Position": [200, 0]}},
        {"kind": "interface", "scope": function, "name": "Return", "entry": {"Position": [400, 0]}},
    ]


def _body():
    body, _ = cr.assemble(_records())
    return body


def _document(body, properties=None):
    return {"SchemaVersion": 1, "Class": cr.CLASS, "Action": "Update", "Target": TARGET,
            "Properties": properties or {}, "Body": body}


class AssembleTests(unittest.TestCase):
    def test_body_from_records(self):
        body, facts = cr.assemble(_records())
        self.assertNotIn("RigType", body)
        self.assertNotIn("ParentClass", body)
        self.assertEqual(body["Variables"], [{"Name": "Weight", "Type": "float", "Default": 1.0, "Public": True}])
        self.assertEqual([element["Name"] for element in body["Hierarchy"]], ["root", "root_ctrl"])
        self.assertEqual(list(body["Graphs"]["Rig"]["Nodes"]), ["BeginExecution", "Call"])
        self.assertEqual(body["Graphs"]["Rig"]["Order"], ["BeginExecution", "Call"])
        function = body["Functions"]["Align"]
        self.assertEqual(function["Entry"], {"Position": [-400, 0]})
        self.assertEqual(function["Return"], {"Position": [400, 0]})
        inner = function["Nodes"]["Inner"]["Collapse"]
        self.assertEqual(inner["Entry"], {"Position": [-200, 0]})
        self.assertEqual(list(inner["Nodes"]), ["Add"])
        self.assertEqual(list(facts["pins"]), ["/Body/Graphs/Rig/Nodes/BeginExecution"])
        self.assertEqual(facts["globals"], {"Bone(root)": {}})

    def test_module_and_modular(self):
        body, _ = cr.assemble([{"kind": "header", "rig_type": "Module", "parent_class": cr.RIG_PARENT,
                                "module": {"Category": "Limbs"}}])
        self.assertEqual(body, {"RigType": "Module", "Module": {"Category": "Limbs"}, "Variables": [], "Hierarchy": [],
                                "Graphs": {}, "Functions": {}})
        body, _ = cr.assemble([
            {"kind": "header", "rig_type": "Modular", "parent_class": cr.MODULAR_PARENT, "modular_settings": {}},
            {"kind": "module", "entry": {"Name": "Root", "Class": "/ControlRig/Modules/Modules56/Root.Root_C"}},
            {"kind": "connections", "entries": {"Root/Root": "Bone(root)"}}])
        self.assertEqual(body, {"RigType": "Modular", "Variables": [], "ModularSettings": {},
                                "Modules": [{"Name": "Root", "Class": "/ControlRig/Modules/Modules56/Root.Root_C"}],
                                "Connections": {"Root/Root": "Bone(root)"}})

    def test_custom_parent_class(self):
        body, _ = cr.assemble([{"kind": "header", "rig_type": "Rig", "parent_class": "/Script/Game.MyRig"}])
        self.assertEqual(body["ParentClass"], "/Script/Game.MyRig")


class FakeCalls:
    def __init__(self, pages):
        self.pages = pages
        self.values = []

    def query(self, primitive, values=None, path="/Body"):
        self.values.append((primitive, dict(values or {})))
        return self.pages[len(self.values) - 1]


class ReadTests(unittest.TestCase):
    def test_pages_carry_the_token(self):
        records = _records()
        calls = FakeCalls([{"records": records[:3], "next_offset": 3, "token": "T"},
                           {"records": records[3:], "next_offset": -1, "token": "T"}])
        self.assertEqual(cr.read_records(calls, "inspect"), records)
        self.assertEqual(calls.values, [(ik.CONTROLRIG_READ, {"offset": 0, "detail": "inspect"}),
                                        (ik.CONTROLRIG_READ, {"offset": 3, "detail": "inspect", "token": "T"})])

    def test_no_header(self):
        with self.assertRaises(AssetDocumentContractError) as caught:
            cr.read_records(FakeCalls([{"records": [], "next_offset": -1}]))
        self.assertEqual(caught.exception.diagnostic.code, "ControlRig.InvalidPrimitiveResult")


class CanonicalTests(unittest.TestCase):
    def test_extract_shape_is_canonical(self):
        canonical = cr.canonicalize(_document(_body()))
        self.assertEqual(canonical["Body"], _body())

    def _refused(self, body, code, path):
        with self.assertRaises(AssetDocumentContractError) as caught:
            cr.canonicalize(_document(body))
        diagnostic = caught.exception.diagnostic
        self.assertEqual((diagnostic.code, diagnostic.path), (code, path))

    def test_refusals(self):
        self._refused({"RigType": "Puppet"}, "ControlRig.InvalidBody", "/Body/RigType")
        self._refused({"Unknown": 1}, "ControlRig.UnknownBodyKey", "/Body/Unknown")
        self._refused({"RigType": "Modular", "Graphs": {}}, "ControlRig.ModularRigRegionUnsupported", "/Body/Graphs")
        # With RigType; without it the rig's own type decides (control_rig_apply.plan, ControlRigPlanRegressionTests).
        self._refused({"RigType": "Rig", "Modules": []}, "ControlRig.ModularRegionUnsupported", "/Body/Modules")
        self._refused({"RigType": "Rig", "Module": {}}, "ControlRig.InvalidBody", "/Body/Module")
        self._refused({"Hierarchy": {}}, "ControlRig.InvalidBody", "/Body/Hierarchy")
        self._refused({"Variables": [{"Name": "A"}]}, "ControlRig.InvalidBody", "/Body/Variables/0")
        self._refused({"Graphs": {"Rig": {"Nodes": {"A": {"Collapse": {"Nodes": []}}}}}}, "ControlRig.InvalidBody",
                      "/Body/Graphs/Rig/Nodes/A/Collapse/Nodes")


class DiffTests(unittest.TestCase):
    def _diff(self, authored_body, exact=False):
        return cr.diff(cr.canonicalize(_document(authored_body)), cr.canonicalize(_document(_body())), exact)

    def test_self_diff_is_empty(self):
        self.assertEqual(self._diff(_body()), ())
        self.assertEqual(self._diff(_body(), exact=True), ())

    def test_omitted_regions_keep(self):
        self.assertEqual(self._diff({}), ())

    def test_node_and_element_changes(self):
        body = _body()
        body["Graphs"]["Rig"]["Nodes"]["Call"]["Position"] = [310, 0]
        body["Functions"]["Align"]["Nodes"]["Inner"]["Collapse"]["Nodes"]["Add"]["Defaults"] = {"A": 1.0}
        del body["Variables"][0]["Public"]
        paths = sorted(change.path for change in self._diff(body))
        self.assertEqual(paths, ["/Body/Functions/Align/Nodes/Inner/Collapse/Nodes/Add/Defaults/A",
                                 "/Body/Graphs/Rig/Nodes/Call/Position", "/Body/Variables/Weight/Public"])

    def test_hierarchy_order_is_sibling_order(self):
        root = {"Type": "Bone", "Name": "root"}
        a = {"Type": "Null", "Name": "a", "Parent": "Bone(root)"}
        b = {"Type": "Null", "Name": "b", "Parent": "Bone(root)"}
        child = {"Type": "Null", "Name": "c", "Parent": "Null(a)"}
        live = {"Hierarchy": [root, a, child, b]}   # depth first
        self.assertEqual(cr.body_diff({"Hierarchy": [root, a, b, child]}, live, False), [])
        self.assertEqual([change.path for change in cr.body_diff({"Hierarchy": [root, b, a, child]}, live, False)],
                         ["/Body/Hierarchy"])

    def test_defaults_compare_as_omitted(self):
        body = _body()
        body["Graphs"]["Rig"]["Nodes"]["BeginExecution"]["Defaults"] = {}
        body["Variables"][0]["ReadOnly"] = False
        self.assertEqual(self._diff(body), ())


def _code(call):
    try:
        call()
    except AssetDocumentContractError as caught:
        return caught.diagnostic.code, caught.diagnostic.path
    return None


def _ops(items):
    return [item.op for item in items]


EMPTY = {"Graphs": {"Rig": {"Nodes": {}}}, "Functions": {}}
NULL = {"Type": "Null", "Name": "n", "Parent": "Bone(root)"}
ROOT = {"Type": "Bone", "Name": "root", "Imported": True}


class PlanTests(unittest.TestCase):
    def test_forwards_solve_on_a_pristine_rig(self):
        self.assertEqual(ap.plan({}, EMPTY, True), (True, None))
        self.assertEqual(ap.plan({"Graphs": {"Rig": {"Nodes": {}}}}, EMPTY, True), (False, None))
        self.assertEqual(ap.plan({}, _body(), False), (False, None))
        self.assertTrue(ap.fresh(EMPTY))
        self.assertFalse(ap.fresh(dict(EMPTY, Variables=[{"Name": "A", "Type": "float"}])))
        self.assertTrue(ap.pristine(dict(EMPTY, Hierarchy=[ROOT])))

    def test_function_content_is_authorable(self):
        body = _body()
        body["Graphs"]["Rig"]["Nodes"]["Call"]["Position"] = [310, 0]
        self.assertEqual(ap.plan(body, _body(), False), (False, None))
        body = _body()
        body["Functions"] = {}
        del body["Graphs"]["Rig"]["Nodes"]["Call"]
        self.assertEqual(ap.plan(body, _body(), False), (False, None))
        body = _body()
        body["Functions"]["Moved"] = dict(body["Functions"].pop("Align"), RenamedFrom="Gone")
        self.assertEqual(_code(lambda: ap.plan(body, _body(), False)),
                         ("ControlRig.FunctionNotFound", "/Body/Functions/Moved/RenamedFrom"))

    def test_type_and_class_refusals(self):
        self.assertEqual(_code(lambda: ap.plan({"RigType": "Modular"}, _body(), False))[0], "ControlRig.RigTypeImmutable")
        self.assertEqual(_code(lambda: ap.plan({"ParentClass": "/Script/Game.MyRig"}, _body(), False)),
                         ("ControlRig.ParentClassImmutable", "/Body/ParentClass"))
        self.assertEqual(_code(lambda: ap.plan({"Module": {}}, _body(), False)), ("ControlRig.InvalidBody", "/Body/Module"))
        modular = {"RigType": "Modular", "Variables": [], "Modules": [{"Name": "Root"}], "Connections": {}}
        self.assertEqual(ap.plan(dict(modular), modular, False), (False, None))
        # Modular rigs keep their own variables (module bindings read them); editing them is a normal edit.
        self.assertEqual(ap.plan(dict(modular, Variables=[{"Name": "A", "Type": "float"}]), modular, False), (False, None))
        self.assertEqual(_code(lambda: ap.plan(dict(modular, Modules=[{"Name": "Root"}, {"Name": "Root"}]), modular,
                                               False))[0], "ControlRig.DuplicateModule")

    def test_conversion(self):
        live = dict(_body(), Hierarchy=[ROOT, NULL])
        self.assertEqual(ap.plan({"RigType": "Module", "Hierarchy": [ROOT]}, live, False), (False, "rig.to_module"))
        self.assertEqual(_code(lambda: ap.plan({"RigType": "Module"}, live, False)),
                         ("ControlRig.ConversionRefused", "/Body/RigType"))
        self.assertEqual(ap.plan({"RigType": "Rig"}, dict(_body(), RigType="Module"), False), (False, "rig.to_standalone"))
        removes = ap.phase_element_removes({"Hierarchy": [ROOT]}, live, keep=lambda entry: not ap._blocks_module(entry))
        self.assertEqual(_ops(removes), [{"op": "element.remove", "key": "Null(n)"}])


class CheckTests(unittest.TestCase):
    def test_hierarchy(self):
        ap.check_hierarchy([ROOT, NULL], set())
        self.assertEqual(_code(lambda: ap.check_hierarchy([ROOT, NULL, NULL], set())),
                         ("ControlRig.DuplicateElement", "/Body/Hierarchy/2/Name"))
        self.assertEqual(_code(lambda: ap.check_hierarchy([NULL], set())),
                         ("ControlRig.ElementNotFound", "/Body/Hierarchy/0/Parent"))
        self.assertEqual(_code(lambda: ap.check_hierarchy([NULL, ROOT], set())),
                         ("ControlRig.ParentAfterChild", "/Body/Hierarchy/0/Parent"))
        self.assertEqual(_code(lambda: ap.check_hierarchy([{"Type": "Reference", "Name": "r"}], set())),
                         ("ControlRig.ProceduralElement", "/Body/Hierarchy/0/Type"))
        renamed = dict(NULL, RenamedFrom="old")
        self.assertEqual(_code(lambda: ap.check_hierarchy([ROOT, renamed], {"Bone(root)"})),
                         ("ControlRig.ElementNotFound", "/Body/Hierarchy/1/RenamedFrom"))
        ap.check_hierarchy([ROOT, renamed], {"Bone(root)", "Null(old)"})
        spaced = {"Type": "Control", "Name": "c", "Parent": "Bone(root)", "Spaces": [{"Space": "Null(x)"}]}
        self.assertEqual(_code(lambda: ap.check_hierarchy([ROOT, spaced], set())),
                         ("ControlRig.ElementNotFound", "/Body/Hierarchy/1/Spaces/0/Space"))

    def test_variables(self):
        self.assertEqual(_code(lambda: ap.check_variables([{"Name": "A"}, {"Name": "A"}])),
                         ("ControlRig.DuplicateVariable", "/Body/Variables/1/Name"))


class PhaseTests(unittest.TestCase):
    LIVE = {"Variables": [{"Name": "Weight", "Type": "float", "Default": 1.0},
                          {"Name": "Old", "Type": "bool"},
                          {"Name": "Target", "Type": "/Script/CoreUObject.Vector"}],
            "Hierarchy": [ROOT, NULL, {"Type": "Null", "Name": "m", "Parent": "Bone(root)"}]}

    def test_variables(self):
        body = {"Variables": [{"Name": "Gain", "Type": "float", "Default": 1.0, "RenamedFrom": "Weight"},
                              {"Name": "Target", "Type": "/Script/CoreUObject.Rotator"},
                              {"Name": "New", "Type": "int32", "Category": "Motion", "Default": 3}]}
        self.assertEqual(_ops(ap.phase_variables(body, self.LIVE)), [
            {"op": "variable.rename", "name": "Weight", "to": "Gain"},
            {"op": "variable.remove", "name": "Old"},
            {"op": "variable.type", "name": "Target", "type": "/Script/CoreUObject.Rotator"},
            {"op": "variable.add", "name": "New", "type": "int32"}])
        after = {"Variables": [{"Name": "Gain", "Type": "float", "Default": 1.0},
                               {"Name": "Target", "Type": "/Script/CoreUObject.Rotator"},
                               {"Name": "New", "Type": "int32"}]}
        self.assertEqual(_ops(ap.phase_variable_details(body, after)),
                         [{"op": "variable.set", "name": "New", "category": "Motion", "default": 3}])
        reordered = {"Variables": list(reversed(after["Variables"]))}
        self.assertEqual(_ops(ap.phase_variable_details(reordered, after)),
                         [{"op": "variable.order", "names": ["New", "Target", "Gain"]}])
        reset = {"Variables": [{"Name": "Gain", "Type": "float"}]}
        self.assertEqual(_ops(ap.phase_variable_details(reset, after)),
                         [{"op": "variable.set", "name": "Gain", "default": None}])

    def test_elements(self):
        moved = {"Type": "Null", "Name": "k", "Parents": [{"Parent": "Bone(root)", "Weight": 1.0},
                                                          {"Parent": "Null(n)", "Weight": 0.5}], "RenamedFrom": "m"}
        added = {"Type": "Control", "Name": "c", "Parent": "Null(k)", "Spaces": [{"Space": "Null(n)"}],
                 "Settings": {"ControlType": "Float"}}
        body = {"Hierarchy": [ROOT, moved, NULL, added]}
        self.assertEqual(_ops(ap.phase_element_renames(body, self.LIVE)),
                         [{"op": "element.rename", "key": "Null(m)", "to": "k"}])
        renamed = {"Hierarchy": [ROOT, NULL, {"Type": "Null", "Name": "k", "Parent": "Bone(root)"}]}
        self.assertEqual(_ops(ap.phase_element_removes({"Hierarchy": [ROOT]}, renamed)),
                         [{"op": "element.remove", "key": "Null(k)"}, {"op": "element.remove", "key": "Null(n)"}])
        self.assertEqual(_ops(ap.phase_element_adds(body, renamed)), [{"op": "element.add", "entry": {
            "Type": "Control", "Name": "c", "Parent": "Null(k)", "Settings": {"ControlType": "Float"}}}])
        added_live = {"Hierarchy": renamed["Hierarchy"] + [{"Type": "Control", "Name": "c", "Parent": "Null(k)",
                                                            "Settings": {"ControlType": "Float"}}]}
        sets = _ops(ap.phase_element_sets(body, added_live))
        self.assertEqual([op["entry"]["Name"] for op in sets], ["k", "c"])
        self.assertNotIn("RenamedFrom", sets[0]["entry"])
        self.assertEqual(_ops(ap.phase_element_order(body, added_live)),
                         [{"op": "element.order", "keys": ["Null(k)", "Null(n)"]}])

    def test_error_paths(self):
        items = [ap.Op({}, "/Body/Variables/0", {"to": "/Body/Variables/0/Name"}),
                 ap.Op({}, "/Body/Hierarchy/3", nested={"entry": "/Body/Hierarchy/3"})]
        self.assertEqual(ap.op_path("/ops/0/to", items), "/Body/Variables/0/Name")
        self.assertEqual(ap.op_path("/ops/0/name", items), "/Body/Variables/0")
        self.assertEqual(ap.op_path("/ops/1/entry/Settings/ShapeName", items), "/Body/Hierarchy/3/Settings/ShapeName")
        self.assertEqual(ap.op_path("/ops/7", items), "/Body")
        self.assertEqual(ap.op_path("", items), "/Body")


def _graph(nodes, name="Rig"):
    return {"Graphs": {name: {"Nodes": nodes}}}


BEGIN = {"Unit": "RigUnit_BeginExecution", "Position": [0, 0]}
SET = {"Unit": "RigUnit_SetTransform", "Position": [300, 0], "Inputs": {"ExecuteContext": "Begin.ExecuteContext"},
       "Defaults": {"Item": "Bone(root)", "Weight": 0.5}}


def _phase(name, body, live, deprecated=False):
    return _ops(crg.phase(name, body, live, ap.Op, deprecated=deprecated))


class GraphPlanTests(unittest.TestCase):
    def test_new_nodes(self):
        body = _graph({"Begin": BEGIN, "Set": SET})
        live = _graph({})
        ops = _phase("structure", body, live)
        self.assertEqual(ops, [
            {"op": "node.add", "graph": "Rig", "name": "Begin", "entry": {"Unit": "RigUnit_BeginExecution",
                                                                          "Position": [0, 0]}},
            {"op": "node.add", "graph": "Rig", "name": "Set", "entry": {
                "Unit": "RigUnit_SetTransform", "Position": [300, 0], "Defaults": {"Item": "Bone(root)", "Weight": 0.5}}}])
        self.assertEqual(_phase("structure", body, live, deprecated=True)[0]["deprecated"], "warn")
        after = _graph({"Begin": {"Unit": "RigUnit_BeginExecution", "Position": [0, 0]},
                        "Set": {"Unit": "RigUnit_SetTransform", "Position": [300, 0],
                                "Defaults": {"Item": "Bone(root)", "Weight": 0.5}}})
        self.assertEqual(_phase("wiring", body, after), [
            {"op": "link.add", "graph": "Rig", "from": "Begin.ExecuteContext", "to": "Set.ExecuteContext"}])
        self.assertEqual(_phase("values", body, after), [])
        self.assertEqual(crg.differences(body, _graph({"Begin": BEGIN, "Set": SET})), ())

    def test_new_nodes_follow_the_order(self):
        body = _graph({"A": {"Unit": "U"}, "B": {"Unit": "U"}, "C": {"Unit": "U"}, "Begin": BEGIN})
        body["Graphs"]["Rig"]["Order"] = ["Begin", "C", "A"]
        self.assertEqual([op["name"] for op in _phase("structure", body, _graph({"Begin": BEGIN}))], ["C", "A", "B"])
        # Existing nodes keep their place: an Order that moves them is not compared (no difference), it is reported
        # (order_kept: ControlRig.OrderKept) with the rig's order.
        live = _graph({"Begin": BEGIN, "A": {"Unit": "U"}, "B": {"Unit": "U"}, "C": {"Unit": "U"}})
        live["Graphs"]["Rig"]["Order"] = ["Begin", "C", "A", "B"]
        self.assertEqual(crg.differences(body, live), ())
        self.assertEqual(cr.order_kept(body, live), [])
        live["Graphs"]["Rig"]["Order"] = ["Begin", "A", "B", "C"]
        self.assertEqual(crg.differences(body, live), ())
        self.assertEqual(cr.diff(_document(body), _document(live), True), ())
        self.assertEqual([(item.path, item.after) for item in cr.order_kept(body, live)],
                         [("/Body/Graphs/Rig/Order", ["Begin", "A", "C"])])
        # In a collapse graph too.
        nested = _graph({"Begin": BEGIN, "Box": {"Collapse": {"Nodes": {"X": {"Unit": "U"}, "Y": {"Unit": "U"}},
                                                               "Order": ["Y", "X"]}}})
        nested_live = _graph({"Begin": BEGIN, "Box": {"Collapse": {"Nodes": {"X": {"Unit": "U"}, "Y": {"Unit": "U"}},
                                                                    "Order": ["X", "Y"]}}})
        self.assertEqual([item.path for item in cr.order_kept(nested, nested_live)],
                         ["/Body/Graphs/Rig/Nodes/Box/Collapse/Order"])
        self.assertEqual(crg.differences(_graph({"Begin": BEGIN, "A": {"Unit": "U"}, "B": {"Unit": "U"},
                                                 "C": {"Unit": "U"}}), live), ())

    def test_new_graphs_follow_the_graph_order(self):
        body = _graph({"Begin": BEGIN})
        body["Graphs"].update({"Construction": {"Nodes": {}}, "Backwards": {"Nodes": {}}})
        body["GraphOrder"] = ["Rig", "Construction", "Backwards"]
        ops = _phase("structure", body, _graph({"Begin": BEGIN}))
        self.assertEqual([op["name"] for op in ops if op["op"] == "graph.add"], ["Construction", "Backwards"])
        live = _graph({"Begin": BEGIN})
        live["Graphs"].update({"Backwards": {"Nodes": {}}, "Construction": {"Nodes": {}}})
        live["GraphOrder"] = ["Rig", "Backwards", "Construction"]
        self.assertEqual(crg.differences(body, live), ())
        self.assertEqual([item.path for item in cr.order_kept(body, live)], ["/Body/GraphOrder"])
        live["GraphOrder"] = ["Rig", "Construction", "Backwards"]
        self.assertEqual(cr.order_kept(body, live), [])
        records = [{"kind": "header", "rig_type": "Rig", "parent_class": cr.RIG_PARENT}] + [
            {"kind": "graph", "name": name} for name in ("Rig", "Construction", "Backwards")]
        self.assertEqual(cr.assemble(records)[0]["GraphOrder"], ["Rig", "Construction", "Backwards"])
        self.assertNotIn("GraphOrder", cr.assemble(records[:2])[0])
        bad = {"Graphs": {"Rig": {"Nodes": {}}}, "GraphOrder": ["Rig", "Gone"]}
        with self.assertRaises(Exception) as raised:
            cr.validate_body(bad)
        self.assertIn("GraphOrder/1", str(getattr(raised.exception, "path", raised.exception)) + repr(raised.exception))

    def test_order_refusals(self):
        def check(order):
            body = _graph({"A": {"Unit": "U"}, "B": {"Unit": "U"}})
            body["Graphs"]["Rig"]["Order"] = order
            return _code(lambda: crg.check(body, _graph({})))
        valid = _graph({"A": {"Unit": "U"}, "B": {"Unit": "U"}})
        valid["Graphs"]["Rig"]["Order"] = ["B", "Gone", "A"]
        crg.check(valid, _graph({}))
        self.assertEqual(check(["A", "A"]), ("ControlRig.InvalidBody", "/Body/Graphs/Rig/Order"))
        self.assertEqual(check("A"), ("ControlRig.InvalidBody", "/Body/Graphs/Rig/Order"))

    def test_unpositioned_nodes_are_laid_out(self):
        ops = _phase("structure", _graph({"Begin": BEGIN, "A": {"Unit": "RigUnit_X"}, "B": {"Unit": "RigUnit_Y"}}),
                     _graph({"Begin": BEGIN}))
        self.assertEqual([op["entry"]["Position"] for op in ops], [[400.0, 0.0], [400.0, 150.0]])

    def test_structure_renames_removes_and_remakes(self):
        live = {"Graphs": {"Rig": {"Nodes": {"Begin": BEGIN, "Old": SET, "Gone": {"Unit": "RigUnit_X"},
                                             "Kind": {"Unit": "RigUnit_X"}}},
                           "Events": {"Nodes": {}}, "Spare": {"Nodes": {}}}}
        body = {"Graphs": {"Rig": {"Nodes": {"Begin": BEGIN, "New": dict(SET, RenamedFrom="Old"),
                                             "Kind": {"Unit": "RigUnit_Y"}}},
                           "Setup": {"RenamedFrom": "Events", "Nodes": {}}, "Extra": {"Nodes": {}}}}
        ops = _phase("structure", body, live)
        self.assertEqual([(op["op"], op.get("name"), op.get("to")) for op in ops], [
            ("graph.remove", "Spare", None), ("graph.rename", "Events", "Setup"), ("node.remove", "Gone", None),
            ("node.remove", "Kind", None), ("node.rename", "Old", "New"), ("graph.add", "Extra", None),
            ("node.add", "Kind", None)])

    def test_shape(self):
        live = _graph({"Add": {"Unit": "RigUnit_MathFloatAdd"}, "If": {"Template": "DISPATCH_RigVMDispatch_If"},
                       "T": {"Unit": "RigUnit_X", "Traits": {"Old": {"Type": "/Script/M.A"}}}})
        body = _graph({"Add": {"Unit": "RigUnit_MathFloatAdd", "Aggregate": ["A", "B", "C"]},
                       "If": {"Template": "DISPATCH_RigVMDispatch_If", "PinTypes": {"True": "float"}},
                       "T": {"Unit": "RigUnit_X"}})
        self.assertEqual(_phase("shape", body, live), [
            {"op": "node.aggregate", "graph": "Rig", "name": "Add", "pins": ["A", "B", "C"]},
            {"op": "node.types", "graph": "Rig", "name": "If", "types": {"True": "float"}},
            {"op": "node.traits", "graph": "Rig", "name": "T", "traits": {}}])
        # A resolved pin of another type, or an aggregate node listed without operands: the node is re-made.
        resolved = _graph({"If": {"Template": "DISPATCH_RigVMDispatch_If", "PinTypes": {"True": "int32"}},
                           "Add": {"Unit": "RigUnit_MathFloatAdd", "Aggregate": ["A", "B", "C"]}})
        body = _graph({"If": {"Template": "DISPATCH_RigVMDispatch_If", "PinTypes": {"True": "float"}},
                       "Add": {"Unit": "RigUnit_MathFloatAdd"}})
        self.assertEqual([op["op"] for op in _phase("structure", body, resolved)],
                         ["node.remove", "node.remove", "node.add", "node.add", "node.types"])
        # A new node takes its pin types and operands right after its add (an aggregate node goes last in the model).
        body = _graph({"B": {"Unit": "RigUnit_MathFloatAdd", "Aggregate": ["A", "B", "C"]}, "A": {"Unit": "U"}})
        body["Graphs"]["Rig"]["Order"] = ["B", "A"]
        self.assertEqual([(op["op"], op["name"]) for op in _phase("structure", body, _graph({}))],
                         [("node.add", "B"), ("node.aggregate", "B"), ("node.add", "A")])

    def test_injected_units(self):
        debug = {"Unit": "RigUnit_VisualDebugVectorItemSpace", "Input": "Value", "Output": "Value"}
        named = dict(debug, Name="RigUnit_VisualDebugVectorItemSpace_1", Defaults={"Scale": 2.0})
        live = _graph({"Trace": {"Unit": "RigUnit_SphereTraceByTraceChannel", "Injections": {"Start": [named]}}})
        # A spec without Name / Method compares with the live unit's other fields.
        same = _graph({"Trace": {"Unit": "RigUnit_SphereTraceByTraceChannel",
                                 "Injections": {"Start": [dict(debug, Defaults={"Scale": 2.0})]}}})
        self.assertEqual(_phase("shape", same, live), [])
        self.assertEqual(crg.differences(same, live), ())
        changed = _graph({"Trace": {"Unit": "RigUnit_SphereTraceByTraceChannel", "Injections": {"End": [debug]}}})
        self.assertEqual(_phase("shape", changed, live), [
            {"op": "node.injections", "graph": "Rig", "name": "Trace", "injections": {"End": [debug]}}])
        removed = _graph({"Trace": {"Unit": "RigUnit_SphereTraceByTraceChannel"}})
        self.assertEqual(_phase("shape", removed, live)[0]["injections"], {})
        bad = _graph({"Trace": {"Unit": "RigUnit_SphereTraceByTraceChannel", "Injections": {"Start": [{"Unit": "X"}]}}})
        self.assertEqual(_code(lambda: crg.check(bad, _graph({}))),
                         ("ControlRig.InvalidBody", "/Body/Graphs/Rig/Nodes/Trace/Injections/Start"))

    def test_wiring(self):
        live = _graph({"A": {"Unit": "U", "Inputs": {"X": "B.Out", "Y": "B.Out"}, "Bindings": {"Z": "Old"},
                             "Defaults": {"W": 1.0, "V": {"X": 1.0}}},
                       "B": {"Unit": "U"}})
        body = _graph({"A": {"Unit": "U", "Inputs": {"X": "B.Out", "Y.X": "B.Out2", "Items.0": "B.Out"},
                             "Bindings": {"Z": "New"}, "Defaults": {"V.Y": 2.0, "Items.2": 3.0}},
                       "B": {"Unit": "U"}})
        self.assertEqual([(op["op"], op.get("pin") or op.get("to"), op.get("value", op.get("variable", op.get("size"))))
                          for op in _phase("wiring", body, live)], [
            ("link.break", "A.Y", None), ("pin.bind", "A.Z", ""), ("pin.default", "A.W", None),
            ("pin.default", "A.V", None), ("pin.array", "A.Items", 3), ("link.add", "A.Y.X", None),
            ("link.add", "A.Items.0", None), ("pin.bind", "A.Z", "New")])

    def test_values_and_layout(self):
        live = _graph({"N": {"Unit": "U", "Position": [0, 0], "Defaults": {"A": 1.0}, "Expanded": ["P"]},
                       "C": {"Comment": "old", "Position": [0, 0], "Size": [400, 300],
                             "Color": {"R": 1, "G": 1, "B": 1, "A": 1}}})
        body = _graph({"N": {"Unit": "U", "Position": [10, 0], "Defaults": {"A": 2.0}, "Expanded": ["Q"]},
                       "C": {"Comment": "new", "FontSize": 24}})
        self.assertEqual(_phase("values", body, live), [
            {"op": "pin.default", "graph": "Rig", "pin": "N.A", "value": 2.0},
            {"op": "node.set", "graph": "Rig", "name": "N", "position": [10, 0], "expanded": {"Q": True, "P": False}},
            {"op": "node.set", "graph": "Rig", "name": "C", "comment": "new", "font_size": 24}])

    def test_keep_fields_compare_as_omitted(self):
        live = _graph({"N": {"Unit": "U", "Position": [5, 5], "Expanded": ["P"], "PinTypes": {"A": "float"}}})
        self.assertEqual(crg.differences(_graph({"N": {"Unit": "U"}}), live), ())
        self.assertTrue(crg.differences(_graph({"N": {"Unit": "U", "Position": [0, 0]}}), live))

    def test_null_layout_and_display_fields_are_refused(self):
        # node.set Position / Size / Title / Color and function.set Color have no "reset to default" in the
        # editor; a null was dropped without a word. It is refused at the document path.
        live = _graph({"C": {"Comment": "c", "Position": [0, 0], "Size": [400, 300], "Title": "",
                             "Color": {"R": 1, "G": 1, "B": 1, "A": 1}}})
        for key in ("Position", "Size", "Title", "Color"):
            body = _graph({"C": {"Comment": "c", key: None}})
            self.assertEqual(_code(lambda: crg.check(body, live)),
                             ("ControlRig.InvalidBody", f"/Body/Graphs/Rig/Nodes/C/{key}"), key)
        self.assertEqual(_code(lambda: crg.check({"Functions": {"F": {"Color": None}}}, {"Functions": {"F": {}}})),
                         ("ControlRig.InvalidBody", "/Body/Functions/F/Color"))
        self.assertEqual(_code(lambda: crg.check(_graph({"C": {"Comment": "c", "Color": {"R": "1"}}}), live)),
                         ("ControlRig.InvalidBody", "/Body/Graphs/Rig/Nodes/C/Color"))
        crg.check(_graph({"C": {"Comment": "c", "Title": "T", "Color": {"R": 0.5, "G": 0, "B": 0, "A": 1}}}), live)

    def test_refusals(self):
        def check(body, live=None):
            return _code(lambda: crg.check(body, live or _graph({})))
        self.assertEqual(check(_graph({"N": {"Unit": "U", "Template": "T"}})),
                         ("ControlRig.InvalidBody", "/Body/Graphs/Rig/Nodes/N"))
        self.assertEqual(check(_graph({"N": {"Unit": "U", "Pins": {}}})),
                         ("ControlRig.InvalidBody", "/Body/Graphs/Rig/Nodes/N/Pins"))
        self.assertEqual(check(_graph({"N": {"Unit": "U", "Inputs": {"A": "Nope.Out"}}})),
                         ("ControlRig.NodeNotFound", "/Body/Graphs/Rig/Nodes/N/Inputs/A"))
        self.assertEqual(check(_graph({"N": {"Unit": "U", "Inputs": {"A": "N.Out"}, "Defaults": {"A": 1}}})),
                         ("ControlRig.InvalidBody", "/Body/Graphs/Rig/Nodes/N/Defaults/A"))
        self.assertEqual(check(_graph({"R": {"Reroute": "float"}})),
                         ("ControlRig.UnstableReroute", "/Body/Graphs/Rig/Nodes/R/Inputs"))
        self.assertEqual(check(_graph({"R": {"Reroute": "/Script/CoreUObject.Vector", "Inputs": {"Value.X": "N.Out"}},
                                       "N": {"Unit": "U"}}))[0], "ControlRig.UnstableReroute")
        self.assertEqual(check(_graph({"R": {"Reroute": "float", "Inputs": {"Value": "N.Out"}}, "N": {"Unit": "U"},
                                       "M": {"Unit": "U", "Inputs": {"A": "R.Value.X"}}}))[0],
                         "ControlRig.UnstableReroute")
        crg.check(_graph({"R": {"Reroute": "/Script/RigVM.RigVMExecuteContext"}}), _graph({}))
        crg.check(_graph({"R": {"Reroute": "float", "Inputs": {"Value": "N.Out"}}, "N": {"Unit": "U"}}), _graph({}))
        self.assertEqual(check(_graph({}, "Other"), _graph({})), ("ControlRig.InvalidBody", "/Body/Graphs"))
        self.assertEqual(check(_graph({"N": {"Unit": "U", "RenamedFrom": "Gone"}})),
                         ("ControlRig.NodeNotFound", "/Body/Graphs/Rig/Nodes/N/RenamedFrom"))
        crg.check(_graph({"N": {"Function": "F"}}), _graph({}))
        self.assertEqual(check(_graph({"N": {"Comment": "", "VariableMap": {}}})),
                         ("ControlRig.InvalidBody", "/Body/Graphs/Rig/Nodes/N/VariableMap"))
        self.assertEqual(check({"Functions": {"F": {"Inputs": [{"Name": "A", "Type": "float", "Direction": "Output"}]}}}),
                         ("ControlRig.InvalidBody", "/Body/Functions/F/Inputs/0/Direction"))
        self.assertEqual(check({"Functions": {"F": {"Nodes": {"Entry": {"Comment": ""}}}}}),
                         ("ControlRig.InvalidBody", "/Body/Functions/F/Nodes/Entry"))
        self.assertEqual(check({"Functions": {"F": {"Nodes": {"N": {"Unit": "U", "Inputs": {"A": "Entry.X"}}}}}}), None)
        collapse = {"Collapse": {"Inputs": [{"Name": "A", "Type": "float", "Default": 1.0}]}}
        self.assertEqual(check(_graph({"C": collapse})),
                         ("ControlRig.InvalidBody", "/Body/Graphs/Rig/Nodes/C/Collapse/Inputs/0/Default"))

    def test_notes_become_warnings(self):
        class Calls:
            context = None
            write = None
        edits = ap.Edits(Calls())
        outcome = type("Outcome", (), {"value": {"changed": True, "notes": [
            {"op": 0, "field": "entry/Unit", "code": "ControlRig.DeprecatedUnit", "message": "old"}]}, "changed_set": ()})
        original = ap.common.invoke
        try:
            ap.common.invoke = lambda *args, **kwargs: (kwargs["paths"]("/ops/0/entry/Unit"), outcome)[1]
            edits.run([ap.Op({"op": "node.add"}, "/Body/Graphs/Rig/Nodes/N", nested={"entry": "/Body/Graphs/Rig/Nodes/N"})])
        finally:
            ap.common.invoke = original
        self.assertEqual([(item.path, item.code) for item in edits.warnings],
                         [("/Body/Graphs/Rig/Nodes/N/Unit", "ControlRig.DeprecatedUnit")])


EXEC = {"Name": "ExecuteContext", "Type": "/Script/RigVM.RigVMExecuteContext", "Direction": "IO"}


def _function(**fields):
    return dict({"Inputs": [EXEC], "Outputs": [], "Nodes": {}}, **fields)


class FunctionPlanTests(unittest.TestCase):
    def test_new_function_with_inputs_and_a_local(self):
        body = {"Functions": {"Scale": _function(
            Inputs=[EXEC, {"Name": "A", "Type": "float", "Default": 2.0}, {"Name": "B", "Type": "float"}],
            Outputs=[{"Name": "Result", "Type": "float"}], LocalVariables=[{"Name": "T", "Type": "float", "Default": 1.0}],
            Public=True, Category="Math")}}
        self.assertEqual(_phase("functions", body, {"Functions": {}}),
                         [{"op": "function.add", "name": "Scale", "mutable": True}])
        made = {"Functions": {"Scale": _function()}}
        graph = ["Functions", "Scale"]
        self.assertEqual(_phase("interface", body, made), [
            {"op": "function.set", "graph": graph, "category": "Math", "public": True},
            {"op": "function.pin.add", "graph": graph, "name": "A", "direction": "Input", "type": "float", "default": 2.0},
            {"op": "function.pin.add", "graph": graph, "name": "B", "direction": "Input", "type": "float"},
            {"op": "function.pin.add", "graph": graph, "name": "Result", "direction": "Output", "type": "float"},
            {"op": "local.add", "graph": graph, "name": "T", "type": "float", "default": 1.0}])
        # Not mutable without the editor's ExecuteContext pin first.
        self.assertFalse(_phase("functions", {"Functions": {"Pure": _function(Inputs=[])}}, {"Functions": {}})[0]["mutable"])

    def test_renames_and_removes(self):
        live = {"Functions": {"Old": _function(Inputs=[EXEC, {"Name": "X", "Type": "float"}],
                                               LocalVariables=[{"Name": "L", "Type": "int32"}]),
                              "Gone": _function()}}
        body = {"Functions": {"New": _function(RenamedFrom="Old", Inputs=[EXEC, {"Name": "Y", "Type": "int32",
                                                                                  "RenamedFrom": "X"}],
                                               LocalVariables=[{"Name": "M", "Type": "int32", "RenamedFrom": "L"}])}}
        self.assertEqual(_phase("functions", body, live), [{"op": "function.rename", "name": "Old", "to": "New"}])
        after = {"Functions": {"New": live["Functions"]["Old"], "Gone": live["Functions"]["Gone"]}}
        graph = ["Functions", "New"]
        self.assertEqual(_phase("interface", body, after), [
            {"op": "function.pin.rename", "graph": graph, "name": "X", "to": "Y"},
            {"op": "function.pin.type", "graph": graph, "name": "Y", "type": "int32"},
            {"op": "local.rename", "graph": graph, "name": "L", "to": "M"}])
        self.assertEqual(_phase("cleanup", body, after), [{"op": "function.remove", "name": "Gone"}])
        self.assertEqual(_phase("cleanup", {"Graphs": {}}, after), [])

    def test_collapse_graphs_keep_their_local_variables(self):
        def rig(local_variables):
            return _graph({"Box": {"Position": [0, 0], "Collapse": {"Inputs": [], "Outputs": [], "Nodes": {},
                                                                     "LocalVariables": local_variables}}})
        live = rig([{"Name": "L", "Type": "float"}])
        self.assertIsNone(_code(lambda: _phase("structure", live, live)))
        self.assertEqual(_code(lambda: _phase("structure", rig([]), live)),
                         ("ControlRig.EditRefused", "/Body/Graphs/Rig/Nodes/Box/Collapse/LocalVariables"))
        self.assertEqual(_code(lambda: _phase("structure", rig([{"Name": "L", "Type": "float", "Default": 1.0}]), live)),
                         ("ControlRig.EditRefused", "/Body/Graphs/Rig/Nodes/Box/Collapse/LocalVariables"))

    def test_pin_order_and_defaults(self):
        live = {"Functions": {"F": _function(Inputs=[EXEC, {"Name": "A", "Type": "float", "Default": 1.0},
                                                     {"Name": "B", "Type": "float"}],
                                             Outputs=[{"Name": "R", "Type": "float"}])}}
        body = {"Functions": {"F": _function(Inputs=[EXEC, {"Name": "B", "Type": "float", "Default": 3.0},
                                                     {"Name": "A", "Type": "float"}],
                                             Outputs=[{"Name": "R", "Type": "float"}])}}
        graph = ["Functions", "F"]
        self.assertEqual(_phase("interface", body, live), [
            {"op": "function.pin.index", "graph": graph, "name": "ExecuteContext", "index": 0},
            {"op": "function.pin.index", "graph": graph, "name": "B", "index": 1},
            {"op": "function.pin.index", "graph": graph, "name": "A", "index": 2},
            {"op": "function.pin.index", "graph": graph, "name": "R", "index": 3},
            {"op": "function.pin.default", "graph": graph, "name": "B", "value": 3.0},
            {"op": "function.pin.default", "graph": graph, "name": "A", "value": None}])
        self.assertEqual(_phase("interface", live, live), [])

    def test_dependents_of_public_functions(self):
        live = {"Functions": {"Pub": _function(Public=True), "Priv": _function(), "Kept": _function(Public=True)}}
        body = {"Functions": {"Renamed": _function(Public=True, RenamedFrom="Pub"), "Priv": _function(),
                              "Kept": _function(Public=True)}}
        self.assertEqual(_phase("dependents", body, live), [
            {"op": "function.dependents", "functions": [{"name": "Pub", "change": "renamed to 'Renamed'"}]}])
        body = {"Functions": {"Kept": _function(Public=False, Inputs=[])}}
        self.assertEqual(_phase("dependents", body, live)[0]["functions"], [
            {"name": "Pub", "change": "removed"}, {"name": "Kept", "change": "made private and changed (its pins)"}])

    def test_bodies_and_collapse_graphs(self):
        body = {"Graphs": {"Rig": {"Nodes": {"Call": {"Function": "F", "VariableMap": {"In": "Speed"}},
                                             "Box": {"Collapse": {"Inputs": [{"Name": "V", "Type": "float"}],
                                                                  "Nodes": {"Add": {"Unit": "U", "Inputs": {"A": "Entry.V"}}},
                                                                  "Return": {"Position": [300, 0]}}}}}},
                "Functions": {"F": _function(Nodes={"N": {"Unit": "U"}}, Return={"Inputs": {
                    "ExecuteContext": "N.ExecuteContext"}})}}
        live = {"Graphs": {"Rig": {"Nodes": {}}}, "Functions": {"F": _function(Entry={"Position": [-250, 0]},
                                                                                 Return={"Position": [250, 0]})}}
        planned = set()
        ops = _ops(crg.phase("structure", body, live, ap.Op, planned=planned))
        self.assertEqual([(op["op"], op["graph"], op["name"]) for op in ops], [
            ("node.add", "Rig", "Box"), ("node.add", "Rig", "Call"), ("node.add", ["Functions", "F"], "N")])
        self.assertEqual(ops[0]["entry"]["Collapse"], {})
        # The collapse graph exists after the round: its pins, then its nodes.
        live["Graphs"]["Rig"]["Nodes"] = {"Call": {"Function": "F"}, "Box": {"Collapse": {
            "Inputs": [], "Outputs": [], "Nodes": {}, "Entry": {"Position": [0, 0]}, "Return": {"Position": [0, 0]}}}}
        live["Functions"]["F"]["Nodes"] = {"N": {"Unit": "U"}}
        box = ["Graphs", "Rig", "Nodes", "Box", "Collapse"]
        self.assertEqual(_ops(crg.phase("structure", body, live, ap.Op, planned=planned)), [
            {"op": "function.pin.add", "graph": box, "name": "V", "direction": "Input", "type": "float"},
            {"op": "node.add", "graph": box, "name": "Add", "entry": {"Unit": "U", "Position": [0.0, 0.0]}}])
        self.assertEqual(_ops(crg.phase("structure", body, live, ap.Op, planned=planned)), [])
        live["Graphs"]["Rig"]["Nodes"]["Box"]["Collapse"]["Nodes"] = {"Add": {"Unit": "U"}}
        self.assertEqual(_phase("wiring", body, live), [
            {"op": "link.add", "graph": box, "from": "Entry.V", "to": "Add.A"},
            {"op": "link.add", "graph": ["Functions", "F"], "from": "N.ExecuteContext", "to": "Return.ExecuteContext"}])
        self.assertEqual(_phase("values", body, live), [
            {"op": "node.remap", "graph": "Rig", "name": "Call", "map": {"In": "Speed"}},
            {"op": "node.set", "graph": box, "name": "Return", "position": [300, 0]}])

    def test_diff_keeps_omitted_interface_nodes_and_color(self):
        live = {"Functions": {"F": _function(Entry={"Position": [0, 0]}, Color={"R": 1, "G": 0, "B": 0, "A": 1})}}
        self.assertEqual(crg.differences({"Functions": {"F": _function()}}, live), ())
        self.assertEqual([item.path for item in crg.differences({"Functions": {"F": _function(Entry={"Position": [5, 0]})}},
                                                                live)], ["/Body/Functions/F/Entry/Position"])
        renamed = _function(Inputs=[dict(EXEC, RenamedFrom="Old")])
        self.assertEqual(crg.differences({"Functions": {"F": renamed}}, live), ())


class OutcomeTests(unittest.TestCase):
    OLD = {"Status": "Error", "Messages": [{"Severity": "Error", "Message": "old"}]}

    def test_new_errors_fail(self):
        failure, _ = ap.validation_outcome({"Status": "UpToDate"},
                                           {"Status": "Error", "Messages": [{"Severity": "Error", "Message": "boom"}]})
        self.assertEqual(failure.diagnostic.code, "ControlRig.CompileFailed")
        failure, _ = ap.validation_outcome({"Status": "UpToDate"}, {"Status": "Error"})
        self.assertEqual(failure.diagnostic.code, "ControlRig.CompileFailed")

    def test_old_errors_and_warnings_report(self):
        after = {"Status": "Error", "Messages": self.OLD["Messages"] + [{"Severity": "Warning", "Message": "w"}]}
        failure, warnings = ap.validation_outcome(self.OLD, after)
        self.assertIsNone(failure)
        self.assertEqual([item.code for item in warnings], ["ControlRig.CompileError", "ControlRig.CompileWarning"])
        self.assertEqual(ap.validation_outcome({}, {"Status": "UpToDate"}), (None, ()))


def _make(op, path, fields):
    return (op, path)


ARM = "/ControlRigModules/Modules56/Arm.Arm_C"
LIVE_MODULAR = {"RigType": "Modular", "Modules": [
    {"Name": "Root", "Class": ap.ROOT_MODULE}, {"Name": "Spine", "Class": "/M/Spine.Spine_C", "Parent": "Root",
                                                "Config": {"Body Control Adjustment": 1.5}},
    {"Name": "Arm_l", "Class": ARM, "Parent": "Spine", "Bindings": {"FK IK Default": "UseIK"}},
    {"Name": "Leg_l", "Class": "/M/Leg.Leg_C", "Parent": "Spine"},
    {"Name": "Leg_r", "Class": "/M/Leg.Leg_C", "Parent": "Spine"}],
    "Connections": {"Root/Root": "Bone(root)", "Arm_l/Arm Primary": "Socket(Spine/shoulder_l)",
                    "Arm_l/Twist Bones": ["Bone(a)", "Bone(b)"]}}


def _modules(*names):
    by_name = {entry["Name"]: entry for entry in LIVE_MODULAR["Modules"]}
    return [dict(by_name[name]) for name in names]


class ModularTests(unittest.TestCase):
    def _plan(self, body, live=LIVE_MODULAR):
        crm.check(body, live)
        return {name: [op for op, _ in planner(body, live, _make)] for name, planner in crm.PHASES}

    def test_connect_order(self):
        # Module by module down the tree, each module's primary connector first: Spine's sockets (which Arm_l's primary
        # connector targets) appear once Spine's own connectors resolve.
        modules = [{"Name": "Arm_l", "Parent": "Spine"}, {"Name": "Root"}, {"Name": "Spine", "Parent": "Root"}]
        connections = {"Arm_l/Arm Primary": "Socket(Spine/shoulder_l)", "Arm_l/Hand Bone": "Bone(hand_l)",
                       "Root/Root": "Bone(root)", "Spine/Pelvis Bone": "Bone(pelvis)",
                       "Spine/Spine Primary": "Socket(Root/spine_01_socket)", "Other/X": "Bone(x)"}
        primaries = ["Root/Root", "Spine/Spine Primary", "Arm_l/Arm Primary"]
        self.assertEqual(crm.connect_order(connections, modules, primaries),
                         ["Root/Root", "Spine/Spine Primary", "Spine/Pelvis Bone", "Arm_l/Arm Primary", "Arm_l/Hand Bone",
                          "Other/X"])
        ops = crm.phase_connects({"Modules": modules, "Connections": connections}, {crm.PRIMARIES: primaries}, _make)
        self.assertEqual([op["connector"] for op, _ in ops][:3], ["Root/Root", "Spine/Spine Primary", "Spine/Pelvis Bone"])

    def test_unchanged_plans_nothing(self):
        body = {"Modules": [dict(entry) for entry in LIVE_MODULAR["Modules"]], "Connections": dict(LIVE_MODULAR["Connections"])}
        self.assertTrue(all(not ops for ops in self._plan(body).values()))

    def test_free_edits(self):
        modules = _modules("Root", "Spine", "Leg_r", "Leg_l", "Arm_l")
        modules[1]["Config"] = {"Body Control Adjustment": 2.5, "Enable Stretch": True}
        modules[4]["Bindings"] = {}
        modules[4]["Class"] = "/M/OldArm.OldArm_C"
        modules.append({"Name": "Arm_r", "Class": ARM, "Parent": "Spine"})
        connections = dict(LIVE_MODULAR["Connections"], **{"Arm_r/Arm Primary": "Socket(Spine/shoulder_r)"})
        del connections["Arm_l/Twist Bones"]
        plan = self._plan({"Modules": modules, "Connections": connections})
        self.assertEqual(plan["adds"], [{"op": "module.add", "name": "Arm_r", "class": ARM, "parent": "Spine"}])
        self.assertEqual(plan["swaps"], [{"op": "module.swap", "name": "Arm_l", "class": "/M/OldArm.OldArm_C"}])
        # Spine's children Arm_l, Leg_l, Leg_r become Leg_r, Leg_l, Arm_l (each move simulated on the sibling list).
        self.assertEqual(plan["order"], [{"op": "module.reorder", "name": "Leg_r", "index": 0},
                                         {"op": "module.reorder", "name": "Leg_l", "index": 1}])
        self.assertEqual(plan["config"], [
            {"op": "module.config", "name": "Spine", "path": "Body Control Adjustment", "value": 2.5},
            {"op": "module.config", "name": "Spine", "path": "Enable Stretch", "value": True}])
        self.assertEqual(plan["bindings"], [{"op": "module.bind", "name": "Arm_l", "variable": "FK IK Default", "source": ""}])
        self.assertEqual(plan["disconnects"], [{"op": "disconnect", "connector": "Arm_l/Twist Bones"}])
        self.assertEqual(plan["connects"], [{"op": "connect", "connector": "Arm_r/Arm Primary",
                                             "targets": ["Socket(Spine/shoulder_r)"]}])

    def test_removes_children_first_and_renames(self):
        modules = _modules("Root", "Spine")
        modules[1] = dict(modules[1], Name="Torso", RenamedFrom="Spine")
        plan = self._plan({"Modules": modules})
        self.assertEqual(plan["renames"], [{"op": "module.rename", "name": "Spine", "to": "Torso"}])
        self.assertEqual([op["name"] for op in plan["removes"]], ["Leg_r", "Leg_l", "Arm_l", "Spine"])

    def test_config_reset_and_settings(self):
        modules = _modules("Root", "Spine", "Arm_l", "Leg_l", "Leg_r")
        modules[1]["Config"] = {}
        plan = self._plan({"Modules": modules, "ModularSettings": {"AutoResolve": False}})
        self.assertEqual(plan["config"], [{"op": "module.config", "name": "Spine", "path": "Body Control Adjustment",
                                           "value": None}])
        self.assertEqual(plan["settings"], [{"op": "modular.settings", "auto_resolve": False}])

    def test_refusals(self):
        def code(body):
            return _code(lambda: crm.check(body, LIVE_MODULAR))
        self.assertEqual(code({"Modules": _modules("Root", "Root")}), ("ControlRig.DuplicateModule", "/Body/Modules/1/Name"))
        self.assertEqual(code({"Modules": [{"Name": "Arm", "Class": ARM, "Parent": "Spien"}, *_modules("Spine")]})[0],
                         "ControlRig.ModuleNotFound")
        self.assertIn("Close: Spine", AssetDocumentContractErrorMessage(lambda: crm.check(
            {"Modules": [{"Name": "Arm", "Class": ARM, "Parent": "Spien"}, *_modules("Spine")]}, LIVE_MODULAR)))
        self.assertEqual(code({"Modules": [{"Name": "Arm", "Class": ARM, "Parent": "Spine"}, *_modules("Spine")]}),
                         ("ControlRig.ParentAfterChild", "/Body/Modules/0/Parent"))
        self.assertEqual(code({"Modules": [{"Name": "Arm", "Class": ARM, "Parent": "Arm"}]}),
                         ("ControlRig.ModuleCycle", "/Body/Modules/0/Parent"))
        self.assertEqual(code({"Connections": {"Arn_l/Arm Primary": "Bone(x)"}})[0], "ControlRig.ModuleNotFound")
        self.assertEqual(_code(lambda: crm.validate({"Modules": [{"Name": "A/B", "Class": ARM}]})),
                         ("ControlRig.InvalidName", "/Body/Modules/0/Name"))
        self.assertEqual(_code(lambda: crm.validate({"Connections": {"NoSlash": "Bone(x)"}}))[0], "ControlRig.InvalidBody")
        self.assertEqual(_code(lambda: crm.validate({"ModularSettings": {"AutoResolve": 1}}))[0], "ControlRig.InvalidBody")

    def test_one_target_list_is_the_target(self):
        canonical = cr.canonicalize(_document({"RigType": "Modular", "Modules": _modules("Root"),
                                               "Connections": {"Root/Root": ["Bone(root)"]}}))
        self.assertEqual(canonical["Body"]["Connections"], {"Root/Root": "Bone(root)"})

    def test_module_diff_is_sibling_order(self):
        live = {"RigType": "Modular", "Modules": LIVE_MODULAR["Modules"]}
        body = {"RigType": "Modular", "Modules": _modules("Root", "Spine", "Arm_l", "Leg_l", "Leg_r")}
        self.assertEqual(cr.body_diff(body, live, False), [])
        body = {"RigType": "Modular", "Modules": _modules("Root", "Spine", "Leg_l", "Arm_l", "Leg_r")}
        self.assertEqual([change.path for change in cr.body_diff(body, live, False)], ["/Body/Modules"])


def AssetDocumentContractErrorMessage(call):
    try:
        call()
    except AssetDocumentContractError as caught:
        return caught.diagnostic.message
    return ""


class CandidateTests(unittest.TestCase):
    def test_close_names(self):
        self.assertEqual(ik.close_names("spine", ["Spine", "Neck"]), ["Spine"])
        self.assertEqual(ik.close_names("Begn", ["Begin", "Set", "Add"]), ["Begin"])
        self.assertEqual(ik.describe_candidates("Begn", ["Begin", "Set"], "Nodes"), " Close: Begin.")
        self.assertEqual(ik.describe_candidates("zzz", ["Begin", "Set"], "Nodes"), " Nodes: Begin, Set.")
        self.assertIn("(+3 more)", ik.describe_candidates("zzz", [f"n{index}" for index in range(15)], "Nodes"))

    def test_shared_prefix_does_not_swamp(self):
        """As the native NameSuggestions: a word most candidates share (a naming prefix) does not make a name
        close; a word that starts another counts ("Plucky" / "Pluck")."""
        sources = ["MSS_PlayRandom_Oneshot", "MSS_PlayRandom_Loop", "MSS_Pluck", "MSS_Pluck_Dark", "MSS_MIDISequencePlayer",
                   "MSS_MetronomeClockGenerator", "MSS_Pad", "MSS_Drone"]
        self.assertEqual(ik.close_names("MSS_Plucky", sources), ["MSS_Pluck", "MSS_Pluck_Dark"])
        self.assertEqual(ik.close_names("MSS_PlayRandom", sources), ["MSS_PlayRandom_Loop", "MSS_PlayRandom_Oneshot"])
        self.assertEqual(ik.close_names("MSS_Zebra", sources), [])
        elements = ["Bone(root)", "Bone(pelvis)", "Bone(spine_01)", "Bone(spine_02)", "Bone(hand_l)", "Control(hand_l_ctrl)"]
        self.assertEqual(ik.close_names("Bone(spine_03)", elements), ["Bone(spine_01)", "Bone(spine_02)"])
        self.assertEqual(ik.close_names("Bone(hand_r)", elements)[0], "Bone(hand_l)")
        labels = ["Floor", "Wall_N", "Wall_S", "CeilingLight", "Ceiling", "BP_Door", "CameraActor"]
        self.assertEqual(ik.close_names("CeilingLamp", labels), ["Ceiling", "CeilingLight"])
        self.assertEqual(ik.close_names("CameraActr", labels), ["CameraActor"])
        self.assertEqual(ik.close_names("Door", labels), ["BP_Door"])


class FactoryContextTests(unittest.TestCase):
    def test_factory_context_shape(self):
        document = dict(_document({"Hierarchy": [NULL]}), FactoryContext="/Game/M/SKM.SKM")
        self.assertEqual(cr.canonicalize(document)["FactoryContext"], "/Game/M/SKM.SKM")
        self.assertEqual(_code(lambda: cr.canonicalize(dict(document, FactoryContext=3)))[0], "ControlRig.InvalidFactoryContext")

    def test_imported_elements_are_kept_and_not_diffed(self):
        live = {"Hierarchy": [ROOT, {"Type": "Curve", "Name": "c"}, NULL]}
        authored = cr.canonicalize(dict(_document({"Hierarchy": [NULL]}), FactoryContext="/Game/M/SKM.SKM"))
        extracted = cr.canonicalize(_document(live))
        self.assertEqual(cr.diff(authored, extracted, False), ())
        self.assertNotEqual(cr.diff(cr.canonicalize(_document({"Hierarchy": [NULL]})), extracted, False), ())
        removes = ap.phase_element_removes({"Hierarchy": [NULL]}, live, unlisted_kept=cr.imported_kept)
        self.assertEqual(removes, [])

    def test_mesh_bones_pass_the_hierarchy_check(self):
        ap.check_hierarchy([NULL], set(), None)
        self.assertEqual(_code(lambda: ap.check_hierarchy([NULL], set(), False))[0], "ControlRig.ElementNotFound")
        message = AssetDocumentContractErrorMessage(lambda: ap.check_hierarchy(
            [dict(NULL, Parent="Bone(rot)")], {"Bone(root)"}, {"Bone(root)"}))
        self.assertIn("Bone(root)", message)


class InspectTests(unittest.TestCase):
    def test_modular_suggestions(self):
        class Calls:
            def records(self, detail):
                return None
        facts = {"header": {"facts": {"modular": {"auto_connect": {"Spine/Pelvis Bone": "Bone(pelvis)"},
                                                  "candidates": {"Neck/Neck Primary": ["Socket(Spine/neck_socket)"]},
                                                  "unresolved_primary": ["Arm_l/Hand Bone"]}}}, "pins": {}, "globals": {}}
        original = cr.read_body
        cr.read_body = lambda calls, detail: ({}, facts)
        try:
            payload = cr.inspect_payload({"Body": {}}, Calls())
        finally:
            cr.read_body = original
        self.assertEqual(payload["ModularRigSuggestions"], {
            "AutoConnect": {"Spine/Pelvis Bone": "Bone(pelvis)"}, "Candidates": {"Neck/Neck Primary": ["Socket(Spine/neck_socket)"]},
            "ConnectPrimaryFirst": ["Arm_l/Hand Bone"]})

    def test_class_inspect(self):
        class Item:
            def __init__(self, name, value, writable=True):
                self.name, self.writable = name, writable
                self.default_value = type("D", (), {"is_set": True, "value": value})()

        class Adapter:
            def enumerate(self, cls, context):
                return [Item("PreviewSkeletalMesh", None), Item("bAutoRecompileVM", True), Item("Hidden", 1, writable=False)]

        class Region:
            adapter = Adapter()

            def skipped(self):
                return set()
        original = cr._properties
        cr._properties = lambda: Region()
        try:
            payload = cr.inspect_class(None)
        finally:
            cr._properties = original
        self.assertEqual(payload["ClassSettings"], {"PreviewSkeletalMesh": None, "bAutoRecompileVM": True})
        self.assertIn("FactoryContext", payload["Create"])


class ControlRigPlanRegressionTests(unittest.TestCase):
    """Calls that once went wrong, as planned here."""

    def test_module_parent_cycle_is_a_cycle_in_either_order(self):
        # Spine under Neck and Neck under Spine came back as ParentAfterChild, both ways round.
        spine = {"Name": "Spine", "Class": "/M/Spine.Spine_C", "Parent": "Neck"}
        neck = {"Name": "Neck", "Class": "/M/Neck.Neck_C", "Parent": "Spine"}
        root = {"Name": "Root", "Class": ap.ROOT_MODULE}
        for modules, first in (([root, spine, neck], "Spine"), ([root, neck, spine], "Neck")):
            body = {"RigType": "Modular", "Modules": modules}
            code = _code(lambda: crm.check(body, LIVE_MODULAR))
            self.assertEqual(code, ("ControlRig.ModuleCycle", f"/Body/Modules/1/Parent"))
            message = AssetDocumentContractErrorMessage(lambda: crm.check(body, LIVE_MODULAR))
            other = "Neck" if first == "Spine" else "Spine"
            self.assertIn(f"{first} -> {other} -> {first}", message)
        # A longer cycle names every module on it; an unknown parent is still reported as such first.
        a, b, c = ({"Name": n, "Class": ARM, "Parent": p} for n, p in (("A", "C"), ("B", "A"), ("C", "B")))
        self.assertIn("A -> C -> B -> A", AssetDocumentContractErrorMessage(lambda: crm.check({"Modules": [a, b, c]},
                                                                                                LIVE_MODULAR)))
        self.assertEqual(_code(lambda: crm.check({"Modules": [dict(a, Parent="Nope"), b]}, LIVE_MODULAR))[0],
                         "ControlRig.ModuleNotFound")
        self.assertEqual(crm.module_cycle(_modules("Root", "Spine", "Arm_l")), None)

    def test_sparse_update_without_rig_type_keeps_the_rig_type(self):
        # A sparse Update of a modular rig without RigType failed ModularRegionUnsupported (omitted meant Rig).
        body = {"Connections": {"Root/Root": "Bone(root)", "Arm_l/Arm Primary": "Socket(Spine/shoulder_l)"}}
        canonical = cr.canonicalize(_document(body))
        self.assertEqual(ap.plan(canonical["Body"], LIVE_MODULAR, False), (False, None))
        self.assertEqual(cr.body_diff(canonical["Body"], LIVE_MODULAR, False)[0].path, "/Body/Connections/Arm_l~1Twist Bones")
        # The rig's own type decides: modular regions on a plain rig, graphs on a modular rig, Module settings.
        self.assertEqual(_code(lambda: ap.plan(body, EMPTY, False)), ("ControlRig.ModularRegionUnsupported",
                                                                      "/Body/Connections"))
        self.assertEqual(_code(lambda: ap.plan({"Graphs": {}}, LIVE_MODULAR, False)),
                         ("ControlRig.ModularRigRegionUnsupported", "/Body/Graphs"))
        self.assertEqual(_code(lambda: ap.plan({"Module": {}}, EMPTY, False)), ("ControlRig.InvalidBody", "/Body/Module"))
        ap.plan({"Module": {"Category": "Limbs"}}, dict(EMPTY, RigType="Module"), False)
        # A Create without RigType is a Rig.
        self.assertEqual(_code(lambda: ap.preflight(cr.canonicalize(_document(body)), None, None))[0],
                         "ControlRig.ModularRegionUnsupported")
        # With RigType the document's type is checked at once.
        self.assertEqual(_code(lambda: cr.canonicalize(_document(dict(body, RigType="Rig"))))[0],
                         "ControlRig.ModularRegionUnsupported")

    def test_factory_context_takes_a_package_path(self):
        # A package path was refused ("the object path of the Skeletal Mesh").
        for path in ("/MoverExamples/Characters/Mannequins/Meshes/SKM_Manny_Simple",
                     "/MoverExamples/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"):
            self.assertEqual(cr.canonicalize(dict(_document({}), FactoryContext=path))["FactoryContext"], path)
        for bad in ("SKM_Manny", "/Game/", "/Game//SKM", 3):
            self.assertEqual(_code(lambda: cr.canonicalize(dict(_document({}), FactoryContext=bad)))[0],
                             "ControlRig.InvalidFactoryContext")

    def test_renames_diff_as_renames(self):
        # A function rename read as a new function next to the old one.
        authored = _body()
        authored["Functions"]["AlignChain"] = dict(authored["Functions"].pop("Align"), RenamedFrom="Align")
        authored["Graphs"]["Rig"]["Nodes"]["Call"]["Function"] = "AlignChain"
        authored["Graphs"]["Rig"]["Nodes"]["Begin"] = dict(authored["Graphs"]["Rig"]["Nodes"].pop("BeginExecution"),
                                                           RenamedFrom="BeginExecution")
        authored["Graphs"]["Rig"]["Nodes"]["Call"]["Inputs"] = {"ExecutePin": "Begin.ExecutePin"}
        authored["Graphs"]["Rig"]["Order"] = ["Begin", "Call"]
        authored["Variables"][0] = dict(authored["Variables"][0], Name="Gain", RenamedFrom="Weight")
        authored["Hierarchy"][1] = dict(authored["Hierarchy"][1], Name="main_ctrl", RenamedFrom="root_ctrl")
        changes = {(change.path, change.kind.value, change.before, change.after)
                   for change in cr.body_diff(authored, _body(), False)}
        self.assertEqual(changes, {
            ("/Body/Functions/AlignChain/Name", "Replace", "AlignChain", "Align"),
            ("/Body/Graphs/Rig/Nodes/Begin/Name", "Replace", "Begin", "BeginExecution"),
            ("/Body/Graphs/Rig/Nodes/Call/Function", "Replace", "AlignChain", "Align"),
            ("/Body/Graphs/Rig/Nodes/Call/Inputs/ExecutePin", "Replace", "Begin.ExecutePin", "BeginExecution.ExecutePin"),
            ("/Body/Variables/Gain/Name", "Replace", "Gain", "Weight"),
            ("/Body/Hierarchy/Control(main_ctrl)/Name", "Replace", "main_ctrl", "root_ctrl")})
        # Without RenamedFrom it stays a remove and an add.
        del authored["Functions"]["AlignChain"]["RenamedFrom"]
        kinds = {change.path: change.kind.value for change in cr.body_diff(authored, _body(), False)}
        self.assertEqual((kinds["/Body/Functions/AlignChain"], kinds["/Body/Functions/Align"]), ("Add", "Remove"))
        modules = _modules("Root", "Spine", "Arm_l", "Leg_l", "Leg_r")
        modules[1] = dict(modules[1], Name="Torso", RenamedFrom="Spine")
        for entry in modules[2:]:
            entry["Parent"] = "Torso"
        paths = [change.path for change in cr.body_diff({"RigType": "Modular", "Modules": modules}, LIVE_MODULAR, False)]
        self.assertIn("/Body/Modules/Torso/Name", paths)
        self.assertNotIn("/Body/Modules/Spine", paths)

    def test_explicit_connections_build_every_level_in_one_apply(self):
        # A biped took one apply per tree level. Each level's sockets exist once its parent's connectors
        # resolve, so the apply connects module by module down the tree, each primary connector first, whatever order
        # the map lists them in. Automatic resolution stays an Inspect suggestion (design decision 6).
        live = {"RigType": "Modular", "Modules": [], "Connections": {}, crm.PRIMARIES: []}
        modules = [{"Name": "Root", "Class": ap.ROOT_MODULE}, {"Name": "Spine", "Class": "/M/Spine.Spine_C", "Parent": "Root"},
                   {"Name": "Arm_l", "Class": ARM, "Parent": "Spine"}]
        connections = {"Arm_l/Elbow Bone": "Bone(lowerarm_l)", "Arm_l/Hand Bone": "Bone(hand_l)",
                       "Arm_l/Arm Primary": "Socket(Spine/shoulder_l)", "Spine/Pelvis Bone": "Bone(pelvis)",
                       "Spine/Spine Primary": "Socket(Root/spine_01_socket)", "Root/Root": "Bone(root)"}
        body = {"RigType": "Modular", "Modules": modules, "Connections": connections}
        canonical = cr.canonicalize(_document(body))["Body"]
        crm.check(canonical, live)
        live[crm.PRIMARIES] = ["Root/Root", "Spine/Spine Primary", "Arm_l/Arm Primary"]
        connectors = [op["connector"] for op, _ in crm.phase_connects(canonical, live, _make)]
        self.assertEqual(connectors, ["Root/Root", "Spine/Spine Primary", "Spine/Pelvis Bone", "Arm_l/Arm Primary",
                                      "Arm_l/Elbow Bone", "Arm_l/Hand Bone"])
        # Connected as listed: nothing to diff, disconnect or connect again.
        connected = dict(live, Modules=modules, Connections=dict(connections))
        self.assertEqual(cr.body_diff(canonical, connected, False), [])
        self.assertEqual(crm.phase_disconnects(canonical, connected, _make), [])
        self.assertEqual(crm.phase_connects(canonical, connected, _make), [])
        # A connection is an element key or a list of them; there is no automatic value.
        for key, value in (("Arm_l/*", "Auto"), ("Arm_l/Elbow Bone", "Auto")):
            self.assertEqual(_code(lambda: crm.validate({"Connections": {key: value}}))[0], "ControlRig.InvalidBody")

    def test_inspect_lists_what_the_modules_build(self):
        # Inspect of a modular rig showed only the mesh's bones, none of the controls the modules make.
        class Calls:
            def records(self, detail):
                return None
        elements = {"Control(Arm_l/ik_ctrl)": {"Translation": {"X": 20.0, "Y": 0.0, "Z": 140.0}}}
        check = {"Arm_l/Elbow Bone": "the module's default of 3 matches (others: Bone(lowerarm_l), Bone(upperarm_l))"}
        facts = {"header": {"counts": {"elements": 70}, "facts": {"modular": {"elements": elements, "element_count": 160,
                                                                              "check": check}}},
                 "pins": {}, "globals": {"Bone(root)": {}}}
        original = cr.read_body
        cr.read_body = lambda calls, detail: ({}, facts)
        try:
            payload = cr.inspect_payload({"Body": {}}, Calls())
        finally:
            cr.read_body = original
        self.assertEqual(payload["GlobalTransforms"], {"Bone(root)": {}, **elements})
        self.assertEqual(payload["Counts"], {"elements": 70, "module_elements": 1, "constructed_elements": 160})
        # An AutoConnect pick among several matches says so (Elbow Bone = the clavicle, accepted unseen).
        self.assertEqual(payload["ModularRigSuggestions"]["Check"], check)

    def test_sparse_reads_compare_over_what_they_leave_out(self):
        # Imported bones at the reference pose and pin types the template decides are left out of Extract; an
        # authored value equal to what is left out still matches (Diff / Verify read them as defaults).
        pose = {"Translation": {"X": 0.0, "Y": 2.28, "Z": 95.9}}
        authored = _document({"Hierarchy": [{"Type": "Bone", "Name": "pelvis", "Imported": True, "Transform": pose}],
                              "Graphs": {"Rig": {"Nodes": {"Get": {"Template": "DISPATCH_RigVMDispatch_ArrayGetAtIndex",
                                                                   "Position": [0, 0], "PinTypes": {
                                                                       "Array": "TArray<float>", "Index": "int32",
                                                                       "Element": "float"}}}}}})
        live = _document({"Hierarchy": [{"Type": "Bone", "Name": "pelvis", "Imported": True}],
                          "Graphs": {"Rig": {"Nodes": {"Get": {"Template": "DISPATCH_RigVMDispatch_ArrayGetAtIndex",
                                                               "Position": [0, 0], "PinTypes": {"Array": "TArray<float>"}}}}}})
        defaults = {"references": {"Bone(pelvis)": pose},
                    "pin_types": [(("Graphs", "Rig"), "Get", {"Array": "TArray<float>", "Index": "int32", "Element": "float"})]}
        authored, live = cr.canonicalize(authored), cr.canonicalize(live)
        self.assertEqual(cr.diff(authored, live, False, defaults), ())
        self.assertEqual({change.path for change in cr.diff(authored, live, False)},
                         {"/Body/Hierarchy/Bone(pelvis)/Transform", "/Body/Graphs/Rig/Nodes/Get/PinTypes/Index",
                          "/Body/Graphs/Rig/Nodes/Get/PinTypes/Element"})
        moved = copy.deepcopy(authored)
        moved["Body"]["Hierarchy"][0]["Transform"] = {"Translation": {"X": 1.0, "Y": 2.28, "Z": 95.9}}
        moved["Body"]["Graphs"]["Rig"]["Nodes"]["Get"]["PinTypes"]["Element"] = "int32"
        self.assertEqual({change.path for change in cr.diff(moved, live, False, defaults)},
                         {"/Body/Hierarchy/Bone(pelvis)/Transform/Translation/X",
                          "/Body/Graphs/Rig/Nodes/Get/PinTypes/Element"})
        # Apply's live read carries the complete types (an authored decided pin plans nothing).
        body = cr.with_full_types(copy.deepcopy(live["Body"]), defaults["pin_types"])
        self.assertEqual(body["Graphs"]["Rig"]["Nodes"]["Get"]["PinTypes"]["Element"], "float")


class ProfileTests(unittest.TestCase):
    def test_registration(self):
        meta = cr.CONTROL_RIG_RUNTIME_REGISTRATION.profile_meta
        self.assertEqual((meta.profile_id, meta.route_id, meta.exact_asset_class_path),
                         (cr.PROFILE_ID, ik.ROUTE, "/Script/ControlRigDeveloper.ControlRigBlueprint"))
        required = {item.primitive_id for item in meta.required_primitives}
        self.assertTrue({ik.CONTROLRIG_READ, ik.CONTROLRIG_EDIT, ik.CONTROLRIG_COMPILE} <= required)

    def test_new_rig_preflight(self):
        ap.preflight(cr.canonicalize(_document({"Variables": [{"Name": "A", "Type": "float"}]})), None, None)
        ap.preflight(cr.canonicalize(_document({"RigType": "Modular", "Modules": [
            {"Name": "Root", "Class": ap.ROOT_MODULE}, {"Name": "Spine", "Class": "/Game/M/Spine.Spine_C", "Parent": "Root"}],
            "Connections": {"Spine/Root": "Bone(pelvis)"}})), None, None)


if __name__ == "__main__":
    unittest.main()
