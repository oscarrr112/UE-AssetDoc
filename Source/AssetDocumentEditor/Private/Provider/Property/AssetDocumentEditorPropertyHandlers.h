// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Provider/AssetDocumentEditorPrimitiveHandler.h"

class IAssetDocumentVerifiedHandleLease;
class UObject;
struct FAssetDocumentEditorPropertyBatchTokenView;

bool TryAcquireAssetDocumentEditorPropertyBatchTarget(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const FAssetDocumentOpaqueHandle& AssetHandle,
	IAssetDocumentHandleResolver& Handles,
	TUniquePtr<IAssetDocumentVerifiedHandleLease>& OutLease,
	UObject*& OutExactTarget,
	FAssetDocumentPrimitiveOutcome& OutFailure);

bool TryAcquireAssetDocumentEditorPropertyBatchWrite(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	TUniquePtr<IAssetDocumentVerifiedHandleLease>& OutAssetLease,
	TUniquePtr<IAssetDocumentVerifiedHandleLease>& OutBatchLease,
	UObject*& OutExactTarget,
	FAssetDocumentEditorPropertyBatchTokenView& OutBatchView,
	FAssetDocumentPrimitiveOutcome& OutFailure);

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorPropertyEnumerateAdmitted(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorPropertyReadAdmitted(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorPropertyWriteAdmitted(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorPropertyPreflightAdmitted(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction);

TConstArrayView<FAssetDocumentEditorPrimitiveBinding>
GetAssetDocumentEditorPropertyPrimitiveBindings();
