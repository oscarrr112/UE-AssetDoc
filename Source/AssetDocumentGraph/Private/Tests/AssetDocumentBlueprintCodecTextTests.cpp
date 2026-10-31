// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An editor-made Blueprint whose Print Text node's In Text pin is bound to a String Table row (the pin's text box,
// String Table picker): the pin reads as LOCTABLE("<table>", "<key>") and the extract re-applies as a no-op; a document
// holding the row's display string instead keeps the String Table reference and saves nothing; another string sets a
// new text and INVTEXT a culture-invariant one; the other node's pin default is kept.

#include "AssetDocumentTolerantTestSupport.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
const TCHAR* const BlueprintTextRow = TEXT("Greeting");
const TCHAR* const BlueprintTextRowString = TEXT("Welcome back");
const TCHAR* const BlueprintTextKeptString = TEXT("Kept note");

// The JSON Pointer of Field in the PinOverrides entry of Pin, on the node whose Action contains Needle; empty when none.
FString BlueprintTextOverridePointer(const TSharedPtr<FJsonObject>& Document, const TCHAR* Needle, const TCHAR* Pin, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Pages = nullptr;
	if (!Document.IsValid() || !Document->TryGetObjectField(TEXT("Body"), Body) || !(*Body)->TryGetArrayField(TEXT("UbergraphPages"), Pages))
	{
		return FString();
	}
	for (int32 PageIndex = 0; PageIndex < Pages->Num(); ++PageIndex)
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if ((*Pages)[PageIndex]->Type != EJson::Object || !(*Pages)[PageIndex]->AsObject()->TryGetArrayField(TEXT("Nodes"), Nodes))
		{
			continue;
		}
		for (int32 NodeIndex = 0; NodeIndex < Nodes->Num(); ++NodeIndex)
		{
			FString Action;
			const TArray<TSharedPtr<FJsonValue>>* Overrides = nullptr;
			if ((*Nodes)[NodeIndex]->Type != EJson::Object || !(*Nodes)[NodeIndex]->AsObject()->TryGetStringField(TEXT("Action"), Action)
				|| !Action.Contains(Needle) || !(*Nodes)[NodeIndex]->AsObject()->TryGetArrayField(TEXT("PinOverrides"), Overrides))
			{
				continue;
			}
			for (int32 OverrideIndex = 0; OverrideIndex < Overrides->Num(); ++OverrideIndex)
			{
				FString Name;
				if ((*Overrides)[OverrideIndex]->Type == EJson::Object
					&& (*Overrides)[OverrideIndex]->AsObject()->TryGetStringField(TEXT("Pin"), Name) && Name == Pin)
				{
					return FString::Printf(TEXT("/Body/UbergraphPages/%d/Nodes/%d/PinOverrides/%d/%s"), PageIndex, NodeIndex, OverrideIndex, Field);
				}
			}
		}
	}
	return FString();
}

FString BlueprintInTextPointer(const TSharedPtr<FJsonObject>& Document)
{
	return BlueprintTextOverridePointer(Document, TEXT("PrintText"), TEXT("InText"), TEXT("DefaultTextValue"));
}

// Pin of the event graph's call to Function (the live asset at Target), or null.
UEdGraphPin* BlueprintTextCallPin(const AssetDocumentTolerantTestSupport::FTarget& Target, FName Function, const TCHAR* Pin)
{
	const UBlueprint* Blueprint = Target.Get<UBlueprint>();
	const UEdGraph* Graph = Blueprint != nullptr ? FBlueprintEditorUtils::FindEventGraph(Blueprint) : nullptr;
	if (Graph == nullptr)
	{
		return nullptr;
	}
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
		if (Call != nullptr && Call->FunctionReference.GetMemberName() == Function)
		{
			return Node->FindPin(Pin);
		}
	}
	return nullptr;
}

UEdGraphPin* BlueprintInTextPin(const AssetDocumentTolerantTestSupport::FTarget& Target)
{
	return BlueprintTextCallPin(Target, GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintText), TEXT("InText"));
}

// A call to Function placed in Graph from the palette.
UK2Node_CallFunction* BlueprintTextPlaceCall(UEdGraph& Graph, FName Function, int32 X)
{
	FGraphNodeCreator<UK2Node_CallFunction> Creator(Graph);
	UK2Node_CallFunction* Call = Creator.CreateNode(false);
	Call->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(Function));
	Call->NodePosX = X;
	Creator.Finalize();
	return Call;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintTextPinLocalizationTest, "UEAssetDoc.Codec.Blueprint.TextPinLocalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBlueprintTextPinLocalizationTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	const FTarget Table(TEXT("ST_PinTexts"));
	const FTarget Target(TEXT("BP_PinText"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		Table.Cleanup();
	};

	if (!TestNotNull(TEXT("string table saved"), CreateStringTable(Table, TEXT("UI"), {{BlueprintTextRow, BlueprintTextRowString}})))
	{
		return false;
	}
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*Target.PackageName),
		FName(*Target.AssetName), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("blueprint created"), Blueprint))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Blueprint);
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("event graph"), EventGraph))
	{
		return false;
	}
	// Print Text with its In Text bound to the String Table row, and Print String with a typed string.
	UK2Node_CallFunction* PrintTextNode = BlueprintTextPlaceCall(*EventGraph, GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintText), 400);
	UK2Node_CallFunction* PrintStringNode = BlueprintTextPlaceCall(*EventGraph, GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString), 800);
	UEdGraphPin* InText = PrintTextNode->FindPin(TEXT("InText"));
	UEdGraphPin* InString = PrintStringNode->FindPin(TEXT("InString"));
	if (!TestTrue(TEXT("In Text and In String pins"), InText != nullptr && InString != nullptr))
	{
		return false;
	}
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	Schema->TrySetDefaultText(*InText, StringTableText(Table, BlueprintTextRow));
	Schema->TrySetDefaultValue(*InString, BlueprintTextKeptString);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("the pin holds the String Table row"), IsStringTableText(InText->DefaultTextValue, Table, BlueprintTextRow))
		|| !TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	// Read: the String Table reference, and the extract re-applies as a no-op.
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("String Table pin"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const FString Pointer = BlueprintInTextPointer(Extracted);
	const FString KeptPointer = BlueprintTextOverridePointer(Extracted, TEXT("PrintString"), TEXT("InString"), TEXT("DefaultValue"));
	TestEqual(TEXT("In Text reads as its String Table reference"), StringAt(Extracted, Pointer), StringTableForm(Table, BlueprintTextRow));
	TestEqual(TEXT("In String reads as typed"), StringAt(Extracted, KeptPointer), FString(BlueprintTextKeptString));

	// A document written before texts read in their text form holds the row's display string: the same text.
	{
		const TSharedPtr<FJsonObject> Older = Parse(Serialize(Extracted));
		TestTrue(TEXT("older document: In Text set"), SetStringAt(Older, Pointer, BlueprintTextRowString));
		TestAppliesUnchanged(*this, Target, Older, TEXT("Display string"));
		const UEdGraphPin* Kept = BlueprintInTextPin(Target);
		TestTrue(TEXT("display string: the String Table reference is kept"), Kept != nullptr
			&& IsStringTableText(Kept->DefaultTextValue, Table, BlueprintTextRow));
	}

	// Another string sets a new text, as the pin's text box does.
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		SetStringAt(Document, BlueprintInTextPointer(Document), TEXT("Good night"));
	}, TEXT("Edit"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		const UEdGraphPin* Pin = BlueprintInTextPin(Target);
		TestTrue(TEXT("edit: the pin holds the new string, no longer the String Table row"), Pin != nullptr
			&& !Pin->DefaultTextValue.IsFromStringTable() && !Pin->DefaultTextValue.IsCultureInvariant()
			&& Pin->DefaultTextValue.ToString() == TEXT("Good night"));
		const TSharedPtr<FJsonObject> Reread = ExtractResult(Target).Payload;
		TestEqual(TEXT("edit: reads as the new string"), StringAt(Reread, BlueprintInTextPointer(Reread)), FString(TEXT("Good night")));
	}

	// A culture-invariant text is written and read in its text form.
	const FAssetDocumentResult Invariant = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		SetStringAt(Document, BlueprintInTextPointer(Document), TEXT("INVTEXT(\"See you\")"));
	}, TEXT("Invariant"));
	if (TestTrue(TEXT("invariant text applies"), Invariant.IsSuccess()))
	{
		const UEdGraphPin* Pin = BlueprintInTextPin(Target);
		TestTrue(TEXT("invariant: the pin holds a culture-invariant text"), Pin != nullptr
			&& Pin->DefaultTextValue.IsCultureInvariant() && Pin->DefaultTextValue.ToString() == TEXT("See you"));
		const TSharedPtr<FJsonObject> Reread = ExtractResult(Target).Payload;
		TestEqual(TEXT("invariant: reads as INVTEXT"), StringAt(Reread, BlueprintInTextPointer(Reread)), FString(TEXT("INVTEXT(\"See you\")")));
		TestEqual(TEXT("the other node's pin default is kept"),
			StringAt(Reread, BlueprintTextOverridePointer(Reread, TEXT("PrintString"), TEXT("InString"), TEXT("DefaultValue"))),
			FString(BlueprintTextKeptString));
		const UEdGraphPin* Other = BlueprintTextCallPin(Target, GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString), TEXT("InString"));
		TestTrue(TEXT("the other node's pin is unchanged"), Other != nullptr && Other->DefaultValue == BlueprintTextKeptString);
	}
	return !HasAnyErrors();
}

#endif
