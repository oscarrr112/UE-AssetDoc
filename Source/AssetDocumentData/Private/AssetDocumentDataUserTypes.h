// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentDataPrimitives.h"

class FAssetDocumentDependentSnapshots;
class IAssetDocumentDerivedStateRebuilder;
class UBlueprint;
class UPackage;
class UUserDefinedEnum;
class UUserDefinedStruct;

// User Defined Struct / User Defined Enum primitives and their dependents rule
// (docs/architecture/asset-document-user-defined-types-matrix.md).
//
// The Struct and Enum editors (FStructureEditorUtils / FEnumEditorUtils) update every loaded
// asset that uses the type when it changes: data tables re-read their rows
// (UDataTable::CleanBeforeStructChange / RestoreAfterStructChange), Blueprints are marked
// structurally modified and their nodes reconstructed, structs that contain the type are
// recompiled, enum values in objects are re-resolved by enumerator name. AssetDocument runs
// exactly those editor operations and adds the rule for the packages they touch:
//   * an edit that updates dependents is refused while a loaded dependent has unsaved changes
//     (UserDefinedStruct/Enum.DependentHasUnsavedChanges): its unsaved state could not be
//     restored if the apply failed;
//   * after a successful edit, dependents that were saved stay clean: their saved files are
//     migrated by the same tagged-property load when they are next loaded, so the in-memory
//     state equals what reloading them gives (no unsaved work exists). Each one is reported;
//   * if the apply fails, Core restores the type from its package snapshot and the rebuilders
//     below recompile it and restore the dependents the apply changed from their own snapshots,
//     taken with the type's (in memory, as the type itself; nothing is reloaded from disk).
namespace UE::AssetDocument::Graph::Data
{
bool HandleUserStructRead(FCall& Call, FObjectBuilder& Result);
bool HandleUserStructEdit(FCall& Call, FObjectBuilder& Result);
bool HandleUserEnumRead(FCall& Call, FObjectBuilder& Result);
bool HandleUserEnumEdit(FCall& Call, FObjectBuilder& Result);

namespace UserTypes
{
struct FDependent
{
	TWeakObjectPtr<UPackage> Package;
	FString PackageName;
	// DataTable, Blueprint, UserDefinedStruct.
	FString Kind;
	bool bDirty = false;
};

// Loaded assets (other than the type's own package) the editor updates when the type changes:
// the same searches FUserDefinedStructureCompilerUtils / FStructureEditorUtils::BroadcastPreChange
// and FEnumEditorUtils::BroadcastChanges make. Structs that contain the type are followed (their
// own dependents are recompiled too).
TArray<FDependent> FindLoadedDependents(const UUserDefinedStruct& Struct);
TArray<FDependent> FindLoadedDependents(const UUserDefinedEnum& Enum);

FValue DescribeDependents(TConstArrayView<FDependent> Dependents);

// Wraps one edit call that may update dependents (see the rule above).
class FDependentUpdate
{
public:
	FDependentUpdate(UObject& Asset, TArray<FDependent> InPredicted);
	// Fails the call (Code at ops) when a predicted dependent has unsaved changes.
	bool Admit(FReader& Args, const TCHAR* Code);
	// After every op succeeded: compiles the Blueprints the change left needing a compile, keeps
	// saved dependents clean and lists every package the editor marked dirty or whose Blueprint
	// failed that compile, as dependents[{package, kind, kept_clean, compile_failed}].
	FValue Finish();

private:
	UPackage* Own = nullptr;
	TArray<FDependent> Predicted;
	TSet<const UPackage*> DirtyBefore;
	TArray<TWeakObjectPtr<UBlueprint>> CompiledBefore;
};

// Snapshots of the dependents' packages, taken with the type's package snapshot (null inside
// another dependents capture). A failed apply restores those the apply changed in memory
// (RestoreDependents), whether or not the editor marked them dirty (a shape change migrates their
// data in memory without always doing so); nothing is reloaded from disk.
TSharedPtr<FAssetDocumentDependentSnapshots> CaptureDependents(TConstArrayView<FDependent> Dependents);
// Current: the type's dependents found now (one loaded during the apply cannot be restored and is
// reported if the apply dirtied it).
void RestoreDependents(FAssetDocumentDependentSnapshots& Snapshots, TConstArrayView<FDependent> Current,
	TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeUserStructRebuilder();
TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeUserEnumRebuilder();
} // namespace UserTypes
} // namespace UE::AssetDocument::Graph::Data
