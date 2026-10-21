// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentBlackboardAtomicTransactionAdapter.h"

#include "AssetDocumentRollbackAction.h"
#include "Misc/AutomationTest.h"

namespace AssetDocumentBlackboardAtomicTransactionAdapterTests
{
FAssetDocumentContractValidationResult Valid()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

FAssetDocumentMutationFootprint Row(FString StableId, FString ComponentId)
{
	FAssetDocumentMutationFootprint Result;
	Result.Kind = EAssetDocumentMutationFootprintKind::UObjectState;
	Result.StableId = MoveTemp(StableId);
	Result.RollbackComponentId = MoveTemp(ComponentId);
	return Result;
}

FAssetDocumentBlackboardFootprintProjectionStep Step(
	int32 Index,
	FString PrimitiveId,
	TArray<FAssetDocumentMutationFootprint> RawRows)
{
	FAssetDocumentBlackboardFootprintProjectionStep Result;
	Result.Index = Index;
	Result.PrimitiveId = MoveTemp(PrimitiveId);
	Result.RawFootprint = MoveTemp(RawRows);
	for (int32 RowIndex = 0; RowIndex < Result.RawFootprint.Num(); ++RowIndex)
	{
		FAssetDocumentMutationFootprint Qualified = Result.RawFootprint[RowIndex];
		Qualified.StableId = FString::Printf(TEXT("qualified:stable:%d:%d:%s"), Index, RowIndex, *Qualified.StableId);
		Qualified.RollbackComponentId = FString::Printf(TEXT("qualified:component:%d:%d:%s"), Index, RowIndex, *Qualified.RollbackComponentId);
		Result.QualifiedFootprint.Add(Qualified);
	}
	return Result;
}

FAssetDocumentBlackboardFootprintProjection MakeProjection()
{
	const FString Target = TEXT("/Game/AI/BB_Target.BB_Target");
	const FString Keys = TEXT("asset:/Game/AI/BB_Target.BB_Target/Keys[*]");
	FAssetDocumentBlackboardFootprintProjection Result;
	Result.RequestId = FGuid::NewGuid();
	Result.RuntimeRevision = 17;
	Result.ExactAssetClassPath = TEXT("/Script/AIModule.BlackboardData");
	Result.TargetAssetPath = Target;
	Result.Operation = TEXT("Apply");
	Result.SemanticHash = TEXT("sha256:adapter");
	Result.Steps.Add(Step(0, TEXT("ai.blackboard.parent.set.v2"), {
		Row(Keys, TEXT("assetdocument.ai.blackboard.parent.derived_closure.v2")),
		Row(TEXT("asset:/Game/AI/BB_Target.BB_Target/Parent"), TEXT("assetdocument.ai.blackboard.parent.reference.v2")),
	}));
	Result.Steps.Add(Step(1, TEXT("ai.blackboard.entry.update_existing.v2"), {
		Row(TEXT("asset:/Game/AI/BB_Target.BB_Target/Keys[name=Health].LocalKeysMetadataAggregate"), TEXT("assetdocument.ai.blackboard.entry_metadata.v2")),
		Row(TEXT("asset:/Game/AI/BB_Target.BB_Target/Keys[name=Health].KeyTypeProperties"), TEXT("assetdocument.ai.blackboard.key_type_properties.v2")),
	}));
	Result.Steps.Add(Step(2, TEXT("ai.blackboard.entry.replace_class.v2"), {Row(Keys, TEXT("assetdocument.ai.blackboard.entry.replace_class.v2"))}));
	Result.Steps.Add(Step(3, TEXT("ai.blackboard.entry.add.v2"), {Row(Keys, TEXT("assetdocument.ai.blackboard.entry.add.v2"))}));
	Result.Steps.Add(Step(4, TEXT("ai.blackboard.entry.remove.v2"), {Row(Keys, TEXT("assetdocument.ai.blackboard.entry_remove.v2"))}));
	Result.Steps.Add(Step(5, TEXT("ai.blackboard.local_keys.reorder.v2"), {Row(Keys, TEXT("assetdocument.ai.blackboard.local_keys_reorder.v2"))}));
	for (const FAssetDocumentBlackboardFootprintProjectionStep& ProjectedStep : Result.Steps)
	{
		Result.QualifiedFootprint.Append(ProjectedStep.QualifiedFootprint);
	}
	return Result;
}

class FRecordingRollbackAction final : public IAssetDocumentRollbackAction
{
public:
	FRecordingRollbackAction(FString InComponentId, TArray<FString>& InTrace)
		: ComponentId(MoveTemp(InComponentId)), Trace(InTrace) {}
	virtual FStringView GetRollbackComponentId() const override { return ComponentId; }
	virtual FAssetDocumentContractValidationResult Rollback(FStringView) override
	{
		Trace.Add(TEXT("action:") + ComponentId);
		return Valid();
	}
private:
	FString ComponentId;
	TArray<FString>& Trace;
};

class FRecordingFinalizer final : public IAssetDocumentPostRollbackFinalizer
{
public:
	FRecordingFinalizer(FString InComponentId, TArray<FString>& InTrace)
		: ComponentId(MoveTemp(InComponentId)), Trace(InTrace) {}
	virtual FStringView GetRollbackComponentId() const override { return ComponentId; }
	virtual FAssetDocumentContractValidationResult FinalizePostRollback(FStringView) override
	{
		Trace.Add(TEXT("finalizer:") + ComponentId);
		return Valid();
	}
private:
	FString ComponentId;
	TArray<FString>& Trace;
};

class FRecordingTransaction final : public IAssetDocumentAtomicTransaction
{
public:
	FRecordingTransaction()
	{
		Token.RequestId = FGuid::NewGuid();
		Token.TransactionId = FGuid::NewGuid();
		Token.RuntimeRevision = 17;
	}
	virtual FGuid GetTransactionId() const override { return Token.TransactionId; }
	virtual EAssetDocumentAtomicState GetState() const override { return State; }
	virtual const FAssetDocumentMutationToken& GetMutationToken() const override { return Token; }
	virtual FAssetDocumentContractValidationResult EnlistFootprint(TConstArrayView<FAssetDocumentMutationFootprint> Footprint) override
	{
		TArray<FAssetDocumentMutationFootprint> Batch;
		Batch.Append(Footprint.GetData(), Footprint.Num());
		FootprintBatches.Add(MoveTemp(Batch));
		State = EAssetDocumentAtomicState::Mutating;
		return Valid();
	}
	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override { ++MarkReadyCalls; State = EAssetDocumentAtomicState::ReadyToCommit; return Valid(); }
	virtual FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt&) override { State = EAssetDocumentAtomicState::Committed; return Valid(); }
	virtual FAssetDocumentContractValidationResult Rollback(FStringView StableFailureCode) override
	{
		for (int32 Index = Actions.Num() - 1; Index >= 0; --Index) Actions[Index]->Rollback(StableFailureCode);
		for (int32 Index = Finalizers.Num() - 1; Index >= 0; --Index) Finalizers[Index]->FinalizePostRollback(StableFailureCode);
		State = EAssetDocumentAtomicState::RolledBack;
		return Valid();
	}
	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(TSharedRef<IAssetDocumentRollbackAction> Action) override { ActionComponents.Add(FString(Action->GetRollbackComponentId())); Actions.Add(MoveTemp(Action)); return Valid(); }
	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(TSharedRef<IAssetDocumentPostRollbackFinalizer> Finalizer) override { FinalizerComponents.Add(FString(Finalizer->GetRollbackComponentId())); Finalizers.Add(MoveTemp(Finalizer)); return Valid(); }

	FAssetDocumentMutationToken Token;
	EAssetDocumentAtomicState State = EAssetDocumentAtomicState::Live;
	int32 MarkReadyCalls = 0;
	TArray<TArray<FAssetDocumentMutationFootprint>> FootprintBatches;
	TArray<FString> ActionComponents;
	TArray<FString> FinalizerComponents;
	TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions;
	TArray<TSharedRef<IAssetDocumentPostRollbackFinalizer>> Finalizers;
};
} // namespace AssetDocumentBlackboardAtomicTransactionAdapterTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentBlackboardAtomicTransactionAdapterTest,
	"UEAssetDoc.Python.BlackboardData.ProjectedAtomicEnlistment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBlackboardAtomicTransactionAdapterTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentBlackboardAtomicTransactionAdapterTests;

	FRecordingTransaction Underlying;
	FAssetDocumentBlackboardFootprintProjection Projection = MakeProjection();
	FAssetDocumentBlackboardAtomicTransactionAdapter Adapter(Underlying, Projection, {});
	TestFalse(TEXT("unconsumed steps cannot become ready"), Adapter.ValidateCompleteEnlistment().bValid);
	TArray<FString> RollbackTrace;
	TestTrue(TEXT("first rollback action maps its raw component"), Adapter.EnlistRollbackAction(MakeShared<FRecordingRollbackAction>(Projection.Steps[0].RawFootprint[0].RollbackComponentId, RollbackTrace)).bValid);
	TestTrue(TEXT("second parent finalizer maps its raw component"), Adapter.EnlistPostRollbackFinalizer(MakeShared<FRecordingFinalizer>(Projection.Steps[0].RawFootprint[1].RollbackComponentId, RollbackTrace)).bValid);
	for (const FAssetDocumentBlackboardFootprintProjectionStep& ProjectedStep : Projection.Steps)
	{
		TestTrue(*FString::Printf(TEXT("ordered raw step %d forwards"), ProjectedStep.Index), Adapter.EnlistFootprint(ProjectedStep.RawFootprint).bValid);
	}
	TestTrue(TEXT("all ordered projected rows are consumed"), Adapter.ValidateCompleteEnlistment().bValid);
	TestEqual(TEXT("underlying receives every projected batch"), Underlying.FootprintBatches.Num(), Projection.Steps.Num());
	TestEqual(TEXT("aliased structural raw rows become unique qualified rows"), Underlying.FootprintBatches[2][0].StableId, Projection.Steps[2].QualifiedFootprint[0].StableId);
	TestNotEqual(TEXT("replace and add aliased raw stable identities stay distinct underneath"), Underlying.FootprintBatches[2][0].StableId, Underlying.FootprintBatches[3][0].StableId);
	TestTrue(TEXT("adapter forwards readiness only after all rows"), Adapter.MarkReadyToCommit().bValid);
	TestTrue(TEXT("wrapped action delegates through reverse underlying rollback"), Adapter.Rollback(TEXT("test.rollback")).bValid);
	TestEqual(TEXT("wrapped rollback exposes qualified component"), Underlying.ActionComponents[0], Projection.Steps[0].QualifiedFootprint[0].RollbackComponentId);
	TestEqual(TEXT("wrapped finalizer exposes qualified component"), Underlying.FinalizerComponents[0], Projection.Steps[0].QualifiedFootprint[1].RollbackComponentId);
	TestEqual(TEXT("wrapped rollback delegates raw action"), RollbackTrace[0], FString(TEXT("action:assetdocument.ai.blackboard.parent.derived_closure.v2")));

	const FAssetDocumentBlackboardFootprintProjection DuplicateProjection = MakeProjection();
	FRecordingTransaction DuplicateExactUnderlying;
	FAssetDocumentBlackboardAtomicTransactionAdapter ExactDuplicateAdapter(DuplicateExactUnderlying, DuplicateProjection, {});
	TestTrue(TEXT("first exact parent batch succeeds"), ExactDuplicateAdapter.EnlistFootprint(DuplicateProjection.Steps[0].RawFootprint).bValid);
	TestFalse(TEXT("duplicate raw batch fails closed"), ExactDuplicateAdapter.EnlistFootprint(DuplicateProjection.Steps[0].RawFootprint).bValid);

	FAssetDocumentBlackboardFootprintProjection MismatchProjection = MakeProjection();
	FRecordingTransaction MismatchUnderlying;
	FAssetDocumentBlackboardAtomicTransactionAdapter MismatchAdapter(MismatchUnderlying, MismatchProjection, {});
	TestFalse(TEXT("wrong future structural batch fails before first parent batch"), MismatchAdapter.EnlistFootprint(MismatchProjection.Steps[2].RawFootprint).bValid);
	FRecordingTransaction ComponentMismatchUnderlying;
	FAssetDocumentBlackboardFootprintProjection ComponentMismatchProjection = MakeProjection();
	FAssetDocumentBlackboardAtomicTransactionAdapter ComponentMismatchAdapter(ComponentMismatchUnderlying, ComponentMismatchProjection, {});
	TestFalse(TEXT("wrong rollback component fails closed"), ComponentMismatchAdapter.EnlistRollbackAction(MakeShared<FRecordingRollbackAction>(TEXT("wrong.component"), RollbackTrace)).bValid);

	FAssetDocumentMutationFootprint Prefix = Row(TEXT("editor:create"), TEXT("editor.asset.rollback_created.v1"));
	FAssetDocumentBlackboardFootprintProjection PrefixProjection = MakeProjection();
	FRecordingTransaction PrefixUnderlying;
	FAssetDocumentBlackboardAtomicTransactionAdapter PrefixAdapter(PrefixUnderlying, PrefixProjection, {Prefix});
	TestFalse(TEXT("Blackboard rows cannot bypass exact Editor Create prefix"), PrefixAdapter.EnlistFootprint(PrefixProjection.Steps[0].RawFootprint).bValid);
	FRecordingTransaction PrefixExactUnderlying;
	FAssetDocumentBlackboardFootprintProjection PrefixExactProjection = MakeProjection();
	FAssetDocumentBlackboardAtomicTransactionAdapter PrefixExactAdapter(PrefixExactUnderlying, PrefixExactProjection, {Prefix});
	const TArray<FAssetDocumentMutationFootprint> PrefixBatch = {Prefix};
	TestTrue(TEXT("exact Editor Create prefix passes through unchanged"), PrefixExactAdapter.EnlistFootprint(PrefixBatch).bValid);
	TestEqual(TEXT("prefix stable id is not qualified"), PrefixExactUnderlying.FootprintBatches[0][0].StableId, Prefix.StableId);
	return true;
}
