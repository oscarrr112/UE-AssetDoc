# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Focused codec evidence: full exact state, sparse intent and correspondence."""
from copy import deepcopy
import json
import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.profiles.anim_montage_codec import (
    lower_body, normalize_body, snapshot_to_body,
)


def curve():
    return {
        "Name": "WeightedCurve", "Flags": 7,
        "Color": {"R": {"Bits": 0x3f800000}, "G": {"Bits": 0},
                  "B": {"Bits": 0x80000000}, "A": {"Bits": 0x3f800000}},
        "Comment": "Exact payload", "DefaultValue": {"HasValue": True, "Float32Bits": 0x7fc01234},
        "PreExtrapolation": 2, "PostExtrapolation": 5,
        "Keys": [{"Time": {"Bits": 0}, "Value": {"Bits": 0x7fc00001},
                  "ArriveTangent": {"Bits": 0x80000000}, "LeaveTangent": {"Bits": 0x7f800000},
                  "InterpMode": 2, "TangentMode": 2, "TangentWeightMode": 3,
                  "ArriveTangentWeight": {"Bits": 0x3f000000},
                  "LeaveTangentWeight": {"Bits": 0x3f800000}}],
    }


def segment(path="/Game/A.A", start=0):
    return {"AnimReference": path, "StartPos": start, "AnimStartTime": 0,
            "AnimEndTime": 1, "AnimPlayRate": 1, "LoopingCount": 1}


def body():
    return {
        "Skeleton": "/Game/Skeleton.Skeleton", "PreviewMesh": None,
        "Curves": [curve()],
        "SlotAnimTracks": [{"SlotName": "DefaultSlot", "Segments": [segment()]}],
        "CompositeSections": [{"Name": "Start", "NextSectionName": None,
                               "LinkableTime": 0, "SlotName": "DefaultSlot", "LinkMethod": 0}],
        "Notifies": [{"Time": 0.25, "TrackIndex": 0,
                      "Object": {"Class": "/Script/Engine.AnimNotify", "Properties": {"NotifyColor": [1, 2, 3]}}}],
        "NotifyStates": [{"Time": 0.5, "Duration": 0.25, "TrackIndex": 0,
                          "Object": {"Class": "/Script/Engine.AnimNotifyState", "Properties": {}}}],
        "NotifyTracks": [{"Name": "1"}],
    }


def snapshot(canonical=None):
    result = lower_body(body() if canonical is None else canonical)
    for slot in result["slots"]:
        for i, row in enumerate(slot["segments"]):
            row["previous_slot"], row["previous_segment"] = slot["name"], i
    return result


class MontageCodecTests(unittest.TestCase):
    def test_exact_curve_roundtrip_preserves_nan_weights_unset_signed_zero(self):
        authored = body()
        authored["Curves"].append({**curve(), "Name": "Unset", "DefaultValue": {"HasValue": False, "Float32Bits": None}})
        self.assertEqual(snapshot_to_body(snapshot(authored)), authored)
        json.dumps(lower_body(authored), allow_nan=False)

    def test_omission_and_explicit_empty_are_distinct(self):
        self.assertEqual(lower_body({}), {})
        self.assertEqual(lower_body({"Notifies": []}), {"notifies": []})
        self.assertEqual(lower_body({"Skeleton": None}), {"skeleton": None})

    def test_fixed_native_key_order_independent_of_author_object_order(self):
        authored = body()
        authored["Notifies"][0] = dict(reversed(list(authored["Notifies"][0].items())))
        native = lower_body(dict(reversed(list(authored.items()))))
        self.assertEqual(list(native), ["skeleton", "preview_mesh", "curves", "slots", "sections", "notifies", "notify_states",
                                        "notify_tracks"])
        self.assertEqual(list(native["notifies"][0]), ["time", "track_index", "object"])
        self.assertEqual(list(native["slots"][0]["segments"][0]), ["animation", "start", "source_start", "source_end", "rate", "loops", "previous_slot", "previous_segment"])

    def test_actual_correspondence_is_consumed_and_not_persisted(self):
        baseline = snapshot()
        authored = snapshot_to_body(baseline)
        lowered = lower_body(authored, baseline)
        self.assertEqual(lowered["slots"][0]["segments"][0]["previous_slot"], "DefaultSlot")
        self.assertEqual(lowered["slots"][0]["segments"][0]["previous_segment"], 0)
        self.assertNotIn("previous_segment", authored["SlotAnimTracks"][0]["Segments"][0])
        self.assertEqual(baseline, snapshot())

    def test_reorder_unique_slots_retains_source_correspondence(self):
        authored = body()
        authored["SlotAnimTracks"].append({"SlotName": "Other", "Segments": [segment("/Game/B.B")]})
        baseline = snapshot(authored)
        authored["SlotAnimTracks"].reverse()
        rows = lower_body(authored, baseline)["slots"]
        self.assertEqual(rows[0]["segments"][0]["previous_slot"], "Other")
        self.assertEqual(rows[1]["segments"][0]["previous_slot"], "DefaultSlot")

    def test_modified_segment_does_not_infer_identity_by_array_position(self):
        authored = body()
        authored["SlotAnimTracks"][0]["Segments"][0]["AnimPlayRate"] = -1
        actual = lower_body(authored, snapshot())["slots"][0]["segments"][0]
        self.assertEqual((actual["previous_slot"], actual["previous_segment"]), ("None", -1))

    def test_duplicate_matching_baseline_rejects_ambiguous_correspondence(self):
        authored = body()
        authored["SlotAnimTracks"].append({"SlotName": "Other", "Segments": [segment()]})
        with self.assertRaisesRegex(AssetDocumentContractError, "AmbiguousSegmentCorrespondence"):
            lower_body(body(), snapshot(authored))

    def test_snapshot_requires_full_shape_and_actual_correspondence(self):
        for value in ({}, {**snapshot(), "unexpected": []}):
            with self.assertRaises(AssetDocumentContractError):
                snapshot_to_body(value)
        value = snapshot()
        value["slots"][0]["segments"][0]["previous_slot"] = "Elsewhere"
        with self.assertRaises(AssetDocumentContractError):
            snapshot_to_body(value)

    def test_legacy_incomplete_rows_never_receive_guessed_defaults(self):
        for value in ({"Curves": [{"Name": "Speed", "Keys": []}]},
                      {"CompositeSections": [{"Name": "Start", "NextSectionName": None}]},
                      {"Notifies": [{"Name": "Swing", "Time": 0}]},
                      {"NotifyStates": [{"Time": 0, "TrackIndex": 0, "Object": {"Class": "/Script/Engine.AnimNotifyState", "Properties": {}}}]}):
            with self.assertRaises(AssetDocumentContractError):
                lower_body(value)

    def test_invalid_curve_bits_default_and_time_are_rejected(self):
        cases = []
        for bad in (True, -1, 0x100000000):
            c = curve(); c["Color"]["R"]["Bits"] = bad; cases.append(c)
        c = curve(); c["DefaultValue"]["Float32Bits"] = 0x7f7fffff; cases.append(c)
        c = curve(); c["Keys"][0]["Time"]["Bits"] = 0x7fc00000; cases.append(c)
        c = curve(); c["Keys"].append(deepcopy(c["Keys"][0])); cases.append(c)
        for c in cases:
            with self.subTest(curve=c), self.assertRaises(AssetDocumentContractError):
                lower_body({"Curves": [c]})

    def test_native_numeric_normalization_and_reference_shorthand(self):
        authored = {"Skeleton": "/Game/S", "Notifies": [{"Time": 0.1, "TrackIndex": 0,
                   "Object": {"Class": "/Game/BP_Notify.BP_Notify_C", "Properties": {}}}]}
        actual = normalize_body(authored)
        self.assertEqual(actual["Skeleton"], "/Game/S.S")
        self.assertEqual(actual["Notifies"][0]["Time"], 0.10000000149011612)
        self.assertEqual(authored["Notifies"][0]["Time"], 0.1)

    def test_snapshot_projects_stored_float32_as_shortest_decimal(self):
        authored = body()
        authored["Notifies"][0]["Time"] = 0.1
        authored["SlotAnimTracks"][0]["Segments"][0]["AnimPlayRate"] = 0.3
        projected = snapshot_to_body(snapshot(authored))
        self.assertEqual(projected["Notifies"][0]["Time"], 0.1)
        self.assertEqual(projected["SlotAnimTracks"][0]["Segments"][0]["AnimPlayRate"], 0.3)
        self.assertEqual(lower_body(projected), lower_body(authored))

    def test_notify_state_duration_reads_back_as_authored_through_its_end_time(self):
        # Time 0.3 + Duration 0.35 is stored as an end time; end - start reads back 0.34999996.
        import struct
        f32 = lambda value: struct.unpack("!f", struct.pack("!f", value))[0]
        for time, duration in ((0.3, 0.35), (0.1, 0.2), (1.7, 0.05), (0.25, 0.375)):
            with self.subTest(time=time, duration=duration):
                authored = body()
                authored["SlotAnimTracks"][0]["Segments"][0]["AnimEndTime"] = 3
                authored["NotifyStates"][0].update({"Time": time, "Duration": duration})
                observed = snapshot(authored)
                end = f32(f32(time) + f32(duration))
                observed["notify_states"][0]["duration"] = f32(end - f32(time))
                self.assertEqual(snapshot_to_body(observed)["NotifyStates"][0]["Duration"], duration)

    def test_bool_nonfinite_and_implicit_class_are_rejected(self):
        for time in (True, float("nan"), float("inf"), 1e100, -1):
            authored = body(); authored["Notifies"][0]["Time"] = time
            with self.assertRaises(AssetDocumentContractError):
                lower_body(authored)
        authored = body(); authored["Notifies"][0]["Object"]["Class"] = "/Game/BP_Notify"
        with self.assertRaises(AssetDocumentContractError):
            lower_body(authored)

    def test_unadmitted_keys_and_unknown_row_fields_are_rejected(self):
        for authored in ({"Sync": {}}, {"Skeleton": []}, {"Curves": None}):
            with self.assertRaises(AssetDocumentContractError):
                lower_body(authored)
        authored = body(); authored["SlotAnimTracks"][0]["Segments"][0]["PreviousSegment"] = 0
        with self.assertRaises(AssetDocumentContractError):
            lower_body(authored)


if __name__ == "__main__":
    unittest.main()
