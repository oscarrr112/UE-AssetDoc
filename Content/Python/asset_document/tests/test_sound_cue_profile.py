# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""SoundCue Profile (profile.sound_cue.v1): Body checks, Extract / Diff, and the Apply call sequence against a fake
cue served through graph.* and audio.soundcue.read.v1 (positional inputs, breaks before connects, pin counts, node
and comment Details, finalize classification, refusals).  The editor side is covered by
UEAssetDoc.SoundCue.* in the editor."""
from __future__ import annotations

import copy
import unittest

from asset_document.diagnostics import AssetDocumentContractError, DiagnosticContext, RuntimeDiagnostic
from asset_document.diff.model import DiffKind
from asset_document.profiles import sound_cue
from asset_document.profiles import sound_cue_model as model
from asset_document.regions.primitive_catalog import PrimitiveOutcome, PrimitiveOutcomeStatus

TARGET = "/Game/Audio/SC_Test"
# label -> (starting inputs, min, max, defaults(n))
CLASSES = {
    "WavePlayer": (0, 0, 0, lambda n: {"SoundWaveAssetPtr": None, "bLooping": False}),
    "Random": (2, 1, 32, lambda n: {"Weights": [1.0] * n, "bRandomizeWithoutReplacement": True}),
    "Mixer": (2, 1, 32, lambda n: {"InputVolume": [1.0] * n}),
    "Modulator": (1, 1, 1, lambda n: {"PitchMin": 0.95, "PitchMax": 1.05}),
    "Delay": (1, 0, 1, lambda n: {"DelayMin": 0.0, "DelayMax": 0.0}),
    "Branch": (3, 3, 3, lambda n: {"BoolParameterName": "None"}),
}
COMMENT_DEFAULTS = {"CommentColor": {"R": 1.0, "G": 1.0, "B": 1.0, "A": 1.0}, "FontSize": 18,
                    "bCommentBubbleVisible_InDetailsPanel": True, "bColorCommentBubble": False,
                    "MoveMode": "ECommentBoxMode::GroupMovement"}


def _ok(value):
    return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED, value, (), (), ())


def _fail(code, message, path="/", context=()):
    return PrimitiveOutcome(PrimitiveOutcomeStatus.FAILED, None, (), (),
                            (RuntimeDiagnostic(path=path, code=code, message=message, context=tuple(context)),))


def _guid(number):
    return f"{number:032X}"


class _Cue:
    """A Sound Cue editor graph: Root, sound nodes (positional inputs) and comments."""

    def __init__(self):
        self.next = 1
        self.nodes = {}
        self.root = self._add({"kind": "root", "name": "Root", "pos": [300, 0], "inputs": [None]})
        self.writes = []
        self.open = False
        self.finalized = 0

    def _add(self, node):
        guid = _guid(self.next)
        self.next += 1
        node.setdefault("comment", "")
        node.setdefault("bubble", False)
        self.nodes[guid] = node
        return guid

    def add_sound(self, name, label, inputs=None, pos=(0, 0), props=None):
        count = CLASSES[label][0] if inputs is None else len(inputs)
        full = CLASSES[label][3](count)
        full.update(props or {})
        guid = self._add({"kind": "sound", "name": name, "class": label, "pos": list(pos), "inputs": [None] * count,
                          "props": full})
        return guid

    def add_comment(self, name, text, pos=(0, 0), size=(400, 100), props=None):
        return self._add({"kind": "comment", "name": name, "comment": text, "pos": list(pos), "size": list(size),
                          "props": dict(COMMENT_DEFAULTS, **(props or {}))})

    def guid_of(self, name):
        return next(guid for guid, node in self.nodes.items() if node["name"] == name and node["kind"] != "root")

    def link(self, parent, slot, child):
        self.nodes[parent]["inputs"][slot] = child

    # -- snapshot --------------------------------------------------------
    def _resize(self, node):
        label = node["class"]
        per_child = model.PER_CHILD.get(label)
        if per_child:
            name, offset = per_child
            wanted = max(len(node["inputs"]) + offset, 0)
            values = list(node["props"].get(name, []))
            node["props"][name] = (values + [1.0] * wanted)[:wanted]

    def describe(self, guid):
        node = self.nodes[guid]
        parents = [(other, index) for other, item in self.nodes.items()
                   for index, child in enumerate(item.get("inputs", [])) if child == guid]
        pins = []
        for index, child in enumerate(node.get("inputs", [])):
            pins.append({"name": "Root" if node["kind"] == "root" else f"Input{index}", "direction": "input",
                         "type": {"category": "SoundNode"},
                         "linked_to": [] if child is None else [{"node_guid": child, "pin_name": "Output"}]})
        if node["kind"] == "sound":
            pins.append({"name": "Output", "direction": "output", "type": {"category": "SoundNode"},
                         "linked_to": [{"node_guid": parent, "pin_name": "Root" if self.nodes[parent]["kind"] == "root"
                                        else f"Input{index}"} for parent, index in parents]})
        row = {"node_guid": guid, "class": "/Script/AudioEditor.SoundCueGraphNode", "position": {"x": node["pos"][0], "y": node["pos"][1]},
               "comment": node["comment"], "pins": pins}
        if node["kind"] == "root":
            row["implicit_role"] = "root"
            row["extras"] = {}
        elif node["kind"] == "sound":
            starting, low, high, defaults = CLASSES[node["class"]]
            reference = defaults(len(node["inputs"]))
            row["action_key"] = f"SoundCue:Node:/Script/Engine.SoundNode{node['class']}"
            row["extras"] = {"name": node["name"], "class": node["class"], "child_limits": {"min": low, "max": high},
                             "properties": {key: value for key, value in node["props"].items() if reference.get(key) != value},
                             "input_pins": [f"Input{index}" for index in range(len(node["inputs"]))],
                             "reachable": True, "comment_bubble_visible": node["bubble"]}
        else:
            row["action_key"] = "SoundCue:Comment"
            row["extras"] = {"name": node["name"], "size": list(node["size"]),
                             "properties": {key: value for key, value in node["props"].items()
                                            if COMMENT_DEFAULTS.get(key) != value}}
        return row

    def reaches(self, start, target):
        seen, stack = set(), [start]
        while stack:
            guid = stack.pop()
            if guid == target:
                return True
            if guid in seen:
                continue
            seen.add(guid)
            stack.extend(child for child in self.nodes[guid].get("inputs", []) if child is not None)
        return False

    # -- primitives ------------------------------------------------------
    def handoff(self, *, source_handle, target_provider_id, access):
        return (target_provider_id, access)

    def invoke(self, *, requirement, value_arguments, handle_arguments):
        primitive = requirement.primitive_id
        args = value_arguments
        if primitive == "graph.list.v1":
            return _ok({"graphs": [{"graph_guid": _guid(0xFFFF), "name": "SoundCueGraph", "path": "SoundCue",
                                    "role": "other", "schema_tag": "SoundCue", "caps": {}}]})
        if primitive == "graph.query.v1":
            return _ok({"graphs": [{"graph_guid": _guid(0xFFFF), "path": "SoundCue", "role": "other",
                                    "schema_tag": "SoundCue", "nodes": [self.describe(guid) for guid in self.nodes],
                                    "links": []}]})
        if primitive == model.READ:
            return self.read(args)
        self.writes.append((primitive, copy.deepcopy(args)))
        if primitive == "graph.node.spawn.v1":
            key = args["action_key"]
            where = args.get("position", {"x": 0, "y": 0})
            if key == "SoundCue:Comment":
                guid = self.add_comment(f"EdGraphNode_Comment_{self.next}", "Comment", (where["x"], where["y"]))
            else:
                label = key.rsplit("SoundNode", 1)[-1]
                if label not in CLASSES:
                    return _fail("Graph.ActionNotFound", f"No action has key {key}.", "/action_key")
                guid = self.add_sound(f"SoundNode{label}_{self.next}", label, pos=(where["x"], where["y"]))
            return _ok({"node_guid": guid, "node": self.describe(guid)})
        node = self.nodes.get(str(args.get("node_guid", "")).upper()) if "node_guid" in args else None
        if primitive == "graph.node.rename.v1":
            node["name"] = args["name"]
            return _ok({"name": args["name"]})
        if primitive == "graph.node.remove.v1":
            guid = args["node_guid"]
            del self.nodes[guid]
            for other in self.nodes.values():
                other["inputs"] = [None if child == guid else child for child in other.get("inputs", [])]
            return _ok({"removed_node_guids": [guid]})
        if primitive == "graph.node.dynamic_pin.add.v1":
            node["inputs"].append(None)
            self._resize(node)
            return _ok({"node": self.describe(args["node_guid"])})
        if primitive == "graph.node.dynamic_pin.remove.v1":
            index = int(args["pin_name"].replace("Input", ""))
            del node["inputs"][index]
            self._resize(node)
            return _ok({"node": self.describe(args["node_guid"])})
        if primitive == "graph.pin.break.v1":
            pin = args["from"]
            target = self.nodes[pin["node_guid"]]
            index = 0 if target["kind"] == "root" else int(pin["pin_name"][1:])
            target["inputs"][index] = None
            return _ok({"broken_links": []})
        if primitive == "graph.pin.connect.v1":
            source, pin = args["from"]["node_guid"], args["to"]
            target = self.nodes[pin["node_guid"]]
            index = 0 if target["kind"] == "root" else int(pin["pin_name"][1:])
            if self.reaches(source, pin["node_guid"]):
                return _fail("Graph.ConnectDisallowed", "Cannot connect: Connection would cause loop",
                             context=(DiagnosticContext("reason", "Connection would cause loop"),))
            target["inputs"][index] = source
            return _ok({})
        if primitive == "graph.node.property.set.v1":
            if args["container"] == "node" and args["property_path"] == "Size":
                node["size"] = list(args["value"])
            else:
                node["props"][args["property_path"]] = copy.deepcopy(args["value"])
            return _ok({"value": args["value"]})
        if primitive == "graph.node.layout.set.v1":
            if "position" in args:
                node["pos"] = [args["position"]["x"], args["position"]["y"]]
            if "comment" in args:
                node["comment"] = args["comment"]
            if "comment_bubble_visible" in args:
                node["bubble"] = args["comment_bubble_visible"]
            return _ok({})
        if primitive == "graph.finalize.v1":
            self.finalized += 1
            messages = self.validate()
            if any(item["severity"] == "error" for item in messages):
                return _fail("Graph.FinalizeFailed", "Finalize reported errors",
                             context=(DiagnosticContext("messages", messages),))
            return _ok({"status": "warnings" if messages else "ok", "messages": messages})
        raise AssertionError(f"unexpected primitive {primitive}")

    def validate(self):
        messages = []
        for guid, node in self.nodes.items():
            if node.get("class") == "Delay" and not node["inputs"]:
                messages.append({"severity": "error", "message": f"{node['name']}: Delay has no input slot.",
                                 "node_guid": guid})
            if node.get("class") == "WavePlayer" and not node["props"].get("SoundWaveAssetPtr"):
                messages.append({"severity": "warning", "message": f"{node['name']} plays no wave.", "node_guid": guid})
        return messages

    def read(self, args):
        value = {"editable": True, "open_in_editor": self.open, "status": "UpToDate",
                 "messages": [{"severity": item["severity"], "message": item["message"],
                               "node": self.nodes[item["node_guid"]]["name"]} for item in self.validate()],
                 "quality_levels": ["Low", "High"], "waves": [], "derived": {}}
        if "defaults" in args:
            rows = []
            for index, row in enumerate(args["defaults"]):
                if row["class"] not in CLASSES:
                    return _fail("SoundCue.UnknownNodeClass", f"'{row['class']}' is not a sound node class.",
                                 f"/defaults/{index}/class")
                starting, low, high, defaults = CLASSES[row["class"]]
                inputs = row.get("inputs", starting)
                rows.append({"class": row["class"], "class_path": f"/Script/Engine.SoundNode{row['class']}",
                             "inputs": inputs, "starting_inputs": starting, "min": low, "max": high,
                             "properties": defaults(inputs)})
            value["defaults"] = rows
            value["comment_defaults"] = dict(COMMENT_DEFAULTS)
        return _ok(value)


def _footstep():
    cue = _Cue()
    mod = cue.add_sound("Mod", "Modulator", pos=(0, 0), props={"PitchMin": 0.9})
    rand = cue.add_sound("Rand", "Random", inputs=[None, None], pos=(-300, 0), props={"Weights": [1.0, 0.5]})
    step1 = cue.add_sound("Step1", "WavePlayer", pos=(-600, -100), props={"SoundWaveAssetPtr": "/Game/A.A"})
    step2 = cue.add_sound("Step2", "WavePlayer", pos=(-600, 100), props={"SoundWaveAssetPtr": "/Game/B.B"})
    cue.link(cue.root, 0, mod)
    cue.link(mod, 0, rand)
    cue.link(rand, 0, step1)
    cue.link(rand, 1, step2)
    cue.add_comment("Note", "Variations", (-650, -200), (400, 500))
    return cue


def _document(body, properties=None):
    return {"SchemaVersion": 1, "Target": TARGET, "Class": model.CLASS, "Action": "CreateOrUpdate",
            "Definitions": {}, "Properties": properties or {}, "Body": body}


def _body(cue):
    calls = model.Calls(cue, "read")
    _, body = model.read_live(calls)
    return body


def _apply(cue, body):
    document = sound_cue.canonicalize(_document(body))
    return sound_cue._apply.apply(document, "read", "write", cue, sound_cue.properties())


def _kinds(cue):
    return [primitive for primitive, _ in cue.writes]


class SoundCueBodyTests(unittest.TestCase):
    def test_class_labels(self):
        self.assertEqual(model.class_label("/Script/Engine.SoundNodeRandom"), "Random")
        self.assertEqual(model.class_label("SoundNodeWavePlayer"), "WavePlayer")
        self.assertEqual(model.class_label("Mixer"), "Mixer")
        self.assertEqual(model.class_label("/Script/MyPlugin.MySoundNode"), "/Script/MyPlugin.MySoundNode")

    def test_shape_errors(self):
        cases = [
            ({"Nodes": {"bad key": {"Class": "Random"}}}, "/Body/Nodes/bad key", "SoundCue.InvalidKey"),
            ({"Nodes": {"A": {"Class": "Random", "Pins": {}}}}, "/Body/Nodes/A/Pins", "SoundCue.UnknownField"),
            ({"Nodes": {"A": {}}}, "/Body/Nodes/A/Class", "SoundCue.MissingField"),
            ({"Nodes": {"A": {"Class": "Mixer", "Inputs": [None] * 33}}}, "/Body/Nodes/A/Inputs", "SoundCue.ChildCount"),
            ({"Nodes": {"A": {"Class": "Mixer", "Inputs": ["B"]}}}, "/Body/Nodes/A/Inputs/0", "SoundCue.UnknownKey"),
            ({"Root": {"Input": "X"}, "Nodes": {}}, "/Body/Root/Input", "SoundCue.UnknownKey"),
            ({"Nodes": {"A": {"Class": "Mixer", "Position": [1]}}}, "/Body/Nodes/A/Position", "SoundCue.InvalidPosition"),
            ({"Nodes": {"A": {"Class": "WavePlayer"}}, "Comments": {"A": {"Text": "x"}}}, "/Body/Comments/A",
             "SoundCue.DuplicateKey"),
            ({"Comments": {"C": {"Position": [0, 0]}}}, "/Body/Comments/C/Text", "SoundCue.MissingField"),
        ]
        for body, path, code in cases:
            with self.subTest(code=code, path=path):
                with self.assertRaises(AssetDocumentContractError) as raised:
                    sound_cue.canonicalize(_document(body))
                self.assertEqual((raised.exception.diagnostic.path, raised.exception.diagnostic.code), (path, code))

    def test_not_authored_properties(self):
        for name in ("FirstNode", "AllNodes", "Duration", "bHasDelayNode"):
            with self.assertRaises(AssetDocumentContractError) as raised:
                sound_cue.canonicalize(_document({}, {name: 1}))
            self.assertEqual(raised.exception.diagnostic.code, "SoundCue.NotAuthoredProperty")

    def test_canonical_positions_and_classes(self):
        canonical = sound_cue.canonicalize(_document({"Nodes": {"A": {"Class": "SoundNodeMixer", "Position": [1.4, -2.6]}}}))
        self.assertEqual(canonical["Body"]["Nodes"]["A"], {"Class": "Mixer", "Position": [1, -3]})


class SoundCueExtractDiffTests(unittest.TestCase):
    def test_extract(self):
        body = _body(_footstep())
        self.assertEqual(body["Root"], {"Input": "Mod", "Position": [300, 0]})
        self.assertEqual(body["Nodes"]["Mod"], {"Class": "Modulator", "Position": [0, 0], "Properties": {"PitchMin": 0.9},
                                                "Inputs": ["Rand"]})
        self.assertEqual(body["Nodes"]["Rand"]["Inputs"], ["Step1", "Step2"])
        self.assertEqual(body["Nodes"]["Rand"]["Properties"], {"Weights": [1.0, 0.5]})
        self.assertNotIn("Inputs", body["Nodes"]["Step1"])
        self.assertEqual(body["Comments"], {"Note": {"Text": "Variations", "Position": [-650, -200], "Size": [400, 500]}})

    def test_diff_defaults(self):
        cue = _footstep()
        live = sound_cue.canonicalize(_document(_body(cue)))
        authored = copy.deepcopy(live)
        # Writing a default value, or leaving out a default Inputs list, compares equal.
        authored["Body"]["Nodes"]["Mod"]["Properties"]["PitchMax"] = 1.05
        authored["Body"]["Nodes"]["Step1"]["Properties"]["bLooping"] = False
        authored["Body"]["Comments"]["Note"]["FontSize"] = 18
        self.assertEqual(sound_cue.diff(authored, live, exact=False), ())
        authored["Body"]["Nodes"]["Rand"]["Inputs"] = ["Step2", "Step1"]
        del authored["Body"]["Nodes"]["Step1"]
        changes = sound_cue.diff(authored, live, exact=False)
        self.assertEqual({(item.path, item.kind) for item in changes},
                         {("/Body/Nodes/Rand/Inputs", DiffKind.REPLACE), ("/Body/Nodes/Step1", DiffKind.REMOVE)})


class SoundCueApplyTests(unittest.TestCase):
    def test_reapply_is_a_noop(self):
        cue = _footstep()
        _apply(cue, _body(cue))
        self.assertEqual(cue.writes, [])
        self.assertEqual(cue.finalized, 0)

    def test_from_scratch(self):
        cue = _Cue()
        body = {
            "Root": {"Input": "Mod"},
            "Nodes": {
                "Mod": {"Class": "Modulator", "Properties": {"PitchMin": 0.9, "PitchMax": 1.1}, "Inputs": ["Rand"]},
                "Rand": {"Class": "Random", "Properties": {"Weights": [1.0, 1.0, 0.5]}, "Inputs": ["A", "B", "C"]},
                "A": {"Class": "WavePlayer", "Properties": {"SoundWaveAssetPtr": "/Game/A.A"}},
                "B": {"Class": "WavePlayer", "Properties": {"SoundWaveAssetPtr": "/Game/B.B"}},
                "C": {"Class": "WavePlayer", "Properties": {"SoundWaveAssetPtr": "/Game/C.C"}, "Comment": "rare",
                      "CommentVisible": True}},
            "Comments": {"Box": {"Text": "Variations", "Position": [-700, -200], "Size": [450, 500], "FontSize": 24}}}
        _apply(cue, body)
        self.assertEqual(cue.finalized, 1)
        extracted = _body(cue)
        self.assertEqual(sound_cue.diff(sound_cue.canonicalize(_document(body)),
                                        sound_cue.canonicalize(_document(extracted)), exact=False), ())
        # New nodes without a Position go left of their first parent.
        self.assertEqual(extracted["Nodes"]["Mod"]["Position"], [0, 0])
        self.assertEqual(extracted["Nodes"]["Rand"]["Position"], [-300, 0])
        self.assertEqual(extracted["Nodes"]["C"]["Position"], [-600, 300])
        renames = [args["name"] for primitive, args in cue.writes if primitive == "graph.node.rename.v1"]
        self.assertEqual(renames, ["Mod", "Rand", "A", "B", "C", "Box"])
        cue.writes.clear()
        _apply(cue, body)
        self.assertEqual(cue.writes, [])

    def test_free_edits(self):
        cue = _footstep()
        body = _body(cue)
        # Insert an Attenuation-like Mixer between Mod and Rand, reorder the variations with their weights, add a third.
        body["Nodes"]["Mix"] = {"Class": "Mixer", "Inputs": ["Rand"], "Position": [-150, 0]}
        body["Nodes"]["Mod"]["Inputs"] = ["Mix"]
        body["Nodes"]["Step3"] = {"Class": "WavePlayer", "Properties": {"SoundWaveAssetPtr": "/Game/C.C"}}
        body["Nodes"]["Rand"]["Inputs"] = ["Step2", "Step1", "Step3"]
        body["Nodes"]["Rand"]["Properties"]["Weights"] = [0.5, 1.0, 2.0]
        _apply(cue, body)
        after = _body(cue)
        self.assertEqual(after["Nodes"]["Mod"]["Inputs"], ["Mix"])
        self.assertEqual(after["Nodes"]["Mix"]["Inputs"], ["Rand"])
        self.assertEqual(after["Nodes"]["Rand"]["Inputs"], ["Step2", "Step1", "Step3"])
        self.assertEqual(after["Nodes"]["Rand"]["Properties"]["Weights"], [0.5, 1.0, 2.0])
        kinds = _kinds(cue)
        # Every break of a changed slot comes before the first connect.
        self.assertLess(max(i for i, kind in enumerate(kinds) if kind == "graph.pin.break.v1"),
                        min(i for i, kind in enumerate(kinds) if kind == "graph.pin.connect.v1"))

    def test_delete_input_and_class_change(self):
        cue = _footstep()
        body = _body(cue)
        body["Nodes"]["Rand"]["Inputs"] = ["Step2"]
        body["Nodes"]["Rand"]["Properties"]["Weights"] = [0.5]
        body["Nodes"]["Mod"] = {"Class": "Mixer", "Inputs": ["Rand"], "Position": [0, 0]}
        del body["Nodes"]["Step1"]
        del body["Comments"]["Note"]
        _apply(cue, body)
        after = _body(cue)
        self.assertEqual(after["Nodes"]["Rand"]["Inputs"], ["Step2"])
        self.assertEqual(after["Nodes"]["Mod"]["Class"], "Mixer")
        self.assertEqual(after["Root"]["Input"], "Mod")
        self.assertNotIn("Step1", after["Nodes"])
        self.assertEqual(after["Comments"], {})

    def _refused(self, cue, body, path, code):
        with self.assertRaises(AssetDocumentContractError) as raised:
            _apply(cue, body)
        self.assertEqual((raised.exception.diagnostic.path, raised.exception.diagnostic.code), (path, code))

    def test_refusals_before_changes(self):
        cases = [
            (lambda b: b["Nodes"]["Rand"].update(Inputs=[]), "/Body/Nodes/Rand/Inputs", "SoundCue.ChildCount"),
            (lambda b: b["Nodes"].update(Br={"Class": "Branch", "Inputs": [None, None]}), "/Body/Nodes/Br/Inputs",
             "SoundCue.ChildCount"),
            (lambda b: b["Nodes"]["Rand"]["Properties"].update(Weights=[1.0]), "/Body/Nodes/Rand/Properties/Weights",
             "SoundCue.ChildArrayLength"),
            (lambda b: b["Nodes"].update(X={"Class": "Nope"}), "/Body/Nodes/X/Class", "SoundCue.UnknownNodeClass"),
            (lambda b: b["Nodes"]["Mod"]["Properties"].update(Volume=1), "/Body/Nodes/Mod/Properties/Volume",
             "SoundCue.UnknownProperty"),
        ]
        for edit, path, code in cases:
            with self.subTest(code=code, path=path):
                cue = _footstep()
                body = _body(cue)
                edit(body)
                self._refused(cue, body, path, code)
                self.assertEqual(cue.writes, [])

    def test_open_editor_applies(self):
        # The Sound Cue editor edits the cue itself; Core refreshes it after the apply.
        cue = _footstep()
        cue.open = True
        body = _body(cue)
        body["Nodes"]["Mod"]["Properties"]["PitchMin"] = 0.8
        _apply(cue, body)
        self.assertEqual(_body(cue)["Nodes"]["Mod"]["Properties"]["PitchMin"], 0.8)

    def test_cycle_refused_at_the_slot(self):
        cue = _footstep()
        body = _body(cue)
        body["Nodes"]["Rand"]["Inputs"] = ["Step1", "Mod"]
        self._refused(cue, body, "/Body/Nodes/Rand/Inputs/1", "SoundCue.ConnectionCycle")

    def test_new_validation_error_fails_old_one_warns(self):
        cue = _footstep()
        body = _body(cue)
        body["Nodes"]["D"] = {"Class": "Delay", "Inputs": []}
        self._refused(cue, body, "/Body/Nodes/D", "SoundCue.ValidationFailed")
        # The same error already in the cue: a warning.
        cue = _footstep()
        cue.add_sound("D", "Delay", inputs=[])
        body = _body(cue)
        body["Nodes"]["Mod"]["Properties"]["PitchMin"] = 0.8
        result = _apply(cue, body)
        codes = [item.code for item in getattr(result, "diagnostics", ())]
        self.assertIn("SoundCue.ValidationError", codes)

    def test_delete_input_picks_the_matching_slot(self):
        from asset_document.profiles.sound_cue_apply import _slots_to_remove
        self.assertEqual(_slots_to_remove(["a", "b", "c", "d"], ["a", "c", "d"]), [1])
        self.assertEqual(_slots_to_remove(["a", "b", "c", "d"], ["a", "b", "c"]), [3])
        self.assertEqual(_slots_to_remove([None, None, None], [None]), [1, 2])
        self.assertEqual(_slots_to_remove(["a", "b", "c", "d"], ["b", "d"]), [0, 2])

    def test_float32_values_extract_as_short_decimals(self):
        self.assertEqual(model.tidy({"V": [1.0, 0.6000000238418579], "P": 0.9200000166893005, "N": "x"}),
                         {"V": [1.0, 0.6], "P": 0.92, "N": "x"})
        self.assertEqual(model.tidy(0.1), 0.1)  # not a float32 value: kept as is

    def test_omitted_regions_keep(self):
        cue = _footstep()
        _apply(cue, {"Root": {"Input": "Rand"}})
        after = _body(cue)
        self.assertEqual(after["Root"]["Input"], "Rand")
        self.assertIn("Mod", after["Nodes"])
        self.assertIn("Note", after["Comments"])


if __name__ == "__main__":
    unittest.main()
