// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Templates/Function.h"
#include "UObject/WeakObjectPtrTemplates.h"

class AActor;
class UWorld;

// The actor reference shared by the level tools and the Level Sequence documents
// (docs/architecture/level-editing-tools-design.md §2.1, §6; decision D13).
//
// Canonical form: the actor's soft object path in the editor world, e.g.
// "/Game/Maps/Room.Room:PersistentLevel.PointLight_0" (OFPA actors keep the map's
// namespace: "...:PersistentLevel.StaticMeshActor_UAID_..."; sublevel actors use the
// sublevel's package). It is exactly the string Sequencer persists in
// FActorLocatorFragment::Path for a possessable binding (FEditorPathHelper::GetEditorPath).
// guid (AActor::GetActorGuid) and label are lookup helpers only.
//
// Input forms: "path" | "@id" | {"path"} | {"guid"} | {"label", "level"?} | {"id"}.
// Output: {"path", "guid", "label", "class", "level", "external_package", "loaded"}.
namespace UE::AssetDocumentLevel
{
enum class EActorRefKind : uint8
{
	Path,
	Guid,
	Label,
	BatchId,
};

struct FActorRef
{
	EActorRefKind Kind = EActorRefKind::Path;
	// The path, the guid text, the label, or the batch id without '@'.
	FString Value;
	// Label lookups only: restrict to this level package (e.g. "/Game/Maps/Room").
	FString Level;
};

// The canonical reference of a live actor.
ASSETDOCUMENTLEVEL_API FString GetActorPath(const AActor& Actor);

// The guid as the tools print it (32 hex digits).
ASSETDOCUMENTLEVEL_API FString FormatActorGuid(const FGuid& Guid);

// Parses one ActorRef input. False with a reason for any other shape.
ASSETDOCUMENTLEVEL_API bool ParseActorRef(const TSharedPtr<FJsonValue>& Json, FActorRef& OutRef, FString& OutError);

struct FActorRefResolution
{
	bool bResolved = false;
	// The loaded actor (null for an unloaded World Partition actor).
	TWeakObjectPtr<AActor> Actor;
	// Canonical path and guid (also for unloaded actors).
	FString Path;
	FGuid Guid;
	bool bLoaded = false;
	// Failure: Level.ActorNotFound, Level.ActorRefAmbiguous, Level.UnknownId, Level.InvalidRequest.
	FString Code;
	FString Message;
	// Level.ActorRefAmbiguous: the paths of every match; Level.ActorNotFound: the paths of the closest actors
	// (labels or object names close to the reference; the message names them too).
	TArray<FString> Candidates;
};

// Resolves Ref in World (persistent level, loaded sublevels and, for a partitioned
// world, unloaded actors through their World Partition descriptors). BatchId refs go
// to ResolveBatchId; without it they fail with Level.InvalidRequest.
ASSETDOCUMENTLEVEL_API FActorRefResolution ResolveActorRef(UWorld& World, const FActorRef& Ref,
	const TFunction<AActor*(FStringView Id)>& ResolveBatchId = nullptr);

// The ActorRef output object of a loaded actor.
ASSETDOCUMENTLEVEL_API TSharedRef<FJsonObject> MakeActorRefJson(const AActor& Actor);
}
