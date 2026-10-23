// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeGraphBundleInternal.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeScratchAuthoring.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"

#include "Provider/AssetDocumentAIProviderContracts.h"
#include "AIGraphNode.h"
#include "AIGraphSchema.h"
#include "AIGraphTypes.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTreeEditorModule.h"
#include "BehaviorTreeEditorTypes.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "Layout/SlateRect.h"
#include "Modules/ModuleManager.h"
#include "UObject/GarbageCollection.h"
#include "UObject/SoftObjectPath.h"

namespace
{
bool IsSafeCommentInteger(double Value)
{
	const float FloatValue = static_cast<float>(Value);
	return FMath::IsFinite(Value) && FMath::IsFinite(FloatValue)
		&& Value >= static_cast<double>(MIN_int32)
		&& Value <= static_cast<double>(MAX_int32)
		&& FloatValue >= static_cast<float>(MIN_int32)
		&& FloatValue < static_cast<float>(MAX_int32);
}

bool ResolveActionClass(const FString& Path, UClass*& OutClass)
{
	OutClass = FSoftClassPath(Path).ResolveClass();
	return OutClass != nullptr && OutClass->GetPathName() == Path
		&& !OutClass->HasAnyClassFlags(CLASS_Abstract);
}

int32 FindUniqueClassIndex(TArray<FGraphNodeClassData>& Classes,
	const UClass& RequestedClass)
{
	int32 Match = INDEX_NONE;
	for (int32 Index = 0; Index < Classes.Num(); ++Index)
	{
		if (Classes[Index].GetClass(true) != &RequestedClass) continue;
		if (Match != INDEX_NONE) return INDEX_NONE;
		Match = Index;
	}
	return Match;
}

bool IsExactClassInFamily(FBehaviorTreeEditorModule& Module,
	UClass& BaseClass, const UClass& RequestedClass)
{
	FGCScopeGuard Guard;
	TArray<FGraphNodeClassData> Classes;
	Module.GetClassCache()->GatherClasses(&BaseClass, Classes);
	return FindUniqueClassIndex(Classes, RequestedClass) != INDEX_NONE;
}

bool IsExactNodeActionResult(const UEdGraphNode* Node,
	const UBehaviorTreeGraph& Graph, const UClass& RequestedClass, int32 X, int32 Y)
{
	const UBehaviorTreeGraphNode* BehaviorNode = Cast<UBehaviorTreeGraphNode>(Node);
	return Node != nullptr && BehaviorNode != nullptr && Node->GetOuter() == &Graph
		&& Graph.Nodes.Contains(Node) && Node->NodeGuid.IsValid()
		&& Node->NodePosX == X && Node->NodePosY == Y
		&& BehaviorNode->ParentNode == nullptr
		&& BehaviorNode->NodeInstance != nullptr
		&& BehaviorNode->NodeInstance->GetClass() == &RequestedClass
		&& BehaviorNode->NodeInstance->GetOuter() == Graph.GetOuter();
}

bool IsExactSubnodeActionResult(const UAIGraphNode* Node,
	const UAIGraphNode& Parent, const UBehaviorTreeGraph& Graph,
	const UClass& RequestedClass)
{
	const UBehaviorTreeGraphNode* BehaviorNode = Cast<UBehaviorTreeGraphNode>(Node);
	return Node != nullptr && BehaviorNode != nullptr && Node->GetOuter() == &Graph
		&& Node->GetGraph() == &Graph && Node->NodeGuid.IsValid()
		&& Node->NodePosX == 0 && Node->NodePosY == 0 && Node->ParentNode == &Parent
		&& Parent.SubNodes.Contains(Node) && BehaviorNode->NodeInstance != nullptr
		&& BehaviorNode->NodeInstance->GetClass() == &RequestedClass
		&& BehaviorNode->NodeInstance->GetOuter() == Graph.GetOuter();
}

UEdGraphPin* FindExactPin(UEdGraphNode& Node, const FString& Name)
{
	UEdGraphPin* Found = nullptr;
	for (UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin == nullptr || Pin->PinName.ToString() != Name) continue;
		if (Found != nullptr) return nullptr;
		Found = Pin;
	}
	return Found;
}

bool AcquireGraph(const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles, FAssetDocumentAIBehaviorTreeGraphLease& OutLease,
	TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
	return AcquireAssetDocumentAIBehaviorTreeGraphLeaseInternal(
		Invocation, Handles, TEXTVIEW("graph"), OutLease, Diagnostics);
}

bool IsActionRequest(const FAssetDocumentRuntimeValueDto& Value,
	const FAssetDocumentRuntimeValueDto*& ClassPath)
{
	ClassPath = FindAssetDocumentAIBehaviorTreeField(Value, TEXT("node_class_path"));
	return IsAssetDocumentAIBehaviorTreeScratchContext()
		&& EnsureAssetDocumentAIBehaviorTreeEditorAuthority()
		&& IsAssetDocumentAICanonicalRuntimeValue(Value)
		&& IsAssetDocumentAIBehaviorTreeCanonicalObjectPath(ClassPath);
}

bool InvokeMainNodeAction(const UEdGraphSchema_BehaviorTree& Schema,
	FBehaviorTreeEditorModule& Module, UBehaviorTreeGraph& Graph,
	const UClass& RequestedClass, int32 X, int32 Y, UEdGraphNode*& OutNode)
{
	FGCScopeGuard Guard;
	TSet<UEdGraphNode*> Before;
	for (UEdGraphNode* Existing : Graph.Nodes) Before.Add(Existing);
	TArray<FGraphNodeClassData> Composites;
	TArray<FGraphNodeClassData> Tasks;
	FGraphNodeClassHelper& Cache = *Module.GetClassCache();
	Cache.GatherClasses(UBTCompositeNode::StaticClass(), Composites);
	Cache.GatherClasses(UBTTaskNode::StaticClass(), Tasks);
	int32 Index = FindUniqueClassIndex(Composites, RequestedClass);
	if (Index == INDEX_NONE) Index = FindUniqueClassIndex(Tasks, RequestedClass);
	if (Index == INDEX_NONE) return false;
	if (!Composites.ContainsByPredicate([&RequestedClass](FGraphNodeClassData& Data)
		{ return Data.GetClass(true) == &RequestedClass; })) Index += Composites.Num();
	FGraphContextMenuBuilder Builder(&Graph);
	Schema.GetGraphContextActions(Builder);
	if (Builder.GetNumActions() != Composites.Num() + Tasks.Num() + 2) return false;
	TSharedPtr<FEdGraphSchemaAction>& Action = Builder.GetSchemaAction(Index);
	if (!Action.IsValid()) return false;
	OutNode = Action->PerformAction(&Graph, nullptr, FVector2f(X, Y), false);
	return Graph.Nodes.Num() == Before.Num() + 1 && !Before.Contains(OutNode)
		&& IsExactNodeActionResult(OutNode, Graph, RequestedClass, X, Y);
}

bool InvokeSubnodeAction(const UEdGraphSchema_BehaviorTree& Schema,
	FBehaviorTreeEditorModule& Module, UBehaviorTreeGraph& Graph, UAIGraphNode& Parent,
	const UClass& RequestedClass, bool bDecorator, UAIGraphNode*& OutNode)
{
	FGCScopeGuard Guard;
	FGraphNodeClassHelper& Cache = *Module.GetClassCache();
	TArray<FGraphNodeClassData> Classes;
	Cache.GatherClasses(bDecorator ? UBTDecorator::StaticClass() : UBTService::StaticClass(), Classes);
	const int32 Index = FindUniqueClassIndex(Classes, RequestedClass);
	if (Index == INDEX_NONE) return false;
	TArray<UAIGraphNode*> Before;
	for (UAIGraphNode* Existing : Parent.SubNodes) Before.Add(Existing);
	FGraphContextMenuBuilder Builder(&Graph);
	// Stock subnode collection uses CurrentGraph as its template outer.
	// PerformAction below still receives the exact authored graph and parent.
	Builder.CurrentGraph = Builder.OwnerOfTemporaries;
	Builder.SelectedObjects.Add(&Parent);
	Schema.GetGraphNodeContextActions(Builder,
		bDecorator ? ESubNode::Decorator : ESubNode::Service);
	const int32 Expected = Classes.Num() + (bDecorator ? 1 : 0);
	if (Builder.SelectedObjects.Num() != 1 || Builder.SelectedObjects[0] != &Parent
		|| Builder.GetNumActions() != Expected) return false;
	TSharedPtr<FEdGraphSchemaAction>& Action = Builder.GetSchemaAction(Index);
	if (!Action.IsValid() || Action->PerformAction(&Graph, nullptr, FVector2f::ZeroVector, false) != nullptr
		|| Parent.SubNodes.Num() != Before.Num() + 1) return false;
	for (int32 ExistingIndex = 0; ExistingIndex < Before.Num(); ++ExistingIndex)
	{
		if (Parent.SubNodes[ExistingIndex] != Before[ExistingIndex]) return false;
	}
	OutNode = Parent.SubNodes.Last();
	if (Before.Contains(OutNode)) return false;
	return IsExactSubnodeActionResult(OutNode, Parent, Graph, RequestedClass);
}

bool ReadNodeActionCoordinate(
	const FAssetDocumentRuntimeValueDto* Value, int32& OutCoordinate)
{
	double Coordinate = 0.0;
	if (!ReadAssetDocumentAIBehaviorTreeFiniteCoordinate(Value, Coordinate)
		|| !FMath::IsFinite(Coordinate)
		|| Coordinate < static_cast<double>(MIN_int32)
		|| Coordinate > static_cast<double>(MAX_int32)
		|| FMath::TruncToDouble(Coordinate) != Coordinate) return false;
	OutCoordinate = static_cast<int32>(Coordinate);
	return true;
}

bool ReadNodeActionRequest(const FAssetDocumentRuntimeValueDto& Value,
	const FAssetDocumentRuntimeValueDto*& OutClassPath, int32& OutX, int32& OutY)
{
	const FAssetDocumentRuntimeValueDto* Position =
		FindAssetDocumentAIBehaviorTreeField(Value, TEXT("position"));
	if (!IsActionRequest(Value, OutClassPath)
		|| !HasAssetDocumentAIBehaviorTreeExactFields(Value, {TEXT("node_class_path"), TEXT("position")})
		|| Position == nullptr || !HasAssetDocumentAIBehaviorTreeExactFields(*Position, {TEXT("x"), TEXT("y")})
		|| !ReadNodeActionCoordinate(FindAssetDocumentAIBehaviorTreeField(*Position, TEXT("x")), OutX)
		|| !ReadNodeActionCoordinate(FindAssetDocumentAIBehaviorTreeField(*Position, TEXT("y")), OutY)) return false;
	return true;
}

enum class EMainNodeActionResolution
{
	AuthorityUnavailable,
	ActionUnavailable,
	ClassMismatch,
	Ready,
};

EMainNodeActionResolution ResolveMainNodeAction(
	const FAssetDocumentAIBehaviorTreeGraphLease& GraphLease,
	const FString& ClassPath, const UEdGraphSchema_BehaviorTree*& OutSchema,
	FBehaviorTreeEditorModule*& OutModule, UClass*& OutClass)
{
	OutSchema = Cast<UEdGraphSchema_BehaviorTree>(GraphLease.Graph->GetSchema());
	OutModule = FModuleManager::GetModulePtr<FBehaviorTreeEditorModule>(TEXT("BehaviorTreeEditor"));
	if (OutSchema == nullptr || OutModule == nullptr || !OutModule->GetClassCache().IsValid()) return EMainNodeActionResolution::AuthorityUnavailable;
	if (!ResolveActionClass(ClassPath, OutClass)) return EMainNodeActionResolution::ActionUnavailable;
	return IsExactClassInFamily(*OutModule, *UBTCompositeNode::StaticClass(), *OutClass)
		|| IsExactClassInFamily(*OutModule, *UBTTaskNode::StaticClass(), *OutClass)
		? EMainNodeActionResolution::Ready : EMainNodeActionResolution::ClassMismatch;
}

FAssetDocumentPrimitiveOutcome MakeMainNodeActionResolutionFailure(
	EMainNodeActionResolution Resolution)
{
	switch (Resolution)
	{
	case EMainNodeActionResolution::AuthorityUnavailable:
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.AuthorityUnavailable"), TEXT("The public BehaviorTree class cache is unavailable."));
	case EMainNodeActionResolution::ActionUnavailable:
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.BehaviorTree.ActionUnavailable"), TEXT("The requested BehaviorTree node class is unavailable."));
	default:
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.BehaviorTree.ActionClassMismatch"), TEXT("The requested class is not a BehaviorTree composite or task action."));
	}
}

bool ReadSubnodeActionRequest(const FAssetDocumentRuntimeValueDto& Value,
	const FAssetDocumentRuntimeValueDto*& OutClassPath, bool& bOutDecorator)
{
	const FAssetDocumentRuntimeValueDto* Kind =
		FindAssetDocumentAIBehaviorTreeField(Value, TEXT("subnode_kind"));
	if (!IsActionRequest(Value, OutClassPath)
		|| !HasAssetDocumentAIBehaviorTreeExactFields(Value, {TEXT("node_class_path"), TEXT("subnode_kind")})
		|| Kind == nullptr || Kind->Kind != EAssetDocumentRuntimeValueKind::String
		|| (Kind->StringValue != TEXT("decorator") && Kind->StringValue != TEXT("service"))) return false;
	bOutDecorator = Kind->StringValue == TEXT("decorator");
	return true;
}

bool AcquireSubnodeLeases(const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles, FAssetDocumentAIBehaviorTreeGraphLease& OutGraph,
	FAssetDocumentAIBehaviorTreeNodeLease& OutParent,
	TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
	return AcquireGraph(Invocation, Handles, OutGraph, Diagnostics)
		&& AcquireAssetDocumentAIBehaviorTreeNodeLeaseInternal(
			Invocation, Handles, TEXTVIEW("parent_node"), OutGraph, OutParent, Diagnostics);
}

enum class ESubnodeActionResolution
{
	ParentRejected,
	ActionUnavailable,
	ClassMismatch,
	Ready,
};

ESubnodeActionResolution ResolveSubnodeAction(const FAssetDocumentAIBehaviorTreeGraphLease& GraphLease,
	const FString& ClassPath, bool bDecorator, UAIGraphNode*& OutParent,
	const FAssetDocumentAIBehaviorTreeNodeLease& ParentLease,
	const UEdGraphSchema_BehaviorTree*& OutSchema, FBehaviorTreeEditorModule*& OutModule,
	UClass*& OutClass)
{
	OutParent = Cast<UAIGraphNode>(ParentLease.Node);
	OutSchema = Cast<UEdGraphSchema_BehaviorTree>(GraphLease.Graph->GetSchema());
	OutModule = FModuleManager::GetModulePtr<FBehaviorTreeEditorModule>(TEXT("BehaviorTreeEditor"));
	if (OutParent == nullptr || OutSchema == nullptr || OutModule == nullptr || !OutModule->GetClassCache().IsValid()) return ESubnodeActionResolution::ParentRejected;
	if (!ResolveActionClass(ClassPath, OutClass)) return ESubnodeActionResolution::ActionUnavailable;
	UClass* RequiredBase = bDecorator ? UBTDecorator::StaticClass() : UBTService::StaticClass();
	return IsExactClassInFamily(*OutModule, *RequiredBase, *OutClass)
		? ESubnodeActionResolution::Ready : ESubnodeActionResolution::ClassMismatch;
}

FAssetDocumentPrimitiveOutcome MakeSubnodeActionResolutionFailure(
	ESubnodeActionResolution Resolution)
{
	if (Resolution == ESubnodeActionResolution::ParentRejected)
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.BehaviorTree.SubnodeParentRejected"), TEXT("The selected parent is not an AI graph node in the scratch bundle."));
	if (Resolution == ESubnodeActionResolution::ActionUnavailable)
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.BehaviorTree.ActionUnavailable"), TEXT("The requested BehaviorTree subnode class is unavailable."));
	return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.BehaviorTree.ActionClassMismatch"), TEXT("The requested class is not in the requested BehaviorTree subnode family."));
}

struct FCommentRequest { int32 X = 0; int32 Y = 0; int32 Width = 0; int32 Height = 0; FString Text; };

bool ReadCommentRequest(const FAssetDocumentRuntimeValueDto& Value, FCommentRequest& OutRequest)
{
	const FAssetDocumentRuntimeValueDto* Bounds = FindAssetDocumentAIBehaviorTreeField(Value, TEXT("bounds"));
	const FAssetDocumentRuntimeValueDto* Text = FindAssetDocumentAIBehaviorTreeField(Value, TEXT("text"));
	double X = 0.0, Y = 0.0, Width = 0.0, Height = 0.0;
	if (!IsAssetDocumentAIBehaviorTreeScratchContext() || !EnsureAssetDocumentAIBehaviorTreeEditorAuthority() || !IsAssetDocumentAICanonicalRuntimeValue(Value)
		|| !HasAssetDocumentAIBehaviorTreeExactFields(Value, {TEXT("bounds"), TEXT("text")}) || Bounds == nullptr
		|| !HasAssetDocumentAIBehaviorTreeExactFields(*Bounds, {TEXT("h"), TEXT("w"), TEXT("x"), TEXT("y")})
		|| !ReadAssetDocumentAIBehaviorTreeFiniteCoordinate(FindAssetDocumentAIBehaviorTreeField(*Bounds, TEXT("x")), X)
		|| !ReadAssetDocumentAIBehaviorTreeFiniteCoordinate(FindAssetDocumentAIBehaviorTreeField(*Bounds, TEXT("y")), Y)
		|| !ReadAssetDocumentAIBehaviorTreeFiniteCoordinate(FindAssetDocumentAIBehaviorTreeField(*Bounds, TEXT("w")), Width)
		|| !ReadAssetDocumentAIBehaviorTreeFiniteCoordinate(FindAssetDocumentAIBehaviorTreeField(*Bounds, TEXT("h")), Height)
		|| Width <= 0.0 || Height <= 0.0 || Text == nullptr || Text->Kind != EAssetDocumentRuntimeValueKind::String
		|| !IsSafeCommentInteger(X) || !IsSafeCommentInteger(Y) || !IsSafeCommentInteger(Width) || !IsSafeCommentInteger(Height)
		|| !IsSafeCommentInteger(X + Width) || !IsSafeCommentInteger(Y + Height)) return false;
	OutRequest = {static_cast<int32>(X), static_cast<int32>(Y), static_cast<int32>(Width), static_cast<int32>(Height), Text->StringValue};
	return true;
}

UEdGraphNode_Comment* CreateExactComment(UBehaviorTreeGraph& Graph, const FCommentRequest& Request)
{
	const UAIGraphSchema* Schema = Cast<UAIGraphSchema>(Graph.GetSchema());
	if (Schema == nullptr) return nullptr;
	FGCScopeGuard Guard;
	TSharedPtr<FEdGraphSchemaAction> Action = Schema->GetCreateCommentAction();
	UEdGraphNode_Comment* Comment = Action.IsValid() ? Cast<UEdGraphNode_Comment>(Action->PerformAction(&Graph, nullptr, FVector2f(Request.X, Request.Y), false)) : nullptr;
	if (Comment == nullptr || Comment->GetOuter() != &Graph || !Graph.Nodes.Contains(Comment)) return nullptr;
	Comment->OnRenameNode(Request.Text);
	Comment->SetBounds(FSlateRect(Request.X, Request.Y, Request.X + Request.Width, Request.Y + Request.Height));
	return Comment->NodeComment == Request.Text && Comment->NodePosX == Request.X && Comment->NodePosY == Request.Y
		&& Comment->NodeWidth == Request.Width && Comment->NodeHeight == Request.Height ? Comment : nullptr;
}

struct FConnectionRequest { FString FromPin; FString ToPin; };

bool ReadConnectionRequest(const FAssetDocumentRuntimeValueDto& Value, FConnectionRequest& OutRequest)
{
	const FAssetDocumentRuntimeValueDto* From = FindAssetDocumentAIBehaviorTreeField(Value, TEXT("from_pin_name"));
	const FAssetDocumentRuntimeValueDto* To = FindAssetDocumentAIBehaviorTreeField(Value, TEXT("to_pin_name"));
	if (!IsAssetDocumentAIBehaviorTreeScratchContext() || !EnsureAssetDocumentAIBehaviorTreeEditorAuthority() || !IsAssetDocumentAICanonicalRuntimeValue(Value)
		|| !HasAssetDocumentAIBehaviorTreeExactFields(Value, {TEXT("from_pin_name"), TEXT("to_pin_name")})
		|| From == nullptr || To == nullptr || From->Kind != EAssetDocumentRuntimeValueKind::String || To->Kind != EAssetDocumentRuntimeValueKind::String
		|| From->StringValue.IsEmpty() || To->StringValue.IsEmpty()) return false;
	OutRequest = {From->StringValue, To->StringValue};
	return true;
}

bool AcquireConnectionLeases(const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
	FAssetDocumentAIBehaviorTreeGraphLease& OutGraph, FAssetDocumentAIBehaviorTreeNodeLease& OutFrom,
	FAssetDocumentAIBehaviorTreeNodeLease& OutTo, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
{
	return AcquireGraph(Invocation, Handles, OutGraph, Diagnostics)
		&& AcquireAssetDocumentAIBehaviorTreeNodeLeaseInternal(Invocation, Handles, TEXTVIEW("from_node"), OutGraph, OutFrom, Diagnostics)
		&& AcquireAssetDocumentAIBehaviorTreeNodeLeaseInternal(Invocation, Handles, TEXTVIEW("to_node"), OutGraph, OutTo, Diagnostics);
}

enum class EConnectionResult { PinMissing, CrossGraph, Rejected, Succeeded };

EConnectionResult CreateExactConnection(const FAssetDocumentAIBehaviorTreeGraphLease& GraphLease,
	const FAssetDocumentAIBehaviorTreeNodeLease& FromLease, const FAssetDocumentAIBehaviorTreeNodeLease& ToLease,
	const FConnectionRequest& Request)
{
	UEdGraphPin* From = FindExactPin(*FromLease.Node, Request.FromPin);
	UEdGraphPin* To = FindExactPin(*ToLease.Node, Request.ToPin);
	if (From == nullptr || To == nullptr) return EConnectionResult::PinMissing;
	const UEdGraphSchema* Schema = GraphLease.Graph->GetSchema();
	if (Schema == nullptr || From->GetOwningNode() != FromLease.Node || To->GetOwningNode() != ToLease.Node
		|| FromLease.Node->GetGraph() != GraphLease.Graph || ToLease.Node->GetGraph() != GraphLease.Graph) return EConnectionResult::CrossGraph;
	return Schema->TryCreateConnection(From, To) ? EConnectionResult::Succeeded : EConnectionResult::Rejected;
}

FAssetDocumentPrimitiveOutcome MakeConnectionResultFailure(EConnectionResult Result)
{
	if (Result == EConnectionResult::PinMissing) return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.BehaviorTree.PinMissing"), TEXT("The requested exact BehaviorTree pin is unavailable."));
	if (Result == EConnectionResult::CrossGraph) return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.BehaviorTree.CrossGraphConnection"), TEXT("The requested pins are not owned by the exact scratch graph."));
	return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.BehaviorTree.ConnectionRejected"), TEXT("The BehaviorTree schema rejected the requested connection."));
}

} // namespace

namespace UE::AssetDocumentAI::BehaviorTreeScratchAuthoring
{
UEdGraphNode* CreateMain(UBehaviorTreeGraph& Graph, const FString& ClassPath, int32 X, int32 Y)
{
 const auto* Schema = Cast<UEdGraphSchema_BehaviorTree>(Graph.GetSchema());
 auto* Module = FModuleManager::GetModulePtr<FBehaviorTreeEditorModule>(TEXT("BehaviorTreeEditor"));
 UClass* Class = nullptr;
 UEdGraphNode* Node = nullptr;
 if (!Schema || !Module || !Module->GetClassCache().IsValid() || !ResolveActionClass(ClassPath, Class)
  || (!IsExactClassInFamily(*Module, *UBTCompositeNode::StaticClass(), *Class)
   && !IsExactClassInFamily(*Module, *UBTTaskNode::StaticClass(), *Class))) return nullptr;
 return InvokeMainNodeAction(*Schema, *Module, Graph, *Class, X, Y, Node) ? Node : nullptr;
}

UAIGraphNode* CreateSubnode(UBehaviorTreeGraph& Graph, UAIGraphNode& Parent,
 const FString& ClassPath, bool bDecorator)
{
 const auto* Schema = Cast<UEdGraphSchema_BehaviorTree>(Graph.GetSchema());
 auto* Module = FModuleManager::GetModulePtr<FBehaviorTreeEditorModule>(TEXT("BehaviorTreeEditor"));
 UClass* Class = nullptr;
 UAIGraphNode* Node = nullptr;
 if (!Schema || !Module || !Module->GetClassCache().IsValid() || !ResolveActionClass(ClassPath, Class)
  || !IsExactClassInFamily(*Module, bDecorator ? *UBTDecorator::StaticClass() : *UBTService::StaticClass(), *Class)) return nullptr;
 return InvokeSubnodeAction(*Schema, *Module, Graph, Parent, *Class, bDecorator, Node) ? Node : nullptr;
}

UAIGraphNode* CreateComposite(UBehaviorTreeGraph& Graph, UAIGraphNode& Parent)
{
 const auto* Schema = Cast<UEdGraphSchema_BehaviorTree>(Graph.GetSchema());
 auto* Module = FModuleManager::GetModulePtr<FBehaviorTreeEditorModule>(TEXT("BehaviorTreeEditor"));
 if (!Schema || !Module || !Module->GetClassCache().IsValid()) return nullptr;
 FGCScopeGuard Guard;
 TArray<FGraphNodeClassData> Classes;
 Module->GetClassCache()->GatherClasses(UBTDecorator::StaticClass(), Classes);
 FGraphContextMenuBuilder Builder(&Graph);
 Builder.CurrentGraph = Builder.OwnerOfTemporaries;
 Builder.SelectedObjects.Add(&Parent);
 Schema->GetGraphNodeContextActions(Builder, ESubNode::Decorator);
 if (Builder.GetNumActions() != Classes.Num() + 1) return nullptr;
 const auto Before = Parent.SubNodes;
 const auto Action = Builder.GetSchemaAction(Classes.Num());
 if (!Action.IsValid() || Action->PerformAction(&Graph, nullptr, FVector2f::ZeroVector, false) != nullptr
  || Parent.SubNodes.Num() != Before.Num() + 1) return nullptr;
 for (int32 Index = 0; Index < Before.Num(); ++Index)
  if (Parent.SubNodes[Index] != Before[Index]) return nullptr;
 auto* Node = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Parent.SubNodes.Last());
 return Node && Node->GetOuter() == &Graph && Node->ParentNode == &Parent
  && Node->BoundGraph && Node->BoundGraph->GetOuter() == Node && Node->NodeGuid.IsValid() ? Node : nullptr;
}
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeNodeCreateAction(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction*)
{
	const FAssetDocumentRuntimeValueDto* ClassPath = nullptr;
	int32 X = 0;
	int32 Y = 0;
	if (!ReadNodeActionRequest(Invocation.ValueArguments, ClassPath, X, Y))
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.InvalidValueShape"),
			TEXT("The BehaviorTree node action position is invalid."));
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	FAssetDocumentAIBehaviorTreeGraphLease GraphLease;
	if (!AcquireGraph(Invocation, Handles, GraphLease, Diagnostics))
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.InvalidHandle"),
			TEXT("The graph handle is not a live BehaviorTree scratch bundle."), Diagnostics);
	}
	const UEdGraphSchema_BehaviorTree* Schema = nullptr;
	FBehaviorTreeEditorModule* Module = nullptr;
	UClass* RequestedClass = nullptr;
	UEdGraphNode* Node = nullptr;
	const EMainNodeActionResolution Resolution = ResolveMainNodeAction(
		GraphLease, ClassPath->StringValue, Schema, Module, RequestedClass);
	if (Resolution != EMainNodeActionResolution::Ready)
		return MakeMainNodeActionResolutionFailure(Resolution);
	if (!InvokeMainNodeAction(*Schema, *Module, *GraphLease.Graph, *RequestedClass,
		X, Y, Node))
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(
			TEXT("AIProvider.BehaviorTree.ActionInvocationFailed"),
			TEXT("The public BehaviorTree action did not create the exact requested node."));
	}
	return MakeAssetDocumentAIBehaviorTreeNodeActionSuccess(
		Handles, GraphLease, ClassPath->StringValue, *Node);
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeSubnodeCreateAction(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction*)
{
	const FAssetDocumentRuntimeValueDto* ClassPath = nullptr;
	bool bDecorator = false;
	if (!ReadSubnodeActionRequest(Invocation.ValueArguments, ClassPath, bDecorator))
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.InvalidValueShape"),
			TEXT("The BehaviorTree subnode request is invalid."));
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	FAssetDocumentAIBehaviorTreeGraphLease GraphLease;
	FAssetDocumentAIBehaviorTreeNodeLease ParentLease;
	if (!AcquireSubnodeLeases(Invocation, Handles, GraphLease, ParentLease, Diagnostics))
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(
			TEXT("AIProvider.BehaviorTree.SubnodeParentRejected"),
			TEXT("The parent node does not belong to the requested scratch graph."), Diagnostics);
	}
	UAIGraphNode* Parent = nullptr;
	const UEdGraphSchema_BehaviorTree* Schema = nullptr;
	FBehaviorTreeEditorModule* Module = nullptr;
	UClass* RequestedClass = nullptr;
	UAIGraphNode* Child = nullptr;
	const ESubnodeActionResolution Resolution = ResolveSubnodeAction(GraphLease,
		ClassPath->StringValue, bDecorator, Parent, ParentLease, Schema, Module, RequestedClass);
	if (Resolution != ESubnodeActionResolution::Ready)
		return MakeSubnodeActionResolutionFailure(Resolution);
	if (!InvokeSubnodeAction(*Schema, *Module, *GraphLease.Graph, *Parent,
		*RequestedClass, bDecorator, Child))
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(
			TEXT("AIProvider.BehaviorTree.ActionInvocationFailed"),
			TEXT("The public BehaviorTree subnode action did not create the exact requested child."));
	}
	return MakeAssetDocumentAIBehaviorTreeSubnodeActionSuccess(
		Handles, GraphLease, ClassPath->StringValue,
		*Parent, *Child, bDecorator);
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeCommentCreateAction(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction*)
{
	FCommentRequest Request;
	if (!ReadCommentRequest(Invocation.ValueArguments, Request))
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.InvalidValueShape"),
			TEXT("The BehaviorTree comment bounds or text are invalid."));
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	FAssetDocumentAIBehaviorTreeGraphLease GraphLease;
	if (!AcquireGraph(Invocation, Handles, GraphLease, Diagnostics))
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.InvalidHandle"),
			TEXT("The graph handle is not a live BehaviorTree scratch bundle."), Diagnostics);
	}
	UEdGraphNode_Comment* Comment = CreateExactComment(*GraphLease.Graph, Request);
	if (Comment == nullptr)
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.BehaviorTree.CommentActionFailed"),
			TEXT("The public AI graph comment action did not create the requested state."));
	return MakeAssetDocumentAIBehaviorTreeCommentActionSuccess(
		Handles, GraphLease, *Comment);
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeGraphConnect(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction*)
{
	FConnectionRequest Request;
	if (!ReadConnectionRequest(Invocation.ValueArguments, Request))
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.InvalidValueShape"), TEXT("The BehaviorTree connection pin names are invalid."));
	}
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
	FAssetDocumentAIBehaviorTreeGraphLease GraphLease;
	FAssetDocumentAIBehaviorTreeNodeLease FromLease, ToLease;
	if (!AcquireConnectionLeases(Invocation, Handles, GraphLease, FromLease, ToLease, Diagnostics))
	{
		return MakeAssetDocumentAIBehaviorTreeGraphFailure(TEXT("AIProvider.BehaviorTree.CrossGraphConnection"), TEXT("Both connection nodes must belong to the exact scratch graph."), Diagnostics);
	}
	const EConnectionResult Result = CreateExactConnection(GraphLease, FromLease, ToLease, Request);
	if (Result != EConnectionResult::Succeeded) return MakeConnectionResultFailure(Result);
	return MakeAssetDocumentAIBehaviorTreeConnectionSuccess(
		GraphLease, FromLease, ToLease);
}
