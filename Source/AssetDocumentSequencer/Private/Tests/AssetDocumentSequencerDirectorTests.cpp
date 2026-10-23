// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Sequencer director (design §3.6, §7.2): the Director Blueprint, event keys, director bindings, section / node groups and
// binding references into sub-sequences.
// UEAssetDoc.Sequencer.Director.Twin: the editor's own director work on a duplicate of the fixture
//   ("Open Director Blueprint", "Create Quick Binding" on a key of a binding's event track, a function endpoint created
//   for a key of a global event track with a payload, a "Replaceable from Director Blueprint" binding with its endpoint,
//   compile) and its extract applied to a second duplicate without a Director: equal extract, equal after reload; the
//   document side's keys and binding call its own Director, which compiles.
// UEAssetDoc.Sequencer.Director.FreeEdits: §6.3 "Director / events" as document edits: a Director with a
//   function endpoint and an event key calling it with a bound object pin and a payload; renaming the function in the same
//   document (the key follows the same node); deleting the endpoint while a key uses it (refused, unchanged); a director
//   compile error (rolled back); "Director": null (refused while used, then removed with the keys).
// UEAssetDoc.Sequencer.Director.InvalidPayload: a payload text that is not a
//   value of the endpoint input's type ("wide" for a float) is refused by validate and by apply (Sequencer.InvalidPayload at
//   the payload value, naming the input, its type and the text), for an event key of an existing Director, for a key
//   written together with its Director, and for a director binding's payload; nothing changes.
// UEAssetDoc.Sequencer.Director.Rollback: a Director created by a failing apply is gone afterwards; a failed
//   edit of an existing Director leaves it (and its compiled class) as before; no undo history references the sequence.
// UEAssetDoc.Sequencer.Director.Undo: undo / redo after a Director edit and after a rolled back one.
// UEAssetDoc.Sequencer.Groups: section groups ("Group" on selected sections) and node groups (the Group
//   Manager) as an editor twin, then document edits (regroup, ungroup, add / rename / delete node groups).
// UEAssetDoc.Sequencer.RemoteBinding: a camera cut to the camera of a shot ("Sequence" + "Binding") as an
//   editor twin, and created together with the shot in one document.
// UEAssetDoc.Sequencer.FromScratch: §6.4 as a scripted run: an intro (two spawned cine cameras, dolly,
//   focus pull, camera cuts, fade, markers, a possessable sun, an event at the last frame calling a Director function
//   that prints "Intro done"), an outro and a shot master with a camera cut into the intro's camera, then the three
//   follow-ups (dolly ease-out, camera shake on the reverse shot, the outro two seconds later).

#include "AssetDocumentSequencerTestSupport.h"

#include "Bindings/MovieSceneReplaceableDirectorBlueprintBinding.h"
#include "Channels/MovieSceneEvent.h"
#include "Channels/MovieSceneEventChannel.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MovieScene.h"
#include "MovieSceneBindingReferences.h"
#include "MovieSceneDirectorBlueprintUtils.h"
#include "MovieSceneDynamicBinding.h"
#include "MovieSceneDynamicBindingUtils.h"
#include "MovieSceneEventUtils.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieSceneSequenceEditor.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ScopedTransaction.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneCinematicShotSection.h"
#include "Sections/MovieSceneEventTriggerSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneCinematicShotTrack.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Tracks/MovieSceneFadeTrack.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

// The document-test helpers of AssetDocumentSequencerApplyTests.cpp.
namespace AssetDocumentSequencerApplyTests
{
using namespace AssetDocumentSequencerTests;
TSharedPtr<FJsonObject> ForTarget(const TSharedPtr<FJsonObject>& Document, const FTarget& Target, const TCHAR* Action);
FString WithoutTarget(const TSharedPtr<FJsonObject>& Document);
bool ExpectSame(FAutomationTestBase& Test, const FString& What, const FString& Expected, const FString& Got);
bool ApplyOk(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const FString& Stage,
	FAssetDocumentResult* OutResult);
FString WarningsOf(const FAssetDocumentResult& Result);
ULevelSequence* Duplicate(FAutomationTestBase& Test, UObject* Source, const FTarget& Target);
bool DocEdit(FAutomationTestBase& Test, const FTarget& Target, const FString& Name, TFunctionRef<void(const TSharedPtr<FJsonObject>&)> Mutate,
	FAssetDocumentResult* OutResult);
bool Twin(FAutomationTestBase& Test, FFixtures& Fixtures, UObject* Source, const FString& Name, TFunctionRef<bool(ULevelSequence&)> EngineEdit,
	TFunction<void(ULevelSequence&, ULevelSequence&)> Compare);
UMovieSceneSection* FirstSection(const UMovieSceneTrack* Track);
TSharedPtr<FJsonObject> RootBinding(const TSharedPtr<FJsonObject>& Body, const TCHAR* Name);
TSharedPtr<FJsonObject> GlobalTrack(const TSharedPtr<FJsonObject>& Body, const TCHAR* Type);
TArray<TSharedPtr<FJsonValue>>& ArrayOf(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field);
void Append(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const TCHAR* Json);
UObject* LoadedFixture(FAutomationTestBase& Test, FFixtures& Fixtures, FBuilt& Built, const TCHAR* Prefix);
} // namespace AssetDocumentSequencerApplyTests

namespace AssetDocumentSequencerDirectorTests
{
using namespace AssetDocumentSequencerTests;
using namespace AssetDocumentSequencerApplyTests;

// The first object of a list, or an empty object (the test then fails at its check instead of crashing).
TSharedPtr<FJsonObject> FirstItem(const TArray<TSharedPtr<FJsonObject>>& List)
{
	return List.IsEmpty() || !List[0].IsValid() ? MakeShared<FJsonObject>() : List[0];
}

UBlueprint* DirectorOf(const ULevelSequence* Sequence)
{
	return Sequence != nullptr ? Sequence->GetDirectorBlueprint() : nullptr;
}

// Sequencer's "Open Director Blueprint" (creates it when missing).
UBlueprint* OpenDirector(ULevelSequence& Sequence)
{
	FMovieSceneSequenceEditor* Editor = FMovieSceneSequenceEditor::Find(&Sequence);
	return Editor != nullptr ? Editor->GetOrCreateDirectorBlueprint(&Sequence) : nullptr;
}

FAssetDocumentResult ValidateDocument(const TSharedPtr<FJsonObject>& Document)
{
	FAssetDocumentValidateRequest Request;
	Request.Document = Document;
	return FAssetDocumentModule::Get().GetService().Validate(Request);
}

UMovieSceneEventTriggerSection* AddTriggerSection(UMovieSceneEventTrack& Track)
{
	return Cast<UMovieSceneEventTriggerSection>(AddSection(Track, 0, 150));
}

FMovieSceneEvent* AddEventKey(UMovieSceneEventTriggerSection& Section, int32 DisplayFrame)
{
	TMovieSceneChannelData<FMovieSceneEvent> Data = Section.EventChannel.GetData();
	const int32 Index = Data.AddKey(Frame(DisplayFrame), FMovieSceneEvent());
	return &Data.GetValues()[Index];
}

const FMovieSceneEvent* FirstEvent(const UMovieSceneSection* Section)
{
	const UMovieSceneEventTriggerSection* Trigger = Cast<UMovieSceneEventTriggerSection>(Section);
	if (Trigger == nullptr)
	{
		return nullptr;
	}
	const TArrayView<const FMovieSceneEvent> Values = Trigger->EventChannel.GetData().GetValues();
	return Values.IsEmpty() ? nullptr : &Values[0];
}

FString EndpointName(const FMovieSceneEvent* Event)
{
	if (Event == nullptr)
	{
		return TEXT("<no key>");
	}
	const UObject* Endpoint = Event->WeakEndpoint.Get();
	if (const UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(Endpoint))
	{
		return Custom->CustomFunctionName.ToString();
	}
	if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Endpoint))
	{
		return Entry->GetGraph() != nullptr ? Entry->GetGraph()->GetName() : FString();
	}
	return TEXT("<none>");
}

bool IsInBlueprint(const UObject* Node, const UBlueprint* Blueprint)
{
	return Node != nullptr && Blueprint != nullptr && Node->IsIn(Blueprint);
}

FString StatusText(const UBlueprint* Blueprint)
{
	if (Blueprint == nullptr)
	{
		return TEXT("no director");
	}
	switch (Blueprint->Status)
	{
	case BS_UpToDate:
		return TEXT("UpToDate");
	case BS_UpToDateWithWarnings:
		return TEXT("UpToDateWithWarnings");
	case BS_Error:
		return TEXT("Error");
	case BS_Dirty:
		return TEXT("Dirty");
	default:
		return TEXT("Unknown");
	}
}

bool Compiled(const UBlueprint* Blueprint)
{
	return Blueprint != nullptr && (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
}

TSharedPtr<FJsonObject> BindingTrack(const TSharedPtr<FJsonObject>& Binding, const TCHAR* Type)
{
	return Named(Items(Binding, TEXT("Tracks")), TEXT("Type"), Type);
}

UMovieSceneEventTrack* EventTrackOf(ULevelSequence& Sequence, const FGuid& Binding)
{
	return Sequence.GetMovieScene()->FindTrack<UMovieSceneEventTrack>(Binding);
}

UMovieSceneReplaceableDirectorBlueprintBinding* DirectorBindingOf(ULevelSequence& Sequence, const FGuid& Binding)
{
	const FMovieSceneBindingReferences* References = Sequence.GetBindingReferences();
	if (References == nullptr)
	{
		return nullptr;
	}
	for (const FMovieSceneBindingReference& Reference : References->GetReferences(Binding))
	{
		if (UMovieSceneReplaceableDirectorBlueprintBinding* Custom = Cast<UMovieSceneReplaceableDirectorBlueprintBinding>(Reference.CustomBinding.Get()))
		{
			return Custom;
		}
	}
	return nullptr;
}

// The node group names and the outliner paths of their members, sorted ("Name: path path").
FString NodeGroupsText(UMovieScene& MovieScene)
{
	TArray<FString> Out;
	for (UMovieSceneNodeGroup* Group : MovieScene.GetNodeGroups())
	{
		if (Group == nullptr)
		{
			continue;
		}
		TArray<FString> Nodes(Group->GetNodes());
		Nodes.Sort();
		Out.Add(Group->GetName().ToString() + TEXT(": ") + FString::Join(Nodes, TEXT(" ")));
	}
	return FString::Join(Out, TEXT(" | "));
}

// The Director part of the documents below: a variable and a function endpoint (Door, Amount) that prints.
const TCHAR* const OpenDoorDirector = TEXT(R"json({
	"Variables": [{"Name": "Speed", "Type": "float", "DefaultValue": 2.0}],
	"FunctionGraphs": [{"Name": "OpenDoor",
		"Signature": {"Inputs": [{"Name": "Door", "Type": "object:/Script/Engine.Actor"}, {"Name": "Amount", "Type": "float"}]},
		"Nodes": [{"Id": "Say", "Action": "K2:PrintString", "PinOverrides": [{"Pin": "InString", "DefaultValue": "open"}]}],
		"Links": [{"From": "Entry:then", "To": "Say:execute"}]}]})json");

const TCHAR* const OpenDoorTrack = TEXT(R"json({"Type": "Event", "Sections": [{"Class": "Trigger", "Start": 0, "End": 150,
	"Channels": {"Value": {"Keys": [{"Frame": 30, "Event": {"Endpoint": "OpenDoor", "BoundObjectPin": "Door", "Payload": {"Amount": "2.5"}}}]}}}]})json");
} // namespace AssetDocumentSequencerDirectorTests

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerDirectorTwinTest, "UEAssetDoc.Sequencer.Director.Twin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerDirectorTwinTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerDirectorTests;
	FFixtures Fixtures;
	FBuilt Built;
	UObject* Base = LoadedFixture(*this, Fixtures, Built, TEXT("LS_Base"));
	if (!TestNotNull(TEXT("fixture"), Base))
	{
		return false;
	}
	const FGuid Door = Built.Door;
	const bool bSame = Twin(*this, Fixtures, Base, TEXT("director, event keys and a director binding"), [this, Door](ULevelSequence& Sequence)
		{
			UMovieScene& MovieScene = *Sequence.GetMovieScene();
			UBlueprint* Director = OpenDirector(Sequence);
			if (!TestNotNull(TEXT("Open Director Blueprint"), Director))
			{
				return false;
			}
			// "+ Track > Event > Trigger" on Door, a key, "Create Quick Binding" (a custom event with the bound object pin).
			UMovieSceneEventTriggerSection* DoorSection = AddTriggerSection(*MovieScene.AddTrack<UMovieSceneEventTrack>(Door));
			FMovieSceneEvent* Opened = DoorSection != nullptr ? AddEventKey(*DoorSection, 30) : nullptr;
			UK2Node_CustomEvent* Quick = Opened != nullptr ? FMovieSceneEventUtils::BindNewUserFacingEvent(Opened, DoorSection, Director) : nullptr;
			if (!TestNotNull(TEXT("Create Quick Binding"), Quick))
			{
				return false;
			}
			// A global event track's key bound to a new function endpoint with an int input, and a payload for it.
			UMovieSceneEventTrack* Beats = MovieScene.AddTrack<UMovieSceneEventTrack>();
			UMovieSceneEventTriggerSection* BeatSection = AddTriggerSection(*Beats);
			FMovieSceneDirectorBlueprintEndpointDefinition Definition = FMovieSceneEventUtils::GenerateEventDefinition(Beats);
			Definition.EndpointType = EMovieSceneDirectorBlueprintEndpointType::Function;
			Definition.EndpointName = TEXT("OnBeat");
			Definition.AddExtraInputPin(TEXT("Count"), UEdGraphSchema_K2::PC_Int);
			UK2Node* Function = FMovieSceneDirectorBlueprintUtils::CreateEndpoint(Director, Definition);
			FMovieSceneEvent* Beat = BeatSection != nullptr ? AddEventKey(*BeatSection, 60) : nullptr;
			if (!TestTrue(TEXT("function endpoint"), Function != nullptr && Beat != nullptr))
			{
				return false;
			}
			FMovieSceneEventUtils::BindEventSectionToBlueprint(BeatSection, Director);
			FMovieSceneEventUtils::SetEndpoint(Beat, BeatSection, Function, nullptr);
			FMovieSceneEventPayloadVariable Count;
			Count.Value = TEXT("3");
			Beat->PayloadVariables.Add(TEXT("Count"), Count);
			// Door as "Replaceable from Director Blueprint" with a new endpoint (the dynamic binding's "Create New Endpoint").
			UMovieSceneReplaceableDirectorBlueprintBinding* Custom = NewObject<UMovieSceneReplaceableDirectorBlueprintBinding>(&MovieScene, NAME_None, RF_Transactional);
			static_cast<UMovieSceneSequence&>(Sequence).GetBindingReferences()->AddOrReplaceBinding(Door, Custom, 0);
			const UClass* EndpointUtil = FindObject<UClass>(nullptr, TEXT("/Script/MovieSceneTools.MovieSceneDynamicBindingEndpointUtil"));
			FMovieSceneDirectorBlueprintEndpointDefinition Resolver;
			Resolver.EndpointType = EMovieSceneDirectorBlueprintEndpointType::Function;
			Resolver.EndpointSignature = EndpointUtil != nullptr ? EndpointUtil->FindFunctionByName(TEXT("SampleResolveBinding")) : nullptr;
			Resolver.EndpointName = TEXT("Door_DynamicBinding");
			UK2Node_FunctionEntry* Resolve = Resolver.EndpointSignature != nullptr
				? Cast<UK2Node_FunctionEntry>(FMovieSceneDirectorBlueprintUtils::CreateEndpoint(Director, Resolver)) : nullptr;
			if (!TestNotNull(TEXT("dynamic binding endpoint"), Resolve))
			{
				return false;
			}
			Resolve->MetaData.bCallInEditor = true;
			FMovieSceneDynamicBindingUtils::SetEndpoint(&MovieScene, &Custom->DynamicBinding, Resolve);
			FKismetEditorUtilities::CompileBlueprint(Director);
			return TestTrue(*FString::Printf(TEXT("editor director compiles (%s)"), *StatusText(Director)), Compiled(Director));
		},
		[this, Door](ULevelSequence& Editor, ULevelSequence& Document)
		{
			UBlueprint* Director = DirectorOf(&Document);
			TestTrue(TEXT("document side: a Director inside the sequence"), Director != nullptr && Director->GetOutermost() == Document.GetOutermost());
			TestTrue(*FString::Printf(TEXT("document side: the Director compiles (%s)"), *StatusText(Director)), Compiled(Director));
			const FMovieSceneEvent* Opened = FirstEvent(FirstSection(EventTrackOf(Document, Door)));
			TestTrue(TEXT("Door's key calls a custom event of its own Director"), Opened != nullptr
				&& Cast<UK2Node_CustomEvent>(Opened->WeakEndpoint.Get()) != nullptr && IsInBlueprint(Opened->WeakEndpoint.Get(), Director));
			const FMovieSceneEvent* EditorOpened = FirstEvent(FirstSection(EventTrackOf(Editor, Door)));
			TestEqual(TEXT("Door's key: same endpoint as the editor's"), EndpointName(Opened), EndpointName(EditorOpened));
			TestEqual(TEXT("Door's key: same bound object pin"), Opened != nullptr ? Opened->BoundObjectPinName : FName(),
				EditorOpened != nullptr ? EditorOpened->BoundObjectPinName : FName(TEXT("<no key>")));
			const UMovieSceneEventTrack* Beats = Document.GetMovieScene()->FindTrack<UMovieSceneEventTrack>();
			const FMovieSceneEvent* Beat = FirstEvent(FirstSection(Beats));
			TestEqual(TEXT("the beat key calls OnBeat"), EndpointName(Beat), FString(TEXT("OnBeat")));
			TestTrue(TEXT("the beat key's payload"), Beat != nullptr && Beat->PayloadVariables.Contains(TEXT("Count"))
				&& Beat->PayloadVariables[TEXT("Count")].Value == TEXT("3"));
			const UMovieSceneReplaceableDirectorBlueprintBinding* Custom = DirectorBindingOf(Document, Door);
			TestTrue(TEXT("Door's director binding calls Door_DynamicBinding of its own Director"), Custom != nullptr
				&& Cast<UK2Node_FunctionEntry>(Custom->DynamicBinding.WeakEndpoint.Get()) != nullptr
				&& IsInBlueprint(Custom->DynamicBinding.WeakEndpoint.Get(), Director)
				&& Custom->DynamicBinding.WeakEndpoint->GetTypedOuter<UEdGraph>()->GetName() == TEXT("Door_DynamicBinding"));
		});
	TestTrue(TEXT("twin equal"), bSame);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerDirectorFreeEditsTest, "UEAssetDoc.Sequencer.Director.FreeEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerDirectorFreeEditsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerDirectorTests;
	FFixtures Fixtures;
	FBuilt Built;
	UObject* Base = LoadedFixture(*this, Fixtures, Built, TEXT("LS_Base"));
	if (!TestNotNull(TEXT("fixture"), Base))
	{
		return false;
	}
	const FTarget& Target = Fixtures.Add(TEXT("LS_DirectorEdits"));
	if (Duplicate(*this, Base, Target) == nullptr)
	{
		return false;
	}
	// 1. A Director with a function endpoint and a key calling it (bound object pin, payload).
	if (!DocEdit(*this, Target, TEXT("director and event key"), [](const TSharedPtr<FJsonObject>& Body)
		{
			Body->SetObjectField(TEXT("Director"), Parse(OpenDoorDirector));
			Append(RootBinding(Body, TEXT("Door")), TEXT("Tracks"), OpenDoorTrack);
		}, nullptr))
	{
		return false;
	}
	ULevelSequence* Sequence = Target.GetSequence();
	UBlueprint* Director = DirectorOf(Sequence);
	TestTrue(TEXT("a Director inside the sequence"), Director != nullptr && Director->GetOutermost() == Sequence->GetOutermost());
	TestTrue(*FString::Printf(TEXT("the Director compiles (%s)"), *StatusText(Director)), Compiled(Director));
	const FMovieSceneEvent* Key = FirstEvent(FirstSection(EventTrackOf(*Sequence, Built.Door)));
	const TWeakObjectPtr<UObject> Endpoint = Key != nullptr ? Key->WeakEndpoint.Get() : nullptr;
	TestEqual(TEXT("the key calls OpenDoor"), EndpointName(Key), FString(TEXT("OpenDoor")));
	TestTrue(TEXT("the key's bound object pin and payload"), Key != nullptr && Key->BoundObjectPinName == TEXT("Door")
		&& Key->PayloadVariables.Contains(TEXT("Amount")) && Key->PayloadVariables[TEXT("Amount")].Value == TEXT("2.5"));
	const UClass* Generated = Director != nullptr ? Director->GeneratedClass.Get() : nullptr;
	TestTrue(TEXT("the compiled class has OpenDoor and Speed"), Generated != nullptr && Generated->FindFunctionByName(TEXT("OpenDoor")) != nullptr
		&& FindFProperty<FProperty>(Generated, TEXT("Speed")) != nullptr);

	// 2. The function renamed in the same document: the graph is renamed (its GraphGuid), the key follows the same node.
	DocEdit(*this, Target, TEXT("rename the endpoint"), [](const TSharedPtr<FJsonObject>& Body)
		{
			const TSharedPtr<FJsonObject> Director = Child(Body, TEXT("Director"));
			Named(Items(Director, TEXT("FunctionGraphs")), TEXT("Name"), TEXT("OpenDoor"))->SetStringField(TEXT("Name"), TEXT("OpenGate"));
			const TSharedPtr<FJsonObject> Section = FirstItem(Items(BindingTrack(RootBinding(Body, TEXT("Door")), TEXT("Event")), TEXT("Sections")));
			const TSharedPtr<FJsonObject> First = FirstItem(Items(Child(Child(Section, TEXT("Channels")), TEXT("Value")), TEXT("Keys")));
			Child(First, TEXT("Event"))->SetStringField(TEXT("Endpoint"), TEXT("OpenGate"));
		}, nullptr);
	Key = FirstEvent(FirstSection(EventTrackOf(*Target.GetSequence(), Built.Door)));
	TestEqual(TEXT("after the rename the key calls OpenGate"), EndpointName(Key), FString(TEXT("OpenGate")));
	TestTrue(TEXT("the same endpoint node (renamed, not re-created)"), Key != nullptr && Endpoint.IsValid() && Key->WeakEndpoint.Get() == Endpoint.Get());

	// 3. Deleting the endpoint while a key uses it: refused at the key, nothing changes.
	{
		const TSharedPtr<FJsonObject> Before = Extract(*this, Target.ObjectPath, TEXT("before delete"));
		const TSharedPtr<FJsonObject> Document = ForTarget(Before, Target, TEXT("Update"));
		Child(BodyOf(Document), TEXT("Director"))->SetArrayField(TEXT("FunctionGraphs"), TArray<TSharedPtr<FJsonValue>>());
		const FAssetDocumentResult Refused = Apply(Target, Document);
		TestFalse(TEXT("deleting a used endpoint fails"), Refused.IsSuccess());
		TestTrue(*FString::Printf(TEXT("Sequencer.UnknownEventEndpoint (%s)"), *DiagnosticsText(Refused)), HasCode(Refused, TEXT("Sequencer.UnknownEventEndpoint")));
		TestTrue(*FString::Printf(TEXT("at the key (%s)"), *FirstPath(Refused, TEXT("Sequencer.UnknownEventEndpoint"))),
			FirstPath(Refused, TEXT("Sequencer.UnknownEventEndpoint")).StartsWith(TEXT("/Body/Bindings/")));
		ExpectSame(*this, TEXT("unchanged after the refused delete"), Serialize(Before), Serialize(Extract(*this, Target.ObjectPath, TEXT("after delete"))));
	}

	// 4. A director compile error rolls back the whole apply (a marker change included).
	{
		const TSharedPtr<FJsonObject> Before = Extract(*this, Target.ObjectPath, TEXT("before compile error"));
		const TSharedPtr<FJsonObject> Document = ForTarget(Before, Target, TEXT("Update"));
		FirstItem(Items(BodyOf(Document), TEXT("Markers")))->SetStringField(TEXT("Label"), TEXT("Changed"));
		// An Actor function called without a target: the director is not an Actor ("must have a connection").
		Append(Child(BodyOf(Document), TEXT("Director")), TEXT("FunctionGraphs"), TEXT(R"json({"Name": "Broken",
			"Nodes": [{"Id": "Destroy", "Action": "K2:CallFunction:/Script/Engine.Actor:K2_DestroyActor"}],
			"Links": [{"From": "Entry:then", "To": "Destroy:execute"}]})json"));
		const FAssetDocumentResult Failed = Apply(Target, Document);
		TestFalse(TEXT("a director compile error fails"), Failed.IsSuccess());
		AddInfo(FString::Printf(TEXT("compile error: %s"), *DiagnosticsText(Failed)));
		TestTrue(TEXT("reported under /Body/Director"), !Failed.Diagnostics.IsEmpty() && Failed.Diagnostics[0].Path.StartsWith(TEXT("/Body/Director")));
		ExpectSame(*this, TEXT("unchanged after the compile error"), Serialize(Before), Serialize(Extract(*this, Target.ObjectPath, TEXT("after compile error"))));
		TestTrue(*FString::Printf(TEXT("the Director compiles again (%s)"), *StatusText(DirectorOf(Target.GetSequence()))), Compiled(DirectorOf(Target.GetSequence())));
		TestFalse(TEXT("no undo history references the sequence"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	}

	// 5. "Director": null while a key calls it: refused (Sequencer.DirectorInUse); with the key track removed: removed.
	{
		const TSharedPtr<FJsonObject> Before = Extract(*this, Target.ObjectPath, TEXT("before director removal"));
		const TSharedPtr<FJsonObject> Document = ForTarget(Before, Target, TEXT("Update"));
		BodyOf(Document)->SetField(TEXT("Director"), MakeShared<FJsonValueNull>());
		const FAssetDocumentResult Refused = Apply(Target, Document);
		TestFalse(TEXT("removing a used Director fails"), Refused.IsSuccess());
		TestTrue(*FString::Printf(TEXT("Sequencer.DirectorInUse (%s)"), *DiagnosticsText(Refused)), HasCode(Refused, TEXT("Sequencer.DirectorInUse")));
		ExpectSame(*this, TEXT("unchanged after the refused removal"), Serialize(Before), Serialize(Extract(*this, Target.ObjectPath, TEXT("after refused removal"))));
	}
	DocEdit(*this, Target, TEXT("remove the director with its keys"), [](const TSharedPtr<FJsonObject>& Body)
		{
			Body->SetField(TEXT("Director"), MakeShared<FJsonValueNull>());
			const TSharedPtr<FJsonObject> Door = RootBinding(Body, TEXT("Door"));
			TArray<TSharedPtr<FJsonValue>>& Tracks = ArrayOf(Door, TEXT("Tracks"));
			Tracks.RemoveAll([](const TSharedPtr<FJsonValue>& Track) { return Track->AsObject()->GetStringField(TEXT("Type")) == TEXT("Event"); });
		}, nullptr);
	TestNull(TEXT("no Director afterwards"), DirectorOf(Target.GetSequence()));
	TestFalse(TEXT("the extract has no Director"), BodyOf(Extract(*this, Target.ObjectPath, TEXT("after removal")))->HasField(TEXT("Director")));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerDirectorRollbackTest, "UEAssetDoc.Sequencer.Director.Rollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerDirectorRollbackTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerDirectorTests;
	FFixtures Fixtures;
	FBuilt Built;
	UObject* Base = LoadedFixture(*this, Fixtures, Built, TEXT("LS_Base"));
	if (!TestNotNull(TEXT("fixture"), Base))
	{
		return false;
	}
	const FTarget& Target = Fixtures.Add(TEXT("LS_DirectorRollback"));
	if (Duplicate(*this, Base, Target) == nullptr)
	{
		return false;
	}
	// 1. A Director created by an apply that then fails (a key calling an endpoint it does not have) is gone afterwards.
	{
		const TSharedPtr<FJsonObject> Before = Extract(*this, Target.ObjectPath, TEXT("new director before"));
		const TSharedPtr<FJsonObject> Document = ForTarget(Before, Target, TEXT("Update"));
		BodyOf(Document)->SetObjectField(TEXT("Director"), Parse(OpenDoorDirector));
		Append(RootBinding(BodyOf(Document), TEXT("Door")), TEXT("Tracks"), *FString(OpenDoorTrack).Replace(TEXT("\"Endpoint\": \"OpenDoor\""),
			TEXT("\"Endpoint\": \"Missing\"")));
		const FAssetDocumentResult Failed = Apply(Target, Document);
		TestFalse(TEXT("the apply fails"), Failed.IsSuccess());
		TestTrue(*FString::Printf(TEXT("Sequencer.UnknownEventEndpoint (%s)"), *DiagnosticsText(Failed)), HasCode(Failed, TEXT("Sequencer.UnknownEventEndpoint")));
		ExpectSame(*this, TEXT("new director: the sequence is as before"), Serialize(Before), Serialize(Extract(*this, Target.ObjectPath, TEXT("new director after"))));
		TestNull(TEXT("no Director afterwards"), DirectorOf(Target.GetSequence()));
		TestFalse(TEXT("new director: no undo history references the sequence"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	}
	// 2. An existing Director: an edit that fails later (a camera cut to a missing binding) leaves it as before.
	if (!DocEdit(*this, Target, TEXT("director"), [](const TSharedPtr<FJsonObject>& Body)
		{
			Body->SetObjectField(TEXT("Director"), Parse(OpenDoorDirector));
			Append(RootBinding(Body, TEXT("Door")), TEXT("Tracks"), OpenDoorTrack);
		}, nullptr))
	{
		return false;
	}
	{
		UBlueprint* Director = DirectorOf(Target.GetSequence());
		const UClass* ClassBefore = Director != nullptr ? Director->GeneratedClass.Get() : nullptr;
		const TSharedPtr<FJsonObject> Before = Extract(*this, Target.ObjectPath, TEXT("director edit before"));
		const TSharedPtr<FJsonObject> Document = ForTarget(Before, Target, TEXT("Update"));
		const TSharedPtr<FJsonObject> DirectorBody = Child(BodyOf(Document), TEXT("Director"));
		Append(DirectorBody, TEXT("Variables"), TEXT(R"({"Name": "Extra", "Type": "int"})"));
		Named(Items(DirectorBody, TEXT("FunctionGraphs")), TEXT("Name"), TEXT("OpenDoor"))->SetStringField(TEXT("Name"), TEXT("OpenAll"));
		const TSharedPtr<FJsonObject> Cut = FirstItem(Items(GlobalTrack(BodyOf(Document), TEXT("CameraCut")), TEXT("Sections")));
		Child(Cut, TEXT("Properties"))->SetObjectField(TEXT("CameraBindingID"), Parse(TEXT(R"({"Binding": "Nobody"})")));
		const FAssetDocumentResult Failed = Apply(Target, Document);
		TestFalse(TEXT("the director edit fails"), Failed.IsSuccess());
		ExpectSame(*this, TEXT("director edit: the sequence is as before"), Serialize(Before), Serialize(Extract(*this, Target.ObjectPath, TEXT("director edit after"))));
		UBlueprint* After = DirectorOf(Target.GetSequence());
		TestTrue(*FString::Printf(TEXT("the Director is up to date (%s)"), *StatusText(After)), Compiled(After));
		const UClass* Generated = After != nullptr ? After->GeneratedClass.Get() : nullptr;
		TestTrue(TEXT("its compiled class has OpenDoor and no Extra"), Generated != nullptr && Generated->FindFunctionByName(TEXT("OpenDoor")) != nullptr
			&& Generated->FindFunctionByName(TEXT("OpenAll")) == nullptr && FindFProperty<FProperty>(Generated, TEXT("Extra")) == nullptr);
		AddInfo(FString::Printf(TEXT("generated class %s"), Generated == ClassBefore ? TEXT("kept") : TEXT("rebuilt")));
		TestFalse(TEXT("director edit: no undo history references the sequence"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
		TestEqual(TEXT("the Director's key still calls OpenDoor"), EndpointName(FirstEvent(FirstSection(EventTrackOf(*Target.GetSequence(), Built.Door)))),
			FString(TEXT("OpenDoor")));
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerDirectorUndoTest, "UEAssetDoc.Sequencer.Director.Undo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerDirectorUndoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerDirectorTests;
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
	const FTarget& Target = Fixtures.Add(TEXT("LS_DirectorUndo"));
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
	// A human edit of the Director (a variable added in the Blueprint editor) and a save, then an AI Director edit.
	UBlueprint* Human = OpenDirector(*Target.GetSequence());
	if (!TestNotNull(TEXT("human opens the Director"), Human))
	{
		return false;
	}
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentSequencerTests", "AddVariable", "Add Variable"));
		FEdGraphPinType Type;
		Type.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		FBlueprintEditorUtils::AddMemberVariable(Human, TEXT("bHuman"), Type);
	}
	FKismetEditorUtilities::CompileBlueprint(Human);
	TestTrue(TEXT("human save"), Save(*Target.GetSequence()));
	if (!DocEdit(*this, Target, TEXT("AI director edit"), [](const TSharedPtr<FJsonObject>& Body)
		{
			const TSharedPtr<FJsonObject> Director = Parse(OpenDoorDirector);
			Append(Director, TEXT("Variables"), TEXT(R"({"Name": "bHuman", "Type": "bool"})"));
			Body->SetObjectField(TEXT("Director"), Director);
			Append(RootBinding(Body, TEXT("Door")), TEXT("Tracks"), OpenDoorTrack);
		}, nullptr))
	{
		return false;
	}
	TestFalse(TEXT("after AI edit: no undo history references the sequence"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const FString AfterEdit = Serialize(Extract(*this, Target.ObjectPath, TEXT("after AI edit")));
	UndoRedoAll(TEXT("after AI edit"));
	TestEqual(TEXT("after AI edit + undo/redo"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after undo/redo"))), AfterEdit);
	// A failing AI edit (compile error) rolls back; undo stays safe.
	const TSharedPtr<FJsonObject> Failing = ForTarget(Parse(AfterEdit), Target, TEXT("Update"));
	Append(Child(BodyOf(Failing), TEXT("Director")), TEXT("FunctionGraphs"), TEXT(R"json({"Name": "Broken",
		"Nodes": [{"Id": "Destroy", "Action": "K2:CallFunction:/Script/Engine.Actor:K2_DestroyActor"}],
		"Links": [{"From": "Entry:then", "To": "Destroy:execute"}]})json"));
	TestFalse(TEXT("the failing apply fails"), Apply(Target, Failing).IsSuccess());
	TestEqual(TEXT("after rollback"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after rollback"))), AfterEdit);
	UndoRedoAll(TEXT("after rollback"));
	TestEqual(TEXT("after rollback + undo/redo"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after rollback undo/redo"))), AfterEdit);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerGroupsTest, "UEAssetDoc.Sequencer.Groups",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerGroupsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerDirectorTests;
	FFixtures Fixtures;
	FBuilt Built;
	UObject* Base = LoadedFixture(*this, Fixtures, Built, TEXT("LS_Base"));
	if (!TestNotNull(TEXT("fixture"), Base))
	{
		return false;
	}
	const FGuid Camera = Built.Camera;
	const FGuid Door = Built.Door;
	// Editor twin: "Group" on the camera's transform section and the fade section; a node group with the camera, the
	// Cameras folder and the fade track (the Group Manager's "Add Selected to Group").
	Twin(*this, Fixtures, Base, TEXT("section and node groups"), [Camera, Door](ULevelSequence& Sequence)
		{
			UMovieScene& MovieScene = *Sequence.GetMovieScene();
			UMovieSceneSection* Transform = FirstSection(MovieScene.FindTrack<UMovieScene3DTransformTrack>(Camera));
			UMovieSceneFadeTrack* FadeTrack = MovieScene.FindTrack<UMovieSceneFadeTrack>();
			UMovieSceneSection* Fade = FirstSection(FadeTrack);
			if (Transform == nullptr || Fade == nullptr)
			{
				return false;
			}
			MovieScene.GroupSections({Transform, Fade});
			UMovieSceneNodeGroup* Group = NewObject<UMovieSceneNodeGroup>(&MovieScene.GetNodeGroups(), NAME_None, RF_Transactional);
			Group->SetName(TEXT("Hero"));
			Group->AddNode(TEXT("Cameras.") + Camera.ToString());
			Group->AddNode(Door.ToString());
			Group->AddNode(FadeTrack->GetName());
			MovieScene.GetNodeGroups().AddNodeGroup(Group);
			return true;
		}, [this, Camera](ULevelSequence& Editor, ULevelSequence& Document)
		{
			UMovieScene& MovieScene = *Document.GetMovieScene();
			UMovieSceneSection* Transform = FirstSection(MovieScene.FindTrack<UMovieScene3DTransformTrack>(Camera));
			UMovieSceneSection* Fade = FirstSection(MovieScene.FindTrack<UMovieSceneFadeTrack>());
			TestTrue(TEXT("document side: the two sections are one group"), Transform != nullptr && Fade != nullptr
				&& MovieScene.IsSectionInGroup(*Transform) && MovieScene.GetSectionGroup(*Transform) != nullptr
				&& MovieScene.GetSectionGroup(*Transform)->Contains(*Fade));
			TestEqual(TEXT("document side: one node group"), MovieScene.GetNodeGroups().Num(), 1);
			TestEqual(TEXT("document side: the same outliner paths"), NodeGroupsText(MovieScene), NodeGroupsText(*Editor.GetMovieScene()));
		});

	// Document edits on a duplicate.
	const FTarget& Target = Fixtures.Add(TEXT("LS_Groups"));
	if (Duplicate(*this, Base, Target) == nullptr)
	{
		return false;
	}
	DocEdit(*this, Target, TEXT("group camera transform, fade and focal length"), [](const TSharedPtr<FJsonObject>& Body)
		{
			const TSharedPtr<FJsonObject> Camera = RootBinding(Body, TEXT("ShotCam"));
			FirstItem(Items(BindingTrack(Camera, TEXT("Transform")), TEXT("Sections")))->SetNumberField(TEXT("Group"), 1);
			FirstItem(Items(GlobalTrack(Body, TEXT("Fade")), TEXT("Sections")))->SetNumberField(TEXT("Group"), 1);
			const TSharedPtr<FJsonObject> Component = Named(Items(Camera, TEXT("Children")), TEXT("Name"), TEXT("CameraComponent"));
			FirstItem(Items(FirstItem(Items(Component, TEXT("Tracks"))), TEXT("Sections")))->SetNumberField(TEXT("Group"), 1);
			Append(Body, TEXT("Groups"), TEXT(R"({"Name": "Hero", "Members": [{"Binding": "ShotCam"}, {"Folder": "Cameras"}, {"Track": "/Body/Tracks/1"}]})"));
		}, nullptr);
	{
		UMovieScene& MovieScene = *Target.GetSequence()->GetMovieScene();
		UMovieSceneSection* Transform = FirstSection(MovieScene.FindTrack<UMovieScene3DTransformTrack>(Built.Camera));
		const FMovieSceneSectionGroup* Group = Transform != nullptr ? MovieScene.GetSectionGroup(*Transform) : nullptr;
		TestEqual(TEXT("three sections in one group"), Group != nullptr ? Group->Num() : 0, 3);
		const FString Groups = NodeGroupsText(MovieScene);
		TestTrue(*FString::Printf(TEXT("node group Hero with three members (%s)"), *Groups), MovieScene.GetNodeGroups().Num() == 1
			&& Groups.StartsWith(TEXT("Hero: ")) && Groups.Contains(TEXT("Cameras")));
		int32 Spaces = 0;
		for (const TCHAR Character : Groups)
		{
			Spaces += Character == TEXT(' ') ? 1 : 0;
		}
		TestEqual(TEXT("three members"), Spaces, 3);
	}
	DocEdit(*this, Target, TEXT("regroup and rename"), [](const TSharedPtr<FJsonObject>& Body)
		{
			// The focal length section leaves the group; a second node group; Hero renamed.
			const TSharedPtr<FJsonObject> Camera = RootBinding(Body, TEXT("ShotCam"));
			const TSharedPtr<FJsonObject> Component = Named(Items(Camera, TEXT("Children")), TEXT("Name"), TEXT("CameraComponent"));
			FirstItem(Items(FirstItem(Items(Component, TEXT("Tracks"))), TEXT("Sections")))->RemoveField(TEXT("Group"));
			FirstItem(Items(Body, TEXT("Groups")))->SetStringField(TEXT("Name"), TEXT("Heroes"));
			Append(Body, TEXT("Groups"), TEXT(R"({"Name": "Set", "Members": [{"Binding": "Door"}]})"));
		}, nullptr);
	{
		UMovieScene& MovieScene = *Target.GetSequence()->GetMovieScene();
		UMovieSceneSection* Transform = FirstSection(MovieScene.FindTrack<UMovieScene3DTransformTrack>(Built.Camera));
		const FMovieSceneSectionGroup* Group = Transform != nullptr ? MovieScene.GetSectionGroup(*Transform) : nullptr;
		TestEqual(TEXT("two sections in the group"), Group != nullptr ? Group->Num() : 0, 2);
		TestEqual(TEXT("two node groups"), MovieScene.GetNodeGroups().Num(), 2);
	}
	DocEdit(*this, Target, TEXT("ungroup everything"), [](const TSharedPtr<FJsonObject>& Body)
		{
			const TSharedPtr<FJsonObject> Camera = RootBinding(Body, TEXT("ShotCam"));
			FirstItem(Items(BindingTrack(Camera, TEXT("Transform")), TEXT("Sections")))->RemoveField(TEXT("Group"));
			FirstItem(Items(GlobalTrack(Body, TEXT("Fade")), TEXT("Sections")))->RemoveField(TEXT("Group"));
			Body->SetArrayField(TEXT("Groups"), TArray<TSharedPtr<FJsonValue>>());
		}, nullptr);
	{
		UMovieScene& MovieScene = *Target.GetSequence()->GetMovieScene();
		UMovieSceneSection* Transform = FirstSection(MovieScene.FindTrack<UMovieScene3DTransformTrack>(Built.Camera));
		TestTrue(TEXT("no section group"), Transform != nullptr && !MovieScene.IsSectionInGroup(*Transform));
		TestEqual(TEXT("no node group"), MovieScene.GetNodeGroups().Num(), 0);
	}
	// A section group of one section is refused before any change.
	{
		const TSharedPtr<FJsonObject> Before = Extract(*this, Target.ObjectPath, TEXT("lonely group before"));
		const TSharedPtr<FJsonObject> Document = ForTarget(Before, Target, TEXT("Update"));
		FirstItem(Items(GlobalTrack(BodyOf(Document), TEXT("Fade")), TEXT("Sections")))->SetNumberField(TEXT("Group"), 4);
		const FAssetDocumentResult Refused = Apply(Target, Document);
		TestFalse(TEXT("a one-section group is refused"), Refused.IsSuccess());
		TestTrue(*FString::Printf(TEXT("at its Group (%s)"), *DiagnosticsText(Refused)), FirstPath(Refused, TEXT("Sequencer.InvalidBody")).EndsWith(TEXT("/Group")));
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerRemoteBindingTest, "UEAssetDoc.Sequencer.RemoteBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerRemoteBindingTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerDirectorTests;
	FFixtures Fixtures;
	// A shot with its own spawnable camera.
	const FTarget& ShotTarget = Fixtures.Add(TEXT("LS_Shot"));
	if (!ApplyOk(*this, ShotTarget, Parse(FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Class": "%s", "Action": "Create", "Target": "%s",
		"Properties": {}, "Body": {"Settings": {"PlaybackRange": {"Start": 0, "End": 150}},
		"Bindings": [{"Name": "CloseUp", "Spawnable": {"Class": "/Script/CinematicCamera.CineCameraActor"}}]}})json"), LevelSequenceClass,
		*ShotTarget.PackageName)), TEXT("the shot"), nullptr))
	{
		return false;
	}
	ULevelSequence* Shot = ShotTarget.GetSequence();
	FGuid ShotCamera;
	for (int32 Index = 0; Shot != nullptr && Index < Shot->GetMovieScene()->GetPossessableCount(); ++Index)
	{
		const FMovieScenePossessable& Possessable = Shot->GetMovieScene()->GetPossessable(Index);
		if (Possessable.GetName() == TEXT("CloseUp"))
		{
			ShotCamera = Possessable.GetGuid();
		}
	}
	if (!TestTrue(TEXT("the shot's camera"), ShotCamera.IsValid()))
	{
		return false;
	}
	// A master playing it as a shot, with an empty camera cut track.
	const FTarget& MasterTarget = Fixtures.Add(TEXT("LS_Master"));
	ULevelSequence* Master = NewSequence(MasterTarget);
	UMovieSceneCinematicShotTrack* Shots = Master->GetMovieScene()->AddTrack<UMovieSceneCinematicShotTrack>();
	Shots->AddSequence(Shot, Frame(0), 150 * TicksPerFrame);
	Master->GetMovieScene()->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass());
	TestTrue(TEXT("master saved"), Save(*Master));
	UObject* Source = Reload(MasterTarget.ObjectPath);
	if (!TestNotNull(TEXT("master"), Source))
	{
		return false;
	}
	// Editor twin: the camera cut's binding picker on the shot's camera (FSequenceBindingTree: the ID stack of the shot).
	Twin(*this, Fixtures, Source, TEXT("camera cut into a shot"), [ShotCamera](ULevelSequence& Sequence)
		{
			UMovieScene& MovieScene = *Sequence.GetMovieScene();
			UMovieSceneCinematicShotTrack* Track = MovieScene.FindTrack<UMovieSceneCinematicShotTrack>();
			const UMovieSceneSubSection* Section = Cast<UMovieSceneSubSection>(FirstSection(Track));
			UMovieSceneCameraCutTrack* Cuts = Cast<UMovieSceneCameraCutTrack>(MovieScene.GetCameraCutTrack());
			if (Section == nullptr || Cuts == nullptr)
			{
				return false;
			}
			const FMovieSceneSequenceID Id = Section->GetSequenceID();
			UMovieSceneCameraCutSection* Cut = Cuts->AddNewCameraCut(UE::MovieScene::FRelativeObjectBindingID(ShotCamera, Id, 0), Frame(0));
			Cut->SetRange(Frames(0, 150));
			return true;
		}, [this, ShotCamera](ULevelSequence& Editor, ULevelSequence& Document)
		{
			const UMovieSceneCameraCutSection* Mine = Cast<UMovieSceneCameraCutSection>(FirstSection(Document.GetMovieScene()->GetCameraCutTrack()));
			const UMovieSceneCameraCutSection* Theirs = Cast<UMovieSceneCameraCutSection>(FirstSection(Editor.GetMovieScene()->GetCameraCutTrack()));
			TestTrue(TEXT("document side: the same binding ID"), Mine != nullptr && Theirs != nullptr
				&& Mine->GetCameraBindingID().GetGuid() == ShotCamera
				&& Mine->GetCameraBindingID().GetRelativeSequenceID() == Theirs->GetCameraBindingID().GetRelativeSequenceID());
		});
	// One document: a new shot section and a camera cut into it (the shot is written before the cut).
	const FTarget& Target = Fixtures.Add(TEXT("LS_MasterDoc"));
	const TSharedPtr<FJsonObject> Created = Parse(FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Class": "%s", "Action": "Create", "Target": "%s",
		"Properties": {}, "Body": {"Settings": {"PlaybackRange": {"Start": 0, "End": 150}},
		"Tracks": [{"Type": "CameraCut", "Sections": [{"Start": 0, "End": 150, "Properties": {"CameraBindingID":
				{"Binding": "CloseUp", "Sequence": "/Body/Tracks/1/Sections/0"}}}]},
			{"Type": "Shot", "Sections": [{"Start": 0, "End": 150, "Properties": {"SubSequence": "%s"}}]}]}})json"),
		LevelSequenceClass, *Target.PackageName, *ShotTarget.ObjectPath));
	FAssetDocumentResult Applied;
	if (ApplyOk(*this, Target, Created, TEXT("shot and camera cut in one document"), &Applied))
	{
		FString DiffText;
		const int32 Changes = DiffCount(Created, DiffText);
		if (!TestEqual(TEXT("no difference to the document"), Changes, 0))
		{
			AddInfo(DiffText.Left(1500));
		}
		ULevelSequence* Sequence = Target.GetSequence();
		const UMovieSceneSubSection* Section = Sequence != nullptr
			? Cast<UMovieSceneSubSection>(FirstSection(Sequence->GetMovieScene()->FindTrack<UMovieSceneCinematicShotTrack>())) : nullptr;
		const UMovieSceneCameraCutSection* Cut = Sequence != nullptr ? Cast<UMovieSceneCameraCutSection>(FirstSection(Sequence->GetMovieScene()->GetCameraCutTrack())) : nullptr;
		TestTrue(TEXT("the cut names the shot's camera in the shot"), Section != nullptr && Cut != nullptr
			&& Cut->GetCameraBindingID().GetGuid() == ShotCamera
			&& Cut->GetCameraBindingID().GetRelativeSequenceID() == Section->GetSequenceID());
		const FAssetDocumentResult Inspected = Inspect(Target.ObjectPath);
		for (const TSharedPtr<FJsonObject>& Warning : ReadWarnings(Inspected))
		{
			TestNotEqual(TEXT("no UnsupportedRemoteBinding"), Warning->GetStringField(TEXT("Code")), FString(TEXT("Sequencer.UnsupportedRemoteBinding")));
		}
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

namespace AssetDocumentSequencerDirectorTests
{
FString FromScratch(const TCHAR* Action, const FTarget& Target, const FString& Body)
{
	return FString::Printf(TEXT(R"json({"SchemaVersion": 1, "Class": "%s", "Action": "%s", "Target": "%s", "Properties": {}, "Body": %s})json"),
		LevelSequenceClass, Action, *Target.PackageName, *Body);
}
} // namespace AssetDocumentSequencerDirectorTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerFromScratchTest, "UEAssetDoc.Sequencer.FromScratch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerFromScratchTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerDirectorTests;
	FFixtures Fixtures;
	const FTarget& Intro = Fixtures.Add(TEXT("LS_Intro"));
	const FTarget& Outro = Fixtures.Add(TEXT("LS_Outro"));
	const FTarget& Master = Fixtures.Add(TEXT("LS_IntroMaster"));
	int32 Calls = 0;
	int32 Failed = 0;
	const double Start = FPlatformTime::Seconds();
	auto Run = [this, &Calls, &Failed](const FTarget& Target, const FString& Json, const TCHAR* Stage)
	{
		++Calls;
		const TSharedPtr<FJsonObject> Document = Parse(Json);
		if (!TestTrue(*FString::Printf(TEXT("%s: the document parses"), Stage), Document.IsValid()))
		{
			++Failed;
			return false;
		}
		FAssetDocumentResult Result;
		if (!ApplyOk(*this, Target, Document, Stage, &Result))
		{
			++Failed;
			return false;
		}
		FString DiffText;
		const int32 Changes = DiffCount(Document, DiffText);
		if (!TestEqual(*FString::Printf(TEXT("%s: no difference to the document"), Stage), Changes, 0))
		{
			AddInfo(DiffText.Left(1500));
		}
		AddInfo(FString::Printf(TEXT("%s: warnings %s"), Stage, *WarningsOf(Result)));
		return true;
	};
	// The intro: 12 s at 24 fps (288 frames); two spawned cine cameras (a dolly with a focus pull; a reverse shot), camera
	// cuts, fade in / out, a marker per beat, the sun possessed (a level actor locator), an event at the last frame
	// calling the Director function IntroDone that prints "Intro done".
	const FString IntroBody = TEXT(R"json({
		"Settings": {"DisplayRate": {"Numerator": 24, "Denominator": 1}, "PlaybackRange": {"Start": 0, "End": 288}},
		"Bindings": [
			{"Name": "DollyCam", "Folder": "Cameras", "Spawnable": {"Class": "/Script/CinematicCamera.CineCameraActor"},
			 "Tracks": [{"Type": "Transform", "Sections": [{"Start": 0, "End": 168, "Channels": {
					"Location.X": {"Keys": [{"Frame": 0, "Value": -800}, {"Frame": 168, "Value": -200}]},
					"Location.Z": {"Default": 170}}}]}],
			 "Children": [{"Name": "CameraComponent", "Bind": ["uobj://subobj?payload0=CameraComponent"], "Tracks": [
					{"Type": "Float", "Property": "FocusSettings.ManualFocusDistance", "Sections": [{"Start": 0, "End": 168, "Channels": {
						"Value": {"Keys": [{"Frame": 0, "Value": 1000}, {"Frame": 168, "Value": 250}]}}}]}]}]},
			{"Name": "ReverseCam", "Folder": "Cameras", "Spawnable": {"Class": "/Script/CinematicCamera.CineCameraActor"},
			 "Tracks": [{"Type": "Transform", "Sections": [{"Start": 168, "End": 288, "Channels": {
					"Location.X": {"Default": 400}, "Rotation.Z": {"Default": 180}, "Location.Z": {"Default": 160}}}]}]},
			{"Name": "Sun", "Class": "/Script/Engine.DirectionalLight", "Bind": [],
			 "Tracks": [{"Type": "Transform", "Sections": [{"Start": 0, "End": 288, "Channels": {
					"Rotation.Y": {"Keys": [{"Frame": 0, "Value": -10}, {"Frame": 288, "Value": -35}]}}}]}]}],
		"Tracks": [
			{"Type": "CameraCut", "Sections": [
				{"Start": 0, "End": 168, "Properties": {"CameraBindingID": {"Binding": "DollyCam"}}},
				{"Start": 168, "End": 288, "Properties": {"CameraBindingID": {"Binding": "ReverseCam"}}}]},
			{"Type": "Fade", "Sections": [{"Start": 0, "End": 288, "Channels": {"Value": {"Keys": [
				{"Frame": 0, "Value": 1}, {"Frame": 24, "Value": 0}, {"Frame": 264, "Value": 0}, {"Frame": 288, "Value": 1}]}}}]},
			{"Type": "Event", "Sections": [{"Class": "Trigger", "Start": 0, "End": 288, "Channels": {"Value": {"Keys": [
				{"Frame": 287, "Event": {"Endpoint": "IntroDone"}}]}}}]}],
		"Folders": [{"Path": "Cameras"}],
		"Markers": [{"Frame": 0, "Label": "Open"}, {"Frame": 168, "Label": "Reverse"}, {"Frame": 287, "Label": "Done"}],
		"Director": {"FunctionGraphs": [{"Name": "IntroDone",
			"Nodes": [{"Id": "Say", "Action": "K2:PrintString", "PinOverrides": [{"Pin": "InString", "DefaultValue": "Intro done"}]}],
			"Links": [{"From": "Entry:then", "To": "Say:execute"}]}]}})json");
	const FString OutroBody = TEXT(R"json({
		"Settings": {"DisplayRate": {"Numerator": 24, "Denominator": 1}, "PlaybackRange": {"Start": 0, "End": 120}},
		"Bindings": [{"Name": "OutroCam", "Spawnable": {"Class": "/Script/CinematicCamera.CineCameraActor"}}],
		"Tracks": [{"Type": "CameraCut", "Sections": [{"Start": 0, "End": 120, "Properties": {"CameraBindingID": {"Binding": "OutroCam"}}}]}]})json");
	const auto MasterBody = [&Intro, &Outro](int32 OutroStart)
	{
		return FString::Printf(TEXT(R"json({
			"Settings": {"DisplayRate": {"Numerator": 24, "Denominator": 1}, "PlaybackRange": {"Start": 0, "End": %d}},
			"Tracks": [
				{"Type": "Shot", "Sections": [
					{"Start": 0, "End": 288, "Properties": {"SubSequence": "%s"}},
					{"Start": %d, "End": %d, "Properties": {"SubSequence": "%s"}}]},
				{"Type": "CameraCut", "Sections": [{"Start": 0, "End": 168, "Properties": {"CameraBindingID":
					{"Binding": "DollyCam", "Sequence": "/Body/Tracks/0/Sections/0"}}}]}]})json"),
			OutroStart + 120, *Intro.ObjectPath, OutroStart, OutroStart + 120, *Outro.ObjectPath);
	};
	if (!Run(Outro, FromScratch(TEXT("Create"), Outro, OutroBody), TEXT("LS_Outro"))
		|| !Run(Intro, FromScratch(TEXT("Create"), Intro, IntroBody), TEXT("LS_Intro"))
		|| !Run(Master, FromScratch(TEXT("Create"), Master, MasterBody(288)), TEXT("LS_IntroMaster")))
	{
		return false;
	}
	// The event at the last frame calls IntroDone of the intro's compiled Director.
	ULevelSequence* IntroSequence = Intro.GetSequence();
	UBlueprint* Director = DirectorOf(IntroSequence);
	TestTrue(*FString::Printf(TEXT("the intro's Director compiles (%s)"), *StatusText(Director)), Compiled(Director));
	const FMovieSceneEvent* Done = IntroSequence != nullptr ? FirstEvent(FirstSection(IntroSequence->GetMovieScene()->FindTrack<UMovieSceneEventTrack>())) : nullptr;
	TestEqual(TEXT("the last-frame event calls IntroDone"), EndpointName(Done), FString(TEXT("IntroDone")));
	TestTrue(TEXT("IntroDone is in the compiled class"), Director != nullptr && Director->GeneratedClass != nullptr
		&& Director->GeneratedClass->FindFunctionByName(TEXT("IntroDone")) != nullptr);

	// Follow-up 1: "make the dolly ease out": the last dolly key eases out (Auto), the section eases out 24 frames.
	const TSharedPtr<FJsonObject> IntroDocument = ForTarget(Extract(*this, Intro.ObjectPath, TEXT("intro")), Intro, TEXT("Update"));
	if (IntroDocument.IsValid())
	{
		const TSharedPtr<FJsonObject> Dolly = FirstItem(Items(Named(Items(RootBinding(BodyOf(IntroDocument), TEXT("DollyCam")), TEXT("Tracks")), TEXT("Type"), TEXT("Transform")),
			TEXT("Sections")));
		Dolly->SetObjectField(TEXT("Easing"), Parse(TEXT(R"({"Out": 24})")));
		Run(Intro, Serialize(IntroDocument), TEXT("ease out the dolly"));
	}
	// Follow-up 2: "add a camera shake on the reverse shot".
	const TSharedPtr<FJsonObject> ShakeDocument = ForTarget(Extract(*this, Intro.ObjectPath, TEXT("intro again")), Intro, TEXT("Update"));
	if (ShakeDocument.IsValid())
	{
		Append(RootBinding(BodyOf(ShakeDocument), TEXT("ReverseCam")), TEXT("Tracks"), TEXT(R"json({"Type": "CameraShake", "Sections": [{"Start": 168, "End": 288}]})json"));
		Run(Intro, Serialize(ShakeDocument), TEXT("camera shake on the reverse shot"));
	}
	// Follow-up 3: "move the whole outro two seconds later" (48 frames).
	Run(Master, FromScratch(TEXT("Update"), Master, MasterBody(288 + 48)), TEXT("outro two seconds later"));
	ULevelSequence* MasterSequence = Master.GetSequence();
	const UMovieSceneCinematicShotTrack* Shots = MasterSequence != nullptr ? MasterSequence->GetMovieScene()->FindTrack<UMovieSceneCinematicShotTrack>() : nullptr;
	TestTrue(TEXT("the outro shot starts at 14 s"), Shots != nullptr && Shots->GetAllSections().Num() == 2
		&& Shots->GetAllSections()[1]->GetInclusiveStartFrame() == MasterSequence->GetMovieScene()->GetTickResolution().AsFrameNumber(14.0));
	const UMovieSceneCameraCutSection* Cut = MasterSequence != nullptr ? Cast<UMovieSceneCameraCutSection>(FirstSection(MasterSequence->GetMovieScene()->GetCameraCutTrack())) : nullptr;
	const UMovieSceneSubSection* IntroShot = Shots != nullptr ? Cast<UMovieSceneSubSection>(FirstSection(Shots)) : nullptr;
	TestTrue(TEXT("the master's camera cut names the intro's dolly camera"), Cut != nullptr && IntroShot != nullptr
		&& Cut->GetCameraBindingID().GetRelativeSequenceID() == IntroShot->GetSequenceID());
	AddInfo(FString::Printf(TEXT("from scratch: %d applies, %d failed, %.0f ms"), Calls, Failed, Milliseconds(Start)));
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSequencerDirectorInvalidPayloadTest, "UEAssetDoc.Sequencer.Director.InvalidPayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSequencerDirectorInvalidPayloadTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSequencerDirectorTests;
	FFixtures Fixtures;
	FBuilt Built;
	UObject* Base = LoadedFixture(*this, Fixtures, Built, TEXT("LS_Base"));
	if (!TestNotNull(TEXT("fixture"), Base))
	{
		return false;
	}
	// Validate and apply of Document both refuse it with Sequencer.InvalidPayload at a path ending in PathEnd; the
	// sequence extracts as before.
	auto ExpectRefused = [this](const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const TCHAR* Stage, const TCHAR* PathEnd)
	{
		const TSharedPtr<FJsonObject> Before = Extract(*this, Target.ObjectPath, FString(Stage) + TEXT(" before"));
		const FAssetDocumentResult Validated = ValidateDocument(Document);
		TestFalse(*FString::Printf(TEXT("%s: validate refuses it"), Stage), Validated.IsSuccess());
		TestTrue(*FString::Printf(TEXT("%s: validate says Sequencer.InvalidPayload (%s)"), Stage, *DiagnosticsText(Validated)),
			HasCode(Validated, TEXT("Sequencer.InvalidPayload")));
		const FString ValidatePath = FirstPath(Validated, TEXT("Sequencer.InvalidPayload"));
		TestTrue(*FString::Printf(TEXT("%s: at the payload value (%s)"), Stage, *ValidatePath), ValidatePath.EndsWith(PathEnd));
		const FAssetDocumentResult Applied = Apply(Target, Document);
		TestFalse(*FString::Printf(TEXT("%s: apply refuses it"), Stage), Applied.IsSuccess());
		TestTrue(*FString::Printf(TEXT("%s: apply says Sequencer.InvalidPayload (%s)"), Stage, *DiagnosticsText(Applied)),
			HasCode(Applied, TEXT("Sequencer.InvalidPayload")));
		const FAssetDocumentDiagnostic* Diagnostic = Applied.Diagnostics.FindByPredicate([](const FAssetDocumentDiagnostic& Item)
		{
			return Item.Code == TEXT("Sequencer.InvalidPayload");
		});
		TestTrue(*FString::Printf(TEXT("%s: the message names the input, its type and the text"), Stage), Diagnostic != nullptr
			&& Diagnostic->Message.Contains(TEXT("'wide'")) && Diagnostic->Message.Contains(TEXT("'Amount'")) && Diagnostic->Message.Contains(TEXT("Float")));
		TestEqual(*FString::Printf(TEXT("%s: apply at the same path"), Stage), FirstPath(Applied, TEXT("Sequencer.InvalidPayload")), ValidatePath);
		ExpectSame(*this, FString(Stage) + TEXT(": unchanged"), Serialize(Before), Serialize(Extract(*this, Target.ObjectPath, FString(Stage) + TEXT(" after"))));
	};
	auto KeyEvent = [](const TSharedPtr<FJsonObject>& Body)
	{
		const TSharedPtr<FJsonObject> Section = FirstItem(Items(BindingTrack(RootBinding(Body, TEXT("Door")), TEXT("Event")), TEXT("Sections")));
		return Child(FirstItem(Items(Child(Child(Section, TEXT("Channels")), TEXT("Value")), TEXT("Keys"))), TEXT("Event"));
	};

	// 1. A key and its Director written together (the new input is checked by its pin type, before the Director compiles).
	const FTarget& Together = Fixtures.Add(TEXT("LS_PayloadTogether"));
	if (Duplicate(*this, Base, Together) == nullptr)
	{
		return false;
	}
	{
		const TSharedPtr<FJsonObject> Document = ForTarget(Extract(*this, Together.ObjectPath, TEXT("together")), Together, TEXT("Update"));
		BodyOf(Document)->SetObjectField(TEXT("Director"), Parse(OpenDoorDirector));
		Append(RootBinding(BodyOf(Document), TEXT("Door")), TEXT("Tracks"), *FString(OpenDoorTrack).Replace(TEXT("\"2.5\""), TEXT("\"wide\"")));
		ExpectRefused(Together, Document, TEXT("key with its director"), TEXT("/Payload/Amount"));
		TestNull(TEXT("no Director afterwards"), DirectorOf(Together.GetSequence()));
	}

	// 2. The trial's mistake: a key of an existing, compiled Director (the function's parameter imports the text).
	const FTarget& Target = Fixtures.Add(TEXT("LS_PayloadType"));
	if (Duplicate(*this, Base, Target) == nullptr || !DocEdit(*this, Target, TEXT("director and event key"), [](const TSharedPtr<FJsonObject>& Body)
		{
			Body->SetObjectField(TEXT("Director"), Parse(OpenDoorDirector));
			Append(RootBinding(Body, TEXT("Door")), TEXT("Tracks"), OpenDoorTrack);
		}, nullptr))
	{
		return false;
	}
	TestTrue(TEXT("the Director compiles"), Compiled(DirectorOf(Target.GetSequence())));
	{
		const TSharedPtr<FJsonObject> Document = ForTarget(Extract(*this, Target.ObjectPath, TEXT("key")), Target, TEXT("Update"));
		Child(KeyEvent(BodyOf(Document)), TEXT("Payload"))->SetStringField(TEXT("Amount"), TEXT("wide"));
		ExpectRefused(Target, Document, TEXT("key payload"), TEXT("/Payload/Amount"));
	}
	// A value of the input's type as the Details write it is accepted.
	DocEdit(*this, Target, TEXT("valid payload"), [&KeyEvent](const TSharedPtr<FJsonObject>& Body)
		{
			Child(KeyEvent(Body), TEXT("Payload"))->SetStringField(TEXT("Amount"), TEXT("-4.25"));
		}, nullptr);

	// 3. A director binding's payload (the same endpoint as "Replaceable from Director Blueprint").
	{
		const TSharedPtr<FJsonObject> Document = ForTarget(Extract(*this, Target.ObjectPath, TEXT("binding")), Target, TEXT("Update"));
		const TSharedPtr<FJsonObject> Door = RootBinding(BodyOf(Document), TEXT("Door"));
		Door->RemoveField(TEXT("Bind"));  // a binding is one of Bind, Spawnable or CustomBinding
		Door->SetObjectField(TEXT("CustomBinding"), Parse(TEXT(R"json({
			"Class": "/Script/MovieSceneTracks.MovieSceneReplaceableDirectorBlueprintBinding",
			"Properties": {"DynamicBinding": {"Endpoint": "OpenDoor", "Payload": {"Amount": "wide"}}}})json")));
		ExpectRefused(Target, Document, TEXT("director binding payload"), TEXT("/Payload/Amount"));
	}
	return true;
}

#endif
