// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Runtime/AssetDocumentMutationPreparation.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"

// Native prepared participant for one fixed UAnimSequence FName. No property
// selector, transport token or raw object setter is exposed by this interface.
class ASSETDOCUMENTEDITOR_API IAssetDocumentEditorSequenceRetargetSourceNameMutation
{
public:
 virtual ~IAssetDocumentEditorSequenceRetargetSourceNameMutation() = default;
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
 virtual FName GetOriginalName() const = 0;
 virtual FName GetDesiredName() const = 0;
};

ASSETDOCUMENTEDITOR_API FAssetDocumentContractValidationResult PrepareAssetDocumentEditorSequenceRetargetSourceNameMutation(
 const FAssetDocumentRuntimeRequestDto& Request, const FAssetDocumentMutationIntent& Intent,
 TSharedPtr<IAssetDocumentEditorSequenceRetargetSourceNameMutation>& OutMutation);
