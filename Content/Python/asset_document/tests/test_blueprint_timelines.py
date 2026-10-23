# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.graphs.schemas import k2
from asset_document.profiles import blueprint_timelines as timelines
from asset_document.profiles.ublueprint_runtime import canonicalize

TARGET = "/Game/BP_Door"
SELF = "/Game/BP_Door.BP_Door_C"


def code_of(call):
    try:
        call()
    except AssetDocumentContractError as error:
        return error.diagnostic.code, error.diagnostic.path
    return None


def document(body):
    return {"SchemaVersion": 1, "Target": TARGET, "Class": "/Script/Engine.Blueprint", "Action": "Update",
            "Definitions": {}, "Properties": {}, "Body": body}


def live_key(time, value=0.0, interp="RCIM_Linear", tangent="RCTM_Auto", arrive=0.0, leave=0.0):
    return {"time": time, "value": value, "interp_mode": interp, "tangent_mode": tangent,
            "tangent_weight_mode": "RCTWM_WeightedNone", "arrive_tangent": arrive, "arrive_tangent_weight": 0.0,
            "leave_tangent": leave, "leave_tangent_weight": 0.0}


LIVE = {"name": "Fade", "graph": "EventGraph", "length": 2.0, "use_last_keyframe": False, "auto_play": True,
        "loop": False, "replicated": False, "ignore_time_dilation": False, "tick_group": "TG_PrePhysics",
        "tracks": [
            {"name": "Alpha", "type": "float", "curve": "",
             "channels": [[live_key(0.0), live_key(1.0, 0.5, "RCIM_Cubic", arrive=0.37, leave=0.37),
                           live_key(2.0, 1.0)]]},
            {"name": "Offset", "type": "vector", "curve": "",
             "channels": [[], [], [live_key(0.0), live_key(2.0, 50.0)]]},
            {"name": "Tint", "type": "linear_color", "curve": "/Game/Curves/C_Tint.C_Tint"},
            {"name": "Halfway", "type": "event", "curve": "", "channels": [[live_key(1.0)]]}]}
ROW = {"Name": "Fade", "Length": 2.0, "AutoPlay": True, "Tracks": [
    {"Name": "Alpha", "Type": "Float", "Keys": [{"Time": 0.0}, {"Time": 1.0, "Value": 0.5, "InterpMode": "Cubic"},
                                                 {"Time": 2.0, "Value": 1.0}]},
    {"Name": "Offset", "Type": "Vector", "Keys": {"Z": [{"Time": 0.0}, {"Time": 2.0, "Value": 50.0}]}},
    {"Name": "Tint", "Type": "LinearColor", "Curve": "/Game/Curves/C_Tint.C_Tint"},
    {"Name": "Halfway", "Type": "Event", "Keys": [{"Time": 1.0}]}]}


class CanonicalTests(unittest.TestCase):
    def test_defaults_are_omitted_and_spellings_normalised(self):
        rows = timelines.canonical({"Timelines": [
            {"Name": "T", "Length": 5, "Loop": False, "TickGroup": "TG_PostPhysics",
             "Tracks": [{"Name": "A", "Type": "Float", "Keys": [{"Time": 0, "Value": 0.1, "InterpMode": "RCIM_Constant",
                                                                 "TangentMode": "Auto"}]},
                        {"Name": "C", "Type": "Float", "Curve": "/Game/C_Fade"},
                        {"Name": "V", "Type": "Vector", "Keys": {"X": []}}]}]})
        self.assertEqual(rows, [{"Name": "T", "TickGroup": "PostPhysics", "Tracks": [
            {"Name": "A", "Type": "Float", "Keys": [{"Time": 0.0, "Value": 0.1, "InterpMode": "Constant"}]},
            {"Name": "C", "Type": "Float", "Curve": "/Game/C_Fade.C_Fade"},
            {"Name": "V", "Type": "Vector"}]}])
        self.assertEqual(timelines.canonical({"Timelines": rows}), rows)
        self.assertIsNone(timelines.canonical({}))

    def test_tangents_follow_the_curve_editor(self):
        keys = timelines.canonical({"Timelines": [{"Name": "T", "Tracks": [{"Name": "A", "Type": "Float", "Keys": [
            {"Time": 0, "LeaveTangent": 3, "ArriveTangent": 1},                    # first Auto key: leave -> 0
            {"Time": 1, "InterpMode": "Cubic", "ArriveTangent": 9, "LeaveTangent": 9},  # auto cubic: derived
            {"Time": 2, "InterpMode": "Constant", "TangentMode": "User", "ArriveTangent": 2, "LeaveTangent": 2},
            {"Time": 3, "InterpMode": "Cubic", "TangentMode": "User", "ArriveTangent": 4, "LeaveTangent": 4},
            {"Time": 4, "InterpMode": "Cubic", "TangentMode": "Break", "ArriveTangent": 5, "LeaveTangent": 6,
             "TangentWeightMode": "Both", "ArriveTangentWeight": 0.5}]}]}]})[0]["Tracks"][0]["Keys"]
        self.assertEqual(keys, [
            {"Time": 0.0, "ArriveTangent": 1.0},
            {"Time": 1.0, "InterpMode": "Cubic"},
            # Constant key after a Cubic key: arrive kept, leave cleared.
            {"Time": 2.0, "InterpMode": "Constant", "TangentMode": "User", "ArriveTangent": 2.0},
            # After a Constant key: both cleared.
            {"Time": 3.0, "InterpMode": "Cubic", "TangentMode": "User"},
            # Last key: untouched.
            {"Time": 4.0, "InterpMode": "Cubic", "TangentMode": "Break", "TangentWeightMode": "Both",
             "ArriveTangent": 5.0, "ArriveTangentWeight": 0.5, "LeaveTangent": 6.0}])

    def test_numbers_are_float32(self):
        rows = timelines.canonical({"Timelines": [{"Name": "T", "Length": 0.1, "Tracks": [
            {"Name": "A", "Type": "Float", "Keys": [{"Time": 0.1, "Value": 1 / 3}]}]}]})
        self.assertEqual(rows[0]["Length"], 0.1)
        self.assertEqual(rows[0]["Tracks"][0]["Keys"][0]["Value"], 0.33333334)

    def test_invalid_rows_fail_with_stable_codes(self):
        track = lambda **fields: {"Timelines": [{"Name": "T", "Tracks": [{"Name": "A", "Type": "Float", **fields}]}]}
        for body, code in (
                ({"Timelines": [{"Name": "T"}, {"Name": "t"}]}, "UBlueprint.DuplicateName"),
                ({"Timelines": [{"Name": "T", "Length": 0}]}, "UBlueprint.InvalidValue"),
                ({"Timelines": [{"Name": "T", "TickGroup": "TG_StartPhysics"}]}, "UBlueprint.InvalidValue"),
                ({"Timelines": [{"Name": "T", "Speed": 1}]}, "UBlueprint.UnknownField"),
                (track(Keys=[{"Time": 1}, {"Time": 1}]), "UBlueprint.InvalidValue"),
                (track(Keys=[{"Value": 1}]), "UBlueprint.InvalidBodyRegion"),
                (track(Keys=[{"Time": 0}], Curve="/Game/C"), "BlueprintMember.InvalidTrack"),
                (track(Keys=[{"Time": 0, "InterpMode": "Bezier"}]), "UBlueprint.InvalidValue"),
                ({"Timelines": [{"Name": "T", "Tracks": [{"Name": "V", "Type": "Vector", "Keys": {"W": []}}]}]},
                 "UBlueprint.UnknownField"),
                ({"Timelines": [{"Name": "T", "Tracks": [{"Name": "A", "Type": "Float"},
                                                         {"Name": "a", "Type": "Event"}]}]},
                 "UBlueprint.DuplicateName")):
            with self.subTest(body=body):
                self.assertEqual(code_of(lambda: timelines.canonical(body))[0], code)


class LiveTests(unittest.TestCase):
    def test_extract_shape_round_trips_and_plans_nothing(self):
        rows = timelines.from_query({"timelines": [LIVE]})
        self.assertEqual(rows, [ROW])
        body = {"Timelines": timelines.canonical({"Timelines": rows})}
        self.assertEqual(body["Timelines"], rows)
        query = {"timelines": [LIVE]}
        self.assertEqual(timelines.plan_removes(body, query) + timelines.plan_sets(body, query), [])
        self.assertEqual(timelines.diff(body, {"Timelines": rows}), [])

    def test_changes_send_only_what_differs(self):
        row = dict(ROW, Loop=True)
        body = {"Timelines": timelines.canonical({"Timelines": [row, {"Name": "Open"}]})}
        query = {"timelines": [LIVE, dict(LIVE, name="Old")]}
        self.assertEqual(timelines.plan_removes(body, query),
                         [("blueprint.member.timeline.remove.v1", {"name": "Old"}, "/Body/Timelines")])
        sets = timelines.plan_sets(body, query)
        self.assertEqual(sets[0], ("blueprint.member.timeline.set.v1", {"name": "Fade", "loop": True},
                                   "/Body/Timelines/0"))
        self.assertEqual(sets[1], ("blueprint.member.timeline.set.v1", {"name": "Open", "tracks": []},
                                   "/Body/Timelines/1"))
        self.assertEqual([item.path for item in timelines.diff(body, {"Timelines": timelines.from_query(query)})],
                         ["/Body/Timelines"])

    def test_track_changes_send_the_whole_track_list(self):
        row = dict(ROW, Tracks=ROW["Tracks"][:1])
        body = {"Timelines": timelines.canonical({"Timelines": [row]})}
        values = timelines.plan_sets(body, {"timelines": [LIVE]})[0][1]
        self.assertEqual(list(values), ["name", "tracks"])
        self.assertEqual(values["tracks"][0]["name"], "Alpha")
        self.assertEqual(values["tracks"][0]["channels"][0][1],
                         {"time": 1.0, "value": 0.5, "interp_mode": "RCIM_Cubic", "tangent_mode": "RCTM_Auto",
                          "tangent_weight_mode": "RCTWM_WeightedNone", "arrive_tangent": 0.0,
                          "arrive_tangent_weight": 0.0, "leave_tangent": 0.0, "leave_tangent_weight": 0.0})


class ProfileTests(unittest.TestCase):
    GRAPH = {"Name": "EventGraph", "Nodes": [{"Id": "TL", "Action": "K2:Timeline", "Name": "Fade",
                                              "Position": {"X": 640, "Y": 96}}], "Links": []}

    def test_new_timeline_nodes_are_placed_like_the_document_says(self):
        graphs, _ = k2.decode_body({"UbergraphPages": [self.GRAPH]}, self_class=SELF)
        body = {"Timelines": [{"Name": "Fade"}]}
        values = timelines.plan_sets(body, {"timelines": []}, timelines.timeline_nodes(graphs))[0][1]
        self.assertEqual((values["graph"], values["position"]), ("EventGraph", {"x": 640.0, "y": 96.0}))

    def test_timelines_and_timeline_nodes_must_agree(self):
        canonicalize(document({"Timelines": [{"Name": "Fade"}], "UbergraphPages": [self.GRAPH]}))
        canonicalize(document({"Timelines": [{"Name": "Other"}]}))
        self.assertEqual(code_of(lambda: canonicalize(document({"Timelines": [{"Name": "Other"}],
                                                                "UbergraphPages": [self.GRAPH]})))[0],
                         "UBlueprint.TimelineNodeMismatch")
        self.assertEqual(code_of(lambda: canonicalize(document({"Timelines": [], "UbergraphPages": [self.GRAPH]}))),
                         ("UBlueprint.TimelineNodeMismatch", "/Body/UbergraphPages/0/Nodes/0"))

    def test_timeline_nodes_need_a_name(self):
        body = {"UbergraphPages": [{"Name": "EventGraph", "Nodes": [{"Id": "TL", "Action": "K2:Timeline"}]}]}
        self.assertEqual(code_of(lambda: k2.decode_body(body, self_class=SELF))[0], "Graph.MissingName")

    def test_dispatchers_and_timelines_are_profile_regions(self):
        canonical = canonicalize(document({"EventDispatchers": [{"Name": "OnOpened"}], "Timelines": []}))
        self.assertEqual(canonical["Body"], {"EventDispatchers": [{"Name": "OnOpened"}], "Timelines": []})
        self.assertEqual(code_of(lambda: canonicalize(document({"Variables": [{"Name": "OnOpened", "Type": "int"}],
                                                                "EventDispatchers": [{"Name": "OnOpened"}]})))[0],
                         "UBlueprint.DuplicateName")


if __name__ == "__main__":
    unittest.main()
