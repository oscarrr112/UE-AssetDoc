// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetDocumentAIPrimitiveProvider.h"
#include "Provider/AssetDocumentAIPrimitiveCatalog.h"
#include "Provider/AssetDocumentAIPrimitiveHandler.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreePrimitives.h"

#include "Misc/AutomationTest.h"
#include "Runtime/AssetDocumentHandleRegistry.h"

// This module-private seam is defined by ProviderAssembly.  It deliberately
// accepts a copied projection so the catalog contract can prove drift rejection
// without mutating the frozen static catalog.
bool ValidateAssetDocumentAIDecisionProjection(
	TConstArrayView<FAssetDocumentAIDecisionProjectionRow> CandidateRows);

namespace AssetDocumentAIProviderContractTests
{
struct FExpectedDecision
{
	const TCHAR* PrimitiveId;
	uint32 Version;
	const TCHAR* DecisionConsumerId;
	const TCHAR* RuntimeConsumerId;
	const TCHAR* StableBlockerDiagnostic;
};

constexpr FExpectedDecision ExpectedDecisions[] = {
	{TEXT("ai.behavior_tree.binding.inspect.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.selector.inventory.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.blackboard.readback.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.graph.author_scratch.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.node_class.inspect.v2"), 2, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.blackboard.key_type.inspect.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.blackboard.key_type.create.v1"), 1, TEXT("ai_provider_design"), TEXT(""), TEXT("AD.BLOCK.AI.BLACKBOARD_KEY_TYPE_AUTHORITY")},
	{TEXT("ai.blackboard.key_type.apply_properties.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.blackboard.parent.set.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.blackboard.local_keys.replace.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.authored_graph.readback.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.authored_graph.readback.v2"), 2, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.node_class.inspect.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.instance.validate_properties.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.selector.resolve.v1"), 1, TEXT("ai_blackboard_binding_repair"), TEXT("ai_blackboard_binding_repair"), TEXT("")},
	{TEXT("ai.behavior_tree.selector.resolve_batch.v1"), 1, TEXT("ai_blackboard_binding_repair"), TEXT("ai_blackboard_binding_repair"), TEXT("")},
	{TEXT("ai.behavior_tree.selector.resolve_batch.v2"), 2, TEXT("ai_blackboard_binding_repair"), TEXT("ai_blackboard_binding_repair"), TEXT("")},
	{TEXT("ai.behavior_tree.node.create_action.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.subnode.create_action.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.graph.connect.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.graph.create.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.graph.create_defaults.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.comment.create_action.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.decorator.ensure_input_count.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.runtime_mirror.rebuild.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.graph.install.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.blackboard.set.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.value_or_key.refresh.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.value_or_key.refresh.v2"), 2, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.value_or_key.refresh.v3"), 3, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.behavior_tree.decorator.refresh.v1"), 1, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.blackboard.entry.readback.v2"), 2, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.blackboard.parent.readback.v2"), 2, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.blackboard.local_keys.readback.v2"), 2, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.blackboard.entry.update_existing.v2"), 2, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.blackboard.parent.set.v2"), 2, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.blackboard.entry.add.v2"), 2, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.blackboard.entry.replace_class.v2"), 2, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.blackboard.entry.remove.v2"), 2, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
	{TEXT("ai.blackboard.local_keys.reorder.v2"), 2, TEXT("ai_profile"), TEXT("ai_profile"), TEXT("")},
};

constexpr const TCHAR* ExistingSelectorEvidenceId =
	TEXT("ai.behavior_tree.selector_inspect_evidence.v1");

struct FAssetMutationExpectation
{
	const TCHAR* PrimitiveId;
	uint32 Version;
};

constexpr FAssetMutationExpectation AssetMutations[] = {
	{TEXT("ai.blackboard.key_type.apply_properties.v1"), 1},
	{TEXT("ai.blackboard.parent.set.v1"), 1},
	{TEXT("ai.blackboard.local_keys.replace.v1"), 1},
	{TEXT("ai.blackboard.parent.set.v2"), 2},
	{TEXT("ai.blackboard.entry.add.v2"), 2},
	{TEXT("ai.blackboard.entry.replace_class.v2"), 2},
	{TEXT("ai.blackboard.entry.remove.v2"), 2},
	{TEXT("ai.blackboard.local_keys.reorder.v2"), 2},
	{TEXT("ai.behavior_tree.selector.resolve.v1"), 1},
	{TEXT("ai.behavior_tree.selector.resolve_batch.v1"), 1},
	{TEXT("ai.behavior_tree.runtime_mirror.rebuild.v1"), 1},
	{TEXT("ai.behavior_tree.blackboard.set.v1"), 1},
};

const FAssetDocumentPrimitiveDescriptor* FindDescriptor(
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors,
	const TCHAR* PrimitiveId,
	const uint32 Version = 1)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
	{
		if (Descriptor.PrimitiveId == PrimitiveId && Descriptor.Version == Version)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

void AssertReadbackV2Descriptor(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FString ExactHeader = TEXT("BehaviorTree/BlackboardData.h+BehaviorTree/Blackboard/BlackboardKeyType_Object.h+BehaviorTree/Blackboard/BlackboardKeyType_Class.h+BehaviorTree/Blackboard/BlackboardKeyType_Enum.h+BehaviorTree/Blackboard/BlackboardKeyType_Struct.h+StructUtils/InstancedStruct.h+UObject/UnrealType.h+UObject/Field.h+UObject/EnumProperty.h+Internationalization/Text.h+PropertyEditorModule.h+IPropertyRowGenerator.h+IDetailTreeNode.h+PropertyHandle.h");
	const FString ExactApi = TEXT("UBlackboardData::GetKey>FBlackboardEntry::KeyType>UBlackboardKeyType_Object::BaseClass>UBlackboardKeyType_Object::DefaultValue>UBlackboardKeyType_Class::BaseClass>UBlackboardKeyType_Class::DefaultValue>UBlackboardKeyType_Enum::EnumType>UBlackboardKeyType_Enum::EnumName>UBlackboardKeyType_Enum::DefaultValue>UEnum::GetIndexByValue>UEnum::GetNameStringByValue>UBlackboardKeyType_Struct::DefaultValue>FInstancedStruct::IsValid>FInstancedStruct::GetScriptStruct>FInstancedStruct::GetMemory>TFieldIterator<FProperty>>FField::GetAuthoredName>FProperty::ContainerPtrToValuePtr>FScriptArrayHelper>FScriptSetHelper>FScriptMapHelper>FTextInspector>FPropertyEditorModule::CreatePropertyRowGenerator>IPropertyRowGenerator::SetObjects>IDetailTreeNode::CreatePropertyHandle>IPropertyHandle::GetValue");
	Test.TestEqual(TEXT("readback v2 is versioned independently of v1"), Descriptor.Version, 2u);
	Test.TestEqual(TEXT("readback v2 is read-only"), Descriptor.Access,
		EAssetDocumentPrimitiveAccess::ReadOnly);
	Test.TestEqual(TEXT("readback v2 admits exactly the readback phases"), Descriptor.AllowedPhases,
		TArray<EAssetDocumentRuntimeRequestPhase>{
			EAssetDocumentRuntimeRequestPhase::Validate,
			EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Verify});
	Test.TestFalse(TEXT("readback v2 never requires an active transaction"),
		Descriptor.bRequiresActiveTransaction);
	Test.TestTrue(TEXT("readback v2 accepts no handles and produces no handle results"),
		Descriptor.HandleArgumentSchemas.IsEmpty() && Descriptor.HandleResultSchemas.IsEmpty());
	Test.TestEqual(TEXT("readback v2 input schema is exact"), Descriptor.InputSchemaId,
		FString(TEXT("ai.blackboard.entry.readback.v2.input.v2")));
	Test.TestEqual(TEXT("readback v2 output schema is exact"), Descriptor.OutputSchemaId,
		FString(TEXT("ai.blackboard.entry.readback.v2.output.v2")));
	Test.TestTrue(TEXT("readback v2 has no mutation footprint"),
		Descriptor.MutationFootprintSchemaId.IsEmpty());
	Test.TestEqual(TEXT("readback v2 has only its closed diagnostics"),
		Descriptor.StableDiagnosticCodes, TArray<FString>{
			TEXT("AIProvider.InvalidValueShape"),
			TEXT("AIProvider.InvalidContext"),
			TEXT("AIProvider.InvalidTransaction"),
			TEXT("AIProvider.InvalidHandle"),
			TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("AIProvider.OperationRejected"),
			TEXT("AIProvider.Blackboard.KeyMissing"),
			TEXT("AIProvider.Blackboard.KeyInherited"),
			TEXT("AIProvider.Blackboard.EntryReadbackUnavailable")});
	Test.TestEqual(TEXT("readback v2 public header chain binds the exact Struct seam"),
		Descriptor.Admission.PublicHeader, ExactHeader);
	Test.TestEqual(TEXT("readback v2 uses only the admitted public authority chain"),
		Descriptor.Admission.ExportedAuthoritativeApi, ExactApi);
	Test.TestEqual(TEXT("readback v2 fingerprint binds the private READ ID and live site"),
		Descriptor.Admission.AuthorityFingerprint,
		FString(TEXT("ue-5.7.4-launcher-cl-51494982|ai|ai.blackboard.entry.readback.v2|AssetDocumentAI+AIModule+PropertyEditor|"))
			+ ExactApi + TEXT("|AD.AI.BLACKBOARD.KEYTYPE-STRUCT-CANONICAL-READ-v1|AssetDocumentAIBlackboardKeyTypeStructCanonicalRead::ReadCanonicalValue(ExactLiveDefaultValue)"));
	Test.TestEqual(TEXT("readback v2 verifies the readback and the private canonical READ scope"),
		Descriptor.Admission.VerificationTestIds,
		TArray<FString>{TEXT("UEAssetDoc.AIProvider.Authority.PublicSurface"),
			TEXT("UEAssetDoc.AIProvider.BlackboardKeyTypeStructCanonicalRead"),
			TEXT("UEAssetDoc.AIProvider.BlackboardV2.Readback")});
	Test.TestEqual(TEXT("readback v2 has the readback identity contract"),
		Descriptor.Admission.IdentityLifetimeContractId,
		FString(TEXT("P.identity.path-resolve-single-invocation.v2")));
	Test.TestEqual(TEXT("readback v2 rejects active transactions by contract"),
		Descriptor.Admission.TransactionContractId,
		FString(TEXT("P.transaction.no-active-transaction.v2")));
	Test.TestEqual(TEXT("readback v2 has only the read-only rollback evidence"),
		Descriptor.Admission.RollbackContractId,
		FString(TEXT("ai.blackboard.entry.readback.v2.rollback.read_only.v2")));
	Test.TestEqual(TEXT("readback v2 has canonical-readback completion evidence"),
		Descriptor.Admission.CompletionContractId,
		FString(TEXT("ai.blackboard.entry.readback.v2.completion.canonical_readback.v2")));
}

void AssertParentReadbackV2Descriptor(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FString ExactApi = TEXT("exact /Script/AIModule.BlackboardData target class>FPropertyEditorModule::CreatePropertyRowGenerator>IPropertyRowGenerator::SetPropertyGenerationAllowListPaths>IPropertyRowGenerator::SetObjects({&ExactBlackboard})>IDetailTreeNode::CreatePropertyHandle>exact UBlackboardData::Parent IPropertyHandle>IPropertyHandle::GetValue(const UObject*&)");
	Test.TestEqual(TEXT("parent readback v2 is versioned independently"), Descriptor.Version, 2u);
	Test.TestEqual(TEXT("parent readback v2 is read-only"), Descriptor.Access,
		EAssetDocumentPrimitiveAccess::ReadOnly);
	Test.TestEqual(TEXT("parent readback v2 admits exactly read-only phases"), Descriptor.AllowedPhases,
		TArray<EAssetDocumentRuntimeRequestPhase>{
			EAssetDocumentRuntimeRequestPhase::Validate,
			EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Verify});
	Test.TestFalse(TEXT("parent readback v2 forbids active transactions"), Descriptor.bRequiresActiveTransaction);
	Test.TestTrue(TEXT("parent readback v2 has no handles or footprint"),
		Descriptor.HandleArgumentSchemas.IsEmpty() && Descriptor.HandleResultSchemas.IsEmpty()
			&& Descriptor.AllowedHandleKinds.IsEmpty() && Descriptor.MutationFootprintSchemaId.IsEmpty());
	Test.TestEqual(TEXT("parent readback v2 input schema is exact"), Descriptor.InputSchemaId,
		FString(TEXT("ai.blackboard.parent.readback.v2.input.v2")));
	Test.TestEqual(TEXT("parent readback v2 output schema is exact"), Descriptor.OutputSchemaId,
		FString(TEXT("ai.blackboard.parent.readback.v2.output.v2")));
	Test.TestEqual(TEXT("parent readback v2 has only its closed diagnostics"),
		Descriptor.StableDiagnosticCodes, TArray<FString>{
			TEXT("AIProvider.InvalidValueShape"), TEXT("AIProvider.InvalidContext"),
			TEXT("AIProvider.InvalidTransaction"), TEXT("AIProvider.InvalidHandle"),
			TEXT("AIProvider.AuthorityUnavailable"), TEXT("AIProvider.OperationRejected"),
			TEXT("AIProvider.Blackboard.ParentUnavailable"),
			TEXT("AIProvider.Blackboard.ParentClassMismatch"),
			TEXT("AIProvider.Blackboard.ParentReadbackUnavailable")});
	Test.TestEqual(TEXT("parent readback v2 uses only the admitted public authority"),
		Descriptor.Admission.ExportedAuthoritativeApi, ExactApi);
	Test.TestEqual(TEXT("parent readback v2 uses exact identity contract"),
		Descriptor.Admission.IdentityLifetimeContractId,
		FString(TEXT("ai.blackboard.parent.readback.v2.identity.path-resolve-single-invocation.v2")));
	Test.TestEqual(TEXT("parent readback v2 rejects transactions by contract"),
		Descriptor.Admission.TransactionContractId,
		FString(TEXT("ai.blackboard.parent.readback.v2.transaction.no-active-transaction.v2")));
	Test.TestEqual(TEXT("parent readback v2 has read-only rollback evidence"),
		Descriptor.Admission.RollbackContractId,
		FString(TEXT("ai.blackboard.parent.readback.v2.rollback.read_only.v2")));
	Test.TestEqual(TEXT("parent readback v2 has canonical completion evidence"),
		Descriptor.Admission.CompletionContractId,
		FString(TEXT("ai.blackboard.parent.readback.v2.completion.canonical_parent_readback.v2")));
	Test.TestEqual(TEXT("parent readback v2 has exact failure evidence"),
		Descriptor.Admission.FailureInjectionTestIds,
		TArray<FString>{TEXT("UEAssetDoc.AIProvider.BlackboardV2.ParentReadback.Failure")});
	Test.TestEqual(TEXT("parent readback v2 has exact verification evidence"),
		Descriptor.Admission.VerificationTestIds,
		TArray<FString>{TEXT("UEAssetDoc.AIProvider.Authority.PublicSurface"),
			TEXT("UEAssetDoc.AIProvider.BlackboardV2.ParentReadback")});
}

void AssertLocalKeysReadbackV2Descriptor(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FString PrimitiveId = TEXT("ai.blackboard.local_keys.readback.v2");
	const FString ExactApi = TEXT("exact /Script/AIModule.BlackboardData target class>UBlackboardData::GetKeys()>FBlackboardEntry::EntryName");
	Test.TestEqual(TEXT("local-keys readback v2 is versioned independently"), Descriptor.Version, 2u);
	Test.TestEqual(TEXT("local-keys readback v2 is read-only"), Descriptor.Access,
		EAssetDocumentPrimitiveAccess::ReadOnly);
	Test.TestEqual(TEXT("local-keys readback v2 admits exactly read-only phases"), Descriptor.AllowedPhases,
		TArray<EAssetDocumentRuntimeRequestPhase>{EAssetDocumentRuntimeRequestPhase::Validate,
			EAssetDocumentRuntimeRequestPhase::Preflight, EAssetDocumentRuntimeRequestPhase::Verify});
	Test.TestTrue(TEXT("local-keys readback v2 has no transaction handles or footprint"),
		!Descriptor.bRequiresActiveTransaction && Descriptor.HandleArgumentSchemas.IsEmpty()
			&& Descriptor.HandleResultSchemas.IsEmpty() && Descriptor.AllowedHandleKinds.IsEmpty()
			&& Descriptor.MutationFootprintSchemaId.IsEmpty());
	Test.TestEqual(TEXT("local-keys readback v2 input schema is exact"), Descriptor.InputSchemaId,
		PrimitiveId + TEXT(".input.v2"));
	Test.TestEqual(TEXT("local-keys readback v2 output schema is exact"), Descriptor.OutputSchemaId,
		PrimitiveId + TEXT(".output.v2"));
	Test.TestEqual(TEXT("local-keys readback v2 has only its closed diagnostics"),
		Descriptor.StableDiagnosticCodes, TArray<FString>{TEXT("AIProvider.InvalidValueShape"),
			TEXT("AIProvider.InvalidContext"), TEXT("AIProvider.InvalidTransaction"),
			TEXT("AIProvider.InvalidHandle"), TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("AIProvider.OperationRejected"), TEXT("AIProvider.Blackboard.LocalKeyNameInvalid"),
			TEXT("AIProvider.Blackboard.DuplicateKey"),
			TEXT("AIProvider.Blackboard.LocalKeysReadbackUnavailable")});
	Test.TestEqual(TEXT("local-keys readback v2 uses only the admitted public authority"),
		Descriptor.Admission.ExportedAuthoritativeApi, ExactApi);
	Test.TestEqual(TEXT("local-keys readback v2 uses exact identity contract"),
		Descriptor.Admission.IdentityLifetimeContractId, PrimitiveId + TEXT(".identity.path-resolve-single-invocation.v2"));
	Test.TestEqual(TEXT("local-keys readback v2 rejects transactions by contract"),
		Descriptor.Admission.TransactionContractId, PrimitiveId + TEXT(".transaction.no-active-transaction.v2"));
	Test.TestEqual(TEXT("local-keys readback v2 has read-only rollback evidence"),
		Descriptor.Admission.RollbackContractId, PrimitiveId + TEXT(".rollback.read_only.v2"));
	Test.TestEqual(TEXT("local-keys readback v2 has ordered-name completion evidence"),
		Descriptor.Admission.CompletionContractId, PrimitiveId + TEXT(".completion.ordered_local_names_readback.v2"));
	Test.TestEqual(TEXT("local-keys readback v2 has exact failure evidence"),
		Descriptor.Admission.FailureInjectionTestIds,
		TArray<FString>{TEXT("UEAssetDoc.AIProvider.BlackboardV2.LocalKeysReadback.Failure")});
	Test.TestEqual(TEXT("local-keys readback v2 has exact verification evidence"),
		Descriptor.Admission.VerificationTestIds,
		TArray<FString>{TEXT("UEAssetDoc.AIProvider.Authority.PublicSurface"),
			TEXT("UEAssetDoc.AIProvider.BlackboardV2.LocalKeysReadback")});
}

bool HasMutableAssetArgument(const FAssetDocumentPrimitiveDescriptor& Descriptor);

void AssertParentSetV2Descriptor(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FString PrimitiveId = TEXT("ai.blackboard.parent.set.v2");
	const FString ExactApi = TEXT("WithAssetDocumentEditorAssetIdentity>exact /Script/AIModule.BlackboardData target/desired-Parent class validation>UBlackboardData::IsChildOf(const UBlackboardData&) const>FPropertyEditorModule::CreatePropertyRowGenerator>IPropertyRowGenerator::SetPropertyGenerationAllowListPaths>IPropertyRowGenerator::SetObjects({&ExactBlackboard})>IDetailTreeNode::CreatePropertyHandle>exact UBlackboardData::Parent IPropertyHandle>IPropertyHandle::GetValue(const UObject*&)>CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage>CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage{public capture signature; rollback IPropertyHandle::SetValue(const UObject* const&, EPropertyValueSetFlags::NotTransactable)>IPropertyHandle::GetValue(const UObject*&)}>EnlistFootprint(UObjectState[LocalKeysDerivedClosure,ParentReference])>IPropertyHandle::SetValue(const UObject* const&, EPropertyValueSetFlags::NotTransactable)>UBlackboardData::PropagateKeyChangesToDerivedBlackboardAssets()>IPropertyHandle::GetValue(const UObject*&)");
	Test.TestEqual(TEXT("parent set v2 is versioned independently"), Descriptor.Version, 2u);
	Test.TestEqual(TEXT("parent set v2 is mutating"), Descriptor.Access,
		EAssetDocumentPrimitiveAccess::Mutating);
	Test.TestEqual(TEXT("parent set v2 admits only Apply"), Descriptor.AllowedPhases,
		TArray<EAssetDocumentRuntimeRequestPhase>{EAssetDocumentRuntimeRequestPhase::Apply});
	Test.TestTrue(TEXT("parent set v2 requires an active transaction"),
		Descriptor.bRequiresActiveTransaction);
	Test.TestTrue(TEXT("parent set v2 requires exactly mutable asset:HA"),
		HasMutableAssetArgument(Descriptor) && Descriptor.HandleArgumentSchemas.Num() == 1
			&& Descriptor.HandleResultSchemas.IsEmpty());
	Test.TestEqual(TEXT("parent set v2 allows only Asset handles"),
		Descriptor.AllowedHandleKinds,
		TArray<EAssetDocumentHandleKind>{EAssetDocumentHandleKind::Asset});
	Test.TestEqual(TEXT("parent set v2 input schema is exact"), Descriptor.InputSchemaId,
		PrimitiveId + TEXT(".input.v2"));
	Test.TestEqual(TEXT("parent set v2 output schema is exact"), Descriptor.OutputSchemaId,
		PrimitiveId + TEXT(".output.v2"));
	Test.TestEqual(TEXT("parent set v2 footprint schema is exact"),
		Descriptor.MutationFootprintSchemaId,
		FString(TEXT("assetdocument.ai.blackboard.parent.set.v2")));
	Test.TestEqual(TEXT("parent set v2 has only its closed diagnostics"),
		Descriptor.StableDiagnosticCodes,
		TArray<FString>{TEXT("AIProvider.InvalidValueShape"),
			TEXT("AIProvider.InvalidContext"), TEXT("AIProvider.InvalidTransaction"),
			TEXT("AIProvider.InvalidHandle"), TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("AIProvider.OperationRejected"),
			TEXT("AIProvider.Blackboard.ParentUnavailable"),
			TEXT("AIProvider.Blackboard.ParentClassMismatch"),
			TEXT("AIProvider.Blackboard.ParentCycle"),
			TEXT("AIProvider.Blackboard.ParentWriteRejected"),
			TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("AIProvider.Blackboard.ParentReadbackUnavailable"),
			TEXT("AIProvider.Blackboard.ParentChangedSetMismatch")});
	Test.TestEqual(TEXT("parent set v2 uses the exact public authority"),
		Descriptor.Admission.ExportedAuthoritativeApi, ExactApi);
	Test.TestEqual(TEXT("parent set v2 uses exact identity contract"),
		Descriptor.Admission.IdentityLifetimeContractId,
		PrimitiveId + TEXT(".identity.ha-request-transaction-revision.v2"));
	Test.TestEqual(TEXT("parent set v2 uses exact active-HA transaction contract"),
		Descriptor.Admission.TransactionContractId,
		PrimitiveId + TEXT(".transaction.active-ha.v2"));
	Test.TestEqual(TEXT("parent set v2 uses exact Atomic rollback contract"),
		Descriptor.Admission.RollbackContractId,
		FString(TEXT("assetdocument.ai.blackboard.parent.set.rollback.v2")));
	Test.TestEqual(TEXT("parent set v2 uses exact completion contract"),
		Descriptor.Admission.CompletionContractId,
		PrimitiveId + TEXT(".completion.parent_readback_and_derived_refresh.v2"));
	Test.TestEqual(TEXT("parent set v2 has exact save-reload evidence identity"),
		Descriptor.Admission.SaveReloadEvidenceId,
		PrimitiveId + TEXT(".provider-no-persistence-fresh-reload.v2"));
	Test.TestEqual(TEXT("parent set v2 has exact failure evidence"),
		Descriptor.Admission.FailureInjectionTestIds,
		TArray<FString>{TEXT("UEAssetDoc.AIProvider.BlackboardV2.ParentSet.Failure")});
	Test.TestEqual(TEXT("parent set v2 has exact verification evidence"),
		Descriptor.Admission.VerificationTestIds,
		TArray<FString>{TEXT("UEAssetDoc.EditorNativeState.ObjectReferencePreimageRollback"),
			TEXT("UEAssetDoc.AIProvider.Authority.PublicSurface"),
			TEXT("UEAssetDoc.AIProvider.BlackboardV2.ParentSet")});
}

void AssertUpdateExistingV2Descriptor(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	Test.TestEqual(TEXT("update-existing v2 is versioned independently of v1"), Descriptor.Version, 2u);
	Test.TestEqual(TEXT("update-existing v2 is mutating"), Descriptor.Access,
		EAssetDocumentPrimitiveAccess::Mutating);
	Test.TestEqual(TEXT("update-existing v2 admits only Apply"), Descriptor.AllowedPhases,
		TArray<EAssetDocumentRuntimeRequestPhase>{EAssetDocumentRuntimeRequestPhase::Apply});
	Test.TestTrue(TEXT("update-existing v2 requires an active transaction"),
		Descriptor.bRequiresActiveTransaction);
	Test.TestTrue(TEXT("update-existing v2 requires exactly mutable asset:HA"),
		HasMutableAssetArgument(Descriptor) && Descriptor.HandleArgumentSchemas.Num() == 1
			&& Descriptor.HandleResultSchemas.IsEmpty());
	Test.TestEqual(TEXT("update-existing v2 input schema is exact"), Descriptor.InputSchemaId,
		FString(TEXT("ai.blackboard.entry.update_existing.v2.input.v2")));
	Test.TestEqual(TEXT("update-existing v2 output schema is exact"), Descriptor.OutputSchemaId,
		FString(TEXT("ai.blackboard.entry.update_existing.v2.output.v2")));
	Test.TestEqual(TEXT("update-existing v2 footprint schema is exact"),
		Descriptor.MutationFootprintSchemaId,
		FString(TEXT("assetdocument.ai.blackboard.entry.update_existing.v2")));
	Test.TestEqual(TEXT("update-existing v2 has only its closed diagnostics"),
		Descriptor.StableDiagnosticCodes, TArray<FString>{
			TEXT("AIProvider.InvalidValueShape"),
			TEXT("AIProvider.InvalidContext"),
			TEXT("AIProvider.InvalidTransaction"),
			TEXT("AIProvider.InvalidHandle"),
			TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("AIProvider.OperationRejected"),
			TEXT("AIProvider.Blackboard.EntrySchemaInvalid"),
			TEXT("AIProvider.Blackboard.KeyMissing"),
			TEXT("AIProvider.Blackboard.KeyInherited"),
			TEXT("AIProvider.Blackboard.KeyClassMismatch"),
			TEXT("AIProvider.Blackboard.EntryMetadataForwardWriteUnavailable"),
			TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("AIProvider.Blackboard.EntryChangedSetMismatch")});
	Test.TestEqual(TEXT("update-existing v2 uses the exact identity contract"),
		Descriptor.Admission.IdentityLifetimeContractId,
		FString(TEXT("ai.blackboard.entry.update_existing.v2.identity.ha-request-transaction-revision.v2")));
	Test.TestEqual(TEXT("update-existing v2 uses the exact transaction contract"),
		Descriptor.Admission.TransactionContractId,
		FString(TEXT("ai.blackboard.entry.update_existing.v2.transaction.active-ha.v2")));
	Test.TestEqual(TEXT("update-existing v2 uses the existing Atomic rollback contract"),
		Descriptor.Admission.RollbackContractId,
		FString(TEXT("assetdocument.ai.blackboard.entry.update_existing.rollback.v2")));
	Test.TestEqual(TEXT("update-existing v2 completes with full readback"),
		Descriptor.Admission.CompletionContractId,
		FString(TEXT("ai.blackboard.entry.update_existing.v2.completion.full_readback.v2")));
	Test.TestEqual(TEXT("update-existing v2 has exact failure evidence"),
		Descriptor.Admission.FailureInjectionTestIds,
		TArray<FString>{TEXT("UEAssetDoc.AIProvider.BlackboardV2.UpdateExisting.Failure")});
	Test.TestEqual(TEXT("update-existing v2 has exact verification evidence"),
		Descriptor.Admission.VerificationTestIds,
		TArray<FString>{TEXT("UEAssetDoc.AIProvider.Authority.PublicSurface"),
			TEXT("UEAssetDoc.AIProvider.BlackboardV2.UpdateExisting")});
}

void AssertEntryAddV2Descriptor(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FString PrimitiveId = TEXT("ai.blackboard.entry.add.v2");
	Test.TestEqual(TEXT("entry add v2 is versioned independently"), Descriptor.Version, 2u);
	Test.TestEqual(TEXT("entry add v2 is mutating"), Descriptor.Access,
		EAssetDocumentPrimitiveAccess::Mutating);
	Test.TestEqual(TEXT("entry add v2 admits only Apply"), Descriptor.AllowedPhases,
		TArray<EAssetDocumentRuntimeRequestPhase>{EAssetDocumentRuntimeRequestPhase::Apply});
	Test.TestTrue(TEXT("entry add v2 requires an active transaction"),
		Descriptor.bRequiresActiveTransaction);
	Test.TestTrue(TEXT("entry add v2 requires exactly mutable asset:HA"),
		HasMutableAssetArgument(Descriptor) && Descriptor.HandleArgumentSchemas.Num() == 1
			&& Descriptor.HandleResultSchemas.IsEmpty());
	Test.TestEqual(TEXT("entry add v2 allows only Asset handles"),
		Descriptor.AllowedHandleKinds,
		TArray<EAssetDocumentHandleKind>{EAssetDocumentHandleKind::Asset});
	Test.TestEqual(TEXT("entry add v2 input schema is exact"), Descriptor.InputSchemaId,
		PrimitiveId + TEXT(".input.v2"));
	Test.TestEqual(TEXT("entry add v2 output schema is exact"), Descriptor.OutputSchemaId,
		PrimitiveId + TEXT(".output.v2"));
	Test.TestEqual(TEXT("entry add v2 footprint schema is exact"),
		Descriptor.MutationFootprintSchemaId,
		FString(TEXT("assetdocument.ai.blackboard.entry.add.v2")));
	Test.TestEqual(TEXT("entry add v2 has only D0 plus its frozen suffix"),
		Descriptor.StableDiagnosticCodes,
		TArray<FString>{TEXT("AIProvider.InvalidValueShape"),
			TEXT("AIProvider.InvalidContext"), TEXT("AIProvider.InvalidTransaction"),
			TEXT("AIProvider.InvalidHandle"), TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("AIProvider.OperationRejected"),
			TEXT("AIProvider.Blackboard.EntrySchemaInvalid"),
			TEXT("AIProvider.Blackboard.KeyAlreadyExists"),
			TEXT("AIProvider.Blackboard.KeyInherited"),
			TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"),
			TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("AIProvider.Blackboard.EntryChangedSetMismatch")});
	Test.TestEqual(TEXT("entry add v2 is admitted only through the staging bridge"),
		Descriptor.Admission.Disposition, EAssetDocumentPrimitiveDisposition::AdmittedBridge);
	Test.TestEqual(TEXT("entry add v2 bridge authority path is exact"),
		Descriptor.Admission.AuthorityPath, EAssetDocumentPrimitiveAuthorityPath::BridgeAdmitted);
	Test.TestEqual(TEXT("entry add v2 uses the staging governance record"),
		Descriptor.Admission.GovernanceId,
		FString(TEXT("AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1")));
	Test.TestEqual(TEXT("entry add v2 uses exact identity contract"),
		Descriptor.Admission.IdentityLifetimeContractId,
		PrimitiveId + TEXT(".identity.ha-request-transaction-revision.v2"));
	Test.TestEqual(TEXT("entry add v2 uses exact active-HA transaction contract"),
		Descriptor.Admission.TransactionContractId,
		PrimitiveId + TEXT(".transaction.active-ha.v2"));
	Test.TestEqual(TEXT("entry add v2 uses exact Atomic rollback contract"),
		Descriptor.Admission.RollbackContractId,
		FString(TEXT("assetdocument.ai.blackboard.entry.add.rollback.v2")));
	Test.TestEqual(TEXT("entry add v2 uses exact completion contract"),
		Descriptor.Admission.CompletionContractId,
		PrimitiveId + TEXT(".completion.full_readback_and_order.v2"));
	Test.TestEqual(TEXT("entry add v2 has exact save-reload evidence identity"),
		Descriptor.Admission.SaveReloadEvidenceId,
		PrimitiveId + TEXT(".provider-no-persistence-fresh-reload.v2"));
	Test.TestEqual(TEXT("entry add v2 has exact failure evidence"),
		Descriptor.Admission.FailureInjectionTestIds,
		TArray<FString>{TEXT("UEAssetDoc.AIProvider.BlackboardV2.Add.Failure")});
	Test.TestEqual(TEXT("entry add v2 has exact verification evidence"),
		Descriptor.Admission.VerificationTestIds,
		TArray<FString>{TEXT("UEAssetDoc.AIProvider.Authority.PublicSurface"),
			TEXT("UEAssetDoc.AIProvider.BlackboardV2.StructuralSourceCheck"),
			TEXT("UEAssetDoc.AIProvider.BlackboardV2.Add")});
}

void AssertEntryReplaceClassV2Descriptor(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FString PrimitiveId = TEXT("ai.blackboard.entry.replace_class.v2");
	Test.TestEqual(TEXT("replace-class v2 is versioned independently"), Descriptor.Version, 2u);
	Test.TestEqual(TEXT("replace-class v2 is mutating"), Descriptor.Access,
		EAssetDocumentPrimitiveAccess::Mutating);
	Test.TestEqual(TEXT("replace-class v2 admits only Apply"), Descriptor.AllowedPhases,
		TArray<EAssetDocumentRuntimeRequestPhase>{EAssetDocumentRuntimeRequestPhase::Apply});
	Test.TestTrue(TEXT("replace-class v2 requires an active transaction"),
		Descriptor.bRequiresActiveTransaction);
	Test.TestTrue(TEXT("replace-class v2 requires exactly mutable asset:HA"),
		HasMutableAssetArgument(Descriptor) && Descriptor.HandleArgumentSchemas.Num() == 1
			&& Descriptor.HandleResultSchemas.IsEmpty());
	Test.TestEqual(TEXT("replace-class v2 allows only Asset handles"),
		Descriptor.AllowedHandleKinds,
		TArray<EAssetDocumentHandleKind>{EAssetDocumentHandleKind::Asset});
	Test.TestEqual(TEXT("replace-class v2 input schema is exact"), Descriptor.InputSchemaId,
		PrimitiveId + TEXT(".input.v2"));
	Test.TestEqual(TEXT("replace-class v2 output schema is exact"), Descriptor.OutputSchemaId,
		PrimitiveId + TEXT(".output.v2"));
	Test.TestEqual(TEXT("replace-class v2 footprint schema is exact"),
		Descriptor.MutationFootprintSchemaId,
		FString(TEXT("assetdocument.ai.blackboard.entry.replace_class.v2")));
	Test.TestEqual(TEXT("replace-class v2 has only D0 plus its frozen suffix"),
		Descriptor.StableDiagnosticCodes,
		TArray<FString>{TEXT("AIProvider.InvalidValueShape"),
			TEXT("AIProvider.InvalidContext"), TEXT("AIProvider.InvalidTransaction"),
			TEXT("AIProvider.InvalidHandle"), TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("AIProvider.OperationRejected"),
			TEXT("AIProvider.Blackboard.EntrySchemaInvalid"),
			TEXT("AIProvider.Blackboard.KeyMissing"),
			TEXT("AIProvider.Blackboard.KeyInherited"),
			TEXT("AIProvider.Blackboard.KeyClassReplacementUnavailable"),
			TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"),
			TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("AIProvider.Blackboard.EntryChangedSetMismatch")});
	Test.TestEqual(TEXT("replace-class v2 is admitted only through the staging bridge"),
		Descriptor.Admission.Disposition, EAssetDocumentPrimitiveDisposition::AdmittedBridge);
	Test.TestEqual(TEXT("replace-class v2 bridge authority path is exact"),
		Descriptor.Admission.AuthorityPath, EAssetDocumentPrimitiveAuthorityPath::BridgeAdmitted);
	Test.TestEqual(TEXT("replace-class v2 uses the staging governance record"),
		Descriptor.Admission.GovernanceId,
		FString(TEXT("AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1")));
	Test.TestEqual(TEXT("replace-class v2 uses exact identity contract"),
		Descriptor.Admission.IdentityLifetimeContractId,
		PrimitiveId + TEXT(".identity.ha-request-transaction-revision.v2"));
	Test.TestEqual(TEXT("replace-class v2 uses exact active-HA transaction contract"),
		Descriptor.Admission.TransactionContractId,
		PrimitiveId + TEXT(".transaction.active-ha.v2"));
	Test.TestEqual(TEXT("replace-class v2 uses exact Atomic rollback contract"),
		Descriptor.Admission.RollbackContractId,
		FString(TEXT("assetdocument.ai.blackboard.entry.replace_class.rollback.v2")));
	Test.TestEqual(TEXT("replace-class v2 uses exact completion contract"),
		Descriptor.Admission.CompletionContractId,
		PrimitiveId + TEXT(".completion.full_readback.v2"));
	Test.TestEqual(TEXT("replace-class v2 has exact save-reload evidence identity"),
		Descriptor.Admission.SaveReloadEvidenceId,
		PrimitiveId + TEXT(".provider-no-persistence-fresh-reload.v2"));
	Test.TestEqual(TEXT("replace-class v2 has exact failure evidence"),
		Descriptor.Admission.FailureInjectionTestIds,
		TArray<FString>{TEXT("UEAssetDoc.AIProvider.BlackboardV2.ReplaceClass.Failure")});
	Test.TestEqual(TEXT("replace-class v2 has exact verification evidence"),
		Descriptor.Admission.VerificationTestIds,
		TArray<FString>{TEXT("UEAssetDoc.AIProvider.Authority.PublicSurface"),
			TEXT("UEAssetDoc.AIProvider.BlackboardV2.StructuralSourceCheck"),
			TEXT("UEAssetDoc.AIProvider.BlackboardV2.ReplaceClass")});
}

void AssertEntryRemoveV2Descriptor(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FString PrimitiveId = TEXT("ai.blackboard.entry.remove.v2");
	Test.TestEqual(TEXT("entry remove v2 is versioned independently"), Descriptor.Version, 2u);
	Test.TestEqual(TEXT("entry remove v2 is mutating"), Descriptor.Access,
		EAssetDocumentPrimitiveAccess::Mutating);
	Test.TestEqual(TEXT("entry remove v2 admits only Apply"), Descriptor.AllowedPhases,
		TArray<EAssetDocumentRuntimeRequestPhase>{EAssetDocumentRuntimeRequestPhase::Apply});
	Test.TestTrue(TEXT("entry remove v2 requires an active transaction"),
		Descriptor.bRequiresActiveTransaction);
	Test.TestTrue(TEXT("entry remove v2 requires exactly mutable asset:HA"),
		HasMutableAssetArgument(Descriptor) && Descriptor.HandleArgumentSchemas.Num() == 1
			&& Descriptor.HandleResultSchemas.IsEmpty());
	Test.TestEqual(TEXT("entry remove v2 allows only Asset handles"),
		Descriptor.AllowedHandleKinds,
		TArray<EAssetDocumentHandleKind>{EAssetDocumentHandleKind::Asset});
	Test.TestEqual(TEXT("entry remove v2 input schema is exact"), Descriptor.InputSchemaId,
		PrimitiveId + TEXT(".input.v2"));
	Test.TestEqual(TEXT("entry remove v2 output schema is exact"), Descriptor.OutputSchemaId,
		PrimitiveId + TEXT(".output.v2"));
	Test.TestEqual(TEXT("entry remove v2 footprint schema is exact"),
		Descriptor.MutationFootprintSchemaId,
		FString(TEXT("assetdocument.ai.blackboard.entry.remove.v2")));
	Test.TestEqual(TEXT("entry remove v2 has only D0 plus its frozen suffix"),
		Descriptor.StableDiagnosticCodes,
		TArray<FString>{TEXT("AIProvider.InvalidValueShape"),
			TEXT("AIProvider.InvalidContext"), TEXT("AIProvider.InvalidTransaction"),
			TEXT("AIProvider.InvalidHandle"), TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("AIProvider.OperationRejected"),
			TEXT("AIProvider.Blackboard.KeyMissing"),
			TEXT("AIProvider.Blackboard.KeyInherited"),
			TEXT("AIProvider.Blackboard.OrderedKeysLifecycleUnavailable"),
			TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("AIProvider.Blackboard.EntryChangedSetMismatch")});
	Test.TestEqual(TEXT("entry remove v2 is admitted only through the publication bridge"),
		Descriptor.Admission.Disposition, EAssetDocumentPrimitiveDisposition::AdmittedBridge);
	Test.TestEqual(TEXT("entry remove v2 bridge authority path is exact"),
		Descriptor.Admission.AuthorityPath, EAssetDocumentPrimitiveAuthorityPath::BridgeAdmitted);
	Test.TestEqual(TEXT("entry remove v2 uses the publication governance record"),
		Descriptor.Admission.GovernanceId,
		FString(TEXT("AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1")));
	Test.TestEqual(TEXT("entry remove v2 uses exact identity contract"),
		Descriptor.Admission.IdentityLifetimeContractId,
		PrimitiveId + TEXT(".identity.ha-request-transaction-revision.v2"));
	Test.TestEqual(TEXT("entry remove v2 uses exact active-HA transaction contract"),
		Descriptor.Admission.TransactionContractId,
		PrimitiveId + TEXT(".transaction.active-ha.v2"));
	Test.TestEqual(TEXT("entry remove v2 uses exact Atomic rollback contract"),
		Descriptor.Admission.RollbackContractId,
		FString(TEXT("assetdocument.ai.blackboard.entry.remove.rollback.v2")));
	Test.TestEqual(TEXT("entry remove v2 uses exact completion contract"),
		Descriptor.Admission.CompletionContractId,
		PrimitiveId + TEXT(".completion.absent_name_full_readback_and_order.v2"));
	Test.TestEqual(TEXT("entry remove v2 has exact save-reload evidence identity"),
		Descriptor.Admission.SaveReloadEvidenceId,
		PrimitiveId + TEXT(".provider-no-persistence-fresh-reload.v2"));
	Test.TestEqual(TEXT("entry remove v2 has exact failure evidence"),
		Descriptor.Admission.FailureInjectionTestIds,
		TArray<FString>{TEXT("UEAssetDoc.AIProvider.BlackboardV2.Remove.Failure")});
	Test.TestEqual(TEXT("entry remove v2 has exact verification evidence"),
		Descriptor.Admission.VerificationTestIds,
		TArray<FString>{TEXT("UEAssetDoc.AIProvider.Authority.PublicSurface"),
			TEXT("UEAssetDoc.AIProvider.BlackboardV2.StructuralSourceCheck"),
			TEXT("UEAssetDoc.AIProvider.BlackboardV2.Remove")});
}

void AssertLocalKeysReorderV2Descriptor(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FString PrimitiveId = TEXT("ai.blackboard.local_keys.reorder.v2");
	Test.TestEqual(TEXT("local-keys reorder v2 is versioned independently"), Descriptor.Version, 2u);
	Test.TestEqual(TEXT("local-keys reorder v2 is mutating"), Descriptor.Access,
		EAssetDocumentPrimitiveAccess::Mutating);
	Test.TestEqual(TEXT("local-keys reorder v2 admits only Apply"), Descriptor.AllowedPhases,
		TArray<EAssetDocumentRuntimeRequestPhase>{EAssetDocumentRuntimeRequestPhase::Apply});
	Test.TestTrue(TEXT("local-keys reorder v2 requires an active transaction"),
		Descriptor.bRequiresActiveTransaction);
	Test.TestTrue(TEXT("local-keys reorder v2 requires exactly mutable asset:HA"),
		HasMutableAssetArgument(Descriptor) && Descriptor.HandleArgumentSchemas.Num() == 1
			&& Descriptor.HandleResultSchemas.IsEmpty());
	Test.TestEqual(TEXT("local-keys reorder v2 allows only Asset handles"),
		Descriptor.AllowedHandleKinds,
		TArray<EAssetDocumentHandleKind>{EAssetDocumentHandleKind::Asset});
	Test.TestEqual(TEXT("local-keys reorder v2 input schema is exact"), Descriptor.InputSchemaId,
		PrimitiveId + TEXT(".input.v2"));
	Test.TestEqual(TEXT("local-keys reorder v2 output schema is exact"), Descriptor.OutputSchemaId,
		PrimitiveId + TEXT(".output.v2"));
	Test.TestEqual(TEXT("local-keys reorder v2 footprint schema is exact"),
		Descriptor.MutationFootprintSchemaId,
		FString(TEXT("assetdocument.ai.blackboard.local_keys.reorder.v2")));
	Test.TestEqual(TEXT("local-keys reorder v2 has only D0 terminals"),
		Descriptor.StableDiagnosticCodes,
		TArray<FString>{TEXT("AIProvider.InvalidValueShape"),
			TEXT("AIProvider.InvalidContext"), TEXT("AIProvider.InvalidTransaction"),
			TEXT("AIProvider.InvalidHandle"), TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("AIProvider.OperationRejected"),
			TEXT("AIProvider.Blackboard.OrderedKeysPermutationInvalid"),
			TEXT("AIProvider.Blackboard.OrderedKeysLifecycleUnavailable"),
			TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
			TEXT("AIProvider.Blackboard.EntryChangedSetMismatch")});
	Test.TestEqual(TEXT("local-keys reorder v2 is admitted through the publication bridge"),
		Descriptor.Admission.Disposition, EAssetDocumentPrimitiveDisposition::AdmittedBridge);
	Test.TestEqual(TEXT("local-keys reorder v2 bridge authority path is exact"),
		Descriptor.Admission.AuthorityPath, EAssetDocumentPrimitiveAuthorityPath::BridgeAdmitted);
	Test.TestEqual(TEXT("local-keys reorder v2 uses the publication governance record"),
		Descriptor.Admission.GovernanceId,
		FString(TEXT("AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1")));
	Test.TestEqual(TEXT("local-keys reorder v2 has the exact bridge public module"),
		Descriptor.Admission.PublicModule, FString(TEXT("AssetDocumentAI+AIModule+PropertyEditor")));
	Test.TestEqual(TEXT("local-keys reorder v2 has the exact bridge public header"),
		Descriptor.Admission.PublicHeader, FString(TEXT(
			"AuthorityException/AssetDocumentAIBlackboardStructuralPublication.h+"
			"BehaviorTree/BlackboardData.h+PropertyHandle.h")));
	Test.TestEqual(TEXT("local-keys reorder v2 has the exact bridge authority API"),
		Descriptor.Admission.ExportedAuthoritativeApi, FString(TEXT(
			"AssetDocumentAIBlackboardStructuralPublication::PublishLocalKeysSnapshot")));
	Test.TestEqual(TEXT("local-keys reorder v2 uses exact identity evidence"),
		Descriptor.Admission.IdentityLifetimeContractId,
		PrimitiveId + TEXT(".identity.ha-request-transaction-revision.v2"));
	Test.TestEqual(TEXT("local-keys reorder v2 uses exact active-HA transaction contract"),
		Descriptor.Admission.TransactionContractId,
		PrimitiveId + TEXT(".transaction.active-ha.v2"));
	Test.TestEqual(TEXT("local-keys reorder v2 uses exact Atomic rollback contract"),
		Descriptor.Admission.RollbackContractId,
		FString(TEXT("assetdocument.ai.blackboard.local_keys.reorder.rollback.v2")));
	Test.TestEqual(TEXT("local-keys reorder v2 uses exact completion contract"),
		Descriptor.Admission.CompletionContractId,
		PrimitiveId + TEXT(".completion.identity_order_and_key_id_evidence.v2"));
	Test.TestEqual(TEXT("local-keys reorder v2 has exact save-reload evidence identity"),
		Descriptor.Admission.SaveReloadEvidenceId,
		PrimitiveId + TEXT(".provider-no-persistence-fresh-reload.v2"));
	Test.TestEqual(TEXT("local-keys reorder v2 has exact admission failure evidence"),
		Descriptor.Admission.FailureInjectionTestIds,
		TArray<FString>{TEXT("UEAssetDoc.AIProvider.BlackboardV2.Reorder.Failure")});
	Test.TestEqual(TEXT("local-keys reorder v2 has exact admission evidence"),
		Descriptor.Admission.VerificationTestIds,
		TArray<FString>{TEXT("UEAssetDoc.AIProvider.Authority.PublicSurface"),
			TEXT("UEAssetDoc.AIProvider.BlackboardV2.StructuralSourceCheck"),
			TEXT("UEAssetDoc.AIProvider.BlackboardV2.Reorder")});
	Test.TestTrue(TEXT("local-keys reorder v2 fingerprint retains the canonical READ authority"),
		Descriptor.Admission.AuthorityFingerprint.Contains(
			TEXT("AD.AI.BLACKBOARD.KEYTYPE-STRUCT-CANONICAL-READ-v1")));
	Test.TestFalse(TEXT("local-keys reorder v2 fingerprint excludes canonical WRITE authority"),
		Descriptor.Admission.AuthorityFingerprint.Contains(
			TEXT("AD.AI.BLACKBOARD.KEYTYPE-STRUCT-CANONICAL-WRITE-v1")));
	Test.TestEqual(TEXT("local-keys reorder v2 has the exact READ-only lifecycle fingerprint"),
		Descriptor.Admission.AuthorityFingerprint,
		FString(TEXT("ue-5.7.4-launcher-cl-51494982|ai|ai.blackboard.local_keys.reorder.v2|"))
			+ TEXT("AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1|")
			+ TEXT("AssetDocumentAIBlackboardStructuralPublication::")
			+ TEXT("PublishLocalKeysSnapshot>IPropertyHandle::NotifyPreChange>")
			+ TEXT("UBlackboardData::Keys assignment>IPropertyHandle::NotifyPostChange(")
			+ TEXT("EPropertyChangeType::ValueSet)>IPropertyHandle::NotifyFinishedChangingProperties>")
			+ TEXT("UBlackboardData::UpdateParentKeys()>UBlackboardData::UpdateKeyIDs()")
			+ TEXT("|AD.AI.BLACKBOARD.KEYTYPE-STRUCT-CANONICAL-READ-v1|")
			+ TEXT("exact duplicate-free complete ordered local-name permutation preflight>")
			+ TEXT("CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage>")
			+ TEXT("EnlistFootprint(UObjectState[LocalKeysAggregate])>PublishLocalKeysSnapshot>")
			+ TEXT("CompletePublishedLoadedConsumers>entry/KeyType identity preservation>")
			+ TEXT("before/after Key-ID evidence>full canonical local-name/order readback"));
}

bool HasMutableAssetArgument(const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	if (!Descriptor.AllowedHandleKinds.Contains(EAssetDocumentHandleKind::Asset))
	{
		return false;
	}
	for (const FAssetDocumentPrimitiveHandleArgumentSchema& Argument : Descriptor.HandleArgumentSchemas)
	{
		if (Argument.Name == TEXT("asset")
			&& Argument.Kind == EAssetDocumentHandleKind::Asset
			&& Argument.RequiredAccess == EAssetDocumentHandleAccess::Mutable)
		{
			return true;
		}
	}
	return false;
}

bool DescriptorLeaksStructCanonicalWriteAuthority(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const TCHAR* WriteId)
{
	auto ContainsWriteId = [WriteId](const FString& Value)
	{
		return Value.Contains(WriteId, ESearchCase::CaseSensitive);
	};
	const FAssetDocumentPrimitiveAdmissionEvidence& Admission = Descriptor.Admission;
	if (ContainsWriteId(Descriptor.PrimitiveId) || ContainsWriteId(Descriptor.InputSchemaId)
		|| ContainsWriteId(Descriptor.OutputSchemaId) || ContainsWriteId(Descriptor.MutationFootprintSchemaId)
		|| ContainsWriteId(Admission.PublicModule) || ContainsWriteId(Admission.PublicHeader)
		|| ContainsWriteId(Admission.ExportedAuthoritativeApi) || ContainsWriteId(Admission.AuthorityContextId)
		|| ContainsWriteId(Admission.PrimaryOperationId) || ContainsWriteId(Admission.IdentityLifetimeContractId)
		|| ContainsWriteId(Admission.TransactionContractId) || ContainsWriteId(Admission.RollbackContractId)
		|| ContainsWriteId(Admission.CompletionContractId) || ContainsWriteId(Admission.SaveReloadEvidenceId)
		|| ContainsWriteId(Admission.GovernanceId) || ContainsWriteId(Admission.AuthorityFingerprint))
	{
		return true;
	}
	for (const FString& Value : Descriptor.StableDiagnosticCodes)
	{
		if (ContainsWriteId(Value)) return true;
	}
	for (const FString& Value : Descriptor.RequiredConsumerIds)
	{
		if (ContainsWriteId(Value)) return true;
	}
	for (const FString& Value : Admission.FailureInjectionTestIds)
	{
		if (ContainsWriteId(Value)) return true;
	}
	for (const FString& Value : Admission.VerificationTestIds)
	{
		if (ContainsWriteId(Value)) return true;
	}
	return false;
}

const FAssetDocumentAIDecisionProjectionRow* FindDecisionRow(
	TConstArrayView<FAssetDocumentAIDecisionProjectionRow> Rows,
	const TCHAR* PrimitiveId)
{
	for (const FAssetDocumentAIDecisionProjectionRow& Row : Rows)
	{
		if (Row.DecisionId == PrimitiveId)
		{
			return &Row;
		}
	}
	return nullptr;
}

void AssertCallableDescriptorAuthority(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FAssetDocumentPrimitiveAdmissionEvidence& Evidence = Descriptor.Admission;
	Test.TestTrue(*FString::Printf(TEXT("%s has a structurally valid descriptor"), *Descriptor.PrimitiveId),
		ValidateAssetDocumentPrimitiveDescriptor(Descriptor).bValid);
	Test.TestFalse(*FString::Printf(TEXT("%s has a public module"), *Descriptor.PrimitiveId), Evidence.PublicModule.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s has a public header"), *Descriptor.PrimitiveId), Evidence.PublicHeader.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s has an exported authority API"), *Descriptor.PrimitiveId), Evidence.ExportedAuthoritativeApi.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s has authority context"), *Descriptor.PrimitiveId), Evidence.AuthorityContextId.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s has identity/lifetime evidence"), *Descriptor.PrimitiveId), Evidence.IdentityLifetimeContractId.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s has transaction evidence"), *Descriptor.PrimitiveId), Evidence.TransactionContractId.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s has rollback evidence"), *Descriptor.PrimitiveId), Evidence.RollbackContractId.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s has completion evidence"), *Descriptor.PrimitiveId), Evidence.CompletionContractId.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s has save/reload evidence"), *Descriptor.PrimitiveId), Evidence.SaveReloadEvidenceId.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s has failure injection evidence"), *Descriptor.PrimitiveId), Evidence.FailureInjectionTestIds.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s has verification evidence"), *Descriptor.PrimitiveId), Evidence.VerificationTestIds.IsEmpty());
}

void AssertBlockerDescriptor(
	FAutomationTestBase& Test,
	const FExpectedDecision& Expected,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	Test.TestEqual(*FString::Printf(TEXT("%s is a stable blocker"), Expected.PrimitiveId),
		Descriptor.Admission.Disposition, EAssetDocumentPrimitiveDisposition::StableBlocker);
	Test.TestEqual(*FString::Printf(TEXT("%s has its only blocker diagnostic"), Expected.PrimitiveId),
		Descriptor.StableDiagnosticCodes, TArray<FString>{Expected.StableBlockerDiagnostic});
	Test.TestTrue(*FString::Printf(TEXT("%s omits runtime consumers"), Expected.PrimitiveId),
		Descriptor.RequiredConsumerIds.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("%s has no callable input schema"), Expected.PrimitiveId), Descriptor.InputSchemaId.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("%s has no callable output schema"), Expected.PrimitiveId), Descriptor.OutputSchemaId.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("%s has no footprint"), Expected.PrimitiveId), Descriptor.MutationFootprintSchemaId.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("%s has no handle schemas"), Expected.PrimitiveId),
		Descriptor.HandleArgumentSchemas.IsEmpty() && Descriptor.HandleResultSchemas.IsEmpty());
	Test.TestFalse(*FString::Printf(TEXT("%s has no transaction requirement"), Expected.PrimitiveId),
		Descriptor.bRequiresActiveTransaction);
}
} // namespace AssetDocumentAIProviderContractTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentAIProviderContractCatalogTest,
	"UEAssetDoc.AIProvider.ContractCatalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIProviderContractCatalogTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIProviderContractTests;

	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentAIPrimitiveProvider();
	const TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors =
		Provider->DescribePrimitives();
	const TConstArrayView<FAssetDocumentAIDecisionProjectionRow> DecisionRows =
		GetAssetDocumentAIDecisionProjectionRows();
	TestEqual(TEXT("AI runtime catalog contains forty decisions plus selector evidence, the three StateTree and the three EnvQuery primitives"),
		Descriptors.Num(), 47);
	TestEqual(TEXT("AI private decision projection contains exactly forty rows"),
		DecisionRows.Num(), 40);
	constexpr TCHAR StructCanonicalWriteId[] =
		TEXT("AD.AI.BLACKBOARD.KEYTYPE-STRUCT-CANONICAL-WRITE-v1");
	TestNull(TEXT("STATIC-SC-WRITE-AUTHORITY-001 WRITE ID has no callable Runtime route"),
		FindAssetDocumentAIPrimitiveBinding(StructCanonicalWriteId, 1));
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
	{
		TestFalse(*FString::Printf(
			TEXT("STATIC-SC-WRITE-AUTHORITY-001 WRITE ID has no Runtime descriptor projection: %s"),
			*Descriptor.PrimitiveId),
			DescriptorLeaksStructCanonicalWriteAuthority(Descriptor, StructCanonicalWriteId));
	}
	for (const FAssetDocumentAIDecisionProjectionRow& DecisionRow : DecisionRows)
	{
		TestFalse(*FString::Printf(
			TEXT("STATIC-SC-WRITE-AUTHORITY-001 WRITE ID has no catalog projection: %s"),
			*DecisionRow.DecisionId),
			DecisionRow.DecisionId.Contains(StructCanonicalWriteId)
				|| DecisionRow.DecisionConsumerId.Contains(StructCanonicalWriteId)
				|| DecisionRow.StableDiagnosticCode.Contains(StructCanonicalWriteId));
	}

	int32 OrdinaryCount = 0;
	int32 BlockerCount = 0;
	for (const FExpectedDecision& Expected : ExpectedDecisions)
	{
		const bool bAdmittedBridge =
			FCString::Strcmp(Expected.PrimitiveId, TEXT("ai.blackboard.entry.add.v2")) == 0
			|| FCString::Strcmp(Expected.PrimitiveId,
				TEXT("ai.blackboard.entry.replace_class.v2")) == 0
			|| FCString::Strcmp(Expected.PrimitiveId,
				TEXT("ai.blackboard.entry.remove.v2")) == 0
			|| FCString::Strcmp(Expected.PrimitiveId,
				TEXT("ai.blackboard.local_keys.reorder.v2")) == 0
			|| FCString::Strcmp(Expected.PrimitiveId,
				TEXT("ai.behavior_tree.value_or_key.refresh.v1")) == 0
			|| FCString::Strcmp(Expected.PrimitiveId,
				TEXT("ai.behavior_tree.value_or_key.refresh.v2")) == 0
			|| FCString::Strcmp(Expected.PrimitiveId, TEXT("ai.behavior_tree.value_or_key.refresh.v3")) == 0
			|| FCString::Strcmp(Expected.PrimitiveId,
				TEXT("ai.behavior_tree.graph.install.v1")) == 0;
		const FAssetDocumentAIDecisionProjectionRow* DecisionRow =
			FindDecisionRow(DecisionRows, Expected.PrimitiveId);
		TestNotNull(*FString::Printf(TEXT("private projection includes decision row %s"),
			Expected.PrimitiveId), DecisionRow);
		if (DecisionRow == nullptr)
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("private projection preserves disposition for %s"),
			Expected.PrimitiveId), DecisionRow->Disposition,
			Expected.StableBlockerDiagnostic[0] != TEXT('\0')
				? EAssetDocumentPrimitiveDisposition::StableBlocker
				: (bAdmittedBridge
					? EAssetDocumentPrimitiveDisposition::AdmittedBridge
					: EAssetDocumentPrimitiveDisposition::OrdinaryHelper));
		TestEqual(*FString::Printf(TEXT("private projection keeps %s consumer for %s"),
			Expected.DecisionConsumerId, Expected.PrimitiveId),
			DecisionRow->DecisionConsumerId, FString(Expected.DecisionConsumerId));
		TestEqual(*FString::Printf(TEXT("private projection keeps diagnostic for %s"),
			Expected.PrimitiveId), DecisionRow->StableDiagnosticCode,
			FString(Expected.StableBlockerDiagnostic));
		const FAssetDocumentPrimitiveDescriptor* Descriptor =
			FindDescriptor(Descriptors, Expected.PrimitiveId, Expected.Version);
		TestNotNull(*FString::Printf(TEXT("catalog includes decision row %s"), Expected.PrimitiveId), Descriptor);
		if (Descriptor == nullptr)
		{
			continue;
		}

		TestEqual(*FString::Printf(TEXT("%s belongs to AI"), Expected.PrimitiveId),
			Descriptor->ProviderId, FString(TEXT("ai")));
		TestEqual(*FString::Printf(TEXT("%s preserves its frozen version"), Expected.PrimitiveId),
			Descriptor->Version, Expected.Version);
		TestEqual(*FString::Printf(TEXT("%s has AI physical owner"), Expected.PrimitiveId),
			Descriptor->Owner, EAssetDocumentPrimitiveOwner::AI);
		if (Descriptor->PrimitiveId == TEXT("ai.behavior_tree.graph.create.v1"))
		{
			TestEqual(TEXT("scratch creation admits only Apply after target creation"), Descriptor->AllowedPhases,
				TArray<EAssetDocumentRuntimeRequestPhase>{EAssetDocumentRuntimeRequestPhase::Apply});
			TestEqual(TEXT("graph creation retains Scratch access"), Descriptor->Access, EAssetDocumentPrimitiveAccess::Scratch);
			TestTrue(TEXT("graph creation retains the actual transaction requirement"), Descriptor->bRequiresActiveTransaction);
		}
		if (Expected.StableBlockerDiagnostic[0] != TEXT('\0'))
		{
			++BlockerCount;
			AssertBlockerDescriptor(*this, Expected, *Descriptor);
			TestNull(*FString::Printf(TEXT("%s has zero callable bindings"), Expected.PrimitiveId),
				FindAssetDocumentAIPrimitiveBinding(Expected.PrimitiveId, 1));
		}
		else
		{
			++OrdinaryCount;
			TestEqual(*FString::Printf(TEXT("%s projects its frozen runtime consumer"), Expected.PrimitiveId),
				Descriptor->RequiredConsumerIds, TArray<FString>{Expected.RuntimeConsumerId});
			AssertCallableDescriptorAuthority(*this, *Descriptor);
			TestNotNull(*FString::Printf(TEXT("%s has exactly one callable binding"), Expected.PrimitiveId),
				FindAssetDocumentAIPrimitiveBinding(Expected.PrimitiveId, Expected.Version));
			if (Descriptor->PrimitiveId == TEXT("ai.blackboard.entry.readback.v2"))
			{
				AssertReadbackV2Descriptor(*this, *Descriptor);
			}
			else if (Descriptor->PrimitiveId == TEXT("ai.blackboard.parent.readback.v2"))
			{
				AssertParentReadbackV2Descriptor(*this, *Descriptor);
			}
			else if (Descriptor->PrimitiveId == TEXT("ai.blackboard.local_keys.readback.v2"))
			{
				AssertLocalKeysReadbackV2Descriptor(*this, *Descriptor);
			}
			else if (Descriptor->PrimitiveId == TEXT("ai.blackboard.entry.update_existing.v2"))
			{
				AssertUpdateExistingV2Descriptor(*this, *Descriptor);
			}
			else if (Descriptor->PrimitiveId == TEXT("ai.blackboard.parent.set.v2"))
			{
				AssertParentSetV2Descriptor(*this, *Descriptor);
			}
			else if (Descriptor->PrimitiveId == TEXT("ai.blackboard.entry.add.v2"))
			{
				AssertEntryAddV2Descriptor(*this, *Descriptor);
			}
			else if (Descriptor->PrimitiveId == TEXT("ai.blackboard.entry.replace_class.v2"))
			{
				AssertEntryReplaceClassV2Descriptor(*this, *Descriptor);
			}
			else if (Descriptor->PrimitiveId == TEXT("ai.blackboard.entry.remove.v2"))
			{
				AssertEntryRemoveV2Descriptor(*this, *Descriptor);
			}
			else if (Descriptor->PrimitiveId == TEXT("ai.blackboard.local_keys.reorder.v2"))
			{
				AssertLocalKeysReorderV2Descriptor(*this, *Descriptor);
			}
		}
	}
	const auto* Install = FindDescriptor(Descriptors, TEXT("ai.behavior_tree.graph.install.v1"), 1);
	if (TestNotNull(TEXT("A1S install descriptor exists"), Install))
	{
		TestEqual(TEXT("install has the frozen authority"), Install->Admission.GovernanceId,
			FString(TEXT("AD.AI.BT.GRAPH_INSTALL.v1")));
		TestEqual(TEXT("install owns its independent rollback row"), Install->MutationFootprintSchemaId,
			FString(TEXT("assetdocument.ai.behavior_tree.graph_install.v1")));
		TestEqual(TEXT("install closed diagnostics"), Install->StableDiagnosticCodes, TArray<FString>{
			TEXT("AIProvider.InvalidValueShape"), TEXT("AIProvider.InvalidContext"),
			TEXT("AIProvider.InvalidTransaction"), TEXT("AIProvider.InvalidHandle"),
			TEXT("AIProvider.AuthorityUnavailable"), TEXT("AIProvider.OperationRejected"),
			TEXT("AIProvider.BehaviorTree.GraphInstallPreimageMismatch"), TEXT("AIProvider.BehaviorTree.GraphInstallOwnershipRejected"),
			TEXT("AIProvider.BehaviorTree.GraphInstallRegistrationRejected"), TEXT("AIProvider.BehaviorTree.GraphInstallFootprintRejected"),
			TEXT("AIProvider.BehaviorTree.GraphInstallTransferFailed"), TEXT("AIProvider.BehaviorTree.GraphInstallHandleIssueFailed"),
			TEXT("AIProvider.BehaviorTree.GraphInstallCompletionMismatch"), TEXT("AIProvider.BehaviorTree.GraphInstallRollbackFailed")});
		const auto* Binding = FindAssetDocumentAIPrimitiveBinding(Install->PrimitiveId,1);
		if (TestNotNull(TEXT("install binding exists"),Binding))
			TestTrue(TEXT("install uses the admitted handler"),Binding->Handler==&HandleAssetDocumentAIBehaviorTreeGraphInstall);
	}
	const auto* Decorator = FindDescriptor(Descriptors, TEXT("ai.behavior_tree.decorator.refresh.v1"), 1);
	if (TestNotNull(TEXT("D3 decorator descriptor exists"), Decorator))
	{
		TestEqual(TEXT("decorator is Repair-only"), Decorator->AllowedPhases,
			TArray<EAssetDocumentRuntimeRequestPhase>{EAssetDocumentRuntimeRequestPhase::Repair});
		TestEqual(TEXT("decorator is mutating"), Decorator->Access, EAssetDocumentPrimitiveAccess::Mutating);
		TestTrue(TEXT("decorator requires Atomic"), Decorator->bRequiresActiveTransaction);
		TestEqual(TEXT("decorator has one Asset argument"), Decorator->HandleArgumentSchemas.Num(), 1);
		if (Decorator->HandleArgumentSchemas.Num() == 1)
		{
			const auto& Asset = Decorator->HandleArgumentSchemas[0];
			TestEqual(TEXT("decorator argument name"), Asset.Name, FString(TEXT("asset")));
			TestEqual(TEXT("decorator argument kind"), Asset.Kind, EAssetDocumentHandleKind::Asset);
			TestEqual(TEXT("decorator argument access"), Asset.RequiredAccess, EAssetDocumentHandleAccess::Mutable);
		}
		TestTrue(TEXT("decorator returns no handles"), Decorator->HandleResultSchemas.IsEmpty());
		TestEqual(TEXT("decorator exact terminal component"), Decorator->MutationFootprintSchemaId,
			FString(TEXT("assetdocument.ai.behavior_tree.derived_completion.v1")));
		TestEqual(TEXT("decorator closed diagnostics"), Decorator->StableDiagnosticCodes, TArray<FString>{
			TEXT("AIProvider.InvalidValueShape"), TEXT("AIProvider.InvalidContext"),
			TEXT("AIProvider.InvalidTransaction"), TEXT("AIProvider.InvalidHandle"),
			TEXT("AIProvider.AuthorityUnavailable"), TEXT("AIProvider.OperationRejected"),
			TEXT("AIProvider.BehaviorTree.DecoratorInventoryRejected"),
			TEXT("AIProvider.BehaviorTree.DecoratorSelectorUnresolved"),
			TEXT("AIProvider.BehaviorTree.DecoratorOperationPropertyRejected"),
			TEXT("AIProvider.BehaviorTree.DecoratorNotificationFailed"),
			TEXT("AIProvider.BehaviorTree.DecoratorCompletionMismatch")});
	}
	for (int32 Version : {1, 2})
	{
	const FString Id = FString::Printf(TEXT("ai.behavior_tree.value_or_key.refresh.v%d"), Version);
	const auto* ValueOr = FindDescriptor(Descriptors, *Id, Version);
	if (TestNotNull(TEXT("A3S ValueOr descriptor exists"), ValueOr))
	{
		TestEqual(TEXT("explicit input schema version"), ValueOr->InputSchemaId, Id + FString::Printf(TEXT(".input.v%d"), Version));
		TestEqual(TEXT("explicit output schema version"), ValueOr->OutputSchemaId, Id + FString::Printf(TEXT(".output.v%d"), Version));
		TestEqual(TEXT("value_or_key is Repair-only"), ValueOr->AllowedPhases,
			TArray<EAssetDocumentRuntimeRequestPhase>{EAssetDocumentRuntimeRequestPhase::Repair});
		TestEqual(TEXT("value_or_key is mutating"), ValueOr->Access, EAssetDocumentPrimitiveAccess::Mutating);
		TestTrue(TEXT("value_or_key requires Atomic"), ValueOr->bRequiresActiveTransaction);
		TestEqual(TEXT("value_or_key has one Asset argument"), ValueOr->HandleArgumentSchemas.Num(), 1);
		if (ValueOr->HandleArgumentSchemas.Num() == 1)
		{
			const auto& Asset = ValueOr->HandleArgumentSchemas[0];
			TestEqual(TEXT("value_or_key argument name"), Asset.Name, FString(TEXT("asset")));
			TestEqual(TEXT("value_or_key argument kind"), Asset.Kind, EAssetDocumentHandleKind::Asset);
			TestEqual(TEXT("value_or_key argument access"), Asset.RequiredAccess, EAssetDocumentHandleAccess::Mutable);
		}
		TestTrue(TEXT("value_or_key returns no handles"), ValueOr->HandleResultSchemas.IsEmpty());
		TestEqual(TEXT("ValueOr uses the approved cache exception"), ValueOr->Admission.GovernanceId,
			FString(TEXT("AD.AI.BLACKBOARD.VALUE-OR-KEY-CACHE-INVALIDATION-v1")));
		TestEqual(TEXT("ValueOr exact primitive allowlist"), ValueOr->Admission.RawLifecycleAllowlistId,
			FString(TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_CALLSITE(AD.AI.BLACKBOARD.VALUE-OR-KEY-CACHE-INVALIDATION-v1|")) + Id + TEXT(")"));
		const auto DescriptorValidation = ValidateAssetDocumentPrimitiveDescriptor(*ValueOr);
		for (const auto& Diagnostic : DescriptorValidation.Diagnostics)
			AddInfo(Diagnostic.Code + TEXT(": ") + Diagnostic.Message);
		TestTrue(TEXT("ValueOr passes the real Runtime descriptor admission"), DescriptorValidation.bValid);
		auto InvalidAllowlist = *ValueOr;
		InvalidAllowlist.Admission.RawLifecycleAllowlistId = TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_CALLSITE(AD.AI.BLACKBOARD.VALUE-OR-KEY-CACHE-INVALIDATION-v1|Source/AssetDocumentAI/Private/BehaviorTree/AssetDocumentAIBehaviorTreeDerivedCompletion.cpp::CompleteExactCurrentGraphValueOrInventory)");
		const auto InvalidValidation = ValidateAssetDocumentPrimitiveDescriptor(InvalidAllowlist);
		TestFalse(TEXT("physical callsite cannot replace the Runtime operation-id marker"), InvalidValidation.bValid);
		TestEqual(TEXT("invalid bridge marker has one precise descriptor diagnostic"), InvalidValidation.Diagnostics.Num(), 1);
		if (InvalidValidation.Diagnostics.Num() == 1)
			TestEqual(TEXT("invalid bridge marker is InvalidPath, not invocation identity"),
				InvalidValidation.Diagnostics[0].Code, FString(TEXT("PrimitiveDescriptor.InvalidPath")));
		const auto* Binding = FindAssetDocumentAIPrimitiveBinding(ValueOr->PrimitiveId, Version);
		if (TestNotNull(TEXT("ValueOr exact binding exists"), Binding))
			TestTrue(TEXT("ValueOr binds its top-mode handler"), Binding->Handler == &HandleAssetDocumentAIBehaviorTreeValueOrKeyRefresh);
		TestEqual(TEXT("value_or_key exact terminal component"), ValueOr->MutationFootprintSchemaId,
			FString(TEXT("assetdocument.ai.behavior_tree.derived_completion.v1")));
		TestEqual(TEXT("ValueOr exact closed diagnostics"), ValueOr->StableDiagnosticCodes, TArray<FString>{
			TEXT("AIProvider.InvalidValueShape"),
			TEXT("AIProvider.InvalidContext"),
			TEXT("AIProvider.InvalidTransaction"),
			TEXT("AIProvider.InvalidHandle"),
			TEXT("AIProvider.AuthorityUnavailable"),
			TEXT("AIProvider.OperationRejected"),
			TEXT("AIProvider.BehaviorTree.ValueOrInventoryRejected"),
			TEXT("AIProvider.BehaviorTree.ValueOrRegistrationRejected"),
			TEXT("AIProvider.BehaviorTree.ValueOrBlackboardMismatch"),
			TEXT("AIProvider.BehaviorTree.ValueOrKeyTypeMismatch"),
			TEXT("AIProvider.BehaviorTree.ValueOrInvalidationFailed"),
			TEXT("AIProvider.BehaviorTree.ValueOrCompletionMismatch"),
			TEXT("AIProvider.BehaviorTree.DecoratorInventoryRejected"),
			TEXT("AIProvider.BehaviorTree.DecoratorSelectorUnresolved"),
			TEXT("AIProvider.BehaviorTree.DecoratorOperationPropertyRejected"),
			TEXT("AIProvider.BehaviorTree.DecoratorNotificationFailed"),
			TEXT("AIProvider.BehaviorTree.DecoratorCompletionMismatch")});
	}
	}
	TestEqual(TEXT("the private decision contract has thirty-nine callable rows"), OrdinaryCount, 39);
	TestEqual(TEXT("the private decision contract has one stable blocker row"), BlockerCount, 1);
	TestTrue(TEXT("assembly validates the frozen private decision projection"),
		ValidateAssetDocumentAIDecisionProjection(DecisionRows));

	TArray<FAssetDocumentAIDecisionProjectionRow> OrphanRows;
	OrphanRows.Append(DecisionRows.GetData(), DecisionRows.Num());
	OrphanRows[0].DecisionId = TEXT("ai.orphan.v1");
	TestFalse(TEXT("assembly rejects an orphan private decision row"),
		ValidateAssetDocumentAIDecisionProjection(OrphanRows));

	TArray<FAssetDocumentAIDecisionProjectionRow> ConsumerDriftRows;
	ConsumerDriftRows.Append(DecisionRows.GetData(), DecisionRows.Num());
	ConsumerDriftRows[0].DecisionConsumerId = TEXT("drifted_consumer");
	TestFalse(TEXT("assembly rejects ordinary decision-consumer drift"),
		ValidateAssetDocumentAIDecisionProjection(ConsumerDriftRows));

	TArray<FAssetDocumentAIDecisionProjectionRow> BlockerDriftRows;
	BlockerDriftRows.Append(DecisionRows.GetData(), DecisionRows.Num());
	BlockerDriftRows[1].DecisionConsumerId = TEXT("drifted_consumer");
	TestFalse(TEXT("assembly rejects blocker decision-consumer drift without changing runtime empty consumers"),
		ValidateAssetDocumentAIDecisionProjection(BlockerDriftRows));

	const FAssetDocumentPrimitiveDescriptor* SelectorEvidence =
		FindDescriptor(Descriptors, ExistingSelectorEvidenceId);
	TestNotNull(TEXT("existing selector evidence remains the final runtime descriptor"), SelectorEvidence);
	if (SelectorEvidence != nullptr)
	{
		TestEqual(TEXT("selector evidence keeps the route consumer"), SelectorEvidence->RequiredConsumerIds,
			TArray<FString>{TEXT("route.generic.v1")});
		AssertCallableDescriptorAuthority(*this, *SelectorEvidence);
		TestNotNull(TEXT("selector evidence keeps its callable binding"),
			FindAssetDocumentAIPrimitiveBinding(ExistingSelectorEvidenceId, 1));
	}
	TestTrue(TEXT("AI descriptor/binding assembly is a total ordinary bijection"),
		IsAssetDocumentAIPrimitiveAssemblyValid());
	TestEqual(TEXT("normal Editor authority makes the complete provider available"),
		Provider->InspectAvailability().State, EAssetDocumentProviderAvailabilityState::Available);

	constexpr const TCHAR* RemoveId = TEXT("ai.blackboard.entry.remove.v2");
	TestNotNull(TEXT("released remove row has one decision projection"),
		FindDecisionRow(DecisionRows, RemoveId));
	TestNotNull(TEXT("released remove row has one descriptor"),
		FindDescriptor(Descriptors, RemoveId, 2));
	TestNotNull(TEXT("released remove row has one binding"),
		FindAssetDocumentAIPrimitiveBinding(RemoveId, 2));

	constexpr const TCHAR* ReorderId = TEXT("ai.blackboard.local_keys.reorder.v2");
	TestNotNull(TEXT("released reorder row has one decision projection"),
		FindDecisionRow(DecisionRows, ReorderId));
	TestNotNull(TEXT("released reorder row has one descriptor"),
		FindDescriptor(Descriptors, ReorderId, 2));
	TestNotNull(TEXT("released reorder row has one binding"),
		FindAssetDocumentAIPrimitiveBinding(ReorderId, 2));

	const FAssetDocumentPrimitiveDescriptor* UpdateDescriptor =
		FindDescriptor(Descriptors, TEXT("ai.blackboard.entry.update_existing.v2"), 2);
	TestNotNull(TEXT("UPDATE publication has its exact v2 descriptor"), UpdateDescriptor);
	if (UpdateDescriptor != nullptr)
	{
		TestTrue(TEXT("UPDATE publication requires mutable asset:HA"),
			HasMutableAssetArgument(*UpdateDescriptor));
	}

	for (const FAssetMutationExpectation& Mutation : AssetMutations)
	{
		const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(
			Descriptors, Mutation.PrimitiveId, Mutation.Version);
		TestNotNull(*FString::Printf(TEXT("C-ASSET descriptor exists: %s"),
			Mutation.PrimitiveId), Descriptor);
		if (Descriptor == nullptr)
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("%s requires an active transaction"), Mutation.PrimitiveId),
			Descriptor->bRequiresActiveTransaction);
		TestTrue(*FString::Printf(TEXT("%s requires mutable asset:HA"), Mutation.PrimitiveId),
			HasMutableAssetArgument(*Descriptor));
		TestFalse(*FString::Printf(TEXT("%s declares one footprint before mutation"), Mutation.PrimitiveId),
			Descriptor->MutationFootprintSchemaId.IsEmpty());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIValueOrRequestContractTest,
 "UEAssetDoc.AIProvider.BehaviorTree.ValueOrKeyRefresh.RequestContract",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIValueOrRequestContractTest::RunTest(const FString& Parameters)
{
 class FUnusedResolver final : public IAssetDocumentHandleResolver
 {
 public:
  int32 Calls = 0;
  FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind, EAssetDocumentHandleAccess,
   TUniquePtr<IAssetDocumentNativeHandlePayload>&&) override { ++Calls; return {}; }
  FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(const FAssetDocumentOpaqueHandle&,
   const FAssetDocumentHandleValidationContext&) override { ++Calls; return {}; }
  FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation&) override
   { ++Calls; return {}; }
 } Resolver;
 FAssetDocumentPrimitiveInvocation Request;
 Request.PrimitiveId = TEXT("ai.behavior_tree.value_or_key.refresh.v1"); Request.Version = 1;
 Request.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
 auto AssertFailure = [&](const TCHAR* Expected)
 {
  for (const bool ThroughProvider : {false, true})
  {
  const auto Result = ThroughProvider
   ? CreateAssetDocumentAIPrimitiveProvider()->Invoke(Request, Resolver, nullptr)
   : HandleAssetDocumentAIBehaviorTreeValueOrKeyRefresh(Request, Resolver, nullptr);
  TestEqual(TEXT("request rejection fails"), Result.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
  TestEqual(TEXT("request rejection has one row terminal"), Result.Diagnostics.Num(), 1);
  if (Result.Diagnostics.Num() == 1) TestEqual(TEXT("exact rejection priority"), Result.Diagnostics[0].Code, FString(Expected));
  TestTrue(TEXT("failure has no authored changes or returned handles"), Result.ChangedSet.IsEmpty() && Result.HandleResults.IsEmpty());
  TestTrue(TEXT("failure has the Runtime-required empty Object Value"),
   Result.Value.Kind == EAssetDocumentRuntimeValueKind::Object && Result.Value.ObjectKeys.IsEmpty() && Result.Value.Elements.IsEmpty());
  TestEqual(TEXT("early rejection never uses a handle"), Resolver.Calls, 0);
  }
 };
 AssertFailure(TEXT("AIProvider.InvalidContext"));
 if (!GIsEditor || IsRunningCommandlet() || !IsInGameThread()) return true;
 Request.Phase = EAssetDocumentRuntimeRequestPhase::Repair;
 AssertFailure(TEXT("AIProvider.InvalidValueShape"));
 Request.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
 Request.ValueArguments.ObjectKeys = {TEXT("behavior_tree_path"), TEXT("completion_registration"),
  TEXT("expected_authored_inventory_digest"), TEXT("expected_blackboard_path")};
 for (const TCHAR* Value : {TEXT("/Game/Contract.Tree"), TEXT("current_graph"),
  TEXT("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"), TEXT("/Game/Contract.Blackboard")})
 {
  auto& Item = Request.ValueArguments.Elements.AddDefaulted_GetRef();
  Item.Kind = EAssetDocumentRuntimeValueKind::String; Item.StringValue = Value;
 }
 AssertFailure(TEXT("AIProvider.InvalidTransaction"));
 Request.ValueArguments.Elements[1].StringValue = TEXT("installed_replacement");
 AssertFailure(TEXT("AIProvider.InvalidTransaction"));
 Request.ValueArguments.Elements[1].StringValue = TEXT("other");
 AssertFailure(TEXT("AIProvider.InvalidValueShape"));
 Request.ValueArguments.Elements[1].StringValue = TEXT("current_graph");
 Request.ValueArguments.Elements[2].StringValue[0] = TEXT('A');
 AssertFailure(TEXT("AIProvider.InvalidValueShape"));
 Request.ValueArguments.Elements[2].StringValue = TEXT("short");
 AssertFailure(TEXT("AIProvider.InvalidValueShape"));
 return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
