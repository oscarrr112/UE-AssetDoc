# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

"""Pure transport/semantic tests; native Skeleton tests own UE behavior evidence."""
import copy
import unittest

from asset_document.tests.fixtures import skeleton_caller as caller
from asset_document.tests.test_generic_runtime import request, outcome, handle_group, diagnostic, REQUEST_ID

TARGET = '/Game/S1/M.M'
REFERENCE = '/Game/S1/S.S'


def document(body=None, action='Update', properties=None):
    result = {'SchemaVersion': 1, 'AssetType': 'AnimMontage', 'Class': caller.EXACT_CLASS,
              'Target': '/Game/S1/M', 'Action': action, 'Body': {} if body is None else body}
    if properties is not None:
        result['Properties'] = properties
    return result


def native_request(doc):
    return request(doc, target_asset_path=TARGET, requested_asset_class_path=caller.EXACT_CLASS,
                   requested_profile_class_path=caller.EXACT_CLASS, expected_runtime_revision=61)


class Native:
    """Only scripted typed transport replies; does not emulate UE state."""
    request_id = REQUEST_ID
    runtime_revision = 61

    def __init__(self, phase, exists=True):
        self.phase = phase
        self.exists = exists
        self.calls = []
        self.target_read, self.target_mutable, self.reference = object(), object(), object()
        self.created_read, self.created_mutable = object(), object()
        self.property = object()
        self.observed = REFERENCE
        self.reject_handoff = False

    def handoff(self, source, provider, access):
        self.calls.append(('handoff', source, provider, access))
        if self.reject_handoff:
            raise RuntimeError('AD.RUNTIME.HANDOFF_TRANSACTION_REQUIRED: active transaction required')
        return source

    def invoke(self, primitive, version, values, handles):
        self.calls.append((primitive, values, handles))
        if primitive == caller.RESOLVE:
            exists = self.exists if values['path'] == TARGET else True
            read = self.target_read if values['path'] == TARGET else self.reference
            return outcome(value={'exists': exists}, handle_results=(
                handle_group('asset_read', *((read,) if exists else ())),
                handle_group('asset_mutable', *((self.target_mutable,) if exists else ()))))
        if primitive in (caller.CLONE, caller.SCRATCH_CREATE):
            return outcome(handle_results=(handle_group('scratch', object()),))
        if primitive == 'editor.class.resolve_canonical.v1':
            return outcome(handle_results=(handle_group('class', object()),))
        if primitive == caller.CREATE:
            self.exists = True
            return outcome(handle_results=(handle_group('asset_read', self.created_read),
                                           handle_group('asset_mutable', self.created_mutable)))
        if primitive.startswith('editor.property.enumerate_admitted'):
            row = {'name': 'RateScale', 'declaring_class': caller.EXACT_CLASS, 'codec_id': 'float.v1',
                   'readable': True, 'writable': True}
            groups = (handle_group('properties', self.property),)
            if 'batch' in primitive:
                groups += (handle_group('property_batch', object()),)
            return outcome(value={'properties': (row,)}, handle_results=groups)
        if primitive == 'editor.property.preflight_admitted.v1':
            return outcome(value={'canonical_value': values['value'], 'feasible': True})
        if primitive == caller.WRITE:
            return outcome()
        if primitive == caller.SET:
            return outcome(value={'skeleton_path': REFERENCE, 'skeleton_guid': 'scripted', 'changed': True})
        if primitive == caller.INSPECT:
            return outcome(value={'admitted': (
                {'name': 'Skeleton', 'codec_id': 'object_path.v1', 'current_value': self.observed},
                {'name': 'RateScale', 'codec_id': 'float.v1', 'current_value': 2.0}),
                'rejected': (), 'subject': {}})
        raise AssertionError(primitive)


class SkeletonS1CallerTests(unittest.TestCase):
    def setUp(self):
        self.dispatch = caller.configure()

    def assert_success(self, result):
        self.assertEqual(result['terminal_status'], 0, result)

    def test_alias_and_resolution_preserve_authored(self):
        doc = document({'Skeleton': '/Game/Old/S', 'References': {'Skeleton': ' /Game/S1/S '}})
        original = copy.deepcopy(doc)
        properties, intent = caller.interpret(doc)
        self.assertEqual(intent, {'operation': 'set', 'skeleton_path': REFERENCE})
        self.assertEqual(doc, original)
        self.assertEqual(properties, {})
        self.assertEqual(caller.interpret(document({'Skeleton': REFERENCE, 'References': {'Skeleton': None}}))[1],
                         {'operation': 'clear', 'skeleton_path': None})

    def test_missing_is_not_clear(self):
        for body in ({}, {'References': {}}):
            result = self.dispatch(native_request(document(body)), Native(1))
            self.assert_success(result)
            self.assertEqual(result['canonical_value']['Steps'], ())

    def test_invalid_references_rejected_before_resolver(self):
        for path in ('S', "Skeleton'/Game/S.S'", '/Game/S.S:Sub', '/Game//S', '/Script/Engine.Skeleton', 4, ''):
            native = Native(1)
            result = self.dispatch(native_request(document({'Skeleton': path})), native)
            self.assertEqual(result['terminal_status'], 1)
            self.assertEqual(native.calls, [])

    def test_preflight_projection_and_no_mutator(self):
        native = Native(1)
        result = self.dispatch(native_request(document({'Skeleton': REFERENCE}, properties={'RateScale': 2.0})), native)
        self.assert_success(result)
        projection = result['canonical_value']
        self.assertEqual([row['PrimitiveId'] for row in projection['Steps']], [caller.WRITE, caller.SET])
        self.assertEqual(projection['Steps'][1]['Intent'], {'operation': 'set', 'skeleton_path': REFERENCE})
        self.assertEqual(projection['SemanticHash'], result['semantic_hash'])
        self.assertFalse(any(row[0] in (caller.WRITE, caller.SET) for row in native.calls))

    def test_live_order_and_value_handle_separation(self):
        for order, expected in (('properties_first', [caller.WRITE, caller.SET]),
                                ('skeleton_first', [caller.SET, caller.WRITE])):
            native = Native(2)
            result = caller.configure(order=order)(native_request(document({'Skeleton': REFERENCE}, properties={'RateScale': 2.0})), native)
            self.assert_success(result)
            calls = [row for row in native.calls if row[0] in (caller.WRITE, caller.SET)]
            self.assertEqual([row[0] for row in calls], expected)
            skeleton = next(row for row in calls if row[0] == caller.SET)
            self.assertEqual(skeleton[1], {'operation': 'set'})
            self.assertEqual([arg['name'] for arg in skeleton[2]], ['asset', 'skeleton'])

    def test_factory_reacquires_ordinary_target(self):
        native = Native(2, exists=False)
        result = self.dispatch(native_request(document({'Skeleton': REFERENCE}, action='Create')), native)
        self.assert_success(result)
        ids = [row[0] for row in native.calls]
        factory = ids.index(caller.CREATE)
        resolve = ids.index(caller.RESOLVE, factory + 1)
        setter = ids.index(caller.SET)
        self.assertLess(factory, resolve)
        self.assertLess(resolve, setter)
        self.assertIs(native.calls[setter][2][0]['handle'], native.target_mutable)
        self.assertIsNot(native.calls[setter][2][0]['handle'], native.created_mutable)

    def test_create_clear_needs_separate_factory_context(self):
        doc = native_request(document({'Skeleton': None}, action='Create'))
        self.assertEqual(self.dispatch(doc, Native(1, False))['terminal_status'], 1)
        result = caller.configure(factory_skeleton_path=REFERENCE)(doc, Native(1, False))
        self.assert_success(result)
        steps = result['canonical_value']['Steps']
        self.assertEqual([row['PrimitiveId'] for row in steps], [caller.CREATE, caller.SET])
        self.assertEqual(steps[1]['Intent']['operation'], 'clear')

    def test_handoff_diagnostic_preserved_no_setter(self):
        native = Native(2)
        native.reject_handoff = True
        result = self.dispatch(native_request(document({'Skeleton': REFERENCE})), native)
        self.assertEqual(result['diagnostics'][0]['code'], 'AD.RUNTIME.HANDOFF_TRANSACTION_REQUIRED')
        self.assertFalse(any(row[0] == caller.SET for row in native.calls))

    def test_failure_stops_later_steps(self):
        native = Native(2)
        result = caller.configure(fail_after='properties')(native_request(
            document({'Skeleton': REFERENCE}, properties={'RateScale': 2.0})), native)
        self.assertEqual(result['diagnostics'][0]['code'], 'S1Fixture.InjectedFailure')
        self.assertTrue(any(row[0] == caller.WRITE for row in native.calls))
        self.assertFalse(any(row[0] == caller.SET for row in native.calls))

    def test_verify_reads_native_evidence_and_rejects_mismatch(self):
        doc = native_request(document({'Skeleton': ' /Game/S1/S '}, properties={'RateScale': 2.0}))
        native = Native(5)
        self.assert_success(self.dispatch(doc, native))
        self.assertTrue(any(row[0] == caller.INSPECT for row in native.calls))
        native.observed = '/Game/Other/S.S'
        result = self.dispatch(doc, native)
        self.assertEqual(result['diagnostics'][0]['code'], 'S1Fixture.VerificationMismatch')

    def test_registration_isolated_copy(self):
        projection = self.dispatch.describe_runtime_registrations()[0]
        projection['RuntimeRegistration']['LifecycleId'] = 'wrong'
        self.assertEqual(self.dispatch.describe_runtime_registrations()[0]['RuntimeRegistration']['LifecycleId'],
                         'factory_generic.v1')


if __name__ == '__main__':
    unittest.main()
