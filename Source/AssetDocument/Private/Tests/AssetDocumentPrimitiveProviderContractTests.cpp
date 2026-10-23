// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Runtime/AssetDocumentPrimitiveProvider.h"

#include "Misc/AutomationTest.h"

#include <limits>
#include <type_traits>

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentPrimitiveProviderContractTests
{
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveOwner::Core) == 0);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveOwner::Editor) == 1);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveOwner::AI) == 2);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveOwner::Animation) == 3);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveOwner::Blueprint) == 4);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveOwner::UMG) == 5);
static_assert(static_cast<uint8>(EAssetDocumentProviderAvailabilityState::Available) == 0);
static_assert(static_cast<uint8>(EAssetDocumentProviderAvailabilityState::Unavailable) == 1);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveDisposition::OrdinaryHelper) == 0);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveDisposition::FrameworkRawLifecycleAllowlisted) == 1);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveDisposition::StableBlocker) == 2);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveDisposition::GovernanceCandidate) == 3);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveDisposition::AdmittedBridge) == 4);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper) == 0);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveAuthorityPath::NewExportedHelper) == 1);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveAuthorityPath::FrameworkRawLifecycleAllowlist) == 2);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveAuthorityPath::BridgeAdmitted) == 3);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveAuthorityPath::StableBlocker) == 4);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveOutcomeStatus::Succeeded) == 0);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveOutcomeStatus::Failed) == 1);
static_assert(static_cast<uint8>(EAssetDocumentPrimitiveOutcomeStatus::Unavailable) == 2);
static_assert(static_cast<uint8>(EAssetDocumentHandleResultCardinality::ExactlyOne) == 0);
static_assert(static_cast<uint8>(EAssetDocumentHandleResultCardinality::ZeroOrOne) == 1);
static_assert(static_cast<uint8>(EAssetDocumentHandleResultCardinality::ZeroOrMore) == 2);
static_assert(static_cast<uint8>(EAssetDocumentHandleResultCardinality::OneOrMore) == 3);

#define ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(Type, Member, Expected) \
	static_assert(std::is_same_v<decltype(Type::Member), Expected>)
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, ContractVersion, uint32);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, Disposition, EAssetDocumentPrimitiveDisposition);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, AuthorityPath, EAssetDocumentPrimitiveAuthorityPath);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, ExactEngineVersion, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, PhysicalOwner, EAssetDocumentPrimitiveOwner);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, PublicModule, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, PublicHeader, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, ExportedAuthoritativeApi, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, AuthorityContextId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, PrimaryOperationId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, IdentityLifetimeContractId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, TransactionContractId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, RollbackContractId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, CompletionContractId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, SaveReloadEvidenceId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, FailureInjectionTestIds, TArray<FString>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, VerificationTestIds, TArray<FString>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, SourceMarker, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, RawLifecycleAllowlistId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, GovernanceId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, IndependentCandidateReviewId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, IndependentBridgeReviewId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, HelperReplacementCondition, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, RemovalCondition, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveAdmissionEvidence, AuthorityFingerprint, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveHandleArgumentSchema, Name, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveHandleArgumentSchema, Kind, EAssetDocumentHandleKind);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveHandleArgumentSchema, RequiredAccess, EAssetDocumentHandleAccess);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveHandleArgumentSchema, bRequired, bool);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveHandleArgument, Name, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveHandleArgument, Handle, FAssetDocumentOpaqueHandle);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveHandleResultSchema, Name, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveHandleResultSchema, Kind, EAssetDocumentHandleKind);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveHandleResultSchema, GrantedAccess, EAssetDocumentHandleAccess);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveHandleResultSchema, Cardinality, EAssetDocumentHandleResultCardinality);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveHandleResult, Name, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveHandleResult, Handles, TArray<FAssetDocumentOpaqueHandle>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveDescriptor, PrimitiveId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveDescriptor, Version, uint32);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveDescriptor, ProviderId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveDescriptor, Owner, EAssetDocumentPrimitiveOwner);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveDescriptor, ExecutionThread, EAssetDocumentExecutionThread);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveDescriptor, Access, EAssetDocumentPrimitiveAccess);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveDescriptor, AllowedPhases, TArray<EAssetDocumentRuntimeRequestPhase>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveDescriptor, AllowedHandleKinds, TArray<EAssetDocumentHandleKind>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveDescriptor, HandleArgumentSchemas, TArray<FAssetDocumentPrimitiveHandleArgumentSchema>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveDescriptor, HandleResultSchemas, TArray<FAssetDocumentPrimitiveHandleResultSchema>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveDescriptor, bRequiresActiveTransaction, bool);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveDescriptor, InputSchemaId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveDescriptor, OutputSchemaId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveDescriptor, MutationFootprintSchemaId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveDescriptor, StableDiagnosticCodes, TArray<FString>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveDescriptor, RequiredConsumerIds, TArray<FString>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveDescriptor, Admission, FAssetDocumentPrimitiveAdmissionEvidence);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveInvocation, PrimitiveId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveInvocation, Version, uint32);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveInvocation, RequestId, FGuid);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveInvocation, TransactionId, FGuid);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveInvocation, RuntimeRevision, uint64);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveInvocation, Phase, EAssetDocumentRuntimeRequestPhase);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveInvocation, ValueArguments, FAssetDocumentRuntimeValueDto);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveInvocation, HandleArguments, TArray<FAssetDocumentPrimitiveHandleArgument>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveOutcome, Status, EAssetDocumentPrimitiveOutcomeStatus);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveOutcome, Value, FAssetDocumentRuntimeValueDto);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveOutcome, HandleResults, TArray<FAssetDocumentPrimitiveHandleResult>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveOutcome, ChangedSet, TArray<FAssetDocumentChangedRecord>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentPrimitiveOutcome, Diagnostics, TArray<FAssetDocumentRuntimeDiagnostic>);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentProviderAvailability, ProviderId, FString);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentProviderAvailability, State, EAssetDocumentProviderAvailabilityState);
ASSET_DOCUMENT_ASSERT_MEMBER_TYPE(FAssetDocumentProviderAvailability, Diagnostic, TOptional<FAssetDocumentRuntimeDiagnostic>);
#undef ASSET_DOCUMENT_ASSERT_MEMBER_TYPE

using FValidateDescriptorSignature = FAssetDocumentContractValidationResult (*)(
	const FAssetDocumentPrimitiveDescriptor&, TConstArrayView<FAssetDocumentPrimitiveDescriptor>);
using FValidateInvocationSignature = FAssetDocumentContractValidationResult (*)(
	const FAssetDocumentPrimitiveInvocation&, const FAssetDocumentPrimitiveDescriptor&);
using FValidateOutcomeSignature = FAssetDocumentContractValidationResult (*)(
	const FAssetDocumentPrimitiveInvocation&, const FAssetDocumentPrimitiveDescriptor&,
	const FAssetDocumentPrimitiveOutcome&);
using FGetProviderIdSignature = FString (IAssetDocumentPrimitiveProvider::*)() const;
using FDescribePrimitivesSignature = TConstArrayView<FAssetDocumentPrimitiveDescriptor> (IAssetDocumentPrimitiveProvider::*)() const;
using FInspectAvailabilitySignature = FAssetDocumentProviderAvailability (IAssetDocumentPrimitiveProvider::*)() const;
using FInvokeSignature = FAssetDocumentPrimitiveOutcome (IAssetDocumentPrimitiveProvider::*)(
	const FAssetDocumentPrimitiveInvocation&, IAssetDocumentHandleResolver&, IAssetDocumentAtomicTransaction*);
static_assert(std::is_same_v<decltype(&ValidateAssetDocumentPrimitiveDescriptor), FValidateDescriptorSignature>);
static_assert(std::is_same_v<decltype(&ValidateAssetDocumentPrimitiveInvocation), FValidateInvocationSignature>);
static_assert(std::is_same_v<decltype(&ValidateAssetDocumentPrimitiveOutcome), FValidateOutcomeSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentPrimitiveProvider::GetProviderId), FGetProviderIdSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentPrimitiveProvider::DescribePrimitives), FDescribePrimitivesSignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentPrimitiveProvider::InspectAvailability), FInspectAvailabilitySignature>);
static_assert(std::is_same_v<decltype(&IAssetDocumentPrimitiveProvider::Invoke), FInvokeSignature>);
static_assert(std::has_virtual_destructor_v<IAssetDocumentPrimitiveProvider>);

FAssetDocumentRuntimeValueDto MakeStringValue(const TCHAR* Text)
{
	FAssetDocumentRuntimeValueDto Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::String;
	Value.StringValue = Text;
	return Value;
}

FAssetDocumentRuntimeValueDto MakeIntegerValue(int64 Integer)
{
	FAssetDocumentRuntimeValueDto Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::Integer;
	Value.IntegerValue = Integer;
	return Value;
}

FAssetDocumentRuntimeValueDto MakeValueArguments()
{
	FAssetDocumentRuntimeValueDto Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	Value.ObjectKeys = {TEXT("mode")};
	Value.Elements = {MakeStringValue(TEXT("exact"))};
	return Value;
}

FAssetDocumentPrimitiveDescriptor MakeOrdinaryDescriptor(
	EAssetDocumentPrimitiveAuthorityPath Path = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = TEXT("editor.asset.apply_delta");
	Descriptor.Version = 1;
	Descriptor.ProviderId = TEXT("editor");
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::Editor;
	Descriptor.Access = EAssetDocumentPrimitiveAccess::Mutating;
	Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply};
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
	Descriptor.HandleArgumentSchemas.Add({TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable, true});
	Descriptor.HandleResultSchemas.Add({
		TEXT("asset_result"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable, EAssetDocumentHandleResultCardinality::ExactlyOne});
	Descriptor.bRequiresActiveTransaction = true;
	Descriptor.InputSchemaId = TEXT("asset.apply_delta.input.v1");
	Descriptor.OutputSchemaId = TEXT("asset.apply_delta.output.v1");
	Descriptor.MutationFootprintSchemaId = TEXT("asset.apply_delta.footprint.v1");
	Descriptor.StableDiagnosticCodes = {TEXT("AssetDocument.Primitive.ApplyDeltaFailed")};
	Descriptor.RequiredConsumerIds = {TEXT("python.generic")};
	FAssetDocumentPrimitiveAdmissionEvidence& Evidence = Descriptor.Admission;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath = Path;
	Evidence.ExactEngineVersion = TEXT("5.6.0");
	Evidence.PhysicalOwner = EAssetDocumentPrimitiveOwner::Editor;
	Evidence.PublicModule = TEXT("UnrealEd");
	Evidence.PublicHeader = TEXT("Editor.h");
	Evidence.ExportedAuthoritativeApi = TEXT("FEditorAuthoritativeApi::ApplyDelta");
	Evidence.AuthorityContextId = TEXT("editor.asset.context.v1");
	Evidence.PrimaryOperationId = TEXT("editor.asset.apply_delta.v1");
	Evidence.IdentityLifetimeContractId = TEXT("identity.request-bound.v1");
	Evidence.TransactionContractId = TEXT("transaction.live-required.v1");
	Evidence.RollbackContractId = TEXT("rollback.snapshot.v1");
	Evidence.CompletionContractId = TEXT("completion.post-edit.v1");
	Evidence.SaveReloadEvidenceId = TEXT("save-reload.asset.v1");
	Evidence.FailureInjectionTestIds = {TEXT("failure.editor.asset.apply_delta.v1")};
	Evidence.VerificationTestIds = {TEXT("verify.editor.asset.apply_delta.v1")};
	Evidence.AuthorityFingerprint = TEXT("fingerprint.editor.asset.apply_delta.v1");
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeFrameworkRawDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor();
	Descriptor.PrimitiveId = TEXT("editor.framework.factory.create_scratch");
	Descriptor.InputSchemaId = TEXT("framework.factory.create_scratch.input.v1");
	Descriptor.OutputSchemaId = TEXT("framework.factory.create_scratch.output.v1");
	Descriptor.MutationFootprintSchemaId = TEXT("framework.factory.create_scratch.footprint.v1");
	Descriptor.Admission.Disposition = EAssetDocumentPrimitiveDisposition::FrameworkRawLifecycleAllowlisted;
	Descriptor.Admission.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::FrameworkRawLifecycleAllowlist;
	Descriptor.Admission.PrimaryOperationId = TEXT("framework.factory.create_scratch.v1");
	Descriptor.Admission.SourceMarker = TEXT("ASSETDOCUMENT_FRAMEWORK_RAW_LIFECYCLE_ALLOWLIST(framework.factory.create_scratch.v1)");
	Descriptor.Admission.RawLifecycleAllowlistId = TEXT("ASSETDOCUMENT_FRAMEWORK_RAW_CALLSITE(framework.factory.create_scratch.v1)");
	Descriptor.Admission.HelperReplacementCondition = TEXT("Replace when UE exports a narrower factory helper.");
	Descriptor.Admission.RemovalCondition = TEXT("Remove when the authoritative factory owns scratch creation.");
	Descriptor.Admission.AuthorityFingerprint = TEXT("fingerprint.framework.factory.create_scratch.v1");
	return Descriptor;
}

void RebindFrameworkRawOperation(
	FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FString& OperationId)
{
	Descriptor.Admission.PrimaryOperationId = OperationId;
	Descriptor.Admission.SourceMarker = FString::Printf(
		TEXT("ASSETDOCUMENT_FRAMEWORK_RAW_LIFECYCLE_ALLOWLIST(%s)"), *OperationId);
	Descriptor.Admission.RawLifecycleAllowlistId = FString::Printf(
		TEXT("ASSETDOCUMENT_FRAMEWORK_RAW_CALLSITE(%s)"), *OperationId);
	Descriptor.Admission.AuthorityFingerprint = FString::Printf(TEXT("fingerprint.%s"), *OperationId);
}

FAssetDocumentPrimitiveDescriptor MakeAdmittedBridgeDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor();
	Descriptor.PrimitiveId = TEXT("umg.widget_animation.create");
	Descriptor.ProviderId = TEXT("umg");
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::UMG;
	Descriptor.InputSchemaId = TEXT("umg.widget_animation.create.input.v1");
	Descriptor.OutputSchemaId = TEXT("umg.widget_animation.create.output.v1");
	Descriptor.MutationFootprintSchemaId = TEXT("umg.widget_animation.create.footprint.v1");
	Descriptor.Admission.Disposition = EAssetDocumentPrimitiveDisposition::AdmittedBridge;
	Descriptor.Admission.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::BridgeAdmitted;
	Descriptor.Admission.PhysicalOwner = EAssetDocumentPrimitiveOwner::UMG;
	Descriptor.Admission.PublicModule = TEXT("UMGEditor");
	Descriptor.Admission.PublicHeader = TEXT("WidgetBlueprintEditorUtils.h");
	Descriptor.Admission.ExportedAuthoritativeApi = TEXT("AssetDocumentUMG::CreateWidgetAnimationBridge");
	Descriptor.Admission.AuthorityContextId = TEXT("umg.widget_blueprint.context.v1");
	Descriptor.Admission.PrimaryOperationId = TEXT("umg.widget_animation.create.v1");
	Descriptor.Admission.SourceMarker = TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_ADMISSION(UMG-WIDGET-ANIMATION-CREATE-v1)");
	Descriptor.Admission.RawLifecycleAllowlistId = TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_CALLSITE(UMG-WIDGET-ANIMATION-CREATE-v1|umg.widget_animation.create.v1)");
	Descriptor.Admission.GovernanceId = TEXT("UMG-WIDGET-ANIMATION-CREATE-v1");
	Descriptor.Admission.IndependentCandidateReviewId = TEXT("review.candidate.umg-widget-animation-create.v1");
	Descriptor.Admission.IndependentBridgeReviewId = TEXT("review.bridge.umg-widget-animation-create.v1");
	Descriptor.Admission.HelperReplacementCondition = TEXT("Replace if Engine exports animation create lifecycle.");
	Descriptor.Admission.RemovalCondition = TEXT("Remove after helper replacement verification.");
	Descriptor.Admission.AuthorityFingerprint = TEXT("fingerprint.umg.widget_animation.create.v1");
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeStableBlockerDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = TEXT("umg.designer_binding.apply_delta");
	Descriptor.Version = 1;
	Descriptor.ProviderId = TEXT("umg");
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::UMG;
	Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply};
	Descriptor.StableDiagnosticCodes = {TEXT("AssetDocument.Primitive.AuthorityBlocked")};
	Descriptor.Admission.Disposition = EAssetDocumentPrimitiveDisposition::StableBlocker;
	Descriptor.Admission.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::StableBlocker;
	Descriptor.Admission.PhysicalOwner = EAssetDocumentPrimitiveOwner::UMG;
	Descriptor.Admission.PrimaryOperationId = TEXT("umg.designer_binding.apply_delta.v1");
	Descriptor.Admission.AuthorityFingerprint = TEXT("fingerprint.umg.designer_binding.apply_delta.v1");
	return Descriptor;
}

FAssetDocumentPrimitiveInvocation MakeValidInvocation(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = Descriptor.PrimitiveId;
	Invocation.Version = Descriptor.Version;
	Invocation.RequestId = FGuid::NewGuid();
	Invocation.TransactionId = FGuid::NewGuid();
	Invocation.RuntimeRevision = 7;
	Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Invocation.ValueArguments = MakeValueArguments();
	FAssetDocumentOpaqueHandle Handle;
	Handle.Kind = EAssetDocumentHandleKind::Asset;
	Handle.Access = EAssetDocumentHandleAccess::Mutable;
	Invocation.HandleArguments.Add({TEXT("asset"), Handle});
	return Invocation;
}

FAssetDocumentOpaqueHandle MakeResultHandle(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	EAssetDocumentHandleKind Kind = EAssetDocumentHandleKind::Asset,
	EAssetDocumentHandleAccess Access = EAssetDocumentHandleAccess::Mutable,
	uint32 Slot = 1,
	uint32 Generation = 1)
{
	FAssetDocumentOpaqueHandle Handle;
	Handle.RequestId = Invocation.RequestId;
	Handle.TransactionId = Invocation.TransactionId;
	Handle.RuntimeRevision = Invocation.RuntimeRevision;
	Handle.ProviderId = Descriptor.ProviderId;
	Handle.Slot = Slot;
	Handle.Generation = Generation;
	Handle.Kind = Kind;
	Handle.Access = Access;
	return Handle;
}

FAssetDocumentPrimitiveOutcome MakeValidOutcome(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	Outcome.Value.ObjectKeys = {TEXT("summary")};
	Outcome.Value.Elements = {MakeStringValue(TEXT("ok"))};
	Outcome.HandleResults.Add({
		TEXT("asset_result"), {MakeResultHandle(Invocation, Descriptor)}});
	return Outcome;
}

void TestInvalidContract(
	FAutomationTestBase& Test,
	const TCHAR* What,
	const FAssetDocumentContractValidationResult& Result,
	const TCHAR* ExpectedCode)
{
	Test.TestFalse(*FString::Printf(TEXT("P: %s is rejected"), What), Result.bValid);
	Test.TestEqual(*FString::Printf(TEXT("P: %s returns one diagnostic"), What), Result.Diagnostics.Num(), 1);
	if (Result.Diagnostics.Num() == 1)
	{
		Test.TestEqual(*FString::Printf(TEXT("P: %s diagnostic is stable"), What), Result.Diagnostics[0].Code, FString(ExpectedCode));
	}
}

void TestDefaultHandle(
	FAutomationTestBase& Test,
	const TCHAR* Prefix,
	const FAssetDocumentOpaqueHandle& Handle)
{
	Test.TestFalse(*FString::Printf(TEXT("S: %s request id defaults invalid"), Prefix), Handle.RequestId.IsValid());
	Test.TestFalse(*FString::Printf(TEXT("S: %s transaction id defaults invalid"), Prefix), Handle.TransactionId.IsValid());
	Test.TestEqual(*FString::Printf(TEXT("S: %s runtime revision defaults zero"), Prefix), Handle.RuntimeRevision, uint64{0});
	Test.TestTrue(*FString::Printf(TEXT("S: %s provider defaults empty"), Prefix), Handle.ProviderId.IsEmpty());
	Test.TestEqual(*FString::Printf(TEXT("S: %s slot defaults zero"), Prefix), Handle.Slot, uint32{0});
	Test.TestEqual(*FString::Printf(TEXT("S: %s generation defaults zero"), Prefix), Handle.Generation, uint32{0});
	Test.TestEqual(*FString::Printf(TEXT("S: %s kind defaults object"), Prefix), Handle.Kind, EAssetDocumentHandleKind::Object);
	Test.TestEqual(*FString::Printf(TEXT("S: %s access defaults read-only"), Prefix), Handle.Access, EAssetDocumentHandleAccess::ReadOnly);
}

void TestDefaultRuntimeValue(
	FAutomationTestBase& Test,
	const TCHAR* Prefix,
	const FAssetDocumentRuntimeValueDto& Value)
{
	Test.TestEqual(*FString::Printf(TEXT("S: %s kind defaults null"), Prefix), Value.Kind, EAssetDocumentRuntimeValueKind::Null);
	Test.TestFalse(*FString::Printf(TEXT("S: %s boolean defaults false"), Prefix), Value.BooleanValue);
	Test.TestEqual(*FString::Printf(TEXT("S: %s integer defaults zero"), Prefix), Value.IntegerValue, int64{0});
	Test.TestEqual(*FString::Printf(TEXT("S: %s number defaults zero"), Prefix), Value.NumberValue, 0.0);
	Test.TestTrue(*FString::Printf(TEXT("S: %s string defaults empty"), Prefix), Value.StringValue.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s bytes defaults empty"), Prefix), Value.BytesValue.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s elements default empty"), Prefix), Value.Elements.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s object keys default empty"), Prefix), Value.ObjectKeys.IsEmpty());
}

void TestDefaultAdmission(
	FAutomationTestBase& Test,
	const TCHAR* Prefix,
	const FAssetDocumentPrimitiveAdmissionEvidence& Admission)
{
	Test.TestEqual(*FString::Printf(TEXT("S: %s contract defaults v1"), Prefix), Admission.ContractVersion, AssetDocumentRuntimeContractVersion);
	Test.TestEqual(*FString::Printf(TEXT("S: %s disposition defaults blocker"), Prefix), Admission.Disposition, EAssetDocumentPrimitiveDisposition::StableBlocker);
	Test.TestEqual(*FString::Printf(TEXT("S: %s path defaults blocker"), Prefix), Admission.AuthorityPath, EAssetDocumentPrimitiveAuthorityPath::StableBlocker);
	Test.TestEqual(*FString::Printf(TEXT("S: %s owner defaults core"), Prefix), Admission.PhysicalOwner, EAssetDocumentPrimitiveOwner::Core);
	Test.TestTrue(*FString::Printf(TEXT("S: %s Engine version defaults empty"), Prefix), Admission.ExactEngineVersion.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s public module defaults empty"), Prefix), Admission.PublicModule.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s public header defaults empty"), Prefix), Admission.PublicHeader.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s exported API defaults empty"), Prefix), Admission.ExportedAuthoritativeApi.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s authority context defaults empty"), Prefix), Admission.AuthorityContextId.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s primary operation defaults empty"), Prefix), Admission.PrimaryOperationId.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s identity contract defaults empty"), Prefix), Admission.IdentityLifetimeContractId.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s transaction contract defaults empty"), Prefix), Admission.TransactionContractId.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s rollback contract defaults empty"), Prefix), Admission.RollbackContractId.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s completion contract defaults empty"), Prefix), Admission.CompletionContractId.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s save/reload evidence defaults empty"), Prefix), Admission.SaveReloadEvidenceId.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s failure tests default empty"), Prefix), Admission.FailureInjectionTestIds.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s verification tests default empty"), Prefix), Admission.VerificationTestIds.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s source marker defaults empty"), Prefix), Admission.SourceMarker.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s raw allowlist defaults empty"), Prefix), Admission.RawLifecycleAllowlistId.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s governance id defaults empty"), Prefix), Admission.GovernanceId.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s candidate review defaults empty"), Prefix), Admission.IndependentCandidateReviewId.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s bridge review defaults empty"), Prefix), Admission.IndependentBridgeReviewId.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s replacement condition defaults empty"), Prefix), Admission.HelperReplacementCondition.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s removal condition defaults empty"), Prefix), Admission.RemovalCondition.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("S: %s authority fingerprint defaults empty"), Prefix), Admission.AuthorityFingerprint.IsEmpty());
}

void TestIllegalAuthorityPaths(
	FAutomationTestBase& Test,
	const TCHAR* What,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	EAssetDocumentPrimitiveAuthorityPath ExpectedPath)
{
	for (uint8 Index = 0; Index <= static_cast<uint8>(EAssetDocumentPrimitiveAuthorityPath::StableBlocker); ++Index)
	{
		const EAssetDocumentPrimitiveAuthorityPath Path = static_cast<EAssetDocumentPrimitiveAuthorityPath>(Index);
		if (Path == ExpectedPath)
		{
			continue;
		}
		FAssetDocumentPrimitiveDescriptor Invalid = Descriptor;
		Invalid.Admission.AuthorityPath = Path;
		TestInvalidContract(Test, What, ValidateAssetDocumentPrimitiveDescriptor(Invalid), TEXT("PrimitiveDescriptor.InvalidPath"));
	}
}

using FDescriptorMutator = TFunction<void(FAssetDocumentPrimitiveDescriptor&)>;
using FInvocationMutator = TFunction<void(FAssetDocumentPrimitiveInvocation&)>;
using FOutcomeMutator = TFunction<void(FAssetDocumentPrimitiveOutcome&)>;

struct FDescriptorMutationCase
{
	const TCHAR* What;
	FDescriptorMutator Mutate;
};

struct FInvocationMutationCase
{
	const TCHAR* What;
	FInvocationMutator Mutate;
};

struct FOutcomeMutationCase
{
	const TCHAR* What;
	FOutcomeMutator Mutate;
};

void TestDescriptorMutationCases(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Base,
	const TCHAR* ExpectedCode,
	std::initializer_list<FDescriptorMutationCase> Cases)
{
	for (const FDescriptorMutationCase& Case : Cases)
	{
		FAssetDocumentPrimitiveDescriptor Invalid = Base;
		Case.Mutate(Invalid);
		TestInvalidContract(Test, Case.What, ValidateAssetDocumentPrimitiveDescriptor(Invalid), ExpectedCode);
	}
}

void TestInvocationMutationCases(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentPrimitiveInvocation& Base,
	const TCHAR* ExpectedCode,
	std::initializer_list<FInvocationMutationCase> Cases)
{
	for (const FInvocationMutationCase& Case : Cases)
	{
		FAssetDocumentPrimitiveInvocation Invalid = Base;
		Case.Mutate(Invalid);
		TestInvalidContract(Test, Case.What, ValidateAssetDocumentPrimitiveInvocation(Invalid, Descriptor), ExpectedCode);
	}
}

void TestOutcomeMutationCases(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentPrimitiveOutcome& Base,
	const TCHAR* ExpectedCode,
	std::initializer_list<FOutcomeMutationCase> Cases)
{
	for (const FOutcomeMutationCase& Case : Cases)
	{
		FAssetDocumentPrimitiveOutcome Invalid = Base;
		Case.Mutate(Invalid);
		TestInvalidContract(
			Test, Case.What,
			ValidateAssetDocumentPrimitiveOutcome(Invocation, Descriptor, Invalid),
			ExpectedCode);
	}
}

void TestAdmissionAndHandleDefaults(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveAdmissionEvidence Admission;
	TestDefaultAdmission(Test, TEXT("admission"), Admission);
	const FAssetDocumentPrimitiveHandleArgumentSchema HandleSchema;
	Test.TestTrue(TEXT("S: handle schema name defaults empty"), HandleSchema.Name.IsEmpty());
	Test.TestEqual(TEXT("S: handle schema kind defaults object"), HandleSchema.Kind, EAssetDocumentHandleKind::Object);
	Test.TestEqual(TEXT("S: handle schema access defaults read-only"), HandleSchema.RequiredAccess, EAssetDocumentHandleAccess::ReadOnly);
	Test.TestTrue(TEXT("S: handle schema defaults required"), HandleSchema.bRequired);
	const FAssetDocumentPrimitiveHandleArgument HandleArgument;
	Test.TestTrue(TEXT("S: handle argument name defaults empty"), HandleArgument.Name.IsEmpty());
	TestDefaultHandle(Test, TEXT("embedded handle argument"), HandleArgument.Handle);
	const FAssetDocumentPrimitiveHandleResultSchema ResultSchema;
	Test.TestTrue(TEXT("S: handle result schema name defaults empty"), ResultSchema.Name.IsEmpty());
	Test.TestEqual(TEXT("S: handle result schema kind defaults object"), ResultSchema.Kind, EAssetDocumentHandleKind::Object);
	Test.TestEqual(TEXT("S: handle result schema access defaults read-only"), ResultSchema.GrantedAccess, EAssetDocumentHandleAccess::ReadOnly);
	Test.TestEqual(TEXT("S: handle result schema cardinality defaults exactly one"), ResultSchema.Cardinality, EAssetDocumentHandleResultCardinality::ExactlyOne);
	const FAssetDocumentPrimitiveHandleResult HandleResult;
	Test.TestTrue(TEXT("S: handle result name defaults empty"), HandleResult.Name.IsEmpty());
	Test.TestTrue(TEXT("S: handle result handles default empty"), HandleResult.Handles.IsEmpty());
}

void TestDescriptorDefaults(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveDescriptor Descriptor;
	Test.TestTrue(TEXT("S: descriptor primitive identity defaults empty"), Descriptor.PrimitiveId.IsEmpty());
	Test.TestTrue(TEXT("S: descriptor provider identity defaults empty"), Descriptor.ProviderId.IsEmpty());
	Test.TestEqual(TEXT("S: descriptor version defaults zero"), Descriptor.Version, uint32{0});
	Test.TestEqual(TEXT("S: descriptor owner defaults core"), Descriptor.Owner, EAssetDocumentPrimitiveOwner::Core);
	Test.TestEqual(TEXT("S: descriptor thread defaults game thread"), Descriptor.ExecutionThread, EAssetDocumentExecutionThread::GameThread);
	Test.TestEqual(TEXT("S: descriptor access defaults read-only"), Descriptor.Access, EAssetDocumentPrimitiveAccess::ReadOnly);
	Test.TestTrue(TEXT("S: descriptor phases default empty"), Descriptor.AllowedPhases.IsEmpty());
	Test.TestTrue(TEXT("S: descriptor handle kinds default empty"), Descriptor.AllowedHandleKinds.IsEmpty());
	Test.TestTrue(TEXT("S: descriptor handle schemas default empty"), Descriptor.HandleArgumentSchemas.IsEmpty());
	Test.TestTrue(TEXT("S: descriptor handle result schemas default empty"), Descriptor.HandleResultSchemas.IsEmpty());
	Test.TestTrue(TEXT("S: descriptor diagnostics default empty"), Descriptor.StableDiagnosticCodes.IsEmpty());
	Test.TestTrue(TEXT("S: descriptor consumers default empty"), Descriptor.RequiredConsumerIds.IsEmpty());
	Test.TestFalse(TEXT("S: descriptor transaction defaults false"), Descriptor.bRequiresActiveTransaction);
	Test.TestTrue(TEXT("S: descriptor input schema defaults empty"), Descriptor.InputSchemaId.IsEmpty());
	Test.TestTrue(TEXT("S: descriptor output schema defaults empty"), Descriptor.OutputSchemaId.IsEmpty());
	Test.TestTrue(TEXT("S: descriptor mutation schema defaults empty"), Descriptor.MutationFootprintSchemaId.IsEmpty());
	TestDefaultAdmission(Test, TEXT("embedded descriptor admission"), Descriptor.Admission);
}

void TestInvocationDefaults(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveInvocation Invocation;
	Test.TestTrue(TEXT("S: invocation primitive defaults empty"), Invocation.PrimitiveId.IsEmpty());
	Test.TestEqual(TEXT("S: invocation version defaults zero"), Invocation.Version, uint32{0});
	Test.TestFalse(TEXT("S: invocation request id defaults invalid"), Invocation.RequestId.IsValid());
	Test.TestFalse(TEXT("S: invocation transaction id defaults invalid"), Invocation.TransactionId.IsValid());
	Test.TestEqual(TEXT("S: invocation revision defaults zero"), Invocation.RuntimeRevision, uint64{0});
	Test.TestEqual(TEXT("S: invocation phase defaults validate"), Invocation.Phase, EAssetDocumentRuntimeRequestPhase::Validate);
	TestDefaultRuntimeValue(Test, TEXT("embedded invocation value"), Invocation.ValueArguments);
	Test.TestTrue(TEXT("S: invocation handles default empty"), Invocation.HandleArguments.IsEmpty());
}

void TestOutcomeAndAvailabilityDefaults(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveOutcome Outcome;
	Test.TestEqual(TEXT("S: outcome defaults failed"), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestDefaultRuntimeValue(Test, TEXT("embedded outcome value"), Outcome.Value);
	Test.TestTrue(TEXT("S: outcome handle results default empty"), Outcome.HandleResults.IsEmpty());
	Test.TestTrue(TEXT("S: outcome changed set defaults empty"), Outcome.ChangedSet.IsEmpty());
	Test.TestTrue(TEXT("S: outcome diagnostics default empty"), Outcome.Diagnostics.IsEmpty());
	const FAssetDocumentProviderAvailability Availability;
	Test.TestTrue(TEXT("S: availability provider defaults empty"), Availability.ProviderId.IsEmpty());
	Test.TestEqual(TEXT("S: availability defaults unavailable"), Availability.State, EAssetDocumentProviderAvailabilityState::Unavailable);
	Test.TestFalse(TEXT("S: availability diagnostic defaults unset"), Availability.Diagnostic.IsSet());
}

void TestRecursiveDefaults(FAutomationTestBase& Test)
{
	TestAdmissionAndHandleDefaults(Test);
	TestDescriptorDefaults(Test);
	TestInvocationDefaults(Test);
	TestOutcomeAndAvailabilityDefaults(Test);
}

void TestPositiveDescriptorPaths(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveDescriptor Ordinary = MakeOrdinaryDescriptor();
	Test.TestTrue(TEXT("P: ordinary existing helper is admitted"), ValidateAssetDocumentPrimitiveDescriptor(Ordinary).bValid);
	FAssetDocumentPrimitiveDescriptor NewHelper = MakeOrdinaryDescriptor(EAssetDocumentPrimitiveAuthorityPath::NewExportedHelper);
	NewHelper.PrimitiveId = TEXT("editor.asset.apply_delta_new_helper");
	NewHelper.Admission.PrimaryOperationId = TEXT("editor.asset.apply_delta_new_helper.v1");
	NewHelper.Admission.AuthorityFingerprint = TEXT("fingerprint.editor.asset.apply_delta_new_helper.v1");
	Test.TestTrue(TEXT("P: ordinary new exported helper is admitted"), ValidateAssetDocumentPrimitiveDescriptor(NewHelper).bValid);
	FAssetDocumentPrimitiveDescriptor FrameworkLifecycle = MakeFrameworkRawDescriptor();
	Test.TestTrue(TEXT("P: framework factory raw lifecycle fixture is admitted"), ValidateAssetDocumentPrimitiveDescriptor(FrameworkLifecycle).bValid);
	FrameworkLifecycle.PrimitiveId = TEXT("editor.framework.lifecycle.create_scratch");
	RebindFrameworkRawOperation(FrameworkLifecycle, TEXT("framework.lifecycle.create_scratch.v1"));
	Test.TestTrue(TEXT("P: framework lifecycle raw fixture is admitted"), ValidateAssetDocumentPrimitiveDescriptor(FrameworkLifecycle).bValid);
	FrameworkLifecycle.PrimitiveId = TEXT("editor.framework.scratch.create_container");
	RebindFrameworkRawOperation(FrameworkLifecycle, TEXT("framework.scratch.create_container.v1"));
	Test.TestTrue(TEXT("P: framework scratch raw fixture is admitted"), ValidateAssetDocumentPrimitiveDescriptor(FrameworkLifecycle).bValid);
	Test.TestTrue(TEXT("P: admitted bridge reference shape is admitted"), ValidateAssetDocumentPrimitiveDescriptor(MakeAdmittedBridgeDescriptor()).bValid);
	Test.TestTrue(TEXT("P: stable blocker fails closed as a valid catalog descriptor"), ValidateAssetDocumentPrimitiveDescriptor(MakeStableBlockerDescriptor()).bValid);
}

void TestCallableAdmissionEvidence(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveDescriptor Base = MakeOrdinaryDescriptor();
	TestDescriptorMutationCases(Test, Base, TEXT("PrimitiveDescriptor.InvalidAdmission"), {
		{TEXT("wrong contract version"), [](auto& V) { V.Admission.ContractVersion += 1; }},
		{TEXT("missing exact Engine version"), [](auto& V) { V.Admission.ExactEngineVersion.Reset(); }},
		{TEXT("missing public module"), [](auto& V) { V.Admission.PublicModule.Reset(); }},
		{TEXT("missing public header"), [](auto& V) { V.Admission.PublicHeader.Reset(); }},
		{TEXT("missing exported authoritative symbol"), [](auto& V) { V.Admission.ExportedAuthoritativeApi.Reset(); }},
		{TEXT("missing authority context"), [](auto& V) { V.Admission.AuthorityContextId.Reset(); }},
		{TEXT("missing primary operation"), [](auto& V) { V.Admission.PrimaryOperationId.Reset(); }},
		{TEXT("missing identity/lifetime evidence"), [](auto& V) { V.Admission.IdentityLifetimeContractId.Reset(); }},
		{TEXT("missing transaction evidence"), [](auto& V) { V.Admission.TransactionContractId.Reset(); }},
		{TEXT("missing rollback evidence"), [](auto& V) { V.Admission.RollbackContractId.Reset(); }},
		{TEXT("missing completion evidence"), [](auto& V) { V.Admission.CompletionContractId.Reset(); }},
		{TEXT("missing save/reload evidence"), [](auto& V) { V.Admission.SaveReloadEvidenceId.Reset(); }},
		{TEXT("missing failure-injection evidence"), [](auto& V) { V.Admission.FailureInjectionTestIds.Reset(); }},
		{TEXT("missing verification evidence"), [](auto& V) { V.Admission.VerificationTestIds.Reset(); }},
		{TEXT("missing authority fingerprint"), [](auto& V) { V.Admission.AuthorityFingerprint.Reset(); }},
		{TEXT("duplicate failure evidence"), [](auto& V)
		{
			const FString Duplicate = V.Admission.FailureInjectionTestIds[0];
			V.Admission.FailureInjectionTestIds.Add(Duplicate);
		}},
		{TEXT("duplicate verification evidence"), [](auto& V)
		{
			const FString Duplicate = V.Admission.VerificationTestIds[0];
			V.Admission.VerificationTestIds.Add(Duplicate);
		}},
		{TEXT("empty failure evidence member"), [](auto& V) { V.Admission.FailureInjectionTestIds.Add(TEXT("")); }},
		{TEXT("empty verification evidence member"), [](auto& V) { V.Admission.VerificationTestIds.Add(TEXT("")); }},
		{TEXT("duplicate required consumer"), [](auto& V)
		{
			const FString Duplicate = V.RequiredConsumerIds[0];
			V.RequiredConsumerIds.Add(Duplicate);
		}},
		{TEXT("empty required consumer member"), [](auto& V) { V.RequiredConsumerIds.Add(TEXT("")); }},
		{TEXT("callable descriptor without required consumer"), [](auto& V) { V.RequiredConsumerIds.Reset(); }},
	});
}

void TestDescriptorIdentityRequirements(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveDescriptor Base = MakeOrdinaryDescriptor();
	TestDescriptorMutationCases(Test, Base, TEXT("PrimitiveDescriptor.InvalidIdentity"), {
		{TEXT("missing primitive id"), [](auto& V) { V.PrimitiveId.Reset(); }},
		{TEXT("zero primitive version"), [](auto& V) { V.Version = 0; }},
		{TEXT("missing provider id"), [](auto& V) { V.ProviderId.Reset(); }},
		{TEXT("invalid physical owner"), [](auto& V) { V.Admission.PhysicalOwner = static_cast<EAssetDocumentPrimitiveOwner>(255); }},
		{TEXT("mismatched physical owner"), [](auto& V) { V.Admission.PhysicalOwner = EAssetDocumentPrimitiveOwner::AI; }},
		{TEXT("identity failure precedes schema and admission"), [](auto& V)
		{
			V.PrimitiveId.Reset(); V.AllowedPhases.Reset(); V.Admission.ExactEngineVersion.Reset();
		}},
	});
}

void TestOrdinaryAndCandidatePaths(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveDescriptor Base = MakeOrdinaryDescriptor();
	FAssetDocumentPrimitiveDescriptor Candidate = Base;
	Candidate.Admission.Disposition = EAssetDocumentPrimitiveDisposition::GovernanceCandidate;
	for (uint8 Index = 0; Index <= static_cast<uint8>(EAssetDocumentPrimitiveAuthorityPath::StableBlocker); ++Index)
	{
		Candidate.Admission.AuthorityPath = static_cast<EAssetDocumentPrimitiveAuthorityPath>(Index);
		TestInvalidContract(Test, TEXT("governance candidate used as runtime descriptor"),
			ValidateAssetDocumentPrimitiveDescriptor(Candidate), TEXT("PrimitiveDescriptor.InvalidPath"));
	}
	TestDescriptorMutationCases(Test, Base, TEXT("PrimitiveDescriptor.InvalidPath"), {
		{TEXT("unknown disposition"), [](auto& V) { V.Admission.Disposition = static_cast<EAssetDocumentPrimitiveDisposition>(255); }},
		{TEXT("unknown authority path"), [](auto& V) { V.Admission.AuthorityPath = static_cast<EAssetDocumentPrimitiveAuthorityPath>(255); }},
		{TEXT("ordinary source-marker leakage"), [](auto& V) { V.Admission.SourceMarker = TEXT("marker"); }},
		{TEXT("ordinary raw-allowlist leakage"), [](auto& V) { V.Admission.RawLifecycleAllowlistId = TEXT("allowlist"); }},
		{TEXT("ordinary governance leakage"), [](auto& V) { V.Admission.GovernanceId = TEXT("governance"); }},
		{TEXT("ordinary candidate-review leakage"), [](auto& V) { V.Admission.IndependentCandidateReviewId = TEXT("candidate-review"); }},
		{TEXT("ordinary bridge-review leakage"), [](auto& V) { V.Admission.IndependentBridgeReviewId = TEXT("bridge-review"); }},
		{TEXT("ordinary replacement-condition leakage"), [](auto& V) { V.Admission.HelperReplacementCondition = TEXT("replacement"); }},
		{TEXT("ordinary removal-condition leakage"), [](auto& V) { V.Admission.RemovalCondition = TEXT("removal"); }},
		{TEXT("illegal ordinary raw path"), [](auto& V) { V.Admission.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::FrameworkRawLifecycleAllowlist; }},
		{TEXT("illegal ordinary bridge path"), [](auto& V) { V.Admission.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::BridgeAdmitted; }},
		{TEXT("illegal ordinary blocker path"), [](auto& V) { V.Admission.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::StableBlocker; }},
	});
}

void TestFrameworkRawPath(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveDescriptor Base = MakeFrameworkRawDescriptor();
	TestIllegalAuthorityPaths(Test, TEXT("illegal framework-raw disposition/path pair"),
		Base, EAssetDocumentPrimitiveAuthorityPath::FrameworkRawLifecycleAllowlist);
	TestDescriptorMutationCases(Test, Base, TEXT("PrimitiveDescriptor.InvalidPath"), {
		{TEXT("raw fallback marker"), [](auto& V) { V.Admission.SourceMarker = TEXT("raw-fallback-wrapper"); }},
		{TEXT("framework raw without source marker"), [](auto& V) { V.Admission.SourceMarker.Reset(); }},
		{TEXT("framework raw without per-callsite allowlist"), [](auto& V) { V.Admission.RawLifecycleAllowlistId.Reset(); }},
		{TEXT("framework raw without replacement condition"), [](auto& V) { V.Admission.HelperReplacementCondition.Reset(); }},
		{TEXT("framework raw without removal condition"), [](auto& V) { V.Admission.RemovalCondition.Reset(); }},
		{TEXT("framework raw governance leakage"), [](auto& V) { V.Admission.GovernanceId = TEXT("governance"); }},
		{TEXT("framework raw candidate-review leakage"), [](auto& V) { V.Admission.IndependentCandidateReviewId = TEXT("candidate-review"); }},
		{TEXT("framework raw bridge-review leakage"), [](auto& V) { V.Admission.IndependentBridgeReviewId = TEXT("bridge-review"); }},
		{TEXT("framework raw marker bound to another operation"), [](auto& V)
		{
			V.Admission.SourceMarker = TEXT("ASSETDOCUMENT_FRAMEWORK_RAW_LIFECYCLE_ALLOWLIST(framework.factory.some_other.v1)");
		}},
		{TEXT("framework raw allowlist bound to another callsite"), [](auto& V)
		{
			V.Admission.RawLifecycleAllowlistId = TEXT("ASSETDOCUMENT_FRAMEWORK_RAW_CALLSITE(framework.factory.some_other.v1)");
		}},
		{TEXT("non-framework raw operation"), [](auto& V) { RebindFrameworkRawOperation(V, TEXT("umg.widget.create.v1")); }},
		{TEXT("domain-owned framework raw lifecycle"), [](auto& V)
		{
			V.Owner = EAssetDocumentPrimitiveOwner::UMG; V.Admission.PhysicalOwner = EAssetDocumentPrimitiveOwner::UMG;
		}},
	});
}

void TestBridgePath(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveDescriptor Base = MakeAdmittedBridgeDescriptor();
	TestIllegalAuthorityPaths(Test, TEXT("illegal bridge disposition/path pair"),
		Base, EAssetDocumentPrimitiveAuthorityPath::BridgeAdmitted);
	TestDescriptorMutationCases(Test, Base, TEXT("PrimitiveDescriptor.InvalidPath"), {
		{TEXT("bridge without source marker"), [](auto& V) { V.Admission.SourceMarker.Reset(); }},
		{TEXT("bridge without exact allowlist"), [](auto& V) { V.Admission.RawLifecycleAllowlistId.Reset(); }},
		{TEXT("bridge without governance reference"), [](auto& V) { V.Admission.GovernanceId.Reset(); }},
		{TEXT("bridge without candidate review reference"), [](auto& V) { V.Admission.IndependentCandidateReviewId.Reset(); }},
		{TEXT("bridge without independent bridge review reference"), [](auto& V) { V.Admission.IndependentBridgeReviewId.Reset(); }},
		{TEXT("bridge without replacement condition"), [](auto& V) { V.Admission.HelperReplacementCondition.Reset(); }},
		{TEXT("bridge without removal condition"), [](auto& V) { V.Admission.RemovalCondition.Reset(); }},
		{TEXT("bridge marker bound to another governance id"), [](auto& V)
		{
			V.Admission.SourceMarker = TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_ADMISSION(OTHER-GOVERNANCE-v1)");
		}},
		{TEXT("bridge allowlist bound to another governance id"), [](auto& V)
		{
			V.Admission.RawLifecycleAllowlistId = TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_CALLSITE(OTHER-GOVERNANCE-v1|umg.widget_animation.create.v1)");
		}},
		{TEXT("bridge allowlist bound to another operation"), [](auto& V)
		{
			V.Admission.RawLifecycleAllowlistId = TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_CALLSITE(UMG-WIDGET-ANIMATION-CREATE-v1|umg.widget_animation.delete.v1)");
		}},
	});
}

void TestBlockerPathAndEvidence(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveDescriptor Base = MakeStableBlockerDescriptor();
	TestIllegalAuthorityPaths(Test, TEXT("illegal blocker disposition/path pair"),
		Base, EAssetDocumentPrimitiveAuthorityPath::StableBlocker);
	TestDescriptorMutationCases(Test, Base, TEXT("PrimitiveDescriptor.InvalidPath"), {
		{TEXT("blocker required-consumer leakage"), [](auto& V) { V.RequiredConsumerIds.Add(TEXT("python.widget_blueprint")); }},
		{TEXT("blocker callable-symbol leakage"), [](auto& V) { V.Admission.ExportedAuthoritativeApi = TEXT("ForbiddenCallableSymbol"); }},
		{TEXT("blocker source-marker leakage"), [](auto& V) { V.Admission.SourceMarker = TEXT("marker"); }},
		{TEXT("blocker raw-allowlist leakage"), [](auto& V) { V.Admission.RawLifecycleAllowlistId = TEXT("allowlist"); }},
		{TEXT("blocker governance leakage"), [](auto& V) { V.Admission.GovernanceId = TEXT("governance"); }},
		{TEXT("blocker candidate-review leakage"), [](auto& V) { V.Admission.IndependentCandidateReviewId = TEXT("candidate-review"); }},
		{TEXT("blocker bridge-review leakage"), [](auto& V) { V.Admission.IndependentBridgeReviewId = TEXT("bridge-review"); }},
		{TEXT("blocker replacement-condition leakage"), [](auto& V) { V.Admission.HelperReplacementCondition = TEXT("replacement"); }},
		{TEXT("blocker removal-condition leakage"), [](auto& V) { V.Admission.RemovalCondition = TEXT("removal"); }},
	});
	FAssetDocumentPrimitiveDescriptor WithEvidence = Base;
	WithEvidence.Admission.FailureInjectionTestIds = {TEXT("failure.blocker.v1")};
	WithEvidence.Admission.VerificationTestIds = {TEXT("verify.blocker.v1")};
	Test.TestTrue(TEXT("P: blocker may carry structurally valid optional test evidence"),
		ValidateAssetDocumentPrimitiveDescriptor(WithEvidence).bValid);
	TestDescriptorMutationCases(Test, Base, TEXT("PrimitiveDescriptor.InvalidAdmission"), {
		{TEXT("blocker duplicate failure evidence"), [](auto& V) { V.Admission.FailureInjectionTestIds = {TEXT("failure.blocker.v1"), TEXT("failure.blocker.v1")}; }},
		{TEXT("blocker empty failure evidence member"), [](auto& V) { V.Admission.FailureInjectionTestIds = {TEXT("")}; }},
		{TEXT("blocker duplicate verification evidence"), [](auto& V) { V.Admission.VerificationTestIds = {TEXT("verify.blocker.v1"), TEXT("verify.blocker.v1")}; }},
		{TEXT("blocker empty verification evidence member"), [](auto& V) { V.Admission.VerificationTestIds = {TEXT("")}; }},
	});
}

void TestDescriptorSchemas(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveDescriptor Base = MakeOrdinaryDescriptor();
	TestDescriptorMutationCases(Test, Base, TEXT("PrimitiveDescriptor.InvalidSchema"), {
		{TEXT("duplicate allowed handle kind"), [](auto& V) { V.AllowedHandleKinds.Add(EAssetDocumentHandleKind::Asset); }},
		{TEXT("duplicate handle schema name"), [](auto& V)
		{
			const FAssetDocumentPrimitiveHandleArgumentSchema Duplicate = V.HandleArgumentSchemas[0]; V.HandleArgumentSchemas.Add(Duplicate);
		}},
		{TEXT("schema kind outside allowed handle kinds"), [](auto& V) { V.HandleArgumentSchemas[0].Kind = EAssetDocumentHandleKind::Graph; }},
		{TEXT("ambiguous required handle name"), [](auto& V) { V.HandleArgumentSchemas[0].Name.Reset(); }},
		{TEXT("invalid handle schema kind enum"), [](auto& V) { V.HandleArgumentSchemas[0].Kind = static_cast<EAssetDocumentHandleKind>(255); }},
		{TEXT("invalid handle schema access enum"), [](auto& V) { V.HandleArgumentSchemas[0].RequiredAccess = static_cast<EAssetDocumentHandleAccess>(255); }},
		{TEXT("duplicate handle result schema name"), [](auto& V)
		{
			const FAssetDocumentPrimitiveHandleResultSchema Duplicate = V.HandleResultSchemas[0];
			V.HandleResultSchemas.Add(Duplicate);
		}},
		{TEXT("empty handle result schema name"), [](auto& V) { V.HandleResultSchemas[0].Name.Reset(); }},
		{TEXT("handle result schema kind outside allowed kinds"), [](auto& V) { V.HandleResultSchemas[0].Kind = EAssetDocumentHandleKind::Graph; }},
		{TEXT("invalid handle result schema kind enum"), [](auto& V) { V.HandleResultSchemas[0].Kind = static_cast<EAssetDocumentHandleKind>(255); }},
		{TEXT("invalid handle result schema access enum"), [](auto& V) { V.HandleResultSchemas[0].GrantedAccess = static_cast<EAssetDocumentHandleAccess>(255); }},
		{TEXT("invalid handle result cardinality enum"), [](auto& V) { V.HandleResultSchemas[0].Cardinality = static_cast<EAssetDocumentHandleResultCardinality>(255); }},
	});
}

void TestDescriptorDuplicateAuthority(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveDescriptor Ordinary = MakeOrdinaryDescriptor();
	FAssetDocumentPrimitiveDescriptor SemanticDuplicate = Ordinary;
	SemanticDuplicate.PrimitiveId = TEXT("editor.asset.alias_apply_delta");
	SemanticDuplicate.Admission.AuthorityFingerprint = TEXT("caller-provided-different-fingerprint");
	TestInvalidContract(Test, TEXT("duplicate computed authority fingerprint"),
		ValidateAssetDocumentPrimitiveDescriptor(SemanticDuplicate, MakeArrayView(&Ordinary, 1)),
		TEXT("PrimitiveDescriptor.DuplicateAuthority"));
	FAssetDocumentPrimitiveDescriptor DuplicateIdentity = Ordinary;
	DuplicateIdentity.Admission.PrimaryOperationId = TEXT("editor.asset.other_operation.v1");
	DuplicateIdentity.Admission.AuthorityFingerprint = TEXT("fingerprint.other.v1");
	TestInvalidContract(Test, TEXT("duplicate primitive identity"),
		ValidateAssetDocumentPrimitiveDescriptor(DuplicateIdentity, MakeArrayView(&Ordinary, 1)),
		TEXT("PrimitiveDescriptor.DuplicateIdentity"));
}

void TestReorderedSemanticAuthority(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveDescriptor Ordinary = MakeOrdinaryDescriptor();
	FAssetDocumentPrimitiveDescriptor Base = Ordinary;
	Base.PrimitiveId = TEXT("editor.asset.multi_handle_apply");
	Base.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Preflight, EAssetDocumentRuntimeRequestPhase::Apply};
	Base.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset, EAssetDocumentHandleKind::Graph};
	Base.HandleArgumentSchemas.Add({TEXT("graph"), EAssetDocumentHandleKind::Graph, EAssetDocumentHandleAccess::ReadOnly, false});
	Base.Admission.PrimaryOperationId = TEXT("editor.asset.multi_handle_apply.v1");
	Base.Admission.AuthorityFingerprint = TEXT("fingerprint.editor.asset.multi_handle_apply.v1");
	Test.TestTrue(TEXT("P: multi-element semantic fingerprint base is valid"),
		ValidateAssetDocumentPrimitiveDescriptor(Base).bValid);
	FAssetDocumentPrimitiveDescriptor Reordered = Base;
	Reordered.PrimitiveId = TEXT("editor.asset.multi_handle_apply_alias");
	Reordered.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Preflight};
	Reordered.AllowedHandleKinds = {EAssetDocumentHandleKind::Graph, EAssetDocumentHandleKind::Asset};
	Reordered.HandleArgumentSchemas = {Base.HandleArgumentSchemas[1], Base.HandleArgumentSchemas[0]};
	Reordered.Admission.AuthorityFingerprint = TEXT("caller-provided-reordered-fingerprint");
	TestInvalidContract(Test, TEXT("reordered multi-element computed authority fingerprint"),
		ValidateAssetDocumentPrimitiveDescriptor(Reordered, MakeArrayView(&Base, 1)),
		TEXT("PrimitiveDescriptor.DuplicateAuthority"));
	FAssetDocumentPrimitiveDescriptor Distinct = Ordinary;
	Distinct.PrimitiveId = TEXT("editor.asset.distinct_operation");
	Distinct.Admission.PrimaryOperationId = TEXT("editor.asset.distinct_operation.v1");
	Test.TestTrue(TEXT("P: caller fingerprint string cannot create semantic duplication"),
		ValidateAssetDocumentPrimitiveDescriptor(Distinct, MakeArrayView(&Ordinary, 1)).bValid);
}

void TestInvocationIdentityAndOptionalHandle(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveDescriptor Ordinary = MakeOrdinaryDescriptor();
	const FAssetDocumentPrimitiveInvocation Invocation = MakeValidInvocation(Ordinary);
	Test.TestTrue(TEXT("P: valid typed invocation is admitted"), ValidateAssetDocumentPrimitiveInvocation(Invocation, Ordinary).bValid);
	TestInvocationMutationCases(Test, Ordinary, Invocation, TEXT("PrimitiveInvocation.InvalidIdentity"), {
		{TEXT("invocation primitive id mismatch"), [](auto& V) { V.PrimitiveId = TEXT("editor.asset.other"); }},
		{TEXT("invocation version mismatch"), [](auto& V) { V.Version += 1; }},
		{TEXT("invocation missing request id"), [](auto& V) { V.RequestId.Invalidate(); }},
		{TEXT("invocation missing runtime revision"), [](auto& V) { V.RuntimeRevision = 0; }},
		{TEXT("invocation disallowed phase"), [](auto& V) { V.Phase = EAssetDocumentRuntimeRequestPhase::Verify; }},
		{TEXT("invocation missing required transaction"), [](auto& V) { V.TransactionId.Invalidate(); }},
		{TEXT("invocation identity failure precedes value and handles"), [](auto& V)
		{
			V.PrimitiveId = TEXT("editor.asset.other"); V.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Null; V.HandleArguments.Reset();
		}},
	});
	const FAssetDocumentPrimitiveDescriptor Blocker = MakeStableBlockerDescriptor();
	const FAssetDocumentPrimitiveInvocation BlockerInvocation = MakeValidInvocation(Blocker);
	TestInvalidContract(Test, TEXT("stable blocker invocation"),
		ValidateAssetDocumentPrimitiveInvocation(BlockerInvocation, Blocker), TEXT("PrimitiveInvocation.InvalidIdentity"));
	FAssetDocumentPrimitiveDescriptor OptionalDescriptor = Ordinary;
	OptionalDescriptor.PrimitiveId = TEXT("editor.asset.apply_with_optional_graph");
	OptionalDescriptor.AllowedHandleKinds.Add(EAssetDocumentHandleKind::Graph);
	OptionalDescriptor.HandleArgumentSchemas.Add({TEXT("graph"), EAssetDocumentHandleKind::Graph, EAssetDocumentHandleAccess::ReadOnly, false});
	OptionalDescriptor.Admission.PrimaryOperationId = TEXT("editor.asset.apply_with_optional_graph.v1");
	OptionalDescriptor.Admission.AuthorityFingerprint = TEXT("fingerprint.editor.asset.apply_with_optional_graph.v1");
	FAssetDocumentPrimitiveInvocation Optional = MakeValidInvocation(OptionalDescriptor);
	Test.TestTrue(TEXT("P: optional native handle may be absent"), ValidateAssetDocumentPrimitiveInvocation(Optional, OptionalDescriptor).bValid);
	FAssetDocumentOpaqueHandle GraphHandle;
	GraphHandle.Kind = EAssetDocumentHandleKind::Graph;
	GraphHandle.Access = EAssetDocumentHandleAccess::ReadOnly;
	Optional.HandleArguments.Add({TEXT("graph"), GraphHandle});
	Test.TestTrue(TEXT("P: optional native handle may be present with exact kind/access"),
		ValidateAssetDocumentPrimitiveInvocation(Optional, OptionalDescriptor).bValid);
}

void TestInvocationHandleArguments(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor();
	const FAssetDocumentPrimitiveInvocation Base = MakeValidInvocation(Descriptor);
	TestInvocationMutationCases(Test, Descriptor, Base, TEXT("PrimitiveInvocation.InvalidHandleArguments"), {
		{TEXT("duplicate invocation handle name"), [](auto& V)
		{
			const FAssetDocumentPrimitiveHandleArgument Duplicate = V.HandleArguments[0]; V.HandleArguments.Add(Duplicate);
		}},
		{TEXT("unknown invocation handle name"), [](auto& V) { V.HandleArguments[0].Name = TEXT("unknown"); }},
		{TEXT("missing required invocation handle"), [](auto& V) { V.HandleArguments.Reset(); }},
		{TEXT("invocation handle kind mismatch"), [](auto& V) { V.HandleArguments[0].Handle.Kind = EAssetDocumentHandleKind::Graph; }},
		{TEXT("invocation handle access mismatch"), [](auto& V) { V.HandleArguments[0].Handle.Access = EAssetDocumentHandleAccess::ReadOnly; }},
		{TEXT("invocation invalid handle kind enum"), [](auto& V) { V.HandleArguments[0].Handle.Kind = static_cast<EAssetDocumentHandleKind>(255); }},
		{TEXT("invocation invalid handle access enum"), [](auto& V) { V.HandleArguments[0].Handle.Access = static_cast<EAssetDocumentHandleAccess>(255); }},
		{TEXT("value and handle argument name collision"), [](auto& V)
		{
			V.ValueArguments.ObjectKeys.Add(TEXT("asset")); V.ValueArguments.Elements.Add(MakeStringValue(TEXT("collision")));
		}},
	});
}

void TestJsonHandleSubstitution(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor();
	FAssetDocumentPrimitiveInvocation Invocation = MakeValidInvocation(Descriptor);
	Invocation.HandleArguments.Reset();
	FAssetDocumentRuntimeValueDto JsonHandle;
	JsonHandle.Kind = EAssetDocumentRuntimeValueKind::Object;
	JsonHandle.ObjectKeys = {
		TEXT("RequestId"), TEXT("TransactionId"), TEXT("RuntimeRevision"), TEXT("ProviderId"),
		TEXT("Slot"), TEXT("Generation"), TEXT("Kind"), TEXT("Access")};
	JsonHandle.Elements = {
		MakeStringValue(TEXT("00000000-0000-0000-0000-000000000001")),
		MakeStringValue(TEXT("00000000-0000-0000-0000-000000000002")),
		MakeIntegerValue(7), MakeStringValue(TEXT("editor")), MakeIntegerValue(1), MakeIntegerValue(1),
		MakeIntegerValue(static_cast<int64>(EAssetDocumentHandleKind::Asset)),
		MakeIntegerValue(static_cast<int64>(EAssetDocumentHandleAccess::Mutable))};
	Invocation.ValueArguments.ObjectKeys.Add(TEXT("asset"));
	Invocation.ValueArguments.Elements.Add(JsonHandle);
	TestInvalidContract(Test, TEXT("complete eight-field JSON-like handle substitute"),
		ValidateAssetDocumentPrimitiveInvocation(Invocation, Descriptor), TEXT("PrimitiveInvocation.InvalidHandleArguments"));
}

void TestInvocationValueArguments(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor();
	const FAssetDocumentPrimitiveInvocation Base = MakeValidInvocation(Descriptor);
	TestInvocationMutationCases(Test, Descriptor, Base, TEXT("PrimitiveInvocation.InvalidValueArguments"), {
		{TEXT("non-object value arguments"), [](auto& V) { V.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Null; }},
		{TEXT("invalid value arguments precede handle validation"), [](auto& V)
		{
			V.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Null; V.HandleArguments.Reset();
		}},
		{TEXT("T01-invalid value arguments"), [](auto& V)
		{
			V.ValueArguments.Elements[0].Kind = EAssetDocumentRuntimeValueKind::Number;
			V.ValueArguments.Elements[0].NumberValue = std::numeric_limits<double>::infinity();
			V.ValueArguments.Elements[0].StringValue.Reset();
		}},
	});
}

void TestOutcomeIdentityAndValue(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor();
	const FAssetDocumentPrimitiveInvocation Invocation = MakeValidInvocation(Descriptor);
	const FAssetDocumentPrimitiveOutcome Base = MakeValidOutcome(Invocation, Descriptor);
	Test.TestTrue(TEXT("P: valid primitive outcome is admitted"),
		ValidateAssetDocumentPrimitiveOutcome(Invocation, Descriptor, Base).bValid);
	TestOutcomeMutationCases(Test, Invocation, Descriptor, Base, TEXT("PrimitiveOutcome.InvalidValue"), {
		{TEXT("non-object outcome value"), [](auto& V) { V.Value.Kind = EAssetDocumentRuntimeValueKind::Null; }},
		{TEXT("T01-invalid outcome value"), [](auto& V)
		{
			V.Value.Elements[0].Kind = EAssetDocumentRuntimeValueKind::Number;
			V.Value.Elements[0].NumberValue = std::numeric_limits<double>::infinity();
			V.Value.Elements[0].StringValue.Reset();
		}},
	});
	TestOutcomeMutationCases(Test, Invocation, Descriptor, Base, TEXT("PrimitiveOutcome.InvalidHandleResults"), {
		{TEXT("duplicate handle-result group"), [](auto& V)
		{
			const FAssetDocumentPrimitiveHandleResult Duplicate = V.HandleResults[0];
			V.HandleResults.Add(Duplicate);
		}},
		{TEXT("unknown handle-result group"), [](auto& V) { V.HandleResults[0].Name = TEXT("unknown"); }},
		{TEXT("missing required handle-result group"), [](auto& V) { V.HandleResults.Reset(); }},
	});
	FAssetDocumentPrimitiveOutcome ReusedLaneName = Base;
	ReusedLaneName.Value.ObjectKeys.Add(TEXT("asset_result"));
	ReusedLaneName.Value.Elements.Add(MakeStringValue(TEXT("lane-local value")));
	Test.TestTrue(
		TEXT("outcome value and handle-result lanes may reuse one name"),
		ValidateAssetDocumentPrimitiveOutcome(
			Invocation, Descriptor, ReusedLaneName).bValid);
}

void TestOutcomeCardinalityValidRepresentatives(FAutomationTestBase& Test)
{
	const EAssetDocumentHandleResultCardinality Cardinalities[] = {
		EAssetDocumentHandleResultCardinality::ExactlyOne,
		EAssetDocumentHandleResultCardinality::ZeroOrOne,
		EAssetDocumentHandleResultCardinality::ZeroOrMore,
		EAssetDocumentHandleResultCardinality::OneOrMore,
	};
	for (EAssetDocumentHandleResultCardinality Cardinality : Cardinalities)
	{
		FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor();
		Descriptor.HandleResultSchemas[0].Cardinality = Cardinality;
		const FAssetDocumentPrimitiveInvocation Invocation = MakeValidInvocation(Descriptor);
		FAssetDocumentPrimitiveOutcome Outcome = MakeValidOutcome(Invocation, Descriptor);
		if (Cardinality == EAssetDocumentHandleResultCardinality::ZeroOrMore
			|| Cardinality == EAssetDocumentHandleResultCardinality::OneOrMore)
		{
			Outcome.HandleResults[0].Handles.Add(MakeResultHandle(Invocation, Descriptor, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable, 2));
		}
		Test.TestTrue(TEXT("P: each cardinality admits its representative valid count"),
			ValidateAssetDocumentPrimitiveOutcome(Invocation, Descriptor, Outcome).bValid);
	}
}

void TestOptionalOutcomeCardinalityOmission(FAutomationTestBase& Test)
{
	for (EAssetDocumentHandleResultCardinality Cardinality : {
		EAssetDocumentHandleResultCardinality::ZeroOrOne,
		EAssetDocumentHandleResultCardinality::ZeroOrMore})
	{
		FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor();
		Descriptor.HandleResultSchemas[0].Cardinality = Cardinality;
		const FAssetDocumentPrimitiveInvocation Invocation = MakeValidInvocation(Descriptor);
		FAssetDocumentPrimitiveOutcome Omitted = MakeValidOutcome(Invocation, Descriptor);
		Omitted.HandleResults.Reset();
		Test.TestTrue(TEXT("P: optional zero-cardinality result group may be omitted"),
			ValidateAssetDocumentPrimitiveOutcome(Invocation, Descriptor, Omitted).bValid);
		FAssetDocumentPrimitiveOutcome Empty = MakeValidOutcome(Invocation, Descriptor);
		Empty.HandleResults[0].Handles.Reset();
		Test.TestTrue(TEXT("P: optional zero-cardinality result group may be explicitly empty"),
			ValidateAssetDocumentPrimitiveOutcome(Invocation, Descriptor, Empty).bValid);
	}
}

void TestRequiredOutcomeCardinalityPresence(FAutomationTestBase& Test)
{
	for (EAssetDocumentHandleResultCardinality Cardinality : {
		EAssetDocumentHandleResultCardinality::ExactlyOne,
		EAssetDocumentHandleResultCardinality::OneOrMore})
	{
		FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor();
		Descriptor.HandleResultSchemas[0].Cardinality = Cardinality;
		const FAssetDocumentPrimitiveInvocation Invocation = MakeValidInvocation(Descriptor);
		FAssetDocumentPrimitiveOutcome Missing = MakeValidOutcome(Invocation, Descriptor);
		Missing.HandleResults.Reset();
		TestInvalidContract(Test, TEXT("required nonempty handle-result group omitted"),
			ValidateAssetDocumentPrimitiveOutcome(Invocation, Descriptor, Missing),
			TEXT("PrimitiveOutcome.InvalidHandleResults"));
		FAssetDocumentPrimitiveOutcome Empty = MakeValidOutcome(Invocation, Descriptor);
		Empty.HandleResults[0].Handles.Reset();
		TestInvalidContract(Test, TEXT("required nonempty handle-result group empty"),
			ValidateAssetDocumentPrimitiveOutcome(Invocation, Descriptor, Empty),
			TEXT("PrimitiveOutcome.InvalidHandleResults"));
	}
}

void TestOutcomeCardinalityUpperBounds(FAutomationTestBase& Test)
{
	FAssetDocumentPrimitiveDescriptor ZeroOrOne = MakeOrdinaryDescriptor();
	ZeroOrOne.HandleResultSchemas[0].Cardinality = EAssetDocumentHandleResultCardinality::ZeroOrOne;
	const FAssetDocumentPrimitiveInvocation Invocation = MakeValidInvocation(ZeroOrOne);
	FAssetDocumentPrimitiveOutcome TooMany = MakeValidOutcome(Invocation, ZeroOrOne);
	TooMany.HandleResults[0].Handles.Add(MakeResultHandle(Invocation, ZeroOrOne, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable, 2));
	TestInvalidContract(Test, TEXT("zero-or-one handle-result group returns two"),
		ValidateAssetDocumentPrimitiveOutcome(Invocation, ZeroOrOne, TooMany),
		TEXT("PrimitiveOutcome.InvalidHandleResults"));

	FAssetDocumentPrimitiveDescriptor ExactlyOne = MakeOrdinaryDescriptor();
	const FAssetDocumentPrimitiveInvocation ExactInvocation = MakeValidInvocation(ExactlyOne);
	FAssetDocumentPrimitiveOutcome ExactTooMany = MakeValidOutcome(ExactInvocation, ExactlyOne);
	ExactTooMany.HandleResults[0].Handles.Add(MakeResultHandle(
		ExactInvocation, ExactlyOne, EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable, 2));
	TestInvalidContract(Test, TEXT("exactly-one handle-result group returns two"),
		ValidateAssetDocumentPrimitiveOutcome(ExactInvocation, ExactlyOne, ExactTooMany),
		TEXT("PrimitiveOutcome.InvalidHandleResults"));
}

void TestOutcomeCardinality(FAutomationTestBase& Test)
{
	TestOutcomeCardinalityValidRepresentatives(Test);
	TestOptionalOutcomeCardinalityOmission(Test);
	TestRequiredOutcomeCardinalityPresence(Test);
	TestOutcomeCardinalityUpperBounds(Test);
}

void TestOutcomeHandleMetadata(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor();
	const FAssetDocumentPrimitiveInvocation Invocation = MakeValidInvocation(Descriptor);
	const FAssetDocumentPrimitiveOutcome Base = MakeValidOutcome(Invocation, Descriptor);
	TestOutcomeMutationCases(Test, Invocation, Descriptor, Base, TEXT("PrimitiveOutcome.InvalidHandleResults"), {
		{TEXT("result handle kind mismatch"), [](auto& V) { V.HandleResults[0].Handles[0].Kind = EAssetDocumentHandleKind::Graph; }},
		{TEXT("result handle access mismatch"), [](auto& V) { V.HandleResults[0].Handles[0].Access = EAssetDocumentHandleAccess::ReadOnly; }},
		{TEXT("result handle request mismatch"), [](auto& V) { V.HandleResults[0].Handles[0].RequestId = FGuid::NewGuid(); }},
		{TEXT("result handle transaction mismatch"), [](auto& V) { V.HandleResults[0].Handles[0].TransactionId = FGuid::NewGuid(); }},
		{TEXT("result handle revision mismatch"), [](auto& V) { V.HandleResults[0].Handles[0].RuntimeRevision += 1; }},
		{TEXT("result handle provider mismatch"), [](auto& V) { V.HandleResults[0].Handles[0].ProviderId = TEXT("other"); }},
		{TEXT("result handle zero slot"), [](auto& V) { V.HandleResults[0].Handles[0].Slot = 0; }},
		{TEXT("result handle zero generation"), [](auto& V) { V.HandleResults[0].Handles[0].Generation = 0; }},
	});

	FAssetDocumentPrimitiveDescriptor Many = Descriptor;
	Many.HandleResultSchemas[0].Cardinality = EAssetDocumentHandleResultCardinality::ZeroOrMore;
	const FAssetDocumentPrimitiveInvocation ManyInvocation = MakeValidInvocation(Many);
	FAssetDocumentPrimitiveOutcome Repeated = MakeValidOutcome(ManyInvocation, Many);
	const FAssetDocumentOpaqueHandle Duplicate = Repeated.HandleResults[0].Handles[0];
	Repeated.HandleResults[0].Handles.Add(Duplicate);
	TestInvalidContract(Test, TEXT("repeated result handle identity"),
		ValidateAssetDocumentPrimitiveOutcome(ManyInvocation, Many, Repeated),
		TEXT("PrimitiveOutcome.InvalidHandleResults"));

	Many.HandleResultSchemas.Add({
		TEXT("audit_result"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable, EAssetDocumentHandleResultCardinality::ZeroOrMore});
	FAssetDocumentPrimitiveOutcome RepeatedAcrossGroups = MakeValidOutcome(ManyInvocation, Many);
	RepeatedAcrossGroups.HandleResults.Add({TEXT("audit_result"), {Duplicate}});
	TestInvalidContract(Test, TEXT("repeated result handle identity across named groups"),
		ValidateAssetDocumentPrimitiveOutcome(ManyInvocation, Many, RepeatedAcrossGroups),
		TEXT("PrimitiveOutcome.InvalidHandleResults"));
}

void TestOutcomeFailureAndBounds(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor();
	const FAssetDocumentPrimitiveInvocation Invocation = MakeValidInvocation(Descriptor);
	for (EAssetDocumentPrimitiveOutcomeStatus Status : {
		EAssetDocumentPrimitiveOutcomeStatus::Failed,
		EAssetDocumentPrimitiveOutcomeStatus::Unavailable})
	{
		FAssetDocumentPrimitiveOutcome Leaking = MakeValidOutcome(Invocation, Descriptor);
		Leaking.Status = Status;
		TestInvalidContract(Test, TEXT("failed or unavailable outcome leaks handles"),
			ValidateAssetDocumentPrimitiveOutcome(Invocation, Descriptor, Leaking),
			TEXT("PrimitiveOutcome.HandleLeakOnFailure"));
		Leaking.HandleResults.Reset();
		Test.TestTrue(TEXT("P: failed or unavailable outcome without handles is admitted"),
			ValidateAssetDocumentPrimitiveOutcome(Invocation, Descriptor, Leaking).bValid);
	}

	FAssetDocumentPrimitiveDescriptor Many = Descriptor;
	Many.HandleResultSchemas[0].Cardinality = EAssetDocumentHandleResultCardinality::ZeroOrMore;
	const FAssetDocumentPrimitiveInvocation ManyInvocation = MakeValidInvocation(Many);
	FAssetDocumentPrimitiveOutcome Oversized = MakeValidOutcome(ManyInvocation, Many);
	Oversized.HandleResults[0].Handles.SetNum(4097);
	TestInvalidContract(Test, TEXT("handle result count exceeds contract bound before item validation"),
		ValidateAssetDocumentPrimitiveOutcome(ManyInvocation, Many, Oversized),
		TEXT("PrimitiveOutcome.HandleResultLimitExceeded"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPrimitiveProviderContractTest,
	"UEAssetDoc.RuntimeContracts.PrimitiveProvider",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPrimitiveProviderContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestRecursiveDefaults(*this);
	TestPositiveDescriptorPaths(*this);
	TestCallableAdmissionEvidence(*this);
	TestDescriptorIdentityRequirements(*this);
	TestOrdinaryAndCandidatePaths(*this);
	TestFrameworkRawPath(*this);
	TestBridgePath(*this);
	TestBlockerPathAndEvidence(*this);
	TestDescriptorSchemas(*this);
	TestDescriptorDuplicateAuthority(*this);
	TestReorderedSemanticAuthority(*this);
	TestInvocationIdentityAndOptionalHandle(*this);
	TestInvocationHandleArguments(*this);
	TestJsonHandleSubstitution(*this);
	TestInvocationValueArguments(*this);
	TestOutcomeIdentityAndValue(*this);
	TestOutcomeCardinality(*this);
	TestOutcomeHandleMetadata(*this);
	TestOutcomeFailureAndBounds(*this);

	return true;
}
} // namespace AssetDocumentPrimitiveProviderContractTests

#endif // WITH_DEV_AUTOMATION_TESTS
