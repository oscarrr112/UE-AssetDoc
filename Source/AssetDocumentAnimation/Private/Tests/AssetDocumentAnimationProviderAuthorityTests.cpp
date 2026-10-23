// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/Fixtures/AssetDocumentAnimationProviderFixtures.h"

#include "Animation/AnimationAsset.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "AnimStateEntryNode.h"
#include "AnimStateTransitionNode.h"
#include "AssetDocumentCoreRuntimeComposition.h"
#include "AssetDocumentEditorNativeState.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Runtime/AssetDocumentRequestCoordinator.h"

#include <type_traits>
#include "AssetDocumentPluginPaths.h"

namespace AnimationProviderAuthorityTestsLocal
{
using FSetPreviewMeshSignature =
	void (UAnimationAsset::*)(USkeletalMesh*, bool);
using FSetSkeletonSignature =
	void (UAnimationAsset::*)(USkeleton*);
using FSetRetargetSourceAssetSignature =
	void (UAnimSequence::*)(USkeletalMesh*);
using FNoArgumentSequenceMutationSignature =
	void (UAnimSequence::*)();
using FRefreshSequenceBaseCacheSignature =
	void (UAnimSequenceBase::*)();
using FGetDataModelSignature =
	IAnimationDataModel* (UAnimSequenceBase::*)() const;
using FGetFloatCurvesSignature =
	const TArray<FFloatCurve>& (IAnimationDataModel::*)() const;
using FAddCurveSignature =
	bool (IAnimationDataController::*)(
		const FAnimationCurveIdentifier&, int32, bool);
using FRemoveCurveSignature =
	bool (IAnimationDataController::*)(
		const FAnimationCurveIdentifier&, bool);
using FSetCurveFlagsSignature =
	bool (IAnimationDataController::*)(
		const FAnimationCurveIdentifier&, int32, bool);
using FSetCurveKeysSignature =
	bool (IAnimationDataController::*)(
		const FAnimationCurveIdentifier&,
		const TArray<FRichCurveKey>&,
		bool);
using FGetMetaDataSignature =
	const TArray<UAnimMetaData*>& (UAnimationAsset::*)() const;
using FAddMetaDataSignature =
	void (UAnimationAsset::*)(UAnimMetaData*);
using FRemoveMetaDataSignature =
	void (UAnimationAsset::*)(UAnimMetaData*);
using FGetAssetUserDataArraySignature =
	const TArray<UAssetUserData*>* (UAnimationAsset::*)() const;
using FCreateConnectionsSignature =
	void (UAnimStateTransitionNode::*)(
		UAnimStateNodeBase*, UAnimStateNodeBase*);
using FGetConnectedStateSignature =
	UAnimStateNodeBase* (UAnimStateTransitionNode::*)() const;
using FGetOutputNodeSignature =
	UEdGraphNode* (UAnimStateEntryNode::*)() const;
using FGetOutputPinSignature =
	UEdGraphPin* (UAnimStateEntryNode::*)() const;
using FGetMetadataSignature =
	const FAssetDocumentRequestContextMetadata&
		(IAssetDocumentNativeRequestContext::*)() const;
using FInvokeSignature =
	FAssetDocumentPrimitiveOutcome
		(IAssetDocumentNativeRequestContext::*)(
			const FAssetDocumentPrimitiveInvocation&);
using FInspectCatalogSignature =
	FAssetDocumentPrimitiveRegistrySnapshot
		(IAssetDocumentNativeRequestContext::*)() const;
using FHandoffSignature =
	FAssetDocumentIssuedHandleResult
		(IAssetDocumentNativeRequestContext::*)(
			const FAssetDocumentOpaqueHandle&,
			FStringView,
			EAssetDocumentHandleAccess);
using FGetPayloadTypeIdSignature =
	FName (IAssetDocumentNativeHandlePayload::*)() const;
using FIsPayloadAliveSignature =
	bool (IAssetDocumentNativeHandlePayload::*)() const;
using FEnlistRollbackActionSignature =
	FAssetDocumentContractValidationResult
		(IAssetDocumentAtomicTransaction::*)(
			TSharedRef<IAssetDocumentRollbackAction>);
using FWithAssetIdentitySignature =
	FAssetDocumentContractValidationResult (*) (
		const IAssetDocumentVerifiedHandleLease&,
		FAssetDocumentEditorAssetIdentityConsumer);
using FGetEditorIdentityObjectSignature =
	UObject& (FAssetDocumentEditorAssetIdentityView::*)() const;
using FGetEditorIdentityPathSignature =
	FStringView (FAssetDocumentEditorAssetIdentityView::*)() const;
using FGetEditorIdentityRevisionSignature =
	uint64 (FAssetDocumentEditorAssetIdentityView::*)() const;

static_assert(std::is_same_v<
	decltype(static_cast<FSetPreviewMeshSignature>(
		&UAnimationAsset::SetPreviewMesh)),
	FSetPreviewMeshSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FSetSkeletonSignature>(
		&UAnimationAsset::SetSkeleton)),
	FSetSkeletonSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FSetRetargetSourceAssetSignature>(
		&UAnimSequence::SetRetargetSourceAsset)),
	FSetRetargetSourceAssetSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FNoArgumentSequenceMutationSignature>(
		&UAnimSequence::ClearRetargetSourceAsset)),
	FNoArgumentSequenceMutationSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FNoArgumentSequenceMutationSignature>(
		&UAnimSequence::UpdateRetargetSourceAssetData)),
	FNoArgumentSequenceMutationSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FRefreshSequenceBaseCacheSignature>(
		&UAnimSequenceBase::RefreshCacheData)),
	FRefreshSequenceBaseCacheSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FNoArgumentSequenceMutationSignature>(
		&UAnimSequence::RefreshSyncMarkerDataFromAuthored)),
	FNoArgumentSequenceMutationSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FGetDataModelSignature>(
		&UAnimSequenceBase::GetDataModel)),
	FGetDataModelSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FGetFloatCurvesSignature>(
		&IAnimationDataModel::GetFloatCurves)),
	FGetFloatCurvesSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FAddCurveSignature>(
		&IAnimationDataController::AddCurve)),
	FAddCurveSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FRemoveCurveSignature>(
		&IAnimationDataController::RemoveCurve)),
	FRemoveCurveSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FSetCurveFlagsSignature>(
		&IAnimationDataController::SetCurveFlags)),
	FSetCurveFlagsSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FSetCurveKeysSignature>(
		&IAnimationDataController::SetCurveKeys)),
	FSetCurveKeysSignature>);
static_assert(std::is_constructible_v<
	IAnimationDataController::FScopedBracket,
	IAnimationDataController&,
	const FText&,
	bool>);
static_assert(std::is_same_v<
	decltype(static_cast<FGetMetaDataSignature>(
		&UAnimationAsset::GetMetaData)),
	FGetMetaDataSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FAddMetaDataSignature>(
		&UAnimationAsset::AddMetaData)),
	FAddMetaDataSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FRemoveMetaDataSignature>(
		&UAnimationAsset::RemoveMetaData)),
	FRemoveMetaDataSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FGetAssetUserDataArraySignature>(
		&UAnimationAsset::GetAssetUserDataArray)),
	FGetAssetUserDataArraySignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FCreateConnectionsSignature>(
		&UAnimStateTransitionNode::CreateConnections)),
	FCreateConnectionsSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FGetConnectedStateSignature>(
		&UAnimStateTransitionNode::GetPreviousState)),
	FGetConnectedStateSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FGetConnectedStateSignature>(
		&UAnimStateTransitionNode::GetNextState)),
	FGetConnectedStateSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FGetOutputNodeSignature>(
		&UAnimStateEntryNode::GetOutputNode)),
	FGetOutputNodeSignature>);
static_assert(std::is_same_v<
	decltype(static_cast<FGetOutputPinSignature>(
		&UAnimStateEntryNode::GetOutputPin)),
	FGetOutputPinSignature>);
static_assert(std::is_same_v<
	decltype(&IAssetDocumentNativeRequestContext::GetMetadata),
	FGetMetadataSignature>);
static_assert(std::is_same_v<
	decltype(&IAssetDocumentNativeRequestContext::Invoke),
	FInvokeSignature>);
static_assert(std::is_same_v<
	decltype(&IAssetDocumentNativeRequestContext::InspectCatalog),
	FInspectCatalogSignature>);
static_assert(std::is_same_v<
	decltype(&IAssetDocumentNativeRequestContext::Handoff),
	FHandoffSignature>);
static_assert(std::is_same_v<
	decltype(&IAssetDocumentNativeHandlePayload::GetPayloadTypeId),
	FGetPayloadTypeIdSignature>);
static_assert(std::is_same_v<
	decltype(&IAssetDocumentNativeHandlePayload::IsAlive),
	FIsPayloadAliveSignature>);
static_assert(std::is_same_v<
	decltype(&IAssetDocumentAtomicTransaction::EnlistRollbackAction),
	FEnlistRollbackActionSignature>);
static_assert(std::is_same_v<
	decltype(FAssetDocumentCoreMutationInputs::RollbackActions),
	TArray<TSharedRef<IAssetDocumentRollbackAction>>>);
static_assert(std::is_same_v<
	decltype(&WithAssetDocumentEditorAssetIdentity),
	FWithAssetIdentitySignature>);
static_assert(std::is_same_v<
	decltype(&FAssetDocumentEditorAssetIdentityView::GetObject),
	FGetEditorIdentityObjectSignature>);
static_assert(std::is_same_v<
	decltype(&FAssetDocumentEditorAssetIdentityView::GetCanonicalObjectPath),
	FGetEditorIdentityPathSignature>);
static_assert(std::is_same_v<
	decltype(&FAssetDocumentEditorAssetIdentityView::GetTypeRevision),
	FGetEditorIdentityRevisionSignature>);
static_assert(!std::is_copy_constructible_v<FAssetDocumentEditorAssetIdentityView>);
static_assert(!std::is_copy_assignable_v<FAssetDocumentEditorAssetIdentityView>);
static_assert(!std::is_move_constructible_v<FAssetDocumentEditorAssetIdentityView>);
static_assert(!std::is_move_assignable_v<FAssetDocumentEditorAssetIdentityView>);

// The file names of the headers a source text includes, one per #include line.
TArray<FString> AnimationSourceIncludedHeaderNames(const FString& Text)
{
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	TArray<FString> Names;
	for (const FString& Line : Lines)
	{
		FString Directive = Line.TrimStart();
		if (!Directive.StartsWith(TEXT("#")))
		{
			continue;
		}
		Directive = Directive.RightChop(1).TrimStart();
		if (!Directive.StartsWith(TEXT("include")))
		{
			continue;
		}
		int32 Open = INDEX_NONE;
		Directive.FindChar(TEXT('"'), Open);
		if (Open == INDEX_NONE)
		{
			Directive.FindChar(TEXT('<'), Open);
		}
		if (Open == INDEX_NONE)
		{
			continue;
		}
		const FString Rest = Directive.RightChop(Open + 1);
		int32 Close = INDEX_NONE;
		if (!Rest.FindChar(TEXT('"'), Close) && !Rest.FindChar(TEXT('>'), Close))
		{
			continue;
		}
		Names.Add(FPaths::GetCleanFilename(Rest.Left(Close)));
	}
	return Names;
}

// The source text with comments and the contents of string, raw string and character literals blanked,
// so only identifiers that are code remain.
FString AnimationSourceCodeOnly(const FString& Text)
{
	FString Code = Text;
	const int32 Length = Code.Len();
	auto Blank = [&Code](int32 From, int32 To)
	{
		for (int32 Index = From; Index < To; ++Index)
		{
			if (Code[Index] != TEXT('\n'))
			{
				Code[Index] = TEXT(' ');
			}
		}
	};
	int32 Index = 0;
	while (Index < Length)
	{
		const TCHAR Current = Code[Index];
		const TCHAR Next = Index + 1 < Length ? Code[Index + 1] : TCHAR(0);
		if (Current == TEXT('/') && Next == TEXT('/'))
		{
			int32 End = Index;
			while (End < Length && Code[End] != TEXT('\n'))
			{
				++End;
			}
			Blank(Index, End);
			Index = End;
		}
		else if (Current == TEXT('/') && Next == TEXT('*'))
		{
			const int32 Found = Code.Find(TEXT("*/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index + 2);
			const int32 End = Found == INDEX_NONE ? Length : Found + 2;
			Blank(Index, End);
			Index = End;
		}
		else if (Current == TEXT('R') && Next == TEXT('"'))
		{
			const int32 OpenParen = Code.Find(TEXT("("), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index + 2);
			const FString Terminator = OpenParen == INDEX_NONE
				? FString()
				: TEXT(")") + Code.Mid(Index + 2, OpenParen - Index - 2) + TEXT("\"");
			const int32 Found = Terminator.IsEmpty()
				? INDEX_NONE
				: Code.Find(Terminator, ESearchCase::CaseSensitive, ESearchDir::FromStart, OpenParen + 1);
			const int32 End = Found == INDEX_NONE ? Length : Found + Terminator.Len();
			Blank(Index + 1, End);
			Index = End;
		}
		else if (Current == TEXT('"') || Current == TEXT('\''))
		{
			int32 End = Index + 1;
			while (End < Length && Code[End] != Current && Code[End] != TEXT('\n'))
			{
				End += Code[End] == TEXT('\\') ? 2 : 1;
			}
			End = FMath::Min(End + 1, Length);
			Blank(Index, End);
			Index = End;
		}
		else
		{
			++Index;
		}
	}
	return Code;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAnimationProviderAuthorityTest,
	"UEAssetDoc.AnimationProvider.Authority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationProviderAuthorityTest::RunTest(const FString&)
{
	const TConstArrayView<FAssetDocumentAnimationAuthorityDecisionRecord> Decisions =
		GetAssetDocumentAnimationAuthorityDecisionRecords();
	TestEqual(TEXT("frozen Animation decision closure"), Decisions.Num(), 31);

	TSet<FString> DecisionIds;
	TSet<FString> CoveredGroupIds;
	int32 AdmittedCount = 0;
	int32 StableBlockerCount = 0;
	int32 FrozenNotApplicableCount = 0;
	int32 VerifiedAssetIdentityReadyCount = 0;
	int32 RuntimeHandleBootstrapBlockedCount = 0;
	int32 AdmittedReadOnlyCount = 0;
	int32 MutatingCandidateCount = 0;
	for (const FAssetDocumentAnimationAuthorityDecisionRecord& Decision : Decisions)
	{
		DecisionIds.Add(Decision.DecisionId);
		CoveredGroupIds.Add(Decision.AuthorityGroupId);
		TestTrue(
			FString::Printf(TEXT("%s records its final diagnostic"),
				Decision.DecisionId),
			FCString::Strlen(Decision.FinalDiagnostic) > 0);

		switch (Decision.BootstrapDisposition)
		{
		case decltype(Decision.BootstrapDisposition)::FrozenNotApplicable:
			++FrozenNotApplicableCount;
			break;
		case decltype(Decision.BootstrapDisposition)::VerifiedAssetIdentityReady:
			++VerifiedAssetIdentityReadyCount;
			break;
		case decltype(Decision.BootstrapDisposition)::RuntimeHandleBootstrapBlocked:
			++RuntimeHandleBootstrapBlockedCount;
			TestEqual(
				FString::Printf(
					TEXT("%s retains the runtime bootstrap blocker"),
					Decision.DecisionId),
				FString(Decision.FinalDiagnostic),
				FString(TEXT("AD.ANIMATION.BLOCK.RUNTIME_HANDLE_BOOTSTRAP")));
			break;
		default:
			TestTrue(
				FString::Printf(
					TEXT("%s has a recognized bootstrap disposition"),
					Decision.DecisionId),
				false);
			break;
		}

		switch (Decision.FinalDisposition)
		{
		case decltype(Decision.FinalDisposition)::Admitted:
			++AdmittedCount;
			TestEqual(TEXT("Skeleton, Preview, Curves and Montage structural operations are admitted mutators"), Decision.bMutating,
				(FStringView(Decision.DecisionId) == TEXTVIEW("animation.asset.skeleton.set.v1")
				 || FStringView(Decision.DecisionId) == TEXTVIEW("animation.asset.preview_mesh.set.v1")
				 || FStringView(Decision.DecisionId) == TEXTVIEW("animation.curve.float.replace.v2")
				 || FStringView(Decision.DecisionId) == TEXTVIEW("animation.montage.slot_tracks.replace.v1")
				 || FStringView(Decision.DecisionId) == TEXTVIEW("animation.montage.sections.mutate.v1")));
			TestEqual(
				FString::Printf(
					TEXT("%s admission has verified Asset identity"),
					Decision.DecisionId),
				Decision.BootstrapDisposition,
				decltype(Decision.BootstrapDisposition)::VerifiedAssetIdentityReady);
			if (!Decision.bMutating) { ++AdmittedReadOnlyCount; }
			break;
		case decltype(Decision.FinalDisposition)::StableBlocker:
			++StableBlockerCount;
			break;
		default:
			TestTrue(
				FString::Printf(
					TEXT("%s has a recognized final disposition"),
					Decision.DecisionId),
				false);
			break;
		}

		if (Decision.bMutating)
		{
			++MutatingCandidateCount;
			TestEqual(
				FString::Printf(
					TEXT("%s mutator is never admitted without rollback composition"),
					Decision.DecisionId),
				Decision.FinalDisposition,
				(FStringView(Decision.DecisionId) == TEXTVIEW("animation.asset.skeleton.set.v1")
				 || FStringView(Decision.DecisionId) == TEXTVIEW("animation.asset.preview_mesh.set.v1")
				 || FStringView(Decision.DecisionId) == TEXTVIEW("animation.curve.float.replace.v2")
				 || FStringView(Decision.DecisionId) == TEXTVIEW("animation.montage.slot_tracks.replace.v1")
				 || FStringView(Decision.DecisionId) == TEXTVIEW("animation.montage.sections.mutate.v1"))
					? decltype(Decision.FinalDisposition)::Admitted
					: decltype(Decision.FinalDisposition)::StableBlocker);
		}
	}

	TestEqual(TEXT("decision identities are unique"), DecisionIds.Num(), 31);
	TestEqual(TEXT("final admitted decisions"), AdmittedCount, 9);
	TestEqual(TEXT("final stable blockers"), StableBlockerCount, 22);
	TestEqual(TEXT("frozen rows have no bootstrap projection"),
		FrozenNotApplicableCount, 6);
	TestEqual(TEXT("Asset and AssetPair rows have verified identity bootstrap"),
		VerifiedAssetIdentityReadyCount, 17);
	TestEqual(TEXT("derived Class and GraphNode rows lack runtime bootstrap"),
		RuntimeHandleBootstrapBlockedCount, 8);
	TestEqual(TEXT("four read-only admissions"), AdmittedReadOnlyCount, 4);
	TestEqual(TEXT("ordinary mutating candidates include Curves and the two structural operations"),
		MutatingCandidateCount, 17);

	const TConstArrayView<FAssetDocumentAnimationAuthorityEvidence> EvidenceRows =
		GetAssetDocumentAnimationAuthorityEvidence();
	TestEqual(TEXT("every authority group has its evidence"), EvidenceRows.Num(), 10);
	for (const FAssetDocumentAnimationAuthorityEvidence& Evidence : EvidenceRows)
	{
		TestTrue(
			FString::Printf(
				TEXT("%s is covered by a row or is provider-wide"),
				Evidence.GroupId),
			CoveredGroupIds.Contains(Evidence.GroupId)
				|| FStringView(Evidence.GroupId) == TEXTVIEW("ANIMATION-HANDLE")
				|| FStringView(Evidence.GroupId) == TEXTVIEW("ANIMATION-ROLLBACK"));
		TestTrue(
			FString::Printf(TEXT("%s records public module"), Evidence.GroupId),
			FCString::Strlen(Evidence.PublicModule) > 0);
		TestTrue(
			FString::Printf(TEXT("%s records public header"), Evidence.GroupId),
			FCString::Strlen(Evidence.PublicHeader) > 0);
		TestTrue(
			FString::Printf(TEXT("%s records export authority"), Evidence.GroupId),
			FCString::Strlen(Evidence.ExportMacro) > 0);
		TestTrue(
			FString::Printf(TEXT("%s records exact symbols"), Evidence.GroupId),
			FCString::Strlen(Evidence.PublicSymbols) > 0);
		TestTrue(
			FString::Printf(TEXT("%s records call context"), Evidence.GroupId),
			FCString::Strlen(Evidence.CallContext) > 0);
		TestTrue(
			FString::Printf(TEXT("%s records identity and lifetime"), Evidence.GroupId),
			FCString::Strlen(Evidence.IdentityAndLifetime) > 0);
		TestTrue(
			FString::Printf(TEXT("%s records transaction and rollback"), Evidence.GroupId),
			FCString::Strlen(Evidence.TransactionAndRollback) > 0);
		TestTrue(
			FString::Printf(TEXT("%s records completion"), Evidence.GroupId),
			FCString::Strlen(Evidence.Completion) > 0);
		TestTrue(
			FString::Printf(TEXT("%s records compile/link result"), Evidence.GroupId),
			FCString::Strlen(Evidence.CompileAndLinkResult) > 0);
		TestTrue(
			FString::Printf(TEXT("%s records focused test ID"), Evidence.GroupId),
			FCString::Strlen(Evidence.FocusedTestId) > 0);
		TestTrue(
			FString::Printf(TEXT("%s closes or blocks locally"), Evidence.GroupId),
			Evidence.bLocalAuthorityClosed
				|| FCString::Strlen(Evidence.StableDiagnostic) > 0);
	}
	return true;
}

// Animation edits go through this module's own primitives, never through the timeline mutation helpers of
// UAnimationBlueprintLibrary: no source of the module includes that library's header or names the class,
// and the pose-evaluation header of the same engine module is included only by the bone sampler.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAnimationTimelineLibraryBoundaryTest,
	"UEAssetDoc.AnimationProvider.TimelineLibraryBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimationTimelineLibraryBoundaryTest::RunTest(const FString&)
{
	using namespace AnimationProviderAuthorityTestsLocal;
	const FString SourceRoot = FPaths::Combine(GetAssetDocumentPluginBaseDir(), TEXT("Source/AssetDocumentAnimation"));
	TArray<FString> Paths;
	IFileManager::Get().FindFilesRecursive(Paths, *SourceRoot, TEXT("*"), true, false);
	Paths.Sort();
	const FString BoneSampler = TEXT("Private/AnimData/AnimationBoneSampling.cpp");
	TArray<FString> PoseHeaderIncluders;
	int32 Scanned = 0;
	bool bSawBoneSampler = false;
	for (const FString& Path : Paths)
	{
		const FString Extension = FPaths::GetExtension(Path);
		if (Extension != TEXT("h") && Extension != TEXT("cpp") && Extension != TEXT("inl"))
		{
			continue;
		}
		FString RelativePath = Path;
		FPaths::MakePathRelativeTo(RelativePath, *(SourceRoot + TEXT("/")));
		FPaths::NormalizeFilename(RelativePath);
		FString Text;
		if (!TestTrue(*FString::Printf(TEXT("source is readable: %s"), *RelativePath), FFileHelper::LoadFileToString(Text, *Path)))
		{
			continue;
		}
		++Scanned;
		bSawBoneSampler |= RelativePath == BoneSampler;
		for (const FString& Header : AnimationSourceIncludedHeaderNames(Text))
		{
			TestFalse(*FString::Printf(TEXT("%s does not include the timeline library header"), *RelativePath),
				Header == TEXT("AnimationBlueprintLibrary.h"));
			if (Header == TEXT("AnimPose.h"))
			{
				PoseHeaderIncluders.Add(RelativePath);
			}
		}
		TestFalse(*FString::Printf(TEXT("%s does not use UAnimationBlueprintLibrary"), *RelativePath),
			AnimationSourceCodeOnly(Text).Contains(TEXT("UAnimationBlueprintLibrary"), ESearchCase::CaseSensitive));
	}
	TestTrue(TEXT("the module's sources are scanned"), Scanned > 0 && bSawBoneSampler);
	TestTrue(*FString::Printf(TEXT("only the bone sampler includes the pose header (included by: %s)"),
			*FString::Join(PoseHeaderIncluders, TEXT(", "))),
		PoseHeaderIncluders.Num() == 1 && PoseHeaderIncluders[0] == BoneSampler);
	return !HasAnyErrors();
}

#endif
