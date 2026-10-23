// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Class-neutral preview targets for the generic factory lifecycle:
//   editor.scratch.create_by_factory.v1  - new transient asset from the editor factory
//   editor.scratch.clone_asset.v2        - transient duplicate of any non-Blueprint asset
//   editor.scratch.trial_target.v1       - the same transient copy (source_asset) or new transient
//                                          asset (asset_class + factory contexts), issued as Asset
//                                          handles for Validate's trial apply (see
//                                          AssetDocumentGenericValidateTrial.h in Integration); a
//                                          class with a registered trial target maker
//                                          (AssetDocumentEditorTrialScope.h) makes its own
// All live only in the transient package and die with the request context, except the trial target of a
// staged trial class (AssetDocumentEditorTrialScope.h): a package asset in a staged package, discarded with
// the trial scope.

#include "Provider/Lifecycle/AssetDocumentEditorLifecycleHandlers.h"

#include "AssetDocumentEditorDialogFactory.h"
#include "AssetDocumentEditorTrialScope.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "Factories/Factory.h"
#include "Misc/AssertionMacros.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/Lifecycle/AssetDocumentEditorEditorSafety.h"
#include "Provider/Lifecycle/AssetDocumentEditorGenericFactory.h"
#include "Provider/Lifecycle/AssetDocumentEditorLifecyclePayloads.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"

namespace EditorGenericScratchHandlersLocal
{
using namespace UE::AssetDocument::EditorPrimitive;

FAssetDocumentPrimitiveOutcome MakeGenericScratchFailure(
	const TCHAR* Code,
	const FString& Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics = {})
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	if (!Diagnostics.IsEmpty())
	{
		Outcome.Diagnostics.Append(Diagnostics.GetData(), Diagnostics.Num());
		return Outcome;
	}
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Outcome.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/Primitive");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Outcome;
}

bool HasNoValueArguments(const FAssetDocumentRuntimeValueDto& Value)
{
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString DiagnosticCode;
	return TryReadAssetDocumentEditorExactObject(Value, {}, Values, DiagnosticCode);
}

const FAssetDocumentPrimitiveHandleArgument* FindSingleArgument(
	const FAssetDocumentPrimitiveInvocation& Invocation, const TCHAR* Name)
{
	return Invocation.HandleArguments.Num() == 1 && Invocation.HandleArguments[0].Name == Name
		? &Invocation.HandleArguments[0]
		: nullptr;
}

FAssetDocumentVerifiedLeaseResult AcquireReadOnlyLease(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveHandleArgument& Argument,
	EAssetDocumentHandleKind Kind,
	IAssetDocumentHandleResolver& Handles)
{
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Invocation.RequestId;
	Context.TransactionId = Invocation.TransactionId;
	Context.RuntimeRevision = Invocation.RuntimeRevision;
	Context.ProviderId = TEXT("editor");
	Context.Phase = Invocation.Phase;
	Context.RequiredKind = Kind;
	Context.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
	return Handles.AcquireVerifiedLease(Argument.Handle, Context);
}

UClass* TryGetLeasedClass(IAssetDocumentVerifiedHandleLease& Lease)
{
	INativeIdentityPayload* Payload =
		TryGetNativeIdentityPayload(Lease.GetPayloadChecked(), ClassPayloadTypeId);
	FClassIdentityView Identity;
	return Payload != nullptr && Payload->TryGetClassIdentity(Identity) && Identity.Class != nullptr
			&& Identity.CanonicalClassPath == Identity.Class->GetPathName()
		? Identity.Class
		: nullptr;
}

UObject* TryGetLeasedAsset(IAssetDocumentVerifiedHandleLease& Lease)
{
	INativeIdentityPayload* Payload =
		TryGetNativeIdentityPayload(Lease.GetPayloadChecked(), AssetPayloadTypeId);
	if (Payload == nullptr)
	{
		Payload = TryGetNativeIdentityPayload(Lease.GetPayloadChecked(), CreatedAssetPayloadTypeId);
	}
	FObjectIdentityView Identity;
	return Payload != nullptr && Payload->TryGetObjectIdentity(Identity) && Identity.Object != nullptr
			&& Identity.CanonicalObjectPath == Identity.Object->GetPathName()
		? Identity.Object
		: nullptr;
}

TArray<FString> SnapshotTransientRegistryEntries()
{
	TArray<FAssetData> Assets;
	IAssetRegistry::GetChecked().GetAssetsByPackageName(GetTransientPackage()->GetFName(), Assets);
	TArray<FString> Result;
	for (const FAssetData& Asset : Assets)
	{
		Result.Add(Asset.GetObjectPathString());
	}
	Result.Sort();
	return Result;
}

bool IsTransientScratchObject(const UObject* Object)
{
	return Object != nullptr
		&& Object->GetOutermost() == GetTransientPackage()
		&& Object->HasAnyFlags(RF_Transient)
		&& !Object->HasAnyFlags(RF_Public | RF_Standalone);
}

// Factories written for the Content Browser often add the asset flags themselves, whatever flags they are given
// (UChaosSolverFactory, UGeometryCollectionFactory, UChooserTableFactory, UProxyTableFactory, UMovieGraphConfigFactory:
// NewObject(..., Flags | RF_Transactional | RF_Public | RF_Standalone)). A scratch they made in the transient package is
// still this request's preview object, not an asset: the asset flags are dropped from it (and RF_Standalone from its
// inner objects, which would otherwise keep it alive past the request) and it is marked transient, as AssetTools'
// flags would have left it. An object the factory did not create in the transient package keeps its identity and is
// refused below.
void DropFactoryAssetFlags(UObject& Scratch)
{
	if (Scratch.GetOutermost() != GetTransientPackage() || Scratch.GetOuter() != GetTransientPackage())
	{
		return;
	}
	Scratch.ClearFlags(RF_Public | RF_Standalone);
	Scratch.SetFlags(RF_Transient);
	ForEachObjectWithOuter(&Scratch, [](UObject* Inner) { Inner->ClearFlags(RF_Standalone); });
}

// The trial target of a staged trial class: a package asset (IsAsset()) in its own staged package.
bool IsStagedTrialObject(const UObject* Object)
{
	return Object != nullptr && Object->GetOuter() != nullptr && Object->GetOuter() == Object->GetOutermost()
		&& FAssetDocumentEditorTrialScope::IsStagedTrialPackage(*Object->GetOutermost()) && Object->IsAsset();
}

FAssetDocumentRuntimeValueDto MakeString(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

// Validate's trial apply edits the transient copy through the ordinary Asset-handle primitives:
// besides the scratch handle (which keeps the copy alive until the request context ends) it gets
// read and mutable Asset handles, as editor.asset.resolve_existing.v1 issues for a live target.
bool IssueTrialTargetHandles(UObject& Scratch, IAssetDocumentHandleResolver& Handles, FAssetDocumentPrimitiveOutcome& Outcome)
{
	FAssetDocumentIssuedHandleResult Read = Handles.Issue(EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly, MakeAssetDocumentEditorResolvedAssetPayload(Scratch));
	FAssetDocumentIssuedHandleResult Mutable = Handles.Issue(EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable, MakeAssetDocumentEditorResolvedAssetPayload(Scratch));
	if (!Read.Handle.IsSet() || !Read.Diagnostics.IsEmpty() || !Mutable.Handle.IsSet() || !Mutable.Diagnostics.IsEmpty())
	{
		return false;
	}
	FAssetDocumentPrimitiveHandleResult& ReadResult = Outcome.HandleResults.AddDefaulted_GetRef();
	ReadResult.Name = TEXT("asset_read");
	ReadResult.Handles.Add(MoveTemp(Read.Handle.GetValue()));
	FAssetDocumentPrimitiveHandleResult& MutableResult = Outcome.HandleResults.AddDefaulted_GetRef();
	MutableResult.Name = TEXT("asset_mutable");
	MutableResult.Handles.Add(MoveTemp(Mutable.Handle.GetValue()));
	return true;
}

FAssetDocumentPrimitiveOutcome IssueScratch(
	UObject& Scratch,
	TConstArrayView<FAssetDocumentEditorValueField> Evidence,
	IAssetDocumentHandleResolver& Handles,
	bool bTrialTarget)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	FString DiagnosticCode;
	if (!TryBuildAssetDocumentEditorValueObject(Evidence, Outcome.Value, DiagnosticCode))
	{
		return MakeGenericScratchFailure(TEXT("ScratchCreate.InvalidOutcome"),
			TEXT("Scratch identity evidence could not be projected."));
	}
	FAssetDocumentIssuedHandleResult Issued = Handles.Issue(
		EAssetDocumentHandleKind::Scratch,
		EAssetDocumentHandleAccess::Mutable,
		MakeUnique<FAssetDocumentEditorScratchPayload>(Scratch));
	if (!Issued.Handle.IsSet() || !Issued.Diagnostics.IsEmpty())
	{
		return MakeGenericScratchFailure(TEXT("ScratchCreate.HandleIssueFailed"),
			TEXT("The request context rejected the scratch handle."), Issued.Diagnostics);
	}
	FAssetDocumentPrimitiveHandleResult& Result = Outcome.HandleResults.AddDefaulted_GetRef();
	Result.Name = TEXT("scratch");
	Result.Handles.Add(MoveTemp(Issued.Handle.GetValue()));
	if (bTrialTarget && !IssueTrialTargetHandles(Scratch, Handles, Outcome))
	{
		return MakeGenericScratchFailure(TEXT("ScratchCreate.HandleIssueFailed"),
			TEXT("The request context rejected the trial target's Asset handles."));
	}
	return Outcome;
}

FAssetDocumentPrimitiveOutcome CreateFactoryScratch(UClass& AssetClass, TConstArrayView<UObject*> FactoryContexts,
	IAssetDocumentHandleResolver& Handles, bool bTrialTarget)
{
	const FAssetDocumentEditorGenericFactorySelection Selection =
		SelectAssetDocumentEditorGenericFactory(&AssetClass);
	if (!Selection.IsSelected())
	{
		return MakeGenericScratchFailure(Selection.GetDiagnosticCode(),
			Selection.GetDiagnosticMessage(&AssetClass));
	}
	const TStrongObjectPtr<UFactory> Factory =
		NewAssetDocumentEditorGenericFactoryInstance(*Selection.FactoryClass);
	if (!Factory.IsValid())
	{
		return MakeGenericScratchFailure(TEXT("FactoryCreate.FactoryUnavailable"),
			TEXT("The selected editor factory could not be instantiated."));
	}
	{
		FString ClassCode;
		FString ClassMessage;
		if (!ConfigureAssetDocumentEditorGenericFactoryAssetClass(*Factory.Get(), AssetClass, ClassCode, ClassMessage))
		{
			return MakeGenericScratchFailure(*ClassCode, ClassMessage);
		}
	}
	if (!FactoryContexts.IsEmpty())
	{
		if (const TCHAR* Code = ConfigureAssetDocumentEditorGenericFactoryContexts(*Factory.Get(), FactoryContexts))
		{
			return MakeGenericScratchFailure(Code, DescribeAssetDocumentEditorGenericFactoryContextFailure(*Selection.FactoryClass, FactoryContexts));
		}
	}
	FString RequirementCode;
	FString RequirementMessage;
	if (!CheckAssetDocumentEditorFactoryRequirements(*Factory.Get(), AssetClass, RequirementCode, RequirementMessage))
	{
		return MakeGenericScratchFailure(*RequirementCode, RequirementMessage);
	}
	const bool bTransientDirtyBefore = GetTransientPackage()->IsDirty();
	const TArray<FString> RegistryBefore = SnapshotTransientRegistryEntries();
	// A staged trial class's trial target is made as the Content Browser's New asset makes it (AssetTools: Public,
	// Standalone, Transactional), in a staged package.
	UPackage* const StagedOuter = bTrialTarget && FAssetDocumentEditorTrialScope::IsStagedTrialClass(AssetClass)
		? FAssetDocumentEditorTrialScope::CreateStagedTrialPackage(AssetClass) : nullptr;
	if (bTrialTarget && FAssetDocumentEditorTrialScope::IsStagedTrialClass(AssetClass) && StagedOuter == nullptr)
	{
		return MakeGenericScratchFailure(TEXT("Scratch.TrialTargetFailed"),
			TEXT("A staged trial target can only be made inside Validate's trial."));
	}
	UObject* const TransientOuter = StagedOuter != nullptr ? static_cast<UObject*>(StagedOuter) : GetTransientPackage();
	// A transient scratch is still given RF_Public, as the Content Browser's New asset gives every factory: several
	// factories ensure it (UDataTableFactory, UCurveTableFactory, ...). DropFactoryAssetFlags clears it afterwards.
	const EObjectFlags ScratchFlags = StagedOuter != nullptr ? RF_Public | RF_Standalone | RF_Transactional : RF_Transient | RF_Public;
	const FName ScratchName = StagedOuter != nullptr ? FName(*(TEXT("Trial_") + AssetClass.GetName()))
		: MakeUniqueObjectName(TransientOuter, &AssetClass, *(TEXT("Scratch_") + AssetClass.GetName()));
	TStrongObjectPtr<UObject> Scratch;
	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		// A factory that always opens its settings dialog (AssetDocumentEditorDialogFactory.h): the factory's
		// own NewObject, then what it does after the dialog's OK.
		if (const FAssetDocumentEditorDialogFactoryInit* DialogInit = FindAssetDocumentEditorDialogFactory(*Selection.FactoryClass))
		{
			Scratch.Reset(NewObject<UObject>(TransientOuter, &AssetClass, ScratchName, ScratchFlags));
			FString InitMessage;
			if (!(*DialogInit)(*Factory.Get(), *Scratch.Get(), InitMessage))
			{
				Scratch->MarkAsGarbage();
				Scratch.Reset();
				NonInteractive.DiscardCreatedPackages();
				return MakeGenericScratchFailure(TEXT("FactoryCreate.DialogFactoryInitFailed"),
					FString::Printf(TEXT("%s: %s"), *Selection.FactoryClass->GetPathName(), *InitMessage));
			}
		}
		else
		{
			Scratch.Reset(Factory->FactoryCreateNew(
				&AssetClass, TransientOuter, ScratchName, ScratchFlags, nullptr, GWarn));
		}
		if (NonInteractive.WasInteractionRefused())
		{
			if (Scratch.IsValid())
			{
				Scratch->MarkAsGarbage();
				Scratch.Reset();
			}
			NonInteractive.DiscardCreatedPackages();
			return MakeGenericScratchFailure(AssetDocumentEditorInteractiveFactoryCode,
				FAssetDocumentEditorNonInteractiveScope::DescribeRefusal(Selection.FactoryClass, &AssetClass));
		}
	}
	if (!Scratch.IsValid() || Scratch->GetClass() != &AssetClass)
	{
		FString ShortfallCode;
		const FString Shortfall = DescribeAssetDocumentEditorGenericFactoryShortfall(
			*Factory.Get(), AssetClass, !FactoryContexts.IsEmpty(), Scratch.Get(), ShortfallCode);
		if (Scratch.IsValid())
		{
			Scratch->MarkAsGarbage();
			Scratch.Reset();
		}
		return MakeGenericScratchFailure(*ShortfallCode, Shortfall);
	}
	if (GetTransientPackage()->IsDirty() != bTransientDirtyBefore
		|| SnapshotTransientRegistryEntries() != RegistryBefore)
	{
		return MakeGenericScratchFailure(TEXT("ScratchCreate.ResidueDetected"),
			TEXT("The editor factory left persistent residue while creating a scratch."));
	}
	if (StagedOuter != nullptr)
	{
		if (!IsStagedTrialObject(Scratch.Get()) || Scratch->GetOutermost() != StagedOuter)
		{
			return MakeGenericScratchFailure(TEXT("ScratchCreate.InvalidIdentity"), FString::Printf(
				TEXT("The editor factory did not make the staged trial target as an asset of its staged package (%s)."),
				*Scratch->GetPathName()));
		}
	}
	else
	{
		DropFactoryAssetFlags(*Scratch.Get());
		if (!IsTransientScratchObject(Scratch.Get()))
		{
			return MakeGenericScratchFailure(TEXT("ScratchCreate.InvalidIdentity"),
				TEXT("The editor factory returned a non-transient scratch."));
		}
	}
	const FAssetDocumentEditorValueField Evidence[] = {
		{TEXT("lifecycle_id"), MakeString(AssetDocumentEditorGenericFactoryLifecycleId)},
		{TEXT("factory_class"), MakeString(Selection.FactoryClass->GetPathName())},
		{TEXT("canonical_class"), MakeString(AssetClass.GetPathName())},
		{TEXT("scratch_path"), MakeString(Scratch->GetPathName())},
	};
	if (bTrialTarget)
	{
		FAssetDocumentEditorTrialScope::RegisterTrialTarget(*Scratch.Get(), nullptr);
	}
	return IssueScratch(*Scratch.Get(), Evidence, Handles, bTrialTarget);
}

bool IsClonableSource(const UObject& Source)
{
	const UClass* Class = Source.GetClass();
	return Source.GetOuter() != nullptr && Source.GetOuter()->IsA<UPackage>()
		&& !Class->IsChildOf(UBlueprint::StaticClass())
		&& !Class->IsChildOf(UWorld::StaticClass())
		&& !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
}

// The trial target of a staged trial class's existing asset: the editor's Duplicate (ObjectTools::DuplicateSingleObject:
// StaticDuplicateObject in Normal mode, the asset's flags kept) into a staged package; the class's PostDuplicate
// then runs as it does for the Content Browser's Duplicate (a MetaSound gets a class name of its own).
FAssetDocumentPrimitiveOutcome CloneStagedTrialTarget(UObject& Source, IAssetDocumentHandleResolver& Handles)
{
	UPackage* const Staged = FAssetDocumentEditorTrialScope::CreateStagedTrialPackage(*Source.GetClass());
	if (Staged == nullptr)
	{
		return MakeGenericScratchFailure(TEXT("Scratch.TrialTargetFailed"),
			TEXT("A staged trial target can only be made inside Validate's trial."));
	}
	TMap<UObject*, UObject*> CreatedObjects;
	FObjectDuplicationParameters Parameters(&Source, Staged);
	Parameters.DestName = Source.GetFName();
	Parameters.bAssignExternalPackages = false;
	Parameters.CreatedObjects = &CreatedObjects;
	TStrongObjectPtr<UObject> Target;
	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		Target.Reset(StaticDuplicateObjectEx(Parameters));
		if (NonInteractive.WasInteractionRefused())
		{
			NonInteractive.DiscardCreatedPackages();
			return MakeGenericScratchFailure(AssetDocumentEditorInteractiveFactoryCode, FString::Printf(
				TEXT("Duplicating %s for the trial asked for a dialog."), *Source.GetPathName()));
		}
	}
	FString Offender;
	for (const TPair<UObject*, UObject*>& Pair : CreatedObjects)
	{
		if (Pair.Value != nullptr && Pair.Value->GetOutermost() != Staged && Offender.IsEmpty())
		{
			Offender = Pair.Value->GetPathName();
		}
	}
	if (!Target.IsValid() || CreatedObjects.FindRef(&Source) != Target.Get() || !IsStagedTrialObject(Target.Get()) || !Offender.IsEmpty())
	{
		return MakeGenericScratchFailure(TEXT("Scratch.CloneFailed"), FString::Printf(
			TEXT("The staged trial duplicate of %s did not stay an asset of its staged package (%s)."),
			*Source.GetPathName(), Offender.IsEmpty() ? *GetPathNameSafe(Target.Get()) : *Offender));
	}
	const FAssetDocumentEditorValueField Evidence[] = {
		{TEXT("lifecycle_id"), MakeString(AssetDocumentEditorGenericFactoryLifecycleId)},
		{TEXT("source_path"), MakeString(Source.GetPathName())},
		{TEXT("scratch_path"), MakeString(Target->GetPathName())},
	};
	FAssetDocumentEditorTrialScope::RegisterTrialTarget(*Target.Get(), &Source);
	return IssueScratch(*Target.Get(), Evidence, Handles, true);
}

FAssetDocumentPrimitiveOutcome CloneScratch(UObject& Source, IAssetDocumentHandleResolver& Handles, bool bTrialTarget)
{
	if (!IsClonableSource(Source))
	{
		return MakeGenericScratchFailure(TEXT("Scratch.UnsupportedSource"),
			FString::Printf(TEXT("%s cannot be previewed through a generic clone (Blueprint, World or non-asset object)."),
				*Source.GetClass()->GetPathName()));
	}
	if (bTrialTarget && FAssetDocumentEditorTrialScope::IsStagedTrialClass(*Source.GetClass()))
	{
		return CloneStagedTrialTarget(Source, Handles);
	}
	FString CloneMessage;
	if (!CheckAssetDocumentEditorScratchCloneSupported(Source, CloneMessage))
	{
		return MakeGenericScratchFailure(TEXT("Scratch.UnsupportedSource"), CloneMessage);
	}
	UObject* const TransientOuter = GetTransientPackage();
	TMap<UObject*, UObject*> CreatedObjects;
	FObjectDuplicationParameters Parameters(&Source, TransientOuter);
	Parameters.DestName = MakeUniqueObjectName(TransientOuter, Source.GetClass(), Source.GetFName());
	Parameters.FlagMask = static_cast<EObjectFlags>(RF_AllFlags & ~(RF_Public | RF_Standalone | RF_Transactional));
	Parameters.ApplyFlags = RF_Transient;
	Parameters.bAssignExternalPackages = false;
	Parameters.CreatedObjects = &CreatedObjects;
	// Same duplication the existing data-asset clone (editor.scratch.clone_asset.v1) performs.
	const TStrongObjectPtr<UObject> Scratch(StaticDuplicateObjectEx(Parameters));
	// Duplication routes PostLoad, and some asset classes make themselves Standalone there
	// (IKRigDefinition and IKRetargeter set RF_Standalone unconditionally, for very old saves).
	// A transient scratch is never saved or kept: drop the flags again, as the FlagMask intends,
	// so garbage collection can reclaim it with the request.
	for (const TPair<UObject*, UObject*>& Pair : CreatedObjects)
	{
		if (Pair.Value != nullptr && Pair.Value->GetOutermost() == TransientOuter)
		{
			Pair.Value->ClearFlags(RF_Public | RF_Standalone);
		}
	}
	bool bAdmitted = Scratch.IsValid() && CreatedObjects.FindRef(&Source) == Scratch.Get();
	FString Offender;
	for (const TPair<UObject*, UObject*>& Pair : CreatedObjects)
	{
		if (!IsTransientScratchObject(Pair.Value) && Offender.IsEmpty())
		{
			Offender = GetPathNameSafe(Pair.Value);
		}
		bAdmitted &= IsTransientScratchObject(Pair.Value);
	}
	if (!bAdmitted)
	{
		if (Scratch.IsValid())
		{
			ReleaseAssetDocumentEditorScratchNativeBindings(*Scratch.Get());
		}
		return MakeGenericScratchFailure(TEXT("Scratch.CloneFailed"), FString::Printf(
			TEXT("The transient scratch duplication of %s did not stay inside the transient package as a transient object (%s)."),
			*Source.GetClass()->GetPathName(), *Offender));
	}
	const FAssetDocumentEditorValueField Evidence[] = {
		{TEXT("lifecycle_id"), MakeString(AssetDocumentEditorGenericFactoryLifecycleId)},
		{TEXT("source_path"), MakeString(Source.GetPathName())},
		{TEXT("scratch_path"), MakeString(Scratch->GetPathName())},
	};
	if (bTrialTarget)
	{
		FAssetDocumentEditorTrialScope::RegisterTrialTarget(*Scratch.Get(), &Source);
	}
	return IssueScratch(*Scratch.Get(), Evidence, Handles, bTrialTarget);
}

// A trial target made by the asset class's registered maker (FAssetDocumentEditorTrialScope::
// RegisterTrialTargetMaker): the same transient-identity and residue checks as the generic scratch.
FAssetDocumentPrimitiveOutcome MakeRegisteredTrialTarget(const FAssetDocumentEditorTrialScope::FTrialTargetMaker& Maker,
	UClass& AssetClass, UObject* Source, TConstArrayView<UObject*> FactoryContexts, IAssetDocumentHandleResolver& Handles)
{
	const bool bTransientDirtyBefore = GetTransientPackage()->IsDirty();
	const TArray<FString> RegistryBefore = SnapshotTransientRegistryEntries();
	FString Message;
	TStrongObjectPtr<UObject> Target;
	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		Target.Reset(Maker(AssetClass, Source, FactoryContexts, Message));
		if (NonInteractive.WasInteractionRefused())
		{
			if (Target.IsValid())
			{
				Target->MarkAsGarbage();
				Target.Reset();
			}
			NonInteractive.DiscardCreatedPackages();
			return MakeGenericScratchFailure(AssetDocumentEditorInteractiveFactoryCode, FString::Printf(
				TEXT("Making the trial target of %s asked for a dialog."), *AssetClass.GetPathName()));
		}
	}
	if (!Target.IsValid())
	{
		return MakeGenericScratchFailure(TEXT("Scratch.TrialTargetFailed"), FString::Printf(
			TEXT("The trial target of %s could not be made: %s"), *AssetClass.GetPathName(), *Message));
	}
	if (SnapshotTransientRegistryEntries() != RegistryBefore)
	{
		return MakeGenericScratchFailure(TEXT("ScratchCreate.ResidueDetected"),
			TEXT("Making the trial target registered a transient asset."));
	}
	if (GetTransientPackage()->IsDirty() != bTransientDirtyBefore)
	{
		GetTransientPackage()->SetDirtyFlag(bTransientDirtyBefore);
	}
	if (!IsTransientScratchObject(Target.Get()) || Target->GetClass() != &AssetClass)
	{
		return MakeGenericScratchFailure(TEXT("ScratchCreate.InvalidIdentity"), FString::Printf(
			TEXT("The trial target maker of %s returned %s, not a transient object of that class."),
			*AssetClass.GetPathName(), *GetPathNameSafe(Target.Get())));
	}
	TArray<FAssetDocumentEditorValueField> Evidence = {
		{TEXT("lifecycle_id"), MakeString(AssetDocumentEditorGenericFactoryLifecycleId)},
		{TEXT("canonical_class"), MakeString(AssetClass.GetPathName())},
		{TEXT("scratch_path"), MakeString(Target->GetPathName())},
	};
	if (Source != nullptr)
	{
		Evidence.Add({TEXT("source_path"), MakeString(Source->GetPathName())});
	}
	FAssetDocumentEditorTrialScope::RegisterTrialTarget(*Target.Get(), Source);
	return IssueScratch(*Target.Get(), Evidence, Handles, true);
}

// asset_class plus optional factory_context / factory_context_2 handles (a new asset), or one
// source_asset handle (a copy of an existing one). Null entries are absent arguments.
struct FScratchArguments
{
	const FAssetDocumentPrimitiveHandleArgument* Class = nullptr;
	const FAssetDocumentPrimitiveHandleArgument* Context = nullptr;
	const FAssetDocumentPrimitiveHandleArgument* Context2 = nullptr;
	const FAssetDocumentPrimitiveHandleArgument* Source = nullptr;
	bool bValid = true;
};

FScratchArguments ReadScratchArguments(const FAssetDocumentPrimitiveInvocation& Invocation)
{
	FScratchArguments Arguments;
	for (const FAssetDocumentPrimitiveHandleArgument& Argument : Invocation.HandleArguments)
	{
		const FAssetDocumentPrimitiveHandleArgument** Slot =
			Argument.Name == TEXT("asset_class") ? &Arguments.Class
			: Argument.Name == TEXT("factory_context") ? &Arguments.Context
			: Argument.Name == TEXT("factory_context_2") ? &Arguments.Context2
			: Argument.Name == TEXT("source_asset") ? &Arguments.Source : nullptr;
		if (Slot == nullptr || *Slot != nullptr)
		{
			Arguments.bValid = false;
			return Arguments;
		}
		*Slot = &Argument;
	}
	Arguments.bValid = (Arguments.Context2 == nullptr || Arguments.Context != nullptr)
		&& ((Arguments.Source != nullptr) != (Arguments.Class != nullptr))
		&& (Arguments.Source == nullptr || Arguments.Context == nullptr);
	return Arguments;
}

FAssetDocumentPrimitiveOutcome CreateScratchFromArguments(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FScratchArguments& Arguments,
	IAssetDocumentHandleResolver& Handles,
	bool bTrialTarget)
{
	FAssetDocumentVerifiedLeaseResult Lease =
		AcquireReadOnlyLease(Invocation, *Arguments.Class, EAssetDocumentHandleKind::Class, Handles);
	if (!Lease.Lease.IsValid() || !Lease.Diagnostics.IsEmpty())
	{
		return MakeGenericScratchFailure(TEXT("ScratchCreate.ClassLeaseFailed"),
			TEXT("The Class handle could not be leased."), Lease.Diagnostics);
	}
	UClass* AssetClass = TryGetLeasedClass(*Lease.Lease);
	if (AssetClass == nullptr)
	{
		return MakeGenericScratchFailure(TEXT("ScratchCreate.InvalidIdentity"),
			TEXT("The Class handle identity is not canonical."));
	}
	FAssetDocumentVerifiedLeaseResult ContextLeases[2];
	TArray<UObject*, TInlineAllocator<2>> FactoryContexts;
	const FAssetDocumentPrimitiveHandleArgument* ContextArguments[] = {Arguments.Context, Arguments.Context2};
	for (int32 Index = 0; Index < 2; ++Index)
	{
		if (ContextArguments[Index] == nullptr)
		{
			continue;
		}
		FAssetDocumentVerifiedLeaseResult& ContextLease = ContextLeases[Index];
		ContextLease = AcquireReadOnlyLease(Invocation, *ContextArguments[Index], EAssetDocumentHandleKind::Asset, Handles);
		UObject* FactoryContext = ContextLease.Lease.IsValid() && ContextLease.Diagnostics.IsEmpty()
			? TryGetLeasedAsset(*ContextLease.Lease) : nullptr;
		if (FactoryContext == nullptr)
		{
			return MakeGenericScratchFailure(TEXT("FactoryCreate.ContextLeaseFailed"),
				TEXT("The factory_context handle must lease one existing asset."), ContextLease.Diagnostics);
		}
		FactoryContexts.Add(FactoryContext);
	}
	if (bTrialTarget)
	{
		if (const FAssetDocumentEditorTrialScope::FTrialTargetMaker* Maker = FAssetDocumentEditorTrialScope::FindTrialTargetMaker(*AssetClass))
		{
			return MakeRegisteredTrialTarget(*Maker, *AssetClass, nullptr, FactoryContexts, Handles);
		}
	}
	return CreateFactoryScratch(*AssetClass, FactoryContexts, Handles, bTrialTarget);
}

FAssetDocumentPrimitiveOutcome CloneScratchFromArgument(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentPrimitiveHandleArgument& SourceArgument,
	IAssetDocumentHandleResolver& Handles,
	bool bTrialTarget)
{
	FAssetDocumentVerifiedLeaseResult Lease =
		AcquireReadOnlyLease(Invocation, SourceArgument, EAssetDocumentHandleKind::Asset, Handles);
	if (!Lease.Lease.IsValid() || !Lease.Diagnostics.IsEmpty())
	{
		return MakeGenericScratchFailure(TEXT("Scratch.SourceLeaseFailed"),
			TEXT("The source asset handle could not be leased."), Lease.Diagnostics);
	}
	UObject* Source = TryGetLeasedAsset(*Lease.Lease);
	if (Source == nullptr)
	{
		return MakeGenericScratchFailure(TEXT("Scratch.InvalidIdentity"),
			TEXT("The source asset identity is not canonical."));
	}
	if (bTrialTarget)
	{
		if (const FAssetDocumentEditorTrialScope::FTrialTargetMaker* Maker = FAssetDocumentEditorTrialScope::FindTrialTargetMaker(*Source->GetClass()))
		{
			return MakeRegisteredTrialTarget(*Maker, *Source->GetClass(), Source, {}, Handles);
		}
	}
	return CloneScratch(*Source, Handles, bTrialTarget);
}
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorScratchCreateByFactory(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace EditorGenericScratchHandlersLocal;
	// asset_class plus the optional factory_context asset (e.g. a Montage's Skeleton) and, for a factory
	// whose dialog sets two objects, factory_context_2 (only together with factory_context).
	const FScratchArguments Arguments = ReadScratchArguments(Invocation);
	if (ActiveTransaction != nullptr || !Arguments.bValid || Arguments.Class == nullptr
		|| !HasNoValueArguments(Invocation.ValueArguments))
	{
		return MakeGenericScratchFailure(TEXT("ScratchCreate.InvalidArguments"),
			TEXT("Scratch creation takes one asset_class handle, optional factory_context / factory_context_2 handles, no values and no active transaction."));
	}
	return CreateScratchFromArguments(Invocation, Arguments, Handles, false);
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorScratchCloneAssetV2(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace EditorGenericScratchHandlersLocal;
	const FAssetDocumentPrimitiveHandleArgument* SourceArgument =
		FindSingleArgument(Invocation, TEXT("source_asset"));
	if (ActiveTransaction != nullptr || SourceArgument == nullptr
		|| !HasNoValueArguments(Invocation.ValueArguments))
	{
		return MakeGenericScratchFailure(TEXT("Scratch.InvalidArguments"),
			TEXT("Scratch cloning takes exactly one source_asset handle, no values and no active transaction."));
	}
	return CloneScratchFromArgument(Invocation, *SourceArgument, Handles, false);
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorScratchTrialTarget(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace EditorGenericScratchHandlersLocal;
	// Apply phase of Validate's trial: the trial transaction only scopes the request; nothing of
	// the scratch is enlisted (it is transient and discarded with the request context).
	const FScratchArguments Arguments = ReadScratchArguments(Invocation);
	if (ActiveTransaction == nullptr || !Arguments.bValid || !HasNoValueArguments(Invocation.ValueArguments))
	{
		return MakeGenericScratchFailure(TEXT("Scratch.InvalidArguments"),
			TEXT("A trial target takes one source_asset handle (copy) or one asset_class handle with optional factory_context / factory_context_2 handles (new asset), no values, inside a transaction."));
	}
	if (Arguments.Source == nullptr)
	{
		return CreateScratchFromArguments(Invocation, Arguments, Handles, true);
	}
	// UE 5.7: duplicating a StateTree whose global nodes (or root parameters) have property function
	// bindings fails an ensure in UStateTreeEditorData::DuplicateIDs ("An ID container was not
	// duplicated"; the editor's Duplicate hits it too). Only the IDs of those function nodes keep their
	// source values, which does not matter for a discarded trial copy, so the trial does not report
	// that one engine ensure; any other ensure is reported as usual.
	TFunction<bool(const FEnsureHandlerArgs&)> Previous = GetEnsureHandler();
	SetEnsureHandler([Previous](const FEnsureHandlerArgs& Args)
		{
			if (Args.Expression != nullptr && FCStringAnsi::Strcmp(Args.Expression, "OldToNewIDs.Contains(StructValue.Key)") == 0)
			{
				return true;
			}
			return Previous ? Previous(Args) : false;
		});
	FAssetDocumentPrimitiveOutcome Outcome = CloneScratchFromArgument(Invocation, *Arguments.Source, Handles, true);
	SetEnsureHandler(MoveTemp(Previous));
	return Outcome;
}
