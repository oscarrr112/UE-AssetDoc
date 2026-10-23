// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "LevelToolResult.h"
#include "UObject/WeakObjectPtrTemplates.h"

class AActor;
class APlayerController;
class FProperty;
class UActorComponent;
class UWorld;

// query_play (POST /assetdoc/play/query): read-only view of the running Play In Editor world,
// as query_level is of the editor world. Nothing here changes the world. Request and response:
// MCP/schemas/Playtest.md. Game thread only.
namespace UE::AssetDocumentLevel::Private::Play
{
// The PIE (or Simulate) world, or null.
UWorld* GetPlayWorld();

// The first local player controller of World, or null (Simulate has none).
APlayerController* GetLocalPlayerController(UWorld& World);

struct FPlayActorResolution
{
	AActor* Actor = nullptr;
	// Play.ActorNotFound, Play.ActorRefAmbiguous, Play.InvalidRequest.
	FString Code;
	FString Message;
	TArray<FString> Candidates;
};

// A runtime actor reference: "player" (the player's pawn), "controller", "game_mode", "game_state",
// "player_state", "hud", "camera"; an actor's object name, label, or path (the PIE path, or the
// editor path query_level returned, which maps to the actor's play-world copy); or {"name"},
// {"label"}, {"path"}.
FPlayActorResolution ResolvePlayActor(UWorld& World, const TSharedPtr<FJsonValue>& Ref);

// A component of Actor by object name (case-insensitive), or null.
UActorComponent* FindPlayComponent(AActor& Actor, const FString& Name);

// One reflected value in the AssetDocument value forms; objects of the play world are their names
// (an actor: its object name; a component: "Owner.Component"), other objects their paths.
TSharedPtr<FJsonValue> EncodePlayValue(FProperty& Property, const void* ValuePtr);

// GWorld is the play world for the scope (as while the editor ticks it), so game code that reads
// GWorld or the PIE instance id behaves as in the running game.
class FPlayWorldScope
{
public:
	explicit FPlayWorldScope(UWorld& World);
	~FPlayWorldScope();
	FPlayWorldScope(const FPlayWorldScope&) = delete;
	FPlayWorldScope& operator=(const FPlayWorldScope&) = delete;

private:
	UWorld* Previous = nullptr;
	bool bSwitched = false;
};

// Reads a reflected value by path from Actor: "Health" (the actor's property), "BPC_Combat.Health"
// (a component by name, then its property), "CharacterMovement.Velocity.Z" (struct members),
// "Controller.PlayerState" (object properties are followed). Values use the AssetDocument value
// forms; actor and component references in the play world are their names.
bool ReadActorValue(AActor& Actor, const FString& Path, TSharedPtr<FJsonValue>& OutValue, FString& OutError);

// The compact runtime view of one actor: name, label, class, path (the play-world object path), location,
// rotation, velocity, controller / pawn, health-like values (vitals), the active montage, AI state
// (behavior tree, active task, blackboard values, move status).
struct FPlayActorJsonOptions
{
	// detail "components" / "properties" ("full"): the component list / and the changed properties.
	bool bComponents = false;
	bool bProperties = false;
	bool bDiffOnly = true;
	TArray<FString> Properties;
	TOptional<FVector> DistanceFrom;
};
TSharedRef<FJsonObject> PlayActorJson(AActor& Actor, const FPlayActorJsonOptions& Options);

// A condition the play tool's wait (and input) can stop on.
struct FPlayCondition
{
	enum class EKind : uint8
	{
		Value,
		Exists,
		Count,
		Log,
		SessionEnded,
	};
	EKind Kind = EKind::Value;
	TSharedPtr<FJsonValue> ActorRef;
	TWeakObjectPtr<AActor> ExistsActor;
	bool bExists = true;
	FString Property;
	FString Op = TEXT("==");
	TSharedPtr<FJsonValue> Value;
	// Count: the actor filter.
	TSharedPtr<FJsonObject> Filter;
	// Log: substring and optional category.
	FString LogText;
	FString LogCategory;
	FString Text;
};

// Parses one condition (Path: its JSON Pointer). Actor references are checked against World.
bool ParseCondition(UWorld* World, const TSharedPtr<FJsonValue>& Json, const FString& Path, FPlayCondition& Out,
	TArray<FAssetDocumentDiagnostic>& OutDiagnostics);

// True when the condition holds; OutObserved: the current value (or count, or the matched log line).
bool EvaluateCondition(UWorld* World, const FPlayCondition& Condition, uint64 LogSince, TSharedPtr<FJsonValue>& OutObserved);

// Diagnostics with Play.* codes.
FAssetDocumentDiagnostic MakePlayDiagnostic(const FString& Path, const FString& Code, const FString& Message,
	TSharedPtr<FJsonObject> Details = nullptr);

// Play.InvalidRequest at Path.
FAssetDocumentDiagnostic PlayInvalidRequest(const FString& Path, const FString& Message);

TArray<TSharedPtr<FJsonValue>> PlayStringArray(const TArray<FString>& Values);

// {"X","Y","Z"} rounded to centimetre hundredths.
TSharedRef<FJsonObject> PlayVectorJson(const FVector& Vector);

FLevelToolResult RunPlayQuery(const TSharedPtr<FJsonObject>& Request);
FLevelToolResult RunPlayQueryFromBody(const FString& RequestBody);

// Parses a request body (empty = {}); false with a diagnostic otherwise.
bool ParsePlayBody(const FString& RequestBody, TSharedPtr<FJsonObject>& OutJson, FLevelToolResult& OutFailure);
}
