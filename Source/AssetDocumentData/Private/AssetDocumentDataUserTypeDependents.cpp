// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Dependents of User Defined Structs / Enums and the Core package-snapshot rebuilders for both
// (the rule is described in AssetDocumentDataUserTypes.h and in
// docs/architecture/asset-document-user-defined-types-matrix.md).

#include "AssetDocumentDataUserTypes.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/DataTable.h"
#include "Engine/UserDefinedEnum.h"
#include "K2Node.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/PackageName.h"
#include "NodeDependingOnEnumInterface.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/EnumProperty.h"
#include "UObject/FieldIterator.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

namespace UE::AssetDocument::Graph::Data::UserTypes
{
namespace
{
bool IsAssetPackage(const UPackage* Package)
{
	return Package != nullptr && Package != GetTransientPackage() && !Package->HasAnyPackageFlags(PKG_CompiledIn)
		&& FPackageName::IsValidLongPackageName(Package->GetName()) && !Package->GetName().StartsWith(TEXT("/Temp/"));
}

bool IsCompiled(const UBlueprint& Blueprint)
{
	return Blueprint.Status == BS_UpToDate || Blueprint.Status == BS_UpToDateWithWarnings;
}

class FCollector
{
public:
	explicit FCollector(const UObject& Type) : Own(Type.GetOutermost()) {}

	void Add(const UObject* Object, const TCHAR* Kind)
	{
		UPackage* Package = Object != nullptr ? Object->GetOutermost() : nullptr;
		if (Package == Own || !IsAssetPackage(Package) || Seen.Contains(Package))
		{
			return;
		}
		Seen.Add(Package);
		FDependent& Dependent = Out.AddDefaulted_GetRef();
		Dependent.Package = Package;
		Dependent.PackageName = Package->GetName();
		Dependent.Kind = Kind;
		Dependent.bDirty = Package->IsDirty();
	}

	void AddPropertyOwner(const FProperty& Property, TArray<const UUserDefinedStruct*>* Structs)
	{
		if (const UBlueprintGeneratedClass* Class = Cast<UBlueprintGeneratedClass>(Property.GetOwnerClass()))
		{
			Add(Cast<UBlueprint>(Class->ClassGeneratedBy), TEXT("Blueprint"));
		}
		else if (const UUserDefinedStruct* Owner = Cast<UUserDefinedStruct>(Property.GetOwnerStruct()))
		{
			// The compiler skips its own temporary duplicates (STRUCT_REINST_*).
			if (IsValid(Owner) && Owner->GetOutermost() != GetTransientPackage()
				&& Owner->Status != EUserDefinedStructureStatus::UDSS_Duplicate && !Owner->PrimaryStruct.IsValid())
			{
				Add(Owner, TEXT("UserDefinedStruct"));
				if (Structs != nullptr && !Structs->Contains(Owner))
				{
					Structs->Add(Owner);
				}
			}
		}
	}

	UPackage* Own = nullptr;
	TSet<const UPackage*> Seen;
	TArray<FDependent> Out;
};

// FUserDefinedStructureCompilerUtils::CompileStruct and FStructureEditorUtils::BroadcastPreChange:
// data tables with the struct as row struct, Blueprints with properties of it or nodes that use it,
// structs that contain it (which are recompiled too, so their dependents follow).
void CollectStructDependents(const UUserDefinedStruct& Root, FCollector& Collector)
{
	TArray<const UUserDefinedStruct*> Structs = {&Root};
	for (int32 Index = 0; Index < Structs.Num(); ++Index)
	{
		const UUserDefinedStruct* Struct = Structs[Index];
		for (TObjectIterator<UDataTable> It(RF_ClassDefaultObject | RF_ArchetypeObject); It; ++It)
		{
			if (IsValid(*It) && It->RowStruct == Struct)
			{
				Collector.Add(*It, TEXT("DataTable"));
			}
		}
		for (TAllFieldsIterator<FStructProperty> It(RF_NoFlags, EInternalObjectFlags::Garbage); It; ++It)
		{
			if (It->Struct == Struct)
			{
				Collector.AddPropertyOwner(**It, &Structs);
			}
		}
		for (TObjectIterator<UBlueprint> It(RF_ClassDefaultObject); It; ++It)
		{
			UBlueprint* Blueprint = *It;
			if (!IsValid(Blueprint) || Collector.Seen.Contains(Blueprint->GetOutermost()))
			{
				continue;
			}
			FBlueprintEditorUtils::EnsureCachedDependenciesUpToDate(Blueprint);
			for (const TWeakObjectPtr<UStruct>& Dependency : Blueprint->CachedUDSDependencies)
			{
				if (Dependency.Get() == Struct)
				{
					Collector.Add(Blueprint, TEXT("Blueprint"));
					break;
				}
			}
		}
	}
}
} // namespace

TArray<FDependent> FindLoadedDependents(const UUserDefinedStruct& Struct)
{
	FCollector Collector(Struct);
	CollectStructDependents(Struct, Collector);
	return MoveTemp(Collector.Out);
}

// FEnumEditorUtils::BroadcastChanges: owners of byte/enum properties of the enum (Blueprint classes,
// structs, which are recompiled), nodes that depend on the enum or have pins of it, and Blueprint
// int properties using it as their bitmask enum.
TArray<FDependent> FindLoadedDependents(const UUserDefinedEnum& Enum)
{
	FCollector Collector(Enum);
	TArray<const UUserDefinedStruct*> Structs;
	for (TAllFieldsIterator<FByteProperty> It(RF_NoFlags, EInternalObjectFlags::Garbage); It; ++It)
	{
		if (It->Enum == &Enum)
		{
			Collector.AddPropertyOwner(**It, &Structs);
		}
	}
	for (TAllFieldsIterator<FEnumProperty> It(RF_NoFlags, EInternalObjectFlags::Garbage); It; ++It)
	{
		if (It->GetEnum() == &Enum)
		{
			Collector.AddPropertyOwner(**It, &Structs);
		}
	}
	for (TAllFieldsIterator<FIntProperty> It(RF_NoFlags, EInternalObjectFlags::Garbage); It; ++It)
	{
		if (It->HasMetaData(FBlueprintMetadata::MD_BitmaskEnum) && It->GetMetaData(FBlueprintMetadata::MD_BitmaskEnum) == Enum.GetName())
		{
			Collector.AddPropertyOwner(**It, nullptr);
		}
	}
	for (TObjectIterator<UEdGraphNode> It(RF_Transient); It; ++It)
	{
		UEdGraphNode* Node = *It;
		bool bDepends = false;
		if (const INodeDependingOnEnumInterface* Dependent = Cast<INodeDependingOnEnumInterface>(Node))
		{
			bDepends = Dependent->GetEnum() == &Enum;
		}
		for (int32 Index = 0; !bDepends && Index < Node->Pins.Num(); ++Index)
		{
			bDepends = Node->Pins[Index] != nullptr && Node->Pins[Index]->PinType.PinSubCategoryObject.Get() == &Enum;
		}
		if (bDepends && IsValid(Node))
		{
			Collector.Add(FBlueprintEditorUtils::FindBlueprintForNode(Node), TEXT("Blueprint"));
		}
	}
	for (const UUserDefinedStruct* Struct : Structs)
	{
		CollectStructDependents(*Struct, Collector);
	}
	return MoveTemp(Collector.Out);
}

FValue DescribeDependents(TConstArrayView<FDependent> Dependents)
{
	TArray<FValue> Rows;
	for (const FDependent& Dependent : Dependents)
	{
		Rows.Add(FObjectBuilder().AddString(TEXT("package"), Dependent.PackageName).AddString(TEXT("kind"), Dependent.Kind)
			.AddBool(TEXT("dirty"), Dependent.bDirty).Build());
	}
	return MakeArray(MoveTemp(Rows));
}

FDependentUpdate::FDependentUpdate(UObject& Asset, TArray<FDependent> InPredicted)
	: Own(Asset.GetOutermost()), Predicted(MoveTemp(InPredicted))
{
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		if (It->IsDirty())
		{
			DirtyBefore.Add(*It);
		}
	}
	for (TObjectIterator<UBlueprint> It(RF_ClassDefaultObject); It; ++It)
	{
		if (IsValid(*It) && IsAssetPackage(It->GetOutermost()) && It->GetOutermost() != Own && IsCompiled(**It))
		{
			CompiledBefore.Add(*It);
		}
	}
}

bool FDependentUpdate::Admit(FReader& Args, const TCHAR* Code)
{
	TArray<FString> Unsaved;
	for (const FDependent& Dependent : Predicted)
	{
		if (Dependent.bDirty)
		{
			Unsaved.Add(FString::Printf(TEXT("%s (%s)"), *Dependent.PackageName, *Dependent.Kind));
		}
	}
	if (Unsaved.IsEmpty())
	{
		return true;
	}
	return Args.Fail(Code, FString::Printf(TEXT("This change makes the editor update every loaded asset that uses %s, and %s %s unsaved changes "
		"that could not be restored if the apply failed. Save (or revert) %s first, then apply again."), *Own->GetName(),
		*FString::Join(Unsaved, TEXT(", ")), Unsaved.Num() == 1 ? TEXT("has") : TEXT("have"), Unsaved.Num() == 1 ? TEXT("it") : TEXT("them")),
		TEXT("ops"));
}

FValue FDependentUpdate::Finish()
{
	// A struct change leaves the Blueprints that use it needing a compile: until then their
	// generated classes use the struct's temporary copy (old layout) and have no function bytecode.
	// An enum change can do the same. They are compiled here (the editor's Compile) so no instance
	// or later apply sees the replaced types.
	TSet<const UPackage*> CompileFailed;
	for (const TWeakObjectPtr<UBlueprint>& Weak : CompiledBefore)
	{
		UBlueprint* Blueprint = Weak.Get();
		if (Blueprint == nullptr || IsCompiled(*Blueprint) || Blueprint->bBeingCompiled)
		{
			continue;
		}
		UPackage* Package = Blueprint->GetOutermost();
		const bool bWasDirty = Package->IsDirty();
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
		Package->SetDirtyFlag(bWasDirty);
		if (Blueprint->Status == BS_Error)
		{
			CompileFailed.Add(Package);
		}
	}
	TArray<FValue> Rows;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		UPackage* Package = *It;
		const bool bCompileFailed = CompileFailed.Contains(Package);
		if (Package == Own || !IsAssetPackage(Package) || (!bCompileFailed && (!Package->IsDirty() || DirtyBefore.Contains(Package))))
		{
			continue;
		}
		const FDependent* Dependent = Predicted.FindByPredicate([Package](const FDependent& Item) { return Item.Package.Get() == Package; });
		// A saved dependent the editor only migrated to the new layout: its file loads to the same state.
		const bool bKeepClean = !Package->IsDirty() || (Dependent != nullptr && !Package->HasAnyPackageFlags(PKG_InMemoryOnly)
			&& FPackageName::DoesPackageExist(Package->GetName()) && !DirtyBefore.Contains(Package));
		if (bKeepClean)
		{
			Package->SetDirtyFlag(false);
		}
		Rows.Add(FObjectBuilder().AddString(TEXT("package"), Package->GetName())
			.AddString(TEXT("kind"), Dependent != nullptr ? Dependent->Kind : FString(bCompileFailed ? TEXT("Blueprint") : TEXT("Other")))
			.AddBool(TEXT("kept_clean"), bKeepClean).AddBool(TEXT("compile_failed"), bCompileFailed).Build());
	}
	return MakeArray(MoveTemp(Rows));
}

TSharedPtr<FAssetDocumentDependentSnapshots> CaptureDependents(TConstArrayView<FDependent> Dependents)
{
	// A dependent's own capture (a struct that contains the type) captures no dependents: the
	// outer list already holds the transitive ones.
	if (FAssetDocumentDependentSnapshots::IsCapturing())
	{
		return nullptr;
	}
	TArray<UPackage*> Packages;
	for (const FDependent& Dependent : Dependents)
	{
		if (UPackage* Package = Dependent.Package.Get())
		{
			Packages.Add(Package);
		}
	}
	return FAssetDocumentDependentSnapshots::Capture(Packages);
}

void RestoreDependents(FAssetDocumentDependentSnapshots& Snapshots, TConstArrayView<FDependent> Current, TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	TArray<UPackage*> Others;
	for (const FDependent& Dependent : Current)
	{
		Others.Add(Dependent.Package.Get());
	}
	FAssetDocumentDependentSnapshots::FRestoreResult Restored = Snapshots.Restore(Others);
	if (!Restored.Restored.IsEmpty())
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic = OutDiagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/PackageSnapshot");
		Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Warning;
		Diagnostic.Code = TEXT("UserDefinedType.DependentsRestored");
		Diagnostic.Message = FString::Printf(TEXT("Restored the assets the editor had updated for the change to their state before the apply: %s."),
			*FString::Join(Restored.Restored, TEXT(", ")));
	}
	if (!Restored.NotRestored.IsEmpty())
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic = OutDiagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/PackageSnapshot");
		Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Warning;
		Diagnostic.Code = TEXT("UserDefinedType.DependentNotRestored");
		Diagnostic.Message = FString::Printf(TEXT("These assets keep the editor's in-memory update for the change (unsaved; revert them from disk "
			"if that is not wanted): %s."), *FString::Join(Restored.NotRestored, TEXT("; ")));
	}
	OutDiagnostics.Append(MoveTemp(Restored.Diagnostics));
}

namespace
{
// Compiled form of a struct (member properties, their flags and metadata, the default instance):
// equal before and after a failed apply exactly when the apply changed nothing that compiles.
FString CompiledSignature(const UUserDefinedStruct& Struct)
{
	FString Out = FString::Printf(TEXT("%d;"), static_cast<int32>(Struct.Status.GetValue()));
	const uint8* Defaults = Struct.GetDefaultInstance();
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		Out += FString::Printf(TEXT("%s|%s|%llu|"), *It->GetName(), *It->GetCPPType(), static_cast<uint64>(It->PropertyFlags));
		if (const TMap<FName, FString>* Meta = It->GetMetaDataMap())
		{
			TArray<FName> Keys;
			Meta->GenerateKeyArray(Keys);
			Keys.Sort(FNameLexicalLess());
			for (const FName& Key : Keys)
			{
				Out += Key.ToString() + TEXT("=") + Meta->FindChecked(Key) + TEXT(";");
			}
		}
		if (Defaults != nullptr)
		{
			FString Text;
			It->ExportText_InContainer(0, Text, Defaults, Defaults, nullptr, PPF_None);
			Out += Text;
		}
		Out += TEXT("\n");
	}
	return Out;
}

struct FStructState
{
	FString Signature;
	TSharedPtr<FAssetDocumentDependentSnapshots> Dependents;
};

// The struct object is compiled from its editor data (UUserDefinedStructEditorData, restored by
// Core like any authored object; the editor's Undo restores the same object). Rebuild does what
// the editor does after that undo (UUserDefinedStructEditorData::PostUndo ->
// FStructureEditorUtils::OnStructureChanged), then restores the dependents the change had updated
// from their snapshots (captured with the struct's).
class FUserStructRebuilder final : public IAssetDocumentDerivedStateRebuilder
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentData.UserDefinedStruct"); }
	virtual bool Supports(const UObject& PrimaryAsset) const override { return PrimaryAsset.IsA<UUserDefinedStruct>(); }
	virtual bool IsPrimaryAssetDerived() const override { return true; }

	virtual TSharedPtr<void> CaptureDerivedState(UObject& PrimaryAsset) const override
	{
		const UUserDefinedStruct& Struct = *CastChecked<UUserDefinedStruct>(&PrimaryAsset);
		TSharedRef<FStructState> State = MakeShared<FStructState>();
		State->Signature = CompiledSignature(Struct);
		State->Dependents = CaptureDependents(FindLoadedDependents(Struct));
		return State;
	}

	virtual FAssetDocumentContractValidationResult Rebuild(UObject& PrimaryAsset) override
	{
		return RebuildFromState(PrimaryAsset, nullptr);
	}

	virtual FAssetDocumentContractValidationResult RebuildFromState(UObject& PrimaryAsset, const TSharedPtr<void>& State) override
	{
		FAssetDocumentContractValidationResult Result;
		UUserDefinedStruct& Struct = *CastChecked<UUserDefinedStruct>(&PrimaryAsset);
		UUserDefinedStructEditorData* EditorData = Cast<UUserDefinedStructEditorData>(Struct.EditorData);
		if (EditorData == nullptr)
		{
			Result.Diagnostics.Add({TEXT("/PackageSnapshot"), TEXT("UserDefinedStruct.EditorDataMissing"),
				FString::Printf(TEXT("%s has no editor data to rebuild from."), *Struct.GetPathName())});
			return Result;
		}
		// The restored editor data registered for the editor's next undo (PostEditUndo); the
		// recompile below is that step.
		if (GEditor != nullptr)
		{
			GEditor->UnregisterForUndo(EditorData);
		}
		const FStructState* Captured = static_cast<const FStructState*>(State.Get());
		const bool bShapeChanged = Captured == nullptr || CompiledSignature(Struct) != Captured->Signature;
		if (bShapeChanged)
		{
			FStructureEditorUtils::OnStructureChanged(&Struct, FStructureEditorUtils::Unknown);
		}
		// ChangeTooltip writes the struct's tooltip metadata without a compile.
		Struct.SetMetaData(FBlueprintMetadata::MD_Tooltip, *FStructureEditorUtils::GetTooltip(&Struct));
		if (Captured != nullptr && Captured->Dependents.IsValid())
		{
			RestoreDependents(*Captured->Dependents, FindLoadedDependents(Struct), Result.Diagnostics);
		}
		// A struct that did not compile before the apply (e.g. a member of a deleted type) may
		// stay that way.
		Result.bValid = Struct.Status == EUserDefinedStructureStatus::UDSS_UpToDate
			|| (Captured != nullptr && Captured->Signature.StartsWith(FString::Printf(TEXT("%d;"), static_cast<int32>(Struct.Status.GetValue()))));
		if (!Result.bValid)
		{
			Result.Diagnostics.Add({TEXT("/PackageSnapshot"), TEXT("UserDefinedStruct.RebuildFailed"),
				FString::Printf(TEXT("%s did not compile after the restore: %s"), *Struct.GetPathName(), *Struct.ErrorMessage)});
		}
		return Result;
	}
};

struct FEnumState
{
	TMap<FName, FString> MetaData;
	TSharedPtr<FAssetDocumentDependentSnapshots> Dependents;
};

// The enum object is authored (Core restores it as the editor's Undo does and calls
// UUserDefinedEnum::PostEditUndo -> FEnumEditorUtils::PostEditUndo). Enumerator descriptions and
// the Bitflags flag are package metadata, which the snapshot does not hold: they are restored
// here, then the dependents the change had updated are restored from their snapshots.
class FUserEnumRebuilder final : public IAssetDocumentDerivedStateRebuilder
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentData.UserDefinedEnum"); }
	virtual bool Supports(const UObject& PrimaryAsset) const override { return PrimaryAsset.IsA<UUserDefinedEnum>(); }

	virtual TSharedPtr<void> CaptureDerivedState(UObject& PrimaryAsset) const override
	{
		const UUserDefinedEnum& Enum = *CastChecked<UUserDefinedEnum>(&PrimaryAsset);
		TSharedRef<FEnumState> State = MakeShared<FEnumState>();
		if (const TMap<FName, FString>* Map = FMetaData::GetMapForObject(&Enum))
		{
			State->MetaData = *Map;
		}
		State->Dependents = CaptureDependents(FindLoadedDependents(Enum));
		return State;
	}

	virtual FAssetDocumentContractValidationResult Rebuild(UObject& PrimaryAsset) override
	{
		return RebuildFromState(PrimaryAsset, nullptr);
	}

	virtual FAssetDocumentContractValidationResult RebuildFromState(UObject& PrimaryAsset, const TSharedPtr<void>& State) override
	{
		FAssetDocumentContractValidationResult Result;
		UUserDefinedEnum& Enum = *CastChecked<UUserDefinedEnum>(&PrimaryAsset);
		if (const FEnumState* Captured = static_cast<const FEnumState*>(State.Get()))
		{
			TMap<FName, FString>* Current = FMetaData::GetMapForObject(&Enum);
			if (Current == nullptr ? !Captured->MetaData.IsEmpty() : !Current->OrderIndependentCompareEqual(Captured->MetaData))
			{
				if (Current != nullptr)
				{
					*Current = Captured->MetaData;
				}
				else
				{
					Enum.GetOutermost()->GetMetaData().SetObjectValues(&Enum, Captured->MetaData);
				}
			}
			if (Captured->Dependents.IsValid())
			{
				RestoreDependents(*Captured->Dependents, FindLoadedDependents(Enum), Result.Diagnostics);
			}
		}
		Result.bValid = true;
		return Result;
	}
};
} // namespace

TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeUserStructRebuilder()
{
	return MakeShared<FUserStructRebuilder>();
}

TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeUserEnumRebuilder()
{
	return MakeShared<FUserEnumRebuilder>();
}
} // namespace UE::AssetDocument::Graph::Data::UserTypes
