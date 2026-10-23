// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Open-editor apply for Sound Cues. The Sound Cue editor (FSoundCueEditor) edits the cue itself: its graph panel shows
// USoundCue::SoundCueGraph and its Details panel the selected graph nodes (or the cue). Nothing is copied, so an apply
// runs in place; afterwards the editor is refreshed as its own Undo does (FSoundCueEditor::PostUndo: clear the
// selection so the Details panel drops removed nodes, then NotifyGraphChanged so the panel rebuilds its node widgets).
// The same refresh follows a failed (rolled-back) apply.

#include "SoundCue/AssetDocumentSoundCue.h"

#include "GraphEditor.h"
#include "Runtime/AssetDocumentOpenEditor.h"
#include "Sound/SoundCue.h"
#include "SoundCueGraph/SoundCueGraph.h"

namespace UE::AssetDocument::Graph::Audio::SoundCue
{
namespace
{
class FSoundCueOpenEditorHandler final : public IAssetDocumentOpenEditorHandler
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentAudio.SoundCue"); }
	virtual bool Supports(const UObject& Asset) const override { return IsEditableCue(Asset); }
	virtual FString GetProfileName(const UObject& Asset) const override { return TEXT("SoundCue"); }
	virtual EAssetDocumentOpenEditorStrategy GetStrategy(const UObject& Asset) const override
	{
		return EAssetDocumentOpenEditorStrategy::RefreshInPlace;
	}
	virtual void RefreshOpenEditor(UObject& Asset, bool bSucceeded) const override
	{
		const USoundCue& Cue = *CastChecked<USoundCue>(&Asset);
		if (const TSharedPtr<SGraphEditor> GraphEditor = Cue.SoundCueGraph != nullptr ? SGraphEditor::FindGraphEditorForGraph(Cue.SoundCueGraph) : nullptr)
		{
			GraphEditor->ClearSelectionSet();
			GraphEditor->NotifyGraphChanged();
		}
	}
};
} // namespace

TSharedRef<IAssetDocumentOpenEditorHandler> MakeOpenEditorHandler()
{
	return MakeShared<FSoundCueOpenEditorHandler>();
}
} // namespace UE::AssetDocument::Graph::Audio::SoundCue
