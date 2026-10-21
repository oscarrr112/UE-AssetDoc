// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// SoundCue profile, read side and identity (docs/architecture/asset-document-soundcue-design.md):
// UEAssetDoc.SoundCue.ListTypes: list_types kind sound_node (the palette's classes, inputs, defaults).
// UEAssetDoc.SoundCue.Read: Extract / Inspect of the editor-built SC_AllNodes fixture: every node and
//   link, graph.query's input pins (Inspect NodeFacts) equal to the editor's pins, status, waves; reading changes
//   nothing.
// UEAssetDoc.SoundCue.Corpus: design §6.2 on the shipped cues (NetworkPredictionExtras included,
//   VirtualScouting when mounted, the XRFramework template's Fire_Cue) and the fixtures SC_AllNodes, SC_Quality,
//   SC_FromFactory: Create from the extract gives the same document, graph, runtime tree, derived values and status;
//   Diff is empty, the re-apply is a no-op, the reload is stable. Timings are reported.
// UEAssetDoc.SoundCue.EditorDelete: behaviour test of the delete replica (rule B1): the Sound Cue
//   editor's own Delete command on an open editor vs DeleteNodeAsTheEditorDoes on a second duplicate.
// UEAssetDoc.SoundCue.RenamedNodes: nodes keyed by the document (renamed sound node / comment objects)
//   save, reload, duplicate and still reference their waves.

#include "AssetDocumentSoundCueTestSupport.h"

#include "GraphEditor.h"
#include "Input/Events.h"
#include "Misc/ScopeExit.h"
#include "Sound/SoundNodeWavePlayer.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentSoundCueTests
{
struct FCorpusEntry
{
	const TCHAR* Path;
	const TCHAR* Why;
	bool bRequired;
};

const FCorpusEntry Corpus[] = {
	{TEXT("/Engine/EditorSounds/Notifications/CompileSuccess_Cue.CompileSuccess_Cue"), TEXT("single player"), true},
	{TEXT("/Engine/VREditor/Sounds/VR_object_grabbed_loop_Cue.VR_object_grabbed_loop_Cue"), TEXT("looping player"), true},
	{TEXT("/Engine/VREditor/Sounds/UI/Click_on_Button_Cue.Click_on_Button_Cue"), TEXT("single player"), true},
	{TEXT("/NetworkPredictionExtras/Audio/SciFiLaserShot_Cue.SciFiLaserShot_Cue"), TEXT("Modulator > Random > 3 players"), true},
	{TEXT("/NetworkPredictionExtras/Audio/SFX_Ship_HitGround_01_Cue.SFX_Ship_HitGround_01_Cue"), TEXT("Random + Modulator ranges"), true},
	{TEXT("/NetworkPredictionExtras/Audio/SFX_PhysicsAttract_Cue.SFX_PhysicsAttract_Cue"), TEXT("Mixer, attenuation override"), true},
	{TEXT("/VirtualScouting/Audio/SC_Click_03.SC_Click_03"), TEXT("Mixer + Modulator (optional plugin)"), false},
	{TEXT("/VirtualScouting/Audio/SC_buttonPush.SC_buttonPush"), TEXT("Mixer + Modulator (optional plugin)"), false},
	{TEXT("/Game/XRFramework/Audio/Fire_Cue.Fire_Cue"), TEXT("template copy, NaturalSound override"), true},
};

// One §6.2 round trip of Source (a duplicate in /Game is the input), reported per asset.
inline void CorpusCase(FAutomationTestBase& Test, FFixtures& Fixtures, UObject* Source, const FString& Name, const FString& Why)
{
	const FTarget& Input = Fixtures.Add(TEXT("SC_In"));
	USoundCue* Cue = Duplicate(Test, Source, Input);
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s: duplicate"), *Name), Cue))
	{
		return;
	}
	double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult First = ExtractResult(Input.ObjectPath);
	const double ExtractMs = Milliseconds(Start);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract"), *Name), First.IsSuccess() && First.Payload.IsValid()))
	{
		Test.AddError(FString::Printf(TEXT("%s: %s"), *Name, *DiagnosticsText(First)));
		return;
	}
	const FString Again = Serialize(Extract(Test, Input.ObjectPath, Name + TEXT(" again")));
	Test.TestEqual(*FString::Printf(TEXT("%s: extract is stable"), *Name), Again, Serialize(First.Payload));
	const FTarget& Created = Fixtures.Add(TEXT("SC_Out"));
	const FRoundTrip Trip = RoundTrip(Test, *Cue, First.Payload, Created, Name);
	const int32 Nodes = NodesOf(First.Payload)->Values.Num();
	Test.AddInfo(FString::Printf(TEXT("%s (%s): %d nodes, %d comments, status %s; extract %.0f ms, %s"), *Name, *Why, Nodes,
		CommentsOf(First.Payload)->Values.Num(), *StatusOf(First), ExtractMs, *Trip.Timings()));
	for (const FString& Problem : Trip.Problems)
	{
		Test.AddError(FString::Printf(TEXT("%s: %s"), *Name, *Problem));
	}
	if (!Trip.Problems.IsEmpty())
	{
		Test.AddInfo(FString::Printf(TEXT("%s document: %s"), *Name, *Serialize(First.Payload).Left(3000)));
	}
}

// The editor-built SC_AllNodes fixture, saved.
inline USoundCue* MakeAllNodes(FAutomationTestBase& Test, const FTarget& Target)
{
	USoundCue* Cue = CreateCue(Test, Target);
	if (Cue == nullptr || !BuildAllNodes(Test, *Cue))
	{
		return nullptr;
	}
	Save(Test, Cue, TEXT("SC_AllNodes"));
	ReleaseUndoHistory();
	return Cue;
}
} // namespace AssetDocumentSoundCueTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSoundCueListTypesTest, "UEAssetDoc.SoundCue.ListTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSoundCueListTypesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSoundCueTests;
	const FAssetDocumentService& Service = FAssetDocumentModule::Get().GetService();
	auto List = [&](const TMap<FString, FString>& Query, const TCHAR* Stage) -> TArray<TSharedPtr<FJsonValue>>
	{
		const FAssetDocumentResult Result = Service.ListTypes(Query);
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!TestTrue(*FString::Printf(TEXT("%s: lists"), Stage), Result.IsSuccess() && Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("Types"), Rows)))
		{
			AddInfo(DiagnosticsText(Result));
			return {};
		}
		return *Rows;
	};
	auto Find = [](const TArray<TSharedPtr<FJsonValue>>& Rows, const TCHAR* Class) -> TSharedPtr<FJsonObject>
	{
		for (const TSharedPtr<FJsonValue>& Row : Rows)
		{
			if (Row->AsObject()->GetStringField(TEXT("Class")) == Class)
			{
				return Row->AsObject();
			}
		}
		return nullptr;
	};
	const TArray<TSharedPtr<FJsonValue>> All = List({{TEXT("kind"), TEXT("sound_node")}, {TEXT("limit"), TEXT("200")}}, TEXT("all sound nodes"));
	AddInfo(FString::Printf(TEXT("%d sound node classes"), All.Num()));
	for (const TCHAR* Class : {TEXT("WavePlayer"), TEXT("Random"), TEXT("Mixer"), TEXT("Modulator"), TEXT("ModulatorContinuous"), TEXT("Attenuation"),
		TEXT("Looping"), TEXT("Delay"), TEXT("Switch"), TEXT("Branch"), TEXT("Concatenator"), TEXT("Enveloper"), TEXT("Oscillator"), TEXT("Doppler"),
		TEXT("SoundClass"), TEXT("WaveParam"), TEXT("Mature"), TEXT("GroupControl"), TEXT("DistanceCrossFade"), TEXT("ParamCrossFade"),
		TEXT("QualityLevel"), TEXT("DialoguePlayer")})
	{
		TestTrue(*FString::Printf(TEXT("%s is listed"), Class), Find(All, Class).IsValid());
	}
	if (const TSharedPtr<FJsonObject> Random = Find(All, TEXT("Random")))
	{
		AddInfo(FString::Printf(TEXT("Random row: %s"), *Serialize(Random)));
		TestEqual(TEXT("Random starts with 2 inputs"), static_cast<int32>(Random->GetNumberField(TEXT("Inputs"))), 2);
		TestEqual(TEXT("Random takes up to 32"), static_cast<int32>(Random->GetNumberField(TEXT("MaxInputs"))), 32);
		TestTrue(TEXT("Random defaults list Weights"), Child(Random, TEXT("Properties"))->HasField(TEXT("Weights")));
	}
	if (const TSharedPtr<FJsonObject> Branch = Find(All, TEXT("Branch")))
	{
		TestEqual(TEXT("Branch takes exactly 3"), static_cast<int32>(Branch->GetNumberField(TEXT("MinInputs"))), 3);
		TestEqual(TEXT("Branch max 3"), static_cast<int32>(Branch->GetNumberField(TEXT("MaxInputs"))), 3);
	}
	if (const TSharedPtr<FJsonObject> Player = Find(All, TEXT("WavePlayer")))
	{
		TestEqual(TEXT("players take no input"), static_cast<int32>(Player->GetNumberField(TEXT("MaxInputs"))), 0);
	}
	const TArray<TSharedPtr<FJsonValue>> Named = List({{TEXT("kind"), TEXT("sound_node")}, {TEXT("name"), TEXT("Modulator*")}}, TEXT("named"));
	TestEqual(TEXT("name wildcard"), Named.Num(), 2);
	const FAssetDocumentResult Unknown = Service.ListTypes({{TEXT("kind"), TEXT("soundnode")}});
	TestFalse(TEXT("unknown kind fails"), Unknown.IsSuccess());
	TestTrue(TEXT("unknown kind names sound_node"), Unknown.Message.Contains(TEXT("sound_node")) || DiagnosticsText(Unknown).Contains(TEXT("sound_node")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSoundCueReadTest, "UEAssetDoc.SoundCue.Read",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSoundCueReadTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSoundCueTests;
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("SC_AllNodes"));
	USoundCue* Cue = MakeAllNodes(*this, Target);
	if (!TestNotNull(TEXT("SC_AllNodes"), Cue))
	{
		return false;
	}
	const FEditorCue E(*Cue);
	const FString FormBefore = CueForm(*Cue);
	const double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Result = ExtractResult(Target.ObjectPath);
	AddInfo(FString::Printf(TEXT("SC_AllNodes extract %.0f ms, status %s"), Milliseconds(Start), *StatusOf(Result)));
	if (!TestTrue(TEXT("extract"), Result.IsSuccess() && Result.Payload.IsValid()))
	{
		AddError(DiagnosticsText(Result));
		return false;
	}
	const TSharedPtr<FJsonObject> Document = Result.Payload;
	AddInfo(Serialize(Document).Left(6000));
	TestEqual(TEXT("one Nodes entry per sound node"), NodesOf(Document)->Values.Num(), Cue->AllNodes.Num());
	TestEqual(TEXT("two comments"), CommentsOf(Document)->Values.Num(), 2);
	TestTrue(TEXT("Root.Input is the Mixer"), Cue->FirstNode != nullptr && Child(BodyOf(Document), TEXT("Root"))->GetStringField(TEXT("Input")) == Cue->FirstNode->GetName());
	// Every node: class, inputs in slot order = the editor's links.
	for (const auto& Entry : NodesOf(Document)->Values)
	{
		USoundCueGraphNode* Node = E.Sound(FString(*Entry.Key));
		if (!TestNotNull(*FString::Printf(TEXT("%s is a node"), *Entry.Key), Node))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Row = Entry.Value->AsObject();
		TestEqual(*FString::Printf(TEXT("%s class"), *Entry.Key), Row->GetStringField(TEXT("Class")), SC::ClassLabel(*Node->SoundNode->GetClass()));
		TArray<UEdGraphPin*> Pins;
		Node->GetInputPins(Pins);
		const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
		Row->TryGetArrayField(TEXT("Inputs"), Inputs);
		TestEqual(*FString::Printf(TEXT("%s input count"), *Entry.Key), Inputs != nullptr ? Inputs->Num() : 0, Pins.Num());
		for (int32 Slot = 0; Inputs != nullptr && Slot < FMath::Min(Inputs->Num(), Pins.Num()); ++Slot)
		{
			const FString Linked = PinLink(Pins[Slot]);
			const FString Written = (*Inputs)[Slot]->IsNull() ? FString(TEXT("-")) : (*Inputs)[Slot]->AsString();
			TestEqual(*FString::Printf(TEXT("%s input %d"), *Entry.Key, Slot), Written, Linked);
		}
	}
	// Inspect: graph.query's pins (NodeFacts.InputPins) are the editor's, status, waves, derived values.
	const FAssetDocumentResult Inspected = Inspect(Target.ObjectPath);
	if (TestTrue(TEXT("inspect"), Inspected.IsSuccess() && Inspected.Payload.IsValid()))
	{
		const TSharedPtr<FJsonObject> Facts = Child(Inspected.Payload, TEXT("NodeFacts"));
		TestEqual(TEXT("NodeFacts per node"), Facts->Values.Num(), Cue->AllNodes.Num());
		for (const auto& Fact : Facts->Values)
		{
			USoundCueGraphNode* Node = E.Sound(FString(*Fact.Key));
			if (Node == nullptr)
			{
				continue;
			}
			TArray<UEdGraphPin*> Pins;
			Node->GetInputPins(Pins);
			TArray<FString> Names;
			for (const UEdGraphPin* Pin : Pins)
			{
				Names.Add(Pin->PinName.ToString());
			}
			TArray<FString> Reported;
			for (const TSharedPtr<FJsonValue>& Name : Fact.Value->AsObject()->GetArrayField(TEXT("InputPins")))
			{
				Reported.Add(Name->AsString());
			}
			TestEqual(*FString::Printf(TEXT("%s pins as the editor shows them"), *Fact.Key), FString::Join(Reported, TEXT("|")), FString::Join(Names, TEXT("|")));
		}
		TestTrue(TEXT("inspect lists waves"), Inspected.Payload->GetArrayField(TEXT("Waves")).Num() >= 8);
		TestTrue(TEXT("inspect has status"), Child(Inspected.Payload, TEXT("Status"))->HasField(TEXT("Status")));
		TestTrue(TEXT("inspect has derived Duration"), Child(Inspected.Payload, TEXT("Derived"))->HasField(TEXT("Duration")));
		bool bOrphanUnreachable = false;
		for (const auto& Fact : Facts->Values)
		{
			if (Fact.Value->AsObject()->GetStringField(TEXT("Class")) == TEXT("DialoguePlayer"))
			{
				bOrphanUnreachable = !Fact.Value->AsObject()->GetBoolField(TEXT("Reachable"));
			}
		}
		TestTrue(TEXT("the orphan is not reachable"), bOrphanUnreachable);
	}
	else
	{
		AddError(DiagnosticsText(Inspected));
	}
	TestFalse(TEXT("reading leaves the cue clean"), Cue->GetOutermost()->IsDirty());
	TestEqual(TEXT("reading changes nothing"), CueForm(*Cue), FormBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSoundCueCorpusTest, "UEAssetDoc.SoundCue.Corpus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSoundCueCorpusTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSoundCueTests;
	const FTemplateContent Template;
	FFixtures Fixtures;
	for (const FCorpusEntry& Entry : Corpus)
	{
		UObject* Source = LoadQuiet(Entry.Path);
		if (Source == nullptr)
		{
			if (Entry.bRequired)
			{
				AddError(FString::Printf(TEXT("%s does not load (plugin not mounted?)"), Entry.Path));
			}
			else
			{
				AddInfo(FString::Printf(TEXT("%s is not mounted (optional)"), Entry.Path));
			}
			continue;
		}
		CorpusCase(*this, Fixtures, Source, FPackageName::ObjectPathToObjectName(FString(Entry.Path)), Entry.Why);
	}
	// Editor-built fixtures.
	{
		const FTarget& AllNodes = Fixtures.Add(TEXT("SC_AllNodes"));
		if (USoundCue* Cue = MakeAllNodes(*this, AllNodes))
		{
			CorpusCase(*this, Fixtures, Cue, TEXT("SC_AllNodes"), TEXT("every engine node class"));
		}
	}
	{
		const FQualityLevels Levels({TEXT("Low"), TEXT("High")});
		const FTarget& Quality = Fixtures.Add(TEXT("SC_Quality"));
		USoundCue* Cue = CreateCue(*this, Quality);
		if (Cue != nullptr && BuildQuality(*this, *Cue))
		{
			Save(*this, Cue, TEXT("SC_Quality"));
			ReleaseUndoHistory();
			CorpusCase(*this, Fixtures, Cue, TEXT("SC_Quality"), TEXT("Quality Level, two project levels"));
		}
	}
	{
		const FTarget& FromFactory = Fixtures.Add(TEXT("SC_FromFactory"));
		if (USoundCue* Cue = CreateCue(*this, FromFactory, {WaveA, WaveB, WaveC}))
		{
			CorpusCase(*this, Fixtures, Cue, TEXT("SC_FromFactory"), TEXT("Create Cue from three waves"));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSoundCueEditorDeleteTest, "UEAssetDoc.SoundCue.EditorDelete",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSoundCueEditorDeleteTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSoundCueTests;
	UAssetEditorSubsystem* Editors = GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("asset editor subsystem"), Editors))
	{
		return false;
	}
	FFixtures Fixtures;
	USoundCue* Base = MakeAllNodes(*this, Fixtures.Add(TEXT("SC_DelBase")));
	const FTarget& EditorTarget = Fixtures.Add(TEXT("SC_DelEditor"));
	const FTarget& ReplicaTarget = Fixtures.Add(TEXT("SC_DelReplica"));
	USoundCue* EditorSide = Duplicate(*this, Base, EditorTarget);
	USoundCue* ReplicaSide = Duplicate(*this, Base, ReplicaTarget);
	if (!TestNotNull(TEXT("duplicates"), EditorSide) || !TestNotNull(TEXT("replica duplicate"), ReplicaSide))
	{
		return false;
	}
	if (!Editors->OpenEditorForAsset(EditorSide))
	{
		AddWarning(TEXT("The Sound Cue editor did not open in this session; the editor's Delete was not compared."));
		return true;
	}
	ON_SCOPE_EXIT
	{
		if (UObject* Open = EditorTarget.Get())
		{
			Editors->CloseAllEditorsForAsset(Open);
		}
	};
	const FEditorCue EditorCue(*EditorSide);
	const FEditorCue ReplicaCue(*ReplicaSide);
	// The shared player (two parents), a Random in the middle of the tree, a comment, then the Output node (refused).
	const USoundCueGraphNode* SharedPlayer = nullptr;
	for (const UEdGraphNode* Node : EditorSide->SoundCueGraph->Nodes)
	{
		const USoundCueGraphNode* Sound = Cast<USoundCueGraphNode>(Node);
		if (Sound != nullptr && Sound->SoundNode != nullptr && Sound->SoundNode->IsA<USoundNodeWavePlayer>()
			&& const_cast<USoundCueGraphNode*>(Sound)->GetOutputPin()->LinkedTo.Num() > 1)
		{
			SharedPlayer = Sound;
		}
	}
	TArray<FString> Keys;
	if (SharedPlayer != nullptr)
	{
		Keys.Add(SC::NodeIdentity(*SharedPlayer));
	}
	for (const UEdGraphNode* Node : EditorSide->SoundCueGraph->Nodes)
	{
		const USoundCueGraphNode* Sound = Cast<USoundCueGraphNode>(Node);
		if (Sound != nullptr && Sound->SoundNode != nullptr && SC::ClassLabel(*Sound->SoundNode->GetClass()) == TEXT("Random"))
		{
			Keys.Add(SC::NodeIdentity(*Node));
		}
		if (Node != nullptr && Node->IsA<UEdGraphNode_Comment>() && Keys.Num() < 3)
		{
			Keys.Add(Node->GetName());
		}
	}
	// SGraphEditor -> SBox ("GraphEditorPanel") -> the graph editor implementation, whose OnKeyDown runs the editor's
	// command bindings (SGraphEditorImpl::OnKeyDown -> FUICommandList::ProcessCommandBindings).
	TSharedPtr<SGraphEditor> GraphEditor = SGraphEditor::FindGraphEditorForGraph(EditorSide->SoundCueGraph);
	FChildren* Box = GraphEditor.IsValid() && GraphEditor->GetChildren()->Num() > 0 ? GraphEditor->GetChildren()->GetChildAt(0)->GetChildren() : nullptr;
	if (Box == nullptr || Box->Num() == 0)
	{
		AddWarning(TEXT("The Sound Cue editor's graph panel was not found; the editor's Delete was not compared."));
		return true;
	}
	const TSharedRef<SWidget> Panel = Box->GetChildAt(0);
	for (const FString& Key : Keys)
	{
		UEdGraphNode* EditorNode = EditorCue.Find(Key);
		if (!TestNotNull(*FString::Printf(TEXT("%s on the editor side"), *Key), EditorNode))
		{
			continue;
		}
		// The editor: select the node, press Delete (FSoundCueEditor::DeleteSelectedNodes through its command list).
		GraphEditor->ClearSelectionSet();
		GraphEditor->SetNodeSelection(EditorNode, true);
		const FReply Reply = Panel->OnKeyDown(FGeometry(), FKeyEvent(EKeys::Delete, FModifierKeysState(), 0, false, 0, 0));
		TestTrue(*FString::Printf(TEXT("%s: the editor's command list handles Delete"), *Key), Reply.IsEventHandled());
		if (!TestNull(*FString::Printf(TEXT("%s: the editor deleted it"), *Key), EditorCue.Find(Key)))
		{
			return false;
		}
		// The replica on the other duplicate.
		FString Error;
		TestTrue(*FString::Printf(TEXT("%s: the replica deletes"), *Key), ReplicaCue.Find(Key) != nullptr && SC::DeleteNodeAsTheEditorDoes(*ReplicaSide, *ReplicaCue.Find(Key), Error));
		const FString EditorForm = CueForm(*EditorSide);
		const FString ReplicaForm = CueForm(*ReplicaSide);
		if (!TestEqual(*FString::Printf(TEXT("%s: graph, runtime tree and AllNodes as the editor leaves them"), *Key), ReplicaForm, EditorForm))
		{
			AddInfo(FirstLineDifference(EditorForm, ReplicaForm));
		}
		TestTrue(*FString::Printf(TEXT("%s: both dirty"), *Key), EditorSide->GetOutermost()->IsDirty() == ReplicaSide->GetOutermost()->IsDirty());
	}
	// The Output node: the editor keeps it (CanDeleteNodes), the replica refuses.
	FString Error;
	TestFalse(TEXT("the replica refuses the Output node"), SC::DeleteNodeAsTheEditorDoes(*ReplicaSide, *ReplicaCue.Root(), Error));
	GraphEditor->ClearSelectionSet();
	GraphEditor->SetNodeSelection(EditorCue.Root(), true);
	Panel->OnKeyDown(FGeometry(), FKeyEvent(EKeys::Delete, FModifierKeysState(), 0, false, 0, 0));
	TestNotNull(TEXT("the editor keeps the Output node"), EditorCue.Root());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSoundCueRenamedNodesTest, "UEAssetDoc.SoundCue.RenamedNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSoundCueRenamedNodesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentSoundCueTests;
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("SC_Keys"));
	const TSharedPtr<FJsonObject> Document = Json(*FString::Printf(TEXT(R"({"SchemaVersion": 1, "Target": "%s", "Class": "/Script/Engine.SoundCue", "Action": "Create",
		"Body": {"Root": {"Input": "Mod"},
			"Nodes": {"Mod": {"Class": "Modulator", "Position": [-200, 0], "Inputs": ["Pick"]},
				"Pick": {"Class": "Random", "Position": [-450, 0], "Inputs": ["Step_1", "Step-2"]},
				"Step_1": {"Class": "WavePlayer", "Position": [-750, -100], "Properties": {"SoundWaveAssetPtr": "%s"}},
				"Step-2": {"Class": "WavePlayer", "Position": [-750, 100], "Properties": {"SoundWaveAssetPtr": "%s"}}},
			"Comments": {"Steps": {"Text": "Steps", "Position": [-800, -200], "Size": [400, 400]}}}})"), *Target.PackageName, WaveA, WaveB));
	if (!ApplyOk(*this, Target, Document, TEXT("create with keys")))
	{
		return false;
	}
	auto Check = [this](USoundCue* Cue, const FString& Stage)
	{
		if (!TestNotNull(*FString::Printf(TEXT("%s: cue"), *Stage), Cue))
		{
			return;
		}
		const FEditorCue E(*Cue);
		for (const TCHAR* Key : {TEXT("Mod"), TEXT("Pick"), TEXT("Step_1"), TEXT("Step-2")})
		{
			USoundCueGraphNode* Node = E.Sound(Key);
			if (TestNotNull(*FString::Printf(TEXT("%s: node %s"), *Stage, Key), Node))
			{
				TestTrue(*FString::Printf(TEXT("%s: %s belongs to the cue"), *Stage, Key), Node->SoundNode->GetOuter() == Cue && Cue->AllNodes.Contains(Node->SoundNode));
			}
		}
		TestNotNull(*FString::Printf(TEXT("%s: comment Steps"), *Stage), E.Find(TEXT("Steps")));
		TestTrue(*FString::Printf(TEXT("%s: FirstNode is Mod"), *Stage), Cue->FirstNode != nullptr && Cue->FirstNode->GetName() == TEXT("Mod"));
		const USoundNodeWavePlayer* Player = E.Sound(TEXT("Step_1")) != nullptr ? Cast<USoundNodeWavePlayer>(E.Sound(TEXT("Step_1"))->SoundNode) : nullptr;
		TestTrue(*FString::Printf(TEXT("%s: the player has its wave"), *Stage), Player != nullptr && Player->GetSoundWave() != nullptr);
		TestTrue(*FString::Printf(TEXT("%s: playable, with a duration"), *Stage), Cue->IsPlayable() && Cue->GetDuration() > 0.0f);
	};
	Check(Target.Cue(), TEXT("created"));
	const FString Created = WithoutTarget(Extract(*this, Target.ObjectPath, TEXT("created")));
	// Reload from disk.
	ReleaseUndoHistory();
	UPackageTools::UnloadPackages({Target.Get()->GetOutermost()});
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	Check(Cast<USoundCue>(LoadQuiet(Target.ObjectPath)), TEXT("reloaded"));
	TestEqual(TEXT("reloaded extract"), WithoutTarget(Extract(*this, Target.ObjectPath, TEXT("reloaded"))), Created);
	// Content Browser duplicate: names kept (graph node GUIDs regenerated).
	const FTarget& Copy = Fixtures.Add(TEXT("SC_KeysCopy"));
	USoundCue* Duplicated = Duplicate(*this, Target.Get(), Copy);
	Check(Duplicated, TEXT("duplicated"));
	TestEqual(TEXT("duplicate extracts the same"), WithoutTarget(Extract(*this, Copy.ObjectPath, TEXT("duplicate"))), Created);
	// A key the editor cannot use as an object name is refused at its path.
	const TSharedPtr<FJsonObject> Bad = ForTarget(Parse(Created), Target, TEXT("Update"));
	NodesOf(Bad)->SetObjectField(TEXT("Bad.Key"), Json(TEXT(R"({"Class": "Delay"})")));
	ExpectRefused(*this, Target, Bad, TEXT("SoundCue.InvalidKey"), TEXT("/Body/Nodes/Bad.Key"), TEXT("invalid key"));
	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
