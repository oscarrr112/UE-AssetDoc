// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentPrimitiveRegistryRuntime.h"
#include "AssetDocumentPythonHostTestDoubles.h"
#include "Misc/AutomationTest.h"

namespace
{
FAssetDocumentPrimitiveRegistryBuildInput MakeBuildInput(
	TArray<FAssetDocumentPrimitiveDescriptor> Descriptors,
	EAssetDocumentProviderAvailabilityState State = EAssetDocumentProviderAvailabilityState::Available)
{
	FAssetDocumentPrimitiveRegistryBuildInput Input;
	Input.RuntimeRevision = 1;
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
	{
		Input.Required.Add(MakeAssetDocumentHostTestRequirement(Descriptor));
	}
	Input.Providers.Add(MakeShared<FAssetDocumentHostTestProvider>(TEXT("editor"), MoveTemp(Descriptors), State));
	return Input;
}

void TestRejected(
	FAutomationTestBase& Test,
	const TCHAR* What,
	FAssetDocumentPrimitiveRegistryBuildInput Input,
	const TCHAR* ExpectedCode)
{
	const FAssetDocumentPrimitiveRegistryBuildResult Result =
		FAssetDocumentPrimitiveRegistryRuntime::CreateFrozen(Input);
	Test.TestFalse(What, Result.Registry.IsValid());
	Test.TestEqual(TEXT("one diagnostic"), Result.Diagnostics.Num(), 1);
	Test.TestEqual(TEXT("frozen validation code is preserved"), Result.Diagnostics[0].Code, FString(ExpectedCode));
}

void TestIncompleteAndDuplicates(FAutomationTestBase& Test)
{
	FAssetDocumentPrimitiveDescriptor Incomplete = MakeAssetDocumentHostTestDescriptor();
	Incomplete.Admission.PublicHeader.Reset();
	TestRejected(Test, TEXT("incomplete descriptor does not freeze"), MakeBuildInput({Incomplete}),
		TEXT("PrimitiveDescriptor.InvalidAdmission"));

	FAssetDocumentPrimitiveDescriptor Duplicate = MakeAssetDocumentHostTestDescriptor();
	Duplicate.Admission.AuthorityFingerprint = TEXT("different-fingerprint");
	TestRejected(Test, TEXT("duplicate identity does not freeze"),
		MakeBuildInput({MakeAssetDocumentHostTestDescriptor(), Duplicate}),
		TEXT("PrimitiveDescriptor.DuplicateIdentity"));

	FAssetDocumentPrimitiveDescriptor SameAuthority = MakeAssetDocumentHostTestDescriptor(
		TEXT("editor.host_test.second"), 1, TEXT("host-test-echo-v1"));
	const FAssetDocumentPrimitiveDescriptor Original = MakeAssetDocumentHostTestDescriptor();
	SameAuthority.InputSchemaId = Original.InputSchemaId;
	SameAuthority.OutputSchemaId = Original.OutputSchemaId;
	SameAuthority.Admission.PrimaryOperationId = Original.Admission.PrimaryOperationId;
	SameAuthority.Admission.CompletionContractId = Original.Admission.CompletionContractId;
	TestRejected(Test, TEXT("duplicate authority does not freeze"),
		MakeBuildInput({Original, SameAuthority}),
		TEXT("PrimitiveDescriptor.DuplicateAuthority"));
}

void TestClosureAndAvailability(FAutomationTestBase& Test)
{
	FAssetDocumentPrimitiveRegistryBuildInput Missing = MakeBuildInput({MakeAssetDocumentHostTestDescriptor()});
	Missing.Required[0].PrimitiveId = TEXT("editor.host_test.missing");
	TestRejected(Test, TEXT("missing requirement does not freeze"), MoveTemp(Missing),
		TEXT("PrimitiveRegistry.MissingRequiredPrimitive"));

	FAssetDocumentPrimitiveRegistryBuildInput Orphan = MakeBuildInput({
		MakeAssetDocumentHostTestDescriptor(),
		MakeAssetDocumentHostTestDescriptor(TEXT("editor.host_test.orphan"), 1, TEXT("host-test-orphan-v1"))});
	Orphan.Required.SetNum(1);
	TestRejected(Test, TEXT("orphan descriptor does not freeze"), MoveTemp(Orphan),
		TEXT("PrimitiveRegistry.OrphanDescriptor"));

	TestRejected(Test, TEXT("unavailable provider does not freeze"),
		MakeBuildInput({MakeAssetDocumentHostTestDescriptor()}, EAssetDocumentProviderAvailabilityState::Unavailable),
		TEXT("PrimitiveRegistry.ProviderUnavailable"));
}

void TestRuntimeDispositions(FAutomationTestBase& Test)
{
	FAssetDocumentPrimitiveDescriptor Candidate = MakeAssetDocumentHostTestDescriptor();
	Candidate.Admission.Disposition = EAssetDocumentPrimitiveDisposition::GovernanceCandidate;
	TestRejected(Test, TEXT("governance candidate does not freeze"), MakeBuildInput({Candidate}),
		TEXT("PrimitiveDescriptor.InvalidPath"));

	FAssetDocumentPrimitiveDescriptor Blocker = MakeAssetDocumentHostTestDescriptor();
	Blocker.Admission.Disposition = EAssetDocumentPrimitiveDisposition::StableBlocker;
	Blocker.Admission.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::StableBlocker;
	Blocker.Admission.ExportedAuthoritativeApi.Reset();
	Blocker.Admission.SourceMarker.Reset();
	Blocker.Admission.RawLifecycleAllowlistId.Reset();
	Blocker.RequiredConsumerIds.Reset();
	Blocker.AllowedHandleKinds.Reset();
	Blocker.HandleArgumentSchemas.Reset();
	Blocker.HandleResultSchemas.Reset();
	Blocker.InputSchemaId.Reset();
	Blocker.OutputSchemaId.Reset();
	TestRejected(Test, TEXT("stable blocker does not freeze"), MakeBuildInput({Blocker}),
		TEXT("PrimitiveRegistry.RuntimeDispositionLeakage"));
}

void TestValidCatalog(FAutomationTestBase& Test)
{
	const FAssetDocumentPrimitiveRegistryBuildResult Result =
		FAssetDocumentPrimitiveRegistryRuntime::CreateFrozen(
			MakeBuildInput({MakeAssetDocumentHostTestDescriptor()}));
	Test.TestTrue(TEXT("valid ordinary descriptor freezes"), Result.Registry.IsValid());
	Test.TestEqual(TEXT("valid freeze has no diagnostics"), Result.Diagnostics.Num(), 0);
	Test.TestTrue(TEXT("snapshot is frozen"), Result.Registry->Snapshot().bFrozen);
}

void TestAuthorityCatalogValidationPrecedesHostProjection(FAutomationTestBase& Test)
{
	FAssetDocumentPrimitiveRegistryBuildInput Input =
		MakeBuildInput({MakeAssetDocumentHostTestDescriptor()});
	Input.AuthorityCatalog.SetNum(4097);
	TestRejected(Test, TEXT("authority catalog reaches the frozen authority validator"),
		MoveTemp(Input), TEXT("AuthorityCatalog.InvalidIdentity"));
}
}

FAssetDocumentHostTestProvider::FAssetDocumentHostTestProvider(
	FString InProviderId,
	TArray<FAssetDocumentPrimitiveDescriptor> InDescriptors,
	EAssetDocumentProviderAvailabilityState InState)
	: ProviderId(MoveTemp(InProviderId))
	, Descriptors(MoveTemp(InDescriptors))
	, State(InState)
{
}

FString FAssetDocumentHostTestProvider::GetProviderId() const
{
	return ProviderId;
}

TConstArrayView<FAssetDocumentPrimitiveDescriptor> FAssetDocumentHostTestProvider::DescribePrimitives() const
{
	return Descriptors;
}

FAssetDocumentProviderAvailability FAssetDocumentHostTestProvider::InspectAvailability() const
{
	FAssetDocumentProviderAvailability Availability;
	Availability.ProviderId = ProviderId;
	Availability.State = State;
	return Availability;
}

FAssetDocumentPrimitiveOutcome FAssetDocumentHostTestProvider::Invoke(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	(void)Handles;
	(void)ActiveTransaction;
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = Invocation.ValueArguments;
	return Outcome;
}

FAssetDocumentPrimitiveDescriptor MakeAssetDocumentHostTestDescriptor(
	FString PrimitiveId,
	uint32 Version,
	FString AuthorityFingerprint)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = MoveTemp(PrimitiveId);
	Descriptor.Version = Version;
	Descriptor.ProviderId = TEXT("editor");
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::Editor;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = EAssetDocumentPrimitiveAccess::ReadOnly;
	Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Validate};
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v1");
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v1");
	Descriptor.StableDiagnosticCodes = {TEXT("AD.RUNTIME.HOST_TEST")};
	Descriptor.RequiredConsumerIds = {TEXT("host.test.route")};
	FAssetDocumentPrimitiveAdmissionEvidence& Evidence = Descriptor.Admission;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Evidence.PhysicalOwner = EAssetDocumentPrimitiveOwner::Editor;
	Evidence.ExactEngineVersion = TEXT("5.7");
	Evidence.PublicModule = TEXT("AssetDocumentPython");
	Evidence.PublicHeader = TEXT("AssetDocumentPythonHostTestDoubles.h");
	Evidence.ExportedAuthoritativeApi = TEXT("FAssetDocumentHostTestProvider::Invoke");
	Evidence.AuthorityContextId = TEXT("host-test-context-v1");
	Evidence.PrimaryOperationId = Descriptor.PrimitiveId;
	Evidence.IdentityLifetimeContractId = TEXT("host-test-lifetime-v1");
	Evidence.TransactionContractId = TEXT("host-test-transaction-v1");
	Evidence.RollbackContractId = TEXT("host-test-rollback-v1");
	Evidence.CompletionContractId = Descriptor.PrimitiveId + TEXT(".completion.v1");
	Evidence.SaveReloadEvidenceId = TEXT("host-test-save-reload-v1");
	Evidence.FailureInjectionTestIds = {TEXT("host-test-failure-v1")};
	Evidence.VerificationTestIds = {TEXT("host-test-verification-v1")};
	Evidence.AuthorityFingerprint = MoveTemp(AuthorityFingerprint);
	return Descriptor;
}

FAssetDocumentPrimitiveRequirement MakeAssetDocumentHostTestRequirement(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	FString RouteId)
{
	FAssetDocumentPrimitiveRequirement Requirement;
	Requirement.PrimitiveId = Descriptor.PrimitiveId;
	Requirement.Version = Descriptor.Version;
	Requirement.RequiredByRouteId = MoveTemp(RouteId);
	return Requirement;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentPrimitiveRegistryRejectsIncompleteDescriptorTest,
	"UEAssetDoc.PrimitiveRegistry.RejectsIncompleteDescriptor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentPrimitiveRegistryRejectsIncompleteDescriptorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestIncompleteAndDuplicates(*this);
	TestClosureAndAvailability(*this);
	TestRuntimeDispositions(*this);
	TestValidCatalog(*this);
	TestAuthorityCatalogValidationPrecedesHostProjection(*this);
	return true;
}
