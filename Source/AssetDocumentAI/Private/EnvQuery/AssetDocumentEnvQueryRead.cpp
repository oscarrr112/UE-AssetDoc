// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// ai.env_query.read.v1: the query as the Environment Query editor shows it (records, paged like ai.state_tree.read.v1).
// A query without graph is read as the editor's first open would build it (Root at 0,0; option i at (300·i, 100),
// wired to Root, every test enabled), so Extract of a factory-made or code-made query is the document the editor shows.

#include "EnvQuery/AssetDocumentEnvQueryPrimitives.h"

#include "AIGraph.h"
#include "AIGraphNode.h"
#include "AssetDocumentGraphValue.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::AssetDocument::Graph::EnvQuery
{
namespace
{
FValue Pair(int32 X, int32 Y)
{
	return MakeArray({MakeInt(X), MakeInt(Y)});
}

// New-node defaults by class, for the sparse properties of one read.
struct FDefaults
{
	TMap<UClass*, TStrongObjectPtr<UObject>> Objects;

	UObject* Get(UClass& Class)
	{
		if (TStrongObjectPtr<UObject>* Found = Objects.Find(&Class))
		{
			return Found->Get();
		}
		return Objects.Add(&Class, TStrongObjectPtr<UObject>(NewDefaultsObject(Class))).Get();
	}
};

FValue NodeValues(UObject& Object, FDefaults& Defaults, FObjectBuilder& Record)
{
	TArray<FString> Unsupported;
	FValue Values = EncodeProperties(Object, Defaults.Get(*Object.GetClass()), Unsupported);
	Record.Add(TEXT("unsupported"), MakeStringArray(Unsupported));
	return Values;
}

void AddComment(FObjectBuilder& Record, const UEdGraphNode& Node)
{
	Record.AddString(TEXT("comment"), Node.NodeComment);
	Record.AddBool(TEXT("comment_visible"), Node.bCommentBubbleVisible);
}

void AppendTest(TArray<FValue>& Records, const FString& Option, int32 Index, UEnvQueryTest& Test, bool bEnabled, float Weight,
	const UAIGraphNode* Node, const UEnvQueryGenerator* Generator, FDefaults& Defaults)
{
	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("test"));
	Record.AddString(TEXT("name"), Node != nullptr ? NodeKey(*Node) : Test.GetName());
	Record.AddString(TEXT("option"), Option);
	Record.AddInt(TEXT("index"), Index);
	Record.AddString(TEXT("class"), Test.GetClass()->GetPathName());
	Record.AddString(TEXT("label"), ClassLabel(*Test.GetClass(), ENodeKind::Test));
	Record.AddBool(TEXT("enabled"), bEnabled);
	Record.Add(TEXT("weight_pct"), MakeNumber(Weight));
	Record.AddBool(TEXT("supported"), Generator == nullptr || Generator->ItemType == nullptr || Test.IsSupportedItem(Generator->ItemType));
	Record.Add(TEXT("properties"), NodeValues(Test, Defaults, Record));
	if (Node != nullptr)
	{
		AddComment(Record, *Node);
	}
	Records.Add(Record.Build());
}

void AppendOption(TArray<FValue>& Records, UEnvQueryOption& Option, int32 X, int32 Y, bool bConnected, int32 RuntimeIndex,
	const UAIGraphNode* Node, FDefaults& Defaults)
{
	UEnvQueryGenerator* Generator = Option.Generator;
	FObjectBuilder Record;
	const FString Key = Node != nullptr ? NodeKey(*Node) : Option.GetName();
	Record.AddString(TEXT("kind"), TEXT("option"));
	Record.AddString(TEXT("name"), Key);
	Record.Add(TEXT("position"), Pair(X, Y));
	Record.AddBool(TEXT("connected"), bConnected);
	Record.AddInt(TEXT("runtime_index"), RuntimeIndex);
	if (Generator != nullptr)
	{
		Record.AddString(TEXT("generator_name"), Generator->GetName());
		Record.AddString(TEXT("class"), Generator->GetClass()->GetPathName());
		Record.AddString(TEXT("label"), ClassLabel(*Generator->GetClass(), ENodeKind::Generator));
		Record.AddString(TEXT("item_type"), Generator->ItemType != nullptr ? Generator->ItemType->GetPathName() : FString());
		Record.Add(TEXT("properties"), NodeValues(*Generator, Defaults, Record));
	}
	else
	{
		Record.AddString(TEXT("class"), Node != nullptr ? FString(Node->ClassData.GetClassName()) : FString());
		Record.Add(TEXT("properties"), MakeEmptyObject());
	}
	if (Node != nullptr)
	{
		AddComment(Record, *Node);
		Record.AddString(TEXT("error"), Node->ErrorMessage);
	}
	TArray<FString> Tests;
	if (Node != nullptr)
	{
		for (const UAIGraphNode* Sub : Node->SubNodes)
		{
			if (Sub != nullptr && Sub->NodeInstance != nullptr)
			{
				Tests.Add(NodeKey(*Sub));
			}
		}
	}
	else
	{
		for (const UEnvQueryTest* Test : Option.Tests)
		{
			if (Test != nullptr)
			{
				Tests.Add(Test->GetName());
			}
		}
	}
	Record.Add(TEXT("tests"), MakeStringArray(Tests));
	Records.Add(Record.Build());

	if (Node != nullptr)
	{
		for (int32 Index = 0; Index < Node->SubNodes.Num(); ++Index)
		{
			const UAIGraphNode* Sub = Node->SubNodes[Index];
			if (UEnvQueryTest* Test = Sub != nullptr ? Cast<UEnvQueryTest>(Sub->NodeInstance) : nullptr)
			{
				AppendTest(Records, Key, Index, *Test, IsTestEnabled(*Sub), GetTestWeight(*Sub), Sub, Generator, Defaults);
			}
		}
	}
	else
	{
		for (int32 Index = 0; Index < Option.Tests.Num(); ++Index)
		{
			if (UEnvQueryTest* Test = Option.Tests[Index])
			{
				AppendTest(Records, Key, Index, *Test, true, -1.0f, nullptr, Generator, Defaults);
			}
		}
	}
}

bool AppendDefaults(FCall& Call, const FValue& Requested, TArray<FValue>& Records, FDefaults& Defaults)
{
	for (int32 Index = 0; Index < Requested.Elements.Num(); ++Index)
	{
		FReader Row = Call.Args.Child(FString::Printf(TEXT("defaults/%d"), Index), Requested.Elements[Index]);
		FString Kind;
		FString ClassText;
		if (!Row.OnlyFields({TEXT("kind"), TEXT("class")}) || !Row.String(TEXT("kind"), Kind) || !Row.String(TEXT("class"), ClassText, false))
		{
			return false;
		}
		if (Kind == TEXT("comment"))
		{
			continue;
		}
		if (Kind != TEXT("generator") && Kind != TEXT("test"))
		{
			return Row.Fail(TEXT("Graph.InvalidArgument"), TEXT("kind is generator, test or comment."), TEXT("kind"));
		}
		const ENodeKind NodeKind = Kind == TEXT("generator") ? ENodeKind::Generator : ENodeKind::Test;
		TArray<FString> Candidates;
		UClass* Class = ResolveClass(ClassText, NodeKind, Candidates);
		if (Class == nullptr)
		{
			Row.Fail(TEXT("EnvQuery.UnknownClass"), FString::Printf(TEXT("'%s' is not a%s class of the Environment Query editor%s%s."),
				*ClassText, NodeKind == ENodeKind::Generator ? TEXT(" generator") : TEXT(" test"),
				Candidates.IsEmpty() ? TEXT("") : TEXT("; did you mean: "), *FString::Join(Candidates, TEXT(", "))), TEXT("class"));
			Call.Error.AddContext(TEXT("candidates"), MakeStringArray(Candidates));
			return false;
		}
		TArray<FString> Unsupported;
		FObjectBuilder Record;
		Record.AddString(TEXT("kind"), TEXT("default"));
		Record.AddString(TEXT("node"), Kind);
		Record.AddString(TEXT("request"), ClassText);
		Record.AddString(TEXT("class"), Class->GetPathName());
		Record.AddString(TEXT("label"), ClassLabel(*Class, NodeKind));
		Record.Add(TEXT("properties"), EncodeProperties(*Defaults.Get(*Class), nullptr, Unsupported));
		Record.Add(TEXT("unsupported"), MakeStringArray(Unsupported));
		Records.Add(Record.Build());
	}
	// A new comment box's Details (always: cheap, and Diff needs them for omitted comment fields).
	UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(GetTransientPackage(), NAME_None, RF_Transient);
	FObjectBuilder Record;
	Record.AddString(TEXT("kind"), TEXT("comment_default"));
	Record.Add(TEXT("properties"), EncodeCommentProperties(*Comment));
	Records.Add(Record.Build());
	return true;
}

void AppendDependents(const UEnvQuery& Query, TArray<FValue>& Records)
{
	IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
	TArray<FName> Referencers;
	Registry.GetReferencers(Query.GetOutermost()->GetFName(), Referencers);
	Referencers.Sort(FNameLexicalLess());
	for (const FName& Package : Referencers)
	{
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPackageName(Package, Assets, true);
		FObjectBuilder Record;
		Record.AddString(TEXT("kind"), TEXT("dependent"));
		Record.AddString(TEXT("package"), Package.ToString());
		Record.AddString(TEXT("class"), Assets.IsEmpty() ? FString() : Assets[0].AssetClassPath.ToString());
		Record.AddString(TEXT("asset"), Assets.IsEmpty() ? FString() : Assets[0].GetObjectPathString());
		Records.Add(Record.Build());
	}
}
} // namespace

bool HandleRead(FCall& Call, FObjectBuilder& Result)
{
	UEnvQuery* Query = GetQuery(Call);
	int64 Offset = 0;
	const FValue* Requested = nullptr;
	bool bDependents = false;
	if (Query == nullptr || !Call.Args.OnlyFields({TEXT("offset"), TEXT("defaults"), TEXT("dependents")})
		|| !Call.Args.Int(TEXT("offset"), Offset, false) || !Call.Args.Array(TEXT("defaults"), Requested, false)
		|| !Call.Args.Bool(TEXT("dependents"), bDependents, false))
	{
		return false;
	}
	if (Offset < 0)
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("offset must be >= 0."), TEXT("offset"));
	}
	FDefaults Defaults;
	TArray<FValue> Records;
	const UAIGraph* Graph = GetGraph(*Query);

	TArray<FMessage> Messages;
	Validate(*Query, Messages);
	TArray<FString> RuntimeOrder;
	for (const UEnvQueryOption* Option : Query->GetOptions())
	{
		RuntimeOrder.Add(Option != nullptr ? OptionKey(*Query, *Option) : FString());
	}
	{
		FObjectBuilder Record;
		Record.AddString(TEXT("kind"), TEXT("query"));
		Record.AddBool(TEXT("graph"), Graph != nullptr);
		Record.AddBool(TEXT("open_in_editor"), IsOpenInEditor(*Query));
		const UEdGraphNode* Root = Graph != nullptr ? FindRoot(*Graph) : nullptr;
		Record.Add(TEXT("root"), Root != nullptr ? Pair(Root->NodePosX, Root->NodePosY) : Pair(0, 0));
		Record.Add(TEXT("runtime_order"), MakeStringArray(RuntimeOrder));
		Record.Add(TEXT("params"), ParamsValue(*Query));
		Record.Add(TEXT("messages"), MessagesValue(Messages));
		Records.Add(Record.Build());
	}
	if (Graph != nullptr)
	{
		for (const UAIGraphNode* Node : GetOptionNodes(*Graph))
		{
			if (UEnvQueryOption* Option = GetOption(*Node))
			{
				AppendOption(Records, *Option, Node->NodePosX, Node->NodePosY, IsConnected(*Node), RuntimeOrder.IndexOfByKey(NodeKey(*Node)),
					Node, Defaults);
			}
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node))
			{
				FObjectBuilder Record;
				Record.AddString(TEXT("kind"), TEXT("comment"));
				Record.AddString(TEXT("name"), Comment->GetName());
				Record.AddString(TEXT("text"), Comment->NodeComment);
				Record.Add(TEXT("position"), Pair(Comment->NodePosX, Comment->NodePosY));
				Record.Add(TEXT("size"), Pair(Comment->NodeWidth, Comment->NodeHeight));
				Record.Add(TEXT("properties"), EncodeCommentProperties(*Comment));
				Records.Add(Record.Build());
			}
		}
	}
	else
	{
		const TArray<UEnvQueryOption*>& Options = Query->GetOptions();
		for (int32 Index = 0; Index < Options.Num(); ++Index)
		{
			if (Options[Index] != nullptr && Options[Index]->Generator != nullptr)
			{
				AppendOption(Records, *Options[Index], Index * 300, 100, true, Index, nullptr, Defaults);
			}
		}
	}
	if (Requested != nullptr && !AppendDefaults(Call, *Requested, Records, Defaults))
	{
		return false;
	}
	if (bDependents)
	{
		AppendDependents(*Query, Records);
	}

	TArray<FValue> Page;
	int32 NodeCount = 8;
	int32 Index = static_cast<int32>(FMath::Min<int64>(Offset, Records.Num()));
	for (; Index < Records.Num(); ++Index)
	{
		const int32 RecordNodes = CountValueNodes(Records[Index]);
		if (!Page.IsEmpty() && NodeCount + RecordNodes > QueryValueNodeBudget)
		{
			break;
		}
		if (Page.IsEmpty() && RecordNodes > QueryValueNodeBudget)
		{
			return Fail(Call, TEXT("Graph.ResultTooLarge"), FString::Printf(
				TEXT("Record %d of the Environment Query read exceeds the runtime value budget."), Index), TEXT("/offset"));
		}
		NodeCount += RecordNodes;
		Page.Add(Records[Index]);
	}
	Result.AddInt(TEXT("total"), Records.Num());
	Result.AddInt(TEXT("next_offset"), Index < Records.Num() ? Index : -1);
	Result.Add(TEXT("records"), MakeArray(MoveTemp(Page)));
	return true;
}
} // namespace UE::AssetDocument::Graph::EnvQuery
