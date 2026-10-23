// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// data.data_table.read.v1 / data.data_table.edit.v1: rows of a UDataTable (also
// UCompositeDataTable and UMirrorDataTable), editor-equivalent (see AssetDocumentDataPrimitives.cpp).

#include "AssetDocumentDataPrimitives.h"
#include "AssetDocumentDataTableShared.h"
#include "AssetDocumentEditorPickerValues.h"
#include "AssetDocumentEditorTrialScope.h"

#include "DataTableEditorUtils.h"
#include "Engine/CompositeDataTable.h"
#include "Engine/DataTable.h"
#include "Misc/ScopeExit.h"
#include "ScopedTransaction.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "AssetDocumentDataTable"

namespace UE::AssetDocument::Graph::Data
{
namespace
{
UDataTable* GetDataTable(FCall& Call)
{
	UDataTable* Table = Cast<UDataTable>(&Call.Asset);
	if (Table == nullptr)
	{
		Fail(Call, TEXT("Data.UnsupportedAsset"), TEXT("DataTable primitives edit DataTable assets."), TEXT("/HandleArguments/asset"));
	}
	return Table;
}

TArray<FString> ParentPaths(UObject& Composite)
{
	TArray<FString> Paths;
	for (const UObject* Parent : ReadParentTables(Composite))
	{
		Paths.Add(PathOf(Parent));
	}
	return Paths;
}

TArray<FName> RowNames(const UDataTable& Table)
{
	TArray<FName> Names;
	Table.GetRowMap().GenerateKeyArray(Names);
	return Names;
}

FValue EncodeRow(FCall& Call, const UDataTable& Table, FName Name, uint8* Row, bool& bOk)
{
	FObjectBuilder Values;
	for (FProperty* Member : GetRowMembers(*Table.RowStruct))
	{
		if (GetMemberCodecId(*Member).IsEmpty())
		{
			continue;
		}
		FValue Value;
		FString Code;
		if (!EncodeMember(*Member, Row, Value, Code))
		{
			bOk = Fail(Call, TEXT("Data.EncodeFailed"), FString::Printf(TEXT("Row '%s' field '%s' could not be encoded (%s)."),
				*Name.ToString(), *Member->GetAuthoredName(), *Code));
			return MakeNull();
		}
		Values.Add(Member->GetAuthoredName(), MoveTemp(Value));
	}
	FObjectBuilder Out;
	Out.AddString(TEXT("name"), Name.ToString());
	Out.Add(TEXT("values"), Values.Build());
	return Out.Build();
}
} // namespace

bool HandleDataTableRead(FCall& Call, FObjectBuilder& Result)
{
	UDataTable* Table = GetDataTable(Call);
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
	const bool bComposite = GetParentTablesProperty(*Table->GetClass()) != nullptr;
	Result.AddString(TEXT("class"), Table->GetClass()->GetPathName());
	Result.AddString(TEXT("row_struct"), PathOf(Table->RowStruct));
	Result.AddBool(TEXT("row_struct_valid"), Table->RowStruct != nullptr && FDataTableEditorUtils::IsValidTableStruct(Table->RowStruct));
	Result.AddBool(TEXT("composite"), bComposite);
	int32 NodeCount = 16;
	if (Offset == 0)
	{
		TArray<FValue> Fields;
		if (Table->RowStruct != nullptr)
		{
			for (FProperty* Member : GetRowMembers(*Table->RowStruct))
			{
				Fields.Add(DescribeMember(*Member));
			}
		}
		FValue FieldValue = MakeArray(MoveTemp(Fields));
		NodeCount += CountValueNodes(FieldValue);
		Result.Add(TEXT("fields"), MoveTemp(FieldValue));
		if (Table->RowStruct != nullptr)
		{
			// The values of a new row (the row editor's Add: the struct's defaults). A document's row
			// is the complete row: a field it leaves out has this value.
			UScriptStruct& Struct = *Table->RowStruct;
			uint8* const DefaultRow = static_cast<uint8*>(FMemory::Malloc(FMath::Max(Struct.GetStructureSize(), 1), Struct.GetMinAlignment()));
			Struct.InitializeStruct(DefaultRow);
			bool bDefaultsOk = true;
			FValue Defaults = EncodeRow(Call, *Table, NAME_None, DefaultRow, bDefaultsOk);
			Struct.DestroyStruct(DefaultRow);
			FMemory::Free(DefaultRow);
			if (!bDefaultsOk)
			{
				return false;
			}
			NodeCount += CountValueNodes(Defaults);
			Result.Add(TEXT("row_defaults"), MoveTemp(Defaults));
		}
		if (bComposite)
		{
			const TArray<FString> Parents = ParentPaths(*Table);
			NodeCount += Parents.Num() + 1;
			Result.Add(TEXT("parent_tables"), MakeStringArray(Parents));
		}
	}
	TArray<FName> Names = RowNames(*Table);
	TArray<FValue> Rows;
	bool bOk = true;
	int32 NextOffset = -1;
	if (Table->RowStruct != nullptr)
	{
		NextOffset = AppendPaged(Rows, NodeCount, [&](int32 Index)
		{
			return bOk ? EncodeRow(Call, *Table, Names[Index], Table->GetRowMap().FindRef(Names[Index]), bOk) : MakeNull();
		}, static_cast<int32>(Offset), Names.Num(), static_cast<int32>(Limit));
	}
	if (!bOk)
	{
		return false;
	}
	Result.AddInt(TEXT("total"), Names.Num());
	Result.AddInt(TEXT("next_offset"), NextOffset);
	Result.Add(TEXT("rows"), MakeArray(MoveTemp(Rows)));
	return true;
}

namespace
{
bool RequireRow(FCall& Call, FReader& Op, const UDataTable& Table, const TCHAR* Key, FName& OutName, bool bMustExist)
{
	FString Name;
	if (!Op.String(Key, Name))
	{
		return false;
	}
	FString Reason;
	if (!IsValidRowName(Name, Reason))
	{
		return Op.Fail(TEXT("DataTable.InvalidRowName"), FString::Printf(TEXT("'%s' is not a valid row name: %s"), *Name, *Reason), Key);
	}
	OutName = FName(*Name);
	const bool bExists = Table.GetRowMap().Contains(OutName);
	if (bMustExist && !bExists)
	{
		return Op.Fail(TEXT("DataTable.RowNotFound"), FString::Printf(TEXT("The table has no row '%s'."), *Name), Key);
	}
	if (!bMustExist && bExists)
	{
		return Op.Fail(TEXT("DataTable.RowExists"), FString::Printf(TEXT("The table already has a row '%s'."), *Name), Key);
	}
	return true;
}

// The row editor's property edit (SRowEditor::NotifyPreChange / NotifyPostChange) for a set of fields.
// bReplace: the values are the complete row; every other field the codec can write goes back to the
// struct's default (the row editor's per-field "reset to default"); fields it cannot write keep theirs.
bool SetRowValues(FCall& Call, FReader& Op, UDataTable& Table, FName RowName, const FValue& Values, bool bReplace)
{
	if (Values.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), TEXT("values must be an object of field values."), TEXT("values"));
	}
	UScriptStruct& Struct = *Table.RowStruct;
	uint8* Row = Table.GetRowMap().FindRef(RowName);
	if (Row == nullptr)
	{
		return Op.Fail(TEXT("DataTable.RowNotFound"), TEXT("The row has no data."), TEXT("row"));
	}
	uint8* const StagedRow = static_cast<uint8*>(FMemory::Malloc(FMath::Max(Struct.GetStructureSize(), 1), Struct.GetMinAlignment()));
	Struct.InitializeStruct(StagedRow);
	Struct.CopyScriptStruct(StagedRow, Row);
	ON_SCOPE_EXIT { Struct.DestroyStruct(StagedRow); FMemory::Free(StagedRow); };
	if (bReplace)
	{
		uint8* const DefaultRow = static_cast<uint8*>(FMemory::Malloc(FMath::Max(Struct.GetStructureSize(), 1), Struct.GetMinAlignment()));
		Struct.InitializeStruct(DefaultRow);
		for (FProperty* Member : GetRowMembers(Struct))
		{
			if (!GetMemberCodecId(*Member).IsEmpty())
			{
				Member->CopyCompleteValue_InContainer(StagedRow, DefaultRow);
			}
		}
		Struct.DestroyStruct(DefaultRow);
		FMemory::Free(DefaultRow);
	}
	const FString ValuesPath = Op.FieldPath(TEXT("values"));
	for (int32 Index = 0; Index < Values.ObjectKeys.Num(); ++Index)
	{
		const FString& Key = Values.ObjectKeys[Index];
		const FString Path = ValuesPath + TEXT("/") + Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		FProperty* Member = FindRowMember(Struct, Key);
		if (Member == nullptr)
		{
			TArray<FString> Names;
			for (FProperty* Candidate : GetRowMembers(Struct))
			{
				Names.Add(Candidate->GetAuthoredName());
			}
			const TArray<FString> Suggestions = SuggestActionKeys(Key, Names, 3);
			return Fail(Call, TEXT("DataTable.UnknownField"), FString::Printf(TEXT("%s has no field '%s'.%s"), *Struct.GetName(), *Key,
				Suggestions.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" Did you mean: %s?"), *FString::Join(Suggestions, TEXT(", ")))), Path);
		}
		if (GetMemberCodecId(*Member).IsEmpty())
		{
			return Fail(Call, TEXT("DataTable.UnsupportedField"), FString::Printf(TEXT("Field '%s' (%s) cannot be authored yet; omit it to keep its value."),
				*Key, *Member->GetCPPType()), Path);
		}
		FString Code;
		if (!DecodeMember(*Member, Values.Elements[Index], StagedRow, Code, &Table))
		{
			return Fail(Call, TEXT("DataTable.InvalidValue"), FString::Printf(TEXT("The value of '%s' does not fit %s (%s)."),
				*Key, *Member->GetCPPType(), *Code), Path);
		}
		// A cell naming a table row or a key that does not exist (the row editor's pickers list only existing ones).
		EditorPickerValues::FInvalidPick Invalid;
		// Validate's trial copy stands in for the table its rows name (rows this document adds included).
		if (EditorPickerValues::FindInvalidPick(*Member, Member->ContainerPtrToValuePtr<void>(StagedRow),
			Member->ContainerPtrToValuePtr<void>(Row), Invalid,
			Cast<UDataTable>(FAssetDocumentEditorTrialScope::FindTrialSource(Table)), &Table))
		{
			return Fail(Call, Invalid.Code, FString::Printf(TEXT("Row '%s', field '%s'%s: %s"),
				*RowName.ToString(), *Key, *Invalid.ValuePath, *Invalid.Message), Path + Invalid.ValuePath);
		}
	}
	if (Struct.CompareScriptStruct(Row, StagedRow, PPF_None))
	{
		return true;
	}
	const FScopedTransaction Transaction(LOCTEXT("EditRow", "Edit Data Table Row"));
	Table.Modify();
	FDataTableEditorUtils::BroadcastPreChange(&Table, FDataTableEditorUtils::EDataTableChangeInfo::RowData);
	Struct.CopyScriptStruct(Row, StagedRow);
	Table.HandleDataTableChanged(RowName);
	Table.MarkPackageDirty();
	FDataTableEditorUtils::BroadcastPostChange(&Table, FDataTableEditorUtils::EDataTableChangeInfo::RowData);
	return true;
}

// The Details panel's ParentTables edit on a composite table.
bool SetParentTables(FCall& Call, FReader& Op, UDataTable& Composite, UClass& ExpectedParentClass)
{
	const FValue* Tables = nullptr;
	if (!Op.Array(TEXT("tables"), Tables))
	{
		return false;
	}
	FArrayProperty* Property = GetParentTablesProperty(*Composite.GetClass());
	if (Property == nullptr)
	{
		return Op.Fail(TEXT("DataTable.NotComposite"), TEXT("Only composite tables have ParentTables."), TEXT("op"));
	}
	TArray<UObject*> Parents;
	for (int32 Index = 0; Index < Tables->Elements.Num(); ++Index)
	{
		const FValue& Entry = Tables->Elements[Index];
		const FString Path = Op.FieldPath(TEXT("tables")) + FString::Printf(TEXT("/%d"), Index);
		if (Entry.Kind != EAssetDocumentRuntimeValueKind::String)
		{
			return Fail(Call, TEXT("Graph.InvalidArgument"), TEXT("A parent table is an object path string."), Path);
		}
		UObject* Parent = Entry.StringValue.IsEmpty() ? nullptr : FSoftObjectPath(Entry.StringValue).TryLoad();
		if (Parent == nullptr || !Parent->IsA(&ExpectedParentClass))
		{
			return Fail(Call, TEXT("Composite.ParentNotFound"), FString::Printf(TEXT("'%s' is not a %s asset."),
				*Entry.StringValue, *ExpectedParentClass.GetName()), Path);
		}
		if (const UDataTable* ParentTable = Cast<UDataTable>(Parent))
		{
			if (ParentTable->RowStruct != Composite.RowStruct)
			{
				return Fail(Call, TEXT("Composite.ParentRowStructMismatch"), FString::Printf(
					TEXT("'%s' has row struct %s; a composite table's parents must use %s."), *Entry.StringValue,
					*PathOf(ParentTable->RowStruct), *PathOf(Composite.RowStruct)), Path);
			}
		}
		TSet<const UObject*> Seen;
		if (ReachesSelf(Parent, Composite, Seen))
		{
			return Fail(Call, TEXT("Composite.ParentCycle"), FString::Printf(TEXT("'%s' depends on this table (cyclic dependency)."),
				*Entry.StringValue), Path);
		}
		Parents.Add(Parent);
	}
	if (ReadParentTables(Composite) == Parents)
	{
		return true;
	}
	return WriteParentTables(Op, Composite, *Property, Parents);
}

// The row order through FDataTableEditorUtils::MoveRow (the editor's row move). {rows} lists
// every row once; {rows, at} places a segment of rows at positions at.. in that order (rows placed
// earlier stay in front), so a long order is sent as consecutive segments that each stay within
// the per-call budget.
bool ApplyOrder(FCall& Call, FReader& Op, UDataTable& Table)
{
	const FValue* Rows = nullptr;
	int64 At = MIN_int64;
	if (!Op.Array(TEXT("rows"), Rows) || !Op.Int(TEXT("at"), At, false))
	{
		return false;
	}
	const TArray<FName> Current = RowNames(Table);
	const bool bSegment = At != MIN_int64;
	if (bSegment && (At < 0 || At > Current.Num()))
	{
		return Op.Fail(TEXT("DataTable.InvalidOrder"), TEXT("at must be a row position of the table."), TEXT("at"));
	}
	const int32 Start = bSegment ? static_cast<int32>(At) : 0;
	TArray<FName> Wanted;
	TSet<FName> WantedSet;
	for (const FValue& Entry : Rows->Elements)
	{
		if (Entry.Kind != EAssetDocumentRuntimeValueKind::String)
		{
			return Op.Fail(TEXT("DataTable.InvalidOrder"), TEXT("order rows are row names."), TEXT("rows"));
		}
		const FName Name(*Entry.StringValue);
		bool bDuplicate = false;
		WantedSet.Add(Name, &bDuplicate);
		if (bDuplicate || !Table.GetRowMap().Contains(Name))
		{
			return Op.Fail(TEXT("DataTable.InvalidOrder"), FString::Printf(TEXT("order lists '%s' %s."), *Entry.StringValue,
				bDuplicate ? TEXT("twice") : TEXT("but the table has no such row")), TEXT("rows"));
		}
		Wanted.Add(Name);
	}
	if (!bSegment ? Wanted.Num() != Current.Num() : Start + Wanted.Num() > Current.Num())
	{
		return Op.Fail(TEXT("DataTable.InvalidOrder"), bSegment
			? TEXT("An order segment must fit within the table's rows.")
			: TEXT("order must list every row of the table exactly once."), TEXT("rows"));
	}
	for (int32 Index = 0; Index < Start; ++Index)
	{
		if (WantedSet.Contains(Current[Index]))
		{
			return Op.Fail(TEXT("DataTable.InvalidOrder"), FString::Printf(TEXT("Row '%s' is already placed before position %d."),
				*Current[Index].ToString(), Start), TEXT("rows"));
		}
	}
	// Only rows out of place move (one MoveRow each); an order that already holds costs nothing.
	TArray<FName> Order = Current;
	for (int32 Index = 0; Index < Wanted.Num(); ++Index)
	{
		const int32 Target = Start + Index;
		if (Order[Target] == Wanted[Index])
		{
			continue;
		}
		const int32 From = Order.IndexOfByKey(Wanted[Index]);
		if (From < Target || !FDataTableEditorUtils::MoveRow(&Table, Wanted[Index], FDataTableEditorUtils::ERowMoveDirection::Up, From - Target))
		{
			return Op.Fail(TEXT("DataTable.InvalidOrder"), FString::Printf(TEXT("Row '%s' could not be moved."), *Wanted[Index].ToString()), TEXT("rows"));
		}
		Order.RemoveAt(From, EAllowShrinking::No);
		Order.Insert(Wanted[Index], Target);
	}
	return true;
}
} // namespace

bool HandleDataTableEdit(FCall& Call, FObjectBuilder& Result)
{
	UDataTable* Table = GetDataTable(Call);
	FString RowStructPath;
	const FValue* Ops = nullptr;
	if (Table == nullptr || !Call.Args.OnlyFields({TEXT("row_struct"), TEXT("ops")})
		|| !Call.Args.String(TEXT("row_struct"), RowStructPath) || !Call.Args.Array(TEXT("ops"), Ops))
	{
		return false;
	}
	if (Table->RowStruct == nullptr)
	{
		return Call.Args.Fail(TEXT("DataTable.RowStructMissing"), TEXT("The table has no row struct (it may have been deleted)."), TEXT("row_struct"));
	}
	if (PathOf(Table->RowStruct) != RowStructPath)
	{
		return Call.Args.Fail(TEXT("DataTable.RowStructImmutable"), FString::Printf(
			TEXT("The table's row struct is %s; the editor offers no row struct change on an existing table, so the document must state that struct."),
			*PathOf(Table->RowStruct)), TEXT("row_struct"));
	}
	if (!FDataTableEditorUtils::IsValidTableStruct(Table->RowStruct))
	{
		return Call.Args.Fail(TEXT("DataTable.InvalidRowStruct"), FString::Printf(
			TEXT("%s cannot be a data table row struct (it must derive from FTableRowBase or be a User Defined Struct)."), *RowStructPath),
			TEXT("row_struct"));
	}
	// One editor transaction for the whole call (the per-edit transactions nest into it); the
	// table itself is not recorded (FTableUndoRecordingSuppression).
	const FTableUndoRecordingSuppression NoUndoRecording(*Table);
	const FScopedTransaction CallTransaction(LOCTEXT("EditDataTable", "Edit Data Table"));
	const bool bComposite = GetParentTablesProperty(*Table->GetClass()) != nullptr;
	for (int32 Index = 0; Index < Ops->Elements.Num(); ++Index)
	{
		FReader Op = Call.Args.Child(FString::Printf(TEXT("ops/%d"), Index), Ops->Elements[Index]);
		FString Kind;
		if (!Op.IsObject() || !Op.String(TEXT("op"), Kind))
		{
			return Op.IsObject() ? false : Op.Fail(TEXT("Data.InvalidOp"), TEXT("Each op is an object."));
		}
		if (Kind == TEXT("parent_tables"))
		{
			if (!Op.OnlyFields({TEXT("op"), TEXT("tables")}) || !SetParentTables(Call, Op, *Table, *UDataTable::StaticClass()))
			{
				return false;
			}
			continue;
		}
		if (bComposite)
		{
			return Op.Fail(TEXT("DataTable.CompositeRowsReadOnly"),
				TEXT("A composite table's rows come from its parent tables; edit the parents instead."), TEXT("op"));
		}
		FName Row;
		if (Kind == TEXT("remove"))
		{
			if (!Op.OnlyFields({TEXT("op"), TEXT("row")}) || !RequireRow(Call, Op, *Table, TEXT("row"), Row, true))
			{
				return false;
			}
			FDataTableEditorUtils::RemoveRow(Table, Row);
		}
		else if (Kind == TEXT("add"))
		{
			if (!Op.OnlyFields({TEXT("op"), TEXT("row")}) || !RequireRow(Call, Op, *Table, TEXT("row"), Row, false))
			{
				return false;
			}
			if (FDataTableEditorUtils::AddRow(Table, Row) == nullptr)
			{
				return Op.Fail(TEXT("DataTable.InvalidRowName"), TEXT("The row could not be added."), TEXT("row"));
			}
		}
		else if (Kind == TEXT("rename"))
		{
			FName To;
			if (!Op.OnlyFields({TEXT("op"), TEXT("row"), TEXT("to")}) || !RequireRow(Call, Op, *Table, TEXT("row"), Row, true)
				|| !RequireRow(Call, Op, *Table, TEXT("to"), To, false))
			{
				return false;
			}
			if (!FDataTableEditorUtils::RenameRow(Table, Row, To))
			{
				return Op.Fail(TEXT("DataTable.InvalidRowName"), TEXT("The row could not be renamed."), TEXT("to"));
			}
		}
		else if (Kind == TEXT("set"))
		{
			const FValue* Values = nullptr;
			bool bReplace = false;
			if (!Op.OnlyFields({TEXT("op"), TEXT("row"), TEXT("values"), TEXT("replace")}) || !RequireRow(Call, Op, *Table, TEXT("row"), Row, true)
				|| !Op.Object(TEXT("values"), Values) || !Op.Bool(TEXT("replace"), bReplace)
				|| !SetRowValues(Call, Op, *Table, Row, *Values, bReplace))
			{
				return false;
			}
		}
		else if (Kind == TEXT("order"))
		{
			if (!Op.OnlyFields({TEXT("op"), TEXT("rows"), TEXT("at")}) || !ApplyOrder(Call, Op, *Table))
			{
				return false;
			}
		}
		else
		{
			return Op.Fail(TEXT("Data.InvalidOp"), FString::Printf(
				TEXT("Unknown op '%s' (remove, add, rename, set, order, parent_tables)."), *Kind), TEXT("op"));
		}
	}
	Result.AddInt(TEXT("rows"), Table->GetRowMap().Num());
	return true;
}
} // namespace UE::AssetDocument::Graph::Data

#undef LOCTEXT_NAMESPACE
