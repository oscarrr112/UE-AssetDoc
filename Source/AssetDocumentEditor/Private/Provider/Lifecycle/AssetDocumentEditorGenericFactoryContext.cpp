// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Raw reflected factory configuration kept in its own file. Editor scripting
// configures a New-asset factory the same way before IAssetTools::CreateAsset,
// e.g. AnimMontageFactory.set_editor_property("target_skeleton", Skeleton):
// the one script/editor-settable object property whose class accepts the
// context object receives it. Factories that expose no such property
// (AnimCompositeFactory::TargetSkeleton, PhysicsAssetFactory::TargetSkeletalMesh,
// ...) are configured by their own C++ ConfigureProperties dialog, which
// writes the reflected UPROPERTY() fields directly; for those the same rule
// runs over every object property the factory class declares. A class context
// (UStateTreeFactory::StateTreeSchemaClass, which its class picker dialog sets)
// goes to the single unset class property the factory subclass declares whose
// meta class accepts it, else to the single one still at the factory's
// preselection, its class default (UControlRigBlueprintFactory::ParentClass,
// ControlRig until its parent class picker chooses ModularRig; a setting
// already made is not overwritten); deprecated classes, and
// abstract ones unless the property allows them (meta AllowAbstract), are
// refused as the picker filters them. Profiles hand a class context they validated
// (state_tree_profile: Body.Schema.Class); a generic document's FactoryContext that
// is a class path (/Script/Module.Class or a Blueprint's ..._C) is resolved to the
// class (USlateWidgetStyleAssetFactory::StyleType, the New Slate Widget Style
// dialog's class picker), and only the meta class rule above admits it. A factory whose single settable context field is typed
// UObject (UMetaSoundBaseFactory::ReferencedMetaSoundObject, on engines where the
// Content Browser's "Create Preset" sets it) receives an object no typed field
// accepts there; only Profiles that validated the object's class pass one
// (metasound: Body.Preset.Parent). A factory with a registered context
// configurator (AssetDocumentEditorFactoryContext.h) is configured by it
// instead of these rules. Behavior tests:
// UEAssetDoc.MontageLifecycle.GenericFactoryContext and
// UEAssetDoc.GenericRuntime.Robustness.FactoryConfiguration.

#include "Provider/Lifecycle/AssetDocumentEditorGenericFactory.h"

#include "AssetDocumentEditorFactoryContext.h"
#include "Factories/Factory.h"
#include "UObject/UnrealType.h"

namespace
{
TMap<FTopLevelAssetPath, FAssetDocumentEditorFactoryContextConfigurator>& FactoryContextConfigurators()
{
	static TMap<FTopLevelAssetPath, FAssetDocumentEditorFactoryContextConfigurator> Registry;
	return Registry;
}

bool IsScriptSettable(const FProperty& Property)
{
	return (Property.HasAnyPropertyFlags(CPF_Edit) && !Property.HasAnyPropertyFlags(CPF_EditConst))
		|| (Property.HasAnyPropertyFlags(CPF_BlueprintVisible) && !Property.HasAnyPropertyFlags(CPF_BlueprintReadOnly));
}

// An object (not class) property the factory subclass itself declares.
bool IsFactoryObjectField(const FObjectPropertyBase& Property)
{
	const UClass* Owner = Property.GetOwnerClass();
	return Owner != nullptr && Owner != UFactory::StaticClass() && Owner->IsChildOf(UFactory::StaticClass())
		&& Property.ArrayDim == 1
		&& !CastField<FClassProperty>(&Property) && !CastField<FSoftClassProperty>(&Property)
		&& Property.PropertyClass != nullptr && Property.PropertyClass != UObject::StaticClass();
}

const TCHAR* ConfigureClassContext(UFactory& Factory, UClass& Context)
{
	if (Context.HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		return TEXT("FactoryCreate.ContextUnsupported");
	}
	// The unset class property first; else the single one still at the factory's preselection, the value its class
	// default object holds (the Control Rig factory's ParentClass starts at ControlRig, which its parent class picker
	// replaces). A setting already made (a value other than the preselection) is never overwritten.
	const UObject* Defaults = Factory.GetClass()->GetDefaultObject();
	for (const bool bPreselected : {false, true})
	{
		FClassProperty* Match = nullptr;
		for (TFieldIterator<FClassProperty> It(Factory.GetClass()); It; ++It)
		{
			FClassProperty* Property = *It;
			const UClass* Owner = Property->GetOwnerClass();
			if (Owner == nullptr || Owner == UFactory::StaticClass() || !Owner->IsChildOf(UFactory::StaticClass())
				|| Property->ArrayDim != 1 || Property->MetaClass == nullptr || !Context.IsChildOf(Property->MetaClass)
				|| (Property->GetObjectPropertyValue_InContainer(&Factory) != nullptr) != bPreselected
				|| (bPreselected && Property->GetObjectPropertyValue_InContainer(&Factory) != Property->GetObjectPropertyValue_InContainer(Defaults)))
			{
				continue;
			}
			if (Match != nullptr)
			{
				return TEXT("FactoryCreate.ContextAmbiguous");
			}
			Match = Property;
		}
		if (Match == nullptr)
		{
			continue;
		}
		// The picker filters abstract classes unless the property allows them (meta AllowAbstract).
		if (Context.HasAnyClassFlags(CLASS_Abstract) && !Match->HasMetaData(TEXT("AllowAbstract")))
		{
			return TEXT("FactoryCreate.ContextUnsupported");
		}
		Match->SetObjectPropertyValue_InContainer(&Factory, &Context);
		return Match->GetObjectPropertyValue_InContainer(&Factory) == &Context
			? nullptr : TEXT("FactoryCreate.ContextUnsupported");
	}
	return TEXT("FactoryCreate.ContextUnsupported");
}
}

TArray<FObjectPropertyBase*> GetAssetDocumentEditorGenericFactoryContextFields(const UClass& FactoryClass)
{
	TArray<FObjectPropertyBase*> All;
	TArray<FObjectPropertyBase*> Settable;
	for (TFieldIterator<FObjectPropertyBase> It(&FactoryClass); It; ++It)
	{
		FObjectPropertyBase* Property = *It;
		if (!IsFactoryObjectField(*Property))
		{
			continue;
		}
		All.Add(Property);
		if (IsScriptSettable(*Property))
		{
			Settable.Add(Property);
		}
	}
	return Settable.IsEmpty() ? All : Settable;
}

namespace
{
// The single context field whose class accepts Context receives it (OutField).
const TCHAR* ConfigureOneFactoryContext(UFactory& Factory, UObject& Context, const FObjectPropertyBase*& OutField)
{
	if (Factory.HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || !IsValid(&Context))
	{
		return TEXT("FactoryCreate.ContextUnsupported");
	}
	if (UClass* ContextClass = Cast<UClass>(&Context))
	{
		return ConfigureClassContext(Factory, *ContextClass);
	}
	FObjectPropertyBase* Match = nullptr;
	for (FObjectPropertyBase* Property : GetAssetDocumentEditorGenericFactoryContextFields(*Factory.GetClass()))
	{
		if (!Context.IsA(Property->PropertyClass))
		{
			continue;
		}
		if (Match != nullptr)
		{
			return TEXT("FactoryCreate.ContextAmbiguous");
		}
		Match = Property;
	}
	if (Match == nullptr)
	{
		// The single settable untyped (UObject) field the factory subclass declares.
		for (TFieldIterator<FObjectPropertyBase> It(Factory.GetClass()); It; ++It)
		{
			FObjectPropertyBase* Property = *It;
			const UClass* Owner = Property->GetOwnerClass();
			if (Owner == nullptr || Owner == UFactory::StaticClass() || !Owner->IsChildOf(UFactory::StaticClass()) || Property->ArrayDim != 1
				|| Property->PropertyClass != UObject::StaticClass() || CastField<FClassProperty>(Property) || CastField<FSoftClassProperty>(Property)
				|| !IsScriptSettable(*Property))
			{
				continue;
			}
			if (Match != nullptr)
			{
				return TEXT("FactoryCreate.ContextAmbiguous");
			}
			Match = Property;
		}
	}
	if (Match == nullptr)
	{
		return TEXT("FactoryCreate.ContextUnsupported");
	}
	Match->SetObjectPropertyValue_InContainer(&Factory, &Context);
	OutField = Match;
	return Match->GetObjectPropertyValue_InContainer(&Factory) == &Context
		? nullptr : TEXT("FactoryCreate.ContextUnsupported");
}
} // namespace

const TCHAR* ConfigureAssetDocumentEditorGenericFactoryContext(UFactory& Factory, UObject& Context)
{
	UObject* const Contexts[] = {&Context};
	return ConfigureAssetDocumentEditorGenericFactoryContexts(Factory, Contexts);
}

void RegisterAssetDocumentEditorFactoryContextConfigurator(const FTopLevelAssetPath& FactoryClassPath,
	FAssetDocumentEditorFactoryContextConfigurator Configure)
{
	check(IsInGameThread());
	if (FactoryClassPath.IsValid() && Configure)
	{
		FactoryContextConfigurators().Add(FactoryClassPath, MoveTemp(Configure));
	}
}

void UnregisterAssetDocumentEditorFactoryContextConfigurator(const FTopLevelAssetPath& FactoryClassPath)
{
	check(IsInGameThread());
	FactoryContextConfigurators().Remove(FactoryClassPath);
}

const FAssetDocumentEditorFactoryContextConfigurator* FindAssetDocumentEditorFactoryContextConfigurator(const UClass& FactoryClass)
{
	check(IsInGameThread());
	return FactoryContextConfigurators().Find(FTopLevelAssetPath(&FactoryClass));
}

const TCHAR* ConfigureAssetDocumentEditorGenericFactoryContexts(UFactory& Factory, TConstArrayView<UObject*> Contexts)
{
	// A factory whose dialog takes the context in its own form (AssetDocumentEditorFactoryContext.h).
	if (const FAssetDocumentEditorFactoryContextConfigurator* Configure = FindAssetDocumentEditorFactoryContextConfigurator(*Factory.GetClass()))
	{
		if (Factory.HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || Contexts.Contains(nullptr))
		{
			return TEXT("FactoryCreate.ContextUnsupported");
		}
		return (*Configure)(Factory, Contexts);
	}
	// Each context object fills its own field, as the dialog sets each once; two objects for the
	// same field are ambiguous.
	TSet<const FObjectPropertyBase*> Filled;
	for (UObject* Context : Contexts)
	{
		if (Context == nullptr)
		{
			return TEXT("FactoryCreate.ContextUnsupported");
		}
		const FObjectPropertyBase* Field = nullptr;
		if (const TCHAR* Code = ConfigureOneFactoryContext(Factory, *Context, Field))
		{
			return Code;
		}
		bool bAlreadyFilled = false;
		Filled.Add(Field, &bAlreadyFilled);
		if (bAlreadyFilled)
		{
			return TEXT("FactoryCreate.ContextAmbiguous");
		}
	}
	return nullptr;
}

FString DescribeAssetDocumentEditorGenericFactoryContextFailure(const UClass& FactoryClass, TConstArrayView<UObject*> Contexts)
{
	TArray<FString> Parts;
	bool bHasClassContext = false;
	for (const UObject* Context : Contexts)
	{
		if (const UClass* ContextClass = Cast<UClass>(Context))
		{
			bHasClassContext = true;
			Parts.Add(FString::Printf(TEXT("class %s"), *ContextClass->GetPathName()));
		}
		else
		{
			Parts.Add(GetPathNameSafe(Context != nullptr ? Context->GetClass() : nullptr));
		}
	}
	return FString::Printf(TEXT("%s has no single settable property for each of %s.%s"),
		*FactoryClass.GetPathName(), *FString::Join(Parts, TEXT(", ")),
		bHasClassContext
			? TEXT(" A class context goes to the factory's class setting whose picker offers it: a concrete, non-deprecated subclass of that setting's base class.")
			: TEXT(""));
}
