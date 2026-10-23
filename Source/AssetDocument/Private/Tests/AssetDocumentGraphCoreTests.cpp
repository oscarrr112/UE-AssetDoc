// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Graphs/AssetDocumentGraphParser.h"
#include "Graphs/AssetDocumentGraphDefinitionResolver.h"
#include "Graphs/AssetDocumentGraphDiff.h"
#include "Profiles/BlueprintGraphAdapterCommon.h"

#include "AssetDocumentJsonRegionUtils.h"
#include "AssetDocumentProfile.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
TSharedPtr<FJsonObject> ParseJsonObject(const FString& Json)
{
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	FJsonSerializer::Deserialize(Reader, Object);
	return Object;
}

TSharedPtr<FJsonObject> MakeValidEventGraph()
{
	return ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    {
      "Id": "BeginPlay",
      "Class": "/Script/BlueprintGraph.K2Node_Event",
      "Member": {
        "Kind": "MemberRef",
        "OwnerClass": "/Script/Engine.Actor",
        "Name": "ReceiveBeginPlay"
      },
      "Position": { "X": 0, "Y": 0 }
    }
  ],
  "Links": []
}
)JSON"));
}

FAssetDocumentGraphParseResult ParseGraphs(TArray<TSharedPtr<FJsonValue>> Values)
{
	FAssetDocumentGraphParseOptions Options;
	Options.Path = TEXT("/Body/UbergraphPages");
	return FAssetDocumentGraphParser::ParseGraphArray(Values, Options);
}

bool GraphCoreHasDiagnosticCode(const FAssetDocumentGraphParseResult& Result, const FString& Code)
{
	return Result.Diagnostics.ContainsByPredicate(
		[&Code](const FAssetDocumentGraphDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code;
		});
}

bool HasDiagnosticCodeAtPath(const FAssetDocumentGraphParseResult& Result, const FString& Code, const FString& Path)
{
	return Result.Diagnostics.ContainsByPredicate(
		[&Code, &Path](const FAssetDocumentGraphDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code && Diagnostic.Path == Path;
		});
}

bool GraphCoreHasDiagnosticCode(const FAssetDocumentGraphDefinitionResolveResult& Result, const FString& Code)
{
	return Result.Diagnostics.ContainsByPredicate(
		[&Code](const FAssetDocumentGraphDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code;
		});
}

bool HasDiagnosticCodeAtPath(const FAssetDocumentGraphDefinitionResolveResult& Result, const FString& Code, const FString& Path)
{
	return Result.Diagnostics.ContainsByPredicate(
		[&Code, &Path](const FAssetDocumentGraphDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code && Diagnostic.Path == Path;
		});
}

TSharedPtr<FJsonObject> MakeDefinitions()
{
	return ParseJsonObject(TEXT(R"JSON(
{
  "Func.KismetSystemLibrary.PrintString": {
    "Kind": "MemberRef",
    "OwnerClass": "/Script/Engine.KismetSystemLibrary",
    "Name": "PrintString"
  }
}
)JSON"));
}

FAssetDocumentGraphDefinitionResolveResult ResolveGraphs(
	const TArray<FAssetDocumentGraphSpec>& Graphs,
	const TSharedPtr<FJsonObject>& Definitions)
{
	FAssetDocumentGraphDefinitionResolveOptions Options;
	Options.DefinitionsPath = TEXT("/Definitions");
	Options.GraphsPath = TEXT("/Body/UbergraphPages");
	return FAssetDocumentGraphDefinitionResolver::ResolveGraphArray(Graphs, Definitions, Options);
}

bool HasDiffStatusAtPath(
	const TArray<FAssetDocumentGraphDiffEntry>& Entries,
	const FString& Path,
	const FString& Status)
{
	return Entries.ContainsByPredicate(
		[&Path, &Status](const FAssetDocumentGraphDiffEntry& Entry)
		{
			return Entry.Path == Path && Entry.Status == Status;
		});
}

TSharedPtr<FJsonObject> MakeRecursiveGraphRegion()
{
	return ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "Locomotion",
      "Kind": "StateMachine",
      "Owner": { "StateMachine": "Locomotion" },
      "OwnerNodeId": "StateMachineNode",
      "OwnerPin": "OutputPose",
      "EntryPins": ["Entry"],
      "ResultPins": ["Result"],
      "Position": { "X": 10, "Y": 20 },
      "Metadata": { "EditorCategory": "Movement" },
      "Evidence": { "GraphGuid": "root-guid" },
      "Nodes": [
        {
          "Id": "IdleState",
          "Class": "/Script/AnimGraph.AnimStateNode",
          "Kind": "State",
          "Spawner": { "ActionKey": "AnimState" },
          "Fields": { "DisplayName": "Idle" },
          "Pins": { "Pose": { "Direction": "Output" } },
          "Position": { "X": 120, "Y": 80 },
          "Evidence": { "NodeGuid": "idle-node-guid" }
        }
      ],
      "Links": [],
      "Subgraphs": [
        {
          "Id": "IdleToRunRule",
          "Kind": "TransitionRule",
          "Owner": { "StateMachine": "Locomotion", "ParentNode": "IdleState" },
          "Nodes": [
            {
              "Id": "SpeedCheck",
              "Class": "/Script/BlueprintGraph.K2Node_VariableGet",
              "Fields": { "Variable": "Speed" },
              "Position": { "X": 320, "Y": 160 }
            }
          ],
          "Links": [],
          "Subgraphs": []
        }
      ]
    }
  ]
}
)JSON"));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreParseValidEventGraphTest,
	"UEAssetDoc.GraphCore.ParseValidEventGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreParseValidEventGraphTest::RunTest(const FString& Parameters)
{
	const FAssetDocumentGraphParseResult Result = ParseGraphs({
		MakeShared<FJsonValueObject>(MakeValidEventGraph().ToSharedRef())
	});

	TestTrue(TEXT("Valid graph parses"), Result.IsValid());
	TestEqual(TEXT("One graph parsed"), Result.Graphs.Num(), 1);
	if (Result.Graphs.Num() == 1)
	{
		TestEqual(TEXT("Graph name is preserved"), Result.Graphs[0].Name, FString(TEXT("EventGraph")));
		TestEqual(TEXT("Node id is preserved"), Result.Graphs[0].Nodes[0].Id, FString(TEXT("BeginPlay")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreFullAuthoredNodeSurfaceRoundTripTest,
	"UEAssetDoc.GraphCore.FullAuthoredNodeSurfaceRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreFullAuthoredNodeSurfaceRoundTripTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Graph = MakeValidEventGraph();
	const TSharedPtr<FJsonObject> Node = Graph->GetArrayField(TEXT("Nodes"))[0]->AsObject();
	Node->SetStringField(TEXT("NodeGuid"), TEXT("11111111111111111111111111111111"));
	Node->SetObjectField(
		TEXT("Position"),
		ParseJsonObject(TEXT(R"JSON({ "X": 120, "Y": 80, "Width": 320, "Height": 180 })JSON")));
	Node->SetStringField(TEXT("Comment"), TEXT(""));
	Node->SetBoolField(TEXT("CommentBubblePinned"), true);
	Node->SetBoolField(TEXT("CommentBubbleVisible"), false);
	Node->SetStringField(TEXT("AdvancedPinDisplay"), TEXT("Hidden"));
	Node->SetStringField(TEXT("EnabledState"), TEXT("Disabled"));

	const FAssetDocumentGraphParseResult Parsed = ParseGraphs({
		MakeShared<FJsonValueObject>(Graph.ToSharedRef())
	});
	TestTrue(TEXT("Complete authored node surface parses"), Parsed.IsValid());
	if (Parsed.Graphs.Num() != 1)
	{
		return false;
	}

	const TSharedRef<FJsonValue> CanonicalValue =
		FAssetDocumentGraphParser::WriteCanonicalGraphArray(Parsed.Graphs);
	const TArray<TSharedPtr<FJsonValue>>& CanonicalGraphs = CanonicalValue->AsArray();
	if (CanonicalGraphs.Num() != 1)
	{
		AddError(TEXT("Complete authored node surface did not write one canonical graph"));
		return false;
	}
	const TSharedPtr<FJsonObject> CanonicalNode =
		CanonicalGraphs[0]->AsObject()->GetArrayField(TEXT("Nodes"))[0]->AsObject();
	TestEqual(TEXT("Node guid roundtrips"), CanonicalNode->GetStringField(TEXT("NodeGuid")), FString(TEXT("11111111111111111111111111111111")));
	TestTrue(TEXT("Empty authored comment remains explicit"), CanonicalNode->HasTypedField<EJson::String>(TEXT("Comment")));
	TestEqual(TEXT("Empty authored comment roundtrips"), CanonicalNode->GetStringField(TEXT("Comment")), FString());
	bool bCommentBubblePinned = false;
	bool bCommentBubbleVisible = true;
	FString AdvancedPinDisplay;
	FString EnabledState;
	TestTrue(
		TEXT("Pinned comment bubble field is present"),
		CanonicalNode->TryGetBoolField(TEXT("CommentBubblePinned"), bCommentBubblePinned));
	TestTrue(TEXT("Pinned comment bubble roundtrips"), bCommentBubblePinned);
	TestTrue(
		TEXT("Visible comment bubble field is present"),
		CanonicalNode->TryGetBoolField(TEXT("CommentBubbleVisible"), bCommentBubbleVisible));
	TestFalse(TEXT("Hidden comment bubble roundtrips"), bCommentBubbleVisible);
	TestTrue(
		TEXT("Advanced-pin display field is present"),
		CanonicalNode->TryGetStringField(TEXT("AdvancedPinDisplay"), AdvancedPinDisplay));
	TestEqual(TEXT("Advanced-pin display roundtrips"), AdvancedPinDisplay, FString(TEXT("Hidden")));
	TestTrue(
		TEXT("Enabled-state field is present"),
		CanonicalNode->TryGetStringField(TEXT("EnabledState"), EnabledState));
	TestEqual(TEXT("Enabled state roundtrips"), EnabledState, FString(TEXT("Disabled")));
	const TSharedPtr<FJsonObject> CanonicalPosition = CanonicalNode->GetObjectField(TEXT("Position"));
	TestEqual(TEXT("Node width roundtrips"), CanonicalPosition->GetNumberField(TEXT("Width")), 320.0);
	TestEqual(TEXT("Node height roundtrips"), CanonicalPosition->GetNumberField(TEXT("Height")), 180.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCorePinOverridesAndSplitStateRoundTripTest,
	"UEAssetDoc.GraphCore.PinOverridesAndSplitStateRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCorePinOverridesAndSplitStateRoundTripTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Graph = MakeValidEventGraph();
	const TSharedPtr<FJsonObject> Node = Graph->GetArrayField(TEXT("Nodes"))[0]->AsObject();
	Node->SetArrayField(
		TEXT("PinOverrides"),
		{
			MakeShared<FJsonValueObject>(ParseJsonObject(TEXT(R"JSON(
{
  "Pin": "Location",
  "Direction": "Input",
  "PersistentGuid": "22222222222222222222222222222222",
  "SplitState": "Split",
  "SubPins": ["Location_Z", "Location_X", "Location_Y"]
}
)JSON")).ToSharedRef()),
			MakeShared<FJsonValueObject>(ParseJsonObject(TEXT(R"JSON(
{
  "Pin": "Location_X",
  "Direction": "Input",
  "PersistentGuid": "33333333333333333333333333333333",
  "ParentPin": "Location"
}
)JSON")).ToSharedRef()),
			MakeShared<FJsonValueObject>(ParseJsonObject(TEXT(R"JSON(
{
  "Pin": "Location_Y",
  "Direction": "Input",
  "PersistentGuid": "44444444444444444444444444444444",
  "ParentPin": "Location"
}
)JSON")).ToSharedRef()),
			MakeShared<FJsonValueObject>(ParseJsonObject(TEXT(R"JSON(
{
  "Pin": "Location_Z",
  "Direction": "Input",
  "PersistentGuid": "55555555555555555555555555555555",
  "ParentPin": "Location"
}
)JSON")).ToSharedRef())
		});

	const FAssetDocumentGraphParseResult Parsed = ParseGraphs({
		MakeShared<FJsonValueObject>(Graph.ToSharedRef())
	});
	TestTrue(TEXT("Pin overrides with split state parse"), Parsed.IsValid());
	if (Parsed.Graphs.Num() != 1)
	{
		return false;
	}

	const TSharedRef<FJsonValue> CanonicalValue =
		FAssetDocumentGraphParser::WriteCanonicalGraphArray(Parsed.Graphs);
	const TSharedPtr<FJsonObject> CanonicalNode =
		CanonicalValue->AsArray()[0]->AsObject()->GetArrayField(TEXT("Nodes"))[0]->AsObject();
	const TArray<TSharedPtr<FJsonValue>>& PinOverrides = CanonicalNode->GetArrayField(TEXT("PinOverrides"));
	const TSharedPtr<FJsonValue>* ParentOverride = PinOverrides.FindByPredicate(
		[](const TSharedPtr<FJsonValue>& Value)
		{
			return Value.IsValid()
				&& Value->Type == EJson::Object
				&& Value->AsObject()->GetStringField(TEXT("Pin")) == TEXT("Location");
		});
	TestNotNull(TEXT("Canonical split parent pin is present"), ParentOverride);
	if (ParentOverride)
	{
		const TSharedPtr<FJsonObject> Parent = (*ParentOverride)->AsObject();
		FString SplitState;
		FString PersistentGuid;
		TestTrue(
			TEXT("Split-state field is present"),
			Parent->TryGetStringField(TEXT("SplitState"), SplitState));
		TestEqual(TEXT("Split state roundtrips"), SplitState, FString(TEXT("Split")));
		TestTrue(
			TEXT("Persistent-guid field is present"),
			Parent->TryGetStringField(TEXT("PersistentGuid"), PersistentGuid));
		TestEqual(TEXT("Persistent pin guid roundtrips"), PersistentGuid, FString(TEXT("22222222222222222222222222222222")));
		const TArray<TSharedPtr<FJsonValue>>* SubPins = nullptr;
		TestTrue(TEXT("Split sub-pins field is present"), Parent->TryGetArrayField(TEXT("SubPins"), SubPins));
		TestEqual(TEXT("All split sub-pins roundtrip"), SubPins ? SubPins->Num() : 0, 3);
		if (SubPins && SubPins->Num() == 3)
		{
			TestEqual(TEXT("Split sub-pin order is semantic"), (*SubPins)[0]->AsString(), FString(TEXT("Location_Z")));
			TestEqual(TEXT("Split sub-pin order keeps second entry"), (*SubPins)[1]->AsString(), FString(TEXT("Location_X")));
			TestEqual(TEXT("Split sub-pin order keeps third entry"), (*SubPins)[2]->AsString(), FString(TEXT("Location_Y")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreSameNameOppositeDirectionPinOverridesRoundTripAndDiffTest,
	"UEAssetDoc.GraphCore.SameNameOppositeDirectionPinOverridesRoundTripAndDiff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreSameNameOppositeDirectionPinOverridesRoundTripAndDiffTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> DesiredGraph = MakeValidEventGraph();
	const TSharedPtr<FJsonObject> DesiredNode = DesiredGraph->GetArrayField(TEXT("Nodes"))[0]->AsObject();
	DesiredNode->SetArrayField(
		TEXT("PinOverrides"),
		{
			MakeShared<FJsonValueObject>(ParseJsonObject(TEXT(R"JSON(
{ "Pin": "Pose", "Direction": "Input", "DefaultValue": "InputValue" }
)JSON")).ToSharedRef()),
			MakeShared<FJsonValueObject>(ParseJsonObject(TEXT(R"JSON(
{ "Pin": "Pose", "Direction": "Output", "DefaultValue": "OutputValue" }
)JSON")).ToSharedRef())
		});
	const TSharedPtr<FJsonObject> CurrentGraph =
		FAssetDocumentJsonRegionUtils::DeepCloneJsonObject(DesiredGraph);
	CurrentGraph->GetArrayField(TEXT("Nodes"))[0]
		->AsObject()->GetArrayField(TEXT("PinOverrides"))[1]
		->AsObject()->SetStringField(TEXT("DefaultValue"), TEXT("ChangedOutput"));

	const FAssetDocumentGraphParseResult Desired = ParseGraphs({
		MakeShared<FJsonValueObject>(DesiredGraph.ToSharedRef())
	});
	const FAssetDocumentGraphParseResult Current = ParseGraphs({
		MakeShared<FJsonValueObject>(CurrentGraph.ToSharedRef())
	});
	TestTrue(TEXT("Same-name input/output pin overrides parse independently"), Desired.IsValid());
	TestTrue(TEXT("Current same-name input/output pin overrides parse independently"), Current.IsValid());
	if (!Desired.IsValid() || !Current.IsValid())
	{
		return false;
	}

	const TSharedRef<FJsonValue> Canonical =
		FAssetDocumentGraphParser::WriteCanonicalGraphArray(Desired.Graphs);
	const TArray<TSharedPtr<FJsonValue>>& PinOverrides = Canonical->AsArray()[0]
		->AsObject()->GetArrayField(TEXT("Nodes"))[0]
		->AsObject()->GetArrayField(TEXT("PinOverrides"));
	TestEqual(TEXT("Both directional pin identities roundtrip"), PinOverrides.Num(), 2);

	const TArray<FAssetDocumentGraphDiffEntry> Diff =
		FAssetDocumentGraphDiff::CompareUbergraphPages(Desired.Graphs, Current.Graphs, nullptr);
	TestTrue(
		TEXT("Diff preserves the changed output-side identity"),
		HasDiffStatusAtPath(
			Diff,
			TEXT("/Body/UbergraphPages/EventGraph/Nodes/BeginPlay/PinOverrides/Output:Pose"),
			TEXT("changed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectMixedDirectionlessAndDirectedPinOverridesTest,
	"UEAssetDoc.GraphCore.RejectMixedDirectionlessAndDirectedPinOverrides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectMixedDirectionlessAndDirectedPinOverridesTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Graph = MakeValidEventGraph();
	Graph->GetArrayField(TEXT("Nodes"))[0]->AsObject()->SetArrayField(
		TEXT("PinOverrides"),
		{
			MakeShared<FJsonValueObject>(ParseJsonObject(TEXT(R"JSON(
{ "Pin": "Pose", "DefaultValue": "Legacy" }
)JSON")).ToSharedRef()),
			MakeShared<FJsonValueObject>(ParseJsonObject(TEXT(R"JSON(
{ "Pin": "Pose", "Direction": "Input", "DefaultValue": "Directed" }
)JSON")).ToSharedRef())
		});

	const FAssetDocumentGraphParseResult Result = ParseGraphs({
		MakeShared<FJsonValueObject>(Graph.ToSharedRef())
	});
	TestFalse(
		TEXT("A legacy directionless override cannot overlap a directed override for the same runtime pin"),
		Result.IsValid());
	TestTrue(
		TEXT("Mixed identity reports the second authored pin at its exact path"),
		HasDiagnosticCodeAtPath(
			Result,
			TEXT("DuplicateGraphPinId"),
			TEXT("/Body/UbergraphPages/0/Nodes/0/PinOverrides/1/Pin")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreWrongTypePersistentGuidsRejectedTest,
	"UEAssetDoc.GraphCore.WrongTypePersistentGuidsRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreWrongTypePersistentGuidsRejectedTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> WrongGraphGuid = MakeValidEventGraph();
	WrongGraphGuid->SetNumberField(TEXT("GraphGuid"), 42.0);
	const FAssetDocumentGraphParseResult GraphResult = ParseGraphs({
		MakeShared<FJsonValueObject>(WrongGraphGuid.ToSharedRef())
	});
	TestFalse(TEXT("Wrong-type GraphGuid is not normalized to missing"), GraphResult.IsValid());
	TestTrue(
		TEXT("Wrong-type GraphGuid reports its syntax location"),
		HasDiagnosticCodeAtPath(
			GraphResult,
			TEXT("InvalidGraphGuid"),
			TEXT("/Body/UbergraphPages/0/GraphGuid")));

	const TSharedPtr<FJsonObject> WrongNodeGuid = MakeValidEventGraph();
	WrongNodeGuid->GetArrayField(TEXT("Nodes"))[0]->AsObject()->SetObjectField(
		TEXT("NodeGuid"),
		MakeShared<FJsonObject>());
	const FAssetDocumentGraphParseResult NodeResult = ParseGraphs({
		MakeShared<FJsonValueObject>(WrongNodeGuid.ToSharedRef())
	});
	TestFalse(TEXT("Wrong-type NodeGuid is not normalized to missing"), NodeResult.IsValid());
	TestTrue(
		TEXT("Wrong-type NodeGuid reports its syntax location"),
		HasDiagnosticCodeAtPath(
			NodeResult,
			TEXT("InvalidGraphNodeGuid"),
			TEXT("/Body/UbergraphPages/0/Nodes/0/NodeGuid")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreWrongTypeAuthoredStringsRejectedTest,
	"UEAssetDoc.GraphCore.WrongTypeAuthoredStringsRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreWrongTypeAuthoredStringsRejectedTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> WrongComment = MakeValidEventGraph();
	WrongComment->GetArrayField(TEXT("Nodes"))[0]->AsObject()->SetBoolField(TEXT("Comment"), true);
	const FAssetDocumentGraphParseResult CommentResult = ParseGraphs({
		MakeShared<FJsonValueObject>(WrongComment.ToSharedRef())
	});
	TestFalse(TEXT("Wrong-type Comment is not canonicalized to an explicit empty string"), CommentResult.IsValid());
	TestTrue(
		TEXT("Wrong-type Comment reports its exact authored field"),
		HasDiagnosticCodeAtPath(
			CommentResult,
			TEXT("InvalidGraphNodeComment"),
			TEXT("/Body/UbergraphPages/0/Nodes/0/Comment")));

	const TSharedPtr<FJsonObject> WrongNameRegion = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "AnimGraph",
      "Kind": "AnimGraph",
      "Name": 42,
      "Nodes": [],
      "Links": [],
      "Subgraphs": []
    }
  ]
}
)JSON"));
	FAssetDocumentGraphParseOptions Options;
	Options.Path = TEXT("/Body/AnimGraph");
	const FAssetDocumentGraphParseResult NameResult =
		FAssetDocumentGraphParser::ParseGraphRegion(WrongNameRegion.ToSharedRef(), Options);
	TestFalse(TEXT("Wrong-type recursive graph Name is not normalized to Id"), NameResult.IsValid());
	TestTrue(
		TEXT("Wrong-type graph Name reports its exact authored field"),
		HasDiagnosticCodeAtPath(
			NameResult,
			TEXT("InvalidGraphName"),
			TEXT("/Body/AnimGraph/Graphs/0/Name")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreLinkOrderIsCanonicalTest,
	"UEAssetDoc.GraphCore.LinkOrderIsCanonical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreLinkOrderIsCanonicalTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> DesiredGraph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    { "Id": "Source", "Class": "/Script/BlueprintGraph.K2Node_Self" },
    { "Id": "Middle", "Class": "/Script/BlueprintGraph.K2Node_Self" },
    { "Id": "End", "Class": "/Script/BlueprintGraph.K2Node_Self" }
  ],
  "Links": [
    { "From": "Source.Out", "To": "Middle.In" },
    { "From": "Source.Out", "To": "End.In" }
  ]
}
)JSON"));
	const TSharedPtr<FJsonObject> CurrentGraph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    { "Id": "Source", "Class": "/Script/BlueprintGraph.K2Node_Self" },
    { "Id": "Middle", "Class": "/Script/BlueprintGraph.K2Node_Self" },
    { "Id": "End", "Class": "/Script/BlueprintGraph.K2Node_Self" }
  ],
  "Links": [
    { "From": "Source.Out", "To": "End.In" },
    { "From": "Source.Out", "To": "Middle.In" }
  ]
}
)JSON"));
	const FAssetDocumentGraphParseResult Desired = ParseGraphs({
		MakeShared<FJsonValueObject>(DesiredGraph.ToSharedRef())
	});
	const FAssetDocumentGraphParseResult Current = ParseGraphs({
		MakeShared<FJsonValueObject>(CurrentGraph.ToSharedRef())
	});
	TestTrue(TEXT("Desired ordered-link graph parses"), Desired.IsValid());
	TestTrue(TEXT("Current ordered-link graph parses"), Current.IsValid());
	if (!Desired.IsValid() || !Current.IsValid())
	{
		return false;
	}

	const TSharedRef<FJsonValue> Canonical =
		FAssetDocumentGraphParser::WriteCanonicalGraphArray(Desired.Graphs);
	const TSharedRef<FJsonValue> ReorderedCanonical =
		FAssetDocumentGraphParser::WriteCanonicalGraphArray(Current.Graphs);
	const TArray<TSharedPtr<FJsonValue>>& CanonicalLinks =
		Canonical->AsArray()[0]->AsObject()->GetArrayField(TEXT("Links"));
	TestEqual(
		TEXT("Canonical write sorts the first semantic link by ToKey"),
		CanonicalLinks[0]->AsObject()->GetObjectField(TEXT("To"))->GetStringField(TEXT("Node")),
		FString(TEXT("End")));
	TestEqual(
		TEXT("Canonical write sorts the second semantic link by ToKey"),
		CanonicalLinks[1]->AsObject()->GetObjectField(TEXT("To"))->GetStringField(TEXT("Node")),
		FString(TEXT("Middle")));
	TestTrue(
		TEXT("Two authored orders serialize to identical canonical links"),
		AssetDocumentGraphJson::AreJsonValuesEqual(Canonical, ReorderedCanonical));

	const TArray<FAssetDocumentGraphDiffEntry> Entries = FAssetDocumentGraphDiff::CompareUbergraphPages(
		Desired.Graphs,
		Current.Graphs,
		nullptr);
	TestFalse(
		TEXT("Authored link order produces no semantic graph diff"),
		Entries.ContainsByPredicate(
			[](const FAssetDocumentGraphDiffEntry& Entry)
			{
				return Entry.Status != TEXT("unchanged");
			}));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectInvalidGraphGuidAtSemanticPathTest,
	"UEAssetDoc.GraphCore.RejectInvalidGraphGuidAtSemanticPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectInvalidGraphGuidAtSemanticPathTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Region = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "Locomotion",
      "Kind": "StateMachine",
      "GraphGuid": "not-a-guid",
      "Nodes": [],
      "Links": [],
      "Subgraphs": []
    }
  ]
}
)JSON"));
	FAssetDocumentGraphParseOptions Options;
	Options.Path = TEXT("/Body/StateMachines");
	const FAssetDocumentGraphParseResult Result =
		FAssetDocumentGraphParser::ParseGraphRegion(Region.ToSharedRef(), Options);
	TestFalse(TEXT("Invalid graph GUID fails"), Result.IsValid());
	TestTrue(
		TEXT("Invalid graph GUID uses semantic graph identity path"),
		HasDiagnosticCodeAtPath(
			Result,
			TEXT("InvalidGraphGuid"),
			TEXT("/Body/StateMachines/Graphs/Locomotion/GraphGuid")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectDuplicateGraphGuidAtSemanticPathTest,
	"UEAssetDoc.GraphCore.RejectDuplicateGraphGuidAtSemanticPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectDuplicateGraphGuidAtSemanticPathTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Region = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "Locomotion",
      "Kind": "StateMachine",
      "GraphGuid": "11111111111111111111111111111111",
      "Nodes": [],
      "Links": [],
      "Subgraphs": []
    },
    {
      "Id": "Combat",
      "Kind": "StateMachine",
      "GraphGuid": "11111111111111111111111111111111",
      "Nodes": [],
      "Links": [],
      "Subgraphs": []
    }
  ]
}
)JSON"));
	FAssetDocumentGraphParseOptions Options;
	Options.Path = TEXT("/Body/StateMachines");
	const FAssetDocumentGraphParseResult Result =
		FAssetDocumentGraphParser::ParseGraphRegion(Region.ToSharedRef(), Options);
	TestFalse(TEXT("Duplicate graph GUID fails"), Result.IsValid());
	TestTrue(
		TEXT("Duplicate graph GUID uses semantic graph identity path"),
		HasDiagnosticCodeAtPath(
			Result,
			TEXT("DuplicateGraphGuid"),
			TEXT("/Body/StateMachines/Graphs/Combat/GraphGuid")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectDuplicateNodeGuidAtSemanticPathTest,
	"UEAssetDoc.GraphCore.RejectDuplicateNodeGuidAtSemanticPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectDuplicateNodeGuidAtSemanticPathTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Region = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "Locomotion",
      "Kind": "StateMachine",
      "Nodes": [
        {
          "Id": "Idle",
          "Class": "/Script/AnimGraph.AnimStateNode",
          "NodeGuid": "22222222222222222222222222222222"
        },
        {
          "Id": "Run",
          "Class": "/Script/AnimGraph.AnimStateNode",
          "NodeGuid": "22222222222222222222222222222222"
        }
      ],
      "Links": [],
      "Subgraphs": []
    }
  ]
}
)JSON"));
	FAssetDocumentGraphParseOptions Options;
	Options.Path = TEXT("/Body/StateMachines");
	const FAssetDocumentGraphParseResult Result =
		FAssetDocumentGraphParser::ParseGraphRegion(Region.ToSharedRef(), Options);
	TestFalse(TEXT("Duplicate node GUID fails"), Result.IsValid());
	TestTrue(
		TEXT("Duplicate node GUID uses semantic node identity path"),
		HasDiagnosticCodeAtPath(
			Result,
			TEXT("DuplicateGraphNodeGuid"),
			TEXT("/Body/StateMachines/Graphs/Locomotion/Nodes/Run/NodeGuid")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreAuthoritativeDeletionDiffTest,
	"UEAssetDoc.GraphCore.AuthoritativeDeletionDiff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreAuthoritativeDeletionDiffTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> DesiredRegion = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "Locomotion",
      "Kind": "StateMachine",
      "Nodes": [
        { "Id": "Keep", "Class": "/Script/AnimGraph.AnimStateNode" }
      ],
      "Links": [],
      "Subgraphs": []
    }
  ]
}
)JSON"));
	const TSharedPtr<FJsonObject> CurrentRegion = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "Locomotion",
      "Kind": "StateMachine",
      "Nodes": [
        { "Id": "Keep", "Class": "/Script/AnimGraph.AnimStateNode" },
        { "Id": "DeleteMe", "Class": "/Script/AnimGraph.AnimStateNode" }
      ],
      "Links": [
        { "From": "DeleteMe.Out", "To": "Keep.In" }
      ],
      "Subgraphs": [
        {
          "Id": "DeleteGraph",
          "Kind": "StatePose",
          "Nodes": [],
          "Links": [],
          "Subgraphs": []
        }
      ]
    }
  ]
}
)JSON"));
	FAssetDocumentGraphParseOptions Options;
	Options.Path = TEXT("/Body/StateMachines");
	const FAssetDocumentGraphParseResult Desired =
		FAssetDocumentGraphParser::ParseGraphRegion(DesiredRegion.ToSharedRef(), Options);
	const FAssetDocumentGraphParseResult Current =
		FAssetDocumentGraphParser::ParseGraphRegion(CurrentRegion.ToSharedRef(), Options);
	TestTrue(TEXT("Desired authoritative graph parses"), Desired.IsValid());
	TestTrue(TEXT("Current authoritative graph parses"), Current.IsValid());
	if (!Desired.IsValid() || !Current.IsValid())
	{
		return false;
	}

	const TArray<FAssetDocumentGraphDiffEntry> Entries = FAssetDocumentGraphDiff::CompareGraphRegion(
		Desired.Graphs,
		Current.Graphs,
		TEXT("/Body/StateMachines"));
	TestTrue(
		TEXT("Omitted managed node is authoritative extra"),
		HasDiffStatusAtPath(
			Entries,
			TEXT("/Body/StateMachines/Graphs/Locomotion/Nodes/DeleteMe"),
			TEXT("extra")));
	TestTrue(
		TEXT("Omitted managed link is authoritative extra"),
		HasDiffStatusAtPath(
			Entries,
			TEXT("/Body/StateMachines/Graphs/Locomotion/Links/DeleteMe:Out->Keep:In"),
			TEXT("extra")));
	TestTrue(
		TEXT("Omitted managed subgraph is authoritative extra"),
		HasDiffStatusAtPath(
			Entries,
			TEXT("/Body/StateMachines/Graphs/Locomotion/Subgraphs/DeleteGraph"),
			TEXT("extra")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreNoSkippedOrDiffAllowlistTest,
	"UEAssetDoc.GraphCore.NoSkippedOrDiffAllowlist",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreNoSkippedOrDiffAllowlistTest::RunTest(const FString& Parameters)
{
	FAssetDocumentGraphParseOptions Options;
	Options.Path = TEXT("/Body/AnimGraph");
	const TSharedPtr<FJsonObject> SkippedRegion = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "AnimGraph",
      "Kind": "AnimGraph",
      "_Skipped": { "Nodes": [] },
      "Nodes": [],
      "Links": [],
      "Subgraphs": []
    }
  ]
}
)JSON"));
	const FAssetDocumentGraphParseResult UnderscoreSkippedResult =
		FAssetDocumentGraphParser::ParseGraphRegion(SkippedRegion.ToSharedRef(), Options);
	TestFalse(TEXT("Authored _Skipped graph field is rejected"), UnderscoreSkippedResult.IsValid());
	TestTrue(
		TEXT("Authored _Skipped rejection points at the semantic field"),
		HasDiagnosticCodeAtPath(
			UnderscoreSkippedResult,
			TEXT("UnknownGraphField"),
			TEXT("/Body/AnimGraph/Graphs/0/_Skipped")));

	SkippedRegion->GetArrayField(TEXT("Graphs"))[0]->AsObject()->RemoveField(TEXT("_Skipped"));
	SkippedRegion->GetArrayField(TEXT("Graphs"))[0]->AsObject()->SetArrayField(TEXT("Skipped"), {});
	const FAssetDocumentGraphParseResult SkippedResult =
		FAssetDocumentGraphParser::ParseGraphRegion(SkippedRegion.ToSharedRef(), Options);
	TestFalse(TEXT("Authored Skipped graph field is rejected"), SkippedResult.IsValid());

	const TSharedPtr<FJsonObject> DesiredRegion = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "AnimGraph",
      "Kind": "AnimGraph",
      "Evidence": { "GraphPath": "DesiredProvenance" },
      "Nodes": [
        {
          "Id": "Node",
          "Class": "/Script/AnimGraph.AnimGraphNode_SequencePlayer",
          "Evidence": { "NodePath": "DesiredProvenance" }
        }
      ],
      "Links": [],
      "Subgraphs": []
    }
  ]
}
)JSON"));
	const TSharedPtr<FJsonObject> CurrentRegion = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "AnimGraph",
      "Kind": "AnimGraph",
      "Evidence": { "GraphPath": "CurrentProvenance" },
      "Nodes": [
        {
          "Id": "Node",
          "Class": "/Script/AnimGraph.AnimGraphNode_SequencePlayer",
          "Evidence": { "NodePath": "CurrentProvenance" }
        }
      ],
      "Links": [],
      "Subgraphs": []
    }
  ]
}
)JSON"));
	const FAssetDocumentGraphParseResult Desired =
		FAssetDocumentGraphParser::ParseGraphRegion(DesiredRegion.ToSharedRef(), Options);
	const FAssetDocumentGraphParseResult Current =
		FAssetDocumentGraphParser::ParseGraphRegion(CurrentRegion.ToSharedRef(), Options);
	TestTrue(TEXT("Extract-only desired evidence parses"), Desired.IsValid());
	TestTrue(TEXT("Extract-only current evidence parses"), Current.IsValid());
	const TArray<FAssetDocumentGraphDiffEntry> Entries = FAssetDocumentGraphDiff::CompareGraphRegion(
		Desired.Graphs,
		Current.Graphs,
		TEXT("/Body/AnimGraph"));
	TestTrue(
		TEXT("Extract-only provenance does not change the authored graph"),
		HasDiffStatusAtPath(
			Entries,
			TEXT("/Body/AnimGraph/Graphs/AnimGraph"),
			TEXT("unchanged")));
	TestFalse(
		TEXT("Graph evidence is not an authored diff allowlist or changed field"),
		HasDiffStatusAtPath(
			Entries,
			TEXT("/Body/AnimGraph/Graphs/AnimGraph/Evidence/GraphPath"),
			TEXT("changed")));
	TestFalse(
		TEXT("Node evidence is not an authored diff allowlist or changed field"),
		HasDiffStatusAtPath(
			Entries,
			TEXT("/Body/AnimGraph/Graphs/AnimGraph/Nodes/Node/Evidence/NodePath"),
			TEXT("changed")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreSubgraphOwnershipAndAliasRoundTripTest,
	"UEAssetDoc.GraphCore.SubgraphOwnershipAndAliasRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreSubgraphOwnershipAndAliasRoundTripTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Region = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "Locomotion",
      "Kind": "StateMachine",
      "Nodes": [
        {
          "Id": "Idle",
          "Class": "/Script/AnimGraph.AnimStateNode",
          "SubgraphRefs": {
            "StatePose": "IdlePose",
            "SharedPrimary": "SharedRule",
            "SharedAlias": "SharedRule"
          }
        }
      ],
      "Links": [],
      "Subgraphs": [
        {
          "Id": "IdlePose",
          "Kind": "StatePose",
          "Owner": { "ParentGraph": "Locomotion", "ParentNode": "Idle" },
          "Nodes": [],
          "Links": [],
          "Subgraphs": []
        },
        {
          "Id": "SharedRule",
          "Kind": "TransitionRule",
          "Owner": { "ParentGraph": "Locomotion", "ParentNode": "Idle" },
          "Nodes": [],
          "Links": [],
          "Subgraphs": []
        }
      ]
    }
  ]
}
)JSON"));
	FAssetDocumentGraphParseOptions Options;
	Options.Path = TEXT("/Body/StateMachines");
	const FAssetDocumentGraphParseResult Parsed =
		FAssetDocumentGraphParser::ParseGraphRegion(Region.ToSharedRef(), Options);
	TestTrue(TEXT("Owned subgraphs and same-owner aliases parse"), Parsed.IsValid());
	if (!Parsed.IsValid())
	{
		return false;
	}

	const TSharedRef<FJsonObject> Canonical =
		FAssetDocumentGraphParser::WriteCanonicalGraphRegion(Parsed.Graphs);
	const TSharedPtr<FJsonObject> Root = Canonical->GetArrayField(TEXT("Graphs"))[0]->AsObject();
	const TSharedPtr<FJsonObject> Node = Root->GetArrayField(TEXT("Nodes"))[0]->AsObject();
	const TSharedPtr<FJsonObject> Refs = Node->GetObjectField(TEXT("SubgraphRefs"));
	TestEqual(TEXT("Owned state-pose reference roundtrips"), Refs->GetStringField(TEXT("StatePose")), FString(TEXT("IdlePose")));
	TestEqual(TEXT("Primary shared graph reference roundtrips"), Refs->GetStringField(TEXT("SharedPrimary")), FString(TEXT("SharedRule")));
	TestEqual(TEXT("Same-owner shared graph alias roundtrips"), Refs->GetStringField(TEXT("SharedAlias")), FString(TEXT("SharedRule")));
	const TArray<TSharedPtr<FJsonValue>>& Subgraphs = Root->GetArrayField(TEXT("Subgraphs"));
	TestEqual(TEXT("Both owned subgraphs roundtrip"), Subgraphs.Num(), 2);
	for (const TSharedPtr<FJsonValue>& SubgraphValue : Subgraphs)
	{
		const TSharedPtr<FJsonObject> Owner = SubgraphValue->AsObject()->GetObjectField(TEXT("Owner"));
		TestEqual(TEXT("Owned subgraph parent graph roundtrips"), Owner->GetStringField(TEXT("ParentGraph")), FString(TEXT("Locomotion")));
		TestEqual(TEXT("Owned subgraph parent node roundtrips"), Owner->GetStringField(TEXT("ParentNode")), FString(TEXT("Idle")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectUnknownGraphOwnerTest,
	"UEAssetDoc.GraphCore.RejectUnknownGraphOwner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectUnknownGraphOwnerTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Region = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "Locomotion",
      "Kind": "StateMachine",
      "Nodes": [
        { "Id": "Idle", "Class": "/Script/AnimGraph.AnimStateNode" }
      ],
      "Links": [],
      "Subgraphs": [
        {
          "Id": "IdlePose",
          "Kind": "StatePose",
          "Owner": { "ParentGraph": "Locomotion", "ParentNode": "Missing" },
          "Nodes": [],
          "Links": [],
          "Subgraphs": []
        }
      ]
    }
  ]
}
)JSON"));
	FAssetDocumentGraphParseOptions Options;
	Options.Path = TEXT("/Body/StateMachines");
	const FAssetDocumentGraphParseResult Result =
		FAssetDocumentGraphParser::ParseGraphRegion(Region.ToSharedRef(), Options);
	TestFalse(TEXT("Unknown explicit parent node fails"), Result.IsValid());
	TestTrue(
		TEXT("Unknown parent node diagnostic uses semantic graph ownership path"),
		HasDiagnosticCodeAtPath(
			Result,
			TEXT("UnknownGraphOwnerReference"),
			TEXT("/Body/StateMachines/Graphs/Locomotion/Subgraphs/IdlePose/Owner/ParentNode")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectUnknownKindSpecificGraphOwnerTest,
	"UEAssetDoc.GraphCore.RejectUnknownKindSpecificGraphOwner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectUnknownKindSpecificGraphOwnerTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Region = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "Locomotion",
      "Kind": "StateMachine",
      "Nodes": [
        { "Id": "Idle", "Class": "/Script/AnimGraph.AnimStateNode" }
      ],
      "Links": [],
      "Subgraphs": [
        {
          "Id": "MissingPose",
          "Kind": "StatePose",
          "Owner": { "State": "Missing" },
          "Nodes": [],
          "Links": [],
          "Subgraphs": []
        }
      ]
    }
  ]
}
)JSON"));
	FAssetDocumentGraphParseOptions Options;
	Options.Path = TEXT("/Body/StateMachines");
	const FAssetDocumentGraphParseResult Result =
		FAssetDocumentGraphParser::ParseGraphRegion(Region.ToSharedRef(), Options);
	TestFalse(TEXT("Unknown kind-specific State owner fails"), Result.IsValid());
	TestTrue(
		TEXT("Kind-specific owner failure uses semantic Owner.State path"),
		HasDiagnosticCodeAtPath(
			Result,
			TEXT("UnknownGraphOwnerReference"),
			TEXT("/Body/StateMachines/Graphs/Locomotion/Subgraphs/MissingPose/Owner/State")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectInvalidLinkReferenceTest,
	"UEAssetDoc.GraphCore.RejectInvalidLinkReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectInvalidLinkReferenceTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Region = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "AnimGraph",
      "Kind": "AnimGraph",
      "Nodes": [
        { "Id": "Source", "Class": "/Script/AnimGraph.AnimGraphNode_SequencePlayer" }
      ],
      "Links": [
        { "From": "Source.Out", "To": "Missing.In" }
      ],
      "Subgraphs": []
    }
  ]
}
)JSON"));
	FAssetDocumentGraphParseOptions Options;
	Options.Path = TEXT("/Body/AnimGraph");
	const FAssetDocumentGraphParseResult Result =
		FAssetDocumentGraphParser::ParseGraphRegion(Region.ToSharedRef(), Options);
	TestFalse(TEXT("Link to unknown semantic node fails"), Result.IsValid());
	TestTrue(
		TEXT("Unknown link node uses link identity instead of array index"),
		HasDiagnosticCodeAtPath(
			Result,
			TEXT("UnknownGraphLinkNode"),
			TEXT("/Body/AnimGraph/Graphs/AnimGraph/Links/Source:Out->Missing:In/To/Node")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreSparsePinOverridesDoNotClosePinSurfaceTest,
	"UEAssetDoc.GraphCore.SparsePinOverridesDoNotClosePinSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreSparsePinOverridesDoNotClosePinSurfaceTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> SparseOverrideRegion = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "EventGraph",
      "Kind": "FunctionGraph",
      "Nodes": [
        {
          "Id": "BeginPlay",
          "Class": "/Script/BlueprintGraph.K2Node_Event",
          "PinOverrides": [
            { "Pin": "OutputDelegate", "DefaultValue": "" }
          ]
        },
        {
          "Id": "Print",
          "Class": "/Script/BlueprintGraph.K2Node_CallFunction",
          "PinOverrides": [
            { "Pin": "InString", "DefaultValue": "Hello" }
          ]
        }
      ],
      "Links": [
        { "From": "BeginPlay.then", "To": "Print.execute" }
      ],
      "Subgraphs": []
    }
  ]
}
)JSON"));
	FAssetDocumentGraphParseOptions Options;
	Options.Path = TEXT("/Body/UbergraphPages");
	const FAssetDocumentGraphParseResult SparseOverrideResult =
		FAssetDocumentGraphParser::ParseGraphRegion(SparseOverrideRegion.ToSharedRef(), Options);
	TestTrue(
		TEXT("Sparse default-value overrides do not declare a closed authored pin surface"),
		SparseOverrideResult.IsValid());

	const TSharedPtr<FJsonObject> ClosedPinSurfaceRegion = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "AnimGraph",
      "Kind": "AnimGraph",
      "Nodes": [
        {
          "Id": "Source",
          "Class": "/Script/AnimGraph.AnimGraphNode_SequencePlayer",
          "Pins": { "Pose": { "Direction": "Output" } }
        },
        {
          "Id": "Target",
          "Class": "/Script/AnimGraph.AnimGraphNode_Root",
          "Pins": { "Result": { "Direction": "Input" } }
        }
      ],
      "Links": [
        { "From": "Source.Unknown", "To": "Target.Result" }
      ],
      "Subgraphs": []
    }
  ]
}
)JSON"));
	Options.Path = TEXT("/Body/AnimGraph");
	const FAssetDocumentGraphParseResult ClosedPinSurfaceResult =
		FAssetDocumentGraphParser::ParseGraphRegion(ClosedPinSurfaceRegion.ToSharedRef(), Options);
	TestFalse(TEXT("Explicit Pins remains a closed authored pin surface"), ClosedPinSurfaceResult.IsValid());
	TestTrue(
		TEXT("Unknown pin on an explicit surface uses the semantic endpoint path"),
		HasDiagnosticCodeAtPath(
			ClosedPinSurfaceResult,
			TEXT("UnknownGraphLinkPin"),
			TEXT("/Body/AnimGraph/Graphs/AnimGraph/Links/Source:Unknown->Target:Result/From/Pin")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectConflictingGraphOwnershipTest,
	"UEAssetDoc.GraphCore.RejectConflictingGraphOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectConflictingGraphOwnershipTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Region = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "Locomotion",
      "Kind": "StateMachine",
      "Nodes": [
        {
          "Id": "Idle",
          "Class": "/Script/AnimGraph.AnimStateNode",
          "SubgraphRefs": { "StatePose": "SharedPose" }
        },
        {
          "Id": "Run",
          "Class": "/Script/AnimGraph.AnimStateNode",
          "SubgraphRefs": { "StatePose": "SharedPose" }
        }
      ],
      "Links": [],
      "Subgraphs": [
        {
          "Id": "SharedPose",
          "Kind": "StatePose",
          "Owner": { "ParentGraph": "Locomotion", "ParentNode": "Idle" },
          "Nodes": [],
          "Links": [],
          "Subgraphs": []
        }
      ]
    }
  ]
}
)JSON"));
	FAssetDocumentGraphParseOptions Options;
	Options.Path = TEXT("/Body/StateMachines");
	const FAssetDocumentGraphParseResult Result =
		FAssetDocumentGraphParser::ParseGraphRegion(Region.ToSharedRef(), Options);
	TestFalse(TEXT("Two different node owners cannot claim one owned subgraph"), Result.IsValid());
	TestTrue(
		TEXT("Conflicting ownership uses the second semantic claimant path"),
		HasDiagnosticCodeAtPath(
			Result,
			TEXT("ConflictingGraphOwnership"),
			TEXT("/Body/StateMachines/Graphs/Locomotion/Nodes/Run/SubgraphRefs/StatePose")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreGraphArrayPreservesSharedGraphFieldsTest,
	"UEAssetDoc.GraphCore.GraphArrayPreservesSharedGraphFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreGraphArrayPreservesSharedGraphFieldsTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Graph = MakeValidEventGraph();
	Graph->SetStringField(TEXT("OwnerNodeId"), TEXT("OwnerNode"));
	Graph->SetStringField(TEXT("OwnerPin"), TEXT("Output"));
	Graph->SetArrayField(TEXT("EntryPins"), { MakeShared<FJsonValueString>(TEXT("Entry")) });
	Graph->SetArrayField(TEXT("ResultPins"), { MakeShared<FJsonValueString>(TEXT("Result")) });
	Graph->SetObjectField(TEXT("Metadata"), ParseJsonObject(TEXT(R"JSON({ "Category": "Shared" })JSON")));

	const FAssetDocumentGraphParseResult Result = ParseGraphs({
		MakeShared<FJsonValueObject>(Graph.ToSharedRef())
	});

	TestTrue(TEXT("Graph array with shared graph fields parses"), Result.IsValid());
	TestEqual(TEXT("One graph parsed"), Result.Graphs.Num(), 1);
	if (Result.Graphs.Num() == 1)
	{
		const FAssetDocumentGraphSpec& ParsedGraph = Result.Graphs[0];
		TestEqual(TEXT("OwnerNodeId is preserved"), ParsedGraph.OwnerNodeId, FString(TEXT("OwnerNode")));
		TestEqual(TEXT("OwnerPin is preserved"), ParsedGraph.OwnerPin, FString(TEXT("Output")));
		TestTrue(TEXT("EntryPins is preserved"), ParsedGraph.EntryPins.IsValid());
		TestTrue(TEXT("ResultPins is preserved"), ParsedGraph.ResultPins.IsValid());
		TestTrue(TEXT("Metadata is preserved"), ParsedGraph.Metadata.IsValid());
	}

	const TSharedRef<FJsonValue> Canonical = FAssetDocumentGraphParser::WriteCanonicalGraphArray(Result.Graphs);
	const TSharedPtr<FJsonObject> CanonicalGraph = Canonical->AsArray()[0]->AsObject();
	TestEqual(TEXT("Canonical OwnerNodeId is written"), CanonicalGraph->GetStringField(TEXT("OwnerNodeId")), FString(TEXT("OwnerNode")));
	TestTrue(TEXT("Canonical EntryPins is written"), CanonicalGraph->HasTypedField<EJson::Array>(TEXT("EntryPins")));
	TestTrue(TEXT("Canonical Metadata is written"), CanonicalGraph->HasTypedField<EJson::Object>(TEXT("Metadata")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectDuplicateGraphNamesTest,
	"UEAssetDoc.GraphCore.RejectDuplicateGraphNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectDuplicateGraphNamesTest::RunTest(const FString& Parameters)
{
	const FAssetDocumentGraphParseResult Result = ParseGraphs({
		MakeShared<FJsonValueObject>(MakeValidEventGraph().ToSharedRef()),
		MakeShared<FJsonValueObject>(MakeValidEventGraph().ToSharedRef())
	});

	TestFalse(TEXT("Duplicate graph names fail"), Result.IsValid());
	TestTrue(TEXT("DuplicateGraphName diagnostic is emitted"), GraphCoreHasDiagnosticCode(Result, TEXT("DuplicateGraphName")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectDuplicateNodeIdsTest,
	"UEAssetDoc.GraphCore.RejectDuplicateNodeIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectDuplicateNodeIdsTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Graph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    { "Id": "BeginPlay", "Class": "/Script/BlueprintGraph.K2Node_Event" },
    { "Id": "BeginPlay", "Class": "/Script/BlueprintGraph.K2Node_Event" }
  ],
  "Links": []
}
)JSON"));

	const FAssetDocumentGraphParseResult Result = ParseGraphs({ MakeShared<FJsonValueObject>(Graph.ToSharedRef()) });

	TestFalse(TEXT("Duplicate node ids fail"), Result.IsValid());
	TestTrue(TEXT("DuplicateGraphNodeId diagnostic is emitted"), GraphCoreHasDiagnosticCode(Result, TEXT("DuplicateGraphNodeId")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectInvalidNodeIdTest,
	"UEAssetDoc.GraphCore.RejectInvalidNodeId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectInvalidNodeIdTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Graph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    { "Id": "1-BeginPlay", "Class": "/Script/BlueprintGraph.K2Node_Event" }
  ],
  "Links": []
}
)JSON"));

	const FAssetDocumentGraphParseResult Result = ParseGraphs({ MakeShared<FJsonValueObject>(Graph.ToSharedRef()) });

	TestFalse(TEXT("Invalid node id fails"), Result.IsValid());
	TestTrue(TEXT("InvalidGraphNodeId diagnostic is emitted"), GraphCoreHasDiagnosticCode(Result, TEXT("InvalidGraphNodeId")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectDuplicateLinksTest,
	"UEAssetDoc.GraphCore.RejectDuplicateLinks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectDuplicateLinksTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Graph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    { "Id": "BeginPlay", "Class": "/Script/BlueprintGraph.K2Node_Event" },
    { "Id": "Print", "Class": "/Script/BlueprintGraph.K2Node_CallFunction" }
  ],
  "Links": [
    { "From": { "Node": "BeginPlay", "Pin": "then" }, "To": { "Node": "Print", "Pin": "execute" } },
    { "From": { "Node": "BeginPlay", "Pin": "then" }, "To": { "Node": "Print", "Pin": "execute" } }
  ]
}
)JSON"));

	const FAssetDocumentGraphParseResult Result = ParseGraphs({ MakeShared<FJsonValueObject>(Graph.ToSharedRef()) });

	TestFalse(TEXT("Duplicate links fail"), Result.IsValid());
	TestTrue(TEXT("DuplicateGraphLink diagnostic is emitted"), GraphCoreHasDiagnosticCode(Result, TEXT("DuplicateGraphLink")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreAcceptCompactLinkInputAsSugarTest,
	"UEAssetDoc.GraphCore.AcceptCompactLinkInputAsSugar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreAcceptCompactLinkInputAsSugarTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Graph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    { "Id": "BeginPlay", "Class": "/Script/BlueprintGraph.K2Node_Event" },
    { "Id": "Print", "Class": "/Script/BlueprintGraph.K2Node_CallFunction" }
  ],
  "Links": [
    { "From": "BeginPlay.then", "To": "Print.execute" }
  ]
}
)JSON"));

	const FAssetDocumentGraphParseResult Result = ParseGraphs({ MakeShared<FJsonValueObject>(Graph.ToSharedRef()) });
	TestTrue(TEXT("Compact link parses"), Result.IsValid());
	TestEqual(TEXT("One link parsed"), Result.Graphs[0].Links.Num(), 1);
	TestEqual(TEXT("Compact source node expands"), Result.Graphs[0].Links[0].From.Node, FString(TEXT("BeginPlay")));
	TestEqual(TEXT("Compact target pin expands"), Result.Graphs[0].Links[0].To.Pin, FString(TEXT("execute")));

	const TSharedRef<FJsonValue> Canonical = FAssetDocumentGraphParser::WriteCanonicalGraphArray(Result.Graphs);
	const TArray<TSharedPtr<FJsonValue>>& CanonicalGraphs = Canonical->AsArray();
	const TArray<TSharedPtr<FJsonValue>>& CanonicalLinks =
		CanonicalGraphs[0]->AsObject()->GetArrayField(TEXT("Links"));
	TestTrue(TEXT("Canonical link From is expanded object"), CanonicalLinks[0]->AsObject()->GetField<EJson::Object>(TEXT("From")).IsValid());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectInvalidLinkEndpointIdsTest,
	"UEAssetDoc.GraphCore.RejectInvalidLinkEndpointIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectInvalidLinkEndpointIdsTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> ExpandedInvalidNodeGraph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    { "Id": "BeginPlay", "Class": "/Script/BlueprintGraph.K2Node_Event" },
    { "Id": "Print", "Class": "/Script/BlueprintGraph.K2Node_CallFunction" }
  ],
  "Links": [
    { "From": { "Node": "1BeginPlay", "Pin": "then" }, "To": { "Node": "Print", "Pin": "execute" } }
  ]
}
)JSON"));
	const FAssetDocumentGraphParseResult ExpandedInvalidNodeResult =
		ParseGraphs({ MakeShared<FJsonValueObject>(ExpandedInvalidNodeGraph.ToSharedRef()) });
	TestFalse(TEXT("Expanded endpoint with invalid node token fails"), ExpandedInvalidNodeResult.IsValid());
	TestTrue(
		TEXT("Expanded endpoint invalid node token uses InvalidGraphNodeId"),
		GraphCoreHasDiagnosticCode(ExpandedInvalidNodeResult, TEXT("InvalidGraphNodeId")));

	const TSharedPtr<FJsonObject> ExpandedInvalidPinGraph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    { "Id": "BeginPlay", "Class": "/Script/BlueprintGraph.K2Node_Event" },
    { "Id": "Print", "Class": "/Script/BlueprintGraph.K2Node_CallFunction" }
  ],
  "Links": [
    { "From": { "Node": "BeginPlay", "Pin": "1then" }, "To": { "Node": "Print", "Pin": "execute" } }
  ]
}
)JSON"));
	const FAssetDocumentGraphParseResult ExpandedInvalidPinResult =
		ParseGraphs({ MakeShared<FJsonValueObject>(ExpandedInvalidPinGraph.ToSharedRef()) });
	TestFalse(TEXT("Expanded endpoint with invalid pin token fails"), ExpandedInvalidPinResult.IsValid());
	TestTrue(
		TEXT("Expanded endpoint invalid pin token uses InvalidGraphPinId"),
		GraphCoreHasDiagnosticCode(ExpandedInvalidPinResult, TEXT("InvalidGraphPinId")));

	const TSharedPtr<FJsonObject> CompactInvalidNodeGraph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    { "Id": "BeginPlay", "Class": "/Script/BlueprintGraph.K2Node_Event" },
    { "Id": "Print", "Class": "/Script/BlueprintGraph.K2Node_CallFunction" }
  ],
  "Links": [
    { "From": "1BeginPlay.then", "To": "Print.execute" }
  ]
}
)JSON"));
	const FAssetDocumentGraphParseResult CompactInvalidNodeResult =
		ParseGraphs({ MakeShared<FJsonValueObject>(CompactInvalidNodeGraph.ToSharedRef()) });
	TestFalse(TEXT("Compact endpoint with invalid node token fails"), CompactInvalidNodeResult.IsValid());
	TestTrue(
		TEXT("Compact endpoint invalid node token uses InvalidGraphNodeId"),
		GraphCoreHasDiagnosticCode(CompactInvalidNodeResult, TEXT("InvalidGraphNodeId")));

	const TSharedPtr<FJsonObject> CompactInvalidPinGraph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    { "Id": "BeginPlay", "Class": "/Script/BlueprintGraph.K2Node_Event" },
    { "Id": "Print", "Class": "/Script/BlueprintGraph.K2Node_CallFunction" }
  ],
  "Links": [
    { "From": "BeginPlay.1then", "To": "Print.execute" }
  ]
}
)JSON"));
	const FAssetDocumentGraphParseResult CompactInvalidPinResult =
		ParseGraphs({ MakeShared<FJsonValueObject>(CompactInvalidPinGraph.ToSharedRef()) });
	TestFalse(TEXT("Compact endpoint with invalid pin token fails"), CompactInvalidPinResult.IsValid());
	TestTrue(
		TEXT("Compact endpoint invalid pin token uses InvalidGraphPinId"),
		GraphCoreHasDiagnosticCode(CompactInvalidPinResult, TEXT("InvalidGraphPinId")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectUnknownGraphFieldTest,
	"UEAssetDoc.GraphCore.RejectUnknownGraphField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectUnknownGraphFieldTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Graph = MakeValidEventGraph();
	Graph->SetStringField(TEXT("Unexpected"), TEXT("nope"));

	const FAssetDocumentGraphParseResult Result = ParseGraphs({ MakeShared<FJsonValueObject>(Graph.ToSharedRef()) });

	TestFalse(TEXT("Unknown graph fields fail"), Result.IsValid());
	TestTrue(TEXT("UnknownGraphField diagnostic is emitted"), GraphCoreHasDiagnosticCode(Result, TEXT("UnknownGraphField")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectInvalidNestedFieldTypesTest,
	"UEAssetDoc.GraphCore.RejectInvalidNestedFieldTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectInvalidNestedFieldTypesTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> InvalidSignatureGraph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Signature": "not-an-object",
  "Nodes": [],
  "Links": []
}
)JSON"));
	const FAssetDocumentGraphParseResult InvalidSignatureResult =
		ParseGraphs({ MakeShared<FJsonValueObject>(InvalidSignatureGraph.ToSharedRef()) });
	TestFalse(TEXT("Graph Signature with invalid type fails"), InvalidSignatureResult.IsValid());
	TestTrue(
		TEXT("InvalidGraphSignature diagnostic is emitted"),
		GraphCoreHasDiagnosticCode(InvalidSignatureResult, TEXT("InvalidGraphSignature")));

	const TSharedPtr<FJsonObject> InvalidMemberGraph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    {
      "Id": "BeginPlay",
      "Class": "/Script/BlueprintGraph.K2Node_Event",
      "Member": "not-an-object"
    }
  ],
  "Links": []
}
)JSON"));
	const FAssetDocumentGraphParseResult InvalidMemberResult =
		ParseGraphs({ MakeShared<FJsonValueObject>(InvalidMemberGraph.ToSharedRef()) });
	TestFalse(TEXT("Node Member with invalid type fails"), InvalidMemberResult.IsValid());
	TestTrue(
		TEXT("InvalidGraphMemberReference diagnostic is emitted"),
		GraphCoreHasDiagnosticCode(InvalidMemberResult, TEXT("InvalidGraphMemberReference")));

	const TSharedPtr<FJsonObject> InvalidPositionGraph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    {
      "Id": "BeginPlay",
      "Class": "/Script/BlueprintGraph.K2Node_Event",
      "Position": "not-an-object"
    }
  ],
  "Links": []
}
)JSON"));
	const FAssetDocumentGraphParseResult InvalidPositionResult =
		ParseGraphs({ MakeShared<FJsonValueObject>(InvalidPositionGraph.ToSharedRef()) });
	TestFalse(TEXT("Node Position with invalid type fails"), InvalidPositionResult.IsValid());
	TestTrue(
		TEXT("InvalidGraphPosition diagnostic is emitted"),
		GraphCoreHasDiagnosticCode(InvalidPositionResult, TEXT("InvalidGraphPosition")));

	const TSharedPtr<FJsonObject> InvalidPinOverridesGraph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    {
      "Id": "BeginPlay",
      "Class": "/Script/BlueprintGraph.K2Node_Event",
      "PinOverrides": "not-an-array"
    }
  ],
  "Links": []
}
)JSON"));
	const FAssetDocumentGraphParseResult InvalidPinOverridesResult =
		ParseGraphs({ MakeShared<FJsonValueObject>(InvalidPinOverridesGraph.ToSharedRef()) });
	TestFalse(TEXT("Node PinOverrides with invalid type fails"), InvalidPinOverridesResult.IsValid());
	TestTrue(
		TEXT("InvalidGraphPin diagnostic is emitted for invalid PinOverrides"),
		GraphCoreHasDiagnosticCode(InvalidPinOverridesResult, TEXT("InvalidGraphPin")));

	const TSharedPtr<FJsonObject> InvalidPinTypeGraph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    {
      "Id": "BeginPlay",
      "Class": "/Script/BlueprintGraph.K2Node_Event",
      "PinOverrides": [
        { "Pin": "then", "Type": "not-an-object" }
      ]
    }
  ],
  "Links": []
}
)JSON"));
	const FAssetDocumentGraphParseResult InvalidPinTypeResult =
		ParseGraphs({ MakeShared<FJsonValueObject>(InvalidPinTypeGraph.ToSharedRef()) });
	TestFalse(TEXT("Pin override Type with invalid type fails"), InvalidPinTypeResult.IsValid());
	TestTrue(
		TEXT("InvalidGraphPin diagnostic is emitted for invalid pin Type"),
		GraphCoreHasDiagnosticCode(InvalidPinTypeResult, TEXT("InvalidGraphPin")));

	const TSharedPtr<FJsonObject> InvalidPinDirectionGraph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    {
      "Id": "BeginPlay",
      "Class": "/Script/BlueprintGraph.K2Node_Event",
      "PinOverrides": [
        { "Pin": "then", "Direction": 42 }
      ]
    }
  ],
  "Links": []
}
)JSON"));
	const FAssetDocumentGraphParseResult InvalidPinDirectionResult =
		ParseGraphs({ MakeShared<FJsonValueObject>(InvalidPinDirectionGraph.ToSharedRef()) });
	TestFalse(TEXT("Pin override Direction with invalid type fails"), InvalidPinDirectionResult.IsValid());
	TestTrue(
		TEXT("InvalidGraphPin diagnostic is emitted for invalid pin Direction"),
		GraphCoreHasDiagnosticCode(InvalidPinDirectionResult, TEXT("InvalidGraphPin")));

	const TSharedPtr<FJsonObject> InvalidPinHiddenGraph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    {
      "Id": "BeginPlay",
      "Class": "/Script/BlueprintGraph.K2Node_Event",
      "PinOverrides": [
        { "Pin": "then", "Hidden": "yes" }
      ]
    }
  ],
  "Links": []
}
)JSON"));
	const FAssetDocumentGraphParseResult InvalidPinHiddenResult =
		ParseGraphs({ MakeShared<FJsonValueObject>(InvalidPinHiddenGraph.ToSharedRef()) });
	TestFalse(TEXT("Pin override Hidden with invalid type fails"), InvalidPinHiddenResult.IsValid());
	TestTrue(
		TEXT("InvalidGraphPin diagnostic is emitted for invalid pin Hidden"),
		GraphCoreHasDiagnosticCode(InvalidPinHiddenResult, TEXT("InvalidGraphPin")));

	const TSharedPtr<FJsonObject> InvalidPinAdvancedViewGraph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    {
      "Id": "BeginPlay",
      "Class": "/Script/BlueprintGraph.K2Node_Event",
      "PinOverrides": [
        { "Pin": "then", "AdvancedView": "yes" }
      ]
    }
  ],
  "Links": []
}
)JSON"));
	const FAssetDocumentGraphParseResult InvalidPinAdvancedViewResult =
		ParseGraphs({ MakeShared<FJsonValueObject>(InvalidPinAdvancedViewGraph.ToSharedRef()) });
	TestFalse(TEXT("Pin override AdvancedView with invalid type fails"), InvalidPinAdvancedViewResult.IsValid());
	TestTrue(
		TEXT("InvalidGraphPin diagnostic is emitted for invalid pin AdvancedView"),
		GraphCoreHasDiagnosticCode(InvalidPinAdvancedViewResult, TEXT("InvalidGraphPin")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectUnknownLinkFieldTest,
	"UEAssetDoc.GraphCore.RejectUnknownLinkField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectUnknownLinkFieldTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Graph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    { "Id": "BeginPlay", "Class": "/Script/BlueprintGraph.K2Node_Event" },
    { "Id": "Print", "Class": "/Script/BlueprintGraph.K2Node_CallFunction" }
  ],
  "Links": [
    {
      "From": { "Node": "BeginPlay", "Pin": "then" },
      "To": { "Node": "Print", "Pin": "execute" },
      "Comment": "canonical serializer must not drop this"
    }
  ]
}
)JSON"));

	const FAssetDocumentGraphParseResult Result = ParseGraphs({ MakeShared<FJsonValueObject>(Graph.ToSharedRef()) });

	TestFalse(TEXT("Unknown link fields fail"), Result.IsValid());
	TestTrue(TEXT("UnknownGraphLinkField diagnostic is emitted"), GraphCoreHasDiagnosticCode(Result, TEXT("UnknownGraphLinkField")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectInvalidLinkEndpointTypeTest,
	"UEAssetDoc.GraphCore.RejectInvalidLinkEndpointType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectInvalidLinkEndpointTypeTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Graph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    { "Id": "BeginPlay", "Class": "/Script/BlueprintGraph.K2Node_Event" },
    { "Id": "Print", "Class": "/Script/BlueprintGraph.K2Node_CallFunction" }
  ],
  "Links": [
    {
      "From": 42,
      "To": { "Node": "Print", "Pin": "execute" }
    }
  ]
}
)JSON"));

	const FAssetDocumentGraphParseResult Result = ParseGraphs({ MakeShared<FJsonValueObject>(Graph.ToSharedRef()) });

	TestFalse(TEXT("Invalid endpoint value type fails"), Result.IsValid());
	TestTrue(
		TEXT("InvalidGraphLinkEndpointSyntax diagnostic is emitted"),
		GraphCoreHasDiagnosticCode(Result, TEXT("InvalidGraphLinkEndpointSyntax")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectUnknownEndpointObjectFieldTest,
	"UEAssetDoc.GraphCore.RejectUnknownEndpointObjectField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectUnknownEndpointObjectFieldTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Graph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    { "Id": "BeginPlay", "Class": "/Script/BlueprintGraph.K2Node_Event" },
    { "Id": "Print", "Class": "/Script/BlueprintGraph.K2Node_CallFunction" }
  ],
  "Links": [
    {
      "From": { "Node": "BeginPlay", "Pin": "then", "Role": "source" },
      "To": { "Node": "Print", "Pin": "execute" }
    }
  ]
}
)JSON"));

	const FAssetDocumentGraphParseResult Result = ParseGraphs({ MakeShared<FJsonValueObject>(Graph.ToSharedRef()) });

	TestFalse(TEXT("Unknown endpoint object fields fail"), Result.IsValid());
	TestTrue(
		TEXT("UnknownGraphLinkEndpointField diagnostic is emitted"),
		GraphCoreHasDiagnosticCode(Result, TEXT("UnknownGraphLinkEndpointField")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreResolveDefinitionRefsTest,
	"UEAssetDoc.GraphCore.ResolveDefinitionRefs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreResolveDefinitionRefsTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Graph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    {
      "Id": "Print",
      "Class": "/Script/BlueprintGraph.K2Node_CallFunction",
      "Member": { "Kind": "DefinitionRef", "Id": "Func.KismetSystemLibrary.PrintString" }
    }
  ],
  "Links": []
}
)JSON"));
	const FAssetDocumentGraphParseResult ParseResult = ParseGraphs({ MakeShared<FJsonValueObject>(Graph.ToSharedRef()) });
	TestTrue(TEXT("Graph with DefinitionRef parses"), ParseResult.IsValid());

	const FAssetDocumentGraphDefinitionResolveResult ResolveResult = ResolveGraphs(ParseResult.Graphs, MakeDefinitions());
	TestTrue(TEXT("DefinitionRef resolves"), ResolveResult.IsValid());
	TestEqual(
		TEXT("Resolved member kind"),
		ResolveResult.Graphs[0].Nodes[0].Member->GetStringField(TEXT("Kind")),
		FString(TEXT("MemberRef")));
	TestEqual(
		TEXT("Resolved member name"),
		ResolveResult.Graphs[0].Nodes[0].Member->GetStringField(TEXT("Name")),
		FString(TEXT("PrintString")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectCircularDefinitionRefsTest,
	"UEAssetDoc.GraphCore.RejectCircularDefinitionRefs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectCircularDefinitionRefsTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Definitions = ParseJsonObject(TEXT(R"JSON(
{
  "A": { "Kind": "DefinitionRef", "Id": "B" },
  "B": { "Kind": "DefinitionRef", "Id": "A" }
}
)JSON"));
	const TSharedPtr<FJsonObject> Graph = MakeValidEventGraph();
	Graph->GetArrayField(TEXT("Nodes"))[0]->AsObject()->SetObjectField(
		TEXT("Member"),
		ParseJsonObject(TEXT(R"JSON({ "Kind": "DefinitionRef", "Id": "A" })JSON")));

	const FAssetDocumentGraphParseResult ParseResult = ParseGraphs({ MakeShared<FJsonValueObject>(Graph.ToSharedRef()) });
	const FAssetDocumentGraphDefinitionResolveResult ResolveResult = ResolveGraphs(ParseResult.Graphs, Definitions);

	TestFalse(TEXT("Circular DefinitionRefs fail"), ResolveResult.IsValid());
	TestTrue(
		TEXT("CircularDefinitionReference diagnostic is emitted"),
		GraphCoreHasDiagnosticCode(ResolveResult, TEXT("CircularDefinitionReference")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectUnresolvedDefinitionRefTest,
	"UEAssetDoc.GraphCore.RejectUnresolvedDefinitionRef",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectUnresolvedDefinitionRefTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Graph = MakeValidEventGraph();
	Graph->GetArrayField(TEXT("Nodes"))[0]->AsObject()->SetObjectField(
		TEXT("Member"),
		ParseJsonObject(TEXT(R"JSON({ "Kind": "DefinitionRef", "Id": "Func.Missing" })JSON")));

	const FAssetDocumentGraphParseResult ParseResult = ParseGraphs({ MakeShared<FJsonValueObject>(Graph.ToSharedRef()) });
	const FAssetDocumentGraphDefinitionResolveResult ResolveResult = ResolveGraphs(ParseResult.Graphs, MakeDefinitions());

	TestFalse(TEXT("Unresolved DefinitionRef fails"), ResolveResult.IsValid());
	TestTrue(
		TEXT("UnresolvedDefinitionReference diagnostic is emitted"),
		GraphCoreHasDiagnosticCode(ResolveResult, TEXT("UnresolvedDefinitionReference")));

	const TSharedPtr<FJsonObject> UnknownKindDefinitions = ParseJsonObject(TEXT(R"JSON(
{
  "Broken.Definition": { "Kind": "MysteryRef", "Value": "nope" }
}
)JSON"));
	const FAssetDocumentGraphDefinitionResolveResult UnknownKindResult =
		ResolveGraphs(ParseResult.Graphs, UnknownKindDefinitions);
	TestFalse(TEXT("Unknown definition kind fails"), UnknownKindResult.IsValid());
	TestTrue(
		TEXT("UnknownDefinitionKind diagnostic is emitted"),
		GraphCoreHasDiagnosticCode(UnknownKindResult, TEXT("UnknownDefinitionKind")));

	const TSharedPtr<FJsonObject> EscapedPathDefinitions = ParseJsonObject(TEXT(R"JSON(
{
  "Bad/Definition~Name": { "Kind": "MysteryRef", "Value": "nope" }
}
)JSON"));
	const FAssetDocumentGraphDefinitionResolveResult EscapedPathResult =
		ResolveGraphs(ParseResult.Graphs, EscapedPathDefinitions);
	TestFalse(TEXT("Definition path escaping case fails"), EscapedPathResult.IsValid());
	TestTrue(
		TEXT("Definition diagnostic path uses JSON Pointer escaping"),
		HasDiagnosticCodeAtPath(
			EscapedPathResult,
			TEXT("InvalidDefinitionId"),
			TEXT("/Definitions/Bad~1Definition~0Name")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreCompareInlineAndDefinitionRefAsEqualTest,
	"UEAssetDoc.GraphCore.CompareInlineAndDefinitionRefAsEqual",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreCompareInlineAndDefinitionRefAsEqualTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> InlineGraph = MakeValidEventGraph();
	InlineGraph->GetArrayField(TEXT("Nodes"))[0]->AsObject()->SetStringField(
		TEXT("Class"),
		TEXT("/Script/BlueprintGraph.K2Node_CallFunction"));
	InlineGraph->GetArrayField(TEXT("Nodes"))[0]->AsObject()->SetStringField(TEXT("Id"), TEXT("Print"));
	InlineGraph->GetArrayField(TEXT("Nodes"))[0]->AsObject()->SetObjectField(
		TEXT("Member"),
		MakeDefinitions()->GetObjectField(TEXT("Func.KismetSystemLibrary.PrintString")));

	const TSharedPtr<FJsonObject> RefGraph = MakeValidEventGraph();
	RefGraph->GetArrayField(TEXT("Nodes"))[0]->AsObject()->SetStringField(
		TEXT("Class"),
		TEXT("/Script/BlueprintGraph.K2Node_CallFunction"));
	RefGraph->GetArrayField(TEXT("Nodes"))[0]->AsObject()->SetStringField(TEXT("Id"), TEXT("Print"));
	RefGraph->GetArrayField(TEXT("Nodes"))[0]->AsObject()->SetObjectField(
		TEXT("Member"),
		ParseJsonObject(TEXT(R"JSON({ "Kind": "DefinitionRef", "Id": "Func.KismetSystemLibrary.PrintString" })JSON")));

	const FAssetDocumentGraphParseResult InlineParseResult =
		ParseGraphs({ MakeShared<FJsonValueObject>(InlineGraph.ToSharedRef()) });
	const FAssetDocumentGraphParseResult RefParseResult =
		ParseGraphs({ MakeShared<FJsonValueObject>(RefGraph.ToSharedRef()) });
	const TArray<FAssetDocumentGraphDiffEntry> Entries = FAssetDocumentGraphDiff::CompareUbergraphPages(
		RefParseResult.Graphs,
		InlineParseResult.Graphs,
		MakeDefinitions());

	TestTrue(
		TEXT("Inline MemberRef and equivalent DefinitionRef compare as unchanged"),
		HasDiffStatusAtPath(Entries, TEXT("/Body/UbergraphPages/EventGraph/Nodes/Print"), TEXT("unchanged")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreReportMissingExtraChangedGraphDiffsTest,
	"UEAssetDoc.GraphCore.ReportMissingExtraChangedGraphDiffs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreReportMissingExtraChangedGraphDiffsTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> DesiredGraph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    {
      "Id": "Print",
      "Class": "/Script/BlueprintGraph.K2Node_CallFunction",
      "Member": { "Kind": "DefinitionRef", "Id": "Func.KismetSystemLibrary.PrintString" },
      "PinOverrides": [
        { "Pin": "InString", "DefaultValue": "Desired" }
      ]
    },
    { "Id": "MissingInCurrent", "Class": "/Script/BlueprintGraph.K2Node_Self" }
  ],
  "Links": [
    { "From": "MissingInCurrent.self", "To": "Print.self" }
  ]
}
)JSON"));
	const TSharedPtr<FJsonObject> CurrentGraph = ParseJsonObject(TEXT(R"JSON(
{
  "Name": "EventGraph",
  "Schema": "/Script/BlueprintGraph.EdGraphSchema_K2",
  "Nodes": [
    {
      "Id": "Print",
      "Class": "/Script/BlueprintGraph.K2Node_CallFunction",
      "Member": {
        "Kind": "MemberRef",
        "OwnerClass": "/Script/Engine.KismetSystemLibrary",
        "Name": "PrintString"
      },
      "PinOverrides": [
        { "Pin": "InString", "DefaultValue": "Current" },
        { "Pin": "WorldContextObject", "DefaultValue": "Self" }
      ]
    },
    { "Id": "ExtraInCurrent", "Class": "/Script/BlueprintGraph.K2Node_Self" }
  ],
  "Links": [
    { "From": "ExtraInCurrent.self", "To": "Print.self" }
  ]
}
)JSON"));

	const FAssetDocumentGraphParseResult DesiredParseResult =
		ParseGraphs({ MakeShared<FJsonValueObject>(DesiredGraph.ToSharedRef()) });
	const FAssetDocumentGraphParseResult CurrentParseResult =
		ParseGraphs({ MakeShared<FJsonValueObject>(CurrentGraph.ToSharedRef()) });
	const TArray<FAssetDocumentGraphDiffEntry> Entries = FAssetDocumentGraphDiff::CompareUbergraphPages(
		DesiredParseResult.Graphs,
		CurrentParseResult.Graphs,
		MakeDefinitions());

	TestTrue(
		TEXT("Changed node is reported"),
		HasDiffStatusAtPath(Entries, TEXT("/Body/UbergraphPages/EventGraph/Nodes/Print"), TEXT("changed")));
	TestTrue(
		TEXT("Missing node is reported"),
		HasDiffStatusAtPath(Entries, TEXT("/Body/UbergraphPages/EventGraph/Nodes/MissingInCurrent"), TEXT("missing")));
	TestTrue(
		TEXT("Extra node is reported"),
		HasDiffStatusAtPath(Entries, TEXT("/Body/UbergraphPages/EventGraph/Nodes/ExtraInCurrent"), TEXT("extra")));
	TestTrue(
		TEXT("Changed pin is reported"),
		HasDiffStatusAtPath(
			Entries,
			TEXT("/Body/UbergraphPages/EventGraph/Nodes/Print/PinOverrides/InString"),
			TEXT("changed")));
	TestTrue(
		TEXT("Extra pin is reported"),
		HasDiffStatusAtPath(
			Entries,
			TEXT("/Body/UbergraphPages/EventGraph/Nodes/Print/PinOverrides/WorldContextObject"),
			TEXT("extra")));
	TestTrue(
		TEXT("Missing link is reported"),
		HasDiffStatusAtPath(
			Entries,
			TEXT("/Body/UbergraphPages/EventGraph/Links/MissingInCurrent:self->Print:self"),
			TEXT("missing")));
	TestTrue(
		TEXT("Extra link is reported"),
		HasDiffStatusAtPath(
			Entries,
			TEXT("/Body/UbergraphPages/EventGraph/Links/ExtraInCurrent:self->Print:self"),
			TEXT("extra")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRecursiveGraphParserTest,
	"UEAssetDoc.GraphCore.RecursiveGraphParser",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRecursiveGraphParserTest::RunTest(const FString& Parameters)
{
	FAssetDocumentGraphParseOptions Options;
	Options.Path = TEXT("/Body/StateMachines");
	const FAssetDocumentGraphParseResult Result =
		FAssetDocumentGraphParser::ParseGraphRegion(MakeRecursiveGraphRegion().ToSharedRef(), Options);

	TestTrue(TEXT("Recursive graph region parses"), Result.IsValid());
	TestEqual(TEXT("One root graph"), Result.Graphs.Num(), 1);
	if (Result.Graphs.Num() == 1)
	{
		const FAssetDocumentGraphSpec& RootGraph = Result.Graphs[0];
		TestEqual(TEXT("Root graph id"), RootGraph.Id, FString(TEXT("Locomotion")));
		TestEqual(TEXT("Root graph kind"), RootGraph.Kind, FString(TEXT("StateMachine")));
		TestTrue(TEXT("Root owner is preserved"), RootGraph.Owner.IsValid());
		TestEqual(TEXT("Owner node id is preserved"), RootGraph.OwnerNodeId, FString(TEXT("StateMachineNode")));
		TestEqual(TEXT("Owner pin is preserved"), RootGraph.OwnerPin, FString(TEXT("OutputPose")));
		TestTrue(TEXT("Entry pins are preserved"), RootGraph.EntryPins.IsValid());
		TestTrue(TEXT("Result pins are preserved"), RootGraph.ResultPins.IsValid());
		TestTrue(TEXT("Root position is preserved"), RootGraph.Position.IsValid());
		TestTrue(TEXT("Metadata is preserved"), RootGraph.Metadata.IsValid());
		TestTrue(TEXT("Root evidence is preserved"), RootGraph.Evidence.IsValid());
		TestEqual(TEXT("Root node count"), RootGraph.Nodes.Num(), 1);
		TestEqual(TEXT("Nested graph count"), RootGraph.Subgraphs.Num(), 1);

		if (RootGraph.Nodes.Num() == 1)
		{
			const FAssetDocumentNodeSpec& Node = RootGraph.Nodes[0];
			TestEqual(TEXT("Node kind is preserved"), Node.Kind, FString(TEXT("State")));
			TestTrue(TEXT("Node spawner is preserved"), Node.Spawner.IsValid());
			TestTrue(TEXT("Node fields are preserved"), Node.Fields.IsValid());
			TestTrue(TEXT("Node pins are preserved"), Node.Pins.IsValid());
			TestTrue(TEXT("Node evidence is preserved"), Node.Evidence.IsValid());
		}

		if (RootGraph.Subgraphs.Num() == 1)
		{
			TestEqual(TEXT("Nested graph id"), RootGraph.Subgraphs[0].Id, FString(TEXT("IdleToRunRule")));
			TestEqual(TEXT("Nested graph kind"), RootGraph.Subgraphs[0].Kind, FString(TEXT("TransitionRule")));
			TestEqual(TEXT("Nested graph node id"), RootGraph.Subgraphs[0].Nodes[0].Id, FString(TEXT("SpeedCheck")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreWriteCanonicalRecursiveGraphRegionTest,
	"UEAssetDoc.GraphCore.WriteCanonicalRecursiveGraphRegion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreWriteCanonicalRecursiveGraphRegionTest::RunTest(const FString& Parameters)
{
	FAssetDocumentGraphParseOptions Options;
	Options.Path = TEXT("/Body/StateMachines");
	const FAssetDocumentGraphParseResult Result =
		FAssetDocumentGraphParser::ParseGraphRegion(MakeRecursiveGraphRegion().ToSharedRef(), Options);
	TestTrue(TEXT("Recursive graph region parses before write"), Result.IsValid());

	const TSharedRef<FJsonObject> Canonical =
		FAssetDocumentGraphParser::WriteCanonicalGraphRegion(Result.Graphs);
	const TArray<TSharedPtr<FJsonValue>>& Graphs = Canonical->GetArrayField(TEXT("Graphs"));
	TestEqual(TEXT("Canonical root graph count"), Graphs.Num(), 1);

	const TSharedPtr<FJsonObject> RootGraph = Graphs[0]->AsObject();
	TestEqual(TEXT("Canonical root graph id"), RootGraph->GetStringField(TEXT("Id")), FString(TEXT("Locomotion")));
	TestEqual(TEXT("Canonical owner node id is written"), RootGraph->GetStringField(TEXT("OwnerNodeId")), FString(TEXT("StateMachineNode")));
	TestEqual(TEXT("Canonical owner pin is written"), RootGraph->GetStringField(TEXT("OwnerPin")), FString(TEXT("OutputPose")));
	TestTrue(TEXT("Canonical entry pins are written"), RootGraph->HasTypedField<EJson::Array>(TEXT("EntryPins")));
	TestTrue(TEXT("Canonical result pins are written"), RootGraph->HasTypedField<EJson::Array>(TEXT("ResultPins")));
	TestTrue(TEXT("Canonical root position is written"), RootGraph->HasTypedField<EJson::Object>(TEXT("Position")));
	TestTrue(TEXT("Canonical metadata is written"), RootGraph->HasTypedField<EJson::Object>(TEXT("Metadata")));
	TestTrue(TEXT("Canonical root evidence is written"), RootGraph->HasTypedField<EJson::Object>(TEXT("Evidence")));
	TestTrue(TEXT("Canonical subgraphs are written"), RootGraph->HasTypedField<EJson::Array>(TEXT("Subgraphs")));

	const TSharedPtr<FJsonObject> RootNode = RootGraph->GetArrayField(TEXT("Nodes"))[0]->AsObject();
	TestTrue(TEXT("Canonical node fields are written"), RootNode->HasTypedField<EJson::Object>(TEXT("Fields")));
	TestTrue(TEXT("Canonical node pins are written"), RootNode->HasTypedField<EJson::Object>(TEXT("Pins")));
	TestTrue(TEXT("Canonical node evidence is written"), RootNode->HasTypedField<EJson::Object>(TEXT("Evidence")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreRejectInvalidRecursiveGraphRegionTest,
	"UEAssetDoc.GraphCore.RejectInvalidRecursiveGraphRegion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreRejectInvalidRecursiveGraphRegionTest::RunTest(const FString& Parameters)
{
	FAssetDocumentGraphParseOptions Options;
	Options.Path = TEXT("/Body/StateMachines");

	const TSharedPtr<FJsonObject> DuplicateGraphs = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    { "Id": "Locomotion", "Kind": "StateMachine", "Nodes": [], "Links": [], "Subgraphs": [] },
    { "Id": "Locomotion", "Kind": "StateMachine", "Nodes": [], "Links": [], "Subgraphs": [] }
  ]
}
)JSON"));
	const FAssetDocumentGraphParseResult DuplicateGraphResult =
		FAssetDocumentGraphParser::ParseGraphRegion(DuplicateGraphs.ToSharedRef(), Options);
	TestFalse(TEXT("Duplicate sibling graph ids fail"), DuplicateGraphResult.IsValid());
	TestTrue(
		TEXT("DuplicateGraphId diagnostic is emitted"),
		GraphCoreHasDiagnosticCode(DuplicateGraphResult, TEXT("DuplicateGraphId")));

	const TSharedPtr<FJsonObject> DuplicateNodes = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "Locomotion",
      "Kind": "StateMachine",
      "Nodes": [
        { "Id": "SpeedCheck", "Class": "/Script/BlueprintGraph.K2Node_VariableGet" },
        { "Id": "SpeedCheck", "Class": "/Script/BlueprintGraph.K2Node_VariableGet" }
      ],
      "Links": [],
      "Subgraphs": []
    }
  ]
}
)JSON"));
	const FAssetDocumentGraphParseResult DuplicateNodeResult =
		FAssetDocumentGraphParser::ParseGraphRegion(DuplicateNodes.ToSharedRef(), Options);
	TestFalse(TEXT("Duplicate node ids fail"), DuplicateNodeResult.IsValid());
	TestTrue(
		TEXT("DuplicateGraphNodeId diagnostic is emitted"),
		GraphCoreHasDiagnosticCode(DuplicateNodeResult, TEXT("DuplicateGraphNodeId")));

	const TSharedPtr<FJsonObject> InvalidSubgraphs = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    { "Id": "Locomotion", "Kind": "StateMachine", "Nodes": [], "Links": [], "Subgraphs": "nope" }
  ]
}
)JSON"));
	const FAssetDocumentGraphParseResult InvalidSubgraphsResult =
		FAssetDocumentGraphParser::ParseGraphRegion(InvalidSubgraphs.ToSharedRef(), Options);
	TestFalse(TEXT("Invalid Subgraphs shape fails"), InvalidSubgraphsResult.IsValid());
	TestTrue(
		TEXT("InvalidGraphRegionType diagnostic is emitted for Subgraphs"),
		GraphCoreHasDiagnosticCode(InvalidSubgraphsResult, TEXT("InvalidGraphRegionType")));

	const TSharedPtr<FJsonObject> InvalidOwner = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    { "Id": "Locomotion", "Kind": "StateMachine", "Owner": "nope", "Nodes": [], "Links": [], "Subgraphs": [] }
  ]
}
)JSON"));
	const FAssetDocumentGraphParseResult InvalidOwnerResult =
		FAssetDocumentGraphParser::ParseGraphRegion(InvalidOwner.ToSharedRef(), Options);
	TestFalse(TEXT("Invalid Owner shape fails"), InvalidOwnerResult.IsValid());
	TestTrue(
		TEXT("InvalidGraphOwner diagnostic is emitted"),
		GraphCoreHasDiagnosticCode(InvalidOwnerResult, TEXT("InvalidGraphOwner")));

	const TSharedPtr<FJsonObject> InvalidGraphId = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    { "Id": "1 Locomotion", "Kind": "StateMachine", "Nodes": [], "Links": [], "Subgraphs": [] }
  ]
}
)JSON"));
	const FAssetDocumentGraphParseResult InvalidGraphIdResult =
		FAssetDocumentGraphParser::ParseGraphRegion(InvalidGraphId.ToSharedRef(), Options);
	TestFalse(TEXT("Invalid graph ids fail"), InvalidGraphIdResult.IsValid());
	TestTrue(
		TEXT("InvalidGraphId diagnostic is emitted"),
		GraphCoreHasDiagnosticCode(InvalidGraphIdResult, TEXT("InvalidGraphId")));

	const TSharedPtr<FJsonObject> UnknownGraphKind = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    { "Id": "Locomotion", "Kind": "FutureGraph", "Nodes": [], "Links": [], "Subgraphs": [] }
  ]
}
)JSON"));
	const FAssetDocumentGraphParseResult UnknownGraphKindResult =
		FAssetDocumentGraphParser::ParseGraphRegion(UnknownGraphKind.ToSharedRef(), Options);
	TestFalse(TEXT("Unknown graph kind fails"), UnknownGraphKindResult.IsValid());
	TestTrue(
		TEXT("UnknownGraphKind diagnostic is emitted"),
		GraphCoreHasDiagnosticCode(UnknownGraphKindResult, TEXT("UnknownGraphKind")));

	const TSharedPtr<FJsonObject> UnknownOwnerReference = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "Locomotion",
      "Kind": "StateMachine",
      "Owner": { "StateMachine": "Missing" },
      "Nodes": [],
      "Links": [],
      "Subgraphs": []
    }
  ]
}
)JSON"));
	const FAssetDocumentGraphParseResult UnknownOwnerReferenceResult =
		FAssetDocumentGraphParser::ParseGraphRegion(UnknownOwnerReference.ToSharedRef(), Options);
	TestFalse(TEXT("Unknown owner reference fails"), UnknownOwnerReferenceResult.IsValid());
	TestTrue(
		TEXT("UnknownGraphOwnerReference diagnostic is emitted"),
		GraphCoreHasDiagnosticCode(UnknownOwnerReferenceResult, TEXT("UnknownGraphOwnerReference")));

	const TSharedPtr<FJsonObject> DuplicatePinOverrides = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "Locomotion",
      "Kind": "StateMachine",
      "Nodes": [
        {
          "Id": "Blend",
          "Class": "/Script/AnimGraph.AnimGraphNode_BlendListByBool",
          "PinOverrides": [
            { "Pin": "Pose" },
            { "Pin": "Pose" }
          ]
        }
      ],
      "Links": [],
      "Subgraphs": []
    }
  ]
}
)JSON"));
	const FAssetDocumentGraphParseResult DuplicatePinOverridesResult =
		FAssetDocumentGraphParser::ParseGraphRegion(DuplicatePinOverrides.ToSharedRef(), Options);
	TestFalse(TEXT("Duplicate pin override ids fail"), DuplicatePinOverridesResult.IsValid());
	TestTrue(
		TEXT("DuplicateGraphPinId diagnostic is emitted"),
		GraphCoreHasDiagnosticCode(DuplicatePinOverridesResult, TEXT("DuplicateGraphPinId")));

	const TSharedPtr<FJsonObject> UnknownEscapedField = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [
    {
      "Id": "Locomotion",
      "Kind": "StateMachine",
      "Nodes": [],
      "Links": [],
      "Subgraphs": [],
      "Bad/Field~Name": true
    }
  ]
}
)JSON"));
	const FAssetDocumentGraphParseResult UnknownEscapedFieldResult =
		FAssetDocumentGraphParser::ParseGraphRegion(UnknownEscapedField.ToSharedRef(), Options);
	TestFalse(TEXT("Unknown escaped field fails"), UnknownEscapedFieldResult.IsValid());
	TestTrue(
		TEXT("Unknown field diagnostic path uses JSON Pointer escaping"),
		HasDiagnosticCodeAtPath(
			UnknownEscapedFieldResult,
			TEXT("UnknownGraphField"),
			TEXT("/Body/StateMachines/Graphs/0/Bad~1Field~0Name")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreCompareRecursiveGraphRegionTest,
	"UEAssetDoc.GraphCore.CompareRecursiveGraphRegion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreCompareRecursiveGraphRegionTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> DesiredRegion = MakeRecursiveGraphRegion();
	const TSharedPtr<FJsonObject> CurrentRegion = MakeRecursiveGraphRegion();
	CurrentRegion->GetArrayField(TEXT("Graphs"))[0]
		->AsObject()
		->GetArrayField(TEXT("Subgraphs"))[0]
		->AsObject()
		->GetArrayField(TEXT("Nodes"))[0]
		->AsObject()
		->GetObjectField(TEXT("Fields"))
		->SetStringField(TEXT("Variable"), TEXT("Velocity"));
	CurrentRegion->GetArrayField(TEXT("Graphs"))[0]
		->AsObject()
		->GetArrayField(TEXT("Subgraphs"))[0]
		->AsObject()
		->GetArrayField(TEXT("Nodes"))[0]
		->AsObject()
		->GetObjectField(TEXT("Position"))
		->SetNumberField(TEXT("X"), 640);

	FAssetDocumentGraphParseOptions Options;
	Options.Path = TEXT("/Body/StateMachines");
	const FAssetDocumentGraphParseResult DesiredParseResult =
		FAssetDocumentGraphParser::ParseGraphRegion(DesiredRegion.ToSharedRef(), Options);
	const FAssetDocumentGraphParseResult CurrentParseResult =
		FAssetDocumentGraphParser::ParseGraphRegion(CurrentRegion.ToSharedRef(), Options);
	TestTrue(TEXT("Desired recursive region parses"), DesiredParseResult.IsValid());
	TestTrue(TEXT("Current recursive region parses"), CurrentParseResult.IsValid());

	const TArray<FAssetDocumentGraphDiffEntry> Entries = FAssetDocumentGraphDiff::CompareGraphRegion(
		DesiredParseResult.Graphs,
		CurrentParseResult.Graphs,
		TEXT("/Body/StateMachines"));

	TestTrue(
		TEXT("Changed nested field uses semantic graph and node path"),
		HasDiffStatusAtPath(
			Entries,
			TEXT("/Body/StateMachines/Graphs/Locomotion/Subgraphs/IdleToRunRule/Nodes/SpeedCheck/Fields/Variable"),
			TEXT("changed")));
	TestTrue(
		TEXT("Changed nested layout uses semantic graph and node path"),
		HasDiffStatusAtPath(
			Entries,
			TEXT("/Body/StateMachines/Graphs/Locomotion/Subgraphs/IdleToRunRule/Nodes/SpeedCheck/Position"),
			TEXT("changed")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreCapabilityResultEncodingTest,
	"UEAssetDoc.GraphCore.CapabilityResultEncoding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreCapabilityResultEncodingTest::RunTest(const FString& Parameters)
{
	FAssetDocumentGraphParseResult ValidResult;
	const FAssetDocumentCapabilityResult ValidCapability = ValidResult.ToCapabilityResult();
	TestTrue(TEXT("Graph parse result without diagnostics converts to success"), ValidCapability.bSuccess);
	TestTrue(TEXT("Successful graph parse conversion has no diagnostics"), ValidCapability.Diagnostics.IsEmpty());

	FAssetDocumentGraphParseResult InvalidResult;
	InvalidResult.AddDiagnostic(TEXT("FirstGraphError"), TEXT("/Body/AnimGraph/Graphs/AnimGraph"), TEXT("First graph error"));
	InvalidResult.AddDiagnostic(TEXT("SecondGraphError"), TEXT("/Body/AnimGraph/Graphs/AnimGraph/Nodes/NodeA"), TEXT("Second graph error"));
	const FAssetDocumentCapabilityResult InvalidCapability = InvalidResult.ToCapabilityResult();
	TestFalse(TEXT("Graph parse result with diagnostics converts to failure"), InvalidCapability.bSuccess);
	TestEqual(TEXT("Graph failure message comes from first diagnostic"), InvalidCapability.Message, FString(TEXT("First graph error")));
	TestEqual(TEXT("All graph diagnostics are preserved"), InvalidCapability.Diagnostics.Num(), 2);
	TestEqual(TEXT("First graph diagnostic code is preserved"), InvalidCapability.Diagnostics[0].Code, FString(TEXT("FirstGraphError")));
	TestEqual(TEXT("Second graph diagnostic path is preserved"), InvalidCapability.Diagnostics[1].Path, FString(TEXT("/Body/AnimGraph/Graphs/AnimGraph/Nodes/NodeA")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreBodyDiffEncodingTest,
	"UEAssetDoc.GraphCore.BodyDiffEncoding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreBodyDiffEncodingTest::RunTest(const FString& Parameters)
{
	FAssetDocumentGraphDiffEntry Entry;
	Entry.Path = TEXT("/Body/AnimGraph/Graphs/AnimGraph/Nodes/NodeA");
	Entry.Status = TEXT("changed");
	Entry.Message = TEXT("Node changed");
	Entry.Current = MakeShared<FJsonValueString>(TEXT("old"));

	const TSharedRef<FJsonObject> Encoded = Entry.ToBodyDiffEntryJsonObject();
	TestEqual(TEXT("Body diff uses lower-case path field"), Encoded->GetStringField(TEXT("path")), Entry.Path);
	TestEqual(TEXT("Body diff uses lower-case status field"), Encoded->GetStringField(TEXT("status")), Entry.Status);
	TestEqual(TEXT("Body diff preserves message"), Encoded->GetStringField(TEXT("message")), Entry.Message);
	TestEqual(TEXT("Body diff preserves current value"), Encoded->GetStringField(TEXT("current")), FString(TEXT("old")));
	TestTrue(TEXT("Body diff writes missing desired value as explicit null"), Encoded->HasTypedField<EJson::Null>(TEXT("desired")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreBlueprintAdapterCommonContractTest,
	"UEAssetDoc.GraphCore.BlueprintAdapterCommonContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreBlueprintAdapterCommonContractTest::RunTest(const FString& Parameters)
{
	const TArray<FAssetDocumentBlueprintGraphRegion>& Regions =
		FAssetDocumentBlueprintGraphAdapterCommon::GetGraphRegions();
	TestEqual(TEXT("Blueprint graph common support declares three regions"), Regions.Num(), 3);
	TestEqual(TEXT("Ubergraph path remains canonical"), Regions[0].Path, FString(TEXT("/Body/UbergraphPages")));
	TestEqual(TEXT("Function graph path remains canonical"), Regions[1].Path, FString(TEXT("/Body/FunctionGraphs")));
	TestEqual(TEXT("Macro graph path remains canonical"), Regions[2].Path, FString(TEXT("/Body/MacroGraphs")));

	const TArray<FAssetDocumentBlueprintGraphRegion>& AnimBlueprintRegions =
		FAssetDocumentBlueprintGraphAdapterCommon::GetGraphRegions(true);
	TestEqual(TEXT("AnimBlueprint graph support opts into one additional region"), AnimBlueprintRegions.Num(), 4);
	if (AnimBlueprintRegions.Num() == 4)
	{
		TestEqual(
			TEXT("Delegate signature graph path is only present in the AnimBlueprint opt-in set"),
			AnimBlueprintRegions[3].Path,
			FString(TEXT("/Body/DelegateSignatureGraphs")));
	}

	FAssetDocumentGraphDiffEntry GraphEntry;
	GraphEntry.Path = TEXT("/Body/UbergraphPages/MyGraph/Nodes/MyNode");
	GraphEntry.Status = TEXT("unsupported");
	GraphEntry.Message = TEXT("Unsupported node");
	const TSharedRef<FJsonObject> Encoded =
		FAssetDocumentBlueprintGraphAdapterCommon::MakeCapabilityDiffEntry(Regions[1], GraphEntry);
	TestEqual(
		TEXT("Function graph diff path is rewritten from the graph diff canonical prefix"),
		Encoded->GetStringField(TEXT("path")),
		FString(TEXT("/Body/FunctionGraphs/MyGraph/Nodes/MyNode")));
	TestEqual(TEXT("Unsupported graph diff is public skipped status"), Encoded->GetStringField(TEXT("status")), FString(TEXT("skipped")));
	TestEqual(TEXT("Unsupported graph diff preserves source status as change"), Encoded->GetStringField(TEXT("change")), FString(TEXT("unsupported")));
	TestEqual(TEXT("Unsupported graph diff emits stable code"), Encoded->GetStringField(TEXT("code")), FString(TEXT("UnsupportedGraphDiff")));
	TestTrue(TEXT("Missing current graph diff value is explicit null"), Encoded->HasTypedField<EJson::Null>(TEXT("current")));
	TestTrue(TEXT("Missing desired graph diff value is explicit null"), Encoded->HasTypedField<EJson::Null>(TEXT("desired")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreFreshEvidenceGuidCompletionTest,
	"UEAssetDoc.GraphCore.FreshEvidenceGuidCompletion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreFreshEvidenceGuidCompletionTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Authored = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [{
    "Id": "AnimGraph", "Kind": "AnimGraph", "Metadata": { "Label": "keep graph metadata" },
    "Nodes": [
      { "Id": "Player", "Class": "/Script/AnimGraph.AnimGraphNode_SequencePlayer", "Comment": "keep player comment", "Fields": { "Rate": 1.0 } },
      { "Id": "Keep", "Class": "/Script/AnimGraph.AnimGraphNode_Root", "NodeGuid": "55555555555555555555555555555555", "Comment": "keep explicit node" }
    ],
    "Links": [],
    "Subgraphs": [
      { "Id": "Nested", "Kind": "TransitionRule", "GraphGuid": "", "Metadata": { "Order": "first" }, "Nodes": [{ "Id": "Rule", "Class": "/Script/BlueprintGraph.K2Node_VariableGet", "NodeGuid": "", "Comment": "keep rule comment" }], "Links": [], "Subgraphs": [] },
      { "Id": "KeepGraph", "Kind": "TransitionRule", "GraphGuid": "66666666666666666666666666666666", "Nodes": [], "Links": [], "Subgraphs": [] }
    ]
  }]
}
)JSON"));
	const TSharedPtr<FJsonObject> Fresh = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [{
    "Id": "AnimGraph", "Kind": "AnimGraph", "GraphGuid": "11111111111111111111111111111111",
    "Nodes": [
      { "Id": "Player", "Class": "/Script/AnimGraph.AnimGraphNode_SequencePlayer", "NodeGuid": "22222222222222222222222222222222" },
      { "Id": "Keep", "Class": "/Script/AnimGraph.AnimGraphNode_Root", "NodeGuid": "55555555555555555555555555555555" }
    ],
    "Links": [],
    "Subgraphs": [
      { "Id": "Nested", "Kind": "TransitionRule", "GraphGuid": "33333333333333333333333333333333", "Nodes": [{ "Id": "Rule", "Class": "/Script/BlueprintGraph.K2Node_VariableGet", "NodeGuid": "44444444444444444444444444444444" }], "Links": [], "Subgraphs": [] },
      { "Id": "KeepGraph", "Kind": "TransitionRule", "GraphGuid": "66666666666666666666666666666666", "Nodes": [], "Links": [], "Subgraphs": [] }
    ]
  }]
}
)JSON"));
	const TSharedPtr<FJsonObject> Expected = ParseJsonObject(TEXT(R"JSON(
{
  "Graphs": [{
    "Id": "AnimGraph", "Kind": "AnimGraph", "Metadata": { "Label": "keep graph metadata" }, "GraphGuid": "11111111111111111111111111111111",
    "Nodes": [
      { "Id": "Player", "Class": "/Script/AnimGraph.AnimGraphNode_SequencePlayer", "Comment": "keep player comment", "Fields": { "Rate": 1.0 }, "NodeGuid": "22222222222222222222222222222222" },
      { "Id": "Keep", "Class": "/Script/AnimGraph.AnimGraphNode_Root", "NodeGuid": "55555555555555555555555555555555", "Comment": "keep explicit node" }
    ],
    "Links": [],
    "Subgraphs": [
      { "Id": "Nested", "Kind": "TransitionRule", "GraphGuid": "33333333333333333333333333333333", "Metadata": { "Order": "first" }, "Nodes": [{ "Id": "Rule", "Class": "/Script/BlueprintGraph.K2Node_VariableGet", "NodeGuid": "44444444444444444444444444444444", "Comment": "keep rule comment" }], "Links": [], "Subgraphs": [] },
      { "Id": "KeepGraph", "Kind": "TransitionRule", "GraphGuid": "66666666666666666666666666666666", "Nodes": [], "Links": [], "Subgraphs": [] }
    ]
  }]
}
)JSON"));

	const FAssetDocumentGraphGuidCompletionResult Result = FAssetDocumentGraphDiff::CompleteMissingGuids(
		MakeShared<FJsonValueObject>(Authored.ToSharedRef()),
		MakeShared<FJsonValueObject>(Fresh.ToSharedRef()),
		TEXT("/Body/AnimGraph"));
	TestTrue(TEXT("Missing graph and node GUIDs complete from fresh evidence"), Result.bSuccess);
	TestTrue(TEXT("Completion returns an authored clone"), Result.CompletedRegion.IsValid() && Result.CompletedRegion->Type == EJson::Object);
	if (!Result.bSuccess || !Result.CompletedRegion.IsValid() || Result.CompletedRegion->Type != EJson::Object)
	{
		return false;
	}

	TestTrue(TEXT("Completion changes only the four unspecified GUID fields while preserving authored arrays, comments, metadata, and field presence"), AssetDocumentGraphJson::AreJsonValuesEqual(Result.CompletedRegion, MakeShared<FJsonValueObject>(Expected.ToSharedRef())));
	TestFalse(TEXT("Input authored graph remains missing its GUID"), Authored->GetArrayField(TEXT("Graphs"))[0]->AsObject()->HasField(TEXT("GraphGuid")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreFreshEvidenceGuidCompletionRejectsInvalidPairsTest,
	"UEAssetDoc.GraphCore.FreshEvidenceGuidCompletionRejectsInvalidPairs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreFreshEvidenceGuidCompletionRejectsInvalidPairsTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Authored = ParseJsonObject(TEXT(R"JSON(
{ "Graphs": [{ "Id": "AnimGraph", "Kind": "AnimGraph", "GraphGuid": "11111111111111111111111111111111", "Nodes": [{ "Id": "Player", "Class": "/Script/AnimGraph.AnimGraphNode_SequencePlayer", "NodeGuid": "22222222222222222222222222222222" }], "Links": [], "Subgraphs": [] }] }
)JSON"));
	auto Complete = [&Authored](const FString& FreshJson)
	{
		return FAssetDocumentGraphDiff::CompleteMissingGuids(
			MakeShared<FJsonValueObject>(Authored.ToSharedRef()),
			MakeShared<FJsonValueObject>(ParseJsonObject(FreshJson).ToSharedRef()),
			TEXT("/Body/AnimGraph"));
	};

	FAssetDocumentGraphGuidCompletionResult Result = Complete(TEXT(R"JSON(
{ "Graphs": [{ "Id": "AnimGraph", "Kind": "AnimGraph", "GraphGuid": "11111111111111111111111111111111", "Nodes": [{ "Id": "Player", "Class": "/Script/AnimGraph.AnimGraphNode_SequencePlayer", "NodeGuid": "22222222222222222222222222222222" }], "Links": [], "Subgraphs": [] }] }
)JSON"));
	TestTrue(TEXT("Explicit identical GUIDs are preserved"), Result.bSuccess);
	TestTrue(TEXT("Explicit identical GUIDs preserve the authored region exactly"), Result.CompletedRegion.IsValid() && AssetDocumentGraphJson::AreJsonValuesEqual(Result.CompletedRegion, MakeShared<FJsonValueObject>(Authored.ToSharedRef())));

	Result = Complete(TEXT(R"JSON(
{ "Graphs": [{ "Id": "AnimGraph", "Kind": "AnimGraph", "GraphGuid": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "Nodes": [{ "Id": "Player", "Class": "/Script/AnimGraph.AnimGraphNode_SequencePlayer", "NodeGuid": "22222222222222222222222222222222" }], "Links": [], "Subgraphs": [] }] }
)JSON"));
	TestFalse(TEXT("Explicit graph GUID mismatch is rejected"), Result.bSuccess);
	TestEqual(TEXT("Graph mismatch reports exact GUID path"), Result.Path, FString(TEXT("/Body/AnimGraph/Graphs/AnimGraph/GraphGuid")));

	Result = Complete(TEXT(R"JSON(
{ "Graphs": [{ "Id": "AnimGraph", "Kind": "AnimGraph", "GraphGuid": "11111111111111111111111111111111", "Nodes": [{ "Id": "Player", "Class": "/Script/AnimGraph.AnimGraphNode_SequencePlayer", "NodeGuid": "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB" }], "Links": [], "Subgraphs": [] }] }
)JSON"));
	TestFalse(TEXT("Explicit node GUID mismatch is rejected"), Result.bSuccess);
	TestEqual(TEXT("Node mismatch reports exact GUID path"), Result.Path, FString(TEXT("/Body/AnimGraph/Graphs/AnimGraph/Nodes/Player/NodeGuid")));

	Result = Complete(TEXT(R"JSON(
{ "Graphs": [{ "Id": "OtherGraph", "Kind": "AnimGraph", "GraphGuid": "11111111111111111111111111111111", "Nodes": [{ "Id": "Player", "Class": "/Script/AnimGraph.AnimGraphNode_SequencePlayer", "NodeGuid": "22222222222222222222222222222222" }], "Links": [], "Subgraphs": [] }] }
)JSON"));
	TestFalse(TEXT("Graph stable-ID mismatch is rejected"), Result.bSuccess);
	TestEqual(TEXT("Graph stable-ID mismatch has stable code"), Result.Code, FString(TEXT("GraphStableIdMismatch")));

	Result = Complete(TEXT(R"JSON(
{ "Graphs": [{ "Id": "AnimGraph", "Kind": "AnimGraph", "GraphGuid": "11111111111111111111111111111111", "Nodes": [{ "Id": "Other", "Class": "/Script/AnimGraph.AnimGraphNode_SequencePlayer", "NodeGuid": "22222222222222222222222222222222" }], "Links": [], "Subgraphs": [] }] }
)JSON"));
	TestFalse(TEXT("Node stable-ID mismatch is rejected"), Result.bSuccess);
	TestEqual(TEXT("Node stable-ID mismatch has stable code"), Result.Code, FString(TEXT("NodeStableIdMismatch")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGraphCoreFreshEvidenceGuidCompletionRejectsMissingFreshGuidTest,
	"UEAssetDoc.GraphCore.FreshEvidenceGuidCompletionRejectsMissingFreshGuid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphCoreFreshEvidenceGuidCompletionRejectsMissingFreshGuidTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Authored = ParseJsonObject(TEXT(R"JSON(
{ "Graphs": [{ "Id": "AnimGraph", "Kind": "AnimGraph", "Nodes": [], "Links": [], "Subgraphs": [] }] }
)JSON"));
	const TSharedPtr<FJsonObject> Fresh = ParseJsonObject(TEXT(R"JSON(
{ "Graphs": [{ "Id": "AnimGraph", "Kind": "AnimGraph", "GraphGuid": "", "Nodes": [], "Links": [], "Subgraphs": [] }] }
)JSON"));
	const FAssetDocumentGraphGuidCompletionResult Result = FAssetDocumentGraphDiff::CompleteMissingGuids(
		MakeShared<FJsonValueObject>(Authored.ToSharedRef()),
		MakeShared<FJsonValueObject>(Fresh.ToSharedRef()),
		TEXT("/Body/AnimGraph"));
	TestFalse(TEXT("Missing fresh graph GUID is rejected"), Result.bSuccess);
	TestEqual(TEXT("Missing fresh graph GUID has stable code"), Result.Code, FString(TEXT("MissingFreshGraphGuid")));
	TestEqual(TEXT("Missing fresh graph GUID reports exact path"), Result.Path, FString(TEXT("/Body/AnimGraph/Graphs/AnimGraph/GraphGuid")));

	const TSharedPtr<FJsonObject> MalformedFresh = ParseJsonObject(TEXT(R"JSON(
{ "Graphs": [{ "Id": "AnimGraph", "Kind": "AnimGraph", "GraphGuid": "not-a-guid", "Nodes": [], "Links": [], "Subgraphs": [] }] }
)JSON"));
	const FAssetDocumentGraphGuidCompletionResult MalformedResult = FAssetDocumentGraphDiff::CompleteMissingGuids(
		MakeShared<FJsonValueObject>(Authored.ToSharedRef()),
		MakeShared<FJsonValueObject>(MalformedFresh.ToSharedRef()),
		TEXT("/Body/AnimGraph"));
	TestFalse(TEXT("Malformed non-empty fresh graph GUID is rejected by graph parsing"), MalformedResult.bSuccess);
	TestEqual(TEXT("Malformed fresh graph GUID has stable parser code"), MalformedResult.Code, FString(TEXT("InvalidGraphGuid")));
	TestEqual(TEXT("Malformed fresh graph GUID has stable parser path"), MalformedResult.Path, FString(TEXT("/Body/AnimGraph/Graphs/AnimGraph/GraphGuid")));
	return true;
}

#endif
