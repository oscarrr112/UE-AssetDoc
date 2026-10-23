// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Raw edits of a Widget Blueprint with the editor's results; see the header. Behaviour test:
// UEAssetDoc.WidgetBlueprint.RawEdits compares DeleteWidget with the
// engine's FWidgetBlueprintEditorUtils::DeleteWidgets on twin Blueprints.

#include "AssetDocumentWidgetRawEdits.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetNavigation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/NamedSlotInterface.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "EdGraph/EdGraph.h"
#include "K2Node.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MovieScene.h"
#include "UIComponentWidgetBlueprintExtension.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditorUtils.h"

namespace UE::AssetDocument::Graph::Widget::Raw
{
namespace
{
bool HasSourceWidgetNamed(UWidgetBlueprint& Blueprint, FName Name)
{
	return Blueprint.GetAllSourceWidgets().ContainsByPredicate([Name](const UWidget* Widget) { return Widget->GetFName() == Name; });
}

bool IsInSubtree(UWidget& Root, UWidget* Candidate)
{
	if (Candidate == nullptr)
	{
		return false;
	}
	TArray<UWidget*> Descendants;
	UWidgetTree::GetChildWidgets(&Root, Descendants);
	return Candidate == &Root || Descendants.Contains(Candidate);
}

// Takes a deleted widget out of the Blueprint: with bDropReferences its graph variable nodes go and its variable GUID
// is dropped (unless another source widget still has the name, as when a widget is replaced by one of the same name);
// the object moves to the transient package so that the name is free for a new widget.
void RetireWidget(UWidgetBlueprint& Blueprint, UWidget& Widget, bool bDropReferences)
{
	const FName Name = Widget.GetFName();
	if (bDropReferences && FBlueprintEditorUtils::IsVariableUsed(&Blueprint, Name))
	{
		FBlueprintEditorUtils::RemoveVariableNodes(&Blueprint, Name);
	}
	Widget.Rename(nullptr, GetTransientPackage());
	if (bDropReferences && !HasSourceWidgetNamed(Blueprint, Name))
	{
		Blueprint.OnVariableRemoved(Name);
	}
}

// Animation tracks follow a renamed widget: within each animation, the widget's bindings are renamed up to and
// including the first one that animates the widget's slot; the possessable of a widget (not slot) binding takes the
// new name as well.
void RenameAnimationBindings(UWidgetBlueprint& Blueprint, FName OldName, FName NewName)
{
	for (UWidgetAnimation* Animation : Blueprint.Animations)
	{
		TArray<FWidgetAnimationBinding*> Affected;
		for (FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
		{
			if (Binding.WidgetName == OldName)
			{
				Affected.Add(&Binding);
				if (!Binding.SlotWidgetName.IsNone())
				{
					break;
				}
			}
		}
		if (Affected.IsEmpty())
		{
			continue;
		}
		UMovieScene* Scene = Animation->MovieScene;
		Scene->Modify();
		for (FWidgetAnimationBinding* Binding : Affected)
		{
			Binding->WidgetName = NewName;
			FMovieScenePossessable* Possessable = Binding->SlotWidgetName.IsNone() ? Scene->FindPossessable(Binding->AnimationGuid) : nullptr;
			if (Possessable != nullptr)
			{
				Possessable->SetName(NewName.ToString());
			}
		}
	}
}

// Graph references after a case-only rename. FName compares case-insensitively, so a component bound event already
// "matches" the new name and its own rename handler would report a duplicate binding and keep the old casing: bound
// events of this Blueprint take the new spelling directly; every other node and the dependent Blueprints go through
// the regular variable rename.
void RenameGraphReferencesCaseOnly(UWidgetBlueprint& Blueprint, FName OldName, FName NewName)
{
	TArray<UEdGraph*> Graphs;
	Blueprint.GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		for (UEdGraphNode* GraphNode : Graph->Nodes)
		{
			UK2Node_ComponentBoundEvent* BoundEvent = Cast<UK2Node_ComponentBoundEvent>(GraphNode);
			if (BoundEvent == nullptr)
			{
				if (UK2Node* K2Node = Cast<UK2Node>(GraphNode))
				{
					K2Node->HandleVariableRenamed(&Blueprint, Blueprint.GeneratedClass, Graph, OldName, NewName);
				}
			}
			else if (BoundEvent->ComponentPropertyName == OldName)
			{
				BoundEvent->Modify();
				BoundEvent->ComponentPropertyName = NewName;
			}
		}
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(&Blueprint);
	TArray<UBlueprint*> Dependents;
	FBlueprintEditorUtils::FindDependentBlueprints(&Blueprint, Dependents);
	for (UBlueprint* Dependent : Dependents)
	{
		FBlueprintEditorUtils::RenameVariableReferences(Dependent, Blueprint.GeneratedClass, OldName, NewName);
	}
}
} // namespace

void DeleteWidget(UWidgetBlueprint& Blueprint, UWidget& Widget, bool bKeepReferences)
{
	const bool bDropReferences = !bKeepReferences;
	TArray<UWidget*> Descendants;
	UWidgetTree::GetChildWidgets(&Widget, Descendants);
	Blueprint.WidgetTree->SetFlags(RF_Transactional);
	Blueprint.WidgetTree->Modify();
	Blueprint.Modify();

	// The widget's own property bindings and desired-focus entry (a descendant's stay).
	if (bDropReferences)
	{
		const FString WidgetName = Widget.GetName();
		Blueprint.Bindings.RemoveAll([&WidgetName](const FDelegateEditorBinding& Binding) { return Binding.ObjectName == WidgetName; });
		FWidgetBlueprintEditorUtils::ReplaceDesiredFocus(&Blueprint, Widget.GetFName(), NAME_None);
	}
	Widget.SetFlags(RF_Transactional);
	const bool bDetached = Detach(Blueprint, Widget);
	RetireWidget(Blueprint, Widget, bDropReferences);
	for (UWidget* Descendant : Descendants)
	{
		Descendant->SetFlags(RF_Transactional);
		Descendant->Modify();
		RetireWidget(Blueprint, *Descendant, true);
	}
	if (!bDetached)
	{
		return;
	}
	if (bKeepReferences)
	{
		// The caller re-creates a widget under this name next (class replacement, like the
		// editor's Replace With). A compile now would see the kept GUID without its widget.
		FBlueprintEditorUtils::MarkBlueprintAsModified(&Blueprint);
	}
	else
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
	}
}

void RenameWidget(UWidgetBlueprint& Blueprint, UWidget& Widget, FName NewName)
{
	const FName OldName = Widget.GetFName();
	const FString NewText = NewName.ToString();

	// The widget object and its variable GUID take the new name (the GUID move is a no-op for a case-only change).
	Blueprint.Modify();
	Widget.Modify();
	Blueprint.OnVariableRenamed(OldName, NewName);
	Widget.SetDisplayLabel(NewText);
	Widget.Rename(*NewText);

	// Everything in the Blueprint that names the widget follows: desired focus, property bindings, animation tracks,
	// the Navigation rules of every widget and the UI component extension.
	FWidgetBlueprintEditorUtils::ReplaceDesiredFocus(&Blueprint, OldName, NewName);
	const FString OldText = OldName.ToString();
	for (FDelegateEditorBinding& Binding : Blueprint.Bindings)
	{
		if (Binding.ObjectName == OldText)
		{
			Binding.ObjectName = NewText;
		}
	}
	RenameAnimationBindings(Blueprint, OldName, NewName);
	Blueprint.WidgetTree->ForEachWidget([OldName, NewName](UWidget* Other)
	{
		if (UWidgetNavigation* Navigation = Other->Navigation)
		{
			Navigation->SetFlags(RF_Transactional);
			Navigation->Modify();
			Navigation->TryToRenameBinding(OldName, NewName);
		}
	});
	if (UUIComponentWidgetBlueprintExtension* Components = UWidgetBlueprintExtension::GetExtension<UUIComponentWidgetBlueprintExtension>(&Blueprint))
	{
		Components->RenameWidget(OldName, NewName);
	}

	// Child Blueprints must not declare the new name themselves; then the class is rebuilt before graph references
	// move to the new variable.
	FBlueprintEditorUtils::ValidateBlueprintChildVariables(&Blueprint, NewName);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
	if (OldName == NewName)
	{
		RenameGraphReferencesCaseOnly(Blueprint, OldName, NewName);
	}
	else
	{
		// Getters, setters and bound events of this Blueprint, and the references in the loaded Blueprints that depend
		// on it (marked modified).
		FBlueprintEditorUtils::ReplaceVariableReferences(&Blueprint, OldName, NewName);
	}
}

FPlacement GetPlacement(UWidgetBlueprint& Blueprint, UWidget& Widget)
{
	FPlacement Placement;
	UWidgetTree* Tree = Blueprint.WidgetTree;
	if (Tree == nullptr)
	{
		return Placement;
	}
	if (Tree->RootWidget == &Widget)
	{
		Placement.bAttached = Placement.bRoot = true;
		return Placement;
	}
	if (UPanelWidget* Panel = Widget.GetParent())
	{
		Placement.bAttached = true;
		Placement.Parent = Panel;
		Placement.Index = Panel->GetChildIndex(&Widget);
		return Placement;
	}
	for (const TPair<FName, TObjectPtr<UWidget>>& Binding : Tree->NamedSlotBindings)
	{
		if (Binding.Value == &Widget)
		{
			Placement.bAttached = true;
			Placement.SlotName = Binding.Key;
			return Placement;
		}
	}
	if (UWidget* Host = FWidgetBlueprintEditorUtils::FindNamedSlotHostWidgetForContent(&Widget, Tree))
	{
		if (INamedSlotInterface* Slots = Cast<INamedSlotInterface>(Host))
		{
			Placement.bAttached = true;
			Placement.Parent = Host;
			Placement.SlotName = Slots->FindSlotForContent(&Widget);
		}
	}
	return Placement;
}

bool Detach(UWidgetBlueprint& Blueprint, UWidget& Widget)
{
	UWidgetTree* Tree = Blueprint.WidgetTree;
	const FPlacement Placement = GetPlacement(Blueprint, Widget);
	if (!Placement.bAttached)
	{
		return false;
	}
	Widget.Modify();
	if (Placement.bRoot)
	{
		Tree->Modify();
		Tree->RootWidget = nullptr;
		return true;
	}
	if (UPanelWidget* Panel = Cast<UPanelWidget>(Placement.Parent); Panel != nullptr && Placement.SlotName.IsNone())
	{
		Panel->SetFlags(RF_Transactional);
		Panel->Modify();
		return Panel->RemoveChild(&Widget);
	}
	if (Placement.Parent != nullptr)
	{
		Placement.Parent->Modify();
		return FWidgetBlueprintEditorUtils::RemoveNamedSlotHostContent(&Widget, TScriptInterface<INamedSlotInterface>(Placement.Parent));
	}
	Tree->Modify();
	Tree->SetContentForSlot(Placement.SlotName, nullptr);
	return true;
}

bool Place(UWidgetBlueprint& Blueprint, UWidget& Widget, UWidget* Parent, FName SlotName, int32 Index, FString& OutError)
{
	UWidgetTree* Tree = Blueprint.WidgetTree;
	if (Parent != nullptr && IsInSubtree(Widget, Parent))
	{
		OutError = FString::Printf(TEXT("'%s' cannot be placed inside itself or its own child."), *Widget.GetName());
		return false;
	}
	// Reorder inside the same panel keeps the slot (SPanelWidget drag inside one parent).
	UPanelWidget* Panel = Cast<UPanelWidget>(Parent);
	if (Panel != nullptr && SlotName.IsNone() && Widget.GetParent() == Panel)
	{
		const int32 Count = Panel->GetChildrenCount();
		const int32 Target = Index == INDEX_NONE ? Count - 1 : FMath::Clamp(Index, 0, Count - 1);
		if (Panel->GetChildIndex(&Widget) != Target)
		{
			Panel->Modify();
			Panel->ShiftChild(Target, &Widget);
		}
		return true;
	}
	const FPlacement Current = GetPlacement(Blueprint, Widget);
	if (Current.bAttached)
	{
		Detach(Blueprint, Widget);
	}
	Widget.SetFlags(RF_Transactional);
	Widget.Modify();
	if (Parent == nullptr && SlotName.IsNone())
	{
		if (Tree->RootWidget != nullptr)
		{
			OutError = FString::Printf(TEXT("The root is '%s'; move or remove it first."), *Tree->RootWidget->GetName());
			return false;
		}
		Tree->Modify();
		Tree->RootWidget = &Widget;
		return true;
	}
	if (Parent == nullptr)
	{
		if (Blueprint.GeneratedClass != nullptr && !Blueprint.GetInheritedAvailableNamedSlots().Contains(SlotName))
		{
			OutError = FString::Printf(TEXT("The parent class has no available named slot '%s'. Available: %s."), *SlotName.ToString(),
				*FString::JoinBy(Blueprint.GetInheritedAvailableNamedSlots(), TEXT(", "), [](FName Name) { return Name.ToString(); }));
			return false;
		}
		if (Tree->GetContentForSlot(SlotName) != nullptr)
		{
			OutError = FString::Printf(TEXT("Named slot '%s' already holds '%s'."), *SlotName.ToString(), *Tree->GetContentForSlot(SlotName)->GetName());
			return false;
		}
		Tree->Modify();
		Tree->SetContentForSlot(SlotName, &Widget);
		return true;
	}
	if (!SlotName.IsNone())
	{
		INamedSlotInterface* Host = Cast<INamedSlotInterface>(Parent);
		TArray<FName> Names;
		if (Host != nullptr)
		{
			Host->GetSlotNames(Names);
		}
		if (!Names.Contains(SlotName))
		{
			OutError = FString::Printf(TEXT("'%s' has no named slot '%s'. Named slots: %s."), *Parent->GetName(), *SlotName.ToString(),
				*FString::JoinBy(Names, TEXT(", "), [](FName Name) { return Name.ToString(); }));
			return false;
		}
		if (UWidget* Existing = Host->GetContentForSlot(SlotName))
		{
			OutError = FString::Printf(TEXT("Named slot '%s' of '%s' already holds '%s'."), *SlotName.ToString(), *Parent->GetName(), *Existing->GetName());
			return false;
		}
		Parent->SetFlags(RF_Transactional);
		Parent->Modify();
		Host->SetContentForSlot(SlotName, &Widget);
		return true;
	}
	if (Panel == nullptr)
	{
		OutError = FString::Printf(TEXT("'%s' (%s) cannot have children."), *Parent->GetName(), *Parent->GetClass()->GetName());
		return false;
	}
	if (!Panel->CanAddMoreChildren())
	{
		OutError = FString::Printf(TEXT("'%s' (%s) holds a single child and already has '%s'."), *Panel->GetName(),
			*Panel->GetClass()->GetName(), *GetNameSafe(Panel->GetChildAt(0)));
		return false;
	}
	Panel->SetFlags(RF_Transactional);
	Panel->Modify();
	UPanelSlot* Slot = Index == INDEX_NONE || Index >= Panel->GetChildrenCount() ? Panel->AddChild(&Widget) : Panel->InsertChildAt(FMath::Max(Index, 0), &Widget);
	if (Slot == nullptr)
	{
		OutError = FString::Printf(TEXT("'%s' did not accept '%s'."), *Panel->GetName(), *Widget.GetName());
		return false;
	}
	return true;
}

void GetChildren(UWidget& Parent, TArray<TPair<FName, UWidget*>>& OutChildren)
{
	if (INamedSlotInterface* Host = Cast<INamedSlotInterface>(&Parent))
	{
		TArray<FName> Names;
		Host->GetSlotNames(Names);
		for (const FName Name : Names)
		{
			if (UWidget* Content = Host->GetContentForSlot(Name))
			{
				OutChildren.Emplace(Name, Content);
			}
		}
	}
	if (UPanelWidget* Panel = Cast<UPanelWidget>(&Parent))
	{
		for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
		{
			if (UWidget* Child = Panel->GetChildAt(Index))
			{
				OutChildren.Emplace(NAME_None, Child);
			}
		}
	}
}

const TCHAR* ChildCapacity(UWidget& Widget)
{
	if (UPanelWidget* Panel = Cast<UPanelWidget>(&Widget))
	{
		return Panel->CanHaveMultipleChildren() ? TEXT("many") : TEXT("one");
	}
	return TEXT("none");
}

TArray<FName> NamedSlotNames(UWidget& Widget)
{
	TArray<FName> Names;
	if (INamedSlotInterface* Host = Cast<INamedSlotInterface>(&Widget))
	{
		Host->GetSlotNames(Names);
	}
	return Names;
}

UWidgetAnimation* CreateAnimation(UWidgetBlueprint& Blueprint, FName Name)
{
	// A new animation starts like the editor's New Animation: displayed at 20 fps, playback and work range 0 to 5 s.
	constexpr double InitialLengthSeconds = 5.0;
	const FFrameRate DisplayRate(20, 1);

	UWidgetAnimation* Animation = NewObject<UWidgetAnimation>(&Blueprint, Name, RF_Transactional);
	Animation->SetDisplayLabel(Name.ToString());
	UMovieScene* Scene = NewObject<UMovieScene>(Animation, Name, RF_Transactional);
	Animation->MovieScene = Scene;
	Scene->SetDisplayRate(DisplayRate);
	const FFrameNumber EndFrame = (InitialLengthSeconds * Scene->GetTickResolution()).FrameNumber;
	Scene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), EndFrame + 1));
	FMovieSceneEditorData& EditorData = Scene->GetEditorData();
	EditorData.WorkStart = 0.0;
	EditorData.WorkEnd = InitialLengthSeconds;

	Blueprint.Modify();
	Blueprint.Animations.Add(Animation);
	Blueprint.OnVariableAdded(Animation->GetFName());
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
	return Animation;
}

void DeleteAnimation(UWidgetBlueprint& Blueprint, UWidgetAnimation& Animation)
{
	const FName Name = Animation.GetFName();
	Blueprint.Modify();
	Blueprint.Animations.Remove(&Animation);
	// Out of the Blueprint's outer, so that a new animation can take the name.
	Animation.Rename(nullptr, GetTransientPackage());
	Blueprint.OnVariableRemoved(Name);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
}

FString GetPaletteCategory(const UWidgetBlueprint& Blueprint)
{
	const UUserWidget* Defaults = Blueprint.GeneratedClass != nullptr ? Blueprint.GeneratedClass->GetDefaultObject<UUserWidget>() : nullptr;
	if (Defaults == nullptr || Defaults->PaletteCategory.EqualToCaseIgnored(GetDefault<UUserWidget>()->PaletteCategory))
	{
		return FString(); // SWidgetDetailsView::GetCategoryText shows the default as empty
	}
	return Defaults->PaletteCategory.ToString();
}

void SetPaletteCategory(UWidgetBlueprint& Blueprint, const FString& Category)
{
	UUserWidget* Defaults = CastChecked<UUserWidget>(Blueprint.GeneratedClass->GetDefaultObject());
	const FText Text = Category.IsEmpty() ? GetDefault<UUserWidget>()->PaletteCategory : FText::FromString(Category);
	Defaults->SetFlags(RF_Transactional);
	Defaults->Modify();
	Blueprint.Modify();
	Defaults->PaletteCategory = Text;
	// The Blueprint keeps a copy for the asset registry, read while the Blueprint is not loaded.
	Blueprint.PaletteCategory = Text.ToString();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
}

bool GetDesignSize(const UWidgetBlueprint& Blueprint, FDesignSize& Out)
{
	const UUserWidget* Defaults = Blueprint.GeneratedClass != nullptr ? Blueprint.GeneratedClass->GetDefaultObject<UUserWidget>() : nullptr;
	if (Defaults == nullptr)
	{
		return false;
	}
	Out.Mode = StaticEnum<EDesignPreviewSizeMode>()->GetNameStringByValue(static_cast<int64>(Defaults->DesignSizeMode));
	// The Designer's width/height boxes are integer spin boxes (SDesignerView::GetCustomResolution*).
	Out.Width = FMath::TruncToInt32(Defaults->DesignTimeSize.X);
	Out.Height = FMath::TruncToInt32(Defaults->DesignTimeSize.Y);
	return true;
}

bool SetDesignSize(UWidgetBlueprint& Blueprint, const FDesignSize& Size)
{
	const int64 Mode = StaticEnum<EDesignPreviewSizeMode>()->GetValueByNameString(Size.Mode);
	UUserWidget* Defaults = Blueprint.GeneratedClass != nullptr ? Blueprint.GeneratedClass->GetDefaultObject<UUserWidget>() : nullptr;
	if (Mode == INDEX_NONE || Defaults == nullptr)
	{
		return false;
	}
	Defaults->Modify();
	Defaults->DesignSizeMode = static_cast<EDesignPreviewSizeMode>(Mode);
	Defaults->DesignTimeSize = FVector2D(Size.Width, Size.Height);
	FBlueprintEditorUtils::MarkBlueprintAsModified(&Blueprint);
	return true;
}
} // namespace UE::AssetDocument::Graph::Widget::Raw
