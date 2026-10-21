// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// MetaSound documents, phase M1 (graph apply), through the production Service as the MCP tools send them
// (design §6.2-6.4):
//
// UEAssetDoc.MetaSound.Fidelity: every corpus asset (pages the project settings lack are given
// settings for the test; Harmonix and template content skipped with a note when absent) is duplicated into /Game and extracted; the extraction
// applied as Create on a new /Game asset extracts equal (output node ids aside: interface outputs get theirs from
// the interface) with the same validation status; Diff is then empty; the unchanged re-apply saves and dirties
// nothing; the new asset extracts the same after a reload. MSP_MidiNoteAdsr uses a node class version that is not
// registered (the editor shows "class not found"): its Create is refused with MetaSound.UnknownNodeClass and leaves
// nothing (Update Node of an existing node: AssetDocumentMetaSoundM2Tests.cpp). Timings are reported per asset.
// UEAssetDoc.MetaSound.FreeEdits: the §6.3 graph edits (configuration, Update Node, presets and
// widgets: AssetDocumentMetaSoundM2Tests.cpp; pages: AssetDocumentMetaSoundM3Tests.cpp) on two duplicates of MSS_PlayRandom_Oneshot and of MS_Opera (MSS_PlayRandom_Loop without the
// template content): one is edited by documents, the other by the editor's calls (document builder,
// FInputNodeTemplate::CreateNode, the Audio Analyzer template as its wire-drag action adds it, ModifyInterfaces as the
// Interfaces panel, the Details edit of OutputFormat, then
// UMetaSoundEditorSubsystem::RegisterGraphWithFrontend); after every edit both extract the same graph and validate
// the same.
// UEAssetDoc.MetaSound.Rollback: a new validation error, an incompatible connection and a Create
// with an unknown node class fail at their document paths; the existing asset extracts as before, its package
// stays clean and its file untouched; the failed Create leaves no object, file, asset registry entry, MetaSound
// asset-manager entry or builder.
// UEAssetDoc.MetaSound.Undo: the AssetDocument undo-history rules around a human edit, an AI apply
// and a failed apply; undo and redo keep the asset whole.
// UEAssetDoc.MetaSound.FromScratch: an AI-written stereo one-shot synth (inputs with widget ranges,
// wave player, envelope, filter, reroute, a patch made first and used as a node, a variable counter with
// GetDelayed / Set, a comment, OneShot On Finished wired) is created without node ids, validates without errors and
// re-applies as a no-op; a preset of it overriding Cutoff validates; typical authoring errors (a preset with a graph
// among them) fail at their document paths and change nothing.

#include "AssetDocumentMetaSoundApplyTestSupport.h"
#include "Utils/AssetDocumentJsonKeys.h"

#include "EdGraphNode_Comment.h"
#include "MetaSound/AssetDocumentMetaSound.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundAudioBuffer.h"
#include "MetasoundFrontendNodeTemplateRegistry.h"
#include "MetasoundFrontendSearchEngine.h"
#include "MetasoundAssetSubsystem.h"
#include "MetasoundSource.h"
#include "Misc/App.h"
#include "NodeTemplates/MetasoundFrontendNodeTemplateAudioAnalyzer.h"
#include "Misc/SecureHash.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ScopedTransaction.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentMetaSoundApplyTests
{
using namespace AssetDocumentMetaSoundTests;

// The free edits of one asset. Keys name the asset's nodes as its extraction does.
struct FFreeEditPlan
{
	const TCHAR* Path;
	const TCHAR* MainOut;          // the output member the filter is inserted before
	const TCHAR* MainSource;       // its source key
	const TCHAR* DeleteKey;        // a node to delete ...
	TArray<TPair<FString, FString>> DeleteConsumers;  // ... and the inputs it fed
	const TCHAR* ReconnectKey;     // an input to re-point to another output pin
	const TCHAR* ReconnectPin;
	const TCHAR* ReconnectSource;
	const TCHAR* ReconnectFrom;    // editor side: the source key and pin (nullptr = its only output)
	const TCHAR* ReconnectFromPin;
	const TCHAR* RetypeInput;      // a Float input changed to Int32 ...
	TArray<TPair<FString, FString>> RetypeConsumers;  // ... whose Float consumers lose their edge
	const TCHAR* FloatSource;      // a Float-output node for the variable's Set
	const TCHAR* MoveA;
	const TCHAR* MoveB;
	bool bStereo;                  // the asset's OutputFormat
	const TCHAR* OnFinishedSource; // the OneShot On Finished source ("" = unconnected)
	const TCHAR* OnFinishedPin;
};

TArray<FFreeEdit> FreeEdits(const FFreeEditPlan& Plan)
{
	TArray<FFreeEdit> Edits;
	const FString Out(Plan.MainOut);
	const FString OutKey = FString(Plan.MainOut).Replace(TEXT("."), TEXT("_")).Replace(TEXT(":"), TEXT("_"));
	Edits.Add({TEXT("insert a State Variable Filter"),
		[Plan, Out, OutKey](const TSharedPtr<FJsonObject>& Doc)
		{
			const TSharedPtr<FJsonObject> Filter = Json(TEXT(R"({"Class": "UE.State Variable Filter.Audio", "Position": [2300, 420]})"));
			Child(Filter, TEXT("Inputs"))->SetStringField(TEXT("In"), Plan.MainSource);
			NodesOf(Doc)->SetObjectField(TEXT("Filter"), Filter);
			SetInputs(Doc, OutKey, Out, TEXT("Filter.Band Pass"));
		},
		[Plan, Out](UObject& Asset, const TSharedPtr<FJsonObject>& Doc)
		{
			FEditorSide Side(Asset);
			const FGuid Filter = Side.AddNode(TEXT("UE.State Variable Filter.Audio"), 2300, 420);
			Side.Connect(FEditorSide::Id(Doc, Plan.MainSource), nullptr, Filter, TEXT("In"));
			Side.Connect(Filter, TEXT("Band Pass"), Side.OutputNode(*Out), *Out);
		}});
	// The wire-drag menu of an audio output: "Add Audio Analyzer Node..." (FMetasoundGraphSchemaAction_NewAudioAnalyzer).
	Edits.Add({TEXT("add an Audio Analyzer on an audio output"),
		[Plan](const TSharedPtr<FJsonObject>& Doc)
		{
			const TSharedPtr<FJsonObject> Analyzer = Json(TEXT(R"({"Class": "UE.Audio Analyzer", "Position": [2300, 700]})"));
			Child(Analyzer, TEXT("Inputs"))->SetStringField(TEXT("Value"), Plan.MainSource);
			NodesOf(Doc)->SetObjectField(TEXT("Scope"), Analyzer);
		},
		[Plan](UObject& Asset, const TSharedPtr<FJsonObject>& Doc)
		{
			using namespace Metasound::Frontend;
			FEditorSide Side(Asset);
			const INodeTemplate* Template = INodeTemplateRegistry::Get().FindTemplate(FAudioAnalyzerNodeTemplate::ClassName);
			if (!ensure(Template != nullptr))
			{
				return;
			}
			FNodeTemplateGenerateInterfaceParams Params;
			Params.InputsToConnect.Add(Metasound::GetMetasoundDataTypeName<Metasound::FAudioBuffer>());
			const FMetasoundFrontendNode* Node = Side.Builder.AddNodeByTemplate(*Template, MoveTemp(Params));
			if (!ensure(Node != nullptr))
			{
				return;
			}
			const FGuid Analyzer = Node->GetID();
			Side.Move(Analyzer, 2300, 700);
			Side.Connect(FEditorSide::Id(Doc, Plan.MainSource), nullptr, Analyzer, TEXT("Value"));
		}});
	Edits.Add({TEXT("delete a node"),
		[Plan](const TSharedPtr<FJsonObject>& Doc)
		{
			NodesOf(Doc)->RemoveField(Plan.DeleteKey);
			for (const TPair<FString, FString>& Consumer : Plan.DeleteConsumers)
			{
				SetInputs(Doc, Consumer.Key, Consumer.Value, FString());
			}
		},
		[Plan](UObject& Asset, const TSharedPtr<FJsonObject>& Doc)
		{
			FEditorSide Side(Asset);
			Side.Builder.RemoveNode(FEditorSide::Id(Doc, Plan.DeleteKey));
			Side.Builder.RemoveUnusedDependencies();
		}});
	Edits.Add({TEXT("reconnect to another output pin"),
		[Plan](const TSharedPtr<FJsonObject>& Doc) { SetInputs(Doc, Plan.ReconnectKey, Plan.ReconnectPin, Plan.ReconnectSource); },
		[Plan](UObject& Asset, const TSharedPtr<FJsonObject>& Doc)
		{
			FEditorSide Side(Asset);
			Side.Connect(FEditorSide::Id(Doc, Plan.ReconnectFrom), Plan.ReconnectFromPin, FEditorSide::Id(Doc, Plan.ReconnectKey), Plan.ReconnectPin);
		}});
	Edits.Add({TEXT("add an input with two input nodes"),
		[](const TSharedPtr<FJsonObject>& Doc)
		{
			Child(BodyOf(Doc), TEXT("Inputs"))->SetObjectField(TEXT("Gain"), Json(TEXT(R"({"Type": "Float", "Default": 0.5, "SortOrder": 9})")));
			NodesOf(Doc)->SetObjectField(TEXT("GainA"), Json(TEXT(R"({"Class": "Input", "Member": "Gain", "Position": [-400, 1400]})")));
			NodesOf(Doc)->SetObjectField(TEXT("GainB"), Json(TEXT(R"({"Class": "Input", "Member": "Gain", "Position": [-400, 1500]})")));
		},
		[](UObject& Asset, const TSharedPtr<FJsonObject>&)
		{
			FEditorSide Side(Asset);
			// The Members panel's add (FGraphBuilder::CreateUniqueClassInput) and two drags of the member.
			FMetasoundFrontendClassInput Input;
			Input.Name = TEXT("Gain");
			Input.TypeName = TEXT("Float");
			Input.VertexID = FGuid::NewGuid();
			Input.NodeID = FGuid::NewGuid();
			Input.AccessType = EMetasoundFrontendVertexAccessType::Reference;
			Input.InitDefault();
			Side.Builder.AddGraphInput(MoveTemp(Input));
			FMetasoundFrontendLiteral Half;
			Half.Set(0.5f);
			Side.Builder.SetGraphInputDefault(TEXT("Gain"), Half);
			Side.Builder.SetGraphInputSortOrderIndex(TEXT("Gain"), 9);
			const FMetasoundFrontendNode* A = Metasound::Frontend::FInputNodeTemplate::CreateNode(Side.Builder, TEXT("Gain"));
			const FGuid AId = A != nullptr ? A->GetID() : FGuid();
			Side.Move(AId, -400, 1400);
			const FMetasoundFrontendNode* B = Metasound::Frontend::FInputNodeTemplate::CreateNode(Side.Builder, TEXT("Gain"));
			Side.Move(B != nullptr ? B->GetID() : FGuid(), -400, 1500);
		}});
	Edits.Add({TEXT("rename the input"),
		[](const TSharedPtr<FJsonObject>& Doc)
		{
			const TSharedPtr<FJsonObject> Inputs = Child(BodyOf(Doc), TEXT("Inputs"));
			const TSharedPtr<FJsonObject> Gain = Inputs->GetObjectField(TEXT("Gain"));
			Gain->SetStringField(TEXT("RenamedFrom"), TEXT("Gain"));
			Inputs->RemoveField(TEXT("Gain"));
			Inputs->SetObjectField(TEXT("Level"), Gain);
			for (const auto& Node : NodesOf(Doc)->Values)
			{
				FString Member;
				if (Node.Value->AsObject()->TryGetStringField(TEXT("Member"), Member) && Member == TEXT("Gain"))
				{
					Node.Value->AsObject()->SetStringField(TEXT("Member"), TEXT("Level"));
				}
			}
		},
		[](UObject& Asset, const TSharedPtr<FJsonObject>&)
		{
			FEditorSide Side(Asset);
			Side.Builder.SetGraphInputName(TEXT("Gain"), TEXT("Level"));
		}});
	Edits.Add({TEXT("remove the input"),
		[](const TSharedPtr<FJsonObject>& Doc)
		{
			Child(BodyOf(Doc), TEXT("Inputs"))->RemoveField(TEXT("Level"));
			TArray<FString> Gone;
			for (const auto& Node : NodesOf(Doc)->Values)
			{
				FString Member;
				if (Node.Value->AsObject()->TryGetStringField(TEXT("Member"), Member) && Member == TEXT("Level"))
				{
					Gone.Add(FString(*Node.Key));
				}
			}
			for (const FString& Key : Gone)
			{
				NodesOf(Doc)->RemoveField(Key);
			}
		},
		[](UObject& Asset, const TSharedPtr<FJsonObject>&)
		{
			FEditorSide Side(Asset);
			const FMetasoundFrontendClassInput* Input = Side.Builder.FindGraphInput(TEXT("Level"));
			if (Input != nullptr)
			{
				Side.Builder.ClearMemberMetadata(Input->NodeID);
			}
			Side.Builder.RemoveGraphInput(TEXT("Level"));
		}});
	Edits.Add({TEXT("change an input's type"),
		[Plan](const TSharedPtr<FJsonObject>& Doc)
		{
			const TSharedPtr<FJsonObject> Input = Child(Child(BodyOf(Doc), TEXT("Inputs")), Plan.RetypeInput);
			Input->SetStringField(TEXT("Type"), TEXT("Int32"));
			Input->SetNumberField(TEXT("Default"), 0);
			Input->RemoveField(TEXT("Widget"));
			for (const TPair<FString, FString>& Consumer : Plan.RetypeConsumers)
			{
				SetInputs(Doc, Consumer.Key, Consumer.Value, FString());
			}
		},
		[Plan](UObject& Asset, const TSharedPtr<FJsonObject>&)
		{
			FEditorSide Side(Asset);
			if (const FMetasoundFrontendClassInput* Input = Side.Builder.FindGraphInput(Plan.RetypeInput))
			{
				Side.Builder.ClearMemberMetadata(Input->NodeID);
			}
			Side.Builder.SetGraphInputDataType(Plan.RetypeInput, TEXT("Int32"));
		}});
	Edits.Add({TEXT("add a variable with Get, Set and Get Delayed"),
		[Plan](const TSharedPtr<FJsonObject>& Doc)
		{
			Child(PageOf(Doc), TEXT("Variables"))->SetObjectField(TEXT("Count"), Json(TEXT(R"({"Type": "Float", "Default": 1.0})")));
			NodesOf(Doc)->SetObjectField(TEXT("CountGet"), Json(TEXT(R"({"Class": "Get", "Member": "Count", "Position": [600, 1700]})")));
			const TSharedPtr<FJsonObject> Set = Json(TEXT(R"({"Class": "Set", "Member": "Count", "Position": [900, 1700], "Defaults": {"Value": 1.0}})"));
			Child(Set, TEXT("Inputs"))->SetStringField(TEXT("Value"), Plan.FloatSource);
			NodesOf(Doc)->SetObjectField(TEXT("CountSet"), Set);
			NodesOf(Doc)->SetObjectField(TEXT("CountDelayed"), Json(TEXT(R"({"Class": "GetDelayed", "Member": "Count", "Position": [300, 1700]})")));
		},
		[Plan](UObject& Asset, const TSharedPtr<FJsonObject>& Doc)
		{
			FEditorSide Side(Asset);
			FMetasoundFrontendLiteral One;
			One.Set(1.0f);
			Side.Builder.AddGraphVariable(TEXT("Count"), TEXT("Float"), &One);
			const FMetasoundFrontendNode* Get = Side.Builder.AddGraphVariableAccessorNode(TEXT("Count"));
			const FGuid GetId = Get != nullptr ? Get->GetID() : FGuid();
			Side.Move(GetId, 600, 1700);
			const FMetasoundFrontendNode* Set = Side.Builder.AddGraphVariableMutatorNode(TEXT("Count"));
			const FGuid SetId = Set != nullptr ? Set->GetID() : FGuid();
			Side.Move(SetId, 900, 1700);
			const FMetasoundFrontendNode* Delayed = Side.Builder.AddGraphVariableDeferredAccessorNode(TEXT("Count"));
			Side.Move(Delayed != nullptr ? Delayed->GetID() : FGuid(), 300, 1700);
			Side.Connect(FEditorSide::Id(Doc, Plan.FloatSource), nullptr, SetId, TEXT("Value"));
		}});
	Edits.Add({TEXT("add a comment"),
		[](const TSharedPtr<FJsonObject>& Doc)
		{
			Child(PageOf(Doc), TEXT("Comments"))->SetObjectField(TEXT("Note"), Json(TEXT(R"({"Text": "Volume", "Position": [1100, 550], "Size": [600, 300]})")));
		},
		[](UObject& Asset, const TSharedPtr<FJsonObject>&)
		{
			FEditorSide Side(Asset);
			// FMetasoundGraphSchemaAction_NewComment: a comment box with UEdGraphNode_Comment's defaults.
			FMetaSoundFrontendGraphComment& Comment = Side.Builder.FindOrAddGraphComment(KeyedId(TEXT("AssetDocument.MetaSound.Comment:Note")));
			const UEdGraphNode_Comment* Defaults = GetDefault<UEdGraphNode_Comment>();
			Comment.Comment = TEXT("Volume");
			Comment.Color = Defaults->CommentColor;
			Comment.FontSize = Defaults->FontSize;
			Comment.bColorBubble = Defaults->bColorCommentBubble ? 1 : 0;
			Comment.Position = FIntVector2(1100, 550);
			Comment.Size = FIntVector2(600, 300);
		}});
	Edits.Add({TEXT("edit the comment"),
		[](const TSharedPtr<FJsonObject>& Doc)
		{
			// Extraction names comments Comment, Comment_2, ...: the one added above is found by its text.
			TSharedPtr<FJsonObject> Note;
			for (const auto& Entry : Child(PageOf(Doc), TEXT("Comments"))->Values)
			{
				FString Text;
				if (Entry.Value->AsObject()->TryGetStringField(TEXT("Text"), Text) && Text == TEXT("Volume"))
				{
					Note = Entry.Value->AsObject();
				}
			}
			if (!Note.IsValid())
			{
				return;
			}
			Note->SetStringField(TEXT("Text"), TEXT("Volume chain"));
			Note->SetObjectField(TEXT("Color"), Json(TEXT(R"({"R": 0.2, "G": 0.6, "B": 1.0, "A": 1.0})")));
		},
		[](UObject& Asset, const TSharedPtr<FJsonObject>&)
		{
			FEditorSide Side(Asset);
			FMetaSoundFrontendGraphComment& Comment = Side.Builder.FindOrAddGraphComment(KeyedId(TEXT("AssetDocument.MetaSound.Comment:Note")));
			Comment.Comment = TEXT("Volume chain");
			Comment.Color = FLinearColor(0.2f, 0.6f, 1.0f, 1.0f);
		}});
	Edits.Add({TEXT("move nodes"),
		[Plan](const TSharedPtr<FJsonObject>& Doc)
		{
			NodeOf(Doc, Plan.MoveA)->SetArrayField(TEXT("Position"), AssetDocumentMetaSoundApplyTests::Point(1800, -100));
			NodeOf(Doc, Plan.MoveB)->SetArrayField(TEXT("Position"), AssetDocumentMetaSoundApplyTests::Point(700, -50));
		},
		[Plan](UObject& Asset, const TSharedPtr<FJsonObject>& Doc)
		{
			FEditorSide Side(Asset);
			Side.Move(FEditorSide::Id(Doc, Plan.MoveA), 1800, -100);
			Side.Move(FEditorSide::Id(Doc, Plan.MoveB), 700, -50);
		}});
	// OutputFormat: the other channel layout, its output nodes fed by the filter.
	const TArray<FString> NewOuts = Plan.bStereo ? TArray<FString>{TEXT("UE.OutputFormat.Mono.Audio:0")}
		: TArray<FString>{TEXT("UE.OutputFormat.Stereo.Audio:0"), TEXT("UE.OutputFormat.Stereo.Audio:1")};
	Edits.Add({Plan.bStereo ? TEXT("OutputFormat Stereo -> Mono") : TEXT("OutputFormat Mono -> Stereo"),
		[Plan, NewOuts](const TSharedPtr<FJsonObject>& Doc)
		{
			Doc->GetObjectField(TEXT("Properties"))->SetStringField(TEXT("OutputFormat"), Plan.bStereo ? TEXT("Mono") : TEXT("Stereo"));
			TArray<FString> Old;
			for (const auto& Node : NodesOf(Doc)->Values)
			{
				if (FString(*Node.Key).StartsWith(TEXT("UE_OutputFormat_")))
				{
					Old.Add(FString(*Node.Key));
				}
			}
			for (const FString& Key : Old)
			{
				NodesOf(Doc)->RemoveField(Key);
			}
			for (int32 Index = 0; Index < NewOuts.Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
				Node->SetStringField(TEXT("Class"), TEXT("Output"));
				Node->SetStringField(TEXT("Member"), NewOuts[Index]);
				Node->SetArrayField(TEXT("Position"), AssetDocumentMetaSoundApplyTests::Point(2600, 256 + 100 * Index));
				Child(Node, TEXT("Inputs"))->SetStringField(NewOuts[Index], KeyAt(Doc, TEXT("UE.State Variable Filter.Audio"), 2300, 420) + TEXT(".Band Pass"));
				NodesOf(Doc)->SetObjectField(NewOuts[Index].Replace(TEXT("."), TEXT("_")).Replace(TEXT(":"), TEXT("_")), Node);
			}
		},
		[Plan, NewOuts](UObject& Asset, const TSharedPtr<FJsonObject>& Doc)
		{
			SetOutputFormat(Asset, Plan.bStereo ? EMetaSoundOutputAudioFormat::Mono : EMetaSoundOutputAudioFormat::Stereo);
			FEditorSide Side(Asset);
			const FGuid Filter = FEditorSide::Id(Doc, KeyAt(Doc, TEXT("UE.State Variable Filter.Audio"), 2300, 420));
			for (int32 Index = 0; Index < NewOuts.Num(); ++Index)
			{
				const FGuid Node = Side.OutputNode(*NewOuts[Index]);
				Side.Connect(Filter, TEXT("Band Pass"), Node, *NewOuts[Index]);
				Side.Move(Node, 2600, 256 + 100 * Index);
			}
		}});
	Edits.Add({TEXT("remove UE.Source.OneShot"),
		[](const TSharedPtr<FJsonObject>& Doc)
		{
			BodyOf(Doc)->SetArrayField(TEXT("Interfaces"), {});
			NodesOf(Doc)->RemoveField(TEXT("UE_Source_OneShot_OnFinished"));
		},
		[](UObject& Asset, const TSharedPtr<FJsonObject>&)
		{
			FEditorSide Side(Asset);
			Side.Interface(TEXT("UE.Source.OneShot"), false);
		}});
	Edits.Add({TEXT("add UE.Source.OneShot again"),
		[Plan](const TSharedPtr<FJsonObject>& Doc)
		{
			BodyOf(Doc)->SetArrayField(TEXT("Interfaces"), {MakeShared<FJsonValueString>(TEXT("UE.Source.OneShot"))});
			const TSharedPtr<FJsonObject> Done = Json(TEXT(R"({"Class": "Output", "Member": "UE.Source.OneShot.OnFinished", "Position": [2600, 30]})"));
			if (FCString::Strlen(Plan.OnFinishedSource) > 0)
			{
				Child(Done, TEXT("Inputs"))->SetStringField(TEXT("UE.Source.OneShot.OnFinished"), FString(Plan.OnFinishedSource) + TEXT(".") + Plan.OnFinishedPin);
			}
			NodesOf(Doc)->SetObjectField(TEXT("UE_Source_OneShot_OnFinished"), Done);
		},
		[Plan](UObject& Asset, const TSharedPtr<FJsonObject>& Doc)
		{
			FEditorSide Side(Asset);
			Side.Interface(TEXT("UE.Source.OneShot"), true);
			const FGuid Done = Side.OutputNode(TEXT("UE.Source.OneShot.OnFinished"));
			Side.Move(Done, 2600, 30);
			if (FCString::Strlen(Plan.OnFinishedSource) > 0)
			{
				Side.Connect(FEditorSide::Id(Doc, Plan.OnFinishedSource), Plan.OnFinishedPin, Done, TEXT("UE.Source.OneShot.OnFinished"));
			}
		}});
	return Edits;
}
} // namespace AssetDocumentMetaSoundApplyTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundFidelityTest, "UEAssetDoc.MetaSound.Fidelity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundFidelityTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundApplyTests;
	const FDemoTemplateContent Demo;
	int32 Tried = 0;
	int32 Passed = 0;
	bool bHarmonixSkipped = false;
	TArray<FString> Report;
	for (const FCorpusEntry& Entry : Corpus)
	{
		const FString Path = Entry.Path;
		const FString Name(FPackageName::ObjectPathToObjectName(Path));
		UObject* Source = LoadQuiet(Path);
		if (Source == nullptr)
		{
			bHarmonixSkipped |= Path.StartsWith(TEXT("/Harmonix/"));
			AddInfo(FString::Printf(TEXT("SKIP %s: not available (%s)"), *Name,
				Path.StartsWith(TEXT("/Harmonix/")) ? TEXT("the Harmonix plugin is not enabled in this project") : TEXT("content missing")));
			continue;
		}
		FFixtures Fixtures;
		const FTarget& Original = Fixtures.Add(TEXT("MS_Fid"));
		const FTarget& Created = Fixtures.Add(TEXT("MS_FidNew"));
		if (Duplicate(*this, Source, Original) == nullptr)
		{
			AddError(FString::Printf(TEXT("%s: duplicate failed"), *Name));
			continue;
		}
		double Start = FPlatformTime::Seconds();
		FAssetDocumentResult OriginalResult = ExtractResult(Original.ObjectPath);
		double ExtractMs = Milliseconds(Start);
		if (!OriginalResult.IsSuccess() || !OriginalResult.Payload.IsValid())
		{
			AddError(FString::Printf(TEXT("%s: extract failed: %s"), *Name, *DiagnosticsText(OriginalResult)));
			continue;
		}
		// Pages the project settings do not define (named by their ids) are given settings, as the project that made
		// the asset has; the asset is read again with them.
		TOptional<FScopedProjectPages> ProjectPages;
		TArray<TPair<FName, FGuid>> Unconfigured;
		for (const FString& Page : PagesOf(OriginalResult.Payload))
		{
			FGuid PageID;
			bool bConfigured = true;
			if (UE::AssetDocument::Graph::Audio::MetaSound::FindPageID(Page, PageID, &bConfigured) && !bConfigured)
			{
				Unconfigured.Add({FName(*FString::Printf(TEXT("CorpusPage%d"), Unconfigured.Num() + 1)), PageID});
			}
		}
		if (!Unconfigured.IsEmpty())
		{
			ProjectPages.Emplace(Unconfigured);
			Start = FPlatformTime::Seconds();
			OriginalResult = ExtractResult(Original.ObjectPath);
			ExtractMs = Milliseconds(Start);
		}
		const TSharedPtr<FJsonObject> Extracted = OriginalResult.IsSuccess() ? OriginalResult.Payload : nullptr;
		if (!Extracted.IsValid())
		{
			AddError(FString::Printf(TEXT("%s: extract failed: %s"), *Name, *DiagnosticsText(OriginalResult)));
			continue;
		}
		++Tried;
		const FRoundTrip Trip = RoundTrip(*this, Extracted, StatusOf(OriginalResult), Created, Name);
		if (!Trip.Applied.IsSuccess())
		{
			const FAssetDocumentResult& Applied = Trip.Applied;
			const bool bExpected = Name == TEXT("MSP_MidiNoteAdsr") && HasCode(Applied, TEXT("MetaSound.UnknownNodeClass"));
			// FusionPatch literals cannot be registered without an audio device (the editor would crash): refused.
			const bool bNeedsAudio = !FApp::CanEverRenderAudio() && HasCode(Applied, TEXT("MetaSound.AudioDeviceRequired"));
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			const bool bLeftNothing = Created.Get() == nullptr && !IFileManager::Get().FileExists(*Created.PackageFile) && !AssetManagerKnows(Created.PackageName);
			if ((bExpected || bNeedsAudio) && bLeftNothing)
			{
				++Passed;
				Report.Add(FString::Printf(TEXT("PASS %-52s refused as expected (%s) in %.0f ms, nothing left"), *Name, *DiagnosticsText(Applied).Left(200), Trip.CreateMs));
			}
			else
			{
				AddError(FString::Printf(TEXT("%s: Create failed: %s%s"), *Name, *DiagnosticsText(Applied), bLeftNothing ? TEXT("") : TEXT(" (and left residue)")));
				Report.Add(FString::Printf(TEXT("FAIL %-52s Create failed"), *Name));
			}
			continue;
		}
		const TArray<FString>& Problems = Trip.Problems;
		const FString Line = FString::Printf(TEXT("%-52s %s | extract %.0f ms, %s | %s"), *Name, *GraphSummary(Extracted), ExtractMs, *Trip.Timings(),
			*Trip.Status.Left(120));
		if (Problems.IsEmpty())
		{
			++Passed;
			Report.Add(TEXT("PASS ") + Line);
		}
		else
		{
			Report.Add(TEXT("FAIL ") + Line);
			for (const FString& Problem : Problems)
			{
				AddError(FString::Printf(TEXT("%s: %s"), *Name, *Problem));
			}
		}
	}
	AddInfo(FString::Printf(TEXT("MetaSound fidelity: %d / %d pass (%d corpus entries)%s"), Passed, Tried, UE_ARRAY_COUNT(Corpus),
		bHarmonixSkipped ? TEXT("; Harmonix entries skipped: enable the Harmonix plugin in the project to run them") : TEXT("")));
	for (const FString& Line : Report)
	{
		AddInfo(Line);
	}
	TestTrue(TEXT("at least the Metasound plugin's sources are available"), Tried >= 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundFreeEditsTest, "UEAssetDoc.MetaSound.FreeEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundFreeEditsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundApplyTests;
	const FDemoTemplateContent Demo;
	TArray<FFreeEditPlan> Plans;
	Plans.Add({OneShot, TEXT("UE.OutputFormat.Stereo.Audio:0"), TEXT("Multiply_2"), TEXT("Clamp"),
		{{TEXT("Decibels_to_Linear_Gain"), TEXT("Decibels")}},
		TEXT("Multiply_3"), TEXT("PrimaryOperand"), TEXT("Wave_Player.Out Left"), TEXT("Wave_Player"), TEXT("Out Left"),
		TEXT("PitchBase"), {{TEXT("Add_2"), TEXT("PrimaryOperand")}}, TEXT("Add"), TEXT("Wave_Player"), TEXT("Random_Get"), true,
		TEXT("Wave_Player"), TEXT("On Finished")});
	if (LoadQuiet(Opera) != nullptr)
	{
		Plans.Add({Opera, TEXT("UE.OutputFormat.Mono.Audio:0"), TEXT("Audio_Mixer__Mono__2_"), TEXT("LFO"),
			{{TEXT("Add"), TEXT("PrimaryOperand")}},
			TEXT("Audio_Mixer__Mono__2_"), TEXT("In 0"), TEXT("Audio_Mixer__Mono__3_"), TEXT("Audio_Mixer__Mono__3_"), nullptr,
			TEXT("MidiIn"), {{TEXT("MIDI_To_Frequency"), TEXT("MIDI In")}}, TEXT("Add"), TEXT("Saw"), TEXT("Sine"), false, TEXT(""), TEXT("")});
	}
	else
	{
		AddInfo(TEXT("MS_Opera (TP_UEIntro_BP template) is not available; its free edits are skipped"));
	}
	for (const FFreeEditPlan& Plan : Plans)
	{
		const FString Name(FPackageName::ObjectPathToObjectName(FString(Plan.Path)));
		UObject* Source = LoadQuiet(Plan.Path);
		if (!TestNotNull(*FString::Printf(TEXT("%s loads"), *Name), Source))
		{
			continue;
		}
		FFixtures Fixtures;
		const FTarget& DocumentSide = Fixtures.Add(TEXT("MS_FreeDoc"));
		const FTarget& EditorSide = Fixtures.Add(TEXT("MS_FreeEd"));
		if (Duplicate(*this, Source, DocumentSide) == nullptr || Duplicate(*this, Source, EditorSide) == nullptr)
		{
			AddError(FString::Printf(TEXT("%s: duplicates failed"), *Name));
			continue;
		}
		// Both sides start from the same applied state (the open-time injection made, both registered).
		{
			const TSharedPtr<FJsonObject> Start = Extract(*this, DocumentSide.ObjectPath, Name + TEXT(" start"));
			if (!Start.IsValid())
			{
				continue;
			}
			Child(BodyOf(Start), TEXT("Metadata"))->SetStringField(TEXT("Description"), TEXT("free edits"));
			if (!ApplyOk(*this, DocumentSide, ForTarget(Start, DocumentSide, TEXT("Update")), Name + TEXT(" start (document side)"))
				|| !ApplyOk(*this, EditorSide, ForTarget(Start, EditorSide, TEXT("Update")), Name + TEXT(" start (editor side)")))
			{
				continue;
			}
		}
		TMap<FString, FString> IdMap;
		for (const FFreeEdit& Edit : FreeEdits(Plan))
		{
			const FString Stage = Name + TEXT(": ") + Edit.Name;
			const TSharedPtr<FJsonObject> Before = Extract(*this, DocumentSide.ObjectPath, Stage + TEXT(" (document side, before)"));
			const TSharedPtr<FJsonObject> EditorBefore = Extract(*this, EditorSide.ObjectPath, Stage + TEXT(" (editor side, before)"));
			if (!Before.IsValid() || !EditorBefore.IsValid())
			{
				break;
			}
			const TSharedPtr<FJsonObject> Edited = ForTarget(Before, DocumentSide, TEXT("Update"));
			Edit.Document(Edited);
			const double Start = FPlatformTime::Seconds();
			if (!ApplyOk(*this, DocumentSide, Edited, Stage))
			{
				break;
			}
			const double ApplyMs = Milliseconds(Start);
			Edit.Editor(*EditorSide.Get(), EditorBefore);
			const FAssetDocumentResult DocumentResult = ExtractResult(DocumentSide.ObjectPath);
			const FAssetDocumentResult EditorResult = ExtractResult(EditorSide.ObjectPath);
			if (!TestTrue(*FString::Printf(TEXT("%s: both sides extract"), *Stage), DocumentResult.IsSuccess() && EditorResult.IsSuccess()))
			{
				break;
			}
			MapIds(DocumentResult.Payload, EditorResult.Payload, IdMap);
			const FString Expected = GraphForm(EditorResult.Payload, IdMap);
			const FString Actual = GraphForm(DocumentResult.Payload, {});
			if (!TestEqual(*FString::Printf(TEXT("%s: document and editor give the same graph"), *Stage), Actual, Expected))
			{
				AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *FirstDifference(Actual, Expected)));
				AddInfo(FString::Printf(TEXT("%s: document side %s"), *Stage, *Serialize(DocumentResult.Payload)));
				AddInfo(FString::Printf(TEXT("%s: editor side %s"), *Stage, *Serialize(EditorResult.Payload)));
			}
			TestEqual(*FString::Printf(TEXT("%s: same validation"), *Stage), StatusOf(DocumentResult), StatusOf(EditorResult));
			AddInfo(FString::Printf(TEXT("%s: applied in %.0f ms"), *Stage, ApplyMs));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundRollbackTest, "UEAssetDoc.MetaSound.Rollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundRollbackTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundApplyTests;
	using namespace UE::AssetDocument::Graph::Audio;
	UObject* Source = LoadQuiet(OneShot);
	if (!TestNotNull(TEXT("MSS_PlayRandom_Oneshot loads"), Source))
	{
		return false;
	}
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("MSS_Rollback"));
	if (!TestNotNull(TEXT("duplicate"), Duplicate(*this, Source, Target)))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Baseline = Extract(*this, Target.ObjectPath, TEXT("baseline"));
	if (!Baseline.IsValid())
	{
		return false;
	}
	// A new validation error (the editor's own errors are not reachable from a document: the test hook marks the
	// new node invalid).
	{
		const TSharedPtr<FJsonObject> Document = ForTarget(Baseline, Target, TEXT("Update"));
		NodesOf(Document)->SetObjectField(TEXT("Extra"), Json(TEXT(R"({"Class": "UE.Sine.Audio", "Position": [0, 1200]})")));
		MetaSound::SetValidationErrorNodeForTest(KeyedId(TEXT("AssetDocument.MetaSound.Node:Extra")));
		ExpectRefused(*this, Target, Document, TEXT("MetaSound.ValidationFailed"), TEXT("/Body/Graphs/Default/Nodes/Extra"), TEXT("new validation error"));
		MetaSound::SetValidationErrorNodeForTest(FGuid());
	}
	// An incompatible connection (Int32 into a Trigger input).
	{
		const TSharedPtr<FJsonObject> Document = ForTarget(Baseline, Target, TEXT("Update"));
		Child(BodyOf(Document), TEXT("Metadata"))->SetStringField(TEXT("Description"), TEXT("changed before the failure"));
		SetInputs(Document, TEXT("Wave_Player"), TEXT("Play"), TEXT("Num"));
		ExpectRefused(*this, Target, Document, TEXT("MetaSound.IncompatibleConnection"), TEXT("/Body/Graphs/Default/Nodes/Wave_Player/Inputs/Play"),
			TEXT("incompatible connection"));
	}
	// A loop.
	{
		const TSharedPtr<FJsonObject> Document = ForTarget(Baseline, Target, TEXT("Update"));
		SetInputs(Document, TEXT("Add"), TEXT("PrimaryOperand"), TEXT("Decibels_to_Linear_Gain"));
		ExpectRefused(*this, Target, Document, TEXT("MetaSound.ConnectionCycle"), TEXT("/Body/Graphs/Default/Nodes/Add/Inputs/PrimaryOperand"),
			TEXT("connection loop"));
	}
	// A Create that fails leaves nothing behind.
	{
		const FTarget& Failed = Fixtures.Add(TEXT("MSS_FailedCreate"));
		const TSharedPtr<FJsonObject> Document = ForTarget(Baseline, Failed, TEXT("Create"));
		NodesOf(Document)->SetObjectField(TEXT("Bad"), Json(TEXT(R"({"Class": "UE.NoSuchNode.Audio", "Position": [0, 0]})")));
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		const int32 BuildersBefore = BuilderObjectCount();
		const FAssetDocumentResult Result = Apply(Failed, Document);
		TestFalse(TEXT("failed Create: refused"), Result.IsSuccess());
		if (!TestTrue(TEXT("failed Create: MetaSound.UnknownNodeClass"), HasCode(Result, TEXT("MetaSound.UnknownNodeClass"))))
		{
			AddInfo(DiagnosticsText(Result));
		}
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestNull(TEXT("failed Create: no object"), Failed.Get());
		TestFalse(TEXT("failed Create: no package file"), IFileManager::Get().FileExists(*Failed.PackageFile));
		TArray<FAssetData> Assets;
		FAssetRegistryModule::GetRegistry().GetAssetsByPackageName(FName(*Failed.PackageName), Assets);
		TestEqual(TEXT("failed Create: no asset registry entry"), Assets.Num(), 0);
		TestFalse(TEXT("failed Create: no MetaSound asset manager entry"), AssetManagerKnows(Failed.PackageName));
		TestEqual(TEXT("failed Create: no builder left"), BuilderObjectCount(), BuildersBefore);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundUndoTest, "UEAssetDoc.MetaSound.Undo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundUndoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundApplyTests;
	UObject* Source = LoadQuiet(OneShot);
	if (GEditor == nullptr || GEditor->Trans == nullptr || !TestNotNull(TEXT("MSS_PlayRandom_Oneshot loads"), Source))
	{
		return false;
	}
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("MSS_Undo"));
	if (!TestNotNull(TEXT("duplicate"), Duplicate(*this, Source, Target)))
	{
		return false;
	}
	ReleaseUndoHistory();
	auto HumanMove = [&Target](double X)
	{
		const TSharedPtr<FJsonObject> Now = ExtractResult(Target.ObjectPath).Payload;
		FEditorSide Side(*Target.Get());
		Side.Move(FEditorSide::Id(Now, TEXT("Wave_Player")), X, 0);
	};
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

	// 1. The human moves a node and saves; the AI edits: history referencing the MetaSound is cleared.
	HumanMove(1900);
	TestTrue(TEXT("human save"), UEditorLoadingAndSavingUtils::SavePackages({Target.Get()->GetOutermost()}, false));
	TestTrue(TEXT("the human edit is in the undo history"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const TSharedPtr<FJsonObject> HumanState = Extract(*this, Target.ObjectPath, TEXT("after human edit"));
	const TSharedPtr<FJsonObject> AiEdit = ForTarget(HumanState, Target, TEXT("Update"));
	NodesOf(AiEdit)->SetObjectField(TEXT("Extra"), Json(TEXT(R"({"Class": "UE.Sine.Audio", "Position": [0, 1200]})")));
	if (!ApplyOk(*this, Target, AiEdit, TEXT("AI edit")))
	{
		return false;
	}
	TestFalse(TEXT("after AI edit: no undo history references the MetaSound"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const FString AfterEdit = Serialize(Extract(*this, Target.ObjectPath, TEXT("after AI edit")));
	TestTrue(TEXT("human move kept"), AfterEdit.Contains(TEXT("[1900,0]")));
	UndoRedoAll(TEXT("after AI edit"));
	TestEqual(TEXT("after AI edit + undo/redo"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after undo/redo"))), AfterEdit);

	// 2. The human edits again; an AI apply fails and rolls back to the human state; undo stays safe.
	ReleaseUndoHistory();
	HumanMove(2000);
	const FString SecondHuman = Serialize(Extract(*this, Target.ObjectPath, TEXT("after second human edit")));
	const TSharedPtr<FJsonObject> Failing = ForTarget(Parse(SecondHuman), Target, TEXT("Update"));
	SetInputs(Failing, TEXT("Wave_Player"), TEXT("Play"), TEXT("Num"));
	const FAssetDocumentResult Failed = Apply(Target, Failing);
	TestFalse(TEXT("failing apply fails"), Failed.IsSuccess());
	TestEqual(TEXT("after rollback: the human state"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after rollback"))), SecondHuman);
	const bool bHistoryKept = UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName);
	AddInfo(FString::Printf(TEXT("after the failed apply the undo history %s"), bHistoryKept ? TEXT("keeps the human edit")
		: TEXT("was cleared (the failed edit recorded a transaction)")));
	UndoRedoAll(TEXT("after rollback"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundFromScratchTest, "UEAssetDoc.MetaSound.FromScratch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundFromScratchTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundApplyTests;
	FFixtures Fixtures;
	const FTarget& Patch = Fixtures.Add(TEXT("MSP_Gain"));
	const FTarget& Synth = Fixtures.Add(TEXT("MSS_Synth"));
	// A patch, then a source that uses it as a node; no ids, as an AI writes them.
	const TSharedPtr<FJsonObject> PatchDocument = Json(TEXT(R"({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundPatch", "Action": "Create",
		"Body": {"Metadata": {"Description": "Gain stage", "Referenceable": true},
		  "Inputs": {"In": {"Type": "Audio"}, "Gain": {"Type": "Float", "Default": 0.5}},
		  "Outputs": {"Out": {"Type": "Audio"}},
		  "Graphs": {"Default": {"Nodes": {
		    "In": {"Class": "Input", "Member": "In", "Position": [-400, 0]},
		    "Gain": {"Class": "Input", "Member": "Gain", "Position": [-400, 120]},
		    "Mul": {"Class": "UE.Multiply.Audio by Float", "Position": [-150, 0], "Inputs": {"PrimaryOperand": "In", "AdditionalOperands": "Gain"}},
		    "Out": {"Class": "Output", "Member": "Out", "Position": [100, 0], "Inputs": {"Out": "Mul"}}}}}}})"));
	PatchDocument->SetStringField(TEXT("Target"), Patch.PackageName);
	if (!ApplyOk(*this, Patch, PatchDocument, TEXT("patch Create")))
	{
		return false;
	}
	const FString SynthText = FString(TEXT(R"({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundSource", "Action": "Create",
		"Properties": {"OutputFormat": "Stereo"},
		"Body": {"Interfaces": ["UE.Source.OneShot"], "Metadata": {"Description": "AI synth"},
		  "Inputs": {
		    "Pitch": {"Type": "Float", "Default": 0.0, "Widget": {"Range": {"X": -12.0, "Y": 12.0}}},
		    "Cutoff": {"Type": "Float", "Default": 2000.0, "Widget": {"Range": {"X": 20.0, "Y": 20000.0}}},
		    "Wave": {"Type": "WaveAsset"}},
		  "Graphs": {"Default": {
		    "Variables": {"Plays": {"Type": "Int32", "Default": 0}},
		    "Comments": {"Voice": {"Text": "Voice", "Position": [-700, -200], "Size": [1600, 700]}},
		    "Nodes": {
		      "Play": {"Class": "Input", "Member": "UE.Source.OnPlay", "Position": [-700, 0]},
		      "Wave": {"Class": "Input", "Member": "Wave", "Position": [-700, 100]},
		      "Pitch": {"Class": "Input", "Member": "Pitch", "Position": [-700, 200]},
		      "Cutoff": {"Class": "Input", "Member": "Cutoff", "Position": [-700, 300]},
		      "Player": {"Class": "UE.Wave Player.Stereo", "Position": [-450, 0],
		                 "Inputs": {"Play": "Play", "Wave Asset": "Wave", "Pitch Shift": "Pitch"}},
		      "Env": {"Class": "AD Envelope.AD Envelope.Audio", "Position": [-450, 400], "Inputs": {"Trigger": "Play"},
		              "Defaults": {"Decay Time": 0.5}},
		      "Filter": {"Class": "UE.State Variable Filter.Audio", "Position": [-150, 0],
		                 "Inputs": {"In": "Player.Out Left", "Cutoff Frequency": "Cutoff"}},
		      "Wire": {"Class": "UE.Reroute", "Position": [50, 0], "Inputs": {"Value": "Filter.Band Pass"}},
		      "Shape": {"Class": "UE.Multiply.Audio", "Position": [200, 0], "Inputs": {"PrimaryOperand": "Wire", "AdditionalOperands": "Env.Out Envelope"}},
		      "Stage": {"Class": "%PATCH%", "Position": [400, 0], "Inputs": {"In": "Shape"}, "Defaults": {"Gain": 0.8}},
		      "Left": {"Class": "Output", "Member": "UE.OutputFormat.Stereo.Audio:0", "Position": [650, 0], "Inputs": {"UE.OutputFormat.Stereo.Audio:0": "Stage"}},
		      "Right": {"Class": "Output", "Member": "UE.OutputFormat.Stereo.Audio:1", "Position": [650, 100], "Inputs": {"UE.OutputFormat.Stereo.Audio:1": "Stage"}},
		      "Done": {"Class": "Output", "Member": "UE.Source.OneShot.OnFinished", "Position": [650, -150], "Inputs": {"UE.Source.OneShot.OnFinished": "Player.On Finished"}},
		      "Count": {"Class": "GetDelayed", "Member": "Plays", "Position": [-450, 600]},
		      "Inc": {"Class": "UE.Add.Int32", "Position": [-200, 600], "Inputs": {"PrimaryOperand": "Count.Value"}, "Defaults": {"AdditionalOperands": 1}},
		      "Store": {"Class": "Set", "Member": "Plays", "Position": [50, 600], "Inputs": {"Value": "Inc"}}}}}}})"))
		.Replace(TEXT("%PATCH%"), *Patch.ObjectPath);
	// The example of MCP/schemas/assetdocument/MetaSound.md, as written there.
	{
		const FTarget& Blip = Fixtures.Add(TEXT("MSS_Blip"));
		const TSharedPtr<FJsonObject> Example = Json(TEXT(R"({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundSource", "Target": "/Game/Audio/MSS_Blip", "Action": "Create",
 "Properties": {"OutputFormat": "Mono"},
 "Body": {"Interfaces": ["UE.Source.OneShot"],
  "Inputs": {"Pitch": {"Type": "Float", "Default": 440.0, "Widget": {"Range": {"X": 100, "Y": 2000}}}},
  "Graphs": {"Default": {"Nodes": {
    "Play":  {"Class": "Input", "Member": "UE.Source.OnPlay"},
    "Pitch": {"Class": "Input", "Member": "Pitch"},
    "Osc":   {"Class": "UE.Sine.Audio", "Inputs": {"Frequency": "Pitch"}},
    "Env":   {"Class": "AD Envelope.AD Envelope.Audio", "Inputs": {"Trigger": "Play"}, "Defaults": {"Decay Time": 0.3}},
    "Amp":   {"Class": "UE.Multiply.Audio", "Inputs": {"PrimaryOperand": "Osc", "AdditionalOperands": "Env.Out Envelope"}},
    "Out":   {"Class": "Output", "Member": "UE.OutputFormat.Mono.Audio:0", "Inputs": {"UE.OutputFormat.Mono.Audio:0": "Amp"}},
    "Done":  {"Class": "Output", "Member": "UE.Source.OneShot.OnFinished", "Inputs": {"UE.Source.OneShot.OnFinished": "Env.On Done"}}}}}}})"));
		Example->SetStringField(TEXT("Target"), Blip.PackageName);
		const FAssetDocumentResult Result = Apply(Blip, Example);
		if (TestTrue(TEXT("the documented example applies"), Result.IsSuccess()))
		{
			const FAssetDocumentResult Read = ExtractResult(Blip.ObjectPath);
			TestEqual(TEXT("the documented example validates"), StatusOf(Read), FString(TEXT("UpToDate")));
			AddInfo(FString::Printf(TEXT("documented example: %s"), *SerializePretty(Read.Payload)));
			// The documented preset of it, as written there.
			const FTarget& High = Fixtures.Add(TEXT("MSS_BlipHigh"));
			const TSharedPtr<FJsonObject> PresetExample = Json(TEXT(R"({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundSource", "Target": "/Game/Audio/MSS_Blip_High", "Action": "Create",
 "Body": {"Preset": {"Parent": "/Game/Audio/MSS_Blip"}, "Metadata": {"Description": "Higher blip"},
  "Inputs": {"Pitch": {"Default": 880.0}}}})"));
			PresetExample->SetStringField(TEXT("Target"), High.PackageName);
			Child(BodyOf(PresetExample), TEXT("Preset"))->SetStringField(TEXT("Parent"), Blip.PackageName);
			if (ApplyOk(*this, High, PresetExample, TEXT("the documented preset example")))
			{
				const FAssetDocumentResult PresetRead = ExtractResult(High.ObjectPath);
				TestEqual(TEXT("the documented preset example validates"), StatusOf(PresetRead), FString(TEXT("UpToDate")));
				AddInfo(FString::Printf(TEXT("documented preset example: %s"), *SerializePretty(PresetRead.Payload)));
			}
		}
		else
		{
			AddInfo(FString::Printf(TEXT("documented example: %s"), *DiagnosticsText(Result)));
		}
	}
	const TSharedPtr<FJsonObject> SynthDocument = Parse(SynthText);
	SynthDocument->SetStringField(TEXT("Target"), Synth.PackageName);
	const double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Created = Apply(Synth, SynthDocument);
	if (!TestTrue(TEXT("synth Create applies"), Created.IsSuccess()))
	{
		AddError(DiagnosticsText(Created));
		return false;
	}
	AddInfo(FString::Printf(TEXT("synth created in %.0f ms; diagnostics: %s"), Milliseconds(Start), *DiagnosticsText(Created)));
	const FAssetDocumentResult Read = ExtractResult(Synth.ObjectPath);
	if (!TestTrue(TEXT("synth extracts"), Read.IsSuccess()))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("synth %s"), *SerializePretty(Read.Payload)));
	TestFalse(TEXT("synth validates without errors"), StatusOf(Read).StartsWith(TEXT("Error")));
	AddInfo(FString::Printf(TEXT("synth validation: %s"), *StatusOf(Read)));
	// The AI document again (keyed ids): nothing to do.
	{
		const TSharedPtr<FJsonObject> Again = Parse(Serialize(SynthDocument));
		Again->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));
		const FAssetDocumentResult Result = Apply(Synth, Again);
		TestTrue(TEXT("AI document re-applies"), Result.IsSuccess());
		TestFalse(TEXT("AI document re-apply saves nothing"), Result.bSavedAsset);
		FString DiffText;
		TestEqual(TEXT("AI document: Diff is empty"), DiffCount(Again, DiffText), 0);
		if (!DiffText.IsEmpty() && DiffText.Contains(TEXT("\"path\"")))
		{
			AddInfo(DiffText.Left(1200));
		}
	}
	// A preset of the synth (the Content Browser's Create Preset) with Cutoff overridden.
	{
		const FTarget& Preset = Fixtures.Add(TEXT("MSS_SynthPreset"));
		const TSharedPtr<FJsonObject> PresetDocument = Json(TEXT(R"({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundSource", "Action": "Create",
			"Body": {"Preset": {"Parent": "%PARENT%"}, "Inputs": {"Cutoff": {"Default": 500.0}}}})"));
		Child(BodyOf(PresetDocument), TEXT("Preset"))->SetStringField(TEXT("Parent"), Synth.PackageName);
		PresetDocument->SetStringField(TEXT("Target"), Preset.PackageName);
		if (ApplyOk(*this, Preset, PresetDocument, TEXT("synth preset Create")))
		{
			const FAssetDocumentResult PresetRead = ExtractResult(Preset.ObjectPath);
			TestFalse(TEXT("synth preset validates without errors"), StatusOf(PresetRead).StartsWith(TEXT("Error")));
			AddInfo(FString::Printf(TEXT("synth preset %s"), *SerializePretty(PresetRead.Payload)));
			const TSharedPtr<FJsonObject> Again = Parse(Serialize(PresetDocument));
			Again->SetStringField(TEXT("Action"), TEXT("CreateOrUpdate"));
			const FAssetDocumentResult Result = Apply(Preset, Again);
			TestTrue(TEXT("synth preset re-applies"), Result.IsSuccess() && !Result.bSavedAsset);
			// A graph in a preset: its graph follows the parent.
			const TSharedPtr<FJsonObject> WithGraph = Parse(Serialize(Again));
			WithGraph->SetStringField(TEXT("Action"), TEXT("Update"));
			Child(Child(BodyOf(WithGraph), TEXT("Graphs")), TEXT("Default"))->SetObjectField(TEXT("Nodes"), MakeShared<FJsonObject>());
			ExpectRefused(*this, Preset, WithGraph, TEXT("MetaSound.PresetGraphDerived"), TEXT("/Body/Graphs"), TEXT("preset with a graph"));
		}
	}
	// Typical errors fail at their document paths and change nothing.
	struct FBad
	{
		const TCHAR* Stage;
		TFunction<void(const TSharedPtr<FJsonObject>&)> Edit;
		const TCHAR* Code;
		const TCHAR* Path;
	};
	const TArray<FBad> Bad = {
		{TEXT("unknown class"), [](const TSharedPtr<FJsonObject>& Doc) { NodeOf(Doc, TEXT("Shape"))->SetStringField(TEXT("Class"), TEXT("UE.Multiplyy.Audio")); },
			TEXT("MetaSound.UnknownNodeClass"), TEXT("/Body/Graphs/Default/Nodes/Shape/Class")},
		{TEXT("unknown pin"), [](const TSharedPtr<FJsonObject>& Doc) { SetInputs(Doc, TEXT("Player"), TEXT("Volume"), TEXT("Pitch")); },
			TEXT("MetaSound.PinNotFound"), TEXT("/Body/Graphs/Default/Nodes/Player/Inputs/Volume")},
		{TEXT("type mismatch"), [](const TSharedPtr<FJsonObject>& Doc) { SetInputs(Doc, TEXT("Filter"), TEXT("Cutoff Frequency"), TEXT("Play")); },
			TEXT("MetaSound.IncompatibleConnection"), TEXT("/Body/Graphs/Default/Nodes/Filter/Inputs/Cutoff Frequency")},
		{TEXT("cycle"), [](const TSharedPtr<FJsonObject>& Doc) { SetInputs(Doc, TEXT("Shape"), TEXT("AdditionalOperands"), TEXT("Shape")); },
			TEXT("MetaSound.ConnectionCycle"), TEXT("/Body/Graphs/Default/Nodes/Shape/Inputs/AdditionalOperands")},
		{TEXT("unknown page"), [](const TSharedPtr<FJsonObject>& Doc) { Child(Child(BodyOf(Doc), TEXT("Graphs")), TEXT("Mobile"))->SetObjectField(TEXT("Nodes"), MakeShared<FJsonObject>()); },
			TEXT("MetaSound.UnknownPage"), TEXT("/Body/Graphs/Mobile")},
		{TEXT("preset"), [](const TSharedPtr<FJsonObject>& Doc) { Child(BodyOf(Doc), TEXT("Preset"))->SetStringField(TEXT("Parent"), TEXT("/Game/A.A")); },
			TEXT("MetaSound.CannotConvertToPreset"), TEXT("/Body/Preset")},
		{TEXT("open-ended reroute"), [](const TSharedPtr<FJsonObject>& Doc) { NodesOf(Doc)->SetObjectField(TEXT("Loose"), Json(TEXT(R"({"Class": "UE.Reroute"})"))); },
			TEXT("MetaSound.RerouteType"), TEXT("/Body/Graphs/Default/Nodes/Loose")},
	};
	for (const FBad& Case : Bad)
	{
		const TSharedPtr<FJsonObject> Document = Parse(Serialize(SynthDocument));
		Document->SetStringField(TEXT("Action"), TEXT("Update"));
		Case.Edit(Document);
		ExpectRefused(*this, Synth, Document, Case.Code, Case.Path, Case.Stage);
	}
	// Trial 7: a Float into UE.Multiply.Audio's AdditionalOperands; the refusal names the variant of the same node
	// that takes it (UE.Multiply.Audio by Float), not only a converter.
	{
		const TSharedPtr<FJsonObject> Document = Parse(Serialize(SynthDocument));
		Document->SetStringField(TEXT("Action"), TEXT("Update"));
		SetInputs(Document, TEXT("Shape"), TEXT("AdditionalOperands"), TEXT("Cutoff"));
		const FAssetDocumentResult Result = Apply(Synth, Document);
		const FAssetDocumentDiagnostic* Found = Result.Diagnostics.FindByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == TEXT("MetaSound.IncompatibleConnection");
		});
		if (TestNotNull(TEXT("a Float into an Audio operand is refused"), Found))
		{
			AddInfo(Found->Message);
			TestTrue(TEXT("the refusal names the node's variant that takes a Float"), Found->Message.Contains(TEXT("UE.Multiply.Audio by Float")));
			TestFalse(TEXT("not the variants whose other operand is no longer Audio"), Found->Message.Contains(TEXT("UE.Multiply.Float")));
		}
	}
	return true;
}
// An object input's "Default": null clears its default (the Members panel's clear). The edit primitive read a
// null default as "not given" (FReader::Has is false for null) and kept the old wave.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundNullObjectDefaultTest, "UEAssetDoc.MetaSound.NullObjectDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundNullObjectDefaultTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundApplyTests;
	FFixtures Fixtures;
	const FTarget& Patch = Fixtures.Add(TEXT("MSP_NullWave"));
	const FString Wave = TEXT("/Engine/EditorSounds/Notifications/CompileSuccess.CompileSuccess");
	const TSharedPtr<FJsonObject> Document = Json(*FString::Printf(TEXT(R"({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundPatch",
		"Action": "Create", "Body": {"Inputs": {"Wave": {"Type": "WaveAsset", "Default": "%s"}}}})"), *Wave));
	Document->SetStringField(TEXT("Target"), Patch.PackageName);
	if (!ApplyOk(*this, Patch, Document, TEXT("create with a wave default")))
	{
		return false;
	}
	auto DefaultOf = [this, &Patch](const TCHAR* Stage)
	{
		const TSharedPtr<FJsonObject> Read = Extract(*this, Patch.ObjectPath, Stage);
		const TSharedPtr<FJsonObject> Input = Read.IsValid() ? Child(Child(BodyOf(Read), TEXT("Inputs")), TEXT("Wave")) : nullptr;
		FString Value(TEXT("(none)"));
		if (Input.IsValid())
		{
			Input->TryGetStringField(TEXT("Default"), Value);
		}
		return Value;
	};
	TestEqual(TEXT("the wave default"), DefaultOf(TEXT("created")), Wave);
	Child(Child(BodyOf(Document), TEXT("Inputs")), TEXT("Wave"))->SetField(TEXT("Default"), MakeShared<FJsonValueNull>());
	Document->SetStringField(TEXT("Action"), TEXT("Update"));
	if (ApplyOk(*this, Patch, Document, TEXT("Default null")))
	{
		TestEqual(TEXT("Default null clears the wave"), DefaultOf(TEXT("cleared")), FString(TEXT("(none)")));
		const FAssetDocumentResult Again = Apply(Patch, Document);
		TestTrue(TEXT("the same document again is a no-op"), Again.IsSuccess() && !Again.bSavedAsset);
	}
	return true;
}
// Validate's removals hold only removed entries. Moving nodes (new Position values) removes nothing;
// dropping a node is one removal, named by the node's key.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundValidateRemovalsTest, "UEAssetDoc.MetaSound.ValidateRemovals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundValidateRemovalsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundApplyTests;
	FFixtures Fixtures;
	const FTarget& Patch = Fixtures.Add(TEXT("MSP_Removals"));
	const TSharedPtr<FJsonObject> Created = Json(TEXT(R"({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundPatch", "Action": "Create",
		"Body": {"Inputs": {"In": {"Type": "Audio"}, "Gain": {"Type": "Float", "Default": 0.5}},
		  "Outputs": {"Out": {"Type": "Audio"}},
		  "Graphs": {"Default": {"Nodes": {
		    "In": {"Class": "Input", "Member": "In", "Position": [-400, 0]},
		    "Gain": {"Class": "Input", "Member": "Gain", "Position": [-400, 120]},
		    "Mul": {"Class": "UE.Multiply.Audio by Float", "Position": [-150, 0], "Inputs": {"PrimaryOperand": "In", "AdditionalOperands": "Gain"}},
		    "Out": {"Class": "Output", "Member": "Out", "Position": [100, 0], "Inputs": {"Out": "Mul"}}}}}}})"));
	Created->SetStringField(TEXT("Target"), Patch.PackageName);
	if (!ApplyOk(*this, Patch, Created, TEXT("create")))
	{
		return false;
	}
	FString InKey;
	FString MulKey;
	FString OutKey;
	auto Read = [&](const TCHAR* Stage)
	{
		const TSharedPtr<FJsonObject> Extracted = Extract(*this, Patch.ObjectPath, Stage);
		const TSharedPtr<FJsonObject> Nodes = Extracted.IsValid() ? NodesOf(Extracted) : nullptr;
		for (const auto& Node : Nodes.IsValid() ? Nodes->Values : TMap<AssetDocumentServerJson::FKey, TSharedPtr<FJsonValue>>())
		{
			const TSharedPtr<FJsonObject> Object = Node.Value->AsObject();
			const FString Class = Object->GetStringField(TEXT("Class"));
			FString Member;
			Object->TryGetStringField(TEXT("Member"), Member);
			if (Class == TEXT("UE.Multiply.Audio by Float"))
			{
				MulKey = Node.Key;
			}
			else if (Class == TEXT("Output"))
			{
				OutKey = Node.Key;
			}
			else if (Class == TEXT("Input") && Member == TEXT("In"))
			{
				InKey = Node.Key;
			}
		}
		return Extracted;
	};
	auto Validate = [](const TSharedPtr<FJsonObject>& Document)
	{
		FAssetDocumentValidateRequest Request;
		Request.Document = Document;
		return FAssetDocumentModule::Get().GetService().Validate(Request);
	};
	auto RemovalCount = [](const FAssetDocumentResult& Result)
	{
		const TArray<TSharedPtr<FJsonValue>>* Removals = nullptr;
		return Result.Payload.IsValid() && Result.Payload->TryGetArrayField(TEXT("removals"), Removals) ? Removals->Num() : 0;
	};
	const TSharedPtr<FJsonObject> Moved = Read(TEXT("extract"));
	if (!TestTrue(TEXT("the extract has the nodes"), Moved.IsValid() && !InKey.IsEmpty() && !MulKey.IsEmpty() && !OutKey.IsEmpty()))
	{
		return false;
	}
	// One coordinate of the multiply node and both of the output node change.
	NodeOf(Moved, MulKey)->SetArrayField(TEXT("Position"), AssetDocumentMetaSoundApplyTests::Point(-150, 40));
	NodeOf(Moved, OutKey)->SetArrayField(TEXT("Position"), AssetDocumentMetaSoundApplyTests::Point(300, 80));
	const FAssetDocumentResult MovedSummary = Validate(Moved);
	TestTrue(*FString::Printf(TEXT("moving nodes is not a removal (%s)"), *MovedSummary.Message),
		MovedSummary.IsSuccess() && MovedSummary.Message.Contains(TEXT("would change")) && !MovedSummary.Message.Contains(TEXT("removing")));
	TestEqual(TEXT("moving nodes lists no removal"), RemovalCount(MovedSummary), 0);

	const TSharedPtr<FJsonObject> Dropped = Read(TEXT("extract again"));
	NodesOf(Dropped)->RemoveField(MulKey);
	Child(NodeOf(Dropped, OutKey), TEXT("Inputs"))->SetStringField(TEXT("Out"), InKey);
	const FAssetDocumentResult DroppedSummary = Validate(Dropped);
	TestTrue(*FString::Printf(TEXT("dropping a node is one removal, named (%s)"), *DroppedSummary.Message), DroppedSummary.IsSuccess()
		&& DroppedSummary.Message.Contains(TEXT("removing 1 entry")) && DroppedSummary.Message.Contains(FString::Printf(TEXT("/Nodes/%s"), *MulKey)));
	TestEqual(TEXT("dropping a node lists one removal"), RemovalCount(DroppedSummary), 1);
	return true;
}
// BaseRevision is the revision of what the document sees. A Source's bLooping and Duration follow its
// interfaces and are set by the engine when the asset loads (UMetaSoundSource::PostLoad), not by the apply, so an
// extract that held them changed revision after a reload with nobody changing the asset (a false Document.StaleBase).
// They are neither extracted nor authored.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundRevisionIgnoresDerivedLoopingTest, "UEAssetDoc.MetaSound.RevisionIgnoresDerivedLooping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundRevisionIgnoresDerivedLoopingTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundApplyTests;
	FFixtures Fixtures;
	const FTarget& Source = Fixtures.Add(TEXT("MSS_Looping"));
	// No UE.Source.OneShot: a looping Source.
	const TSharedPtr<FJsonObject> Created = Json(TEXT(R"({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundSource", "Action": "Create",
		"Properties": {"OutputFormat": "Mono"}, "Body": {"Interfaces": []}})"));
	Created->SetStringField(TEXT("Target"), Source.PackageName);
	if (!ApplyOk(*this, Source, Created, TEXT("create a looping source")))
	{
		return false;
	}
	auto Revision = [this, &Source](const TCHAR* Stage, bool& bOutHasLooping)
	{
		FAssetDocumentExtractRequest Request;
		Request.AssetPath = Source.ObjectPath;
		Request.bIncludeBaseRevision = true;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Extract(Request);
		TestTrue(*FString::Printf(TEXT("%s: extract succeeds"), Stage), Result.IsSuccess());
		const TSharedPtr<FJsonObject> Read = Result.Payload;
		FString Value;
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		bOutHasLooping = Read.IsValid() && Read->TryGetObjectField(TEXT("Properties"), Properties) && (*Properties)->HasField(TEXT("bLooping"));
		if (Read.IsValid())
		{
			Read->TryGetStringField(TEXT("BaseRevision"), Value);
		}
		return Value;
	};
	bool bHasLooping = false;
	const FString AfterApply = Revision(TEXT("after the apply"), bHasLooping);
	TestFalse(TEXT("the revision is known"), AfterApply.IsEmpty());
	TestFalse(TEXT("the extract has no bLooping"), bHasLooping);
	UMetaSoundSource* Asset = Cast<UMetaSoundSource>(Source.Get());
	if (!TestNotNull(TEXT("source"), Asset))
	{
		return false;
	}
	// What the next load of the package does (UMetaSoundSource::PostLoad).
	Asset->bLooping = Asset->IsLooping();
	Asset->Duration = Asset->GetDuration();
	TestTrue(TEXT("the source loops"), Asset->bLooping != 0);
	TestEqual(TEXT("the revision does not change when the engine derives looping"), Revision(TEXT("after the load-time derivation"), bHasLooping), AfterApply);
	TestFalse(TEXT("the extract still has no bLooping"), bHasLooping);

	const TSharedPtr<FJsonObject> Authored = Json(TEXT(R"({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundSource", "Action": "Update",
		"Properties": {"bLooping": false}, "Body": {}})"));
	Authored->SetStringField(TEXT("Target"), Source.PackageName);
	Asset->GetOutermost()->ClearDirtyFlag();
	ExpectRefused(*this, Source, Authored, TEXT("MetaSound.NotAuthoredProperty"), TEXT("/Properties/bLooping"), TEXT("authored bLooping"));
	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
