// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentEditorPrimitiveProvider.h"

#include "AssetDocumentEditorProviderTestSupport.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/DataAsset.h"
#include "Engine/Level.h"
#include "Engine/PrimaryAssetLabel.h"
#include "Engine/TextureCollection.h"
#include "Factories/DataAssetFactory.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorCreateScratchProviderTests
{
class FScopedFactoryDataAssetClass final
{
public:
	FScopedFactoryDataAssetClass(
		UDataAssetFactory& InFactory,
		UClass* InitialClass)
		: Factory(InFactory)
		, OriginalClass(InFactory.DataAssetClass)
	{
		Factory.DataAssetClass = InitialClass;
	}

	~FScopedFactoryDataAssetClass()
	{
		Factory.DataAssetClass = OriginalClass;
	}

private:
	UDataAssetFactory& Factory;
	TSubclassOf<UDataAsset> OriginalClass;
};

class FScopedLevelDirtiedCounter final
{
public:
	FScopedLevelDirtiedCounter()
	{
		Handle = ULevel::LevelDirtiedEvent.AddLambda(
			[this]()
			{
				++Count;
			});
	}

	~FScopedLevelDirtiedCounter()
	{
		ULevel::LevelDirtiedEvent.Remove(Handle);
	}

	int32 GetCount() const
	{
		return Count;
	}

private:
	FDelegateHandle Handle;
	int32 Count = 0;
};

TArray<FString> SnapshotTransientRegistryEntries()
{
	TArray<FAssetData> AssetData;
	IAssetRegistry::GetChecked().GetAssetsByPackageName(
		GetTransientPackage()->GetFName(),
		AssetData);
	TArray<FString> Result;
	for (const FAssetData& Entry : AssetData)
	{
		Result.Add(Entry.GetObjectPathString());
	}
	Result.Sort();
	return Result;
}

struct FAuthorityResidueSnapshot
{
	bool bClassPackageDirty = false;
	bool bFactoryPackageDirty = false;
	bool bTransientPackageDirty = false;
	bool bTransientPackageFileExists = false;
	TArray<FString> RegistryEntries;
};

FAuthorityResidueSnapshot CaptureResidue(
	const UClass& AssetClass,
	const UDataAssetFactory& Factory)
{
	FAuthorityResidueSnapshot Result;
	Result.bClassPackageDirty = AssetClass.GetOutermost()->IsDirty();
	Result.bFactoryPackageDirty = Factory.GetOutermost()->IsDirty();
	Result.bTransientPackageDirty = GetTransientPackage()->IsDirty();
	Result.bTransientPackageFileExists = FPackageName::DoesPackageExist(
		GetTransientPackage()->GetName());
	Result.RegistryEntries = SnapshotTransientRegistryEntries();
	return Result;
}

void TestResidueUnchanged(
	FAutomationTestBase& Test,
	const TCHAR* Row,
	const FAuthorityResidueSnapshot& Before,
	const FAuthorityResidueSnapshot& After)
{
	Test.TestEqual(
		*FString::Printf(TEXT("%s class package dirty is unchanged"), Row),
		After.bClassPackageDirty,
		Before.bClassPackageDirty);
	Test.TestEqual(
		*FString::Printf(TEXT("%s factory package dirty is unchanged"), Row),
		After.bFactoryPackageDirty,
		Before.bFactoryPackageDirty);
	Test.TestEqual(
		*FString::Printf(TEXT("%s transient package dirty is unchanged"), Row),
		After.bTransientPackageDirty,
		Before.bTransientPackageDirty);
	Test.TestEqual(
		*FString::Printf(TEXT("%s transient package file state is unchanged"), Row),
		After.bTransientPackageFileExists,
		Before.bTransientPackageFileExists);
	Test.TestEqual(
		*FString::Printf(TEXT("%s AssetRegistry entry set is unchanged"), Row),
		After.RegistryEntries,
		Before.RegistryEntries);
}

UDataAssetFactory* ResolveFactoryCdoForAuthorityProbe(
	const UClass* AssetClass)
{
	UDataAssetFactory* Factory =
		UDataAssetFactory::StaticClass()
			->GetDefaultObject<UDataAssetFactory>();
	if (Factory == nullptr
		|| AssetClass == nullptr
		|| !AssetClass->IsChildOf(UDataAsset::StaticClass())
		|| AssetClass->HasAnyClassFlags(
			CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
		|| (Factory->DataAssetClass != nullptr
			&& Factory->DataAssetClass != AssetClass))
	{
		return nullptr;
	}
	return Factory;
}

bool RunAdmittedFactoryRow(
	FAutomationTestBase& Test,
	UDataAssetFactory& Factory,
	UClass* InitialFactoryClass,
	const TCHAR* Row)
{
	UClass* const RequestedClass = UPrimaryAssetLabel::StaticClass();
	FScopedFactoryDataAssetClass FactoryState(Factory, InitialFactoryClass);
	const TSubclassOf<UDataAsset> ExpectedFactoryClass =
		Factory.DataAssetClass;
	const FAuthorityResidueSnapshot Before =
		CaptureResidue(*RequestedClass, Factory);
	FScopedLevelDirtiedCounter LevelDirtied;

	UDataAssetFactory* const AdmittedFactory =
		ResolveFactoryCdoForAuthorityProbe(RequestedClass);
	if (!Test.TestEqual(
		*FString::Printf(TEXT("%s exact factory CDO is admitted"), Row),
		AdmittedFactory,
		&Factory))
	{
		return false;
	}

	const FName ScratchName = MakeUniqueObjectName(
		GetTransientPackage(),
		RequestedClass,
		*FString::Printf(TEXT("DA_CreateScratch_%s"), Row));
	TStrongObjectPtr<UObject> Scratch(AdmittedFactory->FactoryCreateNew(
		RequestedClass,
		GetTransientPackage(),
		ScratchName,
		RF_Transient,
		nullptr,
		GWarn));
	if (!Test.TestNotNull(
		*FString::Printf(TEXT("%s helper returns an object"), Row),
		Scratch.Get()))
	{
		return false;
	}

	Test.TestEqual(
		*FString::Printf(TEXT("%s exact class"), Row),
		Scratch->GetClass(),
		RequestedClass);
	Test.TestEqual(
		*FString::Printf(TEXT("%s exact outer"), Row),
		Scratch->GetOuter(),
		static_cast<UObject*>(GetTransientPackage()));
	Test.TestTrue(
		*FString::Printf(TEXT("%s is transient"), Row),
		Scratch->HasAnyFlags(RF_Transient));
	Test.TestFalse(
		*FString::Printf(TEXT("%s is not public or standalone"), Row),
		Scratch->HasAnyFlags(RF_Public | RF_Standalone));
	Test.TestEqual(
		*FString::Printf(TEXT("%s factory state is not mutated"), Row),
		Factory.DataAssetClass,
		ExpectedFactoryClass);
	Test.TestEqual(
		*FString::Printf(TEXT("%s emits no level-dirtied delegate"), Row),
		LevelDirtied.GetCount(),
		0);
	TestResidueUnchanged(
		Test,
		Row,
		Before,
		CaptureResidue(*RequestedClass, Factory));

	TWeakObjectPtr<UObject> WeakScratch(Scratch.Get());
	Scratch.Reset();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	Test.TestFalse(
		*FString::Printf(TEXT("%s scratch is collected after release"), Row),
		WeakScratch.IsValid());
	TestResidueUnchanged(
		Test,
		Row,
		Before,
		CaptureResidue(*RequestedClass, Factory));
	return true;
}

bool RunMismatchedFactoryRow(
	FAutomationTestBase& Test,
	UDataAssetFactory& Factory)
{
	UClass* const RequestedClass = UPrimaryAssetLabel::StaticClass();
	UClass* const MismatchedClass = UTextureCollection::StaticClass();
	FScopedFactoryDataAssetClass FactoryState(Factory, MismatchedClass);
	const FAuthorityResidueSnapshot Before =
		CaptureResidue(*RequestedClass, Factory);
	FScopedLevelDirtiedCounter LevelDirtied;
	const FName ScratchName = MakeUniqueObjectName(
		GetTransientPackage(),
		RequestedClass,
		TEXT("DA_CreateScratch_Mismatched"));

	Test.TestTrue(
		TEXT("mismatched class is an admitted concrete DataAsset family"),
		MismatchedClass->IsChildOf(UDataAsset::StaticClass())
			&& !MismatchedClass->HasAnyClassFlags(
				CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists));
	Test.TestNull(
		TEXT("mismatched factory CDO state is rejected before helper call"),
		ResolveFactoryCdoForAuthorityProbe(RequestedClass));
	Test.TestNull(
		TEXT("mismatched row creates no object"),
		FindObjectFast<UObject>(GetTransientPackage(), ScratchName));
	Test.TestEqual(
		TEXT("mismatched row leaves factory state unchanged"),
		Factory.DataAssetClass,
		TSubclassOf<UDataAsset>(MismatchedClass));
	Test.TestEqual(
		TEXT("mismatched row emits no level-dirtied delegate"),
		LevelDirtied.GetCount(),
		0);
	TestResidueUnchanged(
		Test,
		TEXT("Mismatched"),
		Before,
		CaptureResidue(*RequestedClass, Factory));
	return true;
}

using namespace UE::AssetDocument::EditorPrimitive;

class FCreateScratchClassPayload final : public INativeIdentityPayload
{
public:
	explicit FCreateScratchClassPayload(UClass& InClass)
		: Class(&InClass)
		, CanonicalClassPath(InClass.GetPathName())
	{
	}

	virtual FName GetPayloadTypeId() const override
	{
		return ClassPayloadTypeId;
	}

	virtual bool IsAlive() const override
	{
		return Class.IsValid();
	}

	virtual bool TryGetClassIdentity(FClassIdentityView& OutView) const override
	{
		OutView.Class = Class.Get();
		OutView.CanonicalClassPath = CanonicalClassPath;
		OutView.TypeRevision = 1;
		return OutView.Class != nullptr;
	}

private:
	TWeakObjectPtr<UClass> Class;
	FString CanonicalClassPath;
};

class FCreateScratchObservingResolver final
	: public IAssetDocumentHandleResolver
{
public:
	explicit FCreateScratchObservingResolver(
		const FAssetDocumentEditorProviderTestContext& Context)
		: Inner(Context)
	{
	}

	void SetRejectScratchIssue(bool bReject)
	{
		bRejectScratchIssue = bReject;
	}

	TWeakObjectPtr<UObject> GetLastIssuedScratch() const
	{
		return LastIssuedScratch;
	}

	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
	{
		if (Kind == EAssetDocumentHandleKind::Scratch && Payload)
		{
			INativeIdentityPayload* Identity = TryGetNativeIdentityPayload(
				*Payload, ScratchPayloadTypeId);
			FObjectIdentityView View;
			if (Identity != nullptr && Identity->TryGetObjectIdentity(View))
			{
				LastIssuedScratch = View.Object;
			}
			if (bRejectScratchIssue)
			{
				FAssetDocumentIssuedHandleResult Result;
				FAssetDocumentRuntimeDiagnostic& Diagnostic =
					Result.Diagnostics.AddDefaulted_GetRef();
				Diagnostic.Path = TEXT("/HandleResults/scratch");
				Diagnostic.Code = TEXT("TestHandle.RejectedScratchIssue");
				Diagnostic.Message =
					TEXT("The test resolver rejected the Scratch issue.");
				return Result;
			}
		}
		return Inner.Issue(Kind, Access, MoveTemp(Payload));
	}

	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleValidationContext& Context) override
	{
		return Inner.AcquireVerifiedLease(Handle, Context);
	}

	virtual FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation& Event) override
	{
		return Inner.Invalidate(Event);
	}

private:
	FAssetDocumentEditorProviderTestHandleResolver Inner;
	bool bRejectScratchIssue = false;
	TWeakObjectPtr<UObject> LastIssuedScratch;
};

const FAssetDocumentPrimitiveDescriptor* FindCreateScratchDescriptor(
	const IAssetDocumentPrimitiveProvider& Provider)
{
	for (const FAssetDocumentPrimitiveDescriptor& Descriptor :
		Provider.DescribePrimitives())
	{
		if (Descriptor.PrimitiveId
			== TEXT("editor.scratch.create_from_class.v1"))
		{
			return &Descriptor;
		}
	}
	return nullptr;
}

FAssetDocumentRuntimeValueDto MakeStringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeCreateScratchArguments(
	FString CategoryId = TEXT("data_asset_exact.v1"),
	FString FactoryId = TEXT("data_asset_factory.v1"))
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = {TEXT("category_id"), TEXT("factory_id")};
	Result.Elements = {
		MakeStringValue(MoveTemp(CategoryId)),
		MakeStringValue(MoveTemp(FactoryId))
	};
	return Result;
}

FAssetDocumentRuntimeValueDto MakeMalformedCreateScratchArguments()
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = {TEXT("category_id")};
	return Result;
}

FAssetDocumentIssuedHandleResult IssueClassHandle(
	FCreateScratchObservingResolver& Handles,
	UClass& Class)
{
	return Handles.Issue(
		EAssetDocumentHandleKind::Class,
		EAssetDocumentHandleAccess::ReadOnly,
		MakeUnique<FCreateScratchClassPayload>(Class));
}

FAssetDocumentHandleValidationContext MakeScratchLeaseContext(
	const FAssetDocumentEditorProviderTestContext& Context)
{
	FAssetDocumentHandleValidationContext Result;
	Result.RequestId = Context.RequestId;
	Result.TransactionId = Context.TransactionId;
	Result.RuntimeRevision = Context.RuntimeRevision;
	Result.ProviderId = Context.ProviderId;
	Result.Phase = EAssetDocumentRuntimeRequestPhase::Preflight;
	Result.RequiredKind = EAssetDocumentHandleKind::Scratch;
	Result.RequiredAccess = EAssetDocumentHandleAccess::Mutable;
	return Result;
}

const FAssetDocumentPrimitiveHandleResult* FindScratchResult(
	const FAssetDocumentPrimitiveOutcome& Outcome)
{
	return Outcome.HandleResults.FindByPredicate(
		[](const FAssetDocumentPrimitiveHandleResult& Result)
		{
			return Result.Name == TEXT("scratch");
		});
}

bool HasExactRequiredClassArgument(
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return Descriptor.HandleArgumentSchemas.Num() == 1
		&& Descriptor.HandleArgumentSchemas[0].Name == TEXT("asset_class")
		&& Descriptor.HandleArgumentSchemas[0].Kind
			== EAssetDocumentHandleKind::Class
		&& Descriptor.HandleArgumentSchemas[0].RequiredAccess
			== EAssetDocumentHandleAccess::ReadOnly
		&& Descriptor.HandleArgumentSchemas[0].bRequired;
}

bool HasExactScratchResult(
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	return Descriptor.HandleResultSchemas.Num() == 1
		&& Descriptor.HandleResultSchemas[0].Name == TEXT("scratch")
		&& Descriptor.HandleResultSchemas[0].Kind
			== EAssetDocumentHandleKind::Scratch
		&& Descriptor.HandleResultSchemas[0].GrantedAccess
			== EAssetDocumentHandleAccess::Mutable
		&& Descriptor.HandleResultSchemas[0].Cardinality
			== EAssetDocumentHandleResultCardinality::ExactlyOne;
}

void TestCreateScratchAdmission(
	FAutomationTestBase& Test,
	const FAssetDocumentPrimitiveDescriptor& Descriptor)
{
	const FAssetDocumentPrimitiveAdmissionEvidence& Admission =
		Descriptor.Admission;
	Test.TestEqual(TEXT("disposition uses an ordinary helper"),
		Admission.Disposition,
		EAssetDocumentPrimitiveDisposition::OrdinaryHelper);
	Test.TestEqual(TEXT("authority uses an exported Engine helper"),
		Admission.AuthorityPath,
		EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper);
	Test.TestEqual(TEXT("Engine version is exact"),
		Admission.ExactEngineVersion, FString(TEXT("5.7.4")));
	Test.TestEqual(TEXT("physical owner is Editor"),
		Admission.PhysicalOwner, EAssetDocumentPrimitiveOwner::Editor);
	Test.TestEqual(TEXT("public module is UnrealEd"),
		Admission.PublicModule, FString(TEXT("UnrealEd")));
	Test.TestEqual(TEXT("public headers are exact"),
		Admission.PublicHeader,
		FString(TEXT("Factories/DataAssetFactory.h;UObject/Class.h")));
	Test.TestEqual(TEXT("exported helper chain is exact"),
		Admission.ExportedAuthoritativeApi,
		FString(TEXT("UClass::GetDefaultObject<UDataAssetFactory>;"
			"UDataAssetFactory::FactoryCreateNew")));
	Test.TestEqual(TEXT("primary operation is exact"),
		Admission.PrimaryOperationId,
		FString(TEXT(
			"unrealed.data-asset-factory.create-transient-scratch.v1")));
	Test.TestEqual(TEXT("identity lifetime is request-context bound"),
		Admission.IdentityLifetimeContractId,
		FString(TEXT("request-context Scratch lifetime v1")));
	Test.TestEqual(TEXT("transaction contract is preflight-only"),
		Admission.TransactionContractId,
		FString(TEXT("preflight-no-active-transaction.v1")));
	Test.TestEqual(TEXT("rollback contract is destruction-only"),
		Admission.RollbackContractId,
		FString(TEXT(
			"request-context-destruction-no-live-residue.v1")));
	Test.TestEqual(TEXT("completion contract is exact"),
		Admission.CompletionContractId,
		FString(TEXT("exact-class-transient-zero-residue.v1")));
	Test.TestEqual(TEXT("verification scope is exact"),
		Admission.VerificationTestIds,
		TArray<FString>({TEXT(
			"UEAssetDoc.GenericProvider.CreateScratch")}));
	Test.TestTrue(TEXT("ordinary helper has no raw lifecycle marker"),
		Admission.SourceMarker.IsEmpty()
			&& Admission.RawLifecycleAllowlistId.IsEmpty());
}

enum class ECreateScratchFailureCase : uint8
{
	MalformedArguments,
	MissingValueField,
	ExtraValueField,
	DuplicateValueField,
	EmptyValue,
	WrongValueKind,
	MissingHandle,
	ForgedHandle,
	WrongKindHandle,
	UnsupportedClass,
	FactoryCdoMismatch,
	WrongPhase,
	ActiveTransaction,
	WrongCategory,
	WrongFactory,
	RejectedIssue,
};

const TCHAR* GetFailureCaseName(ECreateScratchFailureCase FailureCase)
{
	switch (FailureCase)
	{
	case ECreateScratchFailureCase::MalformedArguments:
		return TEXT("MalformedArguments");
	case ECreateScratchFailureCase::MissingValueField:
		return TEXT("MissingValueField");
	case ECreateScratchFailureCase::ExtraValueField:
		return TEXT("ExtraValueField");
	case ECreateScratchFailureCase::DuplicateValueField:
		return TEXT("DuplicateValueField");
	case ECreateScratchFailureCase::EmptyValue:
		return TEXT("EmptyValue");
	case ECreateScratchFailureCase::WrongValueKind:
		return TEXT("WrongValueKind");
	case ECreateScratchFailureCase::MissingHandle:
		return TEXT("MissingHandle");
	case ECreateScratchFailureCase::ForgedHandle:
		return TEXT("ForgedHandle");
	case ECreateScratchFailureCase::WrongKindHandle:
		return TEXT("WrongKindHandle");
	case ECreateScratchFailureCase::UnsupportedClass:
		return TEXT("UnsupportedClass");
	case ECreateScratchFailureCase::FactoryCdoMismatch:
		return TEXT("FactoryCdoMismatch");
	case ECreateScratchFailureCase::WrongPhase:
		return TEXT("WrongPhase");
	case ECreateScratchFailureCase::ActiveTransaction:
		return TEXT("ActiveTransaction");
	case ECreateScratchFailureCase::WrongCategory:
		return TEXT("WrongCategory");
	case ECreateScratchFailureCase::WrongFactory:
		return TEXT("WrongFactory");
	case ECreateScratchFailureCase::RejectedIssue:
		return TEXT("RejectedIssue");
	}
	return TEXT("Unknown");
}

void ConfigureFailureInvocation(
	ECreateScratchFailureCase FailureCase,
	FAssetDocumentPrimitiveInvocation& Invocation)
{
	if (FailureCase == ECreateScratchFailureCase::MalformedArguments)
	{
		Invocation.ValueArguments = MakeMalformedCreateScratchArguments();
	}
	else if (FailureCase == ECreateScratchFailureCase::MissingValueField)
	{
		Invocation.ValueArguments.ObjectKeys.Pop();
		Invocation.ValueArguments.Elements.Pop();
	}
	else if (FailureCase == ECreateScratchFailureCase::ExtraValueField)
	{
		Invocation.ValueArguments.ObjectKeys.Add(TEXT("extra"));
		Invocation.ValueArguments.Elements.Add(MakeStringValue(TEXT("extra")));
	}
	else if (FailureCase == ECreateScratchFailureCase::DuplicateValueField)
	{
		Invocation.ValueArguments.ObjectKeys = {
			TEXT("category_id"), TEXT("category_id"), TEXT("factory_id")};
		Invocation.ValueArguments.Elements = {
			MakeStringValue(TEXT("data_asset_exact.v1")),
			MakeStringValue(TEXT("data_asset_exact.v1")),
			MakeStringValue(TEXT("data_asset_factory.v1"))};
	}
	else if (FailureCase == ECreateScratchFailureCase::EmptyValue)
	{
		Invocation.ValueArguments.Elements[0].StringValue.Reset();
	}
	else if (FailureCase == ECreateScratchFailureCase::WrongValueKind)
	{
		Invocation.ValueArguments.Elements[1] = {};
		Invocation.ValueArguments.Elements[1].Kind =
			EAssetDocumentRuntimeValueKind::Integer;
		Invocation.ValueArguments.Elements[1].IntegerValue = 1;
	}
	else if (FailureCase == ECreateScratchFailureCase::WrongCategory)
	{
		Invocation.ValueArguments =
			MakeCreateScratchArguments(TEXT("other.v1"));
	}
	else if (FailureCase == ECreateScratchFailureCase::WrongFactory)
	{
		Invocation.ValueArguments = MakeCreateScratchArguments(
			TEXT("data_asset_exact.v1"), TEXT("other_factory.v1"));
	}
	if (FailureCase == ECreateScratchFailureCase::WrongPhase)
	{
		Invocation.Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	}
}

bool RunCreateScratchFailureCase(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	UDataAssetFactory& Factory,
	ECreateScratchFailureCase FailureCase)
{
	const TCHAR* Row = GetFailureCaseName(FailureCase);
	UClass* const RequestedClass = UPrimaryAssetLabel::StaticClass();
	TUniquePtr<FScopedFactoryDataAssetClass> FactoryMismatch;
	if (FailureCase == ECreateScratchFailureCase::FactoryCdoMismatch)
	{
		FactoryMismatch = MakeUnique<FScopedFactoryDataAssetClass>(
			Factory, UTextureCollection::StaticClass());
	}
	const FAuthorityResidueSnapshot Before =
		CaptureResidue(*RequestedClass, Factory);
	FScopedLevelDirtiedCounter LevelDirtied;
	TWeakObjectPtr<UObject> WeakScratch;
	{
		FAssetDocumentEditorProviderTestContext Context;
		FCreateScratchObservingResolver Handles(Context);
		Handles.SetRejectScratchIssue(
			FailureCase == ECreateScratchFailureCase::RejectedIssue);
		FAssetDocumentPrimitiveInvocation Invocation =
			MakeAssetDocumentEditorProviderTestInvocation(
				Descriptor, Context);
		Invocation.ValueArguments = MakeCreateScratchArguments();
		ConfigureFailureInvocation(FailureCase, Invocation);

		if (FailureCase != ECreateScratchFailureCase::MissingHandle)
		{
			UClass* InputClass =
				FailureCase == ECreateScratchFailureCase::UnsupportedClass
				? UObject::StaticClass()
				: RequestedClass;
			FAssetDocumentIssuedHandleResult Issued =
				IssueClassHandle(Handles, *InputClass);
			if (Test.TestTrue(
				*FString::Printf(TEXT("%s Class handle issues"), Row),
				Issued.Handle.IsSet()))
			{
				FAssetDocumentOpaqueHandle Handle =
					Issued.Handle.GetValue();
				if (FailureCase
					== ECreateScratchFailureCase::ForgedHandle)
				{
					++Handle.Generation;
				}
				if (FailureCase
					== ECreateScratchFailureCase::WrongKindHandle)
				{
					Handle.Kind = EAssetDocumentHandleKind::Asset;
				}
				Invocation.HandleArguments.Add(
					{TEXT("asset_class"), MoveTemp(Handle)});
			}
		}

		FAssetDocumentEditorProviderTestTransaction ActiveTransaction(Context);
		IAssetDocumentAtomicTransaction* Transaction =
			FailureCase == ECreateScratchFailureCase::ActiveTransaction
				? &ActiveTransaction
				: nullptr;
		const FAssetDocumentPrimitiveOutcome Outcome =
			Provider.Invoke(Invocation, Handles, Transaction);
		Test.TestEqual(
			*FString::Printf(TEXT("%s fails closed"), Row),
			Outcome.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Failed);
		Test.TestTrue(
			*FString::Printf(TEXT("%s returns no handles"), Row),
			Outcome.HandleResults.IsEmpty());
		Test.TestTrue(
			*FString::Printf(TEXT("%s returns no changed set"), Row),
			Outcome.ChangedSet.IsEmpty());
		Test.TestFalse(
			*FString::Printf(TEXT("%s has a diagnostic"), Row),
			Outcome.Diagnostics.IsEmpty());
		WeakScratch = Handles.GetLastIssuedScratch();
		TestResidueUnchanged(
			Test,
			Row,
			Before,
			CaptureResidue(*RequestedClass, Factory));
	}
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	Test.TestFalse(
		*FString::Printf(TEXT("%s leaves no live Scratch"), Row),
		WeakScratch.IsValid());
	Test.TestEqual(
		*FString::Printf(TEXT("%s emits no level-dirtied delegate"), Row),
		LevelDirtied.GetCount(),
		0);
	TestResidueUnchanged(
		Test,
		Row,
		Before,
		CaptureResidue(*RequestedClass, Factory));
	return true;
}

enum class ECreateScratchTerminalScenario : uint8
{
	SuccessfulConsumer,
	InjectedFailureAfterHelper,
	CancellationAfterIssue,
	AbandonedBeforeConsumer,
};

const TCHAR* GetTerminalScenarioName(
	ECreateScratchTerminalScenario Scenario)
{
	switch (Scenario)
	{
	case ECreateScratchTerminalScenario::SuccessfulConsumer:
		return TEXT("SuccessfulConsumer");
	case ECreateScratchTerminalScenario::InjectedFailureAfterHelper:
		return TEXT("InjectedFailureAfterHelper");
	case ECreateScratchTerminalScenario::CancellationAfterIssue:
		return TEXT("CancellationAfterIssue");
	case ECreateScratchTerminalScenario::AbandonedBeforeConsumer:
		return TEXT("AbandonedBeforeConsumer");
	}
	return TEXT("Unknown");
}

bool RunCreateScratchTerminalScenario(
	FAutomationTestBase& Test,
	IAssetDocumentPrimitiveProvider& Provider,
	const FAssetDocumentPrimitiveDescriptor& Descriptor,
	UDataAssetFactory& Factory,
	ECreateScratchTerminalScenario Scenario)
{
	const TCHAR* Row = GetTerminalScenarioName(Scenario);
	UClass* const RequestedClass = UPrimaryAssetLabel::StaticClass();
	const FAuthorityResidueSnapshot Before =
		CaptureResidue(*RequestedClass, Factory);
	FScopedLevelDirtiedCounter LevelDirtied;
	TWeakObjectPtr<UObject> WeakScratch;
	{
		FAssetDocumentEditorProviderTestContext Context;
		FCreateScratchObservingResolver Handles(Context);
		const bool bRejectIssue =
			Scenario
			== ECreateScratchTerminalScenario::InjectedFailureAfterHelper;
		Handles.SetRejectScratchIssue(bRejectIssue);
		FAssetDocumentIssuedHandleResult ClassResult =
			IssueClassHandle(Handles, *RequestedClass);
		if (!Test.TestTrue(
			*FString::Printf(TEXT("%s Class handle issues"), Row),
			ClassResult.Handle.IsSet()))
		{
			return false;
		}
		FAssetDocumentPrimitiveInvocation Invocation =
			MakeAssetDocumentEditorProviderTestInvocation(
				Descriptor, Context);
		Invocation.ValueArguments = MakeCreateScratchArguments();
		Invocation.HandleArguments.Add(
			{TEXT("asset_class"), ClassResult.Handle.GetValue()});
		const FAssetDocumentPrimitiveOutcome Outcome =
			Provider.Invoke(Invocation, Handles, nullptr);
		const EAssetDocumentPrimitiveOutcomeStatus ExpectedStatus =
			bRejectIssue
				? EAssetDocumentPrimitiveOutcomeStatus::Failed
				: EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
		Test.TestEqual(
			*FString::Printf(TEXT("%s reaches its exact terminal"), Row),
			Outcome.Status,
			ExpectedStatus);
		WeakScratch = Handles.GetLastIssuedScratch();
		Test.TestTrue(
			*FString::Printf(
				TEXT("%s observes the helper-created Scratch"), Row),
			WeakScratch.IsValid());

		if (!bRejectIssue)
		{
			const FAssetDocumentPrimitiveHandleResult* ScratchResult =
				FindScratchResult(Outcome);
			if (Test.TestNotNull(
				*FString::Printf(TEXT("%s returns Scratch"), Row),
				ScratchResult))
			{
				const bool bOneScratch = Test.TestEqual(
					*FString::Printf(
						TEXT("%s returns one Scratch handle"), Row),
					ScratchResult->Handles.Num(),
					1);
				if (bOneScratch
					&& Scenario
						== ECreateScratchTerminalScenario::SuccessfulConsumer)
				{
					FAssetDocumentVerifiedLeaseResult Lease =
						Handles.AcquireVerifiedLease(
							ScratchResult->Handles[0],
							MakeScratchLeaseContext(Context));
					Test.TestNotNull(
						TEXT("successful consumer leases Scratch"),
						Lease.Lease.Get());
				}
			}
			if (Scenario
				== ECreateScratchTerminalScenario::CancellationAfterIssue)
			{
				FAssetDocumentHandleInvalidation Cancellation;
				Cancellation.Reason =
					EAssetDocumentHandleInvalidationReason::Rollback;
				Cancellation.RequestId = Context.RequestId;
				Cancellation.TransactionId = Context.TransactionId;
				Cancellation.RuntimeRevision = Context.RuntimeRevision;
				Cancellation.ProviderId = Context.ProviderId;
				Handles.Invalidate(Cancellation);
			}
		}
		TestResidueUnchanged(
			Test,
			Row,
			Before,
			CaptureResidue(*RequestedClass, Factory));
	}
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	Test.TestFalse(
		*FString::Printf(TEXT("%s Scratch is collected"), Row),
		WeakScratch.IsValid());
	Test.TestEqual(
		*FString::Printf(TEXT("%s emits no level-dirtied delegate"), Row),
		LevelDirtied.GetCount(),
		0);
	TestResidueUnchanged(
		Test,
		Row,
		Before,
		CaptureResidue(*RequestedClass, Factory));
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentCreateScratchEngineHelperAuthorityTest,
	"UEAssetDoc.GenericProvider.CreateScratch.EngineHelperAuthority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCreateScratchEngineHelperAuthorityTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorCreateScratchProviderTests;
	UDataAssetFactory* const Factory =
		UDataAssetFactory::StaticClass()
			->GetDefaultObject<UDataAssetFactory>();
	if (!TestNotNull(TEXT("exact UDataAssetFactory CDO exists"), Factory))
	{
		return false;
	}

	RunAdmittedFactoryRow(*this, *Factory, nullptr, TEXT("Null"));
	RunAdmittedFactoryRow(
		*this,
		*Factory,
		UPrimaryAssetLabel::StaticClass(),
		TEXT("Equal"));
	RunMismatchedFactoryRow(*this, *Factory);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentCreateScratchDescriptorContractTest,
	"UEAssetDoc.GenericProvider.CreateScratch.DescriptorContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCreateScratchDescriptorContractTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorCreateScratchProviderTests;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindCreateScratchDescriptor(*Provider);
	if (!TestNotNull(TEXT("create scratch descriptor exists"), Descriptor))
	{
		return false;
	}

	TestEqual(TEXT("primitive id is exact"),
		Descriptor->PrimitiveId,
		FString(TEXT("editor.scratch.create_from_class.v1")));
	TestEqual(TEXT("version is exact"), Descriptor->Version, 1u);
	TestEqual(TEXT("provider is editor"),
		Descriptor->ProviderId, FString(TEXT("editor")));
	TestEqual(TEXT("owner is Editor"),
		Descriptor->Owner, EAssetDocumentPrimitiveOwner::Editor);
	TestEqual(TEXT("execution is game-thread bound"),
		Descriptor->ExecutionThread,
		EAssetDocumentExecutionThread::GameThread);
	TestEqual(TEXT("access is Scratch"),
		Descriptor->Access, EAssetDocumentPrimitiveAccess::Scratch);
	TestEqual(TEXT("phase is exactly Preflight"),
		Descriptor->AllowedPhases,
		TArray<EAssetDocumentRuntimeRequestPhase>({
			EAssetDocumentRuntimeRequestPhase::Preflight}));
	TestFalse(TEXT("active transaction is forbidden"),
		Descriptor->bRequiresActiveTransaction);
	TestEqual(TEXT("allowed handle kinds are exact"),
		Descriptor->AllowedHandleKinds,
		TArray<EAssetDocumentHandleKind>({
			EAssetDocumentHandleKind::Class,
			EAssetDocumentHandleKind::Scratch}));
	TestTrue(TEXT("input is exactly required Class/ReadOnly"),
		HasExactRequiredClassArgument(*Descriptor));
	TestTrue(TEXT("result is exactly Scratch/Mutable/ExactlyOne"),
		HasExactScratchResult(*Descriptor));
	TestEqual(TEXT("consumer is exact"),
		Descriptor->RequiredConsumerIds,
		TArray<FString>({TEXT("route.generic.v1")}));
	TestCreateScratchAdmission(*this, *Descriptor);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentCreateScratchSuccessZeroResidueTest,
	"UEAssetDoc.GenericProvider.CreateScratch.SuccessZeroResidue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCreateScratchSuccessZeroResidueTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorCreateScratchProviderTests;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindCreateScratchDescriptor(*Provider);
	if (!TestNotNull(TEXT("create scratch descriptor exists"), Descriptor))
	{
		return false;
	}
	UDataAssetFactory* const Factory =
		UDataAssetFactory::StaticClass()
			->GetDefaultObject<UDataAssetFactory>();
	if (!TestNotNull(TEXT("exact UDataAssetFactory CDO exists"), Factory))
	{
		return false;
	}

	UClass* const RequestedClass = UPrimaryAssetLabel::StaticClass();
	FScopedFactoryDataAssetClass FactoryState(*Factory, nullptr);
	const FAuthorityResidueSnapshot Before =
		CaptureResidue(*RequestedClass, *Factory);
	FScopedLevelDirtiedCounter LevelDirtied;
	TWeakObjectPtr<UObject> WeakScratch;
	{
		FAssetDocumentEditorProviderTestContext Context;
		FCreateScratchObservingResolver Handles(Context);
		FAssetDocumentIssuedHandleResult ClassResult =
			IssueClassHandle(Handles, *RequestedClass);
		if (!TestTrue(TEXT("admitted Class handle issues"),
			ClassResult.Handle.IsSet()))
		{
			return false;
		}
		FAssetDocumentPrimitiveInvocation Invocation =
			MakeAssetDocumentEditorProviderTestInvocation(
				*Descriptor, Context);
		Invocation.ValueArguments = MakeCreateScratchArguments();
		Invocation.HandleArguments.Add(
			{TEXT("asset_class"), ClassResult.Handle.GetValue()});
		const FAssetDocumentPrimitiveOutcome Outcome =
			Provider->Invoke(Invocation, Handles, nullptr);
		if (!TestEqual(TEXT("create scratch succeeds"),
			Outcome.Status,
			EAssetDocumentPrimitiveOutcomeStatus::Succeeded))
		{
			return false;
		}
		TestEqual(TEXT("output key order is exact"),
			Outcome.Value.ObjectKeys,
			TArray<FString>({
				TEXT("category_id"),
				TEXT("factory_id"),
				TEXT("canonical_class"),
				TEXT("scratch_path")}));
		if (!TestEqual(TEXT("output has four values"),
			Outcome.Value.Elements.Num(), 4))
		{
			return false;
		}
		TestEqual(TEXT("output kind is Object"),
			Outcome.Value.Kind, EAssetDocumentRuntimeValueKind::Object);
		for (int32 Index = 0; Index < Outcome.Value.Elements.Num(); ++Index)
		{
			TestEqual(
				*FString::Printf(TEXT("output value %d is String"), Index),
				Outcome.Value.Elements[Index].Kind,
				EAssetDocumentRuntimeValueKind::String);
		}
		TestEqual(TEXT("category output is exact"),
			Outcome.Value.Elements[0].StringValue,
			FString(TEXT("data_asset_exact.v1")));
		TestEqual(TEXT("factory output is exact"),
			Outcome.Value.Elements[1].StringValue,
			FString(TEXT("data_asset_factory.v1")));
		TestEqual(TEXT("class output is canonical"),
			Outcome.Value.Elements[2].StringValue,
			RequestedClass->GetPathName());
		const FAssetDocumentPrimitiveHandleResult* ScratchResult =
			FindScratchResult(Outcome);
		if (!TestNotNull(TEXT("one named Scratch result exists"),
			ScratchResult)
			|| !TestEqual(TEXT("one Scratch handle is returned"),
				ScratchResult->Handles.Num(), 1))
		{
			return false;
		}
		const FAssetDocumentOpaqueHandle& ScratchHandle =
			ScratchResult->Handles[0];
		TestEqual(TEXT("Scratch kind is exact"),
			ScratchHandle.Kind, EAssetDocumentHandleKind::Scratch);
		TestEqual(TEXT("Scratch access is Mutable"),
			ScratchHandle.Access, EAssetDocumentHandleAccess::Mutable);
		FAssetDocumentVerifiedLeaseResult Lease =
			Handles.AcquireVerifiedLease(
				ScratchHandle,
				MakeScratchLeaseContext(Context));
		if (!TestNotNull(TEXT("same request leases Scratch"),
			Lease.Lease.Get()))
		{
			return false;
		}
		INativeIdentityPayload* Payload = TryGetNativeIdentityPayload(
			Lease.Lease->GetPayloadChecked(), ScratchPayloadTypeId);
		FObjectIdentityView Identity;
		UObject* Scratch = Payload != nullptr
			&& Payload->TryGetObjectIdentity(Identity)
				? Identity.Object
				: nullptr;
		if (!TestNotNull(TEXT("Scratch native identity resolves"), Scratch))
		{
			return false;
		}
		WeakScratch = Scratch;
		TestEqual(TEXT("Scratch class is exact"),
			Scratch->GetClass(), RequestedClass);
		TestEqual(TEXT("Scratch outer is transient"),
			Scratch->GetOuter(), static_cast<UObject*>(GetTransientPackage()));
		TestTrue(TEXT("Scratch has RF_Transient"),
			Scratch->HasAnyFlags(RF_Transient));
		TestFalse(TEXT("Scratch has no public or standalone flags"),
			Scratch->HasAnyFlags(RF_Public | RF_Standalone));
		TestEqual(TEXT("Scratch path output matches native identity"),
			Outcome.Value.Elements[3].StringValue,
			FString(Identity.CanonicalObjectPath));
		TestResidueUnchanged(
			*this,
			TEXT("Success"),
			Before,
			CaptureResidue(*RequestedClass, *Factory));
		TestEqual(TEXT("success emits no level-dirtied delegate"),
			LevelDirtied.GetCount(), 0);
	}
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestFalse(TEXT("request-context release collects Scratch"),
		WeakScratch.IsValid());
	TestResidueUnchanged(
		*this,
		TEXT("SuccessAfterRelease"),
		Before,
		CaptureResidue(*RequestedClass, *Factory));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentCreateScratchFailureZeroResidueTest,
	"UEAssetDoc.GenericProvider.CreateScratch.FailureZeroResidue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCreateScratchFailureZeroResidueTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorCreateScratchProviderTests;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindCreateScratchDescriptor(*Provider);
	if (!TestNotNull(TEXT("create scratch descriptor exists"), Descriptor))
	{
		return false;
	}
	UDataAssetFactory* const Factory =
		UDataAssetFactory::StaticClass()
			->GetDefaultObject<UDataAssetFactory>();
	if (!TestNotNull(TEXT("exact UDataAssetFactory CDO exists"), Factory))
	{
		return false;
	}
	FScopedFactoryDataAssetClass FactoryState(*Factory, nullptr);
	const ECreateScratchFailureCase FailureCases[] = {
		ECreateScratchFailureCase::MalformedArguments,
		ECreateScratchFailureCase::MissingValueField,
		ECreateScratchFailureCase::ExtraValueField,
		ECreateScratchFailureCase::DuplicateValueField,
		ECreateScratchFailureCase::EmptyValue,
		ECreateScratchFailureCase::WrongValueKind,
		ECreateScratchFailureCase::MissingHandle,
		ECreateScratchFailureCase::ForgedHandle,
		ECreateScratchFailureCase::WrongKindHandle,
		ECreateScratchFailureCase::UnsupportedClass,
		ECreateScratchFailureCase::FactoryCdoMismatch,
		ECreateScratchFailureCase::WrongPhase,
		ECreateScratchFailureCase::ActiveTransaction,
		ECreateScratchFailureCase::WrongCategory,
		ECreateScratchFailureCase::WrongFactory,
		ECreateScratchFailureCase::RejectedIssue,
	};
	for (ECreateScratchFailureCase FailureCase : FailureCases)
	{
		RunCreateScratchFailureCase(
			*this, *Provider, *Descriptor, *Factory, FailureCase);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetDocumentCreateScratchTerminalCleanupTest,
	"UEAssetDoc.GenericProvider.CreateScratch.TerminalCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCreateScratchTerminalCleanupTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentEditorCreateScratchProviderTests;
	const TSharedRef<IAssetDocumentPrimitiveProvider> Provider =
		CreateAssetDocumentEditorPrimitiveProvider();
	const FAssetDocumentPrimitiveDescriptor* Descriptor =
		FindCreateScratchDescriptor(*Provider);
	if (!TestNotNull(TEXT("create scratch descriptor exists"), Descriptor))
	{
		return false;
	}
	UDataAssetFactory* const Factory =
		UDataAssetFactory::StaticClass()
			->GetDefaultObject<UDataAssetFactory>();
	if (!TestNotNull(TEXT("exact UDataAssetFactory CDO exists"), Factory))
	{
		return false;
	}
	FScopedFactoryDataAssetClass FactoryState(*Factory, nullptr);
	const ECreateScratchTerminalScenario Scenarios[] = {
		ECreateScratchTerminalScenario::SuccessfulConsumer,
		ECreateScratchTerminalScenario::InjectedFailureAfterHelper,
		ECreateScratchTerminalScenario::CancellationAfterIssue,
		ECreateScratchTerminalScenario::AbandonedBeforeConsumer,
	};
	for (ECreateScratchTerminalScenario Scenario : Scenarios)
	{
		RunCreateScratchTerminalScenario(
			*this, *Provider, *Descriptor, *Factory, Scenario);
	}
	return true;
}
#endif
