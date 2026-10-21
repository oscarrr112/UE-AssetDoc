// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Property/AssetDocumentEditorPropertyHandlers.h"

#include "Provider/AssetDocumentEditorNativeIdentity.h"
#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/Property/AssetDocumentEditorEnhancedInputChecks.h"
#include "Provider/Property/AssetDocumentEditorPropertyInspectHandler.h"
#include "Provider/Property/AssetDocumentEditorPropertyAdmission.h"
#include "Provider/Property/AssetDocumentEditorPropertyBatchTokenPayload.h"
#include "Provider/Property/AssetDocumentEditorPropertyBatchTokenPrimitive.h"
#include "Provider/Property/AssetDocumentEditorPropertyBatchWritePrimitive.h"
#include "Provider/Property/AssetDocumentEditorPropertyEdit.h"
#include "Provider/Property/AssetDocumentEditorPropertyTokenPayload.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/Class.h"

namespace
{
FAssetDocumentPrimitiveOutcome MakePropertyOutcome(
	EAssetDocumentPrimitiveOutcomeStatus Status,
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = Status;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	FAssetDocumentRuntimeDiagnostic& Diagnostic = Outcome.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/Primitive");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Outcome;
}

const FAssetDocumentPrimitiveHandleArgument* FindPropertyHandleArgument(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const TCHAR* Name)
{
	for (const FAssetDocumentPrimitiveHandleArgument& Argument : Invocation.HandleArguments)
	{
		if (Argument.Name == Name)
		{
			return &Argument;
		}
	}
	return nullptr;
}

bool TryReadExactPropertyHandleArguments(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const TCHAR* ObjectArgumentName,
	const TCHAR* PropertyArgumentName,
	const FAssetDocumentPrimitiveHandleArgument*& OutObject,
	const FAssetDocumentPrimitiveHandleArgument*& OutProperty)
{
	OutObject = FindPropertyHandleArgument(Invocation, ObjectArgumentName);
	OutProperty = FindPropertyHandleArgument(Invocation, PropertyArgumentName);
	return OutObject != nullptr && OutProperty != nullptr;
}

FAssetDocumentHandleValidationContext MakePropertyValidationContext(
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

FAssetDocumentPrimitiveOutcome MakePropertyLeaseFailure(
	TArray<FAssetDocumentRuntimeDiagnostic>&& Diagnostics)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	Outcome.Diagnostics = MoveTemp(Diagnostics);
	if (Outcome.Diagnostics.IsEmpty())
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic = Outcome.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/Handle");
		Diagnostic.Code = TEXT("HandleMetadata.MissingLease");
		Diagnostic.Message = TEXT("Handle verification returned no lease.");
	}
	return Outcome;
}

bool HasPropertyLeaseFailed(const FAssetDocumentVerifiedLeaseResult& LeaseResult)
{
	return !LeaseResult.Lease || !LeaseResult.Diagnostics.IsEmpty();
}

struct FPropertyVerifiedLeasePair
{
	TUniquePtr<IAssetDocumentVerifiedHandleLease> Object;
	TUniquePtr<IAssetDocumentVerifiedHandleLease> Property;
};

bool TryAcquirePropertyLeases(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	const TCHAR* ObjectArgumentName,
	const TCHAR* PropertyArgumentName,
	EAssetDocumentHandleKind ObjectKind,
	EAssetDocumentHandleAccess ObjectAccess,
	FPropertyVerifiedLeasePair& OutLeases,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	const FAssetDocumentPrimitiveHandleArgument* Object = nullptr;
	const FAssetDocumentPrimitiveHandleArgument* Property = nullptr;
	if (!TryReadExactPropertyHandleArguments(
		Invocation,
		ObjectArgumentName,
		PropertyArgumentName,
		Object,
		Property))
	{
		OutFailure = MakePropertyOutcome(
			EAssetDocumentPrimitiveOutcomeStatus::Failed,
			TEXT("PrimitiveInvocation.InvalidHandleArguments"),
			TEXT("Property access requires exact target and property handles."));
		return false;
	}
	FAssetDocumentVerifiedLeaseResult ObjectLease = Handles.AcquireVerifiedLease(
		Object->Handle,
		MakePropertyValidationContext(
			Invocation, ObjectKind, ObjectAccess));
	if (HasPropertyLeaseFailed(ObjectLease))
	{
		OutFailure = MakePropertyLeaseFailure(MoveTemp(ObjectLease.Diagnostics));
		return false;
	}
	FAssetDocumentVerifiedLeaseResult PropertyLease =
		Handles.AcquireVerifiedLease(
			Property->Handle,
			MakePropertyValidationContext(
				Invocation,
				EAssetDocumentHandleKind::Property,
				EAssetDocumentHandleAccess::ReadOnly));
	if (HasPropertyLeaseFailed(PropertyLease))
	{
		OutFailure = MakePropertyLeaseFailure(
			MoveTemp(PropertyLease.Diagnostics));
		return false;
	}
	OutLeases.Object = MoveTemp(ObjectLease.Lease);
	OutLeases.Property = MoveTemp(PropertyLease.Lease);
	return true;
}

bool HasExactEmptyPropertyValueShape(
	const FAssetDocumentRuntimeValueDto& Arguments)
{
	return Arguments.Kind == EAssetDocumentRuntimeValueKind::Object
		&& Arguments.ObjectKeys.IsEmpty()
		&& Arguments.Elements.IsEmpty();
}

bool TryReadExactPropertyWriteValue(
	const FAssetDocumentRuntimeValueDto& Arguments,
	const FAssetDocumentRuntimeValueDto*& OutValue)
{
	OutValue = nullptr;
	if (Arguments.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Arguments.ObjectKeys.Num() != 1
		|| Arguments.Elements.Num() != 1
		|| Arguments.ObjectKeys[0] != TEXT("value"))
	{
		return false;
	}
	OutValue = &Arguments.Elements[0];
	return true;
}

bool TryReadExactPropertyClassPath(
	const FAssetDocumentRuntimeValueDto& Arguments,
	FStringView& OutClassPath)
{
	const TArray<FAssetDocumentEditorValueFieldSpec> Fields = {
		{TEXT("exact_asset_class_path"), EAssetDocumentRuntimeValueKind::String},
	};
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString DiagnosticCode;
	if (!TryReadAssetDocumentEditorExactObject(
		Arguments, Fields, Values, DiagnosticCode))
	{
		return false;
	}
	OutClassPath = Values[0]->StringValue;
	return true;
}

bool TryReadPropertyObjectIdentity(
	IAssetDocumentNativeHandlePayload& Payload,
	FName ExpectedPayloadTypeId,
	UObject*& OutObject)
{
	using namespace UE::AssetDocument::EditorPrimitive;
	OutObject = nullptr;
	INativeIdentityPayload* Identity =
		TryGetNativeIdentityPayload(Payload, ExpectedPayloadTypeId);
	if (Identity == nullptr
		&& ExpectedPayloadTypeId == AssetPayloadTypeId)
	{
		Identity = TryGetNativeIdentityPayload(
			Payload, CreatedAssetPayloadTypeId);
	}
	FObjectIdentityView View;
	if (Identity == nullptr
		|| !Payload.IsAlive()
		|| !Identity->TryGetObjectIdentity(View)
		|| View.Object == nullptr)
	{
		return false;
	}
	OutObject = View.Object;
	return true;
}

bool TryReadPropertyToken(
	IAssetDocumentNativeHandlePayload& Payload,
	FAssetDocumentEditorPropertyTokenView& OutView)
{
	using namespace UE::AssetDocument::EditorPrimitive;
	if (!Payload.IsAlive()
		|| Payload.GetPayloadTypeId() != PropertyTokenPayloadTypeId)
	{
		return false;
	}
	return static_cast<FAssetDocumentEditorPropertyTokenPayload&>(Payload)
		.TryGetView(OutView);
}

bool TryReadPropertyBatchToken(
	IAssetDocumentNativeHandlePayload& Payload,
	FAssetDocumentEditorPropertyBatchTokenView& OutView)
{
	const FName ExpectedTypeId =
		TEXT("assetdocument.editor.property_batch_token.v1");
	if (!Payload.IsAlive()
		|| Payload.GetPayloadTypeId() != ExpectedTypeId)
	{
		return false;
	}
	return static_cast<FAssetDocumentEditorPropertyBatchTokenPayload&>(
		Payload).TryGetView(OutView);
}

bool IsPropertyBatchFieldCompatibleWithTarget(
	const UClass& TargetClass,
	const FAssetDocumentEditorPropertyBatchTokenFieldView& Field)
{
	return Field.DeclaringClass != nullptr
		&& TargetClass.IsChildOf(Field.DeclaringClass);
}

bool HasExactPropertyBatchTarget(
	const UObject& Asset,
	const FAssetDocumentEditorPropertyBatchTokenView& Batch)
{
	if (Batch.Target != &Asset
		|| Batch.TargetClass != Asset.GetClass())
	{
		return false;
	}
	for (const FAssetDocumentEditorPropertyBatchTokenFieldView& Field :
		Batch.Fields)
	{
		if (!IsPropertyBatchFieldCompatibleWithTarget(
			*Batch.TargetClass, Field))
		{
			return false;
		}
	}
	return true;
}

bool TryResolvePropertyPayloads(
	IAssetDocumentNativeHandlePayload& ObjectPayload,
	IAssetDocumentNativeHandlePayload& PropertyPayload,
	FName ExpectedObjectPayloadTypeId,
	UObject*& OutObject,
	FAssetDocumentEditorPropertyTokenView& OutToken,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	if (!TryReadPropertyObjectIdentity(
		ObjectPayload, ExpectedObjectPayloadTypeId, OutObject)
		|| !TryReadPropertyToken(PropertyPayload, OutToken))
	{
		OutFailure = MakePropertyOutcome(
			EAssetDocumentPrimitiveOutcomeStatus::Failed,
			TEXT("PropertyToken.InvalidPayload"),
			TEXT("Property access requires live native target and property payloads."));
		return false;
	}
	if (!OutObject->IsA(OutToken.DeclaringClass))
	{
		OutFailure = MakePropertyOutcome(
			EAssetDocumentPrimitiveOutcomeStatus::Failed,
			TEXT("PropertyToken.ClassMismatch"),
			TEXT("The property token does not belong to the target class."));
		return false;
	}
	return true;
}

FAssetDocumentPrimitiveOutcome MakePropertyEditOutcome(
	FAssetDocumentEditorPropertyEditResult&& Edit)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = Edit.bSucceeded
		? EAssetDocumentPrimitiveOutcomeStatus::Succeeded
		: EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value = MoveTemp(Edit.Value);
	Outcome.ChangedSet = MoveTemp(Edit.ChangedSet);
	Outcome.Diagnostics = MoveTemp(Edit.Diagnostics);
	if (!Edit.bSucceeded && Outcome.Diagnostics.IsEmpty())
	{
		return MakePropertyOutcome(
			EAssetDocumentPrimitiveOutcomeStatus::Failed,
			TEXT("PropertyEdit.Failed"),
			TEXT("The admitted property operation failed."));
	}
	return Outcome;
}

FAssetDocumentPrimitiveOutcome MakePropertyEnumerationOutcome(
	FAssetDocumentEditorPropertyAdmissionResult&& Admission)
{
	if (!Admission.bSucceeded)
	{
		return MakePropertyLeaseFailure(MoveTemp(Admission.Diagnostics));
	}
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = MoveTemp(Admission.Value);
	FAssetDocumentPrimitiveHandleResult& Properties =
		Outcome.HandleResults.AddDefaulted_GetRef();
	Properties.Name = TEXT("properties");
	Properties.Handles = MoveTemp(Admission.Handles);
	return Outcome;
}
}

bool TryAcquireAssetDocumentEditorPropertyBatchTarget(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentOpaqueHandle& AssetHandle,
	IAssetDocumentHandleResolver& Handles,
	TUniquePtr<IAssetDocumentVerifiedHandleLease>& OutLease,
	UObject*& OutExactTarget,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	OutLease.Reset();
	OutExactTarget = nullptr;
	FAssetDocumentVerifiedLeaseResult LeaseResult =
		Handles.AcquireVerifiedLease(
			AssetHandle,
			MakePropertyValidationContext(
				Invocation,
				EAssetDocumentHandleKind::Asset,
				EAssetDocumentHandleAccess::ReadOnly));
	if (HasPropertyLeaseFailed(LeaseResult))
	{
		OutFailure =
			MakePropertyLeaseFailure(MoveTemp(LeaseResult.Diagnostics));
		return false;
	}
	if (!TryReadPropertyObjectIdentity(
		LeaseResult.Lease->GetPayloadChecked(),
		UE::AssetDocument::EditorPrimitive::AssetPayloadTypeId,
		OutExactTarget))
	{
		OutFailure = MakePropertyOutcome(
			EAssetDocumentPrimitiveOutcomeStatus::Failed,
			TEXT("PropertyBatchToken.InvalidAssetPayload"),
			TEXT("Property batch admission requires a live native asset payload."));
		return false;
	}
	OutLease = MoveTemp(LeaseResult.Lease);
	return true;
}

bool TryAcquireAssetDocumentEditorPropertyBatchWrite(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	TUniquePtr<IAssetDocumentVerifiedHandleLease>& OutAssetLease,
	TUniquePtr<IAssetDocumentVerifiedHandleLease>& OutBatchLease,
	UObject*& OutExactTarget,
	FAssetDocumentEditorPropertyBatchTokenView& OutBatchView,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	FPropertyVerifiedLeasePair Leases;
	if (!TryAcquirePropertyLeases(
		Invocation,
		Handles,
		TEXT("asset"),
		TEXT("property_batch"),
		EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable,
		Leases,
		OutFailure))
	{
		return false;
	}
	if (!TryReadPropertyObjectIdentity(
		Leases.Object->GetPayloadChecked(),
		UE::AssetDocument::EditorPrimitive::AssetPayloadTypeId,
		OutExactTarget)
		|| !TryReadPropertyBatchToken(
			Leases.Property->GetPayloadChecked(), OutBatchView))
	{
		OutFailure = MakePropertyOutcome(
			EAssetDocumentPrimitiveOutcomeStatus::Failed,
			TEXT("PropertyToken.InvalidPayload"),
			TEXT("Property batch write requires live exact native payloads."));
		return false;
	}
	if (!HasExactPropertyBatchTarget(*OutExactTarget, OutBatchView))
	{
		OutFailure = MakePropertyOutcome(
			EAssetDocumentPrimitiveOutcomeStatus::Failed,
			TEXT("PropertyBatchWrite.TargetMismatch"),
			TEXT("The admitted property batch belongs to another target."));
		return false;
	}
	OutAssetLease = MoveTemp(Leases.Object);
	OutBatchLease = MoveTemp(Leases.Property);
	return true;
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorPropertyEnumerateAdmitted(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	(void)ActiveTransaction;
	FAssetDocumentRuntimeValueDto Arguments;
	TArray<FString> DeclaredNames;
	FStringView ExactClassPath;
	if (!TrySplitAssetDocumentEditorDeclaredPropertyArguments(
			Invocation.ValueArguments, Arguments, DeclaredNames)
		|| !TryReadExactPropertyClassPath(Arguments, ExactClassPath))
	{
		return MakePropertyOutcome(
			EAssetDocumentPrimitiveOutcomeStatus::Failed,
			TEXT("PrimitiveInvocation.InvalidValueShape"),
			TEXT("Property enumeration requires one exact class path and optional admit_properties names."));
	}
	return MakePropertyEnumerationOutcome(
		EnumerateAssetDocumentEditorAdmittedProperties(
			ExactClassPath, Handles, DeclaredNames));
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorPropertyReadAdmitted(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	(void)ActiveTransaction;
	const FAssetDocumentRuntimeValueDto& ReadArguments = Invocation.ValueArguments;
	const FAssetDocumentRuntimeValueDto* AsAuthored = nullptr;
	// {"sparse": true}: extract's diff_only read, sparse over the class default (struct members equal to it left out).
	bool bSparse = false;
	if (ReadArguments.Kind == EAssetDocumentRuntimeValueKind::Object
		&& ReadArguments.ObjectKeys.Num() == 1
		&& ReadArguments.Elements.Num() == 1
		&& ReadArguments.ObjectKeys[0] == TEXT("as_authored"))
	{
		AsAuthored = &ReadArguments.Elements[0];
	}
	else if (ReadArguments.Kind == EAssetDocumentRuntimeValueKind::Object
		&& ReadArguments.ObjectKeys.Num() == 1
		&& ReadArguments.Elements.Num() == 1
		&& ReadArguments.ObjectKeys[0] == TEXT("sparse")
		&& ReadArguments.Elements[0].Kind == EAssetDocumentRuntimeValueKind::Boolean
		&& ReadArguments.Elements[0].BooleanValue)
	{
		bSparse = true;
	}
	else if (!HasExactEmptyPropertyValueShape(ReadArguments))
	{
		return MakePropertyOutcome(
			EAssetDocumentPrimitiveOutcomeStatus::Failed,
			TEXT("PrimitiveInvocation.InvalidValueShape"),
			TEXT("Property read requires an exact empty value object, one as_authored value or sparse: true."));
	}
	FPropertyVerifiedLeasePair Leases;
	FAssetDocumentPrimitiveOutcome Failure;
	if (!TryAcquirePropertyLeases(
		Invocation,
		Handles,
		TEXT("asset"),
		TEXT("property"),
		EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly,
		Leases,
		Failure))
	{
		return Failure;
	}
	UObject* Asset = nullptr;
	FAssetDocumentEditorPropertyTokenView Token;
	if (!TryResolvePropertyPayloads(
		Leases.Object->GetPayloadChecked(),
		Leases.Property->GetPayloadChecked(),
		UE::AssetDocument::EditorPrimitive::AssetPayloadTypeId,
		Asset,
		Token,
		Failure))
	{
		return Failure;
	}
	if (bSparse)
	{
		return MakePropertyEditOutcome(ReadAssetDocumentEditorAdmittedPropertyChanged(
			*Asset, Token, *Asset->GetClass()->GetDefaultObject()));
	}
	return MakePropertyEditOutcome(
		ReadAssetDocumentEditorAdmittedDocumentProperty(*Asset, Token, AsAuthored));
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorPropertyWriteAdmitted(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	const FAssetDocumentRuntimeValueDto* DesiredValue = nullptr;
	if (!TryReadExactPropertyWriteValue(
		Invocation.ValueArguments, DesiredValue))
	{
		return MakePropertyOutcome(
			EAssetDocumentPrimitiveOutcomeStatus::Failed,
			TEXT("PrimitiveInvocation.InvalidValueShape"),
			TEXT("Property write requires the exact value argument object."));
	}
	if (ActiveTransaction == nullptr)
	{
		return MakePropertyOutcome(
			EAssetDocumentPrimitiveOutcomeStatus::Failed,
			TEXT("GenericProvider.InvalidTransaction"),
			TEXT("Property write requires the active transaction."));
	}
	FPropertyVerifiedLeasePair Leases;
	FAssetDocumentPrimitiveOutcome Failure;
	if (!TryAcquirePropertyLeases(
		Invocation,
		Handles,
		TEXT("asset"),
		TEXT("property"),
		EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::Mutable,
		Leases,
		Failure))
	{
		return Failure;
	}
	UObject* Asset = nullptr;
	FAssetDocumentEditorPropertyTokenView Token;
	if (!TryResolvePropertyPayloads(
		Leases.Object->GetPayloadChecked(),
		Leases.Property->GetPayloadChecked(),
		UE::AssetDocument::EditorPrimitive::AssetPayloadTypeId,
		Asset,
		Token,
		Failure))
	{
		return Failure;
	}
	return MakePropertyEditOutcome(
		WriteAssetDocumentEditorAdmittedProperty(
			*Asset, Token, *DesiredValue, *ActiveTransaction));
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorPropertyPreflightAdmitted(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	(void)ActiveTransaction;
	const FAssetDocumentRuntimeValueDto* DesiredValue = nullptr;
	if (!TryReadExactPropertyWriteValue(
		Invocation.ValueArguments, DesiredValue))
	{
		return MakePropertyOutcome(
			EAssetDocumentPrimitiveOutcomeStatus::Failed,
			TEXT("PrimitiveInvocation.InvalidValueShape"),
			TEXT("Property preflight requires the exact value argument object."));
	}
	FPropertyVerifiedLeasePair Leases;
	FAssetDocumentPrimitiveOutcome Failure;
	if (!TryAcquirePropertyLeases(
		Invocation,
		Handles,
		TEXT("scratch"),
		TEXT("property"),
		EAssetDocumentHandleKind::Scratch,
		EAssetDocumentHandleAccess::Mutable,
		Leases,
		Failure))
	{
		return Failure;
	}
	UObject* Scratch = nullptr;
	FAssetDocumentEditorPropertyTokenView Token;
	if (!TryResolvePropertyPayloads(
		Leases.Object->GetPayloadChecked(),
		Leases.Property->GetPayloadChecked(),
		UE::AssetDocument::EditorPrimitive::ScratchPayloadTypeId,
		Scratch,
		Token,
		Failure))
	{
		return Failure;
	}
	return MakePropertyEditOutcome(
		PreflightAssetDocumentEditorAdmittedProperty(
			*Scratch, Token, *DesiredValue));
}

TConstArrayView<FAssetDocumentEditorPrimitiveBinding>
GetAssetDocumentEditorPropertyPrimitiveBindings()
{
	static const FAssetDocumentEditorPrimitiveBinding Bindings[] = {
		{TEXT("editor.property.inspect_evidence.v1"), 1u,
			&HandleAssetDocumentEditorPropertyInspectEvidence},
		{TEXT("editor.property.enumerate_admitted.v1"), 1u,
			&HandleAssetDocumentEditorPropertyEnumerateAdmitted},
		{TEXT("editor.property.enumerate_admitted_batch.v1"), 1u,
			&HandleAssetDocumentEditorPropertyEnumerateAdmittedBatch},
		{TEXT("editor.property.read_admitted.v1"), 1u,
			&HandleAssetDocumentEditorPropertyReadAdmitted},
		{TEXT("editor.property.write_admitted.v1"), 1u,
			&HandleAssetDocumentEditorPropertyWriteAdmitted},
		{TEXT("editor.property.write_admitted_batch.v1"), 1u,
			&HandleAssetDocumentEditorPropertyWriteAdmittedBatch},
		{TEXT("editor.property.preflight_admitted.v1"), 1u,
			&HandleAssetDocumentEditorPropertyPreflightAdmitted},
		{TEXT("editor.enhanced_input.check_domain.v1"), 1u,
			&HandleAssetDocumentEditorEnhancedInputCheckDomain},
	};
	return MakeArrayView(Bindings);
}
