// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An editor-made Widget Blueprint whose TextBlock Text comes from a String Table row (the Designer Details text box,
// String Table picker): it reads as LOCTABLE("<table>", "<key>") and the extract re-applies as a no-op; a document
// holding the row's display string instead keeps the String Table reference and saves nothing; another string sets a
// new text and INVTEXT a culture-invariant one; the other TextBlock's text is kept.

#include "AssetDocumentTolerantTestSupport.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/TextBlock.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"
#include "WidgetBlueprint.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
const TCHAR* const WidgetTextRow = TEXT("Title");
const TCHAR* const WidgetTextRowString = TEXT("Main Menu");
const TCHAR* const WidgetTextKeptString = TEXT("Press any key");

// The JSON Pointer of the Text property of the root's child named Name; empty when there is none.
FString WidgetTextPointer(const TSharedPtr<FJsonObject>& Document, const TCHAR* Name)
{
	const TSharedPtr<FJsonValue> Children = Document.IsValid()
		? AssetDocumentTolerantTestSupport::At(Document, TEXT("/Body/WidgetTree/RootWidget/Children")) : nullptr;
	if (!Children.IsValid() || Children->Type != EJson::Array)
	{
		return FString();
	}
	const TArray<TSharedPtr<FJsonValue>>& Items = Children->AsArray();
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		FString ChildName;
		if (Items[Index]->Type == EJson::Object && Items[Index]->AsObject()->TryGetStringField(TEXT("Name"), ChildName) && ChildName == Name)
		{
			return FString::Printf(TEXT("/Body/WidgetTree/RootWidget/Children/%d/Properties/Text"), Index);
		}
	}
	return FString();
}

// The text of the live TextBlock named Name (the Widget Blueprint at Target); empty when there is none.
FText WidgetTextOf(const AssetDocumentTolerantTestSupport::FTarget& Target, const TCHAR* Name)
{
	const UWidgetBlueprint* Blueprint = Target.Get<UWidgetBlueprint>();
	const UTextBlock* Block = Blueprint != nullptr && Blueprint->WidgetTree != nullptr
		? Cast<UTextBlock>(Blueprint->WidgetTree->FindWidget(FName(Name))) : nullptr;
	return Block != nullptr ? Block->GetText() : FText::GetEmpty();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentWidgetTextLocalizationTest, "UEAssetDoc.Codec.WidgetBlueprint.TextLocalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentWidgetTextLocalizationTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	const FTarget Table(TEXT("ST_HudTexts"));
	const FTarget Target(TEXT("WBP_LocalizedHud"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		Table.Cleanup();
	};

	if (!TestNotNull(TEXT("string table saved"), CreateStringTable(Table, TEXT("Hud"), {{WidgetTextRow, WidgetTextRowString}})))
	{
		return false;
	}
	UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(),
		CreatePackage(*Target.PackageName), FName(*Target.AssetName), BPTYPE_Normal, UWidgetBlueprint::StaticClass(),
		UWidgetBlueprintGeneratedClass::StaticClass()));
	if (!TestNotNull(TEXT("widget blueprint created"), Blueprint))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Blueprint);
	// Designer: a Canvas Panel with two Text Blocks; Title's Text picked from the String Table, Hint's typed.
	UCanvasPanel* Root = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	Blueprint->WidgetTree->RootWidget = Root;
	UTextBlock* Title = Blueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Title"));
	UTextBlock* Hint = Blueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Hint"));
	Root->AddChild(Title);
	Root->AddChild(Hint);
	Title->SetText(StringTableText(Table, WidgetTextRow));
	Hint->SetText(FText::FromString(WidgetTextKeptString));
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("Title holds the String Table row"), IsStringTableText(Title->GetText(), Table, WidgetTextRow))
		|| !TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	// Read: the String Table reference, and the extract re-applies as a no-op.
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("String Table text"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const FString Pointer = WidgetTextPointer(Extracted, TEXT("Title"));
	TestEqual(TEXT("Title.Text reads as its String Table reference"), StringAt(Extracted, Pointer), StringTableForm(Table, WidgetTextRow));
	TestEqual(TEXT("Hint.Text reads as typed"), StringAt(Extracted, WidgetTextPointer(Extracted, TEXT("Hint"))), FString(WidgetTextKeptString));

	// A document written before texts read in their text form holds the row's display string: the same text.
	{
		const TSharedPtr<FJsonObject> Older = Parse(Serialize(Extracted));
		TestTrue(TEXT("older document: Title.Text set"), SetStringAt(Older, Pointer, WidgetTextRowString));
		TestAppliesUnchanged(*this, Target, Older, TEXT("Display string"));
		TestTrue(TEXT("display string: the String Table reference is kept"),
			IsStringTableText(WidgetTextOf(Target, TEXT("Title")), Table, WidgetTextRow));
	}

	// Another string sets a new text, as the Details text box does.
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		SetStringAt(Document, WidgetTextPointer(Document, TEXT("Title")), TEXT("Pause"));
	}, TEXT("Edit"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		const FText Text = WidgetTextOf(Target, TEXT("Title"));
		TestTrue(TEXT("edit: Title holds the new string, no longer the String Table row"), !Text.IsFromStringTable()
			&& !Text.IsCultureInvariant() && Text.ToString() == TEXT("Pause"));
		const TSharedPtr<FJsonObject> Reread = ExtractResult(Target).Payload;
		TestEqual(TEXT("edit: reads as the new string"), StringAt(Reread, WidgetTextPointer(Reread, TEXT("Title"))), FString(TEXT("Pause")));
	}

	// A culture-invariant text is written and read in its text form.
	const FAssetDocumentResult Invariant = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		SetStringAt(Document, WidgetTextPointer(Document, TEXT("Title")), TEXT("INVTEXT(\"v1.0\")"));
	}, TEXT("Invariant"));
	if (TestTrue(TEXT("invariant text applies"), Invariant.IsSuccess()))
	{
		const FText Text = WidgetTextOf(Target, TEXT("Title"));
		TestTrue(TEXT("invariant: Title holds a culture-invariant text"), Text.IsCultureInvariant() && Text.ToString() == TEXT("v1.0"));
		const TSharedPtr<FJsonObject> Reread = ExtractResult(Target).Payload;
		TestEqual(TEXT("invariant: reads as INVTEXT"), StringAt(Reread, WidgetTextPointer(Reread, TEXT("Title"))), FString(TEXT("INVTEXT(\"v1.0\")")));
		TestEqual(TEXT("Hint.Text is kept"), StringAt(Reread, WidgetTextPointer(Reread, TEXT("Hint"))), FString(WidgetTextKeptString));
		TestEqual(TEXT("Hint's text is unchanged"), WidgetTextOf(Target, TEXT("Hint")).ToString(), FString(WidgetTextKeptString));
	}
	return !HasAnyErrors();
}

#endif
