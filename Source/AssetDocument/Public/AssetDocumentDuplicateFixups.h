// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "UObject/TopLevelAssetPath.h"

struct FObjectDuplicationParameters;

// Fixups of an in-memory duplicate (StaticDuplicateObjectEx) that run after its objects are copied and before any of
// them post-loads. A provider registers one for an asset class whose PostLoad expects subobject state that a duplicate
// does not give it (e.g. a subobject the class post-loads itself only when it is flagged for PostLoad). The fixup of
// the nearest registered class of the copy runs. Registration is game-thread only, by class path (a module's
// ShutdownModule runs after the UObject exit purge on a normal editor exit, so it never touches the class); returns
// false for an empty path, a null fixup or a duplicate, and Unregister false when the path was not registered.
// CreatedObjects maps each source object to its copy, in the order the duplicate post-loads them.
using FAssetDocumentDuplicateFixup = void (*)(UObject& Copy, const TMap<UObject*, UObject*>& CreatedObjects);

ASSETDOCUMENT_API bool RegisterAssetDocumentDuplicateFixup(const FTopLevelAssetPath& ClassPath, FAssetDocumentDuplicateFixup Fixup);
ASSETDOCUMENT_API bool UnregisterAssetDocumentDuplicateFixup(const FTopLevelAssetPath& ClassPath);
ASSETDOCUMENT_API bool HasAssetDocumentDuplicateFixup(const UClass& Class);

// StaticDuplicateObjectEx(Parameters) with the fixup of the source's class run between the copy and its post-loads,
// which then run as StaticDuplicateObjectEx runs them (each created object that is not a template, in order). Without a
// fixup for the class, or when the caller asked for bSkipPostLoad, it is StaticDuplicateObjectEx.
ASSETDOCUMENT_API UObject* DuplicateObjectWithAssetDocumentFixups(FObjectDuplicationParameters& Parameters);
