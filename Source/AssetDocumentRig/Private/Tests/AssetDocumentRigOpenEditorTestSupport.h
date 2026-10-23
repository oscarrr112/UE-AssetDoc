// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Shared helpers of the rig open-editor apply tests (ControlRig.OpenEditorApply, IKRig.OpenEditorApply.*): ticking
// the editor UI, the asset editor open on an asset, the human's save.

#pragma once

#include "AssetDocumentOpenEditorTestSupport.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "TickableEditorObject.h"
#include "Toolkits/AssetEditorToolkit.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentRigOpenEditorTests
{
inline UAssetEditorSubsystem* Editors()
{
	return GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
}

// Ticks the editor UI: Slate and the editor tickables (the editors' previews and deferred refreshes).
inline void TickEditorUi()
{
	for (int32 Tick = 0; Tick < 3; ++Tick)
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().Tick();
		}
		FTickableEditorObject::TickObjects(0.f);
	}
}

inline IAssetEditorInstance* EditorOf(UObject& Asset)
{
	UAssetEditorSubsystem* Subsystem = Editors();
	return Subsystem != nullptr ? Subsystem->FindEditorForAsset(&Asset, false) : nullptr;
}

// The asset editor named EditorName (its toolkit name) open on Asset, as its toolkit class, or null.
template <typename ToolkitType>
ToolkitType* ToolkitOf(UObject& Asset, const TCHAR* EditorName)
{
	IAssetEditorInstance* Instance = EditorOf(Asset);
	if (Instance == nullptr || Instance->GetEditorName() != FName(EditorName))
	{
		return nullptr;
	}
	return static_cast<ToolkitType*>(static_cast<FAssetEditorToolkit*>(Instance));
}

// The human's Save in the open editor (its toolbar Save, Ctrl+S): the editor's SaveAsset command, run as the shared
// open-editor test support does (from automation the bare command saves nothing: see SaveFromEditor). The bare
// command's no-op in this unattended session is checked first.
inline bool HumanSave(FAutomationTestBase& Test, UObject& Asset)
{
	namespace OpenEditorTests = UE::AssetDocument::OpenEditorTests;
	FAssetEditorToolkit* Toolkit = OpenEditorTests::FindToolkit(&Asset);
	if (!Test.TestNotNull(TEXT("the human saves in the open editor"), Toolkit))
	{
		return false;
	}
	Test.TestTrue(TEXT("unattended, the editor's bare Save command saves nothing"), OpenEditorTests::BareSaveCommandSavesNothing(*Toolkit, Asset));
	return OpenEditorTests::SaveFromEditor(*Toolkit, Asset);
}
} // namespace AssetDocumentRigOpenEditorTests
#endif // WITH_DEV_AUTOMATION_TESTS
