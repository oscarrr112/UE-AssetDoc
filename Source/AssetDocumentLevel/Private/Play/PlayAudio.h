// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "ActiveSoundUpdateInterface.h"
#include "Dom/JsonObject.h"
#include "LevelToolResult.h"
#include "Subsystems/AudioEngineSubsystem.h"

#include "PlayAudio.generated.h"

class UWorld;

// Records every sound the audio engine starts (the editor's and each Play In Editor audio device), for query_play
// what "audio": a gameplay sound has no other trace (Play Sound at Location makes no component and logs nothing).
// The audio engine creates one instance per audio device; the notifications arrive on the audio thread and go to
// one bounded, locked record shared by all devices.
UCLASS()
class UAssetDocumentPlayAudioRecorder : public UAudioEngineSubsystem, public IActiveSoundUpdateInterface
{
	GENERATED_BODY()

public:
	// IActiveSoundUpdateInterface (audio thread)
	virtual void NotifyActiveSoundCreated(FActiveSound& ActiveSound) override;
	virtual void NotifyActiveSoundDeleting(const FActiveSound& ActiveSound) override;
	virtual void NotifyVirtualizedSoundCreated(FActiveSound& ActiveSound) override;
	virtual void NotifyVirtualizedSoundDeleting(const FActiveSound& ActiveSound) override;
};

namespace UE::AssetDocumentLevel::Private::Play
{
// query_play what "audio" (game thread): the playing audio components of World, and the sounds started in World
// since a sequence number (the log's numbering: "since" takes an earlier result's last_seq). Request fields:
// since, name, near, limit. Fields and result: MCP/schemas/Playtest.md.
FLevelToolResult QueryAudio(UWorld& World, const FJsonObject& Json);
}
