// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetDocumentAnimationPrimitiveProvider.h"

#include "Animation/AnimCurveMetadata.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimSequence.h"
#include "AssetDocumentRuntimeModule.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "Provider/Asset/AssetDocumentAnimationAssetOperations.h"
#include "Provider/AssetDocumentAnimationPrimitiveCatalog.h"
#include "Provider/Montage/MontageStructuralProvider.h"
#include "Provider/Montage/MontageProfileSnapshot.h"
#include "Provider/Curve/AssetDocumentAnimationFloatCurveOperations.h"
#include "Provider/AssetDocumentAnimationProviderCommon.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Subtitles/SubtitlesAndClosedCaptionsDelegates.h"

namespace AssetDocumentAnimationProviderDescriptorTests
{
struct FExpectedDecision
{
	const TCHAR* PrimitiveId;
	EAssetDocumentAnimationDecisionDisposition Disposition;
	const TCHAR* ConsumerId;
	const TCHAR* AuthorityGateId;
	const TCHAR* StableBlockerDiagnostic;
};

struct FExpectedDescriptor
{
	const TCHAR* PrimitiveId;
	const TCHAR* DomainDiagnostic;
	const TCHAR* PublicModule;
	const TCHAR* PublicHeader;
	const TCHAR* ExportedAuthoritativeApi;
	const TCHAR* ResultName;
	EAssetDocumentHandleKind ResultKind;
};

constexpr const TCHAR* NoStableBlockerDiagnostic = TEXT("");
constexpr const TCHAR* AnimationProfileConsumer = TEXT("animation_profile");
constexpr const TCHAR* AnimationProviderDesignConsumer =
	TEXT("animation_provider_design");

const FExpectedDecision ExpectedDecisions[] = {
	{TEXT("animation.asset.preview_mesh.set.v1"), EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("T0-ASSET"), NoStableBlockerDiagnostic},
	{TEXT("animation.sequence.retarget_source_asset.set.v1"), EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("T0-ASSET"), TEXT("AD.PANIM.BLOCK.RETARGET_SOURCE_AUTHORITY")},
    {TEXT("animation.sequence.retarget_source_asset.set.v2"), EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("T0-ASSET"), NoStableBlockerDiagnostic},
    {TEXT("animation.sequence.retarget_source_name.set.v1"), EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("T0-ASSET"), NoStableBlockerDiagnostic},
    {TEXT("animation.sequence.compression.settings.set.v1"), EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("T0-SEQUENCE"), NoStableBlockerDiagnostic},
	{TEXT("animation.curve.float.enumerate.v1"), EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("T0-DATA"), NoStableBlockerDiagnostic},
	{TEXT("animation.curve.float.enumerate.v2"), EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("T0-DATA"), NoStableBlockerDiagnostic},
	{TEXT("animation.curve.float.replace.v2"), EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("C0-EXACT"), NoStableBlockerDiagnostic},
	{TEXT("animation.curve.float.replace.v1"), EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("T0-DATA"), TEXT("AD.PANIM.BLOCK.ROLLBACK_COMPOSITION")},
	{TEXT("animation.notify_track.enumerate.v1"), EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("T0-TIMELINE"), TEXT("AD.PANIM.BLOCK.NOTIFY_LIBRARY_DEPENDENCY")},
	{TEXT("animation.notify_track.replace.v1"), EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("T0-TIMELINE"), TEXT("AD.PANIM.BLOCK.NOTIFY_LIBRARY_DEPENDENCY")},
	{TEXT("animation.notify_event.enumerate.v1"), EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("T0-TIMELINE"), TEXT("AD.PANIM.BLOCK.NOTIFY_LIBRARY_DEPENDENCY")},
	{TEXT("animation.notify_event.remove.v1"), EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("T0-TIMELINE"), TEXT("AD.PANIM.BLOCK.RUNTIME_HANDLE_BOOTSTRAP")},
	{TEXT("animation.notify_event.add.v1"), EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("T0-TIMELINE"), TEXT("AD.PANIM.BLOCK.RUNTIME_HANDLE_BOOTSTRAP")},
	{TEXT("animation.sync_marker.enumerate.v1"), EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("T0-TIMELINE"), TEXT("AD.PANIM.BLOCK.NOTIFY_LIBRARY_DEPENDENCY")},
	{TEXT("animation.sync_marker.replace.v1"), EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("T0-TIMELINE"), TEXT("AD.PANIM.BLOCK.NOTIFY_LIBRARY_DEPENDENCY")},
	{TEXT("animation.metadata.enumerate.v1"), EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("T0-METADATA"), NoStableBlockerDiagnostic},
	{TEXT("animation.asset_metadata.replace.v1"), EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("T0-METADATA"), TEXT("AD.PANIM.BLOCK.RUNTIME_HANDLE_BOOTSTRAP")},
	{TEXT("animation.montage.section_metadata.replace.v1"), EAssetDocumentAnimationDecisionDisposition::FrozenG1StableBlocker, AnimationProviderDesignConsumer, TEXT("G1"), TEXT("AD.G1.BLOCK.ANIMATION.SECTION_METADATA_AUTHORITY")},
	{TEXT("animation.asset_user_data.enumerate.v1"), EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("T0-METADATA"), NoStableBlockerDiagnostic},
	{TEXT("animation.asset_user_data.replace_managed.v1"), EAssetDocumentAnimationDecisionDisposition::FrozenG1StableBlocker, AnimationProviderDesignConsumer, TEXT("G1"), TEXT("AD.G1.BLOCK.ANIMATION.ASSET_USER_DATA_LIFECYCLE")},
	{TEXT("animation.sequence.complete_edit.v1"), EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("T0-SEQUENCE"), TEXT("AD.PANIM.BLOCK.SEQUENCE_COMPLETION_AUTHORITY")},
    {TEXT("animation.sequence.complete_edit.v2"), EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("T0-SEQUENCE"), NoStableBlockerDiagnostic},
	{TEXT("animation.montage.slot_tracks.replace.v1"), EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("MONTAGE-STRUCTURAL"), NoStableBlockerDiagnostic},
	{TEXT("animation.montage.sections.mutate.v1"), EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("MONTAGE-STRUCTURAL"), NoStableBlockerDiagnostic},
	{TEXT("animation.montage.profile.snapshot.v1"), EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("M5-PROFILE"), NoStableBlockerDiagnostic},
	{TEXT("animation.montage.sync_cache.complete.v1"), EAssetDocumentAnimationDecisionDisposition::FrozenG1StableBlocker, AnimationProviderDesignConsumer, TEXT("G1"), TEXT("AD.G1.BLOCK.ANIMATION.SYNC_CACHE_AUTHORITY")},
	{TEXT("animation.montage.time_stretch.configure_bake.v1"), EAssetDocumentAnimationDecisionDisposition::FrozenG1StableBlocker, AnimationProviderDesignConsumer, TEXT("G1"), TEXT("AD.G1.BLOCK.ANIMATION.TIME_STRETCH_CONFIG_AUTHORITY")},
	{TEXT("animation.linked_anim_layer.action.enumerate.v1"), EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("T0-GRAPH"), TEXT("AD.PANIM.BLOCK.RUNTIME_HANDLE_BOOTSTRAP")},
	{TEXT("animation.anim_graph.lifecycle.v1"), EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("T0-GRAPH"), TEXT("AD.PANIM.BLOCK.RUNTIME_HANDLE_BOOTSTRAP")},
	{TEXT("animation.state_machine.node.create.v1"), EAssetDocumentAnimationDecisionDisposition::FrozenG1StableBlocker, AnimationProviderDesignConsumer, TEXT("G1"), TEXT("AD.G1.BLOCK.ANIMATION.STATE_MACHINE_NODE_AUTHORITY")},
	{TEXT("animation.linked_anim_layer.edit.v1"), EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("T0-GRAPH"), TEXT("AD.PANIM.BLOCK.RUNTIME_HANDLE_BOOTSTRAP")},
	{TEXT("animation.anim_parent_override.replace.v1"), EAssetDocumentAnimationDecisionDisposition::FrozenG1StableBlocker, AnimationProviderDesignConsumer, TEXT("G1"), TEXT("AD.G1.BLOCK.ANIMATION.PARENT_OVERRIDE_LIFECYCLE")},
	{TEXT("animation.state_transition.connect.v1"), EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("T0-GRAPH"), TEXT("AD.PANIM.BLOCK.RUNTIME_HANDLE_BOOTSTRAP")},
	{TEXT("animation.asset.skeleton.set.v1"), EAssetDocumentAnimationDecisionDisposition::Admitted, AnimationProfileConsumer, TEXT("T0-ASSET"), NoStableBlockerDiagnostic},
	{TEXT("animation.state_entry.connect.v1"), EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker, AnimationProfileConsumer, TEXT("T0-GRAPH"), TEXT("AD.PANIM.BLOCK.RUNTIME_HANDLE_BOOTSTRAP")},
};

const FExpectedDescriptor ExpectedDescriptors[] = {
	{TEXT("animation.curve.float.enumerate.v1"), TEXT("AnimationProvider.Curve.DataModelUnavailable"), TEXT("Engine"), TEXT("Animation/AnimData/IAnimationDataModel.h;Animation/AnimSequenceBase.h"), TEXT("UAnimSequenceBase::GetDataModel;IAnimationDataModel::GetFloatCurves"), TEXT("curves"), EAssetDocumentHandleKind::Curve},
	{TEXT("animation.metadata.enumerate.v1"), TEXT("AnimationProvider.Metadata.EnumerationFailed"), TEXT("Engine"), TEXT("Animation/AnimationAsset.h"), TEXT("UAnimationAsset::GetMetaData"), TEXT("metadata"), EAssetDocumentHandleKind::Subobject},
	{TEXT("animation.asset_user_data.enumerate.v1"), TEXT("AnimationProvider.AssetUserData.EnumerationFailed"), TEXT("Engine"), TEXT("Animation/AnimationAsset.h"), TEXT("UAnimationAsset::GetAssetUserDataArray"), TEXT("user_data"), EAssetDocumentHandleKind::Subobject},
};

const FAssetDocumentPrimitiveDescriptor* FindExpectedDescriptor(
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors,
	FStringView PrimitiveId)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
	{
		if (Descriptor.PrimitiveId == PrimitiveId)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

int32 CountCallableBindings(
	TConstArrayView<FAssetDocumentAnimationPrimitiveBinding> Bindings,
	FStringView PrimitiveId)
{
	int32 Count = 0;
	for (const FAssetDocumentAnimationPrimitiveBinding& Binding : Bindings)
	{
		if (Binding.PrimitiveId == PrimitiveId && Binding.Version == ((PrimitiveId == TEXTVIEW("animation.curve.float.enumerate.v2")
            || PrimitiveId == TEXTVIEW("animation.curve.float.replace.v2")
            || PrimitiveId == TEXTVIEW("animation.sequence.complete_edit.v2")
            || PrimitiveId == TEXTVIEW("animation.sequence.retarget_source_asset.set.v2")) ? 2u : 1u)
			&& Binding.Handler != nullptr)
		{
			++Count;
		}
	}
	return Count;
}

void AssertReadOnlyAssetArgument(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	Test.TestEqual(TEXT("descriptor has one input handle"),
		Descriptor.HandleArgumentSchemas.Num(), 1);
	if (Descriptor.HandleArgumentSchemas.Num() != 1)
	{
		return;
	}
	const FAssetDocumentPrimitiveHandleArgumentSchema& Asset =
		Descriptor.HandleArgumentSchemas[0];
	Test.TestEqual(TEXT("input handle name is asset"), Asset.Name, FString(TEXT("asset")));
	Test.TestEqual(TEXT("input handle is an Asset"), Asset.Kind,
		EAssetDocumentHandleKind::Asset);
	Test.TestEqual(TEXT("input handle is read-only"), Asset.RequiredAccess,
		EAssetDocumentHandleAccess::ReadOnly);
	Test.TestTrue(TEXT("input handle is required"), Asset.bRequired);
}

void AssertReadOnlyResultSchema(
	FAutomationTestBase& Test,
	const FExpectedDescriptor& Expected,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	Test.TestEqual(*FString::Printf(TEXT("%s has one result handle family"),
		Expected.PrimitiveId), Descriptor.HandleResultSchemas.Num(), 1);
	if (Descriptor.HandleResultSchemas.Num() != 1)
	{
		return;
	}
	const FAssetDocumentPrimitiveHandleResultSchema& Result =
		Descriptor.HandleResultSchemas[0];
	Test.TestEqual(*FString::Printf(TEXT("%s result name"), Expected.PrimitiveId),
		Result.Name, FString(Expected.ResultName));
	Test.TestEqual(*FString::Printf(TEXT("%s result kind"), Expected.PrimitiveId),
		Result.Kind, Expected.ResultKind);
	Test.TestEqual(*FString::Printf(TEXT("%s result access"), Expected.PrimitiveId),
		Result.GrantedAccess, EAssetDocumentHandleAccess::ReadOnly);
	Test.TestEqual(*FString::Printf(TEXT("%s result cardinality"), Expected.PrimitiveId),
		Result.Cardinality, EAssetDocumentHandleResultCardinality::ZeroOrMore);
}

void AssertCompleteReadOnlyAdmissionEvidence(
	FAutomationTestBase& Test,
	const FExpectedDescriptor& Expected,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FAssetDocumentPrimitiveAdmissionEvidence& Admission = Descriptor.Admission;
	Test.TestEqual(*FString::Printf(TEXT("%s has ordinary helper admission"),
		Expected.PrimitiveId), Admission.Disposition,
		EAssetDocumentPrimitiveDisposition::OrdinaryHelper);
	Test.TestEqual(*FString::Printf(TEXT("%s uses an exported helper"),
		Expected.PrimitiveId), Admission.AuthorityPath,
		EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper);
	Test.TestEqual(*FString::Printf(TEXT("%s admission owns Animation"),
		Expected.PrimitiveId), Admission.PhysicalOwner,
		EAssetDocumentPrimitiveOwner::Animation);
	Test.TestEqual(*FString::Printf(TEXT("%s admission freezes Engine version"),
		Expected.PrimitiveId), Admission.ExactEngineVersion, FString(TEXT("5.7.4")));
	Test.TestEqual(*FString::Printf(TEXT("%s admission module"), Expected.PrimitiveId),
		Admission.PublicModule, FString(Expected.PublicModule));
	Test.TestEqual(*FString::Printf(TEXT("%s admission header"), Expected.PrimitiveId),
		Admission.PublicHeader, FString(Expected.PublicHeader));
	Test.TestEqual(*FString::Printf(TEXT("%s admission API"), Expected.PrimitiveId),
		Admission.ExportedAuthoritativeApi, FString(Expected.ExportedAuthoritativeApi));
	Test.TestEqual(*FString::Printf(TEXT("%s operation ID"), Expected.PrimitiveId),
		Admission.PrimaryOperationId, FString(Expected.PrimitiveId));
	Test.TestFalse(*FString::Printf(TEXT("%s admission context is explicit"),
		Expected.PrimitiveId), Admission.AuthorityContextId.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s identity lifetime is explicit"),
		Expected.PrimitiveId), Admission.IdentityLifetimeContractId.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s read-only transaction is explicit"),
		Expected.PrimitiveId), Admission.TransactionContractId.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s read-only rollback is explicit"),
		Expected.PrimitiveId), Admission.RollbackContractId.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s completion is explicit"),
		Expected.PrimitiveId), Admission.CompletionContractId.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s save reload boundary is explicit"),
		Expected.PrimitiveId), Admission.SaveReloadEvidenceId.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s failure injection is explicit"),
		Expected.PrimitiveId), Admission.FailureInjectionTestIds.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s verification is explicit"),
		Expected.PrimitiveId), Admission.VerificationTestIds.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s authority fingerprint is explicit"),
		Expected.PrimitiveId), Admission.AuthorityFingerprint.IsEmpty());
	Test.TestEqual(*FString::Printf(TEXT("%s admission fingerprint"),
		Expected.PrimitiveId), Admission.AuthorityFingerprint,
		FString::Printf(TEXT("ue-5.7.4|animation|Engine|%s|%s|%s"),
			Expected.PublicHeader, Expected.ExportedAuthoritativeApi,
			Expected.PrimitiveId));
}

void AssertReadOnlyDescriptor(
	FAutomationTestBase& Test,
	const FExpectedDescriptor& Expected,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	Test.TestTrue(*FString::Printf(TEXT("%s validates"), Expected.PrimitiveId),
		ValidateAssetDocumentPrimitiveDescriptor(Descriptor).bValid);
	Test.TestEqual(*FString::Printf(TEXT("%s version"), Expected.PrimitiveId),
		Descriptor.Version, 1u);
	Test.TestEqual(*FString::Printf(TEXT("%s provider"), Expected.PrimitiveId),
		Descriptor.ProviderId, FString(TEXT("animation")));
	Test.TestEqual(*FString::Printf(TEXT("%s owner"), Expected.PrimitiveId),
		Descriptor.Owner, EAssetDocumentPrimitiveOwner::Animation);
	Test.TestEqual(*FString::Printf(TEXT("%s game thread"), Expected.PrimitiveId),
		Descriptor.ExecutionThread, EAssetDocumentExecutionThread::GameThread);
	Test.TestEqual(*FString::Printf(TEXT("%s access"), Expected.PrimitiveId),
		Descriptor.Access, EAssetDocumentPrimitiveAccess::ReadOnly);
	Test.TestEqual(*FString::Printf(TEXT("%s verify-only phase"), Expected.PrimitiveId),
		Descriptor.AllowedPhases,
		TArray<EAssetDocumentRuntimeRequestPhase>{EAssetDocumentRuntimeRequestPhase::Verify});
	Test.TestEqual(*FString::Printf(TEXT("%s allowed handle kinds"), Expected.PrimitiveId),
		Descriptor.AllowedHandleKinds,
		TArray<EAssetDocumentHandleKind>{EAssetDocumentHandleKind::Asset, Expected.ResultKind});
	Test.TestFalse(*FString::Printf(TEXT("%s needs no transaction"), Expected.PrimitiveId),
		Descriptor.bRequiresActiveTransaction);
	Test.TestTrue(*FString::Printf(TEXT("%s has no mutation footprint"), Expected.PrimitiveId),
		Descriptor.MutationFootprintSchemaId.IsEmpty());
	Test.TestEqual(*FString::Printf(TEXT("%s input DTO"), Expected.PrimitiveId),
		Descriptor.InputSchemaId, FString(Expected.PrimitiveId) + TEXT(".input.v1"));
	Test.TestEqual(*FString::Printf(TEXT("%s output DTO"), Expected.PrimitiveId),
		Descriptor.OutputSchemaId, FString(Expected.PrimitiveId) + TEXT(".output.v1"));
	Test.TestEqual(*FString::Printf(TEXT("%s consumer"), Expected.PrimitiveId),
		Descriptor.RequiredConsumerIds, TArray<FString>{AnimationProfileConsumer});
	Test.TestTrue(*FString::Printf(TEXT("%s invalid value diagnostic"), Expected.PrimitiveId),
		Descriptor.StableDiagnosticCodes.Contains(TEXT("AnimationProvider.InvalidValueShape")));
	Test.TestTrue(*FString::Printf(TEXT("%s invalid handle diagnostic"), Expected.PrimitiveId),
		Descriptor.StableDiagnosticCodes.Contains(TEXT("AnimationProvider.InvalidHandlePayload")));
	Test.TestTrue(*FString::Printf(TEXT("%s target mismatch diagnostic"), Expected.PrimitiveId),
		Descriptor.StableDiagnosticCodes.Contains(TEXT("AnimationProvider.TargetClassMismatch")));
	Test.TestTrue(*FString::Printf(TEXT("%s authority diagnostic"), Expected.PrimitiveId),
		Descriptor.StableDiagnosticCodes.Contains(TEXT("AnimationProvider.AuthorityUnavailable")));
	Test.TestTrue(*FString::Printf(TEXT("%s operation diagnostic"), Expected.PrimitiveId),
		Descriptor.StableDiagnosticCodes.Contains(TEXT("AnimationProvider.OperationRejected")));
	Test.TestTrue(*FString::Printf(TEXT("%s readback diagnostic"), Expected.PrimitiveId),
		Descriptor.StableDiagnosticCodes.Contains(TEXT("AnimationProvider.ReadbackMismatch")));
	Test.TestTrue(*FString::Printf(TEXT("%s domain diagnostic"), Expected.PrimitiveId),
		Descriptor.StableDiagnosticCodes.Contains(Expected.DomainDiagnostic));
	AssertReadOnlyAssetArgument(Test, Descriptor);
	AssertReadOnlyResultSchema(Test, Expected, Descriptor);
	AssertCompleteReadOnlyAdmissionEvidence(Test, Expected, Descriptor);
}

class FEnumerationHandleResolver final : public IAssetDocumentHandleResolver
{
public:
	explicit FEnumerationHandleResolver(
		const FAssetDocumentPrimitiveInvocation& InInvocation)
		: Invocation(InInvocation)
	{
	}

	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
	{
		++IssueCount;
		FAssetDocumentIssuedHandleResult Result;
		if (bRejectIssue || !Payload || !Payload->IsAlive())
		{
			FAssetDocumentRuntimeDiagnostic& Diagnostic =
				Result.Diagnostics.AddDefaulted_GetRef();
			Diagnostic.Path = TEXT("/Test/Issue");
			Diagnostic.Code = TEXT("TestHandle.IssueRejected");
			Diagnostic.Message = TEXT("The test resolver rejected child-handle issuance.");
			return Result;
		}
		FAssetDocumentOpaqueHandle Handle;
		Handle.RequestId = Invocation.RequestId;
		Handle.TransactionId = Invocation.TransactionId;
		Handle.RuntimeRevision = Invocation.RuntimeRevision;
		Handle.ProviderId = TEXT("animation");
		Handle.Slot = NextSlot++;
		Handle.Generation = 1;
		Handle.Kind = Kind;
		Handle.Access = Access;
		IssuedHandles.Add(Handle);
		Result.Handle = Handle;
		return Result;
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle&,
		const FAssetDocumentHandleValidationContext&) override
	{
		FAssetDocumentVerifiedLeaseResult Result;
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/Test/Acquire");
		++AcquireCount;
		Diagnostic.Code = TEXT("TestHandle.UnexpectedAcquire");
		Diagnostic.Message = TEXT("Pure asset observation must not acquire a second lease.");
		return Result;
	}

	virtual FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation&) override
	{
		++InvalidationCount;
		return {};
	}

	void RejectIssue() { bRejectIssue = true; }
	int32 GetIssueCount() const { return IssueCount; }
	int32 GetAcquireCount() const { return AcquireCount; }
	int32 GetInvalidationCount() const { return InvalidationCount; }
	const TArray<FAssetDocumentOpaqueHandle>& GetIssuedHandles() const
	{
		return IssuedHandles;
	}

private:
	FAssetDocumentPrimitiveInvocation Invocation;
	TArray<FAssetDocumentOpaqueHandle> IssuedHandles;
	uint32 NextSlot = 1;
	int32 IssueCount = 0;
	int32 AcquireCount = 0;
	int32 InvalidationCount = 0;
	bool bRejectIssue = false;
};

FAssetDocumentPrimitiveInvocation MakeMetadataInvocation(FStringView PrimitiveId)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = PrimitiveId;
	Invocation.Version = 1;
	Invocation.RequestId = FGuid::NewGuid();
	Invocation.TransactionId = FGuid::NewGuid();
	Invocation.RuntimeRevision = 17;
	Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
	Invocation.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
	return Invocation;
}

const FAssetDocumentRuntimeValueDto* FindObjectField(
	const FAssetDocumentRuntimeValueDto& Object,
	FStringView FieldName)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return nullptr;
	}
	for (int32 Index = 0; Index < Object.ObjectKeys.Num(); ++Index)
	{
		if (Object.ObjectKeys[Index] == FieldName)
		{
			return &Object.Elements[Index];
		}
	}
	return nullptr;
}

bool HasDiagnostic(
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics,
	FStringView Code)
{
	return Diagnostics.ContainsByPredicate(
		[Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code;
		});
}

void AssertT01IdentityRow(
	FAutomationTestBase& Test,
	const FAssetDocumentRuntimeValueDto& Row,
	const UObject& ExpectedObject)
{
	Test.TestEqual(TEXT("enumerated row is a T01 object"), Row.Kind,
		EAssetDocumentRuntimeValueKind::Object);
	Test.TestEqual(TEXT("enumerated row has exactly the frozen identity fields"),
		Row.ObjectKeys,
		TArray<FString>{TEXT("stable_id"), TEXT("class_path"), TEXT("name")});
	Test.TestEqual(TEXT("enumerated row has three identity values"), Row.Elements.Num(), 3);
	if (Row.Elements.Num() != 3)
	{
		return;
	}
	Test.TestEqual(TEXT("enumerated stable ID is the authoritative object path"),
		Row.Elements[0].StringValue, ExpectedObject.GetPathName());
	Test.TestEqual(TEXT("enumerated class path is authoritative"),
		Row.Elements[1].StringValue, ExpectedObject.GetClass()->GetPathName());
	Test.TestEqual(TEXT("enumerated object name is authoritative"),
		Row.Elements[2].StringValue, ExpectedObject.GetName());
}

void AssertReadOnlyEnumeration(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const FEnumerationHandleResolver& Handles,
	FStringView ResultName,
	TConstArrayView<UObject*> ExpectedObjects)
{
	Test.TestEqual(TEXT("enumeration succeeds"), Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	Test.TestTrue(TEXT("enumeration records no mutation"), Outcome.ChangedSet.IsEmpty());
	Test.TestTrue(TEXT("enumeration emits no diagnostics"), Outcome.Diagnostics.IsEmpty());
	Test.TestEqual(TEXT("enumeration issues one request-local child handle per row"),
		Handles.GetIssueCount(), ExpectedObjects.Num());
	Test.TestEqual(TEXT("enumeration performs no invalidation"),
		Handles.GetInvalidationCount(), 0);
	Test.TestEqual(TEXT("enumeration returns one handle result family"),
		Outcome.HandleResults.Num(), 1);
	if (Outcome.HandleResults.Num() != 1)
	{
		return;
	}
	const FAssetDocumentPrimitiveHandleResult& Result = Outcome.HandleResults[0];
	Test.TestEqual(TEXT("enumeration result family is exact"), Result.Name,
		FString(ResultName));
	Test.TestEqual(TEXT("enumeration returns one child handle per row"),
		Result.Handles.Num(), ExpectedObjects.Num());
	for (int32 Index = 0; Index < Result.Handles.Num()
		&& Index < Handles.GetIssuedHandles().Num(); ++Index)
	{
		const FAssetDocumentOpaqueHandle& Handle = Result.Handles[Index];
		const FAssetDocumentOpaqueHandle& Issued = Handles.GetIssuedHandles()[Index];
		Test.TestEqual(TEXT("result uses the issued child-handle slot"), Handle.Slot,
			Issued.Slot);
		Test.TestEqual(TEXT("result uses the issued child-handle generation"),
			Handle.Generation, Issued.Generation);
		Test.TestEqual(TEXT("child handle request is request-local"), Handle.RequestId,
			Invocation.RequestId);
		Test.TestEqual(TEXT("child handle transaction is request-local"), Handle.TransactionId,
			Invocation.TransactionId);
		Test.TestEqual(TEXT("child handle revision is request-local"), Handle.RuntimeRevision,
			Invocation.RuntimeRevision);
		Test.TestEqual(TEXT("child handle provider is animation"), Handle.ProviderId,
			FString(TEXT("animation")));
		Test.TestEqual(TEXT("child handle kind is Subobject"), Handle.Kind,
			EAssetDocumentHandleKind::Subobject);
		Test.TestEqual(TEXT("child handle access is read-only"), Handle.Access,
			EAssetDocumentHandleAccess::ReadOnly);
	}
	const FAssetDocumentRuntimeValueDto* Rows = FindObjectField(Outcome.Value, ResultName);
	Test.TestNotNull(TEXT("enumeration T01 output has the exact array field"), Rows);
	if (Rows == nullptr)
	{
		return;
	}
	Test.TestEqual(TEXT("enumeration output field is an array"), Rows->Kind,
		EAssetDocumentRuntimeValueKind::Array);
	Test.TestEqual(TEXT("enumeration output preserves authoritative array length"),
		Rows->Elements.Num(), ExpectedObjects.Num());
	for (int32 Index = 0; Index < Rows->Elements.Num()
		&& Index < ExpectedObjects.Num(); ++Index)
	{
		if (ExpectedObjects[Index] != nullptr)
		{
			AssertT01IdentityRow(Test, Rows->Elements[Index], *ExpectedObjects[Index]);
		}
	}
}

void AssertStableBlocker(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	IAssetDocumentHandleResolver& Handles,
	FStringView PrimitiveId,
	FStringView Diagnostic)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = PrimitiveId;
	Invocation.Version = 1;
	const FAssetDocumentPrimitiveOutcome Outcome = Provider.Invoke(
		Invocation, Handles, nullptr);
	const FString Primitive(PrimitiveId);
	const FString NonCallableMessage = Primitive + TEXT(" remains non-callable");
	const FString BlockerMessage = Primitive + TEXT(" retains its exact blocker");
	Test.TestEqual(*NonCallableMessage,
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestTrue(*BlockerMessage,
		HasDiagnostic(Outcome.Diagnostics, Diagnostic));
}
} // namespace AssetDocumentAnimationProviderDescriptorTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAnimationProviderDescriptorsTest,
	"UEAssetDoc.AnimationProvider.Descriptors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationProviderDescriptorsTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimationProviderDescriptorTests;
	const TConstArrayView<FAssetDocumentAnimationDecision> Decisions =
		GetAssetDocumentAnimationDecisions();
	const TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors =
		GetAssetDocumentAnimationPrimitiveDescriptors();
	const TConstArrayView<FAssetDocumentAnimationPrimitiveBinding> Bindings =
		GetAssetDocumentAnimationPrimitiveBindings();

	const auto ActualProjection=ValidateAnimationDescriptorBindingBijection(Decisions, Descriptors, Bindings);
	for (const auto& D : ActualProjection.Diagnostics) AddInfo(D.Code+TEXT(": ")+D.Message);
	if (!ActualProjection.bValid)
		for (const auto& D : Descriptors)
		{
			const auto Admission=ValidateAnimationAdmissionEvidence(D);
			if (!Admission.bValid) AddInfo(TEXT("Actual admission rejected: ")+D.PrimitiveId);
		}
	TestTrue(TEXT("actual admitted projection closes before any invocation"), ActualProjection.bValid);
	TestEqual(TEXT("real Animation provider is callable with the admitted catalog"),
		CreateAssetDocumentAnimationPrimitiveProvider()->InspectAvailability().State,
		EAssetDocumentProviderAvailabilityState::Available);

	TestEqual(TEXT("existing catalog plus supplied compression leaf has thirty-six rows"), Decisions.Num(),
		static_cast<int32>(UE_ARRAY_COUNT(ExpectedDecisions)));
	int32 AdmittedCount = 0;
	int32 OrdinaryStableBlockerCount = 0;
	int32 FrozenG1StableBlockerCount = 0;
	for (int32 Index = 0; Index < Decisions.Num()
		&& Index < UE_ARRAY_COUNT(ExpectedDecisions); ++Index)
	{
		const FAssetDocumentAnimationDecision& Actual = Decisions[Index];
		const FExpectedDecision& Expected = ExpectedDecisions[Index];
		TestEqual(TEXT("decision catalog order is immutable"), Actual.PrimitiveId,
			FString(Expected.PrimitiveId));
		TestEqual(TEXT("decision catalog exact version"), Actual.Version,
			(Actual.PrimitiveId == TEXT("animation.curve.float.enumerate.v2")
                || Actual.PrimitiveId == TEXT("animation.curve.float.replace.v2")
                || Actual.PrimitiveId == TEXT("animation.sequence.complete_edit.v2")
                || Actual.PrimitiveId == TEXT("animation.sequence.retarget_source_asset.set.v2")) ? 2u : 1u);
		TestEqual(TEXT("decision catalog disposition is immutable"), Actual.Disposition,
			Expected.Disposition);
		TestEqual(TEXT("decision consumer is immutable"), Actual.ConsumerId,
			FString(Expected.ConsumerId));
		TestEqual(TEXT("decision authority gate is immutable"), Actual.AuthorityGateId,
			FString(Expected.AuthorityGateId));
		TestEqual(TEXT("decision blocker diagnostic is immutable"),
			Actual.StableBlockerDiagnostic,
			FString(Expected.StableBlockerDiagnostic));

		switch (Actual.Disposition)
		{
		case EAssetDocumentAnimationDecisionDisposition::Admitted:
			++AdmittedCount;
			break;
		case EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker:
			++OrdinaryStableBlockerCount;
			break;
		case EAssetDocumentAnimationDecisionDisposition::FrozenG1StableBlocker:
			++FrozenG1StableBlockerCount;
			break;
		}
	}
	TestEqual(TEXT("existing thirteen operations and supplied compression leaf are admitted"), AdmittedCount, 14);
	TestEqual(TEXT("sixteen ordinary rows remain stable blockers"),
		OrdinaryStableBlockerCount, 16);
	TestEqual(TEXT("six frozen G1 rows remain stable blockers"),
		FrozenG1StableBlockerCount, 6);
	TestEqual(TEXT("only admitted decisions produce descriptors"), Descriptors.Num(), 14);
	TestEqual(TEXT("only admitted decisions produce bindings"), Bindings.Num(), 14);

	for (const FExpectedDescriptor& Expected : ExpectedDescriptors)
	{
		const FAssetDocumentPrimitiveDescriptor* Descriptor =
			FindExpectedDescriptor(Descriptors, Expected.PrimitiveId);
		TestNotNull(*FString::Printf(TEXT("%s descriptor is admitted"),
			Expected.PrimitiveId), Descriptor);
		if (Descriptor != nullptr)
		{
			AssertReadOnlyDescriptor(*this, Expected, *Descriptor);

			FAssetDocumentPrimitiveDescriptor StaleAdmission = *Descriptor;
			StaleAdmission.Admission.ExactEngineVersion = TEXT("5.7.3");
			TestFalse(*FString::Printf(TEXT("%s rejects stale Engine admission"),
				Expected.PrimitiveId),
				ValidateAnimationAdmissionEvidence(StaleAdmission).bValid);
		}
		TestEqual(*FString::Printf(TEXT("%s has exactly one binding"),
			Expected.PrimitiveId), CountCallableBindings(Bindings, Expected.PrimitiveId), 1);
	}

	for (const FExpectedDecision& Expected : ExpectedDecisions)
	{
		if (Expected.Disposition != EAssetDocumentAnimationDecisionDisposition::Admitted)
		{
			TestNull(*FString::Printf(TEXT("%s stable blocker has no descriptor"),
				Expected.PrimitiveId), FindExpectedDescriptor(Descriptors, Expected.PrimitiveId));
			TestEqual(*FString::Printf(TEXT("%s stable blocker has no binding"),
				Expected.PrimitiveId), CountCallableBindings(Bindings, Expected.PrimitiveId), 0);
		}
	}


 const auto* CompressionLeaf = FindExpectedDescriptor(Descriptors, TEXTVIEW("animation.sequence.compression.settings.set.v1"));
 if (TestNotNull(TEXT("supplied compression leaf is independently admitted"), CompressionLeaf))
 {
  TestEqual(TEXT("compression exact version"), CompressionLeaf->Version, 1u);
  TestTrue(TEXT("compression exact admission"), ValidateAnimationAdmissionEvidence(*CompressionLeaf).bValid);
  TestEqual(TEXT("compression has one callable binding"), CountCallableBindings(Bindings, CompressionLeaf->PrimitiveId), 1);
  for (int32 Case = 0; Case < 4; ++Case)
  {
   auto Drift = *CompressionLeaf;
   if (Case == 0) Drift.Admission.ExportedAuthoritativeApi += TEXT(";RawSettingsWrite");
   if (Case == 1) Drift.HandleArgumentSchemas[1].RequiredAccess = EAssetDocumentHandleAccess::Mutable;
   if (Case == 2) Drift.HandleArgumentSchemas[2].bRequired = true;
   if (Case == 3) Drift.InputSchemaId += TEXT(".drift");
   TestFalse(TEXT("compression authority/schema/handle drift is rejected"), ValidateAnimationAdmissionEvidence(Drift).bValid);
  }
 }
 const auto* NameLeaf = FindExpectedDescriptor(Descriptors, TEXTVIEW("animation.sequence.retarget_source_name.set.v1"));
 if (TestNotNull(TEXT("fixed name leaf is independently admitted"), NameLeaf))
 {
  TestEqual(TEXT("name leaf exact version"), NameLeaf->Version, 1u);
  TestTrue(TEXT("name leaf exact admission"), ValidateAnimationAdmissionEvidence(*NameLeaf).bValid);
  TestEqual(TEXT("name leaf has exactly one callable binding"), CountCallableBindings(Bindings, NameLeaf->PrimitiveId), 1);
  auto Drift = *NameLeaf; Drift.Admission.ExportedAuthoritativeApi += TEXT(";RawNameWrite");
  TestFalse(TEXT("name leaf API drift is rejected"), ValidateAnimationAdmissionEvidence(Drift).bValid);
 }
 const auto* Retarget = FindExpectedDescriptor(Descriptors, TEXTVIEW("animation.sequence.retarget_source_asset.set.v2"));
 if (TestNotNull(TEXT("retarget source is independently versioned and admitted"), Retarget))
 {
  TestEqual(TEXT("retarget exact version"), Retarget->Version, 2u);
  TestTrue(TEXT("retarget exact admission"), ValidateAnimationAdmissionEvidence(*Retarget).bValid);
  TestEqual(TEXT("retarget has exactly one callable binding"),
   CountCallableBindings(Bindings, Retarget->PrimitiveId), 1);
  auto Drift = *Retarget; Drift.Admission.ExportedAuthoritativeApi += TEXT(";RawPoseWrite");
  TestFalse(TEXT("retarget API drift is rejected"), ValidateAnimationAdmissionEvidence(Drift).bValid);
 }

 const auto* Snapshot = FindExpectedDescriptor(Descriptors, TEXTVIEW("animation.curve.float.enumerate.v2"));
 if (TestNotNull(TEXT("full snapshot is independently versioned"), Snapshot))
 {
  TestEqual(TEXT("snapshot version"), Snapshot->Version, 2u);
  TestEqual(TEXT("snapshot input"), Snapshot->InputSchemaId, FString(TEXT("animation.curve.float.enumerate.input.v2")));
  TestEqual(TEXT("snapshot output"), Snapshot->OutputSchemaId, FString(TEXT("animation.curve.float.enumerate.output.v2")));
  TestTrue(TEXT("snapshot has no child handles"), Snapshot->HandleResultSchemas.IsEmpty());
  TestEqual(TEXT("snapshot only uses asset handle kind"), Snapshot->AllowedHandleKinds,
   TArray<EAssetDocumentHandleKind>{EAssetDocumentHandleKind::Asset});
  TestTrue(TEXT("snapshot admission valid"), ValidateAnimationAdmissionEvidence(*Snapshot).bValid);
  for (int32 Case = 0; Case < 5; ++Case)
  {
   auto Drift = *Snapshot;
   if (Case == 0) Drift.Version = 1;
   if (Case == 1) Drift.InputSchemaId += TEXT(".drift");
   if (Case == 2) Drift.OutputSchemaId += TEXT(".drift");
   if (Case == 3) Drift.AllowedHandleKinds.Add(EAssetDocumentHandleKind::Curve);
   if (Case == 4) Drift.Admission.ExportedAuthoritativeApi = TEXT("GetMutableRichCurve");
   TestFalse(TEXT("snapshot exact shape rejects drift"), ValidateAnimationAdmissionEvidence(Drift).bValid);
  }
 }



 const auto* MontageSnapshot = FindExpectedDescriptor(Descriptors, TEXTVIEW("animation.montage.profile.snapshot.v1"));
 if (TestNotNull(TEXT("Montage profile snapshot is admitted"), MontageSnapshot))
 {
  TestTrue(TEXT("Montage snapshot exact admission passes"), ValidateAnimationAdmissionEvidence(*MontageSnapshot).bValid);
  TestEqual(TEXT("Montage snapshot version"), MontageSnapshot->Version, 1u);
  TestEqual(TEXT("Montage snapshot input schema"), MontageSnapshot->InputSchemaId, FString(TEXT("animation.montage.profile.snapshot.v1.input.v1")));
  TestEqual(TEXT("Montage snapshot output schema"), MontageSnapshot->OutputSchemaId, FString(TEXT("animation.montage.profile.snapshot.v1.output.v1")));
  TestEqual(TEXT("Montage snapshot is game-thread readonly"), MontageSnapshot->ExecutionThread, EAssetDocumentExecutionThread::GameThread);
  TestEqual(TEXT("Montage snapshot access"), MontageSnapshot->Access, EAssetDocumentPrimitiveAccess::ReadOnly);
  TestTrue(TEXT("Montage snapshot explicitly allows Preflight and Verify"), MontageSnapshot->AllowedPhases ==
   TArray<EAssetDocumentRuntimeRequestPhase>{EAssetDocumentRuntimeRequestPhase::Preflight, EAssetDocumentRuntimeRequestPhase::Verify});
  TestFalse(TEXT("Montage snapshot does not require a mutation transaction"), MontageSnapshot->bRequiresActiveTransaction);
  TestTrue(TEXT("Montage snapshot has no mutation footprint or result handles"), MontageSnapshot->MutationFootprintSchemaId.IsEmpty() && MontageSnapshot->HandleResultSchemas.IsEmpty());
  TestEqual(TEXT("Montage snapshot only accepts asset kind"), MontageSnapshot->AllowedHandleKinds,
   TArray<EAssetDocumentHandleKind>{EAssetDocumentHandleKind::Asset});
  AssertReadOnlyAssetArgument(*this, *MontageSnapshot);
  TestEqual(TEXT("Montage snapshot has one binding"), CountCallableBindings(Bindings, TEXTVIEW("animation.montage.profile.snapshot.v1")), 1);
  for (const auto& Binding : Bindings)
   if (Binding.PrimitiveId == TEXT("animation.montage.profile.snapshot.v1"))
    TestTrue(TEXT("Montage snapshot routes to exact readonly handler"), Binding.Handler == &InvokeAnimationMontageProfileSnapshot);
  for (int32 Case = 0; Case < 9; ++Case)
  {
   auto Drift = *MontageSnapshot;
   if (Case == 0) Drift.AllowedPhases.Add(EAssetDocumentRuntimeRequestPhase::Apply);
   if (Case == 1) Drift.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Verify};
   if (Case == 2) Drift.HandleArgumentSchemas[0].RequiredAccess = EAssetDocumentHandleAccess::Mutable;
   if (Case == 3) Drift.HandleArgumentSchemas.Add({TEXT("notify"), EAssetDocumentHandleKind::Subobject, EAssetDocumentHandleAccess::ReadOnly, true});
   if (Case == 4) Drift.InputSchemaId += TEXT(".drift");
   if (Case == 5) Drift.Admission.ExportedAuthoritativeApi = TEXT("GetDefaultObject");
   if (Case == 6) Drift.Admission.ExactEngineVersion = TEXT("5.7.3");
   if (Case == 7) Drift.bRequiresActiveTransaction = true;
   if (Case == 8) Drift.Admission.AuthorityContextId = TEXT("generic.read-any-object");
   TestFalse(TEXT("Montage snapshot authority drift is rejected"), ValidateAnimationAdmissionEvidence(Drift).bValid);
  }
 }


    const auto* Replacement = FindExpectedDescriptor(Descriptors, TEXTVIEW("animation.curve.float.replace.v2"));
    if (TestNotNull(TEXT("complete curve replacement is independently versioned"), Replacement))
    {
        TestEqual(TEXT("replacement version"), Replacement->Version, 2u);
        TestEqual(TEXT("replacement input"), Replacement->InputSchemaId, FString(TEXT("animation.curve.float.replace.v2.input.v2")));
        TestEqual(TEXT("replacement output"), Replacement->OutputSchemaId, FString(TEXT("animation.curve.float.replace.v2.output.v2")));
        TestEqual(TEXT("replacement footprint"), Replacement->MutationFootprintSchemaId, FString(TEXT("animation.curve.float.replace.v2.footprint.v2")));
        TestEqual(TEXT("replacement game thread"), Replacement->ExecutionThread, EAssetDocumentExecutionThread::GameThread);
        TestEqual(TEXT("replacement mutates"), Replacement->Access, EAssetDocumentPrimitiveAccess::Mutating);
        TestTrue(TEXT("replacement phases"), Replacement->AllowedPhases ==
            TArray<EAssetDocumentRuntimeRequestPhase>{EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair});
        TestTrue(TEXT("replacement requires transaction"), Replacement->bRequiresActiveTransaction);
        TestTrue(TEXT("replacement has zero result handles"), Replacement->HandleResultSchemas.IsEmpty());
        TestEqual(TEXT("replacement uses only Asset kind"), Replacement->AllowedHandleKinds,
            TArray<EAssetDocumentHandleKind>{EAssetDocumentHandleKind::Asset});
        if (TestEqual(TEXT("replacement has one input"), Replacement->HandleArgumentSchemas.Num(), 1))
        {
            const auto& Asset = Replacement->HandleArgumentSchemas[0];
            TestEqual(TEXT("replacement input name"), Asset.Name, FString(TEXT("asset")));
            TestEqual(TEXT("replacement input kind"), Asset.Kind, EAssetDocumentHandleKind::Asset);
            TestEqual(TEXT("replacement input access"), Asset.RequiredAccess, EAssetDocumentHandleAccess::Mutable);
            TestTrue(TEXT("replacement input required"), Asset.bRequired);
        }
        TestEqual(TEXT("replacement uses the exact approved bridge"), Replacement->Admission.GovernanceId,
            FString(TEXT("ANIMATION-FLOAT-CURVE-EXACT-STATE-v1")));
        TestEqual(TEXT("replacement bridge disposition"), Replacement->Admission.Disposition,
            EAssetDocumentPrimitiveDisposition::AdmittedBridge);
        TestTrue(TEXT("replacement exact admission passes"), ValidateAnimationAdmissionEvidence(*Replacement).bValid);
        TestEqual(TEXT("replacement has one callable v2 binding"),
            CountCallableBindings(Bindings, TEXTVIEW("animation.curve.float.replace.v2")), 1);
        for (const auto& Binding : Bindings)
            if (Binding.PrimitiveId == TEXT("animation.curve.float.replace.v2"))
                TestTrue(TEXT("replacement routes to its typed handler"), Binding.Handler == &InvokeAnimationFloatCurveReplace);
        for (int32 Case = 0; Case < 4; ++Case)
        {
            auto Drift = *Replacement;
            if (Case == 0) Drift.Version = 1;
            if (Case == 1) Drift.InputSchemaId += TEXT(".drift");
            if (Case == 2) Drift.Admission.GovernanceId = TEXT("unapproved.curve.write");
            if (Case == 3) Drift.bRequiresActiveTransaction = false;
            TestFalse(TEXT("replacement admission rejects contract drift"), ValidateAnimationAdmissionEvidence(Drift).bValid);
        }
    }

	for (const auto& Expected : {MakeAnimationMontageSlotsDescriptor(), MakeAnimationMontageSectionsDescriptor()})
	{
		const auto* Structural = FindExpectedDescriptor(Descriptors, Expected.PrimitiveId);
		if (!TestNotNull(TEXT("structural descriptor admitted"), Structural)) continue;
		TestTrue(TEXT("structural descriptor has the exact typed admission"), IsMontageStructuralAdmissionShape(*Structural));
		TestTrue(TEXT("structural admission passes shared validation"), ValidateAnimationAdmissionEvidence(*Structural).bValid);
		TestEqual(TEXT("structural operation is an admitted bridge"), Structural->Admission.Disposition, EAssetDocumentPrimitiveDisposition::AdmittedBridge);
		TestEqual(TEXT("structural operation mutates"), Structural->Access, EAssetDocumentPrimitiveAccess::Mutating);
		TestTrue(TEXT("structural operation requires the prepared transaction"), Structural->bRequiresActiveTransaction);
		TestEqual(TEXT("structural operation has only the target input handle"), Structural->HandleArgumentSchemas.Num(), 1);
		if (Structural->HandleArgumentSchemas.Num() == 1)
		{
			const auto& Asset = Structural->HandleArgumentSchemas[0];
			TestEqual(TEXT("structural input is asset"), Asset.Name, FString(TEXT("asset")));
			TestEqual(TEXT("structural input kind is Asset"), Asset.Kind, EAssetDocumentHandleKind::Asset);
			TestEqual(TEXT("structural target is mutable"), Asset.RequiredAccess, EAssetDocumentHandleAccess::Mutable);
			TestTrue(TEXT("structural target is required"), Asset.bRequired);
		}
		TestEqual(TEXT("structural descriptor has one callable binding"), CountCallableBindings(Bindings, Expected.PrimitiveId), 1);
		for (const auto& Binding : Bindings)
			if (Binding.PrimitiveId == Expected.PrimitiveId)
				TestTrue(TEXT("structural binding routes to the typed operation"), Binding.Handler == &InvokeAnimationMontageStructural);
		// Admission does not permit a direct, unprepared invocation without its transaction.
		FAssetDocumentPrimitiveInvocation Unprepared;
		Unprepared.PrimitiveId = Expected.PrimitiveId; Unprepared.Version = 1;
		FEnumerationHandleResolver RejectedHandles(Unprepared);
		const auto Rejected = InvokeAnimationMontageStructural(Unprepared, RejectedHandles, nullptr);
		TestEqual(TEXT("unprepared structural call fails"), Rejected.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
		TestTrue(TEXT("unprepared structural call requires transaction"), HasDiagnostic(Rejected.Diagnostics, TEXTVIEW("AnimationProvider.TransactionUnavailable")));
		TestTrue(TEXT("unprepared structural call returns no mutations or handles"), Rejected.ChangedSet.IsEmpty() && Rejected.HandleResults.IsEmpty());
		TestEqual(TEXT("unprepared structural call acquires no lease"), RejectedHandles.GetAcquireCount(), 0);
		TestEqual(TEXT("unprepared structural call issues no handle"), RejectedHandles.GetIssueCount(), 0);
		TestEqual(TEXT("unprepared structural call invalidates no handle"), RejectedHandles.GetInvalidationCount(), 0);
		for (int32 Case = 0; Case < 4; ++Case)
		{
			auto Drift = *Structural;
			if (Case == 0) Drift.Admission.GovernanceId = TEXT("unapproved.generic.publication");
			if (Case == 1) Drift.HandleArgumentSchemas.Add({TEXT("animation_0"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly, true});
			if (Case == 2) Drift.Admission.ExactEngineVersion = TEXT("5.7.3");
			if (Case == 3) Drift.bRequiresActiveTransaction = false;
			TestFalse(TEXT("structural authority and input drift fail closed"), ValidateAnimationAdmissionEvidence(Drift).bValid);
		}
	}

	TArray<FAssetDocumentPrimitiveDescriptor> ReplacedDescriptors;
	ReplacedDescriptors.Append(Descriptors.GetData(), Descriptors.Num());
	TArray<FAssetDocumentAnimationPrimitiveBinding> ReplacedBindings;
	ReplacedBindings.Append(Bindings.GetData(), Bindings.Num());
	const FString BlockedPrimitiveId =
		TEXT("animation.sequence.retarget_source_asset.set.v1");
	ReplacedDescriptors[0].PrimitiveId = BlockedPrimitiveId;
	ReplacedDescriptors[0].InputSchemaId = BlockedPrimitiveId + TEXT(".input.v1");
	ReplacedDescriptors[0].OutputSchemaId = BlockedPrimitiveId + TEXT(".output.v1");
	ReplacedDescriptors[0].Admission.PrimaryOperationId = BlockedPrimitiveId;
	ReplacedBindings[0].PrimitiveId = ReplacedDescriptors[0].PrimitiveId;
	const FAssetDocumentContractValidationResult ReplacedAdmission =
		ValidateAnimationDescriptorBindingBijection(
			Decisions, ReplacedDescriptors, ReplacedBindings);
	TestFalse(TEXT("a blocked decision cannot replace an admitted callable row"),
		ReplacedAdmission.bValid);
	TestTrue(TEXT("decision projection mismatch has a stable diagnostic"),
		HasDiagnostic(ReplacedAdmission.Diagnostics,
			TEXTVIEW("AnimationProvider.BindingBijectionInvalid")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAnimationProviderAssetLifecycleTest,
	"UEAssetDoc.AnimationProvider.AssetLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationProviderAssetLifecycleTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimationProviderDescriptorTests;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAnimationPrimitiveProvider();
	const FAssetDocumentPrimitiveInvocation ResolverInvocation =
		MakeMetadataInvocation(TEXT("animation.asset.preview_mesh.set.v1"));
	FEnumerationHandleResolver Handles(ResolverInvocation);

	const auto PreviewWithoutTransaction = Provider->Invoke(ResolverInvocation, Handles, nullptr);
	TestTrue(TEXT("admitted Preview still requires its real transaction"),
		HasDiagnostic(PreviewWithoutTransaction.Diagnostics, TEXTVIEW("AnimationProvider.TransactionUnavailable")));
	AssertStableBlocker(*this, *Provider, Handles,
		TEXT("animation.sequence.retarget_source_asset.set.v1"),
		TEXT("AD.PANIM.BLOCK.RETARGET_SOURCE_AUTHORITY"));
	TestEqual(TEXT("blocked asset mutations do not issue child handles"),
		Handles.GetIssueCount(), 0);
	TestEqual(TEXT("blocked asset mutations do not invalidate handles"),
		Handles.GetInvalidationCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAnimationProviderMetadataLifecycleTest,
	"UEAssetDoc.AnimationProvider.MetadataLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationProviderMetadataLifecycleTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimationProviderDescriptorTests;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAnimationPrimitiveProvider();
	const FAssetDocumentPrimitiveInvocation BlockedInvocation =
		MakeMetadataInvocation(TEXT("animation.asset_metadata.replace.v1"));
	FEnumerationHandleResolver BlockedHandles(BlockedInvocation);
	AssertStableBlocker(*this, *Provider, BlockedHandles,
		TEXT("animation.asset_metadata.replace.v1"),
		TEXT("AD.PANIM.BLOCK.RUNTIME_HANDLE_BOOTSTRAP"));

	UAnimSequence* EmptyAsset = NewObject<UAnimSequence>(GetTransientPackage(),
		TEXT("AssetDocumentAnimationEmptyMetadata"), RF_Transient);
	TestNotNull(TEXT("empty Animation asset fixture exists"), EmptyAsset);
	if (EmptyAsset == nullptr)
	{
		return false;
	}

	const FAssetDocumentPrimitiveInvocation EmptyMetadataInvocation =
		MakeMetadataInvocation(TEXT("animation.metadata.enumerate.v1"));
	FEnumerationHandleResolver EmptyMetadataHandles(EmptyMetadataInvocation);
	const FAssetDocumentPrimitiveOutcome EmptyMetadata =
		BuildAnimationMetadataEnumeration(*EmptyAsset, EmptyMetadataInvocation,
			EmptyMetadataHandles);
	const TArray<UObject*> NoObjects;
	AssertReadOnlyEnumeration(*this, EmptyMetadataInvocation, EmptyMetadata,
		EmptyMetadataHandles, TEXT("metadata"), NoObjects);

	const FAssetDocumentPrimitiveInvocation EmptyUserDataInvocation =
		MakeMetadataInvocation(TEXT("animation.asset_user_data.enumerate.v1"));
	FEnumerationHandleResolver EmptyUserDataHandles(EmptyUserDataInvocation);
	const FAssetDocumentPrimitiveOutcome EmptyUserData =
		BuildAnimationAssetUserDataEnumeration(*EmptyAsset,
			EmptyUserDataInvocation, EmptyUserDataHandles);
	AssertReadOnlyEnumeration(*this, EmptyUserDataInvocation, EmptyUserData,
		EmptyUserDataHandles, TEXT("user_data"), NoObjects);

	UAnimSequence* PopulatedAsset = NewObject<UAnimSequence>(GetTransientPackage(),
		TEXT("AssetDocumentAnimationPopulatedMetadata"), RF_Transient);
	TestNotNull(TEXT("populated Animation asset fixture exists"), PopulatedAsset);
	if (PopulatedAsset == nullptr)
	{
		return false;
	}
	UAnimMetaData* Metadata = UAnimMetaData::StaticClass()
		->GetDefaultObject<UAnimMetaData>();
	TestNotNull(TEXT("Animation metadata CDO is available"), Metadata);
	if (Metadata == nullptr)
	{
		return false;
	}
	PopulatedAsset->AddMetaData(Metadata);
	TestEqual(TEXT("metadata fixture preserves authoritative Engine order"),
		PopulatedAsset->GetMetaData().Num(), 1);
	TestEqual(TEXT("metadata fixture row is the authoritative attached object"),
		PopulatedAsset->GetMetaData()[0], Metadata);
	const FAssetDocumentPrimitiveInvocation MetadataInvocation =
		MakeMetadataInvocation(TEXT("animation.metadata.enumerate.v1"));
	FEnumerationHandleResolver MetadataHandles(MetadataInvocation);
	const FAssetDocumentPrimitiveOutcome MetadataOutcome =
		BuildAnimationMetadataEnumeration(*PopulatedAsset, MetadataInvocation,
			MetadataHandles);
	TArray<UObject*> ExpectedMetadata{Metadata};
	AssertReadOnlyEnumeration(*this, MetadataInvocation, MetadataOutcome,
		MetadataHandles, TEXT("metadata"), ExpectedMetadata);

	UAnimCurveMetaData* CurveMetadata = NewObject<UAnimCurveMetaData>(
		PopulatedAsset, TEXT("FirstUserData"), RF_Transient);
	USubtitleAssetUserData* SubtitleUserData = NewObject<USubtitleAssetUserData>(
		PopulatedAsset, TEXT("SecondUserData"), RF_Transient);
	TestNotNull(TEXT("first concrete Engine asset user-data fixture exists"),
		CurveMetadata);
	TestNotNull(TEXT("second concrete Engine asset user-data fixture exists"),
		SubtitleUserData);
	if (CurveMetadata == nullptr || SubtitleUserData == nullptr)
	{
		return false;
	}
	PopulatedAsset->AddAssetUserData(CurveMetadata);
	PopulatedAsset->AddAssetUserData(SubtitleUserData);
	const TArray<UAssetUserData*>* AuthoritativeUserData =
		PopulatedAsset->GetAssetUserDataArray();
	TestNotNull(TEXT("Animation asset exposes authoritative user-data order"),
		AuthoritativeUserData);
	if (AuthoritativeUserData == nullptr || AuthoritativeUserData->Num() != 2)
	{
		return false;
	}
	UAssetUserData* ExpectedFirstUserData = CurveMetadata;
	UAssetUserData* ExpectedSecondUserData = SubtitleUserData;
	TestEqual(TEXT("first user-data object keeps Engine array order"),
		(*AuthoritativeUserData)[0], ExpectedFirstUserData);
	TestEqual(TEXT("second user-data object keeps Engine array order"),
		(*AuthoritativeUserData)[1], ExpectedSecondUserData);
	const FAssetDocumentPrimitiveInvocation UserDataInvocation =
		MakeMetadataInvocation(TEXT("animation.asset_user_data.enumerate.v1"));
	FEnumerationHandleResolver UserDataHandles(UserDataInvocation);
	const FAssetDocumentPrimitiveOutcome UserDataOutcome =
		BuildAnimationAssetUserDataEnumeration(*PopulatedAsset, UserDataInvocation,
			UserDataHandles);
	TArray<UObject*> ExpectedUserData{CurveMetadata, SubtitleUserData};
	AssertReadOnlyEnumeration(*this, UserDataInvocation, UserDataOutcome,
		UserDataHandles, TEXT("user_data"), ExpectedUserData);

	const FAssetDocumentPrimitiveInvocation WrongTargetInvocation =
		MakeMetadataInvocation(TEXT("animation.metadata.enumerate.v1"));
	FEnumerationHandleResolver WrongTargetHandles(WrongTargetInvocation);
	const FAssetDocumentPrimitiveOutcome WrongTargetOutcome =
		BuildAnimationMetadataEnumeration(*CurveMetadata, WrongTargetInvocation,
			WrongTargetHandles);
	TestEqual(TEXT("metadata rejects a non-Animation target"), WrongTargetOutcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("metadata wrong target uses the stable diagnostic"),
		HasDiagnostic(WrongTargetOutcome.Diagnostics,
			TEXT("AnimationProvider.TargetClassMismatch")));
	TestTrue(TEXT("metadata wrong target issues no child handles"),
		WrongTargetOutcome.HandleResults.IsEmpty());
	TestTrue(TEXT("metadata wrong target has no changed set"),
		WrongTargetOutcome.ChangedSet.IsEmpty());
	const FAssetDocumentPrimitiveInvocation WrongUserDataTargetInvocation =
		MakeMetadataInvocation(TEXT("animation.asset_user_data.enumerate.v1"));
	FEnumerationHandleResolver WrongUserDataTargetHandles(
		WrongUserDataTargetInvocation);
	const FAssetDocumentPrimitiveOutcome WrongUserDataTarget =
		BuildAnimationAssetUserDataEnumeration(*CurveMetadata,
			WrongUserDataTargetInvocation, WrongUserDataTargetHandles);
	TestEqual(TEXT("asset-user-data rejects a non-Animation target"),
		WrongUserDataTarget.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("asset-user-data wrong target uses the stable diagnostic"),
		HasDiagnostic(WrongUserDataTarget.Diagnostics,
			TEXT("AnimationProvider.TargetClassMismatch")));
	TestTrue(TEXT("asset-user-data wrong target returns no child handles"),
		WrongUserDataTarget.HandleResults.IsEmpty());
	TestTrue(TEXT("asset-user-data wrong target has no changed set"),
		WrongUserDataTarget.ChangedSet.IsEmpty());

	const FAssetDocumentPrimitiveInvocation FailedIssueInvocation =
		MakeMetadataInvocation(TEXT("animation.metadata.enumerate.v1"));
	FEnumerationHandleResolver FailedIssueHandles(FailedIssueInvocation);
	FailedIssueHandles.RejectIssue();
	const FAssetDocumentPrimitiveOutcome FailedIssue =
		BuildAnimationMetadataEnumeration(*PopulatedAsset, FailedIssueInvocation,
			FailedIssueHandles);
	TestEqual(TEXT("metadata child-handle issue failure is terminal"),
		FailedIssue.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("metadata issue failure projects operation rejection"),
		HasDiagnostic(FailedIssue.Diagnostics,
			TEXT("AnimationProvider.OperationRejected")));
	TestTrue(TEXT("metadata issue failure returns no partial handles"),
		FailedIssue.HandleResults.IsEmpty());
	TestTrue(TEXT("metadata issue failure returns no partial changed set"),
		FailedIssue.ChangedSet.IsEmpty());
	TestTrue(TEXT("metadata issue failure returns no partial T01 value"),
		FailedIssue.Value.Elements.IsEmpty()
			&& FailedIssue.Value.ObjectKeys.IsEmpty());
	const FAssetDocumentPrimitiveInvocation FailedUserDataIssueInvocation =
		MakeMetadataInvocation(TEXT("animation.asset_user_data.enumerate.v1"));
	FEnumerationHandleResolver FailedUserDataIssueHandles(
		FailedUserDataIssueInvocation);
	FailedUserDataIssueHandles.RejectIssue();
	const FAssetDocumentPrimitiveOutcome FailedUserDataIssue =
		BuildAnimationAssetUserDataEnumeration(*PopulatedAsset,
			FailedUserDataIssueInvocation, FailedUserDataIssueHandles);
	TestEqual(TEXT("asset-user-data child-handle issue failure is terminal"),
		FailedUserDataIssue.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("asset-user-data issue failure projects operation rejection"),
		HasDiagnostic(FailedUserDataIssue.Diagnostics,
			TEXT("AnimationProvider.OperationRejected")));
	TestTrue(TEXT("asset-user-data issue failure returns no partial handles"),
		FailedUserDataIssue.HandleResults.IsEmpty());
	TestTrue(TEXT("asset-user-data issue failure returns no partial changed set"),
		FailedUserDataIssue.ChangedSet.IsEmpty());
	TestTrue(TEXT("asset-user-data issue failure returns no partial T01 value"),
		FailedUserDataIssue.Value.Elements.IsEmpty()
			&& FailedUserDataIssue.Value.ObjectKeys.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAnimationProviderConsumerParityTest,
	"UEAssetDoc.AnimationProvider.ConsumerParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationProviderConsumerParityTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimationProviderDescriptorTests;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAnimationPrimitiveProvider();
	const TConstArrayView<FAssetDocumentPrimitiveDescriptor> Catalog =
		GetAssetDocumentAnimationPrimitiveDescriptors();
	TestEqual(TEXT("factory provider ID is animation"), Provider->GetProviderId(),
		FString(TEXT("animation")));
	TestEqual(TEXT("factory exposes only the admitted catalog"),
		Provider->DescribePrimitives().Num(), Catalog.Num());
	for (int32 Index = 0; Index < Catalog.Num(); ++Index)
	{
		TestEqual(TEXT("factory preserves immutable catalog order"),
			Provider->DescribePrimitives()[Index].PrimitiveId, Catalog[Index].PrimitiveId);
	}

	IAssetDocumentRuntimeModule* Module =
		FModuleManager::LoadModulePtr<IAssetDocumentRuntimeModule>(
			TEXT("AssetDocumentAnimation"));
	TestNotNull(TEXT("Animation module loads for dormant-registration check"), Module);
	TestFalse(TEXT("Animation provider has no active runtime registration"),
		Module != nullptr && Module->HasActiveRuntimeRegistration());
	return true;
}

#endif
