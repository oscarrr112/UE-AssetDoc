# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from dataclasses import dataclass
import json
from types import MappingProxyType
from typing import Callable

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.canonical.semantic_hash import (
    SemanticHashScope,
    compute_semantic_hash,
)
from asset_document.diagnostics import (
    AssetDocumentContractError,
    RuntimeDiagnostic,
    contract_error,
    with_diagnostics,
)
from asset_document.diff import semantic_compare
from asset_document.dto import (
    ChangedRecord,
    OptionalJsonValue,
    RuntimeOperation,
    RuntimePhase,
    RuntimeRequestDto,
    RuntimeResultDto,
    TerminalStatus,
    _decode_native_request,
    _project_native_failure,
    _project_native_result,
    _require_native_dict,
    _validate_native_context,
)
from asset_document.profiles.generic import GenericProfileCandidate
from asset_document.profiles.behavior_tree_production import BehaviorTreeProfile
from asset_document.profiles import behavior_tree_execution
from asset_document.profiles.blackboard_data import (
    BlackboardDataProfileCandidate,
    BlackboardSnapshot,
    BlackboardTransition,
    build_blackboard_footprint_projection,
    plan_blackboard_transitions,
)
from asset_document.meta import DocumentAction, ProfileMeta
from asset_document import inspect_summary as inspect_outline
from asset_document.read_only_extract import extract_document, inspect_live, is_live_inspect_path
from asset_document.registry import FrozenProfileRegistry, ProfileRegistryBuilder
from asset_document.regions.target_lifecycle import GENERIC_FACTORY_LIFECYCLE
from asset_document.regions.primitive_catalog import (
    GenericPrimitiveContext,
    NamedOpaqueHandleArgument,
    PrimitiveOutcomeStatus,
    _NativePrimitiveContext,
    _invoke_required,
    _optional_handle,
    _single_handle,
    _validate_native_catalog,
)


_CLASS_ID = "editor.class.resolve_canonical.v1"
_RESOLVE_ID = "editor.asset.resolve_existing.v1"
_REFERENCE_RESOLVE_ID = "editor.reference.resolve_asset.v1"
_CLASS_CLASS_PATH = "/Script/CoreUObject.Class"
# Blackboard keeps its exact data-asset factory; Generic candidates use the
# class-neutral generic factory lifecycle (any class the editor can create).
_BLACKBOARD_CREATE_ASSET_ID = "editor.asset.create_by_factory.v1"
_GENERIC_LIFECYCLE = GENERIC_FACTORY_LIFECYCLE


ProfileFactory = Callable[[], object]
ProfileBinding = tuple[ProfileMeta, ProfileFactory]


class _BlackboardRuntimePrimitiveContext(_NativePrimitiveContext):
    __slots__ = ()

    def handoff_to_ai_mutable(self, handle: object) -> object:
        return self._native.handoff(handle, "ai", "mutable")


@dataclass(frozen=True, slots=True)
class _BlackboardSnapshotRelay:
    identity: tuple[str, int, str, str, str]
    snapshot: BlackboardSnapshot
    transitions: tuple[BlackboardTransition, ...]
    projection: dict


_profile_registry: FrozenProfileRegistry | None = None
_profile_factories = MappingProxyType({})
_blackboard_snapshot_slot: _BlackboardSnapshotRelay | None = None
_behavior_tree_expected_slot = None


def _clear_behavior_tree_expected():
    global _behavior_tree_expected_slot
    _behavior_tree_expected_slot = None


def _behavior_tree_expected_identity(request):
    canonical = BehaviorTreeProfile().canonicalize(request.canonical_document)
    return _relay_identity(request) + (_evidence(canonical)[1],)


def _prepare_behavior_tree_expected(request, phase, profile):
    if not _is_behavior_tree_profile(profile) or request.operation is not RuntimeOperation.APPLY:
        _clear_behavior_tree_expected()
        return
    if phase in (RuntimePhase.VALIDATE, RuntimePhase.PREFLIGHT):
        _clear_behavior_tree_expected()
    elif _behavior_tree_expected_slot is not None:
        if _behavior_tree_expected_slot[0] != _behavior_tree_expected_identity(request):
            _clear_behavior_tree_expected()
            raise contract_error("/BehaviorTreeExpected", "BehaviorTree.ExpectedIdentityMismatch",
                                 "Expected semantics belong to another request or revision.")


def _store_behavior_tree_expected(request, value):
    global _behavior_tree_expected_slot
    if _behavior_tree_expected_slot is not None:
        raise contract_error("/BehaviorTreeExpected", "BehaviorTree.ExpectedAlreadyPresent",
                             "One Apply may publish its expected semantics once.")
    codec = DocumentCodec()
    _behavior_tree_expected_slot = (_behavior_tree_expected_identity(request), codec.encode(value))


def _get_behavior_tree_expected(request):
    slot = _behavior_tree_expected_slot
    if slot is None or slot[0] != _behavior_tree_expected_identity(request):
        _clear_behavior_tree_expected()
        raise contract_error("/BehaviorTreeExpected", "BehaviorTree.ExpectedMissing",
                             "This phase requires this Apply's pre-mutation semantic expectation.")
    return DocumentCodec().decode(slot[1])


# Interpreter exit also releases the bounded pure-value baseline.
import atexit
atexit.register(_clear_behavior_tree_expected)


def build_profile_registry(bindings: tuple[ProfileBinding, ...]) -> FrozenProfileRegistry:
    builder = ProfileRegistryBuilder()
    for profile, factory in bindings:
        if not callable(factory):
            raise contract_error(
                "/Registry",
                "GenericRuntime.InvalidProfileFactory",
                "Profile factories must be callable.",
            )
        builder.register(profile)
    return builder.freeze()


def configure_profile_registry(bindings: tuple[ProfileBinding, ...]) -> None:
    global _profile_registry, _profile_factories
    if _profile_registry is not None:
        raise contract_error(
            "/Registry",
            "Registry.Frozen",
            "The runtime Profile registry is already frozen.",
        )
    _profile_registry = build_profile_registry(bindings)
    _profile_factories = MappingProxyType({profile.profile_id: factory for profile, factory in bindings})


def profile_factory_for(profile: ProfileMeta) -> ProfileFactory:
    try:
        return _profile_factories[profile.profile_id]
    except KeyError:
        raise contract_error(
            "/Registry",
            "GenericRuntime.ProfileFactoryUnavailable",
            "The frozen Profile registration has no factory.",
        ) from None


def _is_blackboard_profile(profile: object) -> bool:
    return type(profile) is BlackboardDataProfileCandidate


def _is_behavior_tree_profile(profile: object) -> bool:
    return type(profile) is BehaviorTreeProfile


def _relay_identity(request: RuntimeRequestDto) -> tuple[str, int, str, str, str]:
    return (
        request.request_id,
        request.expected_runtime_revision,
        request.requested_asset_class_path,
        request.target_asset_path,
        request.operation.value,
    )


def _copy_blackboard_snapshot(snapshot: BlackboardSnapshot) -> BlackboardSnapshot:
    codec = DocumentCodec()
    entries = tuple(codec.decode(codec.encode(entry)) for entry in snapshot.entries)
    return BlackboardSnapshot(snapshot.parent_path_or_null, entries, tuple(snapshot.ordered_names))


def _copy_blackboard_transitions(
    transitions: tuple[BlackboardTransition, ...],
) -> tuple[BlackboardTransition, ...]:
    codec = DocumentCodec()
    return tuple(
        BlackboardTransition(
            transition.kind,
            transition.name,
            None if transition.entry is None else codec.decode(codec.encode(transition.entry)),
            transition.index,
            tuple(transition.ordered_names),
        )
        for transition in transitions
    )


def _copy_blackboard_projection(projection: dict) -> dict:
    def freeze(value: object):
        if type(value) is dict:
            return MappingProxyType({key: freeze(item) for key, item in value.items()})
        if type(value) is list:
            return tuple(freeze(item) for item in value)
        if type(value) is tuple:
            return tuple(freeze(item) for item in value)
        return value

    return freeze(projection)


def _prepare_blackboard_relay(
    request: RuntimeRequestDto,
    phase: RuntimePhase,
    profile: object,
) -> dict | None:
    global _blackboard_snapshot_slot
    slot = _blackboard_snapshot_slot
    if slot is None:
        return
    identity = _relay_identity(request)
    if phase is RuntimePhase.APPLY:
        if _is_blackboard_profile(profile) and slot.identity == identity:
            return
        _blackboard_snapshot_slot = None
        raise contract_error(
            "/BlackboardSnapshotRelay",
            "BlackboardData.RelayIdentityMismatch",
            "Apply must consume the exact Blackboard Preflight snapshot.",
        )
    if _is_blackboard_profile(profile) and phase is RuntimePhase.PREFLIGHT and slot.identity == identity:
        raise contract_error(
            "/BlackboardSnapshotRelay",
            "BlackboardData.RelayDuplicatePreflight",
            "A Blackboard Preflight snapshot is already pending for this identity.",
        )
    _blackboard_snapshot_slot = None


def _store_blackboard_snapshot(
    request: RuntimeRequestDto,
    snapshot: BlackboardSnapshot,
    transitions: tuple[BlackboardTransition, ...],
    projection: dict,
) -> None:
    global _blackboard_snapshot_slot
    if _blackboard_snapshot_slot is not None:
        raise contract_error(
            "/BlackboardSnapshotRelay",
            "BlackboardData.RelayCapacityExceeded",
            "The bounded Blackboard Preflight relay is occupied.",
        )
    _blackboard_snapshot_slot = _BlackboardSnapshotRelay(
        _relay_identity(request),
        _copy_blackboard_snapshot(snapshot),
        _copy_blackboard_transitions(transitions),
        _copy_blackboard_projection(projection),
    )


def _take_blackboard_snapshot(request: RuntimeRequestDto) -> _BlackboardSnapshotRelay:
    global _blackboard_snapshot_slot
    slot = _blackboard_snapshot_slot
    if slot is None:
        raise contract_error(
            "/BlackboardSnapshotRelay",
            "BlackboardData.RelayMissing",
            "Apply requires one successful Blackboard Preflight snapshot.",
        )
    _blackboard_snapshot_slot = None
    if slot.identity != _relay_identity(request):
        raise contract_error(
            "/BlackboardSnapshotRelay",
            "BlackboardData.RelayIdentityMismatch",
            "Apply must consume the exact Blackboard Preflight snapshot.",
        )
    return _BlackboardSnapshotRelay(
        slot.identity,
        _copy_blackboard_snapshot(slot.snapshot),
        _copy_blackboard_transitions(slot.transitions),
        _copy_blackboard_projection(slot.projection),
    )


def _resolve_class(context: GenericPrimitiveContext, class_path: str):
    result = _invoke_required(
        context,
        _CLASS_ID,
        values={"class": class_path, "expected_base": None},
    )
    return _single_handle(result, "class")


def _resolve_existing(
    context: GenericPrimitiveContext,
    target: str,
    class_path: str,
) -> tuple[bool, object, object]:
    result = _invoke_required(
        context,
        _RESOLVE_ID,
        values={
            "path": target,
            "expected_class": class_path,
        },
    )
    if type(result.value) is not dict or type(result.value.get("exists")) is not bool:
        raise contract_error(
            "/PrimitiveOutcome/Value",
            "GenericRuntime.InvalidNativeProjection",
            "resolve-existing omitted exact existence evidence",
        )
    asset_read = _optional_handle(result, "asset_read")
    asset_mutable = _optional_handle(result, "asset_mutable")
    if result.value["exists"] != (asset_read is not None and asset_mutable is not None):
        raise contract_error(
            "/PrimitiveOutcome/HandleResults",
            "GenericRuntime.InvalidNativeProjection",
            "resolve-existing evidence and handles disagree",
        )
    return result.value["exists"], asset_read, asset_mutable


def _preflight(
    request: RuntimeRequestDto,
    document: dict,
    profile: object,
    context: GenericPrimitiveContext,
) -> tuple[ChangedRecord, ...]:
    if _is_behavior_tree_profile(profile):
        return profile.preflight(document, context=context)
    if _is_blackboard_profile(profile):
        return ()
    class_handle = _resolve_class(context, request.requested_asset_class_path)
    exists, asset_read, _ = _resolve_existing(
        context,
        request.target_asset_path,
        request.requested_asset_class_path,
    )
    lifecycle = _GENERIC_LIFECYCLE
    if exists:
        scratch_outcome = _invoke_lifecycle(
            context,
            lifecycle.clone_scratch,
            (NamedOpaqueHandleArgument("source_asset", asset_read),),
        )
    else:
        # Fails here, before any native preparation, with FactoryCreate.* when
        # the class is abstract, a Blueprint asset class or has no factory.
        scratch_outcome = _invoke_lifecycle(
            context,
            lifecycle.create_scratch,
            (NamedOpaqueHandleArgument("asset_class", class_handle),)
            + _factory_context_handles(document, profile, context),
        )
    warnings = profile.preflight(document, _single_handle(scratch_outcome, "scratch"), context)
    return with_diagnostics((), warnings or ())


def _invoke_lifecycle(context, requirement, handles, values=None):
    outcome = context.invoke(requirement=requirement, value_arguments={} if values is None else values,
                             handle_arguments=handles)
    if outcome.status is PrimitiveOutcomeStatus.SUCCEEDED:
        return outcome
    if outcome.diagnostics:
        raise AssetDocumentContractError(outcome.diagnostics[0])
    raise contract_error("/PrimitiveOutcome", "GenericRuntime.InvalidNativeProjection",
                         "failed lifecycle primitive omitted its diagnostic")


def _target_location(request: RuntimeRequestDto) -> dict:
    package_name, asset_name = request.target_asset_path.rsplit(".", 1)
    return {"package_path": package_name.rsplit("/", 1)[0], "asset_name": asset_name}


def _factory_context_handles(document: dict, profile: object, context: GenericPrimitiveContext) -> tuple:
    """The optional generic-document FactoryContext as the factory_context handle.

    It is what the class's New-asset dialog asks for: an existing asset (a Skeleton, a Static Mesh, ...) or,
    for a dialog that is a class picker, a class (a native ``/Script/Module.Class`` or a Blueprint's
    ``..._C`` class: SlateWidgetStyleAssetFactory's style class). The native factory configuration puts it on
    the one factory field that accepts it and applies the picker's rules (meta class, abstract, deprecated)."""
    path = profile.factory_context(document) if isinstance(profile, GenericProfileCandidate) else None
    if path is None:
        return ()
    is_class_path = path.startswith("/Script/") or path.endswith("_C")
    if not path.startswith("/Script/"):
        exists, asset_read, _ = _resolve_existing(context, path, None)
        if exists:
            return (NamedOpaqueHandleArgument("factory_context", asset_read),)
    class_handle = _factory_context_class(context, path) if is_class_path else None
    if class_handle is None:
        raise contract_error(
            "/FactoryContext",
            "Generic.FactoryContextMissing",
            f"FactoryContext '{path}' does not exist: give the object path of an existing asset "
            "(or, for a factory whose dialog picks a class, a loadable class path).",
        )
    return (NamedOpaqueHandleArgument("factory_context", class_handle),)


def _factory_context_class(context: GenericPrimitiveContext, class_path: str):
    """The class object a class-picker FactoryContext names, as an asset-kind handle (None: not a class)."""
    try:
        _invoke_required(context, _CLASS_ID, values={"class": class_path, "expected_base": None})
        outcome = _invoke_required(context, _REFERENCE_RESOLVE_ID,
                                   values={"expected_class": _CLASS_CLASS_PATH, "path": class_path})
    except AssetDocumentContractError:
        return None
    return _single_handle(outcome, "asset")


def _created_asset(request: RuntimeRequestDto, class_handle: object, context: GenericPrimitiveContext,
                   document: dict, profile: object):
    result = _invoke_lifecycle(
        context,
        _GENERIC_LIFECYCLE.create_asset,
        (NamedOpaqueHandleArgument("asset_class", class_handle),)
        + _factory_context_handles(document, profile, context),
        _target_location(request),
    )
    return (
        _single_handle(result, "asset_read"),
        _single_handle(result, "asset_mutable"),
    )


def _created_blackboard(request: RuntimeRequestDto, class_handle: object, context: GenericPrimitiveContext):
    result = _invoke_required(
        context,
        _BLACKBOARD_CREATE_ASSET_ID,
        values={"factory_id": "data_asset_factory.v1", **_target_location(request)},
        handles=(NamedOpaqueHandleArgument("asset_class", class_handle),),
    )
    return (
        _single_handle(result, "asset_read"),
        _single_handle(result, "asset_mutable"),
    )


def _apply(
    request: RuntimeRequestDto,
    document: dict,
    profile: object,
    context: GenericPrimitiveContext,
    blackboard_relay: _BlackboardSnapshotRelay | None = None,
) -> tuple[ChangedRecord, ...]:
    if _is_behavior_tree_profile(profile):
        return profile.apply(document, context=context,
            expected_sink=lambda value: _store_behavior_tree_expected(request, value))
    if _is_blackboard_profile(profile):
        if blackboard_relay is None:
            raise contract_error(
                "/BlackboardSnapshotRelay",
                "BlackboardData.RelayMissing",
                "Apply requires one successful Blackboard Preflight snapshot.",
            )
        exists, _, asset_mutable = _resolve_existing(
            context,
            request.target_asset_path,
            request.requested_asset_class_path,
        )
        if not exists:
            class_handle = _resolve_class(context, request.requested_asset_class_path)
            _, asset_mutable = _created_blackboard(request, class_handle, context)
            resolved, _, asset_mutable = _resolve_existing(
                context,
                request.target_asset_path,
                request.requested_asset_class_path,
            )
            if not resolved:
                raise contract_error(
                    "/TargetAssetPath",
                    "BlackboardData.CreatedAssetNotResolvable",
                    "The newly created Blackboard must resolve before provider handoff.",
                )
        if type(context) is not _BlackboardRuntimePrimitiveContext:
            raise contract_error(
                "/PrimitiveContext",
                "BlackboardData.NativeHandoffUnavailable",
                "Blackboard Apply requires the bound native handoff context.",
            )
        asset_mutable = context.handoff_to_ai_mutable(asset_mutable)
        return profile.apply(
            document,
            current_snapshot=blackboard_relay.snapshot,
            context=context,
            mutable_asset_handle=asset_mutable,
            transitions=blackboard_relay.transitions,
        )
    class_handle = _resolve_class(context, request.requested_asset_class_path)
    exists, asset_read, asset_mutable = _resolve_existing(
        context,
        request.target_asset_path,
        request.requested_asset_class_path,
    )
    if not exists:
        asset_read, asset_mutable = _created_asset(request, class_handle, context, document, profile)
    return profile.apply(
        document,
        asset_read,
        asset_mutable,
        context,
    )


def _comparison_projection(value: dict) -> dict:
    return {
        "Target": value["Target"],
        "Class": value["Class"],
        "Properties": value.get("Properties", {}),
    }


def sidecar_form(value: dict) -> dict:
    """The document as the sidecar holds it after an apply: a Create is written back as CreateOrUpdate, so that
    editing the sidecar again re-applies to the asset the Create made (Create would refuse TargetExists)."""
    if type(value) is dict and value.get("Action") == "Create":
        return {**value, "Action": "CreateOrUpdate"}
    return value


def _evidence(value: dict) -> tuple[bytes, str]:
    """The sidecar bytes and their semantic hash (both of the sidecar form)."""
    codec = DocumentCodec()
    value = sidecar_form(value)
    return (
        codec.encode_sidecar(value),
        compute_semantic_hash(
            value,
            scope=SemanticHashScope.DOCUMENT,
            codec=codec,
        ),
    )


def _verify(
    request: RuntimeRequestDto,
    document: dict,
    profile: object,
    context: GenericPrimitiveContext,
) -> None:
    if _is_behavior_tree_profile(profile):
        expected = _get_behavior_tree_expected(request)
        try:
            return profile.verify(document, context=context, expected=expected)
        finally:
            _clear_behavior_tree_expected()
    exists, asset_read, _ = _resolve_existing(
        context,
        request.target_asset_path,
        request.requested_asset_class_path,
    )
    if not exists:
        raise contract_error(
            "/Target",
            "GenericRuntime.TargetMissing",
            "Verify could not reacquire the target asset",
        )
    if _is_blackboard_profile(profile):
        extracted = profile.extract(
            target=document["Target"],
            current_snapshot=profile.observe_current_snapshot(
                target=document["Target"],
                context=context,
            ),
        )
    elif type(profile) is GenericProfileCandidate:
        # Partial values (omitted struct members / instanced Properties keep
        # their current value) are read in the authored shape. Every live value,
        # defaults included: an authored value equal to its class default must
        # read back as present, not as omitted.
        extracted = profile.extract(
            target=document["Target"],
            asset_handle=asset_read,
            context=context,
            include_defaults=True,
            authored_properties=_authored_properties(document),
        )
    else:
        extracted = profile.extract(
            target=document["Target"],
            asset_handle=asset_read,
            context=context,
            include_defaults=True,
        )
    actual = profile.canonicalize(extracted)
    if _is_blackboard_profile(profile):
        codec = DocumentCodec()
        expected_projection = codec.decode(codec.encode(document))
        actual_projection = codec.decode(codec.encode(actual))
        actual_projection["Action"] = expected_projection["Action"]
        _, actual_hash = _evidence(actual_projection)
        if expected_projection != actual_projection:
            raise contract_error(
                "/",
                "GenericRuntime.VerificationMismatch",
                "extracted Blackboard semantic value, bytes, or hash does not match",
            )
        return actual_projection
    expected_projection = _comparison_projection(document)
    actual_projection = _comparison_projection(actual)
    actual_properties = actual_projection["Properties"]
    actual_projection["Properties"] = {
        name: actual_properties[name]
        for name in expected_projection["Properties"]
        if name in actual_properties
    }
    expected_bytes, expected_hash = _evidence(expected_projection)
    actual_bytes, actual_hash = _evidence(actual_projection)
    if (
        expected_projection != actual_projection
        or expected_bytes != actual_bytes
        or expected_hash != actual_hash
    ):
        raise contract_error(
            "/",
            "GenericRuntime.VerificationMismatch",
            "extracted semantic value, bytes, or hash does not match"
            + _describe_property_mismatch(expected_projection["Properties"], actual_properties),
        )
    return None


def _describe_property_mismatch(expected: dict, actual: dict) -> str:
    """Names the authored properties the asset holds differently (AI-readable)."""
    def text(value) -> str:
        try:
            return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
        except (TypeError, ValueError):
            return repr(value)

    parts = []
    for name in sorted(expected):
        if name in actual and text(expected[name]) == text(actual[name]):
            continue
        if name not in actual:
            parts.append(f"{name} (asset holds <missing>)")
            continue
        # The complete value, and the innermost member that differs.
        path, mine, theirs = semantic_compare.first_difference(name, expected[name], actual[name])
        where = "" if path == name else f"; at {path} document {semantic_compare.describe(mine, 1000)}, asset " \
            f"{semantic_compare.describe(theirs, 1000)}"
        parts.append(f"{name} (asset holds {semantic_compare.describe(actual[name], 1000)}, document "
                     f"{semantic_compare.describe(expected[name], 1000)}{where})")
    if not parts:
        return ""
    shown = "; ".join(parts[:6]) + ("; ..." if len(parts) > 6 else "")
    return f": {shown}. The editor or a reload changed these values; author what the asset holds."


def _success_result(
    request: RuntimeRequestDto,
    revision: int,
    *,
    canonical_value: OptionalJsonValue = OptionalJsonValue(),
    canonical_bytes: bytes = b"",
    semantic_hash: str = "",
    codec_version: int = 0,
    changed_set: tuple[ChangedRecord, ...] = (),
    diagnostics: tuple[RuntimeDiagnostic, ...] = (),
) -> RuntimeResultDto:
    return RuntimeResultDto(
        request_id=request.request_id,
        terminal_status=TerminalStatus.SUCCEEDED,
        final_phase=RuntimePhase.COMPLETE,
        runtime_revision=revision,
        diagnostics=diagnostics,
        canonical_value=canonical_value,
        canonical_bytes=canonical_bytes,
        semantic_hash=semantic_hash,
        codec_version=codec_version,
        changed_set=changed_set,
    )


def _evidence_result(
    request: RuntimeRequestDto,
    revision: int,
    canonical: dict,
    diagnostics: tuple[RuntimeDiagnostic, ...] = (),
) -> RuntimeResultDto:
    canonical_bytes, semantic_hash = _evidence(canonical)
    return _success_result(
        request,
        revision,
        canonical_value=OptionalJsonValue(True, canonical),
        canonical_bytes=canonical_bytes,
        semantic_hash=semantic_hash,
        codec_version=DocumentCodec().version,
        diagnostics=diagnostics,
    )


def _read_only_target(request: RuntimeRequestDto) -> str:
    return request.target_asset_path.rsplit(".", 1)[0]


def _extract_existing(
    request: RuntimeRequestDto,
    profile: object,
    context: GenericPrimitiveContext,
) -> dict:
    if _is_behavior_tree_profile(profile):
        options = request.canonical_document
        # Diff reads every live value so an authored default compares as present.
        return profile.extract(target=_read_only_target(request), context=context,
            include_defaults=request.operation is RuntimeOperation.DIFF
            or (type(options) is dict and options.get("include_defaults") is True))
    exists, asset_read, _ = _resolve_existing(
        context,
        request.target_asset_path,
        request.requested_asset_class_path,
    )
    if not exists:
        raise contract_error(
            "/Target",
            "GenericRuntime.TargetMissing",
            "read-only operation could not resolve the target asset",
        )
    if _is_blackboard_profile(profile):
        target = _read_only_target(request)
        return profile.extract(
            target=target,
            current_snapshot=profile.observe_current_snapshot(
                target=target, context=context
            ),
        )
    options = request.canonical_document
    # Diff compares authored values with every live value, defaults included,
    # so an authored value equal to its default is unchanged rather than added.
    include_defaults = request.operation is RuntimeOperation.DIFF or (
        type(options) is dict and options.get("include_defaults") is True
    )
    if request.operation is RuntimeOperation.DIFF and type(profile) is GenericProfileCandidate:
        return profile.extract(
            target=_read_only_target(request),
            asset_handle=asset_read,
            context=context,
            include_defaults=include_defaults,
            authored_properties=_authored_properties(request.canonical_document),
        )
    return profile.extract(
        target=_read_only_target(request),
        asset_handle=asset_read,
        context=context,
        include_defaults=include_defaults,
    )


def _authored_properties(document: object) -> dict | None:
    properties = document.get("Properties") if type(document) is dict else None
    return properties if type(properties) is dict else None


# Profiles compute Differences as (document, asset) pairs: ``before`` is the
# authored value and ``after`` the live one. The public Diff reads as what Apply
# does to the asset: before = current asset value, after = document value, and the
# kind is relative to the asset ("Add" = only the document has it, "Remove" = only
# the asset has it). The kind is derived from presence, not from the profile's
# DiffKind: canonical diffs name a document-only item REMOVE, while several profiles
# name it ADD (what Apply would do) and mark a document-absent side with a null.
_TARGET_MISSING_CODES = frozenset({"GenericRuntime.TargetMissing", "BehaviorTree.TargetMissing"})


def _public_diff_kind(value) -> str:
    document_present, asset_present = value.before_present, value.after_present
    if document_present and not asset_present:
        return "Add"
    if asset_present and not document_present:
        return "Remove"
    if value.kind.value != "Replace":
        # Both sides "present" but the profile reports an insertion or a removal:
        # its absent side is a null placeholder.
        if value.before is None and value.after is not None:
            return "Remove"
        if value.after is None and value.before is not None:
            return "Add"
    return "Replace"


def _difference_projection(value) -> dict:
    projection = {
        "path": value.path,
        "kind": _public_diff_kind(value),
        "before": value.after,
        "after": value.before,
    }
    if getattr(value, "row", None) is not None:
        projection["row"] = value.row
    return projection


def _public_diff_entry(entry: dict) -> dict:
    """Give a Profile's own receipt entry the shared path/kind/before/after shape
    (BehaviorTree publishes current/desired with a status)."""
    if type(entry) is not dict or "kind" in entry:
        return entry
    if "current" in entry or "desired" in entry:
        before, after = entry.get("current"), entry.get("desired")
        kind = "Add" if before is None else "Remove" if after is None else "Replace"
        return {**entry, "kind": kind, "before": before, "after": after}
    kind = {"added": "Add", "removed": "Remove", "changed": "Replace"}.get(entry.get("status"))
    return entry if kind is None else {**entry, "kind": kind}


def _public_diff_payload(payload: dict) -> dict:
    return {key: [_public_diff_entry(entry) for entry in value] if type(value) is list
            and key in ("changed", "added", "removed") else value for key, value in payload.items()}


def _would_create_diff(authored: dict) -> dict:
    """Diff of a document whose target does not exist yet: Apply would create it
    with the whole document; Update on a missing target is rejected as Apply rejects it."""
    if authored.get("Action") == DocumentAction.UPDATE.value:
        raise contract_error(
            "/Action",
            "GenericRuntime.UpdateTargetMissing",
            "Update target does not exist; use Action Create or CreateOrUpdate to create it.",
        )
    return {
        "would_create": True,
        "changed": [{"path": "/", "kind": "Add", "before": None, "after": authored}],
        "unchanged": [],
        "skipped": [],
        "failed": [],
    }


def _read_only_result(
    request: RuntimeRequestDto,
    revision: int,
    profile: object,
    context: GenericPrimitiveContext,
) -> RuntimeResultDto:
    authored = None
    if request.operation is RuntimeOperation.DIFF:
        # An invalid authored document keeps its public diagnostic even when a
        # Create document has no target available for extraction, and Diff
        # rejects what Apply would reject.
        authored = profile.canonicalize(request.canonical_document)
        if _is_behavior_tree_profile(profile):
            profile.validate(authored, context=context)
        elif type(profile) is GenericProfileCandidate:
            profile.validate_authored_properties(request.canonical_document, context)
    try:
        extracted = extract_document(
            _read_only_target(request), lambda: _extract_existing(request, profile, context), profile.canonicalize,
            read_only=request.operation is RuntimeOperation.EXTRACT)
    except AssetDocumentContractError as error:
        if request.operation is not RuntimeOperation.DIFF or error.diagnostic.code not in _TARGET_MISSING_CODES:
            raise
        return _evidence_result(request, revision, _would_create_diff(authored))
    if request.operation is RuntimeOperation.EXTRACT:
        return _evidence_result(request, revision, extracted)
    if _is_behavior_tree_profile(profile):
        return _evidence_result(request, revision, _public_diff_payload(profile.diff_payload(
            authored, extracted, context=context, validated=True)))
    differences = profile.diff(authored, extracted)
    return _evidence_result(
        request,
        revision,
        {
            "changed": [_difference_projection(item) for item in differences],
            "unchanged": [],
            "skipped": [],
            "failed": [],
        },
    )


def _phase_changed_set(
    phase: RuntimePhase,
    request: RuntimeRequestDto,
    canonical: dict,
    profile: object,
    context: GenericPrimitiveContext,
    blackboard_relay: _BlackboardSnapshotRelay | None = None,
) -> tuple[ChangedRecord, ...]:
    if phase is RuntimePhase.PREFLIGHT:
        return _preflight(request, canonical, profile, context)
    if phase is RuntimePhase.APPLY:
        return _apply(request, canonical, profile, context, blackboard_relay)
    if phase is RuntimePhase.REPAIR:
        if _is_behavior_tree_profile(profile):
            return profile.repair(canonical, context=context, expected=_get_behavior_tree_expected(request))
        if _is_blackboard_profile(profile):
            return profile.repair(canonical, context=context)
        return profile.repair(canonical, None, context)
    if phase in (RuntimePhase.PERSIST, RuntimePhase.COMPLETE):
        if phase is RuntimePhase.COMPLETE and _is_behavior_tree_profile(profile):
            _clear_behavior_tree_expected()
        return ()
    raise contract_error(
        "/Phase",
        "GenericRuntime.InvalidNativePhase",
        "Failed is not a dispatchable semantic phase",
    )


def _validate_only_profile_projection(
    request: RuntimeRequestDto,
    phase: RuntimePhase,
    profile: GenericProfileCandidate,
    revision: int,
) -> RuntimeResultDto | None:
    if request.operation not in (RuntimeOperation.SCHEMA, RuntimeOperation.TEMPLATE):
        return None
    if phase is not RuntimePhase.VALIDATE:
        raise contract_error(
            "/Phase",
            "GenericRuntime.InvalidNativePhase",
            "Schema and Template operations require the bound Validate context",
        )
    canonical = (
        profile.inspect_profile()
        if request.operation is RuntimeOperation.SCHEMA
        else profile.create_template(_read_only_target(request))
    )
    return _evidence_result(request, revision, canonical)


def _inspect_options(request: RuntimeRequestDto) -> dict:
    """``{"mode"}``, or ``{"mode", "summary": True}`` for an inspect that summarizes large profile details."""
    document = request.canonical_document
    keys = {"mode", "summary"} if type(document) is dict and "summary" in document else {"mode"}
    options = _require_native_dict(document, keys, "/CanonicalDocument")
    if "summary" in options and options["summary"] is not True:
        raise contract_error(
            "/CanonicalDocument/summary",
            "GenericRuntime.InvalidNativeProjection",
            "Inspect summary must be exactly true when present",
        )
    return options


def inspect_summary(request: RuntimeRequestDto) -> bool:
    """Whether the inspect asks a profile to summarize its large details (a profile without any ignores it)."""
    return _inspect_options(request).get("summary") is True


def summarized(handler, request: RuntimeRequestDto, inspected):
    """The inspect result (a live asset's or a class's defaults), summarized when the request asks for a summary
    (inspect_summary.py): by the profile's optional ``summarize_inspect(inspected)``, else with its large document
    ``Body`` as an outline and its large ``properties`` as names, types and small values."""
    if not inspect_summary(request) or type(inspected) is not dict:
        return inspected
    summarize = getattr(handler, "summarize_inspect", None)
    if callable(summarize):
        return summarize(inspected)
    return inspect_outline.summarize(inspected)


def _inspect_mode(request: RuntimeRequestDto) -> str:
    options = _inspect_options(request)
    if request.requested_profile_class_path != "":
        raise contract_error(
            "/RequestedProfileClassPath",
            "GenericRuntime.InvalidNativeProjection",
            "Inspect must not carry an exact Profile binding",
        )
    mode = options["mode"]
    if type(mode) is not str:
        raise contract_error(
            "/CanonicalDocument/mode",
            "GenericRuntime.InvalidNativeProjection",
            "Inspect mode must be an exact string",
        )
    return mode


def _inspect_class_defaults(request: RuntimeRequestDto, profile: GenericProfileCandidate, context: GenericPrimitiveContext) -> dict:
    if request.target_asset_path != request.requested_asset_class_path:
        raise contract_error(
            "/TargetAssetPath",
            "GenericRuntime.InvalidNativeProjection",
            "class-default Inspect target must equal its exact class",
        )
    return summarized(profile, request, profile.inspect(
        mode="class_defaults",
        exact_class_path=request.requested_asset_class_path,
        exact_asset_object_path=None,
        editor_asset_handle=None,
        context=context,
    ))


def _inspect_live_asset(request: RuntimeRequestDto, profile: GenericProfileCandidate, context: GenericPrimitiveContext) -> dict:
    if not is_live_inspect_path(request.target_asset_path):
        raise contract_error(
            "/TargetAssetPath",
            "GenericRuntime.InvalidNativeProjection",
            "live Inspect target must be a canonical asset object path",
        )

    def inspect() -> dict:
        if _is_behavior_tree_profile(profile):
            exists, asset_read, _ = behavior_tree_execution.resolve(context, _read_only_target(request))
        else:
            exists, asset_read, _ = _resolve_existing(context, request.target_asset_path, request.requested_asset_class_path)
        if not exists:
            raise contract_error("/Target", "GenericRuntime.TargetMissing", "Inspect could not resolve the target asset")
        return profile.inspect(
            mode="live_asset",
            exact_class_path=request.requested_asset_class_path,
            exact_asset_object_path=request.target_asset_path,
            editor_asset_handle=asset_read,
            context=context,
        )

    return summarized(profile, request, inspect_live(request.target_asset_path, inspect))


def _inspect(request: RuntimeRequestDto, profile: GenericProfileCandidate, context: GenericPrimitiveContext) -> dict:
    mode = _inspect_mode(request)
    if mode == "class_defaults":
        return _inspect_class_defaults(request, profile, context)
    if mode == "live_asset":
        return _inspect_live_asset(request, profile, context)
    raise contract_error(
        "/CanonicalDocument/mode",
        "GenericRuntime.InvalidNativeProjection",
        "Inspect mode is not supported",
    )


def _select_profile(request: RuntimeRequestDto) -> object:
    if _profile_registry is not None:
        try:
            registered = _profile_registry.resolve_exact(request.requested_asset_class_path)
        except AssetDocumentContractError as error:
            if error.diagnostic.code != "Registry.ProfileNotFound":
                raise
        else:
            return profile_factory_for(registered)()
    return GenericProfileCandidate(request.requested_asset_class_path)


def _dispatch_dependencies(request: RuntimeRequestDto, context: object) -> tuple[RuntimePhase, object, GenericPrimitiveContext]:
    phase = _validate_native_context(request, context)
    profile = _select_profile(request)
    _prepare_blackboard_relay(request, phase, profile)
    _prepare_behavior_tree_expected(request, phase, profile)
    _validate_native_catalog(context, request.expected_runtime_revision)
    return phase, profile, _BlackboardRuntimePrimitiveContext(context, phase)


def _require_verify_phase(phase: RuntimePhase, message: str) -> None:
    if phase is not RuntimePhase.VERIFY:
        raise contract_error("/Phase", "GenericRuntime.InvalidNativePhase", message)


def _inspect_result(request: RuntimeRequestDto, revision: int, phase: RuntimePhase, profile: GenericProfileCandidate, context: GenericPrimitiveContext) -> RuntimeResultDto:
    _require_verify_phase(phase, "Inspect requires the bound Verify context")
    return _evidence_result(request, revision, _inspect(request, profile, context))


def _read_only_operation_result(request: RuntimeRequestDto, revision: int, phase: RuntimePhase, profile: object, context: GenericPrimitiveContext) -> RuntimeResultDto:
    _require_verify_phase(phase, "read-only operations require the bound Verify context")
    return _read_only_result(request, revision, profile, context)


def _mutating_operation_result(request: RuntimeRequestDto, revision: int, phase: RuntimePhase, profile: object, context: GenericPrimitiveContext) -> RuntimeResultDto:
    blackboard_relay = (
        _take_blackboard_snapshot(request)
        if _is_blackboard_profile(profile) and phase is RuntimePhase.APPLY
        else None
    )
    canonical = profile.canonicalize(request.canonical_document)
    if phase is RuntimePhase.VALIDATE:
        if _is_behavior_tree_profile(profile):
            profile.validate(canonical, context=context)
        return _evidence_result(request, revision, canonical)
    if phase is RuntimePhase.PREFLIGHT and _is_blackboard_profile(profile):
        exists, _, _ = _resolve_existing(
            context,
            request.target_asset_path,
            request.requested_asset_class_path,
        )
        action = canonical["Action"]
        if (action == DocumentAction.CREATE.value and exists) or (
            action == DocumentAction.UPDATE.value and not exists
        ):
            raise contract_error(
                "/Action",
                "BlackboardData.TargetActionMismatch",
                "Action disagrees with target existence.",
            )
        snapshot = profile.preflight(
            canonical,
            context=context,
            current_snapshot=None if exists else BlackboardSnapshot(None, (), ()),
        )
        transitions = plan_blackboard_transitions(
            list(snapshot.entries), canonical["Body"]["Keys"]
        )
        canonical_bytes, semantic_hash = _evidence(canonical)
        desired_parent = canonical["Body"]["Parent"]
        projection = build_blackboard_footprint_projection(
            request_id=request.request_id,
            runtime_revision=revision,
            target_asset_path=request.target_asset_path,
            operation=request.operation.value,
            semantic_hash=semantic_hash,
            current_parent_path_or_null=snapshot.parent_path_or_null,
            desired_parent_path_or_null=None if desired_parent is None else desired_parent["Path"],
            transitions=transitions,
        )
        _store_blackboard_snapshot(request, snapshot, transitions, projection)
        return _success_result(
            request,
            revision,
            canonical_value=OptionalJsonValue(True, projection),
            canonical_bytes=canonical_bytes,
            semantic_hash=semantic_hash,
            codec_version=DocumentCodec().version,
        )
    if phase is RuntimePhase.VERIFY:
        verified = _verify(request, canonical, profile, context)
        if verified is not None:
            canonical = verified
        return _evidence_result(request, revision, canonical)
    changed_set = _phase_changed_set(
        phase,
        request,
        canonical,
        profile,
        context,
        blackboard_relay,
    )
    return _success_result(request, revision, changed_set=tuple(changed_set),
                           diagnostics=tuple(getattr(changed_set, "diagnostics", ())))


def _dispatch_typed(request: RuntimeRequestDto, context: object) -> RuntimeResultDto:
    phase, profile, primitive_context = _dispatch_dependencies(request, context)
    validate_only_result = _validate_only_profile_projection(
        request, phase, profile, context.runtime_revision
    )
    if validate_only_result is not None:
        return validate_only_result
    if request.operation is RuntimeOperation.INSPECT:
        return _inspect_result(request, context.runtime_revision, phase, profile, primitive_context)
    if request.operation in (RuntimeOperation.EXTRACT, RuntimeOperation.DIFF):
        return _read_only_operation_result(request, context.runtime_revision, phase, profile, primitive_context)
    return _mutating_operation_result(request, context.runtime_revision, phase, profile, primitive_context)


def dispatch(request, context):
    request_id = request.get("request_id", "") if type(request) is dict else ""
    revision = (
        request.get("expected_runtime_revision", 0)
        if type(request) is dict
        and type(request.get("expected_runtime_revision", 0)) is int
        else 0
    )
    try:
        typed_request = _decode_native_request(request)
        return _project_native_result(_dispatch_typed(typed_request, context))
    except AssetDocumentContractError as error:
        _clear_behavior_tree_expected()
        return _project_native_failure(request_id, revision, error.diagnostic)
    except Exception as failure:
        _clear_behavior_tree_expected()
        detail = f"{type(failure).__name__}: {failure}"[:400]
        return _project_native_failure(
            request_id,
            revision,
            RuntimeDiagnostic(
                "/GenericRuntime",
                "GenericRuntime.DispatchFailed",
                f"generic semantic dispatch failed ({detail})",
            ),
        )


def _clear_behavior_tree_expected_for_terminal(request_id, revision):
    slot = _behavior_tree_expected_slot
    if slot is not None and slot[0][:2] == (request_id, revision):
        _clear_behavior_tree_expected()


dispatch._clear_behavior_tree_expected_for_terminal = _clear_behavior_tree_expected_for_terminal


from asset_document.registered_runtime import RuntimeDispatch


def build_dispatch(registry: FrozenProfileRegistry) -> RuntimeDispatch:
    """Capture an exact registry without changing the legacy bootstrap entry."""
    from asset_document.registered_runtime import build_dispatch as compose
    return compose(registry)
