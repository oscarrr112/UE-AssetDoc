# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import importlib.util
import json
import math
import unittest

import asset_document.bootstrap as bootstrap_module
from asset_document.canonical.document_codec import DocumentCodec
from asset_document.canonical.semantic_hash import SemanticHashScope, compute_semantic_hash
from asset_document.dto import RUNTIME_CONTRACT_VERSION
from asset_document.regions.primitive_catalog import (
    GENERIC_REQUIRED_PRIMITIVES,
    GENERIC_ROUTE_ID,
)
import asset_document.runtime as runtime_module
from asset_document.profiles.blackboard_data import (
    BLACKBOARD_DATA_CLASS,
)


REQUEST_ID = "00000000-0000-0000-0000-000000000001"
TARGET = "/Game/Generic/DA_Generic"
OBJECT_PATH = f"{TARGET}.DA_Generic"
EXACT_CLASS = "/Script/AssetDocumentTests.TestDataAsset"
# The sidecar form (DocumentCodec.encode_sidecar); the semantic hash is over the compact encoding.
CANONICAL_BYTES = (
    b'{\n  "Action": "CreateOrUpdate",\n  "AssetType": "GenericAsset",\n'
    b'  "Class": "/Script/AssetDocumentTests.TestDataAsset",\n  "Definitions": {},\n'
    b'  "Properties": {\n    "Value": 7\n  },\n  "SchemaVersion": 1,\n'
    b'  "Target": "/Game/Generic/DA_Generic"\n}\n'
)
SEMANTIC_HASH = (
    "adsh:v1:document:"
    "f3f25f6cd500f5ee270a3cebd3650f2ced3429a16aea9d374893d628fa4af0e0"
)
EXPECTED_PRIMITIVES = tuple(
    (
        item.primitive_id,
        item.version,
        item.required_by_route_id,
    )
    for item in GENERIC_REQUIRED_PRIMITIVES
)


class NativeHandle:
    __slots__ = ()


def diagnostic(
    code: str,
    path: str = "/Primitive",
    message: str = "native diagnostic",
) -> dict:
    return {
        "path": path,
        "code": code,
        "message": message,
        "severity": 2,
        "context": (),
    }


def outcome(
    *,
    value=None,
    handle_results: tuple[dict, ...] = (),
    changed_set: tuple[dict, ...] = (),
    diagnostics: tuple[dict, ...] = (),
    status: int = 0,
) -> dict:
    return {
        "status": status,
        "value": value,
        "handle_results": handle_results,
        "changed_set": changed_set,
        "diagnostics": diagnostics,
    }


def handle_group(name: str, *handles: NativeHandle) -> dict:
    return {"name": name, "handles": handles}


def changed(path: str = "/Properties/Value") -> dict:
    return {
        "authority_id": "asset",
        "stable_object_id": OBJECT_PATH,
        "path": path,
        "change_kind": "modified",
    }


def document() -> dict:
    return {
        "SchemaVersion": 1,
        "AssetType": "GenericAsset",
        "Target": TARGET,
        "Class": EXACT_CLASS,
        "Action": "CreateOrUpdate",
        "Definitions": {},
        "Properties": {"Value": 7},
        "Body": {},
    }


def request(canonical_document=None, *, operation: int = 5, **overrides) -> dict:
    value = {
        "abi_version": 1,
        "request_id": REQUEST_ID,
        "operation": operation,
        "target_asset_path": OBJECT_PATH,
        "requested_profile_class_path": EXACT_CLASS,
        "requested_asset_class_path": EXACT_CLASS,
        "canonical_document": (
            document() if canonical_document is None else canonical_document
        ),
        "deadline_unix_milliseconds": 0,
        "cancellation": {"sequence": 0, "requested": False},
        "expected_runtime_revision": 17,
    }
    value.update(overrides)
    return value


def inspect_request(*, live: bool = False) -> dict:
    target = OBJECT_PATH if live else EXACT_CLASS
    return {
        "abi_version": 1,
        "request_id": REQUEST_ID,
        "operation": 0,
        "target_asset_path": target,
        "requested_profile_class_path": "",
        "requested_asset_class_path": EXACT_CLASS,
        "canonical_document": {"mode": "live_asset" if live else "class_defaults"},
        "deadline_unix_milliseconds": 0,
        "cancellation": {"sequence": 0, "requested": False},
        "expected_runtime_revision": 17,
    }


def inspect_editor_value(*, live: bool) -> dict:
    subject = {"kind": "live_asset" if live else "class_defaults", "exact_class_path": EXACT_CLASS}
    if live:
        subject["asset_path"] = OBJECT_PATH
    return {"subject": subject, "admitted": (), "rejected": ()}


def inspect_ai_value(*, live: bool) -> dict:
    subject = {"kind": "live_asset" if live else "class_defaults", "exact_class_path": EXACT_CLASS}
    if live:
        subject["asset_path"] = OBJECT_PATH
    return {"subject": subject, "selectors": ()}


_REQUIRED_VERSIONS = {
    item.primitive_id: item.version for item in GENERIC_REQUIRED_PRIMITIVES
}


class NativeContext:
    def __init__(
        self,
        phase: int,
        *,
        required_ids: tuple[str, ...] | None = None,
        request_id: str = REQUEST_ID,
        runtime_revision: int = 17,
    ) -> None:
        self.request_id = request_id
        self.runtime_revision = runtime_revision
        self.phase = phase
        self.calls: list[tuple[str, int, dict, tuple[dict, ...]]] = []
        self.catalog_calls = 0
        self._queues: dict[str, list[dict]] = {}
        self._required_ids = (
            tuple(item.primitive_id for item in GENERIC_REQUIRED_PRIMITIVES)
            if required_ids is None
            else required_ids
        )

    def inspect_catalog(self) -> dict:
        self.catalog_calls += 1
        return {
            "runtime_revision": self.runtime_revision,
            "frozen": True,
            "descriptors": tuple(
                {
                    "primitive_id": primitive_id,
                    "version": _REQUIRED_VERSIONS.get(primitive_id, 1),
                    "provider_id": "editor",
                    "required_consumer_ids": (GENERIC_ROUTE_ID,),
                }
                for primitive_id in self._required_ids
            ),
            "providers": (
                {"provider_id": "editor", "state": 0, "diagnostic": None},
            ),
        }

    def queue(self, primitive_id: str, *results: dict) -> None:
        self._queues.setdefault(primitive_id, []).extend(results)

    def invoke(
        self,
        primitive_id: str,
        version: int,
        value_arguments: dict,
        handle_arguments: tuple[dict, ...],
    ) -> dict:
        self.calls.append(
            (primitive_id, version, value_arguments, handle_arguments)
        )
        pending = self._queues.get(primitive_id, [])
        if not pending:
            return outcome(
                status=2,
                diagnostics=(diagnostic("Primitive.Unavailable"),),
            )
        return pending.pop(0)

    def handoff(self, handle: object, target_provider_id: str, access: str) -> object:
        self.calls.append(
            ("__handoff__", 1, {"target_provider_id": target_provider_id, "access": access}, ({"handle": handle},))
        )
        return handle


def queue_class(context: NativeContext) -> NativeHandle:
    handle = NativeHandle()
    context.queue(
        "editor.class.resolve_canonical.v1",
        outcome(
            value={"canonical_class": EXACT_CLASS},
            handle_results=(handle_group("class", handle),),
        ),
    )
    return handle


def queue_existing(
    context: NativeContext,
    *,
    exists: bool,
) -> tuple[NativeHandle, NativeHandle, NativeHandle]:
    asset_read = NativeHandle()
    asset_mutable = NativeHandle()
    package = NativeHandle()
    value = _resolve_value(exists)
    groups = _resolve_groups(
        exists,
        asset_read,
        asset_mutable,
        package,
    )
    context.queue(
        "editor.asset.resolve_existing.v1",
        outcome(value=value, handle_results=groups),
    )
    return asset_read, asset_mutable, package


def _resolve_value(exists: bool) -> dict:
    if not exists:
        return {
            "exists": False,
            "canonical_path": None,
            "canonical_class": None,
            "package_name": None,
            "package_dirty": False,
        }
    return {
        "exists": True,
        "canonical_path": OBJECT_PATH,
        "canonical_class": EXACT_CLASS,
        "package_name": TARGET,
        "package_dirty": False,
    }


def _resolve_groups(
    exists: bool,
    asset_read: NativeHandle,
    asset_mutable: NativeHandle,
    package: NativeHandle,
) -> tuple[dict, ...]:
    if not exists:
        return (
            handle_group("asset_read"),
            handle_group("asset_mutable"),
            handle_group("package"),
        )
    return (
        handle_group("asset_read", asset_read),
        handle_group("asset_mutable", asset_mutable),
        handle_group("package", package),
    )


def queue_properties(context: NativeContext) -> NativeHandle:
    property_handle = NativeHandle()
    context.queue(
        "editor.property.enumerate_admitted.v1",
        outcome(
            value={
                "properties": (
                    {
                        "name": "Value",
                        "declaring_class": EXACT_CLASS,
                        "codec_id": "property.test.v1",
                        "readable": True,
                        "writable": True,
                        "default": 0,
                    },
                )
            },
            handle_results=(
                handle_group("properties", property_handle),
            ),
        ),
    )
    return property_handle


def queue_created_asset(
    context: NativeContext,
    primitive_id: str = "editor.asset.create_by_factory.v2",
) -> tuple[NativeHandle, NativeHandle]:
    asset_read = NativeHandle()
    asset_mutable = NativeHandle()
    context.queue(
        primitive_id,
        outcome(
            handle_results=(
                handle_group("asset_read", asset_read),
                handle_group("asset_mutable", asset_mutable),
                handle_group("package", NativeHandle()),
                handle_group("creation", NativeHandle()),
            ),
        ),
    )
    return asset_read, asset_mutable


def queue_batch_properties(context: NativeContext) -> NativeHandle:
    batch = NativeHandle()
    context.queue(
        "editor.property.enumerate_admitted_batch.v1",
        outcome(
            value={
                "properties": (
                    {
                        "name": "Value",
                        "declaring_class": EXACT_CLASS,
                        "codec_id": "property.test.v1",
                        "readable": True,
                        "writable": True,
                        "default": 0,
                    },
                )
            },
            handle_results=(
                handle_group("properties", NativeHandle()),
                handle_group("property_batch", batch),
            ),
        ),
    )
    context.queue(
        "editor.property.write_admitted_batch.v1",
        outcome(changed_set=(changed(),)),
    )
    return batch


def assert_success(test: unittest.TestCase, result: dict) -> None:
    test.assertEqual(
        set(result),
        {
            "abi_version",
            "request_id",
            "terminal_status",
            "final_phase",
            "runtime_revision",
            "diagnostics",
            "canonical_value",
            "canonical_bytes",
            "semantic_hash",
            "codec_version",
            "changed_set",
        },
    )
    test.assertEqual(
        (
            result["abi_version"],
            result["request_id"],
            result["terminal_status"],
            result["final_phase"],
            result["runtime_revision"],
            result["diagnostics"],
        ),
        (1, REQUEST_ID, 0, 6, 17, ()),
    )


def dispatch_read_only_extract() -> dict:
    context = NativeContext(5)
    asset_read, _, _ = queue_existing(context, exists=True)
    property_handle = queue_properties(context)
    context.queue(
        "editor.property.read_admitted.v1",
        outcome(value={"value": 7}),
    )
    result = bootstrap_module.dispatch(
        request({"include_defaults": True}, operation=6),
        context,
    )
    return result, context, asset_read, property_handle


def dispatch_read_only_diff() -> tuple[dict, NativeContext]:
    authored = document()
    authored["Action"] = "Create"
    context = NativeContext(5)
    queue_existing(context, exists=True)
    queue_properties(context)  # authored-property admission
    queue_properties(context)  # extraction
    context.queue(
        "editor.property.read_admitted.v1",
        outcome(value={"value": 7}),
    )
    return bootstrap_module.dispatch(request(authored, operation=7), context), context


def contains_handle(value: object) -> bool:
    if type(value) in (tuple, list):
        return any(contains_handle(item) for item in value)
    if type(value) is dict:
        return any(contains_handle(item) for item in value.values())
    return type(value) is NativeHandle


def prepare_preflight_case(exists: bool):
    context = NativeContext(1)
    class_handle = queue_class(context)
    asset_read, _, _ = queue_existing(context, exists=exists)
    scratch = NativeHandle()
    if exists:
        scratch_id = "editor.scratch.clone_asset.v2"
        input_name, input_handle = "source_asset", asset_read
    else:
        scratch_id = "editor.scratch.create_by_factory.v1"
        input_name, input_handle = "asset_class", class_handle
    context.queue(
        scratch_id,
        outcome(
            value={
                "canonical_class": EXACT_CLASS,
                "lifecycle_id": "factory_generic.v1",
            },
            handle_results=(handle_group("scratch", scratch),),
        ),
    )
    property_handle = queue_properties(context)
    context.queue(
        "editor.property.preflight_admitted.v1",
        outcome(value={"canonical_value": 7, "feasible": True}),
    )
    return (
        context,
        scratch_id,
        input_name,
        input_handle,
        scratch,
        property_handle,
    )


def assert_preflight_case(
    test: unittest.TestCase,
    exists: bool,
) -> None:
    (
        context,
        scratch_id,
        input_name,
        input_handle,
        scratch,
        property_handle,
    ) = prepare_preflight_case(exists)
    result = bootstrap_module.dispatch(request(), context)
    assert_success(test, result)
    test.assertEqual(
        [call[0] for call in context.calls],
        [
            "editor.class.resolve_canonical.v1",
            "editor.asset.resolve_existing.v1",
            scratch_id,
            "editor.property.enumerate_admitted.v1",
            "editor.property.preflight_admitted.v1",
        ],
    )
    test.assertEqual(context.calls[1][2]["path"], OBJECT_PATH)
    scratch_call = context.calls[2]
    test.assertEqual(scratch_call[2], {})
    test.assertEqual(scratch_call[3][0]["name"], input_name)
    test.assertIs(scratch_call[3][0]["handle"], input_handle)
    property_call = context.calls[-1]
    test.assertEqual(
        tuple(item["name"] for item in property_call[3]),
        ("scratch", "property"),
    )
    test.assertIs(property_call[3][0]["handle"], scratch)
    test.assertIs(property_call[3][1]["handle"], property_handle)
    test.assertFalse(contains_handle(result))


def blackboard_document() -> dict:
    return {
        "SchemaVersion": 1,
        "Target": "/Game/AI/BB_Target",
        "Class": BLACKBOARD_DATA_CLASS,
        "Action": "CreateOrUpdate",
        "Definitions": {},
        "Properties": {},
        "Body": {"Parent": None, "Keys": ()},
    }


def blackboard_request(**overrides) -> dict:
    values = {
        "canonical_document": blackboard_document(),
        "requested_profile_class_path": BLACKBOARD_DATA_CLASS,
        "requested_asset_class_path": BLACKBOARD_DATA_CLASS,
        "target_asset_path": "/Game/AI/BB_Target.BB_Target",
    }
    values.update(overrides)
    return request(**values)


def queue_blackboard_preflight(context: NativeContext, *, exists: bool = True) -> None:
    queue_existing(context, exists=exists)
    context.queue(
        "ai.blackboard.parent.readback.v2",
        outcome(value={"parent_path_or_null": None}),
    )
    context.queue(
        "ai.blackboard.local_keys.readback.v2",
        outcome(value={"ordered_local_key_names": ()}),
    )


class BlackboardDataRuntimeTests(unittest.TestCase):
    def test_extract_and_diff_observe_blackboard_once_without_relay_or_mutation(self) -> None:
        extract = NativeContext(5)
        queue_existing(extract, exists=True)
        extract.queue("ai.blackboard.parent.readback.v2", outcome(value={"parent_path_or_null": None}))
        extract.queue("ai.blackboard.local_keys.readback.v2", outcome(value={"ordered_local_key_names": ()}))

        extracted = bootstrap_module.dispatch(blackboard_request(operation=6), extract)

        assert_success(self, extracted)
        self.assertEqual(
            extracted["canonical_value"],
            {
                "SchemaVersion": 1,
                "Target": "/Game/AI/BB_Target",
                "Class": BLACKBOARD_DATA_CLASS,
                "Action": "CreateOrUpdate",
                "Definitions": {},
                "Properties": {},
                "Body": {"Parent": None, "Keys": ()},
            },
        )
        self.assertEqual(
            [call[0] for call in extract.calls],
            [
                "editor.asset.resolve_existing.v1",
                "ai.blackboard.parent.readback.v2",
                "ai.blackboard.local_keys.readback.v2",
            ],
        )
        self.assertIsNone(runtime_module._blackboard_snapshot_slot)

        authored = blackboard_document() | {"Action": "Create"}
        diff = NativeContext(5)
        queue_existing(diff, exists=True)
        diff.queue("ai.blackboard.parent.readback.v2", outcome(value={"parent_path_or_null": None}))
        diff.queue("ai.blackboard.local_keys.readback.v2", outcome(value={"ordered_local_key_names": ()}))

        compared = bootstrap_module.dispatch(
            blackboard_request(canonical_document=authored, operation=7), diff
        )

        assert_success(self, compared)
        self.assertEqual(compared["canonical_value"], {"changed": (), "unchanged": (), "skipped": (), "failed": ()})
        self.assertEqual(
            [call[0] for call in diff.calls],
            [
                "editor.asset.resolve_existing.v1",
                "ai.blackboard.parent.readback.v2",
                "ai.blackboard.local_keys.readback.v2",
            ],
        )
        self.assertIsNone(runtime_module._blackboard_snapshot_slot)

    def test_diff_reports_authored_document_errors_before_reading_the_target(self) -> None:
        authored = blackboard_document() | {"Action": "Create"}
        authored["Body"] = dict(authored["Body"]) | {"UnknownRegion": {}}
        diff = NativeContext(5)

        compared = bootstrap_module.dispatch(
            blackboard_request(canonical_document=authored, operation=7), diff
        )

        self.assertEqual(
            [(item["path"], item["code"]) for item in compared["diagnostics"]],
            [("/Body/UnknownRegion", "UnknownBodyKey")],
        )
        self.assertEqual(diff.calls, [])

    def test_create_preflight_uses_empty_snapshot_without_target_readback_then_factory_apply(self) -> None:
        authored = blackboard_document() | {"Action": "Create"}
        preflight = NativeContext(1)
        queue_existing(preflight, exists=False)

        projected = bootstrap_module.dispatch(
            blackboard_request(canonical_document=authored), preflight
        )

        assert_success(self, projected)
        self.assertEqual(
            tuple(call[0] for call in preflight.calls),
            ("editor.asset.resolve_existing.v1",),
        )
        self.assertEqual(projected["canonical_value"]["Steps"], ())

        apply = NativeContext(2)
        queue_existing(apply, exists=False)
        queue_class(apply)
        queue_created_asset(apply, "editor.asset.create_by_factory.v1")
        queue_existing(apply, exists=True)
        applied = bootstrap_module.dispatch(
            blackboard_request(canonical_document=authored), apply
        )

        assert_success(self, applied)
        self.assertEqual(
            tuple(call[0] for call in apply.calls),
            (
                "editor.asset.resolve_existing.v1",
                "editor.class.resolve_canonical.v1",
                "editor.asset.create_by_factory.v1",
                "editor.asset.resolve_existing.v1",
                "__handoff__",
            ),
        )

    def test_preflight_rejects_action_that_disagrees_with_target_existence(self) -> None:
        for action, exists in (("Update", False), ("Create", True)):
            with self.subTest(action=action):
                preflight = NativeContext(1)
                queue_existing(preflight, exists=exists)

                result = bootstrap_module.dispatch(
                    blackboard_request(canonical_document=blackboard_document() | {"Action": action}),
                    preflight,
                )

                self.assertEqual(
                    [(item["path"], item["code"]) for item in result["diagnostics"]],
                    [("/Action", "BlackboardData.TargetActionMismatch")],
                )
                self.assertIsNone(runtime_module._blackboard_snapshot_slot)

    def test_preflight_returns_closed_projection_and_relay_keeps_the_one_frozen_plan(self) -> None:
        validate = NativeContext(0)
        validated = bootstrap_module.dispatch(blackboard_request(), validate)
        assert_success(self, validated)

        preflight = NativeContext(1)
        queue_blackboard_preflight(preflight)
        projected = bootstrap_module.dispatch(blackboard_request(), preflight)
        self.addCleanup(setattr, runtime_module, "_blackboard_snapshot_slot", None)

        assert_success(self, projected)
        self.assertEqual(
            projected["canonical_value"],
            {
                "Kind": "BlackboardFootprintProjectionV1",
                "RequestId": REQUEST_ID,
                "RuntimeRevision": 17,
                "ExactAssetClassPath": BLACKBOARD_DATA_CLASS,
                "TargetAssetPath": "/Game/AI/BB_Target.BB_Target",
                "Operation": "Apply",
                "SemanticHash": validated["semantic_hash"],
                "Steps": (),
            },
        )
        slot = runtime_module._blackboard_snapshot_slot
        self.assertIsNotNone(slot)
        self.assertEqual(slot.projection, projected["canonical_value"])
        self.assertEqual(slot.transitions, ())

        self.assertIs(type(slot.projection["Steps"]), tuple)

        apply = NativeContext(2)
        queue_existing(apply, exists=True)
        applied = bootstrap_module.dispatch(blackboard_request(), apply)
        assert_success(self, applied)
        self.assertFalse(any(call[0].startswith("ai.blackboard.") for call in apply.calls))
        self.assertEqual(apply.calls[-1][0], "__handoff__")

    def test_verify_blackboard_observes_the_exact_snapshot_and_normalizes_action(self) -> None:
        authored = blackboard_document() | {"Action": "Create"}
        verify = NativeContext(5)
        queue_existing(verify, exists=True)
        verify.queue(
            "ai.blackboard.parent.readback.v2",
            outcome(value={"parent_path_or_null": None}),
        )
        verify.queue(
            "ai.blackboard.local_keys.readback.v2",
            outcome(value={"ordered_local_key_names": ()}),
        )

        result = bootstrap_module.dispatch(
            blackboard_request(canonical_document=authored), verify
        )

        assert_success(self, result)
        self.assertEqual(
            tuple(call[0] for call in verify.calls),
            (
                "editor.asset.resolve_existing.v1",
                "ai.blackboard.parent.readback.v2",
                "ai.blackboard.local_keys.readback.v2",
            ),
        )

    def test_verify_blackboard_ignores_engine_derived_self_actor_in_authored_projection(self) -> None:
        authored = blackboard_document() | {
            "Action": "Create",
            "Body": {
                "Parent": None,
                "Keys": (
                    {
                        "Category": "Lifecycle",
                        "Description": "parent baseline",
                        "KeyTypeClass": {"Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Int"},
                        "KeyTypeProperties": {"DefaultValue": 0},
                        "Name": "ParentKey",
                        "bInstanceSynced": False,
                    },
                ),
            },
        }
        verify = NativeContext(5)
        queue_existing(verify, exists=True)
        verify.queue("ai.blackboard.parent.readback.v2", outcome(value={"parent_path_or_null": None}))
        verify.queue(
            "ai.blackboard.local_keys.readback.v2",
            outcome(value={"ordered_local_key_names": ("ParentKey", "SelfActor")}),
        )
        verify.queue(
            "ai.blackboard.entry.readback.v2",
            outcome(value={"entry": authored["Body"]["Keys"][0]}),
        )
        verify.queue(
            "ai.blackboard.entry.readback.v2",
            outcome(
                value={
                    "entry": {
                        "Category": "",
                        "Description": "",
                        "KeyTypeClass": {"Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Object"},
                        "KeyTypeProperties": {
                            "BaseClass": {"Kind": "ClassRef", "Path": "/Script/Engine.Actor"},
                            "DefaultValue": None,
                        },
                        "Name": "SelfActor",
                        "bInstanceSynced": False,
                    }
                }
            ),
        )

        result = bootstrap_module.dispatch(
            blackboard_request(canonical_document=authored), verify
        )

        assert_success(self, result)
        actual_document = DocumentCodec().decode(result["canonical_bytes"])
        # The sidecar written back after a Create says CreateOrUpdate (a re-edit re-applies).
        self.assertEqual(actual_document["Action"], "CreateOrUpdate")
        self.assertEqual(tuple(actual_document["Body"]["Keys"]), authored["Body"]["Keys"])
        self.assertEqual(result["canonical_bytes"], DocumentCodec().encode_sidecar(actual_document))
        self.assertEqual(
            result["semantic_hash"],
            compute_semantic_hash(
                actual_document,
                scope=SemanticHashScope.DOCUMENT,
                codec=DocumentCodec(),
            ),
        )

    def test_preflight_snapshot_relay_is_request_scoped_one_shot(self) -> None:
        preflight = NativeContext(1)
        queue_blackboard_preflight(preflight)
        preflight_result = bootstrap_module.dispatch(blackboard_request(), preflight)

        assert_success(self, preflight_result)
        self.assertEqual(
            tuple(call[0] for call in preflight.calls),
            (
                "editor.asset.resolve_existing.v1",
                "ai.blackboard.parent.readback.v2",
                "ai.blackboard.local_keys.readback.v2",
            ),
        )
        slot = runtime_module._blackboard_snapshot_slot
        self.assertIsNotNone(slot)
        self.assertEqual(
            slot.identity,
            (
                REQUEST_ID,
                17,
                BLACKBOARD_DATA_CLASS,
                "/Game/AI/BB_Target.BB_Target",
                "Apply",
            ),
        )
        self.assertFalse(contains_handle(slot.snapshot.entries))

        duplicate = NativeContext(1)
        queue_blackboard_preflight(duplicate)
        duplicate_result = bootstrap_module.dispatch(blackboard_request(), duplicate)
        self.assertEqual(
            duplicate_result["diagnostics"][0]["code"],
            "BlackboardData.RelayDuplicatePreflight",
        )
        self.assertEqual(duplicate.calls, [])

        apply = NativeContext(2)
        queue_existing(apply, exists=True)
        applied = bootstrap_module.dispatch(blackboard_request(), apply)

        assert_success(self, applied)
        self.assertEqual(
            tuple(call[0] for call in apply.calls),
            ("editor.asset.resolve_existing.v1", "__handoff__"),
        )
        self.assertIsNone(runtime_module._blackboard_snapshot_slot)

        consumed = NativeContext(2)
        queue_existing(consumed, exists=True)
        missing = bootstrap_module.dispatch(blackboard_request(), consumed)
        self.assertEqual(missing["diagnostics"][0]["code"], "BlackboardData.RelayMissing")
        self.assertEqual(consumed.calls, [])

        retry_preflight = NativeContext(1)
        queue_blackboard_preflight(retry_preflight)
        assert_success(self, bootstrap_module.dispatch(blackboard_request(), retry_preflight))
        failed_apply = NativeContext(2)
        failed = bootstrap_module.dispatch(blackboard_request(), failed_apply)
        self.assertEqual(failed["diagnostics"][0]["code"], "Primitive.Unavailable")
        self.assertEqual(
            tuple(call[0] for call in failed_apply.calls),
            ("editor.asset.resolve_existing.v1",),
        )
        self.assertIsNone(runtime_module._blackboard_snapshot_slot)

    def test_relay_clears_stale_and_wrong_identity_before_provider_calls(self) -> None:
        mismatch_cases = (
            ("request", {"request_id": "00000000-0000-0000-0000-000000000002"}),
            ("revision", {"expected_runtime_revision": 18}),
            ("class", {"requested_asset_class_path": "/Script/Engine.DataAsset"}),
            ("target", {"target_asset_path": "/Game/AI/Other.Other"}),
            ("operation", {"operation": 4}),
        )
        for label, overrides in mismatch_cases:
            with self.subTest(identity=label):
                preflight = NativeContext(1)
                queue_blackboard_preflight(preflight)
                assert_success(self, bootstrap_module.dispatch(blackboard_request(), preflight))

                attempted = blackboard_request(**overrides)
                failed = NativeContext(
                    2,
                    request_id=attempted["request_id"],
                    runtime_revision=attempted["expected_runtime_revision"],
                )
                rejected = bootstrap_module.dispatch(attempted, failed)

                self.assertEqual(rejected["diagnostics"][0]["code"], "BlackboardData.RelayIdentityMismatch")
                self.assertEqual(failed.calls, [])
                self.assertIsNone(runtime_module._blackboard_snapshot_slot)

        preflight = NativeContext(1)
        queue_blackboard_preflight(preflight)
        assert_success(self, bootstrap_module.dispatch(blackboard_request(), preflight))
        next_request = request()
        next_context = NativeContext(0)
        next_result = bootstrap_module.dispatch(next_request, next_context)
        assert_success(self, next_result)
        self.assertIsNone(runtime_module._blackboard_snapshot_slot)

    def test_non_blackboard_exact_class_preserves_generic_behavior(self) -> None:
        context = NativeContext(0)
        result = bootstrap_module.dispatch(request(), context)

        assert_success(self, result)
        self.assertEqual(result["canonical_value"]["Class"], EXACT_CLASS)
        self.assertEqual(context.calls, [])


class GenericRuntimeCutoverTests(unittest.TestCase):
    def test_bootstrap_phase_result_projection_and_catalog_closure(self) -> None:
        spec = importlib.util.find_spec("asset_document.bootstrap")
        self.assertIsNotNone(spec)
        binding = bootstrap_module.PROFILE_REGISTRY.resolve_runtime_exact("/Script/Engine.AnimMontage")
        self.assertIsNotNone(binding)
        # AnimMontage, AnimSequence, UBlueprint (profile.ublueprint.v2), WidgetBlueprint
        # (profile.widget_blueprint.v1), AnimBlueprint (profile.anim_blueprint.v1) and the six data table
        # family Profiles (DataTable, CompositeDataTable, MirrorDataTable, CurveTable, CompositeCurveTable,
        # StringTable, UserDefinedStruct, UserDefinedEnum) and the seven animation asset Profiles (Skeleton,
        # BlendSpace, BlendSpace1D, AimOffsetBlendSpace, AimOffsetBlendSpace1D, PoseAsset, AnimComposite) and
        # MaterialInstanceConstant, StateTree (state_tree_profile) and the four Material graph Profiles
        # (Material, MaterialFunction, MaterialFunctionMaterialLayer, ...LayerBlend) and the four read-only
        # Niagara Profiles (NiagaraSystem, NiagaraEmitter, NiagaraScript, NiagaraParameterCollection) and the two
        # MetaSound Profiles (MetaSoundSource, MetaSoundPatch; audio_profile), SoundCue (audio_profile), the IK Rig /
        # IK Retargeter / Control Rig Profiles, PhysicsAsset (physics_profile) and the read-only LevelSequence
        # (seq_profile) and EnvQuery (env_query_profile) are registered runtimes.
        self.assertEqual(len(bootstrap_module.dispatch.describe_runtime_registrations()), 41)
        context = NativeContext(6)
        result = bootstrap_module.dispatch(request(), context)
        assert_success(self, result)
        self.assertEqual(context.catalog_calls, 1)
        self.assertEqual(context.calls, [])

        missing = NativeContext(
            6,
            required_ids=tuple(
                item.primitive_id for item in GENERIC_REQUIRED_PRIMITIVES[:-1]
            ),
        )
        failure = bootstrap_module.dispatch(request(), missing)
        self.assertEqual(
            (
                failure["terminal_status"],
                failure["final_phase"],
                failure["diagnostics"][0]["code"],
            ),
            (1, 7, "GenericRuntime.RequiredPrimitiveUnavailable"),
        )
        invalid = bootstrap_module.dispatch(request(), NativeContext(99))
        self.assertEqual(
            (
                invalid["terminal_status"],
                invalid["final_phase"],
                invalid["diagnostics"][0]["code"],
            ),
            (1, 7, "GenericRuntime.InvalidNativePhase"),
        )

    def test_validate_emits_production_canonical_evidence_before_primitives(self) -> None:
        context = NativeContext(0)
        result = bootstrap_module.dispatch(request(), context)
        assert_success(self, result)
        expected_value = document()
        del expected_value["Body"]
        self.assertEqual(result["canonical_value"], expected_value)
        self.assertNotIn("Body", result["canonical_value"])
        self.assertEqual(result["canonical_bytes"], CANONICAL_BYTES)
        self.assertEqual(result["semantic_hash"], SEMANTIC_HASH)
        self.assertEqual(result["codec_version"], 1)
        self.assertEqual(context.calls, [])

        for invalid, code in (
            ({**document(), "Body": ()}, "InvalidBodyType"),
            (
                {**document(), "Properties": {"Value": math.nan}},
                "Codec.NonFiniteNumber",
            ),
        ):
            rejected = NativeContext(0)
            with self.subTest(code=code):
                failure = bootstrap_module.dispatch(request(invalid), rejected)
                self.assertEqual(failure["diagnostics"][0]["code"], code)
                self.assertEqual(rejected.calls, [])

    def test_a_create_is_written_back_as_create_or_update(self) -> None:
        # The sidecar the apply writes back is what a re-edit applies; a Create would refuse TargetExists.
        created = {**document(), "Action": "Create"}
        result = bootstrap_module.dispatch(request(created), NativeContext(0))
        assert_success(self, result)
        self.assertEqual(result["canonical_value"]["Action"], "Create")
        written = DocumentCodec().decode(result["canonical_bytes"])
        self.assertEqual(written["Action"], "CreateOrUpdate")
        self.assertEqual(result["semantic_hash"], compute_semantic_hash(written, scope=SemanticHashScope.DOCUMENT,
                                                                        codec=DocumentCodec()))
        for action in ("Update", "CreateOrUpdate"):
            kept = bootstrap_module.dispatch(request({**document(), "Action": action}), NativeContext(0))
            self.assertEqual(DocumentCodec().decode(kept["canonical_bytes"])["Action"], action)

    def test_schema_validate_projects_literal_generic_profile_without_primitives(self) -> None:
        context = NativeContext(0)
        result = bootstrap_module.dispatch(
            request({"ignored": "Schema must not validate a document"}, operation=1),
            context,
        )

        assert_success(self, result)
        self.assertEqual(
            result["canonical_value"],
            {
                "Class": EXACT_CLASS,
                "DocumentShape": {
                    "Definitions": "map<string, Fragment>",
                    "Properties": "reflected CDO-diff properties",
                    "Body": {},
                },
                "BodySections": (),
                "FragmentKinds": (
                    "AssetRef",
                    "ClassRef",
                    "StructValue",
                    "EmbeddedObject",
                    "DefinitionRef",
                ),
                "InternalAdapters": (),
                "RegionPolicies": (),
                "BlockedFragments": (
                    {
                        "kind": "EmbeddedObject",
                        "code": "AD.BLOCK.EDITOR.EMBEDDED_OBJECT_AUTHORITY",
                    },
                ),
            },
        )
        self.assertEqual(context.calls, [])

    def test_template_validate_projects_literal_frozen_template_without_primitives(self) -> None:
        context = NativeContext(0)
        result = bootstrap_module.dispatch(
            request({"ignored": "Template must not validate a document"}, operation=2),
            context,
        )

        assert_success(self, result)
        self.assertEqual(
            result["canonical_value"],
            {
                "SchemaVersion": 1,
                "Target": TARGET,
                "Class": EXACT_CLASS,
                "Action": "CreateOrUpdate",
                "Definitions": {},
                "Properties": {},
            },
        )
        self.assertNotIn("AssetType", result["canonical_value"])
        self.assertNotIn("Body", result["canonical_value"])
        self.assertEqual(context.calls, [])

    def test_native_failed_empty_object_preserves_provider_diagnostic(self) -> None:
        context = NativeContext(1)
        context.queue(
            "editor.class.resolve_canonical.v1",
            outcome(
                status=1,
                value={},
                diagnostics=(diagnostic("Class.ResolveFailed"),),
            ),
        )
        result = bootstrap_module.dispatch(request(), context)
        self.assertEqual(result["diagnostics"][0]["code"], "Class.ResolveFailed")

    def test_preflight_existing_and_create_use_only_scratch_for_properties(self) -> None:
        for exists in (True, False):
            with self.subTest(exists=exists):
                assert_preflight_case(self, exists)

    def test_apply_writes_one_batch_and_repair_writes_nothing(self) -> None:
        context = NativeContext(2)
        queue_class(context)
        asset_read, asset_mutable, _ = queue_existing(context, exists=True)
        queue_properties(context)
        batch = queue_batch_properties(context)
        result = bootstrap_module.dispatch(request(), context)
        assert_success(self, result)
        self.assertEqual(result["changed_set"], (changed(),))
        self.assertEqual(
            sum(
                call[0] == "editor.asset.resolve_existing.v1"
                for call in context.calls
            ),
            1,
        )
        self.assertFalse(
            any(
                call[0].startswith("editor.asset.create_by_factory.")
                for call in context.calls
            )
        )
        batch_reads = [
            call
            for call in context.calls
            if call[0] == "editor.property.enumerate_admitted_batch.v1"
        ]
        self.assertEqual(len(batch_reads), 1)
        self.assertIs(batch_reads[0][3][0]["handle"], asset_read)
        writes = [
            call
            for call in context.calls
            if call[0] == "editor.property.write_admitted_batch.v1"
        ]
        self.assertEqual(len(writes), 1)
        self.assertEqual(
            tuple(item["name"] for item in writes[0][3]),
            ("asset", "property_batch"),
        )
        self.assertIs(writes[0][3][0]["handle"], asset_mutable)
        self.assertIs(writes[0][3][1]["handle"], batch)

        repair = NativeContext(3)
        repair_result = bootstrap_module.dispatch(request(), repair)
        assert_success(self, repair_result)
        self.assertEqual(repair_result["changed_set"], ())
        self.assertEqual(repair.calls, [])

    def test_apply_create_derives_factory_arguments_from_object_identity(self) -> None:
        context = NativeContext(2)
        queue_class(context)
        queue_existing(context, exists=False)
        queue_created_asset(context)
        queue_properties(context)
        queue_batch_properties(context)

        result = bootstrap_module.dispatch(request(), context)

        assert_success(self, result)
        factory_call = next(
            call
            for call in context.calls
            if call[0] == "editor.asset.create_by_factory.v2"
        )
        self.assertEqual(factory_call[1], 2)
        self.assertEqual(
            factory_call[2],
            {
                "package_path": "/Game/Generic",
                "asset_name": "DA_Generic",
            },
        )

    def test_verify_reacquires_reads_and_compares_canonical_evidence(self) -> None:
        context = NativeContext(5)
        asset_read, _, _ = queue_existing(context, exists=True)
        property_handle = queue_properties(context)
        context.queue(
            "editor.property.read_admitted.v1",
            outcome(value={"value": 7}),
        )
        result = bootstrap_module.dispatch(request(), context)
        assert_success(self, result)
        self.assertEqual(
            [call[0] for call in context.calls],
            [
                "editor.asset.resolve_existing.v1",
                "editor.property.enumerate_admitted.v1",
                "editor.property.read_admitted.v1",
            ],
        )
        read = context.calls[-1]
        self.assertEqual(
            tuple(item["name"] for item in read[3]),
            ("asset", "property"),
        )
        self.assertIs(read[3][0]["handle"], asset_read)
        self.assertIs(read[3][1]["handle"], property_handle)
        self.assertEqual(result["canonical_bytes"], CANONICAL_BYTES)
        self.assertEqual(result["semantic_hash"], SEMANTIC_HASH)

        mismatch = NativeContext(5)
        queue_existing(mismatch, exists=True)
        queue_properties(mismatch)
        mismatch.queue(
            "editor.property.read_admitted.v1",
            outcome(value={"value": 8}),
        )
        failure = bootstrap_module.dispatch(request(), mismatch)
        self.assertEqual(
            failure["diagnostics"][0]["code"],
            "GenericRuntime.VerificationMismatch",
        )
        # The AI can see which authored property the asset holds differently.
        self.assertIn("Value (asset holds 8, document 7)", failure["diagnostics"][0]["message"])

    def test_verify_compares_only_the_authored_property_surface(self) -> None:
        context = NativeContext(5)
        queue_existing(context, exists=True)
        defaulted_handle = NativeHandle()
        authored_handle = NativeHandle()
        context.queue(
            "editor.property.enumerate_admitted.v1",
            outcome(
                value={
                    "properties": (
                        {
                            "name": "Defaulted",
                            "declaring_class": EXACT_CLASS,
                            "codec_id": "property.test.v1",
                            "readable": True,
                            "writable": True,
                        },
                        {
                            "name": "Value",
                            "declaring_class": EXACT_CLASS,
                            "codec_id": "property.test.v1",
                            "readable": True,
                            "writable": True,
                        },
                    )
                },
                handle_results=(
                    handle_group(
                        "properties",
                        defaulted_handle,
                        authored_handle,
                    ),
                ),
            ),
        )
        context.queue(
            "editor.property.read_admitted.v1",
            outcome(value={"value": 0}),
            outcome(value={"value": 7}),
        )

        result = bootstrap_module.dispatch(request(), context)

        assert_success(self, result)
        self.assertEqual(result["canonical_bytes"], CANONICAL_BYTES)
        self.assertEqual(result["semantic_hash"], SEMANTIC_HASH)

    def test_extract_and_diff_use_python_read_only_semantics(self) -> None:
        extracted, extract, asset_read, property_handle = dispatch_read_only_extract()

        assert_success(self, extracted)
        self.assertEqual(
            extracted["canonical_value"],
            {
                "SchemaVersion": 1,
                "AssetType": "GenericAsset",
                "Target": TARGET,
                "Class": EXACT_CLASS,
                "Action": "CreateOrUpdate",
                "Properties": {"Value": 7},
            },
        )
        self.assertEqual(
            [call[0] for call in extract.calls],
            [
                "editor.asset.resolve_existing.v1",
                "editor.property.enumerate_admitted.v1",
                "editor.property.read_admitted.v1",
            ],
        )
        self.assertIs(extract.calls[-1][3][0]["handle"], asset_read)
        self.assertIs(extract.calls[-1][3][1]["handle"], property_handle)

        compared, diff = dispatch_read_only_diff()

        assert_success(self, compared)
        self.assertEqual(
            compared["canonical_value"],
            {
                "changed": (),
                "unchanged": (),
                "skipped": (),
                "failed": (),
            },
        )
        self.assertFalse(
            any(
                call[0]
                in {
                    "editor.asset.create_by_factory.v1",
                    "editor.property.write_admitted_batch.v1",
                }
                for call in diff.calls
            )
        )

    def test_read_only_diff_rejects_properties_apply_would_reject(self) -> None:
        cases = (
            ({"Missing": 1}, None, "/Properties/Missing", "Generic.UnknownProperty"),
            ({"Value": 7}, False, "/Properties/Value", "Generic.NonWritableProperty"),
        )
        for properties, writable, path, code in cases:
            with self.subTest(code=code):
                authored = document()
                authored["Properties"] = properties
                context = NativeContext(5)
                queue_existing(context, exists=True)
                if writable is None:
                    queue_properties(context)
                else:
                    context.queue(
                        "editor.property.enumerate_admitted.v1",
                        outcome(
                            value={"properties": ({
                                "name": "Value", "declaring_class": EXACT_CLASS,
                                "codec_id": "property.test.v1", "readable": True, "writable": writable,
                            },)},
                            handle_results=(handle_group("properties", NativeHandle()),),
                        ),
                    )

                result = bootstrap_module.dispatch(request(authored, operation=7), context)

                self.assertEqual(
                    [(item["path"], item["code"]) for item in result["diagnostics"]],
                    [(path, code)],
                )
                self.assertNotIn(
                    "editor.property.read_admitted.v1",
                    [call[0] for call in context.calls],
                )

    def test_read_only_diff_of_a_missing_target_reports_would_create(self) -> None:
        authored = document()
        context = NativeContext(5)
        queue_existing(context, exists=False)
        queue_properties(context)

        result = bootstrap_module.dispatch(request(authored, operation=7), context)

        assert_success(self, result)
        value = result["canonical_value"]
        self.assertIs(value["would_create"], True)
        self.assertEqual(len(value["changed"]), 1)
        entry = value["changed"][0]
        self.assertEqual((entry["path"], entry["kind"], entry["before"]), ("/", "Add", None))
        self.assertEqual(entry["after"]["Properties"], {"Value": 7})

    def test_read_only_diff_rejects_update_of_a_missing_target(self) -> None:
        authored = document()
        authored["Action"] = "Update"
        context = NativeContext(5)
        queue_existing(context, exists=False)
        queue_properties(context)

        result = bootstrap_module.dispatch(request(authored, operation=7), context)

        self.assertEqual(
            [(item["path"], item["code"]) for item in result["diagnostics"]],
            [("/Action", "GenericRuntime.UpdateTargetMissing")],
        )

    def test_public_diff_entry_shape_for_profile_receipts(self) -> None:
        self.assertEqual(
            runtime_module._public_diff_entry({"path": "/Body/Tree/X", "status": "changed", "current": 2, "desired": 3}),
            {"path": "/Body/Tree/X", "status": "changed", "current": 2, "desired": 3,
             "kind": "Replace", "before": 2, "after": 3},
        )
        self.assertEqual(
            runtime_module._public_diff_entry({"path": "/Properties/A", "status": "added", "before": None, "after": 1}),
            {"path": "/Properties/A", "status": "added", "before": None, "after": 1, "kind": "Add"},
        )

    def test_public_diff_kinds_are_relative_to_the_asset(self) -> None:
        from asset_document.diff.canonical_diff import canonical_diff
        from asset_document.diff.model import DiffKind, Difference

        def projected(differences):
            return [(item["path"], item["kind"], item["before"], item["after"])
                    for item in map(runtime_module._difference_projection, differences)]

        # Canonical diffs are (document, asset): list and map items only the document has
        # are "Add" (Apply adds them), items only the asset has are "Remove".
        document = {"Body": {"List": [1, 2, 3], "Map": {"Kept": 1, "New": 2}, "Value": 1}}
        asset = {"Body": {"List": [1], "Map": {"Kept": 1, "Old": 3}, "Value": 2}}
        self.assertEqual(projected(canonical_diff(document, asset)), [
            ("/Body/List/1", "Add", None, 2),
            ("/Body/List/2", "Add", None, 3),
            ("/Body/Map/New", "Add", None, 2),
            ("/Body/Map/Old", "Remove", 3, None),
            ("/Body/Value", "Replace", 2, 1),
        ])
        self.assertEqual(projected(canonical_diff(asset, document)), [
            ("/Body/List/1", "Remove", 2, None),
            ("/Body/List/2", "Remove", 3, None),
            ("/Body/Map/New", "Remove", 2, None),
            ("/Body/Map/Old", "Add", None, 3),
            ("/Body/Value", "Replace", 1, 2),
        ])
        # Profiles that name the Apply operation (a new user parameter is ADD with the
        # asset side absent; a removed one is REMOVE with a null document side) project
        # to the same asset-relative kinds.
        parameter = {"Name": "User.FlameIntensity", "Type": "float", "Value": 1.0}
        self.assertEqual(projected([
            Difference("/Body/UserParameters/2", DiffKind.ADD, True, parameter, False, None),
            Difference("/Body/Expressions/Old", DiffKind.REMOVE, True, None, True, {"Class": "X"}),
            Difference("/Body/Inputs/Rate", DiffKind.ADD, True, 2.0, True, 1.0),
            Difference("/Body/Tooltip", DiffKind.REPLACE, True, "Hi", False, None),
            Difference("/Body/Nullable", DiffKind.REPLACE, True, None, True, 1),
        ]), [
            ("/Body/UserParameters/2", "Add", None, parameter),
            ("/Body/Expressions/Old", "Remove", {"Class": "X"}, None),
            ("/Body/Inputs/Rate", "Replace", 1.0, 2.0),
            ("/Body/Tooltip", "Add", None, "Hi"),
            ("/Body/Nullable", "Replace", 1, None),
        ])

    def test_read_only_diff_ignores_unauthored_extracted_defaults(self) -> None:
        authored = document()
        authored["Properties"] = {"Value": 7}
        context = NativeContext(5)
        queue_existing(context, exists=True)
        for _ in range(2):  # authored-property admission, then extraction
            context.queue(
                "editor.property.enumerate_admitted.v1",
                outcome(
                    value={
                        "properties": (
                            {
                                "name": "DefaultInt",
                                "declaring_class": EXACT_CLASS,
                                "codec_id": "property.test.v1",
                                "readable": True,
                                "writable": True,
                            },
                            {
                                "name": "Value",
                                "declaring_class": EXACT_CLASS,
                                "codec_id": "property.test.v1",
                                "readable": True,
                                "writable": True,
                            },
                        )
                    },
                    handle_results=(
                        handle_group("properties", NativeHandle(), NativeHandle()),
                    ),
                ),
            )
        context.queue(
            "editor.property.read_admitted.v1",
            outcome(value={"value": 0}),
            outcome(value={"value": 0}),
        )

        result = bootstrap_module.dispatch(request(authored, operation=7), context)

        assert_success(self, result)
        self.assertEqual(
            result["canonical_value"]["changed"],
            (
                {
                    "path": "/Properties/Value",
                    "kind": "Replace",
                    # before = the asset value, after = the document value.
                    "before": 0,
                    "after": 7,
                },
            ),
        )

    def test_runtime_module_retains_no_native_context_or_handle(self) -> None:
        self.assertFalse(
            any(
                type(value) in (NativeContext, NativeHandle)
                for value in vars(runtime_module).values()
            )
        )

    def test_inspect_dispatches_only_the_two_evidence_primitives(self) -> None:
        for live in (False, True):
            with self.subTest(live=live):
                context = NativeContext(5)
                asset_read = self._queue_inspect_evidence(context, live=live)

                result = bootstrap_module.dispatch(inspect_request(live=live), context)

                assert_success(self, result)
                self._assert_inspect_evidence_calls(context, live=live, asset_read=asset_read)

    def _queue_inspect_evidence(self, context: NativeContext, *, live: bool) -> NativeHandle | None:
        asset_read = None
        if live:
            asset_read, _, _ = queue_existing(context, exists=True)
        context.queue(
            "editor.property.inspect_evidence.v1",
            outcome(value=inspect_editor_value(live=live)),
        )
        context.queue(
            "ai.behavior_tree.selector_inspect_evidence.v1",
            outcome(value=inspect_ai_value(live=live)),
        )
        return asset_read

    def _assert_inspect_evidence_calls(self, context: NativeContext, *, live: bool,
                                       asset_read: NativeHandle | None) -> None:
        expected_calls = (
            "editor.asset.resolve_existing.v1",
            "editor.property.inspect_evidence.v1",
            "ai.behavior_tree.selector_inspect_evidence.v1",
        ) if live else (
            "editor.property.inspect_evidence.v1",
            "ai.behavior_tree.selector_inspect_evidence.v1",
        )
        self.assertEqual(tuple(call[0] for call in context.calls), expected_calls)
        editor, ai = context.calls[-2:]
        expected_mode = "live_asset" if live else "class_defaults"
        self.assertEqual(editor[2], {"mode": expected_mode, "exact_asset_class_path": EXACT_CLASS})
        if live:
            self.assertIs(editor[3][0]["handle"], asset_read)
        else:
            self.assertEqual(editor[3], ())
        self.assertEqual(
            ai[2],
            {"mode": "live_asset", "exact_asset_class_path": EXACT_CLASS, "exact_asset_object_path": OBJECT_PATH}
            if live else {"mode": "class_defaults", "exact_asset_class_path": EXACT_CLASS},
        )
        self.assertEqual(ai[3], ())

    def test_inspect_noncanonical_live_path_fails_before_any_primitive(self) -> None:
        malformed = inspect_request(live=True)
        malformed["target_asset_path"] = "not/a/canonical/object/path"
        context = NativeContext(5)
        queue_existing(context, exists=True)
        context.queue(
            "editor.property.inspect_evidence.v1",
            outcome(value=inspect_editor_value(live=True)),
        )
        context.queue(
            "ai.behavior_tree.selector_inspect_evidence.v1",
            outcome(value=inspect_ai_value(live=True)),
        )

        result = bootstrap_module.dispatch(malformed, context)

        self.assertEqual(
            (
                result["terminal_status"],
                result["final_phase"],
                result["diagnostics"][0]["code"],
            ),
            (1, 7, "GenericRuntime.InvalidNativeProjection"),
        )
        self.assertEqual(context.calls, [])




class ExactProfileRuntimeTests(unittest.TestCase):
    def test_exact_registration_dispatches_without_class_branch(self):
        from asset_document.tests.runtime_registration_fixtures import registry, Context
        frozen, handler = registry()
        dispatch = runtime_module.build_dispatch(frozen)
        result = dispatch(request(operation=1), Context(0))
        self.assertEqual(result['terminal_status'], 0, result)
        self.assertEqual(handler.calls.count('inspect_profile'), 2)
        self.assertIn('RuntimeRegistration', result['canonical_value'])

    def test_cached_projection_isolated_from_handler_and_caller_mutation(self):
        from asset_document.tests.runtime_registration_fixtures import registry
        frozen, handler = registry()
        dispatch = runtime_module.build_dispatch(frozen)
        original = dispatch.describe_runtime_registrations()
        handler.projection['BodySections'].append('Changed')
        returned = dispatch.describe_runtime_registrations()
        returned[0]['Class'] = 'spoof'
        returned[0]['RuntimeRegistration']['TargetPolicy'] = 'spoof'
        self.assertEqual(dispatch.describe_runtime_registrations(), original)
        self.assertEqual(handler.calls, ['inspect_profile'])

    def test_reserved_key_and_invalid_projection_rejected(self):
        from asset_document.tests.runtime_registration_fixtures import registry, Handler
        from asset_document.diagnostics import AssetDocumentContractError
        for change in ({'RuntimeRegistration': {}}, {'Class': 'wrong'}, {'BodySections': ['x', 'x']}, {'BodySections': [4]}):
            with self.subTest(change=change):
                handler = Handler()
                handler.projection.update(change)
                frozen, _ = registry(handler)
                with self.assertRaises(AssetDocumentContractError) as caught:
                    runtime_module.build_dispatch(frozen)
                self.assertEqual(caught.exception.diagnostic.code, 'GenericRuntime.TargetActivationInvalid')

    def test_action_policy_existence_matrix_preflight_and_apply(self):
        from asset_document.tests.runtime_registration_fixtures import registry, Context
        from asset_document.profile_runtime import RuntimeTargetPolicy as Policy
        for policy in Policy:
            for action in ('Create', 'Update', 'CreateOrUpdate'):
                for exists in (False, True):
                    for phase in (1, 2):
                        with self.subTest(policy=policy, action=action, exists=exists, phase=phase):
                            frozen, handler = registry(policy=policy)
                            dispatch = runtime_module.build_dispatch(frozen)
                            context = Context(phase, exists)
                            doc = document()
                            doc['Action'] = action
                            result = dispatch(request(doc), context)
                            expected = None
                            if policy is Policy.UPDATE_ONLY and action != 'Update':
                                expected = 'GenericRuntime.UpdateOnlyActionRequired'
                            elif action == 'Create' and exists:
                                expected = 'GenericRuntime.CreateTargetExists'
                            elif action == 'Update' and not exists:
                                expected = 'GenericRuntime.UpdateTargetMissing'
                            if expected:
                                self.assertEqual(result['diagnostics'][0]['code'], expected, result)
                                self.assertNotIn('create_context', handler.calls)
                                self.assertFalse(any(row[0].startswith('test.') for row in context.calls))
                                if expected.endswith('UpdateOnlyActionRequired'):
                                    self.assertEqual(context.calls, [])
                            else:
                                self.assertEqual(result['terminal_status'], 0, result)
                                self.assertEqual(handler.calls.count('create_context'), int(not exists))
                                self.assertIn('preflight' if phase == 1 else 'apply', handler.calls)
                                expected_primitive = ('test.scratch.clone.v1' if exists else 'test.scratch.create.v1') if phase == 1 else (None if exists else 'test.asset.create.v1')
                                if expected_primitive:
                                    self.assertIn(expected_primitive, [row[0] for row in context.calls])

    def test_phase_rejection_before_catalog_or_handler_and_terminal_no_handler(self):
        from asset_document.tests.runtime_registration_fixtures import registry, Context
        frozen, handler = registry()
        dispatch = runtime_module.build_dispatch(frozen)
        # Validate (3) in the Apply phase (2) is the trial apply; in Preflight it is rejected.
        for operation, phase in ((0, 0), (1, 2), (2, 1), (3, 1), (4, 2), (6, 0), (7, 2)):
            handler.calls.clear()
            context = Context(phase)
            result = dispatch(request(operation=operation), context)
            self.assertEqual(result['diagnostics'][0]['code'], 'GenericRuntime.InvalidNativePhase', result)
            self.assertEqual((handler.calls, context.calls, context.catalog_calls), ([], [], 0))
        for phase in (4, 6):
            handler.calls.clear()
            result = dispatch(request(), Context(phase))
            self.assertEqual(result['terminal_status'], 0, result)
            self.assertEqual(handler.calls, [])

    def test_validate_trial_apply_edits_a_transient_target(self):
        from asset_document.tests.runtime_registration_fixtures import registry, Context, Handler

        class SnapshotCovered(Handler):
            preflight_scratch = False

        for exists in (True, False):
            frozen, handler = registry(SnapshotCovered())
            dispatch = runtime_module.build_dispatch(frozen)
            context = Context(2, exists=exists)
            result = dispatch(request(operation=3), context)
            self.assertEqual(result['terminal_status'], 0, result)
            self.assertIn('apply', handler.calls)
            primitives = [row[0] for row in context.calls]
            self.assertIn('editor.scratch.trial_target.v1', primitives)
            # The real target is only resolved: no create, clone or create-asset primitive runs.
            self.assertNotIn('test.asset.create.v1', primitives)
            trial = next(row for row in context.calls if row[0] == 'editor.scratch.trial_target.v1')
            self.assertEqual([item['name'] for item in trial[3]], ['source_asset'] if exists else ['asset_class'])
        # A Profile whose preflight already ran on a scratch skips the trial.
        frozen, handler = registry()
        context = Context(2)
        result = runtime_module.build_dispatch(frozen)(request(operation=3), context)
        self.assertEqual(result['terminal_status'], 0, result)
        self.assertNotIn('apply', handler.calls)
        self.assertEqual(context.calls, [])

    def test_exact_handler_failure_never_falls_back(self):
        from asset_document.tests.runtime_registration_fixtures import registry, Context, Handler
        class Failing(Handler):
            def canonicalize(self, document): raise ValueError('failure')
        frozen, _ = registry(Failing())
        result = runtime_module.build_dispatch(frozen)(request(), Context(0))
        self.assertEqual(result['diagnostics'][0]['code'], 'GenericRuntime.DispatchFailed')

    def test_unbound_generic_and_blackboard_use_existing_dispatch(self):
        from asset_document.tests.runtime_registration_fixtures import registry
        frozen, _ = registry()
        dispatch = runtime_module.build_dispatch(frozen)
        for exact in ('/Script/Engine.DataAsset', BLACKBOARD_DATA_CLASS):
            req = request(operation=1, requested_asset_class_path=exact, requested_profile_class_path=exact)
            self.assertEqual(dispatch(req, NativeContext(0)), runtime_module.dispatch(req, NativeContext(0)))

    def test_create_context_names_rejected_before_factory(self):
        from asset_document.tests.runtime_registration_fixtures import registry, Context
        from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument
        frozen, handler = registry()
        handler.handles = (NamedOpaqueHandleArgument('extra', NativeHandle()),)
        context = Context(1, False)
        result = runtime_module.build_dispatch(frozen)(request(), context)
        self.assertEqual(result['diagnostics'][0]['code'], 'GenericRuntime.TargetLifecycleContextMismatch')
        self.assertEqual([row[0] for row in context.calls], ['editor.asset.resolve_existing.v1'])

    def test_registered_inspect_uses_exact_resolver_requirement(self):
        from dataclasses import replace
        from asset_document.tests.runtime_registration_fixtures import registry, Context, Handler, META, REQUIREMENTS, LIFECYCLE
        from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetPolicy
        from asset_document.registry import ProfileRegistryBuilder
        frozen, handler = registry()
        dispatch = runtime_module.build_dispatch(frozen)
        for live in (False, True):
            result = dispatch(inspect_request(live=live), Context(5))
            self.assertEqual(result['terminal_status'], 0, result)
            self.assertEqual(result['canonical_value']['mode'], 'live_asset' if live else 'class_defaults')
        reduced = tuple(item for item in REQUIREMENTS if item.primitive_id != 'editor.asset.resolve_existing.v1')
        class NoResolver(Handler):
            def required_primitives(self): return reduced
        builder = ProfileRegistryBuilder()
        builder.register_runtime(RuntimeProfileRegistration(replace(META, required_primitives=reduced), NoResolver,
                                                            RuntimeTargetPolicy.CREATE_OR_UPDATE, LIFECYCLE))
        context = Context(5)
        result = runtime_module.build_dispatch(builder.freeze())(inspect_request(live=True), context)
        self.assertEqual(result['diagnostics'][0]['code'], 'GenericRuntime.RequiredPrimitiveUnavailable')
        self.assertEqual(context.calls, [])

    def test_update_only_profile_without_lifecycle(self):
        from asset_document.tests.runtime_registration_fixtures import (
            registry, Context, UpdateOnlyHandler, UPDATE_ONLY_META, UPDATE_ONLY_REQUIREMENTS)
        from asset_document.profile_runtime import RuntimeTargetPolicy as Policy
        frozen, handler = registry(UpdateOnlyHandler(), Policy.UPDATE_ONLY, None, UPDATE_ONLY_META)
        dispatch = runtime_module.build_dispatch(frozen)
        projection = dispatch.describe_runtime_registrations()[0]['RuntimeRegistration']
        self.assertNotIn('LifecycleId', projection)
        self.assertEqual(projection['TargetPolicy'], 'UpdateOnly')
        doc = document()
        doc['Action'] = 'Update'
        for phase in (1, 2):
            with self.subTest(phase=phase):
                handler.calls.clear()
                context = Context(phase, True, UPDATE_ONLY_REQUIREMENTS)
                result = dispatch(request(doc), context)
                self.assertEqual(result['terminal_status'], 0, result)
                self.assertIn('preflight' if phase == 1 else 'apply', handler.calls)
                self.assertEqual([row[0] for row in context.calls], ['editor.asset.resolve_existing.v1'])
        result = dispatch(request(doc), Context(1, False, UPDATE_ONLY_REQUIREMENTS))
        self.assertEqual(result['diagnostics'][0]['code'], 'GenericRuntime.UpdateTargetMissing')

    def test_generic_factory_lifecycle_creates_and_populates_in_one_apply(self):
        from asset_document.tests.runtime_registration_fixtures import (
            registry, Context, GenericHandler, GENERIC_META, GENERIC_LIFECYCLE, GENERIC_REQUIREMENTS)
        frozen, handler = registry(GenericHandler(), lifecycle=GENERIC_LIFECYCLE, meta=GENERIC_META)
        dispatch = runtime_module.build_dispatch(frozen)
        self.assertEqual(dispatch.describe_runtime_registrations()[0]['RuntimeRegistration']['LifecycleId'],
                         'factory_generic.v1')
        preflight = Context(1, False, GENERIC_REQUIREMENTS)
        self.assertEqual(dispatch(request(), preflight)['terminal_status'], 0)
        scratch = next(row for row in preflight.calls if row[0] == 'editor.scratch.create_by_factory.v1')
        self.assertEqual((scratch[1], scratch[2], [item['name'] for item in scratch[3]]), (1, {}, ['asset_class']))
        apply = Context(2, False, GENERIC_REQUIREMENTS)
        handler.calls.clear()
        self.assertEqual(dispatch(request(), apply)['terminal_status'], 0)
        create = next(row for row in apply.calls if row[0] == 'editor.asset.create_by_factory.v2')
        self.assertEqual((create[1], create[2]), (2, {'package_path': '/Game/Generic', 'asset_name': 'DA_Generic'}))
        self.assertEqual(handler.calls[-1], 'apply')
        existing = Context(1, True, GENERIC_REQUIREMENTS)
        self.assertEqual(dispatch(request(), existing)['terminal_status'], 0)
        self.assertIn('editor.scratch.clone_asset.v2', [row[0] for row in existing.calls])

    def test_generic_factory_rejection_surfaces_its_diagnostic(self):
        context = NativeContext(1)
        queue_class(context)
        queue_existing(context, exists=False)
        context.queue(
            "editor.scratch.create_by_factory.v1",
            outcome(status=2, diagnostics=(diagnostic("FactoryCreate.NoFactory"),)),
        )
        result = bootstrap_module.dispatch(request(), context)
        self.assertEqual(result["diagnostics"][0]["code"], "FactoryCreate.NoFactory")
        self.assertFalse(any(call[0].startswith("editor.asset.create_by_factory") for call in context.calls))

    def test_factory_context_reaches_both_create_primitives_only_on_create(self):
        context_path = "/Game/Characters/SK_Hero.SK_Hero"
        doc = document()
        doc["FactoryContext"] = context_path
        preflight = NativeContext(1)
        queue_class(preflight)
        queue_existing(preflight, exists=False)
        skeleton, _, _ = queue_existing(preflight, exists=True)
        preflight.queue(
            "editor.scratch.create_by_factory.v1",
            outcome(value={"canonical_class": EXACT_CLASS, "lifecycle_id": "factory_generic.v1"},
                    handle_results=(handle_group("scratch", NativeHandle()),)),
        )
        queue_properties(preflight)
        preflight.queue("editor.property.preflight_admitted.v1",
                        outcome(value={"canonical_value": 7, "feasible": True}))
        assert_success(self, bootstrap_module.dispatch(request(doc), preflight))
        resolves = [call for call in preflight.calls if call[0] == "editor.asset.resolve_existing.v1"]
        self.assertEqual(resolves[1][2], {"path": context_path, "expected_class": None})
        scratch = next(call for call in preflight.calls if call[0] == "editor.scratch.create_by_factory.v1")
        self.assertEqual([item["name"] for item in scratch[3]], ["asset_class", "factory_context"])
        self.assertIs(scratch[3][1]["handle"], skeleton)

        apply = NativeContext(2)
        queue_class(apply)
        queue_existing(apply, exists=False)
        skeleton, _, _ = queue_existing(apply, exists=True)
        queue_created_asset(apply)
        queue_properties(apply)
        queue_batch_properties(apply)
        assert_success(self, bootstrap_module.dispatch(request(doc), apply))
        create = next(call for call in apply.calls if call[0] == "editor.asset.create_by_factory.v2")
        self.assertEqual([item["name"] for item in create[3]], ["asset_class", "factory_context"])
        self.assertIs(create[3][1]["handle"], skeleton)

        # Updating an existing asset never resolves the create-time context.
        update = NativeContext(1)
        queue_class(update)
        queue_existing(update, exists=True)
        update.queue(
            "editor.scratch.clone_asset.v2",
            outcome(value={"canonical_class": EXACT_CLASS, "lifecycle_id": "factory_generic.v1"},
                    handle_results=(handle_group("scratch", NativeHandle()),)),
        )
        queue_properties(update)
        update.queue("editor.property.preflight_admitted.v1",
                     outcome(value={"canonical_value": 7, "feasible": True}))
        assert_success(self, bootstrap_module.dispatch(request(doc), update))
        self.assertEqual(sum(call[0] == "editor.asset.resolve_existing.v1" for call in update.calls), 1)

    def test_class_factory_context_reaches_the_factory_as_a_class_object(self):
        # A New-asset dialog that is a class picker (SlateWidgetStyleAssetFactory's style class): FactoryContext
        # names the class; it is resolved as a class and passed as the class object, never as an asset lookup.
        style_class = "/Script/Slate.ButtonWidgetStyle"
        doc = document()
        doc["FactoryContext"] = style_class
        preflight = NativeContext(1)
        queue_class(preflight)
        queue_existing(preflight, exists=False)
        queue_class(preflight)
        class_object = NativeHandle()
        preflight.queue("editor.reference.resolve_asset.v1",
                        outcome(value={"canonical_path": style_class}, handle_results=(handle_group("asset", class_object),)))
        preflight.queue(
            "editor.scratch.create_by_factory.v1",
            outcome(value={"canonical_class": EXACT_CLASS, "lifecycle_id": "factory_generic.v1"},
                    handle_results=(handle_group("scratch", NativeHandle()),)),
        )
        queue_properties(preflight)
        preflight.queue("editor.property.preflight_admitted.v1",
                        outcome(value={"canonical_value": 7, "feasible": True}))
        assert_success(self, bootstrap_module.dispatch(request(doc), preflight))
        self.assertEqual(sum(call[0] == "editor.asset.resolve_existing.v1" for call in preflight.calls), 1)
        reference = next(call for call in preflight.calls if call[0] == "editor.reference.resolve_asset.v1")
        self.assertEqual(reference[2], {"expected_class": "/Script/CoreUObject.Class", "path": style_class})
        scratch = next(call for call in preflight.calls if call[0] == "editor.scratch.create_by_factory.v1")
        self.assertEqual([item["name"] for item in scratch[3]], ["asset_class", "factory_context"])
        self.assertIs(scratch[3][1]["handle"], class_object)

    def test_unknown_class_factory_context_fails_before_the_factory(self):
        doc = document()
        doc["FactoryContext"] = "/Script/Missing.NoSuchStyle"
        context = NativeContext(1)
        queue_class(context)
        queue_existing(context, exists=False)
        context.queue("editor.class.resolve_canonical.v1",
                      outcome(status=2, diagnostics=(diagnostic("ClassReference.NonCanonicalClass"),)))
        result = bootstrap_module.dispatch(request(doc), context)
        self.assertEqual((result["diagnostics"][0]["code"], result["diagnostics"][0]["path"]),
                         ("Generic.FactoryContextMissing", "/FactoryContext"))
        self.assertFalse(any("create_by_factory" in call[0] for call in context.calls))

    def test_missing_factory_context_asset_fails_before_the_factory(self):
        doc = document()
        doc["FactoryContext"] = "/Game/Characters/SK_Missing.SK_Missing"
        context = NativeContext(1)
        queue_class(context)
        queue_existing(context, exists=False)
        queue_existing(context, exists=False)
        result = bootstrap_module.dispatch(request(doc), context)
        self.assertEqual((result["diagnostics"][0]["code"], result["diagnostics"][0]["path"]),
                         ("Generic.FactoryContextMissing", "/FactoryContext"))
        self.assertFalse(any("create_by_factory" in call[0] for call in context.calls))

    def test_handler_diff_controls_verify_and_repair_requires_existing(self):
        from asset_document.tests.runtime_registration_fixtures import registry, Context
        frozen, handler = registry()
        dispatch = runtime_module.build_dispatch(frozen)
        result = dispatch(request(), Context(5))
        self.assertEqual(result['terminal_status'], 0, result)
        self.assertIn('diff', handler.calls)
        handler.differences = (object(),)
        result = dispatch(request(), Context(5))
        self.assertEqual(result['diagnostics'][0]['code'], 'GenericRuntime.VerificationMismatch')
        handler.calls.clear()
        result = dispatch(request(), Context(3, False))
        self.assertEqual(result['diagnostics'][0]['code'], 'GenericRuntime.TargetMissing')
        self.assertNotIn('repair', handler.calls)

    def test_verification_mismatch_names_the_value_read_back(self):
        from asset_document.diff.model import Difference, DiffKind
        from asset_document.tests.runtime_registration_fixtures import registry, Context
        frozen, handler = registry()
        dispatch = runtime_module.build_dispatch(frozen)
        handler.differences = (
            Difference('/Body/Ops/3/Settings/SourceRoot', DiffKind.ADD, True, {'BoneName': 'root'}, False, None),
            Difference('/Body/Alpha', DiffKind.REPLACE, True, 0.5, True, 0.25),
        )
        message = dispatch(request(), Context(5))['diagnostics'][0]['message']
        self.assertIn('/Body/Ops/3/Settings/SourceRoot: document {"BoneName":"root"}, read back (absent)', message)
        self.assertIn('/Body/Alpha: document 0.5, read back 0.25', message)

    def test_verify_passes_the_handlers_live_defaults_to_its_diff(self):
        from asset_document.tests.runtime_registration_fixtures import Handler, registry, Context

        class SparseHandler(Handler):
            def extract_with_defaults(self, *, target, asset_handle, context):
                self.calls.append('extract_with_defaults')
                return self.extract(target=target, asset_handle=asset_handle, context=context), {'Settings': {'A': 1}}

            def diff_with_defaults(self, authored, extracted, live_defaults, exact):
                self.calls.append(('diff', live_defaults, exact))
                return ()

        frozen, handler = registry(SparseHandler())
        result = runtime_module.build_dispatch(frozen)(request(), Context(5))
        self.assertEqual(result['terminal_status'], 0, result)
        self.assertIn('extract_with_defaults', handler.calls)
        self.assertIn(('diff', {'Settings': {'A': 1}}, False), handler.calls)

    def test_verify_hands_the_authored_document_to_an_extract_with_defaults_that_takes_it(self):
        from asset_document.tests.runtime_registration_fixtures import Handler, registry, Context

        class AuthoredHandler(Handler):
            def extract_with_defaults(self, *, target, asset_handle, context, authored=None):
                self.calls.append(('extract_with_defaults', authored))
                return self.extract(target=target, asset_handle=asset_handle, context=context), {}

            def diff_with_defaults(self, authored, extracted, live_defaults, exact):
                return ()

        frozen, handler = registry(AuthoredHandler())
        result = runtime_module.build_dispatch(frozen)(request(), Context(5))
        self.assertEqual(result['terminal_status'], 0, result)
        received = [call[1] for call in handler.calls if type(call) is tuple and call[0] == 'extract_with_defaults']
        self.assertEqual(len(received), 1)
        self.assertEqual(received[0], handler.canonicalize(document()))


if __name__ == "__main__":
    unittest.main()


class HandleHandoffRuntimeTests(unittest.TestCase):
    def test_provider_neutral_keyword_only_forwarding(self):
        from asset_document.dto import RuntimePhase
        from asset_document.regions.primitive_catalog import _NativePrimitiveContext

        class Port:
            def handoff(self, *args):
                self.args = args
                return target

        source, target = NativeHandle(), NativeHandle()
        port = Port()
        context = _NativePrimitiveContext(port, RuntimePhase.VERIFY)
        for provider in ('animation', 'render', 'exact.custom.provider'):
            for access in ('read_only', 'mutable'):
                with self.subTest(provider=provider, access=access):
                    self.assertIs(context.handoff(source_handle=source,
                        target_provider_id=provider, access=access), target)
                    self.assertEqual(port.args, (source, provider, access))
        with self.assertRaises(TypeError):
            context.handoff(source, 'animation', 'read_only')
        self.assertFalse(hasattr(context, '__dict__'))

    def _dispatch_handoff_error(self, error):
        from asset_document.tests.runtime_registration_fixtures import registry, Context, Handler

        class HandoffHandler(Handler):
            def extract(self, *, target, asset_handle, context, include_defaults=False):
                context.handoff(source_handle=asset_handle,
                    target_provider_id='animation', access='read_only')
                raise AssertionError('failed handoff must terminate dispatch')

        class FailingPort(Context):
            def handoff(self, *args):
                self.handoff_args = args
                raise error

        frozen, _ = registry(HandoffHandler())
        port = FailingPort(5)
        result = runtime_module.build_dispatch(frozen)(request(operation=6), port)
        self.assertEqual(port.handoff_args[1:], ('animation', 'read_only'))
        self.assertEqual(result['changed_set'], ())
        return result

    def test_all_frozen_native_terminals_preserved_at_dispatch(self):
        # Independent copy of the frozen §5.2 terminal contract.
        codes = (
            'AD.RUNTIME.HANDOFF_UNAVAILABLE',
            'AD.RUNTIME.HANDOFF_INVALID_CONTEXT',
            'AD.RUNTIME.HANDOFF_TARGET_UNAVAILABLE',
            'AD.RUNTIME.HANDOFF_KIND_UNSUPPORTED',
            'AD.RUNTIME.HANDOFF_ACCESS_ESCALATION',
            'AD.RUNTIME.HANDOFF_TRANSACTION_REQUIRED',
            'AD.RUNTIME.HANDOFF_STORAGE_REJECTED',
            'HandleMetadata.RequestMismatch',
            'HandleMetadata.TransactionMismatch',
            'HandleMetadata.RuntimeRevisionMismatch',
            'HandleMetadata.ProviderMismatch',
            'HandleMetadata.InvalidPhase',
            'HandleMetadata.InvalidKind',
            'HandleMetadata.InvalidAccess',
            'HandleMetadata.KindMismatch',
            'HandleMetadata.AccessMismatch',
            'HandleMetadata.InvalidSlot',
            'HandleMetadata.InvalidGeneration',
            'AD.RUNTIME.STALE_HANDLE',
        )
        message = ' Original native message: 原文\nwith trailing space '
        for code in codes:
            with self.subTest(code=code):
                result = self._dispatch_handoff_error(RuntimeError(f'{code}: {message}'))
                row = result['diagnostics'][0]
                self.assertEqual((row['path'], row['code'], row['message']),
                    ('/HandleHandoff', code, message))

    def test_unknown_malformed_and_other_exceptions_still_dispatch_failed(self):
        code = 'AD.RUNTIME.HANDOFF_INVALID_CONTEXT'
        errors = (
            RuntimeError('Unknown.Code: native message'),
            RuntimeError(f'{code}: '), RuntimeError(code),
            RuntimeError(f'{code}:missing separator space'),
            RuntimeError(f' {code}: message'), RuntimeError(f'{code} : message'),
            ValueError(f'{code}: native message'),
            TypeError(f'{code}: native message'),
        )
        for error in errors:
            with self.subTest(error=repr(error)):
                result = self._dispatch_handoff_error(error)
                self.assertEqual(result['diagnostics'][0]['code'],
                    'GenericRuntime.DispatchFailed')
