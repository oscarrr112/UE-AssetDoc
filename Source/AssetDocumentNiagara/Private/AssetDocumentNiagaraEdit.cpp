// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// fx.niagara.edit.v1: system-level edits of a Niagara system, made through the Niagara editor's own view models
// built headlessly (FNiagaraSystemViewModel in data-processing mode, as N1 reads), one view model per call and
// everything re-resolved by name for each op (adding or deleting an emitter rebuilds the handle view models).
//
// Input {ops: [op]}; ops run in order and the first refused op fails the call (Core rolls the package back):
//   {op: "add_emitter", name, from: {kind: "asset", emitter: <emitter asset path>, version?: <guid>}
//                                  | {kind: "empty"} | {kind: "minimal"}}
//        "+ Emitter": FNiagaraSystemViewModel::AddEmitter (copies the asset; keeps it as the parent when it is
//        inheritable) / AddEmptyEmitter / AddMinimalEmitter, then the overview's rename to name.
//   {op: "delete_emitter", name}                  FNiagaraSystemViewModel::DeleteEmitters
//   {op: "rename_emitter", name, to}              FNiagaraEmitterHandleViewModel::VerifyNameTextChanged + SetName
//   {op: "emitter", name, enabled?, remove_parent?, properties?, reset?}
//        enabled: the overview checkbox (FNiagaraEmitterHandleViewModel::SetIsEnabled, no compile request);
//        remove_parent: "Remove Parent Emitter" (FNiagaraEmitterViewModel::RemoveParentEmitter);
//        properties / reset: Emitter Properties Details (properties: values to write; reset: names set back to
//        the defaults, which the extract leaves out): Modify, the FVersionedNiagaraEmitterData member written
//        with the typed value codec, UNiagaraEmitter::PostEditChangeVersionedProperty.
//   {op: "add_renderer", emitter, name, class}   the Render group's "+" (add utilities' action for the class),
//        then the renderer object renamed to name.
//   {op: "delete_renderer", emitter, name}       UNiagaraStackRendererItem::Delete (inherited ones refused)
//   {op: "order_renderers", emitter, names}      UNiagaraStackRendererItem::MoveRendererUp until names' order holds
//   {op: "renderer", emitter, name, enabled?, properties?, reset?}
//        enabled: the item's checkbox (UNiagaraStackItem::SetIsEnabled); properties / reset: the renderer's
//        Details (reset: back to the class defaults): Modify, PreEditChange, write, PostEditChangeProperty.
//   user parameter ops: AssetDocumentNiagaraUserParameters.cpp; module stack ops: AssetDocumentNiagaraStackEdit.cpp;
//   scratch pad ops: AssetDocumentNiagaraScriptEdit.cpp (which also holds the ops on script assets).
// Output {changed, created: [emitter names]}. A system open in the Niagara editor is closed for the apply and reopened
// (AssetDocumentNiagaraOpenEditor.cpp): the editor holds its own view models and scratch pad copies.
//
// Other assets: script assets (AssetDocumentNiagaraScriptEdit.cpp), emitter assets (the emitter editor's working
// copy runs these system ops: AssetDocumentNiagaraEmitterAsset.cpp) and parameter collections
// (AssetDocumentNiagaraCollectionEdit.cpp).

#include "AssetDocumentNiagaraPrimitives.h"

#include "AssetDocumentEditorNonInteractiveScope.h"

#include "Editor.h"
#include "NiagaraEditorModule.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraParameterCollection.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraEmitterViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/INiagaraStackItemGroupAddUtilities.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackRenderItemGroup.h"
#include "ViewModels/Stack/NiagaraStackRendererItem.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

#define LOCTEXT_NAMESPACE "AssetDocumentNiagaraEdit"

namespace UE::AssetDocument::Niagara
{
namespace
{
using Graph::FReader;

struct FEdit
{
	UNiagaraSystem& System;
	TSharedRef<FNiagaraSystemViewModel> ViewModel;
	bool bChanged = false;
	TArray<FString> Created;
};

TSharedPtr<FNiagaraEmitterHandleViewModel> FindEmitter(FEdit& Edit, const FString& Name)
{
	for (const TSharedRef<FNiagaraEmitterHandleViewModel>& Handle : Edit.ViewModel->GetEmitterHandleViewModels())
	{
		if (Handle->GetEmitterHandle() != nullptr && Handle->GetName().ToString() == Name)
		{
			return Handle.ToSharedPtr();
		}
	}
	return nullptr;
}

bool RequireEmitter(FReader& Op, FEdit& Edit, const TCHAR* Field, TSharedPtr<FNiagaraEmitterHandleViewModel>& OutHandle)
{
	FString Name;
	if (!Op.String(Field, Name))
	{
		return false;
	}
	OutHandle = FindEmitter(Edit, Name);
	if (!OutHandle.IsValid())
	{
		return Op.Fail(TEXT("Niagara.EmitterNotFound"), FString::Printf(TEXT("The system has no emitter '%s'."), *Name), Field);
	}
	return true;
}

// A standard (stateful) emitter (lightweight emitters have their own ops).
bool RequireStandard(FReader& Op, FNiagaraEmitterHandleViewModel& Handle, const TCHAR* Field)
{
	const FNiagaraEmitterHandle* EmitterHandle = Handle.GetEmitterHandle();
	if (EmitterHandle == nullptr || EmitterHandle->GetEmitterMode() != ENiagaraEmitterMode::Standard || EmitterHandle->GetEmitterData() == nullptr)
	{
		return Op.Fail(TEXT("Niagara.StatelessEmitter"), FString::Printf(TEXT(
			"'%s' is a lightweight (stateless) emitter: author its Stateless value (its properties, modules and renderers)."),
			*Handle.GetName().ToString()), Field);
	}
	return true;
}

// The emitter object holding the renderers of a standard or a lightweight (stateless) emitter.
bool RequireRenderable(FReader& Op, FNiagaraEmitterHandleViewModel& Handle, const TCHAR* Field)
{
	const FNiagaraEmitterHandle* EmitterHandle = Handle.GetEmitterHandle();
	const bool bStateless = EmitterHandle != nullptr && EmitterHandle->GetEmitterMode() == ENiagaraEmitterMode::Stateless && EmitterHandle->GetEmitterBase() != nullptr;
	return bStateless || RequireStandard(Op, Handle, Field);
}

// The emitter's renderers in order (a lightweight emitter's list is reflected: UNiagaraStatelessEmitter is
// engine-internal).
TArray<UNiagaraRendererProperties*> EmitterRenderers(FNiagaraEmitterHandleViewModel& Handle)
{
	TArray<UNiagaraRendererProperties*> Out;
	const FNiagaraEmitterHandle* EmitterHandle = Handle.GetEmitterHandle();
	if (EmitterHandle == nullptr)
	{
		return Out;
	}
	if (EmitterHandle->GetEmitterMode() == ENiagaraEmitterMode::Stateless)
	{
		for (UObject* Object : ReflectedObjectList(EmitterHandle->GetEmitterBase(), TEXT("RendererProperties")))
		{
			if (UNiagaraRendererProperties* Renderer = Cast<UNiagaraRendererProperties>(Object))
			{
				Out.Add(Renderer);
			}
		}
		return Out;
	}
	if (const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterViewModel()->GetEmitter().GetEmitterData())
	{
		Out = Data->GetRenderers();
	}
	return Out;
}

// The overview's rename: the editor's name rules, then SetName (which sanitizes and makes the name unique: a
// result other than the requested name is refused).
bool RenameHandle(FReader& Op, FEdit& Edit, FNiagaraEmitterHandleViewModel& Handle, const FString& To, const TCHAR* Field)
{
	FText Error;
	if (!Handle.VerifyNameTextChanged(FText::FromString(To), Error))
	{
		return Op.Fail(TEXT("Niagara.InvalidEmitterName"), FString::Printf(TEXT("'%s' is not an emitter name: %s"), *To, *Error.ToString()), Field);
	}
	if (FindEmitter(Edit, To).IsValid() && Handle.GetName().ToString() != To)
	{
		return Op.Fail(TEXT("Niagara.EmitterExists"), FString::Printf(TEXT("The system already has an emitter '%s'."), *To), Field);
	}
	Handle.SetName(FName(*To));
	if (Handle.GetName().ToString() != To)
	{
		return Op.Fail(TEXT("Niagara.InvalidEmitterName"), FString::Printf(
			TEXT("'%s' is not a valid emitter name (the editor makes it '%s'); use letters, digits and underscores."), *To,
			*Handle.GetName().ToString()), Field);
	}
	return true;
}

bool AddEmitter(FReader& Op, FEdit& Edit)
{
	FString Name;
	const FValue* From = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("from")}) || !Op.String(TEXT("name"), Name) || !Op.Object(TEXT("from"), From))
	{
		return false;
	}
	if (FindEmitter(Edit, Name).IsValid())
	{
		return Op.Fail(TEXT("Niagara.EmitterExists"), FString::Printf(TEXT("The system already has an emitter '%s'."), *Name), TEXT("name"));
	}
	FReader Source = Op.Child(TEXT("from"), *From);
	FString Kind;
	// inherit: the document names the asset as the new emitter's Parent, which the editor sets only for an inheritable
	// asset (UNiagaraSystem::AddEmitterHandle: an emitter added from one whose Is Inheritable is off is a plain copy).
	bool bInherit = false;
	if (!Source.OnlyFields({TEXT("kind"), TEXT("emitter"), TEXT("version"), TEXT("inherit")}) || !Source.String(TEXT("kind"), Kind)
		|| !Source.Bool(TEXT("inherit"), bInherit))
	{
		return false;
	}
	TSharedPtr<FNiagaraEmitterHandleViewModel> Added;
	if (Kind == TEXT("asset"))
	{
		FString Path;
		if (!Source.String(TEXT("emitter"), Path))
		{
			return false;
		}
		UNiagaraEmitter* Template = LoadObject<UNiagaraEmitter>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (Template == nullptr || !Template->IsAsset())
		{
			return Source.Fail(TEXT("Niagara.EmitterAssetNotFound"), FString::Printf(TEXT("'%s' is not a Niagara emitter asset."), *Path), TEXT("emitter"));
		}
		if (bInherit && !Template->bIsInheritable)
		{
			return Source.Fail(TEXT("Niagara.NotInheritable"), FString::Printf(TEXT("%s cannot be a parent: its Is Inheritable option "
				"(Asset Options) is off, as for the engine's template emitters (Fountain, ...), so the editor adds a copy of it without a "
				"parent. Only an emitter asset whose Is Inheritable is on can be a Parent. Leave Parent out to add the copy (From: the "
				"same asset), or name an inheritable emitter asset."), *Template->GetPathName()), TEXT("inherit"));
		}
		FGuid Version = Template->IsVersioningEnabled() ? Template->GetExposedVersion().VersionGuid : FGuid();
		if (Source.Has(TEXT("version")))
		{
			// Versions are written as the extract shows them (hyphenated or not).
			FString Text;
			if (!Source.String(TEXT("version"), Text))
			{
				return false;
			}
			if (!FGuid::Parse(Text, Version) || !Template->IsVersioningEnabled() || Template->GetEmitterData(Version) == nullptr)
			{
				return Source.Fail(TEXT("Niagara.EmitterVersionNotFound"), FString::Printf(TEXT("%s has no version %s."), *Path, *Text), TEXT("version"));
			}
		}
		Added = Edit.ViewModel->AddEmitter(*Template, Version);
	}
	else if (Kind == TEXT("empty"))
	{
		Added = Edit.ViewModel->AddEmptyEmitter();
	}
	else if (Kind == TEXT("minimal"))
	{
		Added = Edit.ViewModel->AddMinimalEmitter();
	}
	else if (Kind == TEXT("stateless"))
	{
		// "+ Emitter" > lightweight emitter: the default template, a spawn info and a sprite renderer.
		Added = Edit.ViewModel->AddStatelessEmitter();
	}
	else
	{
		return Source.Fail(TEXT("Graph.InvalidArgument"), TEXT("kind is \"asset\", \"empty\", \"minimal\" or \"stateless\"."), TEXT("kind"));
	}
	if (!Added.IsValid() || Added->GetEmitterHandle() == nullptr)
	{
		return Op.Fail(TEXT("Niagara.AddEmitterFailed"), FString::Printf(TEXT("The emitter '%s' could not be added."), *Name), TEXT("from"));
	}
	Edit.bChanged = true;
	Edit.Created.Add(Name);
	return RenameHandle(Op, Edit, *Added, Name, TEXT("name"));
}

bool DeleteEmitter(FReader& Op, FEdit& Edit)
{
	TSharedPtr<FNiagaraEmitterHandleViewModel> Handle;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name")}) || !RequireEmitter(Op, Edit, TEXT("name"), Handle))
	{
		return false;
	}
	TSet<FGuid> Ids;
	Ids.Add(Handle->GetId());
	Handle.Reset();
	Edit.ViewModel->DeleteEmitters(Ids);
	Edit.bChanged = true;
	return true;
}

bool RenameEmitter(FReader& Op, FEdit& Edit)
{
	TSharedPtr<FNiagaraEmitterHandleViewModel> Handle;
	FString To;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("to")}) || !RequireEmitter(Op, Edit, TEXT("name"), Handle) || !Op.String(TEXT("to"), To))
	{
		return false;
	}
	if (Handle->GetName().ToString() == To)
	{
		return true;
	}
	Edit.bChanged = true;
	return RenameHandle(Op, Edit, *Handle, To, TEXT("to"));
}

bool EditEmitter(FReader& Op, FEdit& Edit)
{
	TSharedPtr<FNiagaraEmitterHandleViewModel> Handle;
	if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("enabled"), TEXT("remove_parent"), TEXT("properties"), TEXT("reset")})
		|| !RequireEmitter(Op, Edit, TEXT("name"), Handle))
	{
		return false;
	}
	if (Op.Has(TEXT("enabled")))
	{
		bool bEnabled = true;
		if (!Op.Bool(TEXT("enabled"), bEnabled, true))
		{
			return false;
		}
		if (Handle->SetIsEnabled(bEnabled, false))
		{
			Edit.bChanged = true;
		}
	}
	bool bRemoveParent = false;
	if (!Op.Bool(TEXT("remove_parent"), bRemoveParent))
	{
		return false;
	}
	const bool bProperties = Op.Has(TEXT("properties")) || Op.Has(TEXT("reset"));
	if (bRemoveParent || bProperties)
	{
		if (!RequireStandard(Op, *Handle, TEXT("name")))
		{
			return false;
		}
	}
	if (bRemoveParent && Handle->GetEmitterViewModel()->GetParentEmitter().Emitter != nullptr)
	{
		Handle->GetEmitterViewModel()->RemoveParentEmitter();
		Edit.bChanged = true;
	}
	if (bProperties)
	{
		const FVersionedNiagaraEmitter Emitter = Handle->GetEmitterViewModel()->GetEmitter();
		FVersionedNiagaraEmitterData* Data = Emitter.GetEmitterData();
		if (Emitter.Emitter == nullptr || Data == nullptr)
		{
			return Op.Fail(TEXT("Niagara.EmitterNotFound"), TEXT("The emitter has no data."), TEXT("name"));
		}
		UNiagaraEmitter* Object = Emitter.Emitter;
		const FGuid Version = Emitter.Version;
		const FVersionedNiagaraEmitterData Defaults;
		if (!WriteProperties(Op, *FVersionedNiagaraEmitterData::StaticStruct(), Data, &Defaults, Object, {}, Edit.bChanged,
			[Object](FProperty&) { Object->Modify(); },
			[Object, Version](FProperty& Property)
			{
				FPropertyChangedEvent Event(&Property, EPropertyChangeType::ValueSet);
				Object->PostEditChangeVersionedProperty(Event, Version);
			}))
		{
			return false;
		}
	}
	return true;
}

// ---- renderers ------------------------------------------------------------------------------------------

UNiagaraStackRenderItemGroup* RenderGroup(FNiagaraEmitterHandleViewModel& Handle)
{
	UNiagaraStackViewModel* Stack = Handle.GetEmitterStackViewModel();
	UNiagaraStackEntry* Root = Stack != nullptr ? Stack->GetRootEntry() : nullptr;
	if (Root == nullptr)
	{
		return nullptr;
	}
	TArray<UNiagaraStackEntry*> Children;
	Root->GetUnfilteredChildren(Children);
	for (UNiagaraStackEntry* Child : Children)
	{
		if (UNiagaraStackRenderItemGroup* Group = Cast<UNiagaraStackRenderItemGroup>(Child))
		{
			return Group;
		}
	}
	return nullptr;
}

UNiagaraStackRendererItem* FindRendererItem(FNiagaraEmitterHandleViewModel& Handle, const FString& Name)
{
	if (UNiagaraStackRenderItemGroup* Group = RenderGroup(Handle))
	{
		Group->RefreshChildren();
	}
	TArray<UNiagaraStackEntry*> Entries;
	Handle.GetRendererEntries(Entries);
	for (UNiagaraStackEntry* Entry : Entries)
	{
		UNiagaraStackRendererItem* Item = Cast<UNiagaraStackRendererItem>(Entry);
		if (Item != nullptr && Item->GetRendererProperties() != nullptr && Item->GetRendererProperties()->GetName() == Name)
		{
			return Item;
		}
	}
	return nullptr;
}

bool RequireRenderer(FReader& Op, FNiagaraEmitterHandleViewModel& Handle, const FString& Name, const TCHAR* Field, UNiagaraStackRendererItem*& OutItem)
{
	OutItem = FindRendererItem(Handle, Name);
	if (OutItem == nullptr)
	{
		return Op.Fail(TEXT("Niagara.RendererNotFound"), FString::Printf(TEXT("The emitter '%s' has no renderer '%s'."),
			*Handle.GetName().ToString(), *Name), Field);
	}
	return true;
}

// A renderer class by short label (SpriteRenderer -> NiagaraSpriteRendererProperties) or class path.
UClass* RendererClass(const FString& Label)
{
	const FString Path = Label.StartsWith(TEXT("/")) ? Label : FString::Printf(TEXT("/Script/Niagara.Niagara%sProperties"), *Label);
	return FindObject<UClass>(nullptr, *Path);
}

bool AddRenderer(FReader& Op, FEdit& Edit)
{
	TSharedPtr<FNiagaraEmitterHandleViewModel> Handle;
	FString Name;
	FString ClassName;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("name"), TEXT("class")}) || !RequireEmitter(Op, Edit, TEXT("emitter"), Handle)
		|| !Op.String(TEXT("name"), Name) || !Op.String(TEXT("class"), ClassName) || !RequireRenderable(Op, *Handle, TEXT("emitter")))
	{
		return false;
	}
	if (!FName::IsValidXName(Name, INVALID_OBJECTNAME_CHARACTERS) || Name.Contains(TEXT(" ")))
	{
		return Op.Fail(TEXT("Niagara.InvalidRendererName"), FString::Printf(TEXT("'%s' is not a renderer object name."), *Name), TEXT("name"));
	}
	UObject* Outer = Handle->GetEmitterHandle()->GetEmitterMode() == ENiagaraEmitterMode::Stateless
		? static_cast<UObject*>(Handle->GetEmitterHandle()->GetEmitterBase()) : static_cast<UObject*>(Handle->GetEmitterViewModel()->GetEmitter().Emitter);
	if (Outer == nullptr)
	{
		return Op.Fail(TEXT("Niagara.EmitterNotFound"), TEXT("The emitter has no data."), TEXT("emitter"));
	}
	if (UObject* Existing = StaticFindObjectFast(UObject::StaticClass(), Outer, FName(*Name)))
	{
		return Op.Fail(TEXT("Niagara.RendererNameTaken"), FString::Printf(TEXT("The emitter already has an object named '%s' (%s)."), *Name,
			*Existing->GetClass()->GetName()), TEXT("name"));
	}
	UNiagaraStackRenderItemGroup* Group = RenderGroup(*Handle);
	INiagaraStackItemGroupAddUtilities* AddUtilities = Group != nullptr ? Group->GetAddUtilities() : nullptr;
	if (AddUtilities == nullptr)
	{
		return Op.Fail(TEXT("Niagara.ViewModelUnavailable"), TEXT("The emitter stack has no Render group."), TEXT("emitter"));
	}
	TArray<TSharedRef<INiagaraStackItemGroupAddAction>> Actions;
	AddUtilities->GenerateAddActions(Actions, FNiagaraStackItemGroupAddOptions());
	// The Render group's actions are the registered renderer creation infos (by display name).
	const UClass* Wanted = RendererClass(ClassName);
	TSharedPtr<INiagaraStackItemGroupAddAction> Chosen;
	TArray<FString> Offered;
	for (const FNiagaraRendererCreationInfo& Info : FNiagaraEditorModule::Get().GetRendererCreationInfos())
	{
		const UClass* InfoClass = FindObject<UClass>(nullptr, *Info.RendererClassPath.ToString());
		const TSharedRef<INiagaraStackItemGroupAddAction>* Action = Actions.FindByPredicate([&Info](const TSharedRef<INiagaraStackItemGroupAddAction>& Candidate)
		{
			return Candidate->GetDisplayName().EqualTo(Info.DisplayName);
		});
		if (InfoClass == nullptr || Action == nullptr)
		{
			continue;
		}
		Offered.Add(ClassLabel(InfoClass, TEXT("Niagara"), TEXT("Properties")));
		if (InfoClass == Wanted && !Chosen.IsValid())
		{
			Chosen = Action->ToSharedPtr();
		}
	}
	if (!Chosen.IsValid())
	{
		Offered.Sort();
		return Op.Fail(TEXT("Niagara.UnknownRendererClass"), FString::Printf(TEXT("'%s' is not a renderer the Render group offers. Classes: %s."),
			*ClassName, *FString::Join(Offered, TEXT(", "))), TEXT("class"));
	}
	const TArray<UNiagaraRendererProperties*> Before = EmitterRenderers(*Handle);
	AddUtilities->ExecuteAddAction(Chosen.ToSharedRef(), INDEX_NONE);
	UNiagaraRendererProperties* Added = nullptr;
	for (UNiagaraRendererProperties* Renderer : EmitterRenderers(*Handle))
	{
		if (Renderer != nullptr && !Before.Contains(Renderer))
		{
			Added = Renderer;
		}
	}
	if (Added == nullptr)
	{
		return Op.Fail(TEXT("Niagara.AddRendererFailed"), FString::Printf(TEXT("The %s renderer could not be added."), *ClassName), TEXT("class"));
	}
	Edit.bChanged = true;
	if (Added->GetName() != Name)
	{
		Added->Modify();
		Added->Rename(*Name, nullptr, REN_DontCreateRedirectors);
	}
	return true;
}

bool DeleteRenderer(FReader& Op, FEdit& Edit)
{
	TSharedPtr<FNiagaraEmitterHandleViewModel> Handle;
	FString Name;
	UNiagaraStackRendererItem* Item = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("name")}) || !RequireEmitter(Op, Edit, TEXT("emitter"), Handle)
		|| !Op.String(TEXT("name"), Name) || !RequireRenderable(Op, *Handle, TEXT("emitter")) || !RequireRenderer(Op, *Handle, Name, TEXT("name"), Item))
	{
		return false;
	}
	FText Message;
	if (!Item->TestCanDeleteWithMessage(Message))
	{
		return Op.Fail(Item->GetIsInherited() ? TEXT("Niagara.InheritedRenderer") : TEXT("Niagara.RendererNotDeletable"), FString::Printf(
			TEXT("The renderer '%s' cannot be deleted: %s"), *Name, *Message.ToString()), TEXT("name"));
	}
	const FScopedTransaction Transaction(Item->GetDeleteTransactionText());
	Item->Delete();
	Edit.bChanged = true;
	return true;
}

bool OrderRenderers(FReader& Op, FEdit& Edit)
{
	TSharedPtr<FNiagaraEmitterHandleViewModel> Handle;
	const FValue* Names = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("names")}) || !RequireEmitter(Op, Edit, TEXT("emitter"), Handle)
		|| !Op.Array(TEXT("names"), Names) || !RequireRenderable(Op, *Handle, TEXT("emitter")))
	{
		return false;
	}
	for (int32 Target = 0; Target < Names->Elements.Num(); ++Target)
	{
		const FValue& NameValue = Names->Elements[Target];
		const FString Where = FString::Printf(TEXT("names/%d"), Target);
		UNiagaraStackRendererItem* Item = nullptr;
		if (NameValue.Kind != EAssetDocumentRuntimeValueKind::String || !RequireRenderer(Op, *Handle, NameValue.StringValue, *Where, Item))
		{
			return NameValue.Kind == EAssetDocumentRuntimeValueKind::String ? false
				: Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("names lists renderer names."), *Where);
		}
		int32 Current = EmitterRenderers(*Handle).IndexOfByKey(Item->GetRendererProperties());
		while (Current > Target)
		{
			if (!Item->CanMoveRendererUp())
			{
				return Op.Fail(TEXT("Niagara.RendererOrder"), FString::Printf(
					TEXT("The renderer '%s' cannot move up (inherited renderers keep their place)."), *NameValue.StringValue), *Where);
			}
			Item->MoveRendererUp();
			Edit.bChanged = true;
			Current = EmitterRenderers(*Handle).IndexOfByKey(Item->GetRendererProperties());
		}
	}
	return true;
}

bool EditRenderer(FReader& Op, FEdit& Edit)
{
	TSharedPtr<FNiagaraEmitterHandleViewModel> Handle;
	FString Name;
	UNiagaraStackRendererItem* Item = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("name"), TEXT("enabled"), TEXT("properties"), TEXT("reset")}) || !RequireEmitter(Op, Edit, TEXT("emitter"), Handle)
		|| !Op.String(TEXT("name"), Name) || !RequireRenderable(Op, *Handle, TEXT("emitter")) || !RequireRenderer(Op, *Handle, Name, TEXT("name"), Item))
	{
		return false;
	}
	UNiagaraRendererProperties* Renderer = Item->GetRendererProperties();
	if (Op.Has(TEXT("enabled")))
	{
		bool bEnabled = true;
		if (!Op.Bool(TEXT("enabled"), bEnabled, true))
		{
			return false;
		}
		if (Renderer->GetIsEnabled() != bEnabled)
		{
			Item->SetIsEnabled(bEnabled);
			Edit.bChanged = true;
		}
	}
	if (Op.Has(TEXT("properties")) || Op.Has(TEXT("reset")))
	{
		const TArray<FName> Refused = {TEXT("bIsEnabled")};
		if (!WriteProperties(Op, *Renderer->GetClass(), Renderer, Renderer->GetClass()->GetDefaultObject(), Renderer, Refused, Edit.bChanged,
			[Renderer](FProperty& Property)
			{
				Renderer->Modify();
				Renderer->PreEditChange(&Property);
			},
			[Renderer](FProperty& Property)
			{
				FPropertyChangedEvent Event(&Property, EPropertyChangeType::ValueSet);
				Renderer->PostEditChangeProperty(Event);
			}))
		{
			return false;
		}
	}
	return true;
}

// ---- lightweight (stateless) emitters ---------------------------------------------------------------------
// UNiagaraStatelessEmitter and its modules are engine-internal (Niagara/Internal/Stateless): their Details are
// edited through reflection only (Modify, PreEditChange, write, PostEditChangeProperty, as the property editor
// does), which also runs the emitter's own reactions (a new EmitterTemplate rebuilds the module list).

bool RequireStateless(FReader& Op, FNiagaraEmitterHandleViewModel& Handle, const TCHAR* Field, UObject*& OutEmitter)
{
	const FNiagaraEmitterHandle* EmitterHandle = Handle.GetEmitterHandle();
	OutEmitter = EmitterHandle != nullptr && EmitterHandle->GetEmitterMode() == ENiagaraEmitterMode::Stateless ? EmitterHandle->GetEmitterBase() : nullptr;
	if (OutEmitter == nullptr)
	{
		return Op.Fail(TEXT("Niagara.ModeChange"), FString::Printf(TEXT(
			"'%s' is a standard emitter; Stateless values belong to lightweight emitters (an emitter's mode cannot change: delete and add it)."),
			*Handle.GetName().ToString()), Field);
	}
	return true;
}

// A Details write of one property of a reflected object.
template <typename FWrite>
void WriteObjectProperty(UObject& Object, FProperty& Property, FWrite&& Write)
{
	Object.Modify();
	Object.PreEditChange(&Property);
	Write();
	FPropertyChangedEvent Event(&Property, EPropertyChangeType::ValueSet);
	Object.PostEditChangeProperty(Event);
}

// SpawnInfos: each spawn info keeps, by position, the SourceId its stack entry is keyed by (not edit-visible and
// not in the document); new ones get a new id, as the spawn group's "+" (UNiagaraStatelessEmitter::AddSpawnInfo)
// gives them.
bool WriteSpawnInfos(FReader& Op, UObject& Emitter, bool& bOutChanged)
{
	FArrayProperty* Array = CastField<FArrayProperty>(Emitter.GetClass()->FindPropertyByName(TEXT("SpawnInfos")));
	const FStructProperty* Inner = Array != nullptr ? CastField<FStructProperty>(Array->Inner) : nullptr;
	const FStructProperty* SourceId = Inner != nullptr ? CastField<FStructProperty>(Inner->Struct->FindPropertyByName(TEXT("SourceId"))) : nullptr;
	if (SourceId == nullptr || SourceId->Struct != TBaseStructure<FGuid>::Get())
	{
		return Op.Fail(TEXT("Niagara.StatelessReflection"), TEXT(
			"UNiagaraStatelessEmitter::SpawnInfos is not the reflected TArray<FNiagaraStatelessSpawnInfo> with a SourceId this release knows."),
			TEXT("properties/SpawnInfos"));
	}
	void* Live = Array->ContainerPtrToValuePtr<void>(&Emitter);
	void* Copy = FMemory::Malloc(Array->GetSize(), Array->GetMinAlignment());
	Array->InitializeValue(Copy);
	Array->CopyCompleteValue(Copy, Live);
	bool bOk = true;
	const FValue* Properties = Op.Find(TEXT("properties"));
	const int32 Key = Properties != nullptr ? Properties->ObjectKeys.IndexOfByKey(TEXT("SpawnInfos")) : INDEX_NONE;
	if (Key != INDEX_NONE)
	{
		FString Code;
		bOk = DecodeProperty(*Array, Properties->Elements[Key], Copy, &Emitter, Code);
		if (!bOk)
		{
			Op.Fail(TEXT("Niagara.InvalidPropertyValue"), FString::Printf(TEXT("'SpawnInfos' does not fit the property (%s)."), *Code), TEXT("properties/SpawnInfos"));
		}
	}
	else
	{
		// reset: the class default (no spawn info).
		const void* Default = Array->ContainerPtrToValuePtr<void>(Emitter.GetClass()->GetDefaultObject());
		Array->CopyCompleteValue(Copy, Default);
	}
	if (bOk)
	{
		FScriptArrayHelper Before(Array, Live);
		FScriptArrayHelper After(Array, Copy);
		for (int32 Index = 0; Index < After.Num(); ++Index)
		{
			FGuid* Id = SourceId->ContainerPtrToValuePtr<FGuid>(After.GetRawPtr(Index));
			if (Index < Before.Num())
			{
				*Id = *SourceId->ContainerPtrToValuePtr<FGuid>(Before.GetRawPtr(Index));
			}
			if (!Id->IsValid())
			{
				*Id = FGuid::NewGuid();
			}
		}
		if (!Array->Identical(Copy, Live, PPF_None))
		{
			WriteObjectProperty(Emitter, *Array, [Array, Copy, Live]() { Array->CopyCompleteValue(Live, Copy); });
			bOutChanged = true;
		}
	}
	Array->DestroyValue(Copy);
	FMemory::Free(Copy);
	return bOk;
}

bool HasKey(const FValue* List, const TCHAR* Key)
{
	if (List == nullptr)
	{
		return false;
	}
	if (List->Kind == EAssetDocumentRuntimeValueKind::Object)
	{
		return List->ObjectKeys.Contains(Key);
	}
	return List->Kind == EAssetDocumentRuntimeValueKind::Array && List->Elements.ContainsByPredicate([Key](const FValue& Name)
	{
		return Name.Kind == EAssetDocumentRuntimeValueKind::String && Name.StringValue == Key;
	});
}

// {op: "stateless", emitter, properties?, reset?}: the lightweight emitter's Details (Emitter Properties, Emitter
// State, Spawn Info, Scalability); modules and renderers have their own ops.
bool EditStateless(FReader& Op, FEdit& Edit)
{
	TSharedPtr<FNiagaraEmitterHandleViewModel> Handle;
	UObject* Emitter = nullptr;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("properties"), TEXT("reset")}) || !RequireEmitter(Op, Edit, TEXT("emitter"), Handle)
		|| !RequireStateless(Op, *Handle, TEXT("emitter"), Emitter))
	{
		return false;
	}
	const TArray<FName> Refused = {TEXT("Modules"), TEXT("RendererProperties")};
	const TArray<FString> Skip = {TEXT("SpawnInfos")};
	if (!WriteProperties(Op, *Emitter->GetClass(), Emitter, Emitter->GetClass()->GetDefaultObject(), Emitter, Refused, Edit.bChanged,
		[Emitter](FProperty& Property)
		{
			Emitter->Modify();
			Emitter->PreEditChange(&Property);
		},
		[Emitter](FProperty& Property)
		{
			FPropertyChangedEvent Event(&Property, EPropertyChangeType::ValueSet);
			Emitter->PostEditChangeProperty(Event);
		}, Skip))
	{
		return false;
	}
	if (HasKey(Op.Find(TEXT("properties")), TEXT("SpawnInfos")) || HasKey(Op.Find(TEXT("reset")), TEXT("SpawnInfos")))
	{
		return WriteSpawnInfos(Op, *Emitter, Edit.bChanged);
	}
	return true;
}

// {op: "stateless_module", emitter, class, properties?, reset?}: one module of the lightweight emitter's template
// (one per class; "enabled" in the stack is its bModuleEnabled property).
bool EditStatelessModule(FReader& Op, FEdit& Edit)
{
	TSharedPtr<FNiagaraEmitterHandleViewModel> Handle;
	UObject* Emitter = nullptr;
	FString ClassName;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("class"), TEXT("properties"), TEXT("reset")}) || !RequireEmitter(Op, Edit, TEXT("emitter"), Handle)
		|| !RequireStateless(Op, *Handle, TEXT("emitter"), Emitter) || !Op.String(TEXT("class"), ClassName))
	{
		return false;
	}
	UObject* Module = nullptr;
	TArray<FString> Offered;
	for (UObject* Candidate : ReflectedObjectList(Emitter, TEXT("Modules")))
	{
		if (Candidate == nullptr)
		{
			continue;
		}
		const FString Label = ClassLabel(Candidate->GetClass(), TEXT("NiagaraStatelessModule_"), TEXT(""));
		Offered.Add(Label);
		if (Label == ClassName && Module == nullptr)
		{
			Module = Candidate;
		}
	}
	if (Module == nullptr)
	{
		return Op.Fail(TEXT("Niagara.StatelessModuleNotFound"), FString::Printf(TEXT(
			"The template of '%s' has no module '%s' (a lightweight emitter's modules come from its EmitterTemplate). Modules: %s."),
			*Handle->GetName().ToString(), *ClassName, *FString::Join(Offered, TEXT(", "))), TEXT("class"));
	}
	return WriteProperties(Op, *Module->GetClass(), Module, Module->GetClass()->GetDefaultObject(), Module, {}, Edit.bChanged,
		[Module](FProperty& Property)
		{
			Module->Modify();
			Module->PreEditChange(&Property);
		},
		[Module](FProperty& Property)
		{
			FPropertyChangedEvent Event(&Property, EPropertyChangeType::ValueSet);
			Module->PostEditChangeProperty(Event);
		});
}

// ---- parent version ---------------------------------------------------------------------------------------

// {op: "parent_version", emitter, version}: the emitter header's parent version menu
// (FNiagaraEditorUtilities::SwitchParentEmitterVersion, which merges the parent's version and runs its Python
// upgrade script). version "" = the parent's exposed version.
bool ParentVersion(FReader& Op, FEdit& Edit)
{
	TSharedPtr<FNiagaraEmitterHandleViewModel> Handle;
	FString Text;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("version")}) || !RequireEmitter(Op, Edit, TEXT("emitter"), Handle)
		|| !RequireStandard(Op, *Handle, TEXT("emitter")) || !Op.String(TEXT("version"), Text, true))
	{
		return false;
	}
	const TSharedRef<FNiagaraEmitterViewModel> EmitterViewModel = Handle->GetEmitterViewModel();
	const FVersionedNiagaraEmitter Parent = EmitterViewModel->GetParentEmitter();
	if (Parent.Emitter == nullptr || !Parent.Emitter->IsVersioningEnabled())
	{
		return Op.Fail(TEXT("Niagara.ParentNotVersioned"), FString::Printf(TEXT("'%s' has no versioned parent emitter."), *Handle->GetName().ToString()),
			TEXT("version"));
	}
	FGuid Version = Parent.Emitter->GetExposedVersion().VersionGuid;
	if (!Text.IsEmpty() && (!FGuid::Parse(Text, Version) || Parent.Emitter->GetEmitterData(Version) == nullptr))
	{
		return Op.Fail(TEXT("Niagara.EmitterVersionNotFound"), FString::Printf(TEXT("%s has no version '%s'."), *Parent.Emitter->GetPathName(), *Text),
			TEXT("version"));
	}
	if (Parent.Version == Version)
	{
		return true;
	}
	FNiagaraEditorUtilities::SwitchParentEmitterVersion(EmitterViewModel, Edit.ViewModel, Version);
	Edit.bChanged = true;
	if (Handle->GetEmitterViewModel()->GetParentEmitter().Version != Version)
	{
		return Op.Fail(TEXT("Niagara.ParentVersionFailed"), TEXT("The parent version did not change."), TEXT("version"));
	}
	return true;
}

FNiagaraSystemViewModelOptions EditOptions(const UNiagaraSystem& System)
{
	FNiagaraSystemViewModelOptions Options;
	Options.bCanModifyEmittersFromTimeline = false;
	Options.bCanSimulate = false;
	Options.bCanAutoCompile = false;
	Options.bIsForDataProcessingOnly = true;
	Options.EditMode = ENiagaraSystemViewModelEditMode::SystemAsset;
	Options.MessageLogGuid = System.GetAssetGuid();
	return Options;
}
} // namespace

bool ApplySystemEditOp(FReader& Op, const FString& Kind, UNiagaraSystem& System, const TSharedRef<FNiagaraSystemViewModel>& ViewModel,
	bool& bOutChanged, TArray<FString>& OutCreated)
{
	FEdit Edit{System, ViewModel};
	bool bOk = true;
	if (Kind == TEXT("add_emitter"))
	{
		bOk = AddEmitter(Op, Edit);
	}
	else if (Kind == TEXT("delete_emitter"))
	{
		bOk = DeleteEmitter(Op, Edit);
	}
	else if (Kind == TEXT("rename_emitter"))
	{
		bOk = RenameEmitter(Op, Edit);
	}
	else if (Kind == TEXT("emitter"))
	{
		bOk = EditEmitter(Op, Edit);
	}
	else if (Kind == TEXT("add_renderer"))
	{
		bOk = AddRenderer(Op, Edit);
	}
	else if (Kind == TEXT("delete_renderer"))
	{
		bOk = DeleteRenderer(Op, Edit);
	}
	else if (Kind == TEXT("order_renderers"))
	{
		bOk = OrderRenderers(Op, Edit);
	}
	else if (Kind == TEXT("renderer"))
	{
		bOk = EditRenderer(Op, Edit);
	}
	else if (Kind == TEXT("stateless"))
	{
		bOk = EditStateless(Op, Edit);
	}
	else if (Kind == TEXT("stateless_module"))
	{
		bOk = EditStatelessModule(Op, Edit);
	}
	else if (Kind == TEXT("parent_version"))
	{
		bOk = ParentVersion(Op, Edit);
	}
	else if (Kind == TEXT("add_user_parameter") || Kind == TEXT("delete_user_parameter") || Kind == TEXT("rename_user_parameter")
		|| Kind == TEXT("user_parameter") || Kind == TEXT("add_parameter") || Kind == TEXT("delete_parameter") || Kind == TEXT("parameter"))
	{
		bOk = ApplyUserParameterOp(Op, Kind, ViewModel.Get(), Edit.bChanged);
	}
	else if (IsStackOp(Kind))
	{
		bOk = ApplyStackOp(Op, Kind, ViewModel.Get(), Edit.bChanged);
	}
	else if (IsScriptOp(Kind))
	{
		bOk = ApplyScriptOp(Op, Kind, System, &ViewModel.Get(), Edit.bChanged);
	}
	else
	{
		bOk = Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT(
			"Unknown op '%s' (add_emitter, delete_emitter, rename_emitter, emitter, add_renderer, delete_renderer, order_renderers, "
			"renderer, stateless, stateless_module, parent_version, add_user_parameter, delete_user_parameter, rename_user_parameter, "
			"user_parameter, add_parameter, delete_parameter, parameter, add_module, add_assignment, delete_module, move_module, module, "
			"assignment, inputs, add_event_handler, delete_event_handler, event_handler, add_stage, delete_stage, move_stage, stage, "
			"add_scratch_pad, delete_scratch_pad, rename_scratch_pad, refresh_scratch_pad, scratch_pad_guids, script_settings, "
			"script_parameter, purge_parameters)."),
			*Kind), TEXT("op"));
	}
	bOutChanged = bOutChanged || Edit.bChanged;
	OutCreated.Append(Edit.Created);
	return bOk;
}

bool HandleEdit(FCall& Call, FObjectBuilder& Result)
{
	FCall& C = Call;
	const FValue* Ops = nullptr;
	if (!C.Args.OnlyFields({TEXT("ops")}) || !C.Args.Array(TEXT("ops"), Ops))
	{
		return false;
	}
	if (UNiagaraScript* Script = Cast<UNiagaraScript>(&C.Asset))
	{
		return HandleScriptEdit(C, *Script, *Ops, Result);
	}
	if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(&C.Asset))
	{
		return HandleEmitterEdit(C, *Emitter, *Ops, Result);
	}
	if (UNiagaraParameterCollection* Collection = Cast<UNiagaraParameterCollection>(&C.Asset))
	{
		return HandleCollectionEdit(C, *Collection, *Ops, Result);
	}
	UNiagaraSystem* System = Cast<UNiagaraSystem>(&C.Asset);
	if (System == nullptr)
	{
		return Fail(C, TEXT("Niagara.UnsupportedAsset"), FString::Printf(TEXT("%s is not a Niagara system, emitter, script or parameter collection."),
			*C.Asset.GetClass()->GetPathName()));
	}
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	const FScopedTransaction Transaction(LOCTEXT("EditNiagaraSystem", "Edit Niagara System"));
	System->Modify();
	// Settle the load-time compile, so that edits never race a running compile.
	System->WaitForCompilationComplete(false, false);
	FFullStackView FullStack;
	FullStack.Cover(*System);
	const TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeShared<FNiagaraSystemViewModel>();
	{
		// Without the Scratch Pad panel until an op uses it (FScratchPadsSetAside, EnsureScratchPadViewModels).
		FScratchPadsSetAside SetAside(&System->ScratchPadScripts);
		ViewModel->Initialize(*System, EditOptions(*System));
	}
	bool bChanged = false;
	TArray<FString> Created;
	bool bOk = true;
	FNiagaraDefaultInputReport DefaultInputs;
	for (int32 Index = 0; Index < Ops->Elements.Num() && bOk; ++Index)
	{
		FReader Op = C.Args.Child(FString::Printf(TEXT("ops/%d"), Index), Ops->Elements[Index]);
		FString Kind;
		if (!Op.IsObject() || !Op.String(TEXT("op"), Kind))
		{
			bOk = Op.IsObject() ? false : Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("Each op is an object."));
			break;
		}
		FullStack.Cover(*System); // an emitter the previous op added
		bOk = ApplySystemEditOp(Op, Kind, *System, ViewModel, bChanged, Created);
		if (bOk && NonInteractive.WasInteractionRefused())
		{
			bOk = Op.Fail(TEXT("Niagara.InteractiveEditRefused"), TEXT(
				"The edit tried to open an editor dialog, which an MCP request cannot answer; the edit was refused."));
		}
	}
	bOk = bOk && DefaultInputs.Finish(C.Args);
	// Details edits of renderers request a compile of their emitter's systems, as in the editor
	// (UNiagaraSystem::RequestCompileForEmitter); settle it so that nothing still holds the system when the call
	// returns (a rollback deletes or restores it).
	SettleCompilation(*System);
	if (!bOk)
	{
		NonInteractive.DiscardCreatedPackages();
		return false;
	}
	if (bChanged)
	{
		System->MarkPackageDirty();
	}
	Result.AddBool(TEXT("changed"), bChanged);
	Result.Add(TEXT("created"), Graph::MakeStringArray(Created));
	return true;
}
} // namespace UE::AssetDocument::Niagara

#undef LOCTEXT_NAMESPACE
