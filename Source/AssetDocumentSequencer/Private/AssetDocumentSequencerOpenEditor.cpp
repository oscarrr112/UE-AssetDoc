// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Open-editor apply of the Sequencer profiles (Runtime/AssetDocumentOpenEditor.h): in place. Sequencer
// edits the sequence itself (no working copy); what it holds besides is derived from the sequence: the outliner and
// track area view models, the compiled evaluation template, spawned preview objects and the pre-animated state of what
// it animates. After its own Undo it rebuilds them (FSequencer::PostUndo -> NotifyMovieSceneDataChanged(Unknown): the
// tree is refreshed at once, the instances and spawnables re-evaluate), and so it does after an apply and after a
// failed (rolled-back) one, then evaluates at the current time so the preview shows the applied sequence.
// A sequence is also shown by every Sequencer whose root sequence plays it through sub / shot sections (at any depth):
// those Sequencers are refreshed the same way (IsShownInOpenEditor), whether or not they have drilled into it.
// Sequencers are found as they are created (ISequencerModule::RegisterOnSequencerCreated): the Level Sequence editor,
// the Template Sequence editor and every other host of a Sequencer.

#include "AssetDocumentSequencerPrimitives.h"

#include "ISequencer.h"
#include "ISequencerModule.h"
#include "Modules/ModuleManager.h"
#include "MovieSceneSequence.h"
#include "Runtime/AssetDocumentOpenEditor.h"

namespace AssetDocumentSequencerOpenEditorLocal
{
TArray<TWeakPtr<ISequencer>>& LiveSequencers()
{
	static TArray<TWeakPtr<ISequencer>> Sequencers;
	return Sequencers;
}

// The open Sequencers that show Sequence (as their root, or played by their root).
TArray<TSharedPtr<ISequencer>> SequencersShowing(const UMovieSceneSequence& Sequence)
{
	TArray<TSharedPtr<ISequencer>> Showing;
	LiveSequencers().RemoveAll([](const TWeakPtr<ISequencer>& Weak) { return !Weak.IsValid(); });
	for (const TWeakPtr<ISequencer>& Weak : LiveSequencers())
	{
		const TSharedPtr<ISequencer> Sequencer = Weak.Pin();
		const UMovieSceneSequence* Root = Sequencer.IsValid() ? Sequencer->GetRootMovieSceneSequence() : nullptr;
		if (Root != nullptr && UE::AssetDocument::Graph::Sequencer::SequenceReaches(Root, &Sequence))
		{
			Showing.Add(Sequencer);
		}
	}
	return Showing;
}

class FSequenceHandler final : public IAssetDocumentOpenEditorHandler
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentSequencer.Sequence"); }
	virtual bool Supports(const UObject& Asset) const override { return Asset.IsA<UMovieSceneSequence>(); }
	virtual FString GetProfileName(const UObject& Asset) const override { return TEXT("Sequencer"); }
	virtual EAssetDocumentOpenEditorStrategy GetStrategy(const UObject& Asset) const override
	{
		return EAssetDocumentOpenEditorStrategy::RefreshInPlace;
	}
	virtual bool IsShownInOpenEditor(const UObject& Asset) const override
	{
		const UMovieSceneSequence* Sequence = Cast<UMovieSceneSequence>(&Asset);
		return Sequence != nullptr && !SequencersShowing(*Sequence).IsEmpty();
	}
	virtual void RefreshOpenEditor(UObject& Asset, bool bSucceeded) const override
	{
		const UMovieSceneSequence* Sequence = Cast<UMovieSceneSequence>(&Asset);
		if (Sequence == nullptr)
		{
			return;
		}
		for (const TSharedPtr<ISequencer>& Sequencer : SequencersShowing(*Sequence))
		{
			Sequencer->NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::Unknown);
			Sequencer->ForceEvaluate();
		}
	}
};

FDelegateHandle& CreatedHandle()
{
	static FDelegateHandle Handle;
	return Handle;
}

TSharedPtr<IAssetDocumentOpenEditorHandler>& RegisteredHandler()
{
	static TSharedPtr<IAssetDocumentOpenEditorHandler> Registered;
	return Registered;
}
} // namespace AssetDocumentSequencerOpenEditorLocal

namespace UE::AssetDocument::Graph::Sequencer
{
void RegisterOpenEditorHandler()
{
	using namespace AssetDocumentSequencerOpenEditorLocal;
	if (RegisteredHandler().IsValid())
	{
		return;
	}
	if (ISequencerModule* Module = FModuleManager::GetModulePtr<ISequencerModule>(TEXT("Sequencer")))
	{
		CreatedHandle() = Module->RegisterOnSequencerCreated(FOnSequencerCreated::FDelegate::CreateLambda([](TSharedRef<ISequencer> Created)
		{
			LiveSequencers().RemoveAll([](const TWeakPtr<ISequencer>& Weak) { return !Weak.IsValid(); });
			LiveSequencers().Add(Created);
		}));
	}
	RegisteredHandler() = MakeShared<FSequenceHandler>();
	RegisterAssetDocumentOpenEditorHandler(RegisteredHandler().ToSharedRef());
}

void UnregisterOpenEditorHandler()
{
	using namespace AssetDocumentSequencerOpenEditorLocal;
	if (RegisteredHandler().IsValid())
	{
		UnregisterAssetDocumentOpenEditorHandler(RegisteredHandler()->GetId());
		RegisteredHandler().Reset();
	}
	if (CreatedHandle().IsValid())
	{
		if (ISequencerModule* Module = FModuleManager::GetModulePtr<ISequencerModule>(TEXT("Sequencer")))
		{
			Module->UnregisterOnSequencerCreated(CreatedHandle());
		}
		CreatedHandle().Reset();
	}
	LiveSequencers().Reset();
}
} // namespace UE::AssetDocument::Graph::Sequencer
