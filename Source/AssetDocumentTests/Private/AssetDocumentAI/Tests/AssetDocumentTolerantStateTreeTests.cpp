// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An editor-made StateTree holding what a document could not show or re-apply: two sibling states of one name (a
// Duplicate kept without a rename) and a transition to the second, a state named with '/' and ':', two tasks of one
// name, a property function whose input is bound to another property function, an evaluator value the document
// codec has no form for (an instanced object) and one it reads as a marker (a NaN float). It reads with warnings and
// markers, its own extract re-applies as a no-op, another edit works and keeps all of it, and new input of the same
// kind is still refused.

#include "AssetDocumentStateTreeTestTypes.h"
#include "AssetDocumentTolerantTestSupport.h"

#include "Misc/ScopeExit.h"
#include "PropertyBindingPath.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEditorPropertyBindings.h"
#include "StateTreeNodeBase.h"
#include "StateTreeState.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentTolerantStateTreeTestsLocal
{
// The test evaluator is meta=(Hidden); the test lifts it while it runs (the profile refuses Hidden types as the node
// picker does).
struct FScopedTestTypesVisible
{
	FScopedTestTypesVisible() { FAssetDocumentStateTreeTestEvaluator::StaticStruct()->RemoveMetaData(TEXT("Hidden")); }
	~FScopedTestTypesVisible() { FAssetDocumentStateTreeTestEvaluator::StaticStruct()->SetMetaData(TEXT("Hidden"), TEXT("")); }
	FScopedTestTypesVisible(const FScopedTestTypesVisible&) = delete;
	FScopedTestTypesVisible& operator=(const FScopedTestTypesVisible&) = delete;
};

const TCHAR* CreateBody = TEXT(R"json("Schema":{"Class":"/Script/GameplayStateTreeModule.StateTreeComponentSchema"},
"Evaluators":[{"Type":"/Script/AssetDocumentTests.AssetDocumentStateTreeTestEvaluator","Name":"Sensor"}],
"States":[{"Name":"Root","Children":[
  {"Name":"Patrol","Tasks":[{"Type":"/Script/StateTreeModule.StateTreeDelayTask","Name":"Wait"},
                            {"Type":"/Script/StateTreeModule.StateTreeDelayTask","Name":"Hold"}]},
  {"Name":"Phase"}]}])json");

TSharedPtr<FJsonObject> Child(const TSharedPtr<FJsonObject>& Document, int32 Index)
{
	return Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("States"))[0]->AsObject()
		->GetArrayField(TEXT("Children"))[Index]->AsObject();
}

void AppendChild(const TSharedPtr<FJsonObject>& Document, const TCHAR* Name)
{
	const TSharedPtr<FJsonObject> Root = Document->GetObjectField(TEXT("Body"))->GetArrayField(TEXT("States"))[0]->AsObject();
	TArray<TSharedPtr<FJsonValue>> Children = Root->GetArrayField(TEXT("Children"));
	const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
	State->SetStringField(TEXT("Name"), Name);
	Children.Add(MakeShared<FJsonValueObject>(State));
	Root->SetArrayField(TEXT("Children"), Children);
}

int32 CountFunctions(const UStateTreeEditorData& EditorData)
{
	int32 Count = 0;
	for (const FStateTreePropertyPathBinding& Binding : EditorData.EditorBindings.GetBindings())
	{
		Count += Binding.GetPropertyFunctionNode().IsValid() ? 1 : 0;
	}
	return Count;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantStateTreeTest, "UEAssetDoc.Tolerant.StateTree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantStateTreeTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantStateTreeTestsLocal;
	const FTarget Target(TEXT("ST_Tolerant"));
	const FScopedTestTypesVisible Visible;
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	const FString Created = FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/StateTreeModule.StateTree\","
		"\"Action\":\"Create\",\"Properties\":{},\"Body\":{%s}}"), *Target.PackageName, CreateBody);
	if (!TestTrue(TEXT("created"), ApplyText(*this, Target, Created, TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UStateTree* Tree = Target.Get<UStateTree>();
	UStateTreeEditorData* EditorData = Tree != nullptr ? Cast<UStateTreeEditorData>(Tree->EditorData) : nullptr;
	if (!TestNotNull(TEXT("editor data"), EditorData) || !TestEqual(TEXT("children"), EditorData->SubTrees[0]->Children.Num(), 2))
	{
		return false;
	}

	// What a designer makes in the editor: Duplicate on Patrol without renaming the copy, a transition from Patrol to
	// the copy, a rename to "Stage 1: Approach/Hold", the second task renamed like the first, Wait's Duration bound to
	// Add(Float) whose Left is bound to Multiply(Float), and the evaluator's Config set to a new settings object.
	EditorData->Modify();
	UStateTreeState* Root = EditorData->SubTrees[0];
	UStateTreeState* Patrol = Root->Children[0];
	UStateTreeState* Phase = Root->Children[1];
	Root->Modify();
	Patrol->Modify();
	Phase->Modify();
	UStateTreeState& Copy = Root->AddChildState(Patrol->Name);
	Patrol->AddTransition(EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::GotoState, &Copy);
	Phase->Name = TEXT("Stage 1: Approach/Hold");
	Patrol->Tasks[1].Node.GetMutable<FStateTreeNodeBase>().Name = TEXT("Wait");
	const UScriptStruct* AddFloat = FindObject<UScriptStruct>(nullptr, TEXT("/Script/StateTreeModule.StateTreeAddFloatPropertyFunction"));
	const UScriptStruct* MultiplyFloat = FindObject<UScriptStruct>(nullptr, TEXT("/Script/StateTreeModule.StateTreeMultiplyFloatPropertyFunction"));
	if (!TestNotNull(TEXT("Add(Float)"), AddFloat) || !TestNotNull(TEXT("Multiply(Float)"), MultiplyFloat))
	{
		return false;
	}
	const FPropertyBindingPath Duration(Patrol->Tasks[0].ID, TEXT("Duration"));
	const FPropertyBindingPathSegment Result(TEXT("Result"));
	EditorData->EditorBindings.AddFunctionBinding(AddFloat, {Result}, Duration);
	FGuid AddId;
	for (const FStateTreePropertyPathBinding& Binding : EditorData->EditorBindings.GetBindings())
	{
		if (Binding.GetTargetPath() == Duration && Binding.GetPropertyFunctionNode().IsValid())
		{
			AddId = Binding.GetPropertyFunctionNode().GetPtr<const FStateTreeEditorNode>()->ID;
		}
	}
	if (!TestTrue(TEXT("Add(Float) bound"), AddId.IsValid()))
	{
		return false;
	}
	EditorData->EditorBindings.AddFunctionBinding(MultiplyFloat, {Result}, FPropertyBindingPath(AddId, TEXT("Left")));
	FAssetDocumentStateTreeTestEvaluatorInstanceData& Sensor = EditorData->Evaluators[0].Instance.GetMutable<FAssetDocumentStateTreeTestEvaluatorInstanceData>();
	Sensor.Config = NewObject<UAssetDocumentStateTreeTestConfig>(EditorData, NAME_None, RF_Transactional);
	Sensor.Config->Range = 7.f;
	Sensor.Threshold = std::numeric_limits<float>::quiet_NaN();
	Tree->MarkPackageDirty();
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const FString Config = TEXT("/Body/Evaluators/0/Instance/Config");
	const FString Threshold = TEXT("/Body/Evaluators/0/Instance/Threshold");
	FExpected Expected;
	Expected.Warnings = {
		{TEXT("StateTree.DuplicateStateName"), TEXT("/Body/States/0/Children/2/Name")},
		{TEXT("StateTree.DuplicateNodeName"), TEXT("/Body/States/0/Children/0/Tasks/1/Name")},
		{TEXT("StateTree.Unrepresented"), Config},
		{TEXT("StateTree.Unrepresented"), Threshold}};
	Expected.Markers = {Config, Threshold};
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, Expected, TEXT("StateTree"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	FString To;
	const TSharedPtr<FJsonValue> Transition = At(Extracted, TEXT("/Body/States/0/Children/0/Transitions/0/To"));
	TestTrue(TEXT("the transition names the second Patrol"), Transition.IsValid() && Transition->TryGetString(To) && To == TEXT("Root/Patrol#2"));
	const TSharedPtr<FJsonValue> Nested = At(Extracted, TEXT("/Body/States/0/Children/0/Tasks/0/Bindings/Duration/Bindings/Left/Function"));
	TestTrue(TEXT("the nested property function is read"), Nested.IsValid() && Nested->AsString().EndsWith(TEXT("StateTreeMultiplyFloatPropertyFunction")));

	if (TestTrue(TEXT("another state is described"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		Child(Document, 1)->SetStringField(TEXT("Description"), TEXT("Close in"));
	}, TEXT("Edit")).IsSuccess()))
	{
		EditorData = Cast<UStateTreeEditorData>(Target.Get<UStateTree>()->EditorData);
		Root = EditorData->SubTrees[0];
		TestEqual(TEXT("description"), Root->Children[1]->Description, FString(TEXT("Close in")));
		TestTrue(TEXT("both Patrol states kept"), Root->Children.Num() == 3 && Root->Children[0]->Name == Root->Children[2]->Name);
		TestEqual(TEXT("both property functions kept"), CountFunctions(*EditorData), 2);
		const FAssetDocumentStateTreeTestEvaluatorInstanceData& KeptSensor =
			EditorData->Evaluators[0].Instance.Get<FAssetDocumentStateTreeTestEvaluatorInstanceData>();
		TestTrue(TEXT("the evaluator's Config kept"), KeptSensor.Config != nullptr && KeptSensor.Config->Range == 7.f);
		TestTrue(TEXT("the evaluator's NaN Threshold kept"), FMath::IsNaN(KeptSensor.Threshold));
		TestStillShows(*this, Target, TEXT("StateTree.DuplicateStateName"), TEXT("/Body/States/0/Children/2/Name"), TEXT("After edit"));
	}

	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		AppendChild(Document, TEXT("Stage 1: Approach/Hold"));
	}, TEXT("New duplicate")), TEXT("StateTree.DuplicateStateName"), TEXT("/Body/States/0/Children/3/Name"), TEXT("New duplicate"));
	TestRefused(*this, ApplyEdited(*this, Target, [&Config](const TSharedPtr<FJsonObject>& Document)
	{
		At(Document, Config)->AsObject()->SetStringField(TEXT("Unsupported"), TEXT("edited"));
	}, TEXT("Edited marker")), TEXT("StateTree.UnrepresentedValue"), Config, TEXT("Edited marker"));
	TestRefused(*this, ApplyEdited(*this, Target, [&Threshold](const TSharedPtr<FJsonObject>& Document)
	{
		At(Document, Threshold)->AsObject()->SetStringField(TEXT("Text"), TEXT("0.5"));
	}, TEXT("Edited NaN marker")), TEXT("StateTree.UnrepresentedValue"), Threshold, TEXT("Edited NaN marker"));
	return !HasAnyErrors();
}

#endif
