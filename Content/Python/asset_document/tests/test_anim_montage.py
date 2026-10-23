# Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import importlib
import importlib.util
import unittest

from asset_document.diagnostics import (
    AssetDocumentContractError,
    DiagnosticContext,
    DiagnosticSeverity,
    RuntimeDiagnostic,
)
from asset_document.tests.fixtures.anim_montage_cases import (
    CANONICAL_BODY_KEYS,
    AnimationReadOnlyContext,
    EXACT_CLASS,
    InvocationProbe,
    TARGET,
    document,
    rich_document,
    template_body,
    observed_body,
    observed_settings_body,
    native_settings,
    MontageContext,
)


def assert_error(test: unittest.TestCase, code: str, path: str, callback) -> None:
    with test.assertRaises(AssetDocumentContractError) as caught:
        callback()
    diagnostic = caught.exception.diagnostic
    test.assertEqual((diagnostic.code, diagnostic.path), (code, path))


class AnimMontageProfileTests(unittest.TestCase):
    def test_exact_class_and_all_keys(self) -> None:
        self.assertIsNotNone(
            importlib.util.find_spec("asset_document.profiles.anim_montage")
        )
        module = importlib.import_module("asset_document.profiles.anim_montage")
        profile = module.AnimMontageProfileCandidate()
        self.assertEqual(
            profile.inspect_profile(),
            {"Class": EXACT_CLASS, "BodySections": list(CANONICAL_BODY_KEYS)},
        )

    def test_template_exposes_every_canonical_body_key(self) -> None:
        module = importlib.import_module("asset_document.profiles.anim_montage")
        profile = module.AnimMontageProfileCandidate()
        self.assertEqual(
            profile.create_template(TARGET),
            {
                "SchemaVersion": 1,
                "Target": TARGET,
                "Class": EXACT_CLASS,
                "Action": "CreateOrUpdate",
                "Definitions": {},
                "Properties": {},
                "Body": template_body(),
            },
        )

    def test_alias_precedence_and_canonical_omission_empty_delete(self) -> None:
        profile = importlib.import_module(
            "asset_document.profiles.anim_montage"
        ).AnimMontageProfileCandidate()
        sparse = document(
            {
                "Skeleton": "/Game/Animation/SK_Legacy",
                "PreviewMesh": "/Game/Animation/SK_PreviewLegacy",
                "References": {"Skeleton": "/Game/Animation/SK_Combat"},
                "Preview": {"PreviewMesh": "/Game/Animation/SK_CombatPreview"},
                "Curves": [],
                "CompositeSections": [],
            }
        )
        canonical = profile.canonicalize(sparse)
        self.assertEqual(canonical["Body"]["Skeleton"], "/Game/Animation/SK_Combat")
        self.assertEqual(
            canonical["Body"]["PreviewMesh"], "/Game/Animation/SK_CombatPreview"
        )
        self.assertEqual(canonical["Body"]["Curves"], [])
        self.assertEqual(canonical["Body"]["CompositeSections"], [])
        self.assertNotIn("RootMotion", canonical["Body"])

    def test_registered_animation_catalog_closure(self):
        from asset_document.profiles.anim_montage import AnimMontageProfileCandidate, ANIM_MONTAGE_RUNTIME_REGISTRATION
        profile = AnimMontageProfileCandidate()
        expected = (
            "editor.class.resolve_canonical.v1", "editor.asset.resolve_existing.v1",
            "animation.montage.profile.snapshot.v1", "animation.curve.float.replace.v2",
            "animation.asset.preview_mesh.set.v1", "animation.asset.skeleton.set.v1",
            "animation.montage.slot_tracks.replace.v1", "animation.montage.sections.mutate.v1",
            "animation.montage.notifies.replace.v1", "animation.montage.notify_states.replace.v1",
            "editor.property.enumerate_admitted.v1",
            "editor.property.enumerate_admitted_batch.v1", "editor.property.write_admitted_batch.v1",
            "animation.montage.settings.set.v1", "animation_asset.skeleton.read.v1",
            "animation.montage.structure.check.v1",
            # The shared generic factory lifecycle (Skeleton = factory context).
            "editor.scratch.clone_asset.v2", "editor.scratch.create_by_factory.v1",
            "editor.asset.create_by_factory.v2", "editor.asset.rollback_created.v1")
        self.assertEqual(tuple(r.primitive_id for r in profile.required_primitives()), expected)
        self.assertEqual(tuple(r.version for r in profile.required_primitives()),
                         (1,) * 3 + (2,) + (1,) * 12 + (2, 1, 2, 1))
        lifecycle = ANIM_MONTAGE_RUNTIME_REGISTRATION.target_lifecycle
        self.assertEqual((lifecycle.lifecycle_id, lifecycle.create_context_handle_names),
                         ("factory_generic.v1", ("factory_context",)))
        self.assertTrue(all(r.required_by_route_id == "animation_profile" for r in profile.required_primitives()))
        self.assertEqual(ANIM_MONTAGE_RUNTIME_REGISTRATION.profile_meta.required_primitives, profile.required_primitives())

    def test_validation_rejects_unknown_skipped_wrong_class_and_bad_section_link(self) -> None:
        profile = importlib.import_module(
            "asset_document.profiles.anim_montage"
        ).AnimMontageProfileCandidate()
        wrong_class = document()
        wrong_class["Class"] = "/Script/Engine.AnimSequence"
        invalid_link = document(
            {
                "CompositeSections": [
                    {"Name": "AttackA", "NextSectionName": "MissingSection"}
                ]
            }
        )
        invalid_nested_alias = document({"References": {"Skeleton": 7}})
        for candidate, code, path in (
            (document({"Unmanaged": {}}), "AnimMontage.UnknownBodyKey", "/Body/Unmanaged"),
            (document({"_Skipped": []}), "AnimMontage.ExtractOnlyBodyKey", "/Body/_Skipped"),
            (wrong_class, "AnimMontage.ClassMismatch", "/Class"),
            (invalid_link, "AnimMontage.InvalidSectionLink", "/Body/CompositeSections/0/NextSectionName"),
            (invalid_nested_alias, "AnimMontage.InvalidAlias", "/Body/References/Skeleton"),
        ):
            with self.subTest(code=code):
                assert_error(self, code, path, lambda candidate=candidate: profile.validate(candidate))

    def test_invalid_settings_fail_before_any_invocation(self) -> None:
        profile = importlib.import_module(
            "asset_document.profiles.anim_montage"
        ).AnimMontageProfileCandidate()
        cases = {
            "Sync": ({"SyncSlotIndex": -2}, "AnimMontage.InvalidSettings", "/Body/Sync/SyncSlotIndex"),
            "RootMotion": ({"RootMotionRootLock": "Bad"}, "AnimMontage.InvalidSettings",
                           "/Body/RootMotion/RootMotionRootLock"),
            "Metadata": ([{"Class": "NoPath", "Properties": {}}], "AnimMontage.InvalidSettings",
                         "/Body/Metadata/0/Class"),
            "SectionMetadata": ({"Start": {}}, "AnimMontage.InvalidSettings", "/Body/SectionMetadata/Start"),
            "TimeStretch": ({"SamplingRate": 0}, "AnimMontage.InvalidSettings", "/Body/TimeStretch/SamplingRate"),
            "Blend": ({"BlendInOption": "Nope"}, "AnimMontage.InvalidSettings", "/Body/Blend/BlendInOption"),
            "Preview": ({"Extra": True}, "AnimMontage.UnsupportedAuthoredField", "/Body/Preview/Extra"),
        }
        for key, (value, code, path) in cases.items():
            probe = InvocationProbe()
            with self.subTest(key=key):
                assert_error(self, code, path,
                             lambda key=key, value=value, probe=probe: profile.apply(
                                 document({key: value}), None, None, probe))
                self.assertEqual(probe.calls, 0)

    def test_schema_and_extract_requires_real_context(self) -> None:
        profile = importlib.import_module(
            "asset_document.profiles.anim_montage"
        ).AnimMontageProfileCandidate()
        self.assertEqual(
            profile.schema(),
            {
                "SchemaVersion": 1,
                "Class": EXACT_CLASS,
                "Type": "object",
                "Required": ["SchemaVersion", "Target", "Class", "Action"],
                "Properties": {"Body": {"type": "object", "keys": list(CANONICAL_BODY_KEYS)}},
            },
        )
        with self.assertRaises(TypeError):
            profile.extract(rich_document())

    def test_snapshot_extract_and_diff_observe_actual_not_authored(self):
        from asset_document.profiles.anim_montage import AnimMontageProfileCandidate
        profile = AnimMontageProfileCandidate()
        context = AnimationReadOnlyContext()
        actual = profile.extract(target=TARGET, asset_handle=context.asset_handle, context=context)
        self.assertEqual(context.calls, ["animation.montage.profile.snapshot.v1"])
        self.assertEqual(context.handler_calls, 1)
        self.assertEqual(actual["Body"], observed_body())
        self.assertEqual(profile.diff(document(observed_body()), actual), ())
        desired = observed_body()
        desired["Curves"][0]["Comment"] = "desired but never observed"
        desired["Notifies"][0]["Object"]["Properties"]["NotifyColor"] = "blue"
        self.assertEqual([d.path for d in profile.diff(document(desired), actual)],
                         ["/Body/Curves", "/Body/Notifies/0"])
        self.assertEqual(actual["Body"]["Curves"][0]["Comment"], "observed")

    def test_notifies_compare_as_a_keyed_set_with_authored_properties(self):
        # The editor keeps notifies sorted by time; a document may list them in any order, and a
        # notify object lists only the properties it sets.
        from asset_document.profiles.anim_montage import AnimMontageProfileCandidate
        profile = AnimMontageProfileCandidate()

        def notify(time, cls, properties, duration=None):
            row = {"Time": time, "TrackIndex": 0, "Object": {"Class": cls, "Properties": properties}}
            if duration is not None:
                row["Duration"] = duration
            return row

        sound = "/Script/Engine.AnimNotify_PlaySound"
        effect = "/Script/Engine.AnimNotifyState_TimedParticleEffect"
        actual = observed_body()
        actual["Notifies"] = [notify(0.2, sound, {"VolumeMultiplier": 1.0, "bFollow": False}),
                              notify(0.6, sound, {"VolumeMultiplier": 0.5, "bFollow": False})]
        actual["NotifyStates"] = [notify(0.15, effect, {"bDestroyAtEnd": True}, 0.3),
                                  notify(0.5, effect, {"bDestroyAtEnd": False}, 0.25)]
        desired = observed_body()
        desired["Notifies"] = [notify(0.6, sound, {"VolumeMultiplier": 0.5}), notify(0.2, sound, {})]
        desired["NotifyStates"] = [notify(0.5, effect, {"bDestroyAtEnd": False}, 0.25), notify(0.15, effect, {}, 0.3)]
        self.assertEqual(profile.diff(document(desired), document(actual)), ())
        desired["Notifies"][0]["Object"]["Properties"]["VolumeMultiplier"] = 0.75
        differences = profile.diff(document(desired), document(actual))
        self.assertEqual([(d.path, d.after) for d in differences], [("/Body/Notifies/0", actual["Notifies"][1])])
        desired["Notifies"][0]["Object"]["Properties"]["VolumeMultiplier"] = 0.5
        actual["Notifies"].append(notify(0.9, sound, {}))
        self.assertEqual([(d.path, d.kind.value) for d in profile.diff(document(desired), document(actual))],
                         [("/Body/Notifies/-", "Add")])

    def test_stale_provider_phase_access_and_class_rejections_are_preserved(self) -> None:
        profile = importlib.import_module(
            "asset_document.profiles.anim_montage"
        ).AnimMontageProfileCandidate()
        cases = (
            ({"stale": True}, "PrimitiveCatalog.StaleOpaqueHandle"),
            ({"provider_id": "foreign"}, "PrimitiveCatalog.ProviderMismatch"),
            ({"phase": "apply"}, "PrimitiveCatalog.InvalidPhase"),
            ({"access": "Mutable"}, "PrimitiveCatalog.InvalidHandleAccess"),
            ({"asset_class": "/Script/Engine.AnimSequence"}, "PrimitiveCatalog.WrongClass"),
        )
        for options, code in cases:
            normalized = dict(options)
            if normalized.get("phase") == "apply":
                from asset_document.dto import RuntimePhase

                normalized["phase"] = RuntimePhase.APPLY
            context = AnimationReadOnlyContext(**normalized)
            with self.subTest(code=code):
                assert_error(
                    self,
                    code,
                    "/HandleArguments",
                    lambda context=context: profile.extract(
                        target=TARGET,
                        asset_handle=context.asset_handle,
                        context=context,
                    ),
                )
                self.assertEqual((context.calls, context.handler_calls), ([], 0))

    def test_catalog_rejection_preserves_original_runtime_diagnostic(self) -> None:
        profile = importlib.import_module(
            "asset_document.profiles.anim_montage"
        ).AnimMontageProfileCandidate()
        original = RuntimeDiagnostic(
            path="/Provider/Asset",
            code="AnimationProvider.ReadOnlyRejected",
            message="provider rejected the verified asset",
            severity=DiagnosticSeverity.WARNING,
            context=(
                DiagnosticContext("ProviderId", "animation"),
                DiagnosticContext("RequestId", "request-42"),
            ),
        )
        context = AnimationReadOnlyContext(rejection=original)

        with self.assertRaises(AssetDocumentContractError) as caught:
            profile.extract(
                target=TARGET,
                asset_handle=context.asset_handle,
                context=context,
            )

        self.assertIs(caught.exception.diagnostic, original)
        self.assertEqual(
            (context.calls, context.handler_calls),
            (["animation.montage.profile.snapshot.v1"], 0),
        )

    def test_rich_fixture_canonical_diff(self) -> None:
        profile = importlib.import_module(
            "asset_document.profiles.anim_montage"
        ).AnimMontageProfileCandidate()
        authored = document(observed_body())
        extracted = document(observed_body())
        extracted["Body"]["Curves"] = []
        self.assertEqual(
            [(item.path, item.kind.value) for item in profile.diff(authored, extracted)],
            [("/Body/Curves", "Replace")],
        )


class AnimMontageNotifyValidationTests(unittest.TestCase):
    """A Create with notifies passed validate but failed apply; both now agree."""

    def _create(self, notifies=None, states=None, end_time=1.0):
        body = {"Skeleton": "/Game/SK.SK", "SlotAnimTracks": [{"SlotName": "DefaultSlot", "Segments": [
            {"AnimReference": "/Game/A.A", "StartPos": 0.0, "AnimStartTime": 0.0, "AnimEndTime": end_time,
             "AnimPlayRate": 1.0, "LoopingCount": 1}]}]}
        if notifies is not None:
            body["Notifies"] = notifies
        if states is not None:
            body["NotifyStates"] = states
        return dict(document(body), Action="Create")

    def test_create_with_notifies_on_track_zero_validates(self) -> None:
        profile = importlib.import_module("asset_document.profiles.anim_montage").AnimMontageProfileCandidate()
        notify = {"Time": 0.55, "TrackIndex": 0, "Object": {"Class": "/Script/Engine.AnimNotify", "Properties": {}}}
        state = {"Time": 0.3, "Duration": 0.35, "TrackIndex": 0, "Object": {"Class": "/Script/Engine.AnimNotifyState", "Properties": {}}}
        profile.validate(self._create([notify], [state]))
        assert_error(self, "AnimMontage.NotifyTrackMissing", "/Body/NotifyStates/0/TrackIndex",
                     lambda: profile.validate(self._create([notify], [dict(state, TrackIndex=1)])))
        assert_error(self, "AnimMontage.NotifyBeyondLength", "/Body/NotifyStates/0/Time",
                     lambda: profile.validate(self._create([notify], [dict(state, Time=0.8)])))
        assert_error(self, "AnimMontage.NotifyBeyondLength", "/Body/Notifies/0/Time",
                     lambda: profile.validate(self._create([dict(notify, Time=1.5)], [state])))
        profile.validate(dict(self._create([dict(notify, TrackIndex=2)]), Action="CreateOrUpdate"))


class AnimMontageExecutionTests(unittest.TestCase):
    def setUp(self):
        from asset_document.profiles.anim_montage import AnimMontageProfileCandidate
        self.profile = AnimMontageProfileCandidate()

    def preflight(self, body, *, exists=True):
        context = MontageContext(exists=exists)
        plan = self.profile.preflight(document(body), context.asset_handle, context)
        return plan, context

    def apply(self, body):
        from asset_document.dto import RuntimePhase
        context = MontageContext(phase=RuntimePhase.APPLY)
        self.profile.apply(document(body), context.asset_handle, context.asset_handle, context)
        return context

    def test_sparse_omission_and_explicit_empty_remain_distinct(self):
        omitted, _ = self.preflight({})
        self.assertEqual(omitted, [])
        omitted_apply = self.apply({})
        self.assertEqual(omitted_apply.calls, [])
        for key, primitive, intent in (
            ("Curves", "animation.curve.float.replace.v2", {"curves": []}),
            ("Notifies", "animation.montage.notifies.replace.v1", {"notifies": []}),
            ("NotifyStates", "animation.montage.notify_states.replace.v1", {"notify_states": []}),
            ("SlotAnimTracks", "animation.montage.slot_tracks.replace.v1", {"slots": []}),
            ("CompositeSections", "animation.montage.sections.mutate.v1", {"sections": []}),
        ):
            with self.subTest(key=key):
                plan, _ = self.preflight({key: []})
                self.assertEqual(len(plan), 1)
                self.assertEqual((plan[0]["PrimitiveId"], plan[0]["Intent"]), (primitive, intent))
                self.assertEqual(self.apply({key: []}).invocations[0][:2], (primitive, intent))
        self.assertEqual(self.profile.diff(document({}), document(observed_body())), ())
        self.assertEqual([d.path for d in self.profile.diff(document({"Curves": []}), document(observed_body()))], ["/Body/Curves"])

    def test_disjoint_apply_order_and_preflight_only_reads(self):
        body = {"Curves": [], "PreviewMesh": None, "Skeleton": None, "Notifies": []}
        plan, context = self.preflight(body)
        expected = ["animation.curve.float.replace.v2", "animation.asset.preview_mesh.set.v1",
                    "animation.asset.skeleton.set.v1", "animation.montage.notifies.replace.v1"]
        self.assertEqual([p["PrimitiveId"] for p in plan], expected)
        # Preflight only reads: the snapshot and the timeline check of the notifies it replaces.
        self.assertEqual(context.calls, ["editor.asset.resolve_existing.v1", "animation.montage.profile.snapshot.v1",
                                         "animation.montage.structure.check.v1"])
        self.assertEqual(self.apply(body).calls, expected)
        self.assertEqual([p["Index"] for p in plan], [0, 1, 2, 3])
        self.assertEqual(plan[0]["InputSchemaId"], "animation.curve.float.replace.v2.input.v2")

    def test_dual_notify_and_structure_share_one_native_intent(self):
        body = {"Notifies": [], "NotifyStates": [], "SlotAnimTracks": [], "CompositeSections": [], "Skeleton": None}
        plan, _ = self.preflight(body)
        self.assertEqual([s["PrimitiveId"] for s in plan],
                         ["animation.montage.notifies.replace.v1", "animation.asset.skeleton.set.v1"])
        intent = {"notifies": [], "notify_states": [], "structure": {"slots": [], "sections": []}}
        self.assertEqual(plan[0]["Intent"], intent)
        self.assertEqual(self.apply(body).invocations[0][:2], ("animation.montage.notifies.replace.v1", intent))

    def test_correspondence_is_native_owned_and_payload_same_in_both_phases(self):
        body = {"SlotAnimTracks": observed_body()["SlotAnimTracks"]}
        plan, _ = self.preflight(body)
        context = self.apply(body)
        mutation = [row for row in context.invocations if row[0].startswith("animation.montage.slot_tracks")][0]
        self.assertEqual(mutation[1], plan[0]["Intent"])
        segment = mutation[1]["slots"][0]["segments"][0]
        self.assertEqual((segment["previous_slot"], segment["previous_segment"]), ("None", -1))
        self.assertNotIn("animation.montage.profile.snapshot.v1", context.calls)

    def test_alias_precedence_produces_one_mutation_per_surface(self):
        body = {"Skeleton": "/Game/Animation/SK_Old", "PreviewMesh": "/Game/Animation/SK_OldMesh",
                "References": {"Skeleton": "/Game/Animation/SK_Combat"}, "Preview": {"PreviewMesh": None}}
        plan, _ = self.preflight(body)
        self.assertEqual([p["PrimitiveId"] for p in plan],
                         ["animation.asset.preview_mesh.set.v1", "animation.asset.skeleton.set.v1"])
        self.assertEqual(plan[0]["Intent"]["operation"], "clear")
        self.assertEqual(plan[1]["Intent"]["skeleton_path"], "/Game/Animation/SK_Combat.SK_Combat")
        context = self.apply(body)
        mutations = [id for id in context.calls if id.startswith("animation.")]
        self.assertEqual(mutations, [p["PrimitiveId"] for p in plan])
        self.assertFalse(any("SK_Old" in str(values) for _, values, _ in context.invocations))

    def test_notify_class_resolution_and_create_context_use_existing_handles(self):
        body = {key: observed_body()[key] for key in ("Notifies", "NotifyStates")}
        plan, context = self.preflight(body)
        classes = [v for p, v, _ in context.invocations if p == "editor.class.resolve_canonical.v1"]
        self.assertEqual(classes, [
            {"class": "/Script/Engine.AnimNotify", "expected_base": "/Script/Engine.AnimNotify"},
            {"class": "/Script/Engine.AnimNotifyState", "expected_base": "/Script/Engine.AnimNotifyState"}])
        self.assertEqual(len(plan), 1)
        create = MontageContext(exists=False)
        handles = self.profile.create_target_context(document({"Skeleton": "/Game/Animation/SK_Combat"}), create)
        self.assertEqual(tuple(h.name for h in handles), ("factory_context",))
        self.assertIs(handles[0].handle, create.asset_handle)
        steps, _ = self.preflight({"Skeleton": "/Game/Animation/SK_Combat"}, exists=False)
        self.assertEqual([row["PrimitiveId"] for row in steps], [
            "editor.asset.create_by_factory.v2", "animation.asset.skeleton.set.v1"])
        self.assertEqual([(row["Index"], row["Version"]) for row in steps], [(0, 2), (1, 1)])
        self.assertEqual(steps[0]["InputSchemaId"], "editor.asset.create_by_factory.v2.input.v1")
        self.assertEqual(steps[0]["Intent"], {
            "asset_class_path": EXACT_CLASS, "factory_context_path": "/Game/Animation/SK_Combat.SK_Combat",
            "package_path": TARGET.rsplit("/", 1)[0], "asset_name": TARGET.rsplit("/", 1)[1]})
        # Create still requires the Skeleton (the factory context).
        with self.assertRaises(AssetDocumentContractError):
            self.preflight({"Curves": []}, exists=False)

    def test_slot_the_skeleton_lacks_names_the_skeleton_and_its_slots(self):
        def refused(body, slots, exists, path, *texts):
            context = MontageContext(exists=exists)
            context.skeleton_slots = slots
            with self.assertRaises(AssetDocumentContractError) as caught:
                self.profile.preflight(document(body), context.asset_handle, context)
            diagnostic = caught.exception.diagnostic
            self.assertEqual((diagnostic.code, diagnostic.path), ("AnimMontage.UnknownSlot", path))
            for text in texts:
                self.assertIn(text, diagnostic.message)
            self.assertNotIn("editor.asset.create_by_factory.v2", context.calls)

        # A new montage keeps its native DefaultSlot track; the Skeleton has no slots.
        refused({"Skeleton": "/Game/Animation/SK_Combat"}, (), False, "/Body/SlotAnimTracks",
                "/Game/Animation/SK_Combat.SK_Combat", "'DefaultSlot'", "Its slots: none", "Body.SlotGroups")
        refused({"SlotAnimTracks": [{"SlotName": "Missing", "Segments": []}]}, ("DefaultSlot", "UpperBody"), True,
                "/Body/SlotAnimTracks/0/SlotName", "'Missing'", "Its slots: DefaultSlot, UpperBody")
        # A Skeleton change keeps the montage's tracks (DefaultSlot in the observed snapshot).
        refused({"Skeleton": "/Game/Animation/SK_Other"}, ("UpperBody",), True, "/Body/Skeleton",
                "/Game/Animation/SK_Other.SK_Other", "'DefaultSlot'")
        # An engine Skeleton cannot take the slot: the hint says to duplicate it.
        refused({"Skeleton": "/Engine/Tutorial/TutorialTPP_Skeleton"}, (), False, "/Body/SlotAnimTracks",
                "'DefaultSlot'", "read-only", "duplicate_asset", "CompatibleSkeletons")
        # Untouched slots and Skeleton: no Skeleton read.
        _, context = self.preflight({"Curves": []})
        self.assertNotIn("animation_asset.skeleton.read.v1", context.calls)

    def test_rich_create_plans_every_region_after_the_factory_step(self):
        body = dict(observed_body(), **observed_settings_body())
        body["TimeStretch"] = {"TimeStretchCurveName": "MontageTimeStretchCurve"}
        value = dict(document(body), Properties={"RateScale": 2})
        context = MontageContext(exists=False)
        steps = self.profile.preflight(value, context.asset_handle, context)
        self.assertEqual([row["PrimitiveId"] for row in steps], [
            "editor.asset.create_by_factory.v2", "animation.curve.float.replace.v2",
            "animation.asset.preview_mesh.set.v1", "animation.montage.notifies.replace.v1",
            "animation.asset.skeleton.set.v1", "editor.property.write_admitted_batch.v1",
            "animation.montage.settings.set.v1"])
        self.assertEqual([row["Index"] for row in steps], list(range(7)))
        # No target yet: admission comes from the class, never from a target handle.
        self.assertIn("editor.property.enumerate_admitted.v1", context.calls)
        self.assertNotIn("editor.property.enumerate_admitted_batch.v1", context.calls)
        self.assertNotIn("animation.montage.profile.snapshot.v1", context.calls)
        self.assertIn({"name": "RateScale", "value": 2.0}, steps[5]["Intent"]["properties"])

    def test_blocked_alias_payload_and_top_level_region_fail_before_invocation(self):
        for value in (document({"Preview": {"Extra": True}}),
                      document({"References": {"Other": "/Game/A"}}),
                      dict(document({}), Properties={"SyncGroup": "Owned"}),
                      dict(document({}), Definitions={"extra": {}})):
            probe = InvocationProbe()
            with self.subTest(value=value), self.assertRaises(AssetDocumentContractError):
                self.profile.preflight(value, None, probe)
            self.assertEqual(probe.calls, 0)

    def test_malformed_snapshot_cannot_be_extracted_as_desired(self):
        from asset_document.regions.primitive_catalog import PrimitiveOutcome, PrimitiveOutcomeStatus
        class Incomplete(AnimationReadOnlyContext):
            def invoke(self, **kwargs):
                return PrimitiveOutcome(PrimitiveOutcomeStatus.SUCCEEDED, {"curves": []}, (), (), ())
        context = Incomplete()
        with self.assertRaises(AssetDocumentContractError):
            self.profile.extract(target=TARGET, asset_handle=context.asset_handle, context=context)


class MontageLegacyDiffTests(unittest.TestCase):
    def test_mandatory_repair_phase_does_not_repeat_apply_mutation(self):
        from asset_document.profiles.anim_montage import AnimMontageProfileCandidate
        probe = InvocationProbe()
        self.assertEqual(AnimMontageProfileCandidate().repair(document({"Curves": []}), None, probe), ())
        self.assertEqual(probe.calls, 0)

    def test_legacy_read_only_rows_compare_without_granting_mutation(self):
        from asset_document.profiles.anim_montage import AnimMontageProfileCandidate
        profile = AnimMontageProfileCandidate()
        legacy = document({"Curves": [{"Name": "Speed", "Keys": []}],
                           "CompositeSections": [{"Name": "Start", "NextSectionName": None}]})
        self.assertEqual(profile.diff(legacy, legacy), ())
        probe = InvocationProbe()
        with self.assertRaises(AssetDocumentContractError):
            profile.preflight(legacy, None, probe)
        self.assertEqual(probe.calls, 0)

    def test_explicit_alias_container_remainder_is_compared(self):
        from asset_document.profiles.anim_montage import AnimMontageProfileCandidate
        profile = AnimMontageProfileCandidate()
        for key in ("References", "Preview"):
            with self.subTest(key=key):
                differences = profile.diff(document({key: {"Other": "A"}}), document({key: {"Other": "B"}}))
                self.assertEqual([item.path for item in differences], ["/Body/" + key])


class AnimMontageSettingsTests(unittest.TestCase):
    def setUp(self):
        from asset_document.profiles.anim_montage import AnimMontageProfileCandidate
        self.profile = AnimMontageProfileCandidate()

    def preflight(self, value, *, exists=True):
        context = MontageContext(exists=exists)
        return self.profile.preflight(value, context.asset_handle, context), context

    def apply(self, value):
        from asset_document.dto import RuntimePhase
        context = MontageContext(phase=RuntimePhase.APPLY)
        self.profile.apply(value, context.asset_handle, context.asset_handle, context)
        return context

    def extract(self):
        from asset_document.dto import RuntimePhase
        context = MontageContext(phase=RuntimePhase.VERIFY, settings=True)
        return self.profile.extract(target=TARGET, asset_handle=context.asset_handle, context=context)

    def test_settings_lower_to_editor_batch_then_settings_primitive(self):
        import struct
        body = {"Curves": [],
                "Blend": {"BlendInTime": 0.2, "BlendModeOut": "Inertialization", "BlendProfileIn": "Upper"},
                "Sync": {"SyncGroup": "Group", "SyncSlotIndex": 0},
                "RootMotion": {"RootMotionRootLock": "Zero"},
                "TimeStretch": {"SamplingRate": 30},
                "Metadata": [], "SectionMetadata": {"Start": []}}
        value = dict(document(body), Properties={"RateScale": 2, "bLoop": True})
        plan, context = self.preflight(value)
        self.assertEqual([step["PrimitiveId"] for step in plan], [
            "animation.curve.float.replace.v2", "editor.property.write_admitted_batch.v1",
            "animation.montage.settings.set.v1"])
        self.assertEqual([step["Index"] for step in plan], [0, 1, 2])
        self.assertEqual(plan[1]["InputSchemaId"], "editor.property.write_admitted_batch.v1.input.v1")
        rows = plan[1]["Intent"]["properties"]
        self.assertEqual(rows, [{"name": "BlendModeOut", "value": "Inertialization"}, {"name": "bLoop", "value": True},
                                {"name": "RateScale", "value": 2.0}, {"name": "SyncGroup", "value": "Group"},
                                {"name": "SyncSlotIndex", "value": 0}])
        self.assertIs(type(rows[2]["value"]), float)
        self.assertEqual(plan[2]["Intent"], {
            "blend_in": {"time": struct.unpack("!f", struct.pack("!f", 0.2))[0]},
            "blend_profile_in": "Upper", "root_motion": {"root_lock": "Zero"},
            "time_stretch": {"sampling_rate": 30.0}, "metadata": [],
            "section_metadata": [{"section": "Start", "metadata": []}]})
        self.assertIn("editor.property.enumerate_admitted_batch.v1", context.calls)
        applied = self.apply(value)
        mutations = [row for row in applied.invocations
                     if row[0] in [step["PrimitiveId"] for step in plan]]
        self.assertEqual([(row[0], row[1]) for row in mutations], [(step["PrimitiveId"], step["Intent"]) for step in plan])
        write = mutations[1]
        self.assertEqual([argument.name for argument in write[2]], ["asset", "property_batch"])
        self.assertIs(write[2][1].handle, applied.batch_handle)
        self.assertEqual([argument.name for argument in mutations[2][2]], ["asset"])
        # Plain fields alone still end with an empty settings step (derived refresh).
        plan, _ = self.preflight(dict(document({}), Properties={"RateScale": 2}))
        self.assertEqual([(step["PrimitiveId"], step["Intent"]) for step in plan], [
            ("editor.property.write_admitted_batch.v1", {"properties": [{"name": "RateScale", "value": 2.0}]}),
            ("animation.montage.settings.set.v1", {})])
        plan, _ = self.preflight(document({}))
        self.assertEqual(plan, [])

    def test_extract_projects_settings_and_properties_and_round_trips(self):
        actual = self.extract()
        expected = observed_body()
        expected.update(observed_settings_body())
        self.assertEqual(actual["Body"], expected)
        # Body-owned fields (SyncGroup, BlendModeIn) are not repeated in Properties.
        self.assertEqual(actual["Properties"], {"RateScale": 1.5, "bLoop": False})
        self.assertEqual(self.profile.diff(actual, actual), ())
        plan, _ = self.preflight(actual)
        self.assertEqual([step["PrimitiveId"] for step in plan][-2:], [
            "editor.property.write_admitted_batch.v1", "animation.montage.settings.set.v1"])
        settings = plan[-1]["Intent"]
        self.assertEqual(settings["blend_profile_out"], "Upper")
        self.assertEqual(settings["metadata"], [{"class": "/Script/Mod.MetaA", "properties": {"Label": "a", "Count": 2}}])

    def test_sparse_settings_diff_compares_authored_fields_at_float32(self):
        actual = self.extract()
        authored = dict(document({"Blend": {"BlendInTime": 0.25},
                                  "Metadata": [{"Class": "/Script/Mod.MetaA", "Properties": {"Label": "a"}}]}),
                        Properties={"RateScale": 1.5})
        self.assertEqual(self.profile.diff(authored, actual), ())
        for value, path in (
            (document({"Blend": {"BlendInTime": 0.3}}), "/Body/Blend"),
            (dict(document({}), Properties={"RateScale": 2}), "/Properties"),
            (document({"Metadata": [{"Class": "/Script/Mod.MetaA", "Properties": {"Label": "b"}}]}), "/Body/Metadata"),
            (document({"SectionMetadata": {"Start": [{"Class": "/Script/Mod.MetaA", "Properties": {}}]}}),
             "/Body/SectionMetadata"),
            (document({"RootMotion": {"bEnableRootMotionRotation": True}}), "/Body/RootMotion"),
        ):
            with self.subTest(path=path):
                self.assertEqual([item.path for item in self.profile.diff(value, actual)], [path])

    def test_cross_region_checks_and_property_ownership(self):
        curves = observed_body()["Curves"]
        slots = observed_body()["SlotAnimTracks"]
        for body, code, path in (
            ({"Curves": curves, "TimeStretch": {"TimeStretchCurveName": "Missing"}},
             "AnimMontage.MissingTimeStretchCurve", "/Body/TimeStretch/TimeStretchCurveName"),
            ({"SlotAnimTracks": slots, "Sync": {"SyncSlotIndex": 1}},
             "AnimMontage.InvalidSettings", "/Body/Sync/SyncSlotIndex"),
        ):
            with self.subTest(code=code):
                assert_error(self, code, path, lambda body=body: self.preflight(document(body)))
        # Keeping the native (engine default) name round-trips even without a curve.
        native = native_settings()
        native["time_stretch"]["curve_name"] = "MontageTimeStretchCurve"
        context = MontageContext(settings=native)
        kept = document({"Curves": curves, "TimeStretch": {"TimeStretchCurveName": "MontageTimeStretchCurve"}})
        self.profile.preflight(kept, context.asset_handle, context)
        # INDEX_NONE is a valid native SyncSlotIndex.
        self.preflight(document({"SlotAnimTracks": slots, "Sync": {"SyncSlotIndex": -1}}))
        assert_error(self, "AnimMontage.ManagedPropertyOverlap", "/Properties/BlendModeIn",
                     lambda: self.preflight(dict(document({}), Properties={"BlendModeIn": "Standard"})))
        with self.assertRaises(AssetDocumentContractError) as caught:
            self.preflight(dict(document({}), Properties={"NotAProperty": 1}))
        self.assertEqual(caught.exception.diagnostic.code, "AnimMontage.UnknownProperty")
        # Natively admitted references stay outside the Montage Properties region.
        assert_error(self, "AnimMontage.UnknownProperty", "/Properties/PreviewBasePose",
                     lambda: self.preflight(dict(document({}), Properties={"PreviewBasePose": None})))
        # A new Montage starts with the native default curve name, which may stay.
        created = self.preflight(document({"Skeleton": "/Game/SK", "Curves": curves, "Blend": {"BlendInTime": 0.1},
            "TimeStretch": {"TimeStretchCurveName": "MontageTimeStretchCurve"}}), exists=False)[0]
        self.assertEqual(created[0]["PrimitiveId"], "editor.asset.create_by_factory.v2")
        assert_error(self, "AnimMontage.MissingTimeStretchCurve", "/Body/TimeStretch/TimeStretchCurveName",
                     lambda: self.preflight(document({"Skeleton": "/Game/SK", "Curves": curves,
                         "TimeStretch": {"TimeStretchCurveName": "Other"}}), exists=False))
        # Omitted Curves on a new Montage means it has none: a changed name is rejected
        # before creation (preflight), and by Validate when the Action is Create.
        uncurved = document({"Skeleton": "/Game/SK", "TimeStretch": {"TimeStretchCurveName": "Speed"}})
        assert_error(self, "AnimMontage.MissingTimeStretchCurve", "/Body/TimeStretch/TimeStretchCurveName",
                     lambda: self.preflight(uncurved, exists=False))
        self.profile.validate(uncurved)  # CreateOrUpdate may target an existing Montage with curves
        assert_error(self, "AnimMontage.MissingTimeStretchCurve", "/Body/TimeStretch/TimeStretchCurveName",
                     lambda: self.profile.validate(dict(uncurved, Action="Create")))
        self.profile.validate(dict(document({"Skeleton": "/Game/SK", "Curves": curves,
            "TimeStretch": {"TimeStretchCurveName": "Speed"}}), Action="Create"))

    def test_malformed_native_settings_cannot_be_projected(self):
        from asset_document.profiles.anim_montage_settings import settings_snapshot_to_body
        broken = native_settings()
        del broken["properties"]
        with self.assertRaises(AssetDocumentContractError):
            settings_snapshot_to_body(broken)
        regions, properties = settings_snapshot_to_body(native_settings())
        self.assertEqual(regions, observed_settings_body())
        self.assertEqual(properties, {"RateScale": 1.5, "bLoop": False})


class AnimMontageTimelineAuthoringTests(unittest.TestCase):
    """NotifyTracks, the read-only timeline check and the StartPos tolerance."""

    def setUp(self):
        from asset_document.profiles.anim_montage import AnimMontageProfileCandidate
        self.profile = AnimMontageProfileCandidate()

    def preflight(self, body, *, exists=True):
        context = MontageContext(exists=exists)
        return self.profile.preflight(document(body), context.asset_handle, context), context

    @staticmethod
    def segment(start, source_end, rate=1.0):
        return {"AnimReference": "/Game/Animation/AS_Attack.AS_Attack", "StartPos": start, "AnimStartTime": 0.0,
                "AnimEndTime": source_end, "AnimPlayRate": rate, "LoopingCount": 1}

    def test_notify_tracks_travel_with_the_notifies_before_the_structure(self):
        tracks = [{"Name": "Danger"}, {"Name": "Hit"}]
        notify = observed_body()["Notifies"][0]
        plan, context = self.preflight({"NotifyTracks": tracks, "Notifies": [{**notify, "TrackIndex": 1}],
                                        "SlotAnimTracks": observed_body()["SlotAnimTracks"]})
        self.assertEqual([step["PrimitiveId"] for step in plan], ["animation.montage.notifies.replace.v1"])
        self.assertEqual(list(plan[0]["Intent"]), ["notifies", "notify_tracks", "structure"])
        self.assertEqual(plan[0]["Intent"]["notify_tracks"], ["Danger", "Hit"])
        check = [args for primitive, args, _ in context.invocations if primitive == "animation.montage.structure.check.v1"]
        self.assertEqual(len(check), 1)
        self.assertEqual(list(check[0]), ["slots", "notifies", "notify_tracks"])
        self.assertEqual(check[0]["notify_tracks"], ["Danger", "Hit"])

    def test_notify_tracks_rules(self):
        notify = observed_body()["Notifies"][0]
        assert_error(self, "AnimMontage.NotifyTracksNeedNotifies", "/Body/NotifyTracks",
                     lambda: self.profile.validate(document({"NotifyTracks": [{"Name": "1"}]})))
        assert_error(self, "AnimMontage.NotifyTrackRequired", "/Body/NotifyTracks",
                     lambda: self.profile.canonicalize(document({"NotifyTracks": [], "Notifies": []})))
        assert_error(self, "AnimMontage.NotifyTrackMissing", "/Body/Notifies/0/TrackIndex",
                     lambda: self.profile.validate(document({"NotifyTracks": [{"Name": "1"}], "Notifies": [{**notify, "TrackIndex": 1}]})))
        created = document({"Notifies": [{**notify, "TrackIndex": 1}]})
        created["Action"] = "Create"
        assert_error(self, "AnimMontage.NotifyTrackMissing", "/Body/Notifies/0/TrackIndex", lambda: self.profile.validate(created))
        created["Body"]["NotifyTracks"] = [{"Name": "1"}, {"Name": "Hit"}]
        self.profile.validate(created)
        # Names are labels (the editor's rename takes any text); TrackIndex is the position.
        self.profile.validate(document({"NotifyTracks": [{"Name": "A"}, {"Name": "A"}], "Notifies": []}))

    def test_extract_lists_the_editor_track_of_a_montage_without_tracks(self):
        from asset_document.profiles.anim_montage_codec import snapshot_to_document_parts
        from asset_document.tests.fixtures.anim_montage_cases import native_snapshot
        snapshot = native_snapshot()
        snapshot["notify_tracks"] = []
        self.assertEqual(snapshot_to_document_parts(snapshot)[0]["NotifyTracks"], [{"Name": "1"}])
        snapshot["notify_tracks"] = [{"name": "Danger"}, {"name": "Hit"}]
        self.assertEqual(snapshot_to_document_parts(snapshot)[0]["NotifyTracks"], [{"Name": "Danger"}, {"Name": "Hit"}])

    def test_no_check_for_settings_only_or_a_new_montage(self):
        _, context = self.preflight({"Curves": []})
        self.assertNotIn("animation.montage.structure.check.v1", context.calls)
        created = document({"Skeleton": "/Game/Animation/SK_Combat", "SlotAnimTracks": observed_body()["SlotAnimTracks"]})
        context = MontageContext(exists=False)
        self.profile.preflight(created, context.asset_handle, context)
        self.assertNotIn("animation.montage.structure.check.v1", context.calls)

    def test_segment_start_within_tolerance_is_no_change(self):
        observed = observed_body()
        live = [{"SlotName": "DefaultSlot", "Segments": [self.segment(0.0, 0.666, 0.595), self.segment(1.1193277, 0.5)]}]
        observed["SlotAnimTracks"] = live
        double = [{"SlotName": "DefaultSlot", "Segments": [self.segment(0.0, 0.666, 0.595), self.segment(0.666 / 0.595, 0.5)]}]
        self.assertEqual(self.profile.diff(document({"SlotAnimTracks": double}), document(observed)), ())
        gap = [{"SlotName": "DefaultSlot", "Segments": [self.segment(0.0, 0.666, 0.595), self.segment(1.1194, 0.5)]}]
        self.assertEqual([d.path for d in self.profile.diff(document({"SlotAnimTracks": gap}), document(observed))],
                         ["/Body/SlotAnimTracks"])


if __name__ == "__main__":
    unittest.main()
