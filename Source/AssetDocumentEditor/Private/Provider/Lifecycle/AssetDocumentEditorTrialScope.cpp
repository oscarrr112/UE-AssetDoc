// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentEditorTrialScope.h"

#include "AssetDocumentAssetCompilation.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Misc/Guid.h"
#include "Provider/Property/AssetDocumentEditorPreflightIsolation.h"
#include "UObject/ObjectKey.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/WeakObjectPtr.h"

namespace
{
// Trial target -> its source (null weak pointer for a new asset). Game thread only; one trial at a time.
TMap<FObjectKey, TWeakObjectPtr<const UObject>>& TrialTargets()
{
	static TMap<FObjectKey, TWeakObjectPtr<const UObject>> Targets;
	return Targets;
}

// Keyed by class path, never by the UClass: unregistering at module shutdown must not touch a purged class.
TMap<FTopLevelAssetPath, FAssetDocumentEditorTrialScope::FTrialTargetMaker>& TrialTargetMakers()
{
	static TMap<FTopLevelAssetPath, FAssetDocumentEditorTrialScope::FTrialTargetMaker> Makers;
	return Makers;
}
}

namespace AssetDocumentEditorTrialScopeLocal
{
const TCHAR* const StagedPackageRoot = TEXT("/Temp/AssetDocument/ValidateTrial");

// Keyed by class path, like the makers.
TMap<FTopLevelAssetPath, FAssetDocumentEditorTrialScope::FStagedTrialDiscard>& StagedTrialClasses()
{
	static TMap<FTopLevelAssetPath, FAssetDocumentEditorTrialScope::FStagedTrialDiscard> Classes;
	return Classes;
}

// The staged packages of the open scope, in creation order.
TArray<TWeakObjectPtr<UPackage>>& StagedPackages()
{
	static TArray<TWeakObjectPtr<UPackage>> Packages;
	return Packages;
}

int32& OpenScopeCount()
{
	static int32 Count = 0;
	return Count;
}

const FAssetDocumentEditorTrialScope::FStagedTrialDiscard* FindStagedTrialDiscard(const UClass& AssetClass)
{
	for (const UClass* Class = &AssetClass; Class != nullptr; Class = Class->GetSuperClass())
	{
		if (const FAssetDocumentEditorTrialScope::FStagedTrialDiscard* Found = StagedTrialClasses().Find(Class->GetClassPathName()))
		{
			return Found;
		}
	}
	return nullptr;
}

// What the editor's Delete of a never-saved asset leaves: the asset's class first drops what it registered outside
// the package, then the objects lose their asset flags and are collected with the renamed-away package.
void DiscardStagedPackage(UPackage& Package)
{
	TArray<UObject*> Objects;
	GetObjectsWithPackage(&Package, Objects);
	for (UObject* Object : Objects)
	{
		if (Object != nullptr && Object->GetOuter() == &Package && IsValid(Object))
		{
			if (const FAssetDocumentEditorTrialScope::FStagedTrialDiscard* Discard = FindStagedTrialDiscard(*Object->GetClass()))
			{
				(*Discard)(*Object);
			}
		}
	}
	Objects.Reset();
	AssetDocumentAssetCompilation::FinishPackageCompilation({&Package});
	GetObjectsWithPackage(&Package, Objects);
	for (UObject* Object : Objects)
	{
		if (Object != nullptr)
		{
			Object->ClearFlags(RF_Public | RF_Standalone);
			Object->MarkAsGarbage();
		}
	}
	Package.SetDirtyFlag(false);
	Package.ClearFlags(RF_Public | RF_Standalone);
	const FName Discarded = MakeUniqueObjectName(nullptr, UPackage::StaticClass(), FName(TEXT("/Temp/AssetDocumentDiscardedPackage")));
	Package.Rename(*Discarded.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
	Package.MarkAsGarbage();
}
} // namespace AssetDocumentEditorTrialScopeLocal

void FAssetDocumentEditorTrialScope::RegisterStagedTrialClass(const FTopLevelAssetPath& AssetClassPath, FStagedTrialDiscard Discard)
{
	using namespace AssetDocumentEditorTrialScopeLocal;
	check(IsInGameThread());
	StagedTrialClasses().Add(AssetClassPath, MoveTemp(Discard));
}

void FAssetDocumentEditorTrialScope::UnregisterStagedTrialClass(const FTopLevelAssetPath& AssetClassPath)
{
	using namespace AssetDocumentEditorTrialScopeLocal;
	check(IsInGameThread());
	StagedTrialClasses().Remove(AssetClassPath);
}

bool FAssetDocumentEditorTrialScope::IsStagedTrialClass(const UClass& AssetClass)
{
	using namespace AssetDocumentEditorTrialScopeLocal;
	check(IsInGameThread());
	return FindStagedTrialDiscard(AssetClass) != nullptr;
}

UPackage* FAssetDocumentEditorTrialScope::CreateStagedTrialPackage(const UClass& AssetClass)
{
	using namespace AssetDocumentEditorTrialScopeLocal;
	check(IsInGameThread());
	if (OpenScopeCount() == 0)
	{
		return nullptr;
	}
	const FString Name = FString::Printf(TEXT("%s/%s_%s"), StagedPackageRoot, *AssetClass.GetName(),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* Package = CreatePackage(*Name);
	if (Package != nullptr)
	{
		Package->MarkAsFullyLoaded();
		StagedPackages().Add(Package);
	}
	return Package;
}

bool FAssetDocumentEditorTrialScope::IsStagedTrialPackage(const UPackage& Package)
{
	using namespace AssetDocumentEditorTrialScopeLocal;
	if (!IsInGameThread() || StagedPackages().IsEmpty())
	{
		return false;
	}
	return StagedPackages().ContainsByPredicate([&Package](const TWeakObjectPtr<UPackage>& Staged) { return Staged.Get() == &Package; });
}

void FAssetDocumentEditorTrialScope::RegisterTrialTargetMaker(const FTopLevelAssetPath& AssetClassPath, FTrialTargetMaker Maker)
{
	check(IsInGameThread());
	TrialTargetMakers().Add(AssetClassPath, MoveTemp(Maker));
}

void FAssetDocumentEditorTrialScope::UnregisterTrialTargetMaker(const FTopLevelAssetPath& AssetClassPath)
{
	check(IsInGameThread());
	TrialTargetMakers().Remove(AssetClassPath);
}

const FAssetDocumentEditorTrialScope::FTrialTargetMaker* FAssetDocumentEditorTrialScope::FindTrialTargetMaker(const UClass& AssetClass)
{
	check(IsInGameThread());
	return TrialTargetMakers().Find(AssetClass.GetClassPathName());
}

bool FAssetDocumentEditorTrialScope::IsTrialTarget(const UObject& Object)
{
	check(IsInGameThread());
	return TrialTargets().Contains(FObjectKey(&Object));
}

const UObject* FAssetDocumentEditorTrialScope::FindTrialSource(const UObject& Object)
{
	check(IsInGameThread());
	const TWeakObjectPtr<const UObject>* Source = TrialTargets().Find(FObjectKey(&Object));
	return Source != nullptr ? Source->Get() : nullptr;
}

void FAssetDocumentEditorTrialScope::RegisterTrialTarget(const UObject& Target, const UObject* Source)
{
	check(IsInGameThread());
	TrialTargets().Add(FObjectKey(&Target), Source);
}

FAssetDocumentEditorTrialScope::FAssetDocumentEditorTrialScope()
{
	check(IsInGameThread());
	// Inside an open transaction (GUndo set) the buffer cannot be swapped; the trial then records
	// into it like any editor operation would. Validate requests never run inside one.
	if (GEditor != nullptr && GEditor->Trans != nullptr && GUndo == nullptr && !GEditor->IsTransactionActive())
	{
		UTransBuffer* Buffer = NewObject<UTransBuffer>();
		Buffer->Initialize(16 * 1024 * 1024);
		SavedTransactor.Reset(ToRawPtr(GEditor->Trans));
		TrialTransactor.Reset(Buffer);
		GEditor->Trans = Buffer;
	}
	Isolation = MakeUnique<FAssetDocumentEditorPreflightIsolation>(TEXT("ValidateTrial"));
	TrialTargets().Reset();
	++AssetDocumentEditorTrialScopeLocal::OpenScopeCount();
}

FAssetDocumentEditorTrialScope::~FAssetDocumentEditorTrialScope()
{
	TArray<FAssetDocumentRuntimeDiagnostic> Ignored;
	Finish(Ignored);
}

bool FAssetDocumentEditorTrialScope::Finish(TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	check(IsInGameThread());
	if (bFinished)
	{
		return true;
	}
	bFinished = true;
	TrialTargets().Reset();
	const bool bRestored = Isolation.IsValid() ? Isolation->Restore(OutDiagnostics) : true;
	Isolation.Reset();
	if (TrialTransactor.IsValid())
	{
		if (GEditor != nullptr && ToRawPtr(GEditor->Trans) == TrialTransactor.Get())
		{
			GEditor->Trans = CastChecked<UTransactor>(SavedTransactor.Get());
		}
		CastChecked<UTransactor>(TrialTransactor.Get())->Reset(FText::GetEmpty());
		TrialTransactor.Reset();
		SavedTransactor.Reset();
	}
	// Staged trial targets go last: nothing records or guards them any more.
	{
		using namespace AssetDocumentEditorTrialScopeLocal;
		--OpenScopeCount();
		TArray<TWeakObjectPtr<UPackage>> Staged = MoveTemp(StagedPackages());
		StagedPackages().Reset();
		for (const TWeakObjectPtr<UPackage>& Package : Staged)
		{
			if (UPackage* Live = Package.Get())
			{
				DiscardStagedPackage(*Live);
			}
		}
	}
	return bRestored;
}
