// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// ai.env_query.edit.v1 and ai.env_query.finalize.v1: the Environment Query editor's own operations.
//   add_option     -> the generator palette: UEdGraphSchema_EnvironmentQuery::GetGraphContextActions (Root's output as
//                     FromPin when wired) -> FAISchemaAction_NewNode::PerformAction (Option node, UEnvQueryOption and
//                     generator in the query, UpdateNodeVersion, AutowireNewNode), then the node moved to its position;
//   add_test       -> Add Test...: the schema's sub node classes -> FAISchemaAction_NewSubNode::PerformAction
//                     (UAIGraphNode::AddSubNode: the test in the query, UpdateNodeVersion, UpdateAsset);
//   remove         -> FAIGraphEditor::DeleteSelectedNodes (Graph / node Modify, CanUserDeleteNode, DestroyNode);
//   move_test      -> SGraphNodeAI::OnDrop (RemoveSubNode, InsertSubNodeAt, OnSubNodeDropped);
//   set_enabled    -> SGraphNode_EnvironmentQuery::OnTestToggleChanged (bTestEnabled, UpdateAsset);
//   set_property   -> the Details panel of the selected node's generator / test (EditDefaultsOnly shown editable);
//   connect        -> wire drag from Root / Break Link (TryCreateConnection, BreakSinglePinLink);
//   layout         -> node move, comment bubble;
//   add_comment / comment / remove_comment -> Create Comment (FAISchemaAction_AddComment), its Details, resize, Delete;
//   rename         -> new objects take the document key (a stale orphan holding the name is moved aside); an option /
//                     test graph node also carries it (NodeKey), as the editor's Undo renames the instances.
// finalize does what opening / saving in the editor does: Initialize (SpawnMissingNodes, CalculateAllWeights,
// UpdateAsset: Options rebuilt left to right from the wired options, enabled tests in order).

#include "EnvQuery/AssetDocumentEnvQueryPrimitives.h"

#include "AIGraph.h"
#include "AIGraphNode.h"
#include "AIGraphSchema.h"
#include "AIGraphTypes.h"
#include "AssetDocumentEditorObjectProperties.h"
#include "AssetDocumentGraphValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphNode_Comment.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectHash.h"

#define LOCTEXT_NAMESPACE "AssetDocumentEnvQuery"

namespace UE::AssetDocument::Graph::EnvQuery
{
namespace
{
namespace Props = UE::AssetDocument::EditorObjectProperties;

struct FEditContext
{
	FCall& Call;
	UEnvQuery& Query;
	UAIGraph& Graph;
	UAIGraphSchema& Schema;
};

bool ReadPair(FReader& Op, FStringView Key, int32& OutX, int32& OutY, bool bRequired)
{
	const FValue* Pair = nullptr;
	if (!Op.Array(Key, Pair, bRequired))
	{
		return false;
	}
	if (Pair == nullptr)
	{
		return true;
	}
	if (Pair->Elements.Num() != 2)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("A position / size is [x, y]."), Key);
	}
	double Values[2] = {0.0, 0.0};
	for (int32 Index = 0; Index < 2; ++Index)
	{
		const FValue& Element = Pair->Elements[Index];
		if (Element.Kind == EAssetDocumentRuntimeValueKind::Integer)
		{
			Values[Index] = static_cast<double>(Element.IntegerValue);
		}
		else if (Element.Kind == EAssetDocumentRuntimeValueKind::Number)
		{
			Values[Index] = Element.NumberValue;
		}
		else
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("A position / size is [x, y] (numbers)."), Key);
		}
	}
	OutX = FMath::RoundToInt32(Values[0]);
	OutY = FMath::RoundToInt32(Values[1]);
	return true;
}

// The transient ParentNode links of the test sub nodes (UpdateAsset sets them for wired options only).
void LinkParents(UAIGraph& Graph)
{
	for (UAIGraphNode* Option : GetOptionNodes(Graph))
	{
		for (UAIGraphNode* Sub : Option->SubNodes)
		{
			if (Sub != nullptr)
			{
				Sub->ParentNode = Option;
			}
		}
	}
}

// True when Object is part of the query's live graph content (a node, an option / generator / test instance).
bool IsLive(const UAIGraph& Graph, const UObject* Object)
{
	if (Object == &Graph)
	{
		return true;
	}
	for (const UEdGraphNode* Node : Graph.Nodes)
	{
		if (Node == Object)
		{
			return true;
		}
		const UAIGraphNode* AINode = Cast<UAIGraphNode>(Node);
		if (AINode == nullptr)
		{
			continue;
		}
		if (AINode->NodeInstance == Object)
		{
			return true;
		}
		if (const UEnvQueryOption* Option = Cast<UEnvQueryOption>(AINode->NodeInstance); Option != nullptr && Option->Generator == Object)
		{
			return true;
		}
		for (const UAIGraphNode* Sub : AINode->SubNodes)
		{
			if (Sub != nullptr && (Sub == Object || Sub->NodeInstance == Object))
			{
				return true;
			}
		}
	}
	return false;
}

} // namespace

namespace AssetDocumentEnvQueryEditLocal
{
// Moves Holder (an orphan, or an instance whose key lives on its graph node) off the name Base.
void MoveAside(UObject& Holder, const FString& Base)
{
	const FName Aside = MakeUniqueObjectName(Holder.GetOuter(), Holder.GetClass(), FName(*(Base + TEXT("_Removed"))));
	Holder.Rename(*Aside.ToString(), nullptr, REN_DontCreateRedirectors);
}

// Names Node (an option / test graph node) after its key: the editor's Undo / Redo renames node instances
// (ResetNodeOwner) but restores graph node names, so NodeKey reads the key from here. When a live comment or node holds
// the name, Node keeps (or gets back) a class name and its key stays its instance's name.
void StampKeyName(const UAIGraph& Graph, UAIGraphNode& Node, const FString& Key)
{
	if (Node.GetName() == Key)
	{
		return;
	}
	UObject* Outer = Node.GetOuter();
	UObject* Holder = StaticFindObjectFast(UObject::StaticClass(), Outer, FName(*Key), EFindObjectFlags::None);
	if (Holder != nullptr && Holder != &Node)
	{
		if (IsLive(Graph, Holder))
		{
			if (HasKeyName(Node))
			{
				Node.Rename(*MakeUniqueObjectName(Outer, Node.GetClass()).ToString(), nullptr, REN_DontCreateRedirectors);
			}
			return;
		}
		MoveAside(*Holder, Key);
	}
	if (Node.Rename(*Key, nullptr, REN_Test | REN_DontCreateRedirectors))
	{
		Node.Rename(*Key, nullptr, REN_DontCreateRedirectors);
	}
}

// The option / test node other than Node whose key is Key, if any.
const UAIGraphNode* FindKeyHolder(const UAIGraph& Graph, const UAIGraphNode& Node, const FString& Key)
{
	for (const UAIGraphNode* Option : GetOptionNodes(Graph))
	{
		if (Option != &Node && Option->NodeInstance != nullptr && NodeKey(*Option).Equals(Key, ESearchCase::IgnoreCase))
		{
			return Option;
		}
		for (const UAIGraphNode* Sub : Option->SubNodes)
		{
			if (Sub != nullptr && Sub != &Node && Sub->NodeInstance != nullptr && NodeKey(*Sub).Equals(Key, ESearchCase::IgnoreCase))
			{
				return Sub;
			}
		}
	}
	return nullptr;
}

// True when Object is the instance of a live option / test node whose key its graph node carries.
bool IsKeyedInstance(const UAIGraph& Graph, const UObject* Object)
{
	for (const UAIGraphNode* Option : GetOptionNodes(Graph))
	{
		if (Option->NodeInstance == Object)
		{
			return HasKeyName(*Option);
		}
		for (const UAIGraphNode* Sub : Option->SubNodes)
		{
			if (Sub != nullptr && Sub->NodeInstance == Object)
			{
				return HasKeyName(*Sub);
			}
		}
	}
	return false;
}

// The option / test node Node takes the document key Key: its instance is named Key (a stale orphan, or the instance of
// a node keyed by its graph node, is moved aside) and its graph node carries Key.
bool RenameKey(const UAIGraph& Graph, FReader& Op, UAIGraphNode& Node, const FString& Key)
{
	UObject* Instance = Node.NodeInstance;
	if (Key.IsEmpty() || Instance == nullptr)
	{
		return true;
	}
	if (FindKeyHolder(Graph, Node, Key) != nullptr)
	{
		return Op.Fail(TEXT("EnvQuery.DuplicateKey"), FString::Printf(TEXT("'%s' already names another object of the query."), *Key), TEXT("name"));
	}
	if (Instance->GetName() != Key)
	{
		UObject* Outer = Instance->GetOuter();
		if (UObject* Holder = StaticFindObjectFast(UObject::StaticClass(), Outer, FName(*Key), EFindObjectFlags::None); Holder != nullptr && Holder != Instance)
		{
			if (IsLive(Graph, Holder) && !IsKeyedInstance(Graph, Holder))
			{
				return Op.Fail(TEXT("EnvQuery.DuplicateKey"), FString::Printf(TEXT("'%s' already names another object of the query."), *Key), TEXT("name"));
			}
			MoveAside(*Holder, Key);
		}
		if (!Instance->Rename(*Key, nullptr, REN_Test | REN_DontCreateRedirectors))
		{
			return Op.Fail(TEXT("EnvQuery.InvalidKey"), FString::Printf(TEXT("'%s' is not a valid object name here."), *Key), TEXT("name"));
		}
		Instance->Rename(*Key, nullptr, REN_DontCreateRedirectors);
	}
	StampKeyName(Graph, Node, Key);
	return true;
}
} // namespace AssetDocumentEnvQueryEditLocal

namespace
{
bool RenameTo(FEditContext& Context, FReader& Op, UObject& Object, const FString& Key)
{
	if (Key.IsEmpty() || Object.GetName() == Key)
	{
		return true;
	}
	UObject* Outer = Object.GetOuter();
	if (UObject* Holder = StaticFindObjectFast(UObject::StaticClass(), Outer, FName(*Key), EFindObjectFlags::None))
	{
		UAIGraphNode* KeyedNode = Cast<UAIGraphNode>(Holder);
		if (Holder != &Object && KeyedNode != nullptr && HasKeyName(*KeyedNode) && IsLive(Context.Graph, Holder))
		{
			// An option / test graph node carrying its key: it gives the name up, its instance keeps the key.
			KeyedNode->Rename(*MakeUniqueObjectName(Outer, KeyedNode->GetClass()).ToString(), nullptr, REN_DontCreateRedirectors);
		}
		else if (Holder != &Object && IsLive(Context.Graph, Holder))
		{
			return Op.Fail(TEXT("EnvQuery.DuplicateKey"), FString::Printf(TEXT("'%s' already names another object of the query."), *Key), TEXT("name"));
		}
		else if (Holder != &Object)
		{
			// An orphan the editor's next UpdateAsset would move to the transient package (a deleted node's instance).
			const FName Aside = MakeUniqueObjectName(Outer, Holder->GetClass(), FName(*(Key + TEXT("_Removed"))));
			Holder->Rename(*Aside.ToString(), nullptr, REN_DontCreateRedirectors);
		}
	}
	if (!Object.Rename(*Key, nullptr, REN_Test | REN_DontCreateRedirectors))
	{
		return Op.Fail(TEXT("EnvQuery.InvalidKey"), FString::Printf(TEXT("'%s' is not a valid object name here."), *Key), TEXT("name"));
	}
	Object.Rename(*Key, nullptr, REN_DontCreateRedirectors);
	return true;
}

UClass* RequireClass(FEditContext& Context, FReader& Op, ENodeKind Kind)
{
	FString Text;
	if (!Op.String(TEXT("class"), Text))
	{
		return nullptr;
	}
	TArray<FString> Candidates;
	UClass* Class = ResolveClass(Text, Kind, Candidates);
	if (Class == nullptr)
	{
		Op.Fail(TEXT("EnvQuery.UnknownClass"), FString::Printf(TEXT("'%s' is not a %s class of the Environment Query editor%s%s."), *Text,
			Kind == ENodeKind::Generator ? TEXT("generator") : TEXT("test"), Candidates.IsEmpty() ? TEXT("") : TEXT("; did you mean: "),
			*FString::Join(Candidates, TEXT(", "))), TEXT("class"));
		Context.Call.Error.AddContext(TEXT("candidates"), MakeStringArray(Candidates));
	}
	return Class;
}

UAIGraphNode* RequireOption(FEditContext& Context, FReader& Op, FStringView Key)
{
	FString Name;
	if (!Op.String(Key, Name))
	{
		return nullptr;
	}
	UAIGraphNode* Node = FindOptionNode(Context.Graph, Name);
	if (Node == nullptr)
	{
		Op.Fail(TEXT("EnvQuery.OptionNotFound"), FString::Printf(TEXT("The query has no option '%s'."), *Name), Key);
	}
	return Node;
}

UAIGraphNode* RequireTest(FEditContext& Context, FReader& Op)
{
	FString Name;
	if (!Op.String(TEXT("name"), Name))
	{
		return nullptr;
	}
	UAIGraphNode* Node = FindTestNode(Context.Graph, Name);
	if (Node == nullptr)
	{
		Op.Fail(TEXT("EnvQuery.TestNotFound"), FString::Printf(TEXT("The query has no test '%s'."), *Name), TEXT("name"));
	}
	return Node;
}

UEdGraphNode* RequireNode(FEditContext& Context, FReader& Op, bool bAllowRoot)
{
	FString Name;
	if (!Op.String(TEXT("name"), Name, !bAllowRoot))
	{
		return nullptr;
	}
	if (Name.IsEmpty() && bAllowRoot)
	{
		UEdGraphNode* Root = FindRoot(Context.Graph);
		if (Root == nullptr)
		{
			Op.Fail(TEXT("EnvQuery.InvalidGraph"), TEXT("The query graph has no Root node."), TEXT("name"));
		}
		return Root;
	}
	if (UAIGraphNode* Option = FindOptionNode(Context.Graph, Name))
	{
		return Option;
	}
	if (UAIGraphNode* Test = FindTestNode(Context.Graph, Name))
	{
		return Test;
	}
	if (UEdGraphNode_Comment* Comment = FindComment(Context.Graph, Name))
	{
		return Comment;
	}
	Op.Fail(TEXT("EnvQuery.NodeNotFound"), FString::Printf(TEXT("The query has no option, test or comment '%s'."), *Name), TEXT("name"));
	return nullptr;
}

void Move(UEdGraphNode& Node, int32 X, int32 Y)
{
	if (Node.NodePosX != X || Node.NodePosY != Y)
	{
		Node.Modify();
		Node.NodePosX = X;
		Node.NodePosY = Y;
	}
}

bool EditDetails(FReader& Op, UObject& Owner, const FString& Property, const FValue& Value, bool bTemplate, const FString& Field)
{
	Props::FReferencePolicy Policy;
	Policy.bTemplateSemantics = bTemplate;
	Props::FPreparedWrite Prepared;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	FString Hidden;
	if (bTemplate && FindHiddenMember(Owner, Property, Value, Hidden))
	{
		return Op.Fail(TEXT("EnvQuery.UnknownProperty"),
			FString::Printf(TEXT("'%s%s' is not shown in the Details panel (set by the node class or derived from TraceChannel)."), *Property, *Hidden),
			Field + Hidden);
	}
	if (Props::Decode(Owner, Property, Value, Policy, Prepared, Diagnostics) && Props::Commit(Prepared, Diagnostics))
	{
		if (bTemplate)
		{
			SyncTraceChannels(Owner);
		}
		return true;
	}
	const FAssetDocumentRuntimeDiagnostic Diagnostic = Diagnostics.IsEmpty() ? FAssetDocumentRuntimeDiagnostic() : Diagnostics[0];
	FString Inner = Diagnostic.Path;
	const FString Prefix = TEXT("/") + Property;
	if (Inner.StartsWith(Prefix))
	{
		Inner.RightChopInline(Prefix.Len());
	}
	return Op.Fail(Diagnostic.Code.IsEmpty() ? TEXT("EnvQuery.InvalidValue") : *Diagnostic.Code,
		Diagnostic.Message.IsEmpty() ? FString::Printf(TEXT("'%s' could not be written."), *Property) : Diagnostic.Message,
		Field + Inner);
}

bool AddOption(FEditContext& Context, FReader& Op, FString& OutName)
{
	FString Key;
	int32 X = 0;
	int32 Y = 0;
	bool bConnect = true;
	if (!Op.OnlyFields({TEXT("op"), TEXT("class"), TEXT("name"), TEXT("position"), TEXT("connect")}) || !Op.String(TEXT("name"), Key, false)
		|| !ReadPair(Op, TEXT("position"), X, Y, true) || !Op.Bool(TEXT("connect"), bConnect, false))
	{
		return false;
	}
	UClass* Class = RequireClass(Context, Op, ENodeKind::Generator);
	if (Class == nullptr)
	{
		return false;
	}
	UEdGraphPin* From = bConnect ? RootOutput(Context.Graph) : nullptr;
	// UEdGraphSchema_EnvironmentQuery::GetGraphContextActions: an Option node template per palette class, dropped by
	// its FAISchemaAction_NewNode (which renames the template into the graph).
	FGraphNodeClassData Data;
	if (!FindClassData(*Class, ENodeKind::Generator, Data) || OptionNodeClass() == nullptr)
	{
		return Op.Fail(TEXT("EnvQuery.UnknownClass"), FString::Printf(TEXT("The generator palette has no %s."), *Class->GetName()), TEXT("class"));
	}
	UAIGraphNode* Template = NewObject<UAIGraphNode>(&Context.Graph, OptionNodeClass());
	Template->ClassData = Data;
	FAISchemaAction_NewNode NewNodeAction;
	NewNodeAction.NodeTemplate = Template;
	FAISchemaAction_NewNode* Action = &NewNodeAction;
	UAIGraphNode* Node = Cast<UAIGraphNode>(Action->PerformAction(&Context.Graph, From, FVector2f(static_cast<float>(X), static_cast<float>(Y)), false));
	UEnvQueryOption* Option = Node != nullptr ? GetOption(*Node) : nullptr;
	if (Option == nullptr || Option->Generator == nullptr)
	{
		return Op.Fail(TEXT("EnvQuery.EditFailed"), TEXT("The palette action created no option."), TEXT("class"));
	}
	Move(*Node, X, Y);
	using namespace AssetDocumentEnvQueryEditLocal;
	if (!RenameKey(Context.Graph, Op, *Node, Key))
	{
		return false;
	}
	OutName = NodeKey(*Node);
	return true;
}

bool MoveTest(FEditContext& Context, UAIGraphNode& Test, UAIGraphNode& Target, int32 Index)
{
	if (Test.ParentNode != nullptr)
	{
		Test.ParentNode->RemoveSubNode(&Test);
	}
	Test.Modify();
	Test.ParentNode = &Target;
	Target.Modify();
	Target.InsertSubNodeAt(&Test, FMath::Clamp(Index, 0, Target.SubNodes.Num()));
	Context.Graph.OnSubNodeDropped();
	return true;
}

bool AddTest(FEditContext& Context, FReader& Op, FString& OutName)
{
	FString Key;
	int64 Index = -1;
	if (!Op.OnlyFields({TEXT("op"), TEXT("option"), TEXT("class"), TEXT("name"), TEXT("index")}) || !Op.String(TEXT("name"), Key, false)
		|| !Op.Int(TEXT("index"), Index, false))
	{
		return false;
	}
	UAIGraphNode* Parent = RequireOption(Context, Op, TEXT("option"));
	UClass* Class = Parent != nullptr ? RequireClass(Context, Op, ENodeKind::Test) : nullptr;
	if (Class == nullptr)
	{
		return false;
	}
	// UEdGraphSchema_EnvironmentQuery::GetSubNodeClasses: the class cache's tests, as Test graph nodes.
	FGraphNodeClassData ClassData;
	UClass* GraphNodeClass = TestNodeClass();
	FGraphNodeClassData* Data = FindClassData(*Class, ENodeKind::Test, ClassData) ? &ClassData : nullptr;
	if (Data == nullptr || GraphNodeClass == nullptr)
	{
		return Op.Fail(TEXT("EnvQuery.UnknownClass"), FString::Printf(TEXT("Add Test offers no %s."), *Class->GetName()), TEXT("class"));
	}
	// UAIGraphSchema::GetGraphNodeContextActions: one template node per class, then the chosen action.
	UAIGraphNode* Template = NewObject<UAIGraphNode>(&Context.Graph, GraphNodeClass);
	Template->ClassData = *Data;
	FAISchemaAction_NewSubNode Action;
	Action.ParentNode = Parent;
	Action.NodeTemplate = Template;
	Action.PerformAction(&Context.Graph, nullptr, FVector2f::ZeroVector, false);
	UEnvQueryTest* Test = Cast<UEnvQueryTest>(Template->NodeInstance);
	if (Test == nullptr || !Parent->SubNodes.Contains(Template))
	{
		return Op.Fail(TEXT("EnvQuery.EditFailed"), TEXT("Add Test created no test."), TEXT("class"));
	}
	if (Index >= 0 && Index < Parent->SubNodes.Num() - 1)
	{
		MoveTest(Context, *Template, *Parent, static_cast<int32>(Index));
	}
	using namespace AssetDocumentEnvQueryEditLocal;
	if (!RenameKey(Context.Graph, Op, *Template, Key))
	{
		return false;
	}
	OutName = NodeKey(*Template);
	return true;
}

bool Remove(FEditContext& Context, FReader& Op)
{
	if (!Op.OnlyFields({TEXT("op"), TEXT("name")}))
	{
		return false;
	}
	UEdGraphNode* Node = RequireNode(Context, Op, false);
	if (Node == nullptr)
	{
		return false;
	}
	if (!Node->CanUserDeleteNode())
	{
		return Op.Fail(TEXT("EnvQuery.NotDeletable"), TEXT("The editor does not delete this node."), TEXT("name"));
	}
	// FAIGraphEditor::DeleteSelectedNodes.
	const FScopedTransaction Transaction(LOCTEXT("Delete", "Delete"));
	Context.Graph.Modify();
	Node->Modify();
	Node->DestroyNode();
	return true;
}

bool EditOp(FEditContext& Context, FReader& Op, const FString& Kind, FString& OutName)
{
	if (Kind == TEXT("add_option"))
	{
		return AddOption(Context, Op, OutName);
	}
	if (Kind == TEXT("add_test"))
	{
		return AddTest(Context, Op, OutName);
	}
	if (Kind == TEXT("remove") || Kind == TEXT("remove_comment"))
	{
		return Remove(Context, Op);
	}
	if (Kind == TEXT("move_test"))
	{
		int64 Index = 0;
		if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("option"), TEXT("index")}) || !Op.Int(TEXT("index"), Index))
		{
			return false;
		}
		UAIGraphNode* Test = RequireTest(Context, Op);
		UAIGraphNode* Target = Test != nullptr ? RequireOption(Context, Op, TEXT("option")) : nullptr;
		if (Target == nullptr)
		{
			return false;
		}
		const FScopedTransaction Transaction(LOCTEXT("AddNodeTransaction", "Drag&Drop Node"));
		return MoveTest(Context, *Test, *Target, static_cast<int32>(Index));
	}
	if (Kind == TEXT("set_enabled"))
	{
		bool bEnabled = true;
		if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("enabled")}) || !Op.Bool(TEXT("enabled"), bEnabled, true))
		{
			return false;
		}
		UAIGraphNode* Test = RequireTest(Context, Op);
		FBoolProperty* Property = Test != nullptr ? CastField<FBoolProperty>(Test->GetClass()->FindPropertyByName(TEXT("bTestEnabled"))) : nullptr;
		if (Property == nullptr)
		{
			return Test == nullptr ? false : Op.Fail(TEXT("EnvQuery.EditFailed"), TEXT("The test node has no enable flag."), TEXT("name"));
		}
		Test->Modify();
		Property->SetPropertyValue_InContainer(Test, bEnabled);
		Context.Graph.UpdateAsset();
		return true;
	}
	if (Kind == TEXT("set_property"))
	{
		FString Name;
		FString Property;
		const FValue* Value = Op.Find(TEXT("value"));
		if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("property"), TEXT("value")}) || !Op.String(TEXT("name"), Name)
			|| !Op.String(TEXT("property"), Property))
		{
			return false;
		}
		if (Value == nullptr)
		{
			return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("value is required."), TEXT("value"));
		}
		UObject* Owner = nullptr;
		if (UAIGraphNode* Option = FindOptionNode(Context.Graph, Name))
		{
			Owner = GetOption(*Option) != nullptr ? GetOption(*Option)->Generator.Get() : nullptr;
		}
		else if (UAIGraphNode* Test = FindTestNode(Context.Graph, Name))
		{
			Owner = Test->NodeInstance;
		}
		if (Owner == nullptr)
		{
			return Op.Fail(TEXT("EnvQuery.NodeNotFound"), FString::Printf(TEXT("The query has no option or test '%s'."), *Name), TEXT("name"));
		}
		return EditDetails(Op, *Owner, Property, *Value, true, TEXT("value"));
	}
	if (Kind == TEXT("connect"))
	{
		bool bConnected = true;
		if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("connected")}) || !Op.Bool(TEXT("connected"), bConnected, true))
		{
			return false;
		}
		UAIGraphNode* Option = RequireOption(Context, Op, TEXT("name"));
		UEdGraphPin* RootOut = RootOutput(Context.Graph);
		UEdGraphPin* Input = Option != nullptr ? Option->GetInputPin() : nullptr;
		if (Input == nullptr || RootOut == nullptr)
		{
			return Option == nullptr ? false : Op.Fail(TEXT("EnvQuery.InvalidGraph"), TEXT("The option or Root has no pin."), TEXT("name"));
		}
		if (bConnected == IsConnected(*Option))
		{
			return true;
		}
		if (bConnected)
		{
			const FScopedTransaction Transaction(LOCTEXT("LinkPinsTransaction", "Create Pin Link"));
			if (!Context.Schema.TryCreateConnection(RootOut, Input))
			{
				return Op.Fail(TEXT("EnvQuery.ConnectDisallowed"), TEXT("The editor refuses this connection."), TEXT("name"));
			}
			return true;
		}
		Context.Schema.BreakSinglePinLink(RootOut, Input);
		return true;
	}
	if (Kind == TEXT("layout"))
	{
		int32 X = 0;
		int32 Y = 0;
		FString Comment;
		bool bVisible = false;
		if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("position"), TEXT("comment"), TEXT("comment_visible")}))
		{
			return false;
		}
		UEdGraphNode* Node = RequireNode(Context, Op, true);
		if (Node == nullptr)
		{
			return false;
		}
		if (Op.Has(TEXT("position")))
		{
			if (!ReadPair(Op, TEXT("position"), X, Y, true))
			{
				return false;
			}
			Move(*Node, X, Y);
		}
		if (Op.Has(TEXT("comment")))
		{
			if (!Op.String(TEXT("comment"), Comment))
			{
				return false;
			}
			Node->Modify();
			Node->NodeComment = Comment;
		}
		if (Op.Has(TEXT("comment_visible")))
		{
			if (!Op.Bool(TEXT("comment_visible"), bVisible, true))
			{
				return false;
			}
			Node->Modify();
			Node->bCommentBubbleVisible = bVisible;
			Node->bCommentBubblePinned = bVisible;
		}
		return true;
	}
	if (Kind == TEXT("rename"))
	{
		FString Key;
		if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("key")}) || !Op.String(TEXT("key"), Key))
		{
			return false;
		}
		UEdGraphNode* Node = RequireNode(Context, Op, false);
		if (UAIGraphNode* AINode = Cast<UAIGraphNode>(Node))
		{
			using namespace AssetDocumentEnvQueryEditLocal;
			if (AINode->NodeInstance == nullptr || !RenameKey(Context.Graph, Op, *AINode, Key))
			{
				return false;
			}
			OutName = NodeKey(*AINode);
			return true;
		}
		if (Node == nullptr || !RenameTo(Context, Op, *Node, Key))
		{
			return false;
		}
		OutName = Node->GetName();
		return true;
	}
	if (Kind == TEXT("add_comment"))
	{
		FString Key;
		FString Text;
		int32 X = 0;
		int32 Y = 0;
		int32 Width = 400;
		int32 Height = 100;
		if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("text"), TEXT("position"), TEXT("size")}) || !Op.String(TEXT("name"), Key, false)
			|| !Op.String(TEXT("text"), Text) || !ReadPair(Op, TEXT("position"), X, Y, true) || !ReadPair(Op, TEXT("size"), Width, Height, false))
		{
			return false;
		}
		FAISchemaAction_AddComment Action;
		UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(
			Action.PerformAction(&Context.Graph, nullptr, FVector2f(static_cast<float>(X), static_cast<float>(Y)), false));
		if (Comment == nullptr)
		{
			return Op.Fail(TEXT("EnvQuery.EditFailed"), TEXT("Create Comment made no comment."), TEXT("text"));
		}
		Comment->Modify();
		Comment->NodeComment = Text;
		Comment->NodePosX = X;
		Comment->NodePosY = Y;
		Comment->NodeWidth = Width;
		Comment->NodeHeight = Height;
		if (!RenameTo(Context, Op, *Comment, Key))
		{
			return false;
		}
		OutName = Comment->GetName();
		return true;
	}
	if (Kind == TEXT("comment"))
	{
		const FValue* Values = nullptr;
		if (!Op.OnlyFields({TEXT("op"), TEXT("name"), TEXT("text"), TEXT("size"), TEXT("properties")}) || !Op.Object(TEXT("properties"), Values, false))
		{
			return false;
		}
		FString Name;
		if (!Op.String(TEXT("name"), Name))
		{
			return false;
		}
		UEdGraphNode_Comment* Comment = FindComment(Context.Graph, Name);
		if (Comment == nullptr)
		{
			return Op.Fail(TEXT("EnvQuery.NodeNotFound"), FString::Printf(TEXT("The query has no comment '%s'."), *Name), TEXT("name"));
		}
		if (Op.Has(TEXT("text")))
		{
			FString Text;
			if (!Op.String(TEXT("text"), Text))
			{
				return false;
			}
			Comment->Modify();
			Comment->NodeComment = Text;
		}
		if (Op.Has(TEXT("size")))
		{
			int32 Width = 0;
			int32 Height = 0;
			if (!ReadPair(Op, TEXT("size"), Width, Height, true))
			{
				return false;
			}
			Comment->Modify();
			Comment->NodeWidth = Width;
			Comment->NodeHeight = Height;
		}
		if (Values != nullptr)
		{
			FReader ValuesReader = Op.Child(TEXT("properties"), *Values);
			for (int32 Index = 0; Index < Values->ObjectKeys.Num(); ++Index)
			{
				if (!EditDetails(ValuesReader, *Comment, Values->ObjectKeys[Index], Values->Elements[Index], false, Values->ObjectKeys[Index]))
				{
					return false;
				}
			}
		}
		return true;
	}
	return Op.Fail(TEXT("EnvQuery.InvalidOp"), FString::Printf(TEXT("Unknown op '%s'."), *Kind), TEXT("op"));
}
} // namespace

bool HandleEdit(FCall& Call, FObjectBuilder& Result)
{
	UEnvQuery* Query = GetQuery(Call);
	const FValue* Ops = nullptr;
	if (Query == nullptr || !Call.Args.OnlyFields({TEXT("ops")}) || !Call.Args.Array(TEXT("ops"), Ops))
	{
		return false;
	}
	// One editor transaction for the call; the editor's own per-edit transactions nest into it.
	const FScopedTransaction Transaction(LOCTEXT("EditEnvQuery", "Edit Environment Query"));
	UAIGraph* Graph = EnsureGraph(*Query);
	UAIGraphSchema* Schema = Graph != nullptr ? const_cast<UAIGraphSchema*>(Cast<UAIGraphSchema>(Graph->GetSchema())) : nullptr;
	if (Graph == nullptr || Schema == nullptr)
	{
		return Fail(Call, TEXT("EnvQuery.InvalidGraph"), TEXT("The query's editor graph could not be created."));
	}
	LinkParents(*Graph);
	FEditContext Context{Call, *Query, *Graph, *Schema};
	TArray<FString> Created;
	for (int32 Index = 0; Index < Ops->Elements.Num(); ++Index)
	{
		FReader Op = Call.Args.Child(FString::Printf(TEXT("ops/%d"), Index), Ops->Elements[Index]);
		FString Kind;
		if (!Op.IsObject())
		{
			return Op.Fail(TEXT("EnvQuery.InvalidOp"), TEXT("Each op is an object."));
		}
		if (!Op.String(TEXT("op"), Kind))
		{
			return false;
		}
		FString Name;
		if (!EditOp(Context, Op, Kind, Name))
		{
			if (Call.Error.Code.IsEmpty())
			{
				Op.Fail(TEXT("EnvQuery.EditFailed"), FString::Printf(TEXT("Op '%s' failed."), *Kind));
			}
			return false;
		}
		Created.Add(Name);
	}
	Result.Add(TEXT("created"), MakeStringArray(Created));
	return true;
}

bool HandleFinalize(FCall& Call, FObjectBuilder& Result)
{
	UEnvQuery* Query = GetQuery(Call);
	if (Query == nullptr || !Call.Args.OnlyFields({}))
	{
		return false;
	}
	const FScopedTransaction Transaction(LOCTEXT("FinalizeEnvQuery", "Update Environment Query"));
	UAIGraph* Graph = EnsureGraph(*Query);
	if (Graph == nullptr)
	{
		return Fail(Call, TEXT("EnvQuery.InvalidGraph"), TEXT("The query's editor graph could not be created."));
	}
	LinkParents(*Graph);
	Query->Modify();
	// The graph tab's Initialize (also what Save leaves): SpawnMissingNodes, CalculateAllWeights, UpdateAsset.
	Graph->Initialize();
	// Every option / test node carries its key (nodes the editor added keep their instance's name as key), so the
	// editor's Undo / Redo, which renames instances, leaves the keys alone.
	{
		using namespace AssetDocumentEnvQueryEditLocal;
		for (UAIGraphNode* Option : GetOptionNodes(*Graph))
		{
			TArray<UAIGraphNode*> Nodes = Option->SubNodes;
			Nodes.Insert(Option, 0);
			for (UAIGraphNode* Node : Nodes)
			{
				if (Node != nullptr && Node->NodeInstance != nullptr && !HasKeyName(*Node))
				{
					StampKeyName(*Graph, *Node, Node->NodeInstance->GetName());
				}
			}
		}
	}
	TArray<FMessage> Messages;
	Validate(*Query, Messages);
	Result.Add(TEXT("messages"), MessagesValue(Messages));
	Result.Add(TEXT("params"), ParamsValue(*Query));
	return true;
}
} // namespace UE::AssetDocument::Graph::EnvQuery

#undef LOCTEXT_NAMESPACE
