// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Rollback rebuilder of the Level Sequence profile (design §4.3): Core restores the sequence package from its snapshot
// (the movie scene, tracks, sections, bindings and custom bindings are all authored objects of the package); what the
// engine derives from them lives outside the package and is refreshed here, as Undo does
// (UMovieScene::PostEditUndo -> MarkAsChanged): the movie scene's signature changes, so evaluation templates and
// open players recompile, and the precompiled data of the sequence is dropped.

#include "AssetDocumentSequencerPrimitives.h"

#include "Compilation/MovieSceneCompiledDataManager.h"
#include "MovieScene.h"
#include "MovieSceneSequence.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"

namespace UE::AssetDocument::Graph::Sequencer
{
namespace
{
class FSequenceRebuilder final : public IAssetDocumentDerivedStateRebuilder
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentSequencer.Sequence"); }
	virtual bool Supports(const UObject& PrimaryAsset) const override { return PrimaryAsset.IsA<UMovieSceneSequence>(); }

	virtual FAssetDocumentContractValidationResult Rebuild(UObject& PrimaryAsset) override
	{
		FAssetDocumentContractValidationResult Result;
		Result.bValid = true;
		UMovieSceneSequence& Sequence = *CastChecked<UMovieSceneSequence>(&PrimaryAsset);
		if (UMovieScene* MovieScene = Sequence.GetMovieScene())
		{
			MovieScene->MarkAsChanged();
		}
		UMovieSceneCompiledDataManager::GetPrecompiledData()->Reset(&Sequence);
		return Result;
	}
};
} // namespace

TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeRebuilder()
{
	return MakeShared<FSequenceRebuilder>();
}
} // namespace UE::AssetDocument::Graph::Sequencer
