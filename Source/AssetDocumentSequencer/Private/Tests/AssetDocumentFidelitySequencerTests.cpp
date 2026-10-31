// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Level Sequence content that Sequencer authors outside the tracks, on editor-made sequences. Each extract re-applies as
// a no-op, a new sequence created from the extract holds the same content, and an edited document writes it.
// UEAssetDoc.Fidelity.Sequencer.BindingTags: binding tags (the binding tag panel, what gameplay finds bindings by)
//   read per tag; a tag removed and a binding tagged by an edit; tags omitted from a document are kept.
// UEAssetDoc.Fidelity.Sequencer.SpawnedBy: a possessable bound to a parent sequence's spawnable reads its SpawnedBy
//   reference; omitted it is kept, null clears it.
// UEAssetDoc.Fidelity.Sequencer.SpawnedByStale: a possessable whose spawnable was deleted reads SpawnedBy "#<Id>";
//   its Tags edit applies with SpawnedBy as extracted or omitted, and keeps the stored reference. A possessable whose
//   spawnable the same document renames keeps its reference when SpawnedBy is omitted.
// UEAssetDoc.Fidelity.Sequencer.MetaDataObjects: shot metadata (rating, flags) next to Author / Notes reads as
//   MetaData.Objects; an edit changes it, Objects omitted keeps it, an empty Objects removes it.

#include "AssetDocumentTolerantTestSupport.h"

#include "LevelSequence.h"
#include "MetaData/MovieSceneShotMetaData.h"
#include "Misc/ScopeExit.h"
#include "MovieScene.h"
#include "MovieSceneMetaData.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSequenceID.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentFidelitySequencerTestsLocal
{
using namespace AssetDocumentTolerantTestSupport;

// A new Level Sequence as the Content Browser's "Level Sequence" makes it, 30 fps, frames 0-150.
ULevelSequence* NewFidelitySequence(const FTarget& Target)
{
	ULevelSequence* Sequence = Target.Create<ULevelSequence>();
	Sequence->Initialize();
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	MovieScene->SetDisplayRate(FFrameRate(30, 1));
	MovieScene->SetTickResolutionDirectly(FFrameRate(24000, 1));
	MovieScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(150 * 800)));
	return Sequence;
}

// The extract applied as a Create of Other (a new sequence from the document).
FAssetDocumentResult CreateFrom(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Extracted, const FTarget& Other)
{
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Extracted));
	Copy->SetStringField(TEXT("Action"), TEXT("Create"));
	Copy->SetStringField(TEXT("Target"), Other.PackageName);
	return ApplyText(Test, Other, Serialize(Copy), TEXT("Create"));
}

FGuid PossessableNamed(const UMovieScene* ConstMovieScene, const TCHAR* Name)
{
	UMovieScene* MovieScene = const_cast<UMovieScene*>(ConstMovieScene);
	for (int32 Index = 0; MovieScene != nullptr && Index < MovieScene->GetPossessableCount(); ++Index)
	{
		if (MovieScene->GetPossessable(Index).GetName() == Name)
		{
			return MovieScene->GetPossessable(Index).GetGuid();
		}
	}
	return FGuid();
}

// The GUIDs a tag names, in order (root-sequence bindings).
TArray<FGuid> Tagged(const UMovieScene* MovieScene, const TCHAR* Tag)
{
	TArray<FGuid> Guids;
	const FMovieSceneObjectBindingIDs* Ids = MovieScene != nullptr ? MovieScene->AllTaggedBindings().Find(Tag) : nullptr;
	for (int32 Index = 0; Ids != nullptr && Index < Ids->IDs.Num(); ++Index)
	{
		Guids.Add(Ids->IDs[Index].GetGuid());
	}
	return Guids;
}

TSharedRef<FJsonValueObject> BindingRef(const TCHAR* Path)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("Binding"), Path);
	return MakeShared<FJsonValueObject>(Object);
}

const UMovieScene* MovieSceneOf(const FTarget& Target)
{
	const ULevelSequence* Sequence = Target.Get<ULevelSequence>();
	return Sequence != nullptr ? Sequence->GetMovieScene() : nullptr;
}

FMovieSceneObjectBindingID SpawnedByOf(const FTarget& Target, const TCHAR* Name)
{
	UMovieScene* MovieScene = const_cast<UMovieScene*>(MovieSceneOf(Target));
	const FMovieScenePossessable* Possessable = MovieScene != nullptr ? MovieScene->FindPossessable(PossessableNamed(MovieScene, Name)) : nullptr;
	return Possessable != nullptr ? Possessable->GetSpawnableObjectBindingID() : FMovieSceneObjectBindingID();
}

const UMovieSceneShotMetaData* ShotMetaDataOf(const FTarget& Target)
{
	const ULevelSequence* Sequence = Target.Get<ULevelSequence>();
	return Sequence != nullptr ? Sequence->FindMetaData<UMovieSceneShotMetaData>() : nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelitySequencerBindingTagsTest, "UEAssetDoc.Fidelity.Sequencer.BindingTags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelitySequencerBindingTagsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelitySequencerTestsLocal;
	const FTarget Target(TEXT("LS_FidelityTags"));
	const FTarget Created(TEXT("LS_FidelityTagsCreated"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); Created.Cleanup(); };

	// The binding tag panel: "Hero" tags the hero, "Doors" tags the door and the hero.
	ULevelSequence* Sequence = NewFidelitySequence(Target);
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	const FGuid Hero = MovieScene->AddPossessable(TEXT("Hero"), AActor::StaticClass());
	const FGuid Door = MovieScene->AddPossessable(TEXT("Door"), AActor::StaticClass());
	MovieScene->TagBinding(TEXT("Hero"), UE::MovieScene::FFixedObjectBindingID(Hero, MovieSceneSequenceID::Root));
	MovieScene->TagBinding(TEXT("Doors"), UE::MovieScene::FFixedObjectBindingID(Door, MovieSceneSequenceID::Root));
	MovieScene->TagBinding(TEXT("Doors"), UE::MovieScene::FFixedObjectBindingID(Hero, MovieSceneSequenceID::Root));
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Binding tags"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("Hero tag read"), StringAt(Extracted, TEXT("/Body/BindingTags/Hero/0/Binding")), FString(TEXT("Hero")));
	TestEqual(TEXT("Doors tag read in order"), StringAt(Extracted, TEXT("/Body/BindingTags/Doors/1/Binding")), FString(TEXT("Hero")));

	if (TestTrue(TEXT("create from the extract"), CreateFrom(*this, Extracted, Created).IsSuccess()))
	{
		const UMovieScene* New = MovieSceneOf(Created);
		const FGuid NewHero = PossessableNamed(New, TEXT("Hero"));
		const FGuid NewDoor = PossessableNamed(New, TEXT("Door"));
		TestTrue(TEXT("the new sequence has the tags"), Tagged(New, TEXT("Hero")) == TArray<FGuid>({NewHero})
			&& Tagged(New, TEXT("Doors")) == TArray<FGuid>({NewDoor, NewHero}));
	}

	// An edit: Doors removed, the door tagged Hero too.
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedRef<FJsonObject> Tags = MakeShared<FJsonObject>();
		Tags->SetArrayField(TEXT("Hero"), {BindingRef(TEXT("Hero")), BindingRef(TEXT("Door"))});
		Document->GetObjectField(TEXT("Body"))->SetObjectField(TEXT("BindingTags"), Tags);
	}, TEXT("Edit tags"));
	if (TestTrue(TEXT("tag edit applies"), Edited.IsSuccess()))
	{
		const UMovieScene* After = MovieSceneOf(Target);
		TestTrue(TEXT("Hero tags both"), Tagged(After, TEXT("Hero")) == TArray<FGuid>({Hero, Door}));
		TestFalse(TEXT("Doors removed"), After != nullptr && After->AllTaggedBindings().Contains(TEXT("Doors")));
	}

	// A document without BindingTags keeps them.
	const FAssetDocumentResult Omitted = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		Document->GetObjectField(TEXT("Body"))->RemoveField(TEXT("BindingTags"));
		Document->GetObjectField(TEXT("Body"))->SetArrayField(TEXT("Markers"), {});
	}, TEXT("Omitted tags"));
	TestTrue(TEXT("omitted tags apply"), Omitted.IsSuccess());
	TestEqual(TEXT("omitted tags are kept"), Tagged(MovieSceneOf(Target), TEXT("Hero")).Num(), 2);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelitySequencerSpawnedByTest, "UEAssetDoc.Fidelity.Sequencer.SpawnedBy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelitySequencerSpawnedByTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelitySequencerTestsLocal;
	const FTarget Target(TEXT("LS_FidelitySpawnedBy"));
	const FTarget Created(TEXT("LS_FidelitySpawnedByCreated"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); Created.Cleanup(); };

	// A shot's possessable bound to the hero the master sequence spawns ("Bind to spawnable" in the parent's context:
	// the spawnable's GUID in the parent, one level up).
	ULevelSequence* Sequence = NewFidelitySequence(Target);
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	const FGuid Hero = MovieScene->AddPossessable(TEXT("Hero"), AActor::StaticClass());
	const FGuid Spawned = FGuid::NewGuid();
	const FMovieSceneObjectBindingID Parent = UE::MovieScene::FRelativeObjectBindingID(Spawned, MovieSceneSequenceID::Root, 1);
	MovieScene->FindPossessable(Hero)->SetSpawnableObjectBindingID(Parent);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Spawned by"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("SpawnedBy names the parent's spawnable"), StringAt(Extracted, TEXT("/Body/Bindings/0/SpawnedBy/Guid")),
		Spawned.ToString(EGuidFormats::Digits));

	if (TestTrue(TEXT("create from the extract"), CreateFrom(*this, Extracted, Created).IsSuccess()))
	{
		TestTrue(TEXT("the new possessable is bound to the spawnable"), SpawnedByOf(Created, TEXT("Hero")) == Parent);
	}

	const FAssetDocumentResult Omitted = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonValue> Binding = At(Document, TEXT("/Body/Bindings/0"));
		Binding->AsObject()->RemoveField(TEXT("SpawnedBy"));
		const TArray<TSharedPtr<FJsonValue>> Tags = {MakeShared<FJsonValueString>(TEXT("Lead"))};
		Binding->AsObject()->SetArrayField(TEXT("Tags"), Tags);
	}, TEXT("Omitted SpawnedBy"));
	TestTrue(TEXT("an edit that omits SpawnedBy applies"), Omitted.IsSuccess());
	TestTrue(TEXT("omitted SpawnedBy is kept"), SpawnedByOf(Target, TEXT("Hero")) == Parent);

	const FAssetDocumentResult Cleared = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		At(Document, TEXT("/Body/Bindings/0"))->AsObject()->SetField(TEXT("SpawnedBy"), MakeShared<FJsonValueNull>());
	}, TEXT("Cleared SpawnedBy"));
	TestTrue(TEXT("SpawnedBy null applies"), Cleared.IsSuccess());
	TestFalse(TEXT("null clears SpawnedBy"), SpawnedByOf(Target, TEXT("Hero")).IsValid());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelitySequencerSpawnedByStaleTest, "UEAssetDoc.Fidelity.Sequencer.SpawnedByStale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelitySequencerSpawnedByStaleTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelitySequencerTestsLocal;
	const FTarget Target(TEXT("LS_FidelitySpawnedByStale"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// "Hero" was bound to a spawnable of this sequence that was deleted since; "Rider" is bound to "Mount".
	ULevelSequence* Sequence = NewFidelitySequence(Target);
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	const FGuid Hero = MovieScene->AddPossessable(TEXT("Hero"), AActor::StaticClass());
	const FGuid Rider = MovieScene->AddPossessable(TEXT("Rider"), AActor::StaticClass());
	const FGuid Mount = MovieScene->AddPossessable(TEXT("Mount"), AActor::StaticClass());
	const FGuid Deleted = FGuid::NewGuid();
	const FMovieSceneObjectBindingID Stale = UE::MovieScene::FRelativeObjectBindingID(Deleted);
	const FMovieSceneObjectBindingID ToMount = UE::MovieScene::FRelativeObjectBindingID(Mount);
	MovieScene->FindPossessable(Hero)->SetSpawnableObjectBindingID(Stale);
	MovieScene->FindPossessable(Rider)->SetSpawnableObjectBindingID(ToMount);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Stale spawned by"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	auto IndexOf = [](const TSharedPtr<FJsonObject>& Document, const FString& Name) -> int32
	{
		const TArray<TSharedPtr<FJsonValue>> Bindings = Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Bindings"));
		for (int32 Index = 0; Index < Bindings.Num(); ++Index)
		{
			if (Bindings[Index]->AsObject()->GetStringField(TEXT("Name")) == Name)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	};
	const int32 HeroIndex = IndexOf(Extracted, TEXT("Hero"));
	TestEqual(TEXT("the deleted spawnable reads by its Id"),
		StringAt(Extracted, FString::Printf(TEXT("/Body/Bindings/%d/SpawnedBy/Binding"), HeroIndex)), TEXT("#") + Deleted.ToString(EGuidFormats::Digits));

	auto TagHero = [&IndexOf](const TSharedPtr<FJsonObject>& Document, bool bOmitSpawnedBy)
	{
		const TSharedPtr<FJsonObject> Binding = Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Bindings"))[IndexOf(Document, TEXT("Hero"))]->AsObject();
		if (bOmitSpawnedBy)
		{
			Binding->RemoveField(TEXT("SpawnedBy"));
		}
		Binding->SetArrayField(TEXT("Tags"), {MakeShared<FJsonValueString>(bOmitSpawnedBy ? TEXT("Lead") : TEXT("Star"))});
	};
	const FAssetDocumentResult AsExtracted = ApplyEdited(*this, Target, [&TagHero](const TSharedPtr<FJsonObject>& Document)
	{
		TagHero(Document, false);
	}, TEXT("Tags with the stale SpawnedBy"));
	TestTrue(TEXT("a Tags edit with SpawnedBy as extracted applies"), AsExtracted.IsSuccess());
	TestTrue(TEXT("the stale reference is kept"), SpawnedByOf(Target, TEXT("Hero")) == Stale);
	const FAssetDocumentResult Omitted = ApplyEdited(*this, Target, [&TagHero](const TSharedPtr<FJsonObject>& Document)
	{
		TagHero(Document, true);
	}, TEXT("Tags without SpawnedBy"));
	TestTrue(TEXT("a Tags edit that omits SpawnedBy applies"), Omitted.IsSuccess());
	TestTrue(TEXT("the stale reference is still kept"), SpawnedByOf(Target, TEXT("Hero")) == Stale);
	const UMovieScene* After = MovieSceneOf(Target);
	const FMovieScenePossessable* HeroAfter = After != nullptr ? const_cast<UMovieScene*>(After)->FindPossessable(Hero) : nullptr;
	TestTrue(TEXT("the tag is written"), HeroAfter != nullptr && HeroAfter->Tags == TArray<FName>({FName(TEXT("Lead"))}));

	// Mount renamed to Horse in the document that tags Rider and leaves its SpawnedBy out.
	const FAssetDocumentResult Renamed = ApplyEdited(*this, Target, [&IndexOf](const TSharedPtr<FJsonObject>& Document)
	{
		const TArray<TSharedPtr<FJsonValue>> Bindings = Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Bindings"));
		const TSharedPtr<FJsonObject> MountBinding = Bindings[IndexOf(Document, TEXT("Mount"))]->AsObject();
		MountBinding->SetStringField(TEXT("Name"), TEXT("Horse"));
		MountBinding->SetStringField(TEXT("RenamedFrom"), TEXT("Mount"));
		const TSharedPtr<FJsonObject> RiderBinding = Bindings[IndexOf(Document, TEXT("Rider"))]->AsObject();
		RiderBinding->RemoveField(TEXT("SpawnedBy"));
		RiderBinding->SetArrayField(TEXT("Tags"), {MakeShared<FJsonValueString>(TEXT("Rider"))});
	}, TEXT("Rename the spawnable"));
	TestTrue(TEXT("the rename and the Tags edit apply"), Renamed.IsSuccess());
	TestTrue(TEXT("Rider keeps its reference to the renamed binding"), SpawnedByOf(Target, TEXT("Rider")) == ToMount);
	TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Stale spawned by after edits"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelitySequencerMetaDataObjectsTest, "UEAssetDoc.Fidelity.Sequencer.MetaDataObjects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelitySequencerMetaDataObjectsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelitySequencerTestsLocal;
	const FTarget Target(TEXT("LS_FidelityMetaData"));
	const FTarget Created(TEXT("LS_FidelityMetaDataCreated"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); Created.Cleanup(); };

	// Notes, and the shot metadata the Sequencer's shot browser rates and flags.
	ULevelSequence* Sequence = NewFidelitySequence(Target);
	Sequence->FindOrAddMetaData<UMovieSceneMetaData>()->SetNotes(TEXT("Take two"));
	UMovieSceneShotMetaData* Shot = Sequence->FindOrAddMetaData<UMovieSceneShotMetaData>();
	Shot->SetFavoriteRating(4);
	Shot->SetIsFlagged(true);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Metadata objects"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("shot metadata class read"), StringAt(Extracted, TEXT("/Body/MetaData/Objects/0/Class")),
		UMovieSceneShotMetaData::StaticClass()->GetPathName());
	const TSharedPtr<FJsonValue> Rating = At(Extracted, TEXT("/Body/MetaData/Objects/0/Properties/FavoriteRating"));
	double RatingValue = 0.0;
	TestTrue(TEXT("rating read"), Rating.IsValid() && Rating->TryGetNumber(RatingValue) && RatingValue == 4.0);

	if (TestTrue(TEXT("create from the extract"), CreateFrom(*this, Extracted, Created).IsSuccess()))
	{
		const UMovieSceneShotMetaData* New = ShotMetaDataOf(Created);
		TestTrue(TEXT("the new sequence has the shot metadata"), New != nullptr && New->GetFavoriteRating() == TOptional<int32>(4)
			&& New->GetIsFlagged() == TOptional<bool>(true));
	}

	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		At(Document, TEXT("/Body/MetaData/Objects/0/Properties"))->AsObject()->SetNumberField(TEXT("FavoriteRating"), 2);
	}, TEXT("Edit rating"));
	TestTrue(TEXT("rating edit applies"), Edited.IsSuccess());
	const UMovieSceneShotMetaData* AfterEdit = ShotMetaDataOf(Target);
	TestTrue(TEXT("rating written, flag kept"), AfterEdit != nullptr && AfterEdit->GetFavoriteRating() == TOptional<int32>(2)
		&& AfterEdit->GetIsFlagged() == TOptional<bool>(true));

	const FAssetDocumentResult Omitted = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		At(Document, TEXT("/Body/MetaData"))->AsObject()->RemoveField(TEXT("Objects"));
		At(Document, TEXT("/Body/MetaData"))->AsObject()->SetStringField(TEXT("Notes"), TEXT("Take three"));
	}, TEXT("Omitted objects"));
	TestTrue(TEXT("an edit that omits Objects applies"), Omitted.IsSuccess());
	TestNotNull(TEXT("omitted Objects are kept"), ShotMetaDataOf(Target));

	const FAssetDocumentResult Removed = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		At(Document, TEXT("/Body/MetaData"))->AsObject()->SetArrayField(TEXT("Objects"), {});
	}, TEXT("Removed objects"));
	TestTrue(TEXT("an empty Objects applies"), Removed.IsSuccess());
	TestNull(TEXT("an empty Objects removes the shot metadata"), ShotMetaDataOf(Target));
	const ULevelSequence* After = Target.Get<ULevelSequence>();
	const UMovieSceneMetaData* Notes = After != nullptr ? After->FindMetaData<UMovieSceneMetaData>() : nullptr;
	TestTrue(TEXT("Notes are MetaData's own"), Notes != nullptr && Notes->GetNotes() == TEXT("Take three"));
	return !HasAnyErrors();
}

#endif
