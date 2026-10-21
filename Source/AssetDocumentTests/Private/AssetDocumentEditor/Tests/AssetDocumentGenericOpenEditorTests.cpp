// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// UEAssetDoc.Generic.OpenEditorApply: a generic asset (UTestDataAsset) open in the plain
// details-panel asset editor takes an apply that changes its properties. The editor's details panel edits the asset
// itself (no working copy that could write an old state back) and shows its properties after the apply, after a
// failed apply (rolled back) and after the editor's own property edit and its Undo; the editor closes cleanly.

#include "AssetDocumentOpenEditorTestSupport.h"

#include "Misc/ScopeExit.h"
#include "ScopedTransaction.h"
#include "TestDataAsset.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentGenericOpenEditorTests
{
using namespace UE::AssetDocument::OpenEditorTests;

FString Document(const FTarget& Target, const TCHAR* Action, const TCHAR* Properties)
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/AssetDocumentTests.TestDataAsset\",\"Action\":\"%s\",")
		TEXT("\"Definitions\":{},\"Properties\":%s}"), *Target.PackageName, Action, Properties);
}

// The details panel of the open editor shows exactly the asset (the live object, not a copy) and its properties.
void TestDetailsShowAsset(FAutomationTestBase& Test, FAssetEditorToolkit& Editor, UObject& Asset, const FString& Stage)
{
	const TArray<IDetailsView*> Views = DetailsViewsIn(Editor, TEXT("GenericEditor_Properties"));
	if (!Test.TestEqual(*FString::Printf(TEXT("%s: the editor has one details panel"), *Stage), Views.Num(), 1))
	{
		return;
	}
	const TArray<TWeakObjectPtr<UObject>>& Shown = Views[0]->GetSelectedObjects();
	Test.TestTrue(*FString::Printf(TEXT("%s: the details panel edits the asset itself"), *Stage), Shown.Num() == 1 && Shown[0].Get() == &Asset);
	Test.TestTrue(*FString::Printf(TEXT("%s: the details panel shows TestInt"), *Stage), DisplayedProperties(*Views[0]).Contains(TEXT("TestInt")));
}
} // namespace AssetDocumentGenericOpenEditorTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentGenericOpenEditorApplyTest, "UEAssetDoc.Generic.OpenEditorApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentGenericOpenEditorApplyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentGenericOpenEditorTests;
	const FTarget Target(NewFolder(), TEXT("DA_OpenEditor"));
	ON_SCOPE_EXIT { Cleanup({Target}); };
	if (!ApplyOk(*this, Target, Document(Target, TEXT("Create"), TEXT("{\"TestString\":\"a\",\"TestInt\":1}")), TEXT("create")))
	{
		return false;
	}
	UTestDataAsset* Asset = Target.Get<UTestDataAsset>();
	FAssetEditorToolkit* Editor = Open(*this, Asset);
	if (Editor == nullptr)
	{
		return false;
	}
	TestDetailsShowAsset(*this, *Editor, *Asset, TEXT("before"));
	ReleaseUndoHistory();

	// 1. Properties changed.
	if (!ApplyOk(*this, Target, Document(Target, TEXT("Update"), TEXT("{\"TestString\":\"b\",\"TestInt\":2}")), TEXT("apply while open")))
	{
		return false;
	}
	Tick();
	TestTrue(TEXT("the editor is still open"), FindToolkit(Asset) == Editor);
	TestTrue(TEXT("asset: the applied values"), Asset->TestString == TEXT("b") && Asset->TestInt == 2);
	TestDetailsShowAsset(*this, *Editor, *Asset, TEXT("after apply"));
	const FString Applied = Extract(*this, Target, TEXT("after apply"));
	TestUndoKeepsApply(*this, Target, Applied, TEXT("after apply"));

	// 2. A failed apply rolls back.
	ApplyFailingAfterMutation(*this, Target, Document(Target, TEXT("Update"), TEXT("{\"TestString\":\"c\",\"TestInt\":3}")), TEXT("failing apply while open"));
	Tick();
	TestEqual(TEXT("after rollback: the extraction is the applied state"), Extract(*this, Target, TEXT("after rollback")), Applied);
	Asset = Target.Get<UTestDataAsset>();
	if (Asset != nullptr)
	{
		TestDetailsShowAsset(*this, *Editor, *Asset, TEXT("after rollback"));

		// 3. The editor's own property edit after the apply, and its Undo.
		{
			const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentGenericOpenEditorTests", "Edit", "Edit Property"));
			Asset->Modify();
			Asset->TestInt = 7;
		}
		TestTrue(TEXT("the human edit undoes"), GEditor->UndoTransaction());
		Tick();
		TestEqual(TEXT("after Undo: TestInt is the applied value"), Asset->TestInt, 2);
		TestEqual(TEXT("after Undo: the extraction is the applied state"), Extract(*this, Target, TEXT("after undo")), Applied);
		TestDetailsShowAsset(*this, *Editor, *Asset, TEXT("after undo"));
	}
	Close(Asset);
	TestNull(TEXT("the editor closed"), FindEditor(Target.Get()));
	return !HasAnyErrors();
}
#endif // WITH_DEV_AUTOMATION_TESTS
