// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// The preset parent handed to the MetaSound New-asset factories. Engines whose factories take a document template
// configure them the way the Content Browser's "Create Preset" does: a preset template whose Parent is the parent
// MetaSound, with the parent as the selection. The template accepts a parent of exactly the class the factory
// creates (a Source preset of a Source, a Patch preset of a Patch). Older engines set the factory's
// ReferencedMetaSoundObject, which the generic factory-context rule fills.

#include "MetaSound/AssetDocumentMetaSound.h"

#include "Misc/EngineVersionComparison.h"

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
#include "AssetDocumentEditorFactoryContext.h"
#include "DocumentTemplates/MetasoundFrontendPresetTemplate.h"
#include "Factories/Factory.h"
#include "UObject/UnrealType.h"
#endif

namespace UE::AssetDocument::Graph::Audio::MetaSound
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
namespace FactoryContext
{
const TCHAR* const FactoryClassPaths[] = {
	TEXT("/Script/MetasoundEditor.MetaSoundFactory"),
	TEXT("/Script/MetasoundEditor.MetaSoundSourceFactory"),
};

const TCHAR* ConfigurePresetParent(UFactory& Factory, TConstArrayView<UObject*> Contexts)
{
	if (Contexts.Num() != 1 || Contexts[0]->GetClass() != Factory.GetSupportedClass())
	{
		return TEXT("FactoryCreate.ContextUnsupported");
	}
	UObject& Parent = *Contexts[0];
	const FStructProperty* TemplateProperty = FindFProperty<FStructProperty>(Factory.GetClass(), TEXT("Template"));
	const FArrayProperty* SelectedProperty = FindFProperty<FArrayProperty>(Factory.GetClass(), TEXT("SelectedObjects"));
	if (TemplateProperty == nullptr || TemplateProperty->Struct != TBaseStructure<FInstancedStruct>::Get()
		|| SelectedProperty == nullptr || !SelectedProperty->Inner->IsA<FObjectPropertyBase>())
	{
		return TEXT("FactoryCreate.ContextUnsupported");
	}
	TInstancedStruct<FMetaSoundFrontendDocumentTemplate>& Template =
		*TemplateProperty->ContainerPtrToValuePtr<TInstancedStruct<FMetaSoundFrontendDocumentTemplate>>(&Factory);
	Template = TInstancedStruct<FMetaSoundFrontendDocumentTemplate>::Make<FMetaSoundFrontendPresetTemplate>();
	Template.GetMutable<FMetaSoundFrontendPresetTemplate>().Parent = &Parent;
	*SelectedProperty->ContainerPtrToValuePtr<TArray<TObjectPtr<UObject>>>(&Factory) = {&Parent};
	return nullptr;
}
}
#endif

void RegisterFactoryContext()
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	for (const TCHAR* Path : FactoryContext::FactoryClassPaths)
	{
		RegisterAssetDocumentEditorFactoryContextConfigurator(FTopLevelAssetPath(Path), &FactoryContext::ConfigurePresetParent);
	}
#endif
}

void UnregisterFactoryContext()
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	for (const TCHAR* Path : FactoryContext::FactoryClassPaths)
	{
		UnregisterAssetDocumentEditorFactoryContextConfigurator(FTopLevelAssetPath(Path));
	}
#endif
}
}
