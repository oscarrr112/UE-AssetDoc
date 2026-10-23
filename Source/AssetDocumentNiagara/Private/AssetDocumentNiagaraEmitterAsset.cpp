// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Emitter assets: the Niagara emitter editor without its UI. Raw editor-equivalent code: the emitter editor's
// working copy (FNiagaraSystemToolkit::InitializeWithEmitter), its Apply (FNiagaraSystemToolkit::UpdateOriginalEmitter,
// NiagaraSystemToolkit.cpp:1148) and the Apply's follow-up on loaded assets (UpdateExistingEmitters), none of which is
// exported. Behaviour tests: UEAssetDoc.Niagara.EmitterAsset.*.
//
// fx.niagara.edit.v1 on an emitter asset (an emitter editor open on it is closed and reopened around the apply,
// AssetDocumentNiagaraOpenEditor.cpp). Ops run in order; the first refused op fails the call (Core rolls the package
// back):
//   {op: "check_dependents"}
//        refuses when a loaded asset that inherits from the emitter (a child emitter asset, or a system whose emitter
//        has it as parent, directly or through other emitters) has unsaved changes (Niagara.DependentHasUnsavedChanges).
//        Output "dependents": those assets' packages.
//   {op: "initialize_emitter", emitter: <emitter asset path>, version?: <guid>, inherit: bool}
//        a new asset (made by this apply) becomes what the New Niagara Emitter dialog makes from an existing emitter
//        (UNiagaraEmitterFactoryNew with EmitterToCopy): inherit = UNiagaraEmitter::CreateWithParentAndOwner, else a copy
//        (StaticDuplicateObject, SetUniqueEmitterName, DisableVersioning); then the factory's own settings (inheritable,
//        no description / category, the summary view state). The result replaces the asset in place, as Apply does.
//   {op: "versions", versions: [...]}        the version menu (ApplyVersions, AssetDocumentNiagaraScriptEdit.cpp)
//   {op: "update_dependents"}
//        UpdateExistingEmitters: loaded emitters inheriting from the asset merge its changes (MergeChangesFromParent,
//        recursively, those not synchronized with their parent), the systems using a merged emitter recompile and
//        their components reinitialize. Editors open on them follow, as they follow the emitter editor's Apply: the
//        working copy of an emitter editor open on an inheriting emitter merges too, and the view model of every editor
//        showing a system that uses a merged emitter (a system editor, or an emitter editor's placeholder) refreshes
//        (FNiagaraSystemViewModel::RefreshAll). Output "dependents": the packages changed.
//   every other op is a system-level op (AssetDocumentNiagaraEdit.cpp: emitter, renderers, parent_version; module
//        stacks; scratch pads) made on the emitter editor's working copy: a transient placeholder system whose only
//        emitter is a copy of the asset's exposed version, named by its unique emitter name (the handle name the ops
//        and reads use), with a view model in EmitterAsset mode (data processing, no compile, no simulation; the
//        Scratch Pad panel lists the emitter's scratch pads). Consecutive such ops share one working copy, which is
//        written back over the asset before the next asset-level op and at the end, as Apply does: StaticDuplicateObject
//        over the asset (same objects, same addresses; an asset this apply created is held by the apply, and the copy is
//        serialized into its object instead), RF_Standalone | RF_Public, ClearResolvedData on every script (engines that keep it there),
//        PostEditChange. Apply's "editing an exposed version" warning is the document's EditExposedVersion (the profile
//        refuses before any change), and no slow task or busy cursor is shown. System-only ops (emitters, user
//        parameters, lightweight emitters) are refused.
// Output {changed, created: [], dependents: [package]}.
//
// fx.niagara.compile.v1 on an emitter asset: the emitter editor's Compile and Apply: the working copy compiles
// through its placeholder system (RequestCompile, then SettleCompilation) and is written back. The "compile" read
// compiles a working copy without writing it back (the errors an apply compares against).

#include "AssetDocumentNiagaraPrimitives.h"

#include "AssetDocumentEditorNonInteractiveScope.h"
#include "AssetDocumentAssetCompilation.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/ITransaction.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterEditorData.h"
#include "NiagaraEditorModule.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScratchPadContainer.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemEditorData.h"
#include "NiagaraSystemFactoryNew.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectArray.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"

#define LOCTEXT_NAMESPACE "AssetDocumentNiagaraEmitterAsset"

namespace UE::AssetDocument::Niagara
{
namespace
{
using Graph::FReader;

const FName EmitterRebuilderId(TEXT("AssetDocumentNiagara.Emitter"));

FNiagaraSystemViewModelOptions EmitterEditorOptions(const UNiagaraSystem& Placeholder)
{
	FNiagaraSystemViewModelOptions Options;
	Options.bCanModifyEmittersFromTimeline = false;
	Options.bCanSimulate = false;
	Options.bCanAutoCompile = false;
	Options.bIsForDataProcessingOnly = true;
	Options.EditMode = ENiagaraSystemViewModelEditMode::EmitterAsset;
	Options.MessageLogGuid = Placeholder.GetAssetGuid();
	return Options;
}

UNiagaraEmitter* WorkingCopy(const UNiagaraSystem& Placeholder)
{
	return Placeholder.GetEmitterHandles().Num() == 1 ? Placeholder.GetEmitterHandles()[0].GetInstance().Emitter : nullptr;
}

// ---- the working copy and Apply ---------------------------------------------------------------------------------

struct FSession
{
	TStrongObjectPtr<UNiagaraSystem> Placeholder;
	TSharedPtr<FNiagaraSystemViewModel> ViewModel;
	FFullStackView FullStack;
	bool bChanged = false;
};

bool OpenSession(FReader& Op, UNiagaraEmitter& Emitter, FSession& OutSession)
{
	OutSession.Placeholder.Reset(MakeEmitterPlaceholder(Emitter));
	OutSession.FullStack.Cover(*OutSession.Placeholder);
	OutSession.ViewModel = MakeShared<FNiagaraSystemViewModel>();
	{
		// Without the Scratch Pad panel until an op uses it (FScratchPadsSetAside, EnsureScratchPadViewModels).
		FScratchPadsSetAside SetAside(PlaceholderPads(*OutSession.Placeholder));
		OutSession.ViewModel->Initialize(*OutSession.Placeholder, EmitterEditorOptions(*OutSession.Placeholder));
	}
	if (WorkingCopy(*OutSession.Placeholder) == nullptr || OutSession.ViewModel->GetEmitterHandleViewModels().Num() != 1)
	{
		return Op.Fail(TEXT("Niagara.ViewModelUnavailable"), FString::Printf(
			TEXT("The Niagara emitter editor's view model could not be built for %s."), *Emitter.GetPathName()));
	}
	return true;
}

// StaticDuplicateObject constructs each object of the copy over the asset's object at the same path, which must be of
// the same class (StaticAllocateObject cannot replace an object by one of another class): an asset object whose path
// the copy uses for an object of another class is moved out of the way first.
void SetAsideConflicts(UNiagaraEmitter& Original, UNiagaraEmitter& Copy)
{
	TArray<UObject*> CopyObjects;
	GetObjectsWithOuter(&Copy, CopyObjects);
	TArray<UObject*> Conflicts;
	for (const UObject* Object : CopyObjects)
	{
		TArray<FName> Names;
		for (const UObject* Outer = Object; Outer != nullptr && Outer != &Copy; Outer = Outer->GetOuter())
		{
			Names.Insert(Outer->GetFName(), 0);
		}
		UObject* Existing = &Original;
		for (const FName& Name : Names)
		{
			Existing = Existing != nullptr ? StaticFindObjectFast(UObject::StaticClass(), Existing, Name) : nullptr;
		}
		if (Existing != nullptr && Existing != &Original && Existing->GetClass() != Object->GetClass())
		{
			Conflicts.AddUnique(Existing);
		}
	}
	for (UObject* Conflict : Conflicts)
	{
		if (Conflict->GetOutermost() == Original.GetOutermost())
		{
			Conflict->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
		}
	}
}

// StaticDuplicateObject destroys the asset object and constructs the copy over the same memory, which it cannot do while
// something holds the object by reference count (a TStrongObjectPtr: an apply that created the asset holds it to undo the
// creation). Such an asset object stays as it is and the copy is serialized into it instead: its properties first go back
// to the class defaults (as a new object's are), the duplication is seeded with it, and it gets the PostDuplicate and
// PostLoad a duplicated root gets. Its subobjects are constructed over the asset's as before.
bool IsReferenceCounted(const UObject& Object)
{
	const FUObjectItem* Item = GUObjectArray.ObjectToObjectItem(&Object);
	return Item != nullptr && Item->GetRefCount() > 0;
}

UNiagaraEmitter* DuplicateOver(UNiagaraEmitter& Original, UNiagaraEmitter& Copy)
{
	if (!IsReferenceCounted(Original))
	{
		return Cast<UNiagaraEmitter>(StaticDuplicateObject(&Copy, Original.GetOuter(), Original.GetFName(), RF_AllFlags, Original.GetClass()));
	}
	const UObject* Defaults = Original.GetClass()->GetDefaultObject();
	for (TFieldIterator<FProperty> It(Original.GetClass()); It; ++It)
	{
		It->CopyCompleteValue_InContainer(&Original, Defaults);
	}
	FObjectDuplicationParameters Parameters = InitStaticDuplicateObjectParams(&Copy, Original.GetOuter(), Original.GetFName(), RF_AllFlags, Original.GetClass());
	Parameters.DuplicationSeed.Add(&Copy, &Original);
	StaticDuplicateObjectEx(Parameters);
	Original.PostDuplicate(Parameters.DuplicateMode);
	Original.ConditionalPostLoad();
	return &Original;
}

// FNiagaraSystemToolkit::UpdateOriginalEmitter: the working copy overwrites the asset in place.
void WriteBack(UNiagaraEmitter& Original, UNiagaraEmitter& Copy)
{
	if (GEditor != nullptr && Original.IsSelected())
	{
		GEditor->GetSelectedObjects()->Deselect(&Original);
	}
	AssetDocumentAssetCompilation::FinishPackageCompilation({Original.GetOutermost()});
	ResetLoaders(Original.GetOutermost());
	SetAsideConflicts(Original, Copy);
	Original.PreEditChange(nullptr);
	// The editor's Apply is not a transaction: the asset's objects destroyed under the copy must not be recorded (a
	// node's destruction breaks its links, and recording that serializes pins of nodes already destroyed).
	UNiagaraEmitter* Written = nullptr;
	{
		TGuardValue<ITransaction*> NoTransaction(GUndo, nullptr);
		Written = DuplicateOver(Original, Copy);
	}
	check(Written == &Original);
	Written->SetFlags(RF_Standalone | RF_Public);
#if !UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	// The scripts' resolved data references the placeholder system. Newer engines keep resolved data out of the
	// script, so there is nothing to clear there.
	Written->ForEachVersionData([](const FVersionedNiagaraEmitterData& Data)
	{
		TArray<UNiagaraScript*> Scripts;
		Data.GetScripts(Scripts, true);
		for (UNiagaraScript* Script : Scripts)
		{
			Script->ClearResolvedData();
		}
	});
#endif
	Written->PostEditChange();
	Written->MarkPackageDirty();
}

// Ends the working copy: a compile its edits requested settles (nothing may hold the placeholder when the call
// returns), then the copy is written back when an op changed it.
void CloseSession(UNiagaraEmitter& Emitter, FSession& Session, bool bWriteBack)
{
	if (!Session.Placeholder.IsValid())
	{
		return;
	}
	SettleCompilation(*Session.Placeholder);
	// The working copy's Summary View toggle is the asset's again before it is written back.
	Session.FullStack.Restore();
	UNiagaraEmitter* Copy = WorkingCopy(*Session.Placeholder);
	if (bWriteBack && Session.bChanged && Copy != nullptr)
	{
		WriteBack(Emitter, *Copy);
	}
	Session.ViewModel.Reset();
	Session.Placeholder.Reset();
	Session.bChanged = false;
}

// ---- dependents -------------------------------------------------------------------------------------------------

// Transient emitters and systems (the working copies of open editors, placeholders of reads and edits) are no one's
// content. (The copies an apply's save stages and verifies are collected when that apply returns.)
bool IsLoadedContent(const UObject& Object)
{
	return IsValid(&Object) && !Object.HasAnyFlags(RF_ClassDefaultObject) && Object.GetOutermost() != GetTransientPackage();
}

// Loaded emitters by the parent of any of their versions (UpdateExistingEmitters' reference map), content only.
TMap<UNiagaraEmitter*, TArray<UNiagaraEmitter*>> ChildrenByParent()
{
	TMap<UNiagaraEmitter*, TArray<UNiagaraEmitter*>> Children;
	for (TObjectIterator<UNiagaraEmitter> It; It; ++It)
	{
		UNiagaraEmitter* Loaded = *It;
		if (Loaded == nullptr || !IsLoadedContent(*Loaded))
		{
			continue;
		}
		for (const FNiagaraAssetVersion& Version : Loaded->GetAllAvailableVersions())
		{
			const FVersionedNiagaraEmitterData* Data = Loaded->GetEmitterData(Version.VersionGuid);
			UNiagaraEmitter* Parent = Data != nullptr ? Data->GetParent().Emitter : nullptr;
			if (Parent != nullptr)
			{
				Children.FindOrAdd(Parent).AddUnique(Loaded);
			}
		}
	}
	return Children;
}

TArray<UNiagaraEmitter*> Descendants(UNiagaraEmitter& Emitter)
{
	const TMap<UNiagaraEmitter*, TArray<UNiagaraEmitter*>> Children = ChildrenByParent();
	TArray<UNiagaraEmitter*> Out;
	TArray<UNiagaraEmitter*> Pending = {&Emitter};
	while (!Pending.IsEmpty())
	{
		UNiagaraEmitter* Current = Pending.Pop();
		if (const TArray<UNiagaraEmitter*>* Found = Children.Find(Current))
		{
			for (UNiagaraEmitter* Child : *Found)
			{
				if (Child != &Emitter && !Out.Contains(Child))
				{
					Out.Add(Child);
					Pending.Add(Child);
				}
			}
		}
	}
	return Out;
}

bool CheckDependents(FReader& Op, UNiagaraEmitter& Emitter, TArray<FString>& OutDependents)
{
	TArray<UPackage*> Packages;
	for (const UNiagaraEmitter* Child : Descendants(Emitter))
	{
		if (Child->GetOutermost() != Emitter.GetOutermost())
		{
			Packages.AddUnique(Child->GetOutermost());
		}
	}
	TArray<FString> Dirty;
	for (const UPackage* Package : Packages)
	{
		OutDependents.AddUnique(Package->GetName());
		if (Package->IsDirty())
		{
			Dirty.Add(Package->GetName());
		}
	}
	OutDependents.Sort();
	if (!Dirty.IsEmpty())
	{
		Dirty.Sort();
		return Op.Fail(TEXT("Niagara.DependentHasUnsavedChanges"), FString::Printf(TEXT(
			"Assets that inherit from %s have unsaved changes: %s. Applying the emitter merges its changes into them and "
			"recompiles them, and a rollback could not give their unsaved state back; save or discard them first."),
			*Emitter.GetName(), *FString::Join(Dirty, TEXT(", "))));
	}
	return true;
}

void MergeRecursively(const UNiagaraEmitter* Changed, const TMap<UNiagaraEmitter*, TArray<UNiagaraEmitter*>>& Children,
	TSet<UNiagaraEmitter*>& OutMerged)
{
	const TArray<UNiagaraEmitter*>* Found = Children.Find(Changed);
	if (Found == nullptr)
	{
		return;
	}
	for (UNiagaraEmitter* Child : *Found)
	{
		if (!Child->IsSynchronizedWithParent() && !OutMerged.Contains(Child))
		{
			Child->MergeChangesFromParent();
			OutMerged.Add(Child);
			MergeRecursively(Child, Children, OutMerged);
		}
	}
}

// (New helpers are named per file: unity builds join this file's anonymous namespace with others'.)
namespace AssetDocumentNiagaraEmitterAssetLocal
{
// The view model of the editor showing System (a system editor, or an emitter editor's placeholder system), or null.
TSharedPtr<FNiagaraSystemViewModel> EditorViewModelOf(UNiagaraSystem& System)
{
	return FNiagaraEditorModule::Get().GetExistingViewModelForSystem(&System);
}

// The working copies of the emitter editors open on emitters that inherit from a merged emitter (the editable copy in
// the editor's transient placeholder system, which UpdateExistingEmitters merges with every other loaded emitter) merge
// too, recursively.
void MergeOpenEditorCopies(TSet<UNiagaraEmitter*>& Merged)
{
	for (bool bMergedMore = true; bMergedMore;)
	{
		bMergedMore = false;
		for (TObjectIterator<UNiagaraEmitter> It; It; ++It)
		{
			UNiagaraEmitter* Copy = *It;
			UNiagaraSystem* Placeholder = Copy != nullptr && IsValid(Copy) && !IsLoadedContent(*Copy) ? Copy->GetTypedOuter<UNiagaraSystem>() : nullptr;
			if (Placeholder == nullptr || Merged.Contains(Copy) || !EditorViewModelOf(*Placeholder).IsValid())
			{
				continue;
			}
			bool bInherits = false;
			for (const FNiagaraAssetVersion& Version : Copy->GetAllAvailableVersions())
			{
				const FVersionedNiagaraEmitterData* Data = Copy->GetEmitterData(Version.VersionGuid);
				bInherits = bInherits || (Data != nullptr && Merged.Contains(Data->GetParent().Emitter));
			}
			if (bInherits && !Copy->IsSynchronizedWithParent())
			{
				Copy->MergeChangesFromParent();
				Merged.Add(Copy);
				bMergedMore = true;
			}
		}
	}
}
} // namespace AssetDocumentNiagaraEmitterAssetLocal

// FNiagaraSystemToolkit::UpdateExistingEmitters.
void UpdateDependents(UNiagaraEmitter& Emitter, TArray<FString>& OutDependents)
{
	using namespace AssetDocumentNiagaraEmitterAssetLocal;
	TSet<UNiagaraEmitter*> Merged;
	MergeRecursively(&Emitter, ChildrenByParent(), Merged);
	Merged.Add(&Emitter);
	MergeOpenEditorCopies(Merged);
	Merged.Remove(&Emitter);
	for (const UNiagaraEmitter* Child : Merged)
	{
		if (Child->GetOutermost() != Emitter.GetOutermost() && IsLoadedContent(*Child))
		{
			OutDependents.AddUnique(Child->GetOutermost()->GetName());
		}
	}
	for (TObjectIterator<UNiagaraSystem> It; It; ++It)
	{
		UNiagaraSystem* Loaded = *It;
		const TSharedPtr<FNiagaraSystemViewModel> Editor = Loaded != nullptr && IsValid(Loaded) ? EditorViewModelOf(*Loaded) : nullptr;
		if (Loaded == nullptr || (!IsLoadedContent(*Loaded) && !Editor.IsValid()))
		{
			continue;
		}
		bool bUsesMerged = false;
		for (const FNiagaraEmitterHandle& Handle : Loaded->GetEmitterHandles())
		{
			bUsesMerged = bUsesMerged || Merged.Contains(Handle.GetInstance().Emitter);
		}
		if (!bUsesMerged)
		{
			continue;
		}
		Loaded->RequestCompile(false);
		SettleCompilation(*Loaded);
		if (Editor.IsValid())
		{
			// "Invalidate any view models": the editor rebuilds its emitter handles, stacks and tracks from the system.
			Editor->RefreshAll();
		}
		if (!IsLoadedContent(*Loaded))
		{
			continue;
		}
		for (TObjectIterator<UNiagaraComponent> Component; Component; ++Component)
		{
			if (IsValid(*Component) && Component->GetAsset() == Loaded)
			{
				Component->ReinitializeSystem();
			}
		}
		OutDependents.AddUnique(Loaded->GetOutermost()->GetName());
	}
	OutDependents.Sort();
}

// ---- initialize ------------------------------------------------------------------------------------------------

bool InitializeEmitter(FReader& Op, UNiagaraEmitter& Emitter, bool& bOutChanged)
{
	FString Path;
	FString VersionText;
	bool bInherit = false;
	if (!Op.OnlyFields({TEXT("op"), TEXT("emitter"), TEXT("version"), TEXT("inherit")}) || !Op.String(TEXT("emitter"), Path)
		|| !Op.String(TEXT("version"), VersionText, false) || !Op.Bool(TEXT("inherit"), bInherit))
	{
		return false;
	}
	if (!Emitter.GetOutermost()->HasAnyPackageFlags(PKG_NewlyCreated))
	{
		return Op.Fail(TEXT("Niagara.ParentImmutable"), FString::Printf(TEXT(
			"%s already exists: an emitter asset is made from another emitter (From, Parent) only when the apply creates it."),
			*Emitter.GetName()), TEXT("emitter"));
	}
	UNiagaraEmitter* Template = LoadObject<UNiagaraEmitter>(nullptr, *Path, nullptr, LOAD_NoWarn);
	if (Template == nullptr || Template == &Emitter)
	{
		return Op.Fail(TEXT("Niagara.EmitterAssetNotFound"), FString::Printf(TEXT("There is no other emitter asset '%s'."), *Path), TEXT("emitter"));
	}
	FGuid Version = Template->IsVersioningEnabled() ? Template->GetExposedVersion().VersionGuid : FGuid();
	if (!VersionText.IsEmpty() && (!FGuid::Parse(VersionText, Version) || Template->GetEmitterData(Version) == nullptr))
	{
		return Op.Fail(TEXT("Niagara.EmitterVersionNotFound"), FString::Printf(TEXT("%s has no version '%s'."), *Template->GetPathName(), *VersionText),
			TEXT("version"));
	}
	if (bInherit && !Template->bIsInheritable)
	{
		return Op.Fail(TEXT("Niagara.NotInheritable"), FString::Printf(TEXT(
			"%s is not inheritable (its Asset Options): copy it (From) instead of inheriting from it (Parent)."), *Template->GetPathName()),
			TEXT("emitter"));
	}
	// UNiagaraEmitterFactoryNew::FactoryCreateNew with EmitterToCopy, made in the transient package and written over the
	// asset (which keeps its identity: the apply's handles hold it).
	const FName Name = MakeUniqueObjectName(GetTransientPackage(), UNiagaraEmitter::StaticClass(), Emitter.GetFName());
	UNiagaraEmitter* Created = nullptr;
	if (bInherit)
	{
		Created = UNiagaraEmitter::CreateWithParentAndOwner(FVersionedNiagaraEmitter(Template, Version), GetTransientPackage(), Name, RF_Transactional);
	}
	else
	{
		Created = Cast<UNiagaraEmitter>(StaticDuplicateObject(Template, GetTransientPackage(), Name, RF_Transactional, UNiagaraEmitter::StaticClass()));
		if (Created != nullptr)
		{
			Created->DisableVersioning(Version);
		}
	}
	if (Created == nullptr)
	{
		return Op.Fail(TEXT("Niagara.EmitterAssetNotFound"), FString::Printf(TEXT("%s could not be copied."), *Template->GetPathName()), TEXT("emitter"));
	}
	Created->SetUniqueEmitterName(Emitter.GetName());
	Created->bIsInheritable = true;
	Created->TemplateAssetDescription = FText();
	Created->Category = FText();
	Created->ForEachVersionData([](FVersionedNiagaraEmitterData& Data)
	{
		if (UNiagaraEmitterEditorData* EditorData = Cast<UNiagaraEmitterEditorData>(Data.GetEditorData()))
		{
			EditorData->SetShowSummaryView(Data.AddEmitterDefaultViewState == ENiagaraEmitterDefaultSummaryState::Summary);
		}
	});
	WriteBack(Emitter, *Created);
	Created->ClearFlags(RF_Standalone | RF_Public);
	Created->MarkAsGarbage();
	bOutChanged = true;
	return true;
}

// Ops of the system editor that the emitter editor does not have.
bool IsSystemOnlyOp(const FString& Kind)
{
	static const TSet<FString> Kinds = {TEXT("add_emitter"), TEXT("delete_emitter"), TEXT("rename_emitter"), TEXT("stateless"),
		TEXT("stateless_module"), TEXT("add_user_parameter"), TEXT("delete_user_parameter"), TEXT("rename_user_parameter"),
		TEXT("user_parameter"), TEXT("add_parameter"), TEXT("delete_parameter"), TEXT("parameter")};
	return Kinds.Contains(Kind);
}

// ---- compile ----------------------------------------------------------------------------------------------------

void AddStatus(const UNiagaraSystem& Placeholder, FObjectBuilder& Result)
{
	TArray<FCompileMessage> Messages;
	const FString Status = ReadCompileStatus(Placeholder, Messages);
	TArray<FString> Errors;
	TArray<FString> Warnings;
	for (const FCompileMessage& Message : Messages)
	{
		(Message.Severity == TEXT("Error") ? Errors : Warnings).AddUnique(Message.Message);
	}
	Result.AddString(TEXT("status"), Status);
	Result.Add(TEXT("errors"), Graph::MakeStringArray(Errors));
	Result.Add(TEXT("warnings"), Graph::MakeStringArray(Warnings));
}

class FEmitterRebuilder final : public IAssetDocumentDerivedStateRebuilder
{
public:
	virtual FName GetId() const override { return EmitterRebuilderId; }
	virtual bool Supports(const UObject& PrimaryAsset) const override { return PrimaryAsset.IsA<UNiagaraEmitter>(); }
	virtual EAssetDocumentSnapshotObjectRole Classify(const UObject& Object) const override
	{
		return IsDerivedNiagaraObject(Object) ? EAssetDocumentSnapshotObjectRole::Derived : IAssetDocumentDerivedStateRebuilder::Classify(Object);
	}
	// The restored emitter's loaded children merge it again (they merged the rolled-back state when the apply got that
	// far), and the systems using them recompile.
	virtual FAssetDocumentContractValidationResult Rebuild(UObject& PrimaryAsset) override
	{
		FAssetDocumentContractValidationResult Result;
		if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(&PrimaryAsset))
		{
			FAssetDocumentEditorNonInteractiveScope NonInteractive;
			TArray<FString> Dependents;
			UpdateDependents(*Emitter, Dependents);
		}
		Result.bValid = true;
		return Result;
	}
};
} // namespace

UNiagaraSystem* MakeEmitterPlaceholder(UNiagaraEmitter& Emitter)
{
	// FNiagaraSystemToolkit::InitializeWithEmitter: a transient placeholder system holding a copy of the emitter. The copy
	// is made directly, as AddEmitterToSystem makes it for a placeholder (FNiagaraSystemViewModel::AddEmitter would also
	// record the emitter in the editor's recently-used list).
	UNiagaraSystem* Placeholder = NewObject<UNiagaraSystem>(GetTransientPackage(), NAME_None, RF_Transient | RF_Transactional);
	UNiagaraSystemFactoryNew::InitializeSystem(Placeholder, true);
	if (UNiagaraSystemEditorData* PlaceholderData = Cast<UNiagaraSystemEditorData>(Placeholder->GetEditorData()))
	{
		PlaceholderData->SetOwningSystemIsPlaceholder(true, *Placeholder);
	}
	const FGuid Version = Emitter.IsVersioningEnabled() ? Emitter.GetExposedVersion().VersionGuid : FGuid();
	Placeholder->DuplicateEmitterHandle(FNiagaraEmitterHandle(Emitter, Version), FName(*Emitter.GetUniqueEmitterName()));
	return Placeholder;
}

TArray<TObjectPtr<UNiagaraScript>>* PlaceholderPads(UNiagaraSystem& Placeholder)
{
	FVersionedNiagaraEmitterData* Data = Placeholder.GetEmitterHandles().Num() == 1 ? Placeholder.GetEmitterHandles()[0].GetEmitterData() : nullptr;
	return Data != nullptr && Data->ScratchPads != nullptr ? &Data->ScratchPads->Scripts : nullptr;
}

bool HandleEmitterEdit(FCall& Call, UNiagaraEmitter& Emitter, const FValue& Ops, FObjectBuilder& Result)
{
	FCall& C = Call;
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	const FScopedTransaction Transaction(LOCTEXT("EditNiagaraEmitter", "Edit Niagara Emitter"));
	Emitter.Modify();
	FSession Session;
	bool bChanged = false;
	TArray<FString> Dependents;
	TArray<FString> Created;
	bool bOk = true;
	FNiagaraDefaultInputReport DefaultInputs;
	for (int32 Index = 0; Index < Ops.Elements.Num() && bOk; ++Index)
	{
		FReader Op = C.Args.Child(FString::Printf(TEXT("ops/%d"), Index), Ops.Elements[Index]);
		FString Kind;
		if (!Op.IsObject() || !Op.String(TEXT("op"), Kind))
		{
			bOk = Op.IsObject() ? false : Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("Each op is an object."));
			break;
		}
		const bool bAssetOp = Kind == TEXT("check_dependents") || Kind == TEXT("initialize_emitter") || Kind == TEXT("versions")
			|| Kind == TEXT("update_dependents");
		if (bAssetOp)
		{
			// Apply before the asset itself changes.
			CloseSession(Emitter, Session, true);
		}
		if (Kind == TEXT("check_dependents"))
		{
			bOk = Op.OnlyFields({TEXT("op")}) && CheckDependents(Op, Emitter, Dependents);
		}
		else if (Kind == TEXT("initialize_emitter"))
		{
			bOk = InitializeEmitter(Op, Emitter, bChanged);
		}
		else if (Kind == TEXT("versions"))
		{
			bOk = ApplyVersions(Op, Emitter, Emitter, bChanged);
		}
		else if (Kind == TEXT("update_dependents"))
		{
			bOk = Op.OnlyFields({TEXT("op")});
			if (bOk)
			{
				UpdateDependents(Emitter, Dependents);
			}
		}
		else if (IsSystemOnlyOp(Kind))
		{
			bOk = Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT(
				"'%s' edits a system; an emitter asset has no emitter handles, user parameters or lightweight emitters."), *Kind), TEXT("op"));
		}
		else
		{
			bOk = Session.Placeholder.IsValid() || OpenSession(Op, Emitter, Session);
			if (bOk)
			{
				bOk = ApplySystemEditOp(Op, Kind, *Session.Placeholder, Session.ViewModel.ToSharedRef(), Session.bChanged, Created);
				bChanged = bChanged || Session.bChanged;
			}
		}
		if (bOk && NonInteractive.WasInteractionRefused())
		{
			bOk = Op.Fail(TEXT("Niagara.InteractiveEditRefused"), TEXT(
				"The edit tried to open an editor dialog, which an MCP request cannot answer; the edit was refused."));
		}
	}
	bOk = bOk && DefaultInputs.Finish(C.Args);
	CloseSession(Emitter, Session, bOk);
	if (!bOk)
	{
		NonInteractive.DiscardCreatedPackages();
		return false;
	}
	if (bChanged)
	{
		Emitter.MarkPackageDirty();
	}
	Dependents.Sort();
	Result.AddBool(TEXT("changed"), bChanged);
	Result.Add(TEXT("created"), Graph::MakeArray({}));
	Result.Add(TEXT("dependents"), Graph::MakeStringArray(Dependents));
	return true;
}

bool CompileEmitter(FCall& Call, UNiagaraEmitter& Emitter, FObjectBuilder& Result)
{
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	TStrongObjectPtr<UNiagaraSystem> Placeholder(MakeEmitterPlaceholder(Emitter));
	// The copy's graphs recompute their compile ids (ForceGraphToRecompileOnNextCheck): the cached ids came with the
	// copy and can predate the edits written back into the asset; unchanged scripts still come from the DDC.
	Placeholder->RequestCompile(true);
	const bool bDone = SettleCompilation(*Placeholder);
	AddStatus(*Placeholder, Result);
	if (NonInteractive.WasInteractionRefused())
	{
		NonInteractive.DiscardCreatedPackages();
		return Call.Args.Fail(TEXT("Niagara.InteractiveEditRefused"), TEXT(
			"Compiling tried to open an editor dialog, which an MCP request cannot answer."));
	}
	// The emitter editor's Apply after its compile: the asset holds the compiled scripts.
	if (UNiagaraEmitter* Copy = WorkingCopy(*Placeholder.Get()))
	{
		WriteBack(Emitter, *Copy);
	}
	Result.AddBool(TEXT("timed_out"), !bDone);
	return true;
}

bool ReadEmitterCompile(FCall& Call, UNiagaraEmitter& Emitter, FObjectBuilder& Result)
{
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	TStrongObjectPtr<UNiagaraSystem> Placeholder(MakeEmitterPlaceholder(Emitter));
	Placeholder->RequestCompile(true);
	SettleCompilation(*Placeholder);
	Result.AddString(TEXT("class"), TEXT("emitter"));
	AddStatus(*Placeholder, Result);
	if (NonInteractive.WasInteractionRefused())
	{
		return Fail(Call, TEXT("Niagara.InteractiveReadRefused"), FString::Printf(
			TEXT("Compiling %s tried to open an editor dialog, which an MCP request cannot answer; the read was refused."), *Emitter.GetPathName()));
	}
	return true;
}

TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeEmitterRebuilder()
{
	return MakeShared<FEmitterRebuilder>();
}
} // namespace UE::AssetDocument::Niagara

#undef LOCTEXT_NAMESPACE
