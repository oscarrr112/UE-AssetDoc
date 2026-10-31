// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An editor-made Sound Cue holding a project sound node whose Caption text is set from a String Table row in the
// node's Details panel (the text box's String Table picker) and whose Note text is typed: Caption reads as
// LOCTABLE("<table>", "<key>") and the extract re-applies as a no-op; a document holding the row's display string
// instead keeps the String Table reference and saves nothing; another string sets a new text and INVTEXT a
// culture-invariant one; Note keeps its key and the node its link to the Output node. The node class is a test type of
// the AssetDocumentTests module (UAssetDocumentSoundCueTextTestNode), found by path: the automation framework loads
// that module before it lists the tests.

#include "AssetDocumentTolerantTestSupport.h"
#include "SoundCue/AssetDocumentSoundCue.h"

#include "AssetToolsModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Factories/SoundCueFactoryNew.h"
#include "IAssetTools.h"
#include "Misc/ScopeExit.h"
#include "ScopedTransaction.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "SoundCueGraph/SoundCueGraphNode.h"
#include "SoundCueGraph/SoundCueGraphNode_Root.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
const TCHAR* const SoundCueTextNodeClass = TEXT("/Script/AssetDocumentTests.AssetDocumentSoundCueTextTestNode");
const TCHAR* const SoundCueTextRow = TEXT("Thunder");
const TCHAR* const SoundCueTextRowString = TEXT("Distant thunder");
const TCHAR* const SoundCueTextNoteString = TEXT("Ambience bed");

FString SoundCueTextPointer(const FString& Key, const TCHAR* Property)
{
	return FString::Printf(TEXT("/Body/Nodes/%s/Properties/%s"), *Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")), Property);
}

// The node named Key in the live cue at Target, or null.
USoundNode* SoundCueTextNode(const AssetDocumentTolerantTestSupport::FTarget& Target, const FString& Key)
{
	if (const USoundCue* Cue = Target.Get<USoundCue>())
	{
		for (USoundNode* Node : Cue->AllNodes)
		{
			if (Node != nullptr && Node->GetName() == Key)
			{
				return Node;
			}
		}
	}
	return nullptr;
}

FText SoundCueTextOf(const AssetDocumentTolerantTestSupport::FTarget& Target, const FString& Key, const FTextProperty& Property)
{
	const USoundNode* Node = SoundCueTextNode(Target, Key);
	return Node != nullptr ? Property.GetPropertyValue_InContainer(Node) : FText::GetEmpty();
}

// The Details panel's edit of one text property of Node.
void SoundCueTextEdit(USoundNode& Node, FTextProperty& Property, const FText& Text)
{
	const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentSoundCueCodecTextTests", "EditDetail", "Edit Property"));
	Node.Modify();
	Node.PreEditChange(&Property);
	Property.SetPropertyValue_InContainer(&Node, Text);
	FPropertyChangedEvent Event(&Property, EPropertyChangeType::ValueSet);
	Node.PostEditChangeProperty(Event);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentSoundCueTextLocalizationTest, "UEAssetDoc.Codec.SoundCue.TextLocalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentSoundCueTextLocalizationTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	namespace SC = UE::AssetDocument::Graph::Audio::SoundCue;
	const FTarget Table(TEXT("ST_CueTexts"));
	const FTarget Target(TEXT("SC_LocalizedNode"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		Table.Cleanup();
	};

	UClass* NodeClass = FindObject<UClass>(nullptr, SoundCueTextNodeClass);
	FTextProperty* CaptionProperty = NodeClass != nullptr ? FindFProperty<FTextProperty>(NodeClass, TEXT("Caption")) : nullptr;
	FTextProperty* NoteProperty = NodeClass != nullptr ? FindFProperty<FTextProperty>(NodeClass, TEXT("Note")) : nullptr;
	if (!TestTrue(TEXT("the test sound node class and its text properties are loaded"), CaptionProperty != nullptr && NoteProperty != nullptr)
		|| !TestNotNull(TEXT("string table saved"), CreateStringTable(Table, TEXT("Audio"), {{SoundCueTextRow, SoundCueTextRowString}})))
	{
		return false;
	}

	// Content Browser: Add > Sounds > Sound Cue.
	USoundCueFactoryNew* Factory = NewObject<USoundCueFactoryNew>();
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	USoundCue* Cue = Cast<USoundCue>(AssetTools.CreateAsset(Target.AssetName, FPackageName::GetLongPackagePath(Target.PackageName),
		USoundCue::StaticClass(), Factory));
	if (!TestTrue(TEXT("cue created with its graph"), Cue != nullptr && Cue->SoundCueGraph != nullptr))
	{
		return false;
	}

	// Palette: place the node, drag its output to the Output node; Details: Caption from the String Table, Note typed.
	USoundCueGraphNode* GraphNode = Cast<USoundCueGraphNode>(SC::PerformPaletteAction(*Cue->SoundCueGraph, NodeClass, FVector2f(-300.f, 0.f)));
	if (!TestTrue(TEXT("palette places the node"), GraphNode != nullptr && GraphNode->SoundNode != nullptr))
	{
		return false;
	}
	USoundNode* Node = GraphNode->SoundNode;
	const FString Key = Node->GetName();
	for (UEdGraphNode* Candidate : Cue->SoundCueGraph->Nodes)
	{
		if (USoundCueGraphNode_Root* Root = Cast<USoundCueGraphNode_Root>(Candidate))
		{
			const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentSoundCueCodecTextTests", "LinkPins", "Link Pins"));
			TestTrue(TEXT("node linked to the Output node"),
				Cue->SoundCueGraph->GetSchema()->TryCreateConnection(GraphNode->GetOutputPin(), Root->GetInputPin(0)));
		}
	}
	SoundCueTextEdit(*Node, *CaptionProperty, StringTableText(Table, SoundCueTextRow));
	SoundCueTextEdit(*Node, *NoteProperty, NSLOCTEXT("AssetDocumentSoundCueCodecTextTests", "Note", "Ambience bed"));
	if (!TestTrue(TEXT("Caption holds the String Table row"), IsStringTableText(CaptionProperty->GetPropertyValue_InContainer(Node), Table, SoundCueTextRow))
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
	const FString Pointer = SoundCueTextPointer(Key, TEXT("Caption"));
	TestEqual(TEXT("Caption reads as its String Table reference"), StringAt(Extracted, Pointer), StringTableForm(Table, SoundCueTextRow));
	TestEqual(TEXT("Note reads as typed"), StringAt(Extracted, SoundCueTextPointer(Key, TEXT("Note"))), FString(SoundCueTextNoteString));

	// A document written before texts read in their text form holds the row's display string: the same text.
	{
		const TSharedPtr<FJsonObject> Older = Parse(Serialize(Extracted));
		TestTrue(TEXT("older document: Caption set"), SetStringAt(Older, Pointer, SoundCueTextRowString));
		TestAppliesUnchanged(*this, Target, Older, TEXT("Display string"));
		TestTrue(TEXT("display string: the String Table reference is kept"),
			IsStringTableText(SoundCueTextOf(Target, Key, *CaptionProperty), Table, SoundCueTextRow));
	}

	// Another string sets a new text, as the Details text box does.
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [&Pointer](const TSharedPtr<FJsonObject>& Document)
	{
		SetStringAt(Document, Pointer, TEXT("Rain on the roof"));
	}, TEXT("Edit"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		const FText Caption = SoundCueTextOf(Target, Key, *CaptionProperty);
		TestTrue(TEXT("edit: Caption holds the new string, no longer the String Table row"), !Caption.IsFromStringTable()
			&& !Caption.IsCultureInvariant() && Caption.ToString() == TEXT("Rain on the roof"));
		TestEqual(TEXT("edit: reads as the new string"), StringAt(ExtractResult(Target).Payload, Pointer), FString(TEXT("Rain on the roof")));
	}

	// A culture-invariant text is written and read in its text form.
	const FAssetDocumentResult Invariant = ApplyEdited(*this, Target, [&Pointer](const TSharedPtr<FJsonObject>& Document)
	{
		SetStringAt(Document, Pointer, TEXT("INVTEXT(\"SFX_01\")"));
	}, TEXT("Invariant"));
	if (TestTrue(TEXT("invariant text applies"), Invariant.IsSuccess()))
	{
		const FText Caption = SoundCueTextOf(Target, Key, *CaptionProperty);
		TestTrue(TEXT("invariant: Caption holds a culture-invariant text"), Caption.IsCultureInvariant() && Caption.ToString() == TEXT("SFX_01"));
		const TSharedPtr<FJsonObject> Reread = ExtractResult(Target).Payload;
		TestEqual(TEXT("invariant: reads as INVTEXT"), StringAt(Reread, Pointer), FString(TEXT("INVTEXT(\"SFX_01\")")));
		TestEqual(TEXT("Note still reads as typed"), StringAt(Reread, SoundCueTextPointer(Key, TEXT("Note"))), FString(SoundCueTextNoteString));
		const FText Note = SoundCueTextOf(Target, Key, *NoteProperty);
		TestEqual(TEXT("Note keeps its key"), FTextInspector::GetKey(Note).Get(FString()), FString(TEXT("Note")));
		const USoundCue* Live = Target.Get<USoundCue>();
		TestTrue(TEXT("the node is still linked to the Output node"), Live != nullptr && Live->FirstNode != nullptr && Live->FirstNode->GetName() == Key);
	}
	return !HasAnyErrors();
}

#endif
