// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Lifecycle/AssetDocumentEditorLifecycleHandlers.h"

#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/Lifecycle/AssetDocumentEditorEditorSafety.h"
#include "Provider/Lifecycle/AssetDocumentEditorLifecycleAdmission.h"
#include "Provider/Lifecycle/AssetDocumentEditorLifecyclePayloads.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/UObjectGlobals.h"

namespace EditorScratchCloneHandlerLocal
{
using namespace UE::AssetDocument::EditorPrimitive;

FAssetDocumentPrimitiveOutcome MakeScratchFailure(
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

FAssetDocumentPrimitiveOutcome MakeScratchFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	return MakeScratchFailure(
		Code, Message, TConstArrayView<FAssetDocumentRuntimeDiagnostic>());
}

const FAssetDocumentPrimitiveHandleArgument* FindSourceArgument(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	if (Invocation.HandleArguments.Num() == 1
		&& Invocation.HandleArguments[0].Name == TEXT("source_asset"))
	{
		return &Invocation.HandleArguments[0];
	}
	return nullptr;
}

FAssetDocumentHandleValidationContext MakeSourceValidationContext(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Invocation.RequestId;
	Context.TransactionId = Invocation.TransactionId;
	Context.RuntimeRevision = Invocation.RuntimeRevision;
	Context.ProviderId = TEXT("editor");
	Context.Phase = Invocation.Phase;
	Context.RequiredKind = EAssetDocumentHandleKind::Asset;
	Context.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
	return Context;
}

bool TryReadCategory(
	const FAssetDocumentRuntimeValueDto& Arguments,
	FString& OutCategory)
{
	const FAssetDocumentEditorValueFieldSpec Fields[] = {
		{TEXT("category_id"), EAssetDocumentRuntimeValueKind::String},
	};
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString DiagnosticCode;
	if (!TryReadAssetDocumentEditorExactObject(
		Arguments, Fields, Values, DiagnosticCode))
	{
		return false;
	}
	OutCategory = Values[0]->StringValue;
	return OutCategory == TEXT("data_asset_exact.v1");
}

UObject* TryGetCanonicalObject(
	INativeIdentityPayload* Payload)
{
	if (Payload == nullptr)
	{
		return nullptr;
	}
	FObjectIdentityView Identity;
	if (!Payload->TryGetObjectIdentity(Identity))
	{
		return nullptr;
	}
	if (Identity.Object == nullptr)
	{
		return nullptr;
	}
	if (Identity.CanonicalObjectPath != Identity.Object->GetPathName())
	{
		return nullptr;
	}
	return Identity.Object;
}

UObject* TryGetAdmittedSource(
	IAssetDocumentVerifiedHandleLease& Lease)
{
	UObject* Source = TryGetCanonicalObject(TryGetNativeIdentityPayload(
		Lease.GetPayloadChecked(), AssetPayloadTypeId));
	if (Source != nullptr
		&& IsAssetDocumentEditorScratchClassAdmitted(Source->GetClass()))
	{
		return Source;
	}
	return nullptr;
}

bool IsCreatedScratchObjectAdmitted(UObject* Object)
{
	return Object != nullptr
		&& Object->GetOutermost() == GetTransientPackage()
		&& Object->HasAnyFlags(RF_Transient)
		&& !Object->HasAnyFlags(RF_Public | RF_Standalone);
}

bool AreCreatedScratchObjectsAdmitted(
	const TMap<UObject*, UObject*>& CreatedObjects,
	UObject* Source,
	UObject* Scratch)
{
	if (CreatedObjects.FindRef(Source) != Scratch)
	{
		return false;
	}
	for (const TPair<UObject*, UObject*>& Pair : CreatedObjects)
	{
		if (!IsCreatedScratchObjectAdmitted(Pair.Value))
		{
			return false;
		}
	}
	return true;
}

UObject* DuplicateAdmittedScratch(
	UObject& Source,
	TMap<UObject*, UObject*>& OutCreatedObjects)
{
	UObject* TransientOuter = GetTransientPackage();
	FObjectDuplicationParameters Parameters(&Source, TransientOuter);
	Parameters.DestName = MakeUniqueObjectName(
		TransientOuter, Source.GetClass(), Source.GetFName());
	Parameters.FlagMask = static_cast<EObjectFlags>(
		RF_AllFlags & ~(RF_Public | RF_Standalone | RF_Transactional));
	Parameters.ApplyFlags = RF_Transient;
	Parameters.bAssignExternalPackages = false;
	Parameters.CreatedObjects = &OutCreatedObjects;
	// ASSETDOCUMENT_FRAMEWORK_RAW_LIFECYCLE_ALLOWLIST(framework.scratch.clone_asset.v1)
	// ASSETDOCUMENT_FRAMEWORK_RAW_CALLSITE(framework.scratch.clone_asset.v1)
	UObject* Scratch = StaticDuplicateObjectEx(Parameters);
	if (AreCreatedScratchObjectsAdmitted(OutCreatedObjects, &Source, Scratch))
	{
		return Scratch;
	}
	if (Scratch != nullptr)
	{
		ReleaseAssetDocumentEditorScratchNativeBindings(*Scratch);
	}
	return nullptr;
}

FAssetDocumentRuntimeValueDto MakeScratchStringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentPrimitiveOutcome MakeScratchSuccess(
	UObject& Source,
	UObject& Scratch,
	FAssetDocumentOpaqueHandle Handle)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	const FAssetDocumentEditorValueField Fields[] = {
		{TEXT("category_id"), MakeScratchStringValue(TEXT("data_asset_exact.v1"))},
		{TEXT("source_path"), MakeScratchStringValue(Source.GetPathName())},
		{TEXT("scratch_path"), MakeScratchStringValue(Scratch.GetPathName())},
	};
	FString DiagnosticCode;
	if (!TryBuildAssetDocumentEditorValueObject(
		Fields, Outcome.Value, DiagnosticCode))
	{
		return MakeScratchFailure(
			TEXT("Scratch.InvalidOutcome"),
			TEXT("Scratch identity evidence could not be projected."));
	}
	FAssetDocumentPrimitiveHandleResult& Result =
		Outcome.HandleResults.AddDefaulted_GetRef();
	Result.Name = TEXT("scratch");
	Result.Handles.Add(MoveTemp(Handle));
	return Outcome;
}

FAssetDocumentPrimitiveOutcome CloneAndIssueScratch(
	UObject& Source,
	IAssetDocumentHandleResolver& Handles)
{
	TMap<UObject*, UObject*> CreatedObjects;
	UObject* Scratch = DuplicateAdmittedScratch(Source, CreatedObjects);
	if (Scratch == nullptr)
	{
		return MakeScratchFailure(
			TEXT("Scratch.CloneFailed"),
			TEXT("The authoritative scratch duplication did not complete."));
	}
	FAssetDocumentIssuedHandleResult Issued = Handles.Issue(
		EAssetDocumentHandleKind::Scratch,
		EAssetDocumentHandleAccess::Mutable,
		MakeUnique<FAssetDocumentEditorScratchPayload>(*Scratch));
	if (!Issued.Handle.IsSet() || !Issued.Diagnostics.IsEmpty())
	{
		return MakeScratchFailure(
			TEXT("Scratch.HandleIssueFailed"),
			TEXT("The request context rejected the scratch handle."),
			Issued.Diagnostics);
	}
	return MakeScratchSuccess(
		Source, *Scratch, MoveTemp(Issued.Handle.GetValue()));
}

FAssetDocumentPrimitiveOutcome CloneScratchFromLease(
	IAssetDocumentVerifiedHandleLease& Lease,
	IAssetDocumentHandleResolver& Handles)
{
	UObject* Source = TryGetAdmittedSource(Lease);
	if (Source == nullptr)
	{
		return MakeScratchFailure(
			TEXT("Scratch.UnsupportedSource"),
			TEXT("The source is not an exact admitted data-asset family."));
	}
	// Same precondition as editor.scratch.clone_asset.v2: classes whose
	// duplication callbacks require a real asset are never duplicated.
	FString CloneMessage;
	if (!CheckAssetDocumentEditorScratchCloneSupported(*Source, CloneMessage))
	{
		return MakeScratchFailure(TEXT("Scratch.UnsupportedSource"), *CloneMessage);
	}
	return CloneAndIssueScratch(*Source, Handles);
}
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorScratchCloneAsset(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace EditorScratchCloneHandlerLocal;
	(void)ActiveTransaction;
	FString Category;
	const FAssetDocumentPrimitiveHandleArgument* SourceArgument =
		FindSourceArgument(Invocation);
	if (!TryReadCategory(Invocation.ValueArguments, Category)
		|| SourceArgument == nullptr)
	{
		return MakeScratchFailure(
			TEXT("Scratch.UnsupportedFamily"),
			TEXT("Scratch cloning requires the exact admitted family."));
	}
	FAssetDocumentVerifiedLeaseResult SourceLease =
		Handles.AcquireVerifiedLease(
			SourceArgument->Handle,
			MakeSourceValidationContext(Invocation));
	if (!SourceLease.Lease || !SourceLease.Diagnostics.IsEmpty())
	{
		return MakeScratchFailure(
			TEXT("Scratch.SourceLeaseFailed"),
			TEXT("The source asset handle could not be leased."),
			SourceLease.Diagnostics);
	}
	return CloneScratchFromLease(*SourceLease.Lease, Handles);
}
