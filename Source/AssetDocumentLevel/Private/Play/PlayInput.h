// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "LevelToolResult.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UEnhancedInputLocalPlayerSubsystem;
class UEnhancedPlayerInput;
class UInputAction;
class UWorld;

// The player's live Enhanced Input state, as the running game has it: the mappings after context
// priorities and the player's key settings (EnhancedInputUserSettings), not the assets' mappings.
// Used by play input reports, query_play what "input" and play reset_input_settings. Game thread only.
namespace UE::AssetDocumentLevel::Private::Play
{
// One live mapping an injected key drives: the mapping's own key, or a paired 2D axis one half of
// which is the key (MouseX drives a Mouse2D mapping; Mouse2D drives MouseX / MouseY mappings).
struct FLiveKeyReach
{
	TWeakObjectPtr<const UInputAction> Action;
	FKey MappingKey;
};

// True when an injected Key reaches a mapping of MappingKey (the same key, or the paired-axis relation above).
bool InjectedKeyReachesMapping(const FKey& Key, const FKey& MappingKey);

// The player's live mappings (UEnhancedPlayerInput's rebuilt list) that Key reaches, one per action.
TArray<FLiveKeyReach> FindLiveMappingsForKey(const UEnhancedPlayerInput& PlayerInput, const FKey& Key);

// When no live mapping reaches Key: the active contexts' mappings of it that neither the player's key settings
// nor a higher-priority context explain, i.e. mappings of a context added since the last rebuild (the subsystem
// rebuilds its live list a frame later). Each becomes live with that rebuild.
TArray<FLiveKeyReach> FindPendingMappingsForKey(UEnhancedInputLocalPlayerSubsystem& Subsystem, const FKey& Key);

// When no live mapping reaches Key: what the active contexts' assets map it to and why that mapping is not
// live (the player's key settings moved it, a higher-priority context takes the key), or empty.
FString DescribeUnreachedKey(UEnhancedInputLocalPlayerSubsystem& Subsystem, const FKey& Key);

// The mappings the player's key settings change from the assets' keys ("Jump: SpaceBar -> G").
TArray<FString> DescribeCustomizedPlayerKeys(UEnhancedInputLocalPlayerSubsystem& Subsystem);

// query_play {"what": "input"}: the active contexts with priorities, each mapping's effective key, the saved
// user settings.
FLevelToolResult QueryPlayInput(UWorld& World);

// play {"action": "reset_input_settings"}: the player's key settings back to the assets' keys (when a session
// runs) and the saved settings file deleted, so later sessions use the assets' mappings.
FLevelToolResult ResetPlayInputSettings(UWorld* World);
}
