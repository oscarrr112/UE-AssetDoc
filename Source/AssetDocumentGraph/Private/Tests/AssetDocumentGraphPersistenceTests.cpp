// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Acceptance 6 (save / unload / reload keeps GUIDs, links and defaults) and the
// behavior test of the UE-editor-equivalent raw edits.

#include "Tests/AssetDocumentGraphTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetDocumentGraphRawEdits.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "HAL/FileManager.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "PackageTools.h"
#include "UObject/SavePackage.h"

namespace UE::AssetDocument::Graph::Tests
{
namespace
{
struct FGraphFacts
{
	TSet<FString> Nodes;
	TSet<FString> Links;
	TMap<FString, FString> Defaults;
};

FGraphFacts Facts(const FValue& Graph)
{
	FGraphFacts Out;
	if (const FValue* Nodes = At(Graph, TEXT("nodes")))
	{
		for (const FValue& Node : Nodes->Elements)
		{
			const FString Guid = Str(Node, TEXT("node_guid"));
			Out.Nodes.Add(Guid);
			if (const FValue* Pins = At(Node, TEXT("pins")))
			{
				for (const FValue& Pin : Pins->Elements)
				{
					if (Str(Pin, TEXT("direction")) == TEXT("input") && !Str(Pin, TEXT("default_value")).IsEmpty())
					{
						Out.Defaults.Add(Guid + TEXT(".") + Str(Pin, TEXT("name")), Str(Pin, TEXT("default_value")));
					}
				}
			}
		}
	}
	if (const FValue* Links = At(Graph, TEXT("links")))
	{
		for (const FValue& Link : Links->Elements)
		{
			Out.Links.Add(Str(Link, TEXT("from/node_guid")) + TEXT(".") + Str(Link, TEXT("from/pin_name")) + TEXT("->")
				+ Str(Link, TEXT("to/node_guid")) + TEXT(".") + Str(Link, TEXT("to/pin_name")));
		}
	}
	return Out;
}

FString SpawnNode(FSession& Session, const TCHAR* Key, int32 X)
{
	FValue Result;
	Session.Call(TEXT("graph.node.spawn.v1"), FString::Printf(TEXT("{\"graph\":%s,\"action_key\":\"%s\",\"position\":{\"x\":%d,\"y\":0}}"),
		*GraphJson(TEXT("EventGraph")), Key, X), Result);
	return Str(Result, TEXT("node_guid"));
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphSaveReloadTest,
	"UEAssetDoc.Graph.K2.SaveReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphSaveReloadTest::RunTest(const FString& Parameters)
{
	FTestBlueprint Blueprint(TEXT("SaveReload"), TEXT("/Game/AssetDocumentTests/Graph"));
	if (!TestNotNull(TEXT("test Blueprint"), Blueprint.Get()))
	{
		return false;
	}
	const FString Filename = FPackageName::LongPackageNameToFilename(Blueprint.PackageName, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		Blueprint.Destroy();
		IFileManager::Get().Delete(*Filename, false, true);
	};
	FGraphFacts Before;
	{
		FSession Session(*this, *Blueprint.Get());
		if (!Session.IsReady())
		{
			return false;
		}
		const FString BeginPlay = SpawnNode(Session, TEXT("K2:Event:/Script/Engine.Actor:ReceiveBeginPlay"), 0);
		const FString Branch = SpawnNode(Session, TEXT("K2:IfThenElse"), 300);
		const FString Print = SpawnNode(Session, TEXT("K2:CallFunction:/Script/Engine.KismetSystemLibrary:PrintString"), 600);
		FValue Ignored;
		Session.Call(TEXT("graph.pin.connect.v1"), FString::Printf(TEXT("{\"from\":%s,\"to\":%s}"),
			*PinJson(BeginPlay, TEXT("then"), TEXT("output")), *PinJson(Branch, TEXT("execute"), TEXT("input"))), Ignored);
		Session.Call(TEXT("graph.pin.connect.v1"), FString::Printf(TEXT("{\"from\":%s,\"to\":%s}"),
			*PinJson(Branch, TEXT("else"), TEXT("output")), *PinJson(Print, TEXT("execute"), TEXT("input"))), Ignored);
		Session.Call(TEXT("graph.pin.default.set.v1"), FString::Printf(TEXT("{\"pin\":%s,\"kind\":\"value\",\"value\":\"true\"}"),
			*PinJson(Branch, TEXT("Condition"), TEXT("input"))), Ignored);
		Session.Call(TEXT("graph.pin.default.set.v1"), FString::Printf(TEXT("{\"pin\":%s,\"kind\":\"value\",\"value\":\"Persisted\"}"),
			*PinJson(Print, TEXT("InString"), TEXT("input"))), Ignored);
		Session.Call(TEXT("graph.finalize.v1"), TEXT("{}"), Ignored);
		const FValue Query = Session.CallOrNull(TEXT("graph.query.v1"), TEXT("{\"graphs\":[{\"graph_path\":\"EventGraph\"}]}"));
		const FValue* Graph = FindGraph(Query, TEXT("EventGraph"));
		if (!TestNotNull(TEXT("query before save"), Graph))
		{
			return false;
		}
		Before = Facts(*Graph);
		TestEqual(TEXT("two links before save"), Before.Links.Num(), 2);
		TestEqual(TEXT("Condition default before save"), Before.Defaults.FindRef(Branch + TEXT(".Condition")), FString(TEXT("true")));
		TestEqual(TEXT("InString default before save"), Before.Defaults.FindRef(Print + TEXT(".InString")), FString(TEXT("Persisted")));
	}

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	if (!TestTrue(TEXT("package saved"), UPackage::SavePackage(Blueprint.Package, Blueprint.Get(), *Filename, SaveArgs)))
	{
		return false;
	}
	const FString ObjectPath = Blueprint->GetPathName();
	{
		UPackage* Package = Blueprint.Package;
		ResetLoaders(Package);
		TArray<UPackage*> ToUnload{Package};
		FText UnloadError;
		TestTrue(TEXT("package unloaded"), UPackageTools::UnloadPackages(ToUnload, UnloadError, /*bUnloadDirtyPackages=*/true));
		Blueprint.Blueprint = nullptr;
		Blueprint.Package = nullptr;
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	TestNull(TEXT("Blueprint is gone from memory"), FindObject<UBlueprint>(nullptr, *ObjectPath));

	UPackage* Loaded = LoadPackage(nullptr, *Blueprint.PackageName, LOAD_None);
	UBlueprint* Reloaded = Loaded != nullptr ? FindObject<UBlueprint>(Loaded, *FPackageName::GetShortName(Blueprint.PackageName)) : nullptr;
	if (!TestNotNull(TEXT("Blueprint reloaded from disk"), Reloaded))
	{
		return false;
	}
	Blueprint.Package = Loaded;
	Blueprint.Blueprint = Reloaded;
	FSession Session(*this, *Reloaded);
	if (!Session.IsReady())
	{
		return false;
	}
	const FValue Query = Session.CallOrNull(TEXT("graph.query.v1"), TEXT("{\"graphs\":[{\"graph_path\":\"EventGraph\"}]}"));
	const FValue* Graph = FindGraph(Query, TEXT("EventGraph"));
	if (!TestNotNull(TEXT("query after reload"), Graph))
	{
		return false;
	}
	const FGraphFacts After = Facts(*Graph);
	TestTrue(TEXT("node GUIDs persist"), Before.Nodes.Num() == After.Nodes.Num() && Before.Nodes.Includes(After.Nodes));
	TestTrue(TEXT("links persist"), Before.Links.Num() == After.Links.Num() && Before.Links.Includes(After.Links));
	TestTrue(TEXT("pin defaults persist"), Before.Defaults.OrderIndependentCompareEqual(After.Defaults));
	for (const TPair<FString, FString>& Default : Before.Defaults)
	{
		if (After.Defaults.FindRef(Default.Key) != Default.Value)
		{
			AddError(FString::Printf(TEXT("default %s: before '%s' after '%s'"), *Default.Key, *Default.Value, *After.Defaults.FindRef(Default.Key)));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGraphRawEditsTest,
	"UEAssetDoc.Graph.RawEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGraphRawEditsTest::RunTest(const FString& Parameters)
{
	FTestBlueprint Blueprint(TEXT("RawEdits"));
	UEdGraph* Graph = Blueprint.Get() != nullptr && Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0].Get() : nullptr;
	if (!TestNotNull(TEXT("event graph"), Graph))
	{
		return false;
	}
	TArray<UK2Node_Event*> Ghosts;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UK2Node_Event* Event = Cast<UK2Node_Event>(Node); Event != nullptr && Event->IsAutomaticallyPlacedGhostNode())
		{
			Ghosts.Add(Event);
		}
	}
	if (!TestTrue(TEXT("new Actor Blueprint has two or more ghost events"), Ghosts.Num() >= 2))
	{
		return false;
	}
	UK2Node_Event* Ours = Ghosts[0];
	UK2Node_Event* Stock = Ghosts[1];

	// Stock path: the editor connects a pin; UEdGraphPin::MakeLinkTo converts the ghost.
	FGraphNodeCreator<UK2Node_IfThenElse> Creator(*Graph);
	UK2Node_IfThenElse* Branch = Creator.CreateNode();
	Creator.Finalize();
	TestTrue(TEXT("stock connection"), Graph->GetSchema()->TryCreateConnection(Stock->FindPin(UEdGraphSchema_K2::PN_Then), Branch->GetExecPin()));
	TestFalse(TEXT("stock connection converted the ghost"), Stock->IsAutomaticallyPlacedGhostNode());

	TestTrue(TEXT("ConvertGhostNodeToRealNode converts a ghost"), RawEdits::ConvertGhostNodeToRealNode(*Ours));
	TestFalse(TEXT("second conversion is a no-op"), RawEdits::ConvertGhostNodeToRealNode(*Ours));
	TestEqual(TEXT("ghost flag matches stock"), Ours->IsAutomaticallyPlacedGhostNode(), Stock->IsAutomaticallyPlacedGhostNode());
	TestEqual(TEXT("enabled state matches stock"), Ours->GetDesiredEnabledState(), Stock->GetDesiredEnabledState());
	TestEqual(TEXT("user-set enabled state matches stock"), Ours->HasUserSetTheEnabledState(), Stock->HasUserSetTheEnabledState());
	TestEqual(TEXT("comment matches stock"), Ours->NodeComment, Stock->NodeComment);
	TestEqual(TEXT("comment bubble matches stock"), Ours->bCommentBubbleVisible, Stock->bCommentBubbleVisible);

	RawEdits::SetNodePosition(*Branch, 123, -45);
	TestEqual(TEXT("position x"), Branch->NodePosX, 123);
	TestEqual(TEXT("position y"), Branch->NodePosY, -45);
	RawEdits::SetNodeComment(*Branch, TEXT("note"));
	TestEqual(TEXT("comment"), Branch->NodeComment, FString(TEXT("note")));
	RawEdits::SetCommentBubbleVisible(*Branch, true);
	TestTrue(TEXT("bubble visible"), Branch->bCommentBubbleVisible);
	TestTrue(TEXT("bubble pinned like the editor toggle"), Branch->bCommentBubblePinned);
	return true;
}
} // namespace UE::AssetDocument::Graph::Tests

#endif // WITH_DEV_AUTOMATION_TESTS
