// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Property/AssetDocumentEditorPreflightIsolation.h"

#include "AssetDocumentEditorTrialScope.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogAssetDocumentPreflightIsolation, Log, All);

struct FAssetDocumentEditorPreflightIsolation::FTouchedPackage
{
	TWeakObjectPtr<UPackage> Package;
	FString Name;
	bool bWasDirty = false;
	bool bCaptureFailed = false;
	bool bChangedBeforeCapture = false;
	TArray<FString> Signals;
	// "<Class> '<Name>'" of the first objects created in the package while watched.
	TArray<FString> CreatedObjects;
	int32 CreatedObjectCount = 0;
	TUniquePtr<FAssetDocumentPackageSnapshot> Snapshot;
};

namespace
{
FAssetDocumentRuntimeDiagnostic MakeIsolationDiagnostic(
	const TCHAR* Code,
	const FString& Message,
	EAssetDocumentDiagnosticSeverity Severity)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/PreflightIsolation");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	Diagnostic.Severity = Severity;
	return Diagnostic;
}

// A transient object (or one inside a transient object) is never saved with its package, so
// creating or changing it changes nothing a save writes: e.g. the Cine Camera Actor the editor's
// actor factory spawns transiently in the level editing viewport's world, and destroys at once,
// to make a Camera Animation Sequence's default camera. Flags and outers are valid in the
// UObject create listener.
bool IsTransientObject(const UObjectBase* Object)
{
	for (const UObjectBase* Current = Object; Current != nullptr && Current->GetClass() != UPackage::StaticClass();
		Current = Current->GetOuter())
	{
		if (EnumHasAnyFlags(Current->GetFlags(), RF_Transient))
		{
			return true;
		}
	}
	return false;
}

// Packages a scratch probe may never change: everything except the transient
// package that holds the scratch, and compiled-in script packages.
UPackage* GetGuardedPackage(const UObject* Object)
{
	if (Object == nullptr || IsTransientObject(Object))
	{
		return nullptr;
	}
	UPackage* const Package = Object->GetOutermost();
	// A staged trial target's package (AssetDocumentEditorTrialScope.h) is the trial's own, like the transient one.
	if (Package == nullptr || Package == GetTransientPackage()
		|| Package->HasAnyPackageFlags(PKG_CompiledIn) || FAssetDocumentEditorTrialScope::IsStagedTrialPackage(*Package))
	{
		return nullptr;
	}
	return Package;
}
} // namespace

FAssetDocumentEditorPreflightIsolation::FAssetDocumentEditorPreflightIsolation(FString InContext)
	: Context(MoveTemp(InContext))
{
	check(IsInGameThread());
	ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddRaw(
		this, &FAssetDocumentEditorPreflightIsolation::HandleObjectModified);
	PreEditHandle = FCoreUObjectDelegates::OnPreObjectPropertyChanged.AddRaw(
		this, &FAssetDocumentEditorPreflightIsolation::HandlePreEditChange);
	PostEditHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(
		this, &FAssetDocumentEditorPreflightIsolation::HandlePostEditChange);
	DirtyHandle = UPackage::PackageMarkedDirtyEvent.AddRaw(
		this, &FAssetDocumentEditorPreflightIsolation::HandlePackageDirtied);
	GUObjectArray.AddUObjectCreateListener(this);
	bWatching = true;
}

FAssetDocumentEditorPreflightIsolation::~FAssetDocumentEditorPreflightIsolation()
{
	if (!bRestored)
	{
		TArray<FAssetDocumentRuntimeDiagnostic> Ignored;
		Restore(Ignored);
	}
	Stop();
}

void FAssetDocumentEditorPreflightIsolation::Stop()
{
	if (!bWatching)
	{
		return;
	}
	bWatching = false;
	FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);
	FCoreUObjectDelegates::OnPreObjectPropertyChanged.Remove(PreEditHandle);
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PostEditHandle);
	UPackage::PackageMarkedDirtyEvent.Remove(DirtyHandle);
	GUObjectArray.RemoveUObjectCreateListener(this);
}

FAssetDocumentEditorPreflightIsolation::FTouchedPackage*
FAssetDocumentEditorPreflightIsolation::Track(UObject* Object, const TCHAR* Signal)
{
	if (!bWatching || bCapturing || !IsInGameThread())
	{
		return nullptr;
	}
	UPackage* const Package = GetGuardedPackage(Object);
	if (Package == nullptr)
	{
		return nullptr;
	}
	FTouchedPackage* Found = nullptr;
	for (const TUniquePtr<FTouchedPackage>& Entry : Touched)
	{
		if (Entry->Package.Get() == Package)
		{
			Found = Entry.Get();
			break;
		}
	}
	if (Found == nullptr)
	{
		Found = Touched.Add_GetRef(MakeUnique<FTouchedPackage>()).Get();
		Found->Package = Package;
		Found->Name = Package->GetName();
		Found->bWasDirty = Package->IsDirty();
	}
	Found->Signals.AddUnique(Signal);
	return Found;
}

void FAssetDocumentEditorPreflightIsolation::Capture(FTouchedPackage& Entry, bool bBeforeChange)
{
	if (Entry.Snapshot.IsValid() || Entry.bCaptureFailed || !Entry.Package.IsValid())
	{
		return;
	}
	TGuardValue<bool> CapturingGuard(bCapturing, true);
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	Entry.Snapshot = FAssetDocumentPackageSnapshot::Capture(*Entry.Package.Get(), Diagnostics);
	Entry.bCaptureFailed = !Entry.Snapshot.IsValid();
	Entry.bChangedBeforeCapture |= !bBeforeChange;
}

void FAssetDocumentEditorPreflightIsolation::HandleObjectModified(UObject* Object)
{
	if (FTouchedPackage* Entry = Track(Object, TEXT("Modify")))
	{
		Capture(*Entry, true);
	}
}

void FAssetDocumentEditorPreflightIsolation::HandlePreEditChange(UObject* Object, const FEditPropertyChain& Chain)
{
	(void)Chain;
	if (FTouchedPackage* Entry = Track(Object, TEXT("PreEditChange")))
	{
		Capture(*Entry, true);
	}
}

void FAssetDocumentEditorPreflightIsolation::HandlePostEditChange(UObject* Object, FPropertyChangedEvent& Event)
{
	(void)Event;
	if (FTouchedPackage* Entry = Track(Object, TEXT("PostEditChange")))
	{
		Capture(*Entry, true);
	}
}

void FAssetDocumentEditorPreflightIsolation::HandlePackageDirtied(UPackage* Package, bool bWasDirty)
{
	const bool bNew = !Touched.ContainsByPredicate([Package](const TUniquePtr<FTouchedPackage>& Entry)
	{
		return Entry->Package.Get() == Package;
	});
	if (FTouchedPackage* Entry = Track(Package, TEXT("MarkPackageDirty")))
	{
		if (bNew)
		{
			Entry->bWasDirty = bWasDirty;
		}
		// UObject::Modify dirties the package before the object changes.
		Capture(*Entry, true);
	}
}

void FAssetDocumentEditorPreflightIsolation::NotifyUObjectCreated(const UObjectBase* ObjectBase, int32 Index)
{
	(void)Index;
	if (!bWatching || bCapturing || !IsInGameThread() || ObjectBase == nullptr)
	{
		return;
	}
	// The object is still being constructed; only its name, flags, class and outer
	// chain are valid. Loading a package (and its default subobjects) is not a change of it.
	if (ObjectBase->GetClass() == UPackage::StaticClass())
	{
		// A package that appears while watched: loaded (first use of content) or created.
		PackagesAppearedInScope.Add(ObjectBase->GetFName());
		return;
	}
	const UObject* Object = static_cast<const UObject*>(ObjectBase);
	if (IsLoading() || Object->HasAnyFlags(RF_NeedLoad | RF_WasLoaded | RF_ClassDefaultObject))
	{
		return;
	}
	UPackage* const Package = GetGuardedPackage(Object);
	if (Package == nullptr)
	{
		return;
	}
	FTouchedPackage* Found = nullptr;
	for (const TUniquePtr<FTouchedPackage>& Entry : Touched)
	{
		if (Entry->Package.Get() == Package)
		{
			Found = Entry.Get();
			break;
		}
	}
	if (Found == nullptr)
	{
		Found = Touched.Add_GetRef(MakeUnique<FTouchedPackage>()).Get();
		Found->Package = Package;
		Found->Name = Package->GetName();
		Found->bWasDirty = Package->IsDirty();
	}
	Found->Signals.AddUnique(TEXT("NewObject"));
	Found->bChangedBeforeCapture |= !Found->Snapshot.IsValid();
	++Found->CreatedObjectCount;
	if (Found->CreatedObjects.Num() < 3)
	{
		Found->CreatedObjects.Add(FString::Printf(TEXT("%s '%s'"),
			*ObjectBase->GetClass()->GetName(), *ObjectBase->GetFName().ToString()));
	}
}

FString FAssetDocumentEditorPreflightIsolation::DescribeCreatedObjects(const FTouchedPackage& Entry)
{
	FString Described = FString::Join(Entry.CreatedObjects, TEXT(", "));
	if (Entry.CreatedObjectCount > Entry.CreatedObjects.Num())
	{
		Described += FString::Printf(TEXT(" and %d more"), Entry.CreatedObjectCount - Entry.CreatedObjects.Num());
	}
	return Described;
}

void FAssetDocumentEditorPreflightIsolation::OnUObjectArrayShutdown()
{
	GUObjectArray.RemoveUObjectCreateListener(this);
}

TArray<FString> FAssetDocumentEditorPreflightIsolation::GetTouchedPackageNames() const
{
	TArray<FString> Names;
	for (const TUniquePtr<FTouchedPackage>& Entry : Touched)
	{
		Names.Add(Entry->Name);
	}
	return Names;
}

bool FAssetDocumentEditorPreflightIsolation::Restore(TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	check(IsInGameThread());
	Stop();
	if (bRestored)
	{
		return true;
	}
	bRestored = true;
	bool bAllRestored = true;
	for (const TUniquePtr<FTouchedPackage>& Entry : Touched)
	{
		UPackage* const Package = Entry->Package.Get();
		const FString Signals = FString::Join(Entry->Signals, TEXT(","));
		FString Outcome;
		TArray<FAssetDocumentRuntimeDiagnostic> RestoreDiagnostics;
		// Objects created in a package without any change notification and without dirtying it:
		// the first use of content (a package the preflight loaded, whose loaded objects create
		// their editor data lazily, or engine/plugin content initialized on first use). Nothing
		// of the package changed that a save would write.
		const bool bOnlyCreatedObjects = Package != nullptr
			&& Entry->Signals.Num() == 1 && Entry->Signals[0] == TEXT("NewObject")
			&& !Entry->bWasDirty && !Package->IsDirty();
		// A package the preflight loaded whose objects first created their editor data and then
		// finished their load-time upgrade with a PostEditChange (a legacy skeletal mesh builds
		// its mesh descriptions and render data after load), with no Modify, PreEditChange or
		// dirtying: the first use of the content, not a change a save would write.
		const bool bLoadTimeUpgrade = Package != nullptr && !Entry->bWasDirty && !Package->IsDirty()
			&& !Entry->Signals.IsEmpty() && Entry->Signals[0] == TEXT("NewObject")
			&& Entry->Signals.FilterByPredicate([](const FString& Signal)
				{
					return Signal != TEXT("NewObject") && Signal != TEXT("PostEditChange");
				}).IsEmpty();
		const bool bAppeared = Package != nullptr && PackagesAppearedInScope.Contains(Package->GetFName());
		const bool bLoaded = bAppeared && !Package->GetLoadedPath().IsEmpty();
		// A package the preflight loaded whose objects created their editor data and then initialized
		// it with Modify (no PreEditChange, never dirtied): e.g. a Control Rig module or function
		// library Blueprint building its hierarchy and VM on first use. Its first signal is the
		// creation (no snapshot could precede it) and nothing a save would write changed: a change
		// of a loaded asset dirties its package.
		const bool bFirstUseInitialization = Package != nullptr && !Entry->bWasDirty && !Package->IsDirty()
			&& !Entry->Signals.IsEmpty() && Entry->Signals[0] == TEXT("NewObject")
			&& !Entry->Signals.Contains(TEXT("PreEditChange")) && !Entry->Signals.Contains(TEXT("MarkPackageDirty"));
		if (Package == nullptr)
		{
			Outcome = TEXT("unloaded");
		}
		else if ((bOnlyCreatedObjects || bLoadTimeUpgrade || bFirstUseInitialization) && bLoaded)
		{
			Outcome = TEXT("first-load");
		}
		else if (bOnlyCreatedObjects && !Entry->Name.StartsWith(TEXT("/Game/")))
		{
			Outcome = TEXT("content-initialized");
		}
		else if (Entry->bCaptureFailed || Entry->bChangedBeforeCapture || !Entry->Snapshot.IsValid())
		{
			Outcome = Entry->bCaptureFailed ? TEXT("capture-failed")
				: bAppeared && !bLoaded ? TEXT("created-package") : TEXT("changed-before-capture");
		}
		else
		{
			const FAssetDocumentContractValidationResult Restored = Entry->Snapshot->RestoreWithoutEditorCallbacks();
			if (Restored.bValid)
			{
				Package->SetDirtyFlag(Entry->bWasDirty);
				Outcome = Package->IsDirty() == Entry->bWasDirty ? TEXT("restored") : TEXT("dirty-flag-not-restored");
			}
			else
			{
				Outcome = TEXT("restore-failed");
				RestoreDiagnostics = Restored.Diagnostics;
			}
		}
		const bool bUnchanged = Outcome == TEXT("first-load") || Outcome == TEXT("content-initialized");
		const bool bOk = Outcome == TEXT("restored") || bUnchanged;
		bAllRestored &= bOk;
		// One line per touched package: the coverage audit greps these to list the
		// classes whose PostEditChange reaches other packages.
		UE_LOG(LogAssetDocumentPreflightIsolation, Display,
			TEXT("Preflight touched another package: context=%s package=%s signals=%s result=%s"),
			*Context, *Entry->Name, *Signals, *Outcome);
		if (bUnchanged)
		{
			const bool bFirstLoad = Outcome == TEXT("first-load");
			const FString Created = DescribeCreatedObjects(*Entry);
			OutDiagnostics.Add(MakeIsolationDiagnostic(
				bFirstLoad ? TEXT("PreflightIsolation.PackageLoaded") : TEXT("PreflightIsolation.ContentInitialized"),
				bFirstLoad
					? FString::Printf(TEXT("Preflight of %s loaded package '%s' for the first time; its loaded objects created their editor data (%s)%s. The package is unchanged."),
						*Context, *Entry->Name, *Created, Entry->Signals.Contains(TEXT("PostEditChange"))
							? TEXT(" and finished their load-time upgrade")
							: Entry->Signals.Contains(TEXT("Modify")) ? TEXT(" and initialized it") : TEXT(""))
					: FString::Printf(TEXT("Preflight of %s used engine, plugin or scratch package '%s' for the first time; objects were created in it (%s) without changing it."),
						*Context, *Entry->Name, *Created),
				EAssetDocumentDiagnosticSeverity::Info));
			continue;
		}
		if (bOk)
		{
			OutDiagnostics.Add(MakeIsolationDiagnostic(TEXT("PreflightIsolation.PackageRestored"),
				FString::Printf(TEXT("Preflight of %s reached package '%s' (%s); it was restored unchanged."),
					*Context, *Entry->Name, *Signals),
				EAssetDocumentDiagnosticSeverity::Info));
			continue;
		}
		OutDiagnostics.Add(MakeIsolationDiagnostic(TEXT("PreflightIsolation.PackageNotRestored"),
			FString::Printf(TEXT("Preflight of %s changed package '%s' (%s) and it could not be restored (%s): %s"),
				*Context, *Entry->Name, *Signals, *Outcome, *DescribeNotRestored(*Entry, Outcome)),
			EAssetDocumentDiagnosticSeverity::Error));
		OutDiagnostics.Append(RestoreDiagnostics);
	}
	return bAllRestored;
}

FString FAssetDocumentEditorPreflightIsolation::DescribeNotRestored(const FTouchedPackage& Entry, const FString& Outcome)
{
	const TCHAR* const Remedy = TEXT(" Nothing was saved: if the package should not hold this change, reload it from disk ")
		TEXT("(Content Browser: Asset Actions > Reload) or restart the editor; then retry.");
	if (Outcome == TEXT("unloaded"))
	{
		return TEXT("the package was unloaded while the preflight ran, so its state could not be checked.");
	}
	if (Outcome == TEXT("capture-failed"))
	{
		return FString(TEXT("its pre-change state could not be snapshotted.")) + Remedy;
	}
	if (Outcome == TEXT("created-package"))
	{
		return FString::Printf(TEXT("the preflight created this package in memory (objects: %s); it is not on disk and nothing was saved."),
			*DescribeCreatedObjects(Entry));
	}
	if (Outcome == TEXT("changed-before-capture"))
	{
		return Entry.CreatedObjectCount > 0
			? FString::Printf(TEXT("objects were created in it (%s) before any change notification, so no snapshot preceded the change.%s"),
				*DescribeCreatedObjects(Entry), Remedy)
			: FString::Printf(TEXT("it changed before a snapshot could be taken.%s"), Remedy);
	}
	if (Outcome == TEXT("dirty-flag-not-restored"))
	{
		return TEXT("its content was restored but its unsaved-changes flag could not be reset; it holds no change from the preflight.");
	}
	return TEXT("restoring its snapshot failed (see the next diagnostics).");
}
