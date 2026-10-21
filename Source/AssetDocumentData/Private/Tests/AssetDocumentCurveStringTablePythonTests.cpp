// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// End-to-end CurveTable / CompositeCurveTable / StringTable through the production Service and
// the Python Profiles (profile.curve_table.v1, profile.composite_curve_table.v1,
// profile.string_table.v1).
//
// UEAssetDoc.CurveTable.Python: create with the curve type (simple curves) and
// curves in one apply; Extract/Diff agree; the extraction re-applies as a no-op; rename, remove,
// add and edit curves; change the curve type (rich curves, rebuilt from Rows); invalid keys and a
// missing curve type fail with the document path and change nothing; reload keeps the document; validate and diff
// name the removed curve.
// UEAssetDoc.CurveTable.Composite: ParentTables authored, curves derived and
// read-only (a read-only violation after the parent edit rolls back).
// UEAssetDoc.StringTable.Python: namespace, entries and entry metadata (validate and diff name the
// removed keys); errors;
// the editor's (force) delete removes a saved string table completely (the engine keeps string
// tables alive across package unloads until they are garbage).

#include "AssetDocumentDataTableTestSupport.h"

#include "Curves/RichCurve.h"
#include "Curves/SimpleCurve.h"
#include "Engine/CompositeCurveTable.h"
#include "Engine/CurveTable.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentCurveStringTablePythonTests
{
using namespace AssetDocumentDataTableTests;

const TCHAR* CurveTableClass = TEXT("/Script/Engine.CurveTable");
const TCHAR* CompositeCurveTableClass = TEXT("/Script/Engine.CompositeCurveTable");
const TCHAR* StringTableClass = TEXT("/Script/Engine.StringTable");

FString CurveNames(const UCurveTable* Table)
{
	TArray<FString> Names;
	if (Table != nullptr)
	{
		for (const TPair<FName, FRealCurve*>& Pair : Table->GetRowMap())
		{
			Names.Add(Pair.Key.ToString());
		}
	}
	Names.Sort();
	return FString::Join(Names, TEXT(","));
}

float Eval(const UCurveTable* Table, const TCHAR* Name, float Time)
{
	const FRealCurve* Curve = Table != nullptr ? Table->GetRowMap().FindRef(FName(Name)) : nullptr;
	return Curve != nullptr ? Curve->Eval(Time) : -12345.f;
}

FString Source(const UStringTable* Table, const TCHAR* Key)
{
	FString Value;
	return Table != nullptr && Table->GetStringTable()->GetSourceString(FTextKey(Key), Value) ? Value : FString(TEXT("<missing>"));
}

const TCHAR* SimpleRows = TEXT(R"json(
"CurveTableMode":"SimpleCurves","Rows":[
  {"Name":"Damage","InterpMode":"Linear","Keys":[{"Time":1,"Value":10},{"Time":10,"Value":55.5}]},
  {"Name":"Speed","InterpMode":"Constant","Keys":[{"Time":0,"Value":0.3},{"Time":5,"Value":0.7}],"PostInfinityExtrap":"Linear"}])json");
} // namespace AssetDocumentCurveStringTablePythonTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentCurveTablePythonTest, "UEAssetDoc.CurveTable.Python",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCurveTablePythonTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentCurveStringTablePythonTests;
	const FTarget Target(TEXT("CT_Stats"));
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// 1. One apply creates the table with its curve type and curves.
	const FString Created = Document(Target, CurveTableClass, TEXT("Create"), SimpleRows);
	if (!TestTrue(TEXT("create applies"), Apply(*this, Target, Created, TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UCurveTable* Table = Target.Get<UCurveTable>();
	if (!TestNotNull(TEXT("CurveTable exists"), Table))
	{
		return false;
	}
	TestTrue(TEXT("simple curves"), Table->GetCurveTableMode() == ECurveTableMode::SimpleCurves);
	TestEqual(TEXT("curves"), CurveNames(Table), FString(TEXT("Damage,Speed")));
	TestEqual(TEXT("linear curve"), Eval(Table, TEXT("Damage"), 10.f), 55.5f);
	TestEqual(TEXT("constant curve"), Eval(Table, TEXT("Speed"), 4.f), 0.3f);
	TestNoDiff(*this, Created, TEXT("after create"));
	FString Baseline = TestNoOpReapply(*this, Target, TEXT("Reapply"));

	// 2. Rename Speed -> Velocity (and rekey it), remove Damage, add Range.
	const FString Edited = Document(Target, CurveTableClass, TEXT("Update"), TEXT(R"json(
"CurveTableMode":"SimpleCurves","Rows":[
  {"Name":"Velocity","RenamedFrom":"Speed","InterpMode":"Constant","Keys":[{"Time":0,"Value":0.3},{"Time":5,"Value":0.9}],"PostInfinityExtrap":"Linear"},
  {"Name":"Range","Keys":[{"Time":0,"Value":100}]}])json"));
	// Validate and diff count the removed curve (Damage); the renamed one (RenamedFrom Speed) is kept, not removed.
	{
		const FAssetDocumentResult Summary = ValidateDocument(*this, Edited, TEXT("validate edit"));
		TestTrue(*FString::Printf(TEXT("validate names the removed curve (%s)"), *Summary.Message), Summary.IsSuccess()
			&& Summary.Message.Contains(TEXT("removing 1 entry")) && Summary.Message.Contains(TEXT("(Damage)")) && !Summary.Message.Contains(TEXT("(Speed)")));
		FAssetDocumentDiffRequest Request;
		Request.Document = Parse(Edited);
		const FAssetDocumentResult Diffed = FAssetDocumentModule::Get().GetService().Diff(Request);
		const TArray<TSharedPtr<FJsonValue>>* Removals = nullptr;
		TestTrue(*FString::Printf(TEXT("diff lists the removal (%s)"), *Diffed.Message), Diffed.IsSuccess() && Diffed.Payload.IsValid()
			&& Diffed.Payload->TryGetArrayField(TEXT("removals"), Removals) && Removals->Num() == 1 && Diffed.Message.Contains(TEXT("Damage")));
	}
	if (TestTrue(TEXT("edit applies"), Apply(*this, Target, Edited, TEXT("Edit")).IsSuccess()))
	{
		TestEqual(TEXT("curves renamed, removed and added"), CurveNames(Table), FString(TEXT("Range,Velocity")));
		TestEqual(TEXT("renamed curve rekeyed"), Eval(Table, TEXT("Velocity"), 5.f), 0.9f);
		TestNoDiff(*this, Edited, TEXT("after edit"));
	}
	Baseline = TestNoOpReapply(*this, Target, TEXT("Reapply edited"));

	// 3. Errors name the document location and change nothing.
	TestFailsWith(*this, Apply(*this, Target, Document(Target, CurveTableClass, TEXT("Update"), TEXT(
		"\"CurveTableMode\":\"SimpleCurves\",\"Rows\":[{\"Name\":\"Range\",\"Keys\":[{\"Time\":2,\"Value\":1},{\"Time\":1,\"Value\":2}]}]")), TEXT("NotAscending")),
		TEXT("CurveTable.KeysNotAscending"), TEXT("/Body/Rows/0/Keys/1/Time"), TEXT("keys not ascending"));
	TestFailsWith(*this, Apply(*this, Target, Document(Target, CurveTableClass, TEXT("Update"), TEXT(
		"\"CurveTableMode\":\"SimpleCurves\",\"Rows\":[{\"Name\":\"Range\",\"InterpMode\":\"Cubic\",\"Keys\":[]}]")), TEXT("CubicSimple")),
		TEXT("CurveTable.InvalidInterpMode"), TEXT("/Body/Rows/0/InterpMode"), TEXT("cubic simple curve"));
	TestFailsWith(*this, Apply(*this, Target, Document(Target, CurveTableClass, TEXT("Update"), TEXT(
		"\"Rows\":[{\"Name\":\"Range\",\"Keys\":[]}]")), TEXT("ModeRequired")),
		TEXT("CurveTable.ModeRequired"), TEXT("/Body/CurveTableMode"), TEXT("rows without the curve type"));
	TestFailsWith(*this, Apply(*this, Target, Document(Target, CurveTableClass, TEXT("Update"), TEXT(
		"\"CurveTableMode\":\"RichCurves\"")), TEXT("ModeChangeNeedsRows")),
		TEXT("CurveTable.ModeChangeNeedsRows"), TEXT("/Body/CurveTableMode"), TEXT("type change without rows"));
	ExtractEquals(*this, Target, Baseline, TEXT("after errors"));

	// 4. Changing the curve type rebuilds the table from Rows (rich curves, cubic keys).
	const FString Rich = Document(Target, CurveTableClass, TEXT("Update"), TEXT(R"json(
"CurveTableMode":"RichCurves","Rows":[
  {"Name":"Range","Keys":[{"Time":0,"Value":100,"InterpMode":"Cubic","TangentMode":"User","LeaveTangent":2},{"Time":10,"Value":200}],
   "DefaultValue":5}])json"));
	if (TestTrue(TEXT("type change applies"), Apply(*this, Target, Rich, TEXT("Rich")).IsSuccess()))
	{
		Table = Target.Get<UCurveTable>();
		TestTrue(TEXT("rich curves"), Table->GetCurveTableMode() == ECurveTableMode::RichCurves);
		TestEqual(TEXT("one curve"), CurveNames(Table), FString(TEXT("Range")));
		const FRichCurve* Range = Table->GetRichCurveRowMap().FindRef(TEXT("Range"));
		TestTrue(TEXT("cubic user key"), Range != nullptr && Range->GetNumKeys() == 2 && Range->Keys[0].InterpMode == RCIM_Cubic
			&& Range->Keys[0].TangentMode == RCTM_User && Range->Keys[0].LeaveTangent == 2.f && Range->DefaultValue == 5.f);
		TestNoDiff(*this, Rich, TEXT("after type change"));
	}
	Baseline = TestNoOpReapply(*this, Target, TEXT("Reapply rich"));

	// 5. Reload; then Rows [] empties the table.
	TestReload(*this, Target, Baseline);
	const FString Emptied = Document(Target, CurveTableClass, TEXT("Update"), TEXT("\"Rows\":[]"));
	if (TestTrue(TEXT("empty applies"), Apply(*this, Target, Emptied, TEXT("Empty")).IsSuccess()))
	{
		TestEqual(TEXT("no curves"), CurveNames(Target.Get<UCurveTable>()), FString());
		TestNoDiff(*this, Emptied, TEXT("after empty"));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentCompositeCurveTableTest, "UEAssetDoc.CurveTable.Composite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentCompositeCurveTableTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentCurveStringTablePythonTests;
	const FTarget Composite(TEXT("CCT_All"));
	const FTarget ParentA(TEXT("CT_A"));
	const FTarget ParentB(TEXT("CT_B"));
	ON_SCOPE_EXIT
	{
		Composite.Cleanup();
		ParentA.Cleanup();
		ParentB.Cleanup();
	};
	const bool bParents = TestTrue(TEXT("parent A"), Apply(*this, ParentA, Document(ParentA, CurveTableClass, TEXT("Create"), TEXT(
			"\"CurveTableMode\":\"SimpleCurves\",\"Rows\":[{\"Name\":\"Damage\",\"Keys\":[{\"Time\":0,\"Value\":1}]}]")), TEXT("ParentA")).IsSuccess())
		&& TestTrue(TEXT("parent B"), Apply(*this, ParentB, Document(ParentB, CurveTableClass, TEXT("Create"), TEXT(
			"\"CurveTableMode\":\"SimpleCurves\",\"Rows\":[{\"Name\":\"Damage\",\"Keys\":[{\"Time\":0,\"Value\":2}]},"
			"{\"Name\":\"Armor\",\"Keys\":[{\"Time\":0,\"Value\":3}]}]")), TEXT("ParentB")).IsSuccess());
	if (!bParents)
	{
		return false;
	}
	const FString Created = Document(Composite, CompositeCurveTableClass, TEXT("Create"), FString::Printf(
		TEXT("\"ParentTables\":[\"%s\",\"%s\"]"), *ParentA.ObjectPath, *ParentB.ObjectPath));
	if (!TestTrue(TEXT("create applies"), Apply(*this, Composite, Created, TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UCompositeCurveTable* Table = Composite.Get<UCompositeCurveTable>();
	TestEqual(TEXT("derived curves"), CurveNames(Table), FString(TEXT("Armor,Damage")));
	TestEqual(TEXT("parent B overrides Damage"), Eval(Table, TEXT("Damage"), 0.f), 2.f);
	TestNoDiff(*this, Created, TEXT("after create"));
	const FString Baseline = TestNoOpReapply(*this, Composite, TEXT("Reapply"));

	// Curves are read-only: a document with other curves fails after the parent edit and rolls back.
	TestFailsWith(*this, Apply(*this, Composite, Document(Composite, CompositeCurveTableClass, TEXT("Update"), FString::Printf(
		TEXT("\"ParentTables\":[\"%s\"],\"CurveTableMode\":\"SimpleCurves\",\"Rows\":[{\"Name\":\"Damage\",\"Keys\":[{\"Time\":0,\"Value\":9}]}]"),
		*ParentA.ObjectPath)), TEXT("RowsReadOnly")), TEXT("CompositeCurveTable.RowsReadOnly"), TEXT(""), TEXT("curves are read-only"));
	ExtractEquals(*this, Composite, Baseline, TEXT("after read-only rollback"));
	TestEqual(TEXT("parents restored"), Eval(Composite.Get<UCompositeCurveTable>(), TEXT("Damage"), 0.f), 2.f);

	const FString Reordered = Document(Composite, CompositeCurveTableClass, TEXT("Update"), FString::Printf(
		TEXT("\"ParentTables\":[\"%s\",\"%s\"]"), *ParentB.ObjectPath, *ParentA.ObjectPath));
	if (TestTrue(TEXT("reorder applies"), Apply(*this, Composite, Reordered, TEXT("Reorder")).IsSuccess()))
	{
		TestEqual(TEXT("parent A overrides Damage now"), Eval(Composite.Get<UCompositeCurveTable>(), TEXT("Damage"), 0.f), 1.f);
		TestNoDiff(*this, Reordered, TEXT("after reorder"));
	}
	TestReload(*this, Composite, TestNoOpReapply(*this, Composite, TEXT("Reapply reordered")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentStringTablePythonTest, "UEAssetDoc.StringTable.Python",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentStringTablePythonTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentCurveStringTablePythonTests;
	const FTarget Target(TEXT("ST_Menu"));
	const FTarget Failed(TEXT("ST_Failed"));
	// The registry's file watcher notices the force-deleted package file going away.
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		Failed.Cleanup();
	};

	// 1. One apply creates the table with its namespace, entries and metadata.
	const FString Created = Document(Target, StringTableClass, TEXT("Create"), TEXT(R"json(
"Namespace":"UI","Entries":{"Menu.Start":"Start Game","Menu.Quit":"Quit"},
"MetaData":{"Menu.Start":{"Comment":"Main menu button"}})json"));
	if (!TestTrue(TEXT("create applies"), Apply(*this, Target, Created, TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UStringTable* Table = Target.Get<UStringTable>();
	if (!TestNotNull(TEXT("StringTable exists"), Table))
	{
		return false;
	}
	TestEqual(TEXT("namespace"), Table->GetStringTable()->GetNamespace(), FString(TEXT("UI")));
	TestEqual(TEXT("entry"), Source(Table, TEXT("Menu.Start")), FString(TEXT("Start Game")));
	TestEqual(TEXT("metadata"), Table->GetStringTable()->GetMetaData(FTextKey(TEXT("Menu.Start")), TEXT("Comment")), FString(TEXT("Main menu button")));
	TestNoDiff(*this, Created, TEXT("after create"));
	FString Baseline = TestNoOpReapply(*this, Target, TEXT("Reapply"));

	// 2. Change the namespace, remove an entry, add one, edit a source string, move metadata.
	const FString Edited = Document(Target, StringTableClass, TEXT("Update"), TEXT(R"json(
"Namespace":"Menu","Entries":{"Menu.Start":"Play","Menu.Options":"Options"},
"MetaData":{"Menu.Options":{"Comment":"Settings","Context":"Main menu"}})json"));
	// Entries and MetaData are complete maps: validate and diff count and name the keys the edit removes: the
	// entry Menu.Quit and Menu.Start's metadata (Menu.Quit's own metadata goes with it, not counted twice).
	{
		const FAssetDocumentResult Summary = ValidateDocument(*this, Edited, TEXT("validate edit"));
		TestTrue(*FString::Printf(TEXT("validate names the removed keys (%s)"), *Summary.Message), Summary.IsSuccess()
			&& Summary.Message.Contains(TEXT("removing 2 entries")) && Summary.Message.Contains(TEXT("/Body/Entries/Menu.Quit (Menu.Quit)"))
			&& Summary.Message.Contains(TEXT("/Body/MetaData/Menu.Start (Menu.Start)")));
		FAssetDocumentDiffRequest Request;
		Request.Document = Parse(Edited);
		const FAssetDocumentResult Diffed = FAssetDocumentModule::Get().GetService().Diff(Request);
		const TArray<TSharedPtr<FJsonValue>>* Removals = nullptr;
		TestTrue(*FString::Printf(TEXT("diff lists the removals (%s)"), *Diffed.Message), Diffed.IsSuccess() && Diffed.Payload.IsValid()
			&& Diffed.Payload->TryGetArrayField(TEXT("removals"), Removals) && Removals->Num() == 2 && Diffed.Message.Contains(TEXT("Menu.Quit")));
	}
	if (TestTrue(TEXT("edit applies"), Apply(*this, Target, Edited, TEXT("Edit")).IsSuccess()))
	{
		TestEqual(TEXT("namespace changed"), Table->GetStringTable()->GetNamespace(), FString(TEXT("Menu")));
		TestEqual(TEXT("source edited"), Source(Table, TEXT("Menu.Start")), FString(TEXT("Play")));
		TestEqual(TEXT("entry removed"), Source(Table, TEXT("Menu.Quit")), FString(TEXT("<missing>")));
		TestEqual(TEXT("entry added"), Source(Table, TEXT("Menu.Options")), FString(TEXT("Options")));
		TestTrue(TEXT("metadata removed"), Table->GetStringTable()->GetMetaData(FTextKey(TEXT("Menu.Start")), TEXT("Comment")).IsEmpty());
		TestEqual(TEXT("metadata added"), Table->GetStringTable()->GetMetaData(FTextKey(TEXT("Menu.Options")), TEXT("Context")), FString(TEXT("Main menu")));
		TestNoDiff(*this, Edited, TEXT("after edit"));
	}
	Baseline = TestNoOpReapply(*this, Target, TEXT("Reapply edited"));
	const FString NamespaceOnly = Document(Target, StringTableClass, TEXT("Update"), TEXT("\"Namespace\":\"Front\""));
	if (TestTrue(TEXT("namespace-only edit applies"), Apply(*this, Target, NamespaceOnly, TEXT("Namespace")).IsSuccess()))
	{
		TestEqual(TEXT("namespace-only edit"), Table->GetStringTable()->GetNamespace(), FString(TEXT("Front")));
		TestEqual(TEXT("entries kept"), Source(Table, TEXT("Menu.Options")), FString(TEXT("Options")));
	}
	Baseline = TestNoOpReapply(*this, Target, TEXT("Reapply namespace"));

	// 3. Errors name the document location and change nothing.
	TestFailsWith(*this, Apply(*this, Target, Document(Target, StringTableClass, TEXT("Update"), TEXT(
		"\"Entries\":{\"Bad Key\":\"x\"}")), TEXT("InvalidKey")), TEXT("StringTable.InvalidKey"), TEXT("/Body/Entries/Bad Key"), TEXT("invalid key"));
	TestFailsWith(*this, Apply(*this, Target, Document(Target, StringTableClass, TEXT("Update"), TEXT(
		"\"Entries\":{\"Menu.Start\":\"\"}")), TEXT("EmptySource")), TEXT("StringTable.EmptySourceString"), TEXT("/Body/Entries/Menu.Start"), TEXT("empty source"));
	TestFailsWith(*this, Apply(*this, Target, Document(Target, StringTableClass, TEXT("Update"), TEXT(
		"\"MetaData\":{\"Menu.Missing\":{\"Comment\":\"x\"}}")), TEXT("MetaKey")), TEXT("StringTable.KeyNotFound"), TEXT("/Body/MetaData/Menu.Missing"), TEXT("metadata key"));
	TestFailsWith(*this, Apply(*this, Target, Document(Target, StringTableClass, TEXT("Update"), TEXT("\"Namespace\":\"a b\"")), TEXT("Namespace")),
		TEXT("StringTable.InvalidNamespace"), TEXT("/Body/Namespace"), TEXT("invalid namespace"));
	TestFailsWith(*this, Apply(*this, Target, Document(Target, StringTableClass, TEXT("Update"), TEXT(""), TEXT("{\"Foo\":1}")), TEXT("Properties")),
		TEXT("StringTable.UnsupportedProperties"), TEXT("/Properties"), TEXT("properties"));
	ExtractEquals(*this, Target, Baseline, TEXT("after errors"));
	TestFalse(TEXT("package clean after errors"), Table->GetOutermost()->IsDirty());

	// 4. A failed create leaves nothing behind.
	TestFailsWith(*this, Apply(*this, Failed, Document(Failed, StringTableClass, TEXT("Create"), TEXT("\"Entries\":{\"Ok\":\"\"}")), TEXT("FailedCreate")),
		TEXT("StringTable.EmptySourceString"), TEXT("/Body/Entries/Ok"), TEXT("failed create"));
	TestNothingCreated(*this, Failed, TEXT("failed create"));

	// 5. The editor's Force Delete removes the saved table completely (unload alone would not, and the
	//    plain delete reports the bridge's in-memory reference: the engine's string table bridge keeps
	//    string tables alive until they are garbage).
	Target.Cleanup();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestNothingCreated(*this, Target, TEXT("after delete"));
	return !HasAnyErrors();
}
#endif
