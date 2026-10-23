// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Embedded/AssetDocumentEditorEmbeddedHandler.h"

#include "Engine/AssetUserData.h"
#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/Cutover/AssetDocumentEditorMutationFootprintBuilders.h"
#include "Provider/Embedded/AssetDocumentEditorEmbeddedAdmission.h"
#include "Provider/Embedded/AssetDocumentEditorSubobjectPayload.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/Class.h"

namespace EditorEmbeddedHandlerLocal
{
using namespace UE::AssetDocument::EditorPrimitive;

FAssetDocumentRuntimeDiagnostic MakeEmbeddedDiagnostic(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/Primitive");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Diagnostic;
}

FAssetDocumentPrimitiveOutcome MakeEmbeddedFailureWithDiagnostics(
	const TCHAR* Code,
	const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	if (Diagnostics.IsEmpty())
	{
		Outcome.Diagnostics.Add(MakeEmbeddedDiagnostic(Code, Message));
	}
	else
	{
		Outcome.Diagnostics.Append(
			Diagnostics.GetData(), Diagnostics.Num());
	}
	return Outcome;
}

FAssetDocumentPrimitiveOutcome MakeEmbeddedFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	return MakeEmbeddedFailureWithDiagnostics(
		Code,
		Message,
		TConstArrayView<FAssetDocumentRuntimeDiagnostic>());
}

const FAssetDocumentPrimitiveHandleArgument* FindEmbeddedHandle(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	FStringView Name)
{
	for (const FAssetDocumentPrimitiveHandleArgument& Argument :
		Invocation.HandleArguments)
	{
		if (Argument.Name == Name)
		{
			return &Argument;
		}
	}
	return nullptr;
}

FAssetDocumentHandleValidationContext MakeEmbeddedValidationContext(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	EAssetDocumentHandleKind Kind,
	EAssetDocumentHandleAccess Access)
{
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Invocation.RequestId;
	Context.TransactionId = Invocation.TransactionId;
	Context.RuntimeRevision = Invocation.RuntimeRevision;
	Context.ProviderId = TEXT("editor");
	Context.Phase = Invocation.Phase;
	Context.RequiredKind = Kind;
	Context.RequiredAccess = Access;
	return Context;
}

bool TryReadEmbeddedObjectIdentity(
	IAssetDocumentNativeHandlePayload& Payload,
	UObject*& OutObject)
{
	INativeIdentityPayload* Identity =
		TryGetNativeIdentityPayload(Payload, AssetPayloadTypeId);
	FObjectIdentityView View;
	if (Identity == nullptr || !Identity->TryGetObjectIdentity(View))
	{
		return false;
	}
	if (View.Object == nullptr
		|| View.TypeRevision != View.Object->GetUniqueID())
	{
		return false;
	}
	OutObject = View.Object;
	return View.CanonicalObjectPath == OutObject->GetPathName();
}

bool TryReadEmbeddedClassIdentity(
	IAssetDocumentNativeHandlePayload& Payload,
	UClass*& OutClass)
{
	INativeIdentityPayload* Identity =
		TryGetNativeIdentityPayload(Payload, ClassPayloadTypeId);
	FClassIdentityView View;
	if (Identity == nullptr || !Identity->TryGetClassIdentity(View))
	{
		return false;
	}
	if (View.Class == nullptr
		|| View.TypeRevision != View.Class->GetUniqueID())
	{
		return false;
	}
	OutClass = View.Class;
	return View.CanonicalClassPath == OutClass->GetPathName();
}

struct FResolvedEmbeddedRequest
{
	FAssetDocumentVerifiedLeaseResult OuterLease;
	FAssetDocumentVerifiedLeaseResult ClassLease;
	FAssetDocumentEditorEmbeddedAdmission Admission;
};

bool TryAcquireEmbeddedLeases(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FResolvedEmbeddedRequest& OutRequest)
{
	const FAssetDocumentPrimitiveHandleArgument* Outer =
		FindEmbeddedHandle(Invocation, TEXT("outer_asset"));
	const FAssetDocumentPrimitiveHandleArgument* ObjectClass =
		FindEmbeddedHandle(Invocation, TEXT("object_class"));
	if (Outer == nullptr || ObjectClass == nullptr)
	{
		return false;
	}
	OutRequest.OuterLease = Handles.AcquireVerifiedLease(
		Outer->Handle,
		MakeEmbeddedValidationContext(
			Invocation,
			EAssetDocumentHandleKind::Asset,
			EAssetDocumentHandleAccess::Mutable));
	OutRequest.ClassLease = Handles.AcquireVerifiedLease(
		ObjectClass->Handle,
		MakeEmbeddedValidationContext(
			Invocation,
			EAssetDocumentHandleKind::Class,
			EAssetDocumentHandleAccess::ReadOnly));
	return OutRequest.OuterLease.Lease.IsValid()
		&& OutRequest.ClassLease.Lease.IsValid();
}

bool TryResolveEmbeddedRequest(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FResolvedEmbeddedRequest& OutRequest)
{
	if (!TryAcquireEmbeddedLeases(
		Invocation, Handles, OutRequest))
	{
		return false;
	}
	UObject* Outer = nullptr;
	UClass* ObjectClass = nullptr;
	if (!TryReadEmbeddedObjectIdentity(
		OutRequest.OuterLease.Lease->GetPayloadChecked(), Outer))
	{
		return false;
	}
	if (!TryReadEmbeddedClassIdentity(
		OutRequest.ClassLease.Lease->GetPayloadChecked(),
		ObjectClass))
	{
		return false;
	}
	return TryAdmitAssetDocumentEditorEmbeddedObject(
		Invocation.ValueArguments,
		*Outer,
		*ObjectClass,
		OutRequest.Admission);
}

FAssetDocumentContractValidationResult EnlistEmbeddedFootprint(
	const FAssetDocumentEditorEmbeddedAdmission& Admission,
	IAssetDocumentAtomicTransaction& ActiveTransaction)
{
	return ActiveTransaction.EnlistFootprint(
		BuildAssetDocumentEditorEmbeddedMutationFootprint(
			Admission.Outer->GetPathName()));
}

FAssetDocumentPrimitiveOutcome FailAfterEmbeddedHelper(
	const FAssetDocumentEditorEmbeddedAdmission& Admission,
	const TCHAR* Code,
	const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	Admission.Owner->RemoveUserDataOfClass(Admission.ObjectClass);
	if (Admission.Owner->GetAssetUserDataOfClass(
		Admission.ObjectClass) != nullptr)
	{
		return MakeEmbeddedFailure(
			TEXT("EmbeddedObject.InverseFailed"),
			TEXT("The public inverse did not remove the attached object."));
	}
	return MakeEmbeddedFailureWithDiagnostics(
		Code, Message, Diagnostics);
}

bool IsExactCreatedSubobject(
	const FAssetDocumentEditorEmbeddedAdmission& Admission,
	const UAssetUserData* Created)
{
	return Created != nullptr
		&& Created->GetClass() == Admission.ObjectClass
		&& Created->GetOuter() == Admission.Outer
		&& !Created->GetPathName().IsEmpty();
}

FAssetDocumentRuntimeValueDto MakeEmbeddedStringValue(
	FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentPrimitiveOutcome MakeEmbeddedSuccess(
	const FAssetDocumentEditorEmbeddedAdmission& Admission,
	const UAssetUserData& Created,
	FAssetDocumentOpaqueHandle Handle)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	Outcome.Value.ObjectKeys = {
		TEXT("category_id"),
		TEXT("canonical_class"),
		TEXT("outer_path"),
		TEXT("subobject_path"),
	};
	Outcome.Value.Elements = {
		MakeEmbeddedStringValue(Admission.CategoryId),
		MakeEmbeddedStringValue(
			Admission.ObjectClass->GetPathName()),
		MakeEmbeddedStringValue(Admission.Outer->GetPathName()),
		MakeEmbeddedStringValue(Created.GetPathName()),
	};
	FAssetDocumentPrimitiveHandleResult& Result =
		Outcome.HandleResults.AddDefaulted_GetRef();
	Result.Name = TEXT("subobject");
	Result.Handles.Add(MoveTemp(Handle));
	return Outcome;
}

FAssetDocumentPrimitiveOutcome CreateAdmittedEmbeddedObject(
	const FAssetDocumentEditorEmbeddedAdmission& Admission,
	IAssetDocumentHandleResolver& Handles)
{
	const bool bAdded = Admission.Owner->AddAssetUserDataOfClass(
		Admission.ObjectClass);
	UAssetUserData* Created =
		Admission.Owner->GetAssetUserDataOfClass(
			Admission.ObjectClass);
	if (!bAdded || !IsExactCreatedSubobject(Admission, Created))
	{
		return FailAfterEmbeddedHelper(
			Admission,
			TEXT("EmbeddedObject.HelperFailed"),
			TEXT("The public AssetUserData helper did not create the exact object."),
			TConstArrayView<FAssetDocumentRuntimeDiagnostic>());
	}
	FAssetDocumentIssuedHandleResult Issued = Handles.Issue(
		EAssetDocumentHandleKind::Subobject,
		EAssetDocumentHandleAccess::Mutable,
		MakeUnique<FAssetDocumentEditorSubobjectPayload>(
			*Created, *Admission.Outer, Admission.CategoryId));
	if (!Issued.Handle.IsSet())
	{
		return FailAfterEmbeddedHelper(
			Admission,
			TEXT("EmbeddedObject.HandleIssueFailed"),
			TEXT("The attached object handle could not be issued."),
			Issued.Diagnostics);
	}
	return MakeEmbeddedSuccess(
		Admission, *Created, Issued.Handle.GetValue());
}
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorEmbeddedObjectCreate(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace EditorEmbeddedHandlerLocal;
	if (ActiveTransaction == nullptr)
	{
		return MakeEmbeddedFailure(
			TEXT("EmbeddedObject.TransactionRequired"),
			TEXT("Embedded creation requires an active transaction."));
	}
	FResolvedEmbeddedRequest Request;
	if (!TryResolveEmbeddedRequest(
		Invocation, Handles, Request))
	{
		return MakeEmbeddedFailure(
			TEXT("UnsupportedEmbeddedObjectLifecycle"),
			TEXT("The embedded category, class, outer, or handle is not admitted."));
	}
	const FAssetDocumentContractValidationResult Enlisted =
		EnlistEmbeddedFootprint(
			Request.Admission, *ActiveTransaction);
	if (!Enlisted.bValid)
	{
		return MakeEmbeddedFailureWithDiagnostics(
			TEXT("EmbeddedObject.FootprintRejected"),
			TEXT("The transaction rejected the ownership footprint."),
			Enlisted.Diagnostics);
	}
	return CreateAdmittedEmbeddedObject(
		Request.Admission, Handles);
}

TConstArrayView<FAssetDocumentEditorPrimitiveBinding>
GetAssetDocumentEditorEmbeddedPrimitiveBindings()
{
	static const FAssetDocumentEditorPrimitiveBinding Bindings[] = {
		{TEXT("editor.fragment.embedded_object_create.v1"), 1u,
			&HandleAssetDocumentEditorEmbeddedObjectCreate},
	};
	return MakeArrayView(Bindings);
}
