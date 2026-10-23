// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// UEAssetDoc.Sequencer.Read: a Level Sequence built with the editor's calls (spawnable camera with a
//   component binding, level possessable, folder, every key interpolation, a sub-frame key, a dense channel, camera
//   cut, fade, sub track, marker, metadata): Extract / Inspect / Diff through the Python Profile, stable twice and
//   after reload, no side effects, the document shape.
// UEAssetDoc.Sequencer.ReadOnly: an unchanged document applies as a no-op; a Director region with a
//   key a Director Blueprint does not have (Components) is refused at its path before any change.
// UEAssetDoc.Sequencer.Status: the read status checks (design §5.1) at document paths.
// UEAssetDoc.Sequencer.ListTypes: list_types kinds sequencer_track / sequencer_section / sequencer_binding.

#include "AssetDocumentSequencerTestSupport.h"

#include "Bindings/MovieSceneSpawnableActorBinding.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Engine/StaticMeshActor.h"
#include "MovieScene.h"
#include "MovieSceneBindingReferences.h"
#include "MovieSceneFolder.h"
#include "MovieSceneMetaData.h"
#include "MovieScenePossessable.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneFadeTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneSubTrack.h"
#include "UniversalObjectLocator.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentSequencerTests
{
FFrameNumber Frame(int32 DisplayFrame)
{
	return FFrameNumber(DisplayFrame * TicksPerFrame);
}

TRange<FFrameNumber> Frames(int32 Start, int32 End)
{
	return TRange<FFrameNumber>(Frame(Start), Frame(End));
}

ULevelSequence* NewSequence(const FTarget& Target)
{
	UPackage* Package = CreatePackage(*Target.PackageName);
	ULevelSequence* Sequence = NewObject<ULevelSequence>(Package, *Target.AssetName, RF_Public | RF_Standalone | RF_Transactional);
	Sequence->Initialize();
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	MovieScene->SetDisplayRate(FFrameRate(30, 1));
	MovieScene->SetTickResolutionDirectly(FFrameRate(24000, 1));
	MovieScene->SetPlaybackRange(Frames(0, 150));
	FAssetRegistryModule::AssetCreated(Sequence);
	return Sequence;
}

UMovieSceneSection* AddSection(UMovieSceneTrack& Track, int32 Start, int32 End)
{
	UMovieSceneSection* Section = Track.CreateNewSection();
	Track.AddSection(*Section);
	Section->SetRange(Frames(Start, End));
	return Section;
}

// The fixture sequence (saved), with an inner sequence for its sub track (saved first).
ULevelSequence* BuildFixture(FAutomationTestBase& Test, const FTarget& Target, const FTarget& InnerTarget, FBuilt& Out)
{
	ULevelSequence* Inner = NewSequence(InnerTarget);
	if (!Test.TestTrue(TEXT("inner sequence saved"), Save(*Inner)))
	{
		return nullptr;
	}
	ULevelSequence* Sequence = NewSequence(Target);
	UMovieSceneSequence* Base = Sequence;
	UMovieScene* MovieScene = Sequence->GetMovieScene();

	// Spawnable camera (the editor's "+ Track > Cine Camera Actor" as a spawnable: custom binding + spawn track).
	Out.Camera = Base->CreateSpawnable(ACineCameraActor::StaticClass());
	if (!Test.TestTrue(TEXT("spawnable created"), Out.Camera.IsValid()))
	{
		return nullptr;
	}
	MovieScene->FindPossessable(Out.Camera)->SetName(TEXT("ShotCam"));
	MovieScene->SetObjectDisplayName(Out.Camera, FText::FromString(TEXT("ShotCam")));
	ACineCameraActor* Template = nullptr;
	for (const FMovieSceneBindingReference& Reference : Base->GetBindingReferences()->GetReferences(Out.Camera))
	{
		if (UMovieSceneSpawnableActorBinding* Spawnable = Cast<UMovieSceneSpawnableActorBinding>(Reference.CustomBinding))
		{
			Template = Cast<ACineCameraActor>(Spawnable->GetObjectTemplate());
		}
	}
	if (!Test.TestNotNull(TEXT("spawnable template"), Template))
	{
		return nullptr;
	}
	// Its camera component, bound relative to the spawned actor.
	Out.Component = MovieScene->AddPossessable(TEXT("CameraComponent"), UCineCameraComponent::StaticClass());
	MovieScene->FindPossessable(Out.Component)->SetParent(Out.Camera, MovieScene);
	Base->GetBindingReferences()->AddBinding(Out.Component, FUniversalObjectLocator(Template->GetCineCameraComponent(), Template));

	// A level actor possessable: the locator is data; the actor need not exist afterwards.
	UWorld* World = GEditor->GetEditorWorldContext().World();
	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>();
	Out.Door = MovieScene->AddPossessable(TEXT("Door"), AStaticMeshActor::StaticClass());
	Base->GetBindingReferences()->AddBinding(Out.Door, FUniversalObjectLocator(Actor, World));
	World->EditorDestroyActor(Actor, false);

	UMovieSceneFolder* Folder = NewObject<UMovieSceneFolder>(MovieScene, NAME_None, RF_Transactional);
	Folder->SetFolderName(TEXT("Cameras"));
	MovieScene->AddRootFolder(Folder);
	Folder->AddChildObjectBinding(Out.Camera);

	// Transform keys with every interpolation of the key menu, weighted tangents and a sub-frame key; a default.
	UMovieScene3DTransformTrack* Transform = MovieScene->AddTrack<UMovieScene3DTransformTrack>(Out.Camera);
	UMovieSceneSection* TransformSection = AddSection(*Transform, 0, 150);
	FMovieSceneChannelProxy& TransformChannels = TransformSection->GetChannelProxy();
	FMovieSceneDoubleChannel* X = TransformChannels.GetChannel<FMovieSceneDoubleChannel>(0);
	X->AddCubicKey(Frame(0), -600.0, RCTM_SmartAuto);
	X->AddCubicKey(Frame(30), -500.0, RCTM_Auto);
	FMovieSceneTangentData User;
	User.ArriveTangent = 0.5f;
	User.LeaveTangent = 0.5f;
	X->AddCubicKey(Frame(60), -400.0, RCTM_User, User);
	FMovieSceneTangentData Break;
	Break.ArriveTangent = 0.25f;
	Break.LeaveTangent = -0.75f;
	Break.TangentWeightMode = RCTWM_WeightedBoth;
	Break.ArriveTangentWeight = 0.5f;
	Break.LeaveTangentWeight = 1.5f;
	X->AddCubicKey(Frame(90), -300.0, RCTM_Break, Break);
	X->AddLinearKey(FFrameNumber(100 * TicksPerFrame + TicksPerFrame / 2), -280.0);
	X->AddConstantKey(Frame(120), -250.0);
	TransformChannels.GetChannel<FMovieSceneDoubleChannel>(2)->SetDefault(160.0);

	// A dense channel (more than 64 keys: the columnar form).
	UMovieSceneFloatTrack* Focal = MovieScene->AddTrack<UMovieSceneFloatTrack>(Out.Component);
	Focal->SetPropertyNameAndPath(TEXT("CurrentFocalLength"), TEXT("CurrentFocalLength"));
	FMovieSceneFloatChannel* FocalValue = AddSection(*Focal, 0, 150)->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0);
	for (int32 Index = 0; Index < 100; ++Index)
	{
		FocalValue->AddLinearKey(Frame(Index), 24.0f + static_cast<float>(Index) * 0.25f);
	}

	UMovieSceneCameraCutTrack* Cut = Cast<UMovieSceneCameraCutTrack>(MovieScene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass()));
	UMovieSceneCameraCutSection* CutSection = Cut->AddNewCameraCut(UE::MovieScene::FRelativeObjectBindingID(Out.Camera), Frame(0));
	CutSection->SetRange(Frames(0, 150));

	UMovieSceneFadeTrack* Fade = MovieScene->AddTrack<UMovieSceneFadeTrack>();
	FMovieSceneFloatChannel* FadeValue = AddSection(*Fade, 0, 150)->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0);
	FadeValue->AddLinearKey(Frame(0), 1.0f);
	FadeValue->AddLinearKey(Frame(15), 0.0f);

	UMovieSceneSubTrack* Sub = MovieScene->AddTrack<UMovieSceneSubTrack>();
	Sub->AddSequence(Inner, Frame(0), 60 * TicksPerFrame);

	FMovieSceneMarkedFrame Marker(Frame(60));
	Marker.Label = TEXT("Open");
	MovieScene->AddMarkedFrame(Marker);
	if (UMovieSceneMetaData* MetaData = Cast<UMovieSceneMetaData>(Sequence->FindOrAddMetaDataByClass(UMovieSceneMetaData::StaticClass())))
	{
		MetaData->SetNotes(TEXT("Door reveal"));
	}
	Sequence->MarkPackageDirty();
	Test.TestTrue(TEXT("fixture saved"), Save(*Sequence));
	return Target.GetSequence();
}

// Extract twice, Diff, Inspect, reload, extract again: the problems found (empty = stable).
TArray<FString> CheckStable(FAutomationTestBase& Test, const FString& ObjectPath, const FString& Name, FString& OutTimings,
	TSharedPtr<FJsonObject>& OutExtracted, FAssetDocumentResult& OutInspected)
{
	TArray<FString> Problems;
	const FReadSideEffects SideEffects;
	double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult First = ExtractResult(ObjectPath);
	const double ExtractMs = Milliseconds(Start);
	if (!First.IsSuccess() || !First.Payload.IsValid())
	{
		Problems.Add(FString::Printf(TEXT("extract failed: %s"), *DiagnosticsText(First)));
		return Problems;
	}
	OutExtracted = First.Payload;
	const FString Baseline = Serialize(First.Payload);
	const FString Again = Serialize(Extract(Test, ObjectPath, Name + TEXT(" again")));
	if (Again != Baseline)
	{
		Problems.Add(FString::Printf(TEXT("second extraction differs %s"), *FirstDifference(Baseline, Again)));
	}
	Start = FPlatformTime::Seconds();
	OutInspected = Inspect(ObjectPath);
	const double InspectMs = Milliseconds(Start);
	if (!OutInspected.IsSuccess())
	{
		Problems.Add(FString::Printf(TEXT("inspect failed: %s"), *DiagnosticsText(OutInspected)));
	}
	for (const TSharedPtr<FJsonObject>& Warning : ReadWarnings(OutInspected))
	{
		if (!IsKnownReadCode(Warning->GetStringField(TEXT("Code"))))
		{
			Problems.Add(FString::Printf(TEXT("unknown read diagnostic %s"), *Serialize(Warning)));
		}
	}
	for (const FAssetDocumentDiagnostic& Diagnostic : First.Diagnostics)
	{
		Problems.Add(FString::Printf(TEXT("extract diagnostic [%s] %s %s"), *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message));
	}
	// Diff takes a writable target: engine / plugin content (outside /Game) is refused with Meta.ReadOnlyTarget.
	if (ObjectPath.StartsWith(TEXT("/Game/")))
	{
		FString DiffText;
		const int32 Changes = DiffCount(First.Payload, DiffText);
		if (Changes != 0)
		{
			Problems.Add(FString::Printf(TEXT("Diff of the extraction reports %d changes: %s"), Changes, *DiffText.Left(600)));
		}
	}
	SideEffects.Check(Problems);
	FString Status = StatusName(First);
	const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
	if (First.Compile.IsValid() && First.Compile->TryGetArrayField(TEXT("Messages"), Messages))
	{
		Status += FString::Printf(TEXT(" (%d messages)"), Messages->Num());
	}
	if (Reload(ObjectPath) == nullptr)
	{
		Problems.Add(TEXT("reload failed"));
	}
	else
	{
		const FString Reloaded = Serialize(Extract(Test, ObjectPath, Name + TEXT(" after reload")));
		if (Reloaded != Baseline)
		{
			Problems.Add(FString::Printf(TEXT("extraction after reload differs %s"), *FirstDifference(Baseline, Reloaded)));
		}
	}
	OutTimings = FString::Printf(TEXT("extract %.0f ms, inspect %.0f ms, %d chars, status %s"), ExtractMs, InspectMs, Baseline.Len(), *Status);
	return Problems;
}

TSharedPtr<FJsonObject> Child(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject>* Found = nullptr;
	return Object.IsValid() && Object->TryGetObjectField(Field, Found) ? *Found : nullptr;
}

TArray<TSharedPtr<FJsonObject>> Items(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	TArray<TSharedPtr<FJsonObject>> Out;
	const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
	if (Object.IsValid() && Object->TryGetArrayField(Field, List))
	{
		for (const TSharedPtr<FJsonValue>& Value : *List)
		{
			Out.Add(Value->AsObject());
		}
	}
	return Out;
}

TSharedPtr<FJsonObject> Named(const TArray<TSharedPtr<FJsonObject>>& List, const TCHAR* Field, const TCHAR* Value)
{
	for (const TSharedPtr<FJsonObject>& Item : List)
	{
		FString Text;
		if (Item.IsValid() && Item->TryGetStringField(Field, Text) && Text == Value)
		{
			return Item;
		}
	}
	return nullptr;
}

// The first section's channels of the track of that type in the list.
TSharedPtr<FJsonObject> ChannelsOf(const TArray<TSharedPtr<FJsonObject>>& Tracks, const TCHAR* Type)
{
	const TArray<TSharedPtr<FJsonObject>> Sections = Items(Named(Tracks, TEXT("Type"), Type), TEXT("Sections"));
	return Sections.IsEmpty() ? nullptr : Child(Sections[0], TEXT("Channels"));
}
} // namespace AssetDocumentSequencerTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerReadTest, "UEAssetDoc.Sequencer.Read",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerReadTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerTests;
	FFixtures Fixtures;
	const FTarget& Inner = Fixtures.Add(TEXT("LS_Inner"));
	const FTarget& Target = Fixtures.Add(TEXT("LS_Read"));
	FBuilt Built;
	if (BuildFixture(*this, Target, Inner, Built) == nullptr)
	{
		return false;
	}
	// Read the saved state: a freshly created spawnable template has not been through PostLoad, which recomputes derived
	// component fields (a cine camera's FieldOfView / AspectRatio), so the first load reads more than the new object.
	if (!TestNotNull(TEXT("fixture reloads"), Reload(Target.ObjectPath)))
	{
		return false;
	}
	FString Timings;
	TSharedPtr<FJsonObject> Extracted;
	FAssetDocumentResult Inspected;
	for (const FString& Problem : CheckStable(*this, Target.ObjectPath, TEXT("fixture"), Timings, Extracted, Inspected))
	{
		AddError(Problem);
	}
	AddInfo(FString::Printf(TEXT("fixture: %s"), *Timings));
	if (!Extracted.IsValid())
	{
		return false;
	}
	AddInfo(SerializePretty(Extracted).Left(8000));
	TestEqual(TEXT("Class"), Extracted->GetStringField(TEXT("Class")), FString(LevelSequenceClass));
	const TSharedPtr<FJsonObject> Body = BodyOf(Extracted);
	const TSharedPtr<FJsonObject> Settings = Child(Body, TEXT("Settings"));
	const TSharedPtr<FJsonObject> Rate = Child(Settings, TEXT("DisplayRate"));
	TestTrue(TEXT("display rate 30/1"), Rate.IsValid() && Rate->GetIntegerField(TEXT("Numerator")) == 30 && Rate->GetIntegerField(TEXT("Denominator")) == 1);
	const TSharedPtr<FJsonObject> Range = Child(Settings, TEXT("PlaybackRange"));
	TestTrue(TEXT("playback range in display frames"), Range.IsValid() && Range->GetNumberField(TEXT("Start")) == 0.0
		&& Range->GetNumberField(TEXT("End")) == 150.0);

	const TArray<TSharedPtr<FJsonObject>> Bindings = Items(Body, TEXT("Bindings"));
	TestEqual(TEXT("two root bindings"), Bindings.Num(), 2);
	const TSharedPtr<FJsonObject> Camera = Named(Bindings, TEXT("Name"), TEXT("ShotCam"));
	if (TestNotNull(TEXT("ShotCam binding"), Camera.Get()))
	{
		TestEqual(TEXT("spawnable class"), Child(Camera, TEXT("Spawnable")).IsValid() ? Child(Camera, TEXT("Spawnable"))->GetStringField(TEXT("Class")) : FString(),
			FString(TEXT("/Script/CinematicCamera.CineCameraActor")));
		TestEqual(TEXT("camera folder"), Camera->GetStringField(TEXT("Folder")), FString(TEXT("Cameras")));
		const TArray<TSharedPtr<FJsonObject>> Tracks = Items(Camera, TEXT("Tracks"));
		TestNotNull(TEXT("spawn track"), Named(Tracks, TEXT("Type"), TEXT("Spawn")).Get());
		const TSharedPtr<FJsonObject> Channels = ChannelsOf(Tracks, TEXT("Transform"));
		const TArray<TSharedPtr<FJsonObject>> Keys = Items(Child(Channels, TEXT("Location.X")), TEXT("Keys"));
		if (TestEqual(TEXT("Location.X keys"), Keys.Num(), 6))
		{
			TestFalse(TEXT("SmartAuto is the omitted default"), Keys[0]->HasField(TEXT("Interp")));
			TestEqual(TEXT("Auto"), Keys[1]->GetStringField(TEXT("Interp")), FString(TEXT("Auto")));
			TestEqual(TEXT("User"), Keys[2]->GetStringField(TEXT("Interp")), FString(TEXT("User")));
			TestTrue(TEXT("User keys carry tangents"), Keys[2]->HasField(TEXT("Arrive")) && Keys[2]->HasField(TEXT("Leave")));
			TestFalse(TEXT("Auto keys carry no tangents"), Keys[1]->HasField(TEXT("Arrive")));
			TestEqual(TEXT("Break"), Keys[3]->GetStringField(TEXT("Interp")), FString(TEXT("Break")));
			TestTrue(TEXT("weighted tangents"), Keys[3]->HasField(TEXT("ArriveWeight")) && Keys[3]->HasField(TEXT("WeightMode")));
			TestEqual(TEXT("sub-frame key"), Keys[4]->GetNumberField(TEXT("Frame")), 100.5);
			TestEqual(TEXT("Linear"), Keys[4]->GetStringField(TEXT("Interp")), FString(TEXT("Linear")));
			TestEqual(TEXT("Constant"), Keys[5]->GetStringField(TEXT("Interp")), FString(TEXT("Constant")));
		}
		TestEqual(TEXT("Location.Z default"), Child(Channels, TEXT("Location.Z")).IsValid() ? Child(Channels, TEXT("Location.Z"))->GetNumberField(TEXT("Default")) : 0.0, 160.0);
		TestEqual(TEXT("Scale.X keeps its default"), Child(Channels, TEXT("Scale.X")).IsValid() ? Child(Channels, TEXT("Scale.X"))->GetNumberField(TEXT("Default")) : 0.0, 1.0);
		const TArray<TSharedPtr<FJsonObject>> Children = Items(Camera, TEXT("Children"));
		if (TestEqual(TEXT("component binding"), Children.Num(), 1))
		{
			TestEqual(TEXT("component name"), Children[0]->GetStringField(TEXT("Name")), FString(TEXT("CameraComponent")));
			const TArray<TSharedPtr<FJsonValue>>* Bind = nullptr;
			TestTrue(TEXT("component locator"), Children[0]->TryGetArrayField(TEXT("Bind"), Bind) && Bind->Num() == 1
				&& (*Bind)[0]->AsString().StartsWith(TEXT("uobj://")));
			const TSharedPtr<FJsonObject> Focal = Named(Items(Children[0], TEXT("Tracks")), TEXT("Property"), TEXT("CurrentFocalLength"));
			const TArray<TSharedPtr<FJsonObject>> FocalSections = Items(Focal, TEXT("Sections"));
			const TSharedPtr<FJsonObject> Dense = FocalSections.IsEmpty() ? nullptr : Child(Child(FocalSections[0], TEXT("Channels")), TEXT("Value"));
			if (TestNotNull(TEXT("dense channel"), Dense.Get()))
			{
				TestEqual(TEXT("columnar frames"), CountField(Dense, TEXT("Frames")), 100);
				TestEqual(TEXT("columnar values"), CountField(Dense, TEXT("Values")), 100);
				TestEqual(TEXT("uniform Interp collapses to one token"), Dense->GetStringField(TEXT("Interp")), FString(TEXT("Linear")));
			}
		}
	}
	const TSharedPtr<FJsonObject> Door = Named(Bindings, TEXT("Name"), TEXT("Door"));
	if (TestNotNull(TEXT("Door binding"), Door.Get()))
	{
		TestEqual(TEXT("possessed class"), Door->GetStringField(TEXT("Class")), FString(TEXT("/Script/Engine.StaticMeshActor")));
		const TArray<TSharedPtr<FJsonValue>>* Bind = nullptr;
		TestTrue(TEXT("level locator"), Door->TryGetArrayField(TEXT("Bind"), Bind) && Bind->Num() == 1 && (*Bind)[0]->AsString().StartsWith(TEXT("uobj://")));
	}
	const TArray<TSharedPtr<FJsonObject>> Tracks = Items(Body, TEXT("Tracks"));
	if (TestTrue(TEXT("global tracks"), Tracks.Num() == 3))
	{
		TestEqual(TEXT("camera cut first"), Tracks[0]->GetStringField(TEXT("Type")), FString(TEXT("CameraCut")));
		const TArray<TSharedPtr<FJsonObject>> Cuts = Items(Tracks[0], TEXT("Sections"));
		TestTrue(TEXT("camera cut names the binding path"), !Cuts.IsEmpty()
			&& Serialize(Child(Cuts[0], TEXT("Properties"))).Contains(TEXT("\"CameraBindingID\":{\"Binding\":\"ShotCam\"}")));
		TestNotNull(TEXT("fade"), ChannelsOf(Tracks, TEXT("Fade")).Get());
		const TArray<TSharedPtr<FJsonObject>> Subs = Items(Named(Tracks, TEXT("Type"), TEXT("Subsequence")), TEXT("Sections"));
		TestTrue(TEXT("subsequence names the inner sequence"), !Subs.IsEmpty() && Serialize(Child(Subs[0], TEXT("Properties"))).Contains(Inner.ObjectPath));
	}
	const TArray<TSharedPtr<FJsonObject>> Markers = Items(Body, TEXT("Markers"));
	TestTrue(TEXT("marker Open at 60"), Markers.Num() == 1 && Markers[0]->Values.Num() == 2 && Markers[0]->GetNumberField(TEXT("Frame")) == 60.0
		&& Markers[0]->GetStringField(TEXT("Label")) == TEXT("Open"));
	TestEqual(TEXT("folder"), Items(Body, TEXT("Folders")).Num() == 1 ? Items(Body, TEXT("Folders"))[0]->GetStringField(TEXT("Path")) : FString(),
		FString(TEXT("Cameras")));
	TestEqual(TEXT("metadata notes"), Child(Body, TEXT("MetaData")).IsValid() ? Child(Body, TEXT("MetaData"))->GetStringField(TEXT("Notes")) : FString(),
		FString(TEXT("Door reveal")));
	TestFalse(TEXT("no director"), Body->HasField(TEXT("Director")));

	// Inspect: binding facts, animatable properties, track classes, status.
	if (TestTrue(TEXT("inspect"), Inspected.IsSuccess() && Inspected.Payload.IsValid()))
	{
		const TArray<TSharedPtr<FJsonObject>> Facts = Items(Inspected.Payload, TEXT("BindingFacts"));
		TestEqual(TEXT("binding facts"), Facts.Num(), 3);
		const TSharedPtr<FJsonObject> CameraFacts = Named(Facts, TEXT("Path"), TEXT("ShotCam"));
		TestTrue(TEXT("camera kind Spawnable"), CameraFacts.IsValid() && CameraFacts->GetStringField(TEXT("Kind")) == TEXT("Spawnable"));
		const TSharedPtr<FJsonObject> Animatable = Child(Inspected.Payload, TEXT("AnimatableProperties"));
		TestTrue(TEXT("animatable properties of the camera component"), Animatable.IsValid()
			&& Serialize(Animatable).Contains(TEXT("CurrentFocalLength")));
		TestTrue(TEXT("track classes"), CountField(Inspected.Payload, TEXT("TrackClasses")) >= 5);
		TestTrue(TEXT("references list the inner sequence"), Serialize(Inspected.Payload).Contains(Inner.PackageName));
		TestEqual(TEXT("no read warnings"), ReadWarnings(Inspected).Num(), 0);
		AddInfo(FString::Printf(TEXT("status: %s"), *Serialize(Child(Inspected.Payload, TEXT("Status")))));
	}
	const FAssetDocumentResult Status = ExtractResult(Target.ObjectPath);
	TestTrue(*FString::Printf(TEXT("fixture is up to date (%s)"), *StatusOf(Status)), StatusName(Status).StartsWith(TEXT("UpToDate")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerReadOnlyTest, "UEAssetDoc.Sequencer.ReadOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerReadOnlyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerTests;
	FFixtures Fixtures;
	const FTarget& Inner = Fixtures.Add(TEXT("LS_Inner"));
	const FTarget& Target = Fixtures.Add(TEXT("LS_ReadOnly"));
	FBuilt Built;
	if (BuildFixture(*this, Target, Inner, Built) == nullptr)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Extracted = Extract(*this, Target.ObjectPath, TEXT("before"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const FString Before = Serialize(Extracted);

	// The unchanged document applies as a no-op.
	const FAssetDocumentResult Same = Apply(Target, Extracted);
	TestTrue(*FString::Printf(TEXT("unchanged document applies (%s)"), *DiagnosticsText(Same)), Same.IsSuccess());
	TestFalse(TEXT("unchanged document saves nothing"), Same.bSavedAsset);

	// A Director has no components: refused at /Body/Director/Components, before any change, together with an otherwise
	// valid edit.
	const TSharedPtr<FJsonObject> Changed = Parse(Before);
	BodyOf(Changed)->GetArrayField(TEXT("Markers"))[0]->AsObject()->SetStringField(TEXT("Label"), TEXT("Close"));
	BodyOf(Changed)->SetObjectField(TEXT("Director"), Parse(TEXT(R"({"Components": []})")));
	const FAssetDocumentResult Refused = Apply(Target, Changed);
	TestFalse(TEXT("a Director with components is refused"), Refused.IsSuccess());
	TestTrue(*FString::Printf(TEXT("Sequencer.InvalidBody (%s)"), *DiagnosticsText(Refused)), HasCode(Refused, TEXT("Sequencer.InvalidBody")));
	TestEqual(TEXT("at /Body/Director/Components"), FirstPath(Refused, TEXT("Sequencer.InvalidBody")), FString(TEXT("/Body/Director/Components")));

	const TSharedPtr<FJsonObject> After = Extract(*this, Target.ObjectPath, TEXT("after"));
	TestEqual(TEXT("the sequence is unchanged"), Serialize(After), Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerStatusTest, "UEAssetDoc.Sequencer.Status",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerStatusTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerTests;
	FFixtures Fixtures;
	const FTarget& Inner = Fixtures.Add(TEXT("LS_Inner"));
	const FTarget& Target = Fixtures.Add(TEXT("LS_Status"));
	FBuilt Built;
	ULevelSequence* Sequence = BuildFixture(*this, Target, Inner, Built);
	if (Sequence == nullptr)
	{
		return false;
	}
	// Broken states (made with the engine calls; nothing is saved).
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(MovieScene->GetCameraCutTrack()->GetAllSections()[0]);
	Cut->SetCameraBindingID(UE::MovieScene::FRelativeObjectBindingID(FGuid::NewGuid()));
	MovieScene->AddPossessable(TEXT("Ghost"), AStaticMeshActor::StaticClass());
	UMovieSceneFloatTrack* Unknown = MovieScene->AddTrack<UMovieSceneFloatTrack>(Built.Door);
	Unknown->SetPropertyNameAndPath(TEXT("NoSuchProperty"), TEXT("NoSuchProperty"));
	ULevelSequence* InnerSequence = Inner.GetSequence();
	InnerSequence->GetMovieScene()->AddTrack<UMovieSceneSubTrack>()->AddSequence(Sequence, Frame(0), 30 * TicksPerFrame);

	const FAssetDocumentResult Broken = ExtractResult(Target.ObjectPath);
	AddInfo(StatusOf(Broken));
	TestEqual(TEXT("broken: Error"), StatusName(Broken), FString(TEXT("Error")));
	const TArray<FString> Errors = StatusPaths(Broken, TEXT("Error"));
	const TArray<FString> Warnings = StatusPaths(Broken, TEXT("Warning"));
	TestTrue(TEXT("camera cut to a missing binding at the cut's Properties"), Errors.Contains(TEXT("/Body/Tracks/0/Sections/0/Properties")));
	TestTrue(TEXT("sub-sequence cycle at SubSequence"), Errors.ContainsByPredicate([](const FString& Path) { return Path.EndsWith(TEXT("/Properties/SubSequence")); }));
	TestTrue(TEXT("unknown property at the track's Property"), Errors.ContainsByPredicate([](const FString& Path)
		{ return Path.StartsWith(TEXT("/Body/Bindings/")) && Path.EndsWith(TEXT("/Property")); }));
	TestTrue(TEXT("unbound possessable warned at its Bind"), Warnings.ContainsByPredicate([](const FString& Path) { return Path.EndsWith(TEXT("/Bind")); }));

	Sequence->GetOutermost()->ClearDirtyFlag();
	InnerSequence->GetOutermost()->ClearDirtyFlag();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerListTypesTest, "UEAssetDoc.Sequencer.ListTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerListTypesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerTests;
	const FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	auto List = [&](const TMap<FString, FString>& Query, const TCHAR* Stage) -> TArray<TSharedPtr<FJsonObject>>
	{
		const FAssetDocumentResult Result = Service.ListTypes(Query);
		TArray<TSharedPtr<FJsonObject>> Rows = Items(Result.Payload, TEXT("Types"));
		if (!TestTrue(*FString::Printf(TEXT("%s: lists"), Stage), Result.IsSuccess() && !Rows.IsEmpty()))
		{
			AddInfo(DiagnosticsText(Result));
		}
		return Rows;
	};
	const TArray<TSharedPtr<FJsonObject>> TrackRows = List({{TEXT("kind"), TEXT("sequencer_track")}, {TEXT("limit"), TEXT("500")}}, TEXT("tracks"));
	const TSharedPtr<FJsonObject> Transform = Named(TrackRows, TEXT("Type"), TEXT("Transform"));
	if (TestNotNull(TEXT("Transform track listed"), Transform.Get()))
	{
		AddInfo(Serialize(Transform));
		TestEqual(TEXT("Transform path"), Transform->GetStringField(TEXT("Path")), FString(TEXT("/Script/MovieSceneTracks.MovieScene3DTransformTrack")));
	}
	TestNotNull(TEXT("CameraCut listed"), Named(TrackRows, TEXT("Type"), TEXT("CameraCut")).Get());
	TestNotNull(TEXT("Shot listed"), Named(TrackRows, TEXT("Type"), TEXT("Shot")).Get());

	const TArray<TSharedPtr<FJsonObject>> SectionRows = List({{TEXT("kind"), TEXT("sequencer_section")}, {TEXT("name"), TEXT("Transform")}}, TEXT("sections"));
	const TSharedPtr<FJsonObject> TransformSection = Named(SectionRows, TEXT("Path"), TEXT("/Script/MovieSceneTracks.MovieScene3DTransformSection"));
	if (TestNotNull(TEXT("transform section listed"), TransformSection.Get()))
	{
		AddInfo(Serialize(TransformSection));
		TestTrue(TEXT("transform section channels"), Serialize(TransformSection).Contains(TEXT("Location.X")));
	}
	const TArray<TSharedPtr<FJsonObject>> BindingRows = List({{TEXT("kind"), TEXT("sequencer_binding")}}, TEXT("bindings"));
	const TSharedPtr<FJsonObject> Spawnable = Named(BindingRows, TEXT("Path"), TEXT("/Script/MovieSceneTracks.MovieSceneSpawnableActorBinding"));
	if (TestNotNull(TEXT("spawnable actor binding listed"), Spawnable.Get()))
	{
		AddInfo(Serialize(Spawnable));
	}
	return true;
}

#endif
