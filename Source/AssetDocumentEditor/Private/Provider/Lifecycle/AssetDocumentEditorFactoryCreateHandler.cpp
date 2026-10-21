// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Lifecycle/AssetDocumentEditorLifecycleHandlers.h"

#include "AssetDocumentEditorDialogFactory.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/DataAssetFactory.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"
#include "Misc/PackageName.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/Cutover/AssetDocumentEditorMutationFootprintBuilders.h"
#include "Provider/Lifecycle/AssetDocumentEditorEditorSafety.h"
#include "Provider/Lifecycle/AssetDocumentEditorGenericFactory.h"
#include "Provider/Lifecycle/AssetDocumentEditorLifecycleAdmission.h"
#include "Provider/Lifecycle/AssetDocumentEditorLifecyclePayloads.h"
#include "Provider/Lifecycle/AssetDocumentEditorOrphanPackageCleanup.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

namespace EditorFactoryCreateHandlerLocal
{
using namespace UE::AssetDocument::EditorPrimitive;

constexpr const TCHAR* DataAssetFactoryId = TEXT("data_asset_factory.v1");
constexpr const TCHAR* BehaviorTreeFactoryId = TEXT("behavior_tree_factory.v1");
const FTopLevelAssetPath BehaviorTreeFactoryClassPath(
	TEXT("/Script/BehaviorTreeEditor"), TEXT("BehaviorTreeFactory"));

struct FFactoryCreateArguments
{
	FString Factory;
	FString PackagePath;
	FString AssetName;
	FString PackageName;
	FString CanonicalObjectPath;
	FString PackageFilename;
};

struct FFactoryTargetEvidence
{
	bool bObjectExisted = false;
	bool bPackageExisted = false;
	bool bFileExisted = false;
	bool bPackageWasDirty = false;
};

FAssetDocumentPrimitiveOutcome MakeFactoryFailure(
	const TCHAR* Code,
	const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	if (!Diagnostics.IsEmpty())
	{
		Outcome.Diagnostics.Append(Diagnostics.GetData(), Diagnostics.Num());
		return Outcome;
	}
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Outcome.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/Primitive");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Outcome;
}

FAssetDocumentPrimitiveOutcome MakeFactoryFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	return MakeFactoryFailure(
		Code, Message, TConstArrayView<FAssetDocumentRuntimeDiagnostic>());
}

bool TryCompleteFactoryArguments(FFactoryCreateArguments& OutArguments);

bool TryReadFactoryArguments(
	const FAssetDocumentRuntimeValueDto& Value,
	FFactoryCreateArguments& OutArguments)
{
	const FAssetDocumentEditorValueFieldSpec Fields[] = {
		{TEXT("factory_id"), EAssetDocumentRuntimeValueKind::String},
		{TEXT("package_path"), EAssetDocumentRuntimeValueKind::String},
		{TEXT("asset_name"), EAssetDocumentRuntimeValueKind::String},
	};
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString DiagnosticCode;
	if (!TryReadAssetDocumentEditorExactObject(
		Value, Fields, Values, DiagnosticCode))
	{
		return false;
	}
	OutArguments.Factory = Values[0]->StringValue;
	OutArguments.PackagePath = Values[1]->StringValue;
	OutArguments.AssetName = Values[2]->StringValue;
	return TryCompleteFactoryArguments(OutArguments);
}

// v2 has no factory_id: the factory is selected from the class, as the editor does.
bool TryReadGenericFactoryArguments(
	const FAssetDocumentRuntimeValueDto& Value,
	FFactoryCreateArguments& OutArguments)
{
	const FAssetDocumentEditorValueFieldSpec Fields[] = {
		{TEXT("package_path"), EAssetDocumentRuntimeValueKind::String},
		{TEXT("asset_name"), EAssetDocumentRuntimeValueKind::String},
	};
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString DiagnosticCode;
	if (!TryReadAssetDocumentEditorExactObject(
		Value, Fields, Values, DiagnosticCode))
	{
		return false;
	}
	OutArguments.Factory = AssetDocumentEditorGenericFactoryLifecycleId;
	OutArguments.PackagePath = Values[0]->StringValue;
	OutArguments.AssetName = Values[1]->StringValue;
	return TryCompleteFactoryArguments(OutArguments);
}

bool TryCompleteFactoryArguments(FFactoryCreateArguments& OutArguments)
{
	OutArguments.PackageName = FString::Printf(
		TEXT("%s/%s"),
		*OutArguments.PackagePath,
		*OutArguments.AssetName);
	OutArguments.CanonicalObjectPath = FString::Printf(
		TEXT("%s.%s"),
		*OutArguments.PackageName,
		*OutArguments.AssetName);
	OutArguments.PackageFilename =
		FPackageName::LongPackageNameToFilename(
			OutArguments.PackageName,
			FPackageName::GetAssetPackageExtension());
	const FSoftObjectPath ObjectPath(OutArguments.CanonicalObjectPath);
	return FPackageName::IsValidLongPackageName(
			OutArguments.PackagePath)
		&& FPackageName::IsValidObjectPath(
			OutArguments.CanonicalObjectPath)
		&& ObjectPath.ToString() == OutArguments.CanonicalObjectPath;
}

const FAssetDocumentPrimitiveHandleArgument* FindFactoryClassArgument(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	if (Invocation.HandleArguments.Num() == 1
		&& Invocation.HandleArguments[0].Name == TEXT("asset_class"))
	{
		return &Invocation.HandleArguments[0];
	}
	return nullptr;
}

FAssetDocumentHandleValidationContext MakeFactoryClassLeaseContext(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Invocation.RequestId;
	Context.TransactionId = Invocation.TransactionId;
	Context.RuntimeRevision = Invocation.RuntimeRevision;
	Context.ProviderId = TEXT("editor");
	Context.Phase = Invocation.Phase;
	Context.RequiredKind = EAssetDocumentHandleKind::Class;
	Context.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
	return Context;
}

bool TryAcquireFactoryClassLease(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FAssetDocumentVerifiedLeaseResult& OutLease)
{
	const FAssetDocumentPrimitiveHandleArgument* Argument =
		FindFactoryClassArgument(Invocation);
	if (Argument == nullptr)
	{
		return false;
	}
	OutLease = Handles.AcquireVerifiedLease(
		Argument->Handle,
		MakeFactoryClassLeaseContext(Invocation));
	return OutLease.Lease.IsValid() && OutLease.Diagnostics.IsEmpty();
}

// v2: exactly asset_class, plus the optional factory_context asset and, for a factory whose dialog
// sets two objects, factory_context_2 (only together with factory_context).
bool TryFindGenericFactoryArguments(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveHandleArgument*& OutClass,
	const FAssetDocumentPrimitiveHandleArgument*& OutContext,
	const FAssetDocumentPrimitiveHandleArgument*& OutContext2)
{
	OutClass = nullptr;
	OutContext = nullptr;
	OutContext2 = nullptr;
	for (const FAssetDocumentPrimitiveHandleArgument& Argument : Invocation.HandleArguments)
	{
		const FAssetDocumentPrimitiveHandleArgument** Slot =
			Argument.Name == TEXT("asset_class") ? &OutClass
			: Argument.Name == TEXT("factory_context") ? &OutContext
			: Argument.Name == TEXT("factory_context_2") ? &OutContext2 : nullptr;
		if (Slot == nullptr || *Slot != nullptr)
		{
			OutClass = nullptr;
			return false;
		}
		*Slot = &Argument;
	}
	return OutClass != nullptr && (OutContext2 == nullptr || OutContext != nullptr);
}

UObject* TryAcquireFactoryContext(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveHandleArgument& Argument,
	IAssetDocumentHandleResolver& Handles,
	FAssetDocumentVerifiedLeaseResult& OutLease)
{
	FAssetDocumentHandleValidationContext Context = MakeFactoryClassLeaseContext(Invocation);
	Context.RequiredKind = EAssetDocumentHandleKind::Asset;
	OutLease = Handles.AcquireVerifiedLease(Argument.Handle, Context);
	if (!OutLease.Lease.IsValid() || !OutLease.Diagnostics.IsEmpty())
	{
		return nullptr;
	}
	INativeIdentityPayload* Payload = TryGetNativeIdentityPayload(
		OutLease.Lease->GetPayloadChecked(), AssetPayloadTypeId);
	FObjectIdentityView Identity;
	return Payload != nullptr && Payload->TryGetObjectIdentity(Identity) && Identity.Object != nullptr
			&& Identity.CanonicalObjectPath == Identity.Object->GetPathName()
		? Identity.Object
		: nullptr;
}

UClass* TryGetCanonicalFactoryClass(
	IAssetDocumentVerifiedHandleLease& Lease)
{
	INativeIdentityPayload* Payload = TryGetNativeIdentityPayload(
		Lease.GetPayloadChecked(), ClassPayloadTypeId);
	if (Payload == nullptr)
	{
		return nullptr;
	}
	FClassIdentityView Identity;
	if (!Payload->TryGetClassIdentity(Identity))
	{
		return nullptr;
	}
	if (Identity.Class == nullptr)
	{
		return nullptr;
	}
	if (Identity.CanonicalClassPath != Identity.Class->GetPathName())
	{
		return nullptr;
	}
	return Identity.Class;
}

UClass* TryGetAdmittedFactoryClass(
	IAssetDocumentVerifiedHandleLease& Lease)
{
	UClass* AssetClass = TryGetCanonicalFactoryClass(Lease);
	if (!IsAssetDocumentEditorFactoryClassAdmitted(AssetClass))
	{
		return nullptr;
	}
	return AssetClass;
}

bool IsExactDataAssetFactoryClass(const UFactory* Candidate)
{
	return Candidate != nullptr
		&& Candidate->GetClass() == UDataAssetFactory::StaticClass();
}

bool IsBehaviorTreeFactoryRequest(
	const FFactoryCreateArguments& Arguments,
	const UClass& AssetClass)
{
	return Arguments.Factory == BehaviorTreeFactoryId
		&& IsAssetDocumentEditorBehaviorTreeFactoryClassAdmitted(&AssetClass);
}

UFactory* FindExactDataAssetFactory(UClass& AssetClass)
{
	UFactory* const Found =
		UDataAssetFactory::StaticClass()->GetDefaultObject<UDataAssetFactory>();
	if (!IsAssetDocumentEditorDataAssetFactoryAdmitted(
		Found, &AssetClass))
	{
		return nullptr;
	}
	return Found;
}

UFactory* FindExactBehaviorTreeFactory(UClass& AssetClass)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("BehaviorTreeEditor")))
	{
		return nullptr;
	}
	UClass* const FactoryClass = Cast<UClass>(StaticFindObjectSafe(
		UClass::StaticClass(), BehaviorTreeFactoryClassPath,
		EFindObjectFlags::ExactClass));
	if (FactoryClass == nullptr
		|| !FactoryClass->IsChildOf(UFactory::StaticClass())
		|| FactoryClass->HasAnyClassFlags(
			CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		return nullptr;
	}
	UFactory* const Factory = Cast<UFactory>(FactoryClass->GetDefaultObject(false));
	return Factory != nullptr && Factory->GetSupportedClass() == &AssetClass
		&& Factory->CanCreateNew() ? Factory : nullptr;
}

FFactoryTargetEvidence CaptureFactoryTargetEvidence(
	const FFactoryCreateArguments& Arguments)
{
	FFactoryTargetEvidence Evidence;
	Evidence.bObjectExisted = FAssetRegistryModule::GetRegistry()
		.GetAssetByObjectPath(FSoftObjectPath(
			Arguments.CanonicalObjectPath))
		.IsValid();
	UPackage* ExistingPackage =
		FindPackage(nullptr, *Arguments.PackageName);
	Evidence.bPackageExisted = ExistingPackage != nullptr
		|| FPackageName::DoesPackageExist(Arguments.PackageName);
	Evidence.bFileExisted = IFileManager::Get().FileExists(
		*Arguments.PackageFilename);
	Evidence.bPackageWasDirty =
		ExistingPackage != nullptr && ExistingPackage->IsDirty();
	return Evidence;
}

bool HasFactoryTargetCollision(const FFactoryTargetEvidence& Evidence)
{
	return Evidence.bObjectExisted
		|| Evidence.bPackageExisted
		|| Evidence.bFileExisted;
}

FAssetDocumentRuntimeValueDto MakeFactoryStringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

bool TryBuildFactoryOutcomeValue(
	const FFactoryCreateArguments& Arguments,
	const UObject& Asset,
	FAssetDocumentRuntimeValueDto& OutValue)
{
	const FAssetDocumentEditorValueField Fields[] = {
		{TEXT("canonical_path"),
			MakeFactoryStringValue(Arguments.CanonicalObjectPath)},
		{TEXT("canonical_class"),
			MakeFactoryStringValue(Asset.GetClass()->GetPathName())},
		{TEXT("package_name"),
			MakeFactoryStringValue(Arguments.PackageName)},
		{TEXT("package_filename"),
			MakeFactoryStringValue(Arguments.PackageFilename)},
	};
	FString DiagnosticCode;
	return TryBuildAssetDocumentEditorValueObject(
		Fields, OutValue, DiagnosticCode);
}

bool IsFactoryIssueValid(
	const FAssetDocumentIssuedHandleResult& Result)
{
	return Result.Handle.IsSet() && Result.Diagnostics.IsEmpty();
}

void AddFactoryHandle(
	FAssetDocumentPrimitiveOutcome& Outcome,
	const TCHAR* Name,
	FAssetDocumentIssuedHandleResult& Issued)
{
	FAssetDocumentPrimitiveHandleResult& Result =
		Outcome.HandleResults.AddDefaulted_GetRef();
	Result.Name = Name;
	Result.Handles.Add(MoveTemp(Issued.Handle.GetValue()));
}

TSharedRef<const FAssetDocumentEditorCreationRecord> MakeCreationRecord(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FFactoryCreateArguments& Arguments,
	const FFactoryTargetEvidence& Evidence,
	UObject& Asset)
{
	return MakeShared<FAssetDocumentEditorCreationRecord>(
		Invocation.RequestId,
		Invocation.TransactionId,
		Invocation.RuntimeRevision,
		Arguments.Factory,
		Arguments.CanonicalObjectPath,
		Arguments.PackageName,
		Arguments.PackageFilename,
		Evidence.bObjectExisted,
		Evidence.bPackageExisted,
		Evidence.bFileExisted,
		Evidence.bPackageWasDirty,
		Asset,
		*Asset.GetOutermost());
}

FAssetDocumentPrimitiveOutcome MakeCreatedSuccess(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FFactoryCreateArguments& Arguments,
	const FFactoryTargetEvidence& Evidence,
	UObject& Asset,
	IAssetDocumentHandleResolver& Handles)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	if (!TryBuildFactoryOutcomeValue(Arguments, Asset, Outcome.Value))
	{
		return MakeFactoryFailure(
			TEXT("FactoryCreate.InvalidOutcome"),
			TEXT("Created-asset evidence could not be projected."));
	}
	TSharedRef<const FAssetDocumentEditorCreationRecord> Record =
		MakeCreationRecord(Invocation, Arguments, Evidence, Asset);
	FAssetDocumentIssuedHandleResult Read = Handles.Issue(
		EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly,
		MakeUnique<FAssetDocumentEditorCreatedAssetPayload>(
			Asset, Record));
	FAssetDocumentIssuedHandleResult Mutable = Handles.Issue(
		EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable,
		MakeUnique<FAssetDocumentEditorCreatedAssetPayload>(
			Asset, Record));
	FAssetDocumentIssuedHandleResult Package = Handles.Issue(
		EAssetDocumentHandleKind::Package,
		EAssetDocumentHandleAccess::Mutable,
		MakeUnique<FAssetDocumentEditorCreatedPackagePayload>(
			*Asset.GetOutermost(), Record));
	if (!IsFactoryIssueValid(Read)
		|| !IsFactoryIssueValid(Mutable)
		|| !IsFactoryIssueValid(Package))
	{
		return MakeFactoryFailure(
			TEXT("FactoryCreate.HandleIssueFailed"),
			TEXT("The request context rejected a created identity."));
	}
	AddFactoryHandle(Outcome, TEXT("asset_read"), Read);
	AddFactoryHandle(Outcome, TEXT("asset_mutable"), Mutable);
	AddFactoryHandle(Outcome, TEXT("package"), Package);
	return Outcome;
}

bool HasExactCreatedIdentity(
	const FFactoryCreateArguments& Arguments,
	UClass& AssetClass,
	const UObject* Asset)
{
	return Asset != nullptr
		&& Asset->GetClass() == &AssetClass
		&& Asset->GetPathName() == Arguments.CanonicalObjectPath
		&& Asset->GetOutermost()->GetName() == Arguments.PackageName;
}

bool RegistryOwnsCreatedIdentity(
	const FFactoryCreateArguments& Arguments,
	const UObject& Asset)
{
	const FAssetData AssetData = FAssetRegistryModule::GetRegistry()
		.GetAssetByObjectPath(FSoftObjectPath(
			Arguments.CanonicalObjectPath));
	return AssetData.IsValid() && AssetData.GetAsset() == &Asset;
}

FAssetDocumentPrimitiveOutcome CreateAndProjectAsset(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FFactoryCreateArguments& Arguments,
	const FFactoryTargetEvidence& Evidence,
	UClass& AssetClass,
	UFactory& Factory,
	IAssetDocumentHandleResolver& Handles)
{
	UObject* Asset = nullptr;
	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		// A factory that always opens its settings dialog (AssetDocumentEditorDialogFactory.h): the object
		// as AssetTools creates a factory-less class, then what the factory does after the dialog's OK.
		const FAssetDocumentEditorDialogFactoryInit* DialogInit =
			FindAssetDocumentEditorDialogFactory(*Factory.GetClass());
		Asset = IAssetTools::Get().CreateAsset(
			Arguments.AssetName,
			Arguments.PackagePath,
			&AssetClass,
			DialogInit != nullptr ? nullptr : &Factory,
			TEXT("AssetDocumentEditorFactoryCreateHandler"));
		FString InitMessage;
		if (DialogInit != nullptr && Asset != nullptr && !NonInteractive.WasInteractionRefused()
			&& !(*DialogInit)(Factory, *Asset, InitMessage))
		{
			FAssetRegistryModule::AssetDeleted(Asset);
			Asset->ClearFlags(RF_Standalone | RF_Public);
			Asset->MarkAsGarbage();
			if (!Evidence.bPackageExisted)
			{
				DiscardAssetDocumentEditorOrphanCreatedPackage(Arguments.PackageName);
			}
			NonInteractive.DiscardCreatedPackages();
			return MakeFactoryFailure(
				TEXT("FactoryCreate.DialogFactoryInitFailed"),
				*FString::Printf(TEXT("%s: %s"), *Factory.GetClass()->GetPathName(), *InitMessage));
		}
		if (NonInteractive.WasInteractionRefused())
		{
			// Whatever the cancelled factory produced is discarded: the target
			// (in the package this request introduced) and any side packages.
			if (Asset != nullptr && Evidence.bPackageExisted)
			{
				FAssetRegistryModule::AssetDeleted(Asset);
				Asset->ClearFlags(RF_Standalone | RF_Public);
				Asset->MarkAsGarbage();
			}
			if (!Evidence.bPackageExisted)
			{
				DiscardAssetDocumentEditorOrphanCreatedPackage(Arguments.PackageName);
			}
			NonInteractive.DiscardCreatedPackages();
			return MakeFactoryFailure(
				AssetDocumentEditorInteractiveFactoryCode,
				*FAssetDocumentEditorNonInteractiveScope::DescribeRefusal(Factory.GetClass(), &AssetClass));
		}
	}
	if (!HasExactCreatedIdentity(Arguments, AssetClass, Asset))
	{
		// AssetTools created the package before the factory failed; a package
		// this request introduced must not outlive the failed creation.
		// A wrong-class asset (an ancestor factory that ignored the requested
		// class) is discarded with it.
		if (!Evidence.bPackageExisted)
		{
			DiscardAssetDocumentEditorOrphanCreatedPackage(Arguments.PackageName);
		}
		return MakeFactoryFailure(
			TEXT("FactoryCreate.IncompleteCreation"),
			TEXT("The authoritative factory did not create the exact target."));
	}
	if (!RegistryOwnsCreatedIdentity(Arguments, *Asset))
	{
		return MakeFactoryFailure(
			TEXT("FactoryCreate.RegistryMismatch"),
			TEXT("The created target was not registered exactly."));
	}
	return MakeCreatedSuccess(
		Invocation, Arguments, Evidence, *Asset, Handles);
}

FAssetDocumentPrimitiveOutcome CreateWithFactory(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FFactoryCreateArguments& Arguments,
	UClass& AssetClass,
	UFactory& Factory,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction& Transaction);

FAssetDocumentPrimitiveOutcome CreateWithAdmittedClass(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FFactoryCreateArguments& Arguments,
	UClass& AssetClass,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction& Transaction)
{
	UFactory* Factory = IsBehaviorTreeFactoryRequest(Arguments, AssetClass)
		? FindExactBehaviorTreeFactory(AssetClass)
		: FindExactDataAssetFactory(AssetClass);
	if (Factory == nullptr)
	{
		return MakeFactoryFailure(
			TEXT("FactoryCreate.FactoryUnavailable"),
			TEXT("The exact non-null data asset factory is unavailable."));
	}
	return CreateWithFactory(
		Invocation, Arguments, AssetClass, *Factory, Handles, Transaction);
}

FAssetDocumentPrimitiveOutcome CreateWithFactory(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FFactoryCreateArguments& Arguments,
	UClass& AssetClass,
	UFactory& Factory,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction& Transaction)
{
	const FFactoryTargetEvidence Evidence =
		CaptureFactoryTargetEvidence(Arguments);
	if (HasFactoryTargetCollision(Evidence))
	{
		return MakeFactoryFailure(
			TEXT("FactoryCreate.TargetExists"),
			TEXT("The exact target already exists."));
	}
	FAssetDocumentEditorMutationIdentity Identity;
	if (!TryBuildAssetDocumentEditorMutationIdentity(
		Arguments.CanonicalObjectPath, Identity))
	{
		return MakeFactoryFailure(
			TEXT("FactoryCreate.InvalidTarget"),
			TEXT("The factory target identity is not canonical."));
	}
	const FAssetDocumentContractValidationResult Enlistment =
		Transaction.EnlistFootprint(
			BuildAssetDocumentEditorFactoryMutationFootprint(Identity));
	if (!Enlistment.bValid)
	{
		return MakeFactoryFailure(
			TEXT("FactoryCreate.FootprintRejected"),
			TEXT("The atomic transaction rejected the creation footprint."),
			Enlistment.Diagnostics);
	}
	return CreateAndProjectAsset(
		Invocation, Arguments, Evidence,
		AssetClass, Factory, Handles);
}
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorCreateAssetByFactory(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace EditorFactoryCreateHandlerLocal;
	FFactoryCreateArguments Arguments;
	if (!TryReadFactoryArguments(Invocation.ValueArguments, Arguments))
	{
		return MakeFactoryFailure(
			TEXT("FactoryCreate.InvalidArguments"),
			TEXT("Factory creation requires one exact target identity."));
	}
	if (Arguments.Factory != DataAssetFactoryId
		&& Arguments.Factory != BehaviorTreeFactoryId)
	{
		return MakeFactoryFailure(
			TEXT("FactoryCreate.UnsupportedFactory"),
			TEXT("The requested factory is not admitted."));
	}
	FAssetDocumentVerifiedLeaseResult ClassLease;
	if (!TryAcquireFactoryClassLease(Invocation, Handles, ClassLease))
	{
		return MakeFactoryFailure(
			TEXT("FactoryCreate.ClassLeaseFailed"),
			TEXT("The asset class handle could not be leased."),
			ClassLease.Diagnostics);
	}
	UClass* AssetClass = TryGetAdmittedFactoryClass(*ClassLease.Lease);
	if (AssetClass == nullptr)
	{
		return MakeFactoryFailure(
			TEXT("FactoryCreate.ClassMismatch"),
			TEXT("The asset class is not admitted by the exact factory."));
	}
	if ((Arguments.Factory == BehaviorTreeFactoryId)
		!= IsAssetDocumentEditorBehaviorTreeFactoryClassAdmitted(AssetClass))
	{
		return MakeFactoryFailure(
			TEXT("FactoryCreate.ClassMismatch"),
			TEXT("The exact factory does not admit the asset class."));
	}
	return CreateWithAdmittedClass(
		Invocation, Arguments, *AssetClass,
		Handles, *ActiveTransaction);
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorCreateAssetByGenericFactory(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace EditorFactoryCreateHandlerLocal;
	FFactoryCreateArguments Arguments;
	if (!TryReadGenericFactoryArguments(Invocation.ValueArguments, Arguments))
	{
		return MakeFactoryFailure(
			TEXT("FactoryCreate.InvalidArguments"),
			TEXT("Factory creation requires exact package_path and asset_name of one canonical target."));
	}
	if (ActiveTransaction == nullptr)
	{
		return MakeFactoryFailure(
			TEXT("FactoryCreate.TransactionRequired"),
			TEXT("Asset creation requires the active Apply transaction."));
	}
	const FAssetDocumentPrimitiveHandleArgument* ClassArgument = nullptr;
	const FAssetDocumentPrimitiveHandleArgument* ContextArgument = nullptr;
	const FAssetDocumentPrimitiveHandleArgument* Context2Argument = nullptr;
	if (!TryFindGenericFactoryArguments(Invocation, ClassArgument, ContextArgument, Context2Argument))
	{
		return MakeFactoryFailure(
			TEXT("FactoryCreate.InvalidArguments"),
			TEXT("Factory creation takes one asset_class handle and optional factory_context / factory_context_2 handles."));
	}
	FAssetDocumentVerifiedLeaseResult ClassLease = Handles.AcquireVerifiedLease(
		ClassArgument->Handle, MakeFactoryClassLeaseContext(Invocation));
	if (!ClassLease.Lease.IsValid() || !ClassLease.Diagnostics.IsEmpty())
	{
		return MakeFactoryFailure(
			TEXT("FactoryCreate.ClassLeaseFailed"),
			TEXT("The asset class handle could not be leased."),
			ClassLease.Diagnostics);
	}
	UClass* AssetClass = TryGetCanonicalFactoryClass(*ClassLease.Lease);
	const FAssetDocumentEditorGenericFactorySelection Selection =
		SelectAssetDocumentEditorGenericFactory(AssetClass);
	if (!Selection.IsSelected())
	{
		return MakeFactoryFailure(
			Selection.GetDiagnosticCode(),
			*Selection.GetDiagnosticMessage(AssetClass));
	}
	const TStrongObjectPtr<UFactory> Factory =
		NewAssetDocumentEditorGenericFactoryInstance(*Selection.FactoryClass);
	if (!Factory.IsValid())
	{
		return MakeFactoryFailure(
			TEXT("FactoryCreate.FactoryUnavailable"),
			TEXT("The selected editor factory could not be instantiated."));
	}
	{
		FString ClassCode;
		FString ClassMessage;
		if (!ConfigureAssetDocumentEditorGenericFactoryAssetClass(*Factory.Get(), *AssetClass, ClassCode, ClassMessage))
		{
			return MakeFactoryFailure(*ClassCode, *ClassMessage);
		}
	}
	FAssetDocumentVerifiedLeaseResult ContextLeases[2];
	TArray<UObject*, TInlineAllocator<2>> FactoryContexts;
	const FAssetDocumentPrimitiveHandleArgument* ContextArguments[] = {ContextArgument, Context2Argument};
	for (int32 Index = 0; Index < 2; ++Index)
	{
		if (ContextArguments[Index] == nullptr)
		{
			continue;
		}
		UObject* FactoryContext = TryAcquireFactoryContext(
			Invocation, *ContextArguments[Index], Handles, ContextLeases[Index]);
		if (FactoryContext == nullptr)
		{
			return MakeFactoryFailure(
				TEXT("FactoryCreate.ContextLeaseFailed"),
				TEXT("The factory_context handle must lease one existing asset."),
				ContextLeases[Index].Diagnostics);
		}
		FactoryContexts.Add(FactoryContext);
	}
	if (!FactoryContexts.IsEmpty())
	{
		if (const TCHAR* Code = ConfigureAssetDocumentEditorGenericFactoryContexts(*Factory.Get(), FactoryContexts))
		{
			return MakeFactoryFailure(Code, *DescribeAssetDocumentEditorGenericFactoryContextFailure(*Selection.FactoryClass, FactoryContexts));
		}
	}
	FString RequirementCode;
	FString RequirementMessage;
	if (!CheckAssetDocumentEditorFactoryRequirements(*Factory.Get(), *AssetClass, RequirementCode, RequirementMessage))
	{
		return MakeFactoryFailure(*RequirementCode, *RequirementMessage);
	}
	FAssetDocumentPrimitiveOutcome Outcome = CreateWithFactory(
		Invocation, Arguments, *AssetClass, *Factory.Get(),
		Handles, *ActiveTransaction);
	if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded
		&& Outcome.Diagnostics.Num() == 1
		&& Outcome.Diagnostics[0].Code == TEXT("FactoryCreate.IncompleteCreation"))
	{
		FString ShortfallCode;
		Outcome.Diagnostics[0].Message = DescribeAssetDocumentEditorGenericFactoryShortfall(
			*Factory.Get(), *AssetClass, ContextArgument != nullptr, nullptr, ShortfallCode);
		Outcome.Diagnostics[0].Code = ShortfallCode;
	}
	return Outcome;
}
