"""Sparse-over-defaults verification: an authored value equal to the default a sparse read leaves out, and a
partial struct (Apply writes it as an edit: members it omits keep their value), compare equal to the read back.
The editor side (apply + Diff through the production service per Profile) is
UEAssetDoc.SparseDefaults.*."""
from __future__ import annotations

import unittest

from asset_document.diff.model import Difference, DiffKind
from asset_document.profiles import data_table_common as common
from asset_document.profiles import level_sequence as seq
from asset_document.profiles import metasound
from asset_document.profiles import niagara_apply
from asset_document.profiles import physics_asset as physics
from asset_document.profiles import sound_cue_model


def _paths(changes):
    return [change.path for change in changes]


class StructAsAuthoredTests(unittest.TestCase):
    def test_struct_members_the_value_omits_are_left_out(self):
        live = {"FalloffDistance": 2000, "AttenuationShape": "Sphere", "Extents": {"X": 400, "Y": 0}}
        self.assertEqual(common.struct_as_authored({"FalloffDistance": 2000}, live), {"FalloffDistance": 2000})
        self.assertEqual(common.struct_as_authored({"Extents": {"X": 1}}, live), {"Extents": {"X": 400}})
        # Same-length arrays element by element; anything else as it is.
        self.assertEqual(common.struct_as_authored([{"A": 1}], [{"A": 1, "B": 2}]), [{"A": 1}])
        self.assertEqual(common.struct_as_authored([{"A": 1}], [{"A": 1}, {"A": 2}]), [{"A": 1}, {"A": 2}])
        self.assertEqual(common.struct_as_authored(1, {"A": 1}), {"A": 1})
        # A member the live value does not have stays missing (a difference).
        self.assertEqual(common.struct_as_authored({"Unknown": 1}, live), {})


class _Item:
    def __init__(self, name, readable=True, writable=True):
        self.name, self.readable, self.writable = name, readable, writable


class _AsAuthoredAdapter:
    """Stands in for PropertySemanticAdapter: the as-authored read keeps the authored struct members."""
    live = {}
    reads = []

    def enumerate(self, class_path, context):
        return tuple(_Item(name) for name in self.live)

    def extract(self, read, admitted, context, include_defaults=True, authored=None):
        _AsAuthoredAdapter.reads.append((tuple(item.name for item in admitted), authored))
        return {item.name: common.struct_as_authored(authored.get(item.name), self.live[item.name])
                for item in admitted}


class PropertiesAsAuthoredTests(unittest.TestCase):
    def setUp(self):
        self.saved = common.PropertySemanticAdapter
        common.PropertySemanticAdapter = _AsAuthoredAdapter
        _AsAuthoredAdapter.live = {"SolverSettings": {"PositionIterations": 8, "VelocityIterations": 1},
                                   "Tags": ["A", "B"], "Scale": 2.0}
        _AsAuthoredAdapter.reads = []

    def tearDown(self):
        common.PropertySemanticAdapter = self.saved

    def _check(self, properties):
        document = {"Class": "/Script/Engine.PhysicsAsset", "Properties": properties}
        changes = common.properties_diff(properties, _AsAuthoredAdapter.live, False)
        return common.properties_as_authored(tuple(changes), document, None, None, False)

    def test_partial_struct_compares_in_its_shape(self):
        self.assertEqual(self._check({"SolverSettings": {"PositionIterations": 8}}), ())
        self.assertEqual(_AsAuthoredAdapter.reads, [(("SolverSettings",), {"SolverSettings": {"PositionIterations": 8}})])

    def test_a_differing_member_is_still_reported(self):
        self.assertEqual(_paths(self._check({"SolverSettings": {"PositionIterations": 4}})), ["/Properties/SolverSettings"])

    def test_scalars_and_other_differences_are_not_read_again(self):
        self.assertEqual(_paths(self._check({"Scale": 3.0})), ["/Properties/Scale"])
        self.assertEqual(_AsAuthoredAdapter.reads, [])
        self.assertEqual(_paths(self._check({"Tags": ["A"]})), ["/Properties/Tags"])


class PhysicsShapeDefaultsTests(unittest.TestCase):
    def setUp(self):
        header = {"kind": "header", "preview_mesh": "", "bodies": 1, "constraints": 0, "defaults": {
            "body": {}, "constraint": {}, "physical_animation": {},
            "shapes": {"Capsules": {"Center": {"X": 0, "Y": 0, "Z": 0}, "Rotation": {"Pitch": 0, "Yaw": 0, "Roll": 0},
                                    "Radius": 1, "Length": 1, "bContributeToMass": True}}}}
        self.live, _ = physics.assemble([header, {"kind": "body", "bone": "pelvis", "entry": {
            "Shapes": {"Capsules": [{"Radius": 10, "Length": 20}]}}}])

    def _diff(self, element):
        body = {"Bodies": {"pelvis": {"Shapes": {"Capsules": [element]}}}}
        return physics.body_diff(body, self.live, exact=False)

    def test_explicit_shape_defaults_and_the_auto_name_equal_omitted_ones(self):
        self.assertEqual(self._diff({"Center": {"X": 0, "Y": 0, "Z": 0}, "Rotation": {"Pitch": 0, "Yaw": 0, "Roll": 0},
                                     "Radius": 10, "Length": 20, "bContributeToMass": True, "Name": "pelvis_capsule"}), [])

    def test_a_non_default_shape_field_is_a_difference(self):
        self.assertEqual(_paths(self._diff({"Radius": 10, "Length": 20, "bContributeToMass": False})),
                         ["/Body/Bodies/pelvis/Shapes/Capsules/0/bContributeToMass"])
        self.assertEqual(_paths(self._diff({"Radius": 10, "Length": 20, "Name": "Hips"})),
                         ["/Body/Bodies/pelvis/Shapes/Capsules/0/Name"])


class SoundCueNodeStructTests(unittest.TestCase):
    DEFAULTS = {"PitchModulationParams": {"Default": 1.0, "MinInput": 0.0, "ParamMode": "MPM_Normal"}, "Volume": 1.0}

    def _diff(self, authored, live):
        changes = []
        sound_cue_model._properties_diff(changes, "/Body/Nodes/Mod/Properties", authored, live, self.DEFAULTS, False)
        return changes

    def test_partial_node_struct_compares_in_its_shape(self):
        live = {"PitchModulationParams": {"Default": 1.5, "MinInput": 0.0, "ParamMode": "MPM_Normal"}}
        self.assertEqual(self._diff({"PitchModulationParams": {"Default": 1.5}}, live), [])
        self.assertEqual(_paths(self._diff({"PitchModulationParams": {"Default": 2.0}}, live)),
                         ["/Body/Nodes/Mod/Properties/PitchModulationParams"])

    def test_partial_struct_over_an_omitted_default(self):
        self.assertEqual(self._diff({"PitchModulationParams": {"ParamMode": "MPM_Normal"}}, {}), [])
        # An omitted property still compares with its default (complete over the defaults).
        self.assertEqual(_paths(self._diff({}, {"Volume": 0.5})), ["/Body/Nodes/Mod/Properties/Volume"])


class NiagaraPropertyChangesTests(unittest.TestCase):
    DEFAULTS = {"Platforms": {"QualityLevelMask": 0, "CVarConditions": [], "DeviceProfileStates": []},
                "Tags": {}}

    def test_partial_struct_is_no_write(self):
        live = {"Platforms": {"QualityLevelMask": 3, "CVarConditions": [], "DeviceProfileStates": []}}
        self.assertEqual(niagara_apply.property_changes({"Platforms": {"QualityLevelMask": 3}}, live, self.DEFAULTS),
                         ({}, []))
        write, _ = niagara_apply.property_changes({"Platforms": {"QualityLevelMask": 1}}, live, self.DEFAULTS)
        self.assertEqual(write, {"Platforms": {"QualityLevelMask": 1}})

    def test_a_map_is_complete(self):
        # An object whose keys are not the default's is a map: an entry it does not list is a difference.
        live = {"Tags": {"A": 1, "B": 2}}
        write, _ = niagara_apply.property_changes({"Tags": {"A": 1}}, live, self.DEFAULTS)
        self.assertEqual(write, {"Tags": {"A": 1}})


class MetaSoundWidgetDefaultsTests(unittest.TestCase):
    WIDGET_DEFAULTS = {"Pitch": {"Range": {"X": 0, "Y": 1}, "WidgetType": "None", "ClampDefault": False}}

    def _diff(self, widget, live_widget):
        authored = {"Pitch": {"Type": "Float", "Widget": widget}}
        live = {"Pitch": {"Type": "Float", "SortOrder": 0, "Widget": live_widget}}
        changes = []
        metasound._member_diff(authored, live, "/Body/Inputs", set(), False, changes, self.WIDGET_DEFAULTS)
        return changes

    def test_widget_settings_at_their_defaults_equal_omitted_ones(self):
        self.assertEqual(self._diff({"Range": {"X": 100, "Y": 2000}, "WidgetType": "None", "ClampDefault": False},
                                    {"Range": {"X": 100, "Y": 2000}}), [])

    def test_a_non_default_widget_setting_is_a_difference(self):
        self.assertEqual(_paths(self._diff({"WidgetType": "Slider"}, {})), ["/Body/Inputs/Pitch/Widget"])
        # Without the defaults (an input without a settings object) the Widget compares as written.
        changes = []
        metasound._member_diff({"Pitch": {"Type": "Float", "Widget": {"WidgetType": "None"}}},
                               {"Pitch": {"Type": "Float", "Widget": {}}}, "/Body/Inputs", set(), False, changes)
        self.assertEqual(_paths(changes), ["/Body/Inputs/Pitch/Widget"])


class SequencerDefaultsAtTests(unittest.TestCase):
    TEMPLATE = "/Body/Bindings/0/Spawnable/Template"

    def test_absent_paths_are_the_leaves_of_body_adds(self):
        changes = [Difference(f"{self.TEMPLATE}/Components/CameraComponent/FieldOfView", DiffKind.ADD, True, 90, False, None),
                   Difference(f"{self.TEMPLATE}/Properties", DiffKind.ADD, True, {"A": {"B": 1}, "C": [1]}, False, None),
                   Difference("/Body/Bindings/0/Name", DiffKind.REPLACE, True, "Cam", True, "Other"),
                   Difference("/Properties/X", DiffKind.ADD, True, 1, False, None)]
        self.assertEqual(seq.absent_paths(changes), {f"{self.TEMPLATE}/Components/CameraComponent/FieldOfView": 90,
                                                     f"{self.TEMPLATE}/Properties/A/B": 1,
                                                     f"{self.TEMPLATE}/Properties/C": [1]})

    def test_an_add_is_explained_when_every_listed_value_is_its_default(self):
        add = Difference(f"{self.TEMPLATE}/Components", DiffKind.ADD, True,
                         {"CameraComponent": {"FieldOfView": 90, "PostProcessSettings": {"BloomIntensity": 0.675}}}, False, None)
        defaults = {f"{self.TEMPLATE}/Components/CameraComponent/FieldOfView": 90.0,
                    f"{self.TEMPLATE}/Components/CameraComponent/PostProcessSettings/BloomIntensity": 0.675}
        self.assertTrue(seq.explained_by_defaults(add, defaults, False))
        # A value that is not its default, or a path the read reported no default for, stays a difference.
        self.assertFalse(seq.explained_by_defaults(add, {**defaults, f"{self.TEMPLATE}/Components/CameraComponent/FieldOfView": 60},
                                                   False))
        self.assertFalse(seq.explained_by_defaults(add, {f"{self.TEMPLATE}/Components/CameraComponent/FieldOfView": 90}, False))
        replace = Difference(f"{self.TEMPLATE}/Components/CameraComponent/FieldOfView", DiffKind.REPLACE, True, 90, True, 60)
        self.assertFalse(seq.explained_by_defaults(replace, defaults, False))

    def test_defaults_are_read_with_the_listed_paths(self):
        class Calls:
            def __init__(self):
                self.args = []

            def query(self, primitive, args):
                self.args.append(args)
                return {"records": [{"kind": "header"}, {"kind": "default", "path": "/Body/X", "value": 1},
                                    {"kind": "warning", "path": "/Body/Y"}], "next_offset": -1}
        calls = Calls()
        self.assertEqual(seq.read_defaults_at(calls, {"/Body/X": 1, "/Body/Z": 2}), {"/Body/X": 1})
        self.assertEqual(calls.args, [{"defaults_at": ["/Body/X", "/Body/Z"], "offset": 0}])


if __name__ == "__main__":
    unittest.main()
