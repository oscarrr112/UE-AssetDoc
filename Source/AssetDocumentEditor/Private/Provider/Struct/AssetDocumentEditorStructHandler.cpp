// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Struct/AssetDocumentEditorStructHandler.h"

#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/Struct/AssetDocumentEditorStructCodec.h"
#include "Provider/Struct/AssetDocumentEditorStructSchemaPayload.h"
#include "UObject/SoftObjectPath.h"

namespace EditorStructHandlerLocal
{
FAssetDocumentRuntimeDiagnostic MakeStructDiagnostic(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/Value");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Diagnostic;
}

FAssetDocumentPrimitiveOutcome MakeStructFailureWithDiagnostics(
	const TCHAR* Code,
	const TCHAR* Message,
	TConstArrayView<FAssetDocumentRuntimeDiagnostic> Diagnostics)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	if (Diagnostics.IsEmpty())
	{
		Outcome.Diagnostics.Add(MakeStructDiagnostic(Code, Message));
	}
	else
	{
		Outcome.Diagnostics.Append(Diagnostics.GetData(), Diagnostics.Num());
	}
	return Outcome;
}

FAssetDocumentPrimitiveOutcome MakeStructFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	return MakeStructFailureWithDiagnostics(
		Code, Message, TConstArrayView<FAssetDocumentRuntimeDiagnostic>());
}

const FAssetDocumentRuntimeValueDto* FindStructValueField(
	const FAssetDocumentRuntimeValueDto& Arguments,
	FStringView Name)
{
	const int32 Index = Arguments.ObjectKeys.Find(FString(Name));
	return Arguments.Elements.IsValidIndex(Index)
		? &Arguments.Elements[Index]
		: nullptr;
}

bool TryReadStructOperation(
	const FAssetDocumentRuntimeValueDto& Arguments,
	FString& OutOperation)
{
	const FAssetDocumentRuntimeValueDto* Operation =
		FindStructValueField(Arguments, TEXT("operation"));
	if (Operation == nullptr
		|| Operation->Kind != EAssetDocumentRuntimeValueKind::String)
	{
		return false;
	}
	OutOperation = Operation->StringValue;
	return true;
}

bool TryReadResolveArguments(
	const FAssetDocumentRuntimeValueDto& Arguments,
	FString& OutStructPath)
{
	const TArray<FAssetDocumentEditorValueFieldSpec> Specs = {
		{TEXT("operation"), EAssetDocumentRuntimeValueKind::String},
		{TEXT("struct"), EAssetDocumentRuntimeValueKind::String},
	};
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString DiagnosticCode;
	if (!TryReadAssetDocumentEditorExactObject(
		Arguments, Specs, Values, DiagnosticCode))
	{
		return false;
	}
	OutStructPath = Values[1]->StringValue;
	return Values[0]->StringValue == TEXT("resolve_schema");
}

bool TryReadCanonicalResolveArguments(
	const FAssetDocumentRuntimeValueDto& Arguments,
	FString& OutStructPath)
{
	if (!TryReadResolveArguments(Arguments, OutStructPath))
	{
		return false;
	}
	return !OutStructPath.IsEmpty()
		&& OutStructPath.TrimStartAndEnd() == OutStructPath;
}

bool IsExactValidateShape(
	const FAssetDocumentRuntimeValueDto& Arguments)
{
	const FAssetDocumentRuntimeValueDto* Operation =
		FindStructValueField(Arguments, TEXT("operation"));
	const FAssetDocumentRuntimeValueDto* Value =
		FindStructValueField(Arguments, TEXT("value"));
	return Arguments.ObjectKeys.Num() == 2
		&& Operation != nullptr
		&& Operation->Kind == EAssetDocumentRuntimeValueKind::String
		&& Operation->StringValue == TEXT("validate")
		&& Value != nullptr;
}

const FAssetDocumentOpaqueHandle* FindSchemaHandle(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	for (const FAssetDocumentPrimitiveHandleArgument& Argument :
		Invocation.HandleArguments)
	{
		if (Argument.Name == TEXT("schema"))
		{
			return &Argument.Handle;
		}
	}
	return nullptr;
}

FAssetDocumentHandleValidationContext MakeSchemaValidationContext(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Invocation.RequestId;
	Context.TransactionId = Invocation.TransactionId;
	Context.RuntimeRevision = Invocation.RuntimeRevision;
	Context.ProviderId = TEXT("editor");
	Context.Phase = Invocation.Phase;
	Context.RequiredKind = EAssetDocumentHandleKind::Struct;
	Context.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
	return Context;
}

FAssetDocumentPrimitiveOutcome MakeResolveSuccess(
	FString CanonicalPath,
	FAssetDocumentOpaqueHandle Handle)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	Outcome.Value.ObjectKeys.Add(TEXT("canonical_struct"));
	FAssetDocumentRuntimeValueDto& PathValue =
		Outcome.Value.Elements.AddDefaulted_GetRef();
	PathValue.Kind = EAssetDocumentRuntimeValueKind::String;
	PathValue.StringValue = MoveTemp(CanonicalPath);
	FAssetDocumentPrimitiveHandleResult& Result =
		Outcome.HandleResults.AddDefaulted_GetRef();
	Result.Name = TEXT("schema");
	Result.Handles.Add(MoveTemp(Handle));
	return Outcome;
}

UScriptStruct* TryLoadExactStructSchema(const FString& StructPath)
{
	const FSoftObjectPath SoftPath(StructPath);
	UScriptStruct* Schema = Cast<UScriptStruct>(SoftPath.TryLoad());
	if (Schema == nullptr)
	{
		return nullptr;
	}
	return Schema->GetPathName() == StructPath ? Schema : nullptr;
}

FAssetDocumentPrimitiveOutcome ResolveStructSchema(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles)
{
	FString StructPath;
	if (!TryReadCanonicalResolveArguments(
		Invocation.ValueArguments, StructPath))
	{
		return MakeStructFailure(
			TEXT("StructCodec.InvalidArguments"),
			TEXT("resolve_schema requires one canonical struct path."));
	}
	UScriptStruct* Schema = TryLoadExactStructSchema(StructPath);
	if (Schema == nullptr)
	{
		return MakeStructFailure(
			TEXT("StructCodec.NonCanonicalSchemaPath"),
			TEXT("The struct path did not resolve to the exact canonical schema."));
	}
	if (!IsAssetDocumentEditorStructSchemaAdmitted(*Schema))
	{
		return MakeStructFailure(
			TEXT("StructCodec.UnsupportedSchema"),
			TEXT("The struct contains a property family outside the strict codec."));
	}
	FAssetDocumentIssuedHandleResult Issued = Handles.Issue(
		EAssetDocumentHandleKind::Struct,
		EAssetDocumentHandleAccess::ReadOnly,
		MakeUnique<FAssetDocumentEditorStructSchemaPayload>(
			*Schema, StructPath));
	if (!Issued.Handle.IsSet())
	{
		return MakeStructFailureWithDiagnostics(
			TEXT("StructCodec.HandleIssueFailed"),
			TEXT("The schema token could not be issued."),
			Issued.Diagnostics);
	}
	return MakeResolveSuccess(StructPath, Issued.Handle.GetValue());
}

FAssetDocumentEditorStructCodecResult RoundTripVerifiedStructLease(
	const IAssetDocumentVerifiedHandleLease& Lease,
	const FAssetDocumentRuntimeValueDto& Value)
{
	FAssetDocumentEditorStructSchemaPayload* Payload =
		TryGetAssetDocumentEditorStructSchemaPayload(
			Lease.GetPayloadChecked());
	UScriptStruct* Schema = Payload ? Payload->GetSchema() : nullptr;
	if (Schema == nullptr)
	{
		FAssetDocumentEditorStructCodecResult Result;
		Result.DiagnosticCode = TEXT("StructCodec.InvalidSchemaPayload");
		return Result;
	}
	return RoundTripAssetDocumentEditorStructValue(*Schema, Value);
}

FAssetDocumentPrimitiveOutcome ValidateStructValue(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles)
{
	const FAssetDocumentOpaqueHandle* SchemaHandle = FindSchemaHandle(Invocation);
	if (SchemaHandle == nullptr)
	{
		return MakeStructFailure(
			TEXT("StructCodec.SchemaTokenRequired"),
			TEXT("Struct validation requires an admitted schema token."));
	}
	if (!IsExactValidateShape(Invocation.ValueArguments))
	{
		return MakeStructFailure(
			TEXT("StructCodec.InvalidArguments"),
			TEXT("validate accepts only operation and value."));
	}
	FAssetDocumentVerifiedLeaseResult LeaseResult =
		Handles.AcquireVerifiedLease(
			*SchemaHandle, MakeSchemaValidationContext(Invocation));
	if (!LeaseResult.Lease)
	{
		return MakeStructFailureWithDiagnostics(
			TEXT("StructCodec.SchemaTokenRejected"),
			TEXT("The schema token was rejected."),
			LeaseResult.Diagnostics);
	}
	const FAssetDocumentEditorStructCodecResult Codec =
		RoundTripVerifiedStructLease(
			*LeaseResult.Lease,
			*FindStructValueField(Invocation.ValueArguments, TEXT("value")));
	if (!Codec.bSucceeded)
	{
		return MakeStructFailure(
			*Codec.DiagnosticCode,
			TEXT("The typed struct value failed strict roundtrip."));
	}
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	Outcome.Value.ObjectKeys.Add(TEXT("canonical_value"));
	Outcome.Value.Elements.Add(Codec.CanonicalValue);
	return Outcome;
}
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorStructCodec(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	using namespace EditorStructHandlerLocal;
	(void)ActiveTransaction;
	FString Operation;
	if (!TryReadStructOperation(Invocation.ValueArguments, Operation))
	{
		return MakeStructFailure(
			TEXT("StructCodec.InvalidArguments"),
			TEXT("The struct operation is missing or invalid."));
	}
	if (Operation == TEXT("resolve_schema"))
	{
		return ResolveStructSchema(Invocation, Handles);
	}
	if (Operation == TEXT("validate"))
	{
		return ValidateStructValue(Invocation, Handles);
	}
	return MakeStructFailure(
		TEXT("StructCodec.InvalidOperation"),
		TEXT("The struct operation is not supported."));
}

TConstArrayView<FAssetDocumentEditorPrimitiveBinding>
GetAssetDocumentEditorStructPrimitiveBindings()
{
	static const FAssetDocumentEditorPrimitiveBinding Bindings[] = {
		{TEXT("editor.fragment.struct_codec.v1"), 1u,
			&HandleAssetDocumentEditorStructCodec},
	};
	return MakeArrayView(Bindings);
}
