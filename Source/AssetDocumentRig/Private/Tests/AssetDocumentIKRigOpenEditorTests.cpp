// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Open-editor apply of IK Rigs and IK Retargeters (AssetDocumentRigOpenEditor.cpp: close, apply, reopen):
// UEAssetDoc.IKRig.OpenEditorApply.Rig: a rig open in the IK Rig editor; the apply (retarget root, a
//   goal) closes the editor and reopens it on the applied rig (IKRig.EditorReopened): a new editor whose controller
//   reads the applied rig. A failed apply with the editor open rolls back and reopens the editor on the unchanged rig.
//   A human edit through the reopened editor's controller saves, undoes and redoes.
// UEAssetDoc.IKRig.OpenEditorApply.Retargeter: the same for a retargeter open in the IK Retarget editor
//   (an op disabled; IKRetargeter.EditorReopened).

#include "AssetDocumentIKRigTestSupport.h"
#include "AssetDocumentRigOpenEditorTestSupport.h"

#include "Misc/ScopeExit.h"
#include "RetargetEditor/IKRetargetEditor.h"
#include "RetargetEditor/IKRetargetEditorController.h"
#include "RigEditor/IKRigEditorController.h"
#include "RigEditor/IKRigToolkit.h"
#include "Runtime/AssetDocumentUndoHistory.h"
#include "ScopedTransaction.h"
#include "Utils/AssetDocumentJsonKeys.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentIKRigOpenEditorTests
{
using namespace AssetDocumentIKRigTests;
namespace OpenEd = AssetDocumentRigOpenEditorTests;

TSharedPtr<FJsonObject> ForTarget(const TSharedPtr<FJsonObject>& Document, const FTarget& Target)
{
	const TSharedPtr<FJsonObject> Copy = Parse(Serialize(Document));
	Copy->SetStringField(TEXT("Target"), Target.PackageName);
	Copy->SetStringField(TEXT("Action"), TEXT("Update"));
	return Copy;
}

void LogResult(FAutomationTestBase& Test, const TCHAR* Stage, const FAssetDocumentResult& Result)
{
	Test.AddInfo(FString::Printf(TEXT("%s: %s"), Stage, *DiagnosticsText(Result)));
}

// Opens Asset in its editor; false (with a warning) when no editor opens in this session.
bool OpenEditor(FAutomationTestBase& Test, UObject& Asset, const TCHAR* EditorName)
{
	UAssetEditorSubsystem* Subsystem = OpenEd::Editors();
	if (Subsystem == nullptr || !Subsystem->OpenEditorForAsset(&Asset))
	{
		Test.AddWarning(FString::Printf(TEXT("The %s did not open in this session; the open-editor apply was not exercised."), EditorName));
		return false;
	}
	OpenEd::TickEditorUi();
	// The human does not save after opening.
	Test.TestFalse(*FString::Printf(TEXT("opening the %s leaves the package clean"), EditorName), Asset.GetOutermost()->IsDirty());
	ReleaseUndoHistory();
	return true;
}

// The apply closed the editor and reopened it on the asset: a new editor instance (Reopen watched the apply).
void TestReopened(FAutomationTestBase& Test, const FTarget& Target, const FAssetDocumentResult& Result, const TCHAR* Code,
	const UE::AssetDocument::OpenEditorTests::FEditorReopenWatch& Reopen, const TCHAR* Stage)
{
	Test.TestTrue(*FString::Printf(TEXT("%s: %s (%s)"), Stage, Code, *DiagnosticsText(Result)), HasCode(Result, Code));
	UObject* Asset = Target.Get();
	const IAssetEditorInstance* After = Asset != nullptr ? OpenEd::EditorOf(*Asset) : nullptr;
	Test.TestNotNull(*FString::Printf(TEXT("%s: the editor is open again on the asset"), Stage), After);
	Test.TestTrue(*FString::Printf(TEXT("%s: the apply closed the open editor"), Stage), Reopen.Closed());
	Test.TestTrue(*FString::Printf(TEXT("%s: a new editor, built from the asset"), Stage), After != nullptr && After == Reopen.Reopened());
}

// Undo / redo of a human edit (an editor transaction) with the editor open; saving keeps the state. Undo restores
// FName-keyed entries (a chain "Head" on bone "head") with the case of the name table's first entry, which also moves
// them in the extract's name order, so states compare without case and key order: the names are equal FNames.
TSharedPtr<FJsonValue> SortedKeys(const TSharedPtr<FJsonValue>& Value)
{
	if (Value->Type == EJson::Object)
	{
		TArray<FString> Keys;
		AssetDocumentServerJson::GetKeys(*Value->AsObject(), Keys);
		Keys.Sort();
		const TSharedPtr<FJsonObject> Sorted = MakeShared<FJsonObject>();
		for (const FString& Key : Keys)
		{
			Sorted->SetField(Key, SortedKeys(AssetDocumentServerJson::FindFieldRef(*Value->AsObject(), Key)));
		}
		return MakeShared<FJsonValueObject>(Sorted);
	}
	if (Value->Type == EJson::Array)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			Items.Add(SortedKeys(Item));
		}
		return MakeShared<FJsonValueArray>(Items);
	}
	return Value;
}

FString CaseFolded(const FString& State)
{
	const TSharedPtr<FJsonObject> Parsed = Parse(State.ToLower());
	return Parsed.IsValid() ? Serialize(SortedKeys(MakeShared<FJsonValueObject>(Parsed))->AsObject()) : State.ToLower();
}

bool SameState(FAutomationTestBase& Test, const TCHAR* What, const FString& Actual, const FString& Expected)
{
	const bool bSame = CaseFolded(Actual) == CaseFolded(Expected);
	if (!Test.TestTrue(What, bSame))
	{
		Test.AddInfo(FString::Printf(TEXT("%s: expected %s, got %s"), What, *Expected, *Actual));
	}
	else if (Actual != Expected)
	{
		Test.AddInfo(FString::Printf(TEXT("%s: equal except for the case of FName-keyed entries"), What));
	}
	return bSame;
}

void TestHumanEdit(FAutomationTestBase& Test, const FTarget& Target, const FString& Applied, TFunctionRef<void()> Edit)
{
	ReleaseUndoHistory();
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentIKRigOpenEditorTests", "HumanEdit", "Edit"));
		Edit();
	}
	const FString Human = Serialize(ExtractResult(Target.ObjectPath).Payload);
	Test.TestNotEqual(TEXT("the human edit landed"), Human, Applied);
	UObject* Asset = Target.Get();
	Test.TestTrue(TEXT("the human's save"), Asset != nullptr && OpenEd::HumanSave(Test, *Asset));
	Test.TestFalse(TEXT("saved"), Asset == nullptr || Asset->GetOutermost()->IsDirty());
	SameState(Test, TEXT("saving keeps the applied and the human state"), Serialize(ExtractResult(Target.ObjectPath).Payload), Human);
	Test.TestTrue(TEXT("the human edit undoes"), GEditor->Trans->CanUndo() && GEditor->UndoTransaction());
	OpenEd::TickEditorUi();
	SameState(Test, TEXT("undo restores the applied asset"), Serialize(ExtractResult(Target.ObjectPath).Payload), Applied);
	Test.TestTrue(TEXT("the human edit redoes"), GEditor->Trans->CanRedo() && GEditor->RedoTransaction());
	OpenEd::TickEditorUi();
	SameState(Test, TEXT("redo restores the human edit"), Serialize(ExtractResult(Target.ObjectPath).Payload), Human);
	ReleaseUndoHistory();
	if (UObject* Saved = Target.Get())
	{
		Saved->GetOutermost()->SetDirtyFlag(false);
	}
}

void CloseEditor(FAutomationTestBase& Test, const FTarget& Target)
{
	if (UObject* Asset = Target.Get())
	{
		OpenEd::Editors()->CloseAllEditorsForAsset(Asset);
		Test.TestNull(TEXT("the editor closed"), OpenEd::EditorOf(*Asset));
	}
}
} // namespace AssetDocumentIKRigOpenEditorTests

namespace IKRigOpenEd = AssetDocumentIKRigOpenEditorTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRigOpenEditorApplyRigTest, "UEAssetDoc.IKRig.OpenEditorApply.Rig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRigOpenEditorApplyRigTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRigTests;
	namespace OpenEd = AssetDocumentRigOpenEditorTests;
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	FFixtures Fixtures;
	const FTarget& Target = Fixtures.Add(TEXT("IK_Open"));
	UIKRigDefinition* Rig = MakeRig(*this, Target, TutorialMesh, true);
	if (Rig == nullptr || !IKRigOpenEd::OpenEditor(*this, *Rig, TEXT("IK Rig editor")))
	{
		return Rig != nullptr;
	}
	ON_SCOPE_EXIT
	{
		if (UObject* Open = Target.Get())
		{
			OpenEd::Editors()->CloseAllEditorsForAsset(Open);
		}
	};
	const IAssetEditorInstance* EditorBefore = OpenEd::EditorOf(*Rig);
	TestNotNull(TEXT("the IK Rig editor"), OpenEd::ToolkitOf<FIKRigEditorToolkit>(*Rig, TEXT("IKRigEditor")));
	const TSharedPtr<FJsonObject> State = Extract(*this, Target.ObjectPath, TEXT("before"));
	const TSharedPtr<FJsonObject> Edit = IKRigOpenEd::ForTarget(State, Target);
	BodyOf(Edit)->SetStringField(TEXT("RetargetRoot"), TEXT("spine_01"));
	UE::AssetDocument::OpenEditorTests::FEditorReopenWatch Reopen(Target.PackageName, EditorBefore);
	const FAssetDocumentResult Applied = Apply(Target, Edit);
	Reopen.Stop();
	IKRigOpenEd::LogResult(*this, TEXT("apply while open"), Applied);
	if (!TestTrue(TEXT("apply while open"), Applied.IsSuccess()))
	{
		return false;
	}
	OpenEd::TickEditorUi();
	IKRigOpenEd::TestReopened(*this, Target, Applied, TEXT("IKRig.EditorReopened"), Reopen, TEXT("after the apply"));
	Rig = Cast<UIKRigDefinition>(Target.Get());
	if (!TestNotNull(TEXT("rig after the apply"), Rig))
	{
		return false;
	}
	TestEqual(TEXT("the retarget root"), Rig->GetPelvis(), FName(TEXT("spine_01")));
	TestFalse(TEXT("the package is saved"), Rig->GetOutermost()->IsDirty());
	TestFalse(TEXT("the apply is not an undo step"), UE::AssetDocument::UndoHistory::ReferencesPackage(Target.PackageName));
	FIKRigEditorToolkit* Toolkit = OpenEd::ToolkitOf<FIKRigEditorToolkit>(*Rig, TEXT("IKRigEditor"));
	if (TestNotNull(TEXT("the reopened IK Rig editor"), Toolkit))
	{
		const TSharedRef<FIKRigEditorController> Controller = Toolkit->GetController();
		TestTrue(TEXT("the editor's controller edits the rig"), Controller->AssetController != nullptr && Controller->AssetController->GetAsset() == Rig);
		TestEqual(TEXT("the editor reads the applied retarget root"), Controller->AssetController != nullptr
			? Controller->AssetController->GetRetargetRoot() : FName(), FName(TEXT("spine_01")));
		TestNotNull(TEXT("the editor's preview runs the rig"), Controller->AnimInstance.Get());
	}
	const FString AppliedText = Serialize(Extract(*this, Target.ObjectPath, TEXT("applied")));

	// A failure (a goal on an unknown bone) after a valid change rolls back with the editor open.
	{
		const IAssetEditorInstance* EditorApplied = OpenEd::EditorOf(*Rig);
		const TSharedPtr<FJsonObject> Bad = IKRigOpenEd::ForTarget(Parse(AppliedText), Target);
		BodyOf(Bad)->SetStringField(TEXT("RetargetRoot"), TEXT("pelvis"));
		const TSharedPtr<FJsonObject>* Goals = nullptr;
		if (!BodyOf(Bad)->TryGetObjectField(TEXT("Goals"), Goals))
		{
			BodyOf(Bad)->SetObjectField(TEXT("Goals"), MakeShared<FJsonObject>());
			BodyOf(Bad)->TryGetObjectField(TEXT("Goals"), Goals);
		}
		(*Goals)->SetObjectField(TEXT("Bad"), Parse(TEXT(R"({"Bone": "no_such_bone"})")));
		const FAssetDocumentResult Failed = Apply(Target, Bad);
		IKRigOpenEd::LogResult(*this, TEXT("failing apply while open"), Failed);
		TestFalse(TEXT("failing apply while open fails"), Failed.IsSuccess());
		TestTrue(TEXT("failing apply: IKRig.BoneNotFound"), HasCode(Failed, TEXT("IKRig.BoneNotFound")));
		OpenEd::TickEditorUi();
		TestEqual(TEXT("after the rollback the rig is as applied"), Serialize(Extract(*this, Target.ObjectPath, TEXT("rolled back"))), AppliedText);
		Rig = Cast<UIKRigDefinition>(Target.Get());
		if (!TestNotNull(TEXT("rig after the rollback"), Rig))
		{
			return false;
		}
		TestFalse(TEXT("after the rollback the package is clean"), Rig->GetOutermost()->IsDirty());
		TestNotNull(TEXT("after the rollback the editor is open"), OpenEd::EditorOf(*Rig));
		AddInfo(FString::Printf(TEXT("failed apply: the editor was %s"), OpenEd::EditorOf(*Rig) == EditorApplied ? TEXT("kept") : TEXT("reopened")));
		FIKRigEditorToolkit* RolledBack = OpenEd::ToolkitOf<FIKRigEditorToolkit>(*Rig, TEXT("IKRigEditor"));
		TestTrue(TEXT("after the rollback the editor reads the applied retarget root"), RolledBack != nullptr
			&& RolledBack->GetController()->AssetController != nullptr
			&& RolledBack->GetController()->AssetController->GetRetargetRoot() == FName(TEXT("spine_01")));
	}

	// A human edit through the reopened editor's controller (the hierarchy's "Set Retarget Root").
	IKRigOpenEd::TestHumanEdit(*this, Target, AppliedText, [&Target]()
	{
		UIKRigDefinition* Edited = Cast<UIKRigDefinition>(Target.Get());
		FIKRigEditorToolkit* Open = Edited != nullptr ? AssetDocumentRigOpenEditorTests::ToolkitOf<FIKRigEditorToolkit>(*Edited, TEXT("IKRigEditor")) : nullptr;
		if (Open != nullptr && Open->GetController()->AssetController != nullptr)
		{
			Open->GetController()->AssetController->SetRetargetRoot(TEXT("pelvis"));
		}
	});
	IKRigOpenEd::CloseEditor(*this, Target);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentIKRigOpenEditorApplyRetargeterTest, "UEAssetDoc.IKRig.OpenEditorApply.Retargeter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentIKRigOpenEditorApplyRetargeterTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentIKRigTests;
	namespace OpenEd = AssetDocumentRigOpenEditorTests;
	if (GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return false;
	}
	FFixtures Fixtures;
	UIKRigDefinition* Rig = MakeRig(*this, Fixtures.Add(TEXT("IK_OpenRtg")), TutorialMesh, true);
	const FTarget& Target = Fixtures.Add(TEXT("RTG_Open"));
	UIKRetargeter* Retargeter = Rig != nullptr ? MakeRetargeter(*this, Target, Rig, Rig) : nullptr;
	if (Retargeter == nullptr || !IKRigOpenEd::OpenEditor(*this, *Retargeter, TEXT("IK Retarget editor")))
	{
		return Retargeter != nullptr;
	}
	ON_SCOPE_EXIT
	{
		if (UObject* Open = Target.Get())
		{
			OpenEd::Editors()->CloseAllEditorsForAsset(Open);
		}
	};
	const IAssetEditorInstance* EditorBefore = OpenEd::EditorOf(*Retargeter);
	TestNotNull(TEXT("the IK Retarget editor"), OpenEd::ToolkitOf<FIKRetargetEditor>(*Retargeter, TEXT("IKRetargetEditor")));
	const TSharedPtr<FJsonObject> State = Extract(*this, Target.ObjectPath, TEXT("before"));
	const TSharedPtr<FJsonObject> Edit = IKRigOpenEd::ForTarget(State, Target);
	const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
	if (!TestTrue(TEXT("the retargeter has ops"), BodyOf(Edit)->TryGetArrayField(TEXT("Ops"), Ops) && !Ops->IsEmpty()))
	{
		return false;
	}
	(*Ops)[0]->AsObject()->SetBoolField(TEXT("Enabled"), false);
	UE::AssetDocument::OpenEditorTests::FEditorReopenWatch Reopen(Target.PackageName, EditorBefore);
	const FAssetDocumentResult Applied = Apply(Target, Edit);
	Reopen.Stop();
	IKRigOpenEd::LogResult(*this, TEXT("apply while open"), Applied);
	if (!TestTrue(TEXT("apply while open"), Applied.IsSuccess()))
	{
		return false;
	}
	OpenEd::TickEditorUi();
	IKRigOpenEd::TestReopened(*this, Target, Applied, TEXT("IKRetargeter.EditorReopened"), Reopen, TEXT("after the apply"));
	Retargeter = Cast<UIKRetargeter>(Target.Get());
	if (!TestNotNull(TEXT("retargeter after the apply"), Retargeter))
	{
		return false;
	}
	TestFalse(TEXT("the package is saved"), Retargeter->GetOutermost()->IsDirty());
	FIKRetargetEditor* Toolkit = OpenEd::ToolkitOf<FIKRetargetEditor>(*Retargeter, TEXT("IKRetargetEditor"));
	if (TestNotNull(TEXT("the reopened IK Retarget editor"), Toolkit))
	{
		const TSharedRef<FIKRetargetEditorController> Controller = Toolkit->GetController();
		TestTrue(TEXT("the editor's controller edits the retargeter"), Controller->AssetController != nullptr
			&& Controller->AssetController->GetAsset() == Retargeter);
		TestFalse(TEXT("the editor reads the disabled op"), Controller->AssetController == nullptr || Controller->AssetController->GetRetargetOpEnabled(0));
	}
	const FString AppliedText = Serialize(Extract(*this, Target.ObjectPath, TEXT("applied")));

	// A failure (an unknown op type) after a valid change rolls back with the editor open.
	{
		const TSharedPtr<FJsonObject> Bad = IKRigOpenEd::ForTarget(Parse(AppliedText), Target);
		TArray<TSharedPtr<FJsonValue>> BadOps = BodyOf(Bad)->GetArrayField(TEXT("Ops"));
		BadOps[0]->AsObject()->SetBoolField(TEXT("Enabled"), true);
		BadOps.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"({"Name": "X", "Type": "NoSuchOp"})"))));
		BodyOf(Bad)->SetArrayField(TEXT("Ops"), BadOps);
		const FAssetDocumentResult Failed = Apply(Target, Bad);
		IKRigOpenEd::LogResult(*this, TEXT("failing apply while open"), Failed);
		TestFalse(TEXT("failing apply while open fails"), Failed.IsSuccess());
		TestTrue(TEXT("failing apply: IKRetargeter.UnknownOpType"), HasCode(Failed, TEXT("IKRetargeter.UnknownOpType")));
		OpenEd::TickEditorUi();
		TestEqual(TEXT("after the rollback the retargeter is as applied"), Serialize(Extract(*this, Target.ObjectPath, TEXT("rolled back"))), AppliedText);
		Retargeter = Cast<UIKRetargeter>(Target.Get());
		if (!TestNotNull(TEXT("retargeter after the rollback"), Retargeter))
		{
			return false;
		}
		TestFalse(TEXT("after the rollback the package is clean"), Retargeter->GetOutermost()->IsDirty());
		FIKRetargetEditor* RolledBack = OpenEd::ToolkitOf<FIKRetargetEditor>(*Retargeter, TEXT("IKRetargetEditor"));
		TestTrue(TEXT("after the rollback the editor reads the disabled op"), RolledBack != nullptr
			&& RolledBack->GetController()->AssetController != nullptr && !RolledBack->GetController()->AssetController->GetRetargetOpEnabled(0));
	}

	// A human edit through the reopened editor's controller (the op stack's enable checkbox).
	IKRigOpenEd::TestHumanEdit(*this, Target, AppliedText, [&Target]()
	{
		UIKRetargeter* Edited = Cast<UIKRetargeter>(Target.Get());
		FIKRetargetEditor* Open = Edited != nullptr ? AssetDocumentRigOpenEditorTests::ToolkitOf<FIKRetargetEditor>(*Edited, TEXT("IKRetargetEditor")) : nullptr;
		if (Open != nullptr && Open->GetController()->AssetController != nullptr)
		{
			Open->GetController()->AssetController->SetRetargetOpEnabled(0, true);
		}
	});
	IKRigOpenEd::CloseEditor(*this, Target);
	return true;
}

#endif
