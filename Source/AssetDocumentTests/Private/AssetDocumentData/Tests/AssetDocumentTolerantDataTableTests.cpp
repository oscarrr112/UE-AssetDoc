// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An editor-made Data Table whose row holds a value the document codec has no form for (an instanced object, set by
// a script or a details-panel paste). It reads with a warning and a marker, its own extract re-applies as a no-op,
// another edit of the same row and of another row works and keeps the object, and an edited marker is refused.

#include "AssetDocumentDataTestRows.h"
#include "AssetDocumentStateTreeTestTypes.h"
#include "AssetDocumentTolerantTestSupport.h"

#include "Engine/DataTable.h"
#include "IAssetTools.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentTolerantDataTableTestsLocal
{
TSharedPtr<FJsonObject> RowValues(const TSharedPtr<FJsonObject>& Document, int32 Index)
{
	return Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("Rows"))[Index]->AsObject()->GetObjectField(TEXT("Values"));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantDataTableTest, "UEAssetDoc.Tolerant.DataTable.Instanced",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantDataTableTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantDataTableTestsLocal;
	const FTarget Target(TEXT("DT_Tolerant"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UDataTable* Table = Target.Create<UDataTable>();
	Table->RowStruct = FAssetDocumentDataTestRow::StaticStruct();
	FAssetDocumentDataTestRow Axe;
	Axe.Count = 1;
	UAssetDocumentStateTreeTestConfig* Inline = NewObject<UAssetDocumentStateTreeTestConfig>(Table, NAME_None, RF_Transactional);
	Inline->Range = 3.f;
	Axe.Inline = Inline;
	Table->AddRow(TEXT("Axe"), Axe);
	FAssetDocumentDataTestRow Bow;
	Bow.Count = 2;
	Table->AddRow(TEXT("Bow"), Bow);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const FString Marker = TEXT("/Body/Rows/0/Values/Inline");
	FExpected Expected;
	Expected.Warnings = {{TEXT("DataTable.Unrepresented"), Marker}};
	Expected.Markers = {Marker};
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, Expected, TEXT("Data Table"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestFalse(TEXT("an empty instanced field is left out"), RowValues(Extracted, 1)->HasField(TEXT("Inline")));

	if (TestTrue(TEXT("both rows change"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		RowValues(Document, 0)->SetNumberField(TEXT("Count"), 7);
		RowValues(Document, 1)->SetNumberField(TEXT("Count"), 5);
	}, TEXT("Edit")).IsSuccess()))
	{
		Table = Target.Get<UDataTable>();
		const FAssetDocumentDataTestRow* Edited = Table != nullptr ? Table->FindRow<FAssetDocumentDataTestRow>(TEXT("Axe"), TEXT("")) : nullptr;
		const FAssetDocumentDataTestRow* Other = Table != nullptr ? Table->FindRow<FAssetDocumentDataTestRow>(TEXT("Bow"), TEXT("")) : nullptr;
		TestTrue(TEXT("counts written"), Edited != nullptr && Edited->Count == 7 && Other != nullptr && Other->Count == 5);
		const UAssetDocumentStateTreeTestConfig* Kept = Edited != nullptr ? Cast<UAssetDocumentStateTreeTestConfig>(Edited->Inline) : nullptr;
		TestTrue(TEXT("the instanced object kept"), Kept != nullptr && Kept->Range == 3.f);
		TestStillShows(*this, Target, TEXT("DataTable.Unrepresented"), Marker, TEXT("After edit"));
	}

	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		RowValues(Document, 0)->GetObjectField(TEXT("Inline"))->SetStringField(TEXT("Text"), TEXT("None"));
	}, TEXT("Edited marker")), TEXT("DataTable.UnrepresentedValue"), Marker, TEXT("Edited marker"));
	return !HasAnyErrors();
}

// A Data Table moved to another folder (the Content Browser's move: IAssetTools::RenameAssets): the marker of its
// instanced object names the object by the table's path, so the extract made before the move holds the old path. Re-
// targeted to the new path, that extract diffs empty, validates as no change and applies without a verification
// mismatch.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantDataTableMovedTest, "UEAssetDoc.Tolerant.DataTable.Moved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantDataTableMovedTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	const FTarget Target(TEXT("DT_TolerantMoved"));
	FTarget Moved(TEXT("DT_TolerantMoved"));
	const FString MovedFolder = FString(TEXT("/Game/")) + FolderName + TEXT("/Moved");
	Moved.AssetName = Target.AssetName;
	Moved.PackageName = MovedFolder + TEXT("/") + Target.AssetName;
	Moved.ObjectPath = Moved.PackageName + TEXT(".") + Target.AssetName;
	Moved.PackageFile = FPackageName::LongPackageNameToFilename(Moved.PackageName, FPackageName::GetAssetPackageExtension());
	Moved.SidecarPath = FPaths::Combine(FPaths::ProjectContentDir(), FolderName, TEXT("Moved"), Target.AssetName + TEXT(".assetdoc.json"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT
	{
		Moved.Cleanup();
		Target.Cleanup();
	};

	UDataTable* Table = Target.Create<UDataTable>();
	Table->RowStruct = FAssetDocumentDataTestRow::StaticStruct();
	FAssetDocumentDataTestRow Axe;
	Axe.Count = 1;
	UAssetDocumentStateTreeTestConfig* Inline = NewObject<UAssetDocumentStateTreeTestConfig>(Table, NAME_None, RF_Transactional);
	Inline->Range = 3.f;
	Axe.Inline = Inline;
	Table->AddRow(TEXT("Axe"), Axe);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}
	const FString Marker = TEXT("/Body/Rows/0/Values/Inline");
	const FAssetDocumentResult Extracted = ExtractResult(Target);
	if (!TestTrue(TEXT("extract succeeds"), Extracted.IsSuccess() && Extracted.Payload.IsValid()))
	{
		LogResult(*this, TEXT("Extract"), Extracted);
		return false;
	}
	FString Text;
	const TSharedPtr<FJsonValue> Read = At(Extracted.Payload, Marker);
	if (!TestTrue(TEXT("the marker names the object by the table's path"), Read.IsValid() && Read->Type == EJson::Object
		&& Read->AsObject()->TryGetStringField(TEXT("Text"), Text) && Text.Contains(Target.ObjectPath + TEXT(":"))))
	{
		AddInfo(Serialize(Extracted.Payload));
	}

	TArray<FAssetRenameData> Renames;
	Renames.Add(FAssetRenameData(TWeakObjectPtr<UObject>(Table), MovedFolder, Target.AssetName));
	if (!TestTrue(TEXT("moved"), IAssetTools::Get().RenameAssets(Renames) && Moved.Get<UDataTable>() == Table)
		|| !TestTrue(TEXT("moved table saved"), Moved.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Document = Parse(Serialize(Extracted.Payload));
	Document->SetStringField(TEXT("Target"), Moved.PackageName);
	Document->SetStringField(TEXT("Action"), TEXT("Update"));
	const FString UpdateText = Serialize(Document);

	FAssetDocumentDiffRequest DiffRequest;
	DiffRequest.Document = Parse(UpdateText);
	const FAssetDocumentResult Diffed = FAssetDocumentModule::Get().GetService().Diff(DiffRequest);
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	if (!TestTrue(TEXT("diff of the old extract is empty"), Diffed.IsSuccess() && Diffed.Payload.IsValid()
		&& Diffed.Payload->TryGetArrayField(TEXT("changed"), Changed) && Changed->IsEmpty()))
	{
		LogResult(*this, TEXT("Diff"), Diffed);
		if (Diffed.Payload.IsValid())
		{
			AddInfo(Serialize(Diffed.Payload));
		}
	}

	FAssetDocumentValidateRequest ValidateRequest;
	ValidateRequest.Document = Parse(UpdateText);
	const FAssetDocumentResult Validated = FAssetDocumentModule::Get().GetService().Validate(ValidateRequest);
	if (!TestTrue(TEXT("validate of the old extract changes nothing"), Validated.IsSuccess() && Validated.Message.Contains(TEXT("would change nothing"))))
	{
		LogResult(*this, TEXT("Validate"), Validated);
	}

	const FAssetDocumentResult Applied = ApplyText(*this, Moved, UpdateText, TEXT("Apply old extract"));
	TestTrue(TEXT("the old extract applies"), Applied.IsSuccess());
	TestFalse(TEXT("no verification mismatch"), HasDiagnostic(Applied, TEXT("GenericRuntime.VerificationMismatch"), FString()));
	const FAssetDocumentDataTestRow* Kept = Moved.Get<UDataTable>() != nullptr
		? Moved.Get<UDataTable>()->FindRow<FAssetDocumentDataTestRow>(TEXT("Axe"), TEXT("")) : nullptr;
	const UAssetDocumentStateTreeTestConfig* Object = Kept != nullptr ? Cast<UAssetDocumentStateTreeTestConfig>(Kept->Inline) : nullptr;
	TestTrue(TEXT("the instanced object kept"), Object != nullptr && Object->Range == 3.f);
	return !HasAnyErrors();
}

// Row fields holding a struct with a member the codec has no form for (a field path), alone and in an array: the
// marker stands inside the value. Editing a sibling member keeps the path; leaving the field out keeps it; removing an
// array element keeps each remaining element's own path; an element that leaves its path out where the current
// element at its index holds one, or that carries an edited marker, is refused and never takes another element's path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantDataTableNestedTest, "UEAssetDoc.Tolerant.DataTable.Nested",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantDataTableNestedTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantDataTableTestsLocal;
	const FTarget Target(TEXT("DT_TolerantNested"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	UScriptStruct* const Named = FAssetDocumentDataTestRow::StaticStruct();
	const TFieldPath<FProperty> CountPath(Named->FindPropertyByName(GET_MEMBER_NAME_CHECKED(FAssetDocumentDataTestRow, Count)));
	const TFieldPath<FProperty> WeightPath(Named->FindPropertyByName(GET_MEMBER_NAME_CHECKED(FAssetDocumentDataTestRow, Weight)));
	const TFieldPath<FProperty> LabelPath(Named->FindPropertyByName(GET_MEMBER_NAME_CHECKED(FAssetDocumentDataTestRow, Label)));
	auto Entry = [](float X, const TFieldPath<FProperty>& Path)
	{
		FAssetDocumentDataTestMarkedStruct Out;
		Out.X = X;
		Out.P = Path;
		return Out;
	};
	auto Row = [&Target]() -> const FAssetDocumentDataTestMarkedRow*
	{
		UDataTable* Current = Target.Get<UDataTable>();
		return Current != nullptr ? Current->FindRow<FAssetDocumentDataTestMarkedRow>(TEXT("Axe"), TEXT("")) : nullptr;
	};
	auto Holds = [](const FAssetDocumentDataTestMarkedStruct& Value, float X, const TFieldPath<FProperty>& Path)
	{
		return Value.X == X && Value.P.ToString() == Path.ToString();
	};

	UDataTable* Table = Target.Create<UDataTable>();
	Table->RowStruct = FAssetDocumentDataTestMarkedRow::StaticStruct();
	FAssetDocumentDataTestMarkedRow Axe;
	Axe.Count = 1;
	Axe.Settings = Entry(1.f, CountPath);
	Axe.Entries = {Entry(1.f, CountPath), Entry(2.f, WeightPath), Entry(3.f, LabelPath)};
	Table->AddRow(TEXT("Axe"), Axe);
	if (!TestTrue(TEXT("field paths set"), !CountPath.ToString().IsEmpty() && CountPath.ToString() != WeightPath.ToString())
		|| !TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const FString Values = TEXT("/Body/Rows/0/Values");
	FExpected Expected;
	Expected.Markers = {Values + TEXT("/Settings/P"), Values + TEXT("/Entries/0/P"), Values + TEXT("/Entries/2/P")};
	// Each marker inside a value warns at its own path.
	for (const FString& Marker : Expected.Markers)
	{
		Expected.Warnings.Add({TEXT("DataTable.Unrepresented"), Marker});
	}
	if (!TestTolerantRoundTrip(*this, Target, Expected, TEXT("Nested markers")).IsValid())
	{
		return false;
	}

	// A sibling member of the struct edited: the struct is decoded over its current value, so its path stays.
	if (TestTrue(TEXT("sibling edit applies"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		RowValues(Document, 0)->GetObjectField(TEXT("Settings"))->SetNumberField(TEXT("X"), 5);
	}, TEXT("Sibling edit")).IsSuccess()))
	{
		const FAssetDocumentDataTestMarkedRow* Edited = Row();
		TestTrue(TEXT("X written, path kept"), Edited != nullptr && Holds(Edited->Settings, 5.f, CountPath));
	}

	// The struct field left out of the row (with another field edited): it keeps its value, path included.
	if (TestTrue(TEXT("omitted field applies"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		RowValues(Document, 0)->RemoveField(TEXT("Settings"));
		RowValues(Document, 0)->SetNumberField(TEXT("Count"), 9);
	}, TEXT("Omitted field")).IsSuccess()))
	{
		const FAssetDocumentDataTestMarkedRow* Edited = Row();
		TestTrue(TEXT("count written, struct kept"), Edited != nullptr && Edited->Count == 9 && Holds(Edited->Settings, 5.f, CountPath));
	}

	// The first element removed and the paths left out of the others: element 1 would land on element 0's path.
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		TArray<TSharedPtr<FJsonValue>> Entries = RowValues(Document, 0)->GetArrayField(TEXT("Entries"));
		Entries.RemoveAt(0);
		for (const TSharedPtr<FJsonValue>& Element : Entries)
		{
			Element->AsObject()->RemoveField(TEXT("P"));
		}
		RowValues(Document, 0)->SetArrayField(TEXT("Entries"), Entries);
	}, TEXT("Element without its path")), TEXT("DataTable.InvalidValue"), Values + TEXT("/Entries/0"), TEXT("Element without its path"));

	// An element's marker edited: no current element has it.
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		RowValues(Document, 0)->GetArrayField(TEXT("Entries"))[1]->AsObject()->GetObjectField(TEXT("P"))->SetStringField(TEXT("Text"), TEXT("edited"));
	}, TEXT("Edited element marker")), TEXT("DataTable.InvalidValue"), Values + TEXT("/Entries/1"), TEXT("Edited element marker"));
	const FAssetDocumentDataTestMarkedRow* Unchanged = Row();
	TestTrue(TEXT("refused edits leave the elements"), Unchanged != nullptr && Unchanged->Entries.Num() == 3);

	// The first element removed, markers left as read: each remaining element keeps its own path.
	if (TestTrue(TEXT("element removal applies"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		TArray<TSharedPtr<FJsonValue>> Entries = RowValues(Document, 0)->GetArrayField(TEXT("Entries"));
		Entries.RemoveAt(0);
		RowValues(Document, 0)->SetArrayField(TEXT("Entries"), Entries);
	}, TEXT("Element removal")).IsSuccess()))
	{
		const FAssetDocumentDataTestMarkedRow* Edited = Row();
		TestTrue(TEXT("remaining elements keep their own paths"), Edited != nullptr && Edited->Entries.Num() == 2
			&& Holds(Edited->Entries[0], 2.f, WeightPath) && Holds(Edited->Entries[1], 3.f, LabelPath));
	}
	return !HasAnyErrors();
}

#endif
