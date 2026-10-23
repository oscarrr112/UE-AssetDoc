# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

import unittest

from asset_document import bootstrap
from asset_document.diagnostics import AssetDocumentContractError
from asset_document.mutation_intent_projection import SNAPSHOT_COVERED
from asset_document.profiles import widget_blueprint_animation as animation
from asset_document.profiles import widget_blueprint_tree as tree
from asset_document.profiles.widget_blueprint_runtime import (
    WIDGET_BLUEPRINT_CLASS, WidgetBlueprintRuntimeProfile, canonicalize, diff,
)

TARGET = "/Game/Tests/WBP_Menu"
CANVAS = "/Script/UMG.CanvasPanel"
VBOX = "/Script/UMG.VerticalBox"
TEXT = "/Script/UMG.TextBlock"
BUTTON = "/Script/UMG.Button"


def document(body, properties=None, action="CreateOrUpdate"):
    return {"SchemaVersion": 1, "Target": TARGET, "Class": WIDGET_BLUEPRINT_CLASS, "Action": action,
            "Properties": properties or {}, "Body": body}


def node(name, cls, *children, **extra):
    out = {"Name": name, "Class": cls, **extra}
    if children:
        out["Children"] = list(children)
    return out


def rows_from_tree(value, capacity=None):
    """The umg.tree.query.v1 rows the C++ provider would return for a canonical tree."""
    capacity = capacity or {}
    rows = []
    for item, parent, slot, index in tree.walk(value):
        rows.append({"name": item["Name"], "class": item["Class"], "parent": parent, "slot_name": slot or "",
                     "index": index, "is_variable": item.get("IsVariable", False),
                     "capacity": capacity.get(item["Name"], "many"), "properties": item.get("Properties", {}),
                     **({"slot": item["Slot"]} if "Slot" in item else {})})
    return rows


class Simulator:
    """Independent replay of umg.tree.* calls (panel insert/shift, slot occupancy, recursive delete)."""

    def __init__(self, rows):
        self.parent, self.children, self.slots, self.classes = {}, {}, {}, {}
        for row in rows:
            self.classes[row["name"]] = row["class"]
            self._put(row["name"], row["parent"], row["slot_name"] or (None if row["parent"] else ""), None)

    def _put(self, name, parent, slot, index):
        self.parent[name] = (parent, slot)
        if slot is None:
            items = self.children.setdefault(parent, [])
            items.insert(len(items) if index is None else min(index, len(items)), name)
        else:
            assert (parent, slot) not in self.slots, f"slot {parent}.{slot} occupied"
            self.slots[(parent, slot)] = name

    def _off(self, name):
        parent, slot = self.parent.pop(name)
        if slot is None:
            self.children[parent].remove(name)
        else:
            del self.slots[(parent, slot)]

    def _delete(self, name):
        for (parent, _), child in list(self.slots.items()):
            if parent == name:
                self._delete(child)
        for child in list(self.children.get(name, [])):
            self._delete(child)
        if name in self.parent:
            self._off(name)
        del self.classes[name]

    def apply(self, ops):
        for primitive, values, _ in ops:
            name = values["name"]
            if primitive == "umg.tree.remove.v1":
                self._delete(name)
                continue
            if primitive == "umg.tree.add.v1":
                assert name not in self.classes
                self.classes[name] = values["class"]
            elif name in self.parent:
                self._off(name)
            if values.get("detach"):
                continue
            parent = values.get("parent", "")
            slot = None if "index" in values else values.get("slot_name", "")
            self._put(name, parent, slot, values.get("index"))

    def tree(self):
        def build(name):
            out = {"Name": name, "Class": self.classes[name]}
            slots = {slot: build(child) for (parent, slot), child in sorted(self.slots.items()) if parent == name}
            if slots:
                out["NamedSlots"] = slots
            kids = [build(child) for child in self.children.get(name, [])]
            if kids:
                out["Children"] = kids
            return out
        root = self.slots.get(("", ""))
        return {"RootWidget": build(root) if root else None,
                "NamedSlotBindings": {slot: build(child) for (parent, slot), child in sorted(self.slots.items())
                                      if parent == "" and slot}}


def shape(value):
    """Structure only (names, classes, nesting)."""
    def strip(item):
        out = {"Name": item["Name"], "Class": item["Class"]}
        if item.get("NamedSlots"):
            out["NamedSlots"] = {key: strip(child) for key, child in item["NamedSlots"].items()}
        if item.get("Children"):
            out["Children"] = [strip(child) for child in item["Children"]]
        return out
    return {"RootWidget": strip(value["RootWidget"]) if value.get("RootWidget") else None,
            "NamedSlotBindings": {key: strip(child) for key, child in value.get("NamedSlotBindings", {}).items()}}


class CanonicalTests(unittest.TestCase):
    def test_legacy_shapes_are_converted(self):
        body = canonicalize(document({
            "ParentClass": {"Kind": "ClassRef", "Class": "/Script/UMG.UserWidget"},
            "Palette": {"Category": "Menus"},
            "ClassDefaults": {"DesiredFocusWidget": {"WidgetName": "Title"}},
            "EditorOptions": {"bCanCallInitializedWithoutPlayerContext": True},
            "WidgetVariableGuids": {"Title": "0"},
            "Components": [],
            "WidgetTree": {"RootWidget": {"Name": "Root", "Class": CANVAS, "VariableName": "Root",
                                          "Children": [{"Name": "Title", "Class": TEXT, "IsVariable": True}]}},
            "Bindings": [{"Widget": "Title", "Property": "Text", "Kind": "Property", "SourceProperty": "Caption",
                          "MemberGuid": "1"}],
            "Animations": [{"Name": "Intro", "Tracks": [
                {"Widget": "Title", "Property": "RenderTransform", "Type": "transform",
                 "Channels": [{"Name": "Rotation", "Keys": [{"Frame": 0, "Value": 5}]}]},
                {"Widget": "Title", "Property": "RenderOpacity", "Type": "Float", "Keys": [{"Frame": 1.0, "Value": 1}]}]}],
        }))
        b = body["Body"]
        self.assertEqual(body["Properties"], {"bCanCallInitializedWithoutPlayerContext": True})
        self.assertEqual(b["ClassDefaults"], {})
        self.assertEqual(b["PaletteCategory"], "Menus")
        self.assertEqual(b["DesiredFocus"], "Title")
        self.assertNotIn("Components", b)
        self.assertEqual(b["WidgetTree"]["RootWidget"],
                         {"Name": "Root", "Class": CANVAS, "Children": [{"Name": "Title", "Class": TEXT, "IsVariable": True}]})
        self.assertEqual(b["Bindings"], [{"Widget": "Title", "Property": "Text", "Kind": "Property", "SourcePath": ["Caption"]}])
        tracks = b["Animations"][0]["Tracks"]
        self.assertEqual(tracks[0]["Type"], "Transform")
        self.assertEqual(tracks[0]["Channels"][0]["Name"], "Angle")
        self.assertEqual(tracks[1]["Channels"], [{"Name": "Value", "Keys": [{"Frame": 1, "Value": 1}]}])
        self.assertEqual(canonicalize(body), body)

    def test_invalid_documents_fail_with_stable_codes(self):
        for body, code in (({"Components": [{"Name": "Mesh"}]}, "Widget.UnsupportedRegion"),
                           ({"Timelines": [{"Name": "T"}]}, "Widget.UnsupportedRegion"),
                           ({"Graphs": []}, "Widget.UnknownBodyKey"),
                           ({"WidgetTree": {"RootWidget": node("A", CANVAS, node("a", TEXT))}}, "Widget.DuplicateName"),
                           ({"WidgetTree": {"RootWidget": {"Name": "A B", "Class": CANVAS}}}, "Widget.InvalidName"),
                           ({"WidgetTree": {"RootWidget": {"Name": "A", "Class": CANVAS, "Kids": []}}}, "Widget.UnknownField"),
                           ({"WidgetTree": {"RootWidget": node("A", BUTTON, Navigation={"Sideways": "Stop"})}},
                            "Widget.InvalidNavigation"),
                           ({"Bindings": [{"Widget": "A", "Property": "Text", "Kind": "Function"}]}, "Widget.InvalidBinding"),
                           ({"Animations": [{"Name": "X", "FrameRate": {"Numerator": 0}}]}, "Widget.InvalidFrameRate"),
                           ({"Animations": [{"Name": "X", "Tracks": [
                               {"Widget": "A", "Property": "P", "Type": "Float", "Keys": [{"Frame": 0, "Value": 1, "Interp": "Spline"}]}]}]},
                            "Widget.InvalidKey")):
            with self.assertRaises(AssetDocumentContractError, msg=str(body)) as caught:
                canonicalize(document(body))
            self.assertEqual(caught.exception.diagnostic.code, code, str(body))
        with self.assertRaises(AssetDocumentContractError) as caught:
            canonicalize(document({"WidgetTree": {}}, action="Create"))
        self.assertEqual(caught.exception.diagnostic.code, "Widget.ParentClassRequired")

    def test_integral_float_frame_rates_are_ints(self):
        # Documents that went through C++ JSON carry numbers as doubles.
        b = canonicalize(document({"Animations": [{"Name": "X", "FrameRate": {"Numerator": 30.0, "Denominator": 1.0}}]}))
        self.assertEqual(b["Body"]["Animations"][0]["FrameRate"], {"Numerator": 30, "Denominator": 1})
        self.assertIs(type(b["Body"]["Animations"][0]["FrameRate"]["Numerator"]), int)
        for bad in (29.97, True, "30"):
            with self.assertRaises(AssetDocumentContractError):
                canonicalize(document({"Animations": [{"Name": "X", "FrameRate": {"Numerator": bad}}]}))

    def test_navigation_defaults_are_dropped(self):
        b = canonicalize(document({"WidgetTree": {"RootWidget": node("A", BUTTON, Navigation={
            "Up": {"Rule": "Escape"}, "Down": "EUINavigationRule::Explicit", "RoutingPolicy": "Default"})}}))["Body"]
        self.assertEqual(b["WidgetTree"]["RootWidget"]["Navigation"], {"Down": {"Rule": "Explicit"}})

    def test_preflight_is_snapshot_covered_and_registered(self):
        self.assertIs(WidgetBlueprintRuntimeProfile().preflight(document({}), None, None), SNAPSHOT_COVERED)
        binding = bootstrap.PROFILE_REGISTRY.resolve_runtime_exact(WIDGET_BLUEPRINT_CLASS)
        self.assertIsNotNone(binding)
        self.assertEqual(binding.target_lifecycle.create_context_handle_names, ("parent_class",))
        template = WidgetBlueprintRuntimeProfile().create_template(TARGET)
        self.assertEqual(canonicalize(template)["Body"]["ParentClass"], "/Script/UMG.UserWidget")


class TreePlanTests(unittest.TestCase):
    def plan(self, live, desired, capacity=None):
        rows = rows_from_tree(live, capacity)
        canonical, paths = tree.canonical_tree(desired)
        effective = tree.effective_tree(canonical, tree.tree_from_rows(rows))
        ops = tree.plan_structure(effective, rows, paths)
        simulator = Simulator(rows)
        simulator.apply(ops)
        self.assertEqual(shape(simulator.tree()), shape(effective))
        return ops

    def test_unchanged_tree_plans_nothing(self):
        live = {"RootWidget": node("Root", CANVAS, node("A", TEXT), node("B", VBOX, node("C", TEXT))),
                "NamedSlotBindings": {"Header": node("H", TEXT)}}
        self.assertEqual(self.plan(live, live), [])
        self.assertEqual(self.plan(live, {"NamedSlotBindings": live["NamedSlotBindings"]}), [])

    def test_add_remove_reparent_and_reorder(self):
        live = {"RootWidget": node("Root", CANVAS, node("A", TEXT), node("B", VBOX, node("C", TEXT)), node("D", TEXT)),
                "NamedSlotBindings": {}}
        desired = {"RootWidget": node("Root", CANVAS, node("B", VBOX, node("A", TEXT), node("C", TEXT), node("E", BUTTON)))}
        ops = self.plan(live, desired)
        self.assertEqual([op[0].split(".")[-2] for op in ops], ["remove", "move", "move", "add"])
        self.assertEqual(ops[0][1], {"name": "D"})
        self.assertEqual(ops[-1][1], {"name": "E", "class": BUTTON, "parent": "B", "index": 2})

    def test_class_change_replaces_and_keeps_references(self):
        live = {"RootWidget": node("Root", CANVAS, node("Box", VBOX, node("Label", TEXT))), "NamedSlotBindings": {}}
        desired = {"RootWidget": node("Root", CANVAS, node("Box", "/Script/UMG.HorizontalBox", node("Label", TEXT)))}
        ops = self.plan(live, desired)
        self.assertEqual(ops[0], ("umg.tree.move.v1", {"name": "Label", "detach": True},
                                  "/Body/WidgetTree/RootWidget/Children/0/Children/0"))
        self.assertEqual(ops[1][1], {"name": "Box", "keep_references": True})

    def test_root_and_single_content_occupants_are_detached_first(self):
        live = {"RootWidget": node("Old", CANVAS, node("Ok", BUTTON, node("Label", TEXT))), "NamedSlotBindings": {}}
        desired = {"RootWidget": node("New", VBOX, node("Old", CANVAS, node("Ok", BUTTON, node("Icon", TEXT))),
                                      node("Label", TEXT))}
        ops = self.plan(live, desired, capacity={"Ok": "one"})
        self.assertEqual(ops[0], ("umg.tree.move.v1", {"name": "Old", "detach": True}, "/Body/WidgetTree/RootWidget"))
        self.assertIn(("umg.tree.move.v1", {"name": "Label", "detach": True},
                       "/Body/WidgetTree/RootWidget/Children/0/Children/0/Children/0"), ops)

    def test_case_only_name_change_is_a_rename_of_the_same_widget(self):
        live = {"RootWidget": node("Root", CANVAS, node("okbutton", BUTTON, node("Label", TEXT))), "NamedSlotBindings": {}}
        desired, paths = tree.canonical_tree({"RootWidget": node("Root", CANVAS, node("OkButton", BUTTON, node("Label", TEXT)))})
        ops = tree.plan_renames(desired, rows_from_tree(live), paths)
        self.assertEqual(ops, [("umg.widget.rename.v1", {"name": "okbutton", "new_name": "OkButton"},
                                "/Body/WidgetTree/RootWidget/Children/0/Name")])
        # After the rename the live tree matches: no remove + add.
        renamed = {"RootWidget": node("Root", CANVAS, node("OkButton", BUTTON, node("Label", TEXT))), "NamedSlotBindings": {}}
        self.assertEqual(tree.plan_structure(tree.effective_tree(desired, renamed), rows_from_tree(renamed), paths), [])
        self.assertEqual(tree.plan_renames(desired, rows_from_tree(renamed), paths), [])
        self.assertTrue(tree.tree_mismatches(desired, live))

    def test_renamed_from_is_a_rename_of_the_same_widget(self):
        live = {"RootWidget": node("Root", CANVAS, node("OkButton", BUTTON, node("Label", TEXT))), "NamedSlotBindings": {}}
        renamed_node = node("ConfirmButton", BUTTON, node("Label", TEXT), RenamedFrom="OkButton")
        desired, paths = tree.canonical_tree({"RootWidget": node("Root", CANVAS, renamed_node)})
        ops = tree.plan_renames(desired, rows_from_tree(live), paths)
        self.assertEqual(ops, [("umg.widget.rename.v1", {"name": "OkButton", "new_name": "ConfirmButton"},
                                "/Body/WidgetTree/RootWidget/Children/0/RenamedFrom")])
        # Ignored once the rename happened; the renamed live tree matches (RenamedFrom is not compared).
        renamed = {"RootWidget": node("Root", CANVAS, node("ConfirmButton", BUTTON, node("Label", TEXT))),
                   "NamedSlotBindings": {}}
        self.assertEqual(tree.plan_renames(desired, rows_from_tree(renamed), paths), [])
        self.assertEqual(tree.plan_structure(tree.effective_tree(desired, renamed), rows_from_tree(renamed), paths), [])
        self.assertEqual(tree.tree_mismatches(desired, renamed), [])
        # No live widget with the old name: an add.
        other = {"RootWidget": node("Root", CANVAS, node("Label", TEXT)), "NamedSlotBindings": {}}
        self.assertEqual(tree.plan_renames(desired, rows_from_tree(other), paths), [])
        # A source that is still listed or renamed twice is refused.
        for bad in ({"RootWidget": node("Root", CANVAS, node("A", TEXT), node("B", TEXT, RenamedFrom="A"))},
                    {"RootWidget": node("Root", CANVAS, node("B", TEXT, RenamedFrom="X"), node("C", TEXT, RenamedFrom="x"))},
                    {"RootWidget": node("Root", CANVAS, node("B", TEXT, RenamedFrom="B"))},
                    {"RootWidget": node("Root", CANVAS, node("B", TEXT, RenamedFrom="A.B"))}):
            with self.assertRaises(AssetDocumentContractError, msg=str(bad)) as caught:
                tree.canonical_tree(bad)
            self.assertEqual(caught.exception.diagnostic.code, "Widget.InvalidRename", str(bad))

    def test_named_slots_and_clear(self):
        live = {"RootWidget": node("Card", "/Game/UI/WBP_Card.WBP_Card_C", NamedSlots={"Body": node("X", TEXT)}),
                "NamedSlotBindings": {"Header": node("H", TEXT)}}
        desired = {"RootWidget": node("Card", "/Game/UI/WBP_Card.WBP_Card_C", NamedSlots={"Body": node("H", TEXT)}),
                   "NamedSlotBindings": {}}
        ops = self.plan(live, desired)
        self.assertEqual(ops[0][1], {"name": "X"})
        self.assertEqual(ops[-1][1], {"name": "H", "parent": "Card", "slot_name": "Body"})
        self.assertEqual(self.plan(live, {"RootWidget": None, "NamedSlotBindings": {}})[0][1], {"name": "Card"})

    def test_values_set_reset_and_navigation(self):
        desired, paths = tree.canonical_tree({"RootWidget": node("Root", CANVAS, node(
            "Ok", BUTTON, IsVariable=True, Properties={"IsEnabled": False}, Slot={"ZOrder": 3},
            Navigation={"Down": {"Rule": "Explicit", "WidgetToFocus": "Root"}}))})
        rows = rows_from_tree({"RootWidget": node("Root", CANVAS, node("Ok", BUTTON, Properties={"ToolTipText": "x"},
                                                                        Slot={"ZOrder": 3}))})
        ops = tree.plan_values(tree.effective_tree(desired, {"NamedSlotBindings": {}}), rows, paths)
        self.assertEqual([(op[0], op[1].get("property")) for op in ops],
                         [("umg.widget.variable.set.v1", None), ("umg.widget.property.set.v1", "IsEnabled"),
                          ("umg.widget.property.set.v1", "ToolTipText"), ("umg.widget.navigation.set.v1", None)])
        self.assertEqual(ops[2][1]["reset"], True)
        self.assertEqual(ops[3][1]["navigation"], {"Down": {"rule": "Explicit", "widget_to_focus": "Root"}})

    def test_bindings_plan(self):
        desired = tree.canonical_bindings([{"Widget": "T", "Property": "Text", "Function": "GetText"},
                                           {"Widget": "Bar", "Property": "Percent", "SourcePath": "Health"}])
        live = [{"widget": "T", "property": "Text", "kind": "Function", "function": "GetText"},
                {"widget": "Old", "property": "Text", "kind": "Function", "function": "F"}]
        ops = tree.plan_bindings(desired, live)
        self.assertEqual([op[0] for op in ops], ["umg.binding.remove.v1", "umg.binding.set.v1"])
        self.assertEqual(ops[1][1], {"widget": "Bar", "property": "Percent", "kind": "Property", "source_path": ["Health"]})
        self.assertEqual(tree.plan_bindings(tree.canonical_bindings(tree.bindings_from_live(live)), live), [])


class AnimationTests(unittest.TestCase):
    DETAIL = {"name": "Intro", "display_rate": {"numerator": 30, "denominator": 1}, "playback": {"start": 0, "end": 30},
              "tracks": [{"widget": "T", "target": "widget", "property": "RenderOpacity", "type": "Float",
                          "supported": True, "channels": [{"name": "Value", "kind": "float",
                                                           "keys": [{"frame": 0, "value": 0.0},
                                                                    {"frame": 30, "value": 1.0, "interp": "Cubic",
                                                                     "tangent": {"mode": "User", "arrive": 0, "leave": 0.5}}]}]},
                         {"widget": "", "target": "widget", "property": "", "type": "/Script/MovieSceneTracks.MovieSceneEventTrack",
                          "supported": False, "reason": "track is not bound to a widget", "channels": []}]}

    def live(self):
        return animation.canonical_animations([animation.animation_from_live(self.DETAIL)])[0]

    def test_round_trip_is_a_no_op(self):
        live = self.live()
        self.assertIsNone(animation.header_values(live, live))
        self.assertEqual(animation.plan_tracks(live, live, "/Body/Animations/0"), [])
        self.assertEqual(animation.diff_animations([live], [live]), [])
        extracted = animation.animation_from_live(self.DETAIL)
        self.assertIn("Keys", extracted["Tracks"][0])

    def test_unsupported_tracks_must_be_listed(self):
        live = self.live()
        desired = {**live, "Tracks": live["Tracks"][:1]}
        with self.assertRaises(AssetDocumentContractError) as caught:
            animation.plan_tracks(desired, live, "/Body/Animations/0")
        self.assertEqual(caught.exception.diagnostic.code, "Widget.UnsupportedTrack")

    def test_new_animation_and_track_edits(self):
        desired = animation.canonical_animations([{"Name": "Pulse", "Tracks": [
            {"Widget": "T", "Target": "Slot", "Property": "LayoutData.Offsets", "Type": "Margin",
             "Channels": [{"Name": "Left", "Keys": [{"Frame": 0, "Value": 4}]}, {"Name": "Top", "Keys": []}]}]}])[0]
        self.assertEqual(animation.header_values(desired, None),
                         {"name": "Pulse", "display_rate": {"numerator": 30, "denominator": 1},
                          "playback": {"start": 0, "end": 30}})
        ops = animation.plan_tracks(desired, None, "/Body/Animations/0")
        self.assertEqual(ops, [("umg.anim.track.set.v1",
                                {"animation": "Pulse", "widget": "T", "target": "slot", "property": "LayoutData.Offsets",
                                 "type": "Margin", "channels": [{"name": "Left", "keys": [{"frame": 0, "value": 4}]}]},
                                "/Body/Animations/0/Tracks/0")])
        live = self.live()
        edited = {**live, "FrameRate": {"Numerator": 60, "Denominator": 1},
                  "Tracks": [{**live["Tracks"][0], "Channels": [{"Name": "Value", "Keys": [{"Frame": 0, "Value": 0.5}]}]},
                             live["Tracks"][1]]}
        self.assertEqual(animation.header_values(edited, live), {"name": "Intro", "display_rate": {"numerator": 60, "denominator": 1}})
        self.assertEqual([op[0] for op in animation.plan_tracks(edited, live, "/x")], ["umg.anim.track.set.v1"])
        removed = {**live, "Tracks": live["Tracks"][1:]}
        self.assertEqual([op[0] for op in animation.plan_tracks(removed, live, "/x")], ["umg.anim.track.remove.v1"])


class DiffTests(unittest.TestCase):
    def extracted(self):
        return document({"ParentClass": "/Script/UMG.UserWidget", "ImplementedInterfaces": [], "Variables": [],
                         "ClassDefaults": {}, "PaletteCategory": "Menus",
                         "WidgetTree": {"RootWidget": node("Root", CANVAS, node("T", TEXT, IsVariable=True,
                                                                                Properties={"Text": "Hi"})),
                                        "NamedSlotBindings": {}},
                         "DesiredFocus": "T", "Bindings": [],
                         "Animations": [animation.animation_from_live(AnimationTests.DETAIL)],
                         "UbergraphPages": [], "FunctionGraphs": [], "MacroGraphs": []})

    def test_extraction_has_no_strict_diff(self):
        extracted = self.extracted()
        self.assertEqual(diff(canonicalize(extracted), canonicalize(extracted), strict=True), ())

    def test_tree_and_property_mismatch_is_reported(self):
        extracted = canonicalize(self.extracted())
        authored = canonicalize(document({"WidgetTree": {"RootWidget": node("Root", CANVAS, node(
            "T", TEXT, Properties={"Text": "Bye"}))}, "Bindings": [{"Widget": "T", "Property": "Text", "Function": "F"}]}))
        changes = diff(authored, extracted, strict=True)
        # The widget tree difference names the smallest changed node (the property).
        self.assertEqual([item.path for item in changes],
                         ["/Body/WidgetTree/RootWidget/Children/0/Properties/Text", "/Body/Bindings"])
        self.assertEqual((changes[0].before, changes[0].after), ("Bye", "Hi"))
        self.assertEqual(diff(canonicalize(document({"WidgetTree": {"NamedSlotBindings": {}}})), extracted, True), ())
        self.assertEqual([item.path for item in diff(canonicalize(document({"DesiredFocus": None})), extracted, True)],
                         ["/Body/DesiredFocus"])

    def test_widget_tree_diff_is_per_widget(self):
        extracted = canonicalize(self.extracted())
        authored = canonicalize(document({"WidgetTree": {"RootWidget": node("Root", CANVAS, node(
            "T", TEXT, IsVariable=True, Properties={"Text": "Hi"}), node("Bar", "/Script/UMG.ProgressBar"))}}))
        changes = diff(authored, extracted, strict=True)
        self.assertEqual([(item.path, item.kind.value) for item in changes],
                         [("/Body/WidgetTree/RootWidget/Children/1", "Remove")])
        self.assertEqual(changes[0].before["Name"], "Bar")
        removed = canonicalize(document({"WidgetTree": {"RootWidget": node("Root", CANVAS)}}))
        self.assertEqual([(item.path, item.kind.value) for item in diff(removed, extracted, strict=True)],
                         [("/Body/WidgetTree/RootWidget/Children/0", "Add")])

    def test_design_size(self):
        extracted = canonicalize(self.extracted())
        self.assertEqual(canonicalize(document({"DesignSize": None}))["Body"]["DesignSize"],
                         {"Mode": "FillScreen", "Width": 100, "Height": 100})
        custom = canonicalize(document({"DesignSize": {"Mode": "EDesignPreviewSizeMode::Custom", "Width": 1920.0,
                                                       "Height": 1080}}))["Body"]["DesignSize"]
        self.assertEqual(custom, {"Mode": "Custom", "Width": 1920, "Height": 1080})
        self.assertEqual([item.path for item in diff(canonicalize(document({"DesignSize": custom})), extracted, True)],
                         ["/Body/DesignSize"])
        self.assertEqual(diff(canonicalize(document({"DesignSize": {"Mode": "FillScreen"}})), extracted, True), ())
        for bad in ({"Mode": "Huge"}, {"Width": 0}, {"Height": 1.5}, {"Size": 3}, "Custom"):
            with self.assertRaises(AssetDocumentContractError, msg=str(bad)) as caught:
                canonicalize(document({"DesignSize": bad}))
            self.assertEqual(caught.exception.diagnostic.code, "Widget.InvalidValue")


if __name__ == "__main__":
    unittest.main()
