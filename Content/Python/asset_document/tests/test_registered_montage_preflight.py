"""Class-neutral typed Preflight transport used by the production Montage handler."""
import unittest
from copy import deepcopy
import sys
from types import ModuleType
from unittest.mock import patch

import asset_document

from asset_document.registered_runtime import build_dispatch
from asset_document.tests.runtime_registration_fixtures import Context, Handler, registry
from asset_document.tests.test_generic_runtime import document, request, NativeHandle, outcome, handle_group


def steps(value):
    return [{'Index': 0, 'PrimitiveId': 'test.replace.v1', 'Version': 1,
             'Phase': 'Apply', 'InputSchemaId': 'test.replace.v1.input.v1',
             'Intent': {'values': [value]}}]


class TypedHandler(Handler):
    def preflight(self, document, scratch_handle, context):
        return steps(document['Properties']['Value'])


class RegisteredTypedPreflightTests(unittest.TestCase):
    def test_create_resolves_ordinary_handles_before_domain_apply(self):
        ordinary_read, ordinary_mutable = NativeHandle(), NativeHandle()
        class CreatedContext(Context):
            def invoke(self, primitive_id, version, value_arguments, handle_arguments):
                result = super().invoke(primitive_id, version, value_arguments, handle_arguments)
                if primitive_id == 'editor.asset.resolve_existing.v1' and self.exists:
                    return outcome(value={'exists': True}, handle_results=(
                        handle_group('asset_read', ordinary_read), handle_group('asset_mutable', ordinary_mutable)))
                return result
        class DomainHandler(Handler):
            def apply(self, document, read_asset_handle, mutable_asset_handle, context):
                assert read_asset_handle is ordinary_read
                assert mutable_asset_handle is ordinary_mutable
                return ()
        context = CreatedContext(2, exists=False)
        result = self.dispatch_for(DomainHandler())(request(), context)
        self.assertEqual(result['terminal_status'], 0, result)
        self.assertEqual([call[0] for call in context.calls], [
            'editor.asset.resolve_existing.v1', 'editor.class.resolve_canonical.v1',
            'test.asset.create.v1', 'editor.asset.resolve_existing.v1'])

    def test_create_without_resolvable_target_never_calls_domain_apply(self):
        class Missing(Context):
            def invoke(self, primitive_id, version, value_arguments, handle_arguments):
                result = super().invoke(primitive_id, version, value_arguments, handle_arguments)
                if primitive_id == 'test.asset.create.v1':
                    self.exists = False
                return result
        handler = Handler()
        result = self.dispatch_for(handler)(request(), Missing(2, exists=False))
        self.assertEqual(result['diagnostics'][0]['code'], 'GenericRuntime.CreatedTargetMissing')
        self.assertNotIn('apply', handler.calls)

    def test_factory_handle_validation_is_not_bypassed_by_reresolve(self):
        class BrokenFactory(Context):
            def invoke(self, primitive_id, version, value_arguments, handle_arguments):
                result = super().invoke(primitive_id, version, value_arguments, handle_arguments)
                return outcome() if primitive_id == 'test.asset.create.v1' else result
        context = BrokenFactory(2, exists=False)
        handler = Handler()
        result = self.dispatch_for(handler)(request(), context)
        self.assertNotEqual(result['terminal_status'], 0)
        self.assertNotIn('apply', handler.calls)
        self.assertEqual([call[0] for call in context.calls].count('editor.asset.resolve_existing.v1'), 1)

    def test_frozen_dispatch_retains_revision_runtime_after_ambient_replacement(self):
        dispatch = self.dispatch_for(TypedHandler())
        req = request()
        expected = [dispatch(req, Context(phase)) for phase in (0, 1, 2, 5)]
        foreign = ModuleType('asset_document.runtime')
        # Any ambient helper lookup is a regression, regardless of whether its
        # DTO happens to look like the frozen revision's exact Python type.
        def unexpected(name):
            raise AssertionError('frozen dispatch used ambient runtime: ' + name)
        foreign.__getattr__ = unexpected
        with patch.object(asset_document, 'runtime', foreign), patch.dict(
                sys.modules, {'asset_document.runtime': foreign}):
            actual = [dispatch(req, Context(phase)) for phase in (0, 1, 2, 5)]
        self.assertEqual(actual, expected)
        self.assertEqual(actual[1]['canonical_value']['Kind'], 'MutationIntentProjectionV1')

    def test_verify_accepts_real_readonly_staged_handle_but_apply_requires_mutable(self):
        class Readonly(Context):
            def invoke(self, primitive_id, version, value_arguments, handle_arguments):
                result = super().invoke(primitive_id, version, value_arguments, handle_arguments)
                if primitive_id == 'editor.asset.resolve_existing.v1':
                    result['handle_results'] = tuple(group for group in result['handle_results'] if group['name'] == 'asset_read')
                return result
        dispatch = self.dispatch_for(Handler())
        verified = dispatch(request(), Readonly(5))
        self.assertEqual(verified['terminal_status'], 0)
        failed = dispatch(request(), Readonly(2))
        self.assertNotEqual(failed['terminal_status'], 0)
        self.assertEqual(failed['diagnostics'][0]['code'], 'GenericRuntime.InvalidNativeProjection')

    def dispatch_for(self, handler):
        frozen, _ = registry(handler)
        return build_dispatch(frozen)

    def test_typed_projection_keeps_canonical_evidence_and_request_identity(self):
        dispatch = self.dispatch_for(TypedHandler())
        for exists in (True, False):
            with self.subTest(exists=exists):
                req = request()
                validated = dispatch(req, Context(0, exists=exists))
                result = dispatch(req, Context(1, exists=exists))
                self.assertEqual(result['terminal_status'], validated['terminal_status'])
                projection = result['canonical_value']
                self.assertEqual(projection['Kind'], 'MutationIntentProjectionV1')
                self.assertEqual(projection['RequestId'], req['request_id'])
                self.assertEqual(projection['RuntimeRevision'], req['expected_runtime_revision'])
                self.assertEqual(projection['TargetAssetPath'], req['target_asset_path'])
                self.assertEqual(projection['ExactAssetClassPath'], req['requested_asset_class_path'])
                self.assertEqual(projection['SemanticHash'], validated['semantic_hash'])
                for key in ('canonical_bytes', 'semantic_hash', 'codec_version'):
                    self.assertEqual(result[key], validated[key])
                self.assertEqual(projection['Steps'][0]['Intent']['values'], (7,))

    def test_none_preserves_legacy_no_evidence_and_empty_list_is_explicit_projection(self):
        legacy = self.dispatch_for(Handler())(request(), Context(1))
        self.assertIsNone(legacy['canonical_value'])
        self.assertEqual(legacy['semantic_hash'], '')
        class Empty(Handler):
            def preflight(self, document, scratch_handle, context): return []
        result = self.dispatch_for(Empty())(request(), Context(1))
        self.assertEqual(result['canonical_value']['Steps'], ())
        self.assertTrue(result['semantic_hash'])

    def test_reused_handler_does_not_reuse_request_projection(self):
        dispatch = self.dispatch_for(TypedHandler())
        first = dispatch(request(), Context(1))
        changed = document()
        changed['Properties']['Value'] = 9
        second_id = '12345678-1234-4234-9234-123456789abc'
        context = Context(1)
        context.request_id = second_id
        second = dispatch(request(changed, request_id=second_id), context)
        self.assertEqual(first['canonical_value']['Steps'][0]['Intent']['values'], (7,))
        self.assertEqual(second['canonical_value']['Steps'][0]['Intent']['values'], (9,))
        self.assertEqual(second['canonical_value']['RequestId'], second_id)
        self.assertNotEqual(first['semantic_hash'], second['semantic_hash'])

    def test_invalid_return_and_forbidden_or_malformed_steps_fail_closed(self):
        bad_steps = steps(7)
        bad_steps[0]['Intent']['Handles'] = []
        discontinuous = steps(7)
        discontinuous[0]['Index'] = 1
        for value in ({}, (), bad_steps, discontinuous):
            with self.subTest(value=value):
                class Invalid(Handler):
                    def preflight(self, document, scratch_handle, context):
                        return deepcopy(value)
                result = self.dispatch_for(Invalid())(request(), Context(1))
                self.assertEqual(result['diagnostics'][0]['code'], 'GenericRuntime.InvalidPreflightProjection')


if __name__ == '__main__':
    unittest.main()
