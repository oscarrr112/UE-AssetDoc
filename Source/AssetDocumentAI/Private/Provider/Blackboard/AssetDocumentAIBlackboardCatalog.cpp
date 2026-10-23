// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Blackboard/AssetDocumentAIBlackboardPrimitives.h"

namespace AssetDocumentAIBlackboardCatalog
{
struct FAuthorityIdentity
{
	const TCHAR* PublicModule;
	const TCHAR* PublicHeader;
	const TCHAR* ExportedAuthoritativeApi;
};

FAuthorityIdentity GetAuthorityIdentity(FStringView PrimitiveId)
{
	if (PrimitiveId == TEXTVIEW("ai.blackboard.key_type.inspect.v1"))
	{
		return {
			TEXT("PropertyEditor"),
			TEXT("PropertyEditorModule.h+IPropertyRowGenerator.h+PropertyHandle.h"),
			TEXT("FPropertyEditorModule::CreatePropertyRowGenerator>IPropertyRowGenerator::SetObjects>IPropertyHandle"),
		};
	}
	if (PrimitiveId == TEXTVIEW("ai.blackboard.key_type.apply_properties.v1"))
	{
		return {
			TEXT("AssetDocumentAI+PropertyEditor"),
			TEXT("TypedPreimage/AssetDocumentAIBlackboardTypedPreimage.h+PropertyEditorModule.h+IPropertyRowGenerator.h+PropertyHandle.h"),
			TEXT("CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage>IPropertyHandle::SetValue"),
		};
	}
	if (PrimitiveId == TEXTVIEW("ai.blackboard.parent.set.v1"))
	{
		return {
			TEXT("AssetDocumentAI+AssetDocumentEditor+AIModule+PropertyEditor"),
			TEXT("TypedPreimage/AssetDocumentAIBlackboardTypedPreimage.h+AssetDocumentEditorNativeState.h+BehaviorTree/BlackboardData.h+PropertyEditorModule.h+IPropertyRowGenerator.h+PropertyHandle.h"),
			TEXT("CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage>WithAssetDocumentEditorAssetIdentity>CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage>IPropertyHandle::SetValue>UBlackboardData::PostEditChangeProperty>UBlackboardData::UpdateParentKeys"),
		};
	}
	return {
		TEXT("AssetDocumentAI+AIModule+Engine+PropertyEditor"),
		TEXT("TypedPreimage/AssetDocumentAIBlackboardTypedPreimage.h+BehaviorTree/BlackboardData.h+BehaviorTree/Blackboard/BlackboardKeyType_Object.h+GameFramework/Actor.h+PropertyEditorModule.h+IPropertyRowGenerator.h+IDetailTreeNode.h+PropertyHandle.h"),
		TEXT("CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage>FPropertyEditorModule::CreatePropertyRowGenerator>IPropertyRowGenerator::SetPropertyGenerationAllowListPaths>IPropertyRowGenerator::SetObjects>IDetailTreeNode::CreatePropertyHandle>IPropertyHandle::NotifyPreChange>IPropertyHandle::NotifyPostChange>IPropertyHandle::NotifyFinishedChangingProperties>UBlackboardData::UpdateParentKeys>UBlackboardData::UpdateKeyIDs"),
	};
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
		TEXT("UEAssetDoc.AIProvider.BlackboardLifecycle"),
	};
	Evidence.VerificationTestIds = {
		TEXT("UEAssetDoc.AIProvider.Authority.PublicSurface"),
		TEXT("UEAssetDoc.AIProvider.BlackboardLifecycle"),
	};
	Evidence.AuthorityFingerprint =
		TEXT("ue-5.7.4-launcher-cl-51494982|ai|") + PrimitiveId
		+ TEXT("|") + Evidence.PublicModule
		+ TEXT("|") + Evidence.ExportedAuthoritativeApi;
	return Evidence;
}

FAssetDocumentPrimitiveHandleArgumentSchema MakeMutableAssetArgument()
{
	FAssetDocumentPrimitiveHandleArgumentSchema Argument;
	Argument.Name = TEXT("asset");
	Argument.Kind = EAssetDocumentHandleKind::Asset;
	Argument.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	return Argument;
}

FAssetDocumentPrimitiveDescriptor MakeOrdinaryDescriptor(
	const TCHAR* PrimitiveId,
	EAssetDocumentPrimitiveAccess Access,
	TArray<EAssetDocumentRuntimeRequestPhase> AllowedPhases,
	const TCHAR* MutationFootprintSchemaId,
	TArray<FAssetDocumentPrimitiveHandleArgumentSchema> HandleArguments,
	TArray<FString> OperationDiagnostics)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = PrimitiveId;
	Descriptor.Version = 1;
	Descriptor.ProviderId = TEXT("ai");
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::AI;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = Access;
	Descriptor.AllowedPhases = MoveTemp(AllowedPhases);
	Descriptor.bRequiresActiveTransaction =
		Access == EAssetDocumentPrimitiveAccess::Mutating;
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v1");
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v1");
	Descriptor.MutationFootprintSchemaId = MutationFootprintSchemaId;
	Descriptor.HandleArgumentSchemas = MoveTemp(HandleArguments);
	for (const FAssetDocumentPrimitiveHandleArgumentSchema& Argument :
		Descriptor.HandleArgumentSchemas)
	{
		Descriptor.AllowedHandleKinds.AddUnique(Argument.Kind);
	}
	Descriptor.StableDiagnosticCodes = {
		TEXT("AIProvider.InvalidValueShape"),
		TEXT("AIProvider.InvalidContext"),
		TEXT("AIProvider.InvalidTransaction"),
		TEXT("AIProvider.InvalidHandle"),
		TEXT("AIProvider.AuthorityUnavailable"),
		TEXT("AIProvider.OperationRejected"),
	};
	Descriptor.StableDiagnosticCodes.Append(OperationDiagnostics);
	Descriptor.RequiredConsumerIds = {TEXT("ai_profile")};
	Descriptor.Admission = MakeAdmission(Descriptor.PrimitiveId);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeEntryReadbackDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = TEXT("ai.blackboard.entry.readback.v2");
	Descriptor.Version = 2;
	Descriptor.ProviderId = TEXT("ai");
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::AI;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = EAssetDocumentPrimitiveAccess::ReadOnly;
	Descriptor.AllowedPhases = {
		EAssetDocumentRuntimeRequestPhase::Validate,
		EAssetDocumentRuntimeRequestPhase::Preflight,
		EAssetDocumentRuntimeRequestPhase::Verify};
	Descriptor.bRequiresActiveTransaction = false;
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v2");
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v2");
	Descriptor.StableDiagnosticCodes = {
		TEXT("AIProvider.InvalidValueShape"),
		TEXT("AIProvider.InvalidContext"),
		TEXT("AIProvider.InvalidTransaction"),
		TEXT("AIProvider.InvalidHandle"),
		TEXT("AIProvider.AuthorityUnavailable"),
		TEXT("AIProvider.OperationRejected"),
		TEXT("AIProvider.Blackboard.KeyMissing"),
		TEXT("AIProvider.Blackboard.KeyInherited"),
		TEXT("AIProvider.Blackboard.EntryReadbackUnavailable")};
	Descriptor.RequiredConsumerIds = {TEXT("ai_profile")};
	FAssetDocumentPrimitiveAdmissionEvidence& Admission = Descriptor.Admission;
	Admission.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Admission.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Admission.ExactEngineVersion = TEXT("5.7.4");
	Admission.PhysicalOwner = EAssetDocumentPrimitiveOwner::AI;
	Admission.PublicModule = TEXT("AssetDocumentAI+AIModule+PropertyEditor");
	Admission.PublicHeader = TEXT("BehaviorTree/BlackboardData.h+BehaviorTree/Blackboard/BlackboardKeyType_Object.h+BehaviorTree/Blackboard/BlackboardKeyType_Class.h+BehaviorTree/Blackboard/BlackboardKeyType_Enum.h+BehaviorTree/Blackboard/BlackboardKeyType_Struct.h+StructUtils/InstancedStruct.h+UObject/UnrealType.h+UObject/Field.h+UObject/EnumProperty.h+Internationalization/Text.h+PropertyEditorModule.h+IPropertyRowGenerator.h+IDetailTreeNode.h+PropertyHandle.h");
	Admission.ExportedAuthoritativeApi = TEXT("UBlackboardData::GetKey>FBlackboardEntry::KeyType>UBlackboardKeyType_Object::BaseClass>UBlackboardKeyType_Object::DefaultValue>UBlackboardKeyType_Class::BaseClass>UBlackboardKeyType_Class::DefaultValue>UBlackboardKeyType_Enum::EnumType>UBlackboardKeyType_Enum::EnumName>UBlackboardKeyType_Enum::DefaultValue>UEnum::GetIndexByValue>UEnum::GetNameStringByValue>UBlackboardKeyType_Struct::DefaultValue>FInstancedStruct::IsValid>FInstancedStruct::GetScriptStruct>FInstancedStruct::GetMemory>TFieldIterator<FProperty>>FField::GetAuthoredName>FProperty::ContainerPtrToValuePtr>FScriptArrayHelper>FScriptSetHelper>FScriptMapHelper>FTextInspector>FPropertyEditorModule::CreatePropertyRowGenerator>IPropertyRowGenerator::SetObjects>IDetailTreeNode::CreatePropertyHandle>IPropertyHandle::GetValue");
	Admission.AuthorityContextId = Descriptor.PrimitiveId + TEXT(".context.game-thread-editor-ready.v2");
	Admission.PrimaryOperationId = Descriptor.PrimitiveId;
	Admission.IdentityLifetimeContractId = TEXT("P.identity.path-resolve-single-invocation.v2");
	Admission.TransactionContractId = TEXT("P.transaction.no-active-transaction.v2");
	Admission.RollbackContractId = TEXT("ai.blackboard.entry.readback.v2.rollback.read_only.v2");
	Admission.CompletionContractId = TEXT("ai.blackboard.entry.readback.v2.completion.canonical_readback.v2");
	Admission.SaveReloadEvidenceId = Descriptor.PrimitiveId + TEXT(".provider-no-persistence-fresh-reload.v2");
	Admission.FailureInjectionTestIds = {
		TEXT("UEAssetDoc.AIProvider.BlackboardV2.Readback.Failure")};
	Admission.VerificationTestIds = {
		TEXT("UEAssetDoc.AIProvider.Authority.PublicSurface"),
		TEXT("UEAssetDoc.AIProvider.BlackboardKeyTypeStructCanonicalRead"),
		TEXT("UEAssetDoc.AIProvider.BlackboardV2.Readback")};
	Admission.AuthorityFingerprint = TEXT("ue-5.7.4-launcher-cl-51494982|ai|")
		+ Descriptor.PrimitiveId + TEXT("|AssetDocumentAI+AIModule+PropertyEditor|")
		+ Admission.ExportedAuthoritativeApi
		+ TEXT("|AD.AI.BLACKBOARD.KEYTYPE-STRUCT-CANONICAL-READ-v1|")
		+ TEXT("AssetDocumentAIBlackboardKeyTypeStructCanonicalRead::ReadCanonicalValue(ExactLiveDefaultValue)");
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeParentReadbackDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = TEXT("ai.blackboard.parent.readback.v2"); Descriptor.Version = 2;
	Descriptor.ProviderId = TEXT("ai"); Descriptor.Owner = EAssetDocumentPrimitiveOwner::AI;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = EAssetDocumentPrimitiveAccess::ReadOnly;
	Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight, EAssetDocumentRuntimeRequestPhase::Verify};
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v2"); Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v2");
	Descriptor.StableDiagnosticCodes = {TEXT("AIProvider.InvalidValueShape"), TEXT("AIProvider.InvalidContext"), TEXT("AIProvider.InvalidTransaction"), TEXT("AIProvider.InvalidHandle"), TEXT("AIProvider.AuthorityUnavailable"), TEXT("AIProvider.OperationRejected"), TEXT("AIProvider.Blackboard.ParentUnavailable"), TEXT("AIProvider.Blackboard.ParentClassMismatch"), TEXT("AIProvider.Blackboard.ParentReadbackUnavailable")};
	Descriptor.RequiredConsumerIds = {TEXT("ai_profile")}; auto& A = Descriptor.Admission;
	A.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper; A.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper; A.ExactEngineVersion = TEXT("5.7.4"); A.PhysicalOwner = EAssetDocumentPrimitiveOwner::AI;
	A.PublicModule = TEXT("AssetDocumentAI+AIModule+PropertyEditor"); A.PublicHeader = TEXT("BehaviorTree/BlackboardData.h+PropertyEditorModule.h+IPropertyRowGenerator.h+IDetailTreeNode.h+PropertyHandle.h");
	A.ExportedAuthoritativeApi = TEXT("exact /Script/AIModule.BlackboardData target class>FPropertyEditorModule::CreatePropertyRowGenerator>IPropertyRowGenerator::SetPropertyGenerationAllowListPaths>IPropertyRowGenerator::SetObjects({&ExactBlackboard})>IDetailTreeNode::CreatePropertyHandle>exact UBlackboardData::Parent IPropertyHandle>IPropertyHandle::GetValue(const UObject*&)");
	A.AuthorityContextId = Descriptor.PrimitiveId + TEXT(".context.game-thread-editor-ready.v2"); A.PrimaryOperationId = Descriptor.PrimitiveId; A.IdentityLifetimeContractId = Descriptor.PrimitiveId + TEXT(".identity.path-resolve-single-invocation.v2"); A.TransactionContractId = Descriptor.PrimitiveId + TEXT(".transaction.no-active-transaction.v2"); A.RollbackContractId = Descriptor.PrimitiveId + TEXT(".rollback.read_only.v2"); A.CompletionContractId = Descriptor.PrimitiveId + TEXT(".completion.canonical_parent_readback.v2"); A.SaveReloadEvidenceId = Descriptor.PrimitiveId + TEXT(".provider-no-persistence-fresh-reload.v2");
	A.FailureInjectionTestIds = {TEXT("UEAssetDoc.AIProvider.BlackboardV2.ParentReadback.Failure")}; A.VerificationTestIds = {TEXT("UEAssetDoc.AIProvider.Authority.PublicSurface"), TEXT("UEAssetDoc.AIProvider.BlackboardV2.ParentReadback")}; A.AuthorityFingerprint = TEXT("ue-5.7.4-launcher-cl-51494982|ai|") + Descriptor.PrimitiveId + TEXT("|AssetDocumentAI+AIModule+PropertyEditor|") + A.ExportedAuthoritativeApi;
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeLocalKeysReadbackDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = TEXT("ai.blackboard.local_keys.readback.v2"); Descriptor.Version = 2;
	Descriptor.ProviderId = TEXT("ai"); Descriptor.Owner = EAssetDocumentPrimitiveOwner::AI;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = EAssetDocumentPrimitiveAccess::ReadOnly;
	Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight, EAssetDocumentRuntimeRequestPhase::Verify};
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v2"); Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v2");
	Descriptor.StableDiagnosticCodes = {TEXT("AIProvider.InvalidValueShape"), TEXT("AIProvider.InvalidContext"), TEXT("AIProvider.InvalidTransaction"), TEXT("AIProvider.InvalidHandle"), TEXT("AIProvider.AuthorityUnavailable"), TEXT("AIProvider.OperationRejected"), TEXT("AIProvider.Blackboard.LocalKeyNameInvalid"), TEXT("AIProvider.Blackboard.DuplicateKey"), TEXT("AIProvider.Blackboard.LocalKeysReadbackUnavailable")};
	Descriptor.RequiredConsumerIds = {TEXT("ai_profile")}; auto& A = Descriptor.Admission;
	A.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper; A.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper; A.ExactEngineVersion = TEXT("5.7.4"); A.PhysicalOwner = EAssetDocumentPrimitiveOwner::AI;
	A.PublicModule = TEXT("AssetDocumentAI+AIModule"); A.PublicHeader = TEXT("BehaviorTree/BlackboardData.h");
	A.ExportedAuthoritativeApi = TEXT("exact /Script/AIModule.BlackboardData target class>UBlackboardData::GetKeys()>FBlackboardEntry::EntryName");
	A.AuthorityContextId = Descriptor.PrimitiveId + TEXT(".context.game-thread-editor-ready.v2"); A.PrimaryOperationId = Descriptor.PrimitiveId; A.IdentityLifetimeContractId = Descriptor.PrimitiveId + TEXT(".identity.path-resolve-single-invocation.v2"); A.TransactionContractId = Descriptor.PrimitiveId + TEXT(".transaction.no-active-transaction.v2"); A.RollbackContractId = Descriptor.PrimitiveId + TEXT(".rollback.read_only.v2"); A.CompletionContractId = Descriptor.PrimitiveId + TEXT(".completion.ordered_local_names_readback.v2"); A.SaveReloadEvidenceId = Descriptor.PrimitiveId + TEXT(".provider-no-persistence-fresh-reload.v2");
	A.FailureInjectionTestIds = {TEXT("UEAssetDoc.AIProvider.BlackboardV2.LocalKeysReadback.Failure")}; A.VerificationTestIds = {TEXT("UEAssetDoc.AIProvider.Authority.PublicSurface"), TEXT("UEAssetDoc.AIProvider.BlackboardV2.LocalKeysReadback")}; A.AuthorityFingerprint = TEXT("ue-5.7.4-launcher-cl-51494982|ai|") + Descriptor.PrimitiveId + TEXT("|AssetDocumentAI+AIModule|") + A.ExportedAuthoritativeApi;
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeEntryUpdateExistingDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = TEXT("ai.blackboard.entry.update_existing.v2");
	Descriptor.Version = 2;
	Descriptor.ProviderId = TEXT("ai");
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::AI;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = EAssetDocumentPrimitiveAccess::Mutating;
	Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply};
	Descriptor.bRequiresActiveTransaction = true;
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v2");
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v2");
	Descriptor.MutationFootprintSchemaId = TEXT("assetdocument.ai.blackboard.entry.update_existing.v2");
	Descriptor.HandleArgumentSchemas = {MakeMutableAssetArgument()};
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
	Descriptor.StableDiagnosticCodes = {
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
		TEXT("AIProvider.Blackboard.EntryChangedSetMismatch")};
	Descriptor.RequiredConsumerIds = {TEXT("ai_profile")};
	FAssetDocumentPrimitiveAdmissionEvidence& Admission = Descriptor.Admission;
	Admission.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Admission.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Admission.ExactEngineVersion = TEXT("5.7.4");
	Admission.PhysicalOwner = EAssetDocumentPrimitiveOwner::AI;
	Admission.PublicModule = TEXT("AssetDocumentAI+AIModule+PropertyEditor");
	Admission.PublicHeader = TEXT("TypedPreimage/AssetDocumentAIBlackboardTypedPreimage.h+BehaviorTree/BlackboardData.h+PropertyEditorModule.h+IPropertyRowGenerator.h+IDetailTreeNode.h+PropertyHandle.h");
	Admission.ExportedAuthoritativeApi = TEXT("FPropertyEditorModule::CreatePropertyRowGenerator>IPropertyRowGenerator::SetPropertyGenerationAllowListPaths>IPropertyRowGenerator::SetObjects({&ExactKeyType})>exact admitted KeyType leaf IPropertyHandles>FPropertyEditorModule::CreatePropertyRowGenerator>IPropertyRowGenerator::SetPropertyGenerationAllowListPaths>IPropertyRowGenerator::SetObjects({&ExactBlackboard})>outer Keys IPropertyHandle>{exact existing entry child handle|local-generator-miss-only UBlackboardData::GetKeyID(const FName&)>UBlackboardData::GetKey(FBlackboard::FKey)>read-only parent-aware KeyInherited-or-KeyMissing classification}>CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage>CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage>EnlistFootprint(UObjectState[LocalKeysMetadataAggregate,KeyTypeProperties])>exact admitted KeyType leaf IPropertyHandles>IPropertyHandle::SetValue(..., NotTransactable)>EntryDescription leaf IPropertyHandle>IPropertyHandle::SetValue(..., NotTransactable)>EntryCategory leaf IPropertyHandle>IPropertyHandle::SetValue(..., NotTransactable)>bInstanceSynced leaf IPropertyHandle>IPropertyHandle::SetValue(..., NotTransactable)");
	Admission.AuthorityContextId = Descriptor.PrimitiveId + TEXT(".context.game-thread-editor-ready.v2");
	Admission.PrimaryOperationId = Descriptor.PrimitiveId;
	Admission.IdentityLifetimeContractId = Descriptor.PrimitiveId + TEXT(".identity.ha-request-transaction-revision.v2");
	Admission.TransactionContractId = Descriptor.PrimitiveId + TEXT(".transaction.active-ha.v2");
	Admission.RollbackContractId = TEXT("assetdocument.ai.blackboard.entry.update_existing.rollback.v2");
	Admission.CompletionContractId = Descriptor.PrimitiveId + TEXT(".completion.full_readback.v2");
	Admission.SaveReloadEvidenceId = Descriptor.PrimitiveId + TEXT(".provider-no-persistence-fresh-reload.v2");
	Admission.FailureInjectionTestIds = {
		TEXT("UEAssetDoc.AIProvider.BlackboardV2.UpdateExisting.Failure")};
	Admission.VerificationTestIds = {
		TEXT("UEAssetDoc.AIProvider.Authority.PublicSurface"),
		TEXT("UEAssetDoc.AIProvider.BlackboardV2.UpdateExisting")};
		Admission.AuthorityFingerprint = TEXT("ue-5.7.4-launcher-cl-51494982|ai|")
			+ Descriptor.PrimitiveId + TEXT("|AssetDocumentAI+AIModule+PropertyEditor|")
			+ Admission.ExportedAuthoritativeApi + TEXT("|AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::PrepareCanonicalWrite(UpdateExistingV2)>CaptureAndEnlistAssetDocumentAIBlackboardKeyTypePropertiesPreimage>EnlistFootprint(UObjectState[LocalKeysMetadataAggregate,KeyTypeProperties])>AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::ApplyPreparedCanonicalWrite(UpdateExistingV2)>full canonical entry readback");
	return Descriptor;
}

void PopulateParentSetV2Admission(FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	FAssetDocumentPrimitiveAdmissionEvidence& Admission = Descriptor.Admission;
	Admission.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Admission.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Admission.ExactEngineVersion = TEXT("5.7.4");
	Admission.PhysicalOwner = EAssetDocumentPrimitiveOwner::AI;
	Admission.PublicModule = TEXT("AssetDocumentAI+AssetDocumentEditor+AIModule+PropertyEditor");
	Admission.PublicHeader = TEXT("TypedPreimage/AssetDocumentAIBlackboardTypedPreimage.h+AssetDocumentEditorNativeState.h+BehaviorTree/BlackboardData.h+PropertyEditorModule.h+IPropertyRowGenerator.h+IDetailTreeNode.h+PropertyHandle.h");
	Admission.ExportedAuthoritativeApi = TEXT("WithAssetDocumentEditorAssetIdentity>exact /Script/AIModule.BlackboardData target/desired-Parent class validation>UBlackboardData::IsChildOf(const UBlackboardData&) const>FPropertyEditorModule::CreatePropertyRowGenerator>IPropertyRowGenerator::SetPropertyGenerationAllowListPaths>IPropertyRowGenerator::SetObjects({&ExactBlackboard})>IDetailTreeNode::CreatePropertyHandle>exact UBlackboardData::Parent IPropertyHandle>IPropertyHandle::GetValue(const UObject*&)>CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage>CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage{public capture signature; rollback IPropertyHandle::SetValue(const UObject* const&, EPropertyValueSetFlags::NotTransactable)>IPropertyHandle::GetValue(const UObject*&)}>EnlistFootprint(UObjectState[LocalKeysDerivedClosure,ParentReference])>IPropertyHandle::SetValue(const UObject* const&, EPropertyValueSetFlags::NotTransactable)>UBlackboardData::PropagateKeyChangesToDerivedBlackboardAssets()>IPropertyHandle::GetValue(const UObject*&)");
	Admission.AuthorityContextId = Descriptor.PrimitiveId + TEXT(".context.game-thread-editor-ready.v2");
	Admission.PrimaryOperationId = Descriptor.PrimitiveId;
	Admission.IdentityLifetimeContractId = Descriptor.PrimitiveId + TEXT(".identity.ha-request-transaction-revision.v2");
	Admission.TransactionContractId = Descriptor.PrimitiveId + TEXT(".transaction.active-ha.v2");
	Admission.RollbackContractId = TEXT("assetdocument.ai.blackboard.parent.set.rollback.v2");
	Admission.CompletionContractId = Descriptor.PrimitiveId + TEXT(".completion.parent_readback_and_derived_refresh.v2");
	Admission.SaveReloadEvidenceId = Descriptor.PrimitiveId + TEXT(".provider-no-persistence-fresh-reload.v2");
	Admission.FailureInjectionTestIds = {
		TEXT("UEAssetDoc.AIProvider.BlackboardV2.ParentSet.Failure")};
	Admission.VerificationTestIds = {
		TEXT("UEAssetDoc.EditorNativeState.ObjectReferencePreimageRollback"),
		TEXT("UEAssetDoc.AIProvider.Authority.PublicSurface"),
		TEXT("UEAssetDoc.AIProvider.BlackboardV2.ParentSet")};
	Admission.AuthorityFingerprint = TEXT("ue-5.7.4-launcher-cl-51494982|ai|")
		+ Descriptor.PrimitiveId
		+ TEXT("|AssetDocumentAI+AssetDocumentEditor+AIModule+PropertyEditor|")
		+ Admission.ExportedAuthoritativeApi;
}

FAssetDocumentPrimitiveDescriptor MakeParentSetV2Descriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = TEXT("ai.blackboard.parent.set.v2");
	Descriptor.Version = 2;
	Descriptor.ProviderId = TEXT("ai");
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::AI;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = EAssetDocumentPrimitiveAccess::Mutating;
	Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply};
	Descriptor.bRequiresActiveTransaction = true;
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v2");
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v2");
	Descriptor.MutationFootprintSchemaId = TEXT("assetdocument.ai.blackboard.parent.set.v2");
	Descriptor.HandleArgumentSchemas = {MakeMutableAssetArgument()};
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
	Descriptor.StableDiagnosticCodes = {
		TEXT("AIProvider.InvalidValueShape"),
		TEXT("AIProvider.InvalidContext"),
		TEXT("AIProvider.InvalidTransaction"),
		TEXT("AIProvider.InvalidHandle"),
		TEXT("AIProvider.AuthorityUnavailable"),
		TEXT("AIProvider.OperationRejected"),
		TEXT("AIProvider.Blackboard.ParentUnavailable"),
		TEXT("AIProvider.Blackboard.ParentClassMismatch"),
		TEXT("AIProvider.Blackboard.ParentCycle"),
		TEXT("AIProvider.Blackboard.ParentWriteRejected"),
		TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
		TEXT("AIProvider.Blackboard.ParentReadbackUnavailable"),
		TEXT("AIProvider.Blackboard.ParentChangedSetMismatch")};
	Descriptor.RequiredConsumerIds = {TEXT("ai_profile")};
	PopulateParentSetV2Admission(Descriptor);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeEntryAddV2Descriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = TEXT("ai.blackboard.entry.add.v2");
	Descriptor.Version = 2;
	Descriptor.ProviderId = TEXT("ai");
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::AI;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = EAssetDocumentPrimitiveAccess::Mutating;
	Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply};
	Descriptor.bRequiresActiveTransaction = true;
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v2");
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v2");
	Descriptor.MutationFootprintSchemaId = TEXT("assetdocument.ai.blackboard.entry.add.v2");
	Descriptor.HandleArgumentSchemas = {MakeMutableAssetArgument()};
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
	Descriptor.StableDiagnosticCodes = {
		TEXT("AIProvider.InvalidValueShape"),
		TEXT("AIProvider.InvalidContext"),
		TEXT("AIProvider.InvalidTransaction"),
		TEXT("AIProvider.InvalidHandle"),
		TEXT("AIProvider.AuthorityUnavailable"),
		TEXT("AIProvider.OperationRejected"),
		TEXT("AIProvider.Blackboard.EntrySchemaInvalid"),
		TEXT("AIProvider.Blackboard.KeyAlreadyExists"),
		TEXT("AIProvider.Blackboard.KeyInherited"),
		TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"),
		TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
		TEXT("AIProvider.Blackboard.EntryChangedSetMismatch")};
	Descriptor.RequiredConsumerIds = {TEXT("ai_profile")};
	FAssetDocumentPrimitiveAdmissionEvidence& A = Descriptor.Admission;
	A.Disposition = EAssetDocumentPrimitiveDisposition::AdmittedBridge;
	A.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::BridgeAdmitted;
	A.ExactEngineVersion = TEXT("5.7.4");
	A.PhysicalOwner = EAssetDocumentPrimitiveOwner::AI;
	A.PublicModule = TEXT("AssetDocumentAI+AIModule+PropertyEditor");
	A.PublicHeader = TEXT("AuthorityException/AssetDocumentAIBlackboardStructuralPublication.h+BehaviorTree/BlackboardData.h+PropertyHandle.h");
	A.ExportedAuthoritativeApi = TEXT("AssetDocumentAIBlackboardStructuralPublication::StageKeyType");
	A.AuthorityContextId = Descriptor.PrimitiveId + TEXT(".context.game-thread-editor-ready.v2");
	A.PrimaryOperationId = Descriptor.PrimitiveId;
	A.IdentityLifetimeContractId = Descriptor.PrimitiveId + TEXT(".identity.ha-request-transaction-revision.v2");
	A.TransactionContractId = Descriptor.PrimitiveId + TEXT(".transaction.active-ha.v2");
	A.RollbackContractId = TEXT("assetdocument.ai.blackboard.entry.add.rollback.v2");
	A.CompletionContractId = Descriptor.PrimitiveId + TEXT(".completion.full_readback_and_order.v2");
	A.SaveReloadEvidenceId = Descriptor.PrimitiveId + TEXT(".provider-no-persistence-fresh-reload.v2");
	A.FailureInjectionTestIds = {
		TEXT("UEAssetDoc.AIProvider.BlackboardV2.Add.Failure")};
	A.VerificationTestIds = {
		TEXT("UEAssetDoc.AIProvider.Authority.PublicSurface"),
		TEXT("UEAssetDoc.AIProvider.BlackboardV2.StructuralSourceCheck"),
		TEXT("UEAssetDoc.AIProvider.BlackboardV2.Add")};
	A.SourceMarker = TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_ADMISSION(AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1)");
	A.RawLifecycleAllowlistId = TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_CALLSITE(AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1|ai.blackboard.entry.add.v2)");
	A.GovernanceId = TEXT("AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1");
	A.IndependentCandidateReviewId = TEXT("candidate-review.initial/AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1");
	A.IndependentBridgeReviewId = TEXT("bridge-review.initial/AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1");
	A.HelperReplacementCondition = TEXT("Replace only with a stock public/exported Blackboard-specific lifecycle that fully covers this row.");
	A.RemovalCondition = TEXT("Remove only after migrated row evidence passes and its obsolete quarantine dependency and allowlist are deleted.");
	A.AuthorityFingerprint = TEXT(
		"ue-5.7.4-launcher-cl-51494982|ai|ai.blackboard.entry.add.v2|"
			"AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1+AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1|"
			"WithAssetDocumentEditorAssetIdentity>exact absence/inherited/shadow/index/full-entry preflight>AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::PrepareCanonicalWrite(AddV2)>"
			"CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage>"
			"EnlistFootprint(UObjectState[LocalKeysAggregate])>"
			"AssetDocumentAIBlackboardStructuralPublication::StageKeyType>AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::ApplyPreparedCanonicalWrite(AddV2)>"
			"typed complete KeyType configure+canonical validation>"
		"AssetDocumentAIBlackboardStructuralPublication::PublishLocalKeysSnapshot>"
		"IPropertyHandle::NotifyPreChange>UBlackboardData::Keys assignment>"
		"IPropertyHandle::NotifyPostChange(EPropertyChangeType::ValueSet)>"
		"IPropertyHandle::NotifyFinishedChangingProperties>"
		"UBlackboardData::UpdateParentKeys()>UBlackboardData::UpdateKeyIDs()>"
			"UBlackboardData::UpdateIfHasSynchronizedKeys()>UBlackboardData::PropagateKeyChangesToDerivedBlackboardAssets()>"
		"full canonical entry/order readback");
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeEntryReplaceClassV2Descriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = TEXT("ai.blackboard.entry.replace_class.v2");
	Descriptor.Version = 2;
	Descriptor.ProviderId = TEXT("ai");
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::AI;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = EAssetDocumentPrimitiveAccess::Mutating;
	Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply};
	Descriptor.bRequiresActiveTransaction = true;
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v2");
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v2");
	Descriptor.MutationFootprintSchemaId =
		TEXT("assetdocument.ai.blackboard.entry.replace_class.v2");
	Descriptor.HandleArgumentSchemas = {MakeMutableAssetArgument()};
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
	Descriptor.StableDiagnosticCodes = {
		TEXT("AIProvider.InvalidValueShape"),
		TEXT("AIProvider.InvalidContext"),
		TEXT("AIProvider.InvalidTransaction"),
		TEXT("AIProvider.InvalidHandle"),
		TEXT("AIProvider.AuthorityUnavailable"),
		TEXT("AIProvider.OperationRejected"),
		TEXT("AIProvider.Blackboard.EntrySchemaInvalid"),
		TEXT("AIProvider.Blackboard.KeyMissing"),
		TEXT("AIProvider.Blackboard.KeyInherited"),
		TEXT("AIProvider.Blackboard.KeyClassReplacementUnavailable"),
		TEXT("AIProvider.Blackboard.EntryMaterializationUnavailable"),
		TEXT("AIProvider.Blackboard.DerivedRefreshFailed"),
		TEXT("AIProvider.Blackboard.EntryChangedSetMismatch")};
	Descriptor.RequiredConsumerIds = {TEXT("ai_profile")};
	FAssetDocumentPrimitiveAdmissionEvidence& A = Descriptor.Admission;
	A.Disposition = EAssetDocumentPrimitiveDisposition::AdmittedBridge;
	A.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::BridgeAdmitted;
	A.ExactEngineVersion = TEXT("5.7.4");
	A.PhysicalOwner = EAssetDocumentPrimitiveOwner::AI;
	A.PublicModule = TEXT("AssetDocumentAI+AIModule+PropertyEditor");
	A.PublicHeader = TEXT("AuthorityException/AssetDocumentAIBlackboardStructuralPublication.h+BehaviorTree/BlackboardData.h+PropertyHandle.h");
	A.ExportedAuthoritativeApi =
		TEXT("AssetDocumentAIBlackboardStructuralPublication::StageKeyType");
	A.AuthorityContextId = Descriptor.PrimitiveId + TEXT(".context.game-thread-editor-ready.v2");
	A.PrimaryOperationId = Descriptor.PrimitiveId;
	A.IdentityLifetimeContractId =
		Descriptor.PrimitiveId + TEXT(".identity.ha-request-transaction-revision.v2");
	A.TransactionContractId = Descriptor.PrimitiveId + TEXT(".transaction.active-ha.v2");
	A.RollbackContractId = TEXT("assetdocument.ai.blackboard.entry.replace_class.rollback.v2");
	A.CompletionContractId = Descriptor.PrimitiveId + TEXT(".completion.full_readback.v2");
	A.SaveReloadEvidenceId =
		Descriptor.PrimitiveId + TEXT(".provider-no-persistence-fresh-reload.v2");
	A.FailureInjectionTestIds = {
		TEXT("UEAssetDoc.AIProvider.BlackboardV2.ReplaceClass.Failure")};
	A.VerificationTestIds = {
		TEXT("UEAssetDoc.AIProvider.Authority.PublicSurface"),
		TEXT("UEAssetDoc.AIProvider.BlackboardV2.StructuralSourceCheck"),
		TEXT("UEAssetDoc.AIProvider.BlackboardV2.ReplaceClass")};
	A.SourceMarker =
		TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_ADMISSION(AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1)");
	A.RawLifecycleAllowlistId =
		TEXT("ASSETDOCUMENT_AUTHORITY_BRIDGE_CALLSITE(AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1|ai.blackboard.entry.replace_class.v2)");
	A.GovernanceId = TEXT("AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1");
	A.IndependentCandidateReviewId =
		TEXT("candidate-review.initial/AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1");
	A.IndependentBridgeReviewId =
		TEXT("bridge-review.initial/AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1");
	A.HelperReplacementCondition = TEXT(
		"Replace only with a stock public/exported Blackboard-specific lifecycle that fully covers this row.");
	A.RemovalCondition = TEXT(
		"Remove only after migrated row evidence passes and its obsolete quarantine dependency and allowlist are deleted.");
	A.AuthorityFingerprint = TEXT(
		"ue-5.7.4-launcher-cl-51494982|ai|ai.blackboard.entry.replace_class.v2|"
			"AD.AI.BLACKBOARD.KEYTYPE-STAGING-v1+AD.AI.BLACKBOARD.LOCAL-KEYS-PUBLICATION-v1|"
			"WithAssetDocumentEditorAssetIdentity>exact owned-local/same-name/distinct-class/full-entry preflight>AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::PrepareCanonicalWrite(ReplaceClassV2)>"
			"CaptureAndEnlistAssetDocumentAIBlackboardLocalKeysPreimage>"
			"EnlistFootprint(UObjectState[LocalKeysAggregate])>"
			"AssetDocumentAIBlackboardStructuralPublication::StageKeyType>AssetDocumentAIBlackboardKeyTypeStructCanonicalWrite::ApplyPreparedCanonicalWrite(ReplaceClassV2)>"
			"typed complete KeyType configure+canonical validation>"
		"replace exact local snapshot slot>"
		"AssetDocumentAIBlackboardStructuralPublication::PublishLocalKeysSnapshot>"
		"IPropertyHandle::NotifyPreChange>UBlackboardData::Keys assignment>"
		"IPropertyHandle::NotifyPostChange(EPropertyChangeType::ValueSet)>"
		"IPropertyHandle::NotifyFinishedChangingProperties>"
		"UBlackboardData::UpdateParentKeys()>UBlackboardData::UpdateKeyIDs()>"
		"UBlackboardData::UpdateIfHasSynchronizedKeys()>"
		"UBlackboardData::PropagateKeyChangesToDerivedBlackboardAssets()>"
		"full canonical entry/order readback");
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeBlockerDescriptor(
	const TCHAR* PrimitiveId,
	const TCHAR* StableDiagnosticCode)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = PrimitiveId;
	Descriptor.Version = 1;
	Descriptor.ProviderId = TEXT("ai");
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::AI;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = EAssetDocumentPrimitiveAccess::ReadOnly;
	Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply};
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
} // namespace AssetDocumentAIBlackboardCatalog

void AppendAssetDocumentAIBlackboardPrimitiveDescriptors(
	TArray<FAssetDocumentPrimitiveDescriptor>& OutDescriptors)
{
	using namespace AssetDocumentAIBlackboardCatalog;

	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.blackboard.key_type.inspect.v1"),
		EAssetDocumentPrimitiveAccess::ReadOnly,
		{EAssetDocumentRuntimeRequestPhase::Validate,
			EAssetDocumentRuntimeRequestPhase::Preflight},
		TEXT(""),
		{},
		{TEXT("AIProvider.Blackboard.KeyMissing"),
			TEXT("AIProvider.Blackboard.KeyTypeClassMismatch"),
			TEXT("AIProvider.Blackboard.PropertyEvidenceUnavailable")}));
	OutDescriptors.Add(MakeBlockerDescriptor(
		TEXT("ai.blackboard.key_type.create.v1"),
		TEXT("AD.BLOCK.AI.BLACKBOARD_KEY_TYPE_AUTHORITY")));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.blackboard.key_type.apply_properties.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		{EAssetDocumentRuntimeRequestPhase::Apply},
		TEXT("assetdocument.ai.blackboard.key_type_properties.v1"),
		{MakeMutableAssetArgument()},
		{TEXT("AIProvider.Blackboard.KeyMissing"),
			TEXT("AIProvider.Blackboard.KeyTypeClassMismatch"),
			TEXT("AIProvider.Blackboard.PropertyNotAdmitted"),
			TEXT("AIProvider.Blackboard.PropertyWriteRejected")}));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.blackboard.parent.set.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		{EAssetDocumentRuntimeRequestPhase::Apply},
		TEXT("assetdocument.ai.blackboard.parent.v1"),
		{MakeMutableAssetArgument()},
		{TEXT("AIProvider.Blackboard.ParentCycle"),
			TEXT("AIProvider.Blackboard.ParentClassMismatch"),
			TEXT("AIProvider.Blackboard.ParentUnavailable"),
			TEXT("AIProvider.Blackboard.DerivedRefreshFailed")}));
	OutDescriptors.Add(MakeOrdinaryDescriptor(
		TEXT("ai.blackboard.local_keys.replace.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		{EAssetDocumentRuntimeRequestPhase::Apply},
		TEXT("assetdocument.ai.blackboard.local_keys.v1"),
		{MakeMutableAssetArgument()},
		{TEXT("AIProvider.Blackboard.DuplicateKey"),
			TEXT("AIProvider.Blackboard.ShadowConflict"),
			TEXT("AIProvider.Blackboard.NewKeyTypeRequiresBlockedAuthority"),
			TEXT("AIProvider.Blackboard.DerivedRefreshFailed")}));
	OutDescriptors.Add(MakeEntryReadbackDescriptor());
	OutDescriptors.Add(MakeParentReadbackDescriptor());
	OutDescriptors.Add(MakeLocalKeysReadbackDescriptor());
	OutDescriptors.Add(MakeEntryUpdateExistingDescriptor());
	OutDescriptors.Add(MakeParentSetV2Descriptor());
	OutDescriptors.Add(MakeEntryAddV2Descriptor());
	OutDescriptors.Add(MakeEntryReplaceClassV2Descriptor());
	OutDescriptors.Add(MakeAssetDocumentAIBlackboardEntryRemoveV2Descriptor());
}
