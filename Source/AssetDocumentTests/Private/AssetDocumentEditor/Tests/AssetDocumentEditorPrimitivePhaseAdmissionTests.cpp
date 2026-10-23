// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorPrimitiveProvider.h"

#include "AssetDocumentEditorGenericProviderTestTypes.h"
#include "AssetDocumentEditorProviderTestSupport.h"
#include "Engine/DataAsset.h"
#include "Misc/AutomationTest.h"
#include "Provider/AssetDocumentEditorValueDto.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorPrimitivePhaseAdmissionTests
{
const FAssetDocumentPrimitiveDescriptor* FindDescriptor(
	const IAssetDocumentPrimitiveProvider& Provider,
	FStringView PrimitiveId)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor :
		Provider.DescribePrimitives())
	{
		if (Descriptor.PrimitiveId == PrimitiveId)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

FAssetDocumentRuntimeValueDto MakeString(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeNull()
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Null;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeObject(
	FAutomationTestBase& Test,
	TArray<FAssetDocumentEditorValueField> Fields)
{
	FAssetDocumentRuntimeValueDto Result;
	FString DiagnosticCode;
	Test.TestTrue(TEXT("phase test arguments have exact shape"),
		TryBuildAssetDocumentEditorValueObject(
			Fields, Result, DiagnosticCode));
	return Result;
}

bool HasExactPhases(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	std::initializer_list<EAssetDocumentRuntimeRequestPhase> Expected)
{
	const TArray<EAssetDocumentRuntimeRequestPhase> Values(Expected);
	return Descriptor.AllowedPhases == Values;
}

bool HasDiagnostic(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	FStringView Code)
{
	return Outcome.Diagnostics.ContainsByPredicate(
		[Code](const FAssetDocumentRuntimeDiagnostic& Diagnostic)
		{
			return Diagnostic.Code == Code;
		});
}

const FAssetDocumentPrimitiveHandleResultSchema* FindResultSchema(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	FStringView Name)
{
	return Descriptor.HandleResultSchemas.FindByPredicate(
		[Name](const FAssetDocumentPrimitiveHandleResultSchema& Schema)
		{
			return Schema.Name == Name;
		});
}

const FAssetDocumentPrimitiveHandleResult* FindHandleResult(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	FStringView Name)
{
	return Outcome.HandleResults.FindByPredicate(
		[Name](const FAssetDocumentPrimitiveHandleResult& Result)
		{
			return Result.Name == Name;
		});
}

FAssetDocumentPrimitiveOutcome Invoke(
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	EAssetDocumentRuntimeRequestPhase Phase,
	const FAssetDocumentRuntimeValueDto& Arguments)
{
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(
			Descriptor, Context);
	Invocation.Phase = Phase;
	Invocation.ValueArguments = Arguments;
	return Provider.Invoke(Invocation, Handles, nullptr);
}

void TestCommonDescriptorShape(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	FStringView PrimaryOperationId)
{
	Test.TestEqual(TEXT("descriptor remains read-only"),
		Descriptor.Access, EAssetDocumentPrimitiveAccess::ReadOnly);
	Test.TestFalse(TEXT("read-only descriptor needs no transaction"),
		Descriptor.bRequiresActiveTransaction);
	Test.TestEqual(TEXT("descriptor version remains v1"),
		Descriptor.Version, 1u);
	Test.TestEqual(TEXT("descriptor provider remains editor"),
		Descriptor.ProviderId, FString(TEXT("editor")));
	Test.TestEqual(TEXT("descriptor owner remains editor"),
		Descriptor.Owner, EAssetDocumentPrimitiveOwner::Editor);
	Test.TestEqual(TEXT("descriptor remains game-thread bound"),
		Descriptor.ExecutionThread, EAssetDocumentExecutionThread::GameThread);
	Test.TestEqual(TEXT("descriptor primary operation remains exact"),
		Descriptor.Admission.PrimaryOperationId,
		FString(PrimaryOperationId));
	Test.TestEqual(TEXT("descriptor input schema remains exact"),
		Descriptor.InputSchemaId, Descriptor.PrimitiveId + TEXT(".input.v1"));
	Test.TestEqual(TEXT("descriptor output schema remains exact"),
		Descriptor.OutputSchemaId, Descriptor.PrimitiveId + TEXT(".output.v1"));
	Test.TestTrue(TEXT("read-only descriptor has no footprint schema"),
		Descriptor.MutationFootprintSchemaId.IsEmpty());
	Test.TestTrue(TEXT("stable diagnostic contract remains exact"),
		Descriptor.StableDiagnosticCodes
			== TArray<FString>({
				TEXT("AD.RUNTIME.PRIMITIVE_FAILED"),
				TEXT("AD.RUNTIME.PRIMITIVE_UNAVAILABLE")}));
	Test.TestTrue(TEXT("required consumer contract remains exact"),
		Descriptor.RequiredConsumerIds
			== ((Descriptor.PrimitiveId == TEXT("editor.class.resolve_canonical.v1")
				|| Descriptor.PrimitiveId == TEXT("editor.asset.resolve_existing.v1")
				|| Descriptor.PrimitiveId == TEXT("editor.property.enumerate_admitted.v1"))
				? TArray<FString>({TEXT("route.generic.v1"), TEXT("animation_profile")})
				: TArray<FString>({TEXT("route.generic.v1")})));
}

void TestOrdinaryAuthorityEvidence(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	FStringView PublicModule,
	FStringView PublicHeader,
	FStringView AuthoritativeApi,
	FStringView PrimaryOperationId,
	FStringView VerificationTestId)
{
	const FAssetDocumentPrimitiveAdmissionEvidence& Evidence =
		Descriptor.Admission;
	const FString ExpectedFingerprint = FString::Printf(
		TEXT("ue-5.7.4|editor|%.*s|%.*s"),
		PrimaryOperationId.Len(), PrimaryOperationId.GetData(),
		AuthoritativeApi.Len(), AuthoritativeApi.GetData());
	Test.TestTrue(TEXT("ordinary authority evidence remains exact"),
		Evidence.ContractVersion == AssetDocumentRuntimeContractVersion
		&& Evidence.Disposition
			== EAssetDocumentPrimitiveDisposition::OrdinaryHelper
		&& Evidence.AuthorityPath
			== EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper
		&& Evidence.ExactEngineVersion == TEXT("5.7.4")
		&& Evidence.PhysicalOwner == EAssetDocumentPrimitiveOwner::Editor
		&& Evidence.PublicModule == PublicModule
		&& Evidence.PublicHeader == PublicHeader
		&& Evidence.ExportedAuthoritativeApi == AuthoritativeApi
		&& Evidence.AuthorityContextId
			== TEXT("assetdocument.editor.game-thread.editor-ready.v1")
		&& Evidence.PrimaryOperationId == PrimaryOperationId
		&& Evidence.IdentityLifetimeContractId
			== TEXT("identity.request-transaction-revision-bound.v1")
		&& Evidence.TransactionContractId
			== TEXT("transaction.descriptor-governed.v1")
		&& Evidence.RollbackContractId
			== TEXT("rollback.atomic-owner-governed.v1")
		&& Evidence.CompletionContractId
			== Descriptor.PrimitiveId + TEXT(".completion.v1")
		&& Evidence.SaveReloadEvidenceId
			== Descriptor.PrimitiveId + TEXT(".test-owned-save-reload.v1")
		&& Evidence.FailureInjectionTestIds
			== TArray<FString>({
				Descriptor.PrimitiveId + TEXT(".failure-injection.v1")})
		&& Evidence.VerificationTestIds
			== TArray<FString>({FString(VerificationTestId)})
		&& Evidence.SourceMarker.IsEmpty()
		&& Evidence.RawLifecycleAllowlistId.IsEmpty()
		&& Evidence.GovernanceId.IsEmpty()
		&& Evidence.IndependentCandidateReviewId.IsEmpty()
		&& Evidence.IndependentBridgeReviewId.IsEmpty()
		&& Evidence.HelperReplacementCondition.IsEmpty()
		&& Evidence.RemovalCondition.IsEmpty()
		&& Evidence.AuthorityFingerprint == ExpectedFingerprint);
}

void TestHandleSchemas(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Class,
	const FAssetDocumentPrimitiveDescriptor& Properties,
	const FAssetDocumentPrimitiveDescriptor& Existing)
{
	const FAssetDocumentPrimitiveHandleResultSchema* ClassResult =
		FindResultSchema(Class, TEXT("class"));
	Test.TestTrue(TEXT("class schema remains exact"),
		Class.AllowedHandleKinds
			== TArray<EAssetDocumentHandleKind>({
				EAssetDocumentHandleKind::Class})
		&& Class.HandleArgumentSchemas.IsEmpty()
		&& Class.HandleResultSchemas.Num() == 1
		&& ClassResult != nullptr
		&& ClassResult->Kind == EAssetDocumentHandleKind::Class
		&& ClassResult->GrantedAccess == EAssetDocumentHandleAccess::ReadOnly
		&& ClassResult->Cardinality
			== EAssetDocumentHandleResultCardinality::ExactlyOne);

	const FAssetDocumentPrimitiveHandleResultSchema* PropertyResult =
		FindResultSchema(Properties, TEXT("properties"));
	Test.TestTrue(TEXT("property enumeration schema remains exact"),
		Properties.AllowedHandleKinds
			== TArray<EAssetDocumentHandleKind>({
				EAssetDocumentHandleKind::Property})
		&& Properties.HandleArgumentSchemas.IsEmpty()
		&& Properties.HandleResultSchemas.Num() == 1
		&& PropertyResult != nullptr
		&& PropertyResult->Kind == EAssetDocumentHandleKind::Property
		&& PropertyResult->GrantedAccess == EAssetDocumentHandleAccess::ReadOnly
		&& PropertyResult->Cardinality
			== EAssetDocumentHandleResultCardinality::ZeroOrMore);

	Test.TestTrue(TEXT("resolve-existing result schemas remain exact"),
		Existing.AllowedHandleKinds
			== TArray<EAssetDocumentHandleKind>({
				EAssetDocumentHandleKind::Asset,
				EAssetDocumentHandleKind::Package})
		&& Existing.HandleArgumentSchemas.IsEmpty()
		&& Existing.HandleResultSchemas.Num() == 3
		&& FindResultSchema(Existing, TEXT("asset_read")) != nullptr
		&& FindResultSchema(Existing, TEXT("asset_read"))->Kind
			== EAssetDocumentHandleKind::Asset
		&& FindResultSchema(Existing, TEXT("asset_read"))->GrantedAccess
			== EAssetDocumentHandleAccess::ReadOnly
		&& FindResultSchema(Existing, TEXT("asset_read"))->Cardinality
			== EAssetDocumentHandleResultCardinality::ZeroOrOne
		&& FindResultSchema(Existing, TEXT("asset_mutable")) != nullptr
		&& FindResultSchema(Existing, TEXT("asset_mutable"))->Kind
			== EAssetDocumentHandleKind::Asset
		&& FindResultSchema(Existing, TEXT("asset_mutable"))->GrantedAccess
			== EAssetDocumentHandleAccess::Mutable
		&& FindResultSchema(Existing, TEXT("asset_mutable"))->Cardinality
			== EAssetDocumentHandleResultCardinality::ZeroOrOne
		&& FindResultSchema(Existing, TEXT("package")) != nullptr
		&& FindResultSchema(Existing, TEXT("package"))->Kind
			== EAssetDocumentHandleKind::Package
		&& FindResultSchema(Existing, TEXT("package"))->GrantedAccess
			== EAssetDocumentHandleAccess::ReadOnly
		&& FindResultSchema(Existing, TEXT("package"))->Cardinality
			== EAssetDocumentHandleResultCardinality::ZeroOrOne);
}

void TestClassApply(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FAssetDocumentRuntimeValueDto Arguments = MakeObject(
		Test,
		{
			{TEXT("class"), MakeString(UDataAsset::StaticClass()->GetPathName())},
			{TEXT("expected_base"), MakeNull()},
		});
	const FAssetDocumentPrimitiveOutcome Outcome = Invoke(
		Provider, Descriptor,
		EAssetDocumentRuntimeRequestPhase::Apply, Arguments);
	Test.TestEqual(TEXT("class resolve executes in Apply"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	Test.TestEqual(TEXT("class resolve issues one handle group"),
		Outcome.HandleResults.Num(), 1);
}

void TestPropertyPhases(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FAssetDocumentRuntimeValueDto Arguments = MakeObject(
		Test,
		{{TEXT("exact_asset_class_path"),
			MakeString(UAssetDocumentEditorCodecTestObject::StaticClass()
				->GetPathName())}});
	const EAssetDocumentRuntimeRequestPhase Phases[] = {
		EAssetDocumentRuntimeRequestPhase::Preflight,
		EAssetDocumentRuntimeRequestPhase::Apply,
		EAssetDocumentRuntimeRequestPhase::Repair,
		EAssetDocumentRuntimeRequestPhase::Verify,
	};
	for (const EAssetDocumentRuntimeRequestPhase Phase : Phases)
	{
		const FAssetDocumentPrimitiveOutcome Outcome =
			Invoke(Provider, Descriptor, Phase, Arguments);
		Test.TestEqual(TEXT("property enumeration executes in rebind phase"),
			Outcome.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	}
}

void TestResolveExistingPhases(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FAssetDocumentRuntimeValueDto Arguments = MakeObject(
		Test,
		{
			{TEXT("path"), MakeString(
				TEXT("/Game/AssetDocumentTests/PhaseRebind/Missing.Missing"))},
			{TEXT("expected_class"), MakeNull()},
		});
	const EAssetDocumentRuntimeRequestPhase Phases[] = {
		EAssetDocumentRuntimeRequestPhase::Apply,
		EAssetDocumentRuntimeRequestPhase::Repair,
		EAssetDocumentRuntimeRequestPhase::Verify,
	};
	for (const EAssetDocumentRuntimeRequestPhase Phase : Phases)
	{
		const FAssetDocumentPrimitiveOutcome Outcome =
			Invoke(Provider, Descriptor, Phase, Arguments);
		Test.TestEqual(TEXT("missing-target resolution executes in rebind phase"),
			Outcome.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		Test.TestEqual(TEXT("missing target returns three result groups"),
			Outcome.HandleResults.Num(), 3);
		for (const TCHAR* Name : {TEXT("asset_read"), TEXT("asset_mutable"),
			TEXT("package")})
		{
			const FAssetDocumentPrimitiveHandleResult* Result =
				FindHandleResult(Outcome, Name);
			if (Test.TestNotNull(
				FString::Printf(TEXT("%s result exists"), Name), Result))
			{
				Test.TestTrue(
					FString::Printf(TEXT("%s result has no live handles"), Name),
					Result->Handles.IsEmpty());
			}
		}
	}
}

void TestForbiddenPhases(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	TConstArrayView<const FAssetDocumentPrimitiveDescriptor*> Descriptors)
{
	FAssetDocumentRuntimeValueDto Empty;
	Empty.Kind = EAssetDocumentRuntimeValueKind::Object;
	const EAssetDocumentRuntimeRequestPhase Phases[] = {
		EAssetDocumentRuntimeRequestPhase::Persist,
		EAssetDocumentRuntimeRequestPhase::Complete,
		EAssetDocumentRuntimeRequestPhase::Failed,
	};
	for (const FAssetDocumentPrimitiveDescriptor* Descriptor : Descriptors)
	{
		for (const EAssetDocumentRuntimeRequestPhase Phase : Phases)
		{
			const FAssetDocumentPrimitiveOutcome Outcome =
				Invoke(Provider, *Descriptor, Phase, Empty);
			Test.TestTrue(TEXT("forbidden phase fails at invocation identity"),
				HasDiagnostic(
					Outcome, TEXT("PrimitiveInvocation.InvalidIdentity")));
		}
	}
}
} // namespace AssetDocumentEditorPrimitivePhaseAdmissionTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentEditorPrimitivePhaseAdmissionTest,
	"UEAssetDoc.GenericProvider.Shell.CutoverPhaseRebindAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentEditorPrimitivePhaseAdmissionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorPrimitivePhaseAdmissionTests;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Class = FindDescriptor(
		*Provider, TEXT("editor.class.resolve_canonical.v1"));
	const FAssetDocumentPrimitiveDescriptor* Properties = FindDescriptor(
		*Provider, TEXT("editor.property.enumerate_admitted.v1"));
	const FAssetDocumentPrimitiveDescriptor* Existing = FindDescriptor(
		*Provider, TEXT("editor.asset.resolve_existing.v1"));
	const FAssetDocumentPrimitiveDescriptor* Factory = FindDescriptor(
		*Provider, TEXT("editor.asset.create_by_factory.v1"));
	if (!TestNotNull(TEXT("class descriptor exists"), Class)
		|| !TestNotNull(TEXT("property descriptor exists"), Properties)
		|| !TestNotNull(TEXT("existing descriptor exists"), Existing)
		|| !TestNotNull(TEXT("factory descriptor exists"), Factory))
	{
		return false;
	}
	TestTrue(TEXT("class phases match cutover rebind contract"),
		HasExactPhases(*Class, {
			EAssetDocumentRuntimeRequestPhase::Validate,
			EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply}));
	TestTrue(TEXT("property phases match cutover rebind contract"),
		HasExactPhases(*Properties, {
			EAssetDocumentRuntimeRequestPhase::Validate,
			EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply,
			EAssetDocumentRuntimeRequestPhase::Repair,
			EAssetDocumentRuntimeRequestPhase::Verify}));
	TestTrue(TEXT("existing-target phases match cutover rebind contract"),
		HasExactPhases(*Existing, {
			EAssetDocumentRuntimeRequestPhase::Validate,
			EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply,
			EAssetDocumentRuntimeRequestPhase::Repair,
			EAssetDocumentRuntimeRequestPhase::Verify}));
	TestTrue(TEXT("factory remains Apply-only"),
		HasExactPhases(*Factory, {EAssetDocumentRuntimeRequestPhase::Apply}));
	TestEqual(TEXT("factory remains mutating"), Factory->Access,
		EAssetDocumentPrimitiveAccess::Mutating);
	TestCommonDescriptorShape(*this, *Class,
		TEXT("coreuobject.class.resolve-canonical.v1"));
	TestCommonDescriptorShape(*this, *Properties,
		TEXT("coreuobject.property.enumerate-admitted.v1"));
	TestCommonDescriptorShape(*this, *Existing,
		TEXT("assetregistry.asset.resolve-existing.v1"));
	TestOrdinaryAuthorityEvidence(*this, *Class,
		TEXT("CoreUObject"),
		TEXT("UObject/SoftObjectPath.h;UObject/Class.h"),
		TEXT("FSoftClassPath::TryLoadClass<UObject>;UStruct::IsChildOf"),
		TEXT("coreuobject.class.resolve-canonical.v1"),
		TEXT("UEAssetDoc.GenericProvider.ClassReference.CanonicalClassResolve"));
	TestOrdinaryAuthorityEvidence(*this, *Properties,
		TEXT("CoreUObject"),
		TEXT("UObject/UnrealType.h;UObject/PropertyAccessUtil.h"),
		TEXT("TFieldIterator<FProperty>;PropertyAccessUtil::CanGetPropertyValue;PropertyAccessUtil::CanSetPropertyValue"),
		TEXT("coreuobject.property.enumerate-admitted.v1"),
		TEXT("UEAssetDoc.GenericProvider.Property.DeterministicEnumeration"));
	TestOrdinaryAuthorityEvidence(*this, *Existing,
		TEXT("AssetRegistry;CoreUObject"),
		TEXT("AssetRegistry/IAssetRegistry.h;AssetRegistry/AssetData.h"),
		TEXT("IAssetRegistry::GetAssetByObjectPath;FAssetData::GetAsset"),
		TEXT("assetregistry.asset.resolve-existing.v1"),
		TEXT("UEAssetDoc.GenericProvider.ResolveExisting.ReadOnly"));
	TestHandleSchemas(*this, *Class, *Properties, *Existing);
	TestClassApply(*this, *Provider, *Class);
	TestPropertyPhases(*this, *Provider, *Properties);
	TestResolveExistingPhases(*this, *Provider, *Existing);
	const FAssetDocumentPrimitiveDescriptor* Descriptors[] = {
		Class, Properties, Existing};
	TestForbiddenPhases(*this, *Provider, Descriptors);
	return true;
}
#endif
