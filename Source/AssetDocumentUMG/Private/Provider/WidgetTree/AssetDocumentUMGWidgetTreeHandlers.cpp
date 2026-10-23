// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/WidgetTree/AssetDocumentUMGWidgetTreeHandlers.h"

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/ContentWidget.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "EditorUtilityLibrary.h"
#include "Provider/AssetDocumentUMGMutationContract.h"
#include "Provider/AssetDocumentUMGNativePayloads.h"
#include "Provider/AssetDocumentUMGValueDto.h"
#include "UObject/SoftObjectPath.h"
#include "WidgetBlueprintEditorUtils.h"

namespace AssetDocumentUMGWidgetTreeHandlersPrivate
{
FAssetDocumentPrimitiveOutcome FailWidgetTree(
	const TCHAR* Code,
	const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics = {})
{
	return MakeAssetDocumentUMGFailure(Code, Message, Diagnostics);
}

FAssetDocumentPrimitiveOutcome FailWidgetTreeAfterMutation(
	IAssetDocumentAtomicTransaction& Transaction,
	const TCHAR* Message)
{
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	ValidateWidgetBlueprintMutationCompletion(false, Message, Diagnostics);
	RollbackAssetDocumentUMGMutation(
		Transaction, TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"), Diagnostics);
	return FailWidgetTree(
		TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"), Message, Diagnostics);
}

FAssetDocumentPrimitiveOutcome FailEnlist(
	const FAssetDocumentContractValidationResult& Result)
{
	return FailWidgetTree(
		TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
		TEXT("The UMG mutation footprint could not be enlisted."),
		Result.Diagnostics);
}

UWidgetTree* GetTree(UWidgetBlueprint& Blueprint)
{
	return IsValid(Blueprint.WidgetTree) ? Blueprint.WidgetTree : nullptr;
}

bool IsTreeMember(const UWidgetTree& Tree, const UWidget* Candidate)
{
	bool bFound = false;
	Tree.ForEachWidget([Candidate, &bFound](UWidget* Widget)
	{
		bFound |= Widget == Candidate;
	});
	if (bFound)
	{
		return true;
	}
	TArray<FName> SlotNames;
	Tree.GetSlotNames(SlotNames);
	for (const FName SlotName : SlotNames)
	{
		if (Tree.GetContentForSlot(SlotName) == Candidate)
		{
			return true;
		}
	}
	return false;
}

bool IsAttachedToTree(const UWidgetTree& Tree, const UWidget* Widget)
{
	return Widget != nullptr
		&& (Widget->GetParent() != nullptr || Tree.RootWidget == Widget
			|| IsTreeMember(Tree, Widget));
}

bool IsOwnedByTree(const UWidgetTree& Tree, const UWidget* Widget)
{
	return Widget != nullptr && Widget->GetOuter() == &Tree;
}

FName FindNamedSlotForWidget(const UWidgetTree& Tree, const UWidget* Widget)
{
	TArray<FName> SlotNames;
	Tree.GetSlotNames(SlotNames);
	for (const FName SlotName : SlotNames)
	{
		if (Tree.GetContentForSlot(SlotName) == Widget)
		{
			return SlotName;
		}
	}
	return NAME_None;
}

FAssetDocumentRuntimeValueDto MakeWidgetFact(
	const UWidgetTree& Tree,
	const UWidget& Widget)
{
	UPanelWidget* Parent = Widget.GetParent();
	const int32 ChildIndex = Parent != nullptr
		? Parent->GetChildIndex(&Widget) : INDEX_NONE;
	const FName NamedSlot = FindNamedSlotForWidget(Tree, &Widget);
	return MakeAssetDocumentUMGObject({
		{TEXT("name"), MakeAssetDocumentUMGString(Widget.GetName())},
		{TEXT("class_path"), MakeAssetDocumentUMGString(Widget.GetClass()->GetPathName())},
		{TEXT("parent_name"), Parent != nullptr
			? MakeAssetDocumentUMGString(Parent->GetName())
			: MakeAssetDocumentUMGNull()},
		{TEXT("child_index"), ChildIndex != INDEX_NONE
			? MakeAssetDocumentUMGInteger(ChildIndex)
			: MakeAssetDocumentUMGNull()},
		{TEXT("is_variable"), MakeAssetDocumentUMGBool(Widget.bIsVariable)},
		{TEXT("slot_name"), !NamedSlot.IsNone()
			? MakeAssetDocumentUMGString(NamedSlot.ToString())
			: MakeAssetDocumentUMGNull()},
	});
}

FAssetDocumentPrimitiveOutcome MakeTreeNoOp(
	FAssetDocumentRuntimeValueDto&& Value)
{
	return MakeAssetDocumentUMGSuccess(MoveTemp(Value));
}

bool EnlistWidgetTree(
	IAssetDocumentAtomicTransaction& Transaction,
	UWidgetBlueprint& Blueprint,
	TConstArrayView<UObject*> Touched,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	const FAssetDocumentContractValidationResult Result =
		EnlistWidgetBlueprintMutationFootprint(Transaction, Blueprint, Touched);
	if (Result.bValid)
	{
		return true;
	}
	OutFailure = FailEnlist(Result);
	return false;
}

bool IssueWidget(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	UWidget* Widget,
	FAssetDocumentPrimitiveOutcome& Outcome)
{
	return IssueAssetDocumentUMGUObjectResult(
		Invocation, Handles, TEXT("widget"), EAssetDocumentHandleKind::Node,
		EAssetDocumentHandleAccess::Mutable, Widget, Outcome);
}

bool IsWidgetTreeMutationStateValid(IAssetDocumentAtomicTransaction& Transaction)
{
	return Transaction.GetState() == EAssetDocumentAtomicState::Mutating;
}
} // namespace AssetDocumentUMGWidgetTreeHandlersPrivate


FAssetDocumentPrimitiveOutcome InvokeWidgetTreeQuery(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction*)
{
	using namespace AssetDocumentUMGWidgetTreeHandlersPrivate;
	FAssetDocumentUMGWidgetTreeQueryInput Input;
	FString DecodeDiagnostic;
	if (!DecodeWidgetTreeQueryInput(Invocation.ValueArguments, Input, DecodeDiagnostic))
	{
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"), *DecodeDiagnostic);
	}
	(void)Input;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	UWidgetBlueprint* Blueprint = ResolveWidgetBlueprint(
		Invocation, Handles, TEXT("widget_blueprint"),
		EAssetDocumentHandleAccess::ReadOnly, Diagnostics);
	if (Blueprint == nullptr)
	{
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The widget blueprint handle is invalid."), Diagnostics);
	}
	UWidgetTree* Tree = GetTree(*Blueprint);
	if (Tree == nullptr)
	{
		return FailWidgetTree(TEXT("AD.UMG.WIDGET_TREE.INVALID_TREE"),
			TEXT("The widget blueprint has no live widget tree."));
	}
	FAssetDocumentPrimitiveOutcome Outcome = MakeAssetDocumentUMGSuccess(
		MakeAssetDocumentUMGObject({
			{TEXT("nodes"), MakeAssetDocumentUMGArray(TArray<FAssetDocumentRuntimeValueDto>{})},
			{TEXT("named_slots"), MakeAssetDocumentUMGArray(TArray<FAssetDocumentRuntimeValueDto>{})},
		}));
	TArray<FAssetDocumentRuntimeValueDto> Nodes;
	Tree->ForEachWidget([&](UWidget* Widget)
	{
		if (IsValid(Widget))
		{
			Nodes.Add(MakeWidgetFact(*Tree, *Widget));
			IssueAssetDocumentUMGUObjectResult(
				Invocation, Handles, TEXT("widget"), EAssetDocumentHandleKind::Node,
				EAssetDocumentHandleAccess::ReadOnly, Widget, Outcome);
		}
	});
	TArray<FAssetDocumentRuntimeValueDto> NamedSlots;
	TArray<FName> SlotNames;
	Tree->GetSlotNames(SlotNames);
	for (const FName SlotName : SlotNames)
	{
		UWidget* Content = Tree->GetContentForSlot(SlotName);
		NamedSlots.Add(MakeAssetDocumentUMGObject({
			{TEXT("slot_name"), MakeAssetDocumentUMGString(SlotName.ToString())},
			{TEXT("widget_name"), Content != nullptr
				? MakeAssetDocumentUMGString(Content->GetName())
				: MakeAssetDocumentUMGNull()},
		}));
	}
	Outcome.Value = MakeAssetDocumentUMGObject({
		{TEXT("nodes"), MakeAssetDocumentUMGArray(MoveTemp(Nodes))},
		{TEXT("named_slots"), MakeAssetDocumentUMGArray(MoveTemp(NamedSlots))},
	});
	return Outcome.Diagnostics.IsEmpty()
		? Outcome
		: FailWidgetTree(TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The provider could not issue a widget handle."), Outcome.Diagnostics);
}

FAssetDocumentPrimitiveOutcome InvokeWidgetTreeAddSourceWidget(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AssetDocumentUMGWidgetTreeHandlersPrivate;
	FAssetDocumentUMGAddSourceWidgetInput Input;
	FString DecodeDiagnostic;
	if (!DecodeAddSourceWidgetInput(Invocation.ValueArguments, Input, DecodeDiagnostic))
	{
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"), *DecodeDiagnostic);
	}
	if (ActiveTransaction == nullptr)
	{
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("A matching active transaction is required."));
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	UWidgetBlueprint* Blueprint = ResolveWidgetBlueprint(Invocation, Handles,
		TEXT("widget_blueprint"), EAssetDocumentHandleAccess::Mutable, Diagnostics);
	UClass* WidgetClass = ResolveWidgetClass(Invocation, Handles,
		TEXT("widget_class"), Diagnostics);
	UWidget* ParentWidget = ResolveWidget(Invocation, Handles,
		TEXT("parent_widget"), EAssetDocumentHandleAccess::Mutable, Diagnostics, false);
	if (Blueprint == nullptr || WidgetClass == nullptr)
	{
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The required widget blueprint or class handle is invalid."), Diagnostics);
	}
	UWidgetTree* Tree = GetTree(*Blueprint);
	if (Tree == nullptr)
	{
		return FailWidgetTree(TEXT("AD.UMG.WIDGET_TREE.INVALID_BLUEPRINT"),
			TEXT("The widget blueprint has no live widget tree."));
	}
	if (!WidgetClass->IsChildOf(UWidget::StaticClass())
		|| WidgetClass->HasAnyClassFlags(CLASS_Abstract)
		|| WidgetClass->GetPathName() != Input.ClassPath)
	{
		return FailWidgetTree(TEXT("AD.UMG.WIDGET_TREE.INVALID_CLASS"),
			TEXT("The requested class handle does not match a concrete widget class."));
	}
	if (UEditorUtilityLibrary::FindSourceWidgetByName(Blueprint, Input.RequestedName) != nullptr)
	{
		return FailWidgetTree(TEXT("AD.UMG.WIDGET_TREE.NAME_COLLISION"),
			TEXT("The requested source widget name already exists."));
	}
	UPanelWidget* ParentPanel = Cast<UPanelWidget>(ParentWidget);
	if (Input.ParentName.IsSet() != (ParentWidget != nullptr)
		|| (ParentWidget != nullptr && (!IsTreeMember(*Tree, ParentWidget)
			|| ParentWidget->GetFName() != Input.ParentName.GetValue()))
		|| (Input.ParentName.IsSet() && ParentPanel == nullptr))
	{
		return FailWidgetTree(TEXT("AD.UMG.WIDGET_TREE.INVALID_PARENT"),
			TEXT("The requested parent name and typed parent handle do not identify a panel in this tree."));
	}
	if ((!Input.ParentName.IsSet() && Input.ChildIndex.IsSet())
		|| (ParentPanel != nullptr && Input.ChildIndex.IsSet()
			&& Input.ChildIndex.GetValue() > ParentPanel->GetChildrenCount()))
	{
		return FailWidgetTree(TEXT("AD.UMG.WIDGET_TREE.INVALID_INDEX"),
			TEXT("The requested child index is outside the parent panel range."));
	}
	TArray<UObject*> Touched = {Tree};
	if (ParentPanel != nullptr)
	{
		Touched.Add(ParentPanel);
	}
	FAssetDocumentPrimitiveOutcome EnlistFailure;
	if (!EnlistWidgetTree(*ActiveTransaction, *Blueprint, Touched, EnlistFailure))
	{
		return EnlistFailure;
	}
	UWidget* NewWidget = UEditorUtilityLibrary::AddSourceWidget(
		Blueprint, WidgetClass, Input.RequestedName,
		Input.ParentName.Get(NAME_None));
	if (!IsValid(NewWidget) || NewWidget->GetFName() != Input.RequestedName)
	{
		return FailWidgetTreeAfterMutation(*ActiveTransaction,
			TEXT("The AddSourceWidget helper did not create the exact requested widget."));
	}
	if (!IsWidgetTreeMutationStateValid(*ActiveTransaction))
	{
		return FailWidgetTreeAfterMutation(*ActiveTransaction,
			TEXT("The active transaction became invalid after AddSourceWidget."));
	}
	if (ParentPanel != nullptr && Input.ChildIndex.IsSet())
	{
		ParentPanel->ShiftChild(Input.ChildIndex.GetValue(), NewWidget);
		if (!IsWidgetTreeMutationStateValid(*ActiveTransaction))
		{
			return FailWidgetTreeAfterMutation(*ActiveTransaction,
				TEXT("The active transaction became invalid after panel reordering."));
		}
	}
	const int32 FinalIndex = ParentPanel != nullptr
		? ParentPanel->GetChildIndex(NewWidget) : INDEX_NONE;
	const bool bPostcondition = ParentPanel != nullptr
		? NewWidget->GetParent() == ParentPanel
			&& FinalIndex == Input.ChildIndex.Get(ParentPanel->GetChildrenCount() - 1)
		: Tree->RootWidget == NewWidget;
	if (!ValidateWidgetBlueprintMutationCompletion(
		bPostcondition, TEXT("The source widget readback did not match the requested location."), Diagnostics))
	{
		RollbackAssetDocumentUMGMutation(*ActiveTransaction,
			TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"), Diagnostics);
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"),
			TEXT("The source widget readback did not match the requested location."), Diagnostics);
	}
	FAssetDocumentPrimitiveOutcome Outcome = MakeAssetDocumentUMGSuccess(
		MakeAssetDocumentUMGObject({
			{TEXT("actual_name"), MakeAssetDocumentUMGString(NewWidget->GetName())},
			{TEXT("final_parent"), ParentPanel != nullptr
				? MakeAssetDocumentUMGString(ParentPanel->GetName())
				: MakeAssetDocumentUMGNull()},
			{TEXT("final_index"), FinalIndex != INDEX_NONE
				? MakeAssetDocumentUMGInteger(FinalIndex)
				: MakeAssetDocumentUMGNull()},
		}));
	Outcome.ChangedSet.Add(MakeAssetDocumentUMGChangedRecord(
		*Blueprint, TEXT("/WidgetTree"), TEXT("source_widget_added")));
	Outcome.ChangedSet.Add(MakeAssetDocumentUMGChangedRecord(
		*NewWidget, TEXT("/WidgetTree/Widget"), TEXT("added")));
	if (!IssueWidget(Invocation, Handles, NewWidget, Outcome))
	{
		return FailWidgetTreeAfterMutation(*ActiveTransaction,
			TEXT("The provider could not issue the created widget handle."));
	}
	return Outcome;
}

FAssetDocumentPrimitiveOutcome InvokeWidgetTreeDeleteWidget(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AssetDocumentUMGWidgetTreeHandlersPrivate;
	FAssetDocumentUMGDeleteWidgetInput Input;
	FString DecodeDiagnostic;
	if (!DecodeDeleteWidgetInput(Invocation.ValueArguments, Input, DecodeDiagnostic))
	{
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"), *DecodeDiagnostic);
	}
	if (ActiveTransaction == nullptr)
	{
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("A matching active transaction is required."));
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	UWidgetBlueprint* Blueprint = ResolveWidgetBlueprint(Invocation, Handles,
		TEXT("widget_blueprint"), EAssetDocumentHandleAccess::Mutable, Diagnostics);
	UWidget* Widget = ResolveWidget(Invocation, Handles, TEXT("widget"),
		EAssetDocumentHandleAccess::Mutable, Diagnostics);
	if (Blueprint == nullptr || Widget == nullptr)
	{
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The required widget deletion handles are invalid."), Diagnostics);
	}
	UWidgetTree* Tree = GetTree(*Blueprint);
	if (Tree == nullptr || !IsTreeMember(*Tree, Widget))
	{
		return FailWidgetTree(TEXT("AD.UMG.WIDGET_TREE.ROOT_MISMATCH"),
			TEXT("The widget does not belong to the requested widget blueprint tree."));
	}
	TArray<UWidget*> Descendants;
	UWidgetTree::GetChildWidgets(Widget, Descendants);
	TArray<FString> DeletedNames;
	DeletedNames.Add(Widget->GetName());
	TArray<UObject*> Touched = {Tree, Widget};
	for (UWidget* Descendant : Descendants)
	{
		if (IsValid(Descendant))
		{
			DeletedNames.Add(Descendant->GetName());
			Touched.Add(Descendant);
		}
	}
	FAssetDocumentPrimitiveOutcome EnlistFailure;
	if (!EnlistWidgetTree(*ActiveTransaction, *Blueprint, Touched, EnlistFailure))
	{
		return EnlistFailure;
	}
	FWidgetBlueprintEditorUtils::DeleteWidgets(
		Blueprint, {Widget},
		FWidgetBlueprintEditorUtils::EDeleteWidgetWarningType::DeleteSilently);
	if (!IsWidgetTreeMutationStateValid(*ActiveTransaction))
	{
		return FailWidgetTreeAfterMutation(*ActiveTransaction,
			TEXT("The active transaction became invalid after DeleteWidgets."));
	}
	bool bRemoved = true;
	for (const FString& DeletedName : DeletedNames)
	{
		bRemoved &= UEditorUtilityLibrary::FindSourceWidgetByName(
			Blueprint, FName(*DeletedName)) == nullptr;
	}
	if (!ValidateWidgetBlueprintMutationCompletion(
		bRemoved, TEXT("The DeleteWidgets helper left a deleted widget in the tree."), Diagnostics))
	{
		RollbackAssetDocumentUMGMutation(*ActiveTransaction,
			TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"), Diagnostics);
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"),
			TEXT("The DeleteWidgets helper left a deleted widget in the tree."), Diagnostics);
	}
	FAssetDocumentPrimitiveOutcome Outcome = MakeAssetDocumentUMGSuccess(
		MakeAssetDocumentUMGObject({
			{TEXT("deleted_name"), MakeAssetDocumentUMGString(DeletedNames[0])},
			{TEXT("deleted_descendant_names"), [&]()
			{
				TArray<FAssetDocumentRuntimeValueDto> Values;
				for (int32 Index = 1; Index < DeletedNames.Num(); ++Index)
				{
					Values.Add(MakeAssetDocumentUMGString(DeletedNames[Index]));
				}
				return MakeAssetDocumentUMGArray(MoveTemp(Values));
			}()},
		}));
	Outcome.ChangedSet.Add(MakeAssetDocumentUMGChangedRecord(
		*Blueprint, TEXT("/WidgetTree"), TEXT("widget_deleted")));
	return Outcome;
}

FAssetDocumentPrimitiveOutcome InvokeWidgetTreePanelChildAttach(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AssetDocumentUMGWidgetTreeHandlersPrivate;
	FAssetDocumentUMGPanelChildAttachInput Input;
	FString DecodeDiagnostic;
	if (!DecodePanelChildAttachInput(Invocation.ValueArguments, Input, DecodeDiagnostic))
	{
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"), *DecodeDiagnostic);
	}
	if (ActiveTransaction == nullptr)
	{
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("A matching active transaction is required."));
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	UWidgetBlueprint* Blueprint = ResolveWidgetBlueprint(Invocation, Handles,
		TEXT("widget_blueprint"), EAssetDocumentHandleAccess::Mutable, Diagnostics);
	UWidget* Parent = ResolveWidget(Invocation, Handles, TEXT("parent_widget"),
		EAssetDocumentHandleAccess::Mutable, Diagnostics);
	UWidget* Child = ResolveWidget(Invocation, Handles, TEXT("child_widget"),
		EAssetDocumentHandleAccess::Mutable, Diagnostics);
	UPanelWidget* Panel = Cast<UPanelWidget>(Parent);
	if (Blueprint == nullptr || Parent == nullptr || Child == nullptr)
	{
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The required panel attachment handles are invalid."), Diagnostics);
	}
	UWidgetTree* Tree = GetTree(*Blueprint);
	if (Tree == nullptr || !IsTreeMember(*Tree, Parent) || !IsOwnedByTree(*Tree, Child)
		|| Parent == Child || Panel == nullptr)
	{
		return FailWidgetTree(TEXT("AD.UMG.WIDGET_TREE.INVALID_PANEL"),
			TEXT("The requested parent is not a member panel in this widget tree."));
	}
	const int32 CurrentIndex = Panel->GetChildIndex(Child);
	if ((CurrentIndex == INDEX_NONE && IsAttachedToTree(*Tree, Child))
		|| (CurrentIndex != INDEX_NONE && Child->GetParent() != Panel))
	{
		return FailWidgetTree(TEXT("AD.UMG.WIDGET_TREE.CHILD_ALREADY_PARENTED"),
			TEXT("The child is already owned by a different tree location."));
	}
	const int32 MaxIndex = CurrentIndex == INDEX_NONE
		? Panel->GetChildrenCount() : Panel->GetChildrenCount() - 1;
	if (Input.RequestedIndex > MaxIndex)
	{
		return FailWidgetTree(TEXT("AD.UMG.WIDGET_TREE.INVALID_INDEX"),
			TEXT("The requested panel child index is outside the valid range."));
	}
	if (CurrentIndex == Input.RequestedIndex)
	{
		return MakeTreeNoOp(MakeAssetDocumentUMGObject({
			{TEXT("final_index"), MakeAssetDocumentUMGInteger(CurrentIndex)},
			{TEXT("slot_class_path"), MakeAssetDocumentUMGString(Child->Slot->GetClass()->GetPathName())},
		}));
	}
	TArray<UObject*> Touched = {Tree, Panel, Child};
	FAssetDocumentPrimitiveOutcome EnlistFailure;
	if (!EnlistWidgetTree(*ActiveTransaction, *Blueprint, Touched, EnlistFailure))
	{
		return EnlistFailure;
	}
	UPanelSlot* Slot = CurrentIndex == INDEX_NONE
		? Panel->InsertChildAt(Input.RequestedIndex, Child)
		: (Panel->ShiftChild(
			Input.RequestedIndex, Child), Child->Slot.Get());
	if (!IsWidgetTreeMutationStateValid(*ActiveTransaction))
	{
		return FailWidgetTreeAfterMutation(*ActiveTransaction,
			TEXT("The active transaction became invalid after panel child attachment."));
	}
	if (!ValidateWidgetBlueprintMutationCompletion(
		Slot != nullptr && Child->GetParent() == Panel
			&& Panel->GetChildIndex(Child) == Input.RequestedIndex,
		TEXT("The panel child readback did not match the requested index."), Diagnostics))
	{
		RollbackAssetDocumentUMGMutation(*ActiveTransaction,
			TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"), Diagnostics);
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"),
			TEXT("The panel child readback did not match the requested index."), Diagnostics);
	}
	FAssetDocumentPrimitiveOutcome Outcome = MakeAssetDocumentUMGSuccess(
		MakeAssetDocumentUMGObject({
			{TEXT("final_index"), MakeAssetDocumentUMGInteger(Input.RequestedIndex)},
			{TEXT("slot_class_path"), MakeAssetDocumentUMGString(Slot->GetClass()->GetPathName())},
		}));
	Outcome.ChangedSet.Add(MakeAssetDocumentUMGChangedRecord(
		*Blueprint, TEXT("/WidgetTree"), TEXT("panel_child_attached")));
	Outcome.ChangedSet.Add(MakeAssetDocumentUMGChangedRecord(
		*Child, TEXT("/WidgetTree/Child"), TEXT("attached")));
	if (!IssueAssetDocumentUMGUObjectResult(Invocation, Handles, TEXT("slot"),
		EAssetDocumentHandleKind::Subobject, EAssetDocumentHandleAccess::Mutable, Slot, Outcome))
	{
		return FailWidgetTreeAfterMutation(*ActiveTransaction,
			TEXT("The provider could not issue the panel slot handle."));
	}
	return Outcome;
}

FAssetDocumentPrimitiveOutcome InvokeWidgetTreeContentSet(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AssetDocumentUMGWidgetTreeHandlersPrivate;
	FAssetDocumentUMGContentSetInput Input;
	FString DecodeDiagnostic;
	if (!DecodeContentSetInput(Invocation.ValueArguments, Input, DecodeDiagnostic))
	{
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"), *DecodeDiagnostic);
	}
	if (ActiveTransaction == nullptr)
	{
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("A matching active transaction is required."));
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	UWidgetBlueprint* Blueprint = ResolveWidgetBlueprint(Invocation, Handles,
		TEXT("widget_blueprint"), EAssetDocumentHandleAccess::Mutable, Diagnostics);
	UWidget* Parent = ResolveWidget(Invocation, Handles, TEXT("parent_widget"),
		EAssetDocumentHandleAccess::Mutable, Diagnostics);
	UWidget* Child = ResolveWidget(Invocation, Handles, TEXT("child_widget"),
		EAssetDocumentHandleAccess::Mutable, Diagnostics, !Input.bClear);
	UContentWidget* Content = Cast<UContentWidget>(Parent);
	if (Blueprint == nullptr || Parent == nullptr || (!Input.bClear && Child == nullptr))
	{
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The required content mutation handles are invalid."), Diagnostics);
	}
	UWidgetTree* Tree = GetTree(*Blueprint);
	if (Tree == nullptr || !IsTreeMember(*Tree, Parent) || Content == nullptr
		|| (!Input.bClear && !IsOwnedByTree(*Tree, Child)))
	{
		return FailWidgetTree(TEXT("AD.UMG.WIDGET_TREE.INVALID_CONTENT_WIDGET"),
			TEXT("The requested parent is not a content widget in this tree."));
	}
	UWidget* Current = Content->GetContent();
	if (!Input.bClear && Child == Parent)
	{
		return FailWidgetTree(TEXT("AD.UMG.WIDGET_TREE.SELF_CYCLE"),
			TEXT("A content widget cannot contain itself."));
	}
	if ((!Input.bClear && Child == Current) || (Input.bClear && Current == nullptr))
	{
		return MakeTreeNoOp(MakeAssetDocumentUMGObject({
			{TEXT("child_name"), Current != nullptr
				? MakeAssetDocumentUMGString(Current->GetName())
				: MakeAssetDocumentUMGNull()},
		}));
	}
	if (!Input.bClear && IsAttachedToTree(*Tree, Child))
	{
		return FailWidgetTree(TEXT("AD.UMG.WIDGET_TREE.CHILD_ALREADY_PARENTED"),
			TEXT("The requested content child is already attached."));
	}
	TArray<UObject*> Touched = {Tree, Content};
	if (Current != nullptr) { Touched.Add(Current); }
	if (Child != nullptr) { Touched.Add(Child); }
	FAssetDocumentPrimitiveOutcome EnlistFailure;
	if (!EnlistWidgetTree(*ActiveTransaction, *Blueprint, Touched, EnlistFailure))
	{
		return EnlistFailure;
	}
	Content->SetContent(Input.bClear ? nullptr : Child);
	if (!IsWidgetTreeMutationStateValid(*ActiveTransaction))
	{
		return FailWidgetTreeAfterMutation(*ActiveTransaction,
			TEXT("The active transaction became invalid after SetContent."));
	}
	const UWidget* Expected = Input.bClear ? nullptr : Child;
	if (!ValidateWidgetBlueprintMutationCompletion(
		Content->GetContent() == Expected,
		TEXT("The content widget readback did not match the requested child."), Diagnostics))
	{
		RollbackAssetDocumentUMGMutation(*ActiveTransaction,
			TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"), Diagnostics);
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"),
			TEXT("The content widget readback did not match the requested child."), Diagnostics);
	}
	FAssetDocumentPrimitiveOutcome Outcome = MakeAssetDocumentUMGSuccess(
		MakeAssetDocumentUMGObject({
			{TEXT("child_name"), Expected != nullptr
				? MakeAssetDocumentUMGString(Expected->GetName())
				: MakeAssetDocumentUMGNull()},
		}));
	Outcome.ChangedSet.Add(MakeAssetDocumentUMGChangedRecord(
		*Blueprint, TEXT("/WidgetTree"), TEXT("content_set")));
	return Outcome;
}

FAssetDocumentPrimitiveOutcome InvokeWidgetTreeNamedSlotSet(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AssetDocumentUMGWidgetTreeHandlersPrivate;
	FAssetDocumentUMGNamedSlotSetInput Input;
	FString DecodeDiagnostic;
	if (!DecodeNamedSlotSetInput(Invocation.ValueArguments, Input, DecodeDiagnostic))
	{
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.INVALID_INVOCATION"), *DecodeDiagnostic);
	}
	if (ActiveTransaction == nullptr)
	{
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.INVALID_TRANSACTION"),
			TEXT("A matching active transaction is required."));
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	UWidgetBlueprint* Blueprint = ResolveWidgetBlueprint(Invocation, Handles,
		TEXT("widget_blueprint"), EAssetDocumentHandleAccess::Mutable, Diagnostics);
	UWidget* Child = ResolveWidget(Invocation, Handles, TEXT("child_widget"),
		EAssetDocumentHandleAccess::Mutable, Diagnostics, !Input.bClear);
	if (Blueprint == nullptr || (!Input.bClear && Child == nullptr))
	{
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.INVALID_HANDLE"),
			TEXT("The required named slot mutation handles are invalid."), Diagnostics);
	}
	UWidgetTree* Tree = GetTree(*Blueprint);
	if (Tree == nullptr || (!Input.bClear && !IsOwnedByTree(*Tree, Child)))
	{
		return FailWidgetTree(TEXT("AD.UMG.WIDGET_TREE.INVALID_TREE"),
			TEXT("The widget blueprint has no live widget tree."));
	}
	TArray<FName> SlotNames;
	Tree->GetSlotNames(SlotNames);
	if (!SlotNames.Contains(Input.SlotName))
	{
		return FailWidgetTree(TEXT("AD.UMG.WIDGET_TREE.UNKNOWN_NAMED_SLOT"),
			TEXT("The requested named slot is not exposed by the widget tree."));
	}
	UWidget* Current = Tree->GetContentForSlot(Input.SlotName);
	if ((!Input.bClear && Current == Child) || (Input.bClear && Current == nullptr))
	{
		return MakeTreeNoOp(MakeAssetDocumentUMGObject({
			{TEXT("slot_name"), MakeAssetDocumentUMGString(Input.SlotName.ToString())},
			{TEXT("child_name"), Current != nullptr
				? MakeAssetDocumentUMGString(Current->GetName())
				: MakeAssetDocumentUMGNull()},
		}));
	}
	if (!Input.bClear && IsAttachedToTree(*Tree, Child))
	{
		return FailWidgetTree(TEXT("AD.UMG.WIDGET_TREE.CHILD_ALREADY_PARENTED"),
			TEXT("The requested named-slot child is already attached."));
	}
	TArray<UObject*> Touched = {Tree};
	if (Current != nullptr) { Touched.Add(Current); }
	if (Child != nullptr) { Touched.Add(Child); }
	FAssetDocumentPrimitiveOutcome EnlistFailure;
	if (!EnlistWidgetTree(*ActiveTransaction, *Blueprint, Touched, EnlistFailure))
	{
		return EnlistFailure;
	}
	Tree->SetContentForSlot(Input.SlotName, Input.bClear ? nullptr : Child);
	if (!IsWidgetTreeMutationStateValid(*ActiveTransaction))
	{
		return FailWidgetTreeAfterMutation(*ActiveTransaction,
			TEXT("The active transaction became invalid after SetContentForSlot."));
	}
	const UWidget* Expected = Input.bClear ? nullptr : Child;
	if (!ValidateWidgetBlueprintMutationCompletion(
		Tree->GetContentForSlot(Input.SlotName) == Expected,
		TEXT("The named slot readback did not match the requested child."), Diagnostics))
	{
		RollbackAssetDocumentUMGMutation(*ActiveTransaction,
			TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"), Diagnostics);
		return FailWidgetTree(TEXT("AD.UMG.PROVIDER.POSTCONDITION_FAILED"),
			TEXT("The named slot readback did not match the requested child."), Diagnostics);
	}
	FAssetDocumentPrimitiveOutcome Outcome = MakeAssetDocumentUMGSuccess(
		MakeAssetDocumentUMGObject({
			{TEXT("slot_name"), MakeAssetDocumentUMGString(Input.SlotName.ToString())},
			{TEXT("child_name"), Expected != nullptr
				? MakeAssetDocumentUMGString(Expected->GetName())
				: MakeAssetDocumentUMGNull()},
		}));
	Outcome.ChangedSet.Add(MakeAssetDocumentUMGChangedRecord(
		*Blueprint, TEXT("/WidgetTree"), TEXT("named_slot_set")));
	return Outcome;
}

TConstArrayView<FAssetDocumentUMGPrimitiveBinding>
GetAssetDocumentUMGWidgetTreePrimitiveBindings()
{
	static const FAssetDocumentUMGPrimitiveBinding Bindings[] = {
		{TEXT("umg.widget_tree.query.v1"), 1, &InvokeWidgetTreeQuery},
		{TEXT("umg.widget_tree.add_source_widget.v1"), 1, &InvokeWidgetTreeAddSourceWidget},
		{TEXT("umg.widget_tree.delete_widget.v1"), 1, &InvokeWidgetTreeDeleteWidget},
		{TEXT("umg.widget_tree.panel_child.attach.v1"), 1, &InvokeWidgetTreePanelChildAttach},
		{TEXT("umg.widget_tree.content.set.v1"), 1, &InvokeWidgetTreeContentSet},
		{TEXT("umg.widget_tree.named_slot.set.v1"), 1, &InvokeWidgetTreeNamedSlotSet},
	};
	return Bindings;
}
