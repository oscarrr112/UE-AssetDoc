// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Open-editor apply behaviour of the data profiles: the asset is open in its asset editor while documents are
// applied; the tests read what the editor itself holds.
//
// UEAssetDoc.DataTable.OpenEditorApply: the Data Table editor's row list (its list view items: row
// names and cell texts) shows renamed, added and removed rows and changed values after an apply, the rolled-back
// rows after a failed apply, and the editor's own row edit and its Undo after the apply.
// UEAssetDoc.UserDefinedTypes.OpenEditorApply.Struct: the Struct editor's Default Values panel shows
// the members after an apply (added and removed members), after a failed apply and after the editor's own Add
// Variable and its Undo.
// UEAssetDoc.UserDefinedTypes.OpenEditorApply.Enum: the Enum editor's enumerator list after the same
// (the editor's own Add Enumerator is followed by an apply, not by Ctrl+Z, which asserts in stock UE 5.7).
// UEAssetDoc.CurveFloat.OpenEditorApply: the Curve editor's curve model (the keys it draws) after the
// same.

#include "AssetDocumentOpenEditorTestSupport.h"

#include "AssetDocumentDataTestRows.h"
#include "Curves/CurveFloat.h"
#include "CurveEditor.h"
#include "CurveModel.h"
#include "DataTableEditorUtils.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/DataTable.h"
#include "Engine/UserDefinedEnum.h"
#include "StructUtils/UserDefinedStruct.h"
#include "FileHelpers.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/ScopeExit.h"
#include "SCurveEditorPanel.h"
#include "SCurveEditorView.h"
#include "ScopedTransaction.h"
#include "Widgets/Views/SListView.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentDataOpenEditorTests
{
using namespace UE::AssetDocument::OpenEditorTests;

FString Document(const FTarget& Target, const TCHAR* Class, const TCHAR* Action, const FString& Body, const FString& Properties = TEXT("{}"))
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"%s\",\"Action\":\"%s\",\"Properties\":%s,\"Body\":{%s}}"),
		*Target.PackageName, Class, Action, *Properties, *Body);
}

// ---- Data Table ----------------------------------------------------------------------------------------------

using FRowList = SListView<FDataTableEditorRowListViewDataPtr>;

// The rows the Data Table editor's list shows: "Name=<cell texts>" in list order.
TArray<FString> EditorRows(FAssetEditorToolkit& Editor)
{
	TArray<FString> Rows;
	for (const TSharedRef<SWidget>& Widget : FindInEditor(Editor, TEXT("SListView<FDataTableEditorRowListViewDataPtr>"), TEXT("DataTableEditor_DataTable")))
	{
		for (const FDataTableEditorRowListViewDataPtr& Row : static_cast<FRowList&>(Widget.Get()).GetItems())
		{
			TArray<FString> Cells;
			for (const FText& Cell : Row.IsValid() ? Row->CellData : TArray<FText>())
			{
				Cells.Add(Cell.ToString());
			}
			Rows.Add(Row.IsValid() ? FString::Printf(TEXT("%s=%s"), *Row->RowId.ToString(), *FString::Join(Cells, TEXT("|"))) : TEXT("<null>"));
		}
	}
	return Rows;
}

FString RowNames(const TArray<FString>& Rows)
{
	TArray<FString> Names;
	for (const FString& Row : Rows)
	{
		Names.Add(Row.Left(Row.Find(TEXT("="))));
	}
	return FString::Join(Names, TEXT(","));
}

bool RowShows(const TArray<FString>& Rows, const TCHAR* Name, const TCHAR* Cell)
{
	const FString Prefix = FString(Name) + TEXT("=");
	return Rows.ContainsByPredicate([&Prefix, Cell](const FString& Row)
	{
		TArray<FString> Cells;
		Row.Mid(Prefix.Len()).ParseIntoArray(Cells, TEXT("|"), false);
		return Row.StartsWith(Prefix) && Cells.Contains(Cell);
	});
}

// ---- User Defined Struct ---------------------------------------------------------------------------------------

// The members the Struct editor's Default Values panel shows (its property names start with the friendly name).
TArray<FString> DefaultValueMembers(FAssetEditorToolkit& Editor)
{
	TArray<FString> Names;
	for (IDetailsView* View : DetailsViewsIn(Editor, TEXT("UserDefinedStruct_DefaultValuesEditor")))
	{
		for (const FString& Name : DisplayedProperties(*View))
		{
			int32 Underscore = INDEX_NONE;
			Names.AddUnique(Name.FindChar(TEXT('_'), Underscore) ? Name.Left(Underscore) : Name);
		}
	}
	return Names;
}

// ---- User Defined Enum -----------------------------------------------------------------------------------------

int32 EnumeratorRows(FAssetEditorToolkit& Editor)
{
	int32 Rows = 0;
	for (IDetailsView* View : DetailsViewsIn(Editor, TEXT("UserDefinedEnum_EnumeratorEditor")))
	{
		// The panel rebuilds its rows on its Slate tick (deferred refresh after the Enum editor's change notification);
		// tick it as frames would. The enumerator rows sit under the Enumerators category's list; count them expanded
		// (as the user sees them).
		for (int32 Frame = 0; Frame < 2; ++Frame)
		{
			View->Tick(FGeometry(), FPlatformTime::Seconds(), 0.f);
		}
		View->SetRootExpansionStates(true, true);
		Rows += View->CountRows();
	}
	return Rows;
}

// ---- Curve Float -----------------------------------------------------------------------------------------------

// The keys the Curve editor's curve model draws: "time=value" in time order.
TArray<FString> EditorKeys(FAssetEditorToolkit& Editor)
{
	TArray<FString> Keys;
	// The Curve tab's panel builds its views (SCurveEditorView subclasses, sharing the editor's FCurveEditor) on its
	// Slate tick; tick it as a frame would.
	TSharedPtr<FCurveEditor> CurveEditor;
	for (const TSharedRef<SWidget>& Widget : FindInEditor(Editor, TEXT("SCurveEditorPanel"), TEXT("CurveAssetEditor_Curve")))
	{
		SCurveEditorPanel& Panel = static_cast<SCurveEditorPanel&>(Widget.Get());
		static_cast<SWidget&>(Panel).Tick(FGeometry(), FPlatformTime::Seconds(), 0.f);
		for (const TSharedPtr<SCurveEditorView>& View : Panel.GetViews())
		{
			CurveEditor = CurveEditor.IsValid() || !View.IsValid() ? CurveEditor : View->GetCurveEditor();
		}
	}
	{
		if (!CurveEditor.IsValid())
		{
			return Keys;
		}
		for (const TPair<FCurveModelID, TUniquePtr<FCurveModel>>& Curve : CurveEditor->GetCurves())
		{
			TArray<FKeyHandle> Handles;
			Curve.Value->GetKeys(-TNumericLimits<double>::Max(), TNumericLimits<double>::Max(), -TNumericLimits<double>::Max(),
				TNumericLimits<double>::Max(), Handles);
			TArray<FKeyPosition> Positions;
			Positions.SetNum(Handles.Num());
			Curve.Value->GetKeyPositions(Handles, Positions);
			for (const FKeyPosition& Position : Positions)
			{
				Keys.Add(FString::Printf(TEXT("%g=%g"), Position.InputValue, Position.OutputValue));
			}
		}
	}
	return Keys;
}

FString CurveProperties(const TCHAR* Keys)
{
	return FString::Printf(TEXT("{\"FloatCurve\":{\"Keys\":%s}}"), Keys);
}

FString CurveDocument(const FTarget& Target, const TCHAR* Action, const TCHAR* Keys)
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.CurveFloat\",\"AssetType\":\"GenericAsset\",\"Action\":\"%s\",\"Properties\":%s}"),
		*Target.PackageName, Action, *CurveProperties(Keys));
}
} // namespace AssetDocumentDataOpenEditorTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDataTableOpenEditorApplyTest, "UEAssetDoc.DataTable.OpenEditorApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentDataTableOpenEditorApplyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentDataOpenEditorTests;
	const TCHAR* Class = TEXT("/Script/Engine.DataTable");
	const FTarget Target(NewFolder(), TEXT("DT_OpenEditor"));
	ON_SCOPE_EXIT { Cleanup({Target}); };
	if (!ApplyOk(*this, Target, Document(Target, Class, TEXT("Create"), TEXT("\"RowStruct\":\"/Script/AssetDocumentTests.AssetDocumentDataTestRow\","
		"\"Rows\":[{\"Name\":\"Sword\",\"Values\":{\"Count\":3}},{\"Name\":\"Bow\",\"Values\":{\"Count\":1}}]")), TEXT("create")))
	{
		return false;
	}
	UDataTable* Table = Target.Get<UDataTable>();
	FAssetEditorToolkit* Editor = Open(*this, Table);
	if (Editor == nullptr)
	{
		return false;
	}
	// The user has Bow selected (the row editor shows it).
	FDataTableEditorUtils::SelectRow(Table, TEXT("Bow"));
	Tick();
	TestEqual(TEXT("before: the editor lists the rows"), RowNames(EditorRows(*Editor)), FString(TEXT("Sword,Bow")));
	ReleaseUndoHistory();

	// 1. Rows edited, added and removed.
	if (!ApplyOk(*this, Target, Document(Target, Class, TEXT("Update"),
		TEXT("\"Rows\":[{\"Name\":\"Sword\",\"Values\":{\"Count\":77}},{\"Name\":\"Axe\",\"Values\":{\"Count\":5}}]")), TEXT("apply while open")))
	{
		return false;
	}
	Tick();
	TestTrue(TEXT("the editor is still open"), FindToolkit(Table) == Editor);
	TArray<FString> Rows = EditorRows(*Editor);
	AddInfo(FString::Printf(TEXT("after apply: %s"), *FString::Join(Rows, TEXT("; "))));
	TestEqual(TEXT("editor: the row list shows the applied rows"), RowNames(Rows), FString(TEXT("Sword,Axe")));
	TestTrue(TEXT("editor: the row list shows Sword's new Count"), RowShows(Rows, TEXT("Sword"), TEXT("77")));
	const FString Applied = Extract(*this, Target, TEXT("after apply"));
	TestUndoKeepsApply(*this, Target, Applied, TEXT("after apply"));

	// 2. A failed apply rolls back.
	ApplyFailingAfterMutation(*this, Target, Document(Target, Class, TEXT("Update"),
		TEXT("\"Rows\":[{\"Name\":\"Sword\",\"Values\":{\"Count\":5}},{\"Name\":\"Extra\"}]")), TEXT("failing apply while open"));
	Tick();
	Table = Target.Get<UDataTable>();
	TestEqual(TEXT("after rollback: the extraction is the applied state"), Extract(*this, Target, TEXT("after rollback")), Applied);
	Rows = EditorRows(*Editor);
	AddInfo(FString::Printf(TEXT("after rollback: %s"), *FString::Join(Rows, TEXT("; "))));
	TestEqual(TEXT("after rollback: the row list shows the applied rows"), RowNames(Rows), FString(TEXT("Sword,Axe")));
	TestTrue(TEXT("after rollback: the row list shows Sword's applied Count"), RowShows(Rows, TEXT("Sword"), TEXT("77")));

	// 3. The editor's own row edit after the apply (the row editor's value change), and its Undo.
	if (Table != nullptr)
	{
		{
			const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentDataOpenEditorTests", "EditRow", "Edit Row"));
			Table->Modify();
			FDataTableEditorUtils::BroadcastPreChange(Table, FDataTableEditorUtils::EDataTableChangeInfo::RowData);
			if (FAssetDocumentDataTestRow* Sword = Table->FindRow<FAssetDocumentDataTestRow>(TEXT("Sword"), TEXT("OpenEditorApply"), false))
			{
				Sword->Count = 42;
			}
			Table->HandleDataTableChanged(TEXT("Sword"));
			FDataTableEditorUtils::BroadcastPostChange(Table, FDataTableEditorUtils::EDataTableChangeInfo::RowData);
		}
		TestTrue(TEXT("the human edit shows in the row list"), RowShows(EditorRows(*Editor), TEXT("Sword"), TEXT("42")));
		TestTrue(TEXT("the human edit undoes"), GEditor->UndoTransaction());
		Tick();
		TestTrue(TEXT("after Undo: the row list shows the applied Count"), RowShows(EditorRows(*Editor), TEXT("Sword"), TEXT("77")));
		TestEqual(TEXT("after Undo: the extraction is the applied state"), Extract(*this, Target, TEXT("after undo")), Applied);
	}
	Close(Table);
	TestNull(TEXT("the editor closed"), FindEditor(Target.Get()));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUserDefinedStructOpenEditorApplyTest, "UEAssetDoc.UserDefinedTypes.OpenEditorApply.Struct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUserDefinedStructOpenEditorApplyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentDataOpenEditorTests;
	const TCHAR* Class = TEXT("/Script/CoreUObject.UserDefinedStruct");
	const FTarget Target(NewFolder(), TEXT("S_OpenEditor"));
	ON_SCOPE_EXIT { Cleanup({Target}); };
	if (!ApplyOk(*this, Target, Document(Target, Class, TEXT("Create"),
		TEXT("\"Members\":[{\"Name\":\"Label\",\"Type\":\"string\",\"DefaultValue\":\"A\"},{\"Name\":\"Damage\",\"Type\":\"float\",\"DefaultValue\":1.5}]")),
		TEXT("create")))
	{
		return false;
	}
	UUserDefinedStruct* Struct = Target.Get<UUserDefinedStruct>();
	FAssetEditorToolkit* Editor = Open(*this, Struct);
	if (Editor == nullptr)
	{
		return false;
	}
	TArray<FString> Members = DefaultValueMembers(*Editor);
	AddInfo(FString::Printf(TEXT("before: default values show %s"), *FString::Join(Members, TEXT(","))));
	TestTrue(TEXT("before: the Default Values panel shows Damage"), Members.Contains(TEXT("Damage")));
	ReleaseUndoHistory();

	// 1. A member added, one removed, a default changed.
	if (!ApplyOk(*this, Target, Document(Target, Class, TEXT("Update"),
		TEXT("\"Members\":[{\"Name\":\"Label\",\"Type\":\"string\",\"DefaultValue\":\"B\"},{\"Name\":\"Armor\",\"Type\":\"int\",\"DefaultValue\":5}]")),
		TEXT("apply while open")))
	{
		return false;
	}
	Tick();
	TestTrue(TEXT("the editor is still open"), FindToolkit(Struct) == Editor);
	Members = DefaultValueMembers(*Editor);
	AddInfo(FString::Printf(TEXT("after apply: default values show %s"), *FString::Join(Members, TEXT(","))));
	TestTrue(TEXT("editor: the Default Values panel shows Armor"), Members.Contains(TEXT("Armor")));
	TestFalse(TEXT("editor: the Default Values panel no longer shows Damage"), Members.Contains(TEXT("Damage")));
	const FString Applied = Extract(*this, Target, TEXT("after apply"));
	TestUndoKeepsApply(*this, Target, Applied, TEXT("after apply"));

	// 2. A failed apply rolls back.
	ApplyFailingAfterMutation(*this, Target, Document(Target, Class, TEXT("Update"),
		TEXT("\"Members\":[{\"Name\":\"Label\",\"Type\":\"string\",\"DefaultValue\":\"C\"},{\"Name\":\"Armor\",\"Type\":\"int\",\"DefaultValue\":5},"
			"{\"Name\":\"Bogus\",\"Type\":\"bool\"}]")), TEXT("failing apply while open"));
	Tick();
	TestEqual(TEXT("after rollback: the extraction is the applied state"), Extract(*this, Target, TEXT("after rollback")), Applied);
	Members = DefaultValueMembers(*Editor);
	AddInfo(FString::Printf(TEXT("after rollback: default values show %s"), *FString::Join(Members, TEXT(","))));
	TestTrue(TEXT("after rollback: the Default Values panel shows Armor"), Members.Contains(TEXT("Armor")));
	TestFalse(TEXT("after rollback: the Default Values panel does not show Bogus"), Members.Contains(TEXT("Bogus")));

	// 3. The editor's own Add Variable after the apply, and its Undo.
	Struct = Target.Get<UUserDefinedStruct>();
	if (Struct != nullptr)
	{
		FEdGraphPinType Type;
		Type.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		TestTrue(TEXT("the human adds a member"), FStructureEditorUtils::AddVariable(Struct, Type));
		const int32 WithHuman = DefaultValueMembers(*Editor).Num();
		TestEqual(TEXT("the human member shows in the Default Values panel"), WithHuman, Members.Num() + 1);
		TestTrue(TEXT("the human edit undoes"), GEditor->UndoTransaction());
		Tick();
		TestEqual(TEXT("after Undo: the Default Values panel shows the applied members"), DefaultValueMembers(*Editor).Num(), Members.Num());
		TestEqual(TEXT("after Undo: the extraction is the applied state"), Extract(*this, Target, TEXT("after undo")), Applied);
	}
	Close(Struct);
	TestNull(TEXT("the editor closed"), FindEditor(Target.Get()));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentUserDefinedEnumOpenEditorApplyTest, "UEAssetDoc.UserDefinedTypes.OpenEditorApply.Enum",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentUserDefinedEnumOpenEditorApplyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentDataOpenEditorTests;
	const TCHAR* Class = TEXT("/Script/Engine.UserDefinedEnum");
	const FTarget Target(NewFolder(), TEXT("E_OpenEditor"));
	ON_SCOPE_EXIT { Cleanup({Target}); };
	if (!ApplyOk(*this, Target, Document(Target, Class, TEXT("Create"), TEXT("\"Enumerators\":[{\"Name\":\"Alpha\"},{\"Name\":\"Beta\"}]")), TEXT("create")))
	{
		return false;
	}
	UUserDefinedEnum* Enum = Target.Get<UUserDefinedEnum>();
	FAssetEditorToolkit* Editor = Open(*this, Enum);
	if (Editor == nullptr)
	{
		return false;
	}
	const int32 RowsBefore = EnumeratorRows(*Editor);
	AddInfo(FString::Printf(TEXT("before: %d rows"), RowsBefore));
	ReleaseUndoHistory();

	// 1. Two enumerators added, one removed (2 -> 3).
	if (!ApplyOk(*this, Target, Document(Target, Class, TEXT("Update"),
		TEXT("\"Enumerators\":[{\"Name\":\"Alpha\"},{\"Name\":\"Gamma\"},{\"Name\":\"Delta\"}]")), TEXT("apply while open")))
	{
		return false;
	}
	Tick();
	TestTrue(TEXT("the editor is still open"), FindToolkit(Enum) == Editor);
	const int32 RowsApplied = EnumeratorRows(*Editor);
	AddInfo(FString::Printf(TEXT("after apply: %d rows"), RowsApplied));
	TestEqual(TEXT("editor: the enumerator list has one row more"), RowsApplied, RowsBefore + 1);
	const FString Applied = Extract(*this, Target, TEXT("after apply"));
	TestUndoKeepsApply(*this, Target, Applied, TEXT("after apply"));

	// 2. A failed apply rolls back.
	ApplyFailingAfterMutation(*this, Target, Document(Target, Class, TEXT("Update"),
		TEXT("\"Enumerators\":[{\"Name\":\"Alpha\"},{\"Name\":\"Gamma\"},{\"Name\":\"Delta\"},{\"Name\":\"Bogus\"},{\"Name\":\"Bogus2\"}]")),
		TEXT("failing apply while open"));
	Tick();
	TestEqual(TEXT("after rollback: the extraction is the applied state"), Extract(*this, Target, TEXT("after rollback")), Applied);
	TestEqual(TEXT("after rollback: the enumerator list shows the applied enumerators"), EnumeratorRows(*Editor), RowsApplied);

	// 3. The editor's own Add Enumerator after the apply. Stock UE 5.7 cannot undo an Enum editor step (the transaction
	// buffer re-serializes the UEnum over its live name table: UEnum::Serialize asserts NumValues == 0, Enum.cpp:119,
	// also without AssetDocument), so instead of Ctrl+Z the next apply takes the human enumerator out again.
	Enum = Target.Get<UUserDefinedEnum>();
	if (Enum != nullptr)
	{
		FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
		TestEqual(TEXT("the human enumerator shows in the list"), EnumeratorRows(*Editor), RowsApplied + 1);
		// An unsaved editor edit (no private working copy): the existing dirty-package rule refuses before any change.
		const FAssetDocumentResult Refused = Apply(*this, Target, Document(Target, Class, TEXT("Update"),
			TEXT("\"Enumerators\":[{\"Name\":\"Alpha\"},{\"Name\":\"Gamma\"},{\"Name\":\"Delta\"}]")), TEXT("apply over the unsaved human edit"));
		TestTrue(TEXT("an apply over the unsaved human edit is refused (DirtyExistingAssetTransactionUnsupported)"), !Refused.IsSuccess()
			&& Refused.Diagnostics.ContainsByPredicate([](const FAssetDocumentDiagnostic& D) { return D.Code == TEXT("DirtyExistingAssetTransactionUnsupported"); }));
		TestEqual(TEXT("the refused apply leaves the human enumerator in the list"), EnumeratorRows(*Editor), RowsApplied + 1);
		TestTrue(TEXT("the human saves"), UEditorLoadingAndSavingUtils::SavePackages({Enum->GetOutermost()}, false));
		ApplyOk(*this, Target, Document(Target, Class, TEXT("Update"),
			TEXT("\"Enumerators\":[{\"Name\":\"Alpha\"},{\"Name\":\"Gamma\"},{\"Name\":\"Delta\"}]")), TEXT("apply over the human edit"));
		Tick();
		TestEqual(TEXT("after the next apply: the list shows the applied enumerators"), EnumeratorRows(*Editor), RowsApplied);
		TestEqual(TEXT("after the next apply: the extraction is the applied state"), Extract(*this, Target, TEXT("after next apply")), Applied);
	}
	Close(Enum);
	TestNull(TEXT("the editor closed"), FindEditor(Target.Get()));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentCurveFloatOpenEditorApplyTest, "UEAssetDoc.CurveFloat.OpenEditorApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCurveFloatOpenEditorApplyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentDataOpenEditorTests;
	const FTarget Target(NewFolder(), TEXT("C_OpenEditor"));
	ON_SCOPE_EXIT { Cleanup({Target}); };
	if (!ApplyOk(*this, Target, CurveDocument(Target, TEXT("Create"), TEXT("[{\"Time\":0,\"Value\":0},{\"Time\":1,\"Value\":1}]")), TEXT("create")))
	{
		return false;
	}
	UCurveFloat* Curve = Target.Get<UCurveFloat>();
	FAssetEditorToolkit* Editor = Open(*this, Curve);
	if (Editor == nullptr)
	{
		return false;
	}
	TestEqual(TEXT("before: the curve editor draws the keys"), FString::Join(EditorKeys(*Editor), TEXT(",")), FString(TEXT("0=0,1=1")));
	ReleaseUndoHistory();

	// 1. A key moved, one added.
	if (!ApplyOk(*this, Target, CurveDocument(Target, TEXT("Update"), TEXT("[{\"Time\":0,\"Value\":0},{\"Time\":1,\"Value\":5},{\"Time\":2,\"Value\":2}]")),
		TEXT("apply while open")))
	{
		return false;
	}
	Tick();
	TestTrue(TEXT("the editor is still open"), FindToolkit(Curve) == Editor);
	TestEqual(TEXT("editor: the curve editor draws the applied keys"), FString::Join(EditorKeys(*Editor), TEXT(",")), FString(TEXT("0=0,1=5,2=2")));
	const FString Applied = Extract(*this, Target, TEXT("after apply"));
	TestUndoKeepsApply(*this, Target, Applied, TEXT("after apply"));

	// 2. A failed apply rolls back.
	ApplyFailingAfterMutation(*this, Target, CurveDocument(Target, TEXT("Update"), TEXT("[{\"Time\":0,\"Value\":9}]")), TEXT("failing apply while open"));
	Tick();
	TestEqual(TEXT("after rollback: the extraction is the applied state"), Extract(*this, Target, TEXT("after rollback")), Applied);
	TestEqual(TEXT("after rollback: the curve editor draws the applied keys"), FString::Join(EditorKeys(*Editor), TEXT(",")), FString(TEXT("0=0,1=5,2=2")));

	// 3. The editor's own key edit after the apply, and its Undo.
	Curve = Target.Get<UCurveFloat>();
	if (Curve != nullptr)
	{
		{
			const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentDataOpenEditorTests", "AddKey", "Add Key"));
			Curve->Modify();
			Curve->FloatCurve.AddKey(3.f, 3.f);
		}
		TestEqual(TEXT("the human key shows in the curve editor"), FString::Join(EditorKeys(*Editor), TEXT(",")), FString(TEXT("0=0,1=5,2=2,3=3")));
		TestTrue(TEXT("the human edit undoes"), GEditor->UndoTransaction());
		Tick();
		TestEqual(TEXT("after Undo: the curve editor draws the applied keys"), FString::Join(EditorKeys(*Editor), TEXT(",")), FString(TEXT("0=0,1=5,2=2")));
		TestEqual(TEXT("after Undo: the extraction is the applied state"), Extract(*this, Target, TEXT("after undo")), Applied);
	}
	Close(Curve);
	TestNull(TEXT("the editor closed"), FindEditor(Target.Get()));
	return !HasAnyErrors();
}
#endif // WITH_DEV_AUTOMATION_TESTS
