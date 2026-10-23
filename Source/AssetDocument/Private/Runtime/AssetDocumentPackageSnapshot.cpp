// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Runtime/AssetDocumentPackageSnapshot.h"

#include "AssetDocumentModule.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Misc/PackageName.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "Utils/AssetDocumentEngineCompat.h"

namespace
{
constexpr ERenameFlags SnapshotRenameFlags =
	REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty;

FAssetDocumentRuntimeDiagnostic MakeSnapshotDiagnostic(
	const TCHAR* Code,
	const FString& Message,
	EAssetDocumentDiagnosticSeverity Severity = EAssetDocumentDiagnosticSeverity::Error)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/PackageSnapshot");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	Diagnostic.Severity = Severity;
	return Diagnostic;
}

bool HasErrors(TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	return Diagnostics.ContainsByPredicate([](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
	{
		return Diagnostic.Severity == EAssetDocumentDiagnosticSeverity::Error;
	});
}

int32 GetDepthInPackage(const UObject& Object, const UPackage& Package)
{
	int32 Depth = 0;
	for (const UObject* Outer = Object.GetOuter(); Outer && Outer != &Package; Outer = Outer->GetOuter())
	{
		++Depth;
	}
	return Depth;
}

// Writes object references through a map (a replacement -> the object the capture saw), so an
// object that differs from its capture only by references to replaced objects serializes to its
// captured bytes.
class FSnapshotObjectWriter final : public FObjectWriter
{
public:
	FSnapshotObjectWriter(TArray<uint8>& InBytes, const TMap<UObject*, UObject*>& InMap)
		: FObjectWriter(InBytes)
		, Map(InMap)
	{
		ArNoDelta = true;
		ArPortFlags |= PPF_DuplicateVerbatim;
	}

	using FObjectWriter::operator<<;
	virtual FArchive& operator<<(UObject*& Res) override
	{
		UObject* const* Found = Map.Find(Res);
		UObject* Mapped = Found != nullptr ? *Found : Res;
		return FObjectWriter::operator<<(Mapped);
	}
	virtual FArchive& operator<<(FObjectPtr& Value) override
	{
		UObject* const* Found = Map.Find(Value.Get());
		FObjectPtr Mapped = Found != nullptr ? FObjectPtr(*Found) : Value;
		return FObjectWriter::operator<<(Mapped);
	}

private:
	const TMap<UObject*, UObject*>& Map;
};

// Reads captured bytes, remapping references to replaced objects to their replacements.
class FSnapshotObjectReader final : public FObjectReader
{
public:
	FSnapshotObjectReader(const TArray<uint8>& InBytes, const TMap<UObject*, UObject*>& InMap)
		: FObjectReader(InBytes)
		, Map(InMap)
	{
		SetPortFlags(PPF_DuplicateVerbatim);
	}

	using FObjectReader::operator<<;
	virtual FArchive& operator<<(UObject*& Res) override
	{
		FObjectReader::operator<<(Res);
		if (UObject* const* Found = Map.Find(Res))
		{
			Res = *Found;
		}
		return *this;
	}
	virtual FArchive& operator<<(FObjectPtr& Value) override
	{
		FObjectReader::operator<<(Value);
		if (UObject* const* Found = Map.Find(Value.Get()))
		{
			Value = FObjectPtr(*Found);
		}
		return *this;
	}

private:
	const TMap<UObject*, UObject*>& Map;
};

bool SerializeObject(UObject& Object, TArray<uint8>& OutBytes, const TMap<UObject*, UObject*>& ReplacedToCaptured = {})
{
	// Full serialization: delta serialization would omit values equal to
	// archetype defaults and could not restore an exact live state.
	if (ReplacedToCaptured.IsEmpty())
	{
		FObjectWriter Writer(&Object, OutBytes, false, false, false, PPF_DuplicateVerbatim);
		return !Writer.IsError();
	}
	FSnapshotObjectWriter Writer(OutBytes, ReplacedToCaptured);
	Object.Serialize(Writer);
	return !Writer.IsError();
}

TArray<TSharedRef<IAssetDocumentDerivedStateRebuilder>>& GetRebuilderRegistry()
{
	static TArray<TSharedRef<IAssetDocumentDerivedStateRebuilder>> Registry;
	return Registry;
}
} // namespace

EAssetDocumentSnapshotObjectRole IAssetDocumentDerivedStateRebuilder::Classify(
	const UObject& Object) const
{
	return FAssetDocumentPackageSnapshot::ClassifyDefault(Object);
}

bool RegisterAssetDocumentDerivedStateRebuilder(
	TSharedRef<IAssetDocumentDerivedStateRebuilder> Rebuilder)
{
	check(IsInGameThread());
	const FName Id = Rebuilder->GetId();
	TArray<TSharedRef<IAssetDocumentDerivedStateRebuilder>>& Registry = GetRebuilderRegistry();
	if (Id.IsNone() || Registry.ContainsByPredicate([Id](const TSharedRef<IAssetDocumentDerivedStateRebuilder>& Existing)
		{
			return Existing->GetId() == Id;
		}))
	{
		return false;
	}
	Registry.Add(MoveTemp(Rebuilder));
	return true;
}

bool UnregisterAssetDocumentDerivedStateRebuilder(FName RebuilderId)
{
	check(IsInGameThread());
	return GetRebuilderRegistry().RemoveAll([RebuilderId](const TSharedRef<IAssetDocumentDerivedStateRebuilder>& Existing)
	{
		return Existing->GetId() == RebuilderId;
	}) > 0;
}

EAssetDocumentSnapshotObjectRole FAssetDocumentPackageSnapshot::ClassifyDefault(const UObject& Object)
{
	// Generated classes/functions and class default objects (with their default
	// subobjects) are regenerated from authored state, e.g. by a Blueprint compile.
	if (Object.IsA<UStruct>() || Object.IsTemplate(RF_ClassDefaultObject))
	{
		return EAssetDocumentSnapshotObjectRole::Derived;
	}
	return EAssetDocumentSnapshotObjectRole::Authored;
}

bool FAssetDocumentPackageSnapshot::CanRestorePackage(UPackage& Package)
{
	check(IsInGameThread());
	UObject* Primary = Package.FindAssetInPackage();
	if (Primary && GetRebuilderRegistry().ContainsByPredicate(
		[Primary](const TSharedRef<IAssetDocumentDerivedStateRebuilder>& Rebuilder) { return Rebuilder->Supports(*Primary); }))
	{
		return true;
	}
	bool bHasDerived = false;
	ForEachObjectWithOuterBreakable(&Package, [Primary, &bHasDerived](UObject* Object)
	{
		bHasDerived = Object != Primary && ClassifyDefault(*Object) == EAssetDocumentSnapshotObjectRole::Derived;
		return !bHasDerived;
	}, AssetDocumentObjects::IncludeNested, RF_NoFlags, EInternalObjectFlags::Garbage);
	return !bHasDerived;
}

TUniquePtr<FAssetDocumentPackageSnapshot> FAssetDocumentPackageSnapshot::Capture(
	UPackage& Package,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	check(IsInGameThread());
	TUniquePtr<FAssetDocumentPackageSnapshot> Snapshot(new FAssetDocumentPackageSnapshot());
	Snapshot->Package = &Package;
	Snapshot->PackageName = Package.GetName();
	Snapshot->bWasDirty = Package.IsDirty();
	if (UObject* Primary = Package.FindAssetInPackage())
	{
		Snapshot->PrimaryAsset = Primary;
		for (const TSharedRef<IAssetDocumentDerivedStateRebuilder>& Rebuilder : GetRebuilderRegistry())
		{
			if (Rebuilder->Supports(*Primary))
			{
				Snapshot->Rebuilders.Add(Rebuilder);
				Snapshot->RebuilderStates.Add(Rebuilder->CaptureDerivedState(*Primary));
			}
		}
	}

	TArray<UObject*> Candidates;
	GetObjectsWithOuter(&Package, Candidates, AssetDocumentObjects::IncludeNested, RF_NoFlags, EInternalObjectFlags::Garbage);
	for (UObject* Object : Candidates)
	{
		if (Snapshot->Classify(*Object) != EAssetDocumentSnapshotObjectRole::Authored)
		{
			continue;
		}
		FObjectRecord& Record = Snapshot->Objects.AddDefaulted_GetRef();
		Record.Object = Object;
		Record.OriginalOuter = Object->GetOuter();
		Record.OriginalName = Object->GetFName();
		Record.OriginalFlags = Object->GetFlags();
		Record.bWasRooted = Object->IsRooted();
		Record.Depth = GetDepthInPackage(*Object, Package);
		if (!SerializeObject(*Object, Record.Bytes))
		{
			OutDiagnostics.Add(MakeSnapshotDiagnostic(
				TEXT("PackageSnapshot.CaptureFailed"),
				FString::Printf(TEXT("Failed to serialize '%s' for the package snapshot."), *Object->GetPathName())));
			return nullptr;
		}
		if (const UEdGraphNode* Node = Cast<UEdGraphNode>(Object))
		{
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin != nullptr)
				{
					Record.PinIds.Add(Pin->PinId);
				}
			}
		}
		Snapshot->AuthoredObjectSet.Add(Object);
	}
	Snapshot->Objects.StableSort([](const FObjectRecord& A, const FObjectRecord& B)
	{
		return A.Depth < B.Depth;
	});
	for (const FObjectRecord& Record : Snapshot->Objects)
	{
		Snapshot->Tracked.Add(Record.Object.Get());
		Snapshot->Tracked.Add(Record.OriginalOuter.Get());
	}
	// A Blueprint compile's reinstancing reports through OnObjectsReinstanced, other replacements
	// (package reload, constructed components) through OnObjectsReplaced.
	Snapshot->ObjectsReplacedHandle = FCoreUObjectDelegates::OnObjectsReplaced.AddRaw(
		Snapshot.Get(), &FAssetDocumentPackageSnapshot::OnObjectsReplaced);
	Snapshot->ObjectsReinstancedHandle = FCoreUObjectDelegates::OnObjectsReinstanced.AddRaw(
		Snapshot.Get(), &FAssetDocumentPackageSnapshot::OnObjectsReplaced);
	return Snapshot;
}

FAssetDocumentPackageSnapshot::~FAssetDocumentPackageSnapshot()
{
	FCoreUObjectDelegates::OnObjectsReplaced.Remove(ObjectsReplacedHandle);
	FCoreUObjectDelegates::OnObjectsReinstanced.Remove(ObjectsReinstancedHandle);
}

void FAssetDocumentPackageSnapshot::OnObjectsReplaced(const TMap<UObject*, UObject*>& Replaced)
{
	for (const TPair<UObject*, UObject*>& Entry : Replaced)
	{
		if (Entry.Key != nullptr && Entry.Value != nullptr && Entry.Key != Entry.Value && Tracked.Contains(Entry.Key))
		{
			Replacements.Add(Entry.Key, Entry.Value);
			Tracked.Add(Entry.Value);
		}
	}
}

UObject* FAssetDocumentPackageSnapshot::ResolveReplacement(UObject* Object) const
{
	UObject* Latest = Object;
	// Bounded: a replacement chain is one entry per compile.
	for (int32 Step = 0; Step < 64; ++Step)
	{
		const TObjectPtr<UObject>* Next = Replacements.Find(Latest);
		if (Next == nullptr || *Next == nullptr)
		{
			break;
		}
		Latest = Next->Get();
	}
	return Latest != Object && IsValid(Latest) ? Latest : Object;
}

void FAssetDocumentPackageSnapshot::BuildReplacementMaps(TMap<UObject*, UObject*>& OutLatest, TMap<UObject*, UObject*>& OutCaptured) const
{
	TSet<const UObject*> Successors;
	for (const TPair<UObject*, TObjectPtr<UObject>>& Entry : Replacements)
	{
		Successors.Add(Entry.Value.Get());
		UObject* Latest = ResolveReplacement(Entry.Key);
		if (Latest != Entry.Key)
		{
			OutLatest.Add(Entry.Key, Latest);
		}
	}
	// Each replacement maps back to the start of its chain: the object the capture saw (records
	// may already have been retargeted by FollowReplacements; the captured bytes still name it).
	for (const TPair<UObject*, TObjectPtr<UObject>>& Entry : Replacements)
	{
		if (Successors.Contains(Entry.Key))
		{
			continue;
		}
		UObject* Current = Entry.Key;
		for (int32 Step = 0; Step < 64; ++Step)
		{
			const TObjectPtr<UObject>* Next = Replacements.Find(Current);
			if (Next == nullptr || *Next == nullptr || OutCaptured.Contains(Next->Get()))
			{
				break;
			}
			Current = Next->Get();
			OutCaptured.Add(Current, Entry.Key);
		}
	}
}

void FAssetDocumentPackageSnapshot::FollowReplacements()
{
	for (FObjectRecord& Record : Objects)
	{
		UObject* Captured = Record.Object.Get();
		UObject* Latest = ResolveReplacement(Captured);
		if (Latest != Captured)
		{
			AuthoredObjectSet.Remove(Captured);
			AuthoredObjectSet.Add(Latest);
			Record.Object = Latest;
		}
		Record.OriginalOuter = ResolveReplacement(Record.OriginalOuter.Get());
	}
	if (PrimaryAsset != nullptr)
	{
		PrimaryAsset = ResolveReplacement(PrimaryAsset.Get());
	}
}

void FAssetDocumentPackageSnapshot::AddReferencedObjects(FReferenceCollector& Collector)
{
	// Objects an apply removed are usually marked garbage; the restore revives them,
	// so these references must not be cleared by garbage elimination.
	Collector.AllowEliminatingReferences(false);
	Collector.AddReferencedObject(Package);
	Collector.AddReferencedObject(PrimaryAsset);
	for (FObjectRecord& Record : Objects)
	{
		Collector.AddReferencedObject(Record.Object);
		Collector.AddReferencedObject(Record.OriginalOuter);
	}
	for (TPair<UObject*, TObjectPtr<UObject>>& Entry : Replacements)
	{
		Collector.AddReferencedObject(Entry.Value);
	}
	Collector.AllowEliminatingReferences(true);
}

FString FAssetDocumentPackageSnapshot::GetReferencerName() const
{
	return FString::Printf(TEXT("FAssetDocumentPackageSnapshot(%s)"), *PackageName);
}

EAssetDocumentSnapshotObjectRole FAssetDocumentPackageSnapshot::Classify(const UObject& Object) const
{
	if (&Object == PrimaryAsset.Get())
	{
		return !Rebuilders.IsEmpty() && Rebuilders[0]->IsPrimaryAssetDerived()
			? EAssetDocumentSnapshotObjectRole::Derived : EAssetDocumentSnapshotObjectRole::Authored;
	}
	// The first matching rebuilder owns classification; all matching rebuilders run.
	return Rebuilders.IsEmpty()
		? ClassifyDefault(Object)
		: Rebuilders[0]->Classify(Object);
}

TArray<UObject*> FAssetDocumentPackageSnapshot::CollectCurrentAuthoredObjects() const
{
	TArray<UObject*> Current;
	GetObjectsWithOuter(Package.Get(), Current, AssetDocumentObjects::IncludeNested, RF_NoFlags, EInternalObjectFlags::Garbage);
	Current.RemoveAll([this](const UObject* Object)
	{
		return Classify(*Object) != EAssetDocumentSnapshotObjectRole::Authored;
	});
	return Current;
}

bool FAssetDocumentPackageSnapshot::BeginRestore(FAssetDocumentContractValidationResult& OutResult)
{
	check(IsInGameThread());
	if (bConsumed)
	{
		OutResult.Diagnostics.Add(MakeSnapshotDiagnostic(
			TEXT("PackageSnapshot.AlreadyRestored"),
			TEXT("A package snapshot can be restored only once.")));
		return false;
	}
	bConsumed = true;
	if (Package == nullptr || Package->GetName() != PackageName)
	{
		OutResult.Diagnostics.Add(MakeSnapshotDiagnostic(
			TEXT("PackageSnapshot.PackageIdentityChanged"),
			FString::Printf(TEXT("Snapshot package '%s' is no longer valid."), *PackageName)));
		return false;
	}
	return true;
}

FAssetDocumentContractValidationResult FAssetDocumentPackageSnapshot::RestoreWithoutEditorCallbacks()
{
	FAssetDocumentContractValidationResult Result;
	if (!BeginRestore(Result))
	{
		return Result;
	}
	FollowReplacements();
	TArray<FAssetDocumentRuntimeDiagnostic> RestoreDiagnostics;
	RemoveNewObjects(RestoreDiagnostics);
	RestoreIdentities(RestoreDiagnostics);
	TSet<const UObject*> Current;
	for (const UObject* Object : CollectCurrentAuthoredObjects())
	{
		Current.Add(Object);
	}
	RestoreBytes(RestoreDiagnostics, false);
	// No callbacks ran, so nothing may have been created since: the set is exact.
	if (!HasErrors(RestoreDiagnostics) && VerifyObjectSet(Current, RestoreDiagnostics)
		&& CollectCurrentAuthoredObjects().Num() == Current.Num())
	{
		Package->SetDirtyFlag(bWasDirty);
		Result.bValid = true;
	}
	else if (!HasErrors(RestoreDiagnostics))
	{
		RestoreDiagnostics.Add(MakeSnapshotDiagnostic(
			TEXT("PackageSnapshot.VerificationFailed"),
			FString::Printf(TEXT("Objects of '%s' do not match the snapshot after restore."), *PackageName)));
	}
	Result.Diagnostics = MoveTemp(RestoreDiagnostics);
	return Result;
}

FAssetDocumentContractValidationResult FAssetDocumentPackageSnapshot::Restore()
{
	FAssetDocumentContractValidationResult Result;
	if (!BeginRestore(Result))
	{
		return Result;
	}

	FollowReplacements();
	TArray<FAssetDocumentRuntimeDiagnostic> RestoreDiagnostics;
	RemoveNewObjects(RestoreDiagnostics);
	RestoreIdentities(RestoreDiagnostics);
	// Objects that exist once the snapshot set is back in place; anything created
	// after this point comes from the editor's own undo callbacks or a rebuilder.
	TSet<const UObject*> BeforeCallbacks;
	for (const UObject* Object : CollectCurrentAuthoredObjects())
	{
		BeforeCallbacks.Add(Object);
	}
	RestoreBytes(RestoreDiagnostics);
	RunRebuilders(RestoreDiagnostics);
	const bool bVerified = !HasErrors(RestoreDiagnostics) && VerifyObjectSet(BeforeCallbacks, RestoreDiagnostics);
	if (bVerified)
	{
		Package->SetDirtyFlag(bWasDirty);
		Result.bValid = true;
		Result.Diagnostics = MoveTemp(RestoreDiagnostics);
		return Result;
	}

	// The in-memory restore did not verify. Reloading the package from disk is not
	// done: it replaces objects that editor systems still point to (a running
	// simulation, compile tasks, open editors), which can crash later. The objects
	// stay as restored; the failure says so and how to recover. The dirty flag goes
	// back to its captured value, so an unverified state is not offered for saving
	// unless it was already unsaved.
	Package->SetDirtyFlag(bWasDirty);
	ReportUnverifiedRestore(RestoreDiagnostics);
	Result.Diagnostics = MoveTemp(RestoreDiagnostics);
	return Result;
}

bool FAssetDocumentPackageSnapshot::HasChangedSinceCapture() const
{
	check(IsInGameThread());
	if (Package == nullptr || Package->GetName() != PackageName)
	{
		return true;
	}
	// Objects the editor replaced (reinstanced) since the capture count as their captured selves.
	TMap<UObject*, UObject*> Latest;
	TMap<UObject*, UObject*> ReplacedToCaptured;
	BuildReplacementMaps(Latest, ReplacedToCaptured);
	const TArray<UObject*> Current = CollectCurrentAuthoredObjects();
	if (Current.Num() != AuthoredObjectSet.Num())
	{
		return true;
	}
	for (UObject* Object : Current)
	{
		UObject* const* Captured = ReplacedToCaptured.Find(Object);
		if (!AuthoredObjectSet.Contains(Captured != nullptr ? *Captured : Object))
		{
			return true;
		}
	}
	for (const FObjectRecord& Record : Objects)
	{
		UObject* Object = ResolveReplacement(Record.Object.Get());
		if (Object == nullptr || Object->GetOuter() != ResolveReplacement(Record.OriginalOuter.Get()) || Object->GetFName() != Record.OriginalName)
		{
			return true;
		}
		TArray<uint8> Bytes;
		if (!SerializeObject(*Object, Bytes, ReplacedToCaptured) || Bytes != Record.Bytes)
		{
			return true;
		}
	}
	return false;
}

namespace
{
bool GCapturingDependents = false;
}

bool FAssetDocumentDependentSnapshots::IsCapturing()
{
	return GCapturingDependents;
}

TSharedRef<FAssetDocumentDependentSnapshots> FAssetDocumentDependentSnapshots::Capture(TConstArrayView<UPackage*> Packages)
{
	check(IsInGameThread());
	TSharedRef<FAssetDocumentDependentSnapshots> Out = MakeShared<FAssetDocumentDependentSnapshots>();
	TGuardValue<bool> Capturing(GCapturingDependents, true);
	TSet<const UPackage*> Seen;
	for (UPackage* Package : Packages)
	{
		if (Package == nullptr || Seen.Contains(Package))
		{
			continue;
		}
		Seen.Add(Package);
		FString Reason;
		if (FAssetDocumentPackageSnapshot::CanRestorePackage(*Package))
		{
			TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
			if (TUniquePtr<FAssetDocumentPackageSnapshot> Snapshot = FAssetDocumentPackageSnapshot::Capture(*Package, Diagnostics))
			{
				Out->Captured.Add({Package->GetName(), MoveTemp(Snapshot)});
				continue;
			}
			Reason = Diagnostics.IsEmpty() ? FString(TEXT("its snapshot could not be captured")) : Diagnostics[0].Message;
		}
		else
		{
			Reason = TEXT("no package snapshot can restore its derived objects");
		}
		Out->Uncaptured.Add({Package->GetName(), Package->IsDirty(), MoveTemp(Reason)});
	}
	return Out;
}

FAssetDocumentDependentSnapshots::FRestoreResult FAssetDocumentDependentSnapshots::Restore(TConstArrayView<UPackage*> Others)
{
	check(IsInGameThread());
	FRestoreResult Result;
	if (bConsumed)
	{
		return Result;
	}
	bConsumed = true;
	TSet<FString> Known;
	for (FCaptured& Entry : Captured)
	{
		Known.Add(Entry.PackageName);
		UPackage* Package = FindPackage(nullptr, *Entry.PackageName);
		if (!Entry.Snapshot->HasChangedSinceCapture())
		{
			// Only marked dirty (or not touched): putting the captured dirty flag back is the restore.
			if (Package != nullptr && Package->IsDirty())
			{
				Entry.Snapshot->RestoreWithoutEditorCallbacks();
			}
			continue;
		}
		FAssetDocumentContractValidationResult Restored = Entry.Snapshot->Restore();
		if (Restored.bValid)
		{
			Result.Restored.Add(Entry.PackageName);
		}
		else
		{
			Result.NotRestored.Add(FString::Printf(TEXT("%s: %s"), *Entry.PackageName,
				Restored.Diagnostics.IsEmpty() ? TEXT("the restore did not verify") : *Restored.Diagnostics[0].Message));
			for (FAssetDocumentRuntimeDiagnostic& Diagnostic : Restored.Diagnostics)
			{
				Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Warning;
				Result.Diagnostics.Add(MoveTemp(Diagnostic));
			}
		}
	}
	for (const FUncaptured& Entry : Uncaptured)
	{
		Known.Add(Entry.PackageName);
		const UPackage* Package = FindPackage(nullptr, *Entry.PackageName);
		if (Package != nullptr && Package->IsDirty() && !Entry.bWasDirty)
		{
			Result.NotRestored.Add(FString::Printf(TEXT("%s: %s"), *Entry.PackageName, *Entry.Reason));
		}
	}
	for (const UPackage* Package : Others)
	{
		if (Package != nullptr && Package->IsDirty() && !Known.Contains(Package->GetName()))
		{
			Known.Add(Package->GetName());
			Result.NotRestored.Add(FString::Printf(TEXT("%s: it was not loaded when the apply started"), *Package->GetName()));
		}
	}
	return Result;
}

void FAssetDocumentPackageSnapshot::RemoveNewObjects(
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	TArray<UObject*> NewObjects = CollectCurrentAuthoredObjects();
	NewObjects.RemoveAll([this](const UObject* Object)
	{
		return AuthoredObjectSet.Contains(Object);
	});
	// Deepest first, so every new object is detached individually.
	NewObjects.Sort([this](const UObject& A, const UObject& B)
	{
		return GetDepthInPackage(A, *Package) > GetDepthInPackage(B, *Package);
	});
	// A new graph node may be linked to a surviving node (e.g. wired to an existing
	// event node). Pin links are raw pointers on both sides: restoring the surviving
	// node's bytes rewrites only its side, and a later reconstruct trashes its pins,
	// leaving the discarded node pointing at freed pins when GC destroys it. Unlink
	// discarded nodes symmetrically first (the surviving side is then restored from
	// its bytes like any other change).
	for (UObject* Object : NewObjects)
	{
		if (UEdGraphNode* Node = Cast<UEdGraphNode>(Object))
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin != nullptr && !Pin->WasTrashed())
				{
					Pin->BreakAllPinLinks();
				}
			}
		}
	}
	for (UObject* Object : NewObjects)
	{
		if (Object->IsRooted())
		{
			Object->RemoveFromRoot();
		}
		Object->ClearFlags(RF_Public | RF_Standalone);
		Object->SetFlags(RF_Transient);
		const FName TransientName = MakeUniqueObjectName(
			GetTransientPackage(), Object->GetClass(), Object->GetFName());
		if (!Object->Rename(*TransientName.ToString(), GetTransientPackage(), SnapshotRenameFlags))
		{
			OutDiagnostics.Add(MakeSnapshotDiagnostic(
				TEXT("PackageSnapshot.NewObjectRemovalFailed"),
				FString::Printf(TEXT("Failed to detach new object '%s'."), *Object->GetPathName())));
		}
		Object->MarkAsGarbage();
	}
}

void FAssetDocumentPackageSnapshot::RestoreIdentities(
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	auto TryRestoreName = [](FObjectRecord& Record)
	{
		UObject* Object = Record.Object.Get();
		const FString Name = Record.OriginalName.ToString();
		if (!Object->Rename(*Name, Record.OriginalOuter.Get(), SnapshotRenameFlags | REN_Test))
		{
			return false;
		}
		return Object->Rename(*Name, Record.OriginalOuter.Get(), SnapshotRenameFlags);
	};

	// Objects whose original slot is still occupied (e.g. by a swapped sibling)
	// are parked in the transient package and retried once the slots are free.
	TArray<FObjectRecord*> Deferred;
	for (FObjectRecord& Record : Objects)
	{
		UObject* Object = Record.Object.Get();
		if (!Object || Record.OriginalOuter == nullptr)
		{
			OutDiagnostics.Add(MakeSnapshotDiagnostic(
				TEXT("PackageSnapshot.ObjectLost"),
				FString::Printf(TEXT("Snapshot object '%s' is no longer available."), *Record.OriginalName.ToString())));
			continue;
		}
		if (Object->IsUnreachable() || Object->HasAnyInternalFlags(EInternalObjectFlags::Garbage))
		{
			Object->ClearGarbage();
			Object->ClearInternalFlags(EInternalObjectFlags::Unreachable);
		}
		if (Object->GetOuter() != Record.OriginalOuter.Get() || Object->GetFName() != Record.OriginalName)
		{
			if (!TryRestoreName(Record))
			{
				const FName ParkingName = MakeUniqueObjectName(
					GetTransientPackage(), Object->GetClass(), Object->GetFName());
				Object->Rename(*ParkingName.ToString(), GetTransientPackage(), SnapshotRenameFlags);
				Deferred.Add(&Record);
			}
		}

		const EObjectFlags ImmutableFlags = RF_MarkAsNative | RF_MarkAsRootSet | RF_MirroredGarbage;
		const EObjectFlags MutableMask = static_cast<EObjectFlags>(RF_AllFlags & ~ImmutableFlags);
		const EObjectFlags CurrentFlags = static_cast<EObjectFlags>(Object->GetFlags() & MutableMask);
		const EObjectFlags OriginalFlags = static_cast<EObjectFlags>(Record.OriginalFlags & MutableMask);
		Object->ClearFlags(static_cast<EObjectFlags>(CurrentFlags & ~OriginalFlags));
		Object->SetFlags(static_cast<EObjectFlags>(OriginalFlags & ~CurrentFlags));
		if (Record.bWasRooted && !Object->IsRooted())
		{
			Object->AddToRoot();
		}
		else if (!Record.bWasRooted && Object->IsRooted())
		{
			Object->RemoveFromRoot();
		}
	}
	for (FObjectRecord* Record : Deferred)
	{
		if (!TryRestoreName(*Record))
		{
			OutDiagnostics.Add(MakeSnapshotDiagnostic(
				TEXT("PackageSnapshot.IdentityRestoreFailed"),
				FString::Printf(TEXT("Failed to restore the outer/name of '%s'."), *Record->OriginalName.ToString())));
		}
	}
}

void FAssetDocumentPackageSnapshot::RestoreBytes(
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics,
	bool bEditorCallbacks)
{
	// Only objects whose serialized state differs are written back and notified,
	// mirroring UE's transaction buffer which only touches recorded objects.
	TArray<UObject*> Restored;
	// A graph node whose pins were reallocated since the capture (e.g. a Control Rig
	// editor node reconstructed by a compile: new pins, new ids) gets the captured
	// pins back as new pins and frees the current ones. The pin serializer unlinks
	// a freed pin by asking the transaction buffer whether the peer node is being
	// transacted (outside a transaction: an ensure), and only on the freed pin's
	// side of a surviving peer. Unlink such pins first, symmetrically: the peers then
	// differ from their captured bytes and are restored below, links included.
	for (FObjectRecord& Record : Objects)
	{
		UEdGraphNode* Node = Cast<UEdGraphNode>(Record.Object.Get());
		if (!Node)
		{
			continue;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin != nullptr && !Pin->WasTrashed() && !Pin->LinkedTo.IsEmpty() && !Record.PinIds.Contains(Pin->PinId))
			{
				Pin->BreakAllPinLinks();
			}
		}
	}
	// References to objects replaced since the capture (reinstanced templates) are read as their
	// replacements: writing back a captured pointer would revive an instance of an outdated class.
	TMap<UObject*, UObject*> Latest;
	TMap<UObject*, UObject*> ReplacedToCaptured;
	BuildReplacementMaps(Latest, ReplacedToCaptured);
	for (FObjectRecord& Record : Objects)
	{
		UObject* Object = Record.Object.Get();
		if (!Object)
		{
			continue;
		}
		TArray<uint8> Current;
		if (SerializeObject(*Object, Current, ReplacedToCaptured) && Current == Record.Bytes)
		{
			continue;
		}
		if (UEnum* Enum = Cast<UEnum>(Object))
		{
			// UEnum's loading serializer expects an empty name table
			// (UEnum::FNameData::Initialize checks it) and registers the loaded names
			// globally. Clear the table and its global registrations first (the base
			// SetEnums, so a UUserDefinedEnum adds no MAX entry); flags are not serialized.
			EEnumFlags Flags = EEnumFlags::None;
			for (const EEnumFlags Flag : {EEnumFlags::Flags, EEnumFlags::NewerVersionExists})
			{
				if (Enum->HasAnyEnumFlags(Flag))
				{
					Flags |= Flag;
				}
			}
			TArray<TPair<FName, int64>> NoNames;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
			Enum->UEnum::SetEnums(NoNames, Enum->GetCppForm(), Enum->GetUnderlyingType(), Flags, UEnum::EAddMaxKeyIfMissing::No);
#else
			Enum->UEnum::SetEnums(NoNames, Enum->GetCppForm(), Flags, false);
#endif
		}
		FSnapshotObjectReader Reader(Record.Bytes, Latest);
		Object->Serialize(Reader);
		if (Reader.IsError())
		{
			OutDiagnostics.Add(MakeSnapshotDiagnostic(
				TEXT("PackageSnapshot.ObjectRestoreFailed"),
				FString::Printf(TEXT("Failed to restore serialized state of '%s'."), *Object->GetPathName())));
		}
		Restored.Add(Object);
	}
	if (!bEditorCallbacks)
	{
		return;
	}
	// Deepest first, like the editor transaction path: derived caches (graph
	// pins, Blackboard key ids, ...) rebuild from the restored authored state.
	for (int32 Index = Restored.Num() - 1; Index >= 0; --Index)
	{
		Restored[Index]->PostEditUndo();
	}
}

void FAssetDocumentPackageSnapshot::RunRebuilders(
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	UObject* Primary = PrimaryAsset.Get();
	if (!Primary)
	{
		return;
	}
	for (int32 Index = 0; Index < Rebuilders.Num(); ++Index)
	{
		const TSharedRef<IAssetDocumentDerivedStateRebuilder>& Rebuilder = Rebuilders[Index];
		const TSharedPtr<void>& State = RebuilderStates[Index];
		FAssetDocumentContractValidationResult Rebuilt = State.IsValid()
			? Rebuilder->RebuildFromState(*Primary, State)
			: Rebuilder->Rebuild(*Primary);
		OutDiagnostics.Append(Rebuilt.Diagnostics);
		if (!Rebuilt.bValid && !HasErrors(Rebuilt.Diagnostics))
		{
			OutDiagnostics.Add(MakeSnapshotDiagnostic(
				TEXT("PackageSnapshot.RebuildFailed"),
				FString::Printf(TEXT("Derived-state rebuilder '%s' failed for '%s'."),
					*Rebuilder->GetId().ToString(), *Primary->GetPathName())));
		}
	}
}

bool FAssetDocumentPackageSnapshot::VerifyObjectSet(
	const TSet<const UObject*>& BeforeCallbacks,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics) const
{
	// What differs, named for the diagnostic (a few entries are enough to act on).
	TArray<FString> Differences;
	for (const FObjectRecord& Record : Objects)
	{
		const UObject* Object = Record.Object.Get();
		if (Object == nullptr || Object->HasAnyInternalFlags(EInternalObjectFlags::Garbage))
		{
			Differences.Add(FString::Printf(TEXT("'%s' was not revived"), *Record.OriginalName.ToString()));
		}
		else if (Object->GetOuter() != Record.OriginalOuter.Get() || Object->GetFName() != Record.OriginalName)
		{
			Differences.Add(FString::Printf(TEXT("'%s' is at '%s'"), *Record.OriginalName.ToString(), *Object->GetPathName()));
		}
	}
	// Before the undo callbacks the package must hold exactly the snapshot set.
	for (const UObject* Object : BeforeCallbacks)
	{
		if (!AuthoredObjectSet.Contains(Object))
		{
			Differences.Add(FString::Printf(TEXT("'%s' (%s) is not in the snapshot"),
				*Object->GetPathName(), *Object->GetClass()->GetName()));
		}
	}
	bool bExact = Differences.IsEmpty() && BeforeCallbacks.Num() == AuthoredObjectSet.Num();
	// PostEditUndo and rebuilders regenerate editor-derived subobjects from the
	// restored authored state, as the editor's own Undo does: e.g. a PaperSprite
	// creates a fresh BodySetup from its collision geometry on every PostEditChange.
	// Such objects are functionally equivalent to the replaced ones, not residue;
	// the snapshot objects they replace stay intact but unreferenced.
	int32 Rebuilt = 0;
	for (const UObject* Object : CollectCurrentAuthoredObjects())
	{
		Rebuilt += !BeforeCallbacks.Contains(Object) ? 1 : 0;
	}
	UE_CLOG(Rebuilt > 0, LogAssetDocument, Verbose,
		TEXT("Package snapshot restore of '%s': the editor rebuilt %d derived subobject(s) from the restored state."),
		*PackageName, Rebuilt);
#if WITH_DEV_AUTOMATION_TESTS
	bExact &= !bForceVerificationFailure;
#endif
	if (!bExact)
	{
		constexpr int32 MaxNamed = 5;
		FString Detail = FString::Join(TArrayView<const FString>(Differences).Left(MaxNamed), TEXT("; "));
		if (Differences.Num() > MaxNamed)
		{
			Detail += FString::Printf(TEXT("; +%d more"), Differences.Num() - MaxNamed);
		}
		OutDiagnostics.Add(MakeSnapshotDiagnostic(
			TEXT("PackageSnapshot.VerificationFailed"),
			FString::Printf(TEXT("Objects of '%s' do not match the snapshot after restore%s%s."), *PackageName,
				Detail.IsEmpty() ? TEXT("") : TEXT(": "), *Detail)));
	}
	return bExact;
}

void FAssetDocumentPackageSnapshot::ReportUnverifiedRestore(
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics) const
{
	const bool bDiskIsPreApply = !bWasDirty && !Package->HasAnyPackageFlags(PKG_InMemoryOnly)
		&& FPackageName::DoesPackageExist(PackageName);
	const FString Remedy = bDiskIsPreApply
		? TEXT("The file on disk is unchanged: close its editor and reload the asset from disk ")
		  TEXT("(Content Browser: Asset Actions > Reload) or restart the editor before applying to it again.")
		: TEXT("It had unsaved changes before the apply, so disk does not hold its pre-apply state: ")
		  TEXT("check it in its editor (Undo may help) before saving or applying to it again.");
	OutDiagnostics.Insert(MakeSnapshotDiagnostic(
		TEXT("PackageSnapshot.RestoreFailed"),
		FString::Printf(TEXT("Package '%s' could not be verified as restored to its pre-apply state in memory "
			"(the diagnostics after this one say what differs). %s"), *PackageName, *Remedy)), 0);
}
