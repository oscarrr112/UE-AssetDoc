// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Runtime/AssetDocumentMutationPreparation.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"

// Prepared authority for the fixed five-field compression tuple. The verified
// Editor identities remain callback-bound across the supplied completion body.
class ASSETDOCUMENTEDITOR_API IAssetDocumentEditorSequenceCompressionMutation
{
public:
 virtual ~IAssetDocumentEditorSequenceCompressionMutation() = default;
 virtual FAssetDocumentContractValidationResult OpenCapture(const FGuid& Transaction) = 0;
 virtual void CloseCapture() = 0;
 virtual FAssetDocumentContractValidationResult ValidateInvocation(
  const FAssetDocumentPrimitiveInvocation&, IAssetDocumentHandleResolver&) const = 0;
 virtual FAssetDocumentContractValidationResult WithVerifiedAssets(
  const FAssetDocumentPrimitiveInvocation&, IAssetDocumentHandleResolver&,
  TFunctionRef<FAssetDocumentContractValidationResult()> Body) const = 0;
 virtual FAssetDocumentContractValidationResult Apply(
  const FAssetDocumentPrimitiveInvocation&, IAssetDocumentHandleResolver&, IAssetDocumentAtomicTransaction&) = 0;
 virtual FAssetDocumentContractValidationResult Restore() = 0;
 virtual bool IsOriginalCurrent() const = 0;
 virtual bool IsDesiredCurrent() const = 0;
 virtual bool HasChanges() const = 0;
 virtual bool DidWrite() const = 0;
};

ASSETDOCUMENTEDITOR_API FAssetDocumentContractValidationResult PrepareAssetDocumentEditorSequenceCompressionMutation(
 const FAssetDocumentRuntimeRequestDto& Request, const FAssetDocumentMutationIntent& Intent,
 TSharedPtr<IAssetDocumentEditorSequenceCompressionMutation>& OutMutation);
