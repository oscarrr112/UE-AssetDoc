# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""MetaSound Profiles: document validation, node keys, assembling the Body from the audio.metasound.read.v1
records, Diff and the Apply planner (ops, refusals, validation outcome).  The editor side (normalised read, literal
codec, catalog, stability on the fidelity corpus) is covered by UEAssetDoc.MetaSound.*."""
from __future__ import annotations

import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profiles import metasound, metasound_apply
from asset_document.regions.primitive_catalog import (
    NamedOpaqueHandleResult, PrimitiveOutcome, PrimitiveOutcomeStatus,
)

TARGET = "/Game/Audio/MSS_Test"


def _header(**changes):
    header = {"kind": "header", "class": metasound.SOURCE_CLASS, "preset": False, "preset_parent": "", "inherits": [],
              "interfaces": [{"name": "UE.Source", "version": "1.0", "modifiable": False, "registered": True},
                             {"name": "UE.Source.OneShot", "version": "1.0", "modifiable": True, "registered": True},
                             {"name": "UE.OutputFormat.Mono", "version": "1.0", "modifiable": False, "registered": True}],
              "metadata": {"display_name": "", "description": "Blip", "author": "Me", "keywords": [], "categories": [],
                           "referenceable": True, "deprecated": False},
              "pages": [{"id": "0" * 32, "name": "Default"}], "build_page": "Default", "injected": ["N3"],
              "configured_pages": ["Default"]}
    header.update(changes)
    return header


def _input(name, type_name, value, interface="", interface_def=None, **changes):
    row = {"kind": "input", "name": name, "type": type_name, "constructor": False, "display_name": "",
           "description": "", "sort_order": 0, "advanced": False, "node_id": f"H_{name}", "interface": interface,
           "defaults": [{"page": "Default", "value": value}], "widget": {}}
    if interface_def is not None:
        row["interface_def"] = interface_def
    row.update(changes)
    return row


def _node(node_id, cls, class_name, member="", location=(0, 0), inputs=(), outputs=("Out",), derived=False, **extra):
    row = {"kind": "node", "page": "Default", "id": node_id, "class": cls, "class_name": class_name,
           "class_type": "External", "major": 1, "minor": 0, "member": member, "inputs": list(inputs),
           "outputs": list(outputs), "defaults": {}, "comment": "", "comment_visible": False,
           "hide_unconnected": False, "derived": derived}
    if location is not None:
        row["location"] = list(location)
    row.update(extra)
    return row


def _edge(source, source_pin, target, target_pin, derived=False):
    return {"kind": "edge", "page": "Default", "from": source, "from_pin": source_pin, "to": target, "to_pin": target_pin,
            "derived": derived}


RECORDS = [
    _header(),
    _input("UE.Source.OnPlay", "Trigger", None, "UE.Source", {"sort_order": 0, "advanced": False}),
    _input("Pitch", "Float", 0.5, sort_order=1, widget={"WidgetType": "Slider"}),
    {"kind": "output", "name": "UE.OutputFormat.Mono.Audio:0", "type": "Audio", "constructor": False, "display_name": "",
     "description": "", "sort_order": 0, "advanced": False, "node_id": "O1", "interface": "UE.OutputFormat.Mono",
     "interface_def": {"sort_order": 0, "advanced": False}},
    {"kind": "variable", "page": "Default", "name": "Count", "type": "Int32", "default": 3, "display_name": "",
     "description": "", "id": "V1"},
    _node("H1", "Input", "UE.Pitch.Float", member="Pitch", location=None, derived=True),
    _node("N1", "UE.Sine.Audio", "UE.Sine.Audio", inputs=("Frequency", "Phase Offset"), outputs=("Audio",),
          location=(-200, 0), defaults={"Phase Offset": 0.25}),
    _node("N2", "/Game/Audio/MSP_Mix.MSP_Mix", "UE.0123.", inputs=("In",), outputs=("Left", "Right"), location=(100, 0),
          major=2),
    _node("N3", "Input", "UE.Input.", member="Pitch", inputs=("Value",), outputs=("Value",), location=(-500, 0)),
    _node("N4", "Output", "UE.Output.Audio", member="UE.OutputFormat.Mono.Audio:0", inputs=("UE.OutputFormat.Mono.Audio:0",),
          outputs=(), location=None),
    _node("N5", "UE.Sine.Audio", "UE.Sine.Audio", inputs=("Frequency",), outputs=("Audio",), location=(0, 300)),
    _edge("H1", "Pitch", "N3", "Value", derived=True),
    _edge("N3", "Value", "N1", "Frequency"),
    _edge("N1", "Audio", "N2", "In"),
    _edge("N2", "Left", "N4", "UE.OutputFormat.Mono.Audio:0"),
    {"kind": "comment", "page": "Default", "id": "C1", "text": "Note", "position": [-600, -100], "size": [400, 200],
     "color": {"R": 1, "G": 1, "B": 1, "A": 1}, "font_size": 18, "depth": 0, "color_bubble": False,
     "move_mode": "GroupMovement"},
]


def _paged(record):
    return {**record, "page": "Low"}


PAGED_RECORDS = [
    _header(pages=[{"id": "0" * 32, "name": "Default"}, {"id": "1" * 32, "name": "Low"}],
            configured_pages=["Default", "Low"]),
    RECORDS[1],
    _input("Pitch", "Float", 0.5, sort_order=1, widget={"WidgetType": "Slider"},
           defaults=[{"page": "Default", "value": 0.5}, {"page": "Low", "value": 0.25}]),
] + RECORDS[3:] + [_paged(record) for record in RECORDS[4:] if record["kind"] in ("variable", "edge", "comment")
                   or (record["kind"] == "node" and record["id"] != "N5")] + [
    _paged(_node("N5", "UE.Saw.Audio", "UE.Saw.Audio", inputs=("Frequency",), outputs=("Out",), location=(0, 300)))]


PRESET_RECORDS = [_header(preset=True, preset_parent="/Game/Audio/MSS_Base.MSS_Base", inherits=["B"], interfaces=[]),
                  _input("A", "Float", 2.0), _input("B", "Float", 1.0)]


def _document(body=None, properties=None, class_path=metasound.SOURCE_CLASS, action="CreateOrUpdate"):
    return {"SchemaVersion": 1, "Target": TARGET, "Class": class_path, "Action": action,
            "Properties": properties or {}, "Body": body if body is not None else {}}


def test_op_path_of_configuration():
    paths = ["/Body/Graphs/Default/Nodes/Sine/Configuration"]
    assert metasound_apply.op_path("ops/0/configuration/properties/NumChannels", paths) \
        == "/Body/Graphs/Default/Nodes/Sine/Configuration/Properties/NumChannels"


class KeyTests(unittest.TestCase):
    def test_sanitize(self):
        self.assertEqual(metasound.sanitize_key("UE.OutputFormat.Mono.Audio:0"), "UE_OutputFormat_Mono_Audio_0")
        self.assertEqual(metasound.sanitize_key("Wave Player"), "Wave_Player")
        self.assertEqual(metasound.sanitize_key(""), "Node")

    def test_keys_are_numbered_in_node_id_order(self):
        keys = metasound.node_keys([
            {"id": "A", "class": "UE.Sine.Audio", "class_name": "UE.Sine.Audio", "member": ""},
            {"id": "B", "class": "UE.Sine.Audio", "class_name": "UE.Sine.Audio", "member": ""},
            {"id": "C", "class": "Input", "class_name": "UE.Input.", "member": "Sine"},
            {"id": "D", "class": "/Game/A/MSP_X.MSP_X", "class_name": "UE.123.", "member": ""}])
        self.assertEqual(keys, {"A": "Sine", "B": "Sine_2", "C": "Sine_3", "D": "MSP_X"})


class AssembleTests(unittest.TestCase):
    def setUp(self):
        self.body, self.facts, self.keys = metasound.assemble(RECORDS)

    def test_header_regions(self):
        self.assertEqual(self.body["Interfaces"], ["UE.Source.OneShot"])
        self.assertEqual(self.body["Metadata"], {"Description": "Blip", "Author": "Me", "Referenceable": True})

    def test_members(self):
        # Interface members equal to their interface definition are implied by Interfaces.
        self.assertEqual(self.body["Inputs"], {"Pitch": {"Type": "Float", "Default": 0.5, "SortOrder": 1,
                                                         "Widget": {"WidgetType": "Slider"}}})
        self.assertEqual(self.body["Outputs"], {})
        self.assertEqual(self.facts["interface_inputs"], {"UE.Source.OnPlay"})

    def test_nodes_and_sources(self):
        nodes = self.body["Graphs"]["Default"]["Nodes"]
        self.assertEqual(list(nodes), ["Sine", "MSP_Mix", "Pitch", "UE_OutputFormat_Mono_Audio_0", "Sine_2"])
        self.assertEqual(nodes["Sine"], {"Class": "UE.Sine.Audio", "Id": "N1", "Position": [-200, 0],
                                         "Inputs": {"Frequency": "Pitch"}, "Defaults": {"Phase Offset": 0.25}})
        self.assertEqual(nodes["MSP_Mix"]["Version"], 2)
        self.assertEqual(nodes["MSP_Mix"]["Inputs"], {"In": "Sine"})
        self.assertEqual(nodes["UE_OutputFormat_Mono_Audio_0"],
                         {"Class": "Output", "Member": "UE.OutputFormat.Mono.Audio:0", "Id": "N4", "Hidden": True,
                          "Inputs": {"UE.OutputFormat.Mono.Audio:0": "MSP_Mix.Left"}})
        self.assertNotIn("H1", self.keys["Default"])
        self.assertEqual(self.facts["injected"], ["Pitch"])

    def test_variables_and_comments(self):
        graph = self.body["Graphs"]["Default"]
        self.assertEqual(graph["Variables"], {"Count": {"Type": "Int32", "Default": 3}})
        self.assertEqual(graph["Comments"]["Comment"]["Text"], "Note")
        self.assertEqual(graph["Comments"]["Comment"]["Position"], [-600, -100])

    def test_preset(self):
        records = [_header(preset=True, preset_parent="/Game/Audio/MSS_Base.MSS_Base", inherits=["B"]),
                   _input("A", "Float", 2.0), _input("B", "Float", 1.0)]
        body, _, _ = metasound.assemble(records)
        self.assertEqual(body["Preset"], {"Parent": "/Game/Audio/MSS_Base.MSS_Base"})
        self.assertEqual(body["Inputs"], {"A": {"Default": 2.0}})
        self.assertNotIn("Graphs", body)
        self.assertNotIn("Interfaces", body)


class DiffTests(unittest.TestCase):
    def setUp(self):
        body, self.facts, _ = metasound.assemble(RECORDS)
        self.live = metasound.canonicalize(_document(body))

    def _diff(self, body):
        return metasound.diff(metasound.canonicalize(_document(body)), self.live, exact=True, facts=self.facts)

    def test_extract_is_a_no_op(self):
        self.assertEqual(metasound.diff(self.live, self.live, exact=True, facts=self.facts), ())

    def test_omitted_regions_are_kept(self):
        self.assertEqual(self._diff({}), ())

    def test_ai_written_keys_and_explicit_single_output_pin(self):
        body = metasound.common.snapshot(self.live["Body"])
        nodes = body["Graphs"]["Default"]["Nodes"]
        nodes["MSP_Mix"]["Inputs"] = {"In": "Sine.Audio"}
        self.assertEqual(self._diff(body), ())

    def test_changes_are_reported(self):
        body = metasound.common.snapshot(self.live["Body"])
        nodes = body["Graphs"]["Default"]["Nodes"]
        nodes["Sine"]["Defaults"] = {"Phase Offset": 0.5}
        del nodes["Sine_2"]
        body["Inputs"]["Pitch"]["Default"] = 1.0
        paths = [item.path for item in self._diff(body)]
        self.assertIn("/Body/Graphs/Default/Nodes/Sine/Defaults", paths)
        self.assertIn("/Body/Graphs/Default/Nodes/Sine_2", paths)
        self.assertIn("/Body/Inputs/Pitch/Default", paths)

    def test_interface_members_are_not_removed_when_omitted(self):
        body = metasound.common.snapshot(self.live["Body"])
        body["Inputs"] = {"Pitch": body["Inputs"]["Pitch"]}
        self.assertEqual(self._diff(body), ())


class DocumentTests(unittest.TestCase):
    def test_unknown_body_key(self):
        with self.assertRaises(AssetDocumentContractError) as raised:
            metasound.canonicalize(_document({"Nodes": {}}))
        self.assertEqual(raised.exception.diagnostic.code, "MetaSound.UnknownBodyKey")

    def test_unknown_node_key(self):
        with self.assertRaises(AssetDocumentContractError) as raised:
            metasound.canonicalize(_document({"Graphs": {"Default": {"Nodes": {"A": {"Class": "X", "Pins": {}}}}}}))
        self.assertEqual(raised.exception.diagnostic.code, "MetaSound.UnknownKey")

    def test_not_authored_property(self):
        region = metasound._properties(metasound.SOURCE_CLASS)
        with self.assertRaises(AssetDocumentContractError) as raised:
            region.check_owned({"ReferencedAssetClassKeys": []})
        self.assertEqual(raised.exception.diagnostic.code, "MetaSound.NotAuthoredProperty")
        with self.assertRaises(AssetDocumentContractError) as raised:
            region.check_owned({"RootMetasoundDocument": {}})
        self.assertEqual(raised.exception.diagnostic.code, "MetaSound.BodyOwnedProperty")

    def test_looping_follows_the_interfaces(self):
        # The editor derives a Source's bLooping on load (no OneShot interface = looping), so an extract that held it
        # changed BaseRevision on reload: it is neither extracted nor authored.
        region = metasound._properties(metasound.SOURCE_CLASS)
        self.assertTrue({"bLooping", "Duration"} <= region.skipped())
        with self.assertRaises(AssetDocumentContractError) as raised:
            region.check_owned({"bLooping": True})
        self.assertEqual(raised.exception.diagnostic.code, "MetaSound.NotAuthoredProperty")
        self.assertIn("UE.Source.OneShot", raised.exception.diagnostic.message)


class Handle:
    def __init__(self, name):
        self.name = name


class FakeContext:
    """Answers the resolver, the audio read (always the same records) and the edit / compile primitives; records
    the provider hand-offs and the ops."""

    def __init__(self, exists, records=None, compile_messages=(), before_messages=()):
        self.exists = exists
        self.records = records if records is not None else RECORDS
        self.compile_messages = list(compile_messages)
        self.before_messages = list(before_messages)
        self.handoffs = []
        self.ops = []
        self.compiled = 0

    def handoff(self, *, source_handle, target_provider_id, access):
        self.handoffs.append((target_provider_id, access))
        return Handle(f"{target_provider_id}:{access}")

    def invoke(self, *, requirement, value_arguments, handle_arguments):
        primitive = requirement.primitive_id
        if primitive == metasound.common.RESOLVE:
            handles = (NamedOpaqueHandleResult("asset_read", (Handle("read"),) if self.exists else ()),
                       NamedOpaqueHandleResult("asset_mutable", (Handle("mutable"),) if self.exists else ()))
            return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED, {"exists": self.exists}, handles, (), ())
        if primitive == metasound.READ:
            if value_arguments.get("validate"):
                return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED,
                                        {"status": "UpToDate", "messages": self.before_messages}, (), (), ())
            return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED,
                                    {"total": len(self.records), "next_offset": -1, "records": self.records}, (), (), ())
        if primitive == metasound.EDIT:
            self.ops.extend(value_arguments["ops"])
            return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED, {"created": [], "deleted": [], "changed": True,
                                                                      "warnings": []}, (), (), ())
        if primitive == metasound.COMPILE:
            self.compiled += 1
            return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED,
                                    {"status": "UpToDate", "messages": self.compile_messages, "registered": True,
                                     "dependents": []}, (), (), ())
        return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED, {}, (), (), ())


class _NoProperties(metasound._MetaSoundProperties):
    written = []

    def validate(self, properties, context):
        self.check_owned(properties)

    def extract(self, read, context, include_defaults=True):
        return {}

    def apply(self, properties, read, mutable, context):
        self.check_owned(properties)
        _NoProperties.written.append(dict(properties))
        return ()


class ApplyTests(unittest.TestCase):
    """The apply planner against a fake asset (the editor side is UEAssetDoc.MetaSound.*)."""

    def setUp(self):
        self.profile = metasound.MetaSoundRuntimeProfile(metasound.SOURCE_CLASS, "profile.metasound_source.v1")
        self.original = metasound._properties
        metasound._properties = lambda class_path: _NoProperties(class_path, metasound.BODY_OWNED, metasound.PREFIX)
        self.body, _, _ = metasound.assemble(RECORDS)

    def tearDown(self):
        metasound._properties = self.original

    def _apply(self, body, context=None, properties=None):
        context = context or FakeContext(True)
        result = self.profile.apply(_document(body, properties), Handle("read"), Handle("mutable"), context)
        return result, context

    def _refused(self, body, code, path=None, records=None):
        with self.assertRaises(AssetDocumentContractError) as raised:
            self.profile.preflight(_document(body), None, FakeContext(True, records))
        self.assertEqual(raised.exception.diagnostic.code, code)
        if path is not None:
            self.assertEqual(raised.exception.diagnostic.path, path)

    def test_new_metasound_is_admitted(self):
        self.assertIs(self.profile.preflight(_document({"Inputs": {"A": {"Type": "Float"}}}), None,
                                             FakeContext(False)), SNAPSHOT_COVERED)

    def test_unchanged_apply_writes_and_registers_nothing(self):
        result, context = self._apply(metasound.common.snapshot(self.body))
        self.assertEqual(tuple(result), ())
        self.assertEqual((context.ops, context.compiled), ([], 0))
        self.assertIs(self.profile.preflight(_document(self.body), None, FakeContext(True)), SNAPSHOT_COVERED)

    def test_properties_only_change_writes_them_without_registration(self):
        # Core calls apply only when its Diff found a change; the editor's property reads are verify-only, so the
        # authored Details values are written (as the Details panel writes them) and an unchanged graph is left alone.
        _NoProperties.written.clear()
        result, context = self._apply(metasound.common.snapshot(self.body), properties={"Priority": 2.0})
        self.assertEqual(tuple(result), ())
        self.assertEqual(_NoProperties.written, [{"Priority": 2.0}])
        self.assertEqual((context.ops, context.compiled), ([], 0))

    def test_refusals(self):
        body = metasound.common.snapshot(self.body)
        body["Graphs"]["Second"] = {"Nodes": {}}
        # Pages are the project's page settings (the read's configured_pages).
        self._refused(body, "MetaSound.UnknownPage", "/Body/Graphs/Second")
        self._refused({"Inputs": {"Pitch": {"Type": "Float", "PageDefaults": {"Second": 1.0}}}}, "MetaSound.UnknownPage",
                      "/Body/Inputs/Pitch/PageDefaults/Second")
        self._refused({"Inputs": {"Pitch": {"Type": "Float", "PageDefaults": {"Default": 1.0}}}}, "MetaSound.InvalidBody",
                      "/Body/Inputs/Pitch/PageDefaults/Default")
        # The editor cannot convert an existing MetaSound into a preset.
        self._refused({"Preset": {"Parent": "/Game/A.A"}}, "MetaSound.CannotConvertToPreset", "/Body/Preset")
        # A MetaSound with several pages is edited like any other.
        self.assertIs(self.profile.preflight(_document({"Metadata": {"Description": "Other"}}), None,
                                             FakeContext(True, PAGED_RECORDS)), SNAPSHOT_COVERED)

    def test_pages_are_read_per_page(self):
        body, facts, keys = metasound.assemble(PAGED_RECORDS)
        self.assertEqual(list(body["Graphs"]), ["Default", "Low"])
        self.assertEqual(body["Inputs"]["Pitch"]["PageDefaults"], {"Low": 0.25})
        low = body["Graphs"]["Low"]["Nodes"]
        # The page is a copy of Default (same node IDs), with its own class on N5.
        self.assertEqual(low["Sine"]["Id"], "N1")
        self.assertEqual(low["Saw"]["Class"], "UE.Saw.Audio")
        self.assertEqual(metasound.page_facts(facts, "Low")["outputs"]["N5"], ["Out"])
        self.assertEqual(metasound.page_facts(facts, "Default")["outputs"]["N5"], ["Audio"])
        live = metasound.canonicalize(_document(body))
        self.assertEqual(metasound.diff(live, live, exact=True, facts=facts), ())
        # Graphs without a page removes it; Default is kept when omitted.
        authored = metasound.common.snapshot(body)
        del authored["Graphs"]["Low"]
        del authored["Graphs"]["Default"]
        self.assertEqual([item.path for item in metasound.diff(metasound.canonicalize(_document(authored)), live, exact=True,
                                                               facts=facts)], ["/Body/Graphs/Low"])
        # PageDefaults is a complete map, diffed page by page: a dropped page value is a Remove entry.
        authored = metasound.common.snapshot(body)
        authored["Inputs"]["Pitch"]["PageDefaults"] = {}
        self.assertEqual([(item.path, item.before_present, item.after_present) for item in metasound.diff(
            metasound.canonicalize(_document(authored)), live, exact=True, facts=facts)],
            [("/Body/Inputs/Pitch/PageDefaults/Low", False, True)])

    def test_page_edits(self):
        body, _, _ = metasound.assemble(PAGED_RECORDS)
        body["Inputs"]["Pitch"]["PageDefaults"] = {"Low": 0.75}
        low = body["Graphs"]["Low"]["Nodes"]
        low["Sine"]["Position"] = [5, 5]
        low["Saw"]["Defaults"] = {"Frequency": 110.0}
        body["Graphs"]["Low"]["Variables"]["Count"]["Default"] = 9
        _, context = self._apply(body, FakeContext(True, PAGED_RECORDS))
        self.assertIn({"op": "input.set", "name": "Pitch", "page_defaults": {"Low": 0.75}}, context.ops)
        self.assertIn({"op": "node.set", "id": "N1", "position": [5, 5], "page": "Low"}, context.ops)
        self.assertIn({"op": "node.set", "id": "N5", "defaults": {"Frequency": 110.0}, "page": "Low"}, context.ops)
        self.assertIn({"op": "variable.set", "name": "Count", "default": 9, "page": "Low"}, context.ops)
        self.assertFalse(any(op["op"].startswith("page.") for op in context.ops))
        # Removing the page: the Pages Details delete button, first.
        body, _, _ = metasound.assemble(PAGED_RECORDS)
        del body["Graphs"]["Low"]
        body["Inputs"]["Pitch"]["PageDefaults"] = {}
        _, context = self._apply(body, FakeContext(True, PAGED_RECORDS))
        self.assertEqual(context.ops[0], {"op": "page.remove", "name": "Low"})
        self.assertIn({"op": "input.set", "name": "Pitch", "page_defaults": {}}, context.ops)

    def test_page_add(self):
        # The fake asset never changes, so the new page stays missing: only its add is planned.
        body = metasound.common.snapshot(self.body)
        body["Graphs"]["Low"] = metasound.common.snapshot(body["Graphs"]["Default"])
        records = [_header(configured_pages=["Default", "Low"])] + RECORDS[1:]
        _, context = self._apply(body, FakeContext(True, records))
        self.assertEqual([op for op in context.ops if op["op"].startswith("page.")], [{"op": "page.add", "name": "Low"}])
        self.assertFalse(any(op.get("page") for op in context.ops))

    def test_preset_refusals(self):
        preset = PRESET_RECORDS
        self._refused({"Graphs": {"Default": {"Nodes": {}}}}, "MetaSound.PresetGraphDerived", "/Body/Graphs", records=preset)
        self._refused({"Inputs": {"A": {"Type": "Int32"}}}, "MetaSound.PresetGraphDerived", "/Body/Inputs/A/Type",
                      records=preset)
        self._refused({"Inputs": {"Nope": {"Default": 1.0}}}, "MetaSound.UnknownMember", "/Body/Inputs/Nope", records=preset)
        self._refused({"Preset": {"Parent": "/Game/Other"}}, "MetaSound.PresetParentChanged", "/Body/Preset/Parent",
                      records=preset)
        # The parent named by package path is the same parent.
        self.assertIs(self.profile.preflight(_document({"Preset": {"Parent": "/Game/Audio/MSS_Base"}}), None,
                                             FakeContext(True, preset)), SNAPSHOT_COVERED)

    def test_preset_create_is_checked_before_the_factory(self):
        with self.assertRaises(AssetDocumentContractError) as raised:
            self.profile.preflight(_document({"Preset": {"Parent": "/Game/A"}, "Outputs": {}}), None, FakeContext(False))
        self.assertEqual(raised.exception.diagnostic.path, "/Body/Outputs")
        self.assertIs(self.profile.preflight(_document({"Preset": {"Parent": "/Game/A"}, "Inputs": {"X": {"Default": 1}}}),
                                             None, FakeContext(False)), SNAPSHOT_COVERED)

    def test_preset_overrides(self):
        # A overridden (2.0), B inherits: override B, let A inherit again, keep the metadata.
        _, context = self._apply({"Preset": {"Parent": "/Game/Audio/MSS_Base.MSS_Base"}, "Inputs": {"B": {"Default": 5.0}}},
                                 FakeContext(True, PRESET_RECORDS))
        self.assertEqual(context.ops, [{"op": "input.inherit", "name": "A", "inherits": True},
                                       {"op": "input.set", "name": "B", "default": 5.0}])
        self.assertEqual(context.compiled, 1)
        # Overriding with the inherited value itself only clears the inheritance.
        _, context = self._apply({"Inputs": {"A": {"Default": 2.0}, "B": {}}}, FakeContext(True, PRESET_RECORDS))
        self.assertEqual(context.ops, [{"op": "input.inherit", "name": "B", "inherits": False}])
        # The extract re-applies as a no-op.
        body, _, _ = metasound.assemble(PRESET_RECORDS)
        _, context = self._apply(body, FakeContext(True, PRESET_RECORDS))
        self.assertEqual((context.ops, context.compiled), ([], 0))

    def test_convert_from_preset(self):
        _, context = self._apply({"Preset": None}, FakeContext(True, PRESET_RECORDS))
        self.assertEqual(context.ops[-1], {"op": "preset.convert_from"})
        self.assertEqual(context.compiled, 1)
        # Verified against the converted MetaSound: only the listed inputs' values count.
        live, facts, _ = metasound.assemble(RECORDS)
        pitch = live["Inputs"]["Pitch"].get("Default")
        authored = {"Preset": None, "Metadata": live["Metadata"], "Inputs": {"Pitch": {"Default": pitch}}}
        self.assertEqual(metasound.diff(metasound.canonicalize(_document(authored)), metasound.canonicalize(_document(live)),
                                        exact=True, facts=facts), ())
        authored["Inputs"]["Pitch"]["Default"] = 123.0
        self.assertEqual([item.path for item in metasound.diff(metasound.canonicalize(_document(authored)),
                                                               metasound.canonicalize(_document(live)), exact=True,
                                                               facts=facts)], ["/Body/Inputs/Pitch/Default"])

    def test_configuration_and_update_node(self):
        records = RECORDS[:6] + [dict(RECORDS[6], configuration={"type": "/Script/H.Config", "properties": {}})] + RECORDS[7:]
        body, _, _ = metasound.assemble(records)
        # A default configuration is not extracted.
        self.assertNotIn("Configuration", body["Graphs"]["Default"]["Nodes"]["Sine"])
        nodes = body["Graphs"]["Default"]["Nodes"]
        nodes["Sine"]["Configuration"] = {"Properties": {"NumChannels": 2}}
        nodes["MSP_Mix"]["Version"] = 3
        nodes["Saw"] = {"Class": "UE.Saw.Audio", "Configuration": {"Type": "/Script/H.Config", "Properties": {"N": 1}},
                        "Defaults": {"Frequency": 2.0}}
        _, context = self._apply(body, FakeContext(True, records))
        self.assertIn({"op": "node.update", "id": "N2", "major": 3}, context.ops)
        self.assertIn({"op": "node.set", "id": "N1", "configuration": {"properties": {"NumChannels": 2}}}, context.ops)
        saw = metasound.keyed_id(metasound.NODE_SEED + "Saw")
        self.assertIn({"op": "node.set", "id": saw, "configuration": {"properties": {"N": 1}, "type": "/Script/H.Config"},
                       "defaults": {"Frequency": 2.0}}, context.ops)
        # Classes and configurations change before the graph phase plans connections.
        self.assertLess(context.ops.index({"op": "node.update", "id": "N2", "major": 3}),
                        next(index for index, op in enumerate(context.ops) if op["op"] == "node.create"))
        # An explicit default configuration equals an omitted one.
        live, facts, _ = metasound.assemble(records)
        authored = metasound.common.snapshot(live)
        authored["Graphs"]["Default"]["Nodes"]["Sine"]["Configuration"] = {"Type": "/Script/H.Config", "Properties": {}}
        self.assertEqual(metasound.diff(metasound.canonicalize(_document(authored)), metasound.canonicalize(_document(live)),
                                        exact=True, facts=facts), ())
        # A setting written with the class's default value is the same as leaving it out.
        records = RECORDS[:6] + [dict(RECORDS[6], configuration={"type": "/Script/H.Config", "properties": {"Ramp": 8.0},
                                                                 "defaults": {"NumChannels": 1, "Ramp": 5.0}})] + RECORDS[7:]
        live, facts, _ = metasound.assemble(records)
        self.assertEqual(live["Graphs"]["Default"]["Nodes"]["Sine"]["Configuration"]["Properties"], {"Ramp": 8.0})
        authored = metasound.common.snapshot(live)
        authored["Graphs"]["Default"]["Nodes"]["Sine"]["Configuration"]["Properties"] = {"NumChannels": 1, "Ramp": 8.0}
        self.assertEqual(metasound.diff(metasound.canonicalize(_document(authored)), metasound.canonicalize(_document(live)),
                                        exact=True, facts=facts), ())
        authored["Graphs"]["Default"]["Nodes"]["Sine"]["Configuration"]["Properties"] = {"NumChannels": 2, "Ramp": 8.0}
        self.assertEqual(len(metasound.diff(metasound.canonicalize(_document(authored)), metasound.canonicalize(_document(live)),
                                            exact=True, facts=facts)), 1)

    def test_pin_names_compare_without_case(self):
        live, facts, _ = metasound.assemble(RECORDS)
        authored = metasound.common.snapshot(live)
        node = next(entry for entry in authored["Graphs"]["Default"]["Nodes"].values() if entry.get("Inputs"))
        node["Inputs"] = {pin.upper(): source for pin, source in node["Inputs"].items()}
        self.assertEqual(metasound.diff(metasound.canonicalize(_document(authored)), metasound.canonicalize(_document(live)),
                                        exact=True, facts=facts), ())

    def test_layout_of_new_nodes(self):
        """New nodes go left of the placed node they feed, right of the placed nodes feeding them, and never on a
        node (two nodes at [960, 0]; a new Source's output left of its upstream node)."""
        from asset_document.profiles import metasound_apply as apply
        nodes = {"Out": {"Class": "Output", "Member": "Out", "Inputs": {"Out": "Mix"}},
                 "Mix": {"Class": "UE.Mix", "Inputs": {"A": "Sine", "B": "Saw"}},
                 "Sine": {"Class": "UE.Sine.Audio"}, "Saw": {"Class": "UE.Saw.Audio"},
                 "Gain": {"Class": "UE.Gain", "Inputs": {"In": "Old"}}, "Old": {"Class": "UE.Noise", "Position": [500, 400]}}
        layout = apply._layout(nodes, ["Mix", "Sine", "Saw", "Gain"], {"Out": [0, 0], "Old": [500, 400]})
        self.assertEqual(layout["Mix"], [-320, 0])
        self.assertEqual(layout["Sine"], [-640, 0])
        self.assertEqual(layout["Saw"], [-640, 160])
        self.assertEqual(layout["Gain"], [820, 400])
        # Nothing placed: columns by depth, one row per node, as before.
        self.assertEqual(apply._layout(nodes, ["Out", "Mix", "Sine", "Saw"]),
                         {"Out": [640, 0], "Mix": [320, 0], "Sine": [0, 0], "Saw": [0, 160]})

    def test_edit_ops_and_one_registration(self):
        body = metasound.common.snapshot(self.body)
        nodes = body["Graphs"]["Default"]["Nodes"]
        body["Metadata"]["Description"] = "Boop"
        body["Inputs"]["Pitch"]["Default"] = 1.0
        body["Inputs"]["Gain"] = {"Type": "Float", "Default": 0.5}
        nodes["Sine"]["Position"] = [10, 20]
        nodes["Sine"]["Defaults"] = {}
        del nodes["Sine_2"]
        nodes["Osc"] = {"Class": "UE.Saw.Audio", "Inputs": {"Frequency": "Pitch"}}
        nodes["MSP_Mix"]["Inputs"] = {"In": "Osc"}
        result, context = self._apply(body)
        kinds = [op["op"] for op in context.ops]
        self.assertEqual(kinds[0], "normalize")  # the read showed an open-time injection
        self.assertIn({"op": "metadata", "description": "Boop"}, context.ops)
        self.assertIn({"op": "input.set", "name": "Pitch", "default": 1.0}, context.ops)
        self.assertIn({"op": "input.add", "name": "Gain", "type": "Float", "default": 0.5}, context.ops)
        self.assertIn({"op": "node.delete", "id": "N5"}, context.ops)
        osc = metasound.keyed_id(metasound.NODE_SEED + "Osc")
        # Left of MSP_Mix (100, 0) that it feeds, below the Sine (10, 20) it would overlap.
        self.assertIn({"op": "node.create", "id": osc, "kind": "class", "class": "UE.Saw.Audio", "position": [-220, 320]},
                      context.ops)
        self.assertIn({"op": "node.set", "id": "N1", "position": [10, 20], "defaults": {}}, context.ops)
        self.assertIn({"op": "connect", "id": osc, "pin": "Frequency", "from": "N3", "from_pin": "Value"}, context.ops)
        # Rewiring disconnects first.
        self.assertLess(context.ops.index({"op": "connect", "id": "N2", "pin": "In", "from": None}),
                        context.ops.index({"op": "connect", "id": "N2", "pin": "In", "from": osc}))
        self.assertEqual(context.compiled, 1)
        self.assertTrue(all(item[0] == "audio" for item in context.handoffs))
        self.assertIn(("audio", "mutable"), context.handoffs)

    def test_removed_members_and_output_nodes(self):
        body = metasound.common.snapshot(self.body)
        body["Inputs"] = {}
        nodes = body["Graphs"]["Default"]["Nodes"]
        del nodes["Pitch"]
        del nodes["UE_OutputFormat_Mono_Audio_0"]
        nodes["Sine"]["Inputs"] = {}
        _, context = self._apply(body)
        self.assertIn({"op": "node.delete", "id": "N3"}, context.ops)
        self.assertIn({"op": "node.delete", "id": "output:UE.OutputFormat.Mono.Audio:0"}, context.ops)
        self.assertEqual(context.ops[-1], {"op": "input.remove", "name": "Pitch"})

    def test_new_validation_errors_fail_and_old_ones_warn(self):
        body = metasound.common.snapshot(self.body)
        body["Metadata"]["Description"] = "Boop"
        old = {"severity": "Error", "message": "Old", "node_id": "N1"}
        new = {"severity": "Error", "message": "New", "node_id": "N2"}
        with self.assertRaises(AssetDocumentContractError) as raised:
            self._apply(body, FakeContext(True, compile_messages=[old, new], before_messages=[old]))
        self.assertEqual(raised.exception.diagnostic.code, "MetaSound.ValidationFailed")
        self.assertEqual(raised.exception.diagnostic.path, "/Body/Graphs/Default/Nodes/MSP_Mix")
        result, _ = self._apply(body, FakeContext(True, compile_messages=[old], before_messages=[old]))
        self.assertEqual([item.code for item in result.diagnostics], ["MetaSound.ValidationError"])

    def test_build_errors_fail_at_their_page_node(self):
        body, _, _ = metasound.assemble(PAGED_RECORDS)
        body["Metadata"]["Description"] = "Boop"
        build = {"severity": "Error", "message": "The graph does not build into an operator (GraphCycle)", "node_id": "",
                 "node_ids": ["X", "N5"], "page": "Low", "source": "Build"}
        with self.assertRaises(AssetDocumentContractError) as raised:
            self._apply(body, FakeContext(True, PAGED_RECORDS, compile_messages=[build]))
        self.assertEqual(raised.exception.diagnostic.code, "MetaSound.BuildFailed")
        self.assertEqual(raised.exception.diagnostic.path, "/Body/Graphs/Low/Nodes/Saw")
        # A build error the MetaSound already had is a warning.
        result, _ = self._apply(body, FakeContext(True, PAGED_RECORDS, compile_messages=[build], before_messages=[build]))
        self.assertEqual([item.code for item in result.diagnostics], ["MetaSound.BuildError"])

    def test_dependents_are_warnings(self):
        _, warnings = metasound_apply.compile_outcome(
            {}, {"messages": [], "dependents": [{"path": "/Game/A.A", "dirty_before": True, "dirty_after": True}]}, {})
        self.assertEqual([item.code for item in warnings],
                         ["MetaSound.DependentsUpdated", "MetaSound.DependentHasUnsavedChanges"])

    def test_op_paths(self):
        paths = ["/Body/Graphs/Default/Nodes/Sine", "/Body/Inputs/Pitch/Widget", "/Body/Graphs/Default/Nodes/Sine/Inputs/In"]
        self.assertEqual(metasound_apply.op_path("ops/0/defaults/Frequency", paths),
                         "/Body/Graphs/Default/Nodes/Sine/Defaults/Frequency")
        self.assertEqual(metasound_apply.op_path("/ops/0/class", paths), "/Body/Graphs/Default/Nodes/Sine/Class")
        self.assertEqual(metasound_apply.op_path("ops/1/values/Range", paths), "/Body/Inputs/Pitch/Widget/Range")
        self.assertEqual(metasound_apply.op_path("ops/2/from", paths), paths[2])
        self.assertEqual(metasound_apply.op_path("ops", paths), "/Body")
        self.assertEqual(metasound_apply.op_path("ops/1/page_defaults/Low", ["/Body/Inputs/Pitch", "/Body/Inputs/Pitch"]),
                         "/Body/Inputs/Pitch/PageDefaults/Low")
        self.assertEqual(metasound_apply.op_path("ops/0/page", paths), paths[0])

    def test_keyed_ids_are_stable_guids(self):
        value = metasound.keyed_id(metasound.NODE_SEED + "Osc")
        self.assertRegex(value, r"\A[0-9A-F]{32}\Z")
        self.assertEqual(value, metasound.keyed_id(metasound.NODE_SEED + "Osc"))
        self.assertNotEqual(value, metasound.keyed_id(metasound.COMMENT_SEED + "Osc"))


if __name__ == "__main__":
    unittest.main()
