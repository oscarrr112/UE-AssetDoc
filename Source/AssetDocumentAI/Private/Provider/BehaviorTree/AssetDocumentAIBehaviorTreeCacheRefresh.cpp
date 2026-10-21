// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreePrimitives.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeMutationInternal.h"

#include "AssetDocumentEditorNativeState.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollback.h"
#include "Provider/AssetDocumentAIProviderContracts.h"
#include "TypedPreimage/AssetDocumentAISelectorTypedPreimage.h"

#include "AIGraphNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTreeGraph.h"
#include "DetailWidgetRow.h"
#include "Editor.h"
#include "EdGraph/EdGraphNode.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "IPropertyTypeCustomization.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#include <initializer_list>

namespace AIBehaviorTreeCacheRefreshLocal
{
using namespace AssetDocumentAIBehaviorTreeMutation;
constexpr TCHAR SelectorRollbackComponent[] =
	TEXT("assetdocument.ai.behavior_tree.selector_cache.v1");
constexpr TCHAR RuntimeMirrorRollbackComponent[] =
	TEXT("assetdocument.ai.behavior_tree.runtime_mirror.v1");

class FAssetDocumentAIPassiveBlackboardSelectorCustomization final
	: public IPropertyTypeCustomization
{
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance()
	{
		return MakeShared<FAssetDocumentAIPassiveBlackboardSelectorCustomization>();
	}

	virtual void CustomizeHeader(
		TSharedRef<IPropertyHandle> PropertyHandle,
		FDetailWidgetRow& HeaderRow,
		IPropertyTypeCustomizationUtils&) override
	{
		HeaderRow
			.NameContent()[PropertyHandle->CreatePropertyNameWidget()]
			.ValueContent()[PropertyHandle->CreatePropertyValueWidget()];
	}

	virtual void CustomizeChildren(
		TSharedRef<IPropertyHandle>,
		IDetailChildrenBuilder&,
		IPropertyTypeCustomizationUtils&) override
	{
	}
};

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

bool MakeRows(
	UObject& Object,
	const FString& PropertyPath,
	TSharedPtr<IPropertyRowGenerator>& OutRows)
{
	if (!IsEditorPropertyContext(&Object)) return false;
	FPropertyEditorModule* PropertyEditor =
		FModuleManager::LoadModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));
	if (PropertyEditor == nullptr) return false;
	FPropertyRowGeneratorArgs Args;
	Args.bAllowEditingClassDefaultObjects = false;
	OutRows = PropertyEditor->CreatePropertyRowGenerator(Args);
	if (!OutRows.IsValid()) return false;
	OutRows->RegisterInstancedCustomPropertyTypeLayout(
		FBlackboardKeySelector::StaticStruct()->GetFName(),
		FOnGetPropertyTypeCustomizationInstance::CreateStatic(
			&FAssetDocumentAIPassiveBlackboardSelectorCustomization::MakeInstance));
	OutRows->SetPropertyGenerationAllowListPaths({PropertyPath});
	OutRows->SetObjects({&Object});
	return true;
}

TSharedPtr<IPropertyHandle> FindUniqueSelectorHandle(
	IPropertyRowGenerator& Rows,
	UObject& Owner,
	const FString& PropertyPath)
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
			const FStructProperty* Struct = Property != nullptr
				? CastField<FStructProperty>(Property) : nullptr;
			if (Struct != nullptr && Struct->Struct == FBlackboardKeySelector::StaticStruct()
				&& Candidate->GeneratePathToProperty() == PropertyPath
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

UEdGraphNode* FindExactNode(UBehaviorTreeGraph& Graph, const FString& StableId)
{
	FGuid NodeGuid;
	if (!FGuid::ParseExact(StableId, EGuidFormats::DigitsWithHyphensLower, NodeGuid))
	{
		return nullptr;
	}
	UEdGraphNode* Result = nullptr;
	for (UEdGraphNode* Node : Graph.Nodes)
	{
		if (Node != nullptr && Node->NodeGuid == NodeGuid)
		{
			if (Result != nullptr) return nullptr;
			Result = Node;
		}
	}
	return Result;
}

UBlackboardData* ResolveBlackboard(const FString& Path)
{
	UBlackboardData* Blackboard = Cast<UBlackboardData>(FSoftObjectPath(Path).ResolveObject());
	return Blackboard != nullptr && Blackboard->GetPathName() == Path ? Blackboard : nullptr;
}

FAssetDocumentMutationFootprint MakeFootprint(
	const FString& StableId,
	const TCHAR* ComponentId)
{
	FAssetDocumentMutationFootprint Result;
	Result.Kind = EAssetDocumentMutationFootprintKind::UObjectState;
	Result.StableId = StableId;
	Result.RollbackComponentId = ComponentId;
	return Result;
}

FAssetDocumentChangedRecord Changed(
	const TCHAR* AuthorityId,
	const FString& StableObjectId,
	const FString& Path)
{
	FAssetDocumentChangedRecord Result;
	Result.AuthorityId = AuthorityId;
	Result.StableObjectId = StableObjectId;
	Result.Path = Path;
	Result.ChangeKind = TEXT("Update");
	return Result;
}

bool InvalidateRebuiltBehaviorTree(
	IAssetDocumentHandleResolver& Handles,
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	FAssetDocumentHandleInvalidation Event;
	Event.Reason = EAssetDocumentHandleInvalidationReason::Rebuild;
	Event.RequestId = Invocation.RequestId;
	Event.TransactionId = Invocation.TransactionId;
	Event.RuntimeRevision = Invocation.RuntimeRevision;
	Event.ProviderId = TEXT("ai");
	return Handles.Invalidate(Event).InvalidatedHandleCount > 0;
}
struct FSelectorResolveRequest
{
	FString TreePath;
	FString BlackboardPath;
	FString PropertyPath;
	FString SelectedKeyName;
	FString StableNodeId;
};

struct FSelectorResolveMutation
{
	UObject* PropertyOwner = nullptr;
	TSharedPtr<IPropertyRowGenerator> Rows;
	TSharedPtr<IPropertyHandle> SelectorHandle;
	FBlackboardKeySelector* Selector = nullptr;
	FString StablePath;
};

bool HasSelectorResolveFieldValues(
	const FAssetDocumentRuntimeValueDto* TreePath,
	const FAssetDocumentRuntimeValueDto* BlackboardPath,
	const FAssetDocumentRuntimeValueDto* PropertyPath,
	const FAssetDocumentRuntimeValueDto* SelectedKeyName,
	const FAssetDocumentRuntimeValueDto* StableNodeId)
{
	return IsCanonicalObjectPath(TreePath) && IsCanonicalObjectPath(BlackboardPath)
		&& PropertyPath != nullptr && PropertyPath->Kind == EAssetDocumentRuntimeValueKind::String
		&& !PropertyPath->StringValue.IsEmpty() && SelectedKeyName != nullptr
		&& SelectedKeyName->Kind == EAssetDocumentRuntimeValueKind::String
		&& !SelectedKeyName->StringValue.IsEmpty() && StableNodeId != nullptr
		&& StableNodeId->Kind == EAssetDocumentRuntimeValueKind::String;
}

bool ParseSelectorResolveRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FSelectorResolveRequest& OutRequest,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	const FAssetDocumentRuntimeValueDto& Value = Invocation.ValueArguments;
	if (!IsAssetDocumentAICanonicalRuntimeValue(Value)
		|| !HasExactFields(Value, {TEXT("behavior_tree_path"), TEXT("blackboard_path"),
			TEXT("property_path"), TEXT("selected_key_name"), TEXT("stable_node_id")}))
	{
		OutFailure = Failure(TEXT("AIProvider.InvalidValueShape"), TEXT("The selector resolve request is not canonical."));
		return false;
	}
	const FAssetDocumentRuntimeValueDto* TreePath = FindField(Value, TEXT("behavior_tree_path"));
	const FAssetDocumentRuntimeValueDto* BlackboardPath = FindField(Value, TEXT("blackboard_path"));
	const FAssetDocumentRuntimeValueDto* PropertyPath = FindField(Value, TEXT("property_path"));
	const FAssetDocumentRuntimeValueDto* SelectedKeyName = FindField(Value, TEXT("selected_key_name"));
	const FAssetDocumentRuntimeValueDto* StableNodeId = FindField(Value, TEXT("stable_node_id"));
	if (!HasSelectorResolveFieldValues(TreePath, BlackboardPath, PropertyPath, SelectedKeyName, StableNodeId))
	{
		OutFailure = Failure(TEXT("AIProvider.InvalidValueShape"), TEXT("The selector resolve fields are invalid."));
		return false;
	}
	OutRequest = {TreePath->StringValue, BlackboardPath->StringValue, PropertyPath->StringValue,
		SelectedKeyName->StringValue, StableNodeId->StringValue};
	return true;
}

bool IsExactSelectorTreeIdentity(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const FSelectorResolveRequest& Request,
	UBlackboardData& Blackboard,
	UBehaviorTree*& OutTree,
	UBehaviorTreeGraph*& OutGraph)
{
	OutTree = Cast<UBehaviorTree>(&Identity.GetObject());
	OutGraph = OutTree != nullptr ? Cast<UBehaviorTreeGraph>(OutTree->BTGraph) : nullptr;
	return OutTree != nullptr && Identity.GetCanonicalObjectPath() == Request.TreePath
		&& OutTree->BlackboardAsset == &Blackboard && OutGraph != nullptr
		&& OutGraph->GetOuter() == OutTree && IsEditorPropertyContext(OutGraph);
}

bool PrepareSelectorResolveMutation(
	const FSelectorResolveRequest& Request,
	UBlackboardData& Blackboard,
	UBehaviorTreeGraph& Graph,
	FSelectorResolveMutation& OutMutation,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	UAIGraphNode* GraphNode = Cast<UAIGraphNode>(FindExactNode(Graph, Request.StableNodeId));
	OutMutation.PropertyOwner = GraphNode != nullptr ? GraphNode->NodeInstance.Get() : nullptr;
	if (OutMutation.PropertyOwner == nullptr || !IsEditorPropertyContext(OutMutation.PropertyOwner))
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.SelectorPropertyRejected"), TEXT("The requested selector owner is not an exact live node instance."));
		return false;
	}
	if (Blackboard.GetKeyID(FName(*Request.SelectedKeyName)) == FBlackboard::InvalidKey)
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.SelectorResolveFailed"), TEXT("The requested Blackboard key does not exist."));
		return false;
	}
	if (!MakeRows(*OutMutation.PropertyOwner, Request.PropertyPath, OutMutation.Rows))
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.SelectorPropertyRejected"), TEXT("The exact selector property is unavailable through PropertyEditor."));
		return false;
	}
	OutMutation.SelectorHandle = FindUniqueSelectorHandle(*OutMutation.Rows, *OutMutation.PropertyOwner, Request.PropertyPath);
	if (!OutMutation.SelectorHandle.IsValid())
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.SelectorPropertyRejected"), TEXT("The requested selector property is not uniquely admitted."));
		return false;
	}
	void* SelectorAddress = nullptr;
	if (OutMutation.SelectorHandle->GetValueData(SelectorAddress) != FPropertyAccess::Success || SelectorAddress == nullptr)
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.SelectorPropertyRejected"), TEXT("The admitted selector property has no exact value address."));
		return false;
	}
	OutMutation.Selector = static_cast<FBlackboardKeySelector*>(SelectorAddress);
	if (OutMutation.Selector->SelectedKeyName.ToString() != Request.SelectedKeyName)
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.BlackboardMismatch"), TEXT("The selector authored key does not match the requested key."));
		return false;
	}
	OutMutation.StablePath = FString::Printf(TEXT("asset:%s/Nodes[guid=%s].%s"), *Request.TreePath, *Request.StableNodeId, *Request.PropertyPath);
	return true;
}

bool CaptureAndResolveSelector(
	const FSelectorResolveRequest& Request,
	UBlackboardData& Blackboard,
	const IAssetDocumentVerifiedHandleLease& Lease,
	IAssetDocumentAtomicTransaction& ActiveTransaction,
	FSelectorResolveMutation& Mutation,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	const FAssetDocumentEditorTypedPreimageResult Capture = CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorPreimage(
		Lease, *Mutation.PropertyOwner, Mutation.Rows.ToSharedRef(), Mutation.SelectorHandle.ToSharedRef(),
		{SelectorRollbackComponent}, ActiveTransaction);
	if (!Capture.bSucceeded)
	{
		OutFailure = Failure(TEXT("AIProvider.OperationRejected"), TEXT("The admitted selector typed-preimage action was rejected."), Capture.Diagnostics);
		return false;
	}
	const FAssetDocumentContractValidationResult Enlisted = ActiveTransaction.EnlistFootprint({MakeFootprint(Mutation.StablePath, SelectorRollbackComponent)});
	if (!Enlisted.bValid)
	{
		OutFailure = Failure(TEXT("AIProvider.OperationRejected"), TEXT("The selector footprint could not be enlisted."), Enlisted.Diagnostics);
		return false;
	}
	Mutation.Selector->ResolveSelectedKey(Blackboard);
	if (Mutation.Selector->SelectedKeyName.ToString() != Request.SelectedKeyName
		|| Mutation.Selector->GetSelectedKeyID() == FBlackboard::InvalidKey || Mutation.Selector->SelectedKeyType == nullptr)
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.SelectorResolveFailed"), TEXT("The requested Blackboard key could not be resolved by the selector."));
		return false;
	}
	return true;
}

FAssetDocumentPrimitiveOutcome ResolveSelectorWithEditorIdentity(
	const FSelectorResolveRequest& Request,
	UBlackboardData& Blackboard,
	const IAssetDocumentVerifiedHandleLease& Lease,
	IAssetDocumentAtomicTransaction& ActiveTransaction)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	const FAssetDocumentContractValidationResult IdentityResult = WithAssetDocumentEditorAssetIdentity(Lease,
		[&](const FAssetDocumentEditorAssetIdentityView& Identity)
	{
		UBehaviorTree* Tree = nullptr;
		UBehaviorTreeGraph* Graph = nullptr;
		if (!IsExactSelectorTreeIdentity(Identity, Request, Blackboard, Tree, Graph))
		{
			Outcome = Failure(TEXT("AIProvider.InvalidHandle"), TEXT("The Asset identity does not match the requested live BehaviorTree."));
			return FAssetDocumentContractValidationResult();
		}
		FSelectorResolveMutation Mutation;
		if (!PrepareSelectorResolveMutation(Request, Blackboard, *Graph, Mutation, Outcome)
			|| !CaptureAndResolveSelector(Request, Blackboard, Lease, ActiveTransaction, Mutation, Outcome))
		{
			return FAssetDocumentContractValidationResult();
		}
		Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
		Outcome.Value = ObjectValue({TEXT("selected_key_id"), TEXT("selected_key_name"), TEXT("selected_key_type_path")},
			{IntegerValue(static_cast<int32>(Mutation.Selector->GetSelectedKeyID())), StringValue(Mutation.Selector->SelectedKeyName.ToString()), StringValue(Mutation.Selector->SelectedKeyType->GetPathName())});
		Outcome.ChangedSet.Add(Changed(SelectorRollbackComponent, Request.TreePath, Mutation.StablePath));
		return Valid();
	});
	return IdentityResult.bValid || !Outcome.Diagnostics.IsEmpty() ? Outcome
		: Failure(TEXT("AIProvider.InvalidHandle"), TEXT("The mutable Asset lease could not enter Editor identity."), IdentityResult.Diagnostics);
}

struct FRuntimeMirrorRebuildRequest
{
	FString TreePath;
};

bool ParseRuntimeMirrorRebuildRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FRuntimeMirrorRebuildRequest& OutRequest,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	const FAssetDocumentRuntimeValueDto& Value = Invocation.ValueArguments;
	if (!IsAssetDocumentAICanonicalRuntimeValue(Value) || !HasExactFields(Value, {TEXT("behavior_tree_path"), TEXT("update_flags")}))
	{
		OutFailure = Failure(TEXT("AIProvider.InvalidValueShape"), TEXT("The runtime-mirror rebuild request is not canonical."));
		return false;
	}
	const FAssetDocumentRuntimeValueDto* TreePath = FindField(Value, TEXT("behavior_tree_path"));
	const FAssetDocumentRuntimeValueDto* UpdateFlags = FindField(Value, TEXT("update_flags"));
	if (!IsCanonicalObjectPath(TreePath) || UpdateFlags == nullptr || UpdateFlags->Kind != EAssetDocumentRuntimeValueKind::Integer || UpdateFlags->IntegerValue != UBehaviorTreeGraph::KeepRebuildCounter)
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.UpdateFlagsRejected"), TEXT("Only the literal KeepRebuildCounter update flag is admitted."));
		return false;
	}
	OutRequest.TreePath = TreePath->StringValue;
	return true;
}

bool IsExactRuntimeMirrorIdentity(
	const FAssetDocumentEditorAssetIdentityView& Identity,
	const FRuntimeMirrorRebuildRequest& Request,
	const FAssetDocumentAIBehaviorTreeGraphLeaseView& GraphLease,
	UBehaviorTree*& OutTree,
	UBehaviorTreeGraph*& OutGraph)
{
	OutTree = Cast<UBehaviorTree>(&Identity.GetObject());
	OutGraph = OutTree != nullptr ? Cast<UBehaviorTreeGraph>(OutTree->BTGraph) : nullptr;
	return OutTree != nullptr && Identity.GetCanonicalObjectPath() == Request.TreePath
		&& GraphLease.LiveBehaviorTreePath == Request.TreePath && GraphLease.Graph != nullptr
		&& OutGraph != nullptr && OutGraph->GetClass() == UBehaviorTreeGraph::StaticClass()
		&& OutGraph->GetOuter() == OutTree && !OutGraph->IsLocked();
}

bool RebuildRuntimeMirror(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	const FRuntimeMirrorRebuildRequest& Request,
	const IAssetDocumentVerifiedHandleLease& Lease,
	UBehaviorTreeGraph& Graph,
	IAssetDocumentAtomicTransaction& ActiveTransaction,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	const FString StablePath = FString::Printf(TEXT("asset:%s/RuntimeMirror"), *Request.TreePath);
	const FAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorResult Result = RebuildAssetDocumentBehaviorTreeRuntimeMirror(
		Lease, Graph, UBehaviorTreeGraph::KeepRebuildCounter,
		MakeFootprint(StablePath, RuntimeMirrorRollbackComponent), ActiveTransaction);
	if (!Result.bSucceeded)
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.RuntimeMirrorRebuildFailed"), TEXT("The integrated BehaviorTree runtime-mirror rebuild was rejected."), Result.Diagnostics);
		return false;
	}
	if (!InvalidateRebuiltBehaviorTree(Handles, Invocation))
	{
		OutFailure = Failure(TEXT("AIProvider.BehaviorTree.RuntimeMirrorRebuildFailed"), TEXT("The rebuilt BehaviorTree handles could not be invalidated."));
		return false;
	}
	OutFailure.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	OutFailure.Value = ObjectValue({TEXT("execution_order_hash"), TEXT("node_count"), TEXT("root_node_class_path")},
		{StringValue(Result.ExecutionOrderHash), IntegerValue(Result.NodeCount), StringValue(Result.RootNodeClassPath)});
	OutFailure.ChangedSet.Add(Changed(RuntimeMirrorRollbackComponent, Request.TreePath, StablePath));
	return true;
}

FAssetDocumentPrimitiveOutcome RebuildRuntimeMirrorWithEditorIdentity(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	const FRuntimeMirrorRebuildRequest& Request,
	const IAssetDocumentVerifiedHandleLease& Lease,
	const FAssetDocumentAIBehaviorTreeGraphLeaseView& GraphLease,
	IAssetDocumentAtomicTransaction& ActiveTransaction)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	const FAssetDocumentContractValidationResult IdentityResult = WithAssetDocumentEditorAssetIdentity(Lease,
		[&](const FAssetDocumentEditorAssetIdentityView& Identity)
	{
		UBehaviorTree* Tree = nullptr;
		UBehaviorTreeGraph* Graph = nullptr;
		if (!IsExactRuntimeMirrorIdentity(Identity, Request, GraphLease, Tree, Graph))
		{
			Outcome = Failure(TEXT("AIProvider.InvalidHandle"), TEXT("The Asset lease, scratch provenance lease, and unlocked live BehaviorTree graph do not share one exact identity."));
			return FAssetDocumentContractValidationResult();
		}
		return RebuildRuntimeMirror(Invocation, Handles, Request, Lease, *Graph, ActiveTransaction, Outcome)
			? Valid() : FAssetDocumentContractValidationResult();
	});
	return IdentityResult.bValid || !Outcome.Diagnostics.IsEmpty() ? Outcome
		: Failure(TEXT("AIProvider.InvalidHandle"), TEXT("The mutable Asset lease could not enter Editor identity."), IdentityResult.Diagnostics);
}
} // namespace

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeSelectorResolve(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AIBehaviorTreeCacheRefreshLocal;
	if (!IsAssetDocumentAITransactionValid(Invocation, ActiveTransaction))
		return Failure(TEXT("AIProvider.InvalidTransaction"), TEXT("Selector resolution requires the active AI mutation transaction."));
	FSelectorResolveRequest Request;
	FAssetDocumentPrimitiveOutcome FailureOutcome;
	if (!ParseSelectorResolveRequest(Invocation, Request, FailureOutcome)) return FailureOutcome;
	UBlackboardData* Blackboard = ResolveBlackboard(Request.BlackboardPath);
	if (Blackboard == nullptr)
		return Failure(TEXT("AIProvider.BehaviorTree.BlackboardMismatch"), TEXT("The requested Blackboard asset is not an exact loaded BlackboardData."));
	FAssetDocumentVerifiedLeaseResult LeaseResult = AcquireAssetDocumentAIMutableAssetLease(Handles, Invocation);
	if (LeaseResult.Lease == nullptr)
		return Failure(TEXT("AIProvider.InvalidHandle"), TEXT("A matching AI mutable Asset lease is required."), LeaseResult.Diagnostics);
	return ResolveSelectorWithEditorIdentity(Request, *Blackboard, *LeaseResult.Lease, *ActiveTransaction);
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeRuntimeMirrorRebuild(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace AIBehaviorTreeCacheRefreshLocal;
	if (!IsAssetDocumentAITransactionValid(Invocation, ActiveTransaction))
		return Failure(TEXT("AIProvider.InvalidTransaction"), TEXT("Runtime-mirror rebuild requires the active AI mutation transaction."));
	FRuntimeMirrorRebuildRequest Request;
	FAssetDocumentPrimitiveOutcome FailureOutcome;
	if (!ParseRuntimeMirrorRebuildRequest(Invocation, Request, FailureOutcome)) return FailureOutcome;
	FAssetDocumentVerifiedLeaseResult AssetLease = AcquireAssetDocumentAIMutableAssetLease(Handles, Invocation);
	if (AssetLease.Lease == nullptr)
		return Failure(TEXT("AIProvider.InvalidHandle"), TEXT("A matching AI mutable Asset lease is required."), AssetLease.Diagnostics);
	TArray<FAssetDocumentRuntimeDiagnostic> GraphDiagnostics;
	FAssetDocumentAIBehaviorTreeGraphLeaseView GraphLease;
	if (!AcquireAssetDocumentAIBehaviorTreeGraphLease(Invocation, Handles, TEXTVIEW("graph"), GraphLease, GraphDiagnostics))
		return Failure(TEXT("AIProvider.InvalidHandle"), TEXT("The graph handle is not an exact live BehaviorTree graph."), GraphDiagnostics);
	return RebuildRuntimeMirrorWithEditorIdentity(Invocation, Handles, Request, *AssetLease.Lease, GraphLease, *ActiveTransaction);
}
