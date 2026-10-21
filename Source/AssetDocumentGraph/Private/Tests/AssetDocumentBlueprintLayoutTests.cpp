// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Blueprint graph layout, on the production route (FAssetDocumentService -> Generic Runtime -> Python
// UBlueprint profile -> graph primitives). Node rectangles are measured as the graph editor draws them.
//   FallbackLayout  a large event graph (123 nodes, 16 events) without any Position: no two nodes
//                   overlap, every event has its own band, exec runs left to right, pure nodes sit left of their
//                   consumers; logs overlap / crossing counts.
//   AddNodes        new nodes without Position in a positioned graph go next to their anchor, clear of other
//                   nodes; the positioned nodes do not move.
//   Relayout        nodes without Position never move unless the graph says "Layout": "Auto"; that relayout
//                   leaves no overlaps and applying it again changes nothing.
//   CommentBoxes    comment boxes (K2:Comment): text, position, size, colour; a node's CommentBubbleVisible;
//                   extract writes them and the extract applies back unchanged.

#include "AssetDocumentGraphNodeSize.h"
#include "AssetDocumentModule.h"
#include "AssetDocumentService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentBlueprintLayoutTests
{
struct FLayoutTestAsset
{
	FString PackageName;
	FString ObjectPath;
	FString PackageFile;
	FString SidecarPath;

	FLayoutTestAsset(const FString& Folder, const TCHAR* Name)
	{
		PackageName = FString(TEXT("/Game/")) / Folder / Name;
		ObjectPath = PackageName + TEXT(".") + Name;
		PackageFile = FPaths::ConvertRelativePathToFull(
			FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()));
		SidecarPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), Folder, FString(Name) + TEXT(".assetdoc.json")));
	}

	UBlueprint* Blueprint() const { return Cast<UBlueprint>(FSoftObjectPath(ObjectPath).ResolveObject()); }
};

struct FLayoutTestFixture
{
	FAutomationTestBase& Test;
	const FString Folder;
	FLayoutTestAsset Asset;

	FLayoutTestFixture(FAutomationTestBase& InTest, const TCHAR* Name)
		: Test(InTest)
		, Folder(FString(TEXT("AssetDocumentP74/")) + FGuid::NewGuid().ToString(EGuidFormats::Digits))
		, Asset(Folder, Name)
	{
	}

	~FLayoutTestFixture()
	{
		if (UPackage* Package = FindPackage(nullptr, *Asset.PackageName))
		{
			Package->SetDirtyFlag(false);
			UPackageTools::UnloadPackages({Package});
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		IFileManager::Get().Delete(*Asset.PackageFile, false, true);
		IFileManager::Get().Delete(*Asset.SidecarPath, false, true);
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles({Asset.PackageFile});
		IFileManager::Get().DeleteDirectory(*FPaths::Combine(FPaths::ProjectContentDir(), Folder), false, true);
	}

	void Report(const FString& Stage, const FAssetDocumentResult& Result) const
	{
		Test.AddInfo(FString::Printf(TEXT("%s: %s"), *Stage, *Result.Message));
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
		{
			Test.AddInfo(FString::Printf(TEXT("%s: [%s] %s %s %s"), *Stage, *Diagnostic.Severity, *Diagnostic.Code,
				*Diagnostic.Path, *Diagnostic.Message));
		}
	}

	// {"SchemaVersion":1,"Target":<package>,"Class":Blueprint,"Action":<action>,"Body":{<body>}}.
	FString Document(const TCHAR* Action, const FString& Body) const
	{
		return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\",\"Body\":{%s}}"),
			*Asset.PackageName, Action, *Body);
	}

	FAssetDocumentResult Apply(const FString& Json, const FString& Stage) const
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Asset.SidecarPath), true);
		FFileHelper::SaveStringToFile(Json, *Asset.SidecarPath);
		FAssetDocumentApplyFileRequest Request;
		Request.FilePath = Asset.SidecarPath;
		Request.bSaveAsset = true;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().ApplyFile(Request);
		Report(Stage, Result);
		return Result;
	}

	bool ApplyOk(const FString& Json, const FString& Stage) const
	{
		return Test.TestTrue(*FString::Printf(TEXT("%s: applies"), *Stage), Apply(Json, Stage).IsSuccess());
	}

	// Applying the same document again changes nothing.
	bool ReapplyUnchanged(const FString& Json, const FString& Stage) const
	{
		const FAssetDocumentResult Again = Apply(Json.Replace(TEXT("\"Action\":\"Create\""), TEXT("\"Action\":\"Update\"")), Stage);
		return Test.TestTrue(*FString::Printf(TEXT("%s: changes nothing"), *Stage), Again.IsSuccess() && !Again.bSavedAsset);
	}

	FAssetDocumentResult Validate(const FString& Json, const FString& Stage) const
	{
		TSharedPtr<FJsonObject> Parsed;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: document parses"), *Stage), FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid()))
		{
			return FAssetDocumentResult();
		}
		FAssetDocumentValidateRequest Request;
		Request.Document = Parsed;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Validate(Request);
		Report(Stage, Result);
		return Result;
	}

	TSharedPtr<FJsonObject> Extract(const FString& Stage) const
	{
		FAssetDocumentExtractRequest Request;
		Request.AssetPath = Asset.ObjectPath;
		const FAssetDocumentResult Result = FAssetDocumentModule::Get().GetService().Extract(Request);
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: extract"), *Stage), Result.IsSuccess() && Result.Payload.IsValid()))
		{
			Report(Stage, Result);
			return nullptr;
		}
		return Result.Payload;
	}

	// The extract, applied back as an Update, changes nothing.
	bool ReapplyExtract(const TSharedPtr<FJsonObject>& Extracted, const FString& Stage) const
	{
		if (!Extracted.IsValid())
		{
			return false;
		}
		Extracted->SetStringField(TEXT("Action"), TEXT("Update"));
		FString Text;
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Extracted.ToSharedRef(), Writer);
		const FAssetDocumentResult Result = Apply(Text, Stage);
		return Test.TestTrue(*FString::Printf(TEXT("%s: the extract applies back unchanged"), *Stage), Result.IsSuccess() && !Result.bSavedAsset);
	}

	UEdGraph* EventGraph() const
	{
		UBlueprint* Blueprint = Asset.Blueprint();
		return Blueprint != nullptr && Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0].Get() : nullptr;
	}
};

struct FLayoutTestBox
{
	double X = 0.0;
	double Y = 0.0;
	double Width = 0.0;
	double Height = 0.0;

	double Right() const { return X + Width; }
	double Bottom() const { return Y + Height; }
	bool Overlaps(const FLayoutTestBox& Other) const
	{
		return X < Other.Right() && Other.X < Right() && Y < Other.Bottom() && Other.Y < Bottom();
	}
	FLayoutTestBox United(const FLayoutTestBox& Other) const
	{
		const double Left = FMath::Min(X, Other.X);
		const double Top = FMath::Min(Y, Other.Y);
		return {Left, Top, FMath::Max(Right(), Other.Right()) - Left, FMath::Max(Bottom(), Other.Bottom()) - Top};
	}
};

// The graph's nodes (comment boxes left out) as the graph editor draws them.
TMap<const UEdGraphNode*, FLayoutTestBox> LayoutTestMeasure(FAutomationTestBase& Test, const UEdGraph& Graph)
{
	TMap<const UEdGraphNode*, FLayoutTestBox> Boxes;
	int32 Unmeasured = 0;
	for (UEdGraphNode* Node : Graph.Nodes)
	{
		if (Node == nullptr || Node->IsA<UEdGraphNode_Comment>())
		{
			continue;
		}
		FVector2D Size(200.0, 100.0);
		if (!UE::AssetDocument::Graph::MeasureGraphNodeWidget(*Node, Size))
		{
			++Unmeasured;
		}
		Boxes.Add(Node, {static_cast<double>(Node->NodePosX), static_cast<double>(Node->NodePosY), Size.X, Size.Y});
	}
	if (Unmeasured > 0)
	{
		Test.AddWarning(FString::Printf(TEXT("%d nodes could not be measured (no Slate); 200 x 100 assumed."), Unmeasured));
	}
	return Boxes;
}

int32 LayoutTestOverlaps(const TMap<const UEdGraphNode*, FLayoutTestBox>& Boxes, TArray<FString>* OutPairs = nullptr)
{
	TArray<TPair<const UEdGraphNode*, FLayoutTestBox>> Items;
	for (const TPair<const UEdGraphNode*, FLayoutTestBox>& Item : Boxes)
	{
		Items.Add(Item);
	}
	int32 Count = 0;
	for (int32 A = 0; A < Items.Num(); ++A)
	{
		for (int32 B = A + 1; B < Items.Num(); ++B)
		{
			if (Items[A].Value.Overlaps(Items[B].Value))
			{
				++Count;
				if (OutPairs != nullptr && OutPairs->Num() < 10)
				{
					OutPairs->Add(Items[A].Key->GetNodeTitle(ENodeTitleType::ListView).ToString() + TEXT(" / ")
						+ Items[B].Key->GetNodeTitle(ENodeTitleType::ListView).ToString());
				}
			}
		}
	}
	return Count;
}

bool LayoutTestHasExec(const UEdGraphNode& Node)
{
	return Node.Pins.ContainsByPredicate([](const UEdGraphPin* Pin) { return Pin != nullptr && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec; });
}

struct FLayoutTestWire
{
	const UEdGraphNode* Source = nullptr;
	const UEdGraphNode* Target = nullptr;
	bool bExec = false;
};

TArray<FLayoutTestWire> LayoutTestWires(const UEdGraph& Graph)
{
	TArray<FLayoutTestWire> Wires;
	for (const UEdGraphNode* Node : Graph.Nodes)
	{
		for (const UEdGraphPin* Pin : Node != nullptr ? Node->Pins : TArray<UEdGraphPin*>())
		{
			if (Pin == nullptr || Pin->Direction != EGPD_Output)
			{
				continue;
			}
			for (const UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (Linked != nullptr && Linked->GetOwningNode() != Node)
				{
					Wires.Add({Node, Linked->GetOwningNode(), Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec});
				}
			}
		}
	}
	return Wires;
}

// Straight wires (output: the node's right edge, input: its left edge, both at mid height) that cross.
int32 LayoutTestCrossings(const TMap<const UEdGraphNode*, FLayoutTestBox>& Boxes, const TArray<FLayoutTestWire>& Wires)
{
	struct FSegment { const UEdGraphNode* A; const UEdGraphNode* B; FVector2D P; FVector2D Q; };
	TArray<FSegment> Segments;
	for (const FLayoutTestWire& Wire : Wires)
	{
		const FLayoutTestBox* From = Boxes.Find(Wire.Source);
		const FLayoutTestBox* To = Boxes.Find(Wire.Target);
		if (From != nullptr && To != nullptr)
		{
			Segments.Add({Wire.Source, Wire.Target, FVector2D(From->Right(), From->Y + From->Height / 2), FVector2D(To->X, To->Y + To->Height / 2)});
		}
	}
	const auto Side = [](const FVector2D& A, const FVector2D& B, const FVector2D& C)
	{
		const double Value = (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
		return Value > 0.0 ? 1 : (Value < 0.0 ? -1 : 0);
	};
	int32 Count = 0;
	for (int32 I = 0; I < Segments.Num(); ++I)
	{
		for (int32 J = I + 1; J < Segments.Num(); ++J)
		{
			const FSegment& S = Segments[I];
			const FSegment& T = Segments[J];
			if (S.A == T.A || S.A == T.B || S.B == T.A || S.B == T.B)
			{
				continue;
			}
			if (Side(S.P, S.Q, T.P) * Side(S.P, S.Q, T.Q) < 0 && Side(T.P, T.Q, S.P) * Side(T.P, T.Q, S.Q) < 0)
			{
				++Count;
			}
		}
	}
	return Count;
}

// Text with each {Token} replaced (document templates).
FString LayoutTestFill(const TCHAR* Template, std::initializer_list<TPair<const TCHAR*, FString>> Values)
{
	FString Out(Template);
	for (const TPair<const TCHAR*, FString>& Value : Values)
	{
		Out.ReplaceInline(Value.Key, *Value.Value, ESearchCase::CaseSensitive);
	}
	return Out;
}

const UEdGraphNode* LayoutTestFindByComment(const UEdGraph& Graph, const TCHAR* Comment)
{
	for (const UEdGraphNode* Node : Graph.Nodes)
	{
		if (Node != nullptr && Node->NodeComment == Comment)
		{
			return Node;
		}
	}
	return nullptr;
}

// A large event graph: 16 events, 123 nodes. Events 0-9: Branch on Health > 0, true sets Health to
// Health + 1 and prints, false prints (9 nodes each); 10-12: Sequence, print / delay and a sphere trace (5 nodes);
// 13-15: set Speed to Speed * Scale and a line trace (6 nodes). The traces are big nodes (many pins), as a
// player Blueprint's calls often are. Every node carries its event's tag in its comment.
FString LayoutTestPlayerBody(int32& OutNodes)
{
	TArray<FString> Nodes;
	TArray<FString> Links;
	const auto Node = [&Nodes](const FString& Id, const TCHAR* Action, const FString& Extra = FString())
	{
		Nodes.Add(FString::Printf(TEXT("{\"Id\":\"%s\",\"Action\":\"%s\"%s}"), *Id, Action, *Extra));
	};
	const auto Link = [&Links](const FString& From, const FString& To)
	{
		Links.Add(FString::Printf(TEXT("{\"From\":\"%s\",\"To\":\"%s\"}"), *From, *To));
	};
	for (int32 Index = 0; Index < 16; ++Index)
	{
		const FString E = FString::Printf(TEXT("E%02d"), Index);
		const FString Tag = FString::Printf(TEXT(",\"Comment\":\"band %02d\""), Index);
		if (Index == 0)
		{
			Node(E, TEXT("K2:BeginPlay"), Tag);
		}
		else if (Index == 1)
		{
			Node(E, TEXT("K2:Tick"), Tag);
		}
		else
		{
			Node(E, TEXT("K2:CustomEvent"), FString::Printf(TEXT(",\"Name\":\"Event%02d\""), Index) + Tag);
		}
		if (Index < 10)
		{
			Node(E + TEXT("Branch"), TEXT("K2:Branch"), Tag);
			Node(E + TEXT("Greater"), TEXT("K2:Greater"), Tag);
			Node(E + TEXT("Get1"), TEXT("K2:Get:Health"), Tag);
			Node(E + TEXT("Set"), TEXT("K2:Set:Health"), Tag);
			Node(E + TEXT("Add"), TEXT("K2:Add"), Tag + TEXT(",\"PinOverrides\":[{\"Pin\":\"B\",\"DefaultValue\":\"1.0\"}]"));
			Node(E + TEXT("Get2"), TEXT("K2:Get:Health"), Tag);
			Node(E + TEXT("PrintTrue"), TEXT("K2:PrintString"), Tag);
			Node(E + TEXT("PrintFalse"), TEXT("K2:PrintString"), Tag);
			Link(E + TEXT(":then"), E + TEXT("Branch:execute"));
			Link(E + TEXT("Branch:then"), E + TEXT("Set:execute"));
			Link(E + TEXT("Branch:else"), E + TEXT("PrintFalse:execute"));
			Link(E + TEXT("Set:then"), E + TEXT("PrintTrue:execute"));
			Link(E + TEXT("Greater:ReturnValue"), E + TEXT("Branch:Condition"));
			Link(E + TEXT("Get1:Health"), E + TEXT("Greater:A"));
			Link(E + TEXT("Add:ReturnValue"), E + TEXT("Set:Health"));
			Link(E + TEXT("Get2:Health"), E + TEXT("Add:A"));
		}
		else if (Index < 13)
		{
			Node(E + TEXT("Sequence"), TEXT("K2:Sequence"), Tag);
			Node(E + TEXT("Print1"), TEXT("K2:PrintString"), Tag);
			Node(E + TEXT("Delay"), TEXT("K2:Delay"), Tag);
			Node(E + TEXT("Print2"), TEXT("K2:CallFunction:/Script/Engine.KismetSystemLibrary:SphereTraceSingle"), Tag);
			Link(E + TEXT(":then"), E + TEXT("Sequence:execute"));
			Link(E + TEXT("Sequence:then_0"), E + TEXT("Print1:execute"));
			Link(E + TEXT("Sequence:then_1"), E + TEXT("Delay:execute"));
			Link(E + TEXT("Delay:then"), E + TEXT("Print2:execute"));
		}
		else
		{
			Node(E + TEXT("Set"), TEXT("K2:Set:Speed"), Tag);
			Node(E + TEXT("Multiply"), TEXT("K2:Multiply"), Tag);
			Node(E + TEXT("Speed"), TEXT("K2:Get:Speed"), Tag);
			Node(E + TEXT("Scale"), TEXT("K2:Get:Scale"), Tag);
			Node(E + TEXT("Print"), TEXT("K2:CallFunction:/Script/Engine.KismetSystemLibrary:LineTraceSingle"), Tag);
			Link(E + TEXT(":then"), E + TEXT("Set:execute"));
			Link(E + TEXT("Set:then"), E + TEXT("Print:execute"));
			Link(E + TEXT("Multiply:ReturnValue"), E + TEXT("Set:Speed"));
			Link(E + TEXT("Speed:Speed"), E + TEXT("Multiply:A"));
			Link(E + TEXT("Scale:Scale"), E + TEXT("Multiply:B"));
		}
	}
	OutNodes = Nodes.Num();
	return FString::Printf(TEXT("\"ParentClass\":\"/Script/Engine.Actor\","
		"\"Variables\":[{\"Name\":\"Health\",\"Type\":\"float\",\"DefaultValue\":\"100.0\"},{\"Name\":\"Speed\",\"Type\":\"float\"},"
		"{\"Name\":\"Scale\",\"Type\":\"float\"}],"
		"\"UbergraphPages\":[{\"Name\":\"EventGraph\",\"Nodes\":[%s],\"Links\":[%s]}]"),
		*FString::Join(Nodes, TEXT(",")), *FString::Join(Links, TEXT(",")));
}
} // namespace AssetDocumentBlueprintLayoutTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintLayoutFallbackTest,
	"UEAssetDoc.Blueprint.Layout.FallbackLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintLayoutFallbackTest::RunTest(const FString&)
{
	namespace LT = AssetDocumentBlueprintLayoutTests;
	LT::FLayoutTestFixture Fixture(*this, TEXT("BP_LayoutPlayer"));
	int32 Authored = 0;
	const FString Json = Fixture.Document(TEXT("Create"), LT::LayoutTestPlayerBody(Authored));
	TestEqual(TEXT("the fixture has 123 nodes"), Authored, 123);
	if (!Fixture.ApplyOk(Json, TEXT("player-sized graph")))
	{
		return false;
	}
	Fixture.ReapplyUnchanged(Json, TEXT("player-sized graph again"));
	const UEdGraph* Graph = Fixture.EventGraph();
	if (!TestNotNull(TEXT("event graph"), Graph))
	{
		return false;
	}
	const TMap<const UEdGraphNode*, LT::FLayoutTestBox> Boxes = LT::LayoutTestMeasure(*this, *Graph);
	TArray<FString> Pairs;
	const int32 Overlaps = LT::LayoutTestOverlaps(Boxes, &Pairs);
	// Bands: the area of each event's nodes (tagged "band NN"); the editor's disabled default event is its own.
	TMap<FString, LT::FLayoutTestBox> Bands;
	int32 Tagged = 0;
	for (const TPair<const UEdGraphNode*, LT::FLayoutTestBox>& Item : Boxes)
	{
		const FString Tag = Item.Key->NodeComment.StartsWith(TEXT("band ")) ? Item.Key->NodeComment : Item.Key->GetName();
		Tagged += Item.Key->NodeComment.StartsWith(TEXT("band ")) ? 1 : 0;
		LT::FLayoutTestBox* Band = Bands.Find(Tag);
		Bands.Add(Tag, Band != nullptr ? Band->United(Item.Value) : Item.Value);
	}
	TestEqual(TEXT("every authored node is in the graph"), Tagged, 123);
	TArray<LT::FLayoutTestBox> BandBoxes;
	Bands.GenerateValueArray(BandBoxes);
	int32 MixedBands = 0;
	for (int32 A = 0; A < BandBoxes.Num(); ++A)
	{
		for (int32 B = A + 1; B < BandBoxes.Num(); ++B)
		{
			MixedBands += BandBoxes[A].Overlaps(BandBoxes[B]) ? 1 : 0;
		}
	}
	const TArray<LT::FLayoutTestWire> Wires = LT::LayoutTestWires(*Graph);
	int32 Backward = 0;
	int32 PureRight = 0;
	for (const LT::FLayoutTestWire& Wire : Wires)
	{
		const LT::FLayoutTestBox* From = Boxes.Find(Wire.Source);
		const LT::FLayoutTestBox* To = Boxes.Find(Wire.Target);
		if (From == nullptr || To == nullptr)
		{
			continue;
		}
		if (Wire.bExec && From->Right() >= To->X)
		{
			++Backward;
		}
		if (!Wire.bExec && !LT::LayoutTestHasExec(*Wire.Source) && From->Right() > To->X)
		{
			++PureRight;
		}
	}
	const int32 Crossings = LT::LayoutTestCrossings(Boxes, Wires);
	LT::FLayoutTestBox Extent = Boxes.Num() > 0 ? Boxes.CreateConstIterator()->Value : LT::FLayoutTestBox();
	for (const TPair<const UEdGraphNode*, LT::FLayoutTestBox>& Item : Boxes)
	{
		Extent = Extent.United(Item.Value);
	}
	AddInfo(FString::Printf(TEXT("layout of %d nodes in %d bands: %d overlapping node pairs, %d overlapping band pairs, %d wire crossings, "
		"%d exec wires running backwards, %d pure nodes right of their consumer, extent %.0f x %.0f"),
		Boxes.Num(), Bands.Num(), Overlaps, MixedBands, Crossings, Backward, PureRight, Extent.Width, Extent.Height));
	for (const FString& Pair : Pairs)
	{
		AddInfo(FString::Printf(TEXT("overlap: %s"), *Pair));
	}
	TestEqual(TEXT("no two nodes overlap"), Overlaps, 0);
	TestEqual(TEXT("every event has a band of its own"), MixedBands, 0);
	TestEqual(TEXT("exec runs left to right"), Backward, 0);
	TestEqual(TEXT("pure nodes sit left of their consumers"), PureRight, 0);
	// Branch outputs fan out in pin order: True's row (Set) beside the Branch, False's row below it.
	const UEdGraphNode* Branch = nullptr;
	const UEdGraphNode* SetNode = nullptr;
	const UEdGraphNode* PrintFalse = nullptr;
	for (const LT::FLayoutTestWire& Wire : Wires)
	{
		if (Wire.Source->NodeComment == TEXT("band 00") && Wire.bExec && Wire.Source->GetNodeTitle(ENodeTitleType::ListView).ToString() == TEXT("Branch"))
		{
			Branch = Wire.Source;
			(LT::LayoutTestHasExec(*Wire.Target) && Wire.Target->GetNodeTitle(ENodeTitleType::ListView).ToString().StartsWith(TEXT("Set")) ? SetNode : PrintFalse) = Wire.Target;
		}
	}
	if (TestTrue(TEXT("band 00 has its Branch"), Branch != nullptr && SetNode != nullptr && PrintFalse != nullptr))
	{
		TestEqual(TEXT("True continues the Branch's row"), SetNode->NodePosY, Branch->NodePosY);
		TestTrue(TEXT("False opens a row below"), PrintFalse->NodePosY > SetNode->NodePosY);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintLayoutAddNodesTest,
	"UEAssetDoc.Blueprint.Layout.AddNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintLayoutAddNodesTest::RunTest(const FString&)
{
	namespace LT = AssetDocumentBlueprintLayoutTests;
	LT::FLayoutTestFixture Fixture(*this, TEXT("BP_LayoutAdd"));
	// A hand-placed graph; an unrelated event sits where the next node in BeginPlay's row would go.
	const FString Placed = TEXT(R"JSON("ParentClass":"/Script/Engine.Actor","UbergraphPages":[{"Name":"EventGraph","Nodes":[
{"Id":"Begin","Action":"K2:BeginPlay","Position":{"X":0,"Y":0}},
{"Id":"Print","Action":"K2:PrintString","Comment":"print","Position":{"X":320,"Y":0}},
{"Id":"Other","Action":"K2:CustomEvent","Name":"Other","Comment":"other","Position":{"X":720,"Y":0}}{Nodes}],
"Links":[{"From":"Begin:then","To":"Print:execute"}{Links}]}])JSON");
	const FString Create = Fixture.Document(TEXT("Create"), LT::LayoutTestFill(*Placed, {{TEXT("{Nodes}"), FString()}, {TEXT("{Links}"), FString()}}));
	if (!Fixture.ApplyOk(Create, TEXT("placed graph")))
	{
		return false;
	}
	const UEdGraph* Graph = Fixture.EventGraph();
	const UEdGraphNode* Print = Graph != nullptr ? LT::LayoutTestFindByComment(*Graph, TEXT("print")) : nullptr;
	const UEdGraphNode* Other = Graph != nullptr ? LT::LayoutTestFindByComment(*Graph, TEXT("other")) : nullptr;
	if (!TestTrue(TEXT("placed nodes exist"), Print != nullptr && Other != nullptr))
	{
		return false;
	}
	// Two nodes added after Print, without Position.
	const FString Update = Fixture.Document(TEXT("Update"), LT::LayoutTestFill(*Placed, {
		{TEXT("{Nodes}"), TEXT(R"JSON(,{"Id":"Delay","Action":"K2:Delay","Comment":"delay"},{"Id":"Print2","Action":"K2:PrintString","Comment":"print2"})JSON")},
		{TEXT("{Links}"), TEXT(R"JSON(,{"From":"Print:then","To":"Delay:execute"},{"From":"Delay:then","To":"Print2:execute"})JSON")}}));
	if (!Fixture.ApplyOk(Update, TEXT("added nodes")))
	{
		return false;
	}
	Fixture.ReapplyUnchanged(Update, TEXT("added nodes again"));
	Graph = Fixture.EventGraph();
	Print = LT::LayoutTestFindByComment(*Graph, TEXT("print"));
	Other = LT::LayoutTestFindByComment(*Graph, TEXT("other"));
	const UEdGraphNode* Delay = LT::LayoutTestFindByComment(*Graph, TEXT("delay"));
	const UEdGraphNode* Print2 = LT::LayoutTestFindByComment(*Graph, TEXT("print2"));
	if (!TestTrue(TEXT("all nodes exist"), Print != nullptr && Other != nullptr && Delay != nullptr && Print2 != nullptr))
	{
		return false;
	}
	TestTrue(TEXT("placed nodes did not move"), Print->NodePosX == 320 && Print->NodePosY == 0 && Other->NodePosX == 720 && Other->NodePosY == 0);
	const TMap<const UEdGraphNode*, LT::FLayoutTestBox> Boxes = LT::LayoutTestMeasure(*this, *Graph);
	TArray<FString> Pairs;
	const int32 Overlaps = LT::LayoutTestOverlaps(Boxes, &Pairs);
	AddInfo(FString::Printf(TEXT("Delay at (%d, %d), Print2 at (%d, %d), %d overlaps %s"), Delay->NodePosX, Delay->NodePosY,
		Print2->NodePosX, Print2->NodePosY, Overlaps, *FString::Join(Pairs, TEXT("; "))));
	TestEqual(TEXT("no two nodes overlap"), Overlaps, 0);
	const LT::FLayoutTestBox& PrintBox = Boxes.FindChecked(Print);
	TestTrue(TEXT("Delay goes right of Print, in its column (moved down only)"), Delay->NodePosX > PrintBox.Right() && Delay->NodePosX < PrintBox.Right() + 200);
	TestTrue(TEXT("Delay is near Print"), Delay->NodePosY >= 0 && Delay->NodePosY < 800);
	TestTrue(TEXT("Print2 follows Delay"), Print2->NodePosX > Boxes.FindChecked(Delay).Right());
	// The extract states the new positions; it applies back unchanged.
	Fixture.ReapplyExtract(Fixture.Extract(TEXT("extract")), TEXT("extract applied"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintLayoutRelayoutTest,
	"UEAssetDoc.Blueprint.Layout.Relayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintLayoutRelayoutTest::RunTest(const FString&)
{
	namespace LT = AssetDocumentBlueprintLayoutTests;
	LT::FLayoutTestFixture Fixture(*this, TEXT("BP_LayoutRelayout"));
	// Five nodes piled on one spot.
	const FString Graph = TEXT(R"JSON("ParentClass":"/Script/Engine.Actor","UbergraphPages":[{"Name":"EventGraph"{Layout},"Nodes":[
{"Id":"Begin","Action":"K2:BeginPlay"{At}},
{"Id":"Print","Action":"K2:PrintString"{At}},
{"Id":"Branch","Action":"K2:Branch"{At}},
{"Id":"Yes","Action":"K2:PrintString"{At}},
{"Id":"No","Action":"K2:PrintString"{At}}],
"Links":[{"From":"Begin:then","To":"Print:execute"},{"From":"Print:then","To":"Branch:execute"},
{"From":"Branch:then","To":"Yes:execute"},{"From":"Branch:else","To":"No:execute"}]}])JSON");
	const auto Variant = [&Graph](const TCHAR* Layout, const TCHAR* At)
	{
		return LT::LayoutTestFill(*Graph, {{TEXT("{Layout}"), Layout}, {TEXT("{At}"), At}});
	};
	const FString Create = Fixture.Document(TEXT("Create"), Variant(TEXT(""), TEXT(",\"Position\":{\"X\":2000,\"Y\":2000}")));
	if (!Fixture.ApplyOk(Create, TEXT("piled graph")))
	{
		return false;
	}
	const UEdGraph* EventGraph = Fixture.EventGraph();
	if (!TestNotNull(TEXT("event graph"), EventGraph))
	{
		return false;
	}
	const int32 Piled = LT::LayoutTestOverlaps(LT::LayoutTestMeasure(*this, *EventGraph));
	TestTrue(TEXT("the piled nodes overlap"), Piled >= 10);
	// Without Positions the nodes stay where they are.
	const FString Keep = Fixture.Document(TEXT("Update"), Variant(TEXT(""), TEXT("")));
	const FAssetDocumentResult Kept = Fixture.Apply(Keep, TEXT("no positions"));
	TestTrue(TEXT("no positions: nothing moves"), Kept.IsSuccess() && !Kept.bSavedAsset);
	TestEqual(TEXT("no positions: still piled"), LT::LayoutTestOverlaps(LT::LayoutTestMeasure(*this, *Fixture.EventGraph())), Piled);
	// "Layout": "Auto" lays them out again.
	const FString Auto = Fixture.Document(TEXT("Update"), Variant(TEXT(",\"Layout\":\"Auto\""), TEXT("")));
	if (!Fixture.ApplyOk(Auto, TEXT("relayout")))
	{
		return false;
	}
	TArray<FString> Pairs;
	EventGraph = Fixture.EventGraph();
	const int32 After = LT::LayoutTestOverlaps(LT::LayoutTestMeasure(*this, *EventGraph), &Pairs);
	AddInfo(FString::Printf(TEXT("relayout: %d overlapping pairs before, %d after %s"), Piled, After, *FString::Join(Pairs, TEXT("; "))));
	TestEqual(TEXT("relayout: no two nodes overlap"), After, 0);
	{
		// Diff of the same document: the graph is laid out, so it reports nothing (an empty Diff = nothing to apply).
		TSharedPtr<FJsonObject> Parsed;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Auto);
		FJsonSerializer::Deserialize(Reader, Parsed);
		FAssetDocumentDiffRequest DiffRequest;
		DiffRequest.Document = Parsed;
		const FAssetDocumentResult Diff = FAssetDocumentModule::Get().GetService().Diff(DiffRequest);
		const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
		FString DiffText;
		if (Diff.Payload.IsValid())
		{
			const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&DiffText);
			FJsonSerializer::Serialize(Diff.Payload.ToSharedRef(), Writer);
		}
		AddInfo(TEXT("diff after relayout: ") + DiffText);
		TestTrue(TEXT("Diff after relayout is empty"), Diff.IsSuccess() && Diff.Payload.IsValid()
			&& Diff.Payload->TryGetArrayField(TEXT("changed"), Changed) && Changed->IsEmpty());
	}
	Fixture.ReapplyUnchanged(Auto, TEXT("relayout again"));
	// An invalid Layout is refused with its path.
	const FAssetDocumentResult Invalid = Fixture.Validate(Fixture.Document(TEXT("Update"),
		Variant(TEXT(",\"Layout\":\"Tidy\""), TEXT(""))), TEXT("invalid layout"));
	TestTrue(TEXT("Layout Tidy: Graph.InvalidLayout"), Invalid.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == TEXT("Graph.InvalidLayout") && Diagnostic.Path.EndsWith(TEXT("/Layout"));
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintLayoutCommentBoxesTest,
	"UEAssetDoc.Blueprint.Layout.CommentBoxes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentBlueprintLayoutCommentBoxesTest::RunTest(const FString&)
{
	namespace LT = AssetDocumentBlueprintLayoutTests;
	LT::FLayoutTestFixture Fixture(*this, TEXT("BP_LayoutComments"));
	const FString Graph = TEXT(R"JSON("ParentClass":"/Script/Engine.Actor","UbergraphPages":[{"Name":"EventGraph","Nodes":[
{"Id":"Box","Action":"K2:Comment","Comment":"Movement","Position":{"X":{X},"Y":{Y}},"Size":{"Width":{W},"Height":{H}}{Box}},
{"Id":"Begin","Action":"K2:BeginPlay","Position":{"X":0,"Y":0}},
{"Id":"Print","Action":"K2:PrintString","Comment":"say hi","Position":{"X":320,"Y":0}{Bubble}}],
"Links":[{"From":"Begin:then","To":"Print:execute"}]}])JSON");
	const auto Variant = [&Graph](int32 X, int32 Y, int32 W, int32 H, const TCHAR* BoxExtra, const TCHAR* Bubble)
	{
		return LT::LayoutTestFill(*Graph, {{TEXT("{X}"), FString::FromInt(X)}, {TEXT("{Y}"), FString::FromInt(Y)},
			{TEXT("{W}"), FString::FromInt(W)}, {TEXT("{H}"), FString::FromInt(H)}, {TEXT("{Box}"), BoxExtra}, {TEXT("{Bubble}"), Bubble}});
	};
	const FString Create = Fixture.Document(TEXT("Create"), Variant(-64, -96, 640, 320,
		TEXT(R"JSON(,"Properties":{"CommentColor":{"R":0.1,"G":0.5,"B":0.9,"A":1.0},"FontSize":24})JSON"),
		TEXT(",\"CommentBubbleVisible\":true")));
	if (!TestTrue(TEXT("comment boxes validate"), Fixture.Validate(Create, TEXT("validate")).IsSuccess()) || !Fixture.ApplyOk(Create, TEXT("comment box")))
	{
		return false;
	}
	Fixture.ReapplyUnchanged(Create, TEXT("comment box again"));
	const UEdGraph* EventGraph = Fixture.EventGraph();
	const UEdGraphNode_Comment* Box = EventGraph != nullptr ? Cast<UEdGraphNode_Comment>(LT::LayoutTestFindByComment(*EventGraph, TEXT("Movement"))) : nullptr;
	const UEdGraphNode* Print = EventGraph != nullptr ? LT::LayoutTestFindByComment(*EventGraph, TEXT("say hi")) : nullptr;
	if (!TestNotNull(TEXT("comment box"), Box) || !TestNotNull(TEXT("print"), Print))
	{
		return false;
	}
	TestTrue(TEXT("box position and size"), Box->NodePosX == -64 && Box->NodePosY == -96 && Box->NodeWidth == 640 && Box->NodeHeight == 320);
	TestTrue(TEXT("box colour"), Box->CommentColor.Equals(FLinearColor(0.1f, 0.5f, 0.9f, 1.0f), 1e-4f));
	TestEqual(TEXT("box font size"), Box->FontSize, 24);
	TestTrue(TEXT("Print's comment bubble is shown and pinned"), Print->bCommentBubbleVisible && Print->bCommentBubblePinned);
	// Move, resize, recolour; FontSize left out keeps its value (omitted Properties = keep); the bubble is hidden.
	const FString Update = Fixture.Document(TEXT("Update"), Variant(32, 64, 800, 400,
		TEXT(R"JSON(,"Properties":{"CommentColor":{"R":1.0,"G":0.0,"B":0.0,"A":1.0}})JSON"), TEXT(",\"CommentBubbleVisible\":false")));
	if (!Fixture.ApplyOk(Update, TEXT("moved box")))
	{
		return false;
	}
	Fixture.ReapplyUnchanged(Update, TEXT("moved box again"));
	TestTrue(TEXT("box moved and resized"), Box->NodePosX == 32 && Box->NodePosY == 64 && Box->NodeWidth == 800 && Box->NodeHeight == 400);
	TestTrue(TEXT("box recoloured"), Box->CommentColor.Equals(FLinearColor(1.0f, 0.0f, 0.0f, 1.0f), 1e-4f));
	TestEqual(TEXT("box font size kept"), Box->FontSize, 24);
	TestFalse(TEXT("Print's comment bubble is hidden"), Print->bCommentBubbleVisible || Print->bCommentBubblePinned);
	// Extract: the box as K2:Comment with its Size; the extract applies back unchanged.
	const TSharedPtr<FJsonObject> Extracted = Fixture.Extract(TEXT("extract"));
	FString Text;
	if (Extracted.IsValid())
	{
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Extracted.ToSharedRef(), Writer);
	}
	AddInfo(Text.Left(4000));
	TestTrue(TEXT("extract writes the box"), Text.Contains(TEXT("\"Action\":\"K2:Comment\"")) && Text.Contains(TEXT("\"Size\":{\"Height\":400,\"Width\":800}"))
		&& Text.Contains(TEXT("\"FontSize\":24")));
	Fixture.ReapplyExtract(Extracted, TEXT("extract applied"));
	// The bubble shown again comes back through extract.
	const FString Shown = Fixture.Document(TEXT("Update"), Variant(32, 64, 800, 400,
		TEXT(R"JSON(,"Properties":{"CommentColor":{"R":1.0,"G":0.0,"B":0.0,"A":1.0}})JSON"), TEXT(",\"CommentBubbleVisible\":true")));
	if (Fixture.ApplyOk(Shown, TEXT("bubble shown")))
	{
		const TSharedPtr<FJsonObject> Again = Fixture.Extract(TEXT("extract bubble"));
		FString AgainText;
		if (Again.IsValid())
		{
			const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&AgainText);
			FJsonSerializer::Serialize(Again.ToSharedRef(), Writer);
		}
		TestTrue(TEXT("extract writes CommentBubbleVisible"), AgainText.Contains(TEXT("\"CommentBubbleVisible\":true")));
		Fixture.ReapplyExtract(Again, TEXT("extract bubble applied"));
	}
	// Only a comment box has a Size.
	const FAssetDocumentResult Refused = Fixture.Validate(Fixture.Document(TEXT("Update"), TEXT(R"JSON("ParentClass":"/Script/Engine.Actor",
"UbergraphPages":[{"Name":"EventGraph","Nodes":[{"Id":"Print","Action":"K2:PrintString","Size":{"Width":100,"Height":100}}],"Links":[]}])JSON")),
		TEXT("size on a print"));
	TestTrue(TEXT("Size on a Print String: Graph.NodeNotResizable"), Refused.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == TEXT("Graph.NodeNotResizable") && Diagnostic.Path.EndsWith(TEXT("/Size"));
	}));
	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
