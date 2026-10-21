"""profile.anim_sequence.v1: document validation, lowering, extract projection, diff and orchestration."""
from __future__ import annotations

import copy
import unittest

from asset_document.diagnostics import AssetDocumentContractError, RuntimeDiagnostic
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profiles import anim_sequence as profile_module
from asset_document.profiles import anim_sequence_codec as codec
from asset_document.profiles.anim_sequence import (
    ANIM_SEQUENCE_RUNTIME_REGISTRATION, AnimSequenceRuntimeProfile, document_path,
)
from asset_document.regions.primitive_catalog import (
    NamedOpaqueHandleResult, PrimitiveOutcome, PrimitiveOutcomeStatus,
)

TARGET = "/Game/Anims/Run"
CLASS = "/Script/Engine.AnimSequence"
SKELETON = "/Game/Anims/SK_Hero.SK_Hero"


def assert_error(test, code, path, callback):
    with test.assertRaises(AssetDocumentContractError) as caught:
        callback()
    diagnostic = caught.exception.diagnostic
    test.assertEqual((diagnostic.code, diagnostic.path), (code, path))


def document(body=None, properties=None, action="CreateOrUpdate"):
    return {"SchemaVersion": 1, "Target": TARGET, "Class": CLASS, "Action": action, "Definitions": {},
            "Properties": properties or {}, "Body": body or {}}


def native_key(time, value, interp="RCIM_Linear"):
    return {"Time": time, "Value": value, "InterpMode": interp, "TangentMode": "RCTM_Auto",
            "TangentWeightMode": "RCTWM_WeightedNone", "ArriveTangent": 0.0, "ArriveTangentWeight": 0.0,
            "LeaveTangent": 0.0, "LeaveTangentWeight": 0.0}


def trigger(**overrides):
    return {"TriggerWeightThreshold": 0.00001, "NotifyTriggerChance": 1.0, "NotifyFilterType": "NoFiltering",
            "NotifyFilterLOD": 0, "bCanBeFilteredViaRequest": True, "bTriggerOnDedicatedServer": True,
            "bTriggerOnFollower": False, "MontageTickType": "Queued", **overrides}


def snapshot():
    """A native authoring snapshot as animation.asset.authoring.snapshot.v1 returns it."""
    return {
        "skeleton": SKELETON, "preview_mesh": None,
        "properties": {"RetargetSource": "None", "RetargetSourceAsset": "None", "RateScale": 1.0, "bLoop": False,
                       "Interpolation": "Linear", "AdditiveAnimType": "AAT_None", "RefPoseType": "ABPT_None",
                       "RefFrameIndex": 0, "bEnableRootMotion": False, "RootMotionRootLock": "RefPose",
                       "bForceRootLock": False, "bUseNormalizedRootMotionScale": True,
                       "CompressionErrorThresholdScale": 1.0, "bDoNotOverrideCompression": False,
                       "bAllowFrameStripping": True, "StripAnimDataOnDedicatedServer": "UseProjectSetting"},
        "object_properties": {"PreviewPoseAsset": None, "RefPoseSeq": None,
                              "BoneCompressionSettings": "/ACLPlugin/Bone.Bone", "CurveCompressionSettings": None,
                              "VariableFrameStrippingSettings": None},
        "frame_rate": {"numerator": 30, "denominator": 1}, "number_of_frames": 30, "play_length": 1.0,
        "bone_tracks": ["root"],
        "curves": [{"name": "Speed", "keys": [native_key(0.0, 1.0), native_key(1.0, 2.0)], "flags": 4,
                    "color": {"R": 0.5, "G": 0.25, "B": 1.0, "A": 1.0},
                    "default_color": {"R": 0.5, "G": 0.25, "B": 1.0, "A": 1.0}, "comment": ""}],
        "notify_tracks": ["Sound", "FX"],
        "notifies": [{"time": 0.25, "track": "Sound", "class": "/Script/Engine.AnimNotify_PlaySound", "name": "PlaySound",
                      "properties": {"VolumeMultiplier": 0.5, "Sound": None}, "trigger": trigger(NotifyTriggerChance=0.5)},
                     {"time": 0.5, "track": "Sound", "class": None, "name": "Footstep", "properties": {},
                      "trigger": trigger()}],
        "notify_states": [{"time": 0.5, "duration": 0.25, "track": "FX",
                           "class": "/Script/Engine.AnimNotifyState_TimedParticleEffect", "name": "TimedParticleEffect",
                           "properties": {"bDestroyAtEnd": True}, "trigger": trigger()}],
        "sync_markers": [{"name": "Left", "time": 0.5, "track": "Sound"}],
        "metadata": [], "asset_user_data": [],
    }


class Handle:
    def __init__(self, name):
        self.name = name


class FakeContext:
    """Records invocations; answers the authoring snapshot, the resolver and the Editor batch."""

    def __init__(self, exists=True, fail=None, writable=None):
        self.exists, self.fail = exists, fail
        self.calls, self.handoffs = [], []
        self.native = snapshot()
        self.writable = writable or ("RetargetSource", "RetargetSourceAsset", "RateScale", "bLoop", "Interpolation",
                                     "AdditiveAnimType", "RefPoseType", "RefFrameIndex", "bEnableRootMotion",
                                     "RootMotionRootLock", "bForceRootLock", "bUseNormalizedRootMotionScale",
                                     "CompressionErrorThresholdScale", "bDoNotOverrideCompression",
                                     "bAllowFrameStripping", "StripAnimDataOnDedicatedServer")

    def handoff(self, *, source_handle, target_provider_id, access):
        assert target_provider_id == "animation"
        self.handoffs.append((source_handle.name, access))
        return Handle(f"animation:{access}")

    def invoke(self, *, requirement, value_arguments, handle_arguments):
        name = requirement.primitive_id
        self.calls.append((name, copy.deepcopy(value_arguments), tuple(item.name for item in handle_arguments)))
        if self.fail and self.fail[0] == name:
            return PrimitiveOutcome(PrimitiveOutcomeStatus.FAILED, None, (), (),
                                    (RuntimeDiagnostic(self.fail[1], "Test.Rejected", "rejected"),))
        if name == profile_module.SNAPSHOT:
            return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED, self.native, (), (), ())
        if name == profile_module.RESOLVE:
            handles = (NamedOpaqueHandleResult("asset_read", (Handle("read"),) if self.exists else ()),
                       NamedOpaqueHandleResult("asset_mutable", (Handle("mutable"),) if self.exists else ()))
            return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED, {"exists": self.exists}, handles, (), ())
        if name == profile_module.EDITOR_ENUMERATE:
            rows = [{"name": field, "writable": True, "codec_id": "number.v1" if field in ("RateScale",) else "x.v1"}
                    for field in self.writable]
            return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED, {"properties": rows},
                                    (NamedOpaqueHandleResult("property_batch", (Handle("batch"),)),), (), ())
        return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED, {}, (), (), ())

    def names(self):
        return [call[0] for call in self.calls]


class AnimSequenceProfileShapeTests(unittest.TestCase):
    def setUp(self):
        self.profile = AnimSequenceRuntimeProfile()

    def test_registration_and_requirements(self):
        ids = tuple(item.primitive_id for item in self.profile.required_primitives())
        self.assertEqual(ids, (
            "editor.class.resolve_canonical.v1", "editor.asset.resolve_existing.v1",
            "editor.property.enumerate_admitted_batch.v1", "editor.property.write_admitted_batch.v1",
            "animation.asset.authoring.snapshot.v1", "animation.asset.authoring.edit.v1",
            "editor.scratch.clone_asset.v2", "editor.scratch.create_by_factory.v1",
            "editor.asset.create_by_factory.v2", "editor.asset.rollback_created.v1"))
        self.assertTrue(all(item.required_by_route_id == "animation_profile" for item in self.profile.required_primitives()))
        lifecycle = ANIM_SEQUENCE_RUNTIME_REGISTRATION.target_lifecycle
        self.assertEqual((lifecycle.lifecycle_id, lifecycle.create_context_handle_names),
                         ("factory_generic.v1", ("factory_context",)))
        self.assertEqual(ANIM_SEQUENCE_RUNTIME_REGISTRATION.profile_meta.exact_asset_class_path, CLASS)
        self.assertIs(self.profile.preflight_scratch, False)

    def test_template_validates_and_lists_every_region(self):
        template = self.profile.create_template(TARGET)
        self.profile.validate(template)
        self.assertEqual(set(template["Body"]), set(codec.BODY_KEYS) - {"BoneTracks"})
        self.assertEqual(self.profile.inspect_profile()["BodySections"], list(codec.BODY_KEYS))

    def test_validation_errors(self):
        cases = (
            (document({"Sections": []}), "AnimSequence.UnknownBodyKey", "/Body/Sections"),
            (document({"Playback": {"Speed": 1}}), "AnimSequence.UnknownField", "/Body/Playback/Speed"),
            (document({"Playback": {"RateScale": "fast"}}), "AnimSequence.InvalidDocument", "/Body/Playback/RateScale"),
            (document({"Playback": {"NumberOfFrames": 0}}), "AnimSequence.InvalidDocument", "/Body/Playback/NumberOfFrames"),
            (document({"Playback": {"FrameRate": "30/0"}}), "AnimSequence.InvalidDocument", "/Body/Playback/FrameRate"),
            (document({"References": {"Skeleton": None}}), "AnimSequence.InvalidDocument", "/Body/References/Skeleton"),
            (document({"Preview": {"PreviewMesh": "Mesh"}}), "AnimSequence.InvalidReference", "/Body/Preview/PreviewMesh"),
            (document({"NotifyTracks": []}), "AnimSequence.NotifyTrackRequired", "/Body/NotifyTracks"),
            (document({"NotifyTracks": ["A", "A"]}), "AnimSequence.DuplicateNotifyTrack", "/Body/NotifyTracks/1"),
            (document({"NotifyTracks": ["A"], "Notifies": [{"Time": 0, "Track": "B", "Name": "X"}]}),
             "AnimSequence.UnknownNotifyTrack", "/Body/Notifies/0/Track"),
            (document({"Notifies": [{"Time": 0.1, "Name": "X"}, {"Time": 0.1, "Name": "Y"}]}),
             "AnimSequence.OverlappingNotifies", "/Body/Notifies/1/Time"),
            (document({"Notifies": [{"Time": -1, "Name": "X"}]}), "AnimSequence.TimeOutOfRange", "/Body/Notifies/0/Time"),
            (document({"NotifyStates": [{"Time": 0, "Duration": 1}]}), "AnimSequence.NotifyClassRequired",
             "/Body/NotifyStates/0/Class"),
            (document({"NotifyStates": [{"Time": 0, "Duration": 0, "Class": "/Script/Engine.AnimNotifyState"}]}),
             "AnimSequence.InvalidDocument", "/Body/NotifyStates/0/Duration"),
            (document({"Curves": [{"Name": "A"}, {"Name": "A"}]}), "AnimSequence.DuplicateCurve", "/Body/Curves/1/Name"),
            (document({"Curves": [{"Name": "A", "Flags": ["Bogus"]}]}), "AnimSequence.UnknownCurveFlag",
             "/Body/Curves/0/Flags/0"),
            (document({"Curves": [{"Name": "A", "Keys": [{"Time": 1, "Value": 0}, {"Time": 0, "Value": 0}]}]}),
             "AnimSequence.KeysNotAscending", "/Body/Curves/0/Keys/1/Time"),
            (document({"AssetUserData": [{"Class": "/Script/A.B"}, {"Class": "/Script/A.B"}]}),
             "AnimSequence.DuplicateUserDataClass", "/Body/AssetUserData/1/Class"),
            (document({"Additive": {"AdditiveAnimType": "AAT_None", "RefPoseSeq": "/Game/A"}}),
             "AnimSequence.RefPoseSeqHidden", "/Body/Additive/RefPoseSeq"),
            (document(properties={"RateScale": 2.0}), "AnimSequence.ManagedPropertyOverlap", "/Properties/RateScale"),
            ({**document(), "Definitions": {"X": {}}}, "AnimSequence.UnsupportedDefinitions", "/Definitions"),
            ({**document(), "Class": "/Script/Engine.AnimMontage"}, "AnimSequence.ClassMismatch", "/Class"),
            ({**document(), "Target": "/Game/Bad Name"}, "AnimSequence.NonCanonicalTarget", "/Target"),
        )
        for candidate, code, path in cases:
            with self.subTest(code=code, path=path):
                assert_error(self, code, path, lambda candidate=candidate: self.profile.validate(candidate))

    def test_aliases_and_transport_numbers(self):
        body = {"Notifies": [{"Time": 0.5, "TrackName": "1", "Name": "X"}],
                "Additive": {"RefPoseSeq": {"Kind": "AssetRef", "Path": "/Game/A/B"}},
                "Playback": {"NumberOfFrames": 30.0, "FrameRate": "30000/1001"},
                "Metadata": [{"Kind": "EmbeddedObject", "Class": "/Script/A.Meta", "Properties": {}}]}
        canonical = codec.canonical_body(body)
        self.assertEqual(canonical["Notifies"][0]["Track"], "1")
        self.assertEqual(canonical["Additive"]["RefPoseSeq"], "/Game/A/B.B")
        self.assertEqual(canonical["Playback"], {"NumberOfFrames": 30, "FrameRate": (30000, 1001)})
        self.assertEqual(canonical["Metadata"][0]["Class"], "/Script/A.Meta")


class AnimSequenceLoweringTests(unittest.TestCase):
    def test_lower_routes_each_field_to_its_owner(self):
        body = {"References": {"Skeleton": SKELETON, "RetargetSourceAsset": None},
                "Preview": {"PreviewMesh": None, "PreviewPoseAsset": "/Game/P"},
                "Playback": {"RateScale": 2, "NumberOfFrames": 60, "FrameRate": 30},
                "Curves": [{"Name": "Speed", "Keys": [{"Time": 0, "Value": 1}], "Comment": "c"}],
                "NotifyTracks": [{"Name": "Sound"}],
                "Notifies": [{"Time": 0.25, "Class": "/Script/Engine.AnimNotify_PlaySound",
                              "Properties": {"VolumeMultiplier": 0.5}, "NotifyTriggerChance": 0.5},
                             {"Time": 0.5, "Track": "Sound", "Name": "Footstep"}],
                "SyncMarkers": [{"Name": "L", "Time": 0.5}]}
        intent, fields = codec.lower(codec.canonical_body(body), {"StripAnimDataOnDedicatedServer": "Strip",
                                                                  "SomeReference": "/Game/X"},
                                     {"RetargetSourceAsset", "RateScale", "StripAnimDataOnDedicatedServer"})
        self.assertEqual(fields, {"RetargetSourceAsset": "None", "RateScale": 2.0,
                                  "StripAnimDataOnDedicatedServer": "Strip"})
        self.assertEqual(intent["skeleton"], SKELETON)
        self.assertIsNone(intent["preview_mesh"])
        self.assertEqual(intent["object_properties"], {"PreviewPoseAsset": "/Game/P.P", "SomeReference": "/Game/X"})
        self.assertEqual((intent["number_of_frames"], intent["frame_rate"]), (60, {"numerator": 30, "denominator": 1}))
        self.assertEqual(intent["curves"][0]["comment"], "c")
        self.assertNotIn("flags", intent["curves"][0])
        self.assertEqual(intent["notify_tracks"], ["Sound"])
        self.assertEqual(intent["notifies"][0], {"time": 0.25, "class": "/Script/Engine.AnimNotify_PlaySound",
                                                 "properties": {"VolumeMultiplier": 0.5},
                                                 "trigger": {"NotifyTriggerChance": 0.5}})
        self.assertEqual(intent["notifies"][1], {"time": 0.5, "track": "Sound", "class": None, "name": "Footstep"})
        self.assertEqual(intent["sync_markers"], [{"name": "L", "time": 0.5}])

    def test_document_paths_from_primitive_paths(self):
        for native, expected in (
                ("/notifies/0/properties/Subobject/Properties", "/Body/Notifies/0/Properties/Subobject/Properties"),
                ("/notify_states/1/trigger/NotifyFilterLOD", "/Body/NotifyStates/1/NotifyFilterLOD"),
                ("/object_properties/RefPoseSeq", "/Body/Additive/RefPoseSeq"),
                ("/object_properties/Other", "/Properties/Other"),
                ("/notify_tracks", "/Body/NotifyTracks"),
                ("/curves/0/keys/1", "/Body/Curves/0/Keys/1"),
                ("/skeleton", "/Body/References/Skeleton"),
                ("", "/Body")):
            with self.subTest(native=native):
                self.assertEqual(document_path(native), expected)


class AnimSequenceExtractDiffTests(unittest.TestCase):
    def setUp(self):
        self.profile = AnimSequenceRuntimeProfile()

    def extracted(self, native=None):
        context = FakeContext()
        if native is not None:
            context.native = native
        return self.profile.canonicalize(self.profile.extract(target=TARGET, asset_handle=Handle("read"), context=context))

    def test_extract_projects_every_region(self):
        document_ = self.extracted()
        body = document_["Body"]
        self.assertEqual(set(body), set(codec.BODY_KEYS))
        self.assertEqual(body["References"], {"Skeleton": SKELETON, "RetargetSource": "None", "RetargetSourceAsset": None})
        self.assertEqual(body["Compression"]["BoneCompressionSettings"], "/ACLPlugin/Bone.Bone")
        self.assertEqual(body["Playback"]["FrameRate"], 30)
        self.assertEqual(body["Curves"][0]["Flags"], ["Editable"])
        self.assertEqual(body["Curves"][0]["Keys"][0]["InterpMode"], "Linear")
        self.assertNotIn("Comment", body["Curves"][0])
        self.assertEqual(body["NotifyTracks"], [{"Name": "Sound"}, {"Name": "FX"}])
        self.assertEqual(body["Notifies"][0]["NotifyTriggerChance"], 0.5)
        self.assertNotIn("NotifyFilterType", body["Notifies"][0])
        self.assertNotIn("Name", body["Notifies"][0])
        self.assertEqual(body["Notifies"][1], {"Time": 0.5, "Track": "Sound", "Name": "Footstep"})
        self.assertEqual(body["NotifyStates"][0]["Duration"], 0.25)
        self.assertEqual(body["BoneTracks"], ["root"])
        self.assertEqual(document_["Properties"], {"StripAnimDataOnDedicatedServer": "UseProjectSetting"})

    def test_extract_round_trip_has_no_differences(self):
        extracted = self.extracted()
        self.assertEqual(self.profile.diff_exact(extracted, copy.deepcopy(extracted)), ())

    def test_sparse_authoring_compares_only_authored_fields(self):
        extracted = self.extracted()
        authored = document({"Playback": {"RateScale": 1}, "Curves": [{"Name": "Speed", "Keys": [
            {"Time": 0, "Value": 1, "InterpMode": "Linear"}, {"Time": 1, "Value": 2, "InterpMode": "Linear"}]}],
            "Notifies": [{"Time": 0.5, "Name": "Footstep"},
                         {"Time": 0.25, "Class": "/Script/Engine.AnimNotify_PlaySound",
                          "Properties": {"VolumeMultiplier": 0.5}, "NotifyTriggerChance": 0.5}],
            "SyncMarkers": [{"Name": "Left", "Time": 0.5}]})
        self.assertEqual(self.profile.diff_exact(authored, extracted), ())

    def test_differences_are_reported_per_field_and_region(self):
        extracted = self.extracted()
        authored = document({"Playback": {"RateScale": 2, "bLoop": False},
                             "Notifies": [{"Time": 0.5, "Name": "Footstep"}],
                             "Curves": [], "NotifyStates": [{"Time": 0.5, "Duration": 0.5, "Track": "FX",
                                                              "Class": "/Script/Engine.AnimNotifyState_TimedParticleEffect"}]},
                            {"StripAnimDataOnDedicatedServer": "Strip"})
        paths = [item.path for item in self.profile.diff_exact(authored, extracted)]
        self.assertEqual(paths, ["/Body/Playback/RateScale", "/Body/Curves", "/Body/Notifies", "/Body/NotifyStates",
                                 "/Properties/StripAnimDataOnDedicatedServer"])

    def test_tangents_compare_only_when_authored(self):
        extracted = self.extracted()
        auto = document({"Curves": [{"Name": "Speed", "Keys": [
            {"Time": 0, "Value": 1, "InterpMode": "Linear", "LeaveTangent": 9},
            {"Time": 1, "Value": 2, "InterpMode": "Linear"}]}]})
        self.assertEqual(self.profile.diff_exact(auto, extracted), ())
        user = copy.deepcopy(auto)
        user["Body"]["Curves"][0]["Keys"][0]["TangentMode"] = "User"
        self.assertEqual([item.path for item in self.profile.diff_exact(user, extracted)], ["/Body/Curves"])


class AnimSequenceOrchestrationTests(unittest.TestCase):
    def setUp(self):
        self.profile = AnimSequenceRuntimeProfile()

    def test_preflight_is_snapshot_covered_and_reads_nothing_without_bone_tracks(self):
        context = FakeContext()
        self.assertIs(self.profile.preflight(document({"Playback": {"RateScale": 2}}), None, context), SNAPSHOT_COVERED)
        self.assertEqual(context.calls, [])

    def test_preflight_rejects_changed_bone_tracks(self):
        context = FakeContext()
        self.assertIs(self.profile.preflight(document({"BoneTracks": ["root"]}), None, context), SNAPSHOT_COVERED)
        assert_error(self, "AnimSequence.ReadOnlyRegion", "/Body/BoneTracks",
                     lambda: self.profile.preflight(document({"BoneTracks": []}), None, FakeContext()))
        assert_error(self, "AnimSequence.ReadOnlyRegion", "/Body/BoneTracks",
                     lambda: self.profile.preflight(document({"BoneTracks": ["root"]}), None, FakeContext(exists=False)))

    def test_apply_writes_plain_fields_then_one_authoring_edit(self):
        context = FakeContext()
        body = {"Playback": {"RateScale": 2, "NumberOfFrames": 60}, "Additive": {"RefPoseSeq": None},
                "Curves": []}
        self.profile.apply(document(body, {"StripAnimDataOnDedicatedServer": "Strip"}), Handle("read"),
                           Handle("mutable"), context)
        self.assertEqual(context.names(), [profile_module.EDITOR_ENUMERATE, profile_module.EDITOR_WRITE, profile_module.EDIT])
        self.assertEqual(context.calls[1][1], {"properties": [{"name": "RateScale", "value": 2.0},
                                                              {"name": "StripAnimDataOnDedicatedServer", "value": "Strip"}]})
        self.assertEqual(context.calls[1][2], ("asset", "property_batch"))
        self.assertEqual(context.calls[2][1], {"number_of_frames": 60, "object_properties": {"RefPoseSeq": None},
                                               "curves": []})
        self.assertEqual(context.handoffs, [("mutable", "mutable")])

    def test_apply_without_plain_fields_skips_the_batch(self):
        context = FakeContext()
        self.profile.apply(document({"Notifies": []}), Handle("read"), Handle("mutable"), context)
        self.assertEqual(context.names(), [profile_module.EDIT])

    def test_apply_of_nothing_invokes_nothing(self):
        context = FakeContext()
        self.assertEqual(self.profile.apply(document(), Handle("read"), Handle("mutable"), context), ())
        self.assertEqual(context.calls, [])

    def test_primitive_failures_are_reported_at_the_document_path(self):
        context = FakeContext(fail=(profile_module.EDIT, "/notifies/1/properties"))
        assert_error(self, "Test.Rejected", "/Body/Notifies/1/Properties",
                     lambda: self.profile.apply(document({"Notifies": []}), Handle("read"), Handle("mutable"), context))
        context = FakeContext(fail=(profile_module.EDITOR_WRITE, "/properties/1"))
        assert_error(self, "Test.Rejected", "/Body/Playback/bLoop",
                     lambda: self.profile.apply(document({"Playback": {"RateScale": 1, "bLoop": True}}),
                                                Handle("read"), Handle("mutable"), context))

    def test_unknown_plain_field_is_rejected_before_writing(self):
        context = FakeContext(writable=("RateScale",))
        assert_error(self, "AnimSequence.UnknownProperty", "/Body/Playback/bLoop",
                     lambda: self.profile.apply(document({"Playback": {"bLoop": True}}), Handle("read"),
                                                Handle("mutable"), context))
        self.assertEqual(context.names(), [profile_module.EDITOR_ENUMERATE])

    def test_create_requires_and_resolves_the_skeleton(self):
        assert_error(self, "AnimSequence.SkeletonRequired", "/Body/References/Skeleton",
                     lambda: self.profile.create_target_context(document(), FakeContext()))
        assert_error(self, "AnimSequence.ReferenceUnavailable", "/Body/References/Skeleton",
                     lambda: self.profile.create_target_context(document({"References": {"Skeleton": SKELETON}}),
                                                                FakeContext(exists=False)))
        context = FakeContext()
        handles = self.profile.create_target_context(document({"References": {"Skeleton": SKELETON}}), context)
        self.assertEqual(tuple(item.name for item in handles), ("factory_context",))
        self.assertEqual(context.calls[0][1], {"path": SKELETON, "expected_class": "/Script/Engine.Skeleton"})


if __name__ == "__main__":
    unittest.main()
