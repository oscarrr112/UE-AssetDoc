// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Property/AssetDocumentEditorPropertyBatchWritePrimitive.h"

#include "Provider/AssetDocumentEditorValueDto.h"
#include "Provider/Property/AssetDocumentEditorPropertyAdmission.h"
#include "Provider/Property/AssetDocumentEditorPropertyBatchTokenPayload.h"
#include "Provider/Property/AssetDocumentEditorPropertyEdit.h"
#include "Provider/Property/AssetDocumentEditorPropertyHandlers.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "UObject/UnrealType.h"

namespace
{
struct FPropertyBatchWriteRowView
{
	FStringView Name;
	const FAssetDocumentRuntimeValueDto* Value = nullptr;
};

struct FMatchedPropertyBatchWriteRow
{
	const FAssetDocumentEditorPropertyBatchTokenFieldView* Field = nullptr;
	const FAssetDocumentRuntimeValueDto* Value = nullptr;
};

struct FPreparedPropertyBatchWriteRow
{
	FAssetDocumentEditorPropertyTokenView Token;
	FAssetDocumentEditorPreparedPropertyWrite Prepared;
};

FAssetDocumentPrimitiveOutcome MakePropertyBatchWriteFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Outcome.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/Primitive/properties");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Outcome;
}

FAssetDocumentPrimitiveOutcome MakePropertyBatchWriteFailure(
	TArray<FAssetDocumentRuntimeDiagnostic>&& Diagnostics)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	Outcome.Diagnostics = MoveTemp(Diagnostics);
	if (Outcome.Diagnostics.IsEmpty())
	{
		Outcome.Diagnostics.AddDefaulted_GetRef().Code =
			TEXT("PropertyBatchWrite.Failed");
	}
	return Outcome;
}

bool HasExactPropertyBatchWriteRowShape(
	const FAssetDocumentRuntimeValueDto& Row)
{
	return Row.Kind == EAssetDocumentRuntimeValueKind::Object
		&& Row.ObjectKeys.Num() == 2
		&& Row.Elements.Num() == 2;
}

bool HasValidPropertyBatchWriteRowName(
	const FAssetDocumentRuntimeValueDto& Name)
{
	return Name.Kind == EAssetDocumentRuntimeValueKind::String
		&& !Name.StringValue.IsEmpty();
}

bool TryReadPropertyBatchWriteRow(
	const FAssetDocumentRuntimeValueDto& Row,
	FPropertyBatchWriteRowView& OutRow)
{
	if (!HasExactPropertyBatchWriteRowShape(Row))
	{
		return false;
	}
	const int32 NameIndex = Row.ObjectKeys.IndexOfByKey(TEXT("name"));
	const int32 ValueIndex = Row.ObjectKeys.IndexOfByKey(TEXT("value"));
	if (!Row.Elements.IsValidIndex(NameIndex)
		|| !Row.Elements.IsValidIndex(ValueIndex))
	{
		return false;
	}
	if (!HasValidPropertyBatchWriteRowName(Row.Elements[NameIndex]))
	{
		return false;
	}
	OutRow.Name = Row.Elements[NameIndex].StringValue;
	OutRow.Value = &Row.Elements[ValueIndex];
	return true;
}

bool TryReadPropertyBatchWriteRows(
	const FAssetDocumentRuntimeValueDto& Arguments,
	TArray<FPropertyBatchWriteRowView>& OutRows)
{
	const TArray<FAssetDocumentEditorValueFieldSpec> Fields = {
		{TEXT("properties"), EAssetDocumentRuntimeValueKind::Array},
	};
	TArray<const FAssetDocumentRuntimeValueDto*> Values;
	FString DiagnosticCode;
	if (!TryReadAssetDocumentEditorExactObject(
		Arguments, Fields, Values, DiagnosticCode))
	{
		return false;
	}
	OutRows.Reset();
	OutRows.Reserve(Values[0]->Elements.Num());
	for (const FAssetDocumentRuntimeValueDto& Row : Values[0]->Elements)
	{
		FPropertyBatchWriteRowView& Parsed =
			OutRows.AddDefaulted_GetRef();
		if (!TryReadPropertyBatchWriteRow(Row, Parsed))
		{
			OutRows.Reset();
			return false;
		}
	}
	return true;
}

int32 FindPropertyBatchField(
	TConstArrayView<FAssetDocumentEditorPropertyBatchTokenFieldView> Fields,
	FStringView Name,
	int32 Start)
{
	for (int32 Index = Start; Index < Fields.Num(); ++Index)
	{
		if (Fields[Index].Property->GetAuthoredName() == Name)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

bool TryMatchPropertyBatchRows(
	TConstArrayView<FPropertyBatchWriteRowView> Rows,
	TConstArrayView<FAssetDocumentEditorPropertyBatchTokenFieldView> Fields,
	TConstArrayView<FString> DeclaredNames,
	TArray<FMatchedPropertyBatchWriteRow>& OutMatched,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	TSet<FString> Seen;
	int32 Cursor = 0;
	for (const FPropertyBatchWriteRowView& Row : Rows)
	{
		const FString Name(Row.Name);
		if (Seen.Contains(Name))
		{
			OutFailure = MakePropertyBatchWriteFailure(
				TEXT("PropertyBatchWrite.DuplicateProperty"),
				TEXT("A property batch row name is duplicated."));
			return false;
		}
		const int32 Match = FindPropertyBatchField(Fields, Row.Name, Cursor);
		if (Match == INDEX_NONE)
		{
			const bool bOutOfOrder =
				FindPropertyBatchField(Fields.Left(Cursor), Row.Name, 0)
				!= INDEX_NONE;
			OutFailure = MakePropertyBatchWriteFailure(
				bOutOfOrder
					? TEXT("PropertyBatchWrite.OutOfOrder")
					: TEXT("PropertyBatchWrite.UnknownProperty"),
				TEXT("A property batch row is unknown or out of order."));
			return false;
		}
		// A declared field is writable only when this invocation declares it too,
		// so mutation preparation sees the same admission as the batch token.
		if (Fields[Match].bDeclared && !DeclaredNames.Contains(Name))
		{
			OutFailure = MakePropertyBatchWriteFailure(
				TEXT("PropertyBatchWrite.UndeclaredProperty"),
				TEXT("A declared property row requires the same admit_properties declaration."));
			return false;
		}
		Seen.Add(Name);
		OutMatched.Add({&Fields[Match], Row.Value});
		Cursor = Match + 1;
	}
	return true;
}

FAssetDocumentEditorPropertyTokenView MakeAdmittedPropertyView(
	const FAssetDocumentEditorPropertyBatchTokenFieldView& Field)
{
	FAssetDocumentEditorPropertyTokenView Token;
	Token.DeclaringClass = Field.DeclaringClass;
	Token.Property = Field.Property;
	Token.CodecId = Field.CodecId;
	Token.TypeRevision = Field.TypeRevision;
	Token.bReadable = Field.bReadable;
	Token.bWritable = Field.bWritable;
	Token.bDeclared = Field.bDeclared;
	return Token;
}

bool TryPreparePropertyBatchRows(
	UObject& Asset,
	TConstArrayView<FMatchedPropertyBatchWriteRow> Rows,
	TArray<FPreparedPropertyBatchWriteRow>& OutPrepared,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	OutPrepared.Reset();
	OutPrepared.Reserve(Rows.Num());
	for (const FMatchedPropertyBatchWriteRow& Row : Rows)
	{
		FPreparedPropertyBatchWriteRow& Prepared =
			OutPrepared.AddDefaulted_GetRef();
		Prepared.Token = MakeAdmittedPropertyView(*Row.Field);
		FAssetDocumentEditorPropertyEditResult Failure;
		if (!TryPrepareAssetDocumentEditorAdmittedPropertyWrite(
			Asset, Prepared.Token, *Row.Value, Prepared.Prepared, Failure))
		{
			OutFailure =
				MakePropertyBatchWriteFailure(MoveTemp(Failure.Diagnostics));
			return false;
		}
	}
	return true;
}

bool TryResolveAndPreparePropertyBatchWrite(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	TConstArrayView<FPropertyBatchWriteRowView> Rows,
	TConstArrayView<FString> DeclaredNames,
	TUniquePtr<IAssetDocumentVerifiedHandleLease>& OutAssetLease,
	TUniquePtr<IAssetDocumentVerifiedHandleLease>& OutBatchLease,
	UObject*& OutAsset,
	FAssetDocumentEditorPropertyBatchTokenView& OutBatch,
	TArray<FPreparedPropertyBatchWriteRow>& OutPrepared,
	FAssetDocumentPrimitiveOutcome& OutFailure)
{
	if (!TryAcquireAssetDocumentEditorPropertyBatchWrite(
		Invocation,
		Handles,
		OutAssetLease,
		OutBatchLease,
		OutAsset,
		OutBatch,
		OutFailure))
	{
		return false;
	}
	TArray<FMatchedPropertyBatchWriteRow> Matched;
	if (!TryMatchPropertyBatchRows(
		Rows, OutBatch.Fields, DeclaredNames, Matched, OutFailure))
	{
		return false;
	}
	return TryPreparePropertyBatchRows(
		*OutAsset, Matched, OutPrepared, OutFailure);
}

FAssetDocumentPrimitiveOutcome ApplyPreparedPropertyBatchRows(
	UObject& Asset,
	TArray<FPreparedPropertyBatchWriteRow>& Prepared)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	for (FPreparedPropertyBatchWriteRow& Row : Prepared)
	{
		FAssetDocumentEditorPropertyEditResult Edit =
			ApplyAssetDocumentEditorPreparedPropertyWriteWithoutEnlist(
				Asset,
				Row.Token,
				MoveTemp(Row.Prepared),
				TEXT("editor.property.write_admitted_batch.v1"));
		if (!Edit.bSucceeded)
		{
			return MakePropertyBatchWriteFailure(MoveTemp(Edit.Diagnostics));
		}
		Outcome.ChangedSet.Append(MoveTemp(Edit.ChangedSet));
	}
	return Outcome;
}

FAssetDocumentPrimitiveOutcome CompletePreparedPropertyBatchWrite(
	UObject& Asset,
	TArray<FPreparedPropertyBatchWriteRow>& Prepared,
	IAssetDocumentAtomicTransaction& ActiveTransaction)
{
	if (Prepared.IsEmpty())
	{
		FAssetDocumentPrimitiveOutcome Empty;
		Empty.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
		Empty.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
		return Empty;
	}
	FAssetDocumentContractValidationResult Enlist =
		ActiveTransaction.EnlistFootprint(
			Prepared[0].Prepared.GetFootprint());
	if (!Enlist.bValid)
	{
		return MakePropertyBatchWriteFailure(MoveTemp(Enlist.Diagnostics));
	}
	return ApplyPreparedPropertyBatchRows(Asset, Prepared);
}
} // namespace

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorPropertyWriteAdmittedBatch(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	FAssetDocumentRuntimeValueDto Arguments;
	TArray<FString> DeclaredNames;
	TArray<FPropertyBatchWriteRowView> Rows;
	if (!TrySplitAssetDocumentEditorDeclaredPropertyArguments(
			Invocation.ValueArguments, Arguments, DeclaredNames)
		|| !TryReadPropertyBatchWriteRows(Arguments, Rows))
	{
		return MakePropertyBatchWriteFailure(
			TEXT("PrimitiveInvocation.InvalidValueShape"),
			TEXT("Property batch write requires exact property rows."));
	}
	if (ActiveTransaction == nullptr)
	{
		return MakePropertyBatchWriteFailure(
			TEXT("GenericProvider.InvalidTransaction"),
			TEXT("Property batch write requires the active transaction."));
	}
	TUniquePtr<IAssetDocumentVerifiedHandleLease> AssetLease;
	TUniquePtr<IAssetDocumentVerifiedHandleLease> BatchLease;
	UObject* Asset = nullptr;
	FAssetDocumentEditorPropertyBatchTokenView Batch;
	TArray<FPreparedPropertyBatchWriteRow> Prepared;
	FAssetDocumentPrimitiveOutcome Failure;
	if (!TryResolveAndPreparePropertyBatchWrite(
		Invocation,
		Handles,
		Rows,
		DeclaredNames,
		AssetLease,
		BatchLease,
		Asset,
		Batch,
		Prepared,
		Failure))
	{
		return Failure;
	}
	return CompletePreparedPropertyBatchWrite(
		*Asset, Prepared, *ActiveTransaction);
}
