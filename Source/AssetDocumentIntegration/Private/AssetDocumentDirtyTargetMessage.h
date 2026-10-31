// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Misc/PackageName.h"
#include "Runtime/AssetDocumentOpenEditor.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

// The message of DirtyExistingAssetTransactionUnsupported (every place that refuses an apply because the existing
// target's package has unsaved changes): what happened, the first change that made it dirty when known, and the two
// ways out, in the MCP tools' words.
inline FString DescribeAssetDocumentDirtyTarget(const FString& TargetPath)
{
	const UPackage* Package = TargetPath.IsEmpty() ? nullptr
		: FindPackage(nullptr, *FPackageName::ObjectPathToPackageName(TargetPath));
	const FString FirstChange = Package != nullptr ? UE::AssetDocument::OpenEditor::DescribeFirstUnsavedChange(*Package) : FString();
	return FString::Printf(
		TEXT("%s has unsaved changes made outside this document (in an editor, by a script, or by an earlier apply that "
			"dirtied it), and an apply never overwrites unsaved work.%s Either keep them: save the asset (save_assets), "
			"extract it again and apply a document based on that extract; or discard them: reload the asset from disk "
			"(reload_assets with force: true), then apply."),
		TargetPath.IsEmpty() ? TEXT("The target asset") : *TargetPath,
		FirstChange.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" The first unsaved change: %s."), *FirstChange));
}
