// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/AssetDocumentAIPrimitiveCatalog.h"
#include "Provider/Blackboard/AssetDocumentAIBlackboardPrimitives.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreePrimitives.h"

namespace AssetDocumentAIPrimitiveCatalog
{
FAssetDocumentPrimitiveAdmissionEvidence MakeSelectorEvidenceAdmission()
{
	FAssetDocumentPrimitiveAdmissionEvidence Evidence;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Evidence.ExactEngineVersion = TEXT("5.7.4");
	Evidence.PhysicalOwner = EAssetDocumentPrimitiveOwner::AI;
	Evidence.PublicModule = TEXT("PropertyEditor;AIModule");
	Evidence.PublicHeader = TEXT("PropertyEditorModule.h;IPropertyRowGenerator.h;PropertyHandle.h");
	Evidence.ExportedAuthoritativeApi =
		TEXT("FPropertyEditorModule::CreatePropertyRowGenerator;IPropertyRowGenerator::SetObjects;IPropertyHandle");
	Evidence.AuthorityContextId = TEXT("assetdocument.ai.selector-evidence.game-thread.editor-ready.v1");
	Evidence.PrimaryOperationId = TEXT("propertyeditor.selector-inspect-evidence.v1");
	Evidence.IdentityLifetimeContractId = TEXT("path-resolved-during-game-thread-invocation.v1");
	Evidence.TransactionContractId = TEXT("read-only-no-active-transaction.v1");
	Evidence.RollbackContractId = TEXT("read-only-no-rollback.v1");
	Evidence.CompletionContractId = TEXT("selector-evidence-dto-only.v1");
	Evidence.SaveReloadEvidenceId = TEXT("selector-evidence-no-persistence.v1");
	Evidence.FailureInjectionTestIds = {
		TEXT("UEAssetDoc.AI.SelectorEvidence.Primitive")};
	Evidence.VerificationTestIds = {
		TEXT("UEAssetDoc.AI.SelectorEvidence.HelperFirst"),
		TEXT("UEAssetDoc.AI.SelectorEvidence.Primitive")};
	Evidence.AuthorityFingerprint =
		TEXT("ue-5.7.4|ai|propertyeditor.selector-inspect-evidence.v1|public-row-generator-property-handle");
	return Evidence;
}

FAssetDocumentPrimitiveDescriptor MakeSelectorEvidenceDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = TEXT("ai.behavior_tree.selector_inspect_evidence.v1");
	Descriptor.Version = 1;
	Descriptor.ProviderId = TEXT("ai");
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::AI;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = EAssetDocumentPrimitiveAccess::ReadOnly;
	Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Verify};
	Descriptor.bRequiresActiveTransaction = false;
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v1");
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v1");
	Descriptor.StableDiagnosticCodes = {
		TEXT("PrimitiveInvocation.InvalidValueShape"),
		TEXT("AISelector.NonCanonicalSubject"),
		TEXT("AISelector.ClassMismatch"),
		TEXT("AISelector.SubjectUnavailable"),
		TEXT("AISelector.EvidenceBuildFailed")};
	Descriptor.RequiredConsumerIds = {TEXT("route.generic.v1")};
	Descriptor.Admission = MakeSelectorEvidenceAdmission();
	return Descriptor;
}
} // namespace AssetDocumentAIPrimitiveCatalog

TConstArrayView<FAssetDocumentPrimitiveDescriptor>
GetAssetDocumentAIPrimitiveDescriptors()
{
	static const TArray<FAssetDocumentPrimitiveDescriptor> Descriptors = []
	{
		TArray<FAssetDocumentPrimitiveDescriptor> Result;
		Result.Reserve(32);
		AppendAssetDocumentAIBlackboardPrimitiveDescriptors(Result);
		Result.Add(MakeAssetDocumentAIBlackboardLocalKeysReorderV2Descriptor());
		AppendAssetDocumentAIBehaviorTreePrimitiveDescriptors(Result);
		Result.Add(AssetDocumentAIPrimitiveCatalog::MakeSelectorEvidenceDescriptor());
		return Result;
	}();
	return Descriptors;
}

void AppendBlackboardV1DecisionProjectionRows(
	TArray<FAssetDocumentAIDecisionProjectionRow>& Rows)
{
	Rows.Append({
		{TEXT("ai.blackboard.key_type.inspect.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.blackboard.key_type.create.v1"),
			EAssetDocumentPrimitiveDisposition::StableBlocker,
			TEXT("ai_provider_design"),
			TEXT("AD.BLOCK.AI.BLACKBOARD_KEY_TYPE_AUTHORITY")},
		{TEXT("ai.blackboard.key_type.apply_properties.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.blackboard.parent.set.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.blackboard.local_keys.replace.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")}});
}

void AppendBehaviorTreeDecisionProjectionRows(
	TArray<FAssetDocumentAIDecisionProjectionRow>& Rows)
{
	Rows.Append({
		{TEXT("ai.behavior_tree.binding.inspect.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper, TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.selector.inventory.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper, TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.graph.author_scratch.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper, TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.node_class.inspect.v2"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper, TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.blackboard.readback.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper, TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.authored_graph.readback.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.authored_graph.readback.v2"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.node_class.inspect.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.instance.validate_properties.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.selector.resolve_batch.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_blackboard_binding_repair"), TEXT("")},
		{TEXT("ai.behavior_tree.selector.resolve_batch.v2"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_blackboard_binding_repair"), TEXT("")},
		{TEXT("ai.behavior_tree.selector.resolve.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_blackboard_binding_repair"), TEXT("")},
		{TEXT("ai.behavior_tree.node.create_action.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.subnode.create_action.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.graph.connect.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.graph.create.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.graph.create_defaults.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.comment.create_action.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.decorator.ensure_input_count.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.runtime_mirror.rebuild.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.graph.install.v1"),
			EAssetDocumentPrimitiveDisposition::AdmittedBridge,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.blackboard.set.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.value_or_key.refresh.v1"),
			EAssetDocumentPrimitiveDisposition::AdmittedBridge,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.value_or_key.refresh.v2"),
			EAssetDocumentPrimitiveDisposition::AdmittedBridge,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.value_or_key.refresh.v3"),
			EAssetDocumentPrimitiveDisposition::AdmittedBridge,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.behavior_tree.decorator.refresh.v1"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")}});
}

void AppendBlackboardV2DecisionProjectionRows(
	TArray<FAssetDocumentAIDecisionProjectionRow>& Rows)
{
	Rows.Append({
		{TEXT("ai.blackboard.entry.readback.v2"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.blackboard.parent.readback.v2"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.blackboard.local_keys.readback.v2"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.blackboard.entry.update_existing.v2"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.blackboard.parent.set.v2"),
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.blackboard.entry.add.v2"),
			EAssetDocumentPrimitiveDisposition::AdmittedBridge,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.blackboard.entry.replace_class.v2"),
			EAssetDocumentPrimitiveDisposition::AdmittedBridge,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.blackboard.entry.remove.v2"),
			EAssetDocumentPrimitiveDisposition::AdmittedBridge,
			TEXT("ai_profile"), TEXT("")},
		{TEXT("ai.blackboard.local_keys.reorder.v2"),
			EAssetDocumentPrimitiveDisposition::AdmittedBridge,
			TEXT("ai_profile"), TEXT("")}});
}

TArray<FAssetDocumentAIDecisionProjectionRow> MakeAssetDocumentAIDecisionProjectionRows()
{
	TArray<FAssetDocumentAIDecisionProjectionRow> Rows;
	Rows.Reserve(31);
	AppendBlackboardV1DecisionProjectionRows(Rows);
	AppendBehaviorTreeDecisionProjectionRows(Rows);
	AppendBlackboardV2DecisionProjectionRows(Rows);
	return Rows;
}

TConstArrayView<FAssetDocumentAIDecisionProjectionRow> GetAssetDocumentAIDecisionProjectionRows()
{
	static const TArray<FAssetDocumentAIDecisionProjectionRow> Rows =
		MakeAssetDocumentAIDecisionProjectionRows();
	return Rows;
}
