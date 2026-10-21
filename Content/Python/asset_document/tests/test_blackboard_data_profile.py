from __future__ import annotations

from pathlib import Path
import unittest

import asset_document.profiles.blackboard_data as blackboard_module
from asset_document.diagnostics import AssetDocumentContractError, RuntimeDiagnostic
from asset_document.dto import ChangedRecord, PrimitiveRequirement, RuntimePhase
from asset_document.profiles.blackboard_data import (
    BLACKBOARD_DATA_CLASS,
    BLACKBOARD_DATA_PROFILE_META,
    BLACKBOARD_REQUIRED_PRIMITIVES,
    BlackboardSnapshot,
    BlackboardDataProfileCandidate,
    create_blackboard_data_profile,
    parse_blackboard_snapshot,
    plan_blackboard_transitions,
)
from asset_document.regions.primitive_catalog import NamedOpaqueHandleResult, PrimitiveOutcome, PrimitiveOutcomeStatus


TARGET = "/Game/AI/BB_Target"
TARGET_OBJECT = "/Game/AI/BB_Target.BB_Target"
PARENT_OBJECT = "/Game/AI/BB_Parent.BB_Parent"
GRANDPARENT_OBJECT = "/Game/AI/BB_Grand.BB_Grand"
OLD_PARENT_OBJECT = "/Game/AI/BB_Old.BB_Old"
SHARED_PARENT_OBJECT = "/Game/AI/BB_Shared.BB_Shared"
ROOT_PARENT_OBJECT = "/Game/AI/BB_Root.BB_Root"
OBJECT_KEY_TYPE = "/Script/AIModule.BlackboardKeyType_Object"
CLASS_KEY_TYPE = "/Script/AIModule.BlackboardKeyType_Class"
ENUM_KEY_TYPE = "/Script/AIModule.BlackboardKeyType_Enum"


def entry(name: str, *, key_type: str = OBJECT_KEY_TYPE, properties: dict | None = None) -> dict:
    default_properties = {
        OBJECT_KEY_TYPE: {"BaseClass": {"Kind": "ClassRef", "Path": "/Script/Engine.Actor"}, "DefaultValue": None},
        ENUM_KEY_TYPE: {"DefaultValue": "Set", "EnumName": "", "EnumType": {"Kind": "AssetRef", "Path": "/Game/Data/E_Mode.E_Mode"}},
        "/Script/AIModule.BlackboardKeyType_Bool": {"bDefaultValue": False},
    }.get(key_type, {})
    return {
        "Category": "",
        "Description": "",
        "KeyTypeClass": {"Kind": "ClassRef", "Path": key_type},
        "KeyTypeProperties": default_properties | ({} if properties is None else properties),
        "Name": name,
        "bInstanceSynced": False,
    }


def document(*keys: dict, parent: dict | None = None, target: str = TARGET) -> dict:
    return {
        "SchemaVersion": 1,
        "Target": target,
        "Class": BLACKBOARD_DATA_CLASS,
        "Action": "CreateOrUpdate",
        "Definitions": {},
        "Properties": {},
        "Body": {"Parent": parent, "Keys": list(keys)},
    }


def asset_ref(path: str) -> dict:
    return {"Kind": "AssetRef", "Path": path}


def assert_error(test: unittest.TestCase, code: str, path: str, callback) -> None:
    with test.assertRaises(AssetDocumentContractError) as caught:
        callback()
    test.assertEqual((caught.exception.diagnostic.code, caught.exception.diagnostic.path), (code, path))


def outcome(value: object, *changes: ChangedRecord) -> PrimitiveOutcome:
    return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED, value, (), changes, ())


def failure(code: str, path: str = "/Primitive") -> PrimitiveOutcome:
    return PrimitiveOutcome(
        PrimitiveOutcomeStatus.FAILED,
        None,
        (),
        (),
        (RuntimeDiagnostic(path, code, "injected failure"),),
    )


def changed(primitive_id: str, path: str, kind: str = "Update") -> ChangedRecord:
    return ChangedRecord(primitive_id, TARGET_OBJECT, path, kind)


class PrimitiveContext:
    def __init__(self, phase: RuntimePhase, responses: list[tuple[str, PrimitiveOutcome]]) -> None:
        self.phase = phase
        self.responses = list(responses)
        self.calls: list[tuple[str, object, tuple]] = []

    def invoke(self, *, requirement, value_arguments, handle_arguments) -> PrimitiveOutcome:
        self.calls.append((requirement.primitive_id, value_arguments, handle_arguments))
        if not self.responses:
            raise AssertionError(f"unexpected primitive: {requirement.primitive_id}")
        expected, result = self.responses.pop(0)
        if requirement.primitive_id != expected:
            raise AssertionError(f"expected {expected}, got {requirement.primitive_id}")
        return result

    def assert_drained(self, test: unittest.TestCase) -> None:
        test.assertEqual(self.responses, [])


class BlackboardDataProfileContractTests(unittest.TestCase):
    def test_exact_shape_and_profile_factory_binding(self) -> None:
        profile = create_blackboard_data_profile()

        self.assertIs(type(profile), BlackboardDataProfileCandidate)
        self.assertEqual(
            (
                BLACKBOARD_DATA_PROFILE_META.profile_id,
                BLACKBOARD_DATA_PROFILE_META.route_id,
                BLACKBOARD_DATA_PROFILE_META.exact_asset_class_path,
                BLACKBOARD_DATA_PROFILE_META.required_primitives,
            ),
            (
                "profile.blackboard_data.v2",
                "ai_profile",
                BLACKBOARD_DATA_CLASS,
                BLACKBOARD_REQUIRED_PRIMITIVES,
            ),
        )
        self.assertEqual(
            profile.inspect_profile(),
            {"Class": BLACKBOARD_DATA_CLASS, "BodySections": ["Parent", "Keys"]},
        )
        missing_body = document()
        del missing_body["Body"]
        scalar_body = document()
        scalar_body["Body"] = 7
        missing_parent = document()
        missing_parent["Body"] = {"Keys": []}
        for label, invalid in (("missing", missing_body), ("scalar", scalar_body), ("missing-parent", missing_parent)):
            with self.subTest(body=label):
                context = PrimitiveContext(RuntimePhase.PREFLIGHT, [])
                assert_error(
                    self,
                    "BlackboardData.InvalidBodyShape",
                    "/Body",
                    lambda invalid=invalid, context=context: profile.preflight(invalid, context=context),
                )
                self.assertEqual(context.calls, [])

    def test_top_level_properties_are_rejected_not_dropped(self) -> None:
        # Parent/Keys belong to Body and nothing else is editable; apply never writes
        # Properties, so any authored name must fail validation instead of being ignored.
        profile = BlackboardDataProfileCandidate()
        for name, value, code in (
            ("Parent", None, "BodyOwnedProperty"),
            ("parent", None, "BodyOwnedProperty"),
            ("Keys", [], "BodyOwnedProperty"),
            ("Unrelated", 1, "Generic.UnknownProperty"),
        ):
            with self.subTest(name=name):
                invalid = document()
                invalid["Properties"] = {name: value}
                assert_error(self, code, f"/Properties/{name}", lambda invalid=invalid: profile.validate(invalid))
        # Reported before other header problems (e.g. a legacy AssetType field).
        legacy = document()
        legacy["AssetType"] = "GenericAsset"
        legacy["Properties"] = {"Parent": None}
        assert_error(self, "BodyOwnedProperty", "/Properties/Parent", lambda: profile.validate(legacy))
        profile.validate(document())

    def test_unknown_body_region_names_the_region(self) -> None:
        profile = BlackboardDataProfileCandidate()
        invalid = document()
        invalid["Body"]["UnknownRegion"] = "x"
        assert_error(self, "UnknownBodyKey", "/Body/UnknownRegion", lambda: profile.validate(invalid))

    def test_template_schema_inspect_and_released_required_set(self) -> None:
        profile = BlackboardDataProfileCandidate()

        template = profile.create_template(TARGET)
        self.assertEqual(template["Action"], "CreateOrUpdate")
        self.assertEqual(template["Body"], {"Parent": None, "Keys": []})
        self.assertEqual(
            profile.schema()["Properties"]["Body"],
            {
                "type": "object",
                "required": ["Parent", "Keys"],
                "order": "authored-local-key-order",
                "entry": "BlackboardEntryV2",
            },
        )
        expected_ids = (
            "ai.blackboard.entry.readback.v2",
            "ai.blackboard.parent.readback.v2",
            "ai.blackboard.local_keys.readback.v2",
            "ai.blackboard.entry.update_existing.v2",
            "ai.blackboard.parent.set.v2",
            "ai.blackboard.entry.add.v2",
            "ai.blackboard.entry.replace_class.v2",
            "ai.blackboard.entry.remove.v2",
            "ai.blackboard.local_keys.reorder.v2",
        )
        self.assertEqual(BLACKBOARD_REQUIRED_PRIMITIVES, profile.required_primitives())
        self.assertEqual(tuple(item.primitive_id for item in BLACKBOARD_REQUIRED_PRIMITIVES), expected_ids)
        self.assertTrue(all(item == PrimitiveRequirement(item.primitive_id, 2, "ai_profile") for item in BLACKBOARD_REQUIRED_PRIMITIVES))
        self.assertFalse(any(".v1" in item.primitive_id for item in BLACKBOARD_REQUIRED_PRIMITIVES))
        invalid_action = document()
        invalid_action["Action"] = {}
        assert_error(
            self,
            "BlackboardData.InvalidAction",
            "/Action",
            lambda: profile.validate(invalid_action),
        )

    def test_legacy_alias_normalization_and_conflict_rejection(self) -> None:
        profile = BlackboardDataProfileCandidate()
        legacy_object = {
            "Category": "Perception",
            "Description": "Observed actor",
            "Type": "Object",
            "BaseClass": {"Kind": "ClassRef", "Path": "/Script/Engine.Actor"},
            "KeyTypeProperties": {"DefaultValue": None},
            "Name": "ObservedActor",
            "bInstanceSynced": False,
        }
        legacy_enum = {
            "Category": "State",
            "Description": "Mode",
            "Type": "Enum",
            "Enum": "/Game/Data/E_Mode.E_Mode",
            "KeyTypeProperties": {"DefaultValue": "Set"},
            "Name": "Mode",
            "bInstanceSynced": True,
        }

        keys = profile.canonicalize(document(legacy_object, legacy_enum))["Body"]["Keys"]
        self.assertEqual(
            keys[0],
            entry(
                "ObservedActor",
                properties={
                    "BaseClass": {"Kind": "ClassRef", "Path": "/Script/Engine.Actor"},
                    "DefaultValue": None,
                },
            )
            | {"Category": "Perception", "Description": "Observed actor"},
        )
        self.assertEqual(
            keys[1],
            entry(
                "Mode",
                key_type=ENUM_KEY_TYPE,
                properties={
                    "DefaultValue": "Set",
                    "EnumName": "",
                    "EnumType": {"Kind": "AssetRef", "Path": "/Game/Data/E_Mode.E_Mode"},
                },
            )
            | {"Category": "State", "Description": "Mode", "bInstanceSynced": True},
        )
        string_base = dict(legacy_object, BaseClass="/Script/Engine.Actor")
        self.assertEqual(profile.canonicalize(document(string_base))["Body"]["Keys"][0], keys[0])

        conflict = dict(legacy_object, KeyTypeClass={"Kind": "ClassRef", "Path": ENUM_KEY_TYPE})
        assert_error(self, "BlackboardData.AliasConflict", "/Body/Keys/0/Type", lambda: profile.canonicalize(document(conflict)))
        duplicate_class = dict(legacy_object, KeyTypeClass={"Kind": "ClassRef", "Path": OBJECT_KEY_TYPE})
        assert_error(self, "BlackboardData.AliasConflict", "/Body/Keys/0/Type", lambda: profile.canonicalize(document(duplicate_class)))
        null_canonical_duplicate = dict(legacy_object, KeyTypeClass=None)
        assert_error(self, "BlackboardData.AliasConflict", "/Body/Keys/0/Type", lambda: profile.canonicalize(document(null_canonical_duplicate)))
        null_legacy_duplicate = entry("Duplicate") | {"Type": None}
        assert_error(self, "BlackboardData.AliasConflict", "/Body/Keys/0/Type", lambda: profile.canonicalize(document(null_legacy_duplicate)))
        duplicate_base = dict(legacy_object, KeyTypeProperties={"BaseClass": {"Kind": "ClassRef", "Path": "/Script/Engine.Actor"}, "DefaultValue": None})
        assert_error(self, "BlackboardData.AliasConflict", "/Body/Keys/0/BaseClass", lambda: profile.canonicalize(document(duplicate_base)))
        unknown = dict(legacy_object, Extra=True)
        assert_error(self, "BlackboardData.UnknownEntryField", "/Body/Keys/0/Extra", lambda: profile.canonicalize(document(unknown)))
        missing = dict(legacy_object)
        del missing["Category"]
        assert_error(self, "BlackboardData.MissingEntryField", "/Body/Keys/0/Category", lambda: profile.canonicalize(document(missing)))
        invalid_aliases = (
            ("Type", 1, "BlackboardData.InvalidKeyTypeClass"),
            ("BaseClass", 1, "BlackboardData.InvalidAlias"),
            ("Enum", 1, "BlackboardData.InvalidAlias"),
        )
        for field, value, code in invalid_aliases:
            with self.subTest(field=field):
                invalid = dict(legacy_object, **{field: value})
                assert_error(self, code, f"/Body/Keys/0/{field}", lambda invalid=invalid: profile.canonicalize(document(invalid)))

        invalid_properties = (
            ("base-kind", {"BaseClass": 1}, "/Body/Keys/0/KeyTypeProperties/BaseClass"),
            (
                "base-reference-kind",
                {"BaseClass": {"Kind": "AssetRef", "Path": "/Game/Data/BP_Actor.BP_Actor"}},
                "/Body/Keys/0/KeyTypeProperties/BaseClass",
            ),
            (
                "enum-kind",
                {"EnumName": "", "EnumType": {"Kind": "ClassRef", "Path": "/Script/Engine.Actor"}},
                "/Body/Keys/0/KeyTypeProperties/EnumType",
            ),
            (
                "asset-enum-name",
                {"EnumName": "E_Mode", "EnumType": {"Kind": "AssetRef", "Path": "/Game/Data/E_Mode.E_Mode"}},
                "/Body/Keys/0/KeyTypeProperties/EnumName",
            ),
        )
        for label, properties, path in invalid_properties:
            with self.subTest(properties=label):
                invalid = entry("Invalid", key_type=ENUM_KEY_TYPE, properties=properties)
                assert_error(self, "BlackboardData.InvalidEntryField", path, lambda invalid=invalid: profile.canonicalize(document(invalid)))

        missing_properties = (
            ("object", entry("Object"), {}, "/Body/Keys/0/KeyTypeProperties/BaseClass"),
            (
                "enum-type",
                entry("Enum", key_type=ENUM_KEY_TYPE),
                {"DefaultValue": "Set"},
                "/Body/Keys/0/KeyTypeProperties/EnumType",
            ),
            (
                "enum-name",
                entry("Enum", key_type=ENUM_KEY_TYPE),
                {"DefaultValue": "Set", "EnumType": {"Kind": "AssetRef", "Path": "/Game/Data/E_Mode.E_Mode"}},
                "/Body/Keys/0/KeyTypeProperties/EnumName",
            ),
        )
        for label, invalid, properties, path in missing_properties:
            with self.subTest(missing=label):
                invalid["KeyTypeProperties"] = properties
                assert_error(self, "BlackboardData.InvalidEntryField", path, lambda invalid=invalid: profile.canonicalize(document(invalid)))

        invalid_game_class = entry("Invalid", key_type="/Game/Data/BP_Type.BP_Type")
        assert_error(self, "BlackboardData.InvalidKeyTypeClass", "/Body/Keys/0/KeyTypeClass", lambda: profile.canonicalize(document(invalid_game_class)))
        invalid_base_class = entry("Invalid", properties={"BaseClass": {"Kind": "ClassRef", "Path": "/Game/Data/BP_Actor.BP_Actor"}})
        assert_error(self, "BlackboardData.InvalidEntryField", "/Body/Keys/0/KeyTypeProperties/BaseClass", lambda: profile.canonicalize(document(invalid_base_class)))
        invalid_default_class = entry(
            "Invalid",
            key_type=CLASS_KEY_TYPE,
            properties={"BaseClass": {"Kind": "ClassRef", "Path": "/Script/Engine.Actor"}, "DefaultValue": {"Kind": "ClassRef", "Path": "/Game/Data/BP_Actor.BP_Actor"}},
        )
        assert_error(self, "BlackboardData.InvalidEntryField", "/Body/Keys/0/KeyTypeProperties/DefaultValue", lambda: profile.canonicalize(document(invalid_default_class)))
        invalid_legacy_base = dict(legacy_object, BaseClass="/Game/Data/BP_Actor.BP_Actor")
        assert_error(self, "BlackboardData.InvalidAlias", "/Body/Keys/0/BaseClass", lambda: profile.canonicalize(document(invalid_legacy_base)))

    def test_local_name_self_parent_and_order_identity(self) -> None:
        profile = BlackboardDataProfileCandidate()
        authored = document(entry("Zulu"), entry("Alpha"), parent=asset_ref(PARENT_OBJECT))

        canonical = profile.canonicalize(authored)
        self.assertEqual([item["Name"] for item in canonical["Body"]["Keys"]], ["Zulu", "Alpha"])
        self.assertEqual(canonical["Body"]["Parent"], asset_ref(PARENT_OBJECT))
        invalid_parent = asset_ref(PARENT_OBJECT) | {"Class": BLACKBOARD_DATA_CLASS}
        assert_error(
            self,
            "BlackboardData.InvalidParent",
            "/Body/Parent",
            lambda: profile.validate(document(parent=invalid_parent)),
        )

        assert_error(
            self,
            "BlackboardData.DuplicateLocalName",
            "/Body/Keys/1/Name",
            lambda: profile.validate(document(entry("Same"), entry("Same"))),
        )
        assert_error(
            self,
            "BlackboardData.DuplicateLocalName",
            "/Body/Keys/1/Name",
            lambda: profile.validate(document(entry("Foo"), entry("foo"))),
        )
        assert_error(
            self,
            "BlackboardData.SelfParent",
            "/Body/Parent/Path",
            lambda: profile.validate(document(parent=asset_ref(TARGET_OBJECT))),
        )
        invalid_name = entry("")
        assert_error(
            self,
            "BlackboardData.InvalidLocalName",
            "/Body/Keys/0/Name",
            lambda: profile.validate(document(invalid_name)),
        )

    def test_current_snapshot_is_required_and_complete(self) -> None:
        first = entry("First")
        second = entry("Second", key_type=ENUM_KEY_TYPE, properties={"DefaultValue": "Set"})
        parent_result = {"parent_path_or_null": PARENT_OBJECT}
        names_result = {"ordered_local_key_names": ["First", "Second"]}
        entry_results = [{"entry": first}, {"entry": second}]

        snapshot = parse_blackboard_snapshot(
            parent_result=parent_result,
            local_keys_result=names_result,
            entry_results=entry_results,
        )
        self.assertEqual(
            snapshot,
            BlackboardSnapshot(PARENT_OBJECT, (first, second), ("First", "Second")),
        )
        incomplete_second = entry("Second", key_type=ENUM_KEY_TYPE)
        incomplete_second["KeyTypeProperties"] = {"DefaultValue": "Set"}
        cases = (
            (None, names_result, entry_results, "/Current/Parent"),
            (parent_result, names_result, entry_results[:1], "/Current/Entries"),
            (parent_result, {"ordered_local_key_names": ["First", "First"]}, entry_results, "/Current/OrderedLocalKeyNames/1"),
            (parent_result, names_result, list(reversed(entry_results)), "/Current/Entries/0/Name"),
            (parent_result, names_result, [{"entry": first}, {"entry": incomplete_second}], "/Current/Entries/1"),
            (parent_result, {"ordered_local_key_names": [1]}, [{"entry": first}], "/Current/OrderedLocalKeyNames/0"),
        )
        for parent, names, entries, path in cases:
            with self.subTest(path=path):
                assert_error(
                    self,
                    "BlackboardData.InvalidCurrentSnapshot",
                    path,
                    lambda parent=parent, names=names, entries=entries: parse_blackboard_snapshot(
                        parent_result=parent,
                        local_keys_result=names,
                        entry_results=entries,
                    ),
                )

    def test_read_only_observer_uses_verify_readbacks_without_a_transition_plan(self) -> None:
        current = entry("Current")
        context = PrimitiveContext(
            RuntimePhase.VERIFY,
            [
                ("ai.blackboard.parent.readback.v2", outcome({"parent_path_or_null": None})),
                ("ai.blackboard.local_keys.readback.v2", outcome({"ordered_local_key_names": ["Current"]})),
                ("ai.blackboard.entry.readback.v2", outcome({"entry": current})),
            ],
        )
        observer = getattr(
            BlackboardDataProfileCandidate(), "observe_current_snapshot", None
        )
        self.assertIsNotNone(observer)
        self.assertEqual(observer(target=TARGET, context=context), BlackboardSnapshot(None, (current,), ("Current",)))
        self.assertEqual(
            [call[0] for call in context.calls],
            [
                "ai.blackboard.parent.readback.v2",
                "ai.blackboard.local_keys.readback.v2",
                "ai.blackboard.entry.readback.v2",
            ],
        )
        context.assert_drained(self)

    def test_read_only_observer_excludes_the_exact_engine_derived_self_actor(self) -> None:
        current = entry("Current")
        self_actor = entry(
            "SelfActor",
            key_type="/Script/AIModule.BlackboardKeyType_Object",
            properties={
                "BaseClass": {"Kind": "ClassRef", "Path": "/Script/Engine.Actor"},
                "DefaultValue": None,
            },
        )
        context = PrimitiveContext(
            RuntimePhase.VERIFY,
            [
                ("ai.blackboard.parent.readback.v2", outcome({"parent_path_or_null": None})),
                ("ai.blackboard.local_keys.readback.v2", outcome({"ordered_local_key_names": ["Current", "SelfActor"]})),
                ("ai.blackboard.entry.readback.v2", outcome({"entry": current})),
                ("ai.blackboard.entry.readback.v2", outcome({"entry": self_actor})),
            ],
        )

        self.assertEqual(
            BlackboardDataProfileCandidate().observe_current_snapshot(target=TARGET, context=context),
            BlackboardSnapshot(None, (current,), ("Current",)),
        )
        context.assert_drained(self)

    def test_vector_and_rotator_properties_are_sparse_and_replace_in_place_changes(self) -> None:
        vector, rotator = "/Script/AIModule.BlackboardKeyType_Vector", "/Script/AIModule.BlackboardKeyType_Rotator"
        cases = (
            (vector, {}, {"DefaultValue": None}),
            (vector, {"bUseDefaultValue": False, "DefaultValue": {"X": 1}}, {"DefaultValue": None}),
            (vector, {"DefaultValue": {"X": 10, "Y": -20, "Z": 30}, "bUseDefaultValue": True}, {"DefaultValue": {"X": 10.0, "Y": -20.0, "Z": 30.0}}),
            (vector, {"bUseDefaultValue": True}, {"DefaultValue": {"X": 0.0, "Y": 0.0, "Z": 0.0}}),
            (rotator, {"DefaultValue": {"Yaw": 90}}, {"DefaultValue": {"Pitch": 0.0, "Roll": 0.0, "Yaw": 90.0}}),
        )
        for key_type, properties, expected in cases:
            with self.subTest(key_type=key_type, properties=properties):
                planned = plan_blackboard_transitions([], [entry("Loc", key_type=key_type, properties=properties)])
                self.assertEqual(planned[0].entry["KeyTypeProperties"], expected)
                self.assertEqual(type(planned[0].entry["KeyTypeProperties"]["DefaultValue"] or {}), dict)
        assert_error(self, "BlackboardData.InvalidEntryField", "/Body/Keys/0/KeyTypeProperties/DefaultValue/W",
                     lambda: plan_blackboard_transitions([], [entry("Loc", key_type=vector, properties={"DefaultValue": {"W": 1}})]))
        assert_error(self, "BlackboardData.InvalidEntryField", "/Body/Keys/0/KeyTypeProperties/DefaultValue/X",
                     lambda: plan_blackboard_transitions([], [entry("Loc", key_type=vector, properties={"DefaultValue": {"X": "1"}})]))
        current = entry("Loc", key_type=vector, properties={"DefaultValue": None})
        moved = entry("Loc", key_type=vector, properties={"DefaultValue": {"X": 1.0, "Y": 0.0, "Z": 0.0}})
        described = current | {"Description": "where to go"}
        self.assertEqual([item.kind for item in plan_blackboard_transitions([current], [moved])], ["replace"])
        self.assertEqual([item.kind for item in plan_blackboard_transitions([current], [described])], ["replace"])
        self.assertEqual([item.kind for item in plan_blackboard_transitions([current], [current])], ["unchanged"])
        base = f"asset:{TARGET_OBJECT}/Keys[name=Loc]"
        self.assertEqual(blackboard_module._entry_change_paths("replace", TARGET_OBJECT, "Loc", current, moved), (f"{base}.KeyTypeProperties",))
        self.assertEqual(blackboard_module._entry_change_paths("replace", TARGET_OBJECT, "Loc", current, described), (f"{base}.Description",))

    def test_value_key_types_need_their_complete_properties(self) -> None:
        # The provider creates a key type with all of its properties: an incomplete set fails canonicalization
        # (validate) with the code the create would fail with.
        profile = BlackboardDataProfileCandidate()
        for key_type, complete in (
            ("/Script/AIModule.BlackboardKeyType_Bool", {"bDefaultValue": True}),
            ("/Script/AIModule.BlackboardKeyType_Int", {"DefaultValue": 3}),
            ("/Script/AIModule.BlackboardKeyType_Float", {"DefaultValue": 1.5}),
            ("/Script/AIModule.BlackboardKeyType_Name", {"DefaultValue": "Alert"}),
            ("/Script/AIModule.BlackboardKeyType_String", {"DefaultValue": "text"}),
        ):
            with self.subTest(key_type=key_type):
                accepted = profile.canonicalize(document(entry("Key", key_type=key_type, properties=complete)))
                self.assertEqual(accepted["Body"]["Keys"][0]["KeyTypeProperties"], complete)
                name = next(iter(complete))
                assert_error(self, "AIProvider.Blackboard.EntryMaterializationUnavailable", f"/Body/Keys/0/KeyTypeProperties/{name}",
                             lambda: profile.validate(document(entry("Key", key_type=key_type) | {"KeyTypeProperties": {}})))
                assert_error(self, "AIProvider.Blackboard.EntryMaterializationUnavailable", "/Body/Keys/0/KeyTypeProperties/Extra",
                             lambda: profile.validate(document(entry("Key", key_type=key_type, properties=complete | {"Extra": 1}))))

    def test_v2_transition_classification_and_call_sequence(self) -> None:
        other_class = "/Script/AIModule.BlackboardKeyType_Bool"
        current = [
            entry("Remove"),
            entry("Keep"),
            entry("Update"),
            entry("Replace"),
        ]
        desired = [
            entry("Replace", key_type=other_class),
            entry("Keep"),
            entry("Add"),
            entry("Update") | {"Description": "changed"},
        ]

        transitions = plan_blackboard_transitions(current, desired)

        self.assertEqual(
            [(item.kind, item.name, item.index, item.ordered_names) for item in transitions],
            [
                ("remove", "Remove", None, ()),
                ("replace", "Replace", None, ()),
                ("unchanged", "Keep", None, ()),
                ("update", "Update", None, ()),
                ("add", "Add", 2, ()),
                ("reorder", None, None, ("Replace", "Keep", "Add", "Update")),
            ],
        )
        self.assertEqual(transitions[1].entry, desired[0])
        self.assertEqual(transitions[3].entry, desired[3])
        self.assertEqual(transitions[4].entry, desired[2])
        assert_error(
            self,
            "BlackboardData.InvalidLocalName",
            "/Body/Keys/0/Name",
            lambda: plan_blackboard_transitions([entry("Foo")], [entry("foo")]),
        )

        replacement = entry("Replace", key_type=other_class)
        addition = entry("Add")
        replace_change = changed(
            "ai.blackboard.entry.replace_class.v2",
            f"asset:{TARGET_OBJECT}/Keys[name=Replace].KeyTypeClass",
        )
        replace_properties_change = changed(
            "ai.blackboard.entry.replace_class.v2",
            f"asset:{TARGET_OBJECT}/Keys[name=Replace].KeyTypeProperties",
        )
        add_entry_change = changed("ai.blackboard.entry.add.v2", f"asset:{TARGET_OBJECT}/Keys[name=Add]")
        add_order_change = changed("ai.blackboard.entry.add.v2", f"asset:{TARGET_OBJECT}/Keys.@Order")
        apply_context = PrimitiveContext(
            RuntimePhase.APPLY,
            [
                (
                    "ai.blackboard.entry.replace_class.v2",
                    outcome(
                        {"entry": replacement, "ordered_local_key_names": ["Replace"]},
                        replace_change,
                        replace_properties_change,
                    ),
                ),
                (
                    "ai.blackboard.entry.add.v2",
                    outcome(
                        {"entry": addition, "ordered_local_key_names": ["Replace", "Add"]},
                        add_entry_change,
                        add_order_change,
                    ),
                ),
            ],
        )
        self.assertEqual(
            BlackboardDataProfileCandidate().apply(
                document(replacement, addition),
                current_snapshot=BlackboardSnapshot(None, (entry("Replace"),), ("Replace",)),
                context=apply_context,
                mutable_asset_handle=object(),
            ),
            (replace_change, replace_properties_change, add_entry_change, add_order_change),
        )
        self.assertEqual(
            [call[1] for call in apply_context.calls],
            [
                {"blackboard_path": TARGET_OBJECT, "entry": replacement, "key_name": "Replace"},
                {
                    "blackboard_path": TARGET_OBJECT,
                    "entry": addition,
                    "insertion": {"Kind": "AtIndex", "Index": 1},
                },
            ],
        )
        self.assertTrue(all(len(call[2]) == 1 and call[2][0].name == "asset" for call in apply_context.calls))
        apply_context.assert_drained(self)


        invalid_envelopes = (
            (
                "changed-set",
                outcome({"entry": addition, "ordered_local_key_names": ["Add"]}),
                "BlackboardData.ChangedSetMismatch",
                "/PrimitiveOutcome/ChangedSet",
            ),
            (
                "handle-results",
                PrimitiveOutcome(
                    PrimitiveOutcomeStatus.SUCCEEDED,
                    {"entry": addition, "ordered_local_key_names": ["Add"]},
                    (NamedOpaqueHandleResult("asset", (object(),)),),
                    (add_entry_change, add_order_change),
                    (),
                ),
                "BlackboardData.InvalidSuccessEnvelope",
                "/PrimitiveOutcome/HandleResults",
            ),
        )
        for label, primitive_outcome, code, path in invalid_envelopes:
            with self.subTest(envelope=label):
                invalid_context = PrimitiveContext(
                    RuntimePhase.APPLY,
                    [("ai.blackboard.entry.add.v2", primitive_outcome)],
                )
                assert_error(
                    self,
                    code,
                    path,
                    lambda invalid_context=invalid_context: BlackboardDataProfileCandidate().apply(
                        document(addition),
                        current_snapshot=BlackboardSnapshot(None, (), ()),
                        context=invalid_context,
                        mutable_asset_handle=object(),
                    ),
                )
                self.assertEqual(len(invalid_context.calls), 1)
                invalid_context.assert_drained(self)

    def test_footprint_projection_freezes_parent_then_update_provider_rows(self) -> None:
        current = entry("Current")
        desired = entry("Current") | {"Description": "changed"}
        transitions = plan_blackboard_transitions([current], [desired])
        projection_builder = getattr(
            blackboard_module, "build_blackboard_footprint_projection", None
        )
        self.assertIsNotNone(projection_builder)

        projection = projection_builder(
            request_id="00000000-0000-0000-0000-000000000001",
            runtime_revision=17,
            target_asset_path=TARGET_OBJECT,
            operation="Apply",
            semantic_hash="adsh:v1:document:example",
            current_parent_path_or_null=None,
            desired_parent_path_or_null=PARENT_OBJECT,
            transitions=transitions,
        )

        self.assertEqual(
            projection,
            {
                "Kind": "BlackboardFootprintProjectionV1",
                "RequestId": "00000000-0000-0000-0000-000000000001",
                "RuntimeRevision": 17,
                "ExactAssetClassPath": BLACKBOARD_DATA_CLASS,
                "TargetAssetPath": TARGET_OBJECT,
                "Operation": "Apply",
                "SemanticHash": "adsh:v1:document:example",
                "Steps": [
                    {
                        "Index": 0,
                        "PrimitiveId": "ai.blackboard.parent.set.v2",
                        "RawFootprint": [
                            {
                                "Kind": "UObjectState",
                                "StableId": f"asset:{TARGET_OBJECT}/Keys[*]",
                                "RollbackComponentId": "assetdocument.ai.blackboard.parent.derived_closure.v2",
                            },
                            {
                                "Kind": "UObjectState",
                                "StableId": f"asset:{TARGET_OBJECT}/Parent",
                                "RollbackComponentId": "assetdocument.ai.blackboard.parent.reference.v2",
                            },
                        ],
                    },
                    {
                        "Index": 1,
                        "PrimitiveId": "ai.blackboard.entry.update_existing.v2",
                        "RawFootprint": [
                            {
                                "Kind": "UObjectState",
                                "StableId": f"asset:{TARGET_OBJECT}/Keys[name=Current].LocalKeysMetadataAggregate",
                                "RollbackComponentId": "assetdocument.ai.blackboard.entry_metadata.v2",
                            },
                            {
                                "Kind": "UObjectState",
                                "StableId": f"asset:{TARGET_OBJECT}/Keys[name=Current].KeyTypeProperties",
                                "RollbackComponentId": "assetdocument.ai.blackboard.key_type_properties.v2",
                            },
                        ],
                    },
                ],
            },
        )

    def test_footprint_projection_uses_frozen_structural_provider_components(self) -> None:
        current = [entry("Remove"), entry("Replace"), entry("Keep")]
        desired = [
            entry("Keep"),
            entry("Replace", key_type="/Script/AIModule.BlackboardKeyType_Vector"),
            entry("Add"),
        ]
        projection = blackboard_module.build_blackboard_footprint_projection(
            request_id="00000000-0000-0000-0000-000000000001",
            runtime_revision=17,
            target_asset_path=TARGET_OBJECT,
            operation="Apply",
            semantic_hash="adsh:v1:document:example",
            current_parent_path_or_null=None,
            desired_parent_path_or_null=None,
            transitions=plan_blackboard_transitions(current, desired),
        )

        self.assertEqual(
            [
                (step["Index"], step["PrimitiveId"], step["RawFootprint"])
                for step in projection["Steps"]
            ],
            [
                (0, "ai.blackboard.entry.remove.v2", [{
                    "Kind": "UObjectState",
                    "StableId": f"asset:{TARGET_OBJECT}/Keys[*]",
                    "RollbackComponentId": "assetdocument.ai.blackboard.entry_remove.v2",
                }]),
                (1, "ai.blackboard.entry.replace_class.v2", [{
                    "Kind": "UObjectState",
                    "StableId": f"asset:{TARGET_OBJECT}/Keys[*]",
                    "RollbackComponentId": "assetdocument.ai.blackboard.entry.replace_class.v2",
                }]),
                (2, "ai.blackboard.entry.add.v2", [{
                    "Kind": "UObjectState",
                    "StableId": f"asset:{TARGET_OBJECT}/Keys[*]",
                    "RollbackComponentId": "assetdocument.ai.blackboard.entry.add.v2",
                }]),
                (3, "ai.blackboard.local_keys.reorder.v2", [{
                    "Kind": "UObjectState",
                    "StableId": f"asset:{TARGET_OBJECT}/Keys[*]",
                    "RollbackComponentId": "assetdocument.ai.blackboard.local_keys_reorder.v2",
                }]),
            ],
        )

    def test_versioned_parent_set_clear_shadow_cycle_sequence(self) -> None:
        profile = BlackboardDataProfileCandidate()
        current = entry("Current")
        parent_only = entry("ParentOnly")
        grand_key = entry("GrandKey")
        complete_chain_context = PrimitiveContext(
            RuntimePhase.PREFLIGHT,
            [
                ("ai.blackboard.parent.readback.v2", outcome({"parent_path_or_null": None})),
                ("ai.blackboard.local_keys.readback.v2", outcome({"ordered_local_key_names": ["Current"]})),
                ("ai.blackboard.parent.readback.v2", outcome({"parent_path_or_null": GRANDPARENT_OBJECT})),
                ("ai.blackboard.local_keys.readback.v2", outcome({"ordered_local_key_names": ["ParentOnly"]})),
                ("ai.blackboard.parent.readback.v2", outcome({"parent_path_or_null": None})),
                ("ai.blackboard.local_keys.readback.v2", outcome({"ordered_local_key_names": ["GrandKey"]})),
                ("ai.blackboard.entry.readback.v2", outcome({"entry": current})),
                ("ai.blackboard.entry.readback.v2", outcome({"entry": parent_only})),
                ("ai.blackboard.entry.readback.v2", outcome({"entry": grand_key})),
            ],
        )
        self.assertEqual(
            profile.preflight(document(parent=asset_ref(PARENT_OBJECT)), context=complete_chain_context),
            BlackboardSnapshot(None, (current,), ("Current",)),
        )
        self.assertEqual(
            [call[1] for call in complete_chain_context.calls],
            [
                {"blackboard_path": TARGET_OBJECT},
                {"blackboard_path": TARGET_OBJECT},
                {"blackboard_path": PARENT_OBJECT},
                {"blackboard_path": PARENT_OBJECT},
                {"blackboard_path": GRANDPARENT_OBJECT},
                {"blackboard_path": GRANDPARENT_OBJECT},
                {"blackboard_path": TARGET_OBJECT, "key_name": "Current"},
                {"blackboard_path": PARENT_OBJECT, "key_name": "ParentOnly"},
                {"blackboard_path": GRANDPARENT_OBJECT, "key_name": "GrandKey"},
            ],
        )
        complete_chain_context.assert_drained(self)

        old_key = entry("Old")
        new_key = entry("New")
        shared_key = entry("Shared")
        root_key = entry("Root")
        shared_suffix_context = PrimitiveContext(
            RuntimePhase.PREFLIGHT,
            [
                ("ai.blackboard.parent.readback.v2", outcome({"parent_path_or_null": OLD_PARENT_OBJECT})),
                ("ai.blackboard.local_keys.readback.v2", outcome({"ordered_local_key_names": ["Current"]})),
                ("ai.blackboard.parent.readback.v2", outcome({"parent_path_or_null": SHARED_PARENT_OBJECT})),
                ("ai.blackboard.local_keys.readback.v2", outcome({"ordered_local_key_names": ["Old"]})),
                ("ai.blackboard.parent.readback.v2", outcome({"parent_path_or_null": ROOT_PARENT_OBJECT})),
                ("ai.blackboard.local_keys.readback.v2", outcome({"ordered_local_key_names": ["Shared"]})),
                ("ai.blackboard.parent.readback.v2", outcome({"parent_path_or_null": None})),
                ("ai.blackboard.local_keys.readback.v2", outcome({"ordered_local_key_names": ["Root"]})),
                ("ai.blackboard.parent.readback.v2", outcome({"parent_path_or_null": SHARED_PARENT_OBJECT})),
                ("ai.blackboard.local_keys.readback.v2", outcome({"ordered_local_key_names": ["New"]})),
                ("ai.blackboard.entry.readback.v2", outcome({"entry": current})),
                ("ai.blackboard.entry.readback.v2", outcome({"entry": old_key})),
                ("ai.blackboard.entry.readback.v2", outcome({"entry": new_key})),
                ("ai.blackboard.entry.readback.v2", outcome({"entry": shared_key})),
                ("ai.blackboard.entry.readback.v2", outcome({"entry": root_key})),
            ],
        )
        self.assertEqual(
            profile.preflight(document(parent=asset_ref(PARENT_OBJECT)), context=shared_suffix_context),
            BlackboardSnapshot(OLD_PARENT_OBJECT, (current,), ("Current",)),
        )
        self.assertEqual(
            [call[1] for call in shared_suffix_context.calls],
            [
                {"blackboard_path": TARGET_OBJECT},
                {"blackboard_path": TARGET_OBJECT},
                {"blackboard_path": OLD_PARENT_OBJECT},
                {"blackboard_path": OLD_PARENT_OBJECT},
                {"blackboard_path": SHARED_PARENT_OBJECT},
                {"blackboard_path": SHARED_PARENT_OBJECT},
                {"blackboard_path": ROOT_PARENT_OBJECT},
                {"blackboard_path": ROOT_PARENT_OBJECT},
                {"blackboard_path": PARENT_OBJECT},
                {"blackboard_path": PARENT_OBJECT},
                {"blackboard_path": TARGET_OBJECT, "key_name": "Current"},
                {"blackboard_path": OLD_PARENT_OBJECT, "key_name": "Old"},
                {"blackboard_path": PARENT_OBJECT, "key_name": "New"},
                {"blackboard_path": SHARED_PARENT_OBJECT, "key_name": "Shared"},
                {"blackboard_path": ROOT_PARENT_OBJECT, "key_name": "Root"},
            ],
        )
        shared_suffix_context.assert_drained(self)

        shadow_context = PrimitiveContext(
            RuntimePhase.PREFLIGHT,
            [
                ("ai.blackboard.parent.readback.v2", outcome({"parent_path_or_null": None})),
                ("ai.blackboard.local_keys.readback.v2", outcome({"ordered_local_key_names": []})),
                ("ai.blackboard.parent.readback.v2", outcome({"parent_path_or_null": None})),
                ("ai.blackboard.local_keys.readback.v2", outcome({"ordered_local_key_names": ["Inherited"]})),
            ],
        )
        assert_error(
            self,
            "BlackboardData.InheritedShadow",
            "/Body/Keys/0/Name",
            lambda: profile.preflight(
                document(entry("Inherited"), parent=asset_ref(PARENT_OBJECT)),
                context=shadow_context,
            ),
        )
        shadow_context.assert_drained(self)

        case_shadow_context = PrimitiveContext(
            RuntimePhase.PREFLIGHT,
            [
                ("ai.blackboard.parent.readback.v2", outcome({"parent_path_or_null": None})),
                ("ai.blackboard.local_keys.readback.v2", outcome({"ordered_local_key_names": []})),
                ("ai.blackboard.parent.readback.v2", outcome({"parent_path_or_null": None})),
                ("ai.blackboard.local_keys.readback.v2", outcome({"ordered_local_key_names": ["Inherited"]})),
            ],
        )
        assert_error(
            self,
            "BlackboardData.InheritedShadow",
            "/Body/Keys/0/Name",
            lambda: profile.preflight(document(entry("inherited"), parent=asset_ref(PARENT_OBJECT)), context=case_shadow_context),
        )
        case_shadow_context.assert_drained(self)

        deep_shadow_context = PrimitiveContext(
            RuntimePhase.PREFLIGHT,
            [
                ("ai.blackboard.parent.readback.v2", outcome({"parent_path_or_null": None})),
                ("ai.blackboard.local_keys.readback.v2", outcome({"ordered_local_key_names": []})),
                ("ai.blackboard.parent.readback.v2", outcome({"parent_path_or_null": GRANDPARENT_OBJECT})),
                ("ai.blackboard.local_keys.readback.v2", outcome({"ordered_local_key_names": ["ParentOnly"]})),
                ("ai.blackboard.parent.readback.v2", outcome({"parent_path_or_null": None})),
                ("ai.blackboard.local_keys.readback.v2", outcome({"ordered_local_key_names": ["GrandKey"]})),
            ],
        )
        assert_error(
            self,
            "BlackboardData.InheritedShadow",
            "/Body/Keys/0/Name",
            lambda: profile.preflight(
                document(grand_key, parent=asset_ref(PARENT_OBJECT)),
                context=deep_shadow_context,
            ),
        )
        deep_shadow_context.assert_drained(self)

        chain_cycle_context = PrimitiveContext(
            RuntimePhase.PREFLIGHT,
            [
                ("ai.blackboard.parent.readback.v2", outcome({"parent_path_or_null": None})),
                ("ai.blackboard.local_keys.readback.v2", outcome({"ordered_local_key_names": []})),
                ("ai.blackboard.parent.readback.v2", outcome({"parent_path_or_null": GRANDPARENT_OBJECT})),
                ("ai.blackboard.local_keys.readback.v2", outcome({"ordered_local_key_names": ["ParentOnly"]})),
                ("ai.blackboard.parent.readback.v2", outcome({"parent_path_or_null": PARENT_OBJECT})),
                ("ai.blackboard.local_keys.readback.v2", outcome({"ordered_local_key_names": ["GrandKey"]})),
            ],
        )
        assert_error(
            self,
            "BlackboardData.ParentCycle",
            "/Body/Parent/Path",
            lambda: profile.preflight(
                document(parent=asset_ref(PARENT_OBJECT)),
                context=chain_cycle_context,
            ),
        )
        chain_cycle_context.assert_drained(self)

        cycle_context = PrimitiveContext(
            RuntimePhase.APPLY,
            [("ai.blackboard.parent.set.v2", failure("AIProvider.Blackboard.ParentCycle", "/Parent"))],
        )
        assert_error(
            self,
            "AIProvider.Blackboard.ParentCycle",
            "/Parent",
            lambda: profile.apply(
                document(parent=asset_ref(PARENT_OBJECT)),
                current_snapshot=BlackboardSnapshot(None, (), ()),
                context=cycle_context,
                mutable_asset_handle=object(),
            ),
        )
        self.assertEqual([call[0] for call in cycle_context.calls], ["ai.blackboard.parent.set.v2"])

        spelling_context = PrimitiveContext(RuntimePhase.APPLY, [])
        assert_error(
            self,
            "BlackboardData.InvalidLocalName",
            "/Body/Keys/0/Name",
            lambda: profile.apply(
                document(entry("foo"), parent=asset_ref(PARENT_OBJECT)),
                current_snapshot=BlackboardSnapshot(None, (entry("Foo"),), ("Foo",)),
                context=spelling_context,
                mutable_asset_handle=object(),
            ),
        )
        self.assertEqual(spelling_context.calls, [])

        parent_change = changed("ai.blackboard.parent.set.v2", f"asset:{TARGET_OBJECT}/Parent")
        clear_context = PrimitiveContext(
            RuntimePhase.APPLY,
            [("ai.blackboard.parent.set.v2", outcome({"parent_path_or_null": None}, parent_change))],
        )
        self.assertEqual(
            profile.apply(
                document(),
                current_snapshot=BlackboardSnapshot(PARENT_OBJECT, (), ()),
                context=clear_context,
                mutable_asset_handle=object(),
            ),
            (parent_change,),
        )
        clear_context.assert_drained(self)

    def test_v2_remove_full_remaining_entries_and_order(self) -> None:
        profile = BlackboardDataProfileCandidate()
        first, removed, last = entry("First"), entry("Removed"), entry("Last")
        entry_change = changed("ai.blackboard.entry.remove.v2", f"asset:{TARGET_OBJECT}/Keys[name=Removed]")
        order_change = changed("ai.blackboard.entry.remove.v2", f"asset:{TARGET_OBJECT}/Keys.@Order")
        context = PrimitiveContext(
            RuntimePhase.APPLY,
            [
                (
                    "ai.blackboard.entry.remove.v2",
                    outcome(
                        {
                            "entries": [first, last],
                            "ordered_local_key_names": ["First", "Last"],
                            "removed_name": "Removed",
                        },
                        entry_change,
                        order_change,
                    ),
                )
            ],
        )

        result = profile.apply(
            document(first, last),
            current_snapshot=BlackboardSnapshot(None, (first, removed, last), ("First", "Removed", "Last")),
            context=context,
            mutable_asset_handle=object(),
        )

        self.assertEqual(result, (entry_change, order_change))
        self.assertEqual(context.calls[0][1], {"blackboard_path": TARGET_OBJECT, "key_name": "Removed"})
        self.assertEqual(context.calls[0][2][0].name, "asset")
        context.assert_drained(self)

    def test_v2_reorder_exact_permutation_and_key_id_evidence(self) -> None:
        profile = BlackboardDataProfileCandidate()
        first, second = entry("First"), entry("Second")
        key_id_evidence = changed("ai.blackboard.local_keys.reorder.v2", f"asset:{TARGET_OBJECT}/Keys.@Order")
        context = PrimitiveContext(
            RuntimePhase.APPLY,
            [
                (
                    "ai.blackboard.local_keys.reorder.v2",
                    outcome(
                        {"entries": [second, first], "ordered_local_key_names": ["Second", "First"]},
                        key_id_evidence,
                    ),
                )
            ],
        )

        result = profile.apply(
            document(second, first),
            current_snapshot=BlackboardSnapshot(None, (first, second), ("First", "Second")),
            context=context,
            mutable_asset_handle=object(),
        )

        self.assertEqual(result, (key_id_evidence,))
        self.assertEqual(
            context.calls[0][1],
            {"blackboard_path": TARGET_OBJECT, "ordered_local_key_names": ["Second", "First"]},
        )
        context.assert_drained(self)

    def test_first_terminal_stops_without_retry_or_compensation(self) -> None:
        profile = BlackboardDataProfileCandidate()
        current = entry("Current")
        desired = entry("Current") | {"Description": "changed"}
        context = PrimitiveContext(
            RuntimePhase.APPLY,
            [("ai.blackboard.entry.update_existing.v2", failure("AIProvider.OperationRejected"))],
        )

        assert_error(
            self,
            "AIProvider.OperationRejected",
            "/Primitive",
            lambda: profile.apply(
                document(desired, entry("Later")),
                current_snapshot=BlackboardSnapshot(None, (current,), ("Current",)),
                context=context,
                mutable_asset_handle=object(),
            ),
        )
        self.assertEqual([call[0] for call in context.calls], ["ai.blackboard.entry.update_existing.v2"])
        context.assert_drained(self)

        malformed_failure = type(
            "MalformedOutcome",
            (),
            {"status": PrimitiveOutcomeStatus.FAILED, "value": {"entry": current}, "handle_results": (), "changed_set": (), "diagnostics": (RuntimeDiagnostic("/Primitive", "AIProvider.OperationRejected", "bad"),)},
        )()
        malformed_context = PrimitiveContext(RuntimePhase.APPLY, [("ai.blackboard.entry.update_existing.v2", malformed_failure)])
        assert_error(
            self,
            "BlackboardData.InvalidSuccessEnvelope",
            "/PrimitiveOutcome",
            lambda: profile.apply(
                document(desired),
                current_snapshot=BlackboardSnapshot(None, (current,), ("Current",)),
                context=malformed_context,
                mutable_asset_handle=object(),
            ),
        )
        success_diagnostic = PrimitiveOutcome(
            PrimitiveOutcomeStatus.SUCCEEDED,
            {"entry": desired, "ordered_local_key_names": ["Current"]},
            (),
            (changed("ai.blackboard.entry.update_existing.v2", f"asset:{TARGET_OBJECT}/Keys[name=Current].Description"),),
            (RuntimeDiagnostic("/Primitive", "AIProvider.Unexpected", "bad"),),
        )
        diagnostic_context = PrimitiveContext(RuntimePhase.APPLY, [("ai.blackboard.entry.update_existing.v2", success_diagnostic)])
        assert_error(
            self,
            "BlackboardData.InvalidSuccessEnvelope",
            "/PrimitiveOutcome/Diagnostics",
            lambda: profile.apply(
                document(desired),
                current_snapshot=BlackboardSnapshot(None, (current,), ("Current",)),
                context=diagnostic_context,
                mutable_asset_handle=object(),
            ),
        )

    def test_full_readback_and_order_mismatch_fail_closed(self) -> None:
        profile = BlackboardDataProfileCandidate()
        current = entry("Current")
        desired = entry("Current") | {"Description": "changed"}
        update_change = changed("ai.blackboard.entry.update_existing.v2", f"asset:{TARGET_OBJECT}/Keys[name=Current].Description")
        context = PrimitiveContext(
            RuntimePhase.APPLY,
            [
                (
                    "ai.blackboard.entry.update_existing.v2",
                    outcome(
                        {"entry": current, "ordered_local_key_names": ["Current"]},
                        update_change,
                    ),
                )
            ],
        )

        assert_error(
            self,
            "BlackboardData.ReadbackMismatch",
            "/PrimitiveOutcome/Value",
            lambda: profile.apply(
                document(desired),
                current_snapshot=BlackboardSnapshot(None, (current,), ("Current",)),
                context=context,
                mutable_asset_handle=object(),
            ),
        )
        context.assert_drained(self)

    def test_extract_and_diff_preserve_parent_entries_order_and_full_state(self) -> None:
        profile = BlackboardDataProfileCandidate()
        first = entry("First", properties={"BaseClass": {"Kind": "ClassRef", "Path": "/Script/Engine.Actor"}})
        second = entry("Second", properties={"BaseClass": {"Kind": "ClassRef", "Path": "/Script/Engine.Pawn"}}) | {"Category": "State", "Description": "second", "bInstanceSynced": True}
        snapshot = BlackboardSnapshot(PARENT_OBJECT, (first, second), ("First", "Second"))

        extracted = profile.extract(target=TARGET, current_snapshot=snapshot)
        self.assertEqual(extracted["Body"], {"Parent": asset_ref(PARENT_OBJECT), "Keys": [first, second]})
        self.assertEqual(profile.diff(extracted, extracted), ())
        changed_document = document(second, first, parent=None)
        paths = {difference.path for difference in profile.diff(extracted, changed_document)}
        self.assertIn("/Body/Parent", paths)
        self.assertIn("/Body/Keys/0/Name", paths)
        self.assertIn("/Body/Keys/0/Category", paths)
        self.assertIn("/Body/Keys/0/Description", paths)
        self.assertIn("/Body/Keys/0/KeyTypeProperties/BaseClass/Path", paths)
        self.assertIn("/Body/Keys/0/bInstanceSynced", paths)

    def test_repair_is_completion_noop_and_wrong_phase_fails_closed(self) -> None:
        profile = BlackboardDataProfileCandidate()
        repair = PrimitiveContext(RuntimePhase.REPAIR, [])
        self.assertEqual(profile.repair(document(), context=repair), ())
        self.assertEqual(repair.calls, [])
        assert_error(
            self,
            "BlackboardData.InvalidRepairPhase",
            "/Phase",
            lambda: profile.repair(document(), context=PrimitiveContext(RuntimePhase.APPLY, [])),
        )


if __name__ == "__main__":
    unittest.main()
