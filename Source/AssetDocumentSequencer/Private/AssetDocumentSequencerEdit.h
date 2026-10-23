// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentSequencerPrimitives.h"

class AActor;
struct FMovieScenePossessable;
class UMovieSceneCustomBinding;
class UMovieSceneSequence;
class UMovieScene;
class UMovieSceneSection;
class UMovieSceneTrack;

// Shared state of one seq.edit.v1 call (AssetDocumentSequencerEdit.cpp, AssetDocumentSequencerEditTracks.cpp,
// AssetDocumentSequencerBindingConversion.cpp).
namespace UE::AssetDocument::Graph::Sequencer::Edit
{
struct FEdit
{
	UMovieSceneSequence& Sequence;
	UMovieScene& MovieScene;
	bool bChanged = false;
	// The movie scene's read-only flag at the end of the call (it is lifted while ops run, as the editor's tick
	// resolution change does, because sections refuse TryModify in a read-only scene).
	bool bReadOnly = false;
	// Bindings removed (or recreated) this call, for the dependents report.
	TArray<FString> RemovedBindings;
	// Index of the op that runs, and the objects ops created ({op, object}: the object name later ops address).
	int32 CurrentOp = 0;
	TArray<FValue> Created;

	FEdit(UMovieSceneSequence& InSequence, UMovieScene& InMovieScene)
		: Sequence(InSequence), MovieScene(InMovieScene)
	{
	}

	// The document model of the sequence as it is now (rebuilt after structural edits).
	const FSequenceModel& Model();
	void Invalidate() { bModelValid = false; }
	void NoteCreated(const UObject& Object);

private:
	FSequenceModel CachedModel;
	bool bModelValid = false;
};

// The two rates of the movie scene.
FTimeBase TimeOf(const UMovieScene& MovieScene);
// Object has only the Allowed keys (Graph.UnknownField otherwise).
bool CheckFields(FReader& Reader, const FValue& Object, std::initializer_list<const TCHAR*> Allowed, const TCHAR* What);
// A spawnable's template (design §3.4.3): {Properties?, Components?}, complete (omitted fields take the defaults).
bool WriteTemplate(FEdit& Edit, FReader& Reader, AActor& Template, const FValue* Saved);
// PossessedObjectClass as a class path ("" = none).
void SetPossessedClass(FMovieScenePossessable& Possessable, const FString& Path);
// Tags: an array of names.
bool ReadTags(FReader& Entry, const FValue& Tags, TArray<FName>& Out);

// Reads the GUID field Key of an op.
bool ReadGuid(FReader& Op, const TCHAR* Key, FGuid& Out);
// A track by its object name (global, camera cut or binding track).
UMovieSceneTrack* FindTrackByName(UMovieScene& MovieScene, const FString& Name);

// ---- tracks, sections, channels (AssetDocumentSequencerEditTracks.cpp) ----------------------------------------------

bool OpTrackAdd(FEdit& Edit, FReader& Op);
bool OpTrackRemove(FEdit& Edit, FReader& Op);
bool OpTrackSet(FEdit& Edit, FReader& Op);
bool OpTrackOrder(FEdit& Edit, FReader& Op);
bool OpSectionAdd(FEdit& Edit, FReader& Op);
bool OpSectionSet(FEdit& Edit, FReader& Op);
bool OpSectionRemove(FEdit& Edit, FReader& Op);
bool OpChannelAppend(FEdit& Edit, FReader& Op);
bool OpParametersAppend(FEdit& Edit, FReader& Op);
// Removes a track from the movie scene and from the folders that list it.
void RemoveTrack(FEdit& Edit, UMovieSceneTrack& Track);

// ---- binding kinds (AssetDocumentSequencerBindingConversion.cpp) ----------------------------------------------------

// The editor's "Convert To ..." without a world (FSequencerUtilities::ConvertToPossessable / ConvertToCustomBinding data
// steps): the binding keeps its GUID, tracks, children and folder; only its references change.
// To a possessable: the custom binding references are removed (the locators are written next).
void ConvertToPossessable(FEdit& Edit, const FGuid& Guid);
// To a custom binding of BindingClass (a spawnable actor binding when SpawnClass is set: its template is a new actor of
// SpawnClass, as CreateNewCustomBinding makes it from a class). An existing reference of the same binding class (and
// template class) is kept. Returns the binding's single custom binding, or null with an error on Op.
UMovieSceneCustomBinding* ConvertToCustomBinding(FEdit& Edit, const FGuid& Guid, UClass& BindingClass, UClass* SpawnClass, FReader& Op,
	const FString& Path);

// ---- director and groups (AssetDocumentSequencerDirector.cpp, AssetDocumentSequencerGroups.cpp) --------------------

// director.create {}: Sequencer's "Open Director Blueprint" (GetOrCreateDirectorBlueprint); created row {op, object,
// director: {Blueprint, Class}}.
bool OpDirectorCreate(FEdit& Edit, FReader& Op);
// director.remove {}: the director leaves the sequence (Sequencer.DirectorInUse while events or director bindings call it).
bool OpDirectorRemove(FEdit& Edit, FReader& Op);
// section_groups.set {groups: [[{track, section}, ...]]}: every section group (Ungroup all, then Group each list).
bool OpSectionGroupsSet(FEdit& Edit, FReader& Op);
// groups.set {groups: [{Name, Members}]}: the node groups, in order.
bool OpNodeGroupsSet(FEdit& Edit, FReader& Op);

// ---- template sequences (AssetDocumentSequencerTemplateRoot.cpp) -----------------------------------------------------

// template.root {class}: the bound actor class of a Template / Camera Animation Sequence (the Details panel's class
// picker, FTemplateSequenceEditorUtil::ChangeActorBinding without an ISequencer).
bool OpTemplateRoot(FEdit& Edit, FReader& Op);
// binding.set of a template sequence: the root spawnable {Root: {Class, Template?, Ownership?, ...}, Tags?} or a
// component binding {Class?, Component, Tags?}.
bool SetTemplateBinding(FEdit& Edit, FReader& Op, FReader& Entry, const FValue& EntryValue, const FGuid& Guid);
} // namespace UE::AssetDocument::Graph::Sequencer::Edit
