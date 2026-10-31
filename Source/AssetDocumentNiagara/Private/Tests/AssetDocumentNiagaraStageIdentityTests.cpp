// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// A Niagara system (a Content Browser duplicate of the DirectionalBurst template) whose emitter, set to GPU simulation
// as the stack's fix offers, got two Generic simulation stages from the stack's "+ Stage" menu, which names neither
// (both are None), and a Color module in each. Extract lists each stage with its own module, the extract re-applies
// as a no-op, and a rename (RenamedFrom) and a disable of the second stage reach that stage only and keep both modules
// in place. Replacing the second stage with a new entry of the same class deletes it and adds an empty, enabled stage.

#include "AssetDocumentTolerantTestSupport.h"

#include "AssetDocumentNiagaraPrimitives.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "IAssetTools.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraEmitterViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/INiagaraStackItemGroupAddUtilities.h"
#include "ViewModels/Stack/NiagaraStackEmitterPropertiesGroup.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
// The emitter stack's "+ Stage" menu entry for a Generic simulation stage, run as the menu runs it.
bool StageIdentityAddGenericStage(FNiagaraEmitterHandleViewModel& Handle)
{
	UNiagaraStackViewModel* Stack = Handle.GetEmitterStackViewModel();
	UNiagaraStackEntry* Root = Stack != nullptr ? Stack->GetRootEntry() : nullptr;
	if (Root == nullptr)
	{
		return false;
	}
	Root->RefreshChildren();
	TArray<UNiagaraStackEntry*> Children;
	Root->GetUnfilteredChildren(Children);
	for (UNiagaraStackEntry* Child : Children)
	{
		UNiagaraStackEmitterPropertiesGroup* Group = Cast<UNiagaraStackEmitterPropertiesGroup>(Child);
		INiagaraStackItemGroupAddUtilities* AddUtilities = Group != nullptr ? Group->GetAddUtilities() : nullptr;
		if (AddUtilities == nullptr)
		{
			continue;
		}
		TArray<TSharedRef<INiagaraStackItemGroupAddAction>> Actions;
		AddUtilities->GenerateAddActions(Actions, FNiagaraStackItemGroupAddOptions());
		for (const TSharedRef<INiagaraStackItemGroupAddAction>& Action : Actions)
		{
			if (Action->GetCategories().Contains(TEXT("Simulation Stages")) && Action->GetKeywords().ToString() == TEXT("NiagaraSimulationStageGeneric"))
			{
				AddUtilities->ExecuteAddAction(Action, INDEX_NONE);
				return true;
			}
		}
	}
	return false;
}

TArray<const UNiagaraSimulationStageBase*> StageIdentityStages(const UNiagaraSystem* System)
{
	TArray<const UNiagaraSimulationStageBase*> Stages;
	const FVersionedNiagaraEmitterData* Data = System != nullptr && System->GetEmitterHandles().Num() > 0
		? System->GetEmitterHandles()[0].GetEmitterData() : nullptr;
	if (Data != nullptr)
	{
		for (const UNiagaraSimulationStageBase* Stage : Data->GetSimulationStages())
		{
			if (Stage != nullptr)
			{
				Stages.Add(Stage);
			}
		}
	}
	return Stages;
}

// The emitter entry of the document's Body named Name.
TSharedPtr<FJsonObject> StageIdentityEmitter(const TSharedPtr<FJsonObject>& Document, const FString& Name)
{
	const TSharedPtr<FJsonObject>* Body = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Emitters = nullptr;
	if (!Document.IsValid() || !Document->TryGetObjectField(TEXT("Body"), Body) || !(*Body)->TryGetArrayField(TEXT("Emitters"), Emitters))
	{
		return nullptr;
	}
	for (const TSharedPtr<FJsonValue>& Item : *Emitters)
	{
		FString ItemName;
		if (Item.IsValid() && Item->Type == EJson::Object && Item->AsObject()->TryGetStringField(TEXT("Name"), ItemName) && ItemName == Name)
		{
			return Item->AsObject();
		}
	}
	return nullptr;
}

TArray<TSharedPtr<FJsonValue>> StageIdentityList(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	return Object.IsValid() && Object->TryGetArrayField(Field, Items) ? *Items : TArray<TSharedPtr<FJsonValue>>();
}

// The Name of each module the stage entry lists.
TArray<FString> StageIdentityModuleNames(const TSharedPtr<FJsonValue>& Stage)
{
	TArray<FString> Names;
	if (Stage.IsValid() && Stage->Type == EJson::Object)
	{
		for (const TSharedPtr<FJsonValue>& Module : StageIdentityList(Stage->AsObject(), TEXT("Modules")))
		{
			FString Name;
			if (Module.IsValid() && Module->Type == EJson::Object && Module->AsObject()->TryGetStringField(TEXT("Name"), Name))
			{
				Names.Add(Name);
			}
		}
	}
	return Names;
}

FString StageIdentityName(const TSharedPtr<FJsonValue>& Stage)
{
	FString Name;
	if (Stage.IsValid() && Stage->Type == EJson::Object)
	{
		Stage->AsObject()->TryGetStringField(TEXT("Name"), Name);
	}
	return Name;
}

bool StageIdentityEnabled(const TSharedPtr<FJsonValue>& Stage)
{
	bool bEnabled = true;
	if (Stage.IsValid() && Stage->Type == EJson::Object)
	{
		Stage->AsObject()->TryGetBoolField(TEXT("Enabled"), bEnabled);
	}
	return bEnabled;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraSimulationStageIdentityTest, "UEAssetDoc.Codec.Niagara.SimulationStageIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraSimulationStageIdentityTest::RunTest(const FString& Parameters)
{
	namespace Tolerant = AssetDocumentTolerantTestSupport;
	const Tolerant::FTarget Target(TEXT("NS_StageIdentity"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
	};
	UObject* Source = LoadObject<UObject>(nullptr, TEXT("/Niagara/DefaultAssets/Templates/Systems/DirectionalBurst.DirectionalBurst"));
	UNiagaraScript* ColorModule = LoadObject<UNiagaraScript>(nullptr, TEXT("/Niagara/Modules/Update/Color/Color.Color"));
	if (!TestNotNull(TEXT("DirectionalBurst loads"), Source) || !TestNotNull(TEXT("the Color module loads"), ColorModule))
	{
		return false;
	}
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UNiagaraSystem* System = Cast<UNiagaraSystem>(AssetTools.DuplicateAsset(Target.AssetName, FPackageName::GetLongPackagePath(Target.PackageName), Source));
	if (!TestNotNull(TEXT("duplicate"), System) || !TestTrue(TEXT("the system has an emitter"), System->GetEmitterHandles().Num() > 0))
	{
		return false;
	}
	System->WaitForCompilationComplete(false, false);
	const int32 Before = StageIdentityStages(System).Num();
	FString EmitterName;
	{
		UE::AssetDocument::Niagara::FFullStackView FullStack;
		FullStack.Cover(*System);
		FNiagaraSystemViewModelOptions Options;
		Options.bCanModifyEmittersFromTimeline = false;
		Options.bCanSimulate = false;
		Options.bCanAutoCompile = false;
		Options.bIsForDataProcessingOnly = true;
		Options.EditMode = ENiagaraSystemViewModelEditMode::SystemAsset;
		Options.MessageLogGuid = System->GetAssetGuid();
		const TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeShared<FNiagaraSystemViewModel>();
		ViewModel->Initialize(*System, Options);
		const TArray<TSharedRef<FNiagaraEmitterHandleViewModel>>& Handles = ViewModel->GetEmitterHandleViewModels();
		if (!TestTrue(TEXT("the view model lists the emitter"), Handles.Num() > 0))
		{
			return false;
		}
		const TSharedRef<FNiagaraEmitterHandleViewModel> Handle = Handles[0];
		EmitterName = Handle->GetName().ToString();

		// The stack's fix for a stage on a CPU emitter: "Set GPU simulation".
		const FVersionedNiagaraEmitter Versioned = Handle->GetEmitterViewModel()->GetEmitter();
		FVersionedNiagaraEmitterData* Data = Versioned.GetEmitterData();
		if (!TestNotNull(TEXT("emitter data"), Data))
		{
			return false;
		}
		Versioned.Emitter->Modify();
		Data->SimTarget = ENiagaraSimTarget::GPUComputeSim;
		FPropertyChangedEvent SimTargetChanged(FindFProperty<FProperty>(FVersionedNiagaraEmitterData::StaticStruct(),
			GET_MEMBER_NAME_CHECKED(FVersionedNiagaraEmitterData, SimTarget)));
		Versioned.Emitter->PostEditChangeVersionedProperty(SimTargetChanged, Versioned.Version);

		// "+ Stage" > Generic, twice: two stages named None.
		if (!TestTrue(TEXT("the first stage is added"), StageIdentityAddGenericStage(*Handle))
			|| !TestTrue(TEXT("the second stage is added"), StageIdentityAddGenericStage(*Handle)))
		{
			return false;
		}
		// A Color module in each stage, as the stage group's "+" adds it.
		const FVersionedNiagaraEmitterData* Added = Handle->GetEmitterViewModel()->GetEmitter().GetEmitterData();
		const UNiagaraScriptSource* GraphSource = Added != nullptr ? Cast<UNiagaraScriptSource>(Added->GraphSource) : nullptr;
		UNiagaraGraph* Graph = GraphSource != nullptr ? GraphSource->NodeGraph : nullptr;
		const TArray<const UNiagaraSimulationStageBase*> Stages = StageIdentityStages(System);
		if (!TestNotNull(TEXT("emitter graph"), Graph) || !TestEqual(TEXT("two stages were added"), Stages.Num(), Before + 2))
		{
			return false;
		}
		for (int32 Index = Before; Index < Stages.Num(); ++Index)
		{
			TestTrue(TEXT("the stack's + names no stage"), Stages[Index]->SimulationStageName.IsNone());
			UNiagaraNodeOutput* Output = Stages[Index]->Script != nullptr
				? Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleSimulationStageScript, Stages[Index]->Script->GetUsageId()) : nullptr;
			if (!TestNotNull(TEXT("the stage's output node"), Output)
				|| !TestNotNull(TEXT("the module is added"), FNiagaraStackGraphUtilities::AddScriptModuleToStack(ColorModule, *Output)))
			{
				return false;
			}
		}
	}
	System->MarkPackageDirty();
	System->RequestCompile(false);
	System->WaitForCompilationComplete(false, false);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = Tolerant::TestTolerantRoundTrip(*this, Target, Tolerant::FExpected(), TEXT("Two unnamed stages"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>> Stages = StageIdentityList(StageIdentityEmitter(Extracted, EmitterName), TEXT("SimulationStages"));
	if (!TestEqual(TEXT("the extract lists every stage"), Stages.Num(), Before + 2))
	{
		return false;
	}
	const TArray<FString> FirstModules = StageIdentityModuleNames(Stages[Before]);
	const TArray<FString> SecondModules = StageIdentityModuleNames(Stages[Before + 1]);
	TestEqual(TEXT("the first stage reads as None"), StageIdentityName(Stages[Before]), FString(TEXT("None")));
	TestEqual(TEXT("the second stage reads as None"), StageIdentityName(Stages[Before + 1]), FString(TEXT("None")));
	if (!TestEqual(TEXT("the first stage lists its own module only"), FirstModules.Num(), 1)
		|| !TestEqual(TEXT("the second stage lists its own module only"), SecondModules.Num(), 1)
		|| !TestTrue(TEXT("the two stages list different modules"), FirstModules[0] != SecondModules[0]))
	{
		return false;
	}

	// The second stage is renamed and disabled, as its Details and checkbox do.
	const int32 Second = Before + 1;
	const FAssetDocumentResult Edited = Tolerant::ApplyEdited(*this, Target, [&EmitterName, Second](const TSharedPtr<FJsonObject>& Document)
	{
		const TArray<TSharedPtr<FJsonValue>> Listed = StageIdentityList(StageIdentityEmitter(Document, EmitterName), TEXT("SimulationStages"));
		if (Listed.IsValidIndex(Second) && Listed[Second]->Type == EJson::Object)
		{
			Listed[Second]->AsObject()->SetStringField(TEXT("Name"), TEXT("Solve"));
			Listed[Second]->AsObject()->SetStringField(TEXT("RenamedFrom"), TEXT("None"));
			Listed[Second]->AsObject()->SetBoolField(TEXT("Enabled"), false);
		}
	}, TEXT("Rename and disable the second stage"));
	if (!TestTrue(TEXT("the edit applies"), Edited.IsSuccess()))
	{
		return false;
	}
	const TArray<const UNiagaraSimulationStageBase*> Live = StageIdentityStages(Target.Get<UNiagaraSystem>());
	if (TestEqual(TEXT("the asset keeps both stages"), Live.Num(), Before + 2))
	{
		TestTrue(TEXT("the first stage keeps its name"), Live[Before]->SimulationStageName.IsNone());
		TestTrue(TEXT("the first stage stays enabled"), Live[Before]->bEnabled != 0);
		TestTrue(TEXT("the second stage is renamed"), Live[Second]->SimulationStageName == FName(TEXT("Solve")));
		TestTrue(TEXT("the second stage is disabled"), Live[Second]->bEnabled == 0);
	}
	const FAssetDocumentResult After = Tolerant::ExtractResult(Target);
	const TArray<TSharedPtr<FJsonValue>> AfterStages = After.Payload.IsValid()
		? StageIdentityList(StageIdentityEmitter(After.Payload, EmitterName), TEXT("SimulationStages")) : TArray<TSharedPtr<FJsonValue>>();
	if (TestEqual(TEXT("the extract after the edit lists every stage"), AfterStages.Num(), Before + 2))
	{
		TestEqual(TEXT("the first stage reads as None"), StageIdentityName(AfterStages[Before]), FString(TEXT("None")));
		TestTrue(TEXT("the first stage reads as enabled"), StageIdentityEnabled(AfterStages[Before]));
		TestTrue(TEXT("the first stage keeps its module"), StageIdentityModuleNames(AfterStages[Before]) == FirstModules);
		TestEqual(TEXT("the second stage reads as Solve"), StageIdentityName(AfterStages[Second]), FString(TEXT("Solve")));
		TestFalse(TEXT("the second stage reads as disabled"), StageIdentityEnabled(AfterStages[Second]));
		TestTrue(TEXT("the second stage keeps its module"), StageIdentityModuleNames(AfterStages[Second]) == SecondModules);
	}

	// The second stage is removed and a new stage of the same class takes its place: the new stage starts empty.
	const FAssetDocumentResult Replaced = Tolerant::ApplyEdited(*this, Target, [&EmitterName, Second](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> StageOwner = StageIdentityEmitter(Document, EmitterName);
		TArray<TSharedPtr<FJsonValue>> Listed = StageIdentityList(StageOwner, TEXT("SimulationStages"));
		if (StageOwner.IsValid() && Listed.IsValidIndex(Second) && Listed[Second]->Type == EJson::Object)
		{
			FString StageClass;
			Listed[Second]->AsObject()->TryGetStringField(TEXT("Class"), StageClass);
			const TSharedPtr<FJsonObject> Fresh = MakeShared<FJsonObject>();
			Fresh->SetStringField(TEXT("Name"), TEXT("Fresh"));
			Fresh->SetStringField(TEXT("Class"), StageClass);
			Listed[Second] = MakeShared<FJsonValueObject>(Fresh);
			StageOwner->SetArrayField(TEXT("SimulationStages"), Listed);
		}
	}, TEXT("Replace the second stage with a new one"));
	if (!TestTrue(TEXT("the replacement applies"), Replaced.IsSuccess()))
	{
		return false;
	}
	const TArray<const UNiagaraSimulationStageBase*> ReplacedLive = StageIdentityStages(Target.Get<UNiagaraSystem>());
	if (TestEqual(TEXT("the asset has as many stages after the replacement"), ReplacedLive.Num(), Before + 2))
	{
		TestTrue(TEXT("the new stage is named Fresh"), ReplacedLive[Second]->SimulationStageName == FName(TEXT("Fresh")));
		TestTrue(TEXT("the new stage is enabled (the class default)"), ReplacedLive[Second]->bEnabled != 0);
	}
	const FAssetDocumentResult ReplacedRead = Tolerant::ExtractResult(Target);
	const TArray<TSharedPtr<FJsonValue>> ReplacedStages = ReplacedRead.Payload.IsValid()
		? StageIdentityList(StageIdentityEmitter(ReplacedRead.Payload, EmitterName), TEXT("SimulationStages")) : TArray<TSharedPtr<FJsonValue>>();
	if (TestEqual(TEXT("the extract after the replacement lists every stage"), ReplacedStages.Num(), Before + 2))
	{
		TestTrue(TEXT("the first stage keeps its module"), StageIdentityModuleNames(ReplacedStages[Before]) == FirstModules);
		TestEqual(TEXT("the new stage reads as Fresh"), StageIdentityName(ReplacedStages[Second]), FString(TEXT("Fresh")));
		TestTrue(TEXT("the new stage reads as enabled"), StageIdentityEnabled(ReplacedStages[Second]));
		TestEqual(TEXT("the new stage has no modules"), StageIdentityModuleNames(ReplacedStages[Second]).Num(), 0);
	}
	return !HasAnyErrors();
}

#endif
