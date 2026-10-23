// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// End-to-end DataTable family through the production Service and the Python Profiles
// (profile.data_table.v1, profile.composite_data_table.v1, profile.mirror_data_table.v1):
//
// UEAssetDoc.DataTable.Python: one ApplyFile creates a table with its row struct
// (the New Data Table dialog's choice) and populates rows of every value kind; Extract/Diff agree;
// re-applying the extraction is a no-op (no save, package file untouched); a second apply renames,
// removes, adds, edits and reorders rows; an unknown field, an invalid value and a different
// RowStruct fail with the document path and change nothing; a failure in the middle of the row
// edits rolls back; Inspect lists the row schema; the saved package reloads to the same document;
// failed creates (no / unknown / invalid row struct) leave nothing behind.
// UEAssetDoc.DataTable.UserDefinedStruct: rows of a User Defined Struct (fields by
// their friendly names).
// UEAssetDoc.DataTable.Composite: ParentTables authored, rows derived and read-only.
// UEAssetDoc.DataTable.Mirror: create = the New Mirror Data Table dialog (FactoryContext =
// Skeleton, Body.RowStruct = Row Structure, default FMirrorTableRow): the same rows as the factory configured
// by its dialog, then update / no-op / reload; no Skeleton or a struct without MirroredName leaves nothing;
// rows of an existing table (made without the factory) update.
// UEAssetDoc.DataTable.Undo: a human row edit + save, then an AI apply; Ctrl+Z
// afterwards must not corrupt the table (AssetDocumentUndoHistory.h).

#include "AssetDocumentDataTableTestSupport.h"
#include "AssetDocumentDataTestRows.h"

#include "Animation/MirrorDataTable.h"
#include "Animation/Skeleton.h"
#include "Factories/Factory.h"
#include "DataTableEditorUtils.h"
#include "Engine/CompositeDataTable.h"
#include "Engine/DataTable.h"
#include "StructUtils/UserDefinedStruct.h"
#include "FileHelpers.h"
#include "Kismet2/StructureEditorUtils.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "Misc/ScopeExit.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ScopedTransaction.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentDataTablePythonTests
{
using namespace AssetDocumentDataTableTests;

const TCHAR* DataTableClass = TEXT("/Script/Engine.DataTable");
const TCHAR* CompositeClass = TEXT("/Script/Engine.CompositeDataTable");
const TCHAR* MirrorClass = TEXT("/Script/Engine.MirrorDataTable");
const TCHAR* TestRow = TEXT("/Script/AssetDocumentTests.AssetDocumentDataTestRow");
const TCHAR* OtherRow = TEXT("/Script/AssetDocumentTests.AssetDocumentDataTestOtherRow");
const TCHAR* NotARow = TEXT("/Script/AssetDocumentTests.AssetDocumentDataTestNotARow");

FString RowsBody(const TCHAR* RowStruct, const TCHAR* Rows)
{
	return FString::Printf(TEXT("\"RowStruct\":\"%s\",\"Rows\":%s"), RowStruct, Rows);
}

const FAssetDocumentDataTestRow* Row(const UDataTable* Table, const TCHAR* Name)
{
	return Table != nullptr ? Table->FindRow<FAssetDocumentDataTestRow>(FName(Name), TEXT("AssetDocumentDataTablePythonTests"), false) : nullptr;
}

FString RowNames(const UDataTable* Table)
{
	TArray<FString> Names;
	for (const FName& Name : Table != nullptr ? Table->GetRowNames() : TArray<FName>())
	{
		Names.Add(Name.ToString());
	}
	return FString::Join(Names, TEXT(","));
}

int32 Count(const UDataTable* Table, const TCHAR* Name)
{
	const FAssetDocumentDataTestRow* Found = Row(Table, Name);
	return Found != nullptr ? Found->Count : INDEX_NONE;
}

const TCHAR* InitialRows = TEXT(R"json([
  {"Name":"Sword","Values":{"Label":"Sword","Count":3,"Weight":0.3,"Precise":0.1,"bEnabled":true,"Tag":"Melee","Kind":"Beta",
    "Offset":{"X":1,"Y":2.5,"Z":-3},"Levels":[1,2,3],"Icon":"/Engine/EngineResources/DefaultTexture.DefaultTexture",
    "DisplayName":"Sword of Dawn","Source":"/Engine/EngineResources/DefaultTexture.DefaultTexture"}},
  {"Name":"Bow","Values":{"Label":"Bow","Count":1,"Source":"/Engine/EngineResources/DefaultTexture.DefaultTexture"}},
  {"Name":"Staff"}])json");

// Rename Bow -> Longbow, remove Staff, add Axe, edit Sword, reorder. Values is the whole row:
// Longbow leaves Label out (back to the struct default), Sword leaves Weight, Precise, bEnabled, Tag,
// Kind, Offset and Icon out (all back to the struct defaults; Weight's default is 1).
const TCHAR* EditedRows = TEXT(R"json([
  {"Name":"Axe","Values":{"Label":"Axe","Kind":"Gamma","DisplayName":"Axe"}},
  {"Name":"Longbow","RenamedFrom":"Bow","Values":{"Count":2,"Source":null}},
  {"Name":"Sword","Values":{"Label":"Sword","Count":5,"Levels":[4],"DisplayName":"Dawnblade",
    "Source":"/Engine/EngineResources/DefaultTexture.DefaultTexture"}}])json");

const TCHAR* DefaultTexture = TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture");

// The Inspect RowSchema entry of a field ({"Name":..., "Type":..., "Codec":..., "Supported":...}).
FString SchemaEntry(const FString& Inspected, const TCHAR* Field)
{
	const int32 Start = Inspected.Find(FString::Printf(TEXT("\"Name\":\"%s\""), Field));
	const int32 End = Start == INDEX_NONE ? INDEX_NONE : Inspected.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
	return End == INDEX_NONE ? FString() : Inspected.Mid(Start, End - Start);
}
} // namespace AssetDocumentDataTablePythonTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDataTablePythonTest, "UEAssetDoc.DataTable.Python",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentDataTablePythonTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentDataTablePythonTests;
	const FTarget Target(TEXT("DT_Items"));
	const FTarget NoStruct(TEXT("DT_NoStruct"));
	const FTarget MissingStruct(TEXT("DT_MissingStruct"));
	const FTarget InvalidStruct(TEXT("DT_InvalidStruct"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		NoStruct.Cleanup();
		MissingStruct.Cleanup();
		InvalidStruct.Cleanup();
	};

	// 1. One apply creates the table with its row struct and populates it.
	const FString Created = Document(Target, DataTableClass, TEXT("Create"), RowsBody(TestRow, InitialRows),
		TEXT("{\"bStripFromClientBuilds\":true}"));
	if (!TestTrue(TEXT("create applies"), Apply(*this, Target, Created, TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UDataTable* Table = Target.Get<UDataTable>();
	if (!TestNotNull(TEXT("DataTable exists"), Table))
	{
		return false;
	}
	TestTrue(TEXT("package is saved"), IFileManager::Get().FileExists(*Target.PackageFile));
	TestTrue(TEXT("row struct is the dialog's choice"), Table->GetRowStruct() == FAssetDocumentDataTestRow::StaticStruct());
	TestEqual(TEXT("rows in document order"), RowNames(Table), FString(TEXT("Sword,Bow,Staff")));
	TestTrue(TEXT("Details property"), Table->bStripFromClientBuilds);
	if (const FAssetDocumentDataTestRow* Sword = Row(Table, TEXT("Sword")))
	{
		TestEqual(TEXT("string"), Sword->Label, FString(TEXT("Sword")));
		TestEqual(TEXT("int"), Sword->Count, 3);
		TestEqual(TEXT("float"), Sword->Weight, 0.3f);
		TestEqual(TEXT("double"), Sword->Precise, 0.1);
		TestTrue(TEXT("bool"), Sword->bEnabled);
		TestEqual(TEXT("name"), Sword->Tag, FName(TEXT("Melee")));
		TestTrue(TEXT("enum"), Sword->Kind == EAssetDocumentDataTestKind::Beta);
		TestEqual(TEXT("struct"), Sword->Offset, FVector(1, 2.5, -3));
		TestEqual(TEXT("array"), Sword->Levels, TArray<int32>({1, 2, 3}));
		TestEqual(TEXT("soft reference"), Sword->Icon.ToSoftObjectPath().ToString(),
			FString(TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture")));
		TestEqual(TEXT("text"), Sword->DisplayName.ToString(), FString(TEXT("Sword of Dawn")));
		TestTrue(TEXT("text is localizable source text"), !Sword->DisplayName.IsCultureInvariant() && !Sword->DisplayName.IsFromStringTable());
		TestTrue(TEXT("hard reference"), Sword->Source != nullptr && Sword->Source->GetPathName() == DefaultTexture);
	}
	else
	{
		AddError(TEXT("row Sword exists"));
	}
	if (const FAssetDocumentDataTestRow* Staff = Row(Table, TEXT("Staff")))
	{
		TestTrue(TEXT("a row without Values keeps the struct defaults"), Staff->Count == 0 && Staff->Weight == 1.f && Staff->Label.IsEmpty());
	}
	TestNoDiff(*this, Created, TEXT("after create"));

	// 2. Re-applying the extraction is a no-op.
	FString Baseline = TestNoOpReapply(*this, Target, TEXT("Reapply"));
	{
		// float32 0.3 extracts as the shortest decimal (0.3), not 0.30000001192092896.
		const TArray<TSharedPtr<FJsonValue>> Rows = BodyArray(Parse(Baseline), TEXT("Rows"));
		const TSharedPtr<FJsonObject>* Values = nullptr;
		TestTrue(TEXT("extraction carries the float as written"), Rows.Num() == 3
			&& Rows[0]->AsObject()->TryGetObjectField(TEXT("Values"), Values) && (*Values)->GetNumberField(TEXT("Weight")) == 0.3);
	}

	// 3. Inspect lists the row schema.
	const FString Inspected = Inspect(*this, Target, TEXT("Inspect"));
	TestTrue(TEXT("Inspect lists RowSchema fields"), Inspected.Contains(TEXT("\"RowSchema\"")) && Inspected.Contains(TEXT("\"Levels\""))
		&& Inspected.Contains(TEXT("\"EnumValues\"")));
	{
		const FString Text = SchemaEntry(Inspected, TEXT("DisplayName"));
		const FString Hard = SchemaEntry(Inspected, TEXT("Source"));
		const FString Inline = SchemaEntry(Inspected, TEXT("Inline"));
		TestTrue(*FString::Printf(TEXT("FText field is supported (%s)"), *Text), Text.Contains(TEXT("\"Codec\":\"text.v1\"")) && Text.Contains(TEXT("\"Supported\":true")));
		TestTrue(*FString::Printf(TEXT("hard reference field is supported (%s)"), *Hard), Hard.Contains(TEXT("\"Codec\":\"object_path.v1\"")) && Hard.Contains(TEXT("\"Supported\":true")));
		TestTrue(*FString::Printf(TEXT("instanced field is not supported (%s)"), *Inline), Inline.Contains(TEXT("\"Codec\":\"\"")) && Inline.Contains(TEXT("\"Supported\":false")));
	}

	// 4. A second apply renames, removes, adds, edits and reorders.
	const FString Edited = Document(Target, DataTableClass, TEXT("Update"), FString::Printf(TEXT("\"Rows\":%s"), EditedRows));
	if (TestTrue(TEXT("edit applies"), Apply(*this, Target, Edited, TEXT("Edit")).IsSuccess()))
	{
		TestEqual(TEXT("rows renamed, removed, added and reordered"), RowNames(Table), FString(TEXT("Axe,Longbow,Sword")));
		const FAssetDocumentDataTestRow* Longbow = Row(Table, TEXT("Longbow"));
		TestTrue(TEXT("renamed row holds the written values"), Longbow != nullptr && Longbow->Count == 2);
		TestTrue(TEXT("a field left out of a row is the struct default (Label)"), Longbow != nullptr && Longbow->Label.IsEmpty());
		TestTrue(TEXT("hard reference cleared by null"), Longbow != nullptr && Longbow->Source == nullptr);
		const FAssetDocumentDataTestRow* Sword = Row(Table, TEXT("Sword"));
		TestTrue(TEXT("written fields applied"), Sword != nullptr && Sword->Count == 5 && Sword->Label == TEXT("Sword")
			&& Sword->Levels == TArray<int32>({4}) && Sword->DisplayName.ToString() == TEXT("Dawnblade")
			&& Sword->Source != nullptr && Sword->Source->GetPathName() == DefaultTexture);
		TestTrue(TEXT("fields left out of the row are the struct defaults"), Sword != nullptr && Sword->Weight == 1.f
			&& Sword->Precise == 0.0 && !Sword->bEnabled && Sword->Tag.IsNone() && Sword->Kind == EAssetDocumentDataTestKind::Alpha
			&& Sword->Offset == FVector::ZeroVector && Sword->Icon.IsNull());
		TestTrue(TEXT("added row"), Row(Table, TEXT("Axe")) != nullptr && Row(Table, TEXT("Axe"))->Kind == EAssetDocumentDataTestKind::Gamma);
		TestNoDiff(*this, Edited, TEXT("after edit"));
	}
	Baseline = TestNoOpReapply(*this, Target, TEXT("Reapply edited"));

	// 5. Errors name the document location and change nothing.
	const FAssetDocumentResult Unknown = Apply(*this, Target, Document(Target, DataTableClass, TEXT("Update"), TEXT(
		"\"Rows\":[{\"Name\":\"Axe\"},{\"Name\":\"Longbow\"},{\"Name\":\"Sword\",\"Values\":{\"Damage\":1}}]")), TEXT("UnknownField"));
	TestFailsWith(*this, Unknown, TEXT("DataTable.UnknownField"), TEXT("/Body/Rows/2/Values/Damage"), TEXT("unknown field"));
	ExtractEquals(*this, Target, Baseline, TEXT("after unknown field"));
	const FAssetDocumentResult WrongStruct = Apply(*this, Target, Document(Target, DataTableClass, TEXT("Update"),
		FString::Printf(TEXT("\"RowStruct\":\"%s\""), OtherRow)), TEXT("WrongRowStruct"));
	TestFailsWith(*this, WrongStruct, TEXT("DataTable.RowStructImmutable"), TEXT("/Body/RowStruct"), TEXT("different row struct"));
	ExtractEquals(*this, Target, Baseline, TEXT("after different row struct"));
	const FAssetDocumentResult BadKey = Apply(*this, Target, Document(Target, DataTableClass, TEXT("Update"), TEXT(
		"\"Rows\":[{\"Name\":\"Axe\",\"Value\":{}}]")), TEXT("UnknownRowKey"));
	TestFailsWith(*this, BadKey, TEXT("DataTable.UnknownRowKey"), TEXT("/Body/Rows/0/Value"), TEXT("unknown row key"));

	// 6. Failure in the middle of the row edits (rows already removed and added, then an invalid
	//    value): the table rolls back whole.
	const FAssetDocumentResult MidFailed = Apply(*this, Target, Document(Target, DataTableClass, TEXT("Update"), TEXT(
		"\"Rows\":[{\"Name\":\"Extra\"},{\"Name\":\"Sword\",\"Values\":{\"Count\":\"many\"}}]")), TEXT("MidApplyFailure"));
	TestFailsWith(*this, MidFailed, TEXT("DataTable.InvalidValue"), TEXT("/Body/Rows/1/Values/Count"), TEXT("invalid value"));
	Table = Target.Get<UDataTable>();
	TestTrue(TEXT("rolled-back package is clean"), Table != nullptr && !Table->GetOutermost()->IsDirty());
	TestEqual(TEXT("rolled-back rows"), RowNames(Table), FString(TEXT("Axe,Longbow,Sword")));
	ExtractEquals(*this, Target, Baseline, TEXT("after mid-apply rollback"));

	// 7. Values the row editor can author but the document gets wrong fail at their location; an
	//    instanced subobject (the row editor cannot create one) is refused when authored.
	const FAssetDocumentResult Missing = Apply(*this, Target, Document(Target, DataTableClass, TEXT("Update"), TEXT(
		"\"Rows\":[{\"Name\":\"Axe\",\"Values\":{\"Source\":\"/Game/AssetDocumentDataTableMissing/Nope.Nope\"}},{\"Name\":\"Longbow\"},{\"Name\":\"Sword\"}]")), TEXT("MissingReference"));
	TestFailsWith(*this, Missing, TEXT("DataTable.InvalidValue"), TEXT("/Body/Rows/0/Values/Source"), TEXT("unresolved hard reference"));
	ExtractEquals(*this, Target, Baseline, TEXT("after unresolved hard reference"));
	const FAssetDocumentResult BadText = Apply(*this, Target, Document(Target, DataTableClass, TEXT("Update"), TEXT(
		"\"Rows\":[{\"Name\":\"Axe\",\"Values\":{\"DisplayName\":7}},{\"Name\":\"Longbow\"},{\"Name\":\"Sword\"}]")), TEXT("InvalidText"));
	TestFailsWith(*this, BadText, TEXT("DataTable.InvalidValue"), TEXT("/Body/Rows/0/Values/DisplayName"), TEXT("invalid text"));
	ExtractEquals(*this, Target, Baseline, TEXT("after invalid text"));
	const FAssetDocumentResult Unsupported = Apply(*this, Target, Document(Target, DataTableClass, TEXT("Update"), TEXT(
		"\"Rows\":[{\"Name\":\"Axe\",\"Values\":{\"Inline\":null}},{\"Name\":\"Longbow\"},{\"Name\":\"Sword\"}]")), TEXT("Unsupported"));
	TestFailsWith(*this, Unsupported, TEXT("DataTable.UnsupportedField"), TEXT("/Body/Rows/0/Values/Inline"), TEXT("unsupported field"));
	ExtractEquals(*this, Target, Baseline, TEXT("after unsupported field"));
	TestFalse(TEXT("the extraction omits the instanced field"), Baseline.Contains(TEXT("\"Inline\"")));
	TestTrue(TEXT("the extraction carries text and hard reference"), Baseline.Contains(TEXT("\"Dawnblade\""))
		&& Baseline.Contains(FString::Printf(TEXT("\"Source\":\"%s\""), DefaultTexture)));

	// 8. The saved package, unloaded and loaded back from disk, extracts to the same document.
	TestReload(*this, Target, Baseline);

	// 9. Failed creates leave nothing behind.
	AddExpectedMessage(TEXT("NoSuchRow"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	TestFailsWith(*this, Apply(*this, NoStruct, Document(NoStruct, DataTableClass, TEXT("Create"), TEXT("\"Rows\":[]")), TEXT("NoStruct")),
		TEXT("DataTable.RowStructRequired"), TEXT("/Body/RowStruct"), TEXT("no row struct"));
	TestNothingCreated(*this, NoStruct, TEXT("no row struct"));
	TestFailsWith(*this, Apply(*this, MissingStruct, Document(MissingStruct, DataTableClass, TEXT("Create"),
		RowsBody(TEXT("/Script/AssetDocumentData.NoSuchRow"), TEXT("[]"))), TEXT("MissingStruct")),
		TEXT("DataTable.RowStructNotFound"), TEXT("/Body/RowStruct"), TEXT("unknown row struct"));
	TestNothingCreated(*this, MissingStruct, TEXT("unknown row struct"));
	TestFailsWith(*this, Apply(*this, InvalidStruct, Document(InvalidStruct, DataTableClass, TEXT("Create"),
		RowsBody(NotARow, TEXT("[{\"Name\":\"A\"}]"))), TEXT("InvalidStruct")),
		TEXT("DataTable.InvalidRowStruct"), TEXT("/Body/RowStruct"), TEXT("not a row struct"));
	TestNothingCreated(*this, InvalidStruct, TEXT("not a row struct"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDataTableUserDefinedStructTest, "UEAssetDoc.DataTable.UserDefinedStruct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentDataTableUserDefinedStructTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentDataTablePythonTests;
	const FTarget Struct(TEXT("S_Row"));
	const FTarget Target(TEXT("DT_UserStruct"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		Struct.Cleanup();
	};

	// A User Defined Struct as the editor makes it: Ready (bool) and Score (int).
	UPackage* Package = CreatePackage(*Struct.PackageName);
	UUserDefinedStruct* UserStruct = FStructureEditorUtils::CreateUserDefinedStruct(Package, FName(*Struct.AssetName),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("User Defined Struct"), UserStruct))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(UserStruct);
	FStructureEditorUtils::AddVariable(UserStruct, FEdGraphPinType(TEXT("int"), NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType()));
	TArray<FStructVariableDescription>& Variables = FStructureEditorUtils::GetVarDesc(UserStruct);
	if (!TestEqual(TEXT("two variables"), Variables.Num(), 2))
	{
		return false;
	}
	FStructureEditorUtils::RenameVariable(UserStruct, Variables[0].VarGuid, TEXT("Ready"));
	FStructureEditorUtils::RenameVariable(UserStruct, Variables[1].VarGuid, TEXT("Score"));
	TestTrue(TEXT("struct saved"), UEditorLoadingAndSavingUtils::SavePackages({Package}, false));
	ReleaseUndoHistory();

	const FString Created = Document(Target, DataTableClass, TEXT("Create"), RowsBody(*Struct.ObjectPath, TEXT(
		"[{\"Name\":\"First\",\"Values\":{\"Ready\":true,\"Score\":7}},{\"Name\":\"Second\",\"Values\":{\"Score\":-2}}]")));
	if (!TestTrue(TEXT("create applies"), Apply(*this, Target, Created, TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UDataTable* Table = Target.Get<UDataTable>();
	TestTrue(TEXT("row struct is the User Defined Struct"), Table != nullptr && Table->GetRowStruct() == UserStruct);
	TestNoDiff(*this, Created, TEXT("after create"));
	const TSharedPtr<FJsonObject> Extracted = Extract(*this, Target, TEXT("extract"));
	const TArray<TSharedPtr<FJsonValue>> Rows = BodyArray(Extracted, TEXT("Rows"));
	const TSharedPtr<FJsonObject>* Values = nullptr;
	TestTrue(TEXT("fields use the friendly names"), Rows.Num() == 2 && Rows[0]->AsObject()->TryGetObjectField(TEXT("Values"), Values)
		&& (*Values)->GetIntegerField(TEXT("Score")) == 7 && (*Values)->GetBoolField(TEXT("Ready")));
	const FString Baseline = TestNoOpReapply(*this, Target, TEXT("Reapply"));
	TestFailsWith(*this, Apply(*this, Target, Document(Target, DataTableClass, TEXT("Update"), TEXT(
		"\"Rows\":[{\"Name\":\"First\",\"Values\":{\"MemberVar_1\":1}}]")), TEXT("InternalName")),
		TEXT("DataTable.UnknownField"), TEXT("/Body/Rows/0/Values/MemberVar_1"), TEXT("internal member name"));
	TestReload(*this, Target, Baseline);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentCompositeDataTableTest, "UEAssetDoc.DataTable.Composite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCompositeDataTableTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentDataTablePythonTests;
	const FTarget Composite(TEXT("CDT_All"));
	const FTarget ParentA(TEXT("DT_A"));
	const FTarget ParentB(TEXT("DT_B"));
	const FTarget Other(TEXT("DT_Other"));
	const FTarget FailedCreate(TEXT("CDT_Failed"));
	ON_SCOPE_EXIT
	{
		FailedCreate.Cleanup();
		Composite.Cleanup();
		ParentA.Cleanup();
		ParentB.Cleanup();
		Other.Cleanup();
	};
	const bool bParents = TestTrue(TEXT("parent A"), Apply(*this, ParentA, Document(ParentA, DataTableClass, TEXT("Create"), RowsBody(TestRow,
			TEXT("[{\"Name\":\"Sword\",\"Values\":{\"Count\":1}},{\"Name\":\"Bow\",\"Values\":{\"Count\":2}}]"))), TEXT("ParentA")).IsSuccess())
		&& TestTrue(TEXT("parent B"), Apply(*this, ParentB, Document(ParentB, DataTableClass, TEXT("Create"), RowsBody(TestRow,
			TEXT("[{\"Name\":\"Bow\",\"Values\":{\"Count\":20}},{\"Name\":\"Axe\",\"Values\":{\"Count\":30}}]"))), TEXT("ParentB")).IsSuccess())
		&& TestTrue(TEXT("other struct"), Apply(*this, Other, Document(Other, DataTableClass, TEXT("Create"), RowsBody(OtherRow,
			TEXT("[{\"Name\":\"X\"}]"))), TEXT("Other")).IsSuccess());
	if (!bParents)
	{
		return false;
	}

	// 1. Create with the row struct and parents; rows are derived (later parents override).
	const FString Created = Document(Composite, CompositeClass, TEXT("Create"), FString::Printf(
		TEXT("\"RowStruct\":\"%s\",\"ParentTables\":[\"%s\",\"%s\"]"), TestRow, *ParentA.ObjectPath, *ParentB.ObjectPath));
	if (!TestTrue(TEXT("create applies"), Apply(*this, Composite, Created, TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UCompositeDataTable* Table = Composite.Get<UCompositeDataTable>();
	if (!TestNotNull(TEXT("CompositeDataTable exists"), Table))
	{
		return false;
	}
	TestEqual(TEXT("derived rows"), RowNames(Table), FString(TEXT("Sword,Bow,Axe")));
	TestEqual(TEXT("parent B overrides Bow"), Count(Table, TEXT("Bow")), 20);
	TestNoDiff(*this, Created, TEXT("after create"));
	const FString Baseline = TestNoOpReapply(*this, Composite, TEXT("Reapply"));
	TestTrue(TEXT("extraction lists the derived rows"), Baseline.Contains(TEXT("\"Rows\":[{\"Name\":\"Sword\"")));

	// 2. Rows are read-only: a document with other rows fails after the parent edit and rolls back.
	const FAssetDocumentResult ReadOnly = Apply(*this, Composite, Document(Composite, CompositeClass, TEXT("Update"), FString::Printf(
		TEXT("\"ParentTables\":[\"%s\"],\"Rows\":[{\"Name\":\"Sword\",\"Values\":{\"Count\":99}}]"), *ParentA.ObjectPath)), TEXT("RowsReadOnly"));
	TestFailsWith(*this, ReadOnly, TEXT("CompositeDataTable.RowsReadOnly"), TEXT(""), TEXT("rows are read-only"));
	ExtractEquals(*this, Composite, Baseline, TEXT("after read-only rollback"));
	TestEqual(TEXT("parents restored"), Count(Composite.Get<UCompositeDataTable>(), TEXT("Bow")), 20);

	// 3. Parents must share the row struct and exist.
	AddExpectedMessage(TEXT("AssetDocumentDataTablePython/Missing"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	TestFailsWith(*this, Apply(*this, Composite, Document(Composite, CompositeClass, TEXT("Update"), FString::Printf(
		TEXT("\"ParentTables\":[\"%s\",\"%s\"]"), *ParentA.ObjectPath, *Other.ObjectPath)), TEXT("Mismatch")),
		TEXT("Composite.ParentRowStructMismatch"), TEXT("/Body/ParentTables/1"), TEXT("parent row struct mismatch"));
	TestFailsWith(*this, Apply(*this, Composite, Document(Composite, CompositeClass, TEXT("Update"), FString::Printf(
		TEXT("\"ParentTables\":[\"%s\"]"), TEXT("/Game/AssetDocumentDataTablePython/Missing.Missing"))), TEXT("Missing")),
		TEXT("Composite.ParentNotFound"), TEXT("/Body/ParentTables/0"), TEXT("missing parent"));
	TestFailsWith(*this, Apply(*this, Composite, Document(Composite, CompositeClass, TEXT("Update"), FString::Printf(
		TEXT("\"ParentTables\":[\"%s\"]"), *Composite.ObjectPath)), TEXT("Cycle")),
		TEXT("Composite.ParentCycle"), TEXT("/Body/ParentTables/0"), TEXT("self parent"));
	ExtractEquals(*this, Composite, Baseline, TEXT("after parent errors"));

	// 4. Reordering the parents changes which one wins.
	const FString Reordered = Document(Composite, CompositeClass, TEXT("Update"), FString::Printf(
		TEXT("\"ParentTables\":[\"%s\",\"%s\"]"), *ParentB.ObjectPath, *ParentA.ObjectPath));
	if (TestTrue(TEXT("reorder applies"), Apply(*this, Composite, Reordered, TEXT("Reorder")).IsSuccess()))
	{
		TestEqual(TEXT("parent A overrides Bow now"), Count(Composite.Get<UCompositeDataTable>(), TEXT("Bow")), 2);
		TestNoDiff(*this, Reordered, TEXT("after reorder"));
	}
	const FString ReorderedBaseline = TestNoOpReapply(*this, Composite, TEXT("Reapply reordered"));
	TestReload(*this, Composite, ReorderedBaseline);

	// 5. A composite create whose parent does not match fails and leaves nothing.
	TestFailsWith(*this, Apply(*this, FailedCreate, Document(FailedCreate, CompositeClass, TEXT("Create"), FString::Printf(
		TEXT("\"RowStruct\":\"%s\",\"ParentTables\":[\"%s\"]"), TestRow, *Other.ObjectPath)), TEXT("FailedCreate")),
		TEXT("Composite.ParentRowStructMismatch"), TEXT("/Body/ParentTables/0"), TEXT("failed composite create"));
	TestNothingCreated(*this, FailedCreate, TEXT("failed composite create"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMirrorDataTableTest, "UEAssetDoc.DataTable.Mirror",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMirrorDataTableTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentDataTablePythonTests;
	const FTarget Target(TEXT("MDT_Mirror"));
	const FTarget Refused(TEXT("MDT_Refused"));
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		Refused.Cleanup();
	};
	const TCHAR* SkeletonPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
	const FString WithSkeleton = FString::Printf(TEXT(",\"FactoryContext\":\"%s\"}"), SkeletonPath);
	auto MirrorDocument = [&](const FTarget& Which, const TCHAR* Action, const FString& Body, bool bSkeleton, const FString& Properties = TEXT("{}"))
	{
		FString Json = Document(Which, MirrorClass, Action, Body, Properties);
		if (bSkeleton)
		{
			Json.LeftChopInline(1);
			Json += WithSkeleton;
		}
		return Json;
	};

	// The dialog cannot finish without a Skeleton, nor with a struct its picker does not list.
	TestFailsWith(*this, Apply(*this, Refused, MirrorDocument(Refused, TEXT("Create"), TEXT(""), false), TEXT("NoSkeleton")),
		TEXT("MirrorDataTable.SkeletonRequired"), TEXT("/FactoryContext"), TEXT("mirror create without skeleton"));
	TestNothingCreated(*this, Refused, TEXT("mirror create without skeleton"));
	// The struct check reads the created table, so the factory has already run with that struct and
	// UMirrorDataTable logs that its rows are not FMirrorTableRow before the create is rolled back.
	AddExpectedError(TEXT("Incorrect type specified for DataTable"), EAutomationExpectedErrorFlags::Contains, 0);
	TestFailsWith(*this, Apply(*this, Refused, MirrorDocument(Refused, TEXT("Create"), FString::Printf(TEXT("\"RowStruct\":\"%s\""), TestRow), true),
		TEXT("NotMirrorRow")), TEXT("MirrorDataTable.InvalidRowStruct"), TEXT("/Body/RowStruct"), TEXT("mirror create with a non-mirror struct"));
	TestNothingCreated(*this, Refused, TEXT("mirror create with a non-mirror struct"));

	// What the dialog's Create does: UMirrorDataTableFactory with Skeleton and Struct set (default find/replace
	// expressions), on a transient twin.
	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, SkeletonPath);
	UClass* FactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/UnrealEd.MirrorDataTableFactory"));
	if (!TestNotNull(TEXT("skeleton"), Skeleton) || !TestNotNull(TEXT("mirror factory class"), FactoryClass))
	{
		return false;
	}
	UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
	CastFieldChecked<FObjectPropertyBase>(FactoryClass->FindPropertyByName(TEXT("Skeleton")))->SetObjectPropertyValue_InContainer(Factory, Skeleton);
	CastFieldChecked<FObjectPropertyBase>(FactoryClass->FindPropertyByName(TEXT("Struct")))->SetObjectPropertyValue_InContainer(Factory, FMirrorTableRow::StaticStruct());
	UMirrorDataTable* Twin = Cast<UMirrorDataTable>(Factory->FactoryCreateNew(UMirrorDataTable::StaticClass(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UMirrorDataTable::StaticClass(), TEXT("MirrorTwin")), RF_Public | RF_Transient, nullptr, GWarn));
	// Factory and twin are transient: garbage collection takes them after the test (the editor may root a factory
	// while it is in use, so they are not marked as garbage here). The twin is kept alive across the applies below,
	// which collect garbage when they return.
	const TStrongObjectPtr<UMirrorDataTable> KeepTwin(Twin);
	if (!TestNotNull(TEXT("dialog twin created"), Twin))
	{
		return false;
	}
	TestTrue(TEXT("the skeleton yields mirror rows"), Twin->GetRowNames().Num() > 0);

	const FTarget Created(TEXT("MDT_Created"));
	ON_SCOPE_EXIT
	{
		Created.Cleanup();
	};
	const FString CreateDocument = MirrorDocument(Created, TEXT("Create"), TEXT(""), true);
	if (!TestTrue(TEXT("mirror create applies"), Apply(*this, Created, CreateDocument, TEXT("MirrorCreate")).IsSuccess()))
	{
		return false;
	}
	UMirrorDataTable* CreatedTable = Created.Get<UMirrorDataTable>();
	if (!TestNotNull(TEXT("created mirror table"), CreatedTable))
	{
		return false;
	}
	TestTrue(TEXT("skeleton set as the dialog does"), CreatedTable->Skeleton == Skeleton);
	TestTrue(TEXT("row struct is the dialog default"), CreatedTable->RowStruct == FMirrorTableRow::StaticStruct());
	TestEqual(TEXT("same rows as the dialog's create"), RowNames(CreatedTable), RowNames(Twin));
	bool bSameValues = true;
	for (const FName& Name : Twin->GetRowNames())
	{
		const FMirrorTableRow* Want = Twin->FindRow<FMirrorTableRow>(Name, TEXT("AssetDocumentDataTablePythonTests"), false);
		const FMirrorTableRow* Have = CreatedTable->FindRow<FMirrorTableRow>(Name, TEXT("AssetDocumentDataTablePythonTests"), false);
		bSameValues &= Want != nullptr && Have != nullptr && Want->Name == Have->Name && Want->MirroredName == Have->MirroredName
			&& Want->MirrorEntryType == Have->MirrorEntryType;
	}
	TestTrue(TEXT("same row values as the dialog's create"), bSameValues);
	TestEqual(TEXT("same find/replace expressions as the dialog's default"), CreatedTable->MirrorFindReplaceExpressions.Num(), Twin->MirrorFindReplaceExpressions.Num());
	TestNoDiff(*this, CreateDocument, TEXT("after mirror create"));
	const FString CreatedBaseline = TestNoOpReapply(*this, Created, TEXT("MirrorCreateReapply"));

	// Update: one row's mirrored name and a Details property; the other rows stay.
	const FName FirstRow = Twin->GetRowNames()[0];
	const FString EditedDocument = MirrorDocument(Created, TEXT("Update"), FString::Printf(TEXT(
		"\"Rows\":[{\"Name\":\"%s\",\"Values\":{\"MirroredName\":\"custom_bone\"}}]"), *FirstRow.ToString()), false,
		TEXT("{\"MirrorAxis\":\"Y\"}"));
	if (TestTrue(TEXT("mirror update applies"), Apply(*this, Created, EditedDocument, TEXT("MirrorUpdate")).IsSuccess()))
	{
		CreatedTable = Created.Get<UMirrorDataTable>();
		const FMirrorTableRow* Edited = CreatedTable->FindRow<FMirrorTableRow>(FirstRow, TEXT("AssetDocumentDataTablePythonTests"), false);
		TestTrue(TEXT("row edited"), Edited != nullptr && Edited->MirroredName == TEXT("custom_bone"));
		TestEqual(TEXT("only the listed rows remain"), CreatedTable->GetRowNames().Num(), 1);
		TestEqual(TEXT("mirror axis"), static_cast<int32>(CreatedTable->MirrorAxis), static_cast<int32>(EAxis::Y));
		TestNoDiff(*this, EditedDocument, TEXT("after mirror update"));
		TestReload(*this, Created, TestNoOpReapply(*this, Created, TEXT("MirrorUpdateReapply")));
	}
	TestTrue(TEXT("create baseline extracted"), !CreatedBaseline.IsEmpty());

	// An existing Mirror Data Table (as the editor's factory leaves it, without a Skeleton).
	UPackage* Package = CreatePackage(*Target.PackageName);
	UMirrorDataTable* Table = NewObject<UMirrorDataTable>(Package, FName(*Target.AssetName), RF_Public | RF_Standalone | RF_Transactional);
	Table->RowStruct = FMirrorTableRow::StaticStruct();
	FAssetRegistryModule::AssetCreated(Table);
	TestTrue(TEXT("mirror table saved"), UEditorLoadingAndSavingUtils::SavePackages({Package}, false));
	ReleaseUndoHistory();

	const FString Updated = Document(Target, MirrorClass, TEXT("Update"), TEXT(
		"\"Rows\":[{\"Name\":\"Hand\",\"Values\":{\"Name\":\"hand_l\",\"MirroredName\":\"hand_r\"}},"
		"{\"Name\":\"Foot\",\"Values\":{\"Name\":\"foot_l\",\"MirroredName\":\"foot_r\"}}]"));
	if (!TestTrue(TEXT("rows apply"), Apply(*this, Target, Updated, TEXT("Update")).IsSuccess()))
	{
		return false;
	}
	const FMirrorTableRow* Hand = Table->FindRow<FMirrorTableRow>(TEXT("Hand"), TEXT("AssetDocumentDataTablePythonTests"), false);
	TestTrue(TEXT("mirror row"), Hand != nullptr && Hand->Name == TEXT("hand_l") && Hand->MirroredName == TEXT("hand_r"));
	TestNoDiff(*this, Updated, TEXT("after update"));
	const FString Baseline = TestNoOpReapply(*this, Target, TEXT("Reapply"));
	TestReload(*this, Target, Baseline);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentDataTableUndoTest, "UEAssetDoc.DataTable.Undo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentDataTableUndoTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentDataTablePythonTests;
	const FTarget Target(TEXT("DT_Undo"));
	ON_SCOPE_EXIT { Target.Cleanup(); };
	if (GEditor == nullptr || GEditor->Trans == nullptr
		|| !TestTrue(TEXT("create applies"), Apply(*this, Target, Document(Target, DataTableClass, TEXT("Create"), RowsBody(TestRow, InitialRows)),
			TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	ReleaseUndoHistory();
	auto Live = [&Target]() { return Target.Get<UDataTable>(); };
	// Undo (then redo) everything the history holds; the table must stay whole at every step.
	auto UndoRedoAll = [this, &Live](const TCHAR* Stage)
	{
		int32 Undone = 0;
		while (Undone < 16 && GEditor->Trans->CanUndo() && GEditor->UndoTransaction())
		{
			++Undone;
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			TestTrue(*FString::Printf(TEXT("%s: table whole after undo %d"), Stage, Undone),
				Live() != nullptr && Live()->GetRowStruct() == FAssetDocumentDataTestRow::StaticStruct());
		}
		int32 Redone = 0;
		while (Redone < Undone && GEditor->Trans->CanRedo() && GEditor->RedoTransaction())
		{
			++Redone;
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		TestEqual(*FString::Printf(TEXT("%s: every undone step redoes"), Stage), Redone, Undone);
		return Undone;
	};

	// 1. The human adds a row in the Data Table editor and saves.
	TestNotNull(TEXT("human adds a row"), FDataTableEditorUtils::AddRow(Live(), TEXT("Human")));
	TestTrue(TEXT("human save"), UEditorLoadingAndSavingUtils::SavePackages({Live()->GetOutermost()}, false));
	TestTrue(TEXT("the human edit is in the undo history"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));

	// 2. The AI rewrites the rows (Human is not in the document): history referencing the table is cleared.
	if (!TestTrue(TEXT("AI edit applies"), Apply(*this, Target, Document(Target, DataTableClass, TEXT("Update"),
		FString::Printf(TEXT("\"Rows\":%s"), EditedRows)), TEXT("Edit")).IsSuccess()))
	{
		return false;
	}
	TestNull(TEXT("AI edit removed Human"), Row(Live(), TEXT("Human")));
	TestFalse(TEXT("after AI edit: no undo history references the table"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	const FString AfterEdit = Serialize(Extract(*this, Target, TEXT("after AI edit")));
	UndoRedoAll(TEXT("after AI edit"));
	ExtractEquals(*this, Target, AfterEdit, TEXT("after AI edit + undo/redo"));

	// 3. The human edits a row value, then an AI apply fails mid-way and rolls back: the human's
	//    step stays undoable and undoes onto the restored (functionally equal) table.
	ReleaseUndoHistory();
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentDataTableTests", "UserEdit", "Edit Row"));
		UDataTable* Table = Live();
		Table->Modify();
		FDataTableEditorUtils::BroadcastPreChange(Table, FDataTableEditorUtils::EDataTableChangeInfo::RowData);
		const_cast<FAssetDocumentDataTestRow*>(Row(Table, TEXT("Sword")))->Count = 42;
		Table->HandleDataTableChanged(TEXT("Sword"));
		Table->MarkPackageDirty();
		FDataTableEditorUtils::BroadcastPostChange(Table, FDataTableEditorUtils::EDataTableChangeInfo::RowData);
	}
	const int32 HumanSteps = GEditor->Trans->GetQueueLength();
	const FAssetDocumentResult MidFailed = Apply(*this, Target, Document(Target, DataTableClass, TEXT("Update"), TEXT(
		"\"Rows\":[{\"Name\":\"Extra\"},{\"Name\":\"Sword\",\"Values\":{\"Count\":\"many\"}}]")), TEXT("MidApplyFailure"));
	TestFalse(TEXT("mid-apply failure fails"), MidFailed.IsSuccess());
	TestEqual(TEXT("after rollback: the human history is kept"), GEditor->Trans->GetQueueLength(), HumanSteps);
	TestEqual(TEXT("after rollback: Sword keeps the human edit"), Count(Live(), TEXT("Sword")), 42);
	TestEqual(TEXT("after rollback: rows unchanged"), RowNames(Live()), FString(TEXT("Axe,Longbow,Sword")));
	TestTrue(TEXT("Ctrl+Z undoes the human edit"), GEditor->UndoTransaction());
	TestEqual(TEXT("after undo: Sword is back to the AI value"), Count(Live(), TEXT("Sword")), 5);
	ExtractEquals(*this, Target, AfterEdit, TEXT("after rollback + undo"));
	TestTrue(TEXT("Ctrl+Y redoes the human edit"), GEditor->RedoTransaction());
	TestEqual(TEXT("after redo: Sword has the human value"), Count(Live(), TEXT("Sword")), 42);
	return !HasAnyErrors();
}
#endif
