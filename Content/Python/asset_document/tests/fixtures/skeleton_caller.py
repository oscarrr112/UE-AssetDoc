# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Test-owned Skeleton caller entry. No default registration, UE wrappers, or persistence IO.

Use configure() before constructing a Host. The callable dispatch exposes the
same native registration projection getter as registered_runtime.build_dispatch.
All observations and mutations use the existing typed native context.
"""
from dataclasses import dataclass
import re

from asset_document.canonical.document_codec import DocumentCodec
from asset_document.canonical.semantic_hash import SemanticHashScope, compute_semantic_hash
from asset_document.diagnostics import AssetDocumentContractError, RuntimeDiagnostic, contract_error
from asset_document.dto import (PrimitiveRequirement, RuntimePhase, RuntimeOperation,
    OptionalJsonValue, _decode_native_request, _validate_native_context,
    _project_native_result, _project_native_failure)
from asset_document.mutation_intent_projection import build_mutation_intent_projection
from asset_document.regions.primitive_catalog import (NamedOpaqueHandleArgument,
    PrimitiveOutcomeStatus, _NativePrimitiveContext, _single_handle, _optional_handle,
    _runtime_to_native)
from asset_document.regions.property_semantics import PropertySemanticAdapter
from asset_document.runtime import _success_result

EXACT_CLASS = '/Script/Engine.AnimMontage'
SKELETON_CLASS = '/Script/Engine.Skeleton'
ENTRY_MODULE = 'asset_document/tests/fixtures/skeleton_caller.py'
SET = 'animation.asset.skeleton.set.v1'
WRITE = 'editor.property.write_admitted_batch.v1'
# The shared generic factory lifecycle; the Skeleton is the factory context.
CREATE = 'editor.asset.create_by_factory.v2'
RESOLVE = 'editor.asset.resolve_existing.v1'
CLONE = 'editor.scratch.clone_asset.v2'
SCRATCH_CREATE = 'editor.scratch.create_by_factory.v1'
ROLLBACK_CREATED = 'editor.asset.rollback_created.v1'
LIFECYCLE_ID = 'factory_generic.v1'
INSPECT = 'editor.property.inspect_evidence.v1'
_EDITOR_IDS = (RESOLVE, 'editor.class.resolve_canonical.v1',
    'editor.property.enumerate_admitted.v1', 'editor.property.enumerate_admitted_batch.v1',
    'editor.property.preflight_admitted.v1', 'editor.property.read_admitted.v1', WRITE, INSPECT)
REQUIREMENTS = tuple(PrimitiveRequirement(key, 1, 'route.generic.v1') for key in _EDITOR_IDS) + tuple(
    PrimitiveRequirement(key, 2 if key in (CLONE, CREATE) else 1, 'animation_profile') for key in
    (CLONE, SCRATCH_CREATE, CREATE, ROLLBACK_CREATED, SET))
_BY_ID = {item.primitive_id: item for item in REQUIREMENTS}


def _error(message, code='InvalidDocument'):
    return contract_error('/S1Fixture', 'S1Fixture.' + code, message)


def canonical_reference(value):
    if type(value) is not str:
        raise _error('Skeleton reference must be a string or explicit null.')
    path = value.strip()
    if path.startswith('/Script/') or not re.fullmatch(r'/[A-Za-z0-9_]+(?:/[A-Za-z0-9_]+)+(?:\.[A-Za-z0-9_]+)?', path):
        raise _error('Skeleton reference must be a long package or object path.')
    return path if '.' in path else path + '.' + path.rsplit('/', 1)[1]


def interpret(document):
    """One alias interpretation; preserve authored strings in the document."""
    if type(document) is not dict or type(document.get('Body', {})) is not dict:
        raise _error('Expected a document with object Body.')
    body = document.get('Body', {})
    if set(body) - {'Skeleton', 'References'}:
        raise _error('The Skeleton caller only owns Skeleton Body semantics.')
    refs = body.get('References', {})
    if type(refs) is not dict or set(refs) - {'Skeleton'}:
        raise _error('Skeleton caller References must be an object containing only Skeleton.')
    properties = document.get('Properties', {})
    if type(properties) is not dict or any(type(key) is not str or not key for key in properties):
        raise _error('Properties must be an object of named admitted values.')
    if 'Skeleton' not in refs and 'Skeleton' not in body:
        return properties, None
    value = refs['Skeleton'] if 'Skeleton' in refs else body['Skeleton']
    return properties, {'operation': 'clear' if value is None else 'set',
                        'skeleton_path': None if value is None else canonical_reference(value)}


def _invoke(context, primitive, values, **handles):
    result = context.invoke(requirement=_BY_ID[primitive], value_arguments=values,
        handle_arguments=tuple(NamedOpaqueHandleArgument(key, value) for key, value in handles.items()))
    if result.status is not PrimitiveOutcomeStatus.SUCCEEDED:
        raise AssetDocumentContractError(result.diagnostics[0])
    return result


def _resolve(context, target, exact_class):
    result = _invoke(context, RESOLVE, {'path': target, 'expected_class': exact_class})
    read, mutable = _optional_handle(result, 'asset_read'), _optional_handle(result, 'asset_mutable')
    if type(result.value) is not dict or type(result.value.get('exists')) is not bool:
        raise _error('Resolver omitted existence evidence.', 'InvalidOutcome')
    exists = result.value['exists']
    if (exists and (read is None or mutable is None)) or (not exists and (read is not None or mutable is not None)):
        raise _error('Resolver handles disagree with existence.', 'InvalidOutcome')
    return exists, read, mutable


def _reference(context, path):
    exists, read, _ = _resolve(context, path, SKELETON_CLASS)
    if not exists:
        raise _error('Skeleton reference does not exist.', 'ReferenceMissing')
    return read


def _step(index, primitive, intent, phase='Apply'):
    return {'Index': index, 'PrimitiveId': primitive, 'Version': _BY_ID[primitive].version, 'Phase': phase,
            'InputSchemaId': primitive + '.input.v1', 'Intent': intent}


@dataclass(frozen=True)
class _Options:
    order: str = 'properties_first'
    factory_skeleton_path: str | None = None
    fail_after: str | None = None


class SkeletonS1Dispatch:
    """Bounded exact test handler with immutable configuration and wire metadata."""
    def __init__(self, options):
        self.options = options

    def registration_projection(self):
        return {'Class': EXACT_CLASS, 'BodySections': ['Skeleton', 'References'],
                'RuntimeRegistration': {'LifecycleId': LIFECYCLE_ID,
                    'TargetPolicy': 'CreateOrUpdate', 'RequiredPrimitives': [
                        {'PrimitiveId': row.primitive_id, 'Version': row.version,
                         'RequiredByRouteId': row.required_by_route_id} for row in REQUIREMENTS]}}

    def describe_runtime_registrations(self):
        return (_runtime_to_native(self.registration_projection()),)

    def _fail(self, stage):
        if self.options.fail_after == stage:
            raise _error('Injected failure after ' + stage, 'InjectedFailure')

    def _factory_intent(self, request, skeleton):
        path = self.options.factory_skeleton_path or (skeleton or {}).get('skeleton_path')
        if path is None:
            raise _error('Create needs a non-null factory Skeleton context.', 'FactoryContextRequired')
        package, name = request.target_asset_path.rsplit('.', 1)
        return {'asset_class_path': EXACT_CLASS, 'factory_context_path': canonical_reference(path),
                'package_path': package.rsplit('/', 1)[0], 'asset_name': name}

    def _factory_handles(self, context, intent):
        cls = _invoke(context, 'editor.class.resolve_canonical.v1',
                      {'class': EXACT_CLASS, 'expected_base': None})
        return {'asset_class': _single_handle(cls, 'class'),
                'factory_context': _reference(context, intent['factory_context_path'])}

    def _plan(self, request, context, properties, skeleton):
        exists, read, mutable = _resolve(context, request.target_asset_path, EXACT_CLASS)
        action = request.canonical_document.get('Action', 'Update')
        if action not in ('Create', 'Update', 'CreateOrUpdate'):
            raise _error('Unsupported target action.')
        if (action == 'Create' and exists) or (action == 'Update' and not exists):
            raise _error('Action and target existence disagree.', 'TargetPolicyRejected')
        steps = []
        if not exists:
            steps.append(_step(0, CREATE, self._factory_intent(request, skeleton)))
        mutations = []
        if 'Properties' in request.canonical_document:
            mutations.append((WRITE, {'properties': [{'name': key, 'value': value}
                               for key, value in sorted(properties.items())]}))
        if skeleton is not None:
            mutations.append((SET, skeleton))
        if self.options.order == 'skeleton_first':
            mutations.reverse()
        for primitive, intent in mutations:
            steps.append(_step(len(steps), primitive, intent))
        return steps, exists, read, mutable

    def _preflight(self, request, context, properties, skeleton, steps, exists, read):
        if skeleton is not None and skeleton['skeleton_path'] is not None:
            source = _reference(context, skeleton['skeleton_path'])
            context.handoff(source_handle=source, target_provider_id='animation', access='read_only')
        if exists:
            scratch = _invoke(context, CLONE, {}, source_asset=read)
        else:
            scratch = _invoke(context, SCRATCH_CREATE, {}, **self._factory_handles(context, steps[0]['Intent']))
        if 'Properties' in request.canonical_document:
            adapter = PropertySemanticAdapter()
            admitted = adapter.enumerate(EXACT_CLASS, context)
            adapter.preflight(_single_handle(scratch, 'scratch'), properties, admitted, context)

    def _apply(self, request, context, properties, steps, read, mutable):
        changed = []
        for step in steps:
            primitive, intent = step['PrimitiveId'], step['Intent']
            if primitive == CREATE:
                result = _invoke(context, CREATE,
                    {key: intent[key] for key in ('package_path', 'asset_name')},
                    **self._factory_handles(context, intent))
                changed.extend(result.changed_set)
                self._fail('factory')
                if self.options.fail_after == 'created_payload':
                    # Negative native identity case: deliberately skip normal resolve.
                    mutable = _single_handle(result, 'asset_mutable')
                else:
                    exists, read, mutable = _resolve(context, request.target_asset_path, EXACT_CLASS)
                    if not exists:
                        raise _error('Factory target must resolve through ordinary Editor asset payload.', 'TargetMissing')
                    self._fail('resolve')
            elif primitive == WRITE:
                adapter = PropertySemanticAdapter()
                changed.extend(adapter.apply(read, mutable, properties,
                    adapter.enumerate(EXACT_CLASS, context), context, phase=RuntimePhase.APPLY))
                self._fail('properties')
            else:
                handles = {'asset': context.handoff(source_handle=mutable,
                           target_provider_id='animation', access='mutable')}
                if intent['operation'] == 'set':
                    handles['skeleton'] = context.handoff(
                        source_handle=_reference(context, intent['skeleton_path']),
                        target_provider_id='animation', access='read_only')
                self._fail('handoff')
                changed.extend(_invoke(context, SET, {'operation': intent['operation']}, **handles).changed_set)
                self._fail('skeleton')
        return tuple(changed)

    def _verify(self, request, context, properties, skeleton):
        exists, read, _ = _resolve(context, request.target_asset_path, EXACT_CLASS)
        if not exists:
            raise _error('Verify target is missing.', 'TargetMissing')
        result = _invoke(context, INSPECT, {'mode': 'live_asset', 'exact_asset_class_path': EXACT_CLASS}, asset=read)
        rows = result.value.get('admitted', []) if type(result.value) is dict else []
        by_name = {row['name']: row for row in rows}
        expected = dict(properties)
        if skeleton is not None:
            expected['Skeleton'] = skeleton['skeleton_path']
            if by_name.get('Skeleton', {}).get('codec_id') != 'object_path.v1':
                raise _error('Skeleton inspection codec unavailable.', 'ReadbackUnavailable')
        for name, value in expected.items():
            if name not in by_name or 'current_value' not in by_name[name] or by_name[name]['current_value'] != value:
                raise _error('Live readback differs for ' + name, 'VerificationMismatch')
        self._fail('verify')

    def __call__(self, request, native):
        request_id = request.get('request_id', '') if type(request) is dict else ''
        revision = request.get('expected_runtime_revision', 0) if type(request) is dict else 0
        try:
            typed = _decode_native_request(request)
            phase = _validate_native_context(typed, native)
            if typed.requested_asset_class_path != EXACT_CLASS:
                raise _error('Only exact AnimMontage is registered.', 'ClassMismatch')
            if typed.operation is RuntimeOperation.INSPECT:
                value = self.registration_projection()
                return self._evidence(typed, value)
            if typed.operation not in (RuntimeOperation.APPLY, RuntimeOperation.VALIDATE, RuntimeOperation.PREFLIGHT):
                raise _error('Operation outside bounded Skeleton caller.', 'OperationUnavailable')
            if canonical_reference(typed.target_asset_path) != typed.target_asset_path:
                raise _error('Request target must already be canonical.')
            properties, skeleton = interpret(typed.canonical_document)
            if typed.canonical_document.get('Class', EXACT_CLASS) != EXACT_CLASS:
                raise _error('Document class differs from exact registration.', 'ClassMismatch')
            if canonical_reference(typed.canonical_document.get('Target', typed.target_asset_path)) != typed.target_asset_path:
                raise _error('Document target differs from canonical request.', 'TargetMismatch')
            context = _NativePrimitiveContext(native, phase)
            if phase is RuntimePhase.VALIDATE:
                return self._evidence(typed, typed.canonical_document)
            if phase is RuntimePhase.VERIFY:
                self._verify(typed, context, properties, skeleton)
                return self._evidence(typed, typed.canonical_document)
            if phase in (RuntimePhase.REPAIR, RuntimePhase.PERSIST, RuntimePhase.COMPLETE):
                self._fail(phase.value.lower())
                return _project_native_result(_success_result(typed, revision))
            steps, exists, read, mutable = self._plan(typed, context, properties, skeleton)
            if phase is RuntimePhase.PREFLIGHT:
                self._preflight(typed, context, properties, skeleton, steps, exists, read)
                codec = DocumentCodec()
                digest = compute_semantic_hash(typed.canonical_document, scope=SemanticHashScope.DOCUMENT, codec=codec)
                projection = build_mutation_intent_projection(request_id=typed.request_id,
                    runtime_revision=revision, exact_asset_class_path=EXACT_CLASS,
                    target_asset_path=typed.target_asset_path, semantic_hash=digest, steps=steps)
                return self._evidence(typed, typed.canonical_document, projection)
            if phase is not RuntimePhase.APPLY:
                raise _error('Phase outside Skeleton fixture.', 'PhaseUnavailable')
            changed = self._apply(typed, context, properties, steps, read, mutable)
            return _project_native_result(_success_result(typed, revision, changed_set=changed))
        except AssetDocumentContractError as error:
            return _project_native_failure(request_id, revision, error.diagnostic)
        except Exception as error:
            return _project_native_failure(request_id, revision,
                RuntimeDiagnostic('/S1Fixture', 'S1Fixture.DispatchFailed', str(error)))

    def _evidence(self, request, document, projection=None):
        codec = DocumentCodec()
        return _project_native_result(_success_result(request, request.expected_runtime_revision,
            canonical_value=OptionalJsonValue(True, document if projection is None else projection),
            canonical_bytes=codec.encode(document), codec_version=codec.version,
            semantic_hash=compute_semantic_hash(document, scope=SemanticHashScope.DOCUMENT, codec=codec)))


def configure(*, order='properties_first', factory_skeleton_path=None, fail_after=None):
    global dispatch
    if order not in ('properties_first', 'skeleton_first'):
        raise ValueError('Invalid mutation order')
    if fail_after not in (None, 'factory', 'resolve', 'properties', 'handoff', 'skeleton', 'repair', 'verify', 'persist', 'created_payload'):
        raise ValueError('Invalid failure point')
    if factory_skeleton_path is not None:
        factory_skeleton_path = canonical_reference(factory_skeleton_path)
    dispatch = SkeletonS1Dispatch(_Options(order, factory_skeleton_path, fail_after))
    return dispatch


dispatch = SkeletonS1Dispatch(_Options())
