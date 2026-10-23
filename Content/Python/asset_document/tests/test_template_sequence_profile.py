# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Template / Camera Animation Sequence Profiles: Body.Root <-> the level binding form, comparison form and Diff, the
apply plan over the root (document paths, root entry, component bindings, class change first), refusals and the create
context. The editor side (round trips, change of class, camera shake playback) is covered by
UEAssetDoc.Sequencer.Template.*."""
from __future__ import annotations

import copy
import unittest
from unittest import mock

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.profiles import level_sequence as seq
from asset_document.profiles import level_sequence_apply as apply_module
from asset_document.profiles import template_sequence as tseq

TARGET = "/Game/Cinematics/TS_Test"
ROOT = "0000000000000000000000000000000A"
CAMERA = "0000000000000000000000000000000B"
CINE = "/Script/CinematicCamera.CineCameraActor"
SETTINGS = {"DisplayRate": {"Numerator": 30, "Denominator": 1}, "TickResolution": {"Numerator": 24000, "Denominator": 1},
            "Evaluation": "WithSubFrames", "PlaybackRange": {"Start": 0, "End": 150}}


def _records():
    return [
        {"kind": "header", "settings": SETTINGS, "folders": [], "markers": [], "director": None, "metadata": {},
         "counts": {"bindings": 2, "tracks": 3}},
        {"kind": "binding", "id": ROOT, "parent": "", "entry": {"Name": "CineCameraActor", "Root": {"Class": CINE}}},
        {"kind": "binding", "id": CAMERA, "parent": ROOT, "entry": {
            "Name": "CameraComponent", "Class": "/Script/CinematicCamera.CineCameraComponent", "Component": "CameraComponent"}},
        {"kind": "track", "binding": ROOT, "object": "Spawn_0", "entry": {"Type": "Spawn"}},
        {"kind": "section", "object": "Bool_0", "entry": {"Channels": {"Value": {"Default": True}}}},
        {"kind": "track", "binding": ROOT, "object": "Transform_0", "entry": {"Type": "Transform"}},
        {"kind": "section", "object": "Transform_S0", "entry": {"Channels": {"Location.X": {}}}},
        {"kind": "keys", "channel": "Location.X", "keys": [{"Frame": 0, "Value": 0}, {"Frame": 30, "Value": 50}]},
        {"kind": "track", "binding": CAMERA, "object": "Float_0", "entry": {"Type": "Float", "Property": "CurrentFocalLength"}},
        {"kind": "section", "object": "Float_S0", "entry": {"Channels": {"Value": {}}}},
        {"kind": "keys", "channel": "Value", "keys": [{"Frame": 0, "Value": 35}]},
    ]


def _level(ids=False):
    body, _ = seq.assemble(_records(), ids=ids)
    return body


def _body():
    return tseq.template_body(_level())


def _document(body, class_path=tseq.TEMPLATE_CLASS, properties=None):
    return {"SchemaVersion": 1, "Class": class_path, "Action": "Update", "Target": TARGET,
            "Properties": properties or {}, "Body": body}


def _plan(body):
    level = _level(ids=True)
    live = tseq.template_body(level)
    canonical = tseq.canonicalize(_document(body), tseq.TEMPLATE_CLASS)["Body"]
    internal = {key: value for key, value in canonical.items() if key != "Root"}
    if "Root" in canonical:
        internal["Bindings"] = [tseq.to_internal(canonical["Root"], live.get("Root"))]
    plan = apply_module.Plan(internal, level, root_path="/Body/Root")
    plan.match()
    return plan


def _ops(items):
    return [item.op for item in items]


class FormTests(unittest.TestCase):
    def test_extracted_body(self):
        body = _body()
        self.assertEqual(list(body), ["Settings", "Root", "Tracks"])
        root = body["Root"]
        self.assertEqual(root["Name"], "CineCameraActor")
        self.assertEqual(root["Class"], CINE)
        self.assertEqual([track["Type"] for track in root["Tracks"]], ["Spawn", "Transform"])
        # Component equal to the Name is omitted.
        self.assertEqual(root["Children"], [{"Name": "CameraComponent", "Class": "/Script/CinematicCamera.CineCameraComponent",
                                             "Tracks": root["Children"][0]["Tracks"]}])

    def test_internal_round_trip(self):
        root = _body()["Root"]
        internal = tseq.to_internal(root, root)
        self.assertEqual(internal["Root"], {"Class": CINE})
        self.assertEqual(internal["Children"][0]["Component"], "CameraComponent")
        self.assertNotIn("RenamedFrom", internal)
        self.assertEqual(tseq.from_internal(internal), root)

    def test_component_other_than_name_and_none(self):
        root = {"Name": "R", "Class": CINE, "Children": [{"Name": "Lens", "Component": "CameraComponent"}, {"Name": "Ghost", "Component": ""}]}
        internal = tseq.to_internal(root)
        self.assertEqual([child["Component"] for child in internal["Children"]], ["CameraComponent", ""])
        self.assertEqual(tseq.from_internal(internal)["Children"], [{"Name": "Lens", "Component": "CameraComponent"},
                                                                    {"Name": "Ghost", "Component": ""}])

    def test_omitted_name_and_class_keep_the_live_ones(self):
        live = _body()["Root"]
        authored = copy.deepcopy(live)
        del authored["Name"], authored["Class"]
        self.assertEqual(tseq.normalize_root(authored, live), tseq.normalize_root(live, live))

    def test_shape_errors(self):
        cases = [
            ({"Root": {"Bind": []}}, "Sequencer.UnknownKey"),
            ({"Root": {"Class": 3}}, "Sequencer.InvalidBody"),
            ({"Root": {"Class": CINE, "Children": [{"Name": "C", "Children": []}]}}, "Sequencer.UnknownKey"),
            ({"Bindings": []}, None),
        ]
        for body, code in cases:
            with self.subTest(body=body):
                with self.assertRaises(AssetDocumentContractError) as raised:
                    tseq.canonicalize(_document(body), tseq.TEMPLATE_CLASS)
                if code is not None:
                    self.assertEqual(raised.exception.diagnostic.code, code)


class DiffTests(unittest.TestCase):
    def test_extract_has_no_differences(self):
        document = _document(_body())
        self.assertEqual(tseq.TemplateSequenceRuntimeProfile().diff(document, copy.deepcopy(document)), ())

    def test_default_spawn_track_and_component(self):
        live = _body()
        authored = copy.deepcopy(live)
        del authored["Root"]["Tracks"][0]  # the default Spawn track
        authored["Root"]["Children"][0]["Component"] = "CameraComponent"
        self.assertEqual(tseq.body_diff(authored, live, exact=False), [])

    def test_paths_are_template_paths(self):
        live = _body()
        authored = copy.deepcopy(live)
        authored["Root"]["Children"][0]["Component"] = "Other"
        authored["Root"]["Ownership"] = "External"
        paths = [item.path for item in tseq.body_diff(authored, live, exact=False)]
        self.assertEqual(sorted(paths), ["/Body/Root/Children/0/Component", "/Body/Root/Ownership"])

    def test_profile_paths_point_into_the_document(self):
        # An Update whose Root lists one track (no Spawn track) reported /Body/Root/Tracks/1/...
        live = _body()
        authored = copy.deepcopy(live)
        del authored["Root"]["Tracks"][0]  # the default Spawn track
        authored["Root"]["Tracks"][0]["Sections"][0]["Channels"]["Location.X"]["Keys"][1]["Value"] = 75
        changes = tseq.CameraAnimationSequenceRuntimeProfile().diff(_document(authored, tseq.CAMERA_CLASS),
                                                                    _document(live, tseq.CAMERA_CLASS))
        self.assertEqual([item.path for item in changes], ["/Body/Root/Tracks/0/Sections/0/Channels/Location.X/Keys/1/Value"])

    def test_missing_root_differs(self):
        live = _body()
        del live["Root"]
        self.assertEqual([item.path for item in tseq.body_diff({"Root": {"Class": CINE}}, live, exact=False)], ["/Body/Root"])


class PlanTests(unittest.TestCase):
    def test_unchanged_plans_nothing(self):
        plan = _plan(_body())
        self.assertEqual((_ops(plan.structure()), _ops(plan.content()), _ops(plan.finish())), ([], [], []))

    def test_root_entry_component_binding_and_paths(self):
        body = _body()
        body["Root"]["Name"] = "Camera"
        body["Root"]["Template"] = {"Properties": {"bHidden": True}}
        body["Root"]["Children"].append({"Name": "Mesh", "Class": "/Script/Engine.StaticMeshComponent",
                                         "Tracks": [{"Type": "Visibility", "Property": "bHiddenInGame"}]})
        plan = _plan(body)
        structure = plan.structure()
        ops = _ops(structure)
        self.assertEqual(ops[0], {"op": "binding.rename", "guid": ROOT, "name": "Camera"})
        self.assertEqual(ops[1]["op"], "binding.add")
        self.assertEqual(ops[1]["parent"], ROOT)
        self.assertEqual(structure[1].path, "/Body/Root/Children/1")
        content = plan.content()
        sets = [item for item in content if item.op["op"] == "binding.set"]
        self.assertEqual(sets[0].op["entry"], {"Root": {"Class": CINE, "Template": {"Properties": {"bHidden": True}}}})
        self.assertEqual(apply_module.op_path("/ops/0/entry/Root/Template/Properties/bHidden", [sets[0]]),
                         "/Body/Root/Template/Properties/bHidden")
        self.assertEqual(sets[1].op["entry"], {"Class": "/Script/Engine.StaticMeshComponent", "Component": "Mesh"})

    def test_removed_component_binding(self):
        body = _body()
        del body["Root"]["Children"]
        ops = _ops(_plan(body).structure())
        self.assertEqual(ops, [{"op": "binding.remove", "guid": CAMERA}])

    def test_root_without_spawn_track_keeps_none(self):
        records = [record for record in _records() if record.get("object") not in ("Spawn_0", "Bool_0")]
        level, _ = seq.assemble(records, ids=True)
        live = tseq.template_body(level)
        internal = {"Bindings": [tseq.to_internal(live["Root"], live["Root"])]}
        plan = apply_module.Plan(internal, level, root_path="/Body/Root")
        plan.match()
        self.assertEqual(_ops(plan.structure()), [])


class ApplyTests(unittest.TestCase):
    def _edit_outcome(self):
        outcome = mock.MagicMock()
        outcome.changed_set = ("changed",)
        outcome.value = {"changed": True, "created": [], "dependents": []}
        return outcome

    def test_class_change_runs_first_alone(self):
        body = _body()
        body["Root"]["Class"] = "/Script/Engine.CameraActor"
        reads = iter([(_level(ids=True), {})] * 3)
        with mock.patch.object(seq, "read_body", side_effect=lambda calls, ids=False: next(reads)), \
                mock.patch.object(apply_module, "status", return_value={"Messages": []}), \
                mock.patch.object(apply_module.common, "invoke", return_value=self._edit_outcome()) as invoke:
            tseq.TemplateSequenceRuntimeProfile().apply(_document(body), object(), object(), mock.MagicMock())
        first = invoke.call_args_list[0].args[3]["ops"]
        self.assertEqual(first, [{"op": "template.root", "class": "/Script/Engine.CameraActor"}])

    def test_unchanged_apply_writes_nothing(self):
        with mock.patch.object(seq, "read_body", return_value=(_level(ids=True), {})), \
                mock.patch.object(apply_module.common, "invoke") as invoke:
            self.assertEqual(tseq.TemplateSequenceRuntimeProfile().apply(_document(_body()), object(), object(), mock.MagicMock()), ())
        invoke.assert_not_called()

    def test_refusals(self):
        cases = [
            ({"Root": {"Name": "R"}}, {}, "/Body/Root/Class", "Sequencer.RootClassRequired"),
            ({"Root": {"Class": CINE, "Folder": "Nope"}}, _body(), "/Body/Root/Folder", "Sequencer.InvalidFolder"),
            ({"Root": {"Class": CINE, "Children": [{"Name": "C", "Id": "x"}]}}, _body(), "/Body/Root/Children/0/Id",
             "Sequencer.InvalidBody"),
        ]
        for body, live, path, code in cases:
            with self.subTest(path=path):
                with self.assertRaises(AssetDocumentContractError) as raised:
                    tseq.refuse(body, live, tseq.TEMPLATE_CLASS)
                self.assertEqual((raised.exception.diagnostic.path, raised.exception.diagnostic.code), (path, code))

    def test_create_context(self):
        handler = tseq.TemplateSequenceRuntimeProfile()
        with self.assertRaises(AssetDocumentContractError) as raised:
            handler.create_target_context(_document({}), object())
        self.assertEqual(raised.exception.diagnostic.code, "Sequencer.RootClassRequired")
        camera = tseq.CameraAnimationSequenceRuntimeProfile()
        self.assertEqual(camera.create_target_context(_document({}, tseq.CAMERA_CLASS), object()), ())

    def test_registrations(self):
        self.assertEqual(tseq.TEMPLATE_SEQUENCE_RUNTIME_REGISTRATION.profile_meta.profile_id, "profile.template_sequence.v1")
        self.assertEqual(tseq.CAMERA_ANIMATION_SEQUENCE_RUNTIME_REGISTRATION.profile_meta.exact_asset_class_path, tseq.CAMERA_CLASS)


if __name__ == "__main__":
    unittest.main()
