// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// UEAssetDoc.Niagara.Spike.HeadlessViewModel: the editor's stack view models built
// without an editor (FNiagaraSystemViewModel with bIsForDataProcessingOnly, as the engine's merge and
// upgrade paths build them) on engine template systems. Every stack entry, module input (with its value
// mode), renderer and user parameter is enumerated; no window opens, no modal is attempted and the
// system's package stays clean.

#include "AssetDocumentEditorNonInteractiveScope.h"

#include "Framework/Application/SlateApplication.h"
#include "Misc/AutomationTest.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackFunctionInput.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackRendererItem.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentNiagaraSpikeTests
{
const TCHAR* const Systems[] = {
	TEXT("/Niagara/DefaultAssets/DefaultSystem.DefaultSystem"),
	TEXT("/Niagara/DefaultAssets/Templates/Systems/DirectionalBurst.DirectionalBurst"),
	TEXT("/Niagara/DefaultAssets/Templates/Systems/RadialBurst.RadialBurst"),
	TEXT("/Niagara/DefaultAssets/Templates/Systems/SimpleExplosion.SimpleExplosion"),
	TEXT("/Niagara/DefaultAssets/Templates/BehaviorExamples/RenderTargetTexturePainter.RenderTargetTexturePainter"),
	TEXT("/Niagara/DefaultAssets/Templates/Systems/MinimalLightweight.MinimalLightweight"),
};

const TCHAR* ModeName(UNiagaraStackFunctionInput::EValueMode Mode)
{
	switch (Mode)
	{
	case UNiagaraStackFunctionInput::EValueMode::Local: return TEXT("Local");
	case UNiagaraStackFunctionInput::EValueMode::Linked: return TEXT("Linked");
	case UNiagaraStackFunctionInput::EValueMode::Dynamic: return TEXT("Dynamic");
	case UNiagaraStackFunctionInput::EValueMode::Data: return TEXT("Data");
	case UNiagaraStackFunctionInput::EValueMode::ObjectAsset: return TEXT("ObjectAsset");
	case UNiagaraStackFunctionInput::EValueMode::Expression: return TEXT("Expression");
	case UNiagaraStackFunctionInput::EValueMode::DefaultFunction: return TEXT("DefaultFunction");
	case UNiagaraStackFunctionInput::EValueMode::InvalidOverride: return TEXT("InvalidOverride");
	case UNiagaraStackFunctionInput::EValueMode::UnsupportedDefault: return TEXT("UnsupportedDefault");
	default: return TEXT("None");
	}
}

struct FCounts
{
	int32 Entries = 0;
	int32 Groups = 0;
	int32 Modules = 0;
	int32 Inputs = 0;
	int32 Renderers = 0;
	TMap<FString, int32> Modes;
};

void DescribeInput(UNiagaraStackFunctionInput& Input, int32 Depth, FCounts& Counts, TArray<FString>& Lines)
{
	++Counts.Inputs;
	const UNiagaraStackFunctionInput::EValueMode Mode = Input.GetValueMode();
	Counts.Modes.FindOrAdd(ModeName(Mode))++;
	FString Value;
	switch (Mode)
	{
	case UNiagaraStackFunctionInput::EValueMode::Local:
	{
		const TSharedPtr<const FStructOnScope> Local = Input.GetLocalValueStruct();
		Value = Local.IsValid() && Local->GetStruct() != nullptr
			? FString::Printf(TEXT("%s[%d bytes]"), *Local->GetStruct()->GetName(), Local->GetStruct()->GetStructureSize()) : TEXT("?");
		break;
	}
	case UNiagaraStackFunctionInput::EValueMode::Linked:
		Value = Input.GetLinkedParameterValue().GetName().ToString();
		break;
	case UNiagaraStackFunctionInput::EValueMode::Dynamic:
	{
		const UNiagaraNodeFunctionCall* Node = Input.GetDynamicInputNode();
		Value = Node != nullptr ? FString::Printf(TEXT("%s (%s)"), *Node->GetFunctionName(), *GetPathNameSafe(Node->FunctionScript)) : TEXT("?");
		break;
	}
	case UNiagaraStackFunctionInput::EValueMode::Expression:
		Value = Input.GetCustomExpressionText().ToString().Left(60);
		break;
	case UNiagaraStackFunctionInput::EValueMode::Data:
		Value = Input.GetDataValueObject() != nullptr ? Input.GetDataValueObject()->GetClass()->GetName() : TEXT("?");
		break;
	case UNiagaraStackFunctionInput::EValueMode::ObjectAsset:
		Value = GetPathNameSafe(Input.GetObjectAssetValue());
		break;
	default:
		break;
	}
	Lines.Add(FString::Printf(TEXT("%*s- %s : %s = %s %s%s"), Depth * 2, TEXT(""),
		*Input.GetInputParameterHandle().GetName().ToString(), *Input.GetInputType().GetName(), ModeName(Mode), *Value,
		Input.CanReset() ? TEXT(" (overridden)") : TEXT("")));
	if (Mode == UNiagaraStackFunctionInput::EValueMode::Dynamic)
	{
		for (UNiagaraStackFunctionInput* Child : Input.GetChildInputs())
		{
			if (Child != nullptr)
			{
				DescribeInput(*Child, Depth + 1, Counts, Lines);
			}
		}
	}
}

void Walk(UNiagaraStackEntry& Entry, int32 Depth, FCounts& Counts, TArray<FString>& Lines)
{
	++Counts.Entries;
	if (UNiagaraStackModuleItem* Module = Cast<UNiagaraStackModuleItem>(&Entry))
	{
		++Counts.Modules;
		const UNiagaraNodeFunctionCall& Node = Module->GetModuleNode();
		Lines.Add(FString::Printf(TEXT("%*sModule %s script=%s version=%s enabled=%d inherited=%d display='%s' [%s/%s]"), Depth * 2, TEXT(""),
			*Node.GetFunctionName(), *GetPathNameSafe(Node.FunctionScript), *Node.SelectedScriptVersion.ToString(),
			Module->GetIsEnabled() ? 1 : 0, Module->GetIsInherited() ? 1 : 0, *Module->GetDisplayName().ToString(),
			*Module->GetExecutionCategoryName().ToString(), *Module->GetExecutionSubcategoryName().ToString()));
		TArray<UNiagaraStackFunctionInput*> Inputs;
		Module->GetParameterInputs(Inputs);
		for (UNiagaraStackFunctionInput* Input : Inputs)
		{
			if (Input != nullptr)
			{
				DescribeInput(*Input, Depth + 1, Counts, Lines);
			}
		}
		return;
	}
	if (UNiagaraStackRendererItem* Renderer = Cast<UNiagaraStackRendererItem>(&Entry))
	{
		++Counts.Renderers;
		Lines.Add(FString::Printf(TEXT("%*sRenderer %s (%s) enabled=%d"), Depth * 2, TEXT(""),
			*GetNameSafe(Renderer->GetRendererProperties()), *GetNameSafe(Renderer->GetRendererProperties() != nullptr
				? Renderer->GetRendererProperties()->GetClass() : nullptr), Renderer->GetIsEnabled() ? 1 : 0));
		return;
	}
	if (Entry.GetStackRowStyle() == UNiagaraStackEntry::EStackRowStyle::GroupHeader
		|| Entry.GetStackRowStyle() == UNiagaraStackEntry::EStackRowStyle::ItemCategory || Depth <= 1)
	{
		++Counts.Groups;
		Lines.Add(FString::Printf(TEXT("%*s[%s] %s (%s/%s)"), Depth * 2, TEXT(""), *Entry.GetClass()->GetName(),
			*Entry.GetDisplayName().ToString(), *Entry.GetExecutionCategoryName().ToString(), *Entry.GetExecutionSubcategoryName().ToString()));
	}
	TArray<UNiagaraStackEntry*> Children;
	Entry.GetUnfilteredChildren(Children);
	for (UNiagaraStackEntry* Child : Children)
	{
		if (Child != nullptr)
		{
			Walk(*Child, Depth + 1, Counts, Lines);
		}
	}
}

int32 CountWindows()
{
	return FSlateApplication::IsInitialized() ? FSlateApplication::Get().GetTopLevelWindows().Num() : 0;
}
} // namespace AssetDocumentNiagaraSpikeTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentNiagaraHeadlessViewModelSpikeTest, "UEAssetDoc.Niagara.Spike.HeadlessViewModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentNiagaraHeadlessViewModelSpikeTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentNiagaraSpikeTests;
	int32 Tried = 0;
	for (const TCHAR* Path : Systems)
	{
		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (System == nullptr)
		{
			AddInfo(FString::Printf(TEXT("SKIP %s: not in this engine's content"), Path));
			continue;
		}
		++Tried;
		// Settle the load-time compile request first, so that only the view model's own effects are measured.
		System->WaitForCompilationComplete(false, false);
		UPackage* Package = System->GetOutermost();
		const bool bDirtyBefore = Package->IsDirty();
		const int32 WindowsBefore = CountWindows();
		const double Start = FPlatformTime::Seconds();
		FCounts Counts;
		TArray<FString> Lines;
		bool bRefused = false;
		{
			FAssetDocumentEditorNonInteractiveScope NonInteractive;
			TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeShared<FNiagaraSystemViewModel>();
			FNiagaraSystemViewModelOptions Options;
			Options.bCanModifyEmittersFromTimeline = false;
			Options.bCanSimulate = false;
			Options.bCanAutoCompile = false;
			Options.bIsForDataProcessingOnly = true;
			Options.EditMode = ENiagaraSystemViewModelEditMode::SystemAsset;
			Options.MessageLogGuid = System->GetAssetGuid();
			ViewModel->Initialize(*System, Options);
			const double Initialized = FPlatformTime::Seconds();
			Lines.Add(FString::Printf(TEXT("init %.3f s"), Initialized - Start));
			if (UNiagaraStackViewModel* SystemStack = ViewModel->GetSystemStackViewModel())
			{
				Lines.Add(TEXT("SYSTEM STACK"));
				if (UNiagaraStackEntry* Root = SystemStack->GetRootEntry())
				{
					Walk(*Root, 0, Counts, Lines);
				}
			}
			for (const TSharedRef<FNiagaraEmitterHandleViewModel>& Handle : ViewModel->GetEmitterHandleViewModels())
			{
				const FNiagaraEmitterHandle* EmitterHandle = Handle->GetEmitterHandle();
				Lines.Add(FString::Printf(TEXT("EMITTER %s enabled=%d mode=%d"), *Handle->GetName().ToString(), Handle->GetIsEnabled() ? 1 : 0,
					EmitterHandle != nullptr ? static_cast<int32>(EmitterHandle->GetEmitterMode()) : -1));
				UNiagaraStackViewModel* Stack = Handle->GetEmitterStackViewModel();
				if (Stack != nullptr && Stack->GetRootEntry() != nullptr)
				{
					Walk(*Stack->GetRootEntry(), 0, Counts, Lines);
				}
			}
			TArray<FNiagaraVariable> UserParameters;
			System->GetExposedParameters().GetUserParameters(UserParameters);
			for (const FNiagaraVariable& Parameter : UserParameters)
			{
				Lines.Add(FString::Printf(TEXT("USER %s : %s"), *Parameter.GetName().ToString(), *Parameter.GetType().GetName()));
			}
			bRefused = NonInteractive.WasInteractionRefused();
		}
		const double Seconds = FPlatformTime::Seconds() - Start;
		const FString Name = FPaths::GetBaseFilename(Path);
		FString ModeSummary;
		for (const TPair<FString, int32>& Pair : Counts.Modes)
		{
			ModeSummary += FString::Printf(TEXT("%s=%d "), *Pair.Key, Pair.Value);
		}
		AddInfo(FString::Printf(TEXT("%s: %.3f s, %d entries, %d groups, %d modules, %d inputs (%s), %d renderers"), *Name, Seconds,
			Counts.Entries, Counts.Groups, Counts.Modules, Counts.Inputs, *ModeSummary, Counts.Renderers));
		for (const FString& Line : Lines)
		{
			AddInfo(FString::Printf(TEXT("%s | %s"), *Name, *Line));
		}
		TestFalse(*FString::Printf(TEXT("%s: no modal or dialog was attempted"), *Name), bRefused);
		TestEqual(*FString::Printf(TEXT("%s: no window opened"), *Name), CountWindows(), WindowsBefore);
		TestEqual(*FString::Printf(TEXT("%s: the package's dirty state is unchanged"), *Name), Package->IsDirty(), bDirtyBefore);
		TestTrue(*FString::Printf(TEXT("%s: the stack lists modules"), *Name), Counts.Modules > 0 || Name.Contains(TEXT("Lightweight")));
	}
	TestTrue(TEXT("at least 4 template systems exist"), Tried >= 4);
	return !HasAnyErrors();
}
#endif
