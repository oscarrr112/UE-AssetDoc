"""IK Rig / IK Retargeter Profiles: assembling the Body from the rig.* read records, Diff, and the IK Rig apply
planner (phases, solver stack alignment, refusals, validation outcome, error paths). The editor side (read, edit
primitive, status, fidelity on the corpus) is covered by UEAssetDoc.IKRig.*."""
from __future__ import annotations

import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.profiles import ik_retargeter, ik_rig
from asset_document.profiles import ik_rig_apply as plan

TARGET = "/Game/Rigs/IK_Test"
MESH = "/Game/Meshes/SKM_Test.SKM_Test"


def _records():
    return [
        {"kind": "header", "preview_mesh": MESH, "excluded_bones": ["ik_root"], "retarget_root": "pelvis"},
        {"kind": "goal", "name": "LeftFoot", "entry": {"Bone": "foot_l"}},
        {"kind": "goal", "name": "RightFoot", "entry": {"Bone": "foot_r", "RotationAlpha": 0.5,
                                                        "Offset": {"Location": {"X": 0, "Y": 0, "Z": 5}}}},
        {"kind": "solver", "index": 0, "entry": {"Type": "FullBodyIK", "StartBone": "pelvis",
                                                 "Goals": {"LeftFoot": {}, "RightFoot": {"PullChainAlpha": 0.5}}}},
        {"kind": "solver_bones", "index": 0, "bones": {"calf_l": {"X": "Limited"}}},
        {"kind": "solver_bones", "index": 0, "bones": {"calf_r": {}}},
        {"kind": "solver", "index": 1, "entry": {"Type": "LimbIK", "Enabled": False}},
        {"kind": "chain", "name": "LeftLeg", "entry": {"Start": "thigh_l", "End": "foot_l", "Goal": "LeftFoot"}},
        {"kind": "chain", "name": "Spine", "entry": {"Start": "spine_01", "End": "spine_03"}},
    ]


def _live():
    body, _ = ik_rig.assemble(_records())
    return body


def _ops(phase, body, live=None):
    return [item.op for item in phase(body, _live() if live is None else live)]


class AssembleTests(unittest.TestCase):
    def test_rig_body_from_records(self):
        body = _live()
        self.assertEqual(body["PreviewMesh"], MESH)
        self.assertEqual(body["ExcludedBones"], ["ik_root"])
        self.assertEqual(list(body["Goals"]), ["LeftFoot", "RightFoot"])
        self.assertEqual(body["Solvers"][0]["Bones"], {"calf_l": {"X": "Limited"}, "calf_r": {}})
        self.assertEqual(body["RetargetRoot"], "pelvis")
        self.assertEqual(body["Chains"]["Spine"], {"Start": "spine_01", "End": "spine_03"})

    def test_empty_regions_are_omitted(self):
        body, _ = ik_rig.assemble([{"kind": "header", "preview_mesh": "", "excluded_bones": [], "retarget_root": ""}])
        self.assertEqual(body, {})

    def test_retargeter_child_ops_nest_under_their_parent(self):
        # The op stack keeps a child before its parent (UIKRetargeterController::AddRetargetOp).
        records = [{"kind": "header", "source": {"ik_rig": None}, "target": {"ik_rig": None}},
                   {"kind": "op", "index": 0, "entry": {"Name": "Pelvis Motion", "Type": "PelvisMotion"}},
                   {"kind": "op", "index": 1, "entry": {"Name": "IK Goals", "Type": "IKChains", "Parent": "Run IK Rig"}},
                   {"kind": "op", "index": 2, "entry": {"Name": "Run IK Rig", "Type": "RunIKRig"}},
                   {"kind": "op", "index": 3, "entry": {"Name": "Orphan", "Type": "IKChains", "Parent": "Missing"}}]
        body, _ = ik_retargeter.assemble(records)
        self.assertEqual([op["Name"] for op in body["Ops"]], ["Pelvis Motion", "Run IK Rig", "Orphan"])
        self.assertEqual(body["Ops"][1]["Children"], [{"Name": "IK Goals", "Type": "IKChains"}])
        self.assertEqual(body["Ops"][2]["Parent"], "Missing")


class DiffTests(unittest.TestCase):
    def test_extract_has_no_differences(self):
        self.assertEqual(ik_rig.body_diff(_live(), _live(), exact=False), [])

    def test_omitted_region_is_kept_and_defaults_are_equal(self):
        body = {"Goals": {"LeftFoot": {"Bone": "foot_l", "PositionAlpha": 1.0},
                          "RightFoot": {"Bone": "foot_r", "RotationAlpha": 0.5,
                                        "Offset": {"Location": {"X": 0, "Y": 0, "Z": 5}}}}}
        self.assertEqual(ik_rig.body_diff(body, _live(), exact=False), [])

    def test_solver_settings_at_their_defaults_verify_against_the_live_defaults(self):
        records = _records()
        records[3] = dict(records[3], settings_defaults={"Iterations": 20, "RootBehavior": "PinToInput"},
                          goal_defaults={"PullChainAlpha": 1.0, "StrengthAlpha": 1.0},
                          bone_defaults={"X": "Free", "RotationStiffness": 0.0})
        live, facts = ik_rig.assemble(records)
        body = {"Solvers": [dict(_live()["Solvers"][0], Settings={"Iterations": 20},
                                 Goals={"LeftFoot": {"StrengthAlpha": 1.0}, "RightFoot": {"PullChainAlpha": 0.5}},
                                 Bones={"calf_l": {"X": "Limited", "RotationStiffness": 0.0}, "calf_r": {"X": "Free"}}),
                            _live()["Solvers"][1]]}
        self.assertNotEqual(ik_rig.body_diff(body, live, exact=False), [])        # the sparse read omits them
        self.assertEqual(ik_rig.body_diff(body, live, exact=False, live_defaults=facts["defaults"]), [])
        # Connected goals and bones with settings are state: a missing one is still a difference.
        body["Solvers"][0]["Goals"] = {"LeftFoot": {}}
        self.assertEqual([item.path for item in ik_rig.body_diff(body, live, exact=False, live_defaults=facts["defaults"])],
                         ["/Body/Solvers/0/Goals/RightFoot"])

    def test_excluded_bones_compare_as_a_set(self):
        live = dict(_live(), ExcludedBones=["a", "b"])
        self.assertEqual(ik_rig.body_diff({"ExcludedBones": ["b", "a"]}, live, exact=False), [])


class SolverStackTests(unittest.TestCase):
    def test_first_unused_solver_of_the_type_matches(self):
        self.assertEqual(plan.match_solvers(["A", "B", "A"], ["A", "A"]), {0: 0, 2: 1})

    def test_unchanged_stack_plans_nothing(self):
        self.assertEqual(plan.plan_solver_stack(["A", "B"], ["A", "B"]), ([], [], []))

    def test_remove_add_and_move(self):
        removes, adds, moves = plan.plan_solver_stack(["A", "B", "C"], ["D", "C", "A"])
        stack = ["A", "B", "C"]
        for index in removes:
            del stack[index]
        stack += [["D", "C", "A"][j] for j in adds]
        for source, target in moves:
            stack.insert(target, stack.pop(source))
        self.assertEqual(stack, ["D", "C", "A"])
        self.assertEqual((removes, adds), ([1], [0]))

    def test_move_only(self):
        removes, adds, moves = plan.plan_solver_stack(["A", "B"], ["B", "A"])
        self.assertEqual((removes, adds), ([], []))
        self.assertEqual(moves, [(1, 0)])


class PhaseTests(unittest.TestCase):
    def test_goal_rename_add_and_bone_move(self):
        body = {"Goals": {"LeftFootIK": {"Bone": "foot_l", "RenamedFrom": "LeftFoot"},
                          "RightFoot": {"Bone": "ball_r"}, "Hand": {"Bone": "hand_r"}}}
        self.assertEqual(_ops(plan.phase_goals, body), [
            {"op": "goal.rename", "goal": "LeftFoot", "to": "LeftFootIK"},
            {"op": "goal.set_bone", "goal": "RightFoot", "bone": "ball_r"},
            {"op": "goal.add", "goal": "Hand", "bone": "hand_r"}])

    def test_rename_onto_a_dropped_goal_removes_it_first(self):
        body = {"Goals": {"RightFoot": {"Bone": "foot_l", "RenamedFrom": "LeftFoot"}}}
        ops = _ops(plan.phase_goals, body)
        self.assertEqual(ops[:2], [{"op": "goal.remove", "goal": "RightFoot"},
                                   {"op": "goal.rename", "goal": "LeftFoot", "to": "RightFoot"}])

    def test_excluded_bones(self):
        self.assertEqual(_ops(plan.phase_excluded, {"ExcludedBones": ["ik_hand"]}),
                         [{"op": "exclude", "bone": "ik_hand", "excluded": True},
                          {"op": "exclude", "bone": "ik_root", "excluded": False}])

    def test_chains(self):
        body = {"Chains": {"LeftLeg": {"Start": "thigh_l", "End": "ball_l", "Goal": ""},
                           "Arm": {"Start": "upperarm_l", "End": "hand_l"}}}
        self.assertEqual(_ops(plan.phase_chains, body), [
            {"op": "chain.set", "chain": "LeftLeg", "end": "ball_l", "goal": ""},
            {"op": "chain.add", "chain": "Arm", "start": "upperarm_l", "end": "hand_l"}])
        self.assertEqual(_ops(plan.phase_removals, body), [{"op": "chain.remove", "chain": "Spine"}])

    def test_connections_and_settings(self):
        live = _live()
        body = {"Solvers": [{"Type": "FullBodyIK", "StartBone": "pelvis", "Settings": {"Iterations": 30},
                             "Goals": {"RightFoot": {}}, "Bones": {"calf_l": {"X": "Limited"}}},
                            {"Type": "LimbIK", "Enabled": False}]}
        self.assertEqual(_ops(plan.phase_solver_stack, body, live), [])
        self.assertEqual(_ops(plan.phase_connections, body, live),
                         [{"op": "solver.disconnect", "index": 0, "goal": "LeftFoot"}])
        self.assertEqual(_ops(plan.phase_bone_settings, body, live),
                         [{"op": "solver.bone_setting.remove", "index": 0, "bone": "calf_r"}])
        self.assertEqual(_ops(plan.phase_settings, body, live), [
            {"op": "solver.settings", "index": 0, "settings": {"Iterations": 30}},
            {"op": "solver.goal_settings", "index": 0, "goal": "RightFoot", "settings": {}}])

    def test_goal_details_and_offset(self):
        body = {"Goals": {"LeftFoot": {"Bone": "foot_l", "PositionAlpha": 0.25},
                          "RightFoot": {"Bone": "foot_r", "RotationAlpha": 0.5}}}
        self.assertEqual(_ops(plan.phase_goal_details, body), [
            {"op": "goal.set", "goal": "LeftFoot", "fields": {"PositionAlpha": 0.25}}])
        # Offsets are the last phase (every other controller edit re-bases the goals).
        self.assertEqual(_ops(plan.phase_goal_offsets, body), [
            {"op": "goal.offset", "goal": "RightFoot", "offset": {}}])
        self.assertIs(plan.PHASES[-1], plan.phase_goal_offsets)

    def test_clearing_a_start_bone_is_refused(self):
        body = {"Solvers": [{"Type": "FullBodyIK"}, {"Type": "LimbIK", "Enabled": False}]}
        with self.assertRaises(AssetDocumentContractError) as raised:
            plan.phase_solver_bones(body, _live())
        self.assertEqual(raised.exception.diagnostic.code, "IKRig.CannotClear")
        self.assertEqual(raised.exception.diagnostic.path, "/Body/Solvers/0/StartBone")


class RefusalTests(unittest.TestCase):
    def test_bones_need_a_preview_mesh(self):
        with self.assertRaises(AssetDocumentContractError) as raised:
            plan.refuse({"Goals": {"Foot": {"Bone": "foot_l"}}}, {})
        self.assertEqual(raised.exception.diagnostic.code, "IKRig.PreviewMeshRequired")
        plan.refuse({"PreviewMesh": MESH, "Goals": {"Foot": {"Bone": "foot_l"}}}, {})
        plan.refuse({}, {})

    def test_preview_mesh_cannot_be_cleared(self):
        with self.assertRaises(AssetDocumentContractError) as raised:
            plan.refuse({"PreviewMesh": ""}, _live())
        self.assertEqual(raised.exception.diagnostic.code, "IKRig.CannotClear")


class OutcomeTests(unittest.TestCase):
    def test_new_errors_fail(self):
        after = {"Status": "Error", "Messages": [{"Severity": "Error", "Message": "bad", "Path": "/Body/Goals/X"}]}
        failure, warnings = plan.validation_outcome({}, after)
        self.assertEqual((failure.diagnostic.code, failure.diagnostic.path), ("IKRig.ValidationFailed", "/Body/Goals/X"))
        failure, warnings = plan.validation_outcome(after, after)
        self.assertIsNone(failure)
        self.assertEqual([item.code for item in warnings], ["IKRig.ValidationError"])

    def test_dependents(self):
        warnings = plan.dependents_warnings([{"package": "/Game/RTG", "loaded": False,
                                              "chains": [{"chain": "Arm", "to": "LeftArm"}]}])
        self.assertEqual((warnings[0].code, warnings[0].path), ("IKRig.DependentsAffected", "/Body/Chains/LeftArm"))

    def test_op_paths(self):
        ops = [plan.Op({"op": "goal.add"}, "/Body/Goals/A", {"bone": "/Body/Goals/A/Bone"}),
               plan.Op({"op": "solver.settings"}, "/Body/Solvers/0/Settings", settings="/Body/Solvers/0/Settings"),
               plan.Op({"op": "goal.set"}, "/Body/Goals/B", settings="/Body/Goals/B")]
        self.assertEqual(plan.op_path("ops/0/bone", ops), "/Body/Goals/A/Bone")
        self.assertEqual(plan.op_path("/ops/1/settings/Iterations", ops), "/Body/Solvers/0/Settings/Iterations")
        self.assertEqual(plan.op_path("ops/2/fields/bExposePosition", ops), "/Body/Goals/B/ExposePosition")
        self.assertEqual(plan.op_path("ops/9", ops), "/Body")


if __name__ == "__main__":
    unittest.main()
