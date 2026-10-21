// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// SoundCue profile, apply side (docs/architecture/asset-document-soundcue-design.md):
// UEAssetDoc.SoundCue.FreeEdits: design §6.3. The same edit made by a document on one duplicate and by
//   the Sound Cue editor's own calls (palette, wire drag, Break Link, Add / Delete Input, Delete, Details) on another,
//   on SciFiLaserShot_Cue, SFX_PhysicsAttract_Cue and SC_AllNodes: graphs, runtime trees, derived values and statuses
//   must match after every step.
// UEAssetDoc.SoundCue.FromScratch: design §6.4. An AI-written Create (Modulator > Random of four
//   weighted variations, a Concatenator tail, a Switch on "Surface", cue attenuation override, comments) is valid,
//   playable and stable; the error cases fail at their document paths and leave nothing behind.
// UEAssetDoc.SoundCue.Rollback: a failed validation and a failed connect after other edits leave an
//   existing cue as it was (graph and runtime tree).
// UEAssetDoc.SoundCue.Undo: the undo-history rules around a human edit, an AI apply and a failed apply.
// UEAssetDoc.SoundCue.OpenEditorApply: with the cue open in the Sound Cue editor an apply lands in the open
//   editor (its graph panel and selection refreshed as its Undo does), a failed apply rolls back, the editor's own undo /
//   redo after the apply stay whole, and the editor closes.

#include "AssetDocumentSoundCueTestSupport.h"

#include "Algo/Reverse.h"
#include "Framework/Application/SlateApplication.h"
#include "GraphEditor.h"
#include "SGraphPanel.h"
#include "TickableEditorObject.h"
#include "Misc/ScopeExit.h"
#include "Runtime/AssetDocumentUndoHistory.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentSoundCueApplyTests
{
using namespace AssetDocumentSoundCueTests;

const TCHAR* const LaserShot = TEXT("/NetworkPredictionExtras/Audio/SciFiLaserShot_Cue.SciFiLaserShot_Cue");
const TCHAR* const PhysicsAttract = TEXT("/NetworkPredictionExtras/Audio/SFX_PhysicsAttract_Cue.SFX_PhysicsAttract_Cue");
const TCHAR* const SingleWave = TEXT("/Engine/EditorSounds/Notifications/CompileSuccess_Cue.CompileSuccess_Cue");

// Ticks the editor UI: Slate (a graph panel rebuilds its node widgets on a timer after NotifyGraphChanged, when drawn)
// and the editor tickables.
inline void TickSoundCueEditorUi()
{
	for (int32 Tick = 0; Tick < 3; ++Tick)
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().Tick();
		}
		FTickableEditorObject::TickObjects(0.f);
	}
}

// The open graph panel of Graph has a node widget for every node of the graph.
inline bool SoundCuePanelShowsEveryNode(const UEdGraph& Graph)
{
	const TSharedPtr<SGraphEditor> GraphEditor = SGraphEditor::FindGraphEditorForGraph(&Graph);
	const SGraphPanel* Panel = GraphEditor.IsValid() ? GraphEditor->GetGraphPanel() : nullptr;
	if (Panel == nullptr || Graph.Nodes.IsEmpty())
	{
		return false;
	}
	for (const UEdGraphNode* Node : Graph.Nodes)
	{
		if (Node != nullptr && !Panel->GetNodeWidgetFromGuid(Node->NodeGuid).IsValid())
		{
			return false;
		}
	}
	return true;
}

// The first node key of a class in a document (document order).
inline FString KeyOfClass(const TSharedPtr<FJsonObject>& Document, const TCHAR* Class, int32 Skip = 0)
{
	for (const auto& Entry : NodesOf(Document)->Values)
	{
		if (Entry.Value->AsObject()->GetStringField(TEXT("Class")) == Class && Skip-- == 0)
		{
			return FString(*Entry.Key);
		}
	}
	return FString();
}

inline TArray<TSharedPtr<FJsonValue>> InputsOf(const TSharedPtr<FJsonObject>& Document, const FString& Key)
{
	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	return NodeOf(Document, Key)->TryGetArrayField(TEXT("Inputs"), Inputs) ? *Inputs : TArray<TSharedPtr<FJsonValue>>();
}

inline FString InputAt(const TSharedPtr<FJsonObject>& Document, const FString& Key, int32 Slot)
{
	const TArray<TSharedPtr<FJsonValue>> Inputs = InputsOf(Document, Key);
	return Inputs.IsValidIndex(Slot) && !Inputs[Slot]->IsNull() ? Inputs[Slot]->AsString() : FString();
}

inline TSharedPtr<FJsonValue> KeyValue(const FString& Key)
{
	return Key.IsEmpty() ? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>()) : StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueString>(Key));
}

inline TSharedPtr<FJsonObject> PropertiesOf(const TSharedPtr<FJsonObject>& Document, const FString& Key)
{
	const TSharedPtr<FJsonObject> Node = NodeOf(Document, Key);
	if (!Node->HasField(TEXT("Properties")))
	{
		Node->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
	}
	return Node->GetObjectField(TEXT("Properties"));
}

inline TArray<TSharedPtr<FJsonValue>> PositionValue(int32 X, int32 Y)
{
	return {MakeShared<FJsonValueNumber>(X), MakeShared<FJsonValueNumber>(Y)};
}

// The per-child array of a node as the editor holds it now (JSON array of numbers).
inline TArray<TSharedPtr<FJsonValue>> NumbersOf(const USoundCueGraphNode* Node, const TCHAR* Name)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	const FProperty* Property = Node != nullptr ? Node->SoundNode->GetClass()->FindPropertyByName(Name) : nullptr;
	if (Property != nullptr)
	{
		const TSharedPtr<FJsonValue> Json = ValueToJson(EncodePropertyValue(*Property, Property->ContainerPtrToValuePtr<void>(Node->SoundNode.Get())));
		if (Json.IsValid() && Json->Type == EJson::Array)
		{
			Out = Json->AsArray();
		}
	}
	return Out;
}

// One free edit: the editor's calls on the editor side, the document change on the document side.
struct FFreeEdits
{
	FAutomationTestBase& Test;
	const FTarget& DocumentSide;
	const FTarget& EditorSide;
	FString Name;
	int32 Steps = 0;

	USoundCue& EditorCue() const { return *EditorSide.Cue(); }
	FEditorCue Editor() const { return FEditorCue(EditorCue()); }

	TSharedPtr<FJsonObject> Document() const
	{
		return ForTarget(Extract(Test, DocumentSide.ObjectPath, Name + TEXT(" document side")), DocumentSide, TEXT("Update"));
	}

	// Applies Document and compares both sides.
	bool Compare(const TCHAR* Step, const TSharedPtr<FJsonObject>& Document)
	{
		++Steps;
		const FString Stage = FString::Printf(TEXT("%s / %s"), *Name, Step);
		const double Start = FPlatformTime::Seconds();
		if (!ApplyOk(Test, DocumentSide, Document, Stage))
		{
			Test.AddInfo(Serialize(Document).Left(3000));
			return false;
		}
		const double ApplyMs = Milliseconds(Start);
		const FString DocumentForm = CueForm(*DocumentSide.Cue());
		const FString EditorForm = CueForm(EditorCue());
		if (!Test.TestEqual(*FString::Printf(TEXT("%s: document and editor give the same cue"), *Stage), DocumentForm, EditorForm))
		{
			Test.AddInfo(FirstLineDifference(DocumentForm, EditorForm));
			Test.AddInfo(Serialize(Document).Left(3000));
			return false;
		}
		Test.AddInfo(FString::Printf(TEXT("%s: equal (apply %.0f ms)"), *Stage, ApplyMs));
		// The applied document is what the cue holds now.
		FString DiffText;
		if (!Test.TestEqual(*FString::Printf(TEXT("%s: Diff empty"), *Stage), DiffCount(Document, DiffText), 0))
		{
			Test.AddInfo(DiffText);
		}
		return true;
	}
};

inline bool StartBoth(FAutomationTestBase& Test, UObject* Source, const FTarget& DocumentSide, const FTarget& EditorSide, const FString& Name)
{
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s loads"), *Name), Source))
	{
		return false;
	}
	USoundCue* A = Duplicate(Test, Source, DocumentSide);
	USoundCue* B = Duplicate(Test, Source, EditorSide);
	return Test.TestNotNull(*FString::Printf(TEXT("%s: document side"), *Name), A) && Test.TestNotNull(*FString::Printf(TEXT("%s: editor side"), *Name), B)
		&& Test.TestEqual(*FString::Printf(TEXT("%s: duplicates start equal"), *Name), CueForm(*A), CueForm(*B));
}

inline void Position(const FEditorCue& E, const FString& Key, const TSharedPtr<FJsonObject>& Document)
{
	const UEdGraphNode* Node = E.Find(Key);
	if (Node != nullptr)
	{
		NodeOf(Document, Key)->SetArrayField(TEXT("Position"), PositionValue(Node->NodePosX, Node->NodePosY));
	}
}
} // namespace AssetDocumentSoundCueApplyTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSoundCueFreeEditsTest, "UEAssetDoc.SoundCue.FreeEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSoundCueFreeEditsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSoundCueApplyTests;
	FFixtures Fixtures;

	// 1. SciFiLaserShot_Cue: Modulator > Random (3) > players.
	{
		const FTarget& DocumentSide = Fixtures.Add(TEXT("SC_FeLaserDoc"));
		const FTarget& EditorSide = Fixtures.Add(TEXT("SC_FeLaserEd"));
		if (StartBoth(*this, LoadQuiet(LaserShot), DocumentSide, EditorSide, TEXT("SciFiLaserShot_Cue")))
		{
			FFreeEdits Edits{*this, DocumentSide, EditorSide, TEXT("SciFiLaserShot_Cue")};
			TSharedPtr<FJsonObject> Doc = Edits.Document();
			const FString Mod = KeyOfClass(Doc, TEXT("Modulator"));
			const FString Rand = KeyOfClass(Doc, TEXT("Random"));
			const FEditorCue E = Edits.Editor();
			// Insert an Attenuation node between Modulator and Random.
			{
				const FString Att = E.Add(TEXT("Attenuation"), E.Find(Rand)->NodePosX + 150, E.Find(Rand)->NodePosY - 200);
				E.Connect(Rand, Att, 0);
				E.Connect(Att, Mod, 0);
				NodesOf(Doc)->SetObjectField(Att, Json(TEXT(R"({"Class": "Attenuation"})")));
				NodeOf(Doc, Att)->SetArrayField(TEXT("Inputs"), {KeyValue(Rand)});
				Position(E, Att, Doc);
				NodeOf(Doc, Mod)->SetArrayField(TEXT("Inputs"), {KeyValue(Att)});
				Edits.Compare(TEXT("insert attenuation"), Doc);
			}
			// Add a 4th variation with a weight.
			{
				Doc = Edits.Document();
				E.AddInput(Rand);
				const FString Player = E.Add(TEXT("WavePlayer"), -900, 400);
				E.SetDetail(Player, TEXT("SoundWaveAssetPtr"), FString::Printf(TEXT("\"%s\""), WaveC));
				E.Connect(Player, Rand, 3);
				TArray<TSharedPtr<FJsonValue>> Weights = NumbersOf(E.Sound(Rand), TEXT("Weights"));
				Weights[3] = MakeShared<FJsonValueNumber>(0.25);
				E.SetDetail(Rand, TEXT("Weights"), FString::Printf(TEXT("[%s]"), *FString::JoinBy(Weights, TEXT(","), [](const TSharedPtr<FJsonValue>& V) { return FString::SanitizeFloat(V->AsNumber()); })));
				NodesOf(Doc)->SetObjectField(Player, Json(*FString::Printf(TEXT(R"({"Class": "WavePlayer", "Properties": {"SoundWaveAssetPtr": "%s"}})"), WaveC)));
				Position(E, Player, Doc);
				TArray<TSharedPtr<FJsonValue>> Inputs = InputsOf(Doc, Rand);
				Inputs.Add(KeyValue(Player));
				NodeOf(Doc, Rand)->SetArrayField(TEXT("Inputs"), Inputs);
				PropertiesOf(Doc, Rand)->SetArrayField(TEXT("Weights"), Weights);
				Edits.Compare(TEXT("4th variation"), Doc);
			}
			// Delete the second Random input (later slots shift, as the editor's Delete Input does).
			{
				Doc = Edits.Document();
				TArray<TSharedPtr<FJsonValue>> Weights = NumbersOf(E.Sound(Rand), TEXT("Weights"));
				E.RemoveInput(Rand, 1);
				TArray<TSharedPtr<FJsonValue>> Inputs = InputsOf(Doc, Rand);
				Inputs.RemoveAt(1);
				Weights.RemoveAt(1);
				NodeOf(Doc, Rand)->SetArrayField(TEXT("Inputs"), Inputs);
				PropertiesOf(Doc, Rand)->SetArrayField(TEXT("Weights"), Weights);
				Edits.Compare(TEXT("delete input 2"), Doc);
			}
			// Delete a wave player (the one now on slot 0).
			{
				Doc = Edits.Document();
				const FString Player = InputAt(Doc, Rand, 0);
				E.Delete(Player);
				NodesOf(Doc)->RemoveField(Player);
				TArray<TSharedPtr<FJsonValue>> Inputs = InputsOf(Doc, Rand);
				Inputs[0] = KeyValue(FString());
				NodeOf(Doc, Rand)->SetArrayField(TEXT("Inputs"), Inputs);
				Edits.Compare(TEXT("delete a player"), Doc);
			}
			// Change a wave, retarget the Output to Random, move nodes, comment bubble, add and resize a comment.
			{
				Doc = Edits.Document();
				const FString Player = InputAt(Doc, Rand, 1);
				E.SetDetail(Player, TEXT("SoundWaveAssetPtr"), FString::Printf(TEXT("\"%s\""), WaveD));
				PropertiesOf(Doc, Player)->SetStringField(TEXT("SoundWaveAssetPtr"), WaveD);
				E.Connect(Rand, TEXT(""), 0);
				Child(BodyOf(Doc), TEXT("Root"))->SetStringField(TEXT("Input"), Rand);
				E.Move(Mod, 120, 480);
				NodeOf(Doc, Mod)->SetArrayField(TEXT("Position"), PositionValue(120, 480));
				E.Move(TEXT(""), 400, 40);
				Child(BodyOf(Doc), TEXT("Root"))->SetArrayField(TEXT("Position"), PositionValue(400, 40));
				E.SetBubble(Player, TEXT("louder take"));
				NodeOf(Doc, Player)->SetStringField(TEXT("Comment"), TEXT("louder take"));
				NodeOf(Doc, Player)->SetBoolField(TEXT("CommentVisible"), true);
				const FString Box = E.AddComment(TEXT("Shots"), -1000, -300, 520, 700);
				CommentsOf(Doc)->SetObjectField(Box, Json(TEXT(R"({"Text": "Shots", "Position": [-1000, -300], "Size": [520, 700]})")));
				Child(CommentsOf(Doc), Box)->SetArrayField(TEXT("Position"), PositionValue(E.Find(Box)->NodePosX, E.Find(Box)->NodePosY));
				if (!BodyOf(Doc)->HasField(TEXT("Comments")))
				{
					BodyOf(Doc)->SetObjectField(TEXT("Comments"), CommentsOf(Doc));
				}
				Edits.Compare(TEXT("wave, root, move, bubble, comment"), Doc);
			}
		}
	}

	// 2. SFX_PhysicsAttract_Cue: Mixer (4 inputs, InputVolume).
	{
		const FTarget& DocumentSide = Fixtures.Add(TEXT("SC_FeMixDoc"));
		const FTarget& EditorSide = Fixtures.Add(TEXT("SC_FeMixEd"));
		if (StartBoth(*this, LoadQuiet(PhysicsAttract), DocumentSide, EditorSide, TEXT("SFX_PhysicsAttract_Cue")))
		{
			FFreeEdits Edits{*this, DocumentSide, EditorSide, TEXT("SFX_PhysicsAttract_Cue")};
			TSharedPtr<FJsonObject> Doc = Edits.Document();
			const FString Mixer = KeyOfClass(Doc, TEXT("Mixer"));
			const FEditorCue E = Edits.Editor();
			// Reorder the Mixer's inputs with their volumes (reverse).
			{
				TArray<TSharedPtr<FJsonValue>> Inputs = InputsOf(Doc, Mixer);
				TArray<TSharedPtr<FJsonValue>> Volumes = NumbersOf(E.Sound(Mixer), TEXT("InputVolume"));
				const int32 Count = Inputs.Num();
				TArray<FString> Children;
				for (const TSharedPtr<FJsonValue>& Input : Inputs)
				{
					Children.Add(Input->IsNull() ? FString() : Input->AsString());
				}
				for (int32 Slot = 0; Slot < Count; ++Slot)
				{
					const FString& Wanted = Children[Count - 1 - Slot];
					if (Wanted.IsEmpty())
					{
						E.Break(Mixer, Slot);
					}
					else
					{
						E.Connect(Wanted, Mixer, Slot);
					}
				}
				Algo::Reverse(Inputs);
				Algo::Reverse(Volumes);
				E.SetDetail(Mixer, TEXT("InputVolume"), FString::Printf(TEXT("[%s]"), *FString::JoinBy(Volumes, TEXT(","), [](const TSharedPtr<FJsonValue>& V) { return FString::SanitizeFloat(V->AsNumber()); })));
				NodeOf(Doc, Mixer)->SetArrayField(TEXT("Inputs"), Inputs);
				PropertiesOf(Doc, Mixer)->SetArrayField(TEXT("InputVolume"), Volumes);
				Edits.Compare(TEXT("reorder inputs"), Doc);
			}
			// Replace the Mixer by a Concatenator (the editor: place, add inputs, rewire, delete the Mixer).
			{
				Doc = Edits.Document();
				const TArray<TSharedPtr<FJsonValue>> Inputs = InputsOf(Doc, Mixer);
				const FString Concat = E.Add(TEXT("Concatenator"), E.Find(Mixer)->NodePosX, E.Find(Mixer)->NodePosY + 300);
				while (E.Sound(Concat)->GetInputCount() < Inputs.Num())
				{
					E.AddInput(Concat);
				}
				for (int32 Slot = 0; Slot < Inputs.Num(); ++Slot)
				{
					if (!Inputs[Slot]->IsNull())
					{
						E.Connect(Inputs[Slot]->AsString(), Concat, Slot);
					}
				}
				E.Connect(Concat, TEXT(""), 0);
				E.Delete(Mixer);
				NodesOf(Doc)->RemoveField(Mixer);
				NodesOf(Doc)->SetObjectField(Concat, Json(TEXT(R"({"Class": "Concatenator"})")));
				NodeOf(Doc, Concat)->SetArrayField(TEXT("Inputs"), Inputs);
				Position(E, Concat, Doc);
				Child(BodyOf(Doc), TEXT("Root"))->SetStringField(TEXT("Input"), Concat);
				Edits.Compare(TEXT("Mixer -> Concatenator"), Doc);
			}
		}
	}

	// 3. SC_AllNodes: grow a Switch from 5 to 7 slots (pin names update), then a class change on the same key
	//    (document only: delete + create keeps the key).
	{
		const FTarget& Base = Fixtures.Add(TEXT("SC_FeAllBase"));
		USoundCue* BaseCue = CreateCue(*this, Base);
		if (BaseCue != nullptr && BuildAllNodes(*this, *BaseCue))
		{
			Save(*this, BaseCue, TEXT("SC_AllNodes"));
			ReleaseUndoHistory();
			const FTarget& DocumentSide = Fixtures.Add(TEXT("SC_FeAllDoc"));
			const FTarget& EditorSide = Fixtures.Add(TEXT("SC_FeAllEd"));
			if (StartBoth(*this, BaseCue, DocumentSide, EditorSide, TEXT("SC_AllNodes")))
			{
				FFreeEdits Edits{*this, DocumentSide, EditorSide, TEXT("SC_AllNodes")};
				TSharedPtr<FJsonObject> Doc = Edits.Document();
				const FString Switch = KeyOfClass(Doc, TEXT("Switch"));
				const FEditorCue E = Edits.Editor();
				E.AddInput(Switch);
				E.AddInput(Switch);
				TArray<TSharedPtr<FJsonValue>> Inputs = InputsOf(Doc, Switch);
				Inputs.Add(KeyValue(FString()));
				Inputs.Add(KeyValue(FString()));
				NodeOf(Doc, Switch)->SetArrayField(TEXT("Inputs"), Inputs);
				Edits.Compare(TEXT("Switch 5 -> 7 slots"), Doc);
				// Delete the Switch's second slot (pin names shift).
				Doc = Edits.Document();
				E.RemoveInput(Switch, 1);
				Inputs = InputsOf(Doc, Switch);
				Inputs.RemoveAt(1);
				NodeOf(Doc, Switch)->SetArrayField(TEXT("Inputs"), Inputs);
				Edits.Compare(TEXT("Switch: delete slot 2"), Doc);

				// Class change on a key (document side only): the node is replaced, links follow the document.
				Doc = Edits.Document();
				const FString Random = KeyOfClass(Doc, TEXT("Random"));
				const TArray<TSharedPtr<FJsonValue>> RandomInputs = InputsOf(Doc, Random);
				NodeOf(Doc, Random)->SetStringField(TEXT("Class"), TEXT("Mixer"));
				NodeOf(Doc, Random)->RemoveField(TEXT("Properties"));
				if (ApplyOk(*this, DocumentSide, Doc, TEXT("class change")))
				{
					const TSharedPtr<FJsonObject> After = Extract(*this, DocumentSide.ObjectPath, TEXT("class change"));
					TestEqual(TEXT("class change: the key holds a Mixer"), NodeOf(After, Random)->GetStringField(TEXT("Class")), FString(TEXT("Mixer")));
					auto Joined = [](const TArray<TSharedPtr<FJsonValue>>& Values)
					{
						return FString::JoinBy(Values, TEXT(","), [](const TSharedPtr<FJsonValue>& V) { return V->IsNull() ? FString() : V->AsString(); });
					};
					TestEqual(TEXT("class change: inputs kept"), Joined(InputsOf(After, Random)), Joined(RandomInputs));
					FString DiffText;
					TestEqual(TEXT("class change: Diff empty"), DiffCount(Doc, DiffText), 0);
				}
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSoundCueFromScratchTest, "UEAssetDoc.SoundCue.FromScratch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSoundCueFromScratchTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSoundCueApplyTests;
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("SC_Weapon"));
	// As an AI writes it: short class names, keys, no positions for most nodes, defaults left out.
	const FString Text = FString::Printf(TEXT(R"({"SchemaVersion": 1, "Target": "%s", "Class": "/Script/Engine.SoundCue", "Action": "Create",
		"Properties": {"VolumeMultiplier": 0.8, "bOverrideAttenuation": true},
		"Body": {"Root": {"Input": "Tail"},
			"Nodes": {
				"Tail": {"Class": "Concatenator", "Inputs": ["Shot", "Surface"], "Properties": {"InputVolume": [1.0, 0.6]}},
				"Shot": {"Class": "Modulator", "Inputs": ["Variations"], "Properties": {"PitchMin": 0.92, "PitchMax": 1.08}},
				"Variations": {"Class": "Random", "Inputs": ["ShotA", "ShotB", "ShotC", "ShotD"], "Properties": {"Weights": [1.0, 1.0, 0.5, 0.25]}},
				"Surface": {"Class": "Switch", "Inputs": ["ShotA", "ShotB", "ShotC"], "Properties": {"IntParameterName": "Surface"}},
				"ShotA": {"Class": "WavePlayer", "Properties": {"SoundWaveAssetPtr": "%s"}},
				"ShotB": {"Class": "WavePlayer", "Properties": {"SoundWaveAssetPtr": "%s"}},
				"ShotC": {"Class": "WavePlayer", "Properties": {"SoundWaveAssetPtr": "%s"}, "Comment": "rare", "CommentVisible": true},
				"ShotD": {"Class": "WavePlayer", "Properties": {"SoundWaveAssetPtr": "%s"}}},
			"Comments": {"Layers": {"Text": "Shot layers", "Position": [-1300, -300], "Size": [500, 800], "FontSize": 24}}}})"),
		*Target.PackageName, WaveA, WaveB, WaveC, WaveD);
	const TSharedPtr<FJsonObject> Document = Json(*Text);
	const double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Created = Apply(Target, Document);
	if (!TestTrue(TEXT("weapon cue created"), Created.IsSuccess()))
	{
		AddError(DiagnosticsText(Created));
		return false;
	}
	AddInfo(FString::Printf(TEXT("weapon cue created in %.0f ms, status %s"), Milliseconds(Start), *StatusOf(Created)));
	USoundCue* Cue = Target.Cue();
	if (!TestNotNull(TEXT("weapon cue"), Cue))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Extracted = Extract(*this, Target.ObjectPath, TEXT("weapon"));
	const FAssetDocumentResult ExtractedResult = ExtractResult(Target.ObjectPath);
	TestEqual(TEXT("status UpToDate"), ExtractedResult.Compile.IsValid() ? ExtractedResult.Compile->GetStringField(TEXT("Status")) : FString(), FString(TEXT("UpToDate")));
	TestTrue(TEXT("playable"), Cue->IsPlayable());
	TestTrue(TEXT("has a duration"), Cue->GetDuration() > 0.0f);
	TestTrue(TEXT("FirstNode is the Concatenator"), Cue->FirstNode != nullptr && Cue->FirstNode->GetName() == TEXT("Tail"));
	TestEqual(TEXT("eight sound nodes"), Cue->AllNodes.Num(), 8);
	FString DiffText;
	const int32 Changes = DiffCount(ForTarget(Document, Target, TEXT("Update")), DiffText);
	if (!TestEqual(TEXT("the AI document Diffs empty against the cue"), Changes, 0))
	{
		AddInfo(DiffText);
	}
	AddInfo(FString::Printf(TEXT("weapon extract: %s"), *Serialize(Extracted)));
	// Re-applying the AI's own document changes nothing.
	const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Target.PackageFile);
	const FAssetDocumentResult Again = Apply(Target, ForTarget(Document, Target, TEXT("Update")));
	TestTrue(TEXT("re-apply succeeds"), Again.IsSuccess());
	if (!TestFalse(TEXT("re-apply saves nothing"), Again.bSavedAsset || IFileManager::Get().GetTimeStamp(*Target.PackageFile) != Stamp))
	{
		AddInfo(FString::Printf(TEXT("re-apply: saved=%d stamp %s -> %s: %s"), Again.bSavedAsset ? 1 : 0, *Stamp.ToString(),
			*IFileManager::Get().GetTimeStamp(*Target.PackageFile).ToString(), Again.Payload.IsValid() ? *Serialize(Again.Payload) : TEXT("(no payload)")));
	}

	// Error cases of a Create: each fails at its document path and leaves no asset.
	const FString Wave = FString::Printf(TEXT(R"("W": {"Class": "WavePlayer", "Properties": {"SoundWaveAssetPtr": "%s"}})"), WaveA);
	const FString NotAWave = FString::Printf(TEXT(R"("W": {"Class": "WavePlayer", "Properties": {"SoundWaveAssetPtr": "%s"}})"), SingleWave);
	TArray<FString> Nulls;
	for (int32 Index = 0; Index < 33; ++Index)
	{
		Nulls.Add(TEXT("null"));
	}
	const FString Mixer33 = FString::Printf(TEXT(R"("A": {"Class": "Mixer", "Inputs": [%s]})"), *FString::Join(Nulls, TEXT(",")));
	struct FCase
	{
		FString Name;
		FString Nodes;
		FString Code;
		FString Path;
	};
	const TArray<FCase> Cases = {
		{TEXT("unknown class"), FString::Printf(TEXT(R"("A": {"Class": "Reverberator"}, %s)"), *Wave), TEXT("SoundCue.UnknownNodeClass"), TEXT("/Body/Nodes/A/Class")},
		{TEXT("cycle"), FString::Printf(TEXT(R"("A": {"Class": "Modulator", "Inputs": ["B"]}, "B": {"Class": "Modulator", "Inputs": ["A"]}, %s)"), *Wave), TEXT("SoundCue.ConnectionCycle"), TEXT("/Body/Nodes/B/Inputs/0")},
		{TEXT("33 inputs"), Mixer33, TEXT("SoundCue.ChildCount"), TEXT("/Body/Nodes/A/Inputs")},
		{TEXT("Branch with 2 inputs"), TEXT(R"("A": {"Class": "Branch", "Inputs": [null, null]})"), TEXT("SoundCue.ChildCount"), TEXT("/Body/Nodes/A/Inputs")},
		{TEXT("Weights length"), FString::Printf(TEXT(R"("A": {"Class": "Random", "Inputs": ["W", "W"], "Properties": {"Weights": [1.0]}}, %s)"), *Wave), TEXT("SoundCue.ChildArrayLength"), TEXT("/Body/Nodes/A/Properties/Weights")},
		{TEXT("unknown key"), TEXT(R"("A": {"Class": "Random", "Inputs": ["W", "Nope"]})"), TEXT("SoundCue.UnknownKey"), TEXT("/Body/Nodes/A/Inputs/0")},
		{TEXT("Delay without a slot"), FString::Printf(TEXT(R"("A": {"Class": "Delay", "Inputs": []}, %s)"), *Wave), TEXT("SoundCue.ValidationFailed"), TEXT("/Body/Nodes/A")},
		{TEXT("a cue as the wave"), NotAWave, FString(), TEXT("/Body/Nodes/W")},
	};
	for (const FCase& Case : Cases)
	{
		const FTarget& Failed = Fixtures.Add(TEXT("SC_Bad"));
		const FString Root = Case.Nodes.StartsWith(TEXT("\"W\"")) ? TEXT("W") : TEXT("A");
		const TSharedPtr<FJsonObject> Bad = Json(*FString::Printf(TEXT(R"({"SchemaVersion": 1, "Target": "%s", "Class": "/Script/Engine.SoundCue", "Action": "Create", "Body": {"Root": {"Input": "%s"}, "Nodes": {%s}}})"),
			*Failed.PackageName, *Root, *Case.Nodes));
		if (!TestTrue(*FString::Printf(TEXT("%s: document parses"), *Case.Name), Bad.IsValid()))
		{
			continue;
		}
		const FAssetDocumentResult Result = Apply(Failed, Bad);
		TestFalse(*FString::Printf(TEXT("%s: refused"), *Case.Name), Result.IsSuccess());
		const FAssetDocumentDiagnostic* First = Result.Diagnostics.IsEmpty() ? nullptr : &Result.Diagnostics[0];
		AddInfo(FString::Printf(TEXT("%s: %s"), *Case.Name, *DiagnosticsText(Result)));
		if (!Case.Code.IsEmpty())
		{
			TestTrue(*FString::Printf(TEXT("%s: %s"), *Case.Name, *Case.Code), HasCode(Result, *Case.Code));
			const FAssetDocumentDiagnostic* Found = Result.Diagnostics.FindByPredicate([&Case](const FAssetDocumentDiagnostic& D) { return D.Code == Case.Code; });
			TestEqual(*FString::Printf(TEXT("%s: path"), *Case.Name), Found != nullptr ? Found->Path : FString(), Case.Path);
		}
		else
		{
			TestTrue(*FString::Printf(TEXT("%s: at %s"), *Case.Name, *Case.Path), First != nullptr && First->Path.StartsWith(Case.Path));
		}
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestNull(*FString::Printf(TEXT("%s: no object left"), *Case.Name), Failed.Get());
		TestFalse(*FString::Printf(TEXT("%s: no package file"), *Case.Name), IFileManager::Get().FileExists(*Failed.PackageFile));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSoundCueRollbackTest, "UEAssetDoc.SoundCue.Rollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSoundCueRollbackTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSoundCueApplyTests;
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("SC_Rollback"));
	USoundCue* Cue = Duplicate(*this, LoadQuiet(LaserShot), Target);
	if (!TestNotNull(TEXT("SciFiLaserShot_Cue duplicate"), Cue))
	{
		return false;
	}
	const FString Before = CueForm(*Cue);
	const TSharedPtr<FJsonObject> Base = ForTarget(Extract(*this, Target.ObjectPath, TEXT("rollback base")), Target, TEXT("Update"));
	const FString Mod = KeyOfClass(Base, TEXT("Modulator"));
	const FString Rand = KeyOfClass(Base, TEXT("Random"));
	// Several edits that succeed (new nodes, a rewire, a property, a move, a comment), then a validation error.
	{
		const TSharedPtr<FJsonObject> Doc = Parse(Serialize(Base));
		// A wave player given a Sound Cue: only the cue's validation (finalize) finds it.
		NodesOf(Doc)->SetObjectField(TEXT("Wait"), Json(*FString::Printf(TEXT(R"({"Class": "WavePlayer", "Position": [-100, 300], "Properties": {"SoundWaveAssetPtr": "%s"}})"), SingleWave)));
		NodesOf(Doc)->SetObjectField(TEXT("Att"), Json(TEXT(R"({"Class": "Attenuation", "Position": [-100, 0]})")));
		NodeOf(Doc, TEXT("Att"))->SetArrayField(TEXT("Inputs"), {KeyValue(Rand)});
		NodeOf(Doc, Mod)->SetArrayField(TEXT("Inputs"), {KeyValue(TEXT("Att"))});
		PropertiesOf(Doc, Mod)->SetNumberField(TEXT("PitchMax"), 1.3);
		NodeOf(Doc, Rand)->SetArrayField(TEXT("Position"), PositionValue(-777, 77));
		BodyOf(Doc)->SetObjectField(TEXT("Comments"), Json(TEXT(R"({"Note": {"Text": "x", "Position": [0, 0], "Size": [300, 200]}})")));
		ExpectRefused(*this, Target, Doc, TEXT("SoundCue.ValidationFailed"), TEXT("/Body/Nodes/Wait"), TEXT("failed validation"));
		TestEqual(TEXT("failed validation: graph and runtime tree as before"), CueForm(*Target.Cue()), Before);
	}
	// The same edits, then a connect the schema refuses (a loop).
	{
		const TSharedPtr<FJsonObject> Doc = Parse(Serialize(Base));
		NodesOf(Doc)->SetObjectField(TEXT("Att"), Json(TEXT(R"({"Class": "Attenuation", "Position": [-100, 0]})")));
		NodeOf(Doc, TEXT("Att"))->SetArrayField(TEXT("Inputs"), {KeyValue(Mod)});
		NodeOf(Doc, Mod)->SetArrayField(TEXT("Inputs"), {KeyValue(TEXT("Att"))});
		PropertiesOf(Doc, Mod)->SetNumberField(TEXT("PitchMax"), 1.3);
		// Slots connect in document order: Mod <- Att succeeds, then Att <- Mod closes the loop.
		ExpectRefused(*this, Target, Doc, TEXT("SoundCue.ConnectionCycle"), TEXT("/Body/Nodes/Att/Inputs/0"), TEXT("failed connect"));
		TestEqual(TEXT("failed connect: graph and runtime tree as before"), CueForm(*Target.Cue()), Before);
	}
	// The cue still applies normally afterwards.
	{
		const TSharedPtr<FJsonObject> Doc = Parse(Serialize(Base));
		PropertiesOf(Doc, Mod)->SetNumberField(TEXT("PitchMax"), 1.3);
		TestTrue(TEXT("apply after the rollbacks"), ApplyOk(*this, Target, Doc, TEXT("after rollbacks")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSoundCueUndoTest, "UEAssetDoc.SoundCue.Undo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSoundCueUndoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSoundCueApplyTests;
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("SC_Undo"));
	USoundCue* Cue = Duplicate(*this, LoadQuiet(LaserShot), Target);
	if (!TestNotNull(TEXT("duplicate"), Cue))
	{
		return false;
	}
	ReleaseUndoHistory();
	const FString Rand = KeyOfClass(Extract(*this, Target.ObjectPath, TEXT("undo base")), TEXT("Random"));
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

	// 1. The human moves a node and saves; the AI edits: history referencing the cue is cleared.
	FEditorCue(*Target.Cue()).Move(Rand, -1900, 0);
	Save(*this, Target.Get(), TEXT("human save"));
	TestTrue(TEXT("the human edit is in the undo history"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const TSharedPtr<FJsonObject> Human = ForTarget(Extract(*this, Target.ObjectPath, TEXT("after human edit")), Target, TEXT("Update"));
	NodesOf(Human)->SetObjectField(TEXT("Extra"), Json(*FString::Printf(TEXT(R"({"Class": "WavePlayer", "Position": [0, 900], "Properties": {"SoundWaveAssetPtr": "%s"}})"), WaveA)));
	if (!ApplyOk(*this, Target, Human, TEXT("AI edit")))
	{
		return false;
	}
	TestFalse(TEXT("after AI edit: no undo history references the cue"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const FString AfterEdit = Serialize(Extract(*this, Target.ObjectPath, TEXT("after AI edit")));
	TestTrue(TEXT("human move kept"), AfterEdit.Contains(TEXT("[-1900,0]")));
	UndoRedoAll(TEXT("after AI edit"));
	TestEqual(TEXT("after AI edit + undo/redo"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after undo/redo"))), AfterEdit);

	// 2. The human edits again; an AI apply fails and rolls back to the human state; undo stays safe.
	ReleaseUndoHistory();
	FEditorCue(*Target.Cue()).Move(Rand, -2000, 0);
	const FString SecondHuman = Serialize(Extract(*this, Target.ObjectPath, TEXT("after second human edit")));
	const TSharedPtr<FJsonObject> Failing = ForTarget(Parse(SecondHuman), Target, TEXT("Update"));
	// Fails in finalize, after the spawn and Details edit recorded transactions: a wave player given a Sound Cue.
	NodesOf(Failing)->SetObjectField(TEXT("Wait"), Json(*FString::Printf(TEXT(R"({"Class": "WavePlayer", "Properties": {"SoundWaveAssetPtr": "%s"}})"), SingleWave)));
	const FAssetDocumentResult Failed = Apply(Target, Failing);
	TestFalse(TEXT("failing apply fails"), Failed.IsSuccess());
	TestEqual(TEXT("after rollback: the human state"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after rollback"))), SecondHuman);
	AddInfo(FString::Printf(TEXT("after the failed apply the undo history %s"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName)
		? TEXT("keeps the human edit") : TEXT("was cleared (the failed edit recorded transactions)")));
	UndoRedoAll(TEXT("after rollback"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSoundCueOpenEditorApplyTest, "UEAssetDoc.SoundCue.OpenEditorApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSoundCueOpenEditorApplyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSoundCueApplyTests;
	UAssetEditorSubsystem* Editors = GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("asset editor subsystem"), Editors) || GEditor->Trans == nullptr)
	{
		return false;
	}
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("SC_Open"));
	USoundCue* Cue = Duplicate(*this, LoadQuiet(SingleWave), Target);
	if (!TestNotNull(TEXT("duplicate"), Cue))
	{
		return false;
	}
	if (!Editors->OpenEditorForAsset(Cue))
	{
		AddWarning(TEXT("The Sound Cue editor did not open in this session; the open-editor apply was not exercised."));
		return true;
	}
	ON_SCOPE_EXIT
	{
		if (UObject* Open = Target.Get())
		{
			Editors->CloseAllEditorsForAsset(Open);
		}
	};
	TickSoundCueEditorUi();
	// The human does not save after opening: the Sound Cue editor changes nothing while opening.
	TestFalse(TEXT("opening the editor leaves the package clean"), Cue->GetOutermost()->IsDirty());
	TSharedPtr<SGraphEditor> GraphEditor = SGraphEditor::FindGraphEditorForGraph(Cue->SoundCueGraph);
	if (!TestTrue(TEXT("the open editor shows the cue's graph"), GraphEditor.IsValid() && GraphEditor->GetCurrentGraph() == Cue->SoundCueGraph))
	{
		return false;
	}
	// The panel builds its node widgets when it is drawn; an unattended session may never draw it.
	const bool bPainted = SoundCuePanelShowsEveryNode(*Cue->SoundCueGraph);
	AddInfo(FString::Printf(TEXT("the graph panel %s"), bPainted ? TEXT("is drawn (node widgets checked)") : TEXT("is not drawn in this session (node widgets not checked)")));
	ReleaseUndoHistory();

	// The human selects the wave player in the open editor; an apply puts a Delay between it and the Output.
	const TSharedPtr<FJsonObject> Base = ForTarget(Extract(*this, Target.ObjectPath, TEXT("open base")), Target, TEXT("Update"));
	const FString Wave = KeyOfClass(Base, TEXT("WavePlayer"));
	GraphEditor->SetNodeSelection(FEditorCue(*Cue).Find(Wave), true);
	TestEqual(TEXT("the human's selection"), GraphEditor->GetSelectedNodes().Num(), 1);
	const TSharedPtr<FJsonObject> Edit = ForTarget(Base, Target, TEXT("Update"));
	NodesOf(Edit)->SetObjectField(TEXT("Extra"), Json(*FString::Printf(TEXT(R"({"Class": "Delay", "Position": [-150, 0], "Inputs": ["%s"]})"), *Wave)));
	Child(BodyOf(Edit), TEXT("Root"))->SetField(TEXT("Input"), KeyValue(TEXT("Extra")));
	if (!ApplyOk(*this, Target, Edit, TEXT("apply while open")))
	{
		return false;
	}
	Cue = Target.Cue();
	if (!TestNotNull(TEXT("the cue after the apply"), Cue))
	{
		return false;
	}
	TestTrue(TEXT("the cue plays the Delay first"), Cue->FirstNode != nullptr && Cue->FirstNode->GetName() == TEXT("Extra"));
	TestFalse(TEXT("the apply is not an undo step"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	TestTrue(TEXT("the editor is still open"), Editors->FindEditorForAsset(Cue, false) != nullptr);
	GraphEditor = SGraphEditor::FindGraphEditorForGraph(Cue->SoundCueGraph);
	TestTrue(TEXT("the open editor still shows the cue's graph"), GraphEditor.IsValid() && GraphEditor->GetCurrentGraph() == Cue->SoundCueGraph);
	TestEqual(TEXT("the open editor dropped its selection (as its Undo does)"), GraphEditor.IsValid() ? GraphEditor->GetSelectedNodes().Num() : -1, 0);
	const UEdGraphNode* DelayNode = FEditorCue(*Cue).Find(TEXT("Extra"));
	TestNotNull(TEXT("the graph has the Delay node"), DelayNode);
	const FGuid DelayGuid = DelayNode != nullptr ? DelayNode->NodeGuid : FGuid();
	TickSoundCueEditorUi();
	if (bPainted)
	{
		TestTrue(TEXT("the graph panel shows every node after the apply"), SoundCuePanelShowsEveryNode(*Cue->SoundCueGraph));
	}
	const FString Applied = CueForm(*Cue);
	const FString AppliedDocument = Serialize(Extract(*this, Target.ObjectPath, TEXT("after apply while open")));

	// A failing apply (a wave player given a Sound Cue: validation after the edits) rolls back with the editor open.
	{
		const TSharedPtr<FJsonObject> Failing = ForTarget(Parse(AppliedDocument), Target, TEXT("Update"));
		NodesOf(Failing)->SetObjectField(TEXT("Wait"), Json(*FString::Printf(TEXT(R"({"Class": "WavePlayer", "Properties": {"SoundWaveAssetPtr": "%s"}})"), SingleWave)));
		const FAssetDocumentResult Failed = Apply(Target, Failing);
		TestFalse(TEXT("failing apply while open fails"), Failed.IsSuccess());
		Cue = Target.Cue();
		if (!TestNotNull(TEXT("the cue after the rollback"), Cue))
		{
			return false;
		}
		TestEqual(TEXT("after the rollback the cue is as applied"), CueForm(*Cue), Applied);
		GraphEditor = SGraphEditor::FindGraphEditorForGraph(Cue->SoundCueGraph);
		TestTrue(TEXT("after the rollback the open editor shows the cue's graph"), GraphEditor.IsValid() && GraphEditor->GetCurrentGraph() == Cue->SoundCueGraph);
		TestNull(TEXT("after the rollback the graph has no failed node"), FEditorCue(*Cue).Find(TEXT("Wait")));
		TickSoundCueEditorUi();
		if (bPainted)
		{
			TestTrue(TEXT("after the rollback the graph panel shows every node"), SoundCuePanelShowsEveryNode(*Cue->SoundCueGraph));
		}
	}

	// The editor's own edit and undo / redo after the apply.
	{
		FEditorCue(*Cue).Move(Wave, -900, 0);
		const FString Human = Serialize(Extract(*this, Target.ObjectPath, TEXT("after the human edit")));
		TestTrue(TEXT("the human edit landed"), Human.Contains(TEXT("[-900,0]")));
		TestTrue(TEXT("the human edit undoes"), GEditor->Trans->CanUndo() && GEditor->UndoTransaction());
		TestEqual(TEXT("undo restores the applied cue"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after undo"))), AppliedDocument);
		TestTrue(TEXT("the human edit redoes"), GEditor->Trans->CanRedo() && GEditor->RedoTransaction());
		TestEqual(TEXT("redo restores the human edit"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after redo"))), Human);
		TickSoundCueEditorUi();
		// Applies need the human's changes saved first (as for every asset).
		Save(*this, Cue, TEXT("the human edit"));
	}

	// A second apply removes the Delay: the panel drops its widget.
	{
		const TSharedPtr<FJsonObject> Again = ForTarget(Extract(*this, Target.ObjectPath, TEXT("before the second apply")), Target, TEXT("Update"));
		NodesOf(Again)->RemoveField(TEXT("Extra"));
		Child(BodyOf(Again), TEXT("Root"))->SetField(TEXT("Input"), KeyValue(Wave));
		const bool bAppliedAgain = ApplyOk(*this, Target, Again, TEXT("second apply while open"));
		Cue = Target.Cue();
		if (bAppliedAgain && TestNotNull(TEXT("the cue after the second apply"), Cue))
		{
			TestNull(TEXT("the graph has no Delay node"), FEditorCue(*Cue).Find(TEXT("Extra")));
			TickSoundCueEditorUi();
			GraphEditor = SGraphEditor::FindGraphEditorForGraph(Cue->SoundCueGraph);
			SGraphPanel* Panel = GraphEditor.IsValid() ? GraphEditor->GetGraphPanel() : nullptr;
			TestFalse(TEXT("the graph panel holds no widget of the removed node"), Panel != nullptr && Panel->GetNodeWidgetFromGuid(DelayGuid).IsValid());
			if (bPainted)
			{
				TestTrue(TEXT("after the second apply the graph panel shows every node"), SoundCuePanelShowsEveryNode(*Cue->SoundCueGraph));
			}
		}
	}
	if (Cue != nullptr)
	{
		TestTrue(TEXT("the cue is still open in its editor"), Editors->FindEditorForAsset(Cue, false) != nullptr);
		Editors->CloseAllEditorsForAsset(Cue);
		TestNull(TEXT("the editor closed"), Editors->FindEditorForAsset(Cue, false));
	}
	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
