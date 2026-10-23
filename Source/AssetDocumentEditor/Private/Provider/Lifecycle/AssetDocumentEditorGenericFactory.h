// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

class FObjectPropertyBase;
class UClass;
class UFactory;
class UObject;

// Lifecycle identity shared by the Editor catalog, the generic create/scratch
// primitives and created-asset rollback records.
inline constexpr const TCHAR* AssetDocumentEditorGenericFactoryLifecycleId =
	TEXT("factory_generic.v1");

// The requested class has no factory of its own and its ancestor factory
// cannot be told to create it.
inline constexpr const TCHAR* AssetDocumentEditorSubclassUnsupportedCode =
	TEXT("FactoryCreate.SubclassUnsupported");
// The factory needs what its New-asset dialog asks for (a context asset or a
// class) and was not given it.
inline constexpr const TCHAR* AssetDocumentEditorMissingFactoryContextCode =
	TEXT("FactoryCreate.MissingFactoryContext");
// The class is (or its factory makes) a Blueprint asset; Blueprints have their own profile.
inline constexpr const TCHAR* AssetDocumentEditorBlueprintClassUnsupportedCode =
	TEXT("FactoryCreate.BlueprintClassUnsupported");
// The class's own editor factory is switched off in this editor.
inline constexpr const TCHAR* AssetDocumentEditorFactoryDisabledCode =
	TEXT("FactoryCreate.FactoryDisabled");

enum class EAssetDocumentEditorGenericFactoryRejection : uint8
{
	None,
	InvalidClass,
	BlueprintClass,
	AbstractClass,
	NoFactory,
	// An Actor class: its New-asset factory (UBlueprintFactory, ULightWeightInstanceFactory) makes a Blueprint
	// with that parent class; Actors are never standalone assets.
	ActorClass,
	// The class's own factory exists but is switched off in this editor (bCreateNew, but CanCreateNew() is
	// false, e.g. UExternalDataLayerFactory behind an experimental setting), so the New menu does not offer it.
	FactoryDisabled,
};

struct FAssetDocumentEditorGenericFactorySelection
{
	UClass* FactoryClass = nullptr;
	bool bExactSupportedClass = false;
	// FactoryDisabled: the switched-off factory of the class.
	FString DisabledFactoryPath;
	EAssetDocumentEditorGenericFactoryRejection Rejection =
		EAssetDocumentEditorGenericFactoryRejection::InvalidClass;

	bool IsSelected() const { return FactoryClass != nullptr; }
	const TCHAR* GetDiagnosticCode() const;
	FString GetDiagnosticMessage(const UClass* AssetClass) const;
};

// Picks the factory the editor's "Add New" flow would use for AssetClass:
// a concrete UFactory whose CDO can create new assets in the default workflow
// and whose SupportedClass is AssetClass (preferred, regardless of
// ShouldShowInNewMenu) or its nearest ancestor shown in the New menu.
// Ties prefer New-menu factories, then the factory class path. Blueprint asset
// classes (UBlueprint and subclasses) and Actor classes (created as Blueprints)
// are rejected; they own their profiles. A class whose own factory is switched
// off (CanCreateNew() false although bCreateNew) is refused rather than handed
// to an ancestor factory: the editor does not create it either. The exception
// is a Blueprint class an exact profile serves: its own factory creates it even
// where an editor setting switches that factory off.
FAssetDocumentEditorGenericFactorySelection
SelectAssetDocumentEditorGenericFactory(const UClass* AssetClass);

bool IsAssetDocumentEditorGenericFactoryClassAdmitted(const UClass* AssetClass);

// A fresh transient factory instance configured from its class defaults, the
// way AssetTools instantiates factories for the New/Import menus (no dialog
// ConfigureProperties). Defined in AssetDocumentEditorGenericFactoryInstance.cpp.
ASSETDOCUMENTEDITOR_API TStrongObjectPtr<UFactory> NewAssetDocumentEditorGenericFactoryInstance(
	UClass& FactoryClass);

// The factory fields a context asset can fill (the fields its New-asset dialog
// sets): the object properties the factory class declares that editor
// scripting can set, or, when it declares none, all of its object properties
// (their C++ dialog writes them directly). Class properties are never context
// fields. Defined in AssetDocumentEditorGenericFactoryContext.cpp.
TArray<FObjectPropertyBase*> GetAssetDocumentEditorGenericFactoryContextFields(
	const UClass& FactoryClass);

// Configures a transient factory instance with one context asset the way
// editor scripting does (AnimMontage: UAnimMontageFactory::TargetSkeleton):
// the single context field whose class accepts Context receives it. Returns
// nullptr on success, otherwise FactoryCreate.ContextUnsupported (no such
// field) or FactoryCreate.ContextAmbiguous (several). Defined in
// AssetDocumentEditorGenericFactoryContext.cpp.
ASSETDOCUMENTEDITOR_API const TCHAR* ConfigureAssetDocumentEditorGenericFactoryContext(
	UFactory& Factory, UObject& Context);

// Several context assets, for a factory whose New-asset dialog sets more than one
// object (UMirrorDataTableFactory: Skeleton and the row Struct): each one fills
// its own field by the rule above. Two objects that would fill the same field
// give FactoryCreate.ContextAmbiguous.
const TCHAR* ConfigureAssetDocumentEditorGenericFactoryContexts(
	UFactory& Factory, TConstArrayView<UObject*> Contexts);

// For an ancestor factory (SupportedClass is a base of AssetClass), sets the
// one class-typed factory setting that its New-asset class picker would set to
// AssetClass (UCurveFactory::CurveClass, ...). Returns false with
// FactoryCreate.SubclassUnsupported when several settings could select it. A
// factory without such a setting is left as is (it may honor the requested
// class itself). Defined in AssetDocumentEditorGenericFactoryClass.cpp.
ASSETDOCUMENTEDITOR_API bool ConfigureAssetDocumentEditorGenericFactoryAssetClass(
	UFactory& Factory, UClass& AssetClass, FString& OutCode, FString& OutMessage);

// Message for contexts ConfigureAssetDocumentEditorGenericFactoryContexts refused: the factory and what each context
// is (an asset's class, or a class context as that class).
FString DescribeAssetDocumentEditorGenericFactoryContextFailure(const UClass& FactoryClass, TConstArrayView<UObject*> Contexts);

// Diagnostic for a factory that did not create an exact AssetClass (Created
// is what it returned, possibly null): FactoryCreate.BlueprintClassUnsupported
// when it made a Blueprint, FactoryCreate.SubclassUnsupported for
// an ancestor factory, FactoryCreate.MissingFactoryContext naming the unset
// dialog fields of an exact factory, else FactoryCreate.IncompleteCreation.
FString DescribeAssetDocumentEditorGenericFactoryShortfall(
	const UFactory& Factory, const UClass& AssetClass, bool bHadContext, const UObject* Created, FString& OutCode);

// Registers the class-picker factories (AssetDocumentEditorClassPickerFactories.cpp) as dialog factories: their
// New-asset dialog only picks the class to create and keeps it in unreflected factory state. Called by the module.
void RegisterAssetDocumentEditorClassPickerFactories();
void UnregisterAssetDocumentEditorClassPickerFactories();
