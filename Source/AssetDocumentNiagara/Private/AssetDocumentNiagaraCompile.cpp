// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Compile status of a Niagara system as the toolbar shows it. The toolbar's own status is not exported, so the
// status is derived here from the same scripts (system spawn / update and every enabled emitter's compilable
// scripts): a script out of sync with its source or Unknown / Dirty / BeingCreated ->
// Dirty, else any error -> Error, any warning -> UpToDateWithWarnings, else UpToDate. Messages are the
// warnings and errors of each script's last compile. Reads never compile; a read that finds a compile in
// flight (Dirty with a request outstanding) waits for it and reports its outcome (Compiling past the budget).
//
// fx.niagara.compile.v1 is the toolbar's Compile without its progress UI: UNiagaraSystem::RequestCompile, then
// WaitForCompilationComplete(bIncludingGPUShaders = false, bShowProgress = false) until no request is
// outstanding or the time budget runs out (under automation one wait gives up after a few log intervals).
// Output {status, errors: [message], warnings: [message], timed_out}. GPU shader compilation is not awaited.
// The rebuilder (AssetDocumentNiagara.System) runs the same compile after Core restores a system's package.
// On a module / function / dynamic input asset it is the script editor's Compile (AssetDocumentNiagaraScriptEdit.cpp).

#include "AssetDocumentNiagaraPrimitives.h"

#include "AssetDocumentCompileStatus.h"
#include "AssetDocumentEditorNonInteractiveScope.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "HAL/PlatformTime.h"
#include "AssetCompilingManager.h"

namespace UE::AssetDocument::Niagara
{
namespace
{
const FName DescriberId(TEXT("AssetDocumentNiagara.System"));

// Module function names by call node guid, over the system graph and each emitter graph (stack attribution).
TMap<FGuid, FString> ModuleNames(const UNiagaraSystem& System)
{
	TMap<FGuid, FString> Names;
	auto AddGraph = [&Names](const UNiagaraScript* Script)
	{
		const UNiagaraScriptSource* Source = Script != nullptr ? Cast<UNiagaraScriptSource>(Script->GetLatestSource()) : nullptr;
		if (Source == nullptr || Source->NodeGraph == nullptr)
		{
			return;
		}
		for (const UEdGraphNode* Node : Source->NodeGraph->Nodes)
		{
			if (const UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Node))
			{
				Names.Add(Call->NodeGuid, Call->GetFunctionName());
			}
		}
	};
	AddGraph(System.GetSystemSpawnScript());
	for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
	{
		if (const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
		{
			TArray<UNiagaraScript*> Scripts;
			Data->GetScripts(Scripts, false);
			if (!Scripts.IsEmpty())
			{
				AddGraph(Scripts[0]);
			}
		}
	}
	return Names;
}

TSharedPtr<FJsonObject> DescribeSystem(const UObject& Asset)
{
	const UNiagaraSystem* System = Cast<UNiagaraSystem>(&Asset);
	if (System == nullptr)
	{
		return nullptr;
	}
	TArray<FCompileMessage> Messages;
	const FString Status = ReadSettledCompileStatus(*System, Messages);
	const TMap<FGuid, FString> Names = ModuleNames(*System);
	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FCompileMessage& Message : Messages)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("Severity"), Message.Severity);
		Item->SetStringField(TEXT("Message"), Message.Message);
		for (const FGuid& Guid : Message.StackGuids)
		{
			if (const FString* Name = Names.Find(Guid))
			{
				Item->SetStringField(TEXT("Node"), *Name);
				Item->SetStringField(TEXT("NodeGuid"), Guid.ToString(EGuidFormats::DigitsWithHyphensLower));
				break;
			}
		}
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}
	TSharedPtr<FJsonObject> Compile = MakeShared<FJsonObject>();
	Compile->SetStringField(TEXT("Status"), Status);
	Compile->SetArrayField(TEXT("Messages"), Items);
	return Compile;
}
} // namespace

FString ReadCompileStatus(const UNiagaraSystem& System, TArray<FCompileMessage>& OutMessages)
{
	TArray<UNiagaraScript*> Scripts;
	Scripts.Add(const_cast<UNiagaraScript*>(System.GetSystemSpawnScript()));
	Scripts.Add(const_cast<UNiagaraScript*>(System.GetSystemUpdateScript()));
	for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
	{
		if (Handle.GetIsEnabled() && Handle.GetEmitterData() != nullptr)
		{
			Handle.GetEmitterData()->GetScripts(Scripts, true);
		}
	}
	bool bDirty = false;
	bool bError = false;
	bool bWarning = false;
	const bool bCompiling = System.HasOutstandingCompilationRequests(false);
	for (const UNiagaraScript* Script : Scripts)
	{
		if (Script == nullptr)
		{
			continue;
		}
		const ENiagaraScriptCompileStatus Status = Script->GetLastCompileStatus();
		// A finished compile that left the script without executable data (no status, no events): the Vector VM
		// compile failed and Unreal wrote its errors only to the Output Log (LogNiagaraCompiler).
		if (Status == ENiagaraScriptCompileStatus::NCS_Unknown && !bCompiling && Script->AreScriptAndSourceSynchronized()
			&& Script->GetVMExecutableData().LastCompileEvents.IsEmpty())
		{
			bError = true;
			OutMessages.Add({TEXT("Error"), FString::Printf(TEXT("%s did not compile: the Vector VM compile failed (its errors are in the Output Log, LogNiagaraCompiler)."),
				*Script->GetPathName(&System)), {}});
			continue;
		}
		if (!Script->AreScriptAndSourceSynchronized() || Status == ENiagaraScriptCompileStatus::NCS_Unknown
			|| Status == ENiagaraScriptCompileStatus::NCS_Dirty || Status == ENiagaraScriptCompileStatus::NCS_BeingCreated)
		{
			bDirty = true;
		}
		bError = bError || Status == ENiagaraScriptCompileStatus::NCS_Error;
		bWarning = bWarning || Status == ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings
			|| Status == ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings;
		for (const FNiagaraCompileEvent& Event : Script->GetVMExecutableData().LastCompileEvents)
		{
			if (Event.Severity == FNiagaraCompileEventSeverity::Warning || Event.Severity == FNiagaraCompileEventSeverity::Error)
			{
				OutMessages.Add({Event.Severity == FNiagaraCompileEventSeverity::Error ? TEXT("Error") : TEXT("Warning"),
					Event.Message, Event.StackGuids});
			}
		}
	}
	return bDirty ? TEXT("Dirty") : bError ? TEXT("Error") : bWarning ? TEXT("UpToDateWithWarnings") : TEXT("UpToDate");
}

FString ReadSettledCompileStatus(const UNiagaraSystem& System, TArray<FCompileMessage>& OutMessages)
{
	const FString Status = ReadCompileStatus(System, OutMessages);
	if (Status != TEXT("Dirty") || !System.HasOutstandingCompilationRequests(false))
	{
		return Status;
	}
	// A compile is in flight, e.g. the one an editor requests when it opens (compile for edit), as an apply's
	// reopen does: report its outcome, not the moment before it (Dirty right after the apply,
	// UpToDate on the next inspect). Waiting starts no compile.
	const bool bSettled = SettleCompilation(const_cast<UNiagaraSystem&>(System));
	OutMessages.Reset();
	const FString Settled = ReadCompileStatus(System, OutMessages);
	return bSettled || Settled != TEXT("Dirty") ? Settled : FString(TEXT("Compiling"));
}

void RegisterNiagaraCompileStatusDescriber()
{
	RegisterAssetDocumentCompileStatusDescriber(DescriberId, &DescribeSystem);
}

void UnregisterNiagaraCompileStatusDescriber()
{
	UnregisterAssetDocumentCompileStatusDescriber(DescriberId);
}

bool SettleCompilation(UNiagaraSystem& System)
{
	const double Start = FPlatformTime::Seconds();
	do
	{
		System.WaitForCompilationComplete(false, false);
	}
	while (System.HasOutstandingCompilationRequests(false) && FPlatformTime::Seconds() - Start < 600.0);
	// The editor's tick (FAssetCompilingManager::ProcessAsyncTasks) is what removes retrieved tasks from the Niagara
	// compiling manager: the first pass moves them to "awaiting retrieval", the second removes them.
	FAssetCompilingManager::Get().ProcessAsyncTasks(false);
	FAssetCompilingManager::Get().ProcessAsyncTasks(false);
	return !System.HasOutstandingCompilationRequests(false);
}

bool IsDerivedNiagaraObject(const UObject& Object)
{
	if (Object.IsA<UNiagaraDataInterface>() && Object.GetOuter() != nullptr && Object.GetOuter()->IsA<UNiagaraScript>())
	{
		return true;
	}
	for (const UObject* Outer = &Object; Outer != nullptr; Outer = Outer->GetOuter())
	{
		if (Outer->IsA<UNiagaraScript>() && Outer->GetOuter() != nullptr && Outer->GetOuter()->IsA<UNiagaraNodeAssignment>())
		{
			return true;
		}
	}
	return false;
}

namespace
{
// RequestCompile and wait within the budget; true when no compile request is left outstanding.
bool CompileAndWait(UNiagaraSystem& System)
{
	System.RequestCompile(false);
	return SettleCompilation(System);
}

class FSystemRebuilder final : public IAssetDocumentDerivedStateRebuilder
{
public:
	virtual FName GetId() const override { return DescriberId; }
	virtual bool Supports(const UObject& PrimaryAsset) const override { return PrimaryAsset.IsA<UNiagaraSystem>(); }
	// A compile copies every data interface a script uses into the script (its cached default data interfaces)
	// and renames the previous copies to Invalidated_* (garbage): those copies are compile output, rebuilt by
	// the recompile below, not authored state. Authored data interfaces live on graph nodes, user parameters
	// and renderers.
	// A Set Parameters node's function script (with its source, graph and nodes) is generated from the node's
	// assignment targets: UNiagaraNodeAssignment::RefreshFromExternalChanges creates it anew under the same name
	// whenever a target is added or removed (NewObject over the existing object, which must not be referenced
	// by a snapshot's strong pointer).
	virtual EAssetDocumentSnapshotObjectRole Classify(const UObject& Object) const override
	{
		return IsDerivedNiagaraObject(Object) ? EAssetDocumentSnapshotObjectRole::Derived : IAssetDocumentDerivedStateRebuilder::Classify(Object);
	}
	virtual FAssetDocumentContractValidationResult Rebuild(UObject& PrimaryAsset) override
	{
		FAssetDocumentContractValidationResult Result;
		if (UNiagaraSystem* System = Cast<UNiagaraSystem>(&PrimaryAsset))
		{
			// The restored Set Parameters nodes regenerate their function scripts from their restored targets.
			TArray<UNiagaraGraph*> Graphs;
			auto AddSource = [&Graphs](UNiagaraScriptSourceBase* Base)
			{
				if (UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Base); Source != nullptr && Source->NodeGraph != nullptr)
				{
					Graphs.AddUnique(Source->NodeGraph);
				}
			};
			if (System->GetSystemSpawnScript() != nullptr)
			{
				AddSource(System->GetSystemSpawnScript()->GetLatestSource());
			}
			for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
			{
				if (const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
				{
					AddSource(Data->GraphSource);
				}
			}
			for (UNiagaraGraph* Graph : Graphs)
			{
				TArray<UNiagaraNodeAssignment*> Nodes;
				Graph->GetNodesOfClass(Nodes);
				for (UNiagaraNodeAssignment* Node : Nodes)
				{
					Node->RefreshFromExternalChanges();
				}
			}
			// Forced: the restored scripts carry the compile ids of the restored state, so an unforced request could
			// keep the compile output (data interface copies) of the rolled-back state.
			FAssetDocumentEditorNonInteractiveScope NonInteractive;
			System->RequestCompile(true);
			SettleCompilation(*System);
		}
		Result.bValid = true;
		return Result;
	}
};
} // namespace

bool HandleCompile(FCall& Call, FObjectBuilder& Result)
{
	FCall& C = Call;
	if (!C.Args.OnlyFields({}))
	{
		return false;
	}
	if (UNiagaraScript* Script = Cast<UNiagaraScript>(&C.Asset))
	{
		return CompileScript(C, *Script, Result);
	}
	if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(&C.Asset))
	{
		return CompileEmitter(C, *Emitter, Result);
	}
	UNiagaraSystem* System = Cast<UNiagaraSystem>(&C.Asset);
	if (System == nullptr)
	{
		return Fail(C, TEXT("Niagara.UnsupportedAsset"), FString::Printf(TEXT("%s is not a Niagara system, emitter or script."), *C.Asset.GetClass()->GetPathName()));
	}
	FAssetDocumentEditorNonInteractiveScope NonInteractive;
	const bool bDone = CompileAndWait(*System);
	TArray<FCompileMessage> Messages;
	const FString Status = ReadCompileStatus(*System, Messages);
	TArray<FString> Errors;
	TArray<FString> Warnings;
	for (const FCompileMessage& Message : Messages)
	{
		(Message.Severity == TEXT("Error") ? Errors : Warnings).AddUnique(Message.Message);
	}
	if (NonInteractive.WasInteractionRefused())
	{
		NonInteractive.DiscardCreatedPackages();
		return C.Args.Fail(TEXT("Niagara.InteractiveEditRefused"), TEXT(
			"Compiling tried to open an editor dialog, which an MCP request cannot answer."));
	}
	Result.AddString(TEXT("status"), Status);
	Result.Add(TEXT("errors"), Graph::MakeStringArray(Errors));
	Result.Add(TEXT("warnings"), Graph::MakeStringArray(Warnings));
	Result.AddBool(TEXT("timed_out"), !bDone);
	return true;
}

TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeSystemRebuilder()
{
	return MakeShared<FSystemRebuilder>();
}
} // namespace UE::AssetDocument::Niagara
