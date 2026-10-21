// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

class UPanelSlot;
class UWidget;
class UWidgetAnimation;
class UWidgetBlueprint;

// Raw edits of a Widget Blueprint whose result matches an editor operation (kept in their own
// file with a behaviour test, UEAssetDoc.WidgetBlueprint.RawEdits). Each
// function leaves the Blueprint as the named editor operation leaves it, without its
// transaction and user dialogs: the AssetDocument apply is covered by Core's package
// snapshot instead of the undo buffer.
namespace UE::AssetDocument::Graph::Widget::Raw
{
// Same result as the editor's silent Delete of one widget: removes the
// widget and its descendants, the graph variable nodes of all of them, and the bindings
// and desired focus of Widget itself (like the editor, a descendant's binding/focus stays),
// moves them to the transient package and drops their variable GUIDs.
// bKeepReferences keeps bindings, graph nodes and the variable GUID of Widget itself
// (the document re-creates a widget of another class under the same name) and does not
// compile: the caller adds the replacement before anything compiles.
void DeleteWidget(UWidgetBlueprint& Blueprint, UWidget& Widget, bool bKeepReferences);

// Same result as renaming in the Hierarchy / Details name box: the variable GUID moves to the new
// name (UWidgetBlueprint::OnVariableRenamed); the widget object, its display label, the desired focus, the bindings,
// the animation bindings and possessable names, the Navigation rules that name it, the UI component extension and
// the graph variable references (FBlueprintEditorUtils::ReplaceVariableReferences: getters, setters, bound events of
// this Blueprint and the references in the loaded Blueprints that depend on it, which it marks modified) take the
// new name. Same widget object, same variable GUID. Differences from the editor: the editor-instance parts (preview
// widget, the preview's UI components) do not exist, and the editor's block that re-creates the value pin of
// variable nodes whose name already equals the new one is skipped (for a case-only change the FName already
// matches, so it would orphan the links of every getter instead of keeping them; for any other change no node can
// name the new variable yet). A case-only change renames this Blueprint's bound events directly: their
// HandleVariableRenamed reads it as a duplicate binding.
void RenameWidget(UWidgetBlueprint& Blueprint, UWidget& Widget, FName NewName);

// Removes the widget from wherever it is attached (panel, content, named slot of a
// host widget, inherited named slot or root) without deleting it, as the hierarchy
// drag-and-drop does before re-attaching. Returns false if it was not attached.
bool Detach(UWidgetBlueprint& Blueprint, UWidget& Widget);

// Where a widget is attached: Parent (nullptr for the root or an inherited named slot)
// and SlotName (named slot of Parent or of the Blueprint's parent class; NAME_None for
// a panel/content child or the root). Index is the panel child index (0 otherwise).
struct FPlacement
{
	UWidget* Parent = nullptr;
	FName SlotName;
	int32 Index = 0;
	bool bAttached = false;
	bool bRoot = false;
};
FPlacement GetPlacement(UWidgetBlueprint& Blueprint, UWidget& Widget);

// Attaches a detached widget (hierarchy drop / palette drop target semantics):
// root, inherited named slot, a host's named slot, a panel at Index (InsertChildAt,
// INDEX_NONE appends) or a single-content widget. Same panel = ShiftChild.
bool Place(UWidgetBlueprint& Blueprint, UWidget& Widget, UWidget* Parent, FName SlotName, int32 Index, FString& OutError);

// Child widgets of Parent in Designer hierarchy order, with their named slot (NAME_None
// for panel/content children).
void GetChildren(UWidget& Parent, TArray<TPair<FName, UWidget*>>& OutChildren);

// "many" (panel), "one" (single content: content widget or reflected SetContent), "none".
const TCHAR* ChildCapacity(UWidget& Widget);
TArray<FName> NamedSlotNames(UWidget& Widget);

// SUMGAnimationList: New Animation + rename to Name (default 5 s range at 20 fps
// display rate is replaced by the caller), and Delete Animation.
UWidgetAnimation* CreateAnimation(UWidgetBlueprint& Blueprint, FName Name);
void DeleteAnimation(UWidgetBlueprint& Blueprint, UWidgetAnimation& Animation);

// SWidgetDetailsView::HandleCategoryTextCommitted (the Palette Category row shown when the
// Widget Blueprint itself is selected): the generated class CDO's PaletteCategory and the
// asset-registry copy on the Blueprint. "" is the UUserWidget default ("User Created"),
// which the Details row shows as empty.
FString GetPaletteCategory(const UWidgetBlueprint& Blueprint);
void SetPaletteCategory(UWidgetBlueprint& Blueprint, const FString& Category);

// SDesignerView::OnScreenFillRuleSelected / OnCustomResolutionWidthChanged /
// OnCustomResolutionHeightChanged (the Designer toolbar's Fill Screen / Custom / Desired size and
// the width x height boxes): DesignSizeMode and DesignTimeSize on the generated class CDO, then
// MarkDesignModifed(false), i.e. the Blueprint is marked modified without a recompile.
struct FDesignSize
{
	FString Mode;  // EDesignPreviewSizeMode name: FillScreen, Custom, CustomOnScreen, Desired, DesiredOnScreen
	int32 Width = 0;
	int32 Height = 0;
};
bool GetDesignSize(const UWidgetBlueprint& Blueprint, FDesignSize& Out);
// False (nothing changed) when Mode is not an EDesignPreviewSizeMode name.
bool SetDesignSize(UWidgetBlueprint& Blueprint, const FDesignSize& Size);
} // namespace UE::AssetDocument::Graph::Widget::Raw
