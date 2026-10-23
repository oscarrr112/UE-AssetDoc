// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorPrimitiveProvider.h"

#include "AssetDocumentEditorProviderTestSupport.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Engine/PrimaryAssetLabel.h"
#include "Engine/Texture.h"
#include "Factories/DataAssetFactory.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorAssetLifecycleProviderTests
{
using namespace UE::AssetDocument::EditorPrimitive;

class FTestClassPayload final : public INativeIdentityPayload
{
public:
	explicit FTestClassPayload(UClass& InClass)
		: Class(&InClass)
		, CanonicalClassPath(InClass.GetPathName())
		, TypeRevision(static_cast<uint64>(InClass.GetUniqueID()))
	{
	}

	virtual FName GetPayloadTypeId() const override
	{
		return ClassPayloadTypeId;
	}

	virtual bool IsAlive() const override
	{
		const UClass* Resolved = Class.Get();
		return Resolved != nullptr
			&& Resolved->GetPathName() == CanonicalClassPath
			&& static_cast<uint64>(Resolved->GetUniqueID()) == TypeRevision;
	}

	virtual bool TryGetClassIdentity(FClassIdentityView& OutView) const override
	{
		if (!IsAlive())
		{
			return false;
		}
		OutView.Class = Class.Get();
		OutView.CanonicalClassPath = CanonicalClassPath;
		OutView.TypeRevision = TypeRevision;
		return true;
	}

private:
	TWeakObjectPtr<UClass> Class;
	FString CanonicalClassPath;
	uint64 TypeRevision = 0;
};

FAssetDocumentRuntimeValueDto MakeStringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeFactoryArguments(
	FAutomationTestBase& Test,
	FString FactoryId,
	FString PackagePath,
	FString AssetName)
{
	const FAssetDocumentEditorValueField Fields[] = {
		{TEXT("factory_id"), MakeStringValue(MoveTemp(FactoryId))},
		{TEXT("package_path"), MakeStringValue(MoveTemp(PackagePath))},
		{TEXT("asset_name"), MakeStringValue(MoveTemp(AssetName))},
	};
	FAssetDocumentRuntimeValueDto Result;
	FString DiagnosticCode;
	Test.TestTrue(TEXT("factory arguments have exact shape"),
		TryBuildAssetDocumentEditorValueObject(
			Fields, Result, DiagnosticCode));
	return Result;
}

FAssetDocumentRuntimeValueDto MakeNullValue()
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Null;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeResolveArguments(
	FAutomationTestBase& Test,
	FString Path,
	FAssetDocumentRuntimeValueDto ExpectedClass)
{
	const FAssetDocumentEditorValueField Fields[] = {
		{TEXT("path"), MakeStringValue(MoveTemp(Path))},
		{TEXT("expected_class"), MoveTemp(ExpectedClass)},
	};
	FAssetDocumentRuntimeValueDto Result;
	FString DiagnosticCode;
	Test.TestTrue(TEXT("resolve arguments have exact shape"),
		TryBuildAssetDocumentEditorValueObject(
			Fields, Result, DiagnosticCode));
	return Result;
}

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

const FAssetDocumentPrimitiveHandleResult* FindHandleResult(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const TCHAR* Name)
{
	for (const FAssetDocumentPrimitiveHandleResult& Result :
		Outcome.HandleResults)
	{
		if (Result.Name == Name)
		{
			return &Result;
		}
	}
	return nullptr;
}

FAssetDocumentHandleValidationContext MakeLeaseContext(
	const FAssetDocumentEditorProviderTestContext& Context,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access)
{
	FAssetDocumentHandleValidationContext Result;
	Result.RequestId = Context.RequestId;
	Result.TransactionId = Context.TransactionId;
	Result.RuntimeRevision = Context.RuntimeRevision;
	Result.ProviderId = Context.ProviderId;
	Result.Phase = EAssetDocumentRuntimeRequestPhase::Validate;
	Result.RequiredKind = Kind;
	Result.RequiredAccess = Access;
	return Result;
}

FAssetDocumentPrimitiveOutcome InvokeResolve(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentRuntimeValueDto& Arguments,
	FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentEditorProviderTestHandleResolver& Handles)
{
	TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(Descriptor, Context);
	Invocation.ValueArguments = Arguments;
	return Provider->Invoke(Invocation, Handles, nullptr);
}

FAssetDocumentPrimitiveOutcome InvokeFactoryCreate(
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentRuntimeValueDto& Arguments,
	UClass& AssetClass,
	FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	FAssetDocumentEditorProviderTestTransaction& Transaction)
{
	FAssetDocumentIssuedHandleResult ClassResult = Handles.Issue(
		EAssetDocumentHandleKind::Class,
		EAssetDocumentHandleAccess::ReadOnly,
		MakeUnique<FTestClassPayload>(AssetClass));
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(Descriptor, Context);
	Invocation.ValueArguments = Arguments;
	if (ClassResult.Handle.IsSet())
	{
		Invocation.HandleArguments.Add(
			{TEXT("asset_class"), ClassResult.Handle.GetValue()});
	}
	TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	return Provider->Invoke(Invocation, Handles, &Transaction);
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
		TEXT("AssetDocumentEditorAssetLifecycleProviderTests")));
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

FString MakeCanonicalAssetPath(
	const FString& PackagePath,
	const FString& AssetName)
{
	return FString::Printf(
		TEXT("%s/%s.%s"), *PackagePath, *AssetName, *AssetName);
}

void ReleaseTestHandles(
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	const FAssetDocumentEditorProviderTestContext& Context)
{
	FAssetDocumentHandleInvalidation Invalidation;
	Invalidation.Reason =
		EAssetDocumentHandleInvalidationReason::ProviderUnload;
	Invalidation.RequestId = Context.RequestId;
	Invalidation.TransactionId = Context.TransactionId;
	Invalidation.RuntimeRevision = Context.RuntimeRevision;
	Invalidation.ProviderId = Context.ProviderId;
	Handles.Invalidate(Invalidation);
}

bool HasFactoryDiagnostic(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const TCHAR* Code)
{
	for (const FAssetDocumentRuntimeDiagnostic& Diagnostic :
		Outcome.Diagnostics)
	{
		if (Diagnostic.Code == Code)
		{
			return true;
		}
	}
	return false;
}

bool TestResolvedObjectLease(
	FAutomationTestBase& Test,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentHandleValidationContext& LeaseContext,
	UObject& ExpectedAsset)
{
	FAssetDocumentVerifiedLeaseResult Lease =
		Handles.AcquireVerifiedLease(Handle, LeaseContext);
	if (!Test.TestTrue(TEXT("resolved asset handle leases"),
		Lease.Lease.IsValid()))
	{
		return false;
	}
	INativeIdentityPayload* Payload = TryGetNativeIdentityPayload(
		Lease.Lease->GetPayloadChecked(), AssetPayloadTypeId);
	if (!Test.TestNotNull(TEXT("resolved asset payload type is exact"), Payload))
	{
		return false;
	}
	FObjectIdentityView Identity;
	Test.TestTrue(TEXT("resolved asset identity is available"),
		Payload->TryGetObjectIdentity(Identity));
	Test.TestTrue(TEXT("resolved asset identity is exact"),
		Identity.Object == &ExpectedAsset);
	Test.TestEqual(TEXT("resolved canonical path is exact"),
		FString(Identity.CanonicalObjectPath), ExpectedAsset.GetPathName());
	return true;
}

bool TestResolvedPackageLease(
	FAutomationTestBase& Test,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentHandleValidationContext& LeaseContext,
	UPackage& ExpectedPackage)
{
	FAssetDocumentVerifiedLeaseResult Lease =
		Handles.AcquireVerifiedLease(Handle, LeaseContext);
	if (!Test.TestTrue(TEXT("resolved package handle leases"),
		Lease.Lease.IsValid()))
	{
		return false;
	}
	INativeIdentityPayload* Payload = TryGetNativeIdentityPayload(
		Lease.Lease->GetPayloadChecked(), AssetPayloadTypeId);
	if (!Test.TestNotNull(TEXT("resolved package payload type is exact"), Payload))
	{
		return false;
	}
	FPackageIdentityView Identity;
	Test.TestTrue(TEXT("resolved package identity is available"),
		Payload->TryGetPackageIdentity(Identity));
	Test.TestTrue(TEXT("resolved package identity is exact"),
		Identity.Package == &ExpectedPackage);
	Test.TestEqual(TEXT("resolved package name is exact"),
		FString(Identity.PackageName), ExpectedPackage.GetName());
	return true;
}

bool TestExistingTarget(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	UPrimaryAssetLabel& Asset)
{
	UPackage* Package = Asset.GetOutermost();
	const bool bDirtyBefore = Package->IsDirty();
	const bool bRegistryBefore = FAssetRegistryModule::GetRegistry()
		.GetAssetByObjectPath(FSoftObjectPath(Asset.GetPathName()), true)
		.IsValid();
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	const FAssetDocumentRuntimeValueDto Arguments = MakeResolveArguments(
		Test,
		Asset.GetPathName(),
		MakeStringValue(UPrimaryAssetLabel::StaticClass()->GetPathName()));
	const FAssetDocumentPrimitiveOutcome Outcome = InvokeResolve(
		Descriptor, Arguments, Context, Handles);
	if (!Test.TestEqual(TEXT("existing target resolution succeeds"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return false;
	}

	const FAssetDocumentPrimitiveHandleResult* Read =
		FindHandleResult(Outcome, TEXT("asset_read"));
	const FAssetDocumentPrimitiveHandleResult* Mutable =
		FindHandleResult(Outcome, TEXT("asset_mutable"));
	const FAssetDocumentPrimitiveHandleResult* PackageResult =
		FindHandleResult(Outcome, TEXT("package"));
	if (!Test.TestNotNull(TEXT("read result exists"), Read)
		|| !Test.TestNotNull(TEXT("mutable result exists"), Mutable)
		|| !Test.TestNotNull(TEXT("package result exists"), PackageResult))
	{
		return false;
	}
	if (!Test.TestEqual(TEXT("one read handle"), Read->Handles.Num(), 1)
		|| !Test.TestEqual(TEXT("one mutable handle"),
			Mutable->Handles.Num(), 1)
		|| !Test.TestEqual(TEXT("one package handle"),
			PackageResult->Handles.Num(), 1))
	{
		return false;
	}
	Test.TestTrue(TEXT("read and mutable handles differ"),
		Read->Handles[0].Slot != Mutable->Handles[0].Slot);
	const bool bReadIdentity = TestResolvedObjectLease(
		Test, Handles, Read->Handles[0],
		MakeLeaseContext(
			Context, EAssetDocumentHandleKind::Asset,
			EAssetDocumentHandleAccess::ReadOnly),
		Asset);
	const bool bMutableIdentity = TestResolvedObjectLease(
		Test, Handles, Mutable->Handles[0],
		MakeLeaseContext(
			Context, EAssetDocumentHandleKind::Asset,
			EAssetDocumentHandleAccess::Mutable),
		Asset);
	const bool bPackageIdentity = TestResolvedPackageLease(
		Test, Handles, PackageResult->Handles[0],
		MakeLeaseContext(
			Context, EAssetDocumentHandleKind::Package,
			EAssetDocumentHandleAccess::ReadOnly),
		*Package);

	const FAssetDocumentEditorValueFieldSpec OutputShape[] = {
		{TEXT("exists"), EAssetDocumentRuntimeValueKind::Boolean},
		{TEXT("canonical_path"), EAssetDocumentRuntimeValueKind::String},
		{TEXT("canonical_class"), EAssetDocumentRuntimeValueKind::String},
		{TEXT("package_name"), EAssetDocumentRuntimeValueKind::String},
		{TEXT("package_dirty"), EAssetDocumentRuntimeValueKind::Boolean},
	};
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString DiagnosticCode;
	Test.TestTrue(TEXT("existing output has exact shape"),
		TryReadAssetDocumentEditorExactObject(
			Outcome.Value, OutputShape, Values, DiagnosticCode));
	if (Values.Num() == 5)
	{
		Test.TestTrue(TEXT("existing output marks target present"),
			Values[0]->BooleanValue);
		Test.TestEqual(TEXT("existing output canonical path"),
			Values[1]->StringValue, Asset.GetPathName());
		Test.TestEqual(TEXT("existing output canonical class"),
			Values[2]->StringValue, Asset.GetClass()->GetPathName());
		Test.TestEqual(TEXT("existing output package"),
			Values[3]->StringValue, Package->GetName());
		Test.TestEqual(TEXT("existing output dirty evidence"),
			Values[4]->BooleanValue, bDirtyBefore);
	}
	Test.TestEqual(TEXT("resolve preserves package dirty state"),
		Package->IsDirty(), bDirtyBefore);
	Test.TestEqual(TEXT("resolve preserves registry state"),
		FAssetRegistryModule::GetRegistry()
			.GetAssetByObjectPath(FSoftObjectPath(Asset.GetPathName()), true)
			.IsValid(),
		bRegistryBefore);
	return bReadIdentity && bMutableIdentity && bPackageIdentity;
}

bool TestMissingTarget(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FString AssetName = FString::Printf(
		TEXT("DA_ResolveMissing_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString MissingPath = FString::Printf(
		TEXT("/Game/AssetDocumentTests/GenericProvider/TrackD/%s.%s"),
		*AssetName, *AssetName);
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	const FAssetDocumentPrimitiveOutcome Outcome = InvokeResolve(
		Descriptor,
		MakeResolveArguments(Test, MissingPath, MakeNullValue()),
		Context,
		Handles);
	if (!Test.TestEqual(TEXT("missing target resolution succeeds"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return false;
	}
	for (const TCHAR* Name : {TEXT("asset_read"), TEXT("asset_mutable"),
		TEXT("package")})
	{
		const FAssetDocumentPrimitiveHandleResult* Result =
			FindHandleResult(Outcome, Name);
		if (!Test.TestNotNull(
			FString::Printf(TEXT("%s result exists"), Name), Result))
		{
			return false;
		}
		Test.TestTrue(
			FString::Printf(TEXT("%s result is empty"), Name),
			Result->Handles.IsEmpty());
	}
	const FAssetDocumentEditorValueFieldSpec OutputShape[] = {
		{TEXT("exists"), EAssetDocumentRuntimeValueKind::Boolean},
		{TEXT("canonical_path"), EAssetDocumentRuntimeValueKind::Null},
		{TEXT("canonical_class"), EAssetDocumentRuntimeValueKind::Null},
		{TEXT("package_name"), EAssetDocumentRuntimeValueKind::Null},
		{TEXT("package_dirty"), EAssetDocumentRuntimeValueKind::Boolean},
	};
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString DiagnosticCode;
	Test.TestTrue(TEXT("missing output has exact shape"),
		TryReadAssetDocumentEditorExactObject(
			Outcome.Value, OutputShape, Values, DiagnosticCode));
	if (Values.Num() == 5)
	{
		Test.TestFalse(TEXT("missing output marks target absent"),
			Values[0]->BooleanValue);
		Test.TestFalse(TEXT("missing output has no dirty evidence"),
			Values[4]->BooleanValue);
	}
	return true;
}

bool TestExpectedClassMismatch(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	UPrimaryAssetLabel& Asset)
{
	UPackage* Package = Asset.GetOutermost();
	const bool bDirtyBefore = Package->IsDirty();
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	const FAssetDocumentPrimitiveOutcome Outcome = InvokeResolve(
		Descriptor,
		MakeResolveArguments(
			Test, Asset.GetPathName(),
			MakeStringValue(UTexture::StaticClass()->GetPathName())),
		Context,
		Handles);
	Test.TestEqual(TEXT("expected class mismatch fails closed"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestTrue(TEXT("type mismatch issues no handles"),
		Outcome.HandleResults.IsEmpty());
	Test.TestEqual(TEXT("type mismatch preserves dirty state"),
		Package->IsDirty(), bDirtyBefore);
	return true;
}

bool TestExactFactoryCreate(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FString PackagePath =
		TEXT("/Game/AssetDocumentTests/GenericProvider/TrackD");
	const FString AssetName = FString::Printf(
		TEXT("DA_FactoryAuthority_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString CanonicalPath =
		MakeCanonicalAssetPath(PackagePath, AssetName);
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	const FAssetDocumentPrimitiveOutcome Outcome = InvokeFactoryCreate(
		Descriptor,
		MakeFactoryArguments(
			Test, TEXT("data_asset_factory.v1"), PackagePath, AssetName),
		*UPrimaryAssetLabel::StaticClass(),
		Context,
		Handles,
		Transaction);
	if (!Test.TestEqual(TEXT("exact factory creation succeeds"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		DeleteFixtureAsset(FSoftObjectPath(CanonicalPath).ResolveObject());
		return false;
	}
	UObject* CreatedAsset = FSoftObjectPath(CanonicalPath).ResolveObject();
	if (!Test.TestNotNull(TEXT("factory created the exact target"),
		CreatedAsset))
	{
		return false;
	}
	Test.TestTrue(TEXT("factory created the requested exact class"),
		CreatedAsset->GetClass() == UPrimaryAssetLabel::StaticClass());
	Test.TestFalse(TEXT("factory create enlists its mutation footprint"),
		Transaction.GetEnlistedFootprint().IsEmpty());

	const FAssetDocumentEditorValueFieldSpec OutputShape[] = {
		{TEXT("canonical_path"), EAssetDocumentRuntimeValueKind::String},
		{TEXT("canonical_class"), EAssetDocumentRuntimeValueKind::String},
		{TEXT("package_name"), EAssetDocumentRuntimeValueKind::String},
		{TEXT("package_filename"), EAssetDocumentRuntimeValueKind::String},
	};
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString DiagnosticCode;
	Test.TestTrue(TEXT("factory output has exact shape"),
		TryReadAssetDocumentEditorExactObject(
			Outcome.Value, OutputShape, Values, DiagnosticCode));
	if (Values.Num() == 4)
	{
		Test.TestEqual(TEXT("factory output canonical path"),
			Values[0]->StringValue, CanonicalPath);
		Test.TestEqual(TEXT("factory output canonical class"),
			Values[1]->StringValue,
			UPrimaryAssetLabel::StaticClass()->GetPathName());
		Test.TestEqual(TEXT("factory output package name"),
			Values[2]->StringValue,
			FString::Printf(TEXT("%s/%s"), *PackagePath, *AssetName));
		Test.TestEqual(TEXT("factory output package filename"),
			Values[3]->StringValue,
			FPackageName::LongPackageNameToFilename(
				Values[2]->StringValue,
				FPackageName::GetAssetPackageExtension()));
	}

	const FAssetDocumentPrimitiveHandleResult* Read =
		FindHandleResult(Outcome, TEXT("asset_read"));
	const FAssetDocumentPrimitiveHandleResult* Mutable =
		FindHandleResult(Outcome, TEXT("asset_mutable"));
	const FAssetDocumentPrimitiveHandleResult* Package =
		FindHandleResult(Outcome, TEXT("package"));
	if (Test.TestNotNull(TEXT("factory read result exists"), Read)
		&& Test.TestNotNull(TEXT("factory mutable result exists"), Mutable)
		&& Test.TestNotNull(TEXT("factory package result exists"), Package))
	{
		Test.TestEqual(TEXT("factory returns one read handle"),
			Read->Handles.Num(), 1);
		Test.TestEqual(TEXT("factory returns one mutable handle"),
			Mutable->Handles.Num(), 1);
		Test.TestEqual(TEXT("factory returns one package handle"),
			Package->Handles.Num(), 1);
	}
	ReleaseTestHandles(Handles, Context);
	DeleteFixtureAsset(CreatedAsset);
	return true;
}

bool TestBehaviorTreeFactoryCreate(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	if (FModuleManager::LoadModulePtr<IModuleInterface>(TEXT("BehaviorTreeEditor"))
		== nullptr)
	{
		Test.AddError(TEXT("BehaviorTreeEditor module is unavailable."));
		return false;
	}
	UClass* const AssetClass = FSoftClassPath(
		TEXT("/Script/AIModule.BehaviorTree")).ResolveClass();
	if (!Test.TestNotNull(TEXT("BehaviorTree class is loaded"), AssetClass))
	{
		return false;
	}
	const FString PackagePath =
		TEXT("/Game/AssetDocumentTests/GenericProvider/TrackD");
	const FString AssetName = FString::Printf(
		TEXT("BT_FactoryAuthority_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString CanonicalPath = MakeCanonicalAssetPath(PackagePath, AssetName);
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	const FAssetDocumentPrimitiveOutcome Outcome = InvokeFactoryCreate(
		Descriptor,
		MakeFactoryArguments(
			Test, TEXT("behavior_tree_factory.v1"), PackagePath, AssetName),
		*AssetClass,
		Context,
		Handles,
		Transaction);
	if (!Test.TestEqual(TEXT("BehaviorTree factory creation succeeds"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		DeleteFixtureAsset(FSoftObjectPath(CanonicalPath).ResolveObject());
		return false;
	}
	UObject* const Created = FSoftObjectPath(CanonicalPath).ResolveObject();
	Test.TestNotNull(TEXT("BehaviorTree factory created the exact target"), Created);
	Test.TestTrue(TEXT("BehaviorTree factory preserves exact class"),
		Created != nullptr && Created->GetClass() == AssetClass);
	Test.TestFalse(TEXT("BehaviorTree factory enlists one creation footprint"),
		Transaction.GetEnlistedFootprint().IsEmpty());
	ReleaseTestHandles(Handles, Context);
	DeleteFixtureAsset(Created);
	return Created != nullptr;
}

bool SaveFactoryFixture(
	FAutomationTestBase& Test,
	UObject& Asset,
	const FString& PackageFilename)
{
	FSavePackageArgs SaveArguments;
	SaveArguments.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArguments.SaveFlags = SAVE_NoError;
	return Test.TestTrue(
		TEXT("test-owned persistence saves the factory asset"),
		UPackage::SavePackage(
			Asset.GetOutermost(),
			&Asset,
			*PackageFilename,
			SaveArguments));
}

bool TestReloadedFactoryIdentity(
	FAutomationTestBase& Test,
	UObject& Reloaded,
	const UClass& ExpectedClass,
	const FString& CanonicalPath,
	const FString& PackageName,
	const FString& PackageFilename)
{
	Test.TestTrue(TEXT("fresh reload preserves the exact class"),
		Reloaded.GetClass() == &ExpectedClass);
	Test.TestEqual(TEXT("fresh reload preserves the canonical path"),
		Reloaded.GetPathName(), CanonicalPath);
	Test.TestEqual(TEXT("fresh reload preserves the package name"),
		Reloaded.GetOutermost()->GetName(), PackageName);
	Test.TestTrue(TEXT("fresh reload preserves the exact package file"),
		IFileManager::Get().FileExists(*PackageFilename));
	const FAssetData AssetData = FAssetRegistryModule::GetRegistry()
		.GetAssetByObjectPath(FSoftObjectPath(CanonicalPath));
	Test.TestTrue(TEXT("fresh reload remains registered"),
		AssetData.IsValid());
	Test.TestTrue(TEXT("registry and reload share exact identity"),
		AssetData.GetAsset() == &Reloaded);
	return true;
}

void DeleteFactorySaveReloadFixture(
	const FString& CanonicalPath,
	const FString& PackageFilename)
{
	UObject* Asset = FSoftObjectPath(CanonicalPath).ResolveObject();
	if (Asset == nullptr
		&& IFileManager::Get().FileExists(*PackageFilename))
	{
		Asset = LoadObject<UObject>(nullptr, *CanonicalPath);
	}
	if (IFileManager::Get().FileExists(*PackageFilename))
	{
		if (Asset != nullptr)
		{
			UPackageTools::UnloadPackages({Asset->GetOutermost()});
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		IFileManager::Get().Delete(*PackageFilename, false, true);
		FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(
			{PackageFilename});
	}
	else
	{
		DeleteFixtureAsset(Asset);
	}
}

UObject* CreateFactorySaveReloadFixture(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FString& PackagePath,
	const FString& AssetName,
	const FString& CanonicalPath,
	FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	FAssetDocumentEditorProviderTestTransaction& Transaction)
{
	const FAssetDocumentPrimitiveOutcome Outcome = InvokeFactoryCreate(
		Descriptor,
		MakeFactoryArguments(
			Test, TEXT("data_asset_factory.v1"), PackagePath, AssetName),
		*UPrimaryAssetLabel::StaticClass(),
		Context,
		Handles,
		Transaction);
	UObject* Created = FSoftObjectPath(CanonicalPath).ResolveObject();
	if (!Test.TestEqual(
		TEXT("provider creates the persistence fixture"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return nullptr;
	}
	Test.TestNotNull(TEXT("persistence fixture exists"), Created);
	return Created;
}

UObject* SaveUnloadReloadFactoryFixture(
	FAutomationTestBase& Test,
	UObject& Created,
	const FString& CanonicalPath,
	const FString& PackageFilename)
{
	if (!SaveFactoryFixture(Test, Created, PackageFilename))
	{
		return nullptr;
	}
	UPackage* Package = Created.GetOutermost();
	if (!Test.TestTrue(TEXT("test-owned persistence unloads the package"),
		UPackageTools::UnloadPackages({Package})))
	{
		return nullptr;
	}
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	UObject* Reloaded = LoadObject<UObject>(nullptr, *CanonicalPath);
	Test.TestNotNull(TEXT("saved asset fresh reloads"), Reloaded);
	return Reloaded;
}

bool TestFactoryCreateSaveReload(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FString PackagePath =
		TEXT("/Game/AssetDocumentTests/GenericProvider/TrackD");
	const FString AssetName = FString::Printf(
		TEXT("DA_FactorySaveReload_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString PackageName = FString::Printf(
		TEXT("%s/%s"), *PackagePath, *AssetName);
	const FString CanonicalPath =
		MakeCanonicalAssetPath(PackagePath, AssetName);
	const FString PackageFilename =
		FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		DeleteFactorySaveReloadFixture(CanonicalPath, PackageFilename);
	};
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	UObject* Created = CreateFactorySaveReloadFixture(
		Test, Descriptor, PackagePath, AssetName, CanonicalPath,
		Context, Handles, Transaction);
	ReleaseTestHandles(Handles, Context);
	if (Created == nullptr)
	{
		return false;
	}
	UObject* Reloaded = SaveUnloadReloadFactoryFixture(
		Test, *Created, CanonicalPath, PackageFilename);
	if (Reloaded == nullptr)
	{
		return false;
	}
	return TestReloadedFactoryIdentity(
		Test, *Reloaded, *UPrimaryAssetLabel::StaticClass(), CanonicalPath,
		PackageName, PackageFilename);
}

bool TestBehaviorTreeFactoryCreateSaveReload(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	if (FModuleManager::LoadModulePtr<IModuleInterface>(TEXT("BehaviorTreeEditor"))
		== nullptr)
	{
		Test.AddError(TEXT("BehaviorTreeEditor module is unavailable."));
		return false;
	}
	UClass* const BehaviorTreeClass = FSoftClassPath(
		TEXT("/Script/AIModule.BehaviorTree")).ResolveClass();
	if (!Test.TestNotNull(TEXT("BehaviorTree save/reload class is loaded"),
		BehaviorTreeClass))
	{
		return false;
	}
	const FString PackagePath =
		TEXT("/Game/AssetDocumentTests/GenericProvider/TrackD");
	const FString AssetName = FString::Printf(
		TEXT("BT_FactorySaveReload_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString PackageName = FString::Printf(TEXT("%s/%s"),
		*PackagePath, *AssetName);
	const FString CanonicalPath =
		MakeCanonicalAssetPath(PackagePath, AssetName);
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		DeleteFactorySaveReloadFixture(CanonicalPath, PackageFilename);
	};
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	const FAssetDocumentPrimitiveOutcome Outcome = InvokeFactoryCreate(
		Descriptor,
		MakeFactoryArguments(Test, TEXT("behavior_tree_factory.v1"),
			PackagePath, AssetName),
		*BehaviorTreeClass,
		Context,
		Handles,
		Transaction);
	if (!Test.TestEqual(TEXT("BehaviorTree persistence fixture creation succeeds"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return false;
	}
	UObject* const Created = FSoftObjectPath(CanonicalPath).ResolveObject();
	ReleaseTestHandles(Handles, Context);
	if (!Test.TestNotNull(TEXT("BehaviorTree persistence fixture exists"), Created))
	{
		return false;
	}
	UObject* const Reloaded = SaveUnloadReloadFactoryFixture(
		Test, *Created, CanonicalPath, PackageFilename);
	return Reloaded != nullptr && TestReloadedFactoryIdentity(
		Test, *Reloaded, *BehaviorTreeClass, CanonicalPath, PackageName,
		PackageFilename);
}

bool TestClassOnlyFallbackRejected(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FString PackagePath =
		TEXT("/Game/AssetDocumentTests/GenericProvider/TrackD");
	const FString AssetName = FString::Printf(
		TEXT("DA_NoFactoryFallback_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString CanonicalPath =
		MakeCanonicalAssetPath(PackagePath, AssetName);
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	const FAssetDocumentPrimitiveOutcome Outcome = InvokeFactoryCreate(
		Descriptor,
		MakeFactoryArguments(
			Test, TEXT("arbitrary_factory.v1"), PackagePath, AssetName),
		*UPrimaryAssetLabel::StaticClass(),
		Context,
		Handles,
		Transaction);
	Test.TestEqual(TEXT("unknown factory fails closed"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestTrue(TEXT("unknown factory uses stable diagnostic"),
		HasFactoryDiagnostic(
			Outcome, TEXT("FactoryCreate.UnsupportedFactory")));
	Test.TestNull(TEXT("unknown factory never falls back to class creation"),
		FSoftObjectPath(CanonicalPath).ResolveObject());
	Test.TestTrue(TEXT("unknown factory enlists no footprint"),
		Transaction.GetEnlistedFootprint().IsEmpty());
	return true;
}

bool TestFactoryClassMismatchRejected(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FString PackagePath =
		TEXT("/Game/AssetDocumentTests/GenericProvider/TrackD");
	const FString AssetName = FString::Printf(
		TEXT("DA_FactoryClassMismatch_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString CanonicalPath =
		MakeCanonicalAssetPath(PackagePath, AssetName);
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	const FAssetDocumentPrimitiveOutcome Outcome = InvokeFactoryCreate(
		Descriptor,
		MakeFactoryArguments(
			Test, TEXT("data_asset_factory.v1"), PackagePath, AssetName),
		*UTexture::StaticClass(),
		Context,
		Handles,
		Transaction);
	Test.TestEqual(TEXT("factory class mismatch fails closed"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestTrue(TEXT("factory class mismatch creates no target"),
		FSoftObjectPath(CanonicalPath).ResolveObject() == nullptr);
	Test.TestTrue(TEXT("factory class mismatch enlists no footprint"),
		Transaction.GetEnlistedFootprint().IsEmpty());
	return true;
}

bool TestPreexistingTargetRejected(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FString AssetName = FString::Printf(
		TEXT("DA_FactoryPreexisting_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPrimaryAssetLabel* Existing = CreateFixtureAsset(AssetName);
	if (!Test.TestNotNull(TEXT("preexisting fixture exists"), Existing))
	{
		return false;
	}
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	const FAssetDocumentPrimitiveOutcome Outcome = InvokeFactoryCreate(
		Descriptor,
		MakeFactoryArguments(
			Test,
			TEXT("data_asset_factory.v1"),
			TEXT("/Game/AssetDocumentTests/GenericProvider/TrackD"),
			AssetName),
		*UPrimaryAssetLabel::StaticClass(),
		Context,
		Handles,
		Transaction);
	Test.TestEqual(TEXT("preexisting target fails closed"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	Test.TestTrue(TEXT("preexisting identity is preserved"),
		FSoftObjectPath(Existing->GetPathName()).ResolveObject() == Existing);
	Test.TestTrue(TEXT("preexisting target enlists no footprint"),
		Transaction.GetEnlistedFootprint().IsEmpty());
	DeleteFixtureAsset(Existing);
	return true;
}

struct FRollbackFixturePaths
{
	FString PackagePath;
	FString AssetName;
	FString PackageName;
	FString CanonicalPath;
	FString PackageFilename;
};

FRollbackFixturePaths MakeRollbackFixturePaths()
{
	FRollbackFixturePaths Paths;
	Paths.PackagePath =
		TEXT("/Game/AssetDocumentTests/GenericProvider/TrackD");
	Paths.AssetName = FString::Printf(
		TEXT("DA_RollbackCreated_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	Paths.PackageName = FString::Printf(
		TEXT("%s/%s"), *Paths.PackagePath, *Paths.AssetName);
	Paths.CanonicalPath =
		MakeCanonicalAssetPath(Paths.PackagePath, Paths.AssetName);
	Paths.PackageFilename =
		FPackageName::LongPackageNameToFilename(
			Paths.PackageName, FPackageName::GetAssetPackageExtension());
	return Paths;
}

const FAssetDocumentOpaqueHandle* FindSingleHandle(
	const FAssetDocumentPrimitiveOutcome& Outcome,
	const TCHAR* Name)
{
	const FAssetDocumentPrimitiveHandleResult* Result =
		FindHandleResult(Outcome, Name);
	return Result != nullptr && Result->Handles.Num() == 1
		? &Result->Handles[0]
		: nullptr;
}

bool CreateRollbackFixture(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& FactoryDescriptor,
	const FRollbackFixturePaths& Paths,
	FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	FAssetDocumentEditorProviderTestTransaction& Transaction,
	FAssetDocumentOpaqueHandle& OutAsset,
	FAssetDocumentOpaqueHandle& OutPackage)
{
	const FAssetDocumentPrimitiveOutcome Outcome = InvokeFactoryCreate(
		FactoryDescriptor,
		MakeFactoryArguments(
			Test,
			TEXT("data_asset_factory.v1"),
			Paths.PackagePath,
			Paths.AssetName),
		*UPrimaryAssetLabel::StaticClass(),
		Context,
		Handles,
		Transaction);
	if (!Test.TestEqual(TEXT("rollback fixture creation succeeds"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return false;
	}
	const FAssetDocumentOpaqueHandle* Asset =
		FindSingleHandle(Outcome, TEXT("asset_mutable"));
	const FAssetDocumentOpaqueHandle* Package =
		FindSingleHandle(Outcome, TEXT("package"));
	if (!Test.TestNotNull(TEXT("rollback fixture has mutable asset"), Asset)
		|| !Test.TestNotNull(TEXT("rollback fixture has mutable package"),
			Package))
	{
		return false;
	}
	OutAsset = *Asset;
	OutPackage = *Package;
	return true;
}

FAssetDocumentPrimitiveOutcome InvokeRollback(
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	const FAssetDocumentEditorProviderTestContext& Context,
	FAssetDocumentEditorProviderTestHandleResolver& Handles,
	FAssetDocumentEditorProviderTestTransaction& Transaction,
	const FAssetDocumentOpaqueHandle& Asset,
	const FAssetDocumentOpaqueHandle& Package)
{
	FAssetDocumentPrimitiveInvocation Invocation =
		MakeAssetDocumentEditorProviderTestInvocation(Descriptor, Context);
	Invocation.HandleArguments.Add({TEXT("asset"), Asset});
	Invocation.HandleArguments.Add({TEXT("package"), Package});
	return Provider.Invoke(Invocation, Handles, &Transaction);
}

bool FindRollbackDescriptors(
	FAutomationTestBase& Test,
	const IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor*& OutFactory,
	const FAssetDocumentPrimitiveDescriptor*& OutRollback)
{
	OutFactory = FindDescriptor(
		Provider, TEXT("editor.asset.create_by_factory.v1"));
	OutRollback = FindDescriptor(
		Provider, TEXT("editor.asset.rollback_created.v1"));
	return Test.TestNotNull(TEXT("factory descriptor exists"), OutFactory)
		&& Test.TestNotNull(TEXT("rollback descriptor exists"), OutRollback);
}

void DeleteRollbackFixture(const FRollbackFixturePaths& Paths)
{
	if (UObject* Asset =
		FSoftObjectPath(Paths.CanonicalPath).ResolveObject())
	{
		DeleteFixtureAsset(Asset);
	}
	if (UPackage* Package = FindPackage(nullptr, *Paths.PackageName))
	{
		Package->SetDirtyFlag(false);
		UPackageTools::UnloadPackages({Package});
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	IFileManager::Get().Delete(*Paths.PackageFilename, false, true);
}

bool TestRollbackValue(
	FAutomationTestBase& Test,
	const FAssetDocumentRuntimeValueDto& Value)
{
	const FAssetDocumentEditorValueFieldSpec Shape[] = {
		{TEXT("object_absent"), EAssetDocumentRuntimeValueKind::Boolean},
		{TEXT("package_absent"), EAssetDocumentRuntimeValueKind::Boolean},
		{TEXT("registry_absent"), EAssetDocumentRuntimeValueKind::Boolean},
		{TEXT("file_absent"), EAssetDocumentRuntimeValueKind::Boolean},
		{TEXT("dirty_residue"), EAssetDocumentRuntimeValueKind::Boolean},
	};
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString DiagnosticCode;
	if (!Test.TestTrue(TEXT("rollback output has exact absence shape"),
		TryReadAssetDocumentEditorExactObject(
			Value, Shape, Values, DiagnosticCode)))
	{
		return false;
	}
	Test.TestTrue(TEXT("rollback reports object absence"),
		Values[0]->BooleanValue);
	Test.TestTrue(TEXT("rollback reports package absence"),
		Values[1]->BooleanValue);
	Test.TestTrue(TEXT("rollback reports registry absence"),
		Values[2]->BooleanValue);
	Test.TestTrue(TEXT("rollback reports file absence"),
		Values[3]->BooleanValue);
	Test.TestFalse(TEXT("rollback reports no dirty residue"),
		Values[4]->BooleanValue);
	return true;
}

bool TestPhysicalAbsence(
	FAutomationTestBase& Test,
	const FRollbackFixturePaths& Paths)
{
	Test.TestNull(TEXT("rolled-back object is absent"),
		FSoftObjectPath(Paths.CanonicalPath).ResolveObject());
	UPackage* Package = FindPackage(nullptr, *Paths.PackageName);
	Test.TestNull(TEXT("rolled-back package is absent"), Package);
	Test.TestFalse(TEXT("rolled-back registry identity is absent"),
		FAssetRegistryModule::GetRegistry()
			.GetAssetByObjectPath(FSoftObjectPath(Paths.CanonicalPath))
			.IsValid());
	Test.TestFalse(TEXT("rolled-back file is absent"),
		IFileManager::Get().FileExists(*Paths.PackageFilename));
	Test.TestFalse(TEXT("rollback leaves no dirty package"),
		Package != nullptr && Package->IsDirty());
	return true;
}

bool TestBehaviorTreeRollbackCreated(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& FactoryDescriptor,
	const FAssetDocumentPrimitiveDescriptor& RollbackDescriptor,
	IAssetDocumentPrimitiveProvider& Provider)
{
	if (FModuleManager::LoadModulePtr<IModuleInterface>(TEXT("BehaviorTreeEditor"))
		== nullptr)
	{
		Test.AddError(TEXT("BehaviorTreeEditor module is unavailable."));
		return false;
	}
	UClass* const BehaviorTreeClass = FSoftClassPath(
		TEXT("/Script/AIModule.BehaviorTree")).ResolveClass();
	if (!Test.TestNotNull(TEXT("BehaviorTree rollback class is loaded"),
		BehaviorTreeClass))
	{
		return false;
	}
	FRollbackFixturePaths Paths = MakeRollbackFixturePaths();
	Paths.AssetName = FString::Printf(TEXT("BT_RollbackCreated_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	Paths.PackageName = FString::Printf(TEXT("%s/%s"),
		*Paths.PackagePath, *Paths.AssetName);
	Paths.CanonicalPath = MakeCanonicalAssetPath(Paths.PackagePath, Paths.AssetName);
	Paths.PackageFilename = FPackageName::LongPackageNameToFilename(
		Paths.PackageName, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		DeleteRollbackFixture(Paths);
	};
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	const FAssetDocumentPrimitiveOutcome Created = InvokeFactoryCreate(
		FactoryDescriptor,
		MakeFactoryArguments(Test, TEXT("behavior_tree_factory.v1"),
			Paths.PackagePath, Paths.AssetName),
		*BehaviorTreeClass,
		Context,
		Handles,
		Transaction);
	if (!Test.TestEqual(TEXT("BehaviorTree rollback fixture creation succeeds"),
		Created.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return false;
	}
	const FAssetDocumentOpaqueHandle* const Asset =
		FindSingleHandle(Created, TEXT("asset_mutable"));
	const FAssetDocumentOpaqueHandle* const Package =
		FindSingleHandle(Created, TEXT("package"));
	if (!Test.TestNotNull(TEXT("BehaviorTree rollback has mutable asset"), Asset)
		|| !Test.TestNotNull(TEXT("BehaviorTree rollback has package"), Package))
	{
		return false;
	}
	const FAssetDocumentPrimitiveOutcome RolledBack = InvokeRollback(
		Provider, RollbackDescriptor, Context, Handles, Transaction, *Asset, *Package);
	ReleaseTestHandles(Handles, Context);
	return Test.TestEqual(TEXT("BehaviorTree rollback succeeds"),
		RolledBack.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)
		&& TestRollbackValue(Test, RolledBack.Value)
		&& TestPhysicalAbsence(Test, Paths);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderResolveExistingReadOnlyTest,
	"UEAssetDoc.GenericProvider.ResolveExisting.ReadOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderResolveExistingReadOnlyTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorAssetLifecycleProviderTests;
	TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindDescriptor(*Provider, TEXT("editor.asset.resolve_existing.v1"));
	if (!TestNotNull(TEXT("resolve descriptor exists"), Descriptor))
	{
		return false;
	}
	UPrimaryAssetLabel* Asset = CreateFixtureAsset(FString::Printf(
		TEXT("DA_ResolveExisting_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!TestNotNull(TEXT("fixture asset exists"), Asset))
	{
		return false;
	}
	const bool bExisting = TestExistingTarget(*this, *Descriptor, *Asset);
	const bool bMissing = bExisting
		&& TestMissingTarget(*this, *Descriptor);
	const bool bMismatch = bMissing
		&& TestExpectedClassMismatch(*this, *Descriptor, *Asset);
	DeleteFixtureAsset(Asset);
	return bExisting && bMissing && bMismatch;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderFactoryCreateAuthorityTest,
	"UEAssetDoc.GenericProvider.FactoryCreate.Authority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderFactoryCreateAuthorityTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorAssetLifecycleProviderTests;
	TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindDescriptor(*Provider, TEXT("editor.asset.create_by_factory.v1"));
	if (!TestNotNull(TEXT("factory descriptor exists"), Descriptor)
		|| !TestNotNull(TEXT("exact non-null data asset factory is available"),
			FindExactDataAssetFactory()))
	{
		return false;
	}
	const bool bCreated = TestExactFactoryCreate(*this, *Descriptor);
	const bool bBehaviorTree = bCreated
		&& TestBehaviorTreeFactoryCreate(*this, *Descriptor);
	const bool bNoFallback = bBehaviorTree
		&& TestClassOnlyFallbackRejected(*this, *Descriptor);
	const bool bClassMismatch = bNoFallback
		&& TestFactoryClassMismatchRejected(*this, *Descriptor);
	const bool bPreexisting = bClassMismatch
		&& TestPreexistingTargetRejected(*this, *Descriptor);
	return bCreated && bBehaviorTree && bNoFallback && bClassMismatch
		&& bPreexisting;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderFactoryCreateSaveReloadTest,
	"UEAssetDoc.GenericProvider.FactoryCreate.SaveReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderFactoryCreateSaveReloadTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorAssetLifecycleProviderTests;
	TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindDescriptor(*Provider, TEXT("editor.asset.create_by_factory.v1"));
	if (!TestNotNull(TEXT("factory descriptor exists"), Descriptor)
		|| !TestNotNull(TEXT("exact non-null data asset factory is available"),
			FindExactDataAssetFactory()))
	{
		return false;
	}
	return TestFactoryCreateSaveReload(*this, *Descriptor)
		&& TestBehaviorTreeFactoryCreateSaveReload(*this, *Descriptor);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderRollbackCreatedAbsenceTest,
	"UEAssetDoc.GenericProvider.RollbackCreated.Absence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderRollbackCreatedAbsenceTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorAssetLifecycleProviderTests;
	const FRollbackFixturePaths Paths = MakeRollbackFixturePaths();
	ON_SCOPE_EXIT
	{
		DeleteRollbackFixture(Paths);
	};
	TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* FactoryDescriptor = nullptr;
	const FAssetDocumentPrimitiveDescriptor* RollbackDescriptor = nullptr;
	if (!FindRollbackDescriptors(
		*this, *Provider, FactoryDescriptor, RollbackDescriptor))
	{
		return false;
	}
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	FAssetDocumentOpaqueHandle Asset;
	FAssetDocumentOpaqueHandle Package;
	if (!CreateRollbackFixture(
		*this, *FactoryDescriptor, Paths, Context, Handles, Transaction,
		Asset, Package))
	{
		return false;
	}
	const FAssetDocumentPrimitiveOutcome Outcome = InvokeRollback(
		*Provider, *RollbackDescriptor, Context, Handles, Transaction,
		Asset, Package);
	if (!TestEqual(TEXT("rollback succeeds"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
	{
		return false;
	}
	TestRollbackValue(*this, Outcome.Value);
	return TestPhysicalAbsence(*this, Paths)
		&& TestBehaviorTreeRollbackCreated(
			*this, *FactoryDescriptor, *RollbackDescriptor, *Provider);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderRollbackCreatedOwnershipMismatchTest,
	"UEAssetDoc.GenericProvider.RollbackCreated.OwnershipMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderRollbackCreatedOwnershipMismatchTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorAssetLifecycleProviderTests;
	const FRollbackFixturePaths FirstPaths = MakeRollbackFixturePaths();
	const FRollbackFixturePaths SecondPaths = MakeRollbackFixturePaths();
	ON_SCOPE_EXIT
	{
		DeleteRollbackFixture(FirstPaths);
		DeleteRollbackFixture(SecondPaths);
	};
	TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* FactoryDescriptor = nullptr;
	const FAssetDocumentPrimitiveDescriptor* RollbackDescriptor = nullptr;
	if (!FindRollbackDescriptors(
		*this, *Provider, FactoryDescriptor, RollbackDescriptor))
	{
		return false;
	}
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	FAssetDocumentOpaqueHandle FirstAsset;
	FAssetDocumentOpaqueHandle FirstPackage;
	FAssetDocumentOpaqueHandle SecondAsset;
	FAssetDocumentOpaqueHandle SecondPackage;
	if (!CreateRollbackFixture(
			*this, *FactoryDescriptor, FirstPaths, Context, Handles,
			Transaction, FirstAsset, FirstPackage)
		|| !CreateRollbackFixture(
			*this, *FactoryDescriptor, SecondPaths, Context, Handles,
			Transaction, SecondAsset, SecondPackage))
	{
		return false;
	}
	const FAssetDocumentPrimitiveOutcome Outcome = InvokeRollback(
		*Provider, *RollbackDescriptor, Context, Handles, Transaction,
		FirstAsset, SecondPackage);
	TestEqual(TEXT("ownership mismatch fails"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("ownership mismatch uses stable diagnostic"),
		HasFactoryDiagnostic(
			Outcome, TEXT("RollbackCreated.CreationRecordMismatch")));
	TestNotNull(TEXT("ownership mismatch preserves first asset"),
		FSoftObjectPath(FirstPaths.CanonicalPath).ResolveObject());
	TestNotNull(TEXT("ownership mismatch preserves second asset"),
		FSoftObjectPath(SecondPaths.CanonicalPath).ResolveObject());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentGenericProviderRollbackCreatedResiduePoisonsTest,
	"UEAssetDoc.GenericProvider.RollbackCreated.ResiduePoisons",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericProviderRollbackCreatedResiduePoisonsTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorAssetLifecycleProviderTests;
	const FRollbackFixturePaths Paths = MakeRollbackFixturePaths();
	ON_SCOPE_EXIT
	{
		DeleteRollbackFixture(Paths);
	};
	TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* FactoryDescriptor = nullptr;
	const FAssetDocumentPrimitiveDescriptor* RollbackDescriptor = nullptr;
	if (!FindRollbackDescriptors(
		*this, *Provider, FactoryDescriptor, RollbackDescriptor))
	{
		return false;
	}
	FAssetDocumentEditorProviderTestContext Context;
	FAssetDocumentEditorProviderTestHandleResolver Handles(Context);
	FAssetDocumentEditorProviderTestTransaction Transaction(Context);
	FAssetDocumentOpaqueHandle Asset;
	FAssetDocumentOpaqueHandle PackageHandle;
	if (!CreateRollbackFixture(
		*this, *FactoryDescriptor, Paths, Context, Handles, Transaction,
		Asset, PackageHandle))
	{
		return false;
	}
	UObject* Created =
		FSoftObjectPath(Paths.CanonicalPath).ResolveObject();
	if (!TestNotNull(TEXT("residue fixture asset exists"), Created))
	{
		return false;
	}
	TStrongObjectPtr<UPackage> RetainedPackage(Created->GetOutermost());
	const FAssetDocumentPrimitiveOutcome Outcome = InvokeRollback(
		*Provider, *RollbackDescriptor, Context, Handles, Transaction,
		Asset, PackageHandle);
	TestEqual(TEXT("residue never reports success"),
		Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
	TestTrue(TEXT("residue uses stable poison-worthy diagnostic"),
		HasFactoryDiagnostic(
			Outcome, TEXT("RollbackCreated.ResidueDetected")));
	TestNull(TEXT("authoritative deletion removed the target object"),
		FSoftObjectPath(Paths.CanonicalPath).ResolveObject());
	TestNotNull(TEXT("test-owned package retention injects residue"),
		FindPackage(nullptr, *Paths.PackageName));
	return true;
}
#endif
