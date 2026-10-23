// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// data.curve_table.read.v1 / data.curve_table.edit.v1: named curves of a UCurveTable (also
// UCompositeCurveTable), as the Curve Table editor edits them (see AssetDocumentDataPrimitives.cpp).
// A curve is the reflected FRichCurve / FSimpleCurve encoded member by member with the shared
// typed value codec: the same native form the generic route uses for UCurveFloat::FloatCurve.

#include "AssetDocumentDataPrimitives.h"
#include "AssetDocumentDataTableShared.h"

#include "Curves/RichCurve.h"
#include "Curves/SimpleCurve.h"
#include "Engine/CompositeCurveTable.h"
#include "Engine/CurveTable.h"
#include "ScopedTransaction.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "AssetDocumentCurveTable"

namespace UE::AssetDocument::Graph::Data
{
namespace
{
UCurveTable* GetCurveTable(FCall& Call)
{
	UCurveTable* Table = Cast<UCurveTable>(&Call.Asset);
	if (Table == nullptr)
	{
		Fail(Call, TEXT("Data.UnsupportedAsset"), TEXT("CurveTable primitives edit CurveTable assets."), TEXT("/HandleArguments/asset"));
	}
	return Table;
}

const TCHAR* ModeName(ECurveTableMode Mode)
{
	switch (Mode)
	{
	case ECurveTableMode::SimpleCurves: return TEXT("SimpleCurves");
	case ECurveTableMode::RichCurves: return TEXT("RichCurves");
	default: return TEXT("Empty");
	}
}

UScriptStruct* CurveStruct(ECurveTableMode Mode)
{
	return Mode == ECurveTableMode::RichCurves ? FRichCurve::StaticStruct() : FSimpleCurve::StaticStruct();
}

bool EncodeCurve(FCall& Call, ECurveTableMode Mode, FName Name, const FRealCurve& Curve, FValue& Out)
{
	FObjectBuilder Fields;
	for (FProperty* Member : GetRowMembers(*CurveStruct(Mode)))
	{
		FValue Value;
		FString Code;
		if (!EncodeMember(*Member, &Curve, Value, Code))
		{
			return Fail(Call, TEXT("Data.EncodeFailed"), FString::Printf(TEXT("Curve '%s' field '%s' could not be encoded (%s)."),
				*Name.ToString(), *Member->GetAuthoredName(), *Code));
		}
		Fields.Add(Member->GetAuthoredName(), MoveTemp(Value));
	}
	Out = Fields.Build();
	return true;
}

TArray<FName> CurveNames(const UCurveTable& Table)
{
	TArray<FName> Names;
	Table.GetRowMap().GenerateKeyArray(Names);
	return Names;
}

} // namespace

bool HandleCurveTableRead(FCall& Call, FObjectBuilder& Result)
{
	UCurveTable* Table = GetCurveTable(Call);
	int64 Offset = 0;
	int64 Limit = DefaultRowLimit;
	if (Table == nullptr || !Call.Args.OnlyFields({TEXT("offset"), TEXT("limit")})
		|| !Call.Args.Int(TEXT("offset"), Offset, false) || !Call.Args.Int(TEXT("limit"), Limit, false))
	{
		return false;
	}
	if (Offset < 0 || Limit < 1)
	{
		return Call.Args.Fail(TEXT("Graph.InvalidArgument"), TEXT("offset must be >= 0 and limit >= 1."), TEXT("offset"));
	}
	const ECurveTableMode Mode = Table->GetCurveTableMode();
	const bool bComposite = GetParentTablesProperty(*Table->GetClass()) != nullptr;
	Result.AddString(TEXT("class"), Table->GetClass()->GetPathName());
	Result.AddString(TEXT("mode"), ModeName(Mode));
	Result.AddBool(TEXT("composite"), bComposite);
	int32 NodeCount = 16;
	if (Offset == 0 && bComposite)
	{
		TArray<FString> Parents;
		for (const UObject* Parent : ReadParentTables(*Table))
		{
			Parents.Add(PathOf(Parent));
		}
		NodeCount += Parents.Num() + 1;
		Result.Add(TEXT("parent_tables"), MakeStringArray(Parents));
	}
	const TArray<FName> Names = CurveNames(*Table);
	TArray<FValue> Rows;
	bool bOk = true;
	const int32 NextOffset = AppendPaged(Rows, NodeCount, [&](int32 Index)
	{
		FValue Curve;
		const FRealCurve* Real = Table->GetRowMap().FindRef(Names[Index]);
		bOk = bOk && Real != nullptr && EncodeCurve(Call, Mode, Names[Index], *Real, Curve);
		FObjectBuilder Row;
		Row.AddString(TEXT("name"), Names[Index].ToString());
		Row.Add(TEXT("curve"), MoveTemp(Curve));
		return Row.Build();
	}, static_cast<int32>(Offset), Names.Num(), static_cast<int32>(Limit));
	if (!bOk)
	{
		return Fail(Call, TEXT("Data.EncodeFailed"), TEXT("A curve could not be encoded."));
	}
	Result.AddInt(TEXT("total"), Names.Num());
	Result.AddInt(TEXT("next_offset"), NextOffset);
	Result.Add(TEXT("rows"), MakeArray(MoveTemp(Rows)));
	return true;
}

namespace
{
bool ReadCurveName(FReader& Op, const UCurveTable& Table, const TCHAR* Key, FName& OutName, bool bMustExist)
{
	FString Name;
	if (!Op.String(Key, Name))
	{
		return false;
	}
	FString Reason;
	if (!IsValidRowName(Name, Reason))
	{
		return Op.Fail(TEXT("CurveTable.InvalidRowName"), FString::Printf(TEXT("'%s' is not a valid curve name: %s"), *Name, *Reason), Key);
	}
	OutName = FName(*Name);
	const bool bExists = Table.GetRowMap().Contains(OutName);
	if (bMustExist && !bExists)
	{
		return Op.Fail(TEXT("CurveTable.RowNotFound"), FString::Printf(TEXT("The table has no curve '%s'."), *Name), Key);
	}
	if (!bMustExist && bExists)
	{
		return Op.Fail(TEXT("CurveTable.RowExists"), FString::Printf(TEXT("The table already has a curve '%s'."), *Name), Key);
	}
	return true;
}

void NotifyChanged(UCurveTable& Table)
{
	FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
	Table.PostEditChangeProperty(Event);
}

// Decodes a whole curve (every reflected member) into Staged.
bool DecodeCurve(FCall& Call, FReader& Op, UScriptStruct& Struct, const FValue& Curve, void* Staged)
{
	const FString CurvePath = Op.FieldPath(TEXT("curve"));
	const TArray<FProperty*> Members = GetRowMembers(Struct);
	if (Curve.Kind != EAssetDocumentRuntimeValueKind::Object || Curve.ObjectKeys.Num() != Members.Num())
	{
		TArray<FString> Names;
		for (FProperty* Member : Members)
		{
			Names.Add(Member->GetAuthoredName());
		}
		return Fail(Call, TEXT("CurveTable.InvalidCurve"), FString::Printf(TEXT("A %s curve is an object with exactly %s."),
			*Struct.GetName(), *FString::Join(Names, TEXT(", "))), CurvePath);
	}
	for (int32 Index = 0; Index < Curve.ObjectKeys.Num(); ++Index)
	{
		const FString Path = CurvePath + TEXT("/") + Curve.ObjectKeys[Index];
		FProperty* Member = FindRowMember(Struct, Curve.ObjectKeys[Index]);
		FString Code;
		if (Member == nullptr)
		{
			return Fail(Call, TEXT("CurveTable.InvalidCurve"), FString::Printf(TEXT("%s has no field '%s'."), *Struct.GetName(), *Curve.ObjectKeys[Index]), Path);
		}
		if (!DecodeMember(*Member, Curve.Elements[Index], Staged, Code))
		{
			return Fail(Call, TEXT("CurveTable.InvalidCurve"), FString::Printf(TEXT("The value of '%s' does not fit %s (%s)."),
				*Curve.ObjectKeys[Index], *Member->GetCPPType(), *Code), Path);
		}
	}
	return true;
}

bool SetCurve(FCall& Call, FReader& Op, UCurveTable& Table, FName Name, const FString& ModeText, const FValue& Curve)
{
	if (ModeText != TEXT("SimpleCurves") && ModeText != TEXT("RichCurves"))
	{
		return Op.Fail(TEXT("CurveTable.InvalidMode"), TEXT("mode is SimpleCurves or RichCurves."), TEXT("mode"));
	}
	const ECurveTableMode Mode = ModeText == TEXT("RichCurves") ? ECurveTableMode::RichCurves : ECurveTableMode::SimpleCurves;
	if (Table.GetCurveTableMode() != ECurveTableMode::Empty && Table.GetCurveTableMode() != Mode)
	{
		return Op.Fail(TEXT("CurveTable.ModeMismatch"), FString::Printf(
			TEXT("The table holds %s; changing the curve type needs a reset op first."), ModeName(Table.GetCurveTableMode())), TEXT("mode"));
	}
	const bool bExists = Table.GetRowMap().Contains(Name);
	if (Mode == ECurveTableMode::RichCurves)
	{
		FRichCurve Staged;
		if (bExists)
		{
			Staged = *static_cast<FRichCurve*>(Table.GetRowMap().FindRef(Name));
		}
		if (!DecodeCurve(Call, Op, *FRichCurve::StaticStruct(), Curve, &Staged))
		{
			return false;
		}
		FRichCurve* Existing = bExists ? static_cast<FRichCurve*>(Table.GetRowMap().FindRef(Name)) : nullptr;
		if (Existing != nullptr && FRichCurve::StaticStruct()->CompareScriptStruct(Existing, &Staged, PPF_None))
		{
			return true;
		}
		const FScopedTransaction Transaction(LOCTEXT("SetCurve", "Set Curve"));
		Table.Modify();
		FRichCurve& Target = Existing != nullptr ? *Existing : Table.AddRichCurve(Name);
		Target = Staged;
		NotifyChanged(Table);
		return true;
	}
	FSimpleCurve Staged;
	if (bExists)
	{
		Staged = *static_cast<FSimpleCurve*>(Table.GetRowMap().FindRef(Name));
	}
	if (!DecodeCurve(Call, Op, *FSimpleCurve::StaticStruct(), Curve, &Staged))
	{
		return false;
	}
	FSimpleCurve* Existing = bExists ? static_cast<FSimpleCurve*>(Table.GetRowMap().FindRef(Name)) : nullptr;
	if (Existing != nullptr && FSimpleCurve::StaticStruct()->CompareScriptStruct(Existing, &Staged, PPF_None))
	{
		return true;
	}
	const FScopedTransaction Transaction(LOCTEXT("SetCurve", "Set Curve"));
	Table.Modify();
	FSimpleCurve& Target = Existing != nullptr ? *Existing : Table.AddSimpleCurve(Name);
	Target = Staged;
	NotifyChanged(Table);
	return true;
}

bool SetCurveParentTables(FCall& Call, FReader& Op, UCurveTable& Composite)
{
	const FValue* Tables = nullptr;
	if (!Op.Array(TEXT("tables"), Tables))
	{
		return false;
	}
	FArrayProperty* Property = GetParentTablesProperty(*Composite.GetClass());
	if (Property == nullptr)
	{
		return Op.Fail(TEXT("DataTable.NotComposite"), TEXT("Only composite curve tables have ParentTables."), TEXT("op"));
	}
	TArray<UObject*> Parents;
	for (int32 Index = 0; Index < Tables->Elements.Num(); ++Index)
	{
		const FValue& Entry = Tables->Elements[Index];
		const FString Path = Op.FieldPath(TEXT("tables")) + FString::Printf(TEXT("/%d"), Index);
		UObject* Parent = Entry.Kind == EAssetDocumentRuntimeValueKind::String && !Entry.StringValue.IsEmpty()
			? FSoftObjectPath(Entry.StringValue).TryLoad() : nullptr;
		if (Parent == nullptr || !Parent->IsA<UCurveTable>())
		{
			return Fail(Call, TEXT("Composite.ParentNotFound"), FString::Printf(TEXT("'%s' is not a CurveTable asset."), *Entry.StringValue), Path);
		}
		TSet<const UObject*> Seen;
		if (ReachesSelf(Parent, Composite, Seen))
		{
			return Fail(Call, TEXT("Composite.ParentCycle"), FString::Printf(TEXT("'%s' depends on this table (cyclic dependency)."), *Entry.StringValue), Path);
		}
		Parents.Add(Parent);
	}
	if (ReadParentTables(Composite) == Parents)
	{
		return true;
	}
	return WriteParentTables(Op, Composite, *Property, Parents);
}
} // namespace

bool HandleCurveTableEdit(FCall& Call, FObjectBuilder& Result)
{
	UCurveTable* Table = GetCurveTable(Call);
	const FValue* Ops = nullptr;
	if (Table == nullptr || !Call.Args.OnlyFields({TEXT("ops")}) || !Call.Args.Array(TEXT("ops"), Ops))
	{
		return false;
	}
	// One editor transaction for the whole call (the per-edit transactions nest into it); the
	// table itself is not recorded (FTableUndoRecordingSuppression).
	const FTableUndoRecordingSuppression NoUndoRecording(*Table);
	const FScopedTransaction CallTransaction(LOCTEXT("EditCurveTable", "Edit Curve Table"));
	const bool bComposite = GetParentTablesProperty(*Table->GetClass()) != nullptr;
	for (int32 Index = 0; Index < Ops->Elements.Num(); ++Index)
	{
		FReader Op = Call.Args.Child(FString::Printf(TEXT("ops/%d"), Index), Ops->Elements[Index]);
		FString Kind;
		if (!Op.IsObject())
		{
			return Op.Fail(TEXT("Data.InvalidOp"), TEXT("Each op is an object."));
		}
		if (!Op.String(TEXT("op"), Kind))
		{
			return false;
		}
		if (Kind == TEXT("parent_tables"))
		{
			if (!Op.OnlyFields({TEXT("op"), TEXT("tables")}) || !SetCurveParentTables(Call, Op, *Table))
			{
				return false;
			}
			continue;
		}
		if (bComposite)
		{
			return Op.Fail(TEXT("CurveTable.CompositeRowsReadOnly"),
				TEXT("A composite curve table's curves come from its parent tables; edit the parents instead."), TEXT("op"));
		}
		FName Name;
		if (Kind == TEXT("reset"))
		{
			FString Mode;
			if (!Op.OnlyFields({TEXT("op"), TEXT("mode")}) || !Op.String(TEXT("mode"), Mode))
			{
				return false;
			}
			if (Mode != TEXT("SimpleCurves") && Mode != TEXT("RichCurves"))
			{
				return Op.Fail(TEXT("CurveTable.InvalidMode"), TEXT("mode is SimpleCurves or RichCurves."), TEXT("mode"));
			}
			// As a reimport does (UCurveTable::CreateTableFromCSVString): empty the table; the next
			// set op adds curves of the new mode.
			const FScopedTransaction Transaction(LOCTEXT("ResetCurves", "Reset Curve Table"));
			Table->Modify();
			Table->EmptyTable();
			NotifyChanged(*Table);
		}
		else if (Kind == TEXT("remove"))
		{
			if (!Op.OnlyFields({TEXT("op"), TEXT("row")}) || !ReadCurveName(Op, *Table, TEXT("row"), Name, true))
			{
				return false;
			}
			const FScopedTransaction Transaction(LOCTEXT("DeleteCurve", "Delete Curve Rows"));
			Table->Modify();
			Table->DeleteRow(Name);
			NotifyChanged(*Table);
		}
		else if (Kind == TEXT("rename"))
		{
			FName To;
			if (!Op.OnlyFields({TEXT("op"), TEXT("row"), TEXT("to")}) || !ReadCurveName(Op, *Table, TEXT("row"), Name, true)
				|| !ReadCurveName(Op, *Table, TEXT("to"), To, false))
			{
				return false;
			}
			const FScopedTransaction Transaction(LOCTEXT("RenameCurve", "Rename Curve"));
			Table->Modify();
			Table->RenameRow(Name, To);
			NotifyChanged(*Table);
		}
		else if (Kind == TEXT("set"))
		{
			const FValue* Curve = nullptr;
			FString RowName;
			FString Mode;
			FString Reason;
			if (!Op.OnlyFields({TEXT("op"), TEXT("row"), TEXT("mode"), TEXT("curve")}) || !Op.String(TEXT("row"), RowName)
				|| !Op.String(TEXT("mode"), Mode) || !Op.Object(TEXT("curve"), Curve))
			{
				return false;
			}
			if (!IsValidRowName(RowName, Reason))
			{
				return Op.Fail(TEXT("CurveTable.InvalidRowName"), FString::Printf(TEXT("'%s' is not a valid curve name: %s"), *RowName, *Reason), TEXT("row"));
			}
			if (!SetCurve(Call, Op, *Table, FName(*RowName), Mode, *Curve))
			{
				return false;
			}
		}
		else
		{
			return Op.Fail(TEXT("Data.InvalidOp"), FString::Printf(
				TEXT("Unknown op '%s' (reset, remove, rename, set, parent_tables)."), *Kind), TEXT("op"));
		}
	}
	Result.AddString(TEXT("mode"), ModeName(Table->GetCurveTableMode()));
	Result.AddInt(TEXT("rows"), Table->GetRowMap().Num());
	return true;
}
} // namespace UE::AssetDocument::Graph::Data

#undef LOCTEXT_NAMESPACE
