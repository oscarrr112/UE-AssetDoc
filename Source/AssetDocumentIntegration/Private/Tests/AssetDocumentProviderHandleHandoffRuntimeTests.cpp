// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentEditorNativeState.h"
#include "AssetDocumentPythonRuntime.h"
#include "AssetDocumentRollbackAction.h"

#include "Engine/PrimaryAssetLabel.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "AssetDocumentPluginPaths.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr uint64 HandoffRuntimeRevision = 41;
constexpr const TCHAR* AnimationConsumePrimitive =
	TEXT("animation.handle_handoff.consume_asset.v1");

FAssetDocumentContractValidationResult MakeValidContractResult()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

class FHandleHandoffConsumerTransaction final
	: public IAssetDocumentAtomicTransaction
{
public:
	FHandleHandoffConsumerTransaction()
	{
		Token.RequestId = FGuid::NewGuid();
		Token.TransactionId = FGuid::NewGuid();
		Token.RuntimeRevision = HandoffRuntimeRevision;
		RollbackActionEnlistmentResult.bValid = true;
		PostRollbackFinalizerEnlistmentResult.bValid = true;
	}

	FGuid GetTransactionId() const override { return Token.TransactionId; }
	EAssetDocumentAtomicState GetState() const override
	{
		return EAssetDocumentAtomicState::Live;
	}
	const FAssetDocumentMutationToken& GetMutationToken() const override
	{
		return Token;
	}
	FAssetDocumentContractValidationResult EnlistFootprint(
		TConstArrayView<FAssetDocumentMutationFootprint> Footprints) override
	{
		EnlistedFootprintCount += Footprints.Num();
		return MakeValidContractResult();
	}
	FAssetDocumentContractValidationResult MarkReadyToCommit() override
	{
		return MakeValidContractResult();
	}
	FAssetDocumentContractValidationResult Commit(
		const FAssetDocumentDurableCompletionReceipt&) override
	{
		return MakeValidContractResult();
	}
	FAssetDocumentContractValidationResult Rollback(FStringView) override
	{
		return MakeValidContractResult();
	}
	FAssetDocumentContractValidationResult EnlistRollbackAction(
		TSharedRef<IAssetDocumentRollbackAction> Action) override
	{
		EnlistedRollbackActionComponentIds.Add(
			FString(Action->GetRollbackComponentId()));
		return RollbackActionEnlistmentResult;
	}
	FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer>) override
	{
		return PostRollbackFinalizerEnlistmentResult;
	}

	int32 EnlistedFootprintCount = 0;
	TArray<FString> EnlistedRollbackActionComponentIds;
	FAssetDocumentContractValidationResult RollbackActionEnlistmentResult;
	FAssetDocumentContractValidationResult PostRollbackFinalizerEnlistmentResult;

private:
	FAssetDocumentMutationToken Token;
};

class FHandleHandoffConsumerRollbackAction final
	: public IAssetDocumentRollbackAction
{
public:
	virtual FStringView GetRollbackComponentId() const override
	{
		return TEXTVIEW("rollback.test-provider-handoff");
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView) override
	{
		return MakeValidContractResult();
	}
};

void TestTransactionRollbackActionEnlistment(FAutomationTestBase& Test)
{
	FHandleHandoffConsumerTransaction Transaction;
	Transaction.RollbackActionEnlistmentResult.bValid = false;
	FAssetDocumentRuntimeDiagnostic& ExpectedDiagnostic =
		Transaction.RollbackActionEnlistmentResult.Diagnostics.AddDefaulted_GetRef();
	ExpectedDiagnostic.Code =
		TEXT("Test.ProviderHandoff.RollbackActionRejected");

	const FAssetDocumentContractValidationResult Actual =
		Transaction.EnlistRollbackAction(
			MakeShared<FHandleHandoffConsumerRollbackAction>());
	Test.TestFalse(
		TEXT("provider-handoff fake returns configured action-enlistment result"),
		Actual.bValid);
	Test.TestEqual(
		TEXT("provider-handoff fake preserves configured diagnostic"),
		Actual.Diagnostics.Num() == 1 ? Actual.Diagnostics[0].Code : FString(),
		FString(TEXT("Test.ProviderHandoff.RollbackActionRejected")));
	Test.TestEqual(
		TEXT("provider-handoff fake records rollback component ID"),
		Transaction.EnlistedRollbackActionComponentIds,
		TArray<FString>({TEXT("rollback.test-provider-handoff")}));
}

FAssetDocumentPrimitiveDescriptor MakeAnimationConsumeDescriptor(
	EAssetDocumentHandleAccess RequiredAccess)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = AnimationConsumePrimitive;
	Descriptor.Version = 1;
	Descriptor.ProviderId = TEXT("animation");
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::Animation;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = EAssetDocumentPrimitiveAccess::ReadOnly;
	Descriptor.AllowedPhases = {
		EAssetDocumentRuntimeRequestPhase::Apply,
		EAssetDocumentRuntimeRequestPhase::Repair};
	if (RequiredAccess == EAssetDocumentHandleAccess::ReadOnly)
	{
		Descriptor.AllowedPhases.Add(EAssetDocumentRuntimeRequestPhase::Preflight);
		Descriptor.AllowedPhases.Add(EAssetDocumentRuntimeRequestPhase::Verify);
	}
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
	Descriptor.HandleArgumentSchemas.Add({
		TEXT("asset"),
		EAssetDocumentHandleKind::Asset,
		RequiredAccess,
		true});
	Descriptor.InputSchemaId =
		TEXT("animation.handle_handoff.consume_asset.input.v1");
	Descriptor.OutputSchemaId =
		TEXT("animation.handle_handoff.consume_asset.output.v1");
	Descriptor.StableDiagnosticCodes = {
		TEXT("AD.RUNTIME.HANDOFF_CONSUMER_TEST")};
	Descriptor.RequiredConsumerIds = {
		TEXT("route.handle-handoff-test.v1")};
	FAssetDocumentPrimitiveAdmissionEvidence& Evidence = Descriptor.Admission;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath =
		EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Evidence.ExactEngineVersion = TEXT("5.7.3");
	Evidence.PhysicalOwner = EAssetDocumentPrimitiveOwner::Animation;
	Evidence.PublicModule = TEXT("AssetDocumentIntegration");
	Evidence.PublicHeader =
		TEXT("AssetDocumentProviderHandleHandoffRuntimeTests.cpp");
	Evidence.ExportedAuthoritativeApi =
		TEXT("FAnimationHandoffConsumerProvider::Invoke");
	Evidence.AuthorityContextId =
		TEXT("test.handle-handoff-consumer.apply.v1");
	Evidence.PrimaryOperationId = AnimationConsumePrimitive;
	Evidence.IdentityLifetimeContractId =
		TEXT("identity.request-transaction-revision-bound.v1");
	Evidence.TransactionContractId =
		TEXT("transaction.test-active-owner.v1");
	Evidence.RollbackContractId =
		TEXT("rollback.test-no-mutation.v1");
	Evidence.CompletionContractId =
		TEXT("animation.handle-handoff.consume_asset.completion.v1");
	Evidence.SaveReloadEvidenceId =
		TEXT("animation.handle-handoff.consume_asset.no-save.v1");
	Evidence.FailureInjectionTestIds = {
		TEXT("UEAssetDoc.HandleHandoff.ForeignDirectProviderMismatch")};
	Evidence.VerificationTestIds = {
		TEXT("UEAssetDoc.HandleHandoff.EditorMutableToAnimationMutable"),
		TEXT("UEAssetDoc.HandleHandoff.EditorMutableToAnimationTypedObjectLease")};
	Evidence.AuthorityFingerprint =
		TEXT("ue-5.7.3|test-animation|handle-handoff-consumer-v1");
	return Descriptor;
}

class FAnimationHandoffConsumerProvider final
	: public IAssetDocumentPrimitiveProvider
{
public:
	explicit FAnimationHandoffConsumerProvider(
		EAssetDocumentHandleAccess RequiredAccess = EAssetDocumentHandleAccess::Mutable)
		: Descriptor(MakeAnimationConsumeDescriptor(RequiredAccess))
	{
	}

	FString GetProviderId() const override { return TEXT("animation"); }
	TConstArrayView<FAssetDocumentPrimitiveDescriptor>
	DescribePrimitives() const override
	{
		return MakeArrayView(&Descriptor, 1);
	}
	FAssetDocumentProviderAvailability InspectAvailability() const override
	{
		FAssetDocumentProviderAvailability Result;
		Result.ProviderId = GetProviderId();
		Result.State = EAssetDocumentProviderAvailabilityState::Available;
		return Result;
	}
	FAssetDocumentPrimitiveOutcome Invoke(
		const FAssetDocumentPrimitiveInvocation& Invocation,
		IAssetDocumentHandleResolver& Handles,
		IAssetDocumentAtomicTransaction* ActiveTransaction) override
	{
		(void)ActiveTransaction;
		++InvokeCount;
		FAssetDocumentPrimitiveOutcome Outcome;
		if (Invocation.HandleArguments.Num() != 1
			|| Invocation.HandleArguments[0].Name != TEXT("asset"))
		{
			Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
			return Outcome;
		}
		ObservedHandle = Invocation.HandleArguments[0].Handle;

		FAssetDocumentHandleValidationContext Validation;
		Validation.RequestId = Invocation.RequestId;
		Validation.TransactionId = Invocation.TransactionId;
		Validation.RuntimeRevision = Invocation.RuntimeRevision;
		Validation.ProviderId = GetProviderId();
		Validation.Phase = Invocation.Phase;
		Validation.RequiredKind = EAssetDocumentHandleKind::Asset;
		Validation.RequiredAccess = Descriptor.HandleArgumentSchemas[0].RequiredAccess;
		FAssetDocumentVerifiedLeaseResult Lease =
			Handles.AcquireVerifiedLease(
				Invocation.HandleArguments[0].Handle,
				Validation);
		if (Lease.Lease == nullptr)
		{
			Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
			Outcome.Diagnostics = MoveTemp(Lease.Diagnostics);
			return Outcome;
		}

		const FAssetDocumentContractValidationResult Identity =
			WithAssetDocumentEditorAssetIdentity(
				*Lease.Lease,
				[this](const FAssetDocumentEditorAssetIdentityView& View)
				{
					ObservedCanonicalPath =
						FString(View.GetCanonicalObjectPath());
					ObservedTypeRevision = View.GetTypeRevision();
					bObservedExactNativeIdentity =
						!ExpectedCanonicalPath.IsEmpty()
						&& ObservedCanonicalPath == ExpectedCanonicalPath
						&& View.GetObject().GetPathName()
							== ExpectedCanonicalPath
						&& ObservedTypeRevision
							== static_cast<uint64>(
								View.GetObject().GetUniqueID())
						&& View.GetObject().IsA<UPrimaryAssetLabel>();
					return MakeValidContractResult();
				});
		if (!Identity.bValid)
		{
			Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
			Outcome.Diagnostics = Identity.Diagnostics;
			return Outcome;
		}
		Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
		Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
		return Outcome;
	}

	FAssetDocumentPrimitiveDescriptor Descriptor;
	TOptional<FAssetDocumentOpaqueHandle> ObservedHandle;
	FString ExpectedCanonicalPath;
	FString ObservedCanonicalPath;
	uint64 ObservedTypeRevision = 0;
	bool bObservedExactNativeIdentity = false;
	int32 InvokeCount = 0;
};

void AppendRequirements(
	const IAssetDocumentPrimitiveProvider& Provider,
	TArray<FAssetDocumentPrimitiveRequirement>& OutRequirements)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor :
		Provider.DescribePrimitives())
	{
		for (const FString& ConsumerId : Descriptor.RequiredConsumerIds)
		{
			OutRequirements.Add({
				Descriptor.PrimitiveId,
				Descriptor.Version,
				ConsumerId});
		}
	}
}

FAssetDocumentRuntimeValueDto MakeStringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeNullValue()
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Null;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeObjectValue(
	TArray<FString> Keys,
	TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Values);
	return Result;
}

const FAssetDocumentOpaqueHandle* FindSingleHandle(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	FStringView Name)
{
	for (const FAssetDocumentPrimitiveHandleResult& Result :
		Outcome.HandleResults)
	{
		if (Result.Name == Name && Result.Handles.Num() == 1)
		{
			return &Result.Handles[0];
		}
	}
	return nullptr;
}

bool HasDiagnosticCode(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	FStringView Code)
{
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic :
		Outcome.Diagnostics)
	{
		if (Diagnostic.Code == Code)
		{
			return true;
		}
	}
	return false;
}

class FProviderHandoffConsumerHarness final
{
public:
	explicit FProviderHandoffConsumerHarness(FAutomationTestBase& InTest)
		: Test(InTest)
	{
	}

	~FProviderHandoffConsumerHarness()
	{
		Shutdown();
	}

	bool Initialize(
		EAssetDocumentHandleAccess ConsumerAccess = EAssetDocumentHandleAccess::Mutable)
	{
		FAssetDocumentPythonRuntimeConfiguration Configuration;
		Configuration.RepositoryRoot = GetAssetDocumentPluginBaseDir();
		Configuration.PackageRootRelativePath =
			TEXT("Source/AssetDocumentPython/Private/Tests/Fixtures");
		Runtime = CreateAssetDocumentPythonRuntime(Configuration);
		if (!Test.TestTrue(TEXT("native runtime binding initializes"),
			Runtime->InitializePythonBinding().bValid))
		{
			return false;
		}
		bBindingInitialized = true;

		EditorProvider = CreateAssetDocumentEditorPrimitiveProvider();
		AnimationProvider = MakeShared<FAnimationHandoffConsumerProvider>(ConsumerAccess);
		FAssetDocumentPythonRevisionCandidate Candidate;
		Candidate.RuntimeRevision = HandoffRuntimeRevision;
		Candidate.EntryModuleRelativePath = TEXT("revision_one.py");
		Candidate.Providers = {EditorProvider, AnimationProvider.ToSharedRef()};
		AppendRequirements(*EditorProvider, Candidate.RequiredPrimitives);
		AppendRequirements(
			*AnimationProvider, Candidate.RequiredPrimitives);
		const FAssetDocumentPythonRevisionActivationResult Activation =
			Runtime->TryActivateRevision(Candidate);
		if (!Test.TestTrue(TEXT("real Editor plus fake Animation registry freezes"),
			Activation.bActivated))
		{
			return false;
		}

		Metadata.RequestId = Transaction.GetMutationToken().RequestId;
		Metadata.PinnedRuntimeRevision = HandoffRuntimeRevision;
		Metadata.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
		FAssetDocumentPythonRequestContextCreateResult Created =
			Runtime->CreateRequestContext(
				Metadata, Transaction.GetTransactionId(), &Transaction);
		Context = MoveTemp(Created.Context);
		return Test.TestNotNull(
			TEXT("Apply request context is created"), Context.Get());
	}

	bool CreateAndResolveFixture()
	{
		const FAssetDocumentPrimitiveOutcome Class = Invoke(
			TEXT("editor.class.resolve_canonical.v1"),
			MakeObjectValue(
				{TEXT("class"), TEXT("expected_base")},
				{MakeStringValue(
					UPrimaryAssetLabel::StaticClass()->GetPathName()),
					MakeNullValue()}),
			{});
		const FAssetDocumentOpaqueHandle* ClassHandle =
			FindSingleHandle(Class, TEXT("class"));
		if (!Test.TestEqual(TEXT("class resolve succeeds"), Class.Status,
				EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
			|| !Test.TestNotNull(TEXT("class resolve returns a handle"),
				ClassHandle))
		{
			return false;
		}

		const FString AssetName = FString::Printf(
			TEXT("DA_HandleHandoff_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		const FString PackagePath =
			TEXT("/Game/AssetDocumentTests/CoreHandleHandoff");
		CanonicalPath = FString::Printf(
			TEXT("%s/%s.%s"), *PackagePath, *AssetName, *AssetName);
		const FAssetDocumentPrimitiveOutcome Created = Invoke(
			TEXT("editor.asset.create_by_factory.v1"),
			MakeObjectValue(
				{TEXT("factory_id"), TEXT("package_path"), TEXT("asset_name")},
				{MakeStringValue(TEXT("data_asset_factory.v1")),
					MakeStringValue(PackagePath),
					MakeStringValue(AssetName)}),
			{{TEXT("asset_class"), *ClassHandle}});
		const FAssetDocumentOpaqueHandle* CreatedAsset =
			FindSingleHandle(Created, TEXT("asset_mutable"));
		const FAssetDocumentOpaqueHandle* CreatedPackage =
			FindSingleHandle(Created, TEXT("package"));
		if (!Test.TestEqual(TEXT("factory fixture creation succeeds"),
				Created.Status,
				EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
			|| !Test.TestNotNull(TEXT("factory returns mutable asset"),
				CreatedAsset)
			|| !Test.TestNotNull(TEXT("factory returns mutable package"),
				CreatedPackage))
		{
			return false;
		}
		CreatedAssetHandle = *CreatedAsset;
		CreatedPackageHandle = *CreatedPackage;
		bHasCreatedFixture = true;

		const FAssetDocumentPrimitiveOutcome Resolved = Invoke(
			TEXT("editor.asset.resolve_existing.v1"),
			MakeObjectValue(
				{TEXT("path"), TEXT("expected_class")},
				{MakeStringValue(CanonicalPath),
					MakeStringValue(
						UPrimaryAssetLabel::StaticClass()->GetPathName())}),
			{});
		const FAssetDocumentOpaqueHandle* ReadOnly =
			FindSingleHandle(Resolved, TEXT("asset_read"));
		const FAssetDocumentOpaqueHandle* Mutable =
			FindSingleHandle(Resolved, TEXT("asset_mutable"));
		if (!Test.TestEqual(TEXT("real resolve_existing succeeds"),
				Resolved.Status,
				EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
			|| !Test.TestNotNull(TEXT("resolve returns read-only Asset"),
				ReadOnly)
			|| !Test.TestNotNull(TEXT("resolve returns mutable Asset"),
				Mutable))
		{
			return false;
		}
		ResolvedReadOnly = *ReadOnly;
		ResolvedMutable = *Mutable;
		AnimationProvider->ExpectedCanonicalPath = CanonicalPath;
		return true;
	}

	FAssetDocumentPrimitiveOutcome InvokeAnimation(
		const FAssetDocumentOpaqueHandle& Handle)
	{
		return Invoke(
			AnimationConsumePrimitive,
			MakeObjectValue({}, {}),
			{{TEXT("asset"), Handle}});
	}

	bool CleanupFixture()
	{
		if (!bHasCreatedFixture || Runtime == nullptr)
		{
			return true;
		}
		FAssetDocumentRequestContextMetadata CleanupMetadata = Metadata;
		CleanupMetadata.Phase = EAssetDocumentRuntimeRequestPhase::Failed;
		FAssetDocumentPythonRequestContextCreateResult Created =
			Runtime->CreateRequestContext(
				CleanupMetadata,
				Transaction.GetTransactionId(),
				&Transaction);
		if (Created.Context == nullptr)
		{
			return false;
		}
		FAssetDocumentPrimitiveInvocation Invocation;
		Invocation.PrimitiveId = TEXT("editor.asset.rollback_created.v1");
		Invocation.Version = 1;
		Invocation.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
		Invocation.HandleArguments = {
			{TEXT("asset"), CreatedAssetHandle},
			{TEXT("package"), CreatedPackageHandle}};
		const FAssetDocumentPrimitiveOutcome Cleanup =
			Created.Context->Invoke(Invocation);
		bHasCreatedFixture = false;
		if (Cleanup.Status
			!= EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		{
			for (const FAssetDocumentRuntimeDiagnostic& Diagnostic :
				Cleanup.Diagnostics)
			{
				Test.AddInfo(FString::Printf(
					TEXT("cleanup diagnostic [%s] %s: %s"),
					*Diagnostic.Code,
					*Diagnostic.Path,
					*Diagnostic.Message));
			}
		}
		return Cleanup.Status
			== EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	}

	void Shutdown()
	{
		if (bShutdown)
		{
			return;
		}
		CleanupFixture();
		Context.Reset();
		if (Runtime != nullptr && bBindingInitialized)
		{
			Runtime->ShutdownPythonBinding();
		}
		Runtime.Reset();
		bShutdown = true;
	}

	FAssetDocumentIssuedHandleResult Handoff(
		const FAssetDocumentOpaqueHandle& Source,
		EAssetDocumentHandleAccess Access)
	{
		return Context->Handoff(Source, TEXTVIEW("animation"), Access);
	}

	bool SetRequestPhase(
		EAssetDocumentRuntimeRequestPhase Phase,
		bool bWithActiveTransaction)
	{
		Metadata.Phase = Phase;
		FAssetDocumentPythonRequestContextCreateResult Created =
			Runtime->CreateRequestContext(
				Metadata,
				Transaction.GetTransactionId(),
				bWithActiveTransaction ? &Transaction : nullptr);
		Context = MoveTemp(Created.Context);
		return Test.TestNotNull(
			TEXT("requested phase context is created"), Context.Get());
	}

	FAutomationTestBase& Test;
	FHandleHandoffConsumerTransaction Transaction;
	FAssetDocumentRequestContextMetadata Metadata;
	TUniquePtr<IAssetDocumentPythonRuntime> Runtime;
	TSharedRef<IAssetDocumentPrimitiveProvider> EditorProvider =
		CreateAssetDocumentEditorPrimitiveProvider();
	TSharedPtr<FAnimationHandoffConsumerProvider> AnimationProvider;
	TUniquePtr<IAssetDocumentNativeRequestContext> Context;
	FAssetDocumentOpaqueHandle CreatedAssetHandle;
	FAssetDocumentOpaqueHandle CreatedPackageHandle;
	FAssetDocumentOpaqueHandle ResolvedReadOnly;
	FAssetDocumentOpaqueHandle ResolvedMutable;
	FString CanonicalPath;
	bool bBindingInitialized = false;
	bool bHasCreatedFixture = false;
	bool bShutdown = false;

private:
	FAssetDocumentPrimitiveOutcome Invoke(
		FString PrimitiveId,
		FAssetDocumentRuntimeValueDto Value,
		TArray<FAssetDocumentPrimitiveHandleArgument> Handles)
	{
		FAssetDocumentPrimitiveInvocation Invocation;
		Invocation.PrimitiveId = MoveTemp(PrimitiveId);
		Invocation.Version = 1;
		Invocation.ValueArguments = MoveTemp(Value);
		Invocation.HandleArguments = MoveTemp(Handles);
		return Context->Invoke(Invocation);
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentEditorMutableToAnimationMutableHandoffTest,
	"UEAssetDoc.HandleHandoff.EditorMutableToAnimationMutable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorMutableToAnimationMutableHandoffTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	TestTransactionRollbackActionEnlistment(*this);
	FProviderHandoffConsumerHarness Harness(*this);
	if (!Harness.Initialize() || !Harness.CreateAndResolveFixture())
	{
		return false;
	}
	const FAssetDocumentIssuedHandleResult Handoff = Harness.Handoff(
		Harness.ResolvedMutable,
		EAssetDocumentHandleAccess::Mutable);
	if (!TestTrue(TEXT("mutable Editor Asset hands off to Animation"),
		Handoff.Handle.IsSet()))
	{
		return false;
	}
	const FAssetDocumentOpaqueHandle Target = Handoff.Handle.GetValue();
	TestEqual(TEXT("target provider identity is Animation"),
		Target.ProviderId, FString(TEXT("animation")));
	TestNotEqual(TEXT("target slot is fresh"),
		Target.Slot, Harness.ResolvedMutable.Slot);
	TestEqual(TEXT("request identity is preserved"),
		Target.RequestId, Harness.ResolvedMutable.RequestId);
	TestEqual(TEXT("transaction identity is preserved"),
		Target.TransactionId, Harness.ResolvedMutable.TransactionId);
	TestEqual(TEXT("runtime revision is preserved"),
		Target.RuntimeRevision, Harness.ResolvedMutable.RuntimeRevision);

	const FAssetDocumentPrimitiveOutcome Consumed =
		Harness.InvokeAnimation(Target);
	TestEqual(TEXT("fake Animation descriptor accepts handed-off Asset"),
		Consumed.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestEqual(TEXT("fake Animation handler runs exactly once"),
		Harness.AnimationProvider->InvokeCount, 1);
	TestTrue(TEXT("fake Animation handler observes an opaque handle only"),
		Harness.AnimationProvider->ObservedHandle.IsSet());
	if (Harness.AnimationProvider->ObservedHandle.IsSet())
	{
		TestEqual(TEXT("handler receives the fresh Animation identity"),
			Harness.AnimationProvider->ObservedHandle.GetValue().ProviderId,
			FString(TEXT("animation")));
	}
	TestFalse(TEXT("Asset-only prerequisite does not claim Graph handles"),
		Target.Kind == EAssetDocumentHandleKind::Graph);
	TestFalse(TEXT("Asset-only prerequisite does not claim Node handles"),
		Target.Kind == EAssetDocumentHandleKind::Node);

	struct FAllowedReadOnlyPhase
	{
		EAssetDocumentRuntimeRequestPhase Phase;
		const TCHAR* Name;
	};
	static constexpr FAllowedReadOnlyPhase AllowedReadOnlyPhases[] = {
		{EAssetDocumentRuntimeRequestPhase::Preflight, TEXT("Preflight")},
		{EAssetDocumentRuntimeRequestPhase::Apply, TEXT("Apply")},
		{EAssetDocumentRuntimeRequestPhase::Repair, TEXT("Repair")},
		{EAssetDocumentRuntimeRequestPhase::Verify, TEXT("Verify")},
	};
	for (const FAllowedReadOnlyPhase& Allowed : AllowedReadOnlyPhases)
	{
		if (!Harness.SetRequestPhase(Allowed.Phase, false))
		{
			return false;
		}
		const FAssetDocumentIssuedHandleResult ReadOnlyHandoff =
			Harness.Handoff(
				Harness.ResolvedReadOnly,
				EAssetDocumentHandleAccess::ReadOnly);
		if (!TestTrue(
			FString::Printf(
				TEXT("real Editor read-only Asset hands off in %s"),
				Allowed.Name),
			ReadOnlyHandoff.Handle.IsSet()))
		{
			return false;
		}
		TestTrue(
			FString::Printf(
				TEXT("%s target remains read-only"), Allowed.Name),
			ReadOnlyHandoff.Handle.GetValue().Access
				== EAssetDocumentHandleAccess::ReadOnly);
	}
	if (!Harness.SetRequestPhase(
		EAssetDocumentRuntimeRequestPhase::Apply, false))
	{
		return false;
	}
	const FAssetDocumentIssuedHandleResult Downgraded = Harness.Handoff(
		Harness.ResolvedMutable,
		EAssetDocumentHandleAccess::ReadOnly);
	if (!TestTrue(TEXT("real mutable Editor Asset may hand off read-only"),
		Downgraded.Handle.IsSet()))
	{
		return false;
	}
	TestTrue(TEXT("downgraded real target is read-only"),
		Downgraded.Handle.GetValue().Access
			== EAssetDocumentHandleAccess::ReadOnly);
	TestTrue(TEXT("authoritative fixture cleanup succeeds"),
		Harness.CleanupFixture());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentForeignDirectProviderMismatchHandoffTest,
	"UEAssetDoc.HandleHandoff.ForeignDirectProviderMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentForeignDirectProviderMismatchHandoffTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FProviderHandoffConsumerHarness Harness(*this);
	if (!Harness.Initialize() || !Harness.CreateAndResolveFixture())
	{
		return false;
	}
	const FAssetDocumentPrimitiveOutcome Direct =
		Harness.InvokeAnimation(Harness.ResolvedMutable);
	TestEqual(TEXT("direct foreign Editor handle fails before dispatch"),
		Direct.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("direct foreign handle preserves ProviderMismatch"),
		HasDiagnosticCode(Direct, TEXTVIEW("HandleMetadata.ProviderMismatch")));
	TestEqual(TEXT("fake Animation handler is not called"),
		Harness.AnimationProvider->InvokeCount, 0);
	TestTrue(TEXT("authoritative fixture cleanup succeeds"),
		Harness.CleanupFixture());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentEditorMutableToAnimationTypedObjectLeaseHandoffTest,
	"UEAssetDoc.HandleHandoff.EditorMutableToAnimationTypedObjectLease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorMutableToAnimationTypedObjectLeaseHandoffTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FProviderHandoffConsumerHarness Harness(*this);
	if (!Harness.Initialize() || !Harness.CreateAndResolveFixture())
	{
		return false;
	}
	const FAssetDocumentIssuedHandleResult Handoff = Harness.Handoff(
		Harness.ResolvedMutable,
		EAssetDocumentHandleAccess::Mutable);
	if (!TestTrue(TEXT("mutable Editor Asset hands off to Animation"),
		Handoff.Handle.IsSet()))
	{
		return false;
	}
	const FAssetDocumentOpaqueHandle Target = Handoff.Handle.GetValue();
	const FAssetDocumentPrimitiveOutcome Consumed =
		Harness.InvokeAnimation(Target);
	TestEqual(TEXT("fake Animation provider acquires and consumes typed lease"),
		Consumed.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestEqual(TEXT("typed Animation handler runs exactly once"),
		Harness.AnimationProvider->InvokeCount, 1);
	TestTrue(TEXT("typed callback observes the exact live native identity"),
		Harness.AnimationProvider->bObservedExactNativeIdentity);
	TestEqual(TEXT("typed callback preserves canonical object identity"),
		Harness.AnimationProvider->ObservedCanonicalPath,
		Harness.CanonicalPath);
	TestTrue(TEXT("typed callback preserves non-zero type revision"),
		Harness.AnimationProvider->ObservedTypeRevision > 0);
	TestTrue(TEXT("authoritative fixture cleanup succeeds"),
		Harness.CleanupFixture());
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentEditorToAnimationHandoffPhaseEffectsTest,
	"UEAssetDoc.HandleHandoff.PhaseAccessAndNoMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorToAnimationHandoffPhaseEffectsTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	for (const EAssetDocumentHandleAccess Access : {
		EAssetDocumentHandleAccess::ReadOnly, EAssetDocumentHandleAccess::Mutable})
	{
		FProviderHandoffConsumerHarness Harness(*this);
		if (!Harness.Initialize(Access) || !Harness.CreateAndResolveFixture())
		{
			return false;
		}
		UPrimaryAssetLabel* Asset = FindObject<UPrimaryAssetLabel>(
			nullptr, *Harness.CanonicalPath);
		if (!TestNotNull(TEXT("factory fixture remains loaded"), Asset))
		{
			return false;
		}
		const bool bDirtyBefore = Asset->GetOutermost()->IsDirty();
		const EObjectFlags FlagsBefore = Asset->GetFlags();
		const int32 FootprintsBefore = Harness.Transaction.EnlistedFootprintCount;
		const int32 RollbackActionsBefore =
			Harness.Transaction.EnlistedRollbackActionComponentIds.Num();
		const TArray<EAssetDocumentRuntimeRequestPhase> Phases =
			Access == EAssetDocumentHandleAccess::ReadOnly
			? TArray<EAssetDocumentRuntimeRequestPhase>{
				EAssetDocumentRuntimeRequestPhase::Preflight,
				EAssetDocumentRuntimeRequestPhase::Apply,
				EAssetDocumentRuntimeRequestPhase::Repair,
				EAssetDocumentRuntimeRequestPhase::Verify}
			: TArray<EAssetDocumentRuntimeRequestPhase>{
				EAssetDocumentRuntimeRequestPhase::Apply,
				EAssetDocumentRuntimeRequestPhase::Repair};
		for (const EAssetDocumentRuntimeRequestPhase Phase : Phases)
		{
			if (!Harness.SetRequestPhase(Phase,
				Access == EAssetDocumentHandleAccess::Mutable))
			{
				return false;
			}
			// Read-only consumers exercise both preserved access and downgrade.
			TArray<FAssetDocumentOpaqueHandle> Sources = {Harness.ResolvedMutable};
			if (Access == EAssetDocumentHandleAccess::ReadOnly)
			{
				Sources.Add(Harness.ResolvedReadOnly);
			}
			for (const FAssetDocumentOpaqueHandle& Source : Sources)
			{
				const FAssetDocumentIssuedHandleResult Handoff =
					Harness.Handoff(Source, Access);
				if (!TestTrue(TEXT("phase/access handoff succeeds"), Handoff.Handle.IsSet()))
				{
					return false;
				}
				const FAssetDocumentOpaqueHandle& Target = Handoff.Handle.GetValue();
				TestEqual(TEXT("target access is exact"), Target.Access, Access);
				TestNotEqual(TEXT("target receives fresh slot"), Target.Slot, Source.Slot);
				TestTrue(TEXT("target generation is valid"), Target.Generation != 0);
				TestEqual(TEXT("same request"), Target.RequestId, Source.RequestId);
				TestEqual(TEXT("same transaction"), Target.TransactionId, Source.TransactionId);
				TestEqual(TEXT("same revision"), Target.RuntimeRevision, Source.RuntimeRevision);
				const FAssetDocumentPrimitiveOutcome Consumed = Harness.InvokeAnimation(Target);
				TestEqual(TEXT("exact Animation consumer acquires typed lease"),
					Consumed.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
				TestTrue(TEXT("consumer sees same native asset"),
					Harness.AnimationProvider->bObservedExactNativeIdentity);
				TestEqual(TEXT("consumer returns no ChangedSet"), Consumed.ChangedSet.Num(), 0);
			}
		}
		if (!Harness.SetRequestPhase(EAssetDocumentRuntimeRequestPhase::Verify, true))
		{
			return false;
		}
		const int32 InvocationsBefore = Harness.AnimationProvider->InvokeCount;
		const FAssetDocumentIssuedHandleResult Rejected = Harness.Handoff(
			Harness.ResolvedMutable, EAssetDocumentHandleAccess::Mutable);
		TestFalse(TEXT("Verify mutable is rejected with live matching transaction"),
			Rejected.Handle.IsSet());
		TestEqual(TEXT("Verify mutable keeps transaction-required terminal"),
			Rejected.Diagnostics.Num() == 1 ? Rejected.Diagnostics[0].Code : FString(),
			FString(TEXT("AD.RUNTIME.HANDOFF_TRANSACTION_REQUIRED")));
		TestEqual(TEXT("rejection never reaches consumer"),
			Harness.AnimationProvider->InvokeCount, InvocationsBefore);
		TestEqual(TEXT("handoff leaves package dirty state unchanged"),
			Asset->GetOutermost()->IsDirty(), bDirtyBefore);
		TestEqual(TEXT("handoff leaves object flags unchanged"), Asset->GetFlags(), FlagsBefore);
		TestEqual(TEXT("handoff enlists no footprint"),
			Harness.Transaction.EnlistedFootprintCount, FootprintsBefore);
		TestEqual(TEXT("handoff enlists no rollback action"),
			Harness.Transaction.EnlistedRollbackActionComponentIds.Num(), RollbackActionsBefore);
		TestTrue(TEXT("authoritative fixture cleanup succeeds"), Harness.CleanupFixture());
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
