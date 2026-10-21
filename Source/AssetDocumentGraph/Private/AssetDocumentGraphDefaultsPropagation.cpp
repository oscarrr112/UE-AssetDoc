// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentGraphDefaultsPropagation.h"

#include "Components/ActorComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph
{
namespace DefaultsPropagationDetail
{
using FValueSnapshot = FBlueprintDefaultsSnapshot::FValueSnapshot;
using FSourceValues = TMap<FString, FValueSnapshot>;

// Component references are the construction's business (each instance makes its own components).
bool PropagationReferencesComponents(const FProperty& Property)
{
	const FProperty* Inner = &Property;
	if (const FArrayProperty* Array = CastField<FArrayProperty>(Inner))
	{
		Inner = Array->Inner;
	}
	else if (const FSetProperty* Set = CastField<FSetProperty>(Inner))
	{
		Inner = Set->ElementProp;
	}
	else if (const FMapProperty* Map = CastField<FMapProperty>(Inner))
	{
		Inner = Map->ValueProp;
	}
	const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Inner);
	return Object != nullptr && Object->PropertyClass != nullptr && Object->PropertyClass->IsChildOf(UActorComponent::StaticClass());
}

// The values a class default edit can change and an instance inherits: serialized, not deprecated.
bool IsPropagatedProperty(const FProperty& Property)
{
	return !Property.HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) && !PropagationReferencesComponents(Property);
}

FString PropagationTypeKey(const FProperty& Property)
{
	return Property.GetClass()->GetName() + TEXT(" ") + Property.GetCPPType();
}

FString PropagationKey(const FProperty& Property, int32 Index)
{
	return Property.ArrayDim > 1 ? FString::Printf(TEXT("%s[%d]"), *Property.GetName(), Index) : Property.GetName();
}

// The text FPropertyNode::PropagatePropertyChange compares (PPF_ForDiff, no delta).
FString ExportPropagationText(const FProperty& Property, const void* ValuePtr, UObject* Owner)
{
	FString Text;
	Property.ExportText_Direct(Text, ValuePtr, nullptr, Owner, PPF_ForDiff);
	return Text;
}

// The value a property has before anything sets it (a variable the apply added, a retyped one).
FString ExportInitialText(const FProperty& Property, UObject* Owner)
{
	void* Buffer = FMemory::Malloc(FMath::Max(Property.GetElementSize(), 1), Property.GetMinAlignment());
	Property.InitializeValue(Buffer);
	FString Text = ExportPropagationText(Property, Buffer, Owner);
	Property.DestroyValue(Buffer);
	FMemory::Free(Buffer);
	return Text;
}

template <typename FVisit>
void ForEachPropagatedValue(UObject& Object, FVisit&& Visit)
{
	for (TFieldIterator<FProperty> It(Object.GetClass()); It; ++It)
	{
		FProperty& Property = **It;
		if (!IsPropagatedProperty(Property))
		{
			continue;
		}
		for (int32 Index = 0; Index < Property.ArrayDim; ++Index)
		{
			Visit(Property, Index, Property.ContainerPtrToValuePtr<void>(&Object, Index));
		}
	}
}

FSourceValues SnapshotValues(UObject& Object)
{
	FSourceValues Values;
	ForEachPropagatedValue(Object, [&Values, &Object](const FProperty& Property, int32 Index, void* ValuePtr)
	{
		Values.Add(PropagationKey(Property, Index), FValueSnapshot{PropagationTypeKey(Property), ExportPropagationText(Property, ValuePtr, &Object)});
	});
	return Values;
}

// The objects whose values a Blueprint's class default edit reaches in the editor: its class
// default object (and its default subobjects: native components) and its component templates
// (own Simple Construction Script nodes, overrides of inherited components).
template <typename FVisit>
void ForEachDefaultsSource(UBlueprint& Blueprint, FVisit&& Visit)
{
	UClass* Generated = Blueprint.GeneratedClass;
	if (UObject* Defaults = Generated != nullptr ? Generated->GetDefaultObject(false) : nullptr)
	{
		Visit(FString(TEXT("cdo")), *Defaults);
		TArray<UObject*> Subobjects;
		Defaults->GetDefaultSubobjects(Subobjects);
		for (UObject* Subobject : Subobjects)
		{
			if (Subobject != nullptr)
			{
				Visit(TEXT("sub:") + Subobject->GetName(), *Subobject);
			}
		}
	}
	if (const USimpleConstructionScript* Script = Blueprint.SimpleConstructionScript)
	{
		for (USCS_Node* Node : Script->GetAllNodes())
		{
			if (Node != nullptr && Node->ComponentTemplate != nullptr)
			{
				Visit(TEXT("scs:") + Node->VariableGuid.ToString(EGuidFormats::Digits), *Node->ComponentTemplate);
			}
		}
	}
	if (UInheritableComponentHandler* Handler = Blueprint.GetInheritableComponentHandler(false))
	{
		for (auto Record = Handler->CreateRecordIterator(); Record; ++Record)
		{
			if (Record->ComponentTemplate != nullptr)
			{
				Visit(TEXT("ich:") + Record->ComponentKey.GetAssociatedGuid().ToString(EGuidFormats::Digits), *Record->ComponentTemplate);
			}
		}
	}
}

// Staged verification copies and scratch Blueprints live in /Temp/ packages; dead templates and
// reinstanced leftovers in the transient package.
bool IsLiveDependent(const UObject& Object, const UObject& Source)
{
	const UPackage* Package = Object.GetOutermost();
	if (!IsValid(&Object) || Package == nullptr || Package == GetTransientPackage()
		|| Object.GetClass()->HasAnyClassFlags(CLASS_NewerVersionExists)
		|| (Object.HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) && Package->GetName().StartsWith(TEXT("/Temp/"))))
	{
		return false;
	}
	return Object.IsBasedOnArchetype(&Source);
}

TArray<UObject*> FindDependents(UObject& Source)
{
	TArray<UObject*> Dependents;
	ForEachObjectOfClass(Source.GetClass(), [&Dependents, &Source](UObject* Object)
	{
		if (Object != &Source && IsLiveDependent(*Object, Source))
		{
			Dependents.Add(Object);
		}
	}, true, RF_NoFlags, EInternalObjectFlags::Garbage);
	return Dependents;
}

bool HoldsInstancedObjects(const FProperty& Property)
{
	return Property.HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference | CPF_PersistentInstance);
}

struct FChangedValue
{
	FProperty* Property = nullptr;
	int32 Index = 0;
	FString OldText;
	FString NewImportText;
	const void* NewValue = nullptr;
};

bool IsInGameWorld(const UObject& Object)
{
	const UWorld* World = Object.GetWorld();
	return World != nullptr && World->IsGameWorld();
}
} // namespace DefaultsPropagationDetail

TOptional<FBlueprintDefaultsSnapshot> FBlueprintDefaultsSnapshot::Capture(const FString& BlueprintObjectPath)
{
	using namespace DefaultsPropagationDetail;
	UBlueprint* Blueprint = Cast<UBlueprint>(FSoftObjectPath(BlueprintObjectPath).ResolveObject());
	if (Blueprint == nullptr || !IsValid(Blueprint))
	{
		return {};
	}
	FBlueprintDefaultsSnapshot Snapshot;
	Snapshot.BlueprintPath = BlueprintObjectPath;
	ForEachDefaultsSource(*Blueprint, [&Snapshot](const FString& Key, UObject& Source)
	{
		Snapshot.Sources.Add(Key, SnapshotValues(Source));
	});
	return Snapshot;
}

void FBlueprintDefaultsSnapshot::Propagate(TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics) const
{
	using namespace DefaultsPropagationDetail;
	UBlueprint* Blueprint = Cast<UBlueprint>(FSoftObjectPath(BlueprintPath).ResolveObject());
	if (Blueprint == nullptr || !IsValid(Blueprint))
	{
		return;
	}
	int32 ValueCount = 0;
	TSet<UObject*> Updated;
	TMap<UPackage*, bool> WasDirty;
	ForEachDefaultsSource(*Blueprint, [&](const FString& Key, UObject& Source)
	{
		// A template the apply created (a new inherited-component override) replaced its archetype
		// as the object its instances are based on: the archetype's values are what they held.
		const FSourceValues* Before = Sources.Find(Key);
		FSourceValues ArchetypeValues;
		if (Before == nullptr)
		{
			UObject* Archetype = Source.GetArchetype();
			if (Archetype == nullptr || Archetype == &Source || Archetype->HasAnyFlags(RF_ClassDefaultObject))
			{
				return;
			}
			ArchetypeValues = SnapshotValues(*Archetype);
			Before = &ArchetypeValues;
		}
		TArray<FChangedValue> Changes;
		ForEachPropagatedValue(Source, [&](const FProperty& Property, int32 Index, void* ValuePtr)
		{
			const FValueSnapshot* Old = Before->Find(PropagationKey(Property, Index));
			FString OldText = Old != nullptr && Old->TypeKey == PropagationTypeKey(Property) ? Old->Text : ExportInitialText(Property, &Source);
			if (OldText == ExportPropagationText(Property, ValuePtr, &Source))
			{
				return;
			}
			FChangedValue& Change = Changes.AddDefaulted_GetRef();
			Change.Property = const_cast<FProperty*>(&Property);
			Change.Index = Index;
			Change.OldText = MoveTemp(OldText);
			Change.NewValue = ValuePtr;
			if (HoldsInstancedObjects(Property))
			{
				Property.ExportText_Direct(Change.NewImportText, ValuePtr, nullptr, &Source, PPF_None);
			}
		});
		if (Changes.IsEmpty())
		{
			return;
		}
		for (UObject* Dependent : FindDependents(Source))
		{
			TArray<FProperty*> Edited;
			for (const FChangedValue& Change : Changes)
			{
				void* Target = Change.Property->ContainerPtrToValuePtr<void>(Dependent, Change.Index);
				if (ExportPropagationText(*Change.Property, Target, Dependent) != Change.OldText)
				{
					continue;  // the dependent overrides this value: it keeps it
				}
				UPackage* Package = Dependent->GetOutermost();
				if (!WasDirty.Contains(Package))
				{
					WasDirty.Add(Package, Package->IsDirty());
				}
				if (HoldsInstancedObjects(*Change.Property))
				{
					Change.Property->ImportText_Direct(*Change.NewImportText, Target, Dependent, PPF_InstanceSubobjects);
				}
				else
				{
					Change.Property->CopySingleValue(Target, Change.NewValue);
				}
				Edited.AddUnique(Change.Property);
				++ValueCount;
			}
			if (Edited.IsEmpty())
			{
				continue;
			}
			Updated.Add(Dependent);
			if (!IsInGameWorld(*Dependent))
			{
				for (FProperty* Property : Edited)
				{
					FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
					Dependent->PostEditChangeProperty(Event);
				}
			}
		}
	});
	for (const TPair<UPackage*, bool>& Entry : WasDirty)
	{
		if (IsValid(Entry.Key) && !Entry.Value && Entry.Key->IsDirty())
		{
			Entry.Key->SetDirtyFlag(false);
		}
	}
	if (ValueCount == 0)
	{
		return;
	}
	TArray<FString> Names;
	for (const UObject* Object : Updated)
	{
		if (Names.Num() == 5)
		{
			Names.Add(TEXT("..."));
			break;
		}
		Names.Add(Object->GetPathName());
	}
	FAssetDocumentRuntimeDiagnostic& Diagnostic = OutDiagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/Body");
	Diagnostic.Code = TEXT("Graph.DefaultsPropagated");
	Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Info;
	Diagnostic.Message = FString::Printf(TEXT("%d inherited value(s) updated on %d object(s) that still held the previous value, "
		"as the Blueprint editor does when a class default changes (child Blueprint defaults and component overrides, "
		"placed instances; overridden values are kept): %s"), ValueCount, Updated.Num(), *FString::Join(Names, TEXT(", ")));
}
}
