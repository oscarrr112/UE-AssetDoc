// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Raw file (design §4.2 / §4.4, B1 rule): the root of a Template / Camera Animation Sequence without an ISequencer.
//
// * template.root {class}: the Details panel's "Bound Actor Class" picker, FTemplateSequenceEditorUtil::ChangeActorBinding
//   (TemplateSequenceEditor/Private/Misc/TemplateSequenceEditorUtil.cpp). The editor makes the new spawnable through
//   ISequencer::MakeNewSpawnable, which needs an open Sequencer (it spawns the actor in the preview world). The data
//   steps without a world are the ones FTemplateSequenceFactoryUtil::InitializeSpawnable (the factory) makes:
//   ILevelSequenceModule::GenerateObjectSpawners -> CreateNewSpawnableType(class) -> MakeUniqueSpawnableName ->
//   UMovieScene::AddSpawnable. Then, as ChangeActorBinding: PreEditChange(BoundActorClass), BoundActorClass = class,
//   MoveBindingContents(old root, new root) (tracks and component bindings move; children are re-parented and listed as
//   the new spawnable's child possessables), RemoveSpawnable(old root), PostEditChangeProperty. The root GUID changes,
//   as in the editor. Differences from the editor, all for the document to state: no preview spawn, no
//   SetupDefaultsForSpawnable / camera viewport lock (preview conveniences), and the moved Spawn track's object id and
//   the old root's folder entry follow the new root (the editor leaves them on the removed GUID, where nothing reads
//   them). A sequence without a root gets one without a Spawn track (the picker's path); the profile's default Spawn
//   track is an ordinary track of the document.
// * binding.set of the root {Root: {Class, Template?, Ownership?, ContinuouslyRespawn?, NetAddressableName?, LevelName?,
//   SpawnTransform?}, Tags?}: the spawnable's template (Details of the spawned actor saved as the default state) and its
//   Details fields, complete against a new spawnable of the template; Class must be the bound actor class.
// * binding.set of a component binding {Class?, Component, Tags?}: PossessedObjectClass and the component name the
//   binding locates (UTemplateSequence::BoundActorComponents, written by BindPossessableObject when the editor adds a
//   component; "" removes the entry as UnbindPossessableObjects does).
// Behaviour tests: UEAssetDoc.Sequencer.Template.*.

#include "AssetDocumentSequencerEdit.h"

#include "Camera/CameraActor.h"
#include "CameraAnimationSequence.h"
#include "Components/ActorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "ILevelSequenceModule.h"
#include "IMovieSceneObjectSpawner.h"
#include "Modules/ModuleManager.h"
#include "MovieScene.h"
#include "MovieSceneCommonHelpers.h"
#include "MovieSceneFolder.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSpawnable.h"
#include "TemplateSequence.h"
#include "Tracks/MovieSceneSpawnTrack.h"

namespace UE::AssetDocument::Graph::Sequencer
{
FMovieSceneSpawnable RootSpawnableDefaults(const UObject* Template)
{
	FMovieSceneSpawnable Defaults;
	const AActor* Actor = Cast<AActor>(Template);
	Defaults.bNetAddressableName = Actor != nullptr && Actor->FindComponentByClass<UStaticMeshComponent>() != nullptr;
	return Defaults;
}

UClass* FindTemplateComponentClass(const AActor& Template, FName Name)
{
	TArray<UActorComponent*> Components;
	Template.GetComponents(Components);
	for (const UActorComponent* Component : Components)
	{
		if (Component != nullptr && Component->GetFName() == Name)
		{
			return Component->GetClass();
		}
	}
	// Blueprint components are made by the construction script when the actor spawns (named by their variable).
	for (const UClass* Class = Template.GetClass(); Class != nullptr; Class = Class->GetSuperClass())
	{
		const UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(Class);
		const USimpleConstructionScript* Script = Generated != nullptr ? Generated->SimpleConstructionScript.Get() : nullptr;
		if (Script == nullptr)
		{
			continue;
		}
		for (const USCS_Node* Node : Script->GetAllNodes())
		{
			if (Node != nullptr && Node->GetVariableName() == Name && Node->ComponentTemplate != nullptr)
			{
				return Node->ComponentTemplate->GetClass();
			}
		}
	}
	return nullptr;
}

bool CheckRootClass(const UMovieSceneSequence& Sequence, const UClass* Class, const FString& Path, FString& OutCode, FString& OutMessage)
{
	if (Class == nullptr || !Class->IsChildOf(AActor::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		OutCode = TEXT("Sequencer.RootClassInvalid");
		OutMessage = FString::Printf(TEXT("'%s' is not an actor class a template can spawn (it must load, derive from Actor and not be abstract or "
			"deprecated)."), *Path);
		return false;
	}
	if (Sequence.IsA<UCameraAnimationSequence>() && !Class->IsChildOf(ACameraActor::StaticClass()))
	{
		OutCode = TEXT("Sequencer.CameraClassRequired");
		OutMessage = FString::Printf(TEXT("A Camera Animation Sequence animates a camera: '%s' is not a Camera Actor class (the editor offers "
			"/Script/Engine.CameraActor and /Script/CinematicCamera.CineCameraActor and their subclasses)."), *Path);
		return false;
	}
	return true;
}
} // namespace UE::AssetDocument::Graph::Sequencer

namespace UE::AssetDocument::Graph::Sequencer::Edit
{
namespace
{
// The old root's folder entry follows the new root.
void MoveFolderEntry(UMovieScene& MovieScene, const FGuid& From, const FGuid& To)
{
#if WITH_EDITORONLY_DATA
	TArray<UMovieSceneFolder*> Folders;
	GetMovieSceneFoldersRecursive(MovieScene.GetRootFolders(), Folders);
	for (UMovieSceneFolder* Folder : Folders)
	{
		if (Folder != nullptr && Folder->GetChildObjectBindings().Contains(From))
		{
			Folder->Modify();
			Folder->RemoveChildObjectBinding(From);
			Folder->AddChildObjectBinding(To);
		}
	}
#endif
}

bool WriteRoot(FEdit& Edit, FReader& Entry, const FValue& Root, const FGuid& Guid)
{
	FReader Reader = Entry.Child(TEXT("Root"), Root);
	if (!CheckFields(Reader, Root, {TEXT("Class"), TEXT("Template"), TEXT("Ownership"), TEXT("ContinuouslyRespawn"), TEXT("NetAddressableName"),
		TEXT("LevelName"), TEXT("SpawnTransform")}, TEXT("Root")))
	{
		return false;
	}
	FMovieSceneSpawnable& Spawnable = *Edit.MovieScene.FindSpawnable(Guid);
	AActor* Actor = Cast<AActor>(Spawnable.GetObjectTemplate());
	const FValue* ClassPath = FindField(Root, TEXT("Class"));
	if (Actor == nullptr || ClassPath == nullptr || ClassPath->Kind != EAssetDocumentRuntimeValueKind::String
		|| ClassPath->StringValue != Actor->GetClass()->GetPathName())
	{
		return Reader.Fail(TEXT("Sequencer.InvalidBody"), FString::Printf(TEXT("The root spawns %s; its Class changes with template.root "
			"(a new root, as the Bound Actor Class picker makes it)."), Actor != nullptr ? *Actor->GetClass()->GetPathName() : TEXT("nothing")),
			TEXT("Class"));
	}
	const FValue* Saved = FindField(Root, TEXT("Template"));
	FReader TemplateReader = Reader.Child(TEXT("Template"), Saved != nullptr ? *Saved : Root);
	if (!WriteTemplate(Edit, TemplateReader, *Actor, Saved))
	{
		return false;
	}
	// The spawnable's own fields, complete against a new spawnable of this template.
	FObjectBuilder Fields;
	TSet<FName> Allowed;
	for (const TPair<const TCHAR*, const TCHAR*>& Known : RootSpawnableFields())
	{
		Allowed.Add(Known.Value);
		if (const FValue* Value = FindField(Root, Known.Key))
		{
			Fields.Add(Known.Value, *Value);
		}
	}
	const FMovieSceneSpawnable Defaults = RootSpawnableDefaults(Actor);
	const FValue FieldValue = Fields.Build();
	FDecodeContext Context{Edit.Model(), TimeOf(Edit.MovieScene), &Edit.MovieScene, Reader, FString()};
	return DecodeSparseObject(*FMovieSceneSpawnable::StaticStruct(), FieldValue, &Spawnable, &Defaults,
		[&Allowed](const FProperty& Property) { return Allowed.Contains(Property.GetFName()); }, Context);
}
} // namespace

bool OpTemplateRoot(FEdit& Edit, FReader& Op)
{
	UTemplateSequence* Template = Cast<UTemplateSequence>(&Edit.Sequence);
	if (Template == nullptr)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("template.root edits Template and Camera Animation Sequences."), TEXT("op"));
	}
	FString ClassPath;
	if (!Op.String(TEXT("class"), ClassPath))
	{
		return false;
	}
	UClass* Class = LoadObject<UClass>(nullptr, *ClassPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	FString Code;
	FString Message;
	if (!CheckRootClass(*Template, Class, ClassPath, Code, Message))
	{
		return Op.Fail(Code, Message, TEXT("class"));
	}
	UMovieScene& MovieScene = Edit.MovieScene;
	if (MovieScene.GetSpawnableCount() > 1)
	{
		return Op.Fail(TEXT("Sequencer.UnsupportedBindingShape"), FString::Printf(TEXT("The template has %d spawnables; only one (its root) is "
			"documented, so its class is not changed here."), MovieScene.GetSpawnableCount()), TEXT("class"));
	}
	const FGuid Previous = MovieScene.GetSpawnableCount() == 1 ? MovieScene.GetSpawnable(0).GetGuid() : FGuid();
	if (Previous.IsValid())
	{
		const UObject* Current = MovieScene.GetSpawnable(0).GetObjectTemplate();
		if (Current != nullptr && Current->GetClass() == Class && Template->BoundActorClass.Get() == Class)
		{
			return true;
		}
	}

	// The new root spawnable (FTemplateSequenceFactoryUtil::InitializeSpawnable's spawner steps).
	TArray<TSharedRef<IMovieSceneObjectSpawner>> Spawners;
	FModuleManager::LoadModuleChecked<ILevelSequenceModule>(TEXT("LevelSequence")).GenerateObjectSpawners(Spawners);
	FNewSpawnable NewSpawnable;
	bool bCreated = false;
	for (const TSharedRef<IMovieSceneObjectSpawner>& Spawner : Spawners)
	{
		TValueOrError<FNewSpawnable, FText> Result = Spawner->CreateNewSpawnableType(*Class, MovieScene, nullptr);
		if (Result.IsValid())
		{
			NewSpawnable = Result.StealValue();
			bCreated = NewSpawnable.ObjectTemplate != nullptr;
			break;
		}
	}
	if (!bCreated)
	{
		return Op.Fail(TEXT("Sequencer.RootClassInvalid"), FString::Printf(TEXT("No object spawner makes a spawnable of %s."), *ClassPath),
			TEXT("class"));
	}
	Template->Modify();
	MovieScene.Modify();
	const FString Name = MovieSceneHelpers::MakeUniqueSpawnableName(&MovieScene, NewSpawnable.Name);
	const FGuid NewGuid = MovieScene.AddSpawnable(Name, *NewSpawnable.ObjectTemplate);

	FProperty* BoundActorClass = FindFProperty<FProperty>(UTemplateSequence::StaticClass(), GET_MEMBER_NAME_CHECKED(UTemplateSequence, BoundActorClass));
	Template->PreEditChange(BoundActorClass);
	Template->BoundActorClass = Class;
	if (Previous.IsValid())
	{
		MovieScene.MoveBindingContents(Previous, NewGuid);
		if (const FMovieSceneBinding* Binding = MovieScene.FindBinding(NewGuid))
		{
			for (UMovieSceneTrack* Track : Binding->GetTracks())
			{
				if (UMovieSceneSpawnTrack* Spawn = Cast<UMovieSceneSpawnTrack>(Track))
				{
					Spawn->Modify();
					Spawn->SetObjectId(NewGuid);
				}
			}
		}
		MoveFolderEntry(MovieScene, Previous, NewGuid);
		MovieScene.SetObjectDisplayName(Previous, FText::GetEmpty());
		MovieScene.RemoveSpawnable(Previous);
	}
	FPropertyChangedEvent Event(BoundActorClass, EPropertyChangeType::ValueSet);
	Template->PostEditChangeProperty(Event);
	Edit.bChanged = true;
	Edit.Invalidate();
	return true;
}

bool SetTemplateBinding(FEdit& Edit, FReader& Op, FReader& Entry, const FValue& EntryValue, const FGuid& Guid)
{
	UTemplateSequence& Template = *CastChecked<UTemplateSequence>(&Edit.Sequence);
	const FValue NoTags = MakeArray(TArray<FValue>());
	const FValue* Tags = FindField(EntryValue, TEXT("Tags"));
	if (Edit.Model().Root.IsValid() && Guid == Edit.Model().Root)
	{
		if (!CheckFields(Entry, EntryValue, {TEXT("Root"), TEXT("Tags")}, TEXT("binding.set entry of the template root")))
		{
			return false;
		}
		const FValue* Root = FindField(EntryValue, TEXT("Root"));
		if (Root == nullptr || Root->Kind != EAssetDocumentRuntimeValueKind::Object)
		{
			return Entry.Fail(TEXT("Sequencer.InvalidBody"), TEXT("The template root's entry has Root {Class, Template?, ...}."), TEXT("Root"));
		}
		Edit.MovieScene.Modify();
		if (!WriteRoot(Edit, Entry, *Root, Guid)
			|| !ReadTags(Entry, Tags != nullptr ? *Tags : NoTags, Edit.MovieScene.FindSpawnable(Guid)->Tags))
		{
			return false;
		}
		Edit.bChanged = true;
		Edit.Invalidate();
		return true;
	}
	if (!CheckFields(Entry, EntryValue, {TEXT("Class"), TEXT("Component"), TEXT("Tags")}, TEXT("binding.set entry of a component binding")))
	{
		return false;
	}
	FMovieScenePossessable* Possessable = Edit.MovieScene.FindPossessable(Guid);
	if (Possessable == nullptr)
	{
		return Op.Fail(TEXT("Sequencer.UnknownBinding"), FString::Printf(TEXT("The sequence has no binding %s."), *Guid.ToString(EGuidFormats::Digits)),
			TEXT("guid"));
	}
	const FValue* Class = FindField(EntryValue, TEXT("Class"));
	const FValue* Component = FindField(EntryValue, TEXT("Component"));
	if ((Class != nullptr && Class->Kind != EAssetDocumentRuntimeValueKind::String)
		|| (Component != nullptr && Component->Kind != EAssetDocumentRuntimeValueKind::String))
	{
		return Entry.Fail(TEXT("Sequencer.InvalidValue"), TEXT("Class and Component are strings."), Class != nullptr
			&& Class->Kind != EAssetDocumentRuntimeValueKind::String ? TEXT("Class") : TEXT("Component"));
	}
	Edit.MovieScene.Modify();
	Template.Modify();
	SetPossessedClass(*Possessable, Class != nullptr ? Class->StringValue : FString());
	if (Component != nullptr && !Component->StringValue.IsEmpty())
	{
		Template.BoundActorComponents.Add(Guid, FName(*Component->StringValue));
	}
	else
	{
		Template.BoundActorComponents.Remove(Guid);
	}
	if (!ReadTags(Entry, Tags != nullptr ? *Tags : NoTags, Possessable->Tags))
	{
		return false;
	}
	Edit.bChanged = true;
	Edit.Invalidate();
	return true;
}
} // namespace UE::AssetDocument::Graph::Sequencer::Edit
