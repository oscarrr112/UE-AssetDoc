// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreePrimitives.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeMutationInternal.h"

#include "AssetDocumentEditorNativeState.h"
#include "AssetDocumentRollbackAction.h"
#include "Provider/AssetDocumentAIProviderContracts.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredGraphValidation.h"

#include "AIGraphNode.h"
#include "AIGraphTypes.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTreeDecoratorGraph.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "BehaviorTreeDecoratorGraphNode_Logic.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "EdGraphNode_Comment.h"
#include "Editor.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#include <initializer_list>

namespace AssetDocumentAIBehaviorTreeMutation
{
constexpr TCHAR BlackboardRollbackComponent[] =
	TEXT("assetdocument.ai.behavior_tree.blackboard_asset.v1");

FAssetDocumentPrimitiveOutcome Failure(
	const TCHAR* Code,
	const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	return MakeAssetDocumentAIPrimitiveFailure(
		EAssetDocumentPrimitiveOutcomeStatus::Failed, Code, Message, Diagnostics);
}

FAssetDocumentContractValidationResult Valid()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

FAssetDocumentRuntimeValueDto StringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto IntegerValue(const int64 Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Integer;
	Result.IntegerValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto NullValue()
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Null;
	return Result;
}

FAssetDocumentRuntimeValueDto ObjectValue(
	TArray<FString> Keys,
	TArray<FAssetDocumentRuntimeValueDto> Values)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Values);
	return Result;
}

const FAssetDocumentRuntimeValueDto* FindField(
	const FAssetDocumentRuntimeValueDto& Object,
	const TCHAR* Name)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Object.ObjectKeys.Num() != Object.Elements.Num()) return nullptr;
	for (int32 Index = 0; Index < Object.ObjectKeys.Num(); ++Index)
	{
		if (Object.ObjectKeys[Index] == Name) return &Object.Elements[Index];
	}
	return nullptr;
}

bool HasExactFields(
	const FAssetDocumentRuntimeValueDto& Object,
	std::initializer_list<const TCHAR*> Names)
{
	if (Object.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Object.ObjectKeys.Num() != Object.Elements.Num()
		|| Object.ObjectKeys.Num() != static_cast<int32>(Names.size())) return false;
	TSet<FString> Seen;
	for (const FString& Key : Object.ObjectKeys)
	{
		if (Key.IsEmpty() || Seen.Contains(Key)) return false;
		Seen.Add(Key);
	}
	for (const TCHAR* Name : Names)
	{
		if (FindField(Object, Name) == nullptr) return false;
	}
	return true;
}

bool IsCanonicalObjectPath(const FAssetDocumentRuntimeValueDto* Value)
{
	if (Value == nullptr || Value->Kind != EAssetDocumentRuntimeValueKind::String
		|| Value->StringValue.IsEmpty()) return false;
	const FSoftObjectPath Path(Value->StringValue);
	return Path.IsValid() && Path.GetSubPathString().IsEmpty()
		&& Path.ToString() == Value->StringValue;
}

} // namespace AssetDocumentAIBehaviorTreeMutation


namespace AIBehaviorTreeBlackboardMutationLocal
{
using namespace AssetDocumentAIBehaviorTreeMutation;

bool IsEditorPropertyContext(const UObject* Object)
{
	if (!IsInGameThread() || !GIsEditor || IsRunningCommandlet()
		|| IsGarbageCollecting() || Object == nullptr
		|| Object->HasAnyFlags(RF_ClassDefaultObject | RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return false;
	}
	const UPackage* Package = Object->GetOutermost();
	return Package != nullptr && !Package->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed);
}

UBlackboardData* ResolveBlackboard(const FString& Path)
{
	UBlackboardData* Blackboard = Cast<UBlackboardData>(FSoftObjectPath(Path).ResolveObject());
	return Blackboard != nullptr && Blackboard->GetPathName() == Path ? Blackboard : nullptr;
}

bool MakeRows(UObject& Owner, bool bShowHiddenProperties,
	TSharedPtr<IPropertyRowGenerator>& OutRows)
{
	if (!IsEditorPropertyContext(&Owner)) return false;
	FPropertyEditorModule* PropertyEditor =
		FModuleManager::LoadModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));
	if (PropertyEditor == nullptr) return false;
	FPropertyRowGeneratorArgs Args;
	Args.bAllowEditingClassDefaultObjects = false;
	Args.bShouldShowHiddenProperties = bShowHiddenProperties;
	OutRows = PropertyEditor->CreatePropertyRowGenerator(Args);
	if (!OutRows.IsValid()) return false;
	OutRows->SetPropertyGenerationAllowListPaths(
		{GET_MEMBER_NAME_CHECKED(UBehaviorTreeGraphNode_Root, BlackboardAsset).ToString()});
	OutRows->SetObjects({&Owner});
	return true;
}

TSharedPtr<IPropertyHandle> FindUniqueBlackboardHandle(
	IPropertyRowGenerator& Rows,
	UObject& Owner,
	const UClass* DeclaredOwner)
{
	TArray<TSharedPtr<IPropertyHandle>> Matches;
	TFunction<void(const TArray<TSharedRef<IDetailTreeNode>>&)> Gather =
		[&](const TArray<TSharedRef<IDetailTreeNode>>& Nodes)
	{
		for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
		{
			const TSharedPtr<IPropertyHandle> Candidate = Node->CreatePropertyHandle();
			TArray<UObject*> Outers;
			if (Candidate.IsValid()) Candidate->GetOuterObjects(Outers);
			FProperty* Property = Candidate.IsValid() ? Candidate->GetProperty() : nullptr;
			const FObjectPropertyBase* ObjectProperty = Property != nullptr
				? CastField<FObjectPropertyBase>(Property) : nullptr;
			if (ObjectProperty != nullptr
				&& Property->GetFName() == GET_MEMBER_NAME_CHECKED(UBehaviorTreeGraphNode_Root, BlackboardAsset)
				&& Property->GetOwnerClass() == DeclaredOwner
				&& Candidate->GeneratePathToProperty() == TEXT("BlackboardAsset")
				&& ObjectProperty->PropertyClass == UBlackboardData::StaticClass()
				&& Candidate->IsValidHandle() && Candidate->IsEditable()
				&& Outers.Num() == 1 && Outers[0] == &Owner)
			{
				Matches.Add(Candidate);
			}
			TArray<TSharedRef<IDetailTreeNode>> Children;
			Node->GetChildren(Children, true);
			Gather(Children);
		}
	};
	Gather(Rows.GetRootTreeNodes());
	return Matches.Num() == 1 ? Matches[0] : nullptr;
}

UBehaviorTreeGraphNode_Root* FindUniqueRoot(UBehaviorTreeGraph& Graph)
{
	UBehaviorTreeGraphNode_Root* Result = nullptr;
	for (UEdGraphNode* Node : Graph.Nodes)
	{
		UBehaviorTreeGraphNode_Root* Root = Cast<UBehaviorTreeGraphNode_Root>(Node);
		if (Root != nullptr)
		{
			if (Root->GetClass() != UBehaviorTreeGraphNode_Root::StaticClass()
				|| Root->GetOuter() != &Graph || Result != nullptr) return nullptr;
			Result = Root;
		}
	}
	return Result;
}

bool HasExactInitializedClosure(
	const UAIGraphNode& Node,
	const UBehaviorTree& Tree,
	const UBlackboardData* Blackboard,
	bool& bCompositeIdentityChecked)
{
	if (Node.IsA<UBehaviorTreeGraphNode_Root>())
	{
		return Node.NodeInstance == nullptr && Node.SubNodes.IsEmpty()
			&& Node.ErrorMessage.IsEmpty();
	}
	if (const auto* Wrapper=Cast<UBehaviorTreeGraphNode_CompositeDecorator>(&Node))
	{
		const auto* BoundGraph=Cast<UBehaviorTreeDecoratorGraph>(Wrapper->BoundGraph);
		if (Wrapper->GetClass()!=UBehaviorTreeGraphNode_CompositeDecorator::StaticClass()
			|| Wrapper->bInjectedNode || Wrapper->NodeInstance || !Wrapper->SubNodes.IsEmpty()
			|| !Wrapper->Decorators.IsEmpty() || !Wrapper->Services.IsEmpty() || !Wrapper->Pins.IsEmpty()
			|| !Wrapper->ErrorMessage.IsEmpty() || !Wrapper->ErrorMsg.IsEmpty()
			|| !Wrapper->ClassData.GetClassName().IsEmpty() || !Wrapper->ClassData.GetPackageName().IsEmpty()
			|| Wrapper->ClassData.IsDeprecated() || !IsEditorPropertyContext(BoundGraph)
			|| BoundGraph->GetClass()!=UBehaviorTreeDecoratorGraph::StaticClass() || BoundGraph->GetOuter()!=Wrapper)
		{
			return false;
		}
		if (!bCompositeIdentityChecked)
		{
			using namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback;
			FString Identity, Diagnostic;
			if (!CaptureGraphIdentity(Tree,Identity,Diagnostic,EAuthoredGraphReadbackVersion::V2)) return false;
			bCompositeIdentityChecked=true;
		}
		for (const UEdGraphNode* BoundNode : BoundGraph->Nodes)
		{
			if (!IsEditorPropertyContext(BoundNode) || BoundNode->GetOuter()!=BoundGraph
				|| BoundNode->GetGraph()!=BoundGraph || !BoundNode->ErrorMsg.IsEmpty()) return false;
			if (const auto* Test=Cast<UBehaviorTreeDecoratorGraphNode_Decorator>(BoundNode))
			{
				const auto* Instance=Cast<UBTDecorator>(Test->NodeInstance);
				if (Test->GetClass()!=UBehaviorTreeDecoratorGraphNode_Decorator::StaticClass()
					|| !IsEditorPropertyContext(Instance) || !Instance->IsIn(&Tree)
					|| Instance->GetTreeAsset()!=&Tree || Instance->GetBlackboardAsset()!=Blackboard
					|| Test->ClassData.GetClassName()!=Instance->GetClass()->GetName()
					|| (!Test->ClassData.GetPackageName().IsEmpty()
						&& Test->ClassData.GetPackageName()!=Instance->GetClass()->GetOutermost()->GetName())
					|| Test->ClassData.IsDeprecated()
					|| !FGraphNodeClassHelper::GetDeprecationMessage(Instance->GetClass()).IsEmpty()
					|| !Instance->GetErrorMessage().IsEmpty()) return false;
			}
			else if (BoundNode->GetClass()!=UBehaviorTreeDecoratorGraphNode_Logic::StaticClass()
				&& BoundNode->GetClass()!=UEdGraphNode_Comment::StaticClass()) return false;
		}
		return true;
	}
	if (Node.NodeInstance == nullptr)
	{
		return false;
	}
	const UBTNode* Instance = Cast<UBTNode>(Node.NodeInstance);
	if (Instance == nullptr || Instance->GetTreeAsset() != &Tree
		|| Instance->GetBlackboardAsset() != Blackboard)
	{
		return false;
	}
	FString ExpectedError =
		FGraphNodeClassHelper::GetDeprecationMessage(Instance->GetClass());
	if (ExpectedError.IsEmpty())
	{
		ExpectedError = Instance->GetErrorMessage();
	}
	if (Node.ErrorMessage != ExpectedError)
	{
		return false;
	}
	for (const UAIGraphNode* SubNode : Node.SubNodes)
	{
		if (SubNode == nullptr
			|| !HasExactInitializedClosure(*SubNode, Tree, Blackboard, bCompositeIdentityChecked))
		{
			return false;
		}
	}
	return true;
}

bool HasExactNativeBlackboardClosure(
	const UBehaviorTree& Tree,
	const UBehaviorTreeGraph& Graph,
	const UBehaviorTreeGraphNode_Root& Root,
	const UBlackboardData* Blackboard)
{
	if (Root.BlackboardAsset != Blackboard || Tree.BlackboardAsset != Blackboard
		|| Graph.GetOuter() != &Tree || Root.GetGraph() != &Graph)
	{
		return false;
	}
	bool bCompositeIdentityChecked=false;
	for (const UEdGraphNode* Node : Graph.Nodes)
	{
		const UAIGraphNode* AINode = Cast<UAIGraphNode>(Node);
		if (AINode != nullptr
			&& !HasExactInitializedClosure(*AINode, Tree, Blackboard, bCompositeIdentityChecked))
		{
			return false;
		}
	}
	return Tree.GetOutermost() != nullptr
		&& !Tree.GetOutermost()->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed);
}

FAssetDocumentContractValidationResult RejectBlackboardCapture(const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Code=TEXT("AIProvider.BehaviorTree.BlackboardCaptureRejected");
	Diagnostic.Path=TEXT("/BehaviorTreeBlackboardRollback"); Diagnostic.Message=Message;
	Result.Diagnostics.Add(MoveTemp(Diagnostic)); return Result;
}

// Only the two existing Editor capture calls receive this facade. Their
// identity and state checks read the real transaction; their actions stay
// local until the single Blackboard component is ready to be registered.
class FBlackboardPreimageCaptureTransaction final : public IAssetDocumentAtomicTransaction
{
public:
	explicit FBlackboardPreimageCaptureTransaction(IAssetDocumentAtomicTransaction& InParent) : Parent(InParent) {}
	FGuid GetTransactionId() const override { return Parent.GetTransactionId(); }
	EAssetDocumentAtomicState GetState() const override { return Parent.GetState(); }
	const FAssetDocumentMutationToken& GetMutationToken() const override { return Parent.GetMutationToken(); }
	FAssetDocumentContractValidationResult EnlistRollbackAction(TSharedRef<IAssetDocumentRollbackAction> Action) override
	{
		if ((GetState()!=EAssetDocumentAtomicState::Live && GetState()!=EAssetDocumentAtomicState::Mutating)
			|| !GetTransactionId().IsValid() || Actions.Num()>=2
			|| Action->GetRollbackComponentId()!=BlackboardRollbackComponent)
		{
			return RejectBlackboardCapture(TEXT("Capture admits exactly two typed actions for the existing Blackboard component."));
		}
		Actions.Add(MoveTemp(Action)); return Valid();
	}
	FAssetDocumentContractValidationResult EnlistFootprint(TConstArrayView<FAssetDocumentMutationFootprint>) override { return RejectMutation(); }
	FAssetDocumentContractValidationResult MarkReadyToCommit() override { return RejectMutation(); }
	FAssetDocumentContractValidationResult Commit(const FAssetDocumentDurableCompletionReceipt&) override { return RejectMutation(); }
	FAssetDocumentContractValidationResult Rollback(FStringView) override { return RejectMutation(); }
	FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(TSharedRef<IAssetDocumentPostRollbackFinalizer>) override { return RejectMutation(); }
	int32 NumActions() const { return Actions.Num(); }
	TArray<TSharedRef<IAssetDocumentRollbackAction>> TakeActions() { return MoveTemp(Actions); }
private:
	FAssetDocumentContractValidationResult RejectMutation() const
	{
		return RejectBlackboardCapture(TEXT("The Blackboard preimage facade cannot mutate the real transaction."));
	}
	IAssetDocumentAtomicTransaction& Parent;
	TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions;
};

class FBlackboardErrorRollback final : public IAssetDocumentRollbackAction
{
public:
	FBlackboardErrorRollback(UBehaviorTree& InTree, UBehaviorTreeGraph& InGraph, UBehaviorTreeGraphNode_Root& InRoot)
		: Tree(&InTree), Graph(&InGraph), Root(&InRoot), Blackboard(InTree.BlackboardAsset.Get()), Package(InTree.GetOutermost())
		, TreePath(InTree.GetPathName()), GraphPath(InGraph.GetPathName()), RootPath(InRoot.GetPathName())
		, BlackboardPath(InTree.BlackboardAsset ? InTree.BlackboardAsset->GetPathName() : FString())
		, GraphGuid(InGraph.GraphGuid), RootGuid(InRoot.NodeGuid), bDirty(InTree.GetOutermost()->IsDirty()) {}
	FStringView GetRollbackComponentId() const override { return BlackboardRollbackComponent; }
	FAssetDocumentContractValidationResult Rollback(FStringView) override
	{
		if (bConsumed) return CachedResult;
		bConsumed=true;
		CachedResult=RestoreOnce();
		if (CachedResult.bValid)
		{
			// A verified inverse must release a newly-created target before
			// earlier graph-install and factory actions undo their ownership.
			Blackboard.Reset(); Root.Reset(); Graph.Reset(); Tree.Reset(); Package.Reset();
		}
		return CachedResult;
	}
private:
	FAssetDocumentContractValidationResult RestoreOnce()
	{
		if (!IsEditorPropertyContext(Tree.Get()) || !IsEditorPropertyContext(Graph.Get()) || !IsEditorPropertyContext(Root.Get())
			|| !Package.IsValid() || Tree->BTGraph.Get()!=Graph.Get() || Graph->GetOuter()!=Tree.Get()
			|| Root->GetGraph()!=Graph.Get() || FindUniqueRoot(*Graph)!=Root.Get()
			|| Tree->GetPathName()!=TreePath || Graph->GetPathName()!=GraphPath || Root->GetPathName()!=RootPath
			|| Graph->GraphGuid!=GraphGuid || Root->NodeGuid!=RootGuid || Tree->GetOutermost()!=Package.Get()
			|| (!BlackboardPath.IsEmpty() && (!IsEditorPropertyContext(Blackboard.Get()) || Blackboard->GetPathName()!=BlackboardPath))
			|| Tree->BlackboardAsset.Get()!=Blackboard.Get() || Root->BlackboardAsset.Get()!=Blackboard.Get())
		{
			return Reject(TEXT("The captured graph and restored Blackboard references are not the exact rollback identity."));
		}
		Graph->UpdateErrorMessages();
		Package->SetDirtyFlag(bDirty);
		return HasExactNativeBlackboardClosure(*Tree,*Graph,*Root,Blackboard.Get()) && Package->IsDirty()==bDirty
			? Valid() : Reject(TEXT("The restored Blackboard error cache does not match its initialized graph closure."));
	}
	FAssetDocumentContractValidationResult Reject(const TCHAR* Message) const
	{
		FAssetDocumentContractValidationResult Result;
		FAssetDocumentRuntimeDiagnostic Diagnostic;
		Diagnostic.Code=TEXT("AIProvider.BehaviorTree.BlackboardRollbackFailed");
		Diagnostic.Path=TEXT("/BehaviorTreeBlackboardRollback"); Diagnostic.Message=Message;
		Result.Diagnostics.Add(MoveTemp(Diagnostic)); return Result;
	}
	TStrongObjectPtr<UBehaviorTree> Tree;
	TStrongObjectPtr<UBehaviorTreeGraph> Graph;
	TStrongObjectPtr<UBehaviorTreeGraphNode_Root> Root;
	TStrongObjectPtr<UBlackboardData> Blackboard;
	TStrongObjectPtr<UPackage> Package;
	FString TreePath, GraphPath, RootPath, BlackboardPath;
	FGuid GraphGuid, RootGuid;
	bool bDirty;
	bool bConsumed=false;
	FAssetDocumentContractValidationResult CachedResult;
};

class FBlackboardRollback final : public IAssetDocumentRollbackAction
{
public:
	FBlackboardRollback(UBehaviorTree& Tree, UBehaviorTreeGraph& Graph, UBehaviorTreeGraphNode_Root& Root,
		TArray<TSharedRef<IAssetDocumentRollbackAction>> InTypedActions)
		: TypedActions(MoveTemp(InTypedActions)), ErrorRollback(MakeShared<FBlackboardErrorRollback>(Tree,Graph,Root)) {}
	FStringView GetRollbackComponentId() const override { return BlackboardRollbackComponent; }
	FAssetDocumentContractValidationResult Rollback(FStringView FailureCode) override
	{
		if (bConsumed) return CachedResult;
		bConsumed=true;
		if (TypedActions.Num()!=2 || !ErrorRollback.IsValid())
		{
			CachedResult=RejectBlackboardCapture(TEXT("The Blackboard inverse requires its exact two typed preimages and error-cache action."));
			return CachedResult;
		}
		// Capture order is Root, Tree. A failed Tree restore must not run the
		// Root setter against the wrong header and trigger project lifecycle.
		for (int32 Index=TypedActions.Num()-1; Index>=0; --Index)
		{
			CachedResult=TypedActions[Index]->Rollback(FailureCode);
			if (!CachedResult.bValid) return CachedResult;
		}
		CachedResult=ErrorRollback->Rollback(FailureCode);
		if (CachedResult.bValid) { TypedActions.Reset(); ErrorRollback.Reset(); }
		return CachedResult;
	}
	void AbandonUnwritten()
	{
		// Registration alone does not make an action a rollback target. If
		// footprint enlistment fails before writes, release its preimages now.
		TypedActions.Reset(); ErrorRollback.Reset(); bConsumed=true; CachedResult=Valid();
	}
private:
	TArray<TSharedRef<IAssetDocumentRollbackAction>> TypedActions;
	TSharedPtr<FBlackboardErrorRollback> ErrorRollback;
	bool bConsumed=false;
	FAssetDocumentContractValidationResult CachedResult;
};

FAssetDocumentMutationFootprint MakeFootprint(const FString& StableId)
{
	FAssetDocumentMutationFootprint Result;
	Result.Kind = EAssetDocumentMutationFootprintKind::UObjectState;
	Result.StableId = StableId;
	Result.RollbackComponentId = BlackboardRollbackComponent;
	return Result;
}

FAssetDocumentChangedRecord Changed(const FString& TreePath, const FString& StablePath)
{
	FAssetDocumentChangedRecord Result;
	Result.AuthorityId = BlackboardRollbackComponent;
	Result.StableObjectId = TreePath;
	Result.Path = StablePath;
	Result.ChangeKind = TEXT("Update");
	return Result;
}

struct FBlackboardSetRequest
{
	FString TreePath;
	UBlackboardData* Blackboard = nullptr;
};

struct FBlackboardRootMutation
{
	UBehaviorTreeGraphNode_Root* Root = nullptr;
	TSharedPtr<IPropertyRowGenerator> Rows;
	TSharedPtr<IPropertyHandle> Handle;
	TSharedPtr<IPropertyRowGenerator> TreeRows;
	TSharedPtr<IPropertyHandle> TreeHandle;
	FString StablePath;
};

bool ParseBlackboardSetRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FBlackboardSetRequest& OutRequest,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	const FAssetDocumentRuntimeValueDto& Value = Invocation.ValueArguments;
	if (!IsAssetDocumentAICanonicalRuntimeValue(Value)
		|| !HasExactFields(Value, {TEXT("behavior_tree_path"), TEXT("blackboard_path_or_null")}))
	{
		OutFailure = Failure(TEXT("AIProvider.InvalidValueShape"), TEXT("The BehaviorTree Blackboard request is not canonical."));
		return false;
	}
	const FAssetDocumentRuntimeValueDto* TreePath = FindField(Value, TEXT("behavior_tree_path"));
	const FAssetDocumentRuntimeValueDto* BlackboardPath = FindField(Value, TEXT("blackboard_path_or_null"));
	if (!IsCanonicalObjectPath(TreePath) || BlackboardPath == nullptr
		|| (BlackboardPath->Kind != EAssetDocumentRuntimeValueKind::Null && !IsCanonicalObjectPath(BlackboardPath)))
	{
		OutFailure = Failure(TEXT("AIProvider.InvalidValueShape"), TEXT("The BehaviorTree and Blackboard paths are invalid."));
		return false;
	}
	OutRequest.TreePath = TreePath->StringValue;
	OutRequest.Blackboard = BlackboardPath->Kind == EAssetDocumentRuntimeValueKind::Null ? nullptr : ResolveBlackboard(BlackboardPath->StringValue);
	if (BlackboardPath->Kind != EAssetDocumentRuntimeValueKind::Null && OutRequest.Blackboard == nullptr)
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.BlackboardClassMismatch"), TEXT("The requested Blackboard asset is not an exact loaded BlackboardData."));
		return false;
	}
	return true;
}

bool IsExactBlackboardTreeIdentity(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const FBlackboardSetRequest& Request,
	UBehaviorTree*& OutTree,
	UBehaviorTreeGraph*& OutGraph)
{
	OutTree = Cast<UBehaviorTree>(&Identity.GetObject());
	OutGraph = OutTree != nullptr ? Cast<UBehaviorTreeGraph>(OutTree->BTGraph) : nullptr;
	return OutTree != nullptr && Identity.GetCanonicalObjectPath() == Request.TreePath
		&& OutGraph != nullptr && OutGraph->GetClass() == UBehaviorTreeGraph::StaticClass()
		&& OutGraph->GetOuter() == OutTree && IsEditorPropertyContext(OutGraph);
}

bool PrepareBlackboardRootMutation(
	const FBlackboardSetRequest& Request,
	UBehaviorTree& Tree,
	UBehaviorTreeGraph& Graph,
	FBlackboardRootMutation& OutMutation,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	OutMutation.Root = FindUniqueRoot(Graph);
	if (OutMutation.Root == nullptr)
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.BlackboardAssignmentRejected"), TEXT("The live BehaviorTree graph does not contain exactly one Root node."));
		return false;
	}
	if (!HasExactNativeBlackboardClosure(Tree, Graph, *OutMutation.Root, Tree.BlackboardAsset))
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.BlackboardAssignmentRejected"), TEXT("The live BehaviorTree graph is not fully initialized from its current Blackboard source."));
		return false;
	}
	if (!MakeRows(*OutMutation.Root, false, OutMutation.Rows)
		|| !MakeRows(Tree, true, OutMutation.TreeRows))
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.BlackboardAssignmentRejected"), TEXT("The Tree and Root Blackboard properties are unavailable through PropertyEditor."));
		return false;
	}
	OutMutation.Handle = FindUniqueBlackboardHandle(*OutMutation.Rows, *OutMutation.Root, UBehaviorTreeGraphNode_Root::StaticClass());
	OutMutation.TreeHandle = FindUniqueBlackboardHandle(*OutMutation.TreeRows, Tree, UBehaviorTree::StaticClass());
	if (!OutMutation.Handle.IsValid() || !OutMutation.TreeHandle.IsValid())
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.BlackboardAssignmentRejected"), TEXT("The Tree and Root Blackboard properties are not uniquely admitted."));
		return false;
	}
	OutMutation.StablePath = FString::Printf(TEXT("asset:%s/BlackboardAsset"), *Request.TreePath);
	return true;
}

bool CaptureAndAssignBlackboard(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const FBlackboardSetRequest& Request,
	UBehaviorTree& Tree,
	UBehaviorTreeGraph& Graph,
	IAssetDocumentAtomicTransaction& ActiveTransaction,
	FBlackboardRootMutation& Mutation,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	FBlackboardPreimageCaptureTransaction CaptureTransaction(ActiveTransaction);
	// Ordinary actions unwind in reverse order. Capture Root before Tree so
	// both the forward write and inverse restore publish Tree before Root.
	const FAssetDocumentEditorTypedPreimageResult Capture = CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage(
		Identity, *Mutation.Root, Mutation.Rows.ToSharedRef(), Mutation.Handle.ToSharedRef(),
		{BlackboardRollbackComponent}, CaptureTransaction);
	if (!Capture.bSucceeded)
	{
		OutFailure = Failure(TEXT("AIProvider.OperationRejected"), TEXT("The admitted Blackboard typed-preimage action was rejected."), Capture.Diagnostics);
		return false;
	}
	const FAssetDocumentEditorTypedPreimageResult TreeCapture = CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage(
		Identity, Tree, Mutation.TreeRows.ToSharedRef(), Mutation.TreeHandle.ToSharedRef(),
		{BlackboardRollbackComponent}, CaptureTransaction);
	if (!TreeCapture.bSucceeded)
	{
		OutFailure = Failure(TEXT("AIProvider.OperationRejected"), TEXT("The admitted Tree Blackboard typed-preimage action was rejected."), TreeCapture.Diagnostics);
		return false;
	}
	if (CaptureTransaction.NumActions()!=2)
	{
		OutFailure=Failure(TEXT("AIProvider.OperationRejected"),TEXT("The Blackboard capture did not produce exactly two typed preimages."));
		return false;
	}
	const auto Rollback=MakeShared<FBlackboardRollback>(Tree,Graph,*Mutation.Root,CaptureTransaction.TakeActions());
	const auto Registration=ActiveTransaction.EnlistRollbackAction(Rollback);
	if (!Registration.bValid)
	{
		Rollback->AbandonUnwritten();
		OutFailure=Failure(TEXT("AIProvider.OperationRejected"),TEXT("The combined Blackboard inverse could not be enlisted."),Registration.Diagnostics);
		return false;
	}
	const FAssetDocumentContractValidationResult Enlisted = ActiveTransaction.EnlistFootprint({MakeFootprint(Mutation.StablePath)});
	if (!Enlisted.bValid)
	{
		Rollback->AbandonUnwritten();
		OutFailure = Failure(TEXT("AIProvider.OperationRejected"), TEXT("The Root Blackboard footprint could not be enlisted."), Enlisted.Diagnostics);
		return false;
	}
	// The stock Root notification only initializes nodes when its reference
	// differs from the Tree header. Updating both typed references in this
	// order retains the initialized closure without project lifecycle calls.
	if (Mutation.TreeHandle->SetValue(Request.Blackboard, EPropertyValueSetFlags::NotTransactable) != FPropertyAccess::Success
		|| Tree.BlackboardAsset != Request.Blackboard
		|| Mutation.Handle->SetValue(Request.Blackboard, EPropertyValueSetFlags::NotTransactable) != FPropertyAccess::Success)
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.BlackboardNotificationFailed"), TEXT("The Tree and Root typed Blackboard references did not read back exactly."));
		return false;
	}
	Graph.UpdateErrorMessages();
	if (!HasExactNativeBlackboardClosure(Tree, Graph, *Mutation.Root, Request.Blackboard))
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.BlackboardNotificationFailed"), TEXT("The Root typed Blackboard property did not complete its native mirror lifecycle."));
		return false;
	}
	return true;
}

FAssetDocumentPrimitiveOutcome AssignBlackboardWithEditorIdentity(
	const FBlackboardSetRequest& Request,
	const IAssetDocumentVerifiedHandleLease& Lease,
	IAssetDocumentAtomicTransaction& ActiveTransaction)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	const FAssetDocumentContractValidationResult IdentityResult = WithAssetDocumentEditorAssetIdentity(Lease,
		[&](const FAssetDocumentEditorAssetIdentityView& Identity)
	{
		UBehaviorTree* Tree = nullptr;
		UBehaviorTreeGraph* Graph = nullptr;
		if (!IsExactBlackboardTreeIdentity(Identity, Request, Tree, Graph))
		{
			Outcome = Failure(TEXT("AIProvider.InvalidHandle"), TEXT("The Asset identity does not match the requested live BehaviorTree."));
			return FAssetDocumentContractValidationResult();
		}
		FBlackboardRootMutation Mutation;
		if (!PrepareBlackboardRootMutation(Request, *Tree, *Graph, Mutation, Outcome)
			|| !CaptureAndAssignBlackboard(Identity, Request, *Tree, *Graph, ActiveTransaction, Mutation, Outcome))
		{
			return FAssetDocumentContractValidationResult();
		}
		Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
		Outcome.Value = ObjectValue({TEXT("blackboard_path_or_null")}, {Request.Blackboard == nullptr ? NullValue() : StringValue(Request.Blackboard->GetPathName())});
		Outcome.ChangedSet.Add(Changed(Request.TreePath, Mutation.StablePath));
		return Valid();
	});
	return IdentityResult.bValid || !Outcome.Diagnostics.IsEmpty() ? Outcome
		: Failure(TEXT("AIProvider.InvalidHandle"), TEXT("The mutable Asset lease could not enter Editor identity."), IdentityResult.Diagnostics);
}
} // namespace

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeBlackboardSet(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AIBehaviorTreeBlackboardMutationLocal;
	if (!IsAssetDocumentAITransactionValid(Invocation, ActiveTransaction))
		return Failure(TEXT("AIProvider.InvalidTransaction"), TEXT("BehaviorTree Blackboard assignment requires the active AI mutation transaction."));
	FBlackboardSetRequest Request;
	FAssetDocumentPrimitiveOutcome FailureOutcome;
	if (!ParseBlackboardSetRequest(Invocation, Request, FailureOutcome)) return FailureOutcome;
	FAssetDocumentVerifiedLeaseResult LeaseResult = AcquireAssetDocumentAIMutableAssetLease(Handles, Invocation);
	if (LeaseResult.Lease == nullptr)
		return Failure(TEXT("AIProvider.InvalidHandle"), TEXT("A matching AI mutable Asset lease is required."), LeaseResult.Diagnostics);
	return AssignBlackboardWithEditorIdentity(Request, *LeaseResult.Lease, *ActiveTransaction);
}
