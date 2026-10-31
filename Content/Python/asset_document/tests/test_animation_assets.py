# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Skeleton, blend space, PoseAsset and AnimComposite Profiles: document validation and op planning.
The editor side is covered end to end by UEAssetDoc.AnimationAssets.* in the editor."""
from __future__ import annotations

import types
import unittest

from asset_document import tolerance
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

    RICH = dict(LIVE, curves=[{"name": "CTRL_Blink", "material": False, "morph_target": True,
                               "linked_bones": ["eyelid_l"], "max_lod": 2},
                              {"name": "CTRL_Smile", "material": True, "morph_target": False, "linked_bones": [],
                               "max_lod": 255}],
                preview_mesh="/Game/SK_Hero.SK_Hero", additional_preview_meshes="", notifies=["Step", "Land"],
                preview_attached_assets=[{"asset": "/Game/SM_Sword.SM_Sword", "attached_to": "hand_r_weapon"},
                                         {"asset": "/Game/SM_Hat.SM_Hat", "attached_to": "head"}],
                retarget_sources=[{"name": "Tall", "mesh": "/Game/SK_Tall.SK_Tall"}], curve_metadata_index=0)

    def test_rich_body_reads_and_plans_nothing(self):
        body = skeleton.body_from_live(self.RICH)
        self.assertEqual(body["Curves"], [
            {"Name": "CTRL_Blink", "Material": False, "MorphTarget": True, "LinkedBones": ["eyelid_l"], "MaxLOD": 2},
            {"Name": "CTRL_Smile", "Material": True, "MorphTarget": False}])
        self.assertEqual((body["PreviewMesh"], body["AdditionalPreviewMeshes"]), ("/Game/SK_Hero.SK_Hero", ""))
        self.assertEqual(body["AnimNotifyNames"], ["Land", "Step"])
        self.assertEqual(body["RetargetSources"], [{"Name": "Tall", "Mesh": "/Game/SK_Tall.SK_Tall"}])
        self.assertNotIn("PreviewAttachedAssets", skeleton.body_from_live(self.LIVE))
        canonical = skeleton.canonical_body(body)
        self.assertEqual(skeleton.body_ops(canonical, self.RICH), [])
        document = _document(skeleton.CLASS, body)
        self.assertEqual(list(skeleton.create_skeleton_runtime_handler().diff(document, document)), [])

    def test_curve_bone_links(self):
        body = skeleton.canonical_body({"Curves": [{"Name": "CTRL_Blink", "LinkedBones": [], "MaxLOD": 255},
                                                   {"Name": "CTRL_Smile", "LinkedBones": ["jaw", "lip"]},
                                                   {"Name": "New", "MaxLOD": 1}]})
        self.assertEqual(_ops(skeleton.body_ops(body, self.RICH)), [
            {"op": "curve_set", "name": "CTRL_Blink", "linked_bones": [], "max_lod": 255},
            {"op": "curve_set", "name": "CTRL_Smile", "linked_bones": ["jaw", "lip"]},
            {"op": "curve_add", "name": "New"}, {"op": "curve_set", "name": "New", "max_lod": 1}])
        self.assertRefused(lambda: skeleton.canonical_body({"Curves": [{"Name": "A", "MaxLOD": 256}]}),
                           "Skeleton.InvalidCurve", "/Body/Curves/0/MaxLOD")
        self.assertRefused(lambda: skeleton.canonical_body({"Curves": [{"Name": "A", "LinkedBones": ["a", "a"]}]}),
                           "Skeleton.InvalidCurve", "/Body/Curves/0/LinkedBones/1")
        self.assertEqual(skeleton.map_path("/ops/1/linked_bones/0", ["/Body/Curves/0", "/Body/Curves/1"]),
                         "/Body/Curves/1/LinkedBones/0")
        # A curve that leaves them out reads as the defaults.
        live = _document(skeleton.CLASS, skeleton.body_from_live(self.RICH))
        authored = _document(skeleton.CLASS, {"Curves": [{"Name": "CTRL_Blink", "LinkedBones": ["eyelid_l"]},
                                                         {"Name": "CTRL_Smile", "LinkedBones": [], "MaxLOD": 255}]})
        self.assertEqual(list(skeleton.create_skeleton_runtime_handler().diff(authored, live)), [])

    def test_preview_settings_and_notify_names(self):
        body = skeleton.canonical_body({"PreviewMesh": "", "AdditionalPreviewMeshes": "/Game/PMC.PMC",
                                        "AnimNotifyNames": ["Step", "Jump"]})
        self.assertEqual(_ops(skeleton.body_ops(body, self.RICH)), [
            {"op": "preview_mesh", "path": ""}, {"op": "additional_preview_meshes", "path": "/Game/PMC.PMC"},
            {"op": "notify_add", "name": "Jump"}])
        # Names left out are kept: a subset is no difference.
        live = _document(skeleton.CLASS, skeleton.body_from_live(self.RICH))
        self.assertEqual(list(skeleton.create_skeleton_runtime_handler().diff(
            _document(skeleton.CLASS, {"AnimNotifyNames": ["Step"]}), live)), [])
        self.assertRefused(lambda: skeleton.canonical_body({"AnimNotifyNames": ["A", "A"]}),
                           "Skeleton.InvalidNotifyName", "/Body/AnimNotifyNames/1")

    def test_read_only_sections(self):
        body = skeleton.body_from_live(self.RICH)
        changed = skeleton.canonical_body({"RetargetSources": [{"Name": "Short", "Mesh": "/Game/SK_Tall.SK_Tall"}]})
        self.assertRefused(lambda: skeleton.body_ops(changed, self.RICH), "Skeleton.RetargetSourcesReadOnly",
                           "/Body/RetargetSources")
        attached = body["PreviewAttachedAssets"]
        self.assertRefused(lambda: skeleton.body_ops(skeleton.canonical_body({"PreviewAttachedAssets": attached[:1]}), self.RICH),
                           "Skeleton.PreviewAttachedAssetsReadOnly", "/Body/PreviewAttachedAssets")
        # A renamed socket keeps its attachment, a removed one loses it, as the skeleton tree does.
        renamed = skeleton.canonical_body({"Sockets": [{"Name": "weapon", "RenamedFrom": "hand_r_weapon", "Bone": "hand_r"}],
                                           "PreviewAttachedAssets": [dict(attached[0], AttachedTo="weapon"), attached[1]]})
        skeleton.body_ops(renamed, self.RICH)
        removed = skeleton.canonical_body({"Sockets": [], "PreviewAttachedAssets": [attached[1]]})
        skeleton.body_ops(removed, self.RICH)
        kept = skeleton.canonical_body({"Sockets": [], "PreviewAttachedAssets": attached})
        self.assertRefused(lambda: skeleton.body_ops(kept, self.RICH), "Skeleton.PreviewAttachedAssetsReadOnly",
                           "/Body/PreviewAttachedAssets")

    def test_asset_user_data_leaves_out_only_the_curve_metadata(self):
        curve_metadata = {"Class": skeleton.CURVE_METADATA_CLASS, "Properties": {}}
        user = {"Class": "/Script/Game.Modifiers", "Properties": {"Count": 1}}
        other = {"Class": "/Script/Game.Other", "Properties": {}}

        class Adapter:
            def __init__(self, values):
                self.values = values

            def enumerate(self, class_path, context):
                return (types.SimpleNamespace(name="AssetUserData"),)

            def extract(self, read, admitted, context, include_defaults=True):
                return dict(self.values)

        region = skeleton._properties()
        self.assertNotIn("AssetUserData", region.body_owned)
        region.adapter = Adapter({"AssetUserData": [user, curve_metadata]})
        self.assertEqual(region.extract(None, None, include_defaults=False), {"AssetUserData": [user]})
        region.adapter = Adapter({"AssetUserData": [curve_metadata]})
        self.assertEqual(region.extract(None, None, include_defaults=False), {})
        self.assertRefused(lambda: region.check_owned({"AssetUserData": [curve_metadata]}), "Skeleton.BodyOwnedProperty",
                           "/Properties/AssetUserData/0")
        # An apply parks the curve metadata object while AssetUserData is written, and writes the rest after it.
        steps = []

        class Writer(Adapter):
            def apply(self, read, mutable, properties, admitted, context, phase=None):
                steps.append(("write", properties))
                return ("/Properties/" + name for name in properties)

        region.adapter = Writer({})
        original = skeleton.edit
        skeleton.edit = lambda calls, ops: steps.extend(("edit", op) for op in _ops(ops)) or []
        try:
            region.apply({"AssetUserData": [other], "PreviewForwardAxis": "Y"}, None, None, None, None,
                         {"curve_metadata_index": 1})
            self.assertEqual(steps, [("edit", {"op": "curve_metadata_park", "index": 1}),
                                     ("write", {"AssetUserData": [other]}),
                                     ("edit", {"op": "curve_metadata_unpark", "index": 1}),
                                     ("write", {"PreviewForwardAxis": "Y"})])
            # AssetUserData as the asset has it (the curve metadata object left out): nothing parked or written.
            del steps[:]
            region.adapter = Writer({"AssetUserData": [other, curve_metadata]})
            region.apply({"AssetUserData": [other], "PreviewForwardAxis": "Y"}, None, None, None, None,
                         {"curve_metadata_index": 1})
            self.assertEqual(steps, [("write", {"PreviewForwardAxis": "Y"})])
            del steps[:]
            region.apply({"AssetUserData": [other]}, None, None, None, None, {"curve_metadata_index": 1})
            self.assertEqual(steps, [])
            region.adapter = Writer({})
            # No curve metadata object in AssetUserData: one plain write.
            del steps[:]
            region.apply({"AssetUserData": [other]}, None, None, None, None, {"curve_metadata_index": -1})
            self.assertEqual(steps, [("write", {"AssetUserData": [other]})])
        finally:
            skeleton.edit = original

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

    def test_axis_analysis_reads_and_plans_as_the_function_picker_and_settings_rows(self):
        names = blend_space.BLEND_SPACE_CLASS
        live = {**self.LIVE, "axes": [dict(self.LIVE["axes"][0], analysis={"function": "Velocity", "properties": {
            "FunctionAxis": "PlusY", "StartTimeFraction": 0.0}}), dict(self.LIVE["axes"][1], analysis=None)]}
        body = blend_space.body_from_live(live, names)
        self.assertEqual(body["Axes"]["X"]["Analysis"], {"Function": "Velocity", "Properties": {
            "FunctionAxis": "PlusY", "StartTimeFraction": 0.0}})
        self.assertNotIn("Analysis", body["Axes"]["Y"])
        # The extract plans nothing.
        self.assertEqual(blend_space.plan(blend_space.canonical_body(body, names), live, names), [])
        # A changed setting writes that row; a new function goes through the picker; null removes it.
        edited = blend_space.canonical_body({"Axes": {
            "X": {"Analysis": {"Function": "Velocity", "Properties": {"FunctionAxis": "PlusX"}}},
            "Y": {"Analysis": {"Function": "Orientation"}}}}, names)
        self.assertEqual(_ops(blend_space.plan(edited, live, names)), [
            {"op": "analysis", "axis": 0, "properties": {"FunctionAxis": "PlusX"}},
            {"op": "analysis", "axis": 1, "function": "Orientation"}])
        removed = blend_space.canonical_body({"Axes": {"X": {"Analysis": None}}}, names)
        self.assertEqual(_ops(blend_space.plan(removed, live, names)), [{"op": "analysis", "axis": 0, "function": None}])
        self.assertEqual(blend_space.map_path("/ops/0/properties/FunctionAxis", ["/Body/Axes/X/Analysis"]),
                         "/Body/Axes/X/Analysis/Properties/FunctionAxis")
        self.assertRefused(lambda: blend_space.canonical_body({"Axes": {"X": {"Analysis": {"Function": ""}}}}, names),
                           "BlendSpace.InvalidAnalysis", "/Body/Axes/X/Analysis/Function")
        diff = blend_space.diff(_document(names, {"Axes": {"X": {"Analysis": None}}}), _document(names, body), False)
        self.assertEqual([item.path for item in diff], ["/Body/Axes/X/Analysis"])

    def test_primitive_paths_map_to_document_paths(self):
        paths = ["/Body/Samples/2", "/Body/Axes/Y", "/Body/PreviewMesh"]
        self.assertEqual(blend_space.map_path("/ops/0/animation", paths), "/Body/Samples/2/Animation")
        self.assertEqual(blend_space.map_path("/ops/1/max", paths), "/Body/Axes/Y/Max")
        self.assertEqual(blend_space.map_path("/ops/2/path", paths), "/Body/PreviewMesh")

    def test_create_requires_the_skeleton(self):
        self.assertRefused(lambda: blend_space.create_target_context(_document(blend_space.BLEND_SPACE_CLASS, {}), None),
                           "BlendSpace.SkeletonRequired", "/Body/Skeleton")


class PoseAssetTests(AnimationAssetTestCase):
    LIVE = {"poses": ["Pose_0", "Pose_1", "Pose_2"], "additive": False, "base_pose": "", "skeleton": SKELETON,
            "source_animation": WALK, "curves": ["Smile", "Blink"]}
    UNSOURCED = {**LIVE, "source_animation": ""}
    RECORDS = [{"pose": 0, "curve": "Smile", "value": 1.0}, {"pose": 2, "curve": "Blink", "value": 0.5},
               {"pose": 2, "bone": "jaw", "rotation": {"Pitch": 0.0, "Yaw": 0.0, "Roll": -12.0}},
               {"pose": 2, "bone": "head", "location": {"X": 1.0, "Y": 0.0, "Z": 0.0}, "scale": {"X": 2.0, "Y": 2.0, "Z": 2.0}}]

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

    def _extracted(self):
        live = pose_asset.body_from_live(self.LIVE, pose_asset.pose_data(self.LIVE["poses"], self.RECORDS))
        return pose_asset.canonical_body(live)

    def test_pose_data_is_extracted_per_pose(self):
        body = self._extracted()
        self.assertEqual(body["Poses"][0], {"Name": "Pose_0", "Curves": {"Smile": 1.0}})
        self.assertEqual(body["Poses"][1], {"Name": "Pose_1"})
        self.assertEqual(body["Poses"][2]["Bones"], {
            "jaw": {"Rotation": {"Pitch": 0.0, "Yaw": 0.0, "Roll": -12.0}},
            "head": {"Location": {"X": 1.0, "Y": 0.0, "Z": 0.0}, "Scale": {"X": 2.0, "Y": 2.0, "Z": 2.0}}})
        self.assertEqual(body["Curves"], ["Smile", "Blink"])
        # A pose without data is written as its name.
        self.assertEqual(pose_asset.body_from_live(self.LIVE, pose_asset.pose_data(self.LIVE["poses"], self.RECORDS))
                         ["Poses"][1], "Pose_1")

    def test_pose_data_is_read_only(self):
        body = self._extracted()
        data = pose_asset.pose_data(self.LIVE["poses"], self.RECORDS)
        pose_asset.check_pose_data(body, self.LIVE, data)
        changed = pose_asset.canonical_body({"Poses": [{"Name": "Pose_0", "Curves": {"Smile": 0.25}}, "Pose_1", "Pose_2"]})
        self.assertRefused(lambda: pose_asset.check_pose_data(changed, self.LIVE, data), "PoseAsset.PoseDataReadOnly",
                           "/Body/Poses/0/Curves")
        bones = pose_asset.canonical_body({"Poses": ["Pose_0", "Pose_1", {"Name": "Pose_2", "Bones": {}}]})
        self.assertRefused(lambda: pose_asset.check_pose_data(bones, self.LIVE, data), "PoseAsset.PoseDataReadOnly",
                           "/Body/Poses/2/Bones")
        # A curve the document deletes is not compared.
        deleting = pose_asset.canonical_body({"Poses": ["Pose_0", "Pose_1", {"Name": "Pose_2", "Curves": {}}],
                                              "Curves": ["Smile"]})
        pose_asset.check_pose_data(deleting, self.LIVE, data)
        new = pose_asset.canonical_body({"Poses": ["Pose_0", "Pose_1", "Pose_2", {"Name": "New", "Curves": {}}]})
        self.assertRefused(lambda: pose_asset.check_pose_data(new, self.UNSOURCED, data), "PoseAsset.PoseDataReadOnly",
                           "/Body/Poses/3/Curves")
        self.assertRefused(lambda: pose_asset.canonical_body({"Poses": [{"Name": "A", "Bones": {"jaw": {"Location": {"X": 1}}}}]}),
                           "PoseAsset.InvalidPoses", "/Body/Poses/0/Bones/jaw/Location")

    def test_pose_data_diff(self):
        extracted = _document(pose_asset.CLASS, self._extracted())
        handler = pose_asset.create_pose_asset_runtime_handler()
        self.assertEqual(list(handler.diff(extracted, extracted)), [])
        authored = _document(pose_asset.CLASS, {"Poses": [{"Name": "Pose_0", "Curves": {"Smile": 0.5}}, "Pose_1", "Pose_2"]})
        self.assertEqual([change.path for change in handler.diff(authored, extracted)], ["/Body/Poses/0/Curves"])
        fewer = _document(pose_asset.CLASS, {"Curves": ["Smile"]})
        self.assertEqual([change.path for change in handler.diff(fewer, extracted)], ["/Body/Curves"])

    def test_curves_are_deleted_not_added(self):
        self.assertEqual(_ops(pose_asset.curve_ops(["Blink"], self.LIVE)), [{"op": "remove_curves", "names": ["Smile"]}])
        # A new source re-reads the curves: a Curves list of the old source is stale, not a removal list.
        source = "/Game/Anims/Face_Other.Face_Other"
        self.assertRefused(lambda: pose_asset.check_source_change(pose_asset.canonical_body(
            {"SourceAnimation": source, "Curves": ["Blink", "Smile"]})), "PoseAsset.PoseDataReadOnly", "/Body/Curves")
        pose_asset.check_source_change(pose_asset.canonical_body({"SourceAnimation": source}))
        pose_asset.check_source_change(pose_asset.canonical_body({"SourceAnimation": "", "Curves": ["Blink"]}))
        self.assertEqual(pose_asset.curve_ops(["Blink", "Smile"], self.LIVE), [])
        self.assertRefused(lambda: pose_asset.curve_ops(["Smile", "Frown"], self.LIVE), "PoseAsset.CurveAddUnsupported",
                           "/Body/Curves/1")
        self.assertRefused(lambda: pose_asset.canonical_body({"Curves": ["A", "A"]}), "PoseAsset.InvalidCurves",
                           "/Body/Curves/1")

    def test_poses_are_added_from_the_reference_pose_without_a_source(self):
        ops = pose_asset.pose_ops(self._poses(["Pose_0", {"Name": "Wide", "RenamedFrom": "Pose_2"}, "New", "Other"]),
                                  self.UNSOURCED)
        self.assertEqual(_ops(ops), [{"op": "remove", "names": ["Pose_1"]}, {"op": "rename", "name": "Pose_2", "to": "Wide"},
                                     {"op": "add_reference_pose", "name": "New"},
                                     {"op": "add_reference_pose", "name": "Other"}])
        self.assertRefused(lambda: pose_asset.pose_ops(self._poses(["New", "Pose_0"]), self.UNSOURCED),
                           "PoseAsset.PoseOrderUnsupported", "/Body/Poses")


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
        # The segment the track holds is written from itself, with only the changed field.
        self.assertEqual(composite.segments_op(body["Segments"], live), {"op": "segments", "segments": [
            {"keep": 0, "loops": 3}, {"animation": WALK}]})

    def test_held_segments_with_a_missing_animation_or_legacy_values_are_kept(self):
        live = [{"Animation": None, "StartTime": 0, "EndTime": 0.5, "PlayRate": 1, "LoopCount": 1},
                {"Animation": IDLE, "StartTime": 0, "EndTime": 1, "PlayRate": 1, "LoopCount": 0}]
        document = _document(composite.CLASS, {"Segments": [dict(row) for row in live]})
        with self.assertRaises(AssetDocumentContractError):
            composite.canonicalize(document)
        with tolerance.authoring() as findings:
            composite.canonicalize(document)
        self.assertEqual([(item.path, item.code) for item in findings],
                         [("/Body/Segments/0/Animation", "AnimComposite.MissingReference"),
                          ("/Body/Segments/1/LoopCount", "AnimComposite.InvalidSegment")])
        tolerance.settle(findings, document, document)
        edited = [dict(live[0]), dict(live[1], PlayRate=2)]
        self.assertEqual(composite.segments_op(edited, live)["segments"], [{"keep": 0}, {"keep": 1, "rate": 2}])
        self.assertEqual(composite.map_path("/ops/0/segments/1/rate", ["/Body/Segments"]), "/Body/Segments/1/PlayRate")

    TIMELINE = {"curves": [], "notify_tracks": ["1", "Feet"],
                "notifies": [{"time": 0.25, "track": "Feet", "class": None, "name": "Footstep",
                              "trigger": {"TriggerWeightThreshold": 0.00001, "NotifyTriggerChance": 1.0,
                                          "NotifyFilterType": "NoFiltering", "NotifyFilterLOD": 0,
                                          "bCanBeFilteredViaRequest": True, "bTriggerOnDedicatedServer": True,
                                          "bTriggerOnFollower": False, "MontageTickType": "Queued"},
                              "guid": "0123456789ABCDEF0123456789ABCDEF"}],
                "notify_states": []}

    def test_timeline_regions_read_and_write_through_the_sequence_authoring_intent(self):
        live = {"skeleton": SKELETON, "preview_mesh": "", "segments": [], "timeline": self.TIMELINE}
        body = composite.body_from_live(live)
        self.assertEqual(body["NotifyTracks"], [{"Name": "1"}, {"Name": "Feet"}])
        self.assertEqual(body["Notifies"], [{"Time": 0.25, "Track": "Feet", "Name": "Footstep",
                                             "Guid": "0123456789ABCDEF0123456789ABCDEF"}])
        self.assertEqual(body["Curves"], [])
        # The extracted timeline matches itself: no op.
        canonical = composite.canonical_body(body)
        self.assertTrue(composite.timeline_matches({key: canonical[key] for key in composite.TIMELINE_KEYS},
                                                   composite.live_timeline(live)))
        edited = composite.canonical_body({"Notifies": [{"Time": 0.5, "Track": "Feet", "Name": "Footstep"}]})
        self.assertFalse(composite.timeline_matches({"Notifies": edited["Notifies"]}, composite.live_timeline(live)))
        op = composite.timeline_op({"Notifies": edited["Notifies"]}, live)
        self.assertEqual(op, {"op": "timeline", "intent": {"notifies": [
            {"time": 0.5, "track": "Feet", "class": None, "name": "Footstep",
             "reuse": "0123456789ABCDEF0123456789ABCDEF"}]}})
        self.assertEqual(composite.map_path("/ops/1/intent/notifies/0/time", ["/Body/Segments", "/Body"]),
                         "/Body/Notifies/0/Time")
        # The sequence rules apply: a row names a listed track.
        self.assertRefused(lambda: composite.canonical_body({"NotifyTracks": [{"Name": "1"}], "Notifies": [
            {"Time": 0.1, "Track": "Hands", "Name": "Clap"}]}), "AnimSequence.UnknownNotifyTrack", "/Body/Notifies/0/Track")
        # An older read without the timeline extracts no timeline regions.
        self.assertNotIn("Notifies", composite.body_from_live({"skeleton": SKELETON, "segments": []}))


if __name__ == "__main__":
    unittest.main()
