// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

// Whether a path lies inside a directory, judged by what the filesystem opens rather than by the path text:
// symbolic links and junctions are followed, and the case of every existing component is the one stored on
// the volume (a case-insensitive volume yields its canonical case; a case-sensitive one keeps names distinct).
namespace AssetDocumentPaths
{
enum class EPathLookup : uint8
{
	// The entry exists; the resolver set its real path.
	Resolved,
	// Nothing exists at the path.
	Missing,
	// An entry exists but cannot be followed (a dangling link, or one the process may not read).
	Unresolvable,
};

// Resolves one absolute path the way the filesystem does: links followed, case as stored.
using FResolveExisting = TFunction<EPathLookup(const FString& AbsolutePath, FString& OutRealPath)>;

// The real path of Path (relative paths are made absolute first): its deepest existing ancestor resolved by
// the filesystem, followed by the part that does not exist yet. Empty when Path is empty, keeps a '..' that
// climbs above the volume root, or crosses an entry that cannot be followed. Uses '/' separators.
ASSETDOCUMENT_API FString ResolveRealPath(const FString& Path);

// True when Candidate is Root or lies inside it once both are resolved with ResolveRealPath: the resolved
// candidate equals the resolved root or starts with it followed by a separator, compared case-sensitively.
// A link inside Root that leads out of it is outside.
ASSETDOCUMENT_API bool IsWithin(const FString& Root, const FString& Candidate);

// IsWithin, also giving Candidate's resolved path relative to Root's ("" for Root itself).
ASSETDOCUMENT_API bool MakeRelativeWithin(const FString& Root, const FString& Candidate, FString& OutRelative);

// The check every read or write of a path taken from a tool argument or a document makes before its first access:
// true when the file the access reaches lies inside Root, with OutRealPath set to that file's real path
// (ResolveRealPath: the leaf itself is followed when it is a link, so a link inside Root to a file outside is
// refused). False, with OutRealPath empty, otherwise; a dangling leaf link is refused too. Reading or writing
// OutRealPath, not Path, keeps the access on the file that was checked.
ASSETDOCUMENT_API bool ResolveForAccess(const FString& Root, const FString& Path, FString& OutRealPath);

// Replaces the filesystem resolver while it lives (tests use it to stand for a volume with other semantics).
class ASSETDOCUMENT_API FScopedResolverOverride
{
public:
	explicit FScopedResolverOverride(FResolveExisting Resolver);
	~FScopedResolverOverride();

	FScopedResolverOverride(const FScopedResolverOverride&) = delete;
	FScopedResolverOverride& operator=(const FScopedResolverOverride&) = delete;

private:
	FResolveExisting Previous;
};
}
