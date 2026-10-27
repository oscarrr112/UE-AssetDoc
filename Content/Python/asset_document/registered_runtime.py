# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Exact registered dispatch; legacy runtime remains the unbound entry path."""
from __future__ import annotations

from inspect import signature
from dataclasses import dataclass, replace
from typing import Protocol

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.diagnostics import AssetDocumentContractError, RuntimeDiagnostic, collecting_read_warnings, contract_error
from asset_document.diff import semantic_compare
from asset_document.mutation_intent_projection import (
    SNAPSHOT_COVERED, SNAPSHOT_COVERED_KIND, build_mutation_intent_projection,
)
# Module-level on purpose: the native runtime restores sys.modules between phase calls, so an in-call import would load
# a second copy of the diagnostics module whose errors the handlers below do not catch.
from asset_document.sync_evidence import dispatch_sync
from asset_document.dto import OptionalJsonValue, RuntimeOperation as Op, RuntimePhase as Phase, _decode_native_request, _project_native_result, _project_native_failure, _validate_native_context
from asset_document.dto import PrimitiveRequirement
from asset_document.profile_runtime import RuntimeTargetPolicy
from asset_document.profiles import data_table_common as common
from asset_document.read_only_extract import extract_document, inspect_live, is_live_inspect_path
from asset_document.registry import FrozenProfileRegistry
from asset_document.regions.primitive_catalog import NamedOpaqueHandleArgument, PrimitiveOutcomeStatus, _NativePrimitiveContext, _optional_handle, _single_handle, _runtime_to_native


def _error(code, message):
    return contract_error('/RuntimeRegistration', code, message)


def _profile_projection(binding):
    value = binding.handler.inspect_profile()
    if type(value) is not dict or 'RuntimeRegistration' in value:
        raise _error('GenericRuntime.TargetActivationInvalid', 'Handler projection must not supply reserved RuntimeRegistration.')
    sections = value.get('BodySections')
    if (value.get('Class') != binding.profile_meta.exact_asset_class_path
        or type(sections) is not list
        or any(type(item) is not str or not item for item in sections)
        or len(set(sections)) != len(sections)):
        raise _error('GenericRuntime.TargetActivationInvalid', 'Handler class/body projection is invalid.')
    result = DocumentCodec().decode(DocumentCodec().encode(value))
    registration = {} if binding.target_lifecycle is None else {'LifecycleId': binding.target_lifecycle.lifecycle_id}
    # Without a lifecycle the native side binds the closed existing-only shape.
    result['RuntimeRegistration'] = {
        **registration,
        'TargetPolicy': binding.target_policy.value,
        'RequiredPrimitives': [
            {'PrimitiveId': item.primitive_id, 'Version': item.version,
             'RequiredByRouteId': item.required_by_route_id}
            for item in binding.profile_meta.required_primitives
        ],
    }
    return result


class RuntimeDispatch(Protocol):
    def __call__(self, request: object, context: object) -> object: ...
    def describe_runtime_registrations(self) -> tuple[dict, ...]: ...


# Pin helpers from this revision while its import scope is active. Runtime
# imports RuntimeDispatch for its public annotation, so define that protocol
# before resolving the reciprocal module reference.
from asset_document import runtime as legacy


@dataclass(frozen=True, slots=True)
class _Dispatch:
    registry: FrozenProfileRegistry
    projection_bytes: tuple[bytes, ...]

    def describe_runtime_registrations(self):
        # Immutable owned bytes; callers receive independent native-wire copies.
        return tuple(_runtime_to_native(DocumentCodec().decode(row)) for row in self.projection_bytes)

    def __call__(self, request, context):
        request_id = request.get('request_id', '') if type(request) is dict else ''
        revision = request.get('expected_runtime_revision', 0) if type(request) is dict else 0
        if type(revision) is not int:
            revision = 0
        try:
            typed = _decode_native_request(request)
            if typed.operation is Op.SYNC:
                # Sidecar mirroring evidence: a pure function of the document text, served without a profile.
                return _project_native_result(dispatch_sync(typed, getattr(context, "runtime_revision", revision)))
            binding = self.registry.resolve_runtime_exact(typed.requested_asset_class_path)
            if binding is None:
                return legacy.dispatch(request, context)
            return _project_native_result(_dispatch_registered(typed, context, binding))
        except AssetDocumentContractError as error:
            return _project_native_failure(request_id, revision, error.diagnostic, error.additional)
        except Exception as failure:
            # Name the failure so a client (and a test log) can tell what broke.
            detail = f'{type(failure).__name__}: {failure}'[:400]
            return _project_native_failure(request_id, revision, RuntimeDiagnostic(
                '/GenericRuntime', 'GenericRuntime.DispatchFailed', f'generic semantic dispatch failed ({detail})'))


def build_dispatch(registry: FrozenProfileRegistry) -> RuntimeDispatch:
    if type(registry) is not FrozenProfileRegistry:
        raise _error('Registry.RuntimeRegistrationInvalid', 'Dispatch requires an already frozen exact registry.')
    rows = []
    for binding in registry.runtime_registrations():
        projection = _profile_projection(binding)
        rows.append(DocumentCodec().encode({key: projection[key] for key in ('Class', 'BodySections', 'RuntimeRegistration')}))
    return _Dispatch(registry, tuple(rows))


def _check_phase(operation, phase):
    if operation is Op.VALIDATE:
        # Validate in the Apply phase is the trial apply (see _trial_apply).
        valid = phase in (Phase.VALIDATE, Phase.APPLY)
    elif operation in (Op.SCHEMA, Op.TEMPLATE):
        valid = phase is Phase.VALIDATE
    elif operation in (Op.INSPECT, Op.EXTRACT, Op.DIFF, Op.REGENERATE_SIDECAR):
        valid = phase is Phase.VERIFY
    elif operation is Op.PREFLIGHT:
        valid = phase is Phase.PREFLIGHT
    else:
        valid = phase in (Phase.VALIDATE, Phase.PREFLIGHT, Phase.APPLY, Phase.REPAIR, Phase.PERSIST, Phase.VERIFY, Phase.COMPLETE)
    if not valid:
        raise contract_error('/Phase', 'GenericRuntime.InvalidNativePhase', 'Operation does not accept the bound phase.')


def _catalog(native, binding, revision):
    value = native.inspect_catalog()
    if (type(value) is not dict or value.get('runtime_revision') != revision
        or type(value.get('runtime_revision')) is not int or value.get('frozen') is not True
        or type(value.get('providers')) is not tuple or type(value.get('descriptors')) is not tuple):
        raise _error('GenericRuntime.RequiredPrimitiveUnavailable', 'Frozen runtime catalog is invalid.')
    providers = {row.get('provider_id') for row in value['providers'] if type(row) is dict and row.get('state') == 0}
    for required in binding.profile_meta.required_primitives:
        if not any(type(row) is dict and row.get('primitive_id') == required.primitive_id
                   and type(row.get('version')) is int and row['version'] == required.version
                   and row.get('provider_id') in providers
                   and type(row.get('required_consumer_ids')) is tuple
                   and required.required_by_route_id in row['required_consumer_ids'] for row in value['descriptors']):
            raise _error('GenericRuntime.RequiredPrimitiveUnavailable', 'Registered primitive closure is unavailable.')


def _invoke(context, requirement, values, handles=()):
    result = context.invoke(requirement=requirement, value_arguments=values, handle_arguments=handles)
    if result.status is not PrimitiveOutcomeStatus.SUCCEEDED:
        if result.diagnostics:
            raise AssetDocumentContractError(result.diagnostics[0])
        raise _error('GenericRuntime.InvalidNativeProjection', 'Primitive failed without diagnostic.')
    return result


def _requirement(binding, primitive_id):
    for item in binding.profile_meta.required_primitives:
        if item.primitive_id == primitive_id:
            return item
    raise _error('GenericRuntime.RequiredPrimitiveUnavailable', 'Required resolver is not in registered closure.')


def _resolve(request, binding, context):
    result = _invoke(context, _requirement(binding, 'editor.asset.resolve_existing.v1'),
                     {'path': request.target_asset_path, 'expected_class': request.requested_asset_class_path})
    if type(result.value) is not dict or type(result.value.get('exists')) is not bool:
        raise _error('GenericRuntime.InvalidNativeProjection', 'Resolver omitted existence evidence.')
    read = _optional_handle(result, 'asset_read')
    readonly_verify = context.phase is Phase.VERIFY
    mutable = (None if readonly_verify and not any(group.name == 'asset_mutable' for group in result.handle_results)
               else _optional_handle(result, 'asset_mutable'))
    exists = result.value['exists']
    if (exists and (read is None or (not readonly_verify and mutable is None))) or (not exists and (read is not None or mutable is not None)):
        raise _error('GenericRuntime.InvalidNativeProjection', 'Resolver handles disagree with existence.')
    return exists, read, mutable


def _check_action(policy, document):
    if type(policy) is not RuntimeTargetPolicy:
        raise _error('GenericRuntime.TargetPolicyInvalid', 'Unknown target policy.')
    action = document.get('Action')
    if policy is RuntimeTargetPolicy.UPDATE_ONLY and action != 'Update':
        raise _error('GenericRuntime.UpdateOnlyActionRequired', 'UpdateOnly requires Action=Update.')
    if type(action) is not str or action not in ('Create', 'Update', 'CreateOrUpdate'):
        raise _error('GenericRuntime.TargetActivationInvalid', 'A registered target requires an explicit valid Action.')
    return action


def _decision(action, exists):
    if action == 'Create' and exists:
        raise _error('GenericRuntime.CreateTargetExists', 'Create target already exists.')
    if action == 'Update' and not exists:
        raise _error('GenericRuntime.UpdateTargetMissing', 'Update target does not exist.')
    return not exists


def _context_names_match(names, lifecycle):
    declared = lifecycle.create_context_handle_names
    optional = lifecycle.optional_create_context_handle_names
    return names == tuple(name for name in declared if name in names) and all(
        name in names for name in declared if name not in optional)


def _create_handles(document, binding, context):
    if binding.target_lifecycle is None:
        raise _error('GenericRuntime.TargetLifecycleUnavailable', 'This Profile has no target lifecycle and can only update existing assets.')
    handles = binding.handler.create_target_context(document, context)
    if (type(handles) is not tuple or any(type(item) is not NamedOpaqueHandleArgument for item in handles)
        or not _context_names_match(tuple(item.name for item in handles), binding.target_lifecycle)
        or any(item.handle is None for item in handles)):
        raise _error('GenericRuntime.TargetLifecycleContextMismatch', 'Create context must exactly match registered handle names.')
    return handles


def _lifecycle(request, document, binding, context):
    action = _check_action(binding.target_policy, document)
    exists, read, mutable = _resolve(request, binding, context)
    create = _decision(action, exists)
    lifecycle = binding.target_lifecycle
    if create:
        extra = _create_handles(document, binding, context)
        class_result = _invoke(context, _requirement(binding, 'editor.class.resolve_canonical.v1'),
                               {'class': request.requested_asset_class_path, 'expected_base': None})
        handles = (NamedOpaqueHandleArgument('asset_class', _single_handle(class_result, 'class')),) + extra
    if context.phase is Phase.PREFLIGHT:
        if lifecycle is None or getattr(binding.handler, 'preflight_scratch', True) is False:
            # Update-only Profiles have no scratch primitives, and snapshot-covered
            # Profiles that never read a preview opt out: both preflight without a scratch.
            return binding.handler.preflight(document, None, context)
        result = (_invoke(context, lifecycle.create_scratch, {}, handles) if create else
                  _invoke(context, lifecycle.clone_scratch, {}, (NamedOpaqueHandleArgument('source_asset', read),)))
        return binding.handler.preflight(document, _single_handle(result, 'scratch'), context)
    if create:
        package, name = request.target_asset_path.rsplit('.', 1)
        result = _invoke(context, lifecycle.create_asset,
                         {'package_path': package.rsplit('/', 1)[0], 'asset_name': name}, handles)
        _single_handle(result, 'asset_read')
        _single_handle(result, 'asset_mutable')
        # Factory handles retain lifecycle payloads; domain handlers consume the
        # ordinary Editor Asset identity, obtained after successful creation.
        exists, read, mutable = _resolve(request, binding, context)
        if not exists:
            raise _error('GenericRuntime.CreatedTargetMissing', 'Created target must resolve before handler Apply.')
    return binding.handler.apply(document, read, mutable, context)


# Validate's trial apply: a transient copy of the target (or a new transient asset) as Asset handles.
TRIAL_TARGET = PrimitiveRequirement('editor.scratch.trial_target.v1', 1, 'route.generic.v1')


def _trial_apply(request, document, binding, context):
    """Validate of a Profile whose checks run while it edits (snapshot-covered Preflight): the
    Profile's apply() on a transient copy of the target, which the native side discards. Same
    target decision and create context as Apply; the real asset is only read."""
    handler = binding.handler
    if binding.target_lifecycle is not None and getattr(handler, 'preflight_scratch', True) is not False:
        # No trial copy: a Profile may still check an existing target read-only (validate_checks).
        checks = getattr(handler, 'validate_checks', None)
        if checks is not None:
            exists, read, _ = _resolve(request, binding, context)
            if exists and read is not None:
                checks(document, read, context)
            elif not exists and _decision(_check_action(binding.target_policy, document), exists):
                # A new target: the same read-only checks on a trial target the Profile names the create
                # context of (validate_trial_context; () = no such check, e.g. a type it has no maker for).
                trial_context = getattr(handler, 'validate_trial_context', None)
                extra = trial_context(document, context) if trial_context is not None else ()
                if extra:
                    class_result = _invoke(context, _requirement(binding, 'editor.class.resolve_canonical.v1'),
                                           {'class': request.requested_asset_class_path, 'expected_base': None})
                    handles = (NamedOpaqueHandleArgument('asset_class', _single_handle(class_result, 'class')),) + extra
                    try:
                        trial = _single_handle(_invoke(context, TRIAL_TARGET, {}, handles), 'asset_read')
                    except AssetDocumentContractError:
                        trial = None  # an early check only: Apply creates the target and reports
                    if trial is not None:
                        checks(document, trial, context)
        return ()
    action = _check_action(binding.target_policy, document)
    exists, read, _ = _resolve(request, binding, context)
    if _decision(action, exists):
        extra = _create_handles(document, binding, context)
        class_result = _invoke(context, _requirement(binding, 'editor.class.resolve_canonical.v1'),
                               {'class': request.requested_asset_class_path, 'expected_base': None})
        handles = (NamedOpaqueHandleArgument('asset_class', _single_handle(class_result, 'class')),) + extra
    else:
        handles = (NamedOpaqueHandleArgument('source_asset', read),)
    result = _invoke(context, TRIAL_TARGET, {}, handles)
    return handler.apply(document, _single_handle(result, 'asset_read'), _single_handle(result, 'asset_mutable'), context)


def _preflight_result(request, revision, canonical, steps):
    if steps is None:
        return legacy._success_result(request, revision)
    kind = {}
    if steps is SNAPSHOT_COVERED:
        steps, kind = [], {'kind': SNAPSHOT_COVERED_KIND}
    if type(steps) is not list:
        raise _error('GenericRuntime.InvalidPreflightProjection', 'Preflight must return typed steps or None.')
    evidence = legacy._evidence_result(request, revision, canonical)
    try:
        projection = build_mutation_intent_projection(
            request_id=request.request_id, runtime_revision=revision,
            exact_asset_class_path=request.requested_asset_class_path,
            target_asset_path=request.target_asset_path,
            semantic_hash=evidence.semantic_hash, steps=steps, **kind)
    except ValueError as error:
        raise _error('GenericRuntime.InvalidPreflightProjection', str(error)) from None
    # Canonical bytes/hash describe the document, never the preparation envelope.
    return replace(evidence, canonical_value=OptionalJsonValue(True, projection))


def _accepts_authored(function):
    try:
        return 'authored' in signature(function).parameters
    except (TypeError, ValueError):
        return False


def _extract(request, binding, context, include_defaults=None, live_defaults=None, authored=None):
    """Extract honours the request's diff_only option; Verify / Diff read every live value
    (include_defaults=True) so an authored value equal to its default compares as present.

    ``live_defaults`` (a dict, Verify / Diff): a handler whose read is sparse over defaults that
    depend on the asset (optional ``extract_with_defaults``) also returns those defaults, stored
    here under ``'defaults'`` for its ``diff_with_defaults``: an authored value equal to its
    default is absent from the sparse read, and only the defaults tell it from a lost value. An
    ``extract_with_defaults`` that takes ``authored`` also gets the canonical document compared against
    (to read only the defaults of the values it lists)."""
    exists, read, _ = _resolve(request, binding, context)
    if not exists:
        raise contract_error('/Target', 'GenericRuntime.TargetMissing', 'Target does not exist.')
    if include_defaults is None:
        options = request.canonical_document
        include_defaults = type(options) is dict and options.get('include_defaults') is True
    target = request.target_asset_path.rsplit('.', 1)[0]
    with_defaults = (getattr(binding.handler, 'extract_with_defaults', None)
                     if live_defaults is not None and hasattr(binding.handler, 'diff_with_defaults') else None)
    if with_defaults is not None:
        extra = {'authored': authored} if authored is not None and _accepts_authored(with_defaults) else {}

        def read_with_defaults():
            document, live_defaults['defaults'] = with_defaults(target=target, asset_handle=read, context=context, **extra)
            return document
        return extract_document(target, read_with_defaults, binding.handler.canonicalize,
                                read_only=request.operation is Op.EXTRACT)
    return extract_document(target, lambda: binding.handler.extract(
        target=target, asset_handle=read, context=context, include_defaults=include_defaults),
        binding.handler.canonicalize, read_only=request.operation is Op.EXTRACT)


def _properties_as_authored(request, binding, context, canonical, differences, exact):
    """Differences of the top-level Properties that a partial structured value explains are dropped (see
    ``data_table_common.properties_as_authored``): the Profile reads them in full, Apply writes them as an edit.
    A Profile whose Properties the generic property primitives do not read (enumerate / read refused) keeps its
    differences."""
    if not common.structured_property_differences(differences, canonical):
        return differences
    _, read, _ = _resolve(request, binding, context)
    try:
        return common.properties_as_authored(tuple(differences), canonical, read, context, exact)
    except AssetDocumentContractError:
        return differences


def _describe_value(present, value):
    if not present:
        return '(absent)'
    return semantic_compare.describe(value)


def _narrow(path, before, after):
    """The innermost place a document value and its read-back really differ (semantic_compare.first_difference:
    keyed lists by Name / Id / Key, numbers within float32, members the document omits left aside)."""
    return semantic_compare.first_difference(path, before, after)


def _describe_difference(item):
    path, before, after = getattr(item, 'path', '?'), item.before, item.after
    after_present = item.after_present
    if item.before_present and item.after_present:
        path, before, after = _narrow(path, before, after)
        if after is semantic_compare.ABSENT:
            after_present, after = False, None
    return (f"{path}: document {_describe_value(item.before_present, before)}, "
            f"read back {_describe_value(after_present, after)}")


def _describe_differences(differences, limit=4):
    """The first differences as 'path: document X, read back Y' (Difference.before is the
    document's value, .after the value read back from the asset), narrowed to where they differ."""
    parts = [_describe_difference(item)
             for item in differences[:limit] if hasattr(item, 'before_present') and hasattr(item, 'after_present')]
    more = len(differences) - limit
    return '; '.join(parts) + (f'; and {more} more' if more > 0 else '')



def _inspect_registered(request, binding, context):
    mode = legacy._inspect_mode(request)
    if mode == 'class_defaults':
        return legacy._inspect_class_defaults(request, binding.handler, context)
    if mode != 'live_asset':
        raise contract_error('/CanonicalDocument/mode', 'GenericRuntime.InvalidNativeProjection', 'Inspect mode is not supported.')
    if not is_live_inspect_path(request.target_asset_path):
        raise contract_error('/TargetAssetPath', 'GenericRuntime.InvalidNativeProjection', 'live Inspect target must be a canonical asset object path.')

    def inspect():
        exists, read, _ = _resolve(request, binding, context)
        if not exists:
            raise contract_error('/Target', 'GenericRuntime.TargetMissing', 'Inspect could not resolve the target asset.')
        return binding.handler.inspect(mode='live_asset', exact_class_path=request.requested_asset_class_path,
                                       exact_asset_object_path=request.target_asset_path,
                                       editor_asset_handle=read, context=context)

    return legacy.summarized(binding.handler, request, inspect_live(request.target_asset_path, inspect))


def _dispatch_registered(request, native, binding):
    phase = _validate_native_context(request, native)
    _check_phase(request.operation, phase)
    revision = request.expected_runtime_revision
    if phase in (Phase.PERSIST, Phase.COMPLETE):
        return legacy._success_result(request, revision)
    _catalog(native, binding, revision)
    context = _NativePrimitiveContext(native, phase)
    handler = binding.handler
    if request.operation is Op.SCHEMA:
        return legacy._evidence_result(request, revision, _profile_projection(binding))
    if request.operation is Op.TEMPLATE:
        return legacy._evidence_result(request, revision, handler.create_template(legacy._read_only_target(request)))
    if request.operation is Op.INSPECT:
        with collecting_read_warnings() as warnings:
            inspected = _inspect_registered(request, binding, context)
        return legacy._evidence_result(request, revision, inspected, tuple(warnings))
    if request.operation in (Op.EXTRACT, Op.REGENERATE_SIDECAR):
        with collecting_read_warnings() as warnings:
            extracted = _extract(request, binding, context)
        return legacy._evidence_result(request, revision, extracted, tuple(warnings))
    canonical = handler.canonicalize(request.canonical_document)
    if phase is Phase.VALIDATE:
        return legacy._evidence_result(request, revision, canonical)
    if request.operation is Op.VALIDATE:
        changes = _trial_apply(request, canonical, binding, context)
        return legacy._success_result(request, revision, changed_set=tuple(changes),
                                      diagnostics=tuple(getattr(changes, 'diagnostics', ())))
    if phase is Phase.VERIFY:
        live_defaults = {}
        try:
            actual = _extract(request, binding, context, include_defaults=True, live_defaults=live_defaults,
                              authored=canonical)
        except AssetDocumentContractError as error:
            if request.operation is not Op.DIFF or error.diagnostic.code not in legacy._TARGET_MISSING_CODES:
                raise
            return legacy._evidence_result(request, revision, legacy._would_create_diff(canonical))
        # The Diff operation may ask a handler for its exact comparison (an empty
        # Diff then proves an apply would change nothing); verification stays lenient.
        exact = getattr(handler, 'diff_exact', None) if request.operation is Op.DIFF else None
        if 'defaults' in live_defaults:
            differences = handler.diff_with_defaults(canonical, actual, live_defaults['defaults'], exact is not None)
        else:
            differences = (exact or handler.diff)(canonical, actual)
        # Notes of the comparison (e.g. an omitted field the profile keeps): Info diagnostics of the Diff result.
        notes = tuple(getattr(differences, 'diagnostics', ()))
        differences = _properties_as_authored(request, binding, context, canonical, differences, exact is not None)
        refine =getattr(handler, 'refine_diff', None) if request.operation is Op.DIFF else None
        if refine is not None and differences:
            # The Profile may read the asset again for the current value of a difference.
            _, read, _ = _resolve(request, binding, context)
            differences = refine(tuple(differences), canonical, actual, read, context)
        if request.operation is Op.DIFF:
            return legacy._evidence_result(request, revision, {'changed': [legacy._difference_projection(item) for item in differences], 'unchanged': [], 'skipped': [], 'failed': []}, notes)
        if differences:
            paths = ', '.join(str(getattr(item, 'path', '?')) for item in differences[:8])
            described = _describe_differences(differences)
            raise contract_error('/', 'GenericRuntime.VerificationMismatch',
                                 f'Registered handler reported semantic differences at {paths}.'
                                 + (f' {described}.' if described else ''))
        return legacy._evidence_result(request, revision, canonical)
    if phase is Phase.PREFLIGHT:
        return _preflight_result(request, revision, canonical,
                                 _lifecycle(request, canonical, binding, context))
    if phase is Phase.REPAIR:
        exists, _, mutable = _resolve(request, binding, context)
        if not exists:
            raise contract_error('/Target', 'GenericRuntime.TargetMissing', 'Repair requires an existing target.')
        changes = handler.repair(canonical, mutable, context)
    else:
        changes = _lifecycle(request, canonical, binding, context)
    diagnostics = getattr(changes, 'diagnostics', ())
    return legacy._success_result(request, revision, changed_set=tuple(changes), diagnostics=tuple(diagnostics))
