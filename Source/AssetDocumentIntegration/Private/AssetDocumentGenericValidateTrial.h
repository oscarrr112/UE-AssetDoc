// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "AssetDocumentPythonRuntime.h"

// Validate's trial apply. Profiles whose Preflight is covered by Core's package snapshot
// (SnapshotCoveredMutationV1: Material, StateTree, BlendSpace, DataTable, user types, ...) make
// their checks while they edit (pins, expression classes, states, transitions, bindings, sample
// animations, ...). Validate runs that same edit on a transient copy of the target (or on a new
// transient asset when Apply would create it) and discards the copy, so it reports the
// diagnostics Apply reports, without changing the asset, other packages, the undo history or
// the disk:
//   - the Python dispatch is the Validate operation in the Apply phase; the registered runtime
//     resolves the target read-only and asks editor.scratch.trial_target.v1 for the copy, then
//     runs the Profile's apply() on it;
//   - the transaction is a request-local trial transaction: it scopes the mutating primitives
//     (their token checks) and accepts their footprints, but nothing is committed or snapshotted;
//     rollback actions a primitive enlists run when the trial ends;
//   - FAssetDocumentEditorTrialScope keeps the undo history and every other package unchanged.
// Compile errors the edit reports are included (Apply's compile runs on the copy). Factory,
// save and fresh-load verification failures still surface only in Apply.
FAssetDocumentRuntimeResultDto RunAssetDocumentValidateTrial(
	IAssetDocumentPythonRuntime& Runtime,
	const FAssetDocumentRuntimeRequestDto& Request);
