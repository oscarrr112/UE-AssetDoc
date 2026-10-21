// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreePrimitives.h"

namespace AssetDocumentAIBehaviorTreeCatalog
{
struct FAuthorityIdentity
{
	const TCHAR* PublicModule;
	const TCHAR* PublicHeader;
	const TCHAR* ExportedAuthoritativeApi;
};

FAuthorityIdentity GetAuthorityIdentity(FStringView PrimitiveId)
{
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.binding.inspect.v1"))
		return {TEXT("AIModule+CoreUObject+PropertyEditor+AssetDocumentEditor"),
			TEXT("BehaviorTree/BehaviorTreeTypes.h+BehaviorTree/ValueOrBBKey.h+BehaviorTree/BlackboardData.h+UObject/StructOnScope.h+PropertyHandle.h+AssetDocumentEditorNativeState.h"),
			TEXT("FBlackboardKeySelector::AllowedTypes>UBlackboardKeyType::IsAllowedByFilter>FValueOrBBKey::IsCompatibleType>UBlackboardData::IsChildOf>UBehaviorTree::RootDecorators")};
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.selector.inventory.v1"))
		return {TEXT("AssetDocumentEditor+AIModule+BehaviorTreeEditor+PropertyEditor"),
			TEXT("AssetDocumentEditorNativeState.h+BehaviorTreeGraph.h+PropertyEditorModule.h+IPropertyRowGenerator.h+PropertyHandle.h"),
			TEXT("WithAssetDocumentEditorAssetIdentity>FPropertyEditorModule::CreatePropertyRowGenerator>IPropertyRowGenerator::SetObjects>IPropertyHandle::GetPropertyPath>IPropertyHandle::GetValue")};
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.graph.author_scratch.v1"))
		return {TEXT("BehaviorTreeEditor+AIGraph+PropertyEditor+UnrealEd"),
			TEXT("EdGraphSchema_BehaviorTree.h+AIGraphSchema.h+EdGraph/EdGraphSchema.h+PropertyHandle.h+PropertyCustomizationHelpers.h"),
			TEXT("UEdGraphSchema_BehaviorTree::CreateDefaultNodesForGraph>FEdGraphSchemaAction::PerformAction>UEdGraphSchema::TryCreateConnection>IPropertyHandle::SetValueFromFormattedString>PropertyCustomizationHelpers::CreateNewInstanceOfEditInlineObjectClass")};
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.node_class.inspect.v2"))
		return {TEXT("CoreUObject+PropertyEditor"),
			TEXT("UObject/UnrealType.h+UObject/StructOnScope.h+PropertyEditorModule.h+IPropertyRowGenerator.h+PropertyHandle.h"),
			TEXT("UClass::GetDefaultObject>FPropertyEditorModule::CreatePropertyRowGenerator>IPropertyRowGenerator::SetObjects>IPropertyRowGenerator::SetStructure>IPropertyHandle::GetValue")};
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.blackboard.readback.v1"))
		return {TEXT("AssetDocumentEditor+AIModule"),
			TEXT("AssetDocumentEditorNativeState.h+BehaviorTree/BehaviorTree.h+BehaviorTree/BlackboardAssetProvider.h"),
			TEXT("WithAssetDocumentEditorAssetIdentity>UBehaviorTree::GetBlackboardAsset")};
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.decorator.ensure_input_count.v1"))
		return {TEXT("BehaviorTreeEditor+UnrealEd"), TEXT("BehaviorTreeDecoratorGraph.h+EdGraph/EdGraphSchema.h+EdGraph/EdGraph.h"),
			TEXT("UBehaviorTreeDecoratorGraph::SpawnMissingNodes>UEdGraphSchema::BreakNodeLinks>UEdGraph::RemoveNode>UEdGraphSchema::TryCreateConnection")};
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.graph.install.v1"))
	{
		return {TEXT("AssetDocumentAI+CoreUObject+AIModule+BehaviorTreeEditor"),
			TEXT("BehaviorTree/AssetDocumentAIBehaviorTreeGraphInstall.h+UObject/Object.h+UObject/UObjectHash.h+BehaviorTree/BehaviorTree.h+BehaviorTreeGraph.h"),
			TEXT("InstallExactGraph>CaptureAndRegisterInstalledReplacementCompletion>UObject::Rename>UBehaviorTree::BTGraph>UBehaviorTree::LastEditedDocuments>RecompleteGraphInstallInverse")};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.value_or_key.refresh.v1") || (PrimitiveId == TEXTVIEW("ai.behavior_tree.value_or_key.refresh.v2") || PrimitiveId == TEXTVIEW("ai.behavior_tree.value_or_key.refresh.v3")))
	{
		return {TEXT("AssetDocumentAI+AIModule+BehaviorTreeEditor+PropertyEditor"),
			TEXT("BehaviorTree/AssetDocumentAIBehaviorTreeDerivedCompletion.h+PropertyHandle.h+BehaviorTree/BlackboardData.h+BehaviorTreeGraph.h"),
			TEXT("RunAssetDocumentBehaviorTreeDerivedCompletion/ValueOrDecoratorMirror/CurrentGraph>CompleteExactCurrentGraphValueOrInventory>InvalidateExactValueOrBlackboardKeyCache")};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.decorator.refresh.v1"))
	{
		return {TEXT("AIModule+BehaviorTreeEditor+PropertyEditor"),
			TEXT("PropertyEditorModule.h+IPropertyRowGenerator.h+IDetailTreeNode.h+PropertyHandle.h+BehaviorTreeGraphNode_CompositeDecorator.h+BehaviorTreeGraph.h"),
			TEXT("FPropertyEditorModule::CreatePropertyRowGenerator>IPropertyRowGenerator::SetObjects>IDetailTreeNode::CreatePropertyHandle>IPropertyHandle::NotifyPreChange>IPropertyHandle::NotifyPostChange>IPropertyHandle::NotifyFinishedChangingProperties>UBehaviorTreeGraphNode_CompositeDecorator::OnInnerGraphChanged>UBehaviorTreeGraph::UpdateAsset")};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.authored_graph.readback.v1") || PrimitiveId == TEXTVIEW("ai.behavior_tree.authored_graph.readback.v2"))
	{
		return {
			TEXT("AIModule+BehaviorTreeEditor+PropertyEditor+AssetDocumentEditor"),
			TEXT("AssetDocumentEditorNativeState.h+BehaviorTree/BehaviorTree.h+BehaviorTreeGraph.h+BehaviorTreeGraphNode.h+PropertyEditorModule.h+IPropertyRowGenerator.h+IDetailTreeNode.h+PropertyHandle.h"),
			TEXT("WithAssetDocumentEditorAssetIdentity>UBehaviorTree::BTGraph>UEdGraph nodes/pins>FPropertyEditorModule::CreatePropertyRowGenerator>IPropertyRowGenerator::SetObjects>IDetailTreeNode::CreatePropertyHandle>IPropertyHandle::GetValue"),
		};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.node_class.inspect.v1")
		|| PrimitiveId == TEXTVIEW("ai.behavior_tree.instance.validate_properties.v1"))
	{
		return {
			TEXT("PropertyEditor"),
			TEXT("PropertyEditorModule.h+IPropertyRowGenerator.h+PropertyHandle.h"),
			TEXT("FPropertyEditorModule::CreatePropertyRowGenerator>IPropertyRowGenerator::SetObjects>IPropertyHandle"),
		};
	}
	if ((PrimitiveId == TEXTVIEW("ai.behavior_tree.selector.resolve_batch.v1") || PrimitiveId == TEXTVIEW("ai.behavior_tree.selector.resolve_batch.v2")))
	{
		return {TEXT("AssetDocumentAI+AIModule+PropertyEditor"),
			TEXT("TypedPreimage/AssetDocumentAISelectorTypedPreimageInternal.h+BehaviorTree/BehaviorTreeTypes.h+PropertyEditorModule.h+IPropertyRowGenerator.h+PropertyHandle.h"),
			TEXT("CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorBatchPreimage>FBlackboardKeySelector::ResolveSelectedKey")};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.selector.resolve.v1"))
	{
		return {
			TEXT("AssetDocumentAI+AIModule+PropertyEditor"),
			TEXT("TypedPreimage/AssetDocumentAISelectorTypedPreimage.h+BehaviorTree/BehaviorTreeTypes.h+PropertyEditorModule.h+IPropertyRowGenerator.h+PropertyHandle.h"),
			TEXT("CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorPreimage>FBlackboardKeySelector::ResolveSelectedKey"),
		};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.graph.create.v1"))
	{
		return {
			TEXT("CoreUObject+AIModule+BehaviorTreeEditor+UnrealEd"),
			TEXT("UObject/Class.h+UObject/UObjectGlobals.h+UObject/UObjectBaseUtility.h+UObject/StrongObjectPtr.h+BehaviorTree/BehaviorTree.h+BehaviorTreeFactory.h+BehaviorTreeGraph.h+EdGraphSchema_BehaviorTree.h+Kismet2/BlueprintEditorUtils.h"),
			TEXT("UClass::GetDefaultObject>GetTransientPackage>MakeUniqueObjectName>UBehaviorTreeFactory::FactoryCreateNew>FBlueprintEditorUtils::CreateNewGraph>UObjectBaseUtility::SetFlags>TStrongObjectPtr"),
		};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.graph.create_defaults.v1"))
	{
		return {
			TEXT("CoreUObject+AIModule+BehaviorTreeEditor+UnrealEd"),
			TEXT("UObject/Class.h+UObject/UObjectGlobals.h+UObject/UObjectBaseUtility.h+UObject/StrongObjectPtr.h+BehaviorTree/BehaviorTree.h+BehaviorTreeFactory.h+BehaviorTreeGraph.h+Kismet2/BlueprintEditorUtils.h+EdGraphSchema_BehaviorTree.h"),
			TEXT("UClass::GetDefaultObject>GetTransientPackage>MakeUniqueObjectName>UBehaviorTreeFactory::FactoryCreateNew>FBlueprintEditorUtils::CreateNewGraph>UObjectBaseUtility::SetFlags>TStrongObjectPtr>UEdGraphSchema_BehaviorTree::CreateDefaultNodesForGraph"),
		};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.node.create_action.v1"))
	{
		return {
			TEXT("CoreUObject+Engine+AIGraph+BehaviorTreeEditor+UnrealEd"),
			TEXT("BehaviorTreeEditorModule.h+IBehaviorTreeEditor.h+BehaviorTreeFactory.h+AssetEditorToolkit.h+AssetEditorSubsystem.h+EdGraphSchema.h+EdGraphSchema_BehaviorTree.h+AIGraphSchema.h+AIGraphTypes.h"),
			TEXT("UBehaviorTreeFactory::FactoryCreateNew>FBehaviorTreeEditorModule::CreateBehaviorTreeEditor>FAssetEditorToolkit::CloseWindow>FBehaviorTreeEditorModule::GetClassCache>FGraphNodeClassHelper::GatherClasses>UEdGraphSchema_BehaviorTree::GetGraphContextActions>FGraphActionListBuilderBase::GetSchemaAction>FEdGraphSchemaAction::PerformAction"),
		};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.subnode.create_action.v1"))
	{
		return {
			TEXT("CoreUObject+Engine+AIGraph+BehaviorTreeEditor+UnrealEd"),
			TEXT("BehaviorTreeEditorModule.h+IBehaviorTreeEditor.h+BehaviorTreeFactory.h+AssetEditorToolkit.h+AssetEditorSubsystem.h+EdGraphSchema.h+EdGraphSchema_BehaviorTree.h+AIGraphSchema.h+AIGraphTypes.h"),
			TEXT("UBehaviorTreeFactory::FactoryCreateNew>FBehaviorTreeEditorModule::CreateBehaviorTreeEditor>FAssetEditorToolkit::CloseWindow>FBehaviorTreeEditorModule::GetClassCache>FGraphNodeClassHelper::GatherClasses>UEdGraphSchema_BehaviorTree::GetGraphNodeContextActions>FGraphActionListBuilderBase::GetSchemaAction>FEdGraphSchemaAction::PerformAction"),
		};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.comment.create_action.v1"))
	{
		return {
			TEXT("CoreUObject+Engine+AIGraph+BehaviorTreeEditor+UnrealEd"),
			TEXT("BehaviorTreeEditorModule.h+IBehaviorTreeEditor.h+BehaviorTreeFactory.h+AssetEditorToolkit.h+AssetEditorSubsystem.h+EdGraphSchema.h+EdGraphSchema_BehaviorTree.h+AIGraphSchema.h"),
			TEXT("UBehaviorTreeFactory::FactoryCreateNew>FBehaviorTreeEditorModule::CreateBehaviorTreeEditor>FAssetEditorToolkit::CloseWindow>FBehaviorTreeEditorModule::GetClassCache>UAIGraphSchema::GetCreateCommentAction>FAISchemaAction_AddComment::PerformAction"),
		};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.graph.connect.v1"))
	{
		return {
			TEXT("Engine+BehaviorTreeEditor"),
			TEXT("EdGraphSchema_BehaviorTree.h+EdGraph/EdGraphSchema.h"),
			TEXT("P-AI-SCRATCH-BUNDLE-v1>UEdGraphSchema::TryCreateConnection"),
		};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.runtime_mirror.rebuild.v1"))
	{
		return {
			TEXT("AssetDocumentAI"),
			TEXT("BehaviorTree/AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollback.h"),
			TEXT("RebuildAssetDocumentBehaviorTreeRuntimeMirror"),
		};
	}
	return {
		TEXT("AIModule+BehaviorTreeEditor+PropertyEditor+AssetDocumentEditor"),
		TEXT("AssetDocumentEditorNativeState.h+BehaviorTree/BlackboardAssetProvider.h+BehaviorTree/BehaviorTree.h+BehaviorTreeGraph.h+BehaviorTreeGraphNode_Root.h+PropertyEditorModule.h+IPropertyRowGenerator.h+IDetailTreeNode.h+PropertyHandle.h"),
		TEXT("WithAssetDocumentEditorAssetIdentity>CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage>FPropertyEditorModule::CreatePropertyRowGenerator>IPropertyRowGenerator::SetPropertyGenerationAllowListPaths>IPropertyRowGenerator::SetObjects>IDetailTreeNode::CreatePropertyHandle>IPropertyHandle::SetValue>UBehaviorTreeGraphNode_Root::PostEditChangeProperty>UBehaviorTreeGraphNode_Root::UpdateBlackboard>UBehaviorTreeGraph::UpdateBlackboardChange>UBTNode::InitializeFromAsset>UAIGraph::UpdateErrorMessages"),
	};
}

TArray<FString> GetOperationDiagnostics(FStringView PrimitiveId)
{
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.binding.inspect.v1"))
		return {TEXT("AIProvider.BehaviorTree.NodeClassUnavailable"),
			TEXT("AIProvider.BehaviorTree.NodeClassRejected"),
			TEXT("AIProvider.BehaviorTree.BindingLocatorRejected"),
			TEXT("AIProvider.BehaviorTree.BindingInspectionUnavailable")};
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.selector.inventory.v1"))
		return {TEXT("AIProvider.BehaviorTree.SelectorBatchInvalidInventory"),
			TEXT("AIProvider.BehaviorTree.SelectorPropertyRejected")};
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.graph.author_scratch.v1"))
		return {TEXT("AIProvider.BehaviorTree.ScratchAuthoringRejected"),
			TEXT("AIProvider.BehaviorTree.ScratchReadbackRejected")};
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.blackboard.readback.v1"))
		return {TEXT("AIProvider.BehaviorTree.BlackboardMismatch")};
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.decorator.ensure_input_count.v1"))
		return {TEXT("AIProvider.BehaviorTree.DecoratorInputRejected"), TEXT("AIProvider.BehaviorTree.DecoratorInputBuildFailed"), TEXT("AIProvider.BehaviorTree.DecoratorInputRollbackFailed")};
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.graph.install.v1"))
	{
		return {TEXT("AIProvider.BehaviorTree.GraphInstallPreimageMismatch"),
			TEXT("AIProvider.BehaviorTree.GraphInstallOwnershipRejected"),
			TEXT("AIProvider.BehaviorTree.GraphInstallRegistrationRejected"),
			TEXT("AIProvider.BehaviorTree.GraphInstallFootprintRejected"),
			TEXT("AIProvider.BehaviorTree.GraphInstallTransferFailed"),
			TEXT("AIProvider.BehaviorTree.GraphInstallHandleIssueFailed"),
			TEXT("AIProvider.BehaviorTree.GraphInstallCompletionMismatch"),
			TEXT("AIProvider.BehaviorTree.GraphInstallRollbackFailed")};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.value_or_key.refresh.v1") || (PrimitiveId == TEXTVIEW("ai.behavior_tree.value_or_key.refresh.v2") || PrimitiveId == TEXTVIEW("ai.behavior_tree.value_or_key.refresh.v3")))
	{
		return {TEXT("AIProvider.BehaviorTree.ValueOrInventoryRejected"),
			TEXT("AIProvider.BehaviorTree.ValueOrRegistrationRejected"),
			TEXT("AIProvider.BehaviorTree.ValueOrBlackboardMismatch"),
			TEXT("AIProvider.BehaviorTree.ValueOrKeyTypeMismatch"),
			TEXT("AIProvider.BehaviorTree.ValueOrInvalidationFailed"),
			TEXT("AIProvider.BehaviorTree.ValueOrCompletionMismatch"),
			TEXT("AIProvider.BehaviorTree.DecoratorInventoryRejected"),
			TEXT("AIProvider.BehaviorTree.DecoratorSelectorUnresolved"),
			TEXT("AIProvider.BehaviorTree.DecoratorOperationPropertyRejected"),
			TEXT("AIProvider.BehaviorTree.DecoratorNotificationFailed"),
			TEXT("AIProvider.BehaviorTree.DecoratorCompletionMismatch")};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.decorator.refresh.v1"))
	{
		return {TEXT("AIProvider.BehaviorTree.DecoratorInventoryRejected"),
			TEXT("AIProvider.BehaviorTree.DecoratorSelectorUnresolved"),
			TEXT("AIProvider.BehaviorTree.DecoratorOperationPropertyRejected"),
			TEXT("AIProvider.BehaviorTree.DecoratorNotificationFailed"),
			TEXT("AIProvider.BehaviorTree.DecoratorCompletionMismatch")};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.authored_graph.readback.v1") || PrimitiveId == TEXTVIEW("ai.behavior_tree.authored_graph.readback.v2"))
	{
		return {TEXT("AIProvider.BehaviorTree.GraphReadbackInvalid"),
			TEXT("AIProvider.BehaviorTree.GraphReadbackUnsupportedNode"),
			TEXT("AIProvider.BehaviorTree.GraphReadbackTopologyRejected"),
			TEXT("AIProvider.BehaviorTree.GraphReadbackPropertyRejected"),
			TEXT("AIProvider.BehaviorTree.GraphReadbackProjectionOverflow")};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.node_class.inspect.v1")
		|| PrimitiveId == TEXTVIEW("ai.behavior_tree.node_class.inspect.v2"))
	{
		TArray<FString> Codes = {TEXT("AIProvider.BehaviorTree.NodeClassUnavailable"),
			TEXT("AIProvider.BehaviorTree.NodeClassRejected"),
			TEXT("AIProvider.BehaviorTree.PropertyEvidenceUnavailable")};
		if (PrimitiveId == TEXTVIEW("ai.behavior_tree.node_class.inspect.v2"))
			Codes.Append({TEXT("AIProvider.BehaviorTree.NodeClassAbstract"), TEXT("AIProvider.BehaviorTree.NodeClassDeprecated")});
		return Codes;
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.instance.validate_properties.v1"))
	{
		return {TEXT("AIProvider.BehaviorTree.NodeMissing"),
			TEXT("AIProvider.BehaviorTree.NodeClassMismatch"),
			TEXT("AIProvider.BehaviorTree.PropertyNotAdmitted")};
	}
	if ((PrimitiveId == TEXTVIEW("ai.behavior_tree.selector.resolve_batch.v1") || PrimitiveId == TEXTVIEW("ai.behavior_tree.selector.resolve_batch.v2")))
	{
		return {TEXT("AIProvider.BehaviorTree.SelectorBatchInvalidInventory"),
			TEXT("AIProvider.BehaviorTree.SelectorBatchDuplicateLocator"),
			TEXT("AIProvider.BehaviorTree.SelectorPropertyRejected"),
			TEXT("AIProvider.BehaviorTree.BlackboardMismatch"),
			TEXT("AIProvider.BehaviorTree.SelectorResolveFailed"),
			TEXT("AIProvider.BehaviorTree.SelectorBatchCompletionMismatch")};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.selector.resolve.v1"))
	{
		return {TEXT("AIProvider.BehaviorTree.SelectorPropertyRejected"),
			TEXT("AIProvider.BehaviorTree.BlackboardMismatch"),
			TEXT("AIProvider.BehaviorTree.SelectorResolveFailed")};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.node.create_action.v1"))
	{
		return {TEXT("AIProvider.BehaviorTree.ActionUnavailable"),
			TEXT("AIProvider.BehaviorTree.ActionClassMismatch"),
			TEXT("AIProvider.BehaviorTree.ActionInvocationFailed")};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.subnode.create_action.v1"))
	{
		return {TEXT("AIProvider.BehaviorTree.SubnodeParentRejected"),
			TEXT("AIProvider.BehaviorTree.ActionUnavailable"),
			TEXT("AIProvider.BehaviorTree.ActionInvocationFailed")};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.graph.connect.v1"))
	{
		return {TEXT("AIProvider.BehaviorTree.PinMissing"),
			TEXT("AIProvider.BehaviorTree.ConnectionRejected"),
			TEXT("AIProvider.BehaviorTree.CrossGraphConnection")};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.graph.create.v1"))
	{
		return {TEXT("AIProvider.BehaviorTree.GraphCreateFailed"),
			TEXT("AIProvider.BehaviorTree.GraphUnexpectedlyInstalled")};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.graph.create_defaults.v1"))
	{
		return {TEXT("AIProvider.BehaviorTree.DefaultNodesRejected"),
			TEXT("AIProvider.BehaviorTree.RootInvariantFailed")};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.comment.create_action.v1"))
	{
		return {TEXT("AIProvider.BehaviorTree.CommentActionUnavailable"),
			TEXT("AIProvider.BehaviorTree.CommentActionFailed")};
	}
	if (PrimitiveId == TEXTVIEW("ai.behavior_tree.runtime_mirror.rebuild.v1"))
	{
		return {TEXT("AIProvider.BehaviorTree.UpdateFlagsRejected"),
			TEXT("AIProvider.BehaviorTree.RuntimeMirrorRebuildFailed"),
			TEXT("AIProvider.BehaviorTree.RuntimeMirrorMismatch")};
	}
	return {TEXT("AIProvider.BehaviorTree.BlackboardClassMismatch"),
		TEXT("AIProvider.BehaviorTree.BlackboardAssignmentRejected"),
		TEXT("AIProvider.BehaviorTree.BlackboardNotificationFailed")};
}

FAssetDocumentPrimitiveAdmissionEvidence MakeAdmission(const FString& PrimitiveId)
{
	const FAuthorityIdentity Authority = GetAuthorityIdentity(PrimitiveId);
	FAssetDocumentPrimitiveAdmissionEvidence Evidence;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath =
		EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Evidence.ExactEngineVersion = TEXT("5.7.4");
	Evidence.PhysicalOwner = EAssetDocumentPrimitiveOwner::AI;
	Evidence.PublicModule = Authority.PublicModule;
	Evidence.PublicHeader = Authority.PublicHeader;
	Evidence.ExportedAuthoritativeApi = Authority.ExportedAuthoritativeApi;
	Evidence.AuthorityContextId =
		PrimitiveId + TEXT(".context.game-thread-editor-ready.v1");
	Evidence.PrimaryOperationId = PrimitiveId;
	Evidence.IdentityLifetimeContractId =
		PrimitiveId + TEXT(".identity.request-transaction-revision.v1");
	Evidence.TransactionContractId =
		PrimitiveId + TEXT(".transaction.active-or-read-only.v1");
	Evidence.RollbackContractId =
		PrimitiveId + TEXT(".rollback.atomic-or-read-only.v1");
	Evidence.CompletionContractId =
		PrimitiveId + TEXT(".completion.validated-outcome.v1");
	Evidence.SaveReloadEvidenceId =
		PrimitiveId + TEXT(".provider-no-persistence-fresh-reload.v1");
	Evidence.FailureInjectionTestIds = {
		TEXT("UEAssetDoc.AIProvider.BehaviorTreeActionSchemaLifecycle"),
	};
	Evidence.VerificationTestIds = {
		TEXT("UEAssetDoc.AIProvider.Authority.PublicSurface"),
		TEXT("UEAssetDoc.AIProvider.BehaviorTreeActionSchemaLifecycle"),
	};
	if (PrimitiveId == TEXT("ai.behavior_tree.binding.inspect.v1"))
	{
		Evidence.IdentityLifetimeContractId = PrimitiveId + TEXT(".identity.phase-local-read-only.v1");
		Evidence.TransactionContractId = PrimitiveId + TEXT(".transaction.none.v1");
		Evidence.RollbackContractId = PrimitiveId + TEXT(".rollback.no-mutation.v1");
		Evidence.FailureInjectionTestIds = {TEXT("UEAssetDoc.AIProvider.BehaviorTree.BindingInspect")};
		Evidence.VerificationTestIds = Evidence.FailureInjectionTestIds;
	}
	if (PrimitiveId == TEXT("ai.behavior_tree.runtime_mirror.rebuild.v1")
		|| PrimitiveId == TEXT("ai.behavior_tree.blackboard.set.v1"))
	{
		Evidence.FailureInjectionTestIds.Add(
			TEXT("UEAssetDoc.AIProvider.CacheRefresh"));
		Evidence.VerificationTestIds.Add(
			TEXT("UEAssetDoc.AIProvider.CacheRefresh"));
	}
	if (PrimitiveId == TEXT("ai.behavior_tree.decorator.refresh.v1"))
	{
		Evidence.FailureInjectionTestIds = {TEXT("UEAssetDoc.AIProvider.BehaviorTree.DecoratorRefresh")};
		Evidence.VerificationTestIds = {TEXT("UEAssetDoc.AIProvider.BehaviorTree.DecoratorRefresh"),
			TEXT("UEAssetDoc.AIProvider.BehaviorTree.DerivedCompletionSourceGate")};
	}
	if ((PrimitiveId == TEXT("ai.behavior_tree.selector.resolve_batch.v1") || PrimitiveId == TEXT("ai.behavior_tree.selector.resolve_batch.v2")))
	{
		Evidence.FailureInjectionTestIds = {TEXT("UEAssetDoc.AI.SelectorBatch.PartialFailureZeroResidual"),
			TEXT("UEAssetDoc.AI.SelectorBatch.RejectedInventoryZeroResidual")};
		Evidence.VerificationTestIds = {TEXT("UEAssetDoc.AI.SelectorBatch.AtomicCountsAndReverseRollback"),
			TEXT("UEAssetDoc.AI.SelectorBatch.ScalarLateEnlistmentAndMutualExclusion")};
	}
	if (PrimitiveId == TEXT("ai.behavior_tree.value_or_key.refresh.v1") || (PrimitiveId == TEXT("ai.behavior_tree.value_or_key.refresh.v2") || PrimitiveId == TEXT("ai.behavior_tree.value_or_key.refresh.v3")))
	{
		Evidence.Disposition = EAssetDocumentPrimitiveDisposition::AdmittedBridge;
		Evidence.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::BridgeAdmitted;
		Evidence.GovernanceId = TEXT("AD.AI.BLACKBOARD.VALUE-OR-KEY-CACHE-INVALIDATION-v1");
		Evidence.SourceMarker = TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_ADMISSION(AD.AI.BLACKBOARD.VALUE-OR-KEY-CACHE-INVALIDATION-v1)");
		Evidence.RawLifecycleAllowlistId = TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_CALLSITE(AD.AI.BLACKBOARD.VALUE-OR-KEY-CACHE-INVALIDATION-v1|") + PrimitiveId + TEXT(")");
		Evidence.IndependentCandidateReviewId = TEXT("candidate-review.A2/AD.AI.BLACKBOARD.VALUE-OR-KEY-CACHE-INVALIDATION-v1");
		Evidence.IndependentBridgeReviewId = TEXT("bridge-review.BTProduction/AD.AI.BLACKBOARD.VALUE-OR-KEY-CACHE-INVALIDATION-v1");
		Evidence.HelperReplacementCondition = TEXT("Replace only with a stock public exported exact ValueOr cache invalidation lifecycle covering all eleven families.");
		Evidence.RemovalCondition = TEXT("Remove this exact CurrentGraph caller after equivalent forward, rollback and fresh-read evidence passes.");
		Evidence.RollbackContractId = TEXT("assetdocument.ai.behavior_tree.derived_completion.v1");
		Evidence.CompletionContractId = PrimitiveId + TEXT(".current_graph.single-terminal-finalizer.v1");
		Evidence.FailureInjectionTestIds = {TEXT("UEAssetDoc.AIProvider.BehaviorTree.ValueOrKeyRefresh")};
		Evidence.VerificationTestIds = {TEXT("UEAssetDoc.AIProvider.BehaviorTree.ValueOrKeyRefresh"),
			TEXT("UEAssetDoc.AIProvider.BehaviorTree.DerivedCompletionSourceGate")};
	}
	if (PrimitiveId == TEXT("ai.behavior_tree.graph.install.v1"))
	{
		Evidence.Disposition = EAssetDocumentPrimitiveDisposition::AdmittedBridge;
		Evidence.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::BridgeAdmitted;
		Evidence.GovernanceId = TEXT("AD.AI.BT.GRAPH_INSTALL.v1");
		Evidence.SourceMarker = TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_ADMISSION(AD.AI.BT.GRAPH_INSTALL.v1)");
		Evidence.RawLifecycleAllowlistId = TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_CALLSITE(AD.AI.BT.GRAPH_INSTALL.v1|ai.behavior_tree.graph.install.v1)");
		Evidence.IndependentCandidateReviewId = TEXT("candidate-review.A2/AD.AI.BT.GRAPH_INSTALL.v1");
		Evidence.IndependentBridgeReviewId = TEXT("bridge-review.D5/AD.AI.BT.GRAPH_INSTALL.v1");
		Evidence.HelperReplacementCondition = TEXT("Replace with stock exported graph ownership transfer including optional-old rollback and scratch/live capability transition.");
		Evidence.RemovalCondition = TEXT("Remove only after equivalent installation, inverse ownership and fresh-read evidence passes.");
		Evidence.RollbackContractId = TEXT("assetdocument.ai.behavior_tree.graph_install.v1");
		Evidence.CompletionContractId = TEXT("ai.behavior_tree.graph.install.v1.installed_replacement.single-finalizer.v1");
		Evidence.FailureInjectionTestIds = {TEXT("UEAssetDoc.AIProvider.BehaviorTree.GraphInstall")};
		Evidence.VerificationTestIds = {TEXT("UEAssetDoc.AIProvider.BehaviorTree.GraphInstall"),
			TEXT("UEAssetDoc.AIProvider.BehaviorTree.DerivedCompletionSourceGate")};
	}
	Evidence.AuthorityFingerprint =
		TEXT("ue-5.7.4-launcher-cl-51494982|ai|") + PrimitiveId
		+ TEXT("|") + Evidence.PublicModule
		+ TEXT("|") + Evidence.ExportedAuthoritativeApi;
	return Evidence;
}

FAssetDocumentPrimitiveHandleArgumentSchema MakeMutableArgument(
	const TCHAR* Name,
	EAssetDocumentHandleKind Kind)
{
	FAssetDocumentPrimitiveHandleArgumentSchema Argument;
	Argument.Name = Name;
	Argument.Kind = Kind;
	Argument.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	return Argument;
}

FAssetDocumentPrimitiveHandleResultSchema MakeMutableResult(
	const TCHAR* Name,
	EAssetDocumentHandleKind Kind)
{
	FAssetDocumentPrimitiveHandleResultSchema Result;
	Result.Name = Name;
	Result.Kind = Kind;
	Result.GrantedAccess = EAssetDocumentHandleAccess::Mutable;
	Result.Cardinality = EAssetDocumentHandleResultCardinality::ExactlyOne;
	return Result;
}

FAssetDocumentPrimitiveDescriptor MakeOrdinaryDescriptor(
	const TCHAR* PrimitiveId,
	EAssetDocumentPrimitiveAccess Access,
	TArray<EAssetDocumentRuntimeRequestPhase> AllowedPhases,
	const TCHAR* MutationFootprintSchemaId,
	const TCHAR* ConsumerId,
	TArray<FAssetDocumentPrimitiveHandleArgumentSchema> HandleArguments = {},
	TArray<FAssetDocumentPrimitiveHandleResultSchema> HandleResults = {}, int32 Version = 1)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = PrimitiveId;
	Descriptor.Version = Version;
	Descriptor.ProviderId = TEXT("ai");
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::AI;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = Access;
	Descriptor.AllowedPhases = MoveTemp(AllowedPhases);
	Descriptor.bRequiresActiveTransaction =
		Access != EAssetDocumentPrimitiveAccess::ReadOnly;
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + FString::Printf(TEXT(".input.v%d"), Version);
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + FString::Printf(TEXT(".output.v%d"), Version);
	Descriptor.MutationFootprintSchemaId = MutationFootprintSchemaId;
	Descriptor.HandleArgumentSchemas = MoveTemp(HandleArguments);
	Descriptor.HandleResultSchemas = MoveTemp(HandleResults);
	for (const FAssetDocumentPrimitiveHandleArgumentSchema& Argument :
		Descriptor.HandleArgumentSchemas)
	{
		Descriptor.AllowedHandleKinds.AddUnique(Argument.Kind);
	}
	for (const FAssetDocumentPrimitiveHandleResultSchema& Result :
		Descriptor.HandleResultSchemas)
	{
		Descriptor.AllowedHandleKinds.AddUnique(Result.Kind);
	}
	Descriptor.StableDiagnosticCodes = {
		TEXT("AIProvider.InvalidValueShape"),
		TEXT("AIProvider.InvalidContext"),
		TEXT("AIProvider.InvalidTransaction"),
		TEXT("AIProvider.InvalidHandle"),
		TEXT("AIProvider.AuthorityUnavailable"),
		TEXT("AIProvider.OperationRejected"),
	};
	Descriptor.StableDiagnosticCodes.Append(
		GetOperationDiagnostics(Descriptor.PrimitiveId));
	Descriptor.RequiredConsumerIds = {ConsumerId};
	Descriptor.Admission = MakeAdmission(Descriptor.PrimitiveId);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeBlockerDescriptor(
	const TCHAR* PrimitiveId,
	const TCHAR* StableDiagnosticCode,
	EAssetDocumentRuntimeRequestPhase AllowedPhase)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = PrimitiveId;
	Descriptor.Version = 1;
	Descriptor.ProviderId = TEXT("ai");
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::AI;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = EAssetDocumentPrimitiveAccess::ReadOnly;
	Descriptor.AllowedPhases = {AllowedPhase};
	Descriptor.StableDiagnosticCodes = {StableDiagnosticCode};
	Descriptor.Admission.Disposition =
		EAssetDocumentPrimitiveDisposition::StableBlocker;
	Descriptor.Admission.AuthorityPath =
		EAssetDocumentPrimitiveAuthorityPath::StableBlocker;
	Descriptor.Admission.ExactEngineVersion = TEXT("5.7.4");
	Descriptor.Admission.PhysicalOwner = EAssetDocumentPrimitiveOwner::AI;
	Descriptor.Admission.PrimaryOperationId = PrimitiveId;
	Descriptor.Admission.AuthorityFingerprint =
		FString(TEXT("g1|ai|")) + PrimitiveId
		+ TEXT("|") + StableDiagnosticCode;
	return Descriptor;
}
} // namespace AssetDocumentAIBehaviorTreeCatalog

void AppendAssetDocumentAIBehaviorTreePrimitiveDescriptors(
	TArray<FAssetDocumentPrimitiveDescriptor>& OutDescriptors)
{
	using namespace AssetDocumentAIBehaviorTreeCatalog;
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.binding.inspect.v1"), EAssetDocumentPrimitiveAccess::ReadOnly,
		{EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Verify}, TEXT(""), TEXT("ai_profile"),
		{FAssetDocumentPrimitiveHandleArgumentSchema{TEXT("asset"), EAssetDocumentHandleKind::Asset,
			EAssetDocumentHandleAccess::ReadOnly, false}}));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.selector.inventory.v1"), EAssetDocumentPrimitiveAccess::ReadOnly,
		{EAssetDocumentRuntimeRequestPhase::Preflight, EAssetDocumentRuntimeRequestPhase::Apply,
			EAssetDocumentRuntimeRequestPhase::Repair, EAssetDocumentRuntimeRequestPhase::Verify},
		TEXT(""), TEXT("ai_profile"),
		{FAssetDocumentPrimitiveHandleArgumentSchema{TEXT("asset"), EAssetDocumentHandleKind::Asset,
			EAssetDocumentHandleAccess::ReadOnly, true}}));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.graph.author_scratch.v1"), EAssetDocumentPrimitiveAccess::Scratch,
		{EAssetDocumentRuntimeRequestPhase::Apply},
		TEXT("assetdocument.ai.behavior_tree.scratch_graph.v1"), TEXT("ai_profile"),
		{MakeMutableArgument(TEXT("graph"), EAssetDocumentHandleKind::Graph)}));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.node_class.inspect.v2"), EAssetDocumentPrimitiveAccess::ReadOnly,
		{EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair,
			EAssetDocumentRuntimeRequestPhase::Verify}, TEXT(""), TEXT("ai_profile"), {}, {}, 2));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.blackboard.readback.v1"), EAssetDocumentPrimitiveAccess::ReadOnly,
		{EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight, EAssetDocumentRuntimeRequestPhase::Apply,
			EAssetDocumentRuntimeRequestPhase::Repair, EAssetDocumentRuntimeRequestPhase::Verify},
		TEXT(""), TEXT("ai_profile"),
		{FAssetDocumentPrimitiveHandleArgumentSchema{TEXT("asset"), EAssetDocumentHandleKind::Asset,
			EAssetDocumentHandleAccess::ReadOnly, true}}));

	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.authored_graph.readback.v1"),
		EAssetDocumentPrimitiveAccess::ReadOnly,
		{EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply,
			EAssetDocumentRuntimeRequestPhase::Repair,
			EAssetDocumentRuntimeRequestPhase::Verify},
		TEXT(""),
		TEXT("ai_profile"),
		{FAssetDocumentPrimitiveHandleArgumentSchema{TEXT("asset"),
			EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly, true}}));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.authored_graph.readback.v2"),
		EAssetDocumentPrimitiveAccess::ReadOnly,
		{EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply,
			EAssetDocumentRuntimeRequestPhase::Repair,
			EAssetDocumentRuntimeRequestPhase::Verify},
		TEXT(""),
		TEXT("ai_profile"),
		{FAssetDocumentPrimitiveHandleArgumentSchema{TEXT("asset"),
			EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly, true}}, {}, 2));

	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.node_class.inspect.v1"),
		EAssetDocumentPrimitiveAccess::ReadOnly,
		{EAssetDocumentRuntimeRequestPhase::Validate,
			EAssetDocumentRuntimeRequestPhase::Preflight},
		TEXT(""),
		TEXT("ai_profile")));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.instance.validate_properties.v1"),
		EAssetDocumentPrimitiveAccess::ReadOnly,
		{EAssetDocumentRuntimeRequestPhase::Validate,
			EAssetDocumentRuntimeRequestPhase::Preflight},
		TEXT(""),
		TEXT("ai_profile")));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.selector.resolve_batch.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		{EAssetDocumentRuntimeRequestPhase::Repair},
		TEXT("assetdocument.ai.behavior_tree.selector_cache.v1"),
		TEXT("ai_blackboard_binding_repair"),
		{MakeMutableArgument(TEXT("asset"), EAssetDocumentHandleKind::Asset)}));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.selector.resolve_batch.v2"),
		EAssetDocumentPrimitiveAccess::Mutating,
		{EAssetDocumentRuntimeRequestPhase::Repair},
		TEXT("assetdocument.ai.behavior_tree.selector_cache.v1"),
		TEXT("ai_blackboard_binding_repair"),
		{MakeMutableArgument(TEXT("asset"), EAssetDocumentHandleKind::Asset)}, {}, 2));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.selector.resolve.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		{EAssetDocumentRuntimeRequestPhase::Repair},
		TEXT("assetdocument.ai.behavior_tree.selector_cache.v1"),
		TEXT("ai_blackboard_binding_repair"),
		{MakeMutableArgument(TEXT("asset"), EAssetDocumentHandleKind::Asset)}));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.node.create_action.v1"),
		EAssetDocumentPrimitiveAccess::Scratch,
		{EAssetDocumentRuntimeRequestPhase::Apply},
		TEXT("assetdocument.ai.behavior_tree.scratch_graph.v1"),
		TEXT("ai_profile"),
		{MakeMutableArgument(TEXT("graph"), EAssetDocumentHandleKind::Graph)},
		{MakeMutableResult(TEXT("node"), EAssetDocumentHandleKind::Node)}));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.subnode.create_action.v1"),
		EAssetDocumentPrimitiveAccess::Scratch,
		{EAssetDocumentRuntimeRequestPhase::Apply},
		TEXT("assetdocument.ai.behavior_tree.scratch_graph.v1"),
		TEXT("ai_profile"),
		{MakeMutableArgument(TEXT("graph"), EAssetDocumentHandleKind::Graph),
			MakeMutableArgument(TEXT("parent_node"), EAssetDocumentHandleKind::Node)},
		{MakeMutableResult(TEXT("node"), EAssetDocumentHandleKind::Node)}));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.graph.connect.v1"),
		EAssetDocumentPrimitiveAccess::Scratch,
		{EAssetDocumentRuntimeRequestPhase::Apply},
		TEXT("assetdocument.ai.behavior_tree.scratch_graph.v1"),
		TEXT("ai_profile"),
		{MakeMutableArgument(TEXT("graph"), EAssetDocumentHandleKind::Graph),
			MakeMutableArgument(TEXT("from_node"), EAssetDocumentHandleKind::Node),
			MakeMutableArgument(TEXT("to_node"), EAssetDocumentHandleKind::Node)}));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.graph.create.v1"),
		EAssetDocumentPrimitiveAccess::Scratch,
		{EAssetDocumentRuntimeRequestPhase::Apply},
		TEXT("assetdocument.ai.behavior_tree.scratch_bundle.v1"),
		TEXT("ai_profile"),
		{},
		{MakeMutableResult(TEXT("graph"), EAssetDocumentHandleKind::Graph)}));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.graph.create_defaults.v1"),
		EAssetDocumentPrimitiveAccess::Scratch,
		{EAssetDocumentRuntimeRequestPhase::Apply},
		TEXT("assetdocument.ai.behavior_tree.scratch_graph.v1"),
		TEXT("ai_profile"),
		{MakeMutableArgument(TEXT("graph"), EAssetDocumentHandleKind::Graph)},
		{MakeMutableResult(TEXT("root_node"), EAssetDocumentHandleKind::Node)}));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.comment.create_action.v1"),
		EAssetDocumentPrimitiveAccess::Scratch,
		{EAssetDocumentRuntimeRequestPhase::Apply},
		TEXT("assetdocument.ai.behavior_tree.scratch_graph.v1"),
		TEXT("ai_profile"),
		{MakeMutableArgument(TEXT("graph"), EAssetDocumentHandleKind::Graph)},
		{MakeMutableResult(TEXT("node"), EAssetDocumentHandleKind::Node)}));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.decorator.ensure_input_count.v1"),
		EAssetDocumentPrimitiveAccess::Scratch, {EAssetDocumentRuntimeRequestPhase::Apply},
		TEXT("assetdocument.ai.behavior_tree.scratch_graph.v1"), TEXT("ai_profile"),
		{MakeMutableArgument(TEXT("graph"), EAssetDocumentHandleKind::Graph)}));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.runtime_mirror.rebuild.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		{EAssetDocumentRuntimeRequestPhase::Repair},
		TEXT("assetdocument.ai.behavior_tree.runtime_mirror.v1"),
		TEXT("ai_profile"),
		{MakeMutableArgument(TEXT("asset"), EAssetDocumentHandleKind::Asset),
			MakeMutableArgument(TEXT("graph"), EAssetDocumentHandleKind::Graph)}));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.graph.install.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		{EAssetDocumentRuntimeRequestPhase::Apply},
		TEXT("assetdocument.ai.behavior_tree.graph_install.v1"), TEXT("ai_profile"),
		{MakeMutableArgument(TEXT("asset"), EAssetDocumentHandleKind::Asset),
			MakeMutableArgument(TEXT("graph"), EAssetDocumentHandleKind::Graph)},
		{MakeMutableResult(TEXT("live_graph"), EAssetDocumentHandleKind::Graph)}));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.blackboard.set.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		{EAssetDocumentRuntimeRequestPhase::Apply},
		TEXT("assetdocument.ai.behavior_tree.blackboard_asset.v1"),
		TEXT("ai_profile"),
		{MakeMutableArgument(TEXT("asset"), EAssetDocumentHandleKind::Asset)}));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.value_or_key.refresh.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		{EAssetDocumentRuntimeRequestPhase::Repair},
		TEXT("assetdocument.ai.behavior_tree.derived_completion.v1"),
		TEXT("ai_profile"),
		{MakeMutableArgument(TEXT("asset"), EAssetDocumentHandleKind::Asset)}));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.value_or_key.refresh.v2"),
		EAssetDocumentPrimitiveAccess::Mutating,
		{EAssetDocumentRuntimeRequestPhase::Repair},
		TEXT("assetdocument.ai.behavior_tree.derived_completion.v1"),
		TEXT("ai_profile"),
		{MakeMutableArgument(TEXT("asset"), EAssetDocumentHandleKind::Asset)}, {}, 2));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.value_or_key.refresh.v3"),
		EAssetDocumentPrimitiveAccess::Mutating,
		{EAssetDocumentRuntimeRequestPhase::Repair},
		TEXT("assetdocument.ai.behavior_tree.derived_completion.v1"),
		TEXT("ai_profile"),
		{MakeMutableArgument(TEXT("asset"), EAssetDocumentHandleKind::Asset)}, {}, 3));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.behavior_tree.decorator.refresh.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		{EAssetDocumentRuntimeRequestPhase::Repair},
		TEXT("assetdocument.ai.behavior_tree.derived_completion.v1"),
		TEXT("ai_profile"),
		{MakeMutableArgument(TEXT("asset"), EAssetDocumentHandleKind::Asset)}));
}
