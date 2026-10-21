// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentGraphPrimitives.h"
#include "UObject/Object.h"

class UObject;
class FArrayProperty;
class FProperty;
class UScriptStruct;

// Data table family primitives (canonical provider "data", owner Data). Same conventions
// as the graph and widget providers: one "asset" handle, value DTOs with schema_version 1,
// whole-package footprint, no typed inverse (Core restores the package snapshot on
// rollback). Each handler does what the corresponding editor does for the same edit
// (DataTable editor / FDataTableEditorUtils, Curve Table editor, String Table editor,
// Details panel for ParentTables).
namespace UE::AssetDocument::Graph::Data
{
inline const TCHAR* ProviderId = TEXT("data");

TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetDescriptors();
TConstArrayView<FBinding> GetBindings();

// ---- shared by the data primitive files ---------------------------------------

bool Fail(FCall& Call, FStringView Code, FStringView Message, FStringView Path = TEXTVIEW("/"));

// Shortest decimal that reads back as the same float32 (stored floats extract as 0.1, not
// 0.10000000149011612), like the Python profiles' float32 rendering.
double ShortestFloat(float Value);

// Members of a row struct the document addresses (not transient / deprecated), keyed by
// their authored name (user-defined structs: the name shown in the editor).
TArray<FProperty*> GetRowMembers(const UScriptStruct& Struct);
FProperty* FindRowMember(const UScriptStruct& Struct, FStringView AuthoredName);

// Codec id of a member the shared Editor typed value codec represents; empty otherwise.
FString GetMemberCodecId(FProperty& Member);

// One member value <-> runtime value through the shared Editor typed value codec;
// float32 values are rendered shortest and integral numbers decode into float members.
bool EncodeMember(FProperty& Member, const void* ContainerPtr, FValue& OutValue, FString& OutCode);
// Decodes Value as an edit of the member value already in ContainerPtr (omitted
// struct members keep theirs); Owner is the table (text ids, picker filters).
bool DecodeMember(FProperty& Member, const FValue& Value, void* ContainerPtr, FString& OutCode, UObject* Owner = nullptr);

// Schema row of a member: {name, type, codec, supported, enum_values?}.
FValue DescribeMember(FProperty& Member);

// Path of an object; "" for null.
FString PathOf(const UObject* Object);

// Row / curve / key names the editors accept (FName::IsValidXName with INVALID_NAME_CHARACTERS).
bool IsValidRowName(const FString& Name, FString& OutReason);

// Paging over a node budget: appends Rows[Offset..] while the result stays under the
// budget; returns the next offset or -1.
int32 AppendPaged(TArray<FValue>& Out, int32& NodeCount, TFunctionRef<FValue(int32)> MakeRow, int32 Offset, int32 Total, int32 Limit);

// An AI apply is not part of the editor's undo history: Core restores a
// failed apply from its package snapshot, and a successful one clears history that refers to the
// asset. The editor operations a table edit runs still open their transactions (they nest into the
// call's transaction), but the table is not recorded while this scope lives. Recording it
// serialized the whole table twice per primitive call (the preimage on Modify, the finalize diff),
// so a large table create, sent as many chunked calls, grew quadratically with the row count. A
// transaction left without records is dropped by the transaction buffer (UTransBuffer::End:
// transient transaction), so the user's undo and redo history is untouched.
class FTableUndoRecordingSuppression
{
public:
	explicit FTableUndoRecordingSuppression(UObject& InTable)
		: Table(InTable)
		, bWasTransactional(InTable.HasAnyFlags(RF_Transactional))
	{
		Table.ClearFlags(RF_Transactional);
	}

	~FTableUndoRecordingSuppression()
	{
		if (bWasTransactional)
		{
			Table.SetFlags(RF_Transactional);
		}
	}

	FTableUndoRecordingSuppression(const FTableUndoRecordingSuppression&) = delete;
	FTableUndoRecordingSuppression& operator=(const FTableUndoRecordingSuppression&) = delete;

private:
	UObject& Table;
	const bool bWasTransactional;
};

// The Details panel's ParentTables edit on a composite table (Modify, PreEditChange, array
// write, PostEditChangeProperty(ValueSet)), run inside FAssetDocumentEditorNonInteractiveScope:
// the composite's change callbacks may raise a dialog; a refused one fails the op
// (Composite.InteractiveEditRefused, Core rolls the package back). Parents are pre-checked.
bool WriteParentTables(FReader& Op, UObject& Composite, FArrayProperty& Property, const TArray<UObject*>& Parents);

// Data table handlers (AssetDocumentDataTablePrimitives.cpp).
bool HandleDataTableRead(FCall& Call, FObjectBuilder& Result);
bool HandleDataTableEdit(FCall& Call, FObjectBuilder& Result);
// Curve table handlers (AssetDocumentDataCurveTablePrimitives.cpp).
bool HandleCurveTableRead(FCall& Call, FObjectBuilder& Result);
bool HandleCurveTableEdit(FCall& Call, FObjectBuilder& Result);
// String table handlers (AssetDocumentDataStringTablePrimitives.cpp).
bool HandleStringTableRead(FCall& Call, FObjectBuilder& Result);
bool HandleStringTableEdit(FCall& Call, FObjectBuilder& Result);
// Material instance handlers (AssetDocumentDataMaterialInstancePrimitives.cpp).
bool HandleMaterialInstanceRead(FCall& Call, FObjectBuilder& Result);
bool HandleMaterialInstanceEdit(FCall& Call, FObjectBuilder& Result);
// Material / Material Function graph handlers (AssetDocumentDataMaterialPrimitives.cpp).
bool HandleMaterialRead(FCall& Call, FObjectBuilder& Result);
bool HandleMaterialEdit(FCall& Call, FObjectBuilder& Result);
bool HandleMaterialCompile(FCall& Call, FObjectBuilder& Result);
} // namespace UE::AssetDocument::Graph::Data
