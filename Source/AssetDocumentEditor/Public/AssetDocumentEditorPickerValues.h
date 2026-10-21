// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

class FProperty;
class UDataTable;
class UScriptStruct;

// Values the editor's pickers cannot produce. The Details panel picks a row handle's RowName from the
// rows of its DataTable and an FKey from the registered keys (EKeys), so only a written value can name a missing row
// ("Bost" for "Boost": Get Data Table Row then takes Row Not Found without a log line) or a key that does not exist
// ("Spacbar": the mapping never fires). Every property writer checks the values it writes with FindInvalidPick
// (edit_level and settings, the generic Properties writer, DataTable cells, struct defaults, Blueprint variable defaults,
// class defaults and component templates), so validate / dry run refuse them before anything changes, naming the close
// names. A row handle with no table or no row name and an empty key (None) are not checked (as before), and a value the
// property already holds is kept (keeping a stored value is not authoring it).
namespace UE::AssetDocument::EditorPickerValues
{
// A row handle whose RowName is not a row of its DataTable.
ASSETDOCUMENTEDITOR_API extern const TCHAR* const RowNotFoundCode;
// An FKey whose name is not a registered key.
ASSETDOCUMENTEDITOR_API extern const TCHAR* const KeyNotFoundCode;

struct FInvalidPick
{
	// RowNotFoundCode or KeyNotFoundCode.
	FString Code;
	// JSON pointer of the value inside the property value ("" = the value itself).
	FString ValuePath;
	// AI-readable reason naming the close names.
	FString Message;
};

// Identities of the picked values (row handles, keys) a value holds, at any depth (struct members, array / set elements,
// map values, static-array slots). Take it before a write that changes the value in place, to pass as Kept.
ASSETDOCUMENTEDITOR_API TArray<FString> CollectPicks(const FProperty& Property, const void* Value);

// The first picked value in Value (as CollectPicks walks it) no picker offers, unless Kept holds it. False when none.
// RowsOf / RowsFrom: a row handle naming table RowsOf is checked against RowsFrom's rows (Validate's trial copy of a
// table stands in for the table its own rows name, rows the same document adds included).
ASSETDOCUMENTEDITOR_API bool FindInvalidPick(const FProperty& Property, const void* Value, TConstArrayView<FString> Kept,
	FInvalidPick& OutInvalid, const UDataTable* RowsOf = nullptr, const UDataTable* RowsFrom = nullptr);

// As above with Kept = the picks of KeptValue (another value of Property, e.g. the current one; may be null).
ASSETDOCUMENTEDITOR_API bool FindInvalidPick(const FProperty& Property, const void* Value, const void* KeptValue, FInvalidPick& OutInvalid,
	const UDataTable* RowsOf = nullptr, const UDataTable* RowsFrom = nullptr);

// As FindInvalidPick for a value of Struct (a struct value without a property, e.g. a new variable's default).
ASSETDOCUMENTEDITOR_API bool FindInvalidPickInStruct(const UScriptStruct& Struct, const void* Value, TConstArrayView<FString> Kept,
	FInvalidPick& OutInvalid);

// The message for Row of Table when the table has no such row ("'Bost' is not a row of DT_Pickups (/Game/...) ...
// Close: Boost."), else empty.
ASSETDOCUMENTEDITOR_API FString DescribeMissingRow(const UDataTable& Table, FName Row);

// The message for a key name no registered key has ("'Spacbar' is not a key ... Close: SpaceBar."), else empty.
ASSETDOCUMENTEDITOR_API FString DescribeMissingKey(FName Key);
}
