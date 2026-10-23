// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Handlers of the graph primitives (contract §3). They only perform the edit
// the stock graph editor would perform; no inverse code: Core rolls back the
// whole package snapshot.

#include "AssetDocumentGraphPrimitives.h"

#include "AssetDocumentGraphPropertyCodec.h"
#include "AssetDocumentEditorPickerValues.h"
#include "AssetDocumentEditorPropertyValueCodec.h"
#include "AssetDocumentGraphRawEdits.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionResult.h"
#include "Engine/Blueprint.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "UObject/SoftObjectPath.h"

namespace UE::AssetDocument::Graph
{
bool FCall::Refresh()
{
	return CollectGraphs(Asset, Graphs, Error);
}

namespace
{
// ---- shared helpers ---------------------------------------------------------

bool Fail(FCall& Call, FStringView Code, FStringView Message, FStringView Path = TEXTVIEW("/"))
{
	if (Call.Error.Code.IsEmpty())
	{
		Call.Error.Set(Code, Message, Path);
	}
	return false;
}

bool ReadGraph(FCall& Call, FReader& Reader, FStringView Key, const FGraphEntry*& OutEntry, bool bRequired = true)
{
	OutEntry = nullptr;
	const FValue* Address = nullptr;
	if (!Reader.Object(Key, Address, bRequired))
	{
		return false;
	}
	if (Address == nullptr)
	{
		return true;
	}
	FReader Child = Reader.Child(Key, *Address);
	return ResolveGraphAddress(Call.Graphs, Child, OutEntry);
}

// The struct pin whose split makes the addressed pin: "ReturnValue_X" is a sub-pin of ReturnValue
// (UEdGraphSchema_K2::SplitPin names sub-pins "<Pin>_<Member>"). Null when there is none.
UEdGraphPin* FindSplittableParent(FCall& Call, FReader& Address)
{
	const FAssetDocumentGraphError Saved = Call.Error;
	UEdGraphNode* Node = nullptr;
	FString PinName;
	FString DirectionText;
	if (!ResolveNodeGuid(Call.Graphs, Address, TEXT("node_guid"), Node) || !Address.String(TEXT("pin_name"), PinName)
		|| !Address.String(TEXT("direction"), DirectionText, false))
	{
		Call.Error = Saved;
		return nullptr;
	}
	EEdGraphPinDirection Direction = EGPD_Input;
	const bool bDirection = ParseDirection(DirectionText, Direction);
	const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Node->GetSchema());  // struct pins split on K2 graphs
	UEdGraphPin* Best = nullptr;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin != nullptr && Pin->SubPins.IsEmpty() && (!bDirection || Pin->Direction == Direction)
			&& PinName.StartsWith(Pin->PinName.ToString() + TEXT("_"), ESearchCase::IgnoreCase)  // pin names are FNames
			&& Schema != nullptr && Schema->CanSplitStructPin(*Pin)
			&& (Best == nullptr || Pin->PinName.GetStringLength() > Best->PinName.GetStringLength()))
		{
			Best = Pin;
		}
	}
	return Best;
}

// bSplitStructs: a mutation addressing a sub-pin of a struct pin that is not split yet splits it
// first, as the editor's "Split Struct Pin" does (nested members one level per split).
bool ReadPin(FCall& Call, FReader& Reader, FStringView Key, UEdGraphPin*& OutPin, bool bRequired = true, bool bSplitStructs = false)
{
	OutPin = nullptr;
	const FValue* Address = nullptr;
	if (!Reader.Object(Key, Address, bRequired))
	{
		return false;
	}
	if (Address == nullptr)
	{
		return true;
	}
	FReader Child = Reader.Child(Key, *Address);
	if (ResolvePinAddress(Call.Graphs, Child, OutPin))
	{
		return true;
	}
	for (int32 Split = 0; bSplitStructs && Call.bMutating && Call.Error.Code == TEXT("Graph.PinNotFound") && Split < 8; ++Split)
	{
		FReader Lookup = Reader.Child(Key, *Address);
		UEdGraphPin* Parent = FindSplittableParent(Call, Lookup);
		if (Parent == nullptr)
		{
			break;
		}
		Parent->GetSchema()->SplitPin(Parent);
		Call.Error = FAssetDocumentGraphError();
		FReader Retry = Reader.Child(Key, *Address);
		if (ResolvePinAddress(Call.Graphs, Retry, OutPin))
		{
			return true;
		}
	}
	return false;
}

// {width, height}: a resizable node's size (a comment box).
bool ReadSize(FReader& Reader, TOptional<FIntPoint>& OutSize)
{
	const FValue* Size = nullptr;
	if (!Reader.Object(TEXT("size"), Size, false))
	{
		return false;
	}
	if (Size == nullptr)
	{
		return true;
	}
	FReader Child = Reader.Child(TEXT("size"), *Size);
	int64 Width = 0;
	int64 Height = 0;
	if (!Child.OnlyFields({TEXT("width"), TEXT("height")}) || !Child.Int(TEXT("width"), Width) || !Child.Int(TEXT("height"), Height))
	{
		return false;
	}
	if (Width < 1 || Height < 1 || Width > 100000 || Height > 100000)
	{
		return Child.Fail(TEXT("Graph.InvalidArgument"), TEXT("A size is 1..100000 wide and high."), TEXT("width"));
	}
	OutSize = FIntPoint(static_cast<int32>(Width), static_cast<int32>(Height));
	return true;
}

bool ReadPosition(FReader& Reader, TOptional<FVector2D>& OutPosition)
{
	const FValue* Position = nullptr;
	if (!Reader.Object(TEXT("position"), Position, false))
	{
		return false;
	}
	if (Position == nullptr)
	{
		return true;
	}
	FReader Child = Reader.Child(TEXT("position"), *Position);
	double X = 0.0;
	double Y = 0.0;
	if (!Child.OnlyFields({TEXT("x"), TEXT("y")}) || !Child.Number(TEXT("x"), X) || !Child.Number(TEXT("y"), Y))
	{
		return false;
	}
	OutPosition = FVector2D(X, Y);
	return true;
}

// {from_pin?, parent_node_guid?}
bool ReadContext(FCall& Call, FReader& Reader, UEdGraphPin*& OutFromPin, UEdGraphNode*& OutParent)
{
	OutFromPin = nullptr;
	OutParent = nullptr;
	const FValue* Context = nullptr;
	if (!Reader.Object(TEXT("context"), Context, false))
	{
		return false;
	}
	if (Context == nullptr)
	{
		return true;
	}
	FReader Child = Reader.Child(TEXT("context"), *Context);
	if (!Child.OnlyFields({TEXT("from_pin"), TEXT("parent_node_guid")}) || !ReadPin(Call, Child, TEXT("from_pin"), OutFromPin, false))
	{
		return false;
	}
	if (Child.Has(TEXT("parent_node_guid")))
	{
		return ResolveNodeGuid(Call.Graphs, Child, TEXT("parent_node_guid"), OutParent);
	}
	return true;
}

TArray<FValue> GuidValues(const TArray<FGuid>& Guids)
{
	TArray<FValue> Values;
	for (const FGuid& Guid : Guids)
	{
		Values.Add(MakeString(FormatAssetDocumentGraphGuid(Guid)));
	}
	return Values;
}

struct FAssetShape
{
	TSet<FGuid> Nodes;
	TSet<FGuid> Graphs;
	TMap<FGuid, TArray<UEdGraphPin*>> PinsByNode;
};

FAssetShape CaptureShape(const FAssetGraphs& Graphs)
{
	FAssetShape Shape;
	for (const FGraphEntry& Entry : Graphs.Graphs)
	{
		Shape.Graphs.Add(Entry.Guid());
		for (UEdGraphNode* Node : Entry.Graph->Nodes)
		{
			if (IsValid(Node))
			{
				Shape.Nodes.Add(Node->NodeGuid);
				Shape.PinsByNode.Add(Node->NodeGuid, Node->Pins);
			}
		}
	}
	return Shape;
}

TArray<FGuid> Difference(const TSet<FGuid>& Left, const TSet<FGuid>& Right)
{
	TArray<FGuid> Out;
	for (const FGuid& Guid : Left)
	{
		if (!Right.Contains(Guid))
		{
			Out.Add(Guid);
		}
	}
	Out.Sort([](const FGuid& A, const FGuid& B) { return A < B; });
	return Out;
}

struct FLinkKey
{
	FGuid FromNode;
	FName FromPin;
	FGuid ToNode;
	FName ToPin;
	bool operator==(const FLinkKey& Other) const
	{
		return FromNode == Other.FromNode && FromPin == Other.FromPin && ToNode == Other.ToNode && ToPin == Other.ToPin;
	}
	friend uint32 GetTypeHash(const FLinkKey& Key)
	{
		return HashCombine(HashCombine(GetTypeHash(Key.FromNode), GetTypeHash(Key.FromPin)),
			HashCombine(GetTypeHash(Key.ToNode), GetTypeHash(Key.ToPin)));
	}
};

FLinkKey MakeLinkKey(const UEdGraphPin& A, const UEdGraphPin& B)
{
	const UEdGraphPin& From = A.Direction == EGPD_Output ? A : B;
	const UEdGraphPin& To = A.Direction == EGPD_Output ? B : A;
	return {From.GetOwningNode()->NodeGuid, From.PinName, To.GetOwningNode()->NodeGuid, To.PinName};
}

TSet<FLinkKey> CaptureLinks(TConstArrayView<const UEdGraphNode*> Nodes)
{
	TSet<FLinkKey> Links;
	for (const UEdGraphNode* Node : Nodes)
	{
		if (Node == nullptr)
		{
			continue;
		}
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			for (const UEdGraphPin* Other : Pin != nullptr ? Pin->LinkedTo : TArray<UEdGraphPin*>())
			{
				if (Other != nullptr && Other->GetOwningNodeUnchecked() != nullptr)
				{
					Links.Add(MakeLinkKey(*Pin, *Other));
				}
			}
		}
	}
	return Links;
}

TArray<FValue> BrokenLinks(const TSet<FLinkKey>& Before, const TSet<FLinkKey>& After)
{
	TArray<FLinkKey> Broken;
	for (const FLinkKey& Link : Before)
	{
		if (!After.Contains(Link))
		{
			Broken.Add(Link);
		}
	}
	Broken.Sort([](const FLinkKey& A, const FLinkKey& B)
	{
		if (A.FromNode != B.FromNode) return A.FromNode < B.FromNode;
		if (A.FromPin != B.FromPin) return A.FromPin.LexicalLess(B.FromPin);
		if (A.ToNode != B.ToNode) return A.ToNode < B.ToNode;
		return A.ToPin.LexicalLess(B.ToPin);
	});
	TArray<FValue> Values;
	for (const FLinkKey& Link : Broken)
	{
		Values.Add(FObjectBuilder()
			.Add(TEXT("from"), FObjectBuilder().AddString(TEXT("node_guid"), FormatAssetDocumentGraphGuid(Link.FromNode))
				.AddString(TEXT("pin_name"), Link.FromPin.ToString()).Build())
			.Add(TEXT("to"), FObjectBuilder().AddString(TEXT("node_guid"), FormatAssetDocumentGraphGuid(Link.ToNode))
				.AddString(TEXT("pin_name"), Link.ToPin.ToString()).Build())
			.Build());
	}
	return Values;
}

void NotifyChanged(FCall& Call, UEdGraph* Graph, bool bStructural)
{
	Call.Graphs.Host->NotifyGraphChanged(Call.Asset, Graph, bStructural);
}

const FGraphEntry* EntryOf(FCall& Call, const UEdGraphNode& Node)
{
	return Call.Graphs.FindEntry(Node.GetGraph());
}

FValue SnapshotOf(FCall& Call, UEdGraphNode& Node)
{
	if (const FGraphEntry* Entry = EntryOf(Call, Node))
	{
		return MakeNodeSnapshot(*Entry, Node, false);
	}
	FGraphEntry Detached;
	Detached.Graph = Node.GetGraph();
	Detached.Schema = FindAssetDocumentGraphSchemaAdapter(Node.GetSchema());
	return MakeNodeSnapshot(Detached, Node, false);
}

bool RequireSchema(FCall& Call, const FGraphEntry& Entry)
{
	if (!Entry.Schema.IsValid())
	{
		return Fail(Call, TEXT("Graph.GraphOperationUnsupported"),
			FString::Printf(TEXT("No schema adapter handles graph %s."), *Entry.Path));
	}
	return true;
}

bool EnumerateKeys(
	FCall& Call, const FGraphEntry& Entry, UEdGraphPin* FromPin, UEdGraphNode* Parent, TArray<FString>& OutKeys)
{
	FAssetDocumentGraphActionQuery Query;
	Query.Asset = &Call.Asset;
	Query.Graph = Entry.Graph;
	Query.FromPin = FromPin;
	Query.ParentNode = Parent;
	Query.bWantUiText = false;
	TArray<FAssetDocumentGraphActionRecord> Records;
	FAssetDocumentGraphError Ignored;
	if (!Entry.Schema->EnumerateActions(Query, Records, Ignored))
	{
		return false;
	}
	for (const FAssetDocumentGraphActionRecord& Record : Records)
	{
		if (!Record.bRequiresContext)
		{
			OutKeys.Add(Record.ActionKey);
		}
	}
	return true;
}

bool TextMatches(const FAssetDocumentGraphActionRecord& Record, const TArray<FString>& Words)
{
	for (const FString& Word : Words)
	{
		if (!Record.ActionKey.Contains(Word) && !Record.DisplayName.Contains(Word) && !Record.Category.Contains(Word))
		{
			return false;
		}
	}
	return true;
}

// ---- graph.list.v1 ----------------------------------------------------------

// Without "offset" every graph is listed (or Graph.ResultTooLarge for an asset with hundreds of
// graphs). With "offset" the list is cut into pages of whole entries that fit the runtime value
// budget (at most "limit" entries); "total" counts all graphs and "next_offset" is null on the
// last page.
bool HandleList(FCall& Call, FObjectBuilder& Result)
{
	FReader& Args = Call.Args;
	// A null offset is not "no offset": a pager that sends back the last page's next_offset (null) must not get the
	// whole unpaged list.
	if (!Args.OnlyFields({TEXT("offset"), TEXT("limit")})
		|| !Args.RefuseNull(TEXT("offset"), TEXT("Graph.InvalidArgument"),
			TEXT("send a number to page (0 for the first page), or omit offset to list every graph; next_offset is null on the last page."))
		|| !Args.RefuseNull(TEXT("limit"), TEXT("Graph.InvalidArgument"), TEXT("send a number, or omit limit.")))
	{
		return false;
	}
	int64 Offset = -1;
	int64 Limit = MAX_int32;
	if (!Args.Int(TEXT("offset"), Offset, false) || !Args.Int(TEXT("limit"), Limit, false))
	{
		return false;
	}
	const bool bPaged = Args.Has(TEXT("offset"));
	if ((bPaged && Offset < 0) || Limit < 1)
	{
		return Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("offset must be >= 0 and limit >= 1."), TEXT("offset"));
	}
	if (!bPaged && Args.Has(TEXT("limit")))
	{
		return Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("limit pages the list and needs offset (0 for the first page)."), TEXT("limit"));
	}
	const TArray<FGraphEntry>& Entries = Call.Graphs.Graphs;
	TArray<FValue> Graphs;
	int32 Next = Entries.Num();
	int32 Budget = QueryValueNodeBudget - 16;
	for (int32 Index = bPaged ? static_cast<int32>(FMath::Min<int64>(Offset, Entries.Num())) : 0; Index < Entries.Num(); ++Index)
	{
		FValue Entry = MakeGraphListEntry(Entries[Index]);
		if (bPaged)
		{
			const int32 Cost = CountValueNodes(Entry) + 1;
			if (Graphs.Num() >= Limit || (Cost > Budget && !Graphs.IsEmpty()))
			{
				Next = Index;
				break;
			}
			Budget -= Cost;
		}
		Graphs.Add(MoveTemp(Entry));
	}
	Result.Add(TEXT("graphs"), MakeArray(MoveTemp(Graphs)));
	if (bPaged)
	{
		Result.AddInt(TEXT("total"), Entries.Num());
		Result.Add(TEXT("next_offset"), Next < Entries.Num() ? MakeInt(Next) : MakeNull());
	}
	// A Blueprint's ancestor classes, nearest first: members it inherits are keyed by the class
	// declaring them, which a short form naming the Blueprint's own class may mean.
	if (const UBlueprint* Blueprint = Cast<UBlueprint>(&Call.Asset))
	{
		TArray<FValue> Chain;
		for (UClass* Ancestor = Blueprint->ParentClass; Ancestor != nullptr; Ancestor = Ancestor->GetSuperClass())
		{
			const UClass* Authoritative = Ancestor->GetAuthoritativeClass();
			Chain.Add(MakeString((Authoritative != nullptr ? Authoritative : Ancestor)->GetPathName()));
		}
		Result.Add(TEXT("class_chain"), MakeArray(MoveTemp(Chain)));
	}
	return true;
}

// ---- graph.query.v1 ---------------------------------------------------------

bool HandleQuery(FCall& Call, FObjectBuilder& Result)
{
	FReader& Args = Call.Args;
	if (!Args.OnlyFields({TEXT("graphs"), TEXT("include_properties"), TEXT("include_sizes"), TEXT("node_offset"),
		TEXT("node_limit")}))
	{
		return false;
	}
	bool bProperties = false;
	// include_sizes: each node's "widget_size", as the graph editor draws it (for laying nodes out).
	bool bSizes = false;
	int64 Offset = 0;
	int64 Limit = MAX_int32;
	const FValue* Requested = nullptr;
	if (!Args.Bool(TEXT("include_properties"), bProperties) || !Args.Bool(TEXT("include_sizes"), bSizes)
		|| !Args.Int(TEXT("node_offset"), Offset, false)
		|| !Args.Int(TEXT("node_limit"), Limit, false) || !Args.Array(TEXT("graphs"), Requested, false))
	{
		return false;
	}
	if (Offset < 0 || Limit < 1)
	{
		return Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("node_offset must be >= 0 and node_limit >= 1."), TEXT("node_offset"));
	}
	TArray<const FGraphEntry*> Entries;
	if (Requested == nullptr)
	{
		for (const FGraphEntry& Entry : Call.Graphs.Graphs)
		{
			Entries.Add(&Entry);
		}
	}
	else
	{
		for (int32 Index = 0; Index < Requested->Elements.Num(); ++Index)
		{
			FReader Address(Requested->Elements[Index], FString::Printf(TEXT("/graphs/%d"), Index), Call.Error);
			const FGraphEntry* Entry = nullptr;
			if (!ResolveGraphAddress(Call.Graphs, Address, Entry))
			{
				return false;
			}
			Entries.AddUnique(Entry);
		}
	}
	// Every requested graph reports its header even when none of its nodes fit, so a request for
	// hundreds of graphs can exceed the budget by headers alone: callers pass "graphs" in chunks.
	constexpr int32 GraphHeaderNodes = 16;
	int32 Budget = QueryValueNodeBudget;
	bool bTruncated = false;
	TArray<FValue> Snapshots;
	for (const FGraphEntry* Entry : Entries)
	{
		TArray<UEdGraphNode*> Nodes;
		for (UEdGraphNode* Node : Entry->Graph->Nodes)
		{
			if (IsValid(Node))
			{
				Nodes.Add(Node);
			}
		}
		const int32 Start = static_cast<int32>(FMath::Min<int64>(Offset, Nodes.Num()));
		const int32 End = static_cast<int32>(FMath::Min<int64>(Nodes.Num(), Start + Limit));
		TArray<FValue> NodeValues;
		TArray<FValue> Links;
		Budget -= GraphHeaderNodes;
		int32 Next = Start;
		for (; Next < End; ++Next)
		{
			UEdGraphNode* Node = Nodes[Next];
			FValue Snapshot = MakeNodeSnapshot(*Entry, *Node, bProperties, bSizes);
			TArray<FValue> NodeLinks;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin == nullptr || Pin->Direction != EGPD_Output)
				{
					continue;
				}
				for (const UEdGraphPin* Other : Pin->LinkedTo)
				{
					if (Other != nullptr && Other->GetOwningNodeUnchecked() != nullptr)
					{
						NodeLinks.Add(MakeLinkValue(*Pin, *Other));
					}
				}
			}
			int32 Cost = CountValueNodes(Snapshot) + 1;
			for (const FValue& Link : NodeLinks)
			{
				Cost += CountValueNodes(Link) + 1;
			}
			const bool bFirstOfResult = Snapshots.IsEmpty() && NodeValues.IsEmpty();
			if (Cost > Budget && !bFirstOfResult)
			{
				break;
			}
			Budget -= Cost;
			NodeValues.Add(MoveTemp(Snapshot));
			Links.Append(MoveTemp(NodeLinks));
		}
		const bool bComplete = Next >= Nodes.Num();
		bTruncated |= Next < End;
		Snapshots.Add(FObjectBuilder()
			.AddInt(TEXT("schema_version"), 1)
			.AddString(TEXT("graph_guid"), FormatAssetDocumentGraphGuid(Entry->Guid()))
			.AddString(TEXT("path"), Entry->Path)
			.AddString(TEXT("role"), Entry->Role)
			.AddString(TEXT("schema_tag"), Entry->SchemaTag)
			.AddInt(TEXT("node_count"), Nodes.Num())
			.Add(TEXT("next_node_offset"), bComplete ? MakeNull() : MakeInt(Next))
			.Add(TEXT("nodes"), MakeArray(MoveTemp(NodeValues)))
			.Add(TEXT("links"), MakeArray(MoveTemp(Links)))
			.Build());
	}
	Result.Add(TEXT("graphs"), MakeArray(MoveTemp(Snapshots)));
	Result.AddBool(TEXT("truncated"), bTruncated);
	return true;
}

// ---- graph.actions.enumerate.v1 --------------------------------------------

bool HandleEnumerate(FCall& Call, FObjectBuilder& Result)
{
	FReader& Args = Call.Args;
	if (!Args.OnlyFields({TEXT("graph"), TEXT("context"), TEXT("filter"), TEXT("limit")}))
	{
		return false;
	}
	const FGraphEntry* Entry = nullptr;
	UEdGraphPin* FromPin = nullptr;
	UEdGraphNode* Parent = nullptr;
	int64 Limit = EnumerateDefaultLimit;
	if (!ReadGraph(Call, Args, TEXT("graph"), Entry) || !ReadContext(Call, Args, FromPin, Parent)
		|| !Args.Int(TEXT("limit"), Limit, false) || !RequireSchema(Call, *Entry))
	{
		return false;
	}
	Limit = FMath::Clamp<int64>(Limit, 1, EnumerateMaximumLimit);
	FString Text;
	FString Prefix;
	TArray<FString> Keys;
	const FValue* Filter = nullptr;
	if (!Args.Object(TEXT("filter"), Filter, false))
	{
		return false;
	}
	if (Filter != nullptr)
	{
		FReader FilterReader = Args.Child(TEXT("filter"), *Filter);
		const FValue* KeyArray = nullptr;
		if (!FilterReader.OnlyFields({TEXT("text"), TEXT("key_prefix"), TEXT("keys")})
			|| !FilterReader.String(TEXT("text"), Text, false) || !FilterReader.String(TEXT("key_prefix"), Prefix, false)
			|| !FilterReader.Array(TEXT("keys"), KeyArray, false))
		{
			return false;
		}
		if (KeyArray != nullptr)
		{
			for (const FValue& Key : KeyArray->Elements)
			{
				if (Key.Kind != EAssetDocumentRuntimeValueKind::String)
				{
					return FilterReader.Fail(TEXT("Graph.InvalidArgument"), TEXT("keys must be strings."), TEXT("keys"));
				}
				Keys.AddUnique(Key.StringValue);
			}
		}
	}
	FAssetDocumentGraphActionQuery Query;
	Query.Asset = &Call.Asset;
	Query.Graph = Entry->Graph;
	Query.FromPin = FromPin;
	Query.ParentNode = Parent;
	Query.Text = Text;
	Query.KeyPrefix = Prefix;
	Query.ExactKeys = Keys;
	Query.bWantUiText = true;
	TArray<FAssetDocumentGraphActionRecord> Records;
	if (!Entry->Schema->EnumerateActions(Query, Records, Call.Error))
	{
		return false;
	}
	TArray<FString> Words;
	Text.ParseIntoArrayWS(Words);
	Records.RemoveAll([&](const FAssetDocumentGraphActionRecord& Record)
	{
		return (!Prefix.IsEmpty() && !Record.ActionKey.StartsWith(Prefix))
			|| (!Keys.IsEmpty() && !Keys.Contains(Record.ActionKey))
			|| !TextMatches(Record, Words);
	});
	Records.Sort([](const FAssetDocumentGraphActionRecord& A, const FAssetDocumentGraphActionRecord& B)
	{
		return A.ActionKey < B.ActionKey;
	});
	TArray<FValue> Actions;
	for (int32 Index = 0; Index < Records.Num() && Index < Limit; ++Index)
	{
		const FAssetDocumentGraphActionRecord& Record = Records[Index];
		FObjectBuilder Action;
		Action.AddString(TEXT("action_key"), Record.ActionKey)
			.AddString(TEXT("display_name"), Record.DisplayName)
			.AddString(TEXT("category"), Record.Category)
			.AddString(TEXT("tooltip"), Record.Tooltip.Left(256))
			.AddString(TEXT("node_class"), Record.NodeClassPath)
			.AddBool(TEXT("requires_context"), Record.bRequiresContext);
		if (!Record.bStable)
		{
			Action.AddBool(TEXT("stable"), false);
		}
		Actions.Add(Action.Build());
	}
	TArray<FValue> Unresolved;
	if (!Keys.IsEmpty())
	{
		TArray<FString> Missing;
		for (const FString& Key : Keys)
		{
			if (!Records.ContainsByPredicate([&Key](const FAssetDocumentGraphActionRecord& Record) { return Record.ActionKey == Key; }))
			{
				Missing.Add(Key);
			}
		}
		if (!Missing.IsEmpty())
		{
			TArray<FString> Known;
			EnumerateKeys(Call, *Entry, FromPin, Parent, Known);
			for (const FString& Key : Missing)
			{
				Unresolved.Add(FObjectBuilder()
					.AddString(TEXT("key"), Key)
					.Add(TEXT("suggestions"), MakeStringArray(SuggestActionKeys(Key, Known)))
					.Build());
			}
		}
	}
	Result.Add(TEXT("actions"), MakeArray(MoveTemp(Actions)))
		.Add(TEXT("unresolved"), MakeArray(MoveTemp(Unresolved)))
		.AddInt(TEXT("total"), Records.Num())
		.AddBool(TEXT("truncated"), Records.Num() > Limit);
	return true;
}

// ---- graph.node.spawn.v1 ----------------------------------------------------

bool HandleSpawn(FCall& Call, FObjectBuilder& Result)
{
	FReader& Args = Call.Args;
	if (!Args.OnlyFields({TEXT("graph"), TEXT("action_key"), TEXT("position"), TEXT("context")}))
	{
		return false;
	}
	const FGraphEntry* Entry = nullptr;
	FString Key;
	TOptional<FVector2D> Position;
	UEdGraphPin* FromPin = nullptr;
	UEdGraphNode* Parent = nullptr;
	if (!ReadGraph(Call, Args, TEXT("graph"), Entry) || !Args.String(TEXT("action_key"), Key)
		|| !ReadPosition(Args, Position) || !ReadContext(Call, Args, FromPin, Parent) || !RequireSchema(Call, *Entry))
	{
		return false;
	}
	if (!Entry->bCanSpawn)
	{
		return Fail(Call, TEXT("Graph.GraphOperationUnsupported"),
			FString::Printf(TEXT("Graph %s is not editable."), *Entry->Path), TEXTVIEW("/graph"));
	}
	const FAssetShape Before = CaptureShape(Call.Graphs);
	FAssetDocumentGraphSpawnRequest Request;
	Request.Asset = &Call.Asset;
	Request.Graph = Entry->Graph;
	Request.ActionKey = Key;
	Request.Position = Position;
	Request.FromPin = FromPin;
	Request.ParentNode = Parent;
	FAssetDocumentGraphSpawnResult Spawned;
	const TSharedPtr<IAssetDocumentGraphSchemaAdapter> Schema = Entry->Schema;
	UEdGraph* Graph = Entry->Graph;
	if (!Schema->ResolveAndSpawn(Request, Spawned, Call.Error))
	{
		if (Call.Error.Code == TEXT("Graph.ActionNotFound"))
		{
			TArray<FString> Known;
			if (EnumerateKeys(Call, *Entry, FromPin, Parent, Known))
			{
				const TArray<FString> Suggestions = SuggestActionKeys(Key, Known);
				Call.Error.AddContext(TEXT("suggestions"), MakeStringArray(Suggestions));
				if (!Suggestions.IsEmpty())
				{
					Call.Error.Message += TEXT(" Did you mean: ") + FString::Join(Suggestions, TEXT(", ")) + TEXT("?");
				}
			}
		}
		if (Call.Error.Path.IsEmpty() || Call.Error.Path == TEXT("/"))
		{
			Call.Error.Path = TEXT("/action_key");
		}
		return false;
	}
	NotifyChanged(Call, Graph, true);
	if (!Call.Refresh())
	{
		return false;
	}
	const FAssetShape After = CaptureShape(Call.Graphs);
	TArray<FGuid> Created = Difference(After.Nodes, Before.Nodes);
	Created.Remove(Spawned.Node->NodeGuid);
	Result.AddString(TEXT("node_guid"), FormatAssetDocumentGraphGuid(Spawned.Node->NodeGuid))
		.Add(TEXT("created_node_guids"), MakeArray(GuidValues(Created)))
		.Add(TEXT("created_graph_guids"), MakeArray(GuidValues(Difference(After.Graphs, Before.Graphs))))
		.AddBool(TEXT("reused"), Spawned.bReused)
		.Add(TEXT("node"), SnapshotOf(Call, *Spawned.Node));
	return true;
}

// ---- graph.node.remove.v1 ---------------------------------------------------

bool HandleRemoveNode(FCall& Call, FObjectBuilder& Result)
{
	FReader& Args = Call.Args;
	UEdGraphNode* Node = nullptr;
	const FGraphEntry* Entry = nullptr;
	if (!Args.OnlyFields({TEXT("node_guid")}) || !ResolveNodeGuid(Call.Graphs, Args, TEXT("node_guid"), Node, &Entry)
		|| !RequireSchema(Call, *Entry))
	{
		return false;
	}
	const FAssetShape Before = CaptureShape(Call.Graphs);
	UEdGraph* Graph = Entry->Graph;
	if (!Entry->Schema->RemoveNode(Call.Asset, *Node, Call.Error))
	{
		return false;
	}
	NotifyChanged(Call, Graph, true);
	if (!Call.Refresh())
	{
		return false;
	}
	const FAssetShape After = CaptureShape(Call.Graphs);
	Result.Add(TEXT("removed_node_guids"), MakeArray(GuidValues(Difference(Before.Nodes, After.Nodes))))
		.Add(TEXT("removed_graph_guids"), MakeArray(GuidValues(Difference(Before.Graphs, After.Graphs))));
	return true;
}

// ---- graph.pin.connect.v1 / graph.pin.break.v1 ------------------------------

FString ResponseText(ECanCreateConnectionResponse Response)
{
	switch (Response)
	{
	case CONNECT_RESPONSE_MAKE: return TEXT("make");
	case CONNECT_RESPONSE_BREAK_OTHERS_A: return TEXT("break_others_a");
	case CONNECT_RESPONSE_BREAK_OTHERS_B: return TEXT("break_others_b");
	case CONNECT_RESPONSE_BREAK_OTHERS_AB: return TEXT("break_others_ab");
	case CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE: return TEXT("make_with_conversion_node");
	case CONNECT_RESPONSE_MAKE_WITH_PROMOTION: return TEXT("make_with_promotion");
	default: return TEXT("disallow");
	}
}

// An optional object {pin name: pin type DTO}.
bool ReadPinTypeMap(FReader& Args, FStringView Key, TMap<FName, FEdGraphPinType>& Out)
{
	const FValue* Map = nullptr;
	if (!Args.Has(Key))
	{
		return true;
	}
	if (!Args.Object(Key, Map))
	{
		return false;
	}
	FReader MapReader = Args.Child(Key, *Map);
	for (int32 Index = 0; Index < Map->ObjectKeys.Num() && Index < Map->Elements.Num(); ++Index)
	{
		if (Map->Elements[Index].Kind != EAssetDocumentRuntimeValueKind::Object)
		{
			return MapReader.Fail(TEXT("Graph.InvalidPinType"), TEXT("Each stated pin type is a pin type object."), Map->ObjectKeys[Index]);
		}
		FReader TypeReader = MapReader.Child(Map->ObjectKeys[Index], Map->Elements[Index]);
		FEdGraphPinType Type;
		if (!ReadPinType(TypeReader, Type))
		{
			return false;
		}
		Out.Add(FName(*Map->ObjectKeys[Index]), Type);
	}
	return true;
}

FString PinTypeText(const UEdGraphPin& Pin)
{
	return Cast<UEdGraphSchema_K2>(Pin.GetSchema()) != nullptr
		? UEdGraphSchema_K2::TypeToText(Pin.PinType).ToString() : Pin.PinType.PinCategory.ToString();
}

// What a link that needs a conversion node needs: the node the editor would insert (graph.pin.connect.v1 refuses
// to insert it, as nodes are the caller's to place) and the types on both sides, as diagnostic context.
struct FConversionNeed
{
	FString Reason;
	FString OutputType;
	FString InputType;
	FAssetDocumentGraphConversion Conversion;
	bool bKnown = false;

	FConversionNeed(const IAssetDocumentGraphSchemaAdapter* Adapter, const UEdGraphPin& Output, const UEdGraphPin& Input,
		const FPinConnectionResponse& Response)
		: Reason(Response.Message.ToString()), OutputType(PinTypeText(Output)), InputType(PinTypeText(Input))
	{
		bKnown = Adapter != nullptr && Adapter->DescribeConversion(Output, Input, Conversion);
	}

	FString NodeText() const
	{
		if (!bKnown || Conversion.ActionKey.IsEmpty())
		{
			return FString::Printf(TEXT("the conversion node (%s)"), *Reason);
		}
		FString Text = FString::Printf(TEXT("a node with action %s (%s)"), *Conversion.ActionKey, *Reason);
		if (!Conversion.InputPin.IsNone() && !Conversion.OutputPin.IsNone())
		{
			Text += FString::Printf(TEXT(", linked into its %s and out of its %s"), *Conversion.InputPin.ToString(), *Conversion.OutputPin.ToString());
		}
		return Text;
	}

	void AddContext(FAssetDocumentGraphError& Error) const
	{
		Error.AddContextString(TEXT("reason"), Reason);
		Error.AddContextString(TEXT("from_type"), OutputType);
		Error.AddContextString(TEXT("to_type"), InputType);
		if (bKnown)
		{
			Error.AddContextString(TEXT("conversion_action"), Conversion.ActionKey);
			Error.AddContextString(TEXT("conversion_node_class"), Conversion.NodeClassPath);
			if (!Conversion.InputPin.IsNone() && !Conversion.OutputPin.IsNone())
			{
				Error.AddContextString(TEXT("conversion_input"), Conversion.InputPin.ToString());
				Error.AddContextString(TEXT("conversion_output"), Conversion.OutputPin.ToString());
			}
		}
	}

	void AddTo(FObjectBuilder& Value) const
	{
		Value.AddString(TEXT("reason"), Reason).AddString(TEXT("from_type"), OutputType).AddString(TEXT("to_type"), InputType);
		if (bKnown)
		{
			Value.AddString(TEXT("conversion_action"), Conversion.ActionKey)
				.AddString(TEXT("conversion_node_class"), Conversion.NodeClassPath);
			if (!Conversion.InputPin.IsNone() && !Conversion.OutputPin.IsNone())
			{
				Value.AddString(TEXT("conversion_input"), Conversion.InputPin.ToString())
					.AddString(TEXT("conversion_output"), Conversion.OutputPin.ToString());
			}
		}
	}
};

// ---- pin defaults (graph.pin.default.set.v1; graph.link.check.v1 defaults) ---------------------------

// A K2 input the editor takes no literal for (a by-ref input that is not auto-created, or a container input: the
// schema refuses a default and the node shows no default widget there) and what to link into it instead: a Make
// Literal node of its type (Value in, ReturnValue out), a Make Array / Set / Map node, or a variable or another node's
// output of that type. NeedsLink is empty for any other refusal (a value the type cannot hold).
struct FPinDefaultLink
{
	FString NeedsLink;
	FString PinType;
	FString MakeAction;
	FString MakeInput;
	FString MakeOutput;

	explicit FPinDefaultLink(const UEdGraphPin& Pin)
	{
		// A function's return node takes its inputs unlinked (they are its outputs).
		if (Cast<UEdGraphSchema_K2>(Pin.GetSchema()) == nullptr || Pin.Direction != EGPD_Input || UEdGraphSchema_K2::IsAutoCreateRefTerm(&Pin)
			|| Cast<UK2Node_FunctionResult>(Pin.GetOwningNodeUnchecked()) != nullptr)
		{
			return;
		}
		const FEdGraphPinType& Type = Pin.PinType;
		// The type of what links into it (a by-ref input reads "Name (by ref)").
		FEdGraphPinType LinkedType = Type;
		LinkedType.bIsReference = false;
		PinType = UEdGraphSchema_K2::TypeToText(LinkedType).ToString();
		if (Type.IsContainer())
		{
			NeedsLink = TEXT("container");
			MakeAction = Type.IsArray() ? TEXT("K2:MakeArray") : Type.IsSet() ? TEXT("K2:MakeSet") : TEXT("K2:MakeMap");
			MakeOutput = Type.IsArray() ? TEXT("Array") : Type.IsSet() ? TEXT("Set") : TEXT("Map");
			return;
		}
		if (!Type.bIsReference)
		{
			return;
		}
		NeedsLink = TEXT("by_ref");
		const FName Category = Type.PinCategory;
		const TCHAR* Literal = Category == UEdGraphSchema_K2::PC_Name ? TEXT("MakeLiteralName")
			: Category == UEdGraphSchema_K2::PC_String ? TEXT("MakeLiteralString")
			: Category == UEdGraphSchema_K2::PC_Text ? TEXT("MakeLiteralText")
			: Category == UEdGraphSchema_K2::PC_Int ? TEXT("MakeLiteralInt")
			: Category == UEdGraphSchema_K2::PC_Int64 ? TEXT("MakeLiteralInt64")
			: Category == UEdGraphSchema_K2::PC_Real ? TEXT("MakeLiteralDouble")
			: Category == UEdGraphSchema_K2::PC_Boolean ? TEXT("MakeLiteralBool")
			: Category == UEdGraphSchema_K2::PC_Byte && Type.PinSubCategoryObject == nullptr ? TEXT("MakeLiteralByte")
			: nullptr;
		if (Literal != nullptr)
		{
			MakeAction = FString(TEXT("K2:CallFunction:/Script/Engine.KismetSystemLibrary:")) + Literal;
			MakeInput = TEXT("Value");
			MakeOutput = TEXT("ReturnValue");
		}
	}

	void AddContext(FAssetDocumentGraphError& Error) const
	{
		if (NeedsLink.IsEmpty())
		{
			return;
		}
		Error.AddContextString(TEXT("needs_link"), NeedsLink);
		Error.AddContextString(TEXT("pin_type"), PinType);
		if (!MakeAction.IsEmpty())
		{
			Error.AddContextString(TEXT("make_action"), MakeAction);
			Error.AddContextString(TEXT("make_output"), MakeOutput);
			if (!MakeInput.IsEmpty())
			{
				Error.AddContextString(TEXT("make_input"), MakeInput);
			}
		}
	}

	void AddTo(FObjectBuilder& Value) const
	{
		if (NeedsLink.IsEmpty())
		{
			return;
		}
		Value.AddString(TEXT("needs_link"), NeedsLink).AddString(TEXT("pin_type"), PinType);
		if (!MakeAction.IsEmpty())
		{
			Value.AddString(TEXT("make_action"), MakeAction).AddString(TEXT("make_output"), MakeOutput);
			if (!MakeInput.IsEmpty())
			{
				Value.AddString(TEXT("make_input"), MakeInput);
			}
		}
	}
};

// The check graph.pin.default.set.v1 makes before it sets a default (kinds value / text / object): the schema's
// (or its adapter's) IsPinDefaultValid. Value: an enum entry written as the editor's dropdown shows it becomes the
// enumerator's name; Object: the loaded object (an object path that loads nothing fails with Path "/value").
struct FPinDefaultCheck
{
	FString Value;
	FText Text;
	UObject* Object = nullptr;
	FString Invalid;
	bool bUnresolvedObject = false;

	FPinDefaultCheck(const UEdGraphPin& Pin, const FString& Kind, const FString& InValue)
		: Value(InValue)
	{
		const UEdGraphSchema* Schema = Pin.GetSchema();
		const TSharedPtr<IAssetDocumentGraphSchemaAdapter> Adapter = FindAssetDocumentGraphSchemaAdapter(Schema);
		const auto Validate = [&Adapter, Schema, &Pin](const FString& String, UObject* Loaded, const FText& TextValue)
		{
			return Adapter.IsValid() ? Adapter->ValidatePinDefault(Pin, String, Loaded, TextValue)
				: Schema != nullptr ? Schema->IsPinDefaultValid(&Pin, String, Loaded, TextValue) : FString(TEXT("The pin has no schema."));
		};
		if (Kind == TEXT("value"))
		{
			// An enum pin holds the enumerator's name; the author may name it as the editor's dropdown shows
			// it (a User Defined Enum's display name "Ground" for NewEnumerator0).
			if (const UEnum* Enum = Cast<UEnum>(Pin.PinType.PinSubCategoryObject.Get()); Enum != nullptr && Enum->GetIndexByNameString(Value) == INDEX_NONE)
			{
				const int32 Index = EditorPropertyValueCodec::FindEnumIndexBySpelling(*Enum, Value);
				if (Index != INDEX_NONE)
				{
					Value = Enum->GetNameStringByIndex(Index);
				}
			}
			Invalid = Validate(Value, nullptr, FText::GetEmpty());
		}
		else if (Kind == TEXT("text"))
		{
			Text = FText::FromString(Value);
			Invalid = Validate(FString(), nullptr, Text);
		}
		else if (Kind == TEXT("object"))
		{
			if (!Value.IsEmpty())
			{
				Object = FSoftObjectPath(Value).TryLoad();
				if (Object == nullptr)
				{
					bUnresolvedObject = true;
					Invalid = FString::Printf(TEXT("Object '%s' does not resolve."), *Value);
					return;
				}
			}
			Invalid = Validate(FString(), Object, FText::GetEmpty());
		}
	}

	// The diagnostic's message: the editor's reason, for the pin.
	FString Message(const UEdGraphPin& Pin) const
	{
		return bUnresolvedObject ? Invalid : FString::Printf(TEXT("Pin '%s': %s"), *Pin.PinName.ToString(), *Invalid);
	}
};

bool HandleConnect(FCall& Call, FObjectBuilder& Result)
{
	FReader& Args = Call.Args;
	UEdGraphPin* A = nullptr;
	UEdGraphPin* B = nullptr;
	bool bAllowConversion = false;
	TMap<FName, FEdGraphPinType> KeepFromTypes;
	TMap<FName, FEdGraphPinType> KeepToTypes;
	if (!Args.OnlyFields({TEXT("from"), TEXT("to"), TEXT("allow_conversion_node"), TEXT("keep_from_types"), TEXT("keep_to_types")})
		|| !ReadPin(Call, Args, TEXT("from"), A, true, true) || !ReadPin(Call, Args, TEXT("to"), B, true, true)
		|| !Args.Bool(TEXT("allow_conversion_node"), bAllowConversion) || !ReadPinTypeMap(Args, TEXT("keep_from_types"), KeepFromTypes)
		|| !ReadPinTypeMap(Args, TEXT("keep_to_types"), KeepToTypes))
	{
		return false;
	}
	UEdGraph* Graph = A->GetOwningNode()->GetGraph();
	const UEdGraphSchema* Schema = Graph->GetSchema();
	const FPinConnectionResponse Response = Schema->CanCreateConnection(A, B);
	const FString Text = ResponseText(Response.Response);
	if (Response.Response == CONNECT_RESPONSE_DISALLOW)
	{
		// A promotable == / != takes its type from the first link and the editor then offers an enum only
		// as a byte; two enum values compare with the editor's Equal (Enum) / Not Equal (Enum) nodes.
		const auto IsEnumPin = [](const UEdGraphPin* Pin) { return Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get()) != nullptr; };
		const auto IsPromotable = [](const UEdGraphPin* Pin) { return Pin->GetOwningNode()->GetClass()->GetName() == TEXT("K2Node_PromotableOperator"); };
		const FString Hint = (IsEnumPin(A) && IsPromotable(B)) || (IsEnumPin(B) && IsPromotable(A))
			? TEXT(" Compare enum values with K2:EqualEnum / K2:NotEqualEnum (the editor's Equal (Enum) / Not Equal (Enum)).") : TEXT("");
		Fail(Call, TEXT("Graph.ConnectDisallowed"), FString::Printf(TEXT("Cannot connect %s.%s to %s.%s: %s%s"),
			*FormatAssetDocumentGraphGuid(A->GetOwningNode()->NodeGuid), *A->PinName.ToString(),
			*FormatAssetDocumentGraphGuid(B->GetOwningNode()->NodeGuid), *B->PinName.ToString(),
			*Response.Message.ToString(), *Hint), TEXTVIEW("/to"));
		Call.Error.AddContextString(TEXT("reason"), Response.Message.ToString());
		return false;
	}
	// A promotion (a promotable operator's pin taking the linked type, e.g. Multiply's B becoming a
	// float next to a vector A) is what the editor's drag-drop does without asking: no node is added.
	// Only a conversion node needs the caller's consent.
	const bool bNeedsConversion = Response.Response == CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE;
	const bool bReshapes = bNeedsConversion || Response.Response == CONNECT_RESPONSE_MAKE_WITH_PROMOTION;
	if (bNeedsConversion && !bAllowConversion)
	{
		const bool bAIsOutput = A->Direction == EGPD_Output;
		const UEdGraphPin& Output = bAIsOutput ? *A : *B;
		const UEdGraphPin& Input = bAIsOutput ? *B : *A;
		const FGraphEntry* Owner = EntryOf(Call, *Output.GetOwningNode());
		const FConversionNeed Need(Owner != nullptr ? Owner->Schema.Get() : nullptr, Output, Input, Response);
		Fail(Call, TEXT("Graph.ConnectNeedsConversion"), FString::Printf(
			TEXT("%s.%s (%s) joins %s.%s (%s) only through %s, which the editor inserts on drag: place that node and link through it, or link a pin of type %s."),
			*FormatAssetDocumentGraphGuid(Output.GetOwningNode()->NodeGuid), *Output.PinName.ToString(), *Need.OutputType,
			*FormatAssetDocumentGraphGuid(Input.GetOwningNode()->NodeGuid), *Input.PinName.ToString(), *Need.InputType,
			*Need.NodeText(), *Need.InputType), TEXTVIEW("/to"));
		Call.Error.AddContextString(TEXT("response"), Text);
		Need.AddContext(Call.Error);
		return false;
	}
	const FAssetShape Before = CaptureShape(Call.Graphs);
	TArray<const UEdGraphNode*> Touched = {A->GetOwningNode(), B->GetOwningNode()};
	for (const UEdGraphPin* Pin : {A, B})
	{
		for (const UEdGraphPin* Other : Pin->LinkedTo)
		{
			Touched.AddUnique(Other->GetOwningNode());
		}
	}
	const TSet<FLinkKey> LinksBefore = CaptureLinks(Touched);
	// The stock editor (SGraphPanel drag-drop) calls TryCreateConnection; ghost
	// nodes become real nodes inside UEdGraphPin::MakeLinkTo.
	const FGraphEntry* PinEntry = EntryOf(Call, *A->GetOwningNode());
	const bool bConnected = PinEntry != nullptr && PinEntry->Schema.IsValid()
		? PinEntry->Schema->ConnectPins(*A, *B, KeepFromTypes.Num() > 0 ? &KeepFromTypes : nullptr,
			KeepToTypes.Num() > 0 ? &KeepToTypes : nullptr) : Schema->TryCreateConnection(A, B);
	if (!bConnected)
	{
		return Fail(Call, TEXT("Graph.ConnectDisallowed"), FString::Printf(TEXT("The schema refused the connection: %s"),
			*Response.Message.ToString()), TEXTVIEW("/to"));
	}
	NotifyChanged(Call, Graph, bReshapes);
	if (!Call.Refresh())
	{
		return false;
	}
	const FAssetShape After = CaptureShape(Call.Graphs);
	TArray<const UEdGraphNode*> TouchedAfter;
	for (const UEdGraphNode* Node : Touched)
	{
		if (IsValid(Node))
		{
			TouchedAfter.Add(Node);
		}
	}
	TArray<FGuid> Reconstructed;
	for (const TPair<FGuid, TArray<UEdGraphPin*>>& Pair : Before.PinsByNode)
	{
		const TArray<UEdGraphPin*>* Now = After.PinsByNode.Find(Pair.Key);
		if (Now != nullptr && *Now != Pair.Value)
		{
			Reconstructed.Add(Pair.Key);
		}
	}
	Reconstructed.Sort([](const FGuid& L, const FGuid& R) { return L < R; });
	Result.AddString(TEXT("response"), Text)
		.Add(TEXT("created_node_guids"), MakeArray(GuidValues(Difference(After.Nodes, Before.Nodes))))
		.Add(TEXT("broken_links"), MakeArray(BrokenLinks(LinksBefore, CaptureLinks(TouchedAfter))))
		.Add(TEXT("reconstructed_node_guids"), MakeArray(GuidValues(Reconstructed)));
	return true;
}

// ---- graph.link.check.v1 ----------------------------------------------------

// Read-only: which of the given links the editor would make only through a conversion node. Each end names the
// action key of its node and a pin; the pins are those of the palette's template node of that key (what a node
// placed from the palette has before links and settings change it). Links whose ends have no template, no such
// pin or a wildcard pin are not judged. Result: conversions [{index, ...FConversionNeed}], checked (links judged).
bool HandleLinkCheck(FCall& Call, FObjectBuilder& Result)
{
	FReader& Args = Call.Args;
	const FValue* Links = nullptr;
	const FValue* Describe = nullptr;
	const FValue* Defaults = nullptr;
	if (!Args.OnlyFields({TEXT("links"), TEXT("describe"), TEXT("defaults")}) || !Args.Array(TEXT("links"), Links, false)
		|| !Args.Array(TEXT("describe"), Describe, false) || !Args.Array(TEXT("defaults"), Defaults, false))
	{
		return false;
	}
	if (Links == nullptr && Describe == nullptr && Defaults == nullptr)
	{
		return Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("links, describe or defaults is required."), TEXT("links"));
	}
	struct FEnd
	{
		FString Key;
		FName Pin;
	};
	TArray<TPair<FEnd, FEnd>> Requested;
	TArray<FString> Keys;
	TArray<FString> Described;
	for (int32 Index = 0; Describe != nullptr && Index < Describe->Elements.Num(); ++Index)
	{
		if (Describe->Elements[Index].Kind != EAssetDocumentRuntimeValueKind::String)
		{
			return Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("describe lists action keys."), FString::Printf(TEXT("describe/%d"), Index));
		}
		Described.AddUnique(Describe->Elements[Index].StringValue);
		Keys.AddUnique(Describe->Elements[Index].StringValue);
	}
	for (int32 Index = 0; Links != nullptr && Index < Links->Elements.Num(); ++Index)
	{
		FReader Link(Links->Elements[Index], FString::Printf(TEXT("/links/%d"), Index), Call.Error);
		if (!Link.OnlyFields({TEXT("from"), TEXT("to")}))
		{
			return false;
		}
		FEnd Ends[2];
		const TCHAR* const Names[2] = {TEXT("from"), TEXT("to")};
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const FValue* EndValue = nullptr;
			if (!Link.Object(Names[Side], EndValue))
			{
				return false;
			}
			FReader End = Link.Child(Names[Side], *EndValue);
			FString PinName;
			if (!End.OnlyFields({TEXT("action_key"), TEXT("pin_name")}) || !End.String(TEXT("action_key"), Ends[Side].Key)
				|| !End.String(TEXT("pin_name"), PinName))
			{
				return false;
			}
			Ends[Side].Pin = FName(*PinName);
			Keys.AddUnique(Ends[Side].Key);
		}
		Requested.Emplace(Ends[0], Ends[1]);
	}
	struct FDefault
	{
		FEnd At;
		FString Kind;
		FString Value;
	};
	TArray<FDefault> RequestedDefaults;
	for (int32 Index = 0; Defaults != nullptr && Index < Defaults->Elements.Num(); ++Index)
	{
		FReader Entry(Defaults->Elements[Index], FString::Printf(TEXT("/defaults/%d"), Index), Call.Error);
		FDefault& Default = RequestedDefaults.AddDefaulted_GetRef();
		FString PinName;
		if (!Entry.OnlyFields({TEXT("action_key"), TEXT("pin_name"), TEXT("kind"), TEXT("value")})
			|| !Entry.String(TEXT("action_key"), Default.At.Key) || !Entry.String(TEXT("pin_name"), PinName)
			|| !Entry.String(TEXT("kind"), Default.Kind) || !Entry.String(TEXT("value"), Default.Value, false))
		{
			return false;
		}
		Default.At.Pin = FName(*PinName);
		Keys.AddUnique(Default.At.Key);
	}
	// The template cache places templates by the Blueprint's own top-level graphs: a K2 graph first (an Animation
	// Blueprint lists its AnimGraph, whose anim schema adapter places no palette templates, before its event graph).
	const FGraphEntry* Host = nullptr;
	for (const FGraphEntry& Entry : Call.Graphs.Graphs)
	{
		if (Entry.Graph != nullptr && Entry.Schema.IsValid() && Entry.Graph->GetOuter() == &Call.Asset
			&& !Entry.Graph->HasAnyFlags(RF_Transient) && (Host == nullptr || (Host->SchemaTag != TEXT("K2") && Entry.SchemaTag == TEXT("K2"))))
		{
			Host = &Entry;
		}
	}
	TMap<FString, UEdGraphNode*> Templates;
	if (Host != nullptr)
	{
		Host->Schema->GetTemplateNodes(Call.Asset, *Host->Graph, Keys, Templates);
	}
	auto FindPin = [&Templates](const FEnd& End, EEdGraphPinDirection Direction) -> UEdGraphPin*
	{
		UEdGraphNode* const* Node = Templates.Find(End.Key);
		UEdGraphPin* Pin = Node != nullptr && *Node != nullptr ? (*Node)->FindPin(End.Pin, Direction) : nullptr;
		// Object pins are left out: their types depend on the Blueprint (self) and on class picks.
		const FName Category = Pin != nullptr ? Pin->PinType.PinCategory : NAME_None;
		const bool bJudged = Pin != nullptr && !Pin->bHidden && Category != UEdGraphSchema_K2::PC_Wildcard
			&& Category != UEdGraphSchema_K2::PC_Object && Category != UEdGraphSchema_K2::PC_Class
			&& Category != UEdGraphSchema_K2::PC_SoftObject && Category != UEdGraphSchema_K2::PC_SoftClass
			&& Category != UEdGraphSchema_K2::PC_Interface;
		return bJudged ? Pin : nullptr;
	};
	TArray<FValue> Conversions;
	TArray<FValue> Refused;
	int32 Checked = 0;
	for (int32 Index = 0; Index < Requested.Num(); ++Index)
	{
		const TPair<FEnd, FEnd>& Link = Requested[Index];
		UEdGraphPin* Output = FindPin(Link.Key, EGPD_Output);
		UEdGraphPin* Input = FindPin(Link.Value, EGPD_Input);
		if (Output == nullptr || Input == nullptr)
		{
			// A link written input -> output (graph.pin.connect.v1 takes either order).
			Output = FindPin(Link.Value, EGPD_Output);
			Input = FindPin(Link.Key, EGPD_Input);
		}
		if (Output == nullptr || Input == nullptr || Output->GetOwningNode() == Input->GetOwningNode()
			|| Output->GetSchema() == nullptr)
		{
			continue;
		}
		++Checked;
		const FPinConnectionResponse Response = Output->GetSchema()->CanCreateConnection(Output, Input);
		if (Response.Response == CONNECT_RESPONSE_DISALLOW)
		{
			// graph.pin.connect.v1 refuses it (Graph.ConnectDisallowed).
			Refused.Add(FObjectBuilder().AddInt(TEXT("index"), Index).AddString(TEXT("from_pin"), Output->PinName.ToString())
				.AddString(TEXT("to_pin"), Input->PinName.ToString()).AddString(TEXT("from_type"), PinTypeText(*Output))
				.AddString(TEXT("to_type"), PinTypeText(*Input)).AddString(TEXT("reason"), Response.Message.ToString()).Build());
			continue;
		}
		if (Response.Response != CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE)
		{
			continue;
		}
		const FConversionNeed Need(Host->Schema.Get(), *Output, *Input, Response);
		FObjectBuilder Entry;
		Entry.AddInt(TEXT("index"), Index).AddString(TEXT("from_pin"), Output->PinName.ToString())
			.AddString(TEXT("to_pin"), Input->PinName.ToString());
		Need.AddTo(Entry);
		Conversions.Add(Entry.Build());
	}
	Result.Add(TEXT("conversions"), MakeArray(MoveTemp(Conversions))).Add(TEXT("refused"), MakeArray(MoveTemp(Refused)))
		.AddInt(TEXT("checked"), Checked);
	// defaults: graph.pin.default.set.v1's check, on the palette template's pin. Pins whose type the node takes
	// from its links (wildcards) or from the Blueprint (object pins given an object) are left to Apply.
	TArray<FValue> InvalidDefaults;
	for (int32 Index = 0; Index < RequestedDefaults.Num(); ++Index)
	{
		const FDefault& Default = RequestedDefaults[Index];
		UEdGraphNode* const* Template = Templates.Find(Default.At.Key);
		UEdGraphPin* Pin = Template != nullptr && *Template != nullptr ? (*Template)->FindPin(Default.At.Pin) : nullptr;
		if (Pin == nullptr || Pin->GetSchema() == nullptr || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard
			|| (Default.Kind != TEXT("value") && Default.Kind != TEXT("text") && Default.Kind != TEXT("object")))
		{
			continue;
		}
		FObjectBuilder Entry;
		Entry.AddInt(TEXT("index"), Index).AddString(TEXT("pin_name"), Pin->PinName.ToString())
			.AddString(TEXT("code"), TEXT("Graph.PinDefaultInvalid"));
		if (Pin->Direction != EGPD_Input)
		{
			InvalidDefaults.Add(Entry.AddString(TEXT("message"), TEXT("Only input pins have default values.")).Build());
			continue;
		}
		const FName Category = Pin->PinType.PinCategory;
		if (Default.Kind == TEXT("object") && (Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Class
			|| Category == UEdGraphSchema_K2::PC_SoftObject || Category == UEdGraphSchema_K2::PC_SoftClass
			|| Category == UEdGraphSchema_K2::PC_Interface) && !Pin->PinType.IsContainer() && !Pin->PinType.bIsReference)
		{
			continue;
		}
		// Apply leaves a pin that already holds the value as it is.
		const bool bNumbers = Default.Kind == TEXT("value") && Default.Value.IsNumeric() && Pin->DefaultValue.IsNumeric();
		if (Default.Kind == TEXT("value") ? Pin->DefaultValue == Default.Value || (bNumbers && FCString::Atod(*Pin->DefaultValue) == FCString::Atod(*Default.Value))
			: Default.Kind == TEXT("text") ? Pin->DefaultTextValue.ToString() == Default.Value
			: (Pin->DefaultObject != nullptr ? Pin->DefaultObject->GetPathName() : FString()) == Default.Value)
		{
			continue;
		}
		const FPinDefaultCheck Check(*Pin, Default.Kind, Default.Value);
		if (Check.Invalid.IsEmpty())
		{
			continue;
		}
		Entry.AddString(TEXT("message"), Check.Message(*Pin));
		if (!Check.bUnresolvedObject)
		{
			Entry.AddString(TEXT("reason"), Check.Invalid);
			FPinDefaultLink(*Pin).AddTo(Entry);
		}
		InvalidDefaults.Add(Entry.Build());
	}
	Result.Add(TEXT("invalid_defaults"), MakeArray(MoveTemp(InvalidDefaults)));
	if (Described.IsEmpty())
	{
		return true;
	}
	// describe: whether each key places exactly one node here (as graph.node.spawn.v1 would resolve it),
	// and the pins of its palette template, so a document's keys and pin names are checked before Apply.
	TArray<FValue> Unresolved;
	TArray<FValue> Templated;
	TArray<FString> Known;
	bool bKnown = false;
	for (const FString& Key : Described)
	{
		FAssetDocumentGraphError KeyError;
		if (Host != nullptr && !Host->Schema->CheckActionKey(Call.Asset, *Host->Graph, Key, KeyError))
		{
			if (!bKnown)
			{
				bKnown = true;
				EnumerateKeys(Call, *Host, nullptr, nullptr, Known);
			}
			FObjectBuilder Entry;
			Entry.AddString(TEXT("key"), Key).AddString(TEXT("code"), KeyError.Code).AddString(TEXT("message"), KeyError.Message)
				.Add(TEXT("suggestions"), MakeStringArray(SuggestActionKeys(Key, Known)));
			Unresolved.Add(Entry.Build());
			continue;
		}
		UEdGraphNode* const* Template = Templates.Find(Key);
		if (Template == nullptr || *Template == nullptr)
		{
			continue;
		}
		TArray<FValue> Pins;
		for (const UEdGraphPin* Pin : (*Template)->Pins)
		{
			if (Pin == nullptr)
			{
				continue;
			}
			FObjectBuilder PinRow;
			PinRow.AddString(TEXT("name"), Pin->PinName.ToString())
				.AddString(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"))
				.AddString(TEXT("category"), Pin->PinType.PinCategory.ToString())
				.AddBool(TEXT("hidden"), Pin->bHidden);
			if (!Pin->PinFriendlyName.IsEmpty() && Pin->PinFriendlyName.ToString() != Pin->PinName.ToString())
			{
				PinRow.AddString(TEXT("display_name"), Pin->PinFriendlyName.ToString());
			}
			Pins.Add(PinRow.Build());
		}
		Templated.Add(FObjectBuilder()
			.AddString(TEXT("key"), Key)
			.AddBool(TEXT("can_add_pin"), Host->Schema->CanAddDynamicPin(**Template))
			.Add(TEXT("pins"), MakeArray(MoveTemp(Pins)))
			.Build());
	}
	Result.Add(TEXT("unresolved"), MakeArray(MoveTemp(Unresolved))).Add(TEXT("templates"), MakeArray(MoveTemp(Templated)));
	return true;
}

bool HandleBreak(FCall& Call, FObjectBuilder& Result)
{
	FReader& Args = Call.Args;
	UEdGraphPin* A = nullptr;
	UEdGraphPin* B = nullptr;
	if (!Args.OnlyFields({TEXT("from"), TEXT("to")}) || !ReadPin(Call, Args, TEXT("from"), A)
		|| !ReadPin(Call, Args, TEXT("to"), B, false))
	{
		return false;
	}
	UEdGraph* Graph = A->GetOwningNode()->GetGraph();
	TArray<const UEdGraphNode*> Touched = {A->GetOwningNode()};
	for (const UEdGraphPin* Other : A->LinkedTo)
	{
		Touched.AddUnique(Other->GetOwningNode());
	}
	const TSet<FLinkKey> LinksBefore = CaptureLinks(Touched);
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (B != nullptr)
	{
		if (A->LinkedTo.Contains(B))
		{
			Schema->BreakSinglePinLink(A, B);
		}
	}
	else
	{
		Schema->BreakPinLinks(*A, /*bSendsNodeNotifcation=*/true);
	}
	NotifyChanged(Call, Graph, false);
	Result.Add(TEXT("broken_links"), MakeArray(BrokenLinks(LinksBefore, CaptureLinks(Touched))));
	return true;
}

// ---- graph.pin.default.set.v1 -----------------------------------------------

bool HandlePinDefault(FCall& Call, FObjectBuilder& Result)
{
	FReader& Args = Call.Args;
	UEdGraphPin* Pin = nullptr;
	FString Kind;
	if (!Args.OnlyFields({TEXT("pin"), TEXT("kind"), TEXT("value")}) || !ReadPin(Call, Args, TEXT("pin"), Pin, true, true)
		|| !Args.String(TEXT("kind"), Kind))
	{
		return false;
	}
	UEdGraphNode* Node = Pin->GetOwningNode();
	const UEdGraphSchema* Schema = Node->GetSchema();
	if (Pin->Direction != EGPD_Input)
	{
		return Fail(Call, TEXT("Graph.PinDefaultInvalid"), TEXT("Only input pins have default values."), TEXTVIEW("/pin"));
	}
	if (Kind == TEXT("reset"))
	{
		if (Args.Has(TEXT("value")))
		{
			return Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("kind 'reset' takes no value."), TEXT("value"));
		}
		RawEdits::ConvertGhostNodeToRealNode(*Node);
		Schema->ResetPinToAutogeneratedDefaultValue(Pin, /*bCallModifyCallbacks=*/true);
	}
	else if (Kind == TEXT("value") || Kind == TEXT("text") || Kind == TEXT("object"))
	{
		FString Value;
		if (!Args.String(TEXT("value"), Value, Kind != TEXT("object")))
		{
			return false;
		}
		RawEdits::ConvertGhostNodeToRealNode(*Node);
		const FPinDefaultCheck Check(*Pin, Kind, Value);
		if (!Check.Invalid.IsEmpty())
		{
			Fail(Call, TEXT("Graph.PinDefaultInvalid"), Check.Message(*Pin), TEXTVIEW("/value"));
			if (!Check.bUnresolvedObject)
			{
				Call.Error.AddContextString(TEXT("reason"), Check.Invalid);
				FPinDefaultLink(*Pin).AddContext(Call.Error);
			}
			return false;
		}
		if (Kind == TEXT("value"))
		{
			Schema->TrySetDefaultValue(*Pin, Check.Value, /*bMarkAsModified=*/true);
		}
		else if (Kind == TEXT("text"))
		{
			Schema->TrySetDefaultText(*Pin, Check.Text, /*bMarkAsModified=*/true);
		}
		else
		{
			Schema->TrySetDefaultObject(*Pin, Check.Object, /*bMarkAsModified=*/true);
		}
	}
	else
	{
		return Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("kind must be value, object, text or reset."), TEXT("kind"));
	}
	NotifyChanged(Call, Node->GetGraph(), false);
	// Re-resolve: some nodes reconstruct on default changes.
	FReader PinReader = Args.Child(TEXT("pin"), *Args.Find(TEXT("pin")));
	UEdGraphPin* Stored = nullptr;
	if (!Call.Refresh() || !ResolvePinAddress(Call.Graphs, PinReader, Stored))
	{
		return false;
	}
	Result.AddString(TEXT("default_value"), Stored->DefaultValue)
		.AddString(TEXT("default_object"), ObjectPathOrEmpty(Stored->DefaultObject))
		.AddString(TEXT("default_text"), Stored->DefaultTextValue.ToString());
	return true;
}

// ---- graph.node.property.get.v1 / set.v1 ------------------------------------

bool ResolveContainer(FCall& Call, FReader& Args, UEdGraphNode*& OutNode, UObject*& OutContainer,
	const FGraphEntry** OutEntry = nullptr)
{
	const FGraphEntry* Entry = nullptr;
	FString Container = TEXT("node");
	if (!ResolveNodeGuid(Call.Graphs, Args, TEXT("node_guid"), OutNode, &Entry) || !Args.String(TEXT("container"), Container, false))
	{
		return false;
	}
	if (OutEntry != nullptr)
	{
		*OutEntry = Entry;
	}
	OutContainer = Entry->Schema.IsValid() ? Entry->Schema->ResolvePropertyContainer(*OutNode, Container) : nullptr;
	if (OutContainer == nullptr && Container == TEXT("node"))
	{
		OutContainer = OutNode;
	}
	if (OutContainer == nullptr)
	{
		return Args.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(
			TEXT("Node %s has no '%s' property container."), *OutNode->GetClass()->GetName(), *Container), TEXT("container"));
	}
	return true;
}

bool HandlePropertyGet(FCall& Call, FObjectBuilder& Result)
{
	FReader& Args = Call.Args;
	UEdGraphNode* Node = nullptr;
	UObject* Container = nullptr;
	FString Path;
	if (!Args.OnlyFields({TEXT("node_guid"), TEXT("container"), TEXT("property_path")})
		|| !ResolveContainer(Call, Args, Node, Container) || !Args.String(TEXT("property_path"), Path, false))
	{
		return false;
	}
	if (Path.IsEmpty())
	{
		Result.Add(TEXT("properties"), EncodeEditableProperties(*Container));
		return true;
	}
	FResolvedPropertyPath Resolved;
	FString Error;
	if (!ResolvePropertyPath(*Container, Path, Resolved, Error))
	{
		return Fail(Call, TEXT("Graph.InvalidArgument"), Error, TEXTVIEW("/property_path"));
	}
	Result.Add(TEXT("properties"), FObjectBuilder().Add(Path, EncodePropertyValue(*Resolved.Leaf, Resolved.LeafPtr)).Build());
	return true;
}

bool HandlePropertySet(FCall& Call, FObjectBuilder& Result)
{
	FReader& Args = Call.Args;
	UEdGraphNode* Node = nullptr;
	UObject* Container = nullptr;
	const FGraphEntry* Entry = nullptr;
	FString Path;
	if (!Args.OnlyFields({TEXT("node_guid"), TEXT("container"), TEXT("property_path"), TEXT("value")})
		|| !ResolveContainer(Call, Args, Node, Container, &Entry) || !Args.String(TEXT("property_path"), Path))
	{
		return false;
	}
	const FValue* Value = Args.Find(TEXT("value"));
	if (Value == nullptr)
	{
		return Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("Missing required field."), TEXT("value"));
	}
	FValue Mapped;
	if (Container == Node && Entry != nullptr && Entry->Schema.IsValid())
	{
		Mapped = *Value;
		if (!Entry->Schema->MapNodeSetting(*Node, Path, Mapped, Call.Error))
		{
			if (Call.Error.Code.IsEmpty())
			{
				Call.Error.Set(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Setting '%s' was rejected."), *Path), TEXT("/value"));
			}
			return false;
		}
		Value = &Mapped;
		// Adapter-owned node settings (contract §5): not plain reflected properties.
		bool bHandled = false;
		if (!Entry->Schema->SetNodeSetting(Call.Asset, *Node, Path, *Value, bHandled, Call.Error))
		{
			if (Call.Error.Code.IsEmpty())
			{
				Call.Error.Set(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Setting '%s' was rejected."), *Path), TEXT("/value"));
			}
			return false;
		}
		if (bHandled)
		{
			NotifyChanged(Call, Node->GetGraph(), true);
			Result.Add(TEXT("value"), *Value);
			return true;
		}
	}
	FResolvedPropertyPath Resolved;
	FString Error;
	if (!ResolvePropertyPath(*Container, Path, Resolved, Error))
	{
		return Fail(Call, TEXT("Graph.InvalidArgument"), Error, TEXTVIEW("/property_path"));
	}
	if (!IsPropertyEditable(*Resolved.TopLevel))
	{
		return Fail(Call, TEXT("Graph.PropertyNotEditable"), FString::Printf(
			TEXT("%s.%s is not an editable (CPF_Edit, non-EditConst) property."),
			*Container->GetClass()->GetName(), *Resolved.TopLevel->GetName()), TEXTVIEW("/property_path"));
	}
	RawEdits::ConvertGhostNodeToRealNode(*Node);
	// Details-panel edit: PreEditChange / write / PostEditChangeChainProperty. As in the
	// details panel the event names the edited leaf (Property) and its top-level member
	// (MemberProperty), so leaf-specific handlers (e.g. "Node.InstanceClass") run.
	FEditPropertyChain Chain;
	Chain.AddHead(Resolved.TopLevel);
	if (Resolved.Leaf != Resolved.TopLevel)
	{
		Chain.AddTail(Resolved.Leaf);
	}
	Chain.SetActiveMemberPropertyNode(Resolved.TopLevel);
	Chain.SetActivePropertyNode(Resolved.Leaf);
	Container->Modify();
	Container->PreEditChange(Chain);
	// The row handles and keys the property holds now are kept; a written one must be offered by the pickers.
	const TArray<FString> Kept = EditorPickerValues::CollectPicks(*Resolved.Leaf, Resolved.LeafPtr);
	const bool bDecoded = DecodePropertyValue(*Resolved.Leaf, Resolved.LeafPtr, *Value, Error);
	FPropertyChangedEvent Event(Resolved.Leaf, EPropertyChangeType::ValueSet);
	FPropertyChangedChainEvent ChainEvent(Chain, Event);
	Container->PostEditChangeChainProperty(ChainEvent);
	if (!bDecoded)
	{
		return Fail(Call, TEXT("Graph.InvalidArgument"), Error, TEXTVIEW("/value"));
	}
	EditorPickerValues::FInvalidPick Invalid;
	if (EditorPickerValues::FindInvalidPick(*Resolved.Leaf, Resolved.LeafPtr, Kept, Invalid))
	{
		return Fail(Call, *Invalid.Code, FString::Printf(TEXT("'%s'%s: %s"), *Resolved.Leaf->GetAuthoredName(), *Invalid.ValuePath,
			*Invalid.Message), FString(TEXT("/value")) + Invalid.ValuePath);
	}
	NotifyChanged(Call, Node->GetGraph(), true);
	Result.Add(TEXT("value"), EncodePropertyValue(*Resolved.Leaf, Resolved.LeafPtr));
	return true;
}

// ---- graph.node.layout.set.v1 / graph.node.rename.v1 ------------------------

bool HandleLayout(FCall& Call, FObjectBuilder& Result)
{
	FReader& Args = Call.Args;
	UEdGraphNode* Node = nullptr;
	TOptional<FVector2D> Position;
	TOptional<FIntPoint> Size;
	FString Comment;
	bool bBubble = false;
	if (!Args.OnlyFields({TEXT("node_guid"), TEXT("position"), TEXT("size"), TEXT("comment"), TEXT("comment_bubble_visible")})
		|| !ResolveNodeGuid(Call.Graphs, Args, TEXT("node_guid"), Node) || !ReadPosition(Args, Position) || !ReadSize(Args, Size)
		|| !Args.String(TEXT("comment"), Comment, false) || !Args.Bool(TEXT("comment_bubble_visible"), bBubble))
	{
		return false;
	}
	if (Size.IsSet() && !RawEdits::SetNodeSize(*Node, Size->X, Size->Y))
	{
		return Fail(Call, TEXT("Graph.NodeNotResizable"), FString::Printf(
			TEXT("%s cannot be resized: only a comment box has a Size."), *Node->GetNodeTitle(ENodeTitleType::ListView).ToString()),
			TEXTVIEW("/size"));
	}
	if (Position.IsSet())
	{
		RawEdits::SetNodePosition(*Node, FMath::RoundToInt(Position->X), FMath::RoundToInt(Position->Y));
	}
	if (Args.Has(TEXT("comment")))
	{
		RawEdits::SetNodeComment(*Node, Comment);
	}
	if (Args.Has(TEXT("comment_bubble_visible")))
	{
		RawEdits::SetCommentBubbleVisible(*Node, bBubble);
	}
	NotifyChanged(Call, Node->GetGraph(), false);
	return true;
}

bool HandleRename(FCall& Call, FObjectBuilder& Result)
{
	FReader& Args = Call.Args;
	UEdGraphNode* Node = nullptr;
	FString Name;
	if (!Args.OnlyFields({TEXT("node_guid"), TEXT("name")}) || !ResolveNodeGuid(Call.Graphs, Args, TEXT("node_guid"), Node)
		|| !Args.String(TEXT("name"), Name))
	{
		return false;
	}
	const FGraphEntry* RenameEntry = EntryOf(Call, *Node);
	if (RenameEntry != nullptr && RenameEntry->Schema.IsValid())
	{
		// Adapter-owned node identity (contract §5): e.g. a SoundCue node's sound node object name.
		bool bHandled = false;
		if (!RenameEntry->Schema->RenameNode(Call.Asset, *Node, Name, bHandled, Call.Error))
		{
			if (Call.Error.Code.IsEmpty())
			{
				Call.Error.Set(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Name '%s' was rejected."), *Name), TEXT("/name"));
			}
			return false;
		}
		if (bHandled)
		{
			NotifyChanged(Call, Node->GetGraph(), true);
			FAssetDocumentGraphNodeDescription Handled;
			RenameEntry->Schema->DescribeNode(*Node, Handled);
			const FValue* HandledName = FindField(Handled.Extras, TEXT("name"));
			Result.AddString(TEXT("name"), HandledName != nullptr ? HandledName->StringValue : Name);
			return true;
		}
	}
	if (!Node->GetCanRenameNode())
	{
		return Fail(Call, TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("Node class %s cannot be renamed."),
			*Node->GetClass()->GetName()), TEXTVIEW("/node_guid"));
	}
	if (TSharedPtr<INameValidatorInterface> Validator = Node->MakeNameValidator())
	{
		const EValidatorResult Valid = Validator->IsValid(Name);
		if (Valid != EValidatorResult::Ok && Valid != EValidatorResult::ExistingName)
		{
			return Fail(Call, TEXT("Graph.InvalidArgument"), INameValidatorInterface::GetErrorString(Name, Valid), TEXTVIEW("/name"));
		}
		if (Valid == EValidatorResult::ExistingName && Node->GetNodeTitle(ENodeTitleType::EditableTitle).ToString() != Name)
		{
			return Fail(Call, TEXT("Graph.InvalidArgument"), INameValidatorInterface::GetErrorString(Name, Valid), TEXTVIEW("/name"));
		}
	}
	// SGraphNode::OnRenameNode.
	Node->Modify();
	Node->OnRenameNode(Name);
	NotifyChanged(Call, Node->GetGraph(), true);
	FAssetDocumentGraphNodeDescription Description;
	if (const FGraphEntry* Entry = EntryOf(Call, *Node); Entry != nullptr && Entry->Schema.IsValid())
	{
		Entry->Schema->DescribeNode(*Node, Description);
	}
	const FValue* Authored = FindField(Description.Extras, TEXT("name"));
	Result.AddString(TEXT("name"), Authored != nullptr ? Authored->StringValue : Name);
	return true;
}

// ---- graph.node.user_pins.set.v1 / dynamic pins ------------------------------

bool HandleUserPins(FCall& Call, FObjectBuilder& Result)
{
	FReader& Args = Call.Args;
	UEdGraphNode* Node = nullptr;
	const FGraphEntry* Entry = nullptr;
	const FValue* PinArray = nullptr;
	if (!Args.OnlyFields({TEXT("node_guid"), TEXT("pins")}) || !ResolveNodeGuid(Call.Graphs, Args, TEXT("node_guid"), Node, &Entry)
		|| !Args.Array(TEXT("pins"), PinArray) || !RequireSchema(Call, *Entry))
	{
		return false;
	}
	TArray<FAssetDocumentGraphUserPin> Pins;
	for (int32 Index = 0; Index < PinArray->Elements.Num(); ++Index)
	{
		FReader PinReader(PinArray->Elements[Index], FString::Printf(TEXT("/pins/%d"), Index), Call.Error);
		FString Name;
		FString Direction;
		const FValue* Type = nullptr;
		FString Default;
		if (!PinReader.OnlyFields({TEXT("name"), TEXT("direction"), TEXT("type"), TEXT("default")})
			|| !PinReader.String(TEXT("name"), Name) || !PinReader.String(TEXT("direction"), Direction)
			|| !PinReader.Object(TEXT("type"), Type) || !PinReader.String(TEXT("default"), Default, false))
		{
			return false;
		}
		FAssetDocumentGraphUserPin& Pin = Pins.AddDefaulted_GetRef();
		Pin.Name = FName(*Name);
		if (!ParseDirection(Direction, Pin.Direction))
		{
			return PinReader.Fail(TEXT("Graph.InvalidArgument"), TEXT("direction must be 'input' or 'output' (the pin's direction on this node)."), TEXT("direction"));
		}
		FReader TypeReader = PinReader.Child(TEXT("type"), *Type);
		if (!ReadPinType(TypeReader, Pin.Type))
		{
			return false;
		}
		if (PinReader.Has(TEXT("default")))
		{
			Pin.DefaultValue = Default;
		}
	}
	UEdGraph* Graph = Node->GetGraph();
	if (!Entry->Schema->SetUserPins(*Node, Pins, Call.Error))
	{
		return false;
	}
	NotifyChanged(Call, Graph, true);
	if (!Call.Refresh())
	{
		return false;
	}
	Result.Add(TEXT("node"), SnapshotOf(Call, *Node));
	return true;
}

bool HandleDynamicPin(FCall& Call, FObjectBuilder& Result, bool bAdd)
{
	FReader& Args = Call.Args;
	UEdGraphNode* Node = nullptr;
	const FGraphEntry* Entry = nullptr;
	FString PinName;
	if (!Args.OnlyFields({TEXT("node_guid"), TEXT("pin_name")}) || !ResolveNodeGuid(Call.Graphs, Args, TEXT("node_guid"), Node, &Entry)
		|| !Args.String(TEXT("pin_name"), PinName, !bAdd) || !RequireSchema(Call, *Entry))
	{
		return false;
	}
	UEdGraph* Graph = Node->GetGraph();
	if (bAdd)
	{
		if (!Entry->Schema->AddDynamicPin(*Node, Call.Error))
		{
			return false;
		}
	}
	else
	{
		UEdGraphPin* Pin = Node->FindPin(FName(*PinName));
		if (Pin == nullptr)
		{
			return Fail(Call, TEXT("Graph.PinNotFound"), FString::Printf(TEXT("Node %s has no pin '%s'."),
				*FormatAssetDocumentGraphGuid(Node->NodeGuid), *PinName), TEXTVIEW("/pin_name"));
		}
		if (!Entry->Schema->RemoveDynamicPin(*Node, Pin, Call.Error))
		{
			return false;
		}
	}
	NotifyChanged(Call, Graph, true);
	if (!Call.Refresh())
	{
		return false;
	}
	Result.Add(TEXT("node"), SnapshotOf(Call, *Node));
	return true;
}

bool HandleDynamicPinAdd(FCall& Call, FObjectBuilder& Result)
{
	return HandleDynamicPin(Call, Result, true);
}

bool HandleDynamicPinRemove(FCall& Call, FObjectBuilder& Result)
{
	return HandleDynamicPin(Call, Result, false);
}

// graph.node.pin_type.set.v1 {node_guid, pin_name, type}: the type picked on the pin itself
// (DescribeNode extras "typed_pins"; K2: the Select node's Index type selector).
bool HandlePinTypeSet(FCall& Call, FObjectBuilder& Result)
{
	FReader& Args = Call.Args;
	UEdGraphNode* Node = nullptr;
	const FGraphEntry* Entry = nullptr;
	FString PinName;
	const FValue* TypeValue = nullptr;
	if (!Args.OnlyFields({TEXT("node_guid"), TEXT("pin_name"), TEXT("type")}) || !ResolveNodeGuid(Call.Graphs, Args, TEXT("node_guid"), Node, &Entry)
		|| !Args.String(TEXT("pin_name"), PinName) || !Args.Object(TEXT("type"), TypeValue) || !RequireSchema(Call, *Entry))
	{
		return false;
	}
	FEdGraphPinType Type;
	FReader TypeReader = Args.Child(TEXT("type"), *TypeValue);
	if (!ReadPinType(TypeReader, Type))
	{
		return false;
	}
	UEdGraphPin* Pin = Node->FindPin(FName(*PinName));
	if (Pin == nullptr)
	{
		return Fail(Call, TEXT("Graph.PinNotFound"), FString::Printf(TEXT("Node %s has no pin '%s'."),
			*FormatAssetDocumentGraphGuid(Node->NodeGuid), *PinName), TEXTVIEW("/pin_name"));
	}
	UEdGraph* Graph = Node->GetGraph();
	if (!Entry->Schema->SetPinType(*Node, *Pin, Type, Call.Error))
	{
		return false;
	}
	NotifyChanged(Call, Graph, true);
	if (!Call.Refresh())
	{
		return false;
	}
	Result.Add(TEXT("node"), SnapshotOf(Call, *Node));
	return true;
}

// ---- graph.create.v1 / remove.v1 / rename.v1 / function.flags.set.v1 --------

bool HandleCreateGraph(FCall& Call, FObjectBuilder& Result)
{
	FReader& Args = Call.Args;
	FString Role;
	FString Name;
	const FGraphEntry* Parent = nullptr;
	if (!Args.OnlyFields({TEXT("role"), TEXT("name"), TEXT("parent")}) || !Args.String(TEXT("role"), Role)
		|| !Args.String(TEXT("name"), Name) || !ReadGraph(Call, Args, TEXT("parent"), Parent, false))
	{
		return false;
	}
	UEdGraph* Graph = nullptr;
	if (!Call.Graphs.Host->CreateGraph(Call.Asset, Role, Name, Parent != nullptr ? Parent->Graph : nullptr, Graph, Call.Error))
	{
		return false;
	}
	if (!Call.Refresh())
	{
		return false;
	}
	const FGraphEntry* Created = Call.Graphs.FindEntry(Graph);
	Result.AddString(TEXT("graph_guid"), FormatAssetDocumentGraphGuid(Created != nullptr ? Created->Guid() : Graph->GraphGuid))
		.AddString(TEXT("path"), Created != nullptr ? Created->Path : Graph->GetName());
	return true;
}

bool HandleRemoveGraph(FCall& Call, FObjectBuilder& Result)
{
	FReader& Args = Call.Args;
	const FGraphEntry* Entry = nullptr;
	if (!Args.OnlyFields({TEXT("graph")}) || !ReadGraph(Call, Args, TEXT("graph"), Entry))
	{
		return false;
	}
	if (Entry->OwnerNode != nullptr)
	{
		return Fail(Call, TEXT("Graph.GraphOperationUnsupported"),
			TEXT("Node-owned subgraphs are removed with their owner node (graph.node.remove.v1)."), TEXTVIEW("/graph"));
	}
	const FAssetShape Before = CaptureShape(Call.Graphs);
	if (!Call.Graphs.Host->RemoveGraph(Call.Asset, *Entry->Graph, Call.Error) || !Call.Refresh())
	{
		return false;
	}
	const FAssetShape After = CaptureShape(Call.Graphs);
	Result.Add(TEXT("removed_graph_guids"), MakeArray(GuidValues(Difference(Before.Graphs, After.Graphs))));
	return true;
}

bool HandleRenameGraph(FCall& Call, FObjectBuilder& Result)
{
	FReader& Args = Call.Args;
	const FGraphEntry* Entry = nullptr;
	FString Name;
	if (!Args.OnlyFields({TEXT("graph"), TEXT("name")}) || !ReadGraph(Call, Args, TEXT("graph"), Entry)
		|| !Args.String(TEXT("name"), Name))
	{
		return false;
	}
	UEdGraph* Graph = Entry->Graph;
	if (!Call.Graphs.Host->RenameGraph(Call.Asset, *Graph, Name, Call.Error) || !Call.Refresh())
	{
		return false;
	}
	const FGraphEntry* Renamed = Call.Graphs.FindEntry(Graph);
	Result.AddString(TEXT("path"), Renamed != nullptr ? Renamed->Path : Graph->GetName());
	return true;
}

bool HandleFunctionFlags(FCall& Call, FObjectBuilder& Result)
{
	FReader& Args = Call.Args;
	const FGraphEntry* Entry = nullptr;
	const FValue* Flags = nullptr;
	if (!Args.OnlyFields({TEXT("graph"), TEXT("flags")}) || !ReadGraph(Call, Args, TEXT("graph"), Entry)
		|| !Args.Object(TEXT("flags"), Flags) || !RequireSchema(Call, *Entry))
	{
		return false;
	}
	FValue OutFlags;
	if (!Entry->Schema->SetFunctionFlags(Call.Asset, *Entry->Graph, *Flags, OutFlags, Call.Error))
	{
		return false;
	}
	Result.Add(TEXT("flags"), MoveTemp(OutFlags));
	return true;
}

// ---- graph.finalize.v1 ------------------------------------------------------

// graph.finalize.v1 {node_meta?: [{node_guid, id, action?}], compile?}: records each node's document Id and authored
// action key (RawEdits::SetNodeDocumentIdentity), then compiles. compile: false only records (a large graph's
// node_meta is sent in batches; the last one compiles).
bool HandleFinalize(FCall& Call, FObjectBuilder& Result)
{
	const FValue* NodeMeta = nullptr;
	bool bCompile = true;
	if (!Call.Args.OnlyFields({TEXT("node_meta"), TEXT("compile")}) || !Call.Args.Array(TEXT("node_meta"), NodeMeta, false)
		|| !Call.Args.Bool(TEXT("compile"), bCompile, false))
	{
		return false;
	}
	for (int32 Index = 0; NodeMeta != nullptr && Index < NodeMeta->Elements.Num(); ++Index)
	{
		FReader Item(NodeMeta->Elements[Index], FString::Printf(TEXT("/node_meta/%d"), Index), Call.Error);
		UEdGraphNode* Node = nullptr;
		FString Id;
		FString Action;
		if (!Item.OnlyFields({TEXT("node_guid"), TEXT("id"), TEXT("action")}) || !ResolveNodeGuid(Call.Graphs, Item, TEXT("node_guid"), Node)
			|| !Item.String(TEXT("id"), Id) || !Item.String(TEXT("action"), Action, false))
		{
			return false;
		}
		RawEdits::SetNodeDocumentIdentity(*Node, Id, Action);
	}
	if (!bCompile)
	{
		Result.AddString(TEXT("status"), TEXT("recorded")).Add(TEXT("messages"), MakeArray(TArray<FValue>()));
		return true;
	}
	FAssetDocumentGraphFinalizeResult Finalized;
	if (!Call.Graphs.Host->Finalize(Call.Asset, Finalized, Call.Error))
	{
		return false;
	}
	TArray<FValue> Messages;
	for (const FAssetDocumentGraphFinalizeMessage& Message : Finalized.Messages)
	{
		FObjectBuilder Entry;
		Entry.AddString(TEXT("severity"), Message.Severity).AddString(TEXT("message"), Message.Message);
		if (!Message.Identifier.IsEmpty())
		{
			Entry.AddString(TEXT("id"), Message.Identifier);
		}
		if (Message.NodeGuid.IsValid())
		{
			Entry.AddString(TEXT("node_guid"), FormatAssetDocumentGraphGuid(Message.NodeGuid));
		}
		if (!Message.PinName.IsEmpty())
		{
			Entry.AddString(TEXT("pin"), Message.PinName);
			if (Message.bOrphanedPin)
			{
				Entry.AddBool(TEXT("orphaned"), true);
			}
		}
		Messages.Add(Entry.Build());
	}
	if (Finalized.Status == TEXT("errors"))
	{
		TArray<FString> Lines;
		for (const FAssetDocumentGraphFinalizeMessage& Message : Finalized.Messages)
		{
			if (Message.Severity == TEXT("error"))
			{
				Lines.Add(Message.NodeGuid.IsValid()
					? FString::Printf(TEXT("[%s] %s"), *FormatAssetDocumentGraphGuid(Message.NodeGuid), *Message.Message)
					: Message.Message);
			}
		}
		Fail(Call, TEXT("Graph.FinalizeFailed"), TEXT("Finalize reported errors: ") + FString::Join(Lines, TEXT(" | ")));
		Call.Error.AddContext(TEXT("messages"), MakeArray(MoveTemp(Messages)));
		return false;
	}
	Result.AddString(TEXT("status"), Finalized.Status).Add(TEXT("messages"), MakeArray(MoveTemp(Messages)));
	return true;
}
} // namespace

TConstArrayView<FBinding> GetGraphPrimitiveBindings()
{
	static const FBinding Bindings[] = {
		{TEXT("graph.list.v1"), &HandleList},
		{TEXT("graph.query.v1"), &HandleQuery},
		{TEXT("graph.actions.enumerate.v1"), &HandleEnumerate},
		{TEXT("graph.node.spawn.v1"), &HandleSpawn},
		{TEXT("graph.node.remove.v1"), &HandleRemoveNode},
		{TEXT("graph.pin.connect.v1"), &HandleConnect},
		{TEXT("graph.pin.break.v1"), &HandleBreak},
		{TEXT("graph.link.check.v1"), &HandleLinkCheck},
		{TEXT("graph.pin.default.set.v1"), &HandlePinDefault},
		{TEXT("graph.node.property.get.v1"), &HandlePropertyGet},
		{TEXT("graph.node.property.set.v1"), &HandlePropertySet},
		{TEXT("graph.node.layout.set.v1"), &HandleLayout},
		{TEXT("graph.node.rename.v1"), &HandleRename},
		{TEXT("graph.node.user_pins.set.v1"), &HandleUserPins},
		{TEXT("graph.node.dynamic_pin.add.v1"), &HandleDynamicPinAdd},
		{TEXT("graph.node.dynamic_pin.remove.v1"), &HandleDynamicPinRemove},
		{TEXT("graph.node.pin_type.set.v1"), &HandlePinTypeSet},
		{TEXT("graph.create.v1"), &HandleCreateGraph},
		{TEXT("graph.remove.v1"), &HandleRemoveGraph},
		{TEXT("graph.rename.v1"), &HandleRenameGraph},
		{TEXT("graph.finalize.v1"), &HandleFinalize},
		{TEXT("graph.function.flags.set.v1"), &HandleFunctionFlags},
	};
	return Bindings;
}

// ---- suggestions ------------------------------------------------------------

namespace
{
TArray<FString> Tokens(const FString& Text)
{
	TArray<FString> Out;
	FString Current;
	auto Flush = [&]()
	{
		if (!Current.IsEmpty())
		{
			const FString Lower = Current.ToLower();
			if (Lower != TEXT("k2") && Lower != TEXT("script") && Lower != TEXT("engine") && Lower != TEXT("game"))
			{
				Out.AddUnique(Lower);
			}
			Current.Reset();
		}
	};
	for (const TCHAR Char : Text)
	{
		if (FChar::IsAlnum(Char) || Char == TEXT('_'))
		{
			Current.AppendChar(Char);
		}
		else
		{
			Flush();
		}
	}
	Flush();
	return Out;
}

int32 EditDistance(const FString& A, const FString& B)
{
	TArray<int32> Previous;
	TArray<int32> Current;
	Previous.SetNumUninitialized(B.Len() + 1);
	Current.SetNumUninitialized(B.Len() + 1);
	for (int32 J = 0; J <= B.Len(); ++J)
	{
		Previous[J] = J;
	}
	for (int32 I = 1; I <= A.Len(); ++I)
	{
		Current[0] = I;
		for (int32 J = 1; J <= B.Len(); ++J)
		{
			const int32 Cost = A[I - 1] == B[J - 1] ? 0 : 1;
			Current[J] = FMath::Min3(Previous[J] + 1, Current[J - 1] + 1, Previous[J - 1] + Cost);
		}
		Swap(Previous, Current);
	}
	return Previous[B.Len()];
}
} // namespace

TArray<FString> SuggestActionKeys(const FString& Unknown, TConstArrayView<FString> Candidates, int32 Max)
{
	const TArray<FString> Wanted = Tokens(Unknown);
	// The member / node name (last segment) matters most: "...:PrintStrin" should
	// suggest PrintString before other members of the same owner.
	int32 LastSeparator = INDEX_NONE;
	Unknown.FindLastChar(TEXT(':'), LastSeparator);
	const TArray<FString> Leaf = Tokens(LastSeparator == INDEX_NONE ? Unknown : Unknown.Mid(LastSeparator + 1));
	struct FScored
	{
		int32 Score = 0;
		const FString* Key = nullptr;
	};
	TArray<FScored> Scored;
	for (const FString& Candidate : Candidates)
	{
		const TArray<FString> Have = Tokens(Candidate);
		int32 Score = 0;
		for (const FString& Token : Wanted)
		{
			int32 Best = 0;
			for (const FString& Other : Have)
			{
				if (Other == Token)
				{
					Best = FMath::Max(Best, 4);
				}
				else if (Token.Len() >= 3 && Other.Len() >= 3 && (Other.Contains(Token) || Token.Contains(Other)))
				{
					Best = FMath::Max(Best, 2);
				}
				else if (Token.Len() >= 4 && EditDistance(Token, Other) <= FMath::Max(1, Token.Len() / 4))
				{
					Best = FMath::Max(Best, 1);
				}
			}
			Score += Leaf.Contains(Token) ? Best * 3 : Best;
		}
		if (Score > 0)
		{
			Scored.Add({Score, &Candidate});
		}
	}
	Scored.Sort([](const FScored& A, const FScored& B)
	{
		if (A.Score != B.Score) return A.Score > B.Score;
		if (A.Key->Len() != B.Key->Len()) return A.Key->Len() < B.Key->Len();
		return *A.Key < *B.Key;
	});
	TArray<FString> Out;
	for (int32 Index = 0; Index < Scored.Num() && Index < Max; ++Index)
	{
		Out.Add(*Scored[Index].Key);
	}
	return Out;
}
} // namespace UE::AssetDocument::Graph
