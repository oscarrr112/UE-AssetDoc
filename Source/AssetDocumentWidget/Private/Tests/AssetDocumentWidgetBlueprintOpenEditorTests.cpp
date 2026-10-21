// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// UEAssetDoc.WidgetBlueprint.OpenEditorApply: a Widget Blueprint open in the Widget Blueprint
// editor, with a widget selected in the Designer, takes an apply that removes that widget, adds another one, changes
// a property of a third, adds a variable and an EventGraph node with a link. The open editor then shows what it shows
// after the same edits made in it: the Designer's preview widget has the new widget and the new property value and
// not the removed widget, the Designer selection holds no removed widget, My Blueprint lists the new variable. A
// failed apply rolls back and the Designer preview shows the rolled-back tree; the editor's own edit and Undo after
// the apply keep the applied state; the editor closes cleanly.

#include "AssetDocumentOpenEditorTestSupport.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/ScopeExit.h"
#include "SMyBlueprint.h"
#include "ScopedTransaction.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditor.h"
#include "WidgetReference.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentWidgetBlueprintOpenEditorTests
{
using namespace UE::AssetDocument::OpenEditorTests;

FString Document(const FTarget& Target, const TCHAR* Action, const FString& Body)
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/UMGEditor.WidgetBlueprint\",\"Action\":\"%s\",")
		TEXT("\"Body\":{\"ParentClass\":\"/Script/UMG.UserWidget\",%s}}"), *Target.PackageName, Action, *Body);
}

const TCHAR* InitialBody = TEXT(R"json(
"Variables":[{"Name":"Count","Type":"int"}],
"WidgetTree":{"RootWidget":{"Name":"Root","Class":"/Script/UMG.CanvasPanel","Children":[
  {"Name":"Title","Class":"/Script/UMG.TextBlock","IsVariable":true,"Properties":{"Text":"Welcome"}},
  {"Name":"OldButton","Class":"/Script/UMG.Button","IsVariable":true}]}})json");

// Removes OldButton, adds NewImage, retitles Title, adds Score and a Print String after Construct.
const TCHAR* EditedBody = TEXT(R"json(
"Variables":[{"Name":"Count","Type":"int"},{"Name":"Score","Type":"float","DefaultValue":1.5}],
"WidgetTree":{"RootWidget":{"Name":"Root","Class":"/Script/UMG.CanvasPanel","Children":[
  {"Name":"Title","Class":"/Script/UMG.TextBlock","IsVariable":true,"Properties":{"Text":"Hello"}},
  {"Name":"NewImage","Class":"/Script/UMG.Image","IsVariable":true}]}},
"UbergraphPages":[{"Name":"EventGraph","Nodes":[{"Id":"Construct","Action":"K2:Event:/Script/UMG.UserWidget:Construct"},
  {"Id":"Print","Action":"K2:PrintString"}],"Links":[{"From":"Construct:then","To":"Print:execute"}]}])json");

// The edit a failed apply rolls back: retitles Title, adds Bogus, removes NewImage.
const TCHAR* FailingBody = TEXT(R"json(
"Variables":[{"Name":"Count","Type":"int"},{"Name":"Score","Type":"float","DefaultValue":1.5},{"Name":"Bogus","Type":"bool"}],
"WidgetTree":{"RootWidget":{"Name":"Root","Class":"/Script/UMG.CanvasPanel","Children":[
  {"Name":"Title","Class":"/Script/UMG.TextBlock","IsVariable":true,"Properties":{"Text":"Failed"}},
  {"Name":"BogusText","Class":"/Script/UMG.TextBlock","IsVariable":true}]}})json");

FWidgetBlueprintEditor* EditorOf(UWidgetBlueprint* Blueprint)
{
	return static_cast<FWidgetBlueprintEditor*>(FindEditor(Blueprint));
}

bool MyBlueprintLists(FWidgetBlueprintEditor& Editor, const TCHAR* Name)
{
	const TSharedPtr<SMyBlueprint> MyBlueprint = Editor.GetMyBlueprintWidget();
	const bool bListed = MyBlueprint.IsValid() && ActionMenuLists(MyBlueprint.ToSharedRef(), FName(Name));
	if (bListed)
	{
		MyBlueprint->ClearGraphActionMenuSelection();
	}
	return bListed;
}

// A widget of the Designer's preview (the widget instance the Designer draws), or null.
UWidget* PreviewWidget(FWidgetBlueprintEditor& Editor, const TCHAR* Name)
{
	const UUserWidget* Preview = Editor.GetPreview();
	return Preview != nullptr && Preview->WidgetTree != nullptr ? Preview->WidgetTree->FindWidget(FName(Name)) : nullptr;
}

FString PreviewTitle(FWidgetBlueprintEditor& Editor)
{
	const UTextBlock* Title = Cast<UTextBlock>(PreviewWidget(Editor, TEXT("Title")));
	return Title != nullptr ? Title->GetText().ToString() : FString(TEXT("<no Title in the preview>"));
}

// The Designer selection names only widgets of the Blueprint's current tree.
void TestSelectionIsCurrent(FAutomationTestBase& Test, FWidgetBlueprintEditor& Editor, const UWidgetBlueprint& Blueprint, const FString& Stage)
{
	for (const FWidgetReference& Selected : Editor.GetSelectedWidgets())
	{
		const UWidget* Template = Selected.GetTemplate();
		Test.TestTrue(*FString::Printf(TEXT("%s: the Designer selection holds no removed widget (%s)"), *Stage, *GetPathNameSafe(Template)),
			Template == nullptr || (IsValid(Template) && Blueprint.WidgetTree != nullptr && Blueprint.WidgetTree->FindWidget(Template->GetFName()) == Template));
	}
}
} // namespace AssetDocumentWidgetBlueprintOpenEditorTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentWidgetBlueprintOpenEditorApplyTest, "UEAssetDoc.WidgetBlueprint.OpenEditorApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentWidgetBlueprintOpenEditorApplyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentWidgetBlueprintOpenEditorTests;
	const FString Folder = NewFolder();
	const FTarget Target(Folder, TEXT("WBP_OpenEditor"));
	ON_SCOPE_EXIT { Cleanup({Target}); };
	if (!ApplyOk(*this, Target, Document(Target, TEXT("Create"), InitialBody), TEXT("create")))
	{
		return false;
	}
	UWidgetBlueprint* Blueprint = Target.Get<UWidgetBlueprint>();
	if (!TestNotNull(TEXT("Widget Blueprint exists"), Blueprint) || Open(*this, Blueprint) == nullptr)
	{
		return false;
	}
	FWidgetBlueprintEditor* Editor = EditorOf(Blueprint);
	if (!TestNotNull(TEXT("the Widget Blueprint editor"), Editor))
	{
		return false;
	}
	// The user has OldButton selected in the Designer.
	if (UWidget* OldButton = Blueprint->WidgetTree != nullptr ? Blueprint->WidgetTree->FindWidget(TEXT("OldButton")) : nullptr)
	{
		Editor->SelectWidgets({Editor->GetReferenceFromTemplate(OldButton)}, false);
	}
	Tick();
	TestNotNull(TEXT("before: the Designer preview has OldButton"), PreviewWidget(*Editor, TEXT("OldButton")));
	TestEqual(TEXT("before: the Designer preview's Title"), PreviewTitle(*Editor), FString(TEXT("Welcome")));
	TestEqual(TEXT("before: OldButton is selected"), Editor->GetSelectedWidgets().Num(), 1);
	ReleaseUndoHistory();

	// 1. The apply lands in the open editor.
	if (!ApplyOk(*this, Target, Document(Target, TEXT("Update"), EditedBody), TEXT("apply while open")))
	{
		return false;
	}
	Tick();
	TestTrue(TEXT("the editor is still open on the same Blueprint"), EditorOf(Target.Get<UWidgetBlueprint>()) == Editor);
	TestTrue(TEXT("asset: compiles"), Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
	TestNotNull(TEXT("editor: the Designer preview has NewImage"), PreviewWidget(*Editor, TEXT("NewImage")));
	TestNull(TEXT("editor: the Designer preview no longer has OldButton"), PreviewWidget(*Editor, TEXT("OldButton")));
	TestEqual(TEXT("editor: the Designer preview's Title has the new text"), PreviewTitle(*Editor), FString(TEXT("Hello")));
	TestTrue(TEXT("editor: My Blueprint lists Score"), MyBlueprintLists(*Editor, TEXT("Score")));
	TestSelectionIsCurrent(*this, *Editor, *Blueprint, TEXT("after apply"));
	const FString Applied = Extract(*this, Target, TEXT("after apply"));
	TestTrue(TEXT("extraction has the Print String"), Applied.Contains(TEXT("PrintString")));
	TestUndoKeepsApply(*this, Target, Applied, TEXT("after apply"));

	// 2. A failed apply rolls back while the editor is open.
	ApplyFailingAfterMutation(*this, Target, Document(Target, TEXT("Update"), FailingBody), TEXT("failing apply while open"));
	Tick();
	Blueprint = Target.Get<UWidgetBlueprint>();
	TestEqual(TEXT("after rollback: the extraction is the applied state"), Extract(*this, Target, TEXT("after rollback")), Applied);
	if (TestTrue(TEXT("after rollback: the editor is still open"), Blueprint != nullptr && EditorOf(Blueprint) == Editor))
	{
		TestNotNull(TEXT("after rollback: the Designer preview has NewImage"), PreviewWidget(*Editor, TEXT("NewImage")));
		TestNull(TEXT("after rollback: the Designer preview has no BogusText"), PreviewWidget(*Editor, TEXT("BogusText")));
		TestEqual(TEXT("after rollback: the Designer preview's Title"), PreviewTitle(*Editor), FString(TEXT("Hello")));
		TestFalse(TEXT("after rollback: My Blueprint does not list Bogus"), MyBlueprintLists(*Editor, TEXT("Bogus")));
		TestSelectionIsCurrent(*this, *Editor, *Blueprint, TEXT("after rollback"));
	}

	// 3. The editor's own edit after the apply, and its Undo.
	if (Blueprint != nullptr)
	{
		{
			const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentWidgetBlueprintOpenEditorTests", "AddVariable", "Add Variable"));
			Blueprint->Modify();
			FEdGraphPinType Type;
			Type.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("HumanVar"), Type);
		}
		Editor->RefreshEditors();
		TestTrue(TEXT("the human edit shows in My Blueprint"), MyBlueprintLists(*Editor, TEXT("HumanVar")));
		TestTrue(TEXT("the human edit undoes"), GEditor->UndoTransaction());
		Tick();
		TestFalse(TEXT("after Undo: My Blueprint no longer lists HumanVar"), MyBlueprintLists(*Editor, TEXT("HumanVar")));
		TestEqual(TEXT("after Undo: the extraction is the applied state"), Extract(*this, Target, TEXT("after undo")), Applied);
		TestNotNull(TEXT("after Undo: the Designer preview has NewImage"), PreviewWidget(*Editor, TEXT("NewImage")));
	}

	// 4. The editor closes without prompts or crashes.
	Close(Blueprint);
	TestNull(TEXT("the editor closed"), FindEditor(Target.Get()));
	return !HasAnyErrors();
}
#endif // WITH_DEV_AUTOMATION_TESTS
