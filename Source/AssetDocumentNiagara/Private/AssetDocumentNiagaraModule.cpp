// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentNiagaraPrimitives.h"
#include "AssetDocumentRuntimeModule.h"
#include "Modules/ModuleManager.h"
#include "Runtime/AssetDocumentPackageSave.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"

// The "fx" provider is assembled by the runtime bootstrap (AssetDocumentIntegration); it is
// stateless. The module registers the compile status describer (the "Compile" block of Extract and
// Apply results for systems) and the Core package-snapshot rebuilder of systems (a rolled-back system
// is recompiled from its restored graphs), the script rebuilder (a rolled-back module / function / dynamic input is
// recompiled), the emitter asset rebuilder (loaded children re-merge a rolled-back emitter) the Niagara adapters of the shared graph primitives (script graphs and scratch pads), the open-editor handler
// (AssetDocumentNiagaraOpenEditor.cpp) and the settling of emitter merge copies before a package save.
class FAssetDocumentNiagaraModule final : public IAssetDocumentRuntimeModule
{
public:
	virtual void StartupModule() override
	{
		UE::AssetDocument::Niagara::RegisterNiagaraCompileStatusDescriber();
		SystemRebuilder = UE::AssetDocument::Niagara::MakeSystemRebuilder();
		RegisterAssetDocumentDerivedStateRebuilder(SystemRebuilder.ToSharedRef());
		ScriptRebuilder = UE::AssetDocument::Niagara::MakeScriptRebuilder();
		RegisterAssetDocumentDerivedStateRebuilder(ScriptRebuilder.ToSharedRef());
		EmitterRebuilder = UE::AssetDocument::Niagara::MakeEmitterRebuilder();
		RegisterAssetDocumentDerivedStateRebuilder(EmitterRebuilder.ToSharedRef());
		UE::AssetDocument::Niagara::RegisterNiagaraGraphAdapters();
		UE::AssetDocument::Niagara::RegisterNiagaraOpenEditorHandler();
		BeforeSave = UE::AssetDocument::PackageSave::OnBeforeSave().AddStatic(&UE::AssetDocument::Niagara::SettleMergeCopies);
	}

	virtual void ShutdownModule() override
	{
		UE::AssetDocument::PackageSave::OnBeforeSave().Remove(BeforeSave);
		UE::AssetDocument::Niagara::UnregisterNiagaraOpenEditorHandler();
		UE::AssetDocument::Niagara::UnregisterNiagaraGraphAdapters();
		if (EmitterRebuilder.IsValid())
		{
			UnregisterAssetDocumentDerivedStateRebuilder(EmitterRebuilder->GetId());
		}
		if (ScriptRebuilder.IsValid())
		{
			UnregisterAssetDocumentDerivedStateRebuilder(ScriptRebuilder->GetId());
		}
		if (SystemRebuilder.IsValid())
		{
			UnregisterAssetDocumentDerivedStateRebuilder(SystemRebuilder->GetId());
		}
		UE::AssetDocument::Niagara::UnregisterNiagaraCompileStatusDescriber();
	}

	virtual FName GetRuntimeModuleName() const override { return TEXT("AssetDocumentNiagara"); }

private:
	TSharedPtr<IAssetDocumentDerivedStateRebuilder> SystemRebuilder;
	TSharedPtr<IAssetDocumentDerivedStateRebuilder> ScriptRebuilder;
	TSharedPtr<IAssetDocumentDerivedStateRebuilder> EmitterRebuilder;
	FDelegateHandle BeforeSave;
};

IMPLEMENT_MODULE(FAssetDocumentNiagaraModule, AssetDocumentNiagara)
