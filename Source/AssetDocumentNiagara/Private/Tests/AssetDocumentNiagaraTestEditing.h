// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Document and editor-API helpers shared by the Niagara Apply / Stack / Corpus tests.

#include "AssetDocumentNiagaraTestSupport.h"

#include "Misc/ScopeExit.h"
#include "Policies/CondensedJsonPrintPolicy.h"

#include "AssetDocumentEditorNonInteractiveScope.h"
#include "Runtime/AssetDocumentUndoHistory.h"

#include "DataHierarchyViewModelBase.h"
#include "Dom/JsonValue.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEmitter.h"
#include "NiagaraEditorModule.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraLightRendererProperties.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemEditorData.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/ReferenceChainSearch.h"
#include "UObject/UObjectIterator.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraEmitterViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/INiagaraStackItemGroupAddUtilities.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackFunctionInput.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackRenderItemGroup.h"
#include "ViewModels/Stack/NiagaraStackRendererItem.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentNiagaraTests
{
inline const TCHAR* const DirectionalBurstSystem = TEXT("/Niagara/DefaultAssets/Templates/Systems/DirectionalBurst.DirectionalBurst");
inline const TCHAR* const TemplateEmitters = TEXT("/Niagara/DefaultAssets/Templates/Emitters");
inline const TCHAR* const FountainEmitter = TEXT("/Niagara/DefaultAssets/Templates/Emitters/Fountain.Fountain");
inline const TCHAR* const TemplateSystems[] = {
	TEXT("/Niagara/DefaultAssets/Templates/Systems/AttributeReaderTrails.AttributeReaderTrails"),
	TEXT("/Niagara/DefaultAssets/Templates/Systems/DirectionalBurst.DirectionalBurst"),
	TEXT("/Niagara/DefaultAssets/Templates/Systems/DirectionalBurstLightweight.DirectionalBurstLightweight"),
	TEXT("/Niagara/DefaultAssets/Templates/Systems/FountainLightweight.FountainLightweight"),
	TEXT("/Niagara/DefaultAssets/Templates/Systems/MinimalLightweight.MinimalLightweight"),
	TEXT("/Niagara/DefaultAssets/Templates/Systems/RadialBurst.RadialBurst"),
	TEXT("/Niagara/DefaultAssets/Templates/Systems/SimpleExplosion.SimpleExplosion"),
};
inline const TCHAR* const EmitterStackKeys[] = {TEXT("EmitterSpawn"), TEXT("EmitterUpdate"), TEXT("ParticleSpawn"), TEXT("ParticleUpdate"),
	TEXT("EventHandlers"), TEXT("SimulationStages"), TEXT("ScratchPads")};

// ---- documents --------------------------------------------------------------------------------------------

inline TSharedPtr<FJsonObject> Parse(const FString& Text)
{
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	FJsonSerializer::Deserialize(Reader, Object);
	return Object;
}

inline TSharedPtr<FJsonObject> Clone(const TSharedPtr<FJsonObject>& Object)
{
	return Parse(Serialize(Object));
}

inline TSharedPtr<FJsonObject> Child(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
{
	const TSharedPtr<FJsonObject>* Found = nullptr;
	return Object.IsValid() && Object->TryGetObjectField(Name, Found) ? *Found : nullptr;
}

inline TArray<TSharedPtr<FJsonValue>> List(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
{
	const TArray<TSharedPtr<FJsonValue>>* Found = nullptr;
	return Object.IsValid() && Object->TryGetArrayField(Name, Found) ? *Found : TArray<TSharedPtr<FJsonValue>>();
}

inline TSharedPtr<FJsonObject> Named(const TArray<TSharedPtr<FJsonValue>>& Entries, const FString& Name)
{
	for (const TSharedPtr<FJsonValue>& Entry : Entries)
	{
		const TSharedPtr<FJsonObject> Object = Entry.IsValid() ? Entry->AsObject() : nullptr;
		if (Object.IsValid() && Object->GetStringField(TEXT("Name")) == Name)
		{
			return Object;
		}
	}
	return nullptr;
}

// The document's content (Properties and Body), as text: what two assets must share.
inline FString Content(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	if (Document.IsValid())
	{
		Out->SetObjectField(TEXT("Properties"), Child(Document, TEXT("Properties")));
		Out->SetObjectField(TEXT("Body"), Child(Document, TEXT("Body")));
	}
	return Serialize(Out);
}

inline TSharedPtr<FJsonObject> Retargeted(const TSharedPtr<FJsonObject>& Document, const FTarget& Target, const TCHAR* Action)
{
	const TSharedPtr<FJsonObject> Copy = Clone(Document);
	Copy->SetStringField(TEXT("Target"), Target.PackageName);
	Copy->SetStringField(TEXT("Action"), Action);
	return Copy;
}

// System-level regions only: Properties, UserParameters, and per emitter its handle fields, Properties and
// Renderers (module stacks dropped unless bKeepStacks).
inline TSharedPtr<FJsonObject> SystemLevel(const TSharedPtr<FJsonObject>& Document, bool bKeepStacks)
{
	const TSharedPtr<FJsonObject> Out = Clone(Document);
	const TSharedPtr<FJsonObject> Body = Child(Out, TEXT("Body"));
	for (const TCHAR* Key : {TEXT("SystemSpawn"), TEXT("SystemUpdate"), TEXT("ScratchPads")})
	{
		Body->RemoveField(Key);
	}
	if (!bKeepStacks)
	{
		for (const TSharedPtr<FJsonValue>& Entry : List(Body, TEXT("Emitters")))
		{
			for (const TCHAR* Key : EmitterStackKeys)
			{
				Entry->AsObject()->RemoveField(Key);
			}
			// Apply-only keys (how an emitter is made), not state.
			Entry->AsObject()->RemoveField(TEXT("From"));
			Entry->AsObject()->RemoveField(TEXT("RenamedFrom"));
		}
	}
	return Out;
}

// The document value at a diagnostic's JSON pointer (for the log of a failed apply).
inline FString DocumentAt(const TSharedPtr<FJsonObject>& Document, const FString& Pointer)
{
	TArray<FString> Parts;
	Pointer.ParseIntoArray(Parts, TEXT("/"));
	TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueObject>(Document);
	for (const FString& Part : Parts)
	{
		const FString Key = Part.Replace(TEXT("~1"), TEXT("/")).Replace(TEXT("~0"), TEXT("~"));
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Value->TryGetArray(Array) && Key.IsNumeric() && Array->IsValidIndex(FCString::Atoi(*Key)))
		{
			Value = (*Array)[FCString::Atoi(*Key)];
		}
		else if (Value->TryGetObject(Object) && (*Object)->HasField(Key))
		{
			Value = (*Object)->TryGetField(Key);
		}
		else
		{
			break;
		}
	}
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Value, FString(), Writer);
	return Text;
}

inline FAssetDocumentResult ApplyDocument(FAutomationTestBase& Test, const FTarget& Target, const TSharedPtr<FJsonObject>& Document, const FString& Stage)
{
	FAssetDocumentResult Result = Apply(Test, Target, Serialize(Document), Stage);
	if (!Result.IsSuccess())
	{
		for (const FAssetDocumentDiagnostic& Diagnostic : Result.Diagnostics)
		{
			// The node of a graph diagnostic (".../Nodes/<n>/...") or the value itself.
			FString Path = Diagnostic.Path;
			const int32 Nodes = Path.Find(TEXT("/Nodes/"));
			if (Nodes != INDEX_NONE)
			{
				const int32 End = Path.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Nodes + 7);
				Path = End != INDEX_NONE ? Path.Left(End) : Path;
			}
			else if (Path.Contains(TEXT("/Parameters/")))
			{
				// The whole parameter entry.
				const int32 Parameters = Path.Find(TEXT("/Parameters/"));
				const int32 End = Path.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Parameters + 12);
				Path = End != INDEX_NONE ? Path.Left(End) : Path;
			}
			UE_LOG(LogTemp, Display, TEXT("AssetDocumentNiagaraTests %s at %s: %s"), *Stage, *Path, *DocumentAt(Document, Path).Left(3000));
			// A link diagnostic: the nodes at its ends too.
			const int32 Links = Path.Find(TEXT("/Links/"));
			if (Links != INDEX_NONE)
			{
				const FString LinkText = DocumentAt(Document, Path);
				const FString NodesText = DocumentAt(Document, Path.Left(Links) + TEXT("/Nodes"));
				const TSharedPtr<FJsonObject> Link = Parse(LinkText);
				TArray<TSharedPtr<FJsonValue>> AllNodes;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(NodesText);
				FJsonSerializer::Deserialize(Reader, AllNodes);
				for (const TCHAR* End : {TEXT("From"), TEXT("To")})
				{
					FString Address;
					FString NodeId;
					if (Link.IsValid() && Link->TryGetStringField(End, Address) && Address.Split(TEXT(":"), &NodeId, nullptr))
					{
						for (const TSharedPtr<FJsonValue>& Node : AllNodes)
						{
							FString Id;
							if (Node.IsValid() && Node->Type == EJson::Object && Node->AsObject()->TryGetStringField(TEXT("Id"), Id) && Id == NodeId)
							{
								FString NodeText;
								const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&NodeText);
								FJsonSerializer::Serialize(Node->AsObject().ToSharedRef(), Writer);
								UE_LOG(LogTemp, Display, TEXT("AssetDocumentNiagaraTests %s link %s node: %s"), *Stage, End, *NodeText.Left(3000));
							}
						}
					}
				}
			}
		}
	}
	return Result;
}

inline FAssetDocumentResult DiffDocument(const TSharedPtr<FJsonObject>& Document)
{
	FAssetDocumentDiffRequest Request;
	Request.Document = Document;
	return FAssetDocumentModule::Get().GetService().Diff(Request);
}

// The entries of a Diff result (its "changed" list; a profile receipt's "added" / "removed" too), or INDEX_NONE when
// the payload has none of them (not a Diff result).
inline int32 DiffCount(const FAssetDocumentResult& Result)
{
	int32 Count = INDEX_NONE;
	for (const TCHAR* Bucket : {TEXT("changed"), TEXT("added"), TEXT("removed")})
	{
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (Result.Payload.IsValid() && Result.Payload->TryGetArrayField(Bucket, Entries))
		{
			Count = FMath::Max(Count, 0) + Entries->Num();
		}
	}
	return Count;
}

// Re-applying the asset's own extract changes nothing: success, nothing saved, package clean, file untouched.
inline void TestNoOpReapply(FAutomationTestBase& Test, const FTarget& Target, const FString& Stage)
{
	const TSharedPtr<FJsonObject> Document = Extract(Test, Target, Stage + TEXT(" extract"));
	if (!Document.IsValid())
	{
		return;
	}
	const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Target.PackageFile);
	const FAssetDocumentResult Result = ApplyDocument(Test, Target, Retargeted(Document, Target, TEXT("Update")), Stage + TEXT(" re-apply"));
	Test.TestTrue(*FString::Printf(TEXT("%s: re-apply succeeds"), *Stage), Result.IsSuccess());
	Test.TestFalse(*FString::Printf(TEXT("%s: re-apply saves nothing"), *Stage), Result.bSavedAsset);
	Test.TestEqual(*FString::Printf(TEXT("%s: package file untouched"), *Stage), IFileManager::Get().GetTimeStamp(*Target.PackageFile), Stamp);
	const UObject* Asset = Target.Get();
	Test.TestTrue(*FString::Printf(TEXT("%s: package stays clean"), *Stage), Asset != nullptr && !Asset->GetOutermost()->IsDirty());
	const FAssetDocumentResult Diff = DiffDocument(Document);
	Test.TestTrue(*FString::Printf(TEXT("%s: Diff of the extract is empty"), *Stage), Diff.IsSuccess() && DiffCount(Diff) == 0);
	if (!Diff.IsSuccess())
	{
		LogResult(Test, Stage + TEXT(" Diff"), Diff);
	}
	if (DiffCount(Diff) > 0)
	{
		Test.AddInfo(FString::Printf(TEXT("%s diff: %s"), *Stage, *Serialize(Diff.Payload).Left(3000)));
		UE_LOG(LogTemp, Display, TEXT("AssetDocumentNiagaraTests %s diff: %s"), *Stage, *Serialize(Diff.Payload).Left(3000));
	}
}

// Unload the package and extract it from disk again.
inline void TestReload(FAutomationTestBase& Test, const FTarget& Target, const FString& Stage)
{
	const TSharedPtr<FJsonObject> Before = Extract(Test, Target, Stage + TEXT(" before reload"));
	UObject* Asset = Target.Get();
	if (!Before.IsValid() || Asset == nullptr)
	{
		return;
	}
	Test.TestFalse(*FString::Printf(TEXT("%s: clean before unload"), *Stage), Asset->GetOutermost()->IsDirty());
	ReleaseUndoHistory();
	TArray<UPackage*> Staged;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		if (It->GetName().StartsWith(TEXT("/Temp/AssetDocument/Runtime/Staging/")))
		{
			Staged.Add(*It);
		}
	}
	if (!Staged.IsEmpty())
	{
		AssetDocumentNiagaraTests::UnloadPackages(Staged);
	}
	AssetDocumentNiagaraTests::UnloadPackages({Asset->GetOutermost()});
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	const TSharedPtr<FJsonObject> After = Extract(Test, Target, Stage + TEXT(" after reload"));
	TestTextEqual(Test, FString::Printf(TEXT("%s: the saved system reloads to the same document"), *Stage), Serialize(After), Serialize(Before));
}

// What still holds an asset that should be gone (diagnostics of a failed rollback).
inline void LogResidue(FAutomationTestBase& Test, const FTarget& Target, const FString& Stage)
{
	UObject* Asset = Target.Get();
	if (Asset == nullptr)
	{
		return;
	}
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	if (UObject* Alive = Target.Get())
	{
		FReferenceChainSearch Search(Alive, EReferenceChainSearchMode::Shortest | EReferenceChainSearchMode::PrintResults);
		Test.AddInfo(FString::Printf(TEXT("%s residue: flags 0x%x rooted %d dirty %d chains: %s"), *Stage, static_cast<uint32>(Alive->GetFlags()),
			Alive->IsRooted() ? 1 : 0, Alive->GetOutermost()->IsDirty() ? 1 : 0, *Search.GetRootPath().Left(3000)));
	}
}

// ---- the editor's own API -----------------------------------------------------------------------------------

inline FNiagaraSystemViewModelOptions HeadlessOptions(const UNiagaraSystem& System)
{
	FNiagaraSystemViewModelOptions Options;
	Options.bCanModifyEmittersFromTimeline = false;
	Options.bCanSimulate = false;
	Options.bCanAutoCompile = false;
	Options.bIsForDataProcessingOnly = true;
	Options.EditMode = ENiagaraSystemViewModelEditMode::SystemAsset;
	Options.MessageLogGuid = System.GetAssetGuid();
	return Options;
}

inline TSharedRef<FNiagaraSystemViewModel> MakeViewModel(UNiagaraSystem& System)
{
	const TSharedRef<FNiagaraSystemViewModel> ViewModel = MakeShared<FNiagaraSystemViewModel>();
	ViewModel->Initialize(System, HeadlessOptions(System));
	return ViewModel;
}

inline TSharedPtr<FNiagaraEmitterHandleViewModel> HandleNamed(FNiagaraSystemViewModel& ViewModel, const FString& Name)
{
	for (const TSharedRef<FNiagaraEmitterHandleViewModel>& Handle : ViewModel.GetEmitterHandleViewModels())
	{
		if (Handle->GetName().ToString() == Name)
		{
			return Handle.ToSharedPtr();
		}
	}
	return nullptr;
}

inline UNiagaraStackRenderItemGroup* RenderGroupOf(FNiagaraEmitterHandleViewModel& Handle)
{
	TArray<UNiagaraStackRenderItemGroup*> Groups;
	if (UNiagaraStackViewModel* Stack = Handle.GetEmitterStackViewModel())
	{
		if (Stack->GetRootEntry() != nullptr)
		{
			Stack->GetRootEntry()->GetUnfilteredChildrenOfType(Groups);
		}
	}
	return Groups.IsEmpty() ? nullptr : Groups[0];
}

// The Render group's "+" menu entry for a renderer class.
inline bool AddRendererLikeTheEditor(FNiagaraEmitterHandleViewModel& Handle, const UClass& RendererClass)
{
	UNiagaraStackRenderItemGroup* Group = RenderGroupOf(Handle);
	INiagaraStackItemGroupAddUtilities* AddUtilities = Group != nullptr ? Group->GetAddUtilities() : nullptr;
	if (AddUtilities == nullptr)
	{
		return false;
	}
	TArray<TSharedRef<INiagaraStackItemGroupAddAction>> Actions;
	AddUtilities->GenerateAddActions(Actions, FNiagaraStackItemGroupAddOptions());
	for (const FNiagaraRendererCreationInfo& Info : FNiagaraEditorModule::Get().GetRendererCreationInfos())
	{
		if (FindObject<UClass>(nullptr, *Info.RendererClassPath.ToString()) != &RendererClass)
		{
			continue;
		}
		for (const TSharedRef<INiagaraStackItemGroupAddAction>& Action : Actions)
		{
			if (Action->GetDisplayName().EqualTo(Info.DisplayName))
			{
				AddUtilities->ExecuteAddAction(Action, INDEX_NONE);
				return true;
			}
		}
	}
	return false;
}

inline UNiagaraStackRendererItem* RendererItem(FNiagaraEmitterHandleViewModel& Handle, const UNiagaraRendererProperties* Renderer)
{
	if (UNiagaraStackRenderItemGroup* Group = RenderGroupOf(Handle))
	{
		Group->RefreshChildren();
	}
	TArray<UNiagaraStackEntry*> Entries;
	Handle.GetRendererEntries(Entries);
	for (UNiagaraStackEntry* Entry : Entries)
	{
		UNiagaraStackRendererItem* Item = Cast<UNiagaraStackRendererItem>(Entry);
		if (Item != nullptr && Item->GetRendererProperties() == Renderer)
		{
			return Item;
		}
	}
	return nullptr;
}

// A Details edit: Modify, PreEditChange, write, PostEditChangeProperty.
template <typename FWrite>
inline void DetailsEdit(UObject& Object, const TCHAR* PropertyName, FWrite&& Write)
{
	FProperty* Property = Object.GetClass()->FindPropertyByName(PropertyName);
	Object.Modify();
	Object.PreEditChange(Property);
	Write();
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	Object.PostEditChangeProperty(Event);
}

inline void CompileAndWait(UNiagaraSystem& System)
{
	System.RequestCompile(false);
	const double Start = FPlatformTime::Seconds();
	while (System.HasOutstandingCompilationRequests(false) && FPlatformTime::Seconds() - Start < 600.0)
	{
		System.WaitForCompilationComplete(false, false);
	}
}

inline UNiagaraStackFunctionInput* ModuleInput(FNiagaraSystemViewModel& ViewModel, const FString& Emitter, const FString& Module, const FString& Input)
{
	const TSharedPtr<FNiagaraEmitterHandleViewModel> Handle = HandleNamed(ViewModel, Emitter);
	UNiagaraStackViewModel* Stack = Handle.IsValid() ? Handle->GetEmitterStackViewModel() : nullptr;
	TArray<UNiagaraStackModuleItem*> Modules;
	if (Stack != nullptr && Stack->GetRootEntry() != nullptr)
	{
		Stack->GetRootEntry()->GetUnfilteredChildrenOfType(Modules, true);
	}
	for (UNiagaraStackModuleItem* Item : Modules)
	{
		if (Item->GetModuleNode().GetFunctionName() != Module)
		{
			continue;
		}
		for (UNiagaraStackFunctionInput* Candidate : UE::AssetDocument::Niagara::ModuleInputs(*Item))
		{
			if (Candidate->GetInputParameterHandle().GetName().ToString() == Input)
			{
				return Candidate;
			}
		}
	}
	return nullptr;
}

inline FString CompileErrors(const UNiagaraSystem& System)
{
	TArray<FString> Errors;
	for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
	{
		if (!Handle.GetIsEnabled() || Handle.GetEmitterData() == nullptr)
		{
			continue;
		}
		TArray<UNiagaraScript*> Scripts;
		Handle.GetEmitterData()->GetScripts(Scripts, false);
		for (const UNiagaraScript* Script : Scripts)
		{
			if (Script != nullptr && Script->GetLastCompileStatus() == ENiagaraScriptCompileStatus::NCS_Error)
			{
				Errors.Add(Handle.GetName().ToString() + TEXT("/") + Script->GetName());
			}
		}
	}
	return FString::Join(Errors, TEXT(", "));
}

// ---- round trips ------------------------------------------------------------------------------------------

// Set Parameters modules get editor-made names (SetVariables_<32 hex digits>): masked for comparisons.
inline FString MaskEditorNames(FString Text)
{
	const FString Prefix = TEXT("SetVariables_");
	int32 From = 0;
	while ((From = Text.Find(Prefix, ESearchCase::CaseSensitive, ESearchDir::FromStart, From)) != INDEX_NONE)
	{
		const int32 Start = From + Prefix.Len();
		int32 End = Start;
		while (End < Text.Len() && FChar::IsHexDigit(Text[End]))
		{
			++End;
		}
		if (End - Start == 32)
		{
			Text.RemoveAt(Start, 32);
			Text.InsertAt(Start, TEXT("#"));
		}
		From = Start;
	}
	return Text;
}

// Emitter scratch pads come with their emitter (an emitter asset's pads); the system editor cannot add them.
inline bool HasEmitterScratchPads(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonObject> Body = Child(Document, TEXT("Body"));
	for (const TSharedPtr<FJsonValue>& Entry : List(Body, TEXT("Emitters")))
	{
		if (!List(Entry->AsObject(), TEXT("ScratchPads")).IsEmpty())
		{
			return true;
		}
	}
	return false;
}

// Modules whose Name the editor never gives a new module: not the script's name or the script's name plus digits
// (UNiagaraNodeFunctionCall::ComputeNodeName; legacy content whose module script was renamed later, e.g. UpdateAge
// on ParticleState). Creating them is refused (Niagara.InvalidModuleName); editing them in place is not.
inline TArray<FString> LegacyModuleNames(const TSharedPtr<FJsonObject>& Document)
{
	TArray<FString> Out;
	auto Visit = [&Out](const TArray<TSharedPtr<FJsonValue>>& Modules)
	{
		for (const TSharedPtr<FJsonValue>& Entry : Modules)
		{
			const TSharedPtr<FJsonObject> Module = Entry->AsObject();
			FString Name;
			FString Script;
			if (!Module.IsValid() || !Module->TryGetStringField(TEXT("Name"), Name) || !Module->TryGetStringField(TEXT("Script"), Script)
				|| Script.StartsWith(TEXT("ScratchPad:")))
			{
				continue;
			}
			FString ScriptName;
			if (!Script.Split(TEXT("."), nullptr, &ScriptName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
			{
				ScriptName = Script;
			}
			if (Name != ScriptName && !(Name.StartsWith(ScriptName, ESearchCase::CaseSensitive) && Name.RightChop(ScriptName.Len()).IsNumeric()))
			{
				Out.Add(FString::Printf(TEXT("%s (%s)"), *Name, *ScriptName));
			}
		}
	};
	auto VisitGroups = [&Visit](const TSharedPtr<FJsonObject>& Owner)
	{
		for (const TCHAR* Key : {TEXT("SystemSpawn"), TEXT("SystemUpdate"), TEXT("EmitterSpawn"), TEXT("EmitterUpdate"), TEXT("ParticleSpawn"),
			TEXT("ParticleUpdate")})
		{
			Visit(List(Owner, Key));
		}
		for (const TCHAR* Key : {TEXT("EventHandlers"), TEXT("SimulationStages")})
		{
			for (const TSharedPtr<FJsonValue>& Entry : List(Owner, Key))
			{
				Visit(List(Entry->AsObject(), TEXT("Modules")));
			}
		}
	};
	const TSharedPtr<FJsonObject> Body = Child(Document, TEXT("Body"));
	VisitGroups(Body);
	for (const TSharedPtr<FJsonValue>& Entry : List(Body, TEXT("Emitters")))
	{
		VisitGroups(Entry->AsObject());
	}
	return Out;
}

// Apply-only emitter keys (how an emitter is made) dropped: what an extract shows. Documents compared with a
// document parsed from text go through Clone too (numbers are then printed alike).
inline TSharedPtr<FJsonObject> WithoutMakeKeys(const TSharedPtr<FJsonObject>& Document)
{
	const TSharedPtr<FJsonObject> Out = Clone(Document);
	for (const TSharedPtr<FJsonValue>& Entry : List(Child(Out, TEXT("Body")), TEXT("Emitters")))
	{
		Entry->AsObject()->RemoveField(TEXT("From"));
		Entry->AsObject()->RemoveField(TEXT("RenamedFrom"));
	}
	return Out;
}

struct FRoundTripTiming
{
	double CreateMs = 0.0;
	double ExtractMs = 0.0;
	double ReapplyMs = 0.0;
};

// The extract of Source (an asset of the test folder) written as a Create document of a new system: emitters with a
// Parent are added from it, lightweight ones as lightweight, the others empty, and every region is authored
// (stacks, inputs, handlers, stages, renderers, properties). The created system's document equals the source's
// (Set Parameters names masked), and re-applying it is a no-op. System scratch pads are added and authored;
// emitters with scratch pads and no Parent cannot be made by the system editor (their pads come with an emitter asset),
// systems with legacy module names (LegacyModuleNames), deprecated data interface functions or Vector / Position links
// without a conversion node in a scratch pad cannot be created through the editor (OutNote).
inline bool RoundTrip(FAutomationTestBase& Test, const FString& Label, const FTarget& Source, FRoundTripTiming& OutTiming, FString& OutNote)
{
	const TSharedPtr<FJsonObject> Extracted = Extract(Test, Source, Label + TEXT(" round trip source"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	if (HasEmitterScratchPads(Extracted))
	{
		OutNote = TEXT("emitter scratch pads come with an emitter asset (the system editor cannot add them): round tripped on duplicates only");
		return false;
	}
	const FTarget Fresh(FString(TEXT("NS_RT_")) + Label.Left(30));
	ON_SCOPE_EXIT
	{
		Fresh.Cleanup();
	};
	FString Text = Serialize(Extracted);
	Text.ReplaceInline(*Source.ObjectPath, *Fresh.ObjectPath, ESearchCase::CaseSensitive);
	Text.ReplaceInline(*Source.PackageName, *Fresh.PackageName, ESearchCase::CaseSensitive);
	const TSharedPtr<FJsonObject> Document = Retargeted(Parse(Text), Fresh, TEXT("Create"));
	for (const TSharedPtr<FJsonValue>& Entry : List(Child(Document, TEXT("Body")), TEXT("Emitters")))
	{
		const TSharedPtr<FJsonObject> Emitter = Entry->AsObject();
		FString Mode;
		const bool bStateless = Emitter->TryGetStringField(TEXT("Mode"), Mode) && Mode == TEXT("Stateless");
		if (!bStateless && !Emitter->HasField(TEXT("Parent")))
		{
			Emitter->SetStringField(TEXT("From"), TEXT("Empty"));
		}
	}
	double Start = FPlatformTime::Seconds();
	const FAssetDocumentResult Created = ApplyDocument(Test, Fresh, Document, Label + TEXT(" round trip create"));
	OutTiming.CreateMs = Milliseconds(Start);
	// Scratch pad graphs apply before the stacks: a legacy pad refusal can come first.
	const bool bLegacyPadRefused = !Created.IsSuccess()
		&& (HasDiagnostic(Created, TEXT("Niagara.DeprecatedFunction")) || HasDiagnostic(Created, TEXT("Graph.ConnectNeedsConversion")));
	const TArray<FString> Legacy = LegacyModuleNames(Document);
	if (!Legacy.IsEmpty())
	{
		Test.TestTrue(*FString::Printf(TEXT("%s: a module name the editor cannot give is refused (Niagara.InvalidModuleName)"), *Label),
			!Created.IsSuccess() && (HasDiagnostic(Created, TEXT("Niagara.InvalidModuleName")) || bLegacyPadRefused));
		Test.TestFalse(*FString::Printf(TEXT("%s: the refused create leaves no asset"), *Label), Fresh.Exists());
		OutNote = FString::Printf(TEXT("module names the editor never gives a new module: %s (refused, not round tripped)"),
			*FString::Join(Legacy, TEXT(", ")));
		return false;
	}
	if (!Created.IsSuccess() && HasDiagnostic(Created, TEXT("Niagara.DeprecatedFunction")))
	{
		// A scratch pad calls a data interface function deprecated since the asset was made: the graph menu no longer
		// offers it, so neither the editor nor a document can place that node again.
		Test.TestFalse(*FString::Printf(TEXT("%s: the refused create leaves no asset"), *Label), Fresh.Exists());
		OutNote = TEXT("a scratch pad calls a deprecated data interface function the menu no longer offers (refused, not round tripped)");
		return false;
	}
	if (!Created.IsSuccess() && HasDiagnostic(Created, TEXT("Graph.ConnectNeedsConversion")))
	{
		// A scratch pad links a Vector straight to a Position pin (saved before the Position type and strict stack
		// types): the editor now joins those pins only through a conversion node, so the same graph cannot be made again.
		Test.TestFalse(*FString::Printf(TEXT("%s: the refused create leaves no asset"), *Label), Fresh.Exists());
		OutNote = TEXT("a scratch pad links Vector and Position pins directly, which the editor now joins only through a conversion node (refused, not round tripped)");
		return false;
	}
	if (!Created.IsSuccess() && HasDiagnostic(Created, TEXT("Niagara.CompileFailed")))
	{
		// A create whose compile has errors is refused. When the source itself does not compile in this project
		// (e.g. mesh distance fields disabled in the project settings), that is the expected outcome.
		UNiagaraSystem* SourceSystem = Cast<UNiagaraSystem>(Source.Get());
		if (SourceSystem != nullptr)
		{
			CompileAndWait(*SourceSystem);
			const FString SourceErrors = CompileErrors(*SourceSystem);
			if (!SourceErrors.IsEmpty())
			{
				Test.TestFalse(*FString::Printf(TEXT("%s: the refused create leaves no asset"), *Label), Fresh.Exists());
				OutNote = FString::Printf(TEXT("the source does not compile in this project (%s): its create is refused (Niagara.CompileFailed)"),
					*SourceErrors.Left(300));
				return false;
			}
		}
	}
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: the round trip document creates the system"), *Label), Created.IsSuccess()))
	{
		LogResidue(Test, Fresh, Label);
		return false;
	}
	const TSharedPtr<FJsonObject> Result = Extract(Test, Fresh, Label + TEXT(" round trip result"), &OutTiming.ExtractMs);
	TestTextEqual(Test, FString::Printf(TEXT("%s: round trip == source"), *Label), MaskEditorNames(Content(Clone(Result))),
		MaskEditorNames(Content(WithoutMakeKeys(Document))));
	Start = FPlatformTime::Seconds();
	TestNoOpReapply(Test, Fresh, Label + TEXT(" round trip"));
	OutTiming.ReapplyMs = Milliseconds(Start);
	return true;
}
} // namespace AssetDocumentNiagaraTests
#endif
