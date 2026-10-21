// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreePrimitives.h"

#include "AssetDocumentEditorNativeState.h"
#include "Provider/AssetDocumentAIProviderContracts.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"

#include "AIGraphNode.h"
#include "BehaviorTreeEditorTypes.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredGraphValidation.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "BehaviorTreeDecoratorGraphNode_Logic.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/SoftObjectPath.h"

namespace AssetDocumentAIBehaviorTreeAuthoredGraphReadback
{
using namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback;

FAssetDocumentPrimitiveOutcome Failure(const TCHAR* Code, const TCHAR* Message,
	const FAuthoredGraphValidationFailure* GraphFailure = nullptr)
{
	FAssetDocumentPrimitiveOutcome Outcome = MakeAssetDocumentAIPrimitiveFailure(
		EAssetDocumentPrimitiveOutcomeStatus::Failed, Code, Message);
	const TCHAR* Fault = nullptr;
	if (GraphFailure)
	{
		switch (GraphFailure->Fault)
		{
		case EAuthoredGraphValidationFault::MultipleRoots: Fault = TEXT("multiple_roots"); break;
		case EAuthoredGraphValidationFault::Unreachable: Fault = TEXT("unreachable"); break;
		case EAuthoredGraphValidationFault::Cycle: Fault = TEXT("cycle"); break;
		case EAuthoredGraphValidationFault::InvalidConnection: Fault = TEXT("invalid_connection"); break;
		default: break;
		}
	}
	if (Fault)
	{
		FAssetDocumentRuntimeDiagnosticContext& Context = Outcome.Diagnostics[0].Context.AddDefaulted_GetRef();
		Context.Key = TEXT("graph_validation");
		Context.Value = ObjectValue({TEXT("fault"), TEXT("path")},
			{StringValue(Fault), StringValue(GraphFailure->Path)});
	}
	return Outcome;
}

const FAssetDocumentRuntimeValueDto* FindField(
	const FAssetDocumentRuntimeValueDto& Object, FStringView Name)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Object.ObjectKeys.Num() != Object.Elements.Num()) return nullptr;
	for (int32 Index = 0; Index < Object.ObjectKeys.Num(); ++Index)
	{
		if (Object.ObjectKeys[Index] == Name) return &Object.Elements[Index];
	}
	return nullptr;
}

bool IsCanonicalTreePath(const FAssetDocumentRuntimeValueDto* Value)
{
	if (Value == nullptr || Value->Kind != EAssetDocumentRuntimeValueKind::String
		|| Value->StringValue.IsEmpty()) return false;
	const FSoftObjectPath Path(Value->StringValue);
	return Path.IsValid() && Path.GetSubPathString().IsEmpty()
		&& Path.ToString() == Value->StringValue;
}

bool ParseRequest(const FAssetDocumentPrimitiveInvocation& Invocation, FString& OutPath)
{
	const FAssetDocumentRuntimeValueDto& Value = Invocation.ValueArguments;
	if (!IsAssetDocumentAICanonicalRuntimeValue(Value)
		|| Value.ObjectKeys != TArray<FString>{TEXT("behavior_tree_path")}
		|| Value.Elements.Num() != 1 || !IsCanonicalTreePath(&Value.Elements[0])) return false;
	OutPath = Value.Elements[0].StringValue;
	return true;
}

FAssetDocumentVerifiedLeaseResult AcquireReadOnlyAssetLease(
	IAssetDocumentHandleResolver& Handles,
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	FAssetDocumentVerifiedLeaseResult Empty;
	if (Invocation.HandleArguments.Num() != 1
		|| Invocation.HandleArguments[0].Name != TEXT("asset")) return Empty;
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Invocation.RequestId;
	Context.TransactionId = Invocation.TransactionId;
	Context.RuntimeRevision = Invocation.RuntimeRevision;
	Context.ProviderId = TEXT("ai");
	Context.Phase = Invocation.Phase;
	Context.RequiredKind = EAssetDocumentHandleKind::Asset;
	Context.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
	return Handles.AcquireVerifiedLease(Invocation.HandleArguments[0].Handle, Context);
}

FAssetDocumentRuntimeValueDto EditorValue(const UEdGraphNode& Node)
{
	return ObjectValue(
		{TEXT("comment_bubble_pinned"), TEXT("comment_bubble_visible"), TEXT("node_comment"), TEXT("position")},
		{BooleanValue(Node.bCommentBubblePinned), BooleanValue(Node.bCommentBubbleVisible),
			StringValue(Node.NodeComment), ObjectValue({TEXT("x"), TEXT("y")},
				{IntegerValue(Node.NodePosX), IntegerValue(Node.NodePosY)})});
}

bool BuildMainNode(const UBehaviorTreeGraphNode& Node,
	FAssetDocumentRuntimeValueDto& OutNode, bool& Overflow, EAuthoredGraphReadbackVersion Version);

FString StableId(const UEdGraphNode& Node)
{
	return Node.NodeGuid.ToString(EGuidFormats::Digits);
}

bool BuildMainChildren(const UEdGraphNode& Node,
	TArray<FAssetDocumentRuntimeValueDto>& OutChildren, bool& Overflow, EAuthoredGraphReadbackVersion Version)
{
	TSet<const UBehaviorTreeGraphNode*> Seen;
	for (const UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin == nullptr || Pin->Direction != EGPD_Output) continue;
		TArray<UEdGraphPin*> SortedLinks;
		for (UEdGraphPin* Linked : Pin->LinkedTo) SortedLinks.Add(Linked);
		SortedLinks.StableSort(FCompareNodeXLocation());
		for (const UEdGraphPin* Linked : SortedLinks)
		{
			const UBehaviorTreeGraphNode* Child = Linked != nullptr
				? Cast<UBehaviorTreeGraphNode>(Linked->GetOwningNode()) : nullptr;
			if (Child != nullptr && !Child->IsSubNode() && !Seen.Contains(Child))
			{
				Seen.Add(Child);
				FAssetDocumentRuntimeValueDto Value;
				if (!BuildMainNode(*Child, Value, Overflow, Version)) return false;
				OutChildren.Add(MoveTemp(Value));
			}
		}
	}
	return true;
}

bool BuildSimpleNode(const UBehaviorTreeGraphNode& Node,
	FAssetDocumentRuntimeValueDto& OutNode, bool& Overflow, EAuthoredGraphReadbackVersion Version)
{
	UObject* const Instance = Node.NodeInstance.Get();
	if (Instance == nullptr || !Node.NodeGuid.IsValid()) return false;
	FAssetDocumentRuntimeValueDto Properties;
	if (!BuildAuthoredProperties(*Instance, Properties, &Overflow, Version)) return false;
	OutNode = ObjectValue(
		{TEXT("authored_properties"), TEXT("editor"), TEXT("node_class_path"), TEXT("stable_node_id")},
		{MoveTemp(Properties), EditorValue(Node), StringValue(Instance->GetClass()->GetPathName()),
			StringValue(Node.NodeGuid.ToString(EGuidFormats::Digits))});
	return true;
}

bool BuildComments(const UEdGraph& Graph, TArray<FAssetDocumentRuntimeValueDto>& CommentValues)
{
	TArray<const UEdGraphNode_Comment*> Comments;
	for (const UEdGraphNode* Node : Graph.Nodes)
	{
		if (const UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node))
		{
			if (!Comment->NodeGuid.IsValid()) return false;
			Comments.Add(Comment);
		}
	}
	Comments.Sort([](const UEdGraphNode_Comment& Left, const UEdGraphNode_Comment& Right)
	{
		return StableId(Left) < StableId(Right);
	});

	for (const UEdGraphNode_Comment* Comment : Comments)
	{
		const FLinearColor Color = Comment->CommentColor;
		const FString MoveMode = Comment->MoveMode == ECommentBoxMode::GroupMovement
			? TEXT("GroupMovement") : TEXT("NoGroupMovement");
		CommentValues.Add(ObjectValue(
			{TEXT("color"), TEXT("color_comment_bubble"), TEXT("comment_bubble_pinned"), TEXT("comment_bubble_visible"), TEXT("comment_bubble_visible_in_details"), TEXT("comment_depth"), TEXT("font_size"), TEXT("move_mode"), TEXT("node_details"), TEXT("position"), TEXT("size"), TEXT("stable_node_id"), TEXT("text")},
			{ObjectValue({TEXT("a"), TEXT("b"), TEXT("g"), TEXT("r")}, {NumberValue(Color.A), NumberValue(Color.B), NumberValue(Color.G), NumberValue(Color.R)}),
				BooleanValue(Comment->bColorCommentBubble), BooleanValue(Comment->bCommentBubblePinned), BooleanValue(Comment->bCommentBubbleVisible), BooleanValue(Comment->bCommentBubbleVisible_InDetailsPanel),
				IntegerValue(Comment->CommentDepth), IntegerValue(Comment->FontSize), StringValue(MoveMode), StringValue(Comment->NodeDetails.ToString()),
				ObjectValue({TEXT("x"), TEXT("y")}, {IntegerValue(Comment->NodePosX), IntegerValue(Comment->NodePosY)}),
				ObjectValue({TEXT("height"), TEXT("width")}, {IntegerValue(Comment->NodeHeight), IntegerValue(Comment->NodeWidth)}), StringValue(StableId(*Comment)), StringValue(Comment->NodeComment)}));
	}
	return true;
}

bool BuildBoundGraph(const UEdGraph& Graph, FAssetDocumentRuntimeValueDto& OutValue, bool& Overflow, EAuthoredGraphReadbackVersion Version)
{
	if (!Graph.GraphGuid.IsValid()) return false;
	TArray<const UEdGraphNode*> Nodes;
	for (const UEdGraphNode* Node : Graph.Nodes)
	{
		if (Node == nullptr || !Node->NodeGuid.IsValid()) return false;
		if (Version == EAuthoredGraphReadbackVersion::V2 && Cast<UEdGraphNode_Comment>(Node)) continue;
		Nodes.Add(Node);
	}
	Nodes.Sort([](const UEdGraphNode& Left, const UEdGraphNode& Right)
	{
		return StableId(Left) < StableId(Right);
	});
	TArray<FAssetDocumentRuntimeValueDto> NodeValues;
	TArray<FAssetDocumentRuntimeValueDto> LinkValues;
	for (const UEdGraphNode* Node : Nodes)
	{
		FString Kind;
		FAssetDocumentRuntimeValueDto ClassPath = NullValue();
		FAssetDocumentRuntimeValueDto Properties = ObjectValue({}, {});
		FAssetDocumentRuntimeValueDto Editor = EditorValue(*Node);
		if (const UBehaviorTreeDecoratorGraphNode_Logic* Logic =
			Cast<UBehaviorTreeDecoratorGraphNode_Logic>(Node))
		{
			switch (Logic->LogicMode)
			{
			case EDecoratorLogicMode::Sink: Kind = TEXT("sink"); break;
			case EDecoratorLogicMode::And: Kind = TEXT("and"); break;
			case EDecoratorLogicMode::Or: Kind = TEXT("or"); break;
			case EDecoratorLogicMode::Not: Kind = TEXT("not"); break;
			default: return false;
			}
		}
		else if (const UBehaviorTreeDecoratorGraphNode_Decorator* Test =
			Cast<UBehaviorTreeDecoratorGraphNode_Decorator>(Node))
		{
			if (Test->NodeInstance == nullptr || !BuildAuthoredProperties(*Test->NodeInstance, Properties, &Overflow, Version)) return false;
			Kind = TEXT("test");
			ClassPath = StringValue(Test->NodeInstance->GetClass()->GetPathName());
		}
		else return false;
		NodeValues.Add(ObjectValue(
			{TEXT("authored_properties"), TEXT("editor"), TEXT("kind"), TEXT("node_class_path_or_null"), TEXT("stable_node_id")},
			{MoveTemp(Properties), MoveTemp(Editor), StringValue(MoveTemp(Kind)), MoveTemp(ClassPath), StringValue(StableId(*Node))}));
		if (Version == EAuthoredGraphReadbackVersion::V2)
		{
			int32 Count = 0;
			for (const UEdGraphPin* Pin : Node->Pins) if (Pin && Pin->Direction == EGPD_Input) ++Count;
			NodeValues.Last().ObjectKeys.Insert(TEXT("input_count"), 2);
			NodeValues.Last().Elements.Insert(IntegerValue(Count), 2);
		}
		int32 InputIndex = -1;
		for (const UEdGraphPin* Input : Node->Pins)
		{
			if (Input == nullptr) return false;
			if (Input->Direction != EGPD_Input) continue;
			++InputIndex;
			if (Input->LinkedTo.Num() > 1) return false;
			for (const UEdGraphPin* SourcePin : Input->LinkedTo)
			{
				const UEdGraphNode* Source = SourcePin ? SourcePin->GetOwningNode() : nullptr;
				if (Source == nullptr || SourcePin->Direction != EGPD_Output || !Source->NodeGuid.IsValid()) return false;
				LinkValues.Add(ObjectValue(
					{TEXT("from_stable_node_id"), TEXT("to_input_index"), TEXT("to_stable_node_id")},
					{StringValue(StableId(*Source)), IntegerValue(InputIndex), StringValue(StableId(*Node))}));
			}
		}
	}
	LinkValues.Sort([](const FAssetDocumentRuntimeValueDto& Left, const FAssetDocumentRuntimeValueDto& Right)
	{
		const FAssetDocumentRuntimeValueDto* LeftFrom = FindField(Left, TEXT("from_stable_node_id"));
		const FAssetDocumentRuntimeValueDto* RightFrom = FindField(Right, TEXT("from_stable_node_id"));
		if (LeftFrom->StringValue != RightFrom->StringValue) return LeftFrom->StringValue < RightFrom->StringValue;
		const FAssetDocumentRuntimeValueDto* LeftTo = FindField(Left, TEXT("to_stable_node_id"));
		const FAssetDocumentRuntimeValueDto* RightTo = FindField(Right, TEXT("to_stable_node_id"));
		if (LeftTo->StringValue != RightTo->StringValue) return LeftTo->StringValue < RightTo->StringValue;
		return FindField(Left, TEXT("to_input_index"))->IntegerValue < FindField(Right, TEXT("to_input_index"))->IntegerValue;
	});
	OutValue = ObjectValue({TEXT("graph_guid"), TEXT("links"), TEXT("nodes")},
		{StringValue(Graph.GraphGuid.ToString(EGuidFormats::Digits)), ArrayValue(MoveTemp(LinkValues)), ArrayValue(MoveTemp(NodeValues))});
	if (Version == EAuthoredGraphReadbackVersion::V2)
	{
		TArray<FAssetDocumentRuntimeValueDto> Comments;
		if (!BuildComments(Graph, Comments)) return false;
		OutValue.ObjectKeys.Insert(TEXT("comments"), 0);
		OutValue.Elements.Insert(ArrayValue(MoveTemp(Comments)), 0);
	}
	return true;
}

bool BuildDecorator(const UBehaviorTreeGraphNode& Node, FAssetDocumentRuntimeValueDto& OutValue, bool& Overflow, EAuthoredGraphReadbackVersion Version)
{
	if (!Node.NodeGuid.IsValid()) return false;
	if (const UBehaviorTreeGraphNode_CompositeDecorator* Composite =
		Cast<UBehaviorTreeGraphNode_CompositeDecorator>(&Node))
	{
		FAssetDocumentRuntimeValueDto Bound = NullValue();
		if (Composite->BoundGraph != nullptr && !BuildBoundGraph(*Composite->BoundGraph, Bound, Overflow, Version)) return false;
		// V2's authored object already owns these public editable wrapper fields.
		// Preserve the V1 projection while completing the V2 authored content.
		const FAssetDocumentRuntimeValueDto Properties = Version == EAuthoredGraphReadbackVersion::V2
			? ObjectValue({TEXT("bShowOperations"), TEXT("CompositeName")},
				{BooleanValue(Composite->bShowOperations != 0), StringValue(Composite->CompositeName)})
			: ObjectValue({}, {});
		OutValue = ObjectValue(
			{TEXT("authored_properties"), TEXT("bound_graph"), TEXT("editor"), TEXT("kind"), TEXT("node_class_path"), TEXT("stable_node_id")},
			{Properties, MoveTemp(Bound), EditorValue(Node), StringValue(TEXT("composite")), NullValue(), StringValue(StableId(Node))});
		return true;
	}
	FAssetDocumentRuntimeValueDto Simple;
	if (!BuildSimpleNode(Node, Simple, Overflow, Version)) return false;
	OutValue = ObjectValue(
		{TEXT("authored_properties"), TEXT("bound_graph"), TEXT("editor"), TEXT("kind"), TEXT("node_class_path"), TEXT("stable_node_id")},
		{*FindField(Simple, TEXT("authored_properties")), NullValue(), *FindField(Simple, TEXT("editor")),
			StringValue(TEXT("simple")), *FindField(Simple, TEXT("node_class_path")), *FindField(Simple, TEXT("stable_node_id"))});
	return true;
}

bool BuildMainNode(const UBehaviorTreeGraphNode& Node,
	FAssetDocumentRuntimeValueDto& OutNode, bool& Overflow, EAuthoredGraphReadbackVersion Version)
{
	FAssetDocumentRuntimeValueDto Simple;
	if (!BuildSimpleNode(Node, Simple, Overflow, Version)) return false;
	TArray<FAssetDocumentRuntimeValueDto> Children;
	if (!BuildMainChildren(Node, Children, Overflow, Version)) return false;
	TArray<FAssetDocumentRuntimeValueDto> Decorators;
	for (const TObjectPtr<UBehaviorTreeGraphNode>& Decorator : Node.Decorators)
	{
		if (Decorator && Decorator->bInjectedNode) continue;
		FAssetDocumentRuntimeValueDto Value;
		if (Decorator == nullptr || !BuildDecorator(*Decorator, Value, Overflow, Version)) return false;
		Decorators.Add(MoveTemp(Value));
	}
	TArray<FAssetDocumentRuntimeValueDto> Services;
	for (const TObjectPtr<UBehaviorTreeGraphNode>& Service : Node.Services)
	{
		if (Service && Service->bInjectedNode) continue;
		FAssetDocumentRuntimeValueDto Value;
		if (Service == nullptr || !BuildSimpleNode(*Service, Value, Overflow, Version)) return false;
		Services.Add(MoveTemp(Value));
	}
	OutNode = ObjectValue(
		{TEXT("authored_properties"), TEXT("children"), TEXT("decorators"), TEXT("editor"),
			TEXT("node_class_path"), TEXT("services"), TEXT("stable_node_id")},
		{*FindField(Simple, TEXT("authored_properties")), ArrayValue(MoveTemp(Children)),
			ArrayValue(MoveTemp(Decorators)), *FindField(Simple, TEXT("editor")),
			*FindField(Simple, TEXT("node_class_path")), ArrayValue(MoveTemp(Services)),
			*FindField(Simple, TEXT("stable_node_id"))});
	return true;
}

bool BuildProjection(UBehaviorTree& Tree, FAssetDocumentRuntimeValueDto& OutValue, bool& Overflow,
	EAuthoredGraphReadbackVersion Version, UBehaviorTreeGraph* ExplicitScratchGraph = nullptr)
{
	if (ExplicitScratchGraph && (Tree.BTGraph != nullptr || ExplicitScratchGraph->GetOuter() != &Tree)) return false;
	UBehaviorTreeGraph* const Graph = ExplicitScratchGraph ? ExplicitScratchGraph : Cast<UBehaviorTreeGraph>(Tree.BTGraph);
	if (Tree.BTGraph == nullptr && !ExplicitScratchGraph)
	{
		OutValue = ObjectValue({TEXT("comments"), TEXT("graph_guid"), TEXT("graph_state"), TEXT("root")},
			{ArrayValue({}), NullValue(), StringValue(TEXT("absent")), NullValue()});
		return true;
	}
	if (Graph == nullptr || !Graph->GraphGuid.IsValid()) return false;
	UBehaviorTreeGraphNode_Root* Root = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UBehaviorTreeGraphNode_Root* Candidate = Cast<UBehaviorTreeGraphNode_Root>(Node))
		{
			if (Root != nullptr) return false;
			Root = Candidate;
		}
	}
	if (Root == nullptr) return false;
	TArray<FAssetDocumentRuntimeValueDto> Children;
	if (!BuildMainChildren(*Root, Children, Overflow, Version) || Children.Num() > 1) return false;
	TArray<FAssetDocumentRuntimeValueDto> CommentValues;
	if (!BuildComments(*Graph, CommentValues)) return false;
	OutValue = ObjectValue({TEXT("comments"), TEXT("graph_guid"), TEXT("graph_state"), TEXT("root")},
		{ArrayValue(MoveTemp(CommentValues)), StringValue(Graph->GraphGuid.ToString(EGuidFormats::Digits)),
			StringValue(TEXT("present")), Children.Num() == 1 ? MoveTemp(Children[0]) : NullValue()});
	return true;
}
} // namespace AssetDocumentAIBehaviorTreeAuthoredGraphReadback

bool UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::BuildAuthoredGraphProjection(
	UBehaviorTree& Tree, FAssetDocumentRuntimeValueDto& OutValue, bool& Overflow, EAuthoredGraphReadbackVersion Version)
{
	return AssetDocumentAIBehaviorTreeAuthoredGraphReadback::BuildProjection(Tree, OutValue, Overflow, Version);
}

bool UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::BuildAuthoredGraphProjection(
	UBehaviorTree& ScratchTree, UBehaviorTreeGraph& ScratchGraph,
	FAssetDocumentRuntimeValueDto& OutValue, bool& Overflow, EAuthoredGraphReadbackVersion Version)
{
	return AssetDocumentAIBehaviorTreeAuthoredGraphReadback::BuildProjection(ScratchTree, OutValue, Overflow, Version, &ScratchGraph);
}

bool UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::BuildAuthoredBoundGraphProjection(
	const UEdGraph& Graph, FAssetDocumentRuntimeValueDto& OutValue, bool& Overflow, EAuthoredGraphReadbackVersion Version)
{
	return AssetDocumentAIBehaviorTreeAuthoredGraphReadback::BuildBoundGraph(Graph, OutValue, Overflow, Version);
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeAuthoredGraphReadback(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AssetDocumentAIBehaviorTreeAuthoredGraphReadback;
	if (Invocation.Version != 1 && Invocation.Version != 2)
		return Failure(TEXT("AIProvider.InvalidValueShape"), TEXT("Unsupported authored-graph readback version."));
	const auto Version = Invocation.Version == 2 ? EAuthoredGraphReadbackVersion::V2 : EAuthoredGraphReadbackVersion::V1;
	FString RequestedPath;
	if (!ParseRequest(Invocation, RequestedPath))
		return Failure(TEXT("AIProvider.InvalidValueShape"), TEXT("The authored-graph readback request is not canonical."));
	const bool bMutationPhase = Invocation.Phase == EAssetDocumentRuntimeRequestPhase::Apply
		|| Invocation.Phase == EAssetDocumentRuntimeRequestPhase::Repair;
	if ((bMutationPhase && !IsAssetDocumentAITransactionValid(Invocation, ActiveTransaction))
		|| (!bMutationPhase && ActiveTransaction != nullptr))
		return Failure(TEXT("AIProvider.InvalidTransaction"), TEXT("The readback transaction state does not match its phase."));
	FAssetDocumentVerifiedLeaseResult LeaseResult = AcquireReadOnlyAssetLease(Handles, Invocation);
	if (LeaseResult.Lease == nullptr)
		return Failure(TEXT("AIProvider.InvalidHandle"), TEXT("One matching AI read-only Asset lease is required."));
	FAssetDocumentPrimitiveOutcome Outcome;
	const FAssetDocumentContractValidationResult Identity = WithAssetDocumentEditorAssetIdentity(
		*LeaseResult.Lease, [&](const FAssetDocumentEditorAssetIdentityView& View)
		{
			UBehaviorTree* const Tree = Cast<UBehaviorTree>(&View.GetObject());
			if (Tree == nullptr || Tree->GetClass() != UBehaviorTree::StaticClass()
				|| View.GetCanonicalObjectPath() != RequestedPath)
			{
				Outcome = Failure(TEXT("AIProvider.InvalidHandle"), TEXT("The Asset lease is not the requested exact BehaviorTree."));
				return FAssetDocumentContractValidationResult{};
			}
			FString Before, After, Diagnostic;
			FAuthoredGraphValidationFailure GraphFailure;
			if (!CaptureGraphIdentity(*Tree, Before, Diagnostic, Version,
				Version == EAuthoredGraphReadbackVersion::V2 ? &GraphFailure : nullptr))
			{
				Outcome = Failure(*Diagnostic, TEXT("The authored graph inventory is invalid."), &GraphFailure);
				return FAssetDocumentContractValidationResult{};
			}
			bool Overflow = false;
			if (!BuildAuthoredGraphProjection(*Tree, Outcome.Value, Overflow, Version))
			{
				Outcome = Failure(Overflow ? TEXT("AIProvider.BehaviorTree.GraphReadbackProjectionOverflow") : TEXT("AIProvider.BehaviorTree.GraphReadbackPropertyRejected"), TEXT("The complete authored graph projection is unavailable."));
				return FAssetDocumentContractValidationResult{};
			}
			if (!IsAssetDocumentAICanonicalRuntimeValue(Outcome.Value))
			{
				Outcome = Failure(TEXT("AIProvider.BehaviorTree.GraphReadbackProjectionOverflow"), TEXT("The authored projection exceeds the canonical RuntimeValue envelope."));
				return FAssetDocumentContractValidationResult{};
			}
			if (!CaptureGraphIdentity(*Tree, After, Diagnostic, Version) || Before != After)
			{
				Outcome = Failure(TEXT("AIProvider.BehaviorTree.GraphReadbackInvalid"), TEXT("The graph identity or dirty state changed during readback."));
				return FAssetDocumentContractValidationResult{};
			}
			Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
			FAssetDocumentContractValidationResult Valid;
			Valid.bValid = true;
			return Valid;
		});
	return Identity.bValid || !Outcome.Diagnostics.IsEmpty() ? Outcome
		: Failure(TEXT("AIProvider.InvalidHandle"), TEXT("The Asset lease could not enter Editor identity."));
}
