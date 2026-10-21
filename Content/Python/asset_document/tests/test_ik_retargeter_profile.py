"""IK Retargeter Profile: Body from the read records, Diff, validation and the apply planner (phases, op stack order,
poses, error paths). The editor side (edit primitive, suggestions, fidelity on the corpus) is covered by
UEAssetDoc.IKRig.*."""
from __future__ import annotations

import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.profiles import ik_retargeter as rtg
from asset_document.profiles import ik_retargeter_apply as plan

SOURCE = "/Game/Rigs/IK_Source.IK_Source"
TARGET = "/Game/Rigs/IK_Target.IK_Target"
MESH = "/Game/Meshes/SKM_Target.SKM_Target"


def _records():
    return [
        {"kind": "header", "source": {"ik_rig": SOURCE, "preview_mesh": "/Game/M/S.S", "rig_mesh": "/Game/M/S.S"},
         "target": {"ik_rig": TARGET, "preview_mesh": MESH, "rig_mesh": "/Game/M/T.T"}},
        {"kind": "op", "index": 0, "entry": {"Name": "Pelvis Motion", "Type": "PelvisMotion"}},
        {"kind": "op", "index": 1, "entry": {"Name": "FK Chains", "Type": "FKChains",
                                             "ChainMapping": {"Spine": "Spine", "Head": None}}},
        {"kind": "op_chains", "index": 1, "chains": {"Spine": {"RotationAlpha": 0.5}}},
        {"kind": "op", "index": 2, "entry": {"Name": "IK Chains", "Type": "IKChains", "Parent": "Run IK Rig"}},
        {"kind": "op", "index": 3, "entry": {"Name": "Run IK Rig", "Type": "RunIKRig", "ChainMapping": {"LeftLeg": "LeftLeg"}}},
        {"kind": "op", "index": 4, "entry": {"Name": "Root Motion", "Type": "RootMotion", "Enabled": False}},
        {"kind": "pose", "side": "Source", "name": "Default Pose", "current": True},
        {"kind": "pose", "side": "Target", "name": "Default Pose", "current": False},
        {"kind": "pose", "side": "Target", "name": "A-Pose", "current": True, "root_offset": 2.5},
        {"kind": "pose_bones", "side": "Target", "name": "A-Pose", "bones": {"upperarm_l": {"X": 0, "Y": 0, "Z": -0.34, "W": 0.94}}},
    ]


def _live():
    body, facts = rtg.assemble(_records())
    return plan.Live(body, facts["sides"])


def _ops(phase, body, live=None):
    return [item.op for item in phase(body, _live() if live is None else live)]


def _body(**changes):
    body = _live().body
    body = {key: value for key, value in body.items()}
    body.update(changes)
    return body


def _code(call):
    try:
        call()
    except AssetDocumentContractError as caught:
        return caught.diagnostic.code, caught.diagnostic.path
    return None


def _clean(stack):
    """UIKRetargeter::CleanOpStack ordering: per top-level op in array order, its children (array order), then it."""
    tops = [name for name, parent in stack if not parent]
    out = []
    for top in tops:
        out += [item for item in stack if item[1] == top]
        out.append(next(item for item in stack if item[0] == top))
    return out


def _simulate(stack, name, to):
    """UIKRetargeterController::MoveRetargetOpInStack."""
    stack = list(stack)
    index = [item[0] for item in stack].index(name)
    moved = stack.pop(index)
    stack.insert(to, moved)
    return _clean(stack)


def _as_body(stack):
    ops, children = [], {}
    for name, parent in stack:
        if parent:
            children.setdefault(parent, []).append({"Name": name, "Type": "IKChains"})
    for name, parent in stack:
        if not parent:
            entry = {"Name": name, "Type": "X"}
            if name in children:
                entry["Children"] = children[name]
            ops.append(entry)
    return {"Ops": ops}


class AssembleTests(unittest.TestCase):
    def test_body(self):
        body = _live().body
        self.assertEqual(body["Source"], {"IKRig": SOURCE})
        self.assertEqual(body["Target"], {"IKRig": TARGET, "PreviewMesh": MESH})
        self.assertEqual([op["Name"] for op in body["Ops"]], ["Pelvis Motion", "FK Chains", "Run IK Rig", "Root Motion"])
        self.assertEqual(body["Ops"][1]["Chains"], {"Spine": {"RotationAlpha": 0.5}})
        self.assertEqual(body["Ops"][2]["Children"], [{"Name": "IK Chains", "Type": "IKChains"}])
        self.assertEqual(body["Poses"]["Target"]["Current"], "A-Pose")
        self.assertEqual(body["Poses"]["Target"]["Poses"]["A-Pose"]["RootOffset"], 2.5)

    def test_cleared_preview_mesh_is_null(self):
        side = rtg._side({"ik_rig": TARGET, "preview_mesh": "", "rig_mesh": MESH})
        self.assertEqual(side, {"IKRig": TARGET, "PreviewMesh": None})


class DiffTests(unittest.TestCase):
    def test_extract_has_no_differences(self):
        self.assertEqual(rtg.body_diff(_live().body, _live().body, exact=False), [])

    def test_omitted_chain_mapping_and_preview_mesh_are_kept(self):
        body = _body()
        del body["Ops"][1]["ChainMapping"]
        body["Source"] = {"IKRig": SOURCE}
        self.assertEqual(rtg.body_diff(body, _live().body, exact=False), [])

    def test_omitted_preview_mesh_is_the_rig_mesh(self):
        body = _body(Target={"IKRig": TARGET})
        self.assertEqual([item.path for item in rtg.body_diff(body, _live().body, exact=False)],
                         ["/Body/Target/PreviewMesh"])


    def test_pose_rotations_compare_as_stored(self):
        state = _live()
        live = state.body
        stored = {"X": 0.3826834323650898, "Y": 0.0, "Z": 0.0, "W": 0.9238795325112867}   # what the engine stored
        live["Poses"]["Target"]["Poses"]["A-Pose"]["Bones"]["upperarm_r"] = dict(stored)
        body = _body()
        bones = body["Poses"]["Target"]["Poses"]["A-Pose"]["Bones"]
        bones["upperarm_r"] = {"Roll": -45}                       # rotator, converted as FRotator::Quaternion
        self.assertEqual(rtg.body_diff(body, live, exact=True), [])
        bones["upperarm_r"] = {key: value * 1.01 for key, value in stored.items()}  # normalized on write
        self.assertEqual(rtg.body_diff(body, live, exact=True), [])
        bones["upperarm_r"] = {"Roll": -40}
        self.assertEqual({item.path for item in rtg.body_diff(body, live, exact=True)},
                         {f"/Body/Poses/Target/Poses/A-Pose/Bones/upperarm_r/{key}" for key in "XW"})
        # The live (not normalized) rotation re-applied as it is equals itself; an identity rotation is not listed.
        bones["upperarm_r"] = dict(stored)
        bones["hand_r"] = {"Pitch": 0}
        self.assertEqual(rtg.body_diff(body, live, exact=True), [])
        self.assertEqual(_ops(plan.phase_pose_content, body, state), [])

    def test_negative_zero_reads_as_zero(self):
        records = _records()
        records[-1]["bones"]["spine_01"] = {"X": -0.0, "Y": 0.5, "Z": -0.0, "W": 0.8660254037844386}
        bones = rtg.assemble(records)[0]["Poses"]["Target"]["Poses"]["A-Pose"]["Bones"]
        self.assertEqual(str(bones["spine_01"]["X"]), "0.0")


    def test_values_at_their_defaults_verify_against_the_live_defaults(self):
        # The Root Motion op's bones written as the values a new op fills in; the sparse read omits them.
        records = _records()
        records[6] = dict(records[6], settings_defaults={"SourceRoot": {"BoneName": "root"}, "TargetPelvis":
                                                         {"BoneName": "pelvis"}, "bPropagateToNonRetargetedChildren": True})
        records[2] = dict(records[2], settings_defaults={}, chain_defaults={"RotationAlpha": 1.0, "TranslationAlpha": 1.0})
        live, facts = rtg.assemble(records)
        body = _body()
        body["Ops"][3] = dict(body["Ops"][3], Settings={"SourceRoot": {"BoneName": "root"},
                                                        "bPropagateToNonRetargetedChildren": True})
        body["Ops"][1] = dict(body["Ops"][1], Chains={"Spine": {"RotationAlpha": 0.5}, "Head": {"TranslationAlpha": 1.0}})
        without = [item.path for item in rtg.body_diff(body, live, exact=False)]
        self.assertIn("/Body/Ops/3/Settings/SourceRoot", without)       # what failed verification before
        self.assertEqual(rtg.body_diff(body, live, exact=False, live_defaults=facts["defaults"]), [])
        # Names are case-insensitive: "ROOT" is the bone the name table spells "root".
        body["Ops"][3]["Settings"] = {"SourceRoot": {"BoneName": "ROOT"}}
        self.assertEqual(rtg.body_diff(body, live, exact=False, live_defaults=facts["defaults"]), [])
        # A value that is not the default still differs, with the value read back (the default).
        body["Ops"][3]["Settings"] = {"SourceRoot": {"BoneName": "spine_01"}}
        changes = rtg.body_diff(body, live, exact=False, live_defaults=facts["defaults"])
        self.assertEqual([(item.path, item.before, item.after) for item in changes],
                         [("/Body/Ops/3/Settings/SourceRoot/BoneName", "spine_01", "root")])
        # A live value away from its default with the field omitted is a difference too.
        live["Ops"][3]["Settings"] = {"bPropagateToNonRetargetedChildren": False}
        body["Ops"][3]["Settings"] = {}
        self.assertEqual([item.path for item in rtg.body_diff(body, live, exact=False, live_defaults=facts["defaults"])],
                         ["/Body/Ops/3/Settings/bPropagateToNonRetargetedChildren"])


class ValidateTests(unittest.TestCase):
    def test_duplicate_op_names(self):
        body = {"Ops": [{"Name": "FK", "Type": "FKChains"}, {"Name": "fk", "Type": "FKChains"}]}
        self.assertEqual(_code(lambda: rtg.validate_body(body)), ("IKRetargeter.DuplicateName", "/Body/Ops/1/Name"))

    def test_default_pose_is_required(self):
        body = {"Poses": {"Target": {"Poses": {"A": {}}}}}
        self.assertEqual(_code(lambda: rtg.validate_body(body)),
                         ("IKRetargeter.DefaultPoseRequired", "/Body/Poses/Target/Poses"))

    def test_current_names_a_pose(self):
        body = {"Poses": {"Target": {"Current": "B", "Poses": {"Default Pose": {}}}}}
        self.assertEqual(_code(lambda: rtg.validate_body(body)),
                         ("IKRetargeter.PoseNotFound", "/Body/Poses/Target/Current"))

    def test_bone_rotation_shape(self):
        body = {"Poses": {"Target": {"Poses": {"Default Pose": {"Bones": {"a": {"X": 1}}}}}}}
        self.assertEqual(_code(lambda: rtg.validate_body(body)),
                         ("IKRetargeter.InvalidBody", "/Body/Poses/Target/Poses/Default Pose/Bones/a"))
        rtg.validate_body({"Poses": {"Target": {"Poses": {"Default Pose": {"Bones": {"a": {"Yaw": 10}}}}}}})

    def test_grandchildren_are_refused(self):
        body = {"Ops": [{"Name": "A", "Type": "RunIKRig", "Children": [{"Name": "B", "Type": "IKChains",
                                                                        "Children": []}]}]}
        self.assertEqual(_code(lambda: rtg.validate_body(body)),
                         ("IKRetargeter.InvalidParentOp", "/Body/Ops/0/Children/0/Children"))


class PlanTests(unittest.TestCase):
    def test_unchanged_body_plans_nothing(self):
        body, live = _live().body, _live()
        for phase in plan.PHASES + plan.OP_CONTENT_PHASES + plan.POSE_PHASES:
            self.assertEqual(_ops(phase, body, live), [], phase.__name__)
        self.assertIsNone(plan.plan_move(body, live))
        self.assertEqual([item.op for item in plan.phase_pose_current(body, live, {})], [])

    def test_sides(self):
        body = _body(Source={"IKRig": "/Game/Rigs/IK_Other.IK_Other"}, Target={"IKRig": TARGET, "PreviewMesh": None})
        self.assertEqual(_ops(plan.phase_rigs, body), [{"op": "rig.set", "side": "Source", "rig": "/Game/Rigs/IK_Other.IK_Other"}])
        # Source: no PreviewMesh = the rig's mesh (already stored); Target: null clears it.
        self.assertEqual(_ops(plan.phase_meshes, body), [{"op": "preview_mesh", "side": "Target", "mesh": ""}])

    def test_op_structure(self):
        body = _body(Ops=[
            {"Name": "Pelvis", "Type": "PelvisMotion", "RenamedFrom": "Pelvis Motion"},
            {"Name": "FK Chains", "Type": "PinBone"},
            {"Name": "Run IK Rig", "Type": "RunIKRig", "Children": [{"Name": "Floor", "Type": "FloorConstraint"}]},
            {"Name": "Curves", "Type": "CurveRemap"}])
        self.assertEqual(_ops(plan.phase_op_renames, body), [{"op": "op.rename", "name": "Pelvis Motion", "to": "Pelvis"}])
        # FK Chains changes Type (replaced), IK Chains and Root Motion are dropped (Pelvis Motion only because this
        # read is from before the rename).
        self.assertEqual(_ops(plan.phase_op_removals, body), [
            {"op": "op.remove", "name": "Pelvis Motion"}, {"op": "op.remove", "name": "FK Chains"},
            {"op": "op.remove", "name": "IK Chains"}, {"op": "op.remove", "name": "Root Motion"}])
        # Planned against the read before the renames (each phase runs on a fresh read in the apply).
        self.assertEqual(_ops(plan.phase_op_adds, body)[1], {"op": "op.add", "type": "FloorConstraint", "name": "Floor",
                                                             "parent": "Run IK Rig"})
        names = [op["name"] for op in _ops(plan.phase_op_adds, body)]
        self.assertEqual(names, ["Pelvis", "Floor", "Curves"])

    def test_removing_a_parent_removes_its_children(self):
        body = _body(Ops=[op for op in _live().body["Ops"] if op["Name"] != "Run IK Rig"])
        self.assertEqual(_ops(plan.phase_op_removals, body), [{"op": "op.remove", "name": "Run IK Rig"}])

    def test_stack_order_converges(self):
        stack = [("Pelvis Motion", ""), ("FK Chains", ""), ("IK Chains", "Run IK Rig"), ("Stride", "Run IK Rig"),
                 ("Run IK Rig", ""), ("Root Motion", "")]
        wanted = [("Root Motion", ""), ("Stride", "Run IK Rig"), ("IK Chains", "Run IK Rig"), ("Run IK Rig", ""),
                  ("Pelvis Motion", ""), ("FK Chains", "")]
        body = _as_body(wanted)
        moves = 0
        while True:
            move = plan.plan_move(body, plan.Live(_as_body(stack), {}))
            if move is None:
                break
            stack = _simulate(stack, *move)
            moves += 1
            self.assertLess(moves, 10)
        self.assertEqual(stack, wanted)

    def test_op_content(self):
        body = _body()
        body["Ops"] = [dict(op) for op in body["Ops"]]
        body["Ops"][1] = {**body["Ops"][1], "Settings": {"Enabled": True}, "ChainMapping": {"Spine": None, "Head": "Head"},
                          "Chains": {"Head": {"RotationAlpha": 0.25}}, "TargetIKRig": "/Game/Rigs/IK_B.IK_B"}
        body["Ops"][3] = {**body["Ops"][3], "Enabled": True}
        self.assertEqual(_ops(plan.phase_op_enable, body), [{"op": "op.enable", "name": "Root Motion", "enabled": True}])
        self.assertEqual(_ops(plan.phase_op_target_rigs, body),
                         [{"op": "op.target_rig", "name": "FK Chains", "rig": "/Game/Rigs/IK_B.IK_B"}])
        self.assertEqual(_ops(plan.phase_op_settings, body),
                         [{"op": "op.settings", "name": "FK Chains", "settings": {"Enabled": True}}])
        self.assertEqual(_ops(plan.phase_op_chains, body), [
            {"op": "op.map", "name": "FK Chains", "mapping": {"Spine": None, "Head": "Head"}},
            {"op": "op.chain_settings", "name": "FK Chains", "chain": "Head", "settings": {"RotationAlpha": 0.25}},
            {"op": "op.chain_settings", "name": "FK Chains", "chain": "Spine", "settings": {}}])

    def test_poses(self):
        body = _body(Poses={"Target": {"Current": "T", "Poses": {
            "Default Pose": {}, "T": {"RenamedFrom": "A-Pose", "RootOffset": 2.5,
                                      "Bones": {"upperarm_l": {"X": 0, "Y": 0, "Z": -0.34, "W": 0.94}}},
            "Crouch": {"RootOffset": -10}}}})
        self.assertEqual(_ops(plan.phase_pose_structure, body), [
            {"op": "pose.rename", "side": "Target", "pose": "A-Pose", "to": "T"},
            {"op": "pose.add", "side": "Target", "pose": "Crouch"}])
        live = _live()
        live.body["Poses"]["Target"]["Poses"]["Crouch"] = {}
        self.assertEqual(_ops(plan.phase_pose_content, body, live), [
            {"op": "pose.set", "side": "Target", "pose": "Crouch", "root": -10, "bones": {}}])
        live.body["Poses"]["Target"]["Poses"]["T"] = {}
        self.assertEqual([item.op for item in plan.phase_pose_current(body, live, {})],
                         [{"op": "pose.current", "side": "Target", "pose": "T"}])

    def test_dropped_poses_are_removed_but_not_the_default(self):
        body = _body(Poses={"Target": {"Poses": {"Default Pose": {}}}})
        self.assertEqual(_ops(plan.phase_pose_structure, body), [{"op": "pose.remove", "side": "Target", "pose": "A-Pose"}])
        # The current pose comes back to the one current before the apply (or Default Pose when it was removed).
        live = _live()
        del live.body["Poses"]["Target"]["Poses"]["A-Pose"]
        live.body["Poses"]["Target"]["Current"] = "Default Pose"
        self.assertEqual([item.op for item in plan.phase_pose_current(body, live, {"Target": "A-Pose"})], [])


class OutcomeTests(unittest.TestCase):
    def test_new_errors_fail(self):
        after = {"Status": "Error", "Messages": [{"Severity": "Error", "Message": "bad", "Path": "/Body/Ops/1"}]}
        failure, _ = plan.validation_outcome({}, after)
        self.assertEqual((failure.diagnostic.code, failure.diagnostic.path), ("IKRetargeter.ValidationFailed", "/Body/Ops/1"))
        failure, warnings = plan.validation_outcome(after, after)
        self.assertIsNone(failure)
        self.assertEqual([item.code for item in warnings], ["IKRetargeter.ValidationError"])

    def test_op_paths(self):
        ops = [plan.Op({"op": "op.add"}, "/Body/Ops/2", {"type": "/Body/Ops/2/Type"}),
               plan.Op({"op": "op.settings"}, "/Body/Ops/1/Settings", nested="/Body/Ops/1/Settings"),
               plan.Op({"op": "pose.set"}, "/Body/Poses/Target/Poses/A", nested="/Body/Poses/Target/Poses/A/Bones")]
        self.assertEqual(plan.op_path("ops/0/type", ops), "/Body/Ops/2/Type")
        self.assertEqual(plan.op_path("/ops/1/settings/RotationAlpha", ops), "/Body/Ops/1/Settings/RotationAlpha")
        self.assertEqual(plan.op_path("ops/2/bones/hand_l", ops), "/Body/Poses/Target/Poses/A/Bones/hand_l")
        self.assertEqual(plan.op_path("ops/9", ops), "/Body")


if __name__ == "__main__":
    unittest.main()
