// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentUMGPrimitiveProvider.h"
#include "AssetDocumentUMGProviderTestSupport.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/ContentWidget.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Fixtures/AssetDocumentUMGProviderTestFixture.h"
#include "Misc/AutomationTest.h"
#include "Provider/AssetDocumentUMGValueDto.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentUMGWidgetTreeProviderTests
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

void AddHandle(
	FAssetDocumentPrimitiveInvocation& Invocation,
	const TCHAR* Name,
	FAssetDocumentUMGProviderTestHandleResolver& Handles,
	UObject* Object,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access)
{
	Invocation.HandleArguments.Add({Name, Handles.AddObjectHandle(Object, Kind, Access)});
}

bool HasCode(const FAssetDocumentPrimitiveOutcome& Outcome, const TCHAR* Code)
{
	return Outcome.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == Code;
	});
}

FAssetDocumentRuntimeValueDto AddInput(
	FName Name,
	UClass& WidgetClass,
	TOptional<FName> ParentName = {},
	TOptional<int32> ChildIndex = {})
{
	return MakeAssetDocumentUMGObject({
		{TEXT("requested_name"), MakeAssetDocumentUMGString(Name.ToString())},
		{TEXT("class_path"), MakeAssetDocumentUMGString(WidgetClass.GetPathName())},
		{TEXT("parent_name"), ParentName.IsSet()
			? MakeAssetDocumentUMGString(ParentName.GetValue().ToString())
			: MakeAssetDocumentUMGNull()},
		{TEXT("child_index"), ChildIndex.IsSet()
			? MakeAssetDocumentUMGInteger(ChildIndex.GetValue())
			: MakeAssetDocumentUMGNull()},
	});
}

FAssetDocumentRuntimeValueDto PanelAttachInput(int32 RequestedIndex)
{
	return MakeAssetDocumentUMGObject({
		{TEXT("requested_index"), MakeAssetDocumentUMGInteger(RequestedIndex)},
	});
}

FAssetDocumentRuntimeValueDto ContentSetInput(bool bClear)
{
	return MakeAssetDocumentUMGObject({
		{TEXT("clear"), MakeAssetDocumentUMGBool(bClear)},
	});
}

FAssetDocumentRuntimeValueDto NamedSlotSetInput(FName SlotName, bool bClear)
{
	return MakeAssetDocumentUMGObject({
		{TEXT("slot_name"), MakeAssetDocumentUMGString(SlotName.ToString())},
		{TEXT("clear"), MakeAssetDocumentUMGBool(bClear)},
	});
}

FString CaptureFullWidgetTreeDigest(const UWidgetBlueprint& Blueprint)
{
	TArray<FString> Facts;
	Facts.Add(Blueprint.GetOutermost()->IsDirty() ? TEXT("package:dirty") : TEXT("package:clean"));
	Facts.Add(Blueprint.WidgetTree != nullptr && Blueprint.WidgetTree->RootWidget != nullptr
		? FString::Printf(TEXT("root:%s"), *Blueprint.WidgetTree->RootWidget->GetPathName())
		: TEXT("root:none"));
	for (const TPair<FName, FGuid>& Pair : Blueprint.WidgetVariableNameToGuidMap)
	{
		Facts.Add(FString::Printf(TEXT("guid:%s:%s"), *Pair.Key.ToString(),
			*Pair.Value.ToString(EGuidFormats::DigitsWithHyphens)));
	}
	for (const FDelegateEditorBinding& Binding : Blueprint.Bindings)
	{
		Facts.Add(FString::Printf(TEXT("binding:%s:%s:%s:%s"),
			*Binding.ObjectName, *Binding.PropertyName.ToString(),
			*Binding.FunctionName.ToString(),
			*Binding.MemberGuid.ToString(EGuidFormats::DigitsWithHyphens)));
	}
	if (Blueprint.WidgetTree != nullptr)
	{
		Blueprint.WidgetTree->ForEachWidget([&Facts](UWidget* Widget)
		{
			if (!IsValid(Widget))
			{
				return;
			}
			const UPanelWidget* Parent = Widget->GetParent();
			Facts.Add(FString::Printf(TEXT("widget:%s:%s:%s:%d"),
				*Widget->GetPathName(), *Widget->GetClass()->GetPathName(),
				Parent != nullptr ? *Parent->GetPathName() : TEXT("none"),
				Parent != nullptr ? Parent->GetChildIndex(Widget) : INDEX_NONE));
		});
		for (const TPair<FName, TObjectPtr<UWidget>>& Pair : Blueprint.WidgetTree->NamedSlotBindings)
		{
			Facts.Add(FString::Printf(TEXT("named_slot:%s:%s"), *Pair.Key.ToString(),
				Pair.Value != nullptr ? *Pair.Value->GetPathName() : TEXT("none")));
		}
	}
	Facts.Sort();
	return FString::Join(Facts, TEXT("|"));
}

void RestorePackageDirty(UWidgetBlueprint& Blueprint, bool bWasDirty)
{
	Blueprint.GetOutermost()->SetDirtyFlag(bWasDirty);
}

void ExpectRollbackParity(
	FAutomationTestBase& Test,
	const TCHAR* Boundary,
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const FAssetDocumentUMGProviderTestTransaction& Transaction,
	const FString& Before,
	const UWidgetBlueprint& Blueprint)
{
	Test.TestEqual(*FString::Printf(TEXT("%s failure is terminal"), Boundary),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestEqual(*FString::Printf(TEXT("%s invokes the rollback callback once"), Boundary),
		Transaction.GetRollbackCount(), 1);
	Test.TestEqual(*FString::Printf(TEXT("%s restores the full widget-tree digest"), Boundary),
		CaptureFullWidgetTreeDigest(Blueprint), Before);
}

FAssetDocumentPrimitiveOutcome Invoke(
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& DescriptorValue,
	const FAssetDocumentUMGProviderTestContext& Context,
	FAssetDocumentUMGProviderTestHandleResolver& Handles,
	FAssetDocumentUMGProviderTestTransaction& Transaction,
	FAssetDocumentRuntimeValueDto&& Value,
	TFunctionRef<void(FAssetDocumentPrimitiveInvocation&)> AddHandles)
{
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentUMGProviderTestInvocation(DescriptorValue, Context);
	Invocation.ValueArguments = MoveTemp(Value);
	AddHandles(Invocation);
	return Provider.Invoke(Invocation, Handles, &Transaction);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUMGWidgetTreeLifecycleTest,
	"UEAssetDoc.UMGProvider.WidgetTreeLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUMGWidgetTreeLifecycleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUMGWidgetTreeProviderTests;
	FAssetDocumentUMGProviderTestFixture Fixture;
	TestTrue(TEXT("fixture is created"), Fixture.CreateTransientWidgetBlueprint());
	UWidgetBlueprint* Blueprint = Fixture.GetWidgetBlueprint();
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentUMGPrimitiveProvider();

	{
		FAssetDocumentUMGProviderTestContext Context;
		FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
		FAssetDocumentUMGProviderTestTransaction Transaction(Context);
		const FAssetDocumentPrimitiveOutcome Outcome = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.query.v1")), Context, Handles, Transaction,
			MakeAssetDocumentUMGObject({{TEXT("include_properties"), MakeAssetDocumentUMGBool(false)}}),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), Handles, Blueprint,
					EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly);
			});
		TestEqual(TEXT("query succeeds"), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	}

	FAssetDocumentUMGProviderTestContext AddContext;
	FAssetDocumentUMGProviderTestHandleResolver AddHandles(AddContext);
	FAssetDocumentUMGProviderTestTransaction AddTransaction(AddContext);
	const FAssetDocumentPrimitiveOutcome Added = Invoke(Provider.Get(),
		Descriptor(Provider.Get(), TEXT("umg.widget_tree.add_source_widget.v1")), AddContext,
		AddHandles, AddTransaction, AddInput(TEXT("RootPanel"), *UCanvasPanel::StaticClass()),
		[&](FAssetDocumentPrimitiveInvocation& Invocation)
		{
			AddHandle(Invocation, TEXT("widget_blueprint"), AddHandles, Blueprint,
				EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
			AddHandle(Invocation, TEXT("widget_class"), AddHandles, UCanvasPanel::StaticClass(),
				EAssetDocumentHandleKind::Class, EAssetDocumentHandleAccess::ReadOnly);
		});
	TestEqual(TEXT("exact-name root creation succeeds"), Added.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestEqual(TEXT("root name is exact"), Blueprint->WidgetTree->RootWidget->GetFName(), FName(TEXT("RootPanel")));
	TestEqual(TEXT("root add enlists before mutation"), AddTransaction.GetEnlistCount(), 1);

	{
		FAssetDocumentUMGProviderTestContext Context;
		FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
		FAssetDocumentUMGProviderTestTransaction Transaction(Context);
		const FAssetDocumentPrimitiveOutcome Collision = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.add_source_widget.v1")), Context,
			Handles, Transaction, AddInput(TEXT("RootPanel"), *UCanvasPanel::StaticClass()),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), Handles, Blueprint,
					EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("widget_class"), Handles, UCanvasPanel::StaticClass(),
					EAssetDocumentHandleKind::Class, EAssetDocumentHandleAccess::ReadOnly);
			});
		TestTrue(TEXT("name collision is stable"), HasCode(Collision, TEXT("AD.UMG.WIDGET_TREE.NAME_COLLISION")));
		TestEqual(TEXT("collision has no enlist"), Transaction.GetEnlistCount(), 0);
	}

	UPanelWidget* RootPanel = Cast<UPanelWidget>(Blueprint->WidgetTree->RootWidget);
	TestNotNull(TEXT("root panel is available for panel attachment"), RootPanel);
	UWidget* FirstChild = Fixture.AddFixtureWidget(UTextBlock::StaticClass(), TEXT("FirstChild"));
	UWidget* SecondChild = Fixture.AddFixtureWidget(UTextBlock::StaticClass(), TEXT("SecondChild"));
	TestNotNull(TEXT("first detached widget fixture is created"), FirstChild);
	TestNotNull(TEXT("second detached widget fixture is created"), SecondChild);
	{
		FAssetDocumentUMGProviderTestContext Context;
		FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
		FAssetDocumentUMGProviderTestTransaction Transaction(Context);
		const FAssetDocumentPrimitiveOutcome Attached = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.panel_child.attach.v1")), Context,
			Handles, Transaction, PanelAttachInput(0),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), Handles, Blueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("parent_widget"), Handles, Blueprint->WidgetTree->RootWidget, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("child_widget"), Handles, FirstChild, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		TestEqual(TEXT("panel attach succeeds"), Attached.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		TestEqual(TEXT("panel attach readback"), RootPanel->GetChildIndex(FirstChild), 0);
		TestEqual(TEXT("panel attach enlists one footprint"), Transaction.GetEnlistCount(), 1);
	}
	{
		FAssetDocumentUMGProviderTestContext Context;
		FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
		FAssetDocumentUMGProviderTestTransaction Transaction(Context);
		const FAssetDocumentPrimitiveOutcome Attached = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.panel_child.attach.v1")), Context,
			Handles, Transaction, PanelAttachInput(1),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), Handles, Blueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("parent_widget"), Handles, RootPanel, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("child_widget"), Handles, SecondChild, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		TestEqual(TEXT("second panel attach succeeds"), Attached.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		TestEqual(TEXT("second panel attach readback"), RootPanel->GetChildIndex(SecondChild), 1);
	}
	{
		FAssetDocumentUMGProviderTestContext Context;
		FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
		FAssetDocumentUMGProviderTestTransaction Transaction(Context);
		const FAssetDocumentPrimitiveOutcome Reordered = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.panel_child.attach.v1")), Context,
			Handles, Transaction, PanelAttachInput(1),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), Handles, Blueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("parent_widget"), Handles, RootPanel, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("child_widget"), Handles, FirstChild, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		TestEqual(TEXT("panel reorder succeeds"), Reordered.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		TestEqual(TEXT("panel reorder moves first child"), RootPanel->GetChildIndex(FirstChild), 1);
		TestEqual(TEXT("panel reorder moves second child"), RootPanel->GetChildIndex(SecondChild), 0);
		TestEqual(TEXT("panel reorder enlists one footprint"), Transaction.GetEnlistCount(), 1);
	}
	{
		FAssetDocumentUMGProviderTestContext Context;
		FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
		FAssetDocumentUMGProviderTestTransaction Transaction(Context);
		const FAssetDocumentPrimitiveOutcome NoOp = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.panel_child.attach.v1")), Context,
			Handles, Transaction, PanelAttachInput(1),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), Handles, Blueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("parent_widget"), Handles, RootPanel, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("child_widget"), Handles, FirstChild, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		TestEqual(TEXT("panel same-index request is a no-op success"), NoOp.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		TestEqual(TEXT("panel no-op has no mutation footprint"), Transaction.GetEnlistCount(), 0);
	}
	{
		FAssetDocumentUMGProviderTestContext Context;
		FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
		FAssetDocumentUMGProviderTestTransaction Transaction(Context);
		const FAssetDocumentPrimitiveOutcome InvalidParent = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.panel_child.attach.v1")), Context,
			Handles, Transaction, PanelAttachInput(0),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), Handles, Blueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("parent_widget"), Handles, FirstChild, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("child_widget"), Handles, SecondChild, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		TestTrue(TEXT("non-panel parent is rejected stably"), HasCode(InvalidParent, TEXT("AD.UMG.WIDGET_TREE.INVALID_PANEL")));
		TestEqual(TEXT("invalid parent has no mutation footprint"), Transaction.GetEnlistCount(), 0);
	}
	{
		UWidget* NestedPanel = Fixture.AddFixtureWidget(UCanvasPanel::StaticClass(), TEXT("NestedPanel"));
		TestNotNull(TEXT("nested panel fixture is created"), NestedPanel);
		RootPanel->AddChild(NestedPanel);
		FAssetDocumentUMGProviderTestContext Context;
		FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
		FAssetDocumentUMGProviderTestTransaction Transaction(Context);
		const FAssetDocumentPrimitiveOutcome AlreadyParented = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.panel_child.attach.v1")), Context,
			Handles, Transaction, PanelAttachInput(0),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), Handles, Blueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("parent_widget"), Handles, NestedPanel, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("child_widget"), Handles, FirstChild, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		TestTrue(TEXT("child with another parent is rejected stably"), HasCode(AlreadyParented, TEXT("AD.UMG.WIDGET_TREE.CHILD_ALREADY_PARENTED")));
		TestEqual(TEXT("already-parented child has no mutation footprint"), Transaction.GetEnlistCount(), 0);
	}
	{
		UWidget* InvalidIndexChild = Fixture.AddFixtureWidget(UTextBlock::StaticClass(), TEXT("InvalidIndexChild"));
		FAssetDocumentUMGProviderTestContext Context;
		FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
		FAssetDocumentUMGProviderTestTransaction Transaction(Context);
		const FAssetDocumentPrimitiveOutcome InvalidIndex = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.panel_child.attach.v1")), Context,
			Handles, Transaction, PanelAttachInput(RootPanel->GetChildrenCount() + 1),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), Handles, Blueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("parent_widget"), Handles, RootPanel, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("child_widget"), Handles, InvalidIndexChild, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		TestTrue(TEXT("out-of-range panel index is rejected stably"), HasCode(InvalidIndex, TEXT("AD.UMG.WIDGET_TREE.INVALID_INDEX")));
		TestEqual(TEXT("invalid panel index has no mutation footprint"), Transaction.GetEnlistCount(), 0);
	}
	{
		FAssetDocumentUMGProviderTestFixture ContentFixture;
		TestTrue(TEXT("content fixture is created"), ContentFixture.CreateTransientWidgetBlueprint());
		UWidgetBlueprint* ContentBlueprint = ContentFixture.GetWidgetBlueprint();
		UPanelWidget* ContentRoot = Cast<UPanelWidget>(
			ContentFixture.AddFixtureWidget(UCanvasPanel::StaticClass(), TEXT("ContentRoot")));
		UContentWidget* Content = Cast<UContentWidget>(
			ContentFixture.AddFixtureWidget(UBorder::StaticClass(), TEXT("ContentHost")));
		UWidget* ContentChild = ContentFixture.AddFixtureWidget(UTextBlock::StaticClass(), TEXT("ContentChild"));
		UWidget* AttachedChild = ContentFixture.AddFixtureWidget(UTextBlock::StaticClass(), TEXT("AttachedContentChild"));
		TestNotNull(TEXT("content root panel fixture is created"), ContentRoot);
		TestNotNull(TEXT("content host fixture is created"), Content);
		ContentRoot->AddChild(Content);
		ContentRoot->AddChild(AttachedChild);
		FAssetDocumentUMGProviderTestContext Context;
		FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
		FAssetDocumentUMGProviderTestTransaction Transaction(Context);
		const FAssetDocumentPrimitiveOutcome Set = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.content.set.v1")), Context,
			Handles, Transaction, ContentSetInput(false),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), Handles, ContentBlueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("parent_widget"), Handles, Content, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("child_widget"), Handles, ContentChild, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		TestEqual(TEXT("non-empty content set succeeds"), Set.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		TestEqual(TEXT("content set raw readback"), Content->GetContent(), ContentChild);
		TestEqual(TEXT("content set enlists one footprint"), Transaction.GetEnlistCount(), 1);

		FAssetDocumentUMGProviderTestContext NoOpContext;
		FAssetDocumentUMGProviderTestHandleResolver NoOpHandles(NoOpContext);
		FAssetDocumentUMGProviderTestTransaction NoOpTransaction(NoOpContext);
		const FAssetDocumentPrimitiveOutcome NoOp = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.content.set.v1")), NoOpContext,
			NoOpHandles, NoOpTransaction, ContentSetInput(false),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), NoOpHandles, ContentBlueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("parent_widget"), NoOpHandles, Content, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("child_widget"), NoOpHandles, ContentChild, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		TestEqual(TEXT("same content set is a no-op success"), NoOp.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		TestEqual(TEXT("content no-op does not enlist a mutation footprint"), NoOpTransaction.GetEnlistCount(), 0);

		FAssetDocumentUMGProviderTestContext ClearContext;
		FAssetDocumentUMGProviderTestHandleResolver ClearHandles(ClearContext);
		FAssetDocumentUMGProviderTestTransaction ClearTransaction(ClearContext);
		const FAssetDocumentPrimitiveOutcome Cleared = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.content.set.v1")), ClearContext,
			ClearHandles, ClearTransaction, ContentSetInput(true),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), ClearHandles, ContentBlueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("parent_widget"), ClearHandles, Content, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		TestEqual(TEXT("non-empty content clear succeeds"), Cleared.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		TestNull(TEXT("content clear raw readback"), Content->GetContent());
		TestEqual(TEXT("content clear enlists one footprint"), ClearTransaction.GetEnlistCount(), 1);

		FAssetDocumentUMGProviderTestContext InvalidContext;
		FAssetDocumentUMGProviderTestHandleResolver InvalidHandles(InvalidContext);
		FAssetDocumentUMGProviderTestTransaction InvalidTransaction(InvalidContext);
		const FAssetDocumentPrimitiveOutcome InvalidParent = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.content.set.v1")), InvalidContext,
			InvalidHandles, InvalidTransaction, ContentSetInput(true),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), InvalidHandles, ContentBlueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("parent_widget"), InvalidHandles, AttachedChild, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		TestTrue(TEXT("non-content parent is rejected stably"), HasCode(InvalidParent, TEXT("AD.UMG.WIDGET_TREE.INVALID_CONTENT_WIDGET")));

		FAssetDocumentUMGProviderTestContext ParentContext;
		FAssetDocumentUMGProviderTestHandleResolver ParentHandles(ParentContext);
		FAssetDocumentUMGProviderTestTransaction ParentTransaction(ParentContext);
		const FAssetDocumentPrimitiveOutcome AlreadyParented = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.content.set.v1")), ParentContext,
			ParentHandles, ParentTransaction, ContentSetInput(false),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), ParentHandles, ContentBlueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("parent_widget"), ParentHandles, Content, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("child_widget"), ParentHandles, AttachedChild, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		TestTrue(TEXT("attached content child is rejected stably"), HasCode(AlreadyParented, TEXT("AD.UMG.WIDGET_TREE.CHILD_ALREADY_PARENTED")));
	}
	{
		FAssetDocumentUMGProviderTestFixture NamedSlotFixture;
		TestTrue(TEXT("named-slot fixture is created"), NamedSlotFixture.CreateTransientWidgetBlueprint());
		UWidgetBlueprint* NamedSlotBlueprint = NamedSlotFixture.GetWidgetBlueprint();
		const FName SlotName(TEXT("Footer"));
		NamedSlotBlueprint->WidgetTree->NamedSlotBindings.Add(SlotName, nullptr);
		UPanelWidget* NamedSlotRoot = Cast<UPanelWidget>(
			NamedSlotFixture.AddFixtureWidget(UCanvasPanel::StaticClass(), TEXT("NamedSlotRoot")));
		UWidget* NamedSlotChild = NamedSlotFixture.AddFixtureWidget(UTextBlock::StaticClass(), TEXT("FooterChild"));
		UWidget* AttachedNamedSlotChild = NamedSlotFixture.AddFixtureWidget(UTextBlock::StaticClass(), TEXT("AttachedFooterChild"));
		TestNotNull(TEXT("named-slot root panel fixture is created"), NamedSlotRoot);
		NamedSlotRoot->AddChild(AttachedNamedSlotChild);

		FAssetDocumentUMGProviderTestContext SetContext;
		FAssetDocumentUMGProviderTestHandleResolver SetHandles(SetContext);
		FAssetDocumentUMGProviderTestTransaction SetTransaction(SetContext);
		const FAssetDocumentPrimitiveOutcome Set = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.named_slot.set.v1")), SetContext,
			SetHandles, SetTransaction, NamedSlotSetInput(SlotName, false),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), SetHandles, NamedSlotBlueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("child_widget"), SetHandles, NamedSlotChild, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		TestEqual(TEXT("named-slot set succeeds"), Set.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		TestEqual(TEXT("named-slot set raw readback"), NamedSlotBlueprint->WidgetTree->GetContentForSlot(SlotName), NamedSlotChild);
		TestEqual(TEXT("named-slot set enlists one footprint"), SetTransaction.GetEnlistCount(), 1);

		FAssetDocumentUMGProviderTestContext NoOpContext;
		FAssetDocumentUMGProviderTestHandleResolver NoOpHandles(NoOpContext);
		FAssetDocumentUMGProviderTestTransaction NoOpTransaction(NoOpContext);
		const FAssetDocumentPrimitiveOutcome NoOp = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.named_slot.set.v1")), NoOpContext,
			NoOpHandles, NoOpTransaction, NamedSlotSetInput(SlotName, false),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), NoOpHandles, NamedSlotBlueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("child_widget"), NoOpHandles, NamedSlotChild, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		TestEqual(TEXT("same named-slot set is a no-op success"), NoOp.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		TestEqual(TEXT("named-slot no-op does not enlist a mutation footprint"), NoOpTransaction.GetEnlistCount(), 0);

		FAssetDocumentUMGProviderTestContext ClearContext;
		FAssetDocumentUMGProviderTestHandleResolver ClearHandles(ClearContext);
		FAssetDocumentUMGProviderTestTransaction ClearTransaction(ClearContext);
		const FAssetDocumentPrimitiveOutcome Cleared = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.named_slot.set.v1")), ClearContext,
			ClearHandles, ClearTransaction, NamedSlotSetInput(SlotName, true),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), ClearHandles, NamedSlotBlueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
			});
		TestEqual(TEXT("named-slot clear succeeds"), Cleared.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		TestNull(TEXT("named-slot clear raw readback"), NamedSlotBlueprint->WidgetTree->GetContentForSlot(SlotName));
		TestFalse(TEXT("named-slot clear removes the occupied slot key"), NamedSlotBlueprint->WidgetTree->NamedSlotBindings.Contains(SlotName));

		FAssetDocumentUMGProviderTestContext UnknownContext;
		FAssetDocumentUMGProviderTestHandleResolver UnknownHandles(UnknownContext);
		FAssetDocumentUMGProviderTestTransaction UnknownTransaction(UnknownContext);
		const FAssetDocumentPrimitiveOutcome UnknownSlot = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.named_slot.set.v1")), UnknownContext,
			UnknownHandles, UnknownTransaction, NamedSlotSetInput(TEXT("UnknownSlot"), false),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), UnknownHandles, NamedSlotBlueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("child_widget"), UnknownHandles, NamedSlotChild, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		TestTrue(TEXT("unknown named slot is rejected stably"), HasCode(UnknownSlot, TEXT("AD.UMG.WIDGET_TREE.UNKNOWN_NAMED_SLOT")));

		NamedSlotBlueprint->WidgetTree->NamedSlotBindings.Add(SlotName, nullptr);
		FAssetDocumentUMGProviderTestContext ParentContext;
		FAssetDocumentUMGProviderTestHandleResolver ParentHandles(ParentContext);
		FAssetDocumentUMGProviderTestTransaction ParentTransaction(ParentContext);
		const FAssetDocumentPrimitiveOutcome AlreadyParented = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.named_slot.set.v1")), ParentContext,
			ParentHandles, ParentTransaction, NamedSlotSetInput(SlotName, false),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), ParentHandles, NamedSlotBlueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("child_widget"), ParentHandles, AttachedNamedSlotChild, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		TestTrue(TEXT("attached named-slot child is rejected stably"), HasCode(AlreadyParented, TEXT("AD.UMG.WIDGET_TREE.CHILD_ALREADY_PARENTED")));
	}
	{
		FAssetDocumentPrimitiveInvocation BlockedInvocation;
		BlockedInvocation.PrimitiveId = TEXT("umg.widget_tree.replace.v1");
		BlockedInvocation.Version = 1;
		FAssetDocumentUMGProviderTestContext Context;
		FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
		FAssetDocumentUMGProviderTestTransaction Transaction(Context);
		const FAssetDocumentPrimitiveOutcome Blocked = Provider->Invoke(
			BlockedInvocation, Handles, &Transaction);
		TestTrue(TEXT("widget tree replacement remains an exact blocker"),
			HasCode(Blocked, TEXT("AD.BLOCK.UMG.WIDGET_REPLACE_HEADLESS_AUTHORITY")));
	}
	{
		FAssetDocumentUMGProviderTestContext Context;
		FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
		FAssetDocumentUMGProviderTestTransaction Transaction(Context);
		const FAssetDocumentPrimitiveOutcome Deleted = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.delete_widget.v1")), Context,
			Handles, Transaction,
			MakeAssetDocumentUMGObject({{TEXT("warning_policy"), MakeAssetDocumentUMGString(TEXT("delete_silently"))}}),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), Handles, Blueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("widget"), Handles, Blueprint->WidgetTree->RootWidget, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		TestEqual(TEXT("silent subtree delete succeeds"), Deleted.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		TestNull(TEXT("silent delete removes root"), Blueprint->WidgetTree->RootWidget);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUMGWidgetTreeRollbackTest,
	"UEAssetDoc.UMGProvider.WidgetTreeRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUMGWidgetTreeRollbackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUMGWidgetTreeProviderTests;
	FAssetDocumentUMGProviderTestFixture Fixture;
	TestTrue(TEXT("fixture is created"), Fixture.CreateTransientWidgetBlueprint());
	UWidgetBlueprint* Blueprint = Fixture.GetWidgetBlueprint();
	const FString DigestBefore = CaptureFullWidgetTreeDigest(*Blueprint);
	const bool bPackageWasDirty = Blueprint->GetOutermost()->IsDirty();
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider = CreateAssetDocumentUMGPrimitiveProvider();
	FAssetDocumentUMGProviderTestContext Context;
	FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
	FAssetDocumentUMGProviderTestTransaction Transaction(Context);
	Transaction.SetRestoreCallback([Blueprint, bPackageWasDirty]()
	{
		if (UWidget* Root = Blueprint->WidgetTree->RootWidget)
		{
			Blueprint->WidgetTree->RemoveWidget(Root);
		}
		Blueprint->WidgetVariableNameToGuidMap.Remove(FName(TEXT("RollbackRoot")));
		Blueprint->GetOutermost()->SetDirtyFlag(bPackageWasDirty);
	});
	Handles.SetIssueFailureAfter(2);
	const FAssetDocumentPrimitiveOutcome Outcome = Invoke(Provider.Get(),
		Descriptor(Provider.Get(), TEXT("umg.widget_tree.add_source_widget.v1")), Context,
		Handles, Transaction, AddInput(TEXT("RollbackRoot"), *UCanvasPanel::StaticClass()),
		[&](FAssetDocumentPrimitiveInvocation& Invocation)
		{
			AddHandle(Invocation, TEXT("widget_blueprint"), Handles, Blueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
			AddHandle(Invocation, TEXT("widget_class"), Handles, UCanvasPanel::StaticClass(), EAssetDocumentHandleKind::Class, EAssetDocumentHandleAccess::ReadOnly);
		});
	TestEqual(TEXT("post-side-effect issue failure fails"), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestEqual(TEXT("post-side-effect failure rolls back"), Transaction.GetRollbackCount(), 1);
	TestEqual(TEXT("rollback restores full fixture digest"), CaptureFullWidgetTreeDigest(*Blueprint), DigestBefore);
	TestTrue(TEXT("rollback footprint contains package"), Transaction.GetEnlistedFootprint().ContainsByPredicate([](const FAssetDocumentMutationFootprint& Item)
	{
		return Item.Kind == EAssetDocumentMutationFootprintKind::Package && Item.RollbackComponentId == TEXT("rollback.package");
	}));

	{
		FAssetDocumentUMGProviderTestFixture ReorderFixture;
		TestTrue(TEXT("add-source reorder rollback fixture is created"), ReorderFixture.CreateTransientWidgetBlueprint());
		UWidgetBlueprint* ReorderBlueprint = ReorderFixture.GetWidgetBlueprint();
		UPanelWidget* Root = Cast<UPanelWidget>(ReorderFixture.AddFixtureWidget(UCanvasPanel::StaticClass(), TEXT("RollbackRoot")));
		ReorderBlueprint->WidgetVariableNameToGuidMap.Add(
			Root->GetFName(), FGuid::NewGuid());
		const FString Before = CaptureFullWidgetTreeDigest(*ReorderBlueprint);
		const bool bWasDirty = ReorderBlueprint->GetOutermost()->IsDirty();
		FAssetDocumentUMGProviderTestContext ReorderContext;
		FAssetDocumentUMGProviderTestHandleResolver ReorderHandles(ReorderContext);
		FAssetDocumentUMGProviderTestTransaction ReorderTransaction(ReorderContext);
		ReorderTransaction.SetRestoreCallback([ReorderBlueprint, bWasDirty]()
		{
			if (UWidget* Added = ReorderBlueprint->WidgetTree->FindWidget(TEXT("RollbackReordered")))
			{
				ReorderBlueprint->WidgetTree->RemoveWidget(Added);
			}
			ReorderBlueprint->WidgetVariableNameToGuidMap.Remove(TEXT("RollbackReordered"));
			RestorePackageDirty(*ReorderBlueprint, bWasDirty);
		});
		ReorderTransaction.SetPoisonAfterStateRead(4);
		const FAssetDocumentPrimitiveOutcome Reordered = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.add_source_widget.v1")), ReorderContext,
			ReorderHandles, ReorderTransaction,
			AddInput(TEXT("RollbackReordered"), *UTextBlock::StaticClass(), Root->GetFName(), 0),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), ReorderHandles, ReorderBlueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("widget_class"), ReorderHandles, UTextBlock::StaticClass(), EAssetDocumentHandleKind::Class, EAssetDocumentHandleAccess::ReadOnly);
				AddHandle(Invocation, TEXT("parent_widget"), ReorderHandles, Root, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		ExpectRollbackParity(*this, TEXT("add-source panel reorder"), Reordered,
			ReorderTransaction, Before, *ReorderBlueprint);
	}
	{
		FAssetDocumentUMGProviderTestFixture PanelFixture;
		TestTrue(TEXT("panel rollback fixture is created"), PanelFixture.CreateTransientWidgetBlueprint());
		UWidgetBlueprint* PanelBlueprint = PanelFixture.GetWidgetBlueprint();
		UPanelWidget* Root = Cast<UPanelWidget>(PanelFixture.AddFixtureWidget(UCanvasPanel::StaticClass(), TEXT("PanelRoot")));
		UWidget* Child = PanelFixture.AddFixtureWidget(UTextBlock::StaticClass(), TEXT("PanelChild"));
		const FString Before = CaptureFullWidgetTreeDigest(*PanelBlueprint);
		const bool bWasDirty = PanelBlueprint->GetOutermost()->IsDirty();
		FAssetDocumentUMGProviderTestContext PanelContext;
		FAssetDocumentUMGProviderTestHandleResolver PanelHandles(PanelContext);
		FAssetDocumentUMGProviderTestTransaction PanelTransaction(PanelContext);
		PanelTransaction.SetRestoreCallback([Root, Child, PanelBlueprint, bWasDirty]()
		{
			Root->RemoveChild(Child);
			RestorePackageDirty(*PanelBlueprint, bWasDirty);
		});
		PanelTransaction.SetPoisonAfterStateRead(3);
		const FAssetDocumentPrimitiveOutcome Attached = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.panel_child.attach.v1")), PanelContext,
			PanelHandles, PanelTransaction, PanelAttachInput(0),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), PanelHandles, PanelBlueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("parent_widget"), PanelHandles, Root, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("child_widget"), PanelHandles, Child, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		ExpectRollbackParity(*this, TEXT("panel insert"), Attached,
			PanelTransaction, Before, *PanelBlueprint);
	}
	{
		FAssetDocumentUMGProviderTestFixture ReorderFixture;
		TestTrue(TEXT("panel reorder rollback fixture is created"), ReorderFixture.CreateTransientWidgetBlueprint());
		UWidgetBlueprint* ReorderBlueprint = ReorderFixture.GetWidgetBlueprint();
		UPanelWidget* Root = Cast<UPanelWidget>(ReorderFixture.AddFixtureWidget(UCanvasPanel::StaticClass(), TEXT("PanelReorderRoot")));
		UWidget* First = ReorderFixture.AddFixtureWidget(UTextBlock::StaticClass(), TEXT("PanelFirst"));
		UWidget* Second = ReorderFixture.AddFixtureWidget(UTextBlock::StaticClass(), TEXT("PanelSecond"));
		Root->AddChild(First);
		Root->AddChild(Second);
		const FString Before = CaptureFullWidgetTreeDigest(*ReorderBlueprint);
		const bool bWasDirty = ReorderBlueprint->GetOutermost()->IsDirty();
		FAssetDocumentUMGProviderTestContext ReorderContext;
		FAssetDocumentUMGProviderTestHandleResolver ReorderHandles(ReorderContext);
		FAssetDocumentUMGProviderTestTransaction ReorderTransaction(ReorderContext);
		ReorderTransaction.SetRestoreCallback([Root, First, ReorderBlueprint, bWasDirty]()
		{
			Root->ShiftChild(0, First);
			RestorePackageDirty(*ReorderBlueprint, bWasDirty);
		});
		ReorderTransaction.SetPoisonAfterStateRead(3);
		const FAssetDocumentPrimitiveOutcome Reordered = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.panel_child.attach.v1")), ReorderContext,
			ReorderHandles, ReorderTransaction, PanelAttachInput(1),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), ReorderHandles, ReorderBlueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("parent_widget"), ReorderHandles, Root, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("child_widget"), ReorderHandles, First, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		ExpectRollbackParity(*this, TEXT("panel reorder"), Reordered,
			ReorderTransaction, Before, *ReorderBlueprint);
	}
	{
		FAssetDocumentUMGProviderTestFixture ContentFixture;
		TestTrue(TEXT("content set rollback fixture is created"), ContentFixture.CreateTransientWidgetBlueprint());
		UWidgetBlueprint* ContentBlueprint = ContentFixture.GetWidgetBlueprint();
		UContentWidget* Content = Cast<UContentWidget>(ContentFixture.AddFixtureWidget(UBorder::StaticClass(), TEXT("RollbackContent")));
		UWidget* Child = ContentFixture.AddFixtureWidget(UTextBlock::StaticClass(), TEXT("RollbackContentChild"));
		const FString Before = CaptureFullWidgetTreeDigest(*ContentBlueprint);
		const bool bWasDirty = ContentBlueprint->GetOutermost()->IsDirty();
		FAssetDocumentUMGProviderTestContext ContentContext;
		FAssetDocumentUMGProviderTestHandleResolver ContentHandles(ContentContext);
		FAssetDocumentUMGProviderTestTransaction ContentTransaction(ContentContext);
		ContentTransaction.SetRestoreCallback([Content, ContentBlueprint, bWasDirty]()
		{
			Content->SetContent(nullptr);
			RestorePackageDirty(*ContentBlueprint, bWasDirty);
		});
		ContentTransaction.SetPoisonAfterStateRead(3);
		const FAssetDocumentPrimitiveOutcome Set = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.content.set.v1")), ContentContext,
			ContentHandles, ContentTransaction, ContentSetInput(false),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), ContentHandles, ContentBlueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("parent_widget"), ContentHandles, Content, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("child_widget"), ContentHandles, Child, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		ExpectRollbackParity(*this, TEXT("content set"), Set,
			ContentTransaction, Before, *ContentBlueprint);
	}
	{
		FAssetDocumentUMGProviderTestFixture ContentFixture;
		TestTrue(TEXT("content clear rollback fixture is created"), ContentFixture.CreateTransientWidgetBlueprint());
		UWidgetBlueprint* ContentBlueprint = ContentFixture.GetWidgetBlueprint();
		UContentWidget* Content = Cast<UContentWidget>(ContentFixture.AddFixtureWidget(UBorder::StaticClass(), TEXT("RollbackClearContent")));
		UWidget* Child = ContentFixture.AddFixtureWidget(UTextBlock::StaticClass(), TEXT("RollbackClearContentChild"));
		Content->SetContent(Child);
		const FString Before = CaptureFullWidgetTreeDigest(*ContentBlueprint);
		const bool bWasDirty = ContentBlueprint->GetOutermost()->IsDirty();
		FAssetDocumentUMGProviderTestContext ContentContext;
		FAssetDocumentUMGProviderTestHandleResolver ContentHandles(ContentContext);
		FAssetDocumentUMGProviderTestTransaction ContentTransaction(ContentContext);
		ContentTransaction.SetRestoreCallback([Content, Child, ContentBlueprint, bWasDirty]()
		{
			Content->SetContent(Child);
			RestorePackageDirty(*ContentBlueprint, bWasDirty);
		});
		ContentTransaction.SetPoisonAfterStateRead(3);
		const FAssetDocumentPrimitiveOutcome Cleared = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.content.set.v1")), ContentContext,
			ContentHandles, ContentTransaction, ContentSetInput(true),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), ContentHandles, ContentBlueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("parent_widget"), ContentHandles, Content, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		ExpectRollbackParity(*this, TEXT("content clear"), Cleared,
			ContentTransaction, Before, *ContentBlueprint);
	}
	{
		FAssetDocumentUMGProviderTestFixture NamedSlotFixture;
		TestTrue(TEXT("named-slot set rollback fixture is created"), NamedSlotFixture.CreateTransientWidgetBlueprint());
		UWidgetBlueprint* NamedSlotBlueprint = NamedSlotFixture.GetWidgetBlueprint();
		const FName SlotName(TEXT("RollbackSlot"));
		NamedSlotBlueprint->WidgetTree->NamedSlotBindings.Add(SlotName, nullptr);
		NamedSlotFixture.AddFixtureWidget(UCanvasPanel::StaticClass(), TEXT("NamedSlotRollbackRoot"));
		UWidget* Child = NamedSlotFixture.AddFixtureWidget(UTextBlock::StaticClass(), TEXT("RollbackSlotChild"));
		const FString Before = CaptureFullWidgetTreeDigest(*NamedSlotBlueprint);
		const bool bWasDirty = NamedSlotBlueprint->GetOutermost()->IsDirty();
		FAssetDocumentUMGProviderTestContext NamedSlotContext;
		FAssetDocumentUMGProviderTestHandleResolver NamedSlotHandles(NamedSlotContext);
		FAssetDocumentUMGProviderTestTransaction NamedSlotTransaction(NamedSlotContext);
		NamedSlotTransaction.SetRestoreCallback([NamedSlotBlueprint, SlotName, bWasDirty]()
		{
			NamedSlotBlueprint->WidgetTree->NamedSlotBindings.Add(SlotName, nullptr);
			RestorePackageDirty(*NamedSlotBlueprint, bWasDirty);
		});
		NamedSlotTransaction.SetPoisonAfterStateRead(3);
		const FAssetDocumentPrimitiveOutcome Set = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.named_slot.set.v1")), NamedSlotContext,
			NamedSlotHandles, NamedSlotTransaction, NamedSlotSetInput(SlotName, false),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), NamedSlotHandles, NamedSlotBlueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("child_widget"), NamedSlotHandles, Child, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		ExpectRollbackParity(*this, TEXT("named-slot set"), Set,
			NamedSlotTransaction, Before, *NamedSlotBlueprint);
	}
	{
		FAssetDocumentUMGProviderTestFixture NamedSlotFixture;
		TestTrue(TEXT("named-slot clear rollback fixture is created"), NamedSlotFixture.CreateTransientWidgetBlueprint());
		UWidgetBlueprint* NamedSlotBlueprint = NamedSlotFixture.GetWidgetBlueprint();
		const FName SlotName(TEXT("RollbackClearSlot"));
		NamedSlotFixture.AddFixtureWidget(UCanvasPanel::StaticClass(), TEXT("NamedSlotClearRollbackRoot"));
		UWidget* Child = NamedSlotFixture.AddFixtureWidget(UTextBlock::StaticClass(), TEXT("RollbackClearSlotChild"));
		NamedSlotBlueprint->WidgetTree->SetContentForSlot(SlotName, Child);
		const FString Before = CaptureFullWidgetTreeDigest(*NamedSlotBlueprint);
		const bool bWasDirty = NamedSlotBlueprint->GetOutermost()->IsDirty();
		FAssetDocumentUMGProviderTestContext NamedSlotContext;
		FAssetDocumentUMGProviderTestHandleResolver NamedSlotHandles(NamedSlotContext);
		FAssetDocumentUMGProviderTestTransaction NamedSlotTransaction(NamedSlotContext);
		NamedSlotTransaction.SetRestoreCallback([NamedSlotBlueprint, SlotName, Child, bWasDirty]()
		{
			NamedSlotBlueprint->WidgetTree->SetContentForSlot(SlotName, Child);
			RestorePackageDirty(*NamedSlotBlueprint, bWasDirty);
		});
		NamedSlotTransaction.SetPoisonAfterStateRead(3);
		const FAssetDocumentPrimitiveOutcome Cleared = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.named_slot.set.v1")), NamedSlotContext,
			NamedSlotHandles, NamedSlotTransaction, NamedSlotSetInput(SlotName, true),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), NamedSlotHandles, NamedSlotBlueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
			});
		ExpectRollbackParity(*this, TEXT("named-slot clear"), Cleared,
			NamedSlotTransaction, Before, *NamedSlotBlueprint);
	}
	{
		FAssetDocumentUMGProviderTestFixture DeleteFixture;
		TestTrue(TEXT("delete rollback fixture is created"), DeleteFixture.CreateTransientWidgetBlueprint());
		UWidgetBlueprint* DeleteBlueprint = DeleteFixture.GetWidgetBlueprint();
		UWidget* Root = DeleteFixture.AddFixtureWidget(UCanvasPanel::StaticClass(), TEXT("RollbackDeleteRoot"));
		const FString Before = CaptureFullWidgetTreeDigest(*DeleteBlueprint);
		const bool bWasDirty = DeleteBlueprint->GetOutermost()->IsDirty();
		FAssetDocumentUMGProviderTestContext DeleteContext;
		FAssetDocumentUMGProviderTestHandleResolver DeleteHandles(DeleteContext);
		FAssetDocumentUMGProviderTestTransaction DeleteTransaction(DeleteContext);
		DeleteTransaction.SetRestoreCallback([DeleteBlueprint, Root, bWasDirty]()
		{
			Root->Rename(
				nullptr,
				DeleteBlueprint->WidgetTree,
				REN_DontCreateRedirectors
					| REN_NonTransactional);
			DeleteBlueprint->WidgetTree->RootWidget = Root;
			RestorePackageDirty(*DeleteBlueprint, bWasDirty);
		});
		DeleteTransaction.SetPoisonAfterStateRead(3);
		const FAssetDocumentPrimitiveOutcome Deleted = Invoke(Provider.Get(),
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.delete_widget.v1")), DeleteContext,
			DeleteHandles, DeleteTransaction,
			MakeAssetDocumentUMGObject({{TEXT("warning_policy"), MakeAssetDocumentUMGString(TEXT("delete_silently"))}}),
			[&](FAssetDocumentPrimitiveInvocation& Invocation)
			{
				AddHandle(Invocation, TEXT("widget_blueprint"), DeleteHandles, DeleteBlueprint, EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
				AddHandle(Invocation, TEXT("widget"), DeleteHandles, Root, EAssetDocumentHandleKind::Node, EAssetDocumentHandleAccess::Mutable);
			});
		ExpectRollbackParity(*this, TEXT("silent subtree delete"), Deleted,
			DeleteTransaction, Before, *DeleteBlueprint);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentUMGWidgetTreeHandleInvalidationTest,
	"UEAssetDoc.UMGProvider.WidgetTreeHandleInvalidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUMGWidgetTreeHandleInvalidationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentUMGWidgetTreeProviderTests;
	FAssetDocumentUMGProviderTestFixture Fixture;
	TestTrue(TEXT("fixture is created"), Fixture.CreateTransientWidgetBlueprint());
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider = CreateAssetDocumentUMGPrimitiveProvider();
	for (const EAssetDocumentHandleInvalidationReason Reason : {
		EAssetDocumentHandleInvalidationReason::Rollback,
		EAssetDocumentHandleInvalidationReason::Compile,
		EAssetDocumentHandleInvalidationReason::Reinstance,
		EAssetDocumentHandleInvalidationReason::RuntimeRevisionSwap,
		EAssetDocumentHandleInvalidationReason::ProviderUnload})
	{
		FAssetDocumentUMGProviderTestContext Context;
		FAssetDocumentUMGProviderTestHandleResolver Handles(Context);
		FAssetDocumentUMGProviderTestTransaction Transaction(Context);
		FAssetDocumentPrimitiveInvocation Invocation = MakeAssetDocumentUMGProviderTestInvocation(
			Descriptor(Provider.Get(), TEXT("umg.widget_tree.query.v1")), Context);
		Invocation.ValueArguments = MakeAssetDocumentUMGObject({{TEXT("include_properties"), MakeAssetDocumentUMGBool(false)}});
		AddHandle(Invocation, TEXT("widget_blueprint"), Handles, Fixture.GetWidgetBlueprint(), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly);
		FAssetDocumentHandleInvalidation Event;
		Event.Reason = Reason;
		Event.RequestId = Context.RequestId;
		Event.TransactionId = Context.TransactionId;
		Event.RuntimeRevision = Context.RuntimeRevision;
		Event.ProviderId = TEXT("umg");
		Handles.Invalidate(Event);
		const FAssetDocumentPrimitiveOutcome Outcome = Provider->Invoke(Invocation, Handles, &Transaction);
		TestTrue(TEXT("invalidated handle fails closed"), HasCode(Outcome, TEXT("AD.UMG.PROVIDER.INVALID_HANDLE")));
	}
	return true;
}
#endif
