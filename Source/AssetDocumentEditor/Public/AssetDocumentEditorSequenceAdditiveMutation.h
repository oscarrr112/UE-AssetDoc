// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Runtime/AssetDocumentMutationPreparation.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"

// Exact native prepared participant. No object/property selector, native pointer
// setter or transported token is exposed. All live writes consume verified leases.
class ASSETDOCUMENTEDITOR_API IAssetDocumentEditorSequenceAdditiveMutation
{
public:
 virtual ~IAssetDocumentEditorSequenceAdditiveMutation() = default;
 virtual FAssetDocumentContractValidationResult OpenCapture(const FGuid& Transaction) = 0;
 virtual void CloseCapture() = 0;
 virtual FAssetDocumentContractValidationResult ValidateInvocation(
  const FAssetDocumentPrimitiveInvocation&, IAssetDocumentHandleResolver&) const = 0;
 virtual FAssetDocumentContractValidationResult Apply(
  const FAssetDocumentPrimitiveInvocation&, IAssetDocumentHandleResolver&, IAssetDocumentAtomicTransaction&) = 0;
 virtual FAssetDocumentContractValidationResult Restore() = 0;
 virtual bool IsOriginalCurrent() const = 0;
 virtual bool IsDesiredCurrent() const = 0;
 virtual bool HasChanges() const = 0;
 virtual bool DidWrite() const = 0;
};

ASSETDOCUMENTEDITOR_API FAssetDocumentContractValidationResult PrepareAssetDocumentEditorSequenceAdditiveMutation(
 const FAssetDocumentRuntimeRequestDto& Request, const FAssetDocumentMutationIntent& Intent,
 TSharedPtr<IAssetDocumentEditorSequenceAdditiveMutation>& OutMutation);
