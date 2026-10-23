// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Provider/AssetDocumentEditorPrimitiveHandler.h"

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorScratchCloneAsset(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

FAssetDocumentPrimitiveOutcome
HandleAssetDocumentEditorScratchCreateFromClass(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorResolveExistingAsset(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorCreateAssetByFactory(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorCreateAssetByGenericFactory(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorScratchCreateByFactory(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorScratchCloneAssetV2(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorScratchTrialTarget(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorRollbackCreatedAsset(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorBlueprintScratchClone(const FAssetDocumentPrimitiveInvocation&, IAssetDocumentHandleResolver&, IAssetDocumentAtomicTransaction*);
FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorBlueprintScratchCreate(const FAssetDocumentPrimitiveInvocation&, IAssetDocumentHandleResolver&, IAssetDocumentAtomicTransaction*);
FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorBlueprintAssetCreate(const FAssetDocumentPrimitiveInvocation&, IAssetDocumentHandleResolver&, IAssetDocumentAtomicTransaction*);
inline TConstArrayView<FAssetDocumentEditorPrimitiveBinding>
GetAssetDocumentEditorLifecyclePrimitiveBindings()
{
	static const FAssetDocumentEditorPrimitiveBinding Bindings[] = {
 {TEXT("editor.blueprint.scratch.clone.v1"),1u,&HandleAssetDocumentEditorBlueprintScratchClone},
 {TEXT("editor.blueprint.scratch.create.v1"),1u,&HandleAssetDocumentEditorBlueprintScratchCreate},
 {TEXT("editor.blueprint.asset.create.v1"),1u,&HandleAssetDocumentEditorBlueprintAssetCreate},
		{TEXT("editor.scratch.clone_asset.v1"), 1u,
			&HandleAssetDocumentEditorScratchCloneAsset},
		{TEXT("editor.scratch.create_from_class.v1"), 1u,
			&HandleAssetDocumentEditorScratchCreateFromClass},
		{TEXT("editor.asset.resolve_existing.v1"), 1u,
			&HandleAssetDocumentEditorResolveExistingAsset},
		{TEXT("editor.asset.create_by_factory.v1"), 1u,
			&HandleAssetDocumentEditorCreateAssetByFactory},
		{TEXT("editor.asset.rollback_created.v1"), 1u,
			&HandleAssetDocumentEditorRollbackCreatedAsset},
		{TEXT("editor.asset.create_by_factory.v2"), 2u,
			&HandleAssetDocumentEditorCreateAssetByGenericFactory},
		{TEXT("editor.scratch.create_by_factory.v1"), 1u,
			&HandleAssetDocumentEditorScratchCreateByFactory},
		{TEXT("editor.scratch.clone_asset.v2"), 2u,
			&HandleAssetDocumentEditorScratchCloneAssetV2},
		{TEXT("editor.scratch.trial_target.v1"), 1u,
			&HandleAssetDocumentEditorScratchTrialTarget},
	};
	return MakeArrayView(Bindings);
}
