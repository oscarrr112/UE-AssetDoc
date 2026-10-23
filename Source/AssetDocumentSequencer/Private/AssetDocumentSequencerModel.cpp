// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// The document structure of a sequence (design §3.2, §3.4.2, §3.5): bindings as a tree in Sequencer's order with
// their names, binding paths and document paths; tracks per scope with type tokens and identities; folders and their
// members. Read only.

#include "AssetDocumentSequencerPrimitives.h"

#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneFolder.h"
#include "MovieSceneNameableTrack.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSection.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneSequence.h"
#include "MovieSceneTrack.h"
#include "LevelSequence.h"
#include "TemplateSequence.h"
#include "Sections/MovieSceneEventSectionBase.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "UObject/UObjectIterator.h"

namespace UE::AssetDocument::Graph::Sequencer
{
namespace
{
struct FTypeToken
{
	const TCHAR* ClassName; // without the U prefix
	const TCHAR* Token;
};

// Design §3.5.1. Classes of /Script/MovieSceneTracks and /Script/MovieScene.
const FTypeToken TrackTokens[] = {
	{TEXT("MovieScene3DTransformTrack"), TEXT("Transform")},
	{TEXT("MovieSceneFloatTrack"), TEXT("Float")},
	{TEXT("MovieSceneDoubleTrack"), TEXT("Double")},
	{TEXT("MovieSceneBoolTrack"), TEXT("Bool")},
	{TEXT("MovieSceneByteTrack"), TEXT("Byte")},
	{TEXT("MovieSceneEnumTrack"), TEXT("Enum")},
	{TEXT("MovieSceneIntegerTrack"), TEXT("Integer")},
	{TEXT("MovieSceneStringTrack"), TEXT("String")},
	{TEXT("MovieSceneTextTrack"), TEXT("Text")},
	{TEXT("MovieSceneDoubleVectorTrack"), TEXT("Vector")},
	{TEXT("MovieSceneFloatVectorTrack"), TEXT("FloatVector")},
	{TEXT("MovieSceneColorTrack"), TEXT("Color")},
	{TEXT("MovieSceneRotatorTrack"), TEXT("Rotator")},
	{TEXT("MovieSceneEulerTransformTrack"), TEXT("EulerTransform")},
	{TEXT("MovieSceneObjectPropertyTrack"), TEXT("ObjectProperty")},
	{TEXT("MovieSceneVisibilityTrack"), TEXT("Visibility")},
	{TEXT("MovieSceneSpawnTrack"), TEXT("Spawn")},
	{TEXT("MovieScene3DAttachTrack"), TEXT("Attach")},
	{TEXT("MovieScene3DPathTrack"), TEXT("Path")},
	{TEXT("MovieSceneActorReferenceTrack"), TEXT("ActorReference")},
	{TEXT("MovieSceneSkeletalAnimationTrack"), TEXT("SkeletalAnimation")},
	{TEXT("MovieSceneComponentMaterialTrack"), TEXT("Material")},
	{TEXT("MovieScenePrimitiveMaterialTrack"), TEXT("PrimitiveMaterial")},
	{TEXT("MovieSceneCustomPrimitiveDataTrack"), TEXT("CustomPrimitiveData")},
	{TEXT("MovieSceneCameraShakeTrack"), TEXT("CameraShake")},
	{TEXT("MovieSceneCameraShakeSourceShakeTrack"), TEXT("CameraShakeSource")},
	{TEXT("MovieSceneCameraShakeSourceTriggerTrack"), TEXT("CameraShakeSourceTrigger")},
	{TEXT("MovieSceneParticleTrack"), TEXT("Particle")},
	{TEXT("MovieSceneParticleParameterTrack"), TEXT("ParticleParameter")},
	{TEXT("MovieSceneEventTrack"), TEXT("Event")},
	{TEXT("MovieSceneAudioTrack"), TEXT("Audio")},
	{TEXT("MovieSceneCameraCutTrack"), TEXT("CameraCut")},
	{TEXT("MovieSceneCinematicShotTrack"), TEXT("Shot")},
	{TEXT("MovieSceneSubTrack"), TEXT("Subsequence")},
	{TEXT("MovieSceneFadeTrack"), TEXT("Fade")},
	{TEXT("MovieSceneSlomoTrack"), TEXT("Slomo")},
	{TEXT("MovieSceneLevelVisibilityTrack"), TEXT("LevelVisibility")},
	{TEXT("MovieSceneDataLayerTrack"), TEXT("DataLayer")},
	{TEXT("MovieSceneCVarTrack"), TEXT("CVar")},
	{TEXT("MovieSceneMaterialParameterCollectionTrack"), TEXT("MaterialParameterCollection")},
	{TEXT("MovieSceneTimeWarpTrack"), TEXT("TimeWarp")},
};

bool IsEngineMovieScenePackage(const UClass& Class)
{
	const FName Package = Class.GetOutermost()->GetFName();
	return Package == TEXT("/Script/MovieSceneTracks") || Package == TEXT("/Script/MovieScene");
}

int32 BindingSortingOrder(const UMovieScene& MovieScene, const FGuid& Guid)
{
	const FMovieSceneBinding* Binding = MovieScene.FindBinding(Guid);
#if WITH_EDITORONLY_DATA
	return Binding != nullptr ? Binding->GetSortingOrder() : MAX_int32;
#else
	return Binding != nullptr ? 0 : MAX_int32;
#endif
}

// Sequencer's order of siblings: sorting order (the tree's drag order), then the stored order.
template <typename T, typename FKey>
void SortStable(TArray<T>& Items, FKey Key)
{
	TArray<TPair<int32, int32>> Keys;
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		Keys.Emplace(Key(Items[Index]), Index);
	}
	Keys.Sort([](const TPair<int32, int32>& A, const TPair<int32, int32>& B)
	{
		return A.Key != B.Key ? A.Key < B.Key : A.Value < B.Value;
	});
	TArray<T> Sorted;
	for (const TPair<int32, int32>& Entry : Keys)
	{
		Sorted.Add(Items[Entry.Value]);
	}
	Items = MoveTemp(Sorted);
}

int32 TrackSortingOrder(const UMovieSceneTrack* Track)
{
#if WITH_EDITORONLY_DATA
	return Track != nullptr ? Track->GetSortingOrder() : 0;
#else
	return 0;
#endif
}

FString TrackDisplayName(const UMovieSceneTrack& Track)
{
	static const FTextProperty* DisplayName = FindFProperty<FTextProperty>(UMovieSceneNameableTrack::StaticClass(), TEXT("DisplayName"));
	if (DisplayName == nullptr || !Track.IsA<UMovieSceneNameableTrack>())
	{
		return FString();
	}
	return DisplayName->GetPropertyValue_InContainer(&Track).ToString();
}

void AddTrack(FSequenceModel& Model, const UMovieSceneTrack* Track, const FGuid& Binding, const FString& Scope, int32 Index)
{
	if (Track == nullptr)
	{
		return;
	}
	FTrackInfo Info;
	Info.Track = Track;
	Info.Binding = Binding;
	Info.DocumentPath = FString::Printf(TEXT("%s/Tracks/%d"), *Scope, Index);
	Info.Type = TrackTypeToken(*Track->GetClass());
	if (const UMovieScenePropertyTrack* Property = Cast<UMovieScenePropertyTrack>(Track))
	{
		Info.Property = Property->GetPropertyPath().IsNone() ? FString() : Property->GetPropertyPath().ToString();
	}
	Info.Name = TrackDisplayName(*Track);
	Model.TrackIndex.Add(Track, Model.Tracks.Num());
	Model.Tracks.Add(MoveTemp(Info));
}

// Tracks of one scope whose (Type, Property, Name) repeats get Id (design §3.5.2).
void MarkTrackIds(FSequenceModel& Model, int32 First, int32 End)
{
	TMap<FString, int32> Counts;
	for (int32 Index = First; Index < End; ++Index)
	{
		const FTrackInfo& Info = Model.Tracks[Index];
		++Counts.FindOrAdd(Info.Type + TEXT("|") + Info.Property + TEXT("|") + Info.Name);
	}
	for (int32 Index = First; Index < End; ++Index)
	{
		FTrackInfo& Info = Model.Tracks[Index];
		Info.bNeedsId = Counts.FindRef(Info.Type + TEXT("|") + Info.Property + TEXT("|") + Info.Name) > 1;
	}
}

void AddFolders(FSequenceModel& Model, TArray<UMovieSceneFolder*> Folders, const FString& Prefix)
{
#if WITH_EDITORONLY_DATA
	SortStable(Folders, [](const UMovieSceneFolder* Folder) { return Folder != nullptr ? Folder->GetSortingOrder() : MAX_int32; });
	for (const UMovieSceneFolder* Folder : Folders)
	{
		if (Folder == nullptr)
		{
			continue;
		}
		const FString Path = Prefix.IsEmpty() ? Folder->GetFolderName().ToString() : Prefix + TEXT("/") + Folder->GetFolderName().ToString();
		Model.Folders.Add({Path, Folder});
		for (const FGuid& Guid : Folder->GetChildObjectBindings())
		{
			Model.BindingFolders.FindOrAdd(Guid, Path);
		}
		for (const UMovieSceneTrack* Track : Folder->GetChildTracks())
		{
			if (Track != nullptr)
			{
				Model.TrackFolders.FindOrAdd(Track, Path);
			}
		}
		TArray<UMovieSceneFolder*> Children(Folder->GetChildFolders());
		AddFolders(Model, MoveTemp(Children), Path);
	}
#endif
}

void AddBindingSubtree(FSequenceModel& Model, const FGuid& Guid, const FString& ParentPath, const FString& DocumentPath)
{
	FBindingInfo& Info = Model.Bindings[Guid];
	Info.Path = ParentPath.IsEmpty() ? Info.Segment : ParentPath + TEXT("/") + Info.Segment;
	Info.DocumentPath = DocumentPath;
	Model.Order.Add(Guid);
	const TArray<FGuid> Children = Info.Children;
	const FString Path = Info.Path;
	for (int32 Index = 0; Index < Children.Num(); ++Index)
	{
		AddBindingSubtree(Model, Children[Index], Path, FString::Printf(TEXT("%s/Children/%d"), *DocumentPath, Index));
	}
}

void CollectSubtree(const FSequenceModel& Model, const FGuid& Guid, TArray<FGuid>& Out)
{
	Out.Add(Guid);
	for (const FGuid& Child : Model.Bindings[Guid].Children)
	{
		CollectSubtree(Model, Child, Out);
	}
}

// Siblings sharing a name (or a name that cannot be a path segment) are identified by Id.
void MarkBindingIds(FSequenceModel& Model, const TArray<FGuid>& Siblings)
{
	TMap<FString, int32> Counts;
	for (const FGuid& Guid : Siblings)
	{
		++Counts.FindOrAdd(Model.Bindings[Guid].Name);
	}
	for (const FGuid& Guid : Siblings)
	{
		FBindingInfo& Info = Model.Bindings[Guid];
		Info.bNeedsId = Counts[Info.Name] > 1 || Info.Name.IsEmpty() || Info.Name.Contains(TEXT("/")) || Info.Name.StartsWith(TEXT("#"));
		Info.Segment = Info.bNeedsId ? TEXT("#") + Guid.ToString(EGuidFormats::Digits) : Info.Name;
	}
}
} // namespace

FString TrackTypeToken(const UClass& TrackClass)
{
	if (IsEngineMovieScenePackage(TrackClass))
	{
		const FString Name = TrackClass.GetName();
		for (const FTypeToken& Token : TrackTokens)
		{
			if (Name == Token.ClassName)
			{
				return Token.Token;
			}
		}
	}
	return TrackClass.GetPathName();
}

UClass* TrackClassOf(const FString& Type)
{
	for (const FTypeToken& Token : TrackTokens)
	{
		if (Type == Token.Token)
		{
			for (const TCHAR* Package : {TEXT("/Script/MovieSceneTracks."), TEXT("/Script/MovieScene.")})
			{
				if (UClass* Class = FindObject<UClass>(nullptr, *(FString(Package) + Token.ClassName)))
				{
					return Class;
				}
			}
			return nullptr;
		}
	}
	if (!Type.StartsWith(TEXT("/")))
	{
		return nullptr;
	}
	UClass* Class = FindObject<UClass>(nullptr, *Type);
	if (Class == nullptr)
	{
		Class = LoadObject<UClass>(nullptr, *Type, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	return Class != nullptr && Class->IsChildOf(UMovieSceneTrack::StaticClass()) ? Class : nullptr;
}

UClass* SectionClassOf(const FString& Token, const UClass& TrackClass)
{
	const UMovieSceneTrack* Defaults = Cast<UMovieSceneTrack>(TrackClass.GetDefaultObject());
	if (Defaults == nullptr)
	{
		return nullptr;
	}
	for (UClass* Section : ConcreteSubclasses(*UMovieSceneSection::StaticClass()))
	{
		if (TakesSectionClass(*Defaults, Section) && (SectionClassToken(*Section, TrackClass) == Token || Section->GetPathName() == Token))
		{
			return Section;
		}
	}
	return nullptr;
}

FString SectionClassToken(const UClass& SectionClass, const UClass& TrackClass)
{
	if (!IsEngineMovieScenePackage(SectionClass))
	{
		return SectionClass.GetPathName();
	}
	FString Name = SectionClass.GetName();
	Name.RemoveFromStart(TEXT("MovieScene"));
	Name.RemoveFromEnd(TEXT("Section"));
	const FString TrackToken = TrackTypeToken(TrackClass);
	if (Name.StartsWith(TrackToken) && Name.Len() > TrackToken.Len())
	{
		Name.RightChopInline(TrackToken.Len());
	}
	return Name;
}

TArray<UClass*> ConcreteSubclasses(const UClass& Base)
{
	TArray<UClass*> Classes;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (Class->IsChildOf(&Base) && !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_Hidden)
			&& !Class->GetName().StartsWith(TEXT("SKEL_")) && !Class->GetName().StartsWith(TEXT("REINST_"))
			&& !Class->GetOutermost()->GetName().StartsWith(TEXT("/Engine/Transient")))
		{
			Classes.Add(Class);
		}
	}
	Classes.Sort([](const UClass& A, const UClass& B) { return A.GetPathName() < B.GetPathName(); });
	return Classes;
}

bool TakesSectionClass(const UMovieSceneTrack& TrackDefaults, UClass* SectionClass)
{
	return SectionClass != nullptr && (TrackDefaults.SupportsType(SectionClass)
		|| (TrackDefaults.IsA<UMovieSceneEventTrack>() && SectionClass->IsChildOf(UMovieSceneEventSectionBase::StaticClass())));
}

FOptionalMovieSceneBlendType NewSectionBlend(const UMovieSceneSection& Section)
{
	UMovieSceneTrack* Track = Cast<UMovieSceneTrack>(Section.GetOuter());
	if (Track == nullptr)
	{
		return CastChecked<UMovieSceneSection>(Section.GetClass()->GetDefaultObject())->GetBlendType();
	}
	// What the track's "add section" and the package load construct: a section of the class inside the track.
	UMovieSceneSection* Twin = NewObject<UMovieSceneSection>(Track, Section.GetClass(), NAME_None, RF_Transient);
	const FOptionalMovieSceneBlendType Blend = Twin->GetBlendType();
	Twin->MarkAsGarbage();
	return Blend;
}

bool HasSeveralSectionClasses(const UClass& TrackClass)
{
	static TMap<FString, bool> Cache;
	const FString Key = TrackClass.GetPathName();
	if (const bool* Found = Cache.Find(Key))
	{
		return *Found;
	}
	const UMovieSceneTrack* Defaults = Cast<UMovieSceneTrack>(TrackClass.GetDefaultObject());
	int32 Count = 0;
	if (Defaults != nullptr)
	{
		for (UClass* Section : ConcreteSubclasses(*UMovieSceneSection::StaticClass()))
		{
			Count += TakesSectionClass(*Defaults, Section) ? 1 : 0;
		}
	}
	Cache.Add(Key, Count > 1);
	return Count > 1;
}

FString FSequenceModel::SectionPath(const UMovieSceneSection& Section) const
{
	const UMovieSceneTrack* Track = Section.GetTypedOuter<UMovieSceneTrack>();
	const int32* Index = Track != nullptr ? TrackIndex.Find(Track) : nullptr;
	if (Index == nullptr)
	{
		return TEXT("/Body");
	}
	const int32 SectionIndex = Track->GetAllSections().IndexOfByKey(&Section);
	return FString::Printf(TEXT("%s/Sections/%d"), *Tracks[*Index].DocumentPath, SectionIndex);
}

bool IsDocumentedSequence(const UObject& Asset)
{
	return Asset.IsA<ULevelSequence>() || Asset.IsA<UTemplateSequence>();
}

bool BuildModel(const UMovieSceneSequence& Sequence, FSequenceModel& OutModel, FString& OutError)
{
	UMovieScene* MovieScene = Sequence.GetMovieScene();
	if (MovieScene == nullptr)
	{
		OutError = FString::Printf(TEXT("%s has no movie scene."), *Sequence.GetPathName());
		return false;
	}
	FSequenceModel& Model = OutModel;
	Model.Sequence = &Sequence;
	Model.MovieScene = MovieScene;
	Model.Time.Tick = MovieScene->GetTickResolution();
	Model.Time.Display = MovieScene->GetDisplayRate();

	// Bindings: possessables; the tree is ParentGuid (a parent that is not a possessable makes the child a root). A
	// Template Sequence adds its root spawnable (the first one, UTemplateSequence::GetRootObjectBindingID), the parent of
	// its component bindings, and documents only that tree.
	TSet<FGuid> Possessed;
	for (int32 Index = 0; Index < MovieScene->GetPossessableCount(); ++Index)
	{
		const FMovieScenePossessable& Possessable = MovieScene->GetPossessable(Index);
		FBindingInfo Info;
		Info.Guid = Possessable.GetGuid();
		Info.Parent = Possessable.GetParent();
		const FText Label = MovieScene->GetObjectDisplayName(Info.Guid);
		Info.Name = Label.IsEmpty() ? Possessable.GetName() : Label.ToString();
		Model.Bindings.Add(Info.Guid, MoveTemp(Info));
		Possessed.Add(Possessable.GetGuid());
	}
	Model.bTemplate = Sequence.IsA<UTemplateSequence>();
	if (Model.bTemplate && MovieScene->GetSpawnableCount() > 0 && !Possessed.Contains(MovieScene->GetSpawnable(0).GetGuid()))
	{
		const FMovieSceneSpawnable& Spawnable = MovieScene->GetSpawnable(0);
		FBindingInfo Info;
		Info.Guid = Spawnable.GetGuid();
		Info.bRoot = true;
		const FText Label = MovieScene->GetObjectDisplayName(Info.Guid);
		Info.Name = Label.IsEmpty() ? Spawnable.GetName() : Label.ToString();
		Model.Root = Info.Guid;
		Model.Bindings.Add(Info.Guid, MoveTemp(Info));
		Possessed.Add(Model.Root);
	}
	TArray<FGuid> Roots;
	for (const FMovieSceneBinding& Binding : static_cast<const UMovieScene*>(MovieScene)->GetBindings())
	{
		const FGuid& Guid = Binding.GetObjectGuid();
		if (!Possessed.Contains(Guid))
		{
			Model.Orphans.Add(Guid);
		}
	}
	// Stored order of the possessables follows ObjectBindings (the order bindings were added).
	TArray<FGuid> Stored;
	for (const FMovieSceneBinding& Binding : static_cast<const UMovieScene*>(MovieScene)->GetBindings())
	{
		if (Possessed.Contains(Binding.GetObjectGuid()))
		{
			Stored.Add(Binding.GetObjectGuid());
		}
	}
	for (int32 Index = 0; Index < MovieScene->GetPossessableCount(); ++Index)
	{
		const FGuid& Guid = MovieScene->GetPossessable(Index).GetGuid();
		if (!Stored.Contains(Guid))
		{
			Stored.Add(Guid);
		}
	}
	if (Model.Root.IsValid() && !Stored.Contains(Model.Root))
	{
		Stored.Add(Model.Root);
	}
	for (const FGuid& Guid : Stored)
	{
		FBindingInfo& Info = Model.Bindings[Guid];
		if (Info.Parent.IsValid() && Info.Parent != Guid && Model.Bindings.Contains(Info.Parent))
		{
			Model.Bindings[Info.Parent].Children.Add(Guid);
		}
		else
		{
			Roots.Add(Guid);
		}
	}
	if (Model.bTemplate)
	{
		// Bindings outside the root's tree are not part of a template's document (kept as they are).
		TArray<FGuid> Outside;
		for (const FGuid& Guid : Roots)
		{
			if (Guid != Model.Root)
			{
				CollectSubtree(Model, Guid, Outside);
			}
		}
		for (const FGuid& Guid : Outside)
		{
			Model.Orphans.Add(Guid);
			Model.Bindings.Remove(Guid);
			Stored.Remove(Guid);
		}
		Roots.Reset();
		if (Model.Root.IsValid())
		{
			Roots.Add(Model.Root);
		}
	}
	auto Order = [MovieScene](const FGuid& Guid) { return BindingSortingOrder(*MovieScene, Guid); };
	SortStable(Roots, Order);
	MarkBindingIds(Model, Roots);
	for (TPair<FGuid, FBindingInfo>& Entry : Model.Bindings)
	{
		SortStable(Entry.Value.Children, Order);
	}
	for (const FGuid& Guid : Stored)
	{
		MarkBindingIds(Model, Model.Bindings[Guid].Children);
	}
	Model.Roots = Roots;
	for (int32 Index = 0; Index < Roots.Num(); ++Index)
	{
		AddBindingSubtree(Model, Roots[Index], FString(),
			Model.bTemplate ? FString(TEXT("/Body/Root")) : FString::Printf(TEXT("/Body/Bindings/%d"), Index));
	}

	// Folders.
	AddFolders(Model, TArray<UMovieSceneFolder*>(MovieScene->GetRootFolders()), FString());

	// Global tracks: the camera cut track first (Sequencer pins it at the top), then by sorting order.
	{
		const int32 First = Model.Tracks.Num();
		int32 Index = 0;
		if (const UMovieSceneTrack* CameraCut = MovieScene->GetCameraCutTrack())
		{
			AddTrack(Model, CameraCut, FGuid(), TEXT("/Body"), Index++);
		}
		TArray<UMovieSceneTrack*> Tracks = MovieScene->GetTracks();
		SortStable(Tracks, &TrackSortingOrder);
		for (const UMovieSceneTrack* Track : Tracks)
		{
			AddTrack(Model, Track, FGuid(), TEXT("/Body"), Index++);
		}
		MarkTrackIds(Model, First, Model.Tracks.Num());
	}
	for (const FGuid& Guid : Model.Order)
	{
		const FMovieSceneBinding* Binding = MovieScene->FindBinding(Guid);
		if (Binding == nullptr)
		{
			continue;
		}
		TArray<UMovieSceneTrack*> Tracks = Binding->GetTracks();
		SortStable(Tracks, &TrackSortingOrder);
		const int32 First = Model.Tracks.Num();
		int32 Index = 0;
		for (const UMovieSceneTrack* Track : Tracks)
		{
			AddTrack(Model, Track, Guid, Model.Bindings[Guid].DocumentPath, Index++);
		}
		MarkTrackIds(Model, First, Model.Tracks.Num());
	}

	// Section groups (UMovieScene::GroupSections): numbered from 1 by their first section in model order.
	{
		TMap<const FMovieSceneSectionGroup*, int32> Numbers;
		for (const FTrackInfo& Track : Model.Tracks)
		{
			for (const UMovieSceneSection* Section : Track.Track->GetAllSections())
			{
				const FMovieSceneSectionGroup* Group = Section != nullptr ? MovieScene->GetSectionGroup(*Section) : nullptr;
				if (Group != nullptr && Group->Num() > 1)
				{
					int32 Number = Numbers.Num() + 1; // 1, 2, ... (the document form)
					if (const int32* Found = Numbers.Find(Group))
					{
						Number = *Found;
					}
					else
					{
						Numbers.Add(Group, Number);
					}
					Model.SectionGroups.Add(Section, Number);
				}
			}
		}
	}
	return true;
}
} // namespace UE::AssetDocument::Graph::Sequencer
