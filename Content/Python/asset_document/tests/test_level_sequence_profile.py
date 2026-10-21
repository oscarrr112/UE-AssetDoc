"""Level Sequence Profile: assembling the Body from the seq.read.v1 records (binding tree, tracks, sections, key and
columnar key records, warnings), canonical and comparison forms, Diff, and the apply plan (identity, op batches, dense
channel chunks, refusals, outcome). The editor side (primitives, status, corpus round trips, free edits, rollback) is
covered by UEAssetDoc.Sequencer.*."""
from __future__ import annotations

import copy
import unittest
from unittest import mock

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.profiles import level_sequence as seq
from asset_document.profiles import level_sequence_apply as apply_module
from asset_document.profiles import sequencer_director as director

TARGET = "/Game/Cinematics/LS_Test"
CAMERA = "0000000000000000000000000000000A"
COMPONENT = "0000000000000000000000000000000B"
DOOR = "0000000000000000000000000000000C"
SETTINGS = {"DisplayRate": {"Numerator": 30, "Denominator": 1}, "TickResolution": {"Numerator": 24000, "Denominator": 1},
            "Evaluation": "WithSubFrames", "PlaybackRange": {"Start": 0, "End": 150}}


def _records():
    return [
        {"kind": "header", "settings": SETTINGS, "folders": [{"Path": "Cameras"}], "markers": [{"Frame": 60, "Label": "Open"}],
         "director": None, "metadata": {"Notes": "n"}, "counts": {"bindings": 3, "tracks": 4}},
        {"kind": "binding", "id": CAMERA, "parent": "", "entry": {"Name": "Cam", "Folder": "Cameras",
                                                                   "Spawnable": {"Class": "/Script/CinematicCamera.CineCameraActor"}}},
        {"kind": "binding", "id": COMPONENT, "parent": CAMERA, "entry": {"Name": "CameraComponent",
                                                                         "Bind": ["uobj://subobj?payload0=CameraComponent"]}},
        {"kind": "binding", "id": DOOR, "parent": "", "entry": {"Name": "Door", "Bind": []}},
        {"kind": "track", "binding": "", "entry": {"Type": "CameraCut"}},
        {"kind": "section", "entry": {"Start": 0, "End": 150, "Properties": {"CameraBindingID": {"Binding": "Cam"}},
                                      "Channels": {}}},
        {"kind": "track", "binding": CAMERA, "entry": {"Type": "Transform"}},
        {"kind": "section", "entry": {"Channels": {"Location.X": {}, "Location.Y": {}, "Location.Z": {"Default": 160}}}},
        {"kind": "keys", "channel": "Location.X", "keys": [{"Frame": 0, "Value": -600}, {"Frame": 150, "Value": -250}]},
        {"kind": "track", "binding": COMPONENT, "entry": {"Type": "Float", "Property": "CurrentFocalLength"}},
        {"kind": "section", "entry": {"Channels": {"Value": {}}}},
        {"kind": "columns", "channel": "Value", "frames": [0, 1], "values": [24, 25], "interp": ["Linear", "Linear"]},
        {"kind": "columns", "channel": "Value", "frames": [2], "values": [26], "interp": ["Linear"]},
        {"kind": "track", "binding": DOOR, "entry": {"Type": "Bool", "Property": "bHidden"}},
        {"kind": "section", "entry": {"Channels": {"Value": {}}}},
        {"kind": "columns", "channel": "Value", "frames": [0, 1], "values": [True, False]},
        {"kind": "warning", "code": "Sequencer.UnsupportedLocator", "message": "m", "path": "/Body/Bindings/1"},
    ]


def _document(body, properties=None):
    return {"SchemaVersion": 1, "Class": seq.CLASS, "Action": "Update", "Target": TARGET,
            "Properties": properties or {}, "Body": body}


def _body():
    body, _ = seq.assemble(_records())
    return body


class AssembleTests(unittest.TestCase):
    def test_parameter_key_records(self):
        records = _records()[:1] + [
            {"kind": "track", "binding": "", "entry": {"Type": "/Script/MovieSceneTracks.MovieSceneMaterialParameterCollectionTrack",
                                                       "RowConditions": {"0": {"Class": "/Script/MovieSceneTracks.MovieScenePlatformCondition"}}}},
            {"kind": "section", "entry": {"Channels": {}, "Parameters": [
                {"Type": "Scalar", "Name": "Glow", "Channels": {"ParameterCurve": {}}},
                {"Type": "Color", "Name": "Tint", "Channels": {"RedCurve": {"Default": 1}}, "Parameters": [{"Channels": {"Value": {}}}]}]}},
            {"kind": "keys", "channel": "ParameterCurve", "parameter": [0], "keys": [{"Frame": 0, "Value": 2}]},
            {"kind": "columns", "channel": "Value", "parameter": [1, 0], "frames": [0], "values": [3]},
        ]
        body, _ = seq.assemble(records)
        section = body["Tracks"][0]["Sections"][0]
        self.assertNotIn("Channels", section)
        self.assertEqual(section["Parameters"][0]["Channels"]["ParameterCurve"], {"Keys": [{"Frame": 0, "Value": 2}]})
        self.assertEqual(section["Parameters"][1]["Parameters"][0]["Channels"]["Value"], {"Frames": [0], "Values": [3]})
        self.assertEqual(list(body["Tracks"][0]["RowConditions"]), ["0"])
        canonical = seq.canonicalize(_document(body))
        self.assertEqual(seq.normalize_section(canonical["Body"]["Tracks"][0]["Sections"][0])["Parameters"][1]["Parameters"][0],
                         {"Channels": {"Value": {"Keys": [{"Frame": 0, "Value": 3}]}}})

    def test_parameters_are_validated(self):
        body = _body()
        body["Tracks"].append({"Type": "Fade", "Sections": [{"Parameters": [{"Name": "A", "Curve": {}}]}]})
        with self.assertRaises(AssetDocumentContractError) as raised:
            seq.canonicalize(_document(body))
        self.assertEqual(raised.exception.diagnostic.path, "/Body/Tracks/1/Sections/0/Parameters/0/Curve")

    def test_binding_tree_tracks_and_sections(self):
        body, facts = seq.assemble(_records())
        self.assertEqual(body["Settings"], SETTINGS)
        self.assertEqual([item["Name"] for item in body["Bindings"]], ["Cam", "Door"])
        camera = body["Bindings"][0]
        self.assertEqual(camera["Children"][0]["Name"], "CameraComponent")
        self.assertEqual(camera["Tracks"][0]["Type"], "Transform")
        channels = camera["Tracks"][0]["Sections"][0]["Channels"]
        # Empty channels are omitted (an omitted channel is cleared).
        self.assertEqual(channels, {"Location.X": {"Keys": [{"Frame": 0, "Value": -600}, {"Frame": 150, "Value": -250}]},
                                    "Location.Z": {"Default": 160}})
        self.assertEqual(body["Tracks"][0]["Type"], "CameraCut")
        self.assertNotIn("Channels", body["Tracks"][0]["Sections"][0])
        self.assertEqual(body["Folders"], [{"Path": "Cameras"}])
        self.assertEqual(body["Markers"], [{"Frame": 60, "Label": "Open"}])
        self.assertEqual(body["MetaData"], {"Notes": "n"})
        self.assertNotIn("Director", body)
        self.assertEqual(facts["warnings"], [{"Code": "Sequencer.UnsupportedLocator", "Message": "m",
                                              "Path": "/Body/Bindings/1"}])

    def test_columns_join_and_collapse(self):
        body = _body()
        focal = body["Bindings"][0]["Children"][0]["Tracks"][0]["Sections"][0]["Channels"]["Value"]
        self.assertEqual(focal, {"Frames": [0, 1, 2], "Values": [24, 25, 26], "Interp": "Linear"})
        hidden = body["Bindings"][1]["Tracks"][0]["Sections"][0]["Channels"]["Value"]
        self.assertEqual(hidden, {"Frames": [0, 1], "Values": [True, False]})

    def test_smart_auto_and_mixed_interp(self):
        channel = {}
        seq._join_columns(channel, {"frames": [0, 1], "values": [0, 1], "interp": ["SmartAuto", "SmartAuto"]})
        seq._collapse_columns(channel)
        self.assertNotIn("Interp", channel)
        channel = {}
        seq._join_columns(channel, {"frames": [0, 1], "values": [0, 1], "interp": ["Linear", "Constant"]})
        seq._collapse_columns(channel)
        self.assertEqual(channel["Interp"], ["Linear", "Constant"])

    def test_empty_sequence(self):
        body, _ = seq.assemble([{"kind": "header", "settings": SETTINGS, "folders": [], "markers": [], "director": None,
                                 "metadata": {}}])
        self.assertEqual(body, {"Settings": SETTINGS, "Bindings": [], "Tracks": []})

    def test_director_header_is_a_fact(self):
        # The Director region is read through the Blueprint components (sequencer_director.extract); the header only
        # says that there is one and its class.
        director = {"Blueprint": "/Game/LS.LS:LS_DirectorBP", "Class": "/Game/LS.LS_DirectorBP_C"}
        body, facts = seq.assemble([{"kind": "header", "settings": SETTINGS, "director": director}])
        self.assertNotIn("Director", body)
        self.assertEqual(seq.director_class(facts, "/Game/LS"), "/Game/LS.LS_DirectorBP_C")
        _, facts = seq.assemble([{"kind": "header", "settings": SETTINGS, "director": None}])
        self.assertIsNone(seq.director_class(facts, "/Game/LS"))

    def test_node_groups(self):
        groups = [{"Name": "Hero", "Members": [{"Binding": "Hero"}, {"Folder": "Set"}]}]
        body, _ = seq.assemble([{"kind": "header", "settings": SETTINGS, "groups": groups}])
        self.assertEqual(body["Groups"], groups)


class CanonicalTests(unittest.TestCase):
    def test_omitted_meanings_are_dropped(self):
        body = {"Tracks": [{"Type": "Fade", "Muted": False, "Sections": [
            {"Row": 0, "Active": True, "Locked": False, "Properties": {}, "Channels": {"Fade": {}}}]}],
            "Bindings": [{"Name": "A", "Bind": [], "Tracks": [], "Children": [], "Tags": []}]}
        canonical = seq.canonicalize(_document(body))
        self.assertEqual(canonical["Body"]["Tracks"], [{"Type": "Fade", "Sections": [{}]}])
        # Bind: [] is a possessable with no reference (legal content), not an omission.
        self.assertEqual(canonical["Body"]["Bindings"], [{"Name": "A", "Bind": []}])

    def test_document_shape_errors(self):
        cases = [
            ({"Groups": {}}, "/Body/Groups", "Sequencer.InvalidBody"),
            ({"Groups": [{"Members": []}]}, "/Body/Groups/0/Name", "Sequencer.InvalidBody"),
            ({"Groups": [{"Name": "G", "Nodes": []}]}, "/Body/Groups/0/Nodes", "Sequencer.UnknownKey"),
            ({"Tracks": [{"Type": "Fade", "Sections": [{"Group": 0}]}]}, "/Body/Tracks/0/Sections/0/Group",
             "Sequencer.InvalidBody"),
            ({"Tracks": [{"Type": "Fade", "Sections": [{"Group": 1.5}]}]}, "/Body/Tracks/0/Sections/0/Group",
             "Sequencer.InvalidBody"),
            ({"Tracks": [{"Type": "Fade", "Sections": [{"Group": True}]}]}, "/Body/Tracks/0/Sections/0/Group",
             "Sequencer.InvalidBody"),
            ({"Director": []}, "/Body/Director", "Sequencer.InvalidBody"),
            ({"Director": {"Components": []}}, "/Body/Director/Components", "Sequencer.InvalidBody"),
            ({"Director": {"Functions": []}}, "/Body/Director/Functions", "Sequencer.InvalidBody"),
            ({"Director": {"Variables": [{"Type": "bool"}]}}, "/Body/Director/Variables/0/Name",
             "UBlueprint.InvalidBodyRegion"),
            ({"Settings": {"Rate": 1}}, "/Body/Settings/Rate", "Sequencer.UnknownKey"),
            ({"Bindings": {}}, "/Body/Bindings", "Sequencer.InvalidBody"),
            ({"Bindings": [{"Bind": []}]}, "/Body/Bindings/0/Name", "Sequencer.InvalidBody"),
            ({"Bindings": [{"Name": "A", "Bind": [], "Spawnable": {}}]}, "/Body/Bindings/0", "Sequencer.InvalidBody"),
            ({"Tracks": [{"Sections": []}]}, "/Body/Tracks/0/Type", "Sequencer.InvalidBody"),
            ({"Tracks": [{"Type": "Fade", "Sections": [{"Range": 1}]}]}, "/Body/Tracks/0/Sections/0/Range",
             "Sequencer.UnknownKey"),
            ({"Tracks": [{"Type": "Fade", "Sections": [{"Channels": {"Fade": {"Keys": [], "Frames": []}}}]}]},
             "/Body/Tracks/0/Sections/0/Channels/Fade", "Sequencer.InvalidBody"),
            ({"Tracks": [{"Type": "Fade", "Sections": [{"Channels": {"Fade": {"Frames": [0, 1], "Values": [1]}}}]}]},
             "/Body/Tracks/0/Sections/0/Channels/Fade/Values", "Sequencer.InvalidBody"),
            ({"Markers": [{"Frame": 1, "Name": "x"}]}, "/Body/Markers/0/Name", "Sequencer.UnknownKey"),
        ]
        for body, path, code in cases:
            with self.subTest(path=path):
                with self.assertRaises(AssetDocumentContractError) as raised:
                    seq.canonicalize(_document(body))
                self.assertEqual(raised.exception.diagnostic.path, path)
                self.assertEqual(raised.exception.diagnostic.code, code)


class DiffTests(unittest.TestCase):
    def test_extract_has_no_differences(self):
        document = seq.canonicalize(_document(_body()))
        self.assertEqual(seq.diff(document, document, exact=True), ())

    def test_omitted_regions_and_sparse_settings(self):
        live = seq.canonicalize(_document(_body()))
        authored = seq.canonicalize(_document({"Settings": {"Evaluation": "WithSubFrames"}}))
        self.assertEqual(seq.diff(authored, live, exact=False), ())
        authored = seq.canonicalize(_document({"Settings": {"Evaluation": "FrameLocked"}}))
        self.assertEqual([item.path for item in seq.diff(authored, live, exact=False)], ["/Body/Settings/Evaluation"])

    def test_omitted_settings_flags_and_metadata_are_reset(self):
        # A listed Settings / MetaData is complete. Deleting a flag or Notes from an extract was kept silently.
        body = _body()
        body["Settings"] = dict(SETTINGS, PlaybackRangeLocked=True, ShowMarkers=True, Clock="Platform")
        live = seq.canonicalize(_document(body))
        authored = seq.canonicalize(_document({"Settings": dict(SETTINGS, ShowMarkers=True), "MetaData": {}}))
        changes = seq.diff(authored, live, exact=False)
        self.assertEqual([(item.path, item.kind) for item in changes],
                         [("/Body/Settings/Clock", seq.DiffKind.REMOVE),
                          ("/Body/Settings/PlaybackRangeLocked", seq.DiffKind.REMOVE),
                          ("/Body/MetaData/Notes", seq.DiffKind.REMOVE)])
        self.assertEqual(getattr(changes, "diagnostics", ()), ())
        # A listed default the read leaves out is no difference.
        authored = seq.canonicalize(_document({"Settings": dict(SETTINGS, ReadOnly=False, PlaybackRangeLocked=True,
                                                                ShowMarkers=True, Clock="Platform"),
                                               "MetaData": {"Notes": "n", "Author": ""}}))
        self.assertEqual(seq.diff(authored, live, exact=True), ())

    def test_omitted_timing_settings_are_kept_and_diff_says_so(self):
        live = seq.canonicalize(_document(_body()))
        authored = seq.canonicalize(_document({"Settings": {"PlaybackRange": {"Start": 0, "End": 150}}}))
        changes = seq.diff(authored, live, exact=False)
        self.assertEqual(tuple(changes), ())
        notes = getattr(changes, "diagnostics", ())
        self.assertEqual([(note.code, note.path, note.severity.value) for note in notes],
                         [("Sequencer.OmittedKept", "/Body/Settings", "Info")])
        self.assertIn("DisplayRate, TickResolution, Evaluation", notes[0].message)
        profile = seq.LevelSequenceRuntimeProfile()
        self.assertEqual(len(getattr(profile.diff(authored, live), "diagnostics", ())), 1)

    def test_first_changed_key(self):
        body = _body()
        body["Bindings"][0]["Tracks"][0]["Sections"][0]["Channels"]["Location.X"]["Keys"][1]["Value"] = -200
        changes = seq.diff(seq.canonicalize(_document(body)), seq.canonicalize(_document(_body())), exact=False)
        # Paths are in the comparison form: the spawnable "Cam" lists no Spawn track, so it compares with the default
        # one at index 0.
        self.assertEqual([item.path for item in changes],
                         ["/Body/Bindings/0/Tracks/1/Sections/0/Channels/Location.X/Keys/1/Value"])

    def test_profile_paths_point_into_the_document(self):
        # Validate's change paths were comparison-form paths (the default Spawn track a spawnable leaves
        # out is index 0 there); the profile reports paths into the authored document.
        body = _body()
        body["Bindings"][0]["Tracks"][0]["Sections"][0]["Channels"]["Location.X"]["Keys"][1]["Value"] = -200
        body["Tracks"].insert(0, {"Type": "Fade"})  # the camera cut track compares first
        body["Markers"] = [{"Frame": 90, "Label": "Close"}, {"Frame": 60, "Label": "Open"}]  # markers compare by frame
        changes = seq.LevelSequenceRuntimeProfile().diff(_document(body), _document(_body()))
        self.assertEqual(sorted(item.path for item in changes),
                         ["/Body/Bindings/0/Tracks/0/Sections/0/Channels/Location.X/Keys/1/Value", "/Body/Markers/0",
                          "/Body/Tracks/0"])
        self.assertEqual(seq.document_path("/Body/Bindings/0/Tracks/0/Sections/0", seq.body_index_maps(body)),
                         "/Body/Bindings/0/Tracks")

    def test_float_leniency(self):
        body = _body()
        body["Bindings"][0]["Tracks"][0]["Sections"][0]["Channels"]["Location.Z"]["Default"] = 160.000001
        live = seq.canonicalize(_document(_body()))
        self.assertEqual(seq.diff(seq.canonicalize(_document(body)), live, exact=False), ())
        self.assertEqual(len(seq.diff(seq.canonicalize(_document(body)), live, exact=True)), 1)

    def test_removed_binding_and_explicit_defaults(self):
        body = _body()
        body["Bindings"][1]["Tracks"][0]["Sections"][0]["Row"] = 0
        live = seq.canonicalize(_document(_body()))
        self.assertEqual(seq.diff(seq.canonicalize(_document(body)), live, exact=False), ())
        del body["Bindings"][1]
        self.assertEqual([item.path for item in seq.diff(seq.canonicalize(_document(body)), live, exact=False)],
                         ["/Body/Bindings/1"])


def _live():
    """The live Body with the plan's private ids (seq.read_body(ids=True))."""
    records = _records()
    objects = iter(f"Object_{index}" for index in range(100))
    for record in records:
        if record["kind"] in ("track", "section"):
            record["object"] = next(objects)
    body, _ = seq.assemble(records, ids=True)
    return body


def _ops(plan):
    return [item.op for item in plan]


def _plan(body, live=None):
    plan = apply_module.Plan(seq.canonicalize(_document(body))["Body"], live or _live())
    plan.match()
    return plan


class NormalizeTests(unittest.TestCase):
    def test_spawnable_has_the_default_spawn_track(self):
        bindings = [{"Name": "Cam", "Spawnable": {"Class": "/Script/CinematicCamera.CineCameraActor"}}]
        normalized = seq.normalize_region("Bindings", bindings)
        self.assertEqual(normalized[0]["Tracks"], [seq.SPAWN_TRACK])
        listed = [{"Name": "Cam", "Spawnable": {"Class": "C"}, "Tracks": [seq.SPAWN_TRACK]}]
        self.assertEqual(seq.normalize_region("Bindings", listed)[0]["Tracks"], [seq.SPAWN_TRACK])

    def test_key_forms_compare_equal(self):
        keys = {"Keys": [{"Frame": 0, "Value": 1, "Interp": "SmartAuto", "WeightMode": "Both"},
                         {"Frame": 1, "Value": 2, "Interp": "User", "Arrive": 0.25}], "PreInfinity": "Constant"}
        columns = {"Frames": [0, 1], "Values": [1, 2], "Interp": ["SmartAuto", "User"], "Arrive": [None, 0.25]}
        self.assertEqual(seq.canonical_channel(keys), seq.canonical_channel(columns))
        self.assertEqual(seq.canonical_channel(columns)["Keys"],
                         [{"Frame": 0, "Value": 1}, {"Frame": 1, "Value": 2, "Interp": "User", "Arrive": 0.25}])

    def test_stated_tangents_compare(self):
        # A key other than User / Break with Arrive / Leave states its stored tangents; they are compared.
        stated = {"Keys": [{"Frame": 0, "Value": 1, "Interp": "Linear", "Arrive": 0, "Leave": 0.5}]}
        computed = {"Keys": [{"Frame": 0, "Value": 1, "Interp": "Linear"}]}
        self.assertNotEqual(seq.canonical_channel(stated), seq.canonical_channel(computed))
        self.assertEqual(seq.canonical_channel(stated)["Keys"][0]["Leave"], 0.5)

    def test_renamed_from_and_private_ids_are_not_compared(self):
        live = _live()
        authored = _body()
        authored["Bindings"][1]["RenamedFrom"] = "Door"
        self.assertEqual(seq.body_diff(seq.canonicalize(_document(authored))["Body"], live, exact=False), [])

    def test_markers_compare_sorted(self):
        live = {"Markers": [{"Frame": 1}, {"Frame": 5}]}
        self.assertEqual(seq.body_diff({"Markers": [{"Frame": 5}, {"Frame": 1}]}, live, exact=False), [])


class PlanTests(unittest.TestCase):
    def test_unchanged_document_plans_nothing(self):
        plan = _plan(_body())
        self.assertEqual(_ops(plan.structure()), [])
        self.assertEqual(_ops(plan.content()), [])
        self.assertEqual(_ops(plan.finish()), [])

    def test_rename_keeps_the_guid(self):
        body = _body()
        body["Bindings"][1]["Name"] = "Gate"
        body["Bindings"][1]["RenamedFrom"] = "Door"
        plan = _plan(body)
        self.assertEqual(_ops(plan.structure()), [{"op": "binding.rename", "guid": DOOR, "name": "Gate"}])
        self.assertEqual(_ops(plan.content()), [])

    def test_delete_binding_takes_its_tracks(self):
        body = _body()
        del body["Bindings"][1]
        plan = _plan(body)
        self.assertEqual(_ops(plan.structure()), [{"op": "binding.remove", "guid": DOOR}])
        self.assertEqual(_ops(plan.content()), [])

    def test_new_binding_track_and_section(self):
        body = _body()
        body["Bindings"].append({"Name": "Lamp", "Class": "/Script/Engine.PointLight", "Bind": ["uobj://actor?payload0=L"],
                                 "Tracks": [{"Type": "Float", "Property": "Intensity", "Sections": [
                                     {"Start": 0, "End": 30, "Channels": {"Value": {"Keys": [{"Frame": 0, "Value": 1}]}}}]}]})
        plan = _plan(body)
        structure = plan.structure()
        add = _ops(structure)[0]
        self.assertEqual(add["op"], "binding.add")
        self.assertEqual(add["name"], "Lamp")
        self.assertRegex(add["guid"], "^[0-9A-F]{32}$")
        self.assertEqual(_ops(structure)[1], {"op": "track.add", "type": "Float", "binding": add["guid"], "property": "Intensity"})
        structure[1].created("MovieSceneFloatTrack_7")
        content = _ops(plan.content())
        self.assertEqual(content[0], {"op": "binding.set", "guid": add["guid"], "entry": {
            "Class": "/Script/Engine.PointLight", "Bind": ["uobj://actor?payload0=L"]}})
        self.assertEqual(content[1], {"op": "binding.order", "guids": [CAMERA, COMPONENT, DOOR, add["guid"]]})
        # Track order before the sections: a binding reference into a sub-sequence names its section by path.
        self.assertEqual(content[2], {"op": "track.order", "tracks": ["MovieSceneFloatTrack_7"]})
        self.assertEqual(content[3]["op"], "section.add")
        self.assertEqual(content[3]["track"], "MovieSceneFloatTrack_7")
        self.assertEqual(content[3]["entry"]["Channels"], {"Value": {"Keys": [{"Frame": 0, "Value": 1}]}})

    def test_id_is_kept_for_a_new_binding(self):
        guid = "ABCDEF0123456789ABCDEF0123456789"
        body = _body()
        body["Bindings"].append({"Name": "Door", "Id": guid, "Bind": []})
        add = _ops(_plan(body).structure())[0]
        self.assertEqual(add, {"op": "binding.add", "guid": guid, "name": "Door"})

    def test_sections_by_position(self):
        live = _live()
        track = live["Tracks"][0]
        track["Sections"] = [dict(track["Sections"][0], _Object=f"S{index}", Start=index * 10, End=index * 10 + 10)
                             for index in range(3)]
        body = _strip(copy.deepcopy(live))
        del body["Tracks"][0]["Sections"][1]
        plan = Plan_with(body, live)
        content = _ops(plan.content())
        self.assertEqual([op["op"] for op in content], ["section.remove", "section.set"])
        self.assertEqual(content[0]["section"], "S2")
        self.assertEqual((content[1]["section"], content[1]["entry"]["Start"]), ("S1", 20))

    def test_dense_channel_goes_in_chunks(self):
        body = _body()
        frames = list(range(3000))
        body["Tracks"].append({"Type": "Fade", "Sections": [{"Start": 0, "End": 3000,
                                                             "Channels": {"Fade": {"Frames": frames, "Values": frames}}}]})
        plan = _plan(body)
        structure = plan.structure()
        structure[-1].created("Fade_1")
        content = plan.content()
        add = next(item for item in content if item.op["op"] == "section.add")
        first = add.op["entry"]["Channels"]["Fade"]
        self.assertLess(seq.common.count_nodes(add.op), seq.common.OP_NODE_BUDGET)
        add.created("FadeSection_1")
        appends = [item.op for item in plan.finish() if item.op["op"] == "channel.append"]
        self.assertTrue(appends)
        self.assertEqual({op["section"] for op in appends}, {"FadeSection_1"})
        joined = first["Frames"] + [frame for op in appends for frame in op["keys"]["Frames"]]
        self.assertEqual(joined, frames)

    def test_dense_channel_chunks_keep_stated_tangents(self):
        # Every chunk's stated tangents survive the later appends: each append names the earlier stated keys, in the
        # (time-sorted) order they were written.
        body = _body()
        keys = [{"Frame": frame, "Value": frame % 7, "Arrive": 0.5, "Leave": 0.5} if frame % 3 == 0
                else {"Frame": frame, "Value": frame % 7, "Interp": "User", "Arrive": 1, "Leave": 1} if frame % 3 == 1
                else {"Frame": frame, "Value": frame % 7} for frame in range(1000)]
        unsorted = keys[500:] + keys[:500]
        body["Tracks"].append({"Type": "Fade", "Sections": [{"Start": 0, "End": 1000, "Channels": {"Fade": {"Keys": unsorted}}}]})
        plan = _plan(body)
        plan.structure()[-1].created("Fade_1")
        add = next(item for item in plan.content() if item.op["op"] == "section.add")
        add.created("FadeSection_1")
        first = add.op["entry"]["Channels"]["Fade"]
        appends = [item.op for item in plan.finish() if item.op["op"] == "channel.append"]
        self.assertGreater(len(appends), 1)
        self.assertEqual(first["Frames"] + [frame for op in appends for frame in op["keys"]["Frames"]], list(range(1000)))
        done = len(first["Frames"])
        for op in appends:
            stated = [start + offset for start, count in op["stated"] for offset in range(count)]
            self.assertEqual(stated, [frame for frame in range(done) if frame % 3 == 0])
            done += len(op["keys"]["Frames"])
        self.assertNotIn("parameter", appends[0])

    def test_parameter_channels_append_by_path(self):
        body = _body()
        frames = list(range(2000))
        parameters = [{"Type": "Scalar", "Name": "Glow", "Channels": {"ParameterCurve": {"Frames": frames, "Values": frames}}},
                      {"Type": "Color", "Name": "Tint", "Channels": {"RedCurve": {"Keys": [{"Frame": 0, "Value": 1}]}}}]
        body["Tracks"].append({"Type": "/Script/MovieSceneTracks.MovieSceneMaterialParameterCollectionTrack",
                               "Sections": [{"Start": 0, "End": 2000, "Parameters": parameters}]})
        plan = _plan(body)
        plan.structure()[-1].created("MPC_1")
        add = next(item for item in plan.content() if item.op["op"] == "section.add")
        add.created("MPCSection_1")
        written = add.op["entry"]["Parameters"]
        self.assertEqual([entry["Name"] for entry in written], ["Glow", "Tint"])
        appends = [item for item in plan.finish() if item.op["op"] == "channel.append"]
        self.assertEqual({tuple(item.op["parameter"]) for item in appends}, {(0,), (1,)})
        self.assertEqual(appends[0].path, "/Body/Tracks/1/Sections/0/Parameters/0/Channels/ParameterCurve")
        # The second parameter's channel did not fit the section op: it follows (whole) as an append of its own.
        self.assertEqual(written[1]["Channels"]["RedCurve"], {})
        self.assertEqual(appends[-1].op["keys"]["Frames"], [0])
        appends = [item for item in appends if item.op["parameter"] == [0]]
        joined = written[0]["Channels"]["ParameterCurve"]["Frames"] + [f for item in appends for f in item.op["keys"]["Frames"]]
        self.assertEqual(joined, frames)

    def test_long_parameter_lists_follow_as_parameters_append(self):
        # A DMX-like section: more patch entries (each with function entries and channels) than one op carries. The
        # entries that do not fit follow as parameters.append chunks (nested ones after the op adding their owner),
        # every op stays under the budget, and replaying the ops gives the section back.
        body = _body()
        functions = [{"Properties": {"AttributeName": f"F{index}"}, "Channels": {"Channel": {"Default": 0.0, "Keys": [
            {"Frame": frame, "Value": float(frame % 5), "Interp": "Constant"} for frame in range(index % 4 + 1)]}}}
            for index in range(60)]
        patches = [{"Properties": {"ActiveMode": patch}, "Parameters": copy.deepcopy(functions[:10 + patch * 3])} for patch in range(12)]
        section = {"Start": 0, "End": 100, "Parameters": patches}
        body["Tracks"].append({"Type": "/Script/DMXRuntime.MovieSceneDMXLibraryTrack", "Sections": [section]})
        plan = _plan(body)
        plan.structure()[-1].created("DMX_1")
        add = next(item for item in plan.content() if item.op["op"] == "section.add")
        add.created("DMXSection_1")
        later = [item for item in plan.finish() if item.op["op"] in ("parameters.append", "channel.append")]
        self.assertTrue(any(item.op["op"] == "parameters.append" for item in later))
        for item in [add] + later:
            self.assertLess(seq.common.count_nodes(item.op), seq.common.OP_NODE_BUDGET)
        rebuilt = copy.deepcopy(add.op["entry"])

        def owner(path):
            node = rebuilt
            for index in path:
                node = node["Parameters"][index]
            return node
        appended = next(item for item in later if item.op["op"] == "parameters.append")
        self.assertEqual(appended.path, "/Body/Tracks/1/Sections/0" + "".join(f"/Parameters/{index}" for index in appended.op.get("parameter", []))
                         + "/Parameters")
        for item in later:
            op = item.op
            if op["op"] == "parameters.append":
                node = owner(op.get("parameter", []))
                node.setdefault("Parameters", []).extend(copy.deepcopy(op["entries"]))
            else:
                channel = owner(op["parameter"])["Channels"][op["channel"]]
                keys = list(seq.channel_keys(channel)) + list(seq.channel_keys(op["keys"]))
                for name in list(seq.COLUMNS) + ["Keys"]:
                    channel.pop(name, None)
                channel["Keys"] = keys
        self.assertEqual(seq._drop_defaults(seq.normalize_section(rebuilt)), seq._drop_defaults(seq.normalize_section(section)))

    def test_settings_are_sparse(self):
        body = _body()
        body["Settings"] = {"PlaybackRange": {"Start": 0, "End": 200}, "Evaluation": "WithSubFrames"}
        self.assertEqual(_ops(_plan(body).structure()),
                         [{"op": "settings", "values": {"PlaybackRange": {"Start": 0, "End": 200}}}])

    def test_omitted_settings_are_reset(self):
        live = _live()
        live["Settings"] = dict(live["Settings"], ReadOnly=True, SelectionRange={"Start": 1, "End": 5},
                                Clock="Custom", CustomClock={"Class": "C"})
        body = _body()
        body["Settings"] = {"PlaybackRange": {"Start": 0, "End": 150}}
        self.assertEqual(_ops(_plan(body, live).structure()),
                         [{"op": "settings", "values": {"ReadOnly": False, "SelectionRange": None, "Clock": "Tick",
                                                        "CustomClock": None}}])
        body["Settings"] = {"Clock": "Custom"}               # the custom clock object goes with the clock source
        self.assertEqual(_ops(_plan(body, live).structure()),
                         [{"op": "settings", "values": {"ReadOnly": False, "SelectionRange": None}}])
        live["MetaData"] = {"Notes": "n", "Author": "a"}
        body["MetaData"] = {"Author": "a"}
        self.assertIn({"op": "metadata.set", "values": {"Notes": ""}}, _ops(_plan(body, live).finish()))
        del body["Settings"], body["MetaData"]               # omitted regions keep
        self.assertEqual(_ops(_plan(body, live).structure()), [])
        self.assertNotIn("metadata.set", [op["op"] for op in _ops(_plan(body, live).finish())])

    def test_folders_markers_metadata(self):
        body = _body()
        body["Folders"] = [{"Path": "Cameras"}, {"Path": "Set", "Color": {"R": 1, "G": 2, "B": 3, "A": 255}}]
        body["Bindings"][1]["Folder"] = "Set"
        body["Markers"] = [{"Frame": 90, "Label": "Close"}]
        body["MetaData"] = {"Notes": "n", "Author": "me"}
        ops = _ops(_plan(body).finish())
        self.assertEqual(ops[0], {"op": "folders.set", "folders": body["Folders"],
                                  "bindings": [{"guid": CAMERA, "folder": "Cameras"}, {"guid": DOOR, "folder": "Set"}],
                                  "tracks": []})
        self.assertEqual(ops[1], {"op": "markers.set", "markers": [{"Frame": 90, "Label": "Close"}]})
        self.assertEqual(ops[2], {"op": "metadata.set", "values": {"Author": "me"}})

    def test_track_identity(self):
        live = [{"Type": "Event", "Id": "E1", "_Object": "E1"}, {"Type": "Event", "Id": "E2", "_Object": "E2"},
                {"Type": "Float", "Property": "A", "_Object": "F"}]
        authored = [{"Type": "Event", "Id": "E2"}, {"Type": "Float", "Property": "A"}, {"Type": "Event"}]
        pairs, removed = apply_module.match_tracks(authored, live)
        self.assertEqual([match["_Object"] if match else None for _, match in pairs], ["E2", "F", "E1"])
        self.assertEqual(removed, [])


def _strip(value):
    """A live Body without the plan's private ids."""
    if type(value) is list:
        return [_strip(item) for item in value]
    if type(value) is dict:
        return {key: _strip(item) for key, item in value.items() if not key.startswith("_")}
    return value


def Plan_with(body, live):
    plan = apply_module.Plan(seq.canonicalize(_document(body))["Body"], live)
    plan.match()
    return plan


class ApplyTests(unittest.TestCase):
    def _handler(self):
        return seq.LevelSequenceRuntimeProfile()

    def test_unchanged_apply_writes_nothing(self):
        with mock.patch.object(seq, "read_body", return_value=(_live(), {})), \
                mock.patch.object(seq.common.DataCalls, "query") as query, \
                mock.patch.object(apply_module.common, "invoke") as invoke:
            self.assertEqual(self._handler().apply(_document(_body()), object(), object(), mock.MagicMock()), ())
        invoke.assert_not_called()
        query.assert_not_called()

    def test_edit_runs_batches_and_reports(self):
        body = _body()
        del body["Bindings"][1]
        outcome = mock.MagicMock()
        outcome.changed_set = ("changed",)
        outcome.value = {"changed": True, "created": [], "dependents": [{"package": "/Game/BP_Door", "bindings": ["Door"]}]}
        statuses = iter([{"Status": "UpToDate", "Messages": []},
                         {"Status": "UpToDateWithWarnings", "Messages": [{"Severity": "Warning", "Message": "w", "Path": "/Body"}]}])
        with mock.patch.object(seq, "read_body", return_value=(_live(), {})), \
                mock.patch.object(apply_module, "status", side_effect=lambda calls: next(statuses)), \
                mock.patch.object(apply_module.common, "invoke", return_value=outcome) as invoke:
            result = self._handler().apply(_document(body), object(), object(), mock.MagicMock())
        self.assertEqual(invoke.call_count, 1)
        self.assertEqual(invoke.call_args.args[3]["ops"], [{"op": "binding.remove", "guid": DOOR}])
        self.assertEqual(tuple(result), ("changed",))
        self.assertEqual([item.code for item in result.diagnostics], ["Sequencer.ValidationWarning", "Sequencer.DependentsAffected"])

    def test_new_errors_fail(self):
        failure, _ = apply_module.validation_outcome({"Messages": []}, {"Messages": [{"Severity": "Error", "Message": "x"}]})
        self.assertEqual(failure.diagnostic.code, "Sequencer.ValidationFailed")
        failure, warnings = apply_module.validation_outcome({"Messages": [{"Severity": "Error", "Message": "x"}]},
                                                            {"Messages": [{"Severity": "Error", "Message": "x"}]})
        self.assertIsNone(failure)
        self.assertEqual([item.code for item in warnings], ["Sequencer.ValidationError"])

    def test_op_errors_map_to_document_paths(self):
        ops = [apply_module.Op({"op": "binding.set"}, "/Body/Bindings/0", values={"entry": "/Body/Bindings/0"})]
        self.assertEqual(apply_module.op_path("/ops/0/entry/Bind/1", ops), "/Body/Bindings/0/Bind/1")
        self.assertEqual(apply_module.op_path("/ops/0/guid", ops), "/Body/Bindings/0")

    def test_refusals(self):
        cases = [
            ({"Tracks": [{"Type": "Fade", "Sections": [{"Group": 1}, {}]}]}, "/Body/Tracks/0/Sections/0/Group",
             "Sequencer.InvalidBody"),
            ({"Bindings": [{"Name": "A", "Bind": [], "Folder": "Nope"}]}, "/Body/Bindings/0/Folder", "Sequencer.InvalidFolder"),
            ({"Bindings": [{"Name": "A", "Spawnable": {}}]}, "/Body/Bindings/0/Spawnable", "Sequencer.InvalidBody"),
            ({"Bindings": [{"Name": "A", "Id": "x", "Bind": []}]}, "/Body/Bindings/0/Id", "Sequencer.InvalidBody"),
        ]
        for body, path, code in cases:
            with self.subTest(path=path):
                with self.assertRaises(AssetDocumentContractError) as raised:
                    apply_module.refuse(body, {})
                self.assertEqual((raised.exception.diagnostic.path, raised.exception.diagnostic.code), (path, code))

    def test_create_is_admitted(self):
        self.assertEqual(self._handler().create_target_context(_document({}), object()), ())

    def test_body_owned_properties_are_refused(self):
        with mock.patch.object(seq, "read_body", return_value=(_live(), {})):
            with self.assertRaises(AssetDocumentContractError) as raised:
                self._handler().apply(_document(_body(), {"MovieScene": None}), object(), object(), mock.MagicMock())
        self.assertEqual(raised.exception.diagnostic.code, "Sequencer.NotAuthoredProperty")


class GroupTests(unittest.TestCase):
    """Section groups (section "Group") and node groups (Body.Groups)."""

    def test_camera_cut_track_is_listed_first(self):
        body = {"Tracks": [
            {"Type": "Shot", "Sections": [{"Start": 0, "End": 10, "Properties": {"SubSequence": "/Game/A.A"}}]},
            {"Type": "Fade"},
            {"Type": "CameraCut", "Sections": [{"Start": 0, "End": 10, "Properties": {"CameraBindingID":
                {"Binding": "Cam", "Sequence": "/Body/Tracks/0/Sections/0"}}}]}],
            "Groups": [{"Name": "G", "Members": [{"Track": "/Body/Tracks/1"}, {"Track": "/Body/Tracks/2", "Node": "X"}]}]}
        out = seq.canonicalize(_document(body))["Body"]
        self.assertEqual([track["Type"] for track in out["Tracks"]], ["CameraCut", "Shot", "Fade"])
        self.assertEqual(out["Tracks"][0]["Sections"][0]["Properties"]["CameraBindingID"]["Sequence"],
                         "/Body/Tracks/1/Sections/0")
        self.assertEqual(out["Groups"][0]["Members"], [{"Track": "/Body/Tracks/2"}, {"Track": "/Body/Tracks/0", "Node": "X"}])
        first = {"Tracks": [{"Type": "CameraCut"}, {"Type": "Fade"}]}
        self.assertEqual(seq.canonicalize(_document(first))["Body"]["Tracks"], first["Tracks"])

    def _grouped(self):
        body = _body()
        body["Tracks"][0]["Sections"][0]["Group"] = 1          # camera cut section
        body["Bindings"][1]["Tracks"][0]["Sections"][0]["Group"] = 1  # Door's bool section
        return body

    def test_diff_compares_what_is_grouped(self):
        live = seq.canonicalize(_document(_body()))
        changes = seq.diff(seq.canonicalize(_document(self._grouped())), live, exact=False)
        self.assertEqual(sorted(item.path for item in changes),
                         ["/Body/Bindings/1/Tracks/0/Sections/0/Group", "/Body/Tracks/0/Sections/0/Group"])
        renumbered = self._grouped()
        renumbered["Tracks"][0]["Sections"][0]["Group"] = 7
        renumbered["Bindings"][1]["Tracks"][0]["Sections"][0]["Group"] = 7
        grouped = seq.canonicalize(_document(self._grouped()))
        self.assertEqual(seq.diff(seq.canonicalize(_document(renumbered)), grouped, exact=False), ())
        # An omitted region keeps its live sections (and their numbers) in the comparison.
        only_tracks = {"Tracks": self._grouped()["Tracks"]}
        self.assertEqual(seq.diff(seq.canonicalize(_document(only_tracks)), grouped, exact=False), ())
        only_tracks = {"Tracks": renumbered["Tracks"]}
        self.assertEqual(len(seq.diff(seq.canonicalize(_document(only_tracks)), grouped, exact=False)), 2)

    def test_plan_writes_section_groups_after_content(self):
        plan = _plan(self._grouped())
        plan.structure()
        content = _ops(plan.content())
        self.assertFalse(any("Group" in op.get("entry", {}) for op in content))
        finish = _ops(plan.finish())
        self.assertEqual(finish, [{"op": "section_groups.set", "groups": [[
            {"track": "Object_6", "section": "Object_7"}, {"track": "Object_0", "section": "Object_1"}]]}])
        live = _live()
        live["Tracks"][0]["Sections"][0]["Group"] = 3
        live["Bindings"][1]["Tracks"][0]["Sections"][0]["Group"] = 3
        plan = _plan(self._grouped(), live)
        plan.structure()
        plan.content()
        self.assertEqual(_ops(plan.finish()), [])
        # Ungrouping everything.
        plan = _plan(_body(), live)
        plan.structure()
        plan.content()
        self.assertEqual(_ops(plan.finish()), [{"op": "section_groups.set", "groups": []}])

    def test_node_groups_compare_members_as_a_set(self):
        live = _live()
        live["Groups"] = [{"Name": "Hero", "Members": [{"Binding": "Cam"}, {"Binding": "Door"}]}]
        body = _body()
        body["Groups"] = [{"Name": "Hero", "Members": [{"Binding": "Door"}, {"Binding": "Cam"}]}]
        plan = _plan(body, live)
        plan.structure()
        plan.content()
        self.assertEqual(_ops(plan.finish()), [])
        body["Groups"].append({"Name": "Set", "Members": [{"Folder": "Cameras"}]})
        plan = _plan(body, live)
        plan.structure()
        plan.content()
        self.assertEqual(_ops(plan.finish()), [{"op": "groups.set", "groups": body["Groups"]}])

    def test_remote_references_are_written_last(self):
        body = _body()
        body["Tracks"][0]["Sections"][0]["Properties"] = {"CameraBindingID": {"Binding": "Camera", "Sequence": "/Body/Tracks/1/Sections/0"}}
        body["Tracks"].append({"Type": "CinematicShot", "Sections": [{"Start": 0, "End": 150,
                                                                      "Properties": {"SubSequence": "/Game/Shot.Shot"}}]})
        plan = _plan(body)
        structure = plan.structure()
        structure[-1].created("MovieSceneCinematicShotTrack_0")
        content = _ops(plan.content())
        tracks = [op.get("track") for op in content if op["op"] in ("section.add", "section.set")]
        self.assertEqual(content[0]["op"], "track.order")
        self.assertEqual(tracks, ["MovieSceneCinematicShotTrack_0", "Object_0"])


class DirectorTests(unittest.TestCase):
    """Body.Director (the hosted Director Blueprint) in the plan; the editor side is Sequencer.Director*."""

    LIVE = {"header": {"director": {"Blueprint": "/Game/Cinematics/LS_Test.LS_Test:LS_Test_DirectorBP",
                                    "Class": "/Game/Cinematics/LS_Test.LS_Test_DirectorBP_C"}}}

    def test_class_paths(self):
        self.assertEqual(director.default_class(TARGET), "/Game/Cinematics/LS_Test.LS_Test_DirectorBP_C")
        self.assertEqual(director.class_of("/Game/Cinematics/LS_Test.LS_Test:LS_Test_DirectorBP"),
                         "/Game/Cinematics/LS_Test.LS_Test_DirectorBP_C")

    def test_diff_paths_are_under_director(self):
        owner = director.default_class(TARGET)
        self.assertEqual([(item.path, item.kind.name) for item in director.diff({}, None, owner)], [("/Body/Director", "ADD")])
        self.assertEqual([item.path for item in director.diff(None, {"Variables": []}, owner)], ["/Body/Director"])
        live = {"ImplementedInterfaces": [], "Variables": [], "ClassDefaults": {}, "EventDispatchers": [],
                "UbergraphPages": [], "FunctionGraphs": [], "MacroGraphs": []}
        authored = director.canonical({"Variables": [{"Name": "Speed", "Type": "float"}]}, owner)
        self.assertEqual([item.path for item in director.diff(authored, live, owner)], ["/Body/Director/Variables"])
        self.assertEqual(director.diff(director.canonical({}, owner), live, owner), [])

    def _apply(self, body, facts, created=()):
        outcome = mock.MagicMock()
        outcome.changed_set = ("changed",)
        outcome.value = {"changed": True, "created": list(created), "dependents": []}
        with mock.patch.object(seq, "read_body", return_value=(_live(), facts)), \
                mock.patch.object(apply_module, "status", return_value={"Messages": []}), \
                mock.patch.object(apply_module.director, "apply", return_value=(["director"], [])) as written, \
                mock.patch.object(apply_module.common, "invoke", return_value=outcome) as invoke:
            seq.LevelSequenceRuntimeProfile().apply(_document(body), object(), object(), mock.MagicMock())
        return [call.args[3]["ops"] for call in invoke.call_args_list], written

    def test_a_director_is_created_first(self):
        body = _body()
        body["Director"] = {"Variables": []}
        batches, written = self._apply(body, {"header": {"director": None}},
                                       [{"op": 0, "object": "/Game/Cinematics/LS_Test.LS_Test:LS_Test_DirectorBP"}])
        self.assertEqual(batches, [[{"op": "director.create"}]])
        self.assertEqual(written.call_args.args[5], "/Game/Cinematics/LS_Test.LS_Test_DirectorBP_C")
        self.assertEqual(written.call_args.args[6], {"Variables": []})

    def test_an_existing_director_is_edited_in_place(self):
        body = _body()
        body["Director"] = {}
        batches, written = self._apply(body, self.LIVE)
        self.assertEqual(batches, [])
        self.assertEqual(written.call_args.args[5], "/Game/Cinematics/LS_Test.LS_Test_DirectorBP_C")

    def test_director_null_removes_it_last(self):
        body = _body()
        del body["Bindings"][1]
        body["Director"] = None
        batches, written = self._apply(body, self.LIVE)
        written.assert_not_called()
        self.assertEqual(batches[-1], [{"op": "director.remove"}])
        self.assertEqual(batches[0][0], {"op": "binding.remove", "guid": DOOR})


if __name__ == "__main__":
    unittest.main()
