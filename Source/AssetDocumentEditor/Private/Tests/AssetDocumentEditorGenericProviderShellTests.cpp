// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorPrimitiveProvider.h"

#include "AssetDocumentRollbackAction.h"
#include "AssetDocumentRuntimeModule.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Provider/AssetDocumentEditorPrimitiveHandler.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"
#include "AssetDocumentPluginPaths.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorGenericProviderShellTests
{
const FAssetDocumentPrimitiveDescriptor* FindDescriptor(
	TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors,
	const TCHAR* PrimitiveId)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
	{
		if (Descriptor.PrimitiveId == PrimitiveId)
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

bool HasArgument(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const TCHAR* Name,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access)
{
	for (const FAssetDocumentPrimitiveHandleArgumentSchema& Schema : Descriptor.HandleArgumentSchemas)
	{
		if (Schema.Name == Name && Schema.Kind == Kind && Schema.RequiredAccess == Access)
		{
			return true;
		}
	}
	return false;
}

bool HasResult(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const TCHAR* Name,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access,
	EAssetDocumentHandleResultCardinality Cardinality)
{
	for (const FAssetDocumentPrimitiveHandleResultSchema& Schema : Descriptor.HandleResultSchemas)
	{
		if (Schema.Name == Name && Schema.Kind == Kind
			&& Schema.GrantedAccess == Access && Schema.Cardinality == Cardinality)
		{
			return true;
		}
	}
	return false;
}

bool HasDiagnostic(const FAssetDocumentPrimitiveOutcome& Outcome, const TCHAR* Code)
{
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic : Outcome.Diagnostics)
	{
		if (Diagnostic.Code == Code)
		{
			return true;
		}
	}
	return false;
}

FAssetDocumentPrimitiveInvocation MakeInvocation(
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = Descriptor.PrimitiveId;
	Invocation.Version = Descriptor.Version;
	Invocation.RequestId = FGuid::NewGuid();
	Invocation.RuntimeRevision = 1;
	Invocation.Phase = Descriptor.AllowedPhases[0];
	Invocation.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
	return Invocation;
}

FAssetDocumentOpaqueHandle MakeHandle(
	const FGuid& RequestId,
	const FGuid& TransactionId,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access,
	uint32 Slot)
{
	FAssetDocumentOpaqueHandle Handle;
	Handle.RequestId = RequestId;
	Handle.TransactionId = TransactionId;
	Handle.RuntimeRevision = 1;
	Handle.ProviderId = TEXT("editor");
	Handle.Slot = Slot;
	Handle.Generation = 1;
	Handle.Kind = Kind;
	Handle.Access = Access;
	return Handle;
}

class FAlwaysStaleHandleResolver final : public IAssetDocumentHandleResolver
{
public:
	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind,
		EAssetDocumentHandleAccess,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&&) override
	{
		FAssetDocumentIssuedHandleResult Result;
		FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Code = TEXT("Test.UnexpectedIssue");
		return Result;
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle&,
		const FAssetDocumentHandleValidationContext&) override
	{
		++AcquireCount;
		FAssetDocumentVerifiedLeaseResult Result;
		FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Code = TEXT("HandleMetadata.StaleHandle");
		return Result;
	}

	virtual FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation&) override
	{
		return {};
	}

	int32 AcquireCount = 0;
};

FAssetDocumentContractValidationResult MakeValidContractResult()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

class FMatchingAtomicTransaction final : public IAssetDocumentAtomicTransaction
{
public:
	explicit FMatchingAtomicTransaction(const FAssetDocumentPrimitiveInvocation& Invocation)
	{
		Token.RequestId = Invocation.RequestId;
		Token.TransactionId = Invocation.TransactionId;
		Token.RuntimeRevision = Invocation.RuntimeRevision;
		RollbackActionEnlistmentResult.bValid = true;
		PostRollbackFinalizerEnlistmentResult.bValid = true;
	}

	virtual FGuid GetTransactionId() const override
	{
		return Token.TransactionId;
	}

	virtual EAssetDocumentAtomicState GetState() const override
	{
		return EAssetDocumentAtomicState::Live;
	}

	virtual const FAssetDocumentMutationToken& GetMutationToken() const override
	{
		return Token;
	}

	virtual FAssetDocumentContractValidationResult EnlistFootprint(
		TConstArrayView<FAssetDocumentMutationFootprint>) override
	{
		return MakeValidContractResult();
	}

	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() override
	{
		return MakeValidContractResult();
	}

	virtual FAssetDocumentContractValidationResult Commit(
		const FAssetDocumentDurableCompletionReceipt&) override
	{
		return MakeValidContractResult();
	}

	virtual FAssetDocumentContractValidationResult Rollback(FStringView) override
	{
		return MakeValidContractResult();
	}

	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(
		TSharedRef<IAssetDocumentRollbackAction> Action) override
	{
		EnlistedRollbackActionComponentIds.Add(
			FString(Action->GetRollbackComponentId()));
		return RollbackActionEnlistmentResult;
	}

	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer>) override
	{
		return PostRollbackFinalizerEnlistmentResult;
	}

	TArray<FString> EnlistedRollbackActionComponentIds;
	FAssetDocumentContractValidationResult RollbackActionEnlistmentResult;
	FAssetDocumentContractValidationResult PostRollbackFinalizerEnlistmentResult;

private:
	FAssetDocumentMutationToken Token;
};

void TestDescriptorCatalog(FAutomationTestBase& Test)
{
	static const TCHAR* ExactPrimitiveIds[] = {
		TEXT("editor.class.resolve_canonical.v1"),
		TEXT("editor.reference.resolve_asset.v1"),
		TEXT("editor.property.inspect_evidence.v1"),
		TEXT("editor.property.enumerate_admitted.v1"),
		TEXT("editor.property.enumerate_admitted_batch.v1"),
		TEXT("editor.property.read_admitted.v1"),
		TEXT("editor.property.write_admitted.v1"),
		TEXT("editor.property.write_admitted_batch.v1"),
		TEXT("editor.property.preflight_admitted.v1"),
		TEXT("editor.enhanced_input.check_domain.v1"),
		TEXT("editor.fragment.struct_codec.v1"),
		TEXT("editor.fragment.embedded_object_create.v1"),
		TEXT("editor.scratch.clone_asset.v1"),
		TEXT("editor.scratch.create_from_class.v1"),
		TEXT("editor.asset.resolve_existing.v1"),
		TEXT("editor.asset.create_by_factory.v1"),
		TEXT("editor.asset.rollback_created.v1"),
		TEXT("editor.asset.create_by_factory.v2"),
		TEXT("editor.scratch.create_by_factory.v1"),
		TEXT("editor.scratch.clone_asset.v2"),
		TEXT("editor.scratch.trial_target.v1"),
  TEXT("editor.blueprint.scratch.clone.v1"),
  TEXT("editor.blueprint.scratch.create.v1"),
  TEXT("editor.blueprint.asset.create.v1"),
	};
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	Test.TestEqual(TEXT("provider id"), Provider->GetProviderId(), FString(TEXT("editor")));
	const TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors =
		Provider->DescribePrimitives();
	const TConstArrayView<FAssetDocumentEditorPrimitiveBinding> Bindings =
		GetAssetDocumentEditorPrimitiveBindings();
	Test.TestEqual(TEXT("exact descriptors"), Descriptors.Num(),
		static_cast<int32>(UE_ARRAY_COUNT(ExactPrimitiveIds)));
	Test.TestEqual(TEXT("exact bindings"), Bindings.Num(),
		static_cast<int32>(UE_ARRAY_COUNT(ExactPrimitiveIds)));
	const TSet<FString> AnimationConsumers = {
  TEXT("editor.asset.resolve_existing.v1"),
		TEXT("editor.property.enumerate_admitted.v1"),
		TEXT("editor.asset.rollback_created.v1"),
		TEXT("editor.asset.create_by_factory.v2"),
		TEXT("editor.scratch.create_by_factory.v1"),
		TEXT("editor.scratch.clone_asset.v2"),
		TEXT("editor.class.resolve_canonical.v1"),
		TEXT("editor.reference.resolve_asset.v1"),
		TEXT("editor.property.enumerate_admitted_batch.v1"),
		TEXT("editor.property.preflight_admitted.v1"),
		TEXT("editor.property.read_admitted.v1"),
		TEXT("editor.property.write_admitted_batch.v1"),
	};
	TSet<FString> Ids;
	for (const TCHAR* ExactPrimitiveId : ExactPrimitiveIds)
	{
		const FAssetDocumentPrimitiveDescriptor* Descriptor =
			FindDescriptor(Descriptors, ExactPrimitiveId);
		Test.TestNotNull(*FString::Printf(TEXT("%s descriptor exists"), ExactPrimitiveId), Descriptor);
		const FAssetDocumentEditorPrimitiveBinding* MatchingBinding = nullptr;
		int32 MatchingBindingCount = 0;
		for (const FAssetDocumentEditorPrimitiveBinding& Binding : Bindings)
		{
			if (Binding.PrimitiveId == ExactPrimitiveId)
			{
				MatchingBinding = &Binding;
				++MatchingBindingCount;
			}
		}
		Test.TestEqual(
			*FString::Printf(TEXT("%s has exactly one binding"), ExactPrimitiveId),
			MatchingBindingCount,
			1);
		if (Descriptor != nullptr)
		{
			Test.TestTrue(*FString::Printf(TEXT("%s validates"), ExactPrimitiveId),
				ValidateAssetDocumentPrimitiveDescriptor(*Descriptor).bValid);
			Test.TestEqual(*FString::Printf(TEXT("%s version"), ExactPrimitiveId),
				Descriptor->Version, FString(ExactPrimitiveId).EndsWith(TEXT(".v2")) ? 2u : 1u);
			Test.TestEqual(*FString::Printf(TEXT("%s provider"), ExactPrimitiveId),
				Descriptor->ProviderId, FString(TEXT("editor")));
			Test.TestEqual(*FString::Printf(TEXT("%s owner"), ExactPrimitiveId),
				Descriptor->Owner, EAssetDocumentPrimitiveOwner::Editor);
			Test.TestEqual(*FString::Printf(TEXT("%s execution thread"), ExactPrimitiveId),
				Descriptor->ExecutionThread, EAssetDocumentExecutionThread::GameThread);
			Test.TestEqual(*FString::Printf(TEXT("%s exact consumer"), ExactPrimitiveId),
				Descriptor->RequiredConsumerIds, FString(ExactPrimitiveId).StartsWith(TEXT("editor.blueprint.")) ? TArray<FString>({TEXT("blueprint_profile")}) : AnimationConsumers.Contains(ExactPrimitiveId)
					? TArray<FString>({TEXT("route.generic.v1"), TEXT("animation_profile")})
					: TArray<FString>({TEXT("route.generic.v1")}));
			Test.TestEqual(TEXT("all descriptor evidence binds stock 5.7.4"),
				Descriptor->Admission.ExactEngineVersion, FString(TEXT("5.7.4")));
			Test.TestTrue(TEXT("all fingerprints bind stock 5.7.4"),
				Descriptor->Admission.AuthorityFingerprint.StartsWith(TEXT("ue-5.7.4|editor|")));
			Ids.Add(Descriptor->PrimitiveId);
		}
		if (MatchingBinding != nullptr)
		{
			Test.TestEqual(*FString::Printf(TEXT("%s binding version"), ExactPrimitiveId),
				MatchingBinding->Version, FString(ExactPrimitiveId).EndsWith(TEXT(".v2")) ? 2u : 1u);
			Test.TestTrue(*FString::Printf(TEXT("%s binding handler"), ExactPrimitiveId),
				MatchingBinding->Handler != nullptr);
		}
	}
	Test.TestEqual(TEXT("descriptor ids are unique"), Ids.Num(),
		static_cast<int32>(UE_ARRAY_COUNT(ExactPrimitiveIds)));
	for (const FAssetDocumentEditorPrimitiveBinding& Binding : Bindings)
	{
		Test.TestNotNull(
			*FString::Printf(TEXT("%.*s binding has descriptor"),
				Binding.PrimitiveId.Len(), Binding.PrimitiveId.GetData()),
			FindDescriptor(Descriptors, *FString(Binding.PrimitiveId)));
	}
}

void TestFailedInvocation(
	FAutomationTestBase& Test,
	const TCHAR* Label,
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentAtomicTransaction* ActiveTransaction = nullptr)
{
	FAlwaysStaleHandleResolver Resolver;
	const FAssetDocumentPrimitiveOutcome Outcome =
		CreateAssetDocumentEditorPrimitiveProvider()->Invoke(
			Invocation, Resolver, ActiveTransaction);
	Test.TestEqual(Label, Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestTrue(*FString::Printf(TEXT("%s has stable diagnostic"), Label),
		!Outcome.Diagnostics.IsEmpty() && !Outcome.Diagnostics[0].Code.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("%s leaks no handles"), Label),
		Outcome.HandleResults.IsEmpty());
	Test.TestTrue(*FString::Printf(TEXT("%s leaks no changed set"), Label),
		Outcome.ChangedSet.IsEmpty());
}

bool LoadProviderSource(FString& OutSource)
{
	const FString ProviderRoot = FPaths::Combine(GetAssetDocumentPluginBaseDir(), TEXT("Source/AssetDocumentEditor/Private/Provider"));
	TArray<FString> SourceFiles;
	IFileManager::Get().FindFilesRecursive(SourceFiles, *ProviderRoot, TEXT("*.h"), true, false);
	IFileManager::Get().FindFilesRecursive(SourceFiles, *ProviderRoot, TEXT("*.cpp"), true, false);
	SourceFiles.Sort();
	for (const FString& SourceFile : SourceFiles)
	{
		FString Source;
		if (!FFileHelper::LoadFileToString(Source, *SourceFile))
		{
			return false;
		}
		OutSource += Source;
	}
	return !SourceFiles.IsEmpty();
}
}

#define ASSETDOCUMENT_GENERIC_PROVIDER_TEST(ClassName, TestName) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST( \
		ClassName, TestName, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

ASSETDOCUMENT_GENERIC_PROVIDER_TEST(
	FAssetDocumentGenericProviderDescriptorCatalogTest,
	"UEAssetDoc.GenericProvider.Shell.DescriptorCatalog")

bool FAssetDocumentGenericProviderDescriptorCatalogTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	AssetDocumentEditorGenericProviderShellTests::TestDescriptorCatalog(*this);
	return true;
}

ASSETDOCUMENT_GENERIC_PROVIDER_TEST(
	FAssetDocumentGenericProviderDormantFactoryTest,
	"UEAssetDoc.GenericProvider.Shell.DormantFactory")

bool FAssetDocumentGenericProviderDormantFactoryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	IAssetDocumentRuntimeModule& Module =
		FModuleManager::LoadModuleChecked<IAssetDocumentRuntimeModule>(TEXT("AssetDocumentEditor"));
	const bool bRegisteredBeforeFactory = Module.HasActiveRuntimeRegistration();
	const TSharedRef<IAssetDocumentPrimitiveProvider> First =
		CreateAssetDocumentEditorPrimitiveProvider();
	const TSharedRef<IAssetDocumentPrimitiveProvider> Second =
		CreateAssetDocumentEditorPrimitiveProvider();
	TestFalse(TEXT("module is dormant before factory"), bRegisteredBeforeFactory);
	TestFalse(TEXT("factory performs no runtime registration"),
		Module.HasActiveRuntimeRegistration());
	TestTrue(TEXT("factory returns distinct dormant owners"), &First.Get() != &Second.Get());
	TestEqual(TEXT("factory reports available provider"),
		First->InspectAvailability().State, EAssetDocumentProviderAvailabilityState::Available);
	return true;
}

ASSETDOCUMENT_GENERIC_PROVIDER_TEST(
	FAssetDocumentGenericProviderInvalidInvocationTest,
	"UEAssetDoc.GenericProvider.Shell.InvalidInvocation")

bool FAssetDocumentGenericProviderInvalidInvocationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericProviderShellTests;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors =
		Provider->DescribePrimitives();

	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = TEXT("editor.unknown.v1");
	Invocation.Version = 1;
	Invocation.RequestId = FGuid::NewGuid();
	Invocation.TransactionId = FGuid::NewGuid();
	Invocation.RuntimeRevision = 1;
	Invocation.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
	TestFailedInvocation(*this, TEXT("unknown id fails closed"), Invocation);

	const FAssetDocumentPrimitiveDescriptor* ClassDescriptor =
		FindDescriptor(Descriptors, TEXT("editor.class.resolve_canonical.v1"));
	const FAssetDocumentPrimitiveDescriptor* MutatingDescriptor =
		FindDescriptor(Descriptors, TEXT("editor.property.write_admitted.v1"));
	TestNotNull(TEXT("class descriptor"), ClassDescriptor);
	TestNotNull(TEXT("mutating descriptor"), MutatingDescriptor);
	if (ClassDescriptor != nullptr)
	{
		FAssetDocumentPrimitiveInvocation BadVersion = MakeInvocation(*ClassDescriptor);
		++BadVersion.Version;
		TestFailedInvocation(*this, TEXT("bad version fails closed"), BadVersion);

		FAssetDocumentPrimitiveInvocation BadPhase = MakeInvocation(*ClassDescriptor);
		BadPhase.Phase = EAssetDocumentRuntimeRequestPhase::Persist;
		TestFailedInvocation(*this, TEXT("bad phase fails closed"), BadPhase);

		FAssetDocumentPrimitiveOutcome InvalidOutcome;
		InvalidOutcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
		InvalidOutcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
		InvalidOutcome.HandleResults.AddDefaulted();
		const FAssetDocumentContractValidationResult OutcomeValidation =
			ValidateAssetDocumentPrimitiveOutcome(
				MakeInvocation(*ClassDescriptor), *ClassDescriptor, InvalidOutcome);
		TestFalse(TEXT("invalid handler outcome is rejected"), OutcomeValidation.bValid);
		TestTrue(TEXT("invalid outcome has stable diagnostic"),
			!OutcomeValidation.Diagnostics.IsEmpty()
			&& OutcomeValidation.Diagnostics[0].Code
				== TEXT("PrimitiveOutcome.HandleLeakOnFailure"));
	}
	if (MutatingDescriptor != nullptr)
	{
		FAssetDocumentPrimitiveInvocation MissingTransaction =
			MakeInvocation(*MutatingDescriptor);
		TestFailedInvocation(
			*this, TEXT("missing transaction identity fails closed"), MissingTransaction);

		FAssetDocumentPrimitiveInvocation MissingActiveTransaction =
			MakeInvocation(*MutatingDescriptor);
		MissingActiveTransaction.TransactionId = FGuid::NewGuid();
		TestFailedInvocation(
			*this, TEXT("missing active transaction fails closed"), MissingActiveTransaction);
	}

	FString ProviderSource;
	TestTrue(TEXT("provider source is readable"), LoadProviderSource(ProviderSource));
	TestTrue(TEXT("provider validates every handler outcome"),
		ProviderSource.Contains(TEXT("ValidateAssetDocumentPrimitiveOutcome")));
	return true;
}

ASSETDOCUMENT_GENERIC_PROVIDER_TEST(
	FAssetDocumentGenericProviderAdmissionTokenOnlyTest,
	"UEAssetDoc.GenericProvider.AdmissionTokenOnly")

bool FAssetDocumentGenericProviderAdmissionTokenOnlyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericProviderShellTests;
	const TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors =
		CreateAssetDocumentEditorPrimitiveProvider()->DescribePrimitives();
	const FAssetDocumentPrimitiveDescriptor* Enumerate =
		FindDescriptor(Descriptors, TEXT("editor.property.enumerate_admitted.v1"));
	const FAssetDocumentPrimitiveDescriptor* Write =
		FindDescriptor(Descriptors, TEXT("editor.property.write_admitted.v1"));
	const FAssetDocumentPrimitiveDescriptor* BatchWrite =
		FindDescriptor(
			Descriptors, TEXT("editor.property.write_admitted_batch.v1"));
	TestNotNull(TEXT("enumerate descriptor"), Enumerate);
	TestNotNull(TEXT("write descriptor"), Write);
	TestNotNull(TEXT("batch write descriptor"), BatchWrite);
	if (Enumerate != nullptr)
	{
		TestTrue(TEXT("enumeration consumes no caller token"), Enumerate->HandleArgumentSchemas.IsEmpty());
		TestTrue(TEXT("enumeration issues read-only property tokens"),
			HasResult(*Enumerate, TEXT("properties"), EAssetDocumentHandleKind::Property,
				EAssetDocumentHandleAccess::ReadOnly,
				EAssetDocumentHandleResultCardinality::ZeroOrMore));
	}
	if (Write != nullptr)
	{
		TestTrue(TEXT("write requires mutable asset capability"),
			HasArgument(*Write, TEXT("asset"), EAssetDocumentHandleKind::Asset,
				EAssetDocumentHandleAccess::Mutable));
		TestTrue(TEXT("write requires admitted property token"),
			HasArgument(*Write, TEXT("property"), EAssetDocumentHandleKind::Property,
				EAssetDocumentHandleAccess::ReadOnly));
	}
	if (BatchWrite != nullptr)
	{
		TestTrue(TEXT("batch write requires mutable asset capability"),
			HasArgument(*BatchWrite, TEXT("asset"), EAssetDocumentHandleKind::Asset,
				EAssetDocumentHandleAccess::Mutable));
		TestTrue(TEXT("batch write requires immutable composite property token"),
			HasArgument(
				*BatchWrite,
				TEXT("property_batch"),
				EAssetDocumentHandleKind::Property,
				EAssetDocumentHandleAccess::ReadOnly));
		TestTrue(TEXT("batch write publishes no handles"),
			BatchWrite->HandleResultSchemas.IsEmpty());
	}
	return true;
}

ASSETDOCUMENT_GENERIC_PROVIDER_TEST(
	FAssetDocumentGenericProviderNoPropertyByNameEscapeTest,
	"UEAssetDoc.GenericProvider.NoPropertyByNameEscape")

bool FAssetDocumentGenericProviderNoPropertyByNameEscapeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericProviderShellTests;
	const TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors =
		CreateAssetDocumentEditorPrimitiveProvider()->DescribePrimitives();
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor : Descriptors)
	{
		TestFalse(TEXT("no property-by-name primitive id"),
			Descriptor.PrimitiveId.Contains(TEXT("property_by_name")));
		for (const FAssetDocumentPrimitiveHandleArgumentSchema& Schema :
			Descriptor.HandleArgumentSchemas)
		{
			TestFalse(TEXT("no name-shaped property handle"), Schema.Name == TEXT("property_name"));
		}
	}

	const FAssetDocumentPrimitiveDescriptor* Write =
		FindDescriptor(Descriptors, TEXT("editor.property.write_admitted.v1"));
	TestNotNull(TEXT("write descriptor"), Write);
	if (Write != nullptr)
	{
		FAssetDocumentPrimitiveInvocation Invocation = MakeInvocation(*Write);
		Invocation.TransactionId = FGuid::NewGuid();
		Invocation.ValueArguments.ObjectKeys.Add(TEXT("property_name"));
		FAssetDocumentRuntimeValueDto& PropertyName =
			Invocation.ValueArguments.Elements.AddDefaulted_GetRef();
		PropertyName.Kind = EAssetDocumentRuntimeValueKind::String;
		PropertyName.StringValue = TEXT("AnyProperty");
		for (const FAssetDocumentPrimitiveHandleArgumentSchema& Schema :
			Write->HandleArgumentSchemas)
		{
			FAssetDocumentPrimitiveHandleArgument& Argument =
				Invocation.HandleArguments.AddDefaulted_GetRef();
			Argument.Name = Schema.Name;
			Argument.Handle = MakeHandle(
				Invocation.RequestId,
				Invocation.TransactionId,
				Schema.Kind,
				Schema.RequiredAccess,
				Invocation.HandleArguments.Num());
		}
		FAlwaysStaleHandleResolver Resolver;
		FMatchingAtomicTransaction Transaction(Invocation);
		const FAssetDocumentPrimitiveOutcome Outcome =
			CreateAssetDocumentEditorPrimitiveProvider()->Invoke(
				Invocation, Resolver, &Transaction);
		TestNotEqual(TEXT("property name value escape never succeeds"),
			Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		TestEqual(TEXT("property name is rejected before handle lookup"),
			Resolver.AcquireCount, 0);
	}

	return true;
}

ASSETDOCUMENT_GENERIC_PROVIDER_TEST(
	FAssetDocumentGenericProviderCanonicalClassPathOnlyTest,
	"UEAssetDoc.GenericProvider.CanonicalClassPathOnly")

bool FAssetDocumentGenericProviderCanonicalClassPathOnlyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericProviderShellTests;
	const FAssetDocumentPrimitiveDescriptor* Descriptor = FindDescriptor(
		CreateAssetDocumentEditorPrimitiveProvider()->DescribePrimitives(),
		TEXT("editor.class.resolve_canonical.v1"));
	TestNotNull(TEXT("class resolver descriptor"), Descriptor);
	if (Descriptor != nullptr)
	{
		TestTrue(TEXT("class resolver consumes no forged handle"), Descriptor->HandleArgumentSchemas.IsEmpty());
		TestTrue(TEXT("class resolver issues exact read-only class capability"),
			HasResult(*Descriptor, TEXT("class"), EAssetDocumentHandleKind::Class,
				EAssetDocumentHandleAccess::ReadOnly,
				EAssetDocumentHandleResultCardinality::ExactlyOne));
	}
	return true;
}

ASSETDOCUMENT_GENERIC_PROVIDER_TEST(
	FAssetDocumentGenericProviderLifecycleAuthorityTest,
	"UEAssetDoc.GenericProvider.LifecycleAuthority")

bool FAssetDocumentGenericProviderLifecycleAuthorityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericProviderShellTests;
	const TConstArrayView<FAssetDocumentPrimitiveDescriptor> Descriptors =
		CreateAssetDocumentEditorPrimitiveProvider()->DescribePrimitives();
	const FAssetDocumentPrimitiveDescriptor* Scratch =
		FindDescriptor(Descriptors, TEXT("editor.scratch.clone_asset.v1"));
	const FAssetDocumentPrimitiveDescriptor* Create =
		FindDescriptor(Descriptors, TEXT("editor.asset.create_by_factory.v1"));
	const FAssetDocumentPrimitiveDescriptor* Rollback =
		FindDescriptor(Descriptors, TEXT("editor.asset.rollback_created.v1"));
	TestNotNull(TEXT("scratch descriptor"), Scratch);
	TestNotNull(TEXT("factory descriptor"), Create);
	TestNotNull(TEXT("rollback descriptor"), Rollback);
	if (Scratch != nullptr)
	{
		TestEqual(TEXT("scratch is the sole raw lifecycle disposition"),
			Scratch->Admission.Disposition,
			EAssetDocumentPrimitiveDisposition::FrameworkRawLifecycleAllowlisted);
	}
	if (Create != nullptr)
	{
		TestEqual(TEXT("factory uses ordinary helper"), Create->Admission.Disposition,
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper);
		TestEqual(TEXT("factory keeps its v1 identity"), Create->Version, 1u);
		TestTrue(TEXT("factory evidence names the non-loading exact lookup"),
			Create->Admission.ExportedAuthoritativeApi.Contains(TEXT("StaticFindObjectSafe")));
		TestTrue(TEXT("factory evidence retains public AssetTools creation"),
			Create->Admission.ExportedAuthoritativeApi.Contains(TEXT("IAssetTools::CreateAsset")));
	}
	if (Rollback != nullptr)
	{
		TestEqual(TEXT("rollback uses ordinary helper"), Rollback->Admission.Disposition,
			EAssetDocumentPrimitiveDisposition::OrdinaryHelper);
	}
	return true;
}

ASSETDOCUMENT_GENERIC_PROVIDER_TEST(
	FAssetDocumentGenericProviderStaleHandleRejectedTest,
	"UEAssetDoc.GenericProvider.StaleHandleRejected")

bool FAssetDocumentGenericProviderStaleHandleRejectedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorGenericProviderShellTests;
	FAssetDocumentPrimitiveInvocation Invocation;
	Invocation.PrimitiveId = TEXT("editor.property.read_admitted.v1");
	Invocation.Version = 1;
	Invocation.RequestId = FGuid::NewGuid();
	Invocation.TransactionId = FGuid::NewGuid();
	Invocation.RuntimeRevision = 1;
	Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
	Invocation.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
	FAssetDocumentPrimitiveHandleArgument& Asset = Invocation.HandleArguments.AddDefaulted_GetRef();
	Asset.Name = TEXT("asset");
	Asset.Handle = MakeHandle(Invocation.RequestId, Invocation.TransactionId,
		EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::ReadOnly, 1);
	FAssetDocumentPrimitiveHandleArgument& Property = Invocation.HandleArguments.AddDefaulted_GetRef();
	Property.Name = TEXT("property");
	Property.Handle = MakeHandle(Invocation.RequestId, Invocation.TransactionId,
		EAssetDocumentHandleKind::Property, EAssetDocumentHandleAccess::ReadOnly, 2);
	FAlwaysStaleHandleResolver Resolver;
	const FAssetDocumentPrimitiveOutcome Outcome =
		CreateAssetDocumentEditorPrimitiveProvider()->Invoke(Invocation, Resolver, nullptr);
	TestNotEqual(TEXT("stale handle never succeeds"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	TestTrue(TEXT("stale diagnostic propagates"),
		HasDiagnostic(Outcome, TEXT("HandleMetadata.StaleHandle")));
	return true;
}

#undef ASSETDOCUMENT_GENERIC_PROVIDER_TEST
#endif
