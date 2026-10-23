// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetDocumentAnimationPrimitiveProvider.h"
#include "Provider/AssetDocumentAnimationPrimitiveCatalog.h"
#include "Tests/Fixtures/AssetDocumentAnimationProviderFixtures.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "AssetDocumentPluginPaths.h"

namespace AssetDocumentAnimationGraphStructuralLifecycleTests
{
constexpr const TCHAR* AnimationProfileConsumer = TEXT("animation_profile");
constexpr const TCHAR* AnimationProviderDesignConsumer =
	TEXT("animation_provider_design");
constexpr const TCHAR* RuntimeHandleBootstrapDiagnostic =
	TEXT("AD.ANIMATION.BLOCK.RUNTIME_HANDLE_BOOTSTRAP");

struct FExpectedGraphBlocker
{
	const TCHAR* PrimitiveId;
	EAssetDocumentAnimationDecisionDisposition CatalogDisposition;
	const TCHAR* ConsumerId;
	const TCHAR* CatalogGroupId;
	const TCHAR* RowDiagnostic;
	const TCHAR* FinalDiagnostic;
	bool bFrozenAuthority;
};

const TArray<FExpectedGraphBlocker>& GetExpectedGraphBlockers()
{
	static const TArray<FExpectedGraphBlocker> Rows = {
		{TEXT("animation.linked_anim_layer.action.enumerate.v1"),
			EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker,
			AnimationProfileConsumer, TEXT("ANIMATION-GRAPH"),
			TEXT("AD.ANIMATION.BLOCK.LINKED_LAYER_ACTION_AUTHORITY"),
			RuntimeHandleBootstrapDiagnostic, false},
		{TEXT("animation.anim_graph.lifecycle.v1"),
			EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker,
			AnimationProfileConsumer, TEXT("ANIMATION-GRAPH"),
			TEXT("AD.ANIMATION.BLOCK.ANIM_GRAPH_LIFECYCLE_AUTHORITY"),
			RuntimeHandleBootstrapDiagnostic, false},
		{TEXT("animation.linked_anim_layer.edit.v1"),
			EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker,
			AnimationProfileConsumer, TEXT("ANIMATION-GRAPH"),
			TEXT("AD.ANIMATION.BLOCK.LINKED_LAYER_EDIT_AUTHORITY"),
			RuntimeHandleBootstrapDiagnostic, false},
		{TEXT("animation.state_transition.connect.v1"),
			EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker,
			AnimationProfileConsumer, TEXT("ANIMATION-GRAPH"),
			TEXT("AD.ANIMATION.BLOCK.STATE_TRANSITION_AUTHORITY"),
			RuntimeHandleBootstrapDiagnostic, false},
		{TEXT("animation.state_entry.connect.v1"),
			EAssetDocumentAnimationDecisionDisposition::OrdinaryStableBlocker,
			AnimationProfileConsumer, TEXT("ANIMATION-GRAPH"),
			TEXT("AD.ANIMATION.BLOCK.STATE_ENTRY_AUTHORITY"),
			RuntimeHandleBootstrapDiagnostic, false},
		{TEXT("animation.state_machine.node.create.v1"),
			EAssetDocumentAnimationDecisionDisposition::FrozenStableBlocker,
			AnimationProviderDesignConsumer, TEXT("FROZEN-AUTHORITY"),
			TEXT("AD.BLOCK.ANIMATION.STATE_MACHINE_NODE_AUTHORITY"),
			TEXT("AD.BLOCK.ANIMATION.STATE_MACHINE_NODE_AUTHORITY"), true},
		{TEXT("animation.anim_parent_override.replace.v1"),
			EAssetDocumentAnimationDecisionDisposition::FrozenStableBlocker,
			AnimationProviderDesignConsumer, TEXT("FROZEN-AUTHORITY"),
			TEXT("AD.BLOCK.ANIMATION.PARENT_OVERRIDE_LIFECYCLE"),
			TEXT("AD.BLOCK.ANIMATION.PARENT_OVERRIDE_LIFECYCLE"), true},
	};
	return Rows;
}

class FRejectingGraphHandleResolver final : public IAssetDocumentHandleResolver
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

const FAssetDocumentAnimationDecision* FindCatalogDecision(FStringView PrimitiveId)
{
	return GetAssetDocumentAnimationDecisions().FindByPredicate(
		[PrimitiveId](const FAssetDocumentAnimationDecision& Decision)
		{
			return Decision.PrimitiveId == PrimitiveId;
		});
}

const FAssetDocumentAnimationAuthorityDecisionRecord* FindAuthorityDecision(
	FStringView PrimitiveId)
{
	return GetAssetDocumentAnimationAuthorityDecisionRecords().FindByPredicate(
		[PrimitiveId](const FAssetDocumentAnimationAuthorityDecisionRecord& Decision)
		{
			return FStringView(Decision.DecisionId) == PrimitiveId;
		});
}

void AssertNoCallableGraphSurface(
	FAutomationTestBase& Test,
	FStringView PrimitiveId)
{
	Test.TestNull(
		*FString::Printf(TEXT("%.*s has no descriptor"),
			PrimitiveId.Len(), PrimitiveId.GetData()),
		GetAssetDocumentAnimationPrimitiveDescriptors().FindByPredicate(
			[PrimitiveId](const FAssetDocumentPrimitiveDescriptor& Descriptor)
			{
				return Descriptor.PrimitiveId == PrimitiveId;
			}));
	Test.TestNull(
		*FString::Printf(TEXT("%.*s has no binding"),
			PrimitiveId.Len(), PrimitiveId.GetData()),
		GetAssetDocumentAnimationPrimitiveBindings().FindByPredicate(
			[PrimitiveId](const FAssetDocumentAnimationPrimitiveBinding& Binding)
			{
				return Binding.PrimitiveId == PrimitiveId;
			}));
}

void AssertProviderTerminal(
	FAutomationTestBase& Test,
	const FExpectedGraphBlocker& Expected,
	IAssetDocumentPrimitiveProvider& Provider)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = Expected.PrimitiveId;
	Invocation.Version = 1;
	FRejectingGraphHandleResolver Handles;
	const FAssetDocumentPrimitiveOutcome Outcome = Provider.Invoke(
		Invocation, Handles, nullptr);
	Test.TestEqual(
		*FString::Printf(TEXT("%s is terminal"), Expected.PrimitiveId),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestEqual(
		*FString::Printf(TEXT("%s emits one diagnostic"), Expected.PrimitiveId),
		Outcome.Diagnostics.Num(), 1);
	if (Outcome.Diagnostics.Num() == 1)
	{
		Test.TestEqual(TEXT("graph blocker diagnostic path is exact"),
			Outcome.Diagnostics[0].Path, FString(TEXT("/Primitive")));
		Test.TestEqual(TEXT("graph blocker diagnostic code is exact"),
			Outcome.Diagnostics[0].Code, FString(Expected.FinalDiagnostic));
		Test.TestEqual(TEXT("graph blocker diagnostic message is exact"),
			Outcome.Diagnostics[0].Message,
			FString(TEXT("The exact Animation decision remains non-callable.")));
	}
	Test.TestTrue(
		*FString::Printf(TEXT("%s publishes no value"), Expected.PrimitiveId),
		Outcome.Value.ObjectKeys.IsEmpty() && Outcome.Value.Elements.IsEmpty());
	Test.TestTrue(
		*FString::Printf(TEXT("%s publishes no handles"), Expected.PrimitiveId),
		Outcome.HandleResults.IsEmpty());
	Test.TestTrue(
		*FString::Printf(TEXT("%s publishes no changed set"), Expected.PrimitiveId),
		Outcome.ChangedSet.IsEmpty());
	Test.TestEqual(TEXT("blocked graph row issues no handles"), Handles.IssueCount, 0);
	Test.TestEqual(TEXT("blocked graph row acquires no leases"), Handles.AcquireCount, 0);
	Test.TestEqual(TEXT("blocked graph row invalidates no handles"),
		Handles.InvalidationCount, 0);
}

void AssertNoGraphFallbackSource(FAutomationTestBase& Test)
{
	const FString ModuleRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(GetAssetDocumentPluginBaseDir(), TEXT("Source/AssetDocumentAnimation")));
	const FString GraphRoot = FPaths::Combine(
		ModuleRoot, TEXT("Private/Provider/Graph"));
	Test.TestFalse(TEXT("no Graph production operation pair exists"),
		IFileManager::Get().DirectoryExists(*GraphRoot));

}
} // namespace AssetDocumentAnimationGraphStructuralLifecycleTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAnimationGraphStructuralLifecycleTest,
	"UEAssetDoc.AnimationProvider.AnimGraphStructuralLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationGraphStructuralLifecycleTest::RunTest(const FString&)
{
	using namespace AssetDocumentAnimationGraphStructuralLifecycleTests;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAnimationPrimitiveProvider();
	int32 OrdinaryCount = 0;
	int32 FrozenBlockerCount = 0;
	for (const FExpectedGraphBlocker& Expected : GetExpectedGraphBlockers())
	{
		const FAssetDocumentAnimationDecision* CatalogDecision =
			FindCatalogDecision(Expected.PrimitiveId);
		TestNotNull(
			*FString::Printf(TEXT("%s has a catalog decision"), Expected.PrimitiveId),
			CatalogDecision);
		const FAssetDocumentAnimationAuthorityDecisionRecord* AuthorityDecision =
			FindAuthorityDecision(Expected.PrimitiveId);
		TestNotNull(
			*FString::Printf(TEXT("%s has authority evidence"), Expected.PrimitiveId),
			AuthorityDecision);
		if (CatalogDecision == nullptr || AuthorityDecision == nullptr)
		{
			continue;
		}

		TestEqual(TEXT("catalog disposition is exact"), CatalogDecision->Disposition,
			Expected.CatalogDisposition);
		TestEqual(TEXT("catalog consumer is exact"), CatalogDecision->ConsumerId,
			FString(Expected.ConsumerId));
		TestEqual(TEXT("catalog authority group is exact"), CatalogDecision->AuthorityGroupId,
			FString(Expected.CatalogGroupId));
		TestEqual(TEXT("catalog diagnostic is exact"),
			CatalogDecision->StableBlockerDiagnostic,
			FString(Expected.FinalDiagnostic));
		TestEqual(TEXT("family diagnostic is retained"),
			FString(AuthorityDecision->RowDiagnostic), FString(Expected.RowDiagnostic));
		TestEqual(TEXT("final diagnostic is exact"),
			FString(AuthorityDecision->FinalDiagnostic), FString(Expected.FinalDiagnostic));
		TestEqual(TEXT("final disposition is stable blocker"),
			AuthorityDecision->FinalDisposition,
			EAssetDocumentAnimationFinalDisposition::StableBlocker);

		if (Expected.bFrozenAuthority)
		{
			++FrozenBlockerCount;
			TestTrue(TEXT("frozen graph row remains frozen authority"), AuthorityDecision->bFrozenBlocker);
			TestEqual(TEXT("frozen graph row has no bootstrap dependency"),
				AuthorityDecision->BootstrapDependency,
				EAssetDocumentAnimationBootstrapDependency::None);
			TestEqual(TEXT("frozen graph row remains bootstrap-not-applicable"),
				AuthorityDecision->BootstrapDisposition,
				EAssetDocumentAnimationBootstrapDisposition::FrozenNotApplicable);
		}
		else
		{
			++OrdinaryCount;
			TestFalse(TEXT("ordinary graph row is not frozen authority"), AuthorityDecision->bFrozenBlocker);
			TestEqual(TEXT("ordinary graph row needs GraphOrNode bootstrap"),
				AuthorityDecision->BootstrapDependency,
				EAssetDocumentAnimationBootstrapDependency::GraphOrNode);
			TestEqual(TEXT("ordinary graph row remains bootstrap-blocked"),
				AuthorityDecision->BootstrapDisposition,
				EAssetDocumentAnimationBootstrapDisposition::RuntimeHandleBootstrapBlocked);
		}

		AssertNoCallableGraphSurface(*this, Expected.PrimitiveId);
		AssertProviderTerminal(*this, Expected, *Provider);
	}
	TestEqual(TEXT("five ordinary graph rows are frozen"), OrdinaryCount, 5);
	TestEqual(TEXT("two frozen graph rows are retained"), FrozenBlockerCount, 2);
	AssertNoGraphFallbackSource(*this);
	return true;
}

#endif
