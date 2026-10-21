// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Sequencer apply (design §6, phase S2):
// UEAssetDoc.Sequencer.Apply.CorpusRoundTrip: §6.2 (1) on the built fixture and corpus #1-#12 (#12 DMX:
//   plugin tracks, parameter sets, node groups): extract, Create a new sequence from the document, extract equal and
//   equal status; unchanged re-apply is a no-op; equal after reload; per-asset timings.
// UEAssetDoc.Sequencer.Apply.FreeEdits: §6.3 except director / Template Sequence / groups, on Content
//   Browser duplicates of the fixture. "Editor twins": the edit is made once with the engine calls the editor makes on
//   one duplicate, its extract is applied to a second duplicate, and the second extracts equal (channels also evaluate
//   equal). "Document edits": an AI-style change of the extract applies and Diff is empty afterwards, plus facts.
// UEAssetDoc.Sequencer.Apply.Rollback: failing documents (each with a valid change before the failure)
//   fail at their document paths; Extract afterwards equals the document before; no undo history references the package.
// UEAssetDoc.Sequencer.Apply.Dependents: deleting a binding of a sequence another sequence plays reports
//   Sequencer.DependentsAffected (warning) naming that package; nothing else is edited.
// UEAssetDoc.Sequencer.OpenEditorApply: an apply (a marker label, the Fade track deleted) to a sequence
//   open in Sequencer, not saved after the open, lands in place: the same Sequencer shows the applied sequence (its
//   outliner tree drops the track). A failed apply with Sequencer open rolls back and Sequencer shows the rolled-back
//   sequence. A human edit saves, undoes and redoes. With the parent that plays the sequence open
//   in Sequencer, an apply to the sequence (the Fade track again) lands and the parent's Sequencer, focused on the sub
//   section, shows it.
// UEAssetDoc.Sequencer.Apply.Undo: the undo-history rule around a human edit, an AI apply and a failed
//   apply.
// UEAssetDoc.Sequencer.Apply.TrackKinds: phase S3 (§7.2): CVar, level visibility, data layer, slomo,
//   time warp (play-rate curve), parameter sections (MPC, material, custom primitive data, particle parameters),
//   primitive material, particle, camera shake family, conditions (section, track group, row) and dense channels with
//   stated tangents (a 1000-key slomo curve and a 400-key MPC parameter, several channel.append chunks) as an editor
//   twin (equal extract, curves evaluate equal, tangents equal key by key), then document edits of parameters,
//   conditions and the time warp, and a refused custom primitive data name.
// UEAssetDoc.Sequencer.Apply.Preserved: an unrelated update keeps a section group, a node group (both
//   listed as extracted), and content the document does not author (a binding reference no sub section reaches, a
//   channel override on a section the update rewrites).
// UEAssetDoc.Sequencer.Apply.DependentsBlueprint: a Blueprint's "Get Sequence Binding" node is a
//   dependent: deleting the binding it names reports Sequencer.DependentsAffected naming the Blueprint.
// UEAssetDoc.Sequencer.Apply.CineCameraSpawnable: Cine Camera spawnables from documents (the profile
//   section's example, without Template, converted from a possessable) pass the post-save check.
// UEAssetDoc.Sequencer.Apply.OmittedSettings: a listed Settings / MetaData is complete: the
//   flags and Notes / Author an extract lists, deleted from it, are a Diff and are reset; the timing fields a Settings
//   leaves out keep their value and Diff says so (Sequencer.OmittedKept).
// UEAssetDoc.Sequencer.BindingConversion: the raw-file "Convert to Possessable / Spawnable" (B1 rule):
//   GUID, tracks, children and references kept; back to the spawnable with its template; a possessable made spawnable.

#include "AssetDocumentSequencerTestSupport.h"

#include "AssetDocumentOpenEditorTestSupport.h"
#include "AssetDocumentSequencerPrimitives.h"
#include "AssetToolsModule.h"
#include "Bindings/MovieSceneSpawnableActorBinding.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Engine/StaticMeshActor.h"
#include "IAssetTools.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/ScopeExit.h"
#include "FileHelpers.h"
#include "ISequencer.h"
#include "ISequencerModule.h"
#include "MVVM/Extensions/ITrackExtension.h"
#include "MVVM/ViewModelPtr.h"
#include "MVVM/ViewModels/SequencerEditorViewModel.h"
#include "MVVM/ViewModels/ViewModel.h"
#include "MVVM/ViewModels/ViewModelIterators.h"
#include "TickableEditorObject.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "MovieScene.h"
#include "MovieSceneBindingReferences.h"
#include "MovieSceneFolder.h"
#include "MovieSceneMetaData.h"
#include "MovieScenePossessable.h"
#include "MovieSceneTimeHelpers.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ScopedTransaction.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneSubSection.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneFadeTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneSpawnTrack.h"
#include "Tracks/MovieSceneSubTrack.h"
#include "UniversalObjectLocator.h"
#include "Variants/MovieSceneTimeWarpVariant.h"
#include "Channels/MovieSceneCameraShakeSourceTriggerChannel.h"
#include "Channels/IMovieSceneChannelOverrideProvider.h"
#include "Channels/MovieSceneDoublePerlinNoiseChannelContainer.h"
#include "Channels/MovieSceneSectionChannelOverrideRegistry.h"
#include "Conditions/MovieSceneGroupCondition.h"
#include "Conditions/MovieScenePlatformCondition.h"
#include "Conditions/MovieSceneScalabilityCondition.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "UObject/StrongObjectPtr.h"
#include "GameFramework/Actor.h"
#include "K2Node_GetSequenceBinding.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/MaterialInterface.h"
#include "MovieSceneObjectBindingID.h"
#include "Sections/MovieSceneCVarSection.h"
#include "Sections/MovieSceneCameraShakeSection.h"
#include "Sections/MovieSceneCameraShakeSourceTriggerSection.h"
#include "Sections/MovieSceneComponentMaterialParameterSection.h"
#include "Sections/MovieSceneCustomPrimitiveDataSection.h"
#include "Sections/MovieSceneDataLayerSection.h"
#include "Sections/MovieSceneFadeSection.h"
#include "Sections/MovieSceneLevelVisibilitySection.h"
#include "Sections/MovieSceneParameterSection.h"
#include "Sections/MovieSceneParticleSection.h"
#include "Sections/MovieScenePrimitiveMaterialSection.h"
#include "Sections/MovieSceneSlomoSection.h"
#include "Sections/MovieSceneTimeWarpSection.h"
#include "Tracks/MovieSceneCVarTrack.h"
#include "Tracks/MovieSceneCameraShakeSourceTriggerTrack.h"
#include "Tracks/MovieSceneCameraShakeTrack.h"
#include "Tracks/MovieSceneCustomPrimitiveDataTrack.h"
#include "Tracks/MovieSceneDataLayerTrack.h"
#include "Tracks/MovieSceneLevelVisibilityTrack.h"
#include "Tracks/MovieSceneMaterialParameterCollectionTrack.h"
#include "Tracks/MovieSceneMaterialTrack.h"
#include "Tracks/MovieSceneParticleParameterTrack.h"
#include "Tracks/MovieSceneParticleTrack.h"
#include "Tracks/MovieScenePrimitiveMaterialTrack.h"
#include "Tracks/MovieSceneSlomoTrack.h"
#include "Tracks/MovieSceneTimeWarpTrack.h"
#include "Variants/MovieScenePlayRateCurve.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentSequencerApplyTests
{
using namespace AssetDocumentSequencerTests;

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

// Drops "GraphGuid" / "NodeGuid" (Director graphs; generated metadata of the K2 schema): identities in one Blueprint,
// made new for every graph and node the editor or a document creates, so two sequences with equal Directors differ in
// them.
void WithoutGraphGuids(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return;
	}
	if (Value->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		Object->RemoveField(TEXT("GraphGuid"));
		Object->RemoveField(TEXT("NodeGuid"));
		for (const auto& Field : Object->Values)
		{
			WithoutGraphGuids(Field.Value);
		}
	}
	else if (Value->Type == EJson::Array)
	{
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			WithoutGraphGuids(Item);
		}
	}
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
	WithoutGraphGuids(MakeShared<FJsonValueObject>(Copy));
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

// Design §6.2 (4): metadata Author is not compared (a new sequence gets the creating user, as in the editor).
FString Compared(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonObject> Copy = Parse(WithoutTarget(Document));
	if (!Copy.IsValid())
	{
		return FString();
	}
	const TSharedPtr<FJsonObject> Body = BodyOf(Copy);
	if (const TSharedPtr<FJsonObject> MetaData = Child(Body, TEXT("MetaData")))
	{
		MetaData->RemoveField(TEXT("Author"));
		if (MetaData->Values.IsEmpty())
		{
			Body->RemoveField(TEXT("MetaData"));
		}
	}
	return Serialize(Copy);
}

// Every float / double curve of a sequence, keyed by binding path | track class | property or name (k-th) | section
// index | channel type and index.
struct FCurves
{
	TMap<FString, const FMovieSceneFloatChannel*> Floats;
	TMap<FString, const FMovieSceneDoubleChannel*> Doubles;
};

FString BindingPath(const UMovieScene& MovieScene, const FGuid& Guid)
{
	const FMovieScenePossessable* Possessable = const_cast<UMovieScene&>(MovieScene).FindPossessable(Guid);
	if (Possessable == nullptr)
	{
		return Guid.ToString();
	}
	return Possessable->GetParent().IsValid() ? BindingPath(MovieScene, Possessable->GetParent()) + TEXT("/") + Possessable->GetName() : Possessable->GetName();
}

void AddTracks(FCurves& Out, const FString& Scope, const TArray<UMovieSceneTrack*>& Tracks)
{
	TMap<FString, int32> Seen;
	for (const UMovieSceneTrack* Track : Tracks)
	{
		if (Track == nullptr)
		{
			continue;
		}
		const UMovieScenePropertyTrack* Property = Cast<UMovieScenePropertyTrack>(Track);
		FString Name = Track->GetClass()->GetName() + TEXT("|") + (Property != nullptr ? Property->GetPropertyPath().ToString() : Track->GetDisplayName().ToString());
		Name += FString::Printf(TEXT("#%d"), Seen.FindOrAdd(Name)++);
		const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
		for (int32 Index = 0; Index < Sections.Num(); ++Index)
		{
			if (Sections[Index] == nullptr)
			{
				continue;
			}
			const FString Base = FString::Printf(TEXT("%s|%s|%d"), *Scope, *Name, Index);
			const TArrayView<FMovieSceneFloatChannel*> Floats = Sections[Index]->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
			for (int32 Channel = 0; Channel < Floats.Num(); ++Channel)
			{
				Out.Floats.Add(FString::Printf(TEXT("%s|f%d"), *Base, Channel), Floats[Channel]);
			}
			const TArrayView<FMovieSceneDoubleChannel*> Doubles = Sections[Index]->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
			for (int32 Channel = 0; Channel < Doubles.Num(); ++Channel)
			{
				Out.Doubles.Add(FString::Printf(TEXT("%s|d%d"), *Base, Channel), Doubles[Channel]);
			}
		}
	}
}

FCurves CurvesOf(const ULevelSequence& Sequence)
{
	FCurves Out;
	const UMovieScene& MovieScene = *Sequence.GetMovieScene();
	for (const FMovieSceneBinding& Binding : MovieScene.GetBindings())
	{
		AddTracks(Out, BindingPath(MovieScene, Binding.GetObjectGuid()), Binding.GetTracks());
	}
	TArray<UMovieSceneTrack*> Global = MovieScene.GetTracks();
	if (MovieScene.GetCameraCutTrack() != nullptr)
	{
		Global.Insert(MovieScene.GetCameraCutTrack(), 0);
	}
	AddTracks(Out, TEXT("<global>"), Global);
	return Out;
}

// Design §6.2 (3): each curve evaluates equal at every key and at the midpoints between keys (and a frame before /
// after them: extrapolation). Returns the number of channels compared, or -1 after the first difference.
template <typename FChannelType>
int32 CompareCurves(FAutomationTestBase& Test, const FString& What, const TMap<FString, const FChannelType*>& Expected,
	const TMap<FString, const FChannelType*>& Got)
{
	int32 Compared = 0;
	for (const TPair<FString, const FChannelType*>& Item : Expected)
	{
		const FChannelType* const* Other = Got.Find(Item.Key);
		if (Other == nullptr)
		{
			Test.AddError(FString::Printf(TEXT("%s: no channel %s on the created side"), *What, *Item.Key));
			return -1;
		}
		const TArrayView<const FFrameNumber> Times = Item.Value->GetTimes();
		TArray<FFrameTime> Samples;
		for (int32 Index = 0; Index < Times.Num(); ++Index)
		{
			Samples.Add(FFrameTime(Times[Index]));
			if (Index + 1 < Times.Num())
			{
				Samples.Add(FFrameTime(Times[Index]) + (FFrameTime(Times[Index + 1]) - FFrameTime(Times[Index])) * 0.5);
			}
		}
		if (!Times.IsEmpty())
		{
			Samples.Add(FFrameTime(Times[0] - TicksPerFrame));
			Samples.Add(FFrameTime(Times.Last() + TicksPerFrame));
		}
		for (const FFrameTime& Time : Samples)
		{
			float A = 0.f;
			float B = 0.f;
			const bool bA = Item.Value->Evaluate(Time, A);
			const bool bB = (*Other)->Evaluate(Time, B);
			if (bA != bB || FMath::Abs(A - B) > 1.e-4f * FMath::Max(1.f, FMath::Abs(A)))
			{
				Test.AddError(FString::Printf(TEXT("%s: %s evaluates %f vs %f at tick %d"), *What, *Item.Key, A, B, Time.FrameNumber.Value));
				return -1;
			}
		}
		++Compared;
	}
	return Compared;
}

int32 EvaluatesEqualAll(FAutomationTestBase& Test, const FString& What, const ULevelSequence& Expected, const ULevelSequence& Got)
{
	const FCurves A = CurvesOf(Expected);
	const FCurves B = CurvesOf(Got);
	const int32 Floats = CompareCurves(Test, What, A.Floats, B.Floats);
	const int32 Doubles = CompareCurves(Test, What, A.Doubles, B.Doubles);
	return Floats < 0 || Doubles < 0 ? -1 : Floats + Doubles;
}

// The Content Browser's Duplicate, saved.
ULevelSequence* Duplicate(FAutomationTestBase& Test, UObject* Source, const FTarget& Target)
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	ULevelSequence* Copy = Cast<ULevelSequence>(AssetTools.DuplicateAsset(Target.AssetName, FPackageName::GetLongPackagePath(Target.PackageName), Source));
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s duplicated"), *Target.AssetName), Copy))
	{
		return nullptr;
	}
	Test.TestTrue(*FString::Printf(TEXT("%s saved"), *Target.AssetName), Save(*Copy));
	return Copy;
}

// The document applies, the live sequence then has no difference to it (Diff), and the re-apply of its extract is a
// no-op.
bool DocEdit(FAutomationTestBase& Test, const FTarget& Target, const FString& Name, TFunctionRef<void(const TSharedPtr<FJsonObject>&)> Mutate,
	FAssetDocumentResult* OutResult = nullptr)
{
	const TSharedPtr<FJsonObject> Document = ForTarget(Extract(Test, Target.ObjectPath, Name + TEXT(" before")), Target, TEXT("Update"));
	if (!Document.IsValid())
	{
		return false;
	}
	Mutate(BodyOf(Document));
	const double Start = FPlatformTime::Seconds();
	FAssetDocumentResult Applied;
	if (!ApplyOk(Test, Target, Document, Name, &Applied))
	{
		return false;
	}
	const double ApplyMs = Milliseconds(Start);
	if (OutResult != nullptr)
	{
		*OutResult = Applied;
	}
	FString DiffText;
	const int32 Changes = DiffCount(Document, DiffText);
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: no difference to the document afterwards"), *Name), Changes, 0))
	{
		Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Name, *DiffText.Left(1500)));
	}
	const TSharedPtr<FJsonObject> After = Extract(Test, Target.ObjectPath, Name + TEXT(" after"));
	const FAssetDocumentResult Again = Apply(Target, ForTarget(After, Target, TEXT("Update")));
	Test.TestTrue(*FString::Printf(TEXT("%s: unchanged re-apply succeeds (%s)"), *Name, *DiagnosticsText(Again)), Again.IsSuccess());
	Test.TestFalse(*FString::Printf(TEXT("%s: unchanged re-apply is a no-op"), *Name), Again.bSavedAsset);
	Test.AddInfo(FString::Printf(TEXT("document edit %s: apply %.0f ms; warnings: %s"), *Name, ApplyMs, *WarningsOf(Applied)));
	return Changes == 0;
}

// The editor twin (see the file comment). Compare (optional) gets the editor-side and the document-side sequence.
bool Twin(FAutomationTestBase& Test, FFixtures& Fixtures, UObject* Source, const FString& Name, TFunctionRef<bool(ULevelSequence&)> EngineEdit,
	TFunction<void(ULevelSequence&, ULevelSequence&)> Compare = nullptr)
{
	const FTarget& EditorSide = Fixtures.Add(TEXT("LS_Editor"));
	const FTarget& DocumentSide = Fixtures.Add(TEXT("LS_Document"));
	ULevelSequence* Edited = Duplicate(Test, Source, EditorSide);
	if (Edited == nullptr || Duplicate(Test, Source, DocumentSide) == nullptr)
	{
		return false;
	}
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: the editor's edit"), *Name), EngineEdit(*Edited)))
	{
		return false;
	}
	Edited->GetMovieScene()->MarkAsChanged();
	Edited->MarkPackageDirty();
	Test.TestTrue(*FString::Printf(TEXT("%s: editor side saved"), *Name), Save(*Edited));
	// Read the loaded state of the editor's result (a new spawnable template recomputes derived fields in PostLoad).
	Test.TestNotNull(*FString::Printf(TEXT("%s: editor side reloads"), *Name), Reload(EditorSide.ObjectPath));
	const TSharedPtr<FJsonObject> Expected = Extract(Test, EditorSide.ObjectPath, Name + TEXT(" editor side"));
	if (!Expected.IsValid())
	{
		return false;
	}
	const double Start = FPlatformTime::Seconds();
	FAssetDocumentResult Applied;
	if (!ApplyOk(Test, DocumentSide, ForTarget(Expected, DocumentSide, TEXT("Update")), Name + TEXT(": the editor's document"), &Applied))
	{
		return false;
	}
	const double ApplyMs = Milliseconds(Start);
	const TSharedPtr<FJsonObject> Got = Extract(Test, DocumentSide.ObjectPath, Name + TEXT(" document side"));
	bool bSame = ExpectSame(Test, FString::Printf(TEXT("%s: the document side extracts as the editor side"), *Name), WithoutTarget(Expected),
		WithoutTarget(Got));
	const FAssetDocumentResult Again = Apply(DocumentSide, ForTarget(Got, DocumentSide, TEXT("Update")));
	Test.TestTrue(*FString::Printf(TEXT("%s: unchanged re-apply succeeds (%s)"), *Name, *DiagnosticsText(Again)), Again.IsSuccess());
	Test.TestFalse(*FString::Printf(TEXT("%s: unchanged re-apply is a no-op"), *Name), Again.bSavedAsset);
	if (Compare)
	{
		ULevelSequence* EditorSequence = EditorSide.GetSequence();
		ULevelSequence* DocumentSequence = DocumentSide.GetSequence();
		if (Test.TestTrue(*FString::Printf(TEXT("%s: both sides loaded"), *Name), EditorSequence != nullptr && DocumentSequence != nullptr))
		{
			Compare(*EditorSequence, *DocumentSequence);
		}
	}
	if (Test.TestNotNull(*FString::Printf(TEXT("%s: document side reloads"), *Name), Reload(DocumentSide.ObjectPath)))
	{
		bSame &= ExpectSame(Test, FString::Printf(TEXT("%s: equal after reload"), *Name), WithoutTarget(Expected),
			WithoutTarget(Extract(Test, DocumentSide.ObjectPath, Name + TEXT(" after reload"))));
	}
	Test.AddInfo(FString::Printf(TEXT("%s %s: apply %.0f ms; warnings: %s"), bSame ? TEXT("PASS") : TEXT("FAIL"), *Name, ApplyMs, *WarningsOf(Applied)));
	return bSame;
}

UMovieSceneSection* FirstSection(const UMovieSceneTrack* Track)
{
	return Track != nullptr && !Track->GetAllSections().IsEmpty() ? Track->GetAllSections()[0] : nullptr;
}

UMovieSceneSection* TransformSection(ULevelSequence& Sequence, const FGuid& Binding)
{
	return FirstSection(Sequence.GetMovieScene()->FindTrack<UMovieScene3DTransformTrack>(Binding));
}

FMovieSceneDoubleChannel* DoubleChannel(UMovieSceneSection* Section, int32 Index)
{
	return Section != nullptr ? Section->GetChannelProxy().GetChannel<FMovieSceneDoubleChannel>(Index) : nullptr;
}

ACineCameraActor* CameraTemplate(ULevelSequence& Sequence, const FGuid& Binding)
{
	const FMovieSceneBindingReferences* References = Sequence.GetBindingReferences();
	if (References == nullptr)
	{
		return nullptr;
	}
	for (const FMovieSceneBindingReference& Reference : References->GetReferences(Binding))
	{
		if (UMovieSceneSpawnableActorBinding* Spawnable = Cast<UMovieSceneSpawnableActorBinding>(Reference.CustomBinding.Get()))
		{
			return Cast<ACineCameraActor>(Spawnable->GetObjectTemplate());
		}
	}
	return nullptr;
}

int32 KeyAt(FMovieSceneDoubleChannel& Channel, FFrameNumber Time)
{
	const TArrayView<const FFrameNumber> Times = Channel.GetData().GetTimes();
	for (int32 Index = 0; Index < Times.Num(); ++Index)
	{
		if (Times[Index] == Time)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

// Both channels evaluate equal from 30 frames before to 300 frames (pre / post infinity included), every quarter frame.
void EvaluatesEqual(FAutomationTestBase& Test, const FString& What, const FMovieSceneDoubleChannel* Editor, const FMovieSceneDoubleChannel* Document)
{
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: both channels"), *What), Editor != nullptr && Document != nullptr))
	{
		return;
	}
	int32 Samples = 0;
	for (int32 Tick = -30 * TicksPerFrame; Tick <= 300 * TicksPerFrame; Tick += TicksPerFrame / 4)
	{
		double A = 0.0;
		double B = 0.0;
		const bool bA = Editor->Evaluate(FFrameTime(FFrameNumber(Tick)), A);
		const bool bB = Document->Evaluate(FFrameTime(FFrameNumber(Tick)), B);
		if (bA != bB || FMath::Abs(A - B) > 1.e-6)
		{
			Test.AddError(FString::Printf(TEXT("%s: evaluates %s%f (editor) vs %s%f (document) at tick %d"), *What, bA ? TEXT("") : TEXT("nothing "), A,
				bB ? TEXT("") : TEXT("nothing "), B, Tick));
			return;
		}
		++Samples;
	}
	Test.AddInfo(FString::Printf(TEXT("%s: %d samples evaluate equal"), *What, Samples));
}

TSharedPtr<FJsonObject> RootBinding(const TSharedPtr<FJsonObject>& Body, const TCHAR* Name)
{
	return Named(Items(Body, TEXT("Bindings")), TEXT("Name"), Name);
}

TSharedPtr<FJsonObject> GlobalTrack(const TSharedPtr<FJsonObject>& Body, const TCHAR* Type)
{
	return Named(Items(Body, TEXT("Tracks")), TEXT("Type"), Type);
}

// The locator as the document writes it.
FString LocatorText(const FUniversalObjectLocator& Locator)
{
	return UE::AssetDocument::Graph::Sequencer::LocatorString(Locator);
}

TArray<TSharedPtr<FJsonValue>> OneString(const TCHAR* Text)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	Out.Add(MakeShared<FJsonValueString>(Text));
	return Out;
}

TArray<TSharedPtr<FJsonValue>>& ArrayOf(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	if (!Object->HasTypedField<EJson::Array>(Field))
	{
		Object->SetArrayField(Field, TArray<TSharedPtr<FJsonValue>>());
	}
	return const_cast<TArray<TSharedPtr<FJsonValue>>&>(Object->GetArrayField(Field));
}

void Append(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const TCHAR* Json)
{
	ArrayOf(Object, Field).Add(MakeShared<FJsonValueObject>(Parse(Json)));
}

// The fixture saved and loaded again (the state a human opens).
UObject* LoadedFixture(FAutomationTestBase& Test, FFixtures& Fixtures, FBuilt& Built, const TCHAR* Prefix)
{
	const FTarget& Inner = Fixtures.Add(TEXT("LS_Inner"));
	const FTarget& Base = Fixtures.Add(Prefix);
	if (BuildFixture(Test, Base, Inner, Built) == nullptr)
	{
		return nullptr;
	}
	return Reload(Base.ObjectPath);
}
} // namespace AssetDocumentSequencerApplyTests

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerCorpusRoundTripTest, "UEAssetDoc.Sequencer.Apply.CorpusRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerCorpusRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerApplyTests;
	const FContentCopies Copies = CorpusCopies();
	FFixtures Fixtures;
	TArray<TPair<FString, FString>> Sources; // name, object path
	FBuilt Built;
	const FTarget& Inner = Fixtures.Add(TEXT("LS_Inner"));
	const FTarget& Fixture = Fixtures.Add(TEXT("LS_Fixture"));
	if (BuildFixture(*this, Fixture, Inner, Built) != nullptr && Reload(Fixture.ObjectPath) != nullptr)
	{
		Sources.Emplace(TEXT("fixture"), Fixture.ObjectPath);
	}
	for (const FCorpusEntry& Entry : Corpus)
	{
		const FString Name = CorpusName(Entry);
		if (LoadCorpus(Entry) == nullptr)
		{
			AddInfo(FString::Printf(TEXT("SKIP %s: %s"), *Name, Entry.Plugin != nullptr && !PluginEnabled(Entry.Plugin)
				? *FString::Printf(TEXT("plugin %s is not enabled"), Entry.Plugin) : TEXT("not installed")));
			continue;
		}
		Sources.Emplace(Name, Entry.Path);
	}
	const double AllStart = FPlatformTime::Seconds();
	int32 Passed = 0;
	for (const TPair<FString, FString>& Source : Sources)
	{
		const FString& Name = Source.Key;
		double Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Original = ExtractResult(Source.Value);
		const double ExtractMs = Milliseconds(Start);
		if (!TestTrue(*FString::Printf(TEXT("%s extracts"), *Name), Original.IsSuccess() && Original.Payload.IsValid()))
		{
			continue;
		}
		const FTarget& Created = Fixtures.Add(TEXT("LS_Created"));
		Start = FPlatformTime::Seconds();
		FAssetDocumentResult Applied;
		if (!ApplyOk(*this, Created, ForTarget(Original.Payload, Created, TEXT("Create")), Name + TEXT(" Create"), &Applied))
		{
			// Kept for diagnosis (the sidecar is deleted with the fixture).
			FFileHelper::SaveStringToFile(Serialize(Original.Payload),
				*FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetDocumentSequencerCorpus"), Name + TEXT(".assetdoc.json")));
			continue;
		}
		const double CreateMs = Milliseconds(Start);
		const FAssetDocumentResult Recreated = ExtractResult(Created.ObjectPath);
		const FString Expected = Compared(Original.Payload);
		bool bPass = ExpectSame(*this, FString::Printf(TEXT("%s: the created sequence extracts as the source"), *Name), Expected,
			Compared(Recreated.Payload));
		const ULevelSequence* SourceSequence = Cast<ULevelSequence>(LoadQuiet(Source.Value));
		const ULevelSequence* CreatedSequence = Created.GetSequence();
		int32 Curves = -1;
		if (TestTrue(*FString::Printf(TEXT("%s: both loaded"), *Name), SourceSequence != nullptr && CreatedSequence != nullptr))
		{
			Curves = EvaluatesEqualAll(*this, Name, *SourceSequence, *CreatedSequence);
			bPass &= Curves >= 0;
		}
		const FString SourcePackage = FPackageName::ObjectPathToPackageName(Source.Value);
		bPass &= TestEqual(*FString::Printf(TEXT("%s: equal status"), *Name), StatusOf(Recreated).Replace(*Created.PackageName, TEXT("<self>")),
			StatusOf(Original).Replace(*SourcePackage, TEXT("<self>")));
		Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Again = Apply(Created, ForTarget(Recreated.Payload, Created, TEXT("Update")));
		const double ReapplyMs = Milliseconds(Start);
		bPass &= TestTrue(*FString::Printf(TEXT("%s: unchanged re-apply succeeds (%s)"), *Name, *DiagnosticsText(Again)), Again.IsSuccess());
		bPass &= TestFalse(*FString::Printf(TEXT("%s: unchanged re-apply is a no-op"), *Name), Again.bSavedAsset);
		if (TestNotNull(*FString::Printf(TEXT("%s: reload"), *Name), Reload(Created.ObjectPath)))
		{
			bPass &= ExpectSame(*this, FString::Printf(TEXT("%s: equal after reload"), *Name), Expected,
				Compared(ExtractResult(Created.ObjectPath).Payload));
		}
		Passed += bPass ? 1 : 0;
		AddInfo(FString::Printf(TEXT("%s %s: extract %.0f ms, Create %.0f ms, unchanged re-apply %.0f ms, %d chars, %d curves evaluate equal, status %s; "
			"apply warnings: %s"), bPass ? TEXT("PASS") : TEXT("FAIL"), *Name, ExtractMs, CreateMs, ReapplyMs, Expected.Len(), Curves, *StatusName(Recreated),
			*WarningsOf(Applied)));
	}
	AddInfo(FString::Printf(TEXT("round trip: %d of %d in %.1f s"), Passed, Sources.Num(), Milliseconds(AllStart) / 1000.0));
	TestTrue(TEXT("the fixture and the template corpus are round-tripped"), Sources.Num() >= 11);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerFreeEditsTest, "UEAssetDoc.Sequencer.Apply.FreeEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerFreeEditsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerApplyTests;
	FFixtures Fixtures;
	FBuilt Built;
	UObject* Base = LoadedFixture(*this, Fixtures, Built, TEXT("LS_Base"));
	if (!TestNotNull(TEXT("fixture"), Base))
	{
		return false;
	}
	const FString BasePath = Base->GetPathName();
	auto Source = [&BasePath]() { return LoadQuiet(BasePath); };

	// --- Bindings -----------------------------------------------------------------------------------------------
	// Rename the camera (document: RenamedFrom); the GUID, the camera cut and the folder follow.
	{
		const FTarget& Target = Fixtures.Add(TEXT("LS_Rename"));
		if (Duplicate(*this, Source(), Target) != nullptr)
		{
			DocEdit(*this, Target, TEXT("rename the camera"), [](const TSharedPtr<FJsonObject>& Body)
			{
				const TSharedPtr<FJsonObject> Camera = RootBinding(Body, TEXT("ShotCam"));
				Camera->SetStringField(TEXT("Name"), TEXT("HeroCam"));
				Camera->SetStringField(TEXT("RenamedFrom"), TEXT("ShotCam"));
				const TSharedPtr<FJsonObject> Cut = Items(GlobalTrack(Body, TEXT("CameraCut")), TEXT("Sections"))[0];
				Child(Cut, TEXT("Properties"))->SetObjectField(TEXT("CameraBindingID"), Parse(TEXT(R"({"Binding": "HeroCam"})")));
			});
			if (ULevelSequence* Sequence = Target.GetSequence())
			{
				const FMovieScenePossessable* Camera = Sequence->GetMovieScene()->FindPossessable(Built.Camera);
				TestTrue(TEXT("rename: the GUID is kept"), Camera != nullptr && Camera->GetName() == TEXT("HeroCam"));
				const UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(FirstSection(Sequence->GetMovieScene()->GetCameraCutTrack()));
				TestTrue(TEXT("rename: the camera cut still binds the camera"), Cut != nullptr && Cut->GetCameraBindingID().GetGuid() == Built.Camera);
				TestTrue(TEXT("rename: still in the folder"), Serialize(Extract(*this, Target.ObjectPath, TEXT("rename"))).Contains(TEXT("\"Folder\":\"Cameras\",\"Name\":\"HeroCam\"")));
			}
		}
	}
	// Add a possessable by locator (the actor need not be loaded).
	{
		const FTarget& Target = Fixtures.Add(TEXT("LS_AddPossessable"));
		const FString Locator = TEXT("uobj://actor?payload0=/Game/Maps/Lobby.Lobby:PersistentLevel.PointLight_2");
		if (Duplicate(*this, Source(), Target) != nullptr && DocEdit(*this, Target, TEXT("add a possessable"), [&Locator](const TSharedPtr<FJsonObject>& Body)
			{
				Append(Body, TEXT("Bindings"), *FString::Printf(TEXT(R"({"Name": "Lamp", "Class": "/Script/Engine.PointLight", "Bind": ["%s"]})"), *Locator));
			}))
		{
			ULevelSequence* Sequence = Target.GetSequence();
			FGuid Lamp;
			for (int32 Index = 0; Sequence != nullptr && Index < Sequence->GetMovieScene()->GetPossessableCount(); ++Index)
			{
				if (Sequence->GetMovieScene()->GetPossessable(Index).GetName() == TEXT("Lamp"))
				{
					Lamp = Sequence->GetMovieScene()->GetPossessable(Index).GetGuid();
				}
			}
			const TArrayView<const FMovieSceneBindingReference> References = Lamp.IsValid() ? Sequence->GetBindingReferences()->GetReferences(Lamp)
				: TArrayView<const FMovieSceneBindingReference>();
			TestTrue(TEXT("add a possessable: one locator reference"), References.Num() == 1 && LocatorText(References[0].Locator) == Locator
				&& References[0].CustomBinding == nullptr);
		}
	}
	// A second locator on the door (twin: FMovieSceneBindingReferences::AddBinding, as "Add selected" in the binding menu).
	Twin(*this, Fixtures, Source(), TEXT("second locator"), [&Built](ULevelSequence& Sequence)
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>();
		static_cast<UMovieSceneSequence&>(Sequence).GetBindingReferences()->AddBinding(Built.Door, FUniversalObjectLocator(Actor, World));
		World->EditorDestroyActor(Actor, false);
		return true;
	});
	// Delete the possessable (twin: the tree's Delete).
	Twin(*this, Fixtures, Source(), TEXT("delete a possessable"), [&Built](ULevelSequence& Sequence)
	{
		Sequence.UnbindPossessableObjects(Built.Door);
		return Sequence.GetMovieScene()->RemovePossessable(Built.Door);
	});
	// Edit the spawnable template (Details on the template: CurrentAperture).
	Twin(*this, Fixtures, Source(), TEXT("spawnable template"), [&Built](ULevelSequence& Sequence)
	{
		ACineCameraActor* Template = CameraTemplate(Sequence, Built.Camera);
		if (Template == nullptr)
		{
			return false;
		}
		UCineCameraComponent* Camera = Template->GetCineCameraComponent();
		Camera->Modify();
		Camera->CurrentAperture = 4.0f;
		FPropertyChangedEvent Changed(UCineCameraComponent::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UCineCameraComponent, CurrentAperture)));
		static_cast<UObject*>(Camera)->PostEditChangeProperty(Changed);
		return true;
	}, [this, &Built](ULevelSequence&, ULevelSequence& Document)
	{
		const ACineCameraActor* Template = CameraTemplate(Document, Built.Camera);
		TestTrue(TEXT("spawnable template: the document side's template has the aperture"), Template != nullptr
			&& FMath::IsNearlyEqual(Template->GetCineCameraComponent()->CurrentAperture, 4.0f));
	});

	// --- Tracks and sections ------------------------------------------------------------------------------------
	// A Float track on a nested property of the camera component (a focus pull): FocusSettings.ManualFocusDistance.
	Twin(*this, Fixtures, Source(), TEXT("add a Float track"), [&Built](ULevelSequence& Sequence)
	{
		UMovieSceneFloatTrack* Track = Sequence.GetMovieScene()->AddTrack<UMovieSceneFloatTrack>(Built.Component);
		Track->SetPropertyNameAndPath(TEXT("ManualFocusDistance"), TEXT("FocusSettings.ManualFocusDistance"));
		FMovieSceneFloatChannel* Value = AddSection(*Track, 0, 150)->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0);
		Value->AddCubicKey(Frame(0), 400.0f, RCTM_SmartAuto);
		Value->AddCubicKey(Frame(90), 150.0f, RCTM_Auto);
		Value->AutoSetTangents();
		return true;
	});
	// Split the transform section in two, the second on row 1, additive.
	Twin(*this, Fixtures, Source(), TEXT("split a section (row 1, additive)"), [&Built](ULevelSequence& Sequence)
	{
		UMovieScene3DTransformTrack* Track = Sequence.GetMovieScene()->FindTrack<UMovieScene3DTransformTrack>(Built.Camera);
		if (Track == nullptr || Track->GetAllSections().IsEmpty())
		{
			return false;
		}
		Track->GetAllSections()[0]->SetRange(Frames(0, 75));
		UMovieSceneSection* Second = Track->CreateNewSection();
		Second->SetRange(Frames(75, 150));
		Second->SetRowIndex(1);
		Second->SetBlendType(EMovieSceneBlendType::Additive);
		Track->AddSection(*Second);
		DoubleChannel(Second, 0)->AddCubicKey(Frame(75), 10.0, RCTM_SmartAuto);
		Track->UpdateEasing();
		return true;
	});
	// Ease in 10 frames on the fade section.
	Twin(*this, Fixtures, Source(), TEXT("ease in"), [](ULevelSequence& Sequence)
	{
		UMovieSceneFadeTrack* Fade = Sequence.GetMovieScene()->FindTrack<UMovieSceneFadeTrack>();
		UMovieSceneSection* Section = FirstSection(Fade);
		if (Section == nullptr)
		{
			return false;
		}
		Section->Easing.bManualEaseIn = true;
		Section->Easing.ManualEaseInDuration = 10 * TicksPerFrame;
		Fade->UpdateEasing();
		return true;
	});
	// Mute the fade track.
	Twin(*this, Fixtures, Source(), TEXT("mute a track"), [](ULevelSequence& Sequence)
	{
		UMovieSceneFadeTrack* Fade = Sequence.GetMovieScene()->FindTrack<UMovieSceneFadeTrack>();
		if (Fade == nullptr)
		{
			return false;
		}
		Fade->SetEvalDisabled(true);
		return true;
	});
	// Reorder the global tracks (document: the sub track before the fade).
	{
		const FTarget& Target = Fixtures.Add(TEXT("LS_Reorder"));
		if (Duplicate(*this, Source(), Target) != nullptr && DocEdit(*this, Target, TEXT("reorder tracks"), [](const TSharedPtr<FJsonObject>& Body)
			{
				TArray<TSharedPtr<FJsonValue>>& Tracks = ArrayOf(Body, TEXT("Tracks"));
				const int32 Fade = Tracks.IndexOfByPredicate([](const TSharedPtr<FJsonValue>& Track) { return Track->AsObject()->GetStringField(TEXT("Type")) == TEXT("Fade"); });
				const TSharedPtr<FJsonValue> Moved = Tracks[Fade];
				Tracks.RemoveAt(Fade);
				Tracks.Add(Moved);
			}))
		{
			const TArray<TSharedPtr<FJsonObject>> Tracks = Items(BodyOf(Extract(*this, Target.ObjectPath, TEXT("reordered"))), TEXT("Tracks"));
			TestTrue(TEXT("reorder tracks: fade last"), !Tracks.IsEmpty() && Tracks.Last()->GetStringField(TEXT("Type")) == TEXT("Fade"));
		}
	}
	// Three fade sections, then delete the middle one.
	{
		const FTarget& Target = Fixtures.Add(TEXT("LS_Sections"));
		if (Duplicate(*this, Source(), Target) != nullptr)
		{
			DocEdit(*this, Target, TEXT("three sections"), [](const TSharedPtr<FJsonObject>& Body)
			{
				GlobalTrack(Body, TEXT("Fade"))->SetArrayField(TEXT("Sections"), Parse(TEXT(R"({"S": [
					{"Start": 0, "End": 50, "Channels": {"Value": {"Keys": [{"Frame": 0, "Value": 1.0}, {"Frame": 15, "Value": 0.0}]}}},
					{"Start": 50, "End": 100, "Channels": {"Value": {"Default": 0.5}}},
					{"Start": 100, "End": 150, "Channels": {"Value": {"Keys": [{"Frame": 135, "Value": 0.0}, {"Frame": 150, "Value": 1.0}]}}}]})"))
					->GetArrayField(TEXT("S")));
			});
			DocEdit(*this, Target, TEXT("delete the middle section"), [](const TSharedPtr<FJsonObject>& Body)
			{
				ArrayOf(GlobalTrack(Body, TEXT("Fade")), TEXT("Sections")).RemoveAt(1);
			});
			ULevelSequence* Sequence = Target.GetSequence();
			const UMovieSceneFadeTrack* Fade = Sequence != nullptr ? Sequence->GetMovieScene()->FindTrack<UMovieSceneFadeTrack>() : nullptr;
			TestTrue(TEXT("delete the middle section: two sections 0-50 and 100-150"), Fade != nullptr && Fade->GetAllSections().Num() == 2
				&& Fade->GetAllSections()[0]->GetRange() == Frames(0, 50) && Fade->GetAllSections()[1]->GetRange() == Frames(100, 150));
		}
	}

	// --- Keys ---------------------------------------------------------------------------------------------------
	// One key to Constant, one to Break with tangents, a sub-frame key, post-infinity Cycle, a default on an empty channel;
	// both sides evaluate equal.
	Twin(*this, Fixtures, Source(), TEXT("key edits"), [&Built](ULevelSequence& Sequence)
	{
		UMovieSceneSection* Section = TransformSection(Sequence, Built.Camera);
		FMovieSceneDoubleChannel* X = DoubleChannel(Section, 0);
		FMovieSceneDoubleChannel* Y = DoubleChannel(Section, 1);
		if (X == nullptr || Y == nullptr || KeyAt(*X, Frame(30)) == INDEX_NONE || KeyAt(*X, Frame(60)) == INDEX_NONE)
		{
			return false;
		}
		Section->Modify();
		TArrayView<FMovieSceneDoubleValue> Values = X->GetData().GetValues();
		Values[KeyAt(*X, Frame(30))].InterpMode = RCIM_Constant;
		FMovieSceneDoubleValue& Broken = Values[KeyAt(*X, Frame(60))];
		Broken.InterpMode = RCIM_Cubic;
		Broken.TangentMode = RCTM_Break;
		Broken.Tangent.ArriveTangent = 0.1f;
		Broken.Tangent.LeaveTangent = -0.2f;
		X->AddCubicKey(FFrameNumber(45 * TicksPerFrame + TicksPerFrame / 4), -450.0, RCTM_SmartAuto);
		X->PostInfinityExtrap = RCCE_Cycle;
		X->AutoSetTangents();
		Y->SetDefault(25.0);
		return true;
	}, [this, &Built](ULevelSequence& Editor, ULevelSequence& Document)
	{
		EvaluatesEqual(*this, TEXT("key edits: Location.X"), DoubleChannel(TransformSection(Editor, Built.Camera), 0),
			DoubleChannel(TransformSection(Document, Built.Camera), 0));
		EvaluatesEqual(*this, TEXT("key edits: Location.Y"), DoubleChannel(TransformSection(Editor, Built.Camera), 1),
			DoubleChannel(TransformSection(Document, Built.Camera), 1));
	});

	// --- Time ---------------------------------------------------------------------------------------------------
	Twin(*this, Fixtures, Source(), TEXT("display rate 30 -> 24"), [](ULevelSequence& Sequence)
	{
		Sequence.GetMovieScene()->SetDisplayRate(FFrameRate(24, 1));
		return true;
	});
	Twin(*this, Fixtures, Source(), TEXT("tick resolution 24000 -> 60000"), [](ULevelSequence& Sequence)
	{
		UMovieScene* MovieScene = Sequence.GetMovieScene();
		UE::MovieScene::TimeHelpers::MigrateFrameTimes(MovieScene->GetTickResolution(), FFrameRate(60000, 1), MovieScene);
		MovieScene->SetTickResolutionDirectly(FFrameRate(60000, 1));
		return true;
	}, [this, &Built](ULevelSequence&, ULevelSequence& Document)
	{
		TestTrue(TEXT("tick resolution: the document side migrated"), Document.GetMovieScene()->GetTickResolution() == FFrameRate(60000, 1));
		const FMovieSceneDoubleChannel* X = DoubleChannel(TransformSection(Document, Built.Camera), 0);
		TestTrue(TEXT("tick resolution: the sub-frame key moved to 100.5 frames in the new ticks"), X != nullptr && X->GetData().GetTimes().Num() == 6
			&& X->GetData().GetTimes()[4] == FFrameNumber(201000));
	});
	Twin(*this, Fixtures, Source(), TEXT("move and lock the playback range"), [](ULevelSequence& Sequence)
	{
		Sequence.GetMovieScene()->SetPlaybackRange(Frames(10, 160));
		Sequence.GetMovieScene()->SetPlaybackRangeLocked(true);
		return true;
	});

	// --- Hierarchy ----------------------------------------------------------------------------------------------
	Twin(*this, Fixtures, Source(), TEXT("retime a sub section"), [](ULevelSequence& Sequence)
	{
		UMovieSceneSubSection* Sub = Cast<UMovieSceneSubSection>(FirstSection(Sequence.GetMovieScene()->FindTrack<UMovieSceneSubTrack>()));
		if (Sub == nullptr)
		{
			return false;
		}
		Sub->Modify();
		Sub->Parameters.StartFrameOffset = FFrameNumber(10 * TicksPerFrame);
		Sub->Parameters.TimeScale = FMovieSceneTimeWarpVariant(2.0);
		Sub->Parameters.HierarchicalBias = 5;
		return true;
	});
	Twin(*this, Fixtures, Source(), TEXT("add a sub section"), [](ULevelSequence& Sequence)
	{
		UMovieSceneSubTrack* Track = Sequence.GetMovieScene()->FindTrack<UMovieSceneSubTrack>();
		const UMovieSceneSubSection* First = Cast<UMovieSceneSubSection>(FirstSection(Track));
		if (First == nullptr || First->GetSequence() == nullptr)
		{
			return false;
		}
		return Track->AddSequence(First->GetSequence(), Frame(60), 30 * TicksPerFrame) != nullptr;
	});

	// --- Folders and markers ------------------------------------------------------------------------------------
	Twin(*this, Fixtures, Source(), TEXT("nest folders, move a binding and a track, colour"), [&Built](ULevelSequence& Sequence)
	{
		UMovieScene* MovieScene = Sequence.GetMovieScene();
		UMovieSceneFolder* Set = NewObject<UMovieSceneFolder>(MovieScene, NAME_None, RF_Transactional);
		Set->SetFolderName(TEXT("Set"));
		MovieScene->AddRootFolder(Set);
		UMovieSceneFolder* Props = NewObject<UMovieSceneFolder>(MovieScene, NAME_None, RF_Transactional);
		Props->SetFolderName(TEXT("Props"));
		Set->AddChildFolder(Props);
		Props->AddChildObjectBinding(Built.Door);
		UMovieSceneFadeTrack* Fade = MovieScene->FindTrack<UMovieSceneFadeTrack>();
		if (Fade == nullptr)
		{
			return false;
		}
		Set->AddChildTrack(Fade);
#if WITH_EDITORONLY_DATA
		Set->SetFolderColor(FColor(80, 160, 80));
#endif
		return true;
	});
	Twin(*this, Fixtures, Source(), TEXT("add, move and delete markers; a determinism fence"), [](ULevelSequence& Sequence)
	{
		UMovieScene* MovieScene = Sequence.GetMovieScene();
		auto IndexOf = [MovieScene](const TCHAR* Label)
		{
			return MovieScene->GetMarkedFrames().IndexOfByPredicate([Label](const FMovieSceneMarkedFrame& Marker) { return Marker.Label == Label; });
		};
		FMovieSceneMarkedFrame Close(Frame(90));
		Close.Label = TEXT("Close");
		MovieScene->AddMarkedFrame(Close);
		FMovieSceneMarkedFrame Fence(Frame(120));
		Fence.Label = TEXT("Fence");
		Fence.bIsDeterminismFence = true;
		MovieScene->AddMarkedFrame(Fence);
		if (IndexOf(TEXT("Open")) == INDEX_NONE || IndexOf(TEXT("Close")) == INDEX_NONE)
		{
			return false;
		}
		MovieScene->SetMarkedFrame(IndexOf(TEXT("Open")), Frame(45));
		MovieScene->DeleteMarkedFrame(IndexOf(TEXT("Close")));
		return true;
	});
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerRollbackTest, "UEAssetDoc.Sequencer.Apply.Rollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerRollbackTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerApplyTests;
	FFixtures Fixtures;
	FBuilt Built;
	UObject* Base = LoadedFixture(*this, Fixtures, Built, TEXT("LS_Base"));
	if (!TestNotNull(TEXT("fixture"), Base))
	{
		return false;
	}
	const FTarget& Target = Fixtures.Add(TEXT("LS_Rollback"));
	if (Duplicate(*this, Base, Target) == nullptr)
	{
		return false;
	}
	// A sequence that plays the target: adding it back into the target is an indirect cycle.
	const FTarget& Outer = Fixtures.Add(TEXT("LS_Outer"));
	ULevelSequence* OuterSequence = NewSequence(Outer);
	OuterSequence->GetMovieScene()->AddTrack<UMovieSceneSubTrack>()->AddSequence(Target.GetSequence(), Frame(0), 60 * TicksPerFrame);
	TestTrue(TEXT("outer sequence saved"), Save(*OuterSequence));

	struct FFailure
	{
		const TCHAR* Name;
		const TCHAR* Code;
		TFunction<void(const TSharedPtr<FJsonObject>&)> Mutate;
	};
	const FString OuterPath = Outer.ObjectPath;
	const FFailure Failures[] = {
		{TEXT("unknown property"), TEXT("Sequencer.ValidationFailed"), [](const TSharedPtr<FJsonObject>& Body)
			{
				Append(RootBinding(Body, TEXT("Door")), TEXT("Tracks"), TEXT(R"({"Type": "Float", "Property": "NoSuchProperty", "Sections": [{"Start": 0, "End": 10}]})"));
			}},
		{TEXT("camera cut to a missing binding"), TEXT("Sequencer.UnknownBinding"), [](const TSharedPtr<FJsonObject>& Body)
			{
				const TSharedPtr<FJsonObject> Cut = Items(GlobalTrack(Body, TEXT("CameraCut")), TEXT("Sections"))[0];
				Child(Cut, TEXT("Properties"))->SetObjectField(TEXT("CameraBindingID"), Parse(TEXT(R"({"Binding": "Nobody"})")));
			}},
		{TEXT("sub-sequence cycle"), TEXT("Sequencer.CircularSubsequence"), [&OuterPath](const TSharedPtr<FJsonObject>& Body)
			{
				Append(GlobalTrack(Body, TEXT("Subsequence")), TEXT("Sections"),
					*FString::Printf(TEXT(R"({"Start": 60, "End": 90, "Properties": {"SubSequence": "%s"}})"), *OuterPath));
			}},
		{TEXT("malformed locator"), TEXT("Sequencer.InvalidLocator"), [](const TSharedPtr<FJsonObject>& Body)
			{
				RootBinding(Body, TEXT("Door"))->SetArrayField(TEXT("Bind"), OneString(TEXT("not a locator")));
			}},
		{TEXT("unknown track type"), TEXT("Sequencer.UnknownTrackType"), [](const TSharedPtr<FJsonObject>& Body)
			{
				Append(Body, TEXT("Tracks"), TEXT(R"({"Type": "NoSuchTrack"})"));
			}},
		{TEXT("folder not listed"), TEXT("Sequencer.InvalidFolder"), [](const TSharedPtr<FJsonObject>& Body)
			{
				RootBinding(Body, TEXT("Door"))->SetStringField(TEXT("Folder"), TEXT("Nowhere"));
			}},
	};
	for (const FFailure& Failure : Failures)
	{
		const TSharedPtr<FJsonObject> Before = Extract(*this, Target.ObjectPath, FString(Failure.Name) + TEXT(" before"));
		if (!Before.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Document = ForTarget(Before, Target, TEXT("Update"));
		// A valid change first (a new binding, a marker label), so a late failure has something to roll back.
		Append(BodyOf(Document), TEXT("Bindings"), TEXT(R"({"Name": "Extra", "Bind": []})"));
		BodyOf(Document)->GetArrayField(TEXT("Markers"))[0]->AsObject()->SetStringField(TEXT("Label"), TEXT("Changed"));
		Failure.Mutate(BodyOf(Document));
		const FAssetDocumentResult Result = Apply(Target, Document);
		TestFalse(*FString::Printf(TEXT("%s fails"), Failure.Name), Result.IsSuccess());
		TestTrue(*FString::Printf(TEXT("%s: %s (got %s)"), Failure.Name, Failure.Code, *DiagnosticsText(Result)), HasCode(Result, Failure.Code));
		const FString Path = FirstPath(Result, Failure.Code);
		TestTrue(*FString::Printf(TEXT("%s: at a document path (%s)"), Failure.Name, *Path), Path.StartsWith(TEXT("/Body")));
		ExpectSame(*this, FString::Printf(TEXT("%s: the sequence is as before"), Failure.Name), Serialize(Before),
			Serialize(Extract(*this, Target.ObjectPath, FString(Failure.Name) + TEXT(" after"))));
		TestFalse(*FString::Printf(TEXT("%s: no undo history references the sequence"), Failure.Name),
			UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
		const UObject* Asset = Target.Get();
		AddInfo(FString::Printf(TEXT("%s: %s at %s; package %s"), Failure.Name, Failure.Code, *Path,
			Asset != nullptr && Asset->GetOutermost()->IsDirty() ? TEXT("dirty") : TEXT("clean")));
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerDependentsTest, "UEAssetDoc.Sequencer.Apply.Dependents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerDependentsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerApplyTests;
	FFixtures Fixtures;
	FBuilt Built;
	UObject* Base = LoadedFixture(*this, Fixtures, Built, TEXT("LS_Base"));
	if (!TestNotNull(TEXT("fixture"), Base))
	{
		return false;
	}
	const FTarget& Target = Fixtures.Add(TEXT("LS_Played"));
	if (Duplicate(*this, Base, Target) == nullptr)
	{
		return false;
	}
	const FTarget& Parent = Fixtures.Add(TEXT("LS_Parent"));
	ULevelSequence* ParentSequence = NewSequence(Parent);
	ParentSequence->GetMovieScene()->AddTrack<UMovieSceneSubTrack>()->AddSequence(Target.GetSequence(), Frame(0), 60 * TicksPerFrame);
	TestTrue(TEXT("parent saved"), Save(*ParentSequence));
	FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({Parent.PackageFile});
	const FString ParentBefore = Serialize(Extract(*this, Parent.ObjectPath, TEXT("parent before")));

	// A rename keeps the GUID: no warning.
	FAssetDocumentResult Renamed;
	DocEdit(*this, Target, TEXT("rename the door"), [](const TSharedPtr<FJsonObject>& Body)
	{
		RootBinding(Body, TEXT("Door"))->SetStringField(TEXT("RenamedFrom"), TEXT("Door"));
		RootBinding(Body, TEXT("Door"))->SetStringField(TEXT("Name"), TEXT("FrontDoor"));
	}, &Renamed);
	TestFalse(TEXT("a rename reports no dependents"), HasCode(Renamed, TEXT("Sequencer.DependentsAffected")));

	FAssetDocumentResult Deleted;
	DocEdit(*this, Target, TEXT("delete the door"), [](const TSharedPtr<FJsonObject>& Body)
	{
		TArray<TSharedPtr<FJsonValue>>& Bindings = ArrayOf(Body, TEXT("Bindings"));
		Bindings.RemoveAll([](const TSharedPtr<FJsonValue>& Binding) { return Binding->AsObject()->GetStringField(TEXT("Name")) == TEXT("FrontDoor"); });
	}, &Deleted);
	TestTrue(*FString::Printf(TEXT("Sequencer.DependentsAffected (%s)"), *DiagnosticsText(Deleted)), HasCode(Deleted, TEXT("Sequencer.DependentsAffected")));
	const FAssetDocumentDiagnostic* Warning = Deleted.Diagnostics.FindByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
		{ return Diagnostic.Code == TEXT("Sequencer.DependentsAffected"); });
	TestTrue(TEXT("the warning names the parent and the binding"), Warning != nullptr && Warning->Message.Contains(Parent.PackageName)
		&& Warning->Message.Contains(TEXT("FrontDoor")));
	TestEqual(TEXT("the parent is not edited"), Serialize(Extract(*this, Parent.ObjectPath, TEXT("parent after"))), ParentBefore);
	TestFalse(TEXT("the parent is not dirtied"), Parent.Get() == nullptr || Parent.Get()->GetOutermost()->IsDirty());
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

namespace AssetDocumentSequencerOpenEditorTests
{
// Ticks the editor tickables: Sequencer refreshes its tree and evaluates in its own Tick. Slate is not ticked: the
// Sequencer widgets of a Level Sequence editor opened by a test have no window to tick in.
void TickEditorUi()
{
	for (int32 Tick = 0; Tick < 3; ++Tick)
	{
		FTickableEditorObject::TickObjects(0.f);
	}
}

// The tracks Sequencer's outliner shows (its view model tree).
TArray<UMovieSceneTrack*> ShownTracks(ISequencer& Sequencer)
{
	using namespace UE::Sequencer;
	TArray<UMovieSceneTrack*> Shown;
	const TSharedPtr<FSequencerEditorViewModel> ViewModel = Sequencer.GetViewModel();
	const FViewModelPtr Root = ViewModel.IsValid() ? ViewModel->GetRootModel() : FViewModelPtr();
	if (Root)
	{
		for (const TViewModelPtr<ITrackExtension> Track : Root->GetDescendantsOfType<ITrackExtension>())
		{
			if (UMovieSceneTrack* Model = Track->GetTrack())
			{
				Shown.AddUnique(Model);
			}
		}
	}
	return Shown;
}

// Sequencers are held weakly: Sequencer ensures its editor holds the only persistent reference.
bool ShowsFadeTrack(const TWeakPtr<ISequencer>& Weak)
{
	const TSharedPtr<ISequencer> Sequencer = Weak.Pin();
	return Sequencer.IsValid()
		&& ShownTracks(*Sequencer).ContainsByPredicate([](const UMovieSceneTrack* Track) { return Track->IsA<UMovieSceneFadeTrack>(); });
}

bool ShowsMarker(const TWeakPtr<ISequencer>& Weak, const TCHAR* Label)
{
	const TSharedPtr<ISequencer> Sequencer = Weak.Pin();
	return Sequencer.IsValid()
		&& Sequencer->GetMarkedFrames().ContainsByPredicate([Label](const FMovieSceneMarkedFrame& Marked) { return Marked.Label == Label; });
}

const UMovieSceneSequence* RootOf(const TWeakPtr<ISequencer>& Weak)
{
	const TSharedPtr<ISequencer> Sequencer = Weak.Pin();
	return Sequencer.IsValid() ? Sequencer->GetRootMovieSceneSequence() : nullptr;
}

// The human's Save in the open editor (its toolbar Save, Ctrl+S): the editor's SaveAsset command, run as the shared
// open-editor test support does (from automation the bare command saves nothing: see SaveFromEditor). The bare
// command's no-op in this unattended session is checked first.
bool HumanSave(FAutomationTestBase& Test, UObject& Asset)
{
	namespace OpenEditorTests = UE::AssetDocument::OpenEditorTests;
	FAssetEditorToolkit* Toolkit = OpenEditorTests::FindToolkit(&Asset);
	if (!Test.TestNotNull(TEXT("the human saves in the open editor"), Toolkit))
	{
		return false;
	}
	Test.TestTrue(TEXT("unattended, the editor's bare Save command saves nothing"), OpenEditorTests::BareSaveCommandSavesNothing(*Toolkit, Asset));
	return OpenEditorTests::SaveFromEditor(*Toolkit, Asset);
}

// The Sequencers created while it lives (ISequencerModule::RegisterOnSequencerCreated).
struct FCreatedSequencers
{
	TArray<TWeakPtr<ISequencer>> Created;
	FDelegateHandle Handle;
	FCreatedSequencers()
	{
		Handle = FModuleManager::LoadModuleChecked<ISequencerModule>(TEXT("Sequencer")).RegisterOnSequencerCreated(
			FOnSequencerCreated::FDelegate::CreateLambda([this](TSharedRef<ISequencer> Sequencer) { Created.Add(Sequencer); }));
	}
	~FCreatedSequencers()
	{
		if (ISequencerModule* Module = FModuleManager::GetModulePtr<ISequencerModule>(TEXT("Sequencer")))
		{
			Module->UnregisterOnSequencerCreated(Handle);
		}
	}
	// The open Sequencer whose root sequence is Root.
	TWeakPtr<ISequencer> Find(const UObject* Root) const
	{
		for (const TWeakPtr<ISequencer>& Weak : Created)
		{
			const TSharedPtr<ISequencer> Sequencer = Weak.Pin();
			if (Sequencer.IsValid() && Sequencer->GetRootMovieSceneSequence() == Root)
			{
				return Sequencer;
			}
		}
		return TWeakPtr<ISequencer>();
	}
};
} // namespace AssetDocumentSequencerOpenEditorTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerOpenEditorApplyTest, "UEAssetDoc.Sequencer.OpenEditorApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerOpenEditorApplyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerApplyTests;
	namespace OpenEd = AssetDocumentSequencerOpenEditorTests;
	UAssetEditorSubsystem* Editors = GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("asset editor subsystem"), Editors) || GEditor->Trans == nullptr)
	{
		return false;
	}
	FFixtures Fixtures;
	FBuilt Built;
	UObject* Base = LoadedFixture(*this, Fixtures, Built, TEXT("LS_Base"));
	if (!TestNotNull(TEXT("fixture"), Base))
	{
		return false;
	}
	const FTarget& Target = Fixtures.Add(TEXT("LS_Open"));
	const FTarget& Parent = Fixtures.Add(TEXT("LS_OpenParent"));
	if (Duplicate(*this, Base, Target) == nullptr)
	{
		return false;
	}
	ULevelSequence* ParentSequence = NewSequence(Parent);
	UMovieSceneSubSection* SubSection = Cast<UMovieSceneSubSection>(
		ParentSequence->GetMovieScene()->AddTrack<UMovieSceneSubTrack>()->AddSequence(Target.GetSequence(), Frame(0), 60 * TicksPerFrame));
	TestTrue(TEXT("parent saved"), Save(*ParentSequence));
	const TSharedPtr<FJsonObject> Original = Extract(*this, Target.ObjectPath, TEXT("original"));
	const TSharedPtr<FJsonObject> FadeTrack = GlobalTrack(BodyOf(Original), TEXT("Fade"));
	if (!TestNotNull(TEXT("the fixture has a Fade track"), FadeTrack.Get()))
	{
		return false;
	}
	const FString FadeText = Serialize(FadeTrack);

	OpenEd::FCreatedSequencers Sequencers;
	ON_SCOPE_EXIT
	{
		for (const FTarget* Open : {&Target, &Parent})
		{
			if (UObject* Asset = Open->Get())
			{
				Editors->CloseAllEditorsForAsset(Asset);
			}
		}
	};
	// The sequence is opened and not saved.
	if (!Editors->OpenEditorForAsset(Target.Get()))
	{
		AddWarning(TEXT("Sequencer did not open in this session; the open-editor apply was not exercised."));
		return true;
	}
	OpenEd::TickEditorUi();
	TWeakPtr<ISequencer> Sequencer = Sequencers.Find(Target.Get());
	if (!TestTrue(TEXT("the Sequencer open on the sequence"), Sequencer.IsValid()))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("after the open the package is %s"), Target.Get()->GetOutermost()->IsDirty() ? TEXT("dirty") : TEXT("clean")));
	ReleaseUndoHistory();
	TestTrue(TEXT("before: Sequencer shows the Fade track"), OpenEd::ShowsFadeTrack(Sequencer));
	const IAssetEditorInstance* EditorBefore = Editors->FindEditorForAsset(Target.Get(), false);

	const TSharedPtr<FJsonObject> Edit = ForTarget(Original, Target, TEXT("Update"));
	BodyOf(Edit)->GetArrayField(TEXT("Markers"))[0]->AsObject()->SetStringField(TEXT("Label"), TEXT("Edited"));
	ArrayOf(BodyOf(Edit), TEXT("Tracks")).RemoveAll([](const TSharedPtr<FJsonValue>& Track)
	{
		return Track->AsObject()->GetStringField(TEXT("Type")) == TEXT("Fade");
	});
	const FAssetDocumentResult Applied = Apply(Target, Edit);
	AddInfo(FString::Printf(TEXT("apply while open: %s"), *DiagnosticsText(Applied)));
	if (!TestTrue(TEXT("apply while open"), Applied.IsSuccess()))
	{
		return false;
	}
	OpenEd::TickEditorUi();
	TestNull(TEXT("the Fade track is deleted"), Target.GetSequence()->GetMovieScene()->FindTrack<UMovieSceneFadeTrack>());
	TestFalse(TEXT("the package is saved"), Target.Get()->GetOutermost()->IsDirty());
	TestFalse(TEXT("the apply is not an undo step"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	TestTrue(TEXT("the same editor stays open (in place)"), Editors->FindEditorForAsset(Target.Get(), false) == EditorBefore);
	TestTrue(TEXT("the same Sequencer shows the sequence"), OpenEd::RootOf(Sequencer) == Target.Get());
	TestFalse(TEXT("after the apply: Sequencer's outliner drops the Fade track"), OpenEd::ShowsFadeTrack(Sequencer));
	TestTrue(TEXT("after the apply: Sequencer shows the marker"), OpenEd::ShowsMarker(Sequencer, TEXT("Edited")));
	const FString AppliedText = Serialize(Extract(*this, Target.ObjectPath, TEXT("applied")));

	// A failure (an unknown track type) after a valid change (the marker label, the Fade track back) rolls back.
	{
		const TSharedPtr<FJsonObject> Bad = ForTarget(Parse(AppliedText), Target, TEXT("Update"));
		BodyOf(Bad)->GetArrayField(TEXT("Markers"))[0]->AsObject()->SetStringField(TEXT("Label"), TEXT("Failed"));
		Append(BodyOf(Bad), TEXT("Tracks"), *FadeText);
		Append(BodyOf(Bad), TEXT("Tracks"), TEXT(R"({"Type": "NoSuchTrack"})"));
		const FAssetDocumentResult Failed = Apply(Target, Bad);
		AddInfo(FString::Printf(TEXT("failing apply while open: %s"), *DiagnosticsText(Failed)));
		TestFalse(TEXT("failing apply while open fails"), Failed.IsSuccess());
		TestTrue(TEXT("failing apply: Sequencer.UnknownTrackType"), HasCode(Failed, TEXT("Sequencer.UnknownTrackType")));
		OpenEd::TickEditorUi();
		TestEqual(TEXT("after the rollback the sequence is as applied"), Serialize(Extract(*this, Target.ObjectPath, TEXT("rolled back"))), AppliedText);
		TestFalse(TEXT("after the rollback the package is clean"), Target.Get()->GetOutermost()->IsDirty());
		TestTrue(TEXT("after the rollback the same editor is open"), Editors->FindEditorForAsset(Target.Get(), false) == EditorBefore);
		TestFalse(TEXT("after the rollback: Sequencer shows no Fade track"), OpenEd::ShowsFadeTrack(Sequencer));
		TestTrue(TEXT("after the rollback: Sequencer shows the applied marker"), OpenEd::ShowsMarker(Sequencer, TEXT("Edited")));
	}

	// A human edit (the playback range), saved; undo / redo with Sequencer open.
	ReleaseUndoHistory();
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentSequencerTests", "HumanOpenEdit", "Set Playback Range"));
		UMovieScene* MovieScene = Target.GetSequence()->GetMovieScene();
		MovieScene->Modify();
		MovieScene->SetPlaybackRange(Frames(0, 125));
	}
	const FString Human = Serialize(Extract(*this, Target.ObjectPath, TEXT("human edit")));
	TestNotEqual(TEXT("the human edit landed"), Human, AppliedText);
	TestTrue(TEXT("the human's save"), OpenEd::HumanSave(*this, *Target.Get()));
	TestFalse(TEXT("saved"), Target.Get()->GetOutermost()->IsDirty());
	TestEqual(TEXT("saving keeps the apply and the human edit"), Serialize(Extract(*this, Target.ObjectPath, TEXT("saved"))), Human);
	TestTrue(TEXT("the human edit undoes"), GEditor->Trans->CanUndo() && GEditor->UndoTransaction());
	OpenEd::TickEditorUi();
	TestEqual(TEXT("undo restores the applied sequence"), Serialize(Extract(*this, Target.ObjectPath, TEXT("undone"))), AppliedText);
	TestTrue(TEXT("the human edit redoes"), GEditor->Trans->CanRedo() && GEditor->RedoTransaction());
	OpenEd::TickEditorUi();
	TestEqual(TEXT("redo restores the human edit"), Serialize(Extract(*this, Target.ObjectPath, TEXT("redone"))), Human);
	ReleaseUndoHistory();
	Target.Get()->GetOutermost()->SetDirtyFlag(false);
	Editors->CloseAllEditorsForAsset(Target.Get());
	TestNull(TEXT("the editor closed"), Editors->FindEditorForAsset(Target.Get(), false));
	Sequencer.Reset();

	// The parent that plays the sequence is open in Sequencer: an apply to the sequence (the Fade track back) lands and
	// the parent's Sequencer, focused on the sub section, shows it.
	if (Editors->OpenEditorForAsset(Parent.Get()))
	{
		OpenEd::TickEditorUi();
		const TWeakPtr<ISequencer> ParentSequencer = Sequencers.Find(Parent.Get());
		if (TestTrue(TEXT("the Sequencer open on the parent"), ParentSequencer.IsValid()))
		{
			const TSharedPtr<FJsonObject> Again = ForTarget(Extract(*this, Target.ObjectPath, TEXT("before the sub apply")), Target, TEXT("Update"));
			Append(BodyOf(Again), TEXT("Tracks"), *FadeText);
			const FAssetDocumentResult SubApplied = Apply(Target, Again);
			AddInfo(FString::Printf(TEXT("apply with the parent open: %s"), *DiagnosticsText(SubApplied)));
			TestTrue(TEXT("apply with the parent open"), SubApplied.IsSuccess());
			OpenEd::TickEditorUi();
			TestNotNull(TEXT("the Fade track is back"), Target.GetSequence()->GetMovieScene()->FindTrack<UMovieSceneFadeTrack>());
			TestTrue(TEXT("the parent's Sequencer stays open"), Sequencers.Find(Parent.Get()).IsValid());
			SubSection = Cast<UMovieSceneSubSection>(FirstSection(Parent.GetSequence()->GetMovieScene()->FindTrack<UMovieSceneSubTrack>()));
			if (TestNotNull(TEXT("the parent's sub section"), SubSection))
			{
				if (const TSharedPtr<ISequencer> Focusing = ParentSequencer.Pin())
				{
					Focusing->FocusSequenceInstance(*SubSection);
				}
				OpenEd::TickEditorUi();
				const TSharedPtr<ISequencer> Focused = ParentSequencer.Pin();
				TestTrue(TEXT("focused on the sequence"), Focused.IsValid() && Focused->GetFocusedMovieSceneSequence() == Target.Get());
				TestTrue(TEXT("the parent's Sequencer shows the applied Fade track"), OpenEd::ShowsFadeTrack(ParentSequencer));
			}
		}
		Editors->CloseAllEditorsForAsset(Parent.Get());
		TestNull(TEXT("the parent's editor closed"), Editors->FindEditorForAsset(Parent.Get(), false));
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerUndoTest, "UEAssetDoc.Sequencer.Apply.Undo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerUndoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerApplyTests;
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	FFixtures Fixtures;
	FBuilt Built;
	UObject* Base = LoadedFixture(*this, Fixtures, Built, TEXT("LS_Base"));
	if (!TestNotNull(TEXT("fixture"), Base))
	{
		return false;
	}
	const FTarget& Target = Fixtures.Add(TEXT("LS_Undo"));
	if (Duplicate(*this, Base, Target) == nullptr)
	{
		return false;
	}
	ReleaseUndoHistory();
	auto UndoRedoAll = [this, &Target](const TCHAR* Stage)
	{
		int32 Undone = 0;
		while (Undone < 16 && GEditor->Trans->CanUndo() && GEditor->UndoTransaction())
		{
			++Undone;
			TestTrue(*FString::Printf(TEXT("%s: whole after undo %d"), Stage, Undone), ExtractResult(Target.ObjectPath).IsSuccess());
		}
		int32 Redone = 0;
		while (Redone < Undone && GEditor->Trans->CanRedo() && GEditor->RedoTransaction())
		{
			++Redone;
		}
		TestEqual(*FString::Printf(TEXT("%s: every undone step redoes"), Stage), Redone, Undone);
	};
	auto HumanEdit = [&Target](int32 End)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentSequencerTests", "HumanEdit", "Set Playback Range"));
		UMovieScene* MovieScene = Target.GetSequence()->GetMovieScene();
		MovieScene->Modify();
		MovieScene->SetPlaybackRange(Frames(0, End));
	};
	// 1. The human changes the playback range and saves; the AI edits a marker: history referencing the sequence is
	// cleared, the human edit is kept.
	HumanEdit(125);
	TestTrue(TEXT("human save"), UEditorLoadingAndSavingUtils::SavePackages({Target.Get()->GetOutermost()}, false));
	TestTrue(TEXT("the human edit is in the undo history"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	if (!DocEdit(*this, Target, TEXT("AI edit"), [](const TSharedPtr<FJsonObject>& Body)
		{
			Body->GetArrayField(TEXT("Markers"))[0]->AsObject()->SetStringField(TEXT("Label"), TEXT("AI"));
		}))
	{
		return false;
	}
	TestFalse(TEXT("after AI edit: no undo history references the sequence"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const FString AfterEdit = Serialize(Extract(*this, Target.ObjectPath, TEXT("after AI edit")));
	TestTrue(TEXT("human edit kept"), AfterEdit.Contains(TEXT("\"PlaybackRange\":{\"End\":125,\"Start\":0}")));
	UndoRedoAll(TEXT("after AI edit"));
	TestEqual(TEXT("after AI edit + undo/redo"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after undo/redo"))), AfterEdit);

	// 2. The human edits again and saves; an AI apply fails after its first changes and rolls back to the human state;
	// undo stays safe.
	ReleaseUndoHistory();
	HumanEdit(130);
	TestTrue(TEXT("second human save"), UEditorLoadingAndSavingUtils::SavePackages({Target.Get()->GetOutermost()}, false));
	const FString SecondHuman = Serialize(Extract(*this, Target.ObjectPath, TEXT("after second human edit")));
	const TSharedPtr<FJsonObject> Failing = ForTarget(Parse(SecondHuman), Target, TEXT("Update"));
	Append(BodyOf(Failing), TEXT("Bindings"), TEXT(R"({"Name": "Extra", "Bind": []})"));
	RootBinding(BodyOf(Failing), TEXT("Door"))->SetArrayField(TEXT("Bind"), OneString(TEXT("not a locator")));
	const FAssetDocumentResult Failed = Apply(Target, Failing);
	TestFalse(TEXT("failing apply fails"), Failed.IsSuccess());
	TestEqual(TEXT("after rollback: the human state"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after rollback"))), SecondHuman);
	TestFalse(TEXT("after rollback: no undo history references the sequence"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	UndoRedoAll(TEXT("after rollback"));
	TestEqual(TEXT("after rollback + undo/redo"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after rollback undo/redo"))), SecondHuman);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerBindingConversionTest, "UEAssetDoc.Sequencer.BindingConversion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerBindingConversionTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerApplyTests;
	FFixtures Fixtures;
	FBuilt Built;
	UObject* Base = LoadedFixture(*this, Fixtures, Built, TEXT("LS_Base"));
	if (!TestNotNull(TEXT("fixture"), Base))
	{
		return false;
	}
	const FTarget& Target = Fixtures.Add(TEXT("LS_Convert"));
	if (Duplicate(*this, Base, Target) == nullptr)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Original = Extract(*this, Target.ObjectPath, TEXT("original"));
	auto Facts = [this, &Target, &Built](const TCHAR* Stage, bool bSpawnable)
	{
		ULevelSequence* Sequence = Target.GetSequence();
		if (!TestNotNull(*FString::Printf(TEXT("%s: sequence"), Stage), Sequence))
		{
			return;
		}
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		const FMovieScenePossessable* Camera = MovieScene->FindPossessable(Built.Camera);
		TestTrue(*FString::Printf(TEXT("%s: the camera keeps its GUID"), Stage), Camera != nullptr && Camera->GetName() == TEXT("ShotCam"));
		const FMovieScenePossessable* Component = MovieScene->FindPossessable(Built.Component);
		TestTrue(*FString::Printf(TEXT("%s: the component binding keeps its parent"), Stage), Component != nullptr && Component->GetParent() == Built.Camera);
		const FMovieSceneDoubleChannel* X = DoubleChannel(TransformSection(*Sequence, Built.Camera), 0);
		TestTrue(*FString::Printf(TEXT("%s: the transform keys are kept"), Stage), X != nullptr && X->GetNumKeys() == 6);
		const UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(FirstSection(MovieScene->GetCameraCutTrack()));
		TestTrue(*FString::Printf(TEXT("%s: the camera cut still binds the camera"), Stage), Cut != nullptr && Cut->GetCameraBindingID().GetGuid() == Built.Camera);
		TestEqual(*FString::Printf(TEXT("%s: spawnable"), Stage), CameraTemplate(*Sequence, Built.Camera) != nullptr, bSpawnable);
		TestEqual(*FString::Printf(TEXT("%s: a Spawn track only while spawnable"), Stage),
			MovieScene->FindTrack(UMovieSceneSpawnTrack::StaticClass(), Built.Camera) != nullptr, bSpawnable);
	};
	// Convert To Possessable: Bind (no locator yet), Class, and the Spawn track goes with the conversion.
	DocEdit(*this, Target, TEXT("convert to possessable"), [](const TSharedPtr<FJsonObject>& Body)
	{
		const TSharedPtr<FJsonObject> Camera = RootBinding(Body, TEXT("ShotCam"));
		Camera->RemoveField(TEXT("Spawnable"));
		Camera->SetArrayField(TEXT("Bind"), TArray<TSharedPtr<FJsonValue>>());
		Camera->SetStringField(TEXT("Class"), TEXT("/Script/CinematicCamera.CineCameraActor"));
		ArrayOf(Camera, TEXT("Tracks")).RemoveAll([](const TSharedPtr<FJsonValue>& Track) { return Track->AsObject()->GetStringField(TEXT("Type")) == TEXT("Spawn"); });
	});
	Facts(TEXT("possessable"), false);
	// And back: the extracted document of the spawnable (class + template) applies onto the possessable.
	if (ApplyOk(*this, Target, ForTarget(Original, Target, TEXT("Update")), TEXT("convert back to spawnable")))
	{
		Facts(TEXT("spawnable again"), true);
		ExpectSame(*this, TEXT("spawnable again: extracts as the original"), WithoutTarget(Original),
			WithoutTarget(Extract(*this, Target.ObjectPath, TEXT("spawnable again"))));
	}
	// A possessable made spawnable (no Spawn track listed: the editor's default one).
	DocEdit(*this, Target, TEXT("possessable to spawnable"), [](const TSharedPtr<FJsonObject>& Body)
	{
		const TSharedPtr<FJsonObject> Door = RootBinding(Body, TEXT("Door"));
		Door->RemoveField(TEXT("Bind"));
		Door->RemoveField(TEXT("Class"));
		Door->SetObjectField(TEXT("Spawnable"), Parse(TEXT(R"({"Class": "/Script/Engine.StaticMeshActor"})")));
	});
	if (ULevelSequence* Sequence = Target.GetSequence())
	{
		const FMovieSceneBindingReferences* References = Sequence->GetBindingReferences();
		const TArrayView<const FMovieSceneBindingReference> Refs = References->GetReferences(Built.Door);
		UMovieSceneSpawnableActorBinding* Spawnable = Refs.Num() == 1 ? Cast<UMovieSceneSpawnableActorBinding>(Refs[0].CustomBinding.Get()) : nullptr;
		TestTrue(TEXT("possessable to spawnable: a static mesh actor template"), Spawnable != nullptr && Spawnable->GetObjectTemplate() != nullptr
			&& Spawnable->GetObjectTemplate()->IsA<AStaticMeshActor>());
		TestNotNull(TEXT("possessable to spawnable: the default Spawn track"), Sequence->GetMovieScene()->FindTrack(UMovieSceneSpawnTrack::StaticClass(), Built.Door));
		TestNotNull(TEXT("possessable to spawnable: the GUID is kept"), Sequence->GetMovieScene()->FindPossessable(Built.Door));
	}
	return true;
}


// ---------------------------------------------------------------------------------------------------------------------

namespace AssetDocumentSequencerApplyTests
{
constexpr int32 SlomoKeys = 1000;
constexpr int32 GlowKeys = 400;

UMovieSceneSection* NewSection(UMovieSceneTrack& Track, int32 End = 150)
{
	UMovieSceneSection* Section = Track.CreateNewSection();
	Section->SetRange(TRange<FFrameNumber>(Frame(0), Frame(End)));
	Track.AddSection(*Section);
	return Section;
}

void SetInvert(UMovieSceneCondition& Condition)
{
	if (const FBoolProperty* Invert = FindFProperty<FBoolProperty>(UMovieSceneCondition::StaticClass(), TEXT("bInvert")))
	{
		Invert->SetPropertyValue_InContainer(&Condition, true);
	}
}

// The S3 track kinds, made with the calls the track editors and the Details panel make.
bool AddTrackKinds(ULevelSequence& Sequence, const FBuilt& Built)
{
	UMovieScene& MovieScene = *Sequence.GetMovieScene();

	// Console variables, with a platform condition on the section and a group condition on the track.
	UMovieSceneCVarTrack* CVars = MovieScene.AddTrack<UMovieSceneCVarTrack>();
	UMovieSceneCVarSection* CVar = Cast<UMovieSceneCVarSection>(NewSection(*CVars));
	CVar->ConsoleVariables.ValuesByCVar.Add(TEXT("r.Tonemapper.Sharpen"), TEXT("1"));
	UMovieScenePlatformCondition* Platform = NewObject<UMovieScenePlatformCondition>(CVar, NAME_None, RF_Transactional);
	Platform->ValidPlatforms = {TEXT("Windows"), TEXT("Mac")};
	CVar->ConditionContainer.Condition = Platform;
	UMovieSceneGroupCondition* Group = NewObject<UMovieSceneGroupCondition>(CVars, NAME_None, RF_Transactional);
	Group->Operator = EMovieSceneGroupConditionOperator::Or;
	UMovieSceneScalabilityCondition* Scalability = NewObject<UMovieSceneScalabilityCondition>(Group, NAME_None, RF_Transactional);
	Scalability->Group = EMovieSceneScalabilityConditionGroup::Effects;
	Scalability->Operator = EMovieSceneScalabilityConditionOperator::GreaterThanOrEqualTo;
	Scalability->Level = EMovieSceneScalabilityConditionLevel::High;
	UMovieScenePlatformCondition* NotAndroid = NewObject<UMovieScenePlatformCondition>(Group, NAME_None, RF_Transactional);
	NotAndroid->ValidPlatforms = {TEXT("Android")};
	SetInvert(*NotAndroid);
	Group->SubConditions.AddDefaulted_GetRef().Condition = Scalability;
	Group->SubConditions.AddDefaulted_GetRef().Condition = NotAndroid;
	CVars->ConditionContainer.Condition = Group;

	// Level visibility, with a row condition.
	UMovieSceneLevelVisibilityTrack* Levels = MovieScene.AddTrack<UMovieSceneLevelVisibilityTrack>();
	UMovieSceneLevelVisibilitySection* Visibility = Cast<UMovieSceneLevelVisibilitySection>(NewSection(*Levels));
	Visibility->SetVisibility(ELevelVisibility::Hidden);
	Visibility->SetLevelNames({TEXT("Lobby")});
	UMovieScenePlatformCondition* RowCondition = NewObject<UMovieScenePlatformCondition>(Levels, NAME_None, RF_Transactional);
	RowCondition->ValidPlatforms = {TEXT("Linux")};
	Levels->FindOrAddTrackRowMetadata(0).ConditionContainer.Condition = RowCondition;

	// Data layer.
	UMovieSceneDataLayerSection* DataLayer = Cast<UMovieSceneDataLayerSection>(NewSection(*MovieScene.AddTrack<UMovieSceneDataLayerTrack>()));
	DataLayer->SetDesiredState(EDataLayerRuntimeState::Loaded);
	DataLayer->SetFlushOnUnload(true);

	// Slomo: a dense curve whose Auto keys keep tangents Sequencer would compute differently (stated) and User keys.
	UMovieSceneSlomoSection* Slomo = Cast<UMovieSceneSlomoSection>(NewSection(*MovieScene.AddTrack<UMovieSceneSlomoTrack>(), 260));
	{
		TMovieSceneChannelData<FMovieSceneFloatValue> Data = Slomo->FloatCurve.GetData();
		Data.Reset();
		for (int32 Index = 0; Index < SlomoKeys; ++Index)
		{
			FMovieSceneFloatValue Value(1.f + 0.25f * FMath::Sin(static_cast<float>(Index) * 0.05f));
			Value.InterpMode = RCIM_Cubic;
			Value.TangentMode = Index % 3 == 1 ? RCTM_User : RCTM_Auto;
			Value.Tangent.ArriveTangent = 0.01f * static_cast<float>(Index % 7 + 1);
			Value.Tangent.LeaveTangent = Index % 3 == 1 ? -0.02f : Value.Tangent.ArriveTangent;
			Data.AddKey(FFrameNumber(Index * (TicksPerFrame / 4)), Value);
		}
	}

	// Time warp: a play-rate curve (the "+ Time Warp" menu).
	{
		UMovieSceneTimeWarpTrack* WarpTrack = NewObject<UMovieSceneTimeWarpTrack>(&MovieScene, NAME_None, RF_Transactional);
		UMovieSceneSection* WarpSection = WarpTrack->CreateNewSection();
		UMovieScenePlayRateCurve* Curve = NewObject<UMovieScenePlayRateCurve>(WarpSection, NAME_None, RF_Transactional);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		static_cast<UMovieSceneTimeWarpGetter*>(Curve)->InitializeDefaults(WarpSection);
#else
		static_cast<UMovieSceneTimeWarpGetter*>(Curve)->InitializeDefaults();
#endif
		WarpSection->GetTimeWarp()->Set(Curve);
		WarpTrack->AddSection(*WarpSection);
		MovieScene.AddGivenTrack(WarpTrack);
		TMovieSceneChannelData<FMovieSceneDoubleValue> Rate = Curve->PlayRate.GetData();
		Rate.Reset();
		Rate.AddKey(Frame(0), FMovieSceneDoubleValue(1.0));
		Rate.AddKey(Frame(60), FMovieSceneDoubleValue(0.5));
		Rate.AddKey(Frame(120), FMovieSceneDoubleValue(1.5));
		Curve->PlayRate.AutoSetTangents();
	}

	// Material parameter collection: a dense scalar parameter (some stated tangents) and a color parameter.
	UMovieSceneParameterSection* Collection = Cast<UMovieSceneParameterSection>(NewSection(*MovieScene.AddTrack<UMovieSceneMaterialParameterCollectionTrack>(), 210));
	for (int32 Index = 0; Index < GlowKeys; ++Index)
	{
		Collection->AddScalarParameterKey(TEXT("Glow"), FFrameNumber(Index * (TicksPerFrame / 2)), FMath::Cos(static_cast<float>(Index) * 0.1f));
	}
	Collection->AddColorParameterKey(TEXT("Tint"), Frame(10), FLinearColor(0.2f, 0.4f, 0.6f, 1.f));
	{
		TMovieSceneChannelData<FMovieSceneFloatValue> Glow = Collection->GetScalarParameterNamesAndCurves()[0].ParameterCurve.GetData();
		for (int32 Index = 0; Index < Glow.GetValues().Num(); Index += 5)
		{
			Glow.GetValues()[Index].Tangent.ArriveTangent = 0.3f;
			Glow.GetValues()[Index].Tangent.LeaveTangent = 0.3f;
		}
	}

	// On the door: custom primitive data, a component material parameter, particle parameters, a primitive material and
	// particle keys.
	UMovieSceneCustomPrimitiveDataTrack* Primitive = MovieScene.AddTrack<UMovieSceneCustomPrimitiveDataTrack>(Built.Door);
	Primitive->AddScalarParameterKey(0, Frame(0), 1.f);
	Primitive->AddScalarParameterKey(0, Frame(40), 3.f);
	Primitive->AddColorParameterKey(4, Frame(20), FLinearColor(1.f, 0.5f, 0.25f, 1.f));
	UMovieSceneComponentMaterialTrack* Material = MovieScene.AddTrack<UMovieSceneComponentMaterialTrack>(Built.Door);
	FComponentMaterialInfo Slot;
	Slot.MaterialSlotIndex = 0;
	Slot.MaterialType = EComponentMaterialType::IndexedMaterial;
	Material->SetMaterialInfo(Slot);
	FMaterialParameterInfo Roughness;
	Roughness.Name = TEXT("Roughness");
	Material->AddScalarParameterKey(Roughness, Frame(0), 0.25f, FString(), FString());
	Material->AddColorParameterKey(FMaterialParameterInfo(TEXT("Base")), Frame(30), FLinearColor::Green, FString(), FString());
	UMovieSceneParticleParameterTrack* ParticleParameters = MovieScene.AddTrack<UMovieSceneParticleParameterTrack>(Built.Door);
	ParticleParameters->AddVectorParameterKey(TEXT("Velocity"), Frame(10), FVector(1.0, 2.0, 3.0));
	UMovieScenePrimitiveMaterialSection* PrimitiveMaterial = Cast<UMovieScenePrimitiveMaterialSection>(
		NewSection(*MovieScene.AddTrack<UMovieScenePrimitiveMaterialTrack>(Built.Door)));
	PrimitiveMaterial->MaterialChannel.GetData().AddKey(Frame(15), FMovieSceneObjectPathChannelKeyValue(
		LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"))));
	UMovieSceneParticleSection* Particles = Cast<UMovieSceneParticleSection>(NewSection(*MovieScene.AddTrack<UMovieSceneParticleTrack>(Built.Door)));
	Particles->ParticleKeys.GetData().AddKey(Frame(5), static_cast<uint8>(EParticleKey::Activate));
	Particles->ParticleKeys.GetData().AddKey(Frame(90), static_cast<uint8>(EParticleKey::Deactivate));

	// Camera shakes on the camera: a shake section and a shake-source trigger.
	UMovieSceneCameraShakeSection* Shake = Cast<UMovieSceneCameraShakeSection>(NewSection(*MovieScene.AddTrack<UMovieSceneCameraShakeTrack>(Built.Camera), 60));
	Shake->ShakeData.PlayScale = 2.f;
	Shake->ShakeData.PlaySpace = ECameraShakePlaySpace::World;
	UMovieSceneCameraShakeSourceTriggerSection* Trigger = Cast<UMovieSceneCameraShakeSourceTriggerSection>(
		NewSection(*MovieScene.AddTrack<UMovieSceneCameraShakeSourceTriggerTrack>(Built.Camera)));
	FMovieSceneCameraShakeSourceTrigger Fire;
	Fire.PlayScale = 0.5f;
	const_cast<FMovieSceneCameraShakeSourceTriggerChannel&>(Trigger->GetChannel()).GetData().AddKey(Frame(12), Fire);
	return true;
}

template <typename FValueType, typename FChannelType>
void SameKeys(FAutomationTestBase& Test, const FString& What, const FChannelType& Editor, const FChannelType& Document, int32 Expected)
{
	const TArrayView<const FFrameNumber> TimesA = Editor.GetData().GetTimes();
	const TArrayView<const FFrameNumber> TimesB = Document.GetData().GetTimes();
	const TArrayView<const FValueType> A = Editor.GetData().GetValues();
	const TArrayView<const FValueType> B = Document.GetData().GetValues();
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: key count"), *What), B.Num(), A.Num()) || !Test.TestEqual(*FString::Printf(TEXT("%s: keys"), *What),
		A.Num(), Expected))
	{
		return;
	}
	for (int32 Index = 0; Index < A.Num(); ++Index)
	{
		if (TimesA[Index] != TimesB[Index] || A[Index].Value != B[Index].Value || A[Index].TangentMode != B[Index].TangentMode
			|| A[Index].Tangent.ArriveTangent != B[Index].Tangent.ArriveTangent || A[Index].Tangent.LeaveTangent != B[Index].Tangent.LeaveTangent)
		{
			Test.AddError(FString::Printf(TEXT("%s: key %d differs (tick %d/%d, tangents %f %f / %f %f)"), *What, Index, TimesA[Index].Value,
				TimesB[Index].Value, A[Index].Tangent.ArriveTangent, A[Index].Tangent.LeaveTangent, B[Index].Tangent.ArriveTangent,
				B[Index].Tangent.LeaveTangent));
			return;
		}
	}
}

template <typename FTrackType>
FTrackType* GlobalTrackOf(ULevelSequence& Sequence)
{
	return Sequence.GetMovieScene()->FindTrack<FTrackType>();
}

// The track of a type in a document scope (an empty object when there is none, so a failed lookup fails the test, not
// the editor).
TSharedPtr<FJsonObject> TrackOfType(const TSharedPtr<FJsonObject>& Body, const TCHAR* Field, const TCHAR* Type)
{
	const TSharedPtr<FJsonObject> Track = Named(Items(Body, Field), TEXT("Type"), Type);
	if (Track.IsValid())
	{
		return Track;
	}
	const TSharedPtr<FJsonObject> Empty = MakeShared<FJsonObject>();
	Empty->SetArrayField(TEXT("Sections"), {MakeShared<FJsonValueObject>(MakeShared<FJsonObject>())});
	return Empty;
}
} // namespace AssetDocumentSequencerApplyTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerTrackKindsTest, "UEAssetDoc.Sequencer.Apply.TrackKinds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerTrackKindsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerApplyTests;
	FFixtures Fixtures;
	FBuilt Built;
	UObject* Base = LoadedFixture(*this, Fixtures, Built, TEXT("LS_Base"));
	if (!TestNotNull(TEXT("fixture"), Base))
	{
		return false;
	}
	const FString BasePath = Base->GetPathName();
	Twin(*this, Fixtures, LoadQuiet(BasePath), TEXT("S3 track kinds"), [&Built](ULevelSequence& Sequence) { return AddTrackKinds(Sequence, Built); },
		[this](ULevelSequence& Editor, ULevelSequence& Document)
		{
			const int32 Curves = EvaluatesEqualAll(*this, TEXT("S3 track kinds"), Editor, Document);
			TestTrue(*FString::Printf(TEXT("S3 track kinds: curves evaluate equal (%d)"), Curves), Curves > 0);
			const UMovieSceneSlomoTrack* SlomoA = GlobalTrackOf<UMovieSceneSlomoTrack>(Editor);
			const UMovieSceneSlomoTrack* SlomoB = GlobalTrackOf<UMovieSceneSlomoTrack>(Document);
			if (TestTrue(TEXT("slomo on both sides"), SlomoA != nullptr && SlomoB != nullptr))
			{
				SameKeys<FMovieSceneFloatValue>(*this, TEXT("dense slomo (stated tangents of every chunk)"),
					CastChecked<UMovieSceneSlomoSection>(FirstSection(SlomoA))->FloatCurve, CastChecked<UMovieSceneSlomoSection>(FirstSection(SlomoB))->FloatCurve,
					SlomoKeys);
			}
			const UMovieSceneMaterialParameterCollectionTrack* MPCA = GlobalTrackOf<UMovieSceneMaterialParameterCollectionTrack>(Editor);
			const UMovieSceneMaterialParameterCollectionTrack* MPCB = GlobalTrackOf<UMovieSceneMaterialParameterCollectionTrack>(Document);
			if (TestTrue(TEXT("MPC on both sides"), MPCA != nullptr && MPCB != nullptr))
			{
				const UMovieSceneParameterSection* A = CastChecked<UMovieSceneParameterSection>(FirstSection(MPCA));
				const UMovieSceneParameterSection* B = CastChecked<UMovieSceneParameterSection>(FirstSection(MPCB));
				if (TestEqual(TEXT("MPC scalar parameters"), B->GetScalarParameterNamesAndCurves().Num(), 1))
				{
					TestEqual(TEXT("MPC parameter name"), B->GetScalarParameterNamesAndCurves()[0].ParameterName, FName(TEXT("Glow")));
					SameKeys<FMovieSceneFloatValue>(*this, TEXT("dense MPC parameter"), A->GetScalarParameterNamesAndCurves()[0].ParameterCurve,
						B->GetScalarParameterNamesAndCurves()[0].ParameterCurve, GlowKeys);
				}
			}
			const UMovieSceneTimeWarpTrack* Warp = GlobalTrackOf<UMovieSceneTimeWarpTrack>(Document);
			const UMovieSceneTimeWarpSection* WarpSection = Warp != nullptr ? Cast<UMovieSceneTimeWarpSection>(FirstSection(Warp)) : nullptr;
			const UMovieScenePlayRateCurve* Curve = WarpSection != nullptr && WarpSection->TimeWarp.GetType() == EMovieSceneTimeWarpType::Custom
				? Cast<UMovieScenePlayRateCurve>(WarpSection->TimeWarp.AsCustom()) : nullptr;
			if (TestNotNull(TEXT("time warp: a play-rate curve owned by the section"), Curve))
			{
				TestTrue(TEXT("time warp: the curve is owned by the section"), Curve->GetOuter() == WarpSection);
				TestEqual(TEXT("time warp: play-rate keys"), Curve->PlayRate.GetNumKeys(), 3);
			}
			const UMovieSceneCVarTrack* CVars = GlobalTrackOf<UMovieSceneCVarTrack>(Document);
			const UMovieSceneGroupCondition* Group = CVars != nullptr ? Cast<UMovieSceneGroupCondition>(CVars->ConditionContainer.Condition) : nullptr;
			if (TestNotNull(TEXT("conditions: the track's group condition"), Group))
			{
				TestEqual(TEXT("conditions: two sub-conditions"), Group->SubConditions.Num(), 2);
				TestTrue(TEXT("conditions: sub-conditions owned by the group"), Group->SubConditions.Num() == 2 && Group->SubConditions[1].Condition != nullptr
					&& Group->SubConditions[1].Condition->GetOuter() == Group);
			}
			const UMovieSceneLevelVisibilityTrack* Levels = GlobalTrackOf<UMovieSceneLevelVisibilityTrack>(Document);
			const FMovieSceneTrackRowMetadata* Row = Levels != nullptr ? Levels->FindTrackRowMetadata(0) : nullptr;
			TestTrue(TEXT("conditions: the row condition"), Row != nullptr && Cast<UMovieScenePlatformCondition>(Row->ConditionContainer.Condition) != nullptr);
		});

	// Document edits on the editor side's result: parameters, conditions and the time warp.
	const FTarget& Edited = Fixtures.Add(TEXT("LS_S3Edits"));
	ULevelSequence* Copy = Duplicate(*this, LoadQuiet(BasePath), Edited);
	if (Copy == nullptr || !TestTrue(TEXT("track kinds added"), AddTrackKinds(*Copy, Built)) || !TestTrue(TEXT("saved"), Save(*Copy)))
	{
		return false;
	}
	DocEdit(*this, Edited, TEXT("parameters"), [](const TSharedPtr<FJsonObject>& Body)
	{
		const TSharedPtr<FJsonObject> Section = Items(TrackOfType(Body, TEXT("Tracks"), TEXT("MaterialParameterCollection")),
			TEXT("Sections"))[0];
		TArray<TSharedPtr<FJsonValue>>& Entries = ArrayOf(Section, TEXT("Parameters"));
		if (Entries.IsEmpty())
		{
			return;
		}
		Entries[0]->AsObject()->SetStringField(TEXT("Name"), TEXT("Bloom"));
		Entries.Insert(MakeShared<FJsonValueObject>(Parse(TEXT(R"({"Type": "Scalar", "Name": "Fog", "Channels": {"ParameterCurve": {"Keys": [{"Frame": 0, "Value": 2}, {"Frame": 30, "Value": 4, "Interp": "Linear"}]}}})"))), 1);
	});
	DocEdit(*this, Edited, TEXT("conditions"), [](const TSharedPtr<FJsonObject>& Body)
	{
		const TSharedPtr<FJsonObject> CVars = TrackOfType(Body, TEXT("Tracks"), TEXT("CVar"));
		const TSharedPtr<FJsonObject> Properties = Child(CVars, TEXT("Properties"));
		const TSharedPtr<FJsonObject> Group = Properties.IsValid() ? Child(Properties, TEXT("ConditionContainer")) : nullptr;
		if (!Group.IsValid())
		{
			return;
		}
		Group->SetObjectField(TEXT("Condition"), Parse(TEXT(R"({"Class": "/Script/MovieSceneTracks.MovieScenePlatformCondition", "Properties": {"ValidPlatforms": ["IOS"]}})")));
		const TSharedPtr<FJsonObject> Levels = TrackOfType(Body, TEXT("Tracks"), TEXT("LevelVisibility"));
		Levels->RemoveField(TEXT("RowConditions"));
	});
	if (ULevelSequence* Sequence = Edited.GetSequence())
	{
		const UMovieSceneCVarTrack* CVars = GlobalTrackOf<UMovieSceneCVarTrack>(*Sequence);
		const UMovieScenePlatformCondition* Platform = CVars != nullptr ? Cast<UMovieScenePlatformCondition>(CVars->ConditionContainer.Condition) : nullptr;
		TestTrue(TEXT("conditions: the group became a platform condition"), Platform != nullptr && Platform->ValidPlatforms == TArray<FName>{TEXT("IOS")});
		const UMovieSceneLevelVisibilityTrack* Levels = GlobalTrackOf<UMovieSceneLevelVisibilityTrack>(*Sequence);
		TestTrue(TEXT("conditions: the row condition is removed"), Levels != nullptr && (Levels->FindTrackRowMetadata(0) == nullptr
			|| Levels->FindTrackRowMetadata(0)->ConditionContainer.Condition == nullptr));
		const UMovieSceneMaterialParameterCollectionTrack* MPC = GlobalTrackOf<UMovieSceneMaterialParameterCollectionTrack>(*Sequence);
		const UMovieSceneParameterSection* MPCSection = MPC != nullptr ? Cast<UMovieSceneParameterSection>(FirstSection(MPC)) : nullptr;
		TestTrue(TEXT("parameters: renamed and added"), MPCSection != nullptr && MPCSection->GetScalarParameterNamesAndCurves().Num() == 2
			&& MPCSection->GetScalarParameterNamesAndCurves()[0].ParameterName == TEXT("Bloom")
			&& MPCSection->GetScalarParameterNamesAndCurves()[1].ParameterName == TEXT("Fog")
			&& MPCSection->GetScalarParameterNamesAndCurves()[0].ParameterCurve.GetNumKeys() == GlowKeys);
	}
	DocEdit(*this, Edited, TEXT("fixed time warp"), [](const TSharedPtr<FJsonObject>& Body)
	{
		const TSharedPtr<FJsonObject> Section = Items(TrackOfType(Body, TEXT("Tracks"), TEXT("TimeWarp")), TEXT("Sections"))[0];
		if (const TSharedPtr<FJsonObject> Properties = Child(Section, TEXT("Properties")))
		{
			Properties->SetObjectField(TEXT("TimeWarp"), Parse(TEXT(R"({"PlayRate": 2})")));
		}
		Section->RemoveField(TEXT("Channels"));
	});
	if (ULevelSequence* Sequence = Edited.GetSequence())
	{
		const UMovieSceneTimeWarpTrack* Warp = GlobalTrackOf<UMovieSceneTimeWarpTrack>(*Sequence);
		const UMovieSceneTimeWarpSection* Section = Warp != nullptr ? Cast<UMovieSceneTimeWarpSection>(FirstSection(Warp)) : nullptr;
		TestTrue(TEXT("time warp: fixed play rate 2"), Section != nullptr && Section->TimeWarp.GetType() == EMovieSceneTimeWarpType::FixedPlayRate
			&& Section->TimeWarp.AsFixedPlayRate() == 2.0);
	}
	// A custom primitive data parameter is named by its first float index: another name is refused (the engine asserts).
	{
		const TSharedPtr<FJsonObject> Document = ForTarget(Extract(*this, Edited.ObjectPath, TEXT("CPD before")), Edited, TEXT("Update"));
		const FString Before = WithoutTarget(Document);
		const TSharedPtr<FJsonObject> Door = RootBinding(BodyOf(Document), TEXT("Door"));
		const TSharedPtr<FJsonObject> Section = Items(TrackOfType(Door, TEXT("Tracks"), TEXT("CustomPrimitiveData")),
			TEXT("Sections"))[0];
		TArray<TSharedPtr<FJsonValue>>& Entries = ArrayOf(Section, TEXT("Parameters"));
		if (!TestFalse(TEXT("CPD parameters read"), Entries.IsEmpty()))
		{
			return false;
		}
		Entries[0]->AsObject()->SetStringField(TEXT("Name"), TEXT("35x"));
		const FAssetDocumentResult Refused = Apply(Edited, Document);
		TestFalse(TEXT("CPD: a non-index name is refused"), Refused.IsSuccess());
		TestTrue(*FString::Printf(TEXT("CPD: Sequencer.InvalidValue at the name (%s)"), *DiagnosticsText(Refused)), Refused.Diagnostics.ContainsByPredicate(
			[](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("Sequencer.InvalidValue") && Diagnostic.Path.EndsWith(TEXT("/Parameters/0/Name")); }));
		TestEqual(TEXT("CPD: unchanged"), WithoutTarget(Extract(*this, Edited.ObjectPath, TEXT("CPD after"))), Before);
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerPreservedTest, "UEAssetDoc.Sequencer.Apply.Preserved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerPreservedTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerApplyTests;
	FFixtures Fixtures;
	FBuilt Built;
	UObject* Base = LoadedFixture(*this, Fixtures, Built, TEXT("LS_Base"));
	if (!TestNotNull(TEXT("fixture"), Base))
	{
		return false;
	}
	const FTarget& Target = Fixtures.Add(TEXT("LS_Preserved"));
	ULevelSequence* Sequence = Duplicate(*this, Base, Target);
	if (Sequence == nullptr)
	{
		return false;
	}
	// Section group (the section context menu's Group), node group (the group filter) and a camera cut bound into a
	// sub-sequence (a remote binding).
	FName Overridden;
	{
		UMovieScene& MovieScene = *Sequence->GetMovieScene();
		UMovieSceneSection* Transform = TransformSection(*Sequence, Built.Camera);
		UMovieSceneSection* Fade = FirstSection(MovieScene.FindTrack<UMovieSceneFadeTrack>());
		if (!TestTrue(TEXT("sections to group"), Transform != nullptr && Fade != nullptr))
		{
			return false;
		}
		MovieScene.GroupSections({Transform, Fade});
		UMovieSceneNodeGroup* Group = NewObject<UMovieSceneNodeGroup>(&MovieScene.GetNodeGroups(), NAME_None, RF_Transactional);
		Group->SetName(TEXT("Hero"));
		Group->AddNode(TEXT("ShotCam"));
		MovieScene.GetNodeGroups().AddNodeGroup(Group);
		UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(FirstSection(MovieScene.GetCameraCutTrack()));
		if (!TestNotNull(TEXT("camera cut"), Cut))
		{
			return false;
		}
		Cut->SetCameraBindingID(UE::MovieScene::FRelativeObjectBindingID(FGuid(1, 2, 3, 4), FMovieSceneSequenceID(77), 0));
		// The channel context menu's "Override With > Double Perlin Noise" on Location.X (FChannelGroupOverrideHelper).
		IMovieSceneChannelOverrideProvider* Provider = Cast<IMovieSceneChannelOverrideProvider>(Transform);
		const TArrayView<const FMovieSceneChannelMetaData> MetaData = Transform->GetChannelProxy().GetMetaData<FMovieSceneDoubleChannel>();
		FMovieSceneDoubleChannel* LocationX = DoubleChannel(Transform, 0);
		if (!TestTrue(TEXT("override provider"), Provider != nullptr && MetaData.Num() > 0 && LocationX != nullptr))
		{
			return false;
		}
		UMovieSceneSectionChannelOverrideRegistry* Registry = Provider->GetChannelOverrideRegistry(true);
		UMovieSceneDoublePerlinNoiseChannelContainer* Noise = NewObject<UMovieSceneDoublePerlinNoiseChannelContainer>(Registry, NAME_None, RF_Transactional);
		Noise->InitializeOverride(LocationX);
		Overridden = MetaData[0].Name;
		Registry->AddChannel(Overridden, Noise);
		Provider->OnChannelOverridesChanged();
		MovieScene.MarkAsChanged();
		TestTrue(TEXT("saved"), Save(*Sequence));
	}
	DocEdit(*this, Target, TEXT("an unrelated edit"), [](const TSharedPtr<FJsonObject>& Body)
	{
		Append(Body, TEXT("Markers"), TEXT(R"({"Frame": 100, "Label": "Late"})"));
		const TSharedPtr<FJsonObject> Fade = Items(GlobalTrack(Body, TEXT("Fade")), TEXT("Sections"))[0];
		Fade->SetNumberField(TEXT("End"), 140);
		// The overridden section is rewritten too (section.set).
		const TSharedPtr<FJsonObject> Transform = Items(TrackOfType(RootBinding(Body, TEXT("ShotCam")), TEXT("Tracks"), TEXT("Transform")), TEXT("Sections"))[0];
		Transform->SetNumberField(TEXT("OverlapPriority"), 5);
	});
	ULevelSequence* After = Target.GetSequence();
	if (!TestNotNull(TEXT("after"), After))
	{
		return false;
	}
	UMovieScene& MovieScene = *After->GetMovieScene();
	UMovieSceneSection* Transform = TransformSection(*After, Built.Camera);
	UMovieSceneSection* Fade = FirstSection(MovieScene.FindTrack<UMovieSceneFadeTrack>());
	TestTrue(TEXT("the section group is kept"), Transform != nullptr && Fade != nullptr && MovieScene.IsSectionInGroup(*Transform)
		&& MovieScene.IsSectionInGroup(*Fade));
	TestEqual(TEXT("the node group is kept"), MovieScene.GetNodeGroups().Num(), 1);
	IMovieSceneChannelOverrideProvider* Provider = Cast<IMovieSceneChannelOverrideProvider>(Transform);
	const UMovieSceneSectionChannelOverrideRegistry* Registry = Provider != nullptr ? Provider->GetChannelOverrideRegistry(false) : nullptr;
	TestTrue(TEXT("the transform section was rewritten"), Transform != nullptr && Transform->GetOverlapPriority() == 5);
	TestTrue(TEXT("the channel override registry is kept"), Registry != nullptr && Registry->NumChannels() == 1);
	TestTrue(*FString::Printf(TEXT("the channel override of %s is kept"), *Overridden.ToString()), Registry != nullptr
		&& Cast<UMovieSceneDoublePerlinNoiseChannelContainer>(Registry->GetChannel(Overridden)) != nullptr);
	const UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(FirstSection(MovieScene.GetCameraCutTrack()));
	TestTrue(TEXT("the remote binding is kept"), Cut != nullptr && Cut->GetCameraBindingID().GetGuid() == FGuid(1, 2, 3, 4)
		&& Cut->GetCameraBindingID().GetRelativeSequenceID() == FMovieSceneSequenceID(77));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerDependentsBlueprintTest, "UEAssetDoc.Sequencer.Apply.DependentsBlueprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerDependentsBlueprintTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerApplyTests;
	FFixtures Fixtures;
	FBuilt Built;
	UObject* Base = LoadedFixture(*this, Fixtures, Built, TEXT("LS_Base"));
	if (!TestNotNull(TEXT("fixture"), Base))
	{
		return false;
	}
	const FTarget& Target = Fixtures.Add(TEXT("LS_Bound"));
	ULevelSequence* Sequence = Duplicate(*this, Base, Target);
	if (Sequence == nullptr)
	{
		return false;
	}
	// A Blueprint with a "Get Sequence Binding" node naming the door (the node's sequence picker and binding menu).
	const FTarget& BlueprintTarget = Fixtures.Add(TEXT("BP_UsesBinding"));
	UPackage* Package = CreatePackage(*BlueprintTarget.PackageName);
	const TStrongObjectPtr<UBlueprint> Blueprint(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, *BlueprintTarget.AssetName,
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
	UEdGraph* Graph = Blueprint.IsValid() ? FBlueprintEditorUtils::FindEventGraph(Blueprint.Get()) : nullptr;
	if (!TestNotNull(TEXT("Blueprint event graph"), Graph))
	{
		return false;
	}
	UK2Node_GetSequenceBinding* Node = NewObject<UK2Node_GetSequenceBinding>(Graph);
	Graph->AddNode(Node, false, false);
	Node->CreateNewGuid();
	Node->SetSequence(Sequence);
	if (const FStructProperty* Binding = FindFProperty<FStructProperty>(UK2Node_GetSequenceBinding::StaticClass(), TEXT("Binding")))
	{
		*Binding->ContainerPtrToValuePtr<FMovieSceneObjectBindingID>(Node) = UE::MovieScene::FRelativeObjectBindingID(Built.Door);
	}
	Node->AllocateDefaultPins();
	FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
	TestTrue(TEXT("Blueprint saved"), Save(*Blueprint));
	FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({BlueprintTarget.PackageFile});

	FAssetDocumentResult Deleted;
	DocEdit(*this, Target, TEXT("delete the door"), [](const TSharedPtr<FJsonObject>& Body)
	{
		ArrayOf(Body, TEXT("Bindings")).RemoveAll([](const TSharedPtr<FJsonValue>& Binding) { return Binding->AsObject()->GetStringField(TEXT("Name")) == TEXT("Door"); });
	}, &Deleted);
	const FAssetDocumentDiagnostic* Warning = Deleted.Diagnostics.FindByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
		{ return Diagnostic.Code == TEXT("Sequencer.DependentsAffected"); });
	TestTrue(*FString::Printf(TEXT("the warning names the Blueprint and the binding (%s)"), *DiagnosticsText(Deleted)), Warning != nullptr
		&& Warning->Message.Contains(BlueprintTarget.PackageName) && Warning->Message.Contains(TEXT("Door")));
	TestFalse(TEXT("the Blueprint is not dirtied"), Blueprint->GetOutermost()->IsDirty());
	return true;
}


// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerCineCameraSpawnableTest, "UEAssetDoc.Sequencer.Apply.CineCameraSpawnable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerCineCameraSpawnableTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerApplyTests;
	FFixtures Fixtures;
	// The profile section's example camera (a Cine Camera spawnable with a template Details edit), the same camera
	// without Template, and a possessed camera converted to a Cine Camera spawnable: each applies (the post-save check
	// passes), Diff is empty and the extract after reload lists the document's template.
	const TCHAR* const ExampleCamera = TEXT(R"json({"Name": "ShotCam",
		"Spawnable": {"Class": "/Script/CinematicCamera.CineCameraActor", "Template": {"Components": {"CameraComponent": {"CurrentAperture": 4}}}},
		"Tracks": [{"Type": "Spawn", "Sections": [{"Channels": {"Value": {"Default": true}}}]},
			{"Type": "Transform", "Sections": [{"Start": 0, "End": 150, "Channels": {
				"Location.X": {"Keys": [{"Frame": 0, "Value": -600}, {"Frame": 150, "Value": -250, "Interp": "Linear"}]},
				"Location.Z": {"Default": 160}}}]}]})json");
	const TCHAR* const PlainCamera = TEXT(R"json({"Name": "ShotCam", "Spawnable": {"Class": "/Script/CinematicCamera.CineCameraActor"}})json");
	const TCHAR* const PossessedCamera = TEXT(R"json({"Name": "ShotCam", "Class": "/Script/CinematicCamera.CineCameraActor", "Bind": []})json");
	const auto Document = [](const FTarget& Target, const TCHAR* Action, const TCHAR* Binding)
	{
		return Parse(FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Class": "%s", "Action": "%s", "Target": "%s", "Properties": {},
			"Body": {"Settings": {"PlaybackRange": {"Start": 0, "End": 150}}, "Bindings": [%s],
			"Tracks": [{"Type": "CameraCut", "Sections": [{"Start": 0, "End": 150, "Properties": {"CameraBindingID": {"Binding": "ShotCam"}}}]}]}})json"),
			LevelSequenceClass, Action, *Target.PackageName, Binding));
	};
	const auto Check = [this](const FTarget& Target, const TSharedPtr<FJsonObject>& Applied, const FString& Stage, const FString& Aperture)
	{
		FAssetDocumentResult Result;
		if (!ApplyOk(*this, Target, Applied, Stage, &Result))
		{
			return;
		}
		FString DiffText;
		const int32 Changes = DiffCount(Applied, DiffText);
		if (!TestEqual(*FString::Printf(TEXT("%s: no difference to the document"), *Stage), Changes, 0))
		{
			AddInfo(DiffText.Left(1500));
		}
		TestNotNull(*FString::Printf(TEXT("%s: reload"), *Stage), Reload(Target.ObjectPath));
		const TSharedPtr<FJsonObject> Camera = RootBinding(BodyOf(Extract(*this, Target.ObjectPath, Stage)), TEXT("ShotCam"));
		const FString Template = Serialize(Child(Child(Camera, TEXT("Spawnable")), TEXT("Template")));
		TestEqual(*FString::Printf(TEXT("%s: the template after reload"), *Stage), Template,
			Aperture.IsEmpty() ? FString() : FString::Printf(TEXT("{\"Components\":{\"CameraComponent\":{\"CurrentAperture\":%s}}}"), *Aperture));
		TestEqual(*FString::Printf(TEXT("%s: status"), *Stage), StatusName(ExtractResult(Target.ObjectPath)), FString(TEXT("UpToDate")));
	};

	const FTarget& Example = Fixtures.Add(TEXT("LS_CineExample"));
	Check(Example, Document(Example, TEXT("Create"), ExampleCamera), TEXT("the section's example"), TEXT("4.0"));
	const FTarget& Plain = Fixtures.Add(TEXT("LS_CinePlain"));
	Check(Plain, Document(Plain, TEXT("Create"), PlainCamera), TEXT("a Cine Camera spawnable without Template"), FString());
	const FTarget& Converted = Fixtures.Add(TEXT("LS_CineConverted"));
	if (ApplyOk(*this, Converted, Document(Converted, TEXT("Create"), PossessedCamera), TEXT("a possessed Cine Camera")))
	{
		Check(Converted, Document(Converted, TEXT("Update"), PlainCamera), TEXT("possessable to Cine Camera spawnable"), FString());
	}
	return true;
}


// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerOmittedSettingsTest, "UEAssetDoc.Sequencer.Apply.OmittedSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerOmittedSettingsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerApplyTests;
	FFixtures Fixtures;
	FBuilt Built;
	UObject* Base = LoadedFixture(*this, Fixtures, Built, TEXT("LS_Base"));
	if (!TestNotNull(TEXT("fixture"), Base))
	{
		return false;
	}
	const FTarget& Target = Fixtures.Add(TEXT("LS_Omitted"));
	ULevelSequence* Sequence = Duplicate(*this, Base, Target);
	if (Sequence == nullptr)
	{
		return false;
	}
	{
		UMovieScene& MovieScene = *Sequence->GetMovieScene();
		MovieScene.SetPlaybackRangeLocked(true);
		MovieScene.SetMarkedFramesLocked(true);
		MovieScene.SetGloballyShowMarkedFrames(true);
		UMovieSceneMetaData* MetaData = Sequence->FindOrAddMetaData<UMovieSceneMetaData>();
		MetaData->SetAuthor(TEXT("Someone"));
		MetaData->SetNotes(TEXT("Some notes"));
		MovieScene.MarkAsChanged();
		TestTrue(TEXT("saved"), Save(*Sequence));
	}
	const TSharedPtr<FJsonObject> Before = BodyOf(Extract(*this, Target.ObjectPath, TEXT("before")));
	if (!TestTrue(TEXT("the extract lists the flags and the metadata"), Before.IsValid() && Before->GetObjectField(TEXT("Settings"))->HasField(TEXT("PlaybackRangeLocked"))
		&& Before->HasField(TEXT("MetaData"))))
	{
		return false;
	}
	// Deleting the fields from the extract: Diff used to say nothing and the apply kept them.
	DocEdit(*this, Target, TEXT("delete flags and notes"), [](const TSharedPtr<FJsonObject>& Body)
	{
		const TSharedPtr<FJsonObject> Settings = Body->GetObjectField(TEXT("Settings"));
		Settings->RemoveField(TEXT("PlaybackRangeLocked"));
		Settings->RemoveField(TEXT("MarkersLocked"));
		Body->GetObjectField(TEXT("MetaData"))->RemoveField(TEXT("Notes"));
	});
	ULevelSequence* After = Target.GetSequence();
	if (!TestNotNull(TEXT("after"), After))
	{
		return false;
	}
	TestFalse(TEXT("the playback range is unlocked"), After->GetMovieScene()->IsPlaybackRangeLocked());
	TestFalse(TEXT("the markers are unlocked"), After->GetMovieScene()->AreMarkedFramesLocked());
	TestTrue(TEXT("a listed flag is kept"), After->GetMovieScene()->GetGloballyShowMarkedFrames());
	const UMovieSceneMetaData* MetaData = After->FindMetaData<UMovieSceneMetaData>();
	TestTrue(TEXT("the notes are cleared, the author kept"), MetaData != nullptr && MetaData->GetNotes().IsEmpty() && MetaData->GetAuthor() == TEXT("Someone"));

	// A Settings that lists only PlaybackRange keeps the rates: no difference, and Diff says the omitted ones are kept.
	const TSharedPtr<FJsonObject> Document = ForTarget(Extract(*this, Target.ObjectPath, TEXT("partial")), Target, TEXT("Update"));
	const TSharedPtr<FJsonObject> Settings = BodyOf(Document)->GetObjectField(TEXT("Settings"));
	for (const TCHAR* Kept : {TEXT("DisplayRate"), TEXT("TickResolution"), TEXT("Evaluation")})
	{
		Settings->RemoveField(Kept);
	}
	FAssetDocumentDiffRequest Request;
	Request.Document = Document;
	const FAssetDocumentResult Diff = FAssetDocumentModule::Get().GetService().Diff(Request);
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	TestTrue(*FString::Printf(TEXT("the omitted timing fields are no difference (%s)"), *DiagnosticsText(Diff)), Diff.IsSuccess() && Diff.Payload.IsValid()
		&& Diff.Payload->TryGetArrayField(TEXT("changed"), Changed) && Changed->IsEmpty());
	TestTrue(TEXT("Diff says they are kept (Sequencer.OmittedKept)"), HasCode(Diff, TEXT("Sequencer.OmittedKept")));
	return true;
}

#endif
