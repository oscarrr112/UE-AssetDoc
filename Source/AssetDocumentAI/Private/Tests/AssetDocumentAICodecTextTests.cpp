// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// A Behavior Tree whose project task has a Label text set from a String Table row in the task's Details panel (the
// text box's String Table picker) and a typed Hint text: Label reads as LOCTABLE("<table>", "<key>") and the extract
// re-applies as a no-op; a document holding the row's display string instead keeps the String Table reference and
// saves nothing; another string sets a new text and INVTEXT a culture-invariant one, while the rebuilt tree keeps the
// unchanged Hint with its key. The tree is laid out by a Create apply (a Sequence and the task); the task class is a
// test type of the AssetDocumentTests module (UAssetDocumentAITextTestTask), found by path: the automation framework
// loads that module before it lists the tests.

#include "AssetDocumentTolerantTestSupport.h"

#include "AIGraphNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "EdGraph/EdGraph.h"
#include "Misc/ScopeExit.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
const TCHAR* const BehaviorTreeTextTaskClass = TEXT("/Script/AssetDocumentTests.AssetDocumentAITextTestTask");
const TCHAR* const BehaviorTreeTextRow = TEXT("Attack");
const TCHAR* const BehaviorTreeTextRowString = TEXT("Charge the target");
const TCHAR* const BehaviorTreeTextHintString = TEXT("Wait here");

// A Sequence under the root with the test task as its only child.
FString BehaviorTreeTextDocument(const AssetDocumentTolerantTestSupport::FTarget& Target)
{
	return FString::Printf(TEXT(R"({"SchemaVersion": 1, "Target": "%s", "Class": "/Script/AIModule.BehaviorTree",
		"Action": "CreateOrUpdate", "Definitions": {}, "Properties": {}, "Body": {"BlackboardAsset": null,
		"Tree": {"Root": {"Id": "00000000000000000000000000000001", "Class": "/Script/AIModule.BTComposite_Sequence", "Children": [
			{"Id": "00000000000000000000000000000002", "Class": "%s", "Editor": {"Position": {"X": 300, "Y": 200}}}]}}}})"),
		*Target.PackageName, BehaviorTreeTextTaskClass);
}

// The JSON Pointer of the task's property Name; empty when the task is not the Sequence's child.
FString BehaviorTreeTextPointer(const TSharedPtr<FJsonObject>& Document, const TCHAR* Name)
{
	const TSharedPtr<FJsonValue> Children = Document.IsValid()
		? AssetDocumentTolerantTestSupport::At(Document, TEXT("/Body/Tree/Root/Children")) : nullptr;
	if (!Children.IsValid() || Children->Type != EJson::Array)
	{
		return FString();
	}
	const TArray<TSharedPtr<FJsonValue>>& Items = Children->AsArray();
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		FString Class;
		if (Items[Index]->Type == EJson::Object && Items[Index]->AsObject()->TryGetStringField(TEXT("Class"), Class)
			&& Class == BehaviorTreeTextTaskClass)
		{
			return FString::Printf(TEXT("/Body/Tree/Root/Children/%d/Properties/%s"), Index, Name);
		}
	}
	return FString();
}

// The task the tree's graph holds (the live asset at Target), or null.
UObject* BehaviorTreeTextTask(const AssetDocumentTolerantTestSupport::FTarget& Target, const UClass* TaskClass)
{
	const UBehaviorTree* Tree = Target.Get<UBehaviorTree>();
	if (Tree == nullptr || Tree->BTGraph == nullptr || TaskClass == nullptr)
	{
		return nullptr;
	}
	for (const UEdGraphNode* Node : Tree->BTGraph->Nodes)
	{
		const UAIGraphNode* GraphNode = Cast<UAIGraphNode>(Node);
		if (GraphNode != nullptr && GraphNode->NodeInstance != nullptr && GraphNode->NodeInstance->GetClass() == TaskClass)
		{
			return GraphNode->NodeInstance;
		}
	}
	return nullptr;
}

// The text of the live task's property Property; empty when there is no task.
FText BehaviorTreeTextOf(const AssetDocumentTolerantTestSupport::FTarget& Target, const FTextProperty& Property)
{
	const UObject* Task = BehaviorTreeTextTask(Target, Property.GetOwnerClass());
	return Task != nullptr ? Property.GetPropertyValue_InContainer(Task) : FText::GetEmpty();
}

// The Details panel's edit of one text property of Task.
void BehaviorTreeTextEdit(UObject& Task, FTextProperty& Property, const FText& Text)
{
	const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentAICodecTextTests", "EditDetail", "Edit Property"));
	Task.Modify();
	Task.PreEditChange(&Property);
	Property.SetPropertyValue_InContainer(&Task, Text);
	FPropertyChangedEvent Event(&Property, EPropertyChangeType::ValueSet);
	Task.PostEditChangeProperty(Event);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBehaviorTreeTextLocalizationTest, "UEAssetDoc.Codec.BehaviorTree.TextLocalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBehaviorTreeTextLocalizationTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	const FTarget Table(TEXT("ST_TaskTexts"));
	const FTarget Target(TEXT("BT_LocalizedTask"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		Table.Cleanup();
	};

	UClass* TaskClass = FindObject<UClass>(nullptr, BehaviorTreeTextTaskClass);
	FTextProperty* LabelProperty = TaskClass != nullptr ? FindFProperty<FTextProperty>(TaskClass, TEXT("Label")) : nullptr;
	FTextProperty* HintProperty = TaskClass != nullptr ? FindFProperty<FTextProperty>(TaskClass, TEXT("Hint")) : nullptr;
	if (!TestTrue(TEXT("the test task class and its text properties are loaded"), LabelProperty != nullptr && HintProperty != nullptr)
		|| !TestNotNull(TEXT("string table saved"), CreateStringTable(Table, TEXT("AI"), {{BehaviorTreeTextRow, BehaviorTreeTextRowString}}))
		|| !TestTrue(TEXT("tree created"), ApplyText(*this, Target, BehaviorTreeTextDocument(Target), TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UObject* Task = BehaviorTreeTextTask(Target, TaskClass);
	if (!TestNotNull(TEXT("the task in the tree's graph"), Task))
	{
		return false;
	}
	// Details panel: Label from the String Table, Hint typed.
	BehaviorTreeTextEdit(*Task, *LabelProperty, StringTableText(Table, BehaviorTreeTextRow));
	BehaviorTreeTextEdit(*Task, *HintProperty, NSLOCTEXT("AssetDocumentAICodecTextTests", "Hint", "Wait here"));
	if (!TestTrue(TEXT("Label holds the String Table row"), IsStringTableText(BehaviorTreeTextOf(Target, *LabelProperty), Table, BehaviorTreeTextRow))
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
	const FString Pointer = BehaviorTreeTextPointer(Extracted, TEXT("Label"));
	TestEqual(TEXT("Label reads as its String Table reference"), StringAt(Extracted, Pointer), StringTableForm(Table, BehaviorTreeTextRow));
	TestEqual(TEXT("Hint reads as typed"), StringAt(Extracted, BehaviorTreeTextPointer(Extracted, TEXT("Hint"))), FString(BehaviorTreeTextHintString));

	// A document written before texts read in their text form holds the row's display string: the same text.
	{
		const TSharedPtr<FJsonObject> Older = Parse(Serialize(Extracted));
		TestTrue(TEXT("older document: Label set"), SetStringAt(Older, Pointer, BehaviorTreeTextRowString));
		TestAppliesUnchanged(*this, Target, Older, TEXT("Display string"));
		TestTrue(TEXT("display string: the String Table reference is kept"),
			IsStringTableText(BehaviorTreeTextOf(Target, *LabelProperty), Table, BehaviorTreeTextRow));
	}

	// Another string sets a new text; the rebuilt tree keeps the unchanged Hint with its key.
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		SetStringAt(Document, BehaviorTreeTextPointer(Document, TEXT("Label")), TEXT("Fall back"));
	}, TEXT("Edit"));
	if (TestTrue(TEXT("edit applies"), Edited.IsSuccess()))
	{
		const FText Label = BehaviorTreeTextOf(Target, *LabelProperty);
		TestTrue(TEXT("edit: Label holds the new string, no longer the String Table row"), !Label.IsFromStringTable()
			&& !Label.IsCultureInvariant() && Label.ToString() == TEXT("Fall back"));
		const TSharedPtr<FJsonObject> Reread = ExtractResult(Target).Payload;
		TestEqual(TEXT("edit: reads as the new string"), StringAt(Reread, BehaviorTreeTextPointer(Reread, TEXT("Label"))), FString(TEXT("Fall back")));
		const FText Hint = BehaviorTreeTextOf(Target, *HintProperty);
		TestEqual(TEXT("edit: Hint is kept"), Hint.ToString(), FString(BehaviorTreeTextHintString));
		TestEqual(TEXT("edit: Hint keeps its key"), FTextInspector::GetKey(Hint).Get(FString()), FString(TEXT("Hint")));
	}

	// A culture-invariant text is written and read in its text form.
	const FAssetDocumentResult Invariant = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		SetStringAt(Document, BehaviorTreeTextPointer(Document, TEXT("Label")), TEXT("INVTEXT(\"Hold\")"));
	}, TEXT("Invariant"));
	if (TestTrue(TEXT("invariant text applies"), Invariant.IsSuccess()))
	{
		const FText Label = BehaviorTreeTextOf(Target, *LabelProperty);
		TestTrue(TEXT("invariant: Label holds a culture-invariant text"), Label.IsCultureInvariant() && Label.ToString() == TEXT("Hold"));
		const TSharedPtr<FJsonObject> Reread = ExtractResult(Target).Payload;
		TestEqual(TEXT("invariant: reads as INVTEXT"), StringAt(Reread, BehaviorTreeTextPointer(Reread, TEXT("Label"))), FString(TEXT("INVTEXT(\"Hold\")")));
		TestEqual(TEXT("Hint still reads as typed"), StringAt(Reread, BehaviorTreeTextPointer(Reread, TEXT("Hint"))), FString(BehaviorTreeTextHintString));
	}
	return !HasAnyErrors();
}

#endif
