// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/Fixtures/AssetDocumentAnimationProviderFixtures.h"

#include "AssetDocumentEditorNativeState.h"
#include "Misc/AutomationTest.h"
#include "Runtime/AssetDocumentHandleRegistry.h"

#include <type_traits>

namespace AnimationProviderHandleContractTestsLocal
{
using FWithAssetIdentitySignature =
	FAssetDocumentContractValidationResult (*) (
		const IAssetDocumentVerifiedHandleLease&,
		FAssetDocumentEditorAssetIdentityConsumer);
using FGetObjectSignature =
	UObject& (FAssetDocumentEditorAssetIdentityView::*)() const;
using FGetCanonicalObjectPathSignature =
	FStringView (FAssetDocumentEditorAssetIdentityView::*)() const;
using FGetTypeRevisionSignature =
	uint64 (FAssetDocumentEditorAssetIdentityView::*)() const;

static_assert(std::is_same_v<
	decltype(&WithAssetDocumentEditorAssetIdentity),
	FWithAssetIdentitySignature>);
static_assert(std::is_same_v<
	decltype(&FAssetDocumentEditorAssetIdentityView::GetObject),
	FGetObjectSignature>);
static_assert(std::is_same_v<
	decltype(&FAssetDocumentEditorAssetIdentityView::GetCanonicalObjectPath),
	FGetCanonicalObjectPathSignature>);
static_assert(std::is_same_v<
	decltype(&FAssetDocumentEditorAssetIdentityView::GetTypeRevision),
	FGetTypeRevisionSignature>);
static_assert(!std::is_copy_constructible_v<
	FAssetDocumentEditorAssetIdentityView>);
static_assert(!std::is_copy_assignable_v<
	FAssetDocumentEditorAssetIdentityView>);

class FStandaloneLease final : public IAssetDocumentVerifiedHandleLease
{
public:
	FStandaloneLease(
		FAssetDocumentOpaqueHandle InHandle,
		IAssetDocumentNativeHandlePayload& InPayload)
		: Handle(MoveTemp(InHandle))
		, Payload(&InPayload)
	{
	}

	virtual const FAssetDocumentOpaqueHandle& GetHandle() const override
	{
		return Handle;
	}

	virtual IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override
	{
		check(Payload != nullptr);
		return *Payload;
	}

private:
	FAssetDocumentOpaqueHandle Handle;
	IAssetDocumentNativeHandlePayload* Payload = nullptr;
};

class FWrongPayload final : public IAssetDocumentNativeHandlePayload
{
public:
	virtual FName GetPayloadTypeId() const override
	{
		return TEXT("p-anim.wrong-editor-asset-payload.v1");
	}

	virtual bool IsAlive() const override
	{
		return true;
	}
};

bool HasDiagnostic(
	const TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics,
	FStringView Code)
{
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Diagnostics)
	{
		if (Diagnostic.Code == Code)
		{
			return true;
		}
	}
	return false;
}

bool IsDecision(
	const FAssetDocumentAnimationT0DecisionRecord& Decision,
	FStringView DecisionId)
{
	return FStringView(Decision.DecisionId) == DecisionId;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAnimationProviderHandleContractTest,
	"UEAssetDoc.AnimationProvider.HandleContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationProviderHandleContractTest::RunTest(const FString&)
{
	using namespace AnimationProviderHandleContractTestsLocal;
	FAssetDocumentOpaqueHandle ForeignHandle;
	ForeignHandle.RequestId = FGuid::NewGuid();
	ForeignHandle.TransactionId = FGuid::NewGuid();
	ForeignHandle.RuntimeRevision = 1;
	ForeignHandle.ProviderId = TEXT("editor");
	ForeignHandle.Slot = 1;
	ForeignHandle.Generation = 1;
	ForeignHandle.Kind = EAssetDocumentHandleKind::Asset;
	ForeignHandle.Access = EAssetDocumentHandleAccess::Mutable;

	FAssetDocumentHandleValidationContext AnimationContext;
	AnimationContext.RequestId = ForeignHandle.RequestId;
	AnimationContext.TransactionId = ForeignHandle.TransactionId;
	AnimationContext.RuntimeRevision = ForeignHandle.RuntimeRevision;
	AnimationContext.ProviderId = TEXT("animation");
	AnimationContext.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	AnimationContext.RequiredKind = EAssetDocumentHandleKind::Asset;
	AnimationContext.RequiredAccess = EAssetDocumentHandleAccess::Mutable;

	const FAssetDocumentContractValidationResult ForeignProvider =
		ValidateAssetDocumentHandleMetadata(ForeignHandle, AnimationContext);
	TestFalse(
		TEXT("raw Editor handle is rejected before Animation metadata dispatch"),
		ForeignProvider.bValid);
	TestEqual(
		TEXT("raw Editor handle has a stable Animation provider mismatch"),
		ForeignProvider.Diagnostics.IsEmpty()
			? FString()
			: ForeignProvider.Diagnostics[0].Code,
		FString(TEXT("HandleMetadata.ProviderMismatch")));

	FWrongPayload WrongPayload;
	FStandaloneLease WrongPayloadLease(ForeignHandle, WrongPayload);
	int32 IdentityCallbackCount = 0;
	const FAssetDocumentContractValidationResult WrongPayloadResult =
		WithAssetDocumentEditorAssetIdentity(
			WrongPayloadLease,
			[&IdentityCallbackCount](
				const FAssetDocumentEditorAssetIdentityView&)
			{
				++IdentityCallbackCount;
				FAssetDocumentContractValidationResult Result;
				Result.bValid = true;
				return Result;
			});
	TestFalse(
		TEXT("public-only standalone lease rejects a wrong payload family"),
		WrongPayloadResult.bValid);
	TestTrue(
		TEXT("wrong payload diagnostic is stable"),
		HasDiagnostic(
			WrongPayloadResult.Diagnostics,
			TEXTVIEW("EditorNativeState.AssetPayloadTypeMismatch")));
	TestEqual(
		TEXT("wrong payload fails before the identity callback"),
		IdentityCallbackCount,
		0);

	int32 FrozenG1NotApplicableCount = 0;
	int32 OrdinaryCandidateCount = 0;
	int32 MutatingCandidateCount = 0;
	int32 ReadOnlyCandidateCount = 0;
	int32 ReadyBootstrapCount = 0;
	int32 RuntimeBlockedBootstrapCount = 0;
	int32 AssetBootstrapCount = 0;
	int32 AssetAndDerivedBootstrapCount = 0;
	int32 AssetPairBootstrapCount = 0;
	int32 AssetAndClassBootstrapCount = 0;
	int32 GraphOrNodeBootstrapCount = 0;
	int32 AdmittedCount = 0;
	int32 OrdinaryStableBlockerCount = 0;
	for (const FAssetDocumentAnimationT0DecisionRecord& Decision :
		GetAssetDocumentAnimationT0DecisionRecords())
	{
		if (Decision.bFrozenG1Blocker)
		{
			++FrozenG1NotApplicableCount;
			TestEqual(
				FString::Printf(
					TEXT("%s is frozen G1 bootstrap-not-applicable"),
					Decision.DecisionId),
				Decision.BootstrapDisposition,
				EAssetDocumentAnimationBootstrapDisposition::FrozenG1NotApplicable);
			TestEqual(
				FString::Printf(
					TEXT("%s is a frozen stable blocker"),
					Decision.DecisionId),
				Decision.FinalDisposition,
				EAssetDocumentAnimationFinalDisposition::StableBlocker);
			TestEqual(
				FString::Printf(
					TEXT("%s preserves its byte-exact G1 diagnostic"),
					Decision.DecisionId),
				FString(Decision.FinalDiagnostic),
				FString(Decision.RowDiagnostic));
			continue;
		}

		++OrdinaryCandidateCount;
		Decision.bMutating
			? ++MutatingCandidateCount
			: ++ReadOnlyCandidateCount;
		TestTrue(
			FString::Printf(
				TEXT("%s retains a required input handle"),
				Decision.DecisionId),
			Decision.RequiredInputHandleCount > 0);

		switch (Decision.BootstrapDisposition)
		{
		case EAssetDocumentAnimationBootstrapDisposition::VerifiedAssetIdentityReady:
			++ReadyBootstrapCount;
			break;
		case EAssetDocumentAnimationBootstrapDisposition::RuntimeHandleBootstrapBlocked:
			++RuntimeBlockedBootstrapCount;
			TestEqual(
				FString::Printf(
					TEXT("%s preserves the runtime handle bootstrap blocker"),
					Decision.DecisionId),
				FString(Decision.FinalDiagnostic),
				FString(TEXT("AD.PANIM.BLOCK.RUNTIME_HANDLE_BOOTSTRAP")));
			break;
		case EAssetDocumentAnimationBootstrapDisposition::FrozenG1NotApplicable:
			AddError(FString::Printf(
				TEXT("%s cannot be G1 bootstrap-not-applicable"),
				Decision.DecisionId));
			break;
		}

		switch (Decision.BootstrapDependency)
		{
		case EAssetDocumentAnimationBootstrapDependency::Asset:
			++AssetBootstrapCount;
			break;
		case EAssetDocumentAnimationBootstrapDependency::AssetAndProviderDerived:
			++AssetAndDerivedBootstrapCount;
			break;
		case EAssetDocumentAnimationBootstrapDependency::AssetPair:
			++AssetPairBootstrapCount;
			break;
		case EAssetDocumentAnimationBootstrapDependency::AssetAndClass:
			++AssetAndClassBootstrapCount;
			break;
		case EAssetDocumentAnimationBootstrapDependency::GraphOrNode:
			++GraphOrNodeBootstrapCount;
			break;
		case EAssetDocumentAnimationBootstrapDependency::None:
			AddError(FString::Printf(
				TEXT("%s lacks an ordinary bootstrap dependency classification"),
				Decision.DecisionId));
			break;
		}

		if (Decision.FinalDisposition
			== EAssetDocumentAnimationFinalDisposition::Admitted)
		{
			++AdmittedCount;
			TestEqual(TEXT("Skeleton, Preview, Curves and Montage structural operations are the admitted mutators"),
				Decision.bMutating,
				IsDecision(Decision, TEXTVIEW("animation.asset.skeleton.set.v1"))
				 || IsDecision(Decision, TEXTVIEW("animation.asset.preview_mesh.set.v1"))
				 || IsDecision(Decision, TEXTVIEW("animation.curve.float.replace.v2"))
				 || IsDecision(Decision, TEXTVIEW("animation.montage.slot_tracks.replace.v1"))
				 || IsDecision(Decision, TEXTVIEW("animation.montage.sections.mutate.v1")));
			TestEqual(
				FString::Printf(
					TEXT("%s admitted capability has verified Asset identity"),
					Decision.DecisionId),
				Decision.BootstrapDisposition,
				EAssetDocumentAnimationBootstrapDisposition::VerifiedAssetIdentityReady);
			TestEqual(
				FString::Printf(
					TEXT("%s admitted capability has no final diagnostic"),
					Decision.DecisionId),
				FString(Decision.FinalDiagnostic),
				FString(TEXT("none")));
			TestTrue(
				TEXT("only the four observations, Skeleton, Preview, Curves and Montage structural operations are admitted"),
				IsDecision(Decision, TEXTVIEW("animation.curve.float.enumerate.v1"))
					|| IsDecision(Decision, TEXTVIEW("animation.metadata.enumerate.v1"))
					|| IsDecision(Decision, TEXTVIEW("animation.asset_user_data.enumerate.v1"))
					|| IsDecision(Decision, TEXTVIEW("animation.asset.skeleton.set.v1"))
					|| IsDecision(Decision, TEXTVIEW("animation.asset.preview_mesh.set.v1"))
					|| IsDecision(Decision, TEXTVIEW("animation.curve.float.replace.v2"))
					|| IsDecision(Decision, TEXTVIEW("animation.curve.float.enumerate.v2"))
					|| IsDecision(Decision, TEXTVIEW("animation.montage.slot_tracks.replace.v1"))
					|| IsDecision(Decision, TEXTVIEW("animation.montage.sections.mutate.v1")));
		}
		else
		{
			++OrdinaryStableBlockerCount;
			TestEqual(
				FString::Printf(
					TEXT("%s is an ordinary stable blocker"),
					Decision.DecisionId),
				Decision.FinalDisposition,
				EAssetDocumentAnimationFinalDisposition::StableBlocker);
			if (IsDecision(Decision, TEXTVIEW("animation.curve.float.replace.v1")))
			{
				TestEqual(
					TEXT("curve float replace preserves the rollback composition blocker"),
					FString(Decision.FinalDiagnostic),
					FString(TEXT("AD.PANIM.BLOCK.ROLLBACK_COMPOSITION")));
			}
			else if (Decision.BootstrapDisposition
				== EAssetDocumentAnimationBootstrapDisposition::VerifiedAssetIdentityReady)
			{
				TestEqual(
					FString::Printf(
						TEXT("%s preserves its ready-row final diagnostic"),
						Decision.DecisionId),
					FString(Decision.FinalDiagnostic),
					FString(Decision.RowDiagnostic));
			}
		}
	}

	TestEqual(TEXT("frozen T0 decision closure"),
		GetAssetDocumentAnimationT0DecisionRecords().Num(), 31);
	TestEqual(TEXT("frozen G1 bootstrap-not-applicable candidates"),
		FrozenG1NotApplicableCount, 6);
	TestEqual(TEXT("ordinary candidates"), OrdinaryCandidateCount, 25);
	TestEqual(TEXT("ordinary mutating candidates"), MutatingCandidateCount, 17);
	TestEqual(TEXT("ordinary read-only candidates"), ReadOnlyCandidateCount, 8);
	TestEqual(TEXT("verified Asset identity ready candidates"), ReadyBootstrapCount, 17);
	TestEqual(TEXT("runtime handle bootstrap blocked candidates"),
		RuntimeBlockedBootstrapCount, 8);
	TestEqual(TEXT("Asset-only bootstrap candidates"), AssetBootstrapCount, 15);
	TestEqual(TEXT("Asset plus provider-derived bootstrap candidates"),
		AssetAndDerivedBootstrapCount, 2);
	TestEqual(TEXT("paired Asset bootstrap candidates"), AssetPairBootstrapCount, 2);
	TestEqual(TEXT("Asset plus Class bootstrap candidates"),
		AssetAndClassBootstrapCount, 1);
	TestEqual(TEXT("Graph or Node bootstrap candidates"),
		GraphOrNodeBootstrapCount, 5);
	TestEqual(TEXT("admitted capabilities"), AdmittedCount, 9);
	TestEqual(TEXT("ordinary stable blockers"), OrdinaryStableBlockerCount, 16);
	return true;
}

#endif
