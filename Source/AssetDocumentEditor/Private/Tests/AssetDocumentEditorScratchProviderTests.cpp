// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentEditorPrimitiveProvider.h"

#include "AssetDocumentEditorProviderTestSupport.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Engine/PrimaryAssetLabel.h"
#include "Factories/DataAssetFactory.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorScratchProviderTests
{
using namespace UE::AssetDocument::EditorPrimitive;

class FSourceAssetPayload final : public INativeIdentityPayload
{
public:
	explicit FSourceAssetPayload(UObject& InAsset)
		: Asset(&InAsset)
		, CanonicalPath(InAsset.GetPathName())
	{
	}

	virtual FName GetPayloadTypeId() const override
	{
		return AssetPayloadTypeId;
	}

	virtual bool IsAlive() const override
	{
		return Asset.IsValid();
	}

	virtual bool TryGetObjectIdentity(FObjectIdentityView& OutView) const override
	{
		OutView.Object = Asset.Get();
		OutView.CanonicalObjectPath = CanonicalPath;
		OutView.TypeRevision = 1;
		return OutView.Object != nullptr;
	}

private:
	TStrongObjectPtr<UObject> Asset;
	FString CanonicalPath;
};

const FAssetDocumentPrimitiveDescriptor* FindDescriptor(
	const IAssetDocumentPrimitiveProvider& Provider,
	const TCHAR* PrimitiveId)
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

UFactory* FindExactDataAssetFactory()
{
	IAssetTools& AssetTools =
		FModuleManager::LoadModuleChecked<FAssetToolsModule>(
			TEXT("AssetTools")).Get();
	for (UFactory* Factory : AssetTools.GetNewAssetFactories())
	{
		if (Factory != nullptr
			&& Factory->GetClass() == UDataAssetFactory::StaticClass())
		{
			return Factory;
		}
	}
	return nullptr;
}

UPrimaryAssetLabel* CreateFixtureAsset(const FString& AssetName)
{
	UFactory* Factory = FindExactDataAssetFactory();
	if (Factory == nullptr)
	{
		return nullptr;
	}
	IAssetTools& AssetTools =
		FModuleManager::LoadModuleChecked<FAssetToolsModule>(
			TEXT("AssetTools")).Get();
	return Cast<UPrimaryAssetLabel>(AssetTools.CreateAsset(
		AssetName,
		TEXT("/Game/AssetDocumentTests/GenericProvider/TrackD"),
		UPrimaryAssetLabel::StaticClass(),
		Factory,
		TEXT("AssetDocumentEditorScratchProviderTests")));
}

void DeleteFixtureAsset(UObject* Asset)
{
	if (Asset != nullptr)
	{
		ObjectTools::DeleteObjects(
			{Asset},
			false,
			ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
	}
}

FAssetDocumentRuntimeValueDto MakeCategoryArguments()
{
	FAssetDocumentRuntimeValueDto Category;
	Category.Kind = EAssetDocumentRuntimeValueKind::String;
	Category.StringValue = TEXT("data_asset_exact.v1");
	FAssetDocumentRuntimeValueDto Arguments;
	Arguments.Kind = EAssetDocumentRuntimeValueKind::Object;
	Arguments.ObjectKeys = {TEXT("category_id")};
	Arguments.Elements = {MoveTemp(Category)};
	return Arguments;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderScratchContextBoundIssueTest,
	"UEAssetDoc.GenericProvider.Scratch.ContextBoundIssue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderScratchContextBoundIssueTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorScratchProviderTests;
	TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindDescriptor(*Provider, TEXT("editor.scratch.clone_asset.v1"));
	if (!TestNotNull(TEXT("scratch descriptor exists"), Descriptor))
	{
		return false;
	}

	UPrimaryAssetLabel* Source = CreateFixtureAsset(
		FString::Printf(TEXT("DA_ScratchSource_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!TestNotNull(TEXT("fixture source exists"), Source))
	{
		return false;
	}

	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FAssetDocumentIssuedHandleResult SourceResult = Handles.Issue(
		EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly,
		MakeUnique<FSourceAssetPayload>(*Source));
	if (!TestTrue(TEXT("source handle issues"), SourceResult.Handle.IsSet()))
	{
		DeleteFixtureAsset(Source);
		return false;
	}

	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(*Descriptor, Context);
	Invocation.ValueArguments = MakeCategoryArguments();
	Invocation.HandleArguments.Add(
		{TEXT("source_asset"), SourceResult.Handle.GetValue()});
	const FAssetDocumentPrimitiveOutcome Outcome =
		Provider->Invoke(Invocation, Handles, nullptr);
	TestEqual(TEXT("scratch clone succeeds"), Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	if (Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
	{
		TestEqual(TEXT("one scratch result group"), Outcome.HandleResults.Num(), 1);
		TestEqual(TEXT("scratch result name"),
			Outcome.HandleResults[0].Name, FString(TEXT("scratch")));
		TestEqual(TEXT("one scratch handle"),
			Outcome.HandleResults[0].Handles.Num(), 1);
		const FAssetDocumentOpaqueHandle& Scratch =
			Outcome.HandleResults[0].Handles[0];
		TestEqual(TEXT("scratch kind"), Scratch.Kind,
			EAssetDocumentHandleKind::Scratch);
		TestEqual(TEXT("scratch access"), Scratch.Access,
			EAssetDocumentHandleAccess::Mutable);
		FAssetDocumentHandleValidationContext LeaseContext;
		LeaseContext.RequestId = Context.RequestId;
		LeaseContext.TransactionId = Context.TransactionId;
		LeaseContext.RuntimeRevision = Context.RuntimeRevision;
		LeaseContext.ProviderId = Context.ProviderId;
		LeaseContext.Phase = EAssetDocumentRuntimeRequestPhase::Preflight;
		LeaseContext.RequiredKind = EAssetDocumentHandleKind::Scratch;
		LeaseContext.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
		const FAssetDocumentVerifiedLeaseResult Lease =
			Handles.AcquireVerifiedLease(Scratch, LeaseContext);
		TestNotNull(TEXT("same-context next invocation can lease scratch"),
			Lease.Lease.Get());
	}
	DeleteFixtureAsset(Source);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderScratchNoResidueTest,
	"UEAssetDoc.GenericProvider.Scratch.NoResidue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderScratchNoResidueTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorScratchProviderTests;
	using namespace UE::AssetDocument::EditorPrimitive;
	TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindDescriptor(*Provider, TEXT("editor.scratch.clone_asset.v1"));
	if (!TestNotNull(TEXT("scratch descriptor exists"), Descriptor))
	{
		return false;
	}
	UPrimaryAssetLabel* Source = CreateFixtureAsset(
		FString::Printf(TEXT("DA_ScratchResidue_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!TestNotNull(TEXT("fixture source exists"), Source))
	{
		return false;
	}
	UPackage* SourcePackage = Source->GetOutermost();
	const bool bSourceWasDirty = SourcePackage->IsDirty();
	const bool bTransientWasDirty = GetTransientPackage()->IsDirty();

	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FAssetDocumentIssuedHandleResult SourceResult = Handles.Issue(
		EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly,
		MakeUnique<FSourceAssetPayload>(*Source));
	if (!TestTrue(TEXT("source handle issues"), SourceResult.Handle.IsSet()))
	{
		DeleteFixtureAsset(Source);
		return false;
	}
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(*Descriptor, Context);
	Invocation.ValueArguments = MakeCategoryArguments();
	Invocation.HandleArguments.Add(
		{TEXT("source_asset"), SourceResult.Handle.GetValue()});
	const FAssetDocumentPrimitiveOutcome Outcome =
		Provider->Invoke(Invocation, Handles, nullptr);
	if (!TestEqual(TEXT("scratch clone succeeds"), Outcome.Status,
		EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		DeleteFixtureAsset(Source);
		return false;
	}

	const FAssetDocumentOpaqueHandle& ScratchHandle =
		Outcome.HandleResults[0].Handles[0];
	FAssetDocumentHandleValidationContext LeaseContext;
	LeaseContext.RequestId = Context.RequestId;
	LeaseContext.TransactionId = Context.TransactionId;
	LeaseContext.RuntimeRevision = Context.RuntimeRevision;
	LeaseContext.ProviderId = Context.ProviderId;
	LeaseContext.Phase = EAssetDocumentRuntimeRequestPhase::Preflight;
	LeaseContext.RequiredKind = EAssetDocumentHandleKind::Scratch;
	LeaseContext.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	FAssetDocumentVerifiedLeaseResult Lease =
		Handles.AcquireVerifiedLease(ScratchHandle, LeaseContext);
	INativeIdentityPayload* Payload = Lease.Lease
		? TryGetNativeIdentityPayload(
			Lease.Lease->GetPayloadChecked(), ScratchPayloadTypeId)
		: nullptr;
	FObjectIdentityView ScratchIdentity;
	UObject* Scratch = Payload
		&& Payload->TryGetObjectIdentity(ScratchIdentity)
		? ScratchIdentity.Object
		: nullptr;
	if (!TestNotNull(TEXT("scratch identity resolves"), Scratch))
	{
		DeleteFixtureAsset(Source);
		return false;
	}

	TestEqual(TEXT("scratch outermost is the transient package"),
		Scratch->GetOutermost(), GetTransientPackage());
	TestTrue(TEXT("scratch is transient"),
		Scratch->HasAnyFlags(RF_Transient));
	TestFalse(TEXT("scratch strips public standalone and transactional flags"),
		Scratch->HasAnyFlags(RF_Public | RF_Standalone | RF_Transactional));
	TestEqual(TEXT("source package dirty state is unchanged"),
		SourcePackage->IsDirty(), bSourceWasDirty);
	TestEqual(TEXT("transient package dirty state is unchanged"),
		GetTransientPackage()->IsDirty(), bTransientWasDirty);
	TestFalse(TEXT("scratch is absent from AssetRegistry"),
		FAssetRegistryModule::GetRegistry()
			.GetAssetByObjectPath(FSoftObjectPath(
				FString(ScratchIdentity.CanonicalObjectPath)), true)
			.IsValid());
	const FString ScratchPackageName = Scratch->GetOutermost()->GetName();
	const FString ScratchFilename = FPackageName::LongPackageNameToFilename(
		ScratchPackageName, FPackageName::GetAssetPackageExtension());
	TestFalse(TEXT("scratch has no package file"),
		IFileManager::Get().FileExists(*ScratchFilename));
	DeleteFixtureAsset(Source);
	return true;
}
#endif
