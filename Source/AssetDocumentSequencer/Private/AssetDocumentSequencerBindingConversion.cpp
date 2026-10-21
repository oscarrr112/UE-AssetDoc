// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Raw file (design §4.2 / §4.4, B1 rule): the binding context menu's "Convert To Possessable / Spawnable / <custom
// binding>" without an ISequencer. FSequencerUtilities::ConvertToPossessable / ConvertToCustomBinding
// (Editor/Sequencer/Private/SequencerUtilities.cpp) need a playback world: they spawn or copy the live object and call
// CreateOrReplaceBinding with it. The data steps that remain without a world are reproduced here:
//   * the binding keeps its GUID (CreateOrReplaceBinding replaces the binding reference at the same GUID), so its
//     tracks, children, folder and every reference to it stay;
//   * the new custom binding comes from the binding type's CDO (CreateNewCustomBinding from the class: a spawnable's
//     template is a new actor of that class, which the document's Template then edits; the editor copies the live actor
//     instead, and the document supplies class + template);
//   * PossessedObjectClass follows the custom binding's bound class (TryCreateCustomSpawnableBinding does the same).
// The Spawn / binding-lifetime tracks the editor adds or removes with the conversion are ordinary tracks of the
// document (a spawnable without a listed Spawn track gets the editor's default one from the profile), so they are
// written by the track ops, not here. Behaviour test: UEAssetDoc.Sequencer.BindingConversion.

#include "AssetDocumentSequencerEdit.h"

#include "Bindings/MovieSceneCustomBinding.h"
#include "Bindings/MovieSceneSpawnableActorBinding.h"
#include "GameFramework/Actor.h"
#include "MovieScene.h"
#include "MovieSceneBindingReferences.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSequence.h"
#include "UniversalObjectLocator.h"

namespace UE::AssetDocument::Graph::Sequencer::Edit
{
namespace
{
UMovieSceneCustomBinding* SingleCustomBinding(const FMovieSceneBindingReferences& References, const FGuid& Guid)
{
	const TArrayView<const FMovieSceneBindingReference> Refs = References.GetReferences(Guid);
	return Refs.Num() == 1 ? Refs[0].CustomBinding.Get() : nullptr;
}

void SetBoundClass(FEdit& Edit, const FGuid& Guid, UClass* Class)
{
#if WITH_EDITORONLY_DATA
	if (FMovieScenePossessable* Possessable = Edit.MovieScene.FindPossessable(Guid))
	{
		Possessable->SetPossessedObjectClass(Class);
	}
#endif
}
} // namespace

void ConvertToPossessable(FEdit& Edit, const FGuid& Guid)
{
	FMovieSceneBindingReferences* References = Edit.Sequence.GetBindingReferences();
	if (References == nullptr)
	{
		return;
	}
	bool bCustom = false;
	for (const FMovieSceneBindingReference& Reference : References->GetReferences(Guid))
	{
		bCustom |= Reference.CustomBinding != nullptr;
	}
	if (!bCustom)
	{
		return;
	}
	Edit.Sequence.Modify();
	Edit.MovieScene.Modify();
	// CreateOrReplaceBinding with bAllowCustomBinding = false: the binding's references become locators only.
	References->RemoveBinding(Guid);
	SetBoundClass(Edit, Guid, nullptr);
	Edit.bChanged = true;
	Edit.Invalidate();
}

UMovieSceneCustomBinding* ConvertToCustomBinding(FEdit& Edit, const FGuid& Guid, UClass& BindingClass, UClass* SpawnClass, FReader& Op,
	const FString& Path)
{
	FMovieSceneBindingReferences* References = Edit.Sequence.GetBindingReferences();
	if (References == nullptr)
	{
		Op.Fail(TEXT("Sequencer.UnsupportedBindingShape"), TEXT("This sequence has no binding references (custom bindings need them)."), Path);
		return nullptr;
	}
	if (UMovieSceneCustomBinding* Existing = SingleCustomBinding(*References, Guid); Existing != nullptr && Existing->GetClass() == &BindingClass)
	{
		UMovieSceneSpawnableActorBinding* Spawnable = Cast<UMovieSceneSpawnableActorBinding>(Existing);
		const UObject* Template = Spawnable != nullptr ? Spawnable->GetObjectTemplate() : nullptr;
		if (SpawnClass == nullptr || (Template != nullptr && Template->GetClass() == SpawnClass))
		{
			return Existing;
		}
	}
	if (!BindingClass.IsChildOf(UMovieSceneCustomBinding::StaticClass()) || BindingClass.HasAnyClassFlags(CLASS_Abstract))
	{
		Op.Fail(TEXT("Sequencer.InvalidBindingClass"), FString::Printf(TEXT("%s is not a concrete custom binding class (list_types kind "
			"sequencer_binding lists them)."), *BindingClass.GetPathName()), Path);
		return nullptr;
	}
	UMovieSceneCustomBinding* Created = nullptr;
	if (SpawnClass != nullptr)
	{
		UMovieSceneCustomBinding* Defaults = BindingClass.GetDefaultObject<UMovieSceneCustomBinding>();
		if (!SpawnClass->IsChildOf(AActor::StaticClass()) || SpawnClass->HasAnyClassFlags(CLASS_Abstract) || !Defaults->SupportsBindingCreationFromObject(SpawnClass))
		{
			Op.Fail(TEXT("Sequencer.InvalidSpawnableClass"), FString::Printf(TEXT("A spawnable spawns a concrete actor class; %s is not one."),
				*SpawnClass->GetPathName()), Path);
			return nullptr;
		}
		Edit.MovieScene.Modify();
		Created = Defaults->CreateNewCustomBinding(SpawnClass, Edit.MovieScene);
	}
	else
	{
		Edit.MovieScene.Modify();
		const FName Name = MakeUniqueObjectName(&Edit.MovieScene, &BindingClass, *(BindingClass.GetName() + TEXT("_CustomBinding")));
		Created = NewObject<UMovieSceneCustomBinding>(&Edit.MovieScene, &BindingClass, Name, RF_Transactional);
	}
	if (Created == nullptr)
	{
		Op.Fail(TEXT("Sequencer.InvalidBindingClass"), FString::Printf(TEXT("%s did not create a binding."), *BindingClass.GetPathName()), Path);
		return nullptr;
	}
	Edit.Sequence.Modify();
	References->RemoveBinding(Guid);
	References->AddBinding(Guid, FUniversalObjectLocator(), ELocatorResolveFlags::None, Created);
	SetBoundClass(Edit, Guid, Created->GetBoundObjectClass());
	Edit.bChanged = true;
	Edit.Invalidate();
	return Created;
}
} // namespace UE::AssetDocument::Graph::Sequencer::Edit
