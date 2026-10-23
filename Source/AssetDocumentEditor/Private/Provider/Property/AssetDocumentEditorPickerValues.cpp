// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentEditorPickerValues.h"

#include "AssetDocumentNameSuggestions.h"
#include "Engine/DataTable.h"
#include "InputCoreTypes.h"
#include "UObject/UnrealType.h"

namespace AssetDocumentEditorPickerValuesLocal
{
constexpr int32 MaxPickDepth = 16;

// A picked value found by the walk: its identity (for Kept) and, when no picker offers it, the reason.
struct FPick
{
	FString Identity;
	const TCHAR* Code = nullptr;
	FString Message;
};

using FPickVisitor = TFunctionRef<bool(const FPick&, const FString&)>;

// How the walk reads picks: bDescribe adds the reason of an invalid one; a row handle naming RowsOf is checked against
// RowsFrom's rows (Validate's trial copy of a table stands in for the table its own rows name).
struct FPickOptions
{
	bool bDescribe = false;
	const UDataTable* RowsOf = nullptr;
	const UDataTable* RowsFrom = nullptr;
};

FString AppendPickPath(const FString& Path, const FString& Segment)
{
	return Path + TEXT("/") + Segment.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
}

// The map key as a document key: string-like keys as written, others by their export text.
FString PickMapKey(const FProperty& KeyProperty, const void* Key)
{
	if (const FStrProperty* String = CastField<FStrProperty>(&KeyProperty))
	{
		return String->GetPropertyValue(Key);
	}
	if (const FNameProperty* Name = CastField<FNameProperty>(&KeyProperty))
	{
		return Name->GetPropertyValue(Key).ToString();
	}
	FString Text;
	KeyProperty.ExportTextItem_Direct(Text, Key, nullptr, nullptr, PPF_None);
	return Text;
}

// A row handle as a pick; false when it is not checked (no table or no row name).
bool MakeRowPick(const FDataTableRowHandle& Handle, const FPickOptions& Options, FPick& OutPick)
{
	if (Handle.DataTable == nullptr || Handle.RowName.IsNone())
	{
		return false;
	}
	OutPick.Identity = FString::Printf(TEXT("Row:%s:%s"), *Handle.DataTable->GetPathName(), *Handle.RowName.ToString());
	if (Options.bDescribe)
	{
		const UDataTable& Rows = Handle.DataTable == Options.RowsOf && Options.RowsFrom != nullptr ? *Options.RowsFrom : *Handle.DataTable;
		OutPick.Message = Rows.GetRowMap().Contains(Handle.RowName) ? FString()
			: UE::AssetDocument::EditorPickerValues::DescribeMissingRow(*Handle.DataTable, Handle.RowName);
		OutPick.Code = UE::AssetDocument::EditorPickerValues::RowNotFoundCode;
	}
	return true;
}

// A key as a pick; false when it is not checked (None).
bool MakeKeyPick(const FKey& Key, const FPickOptions& Options, FPick& OutPick)
{
	if (Key.GetFName().IsNone())
	{
		return false;
	}
	OutPick.Identity = TEXT("Key:") + Key.GetFName().ToString();
	if (Options.bDescribe)
	{
		OutPick.Message = UE::AssetDocument::EditorPickerValues::DescribeMissingKey(Key.GetFName());
		OutPick.Code = UE::AssetDocument::EditorPickerValues::KeyNotFoundCode;
	}
	return true;
}

// Calls Visit(pick, path) for every picked value of one value of Property (ArrayDim 1) at Value; false stops the walk.
bool VisitPickValue(const FProperty& Property, const void* Value, const FString& Path, int32 Depth, const FPickOptions& Options, FPickVisitor Visit);

bool VisitPickProperty(const FProperty& Property, const void* Value, const FString& Path, int32 Depth, const FPickOptions& Options, FPickVisitor Visit);

bool VisitPickStruct(const UScriptStruct& Struct, const void* Value, const FString& Path, int32 Depth, const FPickOptions& Options, FPickVisitor Visit)
{
	if (Value == nullptr || Depth > MaxPickDepth)
	{
		return true;
	}
	FPick Pick;
	if (&Struct == FDataTableRowHandle::StaticStruct())
	{
		return !MakeRowPick(*static_cast<const FDataTableRowHandle*>(Value), Options, Pick) || Visit(Pick, Path);
	}
	if (&Struct == FKey::StaticStruct())
	{
		return !MakeKeyPick(*static_cast<const FKey*>(Value), Options, Pick) || Visit(Pick, Path);
	}
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		if (!VisitPickProperty(**It, It->ContainerPtrToValuePtr<void>(Value), AppendPickPath(Path, It->GetAuthoredName()),
			Depth + 1, Options, Visit))
		{
			return false;
		}
	}
	return true;
}

bool VisitPickProperty(const FProperty& Property, const void* Value, const FString& Path, int32 Depth, const FPickOptions& Options, FPickVisitor Visit)
{
	if (Property.ArrayDim == 1)
	{
		return VisitPickValue(Property, Value, Path, Depth, Options, Visit);
	}
	for (int32 Index = 0; Index < Property.ArrayDim; ++Index)
	{
		if (!VisitPickValue(Property, static_cast<const uint8*>(Value) + Index * Property.GetElementSize(),
			AppendPickPath(Path, LexToString(Index)), Depth, Options, Visit))
		{
			return false;
		}
	}
	return true;
}

bool VisitPickValue(const FProperty& Property, const void* Value, const FString& Path, int32 Depth, const FPickOptions& Options, FPickVisitor Visit)
{
	if (Value == nullptr || Depth > MaxPickDepth)
	{
		return true;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		return Struct->Struct == nullptr || VisitPickStruct(*Struct->Struct, Value, Path, Depth, Options, Visit);
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		FScriptArrayHelper Helper(Array, Value);
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			if (!VisitPickValue(*Array->Inner, Helper.GetRawPtr(Index), AppendPickPath(Path, LexToString(Index)), Depth + 1, Options, Visit))
			{
				return false;
			}
		}
		return true;
	}
	if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
	{
		FScriptSetHelper Helper(Set, Value);
		int32 Logical = 0;
		for (FScriptSetHelper::FIterator It(Helper); It; ++It, ++Logical)
		{
			if (!VisitPickValue(*Set->ElementProp, Helper.GetElementPtr(It), AppendPickPath(Path, LexToString(Logical)), Depth + 1,
				Options, Visit))
			{
				return false;
			}
		}
		return true;
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
	{
		FScriptMapHelper Helper(Map, Value);
		for (FScriptMapHelper::FIterator It(Helper); It; ++It)
		{
			const FString KeyPath = AppendPickPath(Path, PickMapKey(*Map->KeyProp, Helper.GetKeyPtr(It)));
			if (!VisitPickValue(*Map->KeyProp, Helper.GetKeyPtr(It), KeyPath, Depth + 1, Options, Visit)
				|| !VisitPickValue(*Map->ValueProp, Helper.GetValuePtr(It), KeyPath, Depth + 1, Options, Visit))
			{
				return false;
			}
		}
		return true;
	}
	return true;
}
// Takes Pick as the invalid one unless a picker offers it or Kept holds it; true when taken (the walk stops).
bool TakeInvalidPick(const FPick& Pick, const FString& Path, TConstArrayView<FString> Kept,
	UE::AssetDocument::EditorPickerValues::FInvalidPick& OutInvalid, bool& bOutFound)
{
	if (Pick.Message.IsEmpty() || Kept.Contains(Pick.Identity))
	{
		return false;
	}
	OutInvalid.Code = Pick.Code;
	OutInvalid.ValuePath = Path;
	OutInvalid.Message = Pick.Message;
	bOutFound = true;
	return true;
}
} // namespace AssetDocumentEditorPickerValuesLocal

namespace UE::AssetDocument::EditorPickerValues
{
const TCHAR* const RowNotFoundCode = TEXT("Property.RowNotFound");
const TCHAR* const KeyNotFoundCode = TEXT("Property.KeyNotFound");

TArray<FString> CollectPicks(const FProperty& Property, const void* Value)
{
	using namespace AssetDocumentEditorPickerValuesLocal;
	TArray<FString> Picks;
	VisitPickProperty(Property, Value, FString(), 0, FPickOptions(), [&Picks](const FPick& Pick, const FString&)
		{
			Picks.Add(Pick.Identity);
			return true;
		});
	return Picks;
}

FString DescribeMissingRow(const UDataTable& Table, FName Row)
{
	if (Table.GetRowMap().Contains(Row))
	{
		return FString();
	}
	TArray<FString> Rows;
	for (const FName& Name : Table.GetRowNames())
	{
		Rows.Add(Name.ToString());
	}
	return FString::Printf(TEXT("'%s' is not a row of %s (%s); the Details panel's row picker lists only its rows.%s"),
		*Row.ToString(), *Table.GetName(), *Table.GetPathName(),
		Rows.IsEmpty() ? TEXT(" The table has no rows.") : *UE::AssetDocument::DescribeNameCandidates(Row.ToString(), Rows, TEXT("Rows")));
}

FString DescribeMissingKey(FName Key)
{
	if (Key.IsNone() || FKey(Key).IsValid())
	{
		return FString();
	}
	TArray<FKey> Keys;
	EKeys::GetAllKeys(Keys);
	TArray<FString> Names;
	Names.Reserve(Keys.Num());
	for (const FKey& Candidate : Keys)
	{
		Names.Add(Candidate.GetFName().ToString());
	}
	const TArray<FString> Close = UE::AssetDocument::SuggestCloseNames(Key.ToString(), Names, 5);
	return FString::Printf(TEXT("'%s' is not a key; the key picker lists only registered keys (EKeys).%s"), *Key.ToString(),
		Close.IsEmpty()
			? TEXT(" Key names are EKeys names, e.g. SpaceBar, LeftMouseButton, Gamepad_FaceButton_Bottom.")
			: *FString::Printf(TEXT(" Close: %s."), *FString::Join(Close, TEXT(", "))));
}

bool FindInvalidPick(const FProperty& Property, const void* Value, TConstArrayView<FString> Kept, FInvalidPick& OutInvalid,
	const UDataTable* RowsOf, const UDataTable* RowsFrom)
{
	using namespace AssetDocumentEditorPickerValuesLocal;
	bool bFound = false;
	FPickOptions Options;
	Options.bDescribe = true;
	Options.RowsOf = RowsOf;
	Options.RowsFrom = RowsFrom;
	VisitPickProperty(Property, Value, FString(), 0, Options, [&](const FPick& Pick, const FString& Path)
		{
			return !TakeInvalidPick(Pick, Path, Kept, OutInvalid, bFound);
		});
	return bFound;
}

bool FindInvalidPickInStruct(const UScriptStruct& Struct, const void* Value, TConstArrayView<FString> Kept, FInvalidPick& OutInvalid)
{
	using namespace AssetDocumentEditorPickerValuesLocal;
	bool bFound = false;
	FPickOptions Options;
	Options.bDescribe = true;
	VisitPickStruct(Struct, Value, FString(), 0, Options, [&](const FPick& Pick, const FString& Path)
		{
			return !TakeInvalidPick(Pick, Path, Kept, OutInvalid, bFound);
		});
	return bFound;
}

bool FindInvalidPick(const FProperty& Property, const void* Value, const void* KeptValue, FInvalidPick& OutInvalid,
	const UDataTable* RowsOf, const UDataTable* RowsFrom)
{
	const TArray<FString> Kept = KeptValue != nullptr ? CollectPicks(Property, KeptValue) : TArray<FString>();
	return FindInvalidPick(Property, Value, Kept, OutInvalid, RowsOf, RowsFrom);
}
}
