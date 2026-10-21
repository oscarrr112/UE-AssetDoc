// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Derived-state rebuilder for UBlueprint packages: after a snapshot rollback the
// Blueprint is functionally equivalent to its pre-edit state.
//
// Core restores the authored objects of a Blueprint package (the UBlueprint,
// graphs, SCS nodes, component templates, ...) from its snapshot. The generated
// class, skeleton class and CDO are derived and are regenerated here the way the
// editor does after undo / load: reconstruct every node, then compile.
//
// Class-default values (ClassDefaults, variable defaults, native component
// overrides) live on the derived CDO and a compile copies them forward from the
// current CDO, so they are captured with the snapshot and written back after
// the compile.
//
// Some editor edits also update the loaded Blueprints that depend on this one (a widget or member
// rename: FBlueprintEditorUtils::ReplaceVariableReferences marks them modified). The primitives
// refuse while such a dependent has unsaved changes; the dependents are captured with the
// Blueprint's snapshot and, after a failed apply, those the apply changed are restored in memory
// from their own snapshots (as for User Defined Types; nothing is reloaded from disk).

#include "AssetDocumentGraphBlueprintRebuilder.h"

#include "AssetDocumentGraphAdapters.h"
#include "AssetDocumentGraphProvider.h"

#include "Animation/AnimBlueprint.h"
#include "Engine/Blueprint.h"
#include "Engine/DynamicBlueprintBinding.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph
{
namespace
{
using FPropertyTexts = TMap<FName, FString>;

struct FBlueprintDerivedState
{
	EBlueprintStatus Status = BS_Unknown;
	FPropertyTexts ClassDefaults;
	TMap<FName, FPropertyTexts> DefaultSubobjects;
	// The dependent Blueprints' packages, captured with the snapshot (null inside another
	// dependents capture: a dependent's own dependents are not followed).
	TSharedPtr<FAssetDocumentDependentSnapshots> Dependents;
};

// Packages of the loaded Blueprints that depend on Blueprint (FBlueprintEditorUtils::FindDependentBlueprints,
// the set the editor updates), without Blueprint's own.
TArray<UPackage*> DependentPackages(UBlueprint& Blueprint)
{
	TArray<UBlueprint*> Dependents;
	FBlueprintEditorUtils::FindDependentBlueprints(&Blueprint, Dependents);
	TArray<UPackage*> Packages;
	for (UBlueprint* Dependent : Dependents)
	{
		UPackage* Package = Dependent != nullptr ? Dependent->GetOutermost() : nullptr;
		if (Package != nullptr && Package != Blueprint.GetOutermost())
		{
			Packages.AddUnique(Package);
		}
	}
	return Packages;
}

// The dependents the failed apply changed, restored from their snapshots (each one recompiled by
// this rebuilder, as the Blueprint itself).
void RestoreDependents(UBlueprint& Blueprint, FAssetDocumentDependentSnapshots& Snapshots, TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	FAssetDocumentDependentSnapshots::FRestoreResult Restored = Snapshots.Restore(DependentPackages(Blueprint));
	if (!Restored.Restored.IsEmpty())
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic = OutDiagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/PackageSnapshot");
		Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Warning;
		Diagnostic.Code = TEXT("Graph.DependentsRestored");
		Diagnostic.Message = FString::Printf(TEXT("Restored the Blueprints the editor had updated for the change to their state before the apply: %s."),
			*FString::Join(Restored.Restored, TEXT(", ")));
	}
	if (!Restored.NotRestored.IsEmpty())
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic = OutDiagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/PackageSnapshot");
		Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Warning;
		Diagnostic.Code = TEXT("Graph.DependentNotRestored");
		Diagnostic.Message = FString::Printf(TEXT("These Blueprints keep the editor's in-memory update for the change (unsaved; revert them "
			"from disk if that is not wanted): %s."), *FString::Join(Restored.NotRestored, TEXT("; ")));
	}
	OutDiagnostics.Append(MoveTemp(Restored.Diagnostics));
}

// Calls Func(Property, Container, Key) for every captured value of Struct in Container. Instanced
// references point at the CDO's own subobjects; those are captured field by field (the subobject
// pass) instead of by reference, and a struct that holds one (FWidgetChild's weak UWidget pointer:
// UWidget is DefaultToInstanced) is captured member by member.
template <typename FuncType>
void ForEachCapturedValue(const UStruct& Struct, void* Container, const FString& Prefix, int32 Depth, FuncType&& Func)
{
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		FProperty* Property = *It;
		if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_InstancedReference | CPF_Deprecated))
		{
			continue;
		}
		const FString Key = Prefix.IsEmpty() ? Property->GetName() : Prefix + TEXT(".") + Property->GetName();
		if (Property->HasAnyPropertyFlags(CPF_ContainsInstancedReference))
		{
			const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (StructProperty != nullptr && StructProperty->ArrayDim == 1 && Depth < 4)
			{
				ForEachCapturedValue(*StructProperty->Struct, StructProperty->ContainerPtrToValuePtr<void>(Container), Key, Depth + 1, Func);
			}
			continue;
		}
		Func(*Property, Container, FName(*Key));
	}
}

FPropertyTexts ExportProperties(UObject& Object)
{
	FPropertyTexts Out;
	ForEachCapturedValue(*Object.GetClass(), &Object, FString(), 0, [&Object, &Out](const FProperty& Property, void* Container, FName Key)
	{
		FString Text;
		Property.ExportText_InContainer(0, Text, Container, Container, &Object, PPF_None);
		Out.Add(Key, MoveTemp(Text));
	});
	return Out;
}

void ImportChangedProperties(UObject& Object, const FPropertyTexts& Captured, TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	// Values the restored class no longer has (or never had) have nothing to restore.
	ForEachCapturedValue(*Object.GetClass(), &Object, FString(), 0,
		[&Object, &Captured, &OutDiagnostics](const FProperty& Property, void* Container, FName Key)
	{
		const FString* Value = Captured.Find(Key);
		if (Value == nullptr)
		{
			return;
		}
		FString Current;
		Property.ExportText_InContainer(0, Current, Container, Container, &Object, PPF_None);
		if (Current == *Value)
		{
			return;
		}
		if (Property.ImportText_InContainer(**Value, Container, &Object, PPF_None) == nullptr)
		{
			FAssetDocumentRuntimeDiagnostic& Diagnostic = OutDiagnostics.AddDefaulted_GetRef();
			Diagnostic.Path = TEXT("/PackageSnapshot");
			Diagnostic.Code = TEXT("Graph.BlueprintDefaultsRestoreFailed");
			Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Warning;
			Diagnostic.Message = FString::Printf(TEXT("Could not restore default '%s' of '%s'."),
				*Key.ToString(), *Object.GetPathName());
		}
	});
}

class FBlueprintDerivedStateRebuilder final : public IAssetDocumentDerivedStateRebuilder
{
public:
	virtual FName GetId() const override
	{
		return TEXT("AssetDocumentGraph.Blueprint");
	}

	virtual bool Supports(const UObject& PrimaryAsset) const override
	{
		// Exact UBlueprint, Anim Blueprints and every Blueprint class a provider module
		// registered a rebuild adapter for (Widget Blueprints: AssetDocumentWidget). A
		// Widget Blueprint's widget tree and animations, and an Anim Blueprint's anim
		// graphs, state machines and anim layers, are authored objects outered to the
		// asset and are restored from the snapshot; the compile regenerates their
		// class-side copies. The anim compiler re-creates its transient
		// "<Graph>__AnimFunc" stub graphs by name on every compile, so transient objects
		// must stay Derived (an Authored record would pin them while the compiler
		// replaces them).
		if (PrimaryAsset.GetClass() == UBlueprint::StaticClass() || PrimaryAsset.IsA<UAnimBlueprint>())
		{
			return true;
		}
		const UBlueprint* Blueprint = Cast<UBlueprint>(&PrimaryAsset);
		return Blueprint != nullptr && !FindAssetDocumentBlueprintRebuildAdapters(Blueprint).IsEmpty();
	}

	virtual EAssetDocumentSnapshotObjectRole Classify(const UObject& Object) const override
	{
		// The compiler leaves transient intermediates (consolidated ubergraph,
		// ...) in the package and recreates them by name on the next compile;
		// they are compiler output, not authored state.
		for (const UObject* Current = &Object; Current != nullptr && !Current->IsA<UPackage>(); Current = Current->GetOuter())
		{
			if (Current->HasAnyFlags(RF_Transient))
			{
				return EAssetDocumentSnapshotObjectRole::Derived;
			}
		}
		// The generated class's dynamic binding objects (Enhanced Input / input / component-bound event /
		// widget animation delegate bindings, UBlueprintGeneratedClass::DynamicBindingObjects) are compiler
		// output: every compile moves them to the TRASHCLASS_ class with the rest of the old class and
		// BuildDynamicBindingObjects makes new ones from the graph's event nodes. As Authored records they
		// were revived into the class and trashed again by the rebuild compile, and the restore failed
		// (PackageSnapshot.RestoreFailed: "EnhancedInputActionDelegateBinding_N is at TRASHCLASS_...").
		for (const UObject* Current = &Object; Current != nullptr && !Current->IsA<UPackage>(); Current = Current->GetOuter())
		{
			if (Current->IsA<UDynamicBlueprintBinding>())
			{
				return EAssetDocumentSnapshotObjectRole::Derived;
			}
		}
		// Class-specific compiler output, e.g. the widget-tree archetype and *_INST
		// animations under a widget generated class (duplicated from the authored
		// tree/animations by every compile).
		for (const TSharedRef<IAssetDocumentBlueprintRebuildAdapter>& Adapter : GetAssetDocumentBlueprintRebuildAdapters())
		{
			if (Adapter->IsDerivedObject(Object))
			{
				return EAssetDocumentSnapshotObjectRole::Derived;
			}
		}
		return IAssetDocumentDerivedStateRebuilder::Classify(Object);
	}

	virtual TSharedPtr<void> CaptureDerivedState(UObject& PrimaryAsset) const override
	{
		UBlueprint& Blueprint = *CastChecked<UBlueprint>(&PrimaryAsset);
		TSharedRef<FBlueprintDerivedState> State = MakeShared<FBlueprintDerivedState>();
		State->Status = Blueprint.Status;
		if (!FAssetDocumentDependentSnapshots::IsCapturing())
		{
			State->Dependents = FAssetDocumentDependentSnapshots::Capture(DependentPackages(Blueprint));
		}
		UClass* Generated = Blueprint.GeneratedClass;
		UObject* Defaults = Generated != nullptr ? Generated->GetDefaultObject(false) : nullptr;
		if (Defaults != nullptr)
		{
			State->ClassDefaults = ExportProperties(*Defaults);
			TArray<UObject*> Subobjects;
			Defaults->GetDefaultSubobjects(Subobjects);
			for (UObject* Subobject : Subobjects)
			{
				State->DefaultSubobjects.Add(Subobject->GetFName(), ExportProperties(*Subobject));
			}
		}
		return State;
	}

	virtual FAssetDocumentContractValidationResult Rebuild(UObject& PrimaryAsset) override
	{
		return RebuildBlueprint(*CastChecked<UBlueprint>(&PrimaryAsset), nullptr);
	}

	virtual FAssetDocumentContractValidationResult RebuildFromState(UObject& PrimaryAsset, const TSharedPtr<void>& State) override
	{
		return RebuildBlueprint(*CastChecked<UBlueprint>(&PrimaryAsset), static_cast<const FBlueprintDerivedState*>(State.Get()));
	}

private:
	static FAssetDocumentContractValidationResult RebuildBlueprint(UBlueprint& Blueprint, const FBlueprintDerivedState* State)
	{
		FAssetDocumentContractValidationResult Result;
		// FBlueprintEditorUtils is what the editor runs after load / undo.
		FBlueprintEditorUtils::RefreshAllNodes(&Blueprint);
		FCompilerResultsLog Log;
		Log.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(&Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Log);
		if (State != nullptr)
		{
			// Dependents first: recompiling them can reinstance classes, and the class defaults
			// written back below must be the last word.
			if (State->Dependents.IsValid())
			{
				RestoreDependents(Blueprint, *State->Dependents, Result.Diagnostics);
			}
			if (UObject* Defaults = Blueprint.GeneratedClass ? Blueprint.GeneratedClass->GetDefaultObject(false) : nullptr)
			{
				ImportChangedProperties(*Defaults, State->ClassDefaults, Result.Diagnostics);
				for (const TPair<FName, FPropertyTexts>& Entry : State->DefaultSubobjects)
				{
					if (UObject* Subobject = Defaults->GetDefaultSubobjectByName(Entry.Key))
					{
						ImportChangedProperties(*Subobject, Entry.Value, Result.Diagnostics);
					}
				}
			}
		}
		// Compile status must not be worse than before the apply (§3.2).
		const bool bWasError = State != nullptr && State->Status == BS_Error;
		if (Blueprint.Status == BS_Error && !bWasError)
		{
			FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
			Diagnostic.Path = TEXT("/PackageSnapshot");
			Diagnostic.Code = TEXT("Graph.BlueprintRebuildFailed");
			Diagnostic.Message = FString::Printf(TEXT("Restored Blueprint '%s' does not compile (%d errors)."),
				*Blueprint.GetPathName(), Log.NumErrors);
			return Result;
		}
		Result.bValid = true;
		return Result;
	}
};
// The Blueprint a host asset holds (RegisterAssetDocumentHostedBlueprint: a Level Sequence's Director Blueprint) is
// rebuilt as a Blueprint package's own: after Core restores the host package (the Blueprint and its graphs are
// authored objects of it), refresh, compile and the captured class defaults. A Blueprint the failed apply created is
// gone with the restore (the host no longer refers to it) and needs nothing.
class FHostedBlueprintRebuilder final : public IAssetDocumentDerivedStateRebuilder
{
public:
	virtual FName GetId() const override
	{
		return TEXT("AssetDocumentGraph.HostedBlueprint");
	}

	virtual bool Supports(const UObject& PrimaryAsset) const override
	{
		bool bHostClass = false;
		ResolveAssetDocumentHostedBlueprint(const_cast<UObject&>(PrimaryAsset), &bHostClass);
		return bHostClass && !PrimaryAsset.IsA<UBlueprint>();
	}

	virtual EAssetDocumentSnapshotObjectRole Classify(const UObject& Object) const override
	{
		return Inner.Classify(Object);
	}

	virtual TSharedPtr<void> CaptureDerivedState(UObject& PrimaryAsset) const override
	{
		UBlueprint* Blueprint = ResolveAssetDocumentHostedBlueprint(PrimaryAsset);
		return Blueprint != nullptr ? Inner.CaptureDerivedState(*Blueprint) : nullptr;
	}

	virtual FAssetDocumentContractValidationResult Rebuild(UObject& PrimaryAsset) override
	{
		return RebuildFromState(PrimaryAsset, nullptr);
	}

	virtual FAssetDocumentContractValidationResult RebuildFromState(UObject& PrimaryAsset, const TSharedPtr<void>& State) override
	{
		UBlueprint* Blueprint = ResolveAssetDocumentHostedBlueprint(PrimaryAsset);
		if (Blueprint == nullptr)
		{
			FAssetDocumentContractValidationResult Result;
			Result.bValid = true;
			return Result;
		}
		return State.IsValid() ? Inner.RebuildFromState(*Blueprint, State) : Inner.Rebuild(*Blueprint);
	}

private:
	FBlueprintDerivedStateRebuilder Inner;
};
} // namespace

TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeBlueprintDerivedStateRebuilder()
{
	return MakeShared<FBlueprintDerivedStateRebuilder>();
}

TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeHostedBlueprintDerivedStateRebuilder()
{
	return MakeShared<FHostedBlueprintRebuilder>();
}
} // namespace UE::AssetDocument::Graph
