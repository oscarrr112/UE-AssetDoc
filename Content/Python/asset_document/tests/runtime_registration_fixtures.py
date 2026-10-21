"""Test-only complete handler and native lifecycle fixtures."""
from copy import deepcopy
from asset_document.dto import PrimitiveRequirement
from asset_document.meta import ProfileMeta
from asset_document.profile_runtime import RuntimeProfileRegistration, RuntimeTargetLifecycleSpec, RuntimeTargetPolicy
from asset_document.registry import ProfileRegistryBuilder
from asset_document.regions.target_lifecycle import generic_factory_lifecycle, lifecycle_requirements
from asset_document.tests.test_generic_runtime import NativeContext, NativeHandle, EXACT_CLASS, document, outcome, handle_group

ROUTE = 'route.registered.test.v1'
REQUIREMENTS = tuple(PrimitiveRequirement(name, 1, ROUTE) for name in (
    'editor.class.resolve_canonical.v1', 'editor.asset.resolve_existing.v1',
    'test.scratch.clone.v1', 'test.scratch.create.v1', 'test.asset.create.v1', 'test.asset.rollback.v1'))
META = ProfileMeta('profile.registered.test.v1', ROUTE, EXACT_CLASS, (), REQUIREMENTS)
LIFECYCLE = RuntimeTargetLifecycleSpec('test.lifecycle.v1', 'test.scratch.v1', 'test.factory.v1', *REQUIREMENTS[2:], ())

class Handler:
    profile_id = META.profile_id
    exact_asset_class_path = EXACT_CLASS
    runtime_entry_module = 'asset_document/bootstrap.py'
    def __init__(self):
        self.calls = []
        self.projection = {'Class': EXACT_CLASS, 'BodySections': []}
        self.differences = ()
        self.handles = ()
    def required_primitives(self): return REQUIREMENTS
    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        self.calls.append('inspect')
        return {'mode': mode}
    def inspect_profile(self):
        self.calls.append('inspect_profile')
        return self.projection
    def create_template(self, target):
        self.calls.append('template')
        return {'Target': target}
    def validate(self, document): self.calls.append('validate')
    def canonicalize(self, document):
        self.calls.append('canonicalize')
        return deepcopy(document)
    def create_target_context(self, document, context):
        self.calls.append('create_context')
        return self.handles
    def preflight(self, document, scratch_handle, context): self.calls.append('preflight')
    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        self.calls.append('apply')
        return ()
    def repair(self, document, asset_handle, context):
        self.calls.append('repair')
        return ()
    def extract(self, *, target, asset_handle, context, include_defaults=False):
        self.calls.append('extract')
        return document()
    def diff(self, authored, extracted):
        self.calls.append('diff')
        return self.differences


def registry(handler=None, policy=RuntimeTargetPolicy.CREATE_OR_UPDATE, lifecycle=LIFECYCLE, meta=META):
    handler = Handler() if handler is None else handler
    builder = ProfileRegistryBuilder()
    builder.register_runtime(RuntimeProfileRegistration(meta, lambda: handler, policy, lifecycle))
    return builder.freeze(), handler


# Update-only Profiles declare no lifecycle primitives at all.
UPDATE_ONLY_REQUIREMENTS = REQUIREMENTS[:2]
UPDATE_ONLY_META = ProfileMeta(META.profile_id, ROUTE, EXACT_CLASS, (), UPDATE_ONLY_REQUIREMENTS)

# A Profile assembled from the reusable generic factory lifecycle component.
GENERIC_LIFECYCLE = generic_factory_lifecycle(ROUTE)
GENERIC_REQUIREMENTS = UPDATE_ONLY_REQUIREMENTS + lifecycle_requirements(GENERIC_LIFECYCLE)
GENERIC_META = ProfileMeta(META.profile_id, ROUTE, EXACT_CLASS, (), GENERIC_REQUIREMENTS)


class UpdateOnlyHandler(Handler):
    def required_primitives(self): return UPDATE_ONLY_REQUIREMENTS


class GenericHandler(Handler):
    def required_primitives(self): return GENERIC_REQUIREMENTS


class Context(NativeContext):
    def __init__(self, phase, exists=True, requirements=REQUIREMENTS):
        super().__init__(phase, required_ids=tuple(item.primitive_id for item in requirements))
        self._versions = {item.primitive_id: item.version for item in requirements}
        self.exists = exists
    def inspect_catalog(self):
        value = super().inspect_catalog()
        for row in value['descriptors']:
            row['required_consumer_ids'] = (ROUTE,)
            row['version'] = self._versions[row['primitive_id']]
        return value
    def invoke(self, primitive_id, version, value_arguments, handle_arguments):
        self.calls.append((primitive_id, version, value_arguments, handle_arguments))
        if primitive_id == 'editor.asset.resolve_existing.v1':
            handles = (NativeHandle(),) if self.exists else ()
            return outcome(value={'exists': self.exists}, handle_results=(handle_group('asset_read', *handles), handle_group('asset_mutable', *handles)))
        if primitive_id == 'editor.class.resolve_canonical.v1':
            return outcome(handle_results=(handle_group('class', NativeHandle()),))
        if primitive_id in ('test.scratch.clone.v1', 'test.scratch.create.v1',
                            'editor.scratch.clone_asset.v2', 'editor.scratch.create_by_factory.v1'):
            return outcome(handle_results=(handle_group('scratch', NativeHandle()),))
        if primitive_id == 'editor.scratch.trial_target.v1':
            return outcome(handle_results=(handle_group('scratch', NativeHandle()), handle_group('asset_read', NativeHandle()),
                                           handle_group('asset_mutable', NativeHandle())))
        if primitive_id in ('test.asset.create.v1', 'editor.asset.create_by_factory.v2'):
            self.exists = True
            return outcome(handle_results=(handle_group('asset_read', NativeHandle()), handle_group('asset_mutable', NativeHandle())))
        raise AssertionError(primitive_id)
