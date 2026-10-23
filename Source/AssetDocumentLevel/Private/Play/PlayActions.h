// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "LevelToolResult.h"

class UWorld;

// play "call" and "set": what a tester's cheat or debug command does to the running game, without
// the console's string arguments. Both act on the play world only (the editor world is never
// touched) and run on the game thread at once, also while the game is paused.
namespace UE::AssetDocumentLevel::Private::Play
{
// {"actor", "function", "args"?}: calls a Blueprint function, custom event or UFUNCTION on a play-world
// actor ("Function") or on one of its components ("Component.Function"), with arguments in the
// AssetDocument value forms (structs such as {"X","Y","Z"}, enums by name, play-world actors by name
// or label). Payload: {target, function, return?, outputs?, defaulted?}.
FLevelToolResult RunPlayCall(UWorld& World, const FJsonObject& Request);

// {"actor", "values"?: {path: value}, "location"?, "rotation"?}: writes reflected values on a
// play-world actor or its components ("Health", "BPC_Combat.bAutoAttack",
// "CharacterMovement.Velocity") as a Blueprint Set node does, and teleports it. All values are
// checked before any is written. Payload: {changes: [{path, before, after}], actor}.
FLevelToolResult RunPlaySet(UWorld& World, const FJsonObject& Request);
}
