// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "AssetDocumentEditorCutoverTransactionPreparation.h"
#include "AssetDocumentRollbackAction.h"

// bPackageSnapshotRestores: Core's package snapshot of the existing target
// restores it last (bytes + PostEditUndo, like editor Undo), so replaying the
// typed property preimage first is redundant and can fail where PostEditChange
// rebuilds state (e.g. a restored subobject reference is replaced again). The
// actions then succeed without replay; the snapshot owns the footprint.
TArray<TSharedRef<IAssetDocumentRollbackAction>>
AdaptAssetDocumentEditorRollbackSeeds(
	TConstArrayView<
		TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>> Seeds,
	bool bPackageSnapshotRestores = false);

bool IsAssetDocumentGenericRuntimeRollbackEligibleFailure(
	FStringView StableFailureCode);
