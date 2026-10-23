// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentAssetDelete.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequenceBase.h"
#include "AssetDeleteModel.h"
#include "AssetDocumentAssetCompilation.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/WeakObjectPtrTemplates.h"

FString FAssetDocumentDeleteBlocker::Describe() const
{
	if (Kind == TEXT("subobject"))
	{
		return FString::Printf(TEXT("its subobject %s is kept alive by its own flags"), *Referencer);
	}
	if (Kind == TEXT("registry"))
	{
		return TEXT("the asset registry is still discovering assets");
	}
	const TCHAR* InBatch = bRefusedInBatch ? TEXT(" (in this delete too, but refused itself)") : TEXT("");
	if (Kind == TEXT("package"))
	{
		return FString::Printf(TEXT("package %s references it (asset registry)%s"), *Referencer, InBatch);
	}
	return FString::Printf(TEXT("%s references it%s%s"), *Referencer,
		Properties.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *FString::Join(Properties, TEXT(", "))), InBatch);
}

namespace AssetDocumentAssetDeletePrivate
{
// The blockers of each asset of a delete batch (FAssetDeleteModel over the whole batch, as the Delete Assets
// dialog with that selection): references from inside the batch do not count. Only blocked assets are listed.
TMap<UObject*, TArray<FAssetDocumentDeleteBlocker>> FindBatchBlockers(const TArray<UObject*>& Assets)
{
	TMap<UObject*, TArray<FAssetDocumentDeleteBlocker>> Blocked;
	if (IAssetRegistry::GetChecked().IsLoadingAssets())
	{
		for (UObject* Asset : Assets)
		{
			Blocked.FindOrAdd(Asset).Add({TEXT("registry"), FString(), {}});
		}
	}
	FAssetDeleteModel Model(Assets);
	for (int32 Ticks = 0; Model.GetState() != FAssetDeleteModel::Finished && Ticks < 1000; ++Ticks)
	{
		Model.Tick(0.f);
	}
	for (const TSharedPtr<FPendingDelete>& Pending : *Model.GetPendingDeletedAssets())
	{
		TArray<FAssetDocumentDeleteBlocker> Blockers;
		for (const FName& Referencer : Pending->DiskReferences)
		{
			if (!Model.IsAssetInPendingDeletes(Referencer))
			{
				Blockers.Add({TEXT("package"), Referencer.ToString(), {}});
			}
		}
		for (const FReferencerInformation& Reference : Pending->MemoryReferences.ExternalReferences)
		{
			if (Reference.Referencer == nullptr || Model.IsObjectInPendingDeletes(Reference.Referencer))
			{
				continue;
			}
			FAssetDocumentDeleteBlocker& Blocker = Blockers.Add_GetRef({TEXT("object"), Reference.Referencer->GetFullName(), {}});
			for (const FProperty* Property : Reference.ReferencingProperties)
			{
				Blocker.Properties.Add(Property != nullptr ? Property->GetName() : FString());
			}
		}
		for (const FReferencerInformation& Reference : Pending->MemoryReferences.InternalReferences)
		{
			if (Reference.Referencer != nullptr && (Reference.Referencer->HasAnyFlags(GARBAGE_COLLECTION_KEEPFLAGS)
				|| Reference.Referencer->HasAnyInternalFlags(EInternalObjectFlags_GarbageCollectionKeepFlags)))
			{
				Blockers.Add({TEXT("subobject"), Reference.Referencer->GetFullName(), {}});
			}
		}
		if (!Blockers.IsEmpty() && Pending->GetObject() != nullptr)
		{
			Blocked.FindOrAdd(Pending->GetObject()).Append(MoveTemp(Blockers));
		}
	}
	return Blocked;
}

// An animation asset's data controller (a transient helper the asset creates for edits) references the asset's
// model subobject from outside the package: the deletion would count it as a user. Unbound for the delete,
// restored if the asset is not deleted.
struct FUnboundController
{
	TWeakObjectPtr<UAnimSequenceBase> Animation;
	TWeakObjectPtr<UObject> Model;

	static TOptional<FUnboundController> Unbind(UObject& Asset)
	{
		UAnimSequenceBase* Animation = Cast<UAnimSequenceBase>(&Asset);
		if (Animation == nullptr)
		{
			return {};
		}
		const TScriptInterface<IAnimationDataModel> Model = Animation->GetDataModelInterface();
		if (Model.GetObject() == nullptr || Animation->GetController().GetModelInterface().GetObject() != Model.GetObject())
		{
			return {};
		}
		Animation->GetController().SetModel(nullptr);
		return FUnboundController{Animation, Model.GetObject()};
	}

	void Restore() const
	{
		if (Animation.IsValid() && Model.IsValid())
		{
			const TScriptInterface<IAnimationDataModel> Current = Animation->GetDataModelInterface();
			if (Current.GetObject() == Model.Get())
			{
				Animation->GetController().SetModel(Current);
			}
		}
	}
};
}

namespace AssetDocumentAssetDelete
{
TArray<FAssetDocumentDeleteBlocker> FindDeleteBlockers(UObject& Asset)
{
	TMap<UObject*, TArray<FAssetDocumentDeleteBlocker>> Blocked = AssetDocumentAssetDeletePrivate::FindBatchBlockers({&Asset});
	TArray<FAssetDocumentDeleteBlocker>* Blockers = Blocked.Find(&Asset);
	return Blockers != nullptr ? MoveTemp(*Blockers) : TArray<FAssetDocumentDeleteBlocker>();
}

FString DescribeDeleteBlockers(const TArray<FAssetDocumentDeleteBlocker>& Blockers, int32 Limit)
{
	TArray<FString> Lines;
	for (int32 Index = 0; Index < Blockers.Num() && Index < Limit; ++Index)
	{
		Lines.Add(Blockers[Index].Describe());
	}
	if (Blockers.Num() > Limit)
	{
		Lines.Add(FString::Printf(TEXT("%d more"), Blockers.Num() - Limit));
	}
	return Lines.IsEmpty() ? FString(TEXT("no blocker found after the refusal")) : FString::Join(Lines, TEXT("; "));
}

TArray<FString> ReleaseTransientReferencers(UObject& Asset, UPackage& Package)
{
	TArray<FString> Released;
	bool bReferenced = false;
	bool bReferencedByUndo = false;
	FReferencerInformationList References;
	ObjectTools::GatherObjectReferencersForDeletion(&Asset, bReferenced, bReferencedByUndo, &References);
	if (!bReferenced)
	{
		return Released;
	}
	UPackage* Transient = GetTransientPackage();
	const UObject* Transactor = GEditor ? static_cast<const UObject*>(ToRawPtr(GEditor->Trans)) : nullptr;
	TArray<UObject*> Holders;
	for (const FReferencerInformation& Reference : References.ExternalReferences)
	{
		UObject* Referencer = Reference.Referencer;
		if (Referencer == nullptr || Referencer->GetOutermost() != Transient || Referencer->IsIn(&Package)
			|| (Transactor != nullptr && (Referencer == Transactor || Referencer->IsIn(Transactor))))
		{
			continue;
		}
		Holders.AddUnique(Referencer);
	}
	if (Holders.IsEmpty())
	{
		return Released;
	}
	TArray<UObject*> Inside;
	GetObjectsWithPackage(&Package, Inside);
	TMap<UObject*, UObject*> ToNull;
	for (UObject* Object : Inside)
	{
		ToNull.Add(Object, nullptr);
	}
	for (UObject* Holder : Holders)
	{
		FArchiveReplaceObjectRef<UObject> Replace(Holder, ToNull,
			EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
		if (Replace.GetCount() > 0)
		{
			Released.Add(Holder->GetFullName());
		}
	}
	return Released;
}

TArray<FAssetDocumentDeleteOutcome> DeleteAssetsAsEditor(const TArray<UObject*>& Assets)
{
	using namespace AssetDocumentAssetDeletePrivate;
	check(IsInGameThread());

	// One entry per distinct asset; Slot maps each request entry to it (before anything is deleted).
	struct FEntry
	{
		TWeakObjectPtr<UObject> Asset;
		FString Package;
		TOptional<FUnboundController> Controller;
		FAssetDocumentDeleteOutcome Outcome;
		bool bRefused = false;
	};
	TArray<FEntry> Entries;
	TArray<int32> Slot;
	for (UObject* Asset : Assets)
	{
		int32 Found = INDEX_NONE;
		if (Asset != nullptr)
		{
			Found = Entries.IndexOfByPredicate([Asset](const FEntry& Entry) { return Entry.Asset.Get() == Asset; });
			if (Found == INDEX_NONE)
			{
				Found = Entries.Num();
				FEntry& Entry = Entries.AddDefaulted_GetRef();
				Entry.Asset = Asset;
				Entry.Package = Asset->GetOutermost()->GetName();
			}
		}
		Slot.Add(Found);
	}
	const auto EntryOf = [&Entries](const UObject* Asset) -> FEntry*
	{
		return Entries.FindByPredicate([Asset](const FEntry& Entry) { return Entry.Asset.Get() == Asset; });
	};

	TArray<UObject*> Batch;
	for (FEntry& Entry : Entries)
	{
		Entry.Controller = FUnboundController::Unbind(*Entry.Asset.Get());
		Batch.Add(Entry.Asset.Get());
	}
	FEditorDelegates::OnAssetsPreDelete.Broadcast(Batch);
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TArray<UObject*> Deletable;
	for (FEntry& Entry : Entries)
	{
		if (UObject* Asset = Entry.Asset.Get())
		{
			Entry.Outcome.Released = ReleaseTransientReferencers(*Asset, *Asset->GetOutermost());
			Deletable.Add(Asset);
		}
	}

	// Refuse what is referenced from outside the deletable set, until nothing more is refused: a batch asset that a
	// refused asset references is then referenced from outside the set as well.
	for (int32 Pass = 0; Pass <= Entries.Num() && !Deletable.IsEmpty(); ++Pass)
	{
		TMap<UObject*, TArray<FAssetDocumentDeleteBlocker>> Blocked = FindBatchBlockers(Deletable);
		if (Blocked.IsEmpty())
		{
			break;
		}
		for (TPair<UObject*, TArray<FAssetDocumentDeleteBlocker>>& Blocking : Blocked)
		{
			Deletable.Remove(Blocking.Key);
			if (FEntry* Entry = EntryOf(Blocking.Key))
			{
				Entry->bRefused = true;
				Entry->Outcome.Blockers = MoveTemp(Blocking.Value);
			}
		}
	}

	if (!Deletable.IsEmpty())
	{
		// As ObjectTools::DeleteSingleObject: when only the Undo history still references the assets, it is cleared.
		bool bUndoCleared = false;
		{
			FAssetDeleteModel Model(Deletable);
			for (int32 Ticks = 0; Model.GetState() != FAssetDeleteModel::Finished && Ticks < 1000; ++Ticks)
			{
				Model.Tick(0.f);
			}
			if (Model.IsAnythingReferencedInMemoryByUndo() && !Model.IsAnythingReferencedInMemoryByNonUndo() && GEditor != nullptr)
			{
				GEditor->ResetTransaction(NSLOCTEXT("AssetDocumentServer", "DeleteAsset", "Delete Asset"));
				bUndoCleared = true;
			}
		}
		TArray<FEntry*> Attempted;
		for (UObject* Asset : Deletable)
		{
			Attempted.Add(EntryOf(Asset));
		}
		TArray<UPackage*> Packages;
		for (UObject* Asset : Deletable)
		{
			Packages.AddUnique(Asset->GetPackage());
		}
		AssetDocumentAssetCompilation::FinishPackageCompilation(Packages);
		ObjectTools::DeleteObjects(Deletable, /*bShowConfirmation*/ false, ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
		for (FEntry* Entry : Attempted)
		{
			if (Entry == nullptr)
			{
				continue;
			}
			UObject* Asset = Entry->Asset.Get();
			if ((Asset == nullptr || !IsValid(Asset)) && !FPackageName::DoesPackageExist(Entry->Package))
			{
				Entry->Outcome.bDeleted = true;
				Entry->Outcome.bUndoCleared = bUndoCleared;
			}
			else if (Asset != nullptr)
			{
				Entry->bRefused = true;
				Entry->Outcome.bUndoCleared = bUndoCleared;
				Entry->Outcome.Blockers = FindDeleteBlockers(*Asset);
			}
		}
	}

	// A blocker that is itself a refused asset of the batch says so.
	TSet<FString> RefusedPackages;
	for (const FEntry& Entry : Entries)
	{
		if (Entry.bRefused)
		{
			RefusedPackages.Add(Entry.Package);
		}
	}
	for (FEntry& Entry : Entries)
	{
		for (FAssetDocumentDeleteBlocker& Blocker : Entry.Outcome.Blockers)
		{
			FString Package = Blocker.Referencer;
			if (Blocker.Kind == TEXT("object"))
			{
				// A full name, "<Class> <Path>": the package is the path up to its first '.'.
				int32 Space = INDEX_NONE;
				Package = Package.FindChar(TEXT(' '), Space) ? Package.Mid(Space + 1) : Package;
				int32 Dot = INDEX_NONE;
				Package = Package.FindChar(TEXT('.'), Dot) ? Package.Left(Dot) : Package;
			}
			Blocker.bRefusedInBatch = (Blocker.Kind == TEXT("package") || Blocker.Kind == TEXT("object")) && RefusedPackages.Contains(Package);
		}
		if (!Entry.Outcome.bDeleted && Entry.Controller.IsSet())
		{
			Entry.Controller->Restore();
		}
	}

	TArray<FAssetDocumentDeleteOutcome> Outcomes;
	Outcomes.SetNum(Assets.Num());
	for (int32 Index = 0; Index < Assets.Num(); ++Index)
	{
		if (Slot[Index] != INDEX_NONE)
		{
			Outcomes[Index] = Entries[Slot[Index]].Outcome;
		}
	}
	return Outcomes;
}

FAssetDocumentDeleteOutcome DeleteAssetAsEditor(UObject& Asset)
{
	return DeleteAssetsAsEditor({&Asset})[0];
}
}
