// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// UEAssetDoc.AI.Blackboard.OpenEditorApply: a Blackboard open in the Behavior Tree editor's
// Blackboard mode, with a key selected, takes an apply that adds a key and removes the selected one. The editor's
// key list (the Blackboard panel's action menu) then lists the new key and not the removed one; after a failed
// apply it lists the rolled-back keys; the editor's own key edit and its Undo after the apply keep the applied
// state; the editor closes cleanly.

#include "AssetDocumentOpenEditorTestSupport.h"

#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTreeEditor.h"
#include "Misc/ScopeExit.h"
#include "ScopedTransaction.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentAIBlackboardOpenEditorTests
{
using namespace UE::AssetDocument::OpenEditorTests;

FString Key(const TCHAR* Name, const TCHAR* Type, const TCHAR* Properties)
{
	return FString::Printf(TEXT("{\"Name\":\"%s\",\"Category\":\"\",\"Description\":\"\",\"bInstanceSynced\":false,")
		TEXT("\"KeyTypeClass\":{\"Kind\":\"ClassRef\",\"Path\":\"/Script/AIModule.BlackboardKeyType_%s\"},\"KeyTypeProperties\":%s}"),
		Name, Type, Properties);
}

FString Document(const FTarget& Target, const TCHAR* Action, const TArray<FString>& Keys)
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/AIModule.BlackboardData\",\"Action\":\"%s\",")
		TEXT("\"Definitions\":{},\"Properties\":{},\"Body\":{\"Parent\":null,\"Keys\":[%s]}}"),
		*Target.PackageName, Action, *FString::Join(Keys, TEXT(",")));
}

const FString Speed = Key(TEXT("Speed"), TEXT("Float"), TEXT("{\"DefaultValue\":0.0}"));
const FString Alert = Key(TEXT("Alert"), TEXT("Bool"), TEXT("{\"bDefaultValue\":false}"));
const FString Health = Key(TEXT("Health"), TEXT("Int"), TEXT("{\"DefaultValue\":100}"));
const FString Bogus = Key(TEXT("Bogus"), TEXT("Bool"), TEXT("{\"bDefaultValue\":true}"));

// True when the Blackboard panels of the open editor list the key.
bool EditorLists(FAssetEditorToolkit& Editor, const TCHAR* Name)
{
	for (const FName Tab : {FName(TEXT("BehaviorTreeEditor_BlackboardEditor")), FName(TEXT("BehaviorTreeEditor_Blackboard"))})
	{
		const TSharedPtr<SWidget> Content = TabContent(Editor, Tab);
		if (Content.IsValid() && ActionMenuLists(Content.ToSharedRef(), FName(Name)))
		{
			return true;
		}
	}
	return false;
}
} // namespace AssetDocumentAIBlackboardOpenEditorTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBlackboardOpenEditorApplyTest, "UEAssetDoc.AI.Blackboard.OpenEditorApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAIBlackboardOpenEditorApplyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAIBlackboardOpenEditorTests;
	const FTarget Target(NewFolder(), TEXT("BB_OpenEditor"));
	ON_SCOPE_EXIT { Cleanup({Target}); };
	if (!ApplyOk(*this, Target, Document(Target, TEXT("Create"), {Speed, Alert}), TEXT("create")))
	{
		return false;
	}
	UBlackboardData* Blackboard = Target.Get<UBlackboardData>();
	FAssetEditorToolkit* Editor = Open(*this, Blackboard);
	if (Editor == nullptr)
	{
		return false;
	}
	// The user has Alert selected in the Blackboard panel.
	TestTrue(TEXT("before: the Blackboard panel lists Speed"), EditorLists(*Editor, TEXT("Speed")));
	TestTrue(TEXT("before: the Blackboard panel lists Alert (selected)"), EditorLists(*Editor, TEXT("Alert")));
	Tick();
	ReleaseUndoHistory();

	// 1. Health added, Alert removed.
	if (!ApplyOk(*this, Target, Document(Target, TEXT("Update"), {Speed, Health}), TEXT("apply while open")))
	{
		return false;
	}
	Tick();
	TestTrue(TEXT("the editor is still open"), FindToolkit(Blackboard) == Editor);
	TestTrue(TEXT("editor: the Blackboard panel lists Health"), EditorLists(*Editor, TEXT("Health")));
	TestFalse(TEXT("editor: the Blackboard panel no longer lists Alert"), EditorLists(*Editor, TEXT("Alert")));
	const FString Applied = Extract(*this, Target, TEXT("after apply"));
	TestUndoKeepsApply(*this, Target, Applied, TEXT("after apply"));

	// 2. A failed apply rolls back.
	ApplyFailingAfterMutation(*this, Target, Document(Target, TEXT("Update"), {Speed, Health, Bogus}), TEXT("failing apply while open"));
	Tick();
	TestEqual(TEXT("after rollback: the extraction is the applied state"), Extract(*this, Target, TEXT("after rollback")), Applied);
	TestTrue(TEXT("after rollback: the Blackboard panel lists Health"), EditorLists(*Editor, TEXT("Health")));
	TestFalse(TEXT("after rollback: the Blackboard panel does not list Bogus"), EditorLists(*Editor, TEXT("Bogus")));

	// 3. The editor's own New Key after the apply (the panel refreshes itself), and its Undo.
	Blackboard = Target.Get<UBlackboardData>();
	if (Blackboard != nullptr)
	{
		{
			const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentAIBlackboardOpenEditorTests", "NewKey", "New Blackboard Entry"));
			Blackboard->Modify();
			FBlackboardEntry Entry;
			Entry.EntryName = TEXT("HumanKey");
			Entry.KeyType = NewObject<UBlackboardKeyType_Bool>(Blackboard, NAME_None, RF_Transactional);
			Blackboard->Keys.Add(Entry);
		}
		// The panel refreshes itself after its own New Key (as on the editor's Undo).
		static_cast<FBehaviorTreeEditor*>(Editor)->PostUndo(true);
		TestTrue(TEXT("the human key shows in the Blackboard panel"), EditorLists(*Editor, TEXT("HumanKey")));
		TestTrue(TEXT("the human edit undoes"), GEditor->UndoTransaction());
		Tick();
		TestFalse(TEXT("after Undo: the Blackboard panel no longer lists HumanKey"), EditorLists(*Editor, TEXT("HumanKey")));
		TestEqual(TEXT("after Undo: the extraction is the applied state"), Extract(*this, Target, TEXT("after undo")), Applied);
	}
	Close(Blackboard);
	TestNull(TEXT("the editor closed"), FindEditor(Target.Get()));
	return !HasAnyErrors();
}
#endif // WITH_DEV_AUTOMATION_TESTS
