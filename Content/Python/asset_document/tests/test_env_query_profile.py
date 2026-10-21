"""EnvQuery Profile (profile.env_query.v1): Body checks, Extract / Diff over new-node defaults, and the Apply op
sequence against a fake query served through ai.env_query.read / edit / finalize (adds, moves, removals, key renames,
generator switches, Details, toggles, layout, comment boxes, validation classification, refusals).  The editor side
is covered by UEAssetDoc.EnvQuery.* in the editor."""
from __future__ import annotations

import copy
import unittest

from asset_document.diagnostics import AssetDocumentContractError, RuntimeDiagnostic
from asset_document.diff.model import DiffKind
from asset_document.profiles import env_query
from asset_document.profiles import env_query_model as model
from asset_document.regions.primitive_catalog import PrimitiveOutcome, PrimitiveOutcomeStatus

TARGET = "/Game/AI/EQS_Test"
GENERATORS = {
    "SimpleGrid": {"GridSize": {"DefaultValue": 500.0, "ParamName": ""}, "SpaceBetween": {"DefaultValue": 100.0, "ParamName": ""}},
    "ActorsOfClass": {"SearchRadius": {"DefaultValue": 500.0, "ParamName": ""}, "SearchedActorClass": None},
    "Donut": {"InnerRadius": {"DefaultValue": 300.0, "ParamName": ""}, "OuterRadius": {"DefaultValue": 1000.0, "ParamName": ""}},
}
TESTS = {
    "Distance": {"TestPurpose": "EEnvTestPurpose::FilterAndScore", "ScoringFactor": {"DefaultValue": 1.0, "ParamName": ""}},
    "Trace": {"TestPurpose": "EEnvTestPurpose::FilterAndScore", "BoolValue": {"DefaultValue": True, "ParamName": ""}},
    "Dot": {"TestPurpose": "EEnvTestPurpose::FilterAndScore", "ScoringFactor": {"DefaultValue": 1.0, "ParamName": ""}},
}
COMMENT_DEFAULTS = {"CommentColor": {"R": 1.0, "G": 1.0, "B": 1.0, "A": 1.0}, "FontSize": 18,
                    "bCommentBubbleVisible_InDetailsPanel": True, "bColorCommentBubble": False,
                    "MoveMode": "ECommentBoxMode::GroupMovement", "NodeDetails": ""}


def _ok(value):
    return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED, value, (), (), ())


def _fail(code, message, path="/"):
    return PrimitiveOutcome(PrimitiveOutcomeStatus.FAILED, None, (), (),
                            (RuntimeDiagnostic(path=path, code=code, message=message),))


def _resolve(kind, text):
    table = GENERATORS if kind == "generator" else TESTS
    label = model.class_label(text, kind)
    for name in table:
        if name.casefold() == label.casefold():
            return name
    return None


def _sparse(props, defaults):
    return {key: value for key, value in props.items() if defaults.get(key) != value}


class _Query:
    """An Environment Query editor graph: Root, option nodes (a generator and its tests each) and comment boxes."""

    def __init__(self):
        self.root = [0, 0]
        self.options = []  # dicts: name, label, props, pos, connected, tests, comment, bubble
        self.tests = {}    # name -> label, props, enabled, comment, bubble
        self.comments = {}
        self.counter = 0
        self.writes = []
        self.open = False
        self.finalized = 0
        self.dependents = []

    def unique(self, base):
        self.counter += 1
        return f"{base}_{self.counter}"

    def add_option(self, name, label, pos, props=None, connected=True, tests=()):
        option = {"name": name, "label": label, "props": dict(copy.deepcopy(GENERATORS[label]), **(props or {})),
                  "pos": list(pos), "connected": connected, "tests": [], "comment": "", "bubble": False}
        self.options.append(option)
        for test in tests:
            self.add_test(option, *test)
        return option

    def add_test(self, option, name, label, props=None, enabled=True):
        self.tests[name] = {"label": label, "props": dict(copy.deepcopy(TESTS[label]), **(props or {})),
                            "enabled": enabled, "comment": "", "bubble": False}
        option["tests"].append(name)

    def option(self, name):
        return next((option for option in self.options if option["name"].casefold() == name.casefold()), None)

    def holder(self, name):
        return (self.option(name) is not None or any(key.casefold() == name.casefold() for key in self.tests)
                or any(key.casefold() == name.casefold() for key in self.comments))

    def runtime(self):
        return [option["name"] for option in sorted((item for item in self.options if item["connected"]),
                                                    key=lambda item: item["pos"][0])]

    def params(self):
        out = []
        for props in [option["props"] for option in self.options] + [test["props"] for test in self.tests.values()]:
            for value in props.values():
                if type(value) is dict and value.get("ParamName"):
                    out.append({"name": value["ParamName"], "type": "Float"})
        return out

    def validate(self):
        messages = []
        for option in self.options:
            if option["props"].get("GridSize", {}).get("DefaultValue", 0) < 0:
                messages.append({"severity": "error", "message": f"{option['name']}: GridSize is negative.",
                                 "option": option["name"]})
            for name in option["tests"]:
                if option["label"] == "ActorsOfClass" and self.tests[name]["label"] == "Trace":
                    messages.append({"severity": "warning", "message": "Trace on actors.", "option": option["name"],
                                     "test": name})
        if not any(option["connected"] for option in self.options):
            messages.append({"severity": "warning", "message": "No option is connected to Root."})
        return messages

    # -- primitives ------------------------------------------------------
    def handoff(self, *, source_handle, target_provider_id, access):
        return (target_provider_id, access)

    def invoke(self, *, requirement, value_arguments, handle_arguments):
        primitive = requirement.primitive_id
        args = value_arguments
        if primitive == model.READ:
            return self.read(args)
        if primitive == "editor.property.enumerate_admitted.v1":
            return _ok({"properties": []})
        self.writes.append((primitive, copy.deepcopy(args)))
        if primitive == model.FINALIZE:
            self.finalized += 1
            return _ok({"messages": self.validate(), "params": self.params()})
        if primitive == model.EDIT:
            created = []
            for index, op in enumerate(args["ops"]):
                result = self.edit(op, f"/ops/{index}")
                if type(result) is PrimitiveOutcome:
                    return result
                created.append(result)
            return _ok({"created": created})
        raise AssertionError(f"unexpected primitive {primitive}")

    def rename(self, table_rename, key, path):
        if key and self.holder(key):
            return _fail("EnvQuery.DuplicateKey", f"'{key}' names another object.", f"{path}/name")
        return None

    def edit(self, op, path):
        kind = op["op"]
        if kind == "add_option":
            label = _resolve("generator", op["class"])
            if label is None:
                return _fail("EnvQuery.UnknownClass", "unknown generator", f"{path}/class")
            name = op.get("name") or self.unique(f"EnvQueryOption")
            if op.get("name") and self.holder(name):
                return _fail("EnvQuery.DuplicateKey", "held", f"{path}/name")
            self.add_option(name, label, op["position"], connected=op.get("connect", True))
            return name
        if kind == "add_test":
            option = self.option(op["option"])
            label = _resolve("test", op["class"])
            if label is None:
                return _fail("EnvQuery.UnknownClass", "unknown test", f"{path}/class")
            name = op.get("name") or self.unique(f"EnvQueryTest_{label}")
            if op.get("name") and self.holder(name):
                return _fail("EnvQuery.DuplicateKey", "held", f"{path}/name")
            self.add_test(option, name, label)
            index = op.get("index", -1)
            if 0 <= index < len(option["tests"]) - 1:
                option["tests"].remove(name)
                option["tests"].insert(index, name)
            return name
        if kind in ("remove", "remove_comment"):
            name = op["name"]
            if name in self.comments:
                del self.comments[name]
                return ""
            option = self.option(name)
            if option is not None:
                self.options.remove(option)
                for test in option["tests"]:
                    del self.tests[test]
                return ""
            for option in self.options:
                if name in option["tests"]:
                    option["tests"].remove(name)
                    del self.tests[name]
                    return ""
            return _fail("EnvQuery.NodeNotFound", "no node", f"{path}/name")
        if kind == "move_test":
            for option in self.options:
                if op["name"] in option["tests"]:
                    option["tests"].remove(op["name"])
            target = self.option(op["option"])
            target["tests"].insert(min(op["index"], len(target["tests"])), op["name"])
            return ""
        if kind == "set_enabled":
            self.tests[op["name"]]["enabled"] = op["enabled"]
            return ""
        if kind == "set_property":
            owner = self.option(op["name"])
            props = owner["props"] if owner is not None else self.tests[op["name"]]["props"]
            if op["property"] not in props:
                return _fail("EnvQuery.UnknownProperty", "no such property", f"{path}/property")
            value = op["value"]
            if type(value) is dict and type(props[op["property"]]) is dict:
                props[op["property"]] = {**props[op["property"]], **value}
            else:
                props[op["property"]] = copy.deepcopy(value)
            return ""
        if kind == "connect":
            self.option(op["name"])["connected"] = op["connected"]
            return ""
        if kind == "layout":
            if op["name"] == "":
                self.root = list(op["position"])
                return ""
            node = self.option(op["name"]) or self.tests.get(op["name"]) or self.comments.get(op["name"])
            if "position" in op:
                node["pos"] = list(op["position"])
            if "comment" in op:
                node["comment"] = op["comment"]
            if "comment_visible" in op:
                node["bubble"] = op["comment_visible"]
            return ""
        if kind == "rename":
            if self.holder(op["key"]):
                return _fail("EnvQuery.DuplicateKey", "held", f"{path}/key")
            option = self.option(op["name"])
            if option is not None:
                option["name"] = op["key"]
                return op["key"]
            if op["name"] in self.tests:
                self.tests[op["key"]] = self.tests.pop(op["name"])
                for item in self.options:
                    item["tests"] = [op["key"] if test == op["name"] else test for test in item["tests"]]
                return op["key"]
            self.comments[op["key"]] = self.comments.pop(op["name"])
            return op["key"]
        if kind == "add_comment":
            name = op.get("name") or self.unique("EdGraphNode_Comment")
            self.comments[name] = {"text": op["text"], "pos": list(op["position"]), "size": list(op.get("size", [400, 100])),
                                   "props": dict(COMMENT_DEFAULTS)}
            return name
        if kind == "comment":
            comment = self.comments[op["name"]]
            if "text" in op:
                comment["text"] = op["text"]
            if "size" in op:
                comment["size"] = list(op["size"])
            comment["props"].update(copy.deepcopy(op.get("properties", {})))
            return ""
        raise AssertionError(f"unexpected op {kind}")

    def read(self, args):
        records = [{"kind": "query", "graph": True, "open_in_editor": self.open, "root": list(self.root),
                    "runtime_order": self.runtime(), "params": self.params(), "messages": self.validate()}]
        runtime = self.runtime()
        for option in self.options:
            records.append({"kind": "option", "name": option["name"], "position": list(option["pos"]),
                            "connected": option["connected"],
                            "runtime_index": runtime.index(option["name"]) if option["name"] in runtime else -1,
                            "class": f"/Script/AIModule.EnvQueryGenerator_{option['label']}", "label": option["label"],
                            "item_type": "/Script/AIModule.EnvQueryItemType_Point",
                            "properties": _sparse(option["props"], GENERATORS[option["label"]]), "unsupported": [],
                            "comment": option["comment"], "comment_visible": option["bubble"], "error": "",
                            "tests": list(option["tests"])})
            for index, name in enumerate(option["tests"]):
                test = self.tests[name]
                records.append({"kind": "test", "name": name, "option": option["name"], "index": index,
                                "class": f"/Script/AIModule.EnvQueryTest_{test['label']}", "label": test["label"],
                                "enabled": test["enabled"], "weight_pct": 100.0, "supported": True,
                                "properties": _sparse(test["props"], TESTS[test["label"]]), "unsupported": [],
                                "comment": test["comment"], "comment_visible": test["bubble"]})
        for name, comment in self.comments.items():
            records.append({"kind": "comment", "name": name, "text": comment["text"], "position": list(comment["pos"]),
                            "size": list(comment["size"]), "properties": dict(comment["props"])})
        if "defaults" in args:
            for index, row in enumerate(args["defaults"]):
                label = _resolve(row["kind"], row["class"])
                if label is None:
                    return _fail("EnvQuery.UnknownClass", f"'{row['class']}' is not a {row['kind']} class.",
                                 f"/defaults/{index}/class")
                table = GENERATORS if row["kind"] == "generator" else TESTS
                prefix = "EnvQueryGenerator_" if row["kind"] == "generator" else "EnvQueryTest_"
                records.append({"kind": "default", "node": row["kind"], "request": row["class"],
                                "class": f"/Script/AIModule.{prefix}{label}", "label": label,
                                "properties": copy.deepcopy(table[label]), "unsupported": []})
            records.append({"kind": "comment_default", "properties": dict(COMMENT_DEFAULTS)})
        if args.get("dependents"):
            records += [{"kind": "dependent", "package": item, "class": "/Script/AIModule.BehaviorTree",
                         "asset": f"{item}.{item.rsplit('/', 1)[-1]}"} for item in self.dependents]
        offset = args.get("offset", 0)
        return _ok({"total": len(records), "next_offset": -1, "records": records[offset:]})


def _cover():
    query = _Query()
    grid = query.add_option("Grid", "SimpleGrid", (0, 100), props={"GridSize": {"DefaultValue": 800.0, "ParamName": ""}},
                            tests=[("Dist", "Distance", {"TestPurpose": "EEnvTestPurpose::Score"}), ("Vis", "Trace")])
    query.tests["Vis"]["enabled"] = False
    query.add_option("Actors", "ActorsOfClass", (300, 100), tests=[("Facing", "Dot")])
    query.comments["Note"] = {"text": "cover", "pos": [-50, -80], "size": [700, 300],
                              "props": dict(COMMENT_DEFAULTS, FontSize=24)}
    return query, grid


def _document(body, properties=None):
    return {"SchemaVersion": 1, "Target": TARGET, "Class": model.CLASS, "Action": "CreateOrUpdate",
            "Definitions": {}, "Properties": properties or {}, "Body": body}


def _extract(query):
    return model.extract_body(model.Calls(query, "read").live())


def _apply(query, body):
    document = env_query.canonicalize(_document(body))
    return env_query._apply.apply(document, "read", "write", query, _NoProperties())


class _NoProperties(env_query.common.PropertiesRegion):
    """The query's Details panel (nothing beyond the Body-owned Options)."""

    def __init__(self):
        super().__init__(model.CLASS, env_query.BODY_OWNED, model.PREFIX)

    def apply(self, properties, read, mutable, context):
        return ()

    def extract(self, read, context, include_defaults=True):
        return {}


def setUpModule():
    global _saved_properties
    _saved_properties = env_query.properties
    env_query.properties = _NoProperties


def tearDownModule():
    env_query.properties = _saved_properties


def _ops(query):
    return [op["op"] for primitive, args in query.writes if primitive == model.EDIT for op in args["ops"]]


def _codes(result):
    return [item.code for item in getattr(result, "diagnostics", ())]


class EnvQueryBodyTests(unittest.TestCase):
    def test_class_labels(self):
        self.assertEqual(model.class_label("/Script/AIModule.EnvQueryGenerator_SimpleGrid", "generator"), "SimpleGrid")
        self.assertEqual(model.class_label("EnvQueryTest_Distance", "test"), "Distance")
        self.assertEqual(model.class_label("Dot", "test"), "Dot")
        self.assertEqual(model.class_label("/Script/SmartObjectsModule.EnvQueryGenerator_SmartObjects", "generator"),
                         "/Script/SmartObjectsModule.EnvQueryGenerator_SmartObjects")
        self.assertEqual(model.class_label("/Script/AIModule.EnvQueryTest_Trace", "generator"),
                         "/Script/AIModule.EnvQueryTest_Trace")

    def test_shape_errors(self):
        grid = {"Generator": {"Class": "SimpleGrid"}}
        cases = [
            ({"Options": {}}, "/Body/Options", "EnvQuery.InvalidOptions"),
            ({"Options": [{}]}, "/Body/Options/0/Generator", "EnvQuery.MissingField"),
            ({"Options": [{"Generator": {}}]}, "/Body/Options/0/Generator/Class", "EnvQuery.MissingField"),
            ({"Options": [{**grid, "Pins": 1}]}, "/Body/Options/0/Pins", "EnvQuery.UnknownField"),
            ({"Options": [{**grid, "Key": "bad key"}]}, "/Body/Options/0/Key", "EnvQuery.InvalidKey"),
            ({"Options": [{**grid, "Key": "A"}, {**grid, "Key": "A"}]}, "/Body/Options/1/Key", "EnvQuery.DuplicateKey"),
            ({"Options": [{**grid, "Key": "A", "Tests": [{"Key": "A", "Class": "Dot"}]}]},
             "/Body/Options/0/Tests/0/Key", "EnvQuery.DuplicateKey"),
            ({"Options": [{**grid, "Tests": [{"Key": "T"}]}]}, "/Body/Options/0/Tests/0/Class", "EnvQuery.MissingField"),
            ({"Options": [{**grid, "Tests": [{"Class": "Dot", "Enabled": 1}]}]}, "/Body/Options/0/Tests/0/Enabled",
             "EnvQuery.InvalidEnabled"),
            ({"Options": [{**grid, "Position": [1]}]}, "/Body/Options/0/Position", "EnvQuery.InvalidPosition"),
            ({"Comments": {"N": {"Position": [0, 0]}}}, "/Body/Comments/N/Text", "EnvQuery.MissingField"),
            ({"Comments": {"N": {"Text": "x", "Size": [0, 10]}}}, "/Body/Comments/N/Size", "EnvQuery.InvalidSize"),
            ({"Root": {"Input": "x"}}, "/Body/Root/Input", "EnvQuery.UnknownField"),
        ]
        for body, path, code in cases:
            with self.subTest(path=path):
                with self.assertRaises(AssetDocumentContractError) as caught:
                    env_query.canonicalize(_document(body))
                self.assertEqual((caught.exception.diagnostic.path, caught.exception.diagnostic.code), (path, code))

    def test_options_is_body_owned(self):
        with self.assertRaises(AssetDocumentContractError) as caught:
            env_query.canonicalize(_document({}, {"Options": []}))
        self.assertEqual(caught.exception.diagnostic.code, "EnvQuery.BodyOwnedProperty")


class EnvQueryExtractTests(unittest.TestCase):
    def test_extract_is_sparse_and_in_runtime_order(self):
        query, _ = _cover()
        query.option("Actors")["pos"] = [-300, 100]  # runs first now
        body = _extract(query)
        self.assertEqual([option["Key"] for option in body["Options"]], ["Actors", "Grid"])
        grid = body["Options"][1]
        self.assertEqual(grid["Generator"], {"Class": "SimpleGrid",
                                             "Properties": {"GridSize": {"DefaultValue": 800.0, "ParamName": ""}}})
        self.assertEqual(grid["Tests"], [{"Key": "Dist", "Class": "Distance",
                                          "Properties": {"TestPurpose": "EEnvTestPurpose::Score"}},
                                         {"Key": "Vis", "Class": "Trace", "Enabled": False}])
        self.assertNotIn("Connected", grid)
        self.assertEqual(body["Comments"], {"Note": {"Text": "cover", "Position": [-50, -80], "Size": [700, 300],
                                                     "FontSize": 24}})

    def test_extract_with_every_value_applies_as_a_no_op(self):
        query, _ = _cover()
        document = env_query.extract(TARGET, "read", query, include_defaults=True)
        grid = document["Body"]["Options"][0]
        self.assertEqual(grid["Generator"]["Properties"]["SpaceBetween"], {"DefaultValue": 100.0, "ParamName": ""})
        self.assertEqual(grid["Tests"][1]["Properties"]["TestPurpose"], "EEnvTestPurpose::FilterAndScore")
        _apply(query, document["Body"])
        self.assertEqual(query.writes, [])

    def test_unconnected_options_follow_the_connected_ones(self):
        query, _ = _cover()
        query.option("Grid")["connected"] = False
        body = _extract(query)
        self.assertEqual([option["Key"] for option in body["Options"]], ["Actors", "Grid"])
        self.assertIs(body["Options"][1]["Connected"], False)

    def test_extract_applies_as_a_no_op_and_diffs_empty(self):
        query, _ = _cover()
        body = _extract(query)
        _apply(query, copy.deepcopy(body))
        self.assertEqual(query.writes, [])
        document = env_query.canonicalize(_document(body))
        self.assertEqual(env_query.diff(document, document, True), ())

    def test_diff_with_defaults_reads_an_omitted_value_as_its_default(self):
        query, _ = _cover()
        authored = env_query.canonicalize(_document({"Options": [
            {"Key": "Grid", "Generator": {"Class": "/Script/AIModule.EnvQueryGenerator_SimpleGrid", "Properties": {
                "GridSize": {"DefaultValue": 800.0, "ParamName": ""}, "SpaceBetween": {"DefaultValue": 100.0}}},
             "Tests": [{"Key": "Dist", "Class": "distance", "Properties": {"TestPurpose": "EEnvTestPurpose::Score"}},
                       {"Key": "Vis", "Class": "Trace", "Enabled": False}]},
            {"Key": "Actors", "Generator": {"Class": "ActorsOfClass"}}]}))
        document, defaults = env_query.extract_with_defaults(TARGET, "read", query, authored)
        extracted = env_query.canonicalize(document)
        self.assertEqual(env_query.diff(authored, extracted, False, defaults), ())
        # Plain Diff compares as written: SpaceBetween (a default) and the class spelling differ.
        self.assertTrue(env_query.diff(authored, extracted, False))

    def test_diff_reports_lost_values_order_and_removals(self):
        query, _ = _cover()
        document, defaults = env_query.extract_with_defaults(TARGET, "read", query)
        extracted = env_query.canonicalize(document)
        authored = copy.deepcopy(extracted)
        authored["Body"]["Options"].reverse()
        authored["Body"]["Options"][0]["Position"] = [-300, 100]
        authored["Body"]["Options"][1]["Tests"][1]["Enabled"] = True
        del authored["Body"]["Comments"]["Note"]
        paths = {item.path: item.kind for item in env_query.diff(authored, extracted, False, defaults)}
        self.assertIn("/Body/Options/0/Position", paths)
        self.assertIn("/Body/Options/1/Tests/1/Enabled", paths)
        self.assertIn("/Body/Options", paths)
        self.assertEqual(paths["/Body/Comments/Note"], DiffKind.REMOVE)


class EnvQueryApplyTests(unittest.TestCase):
    def test_create_from_empty(self):
        query = _Query()
        body = {"Root": {"Position": [0, 0]}, "Options": [
            {"Key": "Grid", "Generator": {"Class": "SimpleGrid", "Properties": {"GridSize": {"DefaultValue": 1200.0}}},
             "Tests": [{"Key": "Dist", "Class": "Distance"}, {"Key": "Vis", "Class": "Trace", "Enabled": False}]},
            {"Key": "Actors", "Generator": {"Class": "ActorsOfClass"}, "Tests": []}], "Comments": {}}
        result = _apply(query, body)
        self.assertEqual(query.runtime(), ["Grid", "Actors"])
        self.assertEqual([option["pos"] for option in query.options], [[0, 100], [300, 100]])
        self.assertEqual(query.option("Grid")["tests"], ["Dist", "Vis"])
        self.assertIs(query.tests["Vis"]["enabled"], False)
        self.assertEqual(query.option("Grid")["props"]["GridSize"], {"DefaultValue": 1200.0, "ParamName": ""})
        self.assertEqual(query.finalized, 1)
        self.assertEqual(_codes(result), [])
        after = _extract(query)
        self.assertEqual(env_query.diff(env_query.canonicalize(_document(body)),
                                        env_query.canonicalize(_document(after)), False), ())

    def test_free_edits_reorder_switch_and_remove(self):
        query, _ = _cover()
        body = _extract(query)
        grid, actors = body["Options"]
        grid["Tests"].reverse()                                   # reorder tests
        grid["Tests"].append({"Class": "Dot"})                    # add a keyless test
        actors["Generator"] = {"Class": "Donut"}                  # switch generator (tests kept: Tests listed)
        actors["Tests"][0]["Properties"] = {"ScoringFactor": {"DefaultValue": 2.0}}
        body["Options"] = [actors, grid]                          # Donut runs first
        del actors["Position"]
        del body["Comments"]["Note"]
        _apply(query, body)
        self.assertEqual(query.runtime()[1], "Grid")
        donut = query.options[-1]
        self.assertEqual((donut["name"], donut["label"]), ("Actors", "Donut"))
        self.assertLess(donut["pos"][0], query.option("Grid")["pos"][0])
        self.assertEqual(query.runtime(), ["Actors", "Grid"])
        self.assertEqual(donut["tests"], ["Facing"])
        self.assertEqual(query.tests["Facing"]["props"]["ScoringFactor"]["DefaultValue"], 2.0)
        self.assertEqual(query.option("Grid")["tests"][:2], ["Vis", "Dist"])
        self.assertEqual(query.tests[query.option("Grid")["tests"][2]]["label"], "Dot")
        self.assertEqual(query.comments, {})
        ops = _ops(query)
        self.assertIn("rename", ops)  # the replacement takes the key once the old option is gone
        self.assertLess(ops.index("add_option"), ops.index("move_test"))

    def test_moving_a_test_between_options_keeps_it(self):
        query, _ = _cover()
        body = _extract(query)
        body["Options"][1]["Tests"].append(body["Options"][0]["Tests"].pop(0))
        _apply(query, body)
        self.assertEqual(query.option("Actors")["tests"], ["Facing", "Dist"])
        self.assertEqual(query.tests["Dist"]["props"]["TestPurpose"], "EEnvTestPurpose::Score")
        self.assertNotIn("remove", _ops(query))

    def test_option_without_tests_keeps_them(self):
        query, _ = _cover()
        _apply(query, {"Options": [{"Key": "Grid", "Generator": {"Class": "SimpleGrid", "Properties": {
            "GridSize": {"DefaultValue": 800.0}}}}, {"Key": "Actors", "Generator": {"Class": "ActorsOfClass"}}]})
        self.assertEqual(query.option("Grid")["tests"], ["Dist", "Vis"])
        self.assertEqual(query.writes, [])

    def test_derived_positions_keep_the_document_order(self):
        query, _ = _cover()
        body = _extract(query)
        body["Options"].insert(1, {"Key": "Mid", "Generator": {"Class": "Donut"}})
        body["Options"].append({"Key": "Last", "Generator": {"Class": "Donut"}})
        _apply(query, body)
        self.assertEqual(query.runtime(), ["Grid", "Mid", "Actors", "Last"])
        self.assertEqual(query.option("Mid")["pos"], [150, 100])
        self.assertEqual(query.option("Last")["pos"], [600, 100])

    def test_contradicting_positions_are_refused(self):
        query, _ = _cover()
        body = _extract(query)
        body["Options"][1]["Position"] = [-500, 100]
        with self.assertRaises(AssetDocumentContractError) as caught:
            _apply(query, body)
        self.assertEqual((caught.exception.diagnostic.path, caught.exception.diagnostic.code),
                         ("/Body/Options/1/Position", "EnvQuery.OptionOrder"))
        self.assertEqual(query.writes, [])

    def test_unknown_class_and_property(self):
        query, _ = _cover()
        body = _extract(query)
        body["Options"][0]["Tests"][0]["Class"] = "Distanse"
        with self.assertRaises(AssetDocumentContractError) as caught:
            _apply(query, body)
        self.assertEqual((caught.exception.diagnostic.path, caught.exception.diagnostic.code),
                         ("/Body/Options/0/Tests/0/Class", "EnvQuery.UnknownClass"))
        body = _extract(query)
        body["Options"][1]["Generator"]["Properties"] = {"Radius": 1.0}
        with self.assertRaises(AssetDocumentContractError) as caught:
            _apply(query, body)
        self.assertEqual((caught.exception.diagnostic.path, caught.exception.diagnostic.code),
                         ("/Body/Options/1/Generator/Properties/Radius", "EnvQuery.UnknownProperty"))
        self.assertEqual(query.writes, [])

    def test_new_errors_fail_and_old_ones_warn(self):
        query, _ = _cover()
        body = _extract(query)
        body["Options"][0]["Generator"]["Properties"]["GridSize"] = {"DefaultValue": -1.0}
        with self.assertRaises(AssetDocumentContractError) as caught:
            _apply(query, body)
        self.assertEqual((caught.exception.diagnostic.path, caught.exception.diagnostic.code),
                         ("/Body/Options/0", "EnvQuery.ValidationFailed"))
        # Already broken: the error comes back as a warning; a warning stays one.
        body = _extract(query)
        body["Options"][1]["Tests"].append({"Class": "Trace"})
        result = _apply(query, body)
        self.assertEqual(sorted(set(_codes(result))), ["EnvQuery.ValidationError", "EnvQuery.ValidationWarning"])

    def test_param_change_warns_when_assets_use_the_query(self):
        query, _ = _cover()
        query.dependents = ["/Game/AI/BT_Cover"]
        body = _extract(query)
        body["Options"][0]["Generator"]["Properties"]["GridSize"] = {"DefaultValue": 800.0, "ParamName": "Radius"}
        result = _apply(query, body)
        self.assertIn("EnvQuery.DependentParams", _codes(result))

    def test_held_key_renames_after_removal(self):
        query, _ = _cover()
        body = _extract(query)
        # 'Dist' becomes a new Dot test (the old Distance test is removed).
        body["Options"][0]["Tests"][0] = {"Key": "Dist", "Class": "Dot"}
        _apply(query, body)
        self.assertEqual(query.tests["Dist"]["label"], "Dot")
        self.assertEqual(query.option("Grid")["tests"], ["Dist", "Vis"])

    def test_comment_boxes(self):
        query, _ = _cover()
        body = _extract(query)
        body["Comments"]["Note"]["Text"] = "cover points"
        body["Comments"]["Note"]["Color"] = {"R": 1.0, "G": 0.0, "B": 0.0, "A": 1.0}
        body["Comments"]["Extra"] = {"Text": "extra", "Position": [400, -80]}
        _apply(query, body)
        self.assertEqual(query.comments["Note"]["text"], "cover points")
        self.assertEqual(query.comments["Note"]["props"]["CommentColor"]["G"], 0.0)
        self.assertEqual(query.comments["Extra"]["pos"], [400, -80])
        self.assertEqual(model.extract_body(model.Calls(query, "read").live())["Comments"]["Extra"],
                         {"Text": "extra", "Position": [400, -80], "Size": [400, 100]})

    def test_open_in_editor_applies(self):
        # The Environment Query editor edits the query's graph itself; Core refreshes it after the apply.
        query, _ = _cover()
        query.open = True
        body = _extract(query)
        body["Comments"]["Note"]["Text"] = "edited while open"
        _apply(query, body)
        self.assertEqual(query.comments["Note"]["text"], "edited while open")

    def test_inspect_payload(self):
        query, _ = _cover()
        query.dependents = ["/Game/AI/BT_Cover"]
        live = model.Calls(query, "read").live(dependents=True)
        payload = env_query.inspect_payload({"Body": model.extract_body(live)}, live)
        self.assertEqual(payload["Status"]["Status"], "UpToDate")
        self.assertEqual(payload["NodeFacts"]["Vis"]["Enabled"], False)
        self.assertEqual(payload["Dependents"], [{"Asset": "/Game/AI/BT_Cover.BT_Cover",
                                                  "Class": "/Script/AIModule.BehaviorTree"}])


if __name__ == "__main__":
    unittest.main()
