// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/Fixtures/AssetDocumentAnimationProviderFixtures.h"

#include "AssetDocumentAnimationPrimitiveProvider.h"
#include "Misc/AutomationTest.h"
#include "Provider/AssetDocumentAnimationPrimitiveCatalog.h"
#include "Provider/Montage/MontageStructuralProvider.h"
#include "Runtime/AssetDocumentHandleRegistry.h"

namespace AssetDocumentAnimationMontageCompletionTests
{
struct FExpectedBlocker
{
	const TCHAR* PrimitiveId;
	EAssetDocumentAnimationDecisionDisposition Disposition;
	const TCHAR* ConsumerId;
	const TCHAR* AuthorityGroupId;
	const TCHAR* Diagnostic;
};

constexpr FExpectedBlocker ExpectedBlockers[] = {
	{TEXT("animation.montage.section_metadata.replace.v1"),
		EAssetDocumentAnimationDecisionDisposition::FrozenStableBlocker,
		TEXT("animation_provider_design"), TEXT("FROZEN-AUTHORITY"),
		TEXT("AD.BLOCK.ANIMATION.SECTION_METADATA_AUTHORITY")},
	{TEXT("animation.sequence.complete_edit.v1"),
		EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker,
		TEXT("animation_profile"), TEXT("ANIMATION-SEQUENCE"),
		TEXT("AD.ANIMATION.BLOCK.SEQUENCE_COMPLETION_AUTHORITY")},
	{TEXT("animation.montage.sync_cache.complete.v1"),
		EAssetDocumentAnimationDecisionDisposition::FrozenStableBlocker,
		TEXT("animation_provider_design"), TEXT("FROZEN-AUTHORITY"),
		TEXT("AD.BLOCK.ANIMATION.SYNC_CACHE_AUTHORITY")},
	{TEXT("animation.montage.time_stretch.configure_bake.v1"),
		EAssetDocumentAnimationDecisionDisposition::FrozenStableBlocker,
		TEXT("animation_provider_design"), TEXT("FROZEN-AUTHORITY"),
		TEXT("AD.BLOCK.ANIMATION.TIME_STRETCH_CONFIG_AUTHORITY")},
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

int32 CountDescriptors(FStringView PrimitiveId)
{
	int32 Count = 0;
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor :
		GetAssetDocumentAnimationPrimitiveDescriptors())
	{
		if (Descriptor.PrimitiveId == PrimitiveId)
		{
			++Count;
		}
	}
	return Count;
}

int32 CountBindings(FStringView PrimitiveId)
{
	int32 Count = 0;
	for (const FAssetDocumentAnimationPrimitiveBinding& Binding :
		GetAssetDocumentAnimationPrimitiveBindings())
	{
		if (Binding.PrimitiveId == PrimitiveId && Binding.Handler != nullptr)
		{
			++Count;
		}
	}
	return Count;
}

class FNoActivityHandleResolver final : public IAssetDocumentHandleResolver
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

void AssertTerminalBlockerOutcome(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const FExpectedBlocker& Expected)
{
	Test.TestEqual(TEXT("non-callable invocation has a failed terminal outcome"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestEqual(TEXT("non-callable invocation has one exact diagnostic"),
		Outcome.Diagnostics.Num(), 1);
	if (Outcome.Diagnostics.Num() == 1)
	{
		Test.TestEqual(TEXT("non-callable invocation preserves its exact diagnostic code"),
			Outcome.Diagnostics[0].Code, FString(Expected.Diagnostic));
		Test.TestEqual(TEXT("non-callable invocation preserves the standard terminal path"),
			Outcome.Diagnostics[0].Path, FString(TEXT("/Primitive")));
		Test.TestEqual(TEXT("non-callable invocation preserves the standard terminal message"),
			Outcome.Diagnostics[0].Message,
			FString(TEXT("The exact Animation decision remains non-callable.")));
	}
	Test.TestEqual(TEXT("non-callable invocation returns an empty object value"),
		Outcome.Value.Kind, EAssetDocumentRuntimeValueKind::Object);
	Test.TestTrue(TEXT("non-callable invocation returns no value fields"),
		Outcome.Value.ObjectKeys.IsEmpty() && Outcome.Value.Elements.IsEmpty());
	Test.TestTrue(TEXT("non-callable invocation returns no handles"),
		Outcome.HandleResults.IsEmpty());
	Test.TestTrue(TEXT("non-callable invocation returns no changed set"),
		Outcome.ChangedSet.IsEmpty());
}
} // namespace AssetDocumentAnimationMontageCompletionTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAnimationMontageCompletionTest,
	"UEAssetDoc.AnimationProvider.MontageCompletion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationMontageCompletionTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimationMontageCompletionTests;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAnimationPrimitiveProvider();
	FNoActivityHandleResolver Handles;

	for (const FExpectedBlocker& Expected : ExpectedBlockers)
	{
		const FAssetDocumentAnimationDecision* Decision =
			FindDecision(Expected.PrimitiveId);
		TestNotNull(*FString::Printf(TEXT("%s remains in the immutable decision catalog"),
			Expected.PrimitiveId), Decision);
		if (Decision == nullptr)
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("%s retains its exact disposition"),
			Expected.PrimitiveId), Decision->Disposition, Expected.Disposition);
		TestEqual(FString::Printf(TEXT("%s retains its exact consumer"),
			Expected.PrimitiveId), Decision->ConsumerId, FString(Expected.ConsumerId));
		TestEqual(FString::Printf(TEXT("%s retains its exact authority group"),
			Expected.PrimitiveId), Decision->AuthorityGroupId,
			FString(Expected.AuthorityGroupId));
		TestEqual(FString::Printf(TEXT("%s retains its exact blocker diagnostic"),
			Expected.PrimitiveId), Decision->StableBlockerDiagnostic,
			FString(Expected.Diagnostic));
		TestEqual(FString::Printf(TEXT("%s has no admitted descriptor"),
			Expected.PrimitiveId), CountDescriptors(Expected.PrimitiveId), 0);
		TestEqual(FString::Printf(TEXT("%s has no callable binding"),
			Expected.PrimitiveId), CountBindings(Expected.PrimitiveId), 0);

		FAssetDocumentPrimitiveInvocation Invocation;
		Invocation.PrimitiveId = Expected.PrimitiveId;
		Invocation.Version = 1;
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(
			Invocation, Handles, nullptr);
		AssertTerminalBlockerOutcome(*this, Outcome, Expected);
	}

	// The two released structural rows remain covered here as admitted operations,
	// while metadata, generic sequence completion, sync and time-stretch keep their blockers.
	for (const auto& Descriptor : {MakeAnimationMontageSlotsDescriptor(), MakeAnimationMontageSectionsDescriptor()})
	{
		const auto* Decision = FindDecision(Descriptor.PrimitiveId);
		if (!TestNotNull(TEXT("structural decision remains in catalog"), Decision)) continue;
		TestEqual(TEXT("structural row is admitted"), Decision->Disposition, EAssetDocumentAnimationDecisionDisposition::Admitted);
		TestEqual(TEXT("structural consumer is animation profile"), Decision->ConsumerId, FString(TEXT("animation_profile")));
		TestEqual(TEXT("structural authority group is the approved typed publication"), Decision->AuthorityGroupId, FString(TEXT("MONTAGE-STRUCTURAL")));
		TestTrue(TEXT("structural row has no legacy blocker diagnostic"), Decision->StableBlockerDiagnostic.IsEmpty());
		TestTrue(TEXT("structural descriptor has exact authority shape"), IsMontageStructuralAdmissionShape(Descriptor));
		TestEqual(TEXT("structural row has one descriptor"), CountDescriptors(Descriptor.PrimitiveId), 1);
		TestEqual(TEXT("structural row has one callable binding"), CountBindings(Descriptor.PrimitiveId), 1);
		FAssetDocumentPrimitiveInvocation Invocation;
		Invocation.PrimitiveId = Descriptor.PrimitiveId; Invocation.Version = 1;
		const auto Outcome = Provider->Invoke(Invocation, Handles, nullptr);
		TestEqual(TEXT("admitted structural row still rejects missing transaction"), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
		TestEqual(TEXT("transaction rejection has one diagnostic"), Outcome.Diagnostics.Num(), 1);
		if (Outcome.Diagnostics.Num() == 1)
			TestEqual(TEXT("structural rejection is transaction requirement, not a frozen blocker"), Outcome.Diagnostics[0].Code, FString(TEXT("AnimationProvider.TransactionUnavailable")));
		TestTrue(TEXT("transaction rejection has no changed set or handles"), Outcome.ChangedSet.IsEmpty() && Outcome.HandleResults.IsEmpty());
	}

	TestEqual(TEXT("no blocked or transaction-rejected completion row issues a handle"),
		Handles.IssueCount, 0);
	TestEqual(TEXT("no blocked or transaction-rejected completion row acquires a lease"),
		Handles.AcquireCount, 0);
	TestEqual(TEXT("no blocked or transaction-rejected completion row invalidates handles"),
		Handles.InvalidationCount, 0);
	return true;
}

#endif
