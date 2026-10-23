// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// UEAssetDoc.Niagara.Profile.*: the Profiles' reads through the production Service (system Apply:
// AssetDocumentNiagaraApplyTests.cpp; emitter asset and collection Apply: AssetDocumentNiagaraEmitterAssetTests.cpp).
//   InspectMatches  Inspect lists, per stack group, the modules and every input the editor's stack view
//                   model shows (built independently in the test), with value modes; Inspect and Extract
//                   report the compile status.
//   EmitterAsset    an emitter asset reads through the emitter editor's placeholder system; the asset and
//                   its package stay untouched.

#include "AssetDocumentNiagaraTestSupport.h"

#include "AssetDocumentEditorNonInteractiveScope.h"

#include "Dom/JsonValue.h"
#include "NiagaraEmitter.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraSystem.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackFunctionInput.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentNiagaraTests
{
namespace
{
const TCHAR* const DirectionalBurst = TEXT("/Niagara/DefaultAssets/Templates/Systems/DirectionalBurst.DirectionalBurst");
const TCHAR* const RibbonEmitter = TEXT("/Niagara/DefaultAssets/Templates/BehaviorExamples/RibbonID.RibbonID");

// "<emitter>|<module>" -> input names, in stack order, from a headless view model built here.
TArray<FString> StackModules(UNiagaraSystem& System)
{
	TArray<FString> Lines;
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	FNiagaraSystemViewModelOptions Options;
	Options.bCanModifyEmittersFromTimeline = false;
	Options.bCanSimulate = false;
	Options.bCanAutoCompile = false;
	Options.bIsForDataProcessingOnly = true;
	Options.EditMode = ENiagaraSystemViewModelEditMode::SystemAsset;
	Options.MessageLogGuid = System.GetAssetGuid();
	const TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeShared<FNiagaraSystemViewModel>();
	ViewModel->Initialize(System, Options);
	auto Walk = [&Lines](UNiagaraStackViewModel* Stack, const FString& Emitter)
	{
		TArray<UNiagaraStackModuleItem*> Modules;
		if (Stack != nullptr && Stack->GetRootEntry() != nullptr)
		{
			Stack->GetRootEntry()->GetUnfilteredChildrenOfType(Modules, true);
		}
		for (UNiagaraStackModuleItem* Module : Modules)
		{
			const TArray<UNiagaraStackFunctionInput*> Inputs = UE::AssetDocument::Niagara::ModuleInputs(*Module);
			TArray<FString> Names;
			for (UNiagaraStackFunctionInput* Input : Inputs)
			{
				Names.Add(Input->GetInputParameterHandle().GetName().ToString());
			}
			Lines.Add(FString::Printf(TEXT("%s|%s: %s"), *Emitter, *Module->GetModuleNode().GetFunctionName(), *FString::Join(Names, TEXT(", "))));
		}
	};
	Walk(ViewModel->GetSystemStackViewModel(), FString());
	for (const TSharedRef<FNiagaraEmitterHandleViewModel>& Handle : ViewModel->GetEmitterHandleViewModels())
	{
		Walk(Handle->GetEmitterStackViewModel(), Handle->GetName().ToString());
	}
	return Lines;
}

TArray<FString> InspectedModules(const TSharedPtr<FJsonObject>& Details)
{
	TArray<FString> Lines;
	const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
	if (!Details.IsValid() || !Details->TryGetArrayField(TEXT("Modules"), Modules))
	{
		return Lines;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Modules)
	{
		const TSharedPtr<FJsonObject> Module = Value->AsObject();
		TArray<FString> Names;
		for (const TSharedPtr<FJsonValue>& Input : Module->GetArrayField(TEXT("Inputs")))
		{
			Names.Add(Input->AsObject()->GetStringField(TEXT("Name")));
		}
		Lines.Add(FString::Printf(TEXT("%s|%s: %s"), *Module->GetStringField(TEXT("Emitter")), *Module->GetStringField(TEXT("Module")),
			*FString::Join(Names, TEXT(", "))));
	}
	return Lines;
}
} // namespace
} // namespace AssetDocumentNiagaraTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraProfileInspectMatchesTest, "UEAssetDoc.Niagara.Profile.InspectMatches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraProfileInspectMatchesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	UObject* Source = LoadObject<UObject>(nullptr, DirectionalBurst);
	if (!TestNotNull(TEXT("template system loads"), Source))
	{
		return false;
	}
	const FTarget Copy(TEXT("NS_Inspect"));
	if (UNiagaraSystem* System = Cast<UNiagaraSystem>(DuplicateTo(*Source, Copy)))
	{
		System->WaitForCompilationComplete(false, false);
		const TSharedPtr<FJsonObject> Inspected = Inspect(*this, Copy.ObjectPath, TEXT("inspect"));
		const TSharedPtr<FJsonObject> Details = Inspected.IsValid() ? Inspected->GetObjectField(TEXT("NiagaraDetails")) : nullptr;
		const TArray<FString> Expected = StackModules(*System);
		const TArray<FString> Actual = InspectedModules(Details);
		TestTrue(TEXT("the stack has modules"), Expected.Num() > 10);
		TestTextEqual(*this, TEXT("Inspect lists the stack's modules and inputs"), FString::Join(Actual, TEXT("\n")),
			FString::Join(Expected, TEXT("\n")));
		const TSharedPtr<FJsonObject>* Compile = nullptr;
		TestTrue(TEXT("Inspect reports the compile status"), Details.IsValid() && Details->TryGetObjectField(TEXT("Compile"), Compile)
			&& (*Compile)->GetStringField(TEXT("Status")) == TEXT("UpToDate"));
		const TSharedPtr<FJsonObject>* Library = nullptr;
		TestTrue(TEXT("Inspect lists each group's module library"), Details.IsValid() && Details->TryGetObjectField(TEXT("Library"), Library)
			&& (*Library)->HasField(TEXT("ParticleSpawn")));
		AddInfo(FString::Printf(TEXT("NiagaraExample inspect %s"), *Serialize(Inspected).Left(6000)));

		const FAssetDocumentResult Extracted = ExtractResult(Copy.ObjectPath);
		TestTrue(TEXT("Extract reports the compile status"), Extracted.Compile.IsValid()
			&& Extracted.Compile->GetStringField(TEXT("Status")) == TEXT("UpToDate"));
		AddInfo(FString::Printf(TEXT("NiagaraExample extract %s"), *Serialize(Extracted.Payload)));
		TestFalse(TEXT("copy stays clean"), System->GetOutermost()->IsDirty());
	}
	Copy.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraProfileEmitterAssetTest, "UEAssetDoc.Niagara.Profile.EmitterAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraProfileEmitterAssetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace AssetDocumentNiagaraTests;
	UObject* Source = LoadObject<UObject>(nullptr, RibbonEmitter);
	if (!TestNotNull(TEXT("template emitter loads"), Source))
	{
		return false;
	}
	const FTarget Copy(TEXT("NE_Ribbon"));
	if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(DuplicateTo(*Source, Copy)))
	{
		const FGuid ChangeId = Emitter->GetChangeId();
		const TSharedPtr<FJsonObject> Document = Extract(*this, Copy, TEXT("emitter"));
		const TSharedPtr<FJsonObject> Body = Document.IsValid() ? Document->GetObjectField(TEXT("Body")) : nullptr;
		TestTrue(TEXT("emitter body has the particle stacks and renderers"), Body.IsValid()
			&& Body->HasTypedField<EJson::Array>(TEXT("ParticleUpdate")) && Body->GetArrayField(TEXT("Renderers")).Num() > 0);
		TestFalse(TEXT("emitter body has no handle fields"), Body.IsValid() && Body->HasField(TEXT("Name")));
		AddInfo(FString::Printf(TEXT("NiagaraExample emitter %s"), *Serialize(Document).Left(4000)));
		TestFalse(TEXT("emitter stays clean"), Emitter->GetOutermost()->IsDirty());
		TestEqual(TEXT("emitter change id unchanged"), Emitter->GetChangeId(), ChangeId);
	}
	Copy.Cleanup();
	return true;
}
#endif
