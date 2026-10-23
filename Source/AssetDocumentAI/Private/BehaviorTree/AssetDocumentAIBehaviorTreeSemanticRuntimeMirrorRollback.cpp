// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "BehaviorTree/AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollback.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackPrivate.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeDerivedCompletion.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeGraphInstall.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredGraphValidation.h"

#include "AssetDocumentEditorNativeState.h"
#include "AssetDocumentRollbackAction.h"
#include "BehaviorTree/BTAuxiliaryNode.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Decorators/BTDecorator_BlackboardBase.h"
#include "BehaviorTree/Services/BTService_BlackboardBase.h"
#include "BehaviorTree/Tasks/BTTask_BlackboardBase.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "Containers/StringConv.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "EdGraph/EdGraphPin.h"
#include "HAL/PlatformMisc.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace AIBehaviorTreeSemanticRuntimeMirrorRollbackLocal
{
constexpr const TCHAR* DiagnosticPath = TEXT("/BehaviorTreeSemanticRollback");
constexpr const TCHAR* RuntimeMirrorComponentId =
	TEXT("assetdocument.ai.behavior_tree.runtime_mirror.v1");

FAssetDocumentRuntimeDiagnostic MakeDiagnostic(const TCHAR* Code, const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = DiagnosticPath;
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Diagnostic;
}

FAssetDocumentContractValidationResult MakeValidationFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.Diagnostics.Add(MakeDiagnostic(Code, Message));
	return Result;
}

FAssetDocumentContractValidationResult MakeValidationSuccess()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

FAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorResult MakeFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorResult Result;
	Result.Diagnostics.Add(MakeDiagnostic(Code, Message));
	return Result;
}

FAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorResult MakeFailure(
	const FAssetDocumentContractValidationResult& Validation)
{
	FAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorResult Result;
	Result.Diagnostics = Validation.Diagnostics;
	return Result;
}

bool IsEditorGameThread()
{
	return GIsEditor && IsInGameThread();
}

bool IsAdmittedObject(const UObject* Object)
{
	return IsValid(Object)
		&& !Object->IsTemplate()
		&& !Object->HasAnyFlags(
			RF_ClassDefaultObject | RF_ArchetypeObject | RF_BeginDestroyed | RF_FinishDestroyed);
}

bool IsLiveBehaviorTree(const UBehaviorTree* Tree)
{
	return IsAdmittedObject(Tree)
		&& Tree->IsA(UBehaviorTree::StaticClass())
		&& Tree->GetOutermost() != nullptr;
}

bool IsExactGraphIdentity(const UBehaviorTree& Tree, const UBehaviorTreeGraph& Graph)
{
	return IsAdmittedObject(&Graph)
		&& Graph.GetClass() == UBehaviorTreeGraph::StaticClass()
		&& Tree.BTGraph.Get() == &Graph
		&& Graph.GetOuter() == &Tree
		&& Tree.GetOutermost() != nullptr
		&& Graph.GetOutermost() == Tree.GetOutermost();
}

FString PathForObject(const UObject* Object)
{
	return Object != nullptr ? Object->GetPathName() : FString(TEXT("<none>"));
}

#if WITH_DEV_AUTOMATION_TESTS
// Extra blackboard evidence lines for test node classes (installed by the automation tests).
TFunction<void(FString&, const FString&, const UBTNode&)> GBlackboardEvidenceExtensionForTest;
#endif

uint32 RotateRight(uint32 Value, uint32 Count)
{
	return (Value >> Count) | (Value << (32 - Count));
}

FString HashCanonicalUtf8(const FString& Canonical)
{
	static constexpr uint32 Constants[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
	};
	uint32 State[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};
	FTCHARToUTF8 Utf8(*Canonical);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	const uint64 BitLength = static_cast<uint64>(Bytes.Num()) * 8;
	Bytes.Add(0x80);
	while ((Bytes.Num() % 64) != 56)
	{
		Bytes.Add(0);
	}
	for (int32 Shift = 56; Shift >= 0; Shift -= 8)
	{
		Bytes.Add(static_cast<uint8>(BitLength >> Shift));
	}
	for (int32 Offset = 0; Offset < Bytes.Num(); Offset += 64)
	{
		uint32 Words[64] = {};
		for (int32 Index = 0; Index < 16; ++Index)
		{
			const int32 ByteIndex = Offset + Index * 4;
			Words[Index] = (static_cast<uint32>(Bytes[ByteIndex]) << 24)
				| (static_cast<uint32>(Bytes[ByteIndex + 1]) << 16)
				| (static_cast<uint32>(Bytes[ByteIndex + 2]) << 8)
				| static_cast<uint32>(Bytes[ByteIndex + 3]);
		}
		for (int32 Index = 16; Index < 64; ++Index)
		{
			const uint32 S0 = RotateRight(Words[Index - 15], 7) ^ RotateRight(Words[Index - 15], 18) ^ (Words[Index - 15] >> 3);
			const uint32 S1 = RotateRight(Words[Index - 2], 17) ^ RotateRight(Words[Index - 2], 19) ^ (Words[Index - 2] >> 10);
			Words[Index] = Words[Index - 16] + S0 + Words[Index - 7] + S1;
		}
		uint32 A = State[0], B = State[1], C = State[2], D = State[3];
		uint32 E = State[4], F = State[5], G = State[6], H = State[7];
		for (int32 Index = 0; Index < 64; ++Index)
		{
			const uint32 S1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
			const uint32 Choice = (E & F) ^ (~E & G);
			const uint32 Temporary1 = H + S1 + Choice + Constants[Index] + Words[Index];
			const uint32 S0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
			const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
			const uint32 Temporary2 = S0 + Majority;
			H = G; G = F; F = E; E = D + Temporary1;
			D = C; C = B; B = A; A = Temporary1 + Temporary2;
		}
		State[0] += A; State[1] += B; State[2] += C; State[3] += D;
		State[4] += E; State[5] += F; State[6] += G; State[7] += H;
	}
	FString Result;
	for (uint32 Value : State)
	{
		Result += FString::Printf(TEXT("%08x"), Value);
	}
	return Result;
}

struct FPublicDebuggerEvidence
{
	FGuid NodeGuid;
	FString WrapperClassPath;
	FString InstanceClassPath;
	uint32 Breakpoint = 0, BreakpointEnabled = 0, Current = 0, Previous = 0, Flash = 0;
	uint32 Succeeded = 0, Failed = 0, Trigger = 0, FailedTrigger = 0, Reverse = 0, BreakpointTrigger = 0;
	int32 SearchIndex = 0, SearchSize = 0, UpdateCounter = 0;
	FString Description;

	explicit FPublicDebuggerEvidence(UBehaviorTreeGraphNode& InNode)
		: NodeGuid(InNode.NodeGuid), WrapperClassPath(InNode.GetClass()->GetPathName())
		, InstanceClassPath(InNode.NodeInstance ? InNode.NodeInstance->GetClass()->GetPathName() : FString())
		, Breakpoint(InNode.bHasBreakpoint), BreakpointEnabled(InNode.bIsBreakpointEnabled)
		, Current(InNode.bDebuggerMarkCurrentlyActive), Previous(InNode.bDebuggerMarkPreviouslyActive), Flash(InNode.bDebuggerMarkFlashActive)
		, Succeeded(InNode.bDebuggerMarkSearchSucceeded), Failed(InNode.bDebuggerMarkSearchFailed), Trigger(InNode.bDebuggerMarkSearchTrigger)
		, FailedTrigger(InNode.bDebuggerMarkSearchFailedTrigger), Reverse(InNode.bDebuggerMarkSearchReverseConnection)
		, BreakpointTrigger(InNode.bDebuggerMarkBreakpointTrigger), SearchIndex(InNode.DebuggerSearchPathIndex)
		, SearchSize(InNode.DebuggerSearchPathSize), UpdateCounter(InNode.DebuggerUpdateCounter), Description(InNode.DebuggerRuntimeDescription)
	{
	}

	bool Matches(const UBehaviorTreeGraphNode& InNode) const
	{
		const UBehaviorTreeGraphNode* Value = &InNode;
		return NodeGuid == Value->NodeGuid && WrapperClassPath == Value->GetClass()->GetPathName()
			&& InstanceClassPath == (Value->NodeInstance ? Value->NodeInstance->GetClass()->GetPathName() : FString())
			&& Breakpoint == Value->bHasBreakpoint && BreakpointEnabled == Value->bIsBreakpointEnabled
			&& Current == Value->bDebuggerMarkCurrentlyActive && Previous == Value->bDebuggerMarkPreviouslyActive
			&& Flash == Value->bDebuggerMarkFlashActive && Succeeded == Value->bDebuggerMarkSearchSucceeded
			&& Failed == Value->bDebuggerMarkSearchFailed && Trigger == Value->bDebuggerMarkSearchTrigger
			&& FailedTrigger == Value->bDebuggerMarkSearchFailedTrigger && Reverse == Value->bDebuggerMarkSearchReverseConnection
			&& BreakpointTrigger == Value->bDebuggerMarkBreakpointTrigger && SearchIndex == Value->DebuggerSearchPathIndex
			&& SearchSize == Value->DebuggerSearchPathSize && UpdateCounter == Value->DebuggerUpdateCounter
			&& Description == Value->DebuggerRuntimeDescription;
	}
};

void CaptureDebuggerEvidence(UBehaviorTreeGraphNode& Node, TSet<UBehaviorTreeGraphNode*>& Seen, TArray<FPublicDebuggerEvidence>& Out)
{
	if (Seen.Contains(&Node))
	{
		return;
	}
	Seen.Add(&Node);
	Out.Emplace(Node);
	for (UBehaviorTreeGraphNode* Child : Node.Services)
	{
		if (Child != nullptr) CaptureDebuggerEvidence(*Child, Seen, Out);
	}
	for (UBehaviorTreeGraphNode* Child : Node.Decorators)
	{
		if (Child != nullptr) CaptureDebuggerEvidence(*Child, Seen, Out);
	}
}

TArray<FPublicDebuggerEvidence> CaptureDebuggerEvidence(UBehaviorTreeGraph& Graph)
{
	TArray<FPublicDebuggerEvidence> Result;
	TSet<UBehaviorTreeGraphNode*> Seen;
	for (UEdGraphNode* RawNode : Graph.Nodes)
	{
		if (UBehaviorTreeGraphNode* Node = Cast<UBehaviorTreeGraphNode>(RawNode))
		{
			CaptureDebuggerEvidence(*Node, Seen, Result);
		}
	}
	return Result;
}

void CollectCurrentDebuggerNodes(UBehaviorTreeGraphNode& Node, TSet<UBehaviorTreeGraphNode*>& Out)
{
	if (Out.Contains(&Node))
	{
		return;
	}
	Out.Add(&Node);
	for (UBehaviorTreeGraphNode* Child : Node.Services)
	{
		if (Child != nullptr) CollectCurrentDebuggerNodes(*Child, Out);
	}
	for (UBehaviorTreeGraphNode* Child : Node.Decorators)
	{
		if (Child != nullptr) CollectCurrentDebuggerNodes(*Child, Out);
	}
}

bool DoesDebuggerEvidenceMatch(
	const TArray<FPublicDebuggerEvidence>& Evidence,
	UBehaviorTreeGraph& Graph)
{
	TSet<UBehaviorTreeGraphNode*> CurrentNodes;
	for (UEdGraphNode* RawNode : Graph.Nodes)
	{
		if (UBehaviorTreeGraphNode* Node = Cast<UBehaviorTreeGraphNode>(RawNode))
		{
			CollectCurrentDebuggerNodes(*Node, CurrentNodes);
		}
	}
	if (Evidence.Num() != CurrentNodes.Num()) return false;
	TSet<FGuid> SeenGuids;
	for (const FPublicDebuggerEvidence& Item : Evidence)
	{
		if (!Item.NodeGuid.IsValid() || SeenGuids.Contains(Item.NodeGuid)) return false;
		SeenGuids.Add(Item.NodeGuid);
		const UBehaviorTreeGraphNode* Match = nullptr;
		for (const UBehaviorTreeGraphNode* Node : CurrentNodes)
		{
			if (Node->NodeGuid != Item.NodeGuid) continue;
			if (Match != nullptr) return false;
			Match = Node;
		}
		if (Match == nullptr || !Item.Matches(*Match)) return false;
	}
	return true;
}

void AppendBlackboardEvidence(FString& Lines, const FString& RolePath, const UBTNode& Node)
{
	FString SelectedKey;
	if (const UBTTask_BlackboardBase* Task = Cast<UBTTask_BlackboardBase>(&Node)) SelectedKey = Task->GetSelectedBlackboardKey().ToString();
	if (const UBTService_BlackboardBase* Service = Cast<UBTService_BlackboardBase>(&Node)) SelectedKey = Service->GetSelectedBlackboardKey().ToString();
	if (const UBTDecorator_BlackboardBase* Decorator = Cast<UBTDecorator_BlackboardBase>(&Node)) SelectedKey = Decorator->GetSelectedBlackboardKey().ToString();
	Lines += FString::Printf(TEXT("blackboard|%s|%s|%s" LINE_TERMINATOR), *RolePath, *PathForObject(Node.GetBlackboardAsset()), *SelectedKey);
#if WITH_DEV_AUTOMATION_TESTS
	if (GBlackboardEvidenceExtensionForTest)
	{
		GBlackboardEvidenceExtensionForTest(Lines, RolePath, Node);
	}
#endif
}

void AppendDecoratorOps(FString& Lines, const FString& OwnerRolePath, const TCHAR* Scope, int32 ChildIndex, TConstArrayView<FBTDecoratorLogic> Operations)
{
	for (int32 Index = 0; Index < Operations.Num(); ++Index)
	{
		const FBTDecoratorLogic& Operation = Operations[Index];
		Lines += FString::Printf(TEXT("decorator-op|%s|%s|%d|%d|%d|%d" LINE_TERMINATOR), *OwnerRolePath, Scope,
			ChildIndex, Index, static_cast<int32>(Operation.Operation.GetValue()), static_cast<int32>(Operation.Number));
	}
}

void AppendNodeEvidence(FString& ExecutionLines, FString& BlackboardLines, const FString& RolePath, const FString& ParentRolePath,
	int32 ChildIndex, int32 AuxiliaryIndex, const UBTNode& Node, TSet<const UBTNode*>& Visited, int32& NodeCount)
{
	if (Visited.Contains(&Node))
	{
		return;
	}
	Visited.Add(&Node);
	++NodeCount;
	const UBTCompositeNode* Composite = Cast<UBTCompositeNode>(&Node);
	const UBTDecorator* Decorator = Cast<UBTDecorator>(&Node);
	ExecutionLines += FString::Printf(TEXT("%s|%s|%u|%u|%u|%s|%d|%d|%u|%d" LINE_TERMINATOR),
		*RolePath, *Node.GetClass()->GetPathName(), Node.GetExecutionIndex(), Node.GetMemoryOffset(), Node.GetTreeDepth(),
		*ParentRolePath, ChildIndex, AuxiliaryIndex, Composite != nullptr ? Composite->GetLastExecutionIndex() : 0,
		Decorator != nullptr ? static_cast<int32>(Decorator->GetFlowAbortMode()) : 0);
	AppendBlackboardEvidence(BlackboardLines, RolePath, Node);
	if (Composite != nullptr)
	{
		for (int32 Index = 0; Index < Composite->Services.Num(); ++Index)
		{
			if (const UBTService* Service = Composite->Services[Index])
			{
				AppendNodeEvidence(ExecutionLines, BlackboardLines,
					FString::Printf(TEXT("%s/service/%d"), *RolePath, Index), RolePath, -1, Index, *Service, Visited, NodeCount);
			}
		}
	}
	else if (const UBTTaskNode* Task = Cast<UBTTaskNode>(&Node))
	{
		for (int32 Index = 0; Index < Task->Services.Num(); ++Index)
		{
			if (const UBTService* Service = Task->Services[Index])
			{
				AppendNodeEvidence(ExecutionLines, BlackboardLines,
					FString::Printf(TEXT("%s/service/%d"), *RolePath, Index), RolePath, -1, Index, *Service, Visited, NodeCount);
			}
		}
	}
	if (Composite == nullptr)
	{
		return;
	}
	for (int32 Index = 0; Index < Composite->Children.Num(); ++Index)
	{
		const FBTCompositeChild& Child = Composite->Children[Index];
		const FString ChildRole = FString::Printf(TEXT("%s/child/%d"), *RolePath, Index);
		AppendDecoratorOps(ExecutionLines, RolePath, TEXT("child"), Index, Child.DecoratorOps);
		for (int32 DecoratorIndex = 0; DecoratorIndex < Child.Decorators.Num(); ++DecoratorIndex)
		{
			if (const UBTDecorator* ChildDecorator = Child.Decorators[DecoratorIndex])
			{
				AppendNodeEvidence(ExecutionLines, BlackboardLines,
					FString::Printf(TEXT("%s/decorator/%d"), *ChildRole, DecoratorIndex), RolePath, Index, DecoratorIndex,
					*ChildDecorator, Visited, NodeCount);
			}
		}
		if (const UBTCompositeNode* ChildComposite = Child.ChildComposite)
		{
			AppendNodeEvidence(ExecutionLines, BlackboardLines, ChildRole, RolePath, Index, -1, *ChildComposite, Visited, NodeCount);
		}
		else if (const UBTTaskNode* ChildTask = Child.ChildTask)
		{
			AppendNodeEvidence(ExecutionLines, BlackboardLines, ChildRole, RolePath, Index, -1, *ChildTask, Visited, NodeCount);
		}
	}
}

UBehaviorTreeGraphNode* FindAuthoredRuntimeRootGraphNode(UBehaviorTreeGraph& Graph)
{
	UBehaviorTreeGraphNode_Root* RootGraphNode = nullptr;
	for (UEdGraphNode* RawNode : Graph.Nodes)
	{
		if (UBehaviorTreeGraphNode_Root* Candidate = Cast<UBehaviorTreeGraphNode_Root>(RawNode))
		{
			if (RootGraphNode != nullptr && RootGraphNode != Candidate) return nullptr;
			RootGraphNode = Candidate;
		}
	}
	if (RootGraphNode == nullptr) return nullptr;
	UBehaviorTreeGraphNode* RuntimeRootGraphNode = nullptr;
	for (UEdGraphPin* Pin : RootGraphNode->Pins)
	{
		if (Pin == nullptr || Pin->Direction != EGPD_Output) continue;
		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			UBehaviorTreeGraphNode* Candidate = LinkedPin != nullptr
				? Cast<UBehaviorTreeGraphNode>(LinkedPin->GetOwningNode()) : nullptr;
			if (Candidate == nullptr || (RuntimeRootGraphNode != nullptr && RuntimeRootGraphNode != Candidate)) return nullptr;
			RuntimeRootGraphNode = Candidate;
		}
	}
	return RuntimeRootGraphNode;
}

bool FailAuthoredProjection(FAssetDocumentContractValidationResult& Failure, const TCHAR* Message)
{
	Failure = MakeValidationFailure(TEXT("BehaviorTreeSemanticRollback.AuthoredGraphIncoherent"), Message);
	return false;
}

bool GetOrderedAuthoredChildren(
	UBehaviorTreeGraph& Graph,
	const UEdGraphNode& Parent,
	TArray<UBehaviorTreeGraphNode*>& Out,
	FAssetDocumentContractValidationResult& Failure)
{
	TSet<UBehaviorTreeGraphNode*> LinkedNodes;
	for (UEdGraphPin* Pin : Parent.Pins)
	{
		if (Pin == nullptr) return FailAuthoredProjection(Failure, TEXT("The authored graph contains a null pin."));
		if (Pin->Direction != EGPD_Output) continue;
		TArray<UBehaviorTreeGraphNode*> PinNodes;
		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			UBehaviorTreeGraphNode* Child = LinkedPin != nullptr && LinkedPin->Direction == EGPD_Input
				? Cast<UBehaviorTreeGraphNode>(LinkedPin->GetOwningNode()) : nullptr;
			if (Child == nullptr || !Graph.Nodes.Contains(Child) || Child->GetOwnerBehaviorTreeGraph() != &Graph || LinkedNodes.Contains(Child))
			{
				return FailAuthoredProjection(Failure, TEXT("The authored graph has an invalid or duplicate child link."));
			}
			LinkedNodes.Add(Child);
			PinNodes.Add(Child);
		}
		PinNodes.Sort([](const UBehaviorTreeGraphNode& Left, const UBehaviorTreeGraphNode& Right)
		{
			return Left.NodePosX != Right.NodePosX ? Left.NodePosX < Right.NodePosX : Left.NodePosY < Right.NodePosY;
		});
		for (int32 Index = 1; Index < PinNodes.Num(); ++Index)
		{
			if (PinNodes[Index - 1]->NodePosX == PinNodes[Index]->NodePosX
				&& PinNodes[Index - 1]->NodePosY == PinNodes[Index]->NodePosY)
			{
				return FailAuthoredProjection(Failure, TEXT("An authored output pin has an ambiguous child layout order."));
			}
		}
		Out.Append(PinNodes);
	}
	return true;
}

bool MatchesDecoratorOps(TConstArrayView<FBTDecoratorLogic> Actual, TConstArrayView<FBTDecoratorLogic> Expected)
{
	if (Actual.Num() != Expected.Num()) return false;
	for (int32 Index = 0; Index < Actual.Num(); ++Index)
	{
		if (Actual[Index].Operation != Expected[Index].Operation || Actual[Index].Number != Expected[Index].Number) return false;
	}
	return true;
}

bool AdmitAuthoredProjectionNode(
	const UBTNode* Node,
	TSet<const UBTNode*>& SeenNodes,
	FAssetDocumentContractValidationResult& Failure)
{
	if (Node == nullptr || SeenNodes.Contains(Node))
	{
		return FailAuthoredProjection(Failure, TEXT("The authored graph reuses or omits a runtime node instance."));
	}
	SeenNodes.Add(Node);
	return true;
}

bool ValidateRuntimeMainLink(
	const UBTNode& Node,
	const UBTCompositeNode* ExpectedParent,
	int32 ExpectedChildIndex,
	FAssetDocumentContractValidationResult& Failure)
{
	if (Node.GetParentNode() != ExpectedParent)
	{
		return FailAuthoredProjection(Failure, TEXT("A runtime main node has the wrong public parent link."));
	}
	if (ExpectedParent == nullptr)
	{
		return ExpectedChildIndex == INDEX_NONE
			|| FailAuthoredProjection(Failure, TEXT("The runtime root has an unexpected child index."));
	}
	if (ExpectedChildIndex < 0 || ExpectedChildIndex >= BTSpecialChild::OwnedByComposite
		|| ExpectedParent->GetChildIndex(Node) != ExpectedChildIndex)
	{
		return FailAuthoredProjection(Failure, TEXT("A runtime main node has the wrong public child index."));
	}
	return true;
}

bool ValidateRuntimeAuxiliaryLink(
	const UBTAuxiliaryNode& Node,
	const UBTCompositeNode* ExpectedParent,
	int32 ExpectedChildIndex,
	FAssetDocumentContractValidationResult& Failure)
{
	if (Node.GetParentNode() != ExpectedParent)
	{
		return FailAuthoredProjection(Failure, TEXT("A runtime auxiliary node has the wrong public parent link."));
	}
	if (ExpectedChildIndex == INDEX_NONE) return true;
	if (ExpectedChildIndex < 0 || ExpectedChildIndex > MAX_uint8
		|| Node.GetChildIndex() != static_cast<uint8>(ExpectedChildIndex))
	{
		return FailAuthoredProjection(Failure, TEXT("A runtime auxiliary node has the wrong public child link."));
	}
	return true;
}

bool ValidateAuthoredDecorators(
	const UBehaviorTreeGraphNode& Owner,
	const TArray<TObjectPtr<UBTDecorator>>* RuntimeDecorators,
	const TArray<FBTDecoratorLogic>* RuntimeOps,
	const UBTCompositeNode* ExpectedParent,
	int32 ExpectedChildIndex,
	bool bCompareRuntime,
	TSet<const UBTNode*>& SeenNodes,
	FAssetDocumentContractValidationResult& Failure)
{
	TArray<UBTDecorator*> ExpectedDecorators;
	TArray<FBTDecoratorLogic> PublicFragments;
	int32 WrapperCount = 0;
	int32 CompositeWrapperCount = 0;
	for (UBehaviorTreeGraphNode* Wrapper : Owner.Decorators)
	{
		if (Wrapper == nullptr) return FailAuthoredProjection(Failure, TEXT("The authored graph has a null decorator wrapper."));
		if (Wrapper->bInjectedNode) continue;
		if (const UBehaviorTreeGraphNode_CompositeDecorator* Composite = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Wrapper))
		{
			if (Composite->BoundGraph == nullptr) return FailAuthoredProjection(Failure, TEXT("The authored composite decorator has no bound graph."));
			Composite->CollectDecoratorData(ExpectedDecorators, PublicFragments);
			++CompositeWrapperCount;
		}
		else if (const UBehaviorTreeGraphNode_Decorator* Decorator = Cast<UBehaviorTreeGraphNode_Decorator>(Wrapper))
		{
			Decorator->CollectDecoratorData(ExpectedDecorators, PublicFragments);
		}
		else return FailAuthoredProjection(Failure, TEXT("The authored graph has a decorator wrapper of the wrong type."));
		++WrapperCount;
	}
	for (const UBTDecorator* Decorator : ExpectedDecorators)
	{
		if (!AdmitAuthoredProjectionNode(Decorator, SeenNodes, Failure)) return false;
	}
	TArray<FBTDecoratorLogic> ExpectedOps;
	if (CompositeWrapperCount == 0)
	{
		PublicFragments.Reset();
	}
	else if (WrapperCount == 1)
	{
		ExpectedOps = MoveTemp(PublicFragments);
	}
	else
	{
		FBTDecoratorLogic& Prefix = ExpectedOps.AddDefaulted_GetRef();
		Prefix.Operation = EBTDecoratorLogic::And;
		Prefix.Number = static_cast<uint16>(WrapperCount);
		ExpectedOps.Append(PublicFragments);
	}
	if (!bCompareRuntime) return true;
	if (RuntimeDecorators == nullptr || RuntimeOps == nullptr
		|| RuntimeDecorators->Num() != ExpectedDecorators.Num() || !MatchesDecoratorOps(*RuntimeOps, ExpectedOps))
	{
		return FailAuthoredProjection(Failure, TEXT("The runtime decorator projection does not match the public authored wrapper projection."));
	}
	for (int32 Index = 0; Index < ExpectedDecorators.Num(); ++Index)
	{
		if ((*RuntimeDecorators)[Index].Get() != ExpectedDecorators[Index])
		{
			return FailAuthoredProjection(Failure, TEXT("The runtime decorator identity does not match the authored wrapper projection."));
		}
		if (!ValidateRuntimeAuxiliaryLink(*ExpectedDecorators[Index], ExpectedParent, ExpectedChildIndex, Failure)) return false;
	}
	return true;
}

bool ValidateAuthoredServices(
	const UBehaviorTreeGraphNode& Owner,
	const TArray<TObjectPtr<UBTService>>* RuntimeServices,
	const UBTCompositeNode* ExpectedParent,
	int32 ExpectedChildIndex,
	bool bCompareRuntime,
	TSet<const UBTNode*>& SeenNodes,
	FAssetDocumentContractValidationResult& Failure)
{
	if (bCompareRuntime && (RuntimeServices == nullptr || RuntimeServices->Num() != Owner.Services.Num()))
	{
		return FailAuthoredProjection(Failure, TEXT("The runtime service count does not match the authored service order."));
	}
	for (int32 Index = 0; Index < Owner.Services.Num(); ++Index)
	{
		const UBehaviorTreeGraphNode* ServiceGraphNode = Owner.Services[Index];
		const UBTService* Service = ServiceGraphNode != nullptr ? Cast<UBTService>(ServiceGraphNode->NodeInstance.Get()) : nullptr;
		if (ServiceGraphNode == nullptr || !ServiceGraphNode->IsA(UBehaviorTreeGraphNode_Service::StaticClass()) || Service == nullptr)
		{
			return FailAuthoredProjection(Failure, TEXT("The authored graph has a null or wrong-type service node."));
		}
		if (!AdmitAuthoredProjectionNode(Service, SeenNodes, Failure)) return false;
		if (bCompareRuntime && (*RuntimeServices)[Index].Get() != Service)
		{
			return FailAuthoredProjection(Failure, TEXT("The runtime service identity does not match the authored service order."));
		}
		if (bCompareRuntime && !ValidateRuntimeAuxiliaryLink(*Service, ExpectedParent, ExpectedChildIndex, Failure)) return false;
	}
	return true;
}

bool ValidateAuthoredMainNode(
	UBehaviorTreeGraph& Graph,
	UBehaviorTreeGraphNode& AuthoredNode,
	const UBTCompositeNode* RuntimeComposite,
	const UBTTaskNode* RuntimeTask,
	const UBTCompositeNode* ExpectedParent,
	int32 ExpectedChildIndex,
	bool bCompareRuntime,
	TSet<UBehaviorTreeGraphNode*>& Active,
	TSet<UBehaviorTreeGraphNode*>& Complete,
	TSet<const UBTNode*>& SeenNodes,
	FAssetDocumentContractValidationResult& Failure)
{
	if (!IsAdmittedObject(&AuthoredNode) || Active.Contains(&AuthoredNode) || Complete.Contains(&AuthoredNode))
	{
		return FailAuthoredProjection(Failure, TEXT("The authored BehaviorTree graph has a cycle or reused main node."));
	}
	const UBTCompositeNode* AuthoredComposite = Cast<UBTCompositeNode>(AuthoredNode.NodeInstance.Get());
	const UBTTaskNode* AuthoredTask = Cast<UBTTaskNode>(AuthoredNode.NodeInstance.Get());
	if ((AuthoredComposite == nullptr && AuthoredTask == nullptr) || (AuthoredComposite != nullptr && AuthoredTask != nullptr))
	{
		return FailAuthoredProjection(Failure, TEXT("The authored main graph node has a null or wrong-type node instance."));
	}
	if (!AdmitAuthoredProjectionNode(AuthoredComposite != nullptr ? static_cast<const UBTNode*>(AuthoredComposite) : static_cast<const UBTNode*>(AuthoredTask), SeenNodes, Failure)) return false;
	const UBTNode* AuthoredMain = AuthoredComposite != nullptr
		? static_cast<const UBTNode*>(AuthoredComposite) : static_cast<const UBTNode*>(AuthoredTask);
	if (bCompareRuntime && !ValidateRuntimeMainLink(*AuthoredMain, ExpectedParent, ExpectedChildIndex, Failure)) return false;
	Active.Add(&AuthoredNode);
	if (AuthoredComposite != nullptr)
	{
		if ((bCompareRuntime && RuntimeComposite != AuthoredComposite) || !ValidateAuthoredServices(AuthoredNode,
			RuntimeComposite != nullptr ? &RuntimeComposite->Services : nullptr,
			AuthoredComposite, BTSpecialChild::OwnedByComposite, bCompareRuntime, SeenNodes, Failure)) return false;
		TArray<UBehaviorTreeGraphNode*> Children;
		if (!GetOrderedAuthoredChildren(Graph, AuthoredNode, Children, Failure)) return false;
		if (bCompareRuntime && (RuntimeComposite == nullptr || RuntimeComposite->Children.Num() != Children.Num()))
		{
			return FailAuthoredProjection(Failure, TEXT("The runtime child count does not match the authored layout order."));
		}
		for (int32 Index = 0; Index < Children.Num(); ++Index)
		{
			if (Index >= BTSpecialChild::OwnedByComposite)
			{
				return FailAuthoredProjection(Failure, TEXT("The authored graph exceeds the public auxiliary child-index range."));
			}
			UBehaviorTreeGraphNode& Child = *Children[Index];
			const FBTCompositeChild* RuntimeChild = bCompareRuntime ? &RuntimeComposite->Children[Index] : nullptr;
			if (!ValidateAuthoredDecorators(Child,
				RuntimeChild != nullptr ? &RuntimeChild->Decorators : nullptr,
				RuntimeChild != nullptr ? &RuntimeChild->DecoratorOps : nullptr,
				AuthoredComposite, Index, bCompareRuntime, SeenNodes, Failure)) return false;
			if (bCompareRuntime && ((RuntimeChild->ChildComposite == nullptr) == (RuntimeChild->ChildTask == nullptr)))
			{
				return FailAuthoredProjection(Failure, TEXT("The runtime child must project exactly one main node kind."));
			}
			if (!ValidateAuthoredMainNode(Graph, Child,
				RuntimeChild != nullptr ? RuntimeChild->ChildComposite.Get() : nullptr,
				RuntimeChild != nullptr ? RuntimeChild->ChildTask.Get() : nullptr,
				AuthoredComposite, Index, bCompareRuntime, Active, Complete, SeenNodes, Failure)) return false;
		}
	}
	else
	{
		if ((bCompareRuntime && RuntimeTask != AuthoredTask) || !ValidateAuthoredServices(AuthoredNode,
			RuntimeTask != nullptr ? &RuntimeTask->Services : nullptr,
			ExpectedParent, ExpectedChildIndex, bCompareRuntime, SeenNodes, Failure)) return false;
		TArray<UBehaviorTreeGraphNode*> Children;
		if (!GetOrderedAuthoredChildren(Graph, AuthoredNode, Children, Failure) || Children.Num() != 0)
		{
			return FailAuthoredProjection(Failure, TEXT("An authored task node has an invalid child link."));
		}
	}
	Active.Remove(&AuthoredNode);
	Complete.Add(&AuthoredNode);
	return true;
}

bool ValidateAuthoredRuntimeProjection(
	UBehaviorTree& Tree,
	UBehaviorTreeGraph& Graph,
	bool bCompareRuntime,
	FAssetDocumentContractValidationResult& Failure,
	bool bAllowEmptyAuthoredGraph = false)
{
	UBehaviorTreeGraphNode_Root* Root = nullptr;
	for (UEdGraphNode* RawNode : Graph.Nodes)
	{
		if (RawNode == nullptr) return FailAuthoredProjection(Failure, TEXT("The authored graph contains a null top-level node."));
		if (UBehaviorTreeGraphNode_Root* Candidate = Cast<UBehaviorTreeGraphNode_Root>(RawNode))
		{
			if (Root != nullptr) return FailAuthoredProjection(Failure, TEXT("The authored graph has more than one root node."));
			Root = Candidate;
		}
	}
	if (Root == nullptr || Root->Decorators.Num() != 0 || Root->Services.Num() != 0)
	{
		return FailAuthoredProjection(Failure, TEXT("The authored graph has no coherent BehaviorTree root closure."));
	}
	if (bCompareRuntime && Root->BlackboardAsset.Get() != Tree.GetBlackboardAsset())
	{
		return FailAuthoredProjection(Failure, TEXT("The runtime Blackboard does not match the authored root Blackboard."));
	}
	TArray<UBehaviorTreeGraphNode*> RootChildren;
	if (!GetOrderedAuthoredChildren(Graph, *Root, RootChildren, Failure)) return false;
	if (RootChildren.IsEmpty() && bAllowEmptyAuthoredGraph)
	{
		using namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback;
		FString Identity, Diagnostic;
		if (!UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::IsExactEmptyAuthoredGraph(Graph)
			|| !CaptureGraphIdentity(Tree, Identity, Diagnostic, EAuthoredGraphReadbackVersion::V2)
			|| !ValidateInstalledGraphOwnership(Tree).bValid
			|| Root->BlackboardAsset.Get()!=Tree.GetBlackboardAsset()
			|| Tree.RootNode || !Tree.RootDecorators.IsEmpty() || !Tree.RootDecoratorOps.IsEmpty())
		{
			return FailAuthoredProjection(Failure, TEXT("The empty authored graph must contain only its owned Root and comments with an exact Blackboard header and empty runtime mirror."));
		}
		return true;
	}
	if (RootChildren.Num() != 1)
	{
		return FailAuthoredProjection(Failure, TEXT("The authored root must have exactly one ordered runtime child."));
	}
	UBehaviorTreeGraphNode& RootChild = *RootChildren[0];
	const UBTCompositeNode* AuthoredRoot = Cast<UBTCompositeNode>(RootChild.NodeInstance.Get());
	if (AuthoredRoot == nullptr || (bCompareRuntime && Tree.RootNode.Get() != AuthoredRoot))
	{
		return FailAuthoredProjection(Failure, TEXT("The authored root does not project the exact runtime root identity."));
	}
	TSet<UBehaviorTreeGraphNode*> Active;
	TSet<UBehaviorTreeGraphNode*> Complete;
	TSet<const UBTNode*> SeenNodes;
	if (!ValidateAuthoredDecorators(RootChild, &Tree.RootDecorators, &Tree.RootDecoratorOps,
		nullptr, INDEX_NONE, bCompareRuntime, SeenNodes, Failure)) return false;
	if (!ValidateAuthoredMainNode(Graph, RootChild, bCompareRuntime ? Tree.RootNode.Get() : nullptr, nullptr,
		nullptr, INDEX_NONE, bCompareRuntime, Active, Complete, SeenNodes, Failure)) return false;
	for (UEdGraphNode* RawNode : Graph.Nodes)
	{
		if (RawNode->IsA(UEdGraphNode_Comment::StaticClass())) continue;
		if (RawNode != Root && (!RawNode->IsA(UBehaviorTreeGraphNode::StaticClass()) || !Complete.Contains(static_cast<UBehaviorTreeGraphNode*>(RawNode))))
		{
			return FailAuthoredProjection(Failure, TEXT("The authored graph contains an orphan or wrong-type top-level node."));
		}
	}
	return true;
}

bool ReadSemanticEvidence(
	UBehaviorTree& Tree,
	UBehaviorTreeGraph& Graph,
	FAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorResult& Out,
	FAssetDocumentContractValidationResult& Failure,
	bool bAllowEmptyAuthoredGraph = false)
{
	if (!Tree.RootNode && !bAllowEmptyAuthoredGraph)
	{
		Failure = MakeValidationFailure(TEXT("BehaviorTreeSemanticRollback.SemanticReadbackFailed"), TEXT("The rebuilt BehaviorTree exposes no runtime root."));
		return false;
	}
	FAssetDocumentContractValidationResult ProjectionFailure;
	if (!ValidateAuthoredRuntimeProjection(Tree, Graph, true, ProjectionFailure, bAllowEmptyAuthoredGraph))
	{
		Failure = MakeValidationFailure(TEXT("BehaviorTreeSemanticRollback.SemanticReadbackFailed"), TEXT("The runtime mirror does not match the authored semantic projection."));
		return false;
	}
	UBehaviorTreeGraphNode* RuntimeRootGraphNode = FindAuthoredRuntimeRootGraphNode(Graph);
	if (Tree.RootNode && (RuntimeRootGraphNode == nullptr || RuntimeRootGraphNode->NodeInstance.Get() != Tree.RootNode.Get()))
	{
		Failure = MakeValidationFailure(TEXT("BehaviorTreeSemanticRollback.SemanticReadbackFailed"), TEXT("The authored root graph node does not project the exact runtime root identity."));
		return false;
	}
	FString ExecutionLines;
	FString BlackboardLines = FString::Printf(TEXT("tree-blackboard|%s" LINE_TERMINATOR), *PathForObject(Tree.GetBlackboardAsset()));
	AppendDecoratorOps(ExecutionLines, TEXT("root"), TEXT("root"), -1, Tree.RootDecoratorOps);
	TSet<const UBTNode*> Visited;
	int32 NodeCount = 0;
	for (int32 Index = 0; Index < Tree.RootDecorators.Num(); ++Index)
	{
		if (const UBTDecorator* RootDecorator = Tree.RootDecorators[Index])
		{
			AppendNodeEvidence(ExecutionLines, BlackboardLines,
				FString::Printf(TEXT("root/decorator/%d"), Index), TEXT("root"), -1, Index,
				*RootDecorator, Visited, NodeCount);
		}
	}
	if (Tree.RootNode)
	{
		AppendNodeEvidence(ExecutionLines, BlackboardLines, TEXT("root"), TEXT("<none>"), -1, -1, *Tree.RootNode, Visited, NodeCount);
	}
	Out.RootNodeClassPath = Tree.RootNode ? Tree.RootNode->GetClass()->GetPathName() : FString();
	Out.NodeCount = NodeCount;
	Out.ExecutionOrderHash = HashCanonicalUtf8(ExecutionLines);
	Out.BlackboardEvidenceHash = HashCanonicalUtf8(BlackboardLines);
	if (Out.ExecutionOrderHash.Len() != 64 || Out.BlackboardEvidenceHash.Len() != 64)
	{
		Failure = MakeValidationFailure(TEXT("BehaviorTreeSemanticRollback.SemanticReadbackFailed"), TEXT("Canonical semantic hashing failed."));
		return false;
	}
	return true;
}

void RebuildStockMirror(UBehaviorTreeGraph& Graph, bool bInitializeFromAsset)
{
	if (bInitializeFromAsset)
	{
		Graph.UpdateAsset(UBehaviorTreeGraph::KeepRebuildCounter);
	}
	else
	{
		// The admitted CurrentGraph already owns initialized node instances.
		// Rebuild their runtime links without invoking project asset lifecycle.
		Graph.CreateBTFromGraph(FindAuthoredRuntimeRootGraphNode(Graph));
		Graph.UpdateErrorMessages();
	}
}

bool GForceSemanticReadbackFailureForTest = false;

class FBehaviorTreeSemanticRuntimeMirrorState final
{
public:
	FBehaviorTreeSemanticRuntimeMirrorState(UBehaviorTree& InTree, UBehaviorTreeGraph& InGraph, bool bInInitializeFromAsset,
		bool bInAllowEmptyAuthoredGraph)
		: Tree(&InTree), Graph(&InGraph), Package(InTree.GetOutermost())
		, TreePath(InTree.GetPathName()), GraphPath(InGraph.GetPathName()), PackagePath(InTree.GetOutermost()->GetPathName())
		, GraphGuid(InGraph.GraphGuid)
		, bInitializeFromAsset(bInInitializeFromAsset)
		, bAllowEmptyAuthoredGraph(bInAllowEmptyAuthoredGraph)
	{
	}


	FAssetDocumentContractValidationResult Recomplete()
	{
		UBehaviorTree* LiveTree = Tree.Get();
		UBehaviorTreeGraph* LiveGraph = Graph.Get();
		UPackage* LivePackage = Package.Get();
		if (!IsEditorGameThread() || !IsLiveBehaviorTree(LiveTree) || LiveGraph == nullptr || LivePackage == nullptr
			|| !IsExactGraphIdentity(*LiveTree, *LiveGraph)
			|| LiveTree->GetPathName() != TreePath || LiveGraph->GetPathName() != GraphPath || LivePackage->GetPathName() != PackagePath
			|| LiveGraph->GraphGuid != GraphGuid || LiveGraph->IsLocked())
		{
			return MakeValidationFailure(TEXT("BehaviorTreeSemanticRollback.FinalizerIdentityInvalid"), TEXT("Rollback finalization requires the captured live BehaviorTree identity."));
		}
		FAssetDocumentContractValidationResult ProjectionFailure;
		if (!ValidateAuthoredRuntimeProjection(*LiveTree, *LiveGraph, false, ProjectionFailure, bAllowEmptyAuthoredGraph))
		{
			return ProjectionFailure;
		}
		// Ordinary rollback owns the restored editor state. Preserve that state
		// across this rebuild; forward object identities are not restore truth.
		const bool bDirtyAfterAuthoredRollback = LivePackage->IsDirty();
		const int32 ModCounter = LiveGraph->ModCounter;
		const auto DebuggerEvidence = CaptureDebuggerEvidence(*LiveGraph);
		RebuildStockMirror(*LiveGraph, bInitializeFromAsset);
		FAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorResult Evidence;
		FAssetDocumentContractValidationResult ReadbackFailure;
		const bool bReadbackSucceeded = ReadSemanticEvidence(*LiveTree, *LiveGraph, Evidence, ReadbackFailure, bAllowEmptyAuthoredGraph);
		const bool bCounterMatches = LiveGraph->ModCounter == ModCounter;
		const bool bDebuggerMatches = DoesDebuggerEvidenceMatch(DebuggerEvidence, *LiveGraph);
		LivePackage->SetDirtyFlag(bDirtyAfterAuthoredRollback);
		const bool bDirtyMatches = LivePackage->IsDirty() == bDirtyAfterAuthoredRollback;
		if (!bReadbackSucceeded)
		{
			return ReadbackFailure;
		}
		if (GForceSemanticReadbackFailureForTest)
		{
			return MakeValidationFailure(TEXT("BehaviorTreeSemanticRollback.SemanticReadbackFailed"), TEXT("Injected semantic readback failure."));
		}
		if (!bCounterMatches || !bDebuggerMatches || !bDirtyMatches)
		{
			return MakeValidationFailure(TEXT("BehaviorTreeSemanticRollback.FinalizerInvariantFailed"), TEXT("BehaviorTree rollback finalization did not preserve public mirror invariants."));
		}
		return MakeValidationSuccess();
	}

private:
	TStrongObjectPtr<UBehaviorTree> Tree;
	TStrongObjectPtr<UBehaviorTreeGraph> Graph;
	TStrongObjectPtr<UPackage> Package;
	FString TreePath;
	FString GraphPath;
	FString PackagePath;
	FGuid GraphGuid;
	const bool bInitializeFromAsset;
	const bool bAllowEmptyAuthoredGraph;
};

bool IsExactRuntimeMirrorFootprint(const FAssetDocumentMutationFootprint& Footprint)
{
	return Footprint.Kind == EAssetDocumentMutationFootprintKind::UObjectState
		&& Footprint.RollbackComponentId == RuntimeMirrorComponentId
		&& !Footprint.StableId.IsEmpty();
}
} // namespace

namespace UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private
{
FAssetDocumentContractValidationResult ValidateMirror(UBehaviorTree& Tree, UBehaviorTreeGraph& Graph, bool AllowEmptyAuthoredGraph)
{
    using namespace AIBehaviorTreeSemanticRuntimeMirrorRollbackLocal;
    FAssetDocumentContractValidationResult Failure;
    return ValidateAuthoredRuntimeProjection(Tree, Graph, false, Failure, AllowEmptyAuthoredGraph) ? MakeValidationSuccess() : Failure;
}
TFunction<FAssetDocumentContractValidationResult()> CaptureMirrorRollback(UBehaviorTree& Tree, UBehaviorTreeGraph& Graph,
    bool InitializeFromAsset, bool AllowEmptyAuthoredGraph)
{
    using namespace AIBehaviorTreeSemanticRuntimeMirrorRollbackLocal;
    const auto State = MakeShared<FBehaviorTreeSemanticRuntimeMirrorState>(Tree, Graph, InitializeFromAsset, AllowEmptyAuthoredGraph);
    return [State]() { return State->Recomplete(); };
}
FAssetDocumentContractValidationResult CompleteMirror(UBehaviorTree& Tree, UBehaviorTreeGraph& Graph,
    FAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorResult& Out, bool InitializeFromAsset, bool AllowEmptyAuthoredGraph)
{
    using namespace AIBehaviorTreeSemanticRuntimeMirrorRollbackLocal;
    FAssetDocumentContractValidationResult Failure;
    if (!ValidateAuthoredRuntimeProjection(Tree, Graph, false, Failure, AllowEmptyAuthoredGraph)) return Failure;
    RebuildStockMirror(Graph, InitializeFromAsset);
    if (!ReadSemanticEvidence(Tree, Graph, Out, Failure, AllowEmptyAuthoredGraph)) return Failure;
    Out.bSucceeded = true;
    return MakeValidationSuccess();
}
FString HashEvidence(const FString& Value) { using namespace AIBehaviorTreeSemanticRuntimeMirrorRollbackLocal; return HashCanonicalUtf8(Value); }
}

namespace UE::AssetDocumentAI::BehaviorTreeSemanticRuntimeMirrorRollback::Private
{
void SetForceSemanticReadbackFailureForTest(bool bForceFailure)
{
	using namespace AIBehaviorTreeSemanticRuntimeMirrorRollbackLocal;
	GForceSemanticReadbackFailureForTest = bForceFailure;
}

#if WITH_DEV_AUTOMATION_TESTS
void SetBlackboardEvidenceExtensionForTest(TFunction<void(FString& Lines, const FString& RolePath, const UBTNode& Node)> Extension)
{
	AIBehaviorTreeSemanticRuntimeMirrorRollbackLocal::GBlackboardEvidenceExtensionForTest = MoveTemp(Extension);
}
#endif
} // namespace UE::AssetDocumentAI::BehaviorTreeSemanticRuntimeMirrorRollback::Private

FAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorResult
RebuildAssetDocumentBehaviorTreeRuntimeMirror(
	const IAssetDocumentVerifiedHandleLease& MutableAssetLease,
	UBehaviorTreeGraph& ExpectedLiveGraph,
	int32 UpdateFlags,
	const FAssetDocumentMutationFootprint& RuntimeMirrorFootprint,
	IAssetDocumentAtomicTransaction& ActiveTransaction)
{
	using namespace AIBehaviorTreeSemanticRuntimeMirrorRollbackLocal;
	if (!IsEditorGameThread())
	{
		return MakeFailure(TEXT("BehaviorTreeSemanticRollback.InvalidExecutionContext"), TEXT("Runtime mirror rebuild requires the Editor game thread."));
	}
	const EAssetDocumentAtomicState State = ActiveTransaction.GetState();
	if ((State != EAssetDocumentAtomicState::Live && State != EAssetDocumentAtomicState::Mutating)
		|| !ActiveTransaction.GetTransactionId().IsValid())
	{
		return MakeFailure(TEXT("BehaviorTreeSemanticRollback.TransactionStateInvalid"), TEXT("Runtime mirror rebuild requires a live active transaction."));
	}
	const FAssetDocumentMutationToken& Token = ActiveTransaction.GetMutationToken();
	if (!Token.RequestId.IsValid() || !Token.TransactionId.IsValid()
		|| Token.TransactionId != ActiveTransaction.GetTransactionId()
		|| Token.RuntimeRevision == 0)
	{
		return MakeFailure(TEXT("BehaviorTreeSemanticRollback.TransactionStateInvalid"), TEXT("The active transaction has no valid mutation token."));
	}
	const FAssetDocumentOpaqueHandle& Handle = MutableAssetLease.GetHandle();
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Token.RequestId;
	Context.TransactionId = Token.TransactionId;
	Context.RuntimeRevision = Token.RuntimeRevision;
	Context.ProviderId = Handle.ProviderId;
	Context.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	Context.RequiredKind = EAssetDocumentHandleKind::Asset;
	Context.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	if (Handle.TransactionId != ActiveTransaction.GetTransactionId()
		|| !ValidateAssetDocumentHandleMetadata(Handle, Context).bValid)
	{
		return MakeFailure(TEXT("BehaviorTreeSemanticRollback.TransactionBindingMismatch"), TEXT("The mutable Asset lease does not match the active mutation token."));
	}

	FAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorResult Result;
	FAssetDocumentContractValidationResult IdentityResult = WithAssetDocumentEditorAssetIdentity(
		MutableAssetLease,
		[&](const FAssetDocumentEditorAssetIdentityView& Identity)
		{
			UBehaviorTree* Tree = Cast<UBehaviorTree>(&Identity.GetObject());
			if (!IsLiveBehaviorTree(Tree)
				|| Tree->GetPathName() != FString(Identity.GetCanonicalObjectPath())
				|| static_cast<uint64>(Tree->GetUniqueID()) != Identity.GetTypeRevision())
			{
				return MakeValidationFailure(TEXT("BehaviorTreeSemanticRollback.BehaviorTreeRequired"), TEXT("The admitted Asset must remain a live BehaviorTree."));
			}
			if (!IsExactGraphIdentity(*Tree, ExpectedLiveGraph))
			{
				return MakeValidationFailure(TEXT("BehaviorTreeSemanticRollback.GraphIdentityMismatch"), TEXT("The supplied graph is not the exact live BehaviorTree graph."));
			}
			if (ExpectedLiveGraph.IsLocked())
			{
				return MakeValidationFailure(TEXT("BehaviorTreeSemanticRollback.GraphLocked"), TEXT("The live BehaviorTree graph is locked."));
			}
			if (UpdateFlags != UBehaviorTreeGraph::KeepRebuildCounter)
			{
				return MakeValidationFailure(TEXT("BehaviorTreeSemanticRollback.UpdateFlagsRejected"), TEXT("Only KeepRebuildCounter is admitted for runtime mirror rebuild."));
			}
			if (!IsExactRuntimeMirrorFootprint(RuntimeMirrorFootprint))
			{
				return MakeValidationFailure(TEXT("BehaviorTreeSemanticRollback.FootprintRejected"), TEXT("The runtime-mirror footprint is not the declared exact UObject state footprint."));
			}
			const auto Completion = RunAssetDocumentBehaviorTreeDerivedCompletion(*Tree, ExpectedLiveGraph,
                EAssetDocumentAIBehaviorTreeCompletionMode::MirrorOnly, RuntimeMirrorFootprint, ActiveTransaction);
            Result = Completion.Mirror;
            if (!Completion.bSucceeded)
            {
                FAssetDocumentContractValidationResult Failure;
                Failure.Diagnostics = Completion.Diagnostics;
                return Failure;
            }
			return MakeValidationSuccess();
		});
	if (!IdentityResult.bValid)
	{
		return MakeFailure(IdentityResult);
	}
	Result.bSucceeded = true;
	return Result;
}
