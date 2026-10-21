// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentEditorCutoverTransactionPreparation.h"
#include "AssetDocumentEditorProviderTestSupport.h"
#include "AssetDocumentRollbackAction.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Factories/AnimSequenceFactory.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Provider/ClassReference/AssetDocumentEditorAssetPayload.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorAnimSequenceConsumerReplayTests
{
const TCHAR* ReplayIds[] = {
	TEXT("editor.class.resolve_canonical.v1"),
	TEXT("editor.reference.resolve_asset.v1"),
	TEXT("editor.property.enumerate_admitted_batch.v1"),
	TEXT("editor.property.preflight_admitted.v1"),
	TEXT("editor.property.read_admitted.v1"),
	TEXT("editor.property.write_admitted_batch.v1")};

FAssetDocumentRuntimeValueDto String(const FString& Text)
{
	FAssetDocumentRuntimeValueDto Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::String;
	Value.StringValue = Text;
	return Value;
}

FAssetDocumentRuntimeValueDto Object(
	TArray<FString> Keys, TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Value;
	Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	Value.ObjectKeys = MoveTemp(Keys);
	Value.Elements = MoveTemp(Values);
	return Value;
}

FAssetDocumentRuntimeValueDto Batch(bool bEmpty)
{
	FAssetDocumentRuntimeValueDto Rows;
	Rows.Kind = EAssetDocumentRuntimeValueKind::Array;
	if (!bEmpty)
	{
		FAssetDocumentRuntimeValueDto Rate;
		Rate.Kind = EAssetDocumentRuntimeValueKind::Number;
		Rate.NumberValue = 1.75;
		// The existing batch contract requires admission order (authored name).
		Rows.Elements = {
			Object({TEXT("name"), TEXT("value")},
				{String(TEXT("AdditiveAnimType")), String(TEXT("AAT_LocalSpaceBase"))}),
			Object({TEXT("name"), TEXT("value")}, {String(TEXT("RateScale")), Rate}),
			Object({TEXT("name"), TEXT("value")},
				{String(TEXT("RetargetSource")), String(TEXT("ReplayRetarget"))})};
	}
	return Object({TEXT("properties")}, {MoveTemp(Rows)});
}

const FAssetDocumentRuntimeValueDto* Field(
	const FAssetDocumentRuntimeValueDto& Value, const TCHAR* Name)
{
	const int32 Index = Value.ObjectKeys.IndexOfByKey(Name);
	return Value.Elements.IsValidIndex(Index) ? &Value.Elements[Index] : nullptr;
}

const FAssetDocumentPrimitiveHandleResult* Group(
	const FAssetDocumentPrimitiveOutcome& Outcome, const TCHAR* Name)
{
	return Outcome.HandleResults.FindByPredicate(
		[Name](const FAssetDocumentPrimitiveHandleResult& Item) { return Item.Name == Name; });
}

// A test-only bridge between the two existing public interfaces. Capture and
// restoration remain entirely owned by the prepared Editor seed, not this test.
class FSeedAction final : public IAssetDocumentRollbackAction
{
public:
	explicit FSeedAction(TSharedRef<IAssetDocumentEditorCutoverRollbackSeed> InSeed)
		: Seed(MoveTemp(InSeed)) {}
	virtual FStringView GetRollbackComponentId() const override
	{
		return Seed->GetRollbackComponentId();
	}
	virtual FAssetDocumentContractValidationResult Rollback(FStringView Code) override
	{
		return Seed->Rollback(Code);
	}
private:
	TSharedRef<IAssetDocumentEditorCutoverRollbackSeed> Seed;
};

class FCancellation final : public IAssetDocumentCancellationView
{
public:
	virtual bool IsCancellationRequested() const override { return false; }
};

enum class EReplayMode { ProviderBatch, CoreInitial, EmptyBatch };

bool Replay(FAutomationTestBase& Test, IAssetDocumentPrimitiveProvider& Provider,
	EReplayMode Mode, EAssetDocumentRuntimeRequestPhase Phase)
{
	const FString Name = TEXT("AnimSequenceReplay_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TStrongObjectPtr<UPackage> Package(CreatePackage(*(TEXT("/Game/AssetDocumentTests/") + Name)));
	ON_SCOPE_EXIT { Package->SetDirtyFlag(false); };
	// Reuse the stock skeleton fixture already used by AnimSequence tests,
	// duplicating it so this replay owns every object it may update.
	USkeleton* SourceSkeleton = LoadObject<USkeleton>(nullptr,
		TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton"));
	if (!Test.TestNotNull(TEXT("stock skeleton fixture loads"), SourceSkeleton)) return false;
	TStrongObjectPtr<USkeleton> Skeleton(DuplicateObject<USkeleton>(
		SourceSkeleton, GetTransientPackage(), *(Name + TEXT("_Skeleton"))));
	if (!Test.TestNotNull(TEXT("test-owned skeleton duplicates"), Skeleton.Get())) return false;
	Skeleton->SetFlags(RF_Transient);
	Skeleton->ClearFlags(RF_Public | RF_Standalone);
	TStrongObjectPtr<UAnimSequenceFactory> Factory(NewObject<UAnimSequenceFactory>());
	Factory->TargetSkeleton = Skeleton.Get();
	// FactoryCreateNew performs InitializeModel, frame initialization and
	// NotifyPopulated before any replayed property triggers PostEditChange.
	TStrongObjectPtr<UAnimSequence> Asset(Cast<UAnimSequence>(Factory->FactoryCreateNew(
		UAnimSequence::StaticClass(), Package.Get(), *Name, RF_Transactional, nullptr, GWarn)));
	if (!Test.TestNotNull(TEXT("stock factory creates initialized sequence"), Asset.Get())) return false;
	Test.TestEqual(TEXT("factory assigns test-owned skeleton"), Asset->GetSkeleton(), Skeleton.Get());
	const float RateBefore = Asset->RateScale;
	const FName RetargetBefore = Asset->RetargetSource;
	const EAdditiveAnimationType AdditiveBefore = Asset->AdditiveAnimType;
	const int32 FrameBefore = Asset->RefFrameIndex;
	const bool bDirtyBefore = Package->IsDirty();

	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FAssetDocumentEditorCutoverMutationIntent Intent;
	Intent.RequestId = Context.RequestId;
	Intent.RuntimeRevision = Context.RuntimeRevision;
	Intent.CanonicalObjectPath = Asset->GetPathName();
	Intent.ExactAssetClassPath = Asset->GetClass()->GetPathName();
	Intent.bWriteAdmittedProperties = true;
	const auto Preparation = PrepareAssetDocumentEditorCutoverTransaction(Intent);
	if (!Test.TestTrue(TEXT("real AnimSequence typed preimage prepares before Core opens"), Preparation.bPrepared))
	{
		for (const auto& Diagnostic : Preparation.Diagnostics) Test.AddError(Diagnostic.Code);
		return false;
	}
	TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions;
	for (const auto& Seed : Preparation.RollbackSeeds) Actions.Add(MakeShared<FSeedAction>(Seed));
	FAssetDocumentRollbackActionComposition Composition(Handles, MoveTemp(Actions),
		[](EAssetDocumentRuntimeRequestPhase, const FAssetDocumentRuntimeDiagnostic&) {});
	FAssetDocumentPreflightReceipt Receipt;
	Receipt.RequestId = Context.RequestId;
	Receipt.RuntimeRevision = Context.RuntimeRevision;
	Receipt.CompleteFootprint = Preparation.CompleteFootprint;
	Receipt.ReceiptHash = TEXT("test-owned:animation-consumer-replay");
	Receipt.bSucceeded = true;
	FCancellation Cancellation;
	auto Transaction = Composition.BeginLiveAfterPreflight(Receipt, Cancellation);
	if (!Test.TestNotNull(TEXT("real Core factory accepts prepared rollback coverage"), Transaction.Get())) return false;
	Context.TransactionId = Transaction->GetTransactionId();
	// Rebind the still-empty test resolver in place, keeping Core invalidation
	// and provider leases on the same resolver with the actual transaction ID.
	Handles = FAssetDocumentEditorProviderTestHandleResolver(Context);
	auto& LiveHandles = Handles;
	TArray<TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>> Captures;
	for (const auto& Seed : Preparation.RollbackSeeds)
	{
		auto Capture = Seed->OpenMutationCapture(Context.TransactionId);
		if (!Test.TestNotNull(TEXT("prepared seed opens against actual transaction"), Capture.Get())) return false;
		Captures.Add(MoveTemp(Capture));
	}
	ON_SCOPE_EXIT
	{
		Captures.Reset();
		if (Transaction->GetState() != EAssetDocumentAtomicState::RolledBack)
		{
			Transaction->Rollback(TEXT("Replay.Cleanup"));
		}
	};

	int32 BatchInvocationCount = 0;
	if (Mode == EReplayMode::CoreInitial)
	{
		Test.TestTrue(TEXT("CoreInitial enlists the separately prepared complete footprint"),
			Transaction->EnlistFootprint(Preparation.CompleteFootprint).bValid);
	}
	else
	{
		auto Invoke = [&](const TCHAR* Id, FAssetDocumentRuntimeValueDto Arguments,
			TArray<FAssetDocumentPrimitiveHandleArgument> ArgumentsHandles,
			EAssetDocumentRuntimeRequestPhase CallPhase)
		{
			FAssetDocumentPrimitiveInvocation Invocation;
			Invocation.PrimitiveId = Id;
			Invocation.Version = 1;
			Invocation.RequestId = Context.RequestId;
			Invocation.TransactionId = Context.TransactionId;
			Invocation.RuntimeRevision = Context.RuntimeRevision;
			Invocation.Phase = CallPhase;
			Invocation.ValueArguments = MoveTemp(Arguments);
			Invocation.HandleArguments = MoveTemp(ArgumentsHandles);
			return Provider.Invoke(Invocation, LiveHandles, Transaction.Get());
		};
		const auto ResolvedClass = Invoke(ReplayIds[0], Object({TEXT("class"), TEXT("expected_base")},
			{String(Asset->GetClass()->GetPathName()), String(UAnimationAsset::StaticClass()->GetPathName())}), {}, EAssetDocumentRuntimeRequestPhase::Preflight);
		Test.TestEqual(TEXT("canonical class resolver replays"), ResolvedClass.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		const auto ResolvedAsset = Invoke(ReplayIds[1], Object({TEXT("expected_class"), TEXT("path")},
			{String(Asset->GetClass()->GetPathName()), String(Asset->GetPathName())}), {}, EAssetDocumentRuntimeRequestPhase::Preflight);
		if (!Test.TestEqual(TEXT("canonical asset resolver replays"), ResolvedAsset.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
		const auto* AssetGroup = Group(ResolvedAsset, TEXT("asset"));
		if (!Test.TestTrue(TEXT("resolver returns one asset"), AssetGroup && AssetGroup->Handles.Num() == 1)) return false;
		const auto Admission = Invoke(ReplayIds[2], Object({}, {}), {{TEXT("asset"), AssetGroup->Handles[0]}}, Phase);
		if (!Test.TestEqual(TEXT("fresh phase-local batch admission succeeds"), Admission.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
		const auto* Rows = Field(Admission.Value, TEXT("properties"));
		const auto* Properties = Group(Admission, TEXT("properties"));
		const auto* BatchGroup = Group(Admission, TEXT("property_batch"));
		if (!Test.TestTrue(TEXT("admission returns typed rows and exactly one opaque batch"),
			Rows && Properties && BatchGroup && BatchGroup->Handles.Num() == 1)) return false;
		for (const TCHAR* NameToFind : {TEXT("RetargetSource"), TEXT("RateScale"), TEXT("AdditiveAnimType"),
			TEXT("RefPoseType"), TEXT("RefFrameIndex"), TEXT("bEnableRootMotion"), TEXT("RootMotionRootLock"),
			TEXT("bForceRootLock"), TEXT("bUseNormalizedRootMotionScale"),
			TEXT("CompressionErrorThresholdScale"), TEXT("bDoNotOverrideCompression")})
		{
			const int32 Index = Rows->Elements.IndexOfByPredicate([NameToFind](const auto& Row)
			{
				const auto* NameValue = Field(Row, TEXT("name"));
				return NameValue && NameValue->StringValue == NameToFind;
			});
			if (!Test.TestTrue(FString::Printf(TEXT("frozen projection admits %s"), NameToFind),
				Properties->Handles.IsValidIndex(Index))) return false;
			const auto Read = Invoke(ReplayIds[4], Object({}, {}),
				{{TEXT("asset"), AssetGroup->Handles[0]}, {TEXT("property"), Properties->Handles[Index]}}, EAssetDocumentRuntimeRequestPhase::Verify);
			Test.TestEqual(TEXT("admitted projection read succeeds"), Read.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		}
		const auto MutableAsset = LiveHandles.Issue(EAssetDocumentHandleKind::Asset,
			EAssetDocumentHandleAccess::Mutable, MakeAssetDocumentEditorAssetPayload(*Asset));
		if (!Test.TestTrue(TEXT("test fixture issues mutable native asset"), MutableAsset.Handle.IsSet())) return false;
		if (Mode == EReplayMode::ProviderBatch)
		{
			// Negative evidence for the old plan's omission -> null projection.
			// The existing name codec rejects null; this does NOT close that consumer.
			FAssetDocumentRuntimeValueDto NullRows;
			NullRows.Kind = EAssetDocumentRuntimeValueKind::Array;
			NullRows.Elements.Add(Object({TEXT("name"), TEXT("value")},
				{String(TEXT("RetargetSource")), FAssetDocumentRuntimeValueDto()}));
			++BatchInvocationCount;
			const auto RejectedNull = Invoke(ReplayIds[5],
				Object({TEXT("properties")}, {MoveTemp(NullRows)}),
				{{TEXT("asset"), MutableAsset.Handle.GetValue()},
				 {TEXT("property_batch"), BatchGroup->Handles[0]}}, Phase);
			Test.TestEqual(TEXT("RetargetSource null projection fails closed"),
				RejectedNull.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
			Test.TestEqual(TEXT("null rejection has one unchanged primitive diagnostic"),
				RejectedNull.Diagnostics.Num(), 1);
			Test.TestTrue(TEXT("name null rejection retains PropertyWrite.DecodeFailed"),
				RejectedNull.Diagnostics.ContainsByPredicate([](const auto& Diagnostic)
				{
					return Diagnostic.Code == TEXT("PropertyWrite.DecodeFailed");
				}));
			Test.TestTrue(TEXT("null rejection publishes no changes"), RejectedNull.ChangedSet.IsEmpty());
			Test.TestTrue(TEXT("null rejection publishes no handles"), RejectedNull.HandleResults.IsEmpty());
			Test.TestEqual(TEXT("null rejection enlists no footprint"),
				Transaction->GetState(), EAssetDocumentAtomicState::Live);
			Test.TestEqual(TEXT("null rejection preserves name"), Asset->RetargetSource, RetargetBefore);
			Test.TestEqual(TEXT("null rejection preserves scalar"), Asset->RateScale, RateBefore);
			Test.TestEqual(TEXT("null rejection preserves enum"),
				static_cast<EAdditiveAnimationType>(Asset->AdditiveAnimType), AdditiveBefore);
			Test.TestEqual(TEXT("null rejection preserves package dirty state"), Package->IsDirty(), bDirtyBefore);
		}
		++BatchInvocationCount;
		const auto Written = Invoke(ReplayIds[5], Batch(Mode == EReplayMode::EmptyBatch),
			{{TEXT("asset"), MutableAsset.Handle.GetValue()}, {TEXT("property_batch"), BatchGroup->Handles[0]}}, Phase);
		if (!Test.TestEqual(TEXT("existing batch handler succeeds"), Written.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
		{
			for (const auto& Diagnostic : Written.Diagnostics) Test.AddError(Diagnostic.Code);
			return false;
		}
		Test.TestTrue(TEXT("batch publishes no handles"), Written.HandleResults.IsEmpty());
		if (Mode == EReplayMode::EmptyBatch)
		{
			Test.TestTrue(TEXT("empty batch publishes no changes"), Written.ChangedSet.IsEmpty());
			Test.TestEqual(TEXT("empty batch does not enlist"), Transaction->GetState(), EAssetDocumentAtomicState::Live);
		}
		else
		{
			Test.TestEqual(TEXT("real scalar write"), Asset->RateScale, 1.75f);
			Test.TestEqual(TEXT("real name write"), Asset->RetargetSource, FName(TEXT("ReplayRetarget")));
			Test.TestEqual(TEXT("real enum write"), static_cast<EAdditiveAnimationType>(Asset->AdditiveAnimType), AAT_LocalSpaceBase);
			Test.TestEqual(TEXT("omitted scalar preserves"), Asset->RefFrameIndex, FrameBefore);
			Test.TestFalse(TEXT("nonempty batch returns changes"), Written.ChangedSet.IsEmpty());
		}
	}
	Test.TestEqual(TEXT("CoreInitial invokes zero batches; ProviderBatch has one rejection probe and one successful batch"),
		BatchInvocationCount, Mode == EReplayMode::CoreInitial ? 0 : (Mode == EReplayMode::ProviderBatch ? 2 : 1));
	const auto Ready = Transaction->MarkReadyToCommit();
	Test.TestEqual(TEXT("same real complete-footprint gate accepts both projections, rejects empty-batch substitute"),
		Ready.bValid, Mode != EReplayMode::EmptyBatch);
	Captures.Reset();
	Test.TestTrue(TEXT("real Core rollback executes prepared typed restore"), Transaction->Rollback(TEXT("Replay.Requested")).bValid);
	Test.TestEqual(TEXT("rollback terminal is real"), Transaction->GetState(), EAssetDocumentAtomicState::RolledBack);
	Test.TestEqual(TEXT("rollback restores scalar"), Asset->RateScale, RateBefore);
	Test.TestEqual(TEXT("rollback restores name"), Asset->RetargetSource, RetargetBefore);
	Test.TestEqual(TEXT("rollback restores enum"), static_cast<EAdditiveAnimationType>(Asset->AdditiveAnimType), AdditiveBefore);
	Test.TestEqual(TEXT("rollback restores package dirty state"), Package->IsDirty(), bDirtyBefore);
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimSequencePropertyConsumerReplayTest,
	"UEAssetDoc.GenericProvider.AnimSequence.PropertyConsumerReplay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimSequencePropertyConsumerReplayTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorAnimSequenceConsumerReplayTests;
	const auto Provider = CreateAssetDocumentEditorPrimitiveProvider();
	int32 Admitted = 0;
	for (const auto& Descriptor : Provider->DescribePrimitives())
	{
		bool bReplayId = false;
		for (const TCHAR* Id : ReplayIds) bReplayId |= Descriptor.PrimitiveId == Id;
		const TSet<FString> LifecycleConsumers={TEXT("editor.asset.resolve_existing.v1"),TEXT("editor.property.enumerate_admitted.v1"),TEXT("editor.scratch.clone_asset.v2"),TEXT("editor.scratch.create_by_factory.v1"),TEXT("editor.asset.create_by_factory.v2"),TEXT("editor.asset.rollback_created.v1")};
  TestEqual(TEXT("animation consumers equal original six plus resolve and the generic lifecycle"),
   Descriptor.RequiredConsumerIds.Contains(TEXT("animation_profile")), bReplayId || LifecycleConsumers.Contains(Descriptor.PrimitiveId));
		if (bReplayId)
		{
			++Admitted;
			TestTrue(TEXT("existing generic consumer is retained"), Descriptor.RequiredConsumerIds.Contains(TEXT("route.generic.v1")));
		}
	}
	TestEqual(TEXT("all six replay descriptors exist"), Admitted, 6);
	bool bSucceeded = true;
	for (const auto Phase : {EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair})
	{
		bSucceeded &= Replay(*this, *Provider, EReplayMode::ProviderBatch, Phase);
		bSucceeded &= Replay(*this, *Provider, EReplayMode::CoreInitial, Phase);
		bSucceeded &= Replay(*this, *Provider, EReplayMode::EmptyBatch, Phase);
	}
	return bSucceeded;
}
#endif
