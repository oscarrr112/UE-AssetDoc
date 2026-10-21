// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// UEAssetDoc.AnimBlueprint.OpenEditorApply: an Anim Blueprint open in the Animation Blueprint
// editor (Persona) takes an apply that adds a variable, a Two Way Blend bound to it in the AnimGraph and a state
// machine state, and removes a function graph. The open editor then shows what it shows after the same edits made
// in it: My Blueprint lists the new variable and not the removed function, the removed function's tab is closed,
// and the Persona preview instance runs the newly compiled class (it has the new variable). A failed apply rolls
// back and the preview runs the rolled-back class; the editor's own edit and Undo after the apply keep the applied
// state; the editor closes cleanly.

#include "AssetDocumentOpenEditorTestSupport.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "IAnimationBlueprintEditor.h"
#include "IPersonaToolkit.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/ScopeExit.h"
#include "SMyBlueprint.h"
#include "ScopedTransaction.h"
#include "UObject/WeakObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentAnimBlueprintOpenEditorTests
{
using namespace UE::AssetDocument::OpenEditorTests;

const TCHAR* Skeleton = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
const TCHAR* PreviewMesh = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP");
const TCHAR* IdleAnim = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");
const TCHAR* WalkAnim = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Walk_Fwd.Tutorial_Walk_Fwd");

FString Document(const FTarget& Target, const TCHAR* Action, const FString& Body)
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.AnimBlueprint\",\"Action\":\"%s\",")
		TEXT("\"Body\":{\"ParentClass\":\"/Script/Engine.AnimInstance\",\"TargetSkeleton\":\"%s\",\"Preview\":{\"Mesh\":\"%s\"},%s}}"),
		*Target.PackageName, Action, Skeleton, PreviewMesh, *Body);
}

FString InitialBody()
{
	return FString::Printf(TEXT(
		"\"Variables\":[{\"Name\":\"Speed\",\"Type\":\"float\"}],"
		"\"FunctionGraphs\":[{\"Name\":\"OldFn\",\"Nodes\":[],\"Links\":[{\"From\":\"Entry:then\",\"To\":\"Result:execute\"}]}],"
		"\"AnimGraph\":{\"Nodes\":[{\"Id\":\"Idle\",\"Action\":\"Anim:SequencePlayer\",\"Properties\":{\"Node.Sequence\":\"%s\"}}],"
		"\"Links\":[{\"From\":\"Idle:Pose\",\"To\":\"Output:Result\"}]}"), IdleAnim);
}

// Adds Weight, a Two Way Blend bound to it and a Locomotion state machine; removes OldFn.
FString EditedBody()
{
	return FString::Printf(TEXT(
		"\"Variables\":[{\"Name\":\"Speed\",\"Type\":\"float\"},{\"Name\":\"Weight\",\"Type\":\"float\",\"DefaultValue\":0.25}],"
		"\"FunctionGraphs\":[],"
		"\"AnimGraph\":{\"Nodes\":["
		"{\"Id\":\"Idle\",\"Action\":\"Anim:SequencePlayer\",\"Properties\":{\"Node.Sequence\":\"%s\"}},"
		"{\"Id\":\"Locomotion\",\"Action\":\"Anim:StateMachine\",\"Graph\":{\"Nodes\":["
		"{\"Id\":\"Walk\",\"Action\":\"SM:State\",\"Graph\":{\"Nodes\":["
		"{\"Id\":\"Play\",\"Action\":\"Anim:SequencePlayer\",\"Properties\":{\"Node.Sequence\":\"%s\"}}],"
		"\"Links\":[{\"From\":\"Play:Pose\",\"To\":\"Output:Result\"}]}}],"
		"\"Links\":[{\"From\":\"Entry:Entry\",\"To\":\"Walk:In\"}]}},"
		"{\"Id\":\"Blend\",\"Action\":\"Anim:TwoWayBlend\",\"Properties\":{\"Bindings\":{\"Alpha\":{\"Path\":\"Weight\"}}}}],"
		"\"Links\":[{\"From\":\"Idle:Pose\",\"To\":\"Blend:A\"},{\"From\":\"Locomotion:Pose\",\"To\":\"Blend:B\"},"
		"{\"From\":\"Blend:Pose\",\"To\":\"Output:Result\"}]}"), IdleAnim, WalkAnim);
}

// The edit a failed apply rolls back: adds Bogus and a function graph.
FString FailingBody()
{
	return TEXT("\"Variables\":[{\"Name\":\"Speed\",\"Type\":\"float\"},{\"Name\":\"Weight\",\"Type\":\"float\",\"DefaultValue\":0.25},"
		"{\"Name\":\"Bogus\",\"Type\":\"int\"}],"
		"\"FunctionGraphs\":[{\"Name\":\"FailFn\",\"Nodes\":[],\"Links\":[{\"From\":\"Entry:then\",\"To\":\"Result:execute\"}]}]");
}

IAnimationBlueprintEditor* EditorOf(UAnimBlueprint* Blueprint)
{
	return static_cast<IAnimationBlueprintEditor*>(FindEditor(Blueprint));
}

bool MyBlueprintLists(IAnimationBlueprintEditor& Editor, const TCHAR* Name)
{
	const TSharedPtr<SMyBlueprint> MyBlueprint = Editor.GetMyBlueprintWidget();
	const bool bListed = MyBlueprint.IsValid() && ActionMenuLists(MyBlueprint.ToSharedRef(), FName(Name));
	if (bListed)
	{
		MyBlueprint->ClearGraphActionMenuSelection();
	}
	return bListed;
}

// The anim instance the Persona viewport runs.
UAnimInstance* PreviewInstance(IAnimationBlueprintEditor& Editor)
{
	const UDebugSkelMeshComponent* Mesh = Editor.GetPersonaToolkit()->GetPreviewMeshComponent();
	return Mesh != nullptr ? Mesh->GetAnimInstance() : nullptr;
}

void TestPreviewRunsClass(FAutomationTestBase& Test, IAnimationBlueprintEditor& Editor, const UAnimBlueprint& Blueprint,
	const TCHAR* Present, const TCHAR* Absent, const FString& Stage)
{
	const UAnimInstance* Instance = PreviewInstance(Editor);
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s: the Persona preview has an anim instance"), *Stage), Instance))
	{
		return;
	}
	Test.AddInfo(FString::Printf(TEXT("%s: preview instance %s of %s"), *Stage, *Instance->GetName(), *Instance->GetClass()->GetPathName()));
	Test.TestTrue(*FString::Printf(TEXT("%s: the preview runs the Blueprint's current class"), *Stage), Instance->GetClass() == Blueprint.GeneratedClass);
	Test.TestNotNull(*FString::Printf(TEXT("%s: the preview instance has %s"), *Stage, Present), FindFProperty<FProperty>(Instance->GetClass(), Present));
	Test.TestNull(*FString::Printf(TEXT("%s: the preview instance has no %s"), *Stage, Absent), FindFProperty<FProperty>(Instance->GetClass(), Absent));
}

UEdGraph* FunctionGraph(const UBlueprint* Blueprint, const TCHAR* Name)
{
	if (Blueprint == nullptr)
	{
		return nullptr;
	}
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph != nullptr && Graph->GetName() == Name)
		{
			return Graph;
		}
	}
	return nullptr;
}
} // namespace AssetDocumentAnimBlueprintOpenEditorTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimBlueprintOpenEditorApplyTest, "UEAssetDoc.AnimBlueprint.OpenEditorApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimBlueprintOpenEditorApplyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentAnimBlueprintOpenEditorTests;
	const FString Folder = NewFolder();
	const FTarget Target(Folder, TEXT("ABP_OpenEditor"));
	ON_SCOPE_EXIT { Cleanup({Target}); };
	if (!ApplyOk(*this, Target, Document(Target, TEXT("Create"), InitialBody()), TEXT("create")))
	{
		return false;
	}
	UAnimBlueprint* Blueprint = Target.Get<UAnimBlueprint>();
	if (!TestNotNull(TEXT("Anim Blueprint exists"), Blueprint) || Open(*this, Blueprint) == nullptr)
	{
		return false;
	}
	IAnimationBlueprintEditor* Editor = EditorOf(Blueprint);
	TWeakObjectPtr<UEdGraph> OldFn = FunctionGraph(Blueprint, TEXT("OldFn"));
	if (!TestNotNull(TEXT("the Animation Blueprint editor"), Editor) || !TestTrue(TEXT("OldFn exists"), OldFn.IsValid()))
	{
		return false;
	}
	Editor->OpenGraphAndBringToFront(OldFn.Get());
	Tick();
	TestTrue(TEXT("before: My Blueprint lists OldFn"), MyBlueprintLists(*Editor, TEXT("OldFn")));
	TestPreviewRunsClass(*this, *Editor, *Blueprint, TEXT("Speed"), TEXT("Weight"), TEXT("before"));
	ReleaseUndoHistory();

	// 1. The apply lands in the open editor and its preview.
	if (!ApplyOk(*this, Target, Document(Target, TEXT("Update"), EditedBody()), TEXT("apply while open")))
	{
		return false;
	}
	Tick();
	TestTrue(TEXT("the editor is still open on the same Blueprint"), EditorOf(Target.Get<UAnimBlueprint>()) == Editor);
	TestNull(TEXT("asset: OldFn removed"), FunctionGraph(Blueprint, TEXT("OldFn")));
	TestTrue(TEXT("asset: compiles"), Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
	TestTrue(TEXT("editor: My Blueprint lists Weight"), MyBlueprintLists(*Editor, TEXT("Weight")));
	TestFalse(TEXT("editor: My Blueprint no longer lists OldFn"), MyBlueprintLists(*Editor, TEXT("OldFn")));
	{
		TArray<TSharedPtr<SDockTab>> Tabs;
		if (UEdGraph* Removed = OldFn.Get(true))
		{
			Editor->FindOpenTabsContainingDocument(Removed, Tabs);
		}
		TestEqual(TEXT("editor: the removed OldFn's tab is closed"), Tabs.Num(), 0);
	}
	TestPreviewRunsClass(*this, *Editor, *Blueprint, TEXT("Weight"), TEXT("Bogus"), TEXT("after apply"));
	const FString Applied = Extract(*this, Target, TEXT("after apply"));
	TestTrue(TEXT("extraction has the Two Way Blend"), Applied.Contains(TEXT("Anim:TwoWayBlend")));
	TestUndoKeepsApply(*this, Target, Applied, TEXT("after apply"));

	// 2. A failed apply rolls back while the editor is open.
	ApplyFailingAfterMutation(*this, Target, Document(Target, TEXT("Update"), FailingBody()), TEXT("failing apply while open"));
	Tick();
	Blueprint = Target.Get<UAnimBlueprint>();
	TestEqual(TEXT("after rollback: the extraction is the applied state"), Extract(*this, Target, TEXT("after rollback")), Applied);
	if (TestTrue(TEXT("after rollback: the editor is still open"), Blueprint != nullptr && EditorOf(Blueprint) == Editor))
	{
		TestFalse(TEXT("after rollback: My Blueprint does not list Bogus"), MyBlueprintLists(*Editor, TEXT("Bogus")));
		TestFalse(TEXT("after rollback: My Blueprint does not list FailFn"), MyBlueprintLists(*Editor, TEXT("FailFn")));
		TestPreviewRunsClass(*this, *Editor, *Blueprint, TEXT("Weight"), TEXT("Bogus"), TEXT("after rollback"));
	}

	// 3. The editor's own edit after the apply, and its Undo.
	if (Blueprint != nullptr)
	{
		{
			const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentAnimBlueprintOpenEditorTests", "AddVariable", "Add Variable"));
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
	}

	// 4. The editor closes without prompts or crashes.
	Close(Blueprint);
	TestNull(TEXT("the editor closed"), FindEditor(Target.Get()));
	return !HasAnyErrors();
}
#endif // WITH_DEV_AUTOMATION_TESTS
