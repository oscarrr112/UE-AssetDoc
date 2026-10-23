# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Niagara Profiles: document shaping from synthetic fx.niagara.read.v1 rows, paging, Diff and the Apply planners
(system level, module stacks, emitter assets and parameter collections).  The editor side
(headless stack view models, edits against the editor's own paths, extract stability across save / reload /
duplicate) is covered by UEAssetDoc.Niagara.* in the editor."""
from __future__ import annotations

import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.diagnostics import RuntimeDiagnostic
from asset_document.diff.model import DiffKind
from asset_document.profiles import niagara
from asset_document.profiles import niagara_apply as planner
from asset_document.profiles import niagara_parameter_collection as niagara_collection
from asset_document.profiles import niagara_shape as shape
from asset_document.profiles import niagara_stack
from asset_document.read_only_extract import extract_document
from asset_document.regions.primitive_catalog import PrimitiveOutcome, PrimitiveOutcomeStatus

TARGET = "/Game/FX/NS_Test"
SPAWN_BURST = "/Niagara/Modules/Emitter/SpawnBurst_Instantaneous.SpawnBurst_Instantaneous"
UNIFORM = "/Niagara/DynamicInputs/UniformRange/UniformRangedFloat.UniformRangedFloat"

SYSTEM_ROWS = [
    {"kind": "system"},
    {"kind": "user_parameter", "name": "User.Count", "type": "int32", "value": 50},
    {"kind": "user_parameter", "name": "User.Mesh", "type": "Object:/Script/Engine.StaticMesh", "object": "",
     "description": "Spawned mesh", "category": "Look|Meshes"},
    {"kind": "module", "emitter": "", "group": "SystemUpdate", "name": "SystemState", "script": "/Niagara/S.S",
     "enabled": True, "inputs": [{"name": "Loop Behavior", "type": "/Niagara/Enums/E.E", "mode": "Local",
                                  "overridden": True, "value": "Once"}]},
    {"kind": "emitter", "name": "Sparks", "enabled": True, "mode": "Standard", "properties": {"SimTarget": "GPUComputeSim"}},
    {"kind": "renderer", "emitter": "Sparks", "name": "NiagaraSpriteRendererProperties_0", "class": "SpriteRenderer",
     "enabled": True, "properties": {"Alignment": "VelocityAligned"}},
    {"kind": "event_handler", "emitter": "Sparks", "index": 0, "properties": {"SpawnNumber": 4}},
    {"kind": "simulation_stage", "emitter": "Sparks", "name": "Fill", "class": "Generic", "enabled": False, "properties": {}},
    {"kind": "module", "emitter": "Sparks", "group": "EmitterUpdate", "name": "SpawnBurst_Instantaneous",
     "script": SPAWN_BURST, "enabled": False, "display_name": "Burst", "inputs": [
         {"name": "Spawn Count", "type": "int32", "mode": "Linked", "overridden": True, "link": "User.Count"},
         {"name": "Spawn Time", "type": "float", "mode": "Local", "overridden": False, "value": 0.0}]},
    {"kind": "module", "emitter": "Sparks", "group": "ParticleSpawn", "name": "InitializeParticle",
     "script": "/Niagara/I.I", "enabled": True, "inputs": [
         {"name": "Lifetime", "type": "float", "mode": "Dynamic", "overridden": True,
          "dynamic": {"script": UNIFORM, "inputs": [
              {"name": "Minimum", "type": "float", "mode": "Local", "overridden": True, "value": 0.4},
              {"name": "Maximum", "type": "float", "mode": "Local", "overridden": False, "value": 1.0}]}},
         {"name": "Color", "type": "LinearColor", "mode": "Expression", "overridden": True, "expression": "1 - x"},
         {"name": "Odd", "type": "float", "mode": "DefaultFunction", "overridden": True}]},
    {"kind": "module", "emitter": "Sparks", "group": "EventHandler", "event": 0, "name": "SetVariables_1",
     "assign": [{"Target": "Particles.Color", "Type": "LinearColor"}], "enabled": True, "inputs": []},
    {"kind": "module", "emitter": "Sparks", "group": "SimulationStage", "stage": "Fill", "name": "Fill",
     "script": "ScratchPad:Fill", "enabled": True, "inputs": []},
    {"kind": "script", "scope": "Sparks/Fill", "usage": "Module", "settings": {"Category": "Grid"}},
    {"kind": "node", "scope": "Sparks/Fill", "id": "NiagaraNodeOutput_0", "action": "Niagara:Output", "x": 10, "y": -4},
    {"kind": "node", "scope": "Sparks/Fill", "id": "NiagaraNodeParameterMapGet_0", "action": "Niagara:MapGet", "x": 0,
     "y": 0, "pins": {"Inputs": [], "Outputs": [{"Name": "Module.A", "Type": "float"}]}},
    {"kind": "link", "scope": "Sparks/Fill", "from": "NiagaraNodeParameterMapGet_0:Dest", "to": "NiagaraNodeOutput_0:Output"},
    {"kind": "pin_default", "scope": "Sparks/Fill", "pin": "NiagaraNodeOutput_0:Output", "value": "1.0"},
    {"kind": "script", "scope": "/Global", "usage": "Module", "settings": {}},
    {"kind": "library", "group": "ParticleSpawn", "scripts": [UNIFORM]},
    {"kind": "compile", "compile": {"Status": "UpToDate", "Messages": []}},
]


class ShapeTests(unittest.TestCase):
    def test_a_data_interface_is_sparse_in_an_extract_only(self):
        # The read carries the curve with every key field and, for extract (diff_only), the
        # same values without the fields equal to the class default's.
        full = {"FloatCurve": {"Keys": [{"Time": 0, "Value": 1, "InterpMode": "RCIM_Cubic", "ArriveTangentWeight": 0}]}}
        sparse = {"FloatCurve": {"Keys": [{"Value": 1, "InterpMode": "RCIM_Cubic"}]}}
        row = {"name": "Curve", "mode": "Data", "data_interface": {"Class": "Curve", "Properties": full,
                                                                     "SparseProperties": sparse}}
        self.assertEqual(shape.input_value(row), {"DataInterface": {"Class": "Curve", "Properties": full}})
        with shape.sparse_data_interfaces():
            self.assertEqual(shape.input_value(row), {"DataInterface": {"Class": "Curve", "Properties": sparse}})
            with shape.sparse_data_interfaces(False):
                self.assertEqual(shape.input_value(row)["DataInterface"]["Properties"], full)
        self.assertIn("SparseProperties", row["data_interface"])  # the row itself is left as read

    def test_system_body_follows_the_design_shape(self) -> None:
        body = shape.system_body(SYSTEM_ROWS)
        self.assertEqual(body["UserParameters"], [
            {"Name": "User.Count", "Type": "int32", "Value": 50},
            {"Name": "User.Mesh", "Type": "Object:/Script/Engine.StaticMesh", "Value": {"Object": ""},
             "Description": "Spawned mesh", "Category": "Look|Meshes"}])
        self.assertEqual(body["SystemUpdate"], [{"Name": "SystemState", "Script": "/Niagara/S.S",
                                                 "Inputs": {"Loop Behavior": "Once"}}])
        self.assertEqual(body["ScratchPads"][0]["Name"], "Global")
        [emitter] = body["Emitters"]
        self.assertEqual(emitter["Properties"], {"SimTarget": "GPUComputeSim"})
        self.assertEqual(emitter["EmitterUpdate"], [{"Name": "SpawnBurst_Instantaneous", "Script": SPAWN_BURST,
                                                     "Enabled": False, "DisplayName": "Burst",
                                                     "Inputs": {"Spawn Count": {"Link": "User.Count"}}}])
        inputs = emitter["ParticleSpawn"][0]["Inputs"]
        self.assertEqual(inputs["Lifetime"], {"Dynamic": {"Script": UNIFORM, "Inputs": {"Minimum": 0.4}}})
        self.assertEqual(inputs["Color"], {"Expression": "1 - x"})
        self.assertEqual(inputs["Odd"], {"Unsupported": "DefaultFunction"})
        self.assertEqual(emitter["EventHandlers"], [{"Properties": {"SpawnNumber": 4}, "Modules": [
            {"Name": "SetVariables_1", "Assign": [{"Target": "Particles.Color", "Type": "LinearColor"}]}]}])
        self.assertEqual(emitter["SimulationStages"], [{"Name": "Fill", "Class": "Generic", "Enabled": False,
                                                        "Modules": [{"Name": "Fill", "Script": "ScratchPad:Fill"}]}])
        self.assertEqual(emitter["Renderers"], [{"Name": "NiagaraSpriteRendererProperties_0", "Class": "SpriteRenderer",
                                                 "Properties": {"Alignment": "VelocityAligned"}}])
        [pad] = emitter["ScratchPads"]
        self.assertEqual(pad["Graph"]["Links"], [{"From": "NiagaraNodeParameterMapGet_0:Dest", "To": "NiagaraNodeOutput_0:Output"}])
        self.assertEqual(pad["Graph"]["PinDefaults"], {"NiagaraNodeOutput_0:Output": "1.0"})
        self.assertEqual(pad["Settings"], {"Category": "Grid"})

    def test_stateless_emitter_keeps_its_reflected_shape(self) -> None:
        stateless = {"Properties": {}, "Modules": [{"Name": "M", "Class": "ShapeLocation", "Properties": {}}], "Renderers": []}
        body = shape.system_body([{"kind": "emitter", "name": "Light", "enabled": True, "mode": "Stateless",
                                   "stateless": stateless}])
        self.assertEqual(body["Emitters"], [{"Name": "Light", "Mode": "Stateless", "Stateless": stateless}])

    def test_emitter_asset_body_drops_handle_fields(self) -> None:
        rows = [row for row in SYSTEM_ROWS if row.get("emitter", row.get("name")) == "Sparks" or row["kind"] in ("script", "node")]
        body = shape.emitter_asset_body(rows)
        self.assertNotIn("Name", body)
        self.assertIn("ParticleSpawn", body)

    def test_script_and_collection_bodies(self) -> None:
        rows = [{"kind": "script", "scope": "", "usage": "Function", "settings": {"Description": "d"}},
                {"kind": "parameter", "scope": "", "name": "Module.Strength", "type": "float", "default": 1.0,
                 "meta": {"Description": "s"}},
                {"kind": "node", "scope": "", "id": "Op", "action": "Niagara:Op:Numeric::Mul", "x": 1, "y": 2}]
        body = shape.script_asset_body(rows)
        self.assertEqual(body["Usage"], "Function")
        self.assertEqual(body["Parameters"], [{"Name": "Module.Strength", "Type": "float", "Default": 1.0,
                                               "Meta": {"Description": "s"}}])
        self.assertEqual(body["Graph"]["Nodes"], [{"Id": "Op", "Action": "Niagara:Op:Numeric::Mul", "Position": [1, 2]}])
        collection = shape.collection_body([{"kind": "parameter", "name": "Speed", "type": "float", "value": 2.5},
                                            {"kind": "parameter", "name": "Tint", "type": "LinearColor", "material": True,
                                             "value": {"R": 1.0, "G": 0.0, "B": 0.0, "A": 1.0}}])
        self.assertEqual(collection, {"Parameters": [
            {"Name": "Speed", "Type": "float", "Value": 2.5},
            {"Name": "Tint", "Type": "LinearColor", "Value": {"R": 1.0, "G": 0.0, "B": 0.0, "A": 1.0},
             "MaterialCollection": True}]})

    def test_inspect_facts_list_every_input_with_its_mode(self) -> None:
        facts = shape.inspect_facts(SYSTEM_ROWS, {"unsupported_properties": ["Foo"]})
        burst = next(item for item in facts["Modules"] if item["Module"] == "SpawnBurst_Instantaneous")
        self.assertEqual([(item["Name"], item["Mode"], item["Overridden"]) for item in burst["Inputs"]],
                         [("Spawn Count", "Linked", True), ("Spawn Time", "Local", False)])
        self.assertEqual(facts["Library"], {"ParticleSpawn": [UNIFORM]})
        self.assertEqual(facts["Compile"]["Status"], "UpToDate")
        self.assertEqual(facts["UnsupportedProperties"], ["Foo"])
        self.assertEqual(facts["Lifecycle"], [{"Emitter": "", "Module": "SystemState", "Loops": False, "Loop Behavior": "Once"}])

    def test_inspect_summary_keeps_overridden_inputs_and_names_the_others(self) -> None:
        # A system's inspect was 74 KB (every input's full row and every group's library); the
        # summary (inspect's HTTP default) keeps what is set and the names of what can be set.
        facts = shape.summarize(shape.inspect_facts(SYSTEM_ROWS, {"unsupported_properties": []}))
        burst = next(item for item in facts["Modules"] if item["Module"] == "SpawnBurst_Instantaneous")
        self.assertEqual(burst, {"Emitter": "Sparks", "Stack": "EmitterUpdate", "Module": "SpawnBurst_Instantaneous",
                                 "Overridden": {"Spawn Count": {"Link": "User.Count"}}, "Inputs": ["Spawn Time"]})
        initialize = next(item for item in facts["Modules"] if item["Module"] == "InitializeParticle")
        self.assertEqual(sorted(initialize["Overridden"]), ["Color", "Lifetime", "Odd"])
        self.assertNotIn("Inputs", initialize)
        self.assertEqual(facts["Library"], {"ParticleSpawn": 1})
        self.assertEqual(facts["Compile"]["Status"], "UpToDate")
        self.assertEqual(facts["Lifecycle"], shape.inspect_facts(SYSTEM_ROWS, {})["Lifecycle"])
        self.assertNotIn("Summary", shape.inspect_facts(SYSTEM_ROWS, {}))

    def test_lifecycle_says_whether_a_default_state_module_loops(self) -> None:
        # Loop Behavior at its default (Infinite) is not in the document; inspect says so.
        row = {"kind": "module", "emitter": "", "group": "SystemUpdate", "name": "SystemState", "inputs": [
            {"name": "Loop Behavior", "type": "/Niagara/Enums/E.E", "mode": "Local", "overridden": False, "value": "Infinite"},
            {"name": "Loop Duration", "type": "float", "mode": "Local", "overridden": True, "value": 2.0},
            {"name": "Inactive Response", "type": "/Niagara/Enums/R.R", "mode": "Local", "overridden": False, "value": "Complete"}]}
        self.assertEqual(shape.lifecycle_fact(row), {"Emitter": "", "Module": "SystemState", "Loops": True,
                                                     "Loop Behavior": {"Value": "Infinite", "Default": True},
                                                     "Loop Duration": 2.0})
        self.assertIsNone(shape.lifecycle_fact({**row, "group": "ParticleUpdate"}))
        self.assertNotIn("Lifecycle", shape.inspect_facts([], {}))


class _Context:
    """Serves fx.niagara.read.v1 pages of a row list (two rows per page)."""

    def __init__(self, rows, properties=None):
        self.rows = rows
        self.properties = properties or {}
        self.calls = []

    def handoff(self, *, source_handle, target_provider_id, access):
        assert target_provider_id == "fx" and access == "read_only"
        return source_handle

    def invoke(self, *, requirement, value_arguments, handle_arguments):
        if value_arguments.get("detail") == "defaults":
            return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED, {"emitter": {}, "renderers": {},
                                                                       "data_interfaces": {}}, (), (), ())
        self.calls.append(dict(value_arguments))
        offset = value_arguments["offset"]
        page = self.rows[offset:offset + 2]
        value = {"class": "system", "rows": page, "total": len(self.rows),
                 "next_offset": offset + 2 if offset + 2 < len(self.rows) else -1}
        if offset == 0:
            value["properties"] = self.properties
        return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED, value, (), (), ())


class _Recording:
    """A Properties region that records extract's include_defaults."""

    def __init__(self, seen):
        self.seen = seen

    def extract(self, read, context, include_defaults=True):
        self.seen.append(include_defaults)
        return {}


class _Properties:
    """Stands in for the system's generic Properties region."""

    def __init__(self, values):
        self.values = values
        self.applied = []

    def check_owned(self, properties):
        pass

    def validate(self, properties, context):
        pass

    def extract(self, read, context, include_defaults=True):
        return dict(self.values)

    def apply(self, properties, read, mutable, context):
        self.applied.append(dict(properties))
        return ()


class _Failing(_Context):
    def invoke(self, *, requirement, value_arguments, handle_arguments):
        return PrimitiveOutcome(PrimitiveOutcomeStatus.FAILED, None, (), (), (
            RuntimeDiagnostic(path="/", code="Niagara.InteractiveReadRefused", message="refused"),))


class ProfileTests(unittest.TestCase):
    def setUp(self) -> None:
        # System Details properties come from the generic property admission (editor primitives).
        self._properties = niagara.system_properties
        niagara.system_properties = lambda: _Properties({"WarmupTime": 1.0})

    def tearDown(self) -> None:
        niagara.system_properties = self._properties

    def test_extract_reads_every_page(self) -> None:
        context = _Context(SYSTEM_ROWS)
        document = niagara.NiagaraSystemProfile().extract(target=TARGET, asset_handle=object(), context=context)
        self.assertEqual([call["offset"] for call in context.calls], list(range(0, len(SYSTEM_ROWS), 2)))
        self.assertEqual(document["Class"], niagara.SYSTEM_CLASS)
        self.assertEqual(document["Properties"], {"WarmupTime": 1.0})
        self.assertEqual(document["Body"], shape.system_body(SYSTEM_ROWS))
        self.assertEqual(document["Body"]["UserParameters"][1]["Category"], "Look|Meshes")

    def test_read_only_targets_extract(self) -> None:
        profile = niagara.NiagaraSystemProfile()
        source = "/Niagara/DefaultAssets/Templates/Systems/NS_Fountain"
        document = extract_document(source, lambda: profile.extract(target=source, asset_handle=object(),
                                                                    context=_Context(SYSTEM_ROWS)),
                                    profile.canonicalize, read_only=True)
        self.assertEqual(document["Target"], source)
        with self.assertRaises(AssetDocumentContractError):
            profile.validate(document)
        document["Target"] = TARGET
        profile.validate(document)

    def test_inspect_adds_niagara_details(self) -> None:
        context = _Context(SYSTEM_ROWS)
        result = niagara.NiagaraSystemProfile().inspect(
            mode="live_asset", exact_class_path=niagara.SYSTEM_CLASS, exact_asset_object_path="/Niagara/X.X",
            editor_asset_handle=object(), context=context)
        self.assertEqual(context.calls[0]["detail"], "inspect")
        self.assertEqual(result["Target"], "/Niagara/X")
        self.assertIn("Modules", result["NiagaraDetails"])

    def test_inspect_summary_is_requested_by_the_canonical_document(self) -> None:
        # The HTTP inspect sends {"mode", "summary": true} unless detail=true; the runtime then calls the
        # profile's optional summarize_inspect.
        from types import SimpleNamespace
        from asset_document import runtime
        profile = niagara.NiagaraSystemProfile()
        full = profile.inspect(mode="live_asset", exact_class_path=niagara.SYSTEM_CLASS, exact_asset_object_path="/Niagara/X.X",
                               editor_asset_handle=object(), context=_Context(SYSTEM_ROWS))
        summary = SimpleNamespace(canonical_document={"mode": "live_asset", "summary": True})
        detail = SimpleNamespace(canonical_document={"mode": "live_asset"})
        self.assertEqual(runtime.summarized(profile, detail, full), full)
        summarized = runtime.summarized(profile, summary, full)
        self.assertEqual(summarized["NiagaraDetails"], shape.summarize(full["NiagaraDetails"]))
        # The shared summary (inspect_summary.py): one top-level Summary line; a small Body stays whole.
        self.assertEqual(summarized["Body"], full["Body"])
        self.assertIn("NiagaraDetails", summarized["Summary"])
        self.assertIn("detail true", summarized["Summary"])
        self.assertNotIn("Summary", summarized["NiagaraDetails"])
        for wrong in ({"mode": "live_asset", "summary": False}, {"mode": "live_asset", "extra": True}):
            with self.assertRaises(AssetDocumentContractError):
                runtime.inspect_summary(SimpleNamespace(canonical_document=wrong))

    def test_extract_diff_only_leaves_default_properties_out(self) -> None:
        # Extract with diff_only listed every Details value at its default.
        seen = []
        niagara.system_properties = lambda: _Recording(seen)
        profile = niagara.NiagaraSystemProfile()
        profile.extract(target=TARGET, asset_handle=object(), context=_Context(SYSTEM_ROWS))
        profile.extract(target=TARGET, asset_handle=object(), context=_Context(SYSTEM_ROWS), include_defaults=True)
        self.assertEqual(seen, [False, True])

    def test_verify_reads_every_property(self) -> None:
        # Verify / Diff read through extract_with_defaults; an authored value equal to its default
        # (an emitter's bIsInheritable true) must read back present, not be left out as diff_only does.
        seen = []
        niagara.system_properties = lambda: _Recording(seen)
        niagara.NiagaraSystemProfile().extract_with_defaults(target=TARGET, asset_handle=object(),
                                                             context=_Context(SYSTEM_ROWS))
        self.assertEqual(seen, [True])

    def test_primitive_failures_keep_their_code(self) -> None:
        with self.assertRaises(AssetDocumentContractError) as raised:
            niagara.NiagaraSystemProfile().extract(target=TARGET, asset_handle=object(), context=_Failing([]))
        self.assertEqual(raised.exception.diagnostic.code, "Niagara.InteractiveReadRefused")

    def test_every_profile_applies(self) -> None:
        for profile in (niagara.NiagaraSystemProfile(), niagara.NiagaraEmitterProfile(), niagara.NiagaraScriptProfile(),
                        niagara.NiagaraParameterCollectionProfile()):
            self.assertTrue(profile.inspect_profile()["ApplyAvailable"], profile.profile_id)
            document = profile.create_template("/Game/FX/X_Test")
            profile.validate(document)
            self.assertEqual(profile.create_target_context(document, None), ())

    def test_collection_diff_compares_present_body_keys(self) -> None:
        profile = niagara.NiagaraParameterCollectionProfile()
        live = profile.create_template("/Game/FX/NPC_Test")
        live["Body"] = {"Parameters": [{"Name": "A", "Type": "float", "Value": 1.0}]}
        authored = profile.create_template("/Game/FX/NPC_Test")
        authored["Body"] = {}
        self.assertEqual(profile.diff(authored, live), ())
        authored["Body"] = {"Parameters": [{"Name": "A", "Type": "float", "Value": 2.0}]}
        [change] = profile.diff(authored, live)
        self.assertEqual(change.path, "/Body/Parameters/0/Value")

    def test_registrations_cover_the_four_classes(self) -> None:
        classes = {registration.profile_meta.exact_asset_class_path for registration in niagara.NIAGARA_RUNTIME_REGISTRATIONS}
        self.assertEqual(classes, {niagara.SYSTEM_CLASS, niagara.EMITTER_CLASS, niagara.SCRIPT_CLASS, niagara.COLLECTION_CLASS})


def _live():
    return shape.system_body(SYSTEM_ROWS)


def _ops(plan):
    return [op for op, _ in plan.ops]


def _module(name, script=None, **fields):
    return {"Name": name, "Script": script or f"/S/{name}.{name}", **fields}


class PlannerTests(unittest.TestCase):
    """System-level Apply planning (niagara_apply): the edits from a document and the live system's extract."""

    def test_unchanged_body_plans_nothing(self) -> None:
        plan = planner.plan_system(_live(), _live(), {})
        self.assertEqual((plan.ops, plan.refusals, plan.differences), ([], [], []))
        self.assertEqual(planner.plan_system({}, _live(), {}).differences, [])

    def test_user_parameters_are_a_complete_list(self) -> None:
        body = {"UserParameters": [
            {"Name": "User.Total", "RenamedFrom": "User.Count", "Type": "int32", "Value": 60, "Category": "Spawn"},
            {"Name": "User.Size", "Type": "float", "Value": 2.5, "Description": "Sprite size"}]}
        plan = planner.plan_system(body, _live(), {})
        self.assertEqual(_ops(plan), [
            {"op": "delete_user_parameter", "name": "User.Mesh"},
            {"op": "rename_user_parameter", "name": "User.Count", "to": "User.Total"},
            {"op": "user_parameter", "name": "User.Total", "value": 60},
            {"op": "user_parameter", "name": "User.Total", "category": "Spawn"},
            {"op": "add_user_parameter", "name": "User.Size", "type": "float"}])
        self.assertEqual([path for _, path in plan.ops][1:4], ["/Body/UserParameters/0/Name",
                                                                "/Body/UserParameters/0/Value",
                                                                "/Body/UserParameters/0/Category"])
        # The pass after the add sets the new parameter's value and description.
        live = {"UserParameters": [{"Name": "User.Total", "Type": "int32", "Value": 60, "Category": "Spawn"},
                                   {"Name": "User.Size", "Type": "float", "Value": 0.0}]}
        self.assertEqual(_ops(planner.plan_system(body, live, {})), [
            {"op": "user_parameter", "name": "User.Size", "value": 2.5},
            {"op": "user_parameter", "name": "User.Size", "description": "Sprite size"}])

    def test_renames_apply_once(self) -> None:
        body = {"UserParameters": [{"Name": "User.Total", "RenamedFrom": "User.Count", "Type": "int32", "Value": 50}]}
        live = {"UserParameters": [{"Name": "User.Total", "Type": "int32", "Value": 50},
                                   {"Name": "User.Count", "Type": "int32", "Value": 1}]}
        # The system already has the new name: matched by name, the old one is deleted.
        self.assertEqual(_ops(planner.plan_system(body, live, {})), [{"op": "delete_user_parameter", "name": "User.Count"}])
        swap = {"UserParameters": [{"Name": "User.Mesh", "RenamedFrom": "User.Count", "Type": "int32", "Value": 50},
                                   {"Name": "User.Count", "RenamedFrom": "User.Mesh", "Type": "int32", "Value": 1}]}
        plan = planner.plan_system(swap, {"UserParameters": [{"Name": "User.Count", "Type": "int32", "Value": 50},
                                                             {"Name": "User.Mesh", "Type": "int32", "Value": 1}]}, {})
        self.assertEqual([op["op"] for op in _ops(plan)], ["user_parameter", "user_parameter"])

    def test_user_parameter_type_cannot_change(self) -> None:
        body = {"UserParameters": [{"Name": "User.Count", "Type": "float"}]}
        plan = planner.plan_system(body, {"UserParameters": [{"Name": "User.Count", "Type": "int32", "Value": 1}]}, {})
        self.assertEqual(plan.refusals[0].diagnostic.code, "Niagara.UserParameterRetype")
        self.assertEqual(plan.refusals[0].diagnostic.path, "/Body/UserParameters/0/Type")

    def test_data_interface_properties_are_complete(self) -> None:
        live = {"UserParameters": [{"Name": "User.Curve", "Type": "DataInterface:Curve", "Value": {
            "DataInterface": {"Class": "Curve", "Properties": {"bUseLUT": False, "ShowInCurveEditor": True}}}}]}
        body = {"UserParameters": [{"Name": "User.Curve", "Type": "DataInterface:Curve", "Value": {
            "DataInterface": {"Class": "Curve", "Properties": {"bUseLUT": False, "Curve": {"Keys": []}}}}}]}
        defaults = {"data_interfaces": {"Curve": {"Curve": {"Keys": []}}}}
        self.assertEqual(_ops(planner.plan_system(body, live, defaults)), [
            {"op": "user_parameter", "name": "User.Curve", "value": {"DataInterface": {"Properties": {}, "Class": "Curve"}},
             "reset": ["ShowInCurveEditor"]}])

    def test_emitters_rename_delete_add_and_keep_their_order(self) -> None:
        live = {"Emitters": [{"Name": "A", "Properties": {}, "Renderers": []},
                             {"Name": "B", "Properties": {}, "Renderers": []}]}
        body = {"Emitters": [{"Name": "B2", "RenamedFrom": "B"},
                             {"Name": "Fountain", "From": "/Niagara/DefaultAssets/Templates/Emitters/Fountain"},
                             {"Name": "Blank", "From": "Minimal"}, {"Name": "Plain"}]}
        plan = planner.plan_system(body, live, {})
        self.assertEqual(_ops(plan), [
            {"op": "delete_emitter", "name": "A"},
            {"op": "rename_emitter", "name": "B", "to": "B2"},
            {"op": "add_emitter", "name": "Fountain", "from": {
                "kind": "asset", "emitter": "/Niagara/DefaultAssets/Templates/Emitters/Fountain.Fountain"}},
            {"op": "add_emitter", "name": "Blank", "from": {"kind": "minimal"}},
            {"op": "add_emitter", "name": "Plain", "from": {"kind": "empty"}}])
        self.assertEqual(plan.refusals, [])
        reordered = {"Emitters": [{"Name": "B"}, {"Name": "A"}]}
        plan = planner.plan_system(reordered, live, {})
        self.assertEqual(plan.refusals[0].diagnostic.code, "Niagara.EmitterOrder")

    def test_new_emitter_defaults_to_its_parent(self) -> None:
        parent = {"Emitter": "/Game/FX/NE_Base.NE_Base"}
        plan = planner.plan_system({"Emitters": [{"Name": "Child", "Parent": parent}]}, {"Emitters": []}, {})
        # inherit: the editor makes the asset the parent only when it is inheritable (Niagara.NotInheritable otherwise).
        self.assertEqual(_ops(plan), [{"op": "add_emitter", "name": "Child", "from": {
            "kind": "asset", "emitter": "/Game/FX/NE_Base.NE_Base", "inherit": True}}])
        self.assertEqual(planner.op_path("/ops/0/from/inherit", ["/Body/Emitters/0"]), "/Body/Emitters/0/Parent")
        plan = planner.plan_system({"Emitters": [{"Name": "Child", "Parent": parent, "From": "Empty"}]}, {"Emitters": []}, {})
        self.assertEqual(plan.refusals[0].diagnostic.code, "Niagara.ParentImmutable")
        self.assertIn("Is Inheritable", plan.refusals[0].diagnostic.message)
        # A copy From an asset without Parent does not ask to inherit.
        plan = planner.plan_system({"Emitters": [{"Name": "Copy", "From": parent}]}, {"Emitters": []}, {})
        self.assertNotIn("inherit", _ops(plan)[0]["from"])

    def test_emitter_details(self) -> None:
        live = {"Emitters": [{"Name": "Sparks", "Parent": {"Emitter": "/Game/FX/NE_Base.NE_Base"},
                              "Properties": {"SimTarget": "GPUComputeSim", "bLocalSpace": True}, "Renderers": []}]}
        body = {"Emitters": [{"Name": "Sparks", "Enabled": False, "Properties": {"SimTarget": "GPUComputeSim",
                                                                                 "RandomSeed": 0, "MaxDeltaTime": 0.1}}]}
        plan = planner.plan_system(body, live, {"emitter": {"RandomSeed": 0}})
        self.assertEqual(_ops(plan), [
            {"op": "emitter", "name": "Sparks", "remove_parent": True},
            {"op": "emitter", "name": "Sparks", "properties": {"MaxDeltaTime": 0.1}, "reset": ["bLocalSpace"]},
            {"op": "emitter", "name": "Sparks", "enabled": False}])
        body["Emitters"][0]["Parent"] = {"Emitter": "/Game/FX/NE_Other"}
        plan = planner.plan_system(body, live, {})
        self.assertEqual(plan.refusals[0].diagnostic.code, "Niagara.ParentImmutable")
        # The refusal says which assets can be parents (the Fountain template cannot).
        self.assertIn("inheritable emitter asset", plan.refusals[0].diagnostic.message)
        self.assertIn("Fountain", plan.refusals[0].diagnostic.message)

    def test_renderers_are_a_complete_list(self) -> None:
        live = {"Emitters": [{"Name": "E", "Properties": {}, "Renderers": [
            {"Name": "Sprite", "Class": "SpriteRenderer", "Properties": {"Alignment": "VelocityAligned"}},
            {"Name": "Ribbon", "Class": "RibbonRenderer"},
            {"Name": "Old", "Class": "LightRenderer"}]}]}
        body = {"Emitters": [{"Name": "E", "Renderers": [
            {"Name": "Mesh", "Class": "MeshRenderer"},
            {"Name": "Ribbon", "Class": "SpriteRenderer"},
            {"Name": "Sprite", "Class": "SpriteRenderer", "Enabled": False, "Properties": {}}]}]}
        self.assertEqual(_ops(planner.plan_system(body, live, {})), [
            {"op": "delete_renderer", "emitter": "E", "name": "Ribbon"},
            {"op": "delete_renderer", "emitter": "E", "name": "Old"},
            {"op": "add_renderer", "emitter": "E", "name": "Mesh", "class": "MeshRenderer"},
            {"op": "add_renderer", "emitter": "E", "name": "Ribbon", "class": "SpriteRenderer"},
            {"op": "order_renderers", "emitter": "E", "names": ["Mesh", "Ribbon", "Sprite"]},
            {"op": "renderer", "emitter": "E", "name": "Sprite", "enabled": False},
            {"op": "renderer", "emitter": "E", "name": "Sprite", "properties": {}, "reset": ["Alignment"]}])

    def test_module_lists_delete_move_add_and_edit(self) -> None:
        live = {"SystemUpdate": [_module("A"), _module("B"), _module("C")], "SystemSpawn": [_module("X")]}
        body = {"SystemUpdate": [_module("C"), _module("X"), {"Name": "New", "Script": "/S/New", "Inputs": {"Rate": 2}},
                                 {**_module("A"), "Enabled": False, "DisplayName": "Alpha", "Note": {"Header": "h", "Body": "b"}}]}
        plan = planner.plan_system(body, live, {})
        self.assertEqual(plan.refusals, [])
        self.assertEqual(_ops(plan), [
            {"op": "delete_module", "group": "SystemUpdate", "name": "B"},
            {"op": "move_module", "group": "SystemUpdate", "name": "C", "after": None},
            # X moves in from SystemSpawn (a name in another stack of the same graph is a move).
            {"op": "move_module", "group": "SystemUpdate", "name": "X", "after": "C"},
            {"op": "add_module", "group": "SystemUpdate", "name": "New", "script": "/S/New.New", "after": "X"},
            {"op": "module", "group": "SystemUpdate", "name": "A", "enabled": False, "display_name": "Alpha",
             "note": {"Header": "h", "Body": "b"}}])
        # Adds go last entry first, each after the nearest kept module, so consecutive new modules keep their order.
        plan = planner.plan_system({"SystemUpdate": [{"Name": "N1", "Script": "/S/N1"}, {"Name": "N2", "Script": "/S/N2"},
                                                     _module("A")]}, {"SystemUpdate": [_module("A")]}, {})
        self.assertEqual([(op["op"], op.get("name"), op["after"]) for op in _ops(plan)],
                         [("add_module", "N2", None), ("add_module", "N1", None)])
        self.assertEqual(planner.plan_system(live, live, {}).ops, [])
        # A different script under the same name replaces the module.
        plan = planner.plan_system({"SystemUpdate": [_module("A", "/S/Other.Other")]}, {"SystemUpdate": [_module("A")]}, {})
        self.assertEqual([op["op"] for op in _ops(plan)], ["delete_module", "add_module"])

    def test_set_parameters_pair_by_position(self) -> None:
        live = {"SystemUpdate": [{"Name": "SetVariables_1", "Assign": [{"Target": "System.A", "Type": "float"}],
                                  "Inputs": {"System.A": 1.0}}, _module("B")]}
        body = {"SystemUpdate": [_module("B"),
                                 {"Name": "Mine", "Assign": [{"Target": "System.A", "Type": "float"},
                                                             {"Target": "System.B", "Type": "int32"}]},
                                 {"Name": "Two", "Assign": [{"Target": "System.C", "Type": "float"}]}]}
        self.assertEqual(_ops(planner.plan_system(body, live, {})), [
            {"op": "move_module", "group": "SystemUpdate", "name": "B", "after": None},
            {"op": "add_assignment", "group": "SystemUpdate", "after": "SetVariables_1",
             "targets": [{"name": "System.C", "type": "float"}]},
            {"op": "assignment", "group": "SystemUpdate", "name": "SetVariables_1", "add": [{"name": "System.B", "type": "int32"}]}])
        # The next pass pairs the unnamed entries with the editor-named modules, in order.
        live = {"SystemUpdate": [_module("B"),
                                 {"Name": "SetVariables_1", "Assign": body["SystemUpdate"][1]["Assign"], "Inputs": {"System.A": 1.0}},
                                 {"Name": "SetVariables_2", "Assign": body["SystemUpdate"][2]["Assign"]}]}
        self.assertEqual(_ops(planner.plan_system(body, live, {})), [
            {"op": "inputs", "group": "SystemUpdate", "module": "SetVariables_1", "reset": ["System.A"]}])

    def test_inputs_are_complete(self) -> None:
        live = {"SystemUpdate": [_module("A", Inputs={
            "X": 1.0, "Y": {"Link": "User.Y"}, "Z": {"Dynamic": {"Script": "/D/U.U", "Inputs": {"Min": 1}}},
            "Q": {"Unsupported": "DefaultFunction"}, "Gone": 3})]}
        body = {"SystemUpdate": [_module("A", Inputs={
            "X": 1.0, "Y": {"Link": "User.Z"}, "Z": {"Dynamic": {"Script": "/D/U", "Inputs": {"Min": 1}}},
            "W": {"DataInterface": {"Class": "Curve", "Properties": {}}}, "Q": {"Unsupported": "DefaultFunction"}})]}
        plan = planner.plan_system(body, live, {})
        self.assertEqual(_ops(plan), [{"op": "inputs", "group": "SystemUpdate", "module": "A", "set": {
            "Y": {"Link": "User.Z"}, "W": {"DataInterface": {"Class": "Curve", "Properties": {}}}}, "reset": ["Gone"]}])
        self.assertEqual(sorted(change.path for change in plan.differences), [
            "/Body/SystemUpdate/0/Inputs/Gone", "/Body/SystemUpdate/0/Inputs/W", "/Body/SystemUpdate/0/Inputs/Y"])
        body["SystemUpdate"][0]["Inputs"]["Q"] = {"Unsupported": "InvalidOverride"}
        self.assertEqual(planner.plan_system(body, live, {}).refusals[0].diagnostic.code, "Niagara.UnsupportedInputValue")
        # A version change converts the inputs: they are planned in the next pass.
        body["SystemUpdate"][0]["Version"] = "v2"
        self.assertEqual(_ops(planner.plan_system(body, live, {}))[0],
                         {"op": "module", "group": "SystemUpdate", "name": "A", "version": "v2"})

    def test_enum_inputs_compare_by_their_short_name(self) -> None:
        # "Kill" is written for "Kill (Emitter and Particles Die Immediately)"; the read-back full name is
        # the same value (no op, no difference), another value is not.
        full = "Kill (Emitter and Particles Die Immediately)"
        live = {"SystemUpdate": [_module("A", Inputs={"Inactive Response": full})]}
        for written in ("Kill", "kill", full):
            with self.subTest(written=written):
                plan = planner.plan_system({"SystemUpdate": [_module("A", Inputs={"Inactive Response": written})]}, live, {})
                self.assertEqual((plan.ops, plan.differences), ([], []))
        for written in ("Complete", "Kill (", "Kil"):
            with self.subTest(written=written):
                plan = planner.plan_system({"SystemUpdate": [_module("A", Inputs={"Inactive Response": written})]}, live, {})
                self.assertEqual(len(plan.differences), 1)
        self.assertEqual(niagara_stack.enum_short_name("Once"), "Once")
        self.assertEqual(niagara_stack.enum_short_name(full), "Kill")

    def test_event_handlers_by_position_and_stages_by_name(self) -> None:
        live = {"Emitters": [{"Name": "E", "Properties": {}, "Renderers": [],
                              "EventHandlers": [{"Properties": {"SpawnNumber": 2}, "Modules": []}, {"Properties": {}, "Modules": []}],
                              "SimulationStages": [{"Name": "S1", "Class": "Generic", "Modules": [_module("M1")]},
                                                   {"Name": "S2", "Class": "Generic", "Modules": []}]}]}
        body = {"Emitters": [{"Name": "E", "EventHandlers": [{"Properties": {"SpawnNumber": 3, "SourceEmitter": "F"},
                                                              "Modules": [_module("K")]}],
                              "SimulationStages": [{"Name": "S2", "Class": "Generic", "Enabled": False, "Modules": [_module("M1")]},
                                                   {"Name": "S3", "Class": "Generic"},
                                                   {"Name": "S1", "Class": "Generic", "Modules": []}]}]}
        self.assertEqual(_ops(planner.plan_system(body, live, {"event_handler": {"SpawnNumber": 0}})), [
            {"op": "delete_event_handler", "emitter": "E", "event": 1},
            {"op": "event_handler", "emitter": "E", "event": 0, "properties": {"SpawnNumber": 3, "SourceEmitter": "F"}},
            {"op": "move_stage", "emitter": "E", "stage": "S2", "after": None},
            {"op": "add_stage", "emitter": "E", "name": "S3", "class": "Generic", "after": "S2"},
            {"op": "stage", "emitter": "E", "stage": "S2", "enabled": False},
            {"op": "move_module", "emitter": "E", "group": "SimulationStage", "stage": "S2", "name": "M1", "after": None},
            {"op": "add_module", "emitter": "E", "group": "EventHandler", "event": 0, "name": "K", "script": "/S/K.K",
             "after": None}])
        plan = planner.plan_system({"Emitters": [{"Name": "E", "EventHandlers": [{}, {}, {}]}]}, live, {})
        self.assertEqual(_ops(plan), [{"op": "add_event_handler", "emitter": "E"}])

    def test_lightweight_emitters(self) -> None:
        live = {"Emitters": [{"Name": "L", "Mode": "Stateless", "Stateless": {
            "Properties": {"RandomSeed": 3}, "Modules": [{"Class": "ShapeLocation", "Properties": {"bModuleEnabled": False}},
                                                          {"Class": "InitializeParticle", "Properties": {}}],
            "Renderers": [{"Name": "Renderer", "Class": "SpriteRenderer", "Properties": {}}]}}]}
        body = {"Emitters": [{"Name": "L", "Mode": "Stateless", "Stateless": {
            "Properties": {}, "Modules": [{"Class": "InitializeParticle", "Properties": {"Lifetime": 2}}],
            "Renderers": [{"Name": "Renderer", "Class": "SpriteRenderer", "Enabled": False, "Properties": {}}]}}]}
        self.assertEqual(_ops(planner.plan_system(body, live, {})), [
            {"op": "stateless", "emitter": "L", "properties": {}, "reset": ["RandomSeed"]},
            {"op": "stateless_module", "emitter": "L", "class": "InitializeParticle", "properties": {"Lifetime": 2}},
            {"op": "stateless_module", "emitter": "L", "class": "ShapeLocation", "properties": {}, "reset": ["bModuleEnabled"]},
            {"op": "renderer", "emitter": "L", "name": "Renderer", "enabled": False}])
        plan = planner.plan_system({"Emitters": [{"Name": "N", "Mode": "Stateless"}]}, {"Emitters": []}, {})
        self.assertEqual(_ops(plan), [{"op": "add_emitter", "name": "N", "from": {"kind": "stateless"}}])
        plan = planner.plan_system({"Emitters": [{"Name": "L", "Properties": {}}]}, live, {})
        self.assertEqual(plan.refusals[0].diagnostic.code, "Niagara.ModeChange")
        body["Emitters"][0]["Stateless"]["Modules"] = [{"Class": "Missing"}]
        self.assertEqual(planner.plan_system(body, live, {}).refusals[0].diagnostic.code, "Niagara.StatelessModuleNotFound")

    def test_scratch_pad_modules_plan_like_other_modules(self) -> None:
        live = _live()
        body = _live()
        body["Emitters"][0]["SimulationStages"][0]["Modules"][0]["Enabled"] = False
        plan = planner.plan_system(body, live, {})
        self.assertEqual((plan.refusals, len(plan.ops)), ([], 1))
        body = _live()
        body["Emitters"][0]["SimulationStages"][0]["Modules"] = []
        plan = planner.plan_system(body, live, {})
        self.assertEqual([op["op"] for op in _ops(plan)], ["delete_module"])

    def test_scratch_pad_differences_have_no_stack_op(self) -> None:
        live = _live()
        body = _live()
        body["ScratchPads"] = []
        plan = planner.plan_system(body, live, {})
        self.assertEqual(plan.ops, [])
        self.assertEqual([(item.path, item.kind) for item in plan.differences],
                         [("/Body/ScratchPads", DiffKind.REMOVE)])
        body = _live()
        body["Emitters"][0]["ScratchPads"][0]["Graph"]["PinDefaults"] = {}
        plan = planner.plan_system(body, live, {})
        self.assertEqual([item.path for item in plan.differences],
                         ["/Body/Emitters/0/ScratchPads/0/Graph/PinDefaults/NiagaraNodeOutput_0:Output"])

    def test_declared_parameters_and_parent_version(self) -> None:
        live = {"Parameters": [{"Name": "System.A", "Type": "float"}, {"Name": "System.B", "Type": "int32"}],
                "Emitters": [{"Name": "E", "Parent": {"Emitter": "/Game/FX/NE_Base.NE_Base", "Version": "v1"},
                              "Properties": {}, "Renderers": []}]}
        body = {"Parameters": [{"Name": "System.A", "Type": "float", "Description": "a"}, {"Name": "System.B", "Type": "float"}],
                "Emitters": [{"Name": "E", "Parent": {"Emitter": "/Game/FX/NE_Base"}}]}
        self.assertEqual(_ops(planner.plan_system(body, live, {})), [
            {"op": "parent_version", "emitter": "E", "version": ""},
            {"op": "delete_parameter", "name": "System.B"},
            {"op": "parameter", "name": "System.A", "description": "a"},
            {"op": "add_parameter", "name": "System.B", "type": "float"}])

    def test_body_checks(self) -> None:
        for body, code in (({"UserParameters": [{"Name": "Count", "Type": "int32"}]}, "Niagara.InvalidUserParameterName"),
                           ({"UserParameters": [{"Name": "User.A", "Type": "int32", "Colour": 1}]}, "Niagara.UnknownKey"),
                           ({"Emitters": [{"Name": "A"}, {"Name": "A"}]}, "Niagara.DuplicateName"),
                           ({"Emitters": [{"Name": "A", "Renderers": [{"Name": "R"}]}]}, "Niagara.InvalidBody"),
                           ({"SystemUpdate": [{"Name": "A", "Script": "/S/A"}], "SystemSpawn": [{"Name": "A", "Script": "/S/A"}]},
                            "Niagara.DuplicateName"),
                           ({"SystemUpdate": [{"Name": "A"}]}, "Niagara.InvalidBody"),
                           ({"SystemUpdate": [{"Name": "A", "Script": "/S/A", "Inputs": {"X": {"Dynamic": {}}}}]},
                            "Niagara.InvalidBody"),
                           # A link to a user parameter the complete UserParameters list leaves out (the editor's
                           # link would add it).
                           ({"UserParameters": [], "Emitters": [{"Name": "E", "ParticleSpawn": [
                               {"Name": "A", "Script": "/S/A", "Inputs": {"X": {"Dynamic": {
                                   "Script": "/D/U", "Inputs": {"Min": {"Link": "User.Min"}}}}}}]}]},
                            "Niagara.UndeclaredUserParameter")):
            with self.assertRaises(AssetDocumentContractError) as raised:
                planner.check_body(body)
            self.assertEqual(raised.exception.diagnostic.code, code)
        planner.check_body({"UserParameters": [{"Name": "User.Min", "Type": "float"}], "SystemUpdate": [
            {"Name": "A", "Script": "/S/A", "Inputs": {"X": {"Link": "User.Min"}, "Y": {"Link": "Emitter.Age"}}}]})
        self.assertEqual(raised.exception.diagnostic.path,
                         "/Body/Emitters/0/ParticleSpawn/0/Inputs/X/Dynamic/Inputs/Min/Link")

    def test_primitive_paths_map_to_document_paths(self) -> None:
        paths = ["/Body/Emitters/0/Properties", "/Body/Emitters/1"]
        self.assertEqual(planner.op_path("/ops/0/properties/SimTarget", paths), "/Body/Emitters/0/Properties/SimTarget")
        self.assertEqual(planner.op_path("ops/1/from/emitter", paths), "/Body/Emitters/1")
        self.assertEqual(planner.op_path("/", paths), "/Body")
        self.assertEqual(planner.op_path("/ops/0/set/Lifetime/Dynamic/Inputs/Minimum", ["/Body/SystemUpdate/0/Inputs"]),
                         "/Body/SystemUpdate/0/Inputs/Lifetime/Dynamic/Inputs/Minimum")

    def test_diff_reports_the_planned_differences(self) -> None:
        profile = niagara.NiagaraSystemProfile()
        live = profile.create_template(TARGET)
        live["Body"] = _live()
        authored = profile.create_template(TARGET)
        authored["Body"] = {"Emitters": [{**_live()["Emitters"][0], "Enabled": False}]}
        [change] = profile.diff(authored, live)
        self.assertEqual((change.path, change.kind), ("/Body/Emitters/0/Enabled", DiffKind.REPLACE))
        authored["Body"] = {}
        self.assertEqual(profile.diff(authored, live), ())


EMITTER_ROWS = [
    {"kind": "system"},
    {"kind": "emitter_asset", "name": "NE_Sparks", "new": False, "versions": [
        {"Number": "1.0", "Guid": "G1", "Exposed": True, "ChangeDescription": "", "Deprecated": False,
         "DeprecationMessage": "", "Visible": True}]},
    {"kind": "emitter", "name": "NE_Sparks", "enabled": True, "mode": "Standard", "properties": {"SimTarget": "GPUComputeSim"}},
    {"kind": "renderer", "emitter": "NE_Sparks", "name": "Sprite", "class": "SpriteRenderer", "enabled": True,
     "properties": {}},
]


class EmitterAssetTests(unittest.TestCase):
    def live(self):
        return shape.system_body(EMITTER_ROWS)

    def test_body_check(self) -> None:
        niagara.check_emitter_asset_body({"Properties": {}, "Versions": [{"Number": "1.0", "Exposed": True}],
                                          "From": "/Game/FX/NE_A", "EditExposedVersion": True})
        for body, code in (({"Stateless": {}}, "Niagara.StatelessEmitter"), ({"Name": "X"}, "Niagara.UnknownKey"),
                           ({"From": "/Game/A", "Parent": "/Game/B"}, "Niagara.InvalidBody"),
                           ({"Versions": [{"Number": "1.0", "Exposed": True}, {"Number": "1.1", "Exposed": True}]},
                            "Niagara.InvalidBody"),
                           ({"Renderers": [{"Name": "A"}]}, "Niagara.InvalidBody")):
            with self.assertRaises(AssetDocumentContractError) as raised:
                niagara.check_emitter_asset_body(body)
            self.assertEqual(raised.exception.diagnostic.code, code, body)

    def test_extract_shape_and_round_trip(self) -> None:
        body = shape.emitter_asset_body(EMITTER_ROWS)
        self.assertNotIn("Name", body)
        self.assertEqual(body["Versions"][0]["Number"], "1.0")
        plan = niagara.plan_emitter_asset(body, "NE_Sparks", self.live(), niagara.cached_defaults())
        self.assertEqual((plan.ops, plan.differences, plan.refusals), ([], [], []))

    def test_ops_use_the_handle_name_and_document_paths(self) -> None:
        body = {**shape.emitter_asset_body(EMITTER_ROWS), "Properties": {"SimTarget": "CPUSim"}}
        plan = niagara.plan_emitter_asset(body, "NE_Sparks", self.live(), {"emitter": {}})
        [(op, path)] = plan.ops
        self.assertEqual((op["op"], op["name"], path), ("emitter", "NE_Sparks", "/Body/Properties"))
        self.assertEqual([item.path for item in plan.differences], ["/Body/Properties"])

    def test_refusals_are_rebased(self) -> None:
        body = {"Parent": "/Game/FX/NE_Other"}
        plan = niagara.plan_emitter_asset(body, "NE_Sparks", self.live(), {})
        self.assertEqual([(item.diagnostic.path, item.diagnostic.code) for item in plan.refusals],
                         [("/Body/Parent", "Niagara.ParentImmutable")])

    def test_initialize_only_on_a_new_asset(self) -> None:
        row = {"name": "NE_New", "new": True}
        live = {"Emitters": [{"Name": "NE_New"}]}
        op, path = niagara._initialize_op({"Parent": {"Emitter": "/Game/FX/NE_A", "Version": "V"}}, row, live)
        self.assertEqual((op, path), ({"op": "initialize_emitter", "emitter": "/Game/FX/NE_A.NE_A", "inherit": True,
                                       "version": "V"}, "/Body/Parent"))
        op, _ = niagara._initialize_op({"From": "/Game/FX/NE_A.NE_A"}, row, live)
        self.assertFalse(op["inherit"])
        self.assertIsNone(niagara._initialize_op({"From": "/Game/FX/NE_A"}, {**row, "new": False}, live))
        self.assertIsNone(niagara._initialize_op({"Parent": "/Game/FX/NE_A"}, row,
                                                 {"Emitters": [{"Name": "NE_New", "Parent": {"Emitter": "/Game/FX/NE_A"}}]}))

    def test_diff_includes_versions(self) -> None:
        profile = niagara.NiagaraEmitterProfile()
        live = profile.create_template("/Game/FX/NE_Sparks")
        live["Body"] = shape.emitter_asset_body(EMITTER_ROWS)
        authored = profile.create_template("/Game/FX/NE_Sparks")
        authored["Body"] = {"Versions": [{"Number": "1.0", "Exposed": True}, {"Number": "1.1"}]}
        self.assertTrue(any(item.path.startswith("/Body/Versions") for item in profile.diff(authored, live)))
        authored["Body"] = {"Renderers": live["Body"]["Renderers"], "Properties": live["Body"]["Properties"]}
        self.assertEqual(profile.diff(authored, live), ())


class CollectionTests(unittest.TestCase):
    LIVE = {"Parameters": [{"Name": "A", "Type": "float", "Value": 1.0},
                           {"Name": "B", "Type": "int32", "Value": 3},
                           {"Name": "M", "Type": "LinearColor", "Value": {"R": 0.0}, "MaterialCollection": True}]}

    def plan(self, parameters):
        return niagara_collection.plan_collection({"Parameters": parameters}, self.LIVE)

    def test_body_check(self) -> None:
        for parameters, code in (([{"Name": "NPC.X.A", "Type": "float"}], "Niagara.InvalidParameterName"),
                                 ([{"Name": "A", "Type": "float"}, {"Name": "A", "Type": "int32"}], "Niagara.DuplicateName"),
                                 ([{"Name": "A"}], "Niagara.InvalidBody"),
                                 ([{"Name": "A", "Type": "float", "Default": 1}], "Niagara.UnknownKey")):
            with self.assertRaises(AssetDocumentContractError) as raised:
                niagara_collection.check_body({"Parameters": parameters})
            self.assertEqual(raised.exception.diagnostic.code, code)

    def test_no_op(self) -> None:
        plan = self.plan(self.LIVE["Parameters"])
        self.assertEqual((plan.ops, plan.differences), ([], []))

    def test_ops_are_ordered(self) -> None:
        plan = self.plan([{"Name": "A", "Type": "float"}, {"Name": "C", "RenamedFrom": "B", "Type": "float", "Value": 2.0},
                          {"Name": "B", "Type": "Vector"}])
        self.assertEqual([op for op, _ in plan.ops], [
            {"op": "rename_parameter", "name": "B", "to": "C"},
            {"op": "add_parameter", "name": "B", "type": "Vector"},
            {"op": "retype_parameter", "name": "C", "type": "float"}])

    def test_unlisted_material_parameters_stay_and_changes_are_refused(self) -> None:
        plan = self.plan([{"Name": "A", "Type": "float", "Value": 1.0}, {"Name": "B", "Type": "int32"}])
        self.assertEqual(plan.ops, [])
        plan = self.plan([{"Name": "M", "Type": "LinearColor", "Value": {"R": 1.0}}])
        self.assertEqual([item.diagnostic.code for item in plan.refusals], ["Niagara.CollectionParameterOwned"])
        self.assertEqual([op["op"] for op, _ in plan.ops], ["delete_parameter", "delete_parameter"])

    def test_data_interface_values_are_sparse(self) -> None:
        live = {"Parameters": [{"Name": "Curve", "Type": "NiagaraDataInterfaceCurve",
                                "Value": {"DataInterface": {"Class": "Curve", "Properties": {"bUseLUT": False, "X": 1}}}}]}
        authored = {"Parameters": [{"Name": "Curve", "Type": "NiagaraDataInterfaceCurve",
                                    "Value": {"DataInterface": {"Class": "Curve", "Properties": {"bUseLUT": True}}}}]}
        [(op, path)] = niagara_collection.plan_collection(authored, live, {"Curve": {"bUseLUT": False}}).ops
        self.assertEqual(op, {"op": "parameter", "name": "Curve", "reset": ["X"],
                              "value": {"DataInterface": {"Class": "Curve", "Properties": {"bUseLUT": True}}}})
        self.assertEqual(path, "/Body/Parameters/0/Value")


class CompileOutcomeTests(unittest.TestCase):
    def test_new_errors_fail_and_old_ones_warn(self) -> None:
        failure, _ = niagara.compile_outcome((["old"], []), {"errors": ["old", "new"]})
        self.assertEqual(failure.diagnostic.code, "Niagara.CompileFailed")
        self.assertIn("new", failure.diagnostic.message)
        failure, warnings = niagara.compile_outcome((["old"], ["w0"]), {"errors": ["old"], "warnings": ["w0", "w1"],
                                                                       "timed_out": True})
        self.assertIsNone(failure)
        self.assertEqual([item.code for item in warnings], ["Niagara.CompileError", "Niagara.CompileWarning",
                                                            "Niagara.CompileTimeout"])


if __name__ == "__main__":
    unittest.main()
