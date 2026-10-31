// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An editor-made StateTree with an evaluator output bound to a tree parameter and an OnDelegate transition listening
// to an evaluator's dispatcher: extract shows both read-only, the extract re-applies as a no-op, an edit elsewhere
// keeps them, and an edited output binding or listener is refused.

#include "AssetDocumentStateTreeTestTypes.h"
#include "AssetDocumentTolerantTestSupport.h"

#include "Misc/ScopeExit.h"
#include "PropertyBindingPath.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorPropertyBindings.h"
#include "StateTreeState.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentFidelityStateTreeTestsLocal
{
// The test evaluators are meta=(Hidden), as the node picker skips them; the test lifts that while it runs.
struct FFidelityStateTreeTypesVisible
{
	FFidelityStateTreeTypesVisible()
	{
		FAssetDocumentStateTreeTestEvaluator::StaticStruct()->RemoveMetaData(TEXT("Hidden"));
		FAssetDocumentStateTreeTestDispatcherEvaluator::StaticStruct()->RemoveMetaData(TEXT("Hidden"));
	}
	~FFidelityStateTreeTypesVisible()
	{
		FAssetDocumentStateTreeTestEvaluator::StaticStruct()->SetMetaData(TEXT("Hidden"), TEXT(""));
		FAssetDocumentStateTreeTestDispatcherEvaluator::StaticStruct()->SetMetaData(TEXT("Hidden"), TEXT(""));
	}
	FFidelityStateTreeTypesVisible(const FFidelityStateTreeTypesVisible&) = delete;
	FFidelityStateTreeTypesVisible& operator=(const FFidelityStateTreeTypesVisible&) = delete;
};

const TCHAR* FidelityStateTreeBody = TEXT(R"json("Schema":{"Class":"/Script/GameplayStateTreeModule.StateTreeComponentSchema"},
"Parameters":[{"Name":"Distance","Type":"float","Value":0.0}],
"Evaluators":[{"Type":"/Script/AssetDocumentTests.AssetDocumentStateTreeTestEvaluator","Name":"Sensor"},
              {"Type":"/Script/AssetDocumentTests.AssetDocumentStateTreeTestDispatcherEvaluator","Name":"Door"}],
"States":[{"Name":"Root","Children":[{"Name":"Patrol"},{"Name":"Phase"}]}])json");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentFidelityStateTreeBindingsTest, "UEAssetDoc.Fidelity.StateTree.OutputAndDelegateBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentFidelityStateTreeBindingsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentFidelityStateTreeTestsLocal;
	const FTarget Target(TEXT("ST_OutputAndDelegate"));
	const FFidelityStateTreeTypesVisible Visible;
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	const FString Created = FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/StateTreeModule.StateTree\","
		"\"Action\":\"Create\",\"Properties\":{},\"Body\":{%s}}"), *Target.PackageName, FidelityStateTreeBody);
	if (!TestTrue(TEXT("created"), ApplyText(*this, Target, Created, TEXT("Create")).IsSuccess()))
	{
		return false;
	}
	UStateTree* Tree = Target.Get<UStateTree>();
	UStateTreeEditorData* EditorData = Tree != nullptr ? Cast<UStateTreeEditorData>(Tree->EditorData) : nullptr;
	if (!TestNotNull(TEXT("editor data"), EditorData) || !TestEqual(TEXT("evaluators"), EditorData->Evaluators.Num(), 2))
	{
		return false;
	}

	// The designer binds Sensor's Distance output to the tree parameter (the output's bind menu) and adds an
	// OnDelegate transition from Patrol to Phase whose Delegate is Door's OnDone.
	EditorData->Modify();
	UStateTreeState* Patrol = EditorData->SubTrees[0]->Children[0];
	UStateTreeState* Phase = EditorData->SubTrees[0]->Children[1];
	Patrol->Modify();
	EditorData->EditorBindings.AddOutputBinding(FPropertyBindingPath(EditorData->Evaluators[0].ID, TEXT("Distance")),
		FPropertyBindingPath(EditorData->GetRootParametersGuid(), TEXT("Distance")));
	const FStateTreeTransition& Transition = Patrol->AddTransition(EStateTreeTransitionTrigger::OnDelegate, EStateTreeTransitionType::GotoState, Phase);
	EditorData->EditorBindings.AddBinding(FPropertyBindingPath(EditorData->Evaluators[1].ID, TEXT("OnDone")),
		FPropertyBindingPath(Transition.ID, TEXT("DelegateListener")));
	Tree->MarkPackageDirty();
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const FString Outputs = TEXT("/Body/Evaluators/0/OutputBindings");
	const FString Delegate = TEXT("/Body/States/0/Children/0/Transitions/0/Delegate");
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Output and delegate bindings"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonValue> Output = At(Extracted, Outputs + TEXT("/Distance"));
	TestTrue(TEXT("output binding read"), Output.IsValid() && Output->AsString() == TEXT("Parameters:Distance"));
	const TSharedPtr<FJsonValue> Source = At(Extracted, Delegate + TEXT("/Source"));
	TestTrue(TEXT("listener read"), Source.IsValid() && Source->AsString().EndsWith(TEXT(":OnDone")));

	if (TestTrue(TEXT("another edit applies"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		At(Document, TEXT("/Body/States/0/Children/1"))->AsObject()->SetStringField(TEXT("Description"), TEXT("Open"));
	}, TEXT("Edit")).IsSuccess()))
	{
		EditorData = Cast<UStateTreeEditorData>(Target.Get<UStateTree>()->EditorData);
		// The listener binding targets the DelegateListener property of Patrol's live transition. Paths compare by
		// segment names: ToString may print a property's display name ("Delegate") instead of its name.
		const UStateTreeState* KeptPatrol = EditorData->SubTrees[0]->Children[0];
		const FPropertyBindingPath ListenerPath(KeptPatrol->Transitions.Num() == 1 ? KeptPatrol->Transitions[0].ID : FGuid(),
			GET_MEMBER_NAME_CHECKED(FStateTreeTransition, DelegateListener));
		int32 OutputCount = 0;
		int32 ListenerCount = 0;
		for (const FStateTreePropertyPathBinding& Binding : EditorData->EditorBindings.GetBindings())
		{
			OutputCount += Binding.IsOutputBinding() ? 1 : 0;
			ListenerCount += Binding.GetTargetPath() == ListenerPath ? 1 : 0;
		}
		TestEqual(TEXT("output binding kept"), OutputCount, 1);
		TestEqual(TEXT("listener kept"), ListenerCount, 1);
	}
	TestRefused(*this, ApplyEdited(*this, Target, [&Outputs](const TSharedPtr<FJsonObject>& Document)
	{
		At(Document, Outputs)->AsObject()->SetStringField(TEXT("Count"), TEXT("Parameters:Distance"));
	}, TEXT("Edited output binding")), TEXT("StateTree.OutputBindingsReadOnly"), Outputs, TEXT("Edited output binding"));
	TestRefused(*this, ApplyEdited(*this, Target, [&Delegate](const TSharedPtr<FJsonObject>& Document)
	{
		At(Document, Delegate)->AsObject()->SetStringField(TEXT("Source"), TEXT("Evaluators/Sensor:Distance"));
	}, TEXT("Edited listener")), TEXT("StateTree.DelegateReadOnly"), Delegate, TEXT("Edited listener"));
	return !HasAnyErrors();
}

#endif
