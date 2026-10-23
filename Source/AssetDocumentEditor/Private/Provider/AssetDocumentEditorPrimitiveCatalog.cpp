// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/AssetDocumentEditorPrimitiveCatalog.h"
#include "AssetDocumentEditorTargetLifecycleCatalog.h"
#include "Provider/Lifecycle/AssetDocumentEditorGenericFactory.h"
#include "UObject/SoftObjectPath.h"

namespace AssetDocumentEditorPrimitiveCatalog
{
constexpr uint32 ProviderVersion = 1;

FAssetDocumentPrimitiveAdmissionEvidence MakeOrdinaryEvidence(
	const FString& PrimitiveId,
	const TCHAR* PublicModule,
	const TCHAR* PublicHeader,
	const TCHAR* AuthoritativeApi,
	const TCHAR* PrimaryOperationId,
	const TCHAR* VerificationTestId)
{
	FAssetDocumentPrimitiveAdmissionEvidence Evidence;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Evidence.ExactEngineVersion = TEXT("5.7.4");
	Evidence.PhysicalOwner = EAssetDocumentPrimitiveOwner::Editor;
	Evidence.PublicModule = PublicModule;
	Evidence.PublicHeader = PublicHeader;
	Evidence.ExportedAuthoritativeApi = AuthoritativeApi;
	Evidence.AuthorityContextId = TEXT("assetdocument.editor.game-thread.editor-ready.v1");
	Evidence.PrimaryOperationId = PrimaryOperationId;
	Evidence.IdentityLifetimeContractId = TEXT("identity.request-transaction-revision-bound.v1");
	Evidence.TransactionContractId = TEXT("transaction.descriptor-governed.v1");
	Evidence.RollbackContractId = TEXT("rollback.atomic-owner-governed.v1");
	Evidence.CompletionContractId = PrimitiveId + TEXT(".completion.v1");
	Evidence.SaveReloadEvidenceId = PrimitiveId + TEXT(".test-owned-save-reload.v1");
	Evidence.FailureInjectionTestIds = {PrimitiveId + TEXT(".failure-injection.v1")};
	Evidence.VerificationTestIds = {VerificationTestId};
	Evidence.AuthorityFingerprint = FString::Printf(
		TEXT("ue-5.7.4|editor|%s|%s"),
		PrimaryOperationId,
		AuthoritativeApi);
	return Evidence;
}

FAssetDocumentPrimitiveDescriptor MakeOrdinaryDescriptor(
	const TCHAR* PrimitiveId,
	EAssetDocumentPrimitiveAccess Access,
	std::initializer_list<EAssetDocumentRuntimeRequestPhase> AllowedPhases,
	const TCHAR* PublicModule,
	const TCHAR* PublicHeader,
	const TCHAR* AuthoritativeApi,
	const TCHAR* PrimaryOperationId,
	const TCHAR* VerificationTestId)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = PrimitiveId;
	Descriptor.Version = ProviderVersion;
	Descriptor.ProviderId = TEXT("editor");
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::Editor;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = Access;
	Descriptor.AllowedPhases = AllowedPhases;
	Descriptor.bRequiresActiveTransaction =
		Access == EAssetDocumentPrimitiveAccess::Mutating;
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v1");
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v1");
	if (Descriptor.bRequiresActiveTransaction)
	{
		Descriptor.MutationFootprintSchemaId =
			Descriptor.PrimitiveId + TEXT(".footprint.v1");
	}
	Descriptor.StableDiagnosticCodes = {
		TEXT("AD.RUNTIME.PRIMITIVE_FAILED"),
		TEXT("AD.RUNTIME.PRIMITIVE_UNAVAILABLE")
	};
	Descriptor.RequiredConsumerIds = {TEXT("route.generic.v1")};
	Descriptor.Admission = MakeOrdinaryEvidence(
		Descriptor.PrimitiveId,
		PublicModule,
		PublicHeader,
		AuthoritativeApi,
		PrimaryOperationId,
		VerificationTestId);
	return Descriptor;
}

void AddArgument(
	FAssetDocumentPrimitiveDescriptor& Descriptor,
	const TCHAR* Name,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access,
	bool bRequired = true)
{
	Descriptor.AllowedHandleKinds.AddUnique(Kind);
	Descriptor.HandleArgumentSchemas.Add({Name, Kind, Access, bRequired});
}

void AddResult(
	FAssetDocumentPrimitiveDescriptor& Descriptor,
	const TCHAR* Name,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access,
	EAssetDocumentHandleResultCardinality Cardinality)
{
	Descriptor.AllowedHandleKinds.AddUnique(Kind);
	Descriptor.HandleResultSchemas.Add({Name, Kind, Access, Cardinality});
}

FAssetDocumentPrimitiveDescriptor MakeClassResolveDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.class.resolve_canonical.v1"),
		EAssetDocumentPrimitiveAccess::ReadOnly,
		{EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply},
		TEXT("CoreUObject"),
		TEXT("UObject/SoftObjectPath.h;UObject/Class.h"),
		TEXT("FSoftClassPath::TryLoadClass<UObject>;UStruct::IsChildOf"),
		TEXT("coreuobject.class.resolve-canonical.v1"),
		TEXT("UEAssetDoc.GenericProvider.ClassReference.CanonicalClassResolve"));
	AddResult(Descriptor, TEXT("class"), EAssetDocumentHandleKind::Class,
		EAssetDocumentHandleAccess::ReadOnly,
		EAssetDocumentHandleResultCardinality::ExactlyOne);
	Descriptor.RequiredConsumerIds.Add(TEXT("animation_profile"));
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeReferenceResolveDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.reference.resolve_asset.v1"),
		EAssetDocumentPrimitiveAccess::ReadOnly,
		// Apply: a create resolves its factory context (e.g. a DataTable RowStruct, which may be
		// a native /Script struct the asset registry does not list) in the Apply phase too.
		{EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply},
		TEXT("CoreUObject"),
		TEXT("UObject/SoftObjectPath.h;UObject/Object.h"),
		TEXT("FSoftObjectPath::TryLoad;UObject::IsA"),
		TEXT("coreuobject.asset-reference.resolve.v1"),
		TEXT("UEAssetDoc.GenericProvider.ClassReference.AssetResolveReadOnly"));
	AddResult(Descriptor, TEXT("asset"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly,
		EAssetDocumentHandleResultCardinality::ExactlyOne);
	Descriptor.RequiredConsumerIds.Add(TEXT("animation_profile"));
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakePropertyEnumerateDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.property.enumerate_admitted.v1"),
		EAssetDocumentPrimitiveAccess::ReadOnly,
		{EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair,
			EAssetDocumentRuntimeRequestPhase::Verify},
		TEXT("CoreUObject"),
		TEXT("UObject/UnrealType.h;UObject/PropertyAccessUtil.h"),
		TEXT("TFieldIterator<FProperty>;PropertyAccessUtil::CanGetPropertyValue;PropertyAccessUtil::CanSetPropertyValue"),
		TEXT("coreuobject.property.enumerate-admitted.v1"),
		TEXT("UEAssetDoc.GenericProvider.Property.DeterministicEnumeration"));
	AddResult(Descriptor, TEXT("properties"), EAssetDocumentHandleKind::Property,
		EAssetDocumentHandleAccess::ReadOnly,
		EAssetDocumentHandleResultCardinality::ZeroOrMore);
	// AnimMontage create admits Properties from the class before the target exists.
	Descriptor.RequiredConsumerIds.Add(TEXT("animation_profile"));
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakePropertyInspectEvidenceDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.property.inspect_evidence.v1"),
		EAssetDocumentPrimitiveAccess::ReadOnly,
		{EAssetDocumentRuntimeRequestPhase::Verify},
		TEXT("CoreUObject"),
		TEXT("UObject/PropertyAccessUtil.h;UObject/UnrealType.h"),
		TEXT("TFieldIterator<FProperty>;PropertyAccessUtil::GetPropertyValue_Object"),
		TEXT("coreuobject.property.inspect-evidence.v1"),
		TEXT("UEAssetDoc.GenericProvider.Property.InspectEvidence"));
	AddArgument(Descriptor, TEXT("asset"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly, false);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeEnhancedInputCheckDescriptor()
{
	// The generic profile's Enhanced Input checks (warnings) on the preflight scratch.
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.enhanced_input.check_domain.v1"),
		EAssetDocumentPrimitiveAccess::Scratch,
		{EAssetDocumentRuntimeRequestPhase::Preflight},
		TEXT("EnhancedInput"),
		TEXT("InputAction.h;InputMappingContext.h;InputModifiers.h"),
		TEXT("UInputAction::ValueType;UInputMappingContext::GetMappings;UInputModifierSwizzleAxis::Order"),
		TEXT("enhancedinput.domain.check.v1"),
		TEXT("UEAssetDoc.GenericProvider.EnhancedInput.DomainWarnings"));
	AddArgument(Descriptor, TEXT("scratch"), EAssetDocumentHandleKind::Scratch,
		EAssetDocumentHandleAccess::Mutable);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakePropertyEnumerateBatchDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.property.enumerate_admitted_batch.v1"),
		EAssetDocumentPrimitiveAccess::ReadOnly,
		{EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair,
			EAssetDocumentRuntimeRequestPhase::Verify},
		TEXT("CoreUObject"),
		TEXT("UObject/UnrealType.h;UObject/PropertyAccessUtil.h"),
		TEXT("TFieldIterator<FProperty>;PropertyAccessUtil::CanGetPropertyValue;PropertyAccessUtil::CanSetPropertyValue"),
		TEXT("coreuobject.property.enumerate-admitted-batch.v1"),
		TEXT("UEAssetDoc.GenericProvider.Property.BatchToken"));
	AddArgument(Descriptor, TEXT("asset"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly);
	AddResult(Descriptor, TEXT("properties"), EAssetDocumentHandleKind::Property,
		EAssetDocumentHandleAccess::ReadOnly,
		EAssetDocumentHandleResultCardinality::ZeroOrMore);
	AddResult(Descriptor, TEXT("property_batch"), EAssetDocumentHandleKind::Property,
		EAssetDocumentHandleAccess::ReadOnly,
		EAssetDocumentHandleResultCardinality::ExactlyOne);
	Descriptor.RequiredConsumerIds.Add(TEXT("animation_profile"));
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakePropertyReadDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.property.read_admitted.v1"),
		EAssetDocumentPrimitiveAccess::ReadOnly,
		{EAssetDocumentRuntimeRequestPhase::Verify},
		TEXT("CoreUObject"),
		TEXT("UObject/PropertyAccessUtil.h"),
		TEXT("PropertyAccessUtil::GetPropertyValue_Object"),
		TEXT("coreuobject.property.read-admitted.v1"),
		TEXT("UEAssetDoc.GenericProvider.Property.ReadAdmitted"));
	AddArgument(Descriptor, TEXT("asset"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly);
	AddArgument(Descriptor, TEXT("property"), EAssetDocumentHandleKind::Property,
		EAssetDocumentHandleAccess::ReadOnly);
	Descriptor.RequiredConsumerIds.Add(TEXT("animation_profile"));
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakePropertyWriteDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.property.write_admitted.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		{EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair},
		TEXT("CoreUObject"),
		TEXT("UObject/PropertyAccessUtil.h"),
		TEXT("PropertyAccessUtil::SetPropertyValue_Object"),
		TEXT("coreuobject.property.write-admitted.v1"),
		TEXT("UEAssetDoc.GenericProvider.Property.WriteAdmitted"));
	AddArgument(Descriptor, TEXT("asset"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable);
	AddArgument(Descriptor, TEXT("property"), EAssetDocumentHandleKind::Property,
		EAssetDocumentHandleAccess::ReadOnly);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakePropertyWriteBatchDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.property.write_admitted_batch.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		{EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair},
		TEXT("CoreUObject"),
		TEXT("UObject/PropertyAccessUtil.h"),
		TEXT("PropertyAccessUtil::SetPropertyValue_Object"),
		TEXT("coreuobject.property.write-admitted-batch.v1"),
		TEXT("UEAssetDoc.GenericProvider.Property.BatchWrite"));
	AddArgument(Descriptor, TEXT("asset"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable);
	AddArgument(Descriptor, TEXT("property_batch"), EAssetDocumentHandleKind::Property,
		EAssetDocumentHandleAccess::ReadOnly);
	Descriptor.RequiredConsumerIds.Add(TEXT("animation_profile"));
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakePropertyPreflightDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.property.preflight_admitted.v1"),
		EAssetDocumentPrimitiveAccess::Scratch,
		{EAssetDocumentRuntimeRequestPhase::Preflight},
		TEXT("CoreUObject"),
		TEXT("UObject/PropertyAccessUtil.h"),
		TEXT("PropertyAccessUtil::SetPropertyValue_Object"),
		TEXT("coreuobject.property.preflight-admitted.v1"),
		TEXT("UEAssetDoc.GenericProvider.Property.PreflightUsesScratch"));
	AddArgument(Descriptor, TEXT("scratch"), EAssetDocumentHandleKind::Scratch,
		EAssetDocumentHandleAccess::Mutable);
	AddArgument(Descriptor, TEXT("property"), EAssetDocumentHandleKind::Property,
		EAssetDocumentHandleAccess::ReadOnly);
	Descriptor.RequiredConsumerIds.Add(TEXT("animation_profile"));
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeStructCodecDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.fragment.struct_codec.v1"),
		EAssetDocumentPrimitiveAccess::Scratch,
		{EAssetDocumentRuntimeRequestPhase::Preflight, EAssetDocumentRuntimeRequestPhase::Verify},
		TEXT("CoreUObject;JsonUtilities"),
		TEXT("UObject/SoftObjectPath.h;UObject/StructOnScope.h;JsonObjectConverter.h"),
		TEXT("FSoftObjectPath::TryLoad;FStructOnScope;FJsonObjectConverter"),
		TEXT("coreuobject.struct.strict-codec.v1"),
		TEXT("UEAssetDoc.GenericProvider.Struct.TypedRoundTrip"));
	AddArgument(Descriptor, TEXT("schema"), EAssetDocumentHandleKind::Struct,
		EAssetDocumentHandleAccess::ReadOnly, false);
	AddResult(Descriptor, TEXT("schema"), EAssetDocumentHandleKind::Struct,
		EAssetDocumentHandleAccess::ReadOnly,
		EAssetDocumentHandleResultCardinality::ZeroOrOne);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeEmbeddedCreateDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.fragment.embedded_object_create.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		{EAssetDocumentRuntimeRequestPhase::Apply},
		TEXT("Engine"),
		TEXT("Interfaces/Interface_AssetUserData.h"),
		TEXT("IInterface_AssetUserData::AddAssetUserDataOfClass;RemoveUserDataOfClass"),
		TEXT("engine.asset-user-data.create-owned.v1"),
		TEXT("UEAssetDoc.GenericProvider.Embedded.AdmittedAssetUserData"));
	AddArgument(Descriptor, TEXT("outer_asset"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable);
	AddArgument(Descriptor, TEXT("object_class"), EAssetDocumentHandleKind::Class,
		EAssetDocumentHandleAccess::ReadOnly);
	AddResult(Descriptor, TEXT("subobject"), EAssetDocumentHandleKind::Subobject,
		EAssetDocumentHandleAccess::Mutable,
		EAssetDocumentHandleResultCardinality::ExactlyOne);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeScratchCloneDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.scratch.clone_asset.v1"),
		EAssetDocumentPrimitiveAccess::Scratch,
		{EAssetDocumentRuntimeRequestPhase::Preflight},
		TEXT("CoreUObject"),
		TEXT("UObject/UObjectGlobals.h"),
		TEXT("Static") TEXT("Duplicate") TEXT("ObjectEx(FObjectDuplicationParameters);GetTransientPackage"),
		TEXT("framework.scratch.clone_asset.v1"),
		TEXT("UEAssetDoc.GenericProvider.Scratch.NoResidue"));
	Descriptor.Admission.Disposition =
		EAssetDocumentPrimitiveDisposition::FrameworkRawLifecycleAllowlisted;
	Descriptor.Admission.AuthorityPath =
		EAssetDocumentPrimitiveAuthorityPath::FrameworkRawLifecycleAllowlist;
	Descriptor.Admission.SourceMarker =
		TEXT("ASSETDOCUMENT_FRAMEWORK_RAW_LIFECYCLE_ALLOWLIST(framework.scratch.clone_asset.v1)");
	Descriptor.Admission.RawLifecycleAllowlistId =
		TEXT("ASSETDOCUMENT_FRAMEWORK_RAW_CALLSITE(framework.scratch.clone_asset.v1)");
	Descriptor.Admission.HelperReplacementCondition =
		TEXT("Replace this exact callsite when UE exports a transient asset-clone helper with equivalent CreatedObjects identity evidence.");
	Descriptor.Admission.RemovalCondition =
		TEXT("Remove this admission if transient flags, created-object capture, or residue-free request-context cleanup cannot be proved.");
	AddArgument(Descriptor, TEXT("source_asset"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly);
	AddResult(Descriptor, TEXT("scratch"), EAssetDocumentHandleKind::Scratch,
		EAssetDocumentHandleAccess::Mutable,
		EAssetDocumentHandleResultCardinality::ExactlyOne);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeScratchCreateFromClassDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.scratch.create_from_class.v1"),
		EAssetDocumentPrimitiveAccess::Scratch,
		{EAssetDocumentRuntimeRequestPhase::Preflight},
		TEXT("UnrealEd"),
		TEXT("Factories/DataAssetFactory.h;UObject/Class.h"),
		TEXT("UClass::GetDefaultObject<UDataAssetFactory>;"
			"UDataAssetFactory::FactoryCreateNew"),
		TEXT("unrealed.data-asset-factory.create-transient-scratch.v1"),
		TEXT("UEAssetDoc.GenericProvider.CreateScratch"));
	Descriptor.Admission.IdentityLifetimeContractId =
		TEXT("request-context Scratch lifetime v1");
	Descriptor.Admission.TransactionContractId =
		TEXT("preflight-no-active-transaction.v1");
	Descriptor.Admission.RollbackContractId =
		TEXT("request-context-destruction-no-live-residue.v1");
	Descriptor.Admission.CompletionContractId =
		TEXT("exact-class-transient-zero-residue.v1");
	AddArgument(Descriptor, TEXT("asset_class"),
		EAssetDocumentHandleKind::Class,
		EAssetDocumentHandleAccess::ReadOnly);
	AddResult(Descriptor, TEXT("scratch"),
		EAssetDocumentHandleKind::Scratch,
		EAssetDocumentHandleAccess::Mutable,
		EAssetDocumentHandleResultCardinality::ExactlyOne);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeResolveExistingDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.asset.resolve_existing.v1"),
		EAssetDocumentPrimitiveAccess::ReadOnly,
		{EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair,
			EAssetDocumentRuntimeRequestPhase::Verify},
		TEXT("AssetRegistry;CoreUObject"),
		TEXT("AssetRegistry/IAssetRegistry.h;AssetRegistry/AssetData.h"),
		TEXT("IAssetRegistry::GetAssetByObjectPath;FAssetData::GetAsset"),
		TEXT("assetregistry.asset.resolve-existing.v1"),
		TEXT("UEAssetDoc.GenericProvider.ResolveExisting.ReadOnly"));
	AddResult(Descriptor, TEXT("asset_read"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly,
		EAssetDocumentHandleResultCardinality::ZeroOrOne);
	AddResult(Descriptor, TEXT("asset_mutable"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable,
		EAssetDocumentHandleResultCardinality::ZeroOrOne);
	AddResult(Descriptor, TEXT("package"), EAssetDocumentHandleKind::Package,
		EAssetDocumentHandleAccess::ReadOnly,
		EAssetDocumentHandleResultCardinality::ZeroOrOne);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeFactoryCreateDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.asset.create_by_factory.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		{EAssetDocumentRuntimeRequestPhase::Apply},
		TEXT("AssetTools;UnrealEd;BehaviorTreeEditor"),
		TEXT("IAssetTools.h;Factories/Factory.h;UObject/UObjectGlobals.h"),
		TEXT("IAssetTools::CreateAsset;UFactory::FactoryCreateNew;StaticFindObjectSafe;UClass::GetDefaultObject(false)"),
		TEXT("assettools.exact-factory.create.v1"),
		TEXT("UEAssetDoc.GenericProvider.FactoryCreate.Authority"));
	AddArgument(Descriptor, TEXT("asset_class"), EAssetDocumentHandleKind::Class,
		EAssetDocumentHandleAccess::ReadOnly);
	AddResult(Descriptor, TEXT("asset_read"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly,
		EAssetDocumentHandleResultCardinality::ExactlyOne);
	AddResult(Descriptor, TEXT("asset_mutable"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable,
		EAssetDocumentHandleResultCardinality::ExactlyOne);
	AddResult(Descriptor, TEXT("package"), EAssetDocumentHandleKind::Package,
		EAssetDocumentHandleAccess::Mutable,
		EAssetDocumentHandleResultCardinality::ExactlyOne);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeRollbackCreatedDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.asset.rollback_created.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		{EAssetDocumentRuntimeRequestPhase::Failed},
		TEXT("UnrealEd"),
		TEXT("ObjectTools.h"),
		TEXT("ObjectTools::DeleteObjects(EAllowCancelDuringDelete::CancelNotAllowed)"),
		TEXT("unrealed.asset.rollback-created.v1"),
		TEXT("UEAssetDoc.GenericProvider.RollbackCreated.Absence"));
	AddArgument(Descriptor, TEXT("asset"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable);
	AddArgument(Descriptor, TEXT("package"), EAssetDocumentHandleKind::Package,
		EAssetDocumentHandleAccess::Mutable);
	Descriptor.RequiredConsumerIds.Add(TEXT("animation_profile"));
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeGenericFactoryCreateDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.asset.create_by_factory.v2"),
		EAssetDocumentPrimitiveAccess::Mutating,
		{EAssetDocumentRuntimeRequestPhase::Apply},
		TEXT("AssetTools;UnrealEd"),
		TEXT("IAssetTools.h;Factories/Factory.h"),
		TEXT("UFactory::GetSupportedClass;UFactory::CanCreateNew;UFactory::ShouldShowInNewMenu;IAssetTools::CreateAsset"),
		TEXT("assettools.generic-factory.create.v1"),
		TEXT("UEAssetDoc.GenericProvider.GenericFactory.CreateAsset"));
	Descriptor.Version = 2;
	Descriptor.StableDiagnosticCodes.Append({TEXT("FactoryCreate.NoFactory"),
		TEXT("FactoryCreate.AbstractClass"), TEXT("FactoryCreate.BlueprintClassUnsupported"),
		TEXT("FactoryCreate.TargetExists"), TEXT("FactoryCreate.IncompleteCreation"),
		TEXT("FactoryCreate.ContextUnsupported"), TEXT("FactoryCreate.ContextAmbiguous"),
		TEXT("FactoryCreate.SubclassUnsupported"), TEXT("FactoryCreate.MissingFactoryContext"),
		TEXT("FactoryCreate.InteractiveFactory")});
	AddArgument(Descriptor, TEXT("asset_class"), EAssetDocumentHandleKind::Class,
		EAssetDocumentHandleAccess::ReadOnly);
	// Optional factory context (AnimMontage: Skeleton), see AssetDocumentEditorGenericFactoryContext.cpp,
	// and a second one for factories whose dialog sets two objects (MirrorDataTable: Skeleton and row struct).
	AddArgument(Descriptor, TEXT("factory_context"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly, false);
	AddArgument(Descriptor, TEXT("factory_context_2"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly, false);
	Descriptor.RequiredConsumerIds.Add(TEXT("animation_profile"));
	AddResult(Descriptor, TEXT("asset_read"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly,
		EAssetDocumentHandleResultCardinality::ExactlyOne);
	AddResult(Descriptor, TEXT("asset_mutable"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable,
		EAssetDocumentHandleResultCardinality::ExactlyOne);
	AddResult(Descriptor, TEXT("package"), EAssetDocumentHandleKind::Package,
		EAssetDocumentHandleAccess::Mutable,
		EAssetDocumentHandleResultCardinality::ExactlyOne);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeGenericScratchCreateDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.scratch.create_by_factory.v1"),
		EAssetDocumentPrimitiveAccess::Scratch,
		{EAssetDocumentRuntimeRequestPhase::Preflight},
		TEXT("UnrealEd"),
		TEXT("Factories/Factory.h"),
		TEXT("UFactory::FactoryCreateNew"),
		TEXT("unrealed.generic-factory.create-transient-scratch.v1"),
		TEXT("UEAssetDoc.GenericProvider.GenericFactory.Scratch"));
	Descriptor.StableDiagnosticCodes.Append({TEXT("FactoryCreate.NoFactory"),
		TEXT("FactoryCreate.AbstractClass"), TEXT("FactoryCreate.BlueprintClassUnsupported"),
		TEXT("FactoryCreate.IncompleteCreation"), TEXT("FactoryCreate.ContextUnsupported"),
		TEXT("FactoryCreate.ContextAmbiguous"), TEXT("FactoryCreate.SubclassUnsupported"),
		TEXT("FactoryCreate.MissingFactoryContext"), TEXT("FactoryCreate.InteractiveFactory")});
	AddArgument(Descriptor, TEXT("asset_class"), EAssetDocumentHandleKind::Class,
		EAssetDocumentHandleAccess::ReadOnly);
	AddArgument(Descriptor, TEXT("factory_context"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly, false);
	AddArgument(Descriptor, TEXT("factory_context_2"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly, false);
	Descriptor.RequiredConsumerIds.Add(TEXT("animation_profile"));
	AddResult(Descriptor, TEXT("scratch"), EAssetDocumentHandleKind::Scratch,
		EAssetDocumentHandleAccess::Mutable,
		EAssetDocumentHandleResultCardinality::ExactlyOne);
	return Descriptor;
}

FAssetDocumentPrimitiveDescriptor MakeGenericScratchCloneDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.scratch.clone_asset.v2"),
		EAssetDocumentPrimitiveAccess::Scratch,
		{EAssetDocumentRuntimeRequestPhase::Preflight},
		TEXT("CoreUObject"),
		TEXT("UObject/UObjectGlobals.h"),
		TEXT("StaticDuplicateObjectEx(FObjectDuplicationParameters);GetTransientPackage"),
		TEXT("framework.scratch.clone_asset.v2"),
		TEXT("UEAssetDoc.GenericProvider.GenericFactory.Scratch"));
	Descriptor.Version = 2;
	Descriptor.StableDiagnosticCodes.Add(TEXT("Scratch.UnsupportedSource"));
	AddArgument(Descriptor, TEXT("source_asset"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly);
	Descriptor.RequiredConsumerIds.Add(TEXT("animation_profile"));
	AddResult(Descriptor, TEXT("scratch"), EAssetDocumentHandleKind::Scratch,
		EAssetDocumentHandleAccess::Mutable,
		EAssetDocumentHandleResultCardinality::ExactlyOne);
	return Descriptor;
}

// Validate's trial target: a transient copy of the target or a new transient asset, issued as Asset
// handles so the Profile's Apply edits it in the Apply phase of a trial transaction, then discards it.
FAssetDocumentPrimitiveDescriptor MakeTrialTargetDescriptor()
{
	FAssetDocumentPrimitiveDescriptor Descriptor = MakeOrdinaryDescriptor(
		TEXT("editor.scratch.trial_target.v1"),
		EAssetDocumentPrimitiveAccess::Mutating,
		{EAssetDocumentRuntimeRequestPhase::Apply},
		TEXT("CoreUObject;UnrealEd"),
		TEXT("UObject/UObjectGlobals.h;Factories/Factory.h"),
		TEXT("StaticDuplicateObjectEx(FObjectDuplicationParameters);UFactory::FactoryCreateNew;GetTransientPackage"),
		TEXT("framework.scratch.trial_target.v1"),
		TEXT("UEAssetDoc.Material.Python"));
	Descriptor.StableDiagnosticCodes.Append({TEXT("Scratch.UnsupportedSource"), TEXT("Scratch.CloneFailed"),
		TEXT("FactoryCreate.NoFactory"), TEXT("FactoryCreate.AbstractClass"), TEXT("FactoryCreate.BlueprintClassUnsupported"),
		TEXT("FactoryCreate.IncompleteCreation"), TEXT("FactoryCreate.ContextUnsupported"),
		TEXT("FactoryCreate.ContextAmbiguous"), TEXT("FactoryCreate.SubclassUnsupported"),
		TEXT("FactoryCreate.MissingFactoryContext"), TEXT("FactoryCreate.InteractiveFactory")});
	AddArgument(Descriptor, TEXT("source_asset"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly, false);
	AddArgument(Descriptor, TEXT("asset_class"), EAssetDocumentHandleKind::Class,
		EAssetDocumentHandleAccess::ReadOnly, false);
	AddArgument(Descriptor, TEXT("factory_context"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly, false);
	AddArgument(Descriptor, TEXT("factory_context_2"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly, false);
	AddResult(Descriptor, TEXT("scratch"), EAssetDocumentHandleKind::Scratch,
		EAssetDocumentHandleAccess::Mutable,
		EAssetDocumentHandleResultCardinality::ExactlyOne);
	AddResult(Descriptor, TEXT("asset_read"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly,
		EAssetDocumentHandleResultCardinality::ExactlyOne);
	AddResult(Descriptor, TEXT("asset_mutable"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable,
		EAssetDocumentHandleResultCardinality::ExactlyOne);
	return Descriptor;
}

// Exact UBlueprint lifecycle (AssetDocumentEditorBlueprintLifecycleHandlers.cpp).
FAssetDocumentPrimitiveDescriptor MakeBlueprintLifecycleDescriptor(int32 Operation)
{
 const TCHAR* Ids[] = {TEXT("editor.blueprint.scratch.clone.v1"), TEXT("editor.blueprint.scratch.create.v1"), TEXT("editor.blueprint.asset.create.v1")};
 const bool bScratch = Operation < 2;
 FAssetDocumentPrimitiveDescriptor D = MakeOrdinaryDescriptor(Ids[Operation],
  bScratch ? EAssetDocumentPrimitiveAccess::Scratch : EAssetDocumentPrimitiveAccess::Mutating,
  {bScratch ? EAssetDocumentRuntimeRequestPhase::Preflight : EAssetDocumentRuntimeRequestPhase::Apply},
  bScratch ? TEXT("UnrealEd") : TEXT("AssetTools;UnrealEd"),
  bScratch ? TEXT("Kismet2/KismetEditorUtilities.h") : TEXT("IAssetTools.h;Factories/BlueprintFactory.h"),
  bScratch ? TEXT("FKismetEditorUtilities::CanCreateBlueprintOfClass") : TEXT("UBlueprintFactory::ParentClass;IAssetTools::CreateAsset"),
  Ids[Operation], TEXT("UEAssetDoc.UBlueprint.Python"));
 D.RequiredConsumerIds = {TEXT("blueprint_profile")};
 D.StableDiagnosticCodes.Append({TEXT("BlueprintLifecycle.InvalidArguments"), TEXT("BlueprintLifecycle.HandleRejected"),
  TEXT("BlueprintLifecycle.TargetClassMismatch"), TEXT("BlueprintLifecycle.InvalidParentClass"), TEXT("BlueprintLifecycle.HandleIssueFailed"),
  TEXT("BlueprintLifecycle.InvalidOutcome"), TEXT("BlueprintLifecycle.TransactionRequired"), TEXT("BlueprintLifecycle.TargetExists"),
  TEXT("BlueprintLifecycle.InvalidTarget"), TEXT("BlueprintLifecycle.FootprintRejected"), TEXT("BlueprintLifecycle.IncompleteCreation"),
  TEXT("BlueprintLifecycle.RegistryMismatch"), TEXT("BlueprintLifecycle.InvalidSkeleton"), TEXT("BlueprintLifecycle.InvalidFactory")});
 if (Operation == 0)
 {
  AddArgument(D, TEXT("source_asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly);
 }
 else
 {
  AddArgument(D, TEXT("asset_class"), EAssetDocumentHandleKind::Class, EAssetDocumentHandleAccess::ReadOnly);
  AddArgument(D, TEXT("parent_class"), EAssetDocumentHandleKind::Class, EAssetDocumentHandleAccess::ReadOnly);
  // Animation Blueprint: the target skeleton (UAnimBlueprintFactory::TargetSkeleton).
  AddArgument(D, TEXT("factory_context"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly, false);
  // Blueprint: the Blueprint type factory (UBlueprintMacroFactory, UBlueprintInterfaceFactory,
  // UBlueprintFunctionLibraryFactory); omitted = UBlueprintFactory (Blueprint Class).
  AddArgument(D, TEXT("factory_class"), EAssetDocumentHandleKind::Class, EAssetDocumentHandleAccess::ReadOnly, false);
 }
 if (bScratch)
 {
  AddResult(D, TEXT("scratch"), EAssetDocumentHandleKind::Scratch, EAssetDocumentHandleAccess::Mutable, EAssetDocumentHandleResultCardinality::ExactlyOne);
 }
 else
 {
  AddResult(D, TEXT("asset_read"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly, EAssetDocumentHandleResultCardinality::ExactlyOne);
  AddResult(D, TEXT("asset_mutable"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable, EAssetDocumentHandleResultCardinality::ExactlyOne);
  AddResult(D, TEXT("package"), EAssetDocumentHandleKind::Package, EAssetDocumentHandleAccess::Mutable, EAssetDocumentHandleResultCardinality::ExactlyOne);
 }
 return D;
}

TArray<FAssetDocumentPrimitiveDescriptor> BuildDescriptors()
{
	TArray<FAssetDocumentPrimitiveDescriptor> Descriptors;
	Descriptors.Reserve(20);
	Descriptors.Add(MakeClassResolveDescriptor());
	Descriptors.Add(MakeReferenceResolveDescriptor());
	Descriptors.Add(MakePropertyInspectEvidenceDescriptor());
	Descriptors.Add(MakePropertyEnumerateDescriptor());
	Descriptors.Add(MakePropertyEnumerateBatchDescriptor());
	Descriptors.Add(MakePropertyReadDescriptor());
	Descriptors.Add(MakePropertyWriteDescriptor());
	Descriptors.Add(MakePropertyWriteBatchDescriptor());
	Descriptors.Add(MakePropertyPreflightDescriptor());
	Descriptors.Add(MakeEnhancedInputCheckDescriptor());
	Descriptors.Add(MakeStructCodecDescriptor());
	Descriptors.Add(MakeEmbeddedCreateDescriptor());
	Descriptors.Add(MakeScratchCloneDescriptor());
	Descriptors.Add(MakeScratchCreateFromClassDescriptor());
	auto ResolveExisting = MakeResolveExistingDescriptor();
 ResolveExisting.RequiredConsumerIds.AddUnique(TEXT("animation_profile"));
 Descriptors.Add(MoveTemp(ResolveExisting));
	Descriptors.Add(MakeFactoryCreateDescriptor());
	Descriptors.Add(MakeRollbackCreatedDescriptor());
	Descriptors.Add(MakeGenericFactoryCreateDescriptor());
	Descriptors.Add(MakeGenericScratchCreateDescriptor());
	Descriptors.Add(MakeGenericScratchCloneDescriptor());
	Descriptors.Add(MakeTrialTargetDescriptor());
 for (int32 Op = 0; Op < 3; ++Op) Descriptors.Add(MakeBlueprintLifecycleDescriptor(Op));
	return Descriptors;
}
} // namespace AssetDocumentEditorPrimitiveCatalog

TConstArrayView<FAssetDocumentPrimitiveDescriptor>
GetAssetDocumentEditorPrimitiveDescriptors()
{
	static const TArray<FAssetDocumentPrimitiveDescriptor> Descriptors =
		AssetDocumentEditorPrimitiveCatalog::BuildDescriptors();
	return Descriptors;
}

TConstArrayView<FAssetDocumentEditorTargetLifecycleCatalogEntry> GetAssetDocumentEditorTargetLifecycleCatalog()
{
 static const FAssetDocumentEditorTargetLifecycleCatalogEntry Rows[] = {
  // Exact UBlueprint: UBlueprintFactory with the document's parent class, or the Blueprint type
  // factory (factory_class: Macro Library / Interface / Function Library); the created asset is
  // removed by the shared rollback_created seed.
  {TEXT("/Script/Engine.Blueprint"), TEXT("blueprint_exact.v1"), TEXT("blueprint_exact.v1"), TEXT("blueprint_factory.v1"),
   {{TEXT("editor.blueprint.scratch.clone.v1"),1u,TEXT("blueprint_profile")},
    {TEXT("editor.blueprint.scratch.create.v1"),1u,TEXT("blueprint_profile")},
    {TEXT("editor.blueprint.asset.create.v1"),1u,TEXT("blueprint_profile")},
    {TEXT("editor.asset.rollback_created.v1"),1u,TEXT("blueprint_profile")}},
   {{TEXT("parent_class"),EAssetDocumentHandleKind::Class,EAssetDocumentHandleAccess::ReadOnly,true},
    {TEXT("factory_class"),EAssetDocumentHandleKind::Class,EAssetDocumentHandleAccess::ReadOnly,false}}, TEXT("/Script/CoreUObject.Class")},
  // Exact UWidgetBlueprint: the same Blueprint lifecycle primitives with UWidgetBlueprintFactory
  // and a UUserWidget parent class (UWidgetBlueprint::GetReparentingRules).
  {TEXT("/Script/UMGEditor.WidgetBlueprint"), TEXT("widget_blueprint.v1"), TEXT("widget_blueprint.v1"), TEXT("blueprint_factory.v1"),
   {{TEXT("editor.blueprint.scratch.clone.v1"),1u,TEXT("blueprint_profile")},
    {TEXT("editor.blueprint.scratch.create.v1"),1u,TEXT("blueprint_profile")},
    {TEXT("editor.blueprint.asset.create.v1"),1u,TEXT("blueprint_profile")},
    {TEXT("editor.asset.rollback_created.v1"),1u,TEXT("blueprint_profile")}},
   {{TEXT("parent_class"),EAssetDocumentHandleKind::Class,EAssetDocumentHandleAccess::ReadOnly,true}}, TEXT("/Script/CoreUObject.Class")},
  // Exact UAnimBlueprint: UAnimBlueprintFactory with parent class and target skeleton
  // (factory_context; omitted for a template); same Blueprint lifecycle primitives and rollback seed.
  {TEXT("/Script/Engine.AnimBlueprint"), TEXT("anim_blueprint_exact.v1"), TEXT("anim_blueprint_exact.v1"), TEXT("anim_blueprint_factory.v1"),
   {{TEXT("editor.blueprint.scratch.clone.v1"),1u,TEXT("blueprint_profile")},
    {TEXT("editor.blueprint.scratch.create.v1"),1u,TEXT("blueprint_profile")},
    {TEXT("editor.blueprint.asset.create.v1"),1u,TEXT("blueprint_profile")},
    {TEXT("editor.asset.rollback_created.v1"),1u,TEXT("blueprint_profile")}},
   {{TEXT("parent_class"),EAssetDocumentHandleKind::Class,EAssetDocumentHandleAccess::ReadOnly,true},
    {TEXT("factory_context"),EAssetDocumentHandleKind::Asset,EAssetDocumentHandleAccess::ReadOnly,false}}, TEXT("/Script/CoreUObject.Class")},
  // Class-neutral: any class with an editor factory (see AssetDocumentEditorGenericFactory.h),
  // including AnimMontage with its Skeleton as the optional factory_context.
  {FString(), AssetDocumentEditorGenericFactoryLifecycleId, AssetDocumentEditorGenericFactoryLifecycleId,
   AssetDocumentEditorGenericFactoryLifecycleId,
   {{TEXT("editor.scratch.clone_asset.v2"),2u,FString()},
    {TEXT("editor.scratch.create_by_factory.v1"),1u,FString()},
    {TEXT("editor.asset.create_by_factory.v2"),2u,FString()},
    {TEXT("editor.asset.rollback_created.v1"),1u,FString()}},
   {}, FString()}};
 return MakeArrayView(Rows);
}

const FAssetDocumentEditorTargetLifecycleCatalogEntry* FindAssetDocumentEditorTargetLifecycle(
 TConstArrayView<FAssetDocumentEditorTargetLifecycleCatalogEntry> Catalog,
 FStringView ExactAssetClassPath,
 FStringView LifecycleId)
{
 if (ExactAssetClassPath.IsEmpty()) return nullptr;
 const FAssetDocumentEditorTargetLifecycleCatalogEntry* Specific = nullptr;
 const FAssetDocumentEditorTargetLifecycleCatalogEntry* Neutral = nullptr;
 for (const FAssetDocumentEditorTargetLifecycleCatalogEntry& Row : Catalog)
 {
  if (Row.IsClassNeutral())
  {
   if (Neutral) return nullptr;
   Neutral = &Row;
  }
  else if (FStringView(Row.ExactAssetClassPath).Equals(ExactAssetClassPath, ESearchCase::CaseSensitive))
  {
   if (Specific) return nullptr;
   Specific = &Row;
  }
 }
 const FAssetDocumentEditorTargetLifecycleCatalogEntry* Selected = Specific ? Specific : Neutral;
 if (Selected && !LifecycleId.IsEmpty() && !FStringView(Selected->LifecycleId).Equals(LifecycleId, ESearchCase::CaseSensitive))
 {
  return nullptr;
 }
 return Selected;
}

const FAssetDocumentEditorTargetLifecycleCatalogEntry* FindAssetDocumentEditorTargetLifecycle(
 FStringView ExactAssetClassPath, FStringView LifecycleId)
{
 return FindAssetDocumentEditorTargetLifecycle(
  GetAssetDocumentEditorTargetLifecycleCatalog(), ExactAssetClassPath, LifecycleId);
}

bool CanAssetDocumentEditorTargetLifecycleCreate(
 const FAssetDocumentEditorTargetLifecycleCatalogEntry& Lifecycle,
 FStringView ExactAssetClassPath)
{
 if (!Lifecycle.IsClassNeutral())
 {
  return FStringView(Lifecycle.ExactAssetClassPath).Equals(ExactAssetClassPath, ESearchCase::CaseSensitive);
 }
 const FString ClassPath(ExactAssetClassPath);
 const UClass* Class = FSoftClassPath(ClassPath).ResolveClass();
 return Class != nullptr && Class->GetPathName() == ClassPath
  && IsAssetDocumentEditorGenericFactoryClassAdmitted(Class);
}
