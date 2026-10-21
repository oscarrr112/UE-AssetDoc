// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "LevelToolResult.h"

// edit_level (POST /assetdoc/level/edit): the request lifecycle of
// docs/architecture/level-editing-tools-design.md §3.1: admission, parse, preflight
// (dry_run stops here), pre-image digest, one editor transaction for the batch, apply,
// verify, commit or rollback (Undo only when the newest entry is ours, then a digest
// check), post-commit helpers, optional save. Game thread only.
namespace UE::AssetDocumentLevel::Private
{
ASSETDOCUMENTLEVEL_API FLevelToolResult RunLevelEdit(const TSharedPtr<FJsonObject>& Request);

// Parses RequestBody, then RunLevelEdit.
FLevelToolResult RunLevelEditFromBody(const FString& RequestBody);

// Test hook (Transaction.* tests): while alive, every batch fails with
// Level.TestInjectedFailure right after op AfterOp has been applied (-1: before the
// first op), as a real failure at that point would.
class FScopedLevelEditFailureInjection
{
public:
	explicit FScopedLevelEditFailureInjection(int32 AfterOp);
	~FScopedLevelEditFailureInjection();

	FScopedLevelEditFailureInjection(const FScopedLevelEditFailureInjection&) = delete;
	FScopedLevelEditFailureInjection& operator=(const FScopedLevelEditFailureInjection&) = delete;

private:
	TOptional<int32> Previous;
};
}
