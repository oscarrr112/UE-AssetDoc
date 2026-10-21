// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

class UPackage;

// A durable apply loads the package it staged (Saved/AssetDocument/Runtime/..., mounted under
// /Temp/AssetDocument/Runtime/) to verify it. That copy is request-private: the request releases it
// when it cleans its staging (ReleaseLoadedStagedPackage) and the outermost request scope purges it
// with a garbage collection when it ends, so no provider and no editor-wide iterator
// (TObjectIterator, GetObjectsOfClass, the Blueprint action database, list_types) sees a staged
// copy after the request. The purge waits for the outermost scope because request code above the
// persistence step may still hold unrooted objects. A copy released outside every scope is purged
// on the next core tick.
namespace UE::AssetDocument::StagedPackages
{
// The package-name root of every copy a request loads from its staging directories.
ASSETDOCUMENT_API bool IsStagedCopy(const UPackage& Package);

// Records a released staged copy for the purge.
ASSETDOCUMENT_API void NoteReleased(const UPackage& Package);

// Collects garbage if a released staged copy is pending; returns how many released copies are
// still loaded afterwards (something outside the request still references them; logged).
ASSETDOCUMENT_API int32 PurgeReleased();

// An AssetDocument request (apply). Nested scopes purge once, when the outermost one ends.
class ASSETDOCUMENT_API FScopedRequest
{
public:
	FScopedRequest();
	~FScopedRequest();
	FScopedRequest(const FScopedRequest&) = delete;
	FScopedRequest& operator=(const FScopedRequest&) = delete;
};
}
