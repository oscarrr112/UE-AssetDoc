// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentUMGPrimitiveProvider.h"
#include "AssetDocumentUMGProviderTestSupport.h"
#include "WidgetBlueprint.h"
#include "Fixtures/AssetDocumentUMGProviderTestFixture.h"
#include "Misc/AutomationTest.h"
#include "Provider/AssetDocumentUMGValueDto.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentUMGDesignerBindingProviderTests
{
const FAssetDocumentPrimitiveDescriptor& Descriptor(
	IAssetDocumentPrimitiveProvider& Provider,
	const TCHAR* PrimitiveId)
{
	for (const FAssetDocumentPrimitiveDescriptor& Item : Provider.DescribePrimitives())
	{
		if (Item.PrimitiveId == PrimitiveId)
		{
			return Item;
		}
	}
	checkNoEntry();
	return Provider.DescribePrimitives()[0];
}

void AddBlueprintHandle(
	FAssetDocumentPrimitiveInvocation& Invocation,
	FAssetDocumentUMGProviderTestHandleResolver& Handles,
	UWidgetBlueprint* Blueprint,
	EAssetDocumentHandleAccess Access)
{
	Invocation.HandleArguments.Add({TEXT("widget_blueprint"),
		Handles.AddObjectHandle(Blueprint, EAssetDocumentHandleKind::Asset, Access)});
}

FAssetDocumentPrimitiveOutcome Invoke(
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& DescriptorValue,
	const FAssetDocumentUMGProviderTestContext& Context,
	FAssetDocumentUMGProviderTestHandleResolver& Handles,
	FAssetDocumentUMGProviderTestTransaction& Transaction,
	UWidgetBlueprint* Blueprint,
	FAssetDocumentRuntimeValueDto&& Value)
{
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentUMGProviderTestInvocation(DescriptorValue, Context);
	Invocation.ValueArguments = MoveTemp(Value);
	AddBlueprintHandle(Invocation, Handles, Blueprint,
		DescriptorValue.Access == EAssetDocumentPrimitiveAccess::Mutating
			? EAssetDocumentHandleAccess::Mutable
			: EAssetDocumentHandleAccess::ReadOnly);
	return Provider.Invoke(Invocation, Handles, &Transaction);
}

bool HasCode(const FAssetDocumentPrimitiveOutcome& Outcome, const TCHAR* Code)
{
	return Outcome.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == Code;
	});
}

FAssetDocumentRuntimeValueDto VariableInput(
	const TCHAR* Operation,
	FName Name,
	FName NewName = NAME_None)
{
	if (FCString::Strcmp(Operation, TEXT("rename")) == 0)
	{
		return MakeAssetDocumentUMGObject({
			{TEXT("operation"), MakeAssetDocumentUMGString(TEXT("rename"))},
			{TEXT("old_name"), MakeAssetDocumentUMGString(Name.ToString())},
			{TEXT("new_name"), MakeAssetDocumentUMGString(NewName.ToString())},
		});
	}
	return MakeAssetDocumentUMGObject({
		{TEXT("operation"), MakeAssetDocumentUMGString(Operation)},
		{TEXT("name"), MakeAssetDocumentUMGString(Name.ToString())},
	});
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUMGDesignerBindingLifecycleTest,
	"UEAssetDoc.UMGProvider.DesignerBindingLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUMGDesignerBindingLifecycleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUMGDesignerBindingProviderTests;
	FAssetDocumentUMGProviderTestFixture Fixture;
	TestTrue(TEXT("fixture is created"), Fixture.CreateTransientWidgetBlueprint());
	UWidgetBlueprint* Blueprint = Fixture.GetWidgetBlueprint();
	FDelegateEditorBinding Binding;
	Binding.ObjectName = TEXT("BoundText");
	Binding.PropertyName = TEXT("Text");
	Binding.FunctionName = TEXT("GetText");
	Binding.MemberGuid = FGuid::NewGuid();
	Blueprint->Bindings.Add(Binding);
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider = CreateAssetDocumentUMGPrimitiveProvider();
	FAssetDocumentUMGProviderTestContext Context;
	FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
	FAssetDocumentUMGProviderTestTransaction Transaction(Context);
	const FAssetDocumentPrimitiveOutcome Query = Invoke(Provider.Get(),
		Descriptor(Provider.Get(), TEXT("umg.designer_binding.query.v1")), Context, Handles,
		Transaction, Blueprint, MakeAssetDocumentUMGObject({
			{TEXT("widget_name"), MakeAssetDocumentUMGString(TEXT("BoundText"))},
			{TEXT("property_name"), MakeAssetDocumentUMGString(TEXT("Text"))},
		}));
	TestEqual(TEXT("raw binding query succeeds"), Query.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestEqual(TEXT("raw binding query returns one identity"), Query.Value.Elements[0].Elements.Num(), 1);
	Blueprint->Bindings.Add(Binding);
	FAssetDocumentUMGProviderTestContext DuplicateContext;
	FAssetDocumentUMGProviderTestHandleResolver DuplicateHandles(DuplicateContext);
	FAssetDocumentUMGProviderTestTransaction DuplicateTransaction(DuplicateContext);
	const FAssetDocumentPrimitiveOutcome Duplicate = Invoke(Provider.Get(),
		Descriptor(Provider.Get(), TEXT("umg.designer_binding.query.v1")), DuplicateContext,
		DuplicateHandles, DuplicateTransaction, Blueprint, MakeAssetDocumentUMGObject({
			{TEXT("widget_name"), MakeAssetDocumentUMGNull()},
			{TEXT("property_name"), MakeAssetDocumentUMGNull()},
		}));
	TestTrue(TEXT("duplicate identity fails closed"), HasCode(Duplicate, TEXT("AD.UMG.DESIGNER_BINDING.DUPLICATE_IDENTITY")));
	FAssetDocumentPrimitiveInvocation BlockedInvocation;
	BlockedInvocation.PrimitiveId = TEXT("UMG-DESIGNER-BINDING-APPLY-DELTA-v1");
	BlockedInvocation.Version = 1;
	const FAssetDocumentPrimitiveOutcome Blocked = Provider->Invoke(
		BlockedInvocation, DuplicateHandles, &DuplicateTransaction);
	TestTrue(TEXT("designer binding apply delta remains an exact blocker"),
		HasCode(Blocked, TEXT("AD.G1.BLOCK.UMG.DESIGNER_BINDING_EVIDENCE")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUMGVariableGuidIsEngineObservedTest,
	"UEAssetDoc.UMGProvider.VariableGuidIsEngineObserved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUMGVariableGuidIsEngineObservedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUMGDesignerBindingProviderTests;
	FAssetDocumentUMGProviderTestFixture Fixture;
	TestTrue(TEXT("fixture is created"), Fixture.CreateTransientWidgetBlueprint());
	UWidgetBlueprint* Blueprint = Fixture.GetWidgetBlueprint();
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider = CreateAssetDocumentUMGPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor& Notify =
		Descriptor(Provider.Get(), TEXT("umg.variable.notify_lifecycle.v1"));

	FAssetDocumentUMGProviderTestContext AddContext;
	FAssetDocumentUMGProviderTestHandleResolver AddHandles(AddContext);
	FAssetDocumentUMGProviderTestTransaction AddTransaction(AddContext);
	const FAssetDocumentPrimitiveOutcome Added = Invoke(Provider.Get(), Notify, AddContext,
		AddHandles, AddTransaction, Blueprint, VariableInput(TEXT("add"), TEXT("ObservedVariable")));
	TestEqual(TEXT("public add notification succeeds"), Added.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	const FGuid ObservedGuid = Blueprint->WidgetVariableNameToGuidMap.FindRef(TEXT("ObservedVariable"));
	TestTrue(TEXT("add observes an Engine GUID"), ObservedGuid.IsValid());

	FAssetDocumentUMGProviderTestContext CollisionContext;
	FAssetDocumentUMGProviderTestHandleResolver CollisionHandles(CollisionContext);
	FAssetDocumentUMGProviderTestTransaction CollisionTransaction(CollisionContext);
	const FAssetDocumentPrimitiveOutcome Collision = Invoke(Provider.Get(), Notify, CollisionContext,
		CollisionHandles, CollisionTransaction, Blueprint, VariableInput(TEXT("add"), TEXT("ObservedVariable")));
	TestTrue(TEXT("add collision is stable"), HasCode(Collision, TEXT("AD.UMG.VARIABLE.NAME_COLLISION")));

	FAssetDocumentUMGProviderTestContext RenameContext;
	FAssetDocumentUMGProviderTestHandleResolver RenameHandles(RenameContext);
	FAssetDocumentUMGProviderTestTransaction RenameTransaction(RenameContext);
	const FAssetDocumentPrimitiveOutcome Renamed = Invoke(Provider.Get(), Notify, RenameContext,
		RenameHandles, RenameTransaction, Blueprint,
		VariableInput(TEXT("rename"), TEXT("ObservedVariable"), TEXT("RenamedVariable")));
	TestEqual(TEXT("public rename notification succeeds"), Renamed.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestEqual(TEXT("rename preserves the observed Engine GUID"),
		Blueprint->WidgetVariableNameToGuidMap.FindRef(TEXT("RenamedVariable")), ObservedGuid);

	FAssetDocumentUMGProviderTestContext RemoveContext;
	FAssetDocumentUMGProviderTestHandleResolver RemoveHandles(RemoveContext);
	FAssetDocumentUMGProviderTestTransaction RemoveTransaction(RemoveContext);
	const FAssetDocumentPrimitiveOutcome Removed = Invoke(Provider.Get(), Notify, RemoveContext,
		RemoveHandles, RemoveTransaction, Blueprint, VariableInput(TEXT("remove"), TEXT("RenamedVariable")));
	TestEqual(TEXT("public remove notification succeeds"), Removed.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestFalse(TEXT("remove leaves no observed GUID"), Blueprint->WidgetVariableNameToGuidMap.Contains(TEXT("RenamedVariable")));

	FAssetDocumentUMGProviderTestContext RollbackContext;
	FAssetDocumentUMGProviderTestHandleResolver RollbackHandles(RollbackContext);
	FAssetDocumentUMGProviderTestTransaction RollbackTransaction(RollbackContext);
	RollbackTransaction.SetRestoreCallback([Blueprint]()
	{
		Blueprint->WidgetVariableNameToGuidMap.Remove(TEXT("RollbackVariable"));
	});
	RollbackTransaction.SetPoisonAfterStateRead(3);
	const FAssetDocumentPrimitiveOutcome RolledBack = Invoke(Provider.Get(), Notify, RollbackContext,
		RollbackHandles, RollbackTransaction, Blueprint, VariableInput(TEXT("add"), TEXT("RollbackVariable")));
	TestEqual(TEXT("post-notification transaction poisoning fails"), RolledBack.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestEqual(TEXT("post-notification transaction poisoning rolls back"), RollbackTransaction.GetRollbackCount(), 1);
	TestFalse(TEXT("rollback restores variable GUID state"),
		Blueprint->WidgetVariableNameToGuidMap.Contains(TEXT("RollbackVariable")));
	for (const EAssetDocumentHandleInvalidationReason Reason : {
		EAssetDocumentHandleInvalidationReason::Compile,
		EAssetDocumentHandleInvalidationReason::Reinstance})
	{
		FAssetDocumentUMGProviderTestContext InvalidationContext;
		FAssetDocumentUMGProviderTestHandleResolver InvalidationHandles(InvalidationContext);
		FAssetDocumentUMGProviderTestTransaction InvalidationTransaction(InvalidationContext);
		FAssetDocumentPrimitiveInvocation IssuedInvocation =
			MakeAssetDocumentUMGProviderTestInvocation(Notify, InvalidationContext);
		IssuedInvocation.ValueArguments = VariableInput(TEXT("add"),
			Reason == EAssetDocumentHandleInvalidationReason::Compile
				? TEXT("CompileIssuedHandle") : TEXT("ReinstanceIssuedHandle"));
		AddBlueprintHandle(IssuedInvocation, InvalidationHandles, Blueprint,
			EAssetDocumentHandleAccess::Mutable);
		TestEqual(TEXT("The lifecycle invocation receives one issued input handle."),
			InvalidationHandles.GetIssueCount(), 1);
		TestEqual(TEXT("The lifecycle mutation succeeds before its invalidation event."),
			Provider->Invoke(IssuedInvocation, InvalidationHandles, &InvalidationTransaction).Status,
			EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		FAssetDocumentHandleInvalidation Event;
		Event.Reason = Reason;
		Event.RequestId = InvalidationContext.RequestId;
		Event.TransactionId = InvalidationContext.TransactionId;
		Event.RuntimeRevision = InvalidationContext.RuntimeRevision;
		Event.ProviderId = InvalidationContext.ProviderId;
		InvalidationHandles.Invalidate(Event);
		TestEqual(TEXT("The invalidation event reaches the issued-handle resolver."),
			InvalidationHandles.GetInvalidationCount(), 1);
		const FAssetDocumentPrimitiveOutcome FollowUp = Provider->Invoke(
			IssuedInvocation, InvalidationHandles, &InvalidationTransaction);
		TestEqual(TEXT("Compile or reinstance invalidates issued handles before a follow-up mutation."),
			FollowUp.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
		TestTrue(TEXT("Compile or reinstance follow-up fails closed with the stable invalid-handle diagnostic."),
			HasCode(FollowUp, TEXT("AD.UMG.PROVIDER.INVALID_HANDLE")));
	}
	FAssetDocumentPrimitiveInvocation BlockedInvocation;
	BlockedInvocation.PrimitiveId = TEXT("UMG.VAR.FORCE_GUID_V1");
	BlockedInvocation.Version = 1;
	const FAssetDocumentPrimitiveOutcome Blocked = Provider->Invoke(
		BlockedInvocation, RollbackHandles, &RollbackTransaction);
	TestTrue(TEXT("caller-selected GUID remains an exact blocker"),
		HasCode(Blocked, TEXT("AD.G1.BLOCK.UMG.CALLER_SELECTED_GUID_UNSUPPORTED")));
	return true;
}
#endif
