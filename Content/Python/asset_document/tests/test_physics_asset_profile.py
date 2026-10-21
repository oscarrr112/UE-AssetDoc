"""Physics Asset Profile: assembling the Body from the physics.asset.read.v1 records, canonical form, Diff, and the
apply planner (phases, profile renames, refusals, validation outcome, error paths). The editor side (read, edit
primitive, status, fidelity on the corpus) is covered by UEAssetDoc.PhysicsAsset.*."""
from __future__ import annotations

import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.profiles import physics_asset as physics
from asset_document.profiles import physics_asset_apply as plan

TARGET = "/Game/Physics/PA_Test"
MESH = "/Game/Meshes/SKM_Test.SKM_Test"


DEFAULTS = {"body": {"PhysicsType": "PhysType_Default", "DefaultInstance": {"bOverrideMass": False, "MassInKgOverride": 0.0}},
            "constraint": {"ProfileInstance": {"TwistLimit": {"TwistMotion": "ACM_Free", "TwistLimitDegrees": 45.0}}},
            "physical_animation": {"OrientationStrength": 0.0, "bIsLocalSimulation": True}}


def _records():
    return [
        {"kind": "header", "preview_mesh": MESH, "bodies": 3, "constraints": 2, "constraint_profiles": ["Ragdoll"],
         "physical_animation_profiles": ["HitReact"], "defaults": DEFAULTS},
        {"kind": "body", "bone": "pelvis", "entry": {"Shapes": {"Capsules": [{"Radius": 10, "Length": 20}]},
                                                     "PhysicalAnimation": {"HitReact": {"OrientationStrength": 500}}}},
        {"kind": "body", "bone": "spine_01", "entry": {"Shapes": {"Convexes": [{}]}}},
        {"kind": "convex_vertices", "bone": "spine_01", "index": 0, "vertices": [{"X": 0, "Y": 0, "Z": 0}]},
        {"kind": "convex_vertices", "bone": "spine_01", "index": 0, "vertices": [{"X": 1, "Y": 0, "Z": 0}]},
        {"kind": "body", "bone": "head", "entry": {"Properties": {"PhysicsType": "PhysType_Kinematic"},
                                                   "Shapes": {"Spheres": [{"Radius": 8}]}}},
        {"kind": "constraint", "key": "spine_01", "entry": {"Child": "spine_01", "Parent": "pelvis",
                                                            "Profiles": {"Ragdoll": {"ConeLimit": {"Swing1LimitDegrees": 70}}}}},
        {"kind": "constraint", "key": "head", "entry": {"Child": "head", "Parent": "spine_01"}},
        {"kind": "collision", "pairs": [["pelvis", "head"]]},
        {"kind": "warning", "code": "DuplicateJointName", "message": "m", "path": "/Body/Constraints/head~1"},
    ]


def _live():
    body, facts = physics.assemble(_records())
    return {**body, "_profiles": {"ConstraintProfiles": facts["header"]["constraint_profiles"],
                                  "PhysicalAnimationProfiles": facts["header"]["physical_animation_profiles"]}}


def _body():
    body, _ = physics.assemble(_records())
    return body


def _ops(phase, body, properties=None, live=None):
    return [item.op for item in phase(body, properties or {}, _live() if live is None else live)]


def _document(body, properties=None):
    return {"SchemaVersion": 1, "Class": physics.CLASS, "Action": "Update", "Target": TARGET,
            "Properties": properties or {}, "Body": body}


class AssembleTests(unittest.TestCase):
    def test_body_from_records(self):
        body, facts = physics.assemble(_records())
        self.assertEqual(body["PreviewMesh"], MESH)
        self.assertEqual(list(body["Bodies"]), ["pelvis", "spine_01", "head"])
        self.assertEqual(body["Bodies"]["spine_01"]["Shapes"]["Convexes"][0]["VertexData"],
                         [{"X": 0, "Y": 0, "Z": 0}, {"X": 1, "Y": 0, "Z": 0}])
        self.assertEqual(list(body["Constraints"]), ["spine_01", "head"])
        self.assertEqual(body["CollisionDisabled"], [["pelvis", "head"]])
        self.assertEqual(facts["warnings"][0]["code"], "DuplicateJointName")

    def test_no_preview_mesh(self):
        body, _ = physics.assemble([{"kind": "header", "preview_mesh": None}])
        self.assertEqual(body, {"Bodies": {}, "Constraints": {}, "CollisionDisabled": []})


class CanonicalTests(unittest.TestCase):
    def test_auto_shape_names_are_dropped(self):
        body = {"Bodies": {"head": {"Shapes": {"Spheres": [{"Radius": 8, "Name": "head_sphere"}],
                                               "Boxes": [{"X": 1, "Name": "visor"}]}}}}
        canonical = physics.canonicalize(_document(body))
        shapes = canonical["Body"]["Bodies"]["head"]["Shapes"]
        self.assertEqual(shapes["Spheres"], [{"Radius": 8}])
        self.assertEqual(shapes["Boxes"], [{"X": 1, "Name": "visor"}])

    def _refused(self, body, code, path):
        with self.assertRaises(AssetDocumentContractError) as caught:
            physics.canonicalize(_document(body))
        diagnostic = caught.exception.diagnostic
        self.assertEqual((diagnostic.code, diagnostic.path), (code, path))

    def test_document_shape_errors(self):
        self._refused({"Bodies": {"head": {"Shapes": {"Cylinders": []}}}}, "PhysicsAsset.UnknownKey",
                      "/Body/Bodies/head/Shapes/Cylinders")
        self._refused({"Bodies": {"head": {"Shapes": {"LevelSets": []}}}}, "PhysicsAsset.ShapeTypeNotAuthored",
                      "/Body/Bodies/head/Shapes/LevelSets")
        self._refused({"Bodies": {"head": {"Fit": []}}}, "PhysicsAsset.InvalidBody", "/Body/Bodies/head/Fit")
        self._refused({"Generation": {}, "Bodies": {}}, "PhysicsAsset.GenerationWithBodies", "/Body/Generation")
        self._refused({"Generation": "Box"}, "PhysicsAsset.InvalidBody", "/Body/Generation")
        self._refused({"Constraints": {"a~1": {"Child": "a", "Parent": "b"}}}, "PhysicsAsset.InvalidBody",
                      "/Body/Constraints/a~01")
        self._refused({"Constraints": {"a": {"Child": "a", "Parent": "a"}}}, "PhysicsAsset.InvalidBody",
                      "/Body/Constraints/a/Parent")
        self._refused({"Constraints": {"a": {"Child": "a", "Parent": "b", "Frames": {"Child": {}}}}},
                      "PhysicsAsset.InvalidBody", "/Body/Constraints/a/Frames/Child/Position")
        self._refused({"CollisionDisabled": [["a", "b"], ["b", "a"]]}, "PhysicsAsset.InvalidBody",
                      "/Body/CollisionDisabled/1")


class CreateTests(unittest.TestCase):
    def test_create_only_keys_are_accepted_and_never_compared(self):
        document = _document({"Generation": {"GeomType": "EFG_Box", "MinBoneSize": 10}})
        document["FactoryContext"] = MESH
        canonical = physics.canonicalize(document)
        self.assertEqual(canonical["FactoryContext"], MESH)
        self.assertEqual(physics.diff(canonical, physics.canonicalize(_document(_body())), exact=False), ())
        fitted = _body()
        fitted["Bodies"]["head"]["Fit"] = {"GeomType": "EFG_Sphere"}
        self.assertEqual(physics.body_diff(fitted, _body(), exact=False), [])

    def test_factory_context(self):
        document = _document({})
        for value in ("SKM_Hero", 3, "/Game/NoDot"):
            document["FactoryContext"] = value
            with self.assertRaises(AssetDocumentContractError) as caught:
                physics.canonicalize(document)
            self.assertEqual(caught.exception.diagnostic.code, "PhysicsAsset.InvalidFactoryContext")
        self.assertEqual(physics.factory_mesh({"FactoryContext": MESH, "Body": {"PreviewMesh": "/Game/B.B"}}),
                         (MESH, "/FactoryContext"))
        self.assertEqual(physics.factory_mesh({"Body": {"PreviewMesh": MESH}}), (MESH, "/Body/PreviewMesh"))
        self.assertEqual(physics.factory_mesh({"Body": {}}), (None, "/FactoryContext"))

    def test_creating_is_held_by_identity(self):
        document = _document({})
        twin = _document({})
        physics.mark_creating(document)
        self.assertFalse(physics.take_creating(twin))
        self.assertTrue(physics.take_creating(document))
        self.assertFalse(physics.take_creating(document))

    def test_lifecycle_takes_the_mesh_as_factory_context(self):
        self.assertEqual(physics.LIFECYCLE.create_context_handle_names, ("factory_context",))
        self.assertIn(physics.common.REFERENCE_RESOLVE, [item.primitive_id for item in physics.REQUIREMENTS])

    def test_generation_op(self):
        op = plan.generation_op({"Generation": {"GeomType": "EFG_Box"}})
        self.assertEqual(op.op, {"op": "generate", "params": {"GeomType": "EFG_Box"}})
        self.assertEqual(plan.op_path("/ops/0/params/MinBoneSize", [op]), "/Body/Generation/MinBoneSize")
        self.assertEqual(plan.generation_op({}).op, {"op": "generate"})

    def test_fit_on_new_bodies(self):
        body = {"Bodies": {"neck": {"Fit": {"GeomType": "EFG_Sphere"}}, "jaw": {"Fit": {}, "Shapes": {"Spheres": [{}]}},
                           "nose": {"Shapes": {}}}}
        adds = plan.phase_body_adds(body, {}, {"Bodies": {}})
        self.assertEqual(adds[0].op, {"op": "bodies.add", "bones": ["neck", "jaw", "nose"],
                                      "fit": {"neck": {"GeomType": "EFG_Sphere"}}})
        self.assertEqual(plan.op_path("/ops/0/fit/neck/LodIndex", adds), "/Body/Bodies/neck/Fit/LodIndex")
        self.assertEqual(plan.op_path("/ops/0/bones/2", adds), "/Body/Bodies/nose")
        # A fitted new body needs no Shapes.
        plan.refuse({"Bodies": {"neck": {"Fit": {}}}}, plan._with_profiles(_live(), {}))

    def test_create_only_warnings(self):
        body = {"Generation": {}, "Bodies": {"head": {"Fit": {}}, "jaw": {"Fit": {}, "Shapes": {}}, "neck": {"Fit": {}}}}
        warnings = plan.create_only_warnings(body, _live(), generated=False)
        self.assertEqual([(item.code, item.path) for item in warnings],
                         [("PhysicsAsset.GenerationIgnored", "/Body/Generation"),
                          ("PhysicsAsset.FitIgnored", "/Body/Bodies/head/Fit"),
                          ("PhysicsAsset.FitIgnored", "/Body/Bodies/jaw/Fit")])
        self.assertEqual(plan.create_only_warnings({"Generation": {}}, {"Bodies": {}}, generated=True), [])

    def test_preflight_create(self):
        document = _document({"Bodies": {"neck": {"Shapes": {}}},
                              "Constraints": {"a": {"Child": "neck", "Parent": "head"}}})
        document["FactoryContext"] = MESH
        with self.assertRaises(AssetDocumentContractError) as caught:
            plan.preflight_create(physics.canonicalize(document))
        self.assertEqual((caught.exception.diagnostic.code, caught.exception.diagnostic.path),
                         ("PhysicsAsset.BodyNotFound", "/Body/Constraints/a/Parent"))
        plan.preflight_create(physics.canonicalize(_document({"Constraints": {"a": {"Child": "x", "Parent": "y"}}})))
        no_mesh = _document({"Bodies": {"neck": {"Shapes": {}}}})
        with self.assertRaises(AssetDocumentContractError) as caught:
            plan.preflight_create(physics.canonicalize(no_mesh))
        self.assertEqual(caught.exception.diagnostic.code, "PhysicsAsset.PreviewMeshRequired")


class DiffTests(unittest.TestCase):
    def test_extract_has_no_differences(self):
        self.assertEqual(physics.body_diff(_body(), _body(), exact=False), [])

    def test_omitted_regions_and_sparse_entries(self):
        # Omitted Shapes keep; omitted Properties / PhysicalAnimation / Profiles / Frames are the defaults.
        body = {"Bodies": {"pelvis": {"PhysicalAnimation": {"HitReact": {"OrientationStrength": 500}}},
                           "spine_01": {}, "head": {"Properties": {"PhysicsType": "PhysType_Kinematic"}}}}
        self.assertEqual(physics.body_diff(body, _body(), exact=False), [])
        body["Bodies"]["head"] = {}
        changes = physics.body_diff(body, _body(), exact=False)
        self.assertEqual([change.path for change in changes], ["/Body/Bodies/head/Properties/PhysicsType"])

    def test_shapes_are_complete_over_authored_types(self):
        body = {"Bodies": {"head": {"Shapes": {"Boxes": [{"X": 2}]}}}}
        paths = [change.path for change in physics.body_diff(body, _body(), exact=False)]
        self.assertIn("/Body/Bodies/head/Shapes/Boxes/0", paths)
        self.assertIn("/Body/Bodies/head/Shapes/Spheres/0", paths)

    def test_explicit_defaults_equal_omitted_ones(self):
        # A sparse entry may spell out a default: it compares equal to the omitted key the read gives.
        body = _body()
        body["Bodies"]["pelvis"]["Properties"] = {"PhysicsType": "PhysType_Default", "DefaultInstance": {"bOverrideMass": False}}
        body["Bodies"]["pelvis"]["PhysicalAnimation"]["HitReact"]["bIsLocalSimulation"] = True
        body["Constraints"]["spine_01"]["Properties"] = {"ProfileInstance": {"TwistLimit": {"TwistMotion": "ACM_Free"}}}
        body["Constraints"]["spine_01"]["Profiles"]["Ragdoll"]["TwistLimit"] = {"TwistLimitDegrees": 45}
        self.assertEqual(physics.body_diff(body, _body(), exact=False), [])
        # A profile entry is sparse against the constraint's own default profile.
        live = _body()
        live["Constraints"]["spine_01"]["Properties"] = {"ProfileInstance": {"TwistLimit": {"TwistMotion": "ACM_Locked"}}}
        body["Constraints"]["spine_01"]["Properties"] = {"ProfileInstance": {"TwistLimit": {"TwistMotion": "ACM_Locked"}}}
        body["Constraints"]["spine_01"]["Profiles"]["Ragdoll"]["TwistLimit"]["TwistMotion"] = "ACM_Locked"
        self.assertEqual(physics.body_diff(body, live, exact=False), [])
        # A profile a body is not in is still an add.
        body = _body()
        body["Bodies"]["head"]["PhysicalAnimation"] = {"HitReact": {}}
        changes = physics.body_diff(body, _body(), exact=False)
        self.assertEqual([change.path for change in changes], ["/Body/Bodies/head/PhysicalAnimation/HitReact"])
        self.assertFalse(plan._differs({"PhysicsType": "PhysType_Default"}, {}, physics.body_defaults()["Properties"]))

    def test_negative_zero_reads_as_zero(self):
        records = _records()
        records[1]["entry"]["Shapes"]["Capsules"][0]["Rotation"] = {"Pitch": 1.5, "Yaw": -0.0, "Roll": 0}
        body, _ = physics.assemble(records)
        rotation = body["Bodies"]["pelvis"]["Shapes"]["Capsules"][0]["Rotation"]
        self.assertEqual(repr(rotation["Yaw"]), "0.0")
        self.assertEqual(rotation["Pitch"], 1.5)

    def test_collision_pairs_compare_as_a_set(self):
        self.assertEqual(physics.body_diff({"CollisionDisabled": [["head", "pelvis"]]}, _body(), exact=False), [])


class PhaseTests(unittest.TestCase):
    def test_unchanged_document_plans_nothing(self):
        properties = {"ConstraintProfiles": ["Ragdoll"], "PhysicalAnimationProfiles": ["HitReact"]}
        for phase in plan.PHASES:
            self.assertEqual(_ops(phase, _body(), properties), [], phase.__name__)

    def test_profile_renames_are_positional(self):
        self.assertEqual(plan.profile_renames(["Falling", "B"], ["Ragdoll", "B"]), [("Ragdoll", "Falling")])
        self.assertEqual(plan.profile_renames(["B", "Ragdoll"], ["Ragdoll", "B"]), [])
        self.assertEqual(plan.profile_renames(["Ragdoll"], ["Ragdoll", "B"]), [])
        ops = _ops(plan.phase_profiles, {}, {"ConstraintProfiles": ["Falling"]})
        self.assertEqual(ops, [{"op": "profiles.rename", "kind": "constraint", "from": "Ragdoll", "to": "Falling"},
                               {"op": "profiles.set", "kind": "constraint", "names": ["Falling"]}])

    def test_structure_phases(self):
        body = _body()
        del body["Bodies"]["head"]
        del body["Constraints"]["head"]
        body["Bodies"]["neck"] = {"Shapes": {"Spheres": [{"Radius": 3}]}}
        body["Constraints"]["neck"] = {"Child": "neck", "Parent": "spine_01"}
        body["Constraints"]["spine_01"]["Parent"] = "neck"
        self.assertEqual(_ops(plan.phase_constraint_removals, body), [{"op": "constraint.remove", "joint": "head"}])
        self.assertEqual(_ops(plan.phase_body_removals, body), [{"op": "body.remove", "bone": "head"}])
        self.assertEqual(_ops(plan.phase_body_adds, body), [{"op": "bodies.add", "bones": ["neck"]}])
        self.assertEqual(_ops(plan.phase_constraint_structure, body),
                         [{"op": "constraint.bones", "joint": "spine_01", "child": "spine_01", "parent": "neck"},
                          {"op": "constraint.add", "joint": "neck", "child": "neck", "parent": "spine_01"}])

    def test_rename_removes_a_taken_name_first(self):
        body = _body()
        renamed = dict(body["Constraints"].pop("spine_01"), RenamedFrom="spine_01")
        body["Constraints"]["head"] = renamed
        self.assertEqual(_ops(plan.phase_constraint_renames, body),
                         [{"op": "constraint.remove", "joint": "head"},
                          {"op": "constraint.rename", "joint": "spine_01", "to": "head"}])

    def test_details_frames_profiles_collision(self):
        body = _body()
        body["Bodies"]["pelvis"]["Properties"] = {"PhysicsType": "PhysType_Simulated"}
        body["Constraints"]["head"]["Frames"] = {"Child": {}, "Parent": {}}
        body["Constraints"]["spine_01"]["Profiles"] = {}
        body["CollisionDisabled"] = [["spine_01", "head"]]
        self.assertEqual(_ops(plan.phase_body_updates, body),
                         [{"op": "body.set", "bone": "pelvis", "properties": {"PhysicsType": "PhysType_Simulated"}}])
        self.assertEqual(_ops(plan.phase_constraint_details, body)[0]["op"], "constraint.frames")
        self.assertEqual(_ops(plan.phase_constraint_profiles, body),
                         [{"op": "constraint.profiles", "joint": "spine_01", "profiles": {}}])
        self.assertEqual(_ops(plan.phase_collision, body),
                         [{"op": "collision.set", "pairs": [["head", "pelvis"]], "enabled": True},
                          {"op": "collision.set", "pairs": [["spine_01", "head"]], "enabled": False}])


class ErrorPathTests(unittest.TestCase):
    def test_op_paths(self):
        adds = plan.phase_body_adds({"Bodies": {"neck": {}, "jaw": {}}}, {}, {"Bodies": {}})
        self.assertEqual(plan.op_path("/ops/0/bones/1", adds), "/Body/Bodies/jaw")
        sets = plan.phase_body_updates({"Bodies": {"head": {"Properties": {"X": 1}}}}, {}, _live())
        self.assertEqual(plan.op_path("/ops/0/properties/X", sets), "/Body/Bodies/head/Properties/X")
        structure = plan.phase_constraint_structure({"Constraints": {"neck": {"Child": "neck", "Parent": "head"}}}, {},
                                                    _live())
        self.assertEqual(plan.op_path("/ops/0/parent", structure), "/Body/Constraints/neck/Parent")
        self.assertEqual(plan.op_path("/ops/7", structure), "/Body")

    def _refused(self, body, code, path, properties=None):
        live = plan._with_profiles(_live(), properties or {})
        with self.assertRaises(AssetDocumentContractError) as caught:
            plan.refuse(body, live)
        diagnostic = caught.exception.diagnostic
        self.assertEqual((diagnostic.code, diagnostic.path), (code, path))
        return diagnostic.message

    def test_body_not_found_names_close_bodies(self):
        # BodyNotFound named no candidates.
        message = self._refused({"CollisionDisabled": [["head", "Head"]]}, "PhysicsAsset.BodyNotFound",
                                "/Body/CollisionDisabled/0/1")
        self.assertIn("Close: head.", message)
        message = self._refused({"Constraints": {"a": {"Child": "head", "Parent": "nope", "Profiles": {}}}},
                                "PhysicsAsset.BodyNotFound", "/Body/Constraints/a/Parent")
        self.assertIn("Bodies: ", message)

    def test_refusals(self):
        self._refused({"Bodies": {"neck": {}}}, "PhysicsAsset.ShapesRequired", "/Body/Bodies/neck")
        self._refused({"PreviewMesh": None, "Bodies": {"neck": {"Shapes": {}}}}, "PhysicsAsset.PreviewMeshRequired",
                      "/Body/Bodies/neck")
        self._refused({"Bodies": {"head": {"PhysicalAnimation": {"Other": {}}}}}, "PhysicsAsset.UnknownProfile",
                      "/Body/Bodies/head/PhysicalAnimation/Other")
        self._refused({"Constraints": {"a": {"Child": "head", "Parent": "nope", "Profiles": {}}}},
                      "PhysicsAsset.BodyNotFound", "/Body/Constraints/a/Parent")
        self._refused({"Constraints": {"a": {"Child": "head", "Parent": "pelvis", "Profiles": {"Ragdoll": {}}}}},
                      "PhysicsAsset.UnknownProfile", "/Body/Constraints/a/Profiles/Ragdoll", {"ConstraintProfiles": []})
        self._refused({"CollisionDisabled": [["head", "nope"]]}, "PhysicsAsset.BodyNotFound", "/Body/CollisionDisabled/0/1")


class OutcomeTests(unittest.TestCase):
    def test_new_errors_fail_old_errors_warn(self):
        before = {"Messages": [{"Severity": "Error", "Message": "old", "Path": "/Body/Bodies/a"}]}
        after = {"Messages": [{"Severity": "Error", "Message": "old", "Path": "/Body/Bodies/a"},
                              {"Severity": "Warning", "Message": "w", "Path": "/Body/Bodies/b"}]}
        failure, warnings = plan.validation_outcome(before, after)
        self.assertIsNone(failure)
        self.assertEqual([item.code for item in warnings], ["PhysicsAsset.ValidationError", "PhysicsAsset.ValidationWarning"])
        after["Messages"].append({"Severity": "Error", "Message": "new", "Path": "/Body/Bodies/c/Shapes"})
        failure, _ = plan.validation_outcome(before, after)
        self.assertEqual((failure.diagnostic.code, failure.diagnostic.path),
                         ("PhysicsAsset.ValidationFailed", "/Body/Bodies/c/Shapes"))


if __name__ == "__main__":
    unittest.main()
