#include "Runtime/AssetDocumentRuntimeTypes.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"
#include "Runtime/AssetDocumentPrimitiveRegistry.h"
#include "Runtime/AssetDocumentAuthorityExceptionRegistry.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentPersistenceCoordinator.h"
#include "Runtime/AssetDocumentSyncCoordinator.h"
#include "Runtime/AssetDocumentRequestCoordinator.h"
#include "AssetDocumentRollbackAction.h"

#include "Misc/AutomationTest.h"

#include <type_traits>

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentCoreContractsDirectIncludeLinkTests
{
class FPayload final : public IAssetDocumentNativeHandlePayload
{
public:
	FName GetPayloadTypeId() const override { return TEXT("F-SKELETON.Payload"); }
	bool IsAlive() const override { return true; }
};

class FLease final : public IAssetDocumentVerifiedHandleLease
{
public:
	const FAssetDocumentOpaqueHandle& GetHandle() const override { return Handle; }
	IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override { return Payload; }
private:
	FAssetDocumentOpaqueHandle Handle;
	mutable FPayload Payload;
};

class FResolver final : public IAssetDocumentHandleResolver
{
public:
	FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind,
		EAssetDocumentHandleAccess,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&&) override { return {}; }
	FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle&,
		const FAssetDocumentHandleValidationContext&) override { return {}; }
	FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation&) override
	{
		return {};
	}
};

class FProvider final : public IAssetDocumentPrimitiveProvider
{
public:
	FString GetProviderId() const override { return TEXT("f-skeleton"); }
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> DescribePrimitives() const override { return {}; }
	FAssetDocumentProviderAvailability InspectAvailability() const override { return {}; }
	FAssetDocumentPrimitiveOutcome Invoke(
		const FAssetDocumentPrimitiveInvocation&,
		IAssetDocumentHandleResolver&,
		IAssetDocumentAtomicTransaction*) override { return {}; }
};

class FRegistry final : public IAssetDocumentPrimitiveRegistry
{
public:
	FAssetDocumentPrimitiveRegistrySnapshot Snapshot() const override { return {}; }
	FAssetDocumentPrimitiveResolution Resolve(
		const FAssetDocumentPrimitiveRequirement&) const override { return {}; }
};

class FCancellation final : public IAssetDocumentCancellationView
{
public:
	bool IsCancellationRequested() const override { return false; }
};

class FLinkOnlyRollbackAction final : public IAssetDocumentRollbackAction
{
public:
	FStringView GetRollbackComponentId() const override
	{
		return TEXTVIEW("test.link-only");
	}

	FAssetDocumentContractValidationResult Rollback(FStringView) override
	{
		FAssetDocumentContractValidationResult Result;
		Result.bValid = false;
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/TestOnly/RollbackAction");
		Diagnostic.Code = TEXT("RollbackAction.TestOnlyNotExecutable");
		Diagnostic.Message =
			TEXT("The link-only rollback action is not executable.");
		Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Error;
		return Result;
	}
};

class FLinkTestTransaction final : public IAssetDocumentAtomicTransaction
{
public:
	FGuid GetTransactionId() const override { return {}; }
	EAssetDocumentAtomicState GetState() const override { return EAssetDocumentAtomicState::Live; }
	const FAssetDocumentMutationToken& GetMutationToken() const override { return Token; }
	FAssetDocumentContractValidationResult EnlistFootprint(
		TConstArrayView<FAssetDocumentMutationFootprint>) override { return {}; }
	FAssetDocumentContractValidationResult MarkReadyToCommit() override { return {}; }
	FAssetDocumentContractValidationResult Commit(
		const FAssetDocumentDurableCompletionReceipt&) override { return {}; }
	FAssetDocumentContractValidationResult Rollback(FStringView) override { return {}; }
	FAssetDocumentContractValidationResult EnlistRollbackAction(
		TSharedRef<IAssetDocumentRollbackAction>) override
	{
		FAssetDocumentContractValidationResult Result;
		Result.bValid = false;
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/TestOnly/AtomicTransaction");
		Diagnostic.Code = TEXT("Atomic.TestOnlyRollbackActionEnlistmentUnsupported");
		Diagnostic.Message =
			TEXT("The direct-include/link-only transaction does not enlist rollback actions.");
		Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Error;
		return Result;
	}
	FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer>) override
	{
		FAssetDocumentContractValidationResult Result;
		Result.bValid = false;
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/TestOnly/AtomicTransaction");
		Diagnostic.Code = TEXT("Atomic.TestOnlyPostRollbackFinalizerEnlistmentUnsupported");
		Diagnostic.Message =
			TEXT("The direct-include/link-only transaction does not enlist post-rollback finalizers.");
		Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Error;
		return Result;
	}
private:
	FAssetDocumentMutationToken Token;
};

class FTransactionFactory final : public IAssetDocumentAtomicTransactionFactory
{
public:
	TUniquePtr<IAssetDocumentAtomicTransaction> BeginLiveAfterPreflight(
		const FAssetDocumentPreflightReceipt&,
		IAssetDocumentCancellationView&) override { return {}; }
};

class FStagedVerifier final : public IAssetDocumentStagedArtifactVerifier
{
public:
	FAssetDocumentFreshVerificationResult VerifyFresh(
		const FAssetDocumentFreshVerificationInput&) override { return {}; }
};

class FPersistence final : public IAssetDocumentPersistenceCoordinator
{
public:
	FAssetDocumentDurableCompletionReceipt Persist(
		const FAssetDocumentPersistencePlan&,
		IAssetDocumentStagedArtifactVerifier&) override { return {}; }
};

class FSync final : public IAssetDocumentSyncCoordinator
{
public:
	FAssetDocumentSyncDecision Compare(const FAssetDocumentSyncComparisonInput&) const override { return {}; }
	FAssetDocumentContractValidationResult AdvanceAfterDurableCompletion(
		const FAssetDocumentSyncEvidencePair&,
		const FAssetDocumentDurableCompletionReceipt&) override { return {}; }
};

class FNativeContext final : public IAssetDocumentNativeRequestContext
{
public:
	const FAssetDocumentRequestContextMetadata& GetMetadata() const override { return Metadata; }
	FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation&) override { return {}; }
	FAssetDocumentPrimitiveRegistrySnapshot InspectCatalog() const override { return {}; }
private:
	FAssetDocumentRequestContextMetadata Metadata;
};

class FHost final : public IAssetDocumentPythonRuntimeHost
{
public:
	FAssetDocumentRuntimeResultDto Dispatch(
		const FAssetDocumentRuntimeRequestDto&,
		IAssetDocumentNativeRequestContext&) override { return {}; }
};

class FObserver final : public IAssetDocumentRequestObserver
{
public:
	void OnPhaseChanged(FGuid, EAssetDocumentRuntimeRequestPhase) override {}
};

class FCoordinator final : public IAssetDocumentRequestCoordinator
{
public:
	FAssetDocumentRuntimeResultDto Execute(
		const FAssetDocumentRuntimeRequestDto&,
		FAssetDocumentRequestCoordinatorDependencies&) override { return {}; }
};

template <typename BaseType, typename DerivedType>
void DestroyThroughExportedBase(FAutomationTestBase& Test, const TCHAR* Label)
{
	static_assert(std::is_base_of_v<BaseType, DerivedType>);
	static_assert(std::has_virtual_destructor_v<BaseType>);
	TUniquePtr<BaseType> Instance = MakeUnique<DerivedType>();
	Instance.Reset();
	Test.TestTrue(Label, true);
}

void ExerciseAllExportedInterfaceDestructors(FAutomationTestBase& Test)
{
	DestroyThroughExportedBase<IAssetDocumentNativeHandlePayload, FPayload>(Test, TEXT("payload destroyed through exported base"));
	DestroyThroughExportedBase<IAssetDocumentVerifiedHandleLease, FLease>(Test, TEXT("lease destroyed through exported base"));
	DestroyThroughExportedBase<IAssetDocumentHandleResolver, FResolver>(Test, TEXT("resolver destroyed through exported base"));
	DestroyThroughExportedBase<IAssetDocumentPrimitiveProvider, FProvider>(Test, TEXT("provider destroyed through exported base"));
	DestroyThroughExportedBase<IAssetDocumentPrimitiveRegistry, FRegistry>(Test, TEXT("registry destroyed through exported base"));
	DestroyThroughExportedBase<IAssetDocumentCancellationView, FCancellation>(Test, TEXT("cancellation destroyed through exported base"));
	DestroyThroughExportedBase<IAssetDocumentAtomicTransaction, FLinkTestTransaction>(Test, TEXT("transaction destroyed through exported base"));
	DestroyThroughExportedBase<IAssetDocumentAtomicTransactionFactory, FTransactionFactory>(Test, TEXT("factory destroyed through exported base"));
	DestroyThroughExportedBase<IAssetDocumentStagedArtifactVerifier, FStagedVerifier>(Test, TEXT("verifier destroyed through exported base"));
	DestroyThroughExportedBase<IAssetDocumentPersistenceCoordinator, FPersistence>(Test, TEXT("persistence destroyed through exported base"));
	DestroyThroughExportedBase<IAssetDocumentSyncCoordinator, FSync>(Test, TEXT("sync destroyed through exported base"));
	DestroyThroughExportedBase<IAssetDocumentNativeRequestContext, FNativeContext>(Test, TEXT("context destroyed through exported base"));
	DestroyThroughExportedBase<IAssetDocumentPythonRuntimeHost, FHost>(Test, TEXT("host destroyed through exported base"));
	DestroyThroughExportedBase<IAssetDocumentRequestObserver, FObserver>(Test, TEXT("observer destroyed through exported base"));
	DestroyThroughExportedBase<IAssetDocumentRequestCoordinator, FCoordinator>(Test, TEXT("coordinator destroyed through exported base"));
}

void ExerciseDefaultHandoffLink(FAutomationTestBase& Test)
{
	FNativeContext Context;
	const FAssetDocumentIssuedHandleResult Result = Context.Handoff(
		{},
		TEXTVIEW("animation"),
		EAssetDocumentHandleAccess::ReadOnly);
	Test.TestFalse(TEXT("default handoff issues no handle"), Result.Handle.IsSet());
	Test.TestEqual(TEXT("default handoff diagnostic links"),
		Result.Diagnostics[0].Code,
		FString(TEXT("AD.RUNTIME.HANDOFF_UNAVAILABLE")));
}

void ExerciseAllContractLinkAnchors(FAutomationTestBase& Test)
{
	const FAssetDocumentRuntimeValueDto Value;
	const FAssetDocumentRuntimeRequestDto Request;
	const FAssetDocumentRuntimeResultDto Result;
	Test.TestTrue(TEXT("runtime value validator links"), ValidateAssetDocumentRuntimeValue(Value).bValid);
	Test.TestFalse(TEXT("runtime request validator links"), ValidateAssetDocumentRuntimeRequest(Request).bValid);
	Test.TestFalse(TEXT("runtime result validator links"), ValidateAssetDocumentRuntimeResult(Request, Result).bValid);

	const FAssetDocumentPrimitiveDescriptor Descriptor;
	const FAssetDocumentPrimitiveInvocation Invocation;
	const FAssetDocumentPrimitiveOutcome Outcome;
	Test.TestFalse(TEXT("descriptor validator links"), ValidateAssetDocumentPrimitiveDescriptor(Descriptor).bValid);
	Test.TestFalse(TEXT("invocation validator links"), ValidateAssetDocumentPrimitiveInvocation(Invocation, Descriptor).bValid);
	Test.TestFalse(TEXT("outcome validator links"), ValidateAssetDocumentPrimitiveOutcome(Invocation, Descriptor, Outcome).bValid);
	Test.TestFalse(TEXT("empty registry freeze links"), ValidateAssetDocumentPrimitiveFreezeInput({}, {}, {}, {}).bValid);

	const TConstArrayView<FAssetDocumentAuthorityCandidateRecord> Catalog = GetG1FrozenAuthorityExceptionRecords();
	Test.TestTrue(TEXT("frozen authority catalog links"), ValidateG1FrozenAuthorityExceptionRecords().bValid);
	Test.TestTrue(TEXT("generic authority validator links"), ValidateAssetDocumentAuthorityExceptionCatalog(Catalog).bValid);
	const FAssetDocumentAuthorityCandidateRecord* Candidate = FindAssetDocumentAuthorityCandidateRecord(
		Catalog, TEXTVIEW("UMG-WIDGET-ANIMATION-CREATE-v1"));
	Test.TestNotNull(TEXT("candidate finder links"), Candidate);
	Test.TestNotNull(TEXT("review finder links"), Candidate == nullptr ? nullptr :
		FindAssetDocumentAuthorityReviewRecord(*Candidate, TEXTVIEW("candidate-review.initial/UMG-WIDGET-ANIMATION-CREATE-v1")));
	Test.TestTrue(TEXT("authority transition links"), IsAssetDocumentAuthorityTransitionAllowed(
		EAssetDocumentAuthorityState::ObservedGap, EAssetDocumentAuthorityState::Researching));

	Test.TestFalse(TEXT("handle validator links"), ValidateAssetDocumentHandleMetadata({}, {}).bValid);
	Test.TestTrue(TEXT("atomic transition links"), IsAssetDocumentAtomicTransitionAllowed(
		EAssetDocumentAtomicState::Live, EAssetDocumentAtomicState::Mutating));
	Test.TestFalse(TEXT("preflight validator links"), ValidateAssetDocumentPreflightReceipt({}).bValid);
	FLinkTestTransaction LinkOnlyTransaction;
	const FAssetDocumentContractValidationResult RollbackActionEnlistment =
		LinkOnlyTransaction.EnlistRollbackAction(
			MakeShared<FLinkOnlyRollbackAction>());
	Test.TestFalse(
		TEXT("link-only rollback action enlistment fails explicitly"),
		RollbackActionEnlistment.bValid);
	Test.TestEqual(
		TEXT("link-only rollback action enlistment emits one diagnostic"),
		RollbackActionEnlistment.Diagnostics.Num(),
		1);
	if (RollbackActionEnlistment.Diagnostics.Num() == 1)
	{
		const FAssetDocumentRuntimeDiagnostic& Diagnostic =
			RollbackActionEnlistment.Diagnostics[0];
		Test.TestEqual(
			TEXT("link-only rollback action enlistment diagnostic path"),
			Diagnostic.Path,
			FString(TEXT("/TestOnly/AtomicTransaction")));
		Test.TestEqual(
			TEXT("link-only rollback action enlistment diagnostic code"),
			Diagnostic.Code,
			FString(TEXT("Atomic.TestOnlyRollbackActionEnlistmentUnsupported")));
		Test.TestFalse(
			TEXT("link-only rollback action enlistment diagnostic message is non-empty"),
			Diagnostic.Message.IsEmpty());
	}
	Test.TestTrue(TEXT("persistence transition links"), IsAssetDocumentPersistenceTransitionAllowed(
		EAssetDocumentPersistenceStep::StagePackage, EAssetDocumentPersistenceStep::StageSidecar));
	Test.TestFalse(TEXT("persistence validator links"), ValidateAssetDocumentPersistencePlan({}).bValid);
	Test.TestFalse(TEXT("region evidence validator links"), ValidateAssetDocumentRegionEvidence({}).bValid);
	Test.TestEqual(TEXT("sync classifier links"), ClassifyAssetDocumentSyncDirection({}).Direction,
		EAssetDocumentRuntimeSyncDirection::EvidenceUnavailable);
	Test.TestTrue(TEXT("request transition links"), IsAssetDocumentRequestPhaseTransitionAllowed(
		EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight));
	Test.TestEqual(TEXT("contract version anchor links"), AssetDocumentRuntimeContractVersion, uint32{1});
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentCoreContractsDirectIncludeLinkTest,
	"UEAssetDoc.Architecture.ModuleGraph.CoreContractsDirectIncludeLink",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCoreContractsDirectIncludeLinkTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	AssetDocumentCoreContractsDirectIncludeLinkTests::ExerciseAllExportedInterfaceDestructors(*this);
	AssetDocumentCoreContractsDirectIncludeLinkTests::ExerciseDefaultHandoffLink(*this);
	AssetDocumentCoreContractsDirectIncludeLinkTests::ExerciseAllContractLinkAnchors(*this);
	return true;
}
#endif
