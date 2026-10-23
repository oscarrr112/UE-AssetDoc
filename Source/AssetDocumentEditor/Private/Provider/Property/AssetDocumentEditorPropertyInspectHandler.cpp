// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Property/AssetDocumentEditorPropertyInspectHandler.h"

#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/Property/AssetDocumentEditorPropertyAdmission.h"
#include "Provider/Property/AssetDocumentEditorPropertyInspection.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"

namespace EditorPropertyInspectHandlerLocal
{
FAssetDocumentPrimitiveOutcome MakeFailure(
	const TCHAR* Path,
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Outcome.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = Path;
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Outcome;
}

bool TryReadArguments(
	const FAssetDocumentRuntimeValueDto& Arguments,
	FStringView& OutMode,
	FStringView& OutClassPath)
{
	const TArray<FAssetDocumentEditorValueFieldSpec> Fields = {
		{TEXT("mode"), EAssetDocumentRuntimeValueKind::String},
		{TEXT("exact_asset_class_path"), EAssetDocumentRuntimeValueKind::String},
	};
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString DiagnosticCode;
	if (!TryReadAssetDocumentEditorExactObject(
		Arguments, Fields, Values, DiagnosticCode))
	{
		return false;
	}
	OutMode = Values[0]->StringValue;
	OutClassPath = Values[1]->StringValue;
	return true;
}

bool IsExactClassPath(FStringView ClassPath)
{
	const FString RequestedPath(ClassPath);
	if (RequestedPath.IsEmpty() || RequestedPath.TrimStartAndEnd() != RequestedPath)
	{
		return false;
	}
	const FSoftClassPath SoftPath(RequestedPath);
	if (!SoftPath.IsValid())
	{
		return false;
	}
	return SoftPath.GetSubPathUtf8String().IsEmpty();
}

UClass* ResolveExactClass(FStringView ClassPath)
{
	if (!IsExactClassPath(ClassPath))
	{
		return nullptr;
	}
	const FString RequestedPath(ClassPath);
	const FSoftClassPath SoftPath(RequestedPath);
	UClass* Resolved = SoftPath.TryLoadClass<UObject>();
	if (Resolved == nullptr || Resolved->GetPathName() != RequestedPath)
	{
		return nullptr;
	}
	return Resolved;
}

FAssetDocumentHandleValidationContext MakeAssetContext(
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

UObject* GetLiveAsset(IAssetDocumentVerifiedHandleLease& Lease)
{
	using namespace UE::AssetDocument::EditorPrimitive;
	IAssetDocumentNativeHandlePayload& Payload = Lease.GetPayloadChecked();
	INativeIdentityPayload* Identity = TryGetNativeIdentityPayload(
		Payload, AssetPayloadTypeId);
	if (Identity == nullptr || !Payload.IsAlive())
	{
		return nullptr;
	}
	FObjectIdentityView View;
	if (!Identity->TryGetObjectIdentity(View))
	{
		return nullptr;
	}
	return View.Object;
}

FAssetDocumentPrimitiveOutcome MakeInspectionOutcome(
	FAssetDocumentEditorPropertyInspectionResult&& Inspection)
{
	if (!Inspection.bSucceeded)
	{
		FAssetDocumentPrimitiveOutcome Outcome;
		Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
		Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
		Outcome.Diagnostics = MoveTemp(Inspection.Diagnostics);
		if (Outcome.Diagnostics.IsEmpty())
		{
			return MakeFailure(
				TEXT("/Primitive"),
				TEXT("PropertyInspection.Failed"),
				TEXT("Property inspection did not produce evidence."));
		}
		return Outcome;
	}
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = MoveTemp(Inspection.Value);
	return Outcome;
}

FAssetDocumentPrimitiveOutcome HandleClassDefaultInspection(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	UClass& ExactClass,
	TConstArrayView<FString> DeclaredNames)
{
	if (!Invocation.HandleArguments.IsEmpty())
	{
		return MakeFailure(
			TEXT("/HandleArguments"),
			TEXT("PrimitiveInvocation.InvalidHandleArguments"),
			TEXT("Class-default inspection does not accept handles."));
	}
	return MakeInspectionOutcome(
		InspectAssetDocumentEditorProperties(ExactClass, nullptr, DeclaredNames));
}

bool HasLiveAssetArgument(const FAssetDocumentPrimitiveInvocation& Invocation)
{
	if (Invocation.HandleArguments.Num() != 1)
	{
		return false;
	}
	return Invocation.HandleArguments[0].Name == TEXT("asset");
}

FAssetDocumentPrimitiveOutcome MakeLeaseFailure(
	FAssetDocumentVerifiedLeaseResult&& LeaseResult)
{
	if (LeaseResult.Diagnostics.IsEmpty())
	{
		return MakeFailure(
			TEXT("/Handle"),
			TEXT("HandleMetadata.MissingLease"),
			TEXT("The asset handle could not be leased."));
	}
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	Outcome.Diagnostics = MoveTemp(LeaseResult.Diagnostics);
	return Outcome;
}

bool HasExactLiveAssetClass(UObject* LiveAsset, const UClass& ExactClass)
{
	return LiveAsset != nullptr && LiveAsset->GetClass() == &ExactClass;
}

FAssetDocumentPrimitiveOutcome HandleLiveAssetInspection(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	UClass& ExactClass,
	TConstArrayView<FString> DeclaredNames)
{
	if (!HasLiveAssetArgument(Invocation))
	{
		return MakeFailure(
			TEXT("/ValueArguments/mode"),
			TEXT("PrimitiveInvocation.InvalidValueShape"),
			TEXT("Live property inspection requires exactly one read-only asset handle."));
	}
	FAssetDocumentVerifiedLeaseResult LeaseResult = Handles.AcquireVerifiedLease(
		Invocation.HandleArguments[0].Handle, MakeAssetContext(Invocation));
	if (!LeaseResult.Lease)
	{
		return MakeLeaseFailure(MoveTemp(LeaseResult));
	}
	if (!LeaseResult.Diagnostics.IsEmpty())
	{
		return MakeLeaseFailure(MoveTemp(LeaseResult));
	}
	UObject* LiveAsset = GetLiveAsset(*LeaseResult.Lease);
	if (!HasExactLiveAssetClass(LiveAsset, ExactClass))
	{
		return MakeFailure(
			TEXT("/HandleArguments/asset"),
			TEXT("PropertyInspection.ClassMismatch"),
			TEXT("The live asset does not have the exact requested class."));
	}
	return MakeInspectionOutcome(
		InspectAssetDocumentEditorProperties(ExactClass, LiveAsset, DeclaredNames));
}
}

FAssetDocumentPrimitiveOutcome
HandleAssetDocumentEditorPropertyInspectEvidence(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace EditorPropertyInspectHandlerLocal;
	(void)ActiveTransaction;
	FAssetDocumentRuntimeValueDto Arguments;
	TArray<FString> DeclaredNames;
	FStringView Mode;
	FStringView ClassPath;
	if (!TrySplitAssetDocumentEditorDeclaredPropertyArguments(Invocation.ValueArguments, Arguments, DeclaredNames)
		|| !TryReadArguments(Arguments, Mode, ClassPath))
	{
		return MakeFailure(
			TEXT("/ValueArguments"),
			TEXT("PrimitiveInvocation.InvalidValueShape"),
			TEXT("Property inspection requires an exact mode and class path."));
	}
	UClass* ExactClass = ResolveExactClass(ClassPath);
	if (ExactClass == nullptr)
	{
		return MakeFailure(
			TEXT("/ValueArguments/exact_asset_class_path"),
			TEXT("PropertyInspection.NonCanonicalClass"),
			TEXT("The exact asset class path is unresolved or non-canonical."));
	}
	if (Mode == TEXT("class_defaults"))
	{
		return HandleClassDefaultInspection(Invocation, *ExactClass, DeclaredNames);
	}
	if (Mode != TEXT("live_asset"))
	{
		return MakeFailure(
			TEXT("/ValueArguments/mode"),
			TEXT("PrimitiveInvocation.InvalidValueShape"),
			TEXT("Live property inspection requires exactly one read-only asset handle."));
	}
	return HandleLiveAssetInspection(Invocation, Handles, *ExactClass, DeclaredNames);
}
