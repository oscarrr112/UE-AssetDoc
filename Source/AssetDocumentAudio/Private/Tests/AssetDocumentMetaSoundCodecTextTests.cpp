// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// A MetaSound Source whose DisplayName is set from a String Table row in the editor (the asset's General Details,
// String Table picker; the editor's document builder): it reads as LOCTABLE("<table>", "<key>") and the extract
// re-applies as a no-op; a document holding the row's display string instead keeps the String Table reference and
// saves nothing; another string sets a new text and INVTEXT a culture-invariant one; the Description and the graph are
// kept. Keywords picked from String Table rows keep their rows when a keyword is inserted before them (each written
// keyword continues the keyword it names, not the one at its index).

#include "AssetDocumentMetaSoundApplyTestSupport.h"
#include "AssetDocumentTolerantTestSupport.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
const TCHAR* const MetaSoundTextRow = TEXT("Engine");
const TCHAR* const MetaSoundTextRowString = TEXT("Engine loop");
const TCHAR* const MetaSoundTextDescription = TEXT("Plays one random sound");
const TCHAR* const MetaSoundTextPointer = TEXT("/Body/Metadata/DisplayName");
const TCHAR* const MetaSoundTextKeywordRows[] = {TEXT("Loop"), TEXT("Impact")};
const TCHAR* const MetaSoundTextKeywordsPointer = TEXT("/Body/Metadata/Keywords");

// The tolerant-test view of a MetaSound fixture (same asset, package and sidecar).
AssetDocumentTolerantTestSupport::FTarget MetaSoundTextView(const AssetDocumentMetaSoundTests::FTarget& Fixture)
{
	AssetDocumentTolerantTestSupport::FTarget Out(TEXT("Unused"));
	Out.AssetName = Fixture.AssetName;
	Out.PackageName = Fixture.PackageName;
	Out.ObjectPath = Fixture.ObjectPath;
	Out.PackageFile = Fixture.PackageFile;
	Out.SidecarPath = Fixture.SidecarPath;
	return Out;
}

// The DisplayName of the live MetaSound at Target (its root graph's metadata).
FText MetaSoundTextDisplayName(const AssetDocumentTolerantTestSupport::FTarget& Target)
{
	const IMetaSoundDocumentInterface* Document = Cast<IMetaSoundDocumentInterface>(Target.Get());
	return Document != nullptr ? Document->GetConstDocument().RootGraph.Metadata.GetDisplayName() : FText::GetEmpty();
}

// The Keywords of the live MetaSound at Target.
TArray<FText> MetaSoundTextKeywords(const AssetDocumentTolerantTestSupport::FTarget& Target)
{
	const IMetaSoundDocumentInterface* Document = Cast<IMetaSoundDocumentInterface>(Target.Get());
	return Document != nullptr ? Document->GetConstDocument().RootGraph.Metadata.GetKeywords() : TArray<FText>();
}

// The strings of the array at Pointer in Document.
TArray<FString> MetaSoundTextStrings(const TSharedPtr<FJsonObject>& Document, const TCHAR* Pointer)
{
	TArray<FString> Strings;
	const TSharedPtr<FJsonValue> Value = Document.IsValid() ? AssetDocumentTolerantTestSupport::At(Document, Pointer) : nullptr;
	if (Value.IsValid() && Value->Type == EJson::Array)
	{
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			Strings.Add(Item.IsValid() && Item->Type == EJson::String ? Item->AsString() : FString());
		}
	}
	return Strings;
}

// The document's Graphs region, serialized.
FString MetaSoundTextGraphs(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonValue> Graphs = Document.IsValid() ? AssetDocumentTolerantTestSupport::At(Document, TEXT("/Body/Graphs")) : nullptr;
	return Graphs.IsValid() && Graphs->Type == EJson::Object ? AssetDocumentTolerantTestSupport::Serialize(Graphs->AsObject()) : FString();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentMetaSoundTextLocalizationTest, "UEAssetDoc.Codec.MetaSound.TextLocalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentMetaSoundTextLocalizationTest::RunTest(const FString& Parameters)
{
	namespace Tolerant = AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentMetaSoundApplyTests;
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	// Removed after the MetaSound that refers to it.
	const Tolerant::FTarget Table(TEXT("ST_MetaSoundTexts"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Table.Cleanup(); };
	FFixtures Fixtures;
	const FTarget& Fixture = Fixtures.Add(TEXT("MSS_TextLocalization"));
	if (!TestNotNull(TEXT("string table saved"), Tolerant::CreateStringTable(Table, TEXT("Audio"), {{MetaSoundTextRow, MetaSoundTextRowString},
		{MetaSoundTextKeywordRows[0], TEXT("Loop sound")}, {MetaSoundTextKeywordRows[1], TEXT("Impact sound")}})))
	{
		return false;
	}
	UObject* Asset = Duplicate(*this, LoadQuiet(OneShot), Fixture);
	if (!TestNotNull(TEXT("duplicate"), Asset))
	{
		return false;
	}
	// Opened once (its input template nodes injected, as an apply does first) with a typed Description.
	const TSharedPtr<FJsonObject> Start = Extract(*this, Fixture.ObjectPath, TEXT("start"));
	if (!Start.IsValid())
	{
		return false;
	}
	Child(BodyOf(Start), TEXT("Metadata"))->SetStringField(TEXT("Description"), MetaSoundTextDescription);
	if (!ApplyOk(*this, Fixture, ForTarget(Start, Fixture, TEXT("Update")), TEXT("start")))
	{
		return false;
	}
	// The editor: DisplayName and two Keywords picked from the String Table.
	{
		FEditorSide Side(*Asset);
		Side.Builder.SetDisplayName(Tolerant::StringTableText(Table, MetaSoundTextRow));
		Side.Builder.SetKeywords({Tolerant::StringTableText(Table, MetaSoundTextKeywordRows[0]),
			Tolerant::StringTableText(Table, MetaSoundTextKeywordRows[1])});
	}
	if (!Save(*this, Asset, TEXT("fixture")))
	{
		return false;
	}
	const Tolerant::FTarget Target = MetaSoundTextView(Fixture);
	if (!TestTrue(TEXT("DisplayName holds the String Table row"),
		Tolerant::IsStringTableText(MetaSoundTextDisplayName(Target), Table, MetaSoundTextRow)))
	{
		return false;
	}

	// Read: the String Table reference, and the extract re-applies as a no-op.
	const TSharedPtr<FJsonObject> Extracted = Tolerant::TestTolerantRoundTrip(*this, Target, Tolerant::FExpected(), TEXT("String Table text"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const FString Graphs = MetaSoundTextGraphs(Extracted);
	TestEqual(TEXT("DisplayName reads as its String Table reference"), Tolerant::StringAt(Extracted, MetaSoundTextPointer),
		Tolerant::StringTableForm(Table, MetaSoundTextRow));
	TestEqual(TEXT("Description reads as typed"), Tolerant::StringAt(Extracted, TEXT("/Body/Metadata/Description")),
		FString(MetaSoundTextDescription));
	const TArray<FString> RowForms = {Tolerant::StringTableForm(Table, MetaSoundTextKeywordRows[0]),
		Tolerant::StringTableForm(Table, MetaSoundTextKeywordRows[1])};
	TestEqual(TEXT("Keywords read as their String Table references"), MetaSoundTextStrings(Extracted, MetaSoundTextKeywordsPointer), RowForms);

	// A document written before texts read in their text form holds the row's display string: the same text.
	{
		const TSharedPtr<FJsonObject> Older = Tolerant::Parse(Tolerant::Serialize(Extracted));
		TestTrue(TEXT("older document: DisplayName set"), Tolerant::SetStringAt(Older, MetaSoundTextPointer, MetaSoundTextRowString));
		Tolerant::TestAppliesUnchanged(*this, Target, Older, TEXT("Display string"));
		TestTrue(TEXT("display string: the String Table reference is kept"),
			Tolerant::IsStringTableText(MetaSoundTextDisplayName(Target), Table, MetaSoundTextRow));
	}

	// Another string sets a new text, as the Details text box does.
	const FAssetDocumentResult Edited = Tolerant::ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		Tolerant::SetStringAt(Document, MetaSoundTextPointer, TEXT("Engine idle"));
	}, TEXT("Edit"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		const FText DisplayName = MetaSoundTextDisplayName(Target);
		TestTrue(TEXT("edit: DisplayName holds the new string, no longer the String Table row"), !DisplayName.IsFromStringTable()
			&& !DisplayName.IsCultureInvariant() && DisplayName.ToString() == TEXT("Engine idle"));
		TestEqual(TEXT("edit: reads as the new string"), Tolerant::StringAt(Tolerant::ExtractResult(Target).Payload, MetaSoundTextPointer),
			FString(TEXT("Engine idle")));
	}

	// A culture-invariant text is written and read in its text form.
	const FAssetDocumentResult Invariant = Tolerant::ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		Tolerant::SetStringAt(Document, MetaSoundTextPointer, TEXT("INVTEXT(\"ENG_01\")"));
	}, TEXT("Invariant"));
	if (TestTrue(TEXT("invariant text applies"), Invariant.IsSuccess()))
	{
		const FText DisplayName = MetaSoundTextDisplayName(Target);
		TestTrue(TEXT("invariant: DisplayName holds a culture-invariant text"), DisplayName.IsCultureInvariant()
			&& DisplayName.ToString() == TEXT("ENG_01"));
		const TSharedPtr<FJsonObject> Reread = Tolerant::ExtractResult(Target).Payload;
		TestEqual(TEXT("invariant: reads as INVTEXT"), Tolerant::StringAt(Reread, MetaSoundTextPointer), FString(TEXT("INVTEXT(\"ENG_01\")")));
		TestEqual(TEXT("Description is kept"), Tolerant::StringAt(Reread, TEXT("/Body/Metadata/Description")), FString(MetaSoundTextDescription));
		TestEqual(TEXT("the graph is kept"), MetaSoundTextGraphs(Reread), Graphs);
	}

	// A keyword inserted before the String Table keywords, which the document holds as their display strings (as a
	// document written before texts read in their text form does): each of them keeps its row, not the row of the
	// keyword that stood at its index.
	const FAssetDocumentResult Inserted = Tolerant::ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonValue> Metadata = Tolerant::At(Document, TEXT("/Body/Metadata"));
		if (Metadata.IsValid() && Metadata->Type == EJson::Object)
		{
			TArray<TSharedPtr<FJsonValue>> Written;
			for (const TCHAR* Keyword : {TEXT("Oneshot"), TEXT("Loop sound"), TEXT("Impact sound")})
			{
				Written.Add(MakeShared<FJsonValueString>(Keyword));
			}
			Metadata->AsObject()->SetArrayField(TEXT("Keywords"), Written);
		}
	}, TEXT("Insert a keyword"));
	if (TestTrue(TEXT("the inserted keyword applies"), Inserted.IsSuccess()))
	{
		const TArray<FText> Keywords = MetaSoundTextKeywords(Target);
		if (TestEqual(TEXT("three keywords"), Keywords.Num(), 3))
		{
			TestTrue(TEXT("the inserted keyword is a plain text"), !Keywords[0].IsFromStringTable() && Keywords[0].ToString() == TEXT("Oneshot"));
			TestTrue(TEXT("the first String Table keyword keeps its row"),
				Tolerant::IsStringTableText(Keywords[1], Table, MetaSoundTextKeywordRows[0]));
			TestTrue(TEXT("the second String Table keyword keeps its row"),
				Tolerant::IsStringTableText(Keywords[2], Table, MetaSoundTextKeywordRows[1]));
		}
		TestEqual(TEXT("Keywords read with their String Table references"),
			MetaSoundTextStrings(Tolerant::ExtractResult(Target).Payload, MetaSoundTextKeywordsPointer),
			TArray<FString>({FString(TEXT("Oneshot")), RowForms[0], RowForms[1]}));
	}
	return !HasAnyErrors();
}

#endif
