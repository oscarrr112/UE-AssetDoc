// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// MetaSound documents (graph pages, page input defaults, the operator build check), through the production
// Service as the MCP tools send them (design §6.2-6.4). The project's MetaSound pages ("Low", "High") are set for each
// test as Project Settings sets them (FScopedProjectPages: in memory, restored afterwards).
//
// UEAssetDoc.MetaSound.Pages: a two-page source made as the editor makes it (opened: its input nodes
// injected; Details > Add Page Graph "Low"; on Low a moved node, a changed pin default, a new node wired in; Low's
// default of an input) passes §6.2 with its timings; the documented two-page example is created from scratch; a page
// the project does not define, a PageDefaults entry for one or for Default, and a failing edit on a page being added
// fail at their document paths and change nothing; a page added, removed and edited by one apply re-applies as a
// no-op and undoes / redoes.
// UEAssetDoc.MetaSound.PageFreeEdits: the §6.3 page edits on two duplicates of MSS_PlayRandom_Oneshot,
// one edited by documents, the other by the editor's calls (Add Page Graph, the page remove button, builder calls on
// the focused page, the page default widgets); after every edit both extract the same and validate the same.
// UEAssetDoc.MetaSound.PagesOpenEditor: with the two-page source open in the MetaSound editor, edits
// of Low, a new page and a failed apply (rolled back) land in the open editor; undo / redo keep the asset whole.
// UEAssetDoc.MetaSound.BuildCheck: a variable's Get -> Add -> Set of the same variable (the editor's
// loop check allows it; the operator build finds the cycle, as Play would) fails with MetaSound.BuildFailed at a node
// of the cycle and rolls back; an asset that already has the error (made by the editor's calls) reads it in Compile,
// takes other edits with a MetaSound.BuildError warning, and a document that removes the cycle clears it.

#include "AssetDocumentMetaSoundApplyTestSupport.h"

#include "Subsystems/AssetEditorSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentMetaSoundPagesTests
{
using namespace AssetDocumentMetaSoundApplyTests;

const FName LowPage(TEXT("Low"));
const FName HighPage(TEXT("High"));
const FGuid LowPageID(0x9A3F1C20, 0x4B7E4D11, 0x8C2A6E55, 0x1D0F0001);
const FGuid HighPageID(0x9A3F1C20, 0x4B7E4D11, 0x8C2A6E55, 0x1D0F0002);
const TCHAR* const Low = TEXT("Low");
const TCHAR* const High = TEXT("High");

TArray<TPair<FName, FGuid>> TestPages()
{
	return {{LowPage, LowPageID}, {HighPage, HighPageID}};
}

FMetasoundFrontendLiteral FloatLiteral(float Value)
{
	FMetasoundFrontendLiteral Literal;
	Literal.Set(Value);
	return Literal;
}

// The editor's graph actions on the focused page (the builder's build page): explicit here.
FGuid AddNodeOnPage(FMetaSoundFrontendDocumentBuilder& Builder, const TCHAR* ClassName, double X, double Y, const FGuid& Page)
{
	FMetasoundFrontendClassName Name;
	FMetasoundFrontendClassName::Parse(ClassName, Name);
	const FMetasoundFrontendNode* Node = Builder.AddNodeByClassName(Name, 1, FGuid::NewGuid(), &Page);
	if (!ensure(Node != nullptr))
	{
		return FGuid();
	}
	const FGuid Id = Node->GetID();
	Builder.SetNodeLocation(Id, FVector2D(X, Y), nullptr, &Page);
	return Id;
}

void ConnectOnPage(FMetaSoundFrontendDocumentBuilder& Builder, const FGuid& From, const FGuid& To, const TCHAR* ToPin, const FGuid& Page)
{
	const FMetasoundFrontendNode* Source = Builder.FindNode(From, &Page);
	const FMetasoundFrontendVertex* In = Builder.FindNodeInput(To, FName(ToPin), &Page);
	if (!ensure(Source != nullptr && !Source->Interface.Outputs.IsEmpty() && In != nullptr))
	{
		return;
	}
	FMetasoundFrontendEdge Edge;
	Edge.FromNodeID = From;
	Edge.FromVertexID = Source->Interface.Outputs[0].VertexID;
	Edge.ToNodeID = To;
	Edge.ToVertexID = In->VertexID;
	Builder.RemoveEdgeToNodeInput(To, In->VertexID, &Page);
	Builder.AddEdge(MoveTemp(Edge), &Page);
}

void SetNodeDefaultOnPage(FMetaSoundFrontendDocumentBuilder& Builder, const FGuid& Node, const TCHAR* Pin, float Value, const FGuid& Page)
{
	if (const FMetasoundFrontendVertex* In = Builder.FindNodeInput(Node, FName(Pin), &Page))
	{
		Builder.SetNodeInputDefault(Node, In->VertexID, FloatLiteral(Value), &Page);
	}
}

TSharedPtr<FJsonObject> Graphs(const TSharedPtr<FJsonObject>& Document)
{
	return Child(BodyOf(Document), TEXT("Graphs"));
}

TSharedPtr<FJsonObject> CopyOf(const TSharedPtr<FJsonObject>& Object)
{
	return Parse(Serialize(Object));
}

TSharedPtr<FJsonObject> PageDefaults(const TSharedPtr<FJsonObject>& Document, const TCHAR* Input)
{
	return Child(Child(Child(BodyOf(Document), TEXT("Inputs")), Input), TEXT("PageDefaults"));
}

FString PositionOf(const TSharedPtr<FJsonObject>& Document, const FString& Page, const FString& Key)
{
	const TSharedPtr<FJsonObject> Nodes = PageNodes(Document, Page);
	const TSharedPtr<FJsonObject>* Node = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Position = nullptr;
	if (!Nodes->TryGetObjectField(Key, Node) || !(*Node)->TryGetArrayField(TEXT("Position"), Position) || Position->Num() != 2)
	{
		return TEXT("-");
	}
	return FString::Printf(TEXT("%.0f,%.0f"), (*Position)[0]->AsNumber(), (*Position)[1]->AsNumber());
}

bool HasNodeOfClass(const TSharedPtr<FJsonObject>& Document, const FString& Page, const TCHAR* Class)
{
	for (const auto& Node : PageNodes(Document, Page)->Values)
	{
		if (Node.Value->AsObject()->GetStringField(TEXT("Class")) == Class)
		{
			return true;
		}
	}
	return false;
}

// The diagnostics of a refused apply with their details (primitive, primitive-side path).
FString DiagnosticsWithDetails(const FAssetDocumentResult& Result)
{
	TArray<FString> Lines;
	for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
	{
		Lines.Add(FString::Printf(TEXT("[%s] %s %s %s"), *Diagnostic.Code, *Diagnostic.Path, *Diagnostic.Message,
			Diagnostic.Details.IsValid() ? *Serialize(Diagnostic.Details) : TEXT("")));
	}
	return FString::Join(Lines, TEXT(" | "));
}

// A source with two pages made as the editor makes them: MSS_PlayRandom_Oneshot duplicated and opened (its input
// nodes injected, as an apply does first), Details > Add Page Graph "Low" (a copy of Default), then on Low: Wave
// Player moved, Clamp's Min changed, a Sine added and wired into Multiply_3, and PitchBase's Low default set.
UObject* MakePagedFixture(FAutomationTestBase& Test, const FTarget& Target)
{
	UObject* Asset = Duplicate(Test, LoadQuiet(OneShot), Target);
	if (Asset == nullptr)
	{
		return nullptr;
	}
	const TSharedPtr<FJsonObject> Start = Extract(Test, Target.ObjectPath, TEXT("paged fixture start"));
	if (!Start.IsValid())
	{
		return nullptr;
	}
	Child(BodyOf(Start), TEXT("Metadata"))->SetStringField(TEXT("Description"), TEXT("two pages"));
	if (!ApplyOk(Test, Target, ForTarget(Start, Target, TEXT("Update")), TEXT("paged fixture start")))
	{
		return nullptr;
	}
	EditorAddPage(*Asset, LowPage);
	const TSharedPtr<FJsonObject> Paged = Extract(Test, Target.ObjectPath, TEXT("paged fixture after Add Page Graph"));
	if (!Paged.IsValid() || !Test.TestTrue(TEXT("Add Page Graph made page Low"), PagesOf(Paged).Contains(Low)))
	{
		return nullptr;
	}
	{
		FEditorSide Side(*Asset);
		FMetaSoundFrontendDocumentBuilder& Builder = Side.Builder;
		Builder.SetNodeLocation(NodeIdOnPage(Paged, Low, TEXT("Wave_Player")), FVector2D(1900, 0), nullptr, &LowPageID);
		SetNodeDefaultOnPage(Builder, NodeIdOnPage(Paged, Low, TEXT("Clamp")), TEXT("Min"), -40.f, LowPageID);
		const FGuid Sine = AddNodeOnPage(Builder, TEXT("UE.Sine.Audio"), 1500, 1000, LowPageID);
		ConnectOnPage(Builder, Sine, NodeIdOnPage(Paged, Low, TEXT("Multiply_3")), TEXT("PrimaryOperand"), LowPageID);
		Builder.SetGraphInputDefault(TEXT("PitchBase"), FloatLiteral(0.25f), &LowPageID);
	}
	return Save(Test, Asset, TEXT("paged fixture")) ? Asset : nullptr;
}
} // namespace AssetDocumentMetaSoundPagesTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundPagesTest, "UEAssetDoc.MetaSound.Pages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundPagesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundPagesTests;
	const FScopedProjectPages Pages(TestPages());
	if (GEditor == nullptr || GEditor->Trans == nullptr || !TestTrue(TEXT("project pages set"), Pages.IsValid()))
	{
		return false;
	}
	FFixtures Fixtures;
	const FTarget& Fixture = Fixtures.Add(TEXT("MSS_Paged"));
	UObject* Asset = MakePagedFixture(*this, Fixture);
	if (!TestNotNull(TEXT("the two-page fixture"), Asset))
	{
		return false;
	}
	const double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Original = ExtractResult(Fixture.ObjectPath);
	const double ExtractMs = Milliseconds(Start);
	const TSharedPtr<FJsonObject> Extracted = Original.IsSuccess() ? Original.Payload : nullptr;
	if (!TestNotNull(TEXT("the two-page fixture extracts"), Extracted.Get()))
	{
		AddInfo(DiagnosticsText(Original));
		return false;
	}
	AddInfo(FString::Printf(TEXT("two-page fixture: %s"), *SerializePretty(Extracted)));
	TestEqual(TEXT("two pages"), PagesOf(Extracted).Num(), 2);
	TestEqual(TEXT("Default keeps Wave Player"), PositionOf(Extracted, TEXT("Default"), TEXT("Wave_Player")), FString(TEXT("1792,0")));
	TestEqual(TEXT("Low moved Wave Player"), PositionOf(Extracted, Low, TEXT("Wave_Player")), FString(TEXT("1900,0")));
	TestEqual(TEXT("Low has the Sine"), KeyAtOnPage(Extracted, Low, TEXT("UE.Sine.Audio"), 1500, 1000).IsEmpty(), false);
	TestEqual(TEXT("Low's Clamp Min"), Child(Child(PageNodes(Extracted, Low), TEXT("Clamp")), TEXT("Defaults"))->GetNumberField(TEXT("Min")), -40.0);
	TestEqual(TEXT("PitchBase's Low default"), PageDefaults(Extracted, TEXT("PitchBase"))->GetNumberField(Low), 0.25);
	const FAssetDocumentResult Inspected = Inspect(Fixture.ObjectPath);
	if (TestTrue(TEXT("Inspect succeeds"), Inspected.IsSuccess() && Inspected.Payload.IsValid()))
	{
		TestEqual(TEXT("Inspect: the page that plays"), Inspected.Payload->GetStringField(TEXT("RegisteredPage")), FString(TEXT("Default")));
		TArray<FString> Configured;
		Inspected.Payload->TryGetStringArrayField(TEXT("ConfiguredPages"), Configured);
		TestEqual(TEXT("Inspect: configured pages"), FString::Join(Configured, TEXT(",")), FString(TEXT("Default,Low,High")));
	}

	// §6.2 with the page settings.
	{
		const FTarget& Created = Fixtures.Add(TEXT("MSS_PagedNew"));
		const FRoundTrip Trip = RoundTrip(*this, Extracted, StatusOf(Original), Created, TEXT("two-page fixture"));
		if (!TestTrue(TEXT("the two-page extraction creates"), Trip.Applied.IsSuccess()))
		{
			AddError(DiagnosticsWithDetails(Trip.Applied));
		}
		for (const FString& Problem : Trip.Problems)
		{
			AddError(FString::Printf(TEXT("two-page fixture: %s"), *Problem));
		}
		AddInfo(FString::Printf(TEXT("two-page fixture §6.2: extract %.0f ms, %s | %s"), ExtractMs, *Trip.Timings(), *Trip.Status.Left(120)));
	}

	// The documented example, as written in MCP/schemas/assetdocument/MetaSound/pages.md.
	{
		const FTarget& Hum = Fixtures.Add(TEXT("MSS_Hum"));
		const TSharedPtr<FJsonObject> Example = Json(TEXT(R"({"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundSource", "Target": "/Game/Audio/MSS_Hum", "Action": "Create",
 "Properties": {"OutputFormat": "Mono"},
 "Body": {
  "Inputs": {"Pitch": {"Type": "Float", "Default": 440.0, "PageDefaults": {"Low": 220.0}}},
  "Graphs": {
   "Default": {"Nodes": {
     "Play":  {"Class": "Input", "Member": "UE.Source.OnPlay"},
     "Pitch": {"Class": "Input", "Member": "Pitch"},
     "Osc":   {"Class": "UE.Sine.Audio", "Inputs": {"Frequency": "Pitch"}},
     "Out":   {"Class": "Output", "Member": "UE.OutputFormat.Mono.Audio:0", "Inputs": {"UE.OutputFormat.Mono.Audio:0": "Osc"}}}},
   "Low": {"Nodes": {
     "Play":   {"Class": "Input", "Member": "UE.Source.OnPlay"},
     "Pitch":  {"Class": "Input", "Member": "Pitch"},
     "Osc":    {"Class": "UE.Sine.Audio", "Inputs": {"Frequency": "Pitch"}},
     "Filter": {"Class": "UE.State Variable Filter.Audio", "Inputs": {"In": "Osc"}, "Defaults": {"Cutoff Frequency": 800.0}},
     "Out":    {"Class": "Output", "Member": "UE.OutputFormat.Mono.Audio:0", "Inputs": {"UE.OutputFormat.Mono.Audio:0": "Filter.Band Pass"}}}}}}})"));
		Example->SetStringField(TEXT("Target"), Hum.PackageName);
		const double CreateStart = FPlatformTime::Seconds();
		if (ApplyOk(*this, Hum, Example, TEXT("the documented two-page example")))
		{
			AddInfo(FString::Printf(TEXT("documented two-page example created in %.0f ms"), Milliseconds(CreateStart)));
			const FAssetDocumentResult Read = ExtractResult(Hum.ObjectPath);
			TestEqual(TEXT("the documented two-page example validates"), StatusOf(Read), FString(TEXT("UpToDate")));
			if (Read.IsSuccess())
			{
				AddInfo(FString::Printf(TEXT("documented two-page example: %s"), *SerializePretty(Read.Payload)));
				TestEqual(TEXT("the example has two pages"), PagesOf(Read.Payload).Num(), 2);
				TestEqual(TEXT("the example's Low default"), PageDefaults(Read.Payload, TEXT("Pitch"))->GetNumberField(Low), 220.0);
				TestTrue(TEXT("the example's Low filter"), HasNodeOfClass(Read.Payload, Low, TEXT("UE.State Variable Filter.Audio")));
				TestFalse(TEXT("no filter on Default"), HasNodeOfClass(Read.Payload, TEXT("Default"), TEXT("UE.State Variable Filter.Audio")));
				const FAssetDocumentResult Again = Apply(Hum, ForTarget(Example, Hum, TEXT("Update")));
				TestTrue(TEXT("the documented example re-applies as a no-op"), Again.IsSuccess() && !Again.bSavedAsset);
			}
		}
	}

	// Refusals: nothing changes.
	{
		const TSharedPtr<FJsonObject> Unknown = ForTarget(Extracted, Fixture, TEXT("Update"));
		Graphs(Unknown)->SetObjectField(TEXT("Nowhere"), CopyOf(Graphs(Unknown)->GetObjectField(TEXT("Default"))));
		ExpectRefused(*this, Fixture, Unknown, TEXT("MetaSound.UnknownPage"), TEXT("/Body/Graphs/Nowhere"), TEXT("a page the project does not define"));

		const TSharedPtr<FJsonObject> UnknownDefault = ForTarget(Extracted, Fixture, TEXT("Update"));
		PageDefaults(UnknownDefault, TEXT("PitchBase"))->SetNumberField(TEXT("Nowhere"), 1.0);
		ExpectRefused(*this, Fixture, UnknownDefault, TEXT("MetaSound.UnknownPage"), TEXT("/Body/Inputs/PitchBase/PageDefaults/Nowhere"),
			TEXT("a page default for an unknown page"));

		const TSharedPtr<FJsonObject> DefaultPage = ForTarget(Extracted, Fixture, TEXT("Update"));
		PageDefaults(DefaultPage, TEXT("PitchBase"))->SetNumberField(TEXT("Default"), 1.0);
		ExpectRefused(*this, Fixture, DefaultPage, TEXT("MetaSound.InvalidBody"), TEXT("/Body/Inputs/PitchBase/PageDefaults/Default"),
			TEXT("a page default for Default"));

		// A failing edit on a page the same apply adds: the page is not left behind.
		const TSharedPtr<FJsonObject> Failing = ForTarget(Extracted, Fixture, TEXT("Update"));
		const TSharedPtr<FJsonObject> HighGraph = CopyOf(Graphs(Failing)->GetObjectField(TEXT("Default")));
		Child(Child(Child(HighGraph, TEXT("Nodes")), TEXT("Wave_Player")), TEXT("Inputs"))->SetStringField(TEXT("Play"), TEXT("Num"));
		Graphs(Failing)->SetObjectField(High, HighGraph);
		ExpectRefused(*this, Fixture, Failing, TEXT("MetaSound.IncompatibleConnection"), TEXT("/Body/Graphs/High/Nodes/Wave_Player/Inputs/Play"),
			TEXT("a failing edit on a new page"));
	}

	// One apply adds High (from Low, edited), removes Low and moves the page default; then no-op, undo / redo.
	{
		ReleaseUndoHistory();
		const TSharedPtr<FJsonObject> Edit = ForTarget(Extracted, Fixture, TEXT("Update"));
		const TSharedPtr<FJsonObject> HighGraph = CopyOf(Graphs(Edit)->GetObjectField(Low));
		Child(Child(HighGraph, TEXT("Nodes")), TEXT("Wave_Player"))->SetArrayField(TEXT("Position"), AssetDocumentMetaSoundApplyTests::Point(2000, 0));
		Graphs(Edit)->SetObjectField(High, HighGraph);
		Graphs(Edit)->RemoveField(Low);
		Child(Child(BodyOf(Edit), TEXT("Inputs")), TEXT("PitchBase"))->SetObjectField(TEXT("PageDefaults"), Json(TEXT(R"({"High": 0.5})")));
		{
			// PageDefaults is a complete map: validate counts and names PitchBase's dropped Low value with the removed
			// page.
			FAssetDocumentValidateRequest Request;
			Request.Document = Edit;
			const FAssetDocumentResult Summary = FAssetDocumentModule::Get().GetService().Validate(Request);
			TestTrue(*FString::Printf(TEXT("validate names the removed page default (%s)"), *Summary.Message), Summary.IsSuccess()
				&& Summary.Message.Contains(TEXT("/Body/Inputs/PitchBase/PageDefaults/Low (Low)")));
		}
		const double EditStart = FPlatformTime::Seconds();
		if (ApplyOk(*this, Fixture, Edit, TEXT("page add / remove")))
		{
			AddInfo(FString::Printf(TEXT("page add / remove applied in %.0f ms"), Milliseconds(EditStart)));
			const TSharedPtr<FJsonObject> After = Extract(*this, Fixture.ObjectPath, TEXT("after page add / remove"));
			if (After.IsValid())
			{
				TArray<FString> AfterPages = PagesOf(After);
				AfterPages.Sort();
				TestEqual(TEXT("pages Default and High"), FString::Join(AfterPages, TEXT(",")), FString(TEXT("Default,High")));
				TestEqual(TEXT("High's Wave Player"), PositionOf(After, High, TEXT("Wave_Player")), FString(TEXT("2000,0")));
				TestFalse(TEXT("High has Low's Sine"), KeyAtOnPage(After, High, TEXT("UE.Sine.Audio"), 1500, 1000).IsEmpty());
				TestEqual(TEXT("PitchBase's page defaults"), Serialize(PageDefaults(After, TEXT("PitchBase"))), Serialize(Json(TEXT(R"({"High": 0.5})"))));
				const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Fixture.PackageFile);
				const FAssetDocumentResult Again = Apply(Fixture, ForTarget(After, Fixture, TEXT("Update")));
				TestTrue(TEXT("the re-apply is a no-op"), Again.IsSuccess() && !Again.bSavedAsset
					&& IFileManager::Get().GetTimeStamp(*Fixture.PackageFile) == Stamp);
				UndoRedoAll(*this, Fixture, TEXT("pages"));
				TestEqual(TEXT("undo / redo keep the paged apply"), Serialize(Extract(*this, Fixture.ObjectPath, TEXT("after undo / redo"))), Serialize(After));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundPageFreeEditsTest, "UEAssetDoc.MetaSound.PageFreeEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundPageFreeEditsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundPagesTests;
	const FScopedProjectPages Pages(TestPages());
	UObject* Source = LoadQuiet(OneShot);
	if (GEditor == nullptr || GEditor->Trans == nullptr || !TestTrue(TEXT("project pages set"), Pages.IsValid())
		|| !TestNotNull(TEXT("MSS_PlayRandom_Oneshot loads"), Source))
	{
		return false;
	}
	FFixtures Fixtures;
	const FTarget& DocumentSide = Fixtures.Add(TEXT("MSS_PageDoc"));
	const FTarget& EditorSide = Fixtures.Add(TEXT("MSS_PageEd"));
	if (Duplicate(*this, Source, DocumentSide) == nullptr || Duplicate(*this, Source, EditorSide) == nullptr
		|| !StartBoth(*this, DocumentSide, EditorSide, TEXT("pages")))
	{
		return false;
	}
	TArray<FFreeEdit> Edits;
	Edits.Add({TEXT("add page Low"),
		[](const TSharedPtr<FJsonObject>& Doc)
		{
			Graphs(Doc)->SetObjectField(Low, CopyOf(Graphs(Doc)->GetObjectField(TEXT("Default"))));
		},
		[](UObject& Asset, const TSharedPtr<FJsonObject>&)
		{
			EditorAddPage(Asset, LowPage);
		}});
	Edits.Add({TEXT("move, re-default and add a node on Low"),
		[](const TSharedPtr<FJsonObject>& Doc)
		{
			const TSharedPtr<FJsonObject> Nodes = PageNodes(Doc, Low);
			Child(Nodes, TEXT("Wave_Player"))->SetArrayField(TEXT("Position"), AssetDocumentMetaSoundApplyTests::Point(1900, 0));
			Child(Child(Nodes, TEXT("Clamp")), TEXT("Defaults"))->SetNumberField(TEXT("Min"), -40.0);
			Nodes->SetObjectField(TEXT("LowSine"), Json(TEXT(R"({"Class": "UE.Sine.Audio", "Position": [1500, 1000]})")));
		},
		[](UObject& Asset, const TSharedPtr<FJsonObject>& Doc)
		{
			FEditorSide Side(Asset);
			Side.Builder.SetNodeLocation(NodeIdOnPage(Doc, Low, TEXT("Wave_Player")), FVector2D(1900, 0), nullptr, &LowPageID);
			SetNodeDefaultOnPage(Side.Builder, NodeIdOnPage(Doc, Low, TEXT("Clamp")), TEXT("Min"), -40.f, LowPageID);
			AddNodeOnPage(Side.Builder, TEXT("UE.Sine.Audio"), 1500, 1000, LowPageID);
		}});
	Edits.Add({TEXT("connect on Low"),
		[](const TSharedPtr<FJsonObject>& Doc)
		{
			const FString Sine = KeyAtOnPage(Doc, Low, TEXT("UE.Sine.Audio"), 1500, 1000);
			Child(Child(PageNodes(Doc, Low), TEXT("Multiply_3")), TEXT("Inputs"))->SetStringField(TEXT("PrimaryOperand"), Sine);
		},
		[](UObject& Asset, const TSharedPtr<FJsonObject>& Doc)
		{
			FEditorSide Side(Asset);
			ConnectOnPage(Side.Builder, NodeIdOnPage(Doc, Low, KeyAtOnPage(Doc, Low, TEXT("UE.Sine.Audio"), 1500, 1000)),
				NodeIdOnPage(Doc, Low, TEXT("Multiply_3")), TEXT("PrimaryOperand"), LowPageID);
		}});
	Edits.Add({TEXT("Low default of an input"),
		[](const TSharedPtr<FJsonObject>& Doc)
		{
			PageDefaults(Doc, TEXT("PitchBase"))->SetNumberField(Low, 0.5);
		},
		[](UObject& Asset, const TSharedPtr<FJsonObject>&)
		{
			FEditorSide Side(Asset);
			Side.Builder.SetGraphInputDefault(TEXT("PitchBase"), FloatLiteral(0.5f), &LowPageID);
		}});
	Edits.Add({TEXT("add page High (a copy of the last page)"),
		[](const TSharedPtr<FJsonObject>& Doc)
		{
			Graphs(Doc)->SetObjectField(High, CopyOf(Graphs(Doc)->GetObjectField(Low)));
		},
		[](UObject& Asset, const TSharedPtr<FJsonObject>&)
		{
			EditorAddPage(Asset, HighPage);
		}});
	Edits.Add({TEXT("delete a node on Low"),
		[](const TSharedPtr<FJsonObject>& Doc)
		{
			const TSharedPtr<FJsonObject> Nodes = PageNodes(Doc, Low);
			Nodes->RemoveField(KeyAtOnPage(Doc, Low, TEXT("UE.Sine.Audio"), 1500, 1000));
			Child(Child(Nodes, TEXT("Multiply_3")), TEXT("Inputs"))->RemoveField(TEXT("PrimaryOperand"));
		},
		[](UObject& Asset, const TSharedPtr<FJsonObject>& Doc)
		{
			FEditorSide Side(Asset);
			Side.Builder.RemoveNode(NodeIdOnPage(Doc, Low, KeyAtOnPage(Doc, Low, TEXT("UE.Sine.Audio"), 1500, 1000)), &LowPageID);
		}});
	Edits.Add({TEXT("remove the Low default (the page default's remove button)"),
		[](const TSharedPtr<FJsonObject>& Doc)
		{
			Child(Child(BodyOf(Doc), TEXT("Inputs")), TEXT("PitchBase"))->RemoveField(TEXT("PageDefaults"));
		},
		[](UObject& Asset, const TSharedPtr<FJsonObject>&)
		{
			FEditorSide Side(Asset);
			TArray<FMetasoundFrontendClassInputDefault> Kept;
			if (const FMetasoundFrontendClassInput* Input = Side.Builder.FindGraphInput(TEXT("PitchBase")))
			{
				for (const FMetasoundFrontendClassInputDefault& Default : Input->GetDefaults())
				{
					if (Default.PageID != LowPageID)
					{
						Kept.Add(Default);
					}
				}
			}
			Side.Builder.SetGraphInputDefaults(TEXT("PitchBase"), MoveTemp(Kept));
		}});
	Edits.Add({TEXT("remove page Low"),
		[](const TSharedPtr<FJsonObject>& Doc)
		{
			Graphs(Doc)->RemoveField(Low);
		},
		[](UObject& Asset, const TSharedPtr<FJsonObject>&)
		{
			EditorRemovePage(Asset, LowPageID);
		}});
	TMap<FString, FString> IdMap;
	for (const FFreeEdit& Edit : Edits)
	{
		if (!CompareFreeEdit(*this, DocumentSide, EditorSide, Edit, IdMap, TEXT("pages")))
		{
			break;
		}
	}
	// The document side's result: its unchanged re-apply is a no-op and it reloads the same.
	const TSharedPtr<FJsonObject> Final = Extract(*this, DocumentSide.ObjectPath, TEXT("pages final"));
	if (Final.IsValid())
	{
		const FTarget& Created = Fixtures.Add(TEXT("MSS_PageNew"));
		const FRoundTrip Trip = RoundTrip(*this, Final, StatusOf(ExtractResult(DocumentSide.ObjectPath)), Created, TEXT("page free edits"));
		if (!TestTrue(TEXT("the edited two-page asset creates"), Trip.Applied.IsSuccess()))
		{
			AddError(DiagnosticsWithDetails(Trip.Applied));
		}
		for (const FString& Problem : Trip.Problems)
		{
			AddError(FString::Printf(TEXT("page free edits: %s"), *Problem));
		}
		AddInfo(FString::Printf(TEXT("page free edits §6.2: %s"), *Trip.Timings()));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundPagesOpenEditorTest, "UEAssetDoc.MetaSound.PagesOpenEditor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundPagesOpenEditorTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundPagesTests;
	const FScopedProjectPages Pages(TestPages());
	UAssetEditorSubsystem* Editors = GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (Editors == nullptr || GEditor->Trans == nullptr || !TestTrue(TEXT("project pages set"), Pages.IsValid()))
	{
		return false;
	}
	IgnoreUnregisteredMidiNoteTrigger(*this);
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("MSS_PagedOpen"));
	UObject* Asset = MakePagedFixture(*this, Target);
	if (!TestNotNull(TEXT("the two-page fixture"), Asset))
	{
		return false;
	}
	UnloadMetaSoundsNeedingAudio(*this, *Asset);
	if (!Editors->OpenEditorForAsset(Asset))
	{
		AddWarning(TEXT("The MetaSound editor did not open in this session; the open-editor apply with pages was not exercised."));
		return true;
	}
	ON_SCOPE_EXIT
	{
		if (UObject* Open = Target.Get())
		{
			Editors->CloseAllEditorsForAsset(Open);
		}
	};
	TickEditor();
	ReleaseUndoHistory();

	const TSharedPtr<FJsonObject> Baseline = Extract(*this, Target.ObjectPath, TEXT("open baseline"));
	if (!Baseline.IsValid())
	{
		return false;
	}
	// Low edited and High added while the editor is open.
	const TSharedPtr<FJsonObject> Edit = ForTarget(Baseline, Target, TEXT("Update"));
	PageNodes(Edit, Low)->SetObjectField(TEXT("Extra"), Json(TEXT(R"({"Class": "UE.Sine.Audio", "Position": [0, 1200]})")));
	PageDefaults(Edit, TEXT("PitchBase"))->SetNumberField(Low, 0.75);
	Graphs(Edit)->SetObjectField(High, CopyOf(Graphs(Edit)->GetObjectField(TEXT("Default"))));
	const double Start = FPlatformTime::Seconds();
	if (!ApplyOk(*this, Target, Edit, TEXT("page apply while open")))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("page apply while open in %.0f ms"), Milliseconds(Start)));
	TickEditor();
	TestTrue(TEXT("the editor is still open"), MetaSoundIsOpen(*Editors, *Asset));
	TestNotNull(TEXT("the open editor has its graph"), EditorGraphOf(*Asset));
	const TSharedPtr<FJsonObject> Applied = Extract(*this, Target.ObjectPath, TEXT("after page apply while open"));
	if (!Applied.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("three pages"), PagesOf(Applied).Num(), 3);
	TestFalse(TEXT("Low has the new node"), KeyAtOnPage(Applied, Low, TEXT("UE.Sine.Audio"), 0, 1200).IsEmpty());
	TestEqual(TEXT("PitchBase's Low default"), PageDefaults(Applied, TEXT("PitchBase"))->GetNumberField(Low), 0.75);

	// A failing apply on a page rolls back with the editor open.
	const TSharedPtr<FJsonObject> Failing = ForTarget(Applied, Target, TEXT("Update"));
	Child(Child(PageNodes(Failing, Low), TEXT("Wave_Player")), TEXT("Inputs"))->SetStringField(TEXT("Play"), TEXT("Num"));
	ExpectRefused(*this, Target, Failing, TEXT("MetaSound.IncompatibleConnection"), TEXT("/Body/Graphs/Low/Nodes/Wave_Player/Inputs/Play"),
		TEXT("failing page apply while open"));
	TickEditor();

	// The editor's own edit on Low, then undo / redo of everything left.
	{
		FEditorSide Side(*Asset);
		Side.Builder.SetNodeLocation(NodeIdOnPage(Applied, Low, TEXT("Wave_Player")), FVector2D(2100, 0), nullptr, &LowPageID);
	}
	TickEditor();
	const FString Human = Serialize(Extract(*this, Target.ObjectPath, TEXT("after the human edit")));
	TestTrue(TEXT("the human edit landed"), Human.Contains(TEXT("[2100,0]")));
	UndoRedoAll(*this, Target, TEXT("pages open editor"));
	TickEditor();
	TestEqual(TEXT("undo / redo keep the human edit"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after undo / redo"))), Human);
	TestTrue(TEXT("the asset is still open in its editor"), MetaSoundIsOpen(*Editors, *Asset));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundBuildCheckTest, "UEAssetDoc.MetaSound.BuildCheck",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundBuildCheckTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentMetaSoundPagesTests;
	UObject* Source = LoadQuiet(OneShot);
	if (GEditor == nullptr || GEditor->Trans == nullptr || !TestNotNull(TEXT("MSS_PlayRandom_Oneshot loads"), Source))
	{
		return false;
	}
	const TCHAR* const BuildError = TEXT("does not build into an operator");
	FFixtures Fixtures;

	// A new cycle through a variable: refused at a node of the cycle, nothing changes.
	{
		const FTarget& Target = Fixtures.Add(TEXT("MSS_BuildCycle"));
		if (!TestNotNull(TEXT("duplicate"), Duplicate(*this, Source, Target)))
		{
			return false;
		}
		const FAssetDocumentResult Before = ExtractResult(Target.ObjectPath);
		if (!TestTrue(TEXT("baseline extracts"), Before.IsSuccess()))
		{
			return false;
		}
		TestFalse(TEXT("MSS_PlayRandom_Oneshot builds"), StatusOf(Before).Contains(BuildError));
		const TSharedPtr<FJsonObject> Document = ForTarget(Before.Payload, Target, TEXT("Update"));
		Child(PageOf(Document), TEXT("Variables"))->SetObjectField(TEXT("Acc"), Json(TEXT(R"({"Type": "Float", "Default": 0.0})")));
		NodesOf(Document)->SetObjectField(TEXT("AccGet"), Json(TEXT(R"({"Class": "Get", "Member": "Acc", "Position": [600, 1700]})")));
		NodesOf(Document)->SetObjectField(TEXT("AccAdd"), Json(TEXT(R"({"Class": "UE.Add.Float", "Position": [800, 1700],
			"Inputs": {"PrimaryOperand": "AccGet.Value"}, "Defaults": {"AdditionalOperands": 1.0}})")));
		NodesOf(Document)->SetObjectField(TEXT("AccSet"), Json(TEXT(R"({"Class": "Set", "Member": "Acc", "Position": [1000, 1700], "Inputs": {"Value": "AccAdd"}})")));
		const FString Baseline = Serialize(Before.Payload);
		const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Target.PackageFile);
		const double Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Result = Apply(Target, Document);
		AddInfo(FString::Printf(TEXT("cycle apply refused in %.0f ms: %s"), Milliseconds(Start), *DiagnosticsText(Result)));
		TestFalse(TEXT("the cycle is refused"), Result.IsSuccess());
		const FAssetDocumentDiagnostic* Found = Result.Diagnostics.FindByPredicate([](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("MetaSound.BuildFailed"); });
		if (TestNotNull(TEXT("MetaSound.BuildFailed"), Found))
		{
			TestTrue(TEXT("at a node of the cycle"), Found->Path == TEXT("/Body/Graphs/Default/Nodes/AccGet") || Found->Path == TEXT("/Body/Graphs/Default/Nodes/AccAdd")
				|| Found->Path == TEXT("/Body/Graphs/Default/Nodes/AccSet"));
			TestTrue(TEXT("the message says why"), Found->Message.Contains(BuildError));
		}
		const UObject* Asset = Target.Get();
		TestFalse(TEXT("package clean"), Asset != nullptr && Asset->GetOutermost()->IsDirty());
		TestEqual(TEXT("package file untouched"), IFileManager::Get().GetTimeStamp(*Target.PackageFile), Stamp);
		TestEqual(TEXT("extraction unchanged"), Serialize(Extract(*this, Target.ObjectPath, TEXT("after the refused cycle"))), Baseline);
	}

	// The same cycle made by the editor's calls: read, kept with a warning, then removed by a document.
	{
		const FTarget& Target = Fixtures.Add(TEXT("MSS_BuildCycleEd"));
		UObject* Asset = Duplicate(*this, Source, Target);
		if (!TestNotNull(TEXT("duplicate"), Asset))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Clean = Extract(*this, Target.ObjectPath, TEXT("before the editor's cycle"));
		{
			FEditorSide Side(*Asset);
			FMetaSoundFrontendDocumentBuilder& Builder = Side.Builder;
			const FMetasoundFrontendLiteral Zero = FloatLiteral(0.f);
			Builder.AddGraphVariable(TEXT("Acc"), TEXT("Float"), &Zero);
			const FMetasoundFrontendNode* Get = Builder.AddGraphVariableAccessorNode(TEXT("Acc"));
			const FGuid GetId = Get != nullptr ? Get->GetID() : FGuid();
			Side.Move(GetId, 600, 1700);
			const FGuid AddId = Side.AddNode(TEXT("UE.Add.Float"), 800, 1700);
			const FMetasoundFrontendNode* Set = Builder.AddGraphVariableMutatorNode(TEXT("Acc"));
			const FGuid SetId = Set != nullptr ? Set->GetID() : FGuid();
			Side.Move(SetId, 1000, 1700);
			Side.Connect(GetId, nullptr, AddId, TEXT("PrimaryOperand"));
			Side.Connect(AddId, nullptr, SetId, TEXT("Value"));
		}
		if (!Save(*this, Asset, TEXT("the editor's cycle")))
		{
			return false;
		}
		const double Start = FPlatformTime::Seconds();
		const FAssetDocumentResult Read = ExtractResult(Target.ObjectPath);
		AddInfo(FString::Printf(TEXT("extract with the build check in %.0f ms: %s"), Milliseconds(Start), *StatusOf(Read)));
		if (!TestTrue(TEXT("the editor's cycle extracts"), Read.IsSuccess()))
		{
			return false;
		}
		TestTrue(TEXT("Compile reports the build error"), StatusOf(Read).Contains(BuildError));
		const TSharedPtr<FJsonObject> Other = ForTarget(Read.Payload, Target, TEXT("Update"));
		Child(BodyOf(Other), TEXT("Metadata"))->SetStringField(TEXT("Description"), TEXT("still cycling"));
		const FAssetDocumentResult Kept = Apply(Target, Other);
		TestTrue(TEXT("an unrelated edit applies"), Kept.IsSuccess());
		TestTrue(TEXT("with a MetaSound.BuildError warning"), HasCode(Kept, TEXT("MetaSound.BuildError")));
		if (Clean.IsValid())
		{
			const TSharedPtr<FJsonObject> Fixed = ForTarget(Clean, Target, TEXT("Update"));
			if (ApplyOk(*this, Target, Fixed, TEXT("the cycle removed by a document")))
			{
				TestFalse(TEXT("the build error is gone"), StatusOf(ExtractResult(Target.ObjectPath)).Contains(BuildError));
			}
		}
	}
	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
