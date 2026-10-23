// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Misc/ConfigCacheIni.h"

namespace UE::AssetDocument::Graph::EditorConfirmations
{
// Ini setting of the editor's "Change Variable Type: This could break connections..." confirmation
// (FBlueprintEditorUtils::VerifyUserWantsVariableTypeChanged, shown by ChangeMemberVariableType and
// ChangeLocalVariableType when nodes use the variable).
inline constexpr const TCHAR* ChangeVariableType = TEXT("ChangeVariableType_Warning");

// Answers one of the editor's suppressible confirmations (FSuppressableWarningDialog) with its confirm button
// for the scope: while it lives, the setting reads as the user's "Don't show this again", so the editor's own
// confirmed path runs (FSuppressableWarningDialog::ShowModal returns Suppressed, which callers treat as
// Confirm) and nothing waits for a click. The document already states the intent the dialog asks about.
// The user's setting is put back when the scope ends (in memory; the ini file is not written).
class FScopedConfirmedDialog final
{
public:
	explicit FScopedConfirmedDialog(const TCHAR* InSetting)
		: Setting(InSetting)
	{
		if (GConfig == nullptr)
		{
			return;
		}
		bHadValue = GConfig->GetBool(Section, Setting, bPrevious, GEditorPerProjectIni);
		if (!bHadValue || !bPrevious)
		{
			GConfig->SetBool(Section, Setting, true, GEditorPerProjectIni);
			bChanged = true;
		}
	}

	~FScopedConfirmedDialog()
	{
		if (!bChanged || GConfig == nullptr)
		{
			return;
		}
		if (bHadValue)
		{
			GConfig->SetBool(Section, Setting, bPrevious, GEditorPerProjectIni);
		}
		else
		{
			GConfig->RemoveKey(Section, Setting, GEditorPerProjectIni);
		}
	}

	FScopedConfirmedDialog(const FScopedConfirmedDialog&) = delete;
	FScopedConfirmedDialog& operator=(const FScopedConfirmedDialog&) = delete;

private:
	// FSuppressableWarningDialog's section.
	static constexpr const TCHAR* Section = TEXT("SuppressableDialogs");
	const TCHAR* Setting;
	bool bHadValue = false;
	bool bPrevious = false;
	bool bChanged = false;
};
} // namespace UE::AssetDocument::Graph::EditorConfirmations
