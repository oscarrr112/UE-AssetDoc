from dataclasses import FrozenInstanceError, replace
from enum import Enum
import unittest

from asset_document.diagnostics import (
    AssetDocumentContractError,
    DiagnosticContext,
    DiagnosticSeverity,
    RuntimeDiagnostic,
)
from asset_document.dto import (
    RUNTIME_CONTRACT_VERSION,
    CancellationEvidence,
    ChangedRecord,
    OptionalJsonValue,
    PrimitiveRequirement,
    RuntimeOperation,
    RuntimePhase,
    RuntimeRequestDto,
    RuntimeResultDto,
    TerminalStatus,
)
from asset_document.meta import (
    ComparisonPolicy,
    DefaultPolicy,
    DeletePolicy,
    DocumentAction,
    DocumentMeta,
    IdentityPolicy,
    OmissionPolicy,
    OrderPolicy,
    ProfileMeta,
    RegionMeta,
    RegionOwnership,
)
from asset_document.registry import FrozenProfileRegistry, ProfileRegistryBuilder
from asset_document.profile_runtime import (
    RuntimeTargetPolicy, RuntimeTargetLifecycleSpec, RuntimeProfileRegistration,
    FrozenRuntimeProfileRegistration, RuntimeProfileHandler,
)
from asset_document.profiles.blackboard_data import (
    BLACKBOARD_DATA_CLASS,
    BLACKBOARD_DATA_PROFILE_META,
    BLACKBOARD_REQUIRED_PRIMITIVES,
    create_blackboard_data_profile,
)
from asset_document.runtime import build_profile_registry, profile_factory_for
from asset_document.bootstrap import PROFILE_REGISTRY


REQUEST_ID = "00000000-0000-0000-0000-000000000001"


def assert_contract_error(test: unittest.TestCase, code: str, path: str, callback) -> None:
    with test.assertRaises(AssetDocumentContractError) as caught:
        callback()
    test.assertEqual((caught.exception.diagnostic.code, caught.exception.diagnostic.path), (code, path))


def make_region(
    region_id: str = "Body",
    body_path: str = "/Body",
    **overrides,
) -> RegionMeta:
    values = {
        "region_id": region_id,
        "body_path": body_path,
        "ownership": RegionOwnership.MANAGED,
        "identity": IdentityPolicy.STABLE_ID,
        "order": OrderPolicy.IDENTITY_KEYED,
        "default": DefaultPolicy.EXPLICIT,
        "omission": OmissionPolicy.OMIT_DEFAULT,
        "deletion": DeletePolicy.EXPLICIT_ONLY,
        "comparison": ComparisonPolicy.CANONICAL,
    }
    values.update(overrides)
    return RegionMeta(**values)


def make_profile(
    profile_id: str = "profile.data_asset.v1",
    route_id: str = "route.data_asset.v1",
    exact_class: str = "/Script/Engine.DataAsset",
    regions: tuple[RegionMeta, ...] | None = None,
    requirements: tuple[PrimitiveRequirement, ...] | None = None,
) -> ProfileMeta:
    if regions is None:
        regions = (make_region(),)
    if requirements is None:
        requirements = (
            PrimitiveRequirement("editor.asset.resolve_existing.v1", 1, route_id),
        )
    return ProfileMeta(profile_id, route_id, exact_class, regions, requirements)


def make_request(**overrides) -> RuntimeRequestDto:
    values = {
        "request_id": REQUEST_ID,
        "operation": RuntimeOperation.VALIDATE,
        "target_asset_path": "/Game/Test/Asset.Asset",
        "requested_profile_class_path": "/Script/AssetDocument.AssetDocumentProfile",
        "requested_asset_class_path": "/Script/Engine.DataAsset",
        "canonical_document": {"Body": []},
    }
    values.update(overrides)
    return RuntimeRequestDto(**values)


def make_result(**overrides) -> RuntimeResultDto:
    values = {
        "request_id": REQUEST_ID,
        "terminal_status": TerminalStatus.SUCCEEDED,
        "final_phase": RuntimePhase.COMPLETE,
        "runtime_revision": 1,
    }
    values.update(overrides)
    return RuntimeResultDto(**values)


class DtoDiagnosticContractTests(unittest.TestCase):
    def test_versioned_dto_and_primitive_requirement(self) -> None:
        requirement = PrimitiveRequirement(
            "editor.asset.resolve_existing.v1", 1, "route.generic.v1"
        )
        request = RuntimeRequestDto(
            request_id=REQUEST_ID,
            operation=RuntimeOperation.VALIDATE,
            target_asset_path="/Game/Test/Asset.Asset",
            requested_profile_class_path="/Script/AssetDocument.AssetDocumentProfile",
            requested_asset_class_path="/Script/Engine.DataAsset",
            canonical_document={"SchemaVersion": 1},
        )
        result = RuntimeResultDto(
            request_id=REQUEST_ID,
            terminal_status=TerminalStatus.SUCCEEDED,
            final_phase=RuntimePhase.COMPLETE,
            runtime_revision=1,
        )
        self.assertEqual(
            (requirement.version, request.abi_version, result.abi_version),
            (1, RUNTIME_CONTRACT_VERSION, RUNTIME_CONTRACT_VERSION),
        )
        with self.assertRaises(FrozenInstanceError):
            request.request_id = "mutated"
        for factory in (
            lambda: RuntimeRequestDto(
                REQUEST_ID,
                RuntimeOperation.VALIDATE,
                "/Game/Test/Asset.Asset",
                "/Script/AssetDocument.AssetDocumentProfile",
                "/Script/Engine.DataAsset",
                {},
                abi_version=2,
            ),
            lambda: RuntimeResultDto(
                REQUEST_ID,
                TerminalStatus.SUCCEEDED,
                RuntimePhase.COMPLETE,
                1,
                abi_version=2,
            ),
        ):
            assert_contract_error(self, "DTO.AbiVersionMismatch", "/AbiVersion", factory)

        self.assertEqual(CancellationEvidence(1, True).sequence, 1)
        for evidence in (
            lambda: CancellationEvidence(-1, False),
            lambda: CancellationEvidence(2**64, False),
            lambda: CancellationEvidence(0, True),
        ):
            assert_contract_error(
                self, "DTO.InvalidCancellationEvidence", "/Cancellation", evidence
            )

        for requirement_factory in (
            lambda: PrimitiveRequirement("editor.asset.resolve_existing.v1", 2, "route.generic.v1"),
            lambda: PrimitiveRequirement("editor.asset.resolve_existing.v1", 1, ""),
        ):
            assert_contract_error(
                self,
                "DTO.InvalidPrimitiveRequirement",
                "/RequiredPrimitives",
                requirement_factory,
            )

        self.assertNotEqual(OptionalJsonValue(), OptionalJsonValue(is_set=True, value=None))
        assert_contract_error(
            self,
            "DTO.InvalidOptionalValue",
            "/CanonicalValue",
            lambda: OptionalJsonValue(is_set=False, value=1),
        )

    def test_g1_request_field_mapping_positive_vector(self) -> None:
        cancellation = CancellationEvidence(sequence=2**64 - 1, requested=True)
        request = RuntimeRequestDto(
            request_id=REQUEST_ID,
            operation=RuntimeOperation.SYNC,
            target_asset_path="/Game/Test/Asset.Asset",
            requested_profile_class_path="/Script/AssetDocument.AssetDocumentProfile",
            requested_asset_class_path="/Script/Engine.DataAsset",
            canonical_document={"Body": [None, True, -(2**63), 2**63 - 1, 1.5, "雪"]},
            deadline_unix_milliseconds=-(2**63),
            cancellation=cancellation,
            expected_runtime_revision=2**64 - 1,
            abi_version=1,
        )
        self.assertEqual(
            (
                request.request_id,
                request.operation,
                request.cancellation,
                request.expected_runtime_revision,
            ),
            (REQUEST_ID, RuntimeOperation.SYNC, cancellation, 2**64 - 1),
        )

    def test_g1_result_field_mapping_positive_vector(self) -> None:
        result = RuntimeResultDto(
            request_id=REQUEST_ID,
            terminal_status=TerminalStatus.PARTIAL,
            final_phase=RuntimePhase.FAILED,
            runtime_revision=2**64 - 1,
            diagnostics=(RuntimeDiagnostic("", "", ""),),
            canonical_value=OptionalJsonValue(True, None),
            canonical_bytes=b"{}",
            semantic_hash="adsh:v1:document:test",
            codec_version=1,
            changed_set=(ChangedRecord("authority", "stable", "/Body", "Replace"),),
        )
        self.assertEqual(result.canonical_bytes, b"{}")
        self.assertTrue(result.canonical_value.is_set)
        self.assertEqual(result.changed_set[0].stable_object_id, "stable")

    def test_g1_nested_dto_field_mapping_positive_vector(self) -> None:
        context = DiagnosticContext("payload", {"Bytes": b"\x00", "Array": [1, None]})
        diagnostic = RuntimeDiagnostic(
            "/Body", "Code", "Message", DiagnosticSeverity.WARNING, (context,)
        )
        self.assertEqual(diagnostic.context[0].value["Bytes"], b"\x00")
        self.assertEqual(
            PrimitiveRequirement("editor.asset.resolve_existing.v1", 1, "route.generic.v1"),
            PrimitiveRequirement("editor.asset.resolve_existing.v1", 1, "route.generic.v1"),
        )

    def test_g1_enum_mapping_is_exhaustive(self) -> None:
        self.assertEqual(
            {member.value for member in RuntimeOperation},
            {
                "Inspect", "Schema", "Template", "Validate", "Preflight", "Apply",
                "Extract", "Diff", "RegenerateSidecar", "Sync",
            },
        )
        self.assertEqual(
            {member.value for member in RuntimePhase},
            {"Validate", "Preflight", "Apply", "Repair", "Persist", "Verify", "Complete", "Failed"},
        )
        self.assertEqual(
            {member.value for member in TerminalStatus},
            {"Succeeded", "Failed", "Partial", "Cancelled", "DeadlineExceeded"},
        )
        self.assertEqual(
            {member.value for member in DiagnosticSeverity}, {"Info", "Warning", "Error"}
        )

    def test_diagnostic_representation_is_exact_and_snapshotted(self) -> None:
        source = {"Nested": [1, {"Value": "before"}]}
        diagnostic = RuntimeDiagnostic(
            "/Body", "Code", "Message", DiagnosticSeverity.ERROR,
            (DiagnosticContext("payload", source),),
        )
        source["Nested"][1]["Value"] = "after"
        self.assertEqual(diagnostic.context[0].value["Nested"][1]["Value"], "before")
        for factory, path in (
            (lambda: RuntimeDiagnostic(1, "Code", "Message"), "/Diagnostics/Path"),
            (lambda: RuntimeDiagnostic("/", "Code", "Message", "Error"), "/Diagnostics/Severity"),
            (lambda: RuntimeDiagnostic("/", "Code", "Message", context=[]), "/Diagnostics/Context"),
            (lambda: DiagnosticContext(1, None), "/Diagnostics/Context/Key"),
        ):
            assert_contract_error(self, "DTO.InvalidRepresentationType", path, factory)

    def test_diagnostic_carrier_does_not_add_g1_semantic_policy(self) -> None:
        diagnostic = RuntimeDiagnostic(
            "", "", "", DiagnosticSeverity.INFO,
            (DiagnosticContext("duplicate", 1), DiagnosticContext("duplicate", 2)),
        )
        self.assertEqual(diagnostic.path, "")
        self.assertEqual([item.key for item in diagnostic.context], ["duplicate", "duplicate"])

    def test_request_and_optional_result_snapshots_are_transitively_alias_isolated(self) -> None:
        request_source = {"Body": [{"Value": "before"}]}
        optional_source = {"Body": [{"Value": "before"}]}
        request = RuntimeRequestDto(
            REQUEST_ID, RuntimeOperation.VALIDATE, "/Game/A.A", "/Script/P", "/Script/C", request_source
        )
        optional = OptionalJsonValue(True, optional_source)
        request_source["Body"][0]["Value"] = "after"
        optional_source["Body"][0]["Value"] = "after"
        self.assertEqual(request.canonical_document["Body"][0]["Value"], "before")
        self.assertEqual(optional.value["Body"][0]["Value"], "before")

    def test_diagnostic_runtime_value_snapshot_is_transitively_alias_isolated(self) -> None:
        source = [{"Payload": bytearray(b"x")}]
        assert_contract_error(
            self,
            "DTO.InvalidRepresentationType",
            "/Diagnostics/Context/Value/0/Payload",
            lambda: DiagnosticContext("payload", source),
        )
        valid = [{"Payload": b"x"}]
        context = DiagnosticContext("payload", valid)
        valid[0]["Payload"] = b"y"
        self.assertEqual(context.value[0]["Payload"], b"x")

    def test_all_dto_exact_type_equivalence_classes_fail_closed(self) -> None:
        class ListSubclass(list):
            pass

        class DictSubclass(dict):
            pass

        class IntSubclass(int):
            pass

        class FloatSubclass(float):
            pass

        class StringSubclass(str):
            pass

        cases = (
            ("request-id", "/RequestId", lambda: make_request(request_id=StringSubclass(REQUEST_ID))),
            ("operation", "/Operation", lambda: make_request(operation="Validate")),
            ("target", "/TargetAssetPath", lambda: make_request(target_asset_path=1)),
            ("profile-class", "/RequestedProfileClassPath", lambda: make_request(requested_profile_class_path=1)),
            ("asset-class", "/RequestedAssetClassPath", lambda: make_request(requested_asset_class_path=1)),
            ("deadline", "/DeadlineUnixMilliseconds", lambda: make_request(deadline_unix_milliseconds=True)),
            ("expected-revision", "/ExpectedRuntimeRevision", lambda: make_request(expected_runtime_revision=True)),
            ("cancellation", "/Cancellation", lambda: make_request(cancellation={})),
            ("request-json-list-subclass", "/CanonicalDocument", lambda: make_request(canonical_document=ListSubclass())),
            ("request-json-dict-subclass", "/CanonicalDocument", lambda: make_request(canonical_document=DictSubclass())),
            ("request-json-int-subclass", "/CanonicalDocument", lambda: make_request(canonical_document=IntSubclass(1))),
            ("request-json-float-subclass", "/CanonicalDocument", lambda: make_request(canonical_document=FloatSubclass(1.0))),
            ("request-json-string-subclass", "/CanonicalDocument", lambda: make_request(canonical_document=StringSubclass("value"))),
            ("request-json-bytes", "/CanonicalDocument", lambda: make_request(canonical_document=b"value")),
            ("result-id", "/RequestId", lambda: make_result(request_id=1)),
            ("terminal", "/TerminalStatus", lambda: make_result(terminal_status="Succeeded")),
            ("final-phase", "/FinalPhase", lambda: make_result(final_phase="Complete")),
            ("runtime-revision", "/RuntimeRevision", lambda: make_result(runtime_revision=True)),
            ("diagnostics-container", "/Diagnostics", lambda: make_result(diagnostics=[])),
            ("diagnostics-element", "/Diagnostics", lambda: make_result(diagnostics=("diagnostic",))),
            ("canonical-value-carrier", "/CanonicalValue", lambda: make_result(canonical_value=None)),
            ("canonical-bytes", "/CanonicalBytes", lambda: make_result(canonical_bytes=bytearray())),
            ("semantic-hash", "/SemanticHash", lambda: make_result(semantic_hash=1)),
            ("codec-version", "/CodecVersion", lambda: make_result(codec_version=True)),
            ("changed-container", "/ChangedSet", lambda: make_result(changed_set=[])),
            ("changed-element", "/ChangedSet", lambda: make_result(changed_set=("change",))),
            ("optional-is-set", "/CanonicalValue", lambda: OptionalJsonValue(is_set=1)),
            ("optional-json", "/CanonicalValue", lambda: OptionalJsonValue(True, b"value")),
        )
        for name, path, factory in cases:
            with self.subTest(case=name):
                assert_contract_error(self, "DTO.InvalidRepresentationType", path, factory)

        for cancellation_factory in (
            lambda: CancellationEvidence(True, False),
            lambda: CancellationEvidence(1, 1),
        ):
            assert_contract_error(
                self,
                "DTO.InvalidCancellationEvidence",
                "/Cancellation",
                cancellation_factory,
            )

        for abi_factory in (
            lambda: make_request(abi_version=True),
            lambda: make_result(abi_version=True),
        ):
            assert_contract_error(self, "DTO.AbiVersionMismatch", "/AbiVersion", abi_factory)

    def test_all_nested_dto_and_diagnostic_scalar_types_fail_closed(self) -> None:
        changed_fields = (
            ("authority_id", "/ChangedSet/AuthorityId"),
            ("stable_object_id", "/ChangedSet/StableObjectId"),
            ("path", "/ChangedSet/Path"),
            ("change_kind", "/ChangedSet/ChangeKind"),
        )
        valid_changed = {
            "authority_id": "authority",
            "stable_object_id": "stable",
            "path": "/Body",
            "change_kind": "Replace",
        }
        for field, path in changed_fields:
            with self.subTest(record_field=field):
                values = dict(valid_changed)
                values[field] = 1
                assert_contract_error(
                    self, "DTO.InvalidRepresentationType", path,
                    lambda values=values: ChangedRecord(**values),
                )

        diagnostic_fields = (
            ("path", "/Diagnostics/Path"),
            ("code", "/Diagnostics/Code"),
            ("message", "/Diagnostics/Message"),
        )
        for field, path in diagnostic_fields:
            with self.subTest(diagnostic_field=field):
                values = {"path": "/", "code": "Code", "message": "Message"}
                values[field] = 1
                assert_contract_error(
                    self, "DTO.InvalidRepresentationType", path,
                    lambda values=values: RuntimeDiagnostic(**values),
                )
        for name, path, factory in (
            ("severity", "/Diagnostics/Severity", lambda: RuntimeDiagnostic("/", "Code", "Message", "Error")),
            ("context-container", "/Diagnostics/Context", lambda: RuntimeDiagnostic("/", "Code", "Message", context=[])),
            ("context-element", "/Diagnostics/Context", lambda: RuntimeDiagnostic("/", "Code", "Message", context=("context",))),
            ("context-key", "/Diagnostics/Context/Key", lambda: DiagnosticContext(1, None)),
            ("context-value-list-subclass", "/Diagnostics/Context/Value", lambda: DiagnosticContext("key", type("ListSubclass", (list,), {})())),
            ("context-value-bytearray", "/Diagnostics/Context/Value", lambda: DiagnosticContext("key", bytearray())),
        ):
            with self.subTest(case=name):
                assert_contract_error(self, "DTO.InvalidRepresentationType", path, factory)

        for name, factory in (
            ("primitive-id-type", lambda: PrimitiveRequirement(1, 1, "route.v1")),
            ("primitive-version-bool", lambda: PrimitiveRequirement("editor.test.v1", True, "route.v1")),
            ("primitive-route-type", lambda: PrimitiveRequirement("editor.test.v1", 1, 1)),
        ):
            with self.subTest(case=name):
                assert_contract_error(
                    self, "DTO.InvalidPrimitiveRequirement", "/RequiredPrimitives", factory
                )


class MetaContractTests(unittest.TestCase):
    def test_typed_meta_keeps_independent_policies_distinct(self) -> None:
        first = make_region()
        second = make_region(
            "Observed",
            "/Body/Observed",
            ownership=RegionOwnership.OBSERVED,
            identity=IdentityPolicy.POSITIONAL,
            order=OrderPolicy.PRESERVE,
            default=DefaultPolicy.ENGINE_DEFAULT,
            omission=OmissionPolicy.ABSENT_BY_POLICY,
            deletion=DeletePolicy.FORBID,
            comparison=ComparisonPolicy.ORDERED,
        )
        profile = make_profile(regions=(first, second))
        document = DocumentMeta(1, "/Game/Test/Asset.Asset", profile.exact_asset_class_path, DocumentAction.UPDATE)
        self.assertEqual(document.action, DocumentAction.UPDATE)
        self.assertNotEqual(first.ownership, second.ownership)
        with self.assertRaises(FrozenInstanceError):
            first.region_id = "mutated"

    def test_declaration_validation_decision_table(self) -> None:
        class OtherEnum(str, Enum):
            VALUE = "Managed"

        cases = (
            ("schema", "Meta.InvalidSchemaVersion", "/SchemaVersion", lambda: DocumentMeta(0, "target", "/Script/C", DocumentAction.UPDATE)),
            ("target", "Meta.EmptyTarget", "/Target", lambda: DocumentMeta(1, "", "/Script/C", DocumentAction.UPDATE)),
            ("document-class", "Meta.EmptyExactClassPath", "/Class", lambda: DocumentMeta(1, "target", "", DocumentAction.UPDATE)),
            ("document-action", "Meta.InvalidDeclarationType", "/Action", lambda: DocumentMeta(1, "target", "/Script/C", "Update")),
            ("profile-id", "Meta.EmptyProfileId", "/ProfileId", lambda: make_profile(profile_id="")),
            ("route-id", "Meta.EmptyRouteId", "/RouteId", lambda: make_profile(route_id="", requirements=())),
            ("profile-class", "Meta.EmptyExactClassPath", "/Class", lambda: make_profile(exact_class="")),
            ("region-id", "Meta.EmptyRegionId", "/Regions", lambda: make_region(region_id="")),
            ("body-path", "Meta.InvalidBodyPath", "/Regions/Body/BodyPath", lambda: make_region(body_path="/Body/~2")),
            ("region-enum", "Meta.InvalidDeclarationType", "/Regions/Body/Ownership", lambda: make_region(ownership=OtherEnum.VALUE)),
            ("regions-list", "Meta.InvalidDeclarationType", "/Regions", lambda: make_profile(regions=[make_region()])),
            ("requirements-list", "Meta.InvalidDeclarationType", "/RequiredPrimitives", lambda: make_profile(requirements=[])),
            ("duplicate-region", "Meta.DuplicateRegionId", "/Regions", lambda: make_profile(regions=(make_region(), make_region("Body", "/Body/Other")))),
            ("duplicate-path", "Meta.DuplicateBodyPath", "/Regions", lambda: make_profile(regions=(make_region(), make_region("Other", "/Body")))),
            ("duplicate-requirement", "Meta.DuplicatePrimitiveRequirement", "/RequiredPrimitives", lambda: make_profile(requirements=(PrimitiveRequirement("editor.asset.resolve_existing.v1", 1, "route.data_asset.v1"),) * 2)),
            ("route-mismatch", "Meta.RequirementRouteMismatch", "/RequiredPrimitives", lambda: make_profile(requirements=(PrimitiveRequirement("editor.asset.resolve_existing.v1", 1, "route.other.v1"),))),
        )
        for name, code, path, factory in cases:
            with self.subTest(case=name):
                assert_contract_error(self, code, path, factory)

    def test_all_meta_exact_type_equivalence_classes_fail_closed(self) -> None:
        cases = (
            ("schema-bool", "/SchemaVersion", lambda: DocumentMeta(True, "target", "/Script/C", DocumentAction.UPDATE)),
            ("target-type", "/Target", lambda: DocumentMeta(1, 1, "/Script/C", DocumentAction.UPDATE)),
            ("document-class-type", "/Class", lambda: DocumentMeta(1, "target", 1, DocumentAction.UPDATE)),
            ("action-string", "/Action", lambda: DocumentMeta(1, "target", "/Script/C", "Update")),
            ("region-id-type", "/Regions", lambda: make_region(region_id=1)),
            ("body-path-type", "/Regions/Body/BodyPath", lambda: make_region(body_path=1)),
            ("profile-id-type", "/ProfileId", lambda: make_profile(profile_id=1)),
            ("route-id-type", "/RouteId", lambda: make_profile(route_id=1, requirements=())),
            ("profile-class-type", "/Class", lambda: make_profile(exact_class=1)),
            ("regions-container", "/Regions", lambda: make_profile(regions=[make_region()])),
            ("regions-element", "/Regions", lambda: make_profile(regions=("region",))),
            ("requirements-container", "/RequiredPrimitives", lambda: make_profile(requirements=[])),
            ("requirements-element", "/RequiredPrimitives", lambda: make_profile(requirements=("requirement",))),
        )
        for field, path in (
            ("ownership", "/Regions/Body/Ownership"),
            ("identity", "/Regions/Body/Identity"),
            ("order", "/Regions/Body/Order"),
            ("default", "/Regions/Body/Default"),
            ("omission", "/Regions/Body/Omission"),
            ("deletion", "/Regions/Body/Deletion"),
            ("comparison", "/Regions/Body/Comparison"),
        ):
            cases += ((f"region-{field}", path, lambda field=field: make_region(**{field: "invalid"})),)
        for name, path, factory in cases:
            with self.subTest(case=name):
                assert_contract_error(self, "Meta.InvalidDeclarationType", path, factory)


class RuntimeTestHandler:
    """A request-state-free handler fixture with the entire frozen signature."""
    runtime_entry_module = "asset_document.tests.test_registry"

    def __init__(self, profile):
        self.profile_id = profile.profile_id
        self.exact_asset_class_path = profile.exact_asset_class_path
        self._requirements = profile.required_primitives

    def required_primitives(self):
        return self._requirements

    def inspect(self, *, mode, exact_class_path, exact_asset_object_path, editor_asset_handle, context):
        return {"Class": exact_class_path}

    def inspect_profile(self):
        return {"Class": self.exact_asset_class_path, "BodySections": ["Body"]}

    def create_template(self, target):
        return {"Target": target, "Class": self.exact_asset_class_path, "Action": "Update", "Body": {}}

    def validate(self, document):
        return None

    def canonicalize(self, document):
        return document

    def create_target_context(self, document, context):
        return ()

    def preflight(self, document, scratch_handle, context):
        return None

    def apply(self, document, read_asset_handle, mutable_asset_handle, context):
        return ()

    def repair(self, document, asset_handle, context):
        return ()

    def extract(self, *, target, asset_handle, context, include_defaults=False):
        return self.create_template(target)

    def diff(self, authored, extracted):
        return ()


def make_runtime_registration(*, exact_class="/Script/Test.Runtime", profile_id="profile.runtime.v1",
                              factory=None, policy=RuntimeTargetPolicy.CREATE_OR_UPDATE):
    route = "route.runtime.v1"
    requirements = tuple(PrimitiveRequirement("editor.test." + role + ".v1", 1, route)
                         for role in ("clone", "scratch", "create", "rollback"))
    profile = make_profile(profile_id=profile_id, exact_class=exact_class, route_id=route,
                           requirements=requirements)
    lifecycle = RuntimeTargetLifecycleSpec("test.v1", "test.scratch.v1", "test.factory.v1",
                                           *requirements, ())
    return RuntimeProfileRegistration(profile, factory or (lambda: RuntimeTestHandler(profile)), policy, lifecycle)


class ProfileRuntimeBindingTests(unittest.TestCase):
    def assert_code(self, code, callback):
        assert_contract_error(self, "Registry." + code, "/Registry", callback)

    def test_profile_meta_cannot_bind_handler_factory(self):
        profile = make_profile()
        with self.assertRaises((TypeError, AttributeError)):
            profile.handler_factory = lambda: None
        self.assert_code("RuntimeRegistrationInvalid", lambda: ProfileRegistryBuilder().register_runtime(profile))

    def test_factory_is_deferred_once_and_freeze_is_idempotent(self):
        calls = []
        base = make_runtime_registration()
        handler = RuntimeTestHandler(base.profile_meta)
        registration = replace(base, handler_factory=lambda: calls.append(1) or handler)
        builder = ProfileRegistryBuilder()
        builder.register_runtime(registration)
        self.assertEqual(calls, [])
        frozen = builder.freeze()
        self.assertIs(frozen, builder.freeze())
        self.assertEqual(calls, [1])
        binding = frozen.resolve_runtime_exact(base.profile_meta.exact_asset_class_path)
        self.assertIs(binding.handler, handler)
        self.assertIs(frozen.resolve_exact(base.profile_meta.exact_asset_class_path), base.profile_meta)
        self.assertIsNone(frozen.resolve_runtime_exact("/Script/Test.Subclass"))
        with self.assertRaises(FrozenInstanceError):
            binding.target_policy = RuntimeTargetPolicy.UPDATE_ONLY
        with self.assertRaises(TypeError):
            frozen.runtime_by_class["new"] = binding

    def test_metadata_only_binding_and_exact_lookup_types(self):
        builder = ProfileRegistryBuilder()
        builder.register(make_profile())
        frozen = builder.freeze()
        self.assertEqual(frozen.runtime_registrations(), ())
        self.assertIsNone(frozen.resolve_runtime_exact(make_profile().exact_asset_class_path))
        assert_contract_error(self, "DTO.InvalidRepresentationType", "/Registry",
                              lambda: frozen.resolve_runtime_exact(1))

    def test_duplicate_registration_is_atomic_across_both_methods(self):
        runtime = make_runtime_registration()
        for first_runtime in (True, False):
            with self.subTest(first_runtime=first_runtime):
                builder = ProfileRegistryBuilder()
                if first_runtime:
                    builder.register_runtime(runtime)
                    self.assert_code("DuplicateProfileId", lambda: builder.register(runtime.profile_meta))
                else:
                    builder.register(runtime.profile_meta)
                    self.assert_code("DuplicateProfileId", lambda: builder.register_runtime(runtime))
                collision = make_runtime_registration(profile_id="other")
                self.assert_code("DuplicateExactClassPath", lambda: builder.register_runtime(collision))
                frozen = builder.freeze()
                self.assertEqual(len(frozen.profiles()), 1)
                self.assertEqual(len(frozen.runtime_registrations()), int(first_runtime))
                self.assert_code("Frozen", lambda: builder.register_runtime(runtime))

    def test_failed_freeze_never_publishes_or_retries_factory(self):
        calls = []
        builder = ProfileRegistryBuilder()
        first = make_runtime_registration(exact_class="/Script/A.A")
        builder.register_runtime(replace(first, handler_factory=lambda: calls.append("first") or RuntimeTestHandler(first.profile_meta)))
        def fail():
            calls.append("second")
            raise RuntimeError("factory failure")
        builder.register_runtime(make_runtime_registration(exact_class="/Script/B.B", profile_id="second", factory=fail))
        for _ in range(2):
            self.assert_code("RuntimeFactoryFailed", builder.freeze)
            self.assertIsNone(builder._snapshot)
        self.assertEqual(calls, ["first", "second"])
        self.assert_code("Frozen", lambda: builder.register(make_profile()))

    def test_invalid_registration_and_lifecycle_values(self):
        base = make_runtime_registration()
        for field, value, code in (
            ("profile_meta", None, "RuntimeRegistrationInvalid"),
            ("target_policy", "CreateOrUpdate", "RuntimeRegistrationInvalid"),
            ("target_lifecycle", None, "RuntimeTargetLifecycleRequired"),
            ("target_lifecycle", "lifecycle", "RuntimeTargetLifecycleInvalid"),
            ("handler_factory", None, "RuntimeFactoryMissing"),
            ("handler_factory", 1, "RuntimeFactoryNotCallable"),
            ("handler_factory", lambda required: None, "RuntimeFactoryNotCallable"),
        ):
            with self.subTest(field=field, value=value):
                self.assert_code(code, lambda: replace(base, **{field: value}))
        for field, value in (("lifecycle_id", ""), ("factory_id", 1),
                             ("scratch_category_id", " "), ("clone_scratch", "primitive"),
                             ("create_context_handle_names", []),
                             ("create_context_handle_names", ("same", "same")),
                             ("create_context_handle_names", (1,))):
            with self.subTest(field=field):
                self.assert_code("RuntimeTargetLifecycleInvalid", lambda: replace(base.target_lifecycle, **{field: value}))
        requirement = PrimitiveRequirement("editor.other.v1", 1, "other.route")
        lifecycle = replace(base.target_lifecycle, clone_scratch=requirement)
        self.assert_code("RuntimeTargetLifecyclePrimitiveMismatch", lambda: replace(base, target_lifecycle=lifecycle))

    def test_lifecycle_is_optional_only_for_update_only_profiles(self):
        base = make_runtime_registration(policy=RuntimeTargetPolicy.UPDATE_ONLY)
        registration = replace(base, target_lifecycle=None)
        self.assertIsNone(registration.target_lifecycle)
        builder = ProfileRegistryBuilder()
        builder.register_runtime(registration)
        binding = builder.freeze().resolve_runtime_exact(base.profile_meta.exact_asset_class_path)
        self.assertIsNone(binding.target_lifecycle)
        self.assertIs(binding.target_policy, RuntimeTargetPolicy.UPDATE_ONLY)
        self.assert_code("RuntimeTargetLifecycleRequired",
                         lambda: replace(registration, target_policy=RuntimeTargetPolicy.CREATE_OR_UPDATE))

    def test_generic_factory_lifecycle_component(self):
        from asset_document.regions.target_lifecycle import (
            GENERIC_FACTORY_LIFECYCLE, GENERIC_FACTORY_LIFECYCLE_ID, generic_factory_lifecycle, lifecycle_requirements)
        from asset_document.regions.primitive_catalog import GENERIC_ROUTE_ID
        lifecycle = generic_factory_lifecycle("route.curve.v1")
        self.assertEqual((lifecycle.lifecycle_id, lifecycle.scratch_category_id, lifecycle.factory_id),
                         (GENERIC_FACTORY_LIFECYCLE_ID,) * 3)
        self.assertEqual(GENERIC_FACTORY_LIFECYCLE_ID, "factory_generic.v1")
        self.assertEqual(lifecycle.create_context_handle_names, ())
        self.assertEqual(
            [(item.primitive_id, item.version, item.required_by_route_id) for item in lifecycle_requirements(lifecycle)],
            [("editor.scratch.clone_asset.v2", 2, "route.curve.v1"),
             ("editor.scratch.create_by_factory.v1", 1, "route.curve.v1"),
             ("editor.asset.create_by_factory.v2", 2, "route.curve.v1"),
             ("editor.asset.rollback_created.v1", 1, "route.curve.v1")])
        self.assertTrue(all(item.required_by_route_id == GENERIC_ROUTE_ID
                            for item in lifecycle_requirements(GENERIC_FACTORY_LIFECYCLE)))
        route = "route.curve.v1"
        profile = make_profile(profile_id="profile.curve.v1", exact_class="/Script/Engine.CurveFloat",
                               route_id=route, requirements=lifecycle_requirements(lifecycle))
        registration = RuntimeProfileRegistration(profile, lambda: RuntimeTestHandler(profile),
                                                  RuntimeTargetPolicy.CREATE_OR_UPDATE, lifecycle)
        self.assertIs(registration.target_lifecycle, lifecycle)
        foreign = generic_factory_lifecycle("route.other.v1")
        self.assert_code("RuntimeTargetLifecyclePrimitiveMismatch",
                         lambda: replace(registration, target_lifecycle=foreign))

    def test_full_protocol_and_identity_primitive_mismatches(self):
        base = make_runtime_registration()
        for name in ("required_primitives", "inspect", "inspect_profile", "create_template", "validate",
                     "canonicalize", "create_target_context", "preflight", "apply", "repair", "extract", "diff"):
            with self.subTest(missing=name):
                handler = RuntimeTestHandler(base.profile_meta)
                setattr(handler, name, None)
                builder = ProfileRegistryBuilder()
                builder.register_runtime(replace(base, handler_factory=lambda: handler))
                self.assert_code("RuntimeHandlerProtocolMismatch", builder.freeze)
        for field, value, code in (
            ("profile_id", "wrong", "RuntimeHandlerProfileMismatch"),
            ("exact_asset_class_path", "/Script/Wrong.Class", "RuntimeHandlerClassMismatch"),
            ("runtime_entry_module", "", "RuntimeHandlerProtocolMismatch"),
            ("_requirements", list(base.profile_meta.required_primitives), "RuntimeHandlerPrimitiveMismatch"),
            ("_requirements", tuple(reversed(base.profile_meta.required_primitives)), "RuntimeHandlerPrimitiveMismatch"),
            ("inspect", lambda **kwargs: {}, "RuntimeHandlerProtocolMismatch"),
        ):
            with self.subTest(field=field):
                handler = RuntimeTestHandler(base.profile_meta)
                setattr(handler, field, value)
                builder = ProfileRegistryBuilder()
                builder.register_runtime(replace(base, handler_factory=lambda: handler))
                self.assert_code(code, builder.freeze)

    def test_protocol_stub_and_async_operation_are_not_implementations(self):
        registration = make_runtime_registration()
        class StubHandler(RuntimeTestHandler):
            apply = RuntimeProfileHandler.apply
        class AsyncHandler(RuntimeTestHandler):
            async def apply(self, document, read_asset_handle, mutable_asset_handle, context):
                return ()
        for handler_type in (StubHandler, AsyncHandler):
            with self.subTest(handler_type=handler_type):
                builder = ProfileRegistryBuilder()
                builder.register_runtime(replace(registration, handler_factory=lambda: handler_type(registration.profile_meta)))
                self.assert_code("RuntimeHandlerProtocolMismatch", builder.freeze)

    def test_direct_snapshot_runtime_mapping_is_validated_and_copied(self):
        registration = make_runtime_registration()
        binding = FrozenRuntimeProfileRegistration(registration.profile_meta,
                    RuntimeTestHandler(registration.profile_meta), registration.target_policy,
                    registration.target_lifecycle)
        key = registration.profile_meta.exact_asset_class_path
        runtime = {key: binding}
        frozen = FrozenProfileRegistry({key: registration.profile_meta}, runtime)
        runtime.clear()
        self.assertEqual(frozen.runtime_registrations(), (binding,))
        self.assert_code("RuntimeRegistrationInvalid", lambda: FrozenProfileRegistry({key: registration.profile_meta}, {"wrong": binding}))


class BlackboardDataRegistryTests(unittest.TestCase):
    def test_exact_profile_registration_is_unique(self) -> None:
        registry = build_profile_registry(
            ((BLACKBOARD_DATA_PROFILE_META, create_blackboard_data_profile),)
        )

        profile = registry.resolve_exact(BLACKBOARD_DATA_CLASS)
        self.assertIs(profile, BLACKBOARD_DATA_PROFILE_META)
        self.assertIs(PROFILE_REGISTRY.resolve_exact(BLACKBOARD_DATA_CLASS), profile)
        self.assertEqual(profile.required_primitives, BLACKBOARD_REQUIRED_PRIMITIVES)
        self.assertIs(profile_factory_for(profile), create_blackboard_data_profile)
        assert_contract_error(
            self,
            "Registry.DuplicateProfileId",
            "/Registry",
            lambda: build_profile_registry(
                (
                    (BLACKBOARD_DATA_PROFILE_META, create_blackboard_data_profile),
                    (BLACKBOARD_DATA_PROFILE_META, create_blackboard_data_profile),
                )
            ),
        )


class ProfileRegistryContractTests(unittest.TestCase):
    def test_register_freeze_and_exact_lookup(self) -> None:
        data = make_profile()
        blackboard = make_profile(
            "profile.blackboard.v1", "route.blackboard.v1", "/Script/AIModule.BlackboardData"
        )
        builder = ProfileRegistryBuilder()
        builder.register(data)
        builder.register(blackboard)
        registry = builder.freeze()
        self.assertEqual(
            tuple(profile.exact_asset_class_path for profile in registry.profiles()),
            ("/Script/AIModule.BlackboardData", "/Script/Engine.DataAsset"),
        )
        self.assertIs(registry.resolve_exact(data.exact_asset_class_path), data)

    def test_duplicate_profile_id_fails_closed(self) -> None:
        builder = ProfileRegistryBuilder()
        builder.register(make_profile())
        assert_contract_error(
            self, "Registry.DuplicateProfileId", "/Registry",
            lambda: builder.register(make_profile(exact_class="/Script/Other.Class")),
        )

    def test_duplicate_exact_class_path_fails_closed(self) -> None:
        builder = ProfileRegistryBuilder()
        builder.register(make_profile())
        assert_contract_error(
            self, "Registry.DuplicateExactClassPath", "/Registry",
            lambda: builder.register(make_profile(profile_id="profile.other.v1")),
        )

    def test_empty_freeze_fails_closed(self) -> None:
        assert_contract_error(self, "Registry.Empty", "/Registry", ProfileRegistryBuilder().freeze)

    def test_register_after_freeze_fails_closed(self) -> None:
        builder = ProfileRegistryBuilder()
        builder.register(make_profile())
        builder.freeze()
        assert_contract_error(
            self, "Registry.Frozen", "/Registry",
            lambda: builder.register(make_profile("other", "route.other.v1", "/Script/Other.Class")),
        )

    def test_repeated_freeze_returns_same_snapshot(self) -> None:
        builder = ProfileRegistryBuilder()
        builder.register(make_profile())
        self.assertIs(builder.freeze(), builder.freeze())

    def test_missing_exact_class_has_no_inheritance_or_generic_fallback(self) -> None:
        builder = ProfileRegistryBuilder()
        builder.register(make_profile())
        registry = builder.freeze()
        assert_contract_error(
            self,
            "Registry.ProfileNotFound",
            "/Registry//Script/Engine.PrimaryDataAsset",
            lambda: registry.resolve_exact("/Script/Engine.PrimaryDataAsset"),
        )

    def test_required_primitives_are_sorted_unique_declarations(self) -> None:
        first = make_profile(requirements=(PrimitiveRequirement("editor.z.v1", 1, "route.data_asset.v1"),))
        second = make_profile(
            "profile.other.v1", "route.other.v1", "/Script/Other.Class",
            requirements=(
                PrimitiveRequirement("editor.z.v1", 1, "route.other.v1"),
                PrimitiveRequirement("editor.a.v1", 1, "route.other.v1"),
            ),
        )
        # The same full triple across two profiles sharing one route is deduplicated globally.
        shared = make_profile(
            "profile.shared.v1", "route.other.v1", "/Script/Shared.Class",
            requirements=(PrimitiveRequirement("editor.a.v1", 1, "route.other.v1"),),
        )
        builder = ProfileRegistryBuilder()
        builder.register(first)
        builder.register(second)
        builder.register(shared)
        self.assertEqual(
            builder.freeze().required_primitives(),
            (
                PrimitiveRequirement("editor.a.v1", 1, "route.other.v1"),
                PrimitiveRequirement("editor.z.v1", 1, "route.data_asset.v1"),
                PrimitiveRequirement("editor.z.v1", 1, "route.other.v1"),
            ),
        )

    def test_wrong_runtime_types_fail_closed(self) -> None:
        cases = (
            ("request-op", "/Operation", lambda: RuntimeRequestDto(REQUEST_ID, "Validate", "/Game/A.A", "/Script/P", "/Script/C", {})),
            ("request-deadline-bool", "/DeadlineUnixMilliseconds", lambda: RuntimeRequestDto(REQUEST_ID, RuntimeOperation.VALIDATE, "/Game/A.A", "/Script/P", "/Script/C", {}, deadline_unix_milliseconds=True)),
            ("result-diagnostics-list", "/Diagnostics", lambda: RuntimeResultDto(REQUEST_ID, TerminalStatus.SUCCEEDED, RuntimePhase.COMPLETE, 1, diagnostics=[])),
            ("changed-list", "/ChangedSet", lambda: RuntimeResultDto(REQUEST_ID, TerminalStatus.SUCCEEDED, RuntimePhase.COMPLETE, 1, changed_set=[])),
            ("profile-wrong-type", "/Registry", lambda: ProfileRegistryBuilder().register("profile")),
        )
        for name, path, factory in cases:
            with self.subTest(case=name):
                assert_contract_error(self, "DTO.InvalidRepresentationType", path, factory)

    def test_registry_snapshot_copies_direct_constructor_and_builder_mappings(self) -> None:
        profile = make_profile()
        original = {profile.exact_asset_class_path: profile}
        direct = FrozenProfileRegistry(original)
        original.clear()
        self.assertEqual(direct.profiles(), (profile,))

        builder = ProfileRegistryBuilder()
        builder.register(profile)
        frozen = builder.freeze()
        builder._profiles_by_class.clear()
        self.assertEqual(frozen.profiles(), (profile,))

    def test_declaration_containers_are_exact_tuples_and_transitively_frozen(self) -> None:
        assert_contract_error(
            self, "Meta.InvalidDeclarationType", "/Regions",
            lambda: make_profile(regions=[make_region()]),
        )
        profile = make_profile()
        with self.assertRaises(FrozenInstanceError):
            profile.regions[0].body_path = "/Body/Mutated"

    def test_direct_frozen_registry_constructor_fails_closed_on_invalid_mapping(self) -> None:
        profile = make_profile()
        duplicate_id = make_profile(exact_class="/Script/Other.Class")
        for name, code, factory in (
            ("non-mapping", "DTO.InvalidRepresentationType", lambda: FrozenProfileRegistry([])),
            ("key-type", "DTO.InvalidRepresentationType", lambda: FrozenProfileRegistry({1: profile})),
            ("value-type", "DTO.InvalidRepresentationType", lambda: FrozenProfileRegistry({profile.exact_asset_class_path: "profile"})),
            ("key-profile-mismatch", "DTO.InvalidRepresentationType", lambda: FrozenProfileRegistry({"/Script/Wrong.Class": profile})),
            ("duplicate-profile-id", "Registry.DuplicateProfileId", lambda: FrozenProfileRegistry({profile.exact_asset_class_path: profile, duplicate_id.exact_asset_class_path: duplicate_id})),
            ("empty", "Registry.Empty", lambda: FrozenProfileRegistry({})),
        ):
            with self.subTest(case=name):
                assert_contract_error(self, code, "/Registry", factory)


if __name__ == "__main__":
    unittest.main()
