# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Skeleton, blend space, PoseAsset and AnimComposite Profiles: document validation and op planning.
The editor side is covered end to end by UEAssetDoc.AnimationAssets.* in the editor."""
from __future__ import annotations

import unittest

from asset_document.diagnostics import AssetDocumentContractError, collecting_read_warnings
from asset_document.profiles import anim_composite as composite
from asset_document.profiles import blend_space
from asset_document.profiles import pose_asset
from asset_document.profiles import skeleton

SKELETON = "/Game/Characters/SK_Hero_Skeleton.SK_Hero_Skeleton"
IDLE = "/Game/Anims/Idle.Idle"
WALK = "/Game/Anims/Walk.Walk"


def _document(class_path, body, properties=None, target="/Game/Anims/Asset"):
    return {"SchemaVersion": 1, "Target": target, "Class": class_path, "Action": "CreateOrUpdate",
            "Properties": properties or {}, "Body": body}


def _ops(ops):
    return [op for op, _ in ops]


class AnimationAssetTestCase(unittest.TestCase):
    def assertRefused(self, callback, code, path):
        with self.assertRaises(AssetDocumentContractError) as raised:
            callback()
        self.assertEqual((raised.exception.diagnostic.code, raised.exception.diagnostic.path), (code, path))
        return raised.exception.diagnostic


class SkeletonTests(AnimationAssetTestCase):
    LIVE = {"slot_groups": [{"name": "DefaultGroup", "slots": ["DefaultSlot", "UpperBody"]}, {"name": "Face", "slots": ["Jaw"]}],
            "sockets": [{"name": "hand_r_weapon", "bone": "hand_r", "location": {"X": 0, "Y": 5, "Z": 0},
                         "rotation": {"Pitch": 0, "Yaw": 0, "Roll": 0}, "scale": {"X": 1, "Y": 1, "Z": 1},
                         "force_always_animated": True}],
            "virtual_bones": [{"name": "VB ik", "source": "pelvis", "target": "hand_r"}],
            "blend_profiles": [{"name": "Upper", "mode": "WeightFactor", "bones": {"spine_01": 1.5}}],
            "retargeting": {"pelvis": "AnimationScaled"},
            "curves": [{"name": "CTRL_Blink", "material": False, "morph_target": True}]}

    def test_invalid_documents_name_the_path(self):
        canonical = skeleton.canonicalize
        self.assertRefused(lambda: canonical(_document(skeleton.CLASS, {"SlotGroups": [{"Name": "Face", "Slots": []}]})),
                           "Skeleton.DefaultGroupRequired", "/Body/SlotGroups")
        self.assertRefused(lambda: canonical(_document(skeleton.CLASS, {"SlotGroups": [
            {"Name": "DefaultGroup", "Slots": ["A"]}, {"Name": "Face", "Slots": ["A"]}]})),
            "Skeleton.DuplicateName", "/Body/SlotGroups/1/Slots/0")
        self.assertRefused(lambda: canonical(_document(skeleton.CLASS, {"VirtualBones": [
            {"Name": "ik", "Source": "a", "Target": "b"}]})), "Skeleton.InvalidVirtualBone", "/Body/VirtualBones/0/Name")
        self.assertRefused(lambda: canonical(_document(skeleton.CLASS, {"Sockets": [
            {"Name": "s", "Bone": "b", "Location": {"X": 1}}]})), "Skeleton.InvalidSocket", "/Body/Sockets/0/Location")
        self.assertRefused(lambda: canonical(_document(skeleton.CLASS, {"BoneRetargeting": {"pelvis": "Sideways"}})),
                           "Skeleton.InvalidRetargetingMode", "/Body/BoneRetargeting/pelvis")
        self.assertRefused(lambda: canonical(_document(skeleton.CLASS, {"Bones": []})), "Skeleton.UnknownBodyKey", "/Body/Bones")
        # A name with a space is valid (virtual bones carry the "VB " prefix).
        canonical(_document(skeleton.CLASS, {"VirtualBones": [{"Name": "VB hand ik", "Source": "a", "Target": "b"}]}))

    def test_unchanged_body_plans_nothing(self):
        body = skeleton.canonical_body(skeleton.body_from_live(self.LIVE))
        self.assertEqual(skeleton.body_ops(body, self.LIVE), [])

    def test_socket_without_bone_is_read_with_a_warning_and_kept(self):
        # Fab Paragon skeletons carry sockets on bone "None": a read lists them with a warning, an apply that leaves
        # them alone plans nothing, and a document that puts a socket on no bone is refused.
        live = dict(self.LIVE, sockets=self.LIVE["sockets"] + [dict(self.LIVE["sockets"][0], name="fx_orphan", bone="None")])
        with collecting_read_warnings() as warnings:
            body = skeleton.body_from_live(live)
            skeleton.warn_sockets_without_bone(body)
        self.assertEqual([(w.code, w.path) for w in warnings], [("Skeleton.SocketWithoutBone", "/Body/Sockets/0/Bone")])
        body = skeleton.canonical_body(body)
        self.assertEqual(skeleton.body_ops(body, live), [])
        moved = skeleton.canonical_body({"Sockets": [dict(row, Bone="None") if row["Name"] == "hand_r_weapon" else row
                                                     for row in body["Sockets"]]})
        self.assertRefused(lambda: skeleton.body_ops(moved, live), "Skeleton.InvalidSocket", "/Body/Sockets/1/Bone")
        added = skeleton.canonical_body({"Sockets": body["Sockets"] + [{"Name": "new", "Bone": "None"}]})
        self.assertRefused(lambda: skeleton.body_ops(added, live), "Skeleton.InvalidSocket", "/Body/Sockets/2/Bone")
        # Outside a read nothing is recorded.
        skeleton.warn_sockets_without_bone(body)

    def test_slot_ops_add_rename_move_remove_in_editor_order(self):
        groups = skeleton.canonical_body({"SlotGroups": [
            {"Name": "DefaultGroup", "Slots": ["DefaultSlot", {"Name": "Upper", "RenamedFrom": "UpperBody"}, "Jaw"]},
            {"Name": "Hands", "Slots": ["Grip"]}]})["SlotGroups"]
        self.assertEqual(_ops(skeleton.slot_ops(groups, self.LIVE)), [
            {"op": "slot_group_add", "name": "Hands"},
            {"op": "slot_rename", "name": "UpperBody", "to": "Upper"},
            {"op": "slot_set", "name": "Jaw", "group": "DefaultGroup"},
            {"op": "slot_set", "name": "Grip", "group": "Hands"},
            {"op": "slot_group_remove", "name": "Face"}])

    def test_named_sections_rename_change_replace_and_remove(self):
        body = skeleton.canonical_body({
            "Sockets": [{"Name": "weapon", "RenamedFrom": "hand_r_weapon", "Bone": "hand_l"}],
            "VirtualBones": [{"Name": "VB ik", "Source": "pelvis", "Target": "hand_l"}],
            "BlendProfiles": [{"Name": "Upper", "Mode": "BlendMask", "Bones": {"spine_01": 0, "spine_02": 1}}],
            "BoneRetargeting": {"root": "Skeleton"},
            "Curves": []})
        self.assertEqual(_ops(skeleton.body_ops(body, self.LIVE)), [
            {"op": "virtual_bone_remove", "name": "VB ik"},
            {"op": "virtual_bone_add", "name": "VB ik", "source": "pelvis", "target": "hand_l"},
            {"op": "socket_rename", "name": "hand_r_weapon", "to": "weapon"},
            {"op": "socket_set", "name": "weapon", "bone": "hand_l"},
            {"op": "blend_profile_set", "name": "Upper", "mode": "BlendMask", "bones": {"spine_02": 1}},
            {"op": "retarget", "bone": "pelvis", "mode": "Animation"},
            {"op": "retarget", "bone": "root", "mode": "Skeleton"},
            {"op": "curve_remove", "name": "CTRL_Blink"}])

    def test_primitive_paths_map_to_document_paths(self):
        paths = ["/Body/Sockets/1", "/Body/BlendProfiles/0"]
        self.assertEqual(skeleton.map_path("/ops/0/bone", paths), "/Body/Sockets/1/Bone")
        self.assertEqual(skeleton.map_path("/ops/1/bones/spine_01", paths), "/Body/BlendProfiles/0/Bones/spine_01")
        self.assertEqual(skeleton.map_path("/schema_version", paths), "/Body")

    def test_create_is_refused(self):
        profile = skeleton.SkeletonRuntimeProfile()
        self.assertRefused(lambda: profile.create_target_context(_document(skeleton.CLASS, {}), None),
                           "Skeleton.CreateUnsupported", "/Target")
        # The refusal comes before any Body diagnostic.
        create = {**_document(skeleton.CLASS, {"SlotGroups": []}), "Action": "Create"}
        self.assertRefused(lambda: profile.validate(create), "Skeleton.CreateUnsupported", "/Target")


class BlendSpaceTests(AnimationAssetTestCase):
    LIVE = {"skeleton": SKELETON, "preview_mesh": "",
            "axes": [{"name": "Direction", "min": -180, "max": 180, "grid": 4, "snap": False, "wrap": False},
                     {"name": "Speed", "min": 0, "max": 600, "grid": 4, "snap": False, "wrap": False}],
            "samples": [{"animation": IDLE, "x": 0, "y": 0, "rate_scale": 1, "single_frame": None, "analyse_all": True,
                         "valid": True}]}

    def test_invalid_documents_name_the_path(self):
        canonical = blend_space.canonicalize
        self.assertRefused(lambda: canonical(_document(blend_space.BLEND_SPACE_1D_CLASS, {"Axes": {"Y": {}}})),
                           "BlendSpace.InvalidAxis", "/Body/Axes/Y")
        self.assertRefused(lambda: canonical(_document(blend_space.BLEND_SPACE_CLASS, {"Axes": {"X": {"Min": 5, "Max": 1}}})),
                           "BlendSpace.InvalidAxis", "/Body/Axes/X/Min")
        self.assertRefused(lambda: canonical(_document(blend_space.BLEND_SPACE_CLASS, {"Samples": [
            {"Animation": IDLE, "X": 0, "Y": 0}, {"Animation": WALK, "X": 0, "Y": 0}]})),
            "BlendSpace.DuplicateSample", "/Body/Samples/1")
        self.assertRefused(lambda: canonical(_document(blend_space.BLEND_SPACE_CLASS, {"Samples": [
            {"Animation": IDLE, "X": 0}]})), "BlendSpace.InvalidSample", "/Body/Samples/0/Y")
        self.assertRefused(lambda: canonical(_document(blend_space.BLEND_SPACE_CLASS, {"Samples": [
            {"Animation": "Idle", "X": 0, "Y": 0}]})), "BlendSpace.InvalidSample", "/Body/Samples/0/Animation")

    def test_range_and_grid_are_checked_against_the_effective_axes(self):
        names = blend_space.BLEND_SPACE_CLASS
        body = blend_space.canonical_body({"Samples": [{"Animation": WALK, "X": 0, "Y": 700}]}, names)
        self.assertRefused(lambda: blend_space.check_samples(body, blend_space.effective_axes(body, self.LIVE, names), names),
                           "BlendSpace.SampleOutOfRange", "/Body/Samples/0/Y")
        widened = blend_space.canonical_body({"Axes": {"Y": {"Max": 800}}, "Samples": [{"Animation": WALK, "X": 0, "Y": 700}]}, names)
        blend_space.check_samples(widened, blend_space.effective_axes(widened, self.LIVE, names), names)
        snapping = blend_space.canonical_body({"Axes": {"X": {"SnapToGrid": True}, "Y": {"SnapToGrid": True}},
                                               "Samples": [{"Animation": WALK, "X": 10, "Y": 150}]}, names)
        self.assertRefused(lambda: blend_space.check_samples(snapping, blend_space.effective_axes(snapping, self.LIVE, names),
                                                             names), "BlendSpace.SampleOffGrid", "/Body/Samples/0/X")

    def test_plan_removes_before_axis_edits_and_adds_after(self):
        names = blend_space.BLEND_SPACE_CLASS
        body = blend_space.canonical_body({"Axes": {"Y": {"Name": "Velocity"}}, "Samples": [
            {"Animation": WALK, "X": 90, "Y": 300, "RateScale": 2}], "PreviewMesh": "/Game/SK_Hero.SK_Hero"}, names)
        self.assertEqual(_ops(blend_space.plan(body, self.LIVE, names)), [
            {"op": "sample_remove", "x": 0, "y": 0},
            {"op": "axis", "axis": 1, "name": "Velocity"},
            {"op": "sample_add", "animation": WALK, "x": 90, "y": 300},
            {"op": "sample_set", "x": 90, "y": 300, "rate_scale": 2},
            {"op": "preview_mesh", "path": "/Game/SK_Hero.SK_Hero"}])
        unchanged = blend_space.canonical_body(blend_space.body_from_live(self.LIVE, names), names)
        self.assertEqual(blend_space.plan(unchanged, self.LIVE, names), [])

    def test_primitive_paths_map_to_document_paths(self):
        paths = ["/Body/Samples/2", "/Body/Axes/Y", "/Body/PreviewMesh"]
        self.assertEqual(blend_space.map_path("/ops/0/animation", paths), "/Body/Samples/2/Animation")
        self.assertEqual(blend_space.map_path("/ops/1/max", paths), "/Body/Axes/Y/Max")
        self.assertEqual(blend_space.map_path("/ops/2/path", paths), "/Body/PreviewMesh")

    def test_create_requires_the_skeleton(self):
        self.assertRefused(lambda: blend_space.create_target_context(_document(blend_space.BLEND_SPACE_CLASS, {}), None),
                           "BlendSpace.SkeletonRequired", "/Body/Skeleton")


class PoseAssetTests(AnimationAssetTestCase):
    LIVE = {"poses": ["Pose_0", "Pose_1", "Pose_2"], "additive": False, "base_pose": "", "skeleton": SKELETON}

    def _poses(self, rows):
        return pose_asset.canonical_body({"Poses": rows})["Poses"]

    def test_rename_and_remove(self):
        ops = pose_asset.pose_ops(self._poses([{"Name": "Contact", "RenamedFrom": "Pose_0"}, "Pose_2"]), self.LIVE)
        self.assertEqual(_ops(ops), [{"op": "remove", "names": ["Pose_1"]},
                                     {"op": "rename", "name": "Pose_0", "to": "Contact"}])

    def test_poses_cannot_be_added_or_reordered(self):
        self.assertRefused(lambda: pose_asset.pose_ops(self._poses(["Pose_0", "New"]), self.LIVE),
                           "PoseAsset.PoseAddUnsupported", "/Body/Poses/1")
        self.assertRefused(lambda: pose_asset.pose_ops(self._poses(["Pose_1", "Pose_0"]), self.LIVE),
                           "PoseAsset.PoseOrderUnsupported", "/Body/Poses")
        self.assertRefused(lambda: pose_asset.canonicalize(_document(pose_asset.CLASS, {"Poses": ["A", "A"]})),
                           "PoseAsset.DuplicatePose", "/Body/Poses/1")

    def test_create_requires_the_source_animation(self):
        self.assertRefused(lambda: pose_asset.create_target_context(_document(pose_asset.CLASS, {}), None),
                           "PoseAsset.SourceAnimationRequired", "/Body/SourceAnimation")


class AnimCompositeTests(AnimationAssetTestCase):
    def test_segment_validation(self):
        canonical = composite.canonicalize
        self.assertRefused(lambda: canonical(_document(composite.CLASS, {"Segments": [{"Animation": IDLE, "PlayRate": 0}]})),
                           "AnimComposite.InvalidSegment", "/Body/Segments/0/PlayRate")
        self.assertRefused(lambda: canonical(_document(composite.CLASS, {"Segments": [
            {"Animation": IDLE, "StartTime": 1, "EndTime": 0.5}]})), "AnimComposite.InvalidSegment", "/Body/Segments/0/EndTime")
        self.assertRefused(lambda: canonical(_document(composite.CLASS, {"Segments": [{"Animation": IDLE, "Loops": 2}]})),
                           "AnimComposite.InvalidSegment", "/Body/Segments/0/Loops")

    def test_integral_numbers_from_diff_are_integers(self):
        # A Diff request hands numbers over as doubles; 2.0 is the integer 2, 2.5 is refused.
        body = composite.canonical_body({"Segments": [{"Animation": IDLE, "LoopCount": 2.0}]})
        self.assertEqual(body["Segments"][0]["LoopCount"], 2)
        self.assertIs(type(body["Segments"][0]["LoopCount"]), int)
        self.assertRefused(lambda: composite.canonicalize(_document(composite.CLASS, {"Segments": [{"Animation": IDLE, "LoopCount": 2.5}]})),
                           "AnimComposite.InvalidSegment", "/Body/Segments/0/LoopCount")
        axes = blend_space.canonical_body({"Axes": {"X": {"GridDivisions": 4.0}}}, blend_space.BLEND_SPACE_CLASS)["Axes"]
        self.assertIs(type(axes["X"]["GridDivisions"]), int)

    def test_positional_replace_keeps_omitted_fields_of_the_same_segment(self):
        live = [{"Animation": IDLE, "StartTime": 0, "EndTime": 2, "PlayRate": 1.5, "LoopCount": 2}]
        body = composite.canonical_body({"Segments": [{"Animation": IDLE, "LoopCount": 3}, {"Animation": WALK}]})
        self.assertEqual(composite.segments_op(body["Segments"], live), {"op": "segments", "segments": [
            {"animation": IDLE, "start": 0, "end": 2, "rate": 1.5, "loops": 3}, {"animation": WALK}]})
        self.assertEqual(composite.map_path("/ops/0/segments/1/rate", ["/Body/Segments"]), "/Body/Segments/1/PlayRate")


if __name__ == "__main__":
    unittest.main()
