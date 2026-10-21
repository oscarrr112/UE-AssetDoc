// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentUMGPrimitiveProvider.h"
#include "AssetDocumentUMGProviderTestSupport.h"
#include "AssetDocumentRollbackAction.h"
#include "Fixtures/AssetDocumentUMGProviderTestFixture.h"
#include "Misc/AutomationTest.h"
#include "Provider/AssetDocumentUMGPrimitiveCatalog.h"
#include "Provider/AssetDocumentUMGPrimitiveHandler.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentUMGProviderContractTests
{
struct FExpectedDecision
{
	const TCHAR* DecisionId;
	EAssetDocumentUMGDecisionDisposition Disposition;
	const TCHAR* ConsumerId;
	const TCHAR* StableBlockerDiagnostic;
};

struct FExpectedDescriptor
{
	const TCHAR* PrimitiveId;
	const TCHAR* InputSchemaId;
	const TCHAR* OutputSchemaId;
	EAssetDocumentPrimitiveAccess Access;
	TArray<EAssetDocumentRuntimeRequestPhase> AllowedPhases;
	TArray<FString> StableDiagnostics;
};

const TArray<FString> CommonDiagnostics = {
	TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"),
	TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
	TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
	TEXT("AD.UMG.PROVIDER.AUTHORITY_UNAVAILABLE"),
	TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"),
	TEXT("AD.UMG.PROVIDER.ROLLBACK_FAILED"),
};

class FProviderTestRollbackAction final
	: public IAssetDocumentRollbackAction
{
public:
	virtual FStringView GetRollbackComponentId() const override
	{
		return TEXTVIEW("rollback.test-umg-provider");
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView) override
	{
		FAssetDocumentContractValidationResult Result;
		Result.bValid = true;
		return Result;
	}
};

void AssertTransactionRollbackActionEnlistment(FAutomationTestBase& Test)
{
	FAssetDocumentUMGProviderTestContext Context;
	FAssetDocumentUMGProviderTestTransaction Transaction(Context);
	FAssetDocumentContractValidationResult Configured;
	Configured.bValid = false;
	FAssetDocumentRuntimeDiagnostic& ExpectedDiagnostic =
		Configured.Diagnostics.AddDefaulted_GetRef();
	ExpectedDiagnostic.Code = TEXT("Test.UMGProvider.RollbackActionRejected");
	Transaction.SetRollbackActionEnlistmentResult(MoveTemp(Configured));

	const FAssetDocumentContractValidationResult Actual =
		Transaction.EnlistRollbackAction(
			MakeShared<FProviderTestRollbackAction>());
	Test.TestFalse(
		TEXT("UMG provider fake returns configured action-enlistment result"),
		Actual.bValid);
	Test.TestEqual(
		TEXT("UMG provider fake preserves configured diagnostic"),
		Actual.Diagnostics.Num() == 1 ? Actual.Diagnostics[0].Code : FString(),
		FString(TEXT("Test.UMGProvider.RollbackActionRejected")));
	Test.TestEqual(
		TEXT("UMG provider fake records rollback component ID"),
		Transaction.GetEnlistedRollbackActionComponentIds(),
		TArray<FString>({TEXT("rollback.test-umg-provider")}));
}

TArray<FString> MakeDiagnostics(std::initializer_list<const TCHAR*> DomainDiagnostics)
{
	TArray<FString> Result = CommonDiagnostics;
	for (const TCHAR* DomainDiagnostic : DomainDiagnostics)
	{
		Result.Add(DomainDiagnostic);
	}
	return Result;
}

const FExpectedDecision ExpectedProjection[] = {
	{TEXT("umg.widget_tree.query.v1"), EAssetDocumentUMGDecisionDisposition::Ordinary, TEXT("umg_profile"), TEXT("")},
	{TEXT("umg.widget_tree.add_source_widget.v1"), EAssetDocumentUMGDecisionDisposition::Ordinary, TEXT("umg_profile"), TEXT("")},
	{TEXT("umg.widget_tree.delete_widget.v1"), EAssetDocumentUMGDecisionDisposition::Ordinary, TEXT("umg_profile"), TEXT("")},
	{TEXT("umg.widget_tree.panel_child.attach.v1"), EAssetDocumentUMGDecisionDisposition::Ordinary, TEXT("umg_profile"), TEXT("")},
	{TEXT("umg.widget_tree.content.set.v1"), EAssetDocumentUMGDecisionDisposition::Ordinary, TEXT("umg_profile"), TEXT("")},
	{TEXT("umg.widget_tree.named_slot.set.v1"), EAssetDocumentUMGDecisionDisposition::Ordinary, TEXT("umg_profile"), TEXT("")},
	{TEXT("umg.widget_tree.replace.v1"), EAssetDocumentUMGDecisionDisposition::StableBlocker, TEXT("umg_provider_design"), TEXT("AD.G1.BLOCK.UMG.WIDGET_REPLACE_HEADLESS_AUTHORITY")},
	{TEXT("umg.designer_binding.query.v1"), EAssetDocumentUMGDecisionDisposition::Ordinary, TEXT("umg_profile"), TEXT("")},
	{TEXT("UMG-DESIGNER-BINDING-APPLY-DELTA-v1"), EAssetDocumentUMGDecisionDisposition::StableBlocker, TEXT("umg_provider_design"), TEXT("AD.G1.BLOCK.UMG.DESIGNER_BINDING_EVIDENCE")},
	{TEXT("umg.variable.notify_lifecycle.v1"), EAssetDocumentUMGDecisionDisposition::Ordinary, TEXT("umg_profile"), TEXT("")},
	{TEXT("UMG.VAR.FORCE_GUID_V1"), EAssetDocumentUMGDecisionDisposition::StableBlocker, TEXT("umg_provider_design"), TEXT("AD.G1.BLOCK.UMG.CALLER_SELECTED_GUID_UNSUPPORTED")},
	{TEXT("umg.animation.query.v1"), EAssetDocumentUMGDecisionDisposition::Ordinary, TEXT("umg_profile"), TEXT("")},
	{TEXT("UMG-WIDGET-ANIMATION-DELETE-v1"), EAssetDocumentUMGDecisionDisposition::StableBlocker, TEXT("umg_provider_design"), TEXT("AD.G1.BLOCK.UMG.ANIMATION_DELETE_EVIDENCE")},
	{TEXT("umg.movie_scene.possessable.add.v1"), EAssetDocumentUMGDecisionDisposition::Ordinary, TEXT("umg_profile"), TEXT("")},
	{TEXT("umg.movie_scene.possessable.remove.v1"), EAssetDocumentUMGDecisionDisposition::Ordinary, TEXT("umg_profile"), TEXT("")},
	{TEXT("umg.animation.sequence.bind.v1"), EAssetDocumentUMGDecisionDisposition::Ordinary, TEXT("umg_profile"), TEXT("")},
	{TEXT("umg.animation.sequence.unbind.v1"), EAssetDocumentUMGDecisionDisposition::Ordinary, TEXT("umg_profile"), TEXT("")},
	{TEXT("umg.movie_scene.float_track.add.v1"), EAssetDocumentUMGDecisionDisposition::Ordinary, TEXT("umg_profile"), TEXT("")},
	{TEXT("umg.movie_scene.transform_track.add.v1"), EAssetDocumentUMGDecisionDisposition::Ordinary, TEXT("umg_profile"), TEXT("")},
	{TEXT("umg.movie_scene.track_section.remove.v1"), EAssetDocumentUMGDecisionDisposition::Ordinary, TEXT("umg_profile"), TEXT("")},
};

const FExpectedDescriptor ExpectedDescriptors[] = {
	{TEXT("umg.widget_tree.query.v1"), TEXT("asset_document.umg.widget_tree.query.input.v1"), TEXT("asset_document.umg.widget_tree.query.output.v1"), EAssetDocumentPrimitiveAccess::ReadOnly, {EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight, EAssetDocumentRuntimeRequestPhase::Verify}, MakeDiagnostics({TEXT("AD.UMG.WIDGET_TREE.INVALID_BLUEPRINT"), TEXT("AD.UMG.WIDGET_TREE.INVALID_TREE")})},
	{TEXT("umg.widget_tree.add_source_widget.v1"), TEXT("asset_document.umg.widget_tree.add_source_widget.input.v1"), TEXT("asset_document.umg.widget_tree.add_source_widget.output.v1"), EAssetDocumentPrimitiveAccess::Mutating, {EAssetDocumentRuntimeRequestPhase::Apply}, MakeDiagnostics({TEXT("AD.UMG.WIDGET_TREE.NAME_COLLISION"), TEXT("AD.UMG.WIDGET_TREE.INVALID_CLASS"), TEXT("AD.UMG.WIDGET_TREE.INVALID_PARENT"), TEXT("AD.UMG.WIDGET_TREE.INVALID_INDEX"), TEXT("AD.UMG.WIDGET_TREE.ADD_HELPER_FAILED")})},
	{TEXT("umg.widget_tree.delete_widget.v1"), TEXT("asset_document.umg.widget_tree.delete_widget.input.v1"), TEXT("asset_document.umg.widget_tree.delete_widget.output.v1"), EAssetDocumentPrimitiveAccess::Mutating, {EAssetDocumentRuntimeRequestPhase::Apply}, MakeDiagnostics({TEXT("AD.UMG.WIDGET_TREE.WIDGET_NOT_FOUND"), TEXT("AD.UMG.WIDGET_TREE.ROOT_MISMATCH"), TEXT("AD.UMG.WIDGET_TREE.DELETE_HELPER_FAILED")})},
	{TEXT("umg.widget_tree.panel_child.attach.v1"), TEXT("asset_document.umg.widget_tree.panel_child_attach.input.v1"), TEXT("asset_document.umg.widget_tree.panel_child_attach.output.v1"), EAssetDocumentPrimitiveAccess::Mutating, {EAssetDocumentRuntimeRequestPhase::Apply}, MakeDiagnostics({TEXT("AD.UMG.WIDGET_TREE.INVALID_PANEL"), TEXT("AD.UMG.WIDGET_TREE.CHILD_ALREADY_PARENTED"), TEXT("AD.UMG.WIDGET_TREE.INVALID_INDEX")})},
	{TEXT("umg.widget_tree.content.set.v1"), TEXT("asset_document.umg.widget_tree.content_set.input.v1"), TEXT("asset_document.umg.widget_tree.content_set.output.v1"), EAssetDocumentPrimitiveAccess::Mutating, {EAssetDocumentRuntimeRequestPhase::Apply}, MakeDiagnostics({TEXT("AD.UMG.WIDGET_TREE.INVALID_CONTENT_WIDGET"), TEXT("AD.UMG.WIDGET_TREE.SELF_CYCLE"), TEXT("AD.UMG.WIDGET_TREE.CHILD_ALREADY_PARENTED")})},
	{TEXT("umg.widget_tree.named_slot.set.v1"), TEXT("asset_document.umg.widget_tree.named_slot_set.input.v1"), TEXT("asset_document.umg.widget_tree.named_slot_set.output.v1"), EAssetDocumentPrimitiveAccess::Mutating, {EAssetDocumentRuntimeRequestPhase::Apply}, MakeDiagnostics({TEXT("AD.UMG.WIDGET_TREE.UNKNOWN_NAMED_SLOT"), TEXT("AD.UMG.WIDGET_TREE.CHILD_ALREADY_PARENTED")})},
	{TEXT("umg.designer_binding.query.v1"), TEXT("asset_document.umg.designer_binding.query.input.v1"), TEXT("asset_document.umg.designer_binding.query.output.v1"), EAssetDocumentPrimitiveAccess::ReadOnly, {EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight, EAssetDocumentRuntimeRequestPhase::Verify}, MakeDiagnostics({TEXT("AD.UMG.DESIGNER_BINDING.DUPLICATE_IDENTITY"), TEXT("AD.UMG.DESIGNER_BINDING.EVIDENCE_UNAVAILABLE")})},
	{TEXT("umg.variable.notify_lifecycle.v1"), TEXT("asset_document.umg.variable.notify_lifecycle.input.v1"), TEXT("asset_document.umg.variable.notify_lifecycle.output.v1"), EAssetDocumentPrimitiveAccess::Mutating, {EAssetDocumentRuntimeRequestPhase::Apply}, MakeDiagnostics({TEXT("AD.UMG.VARIABLE.NAME_COLLISION"), TEXT("AD.UMG.VARIABLE.NOT_FOUND"), TEXT("AD.UMG.VARIABLE.OBSERVED_GUID_MISMATCH")})},
	{TEXT("umg.animation.query.v1"), TEXT("asset_document.umg.animation.query.input.v1"), TEXT("asset_document.umg.animation.query.output.v1"), EAssetDocumentPrimitiveAccess::ReadOnly, {EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight, EAssetDocumentRuntimeRequestPhase::Verify}, MakeDiagnostics({TEXT("AD.UMG.ANIMATION.NOT_FOUND"), TEXT("AD.UMG.ANIMATION.EVIDENCE_UNAVAILABLE")})},
	{TEXT("umg.movie_scene.possessable.add.v1"), TEXT("asset_document.umg.movie_scene.possessable_add.input.v1"), TEXT("asset_document.umg.movie_scene.possessable_add.output.v1"), EAssetDocumentPrimitiveAccess::Mutating, {EAssetDocumentRuntimeRequestPhase::Apply}, MakeDiagnostics({TEXT("AD.UMG.MOVIE_SCENE.POSSESSABLE_IDENTITY_COLLISION"), TEXT("AD.UMG.MOVIE_SCENE.INVALID_CLASS"), TEXT("AD.UMG.MOVIE_SCENE.INVALID_RETURNED_GUID")})},
	{TEXT("umg.movie_scene.possessable.remove.v1"), TEXT("asset_document.umg.movie_scene.possessable_remove.input.v1"), TEXT("asset_document.umg.movie_scene.possessable_remove.output.v1"), EAssetDocumentPrimitiveAccess::Mutating, {EAssetDocumentRuntimeRequestPhase::Apply}, MakeDiagnostics({TEXT("AD.UMG.MOVIE_SCENE.POSSESSABLE_NOT_FOUND"), TEXT("AD.UMG.MOVIE_SCENE.BINDING_STILL_PRESENT")})},
	{TEXT("umg.animation.sequence.bind.v1"), TEXT("asset_document.umg.animation.sequence_bind.input.v1"), TEXT("asset_document.umg.animation.sequence_bind.output.v1"), EAssetDocumentPrimitiveAccess::Mutating, {EAssetDocumentRuntimeRequestPhase::Apply}, MakeDiagnostics({TEXT("AD.UMG.ANIMATION_BINDING.GUID_MISMATCH"), TEXT("AD.UMG.ANIMATION_BINDING.PREVIEW_CONTEXT_MISMATCH"), TEXT("AD.UMG.ANIMATION_BINDING.DUPLICATE_BINDING")})},
	{TEXT("umg.animation.sequence.unbind.v1"), TEXT("asset_document.umg.animation.sequence_unbind.input.v1"), TEXT("asset_document.umg.animation.sequence_unbind.output.v1"), EAssetDocumentPrimitiveAccess::Mutating, {EAssetDocumentRuntimeRequestPhase::Apply}, MakeDiagnostics({TEXT("AD.UMG.ANIMATION_BINDING.BINDING_NOT_FOUND"), TEXT("AD.UMG.ANIMATION_BINDING.COMPANION_REMAINS")})},
	{TEXT("umg.movie_scene.float_track.add.v1"), TEXT("asset_document.umg.movie_scene.float_track_add.input.v1"), TEXT("asset_document.umg.movie_scene.float_track_add.output.v1"), EAssetDocumentPrimitiveAccess::Mutating, {EAssetDocumentRuntimeRequestPhase::Apply}, MakeDiagnostics({TEXT("AD.UMG.MOVIE_SCENE.TRACK_IDENTITY_COLLISION"), TEXT("AD.UMG.MOVIE_SCENE.INVALID_RANGE"), TEXT("AD.UMG.MOVIE_SCENE.DUPLICATE_KEY_FRAME"), TEXT("AD.UMG.MOVIE_SCENE.UNSUPPORTED_INTERPOLATION")})},
	{TEXT("umg.movie_scene.transform_track.add.v1"), TEXT("asset_document.umg.movie_scene.transform_track_add.input.v1"), TEXT("asset_document.umg.movie_scene.transform_track_add.output.v1"), EAssetDocumentPrimitiveAccess::Mutating, {EAssetDocumentRuntimeRequestPhase::Apply}, MakeDiagnostics({TEXT("AD.UMG.MOVIE_SCENE.TRACK_IDENTITY_COLLISION"), TEXT("AD.UMG.MOVIE_SCENE.INVALID_RANGE"), TEXT("AD.UMG.MOVIE_SCENE.DUPLICATE_KEY_FRAME"), TEXT("AD.UMG.MOVIE_SCENE.UNSUPPORTED_INTERPOLATION"), TEXT("AD.UMG.MOVIE_SCENE.UNSUPPORTED_CHANNEL")})},
	{TEXT("umg.movie_scene.track_section.remove.v1"), TEXT("asset_document.umg.movie_scene.track_section_remove.input.v1"), TEXT("asset_document.umg.movie_scene.track_section_remove.output.v1"), EAssetDocumentPrimitiveAccess::Mutating, {EAssetDocumentRuntimeRequestPhase::Apply}, MakeDiagnostics({TEXT("AD.UMG.MOVIE_SCENE.OWNERSHIP_MISMATCH"), TEXT("AD.UMG.MOVIE_SCENE.TARGET_NOT_FOUND")})},
};

const FAssetDocumentPrimitiveDescriptor* FindDescriptor(
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors,
	const TCHAR* PrimitiveId)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
	{
		if (Descriptor.PrimitiveId == PrimitiveId)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

template <typename TBindings>
int32 CountCallableBindings(const TBindings& Bindings, const TCHAR* PrimitiveId)
{
	int32 Count = 0;
	for (const auto& Binding : Bindings)
	{
		if (Binding.PrimitiveId == PrimitiveId && Binding.Version == 1 && Binding.Handler != nullptr)
		{
			++Count;
		}
	}
	return Count;
}

void AssertDescriptor(
	FAutomationTestBase& Test,
	const FExpectedDescriptor& Expected,
	const FAssetDocumentPrimitiveDescriptor& Actual)
{
	Test.TestTrue(*FString::Printf(TEXT("%s validates"), Expected.PrimitiveId),
		ValidateAssetDocumentPrimitiveDescriptor(Actual).bValid);
	Test.TestEqual(*FString::Printf(TEXT("%s version"), Expected.PrimitiveId), Actual.Version, 1u);
	Test.TestEqual(*FString::Printf(TEXT("%s provider"), Expected.PrimitiveId), Actual.ProviderId, FString(TEXT("umg")));
	Test.TestEqual(*FString::Printf(TEXT("%s owner"), Expected.PrimitiveId), Actual.Owner, EAssetDocumentPrimitiveOwner::UMG);
	Test.TestEqual(*FString::Printf(TEXT("%s thread"), Expected.PrimitiveId), Actual.ExecutionThread, EAssetDocumentExecutionThread::GameThread);
	Test.TestEqual(*FString::Printf(TEXT("%s access"), Expected.PrimitiveId), Actual.Access, Expected.Access);
	Test.TestEqual(*FString::Printf(TEXT("%s phases"), Expected.PrimitiveId), Actual.AllowedPhases, Expected.AllowedPhases);
	Test.TestEqual(*FString::Printf(TEXT("%s input schema"), Expected.PrimitiveId), Actual.InputSchemaId, FString(Expected.InputSchemaId));
	Test.TestEqual(*FString::Printf(TEXT("%s output schema"), Expected.PrimitiveId), Actual.OutputSchemaId, FString(Expected.OutputSchemaId));
	Test.TestEqual(*FString::Printf(TEXT("%s consumer"), Expected.PrimitiveId), Actual.RequiredConsumerIds, TArray<FString>({TEXT("umg_profile")}));
	Test.TestEqual(*FString::Printf(TEXT("%s diagnostics"), Expected.PrimitiveId), Actual.StableDiagnosticCodes, Expected.StableDiagnostics);
	Test.TestEqual(*FString::Printf(TEXT("%s primary operation"), Expected.PrimitiveId), Actual.Admission.PrimaryOperationId, FString(Expected.PrimitiveId));
	Test.TestEqual(*FString::Printf(TEXT("%s physical owner"), Expected.PrimitiveId), Actual.Admission.PhysicalOwner, EAssetDocumentPrimitiveOwner::UMG);
	Test.TestEqual(*FString::Printf(TEXT("%s engine version"), Expected.PrimitiveId), Actual.Admission.ExactEngineVersion, FString(TEXT("5.7.3")));
	Test.TestEqual(*FString::Printf(TEXT("%s disposition"), Expected.PrimitiveId), Actual.Admission.Disposition, EAssetDocumentPrimitiveDisposition::OrdinaryHelper);
	Test.TestEqual(*FString::Printf(TEXT("%s authority context"), Expected.PrimitiveId), Actual.Admission.AuthorityContextId, FString(TEXT("umg.editor-game-thread.headless.v1")));
	Test.TestEqual(*FString::Printf(TEXT("%s transaction contract"), Expected.PrimitiveId), Actual.Admission.TransactionContractId, FString(TEXT("atomic.active-mutation-token.v1")));
	Test.TestEqual(*FString::Printf(TEXT("%s rollback contract"), Expected.PrimitiveId), Actual.Admission.RollbackContractId, FString(TEXT("rollback.atomic-owner-governed.v1")));
	Test.TestEqual(*FString::Printf(TEXT("%s completion contract"), Expected.PrimitiveId), Actual.Admission.CompletionContractId, FString(TEXT("umg.direct-provider-local-readback.v1")));
	if (Expected.Access == EAssetDocumentPrimitiveAccess::ReadOnly)
	{
		Test.TestFalse(*FString::Printf(TEXT("%s no transaction"), Expected.PrimitiveId), Actual.bRequiresActiveTransaction);
		Test.TestTrue(*FString::Printf(TEXT("%s no footprint"), Expected.PrimitiveId), Actual.MutationFootprintSchemaId.IsEmpty());
	}
	else
	{
		Test.TestTrue(*FString::Printf(TEXT("%s requires transaction"), Expected.PrimitiveId), Actual.bRequiresActiveTransaction);
		Test.TestEqual(*FString::Printf(TEXT("%s footprint schema"), Expected.PrimitiveId), Actual.MutationFootprintSchemaId, Actual.InputSchemaId.Replace(TEXT(".input.v1"), TEXT(".footprint.v1")));
	}
}

void AssertBlockedOutcome(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	const FExpectedDecision& Expected)
{
	FAssetDocumentUMGProviderTestContext Context;
	FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
	FAssetDocumentUMGProviderTestTransaction Transaction(Context);
	FAssetDocumentUMGProviderTestFixture Fixture;
	Test.TestTrue(TEXT("blocked fixture is created"), Fixture.CreateTransientWidgetBlueprint());
	const FString DigestBefore = Fixture.CaptureDigest();

	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = Expected.DecisionId;
	Invocation.Version = 1;
	Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Failed;
	const FAssetDocumentPrimitiveOutcome Outcome = Provider.Invoke(Invocation, Handles, &Transaction);

	Test.TestEqual(*FString::Printf(TEXT("%s fails"), Expected.DecisionId), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestEqual(*FString::Printf(TEXT("%s returns one frozen diagnostic"), Expected.DecisionId), Outcome.Diagnostics.Num(), 1);
	if (!Outcome.Diagnostics.IsEmpty())
	{
		Test.TestEqual(*FString::Printf(TEXT("%s frozen diagnostic"), Expected.DecisionId), Outcome.Diagnostics[0].Code, FString(Expected.StableBlockerDiagnostic));
	}
	Test.TestEqual(*FString::Printf(TEXT("%s precedes descriptor validation"), Expected.DecisionId), Handles.GetTotalCallCount(), 0);
	Test.TestEqual(*FString::Printf(TEXT("%s does not touch a transaction"), Expected.DecisionId), Transaction.GetTotalCallCount(), 0);
	Test.TestTrue(*FString::Printf(TEXT("%s has no changed set"), Expected.DecisionId), Outcome.ChangedSet.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("%s has no handles"), Expected.DecisionId), Outcome.HandleResults.IsEmpty());
	Test.TestEqual(*FString::Printf(TEXT("%s leaves fixture unchanged"), Expected.DecisionId), Fixture.CaptureDigest(), DigestBefore);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUMGProviderContractProjectionTest,
	"UEAssetDoc.UMGProvider.ContractProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUMGProviderContractProjectionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUMGProviderContractTests;
	AssertTransactionRollbackActionEnlistment(*this);

	const auto Projection = BuildAssetDocumentUMGDecisionProjection();
	TestEqual(TEXT("projection has twenty rows"), Projection.Num(),
		static_cast<int32>(UE_ARRAY_COUNT(ExpectedProjection)));
	int32 OrdinaryCount = 0;
	int32 BlockerCount = 0;
	for (int32 Index = 0; Index < Projection.Num() && Index < UE_ARRAY_COUNT(ExpectedProjection); ++Index)
	{
		const auto& Actual = Projection[Index];
		const FExpectedDecision& Expected = ExpectedProjection[Index];
		TestEqual(TEXT("projection decision is ordered"), Actual.DecisionId, FString(Expected.DecisionId));
		TestEqual(TEXT("projection version is one"), Actual.Version, 1u);
		TestEqual(TEXT("projection owner is UMG"), Actual.Owner, EAssetDocumentPrimitiveOwner::UMG);
		TestEqual(TEXT("projection disposition is frozen"), Actual.Disposition, Expected.Disposition);
		TestEqual(TEXT("projection has its only consumer"), Actual.ConsumerIds, TArray<FString>({Expected.ConsumerId}));
		TestEqual(TEXT("projection blocker diagnostic is exact"), Actual.StableBlockerDiagnostic, FString(Expected.StableBlockerDiagnostic));
		if (Actual.Disposition == EAssetDocumentUMGDecisionDisposition::Ordinary)
		{
			++OrdinaryCount;
		}
		else if (Actual.Disposition == EAssetDocumentUMGDecisionDisposition::StableBlocker)
		{
			++BlockerCount;
		}
	}
	TestEqual(TEXT("projection has sixteen ordinary rows"), OrdinaryCount, 16);
	TestEqual(TEXT("projection has four blocked rows"), BlockerCount, 4);

	const auto Catalog = BuildAssetDocumentUMGPrimitiveCatalog();
	const auto HandlerTable = BuildAssetDocumentUMGHandlerTable();
	TestEqual(TEXT("catalog has sixteen ordinary descriptors"), Catalog.Num(),
		static_cast<int32>(UE_ARRAY_COUNT(ExpectedDescriptors)));
	TestEqual(TEXT("handler table has sixteen ordinary bindings"), HandlerTable.Num(),
		static_cast<int32>(UE_ARRAY_COUNT(ExpectedDescriptors)));
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(ExpectedDescriptors); ++Index)
	{
		const FExpectedDescriptor& Expected = ExpectedDescriptors[Index];
		const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(Catalog, Expected.PrimitiveId);
		TestNotNull(*FString::Printf(TEXT("%s descriptor exists"), Expected.PrimitiveId), Descriptor);
		if (Descriptor != nullptr)
		{
			AssertDescriptor(*this, Expected, *Descriptor);
		}
		TestEqual(*FString::Printf(TEXT("%s has exactly one callable handler"), Expected.PrimitiveId), CountCallableBindings(HandlerTable, Expected.PrimitiveId), 1);
	}
	for (const FExpectedDecision& Expected : ExpectedProjection)
	{
		if (Expected.Disposition == EAssetDocumentUMGDecisionDisposition::StableBlocker)
		{
			TestNull(*FString::Printf(TEXT("%s blocker is absent from catalog"), Expected.DecisionId), FindDescriptor(Catalog, Expected.DecisionId));
			TestEqual(*FString::Printf(TEXT("%s blocker has no handler"), Expected.DecisionId), CountCallableBindings(HandlerTable, Expected.DecisionId), 0);
		}
	}

	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider = CreateAssetDocumentUMGPrimitiveProvider();
	TestEqual(TEXT("factory provider id"), Provider->GetProviderId(), FString(TEXT("umg")));
	TestEqual(TEXT("factory describes exactly the derived catalog"), Provider->DescribePrimitives().Num(), Catalog.Num());
	for (int32 Index = 0; Index < Catalog.Num(); ++Index)
	{
		TestEqual(TEXT("factory catalog preserves derived order"), Provider->DescribePrimitives()[Index].PrimitiveId, Catalog[Index].PrimitiveId);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUMGProviderBlockedDecisionsTest,
	"UEAssetDoc.UMGProvider.RejectsBlockedDecisions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUMGProviderBlockedDecisionsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUMGProviderContractTests;

	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider = CreateAssetDocumentUMGPrimitiveProvider();
	for (const FExpectedDecision& Expected : ExpectedProjection)
	{
		if (Expected.Disposition == EAssetDocumentUMGDecisionDisposition::StableBlocker)
		{
			AssertBlockedOutcome(*this, Provider.Get(), Expected);
		}
	}
	return true;
}
#endif
