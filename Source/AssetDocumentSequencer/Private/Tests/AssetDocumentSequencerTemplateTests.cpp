// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Template / Camera Animation Sequences (design §7.2, phase S4):
// UEAssetDoc.Sequencer.Template.RoundTrip: a Template Sequence and a Camera Animation Sequence made as
//   the editor makes them (New asset factory with its bound actor class, a component binding added as
//   UTemplateSequence::FindOrAddBinding adds it, transform / focal-length keys, a spawnable template edit, folder,
//   marker): extract, Create a new asset from the document, extract equal and equal status; unchanged re-apply is a
//   no-op; equal after reload; per-asset timings.
// UEAssetDoc.Sequencer.Template.ChangeClass: the Details panel's Bound Actor Class picker as an editor
//   twin (FTemplateSequenceEditorUtil::ChangeActorBinding's data steps on one duplicate, its extract applied to another:
//   template.root, AssetDocumentSequencerTemplateRoot.cpp); a document edit of Root.Class alone (tracks and component
//   bindings move to the new root, the root keeps the document's name); a Camera Animation Sequence refuses a
//   non-camera class (Sequencer.CameraClassRequired) and is unchanged.
// UEAssetDoc.Sequencer.Template.CameraShake: a Camera Animation Sequence created from a document (a
//   Camera Actor root: the factory's Cine Camera root changed by the document) plays as a camera shake
//   (USequenceCameraShakePattern on a UDefaultCameraShakeBase, scrubbed): the view moves by the root's transform keys
//   and its field of view follows the component binding's keys.

#include "AssetDocumentSequencerTestSupport.h"

#include "AssetToolsModule.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"
#include "CameraAnimationSequence.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "Editor.h"
#include "Engine/StaticMeshActor.h"
#include "Factories/Factory.h"
#include "IAssetTools.h"
#include "ILevelSequenceModule.h"
#include "IMovieSceneObjectSpawner.h"
#include "MovieScene.h"
#include "MovieSceneCommonHelpers.h"
#include "MovieSceneFolder.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSpawnable.h"
#include "SequenceCameraShake.h"
#include "Shakes/DefaultCameraShakeBase.h"
#include "TemplateSequence.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentSequencerTemplateTests
{
using namespace AssetDocumentSequencerTests;

const TCHAR* const CameraClassPath = TEXT("/Script/TemplateSequence.CameraAnimationSequence");

UTemplateSequence* GetTemplate(const FTarget& Target)
{
	return Cast<UTemplateSequence>(Target.Get());
}

TSharedPtr<FJsonObject> ForTarget(const TSharedPtr<FJsonObject>& Document, const FTarget& Target, const TCHAR* Action)
{
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Document));
	if (Copy.IsValid())
	{
		Copy->SetStringField(TEXT("Target"), Target.PackageName);
		Copy->SetStringField(TEXT("Action"), Action);
	}
	return Copy;
}

FString WithoutTarget(const TSharedPtr<FJsonObject>& Document)
{
	if (!Document.IsValid())
	{
		return FString();
	}
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Document));
	Copy->RemoveField(TEXT("Target"));
	Copy->RemoveField(TEXT("Action"));
	return Serialize(Copy);
}

bool ExpectSame(FAutomationTestBase& Test, const FString& What, const FString& Expected, const FString& Got)
{
	if (Test.TestEqual(*What, Got, Expected))
	{
		return true;
	}
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), *What, *FirstDifference(Expected, Got)));
	return false;
}

bool ApplyOk(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const FString& Stage,
	FAssetDocumentResult* OutResult = nullptr)
{
	const FAssetDocumentResult Result = Apply(Target, Document);
	if (OutResult != nullptr)
	{
		*OutResult = Result;
	}
	if (!Test.TestTrue(*FString::Printf(TEXT("%s applies"), *Stage), Result.IsSuccess()))
	{
		Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *DiagnosticsText(Result)));
		return false;
	}
	return true;
}

FString WarningsOf(const FAssetDocumentResult& Result)
{
	return Result.Diagnostics.IsEmpty() ? FString(TEXT("none")) : DiagnosticsText(Result).Left(600);
}

// The New asset menu: the asset class's factory (a Template Sequence's with its bound actor class, as its dialog sets
// it), through AssetTools.
UTemplateSequence* NewByFactory(FAutomationTestBase& Test, const FTarget& Target, UClass& AssetClass, const TCHAR* FactoryPath, UClass* BoundClass)
{
	UClass* FactoryClass = FindObject<UClass>(nullptr, FactoryPath);
	if (!Test.TestNotNull(*FString::Printf(TEXT("factory %s"), FactoryPath), FactoryClass))
	{
		return nullptr;
	}
	UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
	if (BoundClass != nullptr)
	{
		FClassProperty* Property = FindFProperty<FClassProperty>(FactoryClass, TEXT("BoundActorClass"));
		if (!Test.TestNotNull(TEXT("the factory's BoundActorClass"), Property))
		{
			return nullptr;
		}
		Property->SetObjectPropertyValue_InContainer(Factory, BoundClass);
	}
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UTemplateSequence* Sequence = Cast<UTemplateSequence>(AssetTools.CreateAsset(Target.AssetName,
		FPackageName::GetLongPackagePath(Target.PackageName), &AssetClass, Factory));
	Test.TestNotNull(*FString::Printf(TEXT("%s created by its factory"), *Target.AssetName), Sequence);
	return Sequence;
}

FGuid RootOf(const UTemplateSequence& Sequence)
{
	return Sequence.GetRootObjectBindingID();
}

AActor* RootTemplate(UTemplateSequence& Sequence)
{
	FMovieSceneSpawnable* Spawnable = Sequence.GetMovieScene()->FindSpawnable(RootOf(Sequence));
	return Spawnable != nullptr ? Cast<AActor>(Spawnable->GetObjectTemplate()) : nullptr;
}

// A component binding as the editor's "+ Track > component" makes it (UTemplateSequence::FindOrAddBinding without its
// preview player): a possessable named as the component, parented to the root spawnable and listed as its child, and
// BindPossessableObject's component name.
FGuid AddComponentBinding(UTemplateSequence& Sequence, UActorComponent& Component)
{
	UMovieScene* MovieScene = Sequence.GetMovieScene();
	const FGuid Root = RootOf(Sequence);
	const FGuid Guid = MovieScene->AddPossessable(Component.GetName(), Component.GetClass());
	MovieScene->FindPossessable(Guid)->SetParent(Root, MovieScene);
	MovieScene->FindSpawnable(Root)->AddChildPossessable(Guid);
	Sequence.BindPossessableObject(Guid, Component, nullptr);
	return Guid;
}

UMovieSceneSection* NewSection(UMovieSceneTrack& Track)
{
	UMovieSceneSection* Section = Track.CreateNewSection();
	Track.AddSection(*Section);
	Section->SetRange(Frames(0, 60));
	return Section;
}

// Root transform keys and a focal-length curve on the camera component, a template Details edit, folder and marker.
bool Populate(FAutomationTestBase& Test, UTemplateSequence& Sequence)
{
	UMovieScene* MovieScene = Sequence.GetMovieScene();
	AActor* Template = RootTemplate(Sequence);
	UCineCameraComponent* Camera = Template != nullptr ? Template->FindComponentByClass<UCineCameraComponent>() : nullptr;
	if (!Test.TestNotNull(TEXT("the root's cine camera template"), Camera))
	{
		return false;
	}
	UMovieScene3DTransformTrack* Transform = MovieScene->AddTrack<UMovieScene3DTransformTrack>(RootOf(Sequence));
	UMovieSceneSection* Moves = NewSection(*Transform);
	TArrayView<FMovieSceneDoubleChannel*> Doubles = Moves->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
	Doubles[0]->AddLinearKey(Frame(0), 0.0);
	Doubles[0]->AddCubicKey(Frame(30), 120.0);
	Doubles[0]->AddLinearKey(Frame(60), 40.0);
	Doubles[5]->AddCubicKey(Frame(15), 12.5); // Rotation.Z

	const FGuid Component = AddComponentBinding(Sequence, *Camera);
	UMovieSceneFloatTrack* Focal = MovieScene->AddTrack<UMovieSceneFloatTrack>(Component);
	Focal->SetPropertyNameAndPath(TEXT("CurrentFocalLength"), TEXT("CurrentFocalLength"));
	UMovieSceneSection* Lens = NewSection(*Focal);
	FMovieSceneFloatChannel* Value = Lens->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>()[0];
	Value->AddCubicKey(Frame(0), 35.f);
	Value->AddCubicKey(Frame(45), 50.f);

	// Details edit of the spawned camera (saved as the spawnable's default state).
	Camera->Modify();
	Camera->CurrentAperture = 4.0f;
	Camera->PostEditChange();
#if WITH_EDITORONLY_DATA
	UMovieSceneFolder* Folder = NewObject<UMovieSceneFolder>(MovieScene, NAME_None, RF_Transactional);
	Folder->SetFolderName(TEXT("Cameras"));
	MovieScene->AddRootFolder(Folder);
	Folder->AddChildObjectBinding(RootOf(Sequence));
#endif
	MovieScene->AddMarkedFrame(FMovieSceneMarkedFrame(Frame(30)));
	MovieScene->MarkAsChanged();
	Sequence.MarkPackageDirty();
	return true;
}

// FTemplateSequenceEditorUtil::ChangeActorBinding's data steps (its ISequencer::MakeNewSpawnable makes the spawnable
// with the spawn register's CreateNewSpawnableType; the level sequence spawners are the register's).
bool EditorChangeClass(UTemplateSequence& Sequence, UClass& Class)
{
	UMovieScene* MovieScene = Sequence.GetMovieScene();
	const FGuid Previous = MovieScene->GetSpawnableCount() > 0 ? MovieScene->GetSpawnable(0).GetGuid() : FGuid();
	TArray<TSharedRef<IMovieSceneObjectSpawner>> Spawners;
	FModuleManager::LoadModuleChecked<ILevelSequenceModule>(TEXT("LevelSequence")).GenerateObjectSpawners(Spawners);
	FNewSpawnable NewSpawnable;
	for (const TSharedRef<IMovieSceneObjectSpawner>& Spawner : Spawners)
	{
		TValueOrError<FNewSpawnable, FText> Result = Spawner->CreateNewSpawnableType(Class, *MovieScene, nullptr);
		if (Result.IsValid())
		{
			NewSpawnable = Result.StealValue();
			break;
		}
	}
	if (NewSpawnable.ObjectTemplate == nullptr)
	{
		return false;
	}
	NewSpawnable.Name = MovieSceneHelpers::MakeUniqueSpawnableName(MovieScene, NewSpawnable.Name);
	const FGuid NewGuid = MovieScene->AddSpawnable(NewSpawnable.Name, *NewSpawnable.ObjectTemplate);
	FProperty* Property = FindFProperty<FProperty>(UTemplateSequence::StaticClass(), GET_MEMBER_NAME_CHECKED(UTemplateSequence, BoundActorClass));
	Sequence.PreEditChange(Property);
	Sequence.BoundActorClass = &Class;
	if (Previous.IsValid())
	{
		MovieScene->MoveBindingContents(Previous, NewGuid);
		MovieScene->RemoveSpawnable(Previous);
	}
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	Sequence.PostEditChangeProperty(Event);
	return true;
}

UTemplateSequence* Duplicate(FAutomationTestBase& Test, UObject* Source, const FTarget& Target)
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UTemplateSequence* Copy = Cast<UTemplateSequence>(AssetTools.DuplicateAsset(Target.AssetName,
		FPackageName::GetLongPackagePath(Target.PackageName), Source));
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s duplicated"), *Target.AssetName), Copy))
	{
		return nullptr;
	}
	Test.TestTrue(*FString::Printf(TEXT("%s saved"), *Target.AssetName), Save(*Copy));
	return Copy;
}

// The template fixture: a Template Sequence (Cine Camera Actor root) or a Camera Animation Sequence, populated, saved
// and reloaded.
UTemplateSequence* BuildTemplateFixture(FAutomationTestBase& Test, const FTarget& Target, bool bCamera)
{
	UTemplateSequence* Sequence = bCamera
		? NewByFactory(Test, Target, *UCameraAnimationSequence::StaticClass(), TEXT("/Script/TemplateSequenceEditor.CameraAnimationSequenceFactoryNew"), nullptr)
		: NewByFactory(Test, Target, *UTemplateSequence::StaticClass(), TEXT("/Script/TemplateSequenceEditor.TemplateSequenceFactoryNew"),
			ACineCameraActor::StaticClass());
	if (Sequence == nullptr || !Test.TestTrue(TEXT("the factory made a root"), RootOf(*Sequence).IsValid()) || !Populate(Test, *Sequence))
	{
		return nullptr;
	}
	if (!Test.TestTrue(*FString::Printf(TEXT("%s saved"), *Target.AssetName), Save(*Sequence)))
	{
		return nullptr;
	}
	return Cast<UTemplateSequence>(Reload(Target.ObjectPath));
}

TSharedPtr<FJsonObject> RootOfBody(const TSharedPtr<FJsonObject>& Document)
{
	return Child(BodyOf(Document), TEXT("Root"));
}

double Near(double Value)
{
	return FMath::RoundToDouble(Value * 1000.0) / 1000.0;
}
} // namespace AssetDocumentSequencerTemplateTests

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerTemplateRoundTripTest, "UEAssetDoc.Sequencer.Template.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerTemplateRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerTemplateTests;
	FFixtures Fixtures;
	int32 Passed = 0;
	for (const bool bCamera : {false, true})
	{
		const TCHAR* Name = bCamera ? TEXT("camera animation sequence") : TEXT("template sequence");
		const FTarget& Source = Fixtures.Add(bCamera ? TEXT("CAS_Fixture") : TEXT("TS_Fixture"));
		if (BuildTemplateFixture(*this, Source, bCamera) == nullptr)
		{
			continue;
		}
		double Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Original = ExtractResult(Source.ObjectPath);
		const double ExtractMs = Milliseconds(Start);
		if (!TestTrue(*FString::Printf(TEXT("%s extracts"), Name), Original.IsSuccess() && Original.Payload.IsValid()))
		{
			AddInfo(DiagnosticsText(Original));
			continue;
		}
		const TSharedPtr<FJsonObject> Root = RootOfBody(Original.Payload);
		TestEqual(*FString::Printf(TEXT("%s: Root.Class"), Name), Root.IsValid() ? Root->GetStringField(TEXT("Class")) : FString(),
			ACineCameraActor::StaticClass()->GetPathName());
		const TArray<TSharedPtr<FJsonObject>> Children = Items(Root, TEXT("Children"));
		TestTrue(*FString::Printf(TEXT("%s: the camera component binding is a child of the root, located by name"), Name),
			Children.Num() == 1 && Children[0]->GetStringField(TEXT("Name")) == TEXT("CameraComponent") && !Children[0]->HasField(TEXT("Component")));
		TestEqual(*FString::Printf(TEXT("%s: root tracks (Spawn, Transform)"), Name), Items(Root, TEXT("Tracks")).Num(), 2);
		TestTrue(*FString::Printf(TEXT("%s: the template edit is in Root.Template"), Name),
			Serialize(Child(Root, TEXT("Template"))).Contains(TEXT("CurrentAperture")));
		TestEqual(*FString::Printf(TEXT("%s: status"), Name), StatusName(Original), FString(TEXT("UpToDate")));

		const FTarget& Created = Fixtures.Add(bCamera ? TEXT("CAS_Created") : TEXT("TS_Created"));
		Start = FPlatformTime::Seconds();
		FAssetDocumentResult Applied;
		if (!ApplyOk(*this, Created, ForTarget(Original.Payload, Created, TEXT("Create")), FString(Name) + TEXT(" Create"), &Applied))
		{
			continue;
		}
		const double CreateMs = Milliseconds(Start);
		const FAssetDocumentResult Recreated = ExtractResult(Created.ObjectPath);
		const FString Expected = WithoutTarget(Original.Payload);
		bool bPass = ExpectSame(*this, FString::Printf(TEXT("%s: the created asset extracts as the source"), Name), Expected,
			WithoutTarget(Recreated.Payload));
		bPass &= TestEqual(*FString::Printf(TEXT("%s: equal status"), Name), StatusOf(Recreated).Replace(*Created.PackageName, TEXT("<self>")),
			StatusOf(Original).Replace(*Source.PackageName, TEXT("<self>")));
		UTemplateSequence* SourceSequence = GetTemplate(Source);
		UTemplateSequence* CreatedSequence = GetTemplate(Created);
		if (TestTrue(*FString::Printf(TEXT("%s: both loaded"), Name), SourceSequence != nullptr && CreatedSequence != nullptr))
		{
			bPass &= TestEqual(*FString::Printf(TEXT("%s: created class"), Name), CreatedSequence->GetClass(), SourceSequence->GetClass());
			bPass &= TestEqual(*FString::Printf(TEXT("%s: BoundActorClass"), Name), CreatedSequence->BoundActorClass.ToSoftObjectPath(),
				SourceSequence->BoundActorClass.ToSoftObjectPath());
			TArray<FName> Components;
			CreatedSequence->BoundActorComponents.GenerateValueArray(Components);
			bPass &= TestEqual(*FString::Printf(TEXT("%s: BoundActorComponents"), Name), Components, TArray<FName>({TEXT("CameraComponent")}));
			const FMovieSceneSpawnable* Spawnable = CreatedSequence->GetMovieScene()->FindSpawnable(RootOf(*CreatedSequence));
			bPass &= TestTrue(*FString::Printf(TEXT("%s: the component binding is the root's child possessable"), Name),
				Spawnable != nullptr && Spawnable->GetChildPossessables().Num() == 1);
		}
		Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Again = Apply(Created, ForTarget(Recreated.Payload, Created, TEXT("Update")));
		const double ReapplyMs = Milliseconds(Start);
		bPass &= TestTrue(*FString::Printf(TEXT("%s: unchanged re-apply succeeds (%s)"), Name, *DiagnosticsText(Again)), Again.IsSuccess());
		bPass &= TestFalse(*FString::Printf(TEXT("%s: unchanged re-apply is a no-op"), Name), Again.bSavedAsset);
		if (TestNotNull(*FString::Printf(TEXT("%s: reload"), Name), Reload(Created.ObjectPath)))
		{
			bPass &= ExpectSame(*this, FString::Printf(TEXT("%s: equal after reload"), Name), Expected, WithoutTarget(ExtractResult(Created.ObjectPath).Payload));
		}
		Passed += bPass ? 1 : 0;
		AddInfo(FString::Printf(TEXT("%s %s: extract %.0f ms, Create %.0f ms, unchanged re-apply %.0f ms, %d chars; apply warnings: %s"),
			bPass ? TEXT("PASS") : TEXT("FAIL"), Name, ExtractMs, CreateMs, ReapplyMs, Expected.Len(), *WarningsOf(Applied)));
	}
	TestEqual(TEXT("both template fixtures round-trip"), Passed, 2);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerTemplateChangeClassTest, "UEAssetDoc.Sequencer.Template.ChangeClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerTemplateChangeClassTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerTemplateTests;
	FFixtures Fixtures;
	const FTarget& Source = Fixtures.Add(TEXT("TS_Source"));
	UTemplateSequence* Fixture = BuildTemplateFixture(*this, Source, false);
	if (Fixture == nullptr)
	{
		return true;
	}

	// Editor twin: the class picker on one duplicate, its document applied to another.
	{
		const FTarget& EditorSide = Fixtures.Add(TEXT("TS_Editor"));
		const FTarget& DocumentSide = Fixtures.Add(TEXT("TS_Document"));
		UTemplateSequence* Edited = Duplicate(*this, Fixture, EditorSide);
		if (Edited == nullptr || Duplicate(*this, GetTemplate(Source), DocumentSide) == nullptr)
		{
			return true;
		}
		const FGuid OldRoot = RootOf(*Edited);
		// The editor leaves the root's folder entry on the removed GUID; the reload drops it with this warning (the
		// document side moves the entry with the root, AssetDocumentSequencerTemplateRoot.cpp).
		AddExpectedMessage(TEXT("contained a reference to an Object Binding"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
		TestTrue(TEXT("the editor's class change"), EditorChangeClass(*Edited, *ACameraActor::StaticClass()));
		Edited->GetMovieScene()->MarkAsChanged();
		Edited->MarkPackageDirty();
		TestTrue(TEXT("editor side saved"), Save(*Edited));
		TestNotNull(TEXT("editor side reloads"), Reload(EditorSide.ObjectPath));
		const TSharedPtr<FJsonObject> Expected = Extract(*this, EditorSide.ObjectPath, TEXT("editor side"));
		const double Start = FPlatformTime::Seconds();
		FAssetDocumentResult Applied;
		if (Expected.IsValid() && ApplyOk(*this, DocumentSide, ForTarget(Expected, DocumentSide, TEXT("Update")), TEXT("the editor's document"), &Applied))
		{
			const double ApplyMs = Milliseconds(Start);
			const TSharedPtr<FJsonObject> Got = Extract(*this, DocumentSide.ObjectPath, TEXT("document side"));
			const bool bSame = ExpectSame(*this, TEXT("the document side extracts as the editor side"), WithoutTarget(Expected), WithoutTarget(Got));
			UTemplateSequence* Document = GetTemplate(DocumentSide);
			if (TestNotNull(TEXT("document side loaded"), Document))
			{
				UMovieScene* MovieScene = Document->GetMovieScene();
				const FGuid NewRoot = RootOf(*Document);
				TestEqual(TEXT("one spawnable"), MovieScene->GetSpawnableCount(), 1);
				TestNotEqual(TEXT("the root is a new spawnable (as the editor makes it)"), NewRoot, OldRoot);
				TestTrue(TEXT("BoundActorClass is the Camera Actor"), Document->BoundActorClass.Get() == ACameraActor::StaticClass());
				TestTrue(TEXT("the root spawns a Camera Actor"), RootTemplate(*Document) != nullptr && RootTemplate(*Document)->GetClass() == ACameraActor::StaticClass());
				const FMovieSceneBinding* Binding = MovieScene->FindBinding(NewRoot);
				TestEqual(TEXT("the root's tracks moved to the new root"), Binding != nullptr ? Binding->GetTracks().Num() : 0, 2);
				const FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(NewRoot);
				TestTrue(TEXT("the component binding moved to the new root"), Spawnable != nullptr && Spawnable->GetChildPossessables().Num() == 1
					&& MovieScene->FindPossessable(Spawnable->GetChildPossessables()[0]) != nullptr
					&& MovieScene->FindPossessable(Spawnable->GetChildPossessables()[0])->GetParent() == NewRoot);
				TArray<FName> Components;
				Document->BoundActorComponents.GenerateValueArray(Components);
				TestEqual(TEXT("the component binding still names CameraComponent"), Components, TArray<FName>({TEXT("CameraComponent")}));
			}
			const FAssetDocumentResult Again = Apply(DocumentSide, ForTarget(Got, DocumentSide, TEXT("Update")));
			TestTrue(*FString::Printf(TEXT("twin: unchanged re-apply succeeds (%s)"), *DiagnosticsText(Again)), Again.IsSuccess());
			TestFalse(TEXT("twin: unchanged re-apply is a no-op"), Again.bSavedAsset);
			if (TestNotNull(TEXT("document side reloads"), Reload(DocumentSide.ObjectPath)))
			{
				ExpectSame(*this, TEXT("twin: equal after reload"), WithoutTarget(Expected), WithoutTarget(Extract(*this, DocumentSide.ObjectPath, TEXT("after reload"))));
			}
			AddInfo(FString::Printf(TEXT("%s change class twin: apply %.0f ms; warnings: %s"), bSame ? TEXT("PASS") : TEXT("FAIL"), ApplyMs, *WarningsOf(Applied)));
		}
	}

	// Document edit: Root.Class alone.
	{
		const FTarget& Edited = Fixtures.Add(TEXT("TS_ClassEdit"));
		if (Duplicate(*this, GetTemplate(Source), Edited) != nullptr)
		{
			const TSharedPtr<FJsonObject> Document = ForTarget(Extract(*this, Edited.ObjectPath, TEXT("class edit before")), Edited, TEXT("Update"));
			const TSharedPtr<FJsonObject> Root = RootOfBody(Document);
			if (TestNotNull(TEXT("class edit: Root"), Root.Get()))
			{
				const FString Name = Root->GetStringField(TEXT("Name"));
				Root->SetStringField(TEXT("Class"), ACameraActor::StaticClass()->GetPathName());
				Root->RemoveField(TEXT("Template")); // the Cine Camera's Details do not apply to a Camera Actor
				if (ApplyOk(*this, Edited, Document, TEXT("class edit")))
				{
					FString DiffText;
					const int32 Changes = DiffCount(Document, DiffText);
					if (!TestEqual(TEXT("class edit: no difference to the document afterwards"), Changes, 0))
					{
						AddInfo(DiffText.Left(1500));
					}
					const TSharedPtr<FJsonObject> After = RootOfBody(Extract(*this, Edited.ObjectPath, TEXT("class edit after")));
					TestEqual(TEXT("class edit: the root keeps the document's name"), After.IsValid() ? After->GetStringField(TEXT("Name")) : FString(), Name);
					TestEqual(TEXT("class edit: tracks kept"), Items(After, TEXT("Tracks")).Num(), 2);
					TestEqual(TEXT("class edit: component binding kept"), Items(After, TEXT("Children")).Num(), 1);
				}
			}
		}
	}

	// A Camera Animation Sequence takes camera classes only; the refusal leaves it unchanged.
	{
		const FTarget& Camera = Fixtures.Add(TEXT("CAS_Refused"));
		if (BuildTemplateFixture(*this, Camera, true) != nullptr)
		{
			const TSharedPtr<FJsonObject> Before = Extract(*this, Camera.ObjectPath, TEXT("refusal before"));
			const TSharedPtr<FJsonObject> Document = ForTarget(Before, Camera, TEXT("Update"));
			RootOfBody(Document)->SetStringField(TEXT("Class"), AStaticMeshActor::StaticClass()->GetPathName());
			const FAssetDocumentResult Refused = Apply(Camera, Document);
			TestFalse(TEXT("a static mesh root is refused"), Refused.IsSuccess());
			TestTrue(*FString::Printf(TEXT("Sequencer.CameraClassRequired (%s)"), *DiagnosticsText(Refused)), HasCode(Refused, TEXT("Sequencer.CameraClassRequired")));
			TestEqual(TEXT("at /Body/Root/Class"), FirstPath(Refused, TEXT("Sequencer.CameraClassRequired")), FString(TEXT("/Body/Root/Class")));
			ExpectSame(*this, TEXT("the refused asset is unchanged"), WithoutTarget(Before), WithoutTarget(Extract(*this, Camera.ObjectPath, TEXT("refusal after"))));
		}
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerTemplateCameraShakeTest, "UEAssetDoc.Sequencer.Template.CameraShake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerTemplateCameraShakeTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerTemplateTests;
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("CAS_Shake"));
	const FString Json = FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Class": "%s", "Action": "Create", "Target": "%s", "Properties": {},
		"Body": {"Settings": {"PlaybackRange": {"Start": 0, "End": 30}},
		"Root": {"Class": "/Script/Engine.CameraActor",
			"Tracks": [{"Type": "Transform", "Sections": [{"Channels": {"Location.X": {"Keys": [
				{"Frame": 0, "Value": 0, "Interp": "Linear"}, {"Frame": 30, "Value": 100, "Interp": "Linear"}]}}}]}],
			"Children": [{"Name": "CameraComponent", "Class": "/Script/Engine.CameraComponent",
				"Tracks": [{"Type": "Float", "Property": "FieldOfView", "Sections": [{"Channels": {"Value": {"Keys": [
					{"Frame": 0, "Value": 90, "Interp": "Linear"}, {"Frame": 30, "Value": 60, "Interp": "Linear"}]}}}]}]}]}}})json"),
		CameraClassPath, *Target.PackageName);
	const TSharedPtr<FJsonObject> Document = Parse(Json);
	if (!TestNotNull(TEXT("document parses"), Document.Get()) || !ApplyOk(*this, Target, Document, TEXT("camera animation Create")))
	{
		return true;
	}
	FString DiffText;
	const int32 Changes = DiffCount(Document, DiffText);
	if (!TestEqual(TEXT("no difference to the document after Create"), Changes, 0))
	{
		AddInfo(DiffText.Left(1500));
	}
	UCameraAnimationSequence* Sequence = Cast<UCameraAnimationSequence>(Reload(Target.ObjectPath));
	if (!TestNotNull(TEXT("the camera animation sequence loads"), Sequence))
	{
		return true;
	}
	TestTrue(TEXT("its root is a Camera Actor (the factory's Cine Camera Actor root changed by the document)"),
		RootTemplate(*Sequence) != nullptr && RootTemplate(*Sequence)->GetClass() == ACameraActor::StaticClass());

	// The shake lives in the editor world, as a camera manager's shakes live in theirs (its player plays through the
	// world's camera animation subsystem).
	UWorld* World = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!TestNotNull(TEXT("the editor world"), World))
	{
		return true;
	}
	TStrongObjectPtr<UDefaultCameraShakeBase> Shake(NewObject<UDefaultCameraShakeBase>(World, NAME_None, RF_Transient));
	USequenceCameraShakePattern* Pattern = NewObject<USequenceCameraShakePattern>(Shake.Get());
	Pattern->Sequence = Sequence;
	Pattern->BlendInTime = 0.f;
	Pattern->BlendOutTime = 0.f;
	Shake->SetRootShakePattern(Pattern);
	Shake->StartShake(nullptr, 1.f, ECameraShakePlaySpace::CameraLocal);
	for (const double Seconds : {0.25, 0.5, 0.75})
	{
		FMinimalViewInfo View;
		View.Location = FVector::ZeroVector;
		View.Rotation = FRotator::ZeroRotator;
		View.FOV = 90.f;
		Shake->ScrubAndApplyCameraShake(static_cast<float>(Seconds), 1.f, View);
		const double ExpectedX = 100.0 * Seconds;
		const double ExpectedFov = 90.0 - 30.0 * Seconds;
		TestEqual(*FString::Printf(TEXT("at %.2f s the view moved by the root's Location.X"), Seconds), Near(View.Location.X), Near(ExpectedX), 0.01);
		TestEqual(*FString::Printf(TEXT("at %.2f s the view's field of view follows the component binding"), Seconds), Near(View.FOV), Near(ExpectedFov), 0.01);
		AddInfo(FString::Printf(TEXT("shake at %.2f s: location %s, FOV %.3f"), Seconds, *View.Location.ToString(), View.FOV));
	}
	Shake->StopShake(true);
	Shake->TeardownShake();
	return true;
}

#endif
