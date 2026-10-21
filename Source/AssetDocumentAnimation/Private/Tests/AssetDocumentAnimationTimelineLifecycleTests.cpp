// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/Fixtures/AssetDocumentAnimationProviderFixtures.h"

#include "AssetDocumentAnimationPrimitiveProvider.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Provider/AssetDocumentAnimationPrimitiveCatalog.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "AssetDocumentPluginPaths.h"

namespace AssetDocumentAnimationTimelineLifecycleTests
{
constexpr const TCHAR* AnimationProfileConsumer = TEXT("animation_profile");
constexpr const TCHAR* TimelineGate = TEXT("T0-TIMELINE");
constexpr const TCHAR* NotifyLibraryDependency =
	TEXT("AD.PANIM.BLOCK.NOTIFY_LIBRARY_DEPENDENCY");
constexpr const TCHAR* RuntimeHandleBootstrap =
	TEXT("AD.PANIM.BLOCK.RUNTIME_HANDLE_BOOTSTRAP");

struct FTimelineDecisionExpectation
{
	const TCHAR* PrimitiveId;
	const TCHAR* RowDiagnostic;
	const TCHAR* FinalDiagnostic;
	bool bMutating;
	uint8 RequiredInputHandleCount;
	EAssetDocumentAnimationBootstrapDependency BootstrapDependency;
	EAssetDocumentAnimationBootstrapDisposition BootstrapDisposition;
};

const FTimelineDecisionExpectation TimelineDecisions[] = {
	{TEXT("animation.notify_track.enumerate.v1"), NotifyLibraryDependency,
		NotifyLibraryDependency, false, 1,
		EAssetDocumentAnimationBootstrapDependency::Asset,
		EAssetDocumentAnimationBootstrapDisposition::VerifiedAssetIdentityReady},
	{TEXT("animation.notify_track.replace.v1"), NotifyLibraryDependency,
		NotifyLibraryDependency, true, 1,
		EAssetDocumentAnimationBootstrapDependency::Asset,
		EAssetDocumentAnimationBootstrapDisposition::VerifiedAssetIdentityReady},
	{TEXT("animation.notify_event.enumerate.v1"), NotifyLibraryDependency,
		NotifyLibraryDependency, false, 1,
		EAssetDocumentAnimationBootstrapDependency::Asset,
		EAssetDocumentAnimationBootstrapDisposition::VerifiedAssetIdentityReady},
	{TEXT("animation.notify_event.remove.v1"),
		TEXT("AD.PANIM.BLOCK.NOTIFY_EVENT_IDENTITY_AUTHORITY"),
		RuntimeHandleBootstrap, true, 2,
		EAssetDocumentAnimationBootstrapDependency::AssetAndProviderDerived,
		EAssetDocumentAnimationBootstrapDisposition::RuntimeHandleBootstrapBlocked},
	{TEXT("animation.notify_event.add.v1"), NotifyLibraryDependency,
		RuntimeHandleBootstrap, true, 2,
		EAssetDocumentAnimationBootstrapDependency::AssetAndClass,
		EAssetDocumentAnimationBootstrapDisposition::RuntimeHandleBootstrapBlocked},
	{TEXT("animation.sync_marker.enumerate.v1"), NotifyLibraryDependency,
		NotifyLibraryDependency, false, 1,
		EAssetDocumentAnimationBootstrapDependency::Asset,
		EAssetDocumentAnimationBootstrapDisposition::VerifiedAssetIdentityReady},
	{TEXT("animation.sync_marker.replace.v1"), NotifyLibraryDependency,
		NotifyLibraryDependency, true, 1,
		EAssetDocumentAnimationBootstrapDependency::Asset,
		EAssetDocumentAnimationBootstrapDisposition::VerifiedAssetIdentityReady},
};

class FNonCallableTimelineHandleResolver final : public IAssetDocumentHandleResolver
{
public:
	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind,
		EAssetDocumentHandleAccess,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&&) override
	{
		++IssueCount;
		return {};
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle&,
		const FAssetDocumentHandleValidationContext&) override
	{
		++AcquireCount;
		return {};
	}

	virtual FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation&) override
	{
		++InvalidationCount;
		return {};
	}

	int32 IssueCount = 0;
	int32 AcquireCount = 0;
	int32 InvalidationCount = 0;
};

const FAssetDocumentAnimationDecision* FindDecision(FStringView PrimitiveId)
{
	for (const FAssetDocumentAnimationDecision& Decision :
		GetAssetDocumentAnimationDecisions())
	{
		if (Decision.PrimitiveId == PrimitiveId)
		{
			return &Decision;
		}
	}
	return nullptr;
}

const FAssetDocumentAnimationT0DecisionRecord* FindT0Decision(
	FStringView PrimitiveId)
{
	for (const FAssetDocumentAnimationT0DecisionRecord& Decision :
		GetAssetDocumentAnimationT0DecisionRecords())
	{
		if (FStringView(Decision.DecisionId) == PrimitiveId)
		{
			return &Decision;
		}
	}
	return nullptr;
}

int32 CountDescriptors(FStringView PrimitiveId)
{
	int32 Count = 0;
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor :
		GetAssetDocumentAnimationPrimitiveDescriptors())
	{
		Count += Descriptor.PrimitiveId == PrimitiveId;
	}
	return Count;
}

int32 CountBindings(FStringView PrimitiveId)
{
	int32 Count = 0;
	for (const FAssetDocumentAnimationPrimitiveBinding& Binding :
		GetAssetDocumentAnimationPrimitiveBindings())
	{
		Count += Binding.PrimitiveId == PrimitiveId && Binding.Handler != nullptr;
	}
	return Count;
}

FAssetDocumentPrimitiveInvocation MakeInvocation(const TCHAR* PrimitiveId)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = PrimitiveId;
	Invocation.Version = 1;
	Invocation.RequestId = FGuid::NewGuid();
	Invocation.TransactionId = FGuid::NewGuid();
	Invocation.RuntimeRevision = 31;
	Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
	return Invocation;
}

void AssertTerminalBlocker(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const FNonCallableTimelineHandleResolver& Handles,
	FStringView ExpectedDiagnostic)
{
	Test.TestEqual(TEXT("timeline decision is terminally failed"), Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestEqual(TEXT("timeline failure has one exact diagnostic"),
		Outcome.Diagnostics.Num(), 1);
	if (Outcome.Diagnostics.Num() == 1)
	{
		Test.TestEqual(TEXT("timeline failure diagnostic path is exact"),
			Outcome.Diagnostics[0].Path, FString(TEXT("/Primitive")));
		Test.TestEqual(TEXT("timeline failure diagnostic is exact"),
			Outcome.Diagnostics[0].Code, FString(ExpectedDiagnostic));
	}
	Test.TestTrue(TEXT("timeline blocker returns no public value fields"),
		Outcome.Value.ObjectKeys.IsEmpty() && Outcome.Value.Elements.IsEmpty());
	Test.TestTrue(TEXT("timeline blocker returns no public handles"),
		Outcome.HandleResults.IsEmpty());
	Test.TestTrue(TEXT("timeline blocker returns no changed set"),
		Outcome.ChangedSet.IsEmpty());
	Test.TestEqual(TEXT("timeline blocker never issues a handle"),
		Handles.IssueCount, 0);
	Test.TestEqual(TEXT("timeline blocker never acquires a lease"),
		Handles.AcquireCount, 0);
	Test.TestEqual(TEXT("timeline blocker never invalidates a handle"),
		Handles.InvalidationCount, 0);
}

void AssertTimelineProductionAbsence(FAutomationTestBase& Test)
{
	const FString SourceRoot = FPaths::Combine(GetAssetDocumentPluginBaseDir(), TEXT("Source/AssetDocumentAnimation"));
	const FString TimelineHeader = FPaths::Combine(SourceRoot,
		TEXT("Private/Provider/Timeline/AssetDocumentAnimationTimelineOperations.h"));
	const FString TimelineSource = FPaths::Combine(SourceRoot,
		TEXT("Private/Provider/Timeline/AssetDocumentAnimationTimelineOperations.cpp"));
	Test.TestTrue(TEXT("Animation production source root exists"),
		IFileManager::Get().DirectoryExists(*SourceRoot));
	Test.TestFalse(TEXT("no timeline operation header is admitted"),
		IFileManager::Get().FileExists(*TimelineHeader));
	Test.TestFalse(TEXT("no timeline operation source is admitted"),
		IFileManager::Get().FileExists(*TimelineSource));

}
} // namespace AssetDocumentAnimationTimelineLifecycleTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAnimationTimelineLifecycleTest,
	"UEAssetDoc.AnimationProvider.NotifyTrackMarkerLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationTimelineLifecycleTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimationTimelineLifecycleTests;

	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAnimationPrimitiveProvider();
	TestEqual(TEXT("all seven frozen timeline decisions remain catalog rows"),
		static_cast<int32>(UE_ARRAY_COUNT(TimelineDecisions)), 7);
	for (const FTimelineDecisionExpectation& Expected : TimelineDecisions)
	{
		const FAssetDocumentAnimationDecision* Decision =
			FindDecision(Expected.PrimitiveId);
		TestNotNull(FString::Printf(TEXT("timeline catalog keeps %s"),
			Expected.PrimitiveId), Decision);
		if (Decision == nullptr)
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("%s remains non-callable"),
			Expected.PrimitiveId), Decision->Disposition,
			EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker);
		TestEqual(FString::Printf(TEXT("%s retains its exact consumer"),
			Expected.PrimitiveId), Decision->ConsumerId,
			FString(AnimationProfileConsumer));
		TestEqual(FString::Printf(TEXT("%s retains the timeline T0 gate"),
			Expected.PrimitiveId), Decision->AuthorityGateId, FString(TimelineGate));
		TestEqual(FString::Printf(TEXT("%s retains its final diagnostic"),
			Expected.PrimitiveId), Decision->StableBlockerDiagnostic,
			FString(Expected.FinalDiagnostic));
		TestEqual(FString::Printf(TEXT("%s has no admitted descriptor"),
			Expected.PrimitiveId), CountDescriptors(Expected.PrimitiveId), 0);
		TestEqual(FString::Printf(TEXT("%s has no callable binding"),
			Expected.PrimitiveId), CountBindings(Expected.PrimitiveId), 0);

		const FAssetDocumentAnimationT0DecisionRecord* T0Decision =
			FindT0Decision(Expected.PrimitiveId);
		TestNotNull(FString::Printf(TEXT("T0 evidence retains %s"),
			Expected.PrimitiveId), T0Decision);
		if (T0Decision == nullptr)
		{
			continue;
		}
		TestFalse(FString::Printf(TEXT("%s is not a G1 blocker"),
			Expected.PrimitiveId), T0Decision->bFrozenG1Blocker);
		TestEqual(FString::Printf(TEXT("%s T0 gate is exact"),
			Expected.PrimitiveId), FString(T0Decision->AuthorityGateId),
			FString(TimelineGate));
		TestEqual(FString::Printf(TEXT("%s row evidence is exact"),
			Expected.PrimitiveId), FString(T0Decision->RowDiagnostic),
			FString(Expected.RowDiagnostic));
		TestEqual(FString::Printf(TEXT("%s final evidence is exact"),
			Expected.PrimitiveId), FString(T0Decision->FinalDiagnostic),
			FString(Expected.FinalDiagnostic));
		TestEqual(FString::Printf(TEXT("%s mutability is frozen"),
			Expected.PrimitiveId), T0Decision->bMutating, Expected.bMutating);
		TestEqual(FString::Printf(TEXT("%s handle count is frozen"),
			Expected.PrimitiveId), T0Decision->RequiredInputHandleCount,
			Expected.RequiredInputHandleCount);
		TestEqual(FString::Printf(TEXT("%s bootstrap dependency is frozen"),
			Expected.PrimitiveId), T0Decision->BootstrapDependency,
			Expected.BootstrapDependency);
		TestEqual(FString::Printf(TEXT("%s bootstrap disposition is frozen"),
			Expected.PrimitiveId), T0Decision->BootstrapDisposition,
			Expected.BootstrapDisposition);
		TestEqual(FString::Printf(TEXT("%s final disposition is frozen"),
			Expected.PrimitiveId), T0Decision->FinalDisposition,
			EAssetDocumentAnimationFinalDisposition::StableBlocker);

		const FAssetDocumentPrimitiveInvocation Invocation =
			MakeInvocation(Expected.PrimitiveId);
		FNonCallableTimelineHandleResolver Handles;
		AssertTerminalBlocker(*this, Provider->Invoke(Invocation, Handles, nullptr),
			Handles, Expected.FinalDiagnostic);
	}

	AssertTimelineProductionAbsence(*this);
	return true;
}

#endif
