// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

// The message of DirtyExistingAssetTransactionUnsupported (every place that refuses an apply because the existing
// target's package has unsaved changes): what happened and the two ways out, in the MCP tools' words.
inline FString DescribeAssetDocumentDirtyTarget(const FString& TargetPath)
{
	return FString::Printf(
		TEXT("%s has unsaved changes made outside this document (in an editor, by a script, or by an earlier apply that "
			"dirtied it), and an apply never overwrites unsaved work. Either keep them: save the asset (save_assets), "
			"extract it again and apply a document based on that extract; or discard them: reload the asset from disk "
			"(reload_assets with force: true), then apply."),
		TargetPath.IsEmpty() ? TEXT("The target asset") : *TargetPath);
}
