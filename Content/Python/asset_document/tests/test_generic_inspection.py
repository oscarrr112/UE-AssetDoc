from __future__ import annotations

import unittest

from asset_document.diagnostics import AssetDocumentContractError
from asset_document.dto import RuntimePhase
from asset_document.regions.inspection_semantics import InspectionSemanticAdapter
from asset_document.tests.fixtures.generic_cases import (
    EXACT_CLASS,
    MockPrimitiveContext,
    failed_outcome,
    success_outcome,
)


EDITOR_ID = "editor.property.inspect_evidence.v1"
AI_ID = "ai.behavior_tree.selector_inspect_evidence.v1"
ASSET_PATH = "/Game/Generic/DA_Inspect.DA_Inspect"


def subject(mode: str = "class_defaults") -> dict:
    value = {"kind": mode, "exact_class_path": EXACT_CLASS}
    if mode == "live_asset":
        value["asset_path"] = ASSET_PATH
    return value


def editor_value(
    *,
    mode: str = "class_defaults",
    admitted: list[dict] | None = None,
    rejected: list[dict] | None = None,
) -> dict:
    return {
        "subject": subject(mode),
        "admitted": [] if admitted is None else admitted,
        "rejected": [] if rejected is None else rejected,
    }


def ai_value(*, mode: str = "class_defaults", selectors: list[dict] | None = None) -> dict:
    return {
        "subject": subject(mode),
        "selectors": [] if selectors is None else selectors,
    }


def property_row(
    name: str = "Health",
    *,
    codec_id: str = "number.v1",
    ue_cpp_type: str = "float",
    current_value=125.0,
    default_value=100.0,
    declaring_class: str = EXACT_CLASS,
) -> dict:
    return {
        "name": name,
        "declaring_class": declaring_class,
        "ue_cpp_type": ue_cpp_type,
        "codec_id": codec_id,
        "writable": True,
        "current_value": current_value,
        "default_value": default_value,
    }


def rejected_row(name: str = "TransientState", *, reason_code: str = "Transient") -> dict:
    return {
        "name": name,
        "declaring_class": EXACT_CLASS,
        "ue_cpp_type": "int32",
        "reason_code": reason_code,
    }


def selector_row(path: str = "DirectSelector") -> dict:
    return {
        "property_path": path,
        "authored_field_name": "Key",
        "read_only": True,
        "none_allowed": False,
        "allowed_types": [
            {
                "filter_type_path": "/Script/AIModule.BlackboardKeyType_Object",
                "base_class_path": "/Script/Engine.Actor",
            }
        ],
    }


def assert_error(test: unittest.TestCase, callback, code: str = "GenericRuntime.InvalidNativeProjection") -> None:
    with test.assertRaises(AssetDocumentContractError) as caught:
        callback()
    test.assertEqual(caught.exception.diagnostic.code, code)


class GenericInspectionTests(unittest.TestCase):
    def _adapter(self) -> InspectionSemanticAdapter:
        return InspectionSemanticAdapter()

    def _queue(self, context: MockPrimitiveContext, editor: object, ai: object) -> None:
        context.queue_outcomes(EDITOR_ID, editor)
        context.queue_outcomes(AI_ID, ai)

    def test_class_defaults_projects_exact_five_field_payload_and_no_handles(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.VERIFY)
        self._queue(
            context,
            success_outcome(value=editor_value(admitted=[property_row(default_value=125.0)])),
            success_outcome(value=ai_value()),
        )

        result = self._adapter().inspect(
            mode="class_defaults",
            exact_class_path=EXACT_CLASS,
            exact_asset_object_path=None,
            editor_asset_handle=None,
            context=context,
        )

        self.assertEqual(set(result), {"class", "properties", "skipped", "selector_policies"})
        self.assertEqual(result["class"], EXACT_CLASS)
        self.assertNotIn("asset_path", result)
        self.assertEqual(result["properties"], [{"name": "Health", "ue_type": "float", "type_token": "Float", "current_value": 125.0, "default_value": 125.0, "writable": True}])
        self.assertEqual(result["skipped"], [])
        self.assertEqual(result["selector_policies"], [])
        self.assertEqual(
            [call.requirement.primitive_id for call in context.calls],
            [EDITOR_ID, AI_ID],
        )
        self.assertEqual(context.calls[0].value_arguments, {"mode": "class_defaults", "exact_asset_class_path": EXACT_CLASS})
        self.assertEqual(context.calls[0].handle_arguments, ())
        self.assertEqual(context.calls[1].value_arguments, {"mode": "class_defaults", "exact_asset_class_path": EXACT_CLASS})
        self.assertEqual(context.calls[1].handle_arguments, ())

    def test_live_asset_passes_editor_handle_only_and_ai_uses_paths(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.VERIFY)
        handle = context.issue_handle()
        self._queue(
            context,
            success_outcome(value=editor_value(mode="live_asset")),
            success_outcome(value=ai_value(mode="live_asset")),
        )

        result = self._adapter().inspect(
            mode="live_asset",
            exact_class_path=EXACT_CLASS,
            exact_asset_object_path=ASSET_PATH,
            editor_asset_handle=handle,
            context=context,
        )

        self.assertEqual(result["asset_path"], ASSET_PATH)
        self.assertEqual(context.calls[0].value_arguments, {"mode": "live_asset", "exact_asset_class_path": EXACT_CLASS})
        self.assertEqual(tuple(item.name for item in context.calls[0].handle_arguments), ("asset",))
        self.assertIs(context.calls[0].handle_arguments[0].handle, handle)
        self.assertEqual(context.calls[1].value_arguments, {"mode": "live_asset", "exact_asset_class_path": EXACT_CLASS, "exact_asset_object_path": ASSET_PATH})
        self.assertEqual(context.calls[1].handle_arguments, ())

    def test_noncanonical_live_path_fails_before_editor_or_ai_invocation(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.VERIFY)
        handle = context.issue_handle()
        self._queue(
            context,
            success_outcome(value=editor_value(mode="live_asset")),
            success_outcome(value=ai_value(mode="live_asset")),
        )

        assert_error(
            self,
            lambda: self._adapter().inspect(
                mode="live_asset",
                exact_class_path=EXACT_CLASS,
                exact_asset_object_path="not/a/canonical/object/path",
                editor_asset_handle=handle,
                context=context,
            ),
        )

        self.assertEqual(context.calls, ())

    def test_normalizes_codecs_sorts_set_and_rows_and_maps_rejections(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.VERIFY)
        handle = context.issue_handle()
        rows = [
            property_row("Zed", codec_id="set.v1", ue_cpp_type="TSet<FString>", current_value=["z", "a"], default_value=["b", "a"]),
            property_row("Alpha", codec_id="bool.v1", ue_cpp_type="bool", current_value=True, default_value=False),
            property_row("Record", codec_id="struct.v1", ue_cpp_type="FRecord", current_value={"z": 1}, default_value={"a": 1}),
        ]
        rejected = [
            rejected_row("Zulu", reason_code="Transient"),
            rejected_row("AlphaSkip", reason_code="UnsupportedCodec"),
            rejected_row("BodySetup", reason_code="EditorDerivedReference"),
            rejected_row("Materials", reason_code="SavedByAsset"),
        ]
        selectors = [selector_row("Z/Selector"), selector_row("A/Selector")]
        selectors[0]["allowed_types"] = [
            {"filter_type_path": "/Script/Z.Type", "base_class_path": "/Script/Z.Base"},
            {"filter_type_path": "/Script/A.Type", "base_class_path": None},
        ]
        self._queue(
            context,
            success_outcome(value=editor_value(mode="live_asset", admitted=rows, rejected=rejected)),
            success_outcome(value=ai_value(mode="live_asset", selectors=selectors)),
        )

        result = self._adapter().inspect(mode="live_asset", exact_class_path=EXACT_CLASS, exact_asset_object_path=ASSET_PATH, editor_asset_handle=handle, context=context)

        self.assertEqual([item["name"] for item in result["properties"]], ["Alpha", "Record", "Zed"])
        self.assertEqual(result["properties"][0]["type_token"], "Bool")
        self.assertEqual(result["properties"][1]["type_token"], "FRecord")
        self.assertEqual(result["properties"][2]["current_value"], ["a", "z"])
        self.assertEqual(result["properties"][2]["default_value"], ["a", "b"])
        self.assertEqual(result["skipped"], [
            {"name": "AlphaSkip", "reason": "unsupported-serialization"},
            {"name": "BodySetup", "reason": "editor-derived-reference"},
            {"name": "Materials", "reason": "saved-by-asset"},
            {"name": "Zulu", "reason": "transient"},
        ])
        self.assertEqual([item["path"] for item in result["selector_policies"]], ["A/Selector", "Z/Selector"])
        self.assertEqual(result["selector_policies"][1]["allowed_types"], [{"filter_type": "/Script/A.Type", "base_class": None}, {"filter_type": "/Script/Z.Type", "base_class": "/Script/Z.Base"}])

    def test_option_n_codec_rules_require_the_exact_value_shape_and_public_token(self) -> None:
        rows = [
            property_row("Bool", codec_id="bool.v1", ue_cpp_type="bool", current_value=True, default_value=True),
            property_row("Int", codec_id="integer.v1", ue_cpp_type="int64", current_value=7, default_value=7),
            property_row("Float", codec_id="number.v1", ue_cpp_type="double", current_value=1.5, default_value=1.5),
            property_row("Enum", codec_id="enum.v1", ue_cpp_type="EState", current_value="Ready", default_value="Ready"),
            property_row("String", codec_id="string.v1", ue_cpp_type="FString", current_value="value", default_value="value"),
            property_row("Name", codec_id="name.v1", ue_cpp_type="FName", current_value="Value", default_value="Value"),
            property_row("Text", codec_id="text.v1", ue_cpp_type="FText", current_value="value", default_value="value"),
            property_row("Object", codec_id="soft_object_path.v1", ue_cpp_type="TSoftObjectPtr<UObject>", current_value=None, default_value=None),
            property_row("Class", codec_id="soft_class_path.v1", ue_cpp_type="TSoftClassPtr<UObject>", current_value=None, default_value=None),
            property_row("Struct", codec_id="struct.v1", ue_cpp_type="FValue", current_value={"X": 1}, default_value={"X": 1}),
            property_row("Array", codec_id="array.v1", ue_cpp_type="TArray<int32>", current_value=[1], default_value=[1]),
            property_row("Map", codec_id="string_key_map.v1", ue_cpp_type="TMap<FString, int32>", current_value={"One": 1}, default_value={"One": 1}),
            property_row("Set", codec_id="set.v1", ue_cpp_type="TSet<FString>", current_value=["a"], default_value=["a"]),
        ]
        context = MockPrimitiveContext(RuntimePhase.VERIFY)
        self._queue(context, success_outcome(value=editor_value(admitted=rows)), success_outcome(value=ai_value()))

        result = self._adapter().inspect(mode="class_defaults", exact_class_path=EXACT_CLASS, exact_asset_object_path=None, editor_asset_handle=None, context=context)

        self.assertEqual(
            {item["name"]: item["type_token"] for item in result["properties"]},
            {"Bool": "Bool", "Int": "Int", "Float": "Float", "Enum": "Enum", "String": "String", "Name": "Name", "Text": "Text", "Object": "Object", "Class": "Class", "Struct": "FValue", "Array": "Array", "Map": "Map", "Set": "Set"},
        )

    def test_reference_instanced_and_text_form_struct_codecs_project_type_tokens(self) -> None:
        instanced = {"Class": "/Script/Engine.Texture2D", "Properties": {"LODBias": 1}}
        rows = [
            property_row("Hard", codec_id="object_path.v1", ue_cpp_type="TObjectPtr<UTexture>", current_value="/Game/T.T", default_value="/Game/T.T"),
            property_row("Subclass", codec_id="class_path.v1", ue_cpp_type="TSubclassOf<UObject>", current_value="/Script/Engine.Actor", default_value="/Script/Engine.Actor"),
            property_row("Inline", codec_id="instanced_object.v1", ue_cpp_type="TObjectPtr<UObject>", current_value=instanced, default_value=instanced),
            property_row("Payload", codec_id="instanced_struct.v1", ue_cpp_type="FInstancedStruct", current_value=None, default_value=None),
            property_row("Id", codec_id="struct_text.v1", ue_cpp_type="FGuid", current_value="00000000000000000000000000000001", default_value="00000000000000000000000000000001"),
        ]
        context = MockPrimitiveContext(RuntimePhase.VERIFY)
        self._queue(context, success_outcome(value=editor_value(admitted=rows)), success_outcome(value=ai_value()))

        result = self._adapter().inspect(mode="class_defaults", exact_class_path=EXACT_CLASS, exact_asset_object_path=None, editor_asset_handle=None, context=context)

        self.assertEqual(
            {item["name"]: item["type_token"] for item in result["properties"]},
            {"Hard": "Object", "Subclass": "Class", "Inline": "InstancedObject", "Payload": "InstancedStruct", "Id": "FGuid"},
        )
        self.assertEqual(next(item for item in result["properties"] if item["name"] == "Inline")["current_value"], instanced)

    def test_new_codecs_reject_values_of_the_wrong_json_type(self) -> None:
        for codec_id, value in (("instanced_object.v1", "/Script/Engine.Texture2D"), ("instanced_struct.v1", []), ("struct_text.v1", {"A": 1})):
            with self.subTest(codec_id=codec_id):
                context = MockPrimitiveContext(RuntimePhase.VERIFY)
                self._queue(
                    context,
                    success_outcome(value=editor_value(admitted=[property_row(codec_id=codec_id, current_value=value, default_value=None)])),
                    success_outcome(value=ai_value()),
                )
                assert_error(self, lambda: self._adapter().inspect(mode="class_defaults", exact_class_path=EXACT_CLASS, exact_asset_object_path=None, editor_asset_handle=None, context=context), "GenericRuntime.InvalidNativeProjection")

    def test_active_instanced_struct_unsupported_codec_is_one_skipped_row_without_partial_child(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.VERIFY)
        self._queue(
            context,
            success_outcome(value=editor_value(rejected=[rejected_row("DynamicValue", reason_code="UnsupportedCodec")])),
            success_outcome(value=ai_value()),
        )

        result = self._adapter().inspect(mode="class_defaults", exact_class_path=EXACT_CLASS, exact_asset_object_path=None, editor_asset_handle=None, context=context)

        self.assertEqual(result["properties"], [])
        self.assertEqual(result["skipped"], [{"name": "DynamicValue", "reason": "unsupported-serialization"}])

    def test_optional_selector_base_class_projects_none(self) -> None:
        context = MockPrimitiveContext(RuntimePhase.VERIFY)
        selector = selector_row()
        selector["allowed_types"] = [{"filter_type_path": "/Script/AIModule.BlackboardKeyType_Bool"}]
        self._queue(
            context,
            success_outcome(value=editor_value()),
            success_outcome(value=ai_value(selectors=[selector])),
        )

        result = self._adapter().inspect(
            mode="class_defaults",
            exact_class_path=EXACT_CLASS,
            exact_asset_object_path=None,
            editor_asset_handle=None,
            context=context,
        )

        self.assertEqual(
            result["selector_policies"][0]["allowed_types"],
            [{"filter_type": "/Script/AIModule.BlackboardKeyType_Bool", "base_class": None}],
        )

    def test_fails_closed_for_bad_dto_identity_unknown_codec_and_primitive_failures(self) -> None:
        cases = [
            (editor_value(admitted=[property_row(codec_id="unknown.v1")]), ai_value(), "GenericRuntime.InvalidNativeProjection"),
            ({**editor_value(), "extra": True}, ai_value(), "GenericRuntime.InvalidNativeProjection"),
            (editor_value(), {"subject": subject(), "selectors": [], "extra": True}, "GenericRuntime.InvalidNativeProjection"),
            (editor_value(mode="live_asset"), ai_value(), "GenericRuntime.InvalidNativeProjection"),
            (failed_outcome(), success_outcome(value=ai_value()), "Primitive.TestFailure"),
            (success_outcome(value=editor_value()), failed_outcome(), "Primitive.TestFailure"),
        ]
        for editor, ai, code in cases:
            with self.subTest(editor=type(editor).__name__, ai=type(ai).__name__):
                context = MockPrimitiveContext(RuntimePhase.VERIFY)
                native_editor = editor if type(editor) is not dict else success_outcome(value=editor)
                native_ai = ai if type(ai) is not dict else success_outcome(value=ai)
                self._queue(context, native_editor, native_ai)
                assert_error(self, lambda: self._adapter().inspect(mode="class_defaults", exact_class_path=EXACT_CLASS, exact_asset_object_path=None, editor_asset_handle=None, context=context), code)
